#ifndef UI_GALLERY_H_
#define UI_GALLERY_H_

#include <stdint.h>

#include "lvgl.h"

/* path=JPEG to show; return 0 on success. */
typedef int (*ui_gallery_still_show_fn)(void *user, const char *jpeg_path);
typedef void (*ui_gallery_still_hide_fn)(void *user);
/* path=MP4 to play on video plane; path=NULL/"" stops play and clears still.
 * return 0 on success.
 */
typedef int (*ui_gallery_video_play_fn)(void *user, const char *mp4_path);

typedef struct {
	lv_obj_t *root;
	lv_obj_t *hdr;
	lv_obj_t *list;
	lv_obj_t *title;
	lv_obj_t *close_btn;
	/* Bottom (user) middle strip: double-tap to close (top ~10% often dead). */
	lv_obj_t *exit_zone;
	/* Fullscreen photo (JPG) preview overlay — tap to dismiss. */
	lv_obj_t *preview;
	lv_obj_t *preview_canvas;
	lv_obj_t *preview_hint;
	lv_obj_t *preview_msg;
	/* Packed RGB565 from sidecar / JPEG decode (buffer orientation). */
	uint16_t *preview_rgb;
	/* ARGB8888 canvas pixels; same orientation as capture (no software rot). */
	lv_color32_t *preview_argb;
	int preview_w;
	int preview_h;
	int max_items;
	int visible;
	/* Keep Close/Del inside visible panel (match Polaroid overscan). */
	int inset_l;
	int inset_r;
	int inset_t;
	/* 1: per-widget 180° + L/R pre-mirror for product physical flip (match bars). */
	int bar_rot180;
	uint32_t exit_tap_ms;
	/* Restore Polaroid FB paint when leaving album (prefer display suspend). */
	int frame_l;
	int frame_r;
	int frame_t;
	uint32_t frame_color;
	ui_gallery_still_show_fn on_still_show;
	ui_gallery_still_hide_fn on_still_hide;
	ui_gallery_video_play_fn on_video_play;
	void *still_user;
	/* Ignore / block preview dismiss until open gesture fully ends. */
	int preview_guard_armed;
	uint32_t preview_guard_ms;
	lv_timer_t *preview_guard_timer;
	/* HW album mini-player: bottom chrome + optional top Exit. */
	lv_timer_t *preview_video_timer;
	lv_timer_t *preview_exit_timer; /* auto-hide Exit after idle */
	int preview_video_dur_sec;
	int preview_video_ended;
	int preview_exit_visible;
	/* 1: HW play started; keep album list until first frame (anti-flash). */
	int preview_video_awaiting;
	int preview_video_ui_shown;
	uint32_t preview_video_wait_ms;
	char preview_video_path[160];
	/*
	 * Keep last so a stale .o writing "save_dir" at the pre-exit_zone offset
	 * (right after close_btn) cannot leave "a/lvgl_camera".
	 */
	char save_dir[160];
} ui_gallery_t;

void ui_gallery_init(ui_gallery_t *g, const char *save_dir,
		int inset_l, int inset_r, int inset_t,
		int frame_l, int frame_r, int frame_t, uint32_t frame_color);
void ui_gallery_set_still_ops(ui_gallery_t *g, ui_gallery_still_show_fn show,
		ui_gallery_still_hide_fn hide, void *user);
void ui_gallery_set_video_play(ui_gallery_t *g, ui_gallery_video_play_fn play);
/* Re-copy absolute path before scan/open. Pass pipeline save_dir when known;
 * NULL → CAM_SAVE_DIR, else /userdata/lvgl_camera or /tmp/lvgl_camera (mkdir).
 */
void ui_gallery_set_save_dir(ui_gallery_t *g, const char *dir);
void ui_gallery_show(ui_gallery_t *g);
void ui_gallery_hide(ui_gallery_t *g);
int ui_gallery_is_visible(const ui_gallery_t *g);

#endif
