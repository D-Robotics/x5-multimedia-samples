#include "lvgl_display.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include "lvgl.h"

/* Partial refresh lines — full 720x1280x4 ≈ 3.5MB, too large for embedded heap */
#define DRAW_BUF_LINES  80
#define PIXEL_SIZE      4 /* LV_COLOR_FORMAT_ARGB8888 */

static drm_ui_plane_t *g_ui_plane;
static lv_display_t *g_disp;
static uint8_t *g_buf1;
static volatile int g_ui_dirty;

/*
 * Polaroid edges painted after LVGL memcpy. Widths are visible cream from FB
 * edges (UI_FRAME_*). Panel clip / touch use UI_OVERSCAN_* separately — do
 * not bake overscan into these widths (that made the right matte look ~3×).
 */
static int g_frame_left;
static int g_frame_right;
static int g_frame_top;
static int g_frame_bottom;
static uint32_t g_frame_pixel; /* LE ARGB8888: B,G,R,A */
static uint32_t g_frame_rgb24; /* last 0xRRGGBB for resume */
static int g_saved_left;
static int g_saved_right;
static int g_saved_top;
static int g_saved_bottom;
static uint32_t g_saved_rgb24;
static int g_polaroid_suspended;

static uint32_t custom_tick_get(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000U + ts.tv_nsec / 1000000U);
}

static int clamp_frame_px(int v, const char *which)
{
	if (v < 0 || v > 160) {
		if (v != 0)
			fprintf(stderr,
				"[ui] polaroid reject bad %s=%d (keep previous)\n",
				which, v);
		return -1;
	}
	return v;
}

static void paint_polaroid_into(uint8_t *base, uint32_t pitch, int w, int h,
		int left, int right, int top, int bottom, uint32_t pixel)
{
	int y, x;

	if (!base || pitch == 0 || w <= 0 || h <= 0)
		return;
	if (left <= 0 && right <= 0 && top <= 0 && bottom <= 0)
		return;
	if (left < 0)
		left = 0;
	if (right < 0)
		right = 0;
	if (top < 0)
		top = 0;
	if (bottom < 0)
		bottom = 0;
	if (left + right >= w) {
		left = w / 2;
		right = w - left;
	}
	if (top + bottom >= h) {
		top = h / 4;
		bottom = h / 4;
	}

	for (y = 0; y < h; y++) {
		uint32_t *row = (uint32_t *)(base + (size_t)y * pitch);

		for (x = 0; x < left; x++)
			row[x] = pixel;
		for (x = w - right; x < w; x++)
			row[x] = pixel;
	}

	for (y = 0; y < top; y++) {
		uint32_t *row = (uint32_t *)(base + (size_t)y * pitch);

		for (x = 0; x < w; x++)
			row[x] = pixel;
	}

	for (y = h - bottom; y < h; y++) {
		uint32_t *row = (uint32_t *)(base + (size_t)y * pitch);

		for (x = 0; x < w; x++)
			row[x] = pixel;
	}
}

void lvgl_display_set_polaroid_frame(int left_px, int right_px, int top_px,
		int bottom_px, uint32_t color_rgb24)
{
	uint8_t r;
	uint8_t g;
	uint8_t b;
	int cl, cr, ct, cb;

	cl = clamp_frame_px(left_px, "L");
	cr = clamp_frame_px(right_px, "R");
	ct = clamp_frame_px(top_px, "T");
	cb = clamp_frame_px(bottom_px, "B");
	if (cl < 0 || cr < 0 || ct < 0 || cb < 0)
		return;

	r = (uint8_t)((color_rgb24 >> 16) & 0xFF);
	g = (uint8_t)((color_rgb24 >> 8) & 0xFF);
	b = (uint8_t)(color_rgb24 & 0xFF);

	g_frame_left = cl;
	g_frame_right = cr;
	g_frame_top = ct;
	g_frame_bottom = cb;
	g_frame_rgb24 = color_rgb24 & 0xFFFFFF;
	/* LE memory: B,G,R,A — same as LVGL ARGB8888 + DRM_FORMAT_ARGB8888 */
	g_frame_pixel = (uint32_t)b | ((uint32_t)g << 8) | ((uint32_t)r << 16) |
		((uint32_t)0xFF << 24);

	if (!g_polaroid_suspended) {
		g_saved_left = g_frame_left;
		g_saved_right = g_frame_right;
		g_saved_top = g_frame_top;
		g_saved_bottom = g_frame_bottom;
		g_saved_rgb24 = g_frame_rgb24;
	}

	printf("[ui] polaroid FB edges L=%d R=%d T=%d B=%d color=#%06X\n",
		g_frame_left, g_frame_right, g_frame_top, g_frame_bottom,
		(unsigned)g_frame_rgb24);
	fflush(stdout);
}

void lvgl_display_polaroid_suspend(void)
{
	if (g_polaroid_suspended)
		return;
	g_saved_left = g_frame_left;
	g_saved_right = g_frame_right;
	g_saved_top = g_frame_top;
	g_saved_bottom = g_frame_bottom;
	g_saved_rgb24 = g_frame_rgb24;
	g_polaroid_suspended = 1;
	g_frame_left = 0;
	g_frame_right = 0;
	g_frame_top = 0;
	g_frame_bottom = 0;
	printf("[ui] polaroid suspended (was L=%d R=%d T=%d B=%d color=#%06X)\n",
		g_saved_left, g_saved_right, g_saved_top, g_saved_bottom,
		(unsigned)g_saved_rgb24);
	fflush(stdout);
}

void lvgl_display_polaroid_resume(void)
{
	if (!g_polaroid_suspended)
		return;
	g_polaroid_suspended = 0;
	lvgl_display_set_polaroid_frame(g_saved_left, g_saved_right,
		g_saved_top, g_saved_bottom, g_saved_rgb24);
}

/*
 * Album photo: owned ARGB copy stamped straight into DRM FBs.
 * Never share this with an lv_canvas buffer — LVGL must not touch it.
 */
static uint8_t *g_fs_argb;
static int g_fs_w;
static int g_fs_h;
static size_t g_fs_bytes;
static unsigned g_fs_stamp_n;
static int g_blank_hold;
static int g_hud_active;
static int g_hud_rot180;
static char g_hud_text[80];
static int g_chrome_show_pause;
static int g_chrome_show_exit;
static int g_hit_play_x1, g_hit_play_y1, g_hit_play_x2, g_hit_play_y2;
static int g_hit_exit_x1, g_hit_exit_y1, g_hit_exit_x2, g_hit_exit_y2;
static int g_hit_play_ok;
static int g_hit_exit_ok;
/* Offscreen compose for video paper chrome — avoid memset flash on FB. */
static uint8_t *g_chrome_stage;
static size_t g_chrome_stage_bytes;
/*
 * Live UI in logical coords. Cream is stamped here, then CAM_PREV_UNSHIFT
 * is applied when pushing to DRM maps (same panel tune as album photos /
 * video chrome). Partial LVGL flushes must not write a post-unshift FB.
 */
static uint8_t *g_ui_logical;
static size_t g_ui_logical_bytes;

static int env_int(const char *name, int def)
{
	const char *v = getenv(name);

	if (!v || !v[0])
		return def;
	return atoi(v);
}

/*
 * Screen shows left-circular shift by S (left strip wraps to the right).
 * Compensate: fb[x] = src[(x - S + W) % W].
 */
static void argb_unshift_inplace(uint8_t *buf, int w, int h, int shift)
{
	uint32_t *tmp;
	int y, x;

	if (!buf || w <= 1 || h <= 0 || shift == 0)
		return;
	shift %= w;
	if (shift < 0)
		shift += w;
	if (shift == 0)
		return;

	tmp = malloc((size_t)w * sizeof(uint32_t));
	if (!tmp)
		return;

	for (y = 0; y < h; y++) {
		uint32_t *row = (uint32_t *)(buf + (size_t)y * (size_t)w *
			PIXEL_SIZE);

		for (x = 0; x < w; x++)
			tmp[x] = row[(x - shift + w) % w];
		memcpy(row, tmp, (size_t)w * sizeof(uint32_t));
	}
	free(tmp);
}

/* Bright markers: diagnose whether scanout wraps (red should stay LEFT). */
static void argb_edge_markers(uint8_t *buf, int w, int h)
{
	int y, x;
	const uint32_t red = 0xFFFF0000u;   /* LE B,G,R,A: R=FF A=FF */
	const uint32_t blue = 0xFF0000FFu; /* B=FF A=FF */

	if (!buf || w < 16 || h <= 0)
		return;
	for (y = 0; y < h; y++) {
		uint32_t *row = (uint32_t *)(buf + (size_t)y * (size_t)w *
			PIXEL_SIZE);

		for (x = 0; x < 6; x++)
			row[x] = red;
		for (x = w - 6; x < w; x++)
			row[x] = blue;
	}
}

static void fb_wrap_probe(drm_ui_plane_t *ui, const uint8_t *src)
{
	uint8_t *fb;
	int w, h, y, x, shift, match, best_s, best_m, mism;
	uint32_t pitch;

	if (!ui || !src || !ui->map[ui->front_idx] ||
	    ui->map[ui->front_idx] == MAP_FAILED)
		return;

	w = g_fs_w;
	h = g_fs_h;
	pitch = ui->pitch;
	fb = (uint8_t *)ui->map[ui->front_idx];
	if (w <= 0 || h <= 0 || (int)ui->width < w || (int)ui->height < h)
		return;

	y = h / 2;
	mism = 0;
	for (x = 0; x < w; x += 16) {
		uint32_t a = *(const uint32_t *)(src +
			((size_t)y * (size_t)w + (size_t)x) * PIXEL_SIZE);
		uint32_t b = *(const uint32_t *)(fb +
			(size_t)y * pitch + (size_t)x * PIXEL_SIZE);

		if (a != b)
			mism++;
	}

	best_s = 0;
	best_m = -1;
	for (shift = 40; shift <= 72; shift++) {
		match = 0;
		for (x = 0; x < shift; x++) {
			uint32_t L = *(const uint32_t *)(fb +
				(size_t)y * pitch + (size_t)x * PIXEL_SIZE);
			uint32_t R = *(const uint32_t *)(fb + (size_t)y * pitch +
				(size_t)(w - shift + x) * PIXEL_SIZE);

			if (L == R)
				match++;
		}
		if (match > best_m) {
			best_m = match;
			best_s = shift;
		}
	}

	printf("[ui] fb probe y=%d src_mismatch~%d/row shift=%d match=%d/%d %s\n",
		y, mism * 16, best_s, best_m, best_s,
		(best_m * 2 > best_s) ? "FB_HAS_WRAP" : "fb_ok");
	fflush(stdout);
}

/*
 * If FB row is a left-circular shift of src (left strip wrapped to the
 * right), return best S in [lo,hi]; else 0. Scores against mid scanline.
 */
static int detect_left_wrap_shift(const uint8_t *fb, uint32_t pitch,
		const uint8_t *src, int w, int h, int lo, int hi)
{
	int y, x, shift, best_s, best_score, score, miss;
	const uint32_t *src_row;
	const uint32_t *fb_row;

	if (!fb || !src || w < 80 || h < 2 || pitch == 0)
		return 0;
	if (lo < 1)
		lo = 1;
	if (hi >= w / 2)
		hi = w / 2 - 1;
	if (lo > hi)
		return 0;

	y = h / 2;
	src_row = (const uint32_t *)(src + (size_t)y * (size_t)w * PIXEL_SIZE);
	fb_row = (const uint32_t *)(fb + (size_t)y * pitch);
	best_s = 0;
	best_score = -1;

	for (shift = lo; shift <= hi; shift++) {
		score = 0;
		miss = 0;
		/* fb[x] should equal src[(x+shift)%w] under left-circular wrap */
		for (x = 0; x < w; x += 8) {
			uint32_t expect = src_row[(x + shift) % w];
			if (fb_row[x] == expect)
				score++;
			else
				miss++;
		}
		/* Prefer a clear wrap: left S of src also matches FB right strip */
		{
			int edge = 0;
			int e;

			for (e = 0; e < shift; e += 2) {
				if (fb_row[w - shift + e] == src_row[e])
					edge++;
			}
			if (edge * 4 < shift)
				continue;
		}
		if (score > best_score) {
			best_score = score;
			best_s = shift;
		}
		(void)miss;
	}

	/* Require majority match vs identity (shift 0) */
	{
		int id_score = 0;

		for (x = 0; x < w; x += 8) {
			if (fb_row[x] == src_row[x])
				id_score++;
		}
		if (best_s > 0 && best_score > id_score + (w / 8) / 10)
			return best_s;
	}
	return 0;
}

static void fb_row_unshift(uint8_t *base, uint32_t pitch, int w, int h,
		int shift)
{
	uint32_t *tmp;
	int y, x;

	if (!base || w <= 1 || h <= 0 || shift == 0 || pitch == 0)
		return;
	shift %= w;
	if (shift < 0)
		shift += w;
	if (shift == 0)
		return;

	tmp = malloc((size_t)w * sizeof(uint32_t));
	if (!tmp)
		return;

	for (y = 0; y < h; y++) {
		uint32_t *row = (uint32_t *)(base + (size_t)y * pitch);

		for (x = 0; x < w; x++)
			tmp[x] = row[(x - shift + w) % w];
		memcpy(row, tmp, (size_t)w * sizeof(uint32_t));
	}
	free(tmp);
}

/*
 * Map a pre-unshift hit X range to post-fb_row_unshift FB coords (same
 * circular shift). Touch is in scanout space; chrome_paint_into hits are not.
 */
static void chrome_hit_unshift_x(int *x1, int *x2, int w, int unshift)
{
	int s = unshift;
	int n1, n2;
	int width;

	if (!x1 || !x2 || w <= 1 || unshift == 0)
		return;
	s %= w;
	if (s < 0)
		s += w;
	if (s == 0)
		return;

	width = *x2 - *x1;
	if (width < 1)
		width = 1;
	n1 = (*x1 + s) % w;
	n2 = n1 + width;
	if (n2 <= w) {
		*x1 = n1;
		*x2 = n2;
	} else {
		/* Wrapped strip — accept either edge fragment (enlarge to union). */
		*x1 = 0;
		*x2 = w;
	}
}

static void paint_polaroid_edges(drm_ui_plane_t *ui)
{
	uint8_t *base;

	if (!ui || (g_frame_left <= 0 && g_frame_right <= 0 &&
			g_frame_top <= 0 && g_frame_bottom <= 0))
		return;

	base = g_ui_logical;
	if (!base)
		base = (uint8_t *)drm_ui_plane_get_draw_buf(ui);
	if (!base)
		return;

	paint_polaroid_into(base, ui->pitch, (int)ui->width, (int)ui->height,
		g_frame_left, g_frame_right, g_frame_top, g_frame_bottom,
		g_frame_pixel);
}

static int ui_logical_ensure(drm_ui_plane_t *ui)
{
	size_t need;

	if (!ui || !ui->height || !ui->pitch)
		return -1;
	need = (size_t)ui->height * (size_t)ui->pitch;
	if (g_ui_logical && g_ui_logical_bytes >= need)
		return 0;
	free(g_ui_logical);
	g_ui_logical = calloc(1, need);
	g_ui_logical_bytes = g_ui_logical ? need : 0;
	return g_ui_logical ? 0 : -1;
}

/*
 * Push logical UI (+ cream) to DRM FBs. When live polaroid is active, apply
 * the same CAM_PREV_UNSHIFT as album ARGB / video chrome so cream and the
 * unshifted live NV12 share one panel L/R tune.
 */
static void ui_logical_push_to_fb(drm_ui_plane_t *ui)
{
	size_t sz;
	int i;
	int unshift = 0;
	int polaroid_on;
	static int once_u;

	if (!ui || !g_ui_logical)
		return;

	sz = (size_t)ui->height * (size_t)ui->pitch;
	polaroid_on = !g_polaroid_suspended &&
		(g_frame_left > 0 || g_frame_right > 0 || g_frame_top > 0 ||
			g_frame_bottom > 0);
	if (polaroid_on)
		unshift = env_int("CAM_PREV_UNSHIFT", -35);

	if (!g_chrome_stage || g_chrome_stage_bytes < sz) {
		free(g_chrome_stage);
		g_chrome_stage = malloc(sz);
		g_chrome_stage_bytes = g_chrome_stage ? sz : 0;
	}
	if (!g_chrome_stage)
		return;

	memcpy(g_chrome_stage, g_ui_logical, sz);
	if (unshift != 0)
		fb_row_unshift(g_chrome_stage, ui->pitch, (int)ui->width,
			(int)ui->height, unshift);

	for (i = 0; i < 2; i++) {
		if (!ui->map[i] || ui->map[i] == MAP_FAILED)
			continue;
		memcpy(ui->map[i], g_chrome_stage, sz);
	}

	if (!once_u && polaroid_on) {
		once_u = 1;
		printf("[ui] live cream/UI push unshift=%d "
			"(same CAM_PREV_UNSHIFT as photos)\n",
			unshift);
		fflush(stdout);
	}
}

static void paint_fullscreen_into(uint8_t *base, uint32_t pitch, int fb_w,
		int fb_h)
{
	int y, x, copy_h, copy_w, src_w;
	const uint8_t *src_row;
	uint8_t *dst_row;
	uint32_t opaque_black = 0xFF000000u; /* A=FF — no video bleed */

	if (!base || !g_fs_argb || g_fs_w <= 0 || g_fs_h <= 0 || pitch == 0)
		return;

	/*
	 * Album photo: full-bleed, no polaroid cream. Opaque so the live
	 * NV12 plane cannot show through unused columns/rows.
	 */
	src_w = g_fs_w;
	copy_w = src_w < fb_w ? src_w : fb_w;
	copy_h = g_fs_h;
	if (copy_h > fb_h)
		copy_h = fb_h;

	for (y = 0; y < copy_h; y++) {
		src_row = g_fs_argb + (size_t)y * (size_t)src_w * PIXEL_SIZE;
		dst_row = base + (size_t)y * pitch;
		memcpy(dst_row, src_row, (size_t)copy_w * PIXEL_SIZE);
		for (x = copy_w; x < fb_w; x++)
			((uint32_t *)dst_row)[x] = opaque_black;
	}
	for (y = copy_h; y < fb_h; y++) {
		uint32_t *row = (uint32_t *)(base + (size_t)y * pitch);

		for (x = 0; x < fb_w; x++)
			row[x] = opaque_black;
	}
}

static void paint_fullscreen_argb(drm_ui_plane_t *ui)
{
	int i;
	int wrap_s = 0;

	if (!ui || !g_fs_argb || g_fs_w <= 0 || g_fs_h <= 0)
		return;

	for (i = 0; i < 2; i++) {
		if (!ui->map[i] || ui->map[i] == MAP_FAILED)
			continue;
		paint_fullscreen_into((uint8_t *)ui->map[i], ui->pitch,
			(int)ui->width, (int)ui->height);
	}

	/*
	 * If the first FB already shows left→right wrap vs source, unshift
	 * both buffers. Manual override: CAM_PREV_UNSHIFT (applied earlier
	 * to g_fs_argb) still works; this catches stamp/scanout mismatch.
	 */
	if (ui->map[0] && ui->map[0] != MAP_FAILED) {
		wrap_s = detect_left_wrap_shift((const uint8_t *)ui->map[0],
			ui->pitch, g_fs_argb, g_fs_w, g_fs_h, 32, 80);
		if (wrap_s > 0) {
			for (i = 0; i < 2; i++) {
				if (!ui->map[i] || ui->map[i] == MAP_FAILED)
					continue;
				fb_row_unshift((uint8_t *)ui->map[i], ui->pitch,
					g_fs_w < (int)ui->width ? g_fs_w :
						(int)ui->width,
					g_fs_h < (int)ui->height ? g_fs_h :
						(int)ui->height,
					wrap_s);
			}
			printf("[ui] fs auto-unshift wrap=%d\n", wrap_s);
			fflush(stdout);
		}
	}

	g_fs_stamp_n++;
}

void lvgl_display_set_fullscreen_argb(const void *argb8888, int w, int h)
{
	size_t need;
	const uint8_t *src = (const uint8_t *)argb8888;
	int unshift;
	int mark;

	if (!src || w <= 0 || h <= 0) {
		lvgl_display_clear_fullscreen_argb();
		return;
	}

	need = (size_t)w * (size_t)h * PIXEL_SIZE;
	if (!g_fs_argb || g_fs_bytes != need) {
		free(g_fs_argb);
		g_fs_argb = malloc(need);
		g_fs_bytes = g_fs_argb ? need : 0;
	}
	if (!g_fs_argb) {
		fprintf(stderr, "[ui] fullscreen ARGB alloc failed (%dx%d)\n",
			w, h);
		g_fs_w = 0;
		g_fs_h = 0;
		return;
	}

	memcpy(g_fs_argb, src, need);
	g_fs_w = w;
	g_fs_h = h;

	/*
	 * ARGB overlay scanout on this panel is a RIGHT-circular shift of ~35px
	 * (left edge of the image appears on the far right). FB mmap readback
	 * still matches the stamped buffer (src_mismatch=0) — the shift is in
	 * how the panel presents the plane, not in our memcpy.
	 *
	 * Compensate with a left-circular unshift of -35 (i.e. rotate content
	 * the opposite way before stamp). Tuned on jc-050hd134; override with
	 * CAM_PREV_UNSHIFT. UI_OVERSCAN_R≈40 is the same ballpark.
	 */
	unshift = env_int("CAM_PREV_UNSHIFT", -35);
	if (unshift != 0) {
		argb_unshift_inplace(g_fs_argb, w, h, unshift);
		printf("[ui] fs unshift=%d (ARGB scanout compensate)\n", unshift);
		fflush(stdout);
	}

	mark = env_int("CAM_PREV_MARK", 0);
	if (mark)
		argb_edge_markers(g_fs_argb, w, h);

	if (g_ui_plane) {
		drm_ui_plane_set_content_hold(g_ui_plane, 1);
		paint_fullscreen_argb(g_ui_plane);
		drm_ui_plane_flip(g_ui_plane);
		fb_wrap_probe(g_ui_plane, g_fs_argb);
		g_ui_dirty = 1;
		printf("[ui] fs stamped %dx%d pitch=%u hold=1 fullbleed unshift=%d\n",
			w, h, (unsigned)g_ui_plane->pitch, unshift);
		fflush(stdout);
	}
}

void lvgl_display_clear_fullscreen_argb(void)
{
	if (g_ui_plane)
		drm_ui_plane_set_content_hold(g_ui_plane, 0);
	free(g_fs_argb);
	g_fs_argb = NULL;
	g_fs_bytes = 0;
	g_fs_w = 0;
	g_fs_h = 0;
}

void lvgl_display_hold_blank(int enable)
{
	int i;
	size_t sz;

	if (!g_ui_plane)
		return;

	if (!enable) {
		g_blank_hold = 0;
		g_hud_active = 0;
		g_hud_text[0] = '\0';
		drm_ui_plane_set_content_hold(g_ui_plane, 0);
		printf("[ui] blank hold off\n");
		fflush(stdout);
		return;
	}

	/* Drop any prior ARGB still; UI must stay transparent over video. */
	free(g_fs_argb);
	g_fs_argb = NULL;
	g_fs_bytes = 0;
	g_fs_w = 0;
	g_fs_h = 0;
	g_hud_active = 0;
	g_hud_text[0] = '\0';

	sz = (size_t)g_ui_plane->height * (size_t)g_ui_plane->pitch;
	for (i = 0; i < 2; i++) {
		if (!g_ui_plane->map[i] || g_ui_plane->map[i] == MAP_FAILED)
			continue;
		memset(g_ui_plane->map[i], 0, sz);
	}
	g_blank_hold = 1;
	drm_ui_plane_set_content_hold(g_ui_plane, 1);
	drm_ui_plane_flip(g_ui_plane);
	g_ui_dirty = 1;
	printf("[ui] blank hold on (transparent UI over video still)\n");
	fflush(stdout);
}

void lvgl_display_hold_blank_arm(void)
{
	if (!g_ui_plane)
		return;

	free(g_fs_argb);
	g_fs_argb = NULL;
	g_fs_bytes = 0;
	g_fs_w = 0;
	g_fs_h = 0;
	g_hud_active = 0;
	g_hud_text[0] = '\0';
	g_blank_hold = 1;
	drm_ui_plane_set_content_hold(g_ui_plane, 1);
	/*
	 * Do not memset/flip here — album list stays on the UI plane until
	 * set_video_chrome replaces both FBs with paper chrome in one flip.
	 */
	printf("[ui] blank hold armed (defer flip until chrome)\n");
	fflush(stdout);
}

/*
 * Video time uses the same montserrat_20 glyphs as REC (bpp4 plain).
 * Close × and transport icons are stroked ink on the cream paper frame.
 */
static const uint8_t chrome_opa4[16] = {
	0, 17, 34, 51, 68, 85, 102, 119,
	136, 153, 170, 187, 204, 221, 238, 255
};

static void chrome_blend_rgb(uint32_t *px, uint8_t a, uint8_t fr, uint8_t fg,
		uint8_t fb)
{
	uint32_t bg;
	uint8_t bb, bg_, br, ba;
	uint8_t r, g, b, oa;

	if (!px || a == 0)
		return;
	if (a >= 250) {
		*px = (uint32_t)fb | ((uint32_t)fg << 8) | ((uint32_t)fr << 16) |
			((uint32_t)0xFF << 24);
		return;
	}
	bg = *px;
	bb = (uint8_t)(bg);
	bg_ = (uint8_t)(bg >> 8);
	br = (uint8_t)(bg >> 16);
	ba = (uint8_t)(bg >> 24);
	b = (uint8_t)((fb * a + bb * (255 - a)) / 255);
	g = (uint8_t)((fg * a + bg_ * (255 - a)) / 255);
	r = (uint8_t)((fr * a + br * (255 - a)) / 255);
	oa = ba + (uint8_t)(((255 - ba) * a) / 255);
	*px = (uint32_t)b | ((uint32_t)g << 8) | ((uint32_t)r << 16) |
		((uint32_t)oa << 24);
}

static int chrome_m20_measure(const char *text)
{
	const lv_font_t *font = &lv_font_montserrat_20;
	int w = 0;
	uint32_t letter;
	uint32_t next;
	const char *p;

	if (!text)
		return 0;
	p = text;
	while (*p) {
		lv_font_glyph_dsc_t g;

		letter = (uint8_t)*p;
		if (letter < 0x20 || letter > 0x7F) {
			p++;
			continue;
		}
		next = (uint8_t)p[1];
		if (!lv_font_get_glyph_dsc(font, &g, letter, next)) {
			p++;
			continue;
		}
		w += (int)g.adv_w;
		p++;
	}
	return w;
}

static void chrome_m20_draw(uint32_t *strip, int stride_px, int bw, int bh,
		int x0, int y0, const char *text, uint8_t fr, uint8_t fg,
		uint8_t fb)
{
	const lv_font_t *font = &lv_font_montserrat_20;
	int pen = x0;
	int baseline;
	const char *p;

	if (!strip || !text || bw <= 0 || bh <= 0)
		return;
	baseline = y0 + (int)font->line_height - (int)font->base_line;
	p = text;
	while (*p) {
		lv_font_glyph_dsc_t g;
		const uint8_t *bm;
		uint32_t letter;
		uint32_t next;
		int gx, gy;
		int x, y;
		int bit_i;

		letter = (uint8_t)*p;
		if (letter < 0x20 || letter > 0x7F) {
			p++;
			continue;
		}
		next = (uint8_t)p[1];
		if (!lv_font_get_glyph_dsc(font, &g, letter, next)) {
			p++;
			continue;
		}
		g.req_raw_bitmap = 1;
		g.resolved_font = font;
		bm = font->get_glyph_bitmap(&g, NULL);
		gx = pen + g.ofs_x;
		gy = baseline - (int)g.box_h - g.ofs_y;
		if (bm && g.box_w > 0 && g.box_h > 0) {
			bit_i = 0;
			for (y = 0; y < (int)g.box_h; y++) {
				for (x = 0; x < (int)g.box_w; x++) {
					uint8_t nib;
					uint8_t a;
					int px = gx + x;
					int py = gy + y;

					if ((bit_i & 1) == 0)
						nib = bm[bit_i >> 1] >> 4;
					else
						nib = bm[bit_i >> 1] & 0xF;
					bit_i++;
					a = chrome_opa4[nib];
					if (a == 0 || px < 0 || py < 0 ||
							px >= bw || py >= bh)
						continue;
					chrome_blend_rgb(
						&strip[py * stride_px + px], a,
						fr, fg, fb);
				}
			}
		}
		pen += (int)g.adv_w;
		p++;
	}
}


void lvgl_display_set_video_hud(const char *text, int bar_rot180)
{
	lvgl_video_chrome_t c;

	if (!text || !text[0]) {
		lvgl_display_clear_video_hud();
		return;
	}
	memset(&c, 0, sizeof(c));
	c.time_text = text;
	c.show_pause_icon = 1;
	c.show_exit = 0;
	c.bar_rot180 = bar_rot180;
	lvgl_display_set_video_chrome(&c);
}

void lvgl_display_clear_video_hud(void)
{
	int i;
	size_t sz;

	g_hud_active = 0;
	g_hud_text[0] = '\0';
	g_chrome_show_pause = 0;
	g_chrome_show_exit = 0;
	g_hit_play_ok = 0;
	g_hit_exit_ok = 0;
	if (!g_ui_plane || !g_blank_hold)
		return;
	sz = (size_t)g_ui_plane->height * (size_t)g_ui_plane->pitch;
	for (i = 0; i < 2; i++) {
		if (!g_ui_plane->map[i] || g_ui_plane->map[i] == MAP_FAILED)
			continue;
		memset(g_ui_plane->map[i], 0, sz);
	}
	drm_ui_plane_flip(g_ui_plane);
	g_ui_dirty = 1;
}

static void chrome_blit_strip(uint8_t *base, uint32_t pitch, int fb_w,
		int fb_h, const uint32_t *strip, int bx, int by, int bw,
		int bh, int bar_rot180)
{
	int sy, x;

	for (sy = 0; sy < bh; sy++) {
		int dy = bar_rot180 ? (by + bh - 1 - sy) : (by + sy);
		uint32_t *dst;
		const uint32_t *src;

		if (dy < 0 || dy >= fb_h)
			continue;
		dst = (uint32_t *)(base + (size_t)dy * pitch);
		src = strip + sy * bw;
		for (x = 0; x < bw; x++) {
			int dx = bar_rot180 ? (bx + bw - 1 - x) : (bx + x);

			if (dx < 0 || dx >= fb_w)
				continue;
			dst[dx] = src[x];
		}
	}
}

static void chrome_fill_rect(uint32_t *strip, int bw, int bh, int x0, int y0,
		int w, int h, uint32_t c)
{
	int x, y;

	for (y = y0; y < y0 + h && y < bh; y++) {
		if (y < 0)
			continue;
		for (x = x0; x < x0 + w && x < bw; x++) {
			if (x < 0)
				continue;
			strip[y * bw + x] = c;
		}
	}
}

/* Map local strip hit box through optional 180° blit into FB coords. */
static void chrome_hit_from_local(int strip_x, int strip_y, int strip_w,
		int strip_h, int lx1, int ly1, int lx2, int ly2, int rot,
		int *ox1, int *oy1, int *ox2, int *oy2)
{
	int hx1 = lx1, hy1 = ly1, hx2 = lx2, hy2 = ly2;

	if (rot) {
		hx1 = strip_w - lx2;
		hy1 = strip_h - ly2;
		hx2 = strip_w - lx1;
		hy2 = strip_h - ly1;
	}
	*ox1 = strip_x + hx1;
	*oy1 = strip_y + hy1;
	*ox2 = strip_x + hx2;
	*oy2 = strip_y + hy2;
}

/* Play triangle or pause bars inside a round button. */
static void chrome_draw_transport(uint32_t *strip, int bw, int bh, int x0,
		int y0, int sz, int pause_icon, uint32_t fg)
{
	int cx = x0 + sz / 2;
	int cy = y0 + sz / 2;
	/* Shared inner glyph box (~52% of circle) so play/pause ink weight match. */
	int icon = (sz * 10) / 19;
	int i, j;

	if (icon < 14)
		icon = 14;

	if (pause_icon) {
		int bar_w = icon / 5;
		int bar_h = (icon * 11) / 16;
		int gap = icon / 4;
		int bx0 = cx - gap / 2 - bar_w;
		int bx1 = cx + gap / 2;
		int by = cy - bar_h / 2;

		if (bar_w < 2)
			bar_w = 2;
		chrome_fill_rect(strip, bw, bh, bx0, by, bar_w, bar_h, fg);
		chrome_fill_rect(strip, bw, bh, bx1, by, bar_w, bar_h, fg);
		return;
	}

	/* Play △ sized to the same icon box (slight optical bias right). */
	{
		int ix0 = cx - icon / 2 + icon / 10;
		int iy0 = cy - icon / 2;

		for (j = 0; j < icon; j++) {
			int y = iy0 + j;
			int row = j - icon / 2;
			int half;
			int x_l, x_r;

			if (row < 0)
				row = -row;
			half = (icon / 2) - row;
			if (half < 1)
				continue;
			x_l = ix0;
			x_r = ix0 + half + icon / 5;
			for (i = x_l; i <= x_r; i++) {
				if (y >= 0 && y < bh && i >= 0 && i < bw)
					strip[y * bw + i] = fg;
			}
		}
	}
}

/* Back chevron "<" (two strokes) — album video exit / back. */
static void chrome_draw_chevron_left(uint32_t *strip, int bw, int bh, int cx,
		int cy, int arm, uint32_t ink)
{
	int t = arm / 5;
	int i, k;

	if (arm < 8)
		arm = 8;
	if (t < 2)
		t = 2;
	/* Upper and lower arms meeting at the left tip. */
	for (i = 0; i <= arm; i++) {
		for (k = -t / 2; k <= t / 2; k++) {
			int x = cx - arm / 2 + i;
			int y_up = cy - i + k;
			int y_dn = cy + i + k;

			if (x >= 0 && x < bw && y_up >= 0 && y_up < bh)
				strip[y_up * bw + x] = ink;
			if (x >= 0 && x < bw && y_dn >= 0 && y_dn < bh)
				strip[y_dn * bw + x] = ink;
		}
	}
}

/*
 * Polaroid paper frame for album video — same visible matte as album photos:
 *   L/R = UI_FRAME_* (equal, default 16; matches JPEG polaroid side @720)
 *   top/bottom = status/ctrl heights (thin top / thick bottom caption)
 * After paint, set_video_chrome applies CAM_PREV_UNSHIFT (same as ARGB
 * photos) so cream and content share panel scanout compensation.
 */
static void chrome_paint_into(uint8_t *base, uint32_t pitch, int fb_w,
		int fb_h, const lvgl_video_chrome_t *c)
{
	/* Match live UI_PAPER_COLOR #F4EFE6 */
	uint32_t cream = (uint32_t)0xE6 | (0xEF << 8) | (0xF4 << 16) |
		((uint32_t)0xFF << 24);
	/* Soft charcoal ink on cream */
	uint32_t ink = (uint32_t)0x2C | (0x2C << 8) | (0x2C << 16) |
		((uint32_t)0xFF << 24);
	uint32_t *bot = NULL;
	uint32_t *top = NULL;
	int side_l, side_r;
	int paper_top; /* user top / Exit */
	int paper_bot; /* user bottom / transport (thicker matte) */
	int ap_x0, ap_w;
	int fb_t, fb_b;
	int btn, gap, pad_x, pad_y, th, tw;
	int cluster_w, cluster_h, bot_x, bot_y;
	int bx, by, time_x, time_y;
	int exit_sz, exit_x, exit_y;
	int rot = c && c->bar_rot180;

	g_hit_play_ok = 0;
	g_hit_exit_ok = 0;
	if (!base || !c || fb_w <= 0 || fb_h <= 0)
		return;

	side_l = env_int("UI_FRAME_L", 16);
	side_r = env_int("UI_FRAME_R", 16);
	if (side_l < 0)
		side_l = 0;
	if (side_r < 0)
		side_r = 0;
	paper_top = env_int("UI_STATUS_BAR_H", 56);
	paper_bot = env_int("UI_CTRL_BAR_H", 152);
	if (paper_top < 48)
		paper_top = 48;
	if (paper_bot < 96)
		paper_bot = 96;

	/* With ROT180: FB top = user bottom (thick), FB bottom = user top. */
	fb_t = rot ? paper_bot : paper_top;
	fb_b = rot ? paper_top : paper_bot;
	paint_polaroid_into(base, pitch, fb_w, fb_h, side_l, side_r, fb_t, fb_b,
		cream);

	/* Center chrome in the paper aperture. */
	ap_x0 = side_l;
	ap_w = fb_w - side_l - side_r;
	if (ap_w < 64)
		ap_w = fb_w;

	btn = 48;
	gap = 14;
	pad_x = 14;
	pad_y = 10;
	th = (int)lv_font_montserrat_20.line_height;
	tw = c->time_text ? chrome_m20_measure(c->time_text) : 0;
	cluster_h = btn + pad_y * 2;
	if (cluster_h < th + pad_y * 2)
		cluster_h = th + pad_y * 2;
	if (cluster_h > paper_bot - 8)
		cluster_h = paper_bot - 8;
	cluster_w = pad_x + btn + (tw > 0 ? gap + tw : 0) + pad_x;
	if (cluster_w > ap_w - 8)
		cluster_w = ap_w - 8;
	bot_x = ap_x0 + (ap_w - cluster_w) / 2;
	bot_y = rot ? ((paper_bot - cluster_h) / 2) :
		(fb_h - paper_bot + (paper_bot - cluster_h) / 2);
	if (bot_y < 0)
		bot_y = 0;

	bot = calloc((size_t)cluster_w * (size_t)cluster_h, sizeof(uint32_t));
	if (!bot)
		return;
	/* Opaque cream so blit does not punch holes in the paper band. */
	chrome_fill_rect(bot, cluster_w, cluster_h, 0, 0, cluster_w,
		cluster_h, cream);
	bx = pad_x;
	by = (cluster_h - btn) / 2;
	/* Soft ring around transport so the hit target reads as a button. */
	{
		int cx = bx + btn / 2;
		int cy = by + btn / 2;
		int rad = btn / 2;
		int t = 2;
		int i, k;

		for (i = -rad; i <= rad; i++) {
			int py = cy + i;

			if (py < 0 || py >= cluster_h)
				continue;
			for (k = -rad; k <= rad; k++) {
				int px = cx + k;
				int d2 = i * i + k * k;

				if (px < 0 || px >= cluster_w)
					continue;
				if (d2 <= rad * rad &&
						d2 >= (rad - t) * (rad - t))
					bot[py * cluster_w + px] = ink;
			}
		}
	}
	chrome_draw_transport(bot, cluster_w, cluster_h, bx, by, btn,
		c->show_pause_icon, ink);

	time_x = bx + btn + gap;
	time_y = (cluster_h - th) / 2;
	if (c->time_text && tw > 0)
		chrome_m20_draw(bot, cluster_w, cluster_w, cluster_h, time_x,
			time_y, c->time_text, 0x2C, 0x2C, 0x2C);

	{
		int hx1 = bx - 6;
		int hy1 = by - 6;
		int hx2 = bx + btn + 6;
		int hy2 = by + btn + 6;

		if (hx1 < 0)
			hx1 = 0;
		if (hy1 < 0)
			hy1 = 0;
		chrome_hit_from_local(bot_x, bot_y, cluster_w, cluster_h, hx1, hy1,
			hx2, hy2, rot, &g_hit_play_x1, &g_hit_play_y1,
			&g_hit_play_x2, &g_hit_play_y2);
		g_hit_play_ok = 1;
	}
	chrome_blit_strip(base, pitch, fb_w, fb_h, bot, bot_x, bot_y, cluster_w,
		cluster_h, rot);
	free(bot);

	if (!c->show_exit)
		return;

	/*
	 * Back "<" on the user-LEFT cream frame, vertically at 1/4 from the
	 * user TOP (not along the top status band). BAR_ROT180: user-left =
	 * FB-right, user-top = FB-bottom → FB y ≈ 3/4 height.
	 * Button may overhang the thin side matte into the aperture (side≈16,
	 * hit target stays ~44 for touch).
	 */
	exit_sz = 44;
	if (rot) {
		exit_x = fb_w - side_r + (side_r - exit_sz) / 2;
		exit_y = (fb_h * 3) / 4 - exit_sz / 2;
	} else {
		exit_x = (side_l - exit_sz) / 2;
		exit_y = fb_h / 4 - exit_sz / 2;
	}
	if (exit_x < 0)
		exit_x = 0;
	if (exit_x + exit_sz > fb_w)
		exit_x = fb_w - exit_sz;
	if (exit_y < 4)
		exit_y = 4;
	if (exit_y + exit_sz > fb_h - 4)
		exit_y = fb_h - exit_sz - 4;

	top = calloc((size_t)exit_sz * (size_t)exit_sz, sizeof(uint32_t));
	if (!top)
		return;
	/* Cream chip so blit does not punch transparent holes into the frame. */
	chrome_fill_rect(top, exit_sz, exit_sz, 0, 0, exit_sz, exit_sz, cream);
	chrome_draw_chevron_left(top, exit_sz, exit_sz, exit_sz / 2,
		exit_sz / 2, exit_sz / 3, ink);
	chrome_blit_strip(base, pitch, fb_w, fb_h, top, exit_x, exit_y, exit_sz,
		exit_sz, rot);
	free(top);

	g_hit_exit_x1 = exit_x - 20;
	g_hit_exit_y1 = exit_y - 20;
	g_hit_exit_x2 = exit_x + exit_sz + 20;
	g_hit_exit_y2 = exit_y + exit_sz + 20;
	if (g_hit_exit_x1 < 0)
		g_hit_exit_x1 = 0;
	if (g_hit_exit_y1 < 0)
		g_hit_exit_y1 = 0;
	g_hit_exit_ok = 1;
}

void lvgl_display_set_video_chrome(const lvgl_video_chrome_t *chrome)
{
	int i;
	size_t sz;
	char tbuf[80];
	int pause_i;
	int exit_i;
	int rot;
	int unshift;
	static int logged;
	lvgl_video_chrome_t local;

	if (!g_ui_plane || !g_blank_hold || !chrome)
		return;

	snprintf(tbuf, sizeof(tbuf), "%s",
		chrome->time_text ? chrome->time_text : "");
	pause_i = chrome->show_pause_icon ? 1 : 0;
	exit_i = chrome->show_exit ? 1 : 0;
	rot = chrome->bar_rot180 ? 1 : 0;

	if (g_hud_active && g_hud_rot180 == rot &&
			g_chrome_show_pause == pause_i &&
			g_chrome_show_exit == exit_i &&
			strncmp(g_hud_text, tbuf, sizeof(g_hud_text)) == 0)
		return;

	snprintf(g_hud_text, sizeof(g_hud_text), "%s", tbuf);
	g_hud_rot180 = rot;
	g_chrome_show_pause = pause_i;
	g_chrome_show_exit = exit_i;
	g_hud_active = 1;

	/*
	 * Compose offscreen, then copy finished planes. Never memset the live
	 * FB first — that flashed transparent (side cream flicker) on every
	 * time tick / Exit toggle.
	 *
	 * CAM_PREV_UNSHIFT matches album ARGB photos (panel L/R roll).
	 */
	unshift = env_int("CAM_PREV_UNSHIFT", -35);
	sz = (size_t)g_ui_plane->height * (size_t)g_ui_plane->pitch;
	if (!g_chrome_stage || g_chrome_stage_bytes < sz) {
		free(g_chrome_stage);
		g_chrome_stage = malloc(sz);
		g_chrome_stage_bytes = g_chrome_stage ? sz : 0;
	}
	if (!g_chrome_stage)
		return;

	memset(g_chrome_stage, 0, sz);
	memset(&local, 0, sizeof(local));
	local.time_text = g_hud_text;
	local.show_pause_icon = pause_i;
	local.show_exit = exit_i;
	local.bar_rot180 = rot;
	chrome_paint_into(g_chrome_stage, g_ui_plane->pitch,
		(int)g_ui_plane->width, (int)g_ui_plane->height, &local);
	if (unshift != 0) {
		/* Hits were in paint space; touch uses post-unshift scanout. */
		chrome_hit_unshift_x(&g_hit_play_x1, &g_hit_play_x2,
			(int)g_ui_plane->width, unshift);
		chrome_hit_unshift_x(&g_hit_exit_x1, &g_hit_exit_x2,
			(int)g_ui_plane->width, unshift);
		fb_row_unshift(g_chrome_stage, g_ui_plane->pitch,
			(int)g_ui_plane->width, (int)g_ui_plane->height,
			unshift);
	}

	for (i = 0; i < 2; i++) {
		if (!g_ui_plane->map[i] || g_ui_plane->map[i] == MAP_FAILED)
			continue;
		memcpy(g_ui_plane->map[i], g_chrome_stage, sz);
	}
	if (!logged) {
		printf("[ui] video chrome paper L/R=%d/%d unshift=%d "
			"(offscreen compose, same CAM_PREV_UNSHIFT as photos)\n",
			env_int("UI_FRAME_L", 16), env_int("UI_FRAME_R", 16),
			unshift);
		fflush(stdout);
		logged = 1;
	}
	if (exit_i && g_hit_exit_ok) {
		static int exit_hit_logged;

		if (!exit_hit_logged) {
			printf("[ui] video back hit FB [%d..%d)x[%d..%d) "
				"(post-unshift)\n",
				g_hit_exit_x1, g_hit_exit_x2,
				g_hit_exit_y1, g_hit_exit_y2);
			fflush(stdout);
			exit_hit_logged = 1;
		}
	}
	drm_ui_plane_flip(g_ui_plane);
	g_ui_dirty = 1;
}

int lvgl_display_video_chrome_hit(int x, int y)
{
	if (g_hit_play_ok && x >= g_hit_play_x1 && x < g_hit_play_x2 &&
			y >= g_hit_play_y1 && y < g_hit_play_y2)
		return LVGL_VCHROME_PLAY;
	if (g_hit_exit_ok && x >= g_hit_exit_x1 && x < g_hit_exit_x2 &&
			y >= g_hit_exit_y1 && y < g_hit_exit_y2)
		return LVGL_VCHROME_EXIT;
	return LVGL_VCHROME_NONE;
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area,
		uint8_t *px_map)
{
	drm_ui_plane_t *ui = g_ui_plane;
	int32_t area_w;
	uint8_t *dst;
	uint8_t *src = px_map;
	int y;
	int x1, y1, x2, y2;
	int copy_w, copy_h;
	int src_stride;

	if (!ui || !area || !px_map)
		goto done;

	/*
	 * Blank hold / ARGB still: skip LVGL blit so video (or stamped still)
	 * stays visible. Video time HUD is painted by set_video_hud().
	 */
	if (g_blank_hold || (g_fs_argb && g_fs_w > 0 && g_fs_h > 0))
		goto done;

	/* Transform can inflate dirty areas past FB edges. */
	x1 = area->x1;
	y1 = area->y1;
	x2 = area->x2;
	y2 = area->y2;
	if (x1 < 0)
		x1 = 0;
	if (y1 < 0)
		y1 = 0;
	if (x2 >= (int)ui->width)
		x2 = (int)ui->width - 1;
	if (y2 >= (int)ui->height)
		y2 = (int)ui->height - 1;
	if (x1 > x2 || y1 > y2)
		goto done;

	/*
	 * Use the active draw_buf stride — NOT width_to_stride(area_w).
	 * Partial mode reshapes the buf; guessing stride from the (possibly
	 * transform-inflated) area caused classic left→right row wrap.
	 */
	{
		lv_draw_buf_t *abuf = lv_display_get_buf_active(disp);

		area_w = lv_area_get_width(area);
		if (abuf && abuf->header.stride > 0)
			src_stride = (int)abuf->header.stride;
		else
			src_stride = (int)lv_draw_buf_width_to_stride((uint32_t)area_w,
				LV_COLOR_FORMAT_ARGB8888);
	}
	copy_w = (x2 - x1 + 1) * PIXEL_SIZE;
	copy_h = y2 - y1 + 1;

	/*
	 * Compose in logical space, then CAM_PREV_UNSHIFT when pushing to DRM
	 * (live cream must move with the NV12 plane unshift — not stay nailed
	 * to FB edges while video alone shifts).
	 */
	if (ui_logical_ensure(ui) != 0)
		goto done;
	dst = g_ui_logical;

	for (y = 0; y < copy_h; y++) {
		int src_y = (y1 - area->y1) + y;
		int src_x_off = (x1 - area->x1) * PIXEL_SIZE;

		memcpy(dst + ((size_t)(y1 + y) * ui->pitch + (size_t)x1 * PIXEL_SIZE),
			src + (size_t)src_y * src_stride + src_x_off,
			(size_t)copy_w);
	}

	/*
	 * Polaroid + CAM_PREV_UNSHIFT push touches the whole FB (~3.6MB x2).
	 * Doing that on every partial strip during lv_refr_now starved the
	 * VSE drain thread (getframe miss 0xffffffff until restart).
	 * Only compose+push on the last strip of a refresh cycle.
	 */
	if (lv_display_flush_is_last(disp)) {
		paint_polaroid_edges(ui);
		ui_logical_push_to_fb(ui);
		g_ui_dirty = 1;
	}

done:
	lv_display_flush_ready(disp);
}

int lvgl_display_init(drm_ui_plane_t *ui)
{
	uint32_t buf_bytes;
	lv_obj_t *scr;
	lv_obj_t *bottom;

	if (!ui || !ui->width || !ui->height)
		return -1;

	g_ui_plane = ui;
	lv_init();
	lv_tick_set_cb(custom_tick_get);

	if (ui_logical_ensure(ui) != 0) {
		fprintf(stderr, "lvgl_display: logical FB alloc failed\n");
		return -1;
	}

	buf_bytes = ui->width * DRAW_BUF_LINES * PIXEL_SIZE;
	g_buf1 = lv_malloc(buf_bytes);
	if (!g_buf1) {
		fprintf(stderr, "lvgl_display: draw buf alloc failed (%u bytes)\n",
			buf_bytes);
		free(g_ui_logical);
		g_ui_logical = NULL;
		g_ui_logical_bytes = 0;
		return -1;
	}

	g_disp = lv_display_create((int32_t)ui->width, (int32_t)ui->height);
	if (!g_disp) {
		fprintf(stderr, "lvgl_display: lv_display_create failed\n");
		lv_free(g_buf1);
		g_buf1 = NULL;
		return -1;
	}

	/* ARGB8888 so transparent UI regions reveal the DRM video plane. */
	lv_display_set_color_format(g_disp, LV_COLOR_FORMAT_ARGB8888);
	lv_display_set_flush_cb(g_disp, lvgl_flush_cb);
	lv_display_set_buffers(g_disp, g_buf1, NULL, buf_bytes,
		LV_DISPLAY_RENDER_MODE_PARTIAL);

	scr = lv_display_get_screen_active(g_disp);
	bottom = lv_display_get_layer_bottom(g_disp);
	lv_obj_set_style_bg_opa(scr, LV_OPA_TRANSP, 0);
	if (bottom)
		lv_obj_set_style_bg_opa(bottom, LV_OPA_TRANSP, 0);

	printf("lvgl_display: %ux%u ARGB8888, draw_buf %u lines (%.1f KB)\n",
		ui->width, ui->height, DRAW_BUF_LINES,
		(double)buf_bytes / 1024.0);
	return 0;
}

void lvgl_display_deinit(void)
{
	lvgl_display_clear_fullscreen_argb();
	if (g_buf1) {
		lv_free(g_buf1);
		g_buf1 = NULL;
	}
	free(g_ui_logical);
	g_ui_logical = NULL;
	g_ui_logical_bytes = 0;
	free(g_chrome_stage);
	g_chrome_stage = NULL;
	g_chrome_stage_bytes = 0;
	g_disp = NULL;
	g_ui_plane = NULL;
}

void lvgl_display_refresh(void)
{
	lv_timer_handler();
}

int lvgl_display_take_dirty(void)
{
	if (!g_ui_dirty)
		return 0;
	g_ui_dirty = 0;
	return 1;
}
