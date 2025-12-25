/***************************************************************************
 *                      COPYRIGHT NOTICE
 *             Copyright(C) 2025, D-Robotics Co., Ltd.
 *                     All rights reserved.
 ***************************************************************************/

#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "common_utils.h"
#include "unix_socket.h"

int create_unix_socket(const char *path)
{
	int sock_fd = -1;
	struct sockaddr_un addr;

	unlink(path);

	sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock_fd < 0) {
		perror("Create socket failed");
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path)-1);

	if (bind(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("Bind socket failed");
		close(sock_fd);
		return -1;
	}

	if (listen(sock_fd, 1) < 0) {
		perror("Listen socket failed");
		close(sock_fd);
		return -1;
	}

    return sock_fd;
}

int accept_unix_client(int sock_fd)
{
	struct sockaddr_un client_addr;
	socklen_t len = sizeof(client_addr);
	int client_fd = accept(sock_fd, (struct sockaddr *)&client_addr, &len);
	if (client_fd < 0) {
		perror("Accept client failed");
		return -1;
	}
	return client_fd;
}

int send_frame_pkg(int fd, const frame_meta_t *meta)
{
	socket_pkg_t pkg;
	memset(&pkg, 0, sizeof(pkg));

	pkg.header = FRAME_HEADER;
	pkg.data_len = sizeof(frame_meta_t);
	memcpy(&pkg.meta, meta, sizeof(frame_meta_t));
	pkg.tail = FRAME_TAIL;

	int ret = send(fd, &pkg, sizeof(pkg), 0);
	if (ret != sizeof(pkg)) {
		perror("Send frame pkg failed");
		return -1;
	}
	return 0;
}

int recv_frame_pkg(int fd, frame_meta_t *meta)
{
	socket_pkg_t pkg;
	memset(&pkg, 0, sizeof(pkg));

	int ret = recv(fd, &pkg, sizeof(pkg), 0);
	if (ret != sizeof(pkg) || pkg.header != FRAME_HEADER || pkg.tail != FRAME_TAIL) {
		perror("Recv frame pkg failed");
		return -1;
	}

	memcpy(meta, &pkg.meta, sizeof(frame_meta_t));
	return 0;
}

int send_release_pkg(int fd, uint32_t frame_id)
{
	socket_pkg_t pkg;
	memset(&pkg, 0, sizeof(pkg));

	pkg.header = FRAME_HEADER;
	pkg.data_len = sizeof(frame_meta_t);
	pkg.meta.msg_type = MSG_TYPE_RELEASE;
	pkg.meta.frame_id = frame_id;
	pkg.tail = FRAME_TAIL;

	int ret = send(fd, &pkg, sizeof(pkg), 0);
	if (ret != sizeof(pkg)) {
		perror("Send release pkg failed");
		return -1;
	}
	return 0;
}

int recv_release_pkg(int fd, uint32_t *frame_id)
{
	socket_pkg_t pkg;
	memset(&pkg, 0, sizeof(pkg));

	int ret = recv(fd, &pkg, sizeof(pkg), 0);
	if (ret != sizeof(pkg) || pkg.header != FRAME_HEADER || pkg.tail != FRAME_TAIL ||
		pkg.meta.msg_type != MSG_TYPE_RELEASE) {
		perror("Recv release pkg failed");
		return -1;
	}

	*frame_id = pkg.meta.frame_id;
	return 0;
}

void close_unix_socket(int fd, const char *path)
{
	close(fd);
	unlink(path);
}
