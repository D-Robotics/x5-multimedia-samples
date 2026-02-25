#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

int get_hdmi_connector_id()
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

	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[i]);
		if (!conn)
			continue;

		if (conn->connector_type == 11) {
			printf("hdmi connector id is %d\n", conn->connector_id);
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