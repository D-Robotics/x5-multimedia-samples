
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <pthread.h>

//for hbm
#include "hb_mem_mgr.h"
#include "common_utils.h"

//for display
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

// #define MIPI_TX_TEST

typedef struct drm_frame_buffer_info_s {
	int dump_handle;
	int frame_buffer_id;
	int frame_buffer_size;
	void *frame_buffer_vaddr;
}drm_frame_buffer_info_t;

typedef struct display_context_s {
	int width;
	int height;

	int drm_fd;

	uint32_t crtc_id;
	uint32_t plane_id;
	uint32_t connector_id;

	int connector_type;
	drm_frame_buffer_info_t fb_info;
	drmModeAtomicReq *req;

#ifdef MIPI_TX_TEST
	hb_mem_common_buf_t hb_mem_buf;
#endif
}display_context_t;

static display_context_t display_context = {0};

void printf_insmod_driver_cmd(void){
	perror("drmOpen failed, maybe display driver is not loaded, please execute the following command:");
	printf("\tmodprobe panel-jc-050hd134\n");
	printf("\tmodprobe galcore\n");
	printf("\tmodprobe vio_n2d\n");
	printf("\tmodprobe lontium_lt8618\n");
	printf("\tmodprobe vs-x5-syscon-bridge\n");
	printf("\tmodprobe vs_drm\n");
	printf("\n\n");
}

static int __add_property(int drm_fd, drmModeAtomicReq *req, uint32_t obj_id,
	uint32_t obj_type, const char *name, uint64_t value)
{
	drmModeObjectProperties *props =
		drmModeObjectGetProperties(drm_fd, obj_id, obj_type);
	if (!props)
	{
		fprintf(stderr, "Failed to get properties for object %u\n", obj_id);
		return -1;
	}

	uint32_t prop_id = 0;
	for (uint32_t i = 0; i < props->count_props; i++)
	{
		drmModePropertyRes *prop = drmModeGetProperty(drm_fd, props->props[i]);
		if (!prop){
			continue;
		}

		if (strcmp(prop->name, name) == 0){
			prop_id = prop->prop_id;
			drmModeFreeProperty(prop);
			break;
		}

		drmModeFreeProperty(prop);
	}

	drmModeFreeObjectProperties(props);

	if (prop_id == 0){
		fprintf(stderr, "Property '%s' not found on object %u\n", name, obj_id);
		return -1;
	}

	if (drmModeAtomicAddProperty(req, obj_id, prop_id, value) < 0){
		fprintf(stderr, "Failed to add property '%s' on object %u: %s\n", name, obj_id, strerror(errno));
		return -1;
	}

	return 0;
}

static drmModeModeInfo *__get_valid_mode_from_connector(drmModeConnector* conn, int width, int height){
	drmModeModeInfo *mode = NULL;

	if(conn->connection != DRM_MODE_CONNECTED){
		printf("display connector type %d is not connected.\n", conn->connector_type);
		return mode;
	}else if(conn->count_modes <= 0){
		printf("display connector connector not found mode info.\n");
		return mode;
	}else{
		if((width == -1) || (height == -1)){
			return &conn->modes[0];;
		}
		for (int i = 0; i < conn->count_modes; i++){

			if((conn->modes[i].hdisplay == width) &&
				(conn->modes[i].vdisplay == height)){
				mode = &conn->modes[i];
				break;
			}
		}
		if(mode == NULL){
			printf("display connector not support resolution: %d*%d.\n",
				mode->hdisplay, mode->vdisplay);
			return mode;
		}
	}
	return mode;
}

#if 0
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
#endif

#if 0
static int list_connector_support_resolution(display_context_t *display_context){
	int ret = 0;
	param_config_t *param_config = &display_context->param_config;

	drmModeRes *resources = drmModeGetResources(display_context->drm_fd);
	if (!resources){
		printf("drmModeGetResources failed.\n");
		return -1;
	}

	drmModeConnector* conn = NULL;
	for (int i = 0; i < resources->count_connectors; i++){
		conn = drmModeGetConnector(display_context->drm_fd, resources->connectors[i]);
		if (conn != NULL){
			if (conn->connector_type == display_context->connector_type){
				break;
			} else {
				drmModeFreeConnector(conn);
				conn = NULL; // Reset conn to NULL if it's not what we're looking for
			}
		}
	}
	if(conn == NULL){
		printf("not support %s.\n", param_config->output);
		goto free_res;
	}

	if(conn->connection != DRM_MODE_CONNECTED){
		ret = -2;
		printf("display connector %d is not connected.\n", conn->connector_type);
		goto free_conn;
	}else if(conn->count_modes <= 0){
		ret = -3;
		printf("display connector not found mode info.\n");
		goto free_conn;
	}else{
		drmModeModeInfo *mode = NULL;
		printf("Print [%s] connector support resolution:\n", param_config->output);
		for (int i = 0; i < conn->count_modes; i++){
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
#endif

// RGB888
int __wraper_dma_buffer_to_drm_frame_buffer(int drm_fd, uint32_t format, uint32_t width, uint32_t height, int dma_buf_fd,
	drm_frame_buffer_info_t *fb_info)
{
   struct drm_prime_handle prime_handle = {
	   .fd = dma_buf_fd,
	   .flags = 0,
	   .handle = 0,
   };
   if (drmIoctl(drm_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime_handle) < 0){
	   perror("DRM_IOCTL_PRIME_FD_TO_HANDLE");
	   return -1;
   }

	uint32_t buf_id;
	uint32_t handles[4] = {0};
	uint32_t strides[4] = {0};
	uint32_t offsets[4] = {0};
	handles[0] = prime_handle.handle;
	strides[0] = width * 3;
	offsets[0] = 0;

	// handles[1] = prime_handle.handle;
	// strides[1] = width;
	// offsets[1] = width * height;

    if (drmModeAddFB2(drm_fd, width, height, format, handles, strides, offsets, &buf_id, 0)){
        perror("drmModeAddFB2");
        return -1;
    }
   fb_info->dump_handle = prime_handle.handle;
   fb_info->frame_buffer_id = buf_id;
   fb_info->frame_buffer_vaddr = NULL;
   fb_info->frame_buffer_size = -1;

	printf("buffer id = %d\n", buf_id);

   return 0;
}

void __unwraper_dma_buffer_to_drm_frame_buffer(int drm_fd, drm_frame_buffer_info_t *fb_info){
	drmModeRmFB(drm_fd, fb_info->frame_buffer_id);
}

static int display_setup(display_context_t *display_context, int width, int height){
	int ret = -1;
	int connector_id = display_context->connector_id;
	int crtc_id = display_context->crtc_id;
	int drm_fd = display_context->drm_fd;
	// param_config_t *param_config = &display_context->param_config;

	drmModeRes *resources = drmModeGetResources(drm_fd);
	if (!resources){
		perror("drmModeGetResources");
		return -1;
	}

	drmModeConnector* conn = NULL;
	for (int i = 0; i < resources->count_connectors; i++){
		conn = drmModeGetConnector(drm_fd, resources->connectors[i]);
		if (conn != NULL){
			if (conn->connector_type == display_context->connector_type){
				break;
			} else {
				drmModeFreeConnector(conn);
				conn = NULL; // Reset conn to NULL if it's not what we're looking for
			}
		}
	}
	if(conn == NULL){
		// printf("not support %s, connector type is %d.\n", param_config->output, display_context->connector_type);
		goto free_res;
	}
	drmModeModeInfo* mode = __get_valid_mode_from_connector(conn, width, height);
	if(mode == NULL){
		goto free_conn;
	}
	display_context->width = mode->hdisplay;
	display_context->height = mode->vdisplay;

	printf("display select resolution :%d*%d .\n", display_context->width, display_context->height);
	if (drmSetClientCap(drm_fd, DRM_CLIENT_CAP_ATOMIC, 1) < 0){
		perror("drmSetClientCap DRM_CLIENT_CAP_ATOMIC");
		goto free_conn;
	}

	if (drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) < 0){
		perror("drmSetClientCap DRM_CLIENT_CAP_UNIVERSAL_PLANES");
		goto free_conn;
	}

	drmModeCrtc *crtc = drmModeGetCrtc(drm_fd, crtc_id);
	if (!crtc){
		perror("drmModeGetCrtc");
		goto free_conn;
	}
	uint32_t blob_id;
	if (drmModeCreatePropertyBlob(drm_fd, mode, sizeof(*mode), &blob_id) < 0){
		perror("drmModeCreatePropertyBlob");
		goto free_crtc;
	}

	drmModeAtomicReq *req = drmModeAtomicAlloc();
	if (!req){
		perror("drmModeAtomicAlloc");
		goto free_crtc;
	}

	ret = __add_property(drm_fd, req, crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", 1);
	ret |= __add_property(drm_fd, req, crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID", blob_id);
	ret |= __add_property(drm_fd, req, connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", crtc_id);

	if(ret != 0){
		goto free_req;
	}

	uint32_t flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
	if (drmModeAtomicCommit(drm_fd, req, flags, NULL) < 0){
		perror("drmModeAtomicCommit");
		goto free_req;
	}
	ret = 0;

free_req:
	drmModeAtomicFree(req);
free_crtc:
	drmModeFreeCrtc(crtc);
free_conn:
	drmModeFreeConnector(conn);
free_res:
	drmModeFreeResources(resources);
	return ret;

}

int drm_mipi_tx_flush(void)
{
#ifdef MIPI_TX_TEST
	static uint32_t cnt = 0;
	uint8_t *pixel = (uint8_t *)display_context.hb_mem_buf.virt_addr;

	cnt++;
	if (cnt == 0) {
		for (uint32_t y = 0; y < display_context.height; y++) {
			for (uint32_t x = 0; x < display_context.width; x++) {
				pixel[0] = 0x00;  // R
				pixel[1] = 0xFF;  // G
				pixel[2] = 0x00;  // B
				pixel += 3;
			}
		}
	}
	else if (cnt == 1) {
		for (uint32_t y = 0; y < display_context.height; y++) {
			for (uint32_t x = 0; x < display_context.width; x++) {
				pixel[0] = 0xFF;  // R
				pixel[1] = 0x00;  // G
				pixel[2] = 0x00;  // B
				pixel += 3;
			}
		}
	}else if (cnt == 2) {
		for (uint32_t y = 0; y < display_context.height; y++) {
			for (uint32_t x = 0; x < display_context.width; x++) {
				pixel[0] = 0x00;  // R
				pixel[1] = 0x00;  // G
				pixel[2] = 0xFF;  // B
				pixel += 3;
			}
		}
		cnt = 0;
	}
#endif

	int ret;
	drmModeAtomicReq *req = drmModeAtomicAlloc();
	if(req == NULL){
		printf("drmModeAtomicAlloc failed.\n");
		return -1;
	}
	// printf("crtc id = %d\n", display_context.crtc_id);
	// printf("buffer id = %d\n", display_context.fb_info.frame_buffer_id);
	// printf("plane_id = %d\n", display_context.plane_id);

	ret = __add_property(display_context.drm_fd, req, display_context.plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", display_context.crtc_id);
	ret |= __add_property(display_context.drm_fd, req, display_context.plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID", display_context.fb_info.frame_buffer_id);
	// ret |= __add_property(display_context.drm_fd, req, display_context.plane_id, DRM_MODE_OBJECT_PLANE, "rotation", DRM_MODE_ROTATE_0);
	ret |= __add_property(display_context.drm_fd, req, display_context.plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X", 0);
	ret |= __add_property(display_context.drm_fd, req, display_context.plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y", 0);
	ret |= __add_property(display_context.drm_fd, req, display_context.plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W", display_context.width << 16);
	ret |= __add_property(display_context.drm_fd, req, display_context.plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H", display_context.height << 16);
	ret |= __add_property(display_context.drm_fd, req, display_context.plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X", 0);
	ret |= __add_property(display_context.drm_fd, req, display_context.plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y", 0);
	ret |= __add_property(display_context.drm_fd, req, display_context.plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W", display_context.width);
	ret |= __add_property(display_context.drm_fd, req, display_context.plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H", display_context.height);
	if(ret != 0){
		printf("__add_property failed\n");
		drmModeAtomicFree(req);
		return -1;
	}
	uint32_t flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
	ret = drmModeAtomicCommit(display_context.drm_fd, req, flags, NULL);
	if(ret != 0){
		printf("drmModeAtomicCommit failed %d.\n", ret);
		drmModeAtomicFree(req);
		return -1;
	}
	drmModeAtomicFree(req);

	return 0;
}

int drm_mipi_tx_init(int dma_buf_fd, int width, int height)
{
	int ret;
	display_context.width = width;
	display_context.height = height;
	display_context.connector_id = 77;
	display_context.connector_type = DRM_MODE_CONNECTOR_VIRTUAL;
	display_context.crtc_id = 31;
	display_context.plane_id = 41;

	display_context.drm_fd = drmOpen("vs-drm", NULL);
	if (display_context.drm_fd < 0) {
		printf_insmod_driver_cmd();
		return -1;
	}

	ret = display_setup(&display_context, width, height);
	if(ret != 0){
		printf("[DRM] display_setup Failed: %d\n", ret);
		goto close_drm;
	}

/* Test Bgn */
#ifdef MIPI_TX_TEST
	int64_t flags = HB_MEM_USAGE_CPU_READ_OFTEN | HB_MEM_USAGE_CPU_WRITE_OFTEN ;   // Cached
	if (0 != hb_mem_alloc_com_buf(1920 * 1080 * 3, flags, &display_context.hb_mem_buf)) {
		printf("hb_mem_alloc_com_buf For Stream Buffer Failed\n");
		goto close_drm;
	}
#endif
/* Test End */

	drm_frame_buffer_info_t *fb_info = &display_context.fb_info;
	// DRM_FORMAT_RGB888
	// DRM_FORMAT_NV12
	#if 0
	ret = __wraper_dma_buffer_to_drm_frame_buffer(display_context.drm_fd, DRM_FORMAT_RGB888,
							display_context.width, display_context.height, \
							display_context.hb_mem_buf.fd, \
							fb_info);
	#endif
	ret = __wraper_dma_buffer_to_drm_frame_buffer(display_context.drm_fd, DRM_FORMAT_RGB888,
							display_context.width, display_context.height, \
							dma_buf_fd, \
							fb_info);
	if(ret != 0){
		printf("[DRM] __wraper_dma_buffer_to_drm_frame_buffer failed :%d\n", ret);
		goto close_drm;
	}

	return 0;

close_drm:
	close(display_context.drm_fd);
	return -1;
}

void drm_mipi_tx_destroy(void)
{
	__unwraper_dma_buffer_to_drm_frame_buffer(display_context.drm_fd, &display_context.fb_info);

	if (display_context.drm_fd > 0) {
		close(display_context.drm_fd);
	}

/* Test Bgn */
#ifdef MIPI_TX_TEST
	if (0 != hb_mem_free_buf(display_context.hb_mem_buf.fd)) {
		printf("hb_mem_free_buf For Stream Buffer Failed\n");
	}
#endif
/* Test End */
}

