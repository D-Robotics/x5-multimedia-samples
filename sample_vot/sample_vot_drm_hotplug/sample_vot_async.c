/***************************************************************************
 *                      COPYRIGHT NOTICE
 *             Copyright(C) 2026, D-Robotics Co., Ltd.
 *                     All rights reserved.
 ***************************************************************************/

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libudev.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

// for hbm
#include "common_utils.h"
#include "hb_mem_mgr.h"

// for display
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "connector_id.h"
#include "drm_hotplug.h"

#define DRM_MAX_PLANES 3
#define RESOURCE_FILE_WIDTH 1920
#define RESOURCE_FILE_HEIGHT 1080
#define RESOURCE_FILE_PATH "../resource/nv12_1920x1080.yuv"

#define RGB_DEF_WIDTH 1920
#define RGB_DEF_HEIGHT 1080
#define RGB_MAX_WIDTH 2560
#define RGB_MAX_HEIGHT 1440
#define RGB_DEF_FPS 60

/* 默认 DRM 资源 ID（根据平台配置） */
#define DEFAULT_CRTC_ID 31
#define DEFAULT_PLANE_ID 33

/* 轮询和切换的时间参数（毫秒） */
#define POLL_TIMEOUT_MS 200
#define COLOR_SWITCH_INTERVAL_MS 2000

typedef struct param_config_s {
	int width;   // connector's width
	int height;  // connector's height

	char *output;  // hdmi, dsi
	int input;     // 0: memory buffer 1:file

	int list_connector_rosulotions;
} param_config_t;

typedef struct display_context_s {
	int width;
	int height;

	int drm_fd;

	uint32_t crtc_id;
	uint32_t plane_id;
	uint32_t connector_id;

	int connector_type;
	drmModeAtomicReq *req;

	const char *input_file;
	const int input_file_width;
	const int input_file_height;

	param_config_t param_config;
} display_context_t;

typedef struct drm_frame_buffer_info_s {
	int dump_handle;

	int frame_buffer_id;
	int frame_buffer_size;
	void *frame_buffer_vaddr;
} drm_frame_buffer_info_t;

static int display_setup(display_context_t *display_context);
static int __add_property(int drm_fd, drmModeAtomicReq *req, uint32_t obj_id, uint32_t obj_type, const char *name,
			  uint64_t value);

static int __set_plane_common_properties(display_context_t *display_context, drmModeAtomicReq *req, uint32_t fb_id,
					 int has_rotation, uint64_t rotation)
{
	int ret = 0;

	ret = __add_property(display_context->drm_fd, req, display_context->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID",
			     display_context->crtc_id);
	ret |= __add_property(display_context->drm_fd, req, display_context->plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID",
			      fb_id);

	if (has_rotation) {
		ret |= __add_property(display_context->drm_fd, req, display_context->plane_id, DRM_MODE_OBJECT_PLANE,
				      "rotation", rotation);
	}

	ret |= __add_property(display_context->drm_fd, req, display_context->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X",
			      0);
	ret |= __add_property(display_context->drm_fd, req, display_context->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y",
			      0);
	ret |= __add_property(display_context->drm_fd, req, display_context->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W",
			      display_context->width << 16);
	ret |= __add_property(display_context->drm_fd, req, display_context->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H",
			      display_context->height << 16);
	ret |= __add_property(display_context->drm_fd, req, display_context->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X",
			      0);
	ret |= __add_property(display_context->drm_fd, req, display_context->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y",
			      0);
	ret |= __add_property(display_context->drm_fd, req, display_context->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W",
			      display_context->width);
	ret |= __add_property(display_context->drm_fd, req, display_context->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H",
			      display_context->height);

	return ret;
}

static void page_flip_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, void *data)
{
	(void)fd;
	(void)data;
	printf("page flip event: frame=%u time=%u.%06u\n", frame, sec, usec);
}

static int setup_drm_event_context(drmEventContext *evctx)
{
	if (!evctx)
		return -1;

	memset(evctx, 0, sizeof(*evctx));
	evctx->version = DRM_EVENT_CONTEXT_VERSION;
	evctx->page_flip_handler = page_flip_handler;

	return 0;
}

static void print_help(char *test_case)
{
	printf("Usage: %s [OPTIONS]\n", test_case);
	printf("Options:\n");
	printf("  -o <output>                       Specify display connector: hdmi, dsi, (hdmi is default)\n");
	printf("  -c <image_width>                  Specify display width(column)\n");
	printf("  -r <image_height>                 Specify display height(row)\n");
	printf("  -l <list_connector_rosulotions>   Specify function is only list display's resulotions\n");
	printf("  -f <file>                         Specify input data is file (default: memory data(R,G,B)\n");
	printf("  -h <help>              Show this help message\n");
	printf("     For Example, list display's resulotions: ./%s -l\n", test_case);
	printf("     For Example, use file as input         : ./%s -f\n", test_case);
	printf("     For Example, use memory data as input  : ./%s \n", test_case);
	printf("     For Example, specify dsi as output     : ./%s -o dsi\n", test_case);
}

static int parser_params(int argc, char **argv, param_config_t *param)
{
	struct option const long_options[] = {

	    {"output", optional_argument, NULL, 'o'},
	    {"width", optional_argument, NULL, 'c'},
	    {"height", optional_argument, NULL, 'r'},
	    {"list_connector_rosulotions", optional_argument, NULL, 'l'},
	    {"file", optional_argument, NULL, 'f'},
	    {"help", no_argument, 0, 'h'},
	    {NULL, 0, NULL, 0}};

	int opt_index = 0;
	int c = 0;

	param->width = -1;
	param->height = -1;
	param->output = "hdmi";
	param->input = 0;
	param->list_connector_rosulotions = 0;

	while ((c = getopt_long(argc, argv, "o:c:r:lfh", long_options, &opt_index)) != -1) {
		switch (c) {
			case 'o':
				param->output = optarg;
				break;
			case 'c':
				param->width = atoi(optarg);
				break;
			case 'r':
				param->height = atoi(optarg);
				break;
			case 'f':
				param->input = 1;
				break;
			case 'l':
				param->list_connector_rosulotions = 1;
				break;
			case 'h':
			default:
				print_help("sample_vot");
				return -1;
		}
	}
	if ((strcmp(param->output, "hdmi") != 0) && (strcmp(param->output, "dsi") != 0)) {
		printf("display connector current only support hdmi and dsi, but input param is [%s].\n",
		       param->output);
		return -1;
	}

	if (param->input == 1) {
		if ((param->width != -1) || ((param->height != -1))) {
			printf("\n !!! file mode force display resolution is %d*%d (input file's resolution).\n",
			       RESOURCE_FILE_WIDTH, RESOURCE_FILE_HEIGHT);
		}
		param->width = RESOURCE_FILE_WIDTH;
		param->height = RESOURCE_FILE_HEIGHT;
	}
	printf("\nPrint param Config:\n");
	if (param->list_connector_rosulotions == 1) {
		printf("\tFunction : print connector support resolution.\n");
	} else {
		printf("\tFunction : display.\n");
		if (param->input == 1) {
			printf("\tInput    : file(%s).\n", RESOURCE_FILE_PATH);
		} else {
			printf("\tInput    : default data (R, G, B).\n");
		}
		printf("\tOutput   :\n");
		if ((param->width == -1) || ((param->height == -1))) {
			printf("\t\t Resolution: select connector's first config from EDID\n");
		} else {
			printf("\t\t Resolution: %d*%d\n", param->width, param->height);
		}
		printf("\t\t Connector: %s\n", param->output);
	}

	return 0;
}

static void printf_insmod_driver_cmd(void)
{
	perror("drmOpen failed, maybe display driver is not loaded, please execute the following command:");
	printf("\tmodprobe panel-jc-050hd134\n");
	printf("\tmodprobe galcore\n");
	printf("\tmodprobe vio_n2d\n");
	printf("\tmodprobe lontium_lt8618\n");
	printf("\tmodprobe vs-x5-syscon-bridge\n");
	printf("\tmodprobe vs_drm\n");
	printf("\n\n");
}

static uint32_t get_bpp_from_format(uint32_t format)
{
	switch (format) {
		case DRM_FORMAT_ARGB4444:
		case DRM_FORMAT_XRGB4444:
		case DRM_FORMAT_ABGR4444:
		case DRM_FORMAT_XBGR4444:
			return 16;  // 16 bits per pixel (4 bits per channel)
		case DRM_FORMAT_RGB565:
		case DRM_FORMAT_BGR565:
			return 16;  // 16 bits per pixel (5, 6, 5 bits per channel)
		case DRM_FORMAT_ARGB1555:
		case DRM_FORMAT_XRGB1555:
		case DRM_FORMAT_ABGR1555:
		case DRM_FORMAT_XBGR1555:
			return 16;  // 16 bits per pixel (1 bit alpha, 5 bits per RGB)
		case DRM_FORMAT_ARGB8888:
		case DRM_FORMAT_XRGB8888:
		case DRM_FORMAT_ABGR8888:
		case DRM_FORMAT_XBGR8888:
			return 32;  // 32 bits per pixel (8 bits per channel)
		case DRM_FORMAT_RGB888:
		case DRM_FORMAT_BGR888:
			return 24;  // 24 bits per pixel (8 bits per channel, no alpha)
		case DRM_FORMAT_YUYV:
		case DRM_FORMAT_YVYU:
			return 16;  // 16 bits per pixel for YUV 4:2:2
		case DRM_FORMAT_NV12:
		case DRM_FORMAT_NV21:
			return 12;  // 12 bits per pixel for YUV 4:2:0
		default:
			return 0;  // Unsupported format
	}
}

static int __add_property(int drm_fd, drmModeAtomicReq *req, uint32_t obj_id, uint32_t obj_type, const char *name,
			  uint64_t value)
{
	drmModeObjectProperties *props = drmModeObjectGetProperties(drm_fd, obj_id, obj_type);
	if (!props) {
		fprintf(stderr, "Failed to get properties for object %u\n", obj_id);
		return -1;
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
		return -1;
	}

	if (drmModeAtomicAddProperty(req, obj_id, prop_id, value) < 0) {
		fprintf(stderr, "Failed to add property '%s' on object %u: %s\n", name, obj_id, strerror(errno));
		return -1;
	}

	return 0;
}
static drmModeModeInfo *__get_valid_mode_from_connector(drmModeConnector *conn, int width, int height)
{
	drmModeModeInfo *mode = NULL;

	if (conn->connection != DRM_MODE_CONNECTED) {
		printf("display connector type %d is not connected.\n", conn->connector_type);
		return mode;
	} else if (conn->count_modes <= 0) {
		printf("display connector connector not found mode info.\n");
		return mode;
	} else {
		if ((width == -1) || (height == -1)) {
			width = RGB_DEF_WIDTH;
			height = RGB_DEF_HEIGHT;
		}

		if (width > RGB_MAX_WIDTH || height > RGB_MAX_HEIGHT) {
			width = RGB_MAX_WIDTH;
			height = RGB_MAX_HEIGHT;
		}

		for (int i = 0; i < conn->count_modes; i++) {
			printf("hdmi index: %02d ch:%d cv:%d vrefresh:%d\n", i, conn->modes[i].hdisplay,
			       conn->modes[i].vdisplay, conn->modes[i].vrefresh);

			if ((conn->modes[i].hdisplay == width) && (conn->modes[i].vdisplay == height)
			    && (conn->modes[i].vrefresh == RGB_DEF_FPS)) {
				mode = &conn->modes[i];
				printf("display connector connector found mode info.\n");
				break;
			}
		}

		if (mode == NULL) {
			printf("display connector not support resolution: %d*%d.\n", width, height);
			return mode;
		}
	}

	return mode;
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

static int list_connector_support_resolution(display_context_t *display_context)
{
	int ret = 0;
	param_config_t *param_config = &display_context->param_config;

	drmModeRes *resources = drmModeGetResources(display_context->drm_fd);
	if (!resources) {
		printf("drmModeGetResources failed.\n");
		return -1;
	}

	drmModeConnector *conn = NULL;
	for (int i = 0; i < resources->count_connectors; i++) {
		conn = drmModeGetConnector(display_context->drm_fd, resources->connectors[i]);
		if (conn != NULL) {
			if (conn->connector_type == display_context->connector_type) {
				break;
			} else {
				drmModeFreeConnector(conn);
				conn = NULL;  // Reset conn to NULL if it's not what we're looking for
			}
		}
	}
	if (conn == NULL) {
		printf("not support %s.\n", param_config->output);
		goto free_res;
	}

	if (conn->connection != DRM_MODE_CONNECTED) {
		ret = -2;
		printf("display connector %d is not connected.\n", conn->connector_type);
		goto free_conn;
	} else if (conn->count_modes <= 0) {
		ret = -3;
		printf("display connector not found mode info.\n");
		goto free_conn;
	} else {
		drmModeModeInfo *mode = NULL;
		printf("Print [%s] connector support resolution:\n", param_config->output);
		for (int i = 0; i < conn->count_modes; i++) {
			mode = &conn->modes[i];
			printf("  [%d] %s %.2ffps\n", i, mode->name, __mode_vrefresh(mode));
		}
	}

free_conn:
	drmModeFreeConnector(conn);
free_res:
	drmModeFreeResources(resources);
	return ret;
}
int __wraper_dma_buffer_to_drm_frame_buffer(int drm_fd, uint32_t format, uint32_t width, uint32_t height,
					    int dma_buf_fd, drm_frame_buffer_info_t *fb_info)
{
	if (drm_fd < 0 || fb_info == NULL) {
		fprintf(stderr, "__wraper_dma_buffer_to_drm_frame_buffer: invalid parameter, drm_fd=%d, fb_info=%p\n",
			drm_fd, (void *)fb_info);
		return -1;
	}

	if (DRM_FORMAT_NV12 != format) {
		printf("current only support nv12, but input format param is %d.\n", format);
		return -1;
	}
	struct drm_prime_handle prime_handle = {
	    .fd = dma_buf_fd,
	    .flags = 0,
	    .handle = 0,
	};
	if (drmIoctl(drm_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime_handle) < 0) {
		perror("DRM_IOCTL_PRIME_FD_TO_HANDLE");
		return -1;
	}

	uint32_t handles[4] = {0};
	uint32_t strides[4] = {0};
	uint32_t offsets[4] = {0};
	handles[0] = prime_handle.handle;
	strides[0] = width;
	offsets[0] = 0;

	handles[1] = prime_handle.handle;
	strides[1] = width;
	offsets[1] = width * height;

	uint32_t buf_id;
	if (drmModeAddFB2(drm_fd, width, height, format, handles, strides, offsets, &buf_id, 0)) {
		perror("drmModeAddFB2");
		/* drmModeAddFB2 失败时需要关闭 prime_handle 以避免 DRM 资源泄漏 */
		struct drm_gem_close gem_close = {
		    .handle = prime_handle.handle,
		};
		if (drmIoctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &gem_close) < 0) {
			perror("DRM_IOCTL_GEM_CLOSE");
		}
		return -1;
	}
	fb_info->dump_handle = prime_handle.handle;
	fb_info->frame_buffer_id = buf_id;
	fb_info->frame_buffer_vaddr = NULL;
	fb_info->frame_buffer_size = -1;

	return 0;
}
void __unwraper_dma_buffer_to_drm_frame_buffer(int drm_fd, drm_frame_buffer_info_t *fb_info)
{
	drmModeRmFB(drm_fd, fb_info->frame_buffer_id);
}

int __create_and_mmap_drm_frame_buffer(int drm_fd, uint32_t format, uint32_t width, uint32_t height,
				       drm_frame_buffer_info_t *fb_info)
{
	if (drm_fd < 0 || fb_info == NULL) {
		fprintf(stderr, "__create_and_mmap_drm_frame_buffer: invalid parameter, drm_fd=%d, fb_info=%p\n",
			drm_fd, (void *)fb_info);
		return -1;
	}

	// 创建 dumb buffer
	struct drm_mode_create_dumb create_dumb = {0};
	create_dumb.width = width;
	create_dumb.height = height;
	create_dumb.bpp = get_bpp_from_format(format);

	if (drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_dumb) < 0) {
		perror("DRM_IOCTL_MODE_CREATE_DUMB");
		return -1;
	}

	uint32_t buf_id;
	uint32_t handles[4] = {0};
	uint32_t pitches[4] = {0};
	uint32_t offsets[4] = {0};
	handles[0] = create_dumb.handle;
	pitches[0] = create_dumb.pitch;
	offsets[0] = 0;
	if (drmModeAddFB2(drm_fd, width, height, format, handles, pitches, offsets, &buf_id, 0)) {
		perror("drmModeAddFB2");
		goto err_destroy_dumb;
	}

	// 映射 dumb buffer
	struct drm_mode_map_dumb map_dumb = {0};
	map_dumb.handle = create_dumb.handle;
	if (drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map_dumb) < 0) {
		perror("DRM_IOCTL_MODE_MAP_DUMB");
		goto err_rmfb_destroy_dumb;
	}

	void *map = mmap(0, create_dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, map_dumb.offset);
	if (map == MAP_FAILED) {
		perror("mmap");
		goto err_rmfb_destroy_dumb;
	}

	fb_info->dump_handle = create_dumb.handle;
	fb_info->frame_buffer_id = buf_id;
	fb_info->frame_buffer_vaddr = map;
	fb_info->frame_buffer_size = create_dumb.size;
	return 0;

err_rmfb_destroy_dumb:
	drmModeRmFB(drm_fd, buf_id);
err_destroy_dumb:
{
	struct drm_mode_destroy_dumb destroy = {0};
	destroy.handle = create_dumb.handle;
	if (drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy) < 0) {
		perror("DRM_IOCTL_MODE_DESTROY_DUMB");
	}
	return -1;
}
}
void __destroy_and_unmmap_drm_frame_buffer(int drm_fd, drm_frame_buffer_info_t *fb_info)
{
	drmModeRmFB(drm_fd, fb_info->frame_buffer_id);

	munmap(fb_info->frame_buffer_vaddr, fb_info->frame_buffer_size);

	struct drm_mode_destroy_dumb destroy = {0};
	destroy.handle = fb_info->dump_handle;
	drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
}

static int display_setup(display_context_t *display_context)
{
	int ret = -1;
	int connector_id = display_context->connector_id;
	int crtc_id = display_context->crtc_id;
	int drm_fd = display_context->drm_fd;
	param_config_t *param_config = &display_context->param_config;

	drmModeRes *resources = drmModeGetResources(drm_fd);
	if (!resources) {
		perror("drmModeGetResources");
		return -1;
	}

	drmModeConnector *conn = NULL;
	for (int i = 0; i < resources->count_connectors; i++) {
		conn = drmModeGetConnector(drm_fd, resources->connectors[i]);
		if (conn != NULL) {
			if (conn->connector_type == display_context->connector_type) {
				break;
			} else {
				drmModeFreeConnector(conn);
				conn = NULL;  // Reset conn to NULL if it's not what we're looking for
			}
		}
	}
	if (conn == NULL) {
		printf("not support %s, connector type is %d.\n", param_config->output,
		       display_context->connector_type);
		goto free_res;
	}

	drmModeModeInfo *mode = NULL;
	if (display_context->connector_type == DRM_MODE_CONNECTOR_DSI) {
		mode = &conn->modes[0];
	} else {  // hdmi
		mode = __get_valid_mode_from_connector(conn, param_config->width, param_config->height);
	}

	if (mode == NULL) {
		fprintf(stderr, "display_setup: no valid mode for connector type %d (output=%s)\n",
			display_context->connector_type, param_config->output);
		goto free_conn;
	}

	display_context->width = mode->hdisplay;
	display_context->height = mode->vdisplay;

	printf("display select resolution :%d*%d .\n", display_context->width, display_context->height);
	if (drmSetClientCap(drm_fd, DRM_CLIENT_CAP_ATOMIC, 1) < 0) {
		perror("drmSetClientCap DRM_CLIENT_CAP_ATOMIC");
		goto free_conn;
	}

	if (drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) < 0) {
		perror("drmSetClientCap DRM_CLIENT_CAP_UNIVERSAL_PLANES");
		goto free_conn;
	}

	drmModeCrtc *crtc = drmModeGetCrtc(drm_fd, crtc_id);
	if (!crtc) {
		perror("drmModeGetCrtc");
		goto free_conn;
	}
	uint32_t blob_id = 0;
	if (drmModeCreatePropertyBlob(drm_fd, mode, sizeof(*mode), &blob_id) < 0) {
		perror("drmModeCreatePropertyBlob");
		goto free_crtc;
	}

	drmModeAtomicReq *req = drmModeAtomicAlloc();
	if (!req) {
		perror("drmModeAtomicAlloc");
		goto free_crtc;
	}

	ret = __add_property(drm_fd, req, crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", 1);
	ret |= __add_property(drm_fd, req, crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID", blob_id);
	ret |= __add_property(drm_fd, req, connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", crtc_id);

	if (ret != 0) {
		goto free_req;
	}

	uint32_t flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
	if (drmModeAtomicCommit(drm_fd, req, flags, NULL) < 0) {
		perror("drmModeAtomicCommit");
		goto free_req;
	}
	ret = 0;

free_req:
	drmModeAtomicFree(req);
	if (blob_id != 0) {
		if (drmModeDestroyPropertyBlob(drm_fd, blob_id) != 0) {
			perror("drmModeDestroyPropertyBlob");
		}
	}
free_crtc:
	drmModeFreeCrtc(crtc);
free_conn:
	drmModeFreeConnector(conn);
free_res:
	drmModeFreeResources(resources);
	return ret;
}
int main(int argc, char **argv)
{
	display_context_t display_context = {
	    .input_file = RESOURCE_FILE_PATH,
	    .input_file_width = RESOURCE_FILE_WIDTH,
	    .input_file_height = RESOURCE_FILE_HEIGHT,
	};

	param_config_t *param_config = &display_context.param_config;

	int ret = parser_params(argc, argv, param_config);
	if (ret != 0) {
		return -1;
	}

	if (strcmp(param_config->output, "hdmi") == 0) {
		display_context.connector_type = DRM_MODE_CONNECTOR_HDMIA;
	} else if (strcmp(param_config->output, "dsi") == 0) {
		display_context.connector_type = DRM_MODE_CONNECTOR_DSI;
	} else {
		printf("not support display [%s]\n.", param_config->output);
		return -1;
	}

	display_context.connector_id = get_connector_id(display_context.connector_type);

	display_context.crtc_id = DEFAULT_CRTC_ID;
	display_context.plane_id = DEFAULT_PLANE_ID;

	display_context.drm_fd = drmOpen("vs-drm", NULL);
	if (display_context.drm_fd < 0) {
		printf_insmod_driver_cmd();
		return -1;
	}

	if (param_config->list_connector_rosulotions == 1) {
		list_connector_support_resolution(&display_context);
		goto close_drm;
	}

	int display_ready = 0;
	ret = display_setup(&display_context);
	if (ret == 0) {
		display_ready = 1;
	} else {
		printf("display_setup not ready, will wait for connector hotplug in main loop.\n");
	}

	/* udev / drm event context 初始化，只做一次 */
	struct udev *udev = NULL;
	struct udev_monitor *mon = NULL;
	int udev_fd = setup_udev_drm_monitor(&udev, &mon);
	if (udev_fd < 0) {
		printf("setup_udev_drm_monitor failed, hotplug events will not be handled.\n");
		udev = NULL;
		mon = NULL;
	}

	drmEventContext evctx;
	if (setup_drm_event_context(&evctx) != 0) {
		printf("setup_drm_event_context failed, flip events will not be handled.\n");
	}

	int drm_fd = display_context.drm_fd;

	if (param_config->input == 0) {
		uint32_t colors[3] = {0X00FF0000 /*red*/, 0X0000FF00 /*green*/, 0X000000FF /*blue*/};
		drm_frame_buffer_info_t drm_fb_info[3];
		int fb_initialized = 0;

		int current_index = 0;
		int pending_flip = 0;
		struct timeval last_switch = {0};

		gettimeofday(&last_switch, NULL);

		while (1) {
			fd_set fds;
			struct timeval tv;
			int maxfd = -1;

			FD_ZERO(&fds);

			if (display_ready) {
				FD_SET(drm_fd, &fds);
				maxfd = drm_fd;
			}

			if (udev_fd >= 0) {
				FD_SET(udev_fd, &fds);
				if (udev_fd > maxfd)
					maxfd = udev_fd;
			}

			tv.tv_sec = POLL_TIMEOUT_MS / 1000;
			tv.tv_usec = (POLL_TIMEOUT_MS % 1000) * 1000;  // 轮询时间

			int sel = select(maxfd + 1, &fds, NULL, NULL, &tv);
			if (sel < 0) {
				if (errno == EINTR)
					continue;
				perror("select");
				break;
			}

			if (sel > 0) {
				if (display_ready && FD_ISSET(drm_fd, &fds)) {
					if (evctx.version != 0)
						drmHandleEvent(drm_fd, &evctx);
					pending_flip = 0;
				}

				if (udev_fd >= 0 && FD_ISSET(udev_fd, &fds)) {
					int hp = handle_udev_drm_hotplug(mon, display_context.drm_fd,
									 display_context.connector_id,
									 display_context.connector_type);

					if (hp == 0) { /* disconnected */
						display_ready = 0;
						pending_flip = 0;
						if (fb_initialized) {
							for (int i = 0; i < 3; i++) {
								__destroy_and_unmmap_drm_frame_buffer(display_context
													  .drm_fd,
												      &drm_fb_info[i]);
							}
							fb_initialized = 0;
						}
					} else if (hp == 1) { /* connected */
						if (!display_ready) {
							if (display_setup(&display_context) == 0) {
								printf("Connector is now connected, start display.\n");
								display_ready = 1;
							}
						}
					}
				}
			}

			/* 首次连上显示后，创建 framebuffer 并提交第一帧 */
			if (display_ready && !fb_initialized) {
				int fb_created = 0;
				for (int i = 0; i < 3; i++) {
					ret = __create_and_mmap_drm_frame_buffer(display_context.drm_fd,
										 DRM_FORMAT_ARGB8888,
										 display_context.width,
										 display_context.height,
										 &drm_fb_info[i]);
					if (ret != 0) {
						/* 清理已成功创建的 framebuffer，避免资源泄漏 */
						for (int j = 0; j < fb_created; j++) {
							__destroy_and_unmmap_drm_frame_buffer(display_context.drm_fd,
											      &drm_fb_info[j]);
						}
						goto close_drm;
					}
					fb_created++;
				}

				for (int i = 0; i < 3; i++) {
					uint32_t color = colors[i];
					uint32_t *buffer_vaddr = (uint32_t *)drm_fb_info[i].frame_buffer_vaddr;
					for (int j = 0; j < drm_fb_info[i].frame_buffer_size / 4; j++) {
						buffer_vaddr[j] = color;
					}
				}

#if 1  // atomic 版本的接口
				drmModeAtomicReq *req = drmModeAtomicAlloc();
				ret = __set_plane_common_properties(&display_context, req,
								    drm_fb_info[current_index].frame_buffer_id, 0, 0);
				if (ret != 0) {
					printf("__add_property failed\n");
					drmModeAtomicFree(req);
					goto close_drm;
				}
				uint32_t flags = DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT;
				ret = drmModeAtomicCommit(display_context.drm_fd, req, flags, NULL);
				if (ret != 0) {
					printf("drmModeAtomicCommit failed: ret=%d (errno=%d)\n", ret, errno);
					drmModeAtomicFree(req);
					goto close_drm;
				}
				drmModeAtomicFree(req);
#else  // legacy 版本的接口
				drmModeSetPlane(display_context.drm_fd, display_context.plane_id,
						display_context.crtc_id, drm_fb_info[current_index].frame_buffer_id, 0,
						0, 0, display_context.width, display_context.height, 0 << 16, 0 << 16,
						display_context.width << 16, display_context.height << 16);
#endif
				pending_flip = 1;
				fb_initialized = 1;
				gettimeofday(&last_switch, NULL);
			}

			/* 已经连上并初始化 framebuffer，按 2 秒切换颜色 */
			if (display_ready && fb_initialized) {
				struct timeval now;
				gettimeofday(&now, NULL);
				long diff_ms = (now.tv_sec - last_switch.tv_sec) * 1000L
					       + (now.tv_usec - last_switch.tv_usec) / 1000L;

				if (!pending_flip && diff_ms >= COLOR_SWITCH_INTERVAL_MS) {
					current_index = (current_index + 1) % 3;

#if 1  // atomic 版本的接口
					drmModeAtomicReq *req_loop = drmModeAtomicAlloc();
					ret = __set_plane_common_properties(&display_context, req_loop,
									    drm_fb_info[current_index].frame_buffer_id,
									    0, 0);
					if (ret != 0) {
						printf("__add_property failed\n");
						drmModeAtomicFree(req_loop);
						break;
					}
					uint32_t flags = DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT;
					ret = drmModeAtomicCommit(display_context.drm_fd, req_loop, flags, NULL);
					if (ret != 0) {
						printf("drmModeAtomicCommit failed: ret=%d (errno=%d)\n", ret, errno);
						drmModeAtomicFree(req_loop);
						break;
					}
					drmModeAtomicFree(req_loop);
#else  // legacy 版本的接口
					drmModeSetPlane(display_context.drm_fd, display_context.plane_id,
							display_context.crtc_id,
							drm_fb_info[current_index].frame_buffer_id, 0, 0, 0,
							display_context.width, display_context.height, 0 << 16, 0 << 16,
							display_context.width << 16, display_context.height << 16);
#endif
					pending_flip = 1;
					last_switch = now;
				}
			}
		}

		if (fb_initialized) {
			for (int i = 0; i < 3; i++) {
				__destroy_and_unmmap_drm_frame_buffer(display_context.drm_fd, &drm_fb_info[i]);
			}
		}

	} else {
		hb_mem_module_open();
		hb_mem_graphic_buf_t hb_mem_graphic_buf;
		int64_t flags = HB_MEM_USAGE_CPU_READ_OFTEN | HB_MEM_USAGE_CPU_WRITE_OFTEN | HB_MEM_USAGE_CACHED
				| HB_MEM_USAGE_GRAPHIC_CONTIGUOUS_BUF;
		ret = hb_mem_alloc_graph_buf(display_context.input_file_width, display_context.input_file_height,
					     MEM_PIX_FMT_NV12, flags, 0, 0, &hb_mem_graphic_buf);
		if (ret != 0) {
			printf("hb_mem_alloc_graph_buf failed: ret=%d (errno=%d)\n", ret, errno);
			goto close_mem_module;
		}
		ret = read_nv12_image_to_graphic_buffer(display_context.input_file, &hb_mem_graphic_buf,
							display_context.input_file_width,
							display_context.input_file_height);
		if (ret != 0) {
			printf("read_nv12_image_to_graphic_buffer failed: ret=%d (errno=%d)\n", ret, errno);
			goto free_hb_mem;
		}
		drm_frame_buffer_info_t fb_info = {0};
		int fb_initialized = 0;

		while (1) {
			fd_set fds;
			struct timeval tv;
			int maxfd = -1;

			FD_ZERO(&fds);

			if (display_ready) {
				FD_SET(drm_fd, &fds);
				maxfd = drm_fd;
			}

			if (udev_fd >= 0) {
				FD_SET(udev_fd, &fds);
				if (udev_fd > maxfd)
					maxfd = udev_fd;
			}

			tv.tv_sec = 1;
			tv.tv_usec = 0;

			int sel = select(maxfd + 1, &fds, NULL, NULL, &tv);
			if (sel < 0) {
				if (errno == EINTR)
					continue;
				perror("select");
				break;
			}

			if (sel > 0) {
				if (display_ready && FD_ISSET(drm_fd, &fds)) {
					if (evctx.version != 0)
						drmHandleEvent(drm_fd, &evctx);
				}

				if (udev_fd >= 0 && FD_ISSET(udev_fd, &fds)) {
					int hp = handle_udev_drm_hotplug(mon, display_context.drm_fd,
									 display_context.connector_id,
									 display_context.connector_type);

					if (hp == 0) { /* disconnected */
						display_ready = 0;
						if (fb_initialized) {
							__unwraper_dma_buffer_to_drm_frame_buffer(display_context
												      .drm_fd,
												  &fb_info);
							fb_initialized = 0;
						}
					} else if (hp == 1) { /* connected */
						if (!display_ready) {
							if (display_setup(&display_context) == 0) {
								printf("Connector is now connected, start display.\n");
								display_ready = 1;
							}
						}
					}
				}
			}

			/* 首次连上显示后，wrap buffer 并提交第一帧 */
			if (display_ready && !fb_initialized) {
				ret = __wraper_dma_buffer_to_drm_frame_buffer(display_context.drm_fd, DRM_FORMAT_NV12,
									      display_context.width,
									      display_context.height,
									      hb_mem_graphic_buf.fd[0], &fb_info);
				if (ret != 0) {
					printf("__wraper_dma_buffer_to_drm_frame_buffer failed :%d\n", ret);
					goto free_hb_mem;
				}

				drmModeAtomicReq *req = drmModeAtomicAlloc();
				if (req == NULL) {
					printf("drmModeAtomicAlloc failed.\n");
					goto unraper_dma_buffer;
				}

				ret = __set_plane_common_properties(&display_context, req, fb_info.frame_buffer_id, 1,
								    DRM_MODE_ROTATE_0);
				if (ret != 0) {
					printf("__add_property failed\n");
					drmModeAtomicFree(req);
					goto unraper_dma_buffer;
				}
				flags = DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT;
				ret = drmModeAtomicCommit(display_context.drm_fd, req, flags, NULL);
				if (ret != 0) {
					printf("drmModeAtomicCommit failed %d.\n", ret);
					drmModeAtomicFree(req);
					goto unraper_dma_buffer;
				}
				drmModeAtomicFree(req);

				fb_initialized = 1;
			}
		}

		if (fb_initialized) {
		unraper_dma_buffer:
			__unwraper_dma_buffer_to_drm_frame_buffer(display_context.drm_fd, &fb_info);
		}
	free_hb_mem:
		hb_mem_free_buf(hb_mem_graphic_buf.fd[0]);
	close_mem_module:
		hb_mem_module_close();
	}

	if (mon)
		udev_monitor_unref(mon);
	if (udev)
		udev_unref(udev);

close_drm:
	close(display_context.drm_fd);
	return 0;
}
