#ifndef UI_CAMERA_H_
#define UI_CAMERA_H_

#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"
#include "media_still.h"
#include "ui_gallery.h"

typedef int (*ui_capture_fn)(void *user, char *path, size_t path_len);
/* Poll: 1=busy, 0=ok, -1=fail */
typedef int (*ui_capture_poll_fn)(void *user, char *path, size_t path_len);
/* start=1 begin REC; start=0 begin async stop (remux off UI thread) */
typedef int (*ui_record_fn)(void *user, int start, char *path, size_t path_len);
typedef int (*ui_record_poll_fn)(void *user, char *path, size_t path_len);
/* Optional: live encoded frame count while recording (NULL = skip) */
typedef int (*ui_rec_frames_fn)(void *user);
/* Optional: mid-REC abort (0=none, 1=storage, 2=mux); UI auto-stops */
typedef int (*ui_rec_abort_fn)(void *user);
/* Optional: fill RGB565 thumb after capture; return 0 if valid.
 * out_w/out_h receive pixel size (portrait when CAM_VIDEO_ROTATE=90). */
typedef int (*ui_thumb_fn)(void *user, uint16_t *rgb565, int *out_w, int *out_h,
		int *seq);
typedef const char *(*ui_save_dir_fn)(void *user);

typedef struct {
	lv_obj_t *status_label;     /* brief toast on cream (C: no sensor/meta) */
	lv_obj_t *meta_label;       /* debug only: WxH|fps when UI_DEBUG_META=1 */
	lv_obj_t *caption_bar;      /* user-top cream (FB bottom when BAR_ROT180) */
	lv_obj_t *film_matte;       /* unused; edges via DRM FB paint */
	lv_obj_t *film_badge;       /* remaining shots chip */
	lv_obj_t *capture_btn;      /* primary Ø shutter */
	lv_obj_t *record_btn;       /* secondary short-rec */
	lv_obj_t *gallery_btn;
	lv_obj_t *sys_label;        /* battery + free space (in ctrl bar) */
	lv_obj_t *alert_overlay;    /* dim + modal card (storage / REC stop) */
	lv_obj_t *alert_card;
	lv_obj_t *alert_title;
	lv_obj_t *alert_msg;
	lv_obj_t *rec_hud;          /* REC: dot + elapsed only */
	lv_obj_t *rec_dot;
	lv_obj_t *rec_time_label;
	lv_obj_t *rec_meta_label;   /* kept for ABI; hidden in C style */
	lv_obj_t *flash_overlay;
	lv_obj_t *thumb_card;
	lv_obj_t *thumb_canvas;
	lv_obj_t *count_label;
	int film_remaining;        /* last computed remaining shots */
	lv_timer_t *rec_timer;
	lv_timer_t *sys_timer;
	lv_timer_t *thumb_timer;
	lv_timer_t *job_timer;      /* poll async capture / remux */
	lv_timer_t *gallery_tap_timer; /* brief press flash before album open */
	lv_timer_t *status_timer;     /* auto-clear toast */
	lv_timer_t *alert_timer;      /* auto-dismiss modal alert */
	uint32_t rec_start_ms;
	uint32_t rec_last_sec;
	int rec_dot_on;
	int video_w;
	int video_h;
	int video_fps;
	int capture_count;
	int recording;
	int rec_auto_stop_reason; /* 0=none; set by REC timer when pipeline aborts */
	int job_kind;               /* 0 idle, 1 capture, 2 remux */
	char job_path[256];
	char job_elapsed[32];
	char sensor_name[64];
	ui_capture_fn on_capture;
	ui_capture_poll_fn on_capture_poll;
	ui_record_fn on_record;
	ui_record_poll_fn on_record_poll;
	ui_rec_frames_fn on_rec_frames;
	ui_rec_abort_fn on_rec_abort;
	ui_thumb_fn on_thumb;
	ui_save_dir_fn on_save_dir;
	void *ops_user;
	ui_gallery_t gallery;
	uint16_t thumb_buf[CAM_THUMB_W * CAM_THUMB_H];
	/* v9: canvas pixels are color-format sized; lv_color_t is RGB888 (3B). */
	lv_color32_t thumb_cbuf[CAM_THUMB_W * CAM_THUMB_H];
} ui_camera_t;

void ui_camera_create(ui_camera_t *ui, const char *sensor_name);
void ui_camera_set_ops(ui_camera_t *ui, ui_capture_fn capture,
		ui_record_fn record, void *user);
void ui_camera_set_poll_ops(ui_camera_t *ui, ui_capture_poll_fn capture_poll,
		ui_record_poll_fn record_poll);
void ui_camera_set_rec_frames_cb(ui_camera_t *ui, ui_rec_frames_fn fn);
void ui_camera_set_rec_abort_cb(ui_camera_t *ui, ui_rec_abort_fn fn);
void ui_camera_set_thumb_cb(ui_camera_t *ui, ui_thumb_fn fn);
void ui_camera_set_save_dir_cb(ui_camera_t *ui, ui_save_dir_fn fn);
void ui_camera_set_video_info(ui_camera_t *ui, int width, int height, int fps);
void ui_camera_set_recording(ui_camera_t *ui, int on);
void ui_camera_set_status(ui_camera_t *ui, const char *text);

#endif
