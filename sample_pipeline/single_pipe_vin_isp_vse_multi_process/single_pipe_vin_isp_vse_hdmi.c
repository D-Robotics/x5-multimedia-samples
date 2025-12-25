/***************************************************************************
 *                      COPYRIGHT NOTICE
 *             Copyright(C) 2025, D-Robotics Co., Ltd.
 *                     All rights reserved.
 ***************************************************************************/

#include <stdio.h>
#include <getopt.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <signal.h>

#include "common_utils.h"
#include "vp_display.h"
#include "unix_socket.h"

static int running = 0;
static vp_drm_context_t vp_drm_context;

int hdmi_display_init(int width, int height)
{
	int ret;
	int hdmi_output_width, hdmi_output_height;

	// 检查HDMI连接
	ret = vp_display_check_hdmi_is_connected();
	if(ret == 0) {
		printf("HDMI not connected\n");
		return -1;
	}

	// 检查分辨率
	ret = vp_display_get_max_resolution_if_not_match(width, height, &hdmi_output_width, &hdmi_output_height);
	if (ret < 0) {
		printf("HDMI no appropriate resolution for %dx%d\n", width, height);
		return -1;
	}

	// 初始化HDMI
	ret = vp_display_init(&vp_drm_context, hdmi_output_width, hdmi_output_height);
	if (ret != 0) {
		printf("HDMI init failed\n");
		return -1;
	}

	printf("HDMI init ok: %dx%d (match VSE output)\n", hdmi_output_width, hdmi_output_height);
	return 0;
}

void hdmi_display_deinit()
{
	vp_display_deinit(&vp_drm_context);
}

void hdmi_process_main()
{
	int sock_fd = -1;
	frame_meta_t meta = {0};
	int ret = 0;

	hb_mem_module_open();

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, SOCKET_PATH_VSE_HDMI, sizeof(addr.sun_path)-1);

	sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock_fd < 0) {
		perror("HDMI socket create failed");
		exit(-1);
	}

	int retry = 10;
	while (retry-- > 0) {
		if (connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
			break;
		}
		usleep(100000);
	}
	if (retry < 0) {
		perror("HDMI connect to VSE failed, please start VSE process first");
		close(sock_fd);
		exit(-1);
	}
	printf("HDMI process connected to VSE\n");

	ret = recv_frame_pkg(sock_fd, &meta);
	if (ret != 0) {
		printf("HDMI recv first frame failed\n");
		close(sock_fd);
		exit(-1);
	}

	if (hdmi_display_init(meta.img_buf.width, meta.img_buf.height) < 0) { 
		close(sock_fd);
		exit(-1);
	}

	ret = send_release_pkg(sock_fd, meta.frame_id);
	if (ret != 0) {
		printf("HDMI send first release failed\n");
	}

	hb_mem_graphic_buf_t display_buf = {0};
	hb_mem_import_graph_buf(&meta.img_buf, &display_buf);

	running = 1;
	while (running) {
		ret = recv_frame_pkg(sock_fd, &meta);
		if (ret != 0) {
			printf("HDMI recv frame failed\n");
			usleep(10000);
			continue;
		}

		ret = vp_display_set_frame(&vp_drm_context, &display_buf);
		if(ret != 0){
			printf("HDMI display frame failed %d (frame id: %d)\n", ret, meta.frame_id);
		} else {
			printf("HDMI display frame success: id=%d, resolution=%dx%d\n", meta.frame_id, meta.img_buf.width, meta.img_buf.height);
		}

		ret = send_release_pkg(sock_fd, meta.frame_id);
		if (ret != 0) {
			printf("HDMI send release failed\n");
		}
	}

	hdmi_display_deinit();
	close(sock_fd);
	unlink(SOCKET_PATH_VSE_HDMI);
	hb_mem_module_close();
	exit(0);
}

void hdmi_signal_handle(int signo)
{
	printf("HDMI process received signal %d, exiting...\n", signo);
	running = 0;
	hdmi_display_deinit();

	unlink(SOCKET_PATH_VSE_ENCODE);

	exit(0);
}

int main()
{
	signal(SIGINT, hdmi_signal_handle);
	signal(SIGTERM, hdmi_signal_handle);

	hdmi_process_main();
	return 0;
}