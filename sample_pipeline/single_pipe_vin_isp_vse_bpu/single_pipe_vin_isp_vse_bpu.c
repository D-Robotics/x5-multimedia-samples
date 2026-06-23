/***************************************************************************
 *                      COPYRIGHT NOTICE
 *             Copyright(C) 2024, D-Robotics Co., Ltd.
 *                     All rights reserved.
 ***************************************************************************/

#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "bpu_mobilenetv2.h"
#include "channel_param_parser.h"
#include "common_utils.h"

#define VSE_BPU_CHANNEL 1
#define BPU_MODEL_PATH "./model_zoom/mobilenetv2_224x224_nv12.bin"

static struct option const long_options[] = {{"sensor", required_argument, NULL, 's'},
					     {"channel-type", optional_argument, NULL, 'c'},
					     {NULL, 0, NULL, 0}};

static int32_t running = 0;
static uint32_t sensor_type = 0;
static uint32_t link_port = 0;
extern int vin_isp_is_online;
extern int isp_vse_is_online;

typedef struct {
	pipe_contex_t pipe_contex;
	bpu_handle_t bpu_handle;
} app_context_t;

int32_t hbn_deserial_create(deserial_config_t *des_config, deserial_handle_t *des_fd);
int32_t hbn_deserial_attach_to_vin(deserial_handle_t des_fd, camera_des_link_t link, vpf_handle_t vin_fd);

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

void signal_handle(int signo)
{
	running = 0;
}

static int create_camera_node(pipe_contex_t *pipe_contex)
{
	camera_config_t *camera_config = NULL;
	vp_sensor_config_t *sensor_config = NULL;
	int32_t ret = 0;

	sensor_config = pipe_contex->sensor_config;
	camera_config = sensor_config->camera_config;
	ret = hbn_camera_create(camera_config, &pipe_contex->cam_fd);
	ERR_CON_EQ(ret, 0);

	return 0;
}

static int create_deserial_node(pipe_contex_t *pipe_contex)
{
	vp_sensor_config_t *sensor_config = NULL;
	deserial_config_t *deserial_config = NULL;
	deserial_handle_t *des_handle = NULL;

	int32_t ret = 0;
	des_handle = &pipe_contex->des_fd;

	sensor_config = pipe_contex->sensor_config;
	deserial_config = sensor_config->deserial_node_attr;

	ret = hbn_deserial_create(deserial_config, des_handle);
	if (ret != 0) {
		printf("hbn_deserial_create failed ret = %d\n", ret);
		return ret;
	}
	printf("deserial_config:%02x_%s, des_handle:%ld \n\r", deserial_config->addr, deserial_config->name,
	       *des_handle);
	return 0;
}

static int create_vin_node(pipe_contex_t *pipe_contex)
{
	vp_sensor_config_t *sensor_config = NULL;
	vin_node_attr_t *vin_node_attr = NULL;
	vin_ichn_attr_t *vin_ichn_attr = NULL;
	vin_ochn_attr_t *vin_ochn_attr = NULL;
	hbn_vnode_handle_t *vin_node_handle = NULL;
	vin_attr_ex_t vin_attr_ex;
	uint32_t hw_id = 0;
	int32_t ret = 0;
	uint32_t ichn_id = 0;
	uint32_t ochn_id = 0;
	uint64_t vin_attr_ex_mask = 0;

	sensor_config = pipe_contex->sensor_config;
	vin_node_attr = sensor_config->vin_node_attr;
	vin_ichn_attr = sensor_config->vin_ichn_attr;
	vin_ochn_attr = sensor_config->vin_ochn_attr;
	hw_id = vin_node_attr->cim_attr.mipi_rx;
	vin_node_handle = &pipe_contex->vin_node_handle;

	link_port = vin_node_attr->cim_attr.vc_index;
	if (pipe_contex->csi_config.mclk_is_not_configed) {
		printf("csi%d ignore mclk ex attr, because not config mclk.\n", pipe_contex->csi_config.index);
	} else {
		vin_attr_ex.vin_attr_ex_mask = sensor_config->vin_attr_ex->vin_attr_ex_mask;
		vin_attr_ex.mclk_ex_attr.mclk_freq = sensor_config->vin_attr_ex->mclk_ex_attr.mclk_freq;
		vin_attr_ex_mask = vin_attr_ex.vin_attr_ex_mask;
	}
	if (vin_isp_is_online) {
		sensor_config->vin_node_attr->cim_attr.cim_isp_flyby = 1;
		sensor_config->vin_ochn_attr->ddr_en = 0;
	} else {
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
		for (uint8_t i = 0; i < VIN_ATTR_EX_INVALID; i++) {
			if ((vin_attr_ex_mask & (1 << i)) == 0)
				continue;

			vin_attr_ex.ex_attr_type = i;
			ret = hbn_vnode_set_attr_ex(*vin_node_handle, &vin_attr_ex);
			ERR_CON_EQ(ret, 0);
		}
	}
	if (!vin_isp_is_online) {
		hbn_buf_alloc_attr_t alloc_attr = {0};
		memset(&alloc_attr, 0, sizeof(hbn_buf_alloc_attr_t));
		alloc_attr.buffers_num = 3;
		alloc_attr.is_contig = 1;
		alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN | HB_MEM_USAGE_CPU_WRITE_OFTEN | HB_MEM_USAGE_CACHED;
		ret = hbn_vnode_set_ochn_buf_attr(*vin_node_handle, ochn_id, &alloc_attr);
		ERR_CON_EQ(ret, 0);
	}
	return 0;
}

static int create_isp_node(pipe_contex_t *pipe_contex)
{
	vp_sensor_config_t *sensor_config = NULL;
	isp_attr_t *isp_attr = NULL;
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

	if (vin_isp_is_online) {
		sensor_config->isp_attr->input_mode = PASSTHROUGH_MODE;
	} else {
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
		alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN | HB_MEM_USAGE_CPU_WRITE_OFTEN | HB_MEM_USAGE_CACHED;
		ret = hbn_vnode_set_ochn_buf_attr(*isp_node_handle, ochn_id, &alloc_attr);
		ERR_CON_EQ(ret, 0);
	}
	return 0;
}

static int check_vse_scale_ratio(uint32_t input_width, uint32_t input_height, uint32_t output_width,
				 uint32_t output_height, uint32_t roi_width, uint32_t roi_height)
{
	uint32_t effective_input_width = (roi_width > 0) ? roi_width : input_width;
	uint32_t effective_input_height = (roi_height > 0) ? roi_height : input_height;

	int width_scale = (output_width > effective_input_width) ? 1 : (output_width < effective_input_width) ? -1 : 0;
	int height_scale = (output_height > effective_input_height)   ? 1
			   : (output_height < effective_input_height) ? -1
								      : 0;

	if ((width_scale == 1 && height_scale == -1) || (width_scale == -1 && height_scale == 1)) {
		printf("VSE does not allow one dimension to scale up while the other scales down\n");
		return -1;
	}

	return 0;
}

static int check_vse_output_valid(uint32_t input_width, uint32_t input_height, vse_ochn_attr_t *vse_ochn_attr)
{
	for (int i = 0; i < VSE_MAX_CHANNELS; ++i) {
		int ret = check_vse_scale_ratio(input_width, input_height, vse_ochn_attr[i].target_w,
						vse_ochn_attr[i].target_h, vse_ochn_attr[i].roi.w,
						vse_ochn_attr[i].roi.h);
		if (ret != 0) {
			printf("Error: Sensor VSE Output channel %d resolution %dx%d scaling ratio invalid!\n", i,
			       vse_ochn_attr[i].target_w, vse_ochn_attr[i].target_h);
			return -1;
		}
	}

	return 0;
}

static int create_vse_node(pipe_contex_t *pipe_contex)
{
	int ret = 0;
	hbn_vnode_handle_t *vse_node_handle = &pipe_contex->vse_node_handle;
	isp_ichn_attr_t isp_ichn_attr = {0};
	vse_attr_t vse_attr = {0};
	vse_ichn_attr_t vse_ichn_attr = {0};
	vse_ochn_attr_t vse_ochn_attr[VSE_MAX_CHANNELS] = {0};
	uint32_t ichn_id = 0;
	uint32_t hw_id = 0;
	uint32_t input_width = 0;
	uint32_t input_height = 0;
	hbn_buf_alloc_attr_t alloc_attr = {0};

	ret = hbn_vnode_get_ichn_attr(pipe_contex->isp_node_handle, ichn_id, &isp_ichn_attr);
	ERR_CON_EQ(ret, 0);
	input_width = isp_ichn_attr.width;
	input_height = isp_ichn_attr.height;

	vse_ichn_attr.width = input_width;
	vse_ichn_attr.height = input_height;
	vse_ichn_attr.fmt = FRM_FMT_NV12;
	vse_ichn_attr.bit_width = 8;

	for (int i = 0; i < VSE_MAX_CHANNELS; ++i) {
		vse_ochn_attr[i].chn_en = CAM_TRUE;
		vse_ochn_attr[i].roi.x = 0;
		vse_ochn_attr[i].roi.y = 0;
		vse_ochn_attr[i].roi.w = input_width;
		vse_ochn_attr[i].roi.h = input_height;
		vse_ochn_attr[i].fmt = FRM_FMT_NV12;
		vse_ochn_attr[i].bit_width = 8;
	}

	// Channel 0: original resolution
	vse_ochn_attr[0].target_w = input_width;
	vse_ochn_attr[0].target_h = input_height;

	// Channel 1: for BPU inference (mobilenetv2 uses 224x224)
	vse_ochn_attr[1].target_w = 224;
	vse_ochn_attr[1].target_h = 224;

	// Channel 2: 224x224
	vse_ochn_attr[2].target_w = 224;
	vse_ochn_attr[2].target_h = 224;

	// Channel 3: center crop, scale down
	vse_ochn_attr[3].roi.x = input_width / 2 - input_width / 4;
	vse_ochn_attr[3].roi.y = input_height / 2 - input_height / 4;
	vse_ochn_attr[3].roi.w = input_width / 2;
	vse_ochn_attr[3].roi.h = input_height / 2;
	vse_ochn_attr[3].target_w = 64;
	vse_ochn_attr[3].target_h = 64;

	// Channel 4: 480x480
	vse_ochn_attr[4].target_w = 480;
	vse_ochn_attr[4].target_h = 480;

	// Channel 5: upscale
	vse_ochn_attr[5].target_w = (input_width * 2) > 4096 ? 4096 : (input_width * 2);
	vse_ochn_attr[5].target_h = (input_height * 2) > 3076 ? 3076 : (input_height * 2);

	ret = check_vse_output_valid(input_width, input_height, vse_ochn_attr);
	if (ret != 0) {
		exit(-1);
	}
	ret = hbn_vnode_open(HB_VSE, hw_id, AUTO_ALLOC_ID, vse_node_handle);
	ERR_CON_EQ(ret, 0);

	ret = hbn_vnode_set_attr(*vse_node_handle, &vse_attr);
	ERR_CON_EQ(ret, 0);

	ret = hbn_vnode_set_ichn_attr(*vse_node_handle, ichn_id, &vse_ichn_attr);
	ERR_CON_EQ(ret, 0);

	alloc_attr.buffers_num = 3;
	alloc_attr.is_contig = 1;
	alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN | HB_MEM_USAGE_CPU_WRITE_OFTEN | HB_MEM_USAGE_CACHED;

	for (int i = 0; i < VSE_MAX_CHANNELS; ++i) {
		printf("hbn_vnode_set_ochn_attr: %d, %dx%d\n", i, vse_ochn_attr[i].target_w, vse_ochn_attr[i].target_h);
		ret = hbn_vnode_set_ochn_attr(*vse_node_handle, i, &vse_ochn_attr[i]);
		ERR_CON_EQ(ret, 0);
		ret = hbn_vnode_set_ochn_buf_attr(*vse_node_handle, i, &alloc_attr);
		ERR_CON_EQ(ret, 0);
	}

	return 0;
}

int create_and_run_vflow(pipe_contex_t *pipe_contex)
{
	int32_t ret = 0;

	ret = create_camera_node(pipe_contex);
	ERR_CON_EQ(ret, 0);
	ret = create_vin_node(pipe_contex);
	ERR_CON_EQ(ret, 0);
	ret = create_isp_node(pipe_contex);
	ERR_CON_EQ(ret, 0);
	ret = create_vse_node(pipe_contex);
	ERR_CON_EQ(ret, 0);

	ret = hbn_vflow_create(&pipe_contex->vflow_fd);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_add_vnode(pipe_contex->vflow_fd, pipe_contex->vin_node_handle);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_add_vnode(pipe_contex->vflow_fd, pipe_contex->isp_node_handle);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_add_vnode(pipe_contex->vflow_fd, pipe_contex->vse_node_handle);
	ERR_CON_EQ(ret, 0);
	if (vin_isp_is_online) {
		ret = hbn_vflow_bind_vnode(pipe_contex->vflow_fd, pipe_contex->vin_node_handle, 1,
					   pipe_contex->isp_node_handle, 0);
	} else {
		ret = hbn_vflow_bind_vnode(pipe_contex->vflow_fd, pipe_contex->vin_node_handle, 0,
					   pipe_contex->isp_node_handle, 0);
	}
	ERR_CON_EQ(ret, 0);

	if (isp_vse_is_online) {
		ret = hbn_vflow_bind_vnode(pipe_contex->vflow_fd, pipe_contex->isp_node_handle, 1,
					   pipe_contex->vse_node_handle, 0);
	} else {
		ret = hbn_vflow_bind_vnode(pipe_contex->vflow_fd, pipe_contex->isp_node_handle, 0,
					   pipe_contex->vse_node_handle, 0);
	}
	ERR_CON_EQ(ret, 0);

	if (sensor_type != SENSOR_TYPE_NORMAL) {
		ret = create_deserial_node(pipe_contex);
		ERR_CON_EQ(ret, 0);
		ret = hbn_camera_attach_to_deserial(pipe_contex->cam_fd, pipe_contex->des_fd, link_port);
		ERR_CON_EQ(ret, 0);
		ret = hbn_deserial_attach_to_vin(pipe_contex->des_fd, link_port, pipe_contex->vin_node_handle);
		ERR_CON_EQ(ret, 0);
	} else {
		ret = hbn_camera_attach_to_vin(pipe_contex->cam_fd, pipe_contex->vin_node_handle);
		ERR_CON_EQ(ret, 0);
	}

	ret = hbn_vflow_start(pipe_contex->vflow_fd);
	ERR_CON_EQ(ret, 0);

	return 0;
}

void *process_vse_bpu_infer(void *context)
{
	app_context_t *app_ctx = (app_context_t *)context;
	pipe_contex_t *pipe_context = &app_ctx->pipe_contex;
	bpu_handle_t *bpu_handle = &app_ctx->bpu_handle;
	hbn_vnode_handle_t vse_node_handle = pipe_context->vse_node_handle;
	hbn_vnode_image_t vse_frame;
	char bpu_result[BPU_RESULT_BUF_SIZE];
	int32_t class_id = -1;
	int32_t last_class_id = -1;
	int ret = 0;

	while (running) {
		memset(&vse_frame, 0, sizeof(vse_frame));
		ret = hbn_vnode_getframe(vse_node_handle, VSE_BPU_CHANNEL, 2000, &vse_frame);
		if (ret != 0) {
			printf("hbn_vnode_getframe VSE channel %d failed\n", VSE_BPU_CHANNEL);
			continue;
		}

		for (int j = 0; j < 2; ++j) {
			hb_mem_invalidate_buf_with_vaddr((uint64_t)vse_frame.buffer.virt_addr[j],
							 vse_frame.buffer.size[j]);
		}

		memset(bpu_result, 0, sizeof(bpu_result));
		ret = bpu_mobilenetv2_infer(bpu_handle, &vse_frame, &class_id, bpu_result, sizeof(bpu_result));
		if (ret == 0 && class_id != last_class_id) {
			printf("[BPU mobilenetv2] %s\n", bpu_result);
			last_class_id = class_id;
		}

		hbn_vnode_releaseframe(vse_node_handle, VSE_BPU_CHANNEL, &vse_frame);
	}

	return NULL;
}

int main(int argc, char **argv)
{
	int ret = 0;
	app_context_t app_ctx = {0};
	pipe_contex_t *pipe_contex = &app_ctx.pipe_contex;
	bpu_handle_t *bpu_handle = &app_ctx.bpu_handle;
	pthread_t bpu_infer_thread;
	int opt_index = 0;
	int c = 0;
	int index = -1;

	signal(SIGINT, signal_handle);
	/* parse options */
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
	if (index < vp_get_sensors_list_number() && index >= 0) {
		pipe_contex->sensor_config = vp_sensor_config_list[index];
		printf("Using index:%d  sensor_name:%s  config_file:%s\n", index,
		       vp_sensor_config_list[index]->sensor_name, vp_sensor_config_list[index]->config_file);
		sensor_type = pipe_contex->sensor_config->sensor_type;
		if (sensor_type == SENSOR_TYPE_NORMAL) {
			ret = vp_sensor_fixed_mipi_host(pipe_contex->sensor_config, &pipe_contex->csi_config);
			if (ret != 0) {
				printf("No Camera Sensor found. Please check if the specified "
				       "sensor is connected to the Camera interface.\n");
				return ret;
			}
		}
	} else {
		printf("Unsupport sensor index:%d\n", index);
		print_help(argv[0]);
		return 0;
	}
	if ((pipe_contex->sensor_config->camera_config->sensor_mode == DOL2_M) && (vin_isp_is_online == 0)) {
		printf("\nError:%s's sensor_mode is DOL2_M, must work in online mode.\n\n",
		       pipe_contex->sensor_config->sensor_name);
		return -1;
	}
	printf("\n");
	printf("Connection method from VIN to ISP: %s\n",
	       (vin_isp_is_online == 1) ? "vin online isp" : "vin offline isp");
	printf("Connection method from ISP to VSE: %s.\n",
	       (isp_vse_is_online == 1) ? "isp online vse" : "isp offline vse");
	printf("\n");

	hb_mem_module_open();
	ret = create_and_run_vflow(pipe_contex);
	ERR_CON_EQ(ret, 0);

	ret = bpu_mobilenetv2_init(bpu_handle, BPU_MODEL_PATH);
	if (ret != 0) {
		printf("bpu_wrap_model_init failed, ret=%d\n", ret);
		goto cleanup_vflow;
	}
	bpu_set_ori_hw(bpu_handle, pipe_contex->sensor_config->isp_ichn_attr->width,
		       pipe_contex->sensor_config->isp_ichn_attr->height);

	running = 1;
	ret = pthread_create(&bpu_infer_thread, NULL, process_vse_bpu_infer, (void *)&app_ctx);
	pthread_join(bpu_infer_thread, NULL);

	bpu_mobilenetv2_deinit(bpu_handle);
cleanup_vflow:
	ret = hbn_vflow_stop(pipe_contex->vflow_fd);
	ERR_CON_EQ(ret, 0);
	ret = hbn_camera_destroy(pipe_contex->cam_fd);
	ERR_CON_EQ(ret, 0);
	hbn_vflow_destroy(pipe_contex->vflow_fd);
	hb_mem_module_close();

	return 0;
}
