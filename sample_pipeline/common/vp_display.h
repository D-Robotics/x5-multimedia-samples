/***************************************************************************
 * @COPYRIGHT NOTICE
 * @Copyright 2024 D-Robotics, Inc.
 * @All rights reserved.
 * @Date: 2023-03-05 15:28:19
 * @LastEditTime: 2023-03-05 16:31:05
 ***************************************************************************/
#ifndef VP_DISPLAY_H_
#define VP_DISPLAY_H_

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include "uthash.h"

#include "hbn_api.h"
#ifdef __cplusplus
extern "C" {
#endif


#define DRM_MAX_PLANES 3
#define DRM_ION_MAX_BUFFERS 6

#define VP_LOG_LEVEL_ERROR 0
#define VP_LOG_LEVEL_WARN  1
#define VP_LOG_LEVEL_INFO  2
#define VP_LOG_LEVEL_DEBUG 3

/* 日志输出宏 */
#define VP_LOG(ctx, lvl, fmt, ...) \
	do { \
		if ((ctx)->log_level >= (lvl)) { \
			const char *prefix = \
				(lvl == VP_LOG_LEVEL_ERROR) ? "[ERROR]" : \
				(lvl == VP_LOG_LEVEL_WARN)  ? "[WARN]"  : \
				(lvl == VP_LOG_LEVEL_INFO)  ? "[INFO]"  : "[DEBUG]"; \
			fprintf(stderr, "%s " fmt, prefix, ##__VA_ARGS__); \
		} \
	} while (0)

typedef struct
{
	int dma_buf_fd;
	uint32_t fb_id;
	uint32_t gem_handle;       // 添加 GEM handle
	uint64_t last_used;        // 添加最后使用时间戳
	UT_hash_handle hh; 			//uthash 处理器
} dma_buf_map_t;

typedef struct
{
	uint32_t plane_id;
	uint32_t src_w;
	uint32_t src_h;
	uint32_t crtc_x;
	uint32_t crtc_y;
	uint32_t crtc_w;
	uint32_t crtc_h;
	uint32_t z_pos;
	uint32_t alpha;
	char format[8];          // 图层格式
	uint32_t rotation;       // 旋转属性
	uint32_t color_encoding; // 颜色编码
	uint32_t color_range;    // 颜色范围
	uint32_t pixel_blend_mode; // alpha模式
} drm_plane_config_t;

typedef struct
{
	int drm_fd;
	uint32_t crtc_id;
	uint32_t connector_id;
	drm_plane_config_t planes[DRM_MAX_PLANES];
	uint32_t width;
	uint32_t height;
	int plane_count;
	dma_buf_map_t *buffer_map; // 使用哈希表
	int buffer_count;
	int max_buffers; // 动态调整 buffer_map 的大小

	uint32_t front_fb_id;    // 当前显示的framebuffer
	uint32_t back_fb_id;     // 准备中的framebuffer
	bool back_ready;         // 后台buffer是否就绪
	pthread_mutex_t buf_mutex; // 保护buffer交换的互斥锁
	drmEventContext evctx;  // DRM事件上下文

	bool use_nonblock;
	int max_wait_ms_for_back_ready;
	int max_atomic_retries;
	int busy_warn_threshold;

	int log_level;    /* 新增字段，控制日志等级 */
	bool bt1120;
} vp_drm_context_t;

int32_t vp_display_init(vp_drm_context_t *drm_ctx, int32_t width, int32_t height);
int32_t vp_display_deinit(vp_drm_context_t *drm_ctx);
int32_t vp_display_set_frame(vp_drm_context_t *drm_ctx,
	hb_mem_graphic_buf_t *image_frame);

int32_t vp_display_wait_blank(vp_drm_context_t *drm_ctx);
int32_t vp_display_wait_vsync(vp_drm_context_t *drm_ctx);
int32_t vp_display_check_hdmi_is_connected();
int32_t vp_display_get_max_resolution_if_not_match(int32_t width, int32_t height, int32_t *out_width, int32_t *out_height);
int vp_display_is_resolution_supported(int width, int height);
void vp_display_print_supported_resolutions();

#ifdef __cplusplus
}
#endif /* extern "C" */

#endif // VP_DISPLAY_H_
