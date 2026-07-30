#include "ui_gallery.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lvgl_display.h"
#include "media_player.h"
#include "media_still.h"

#define GALLERY_MAX_DEFAULT 30
#define GALLERY_SCAN_MAX    128
#define GALLERY_HDR_H       80
#define GALLERY_PAD         12
#define GALLERY_DEL_W       112
#define GALLERY_DEL_H       64
/* List-row thumb display size (scaled from CAM_THUMB 160x90). */
#define GALLERY_THUMB_W     120
#define GALLERY_THUMB_H     68
#define GALLERY_ROW_H       100
/* Double-tap window for exit strip (user bottom middle). */
#define GALLERY_EXIT_DBL_MS 450
#define GALLERY_EXIT_H_DEFAULT 96
#define GALLERY_PREV_MAX_W  720
#define GALLERY_PREV_MAX_H  1280
/* Finger move beyond this → treat as scroll/drag, not open (short lists too). */
#define GALLERY_DRAG_OPEN_PX 14
#define PREVIEW_VIDEO_HUD_MS 250
#define PREVIEW_EXIT_HIDE_MS 2500
/* Poll while album list covers decode; reveal UI after first frame. */
#define PREVIEW_VIDEO_WAIT_MS 50
#define PREVIEW_VIDEO_WAIT_MAX_MS 800

/*
 * Play hook kept in .data as well as g->on_video_play. Album UI struct has
 * been stomped before (save_dir / path buffers); a NULL g->on_video_play then
 * silently fell back to static poster with no HW attempt.
 */
static ui_gallery_video_play_fn g_video_play_fn;
static void *g_video_play_user;

/* Suppress open-on-CLICKED after a drag (esp. when list cannot scroll). */
static lv_point_t g_gallery_ptr_down;
static int g_gallery_ptr_dragged;

static void gallery_ptr_track(lv_event_code_t code)
{
	lv_indev_t *indev = lv_indev_get_act();
	lv_point_t p;

	if (!indev)
		return;
	if (code == LV_EVENT_PRESSED) {
		lv_indev_get_point(indev, &g_gallery_ptr_down);
		g_gallery_ptr_dragged = 0;
		return;
	}
	if (code == LV_EVENT_PRESSING || code == LV_EVENT_RELEASED) {
		lv_indev_get_point(indev, &p);
		if (LV_ABS(p.x - g_gallery_ptr_down.x) >= GALLERY_DRAG_OPEN_PX ||
				LV_ABS(p.y - g_gallery_ptr_down.y) >= GALLERY_DRAG_OPEN_PX)
			g_gallery_ptr_dragged = 1;
	}
}

/* 1 = do not open preview (scroll in progress or finger dragged). */
static int gallery_click_is_drag(void)
{
	lv_indev_t *indev = lv_indev_get_act();

	if (g_gallery_ptr_dragged)
		return 1;
	if (indev && lv_indev_get_scroll_obj(indev))
		return 1;
	return 0;
}

typedef struct {
	char path[256];
	char name[96];
	time_t mtime;
	off_t size;
} gallery_entry_t;

static int env_int(const char *name, int def)
{
	const char *v = getenv(name);

	if (v && v[0])
		return atoi(v);
	return def;
}

static int env_on(const char *name, int def)
{
	const char *v = getenv(name);

	if (!v || !v[0])
		return def;
	return atoi(v) != 0;
}

/* Match ui_camera: spin glyph 180° around widget center (layout stays put). */
static void rotate_widget_180(lv_obj_t *obj)
{
	lv_coord_t w;
	lv_coord_t h;

	if (!obj)
		return;
	lv_obj_update_layout(obj);
	w = lv_obj_get_width(obj);
	h = lv_obj_get_height(obj);
	if (w < 1)
		w = 1;
	if (h < 1)
		h = 1;
	lv_obj_set_style_transform_pivot_x(obj, w / 2, 0);
	lv_obj_set_style_transform_pivot_y(obj, h / 2, 0);
	lv_obj_set_style_transform_angle(obj, 1800, 0);
}

static int is_jpeg_name(const char *name)
{
	size_t n;

	if (!name)
		return 0;
	n = strlen(name);
	if (n < 5)
		return 0;
	if (strcasecmp(name + n - 4, ".jpg") == 0)
		return 1;
	if (strcasecmp(name + n - 5, ".jpeg") == 0)
		return 1;
	return 0;
}

static int is_mp4_name(const char *name)
{
	size_t n;

	if (!name)
		return 0;
	n = strlen(name);
	if (n < 5)
		return 0;
	return strcasecmp(name + n - 4, ".mp4") == 0;
}

static void video_anim_sync(void);
typedef struct list_thumb_anim list_thumb_anim_t;
static void video_anim_stop(list_thumb_anim_t *a);
static list_thumb_anim_t *g_vthumb_playing;

static ui_gallery_video_play_fn video_play_fn(ui_gallery_t *g)
{
	if (g && g->on_video_play)
		return g->on_video_play;
	return g_video_play_fn;
}

static void *video_play_user(ui_gallery_t *g)
{
	if (g && g->on_video_play)
		return g->still_user;
	return g_video_play_user;
}

/* Same finger-up that opens preview must not instantly dismiss it. */
#define PREVIEW_DISMISS_GUARD_MS 600

static void preview_guard_timer_cb(lv_timer_t *t)
{
	ui_gallery_t *g = lv_timer_get_user_data(t);

	if (g) {
		g->preview_guard_armed = 0;
		g->preview_guard_timer = NULL;
		if (g->preview && lv_obj_is_valid(g->preview) &&
				!lv_obj_has_flag(g->preview, LV_OBJ_FLAG_HIDDEN))
			lv_obj_add_flag(g->preview, LV_OBJ_FLAG_CLICKABLE);
		printf("[ui] preview dismiss armed\n");
		fflush(stdout);
	}
}

static void preview_arm_guard(ui_gallery_t *g)
{
	lv_indev_t *indev;

	if (!g)
		return;
	g->preview_guard_armed = 1;
	g->preview_guard_ms = lv_tick_get();
	/* Not clickable until gesture ends + short delay. */
	if (g->preview && lv_obj_is_valid(g->preview))
		lv_obj_clear_flag(g->preview, LV_OBJ_FLAG_CLICKABLE);
	indev = lv_indev_get_act();
	if (indev)
		lv_indev_wait_release(indev);
	if (g->preview_guard_timer) {
		lv_timer_del(g->preview_guard_timer);
		g->preview_guard_timer = NULL;
	}
	g->preview_guard_timer = lv_timer_create(preview_guard_timer_cb,
		PREVIEW_DISMISS_GUARD_MS, g);
	if (g->preview_guard_timer)
		lv_timer_set_repeat_count(g->preview_guard_timer, 1);
}

static void preview_clear_guard(ui_gallery_t *g)
{
	if (!g)
		return;
	g->preview_guard_armed = 0;
	if (g->preview_guard_timer) {
		lv_timer_del(g->preview_guard_timer);
		g->preview_guard_timer = NULL;
	}
}

static void format_duration(char *out, size_t out_len, int sec);
static void preview_hide(ui_gallery_t *g);

static void preview_video_session_clear(ui_gallery_t *g)
{
	if (!g)
		return;
	if (g->preview_video_timer) {
		lv_timer_del(g->preview_video_timer);
		g->preview_video_timer = NULL;
	}
	if (g->preview_exit_timer) {
		lv_timer_del(g->preview_exit_timer);
		g->preview_exit_timer = NULL;
	}
	g->preview_video_ended = 0;
	g->preview_video_dur_sec = 0;
	g->preview_exit_visible = 0;
	g->preview_video_awaiting = 0;
	g->preview_video_ui_shown = 0;
	g->preview_video_wait_ms = 0;
	g->preview_video_path[0] = '\0';
	lvgl_display_clear_video_hud();
}

static void preview_video_refresh_chrome(ui_gallery_t *g, int el, int dur,
		int ended)
{
	char a[16];
	char b[16];
	char line[40];
	lvgl_video_chrome_t chrome;
	int paused;

	if (!g)
		return;
	if (dur <= 0)
		dur = g->preview_video_dur_sec;
	if (dur < 0)
		dur = 0;
	if (ended && dur > 0)
		el = dur;
	if (el < 0)
		el = 0;
	format_duration(a, sizeof(a), el);
	format_duration(b, sizeof(b), dur);
	if (dur > 0)
		snprintf(line, sizeof(line), "%s / %s", a, b);
	else
		snprintf(line, sizeof(line), "%s", a);

	paused = media_player_is_paused();
	/* Pause bars while playing; play triangle when paused or ended. */
	memset(&chrome, 0, sizeof(chrome));
	chrome.time_text = line;
	chrome.show_pause_icon = (!ended && !paused) ? 1 : 0;
	chrome.show_exit = g->preview_exit_visible ? 1 : 0;
	chrome.bar_rot180 = g->bar_rot180;

	if (g->preview_hint)
		lv_obj_add_flag(g->preview_hint, LV_OBJ_FLAG_HIDDEN);
	lvgl_display_set_video_chrome(&chrome);
}

static void preview_video_set_hint(ui_gallery_t *g, int el, int dur, int ended)
{
	/* Legacy name — paint mini-player chrome. */
	preview_video_refresh_chrome(g, el, dur, ended);
}

static void preview_video_reveal_ui(ui_gallery_t *g)
{
	if (!g || g->preview_video_ui_shown)
		return;

	g->preview_video_ui_shown = 1;
	g->preview_video_awaiting = 0;

	/*
	 * First decoded frame (or poster after timeout) is already on the
	 * video plane under the album list. Arm + chrome in one flip so the
	 * user never sees poster→frame or empty transparent UI.
	 */
	lvgl_display_hold_blank_arm();

	if (g->hdr)
		lv_obj_add_flag(g->hdr, LV_OBJ_FLAG_HIDDEN);
	if (g->list)
		lv_obj_add_flag(g->list, LV_OBJ_FLAG_HIDDEN);
	if (g->exit_zone)
		lv_obj_add_flag(g->exit_zone, LV_OBJ_FLAG_HIDDEN);
	if (g->preview_msg)
		lv_obj_add_flag(g->preview_msg, LV_OBJ_FLAG_HIDDEN);
	if (g->preview_canvas)
		lv_obj_add_flag(g->preview_canvas, LV_OBJ_FLAG_HIDDEN);

	lv_obj_set_style_bg_opa(g->preview, LV_OPA_TRANSP, 0);
	lv_obj_clear_flag(g->preview, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_foreground(g->preview);
	if (g->root)
		lv_obj_set_style_bg_opa(g->root, LV_OPA_TRANSP, 0);

	preview_video_set_hint(g, 0, g->preview_video_dur_sec, 0);
	if (g->preview_video_timer)
		lv_timer_set_period(g->preview_video_timer, PREVIEW_VIDEO_HUD_MS);

	printf("[ui] album video UI revealed (frames=%d)\n",
		media_player_frames_shown());
	fflush(stdout);
}

static void preview_video_replay(ui_gallery_t *g);

static void preview_exit_hide_timer_cb(lv_timer_t *t)
{
	ui_gallery_t *g = lv_timer_get_user_data(t);
	int el = 0, dur = 0, ended = 0, failed = 0;

	if (!g)
		return;
	g->preview_exit_timer = NULL;
	if (!g->preview_exit_visible)
		return;
	g->preview_exit_visible = 0;
	if (g->preview_video_path[0] &&
			media_player_get_status(&el, &dur, &ended, &failed)) {
		if (dur <= 0 && g->preview_video_dur_sec > 0)
			dur = g->preview_video_dur_sec;
		preview_video_refresh_chrome(g, el, dur,
			ended || g->preview_video_ended);
	} else {
		preview_video_refresh_chrome(g, 0, g->preview_video_dur_sec,
			g->preview_video_ended);
	}
}

static void preview_exit_arm_autohide(ui_gallery_t *g)
{
	if (!g)
		return;
	if (g->preview_exit_timer) {
		lv_timer_del(g->preview_exit_timer);
		g->preview_exit_timer = NULL;
	}
	if (!g->preview_exit_visible)
		return;
	g->preview_exit_timer = lv_timer_create(preview_exit_hide_timer_cb,
		PREVIEW_EXIT_HIDE_MS, g);
	if (g->preview_exit_timer)
		lv_timer_set_repeat_count(g->preview_exit_timer, 1);
}

static void preview_exit_toggle(ui_gallery_t *g)
{
	int el = 0, dur = 0, ended = 0, failed = 0;

	if (!g || !g->preview_video_path[0])
		return;
	g->preview_exit_visible = !g->preview_exit_visible;
	if (media_player_get_status(&el, &dur, &ended, &failed)) {
		if (dur <= 0 && g->preview_video_dur_sec > 0)
			dur = g->preview_video_dur_sec;
		preview_video_refresh_chrome(g, el, dur,
			ended || g->preview_video_ended);
	} else {
		preview_video_refresh_chrome(g, 0, g->preview_video_dur_sec,
			g->preview_video_ended);
	}
	preview_exit_arm_autohide(g);
	printf("[ui] album video Exit %s\n",
		g->preview_exit_visible ? "shown" : "hidden");
	fflush(stdout);
}

static void preview_show_video_poster(ui_gallery_t *g, const char *abs_path);
static void preview_show_video(ui_gallery_t *g, const char *abs_path);

static void preview_video_timer_cb(lv_timer_t *t)
{
	ui_gallery_t *g = lv_timer_get_user_data(t);
	int el = 0;
	int dur = 0;
	int ended = 0;
	int failed = 0;
	char path[512];

	if (!g || !g->visible || !g->preview_video_path[0])
		return;

	/* Keep album list up until first frame is on the video plane. */
	if (g->preview_video_awaiting) {
		int frames = media_player_frames_shown();
		int ready = 0;

		if (!media_player_get_status(&el, &dur, &ended, &failed)) {
			/* Player vanished — fall through to fail path below. */
			failed = 1;
		}
		if (failed) {
			snprintf(path, sizeof(path), "%s", g->preview_video_path);
			fprintf(stderr,
				"[ui] album HW decode failed — poster fallback\n");
			fflush(stderr);
			{
				ui_gallery_video_play_fn play = video_play_fn(g);

				if (play)
					(void)play(video_play_user(g), NULL);
			}
			preview_video_session_clear(g);
			lvgl_display_hold_blank(0);
			preview_show_video_poster(g, path);
			return;
		}
		if (frames >= 1)
			ready = 1;
		else if (lv_tick_elaps(g->preview_video_wait_ms) >=
				PREVIEW_VIDEO_WAIT_MAX_MS)
			ready = 1;
		if (ready)
			preview_video_reveal_ui(g);
		return;
	}

	if (!media_player_get_status(&el, &dur, &ended, &failed))
		return;
	if (failed) {
		snprintf(path, sizeof(path), "%s", g->preview_video_path);
		fprintf(stderr,
			"[ui] album HW decode failed — poster fallback\n");
		fflush(stderr);
		{
			ui_gallery_video_play_fn play = video_play_fn(g);

			if (play)
				(void)play(video_play_user(g), NULL);
		}
		preview_video_session_clear(g);
		lvgl_display_hold_blank(0);
		preview_show_video_poster(g, path);
		return;
	}
	if (dur <= 0 && g->preview_video_dur_sec > 0)
		dur = g->preview_video_dur_sec;
	else if (dur > 0)
		g->preview_video_dur_sec = dur;
	if (ended && !g->preview_video_ended) {
		g->preview_video_ended = 1;
		printf("[ui] album video ended (hold last frame)\n");
		fflush(stdout);
	}
	preview_video_refresh_chrome(g, el, dur, g->preview_video_ended);
}

static void preview_video_replay(ui_gallery_t *g)
{
	int ok;
	ui_gallery_video_play_fn play;

	if (!g || !g->preview_video_path[0])
		return;
	play = video_play_fn(g);
	if (!play)
		return;
	g->preview_video_ended = 0;
	g->preview_video_awaiting = 0;
	g->preview_video_ui_shown = 1;
	ok = play(video_play_user(g), g->preview_video_path);
	if (ok != 0) {
		fprintf(stderr, "[ui] album video replay failed\n");
		preview_hide(g);
		return;
	}
	preview_arm_guard(g);
	preview_video_refresh_chrome(g, 0, g->preview_video_dur_sec, 0);
	printf("[ui] album video replay: %s\n", g->preview_video_path);
	fflush(stdout);
}

static void preview_video_toggle_play(ui_gallery_t *g)
{
	if (!g || !g->preview_video_path[0])
		return;
	if (g->preview_video_ended || !media_player_is_playing()) {
		preview_video_replay(g);
		return;
	}
	if (media_player_is_paused()) {
		media_player_set_paused(0);
		printf("[ui] album video resume\n");
	} else {
		media_player_set_paused(1);
		printf("[ui] album video pause\n");
	}
	fflush(stdout);
	{
		int el = 0, dur = 0, ended = 0, failed = 0;

		if (media_player_get_status(&el, &dur, &ended, &failed)) {
			if (dur <= 0 && g->preview_video_dur_sec > 0)
				dur = g->preview_video_dur_sec;
			preview_video_refresh_chrome(g, el, dur, ended);
		}
	}
}


static void preview_hide(ui_gallery_t *g)
{
	if (!g)
		return;
	printf("[ui] preview_hide (player stop if any)\n");
	fflush(stdout);
	preview_clear_guard(g);
	preview_video_session_clear(g);
	/*
	 * Stop via play(user, NULL) so the compiler always writes an
	 * explicit NULL into the path register. Calling a 1-arg on_still_hide
	 * that was accidentally the play hook left x1 = &play (bytes "@\\n")
	 * and started a bogus decode with still_hold stuck on — live preview
	 * froze after closing the album.
	 */
	{
		ui_gallery_video_play_fn play = video_play_fn(g);

		if (play)
			(void)play(video_play_user(g), NULL);
		else if (g->on_still_hide)
			g->on_still_hide(g->still_user);
	}
	lvgl_display_hold_blank(0);
	lvgl_display_clear_fullscreen_argb();
	/* Restore album list backdrop after HW video overlay. */
	if (g->visible && g->root)
		lv_obj_set_style_bg_opa(g->root, LV_OPA_COVER, 0);
	if (g->preview) {
		lv_obj_add_flag(g->preview, LV_OBJ_FLAG_HIDDEN);
		/* Must not stay clickable on top — steals list scrolls. */
		lv_obj_clear_flag(g->preview, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_move_background(g->preview);
	}
	if (g->preview_canvas)
		lv_obj_add_flag(g->preview_canvas, LV_OBJ_FLAG_HIDDEN);
	if (g->preview_msg)
		lv_obj_add_flag(g->preview_msg, LV_OBJ_FLAG_HIDDEN);
	if (g->preview_hint)
		lv_obj_add_flag(g->preview_hint, LV_OBJ_FLAG_HIDDEN);
	/* Restore list chrome hidden while the photo covers the FB. */
	if (g->hdr)
		lv_obj_clear_flag(g->hdr, LV_OBJ_FLAG_HIDDEN);
	if (g->list) {
		lv_obj_clear_flag(g->list, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(g->list, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_move_foreground(g->list);
	}
	if (g->exit_zone)
		lv_obj_clear_flag(g->exit_zone, LV_OBJ_FLAG_HIDDEN);
	if (g->hdr)
		lv_obj_move_foreground(g->hdr);
	if (g->close_btn)
		lv_obj_move_foreground(g->close_btn);
	if (g->exit_zone)
		lv_obj_move_foreground(g->exit_zone);
	if (g->root)
		lv_obj_invalidate(g->root);
	video_anim_sync();
}

/* RGB565 → ARGB8888; match capture / thumb (no software 180°). */
static void preview_rgb565_to_canvas(const uint16_t *src, lv_color32_t *dst,
		int w, int h)
{
	int x, y;

	if (!src || !dst || w <= 0 || h <= 0)
		return;

	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			uint16_t p = src[y * w + x];
			uint8_t r = (uint8_t)((p >> 11) << 3);
			uint8_t g8 = (uint8_t)(((p >> 5) & 0x3F) << 2);
			uint8_t b = (uint8_t)((p & 0x1F) << 3);

			dst[y * w + x] = (lv_color32_t){
				.blue = b, .green = g8, .red = r, .alpha = 0xFF
			};
		}
	}
}

/*
 * Scale CAM_THUMB → list display size into ARGB.
 * Prefer .t565 when CAM_SIDECAR=1; else decode photo/video on the fly.
 */
static int list_thumb_to_argb(const char *media_path, lv_color32_t *dst)
{
	uint16_t src[CAM_THUMB_W * CAM_THUMB_H];
	int x, y;
	int ok = -1;

	if (!media_path || !dst)
		return -1;
	if (media_still_load_list_thumb(media_path, src) == 0)
		ok = 0;
	else if (is_jpeg_name(media_path))
		ok = media_still_jpeg_to_list_thumb(media_path, src);
	else if (is_mp4_name(media_path))
		ok = media_still_video_to_list_thumb(media_path, src);
	if (ok != 0)
		return -1;

	for (y = 0; y < GALLERY_THUMB_H; y++) {
		int sy = y * CAM_THUMB_H / GALLERY_THUMB_H;

		for (x = 0; x < GALLERY_THUMB_W; x++) {
			int sx = x * CAM_THUMB_W / GALLERY_THUMB_W;
			uint16_t p = src[sy * CAM_THUMB_W + sx];
			uint8_t r = (uint8_t)((p >> 11) << 3);
			uint8_t g8 = (uint8_t)(((p >> 5) & 0x3F) << 2);
			uint8_t b = (uint8_t)((p & 0x1F) << 3);

			dst[y * GALLERY_THUMB_W + x] = (lv_color32_t){
				.blue = b, .green = g8, .red = r, .alpha = 0xFF
			};
		}
	}
	return 0;
}

/* RGB565 CAM_THUMB frame → GALLERY_THUMB ARGB canvas. */
static void thumb_rgb565_frame_to_argb(const uint16_t *src, lv_color32_t *dst)
{
	int x, y;

	if (!src || !dst)
		return;
	for (y = 0; y < GALLERY_THUMB_H; y++) {
		int sy = y * CAM_THUMB_H / GALLERY_THUMB_H;

		for (x = 0; x < GALLERY_THUMB_W; x++) {
			int sx = x * CAM_THUMB_W / GALLERY_THUMB_W;
			uint16_t p = src[sy * CAM_THUMB_W + sx];
			uint8_t r = (uint8_t)((p >> 11) << 3);
			uint8_t g8 = (uint8_t)(((p >> 5) & 0x3F) << 2);
			uint8_t b = (uint8_t)((p & 0x1F) << 3);

			dst[y * GALLERY_THUMB_W + x] = (lv_color32_t){
				.blue = b, .green = g8, .red = r, .alpha = 0xFF
			};
		}
	}
}

typedef struct list_thumb_anim {
	lv_obj_t *canvas;
	lv_color32_t *cbuf; /* nframes * thumb_w * thumb_h */
	int nframes;
	int fps;
	int idx;
	lv_timer_t *timer;
	lv_coord_t thumb_w;
	lv_coord_t thumb_h;
	int is_video_anim;
	struct list_thumb_anim *next;
} list_thumb_anim_t;

static list_thumb_anim_t *g_vthumb_anims;
static ui_gallery_t *g_gallery_anim;

#define VTHUMB_VISIBLE_MAX 32

static void video_anim_show_frame(list_thumb_anim_t *a, int idx)
{
	size_t px;
	lv_color32_t *frame;

	if (!a || !a->cbuf || !a->canvas || a->nframes <= 0)
		return;
	if (idx < 0)
		idx = 0;
	if (idx >= a->nframes)
		idx = a->nframes - 1;
	a->idx = idx;
	px = (size_t)a->thumb_w * (size_t)a->thumb_h;
	frame = a->cbuf + (size_t)idx * px;
	lv_canvas_set_buffer(a->canvas, frame, a->thumb_w, a->thumb_h,
		LV_COLOR_FORMAT_ARGB8888);
	lv_obj_invalidate(a->canvas);
}

static void video_anim_stop(list_thumb_anim_t *a)
{
	if (!a)
		return;
	if (a->timer) {
		lv_timer_del(a->timer);
		a->timer = NULL;
	}
	video_anim_show_frame(a, 0);
	if (g_vthumb_playing == a)
		g_vthumb_playing = NULL;
}

static int thumb_row_visible(lv_obj_t *canvas, lv_obj_t *list)
{
	lv_area_t ca, la;
	lv_coord_t mid;

	if (!canvas || !list)
		return 0;
	lv_obj_get_coords(canvas, &ca);
	lv_obj_get_coords(list, &la);
	mid = (ca.y1 + ca.y2) / 2;
	return mid >= la.y1 && mid <= la.y2;
}

/*
 * Visible video thumbs in user reading order (top → bottom).
 * UI_BAR_ROT180 + physical 180°: FB bottom is user top, so sort by Y descending.
 */
static int collect_visible_vthumbs(list_thumb_anim_t **out, int max)
{
	list_thumb_anim_t *a;
	lv_obj_t *list = g_gallery_anim ? g_gallery_anim->list : NULL;
	int rot180 = g_gallery_anim ? g_gallery_anim->bar_rot180 : 0;
	int n = 0;
	int i, j;

	if (!out || max <= 0 || !list)
		return 0;
	for (a = g_vthumb_anims; a; a = a->next) {
		if (!a->is_video_anim || a->nframes <= 1 || !a->canvas)
			continue;
		if (!lv_obj_is_visible(a->canvas))
			continue;
		if (!thumb_row_visible(a->canvas, list))
			continue;
		if (n < max)
			out[n++] = a;
	}
	for (i = 1; i < n; i++) {
		list_thumb_anim_t *key = out[i];
		lv_area_t ka;

		lv_obj_get_coords(key->canvas, &ka);
		for (j = i - 1; j >= 0; j--) {
			lv_area_t ja;

			lv_obj_get_coords(out[j]->canvas, &ja);
			/* rot180: larger FB Y first; else smaller FB Y first */
			if (rot180 ? (ja.y1 >= ka.y1) : (ja.y1 <= ka.y1))
				break;
			out[j + 1] = out[j];
		}
		out[j + 1] = key;
	}
	return n;
}

static void video_anim_start(list_thumb_anim_t *a);

/* After one full loop, play the next visible thumb (wrap to top). */
static void video_anim_advance_cycle(list_thumb_anim_t *finished)
{
	list_thumb_anim_t *vis[VTHUMB_VISIBLE_MAX];
	int n, i, next;

	n = collect_visible_vthumbs(vis, VTHUMB_VISIBLE_MAX);
	if (n <= 0) {
		if (finished)
			video_anim_stop(finished);
		return;
	}
	if (n == 1) {
		if (vis[0] != finished)
			video_anim_start(vis[0]);
		else if (finished)
			video_anim_show_frame(finished, 0);
		return;
	}
	next = 0;
	for (i = 0; i < n; i++) {
		if (vis[i] == finished) {
			next = (i + 1) % n;
			break;
		}
	}
	video_anim_start(vis[next]);
}

static void list_thumb_anim_tick(lv_timer_t *t)
{
	list_thumb_anim_t *a = lv_timer_get_user_data(t);
	int next;

	if (!a || !a->cbuf || !a->canvas || a->nframes <= 1)
		return;
	next = (a->idx + 1) % a->nframes;
	video_anim_show_frame(a, next);
	if (next == 0)
		video_anim_advance_cycle(a);
}

static void video_anim_start(list_thumb_anim_t *a)
{
	uint32_t period;

	if (!a || a->nframes <= 1 || !a->canvas)
		return;
	if (g_vthumb_playing && g_vthumb_playing != a)
		video_anim_stop(g_vthumb_playing);
	a->idx = 0;
	video_anim_show_frame(a, 0);
	if (!a->timer) {
		period = (uint32_t)(1000 / (a->fps > 0 ? a->fps : CAM_ANIM_FPS));
		if (period < 50)
			period = 50;
		a->timer = lv_timer_create(list_thumb_anim_tick, period, a);
	}
	g_vthumb_playing = a;
}

static void video_anim_sync(void)
{
	list_thumb_anim_t *vis[VTHUMB_VISIBLE_MAX];
	int n, i;

	n = collect_visible_vthumbs(vis, VTHUMB_VISIBLE_MAX);
	if (n <= 0) {
		if (g_vthumb_playing)
			video_anim_stop(g_vthumb_playing);
		return;
	}
	if (g_vthumb_playing) {
		for (i = 0; i < n; i++) {
			if (vis[i] == g_vthumb_playing)
				return; /* still visible: keep current in cycle */
		}
	}
	video_anim_start(vis[0]);
}

static void gallery_list_scroll_cb(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);

	if (code == LV_EVENT_SCROLL || code == LV_EVENT_SCROLL_END)
		video_anim_sync();
}

static void list_thumb_anim_unlink(list_thumb_anim_t *a)
{
	list_thumb_anim_t **pp;

	if (!a || !a->is_video_anim)
		return;
	for (pp = &g_vthumb_anims; *pp; pp = &(*pp)->next) {
		if (*pp == a) {
			*pp = a->next;
			break;
		}
	}
	a->next = NULL;
	a->is_video_anim = 0;
	if (g_vthumb_playing == a)
		g_vthumb_playing = NULL;
}

static void list_thumb_anim_free(list_thumb_anim_t *a)
{
	if (!a)
		return;
	list_thumb_anim_unlink(a);
	if (a->timer) {
		lv_timer_del(a->timer);
		a->timer = NULL;
	}
	free(a->cbuf);
	free(a);
}

static void list_thumb_buf_delete_cb(lv_event_t *e)
{
	list_thumb_anim_t *a;

	if (lv_event_get_code(e) != LV_EVENT_DELETE)
		return;
	a = lv_obj_get_user_data(lv_event_get_target(e));
	lv_obj_set_user_data(lv_event_get_target(e), NULL);
	if (a)
		list_thumb_anim_free(a);
	video_anim_sync();
}

static void format_duration(char *out, size_t out_len, int sec)
{
	int m, s;

	if (!out || out_len < 4)
		return;
	if (sec < 0)
		sec = 0;
	m = sec / 60;
	s = sec % 60;
	if (m >= 60)
		snprintf(out, out_len, "%d:%02d:%02d", m / 60, m % 60, s);
	else
		snprintf(out, out_len, "%d:%02d", m, s);
}

static void add_video_thumb_chrome(lv_obj_t *row, lv_coord_t thumb_x,
		lv_coord_t thumb_y, lv_coord_t thumb_w, lv_coord_t thumb_h,
		const char *path, int bar_rot180)
{
	lv_obj_t *pause;
	lv_obj_t *dur;
	char buf[16];
	int sec;

	if (!row || !path)
		return;

	pause = lv_label_create(row);
	lv_label_set_text(pause, LV_SYMBOL_PAUSE);
	lv_obj_set_style_text_color(pause, lv_color_hex(0xFFFFFF), 0);
	lv_obj_set_style_text_font(pause, &lv_font_montserrat_14, 0);
	lv_obj_set_style_bg_color(pause, lv_color_hex(0x000000), 0);
	lv_obj_set_style_bg_opa(pause, LV_OPA_50, 0);
	lv_obj_set_style_radius(pause, 4, 0);
	lv_obj_set_style_pad_left(pause, 4, 0);
	lv_obj_set_style_pad_right(pause, 4, 0);
	lv_obj_set_style_pad_top(pause, 2, 0);
	lv_obj_set_style_pad_bottom(pause, 2, 0);
	lv_obj_clear_flag(pause, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_update_layout(pause);
	lv_obj_set_pos(pause,
		thumb_x + (thumb_w - lv_obj_get_width(pause)) / 2,
		thumb_y + (thumb_h - lv_obj_get_height(pause)) / 2);

	sec = media_still_load_video_duration(path);
	if (sec < 0)
		sec = 0;
	format_duration(buf, sizeof(buf), sec);
	dur = lv_label_create(row);
	lv_label_set_text(dur, buf);
	lv_obj_set_style_text_color(dur, lv_color_hex(0xFFFFFF), 0);
	lv_obj_set_style_text_font(dur, &lv_font_montserrat_14, 0);
	lv_obj_set_style_bg_color(dur, lv_color_hex(0x000000), 0);
	lv_obj_set_style_bg_opa(dur, LV_OPA_60, 0);
	lv_obj_set_style_radius(dur, 3, 0);
	lv_obj_set_style_pad_left(dur, 3, 0);
	lv_obj_set_style_pad_right(dur, 3, 0);
	lv_obj_set_style_pad_top(dur, 1, 0);
	lv_obj_set_style_pad_bottom(dur, 1, 0);
	lv_obj_clear_flag(dur, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_update_layout(dur);
	lv_obj_set_pos(dur,
		thumb_x + thumb_w - lv_obj_get_width(dur) - 3,
		thumb_y + thumb_h - lv_obj_get_height(dur) - 3);

	if (bar_rot180) {
		rotate_widget_180(pause);
		rotate_widget_180(dur);
	}
}

static int list_thumb_try_anim(const char *media_path, lv_obj_t *cv,
		lv_coord_t thumb_w, lv_coord_t thumb_h)
{
	list_thumb_anim_t *a;
	uint16_t *frames = NULL;
	int nframes = 0;
	int fps = CAM_ANIM_FPS;
	size_t px;
	size_t i;

	if (!media_path || !cv || !is_mp4_name(media_path))
		return -1;
	if (media_still_load_anim_thumb(media_path, &frames, &nframes, &fps) != 0) {
		printf("[ui] album video anim miss: %s\n", media_path);
		fflush(stdout);
		return -1;
	}
	if (!frames || nframes <= 0) {
		free(frames);
		return -1;
	}
	a = calloc(1, sizeof(*a));
	if (!a) {
		free(frames);
		return -1;
	}
	px = (size_t)thumb_w * (size_t)thumb_h;
	a->cbuf = malloc(px * (size_t)nframes * sizeof(lv_color32_t));
	if (!a->cbuf) {
		free(frames);
		free(a);
		return -1;
	}
	for (i = 0; i < (size_t)nframes; i++) {
		size_t src_px = (size_t)CAM_THUMB_W * (size_t)CAM_THUMB_H;

		thumb_rgb565_frame_to_argb(frames + i * src_px,
			a->cbuf + i * px);
	}
	free(frames);
	a->canvas = cv;
	a->nframes = nframes;
	a->fps = fps > 0 ? fps : CAM_ANIM_FPS;
	a->idx = 0;
	a->thumb_w = thumb_w;
	a->thumb_h = thumb_h;
	video_anim_show_frame(a, 0);
	lv_obj_set_user_data(cv, a);
	lv_obj_add_event_cb(cv, list_thumb_buf_delete_cb, LV_EVENT_DELETE, NULL);
	if (nframes > 1) {
		a->is_video_anim = 1;
		a->next = g_vthumb_anims;
		g_vthumb_anims = a;
	}
	printf("[ui] album video anim thumb: %s (%d frames @%dfps)\n",
		media_path, nframes, a->fps);
	fflush(stdout);
	return 0;
}

static int list_thumb_set_static(const char *media_path, lv_obj_t *cv,
		lv_coord_t thumb_w, lv_coord_t thumb_h)
{
	list_thumb_anim_t *a;

	a = calloc(1, sizeof(*a));
	if (!a)
		return -1;
	a->cbuf = malloc((size_t)thumb_w * (size_t)thumb_h * sizeof(lv_color32_t));
	if (!a->cbuf) {
		free(a);
		return -1;
	}
	if (list_thumb_to_argb(media_path, a->cbuf) != 0) {
		free(a->cbuf);
		free(a);
		return -1;
	}
	a->canvas = cv;
	a->thumb_w = thumb_w;
	a->thumb_h = thumb_h;
	a->nframes = 1;
	lv_canvas_set_buffer(cv, a->cbuf, thumb_w, thumb_h,
		LV_COLOR_FORMAT_ARGB8888);
	lv_obj_set_user_data(cv, a);
	lv_obj_add_event_cb(cv, list_thumb_buf_delete_cb, LV_EVENT_DELETE, NULL);
	return 0;
}

/*
 * If the RGB565 buffer itself is circularly shifted, left columns match the
 * far-right strip. Log so we can tell buffer corruption from DRM/LVGL path.
 */
static void preview_log_wrap_probe(const uint16_t *rgb, int w, int h)
{
	int y, i, shift, match, probed;
	int best_shift = 0;
	int best_match = -1;

	if (!rgb || w < 80 || h < 40)
		return;

	/* Prefer a row inside the letterboxed photo band. */
	y = h / 2;
	for (i = h / 4; i < (h * 3) / 4; i++) {
		if (rgb[i * w + w / 2] != 0) {
			y = i;
			break;
		}
	}

	for (shift = 40; shift <= 72; shift++) {
		match = 0;
		probed = 0;
		for (i = 0; i < shift; i++) {
			uint16_t a = rgb[y * w + i];
			uint16_t b = rgb[y * w + (w - shift + i)];

			if (a == 0 && b == 0)
				continue;
			probed++;
			if (a == b)
				match++;
		}
		if (probed > 8 && match > best_match) {
			best_match = match;
			best_shift = shift;
		}
	}

	printf("[ui] preview wrap-probe y=%d best_shift=%d match=%d/%d %s\n",
		y, best_shift, best_match, best_shift,
		(best_match * 2 > best_shift) ? "BUFFER_HAS_WRAP" : "buffer_ok");
	fflush(stdout);
}

static void preview_show_photo(ui_gallery_t *g, const char *abs_path)
{
	int ok;
	const char *use_video;

	if (!g || !g->preview || !abs_path || !abs_path[0])
		return;

	/*
	 * Optional: NV12 video-plane still (CAM_PREV_VIDEO_STILL=1). Default
	 * off — DRM rejected our hb_mem NV12 FB; ARGB + unshift is reliable.
	 */
	use_video = getenv("CAM_PREV_VIDEO_STILL");
	if (g->on_still_show && use_video && use_video[0] == '1') {
		if (g->hdr)
			lv_obj_add_flag(g->hdr, LV_OBJ_FLAG_HIDDEN);
		if (g->list)
			lv_obj_add_flag(g->list, LV_OBJ_FLAG_HIDDEN);
		if (g->exit_zone)
			lv_obj_add_flag(g->exit_zone, LV_OBJ_FLAG_HIDDEN);
		if (g->preview_msg)
			lv_obj_add_flag(g->preview_msg, LV_OBJ_FLAG_HIDDEN);
		if (g->preview_hint)
			lv_obj_add_flag(g->preview_hint, LV_OBJ_FLAG_HIDDEN);
		if (g->preview_canvas)
			lv_obj_add_flag(g->preview_canvas, LV_OBJ_FLAG_HIDDEN);

		lv_obj_set_style_bg_opa(g->preview, LV_OPA_TRANSP, 0);
		lv_obj_clear_flag(g->preview, LV_OBJ_FLAG_HIDDEN);
		lv_obj_move_foreground(g->preview);

		lvgl_display_hold_blank(1);
		preview_arm_guard(g);
		ok = g->on_still_show(g->still_user, abs_path);
		if (ok == 0) {
			printf("[ui] album preview photo: %s (video-plane NV12)\n",
				abs_path);
			fflush(stdout);
			return;
		}
		fprintf(stderr,
			"[ui] album video-plane still failed, trying ARGB\n");
		lvgl_display_hold_blank(0);
	}

	if (!g->preview_rgb || !g->preview_argb)
		return;

	/* JPEG → RGB565 → ARGB DRM stamp (default unshift≈UI_OVERSCAN_R). */
	ok = media_still_decode_jpeg_rgb565(abs_path, g->preview_rgb,
		g->preview_w, g->preview_h);
	if (ok != 0)
		ok = media_still_load_rgb565_preview(abs_path, g->preview_rgb,
			g->preview_w, g->preview_h);
	if (ok != 0) {
		fprintf(stderr, "[ui] album preview: cannot open %s\n", abs_path);
		lvgl_display_clear_fullscreen_argb();
		if (g->preview_msg) {
			lv_label_set_text(g->preview_msg,
				"Cannot open photo\n(tap to close)");
			lv_obj_clear_flag(g->preview_msg, LV_OBJ_FLAG_HIDDEN);
			if (g->bar_rot180)
				rotate_widget_180(g->preview_msg);
		}
		if (g->preview_canvas)
			lv_obj_add_flag(g->preview_canvas, LV_OBJ_FLAG_HIDDEN);
		if (g->preview_hint)
			lv_obj_add_flag(g->preview_hint, LV_OBJ_FLAG_HIDDEN);
		lv_obj_clear_flag(g->preview, LV_OBJ_FLAG_HIDDEN);
		lv_obj_move_foreground(g->preview);
		return;
	}

	preview_log_wrap_probe(g->preview_rgb, g->preview_w, g->preview_h);
	preview_rgb565_to_canvas(g->preview_rgb, g->preview_argb,
		g->preview_w, g->preview_h);

	if (g->hdr)
		lv_obj_add_flag(g->hdr, LV_OBJ_FLAG_HIDDEN);
	if (g->list)
		lv_obj_add_flag(g->list, LV_OBJ_FLAG_HIDDEN);
	if (g->exit_zone)
		lv_obj_add_flag(g->exit_zone, LV_OBJ_FLAG_HIDDEN);
	if (g->preview_msg) {
		lv_obj_set_style_transform_angle(g->preview_msg, 0, 0);
		lv_obj_add_flag(g->preview_msg, LV_OBJ_FLAG_HIDDEN);
	}
	if (g->preview_hint) {
		lv_obj_set_style_transform_angle(g->preview_hint, 0, 0);
		lv_obj_add_flag(g->preview_hint, LV_OBJ_FLAG_HIDDEN);
	}
	if (g->preview_canvas) {
		lv_obj_set_style_transform_angle(g->preview_canvas, 0, 0);
		lv_obj_add_flag(g->preview_canvas, LV_OBJ_FLAG_HIDDEN);
	}

	lv_obj_set_style_bg_opa(g->preview, LV_OPA_TRANSP, 0);
	lv_obj_clear_flag(g->preview, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_foreground(g->preview);
	preview_arm_guard(g);
	lvgl_display_set_fullscreen_argb(g->preview_argb, g->preview_w,
		g->preview_h);
	printf("[ui] album preview photo: %s (ARGB + scanout unshift)\n",
		abs_path);
	fflush(stdout);
}

static void preview_show_video_poster(ui_gallery_t *g, const char *abs_path)
{
	int ok;

	if (!g || !g->preview || !abs_path || !abs_path[0])
		return;
	if (!g->preview_rgb || !g->preview_argb)
		return;

	preview_arm_guard(g);

	if (g->hdr)
		lv_obj_add_flag(g->hdr, LV_OBJ_FLAG_HIDDEN);
	if (g->list)
		lv_obj_add_flag(g->list, LV_OBJ_FLAG_HIDDEN);
	if (g->exit_zone)
		lv_obj_add_flag(g->exit_zone, LV_OBJ_FLAG_HIDDEN);
	if (g->preview_msg)
		lv_obj_add_flag(g->preview_msg, LV_OBJ_FLAG_HIDDEN);
	if (g->preview_canvas)
		lv_obj_add_flag(g->preview_canvas, LV_OBJ_FLAG_HIDDEN);
	if (g->preview_hint) {
		lv_label_set_text(g->preview_hint, "Tap to close");
		lv_obj_clear_flag(g->preview_hint, LV_OBJ_FLAG_HIDDEN);
		if (g->bar_rot180)
			rotate_widget_180(g->preview_hint);
		else
			lv_obj_set_style_transform_angle(g->preview_hint, 0, 0);
	}

	lv_obj_set_style_bg_opa(g->preview, LV_OPA_TRANSP, 0);
	lv_obj_clear_flag(g->preview, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_foreground(g->preview);
	lv_obj_clear_flag(g->preview, LV_OBJ_FLAG_CLICKABLE);
	if (g->visible && g->root)
		lv_obj_set_style_bg_opa(g->root, LV_OPA_COVER, 0);

	ok = media_still_decode_video_rgb565(abs_path, g->preview_rgb,
		g->preview_w, g->preview_h);
	if (ok != 0) {
		fprintf(stderr, "[ui] album video preview: cannot open %s\n",
			abs_path);
		lvgl_display_clear_fullscreen_argb();
		if (g->preview_msg) {
			lv_label_set_text(g->preview_msg,
				"Cannot open video\n(tap to close)");
			lv_obj_clear_flag(g->preview_msg, LV_OBJ_FLAG_HIDDEN);
			if (g->bar_rot180)
				rotate_widget_180(g->preview_msg);
		}
		if (g->preview_hint)
			lv_obj_add_flag(g->preview_hint, LV_OBJ_FLAG_HIDDEN);
		lv_obj_clear_flag(g->preview, LV_OBJ_FLAG_HIDDEN);
		lv_obj_move_foreground(g->preview);
		return;
	}

	preview_log_wrap_probe(g->preview_rgb, g->preview_w, g->preview_h);
	preview_rgb565_to_canvas(g->preview_rgb, g->preview_argb,
		g->preview_w, g->preview_h);

	if (g->preview_msg) {
		lv_obj_set_style_transform_angle(g->preview_msg, 0, 0);
		lv_obj_add_flag(g->preview_msg, LV_OBJ_FLAG_HIDDEN);
	}
	if (g->preview_hint)
		lv_obj_add_flag(g->preview_hint, LV_OBJ_FLAG_HIDDEN);
	if (g->preview_canvas) {
		lv_obj_set_style_transform_angle(g->preview_canvas, 0, 0);
		lv_obj_add_flag(g->preview_canvas, LV_OBJ_FLAG_HIDDEN);
	}

	lv_obj_set_style_bg_opa(g->preview, LV_OPA_TRANSP, 0);
	lv_obj_clear_flag(g->preview, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_foreground(g->preview);
	lvgl_display_set_fullscreen_argb(g->preview_argb, g->preview_w,
		g->preview_h);
	printf("[ui] album preview video: %s (poster ARGB fallback)\n",
		abs_path);
	fflush(stdout);
}

static void preview_show_video(ui_gallery_t *g, const char *abs_path)
{
	int ok;
	ui_gallery_video_play_fn play;

	if (!g || !g->preview || !abs_path || !abs_path[0])
		return;

	/* Arm before showing overlay so the opening tap cannot dismiss. */
	preview_arm_guard(g);

	if (g_vthumb_playing)
		video_anim_stop(g_vthumb_playing);

	/*
	 * Start HW play first (poster on video plane) while album chrome is
	 * still opaque — then blank/hide. Blanking first exposed live preview
	 * and looked like a sudden rotation at open.
	 */
	play = video_play_fn(g);
	if (!play) {
		printf("[ui] album video: no HW play hook (g=%p static=%p) "
			"— poster only\n",
			(void *)g->on_video_play, (void *)g_video_play_fn);
		fflush(stdout);
	} else {
		printf("[ui] album video: calling HW play %s\n", abs_path);
		fflush(stdout);
		ok = play(video_play_user(g), abs_path);
		if (ok == 0) {
			int dur;

			snprintf(g->preview_video_path,
				sizeof(g->preview_video_path), "%s", abs_path);
			dur = media_still_load_video_duration(abs_path);
			g->preview_video_dur_sec = dur > 0 ? dur : 0;
			g->preview_video_ended = 0;
			g->preview_exit_visible = 0;
			g->preview_video_ui_shown = 0;
			g->preview_video_awaiting = 1;
			g->preview_video_wait_ms = lv_tick_get();
			if (g->preview_exit_timer) {
				lv_timer_del(g->preview_exit_timer);
				g->preview_exit_timer = NULL;
			}

			/*
			 * Leave album list opaque while decode warms up. Poster
			 * and the first frame swap under the list; reveal chrome
			 * only after frames_shown>=1 (or wait timeout).
			 */
			if (g->preview_video_timer)
				lv_timer_del(g->preview_video_timer);
			g->preview_video_timer = lv_timer_create(
				preview_video_timer_cb, PREVIEW_VIDEO_WAIT_MS, g);

			printf("[ui] album play video: %s (wait first frame)\n",
				abs_path);
			fflush(stdout);
			return;
		}
		printf("[ui] album HW play failed (ret=%d), falling back to poster\n",
			ok);
		fflush(stdout);
	}

	preview_show_video_poster(g, abs_path);
}

static void preview_dismiss_cb(lv_event_t *e);
static int point_in_obj_exact(lv_obj_t *obj, const lv_point_t *p);
static void gallery_touch_adj(const lv_point_t *raw, lv_point_t *adj);
static lv_obj_t *gallery_row_del_btn(lv_obj_t *row);
static int gallery_do_delete(ui_gallery_t *g, lv_obj_t *del_btn,
		lv_indev_t *indev, const char *via);
static int gallery_point_on_close(ui_gallery_t *g, const lv_point_t *raw,
		const lv_point_t *adj);
static int gallery_x_in_del_column(lv_obj_t *row, lv_coord_t x);

static void preview_dismiss_cb(lv_event_t *e)
{
	ui_gallery_t *g = lv_event_get_user_data(e);
	lv_indev_t *indev;
	lv_point_t p;
	lv_point_t p_adj;
	int hit;

	/* PRESSED: same jitter issue as Del/Close on CLICKED. */
	if (lv_event_get_code(e) != LV_EVENT_PRESSED)
		return;
	if (!g || !g->visible)
		return;
	if (g->preview_guard_armed) {
		printf("[ui] preview tap ignored (open guard)\n");
		fflush(stdout);
		return;
	}

	/* Photo still: tap closes preview. */
	if (!g->preview_video_path[0]) {
		printf("[ui] preview dismiss\n");
		fflush(stdout);
		preview_hide(g);
		return;
	}

	indev = lv_indev_get_act();
	if (!indev)
		return;
	lv_indev_get_point(indev, &p);
	/*
	 * Same digitizer Y warp as album list (UI_GALLERY_YCAL). Raw LVGL
	 * points miss the chrome paint/hit boxes by ~150–200px near the
	 * user-top / FB-bottom half — Exit toggled but never hit.
	 */
	gallery_touch_adj(&p, &p_adj);
	hit = lvgl_display_video_chrome_hit((int)p_adj.x, (int)p_adj.y);

	if (hit == LVGL_VCHROME_PLAY) {
		printf("[ui] album video transport tap\n");
		fflush(stdout);
		preview_video_toggle_play(g);
		/* Keep Exit visible a bit longer if it was up. */
		if (g->preview_exit_visible)
			preview_exit_arm_autohide(g);
		return;
	}
	if (hit == LVGL_VCHROME_EXIT) {
		printf("[ui] album video Exit\n");
		fflush(stdout);
		preview_hide(g);
		return;
	}

	/* Blank: toggle left-frame back "<" (does not close). */
	printf("[ui] album video chrome miss raw=(%d,%d) adj=(%d,%d) hit=%d\n",
		(int)p.x, (int)p.y, (int)p_adj.x, (int)p_adj.y, hit);
	fflush(stdout);
	preview_exit_toggle(g);
}

static const char *gallery_row_path(lv_obj_t *row)
{
	const char *p;

	if (!row)
		return NULL;
	p = lv_obj_get_user_data(row);
	if (p && p[0])
		return p;
	return NULL;
}

/*
 * This panel's digitizer Y is non-linear after global stretch: aiming at
 * visual row N often reports FB Y in row N+1/+2 (labeled session 2026-07-20).
 * Remap reported → visual Y for album hit only (ctrl bar unchanged).
 * Disable: UI_GALLERY_YCAL=0. Extra bias: UI_GALLERY_HIT_DY.
 *
 * Product policy: album open/delete MUST use this calibrated Y + row geometry.
 * Never trust LVGL's raw widget hit for which row — that is the HIT mismatch.
 */
static lv_coord_t gallery_calibrate_touch_y(lv_coord_t y)
{
	/* reported FB Y → intended visual Y (row centers from labeled taps) */
	static const int rp[] = {
		150, 290, 458, 558, 710, 843, 977, 1138, 1268
	};
	static const int cp[] = {
		135, 225, 315, 405, 675, 765, 855, 945, 1035
	};
	const int n = (int)(sizeof(rp) / sizeof(rp[0]));
	int i;
	int64_t num;
	int den;
	int out;

	if (!env_on("UI_GALLERY_YCAL", 1))
		return y;

	if (y <= rp[0]) {
		/* Extrapolate first segment */
		den = rp[1] - rp[0];
		if (den == 0)
			return (lv_coord_t)cp[0];
		num = (int64_t)(y - rp[0]) * (cp[1] - cp[0]);
		out = cp[0] + (int)(num / den);
		return (lv_coord_t)out;
	}
	if (y >= rp[n - 1]) {
		den = rp[n - 1] - rp[n - 2];
		if (den == 0)
			return (lv_coord_t)cp[n - 1];
		num = (int64_t)(y - rp[n - 1]) * (cp[n - 1] - cp[n - 2]);
		out = cp[n - 1] + (int)(num / den);
		return (lv_coord_t)out;
	}
	for (i = 0; i < n - 1; i++) {
		if (y >= rp[i] && y <= rp[i + 1]) {
			den = rp[i + 1] - rp[i];
			if (den == 0)
				return (lv_coord_t)cp[i];
			num = (int64_t)(y - rp[i]) * (cp[i + 1] - cp[i]);
			out = cp[i] + (int)(num / den);
			return (lv_coord_t)out;
		}
	}
	return y;
}

/* Which list row contains screen point p; if in pad gap, nearest row by Y. */
static lv_obj_t *gallery_row_at_point(ui_gallery_t *g, const lv_point_t *p)
{
	uint32_t n;
	uint32_t i;
	lv_obj_t *best = NULL;
	lv_coord_t best_d = 0x7fff;

	if (!g || !g->list || !p)
		return NULL;
	n = lv_obj_get_child_cnt(g->list);
	for (i = 0; i < n; i++) {
		lv_obj_t *row = lv_obj_get_child(g->list, i);
		lv_area_t a;
		lv_coord_t cy;
		lv_coord_t d;

		if (!row)
			continue;
		/* Skip non-row children (empty-state label etc.). */
		if (!gallery_row_path(row) && lv_obj_get_height(row) != GALLERY_ROW_H)
			continue;
		lv_obj_get_coords(row, &a);
		if (p->x >= a.x1 && p->x <= a.x2 &&
		    p->y >= a.y1 && p->y <= a.y2)
			return row;
		/* Prefer nearest row when finger lands in inter-row pad. */
		if (p->x < a.x1 || p->x > a.x2)
			continue;
		cy = (lv_coord_t)((a.y1 + a.y2) / 2);
		d = (lv_coord_t)(p->y > cy ? (p->y - cy) : (cy - p->y));
		if (d < best_d) {
			best_d = d;
			best = row;
		}
	}
	/* Only absorb small gaps (~pad), not distant misses. */
	if (best && best_d <= (GALLERY_ROW_H / 2 + GALLERY_PAD))
		return best;
	return NULL;
}

static const char *gallery_basename(const char *path)
{
	const char *s;

	if (!path || !path[0])
		return "(null)";
	s = strrchr(path, '/');
	return s ? s + 1 : path;
}

/* 1 if X is in this row's Del column (with fat-finger pad). */
static int gallery_x_in_del_column(lv_obj_t *row, lv_coord_t x)
{
	lv_obj_t *del;
	lv_area_t a;
	const int pad = 8;

	del = gallery_row_del_btn(row);
	if (!del)
		return 0;
	lv_obj_get_click_area(del, &a);
	return (x >= a.x1 - pad && x <= a.x2 + pad);
}

/*
 * Dump every row vs touch Y. idx=1 is FB-top (= user-bottom with BAR_ROT180).
 * Enable: UI_GALLERY_HIT_DEBUG=1.
 */
static void gallery_dump_rows(ui_gallery_t *g, const lv_point_t *p,
		lv_obj_t *chosen)
{
	uint32_t n;
	uint32_t i;
	int idx = 0;
	lv_coord_t scroll_y = 0;

	if (!env_on("UI_GALLERY_HIT_DEBUG", 0))
		return;
	if (!g || !g->list || !p)
		return;
	scroll_y = lv_obj_get_scroll_y(g->list);
	n = lv_obj_get_child_cnt(g->list);
	printf("[ui] album HIT dump touch=(%d,%d) scroll_y=%d children=%u "
		"hit_dy=%d ycal=%d\n",
		(int)p->x, (int)p->y, (int)scroll_y, (unsigned)n,
		env_int("UI_GALLERY_HIT_DY", 0),
		env_on("UI_GALLERY_YCAL", 1));
	for (i = 0; i < n; i++) {
		lv_obj_t *row = lv_obj_get_child(g->list, i);
		lv_area_t a;
		const char *path;
		int in_row;
		int is_ch;

		if (!row)
			continue;
		path = gallery_row_path(row);
		if (!path && lv_obj_get_height(row) != GALLERY_ROW_H)
			continue;
		lv_obj_get_coords(row, &a);
		idx++;
		in_row = (p->y >= a.y1 && p->y <= a.y2);
		is_ch = (row == chosen);
		printf("  [%2d] y=%4d..%-4d cy=%4d  %s%s%s\n",
			idx, (int)a.y1, (int)a.y2, (int)((a.y1 + a.y2) / 2),
			gallery_basename(path),
			in_row ? "  <== TOUCH" : "",
			is_ch ? "  <== OPEN" : "");
	}
	fflush(stdout);
}

/*
 * Resolve album tap by calibrated geometry only (ignore LVGL widget hit).
 * Returns 1 if a preview was opened or a delete/close ran.
 */
static int gallery_open_at_touch(ui_gallery_t *g, lv_obj_t *hit_widget,
		int geom_only)
{
	lv_obj_t *row;
	const char *path;
	lv_indev_t *indev;
	lv_point_t p;
	lv_point_t p_adj;
	lv_area_t ra;
	int hit_dy;
	char safe[PATH_MAX];

	(void)hit_widget;
	(void)geom_only;

	if (!g || !g->visible)
		return 0;

	indev = lv_indev_get_act();
	if (!indev)
		return 0;
	lv_indev_get_point(indev, &p);

	/*
	 * Album-only Y fix (does not move camera buttons):
	 * 1) UI_GALLERY_YCAL (default on): piecewise remap for this panel
	 * 2) UI_GALLERY_HIT_DY: extra constant bias after remap
	 */
	hit_dy = env_int("UI_GALLERY_HIT_DY", 0);
	p_adj = p;
	p_adj.y = gallery_calibrate_touch_y(p.y);
	p_adj.y = (lv_coord_t)(p_adj.y + hit_dy);

	if (gallery_point_on_close(g, &p, &p_adj)) {
		printf("[ui] album close (ycal/hdr catch @raw=%d,%d adj=%d,%d)\n",
			(int)p.x, (int)p.y, (int)p_adj.x, (int)p_adj.y);
		fflush(stdout);
		if (indev)
			lv_indev_wait_release(indev);
		ui_gallery_hide(g);
		return 1;
	}

	row = gallery_row_at_point(g, &p_adj);
	if (!row || !gallery_row_path(row))
		return 0;

	gallery_dump_rows(g, &p_adj, row);

	/* Del column on the visual row — even if LVGL hit a neighbor. */
	{
		lv_obj_t *del = gallery_row_del_btn(row);

		if (del && (gallery_x_in_del_column(row, p_adj.x) ||
				gallery_x_in_del_column(row, p.x))) {
			printf("[ui] album Del column catch "
				"raw=(%d,%d) adj=(%d,%d)\n",
				(int)p.x, (int)p.y,
				(int)p_adj.x, (int)p_adj.y);
			fflush(stdout);
			return gallery_do_delete(g, del, indev, "Del column");
		}
	}

	path = gallery_row_path(row);
	if (!path || !path[0])
		return 0;

	ra.x1 = ra.y1 = ra.x2 = ra.y2 = 0;
	lv_obj_get_coords(row, &ra);
	printf("[ui] album open: %s raw_touch=(%d,%d) adj=(%d,%d) row_y=[%d..%d]\n",
		path, (int)p.x, (int)p.y, (int)p_adj.x, (int)p_adj.y,
		(int)ra.y1, (int)ra.y2);
	fflush(stdout);

	if (media_still_confine_path(g->save_dir, path, safe, sizeof(safe),
			0) != 0) {
		fprintf(stderr,
			"[ui] album open rejected (outside save_dir): %s\n",
			path);
		fflush(stderr);
		return 0;
	}

	lv_indev_wait_release(indev);

	if (is_jpeg_name(safe))
		preview_show_photo(g, safe);
	else if (is_mp4_name(safe))
		preview_show_video(g, safe);
	return 1;
}

static void open_item_cb(lv_event_t *e)
{
	ui_gallery_t *g = lv_event_get_user_data(e);
	lv_event_code_t code = lv_event_get_code(e);
	lv_indev_t *indev;
	lv_point_t p;
	lv_point_t p_adj;
	lv_obj_t *row;
	lv_obj_t *del;

	gallery_ptr_track(code);
	if (!g)
		return;

	/*
	 * Del on PRESSED (CLICKED often lost to jitter). Always use calibrated
	 * geometry — never the LVGL-hit Del of a neighbor row.
	 */
	if (code == LV_EVENT_PRESSED) {
		indev = lv_indev_get_act();
		if (!indev)
			return;
		lv_indev_get_point(indev, &p);
		gallery_touch_adj(&p, &p_adj);
		row = gallery_row_at_point(g, &p_adj);
		del = row ? gallery_row_del_btn(row) : NULL;
		if (del && gallery_x_in_del_column(row, p_adj.x)) {
			(void)gallery_do_delete(g, del, indev, "Del geom");
			return;
		}
		return;
	}

	/* CLICKED: drag-to-scroll must win over open. */
	if (code != LV_EVENT_CLICKED)
		return;
	if (gallery_click_is_drag())
		return;
	(void)gallery_open_at_touch(g, NULL, 1);
}

/*
 * Stretch maps taps on visual row 11/12 into the cream Album hdr (FB bottom).
 * Catch those presses and open via Y-calibrated geometry.
 */
static void gallery_hdr_press_cb(lv_event_t *e)
{
	ui_gallery_t *g = lv_event_get_user_data(e);
	lv_event_code_t code = lv_event_get_code(e);

	gallery_ptr_track(code);
	if (code != LV_EVENT_CLICKED)
		return;
	if (!g)
		return;
	if (gallery_click_is_drag())
		return;
	(void)gallery_open_at_touch(g, NULL, 1);
}

static void gallery_list_press_cb(lv_event_t *e)
{
	ui_gallery_t *g = lv_event_get_user_data(e);
	lv_event_code_t code = lv_event_get_code(e);

	gallery_ptr_track(code);
	if (code != LV_EVENT_CLICKED)
		return;
	if (!g)
		return;
	/* Ignore click that finished a scroll/drag gesture. */
	if (gallery_click_is_drag())
		return;
	/* List chrome / pad gaps — geometry only so we don't steal Del. */
	(void)gallery_open_at_touch(g, NULL, 1);
}

static int is_media_name(const char *name)
{
	size_t n;

	if (!name)
		return 0;
	n = strlen(name);
	if (n < 5)
		return 0;
	/* Skip REC poster stills (foo.poster.jpg) — not standalone album items. */
	if (n >= 11 && strcasecmp(name + n - 11, ".poster.jpg") == 0)
		return 0;
	if (strcasecmp(name + n - 4, ".jpg") == 0)
		return 1;
	if (strcasecmp(name + n - 4, ".mp4") == 0)
		return 1;
	if (strcasecmp(name + n - 5, ".jpeg") == 0)
		return 1;
	return 0;
}

static int cmp_mtime_desc(const void *a, const void *b)
{
	const gallery_entry_t *ea = a;
	const gallery_entry_t *eb = b;

	if (ea->mtime > eb->mtime)
		return -1;
	if (ea->mtime < eb->mtime)
		return 1;
	return 0;
}

/*
 * Walk the whole save dir (do not stop early). Keep the newest keep_max
 * entries in out[]; return how many were kept. *total_out = all media
 * files on disk (accurate Album X/Y denominator).
 *
 * Old bug: stop after maxn readdir hits → newest REC can be missing when
 * the folder is large, and title total was capped at GALLERY_SCAN_MAX.
 */
static int scan_entries(const char *dir, gallery_entry_t *out, int keep_max,
		int *total_out)
{
	DIR *d;
	struct dirent *de;
	struct stat st;
	char path[256];
	int kept = 0;
	int total = 0;
	gallery_entry_t cur;

	if (total_out)
		*total_out = 0;
	if (!out || keep_max < 1)
		return 0;

	d = opendir(dir);
	if (!d) {
		fprintf(stderr, "[ui] album opendir(%s) failed: %s\n",
			dir ? dir : "(null)", strerror(errno));
		return 0;
	}

	while ((de = readdir(d)) != NULL) {
		if (!is_media_name(de->d_name))
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		/*
		 * Skip escaping symlinks / odd paths — only list files whose
		 * realpath stays under save_dir.
		 */
		if (media_still_confine_path(dir, path, cur.path,
				sizeof(cur.path), 0) != 0)
			continue;
		if (stat(cur.path, &st) != 0 || !S_ISREG(st.st_mode))
			continue;

		total++;
		snprintf(cur.name, sizeof(cur.name), "%s", de->d_name);
		cur.mtime = st.st_mtime;
		cur.size = st.st_size;

		if (kept < keep_max) {
			out[kept++] = cur;
			if (kept == keep_max)
				qsort(out, (size_t)kept, sizeof(out[0]),
					cmp_mtime_desc);
			continue;
		}

		/* out[] sorted newest-first; replace oldest if this is newer. */
		if (cur.mtime >= out[keep_max - 1].mtime) {
			out[keep_max - 1] = cur;
			qsort(out, (size_t)keep_max, sizeof(out[0]),
				cmp_mtime_desc);
		}
	}
	closedir(d);

	if (kept > 1 && kept < keep_max)
		qsort(out, (size_t)kept, sizeof(out[0]), cmp_mtime_desc);

	if (total_out)
		*total_out = total;
	return kept;
}

static void gallery_refresh(ui_gallery_t *g);

void ui_gallery_set_save_dir(ui_gallery_t *g, const char *dir)
{
	const char *fallback = "/userdata/lvgl_camera";
	struct stat st;

	if (!g)
		return;

	/*
	 * Prefer an explicit absolute path (pipeline save_dir). Only fall back
	 * to CAM_SAVE_DIR / board defaults when dir is missing — never discard
	 * a valid pipeline path by calling this with NULL after the button set it.
	 */
	if (!dir || !dir[0] || dir[0] != '/') {
		dir = getenv("CAM_SAVE_DIR");
		if (!dir || !dir[0] || dir[0] != '/') {
			if (stat("/userdata", &st) == 0 && S_ISDIR(st.st_mode))
				dir = "/userdata/lvgl_camera";
			else
				dir = "/tmp/lvgl_camera";
		}
	}
	snprintf(g->save_dir, sizeof(g->save_dir), "%s", dir);

	/* Create if missing (same as camera_pipeline ensure_save_dir). */
	if (g->save_dir[0] == '/' && access(g->save_dir, F_OK) != 0) {
		if (mkdir(g->save_dir, 0755) != 0 && errno != EEXIST)
			fprintf(stderr, "[ui] album mkdir %s failed: %s\n",
				g->save_dir, strerror(errno));
	}

	/*
	 * Only redirect if still not a usable directory — do not treat "not
	 * created yet" as "use a different disk" without trying mkdir first.
	 */
	if (g->save_dir[0] != '/' ||
			stat(g->save_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
		fprintf(stderr, "[ui] album save_dir invalid '%s', using %s\n",
			g->save_dir, fallback);
		snprintf(g->save_dir, sizeof(g->save_dir), "%s", fallback);
		(void)mkdir(g->save_dir, 0755);
	}
}

void ui_gallery_set_still_ops(ui_gallery_t *g, ui_gallery_still_show_fn show,
		ui_gallery_still_hide_fn hide, void *user)
{
	if (!g)
		return;
	g->on_still_show = show;
	g->on_still_hide = hide;
	g->still_user = user;
	g_video_play_user = user;
}

void ui_gallery_set_video_play(ui_gallery_t *g, ui_gallery_video_play_fn play)
{
	if (!g)
		return;
	g->on_video_play = play;
	g_video_play_fn = play;
	printf("[ui] album HW play hook %s\n", play ? "set" : "cleared");
	fflush(stdout);
}

/* Short lists: keep rows next to Album/Close (user top). Tall lists must use
 * FLEX_ALIGN_START — END packs overflow content and breaks vertical scrolling. */
static void gallery_pin_near_hdr(ui_gallery_t *g)
{
	lv_coord_t bottom;

	if (!g || !g->list)
		return;

	lv_obj_set_style_pad_top(g->list, GALLERY_PAD, 0);

	if (!g->bar_rot180) {
		lv_obj_set_flex_align(g->list, LV_FLEX_ALIGN_START,
			LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
		lv_obj_update_layout(g->list);
		lv_obj_scroll_to_y(g->list, 0, LV_ANIM_OFF);
		return;
	}

	/* Oldest-first: newest sits at the content bottom (near FB hdr). */
	lv_obj_set_flex_align(g->list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
		LV_FLEX_ALIGN_START);
	lv_obj_update_layout(g->list);
	bottom = lv_obj_get_scroll_bottom(g->list);
	if (bottom > 0 && bottom < 8192) {
		/* Overflow: start scrolled to hdr / newest; user can still fling. */
		lv_obj_scroll_by(g->list, 0, -bottom, LV_ANIM_OFF);
	} else {
		/* Fits without scrolling — pack toward hdr (user top). */
		lv_obj_set_flex_align(g->list, LV_FLEX_ALIGN_END,
			LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
		lv_obj_update_layout(g->list);
	}
}

static void del_btn_delete_cb(lv_event_t *e)
{
	char *path;

	if (lv_event_get_code(e) != LV_EVENT_DELETE)
		return;
	path = lv_obj_get_user_data(lv_event_get_target(e));
	free(path);
	lv_obj_set_user_data(lv_event_get_target(e), NULL);
}

static int point_in_obj_exact(lv_obj_t *obj, const lv_point_t *p)
{
	/* Optional 2px inset; Del also has a small ext_click_area for fat finger. */
	lv_area_t a;

	if (!obj || !p)
		return 0;
	lv_obj_get_click_area(obj, &a);
	/* radius=0 path of lv_area_is_point_on (private in v9) */
	return (p->x >= a.x1 && p->x <= a.x2 && p->y >= a.y1 && p->y <= a.y2);
}

static void gallery_touch_adj(const lv_point_t *raw, lv_point_t *adj)
{
	int hit_dy = env_int("UI_GALLERY_HIT_DY", 0);

	*adj = *raw;
	adj->y = gallery_calibrate_touch_y(raw->y);
	adj->y = (lv_coord_t)(adj->y + hit_dy);
}

static lv_obj_t *gallery_row_del_btn(lv_obj_t *row)
{
	uint32_t i;
	uint32_t n;

	if (!row)
		return NULL;
	n = lv_obj_get_child_cnt(row);
	for (i = 0; i < n; i++) {
		lv_obj_t *c = lv_obj_get_child(row, i);

		if (!c)
			continue;
		if (lv_obj_get_width(c) == GALLERY_DEL_W &&
		    lv_obj_get_height(c) == GALLERY_DEL_H)
			return c;
	}
	return NULL;
}

static int gallery_do_delete(ui_gallery_t *g, lv_obj_t *del_btn,
		lv_indev_t *indev, const char *via)
{
	const char *path;
	char safe[PATH_MAX];

	if (!g || !del_btn)
		return 0;
	path = lv_obj_get_user_data(del_btn);
	if (!path || !path[0])
		return 0;
	if (media_still_confine_path(g->save_dir, path, safe, sizeof(safe),
			1) != 0) {
		fprintf(stderr,
			"[ui] album del rejected (outside save_dir): %s\n",
			path);
		fflush(stderr);
		return 0;
	}

	/* Press flash (light red + shrink) before list rebuild. */
	lv_obj_add_state(del_btn, LV_STATE_PRESSED);
	lv_obj_invalidate(del_btn);
	lv_obj_move_foreground(del_btn);
	lv_refr_now(NULL);

	printf("[ui] album del: %s (%s)\n", safe, via ? via : "tap");
	fflush(stdout);
	if (unlink(safe) != 0)
		perror(safe);
	media_still_remove_rgb565_preview(safe);
	media_still_remove_list_thumb(safe);
	media_still_remove_video_poster(safe);
	media_still_remove_anim_thumb(safe);
	media_still_remove_video_duration(safe);
	if (indev)
		lv_indev_wait_release(indev);
	gallery_refresh(g);
	return 1;
}

static int gallery_point_on_close(ui_gallery_t *g, const lv_point_t *raw,
		const lv_point_t *adj)
{
	lv_area_t a;
	lv_coord_t scr_h;

	if (!g || !g->close_btn || !raw)
		return 0;
	if (point_in_obj_exact(g->close_btn, raw))
		return 1;
	if (adj && point_in_obj_exact(g->close_btn, adj))
		return 1;
	/*
	 * Stretch overshoot: finger aimed at Close but raw Y falls into cream
	 * below the button (same column). Keep a tight X band so we do not
	 * steal row opens on the other side.
	 */
	lv_obj_get_click_area(g->close_btn, &a);
	scr_h = lv_disp_get_ver_res(NULL);
	if (scr_h < 1)
		scr_h = 1280;
	if (raw->x >= a.x1 - 4 && raw->x <= a.x2 + 4 &&
	    raw->y > a.y2 && raw->y < scr_h &&
	    raw->y <= a.y2 + 100)
		return 1;
	return 0;
}

static void close_btn_cb(lv_event_t *e)
{
	ui_gallery_t *g = lv_event_get_user_data(e);
	lv_obj_t *btn = lv_event_get_current_target(e);
	lv_indev_t *indev;
	lv_point_t p;
	lv_point_t p_adj;

	/* PRESSED: CLICKED is often lost to finger jitter on this panel. */
	if (lv_event_get_code(e) != LV_EVENT_PRESSED)
		return;
	if (!g || !g->visible || !btn)
		return;
	if (btn != g->close_btn)
		return;
	indev = lv_indev_get_act();
	if (!indev)
		return;
	lv_indev_get_point(indev, &p);
	gallery_touch_adj(&p, &p_adj);
	if (!gallery_point_on_close(g, &p, &p_adj)) {
		printf("[ui] album close ignored (outside Close @%d,%d)\n",
			(int)p.x, (int)p.y);
		return;
	}
	printf("[ui] album close\n");
	fflush(stdout);
	lv_obj_add_state(btn, LV_STATE_PRESSED);
	lv_obj_invalidate(btn);
	lv_indev_wait_release(indev);
	ui_gallery_hide(g);
}

/*
 * Top ~10% is hard to touch on the product. Double-tap the middle of the
 * user-bottom strip (caption end) to leave the album.
 */
static void exit_zone_cb(lv_event_t *e)
{
	ui_gallery_t *g = lv_event_get_user_data(e);
	uint32_t now;

	if (lv_event_get_code(e) != LV_EVENT_PRESSED)
		return;
	if (!g || !g->visible)
		return;

	now = lv_tick_get();
	if (g->exit_tap_ms != 0 &&
	    lv_tick_elaps(g->exit_tap_ms) <= GALLERY_EXIT_DBL_MS) {
		g->exit_tap_ms = 0;
		printf("[ui] album close (double-tap bottom)\n");
		fflush(stdout);
		ui_gallery_hide(g);
		return;
	}
	g->exit_tap_ms = now;
}

static void gallery_refresh(ui_gallery_t *g)
{
	static gallery_entry_t entries[GALLERY_SCAN_MAX];
	int total;
	int show;
	int i;
	char line[160];
	lv_coord_t row_w;
	lv_coord_t content_w;
	lv_coord_t scr_w = lv_disp_get_hor_res(NULL);

	if (!g || !g->list) {
		fprintf(stderr, "[ui] album refresh: bad gallery (g=%p list=%p)\n",
			(void *)g, g ? (void *)g->list : NULL);
		return;
	}

	printf("[ui] album refresh...\n");
	fflush(stdout);

	if (g_vthumb_playing)
		video_anim_stop(g_vthumb_playing);
	g_gallery_anim = g;
	lv_obj_clean(g->list);
	preview_hide(g);
	/* Reset pin padding from previous short list. */
	lv_obj_set_style_pad_top(g->list, GALLERY_PAD, 0);
	if (g->max_items < 1)
		g->max_items = 1;
	if (g->max_items > GALLERY_SCAN_MAX)
		g->max_items = GALLERY_SCAN_MAX;
	/*
	 * Keep newest max_items from a full-directory scan. total = all media
	 * on disk (not capped). show = rows we actually list.
	 */
	show = scan_entries(g->save_dir, entries, g->max_items, &total);
	printf("[ui] album scan dir=%s total=%d show=%d (cap=%d)\n",
		g->save_dir, total, show, g->max_items);
	fflush(stdout);

	if (g->title) {
		lv_label_set_text_fmt(g->title, "Photos  %d/%d", show, total);
		if (g->bar_rot180)
			rotate_widget_180(g->title);
	}

	content_w = (scr_w > 0 ? scr_w : 720) - g->inset_l - g->inset_r;
	if (content_w < 200)
		content_w = 200;
	/* Row fills list content (list already has GALLERY_PAD). */
	row_w = content_w - 2 * GALLERY_PAD;
	if (row_w < 160)
		row_w = 160;

	if (show == 0) {
		lv_obj_t *empty = lv_label_create(g->list);

		lv_label_set_text(empty, "No photos or videos yet");
		lv_obj_set_style_text_color(empty, lv_color_hex(0xAAAAAA), 0);
		lv_obj_set_style_text_font(empty, &lv_font_montserrat_14, 0);
		lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_clear_flag(empty, LV_OBJ_FLAG_CLICKABLE);
		/*
		 * BAR_ROT180: hdr is at FB bottom (= user top). Pin empty tip next
		 * to it and rotate glyphs upright.
		 */
		if (g->bar_rot180) {
			lv_obj_set_flex_align(g->list, LV_FLEX_ALIGN_END,
				LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
			lv_obj_set_style_pad_bottom(g->list, 24, 0);
			rotate_widget_180(empty);
		} else {
			lv_obj_set_flex_align(g->list, LV_FLEX_ALIGN_START,
				LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
			lv_obj_set_style_pad_top(g->list, 24, 0);
		}
		printf("[ui] album empty (%s)\n", g->save_dir);
		fflush(stdout);
		return;
	}

	/*
	 * entries[] is newest-first. With BAR_ROT180 the header sits at the FB
	 * bottom of the list: add oldest first so newest lands nearest the hdr.
	 */
	for (i = 0; i < show; i++) {
		int ei = g->bar_rot180 ? (show - 1 - i) : i;
		lv_obj_t *row;
		lv_obj_t *lbl;
		lv_obj_t *del;
		lv_obj_t *dl;
		lv_obj_t *thumb_obj = NULL;
		char *path_heap;
		const lv_coord_t pad_l = 10;
		const lv_coord_t pad_r = 10;
		const lv_coord_t gap = 8;
		const lv_coord_t row_h = GALLERY_ROW_H;
		const lv_coord_t thumb_w = GALLERY_THUMB_W;
		const lv_coord_t thumb_h = GALLERY_THUMB_H;
		lv_coord_t lbl_w;
		lv_coord_t lbl_x;
		lv_coord_t del_x;
		lv_coord_t del_y;
		lv_coord_t thumb_x;
		lv_coord_t thumb_y;

		/* C: card label — name only, no size/tech meta. */
		snprintf(line, sizeof(line), "%s", entries[ei].name);

		row = lv_obj_create(g->list);
		lv_obj_remove_style_all(row);
		lv_obj_set_layout(row, 0);
		lv_obj_set_size(row, row_w, row_h);
		lv_obj_set_style_bg_color(row, lv_color_hex(0xF4EFE6), 0);
		lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
		lv_obj_set_style_radius(row, 8, 0);
		lv_obj_set_style_pad_all(row, 0, 0);
		lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_flag(row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

		/*
		 * Absolute pos — do not use ALIGN_RIGHT (theme flex / pad often
		 * parks the button on the wrong edge).
		 *
		 * UI_BAR_ROT180: product physical 180° swaps L/R — put Del on FB
		 * left so it reads on the user’s right after mount.
		 * Row: [Del][thumb][label]  (rot180) or [thumb][label][Del].
		 */
		thumb_y = (row_h - thumb_h) / 2;
		if (g->bar_rot180) {
			del_x = pad_l;
			thumb_x = pad_l + GALLERY_DEL_W + gap;
			lbl_x = thumb_x + thumb_w + gap;
			lbl_w = row_w - pad_r - lbl_x;
		} else {
			thumb_x = pad_l;
			lbl_x = pad_l + thumb_w + gap;
			del_x = row_w - pad_r - GALLERY_DEL_W;
			lbl_w = del_x - gap - lbl_x;
		}
		if (lbl_w < 48)
			lbl_w = 48;
		del_y = (row_h - GALLERY_DEL_H) / 2;

		{
			lv_obj_t *cv = lv_canvas_create(row);
			int is_vid = is_mp4_name(entries[ei].path);

			lv_obj_set_size(cv, thumb_w, thumb_h);
			lv_obj_set_pos(cv, thumb_x, thumb_y);
			lv_obj_clear_flag(cv, LV_OBJ_FLAG_CLICKABLE);
			if ((is_vid &&
			     list_thumb_try_anim(entries[ei].path, cv,
					thumb_w, thumb_h) == 0) ||
			    list_thumb_set_static(entries[ei].path, cv,
					thumb_w, thumb_h) == 0) {
				thumb_obj = cv;
			} else {
				lv_obj_delete(cv);
				/* Placeholder keeps row alignment. */
				thumb_obj = lv_obj_create(row);
				lv_obj_remove_style_all(thumb_obj);
				lv_obj_set_size(thumb_obj, thumb_w, thumb_h);
				lv_obj_set_pos(thumb_obj, thumb_x, thumb_y);
				lv_obj_set_style_bg_color(thumb_obj,
					lv_color_hex(0xD0CBC2), 0);
				lv_obj_set_style_bg_opa(thumb_obj, LV_OPA_COVER, 0);
				lv_obj_set_style_radius(thumb_obj, 4, 0);
				lv_obj_clear_flag(thumb_obj, LV_OBJ_FLAG_CLICKABLE);
				lv_obj_clear_flag(thumb_obj, LV_OBJ_FLAG_SCROLLABLE);
			}
		}

		lbl = lv_label_create(row);
		lv_label_set_text(lbl, line);
		lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
		lv_obj_set_size(lbl, lbl_w, 64);
		lv_obj_set_style_text_color(lbl, lv_color_hex(0x2A2A2A), 0);
		lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
		lv_obj_set_pos(lbl, lbl_x, (row_h - 64) / 2);
		lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

		/* Tap anywhere on the row; Del vs open decided by calibrated geometry. */
		{
			lv_obj_t *hit = lv_obj_create(row);
			lv_coord_t hit_w;
			lv_coord_t hit_x;

			hit_x = pad_l;
			hit_w = row_w - pad_l - pad_r;
			lv_obj_remove_style_all(hit);
			if (hit_w < 80)
				hit_w = 80;
			lv_obj_set_size(hit, hit_w, row_h);
			lv_obj_set_pos(hit, hit_x, 0);
			lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);
			lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
			lv_obj_clear_flag(hit, LV_OBJ_FLAG_SCROLLABLE);
			path_heap = malloc(strlen(entries[ei].path) + 1);
			if (path_heap)
				strcpy(path_heap, entries[ei].path);
			/* Same path on row + hit; free once when hit is destroyed. */
			lv_obj_set_user_data(row, path_heap);
			lv_obj_set_user_data(hit, path_heap);
			lv_obj_add_event_cb(hit, open_item_cb, LV_EVENT_PRESSED, g);
			lv_obj_add_event_cb(hit, open_item_cb, LV_EVENT_PRESSING, g);
			lv_obj_add_event_cb(hit, open_item_cb, LV_EVENT_CLICKED, g);
			lv_obj_add_event_cb(hit, del_btn_delete_cb, LV_EVENT_DELETE, NULL);
			lv_obj_move_foreground(lbl);
			if (thumb_obj)
				lv_obj_move_foreground(thumb_obj);
			if (is_mp4_name(entries[ei].path)) {
				add_video_thumb_chrome(row, thumb_x, thumb_y,
					thumb_w, thumb_h, entries[ei].path,
					g->bar_rot180);
			}
			lv_obj_move_foreground(hit);
		}

		/* Visual Del only — hit testing is geometry (YCAL), not this widget. */
		del = lv_btn_create(row);
		lv_obj_remove_style_all(del);
		lv_obj_set_size(del, GALLERY_DEL_W, GALLERY_DEL_H);
		lv_obj_set_pos(del, del_x, del_y);
		lv_obj_set_style_bg_color(del, lv_color_hex(0xE03131), 0);
		lv_obj_set_style_bg_color(del, lv_color_hex(0xFF6B6B),
			LV_STATE_PRESSED);
		lv_obj_set_style_bg_opa(del, LV_OPA_COVER, 0);
		lv_obj_set_style_radius(del, 8, 0);
		lv_obj_set_style_transform_zoom(del, 230, LV_STATE_PRESSED);
		lv_obj_set_style_transform_pivot_x(del, GALLERY_DEL_W / 2, 0);
		lv_obj_set_style_transform_pivot_y(del, GALLERY_DEL_H / 2, 0);
		lv_obj_clear_flag(del, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_clear_flag(del, LV_OBJ_FLAG_SCROLLABLE);
		path_heap = malloc(strlen(entries[ei].path) + 1);
		if (path_heap)
			strcpy(path_heap, entries[ei].path);
		lv_obj_set_user_data(del, path_heap);
		lv_obj_add_event_cb(del, del_btn_delete_cb, LV_EVENT_DELETE, NULL);
		/* Keep Del under the full-row hit so geometry gets every press. */
		lv_obj_move_foreground(del);
		{
			/* Re-raise hit above Del after Del was created. */
			uint32_t ci;
			uint32_t cn = lv_obj_get_child_cnt(row);

			for (ci = 0; ci < cn; ci++) {
				lv_obj_t *c = lv_obj_get_child(row, ci);

				if (!c || c == del)
					continue;
				if (lv_obj_has_flag(c, LV_OBJ_FLAG_CLICKABLE) &&
				    lv_obj_get_height(c) == row_h)
					lv_obj_move_foreground(c);
			}
		}

		dl = lv_label_create(del);
		lv_label_set_text(dl, "Del");
		lv_obj_set_style_text_color(dl, lv_color_white(), 0);
		lv_obj_set_style_text_font(dl, &lv_font_montserrat_14, 0);
		lv_obj_center(dl);
		lv_obj_clear_flag(dl, LV_OBJ_FLAG_CLICKABLE);

		if (g->bar_rot180) {
			rotate_widget_180(lbl);
			rotate_widget_180(dl);
			/* Thumb is camera pixels — same as preview, no 180° rotate. */
		}
	}
	gallery_pin_near_hdr(g);
	g_gallery_anim = g;
	video_anim_sync();
	printf("[ui] album listed %d items (row_w=%d inset_r=%d rot180=%d)\n",
		show, (int)row_w, g->inset_r, g->bar_rot180);
	/* Row map: [n] = from FB top (= user-bottom count when BAR_ROT180). */
	{
		uint32_t n = lv_obj_get_child_cnt(g->list);
		uint32_t ri;
		int idx = 0;

		for (ri = 0; ri < n; ri++) {
			lv_obj_t *row = lv_obj_get_child(g->list, ri);
			lv_area_t a;
			const char *path;

			if (!row)
				continue;
			path = gallery_row_path(row);
			if (!path)
				continue;
			lv_obj_get_coords(row, &a);
			idx++;
			printf("[ui] album row [%2d] y=%4d..%-4d %s\n",
				idx, (int)a.y1, (int)a.y2,
				gallery_basename(path));
		}
	}
	fflush(stdout);
}

void ui_gallery_init(ui_gallery_t *g, const char *save_dir,
		int inset_l, int inset_r, int inset_t,
		int frame_l, int frame_r, int frame_t, uint32_t frame_color)
{
	lv_obj_t *cl;
	lv_coord_t scr_w = lv_disp_get_hor_res(NULL);
	lv_coord_t scr_h = lv_disp_get_ver_res(NULL);
	lv_coord_t content_w;
	lv_coord_t content_h;

	if (!g)
		return;
	memset(g, 0, sizeof(*g));
	g->max_items = env_int("CAM_GALLERY_N", GALLERY_MAX_DEFAULT);
	if (g->max_items < 1)
		g->max_items = 1;
	if (g->max_items > GALLERY_SCAN_MAX)
		g->max_items = GALLERY_SCAN_MAX;

	/*
	 * Keep widgets inside the visible panel: jc-050hd134 clips ~40px on the
	 * right (plus film frame). Full-bleed Del was half-cut by that overscan.
	 * Polaroid FB paint is still disabled while the album is open.
	 *
	 * UI_BAR_ROT180 matches the camera bars: pre-mirror L/R for product
	 * physical flip, and rotate each widget 180° (do not rotate root —
	 * that swaps the right overscan into a large left gap).
	 */
	g->bar_rot180 = env_on("UI_BAR_ROT180", 1);
	g->inset_l = inset_l > 0 ? inset_l : 0;
	g->inset_r = inset_r > 0 ? inset_r : 0;
	g->inset_t = inset_t > 0 ? inset_t : 0;
	g->frame_l = frame_l;
	g->frame_r = frame_r;
	g->frame_t = frame_t;
	g->frame_color = frame_color;

	if (scr_w <= 0)
		scr_w = 720;
	if (scr_h <= 0)
		scr_h = 1280;
	content_w = scr_w - g->inset_l - g->inset_r;
	if (content_w < 200)
		content_w = scr_w;
	content_h = scr_h - g->inset_t;
	if (content_h < 200)
		content_h = scr_h;

	g->root = lv_obj_create(lv_scr_act());
	lv_obj_remove_style_all(g->root);
	lv_obj_set_size(g->root, scr_w, scr_h);
	lv_obj_set_pos(g->root, 0, 0);
	lv_obj_set_style_bg_color(g->root, lv_color_hex(0x1A1A1A), 0);
	lv_obj_set_style_bg_opa(g->root, LV_OPA_TRANSP, 0);
	lv_obj_set_style_pad_all(g->root, 0, 0);
	lv_obj_clear_flag(g->root, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_clear_flag(g->root, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_flag(g->root, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_foreground(g->root);

	{
		lv_coord_t hdr_y;
		lv_coord_t list_y;
		lv_coord_t list_h = content_h - GALLERY_HDR_H;
		lv_coord_t exit_h = 0;

		/*
		 * UI_BAR_ROT180 + product physical 180°: FB top becomes user’s
		 * bottom (caption). Put Album/Close on FB bottom so they land on
		 * the user’s top (same end as the control bar after mount).
		 */
		if (g->bar_rot180) {
			/* Leave margin so 180° transform dirty rect stays inside FB. */
			hdr_y = scr_h - GALLERY_HDR_H - g->inset_t - 8;
			if (hdr_y < 0)
				hdr_y = 0;
			exit_h = env_int("UI_GALLERY_EXIT_H", GALLERY_EXIT_H_DEFAULT);
			if (exit_h < 48)
				exit_h = 48;
			if (exit_h > scr_h / 5)
				exit_h = scr_h / 5;
			/* Keep list clear of the double-tap strip (FB top / user bottom). */
			list_y = exit_h + 8;
			list_h = hdr_y - list_y;
			if (list_h < 120) {
				exit_h = 48;
				list_y = exit_h + 8;
				list_h = hdr_y - list_y;
			}
		} else {
			hdr_y = g->inset_t;
			list_y = g->inset_t + GALLERY_HDR_H;
			list_h = content_h - GALLERY_HDR_H;
			exit_h = env_int("UI_GALLERY_EXIT_H", GALLERY_EXIT_H_DEFAULT);
			if (exit_h < 48)
				exit_h = 48;
			if (exit_h > scr_h / 5)
				exit_h = scr_h / 5;
			list_h -= (exit_h + 8);
			if (list_h < 120)
				list_h = 120;
		}

		g->hdr = lv_obj_create(g->root);
		lv_obj_remove_style_all(g->hdr);
		/*
		 * Extend cream to FB bottom so stretch-overshoot taps (row 11/12
		 * visual → mapped y≈1260+) still land on a clickable widget. Press
		 * uses YCAL geometry to open the real list row.
		 */
		lv_obj_set_size(g->hdr, content_w, scr_h - hdr_y);
		lv_obj_set_pos(g->hdr, g->inset_l, hdr_y);
		lv_obj_set_style_bg_color(g->hdr, lv_color_hex(0xF4EFE6), 0);
		lv_obj_set_style_bg_opa(g->hdr, LV_OPA_COVER, 0);
		lv_obj_set_style_pad_hor(g->hdr, GALLERY_PAD, 0);
		lv_obj_clear_flag(g->hdr, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_add_flag(g->hdr, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_flag(g->hdr, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
		lv_obj_add_event_cb(g->hdr, gallery_hdr_press_cb, LV_EVENT_PRESSED, g);
		lv_obj_add_event_cb(g->hdr, gallery_hdr_press_cb, LV_EVENT_PRESSING, g);
		lv_obj_add_event_cb(g->hdr, gallery_hdr_press_cb, LV_EVENT_CLICKED, g);

		g->title = lv_label_create(g->hdr);
		lv_label_set_text(g->title, "Photos");
		lv_obj_set_style_text_color(g->title, lv_color_hex(0x2A2A2A), 0);
		lv_obj_set_style_text_font(g->title, &lv_font_montserrat_20, 0);
		lv_obj_clear_flag(g->title, LV_OBJ_FLAG_CLICKABLE);

		g->close_btn = lv_btn_create(g->hdr);
		lv_obj_remove_style_all(g->close_btn);
		lv_obj_set_size(g->close_btn, 120, 56);
		lv_obj_set_style_bg_color(g->close_btn, lv_color_hex(0x333333), 0);
		lv_obj_set_style_bg_color(g->close_btn, lv_color_hex(0x666666),
			LV_STATE_PRESSED);
		lv_obj_set_style_bg_opa(g->close_btn, LV_OPA_COVER, 0);
		lv_obj_set_style_radius(g->close_btn, 8, 0);
		lv_obj_set_style_transform_zoom(g->close_btn, 230, LV_STATE_PRESSED);
		lv_obj_set_style_transform_pivot_x(g->close_btn, 60, 0);
		lv_obj_set_style_transform_pivot_y(g->close_btn, 28, 0);
		lv_obj_set_ext_click_area(g->close_btn, 8);
		lv_obj_add_event_cb(g->close_btn, close_btn_cb, LV_EVENT_PRESSED, g);
		cl = lv_label_create(g->close_btn);
		lv_label_set_text(cl, "Close");
		lv_obj_set_style_text_color(cl, lv_color_white(), 0);
		lv_obj_set_style_text_font(cl, &lv_font_montserrat_14, 0);
		lv_obj_center(cl);
		lv_obj_clear_flag(cl, LV_OBJ_FLAG_CLICKABLE);

		if (g->bar_rot180) {
			/*
			 * Close on FB right (= user left after mount) so stretch
			 * overshoot into hdr does not collide with Del (FB left /
			 * user right). Rotate only the label — button bbox stays
			 * aligned with touches.
			 */
			lv_obj_align(g->title, LV_ALIGN_TOP_LEFT, 4, 10);
			lv_obj_align(g->close_btn, LV_ALIGN_TOP_RIGHT, -4, 8);
			rotate_widget_180(g->title);
			rotate_widget_180(cl);
		} else {
			lv_obj_align(g->title, LV_ALIGN_LEFT_MID, 4, 0);
			lv_obj_align(g->close_btn, LV_ALIGN_RIGHT_MID, -4, 0);
		}

		g->list = lv_obj_create(g->root);
		lv_obj_remove_style_all(g->list);
		lv_obj_set_size(g->list, content_w, list_h);
		lv_obj_set_pos(g->list, g->inset_l, list_y);
		lv_obj_set_style_bg_color(g->list, lv_color_hex(0x2A2A2A), 0);
		lv_obj_set_style_bg_opa(g->list, LV_OPA_COVER, 0);
		lv_obj_set_style_pad_all(g->list, GALLERY_PAD, 0);
		lv_obj_set_style_pad_row(g->list, 10, 0);
		/*
		 * COLUMN + START when overflowing (see gallery_pin_near_hdr). Short
		 * BAR_ROT180 lists pin with END so rows sit next to the hdr.
		 */
		lv_obj_set_flex_flow(g->list, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_scroll_dir(g->list, LV_DIR_VER);
		lv_obj_add_flag(g->list, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_clear_flag(g->list, LV_OBJ_FLAG_SCROLL_CHAIN);
		lv_obj_set_scrollbar_mode(g->list, LV_SCROLLBAR_MODE_AUTO);
		lv_obj_add_event_cb(g->list, gallery_list_scroll_cb,
			LV_EVENT_SCROLL, NULL);
		lv_obj_add_event_cb(g->list, gallery_list_scroll_cb,
			LV_EVENT_SCROLL_END, NULL);
		lv_obj_add_flag(g->list, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_event_cb(g->list, gallery_list_press_cb,
			LV_EVENT_PRESSED, g);
		lv_obj_add_event_cb(g->list, gallery_list_press_cb,
			LV_EVENT_PRESSING, g);
		lv_obj_add_event_cb(g->list, gallery_list_press_cb,
			LV_EVENT_CLICKED, g);

		/*
		 * Double-tap exit: invisible hit strip at user-bottom middle.
		 * Must not cover the list (steals scrolls); reserved by list_y.
		 */
		{
			lv_coord_t zone_h = exit_h;
			lv_coord_t zone_w;
			lv_coord_t zone_x;
			lv_coord_t zone_y;
			lv_obj_t *hint;

			zone_w = content_w * 2 / 5;
			if (zone_w < 160)
				zone_w = 160;
			if (zone_w > content_w)
				zone_w = content_w;
			zone_x = g->inset_l + (content_w - zone_w) / 2;
			zone_y = g->bar_rot180 ? 4 : (scr_h - zone_h - 4);

			g->exit_zone = lv_obj_create(g->root);
			lv_obj_remove_style_all(g->exit_zone);
			lv_obj_set_size(g->exit_zone, zone_w, zone_h);
			lv_obj_set_pos(g->exit_zone, zone_x, zone_y);
			/* Fully transparent — hit target only (no overlay on photos). */
			lv_obj_set_style_bg_opa(g->exit_zone, LV_OPA_TRANSP, 0);
			lv_obj_set_style_border_width(g->exit_zone, 0, 0);
			lv_obj_set_style_radius(g->exit_zone, 0, 0);
			lv_obj_clear_flag(g->exit_zone, LV_OBJ_FLAG_SCROLLABLE);
			lv_obj_add_flag(g->exit_zone, LV_OBJ_FLAG_CLICKABLE);
			lv_obj_add_event_cb(g->exit_zone, exit_zone_cb,
				LV_EVENT_PRESSED, g);

			hint = lv_label_create(g->exit_zone);
			lv_label_set_text(hint, "double tap");
			lv_obj_set_style_text_color(hint, lv_color_hex(0xAAAAAA), 0);
			lv_obj_set_style_text_opa(hint, LV_OPA_50, 0);
			lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
			lv_obj_center(hint);
			lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);
			if (g->bar_rot180)
				rotate_widget_180(hint);
		}
	}

	/* Fullscreen photo preview: sidecar RGB565 → ARGB LVGL canvas. */
	g->preview_w = CAM_PREV_W;
	g->preview_h = CAM_PREV_H;
	g->preview_rgb = malloc((size_t)g->preview_w * (size_t)g->preview_h *
		sizeof(uint16_t));
	g->preview_argb = malloc((size_t)g->preview_w * (size_t)g->preview_h *
		sizeof(lv_color32_t));
	if (!g->preview_rgb || !g->preview_argb)
		fprintf(stderr, "[ui] album preview buf alloc failed (%dx%d)\n",
			g->preview_w, g->preview_h);

	g->preview = lv_obj_create(g->root);
	lv_obj_remove_style_all(g->preview);
	lv_obj_set_size(g->preview, scr_w, scr_h);
	lv_obj_set_pos(g->preview, 0, 0);
	/* Transparent overlay; photo pixels come from preview_canvas ARGB. */
	lv_obj_set_style_bg_opa(g->preview, LV_OPA_TRANSP, 0);
	lv_obj_clear_flag(g->preview, LV_OBJ_FLAG_SCROLLABLE);
	/* Hidden + non-clickable until preview_show; else blocks list scroll. */
	lv_obj_clear_flag(g->preview, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_flag(g->preview, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_background(g->preview);
	lv_obj_add_event_cb(g->preview, preview_dismiss_cb, LV_EVENT_PRESSED, g);

	/* ARGB8888 canvas — bound in preview_show_photo (no LVGL xform). */
	g->preview_canvas = lv_canvas_create(g->preview);
	lv_obj_set_size(g->preview_canvas, g->preview_w, g->preview_h);
	lv_obj_center(g->preview_canvas);
	lv_obj_clear_flag(g->preview_canvas, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_flag(g->preview_canvas, LV_OBJ_FLAG_HIDDEN);

	g->preview_hint = lv_label_create(g->preview);
	lv_label_set_text(g->preview_hint, "Tap to close");
	lv_obj_set_style_text_color(g->preview_hint, lv_color_hex(0xFFFFFF), 0);
	lv_obj_set_style_text_font(g->preview_hint, &lv_font_montserrat_14, 0);
	lv_obj_set_style_bg_color(g->preview_hint, lv_color_hex(0x000000), 0);
	lv_obj_set_style_bg_opa(g->preview_hint, LV_OPA_60, 0);
	lv_obj_set_style_pad_hor(g->preview_hint, 12, 0);
	lv_obj_set_style_pad_ver(g->preview_hint, 6, 0);
	lv_obj_set_style_radius(g->preview_hint, 6, 0);
	lv_obj_align(g->preview_hint, LV_ALIGN_BOTTOM_MID, 0, -28);
	lv_obj_clear_flag(g->preview_hint, LV_OBJ_FLAG_CLICKABLE);

	g->preview_msg = lv_label_create(g->preview);
	lv_label_set_text(g->preview_msg, "");
	lv_obj_set_style_text_color(g->preview_msg, lv_color_hex(0xEEEEEE), 0);
	lv_obj_set_style_text_font(g->preview_msg, &lv_font_montserrat_20, 0);
	lv_obj_set_style_text_align(g->preview_msg, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_center(g->preview_msg);
	lv_obj_clear_flag(g->preview_msg, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_flag(g->preview_msg, LV_OBJ_FLAG_HIDDEN);

	printf("[ui] album layout %dx%d inset L/R/T=%d/%d/%d bar_rot180=%d\n",
		(int)scr_w, (int)scr_h, g->inset_l, g->inset_r, g->inset_t,
		g->bar_rot180);
	/* After all pointer fields are filled — last member, cannot be stomped. */
	ui_gallery_set_save_dir(g, save_dir);
	printf("[ui] album save_dir=%s\n", g->save_dir);
	fflush(stdout);
}

void ui_gallery_show(ui_gallery_t *g)
{
	if (!g || !g->root || !g->list) {
		fprintf(stderr, "[ui] album show: not initialized\n");
		return;
	}
	/* Drop cream stamp while album is open (widgets already inset). */
	lvgl_display_polaroid_suspend();
	/*
	 * Keep save_dir already set by gallery_btn (pipeline path). Only repair
	 * if empty/stomped — do NOT set_save_dir(NULL) here: that ignored the
	 * pipeline dir and forced CAM_SAVE_DIR or /userdata while capture may
	 * have written to /tmp/lvgl_camera.
	 */
	if (g->save_dir[0] != '/')
		ui_gallery_set_save_dir(g, NULL);
	lv_obj_set_style_bg_opa(g->root, LV_OPA_COVER, 0);
	lv_obj_clear_flag(g->root, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_foreground(g->root);
	g->visible = 1;
	g->exit_tap_ms = 0;
	preview_hide(g);
	printf("[ui] album show begin (%s)\n", g->save_dir);
	fflush(stdout);
	gallery_refresh(g);
	/* Stay under list scrolls; strip is outside the list bbox. */
	if (g->exit_zone && lv_obj_is_valid(g->exit_zone))
		lv_obj_move_foreground(g->exit_zone);
	printf("[ui] album shown (%s) - tap photo name to preview; "
		"double-tap bottom middle to close\n",
		g->save_dir);
	fflush(stdout);
}

void ui_gallery_hide(ui_gallery_t *g)
{
	if (!g || !g->root)
		return;
	preview_hide(g);
	lv_obj_add_flag(g->root, LV_OBJ_FLAG_HIDDEN);
	lv_obj_set_style_bg_opa(g->root, LV_OPA_TRANSP, 0);
	g->visible = 0;
	g->exit_tap_ms = 0;
	/* Resume stashed edges — never trust possibly-stomped g->frame_*. */
	lvgl_display_polaroid_resume();
	lv_obj_invalidate(lv_scr_act());
	printf("[ui] album hide\n");
	fflush(stdout);
}

int ui_gallery_is_visible(const ui_gallery_t *g)
{
	return g && g->visible;
}
