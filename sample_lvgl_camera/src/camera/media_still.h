#ifndef MEDIA_STILL_H_
#define MEDIA_STILL_H_

#include <stddef.h>
#include <stdint.h>

#define CAM_THUMB_W  160
#define CAM_THUMB_H  90

/* Fixed album fullscreen preview size (matches 720x1280 panel). */
#define CAM_PREV_W     720
#define CAM_PREV_H     1280
#define CAM_PREV_EXT   ".p565"
/* List-row thumbnail beside media (path.jpg → path.jpg.t565). */
#define CAM_THUMB_EXT  ".t565"
/*
 * Video poster still written at REC first frame (path.mp4 → path.poster.jpg).
 * Board ffmpeg cannot extract frames (no image2/rawvideo/mjpeg muxers).
 */
#define CAM_POSTER_EXT ".poster.jpg"
/*
 * Silent looping list thumb for video (path.mp4 → path.a565).
 * REC samples first CAM_ANIM_SECS @ CAM_ANIM_FPS into RGB565 frames.
 */
#define CAM_ANIM_EXT        ".a565"
#define CAM_ANIM_FPS        5
#define CAM_ANIM_SECS       3
#define CAM_ANIM_MAX_FRAMES (CAM_ANIM_FPS * CAM_ANIM_SECS)
/* Full video duration in seconds (path.mp4 → path.vdur). */
#define CAM_VDUR_EXT        ".vdur"

/* Draw ASCII timestamp as dark ink on the Polaroid cream caption (no HUD bar).
 * Placement follows CAM_VIDEO_ROTATE so the stamp reads upright at the
 * viewed bottom after JPEG encode rotation.
 */
void media_still_nv12_watermark(uint8_t *y, uint8_t *uv, int w, int h,
		const char *text);

/*
 * Paint Polaroid cream matte into NV12. Thick caption edge lands at the
 * viewed bottom after CAM_VIDEO_ROTATE. CAM_JPEG_BORDER=0 disables.
 */
void media_still_nv12_polaroid_frame(uint8_t *y, uint8_t *uv, int w, int h,
		int stride_y);

/* Encode NV12 still to JPEG file. Returns 0 on success. */
int media_still_encode_jpeg(const uint8_t *y, const uint8_t *uv,
		int w, int h, int stride_y, int quality, const char *path);

/*
 * Hardware-decode JPEG to tightly packed NV12. On success *out_nv12 is
 * malloc'd (Y then UV, size w*h*3/2); caller must free(). Returns 0.
 */
int media_still_decode_jpeg_nv12(const char *path, uint8_t **out_nv12,
		int *out_w, int *out_h);

/*
 * Hardware-decode JPEG to RGB565 canvas (stretch-filled to dst).
 * dst is dst_w * dst_h pixels. Returns 0 on success.
 */
int media_still_decode_jpeg_rgb565(const char *path, uint16_t *dst,
		int dst_w, int dst_h);

/*
 * Video album preview: optional .p565, else REC-time .poster.jpg → RGB565.
 * Board ffmpeg is remux-only and cannot extract frames.
 */
int media_still_decode_video_rgb565(const char *path, uint16_t *dst,
		int dst_w, int dst_h);

/*
 * Write / remove JPEG poster beside a video (always, independent of CAM_SIDECAR).
 * media_path.mp4 → media_path.poster.jpg
 * Poster is sensor-sized (no Polaroid cream, no CAM_VIDEO_ROTATE) so album HW
 * play can prime plane 33 before decode.
 */
void media_still_video_poster_path(char *out, size_t out_len,
		const char *media_path);
int media_still_write_video_poster(const uint8_t *y, const uint8_t *uv,
		int w, int h, int stride_y, const char *media_path);
void media_still_remove_video_poster(const char *media_path);

/*
 * Animated list thumb beside video (path.mp4 → path.a565).
 * frames: contiguous nframes * CAM_THUMB_W * CAM_THUMB_H RGB565.
 * load: *out_frames malloc'd; caller free(). Returns 0 on success.
 */
int media_still_write_anim_thumb(const char *media_path,
		const uint16_t *frames, int nframes, int fps);
int media_still_load_anim_thumb(const char *media_path, uint16_t **out_frames,
		int *out_nframes, int *out_fps);
void media_still_remove_anim_thumb(const char *media_path);

/* Full clip duration (seconds) beside video. Returns -1 if missing. */
int media_still_write_video_duration(const char *media_path, int duration_sec);
int media_still_load_video_duration(const char *media_path);
void media_still_remove_video_duration(const char *media_path);

/*
 * Write / load RGB565 album preview sidecar beside a media path
 * (path.jpg → path.jpg.p565). Letterboxed into dst_w x dst_h.
 */
int media_still_write_rgb565_preview(const uint8_t *y, const uint8_t *uv,
		int src_w, int src_h, int stride_y, const char *media_path,
		int dst_w, int dst_h);
int media_still_load_rgb565_preview(const char *media_path, uint16_t *dst,
		int dst_w, int dst_h);
void media_still_remove_rgb565_preview(const char *media_path);

/* List-row thumbnail sidecar (CAM_THUMB_W x CAM_THUMB_H). */
int media_still_write_list_thumb(const char *media_path,
		const uint16_t *rgb565);
int media_still_load_list_thumb(const char *media_path, uint16_t *dst);
void media_still_remove_list_thumb(const char *media_path);

/* Nearest-neighbor NV12 -> RGB565 thumbnail into buf[CAM_THUMB_W*CAM_THUMB_H]. */
void media_still_nv12_thumb(const uint8_t *y, const uint8_t *uv,
		int w, int h, uint16_t *rgb565);

/*
 * Orient a CAM_THUMB_W x CAM_THUMB_H RGB565 buffer like JPEG/live preview
 * (CAM_VIDEO_ROTATE). Writes into dst (same pixel count). out_w/out_h set.
 * deg 90 on this board matches CW (same as MC_CCW_90 JPEG path).
 */
void media_still_rgb565_orient_view(const uint16_t *src, uint16_t *dst,
		int *out_w, int *out_h);

/*
 * Build list thumb RGB565 from a JPEG (no sidecar). Returns 0 on success.
 */
int media_still_jpeg_to_list_thumb(const char *jpeg_path, uint16_t *dst);

/*
 * Build list thumb from video poster JPEG (REC-time). Returns 0 on success.
 */
int media_still_video_to_list_thumb(const char *video_path, uint16_t *dst);

/*
 * Require path under save_dir after realpath (blocks ../ and escaping symlinks).
 * for_unlink=1: also allow deleting an in-dir symlink whose target is outside
 * (unlinks the link name under the resolved save_dir, not the target).
 * On success, writes a safe absolute path into out (if non-NULL).
 */
int media_still_confine_path(const char *save_dir, const char *path,
		char *out, size_t out_len, int for_unlink);

#endif
