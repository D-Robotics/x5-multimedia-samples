// #define J5
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>
#include <signal.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include "vp_display.h"


static void page_flip_handler(int fd, unsigned int frame,
							unsigned int sec, unsigned int usec,
							void *data);

static void add_property(int drm_fd, drmModeAtomicReq *req, uint32_t obj_id,
	uint32_t obj_type, const char *name, uint64_t value)
{
	drmModeObjectProperties *props =
		drmModeObjectGetProperties(drm_fd, obj_id, obj_type);
	if (!props)
	{
		fprintf(stderr, "Failed to get properties for object %u\n", obj_id);
		return;
	}

	uint32_t prop_id = 0;
	for (uint32_t i = 0; i < props->count_props; i++)
	{
		drmModePropertyRes *prop = drmModeGetProperty(drm_fd, props->props[i]);
		if (!prop)
		{
			continue;
		}

		if (strcmp(prop->name, name) == 0)
		{
			prop_id = prop->prop_id;
			drmModeFreeProperty(prop);
			break;
		}

		drmModeFreeProperty(prop);
	}

	drmModeFreeObjectProperties(props);

	if (prop_id == 0)
	{
		fprintf(stderr, "Property '%s' not found on object %u\n", name, obj_id);
		return;
	}

	if (drmModeAtomicAddProperty(req, obj_id, prop_id, value) < 0)
	{
		fprintf(stderr, "Failed to add property '%s' on object %u: %s\n", name, obj_id, strerror(errno));
	}
}

static void drm_set_client_capabilities(int drm_fd)
{
	if (drmSetClientCap(drm_fd, DRM_CLIENT_CAP_ATOMIC, 1) < 0)
	{
		perror("drmSetClientCap DRM_CLIENT_CAP_ATOMIC");
	}

	if (drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) < 0)
	{
		perror("drmSetClientCap DRM_CLIENT_CAP_UNIVERSAL_PLANES");
	}
}

void print_connector_info(int drm_fd) {
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
		printf("    Connection: %s\n", connector->connection == DRM_MODE_CONNECTED ? "Connected" : "Disconnected");
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

	float target_fps = 30.0;
	float fps_tolerance = 1.0;
	drmModeModeInfo *mode = NULL;
	drmModeModeInfo *resolution_match = NULL; // 新增：同分辨率模式缓存

	for (int i = 0; i < connector->count_modes; i++) {
		float current_fps = __mode_vrefresh(&connector->modes[i]);
		printf("Checking mode %dx%d@%.2fHz\n",
			connector->modes[i].hdisplay,
			connector->modes[i].vdisplay,
			current_fps);

		if (connector->modes[i].hdisplay == ctx->width &&
			connector->modes[i].vdisplay == ctx->height) {

			if (!resolution_match) {
				resolution_match = &connector->modes[i];
				printf("Found resolution match: %dx%d@%.2fHz\n",
					  resolution_match->hdisplay, resolution_match->vdisplay, current_fps);
			}

			// 检查是否满足帧率要求
			if (fabs(current_fps - target_fps) <= fps_tolerance) {
				mode = &connector->modes[i];
				printf("Selected exact match: %dx%d@%.2fHz\n",
					mode->hdisplay, mode->vdisplay, current_fps);
				break;
			}
		}
	}

	if (!mode && resolution_match) {
		mode = resolution_match;
		printf("Using resolution-matched mode: %dx%d@%.2fHz\n",
			mode->hdisplay, mode->vdisplay, __mode_vrefresh(mode));
	}

	if (!mode) {
		fprintf(stderr, "No matching mode found for %dx%d\n", ctx->width, ctx->height);
		if (connector->count_modes > 0) {
			mode = &connector->modes[0];
			fprintf(stderr, "Using fallback mode %dx%d@%.2fHz\n",
				mode->hdisplay, mode->vdisplay, __mode_vrefresh(mode));
		} else {
			fprintf(stderr, "No available modes\n");
			drmModeFreeCrtc(crtc);
			drmModeFreeConnector(connector);
			drmModeFreeResources(resources);
			return -1;
		}
	}

	if (mode->hdisplay != ctx->width || mode->vdisplay != ctx->height) {
		fprintf(stderr, "Mode resolution mismatch: %dx%d != %dx%d\n",
			mode->hdisplay, mode->vdisplay, ctx->width, ctx->height);
		drmModeFreeCrtc(crtc);
		drmModeFreeConnector(connector);
		drmModeFreeResources(resources);
		return -1;
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
		add_property(ctx->drm_fd, req, ctx->planes[i].plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W", ctx->planes[i].src_w << 16);
		add_property(ctx->drm_fd, req, ctx->planes[i].plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H", ctx->planes[i].src_h << 16);

		add_property(ctx->drm_fd, req, ctx->planes[i].plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X", ctx->planes[i].crtc_x);
		add_property(ctx->drm_fd, req, ctx->planes[i].plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y", ctx->planes[i].crtc_y);
		add_property(ctx->drm_fd, req, ctx->planes[i].plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W", ctx->planes[i].crtc_w);
		add_property(ctx->drm_fd, req, ctx->planes[i].plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H", ctx->planes[i].crtc_h);

		if (ctx->planes[i].z_pos != -1)
		{
			add_property(ctx->drm_fd, req, ctx->planes[i].plane_id, DRM_MODE_OBJECT_PLANE, "zpos", ctx->planes[i].z_pos);
		}

		if (ctx->planes[i].alpha != -1)
		{
			add_property(ctx->drm_fd, req, ctx->planes[i].plane_id, DRM_MODE_OBJECT_PLANE, "alpha", ctx->planes[i].alpha);
		}

		if (ctx->planes[i].pixel_blend_mode != -1)
		{
			add_property(ctx->drm_fd, req, ctx->planes[i].plane_id, DRM_MODE_OBJECT_PLANE, "pixel blend mode", ctx->planes[i].pixel_blend_mode);
		}

		if (ctx->planes[i].rotation != -1)
		{
			add_property(ctx->drm_fd, req, ctx->planes[i].plane_id, DRM_MODE_OBJECT_PLANE, "rotation", ctx->planes[i].rotation);
		}

		if (ctx->planes[i].color_encoding != -1)
		{
			add_property(ctx->drm_fd, req, ctx->planes[i].plane_id, DRM_MODE_OBJECT_PLANE, "COLOR_ENCODING", ctx->planes[i].color_encoding);
		}

		if (ctx->planes[i].color_range != -1)
		{
			add_property(ctx->drm_fd, req, ctx->planes[i].plane_id, DRM_MODE_OBJECT_PLANE, "COLOR_RANGE", ctx->planes[i].color_range);
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

static drmModeConnector* find_connector(int fd)
{
	int i = 0;

	// drmModeRes描述了计算机所有的显卡信息：connector，encoder，crtc，modes等。
	drmModeRes *resources = drmModeGetResources(fd);
	if (!resources)
	{
		return NULL;
	}

	drmModeConnector* conn = NULL;
	for (i = 0; i < resources->count_connectors; i++)
	{
		conn = drmModeGetConnector(fd, resources->connectors[i]);
		if (conn != NULL)
		{
			// Check if the connector type is HDMI and it's connected
			if (conn->connector_type == DRM_MODE_CONNECTOR_HDMIA &&
				conn->connection == DRM_MODE_CONNECTED &&
				conn->count_modes > 0)
			{
				break; // Found an HDMI connector that is connected and has available modes
			}
			else
			{
				drmModeFreeConnector(conn);
				conn = NULL; // Reset conn to NULL if it's not what we're looking for
			}
		}
	}

	drmModeFreeResources(resources);
	return conn; // Will return NULL if no suitable connector was found
}

static void drm_init_config(vp_drm_context_t *drm_ctx,
							int32_t width, int32_t height, uint32_t plane_id, uint32_t crtc_id)
{
	memset(drm_ctx, 0, sizeof(vp_drm_context_t));
	drm_ctx->crtc_id = crtc_id;
	drm_ctx->connector_id = 75;
	drm_ctx->width = width;
	drm_ctx->height = height;

	drm_ctx->plane_count = 1;

	for (int i = 0; i < drm_ctx->plane_count; i++)
	{
		drm_ctx->planes[i].plane_id = plane_id;
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

int32_t vp_display_check_hdmi_is_connected(){
	int drm_fd = drmOpen("vs-drm", NULL);
		if (drm_fd < 0) {
		perror("drmOpen failed, maybe hdmi driver is not loaded, please execute the following command:");
		printf("\tmodprobe panel-jc-050hd134\n");
		printf("\tmodprobe galcore\n");
		printf("\tmodprobe vio_n2d\n");
		printf("\tmodprobe lontium_lt8618\n");
		printf("\tmodprobe vs-x5-syscon-bridge\n");
		printf("\tmodprobe vs_drm\n");
		printf("\n\n");
		return -1;
	}

	drmModeConnectorPtr connector = find_connector(drm_fd);
	if (connector == NULL) {
		close(drm_fd);
		return 0;
	}

	close(drm_fd);
	return 1;
}

// 检查分辨率是否支持
int vp_display_is_resolution_supported(int width, int height)
{
	int drm_fd = drmOpen("vs-drm", NULL);
	if (drm_fd < 0) {
		perror("drmOpen failed");
		return 0;
	}

	drmModeConnectorPtr connector = find_connector(drm_fd);
	if (!connector) {
		close(drm_fd);
		return 0;
	}

	int supported = 0;
	printf("Checking resolutions:\n");
	for (int i = 0; i < connector->count_modes; i++) {
		drmModeModeInfo *mode = &connector->modes[i];
		printf("Mode %d: %dx%d @ %dHz\n", i, mode->hdisplay, mode->vdisplay, mode->vrefresh);
		if (mode->hdisplay == width && mode->vdisplay == height) {
			supported = 1;
			break;
		}
	}

	drmModeFreeConnector(connector);
	close(drm_fd);
	return supported;
}

// 打印支持的分辨率
void vp_display_print_supported_resolutions()
{
	int drm_fd = drmOpen("vs-drm", NULL);
	if (drm_fd < 0) {
		perror("drmOpen failed");
		return;
	}

	drmModeConnectorPtr connector = find_connector(drm_fd);
	if (!connector) {
		close(drm_fd);
		return;
	}

	printf("Connected to connector: %d\n", connector->connector_id);
	for (int i = 0; i < connector->count_modes; i++) {
		drmModeModeInfo *mode = &connector->modes[i];
		printf("  Mode %d: %dx%d @ %dHz\n", i, mode->hdisplay, mode->vdisplay, mode->vrefresh);
	}

	drmModeFreeConnector(connector);
	close(drm_fd);
}

int32_t vp_display_get_max_resolution_if_not_match(
		int32_t width, int32_t height, int32_t *out_width, int32_t *out_height){

	int drm_fd = drmOpen("vs-drm", NULL);
		if (drm_fd < 0) {
		perror("drmOpen failed");
		return -1;
	}

	drmModeRes *resources = drmModeGetResources(drm_fd);
	if (!resources)
	{
		perror("drmModeGetResources");
		return -1;
	}
	drmModeConnectorPtr connector_ptr = NULL;
	connector_ptr = find_connector(drm_fd);
	if (connector_ptr == NULL) {
		perror("find_connector failed");
		return -1;
	}

	drmModeConnector *connector = drmModeGetConnector(drm_fd, connector_ptr->connector_id);
	if (!connector)
	{
		perror("drmModeGetConnector");
		drmModeFreeResources(resources);
		return -1;
	}
	int max_width = 0;
	int max_height = 0;

	drmModeModeInfo *mode = NULL;
	for (int i = 0; i < connector->count_modes; i++)
	{
		int max_resolusion = max_width * max_height;
		if (connector->modes[i].hdisplay == width && connector->modes[i].vdisplay == height)
		{
			mode = &connector->modes[i];
			break;
		}
		if(connector->modes[i].hdisplay * connector->modes[i].vdisplay > max_resolusion){
			max_width = connector->modes[i].hdisplay;
			max_height = connector->modes[i].vdisplay;
		}
	}

	drmModeFreeConnector(connector);
	drmModeFreeResources(resources);
	close(drm_fd);

	if(mode){
		*out_width = mode->hdisplay;
		*out_height = mode->vdisplay;
		return 1;
	}else{
		*out_width = max_width;
		*out_height = max_height;
	}
	return 0;
}

/**
 * @brief 匹配sensor和HDMI分辨率：优先精确匹配，无则找长宽都≤输入值的最大分辨率
 * @param width 输入的目标宽度（sensor分辨率宽）
 * @param height 输入的目标高度（sensor分辨率高）
 * @param out_width 输出匹配到的宽度
 * @param out_height 输出匹配到的高度
 * @return 1-精确匹配成功；0-找到符合条件的非精确匹配；-1-失败（无可用分辨率/接口错误）
 */
int32_t vp_display_get_fit_smaller_resolution(
	int32_t width, int32_t height, int32_t *out_width, int32_t *out_height)
{

	if (out_width == NULL || out_height == NULL || width <= 0 || height <= 0) {
		fprintf(stderr, "Invalid input parameters\n");
		return -1;
	}

	*out_width = 0;
	*out_height = 0;

	int drm_fd = drmOpen("vs-drm", NULL);
	if (drm_fd < 0) {
		perror("drmOpen failed");
		return -1;
	}

	drmModeRes *resources = drmModeGetResources(drm_fd);
	if (!resources) {
		perror("drmModeGetResources failed");
		close(drm_fd);
		return -1;
	}

	drmModeConnectorPtr connector_ptr = find_connector(drm_fd);
	if (connector_ptr == NULL) {
		perror("find_connector failed");
		drmModeFreeResources(resources);
		close(drm_fd);
		return -1;
	}

	drmModeConnector *connector = drmModeGetConnector(drm_fd, connector_ptr->connector_id);
	if (!connector) {
		perror("drmModeGetConnector failed");
		drmModeFreeResources(resources);
		close(drm_fd);
		return -1;
	}

	drmModeModeInfo *exact_mode = NULL;
	int fit_max_area = 0;
	int fit_width = 0, fit_height = 0;

	for (int i = 0; i < connector->count_modes; i++) {
		int curr_width = connector->modes[i].hdisplay;
		int curr_height = connector->modes[i].vdisplay;
		int curr_area = curr_width * curr_height;

		if (curr_width == width && curr_height == height) {
			exact_mode = &connector->modes[i];
			break;
		}

		if (curr_width <= width && curr_height <= height) {
			if (curr_area > fit_max_area) {
				fit_max_area = curr_area;
				fit_width = curr_width;
				fit_height = curr_height;
			}
		}
	}

	drmModeFreeConnector(connector);
	drmModeFreeResources(resources);
	close(drm_fd);

	if (exact_mode) {
		*out_width = exact_mode->hdisplay;
		*out_height = exact_mode->vdisplay;
		return 1;
	} else if (fit_max_area > 0) {
		*out_width = fit_width;
		*out_height = fit_height;
		return 0;
	} else {
		fprintf(stderr, "No resolution found (all modes are larger than %dx%d)\n", width, height);
		return -1;
	}
}

int32_t vp_display_init(vp_drm_context_t *drm_ctx, int32_t width, int32_t height)
{
	int32_t ret = 0;
	drmModeConnectorPtr connector = NULL;

	VP_LOG(drm_ctx, VP_LOG_LEVEL_INFO, "Initializing DRM display...\n");

	if (drm_ctx->bt1120)
		drm_init_config(drm_ctx, width, height, 64, 63); //bt1120
	else
		drm_init_config(drm_ctx, width, height, 33, 31); //dc8000

	drm_ctx->front_fb_id = 0;
	drm_ctx->back_fb_id = 0;
	drm_ctx->back_ready = false;
	pthread_mutex_init(&drm_ctx->buf_mutex, NULL);

	memset(&drm_ctx->evctx, 0, sizeof(drm_ctx->evctx));
	drm_ctx->evctx.version = DRM_EVENT_CONTEXT_VERSION;
	drm_ctx->evctx.page_flip_handler = page_flip_handler;

	drm_ctx->use_nonblock = true;
	drm_ctx->max_wait_ms_for_back_ready = 50;  /* ms */
	drm_ctx->max_atomic_retries = 5;
	drm_ctx->busy_warn_threshold = 3;

	drm_ctx->drm_fd = drmOpen("vs-drm", NULL);
	if (drm_ctx->drm_fd < 0) {
		perror("drmOpen failed");
		return -1;
	}

	connector = find_connector(drm_ctx->drm_fd);
	if (connector == NULL) {
		fprintf(stderr, "No suitable connector found\n");
		close(drm_ctx->drm_fd);
		return -1;
	}
	drm_ctx->connector_id = connector->connector_id;

	VP_LOG(drm_ctx, VP_LOG_LEVEL_DEBUG, "Setting DRM client capabilities...\n");
	drm_set_client_capabilities(drm_ctx->drm_fd);

	printf("Setting up KMS...\n");
	ret = drm_setup_kms(drm_ctx);
	if (ret < 0) {
		fprintf(stderr, "drm_setup_kms failed\n");
		close(drm_ctx->drm_fd);
		return -1;
	}

	return ret;
}

int32_t vp_display_deinit(vp_drm_context_t *drm_ctx)
{
	int32_t ret = 0;
	dma_buf_map_t *current, *tmp;

	struct pollfd pfd = {drm_ctx->drm_fd, POLLIN, 0};
	while (poll(&pfd, 1, 0) > 0) {
		drmHandleEvent(drm_ctx->drm_fd, &drm_ctx->evctx);
	}

	HASH_ITER(hh, drm_ctx->buffer_map, current, tmp) {
		if (current->fb_id) {
			if (drmModeRmFB(drm_ctx->drm_fd, current->fb_id) < 0) {
				perror("drmModeRmFB");
			}
		}
		HASH_DEL(drm_ctx->buffer_map, current);
		free(current);
	}

	pthread_mutex_lock(&drm_ctx->buf_mutex);
	if (drm_ctx->front_fb_id != 0) {
		drmModeRmFB(drm_ctx->drm_fd, drm_ctx->front_fb_id);
	}
	if (drm_ctx->back_fb_id != 0 && drm_ctx->back_fb_id != drm_ctx->front_fb_id) {
		drmModeRmFB(drm_ctx->drm_fd, drm_ctx->back_fb_id);
	}
	pthread_mutex_unlock(&drm_ctx->buf_mutex);
	drmModeSetCrtc(drm_ctx->drm_fd, drm_ctx->crtc_id, 0, 0, 0, NULL, 0, NULL);

	drmModeRes *resources = drmModeGetResources(drm_ctx->drm_fd);
	if (resources) {
		for (int i = 0; i < resources->count_crtcs; i++) {
			drmModeFreeCrtc(drmModeGetCrtc(drm_ctx->drm_fd, resources->crtcs[i]));
		}
		for (int i = 0; i < resources->count_connectors; i++) {
			drmModeFreeConnector(drmModeGetConnector(drm_ctx->drm_fd, resources->connectors[i]));
		}
		for (int i = 0; i < resources->count_encoders; i++) {
			drmModeFreeEncoder(drmModeGetEncoder(drm_ctx->drm_fd, resources->encoders[i]));
		}
		drmModeFreeResources(resources);
	}

	drmModePlaneRes *plane_resources = drmModeGetPlaneResources(drm_ctx->drm_fd);
	if (plane_resources) {
		for (uint32_t i = 0; i < plane_resources->count_planes; i++) {
			drmModeFreePlane(drmModeGetPlane(drm_ctx->drm_fd, plane_resources->planes[i]));
		}
		drmModeFreePlaneResources(plane_resources);
	}
	if (drm_ctx->drm_fd >= 0) {
		close(drm_ctx->drm_fd);
		drm_ctx->drm_fd = -1;
	}

	pthread_mutex_destroy(&drm_ctx->buf_mutex);

	return ret;
}

static uint32_t get_format_from_string(const char *format_str)
{
	if (strcmp(format_str, "AR12") == 0)
	{
		return DRM_FORMAT_ARGB4444;
	}
	else if (strcmp(format_str, "AR15") == 0)
	{
		return DRM_FORMAT_ARGB1555;
	}
	else if (strcmp(format_str, "RG16") == 0)
	{
		return DRM_FORMAT_RGB565;
	}
	else if (strcmp(format_str, "AR24") == 0)
	{
		return DRM_FORMAT_ARGB8888;
	}
	else if (strcmp(format_str, "RA12") == 0)
	{
		return DRM_FORMAT_RGBA4444;
	}
	else if (strcmp(format_str, "RA15") == 0)
	{
		return DRM_FORMAT_RGBA5551;
	}
	else if (strcmp(format_str, "RA24") == 0)
	{
		return DRM_FORMAT_RGBA8888;
	}
	else if (strcmp(format_str, "AB12") == 0)
	{
		return DRM_FORMAT_ABGR4444;
	}
	else if (strcmp(format_str, "AB15") == 0)
	{
		return DRM_FORMAT_ABGR1555;
	}
	else if (strcmp(format_str, "BG16") == 0)
	{
		return DRM_FORMAT_BGR565;
	}
	else if (strcmp(format_str, "BG24") == 0)
	{
		return DRM_FORMAT_BGR888;
	}
	else if (strcmp(format_str, "AB24") == 0)
	{
		return DRM_FORMAT_ABGR8888;
	}
	else if (strcmp(format_str, "BA12") == 0)
	{
		return DRM_FORMAT_BGRA4444;
	}
	else if (strcmp(format_str, "BA15") == 0)
	{
		return DRM_FORMAT_BGRA5551;
	}
	else if (strcmp(format_str, "BA24") == 0)
	{
		return DRM_FORMAT_BGRA8888;
	}
	else if (strcmp(format_str, "YUYV") == 0)
	{
		return DRM_FORMAT_YUYV;
	}
	else if (strcmp(format_str, "YVYU") == 0)
	{
		return DRM_FORMAT_YVYU;
	}
	else if (strcmp(format_str, "NV12") == 0)
	{
		return DRM_FORMAT_NV12;
	}
	else if (strcmp(format_str, "NV21") == 0)
	{
		return DRM_FORMAT_NV21;
	}
	else
	{
		return 0; // Unsupported format
	}
}

/* 处理一次 drm 事件（非阻塞），在需要时清理 event 队列 */
static void drm_process_events_once(vp_drm_context_t *drm_ctx)
{
	struct pollfd pfd = { drm_ctx->drm_fd, POLLIN, 0 };
	int ret = poll(&pfd, 1, 0);
	if (ret > 0 && (pfd.revents & POLLIN)) {
		drmHandleEvent(drm_ctx->drm_fd, &drm_ctx->evctx);
		VP_LOG(drm_ctx, VP_LOG_LEVEL_DEBUG, "Processed one DRM event\n");
	}
}

static void page_flip_handler(int fd, unsigned int frame,
							unsigned int sec, unsigned int usec,
							void *data)
{
	vp_drm_context_t *ctx = (vp_drm_context_t *)data;
	static struct timespec last_flip;
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	long interval_us = (now.tv_sec - last_flip.tv_sec) * 1000000 +
					(now.tv_nsec - last_flip.tv_nsec) / 1000;
	last_flip = now;

	pthread_mutex_lock(&ctx->buf_mutex);
	if (ctx->back_fb_id != 0) {
		ctx->front_fb_id = ctx->back_fb_id;
		ctx->back_fb_id = 0;
		ctx->back_ready = false;
	} else {
		VP_LOG(ctx, VP_LOG_LEVEL_WARN, "No back buffer available during page flip\n");
	}
	pthread_mutex_unlock(&ctx->buf_mutex);

	VP_LOG(ctx, VP_LOG_LEVEL_INFO,
		"Page flip completed at %u.%06u (interval: %ldμs)\n",
		sec, usec, interval_us);

	(void)fd; (void)frame;
}

// 获取或创建 framebuffer（保留旧 fb 直到 commit 完成）
static uint32_t get_framebuffer(vp_drm_context_t *drm_ctx,
								int dma_buf_fd,
								int plane_index,
								int width,
								int height,
								int stride,
								int vstride)
{
	dma_buf_map_t *entry = NULL;
	HASH_FIND_INT(drm_ctx->buffer_map, &dma_buf_fd, entry);

	if (entry) {
		drmModeFB *fb_info = drmModeGetFB(drm_ctx->drm_fd, entry->fb_id);
		if (fb_info) {
			if (fb_info->width == width && fb_info->height == height &&
				fb_info->pitch == stride) {
				drmModeFreeFB(fb_info);
				return entry->fb_id;
			}
			drmModeFreeFB(fb_info);
		}
		// 不立即删除旧 fb，留给 commit 翻转完成后再删除
		HASH_DEL(drm_ctx->buffer_map, entry);
		free(entry);
		drm_ctx->buffer_count--;
	}

	VP_LOG(drm_ctx, VP_LOG_LEVEL_DEBUG,
		"Creating new framebuffer: %dx%d format=%s stride=%d vstride=%d\n",
		width, height, drm_ctx->planes[plane_index].format, stride, vstride);

	struct drm_prime_handle prime_handle = { .fd = dma_buf_fd, .flags = 0, .handle = 0 };
	if (drmIoctl(drm_ctx->drm_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime_handle) < 0) {
		VP_LOG(drm_ctx, VP_LOG_LEVEL_ERROR,
			"Failed to map dma_buf_fd=%d to GEM handle: %s\n",
			dma_buf_fd, strerror(errno));
		return 0;
	}

	uint32_t handles[4] = { prime_handle.handle, prime_handle.handle, 0, 0 };
	uint32_t strides[4] = { stride, stride, 0, 0 };
	uint32_t offsets[4] = { 0, stride * vstride, 0, 0 };
	uint32_t fb_id;
	uint32_t drm_format = get_format_from_string(drm_ctx->planes[plane_index].format);

	if (drmModeAddFB2(drm_ctx->drm_fd, width, height, drm_format,
					handles, strides, offsets, &fb_id, 0)) {
		VP_LOG(drm_ctx, VP_LOG_LEVEL_ERROR,
			"Failed to create framebuffer: %dx%d format=%s(0x%x)\n",
			width, height, drm_ctx->planes[plane_index].format, drm_format);
		return 0;
	}

	entry = (dma_buf_map_t *)malloc(sizeof(dma_buf_map_t));
	if (!entry) {
		VP_LOG(drm_ctx, VP_LOG_LEVEL_ERROR, "Failed to allocate dma_buf_map entry\n");
		drmModeRmFB(drm_ctx->drm_fd, fb_id);
		return 0;
	}

	entry->dma_buf_fd = dma_buf_fd;
	entry->fb_id = fb_id;
	HASH_ADD_INT(drm_ctx->buffer_map, dma_buf_fd, entry);
	drm_ctx->buffer_count++;

	VP_LOG(drm_ctx, VP_LOG_LEVEL_DEBUG, "Created framebuffer ID: %u\n", fb_id);
	return fb_id;
}


int32_t vp_display_wait_blank(vp_drm_context_t *drm_ctx)
{
	drmVBlank vbl;
	memset(&vbl, 0, sizeof(vbl));
	vbl.request.type = (drmVBlankSeqType)(DRM_VBLANK_RELATIVE | 0);
	vbl.request.sequence = 1;

	int ret = drmWaitVBlank(drm_ctx->drm_fd, &vbl);
	if (ret != 0) {
		VP_LOG(drm_ctx, VP_LOG_LEVEL_ERROR, "drmWaitVBlank failed: ret=%d\n", ret);
		return -1;
	}
	VP_LOG(drm_ctx, VP_LOG_LEVEL_DEBUG, "drmWaitVBlank succeeded\n");
	return 0;
}

int32_t vp_display_set_frame(vp_drm_context_t *drm_ctx,
							hb_mem_graphic_buf_t *image_frame)
{
	if (!drm_ctx || !image_frame || image_frame->width == 0 || image_frame->height == 0) {
		VP_LOG(drm_ctx, VP_LOG_LEVEL_ERROR, "Invalid frame parameters\n");
		return -1;
	}

	int dma_buf_fds[DRM_MAX_PLANES] = {-1, -1, -1};
	for (uint32_t i = 0; i < drm_ctx->plane_count; ++i)
		dma_buf_fds[i] = image_frame->fd[i];

	drmModeAtomicReq *req = drmModeAtomicAlloc();
	if (!req) {
		VP_LOG(drm_ctx, VP_LOG_LEVEL_ERROR, "drmModeAtomicAlloc failed\n");
		return -1;
	}

	// 等待上一次 back flip 完成
	while (1) {
		pthread_mutex_lock(&drm_ctx->buf_mutex);
		bool need_wait = drm_ctx->back_ready;
		pthread_mutex_unlock(&drm_ctx->buf_mutex);

		if (!need_wait) break;
		drm_process_events_once(drm_ctx);
		usleep(1000);
	}

	pthread_mutex_lock(&drm_ctx->buf_mutex);
	uint32_t fb_id = get_framebuffer(drm_ctx, dma_buf_fds[0], 0,
									image_frame->width, image_frame->height,
									image_frame->stride, image_frame->vstride);
	if (fb_id == 0) {
		VP_LOG(drm_ctx, VP_LOG_LEVEL_ERROR, "Failed to get framebuffer\n");
		pthread_mutex_unlock(&drm_ctx->buf_mutex);
		drmModeAtomicFree(req);
		return -1;
	}

	drm_ctx->back_fb_id = fb_id;
	drm_ctx->back_ready = true;

	add_property(drm_ctx->drm_fd, req, drm_ctx->planes[0].plane_id,
				DRM_MODE_OBJECT_PLANE, "FB_ID", fb_id);
	add_property(drm_ctx->drm_fd, req, drm_ctx->planes[0].plane_id,
				DRM_MODE_OBJECT_PLANE, "CRTC_ID", drm_ctx->crtc_id);
	pthread_mutex_unlock(&drm_ctx->buf_mutex);

	// 阻塞式 atomic commit，确保 page flip 完全执行
	int ret = drmModeAtomicCommit(drm_ctx->drm_fd, req,
								DRM_MODE_PAGE_FLIP_EVENT,
								drm_ctx);
	if (ret != 0) {
		VP_LOG(drm_ctx, VP_LOG_LEVEL_ERROR,
			"Blocking drmModeAtomicCommit failed: ret=%d errno=%d (%s)\n",
			ret, errno, strerror(errno));
		drmModeAtomicFree(req);
		return -1;
	}

	drmModeAtomicFree(req);

	// 等待 page_flip_handler 执行完成
	while (1) {
		pthread_mutex_lock(&drm_ctx->buf_mutex);
		bool flip_done = !drm_ctx->back_ready;
		pthread_mutex_unlock(&drm_ctx->buf_mutex);
		if (flip_done) break;
		drm_process_events_once(drm_ctx);
		usleep(1000);
	}

	VP_LOG(drm_ctx, VP_LOG_LEVEL_DEBUG, "Frame committed: fb_id=%u\n", fb_id);
	return 0;
}

