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
#include "channel_param_parser.h"
#include "unix_socket.h"

static int running = 0;
extern int vin_isp_is_online;
extern int isp_vse_is_online;

pthread_t g_encode_thread = -1;
pthread_t g_hdmi_thread   = -1;

static struct option const long_options[] = {
	{"sensor", required_argument, NULL, 's'},
	{"channel-type", optional_argument, NULL, 'c'},
	{NULL, 0, NULL, 0}
};

void signal_handle(int signo)
{
	printf("\nReceived signal %d (Ctrl+C), exiting...\n", signo);
	running = 0;

	if (g_encode_thread != -1) {
		pthread_cancel(g_encode_thread);
	}
	if (g_hdmi_thread != -1) {
		pthread_cancel(g_hdmi_thread);
	}

	unlink(SOCKET_PATH_VSE_ENCODE);
	unlink(SOCKET_PATH_VSE_HDMI);

	exit(0);
}

static void print_help(const char *argv0)
{
	printf("Usage: %s [OPTIONS]\n", argv0);
	printf("Options:\n");
	printf("  -s <sensor_index>		Specify sensor index\n");
	printf("  -c <channel_type>		Specify channel type: vo and vf and io and if, default: vf:if\n");
	printf("		Support both individual configuration and combined configuration.\n");
	printf("		The individual configuration supports four types:\n");
	printf("				1. vo: vin online isp\n");
	printf("				2. vf: vin offline isp\n");
	printf("				3. io: isp online vse\n");
	printf("				4. if: isp offline vse\n");
	printf("		The combination configuration supports four types:\n");
	printf("				1. vo:io  vin online isp + isp online vse\n");
	printf("				2. vo:if  vin online isp + isp offline vse\n");
	printf("				3. vf:io  vin offline isp + isp online vse\n");
	printf("				4. vf:if  vin offline isp + isp offline vse\n");
	printf("  -h	Show help message\n");
	vp_show_sensors_list();
}

static int create_camera_node(pipe_contex_t *pipe_contex)
{
	if (!pipe_contex || !pipe_contex->sensor_config) {
		fprintf(stderr, "Invalid pipe_contex or sensor_config\n");
		return -1;
	}

	vp_sensor_config_t *sensor_cfg = pipe_contex->sensor_config;
	camera_config_t *cam_cfg = sensor_cfg->camera_config;

	if (!cam_cfg) {
		fprintf(stderr, "camera_config is NULL\n");
		return -1;
	}

	int32_t ret = hbn_camera_create(cam_cfg, &pipe_contex->cam_fd);
	ERR_CON_EQ(ret, 0);

	return 0;
}

static int create_vin_node(pipe_contex_t *pipe_contex, int active_mipi_host)
{
	vp_sensor_config_t *sensor_config = NULL;
	vin_node_attr_t *vin_node_attr = NULL;
	vin_ichn_attr_t *vin_ichn_attr = NULL;
	vin_ochn_attr_t *vin_ochn_attr = NULL;
	hbn_vnode_handle_t *vin_node_handle = NULL;
	vin_attr_ex_t vin_attr_ex;
	hbn_buf_alloc_attr_t alloc_attr = {0};
	uint32_t hw_id = 0;
	int32_t ret = 0;
	uint32_t ichn_id = 0;
	uint32_t ochn_id = 0;
	uint64_t vin_attr_ex_mask = 0;

	sensor_config = pipe_contex->sensor_config;
	vin_node_attr = sensor_config->vin_node_attr;
	vin_ichn_attr = sensor_config->vin_ichn_attr;
	vin_ochn_attr = sensor_config->vin_ochn_attr;
	vin_node_handle = &pipe_contex->vin_node_handle;

	vin_node_attr->cim_attr.mipi_rx = active_mipi_host;
	hw_id = vin_node_attr->cim_attr.mipi_rx;

	if (pipe_contex->csi_config.mclk_is_not_configed) {
		printf("csi%d ignore mclk ex attr, because not config mclk.\n",
		pipe_contex->csi_config.index);
	} else {
		vin_attr_ex.vin_attr_ex_mask = sensor_config->vin_attr_ex->vin_attr_ex_mask;
		vin_attr_ex.mclk_ex_attr.mclk_freq = sensor_config->vin_attr_ex->mclk_ex_attr.mclk_freq;
		vin_attr_ex_mask = vin_attr_ex.vin_attr_ex_mask;
	}
	if (vin_isp_is_online) { 		/*vin->isp: online mode*/
		sensor_config->vin_node_attr->cim_attr.cim_isp_flyby = 1;
		sensor_config->vin_ochn_attr->ddr_en = 0;
	} else { 				/* vin->isp: offline mode*/
		sensor_config->vin_node_attr->cim_attr.cim_isp_flyby = 0;
		sensor_config->vin_ochn_attr->ddr_en = 1;
	}

	ret = hbn_vnode_open(HB_VIN, hw_id, AUTO_ALLOC_ID, vin_node_handle);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_attr(*vin_node_handle, vin_node_attr);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_ichn_attr(*vin_node_handle, ichn_id, vin_ichn_attr);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_ochn_attr(*vin_node_handle, ochn_id, vin_ochn_attr);
	ERR_CON_EQ(ret, 0);

	if (vin_attr_ex_mask) {
		for (uint8_t i = 0; i < VIN_ATTR_EX_INVALID; i ++) {
			if ((vin_attr_ex_mask & (1 << i)) == 0)
				continue;
			vin_attr_ex.ex_attr_type = i;
			ret = hbn_vnode_set_attr_ex(*vin_node_handle, &vin_attr_ex);
			ERR_CON_EQ(ret, 0);
		}
	}

	if (!vin_isp_is_online) {
		memset(&alloc_attr, 0, sizeof(hbn_buf_alloc_attr_t));
		alloc_attr.buffers_num = 3;
		alloc_attr.is_contig = 1;
		alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN  | \
				   HB_MEM_USAGE_CPU_WRITE_OFTEN | \
				   HB_MEM_USAGE_CACHED;
		ret = hbn_vnode_set_ochn_buf_attr(*vin_node_handle, ochn_id, &alloc_attr);
		ERR_CON_EQ(ret, 0);
	}

	return 0;
}

static int create_isp_node(pipe_contex_t *pipe_contex)
{
	vp_sensor_config_t *sensor_config = NULL;
	isp_attr_t      *isp_attr = NULL;
	isp_ichn_attr_t *isp_ichn_attr = NULL;
	isp_ochn_attr_t *isp_ochn_attr = NULL;
	hbn_vnode_handle_t *isp_node_handle = NULL;
	uint32_t ichn_id = 0;
	uint32_t ochn_id = 0;
	int ret = 0;

	sensor_config = pipe_contex->sensor_config;
	isp_attr = sensor_config->isp_attr;
	isp_ichn_attr = sensor_config->isp_ichn_attr;
	isp_ochn_attr = sensor_config->isp_ochn_attr;
	isp_node_handle = &pipe_contex->isp_node_handle;

	if (vin_isp_is_online) { 		/*vin->isp: online mode*/
		sensor_config->isp_attr->input_mode = PASSTHROUGH_MODE;
	} else { 				/* vin->isp: offline mode*/
		sensor_config->isp_attr->input_mode = DDR_MODE;
	}

	if (isp_vse_is_online) {
		isp_ochn_attr->ddr_en = 0;
	} else {
		isp_ochn_attr->ddr_en = 1;
	}
	ret = hbn_vnode_open(HB_ISP, 0, AUTO_ALLOC_ID, isp_node_handle);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_attr(*isp_node_handle, isp_attr);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_ochn_attr(*isp_node_handle, ochn_id, isp_ochn_attr);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_ichn_attr(*isp_node_handle, ichn_id, isp_ichn_attr);
	ERR_CON_EQ(ret, 0);

	if (!isp_vse_is_online) {
		hbn_buf_alloc_attr_t alloc_attr = {0};
		alloc_attr.buffers_num = 3;
		alloc_attr.is_contig = 1;
		alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN  | \
				   HB_MEM_USAGE_CPU_WRITE_OFTEN | \
				   HB_MEM_USAGE_CACHED;
		ret = hbn_vnode_set_ochn_buf_attr(*isp_node_handle, ochn_id, &alloc_attr);
		ERR_CON_EQ(ret, 0);
	}

	return 0;
}

static int create_vse_node(pipe_contex_t *pipe_contex)
{
	int ret = 0;
	hbn_vnode_handle_t *vse_node_handle = &pipe_contex->vse_node_handle;
	vp_sensor_config_t *sensor_config = pipe_contex->sensor_config;
	isp_attr_t *isp_attr = sensor_config->isp_attr;

	vse_attr_t vse_attr = {0};
	vse_ichn_attr_t vse_ichn_attr = {0};
	vse_ochn_attr_t vse_ochn_attr = {0};
	uint32_t ichn_id = 0;
	uint32_t hw_id = 0;
	uint32_t input_width = isp_attr->crop.w;
	uint32_t input_height = isp_attr->crop.h;
	hbn_buf_alloc_attr_t alloc_attr = {0};

	// 通道0配置
	vse_ichn_attr.width = input_width;
	vse_ichn_attr.height = input_height;
	vse_ichn_attr.fmt = FRM_FMT_NV12;
	vse_ichn_attr.bit_width = 8;

	// 通道0输出配置
	vse_ochn_attr.chn_en = CAM_TRUE;
	vse_ochn_attr.roi.x = 0;
	vse_ochn_attr.roi.y = 0;
	vse_ochn_attr.roi.w = input_width;
	vse_ochn_attr.roi.h = input_height;
	vse_ochn_attr.fmt = FRM_FMT_NV12;
	vse_ochn_attr.bit_width = 8;
	vse_ochn_attr.target_w = input_width;
	vse_ochn_attr.target_h = input_height;

	ret = hbn_vnode_open(HB_VSE, hw_id, AUTO_ALLOC_ID, vse_node_handle);
	ERR_CON_EQ(ret, 0);

	ret = hbn_vnode_set_attr(*vse_node_handle, &vse_attr);
	ERR_CON_EQ(ret, 0);

	ret = hbn_vnode_set_ichn_attr(*vse_node_handle, ichn_id, &vse_ichn_attr);
	ERR_CON_EQ(ret, 0);

	// 配置通道0缓冲区
	alloc_attr.buffers_num = 6;
	alloc_attr.is_contig = 1;
	alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN  | \
			   HB_MEM_USAGE_CPU_WRITE_OFTEN | \
			   HB_MEM_USAGE_GRAPHIC_CONTIGUOUS_BUF;

	printf("hbn_vnode_set_ochn_attr channel 0: %dx%d\n", vse_ochn_attr.target_w, vse_ochn_attr.target_h);
	ret = hbn_vnode_set_ochn_attr(*vse_node_handle, 0, &vse_ochn_attr);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_ochn_buf_attr(*vse_node_handle, 0, &alloc_attr);
	ERR_CON_EQ(ret, 0);

	// 通道1配置
	vse_ochn_attr.chn_en = CAM_TRUE;
	vse_ochn_attr.roi.x = 0;
	vse_ochn_attr.roi.y = 0;
	vse_ochn_attr.roi.w = input_width;
	vse_ochn_attr.roi.h = input_height;
	vse_ochn_attr.target_w = input_width;
	vse_ochn_attr.target_h = input_height;

	printf("hbn_vnode_set_ochn_attr channel 1: %dx%d\n", vse_ochn_attr.target_w, vse_ochn_attr.target_h);
	ret = hbn_vnode_set_ochn_attr(*vse_node_handle, 1, &vse_ochn_attr);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_ochn_buf_attr(*vse_node_handle, 1, &alloc_attr);
	ERR_CON_EQ(ret, 0);

	return 0;
}

static int create_and_run_vflow(pipe_contex_t *pipe_contex, int active_mipi_host)
{
	int32_t ret = 0;

	ret = create_camera_node(pipe_contex);
	ERR_CON_EQ(ret, 0);
	ret = create_vin_node(pipe_contex, active_mipi_host);
	ERR_CON_EQ(ret, 0);
	ret = create_isp_node(pipe_contex);
	ERR_CON_EQ(ret, 0);
	ret = create_vse_node(pipe_contex);
	ERR_CON_EQ(ret, 0);

	// 创建HBN flow
	ret = hbn_vflow_create(&pipe_contex->vflow_fd);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_add_vnode(pipe_contex->vflow_fd, pipe_contex->vin_node_handle);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_add_vnode(pipe_contex->vflow_fd, pipe_contex->isp_node_handle);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_add_vnode(pipe_contex->vflow_fd, pipe_contex->vse_node_handle);
	ERR_CON_EQ(ret, 0);

	// 绑定VIN->ISP
	if (vin_isp_is_online) {
		ret = hbn_vflow_bind_vnode(pipe_contex->vflow_fd,
		pipe_contex->vin_node_handle, 1,
		pipe_contex->isp_node_handle, 0);
	} else {
		ret = hbn_vflow_bind_vnode(pipe_contex->vflow_fd,
		pipe_contex->vin_node_handle, 0,
		pipe_contex->isp_node_handle, 0);
	}
	ERR_CON_EQ(ret, 0);

	// 绑定ISP->VSE
	if (isp_vse_is_online) {
		ret = hbn_vflow_bind_vnode(pipe_contex->vflow_fd,
		pipe_contex->isp_node_handle, 1,
		pipe_contex->vse_node_handle, 0);
	} else {
		ret = hbn_vflow_bind_vnode(pipe_contex->vflow_fd,
		pipe_contex->isp_node_handle, 0,
		pipe_contex->vse_node_handle, 0);
	}
	ERR_CON_EQ(ret, 0);

	ret = hbn_camera_attach_to_vin(pipe_contex->cam_fd, pipe_contex->vin_node_handle);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_start(pipe_contex->vflow_fd);
	ERR_CON_EQ(ret, 0);

	return 0;
}

void *vse_to_encode_thread(void *arg)
{
	pipe_contex_t *pipe_context = (pipe_contex_t *)arg;
	hbn_vnode_handle_t vse_node_handle = pipe_context->vse_node_handle;
	int sock_fd = -1, client_fd = -1;
	int ret = 0;

	sock_fd = create_unix_socket(SOCKET_PATH_VSE_ENCODE);
	if (sock_fd < 0) {
		printf("Create encode socket failed\n");
		return NULL;
	}

	printf("Wait encode process connect...\n");
	client_fd = accept_unix_client(sock_fd);
	if (client_fd < 0) {
		close_unix_socket(sock_fd, SOCKET_PATH_VSE_ENCODE);
		return NULL;
	}
	printf("Encode process connected\n");

	while (running) {
		hbn_vnode_image_t out_img = {0};
		frame_meta_t meta = {0};
		uint32_t release_frame_id = 0;

		ret = hbn_vnode_getframe(vse_node_handle, 1, 1000, &out_img);
		if (ret != 0) {
			printf("hbn_vnode_getframe VSE channel 1 failed\n");
			usleep(10000);
			continue;
		}

		for (int j = 0; j < 2; ++j) {
			hb_mem_invalidate_buf_with_vaddr((uint64_t)out_img.buffer.virt_addr[j], out_img.buffer.size[j]);
		}

		meta.msg_type = MSG_TYPE_FRAME;
		meta.frame_id = out_img.info.frame_id;

		memcpy((void *)&meta.img_buf, (void *)&out_img.buffer, sizeof(hb_mem_graphic_buf_t));

		ret = send_frame_pkg(client_fd, &meta);
		if (ret != 0) {
			printf("Send frame to encode failed\n");
			hbn_vnode_releaseframe(vse_node_handle, 1, &out_img);
			continue;
		}

		ret = recv_release_pkg(client_fd, &release_frame_id);
		if (ret != 0 || release_frame_id != out_img.info.frame_id) {
			printf("Recv release from encode failed\n");
		}

		hbn_vnode_releaseframe(vse_node_handle, 1, &out_img);
	}

	close(client_fd);
	close_unix_socket(sock_fd, SOCKET_PATH_VSE_ENCODE);

	return NULL;
}

void *vse_to_hdmi_thread(void *arg)
{
	pipe_contex_t *pipe_context = (pipe_contex_t *)arg;
	hbn_vnode_handle_t vse_node_handle = pipe_context->vse_node_handle;
	int sock_fd = -1, client_fd = -1;
	int ret = 0;

	sock_fd = create_unix_socket(SOCKET_PATH_VSE_HDMI);
	if (sock_fd < 0) {
		printf("Create hdmi socket failed\n");
		return NULL;
	}

	printf("Wait HDMI process connect...\n");
	client_fd = accept_unix_client(sock_fd);
	if (client_fd < 0) {
		close_unix_socket(sock_fd, SOCKET_PATH_VSE_HDMI);
		return NULL;
	}
	printf("HDMI process connected\n");

	while (running) {
		hbn_vnode_image_t out_img = {0};
		frame_meta_t meta = {0};
		uint32_t release_frame_id = 0;

		ret = hbn_vnode_getframe(vse_node_handle, 0, 1000, &out_img);
		if (ret != 0) {
			printf("hbn_vnode_getframe VSE channel 0 failed\n");
			usleep(10000);
			continue;
		}

		for (int j = 0; j < 2; ++j) {
			hb_mem_invalidate_buf_with_vaddr((uint64_t)out_img.buffer.virt_addr[j], out_img.buffer.size[j]);
		}

		meta.msg_type = MSG_TYPE_FRAME;
		meta.frame_id = out_img.info.frame_id;

		memcpy((void *)&meta.img_buf, (void *)&out_img.buffer, sizeof(hb_mem_graphic_buf_t));

		ret = send_frame_pkg(client_fd, &meta);
		if (ret != 0) {
			printf("Send frame to HDMI failed\n");
			hbn_vnode_releaseframe(vse_node_handle, 0, &out_img);
			continue;
		}

		ret = recv_release_pkg(client_fd, &release_frame_id);
		if (ret != 0 || release_frame_id != out_img.info.frame_id) {
			printf("Recv release from HDMI failed\n");
		}

		hbn_vnode_releaseframe(vse_node_handle, 0, &out_img);
	}

	close(client_fd);
	close_unix_socket(sock_fd, SOCKET_PATH_VSE_HDMI);

	return NULL;
}

int main(int argc, char** argv)
{
	int ret = 0;
	pipe_contex_t pipe_contex = {0};
	int opt_index = 0;
	int c = 0;
	int index = -1;
	int active_mipi_host;

	while ((c = getopt_long(argc, argv, "s:c:h", long_options, &opt_index)) != -1) {
		switch (c) {
		case 's':
			index = atoi(optarg);
			break;
		case 'c':
			if (parse_channel_string(optarg) != 0) {
			printf("Invalid channel type %s.\n", optarg);
			return -1;
			}
			break;
		case 'h':
		default:
			print_help(argv[0]);
			return 0;
		}
	}

	// 检查sensor索引
	if (index < vp_get_sensors_list_number() && index >= 0) {
		pipe_contex.sensor_config = vp_sensor_config_list[index];
		printf("Using index:%d  sensor_name:%s  config_file:%s\n",
			index,
			vp_sensor_config_list[index]->sensor_name,
			vp_sensor_config_list[index]->config_file);
		ret = vp_sensor_fixed_mipi_host(pipe_contex.sensor_config, &pipe_contex.csi_config);
		if (ret != 0) {
			printf("No Camera Sensor found\n");
			return ret;
		}
		active_mipi_host = pipe_contex.sensor_config->vin_node_attr->cim_attr.mipi_rx;
	} else {
		printf("Unsupport sensor index:%d\n", index);
		print_help(argv[0]);
		return 0;
	}

	// 检查DOL2模式
	if ((pipe_contex.sensor_config->camera_config->sensor_mode == DOL2_M) && \
	   (vin_isp_is_online == 0)) {
		printf("Error: sensor_mode is DOL2_M, must work in online mode\n");
		return -1;
	}

	printf("VIN->ISP: %s\n", vin_isp_is_online ? "online" : "offline");
	printf("ISP->VSE: %s\n", isp_vse_is_online ? "online" : "offline");

	signal(SIGINT,  signal_handle); // Ctrl+C
	signal(SIGTERM, signal_handle); // kill

	hb_mem_module_open();

	ret = create_and_run_vflow(&pipe_contex, active_mipi_host);
	ERR_CON_EQ(ret, 0);

	running = 1;
	ret = pthread_create(&g_encode_thread, NULL, vse_to_encode_thread, &pipe_contex);
	if (ret != 0) {
		printf("Create encode thread failed\n");
		goto cleanup_process;
	}

	ret = pthread_create(&g_hdmi_thread, NULL, vse_to_hdmi_thread, &pipe_contex);
	if (ret != 0) {
		printf("Create hdmi thread failed\n");
		pthread_cancel(g_encode_thread);
		pthread_join(g_encode_thread, NULL);
		goto cleanup_process;
	}

	pthread_join(g_encode_thread, NULL);
	pthread_join(g_hdmi_thread, NULL);

cleanup_process:
	running = 0;

	ret = hbn_vflow_stop(pipe_contex.vflow_fd);
	ERR_CON_EQ(ret, 0);
	hbn_vnode_close(pipe_contex.vse_node_handle);
	hbn_vnode_close(pipe_contex.isp_node_handle);
	hbn_vnode_close(pipe_contex.vin_node_handle);
	hbn_camera_destroy(pipe_contex.cam_fd);
	hbn_vflow_destroy(pipe_contex.vflow_fd);
	hb_mem_module_close();

	unlink(SOCKET_PATH_VSE_ENCODE);
	unlink(SOCKET_PATH_VSE_HDMI);

	return 0;
}