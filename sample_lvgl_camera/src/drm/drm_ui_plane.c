/***************************************************************************
 * DRM overlay plane for LVGL UI (ARGB8888 blend plane, z_pos=1).
 ***************************************************************************/
#include "drm_ui_plane.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <drm/drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define UI_VIDEO_PLANE_ID  33
#define UI_Z_POS           1
#define UI_ALPHA           65535
#define UI_BLEND_MODE      1

/* Serialize video plane + UI plane commits (main + preview threads). */
static pthread_mutex_t g_drm_commit_mu = PTHREAD_MUTEX_INITIALIZER;

static int ui_find_prop_id(int drm_fd, uint32_t plane_id, const char *name,
		uint32_t *out)
{
	drmModeObjectProperties *props;
	uint32_t i;

	props = drmModeObjectGetProperties(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE);
	if (!props) {
		fprintf(stderr, "drm_ui_plane: get props for plane %u failed: %s\n",
			plane_id, strerror(errno));
		return -1;
	}

	*out = 0;
	for (i = 0; i < props->count_props; i++) {
		drmModePropertyRes *prop =
			drmModeGetProperty(drm_fd, props->props[i]);

		if (!prop)
			continue;
		if (strcmp(prop->name, name) == 0) {
			*out = prop->prop_id;
			drmModeFreeProperty(prop);
			break;
		}
		drmModeFreeProperty(prop);
	}
	drmModeFreeObjectProperties(props);

	if (!*out) {
		fprintf(stderr, "drm_ui_plane: plane %u missing property '%s'\n",
			plane_id, name);
		return -1;
	}
	return 0;
}

static int ui_cache_plane_props(drm_ui_plane_t *ui)
{
	if (ui_find_prop_id(ui->drm_fd, ui->plane_id, "FB_ID", &ui->prop_ids.fb_id))
		return -1;
	if (ui_find_prop_id(ui->drm_fd, ui->plane_id, "CRTC_ID", &ui->prop_ids.crtc_id))
		return -1;
	if (ui_find_prop_id(ui->drm_fd, ui->plane_id, "SRC_X", &ui->prop_ids.src_x))
		return -1;
	if (ui_find_prop_id(ui->drm_fd, ui->plane_id, "SRC_Y", &ui->prop_ids.src_y))
		return -1;
	if (ui_find_prop_id(ui->drm_fd, ui->plane_id, "SRC_W", &ui->prop_ids.src_w))
		return -1;
	if (ui_find_prop_id(ui->drm_fd, ui->plane_id, "SRC_H", &ui->prop_ids.src_h))
		return -1;
	if (ui_find_prop_id(ui->drm_fd, ui->plane_id, "CRTC_X", &ui->prop_ids.crtc_x))
		return -1;
	if (ui_find_prop_id(ui->drm_fd, ui->plane_id, "CRTC_Y", &ui->prop_ids.crtc_y))
		return -1;
	if (ui_find_prop_id(ui->drm_fd, ui->plane_id, "CRTC_W", &ui->prop_ids.crtc_w))
		return -1;
	if (ui_find_prop_id(ui->drm_fd, ui->plane_id, "CRTC_H", &ui->prop_ids.crtc_h))
		return -1;
	if (ui_find_prop_id(ui->drm_fd, ui->plane_id, "zpos", &ui->prop_ids.zpos))
		return -1;
	if (ui_find_prop_id(ui->drm_fd, ui->plane_id, "alpha", &ui->prop_ids.alpha))
		return -1;
	if (ui_find_prop_id(ui->drm_fd, ui->plane_id, "pixel blend mode",
			&ui->prop_ids.pixel_blend_mode))
		return -1;

	ui->props_cached = true;
	return 0;
}

static int ui_plane_has_format(const drmModePlane *plane, uint32_t fmt)
{
	uint32_t i;

	for (i = 0; i < plane->count_formats; i++) {
		if (plane->formats[i] == fmt)
			return 1;
	}
	return 0;
}

static int ui_plane_has_blend_prop(int drm_fd, uint32_t plane_id)
{
	drmModeObjectProperties *props;
	uint32_t i;
	int found = 0;

	props = drmModeObjectGetProperties(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE);
	if (!props)
		return 0;

	for (i = 0; i < props->count_props; i++) {
		drmModePropertyRes *prop =
			drmModeGetProperty(drm_fd, props->props[i]);

		if (!prop)
			continue;
		if (strcmp(prop->name, "pixel blend mode") == 0)
			found = 1;
		drmModeFreeProperty(prop);
		if (found)
			break;
	}
	drmModeFreeObjectProperties(props);
	return found;
}

static int ui_crtc_index(int drm_fd, uint32_t crtc_id)
{
	drmModeRes *res = drmModeGetResources(drm_fd);
	int idx = -1;
	uint32_t i;

	if (!res)
		return -1;

	for (i = 0; i < res->count_crtcs; i++) {
		if (res->crtcs[i] == crtc_id) {
			idx = (int)i;
			break;
		}
	}
	drmModeFreeResources(res);
	return idx;
}

static int ui_plane_rank(uint32_t pid)
{
	if (pid == 40)
		return 0;
	if (pid == 41)
		return 1;
	if (pid == 48)
		return 2;
	return 100 + (int)pid;
}

static uint32_t ui_find_blend_plane(drm_ui_plane_t *ui, uint32_t video_plane_id)
{
	drmModePlaneRes *plane_res;
	uint32_t crtc_mask;
	int crtc_idx;
	uint32_t best = 0;
	int best_rank = 1000;
	uint32_t i;

	crtc_idx = ui_crtc_index(ui->drm_fd, ui->crtc_id);
	if (crtc_idx < 0)
		return 0;

	crtc_mask = 1U << crtc_idx;
	plane_res = drmModeGetPlaneResources(ui->drm_fd);
	if (!plane_res) {
		fprintf(stderr, "drm_ui_plane: drmModeGetPlaneResources failed\n");
		return 0;
	}

	printf("drm_ui_plane: scanning %u planes for ARGB blend overlay...\n",
		plane_res->count_planes);

	for (i = 0; i < plane_res->count_planes; i++) {
		uint32_t pid = plane_res->planes[i];
		drmModePlane *plane = drmModeGetPlane(ui->drm_fd, pid);
		int rank;

		if (!plane)
			continue;

		if (pid == video_plane_id ||
		    !(plane->possible_crtcs & crtc_mask) ||
		    !ui_plane_has_format(plane, DRM_FORMAT_ARGB8888) ||
		    !ui_plane_has_blend_prop(ui->drm_fd, pid)) {
			drmModeFreePlane(plane);
			continue;
		}

		rank = ui_plane_rank(pid);
		printf("drm_ui_plane: candidate overlay plane %u (rank %d)\n", pid, rank);
		if (!best || rank < best_rank) {
			best = pid;
			best_rank = rank;
		}

		drmModeFreePlane(plane);
	}

	drmModeFreePlaneResources(plane_res);
	return best;
}

static int ui_create_dumb_fb(drm_ui_plane_t *ui, int idx)
{
	struct drm_mode_create_dumb create = {
		.width = ui->width,
		.height = ui->height,
		.bpp = 32,
	};
	struct drm_mode_map_dumb map_arg;
	int ret;

	ret = drmIoctl(ui->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);
	if (ret)
		return -1;

	ui->gem_handles[idx] = create.handle;
	ui->pitch = create.pitch;

	{
		uint32_t handles[4] = { ui->gem_handles[idx], 0, 0, 0 };
		uint32_t pitches[4] = { ui->pitch, 0, 0, 0 };
		uint32_t offsets[4] = { 0, 0, 0, 0 };

		ret = drmModeAddFB2(ui->drm_fd, ui->width, ui->height, ui->format,
				handles, pitches, offsets, &ui->fb_ids[idx], 0);
	}
	if (ret)
		return -1;

	memset(&map_arg, 0, sizeof(map_arg));
	map_arg.handle = create.handle;
	ret = drmIoctl(ui->drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map_arg);
	if (ret)
		return -1;

	ui->map[idx] = mmap(NULL, create.size, PROT_READ | PROT_WRITE,
			MAP_SHARED, ui->drm_fd, map_arg.offset);
	if (ui->map[idx] == MAP_FAILED)
		return -1;

	memset(ui->map[idx], 0, create.size);
	return 0;
}

static int ui_commit_plane_minimal(drm_ui_plane_t *ui, uint32_t fb_id)
{
	drmModeAtomicReq *req;
	int ret;

	if (!ui->props_cached)
		return -1;

	req = drmModeAtomicAlloc();
	if (!req)
		return -1;

	drmModeAtomicAddProperty(req, ui->plane_id, ui->prop_ids.fb_id, fb_id);
	drmModeAtomicAddProperty(req, ui->plane_id, ui->prop_ids.crtc_id, ui->crtc_id);

	ret = drmModeAtomicCommit(ui->drm_fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	drmModeAtomicFree(req);
	return ret;
}

static int ui_commit_plane(drm_ui_plane_t *ui, uint32_t fb_id)
{
	drmModeAtomicReq *req;
	int ret;

	if (!ui->props_cached)
		return -1;

	req = drmModeAtomicAlloc();
	if (!req)
		return -1;

	drmModeAtomicAddProperty(req, ui->plane_id, ui->prop_ids.fb_id, fb_id);
	drmModeAtomicAddProperty(req, ui->plane_id, ui->prop_ids.crtc_id, ui->crtc_id);
	drmModeAtomicAddProperty(req, ui->plane_id, ui->prop_ids.src_x, 0);
	drmModeAtomicAddProperty(req, ui->plane_id, ui->prop_ids.src_y, 0);
	drmModeAtomicAddProperty(req, ui->plane_id, ui->prop_ids.src_w, ui->width << 16);
	drmModeAtomicAddProperty(req, ui->plane_id, ui->prop_ids.src_h, ui->height << 16);
	drmModeAtomicAddProperty(req, ui->plane_id, ui->prop_ids.crtc_x, 0);
	drmModeAtomicAddProperty(req, ui->plane_id, ui->prop_ids.crtc_y, 0);
	drmModeAtomicAddProperty(req, ui->plane_id, ui->prop_ids.crtc_w, ui->width);
	drmModeAtomicAddProperty(req, ui->plane_id, ui->prop_ids.crtc_h, ui->height);
	drmModeAtomicAddProperty(req, ui->plane_id, ui->prop_ids.zpos, UI_Z_POS);
	drmModeAtomicAddProperty(req, ui->plane_id, ui->prop_ids.alpha, UI_ALPHA);
	drmModeAtomicAddProperty(req, ui->plane_id, ui->prop_ids.pixel_blend_mode,
		UI_BLEND_MODE);

	ret = drmModeAtomicCommit(ui->drm_fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	drmModeAtomicFree(req);
	return ret;
}

int drm_ui_plane_init(drm_ui_plane_t *ui, const vp_drm_context_t *video)
{
	uint32_t panel_w = video->planes[0].crtc_w;
	uint32_t panel_h = video->planes[0].crtc_h;
	uint32_t video_plane = video->planes[0].plane_id;

	if (!ui || !video || video->drm_fd < 0)
		return -1;

	if (!panel_w || !panel_h) {
		panel_w = video->width;
		panel_h = video->height;
	}

	memset(ui, 0, sizeof(*ui));
	ui->drm_fd = video->drm_fd;
	ui->crtc_id = video->crtc_id;
	ui->width = panel_w;
	ui->height = panel_h;
	ui->format = DRM_FORMAT_ARGB8888;
	ui->front_idx = 0;

	ui->plane_id = ui_find_blend_plane(ui, video_plane);
	if (!ui->plane_id) {
		fprintf(stderr, "drm_ui_plane: no ARGB blend overlay plane found\n");
		return -1;
	}

	if (ui_cache_plane_props(ui) != 0) {
		fprintf(stderr, "drm_ui_plane: failed to cache plane %u properties\n",
			ui->plane_id);
		return -1;
	}

	if (ui_create_dumb_fb(ui, 0) || ui_create_dumb_fb(ui, 1)) {
		fprintf(stderr, "drm_ui_plane: failed to create dumb buffers\n");
		drm_ui_plane_deinit(ui);
		return -1;
	}

	if (ui_commit_plane(ui, ui->fb_ids[0]) != 0) {
		fprintf(stderr, "drm_ui_plane: failed to enable plane %u\n",
			ui->plane_id);
		drm_ui_plane_deinit(ui);
		return -1;
	}

	ui->plane_enabled = true;
	printf("drm_ui_plane: %ux%u ARGB on plane %u (z=%d)\n",
		ui->width, ui->height, ui->plane_id, UI_Z_POS);
	return 0;
}

void drm_ui_plane_deinit(drm_ui_plane_t *ui)
{
	int i;

	if (!ui)
		return;

	for (i = 0; i < 2; i++) {
		if (ui->map[i] && ui->map[i] != MAP_FAILED)
			munmap(ui->map[i], ui->height * ui->pitch);
		if (ui->fb_ids[i])
			drmModeRmFB(ui->drm_fd, ui->fb_ids[i]);
		if (ui->gem_handles[i]) {
			struct drm_mode_destroy_dumb destroy = {
				.handle = ui->gem_handles[i],
			};
			drmIoctl(ui->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
		}
	}
	memset(ui, 0, sizeof(*ui));
}

void *drm_ui_plane_get_draw_buf(drm_ui_plane_t *ui)
{
	if (!ui)
		return NULL;
	return ui->map[ui->front_idx];
}

void drm_ui_plane_get_overlay(const drm_ui_plane_t *ui, vp_overlay_plane_t *out)
{
	if (!ui || !out)
		return;

	memset(out, 0, sizeof(*out));
	out->plane_id = ui->plane_id;
	out->fb_id = ui->fb_ids[ui->front_idx];
	out->crtc_id = ui->crtc_id;
	out->width = ui->width;
	out->height = ui->height;
	out->z_pos = UI_Z_POS;
	out->alpha = UI_ALPHA;
	out->pixel_blend_mode = UI_BLEND_MODE;

	if (!ui->props_cached)
		return;

	out->props_cached = true;
	out->prop_fb_id = ui->prop_ids.fb_id;
	out->prop_crtc_id = ui->prop_ids.crtc_id;
	out->prop_src_x = ui->prop_ids.src_x;
	out->prop_src_y = ui->prop_ids.src_y;
	out->prop_src_w = ui->prop_ids.src_w;
	out->prop_src_h = ui->prop_ids.src_h;
	out->prop_crtc_x = ui->prop_ids.crtc_x;
	out->prop_crtc_y = ui->prop_ids.crtc_y;
	out->prop_crtc_w = ui->prop_ids.crtc_w;
	out->prop_crtc_h = ui->prop_ids.crtc_h;
	out->prop_zpos = ui->prop_ids.zpos;
	out->prop_alpha = ui->prop_ids.alpha;
	out->prop_blend_mode = ui->prop_ids.pixel_blend_mode;
}

int drm_ui_plane_flip(drm_ui_plane_t *ui)
{
	int ret;

	if (!ui || !ui->plane_enabled)
		return -1;

	pthread_mutex_lock(&g_drm_commit_mu);
	/* Full commit while holding a still — keep CRTC/SRC/alpha/blend intact. */
	if (ui->content_hold)
		ret = ui_commit_plane(ui, ui->fb_ids[ui->front_idx]);
	else
		ret = ui_commit_plane_minimal(ui, ui->fb_ids[ui->front_idx]);
	pthread_mutex_unlock(&g_drm_commit_mu);
	return ret;
}

void drm_ui_plane_set_content_hold(drm_ui_plane_t *ui, int hold)
{
	if (!ui)
		return;
	ui->content_hold = hold ? true : false;
}

void drm_ui_plane_clear(drm_ui_plane_t *ui)
{
	size_t sz;
	int i;

	if (!ui || !ui->height || !ui->pitch)
		return;
	sz = (size_t)ui->height * (size_t)ui->pitch;
	for (i = 0; i < 2; i++) {
		if (ui->map[i] && ui->map[i] != MAP_FAILED)
			memset(ui->map[i], 0, sz);
	}
}

int drm_ui_plane_show_video(drm_ui_plane_t *ui, vp_drm_context_t *drm,
		hb_mem_graphic_buf_t *frame)
{
	vp_overlay_plane_t ov;
	int ret;

	if (!drm || !frame)
		return -1;

	pthread_mutex_lock(&g_drm_commit_mu);
	if (ui && ui->plane_enabled && ui->content_hold) {
		/* Still preview owns the UI FB — do not re-commit overlay. */
		ret = vp_display_set_frame(drm, frame);
	} else if (ui && ui->plane_enabled) {
		drm_ui_plane_get_overlay(ui, &ov);
		ret = vp_display_set_frame_with_overlay(drm, frame, &ov);
		if (ret != 0) {
			/* MIPI often rejects dual-plane commits; split path. */
			ret = vp_display_set_frame(drm, frame);
			if (ret == 0)
				ret = ui_commit_plane_minimal(ui,
					ui->fb_ids[ui->front_idx]);
		}
	} else {
		ret = vp_display_set_frame(drm, frame);
	}
	pthread_mutex_unlock(&g_drm_commit_mu);
	return ret;
}
