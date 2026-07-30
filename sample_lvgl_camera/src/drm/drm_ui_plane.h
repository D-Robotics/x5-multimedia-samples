/***************************************************************************
 * DRM overlay plane for LVGL UI (ARGB8888 blend plane, z_pos=1).
 ***************************************************************************/
#ifndef DRM_UI_PLANE_H_
#define DRM_UI_PLANE_H_

#include <stdint.h>
#include <stdbool.h>
#include "vp_display.h"

typedef struct {
	uint32_t fb_id;
	uint32_t crtc_id;
	uint32_t src_x;
	uint32_t src_y;
	uint32_t src_w;
	uint32_t src_h;
	uint32_t crtc_x;
	uint32_t crtc_y;
	uint32_t crtc_w;
	uint32_t crtc_h;
	uint32_t zpos;
	uint32_t alpha;
	uint32_t pixel_blend_mode;
} ui_plane_prop_ids_t;

typedef struct {
	int drm_fd;
	uint32_t crtc_id;
	uint32_t plane_id;
	uint32_t width;
	uint32_t height;
	uint32_t format;

	uint32_t gem_handles[2];
	uint32_t fb_ids[2];
	void *map[2];
	uint32_t pitch;
	int front_idx;
	bool plane_enabled;
	bool props_cached;
	/* Album photo: video commits must not touch the UI plane. */
	bool content_hold;
	ui_plane_prop_ids_t prop_ids;
} drm_ui_plane_t;

int drm_ui_plane_init(drm_ui_plane_t *ui, const vp_drm_context_t *video);
void drm_ui_plane_deinit(drm_ui_plane_t *ui);
void *drm_ui_plane_get_draw_buf(drm_ui_plane_t *ui);
void drm_ui_plane_get_overlay(const drm_ui_plane_t *ui, vp_overlay_plane_t *out);
int drm_ui_plane_flip(drm_ui_plane_t *ui);
/* Clear ARGB FB(s) to transparent (A=0). */
void drm_ui_plane_clear(drm_ui_plane_t *ui);
/*
 * While hold=1, show_video updates the NV12 plane only (leaves UI FB alone).
 * flip() uses a full plane commit so geometry/alpha stay correct.
 */
void drm_ui_plane_set_content_hold(drm_ui_plane_t *ui, int hold);
/*
 * Atomic-safe video+UI update: try combined overlay commit, else
 * video set_frame + UI flip under the same commit mutex as flip().
 */
int drm_ui_plane_show_video(drm_ui_plane_t *ui, vp_drm_context_t *drm,
		hb_mem_graphic_buf_t *frame);

#endif /* DRM_UI_PLANE_H_ */
