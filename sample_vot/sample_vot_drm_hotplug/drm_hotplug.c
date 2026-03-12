/***************************************************************************
 *                      COPYRIGHT NOTICE
 *             Copyright(C) 2026, D-Robotics Co., Ltd.
 *                     All rights reserved.
 ***************************************************************************/

#include "drm_hotplug.h"

#include <stdio.h>
#include <string.h>

int setup_udev_drm_monitor(struct udev **udev_out, struct udev_monitor **mon_out)
{
	struct udev *udev = NULL;
	struct udev_monitor *mon = NULL;

	if (!udev_out || !mon_out)
		return -1;

	*udev_out = NULL;
	*mon_out = NULL;

	udev = udev_new();
	if (!udev) {
		fprintf(stderr, "udev_new failed\n");
		return -1;
	}

	mon = udev_monitor_new_from_netlink(udev, "udev");
	if (!mon) {
		fprintf(stderr, "udev_monitor_new_from_netlink failed\n");
		udev_unref(udev);
		return -1;
	}

	if (udev_monitor_filter_add_match_subsystem_devtype(mon, "drm", NULL) < 0) {
		fprintf(stderr, "udev_monitor_filter_add_match_subsystem_devtype failed\n");
		udev_monitor_unref(mon);
		udev_unref(udev);
		return -1;
	}

	if (udev_monitor_enable_receiving(mon) < 0) {
		fprintf(stderr, "udev_monitor_enable_receiving failed\n");
		udev_monitor_unref(mon);
		udev_unref(udev);
		return -1;
	}

	*udev_out = udev;
	*mon_out = mon;

	return udev_monitor_get_fd(mon);
}

int handle_udev_drm_hotplug(struct udev_monitor *mon, int drm_fd, uint32_t connector_id, int connector_type)
{
	struct udev_device *dev;

	if (!mon)
		return -1;

	dev = udev_monitor_receive_device(mon);
	if (!dev)
		return -1;

	const char *action = udev_device_get_action(dev);
	const char *subsys = udev_device_get_subsystem(dev);
	const char *hotplug = udev_device_get_property_value(dev, "HOTPLUG");
	const char *devnode = udev_device_get_devnode(dev);

	int result = -1;

	if (action && subsys && hotplug && strcmp(subsys, "drm") == 0 && strcmp(action, "change") == 0
	    && strcmp(hotplug, "1") == 0) {
		printf("DRM hotplug event detected on %s (connector_type=%d)\n", devnode ? devnode : "unknown",
		       connector_type);

		/* 通过 libdrm 查询当前 connector 状态来判断是插入还是拔出 */
		drmModeConnector *conn = drmModeGetConnector(drm_fd, connector_id);
		if (conn) {
			if (conn->connection == DRM_MODE_CONNECTED) {
				printf("DRM connector is now CONNECTED (id=%u)\n", connector_id);
				result = 1;
			} else if (conn->connection == DRM_MODE_DISCONNECTED) {
				printf("DRM connector is now DISCONNECTED (id=%u)\n", connector_id);
				result = 0;
			} else {
				printf("DRM connector state changed, connection=%d (id=%u)\n", conn->connection,
				       connector_id);
			}
			drmModeFreeConnector(conn);
		}
	}

	udev_device_unref(dev);
	return result;
}
