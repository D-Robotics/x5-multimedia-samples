#include "media_still.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hb_media_codec.h"
#include "hb_media_error.h"

struct glyph {
	char c;
	uint8_t col[5];
};

/* Tiny 5x7 glyphs (bits column-major, bit0 = top of ink cell). */
static const struct glyph GLYPHS[] = {
	{' ', {0, 0, 0, 0, 0}},
	{'0', {0x3E, 0x51, 0x49, 0x45, 0x3E}},
	{'1', {0x00, 0x42, 0x7F, 0x40, 0x00}},
	{'2', {0x42, 0x61, 0x51, 0x49, 0x46}},
	{'3', {0x21, 0x41, 0x45, 0x4B, 0x31}},
	{'4', {0x18, 0x14, 0x12, 0x7F, 0x10}},
	{'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
	{'6', {0x3C, 0x4A, 0x49, 0x49, 0x30}},
	{'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
	{'8', {0x36, 0x49, 0x49, 0x49, 0x36}},
	{'9', {0x06, 0x49, 0x49, 0x29, 0x1E}},
	{':', {0x00, 0x36, 0x36, 0x00, 0x00}},
	{'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
	{'/', {0x20, 0x10, 0x08, 0x04, 0x02}},
	{'.', {0x00, 0x60, 0x60, 0x00, 0x00}},
	{'A', {0x7E, 0x11, 0x11, 0x11, 0x7E}},
	{'C', {0x3E, 0x41, 0x41, 0x41, 0x22}},
	{'E', {0x7F, 0x49, 0x49, 0x49, 0x41}},
	{'G', {0x3E, 0x41, 0x49, 0x49, 0x7A}},
	{'J', {0x20, 0x40, 0x41, 0x3F, 0x01}},
	{'M', {0x7F, 0x02, 0x0C, 0x02, 0x7F}},
	{'P', {0x7F, 0x09, 0x09, 0x09, 0x06}},
};

static int env_on(const char *name, int def)
{
	const char *v = getenv(name);

	if (!v || !v[0])
		return def;
	if (v[0] == '0' && v[1] == '\0')
		return 0;
	if (v[0] == '1' && v[1] == '\0')
		return 1;
	return def;
}

/* CAM_SIDECAR=1 writes/reads .p565/.t565. Default 0: decode on the fly. */
static int sidecar_enabled(void)
{
	return env_on("CAM_SIDECAR", 0);
}

static int video_rotate_deg(void)
{
	const char *rv = getenv("CAM_VIDEO_ROTATE");

	/* Board-tuned default 90 — do not change without board confirmation. */
	if (!rv || !rv[0])
		return 90;
	return atoi(rv);
}

/*
 * Cream #F4EFE6 as full-range YUV (approx). Used for JPEG polaroid matte.
 * Ink ≈ UI #2A2A2A on cream (no HUD bar — classic Instant caption).
 */
#define PAPER_Y  239
#define PAPER_U  123
#define PAPER_V  131
#define INK_Y    42

static void nv12_fill_y_rect(uint8_t *y, int stride, int w, int h,
		int x0, int y0, int rw, int rh, uint8_t yv)
{
	int x1, y1, yy, xx;

	if (!y || stride <= 0 || rw <= 0 || rh <= 0)
		return;
	if (x0 < 0) {
		rw += x0;
		x0 = 0;
	}
	if (y0 < 0) {
		rh += y0;
		y0 = 0;
	}
	x1 = x0 + rw;
	y1 = y0 + rh;
	if (x1 > w)
		x1 = w;
	if (y1 > h)
		y1 = h;
	for (yy = y0; yy < y1; yy++) {
		uint8_t *row = y + (size_t)yy * (size_t)stride;

		for (xx = x0; xx < x1; xx++)
			row[xx] = yv;
	}
}

static void nv12_fill_uv_rect(uint8_t *uv, int stride, int w, int h,
		int x0, int y0, int rw, int rh, uint8_t u, uint8_t v)
{
	int x1, y1, yy, xx;
	int uw = w / 2;
	int uh = h / 2;

	if (!uv || stride <= 0 || rw <= 0 || rh <= 0)
		return;
	/* Align to chroma 2x2. */
	x0 &= ~1;
	y0 &= ~1;
	rw = (rw + 1) & ~1;
	rh = (rh + 1) & ~1;
	if (x0 < 0) {
		rw += x0;
		x0 = 0;
	}
	if (y0 < 0) {
		rh += y0;
		y0 = 0;
	}
	x1 = x0 + rw;
	y1 = y0 + rh;
	if (x1 > w)
		x1 = w;
	if (y1 > h)
		y1 = h;
	for (yy = y0 / 2; yy < y1 / 2 && yy < uh; yy++) {
		uint8_t *row = uv + (size_t)yy * (size_t)stride;

		for (xx = x0 / 2; xx < x1 / 2 && xx < uw; xx++) {
			row[xx * 2] = u;
			row[xx * 2 + 1] = v;
		}
	}
}

/*
 * Compute source-edge cream margins so thick caption lands at viewed BOTTOM
 * after JPEG CAM_VIDEO_ROTATE.
 *
 * Board note: for deg=90, MC_CCW_90 on this NV12 path maps like CW in
 * y-down coords (source left → viewed top). Put thick on the RIGHT so it
 * becomes the viewed bottom. Deg=270 is the opposite.
 * Proportions: L/R=16 T=56 B=152 @720x1280.
 */
static void polaroid_src_margins(int w, int h, int deg,
		int *out_sl, int *out_sr, int *out_st, int *out_sb)
{
	int final_w;
	int final_h;
	int side;
	int top;
	int bot;
	int sl, sr, st, sb;

	if (deg == 90 || deg == 270) {
		final_w = h;
		final_h = w;
	} else {
		final_w = w;
		final_h = h;
	}

	side = (16 * final_w) / 720;
	top = (56 * final_h) / 1280;
	bot = (152 * final_h) / 1280;
	if (side < 8)
		side = 8;
	if (top < 16)
		top = 16;
	if (bot < 48)
		bot = 48;
	side &= ~1;
	top &= ~1;
	bot &= ~1;
	if (side * 2 >= final_w)
		side = (final_w / 8) & ~1;
	if (top + bot >= final_h) {
		top = (final_h / 10) & ~1;
		bot = (final_h / 5) & ~1;
	}

	if (deg == 90) {
		/* Thick RIGHT → viewed bottom; thin LEFT → viewed top. */
		sl = top;
		sr = bot;
		st = side;
		sb = side;
	} else if (deg == 270) {
		/* Opposite of 90°. */
		sl = bot;
		sr = top;
		st = side;
		sb = side;
	} else if (deg == 180) {
		sl = side;
		sr = side;
		st = bot;
		sb = top;
	} else {
		sl = side;
		sr = side;
		st = top;
		sb = bot;
	}
	if (sl + sr >= w) {
		sl = (w / 8) & ~1;
		sr = sl;
	}
	if (st + sb >= h) {
		st = (h / 10) & ~1;
		sb = (h / 5) & ~1;
	}
	*out_sl = sl;
	*out_sr = sr;
	*out_st = st;
	*out_sb = sb;
}

/*
 * Paint Polaroid cream into NV12 so that AFTER CAM_VIDEO_ROTATE the thick
 * caption sits at the viewed bottom (classic Instant look).
 * CAM_JPEG_BORDER=0 disables.
 */
void media_still_nv12_polaroid_frame(uint8_t *y, uint8_t *uv, int w, int h,
		int stride_y)
{
	int deg;
	int sl, sr, st, sb;

	if (!y || !uv || w < 64 || h < 64)
		return;
	if (!env_on("CAM_JPEG_BORDER", 1))
		return;
	if (stride_y <= 0)
		stride_y = w;

	deg = video_rotate_deg();
	polaroid_src_margins(w, h, deg, &sl, &sr, &st, &sb);

	nv12_fill_y_rect(y, stride_y, w, h, 0, 0, sl, h, PAPER_Y);
	nv12_fill_y_rect(y, stride_y, w, h, w - sr, 0, sr, h, PAPER_Y);
	nv12_fill_y_rect(y, stride_y, w, h, 0, 0, w, st, PAPER_Y);
	nv12_fill_y_rect(y, stride_y, w, h, 0, h - sb, w, sb, PAPER_Y);

	nv12_fill_uv_rect(uv, stride_y, w, h, 0, 0, sl, h, PAPER_U, PAPER_V);
	nv12_fill_uv_rect(uv, stride_y, w, h, w - sr, 0, sr, h, PAPER_U, PAPER_V);
	nv12_fill_uv_rect(uv, stride_y, w, h, 0, 0, w, st, PAPER_U, PAPER_V);
	nv12_fill_uv_rect(uv, stride_y, w, h, 0, h - sb, w, sb, PAPER_U, PAPER_V);
}

static const uint8_t *glyph_cols(char c)
{
	size_t i;

	if (c >= 'a' && c <= 'z')
		c = (char)(c - 'a' + 'A');
	for (i = 0; i < sizeof(GLYPHS) / sizeof(GLYPHS[0]); i++) {
		if (GLYPHS[i].c == c)
			return GLYPHS[i].col;
	}
	return GLYPHS[0].col;
}

/*
 * Draw one glyph into the Y plane.
 * rot_deg: clockwise rotation of the glyph cell (0/90/180/270). Used so that
 * after MC_CCW_* encode rotation the stamp reads upright in the JPEG.
 */
static void draw_char_y(uint8_t *y, int w, int h, int x0, int y0, char c,
		int scale, int rot_deg, uint8_t ink)
{
	const uint8_t *g = glyph_cols(c);
	int col, row, sx, sy;
	int cell_w = 5 * scale;
	int cell_h = 7 * scale;

	for (col = 0; col < 5; col++) {
		uint8_t bits = g[col];

		for (row = 0; row < 7; row++) {
			if (!(bits & (1u << row)))
				continue;
			for (sy = 0; sy < scale; sy++) {
				for (sx = 0; sx < scale; sx++) {
					int lx = col * scale + sx;
					int ly = row * scale + sy;
					int ox, oy;
					int xx, yy;

					switch (rot_deg) {
					case 90: /* CW */
						ox = ly;
						oy = cell_w - 1 - lx;
						break;
					case 180:
						ox = cell_w - 1 - lx;
						oy = cell_h - 1 - ly;
						break;
					case 270: /* CW */
						ox = cell_h - 1 - ly;
						oy = lx;
						break;
					default:
						ox = lx;
						oy = ly;
						break;
					}
					xx = x0 + ox;
					yy = y0 + oy;
					if (yy < 0 || yy >= h || xx < 0 || xx >= w)
						continue;
					y[yy * w + xx] = ink;
				}
			}
		}
	}
}

/*
 * Timestamp on the Polaroid caption (viewed bottom), upright after encode.
 * Classic Instant: dark ink on cream — no HUD/black plate behind the stamp.
 */
void media_still_nv12_watermark(uint8_t *y, uint8_t *uv, int w, int h,
		const char *text)
{
	int scale = (w >= 1280) ? 3 : 2;
	int glyph_w = 6 * scale;
	int cell_h = 7 * scale;
	int deg;
	int sl, sr, st, sb;
	int x, y0, i;
	size_t len;
	int glyph_rot;
	int advance;
	int text_w, text_h;

	(void)uv;
	if (!y || !text || w <= 0 || h <= 0)
		return;
	len = strlen(text);
	if (len == 0)
		return;

	deg = video_rotate_deg();
	polaroid_src_margins(w, h, deg, &sl, &sr, &st, &sb);

	if (deg == 90) {
		/*
		 * Thick caption = right strip (→ viewed bottom). +180° stamp
		 * (reverse chars, glyphs CW90); center along strip → viewed H-center.
		 */
		glyph_rot = 90;
		advance = glyph_w;
		text_w = cell_h;
		text_h = (int)len * advance;
		x = w - sr + (sr - text_w) / 2;
		if (x + text_w > w - 4)
			x = w - text_w - 4;
		if (x < 0)
			x = 0;
		y0 = st + (h - st - sb - text_h) / 2;
		if (y0 < st + 4)
			y0 = st + 4;
		if (y0 + text_h > h - sb)
			y0 = h - sb - text_h;
		for (i = 0; text[i]; i++) {
			char ch = text[len - 1 - (size_t)i];

			draw_char_y(y, w, h, x, y0 + i * advance, ch,
				scale, glyph_rot, INK_Y);
		}
		return;
	}

	if (deg == 270) {
		/* Thick caption = left strip; mirror of deg 90 (+180 stamp fix). */
		glyph_rot = 270;
		advance = glyph_w;
		text_w = cell_h;
		text_h = (int)len * advance;
		x = (sl - text_w) / 2;
		if (x < 4)
			x = 4;
		y0 = st + (h - st - sb - text_h) / 2;
		if (y0 < st + 4)
			y0 = st + 4;
		if (y0 + text_h > h - sb)
			y0 = h - sb - text_h;
		for (i = 0; text[i]; i++)
			draw_char_y(y, w, h, x, y0 + i * advance, text[i],
				scale, glyph_rot, INK_Y);
		return;
	}

	/* deg 0 / 180: horizontal along viewed bottom. */
	{
		const char *wr = getenv("CAM_WATERMARK_ROT180");
		int rot180;

		if (wr && wr[0])
			rot180 = env_on("CAM_WATERMARK_ROT180", 1);
		else
			rot180 = env_on("UI_BAR_ROT180", 1);
		/* With encode 180, skip panel rot180 — file is already flipped. */
		if (deg == 180)
			rot180 = 0;

		glyph_rot = rot180 ? 180 : 0;
		text_w = (int)len * glyph_w;
		y0 = (deg == 180) ? ((st - cell_h) / 2) :
			(h - sb + (sb - cell_h) / 2);
		if (y0 < 0)
			y0 = 0;
		if (y0 + cell_h > h)
			y0 = h - cell_h - 2;
		x = sl + (w - sl - sr - text_w) / 2;
		if (x < sl + 4)
			x = sl + 4;
		for (i = 0; text[i]; i++) {
			char ch = rot180 ? text[len - 1 - (size_t)i] : text[i];

			draw_char_y(y, w, h, x + i * glyph_w, y0, ch, scale,
				glyph_rot, INK_Y);
		}
	}
}

/*
 * rot_deg: -1 = CAM_VIDEO_ROTATE (still/polaroid); 0/90/180/270 forced.
 * Video posters use 0 so plane priming matches sensor NV12 (1920x1080).
 */
static int encode_jpeg_rot(const uint8_t *y, const uint8_t *uv,
		int w, int h, int stride_y, int quality, const char *path,
		int rot_deg)
{
	media_codec_context_t ctx;
	mc_video_codec_enc_params_t *params;
	mc_av_codec_startup_params_t startup = {0};
	media_codec_buffer_t in_buf = {0};
	media_codec_buffer_t out_buf = {0};
	media_codec_output_buffer_info_t info = {0};
	FILE *fp = NULL;
	size_t y_sz, uv_sz;
	int ret;
	int row;
	int deg;

	if (!y || !uv || !path || w <= 0 || h <= 0)
		return -1;
	if (stride_y <= 0)
		stride_y = w;
	if (quality < 1)
		quality = 1;
	if (quality > 100)
		quality = 100;

	y_sz = (size_t)w * (size_t)h;
	uv_sz = y_sz / 2;

	memset(&ctx, 0, sizeof(ctx));
	ctx.encoder = 1;
	ctx.codec_id = MEDIA_CODEC_ID_JPEG;
	params = &ctx.video_enc_params;
	params->width = w;
	params->height = h;
	params->pix_fmt = MC_PIXEL_FORMAT_NV12;
	params->frame_buf_count = 3;
	params->bitstream_buf_count = 3;
	params->external_frame_buf = 0;
	params->bitstream_buf_size =
		(int)((y_sz + uv_sz + 0xfff) & ~(size_t)0xfff);
	params->jpeg_enc_config.quality_factor = quality;
	/* Match sample_codec: MCU rows with RSTn; HW path expects this. */
	params->jpeg_enc_config.restart_interval = w / 16;
	if (params->jpeg_enc_config.restart_interval < 1)
		params->jpeg_enc_config.restart_interval = 1;
	if (rot_deg < 0) {
		const char *rv = getenv("CAM_VIDEO_ROTATE");

		deg = rv && rv[0] ? atoi(rv) : 90;
	} else {
		deg = rot_deg;
	}
	if (deg == 0)
		params->rot_degree = MC_CCW_0;
	else if (deg == 180)
		params->rot_degree = MC_CCW_180;
	else if (deg == 270)
		params->rot_degree = MC_CCW_270;
	else
		params->rot_degree = MC_CCW_90;

	ret = hb_mm_mc_initialize(&ctx);
	if (ret != 0)
		return ret;
	ret = hb_mm_mc_configure(&ctx);
	if (ret != 0)
		goto fail;
	ret = hb_mm_mc_start(&ctx, &startup);
	if (ret != 0)
		goto fail;

	ret = hb_mm_mc_dequeue_input_buffer(&ctx, &in_buf, 3000);
	if (ret != 0)
		goto fail_stop;

	in_buf.type = MC_VIDEO_FRAME_BUFFER;
	in_buf.vframe_buf.width = w;
	in_buf.vframe_buf.height = h;
	in_buf.vframe_buf.pix_fmt = MC_PIXEL_FORMAT_NV12;
	in_buf.vframe_buf.size = (hb_s32)(y_sz + uv_sz);

	{
		int dst_pitch = in_buf.vframe_buf.stride > 0 ?
			in_buf.vframe_buf.stride : w;
		uint8_t *dst_y = in_buf.vframe_buf.vir_ptr[0];
		uint8_t *dst_uv = in_buf.vframe_buf.vir_ptr[1];

		if (dst_pitch < w)
			dst_pitch = w;
		if (!dst_uv)
			dst_uv = dst_y + (size_t)dst_pitch *
				(size_t)(in_buf.vframe_buf.vstride > 0 ?
					in_buf.vframe_buf.vstride : h);

		/* Always pack by rows — encoder frames are often stride-padded. */
		if (stride_y == dst_pitch && stride_y == w) {
			memcpy(dst_y, y, y_sz);
			memcpy(dst_uv, uv, uv_sz);
		} else {
			for (row = 0; row < h; row++)
				memcpy(dst_y + (size_t)row * (size_t)dst_pitch,
					y + (size_t)row * (size_t)stride_y,
					(size_t)w);
			for (row = 0; row < h / 2; row++)
				memcpy(dst_uv + (size_t)row * (size_t)dst_pitch,
					uv + (size_t)row * (size_t)stride_y,
					(size_t)w);
		}
	}

	ret = hb_mm_mc_queue_input_buffer(&ctx, &in_buf, 3000);
	if (ret != 0)
		goto fail_stop;

	ret = hb_mm_mc_dequeue_output_buffer(&ctx, &out_buf, &info, 5000);
	if (ret != 0)
		goto fail_stop;

	fp = fopen(path, "wb");
	if (!fp) {
		ret = -1;
		(void)hb_mm_mc_queue_output_buffer(&ctx, &out_buf, 1000);
		goto fail_stop;
	}
	if (fwrite(out_buf.vstream_buf.vir_ptr, 1, out_buf.vstream_buf.size, fp)
			!= out_buf.vstream_buf.size)
		ret = -1;
	else
		ret = 0;
	fclose(fp);
	(void)hb_mm_mc_queue_output_buffer(&ctx, &out_buf, 1000);

fail_stop:
	(void)hb_mm_mc_pause(&ctx);
fail:
	(void)hb_mm_mc_release(&ctx);
	return ret;
}

int media_still_encode_jpeg(const uint8_t *y, const uint8_t *uv,
		int w, int h, int stride_y, int quality, const char *path)
{
	/* Stills: CAM_VIDEO_ROTATE so album matches live preview. */
	return encode_jpeg_rot(y, uv, w, h, stride_y, quality, path, -1);
}

static inline uint16_t rgb_to_565(int r, int g, int b)
{
	return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* Parse SOF0/SOF2 for width/height. Returns 0 on success. */
static int jpeg_probe_size(const uint8_t *data, size_t len, int *out_w, int *out_h)
{
	size_t i = 0;

	if (!data || len < 4 || !out_w || !out_h)
		return -1;
	if (data[0] != 0xFF || data[1] != 0xD8)
		return -1;
	i = 2;
	while (i + 9 < len) {
		uint8_t marker;
		uint16_t seglen;

		if (data[i] != 0xFF)
			return -1;
		while (i < len && data[i] == 0xFF)
			i++;
		if (i >= len)
			return -1;
		marker = data[i++];
		if (marker == 0xD8 || marker == 0xD9 || marker == 0x01 ||
		    (marker >= 0xD0 && marker <= 0xD7))
			continue;
		if (i + 1 >= len)
			return -1;
		seglen = (uint16_t)((data[i] << 8) | data[i + 1]);
		if (seglen < 2 || i + seglen > len)
			return -1;
		/* SOF0 baseline / SOF2 progressive — both carried by HW encoder. */
		if ((marker == 0xC0 || marker == 0xC2) && seglen >= 8) {
			*out_h = (data[i + 3] << 8) | data[i + 4];
			*out_w = (data[i + 5] << 8) | data[i + 6];
			if (*out_w > 0 && *out_h > 0)
				return 0;
			return -1;
		}
		i += seglen;
	}
	return -1;
}

/*
 * Scale NV12 → RGB565 panel buffer.
 * fill=0: letterbox (keep aspect)
 * fill=1: stretch to dst (matches video plane SRC→CRTC full-panel map)
 */
static void nv12_scale_to_rgb565(const uint8_t *y, const uint8_t *uv,
		int src_w, int src_h, int y_stride, int uv_stride,
		uint16_t *dst, int dst_w, int dst_h, int fill)
{
	int dx, dy;
	int fit_w, fit_h, off_x, off_y;
	int64_t a, b;

	if (!y || !uv || !dst || src_w <= 0 || src_h <= 0 || dst_w <= 0 ||
	    dst_h <= 0)
		return;
	if (y_stride < src_w)
		y_stride = src_w;
	if (uv_stride < src_w)
		uv_stride = src_w;

	memset(dst, 0, (size_t)dst_w * (size_t)dst_h * sizeof(uint16_t));

	if (fill) {
		fit_w = dst_w;
		fit_h = dst_h;
		off_x = 0;
		off_y = 0;
	} else {
		a = (int64_t)dst_w * src_h;
		b = (int64_t)dst_h * src_w;
		if (a < b) {
			fit_w = dst_w;
			fit_h = (int)(((int64_t)src_h * dst_w) / src_w);
		} else {
			fit_h = dst_h;
			fit_w = (int)(((int64_t)src_w * dst_h) / src_h);
		}
		if (fit_w < 1)
			fit_w = 1;
		if (fit_h < 1)
			fit_h = 1;
		off_x = (dst_w - fit_w) / 2;
		off_y = (dst_h - fit_h) / 2;
	}

	for (dy = 0; dy < fit_h; dy++) {
		int sy = dy * src_h / fit_h;

		for (dx = 0; dx < fit_w; dx++) {
			int sx = dx * src_w / fit_w;
			int Y = y[sy * y_stride + sx];
			int uv_i = (sy / 2) * uv_stride + (sx & ~1);
			int U = uv[uv_i] - 128;
			int V = uv[uv_i + 1] - 128;
			int r = Y + ((V * 359) >> 8);
			int g = Y - ((U * 88 + V * 183) >> 8);
			int bb = Y + ((U * 454) >> 8);

			if (r < 0)
				r = 0;
			else if (r > 255)
				r = 255;
			if (g < 0)
				g = 0;
			else if (g > 255)
				g = 255;
			if (bb < 0)
				bb = 0;
			else if (bb > 255)
				bb = 255;
			dst[(off_y + dy) * dst_w + (off_x + dx)] =
				rgb_to_565(r, g, bb);
		}
	}
}

int media_still_decode_jpeg_nv12(const char *path, uint8_t **out_nv12,
		int *out_w, int *out_h)
{
	FILE *fp = NULL;
	uint8_t *file_buf = NULL;
	long file_sz = 0;
	int src_w = 0, src_h = 0;
	media_codec_context_t ctx;
	mc_video_codec_dec_params_t *params;
	mc_av_codec_startup_params_t startup = {0};
	media_codec_buffer_t in_buf = {0};
	media_codec_buffer_t out_buf = {0};
	media_codec_output_buffer_info_t info = {0};
	const uint8_t *y_ptr;
	const uint8_t *uv_ptr;
	uint8_t *packed = NULL;
	int ret = -1;

	if (!path || !out_nv12 || !out_w || !out_h)
		return -1;
	*out_nv12 = NULL;
	*out_w = 0;
	*out_h = 0;

	fp = fopen(path, "rb");
	if (!fp) {
		perror(path);
		return -1;
	}
	if (fseek(fp, 0, SEEK_END) != 0)
		goto out;
	file_sz = ftell(fp);
	if (file_sz <= 0 || file_sz > 32 * 1024 * 1024)
		goto out;
	if (fseek(fp, 0, SEEK_SET) != 0)
		goto out;
	file_buf = malloc((size_t)file_sz);
	if (!file_buf)
		goto out;
	if (fread(file_buf, 1, (size_t)file_sz, fp) != (size_t)file_sz)
		goto out;
	fclose(fp);
	fp = NULL;

	if (jpeg_probe_size(file_buf, (size_t)file_sz, &src_w, &src_h) != 0) {
		fprintf(stderr, "[still] jpeg probe failed: %s\n", path);
		goto out;
	}
	printf("[still] jpeg decode %s (%dx%d) -> packed NV12\n",
		path, src_w, src_h);

	memset(&ctx, 0, sizeof(ctx));
	ctx.encoder = 0;
	ctx.codec_id = MEDIA_CODEC_ID_JPEG;
	params = &ctx.video_dec_params;
	params->feed_mode = MC_FEEDING_MODE_FRAME_SIZE;
	params->pix_fmt = MC_PIXEL_FORMAT_NV12;
	params->bitstream_buf_size =
		(src_w * src_h * 3 / 2 + 0xfff) & ~0xfff;
	if (params->bitstream_buf_size < (int)file_sz + 4096)
		params->bitstream_buf_size =
			((int)file_sz + 0xfff) & ~0xfff;
	params->bitstream_buf_count = 3;
	params->frame_buf_count = 3;
	params->jpeg_dec_config.frame_crop_enable = 0;
	params->jpeg_dec_config.rot_degree = MC_CCW_0;
	params->jpeg_dec_config.mir_direction = MC_DIRECTION_NONE;

	ret = hb_mm_mc_initialize(&ctx);
	if (ret != 0) {
		fprintf(stderr, "[still] jpeg dec init fail 0x%x\n", ret);
		goto out;
	}
	ret = hb_mm_mc_configure(&ctx);
	if (ret != 0) {
		fprintf(stderr, "[still] jpeg dec configure fail 0x%x\n", ret);
		goto fail_rel;
	}
	ret = hb_mm_mc_start(&ctx, &startup);
	if (ret != 0) {
		fprintf(stderr, "[still] jpeg dec start fail 0x%x\n", ret);
		goto fail_rel;
	}

	ret = hb_mm_mc_dequeue_input_buffer(&ctx, &in_buf, 3000);
	if (ret != 0)
		goto fail_stop;
	if (in_buf.vstream_buf.size < (hb_s32)file_sz) {
		fprintf(stderr, "[still] stream buf too small (%d < %ld)\n",
			(int)in_buf.vstream_buf.size, file_sz);
		(void)hb_mm_mc_queue_input_buffer(&ctx, &in_buf, 1000);
		ret = -1;
		goto fail_stop;
	}
	in_buf.type = MC_VIDEO_STREAM_BUFFER;
	in_buf.vstream_buf.size = (hb_s32)file_sz;
	in_buf.vstream_buf.stream_end = 0;
	memcpy(in_buf.vstream_buf.vir_ptr, file_buf, (size_t)file_sz);
	ret = hb_mm_mc_queue_input_buffer(&ctx, &in_buf, 3000);
	if (ret != 0)
		goto fail_stop;

	memset(&in_buf, 0, sizeof(in_buf));
	ret = hb_mm_mc_dequeue_input_buffer(&ctx, &in_buf, 1000);
	if (ret == 0) {
		in_buf.type = MC_VIDEO_STREAM_BUFFER;
		in_buf.vstream_buf.size = 0;
		in_buf.vstream_buf.stream_end = 1;
		(void)hb_mm_mc_queue_input_buffer(&ctx, &in_buf, 1000);
	}

	memset(&out_buf, 0, sizeof(out_buf));
	memset(&info, 0, sizeof(info));
	ret = hb_mm_mc_dequeue_output_buffer(&ctx, &out_buf, &info, 5000);
	if (ret != 0) {
		fprintf(stderr, "[still] jpeg dec output fail 0x%x\n", ret);
		goto fail_stop;
	}
	if (out_buf.type != MC_VIDEO_FRAME_BUFFER ||
	    out_buf.vframe_buf.size <= 0) {
		(void)hb_mm_mc_queue_output_buffer(&ctx, &out_buf, 1000);
		ret = -1;
		goto fail_stop;
	}

	{
		int img_w = src_w;
		int img_h = src_h;
		int buf_w = out_buf.vframe_buf.width > 0 ?
			out_buf.vframe_buf.width : img_w;
		int buf_h = out_buf.vframe_buf.height > 0 ?
			out_buf.vframe_buf.height : img_h;
		int pitch = buf_w;
		int row;
		size_t packed_sz;

		if (info.jpeg_frame_info.display_width > 0)
			img_w = info.jpeg_frame_info.display_width;
		if (info.jpeg_frame_info.display_height > 0)
			img_h = info.jpeg_frame_info.display_height;
		if (img_w > buf_w)
			img_w = buf_w;
		if (img_h > buf_h)
			img_h = buf_h;
		if (out_buf.vframe_buf.stride > buf_w)
			pitch = out_buf.vframe_buf.stride;

		y_ptr = out_buf.vframe_buf.vir_ptr[0];
		uv_ptr = out_buf.vframe_buf.vir_ptr[1];
		if (!uv_ptr && y_ptr) {
			int y_rows = out_buf.vframe_buf.vstride >= img_h ?
				out_buf.vframe_buf.vstride : buf_h;

			uv_ptr = y_ptr + (size_t)pitch * (size_t)y_rows;
		}
		if (!y_ptr || !uv_ptr) {
			(void)hb_mm_mc_queue_output_buffer(&ctx, &out_buf, 1000);
			ret = -1;
			goto fail_stop;
		}

		printf("[still] jpeg nv12 buf=%dx%d pitch=%d vstride=%d "
			"content=%dx%d size=%d\n",
			buf_w, buf_h, pitch, out_buf.vframe_buf.vstride,
			img_w, img_h, (int)out_buf.vframe_buf.size);
		fflush(stdout);

		packed_sz = (size_t)img_w * (size_t)img_h * 3 / 2;
		packed = malloc(packed_sz);
		if (!packed) {
			(void)hb_mm_mc_queue_output_buffer(&ctx, &out_buf, 1000);
			ret = -1;
			goto fail_stop;
		}
		for (row = 0; row < img_h; row++)
			memcpy(packed + (size_t)row * (size_t)img_w,
				y_ptr + (size_t)row * (size_t)pitch,
				(size_t)img_w);
		for (row = 0; row < img_h / 2; row++)
			memcpy(packed + (size_t)img_w * (size_t)img_h +
					(size_t)row * (size_t)img_w,
				uv_ptr + (size_t)row * (size_t)pitch,
				(size_t)img_w);

		*out_nv12 = packed;
		*out_w = img_w;
		*out_h = img_h;
		packed = NULL;
	}
	(void)hb_mm_mc_queue_output_buffer(&ctx, &out_buf, 1000);
	ret = 0;

fail_stop:
	(void)hb_mm_mc_pause(&ctx);
fail_rel:
	(void)hb_mm_mc_release(&ctx);
out:
	if (fp)
		fclose(fp);
	free(file_buf);
	free(packed);
	return ret;
}

int media_still_decode_jpeg_rgb565(const char *path, uint16_t *dst,
		int dst_w, int dst_h)
{
	uint8_t *nv12 = NULL;
	int w = 0, h = 0;
	int ret;

	if (!path || !dst || dst_w <= 0 || dst_h <= 0)
		return -1;
	ret = media_still_decode_jpeg_nv12(path, &nv12, &w, &h);
	if (ret != 0 || !nv12)
		return -1;
	printf("[still] jpeg %s (%dx%d) -> rgb565 %dx%d\n",
		path, w, h, dst_w, dst_h);
	nv12_scale_to_rgb565(nv12, nv12 + (size_t)w * (size_t)h,
		w, h, w, w, dst, dst_w, dst_h, 1);
	free(nv12);
	return 0;
}

static void preview_sidecar_path(char *out, size_t out_len, const char *media_path)
{
	snprintf(out, out_len, "%s%s", media_path, CAM_PREV_EXT);
}

static int write_rgb565_sidecar(const char *side, const uint16_t *rgb,
		int w, int h)
{
	FILE *fp;
	uint32_t hdr[3];
	size_t px;

	if (!side || !rgb || w <= 0 || h <= 0)
		return -1;
	px = (size_t)w * (size_t)h;
	fp = fopen(side, "wb");
	if (!fp)
		return -1;
	hdr[0] = 0x35363550u; /* 'P''5''6''5' */
	hdr[1] = (uint32_t)w;
	hdr[2] = (uint32_t)h;
	if (fwrite(hdr, sizeof(hdr), 1, fp) != 1 ||
	    fwrite(rgb, sizeof(uint16_t), px, fp) != px) {
		fclose(fp);
		unlink(side);
		return -1;
	}
	fclose(fp);
	return 0;
}

static int load_rgb565_sidecar(const char *side, uint16_t *dst, int w, int h)
{
	FILE *fp;
	uint32_t hdr[3];
	size_t px;

	if (!side || !dst || w <= 0 || h <= 0)
		return -1;
	fp = fopen(side, "rb");
	if (!fp)
		return -1;
	if (fread(hdr, sizeof(hdr), 1, fp) != 1) {
		fclose(fp);
		return -1;
	}
	if (hdr[0] != 0x35363550u || (int)hdr[1] != w || (int)hdr[2] != h) {
		fclose(fp);
		return -1;
	}
	px = (size_t)w * (size_t)h;
	if (fread(dst, sizeof(uint16_t), px, fp) != px) {
		fclose(fp);
		return -1;
	}
	fclose(fp);
	return 0;
}

void media_still_video_poster_path(char *out, size_t out_len,
		const char *media_path)
{
	size_t n;

	if (!out || out_len == 0 || !media_path)
		return;
	n = strlen(media_path);
	if (n >= 4 &&
	    (strcasecmp(media_path + n - 4, ".mp4") == 0 ||
	     strcasecmp(media_path + n - 4, ".MP4") == 0)) {
		snprintf(out, out_len, "%.*s%s", (int)(n - 4), media_path,
			CAM_POSTER_EXT);
		return;
	}
	if (n >= 5 &&
	    (strcasecmp(media_path + n - 5, ".h264") == 0 ||
	     strcasecmp(media_path + n - 5, ".H264") == 0)) {
		snprintf(out, out_len, "%.*s%s", (int)(n - 5), media_path,
			CAM_POSTER_EXT);
		return;
	}
	snprintf(out, out_len, "%s%s", media_path, CAM_POSTER_EXT);
}

static void video_poster_path(char *out, size_t out_len, const char *media_path)
{
	media_still_video_poster_path(out, out_len, media_path);
}

int media_still_write_video_poster(const uint8_t *y, const uint8_t *uv,
		int w, int h, int stride_y, const char *media_path)
{
	char path[512];
	const char *eq;
	int quality = 85;
	int ret;
	uint8_t *copy = NULL;
	uint8_t *cy;
	uint8_t *cuv;
	size_t y_sz;
	size_t total;
	int row;

	if (!y || !uv || !media_path || w <= 0 || h <= 0)
		return -1;
	eq = getenv("CAM_JPEG_QUALITY");
	if (eq && eq[0])
		quality = atoi(eq);
	if (quality < 1)
		quality = 1;
	if (quality > 100)
		quality = 100;
	if (stride_y < w)
		stride_y = w;

	y_sz = (size_t)w * (size_t)h;
	total = y_sz + y_sz / 2;
	copy = malloc(total);
	if (!copy)
		return -1;
	cy = copy;
	cuv = copy + y_sz;
	for (row = 0; row < h; row++)
		memcpy(cy + (size_t)row * (size_t)w,
			y + (size_t)row * (size_t)stride_y, (size_t)w);
	for (row = 0; row < h / 2; row++)
		memcpy(cuv + (size_t)row * (size_t)w,
			uv + (size_t)row * (size_t)stride_y, (size_t)w);
	/*
	 * Plane priming needs sensor-sized NV12 JPEG (no cream, no encode
	 * rotate). Polaroid cream is for still captures only; album HW play
	 * blits this poster onto plane 33 before decode.
	 */
	video_poster_path(path, sizeof(path), media_path);
	ret = encode_jpeg_rot(cy, cuv, w, h, w, quality, path, 0);
	free(copy);
	if (ret == 0) {
		printf("[still] wrote video poster %s\n", path);
		fflush(stdout);
	} else {
		fprintf(stderr, "[still] video poster encode failed: %s (%d)\n",
			path, ret);
	}
	return ret;
}

void media_still_remove_video_poster(const char *media_path)
{
	char path[512];

	if (!media_path)
		return;
	video_poster_path(path, sizeof(path), media_path);
	(void)unlink(path);
}

static void video_anim_path(char *out, size_t out_len, const char *media_path)
{
	size_t n;

	if (!out || out_len == 0 || !media_path)
		return;
	n = strlen(media_path);
	if (n >= 4 && strcasecmp(media_path + n - 4, ".mp4") == 0) {
		snprintf(out, out_len, "%.*s%s", (int)(n - 4), media_path,
			CAM_ANIM_EXT);
		return;
	}
	if (n >= 5 && strcasecmp(media_path + n - 5, ".h264") == 0) {
		snprintf(out, out_len, "%.*s%s", (int)(n - 5), media_path,
			CAM_ANIM_EXT);
		return;
	}
	snprintf(out, out_len, "%s%s", media_path, CAM_ANIM_EXT);
}

int media_still_write_anim_thumb(const char *media_path,
		const uint16_t *frames, int nframes, int fps)
{
	char path[512];
	FILE *fp;
	uint32_t hdr[5];
	size_t px;
	size_t total;

	if (!media_path || !frames || nframes <= 0)
		return -1;
	if (nframes > CAM_ANIM_MAX_FRAMES)
		nframes = CAM_ANIM_MAX_FRAMES;
	if (fps <= 0)
		fps = CAM_ANIM_FPS;
	px = (size_t)CAM_THUMB_W * (size_t)CAM_THUMB_H;
	total = px * (size_t)nframes;
	video_anim_path(path, sizeof(path), media_path);
	fp = fopen(path, "wb");
	if (!fp)
		return -1;
	hdr[0] = 0x35363541u; /* 'A''5''6''5' */
	hdr[1] = (uint32_t)CAM_THUMB_W;
	hdr[2] = (uint32_t)CAM_THUMB_H;
	hdr[3] = (uint32_t)nframes;
	hdr[4] = (uint32_t)fps;
	if (fwrite(hdr, sizeof(hdr), 1, fp) != 1 ||
	    fwrite(frames, sizeof(uint16_t), total, fp) != total) {
		fclose(fp);
		unlink(path);
		return -1;
	}
	fclose(fp);
	printf("[still] wrote anim thumb %s (%d frames @%dfps)\n",
		path, nframes, fps);
	fflush(stdout);
	return 0;
}

int media_still_load_anim_thumb(const char *media_path, uint16_t **out_frames,
		int *out_nframes, int *out_fps)
{
	char path[512];
	FILE *fp;
	uint32_t hdr[5];
	uint16_t *buf = NULL;
	size_t px;
	size_t total;
	int nframes;
	int fps;

	if (!media_path || !out_frames || !out_nframes)
		return -1;
	*out_frames = NULL;
	*out_nframes = 0;
	if (out_fps)
		*out_fps = CAM_ANIM_FPS;

	video_anim_path(path, sizeof(path), media_path);
	fp = fopen(path, "rb");
	if (!fp)
		return -1;
	if (fread(hdr, sizeof(hdr), 1, fp) != 1 ||
	    hdr[0] != 0x35363541u ||
	    (int)hdr[1] != CAM_THUMB_W || (int)hdr[2] != CAM_THUMB_H) {
		fclose(fp);
		return -1;
	}
	nframes = (int)hdr[3];
	fps = (int)hdr[4];
	if (nframes <= 0 || nframes > CAM_ANIM_MAX_FRAMES || fps <= 0) {
		fclose(fp);
		return -1;
	}
	px = (size_t)CAM_THUMB_W * (size_t)CAM_THUMB_H;
	total = px * (size_t)nframes;
	buf = (uint16_t *)malloc(total * sizeof(uint16_t));
	if (!buf) {
		fclose(fp);
		return -1;
	}
	if (fread(buf, sizeof(uint16_t), total, fp) != total) {
		free(buf);
		fclose(fp);
		return -1;
	}
	fclose(fp);
	*out_frames = buf;
	*out_nframes = nframes;
	if (out_fps)
		*out_fps = fps;
	return 0;
}

void media_still_remove_anim_thumb(const char *media_path)
{
	char path[512];

	if (!media_path)
		return;
	video_anim_path(path, sizeof(path), media_path);
	(void)unlink(path);
}

static void video_vdur_path(char *out, size_t out_len, const char *media_path)
{
	size_t n;

	if (!out || out_len == 0 || !media_path)
		return;
	n = strlen(media_path);
	if (n >= 4 && strcasecmp(media_path + n - 4, ".mp4") == 0) {
		snprintf(out, out_len, "%.*s%s", (int)(n - 4), media_path,
			CAM_VDUR_EXT);
		return;
	}
	if (n >= 5 && strcasecmp(media_path + n - 5, ".h264") == 0) {
		snprintf(out, out_len, "%.*s%s", (int)(n - 5), media_path,
			CAM_VDUR_EXT);
		return;
	}
	snprintf(out, out_len, "%s%s", media_path, CAM_VDUR_EXT);
}

int media_still_write_video_duration(const char *media_path, int duration_sec)
{
	char path[512];
	FILE *fp;

	if (!media_path || duration_sec < 0)
		return -1;
	video_vdur_path(path, sizeof(path), media_path);
	fp = fopen(path, "wb");
	if (!fp)
		return -1;
	if (fprintf(fp, "%d\n", duration_sec) < 0) {
		fclose(fp);
		unlink(path);
		return -1;
	}
	fclose(fp);
	return 0;
}

int media_still_load_video_duration(const char *media_path)
{
	char path[512];
	FILE *fp;
	int sec = -1;

	if (!media_path)
		return -1;
	video_vdur_path(path, sizeof(path), media_path);
	fp = fopen(path, "rb");
	if (!fp)
		return -1;
	if (fscanf(fp, "%d", &sec) != 1 || sec < 0)
		sec = -1;
	fclose(fp);
	return sec;
}

void media_still_remove_video_duration(const char *media_path)
{
	char path[512];

	if (!media_path)
		return;
	video_vdur_path(path, sizeof(path), media_path);
	(void)unlink(path);
}

/*
 * Video album preview: optional .p565, else REC-time .poster.jpg → RGB565.
 * Board ffmpeg is remux-only (no image2/rawvideo/mjpeg) and cannot extract.
 */
int media_still_decode_video_rgb565(const char *path, uint16_t *dst,
		int dst_w, int dst_h)
{
	char side[512];
	char poster[512];
	int ret;

	if (!path || !path[0] || !dst || dst_w <= 0 || dst_h <= 0)
		return -1;

	if (sidecar_enabled()) {
		preview_sidecar_path(side, sizeof(side), path);
		if (load_rgb565_sidecar(side, dst, dst_w, dst_h) == 0) {
			printf("[still] video preview from sidecar %s\n", side);
			fflush(stdout);
			return 0;
		}
	}

	video_poster_path(poster, sizeof(poster), path);
	ret = media_still_decode_jpeg_rgb565(poster, dst, dst_w, dst_h);
	if (ret != 0) {
		fprintf(stderr,
			"[still] video preview: no poster %s (re-record to create)\n",
			poster);
		return -1;
	}

	if (sidecar_enabled()) {
		preview_sidecar_path(side, sizeof(side), path);
		if (write_rgb565_sidecar(side, dst, dst_w, dst_h) == 0) {
			printf("[still] cached video preview %s\n", side);
			fflush(stdout);
		}
	}
	return 0;
}

int media_still_write_rgb565_preview(const uint8_t *y, const uint8_t *uv,
		int src_w, int src_h, int stride_y, const char *media_path,
		int dst_w, int dst_h)
{
	char side[512];
	uint16_t *rgb = NULL;
	size_t px;
	int ret = -1;

	if (!sidecar_enabled())
		return 0;
	if (!y || !uv || !media_path || src_w <= 0 || src_h <= 0 ||
	    dst_w <= 0 || dst_h <= 0)
		return -1;
	if (stride_y < src_w)
		stride_y = src_w;

	px = (size_t)dst_w * (size_t)dst_h;
	rgb = malloc(px * sizeof(uint16_t));
	if (!rgb)
		return -1;

	/* fill=1: stretch like live video plane (1920x1080 → 720x1280). */
	nv12_scale_to_rgb565(y, uv, src_w, src_h, stride_y, stride_y,
		rgb, dst_w, dst_h, 1);

	preview_sidecar_path(side, sizeof(side), media_path);
	if (write_rgb565_sidecar(side, rgb, dst_w, dst_h) == 0) {
		ret = 0;
		printf("[still] wrote preview sidecar %s (%dx%d stretch)\n",
			side, dst_w, dst_h);
		fflush(stdout);
	}

	free(rgb);
	return ret;
}

int media_still_load_rgb565_preview(const char *media_path, uint16_t *dst,
		int dst_w, int dst_h)
{
	char side[512];

	if (!sidecar_enabled())
		return -1;
	if (!media_path || !dst || dst_w <= 0 || dst_h <= 0)
		return -1;

	preview_sidecar_path(side, sizeof(side), media_path);
	if (load_rgb565_sidecar(side, dst, dst_w, dst_h) != 0)
		return -1;
	printf("[still] loaded preview sidecar %s\n", side);
	fflush(stdout);
	return 0;
}

void media_still_remove_rgb565_preview(const char *media_path)
{
	char side[512];

	if (!media_path)
		return;
	preview_sidecar_path(side, sizeof(side), media_path);
	(void)unlink(side);
}

void media_still_nv12_thumb(const uint8_t *y, const uint8_t *uv,
		int w, int h, uint16_t *rgb565)
{
	int dst_x, dst_y;

	if (!y || !uv || !rgb565 || w <= 0 || h <= 0)
		return;

	for (dst_y = 0; dst_y < CAM_THUMB_H; dst_y++) {
		int sy = dst_y * h / CAM_THUMB_H;

		for (dst_x = 0; dst_x < CAM_THUMB_W; dst_x++) {
			int sx = dst_x * w / CAM_THUMB_W;
			int Y = y[sy * w + sx];
			int uv_i = (sy / 2) * w + (sx & ~1);
			int U = uv[uv_i] - 128;
			int V = uv[uv_i + 1] - 128;
			int r = Y + ((V * 359) >> 8);
			int g = Y - ((U * 88 + V * 183) >> 8);
			int b = Y + ((U * 454) >> 8);

			if (r < 0)
				r = 0;
			if (r > 255)
				r = 255;
			if (g < 0)
				g = 0;
			if (g > 255)
				g = 255;
			if (b < 0)
				b = 0;
			if (b > 255)
				b = 255;
			rgb565[dst_y * CAM_THUMB_W + dst_x] = rgb_to_565(r, g, b);
		}
	}
}

void media_still_rgb565_orient_view(const uint16_t *src, uint16_t *dst,
		int *out_w, int *out_h)
{
	int deg;
	int w = CAM_THUMB_W;
	int h = CAM_THUMB_H;
	int x, y;

	if (!src || !dst)
		return;
	deg = video_rotate_deg();
	if (deg != 0 && deg != 90 && deg != 180 && deg != 270)
		deg = 90;

	if (deg == 0) {
		memcpy(dst, src, (size_t)w * (size_t)h * sizeof(uint16_t));
		if (out_w)
			*out_w = w;
		if (out_h)
			*out_h = h;
		return;
	}
	if (deg == 180) {
		for (y = 0; y < h; y++)
			for (x = 0; x < w; x++)
				dst[(h - 1 - y) * w + (w - 1 - x)] =
					src[y * w + x];
		if (out_w)
			*out_w = w;
		if (out_h)
			*out_h = h;
		return;
	}
	/*
	 * 90: CW90 in y-down (= previous CCW90 result + 180°, per device check).
	 * 270: CCW90.
	 */
	if (deg == 90) {
		for (y = 0; y < h; y++)
			for (x = 0; x < w; x++)
				dst[x * h + (h - 1 - y)] = src[y * w + x];
		if (out_w)
			*out_w = h;
		if (out_h)
			*out_h = w;
		return;
	}
	/* 270 */
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
			dst[(w - 1 - x) * h + y] = src[y * w + x];
	if (out_w)
		*out_w = h;
	if (out_h)
		*out_h = w;
}

int media_still_jpeg_to_list_thumb(const char *jpeg_path, uint16_t *dst)
{
	uint8_t *nv12 = NULL;
	int w = 0, h = 0;
	int ret;

	if (!jpeg_path || !dst)
		return -1;
	ret = media_still_decode_jpeg_nv12(jpeg_path, &nv12, &w, &h);
	if (ret != 0 || !nv12)
		return -1;
	media_still_nv12_thumb(nv12, nv12 + (size_t)w * (size_t)h, w, h, dst);
	free(nv12);
	return 0;
}

int media_still_video_to_list_thumb(const char *video_path, uint16_t *dst)
{
	char poster[512];

	if (!video_path || !dst)
		return -1;
	video_poster_path(poster, sizeof(poster), video_path);
	return media_still_jpeg_to_list_thumb(poster, dst);
}

static void thumb_sidecar_path(char *out, size_t out_len, const char *media_path)
{
	snprintf(out, out_len, "%s%s", media_path, CAM_THUMB_EXT);
}

int media_still_write_list_thumb(const char *media_path,
		const uint16_t *rgb565)
{
	char side[512];
	FILE *fp;
	uint32_t hdr[3];
	size_t px = (size_t)CAM_THUMB_W * (size_t)CAM_THUMB_H;

	if (!sidecar_enabled())
		return 0;
	if (!media_path || !rgb565)
		return -1;
	thumb_sidecar_path(side, sizeof(side), media_path);
	fp = fopen(side, "wb");
	if (!fp)
		return -1;
	hdr[0] = 0x35363554u; /* 'T''5''6''5' */
	hdr[1] = (uint32_t)CAM_THUMB_W;
	hdr[2] = (uint32_t)CAM_THUMB_H;
	if (fwrite(hdr, sizeof(hdr), 1, fp) != 1 ||
	    fwrite(rgb565, sizeof(uint16_t), px, fp) != px) {
		fclose(fp);
		unlink(side);
		return -1;
	}
	fclose(fp);
	return 0;
}

int media_still_load_list_thumb(const char *media_path, uint16_t *dst)
{
	char side[512];
	FILE *fp;
	uint32_t hdr[3];
	size_t px = (size_t)CAM_THUMB_W * (size_t)CAM_THUMB_H;

	if (!sidecar_enabled())
		return -1;
	if (!media_path || !dst)
		return -1;
	thumb_sidecar_path(side, sizeof(side), media_path);
	fp = fopen(side, "rb");
	if (!fp)
		return -1;
	if (fread(hdr, sizeof(hdr), 1, fp) != 1 ||
	    hdr[0] != 0x35363554u ||
	    (int)hdr[1] != CAM_THUMB_W || (int)hdr[2] != CAM_THUMB_H ||
	    fread(dst, sizeof(uint16_t), px, fp) != px) {
		fclose(fp);
		return -1;
	}
	fclose(fp);
	return 0;
}

void media_still_remove_list_thumb(const char *media_path)
{
	char side[512];

	if (!media_path)
		return;
	thumb_sidecar_path(side, sizeof(side), media_path);
	(void)unlink(side);
}

int media_still_confine_path(const char *save_dir, const char *path,
		char *out, size_t out_len, int for_unlink)
{
	char real_dir[PATH_MAX];
	char real_file[PATH_MAX];
	char parent[PATH_MAX];
	char real_parent[PATH_MAX];
	const char *slash;
	const char *base;
	size_t dlen;
	struct stat st;

	if (!save_dir || !path || save_dir[0] != '/' || path[0] != '/')
		return -1;
	if (!realpath(save_dir, real_dir))
		return -1;
	dlen = strlen(real_dir);
	while (dlen > 1 && real_dir[dlen - 1] == '/') {
		real_dir[dlen - 1] = '\0';
		dlen--;
	}

	if (realpath(path, real_file)) {
		size_t flen = strlen(real_file);

		if (flen >= dlen &&
				strncmp(real_file, real_dir, dlen) == 0 &&
				(real_file[dlen] == '\0' ||
				 real_file[dlen] == '/')) {
			if (out && out_len)
				snprintf(out, out_len, "%s", real_file);
			return 0;
		}
		if (!for_unlink)
			return -1;
		/* Target outside save_dir — fall through for in-dir symlink unlink. */
	} else if (!for_unlink) {
		return -1;
	}

	slash = strrchr(path, '/');
	if (!slash || slash[1] == '\0')
		return -1;
	base = slash + 1;
	if (strcmp(base, ".") == 0 || strcmp(base, "..") == 0)
		return -1;
	if (slash == path)
		snprintf(parent, sizeof(parent), "/");
	else if ((size_t)(slash - path) >= sizeof(parent))
		return -1;
	else {
		memcpy(parent, path, (size_t)(slash - path));
		parent[slash - path] = '\0';
	}
	if (!realpath(parent, real_parent))
		return -1;
	if (strcmp(real_parent, real_dir) != 0)
		return -1;
	if (lstat(path, &st) != 0)
		return -1;
	if (out && out_len) {
		if (dlen == 1 && real_dir[0] == '/')
			snprintf(out, out_len, "/%s", base);
		else
			snprintf(out, out_len, "%s/%s", real_dir, base);
	}
	return 0;
}
