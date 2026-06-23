/***************************************************************************
 * @COPYRIGHT NOTICE
 * @Copyright 2024 D-Robotics, Inc.
 * @All rights reserved.
 * @Date: 2023-03-05 15:28:19
 * @LastEditTime: 2023-03-05 16:31:05
 ***************************************************************************/
#ifndef VP_DISPLAY_H_
#define VP_DISPLAY_H_

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "uthash.h"
#include "vp_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DRM_MAX_PLANES 3
#define DRM_ION_MAX_BUFFERS 10

typedef struct {
	int dma_buf_fd;
	uint32_t fb_id;
	UT_hash_handle hh;  // uthash 处理器
} dma_buf_map_t;

typedef struct {
	uint32_t plane_id;
	uint32_t src_w;
	uint32_t src_h;
	uint32_t crtc_x;
	uint32_t crtc_y;
	uint32_t crtc_w;
	uint32_t crtc_h;
	uint32_t z_pos;
	uint32_t alpha;
	char format[8];		    // 图层格式
	uint32_t rotation;	    // 旋转属性
	uint32_t color_encoding;    // 颜色编码
	uint32_t color_range;	    // 颜色范围
	uint32_t pixel_blend_mode;  // alpha模式
} drm_plane_config_t;

typedef struct {
	int drm_fd;
	uint32_t crtc_id;
	uint32_t connector_id;
	drm_plane_config_t planes[DRM_MAX_PLANES];
	uint32_t width;
	uint32_t height;
	int32_t target_refresh_hz;
	int plane_count;
	dma_buf_map_t *buffer_map;  // 使用哈希表
	int buffer_count;
	int max_buffers;  // 动态调整 buffer_map 的大小

	/* 已提交 PAGE_FLIP_EVENT，等待 drmHandleEvent 收到 flip 后再送下一帧（与 sample_vot_async 主循环一致） */
	volatile int flip_pending;
} vp_drm_context_t;

int32_t vp_display_init(vp_drm_context_t *drm_ctx, int32_t width, int32_t height, int32_t target_refresh_hz);
int32_t vp_display_deinit(vp_drm_context_t *drm_ctx);
int32_t vp_display_set_frame(vp_drm_context_t *drm_ctx, hbn_vnode_image_t *image_frame);
int32_t vp_display_handle_drm_event(vp_drm_context_t *drm_ctx);
int32_t vp_display_check_hdmi_is_connected();

typedef void (*get_connector_info_cb_t)(void *handle, int width, int height, float fps, int is_interleave);
int vp_display_get_connector_info(void *handle, get_connector_info_cb_t cb);

/* 热插拔监控（线程内使用，vp_display 内部管理 udev 资源） */
int vp_display_hotplug_init(void); /* 返回 udev monitor fd（供 poll 用），失败返回 -1 */
void vp_display_hotplug_deinit(void);
int vp_display_hotplug_process(void); /* udev fd 可读时调用；返回 1=连接 0=断开 -1=无变化 */

/*
 * 一次 poll 同时监听 udev 热插拔 + drm flip + timerfd（约每秒读一次 HDMI 硬件状态，应对无可靠 udev 热插拔）。
 *   drm_ctx        : DRM 上下文（flip 事件处理）
 *   display_ready  : 当前是否已 init（决定是否监听 drm_fd）
 *   prev_connected : 上一轮的连接状态
 *   timeout_ms     : poll 超时（毫秒）
 *   返回值          : 最新 connected 状态（udev 事件或轮询会更新）
 */
int vp_display_poll_events(vp_drm_context_t *drm_ctx, int display_ready, int prev_connected, int timeout_ms);

#ifdef __cplusplus
}
#endif /* extern "C" */

#endif	// VP_DISPLAY_H_
