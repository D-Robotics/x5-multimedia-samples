#include "ui_camera.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "lvgl_display.h"
#include "sys_status.h"

/*
 * Polaroid C-style finder UI (classic primary, short-rec secondary).
 *
 * User view (BAR_ROT180 + CTRL=top): FB-top ctrl = user bottom cream bar;
 * FB-bottom caption = user top cream. Pixel baseline 720x1280:
 *   top cream 48 | preview | bottom bar 152; shutter Ø88; rec Ø56; album 64.
 *
 * Env (layout):
 *   UI_CTRL_POS / UI_STATUS_POS / UI_CTRL_MARGIN / UI_CTRL_BAR_H /
 *   UI_SIDE_BAR_W / UI_STATUS_BAR_H / UI_BTN_HIT_PAD / UI_SWAP_BUTTONS
 *   UI_POLAROID=0   disable film frame + cream (default 1)
 *   UI_FRAME_W      FB-top cream above ctrl (default 0; bottom bar is cream)
 *   UI_FRAME_L/R    visible L/R cream (default 16 / 16; equal — like album)
 *   UI_OVERSCAN_L/R panel clip / touch (default 0 / 40; not added into cream)
 *   UI_OVERSCAN_Y   top FB rows clipped (default 0)
 *   UI_BAR_ROT180=1 rotate chrome glyphs
 *   UI_DEBUG_META=1 show WxH|fps on caption
 *   CAM_FILM_KB     force KB/shot. Default: resolution formula, then last JPEG
 *   CAM_FILM_MAX    UI clamp for the badge (default 9999; 0 = no clamp)
 */
#define UI_CTRL_BAR_H_DEFAULT     152
#define UI_STATUS_BAR_H_DEFAULT   56
#define UI_SIDE_BAR_W_DEFAULT     96
#define UI_CTRL_MARGIN_DEFAULT    0
#define UI_BTN_HIT_PAD_DEFAULT    20
#define UI_FRAME_W_DEFAULT        0
#define UI_FRAME_L_DEFAULT        16
#define UI_FRAME_R_DEFAULT        16
#define UI_OVERSCAN_L_DEFAULT     0
#define UI_OVERSCAN_R_DEFAULT     40
#define UI_OVERSCAN_Y_DEFAULT     0
#define UI_CAPTION_H_DEFAULT      56

#define UI_SHUTTER_D_DEFAULT      88
#define UI_ALBUM_BTN_DEFAULT      64
#define UI_REC_BTN_DEFAULT        56
#define UI_BTN_SEP_DEFAULT        264

#define UI_THUMB_CARD_W           (CAM_THUMB_W + 24)
#define UI_THUMB_CARD_H           (CAM_THUMB_H + 48)

/* Polaroid paper tone */
#define UI_PAPER_COLOR   0xF4EFE6
#define UI_INK_COLOR     0x2A2A2A
#define UI_REC_RED       0xE03131

typedef enum {
	UI_POS_TOP = 0,
	UI_POS_BOTTOM,
	UI_POS_LEFT,
	UI_POS_RIGHT,
} ui_pos_t;

static ui_camera_t *g_ui;

static void anim_zoom_cb(void *obj, int32_t v);
static void thumb_hide_cb(lv_timer_t *t);
static void refresh_film_badge(ui_camera_t *ui);

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

/* Rotate a widget 180° around its center (product physical flip).
 * Re-call after text/size changes — pivot must match the current bbox or
 * labels walk right and get clipped (Shot # / res|fps).
 */
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
	lv_obj_set_style_transform_angle(obj, 1800, 0); /* 180.0 deg */
}

static int bar_rot180_on(void)
{
	return env_on("UI_BAR_ROT180", 1);
}

/*
 * Control-bar FB Y.
 * UI_BAR_ROT180 only spins glyphs: product mount makes FB-top the user's
 * bottom bar — keep Album/Capture/REC on UI_CTRL_POS (default top), do not
 * drag them down onto the cream caption.
 */
static int ui_ctrl_bar_y(ui_pos_t ctrl, ui_pos_t status, lv_coord_t scr_h,
		int margin, int edge_t, int ctrl_h, int status_h)
{
	(void)status;
	if (ctrl == UI_POS_BOTTOM)
		return (int)scr_h - status_h - margin - ctrl_h;
	return margin + edge_t;
}

/* Keep transform valid after label text (and thus size) changes. */
static void maybe_rerotate(lv_obj_t *obj)
{
	if (bar_rot180_on())
		rotate_widget_180(obj);
}

static ui_pos_t parse_pos(const char *v, ui_pos_t def)
{
	if (!v || !v[0])
		return def;
	if (v[0] == 't' || v[0] == 'T')
		return UI_POS_TOP;
	if (v[0] == 'b' || v[0] == 'B')
		return UI_POS_BOTTOM;
	if (v[0] == 'l' || v[0] == 'L')
		return UI_POS_LEFT;
	if (v[0] == 'r' || v[0] == 'R')
		return UI_POS_RIGHT;
	return def;
}

static ui_pos_t ui_ctrl_pos(void)
{
	return parse_pos(getenv("UI_CTRL_POS"), UI_POS_TOP);
}

static ui_pos_t ui_status_pos(ui_pos_t ctrl)
{
	const char *v = getenv("UI_STATUS_POS");

	if (v && v[0])
		return parse_pos(v, UI_POS_BOTTOM);
	if (ctrl == UI_POS_BOTTOM)
		return UI_POS_TOP;
	return UI_POS_BOTTOM;
}

static void format_rec_time(char *buf, size_t len, uint32_t elapsed_sec)
{
	uint32_t h = elapsed_sec / 3600;
	uint32_t m = (elapsed_sec % 3600) / 60;
	uint32_t s = elapsed_sec % 60;

	if (h > 0)
		snprintf(buf, len, "%u:%02u:%02u", h, m, s);
	else
		snprintf(buf, len, "%02u:%02u", m, s);
}

static void record_do_async(void *user_data);

static void format_video_meta(ui_camera_t *ui, char *buf, size_t len)
{
	if (!ui || ui->video_w <= 0 || ui->video_h <= 0) {
		snprintf(buf, len, "--");
		return;
	}
	/* Use ASCII separators only — Montserrat lacks U+00B7 middle-dot glyphs. */
	snprintf(buf, len, "%dx%d | %dfps", ui->video_w, ui->video_h,
		ui->video_fps > 0 ? ui->video_fps : 30);
}

static void refresh_caption_idle(ui_camera_t *ui)
{
	char meta[64];

	if (!ui)
		return;
	/* C style: no sensor name / res on main cream. Debug meta optional. */
	if (ui->meta_label && env_on("UI_DEBUG_META", 0)) {
		format_video_meta(ui, meta, sizeof(meta));
		lv_label_set_text_fmt(ui->meta_label, "%s", meta);
		lv_obj_clear_flag(ui->meta_label, LV_OBJ_FLAG_HIDDEN);
		maybe_rerotate(ui->meta_label);
	} else if (ui->meta_label) {
		lv_obj_add_flag(ui->meta_label, LV_OBJ_FLAG_HIDDEN);
	}
	if (ui->status_label && !ui->recording && ui->job_kind == 0) {
		lv_label_set_text(ui->status_label, "");
		lv_obj_add_flag(ui->status_label, LV_OBJ_FLAG_HIDDEN);
	}
}

static void ensure_rec_hud(ui_camera_t *ui)
{
	lv_obj_t *row;
	lv_coord_t scr_h;
	int status_h;
	int caption_h;
	lv_coord_t y;

	if (!ui || ui->rec_hud)
		return;

	/*
	 * C: light HUD on user-top cream. With BAR_ROT180 that is FB bottom
	 * (caption). Place just above the caption toward the preview.
	 */
	scr_h = lv_disp_get_ver_res(NULL);
	status_h = env_int("UI_STATUS_BAR_H", UI_STATUS_BAR_H_DEFAULT);
	caption_h = env_int("UI_CAPTION_H", UI_CAPTION_H_DEFAULT);
	if (status_h < caption_h)
		status_h = caption_h;
	if (bar_rot180_on())
		y = (lv_coord_t)(scr_h - status_h - 40);
	else
		y = (lv_coord_t)(status_h + 8);
	if (y < 8)
		y = 8;

	ui->rec_hud = lv_obj_create(lv_scr_act());
	lv_obj_set_size(ui->rec_hud, LV_SIZE_CONTENT, 28);
	lv_obj_set_style_bg_color(ui->rec_hud, lv_color_hex(0x1A1A1A), 0);
	lv_obj_set_style_bg_opa(ui->rec_hud, LV_OPA_60, 0);
	lv_obj_set_style_radius(ui->rec_hud, 14, 0);
	lv_obj_set_style_border_width(ui->rec_hud, 0, 0);
	lv_obj_set_style_pad_hor(ui->rec_hud, 12, 0);
	lv_obj_set_style_pad_ver(ui->rec_hud, 4, 0);
	lv_obj_clear_flag(ui->rec_hud, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_clear_flag(ui->rec_hud, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_pos(ui->rec_hud, 24, y);
	lv_obj_add_flag(ui->rec_hud, LV_OBJ_FLAG_HIDDEN);

	row = lv_obj_create(ui->rec_hud);
	lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
		LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, 8, 0);
	lv_obj_center(row);
	lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

	ui->rec_dot = lv_obj_create(row);
	lv_obj_set_size(ui->rec_dot, 8, 8);
	lv_obj_set_style_radius(ui->rec_dot, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(ui->rec_dot, lv_color_hex(UI_REC_RED), 0);
	lv_obj_set_style_bg_opa(ui->rec_dot, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(ui->rec_dot, 0, 0);
	lv_obj_clear_flag(ui->rec_dot, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_clear_flag(ui->rec_dot, LV_OBJ_FLAG_CLICKABLE);

	ui->rec_time_label = lv_label_create(row);
	lv_label_set_text(ui->rec_time_label, "0:00");
	lv_obj_set_style_text_color(ui->rec_time_label, lv_color_white(), 0);
	lv_obj_set_style_text_font(ui->rec_time_label, &lv_font_montserrat_20, 0);
	lv_obj_clear_flag(ui->rec_time_label, LV_OBJ_FLAG_CLICKABLE);

	/* C: no WxH|fps; keep widget hidden for optional debug. */
	ui->rec_meta_label = lv_label_create(row);
	lv_label_set_text(ui->rec_meta_label, "");
	lv_obj_add_flag(ui->rec_meta_label, LV_OBJ_FLAG_HIDDEN);
	lv_obj_clear_flag(ui->rec_meta_label, LV_OBJ_FLAG_CLICKABLE);

	if (bar_rot180_on())
		rotate_widget_180(ui->rec_hud);
}

static void rec_timer_cb(lv_timer_t *t)
{
	ui_camera_t *ui = lv_timer_get_user_data(t);
	uint32_t elapsed_ms;
	uint32_t sec;
	char tbuf[16];
	int abort_reason;

	if (!ui || !ui->recording || !ui->rec_time_label)
		return;

	/* Pipeline stopped encode (storage/mux) — finalize on UI thread. */
	if (ui->on_rec_abort && ui->job_kind != 2 &&
	    ui->rec_auto_stop_reason == 0) {
		abort_reason = ui->on_rec_abort(ui->ops_user);
		if (abort_reason > 0) {
			ui->rec_auto_stop_reason = abort_reason;
			if (lv_async_call(record_do_async, ui) != LV_RESULT_OK)
				record_do_async(ui);
			return;
		}
	}

	elapsed_ms = lv_tick_elaps(ui->rec_start_ms);
	sec = elapsed_ms / 1000;

	/* Blink REC tip ~2 Hz */
	ui->rec_dot_on = !ui->rec_dot_on;
	if (ui->rec_dot)
		lv_obj_set_style_bg_opa(ui->rec_dot,
			ui->rec_dot_on ? LV_OPA_COVER : LV_OPA_30, 0);

	if (sec != ui->rec_last_sec) {
		ui->rec_last_sec = sec;
		format_rec_time(tbuf, sizeof(tbuf), sec);
		lv_label_set_text(ui->rec_time_label, tbuf);
		/* Time lives on rec_hud only — avoid duplicate center toast. */
	}
}

static void rec_hud_start(ui_camera_t *ui)
{
	char tbuf[16];

	if (!ui)
		return;

	ensure_rec_hud(ui);
	if (!ui->rec_hud)
		return;

	ui->rec_start_ms = lv_tick_get();
	ui->rec_last_sec = UINT32_MAX;
	ui->rec_dot_on = 1;
	format_rec_time(tbuf, sizeof(tbuf), 0);
	lv_label_set_text(ui->rec_time_label, tbuf);
	if (ui->rec_dot)
		lv_obj_set_style_bg_opa(ui->rec_dot, LV_OPA_COVER, 0);
	lv_obj_clear_flag(ui->rec_hud, LV_OBJ_FLAG_HIDDEN);

	if (!ui->rec_timer)
		ui->rec_timer = lv_timer_create(rec_timer_cb, 250, ui);
	else
		lv_timer_resume(ui->rec_timer);
	lv_timer_reset(ui->rec_timer);
	rec_timer_cb(ui->rec_timer);
}

static void rec_hud_stop(ui_camera_t *ui, char *out, size_t out_len)
{
	uint32_t sec;

	if (!ui)
		return;

	sec = lv_tick_elaps(ui->rec_start_ms) / 1000;
	if (out && out_len)
		format_rec_time(out, out_len, sec);

	if (ui->rec_timer)
		lv_timer_pause(ui->rec_timer);
	if (ui->rec_hud)
		lv_obj_add_flag(ui->rec_hud, LV_OBJ_FLAG_HIDDEN);
}

static void style_bar(lv_obj_t *bar, lv_coord_t radius)
{
	/* C: cream control plate (not dark glass). */
	lv_obj_set_style_bg_color(bar, lv_color_hex(UI_PAPER_COLOR), 0);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(bar, 0, 0);
	lv_obj_set_style_radius(bar, radius, 0);
	lv_obj_set_style_pad_all(bar, 8, 0);
	lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
}

static void style_touch_btn(lv_obj_t *btn, int hit_pad)
{
	lv_coord_t w;
	lv_coord_t h;

	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_pad_all(btn, 0, 0);
	/* Only expand when caller asks; default 0 so gaps ignore touches. */
	lv_obj_set_ext_click_area(btn, hit_pad > 0 ? hit_pad : 0);
	lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
	/* Pressed: darken + slight shrink so taps feel acknowledged. */
	lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
	lv_obj_set_style_transform_zoom(btn, 230, LV_STATE_PRESSED);
	lv_obj_update_layout(btn);
	w = lv_obj_get_width(btn);
	h = lv_obj_get_height(btn);
	if (w > 0 && h > 0) {
		lv_obj_set_style_transform_pivot_x(btn, w / 2, 0);
		lv_obj_set_style_transform_pivot_y(btn, h / 2, 0);
	}
}

static void gallery_btn_set_flash(ui_camera_t *ui, int on)
{
	lv_obj_t *lbl;

	if (!ui || !ui->gallery_btn)
		return;
	lbl = lv_obj_get_child(ui->gallery_btn, 0);
	if (on) {
		lv_obj_set_style_bg_color(ui->gallery_btn,
			lv_color_hex(0xE8E0D4), 0);
		lv_obj_set_style_transform_zoom(ui->gallery_btn, 242, 0);
		if (lbl)
			lv_obj_set_style_text_color(lbl,
				lv_color_hex(UI_INK_COLOR), 0);
	} else {
		lv_obj_set_style_bg_color(ui->gallery_btn,
			lv_color_hex(UI_PAPER_COLOR), 0);
		lv_obj_set_style_transform_zoom(ui->gallery_btn, 256, 0);
		if (lbl)
			lv_obj_set_style_text_color(lbl,
				lv_color_hex(UI_INK_COLOR), 0);
	}
	lv_obj_invalidate(ui->gallery_btn);
}

static void gallery_btn_do_open(ui_camera_t *ui)
{
	const char *dir;

	if (!ui)
		return;
	if (ui->recording || ui->job_kind == 2) {
		ui_camera_set_status(ui,
			ui->recording ? "Stop REC before album" : "Busy — wait");
		return;
	}
	if (ui->job_kind == 1) {
		ui_camera_set_status(ui, "Wait for captures");
		return;
	}
	if (ui->on_save_dir) {
		dir = ui->on_save_dir(ui->ops_user);
		ui_gallery_set_save_dir(&ui->gallery, dir);
	} else {
		ui_gallery_set_save_dir(&ui->gallery, NULL);
	}
	if (ui_gallery_is_visible(&ui->gallery)) {
		ui_gallery_hide(&ui->gallery);
		printf("[ui] album hide\n");
	} else {
		ui_gallery_show(&ui->gallery);
	}
}

static void gallery_tap_timer_cb(lv_timer_t *t)
{
	ui_camera_t *ui = lv_timer_get_user_data(t);

	if (!ui)
		return;
	ui->gallery_tap_timer = NULL;
	gallery_btn_set_flash(ui, 0);
	gallery_btn_do_open(ui);
}

static void place_preview_toast(ui_camera_t *ui)
{
	lv_coord_t scr_h;
	int status_h;
	int caption_h;
	int edge_r;
	int edge_l;

	if (!ui || !ui->status_label)
		return;

	scr_h = lv_disp_get_ver_res(NULL);
	status_h = env_int("UI_STATUS_BAR_H", UI_STATUS_BAR_H_DEFAULT);
	caption_h = env_int("UI_CAPTION_H", UI_CAPTION_H_DEFAULT);
	if (status_h < caption_h)
		status_h = caption_h;
	edge_l = env_int("UI_FRAME_L", UI_FRAME_L_DEFAULT);
	edge_r = env_int("UI_FRAME_R", UI_FRAME_R_DEFAULT);

	/*
	 * Float on the preview (not inside cream caption). FB bottom cream
	 * paint would cover caption children; toast sits just into the video.
	 * User-top ≈ FB-bottom when BAR_ROT180.
	 */
	if (bar_rot180_on()) {
		lv_obj_align(ui->status_label, LV_ALIGN_BOTTOM_MID, 0,
			-(status_h + 10));
		rotate_widget_180(ui->status_label);
	} else {
		lv_obj_align(ui->status_label, LV_ALIGN_TOP_MID, 0,
			status_h + 10);
	}
	(void)edge_l;
	(void)edge_r;
	(void)scr_h;
}

void ui_camera_set_status(ui_camera_t *ui, const char *text)
{
	if (!ui || !ui->status_label || !text)
		return;
	lv_label_set_text(ui->status_label, text);
	if (text[0]) {
		lv_obj_clear_flag(ui->status_label, LV_OBJ_FLAG_HIDDEN);
		lv_obj_move_foreground(ui->status_label);
		place_preview_toast(ui);
	} else {
		lv_obj_add_flag(ui->status_label, LV_OBJ_FLAG_HIDDEN);
	}
	if (ui->status_timer) {
		lv_timer_del(ui->status_timer);
		ui->status_timer = NULL;
	}
}

static void status_clear_cb(lv_timer_t *t)
{
	ui_camera_t *ui = lv_timer_get_user_data(t);

	if (!ui)
		return;
	ui->status_timer = NULL;
	if (ui->status_label && !ui->recording) {
		lv_label_set_text(ui->status_label, "");
		lv_obj_add_flag(ui->status_label, LV_OBJ_FLAG_HIDDEN);
	}
}

/* Brief preview HUD toast — auto-clears; not drawn under cream FB paint. */
static void ui_camera_toast(ui_camera_t *ui, const char *text, uint32_t ms)
{
	if (!ui || !text)
		return;
	ui_camera_set_status(ui, text);
	if (ms == 0)
		return;
	if (ui->status_timer)
		lv_timer_del(ui->status_timer);
	ui->status_timer = lv_timer_create(status_clear_cb, ms, ui);
	lv_timer_set_repeat_count(ui->status_timer, 1);
}

#define UI_ALERT_AUTO_MS 5000

static void alert_hide(ui_camera_t *ui)
{
	if (!ui)
		return;
	if (ui->alert_timer) {
		lv_timer_del(ui->alert_timer);
		ui->alert_timer = NULL;
	}
	if (ui->alert_overlay)
		lv_obj_add_flag(ui->alert_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void alert_timer_cb(lv_timer_t *t)
{
	ui_camera_t *ui = lv_timer_get_user_data(t);

	if (!ui)
		return;
	ui->alert_timer = NULL;
	alert_hide(ui);
}

static void alert_overlay_click_cb(lv_event_t *e)
{
	ui_camera_t *ui = lv_event_get_user_data(e);

	if (lv_event_get_code(e) != LV_EVENT_PRESSED)
		return;
	alert_hide(ui);
}

static void ensure_alert_modal(ui_camera_t *ui)
{
	lv_obj_t *scr;
	lv_obj_t *card;
	lv_obj_t *title;
	lv_obj_t *msg;

	if (!ui || ui->alert_overlay)
		return;

	scr = lv_scr_act();
	ui->alert_overlay = lv_obj_create(scr);
	lv_obj_remove_style_all(ui->alert_overlay);
	lv_obj_set_size(ui->alert_overlay, lv_pct(100), lv_pct(100));
	lv_obj_set_style_bg_color(ui->alert_overlay, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(ui->alert_overlay, LV_OPA_50, 0);
	lv_obj_add_flag(ui->alert_overlay, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_clear_flag(ui->alert_overlay, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(ui->alert_overlay, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_event_cb(ui->alert_overlay, alert_overlay_click_cb,
		LV_EVENT_PRESSED, ui);

	card = lv_obj_create(ui->alert_overlay);
	lv_obj_remove_style_all(card);
	lv_obj_set_size(card, 420, 160);
	lv_obj_center(card);
	lv_obj_set_style_bg_color(card, lv_color_hex(UI_PAPER_COLOR), 0);
	lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(card, 12, 0);
	lv_obj_set_style_pad_all(card, 20, 0);
	lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);
	ui->alert_card = card;

	title = lv_label_create(card);
	lv_label_set_text(title, "");
	lv_obj_set_style_text_color(title, lv_color_hex(UI_INK_COLOR), 0);
	lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
	lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_width(title, lv_pct(100));
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);
	ui->alert_title = title;

	msg = lv_label_create(card);
	lv_label_set_text(msg, "");
	lv_obj_set_style_text_color(msg, lv_color_hex(0x555555), 0);
	lv_obj_set_style_text_font(msg, &lv_font_montserrat_14, 0);
	lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_width(msg, lv_pct(100));
	lv_obj_align(msg, LV_ALIGN_BOTTOM_MID, 0, -20);
	ui->alert_msg = msg;

	if (bar_rot180_on())
		rotate_widget_180(card);
}

/* Modal alert: title + msg, auto-hide after 3s (tap dim area to dismiss). */
static void ui_camera_alert(ui_camera_t *ui, const char *title,
		const char *msg)
{
	if (!ui || !title)
		return;
	ensure_alert_modal(ui);
	if (!ui->alert_overlay || !ui->alert_title)
		return;

	lv_label_set_text(ui->alert_title, title);
	if (ui->alert_msg)
		lv_label_set_text(ui->alert_msg, msg && msg[0] ? msg : "");
	lv_obj_clear_flag(ui->alert_overlay, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_foreground(ui->alert_overlay);

	if (ui->alert_timer) {
		lv_timer_del(ui->alert_timer);
		ui->alert_timer = NULL;
	}
	ui->alert_timer = lv_timer_create(alert_timer_cb, UI_ALERT_AUTO_MS, ui);
	lv_timer_set_repeat_count(ui->alert_timer, 1);
}

void ui_camera_set_ops(ui_camera_t *ui, ui_capture_fn capture,
		ui_record_fn record, void *user)
{
	if (!ui)
		return;
	ui->on_capture = capture;
	ui->on_record = record;
	ui->ops_user = user;
}

void ui_camera_set_poll_ops(ui_camera_t *ui, ui_capture_poll_fn capture_poll,
		ui_record_poll_fn record_poll)
{
	if (!ui)
		return;
	ui->on_capture_poll = capture_poll;
	ui->on_record_poll = record_poll;
}

void ui_camera_set_rec_frames_cb(ui_camera_t *ui, ui_rec_frames_fn fn)
{
	if (!ui)
		return;
	ui->on_rec_frames = fn;
}

void ui_camera_set_rec_abort_cb(ui_camera_t *ui, ui_rec_abort_fn fn)
{
	if (!ui)
		return;
	ui->on_rec_abort = fn;
}

void ui_camera_set_thumb_cb(ui_camera_t *ui, ui_thumb_fn fn)
{
	if (!ui)
		return;
	ui->on_thumb = fn;
}

void ui_camera_set_save_dir_cb(ui_camera_t *ui, ui_save_dir_fn fn)
{
	if (!ui)
		return;
	ui->on_save_dir = fn;
	/* Re-poll now that the real pipeline path is wired. */
	refresh_film_badge(ui);
}

void ui_camera_set_video_info(ui_camera_t *ui, int width, int height, int fps)
{
	if (!ui)
		return;
	ui->video_w = width;
	ui->video_h = height;
	ui->video_fps = fps > 0 ? fps : 30;
	refresh_caption_idle(ui);
	refresh_film_badge(ui);
}

static void flash_anim_cb(void *obj, int32_t v)
{
	lv_obj_set_style_bg_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void flash_anim_ready(lv_anim_t *a)
{
	lv_obj_t *obj = a->var;

	lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
	lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
}

static void shutter_flash(ui_camera_t *ui)
{
	lv_anim_t a;

	if (!ui || !ui->flash_overlay)
		return;
	/*
	 * Default OFF: animating a fullscreen white overlay through LVGL’s
	 * partial draw_buf (80 lines) paints as horizontal white stripes over
	 * the live video plane. Thumb + shot count already give capture feedback.
	 * Opt back in: UI_SHUTTER_FLASH=1
	 */
	if (!env_on("UI_SHUTTER_FLASH", 0))
		return;

	lv_obj_clear_flag(ui->flash_overlay, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_foreground(ui->flash_overlay);
	lv_obj_set_style_bg_opa(ui->flash_overlay, LV_OPA_80, 0);

	lv_anim_init(&a);
	lv_anim_set_var(&a, ui->flash_overlay);
	lv_anim_set_values(&a, LV_OPA_80, LV_OPA_TRANSP);
	lv_anim_set_time(&a, 280);
	lv_anim_set_exec_cb(&a, flash_anim_cb);
	lv_anim_set_ready_cb(&a, flash_anim_ready);
	lv_anim_start(&a);
}

static void thumb_hide_cb(lv_timer_t *t)
{
	ui_camera_t *ui = lv_timer_get_user_data(t);

	if (ui && ui->thumb_card)
		lv_obj_add_flag(ui->thumb_card, LV_OBJ_FLAG_HIDDEN);
	if (ui && ui->thumb_timer) {
		lv_timer_del(ui->thumb_timer);
		ui->thumb_timer = NULL;
	}
}

static void thumb_pos_y_cb(void *obj, int32_t v)
{
	lv_obj_set_y((lv_obj_t *)obj, (lv_coord_t)v);
}

static void thumb_pos_x_cb(void *obj, int32_t v)
{
	lv_obj_set_x((lv_obj_t *)obj, (lv_coord_t)v);
}

static void thumb_eject_mid_ready(lv_anim_t *a)
{
	ui_camera_t *ui = lv_anim_get_user_data(a);
	lv_anim_t ax;
	lv_anim_t ay;
	lv_anim_t az;
	lv_coord_t scr_w;
	lv_coord_t scr_h;
	int status_h;
	int caption_h;
	int end_x;
	int end_y;
	int card_w = UI_THUMB_CARD_W;
	int card_h = UI_THUMB_CARD_H;
	int edge_l;

	if (!ui || !ui->thumb_card)
		return;

	scr_w = lv_disp_get_hor_res(NULL);
	scr_h = lv_disp_get_ver_res(NULL);
	status_h = env_int("UI_STATUS_BAR_H", UI_STATUS_BAR_H_DEFAULT);
	caption_h = env_int("UI_CAPTION_H", UI_CAPTION_H_DEFAULT);
	if (status_h < caption_h)
		status_h = caption_h;
	edge_l = env_int("UI_FRAME_L", UI_FRAME_L_DEFAULT);

	/* Park at user top-right ≈ FB bottom-left when BAR_ROT180. */
	if (bar_rot180_on()) {
		end_x = edge_l + 12;
		end_y = (int)scr_h - status_h - card_h - 12;
	} else {
		end_x = (int)scr_w - card_w - 24;
		end_y = status_h + 24;
	}

	lv_anim_init(&ax);
	lv_anim_set_var(&ax, ui->thumb_card);
	lv_anim_set_values(&ax, lv_obj_get_x(ui->thumb_card), end_x);
	lv_anim_set_time(&ax, 160);
	lv_anim_set_exec_cb(&ax, thumb_pos_x_cb);
	lv_anim_set_path_cb(&ax, lv_anim_path_ease_in);
	lv_anim_start(&ax);

	lv_anim_init(&ay);
	lv_anim_set_var(&ay, ui->thumb_card);
	lv_anim_set_values(&ay, lv_obj_get_y(ui->thumb_card), end_y);
	lv_anim_set_time(&ay, 160);
	lv_anim_set_exec_cb(&ay, thumb_pos_y_cb);
	lv_anim_set_path_cb(&ay, lv_anim_path_ease_in);
	lv_anim_start(&ay);

	lv_anim_init(&az);
	lv_anim_set_var(&az, ui->thumb_card);
	lv_anim_set_values(&az, 256, 115);
	lv_anim_set_time(&az, 160);
	lv_anim_set_exec_cb(&az, anim_zoom_cb);
	lv_anim_start(&az);

	if (ui->thumb_timer)
		lv_timer_del(ui->thumb_timer);
	ui->thumb_timer = lv_timer_create(thumb_hide_cb, 1200, ui);
	lv_timer_set_repeat_count(ui->thumb_timer, 1);
}

static void show_capture_thumb(ui_camera_t *ui)
{
	int x, y;
	int seq = 0;
	lv_anim_t a;
	lv_coord_t scr_w;
	int margin;
	int ctrl_h;
	int edge_t;
	int start_x;
	int start_y;
	int mid_y;
	int tw = CAM_THUMB_W;
	int th = CAM_THUMB_H;
	int card_w;
	int card_h;

	if (!ui || !ui->thumb_canvas || !ui->on_thumb)
		return;
	if (ui->on_thumb(ui->ops_user, ui->thumb_buf, &tw, &th, &seq) != 0)
		return;
	if (tw <= 0 || th <= 0 ||
			(size_t)tw * (size_t)th > (size_t)CAM_THUMB_W * CAM_THUMB_H)
		return;

	card_w = tw + 24;
	card_h = th + 48;

	for (y = 0; y < th; y++) {
		for (x = 0; x < tw; x++) {
			uint16_t p = ui->thumb_buf[y * tw + x];
			uint8_t r = (uint8_t)((p >> 11) << 3);
			uint8_t g = (uint8_t)(((p >> 5) & 0x3F) << 2);
			uint8_t b = (uint8_t)((p & 0x1F) << 3);

			ui->thumb_cbuf[y * tw + x] =
				(lv_color32_t){ .blue = b, .green = g, .red = r,
					.alpha = 0xFF };
		}
	}
	lv_canvas_set_buffer(ui->thumb_canvas, ui->thumb_cbuf, tw, th,
		LV_COLOR_FORMAT_ARGB8888);
	printf("[ui] eject thumb %dx%d seq=%d\n", tw, th, seq);
	fflush(stdout);

	scr_w = lv_disp_get_hor_res(NULL);
	margin = env_int("UI_CTRL_MARGIN", UI_CTRL_MARGIN_DEFAULT);
	ctrl_h = env_int("UI_CTRL_BAR_H", UI_CTRL_BAR_H_DEFAULT);
	edge_t = env_int("UI_OVERSCAN_Y", UI_OVERSCAN_Y_DEFAULT) +
		env_int("UI_FRAME_W", UI_FRAME_W_DEFAULT);
	if (!env_on("UI_POLAROID", 1))
		edge_t = 0;

	/* Eject from shutter (FB-top ctrl center when BAR_ROT180). */
	start_x = ((int)scr_w - card_w) / 2;
	start_y = margin + edge_t + (ctrl_h - card_h) / 2;
	if (start_y < 0)
		start_y = 0;
	mid_y = start_y + 280;

	lv_obj_set_size(ui->thumb_card, card_w, card_h);
	lv_obj_set_style_transform_zoom(ui->thumb_card, 90, 0);
	lv_obj_set_style_transform_pivot_x(ui->thumb_card, card_w / 2, 0);
	lv_obj_set_style_transform_pivot_y(ui->thumb_card, card_h / 2, 0);
	lv_obj_set_pos(ui->thumb_card, start_x, start_y);
	lv_obj_clear_flag(ui->thumb_card, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_foreground(ui->thumb_card);

	lv_anim_delete(ui->thumb_card, NULL);

	lv_anim_init(&a);
	lv_anim_set_var(&a, ui->thumb_card);
	lv_anim_set_values(&a, start_y, mid_y);
	lv_anim_set_time(&a, 200);
	lv_anim_set_exec_cb(&a, thumb_pos_y_cb);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_ready_cb(&a, thumb_eject_mid_ready);
	lv_anim_set_user_data(&a, ui);
	lv_anim_start(&a);

	lv_anim_init(&a);
	lv_anim_set_var(&a, ui->thumb_card);
	lv_anim_set_values(&a, 90, 256);
	lv_anim_set_time(&a, 200);
	lv_anim_set_exec_cb(&a, anim_zoom_cb);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_start(&a);
}

static void anim_zoom_cb(void *obj, int32_t v)
{
	lv_obj_set_style_transform_zoom((lv_obj_t *)obj, (lv_coord_t)v, 0);
}

static void anim_opa_cb(void *obj, int32_t v)
{
	lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void count_anim_ready(lv_anim_t *a)
{
	lv_obj_t *obj = (lv_obj_t *)a->var;

	lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
	lv_obj_set_style_opa(obj, LV_OPA_COVER, 0);
	lv_obj_set_style_transform_zoom(obj, 256, 0);
	maybe_rerotate(obj);
}

static void play_count_anim(ui_camera_t *ui)
{
	lv_anim_t a;

	if (!ui || !ui->count_label)
		return;
	lv_label_set_text_fmt(ui->count_label, "%d", ui->capture_count);
	/* Digit width changes (1 → 10) — refresh 180° pivot before zoom. */
	maybe_rerotate(ui->count_label);
	lv_obj_clear_flag(ui->count_label, LV_OBJ_FLAG_HIDDEN);
	lv_obj_set_style_opa(ui->count_label, LV_OPA_COVER, 0);
	lv_obj_set_style_transform_zoom(ui->count_label, 256, 0);

	lv_anim_init(&a);
	lv_anim_set_var(&a, ui->count_label);
	lv_anim_set_values(&a, 180, 320);
	lv_anim_set_time(&a, 350);
	lv_anim_set_exec_cb(&a, anim_zoom_cb);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_start(&a);

	lv_anim_init(&a);
	lv_anim_set_var(&a, ui->count_label);
	lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
	lv_anim_set_time(&a, 700);
	lv_anim_set_delay(&a, 300);
	lv_anim_set_exec_cb(&a, anim_opa_cb);
	lv_anim_set_ready_cb(&a, count_anim_ready);
	lv_anim_start(&a);
}

/*
 * Badge display:
 *  - precise < 10k: show exact count; refresh on every change (each shot).
 *  - precise >= 10k: sticky "Nk"; refresh only when the thousand-bucket drops.
 * Never raise the sticky Nk from JPEG lock / free_mb noise (was 47k→49k).
 * Uptick only when free_mb grew a lot (delete) — see estimate_film_remaining.
 */
static int g_film_precise = -1;
static int g_film_shown = -1; /* last painted value used for formatting */
static long g_film_free_mb_anchor = -1;

static int film_bucket_value(int precise)
{
	if (precise < 0)
		return -1;
	if (precise >= 100000)
		return 100000;
	if (precise >= 10000)
		return (precise / 1000) * 1000;
	return precise;
}

static void paint_film_badge(ui_camera_t *ui, int shown)
{
	if (shown < 0)
		lv_label_set_text(ui->film_badge, "--");
	else if (shown >= 100000)
		lv_label_set_text(ui->film_badge, "99k+");
	else if (shown >= 10000)
		lv_label_set_text_fmt(ui->film_badge, "%dk", shown / 1000);
	else
		lv_label_set_text_fmt(ui->film_badge, "%d", shown);
	maybe_rerotate(ui->film_badge);
}

static void update_film_badge(ui_camera_t *ui, int precise)
{
	int bucket;
	int need_paint = 0;

	if (!ui || !ui->film_badge)
		return;

	g_film_precise = precise;
	ui->film_remaining = precise;

	if (precise < 0) {
		if (g_film_shown != -1) {
			g_film_shown = -1;
			paint_film_badge(ui, -1);
		}
	} else {
		bucket = film_bucket_value(precise);
		if (g_film_shown < 0) {
			need_paint = 1;
		} else if (precise < 10000) {
			/* Exact mode — follow ones place (capture debit). */
			need_paint = (bucket != g_film_shown);
		} else if (bucket < g_film_shown) {
			/* Sticky Nk — only when thousand bucket drops. */
			need_paint = 1;
		}
		/* Never raise sticky display here. */
		if (need_paint) {
			g_film_shown = bucket;
			paint_film_badge(ui, g_film_shown);
		}
	}

	if (ui->capture_btn) {
		if (precise == 0 || g_film_shown == 0) {
			lv_obj_add_state(ui->capture_btn, LV_STATE_DISABLED);
			lv_obj_set_style_opa(ui->capture_btn, LV_OPA_40, 0);
		} else {
			lv_obj_clear_state(ui->capture_btn, LV_STATE_DISABLED);
			lv_obj_set_style_opa(ui->capture_btn, LV_OPA_COVER, 0);
		}
	}
}

/* After a successful still: count down ones-place immediately. */
static void film_debit_one_shot(ui_camera_t *ui)
{
	int p = g_film_precise;

	if (p < 0)
		return;
	if (p > 0)
		p--;
	update_film_badge(ui, p);
}

/*
 * Per-shot budget:
 *  1) CAM_FILM_KB / CAM_FILM_MB if forced
 *  2) locked size after first capture
 *  3) else ~274KB @1920x1080 (board JPEG q=85 typical)
 */
static uint64_t g_measured_shot_bytes;
static int g_per_kb_locked; /* 0 = use formula / fresh measure */
static int g_formula_kb;    /* last resolution-based estimate */

static void film_note_shot_path(const char *path)
{
	struct stat st;
	uint64_t bytes;
	int kb;

	if (!path || !path[0])
		return;
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
		return;
	bytes = (uint64_t)st.st_size;
	if (bytes < 8 * 1024 || bytes > 32ull * 1024 * 1024)
		return;
	if (g_measured_shot_bytes == 0)
		g_measured_shot_bytes = bytes;
	else
		g_measured_shot_bytes =
			(g_measured_shot_bytes * 3 + bytes) / 4;
	kb = (int)((g_measured_shot_bytes + 1023ull) / 1024ull);
	/*
	 * Lock once, never smaller than the startup formula. A smaller JPEG
	 * would raise remaining (47k→49k); keep the conservative (larger) KB.
	 */
	if (g_per_kb_locked <= 0) {
		g_per_kb_locked = kb;
		if (g_formula_kb > g_per_kb_locked)
			g_per_kb_locked = g_formula_kb;
	}
	printf("[ui] film shot size=%llu KB (ema=%llu KB, locked=%d KB)\n",
		(unsigned long long)(bytes / 1024ull),
		(unsigned long long)(g_measured_shot_bytes / 1024ull),
		g_per_kb_locked);
	fflush(stdout);
}

static int film_kb_per_shot(const ui_camera_t *ui)
{
	const char *forced = getenv("CAM_FILM_KB");
	int legacy_mb;
	uint64_t bytes;
	int w;
	int h;
	int kb;

	if (forced && forced[0]) {
		kb = atoi(forced);
		if (kb < 32)
			kb = 32;
		if (kb > 16 * 1024)
			kb = 16 * 1024;
		return kb;
	}
	legacy_mb = env_int("CAM_FILM_MB", 0);
	if (legacy_mb > 0)
		return legacy_mb * 1024;

	if (g_per_kb_locked > 0)
		return g_per_kb_locked;

	if (g_measured_shot_bytes > 0) {
		bytes = g_measured_shot_bytes;
	} else {
		w = (ui && ui->video_w > 0) ? ui->video_w : 1920;
		h = (ui && ui->video_h > 0) ? ui->video_h : 1080;
		/* ~274KB at 1920x1080 — matches recent board JPEGs. */
		bytes = ((uint64_t)w * (uint64_t)h * 274ull * 1024ull) /
			2073600ull;
	}
	if (bytes < 64ull * 1024)
		bytes = 64ull * 1024;
	kb = (int)((bytes + 1023ull) / 1024ull);
	g_formula_kb = kb;
	return kb;
}

static int estimate_film_remaining(ui_camera_t *ui, const sys_status_t *st)
{
	int per_kb;
	/* 0 = no clamp. Old default 9999 looked like a fake hard limit. */
	int maxn = env_int("CAM_FILM_MAX", 0);
	int n;
	int min_mb;
	int allow_up = 0;
	static int logged;
	const char *how;

	if (!st)
		return -1;
	if (st->free_mb < 0)
		return -1;
	min_mb = env_int("CAM_STORAGE_MIN_MB", 64);
	if (st->free_mb < min_mb)
		return 0;
	per_kb = film_kb_per_shot(ui);
	if (per_kb < 1)
		per_kb = 256;
	/* free_mb only — do not add fields to sys_status_t (ABI fragile). */
	n = (int)(((unsigned long long)st->free_mb * 1024ull) /
		(unsigned long long)per_kb);
	if (n < 0)
		n = 0;
	/* Sanity ceiling only for overflow / absurd values. */
	if (n > 999999)
		n = 999999;
	if (maxn > 0 && n > maxn)
		n = maxn;

	/*
	 * Anchor free_mb. Only treat a big free-space gain as real (delete);
	 * JPEG lock / ±1MB noise must not raise the counter.
	 */
	if (g_film_free_mb_anchor < 0) {
		g_film_free_mb_anchor = st->free_mb;
	} else if (st->free_mb >= g_film_free_mb_anchor + 200) {
		allow_up = 1;
		g_film_free_mb_anchor = st->free_mb;
	} else if (st->free_mb < g_film_free_mb_anchor) {
		g_film_free_mb_anchor = st->free_mb;
	}

	if (g_film_precise >= 0) {
		if (n < g_film_precise)
			; /* follow disk / debit down */
		else if (allow_up)
			; /* free_mb really grew — accept higher n */
		else
			n = g_film_precise; /* clamp: never raise from noise */
	}

	/* If free space grew a lot, allow the sticky badge to retarget once. */
	if (allow_up && g_film_shown >= 0 && n >= g_film_shown + 1000)
		g_film_shown = -1;

	if (!logged) {
		logged = 1;
		if (getenv("CAM_FILM_KB") && getenv("CAM_FILM_KB")[0])
			how = "forced";
		else if (g_measured_shot_bytes > 0)
			how = "last JPEG";
		else
			how = "resolution";
		printf("[ui] film remaining=%d (free=%ldMB / %dKB per shot, %s"
			" %dx%d)\n",
			n, st->free_mb, per_kb, how,
			ui && ui->video_w > 0 ? ui->video_w : 1920,
			ui && ui->video_h > 0 ? ui->video_h : 1080);
		fflush(stdout);
	}
	return n;
}

static void refresh_film_badge(ui_camera_t *ui)
{
	sys_status_t st;
	const char *dir = NULL;

	if (!ui)
		return;
	if (ui->on_save_dir)
		dir = ui->on_save_dir(ui->ops_user);
	if (!dir || !dir[0])
		dir = ui->gallery.save_dir;
	sys_status_poll(&st, dir);
	update_film_badge(ui, estimate_film_remaining(ui, &st));
}

static void sys_timer_cb(lv_timer_t *t)
{
	ui_camera_t *ui = lv_timer_get_user_data(t);
	sys_status_t st;
	const char *dir = NULL;
	char disk[40];
	char line[96];

	if (!ui || !ui->sys_label)
		return;
	if (ui->on_save_dir)
		dir = ui->on_save_dir(ui->ops_user);
	if (!dir || !dir[0])
		dir = ui->gallery.save_dir;

	sys_status_poll(&st, dir);

	{
		int min_mb = env_int("CAM_STORAGE_MIN_MB", 64);
		int full = (st.free_mb >= 0 && st.free_mb < min_mb);

		if (full)
			snprintf(disk, sizeof(disk), "FULL");
		else if (st.free_mb >= 1024)
			snprintf(disk, sizeof(disk), "%.1fGB", st.free_mb / 1024.0);
		else if (st.free_mb >= 0)
			snprintf(disk, sizeof(disk), "%ldMB", st.free_mb);
		else
			snprintf(disk, sizeof(disk), "--");

		/*
		 * C: battery + space. Skip battery glyphs when unknown — Montserrat
		 * symbols often render as "---" on this build and look broken.
		 */
		if (st.battery_pct >= 0) {
			snprintf(line, sizeof(line), "%s%d%%  %s",
				st.charging ? "+" : "", st.battery_pct, disk);
		} else {
			snprintf(line, sizeof(line), "%s", disk);
		}
		lv_label_set_text(ui->sys_label, line);
		lv_obj_set_style_text_font(ui->sys_label, &lv_font_montserrat_14, 0);
		maybe_rerotate(ui->sys_label);

		if (full)
			lv_obj_set_style_text_color(ui->sys_label,
				lv_color_hex(UI_REC_RED), 0);
		else
			lv_obj_set_style_text_color(ui->sys_label,
				lv_color_hex(0x555555), 0);
	}

	update_film_badge(ui, estimate_film_remaining(ui, &st));
}

static void ensure_shutter_widgets(ui_camera_t *ui)
{
	lv_coord_t edge_l = 0;

	if (!ui || ui->flash_overlay)
		return;

	ui->flash_overlay = lv_obj_create(lv_scr_act());
	lv_obj_remove_style_all(ui->flash_overlay);
	lv_obj_set_size(ui->flash_overlay, lv_pct(100), lv_pct(100));
	lv_obj_set_style_bg_color(ui->flash_overlay, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(ui->flash_overlay, LV_OPA_TRANSP, 0);
	lv_obj_clear_flag(ui->flash_overlay, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_flag(ui->flash_overlay, LV_OBJ_FLAG_HIDDEN);

	ui->thumb_card = lv_obj_create(lv_scr_act());
	lv_obj_set_size(ui->thumb_card, UI_THUMB_CARD_W, UI_THUMB_CARD_H);
	lv_obj_set_style_bg_color(ui->thumb_card, lv_color_hex(UI_PAPER_COLOR),
		0);
	lv_obj_set_style_bg_opa(ui->thumb_card, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(ui->thumb_card, 4, 0);
	lv_obj_set_style_border_width(ui->thumb_card, 0, 0);
	lv_obj_set_style_pad_top(ui->thumb_card, 12, 0);
	lv_obj_set_style_pad_hor(ui->thumb_card, 12, 0);
	lv_obj_set_style_pad_bottom(ui->thumb_card, 36, 0);
	/*
	 * Thumb shows camera pixels (same orientation as video plane) — do not
	 * rotate the canvas. Initial park; eject anim repositions.
	 */
	if (bar_rot180_on()) {
		int margin = env_int("UI_CTRL_MARGIN", UI_CTRL_MARGIN_DEFAULT);
		int ctrl_h = env_int("UI_CTRL_BAR_H", UI_CTRL_BAR_H_DEFAULT);
		int edge_t = env_int("UI_OVERSCAN_Y", UI_OVERSCAN_Y_DEFAULT) +
			env_int("UI_FRAME_W", UI_FRAME_W_DEFAULT);

		if (!env_on("UI_POLAROID", 1))
			edge_t = 0;
		edge_l = (lv_coord_t)env_int("UI_FRAME_L", UI_FRAME_L_DEFAULT);
		lv_obj_set_pos(ui->thumb_card, edge_l + 12,
			margin + edge_t + ctrl_h + 8);
	} else {
		lv_obj_align(ui->thumb_card, LV_ALIGN_BOTTOM_RIGHT, -24, -160);
	}
	lv_obj_add_flag(ui->thumb_card, LV_OBJ_FLAG_HIDDEN);
	lv_obj_clear_flag(ui->thumb_card, LV_OBJ_FLAG_SCROLLABLE);

	ui->thumb_canvas = lv_canvas_create(ui->thumb_card);
	lv_obj_align(ui->thumb_canvas, LV_ALIGN_TOP_MID, 0, 0);

	ui->count_label = lv_label_create(lv_scr_act());
	lv_label_set_text(ui->count_label, "0");
	lv_obj_set_style_text_color(ui->count_label, lv_color_white(), 0);
	lv_obj_set_style_text_font(ui->count_label, &lv_font_montserrat_28, 0);
	lv_obj_set_style_text_opa(ui->count_label, LV_OPA_COVER, 0);
	lv_obj_align(ui->count_label, LV_ALIGN_CENTER, 0, 0);
	lv_obj_add_flag(ui->count_label, LV_OBJ_FLAG_HIDDEN);
	lv_obj_clear_flag(ui->count_label, LV_OBJ_FLAG_CLICKABLE);
	if (bar_rot180_on())
		rotate_widget_180(ui->count_label);
}

static void job_timer_stop(ui_camera_t *ui, lv_timer_t *t)
{
	if (!ui)
		return;
	ui->job_kind = 0;
	if (t)
		lv_timer_set_repeat_count(t, 0);
	ui->job_timer = NULL;
}

static void job_timer_cb(lv_timer_t *t)
{
	ui_camera_t *ui = lv_timer_get_user_data(t);
	char path[256];
	int ret;

	if (!ui)
		return;

	path[0] = '\0';
	if (ui->job_kind == 1) {
		if (!ui->on_capture_poll)
			return;
		ret = ui->on_capture_poll(ui->ops_user, path, sizeof(path));
		if (ret == 1)
			return;
		if (ret == 2) {
			/* Idle — stop without lv_timer_del inside this cb. */
			job_timer_stop(ui, t);
			return;
		}
		/* One result drained. */
		if (ret != 0) {
			ui_camera_toast(ui, "Save failed", 2000);
			printf("[ui] Capture failed\n");
		} else {
			ui->capture_count++;
			printf("[ui] Capture OK: %s (showing thumb)\n",
				path[0] ? path : ui->job_path);
			fflush(stdout);
			film_note_shot_path(path[0] ? path : ui->job_path);
			show_capture_thumb(ui);
			play_count_anim(ui);
			ui_camera_toast(ui, "Saved", 1500);
			film_debit_one_shot(ui);
			refresh_film_badge(ui);
		}
		/* Keep timer; next tick sees idle(2) or another burst result. */
		return;
	}

	if (ui->job_kind == 2) {
		if (!ui->on_record_poll)
			return;
		ret = ui->on_record_poll(ui->ops_user, path, sizeof(path));
		if (ret == 1)
			return;
		job_timer_stop(ui, t);
		if (ret != 0) {
			ui_camera_toast(ui, "Save failed", 2000);
			printf("[ui] REC remux failed\n");
		} else {
			refresh_caption_idle(ui);
			ui_camera_toast(ui, "Saved", 1500);
			printf("[ui] REC OFF: %s\n",
				path[0] ? path : ui->job_path);
			refresh_film_badge(ui);
		}
	}
}

static void job_timer_start(ui_camera_t *ui, int kind)
{
	if (!ui)
		return;
	if (ui->job_timer)
		lv_timer_del(ui->job_timer);
	ui->job_kind = kind;
	ui->job_timer = lv_timer_create(job_timer_cb, 40, ui);
}

/* Run after the input event finishes — avoids LVGL re-entrancy on PRESSED. */
static void capture_do_async(void *user)
{
	ui_camera_t *ui = user;
	char path[256];
	int ret;

	if (!ui)
		return;
	if (ui_gallery_is_visible(&ui->gallery))
		return;
	if (!ui->on_capture) {
		ui_camera_set_status(ui, "Capture not wired");
		return;
	}
	if (ui->job_kind == 2) {
		ui_camera_set_status(ui, "Busy — wait");
		return;
	}
	if (ui->recording) {
		ui_camera_set_status(ui, "Stop REC before capture");
		return;
	}
	if (ui->film_remaining == 0) {
		ui_camera_set_status(ui, "No film");
		return;
	}

	ensure_shutter_widgets(ui);
	path[0] = '\0';
	ret = ui->on_capture(ui->ops_user, path, sizeof(path));
	printf("[ui] capture begin ret=%d path=%s job_kind=%d timer=%p\n",
		ret, path[0] ? path : "(none)", ui->job_kind,
		(void *)ui->job_timer);
	fflush(stdout);

	if (ret == -2) {
		ui_camera_alert(ui, "Storage full", "Cannot capture");
	} else if (ret != 0) {
		ui_camera_set_status(ui, "Burst queue full");
		printf("[ui] Capture queue full\n");
	} else {
		snprintf(ui->job_path, sizeof(ui->job_path), "%s", path);
		shutter_flash(ui);
		ui_camera_toast(ui, "Capturing...", 0);
		if (ui->on_capture_poll) {
			/* Always (re)arm — a stale job_kind==1 with NULL timer
			 * previously skipped start and stills never completed UI. */
			job_timer_start(ui, 1);
		} else {
			ui->capture_count++;
			show_capture_thumb(ui);
			play_count_anim(ui);
			ui_camera_toast(ui, "Saved", 1500);
			printf("[ui] Capture OK: %s\n", path);
		}
	}
}

static void capture_btn_event(lv_event_t *e)
{
	ui_camera_t *ui = lv_event_get_user_data(e);

	if (lv_event_get_code(e) != LV_EVENT_PRESSED || !ui)
		return;
	printf("[ui] capture press\n");
	fflush(stdout);

	if (ui_gallery_is_visible(&ui->gallery))
		return;
	if (ui->job_kind == 2) {
		ui_camera_set_status(ui, "Busy — wait");
		return;
	}
	if (ui->recording) {
		ui_camera_set_status(ui, "Stop REC before capture");
		return;
	}

	if (lv_async_call(capture_do_async, ui) != LV_RESULT_OK) {
		/* Fallback if async queue is full. */
		capture_do_async(ui);
	}
}

static void gallery_btn_event(lv_event_t *e)
{
	ui_camera_t *ui = lv_event_get_user_data(e);

	/*
	 * Flash cream + image glyph briefly, then open. Album covers the bar
	 * immediately otherwise, so pressed styles never readable.
	 */
	if (lv_event_get_code(e) != LV_EVENT_PRESSED || !ui)
		return;
	printf("[ui] album press\n");
	fflush(stdout);

	gallery_btn_set_flash(ui, 1);
	if (ui->gallery_tap_timer) {
		lv_timer_reset(ui->gallery_tap_timer);
		return;
	}
	ui->gallery_tap_timer = lv_timer_create(gallery_tap_timer_cb, 100, ui);
	lv_timer_set_repeat_count(ui->gallery_tap_timer, 1);
}

static void record_do_async(void *user_data)
{
	ui_camera_t *ui = user_data;
	char path[256];
	int start;
	int ret;
	int auto_reason;

	if (!ui)
		return;
	if (ui_gallery_is_visible(&ui->gallery))
		return;
	if (!ui->on_record) {
		ui_camera_set_status(ui, "REC not wired");
		return;
	}
	/* Allow REC while a still is encoding; only remux must block. */
	if (ui->job_kind == 2) {
		ui_camera_set_status(ui, "Busy — wait");
		return;
	}

	auto_reason = ui->rec_auto_stop_reason;
	ui->rec_auto_stop_reason = 0;
	/* Auto-stop from storage/mux: always stop, never start. */
	start = auto_reason > 0 ? 0 : !ui->recording;
	path[0] = '\0';
	ret = ui->on_record(ui->ops_user, start, path, sizeof(path));

	if (ret == -2) {
		ui_camera_alert(ui, "Storage full", "Cannot start REC");
	} else if (ret != 0) {
		ui_camera_set_status(ui,
			start ? "REC start failed" : "REC stop failed");
		printf("[ui] REC %s failed\n", start ? "start" : "stop");
	} else if (start) {
		ui->recording = 1;
		ui_camera_set_recording(ui, 1);
		rec_hud_start(ui);
		if (ui->status_label) {
			lv_label_set_text(ui->status_label, "");
			lv_obj_add_flag(ui->status_label, LV_OBJ_FLAG_HIDDEN);
		}
		printf("[ui] REC ON: %s\n", path);
	} else {
		char elapsed[32];

		ui->recording = 0;
		ui_camera_set_recording(ui, 0);
		rec_hud_stop(ui, elapsed, sizeof(elapsed));
		snprintf(ui->job_elapsed, sizeof(ui->job_elapsed), "%s", elapsed);
		snprintf(ui->job_path, sizeof(ui->job_path), "%s", path);
		if (auto_reason == 1)
			ui_camera_alert(ui, "Storage full", "REC stopped");
		else if (auto_reason == 2)
			ui_camera_alert(ui, "REC stopped", "Save failed");
		else
			ui_camera_toast(ui, "Saving...", 0);
		if (auto_reason)
			printf("[ui] REC auto-stop reason=%d\n", auto_reason);
		if (ui->on_record_poll) {
			job_timer_start(ui, 2);
		} else {
			refresh_caption_idle(ui);
			if (!auto_reason)
				ui_camera_toast(ui, "Saved", 1500);
			printf("[ui] REC OFF: %s\n", path);
			refresh_film_badge(ui);
		}
	}
}

static void record_btn_event(lv_event_t *e)
{
	ui_camera_t *ui = lv_event_get_user_data(e);
	lv_event_code_t code = lv_event_get_code(e);
	static uint32_t last_ms;

	/*
	 * PRESSED is primary; CLICKED is a fallback when transform/hit-test
	 * eats the press on the small rotated REC disc.
	 */
	if (code != LV_EVENT_PRESSED && code != LV_EVENT_CLICKED)
		return;
	if (!ui)
		return;
	/* Debounce dual PRESSED+CLICKED and chatter (~1 event wins). */
	if (lv_tick_elaps(last_ms) < 280)
		return;
	last_ms = lv_tick_get();

	printf("[ui] REC press (code=%d)\n", (int)code);
	fflush(stdout);

	if (lv_async_call(record_do_async, ui) != LV_RESULT_OK)
		record_do_async(ui);
}

void ui_camera_set_recording(ui_camera_t *ui, int on)
{
	lv_obj_t *child;

	if (!ui || !ui->record_btn)
		return;
	child = lv_obj_get_child(ui->record_btn, 0);
	if (on) {
		lv_obj_set_style_bg_color(ui->record_btn,
			lv_color_hex(UI_REC_RED), 0);
		lv_obj_set_style_border_color(ui->record_btn,
			lv_color_hex(UI_REC_RED), 0);
		/* Stop square while recording. */
		if (child) {
			lv_obj_set_size(child, 16, 16);
			lv_obj_set_style_radius(child, 3, 0);
			lv_obj_set_style_bg_color(child, lv_color_white(), 0);
			lv_obj_center(child);
		}
	} else {
		lv_obj_set_style_bg_color(ui->record_btn,
			lv_color_hex(UI_PAPER_COLOR), 0);
		lv_obj_set_style_border_color(ui->record_btn,
			lv_color_hex(UI_INK_COLOR), 0);
		if (child) {
			lv_obj_set_size(child, 10, 10);
			lv_obj_set_style_radius(child, LV_RADIUS_CIRCLE, 0);
			lv_obj_set_style_bg_color(child, lv_color_hex(UI_REC_RED), 0);
			lv_obj_center(child);
		}
	}
}

static void create_buttons(ui_camera_t *ui, lv_obj_t *bar, int vertical,
		int hit_pad, int swap_lr)
{
	lv_obj_t *lbl;
	lv_obj_t *dot;
	/*
	 * C: Album 64 | Shutter Ø88 | Rec Ø48 — shutter dominates.
	 */
	const int gal_d = env_int("UI_ALBUM_BTN", UI_ALBUM_BTN_DEFAULT);
	const int cap_d = env_int("UI_SHUTTER_D", UI_SHUTTER_D_DEFAULT);
	const int rec_d = env_int("UI_REC_BTN", UI_REC_BTN_DEFAULT);
	const int sep_x = env_int("UI_BTN_SEP", UI_BTN_SEP_DEFAULT);
	lv_coord_t gal_x = swap_lr ? sep_x : -sep_x;
	lv_coord_t cap_x = 0;
	lv_coord_t rec_x = swap_lr ? -sep_x : sep_x;

	lv_obj_add_flag(bar, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
	lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);

	ui->gallery_btn = lv_btn_create(bar);
	lv_obj_set_size(ui->gallery_btn, gal_d, gal_d);
	style_touch_btn(ui->gallery_btn, hit_pad);
	lv_obj_set_style_radius(ui->gallery_btn, 12, 0);
	lv_obj_set_style_bg_color(ui->gallery_btn, lv_color_hex(UI_PAPER_COLOR), 0);
	lv_obj_set_style_bg_color(ui->gallery_btn, lv_color_hex(0xE8E0D4),
		LV_STATE_PRESSED);
	lv_obj_set_style_border_width(ui->gallery_btn, 2, 0);
	lv_obj_set_style_border_color(ui->gallery_btn, lv_color_hex(UI_INK_COLOR), 0);
	lv_obj_add_event_cb(ui->gallery_btn, gallery_btn_event,
		LV_EVENT_PRESSED, ui);
	lbl = lv_label_create(ui->gallery_btn);
	lv_label_set_text(lbl, LV_SYMBOL_IMAGE);
	lv_obj_set_style_text_color(lbl, lv_color_hex(UI_INK_COLOR), 0);
	lv_obj_center(lbl);
	lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

	/* Short-rec: hollow ring + red tip (secondary). Larger + hit pad. */
	ui->record_btn = lv_btn_create(bar);
	lv_obj_set_size(ui->record_btn, rec_d, rec_d);
	style_touch_btn(ui->record_btn, hit_pad > 0 ? hit_pad : 20);
	/* Zoom on a small 180°-rotated disc eats hit tests — keep stable. */
	lv_obj_set_style_transform_zoom(ui->record_btn, 256, LV_STATE_PRESSED);
	lv_obj_set_style_radius(ui->record_btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(ui->record_btn, lv_color_hex(UI_PAPER_COLOR), 0);
	lv_obj_set_style_bg_opa(ui->record_btn, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(ui->record_btn, 2, 0);
	lv_obj_set_style_border_color(ui->record_btn, lv_color_hex(UI_INK_COLOR), 0);
	lv_obj_set_style_bg_color(ui->record_btn, lv_color_hex(UI_REC_RED),
		LV_STATE_PRESSED);
	lv_obj_add_event_cb(ui->record_btn, record_btn_event,
		LV_EVENT_PRESSED, ui);
	lv_obj_add_event_cb(ui->record_btn, record_btn_event,
		LV_EVENT_CLICKED, ui);
	dot = lv_obj_create(ui->record_btn);
	lv_obj_remove_style_all(dot);
	lv_obj_set_size(dot, 12, 12);
	lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(dot, lv_color_hex(UI_REC_RED), 0);
	lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(dot, 0, 0);
	lv_obj_center(dot);
	lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(dot, LV_OBJ_FLAG_EVENT_BUBBLE);

	/* Primary shutter: ink disc + ring. */
	ui->capture_btn = lv_btn_create(bar);
	lv_obj_set_size(ui->capture_btn, cap_d, cap_d);
	style_touch_btn(ui->capture_btn, hit_pad);
	lv_obj_set_style_radius(ui->capture_btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(ui->capture_btn, lv_color_hex(UI_INK_COLOR), 0);
	lv_obj_set_style_bg_color(ui->capture_btn, lv_color_hex(0x555555),
		LV_STATE_PRESSED);
	lv_obj_set_style_border_width(ui->capture_btn, 3, 0);
	lv_obj_set_style_border_color(ui->capture_btn, lv_color_hex(UI_INK_COLOR), 0);
	lv_obj_set_style_outline_width(ui->capture_btn, 2, 0);
	lv_obj_set_style_outline_pad(ui->capture_btn, 3, 0);
	lv_obj_set_style_outline_color(ui->capture_btn, lv_color_hex(UI_INK_COLOR), 0);
	lv_obj_set_style_transform_zoom(ui->capture_btn, 230, LV_STATE_PRESSED);
	lv_obj_add_event_cb(ui->capture_btn, capture_btn_event,
		LV_EVENT_PRESSED, ui);

	if (vertical) {
		lv_obj_set_size(ui->record_btn, rec_d, rec_d);
		lv_obj_set_size(ui->gallery_btn, gal_d, gal_d);
		if (swap_lr) {
			lv_obj_align(ui->gallery_btn, LV_ALIGN_TOP_MID, 0, 8);
			lv_obj_align(ui->capture_btn, LV_ALIGN_CENTER, 0, 0);
			lv_obj_align(ui->record_btn, LV_ALIGN_BOTTOM_MID, 0, -8);
		} else {
			lv_obj_align(ui->gallery_btn, LV_ALIGN_BOTTOM_MID, 0, -8);
			lv_obj_align(ui->capture_btn, LV_ALIGN_CENTER, 0, 0);
			lv_obj_align(ui->record_btn, LV_ALIGN_TOP_MID, 0, 8);
		}
	} else {
		lv_obj_align(ui->gallery_btn, LV_ALIGN_CENTER, gal_x, -8);
		lv_obj_align(ui->capture_btn, LV_ALIGN_CENTER, cap_x, -8);
		lv_obj_align(ui->record_btn, LV_ALIGN_CENTER, rec_x, -8);
	}

	if (bar_rot180_on()) {
		rotate_widget_180(ui->gallery_btn);
		rotate_widget_180(ui->capture_btn);
		rotate_widget_180(ui->record_btn);
	}
}

void ui_camera_create(ui_camera_t *ui, const char *sensor_name)
{
	lv_obj_t *scr = lv_scr_act();
	lv_obj_t *ctrl_bar;
	ui_pos_t ctrl = ui_ctrl_pos();
	ui_pos_t status = ui_status_pos(ctrl);
	int margin = env_int("UI_CTRL_MARGIN", UI_CTRL_MARGIN_DEFAULT);
	int ctrl_h = env_int("UI_CTRL_BAR_H", UI_CTRL_BAR_H_DEFAULT);
	int status_h = env_int("UI_STATUS_BAR_H", UI_STATUS_BAR_H_DEFAULT);
	int side_w = env_int("UI_SIDE_BAR_W", UI_SIDE_BAR_W_DEFAULT);
	int hit_pad = env_int("UI_BTN_HIT_PAD", UI_BTN_HIT_PAD_DEFAULT);
	int swap_lr = env_on("UI_SWAP_BUTTONS", bar_rot180_on() ? 1 : 0);
	int polaroid = env_on("UI_POLAROID", 1);
	int frame_w = env_int("UI_FRAME_W", UI_FRAME_W_DEFAULT);
	int frame_l = env_int("UI_FRAME_L", UI_FRAME_L_DEFAULT);
	int frame_r = env_int("UI_FRAME_R", UI_FRAME_R_DEFAULT);
	/* Prefer L/R; fall back to UI_OVERSCAN_X if set (legacy). */
	int overscan_x = env_int("UI_OVERSCAN_X", -1);
	int overscan_l = env_int("UI_OVERSCAN_L",
		overscan_x >= 0 ? overscan_x : UI_OVERSCAN_L_DEFAULT);
	int overscan_r = env_int("UI_OVERSCAN_R",
		overscan_x >= 0 ? overscan_x : UI_OVERSCAN_R_DEFAULT);
	int overscan_y = env_int("UI_OVERSCAN_Y", UI_OVERSCAN_Y_DEFAULT);
	int edge_l;
	int edge_r;
	int edge_t;
	int caption_h = env_int("UI_CAPTION_H", UI_CAPTION_H_DEFAULT);
	int vertical = (ctrl == UI_POS_LEFT || ctrl == UI_POS_RIGHT);
	lv_coord_t scr_w;
	lv_coord_t scr_h;

	g_ui = ui;
	memset(ui, 0, sizeof(*ui));
	if (sensor_name)
		snprintf(ui->sensor_name, sizeof(ui->sensor_name), "%s",
			sensor_name);

	/*
	 * Default theme pads the active screen; any transparent pad strip
	 * shows the camera plane underneath — looks like a misaligned frame.
	 */
	lv_obj_set_style_bg_opa(scr, LV_OPA_TRANSP, 0);
	lv_obj_set_style_pad_all(scr, 0, 0);
	lv_obj_set_style_border_width(scr, 0, 0);
	lv_obj_set_style_outline_width(scr, 0, 0);
	lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
	{
		lv_obj_t *bottom = lv_layer_bottom();

		if (bottom)
			lv_obj_set_style_bg_opa(bottom, LV_OPA_TRANSP, 0);
	}

	scr_w = lv_display_get_horizontal_resolution(NULL);
	scr_h = lv_display_get_vertical_resolution(NULL);
	if (scr_w <= 0)
		scr_w = lv_obj_get_width(scr);
	if (scr_h <= 0)
		scr_h = lv_obj_get_height(scr);

	if (status_h < caption_h)
		status_h = caption_h;

	/* Visible cream = UI_FRAME only (equal L/R), same as album JPEG / video.
	 * Do not fold UI_OVERSCAN into cream — that made the right matte ~3× thick.
	 * Gallery insets still use overscan+frame so Del/Close stay on-panel.
	 */
	edge_l = polaroid ? frame_l : 0;
	edge_r = polaroid ? frame_r : 0;
	edge_t = polaroid ? (overscan_y + frame_w) : 0;
	if (edge_l < 0)
		edge_l = 0;
	if (edge_r < 0)
		edge_r = 0;
	if (edge_t < 0)
		edge_t = 0;

	if (polaroid) {
		int edge_b = status_h; /* FB bottom = user top cream when ROT180 */

		ui->film_matte = NULL;
		lvgl_display_set_polaroid_frame(edge_l, edge_r, edge_t, edge_b,
			UI_PAPER_COLOR);
		printf("[ui] film edges %dx%d cream L/R/T/B=%d/%d/%d/%d "
			"(frame=%d/%d/%d; gallery inset uses os+frame)\n",
			(int)scr_w, (int)scr_h, edge_l, edge_r, edge_t, edge_b,
			frame_l, frame_r, frame_w);
	} else {
		lvgl_display_set_polaroid_frame(0, 0, 0, 0, 0);
	}

	ctrl_bar = lv_obj_create(scr);
	if (vertical) {
		lv_obj_set_size(ctrl_bar, side_w, 200);
		if (ctrl == UI_POS_LEFT)
			lv_obj_align(ctrl_bar, LV_ALIGN_LEFT_MID,
				margin + edge_l, 0);
		else
			lv_obj_align(ctrl_bar, LV_ALIGN_RIGHT_MID,
				-(margin + edge_r), 0);
		style_bar(ctrl_bar, 16);
	} else {
		/*
		 * Pin ctrl to cream L/R (equal UI_FRAME) so the bar fills the
		 * paper aperture edge-to-edge.
		 */
		if (polaroid) {
			int ctrl_y;

			lv_obj_set_size(ctrl_bar, scr_w - edge_l - edge_r, ctrl_h);
			ctrl_y = ui_ctrl_bar_y(ctrl, status, scr_h, margin, edge_t,
				ctrl_h, status_h);
			lv_obj_set_pos(ctrl_bar, edge_l, ctrl_y);
		} else {
			int ctrl_y;

			lv_obj_set_size(ctrl_bar, scr_w, ctrl_h);
			ctrl_y = ui_ctrl_bar_y(ctrl, status, scr_h, margin, 0,
				ctrl_h, status_h);
			lv_obj_set_pos(ctrl_bar, 0, ctrl_y);
		}
		style_bar(ctrl_bar, 0);
		/* Cream plate already set in style_bar — do not wash to translucent. */
	}
	create_buttons(ui, ctrl_bar, vertical, hit_pad, swap_lr);

	/* Status line inside bottom cream (under shutter row). */
	ui->sys_label = lv_label_create(ctrl_bar);
	lv_label_set_text(ui->sys_label, "");
	lv_obj_set_style_text_color(ui->sys_label, lv_color_hex(0x555555), 0);
	lv_obj_set_style_text_font(ui->sys_label, &lv_font_montserrat_14, 0);
	lv_obj_set_style_text_opa(ui->sys_label, LV_OPA_70, 0);
	lv_label_set_long_mode(ui->sys_label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(ui->sys_label, lv_pct(80));
	lv_obj_align(ui->sys_label, LV_ALIGN_BOTTOM_MID, 0, -4);
	if (bar_rot180_on())
		rotate_widget_180(ui->sys_label);

	ui->rec_hud = NULL;
	ui->rec_timer = NULL;

	/* User-top cream (FB bottom when BAR_ROT180) — thin plate, no tech meta. */
	ui->caption_bar = lv_obj_create(scr);
	lv_obj_remove_style_all(ui->caption_bar);
	lv_obj_set_size(ui->caption_bar, scr_w, status_h);
	if (status == UI_POS_TOP)
		lv_obj_set_pos(ui->caption_bar, 0, 0);
	else
		lv_obj_set_pos(ui->caption_bar, 0, scr_h - status_h);
	lv_obj_set_style_bg_color(ui->caption_bar, lv_color_hex(UI_PAPER_COLOR),
		0);
	lv_obj_set_style_bg_opa(ui->caption_bar, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(ui->caption_bar, 0, 0);
	lv_obj_set_style_radius(ui->caption_bar, 0, 0);
	lv_obj_set_style_pad_left(ui->caption_bar, 12 + edge_l, 0);
	lv_obj_set_style_pad_right(ui->caption_bar, 12 + edge_r, 0);
	lv_obj_set_style_pad_ver(ui->caption_bar, 4, 0);
	lv_obj_clear_flag(ui->caption_bar, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_clear_flag(ui->caption_bar, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_flag(ui->caption_bar, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

	/* Preview HUD toast (dark chip) — not a child of cream (FB paint covers it). */
	ui->status_label = lv_label_create(scr);
	lv_label_set_text(ui->status_label, "");
	lv_obj_set_style_bg_color(ui->status_label, lv_color_hex(0x1A1A1A), 0);
	lv_obj_set_style_bg_opa(ui->status_label, LV_OPA_70, 0);
	lv_obj_set_style_text_color(ui->status_label, lv_color_white(), 0);
	lv_obj_set_style_text_font(ui->status_label, &lv_font_montserrat_14, 0);
	lv_obj_set_style_text_opa(ui->status_label, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_hor(ui->status_label, 14, 0);
	lv_obj_set_style_pad_ver(ui->status_label, 6, 0);
	lv_obj_set_style_radius(ui->status_label, 14, 0);
	lv_label_set_long_mode(ui->status_label, LV_LABEL_LONG_CLIP);
	lv_obj_clear_flag(ui->status_label, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_flag(ui->status_label, LV_OBJ_FLAG_HIDDEN);
	place_preview_toast(ui);

	ui->meta_label = lv_label_create(ui->caption_bar);
	lv_label_set_text(ui->meta_label, "");
	lv_obj_set_style_text_color(ui->meta_label, lv_color_hex(0x666666), 0);
	lv_obj_set_style_text_font(ui->meta_label, &lv_font_montserrat_14, 0);
	lv_obj_add_flag(ui->meta_label, LV_OBJ_FLAG_HIDDEN);
	lv_obj_align(ui->meta_label, LV_ALIGN_BOTTOM_LEFT, 0, -2);

	if (bar_rot180_on())
		rotate_widget_180(ui->meta_label);

	/* Film remaining chip — user top-left ≈ FB bottom-right when ROT180. */
	ui->film_badge = lv_label_create(scr);
	lv_label_set_text(ui->film_badge, "--");
	lv_obj_set_style_bg_color(ui->film_badge, lv_color_hex(UI_PAPER_COLOR), 0);
	lv_obj_set_style_bg_opa(ui->film_badge, LV_OPA_70, 0);
	lv_obj_set_style_text_color(ui->film_badge, lv_color_hex(UI_INK_COLOR), 0);
	lv_obj_set_style_text_font(ui->film_badge, &lv_font_montserrat_20, 0);
	lv_obj_set_style_pad_hor(ui->film_badge, 10, 0);
	lv_obj_set_style_pad_ver(ui->film_badge, 6, 0);
	lv_obj_set_style_radius(ui->film_badge, 8, 0);
	lv_obj_clear_flag(ui->film_badge, LV_OBJ_FLAG_CLICKABLE);
	if (bar_rot180_on()) {
		lv_obj_align(ui->film_badge, LV_ALIGN_BOTTOM_RIGHT,
			-(12 + edge_r), -(status_h + 12));
		rotate_widget_180(ui->film_badge);
	} else {
		lv_obj_align(ui->film_badge, LV_ALIGN_TOP_LEFT,
			12 + edge_l, status_h + 12);
	}
	ui->film_remaining = -1;

	ensure_shutter_widgets(ui);

	{
		const char *dir = getenv("CAM_SAVE_DIR");
		int inset_l = polaroid ? (overscan_l + frame_l) : 0;
		int inset_r = polaroid ? (overscan_r + frame_r) : 0;

		if (!dir || !dir[0])
			dir = "/userdata/lvgl_camera";
		/* Insets keep Del/Close clear of panel clip; cream stamp is equal. */
		ui_gallery_init(&ui->gallery, dir, inset_l, inset_r, edge_t,
			edge_l, edge_r, edge_t, (uint32_t)UI_PAPER_COLOR);
	}

	ui->sys_timer = lv_timer_create(sys_timer_cb, 2000, ui);
	sys_timer_cb(ui->sys_timer);

	/* Re-assert after theme styles from child create. */
	lv_obj_set_style_bg_opa(scr, LV_OPA_TRANSP, 0);
	{
		lv_obj_t *bottom = lv_layer_bottom();

		if (bottom)
			lv_obj_set_style_bg_opa(bottom, LV_OPA_TRANSP, 0);
	}

	printf("[ui] C-style polaroid=%d ctrl=%s caption=%s margin=%d "
		"hit_pad=%d bar_rot180=%d ctrl_h=%d caption_h=%d ctrl_y=%d\n",
		polaroid,
		ctrl == UI_POS_TOP ? "top" :
		ctrl == UI_POS_BOTTOM ? "bottom" :
		ctrl == UI_POS_LEFT ? "left" : "right",
		status == UI_POS_TOP ? "top" : "bottom",
		margin, hit_pad, bar_rot180_on(), ctrl_h, status_h,
		ui_ctrl_bar_y(ctrl, status, scr_h, margin, edge_t, ctrl_h,
			status_h));
}
