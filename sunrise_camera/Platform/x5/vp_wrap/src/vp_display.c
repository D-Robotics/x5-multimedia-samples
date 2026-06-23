#include "vp_display.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libudev.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <unistd.h>

#include "utils/cJSON.h"
#include "utils/common_utils.h"
#include "utils/utils_log.h"
#include "vp_wrap.h"

static struct udev *g_hp_udev;
static struct udev_monitor *g_hp_mon;
static int g_hp_fd = -1;
/* 无可靠 udev 时，timerfd 每秒产生 POLLIN，在 poll 内读 HDMI 状态 */
static int g_hdmi_timer_fd = -1;

#define VP_DISPLAY_HDMI_POLL_INTERVAL_SEC 1

struct vp_dma_buf_sync {
	uint64_t flags;
};

#define VP_DMA_BUF_IOCTL_BASE 'b'
#define VP_DMA_BUF_SYNC_READ (1ULL << 0)
#define VP_DMA_BUF_SYNC_WRITE (2ULL << 0)
#define VP_DMA_BUF_SYNC_END (1ULL << 2)
#define VP_DMA_BUF_IOCTL_SYNC _IOW(VP_DMA_BUF_IOCTL_BASE, 0, struct vp_dma_buf_sync)
#define IS_MODE_INTERLACED(mode) ((mode)->flags & DRM_MODE_FLAG_INTERLACE)

static void vp_drm_page_flip_handler(int fd, unsigned int sequence, unsigned int tv_sec, unsigned int tv_usec,
				     void *user_data)
{
	vp_drm_context_t *ctx = (vp_drm_context_t *)user_data;

	(void)fd;
	(void)sequence;
	(void)tv_sec;
	(void)tv_usec;

	if (ctx)
		ctx->flip_pending = 0;
}

static void vp_drm_page_flip_handler2(int fd, unsigned int sequence, unsigned int tv_sec, unsigned int tv_usec,
				      unsigned int crtc_id, void *user_data)
{
	vp_drm_context_t *ctx = (vp_drm_context_t *)user_data;

	(void)fd;
	(void)sequence;
	(void)tv_sec;
	(void)tv_usec;
	(void)crtc_id;

	if (ctx)
		ctx->flip_pending = 0;
}

int32_t vp_display_handle_drm_event(vp_drm_context_t *drm_ctx)
{
	static drmEventContext ev;
	static int inited;

	if (!inited) {
		memset(&ev, 0, sizeof(ev));
		ev.version = DRM_EVENT_CONTEXT_VERSION;
		ev.page_flip_handler = vp_drm_page_flip_handler;
		ev.page_flip_handler2 = vp_drm_page_flip_handler2;
		inited = 1;
	}

	if (!drm_ctx || drm_ctx->drm_fd < 0)
		return -1;

	return drmHandleEvent(drm_ctx->drm_fd, &ev);
}

static void add_property(int drm_fd, drmModeAtomicReq *req, uint32_t obj_id, uint32_t obj_type, const char *name,
			 uint64_t value)
{
	drmModeObjectProperties *props = drmModeObjectGetProperties(drm_fd, obj_id, obj_type);
	if (!props) {
		fprintf(stderr, "Failed to get properties for object %u\n", obj_id);
		return;
	}

	uint32_t prop_id = 0;
	for (uint32_t i = 0; i < props->count_props; i++) {
		drmModePropertyRes *prop = drmModeGetProperty(drm_fd, props->props[i]);
		if (!prop) {
			continue;
		}

		if (strcmp(prop->name, name) == 0) {
			prop_id = prop->prop_id;
			drmModeFreeProperty(prop);
			break;
		}

		drmModeFreeProperty(prop);
	}

	drmModeFreeObjectProperties(props);

	if (prop_id == 0) {
		fprintf(stderr, "Property '%s' not found on object %u\n", name, obj_id);
		return;
	}

	if (drmModeAtomicAddProperty(req, obj_id, prop_id, value) < 0) {
		fprintf(stderr, "Failed to add property '%s' on object %u: %s\n", name, obj_id, strerror(errno));
	}
}

static void drm_set_client_capabilities(int drm_fd)
{
	if (drmSetClientCap(drm_fd, DRM_CLIENT_CAP_ATOMIC, 1) < 0) {
		perror("drmSetClientCap DRM_CLIENT_CAP_ATOMIC");
	}

	if (drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) < 0) {
		perror("drmSetClientCap DRM_CLIENT_CAP_UNIVERSAL_PLANES");
	}
}

static void print_connector_info(int drm_fd)
{
	drmModeRes *resources = drmModeGetResources(drm_fd);
	if (!resources) {
		fprintf(stderr, "Failed to get DRM resources\n");
		return;
	}

	for (int i = 0; i < resources->count_crtcs; i++) {
		printf("CRTC ID: %d\n", resources->crtcs[i]);
	}

	printf("Number of connectors: %d\n", resources->count_connectors);

	for (int i = 0; i < resources->count_connectors; i++) {
		uint32_t connector_id = resources->connectors[i];
		drmModeConnector *connector = drmModeGetConnector(drm_fd, connector_id);
		if (!connector) {
			fprintf(stderr, "Failed to get connector %u\n", connector_id);
			continue;
		}

		printf("Connector ID: %u\n", connector_id);
		printf("    Type: %d\n", connector->connector_type);
		printf("    Type Name: %s\n", drmModeGetConnectorTypeName(connector->connector_type));
		printf("    Connection: %s\n",
		       connector->connection == DRM_MODE_CONNECTED ? "Connected" : "Disconnected");
		printf("    Modes: %d\n", connector->count_modes);
		printf("    Subpixel: %d\n", connector->subpixel);

		for (int j = 0; j < connector->count_modes; j++) {
			drmModeModeInfo *mode = &connector->modes[j];
			printf("    Mode %d: %s @ %dHz\n", j, mode->name, mode->vrefresh);
		}

		drmModeFreeConnector(connector);
	}

	drmModeFreeResources(resources);
}

static float __mode_vrefresh(drmModeModeInfo *mode)
{
	unsigned int num, den;

	num = mode->clock;
	den = mode->htotal * mode->vtotal;

	if (mode->flags & DRM_MODE_FLAG_INTERLACE)
		num *= 2;
	if (mode->flags & DRM_MODE_FLAG_DBLSCAN)
		den *= 2;
	if (mode->vscan > 1)
		den *= mode->vscan;

	return num * 1000.00 / den;
}

static int drm_setup_kms(vp_drm_context_t *ctx)
{
	print_connector_info(ctx->drm_fd);

	drmModeRes *resources = drmModeGetResources(ctx->drm_fd);
	if (!resources) {
		perror("drmModeGetResources");
		return -1;
	}

	drmModeConnector *connector = drmModeGetConnector(ctx->drm_fd, ctx->connector_id);
	if (!connector) {
		perror("drmModeGetConnector");
		drmModeFreeResources(resources);
		return -1;
	}

	drmModeCrtc *crtc = drmModeGetCrtc(ctx->drm_fd, ctx->crtc_id);
	if (!crtc) {
		perror("drmModeGetCrtc");
		drmModeFreeConnector(connector);
		drmModeFreeResources(resources);
		return -1;
	}

	float fps = 30.0;
	drmModeModeInfo *mode = NULL;

	if (ctx->target_refresh_hz > 0) {
		float target = (float)ctx->target_refresh_hz;
		float best_diff = 1e6f;
		drmModeModeInfo *best = NULL;
		for (int i = 0; i < connector->count_modes; i++) {
			if (connector->modes[i].hdisplay != ctx->width || connector->modes[i].vdisplay != ctx->height)
				continue;
			if (connector->modes[i].flags & DRM_MODE_FLAG_INTERLACE)
				continue;
			float vf = __mode_vrefresh(&connector->modes[i]);
			float diff = fabsf(vf - target);
			if (diff < best_diff) {
				best_diff = diff;
				best = &connector->modes[i];
			}
		}
		if (best) {
			mode = best;
			fps = __mode_vrefresh(mode);
			printf("drm_setup_kms: target %d Hz -> mode %.2f Hz (best diff %.2f)\n", ctx->target_refresh_hz,
			       fps, best_diff);
		}
	}

	if (!mode) {
		for (int i = 0; i < connector->count_modes; i++) {
			if (connector->modes[i].hdisplay == ctx->width && connector->modes[i].vdisplay == ctx->height
			    && !(connector->modes[i].flags & DRM_MODE_FLAG_INTERLACE)) {
				mode = &connector->modes[i];
				fps = __mode_vrefresh(mode);
				printf("fps:%f\n", fps);
				if ((fps <= 31.00) && (fps >= 28.00)) {
					printf("select %f\n", fps);
					break;
				}
			}
		}
	}

	if (!mode) {
		perror("not found sutiable mode\n");
#if 1
		drmModeFreeCrtc(crtc);
		drmModeFreeConnector(connector);
		drmModeFreeResources(resources);
		return -1;
#else
		if (connector->count_modes > 0) {
			mode = &connector->modes[0];
			fprintf(stderr, "Mode not found set resolution %d %d, so use default %d %d\n", ctx->width,
				ctx->height, connector->modes[0].hdisplay, connector->modes[0].vdisplay);
		} else {
			fprintf(stderr, "Mode not found");
			drmModeFreeCrtc(crtc);
			drmModeFreeConnector(connector);
			drmModeFreeResources(resources);
			return -1;
		}

#endif
	}

	uint32_t blob_id;
	if (drmModeCreatePropertyBlob(ctx->drm_fd, mode, sizeof(*mode), &blob_id) < 0) {
		perror("drmModeCreatePropertyBlob");
		drmModeFreeCrtc(crtc);
		drmModeFreeConnector(connector);
		drmModeFreeResources(resources);
		return -1;
	}

	drmModeAtomicReq *req = drmModeAtomicAlloc();
	if (!req) {
		perror("drmModeAtomicAlloc");
		drmModeFreeCrtc(crtc);
		drmModeFreeConnector(connector);
		drmModeFreeResources(resources);
		return -1;
	}

	uint32_t flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
	add_property(ctx->drm_fd, req, ctx->crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", 1);
	add_property(ctx->drm_fd, req, ctx->crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID", blob_id);
	add_property(ctx->drm_fd, req, ctx->connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", ctx->crtc_id);

	for (int i = 0; i < ctx->plane_count; i++) {
		add_property(ctx->drm_fd, req, ctx->planes[i].plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X", 0);
		add_property(ctx->drm_fd, req, ctx->planes[i].plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y", 0);
		add_property(ctx->drm_fd, req, ctx->planes[i].plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W",
			     ctx->planes[i].src_w << 16);
		add_property(ctx->drm_fd, req, ctx->planes[i].plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H",
			     ctx->planes[i].src_h << 16);

		add_property(ctx->drm_fd, req, ctx->planes[i].plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X",
			     ctx->planes[i].crtc_x);
		add_property(ctx->drm_fd, req, ctx->planes[i].plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y",
			     ctx->planes[i].crtc_y);
		add_property(ctx->drm_fd, req, ctx->planes[i].plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W",
			     ctx->planes[i].crtc_w);
		add_property(ctx->drm_fd, req, ctx->planes[i].plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H",
			     ctx->planes[i].crtc_h);

		if (ctx->planes[i].z_pos != -1) {
			add_property(ctx->drm_fd, req, ctx->planes[i].plane_id, DRM_MODE_OBJECT_PLANE, "zpos",
				     ctx->planes[i].z_pos);
		}

		if (ctx->planes[i].alpha != -1) {
			add_property(ctx->drm_fd, req, ctx->planes[i].plane_id, DRM_MODE_OBJECT_PLANE, "alpha",
				     ctx->planes[i].alpha);
		}

		if (ctx->planes[i].pixel_blend_mode != -1) {
			add_property(ctx->drm_fd, req, ctx->planes[i].plane_id, DRM_MODE_OBJECT_PLANE,
				     "pixel blend mode", ctx->planes[i].pixel_blend_mode);
		}

		if (ctx->planes[i].rotation != -1) {
			add_property(ctx->drm_fd, req, ctx->planes[i].plane_id, DRM_MODE_OBJECT_PLANE, "rotation",
				     ctx->planes[i].rotation);
		}

		if (ctx->planes[i].color_encoding != -1) {
			add_property(ctx->drm_fd, req, ctx->planes[i].plane_id, DRM_MODE_OBJECT_PLANE, "COLOR_ENCODING",
				     ctx->planes[i].color_encoding);
		}

		if (ctx->planes[i].color_range != -1) {
			add_property(ctx->drm_fd, req, ctx->planes[i].plane_id, DRM_MODE_OBJECT_PLANE, "COLOR_RANGE",
				     ctx->planes[i].color_range);
		}
	}

	if (drmModeAtomicCommit(ctx->drm_fd, req, flags, NULL) < 0) {
		perror("drmModeAtomicCommit");
		drmModeAtomicFree(req);
		drmModeFreeCrtc(crtc);
		drmModeFreeConnector(connector);
		drmModeFreeResources(resources);
		return -1;
	}

	drmModeAtomicFree(req);
	drmModeFreeCrtc(crtc);
	drmModeFreeConnector(connector);
	drmModeFreeResources(resources);

	return 0;
}

static drmModeConnector *find_connector(int fd)
{
	int i = 0;

	// drmModeRes描述了计算机所有的显卡信息：connector，encoder，crtc，modes等。
	drmModeRes *resources = drmModeGetResources(fd);
	if (!resources) {
		return NULL;
	}

	drmModeConnector *conn = NULL;
	for (i = 0; i < resources->count_connectors; i++) {
		conn = drmModeGetConnector(fd, resources->connectors[i]);
		if (conn != NULL) {
			// Check if the connector type is HDMI and it's connected
			if (conn->connector_type == DRM_MODE_CONNECTOR_HDMIA && conn->connection == DRM_MODE_CONNECTED
			    && conn->count_modes > 0) {
				break;	// Found an HDMI connector that is connected and has available modes
			} else {
				drmModeFreeConnector(conn);
				conn = NULL;  // Reset conn to NULL if it's not what we're looking for
			}
		}
	}

	drmModeFreeResources(resources);
	return conn;  // Will return NULL if no suitable connector was found
}

static void drm_init_config(vp_drm_context_t *drm_ctx, int32_t width, int32_t height, int32_t target_refresh_hz)
{
	memset(drm_ctx, 0, sizeof(vp_drm_context_t));
	drm_ctx->crtc_id = 63;
	drm_ctx->connector_id = 117;
	drm_ctx->width = width;
	drm_ctx->height = height;
	drm_ctx->target_refresh_hz = target_refresh_hz;

	drm_ctx->plane_count = 1;

	for (int i = 0; i < drm_ctx->plane_count; i++) {
		drm_ctx->planes[i].plane_id = 64;
		drm_ctx->planes[i].src_w = width;
		drm_ctx->planes[i].src_h = height;
		drm_ctx->planes[i].crtc_x = 0;
		drm_ctx->planes[i].crtc_y = 0;
		drm_ctx->planes[i].crtc_w = width;
		drm_ctx->planes[i].crtc_h = height;
		strcpy(drm_ctx->planes[i].format, "NV12");

		drm_ctx->planes[i].z_pos = -1;
		drm_ctx->planes[i].alpha = -1;
		drm_ctx->planes[i].pixel_blend_mode = -1;
		drm_ctx->planes[i].rotation = -1;
		drm_ctx->planes[i].color_encoding = -1;
		drm_ctx->planes[i].color_range = -1;

		printf("------------------------------------------------------\n");
		printf("Plane %d:\n", i);
		printf("  Plane ID: %d\n", drm_ctx->planes[i].plane_id);
		printf("  Src W: %d\n", drm_ctx->planes[i].src_w);
		printf("  Src H: %d\n", drm_ctx->planes[i].src_h);
		printf("  CRTC X: %d\n", drm_ctx->planes[i].crtc_x);
		printf("  CRTC Y: %d\n", drm_ctx->planes[i].crtc_y);
		printf("  CRTC W: %d\n", drm_ctx->planes[i].crtc_w);
		printf("  CRTC H: %d\n", drm_ctx->planes[i].crtc_h);
		printf("  Format: %s\n", drm_ctx->planes[i].format);
		printf("  Z Pos: %d\n", drm_ctx->planes[i].z_pos);
		printf("  Alpha: %d\n", drm_ctx->planes[i].alpha);
		printf("  Pixel Blend Mode: %d\n", drm_ctx->planes[i].pixel_blend_mode);
		printf("  Rotation: %d\n", drm_ctx->planes[i].rotation);
		printf("  Color Encoding: %d\n", drm_ctx->planes[i].color_encoding);
		printf("  Color Range: %d\n", drm_ctx->planes[i].color_range);
		printf("------------------------------------------------------\n");
	}

	drm_ctx->max_buffers = drm_ctx->plane_count * DRM_ION_MAX_BUFFERS;
	drm_ctx->buffer_map = NULL;
	drm_ctx->buffer_count = 0;
}

int32_t vp_display_check_hdmi_is_connected()
{
	int drm_fd = drmOpen("vs-drm", NULL);
	if (drm_fd < 0) {
		SC_LOGE("drmOpen failed");
		return 0;
	}

	drmModeConnectorPtr connector = find_connector(drm_fd);
	if (connector == NULL) {
		close(drm_fd);
		return 0;
	}

	close(drm_fd);
	return 1;
}

int32_t vp_display_init(vp_drm_context_t *drm_ctx, int32_t width, int32_t height, int32_t target_refresh_hz)
{
	int32_t ret = 0;
	drmModeConnectorPtr connector = NULL;

	drm_init_config(drm_ctx, width, height, target_refresh_hz);

	drm_ctx->drm_fd = drmOpen("vs-drm", NULL);
	if (drm_ctx->drm_fd < 0) {
		perror("drmOpen failed");
		return -1;
	}

	connector = find_connector(drm_ctx->drm_fd);
	if (connector == NULL) {
		perror("find_connector failed");
		return -1;
	}
	drm_ctx->connector_id = connector->connector_id;

	printf("Setting DRM client capabilities...\n");
	drm_set_client_capabilities(drm_ctx->drm_fd);

	printf("Setting up KMS...\n");
	ret = drm_setup_kms(drm_ctx);
	if (ret < 0) {
		printf("drm_setup_kms failed\n");
		close(drm_ctx->drm_fd);
		return -1;
	}
	return ret;
}

int32_t vp_display_deinit(vp_drm_context_t *drm_ctx)
{
	int32_t ret = 0;
	dma_buf_map_t *current, *tmp;

	drm_ctx->flip_pending = 0;

	// 释放 buffer_map 中的所有条目
	HASH_ITER(hh, drm_ctx->buffer_map, current, tmp)
	{
		if (current->fb_id) {
			if (drmModeRmFB(drm_ctx->drm_fd, current->fb_id) < 0) {
				perror("drmModeRmFB");
			}
		}
		HASH_DEL(drm_ctx->buffer_map, current);
		free(current);
	}

	drmModeSetCrtc(drm_ctx->drm_fd, drm_ctx->crtc_id, 0, 0, 0, NULL, 0, NULL);

	drmModeRes *resources = drmModeGetResources(drm_ctx->drm_fd);
	if (!resources) {
		perror("drmModeGetResources");
		return -1;
	}

	for (int i = 0; i < resources->count_crtcs; i++) {
		drmModeFreeCrtc(drmModeGetCrtc(drm_ctx->drm_fd, resources->crtcs[i]));
	}

	for (int i = 0; i < resources->count_connectors; i++) {
		drmModeFreeConnector(drmModeGetConnector(drm_ctx->drm_fd, resources->connectors[i]));
	}

	for (int i = 0; i < resources->count_encoders; i++) {
		drmModeFreeEncoder(drmModeGetEncoder(drm_ctx->drm_fd, resources->encoders[i]));
	}

	drmModePlaneRes *plane_resources = drmModeGetPlaneResources(drm_ctx->drm_fd);
	if (plane_resources) {
		for (uint32_t i = 0; i < plane_resources->count_planes; i++) {
			drmModeFreePlane(drmModeGetPlane(drm_ctx->drm_fd, plane_resources->planes[i]));
		}
		drmModeFreePlaneResources(plane_resources);
	}

	drmModeFreeResources(resources);

	if (drm_ctx->drm_fd >= 0) {
		close(drm_ctx->drm_fd);
		drm_ctx->drm_fd = -1;
	}

	printf("\r\nDRM resources cleaned up.\n");

	return ret;
}

static uint32_t get_format_from_string(const char *format_str)
{
	if (strcmp(format_str, "AR12") == 0) {
		return DRM_FORMAT_ARGB4444;
	} else if (strcmp(format_str, "AR15") == 0) {
		return DRM_FORMAT_ARGB1555;
	} else if (strcmp(format_str, "RG16") == 0) {
		return DRM_FORMAT_RGB565;
	} else if (strcmp(format_str, "AR24") == 0) {
		return DRM_FORMAT_ARGB8888;
	} else if (strcmp(format_str, "RA12") == 0) {
		return DRM_FORMAT_RGBA4444;
	} else if (strcmp(format_str, "RA15") == 0) {
		return DRM_FORMAT_RGBA5551;
	} else if (strcmp(format_str, "RA24") == 0) {
		return DRM_FORMAT_RGBA8888;
	} else if (strcmp(format_str, "AB12") == 0) {
		return DRM_FORMAT_ABGR4444;
	} else if (strcmp(format_str, "AB15") == 0) {
		return DRM_FORMAT_ABGR1555;
	} else if (strcmp(format_str, "BG16") == 0) {
		return DRM_FORMAT_BGR565;
	} else if (strcmp(format_str, "BG24") == 0) {
		return DRM_FORMAT_BGR888;
	} else if (strcmp(format_str, "AB24") == 0) {
		return DRM_FORMAT_ABGR8888;
	} else if (strcmp(format_str, "BA12") == 0) {
		return DRM_FORMAT_BGRA4444;
	} else if (strcmp(format_str, "BA15") == 0) {
		return DRM_FORMAT_BGRA5551;
	} else if (strcmp(format_str, "BA24") == 0) {
		return DRM_FORMAT_BGRA8888;
	} else if (strcmp(format_str, "YUYV") == 0) {
		return DRM_FORMAT_YUYV;
	} else if (strcmp(format_str, "YVYU") == 0) {
		return DRM_FORMAT_YVYU;
	} else if (strcmp(format_str, "NV12") == 0) {
		return DRM_FORMAT_NV12;
	} else if (strcmp(format_str, "NV21") == 0) {
		return DRM_FORMAT_NV21;
	} else {
		return 0;  // Unsupported format
	}
}

static uint32_t get_framebuffer(vp_drm_context_t *drm_ctx, int dma_buf_fd, int plane_index)
{
	dma_buf_map_t *entry = NULL;
	HASH_FIND_INT(drm_ctx->buffer_map, &dma_buf_fd, entry);
	if (entry) {
		return entry->fb_id;
	}

	if (drm_ctx->buffer_count >= drm_ctx->max_buffers) {
		printf("Buffer map is full, unable to add new framebuffer\n");
		return 0;
	}

	struct drm_prime_handle prime_handle = {
	    .fd = dma_buf_fd,
	    .flags = 0,
	    .handle = 0,
	};

	if (drmIoctl(drm_ctx->drm_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime_handle) < 0) {
		perror("DRM_IOCTL_PRIME_FD_TO_HANDLE");
		printf("Failed to map dma_buf_fd=%d to GEM handle\n", dma_buf_fd);
		return 0;
	}

	uint32_t handles[4] = {0};
	uint32_t strides[4] = {0};
	uint32_t offsets[4] = {0};
	handles[0] = prime_handle.handle;
	strides[0] = drm_ctx->planes[plane_index].src_w;
	offsets[0] = 0;
	handles[1] = prime_handle.handle;
	strides[1] = drm_ctx->planes[plane_index].src_w;
	offsets[1] = drm_ctx->planes[plane_index].src_w * drm_ctx->planes[plane_index].src_h;

	uint32_t fb_id;
	uint32_t drm_format = get_format_from_string(drm_ctx->planes[plane_index].format);

	if (drmModeAddFB2(drm_ctx->drm_fd, drm_ctx->planes[plane_index].src_w, drm_ctx->planes[plane_index].src_h,
			  drm_format, handles, strides, offsets, &fb_id, 0)) {
		perror("drmModeAddFB2");
		return 0;
	}

	printf("Created new framebuffer: fb_id=%u for dma_buf_fd=%d\n", fb_id, dma_buf_fd);

	entry = (dma_buf_map_t *)malloc(sizeof(dma_buf_map_t));
	if (!entry) {
		perror("malloc");
		return 0;
	}

	entry->dma_buf_fd = dma_buf_fd;
	entry->fb_id = fb_id;
	HASH_ADD_INT(drm_ctx->buffer_map, dma_buf_fd, entry);
	drm_ctx->buffer_count++;

	return fb_id;
}

/*
 * 通知 dma-buf：生产者写入结束（cache/一致性）。不 poll：部分驱动上 poll 可达 100ms，
 * 易与帧节拍错位；ioctl 失败可忽略。
 */
static void vp_drm_prepare_dma_buf_for_scanout(int dma_buf_fd)
{
	if (dma_buf_fd < 0)
		return;

	struct vp_dma_buf_sync sync = {
	    .flags = VP_DMA_BUF_SYNC_END | VP_DMA_BUF_SYNC_WRITE,
	};
	(void)ioctl(dma_buf_fd, VP_DMA_BUF_IOCTL_SYNC, &sync);
}

static int vp_drm_atomic_commit_submit(vp_drm_context_t *drm_ctx, drmModeAtomicReq *req)
{
	uint32_t flags = DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK;
	int ret = -1;
	int busy_retry = 0;

	while (busy_retry < 200) {
		ret = drmModeAtomicCommit(drm_ctx->drm_fd, req, flags, drm_ctx);
		if (ret == 0)
			break;
		if (errno != EBUSY)
			break;
		usleep(1000);
		busy_retry++;
	}
	if (ret != 0) {
		flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
		ret = drmModeAtomicCommit(drm_ctx->drm_fd, req, flags, NULL);
		drm_ctx->flip_pending = 0;
		return ret;
	}

	drm_ctx->flip_pending = 1;
	return 0;
}

int32_t vp_display_set_frame(vp_drm_context_t *drm_ctx, hbn_vnode_image_t *image_frame)
{
	int32_t ret = 0;
	int dma_buf_fds[DRM_MAX_PLANES] = {-1, -1, -1};

	if (!drm_ctx || !image_frame)
		return -1;

	if (drm_ctx->flip_pending)
		return -1;

	for (uint32_t i = 0; i < drm_ctx->plane_count; ++i) {
		dma_buf_fds[i] = image_frame->buffer.fd[i];
		vp_drm_prepare_dma_buf_for_scanout(dma_buf_fds[i]);
	}

	drmModeAtomicReq *req = drmModeAtomicAlloc();
	if (!req) {
		perror("drmModeAtomicAlloc");
		return -1;
	}

	for (int i = 0; i < drm_ctx->plane_count; i++) {
		if (dma_buf_fds[i] == -1) {
			continue;
		}

		uint32_t fb_id = get_framebuffer(drm_ctx, dma_buf_fds[i], i);
		if (fb_id == 0) {
			fprintf(stderr, "vp_display_set_frame: failed to get framebuffer for plane %d\n", i);
			drmModeAtomicFree(req);
			return -1;
		}

		add_property(drm_ctx->drm_fd, req, drm_ctx->planes[i].plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID",
			     drm_ctx->crtc_id);
		add_property(drm_ctx->drm_fd, req, drm_ctx->planes[i].plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID", fb_id);
	}

	ret = vp_drm_atomic_commit_submit(drm_ctx, req);
	if (ret < 0) {
		drmModeAtomicFree(req);
		return -1;
	}

	drmModeAtomicFree(req);
	return ret;
}

int vp_display_get_connector_info(void *handle, get_connector_info_cb_t cb)
{
	int drm_fd = drmOpen("vs-drm", NULL);
	if (drm_fd < 0) {
		perror("drmOpen failed");
		return -1;
	}

	drmModeConnectorPtr connector = find_connector(drm_fd);
	if (!connector) {
		printf("[Display] not found connector\n");
		close(drm_fd);
		return -2;
	}

	for (int i = 0; i < connector->count_modes; i++) {
		drmModeModeInfo *mode = &connector->modes[i];
		if (cb != NULL) {
			int is_interleave = IS_MODE_INTERLACED(mode) ? 1 : 0;
			cb(handle, mode->hdisplay, mode->vdisplay, __mode_vrefresh(mode), is_interleave);
		}
	}

	drmModeFreeConnector(connector);
	close(drm_fd);
	return 0;
}

int vp_display_hotplug_init(void)
{
	g_hp_udev = udev_new();
	if (!g_hp_udev)
		return -1;

	g_hp_mon = udev_monitor_new_from_netlink(g_hp_udev, "udev");
	if (!g_hp_mon) {
		udev_unref(g_hp_udev);
		g_hp_udev = NULL;
		return -1;
	}

	udev_monitor_filter_add_match_subsystem_devtype(g_hp_mon, "drm", NULL);
	udev_monitor_enable_receiving(g_hp_mon);
	g_hp_fd = udev_monitor_get_fd(g_hp_mon);

	g_hdmi_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	if (g_hdmi_timer_fd >= 0) {
		struct itimerspec its;

		memset(&its, 0, sizeof(its));
		its.it_value.tv_sec = VP_DISPLAY_HDMI_POLL_INTERVAL_SEC;
		its.it_interval.tv_sec = VP_DISPLAY_HDMI_POLL_INTERVAL_SEC;
		if (timerfd_settime(g_hdmi_timer_fd, 0, &its, NULL) < 0) {
			perror("timerfd_settime");
			close(g_hdmi_timer_fd);
			g_hdmi_timer_fd = -1;
		}
	} else {
		perror("timerfd_create");
	}

	return g_hp_fd;
}

void vp_display_hotplug_deinit(void)
{
	if (g_hdmi_timer_fd >= 0) {
		close(g_hdmi_timer_fd);
		g_hdmi_timer_fd = -1;
	}
	if (g_hp_mon) {
		udev_monitor_unref(g_hp_mon);
		g_hp_mon = NULL;
	}
	if (g_hp_udev) {
		udev_unref(g_hp_udev);
		g_hp_udev = NULL;
	}
	g_hp_fd = -1;
}

int vp_display_hotplug_process(void)
{
	if (!g_hp_mon)
		return -1;

	struct udev_device *dev = udev_monitor_receive_device(g_hp_mon);
	if (!dev)
		return -1;

	const char *hp = udev_device_get_property_value(dev, "HOTPLUG");
	int result = -1;
	if (hp && strcmp(hp, "1") == 0)
		result = vp_display_check_hdmi_is_connected();

	udev_device_unref(dev);

	return result;
}

int vp_display_poll_events(vp_drm_context_t *drm_ctx, int display_ready, int prev_connected, int timeout_ms)
{
	struct pollfd pfds[3];
	int nfds = 0;
	int ix_drm = -1, ix_udev = -1, ix_timer = -1;

	if (display_ready && drm_ctx && drm_ctx->drm_fd >= 0) {
		ix_drm = nfds;
		pfds[nfds].fd = drm_ctx->drm_fd;
		pfds[nfds].events = POLLIN;
		nfds++;
	}
	if (g_hp_fd >= 0) {
		ix_udev = nfds;
		pfds[nfds].fd = g_hp_fd;
		pfds[nfds].events = POLLIN;
		nfds++;
	}
	if (g_hdmi_timer_fd >= 0) {
		ix_timer = nfds;
		pfds[nfds].fd = g_hdmi_timer_fd;
		pfds[nfds].events = POLLIN;
		nfds++;
	}

	if (nfds > 0)
		poll(pfds, nfds, timeout_ms);
	else
		usleep(timeout_ms * 1000);

	int connected = prev_connected;

	if (ix_udev >= 0 && (pfds[ix_udev].revents & POLLIN)) {
		int hp = vp_display_hotplug_process();
		if (hp >= 0)
			connected = hp;
	}

	if (ix_drm >= 0 && (pfds[ix_drm].revents & POLLIN))
		vp_display_handle_drm_event(drm_ctx);

	/* timerfd：部分硬件无可靠 udev，每秒在 poll 内唤醒并读 HDMI 状态 */
	if (ix_timer >= 0 && (pfds[ix_timer].revents & POLLIN)) {
		uint64_t exp;

		(void)read(g_hdmi_timer_fd, &exp, sizeof(exp));
		connected = vp_display_check_hdmi_is_connected();
	}

	return connected;
}
