#ifndef LVGL_DISPLAY_H_
#define LVGL_DISPLAY_H_

#include <stdint.h>

#include "drm_ui_plane.h"

int lvgl_display_init(drm_ui_plane_t *ui);
void lvgl_display_deinit(void);
void lvgl_display_refresh(void);
int lvgl_display_take_dirty(void);

/*
 * Stamp opaque Polaroid edges into the DRM FB after every LVGL flush.
 * left/right/top/bottom are pixel widths from FB edges (include overscan).
 * color_rgb24 is 0xRRGGBB (alpha forced to 0xFF). Pass 0 widths to disable.
 */
void lvgl_display_set_polaroid_frame(int left_px, int right_px, int top_px,
		int bottom_px, uint32_t color_rgb24);

/* Album open/close: stash&clear polaroid, then restore (do not trust UI state). */
void lvgl_display_polaroid_suspend(void);
void lvgl_display_polaroid_resume(void);

/*
 * Album photo overlay: deep-copy tightly-packed ARGB8888 and stamp into both
 * DRM UI FBs. While active, LVGL flush skips memcpy (transform dirty-rects
 * were still wrapping the photo by ~overscan even when re-stamped after blit).
 */
void lvgl_display_set_fullscreen_argb(const void *argb8888, int w, int h);
void lvgl_display_clear_fullscreen_argb(void);

/*
 * Album photo on video plane: clear UI FB to transparent and freeze LVGL
 * blits so the NV12 video is visible. Chrome (play/pause + time + exit) is
 * stamped via lvgl_display_set_video_chrome().
 */
void lvgl_display_hold_blank(int enable);

/*
 * Arm blank-hold without scanout flip. Keeps the previous UI FB until the
 * next lvgl_display_set_video_chrome() commits paper+controls in one flip
 * (avoids a transparent flash on album video open).
 */
void lvgl_display_hold_blank_arm(void);

/* Legacy text-only HUD (kept for callers); prefer set_video_chrome. */
void lvgl_display_set_video_hud(const char *text, int bar_rot180);
void lvgl_display_clear_video_hud(void);

/* Mini-player chrome while blank-hold is on (DRM-painted, hit-tested). */
enum {
	LVGL_VCHROME_NONE = 0,
	LVGL_VCHROME_PLAY = 1,
	LVGL_VCHROME_EXIT = 2
};

typedef struct {
	const char *time_text; /* e.g. "00:03 / 00:08" */
	int show_pause_icon;   /* 1=playing (pause bars), 0=paused/ended (play △) */
	int show_exit;         /* left-frame back "<" visible */
	int bar_rot180;
} lvgl_video_chrome_t;

void lvgl_display_set_video_chrome(const lvgl_video_chrome_t *chrome);
/* FB-coordinate hit test for the last painted chrome. */
int lvgl_display_video_chrome_hit(int x, int y);

#endif
