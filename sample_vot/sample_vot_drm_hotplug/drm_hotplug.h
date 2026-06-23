/***************************************************************************
 *                      COPYRIGHT NOTICE
 *             Copyright(C) 2026, D-Robotics Co., Ltd.
 *                     All rights reserved.
 ***************************************************************************/

#ifndef DRM_HOTPLUG_H
#define DRM_HOTPLUG_H

#include <libudev.h>
#include <xf86drmMode.h>

/* 初始化 udev monitor，只监听 DRM 子系统
 * 成功返回 monitor 对应的 fd，失败返回 < 0
 */
int setup_udev_drm_monitor(struct udev **udev_out, struct udev_monitor **mon_out);

/* 处理一次 udev hotplug 事件，并根据当前 connector 状态判断插入/拔出
 *
 * 参数：
 *   mon            - 已初始化的 udev_monitor
 *   drm_fd         - 当前使用的 DRM 设备 fd
 *   connector_id   - 关心的 DRM connector id
 *   connector_type - 仅用于日志打印（如 HDMI、DSI）
 *
 * 返回值：
 *   1  - 当前 connector 状态为 CONNECTED（插入）
 *   0  - 当前 connector 状态为 DISCONNECTED（拔出）
 *  -1  - 其它情况/无法判定或不是我们关心的 hotplug 事件
 */
int handle_udev_drm_hotplug(struct udev_monitor *mon, int drm_fd, uint32_t connector_id, int connector_type);

#endif	// DRM_HOTPLUG_H
