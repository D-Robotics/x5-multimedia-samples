/***************************************************************************
 *                      COPYRIGHT NOTICE
 *             Copyright(C) 2024, D-Robotics Co., Ltd.
 *                     All rights reserved.
 ***************************************************************************/

#include <stdio.h>
#include <getopt.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <string.h>
#include <pthread.h>

#include "common_utils.h"
#include "hb_media_codec.h"
#include "hb_media_error.h"

#define PIPES_TOTAL 2

typedef struct hbn_cfg {
	vin_attr_t vin_attr;
	isp_cfg_t isp_attr;
	vse_cfg_t vse_attr;
	codec_cfg_t codec_attr;
} hbn_cfg_t;

int32_t running = 0;
int32_t codec_bind_vse = 0;
camera_config_t g_camera_config[PIPES_TOTAL];
deserial_config_t g_deserial_config[PIPES_TOTAL];
mipi_config_t g_mipi_config[PIPES_TOTAL];
hbn_cfg_t g_hbn_cfg[PIPES_TOTAL];
mipi_host_cfg_t g_mipi_host_cfg[PIPES_TOTAL];
hbn_vflow_handle_t g_vflow_fd[PIPES_TOTAL] = {0};
char dst_file[128];
FILE *fout[PIPES_TOTAL] = {0};
media_codec_buffer_t outputBuffer[PIPES_TOTAL];
media_codec_output_buffer_info_t codec_outinfo[PIPES_TOTAL];
media_codec_context_t media_context[PIPES_TOTAL] = {0};
int64_t g_cam_fd[PIPES_TOTAL] = {-1};


int create_and_run_vflow(int pipe_index);
void *read_vse_data(void *contex);
void *read_codec_data(void *arg);
static int codec_init(int pipe_index);
static int codec_start(int pipe_index);

static void print_help() {
}

void multi_handle(int signo) {
	running = 0;
}


void common_attr_config() {
	/* cim attr config g_hbn_cfg[0] is sc1330t, g_hbn_cfg[1] is sc230ai*/
	g_hbn_cfg[0].vin_attr.vin_ichn_attr.width = 1280;
	g_hbn_cfg[0].vin_attr.vin_ichn_attr.height = 960;
	g_hbn_cfg[0].vin_attr.vin_ichn_attr.format = 0x2b;
	g_hbn_cfg[0].vin_attr.vin_node_attr.cim_attr.ipi_channel = 1;
	g_hbn_cfg[0].vin_attr.vin_node_attr.cim_attr.func.enable_frame_id = 1;
	g_hbn_cfg[0].vin_attr.vin_node_attr.cim_attr.func.set_init_frame_id = 1;
	g_hbn_cfg[0].vin_attr.vin_node_attr.cim_attr.func.enable_pattern = 0;
	g_hbn_cfg[0].vin_attr.vin_node_attr.cim_attr.mipi_en = 1;
	g_hbn_cfg[0].vin_attr.vin_node_attr.cim_attr.cim_isp_flyby = 1;
	g_hbn_cfg[0].vin_attr.vin_node_attr.cim_attr.vc_index = 0;
	g_hbn_cfg[0].vin_attr.vin_node_attr.cim_attr.mipi_rx = 0;
	g_hbn_cfg[0].vin_attr.vin_ochn_attr[VIN_MAIN_FRAME].ddr_en = 1;
	g_hbn_cfg[0].vin_attr.vin_ochn_attr[VIN_MAIN_FRAME].vin_basic_attr.format = 0x2b;
	g_hbn_cfg[0].vin_attr.vin_ochn_attr[VIN_MAIN_FRAME].vin_basic_attr.wstride = 2560;
	g_hbn_cfg[0].vin_attr.vin_ochn_attr[VIN_MAIN_FRAME].vin_basic_attr.pack_mode = 1;
	g_hbn_cfg[0].vin_attr.vin_ochn_buff_attr[VIN_MAIN_FRAME].buffers_num = 6;
	g_hbn_cfg[0].vin_attr.vin_ochn_attr[VIN_MAIN_FRAME].roi_en = 0;
	g_hbn_cfg[0].vin_attr.vin_ochn_attr[VIN_MAIN_FRAME].roi_attr.roi_x = 0;
	g_hbn_cfg[0].vin_attr.vin_ochn_attr[VIN_MAIN_FRAME].roi_attr.roi_y = 0;
	g_hbn_cfg[0].vin_attr.vin_ochn_attr[VIN_MAIN_FRAME].roi_attr.roi_width = 1280;
	g_hbn_cfg[0].vin_attr.vin_ochn_attr[VIN_MAIN_FRAME].roi_attr.roi_height = 960;
	g_hbn_cfg[0].vin_attr.vin_attr_ex.ex_attr_type = VIN_STATIC_MCLK_ATTR;
	g_hbn_cfg[0].vin_attr.vin_attr_ex.mclk_ex_attr.mclk_freq = 24000000;
	if (g_hbn_cfg[0].vin_attr.vin_ichn_attr.width > 8192)
		printf("x5 vin_ichn_attr setting error, vin_ichn_attr width value should from 0 to 8192\n");
	if (g_hbn_cfg[0].vin_attr.vin_ichn_attr.height > 4096)
		printf("x5 vin_ichn_attr setting error, vin_ichn_attr height value should from 0 to 4096\n");

	/* isp attr config */
	g_hbn_cfg[0].isp_attr.isp_attr.input_mode = 1;
	g_hbn_cfg[0].isp_attr.isp_attr.sched_mode = 0;
	g_hbn_cfg[0].isp_attr.isp_attr.crop.x = 0;
	g_hbn_cfg[0].isp_attr.isp_attr.crop.y = 0;
	g_hbn_cfg[0].isp_attr.isp_attr.crop.w = 1280;
	g_hbn_cfg[0].isp_attr.isp_attr.crop.h = 960;
	g_hbn_cfg[0].isp_attr.ichn_attr.tpg_en = CAM_FALSE;
	g_hbn_cfg[0].isp_attr.ichn_attr.width = 1280;
	g_hbn_cfg[0].isp_attr.ichn_attr.height = 960;
	g_hbn_cfg[0].isp_attr.ichn_attr.fmt = FRM_FMT_RAW;
	g_hbn_cfg[0].isp_attr.ichn_attr.bit_width = 10;
	g_hbn_cfg[0].isp_attr.ochn_attr.ddr_en = CAM_TRUE;
	g_hbn_cfg[0].isp_attr.ochn_attr.fmt = FRM_FMT_NV12;
	g_hbn_cfg[0].isp_attr.ochn_attr.bit_width = 8;
	g_hbn_cfg[0].isp_attr.isp_attr.sensor_mode = ISP_NORMAL_M;

	/* vse attr config */
	//g_hbn_cfg[0].vse_attr.vse_attr.fps.src = 30;
	//g_hbn_cfg[0].vse_attr.vse_attr.fps.dst = 30;

	g_hbn_cfg[0].vse_attr.ichn_attr.tpg_en = CAM_FALSE;
	g_hbn_cfg[0].vse_attr.ichn_attr.width = 1280;
	g_hbn_cfg[0].vse_attr.ichn_attr.height = 960;
	g_hbn_cfg[0].vse_attr.ichn_attr.fmt = FRM_FMT_NV12;
	g_hbn_cfg[0].vse_attr.ichn_attr.bit_width = 8;

	g_hbn_cfg[0].vse_attr.ochn_attr[0].chn_en = CAM_TRUE;
	g_hbn_cfg[0].vse_attr.ochn_attr[0].target_w = 640;
	g_hbn_cfg[0].vse_attr.ochn_attr[0].target_h = 480;
	g_hbn_cfg[0].vse_attr.ochn_attr[0].fmt = FRM_FMT_NV12;
	g_hbn_cfg[0].vse_attr.ochn_attr[0].bit_width = 8;
	g_hbn_cfg[0].vse_attr.ochn_attr[0].roi.x = 0;
	g_hbn_cfg[0].vse_attr.ochn_attr[0].roi.y = 0;
	g_hbn_cfg[0].vse_attr.ochn_attr[0].roi.w = 1280;
	g_hbn_cfg[0].vse_attr.ochn_attr[0].roi.h = 960;

	g_hbn_cfg[0].vse_attr.ochn_attr[1].chn_en = CAM_TRUE;
	g_hbn_cfg[0].vse_attr.ochn_attr[1].target_w = 640;
	g_hbn_cfg[0].vse_attr.ochn_attr[1].target_h = 480;
	g_hbn_cfg[0].vse_attr.ochn_attr[1].fmt = FRM_FMT_NV12;
	g_hbn_cfg[0].vse_attr.ochn_attr[1].bit_width = 8;
	g_hbn_cfg[0].vse_attr.ochn_attr[1].roi.x = 0;
	g_hbn_cfg[0].vse_attr.ochn_attr[1].roi.y = 0;
	g_hbn_cfg[0].vse_attr.ochn_attr[1].roi.w = 1280;
	g_hbn_cfg[0].vse_attr.ochn_attr[1].roi.h = 960;

	g_hbn_cfg[0].vse_attr.ochn_attr[2].chn_en = CAM_TRUE;
	g_hbn_cfg[0].vse_attr.ochn_attr[2].target_w = 640;
	g_hbn_cfg[0].vse_attr.ochn_attr[2].target_h = 480;
	g_hbn_cfg[0].vse_attr.ochn_attr[2].fmt = FRM_FMT_NV12;
	g_hbn_cfg[0].vse_attr.ochn_attr[2].bit_width = 8;
	g_hbn_cfg[0].vse_attr.ochn_attr[2].roi.x = 0;
	g_hbn_cfg[0].vse_attr.ochn_attr[2].roi.y = 0;
	g_hbn_cfg[0].vse_attr.ochn_attr[2].roi.w = 1280;
	g_hbn_cfg[0].vse_attr.ochn_attr[2].roi.h = 960;

	g_hbn_cfg[0].vse_attr.ochn_attr[3].chn_en = CAM_TRUE;
	g_hbn_cfg[0].vse_attr.ochn_attr[3].target_w = 640;
	g_hbn_cfg[0].vse_attr.ochn_attr[3].target_h = 480;
	g_hbn_cfg[0].vse_attr.ochn_attr[3].fmt = FRM_FMT_NV12;
	g_hbn_cfg[0].vse_attr.ochn_attr[3].bit_width = 8;
	g_hbn_cfg[0].vse_attr.ochn_attr[3].roi.x = 0;
	g_hbn_cfg[0].vse_attr.ochn_attr[3].roi.y = 0;
	g_hbn_cfg[0].vse_attr.ochn_attr[3].roi.w = 1280;
	g_hbn_cfg[0].vse_attr.ochn_attr[3].roi.h = 960;

	g_hbn_cfg[0].vse_attr.ochn_attr[4].chn_en = CAM_TRUE;
	g_hbn_cfg[0].vse_attr.ochn_attr[4].target_w = 640;
	g_hbn_cfg[0].vse_attr.ochn_attr[4].target_h = 480;
	g_hbn_cfg[0].vse_attr.ochn_attr[4].fmt = FRM_FMT_NV12;
	g_hbn_cfg[0].vse_attr.ochn_attr[4].bit_width = 8;
	g_hbn_cfg[0].vse_attr.ochn_attr[4].roi.x = 0;
	g_hbn_cfg[0].vse_attr.ochn_attr[4].roi.y = 0;
	g_hbn_cfg[0].vse_attr.ochn_attr[4].roi.w = 1280;
	g_hbn_cfg[0].vse_attr.ochn_attr[4].roi.h = 960;

	g_hbn_cfg[0].vse_attr.ochn_attr[5].chn_en = CAM_TRUE;
	g_hbn_cfg[0].vse_attr.ochn_attr[5].target_w = 1280;
	g_hbn_cfg[0].vse_attr.ochn_attr[5].target_h = 960;
	g_hbn_cfg[0].vse_attr.ochn_attr[5].fmt = FRM_FMT_NV12;
	g_hbn_cfg[0].vse_attr.ochn_attr[5].bit_width = 8;
	g_hbn_cfg[0].vse_attr.ochn_attr[5].roi.x = 0;
	g_hbn_cfg[0].vse_attr.ochn_attr[5].roi.y = 0;
	g_hbn_cfg[0].vse_attr.ochn_attr[5].roi.w = 1280;
	g_hbn_cfg[0].vse_attr.ochn_attr[5].roi.h = 960;

	/*codec attr config*/
	g_hbn_cfg[0].codec_attr.input_width = 640;
	g_hbn_cfg[0].codec_attr.input_stride = 640;
	g_hbn_cfg[0].codec_attr.input_height = 480;
	g_hbn_cfg[0].codec_attr.output_width = 640;
	g_hbn_cfg[0].codec_attr.output_stride = 640;
	g_hbn_cfg[0].codec_attr.output_height = 480;
	g_hbn_cfg[0].codec_attr.buf_num = 8;
	g_hbn_cfg[0].codec_attr.fb_buf_num = 8;

	memcpy(&g_hbn_cfg[1], &g_hbn_cfg[0], sizeof(hbn_cfg_t));
	g_hbn_cfg[1].vin_attr.vin_ichn_attr.width = 1920;
	g_hbn_cfg[1].vin_attr.vin_ichn_attr.height = 1080;
	g_hbn_cfg[1].vin_attr.vin_ichn_attr.format = 0x2b;
	g_hbn_cfg[1].vin_attr.vin_node_attr.cim_attr.vc_index = 0;
	g_hbn_cfg[1].vin_attr.vin_node_attr.cim_attr.mipi_rx = 1;
	g_hbn_cfg[1].vin_attr.vin_ochn_attr[VIN_MAIN_FRAME].vin_basic_attr.wstride = 3840;
	g_hbn_cfg[1].vin_attr.vin_ochn_attr[VIN_MAIN_FRAME].roi_en = 0;
	g_hbn_cfg[1].vin_attr.vin_ochn_attr[VIN_MAIN_FRAME].roi_attr.roi_x = 0;
	g_hbn_cfg[1].vin_attr.vin_ochn_attr[VIN_MAIN_FRAME].roi_attr.roi_y = 0;
	g_hbn_cfg[1].vin_attr.vin_ochn_attr[VIN_MAIN_FRAME].roi_attr.roi_width = 1920;
	g_hbn_cfg[1].vin_attr.vin_ochn_attr[VIN_MAIN_FRAME].roi_attr.roi_height = 1080;
	g_hbn_cfg[1].isp_attr.isp_attr.crop.w = 1920;
	g_hbn_cfg[1].isp_attr.isp_attr.crop.h = 1080;
	g_hbn_cfg[1].isp_attr.ichn_attr.width = 1920;
	g_hbn_cfg[1].isp_attr.ichn_attr.height = 1080;

	/* vse_attr.fps.src和vse_attr.fps.dst未实现，暂不能加 */
	//g_hbn_cfg[1].vse_attr.vse_attr.fps.src = 30;
	//g_hbn_cfg[1].vse_attr.vse_attr.fps.dst = 30;

	g_hbn_cfg[1].vse_attr.ichn_attr.tpg_en = CAM_FALSE;
	g_hbn_cfg[1].vse_attr.ichn_attr.width = 1920;
	g_hbn_cfg[1].vse_attr.ichn_attr.height = 1080;
	g_hbn_cfg[1].vse_attr.ichn_attr.fmt = FRM_FMT_NV12;
	g_hbn_cfg[1].vse_attr.ichn_attr.bit_width = 8;

	g_hbn_cfg[1].vse_attr.ochn_attr[0].chn_en = CAM_TRUE;
	g_hbn_cfg[1].vse_attr.ochn_attr[0].target_w = 960;
	g_hbn_cfg[1].vse_attr.ochn_attr[0].target_h = 540;
	g_hbn_cfg[1].vse_attr.ochn_attr[0].fmt = FRM_FMT_NV12;
	g_hbn_cfg[1].vse_attr.ochn_attr[0].bit_width = 8;
	g_hbn_cfg[1].vse_attr.ochn_attr[0].roi.x = 0;
	g_hbn_cfg[1].vse_attr.ochn_attr[0].roi.y = 0;
	g_hbn_cfg[1].vse_attr.ochn_attr[0].roi.w = 1920;
	g_hbn_cfg[1].vse_attr.ochn_attr[0].roi.h = 1080;

	g_hbn_cfg[1].vse_attr.ochn_attr[1].chn_en = CAM_TRUE;
	g_hbn_cfg[1].vse_attr.ochn_attr[1].target_w = 960;
	g_hbn_cfg[1].vse_attr.ochn_attr[1].target_h = 540;
	g_hbn_cfg[1].vse_attr.ochn_attr[1].fmt = FRM_FMT_NV12;
	g_hbn_cfg[1].vse_attr.ochn_attr[1].bit_width = 8;
	g_hbn_cfg[1].vse_attr.ochn_attr[1].roi.x = 0;
	g_hbn_cfg[1].vse_attr.ochn_attr[1].roi.y = 0;
	g_hbn_cfg[1].vse_attr.ochn_attr[1].roi.w = 1920;
	g_hbn_cfg[1].vse_attr.ochn_attr[1].roi.h = 1080;

	g_hbn_cfg[1].vse_attr.ochn_attr[2].chn_en = CAM_TRUE;
	g_hbn_cfg[1].vse_attr.ochn_attr[2].target_w = 960;
	g_hbn_cfg[1].vse_attr.ochn_attr[2].target_h = 540;
	g_hbn_cfg[1].vse_attr.ochn_attr[2].fmt = FRM_FMT_NV12;
	g_hbn_cfg[1].vse_attr.ochn_attr[2].bit_width = 8;
	g_hbn_cfg[1].vse_attr.ochn_attr[2].roi.x = 0;
	g_hbn_cfg[1].vse_attr.ochn_attr[2].roi.y = 0;
	g_hbn_cfg[1].vse_attr.ochn_attr[2].roi.w = 1920;
	g_hbn_cfg[1].vse_attr.ochn_attr[2].roi.h = 1080;

	g_hbn_cfg[1].vse_attr.ochn_attr[3].chn_en = CAM_TRUE;
	g_hbn_cfg[1].vse_attr.ochn_attr[3].target_w = 960;
	g_hbn_cfg[1].vse_attr.ochn_attr[3].target_h = 540;
	g_hbn_cfg[1].vse_attr.ochn_attr[3].fmt = FRM_FMT_NV12;
	g_hbn_cfg[1].vse_attr.ochn_attr[3].bit_width = 8;
	g_hbn_cfg[1].vse_attr.ochn_attr[3].roi.x = 0;
	g_hbn_cfg[1].vse_attr.ochn_attr[3].roi.y = 0;
	g_hbn_cfg[1].vse_attr.ochn_attr[3].roi.w = 1920;
	g_hbn_cfg[1].vse_attr.ochn_attr[3].roi.h = 1080;

	g_hbn_cfg[1].vse_attr.ochn_attr[4].chn_en = CAM_TRUE;
	g_hbn_cfg[1].vse_attr.ochn_attr[4].target_w = 960;
	g_hbn_cfg[1].vse_attr.ochn_attr[4].target_h = 540;
	g_hbn_cfg[1].vse_attr.ochn_attr[4].fmt = FRM_FMT_NV12;
	g_hbn_cfg[1].vse_attr.ochn_attr[4].bit_width = 8;
	g_hbn_cfg[1].vse_attr.ochn_attr[4].roi.x = 0;
	g_hbn_cfg[1].vse_attr.ochn_attr[4].roi.y = 0;
	g_hbn_cfg[1].vse_attr.ochn_attr[4].roi.w = 1920;
	g_hbn_cfg[1].vse_attr.ochn_attr[4].roi.h = 1080;

	g_hbn_cfg[1].vse_attr.ochn_attr[5].chn_en = CAM_TRUE;
	g_hbn_cfg[1].vse_attr.ochn_attr[5].target_w = 1920;
	g_hbn_cfg[1].vse_attr.ochn_attr[5].target_h = 1080;
	g_hbn_cfg[1].vse_attr.ochn_attr[5].fmt = FRM_FMT_NV12;
	g_hbn_cfg[1].vse_attr.ochn_attr[5].bit_width = 8;
	g_hbn_cfg[1].vse_attr.ochn_attr[5].roi.x = 0;
	g_hbn_cfg[1].vse_attr.ochn_attr[5].roi.y = 0;
	g_hbn_cfg[1].vse_attr.ochn_attr[5].roi.w = 1920;
	g_hbn_cfg[1].vse_attr.ochn_attr[5].roi.h = 1080;

	/*codec attr config*/
	g_hbn_cfg[1].codec_attr.input_width = 960;
	g_hbn_cfg[1].codec_attr.input_stride = 960;
	g_hbn_cfg[1].codec_attr.input_height = 536;/*encode height should align to 8 byte*/
	g_hbn_cfg[1].codec_attr.output_width = 960;
	g_hbn_cfg[1].codec_attr.output_stride = 960;
	g_hbn_cfg[1].codec_attr.output_height = 536;/*encode height should align to 8 byte*/
	g_hbn_cfg[1].codec_attr.buf_num = 8;
	g_hbn_cfg[1].codec_attr.fb_buf_num = 8;

	/* deserial config */
	g_mipi_host_cfg[0].phy = 0; // D_PHY
	g_mipi_host_cfg[0].lane = 1;
	g_mipi_host_cfg[0].datatype = 0x2b;
	g_mipi_host_cfg[0].fps = 30;
	g_mipi_host_cfg[0].mclk = 1; // ignore
	g_mipi_host_cfg[0].mipiclk = 504;
	g_mipi_host_cfg[0].width = 1280;
	g_mipi_host_cfg[0].height = 960;
	g_mipi_host_cfg[0].linelenth = 1600;
	g_mipi_host_cfg[0].framelenth = 1050;
	g_mipi_host_cfg[0].settle = 20;
	g_mipi_host_cfg[0].hsaTime = 0;
	g_mipi_host_cfg[0].hbpTime = 0;
	g_mipi_host_cfg[0].hsdTime = 0;
	g_mipi_host_cfg[0].channel_num = 2;
	g_mipi_host_cfg[0].channel_sel[0] = 0;
	g_mipi_host_cfg[0].channel_sel[1] = 1;
	memcpy(&g_mipi_host_cfg[1], &g_mipi_host_cfg[0], sizeof(mipi_host_cfg_t));
	g_mipi_host_cfg[1].mipiclk = 810;
	g_mipi_host_cfg[1].width = 1920;
	g_mipi_host_cfg[1].height = 1080;
	g_mipi_host_cfg[1].linelenth = 2149;
	g_mipi_host_cfg[1].framelenth = 1125;
	g_mipi_host_cfg[1].channel_num = 1;
	g_mipi_host_cfg[1].channel_sel[0] = 0;
	g_mipi_host_cfg[1].channel_sel[1] = 0;

	if (g_mipi_host_cfg[0].phy != 0)
		printf("x5 mipi setting error, dphy should set to 0\n");
	if ((g_mipi_host_cfg[0].lane < 1) || (g_mipi_host_cfg[0].lane > 4))
		printf("x5 mipi setting error, mipi lane value should be from 1 to 4\n");
	if ((g_mipi_host_cfg[0].mclk >= 2) && (g_mipi_host_cfg[0].mclk <= 24)) {
		printf("x5 mipi setting error, mipi mclk set invalid and dropped\n");
	} else if ((g_mipi_host_cfg[0].mclk >= 25) && (g_mipi_host_cfg[0].mclk <= 636)) {
		printf("x5 mipi setting error, mipi mclk set invalid and erroneous\n");
	} else if (g_mipi_host_cfg[0].mclk >= 637) {
		printf("x5 mipi setting error, mipi mclk value should be from 6.37MHz to 655.35MHz\n");
	}

	if (g_mipi_host_cfg[0].width > 8192)
		printf("x5 mipi setting error, mipi width value should be from 0 to 8192\n");
	if (g_mipi_host_cfg[0].height > 4096)
		printf("x5 mipi setting error, mipi height value should be from 0 to 4096\n");


	// mipi host config
	memcpy(&g_mipi_config[0].rx_attr, &g_mipi_host_cfg[0], sizeof(mipi_host_cfg_t)); // RX设备属性.
	g_mipi_config[0].rx_ex_mask = 0;												 // RX增强属性掩码.
	// g_mipi_config[0].rx_attr_ex							// RX增强属性;
	g_mipi_config[0].bypass = NULL; // RX->TX bypass使能;
	g_mipi_config[0].end_flag = MIPI_CONFIG_END_FLAG;
	g_mipi_config[0].rx_enable = 1;
	memcpy(&g_mipi_config[1], &g_mipi_config[0], sizeof(mipi_config_t));

	g_camera_config[0].mipi_cfg = &g_mipi_config[0];
	g_camera_config[0].addr = 0x10;
	g_camera_config[0].serial_addr = 0x40;
	g_camera_config[0].eeprom_addr = 0x50;
	g_camera_config[0].sensor_mode = 5;
	g_camera_config[0].fps = 30;
	g_camera_config[0].width = 1280;
	g_camera_config[0].height = 960;
	// g_camera_config[0].format = 0;
	// g_camera_config[0].flags = 0;
	g_camera_config[0].extra_mode = 2;
	g_camera_config[0].config_index = 0;
	strcpy(g_camera_config[0].name, "sc1330t");
	g_camera_config[0].addr = 0x30;
	g_camera_config[0].sensor_mode = 1; // normal mode
	g_camera_config[0].format = 43;
	g_camera_config[0].flags = 0;
	g_camera_config[0].extra_mode = 0;

	// strcpy(g_camera_config[0].calib_lname, "lib_CA82GB_pwl12_WS_Fov120.so");
	g_camera_config[0].end_flag = CAMERA_CONFIG_END_FLAG;
	memcpy(&g_camera_config[1], &g_camera_config[0], sizeof(camera_config_t));
	g_camera_config[1].mipi_cfg = &g_mipi_config[1];
	strcpy(g_camera_config[1].name, "sc230ai");
	g_camera_config[1].addr = 0x32;
	g_camera_config[1].sensor_mode = 1; // normal mode
	g_camera_config[1].width = 1920;
	g_camera_config[1].height = 1080;
	g_camera_config[1].extra_mode = 0;
}

int main(int argc, char** argv) {
	int ret = 0;
	pthread_t read_vse_thread,read_codec_thread;
	int index = 0;

	print_help();
	common_attr_config();
	hb_mem_module_open();
	for (index = 0; index < PIPES_TOTAL; index++) {
		ret = create_and_run_vflow(index);
		ERR_CON_EQ(ret, 0);
		ret = codec_init(index);
		ERR_CON_EQ(ret, 0);
		ret = codec_start(index);
		ERR_CON_EQ(ret, 0);
	}
	ret = pthread_create(&read_vse_thread, NULL, (void *)read_vse_data,
			NULL);
	running = 1;
	ret = pthread_create(&read_codec_thread, NULL, (void *)read_codec_data,
			NULL);
	pthread_join(read_vse_thread, NULL);
	pthread_join(read_codec_thread, NULL);
	for(index = 0; index < PIPES_TOTAL; index++){
		ret = hbn_vflow_stop(g_vflow_fd[index]);
		ERR_CON_EQ(ret, 0);
		hbn_vflow_destroy(g_vflow_fd[index]);
	}

	hb_mem_module_close();

	return 0;
}

static int creat_camera_node(int pipe_index) {
	int32_t ret = 0;
	ret = hbn_camera_create(&g_camera_config[pipe_index], &g_cam_fd[pipe_index]);
	ERR_CON_EQ(ret, 0);
	printf("creat_camera_node g_cam_fd[pipe_index] = %ld\n",g_cam_fd[pipe_index]);
	return 0;
}

static hbn_vnode_handle_t creat_vin_node(int pipe_index) {
	hbn_vnode_handle_t vin_node_handle;
	uint32_t hw_id = 0;
	int32_t ret = 0;
	uint32_t chn_id = 0;

	hw_id = g_hbn_cfg[pipe_index].vin_attr.vin_node_attr.cim_attr.mipi_rx;

	ret = hbn_vnode_open(HB_VIN, hw_id, AUTO_ALLOC_ID, &vin_node_handle);
	ERR_CON_EQ(ret, 0);
	// 设置基本属性
	ret = hbn_vnode_set_attr(vin_node_handle, &g_hbn_cfg[pipe_index].vin_attr);
	ERR_CON_EQ(ret, 0);
	// 设置输入通道的属性
	ret = hbn_vnode_set_ichn_attr(vin_node_handle, chn_id, &g_hbn_cfg[pipe_index].vin_attr.vin_ichn_attr);
	ERR_CON_EQ(ret, 0);
	// 设置输出通道的属性
	ret = hbn_vnode_set_ochn_attr(vin_node_handle, chn_id, &g_hbn_cfg[pipe_index].vin_attr.vin_ochn_attr[VIN_MAIN_FRAME]);
	ERR_CON_EQ(ret, 0);
	// 设置额外属性，for mclk
	ret = hbn_vnode_set_attr_ex(vin_node_handle, &g_hbn_cfg[pipe_index].vin_attr.vin_attr_ex);
	ERR_CON_EQ(ret, 0);

	return vin_node_handle;
}

static hbn_vnode_handle_t creat_isp_node(int pipe_index) {
	hbn_vnode_handle_t isp_node_handle;
	hbn_buf_alloc_attr_t alloc_attr = {0};
	uint32_t chn_id = 0;
	int ret = 0;

	ret = hbn_vnode_open(HB_ISP, 0, AUTO_ALLOC_ID, &isp_node_handle);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_attr(isp_node_handle, &g_hbn_cfg[pipe_index].isp_attr.isp_attr);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_ochn_attr(isp_node_handle, chn_id, &g_hbn_cfg[pipe_index].isp_attr.ochn_attr);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_ichn_attr(isp_node_handle, chn_id, &g_hbn_cfg[pipe_index].isp_attr.ichn_attr);
	ERR_CON_EQ(ret, 0);

	alloc_attr.buffers_num = 3;
	alloc_attr.is_contig = 1;
	alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN
						| HB_MEM_USAGE_CPU_WRITE_OFTEN
						| HB_MEM_USAGE_CACHED;
	ret = hbn_vnode_set_ochn_buf_attr(isp_node_handle, chn_id, &alloc_attr);
	ERR_CON_EQ(ret, 0);

	return isp_node_handle;
}

static hbn_vnode_handle_t creat_vse_node(int pipe_index) {
	int ret = 0;
	hbn_vnode_handle_t vse_node_handle;
	uint32_t chn_id = 0;
	uint32_t hw_id = 0;
	hbn_buf_alloc_attr_t alloc_attr = {0};

	ret = hbn_vnode_open(HB_VSE, hw_id, AUTO_ALLOC_ID, &vse_node_handle);
	ERR_CON_EQ(ret, 0);

	ret = hbn_vnode_set_attr(vse_node_handle, &g_hbn_cfg[pipe_index].vse_attr.vse_attr);
	ERR_CON_EQ(ret, 0);

	ret = hbn_vnode_set_ichn_attr(vse_node_handle, chn_id, &g_hbn_cfg[pipe_index].vse_attr.ichn_attr);
	ERR_CON_EQ(ret, 0);

	ret = hbn_vnode_set_ochn_attr(vse_node_handle, 0, &g_hbn_cfg[pipe_index].vse_attr.ochn_attr[0]);
	ERR_CON_EQ(ret, 0);
	alloc_attr.buffers_num = 3;
	alloc_attr.is_contig = 1;
	alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN
						| HB_MEM_USAGE_CPU_WRITE_OFTEN
						| HB_MEM_USAGE_CACHED
						| HB_MEM_USAGE_GRAPHIC_CONTIGUOUS_BUF;
	ret = hbn_vnode_set_ochn_buf_attr(vse_node_handle, 0, &alloc_attr);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_ochn_attr(vse_node_handle, 1, &g_hbn_cfg[pipe_index].vse_attr.ochn_attr[1]);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_ochn_buf_attr(vse_node_handle, 1, &alloc_attr);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_ochn_attr(vse_node_handle, 2, &g_hbn_cfg[pipe_index].vse_attr.ochn_attr[2]);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_ochn_buf_attr(vse_node_handle, 2, &alloc_attr);
	ERR_CON_EQ(ret, 0);

	return vse_node_handle;
}

static hbn_vnode_handle_t create_codec_node(int pipe_index)
{
		int32_t ret = 0;
		hbn_vnode_handle_t codec_node_handle;
		hbn_buf_alloc_attr_t alloc_attr;

		ret = hbn_vnode_open(HB_CODEC, 0, AUTO_ALLOC_ID, &codec_node_handle);
		ERR_CON_EQ(ret, 0);

		ret = hbn_vnode_set_attr(codec_node_handle, &g_hbn_cfg[pipe_index].codec_attr);
		ERR_CON_EQ(ret, 0);

		ret = hbn_vnode_set_ichn_attr(codec_node_handle, 0, &g_hbn_cfg[pipe_index].codec_attr);
		ERR_CON_EQ(ret, 0);

		ret = hbn_vnode_set_ochn_attr(codec_node_handle, 0, &g_hbn_cfg[pipe_index].codec_attr);
		ERR_CON_EQ(ret, 0);

		memset(&alloc_attr, 0, sizeof(hbn_buf_alloc_attr_t));
		alloc_attr.buffers_num = g_hbn_cfg[pipe_index].codec_attr.buf_num;
		alloc_attr.is_contig = 1;
		alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN | HB_MEM_USAGE_CPU_WRITE_OFTEN | HB_MEM_USAGE_CACHED;
		ret = hbn_vnode_set_ochn_buf_attr(codec_node_handle, 0, &alloc_attr);
		ERR_CON_EQ(ret, 0);

		return codec_node_handle;
}

int create_and_run_vflow(int pipe_index) {
	int32_t ret = 0;
	hbn_vnode_handle_t vin_node_handle;
	hbn_vnode_handle_t isp_node_handle;
	hbn_vnode_handle_t vse_node_handle;
	hbn_vnode_handle_t codec_node_handle;

	// 创建pipeline中的每个node
	ret = creat_camera_node(pipe_index);
	ERR_CON_EQ(ret, 0);
	vin_node_handle = creat_vin_node(pipe_index);
	ERR_CON_NE(vin_node_handle, 0);
	isp_node_handle = creat_isp_node(pipe_index);
	ERR_CON_NE(isp_node_handle, 0);
	vse_node_handle = creat_vse_node(pipe_index);
	ERR_CON_NE(vse_node_handle, 0);

	// 创建HBN flow
	ret = hbn_vflow_create(&g_vflow_fd[pipe_index]);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_add_vnode(g_vflow_fd[pipe_index],
							vin_node_handle);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_add_vnode(g_vflow_fd[pipe_index],
							isp_node_handle);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_add_vnode(g_vflow_fd[pipe_index],
							vse_node_handle);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_bind_vnode(g_vflow_fd[pipe_index],
							vin_node_handle,
							1,
							isp_node_handle,
							0);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_bind_vnode(g_vflow_fd[pipe_index],
							isp_node_handle,
							0,
							vse_node_handle,
							0);
	ERR_CON_EQ(ret, 0);
	if(codec_bind_vse){
		codec_node_handle = create_codec_node(pipe_index);
		ERR_CON_NE(codec_node_handle, 0);
		ret = hbn_vflow_add_vnode(g_vflow_fd[pipe_index],
							codec_node_handle);
		ERR_CON_EQ(ret, 0);
		ret = hbn_vflow_bind_vnode(g_vflow_fd[pipe_index],
								vse_node_handle,
								0,
								codec_node_handle,
								0);
		ERR_CON_EQ(ret, 0);
	}
	printf("create_and_run_vflow g_cam_fd[pipe_index] = %ld,vin_node_handle=%ld,vse_node_handle = %ld \n",
		g_cam_fd[pipe_index],vin_node_handle,vse_node_handle);
	ret = hbn_camera_attach_to_vin(g_cam_fd[pipe_index],
							vin_node_handle);
	ERR_CON_EQ(ret, 0);
	ret = hbn_camera_start(g_cam_fd[pipe_index]);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_start(g_vflow_fd[pipe_index]);
	ERR_CON_EQ(ret, 0);

	return 0;
}

static int get_rc_params(media_codec_context_t *context,
			mc_rate_control_params_t *rc_params) {
	int ret = 0;
	ret = hb_mm_mc_get_rate_control_config(context, rc_params);
	if (ret != 0) {
		printf("hb_mm_mc_get_rate_control_config Failed to get rc params ret=0x%x\n", ret);
		return -1;
	}
	switch (rc_params->mode) {
	case MC_AV_RC_MODE_H264CBR:
		rc_params->h264_cbr_params.intra_period = 30;
		rc_params->h264_cbr_params.intra_qp = 30;
		rc_params->h264_cbr_params.bit_rate = 5000;
		rc_params->h264_cbr_params.frame_rate = 30;
		rc_params->h264_cbr_params.initial_rc_qp = 20;
		rc_params->h264_cbr_params.vbv_buffer_size = 20;
		rc_params->h264_cbr_params.mb_level_rc_enalbe = 1;
		rc_params->h264_cbr_params.min_qp_I = 8;
		rc_params->h264_cbr_params.max_qp_I = 50;
		rc_params->h264_cbr_params.min_qp_P = 8;
		rc_params->h264_cbr_params.max_qp_P = 50;
		rc_params->h264_cbr_params.min_qp_B = 8;
		rc_params->h264_cbr_params.max_qp_B = 50;
		rc_params->h264_cbr_params.hvs_qp_enable = 1;
		rc_params->h264_cbr_params.hvs_qp_scale = 2;
		rc_params->h264_cbr_params.max_delta_qp = 10;
		rc_params->h264_cbr_params.qp_map_enable = 0;
		break;
	case MC_AV_RC_MODE_H264VBR:
		rc_params->h264_vbr_params.intra_qp = 20;
		rc_params->h264_vbr_params.intra_period = 30;
		rc_params->h264_vbr_params.intra_qp = 35;
		break;
	case MC_AV_RC_MODE_H264AVBR:
		rc_params->h264_avbr_params.intra_period = 15;
		rc_params->h264_avbr_params.intra_qp = 25;
		rc_params->h264_avbr_params.bit_rate = 2000;
		rc_params->h264_avbr_params.vbv_buffer_size = 3000;
		rc_params->h264_avbr_params.min_qp_I = 15;
		rc_params->h264_avbr_params.max_qp_I = 50;
		rc_params->h264_avbr_params.min_qp_P = 15;
		rc_params->h264_avbr_params.max_qp_P = 45;
		rc_params->h264_avbr_params.min_qp_B = 15;
		rc_params->h264_avbr_params.max_qp_B = 48;
		rc_params->h264_avbr_params.hvs_qp_enable = 0;
		rc_params->h264_avbr_params.hvs_qp_scale = 2;
		rc_params->h264_avbr_params.max_delta_qp = 5;
		rc_params->h264_avbr_params.qp_map_enable = 0;
		break;
	case MC_AV_RC_MODE_H264FIXQP:
		rc_params->h264_fixqp_params.force_qp_I = 23;
		rc_params->h264_fixqp_params.force_qp_P = 23;
		rc_params->h264_fixqp_params.force_qp_B = 23;
		rc_params->h264_fixqp_params.intra_period = 23;
		break;
	case MC_AV_RC_MODE_H264QPMAP:
		break;
	case MC_AV_RC_MODE_H265CBR:
		rc_params->h265_cbr_params.intra_period = 20;
		rc_params->h265_cbr_params.intra_qp = 30;
		rc_params->h265_cbr_params.bit_rate = 5000;
		rc_params->h265_cbr_params.frame_rate = 30;
		if (context->video_enc_params.width >= 480 ||
			context->video_enc_params.height >= 480) {
			rc_params->h265_cbr_params.initial_rc_qp = 30;
			rc_params->h265_cbr_params.vbv_buffer_size = 3000;
			rc_params->h265_cbr_params.ctu_level_rc_enalbe = 1;
		} else {
			rc_params->h265_cbr_params.initial_rc_qp = 20;
			rc_params->h265_cbr_params.vbv_buffer_size = 20;
			rc_params->h265_cbr_params.ctu_level_rc_enalbe = 1;
		}
		rc_params->h265_cbr_params.min_qp_I = 8;
		rc_params->h265_cbr_params.max_qp_I = 50;
		rc_params->h265_cbr_params.min_qp_P = 8;
		rc_params->h265_cbr_params.max_qp_P = 50;
		rc_params->h265_cbr_params.min_qp_B = 8;
		rc_params->h265_cbr_params.max_qp_B = 50;
		rc_params->h265_cbr_params.hvs_qp_enable = 1;
		rc_params->h265_cbr_params.hvs_qp_scale = 2;
		rc_params->h265_cbr_params.max_delta_qp = 10;
		rc_params->h265_cbr_params.qp_map_enable = 0;
		break;
	case MC_AV_RC_MODE_H265VBR:
		rc_params->h265_vbr_params.intra_qp = 20;
		rc_params->h265_vbr_params.intra_period = 30;
		rc_params->h265_vbr_params.intra_qp = 35;
		break;
	case MC_AV_RC_MODE_H265AVBR:
		rc_params->h265_avbr_params.intra_period = 15;
		rc_params->h265_avbr_params.intra_qp = 25;
		rc_params->h265_avbr_params.bit_rate = 2000;
		rc_params->h265_avbr_params.vbv_buffer_size = 3000;
		rc_params->h265_avbr_params.min_qp_I = 15;
		rc_params->h265_avbr_params.max_qp_I = 50;
		rc_params->h265_avbr_params.min_qp_P = 15;
		rc_params->h265_avbr_params.max_qp_P = 45;
		rc_params->h265_avbr_params.min_qp_B = 15;
		rc_params->h265_avbr_params.max_qp_B = 48;
		rc_params->h265_avbr_params.hvs_qp_enable = 0;
		rc_params->h265_avbr_params.hvs_qp_scale = 2;
		rc_params->h265_avbr_params.max_delta_qp = 5;
		rc_params->h265_avbr_params.qp_map_enable = 0;
		break;
	case MC_AV_RC_MODE_H265FIXQP:
		rc_params->h265_fixqp_params.force_qp_I = 23;
		rc_params->h265_fixqp_params.force_qp_P = 23;
		rc_params->h265_fixqp_params.force_qp_B = 23;
		rc_params->h265_fixqp_params.intra_period = 23;
		break;
	case MC_AV_RC_MODE_H265QPMAP:
		break;
	default:
		ret = HB_MEDIA_ERR_INVALID_PARAMS;
		break;
	}
	return ret;
}

static int codec_init(int pipe_index){
	int32_t ret = 0;
	mc_video_codec_enc_params_t *params;
	memset(&media_context[pipe_index], 0x00, sizeof(media_codec_context_t));
	media_context[pipe_index].codec_id = MEDIA_CODEC_ID_H264;
	media_context[pipe_index].encoder = 1; //HB_TRUE;
	params = &(media_context[pipe_index].video_enc_params);
	params->width = g_hbn_cfg[pipe_index].codec_attr.input_width;
	params->height = g_hbn_cfg[pipe_index].codec_attr.input_height;
	params->pix_fmt = MC_PIXEL_FORMAT_NV12;
	params->frame_buf_count = 5;
	params->external_frame_buf = 0;
	params->bitstream_buf_count = 5;
	params->bitstream_buf_size = (params->width * params->height * 3 / 2  + 0x3ff) & ~0x3ff;
	params->rc_params.mode = MC_AV_RC_MODE_H264CBR;
	ERR_CON_EQ(get_rc_params(&media_context[pipe_index], &params->rc_params), (int32_t)0);
	params->gop_params.gop_preset_idx = 1;
	params->rot_degree = MC_CCW_0;
	params->mir_direction = MC_DIRECTION_NONE;
	params->frame_cropping_flag = 0;
	params->enable_user_pts = 1;

	ret = hb_mm_mc_initialize(&media_context[pipe_index]);
	if (ret != 0) {
		printf("hb_mm_mc_initialize failed, ret: %x\n", ret);
		return ret;
	}
	if(codec_bind_vse){
		ret = hb_mm_mc_vpf_init(&media_context[pipe_index], pipe_index);
		if (ret != 0) {
			printf("hb_mm_mc_vpf_init failed, ret: %x\n", ret);
			return ret;
		}
	}
	ret = hb_mm_mc_configure(&media_context[pipe_index]);
		if (ret != 0) {
		printf("hb_mm_mc_configure failed, ret: %x\n", ret);
		return ret;
	}
	return 0;
}

static int codec_start(int pipe_index){
	int32_t ret = 0;
	mc_av_codec_startup_params_t startup_params_enc;
	startup_params_enc.video_enc_startup_params.receive_frame_number = 0;
	ret = hb_mm_mc_start(&media_context[pipe_index], &startup_params_enc);
	if (ret != 0) {
		printf("hb_mm_mc_start failed, ret: %x\n", ret);
		return ret;
	}
	return 0;
}

static int64_t get_current_time_us() {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return ((int64_t)tv.tv_sec) * 1000000 + tv.tv_usec;
}

void *read_vse_data(void *arg) {
	hbn_vnode_handle_t vse_node_handle[2] = {0};
	hbn_vnode_image_t out_img[3] = {0};
	media_codec_buffer_t inputBuffer;
	uint32_t count[PIPES_TOTAL] = {0};
	char dst_file[128] = {0};
	int ret = 0;
	for(int i = 0; i < PIPES_TOTAL; i++){
		vse_node_handle[i] = hbn_vflow_get_vnode_handle(g_vflow_fd[i], HB_VSE, 0);
		printf("read_vse_data vse_node_handle[%d] = %ld\n",i, vse_node_handle[i]);
		if (vse_node_handle[i] <= 0) {
			printf("get vflow %d vse handle error\n", i);
			return NULL;
		}
	}
	usleep(100);/*encode need some time to start*/
	while(running) {
		for(int i = 0; i < PIPES_TOTAL; i++){
			ret = hbn_vnode_getframe(vse_node_handle[i], 0, 1000, &out_img[0]);
			if (ret != 0) {
				printf("hbn_vnode_getframe VSE channel 0 failed i = %d,ret = %d\n", i,ret);
			}
			ret = hbn_vnode_getframe(vse_node_handle[i], 1, 1000, &out_img[1]);
			if (ret != 0) {
				printf("hbn_vnode_getframe VSE channel 1 failed i = %d,ret = %d\n", i,ret);
			}
			ret = hbn_vnode_getframe(vse_node_handle[i], 2, 1000, &out_img[2]);
			if (ret != 0) {
				printf("hbn_vnode_getframe VSE channel 2 failed i = %d,ret=%d\n", i,ret);
			}
			if(!codec_bind_vse){
				memset(&inputBuffer, 0x00, sizeof(media_codec_buffer_t));
				inputBuffer.type = MC_VIDEO_FRAME_BUFFER;
				inputBuffer.vframe_buf.width = media_context[i].video_enc_params.width;
				inputBuffer.vframe_buf.height = media_context[i].video_enc_params.height;
				inputBuffer.vframe_buf.pix_fmt = MC_PIXEL_FORMAT_NV12;
				inputBuffer.vframe_buf.size = out_img[0].buffer.size[0] + out_img[0].buffer.size[1];
				inputBuffer.vframe_buf.pts = get_current_time_us();
				ret = hb_mm_mc_dequeue_input_buffer(&media_context[i], &inputBuffer, 2000);
				if(ret != 0){
					printf("hb_mm_mc_dequeue_input_buffer failed. i = %d, count[i] = %d, ret = %d\n",i, count[i], ret);
				}
				memcpy(inputBuffer.vframe_buf.vir_ptr[0], out_img[0].buffer.virt_addr[0], out_img[0].buffer.size[0] + out_img[0].buffer.size[1]);
				ret = hb_mm_mc_queue_input_buffer(&media_context[i], &inputBuffer, 2000);
				if(ret != 0){
					printf("hb_mm_mc_queue_input_buffer failed. i = %d, ret = %d\n",i, ret);

				}

			}
			if (count[i] % 60 == 0) {
				for (int j = 0; j < 3; ++j) {
					hb_mem_invalidate_buf_with_vaddr(
					(uint64_t)out_img[j].buffer.virt_addr[0],
					out_img[j].buffer.size[0]);
					hb_mem_invalidate_buf_with_vaddr(
						(uint64_t)out_img[j].buffer.virt_addr[1],
						out_img[j].buffer.size[1]);
					snprintf(dst_file, sizeof(dst_file), "sensor%d_vse_ch%d_%d.yuv", i, j, count[i]);
					dump_2plane_yuv_to_file(dst_file,
							out_img[j].buffer.virt_addr[0],
							out_img[j].buffer.virt_addr[1],
							out_img[j].buffer.size[0],
							out_img[j].buffer.size[1]);
				}
			}
			hbn_vnode_releaseframe(vse_node_handle[i], 0, &out_img[0]);
			hbn_vnode_releaseframe(vse_node_handle[i], 1, &out_img[1]);
			hbn_vnode_releaseframe(vse_node_handle[i], 2, &out_img[2]);
			count[i]++;
			usleep(1000*10);
		}
	}
	return NULL;
}

void *read_codec_data(void *arg)
{
	int32_t ret;
	printf("codec_read_data start\n");

	while(running){
		for(int i = 0; i < PIPES_TOTAL; i++){
			if(fout[i] == NULL){
				snprintf(dst_file, sizeof(dst_file), "codec_s%d.h264", i);
				fout[i] = fopen(dst_file, "wb");
			}
			memset(&outputBuffer[i], 0x00, sizeof(media_codec_buffer_t));
			memset(&codec_outinfo[i], 0x00, sizeof(media_codec_output_buffer_info_t));

			ret = hb_mm_mc_dequeue_output_buffer(&media_context[i], &outputBuffer[i], &codec_outinfo[i], 3000);
			if (ret == 0 && fout[i]) {
				printf("encode fwrite size(%d)\n", outputBuffer[i].vstream_buf.size);
				fwrite(outputBuffer[i].vstream_buf.vir_ptr, outputBuffer[i].vstream_buf.size, 1, fout[i]);
				hb_mm_mc_queue_output_buffer(&media_context[i], &outputBuffer[i], 100);
			} else {
				printf("dequeue failed\n");
			}
		}
	}
	return NULL;
}
