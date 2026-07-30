#ifndef CAMERA_PIPELINE_H_
#define CAMERA_PIPELINE_H_

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "common_utils.h"
#include "drm_ui_plane.h"
#include "media_still.h"
#include "vp_display.h"

/* Max in-flight burst shots waiting for a VSE frame + encode. */
#define CAM_CAP_Q 8
#define CAM_STILL_CANARY 0x53544C44u /* 'STLD' */

typedef struct camera_pipeline {
	pipe_contex_t pipe;
	vp_drm_context_t drm;
	drm_ui_plane_t *ui_overlay;
	int sensor_index;
	volatile int running;
	int display_ready;
	/*
	 * 0: VSE drain only (getframe+release). 1: also commit to DRM.
	 * Kept 0 through DRM/LVGL/UI create — early unshift+DRM stalls the
	 * flyby path (permanent getframe miss 0xffffffff).
	 */
	volatile int preview_live;
	pthread_t preview_thread;
	/* Init stages — stop/unwind is safe on half-failed create. */
	int hb_mem_opened;
	int sync_inited; /* lock + capture_cond */
	int preview_thread_started;

	/* Keep small hot fields early — large arrays follow. */
	int video_w;
	int video_h;
	int video_fps;

	pthread_mutex_t lock;
	pthread_cond_t capture_cond;
	/*
	 * Burst capture: UI enqueues paths; preview thread grabs one frame per
	 * request and encodes (lock released during JPEG). Poll drains results.
	 */
	char capture_req[CAM_CAP_Q][256];
	int capture_req_head;
	int capture_req_tail;
	volatile int capture_req_count;
	char capture_done_path[CAM_CAP_Q][256];
	int capture_done_ok[CAM_CAP_Q];
	int capture_done_head;
	int capture_done_tail;
	int capture_done_count;
	/* 1 while a packed NV12 is waiting for / undergoing JPEG encode. */
	int capture_encoding;
	unsigned capture_name_seq;
	/* Packed still waiting to encode (filled under lock, encoded after releaseframe). */
	uint8_t *capture_nv12;
	int capture_nv12_w;
	int capture_nv12_h;
	char capture_enc_path[256];
	uint16_t capture_enc_thumb[CAM_THUMB_W * CAM_THUMB_H];
	char save_dir[160];
	char last_path[256];

	int recording;
	int encode_active;
	/*
	 * Preview thread sets while codec/mux run outside cam->lock.
	 * stop_record / teardown wait until 0 before freeing mc.
	 */
	int encode_busy;
	/* start_record: heavy codec init outside lock; blocks a second start. */
	int rec_starting;
	/* Mid-REC abort: 0=none, 1=storage full, 2=mux write fail (UI polls). */
	int rec_abort;
	void *mc;      /* media_codec_context_t* */
	void *rec_mux; /* rec_mux_t* — live MP4 while recording */
	char h264_path[256]; /* legacy name; holds MP4 path during REC */
	int64_t record_pts;
	int record_frames;

	/* Silent album thumb anim: first CAM_ANIM_SECS @ CAM_ANIM_FPS (heap). */
	int anim_nframes;
	uint16_t *anim_frames; /* CAM_ANIM_MAX_FRAMES * CAM_THUMB_W * CAM_THUMB_H */

	/* Finalize mux after REC stop (write trailer off LVGL thread). */
	pthread_t remux_thread;
	int remux_job; /* 0 idle, 1 busy, 2 done-ok, 3 done-fail */
	int remux_result;
	int remux_fps;
	int remux_frames;
	char remux_h264[256]; /* unused (kept for ABI layout stability) */
	char remux_mp4[256];

	int last_thumb_valid;
	int last_thumb_w;
	int last_thumb_h;
	int capture_seq;
	uint16_t last_thumb[CAM_THUMB_W * CAM_THUMB_H];

	/* Album photo / video play: freeze live preview on NV12 plane. */
	int still_hold;
	int still_buf_ok; /* number of valid still_buf[] slots (0..2) */
	int still_idx;    /* next write index for play ping-pong */
	/*
	 * Heap descriptors — never embed hb_mem_graphic_buf_t[2] here.
	 * An in-struct still_buf[] once overflowed into adjacent BSS and
	 * clobbered media-player stop_req (play died after 1 frame).
	 * Pipeline object itself is heap-allocated in main (not BSS).
	 */
	hb_mem_graphic_buf_t *still_buf; /* calloc(2) */
	/* Integrity probe: flipped ⇒ something wrote past earlier fields. */
	uint32_t still_canary; /* CAM_STILL_CANARY */
} camera_pipeline_t;

int camera_find_sc230ai_index(void);
/*
 * Allocate + init in this TU so sizeof(*cam) cannot skew vs main.o after a
 * partial rebuild (calloc in main + memset here used to overflow the chunk →
 * glibc sysmalloc abort before "Camera sensor").
 */
camera_pipeline_t *camera_pipeline_create(int sensor_index);
int camera_pipeline_init(camera_pipeline_t *cam, int sensor_index);
void camera_pipeline_set_ui_overlay(camera_pipeline_t *cam, drm_ui_plane_t *ui);
int camera_pipeline_start_preview(camera_pipeline_t *cam);
void camera_pipeline_stop(camera_pipeline_t *cam);
/* stop + free; cam pointer is invalid after. */
void camera_pipeline_destroy(camera_pipeline_t *cam);

const char *camera_pipeline_save_dir(camera_pipeline_t *cam);

/*
 * Capture: enqueue JPEG job (non-blocking, burst up to CAM_CAP_Q).
 * poll: 1=wait, 0=one OK, -1=one fail, 2=idle (queue empty).
 * camera_pipeline_capture() blocks until that one shot finishes.
 */
int camera_pipeline_capture_begin(camera_pipeline_t *cam, char *out_path,
		size_t out_len);
int camera_pipeline_capture_poll(camera_pipeline_t *cam, char *out_path,
		size_t out_len);
int camera_pipeline_capture(camera_pipeline_t *cam, char *out_path, size_t out_len);

/* Copy last RGB565 thumb; returns 0 if valid. Optional out_w/out_h pixel size. */
int camera_pipeline_get_last_thumb(camera_pipeline_t *cam, uint16_t *dst,
		int *out_w, int *out_h, int *seq);

/*
 * Start recording (sync — codec init is quick).
 * Stop: begin tears down encode immediately and finalizes live MP4
 * (write trailer) on a worker thread; poll returns 1=busy, 0=ok, -1=fail.
 * stop_record() blocks until finalize done.
 */
int camera_pipeline_start_record(camera_pipeline_t *cam, char *out_path,
		size_t out_len);
int camera_pipeline_stop_record_begin(camera_pipeline_t *cam);
int camera_pipeline_stop_record_poll(camera_pipeline_t *cam, char *out_path,
		size_t out_len);
int camera_pipeline_stop_record(camera_pipeline_t *cam, char *out_path,
		size_t out_len);
int camera_pipeline_is_recording(camera_pipeline_t *cam);
int camera_pipeline_is_busy(camera_pipeline_t *cam);
int camera_pipeline_get_video_info(camera_pipeline_t *cam, int *w, int *h,
		int *fps);
int camera_pipeline_get_record_frames(camera_pipeline_t *cam);

/* 1 if save filesystem is below CAM_STORAGE_MIN_MB. */
int camera_pipeline_storage_full(camera_pipeline_t *cam);

/*
 * Mid-REC abort reason for UI (0=none, 1=storage, 2=mux). Non-zero means
 * encoding has stopped; UI should call stop_record_begin to finalize MP4.
 */
int camera_pipeline_rec_abort_reason(camera_pipeline_t *cam);

/*
 * Album photo preview on the NV12 video plane (same path as live preview).
 * Avoids ARGB overlay scanout wrap. path=JPEG. clear restores live frames.
 */
int camera_pipeline_show_still_jpeg(camera_pipeline_t *cam, const char *path);
void camera_pipeline_clear_still(camera_pipeline_t *cam);

/* Album MP4 on plane 33 (HW decode). stop clears still_hold. */
int camera_pipeline_play_video(camera_pipeline_t *cam, const char *mp4_path);
void camera_pipeline_stop_video(camera_pipeline_t *cam);

/*
 * Copy a strided NV12 frame into the contiguous still_buf and commit to
 * plane 33. Decoder dma-bufs are not DRM-contiguous; still_buf is.
 */
int camera_pipeline_show_nv12_frame(camera_pipeline_t *cam,
		const uint8_t *y, const uint8_t *uv, int width, int height,
		int stride, int vstride);

#endif
