#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

// DRM_MODE_CONNECTOR_HDMIA (11)
// DRM_MODE_CONNECTOR_DSI   (16)

int get_connector_id(int connector_type)
{
	int id = 0;

	int fd = open("/dev/dri/card0", O_RDWR);
	if (fd < 0) {
		perror("open /dev/dri/card0 failed");
		return -1;
	}

	drmModeRes *res = drmModeGetResources(fd);
	if (!res) {
		perror("drmModeGetResources failed");
		close(fd);
		return -1;
	}

	if (connector_type == DRM_MODE_CONNECTOR_HDMIA) {
		printf("search hdmi connector: ");
	} else if (connector_type == DRM_MODE_CONNECTOR_DSI) {
		printf("search dsi  connector: ");
	}

	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[i]);

		if (!conn)
			continue;

		if (conn->connector_type == connector_type) {
			printf("connector id is %d\n", conn->connector_id);
			id = conn->connector_id;
			drmModeFreeConnector(conn);
			break;
		}

		drmModeFreeConnector(conn);
	}

	drmModeFreeResources(res);
	close(fd);

	return id;
}