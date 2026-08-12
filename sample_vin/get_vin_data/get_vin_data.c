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
#include "sensor_json_param_parser.h"

#include "common_utils.h"

#define MAX_SENSORS 4
#define FRAME_CHECK_TIMEOUT_NS 5000000000
#define DEBUG

static struct option const long_options[] = {
	{"sensor", required_argument, NULL, 's'},
	{"mode", optional_argument, NULL, 'm'},
	{"extern_param", optional_argument, NULL, 'e'},
	{"fps", required_argument, NULL, 'f'},
	{NULL, 0, NULL, 0}
};

static int create_and_run_vflow(pipe_contex_t *pipe_contex, uint32_t target_fps);
static void handle_user_command(pipe_contex_t *pipe_contex, int sensor_count);
static int verify_sensor_framerate(pipe_contex_t* pipe_contex, uint32_t target_fps);
static void set_target_fps(pipe_contex_t *pipe_contex, uint32_t fps);

int32_t hbn_deserial_create(deserial_config_t *des_config, deserial_handle_t *des_fd);
int32_t hbn_deserial_attach_to_vin(deserial_handle_t des_fd, camera_des_link_t link, vpf_handle_t vin_fd);


static void print_help() {
	printf("Usage: get_vin_data [OPTIONS]\n");
	printf("Options:\n");
	printf("  -s <sensor_index>      Specify sensor index\n");
	printf("  -m <sensor_mode>       Specify sensor mode of camera_config_t, support 'm' and 's'\n");
	printf("  		m: Master\n");
	printf("  		s: Slave\n");
	printf("  		The default value is determined by the configuration in vp_sensor\n");
	printf("  -e <extern_param>	     Specify extension parameters through a JSON file\n");
	printf("  -f <target_fps>        Specify target output FPS (0 = disable, sensor native)\n");
	printf("  -h                     Show this help message\n");
	vp_show_sensors_list(); // Assuming this function displays sensor list
}

static void command_help() {
	printf("\n");
	printf("***************  Command Lists  ***************\n");
	printf(" g	-- get single frame \n");
	printf(" l	-- get a set frames \n");
	printf(" f	-- check frame rate and compare with configured fps \n");
	printf(" s	-- set target FPS \n");
	printf(" q	-- quit  \n");
	printf(" h	-- print help message\n");
}

static uint32_t sensor_mode = INVALID_MOD;
static uint32_t sensors_target_fps[MAX_SENSORS] = {0};

static int parse_args(int argc, char **argv, int *sensor_indexes,
		      int *sensor_count, char *json_param_file,
		      size_t json_file_size)
{
	int opt_index = 0;
	int c;

	while ((c = getopt_long(argc, argv, "s:e:m:f:h",
				long_options, &opt_index)) != -1) {
		switch (c) {
		case 's':
			if (*sensor_count < MAX_SENSORS) {
				sensor_indexes[(*sensor_count)++] = atoi(optarg);
			} else {
				printf("Maximum number of sensors exceeded\n");
				return 0;
			}
			break;
		case 'e':
			strncpy(json_param_file, optarg, json_file_size - 1);
			json_param_file[json_file_size - 1] = '\0';
			printf("input json param file is %s\n", json_param_file);
			break;
		case 'm':
			if (strcmp(optarg, "m") == 0) {
				sensor_mode = NORMAL_M;
			} else if (strcmp(optarg, "s") == 0) {
				sensor_mode = SLAVE_M;
			} else {
				printf("Invalid sensor mode %s.\n", optarg);
				return -1;
			}
			break;
		case 'f':
			if (*sensor_count > 0) {
				sensors_target_fps[*sensor_count - 1] = (uint32_t)atoi(optarg);
				printf("Sensor[%d] target FPS: %u (%s)\n",
				       *sensor_count - 1,
				       sensors_target_fps[*sensor_count - 1],
				       sensors_target_fps[*sensor_count - 1] > 0
				       ? "ratio skip enabled" : "disable skip, sensor native");
			}
			break;
		case 'h':
		default:
			print_help();
			return 1;
		}
	}
	return 0;
}

int main(int argc, char** argv) {
	int ret = 0;
	pipe_contex_t pipe_contex[MAX_SENSORS] = {0};
	int index = -1;
	int sensor_indexes[MAX_SENSORS];
	int sensor_count = 0;
	int used_mipi_host = 0;

	for (int i = 0; i < MAX_SENSORS; i++)
		sensor_indexes[i] = -1;

	char json_param_file[128] = {0};
	ret = parse_args(argc, argv, sensor_indexes, &sensor_count,
			 json_param_file, sizeof(json_param_file));
	if (ret != 0) {
		return -1;
	}

	if (sensor_count == 0) {
		printf("No sensors specified.\n");
		print_help();
		return 0;
	}

	for (int i = 0; i < sensor_count; ++i) {
		index = sensor_indexes[i];
		if (index < vp_get_sensors_list_number() && index >= 0) {
			pipe_contex[i].sensor_config = vp_sensor_config_list[index];
			printf("Using index:%d  sensor_name:%s  config_file:%s\n",
					index,
					vp_sensor_config_list[index]->sensor_name,
					vp_sensor_config_list[index]->config_file);

			if(pipe_contex[i].sensor_config->sensor_type != SENSOR_TYPE_NORMAL)
				continue;
			ret = vp_sensor_multi_fixed_mipi_host(pipe_contex[i].sensor_config,
				used_mipi_host, &pipe_contex[i].csi_config);
			if (ret != 0) {
				printf("No Camera Sensor found. Please check if the specified "
					"sensor is connected to the Camera interface.\n");
				return ret;
			}
			used_mipi_host |= (1 << pipe_contex[i].csi_config.index);
		} else {
			printf("Unsupported sensor index:%d\n", index);
			print_help();
			return 0;
		}
	}

	//检测 sensor_mode
	if(sensor_mode != INVALID_MOD){
		for (int i = 0; i < sensor_count; ++i) {
			int is_found_from_support_list = 0;
			for (int j = 0; j < SENSOR_MODE_SUPPORT_COUNT; j++){
				if(sensor_mode == pipe_contex[i].sensor_config->support_sensor_mode[j]){
					is_found_from_support_list = 1;
				}
			}
			int default_sensor_mode = pipe_contex[i].sensor_config->camera_config->sensor_mode;
			if((is_found_from_support_list == 0) && (sensor_mode != default_sensor_mode)){
				printf("\nError: index %d  sensor_name(%s) not support input mode: %s\n\n",
						i, pipe_contex[i].sensor_config->sensor_name, sensor_mode_to_str(sensor_mode));
				return -1;
			}else{
				if(i == 0){
					printf("\n\n");
				}
				printf("Index %d sensor_name(%s) specify sensor mode to %s\n",
					i, pipe_contex[i].sensor_config->sensor_name, sensor_mode_to_str(sensor_mode));
			}
		}
	}else{
		printf("Not config sensor mode, so use default mode.");
	}
	printf("\n\n");

	//[For Debug]点亮sensor过程中可能需要对某些参数频繁改动：可以选择通过 json 文件更新部分的 Sensor 配置参数
	if(strlen(json_param_file) > 0){
		printf("Attention:Update the sensor configuration using the parameters described in the JSON file: [%s]\n",
				json_param_file);
		for (int i = 0; i < sensor_count; ++i) {
			ret = sensor_param_parse_from_file(json_param_file, pipe_contex[i].sensor_config);
			if(ret != 0){
				return -1;
			}
		}
	}
	hb_mem_module_open();
	for (int i = 0; i < sensor_count; ++i) {
		ret = create_and_run_vflow(&pipe_contex[i], sensors_target_fps[i]);
		if (ret != 0) {
			printf("create_and_run_vflow failed for sensor %d. ret = %d\n", sensor_indexes[i], ret);
			return ret;
		}
	}

	// init sensors_target_fps[]
	for (int i = 0; i < sensor_count; i++) {
		if (sensors_target_fps[i])
			continue;
		sensors_target_fps[i] =
		    pipe_contex[i].sensor_config->camera_config->fps;
	}

	handle_user_command(pipe_contex, sensor_count);

	for (int i = 0; i < sensor_count; ++i) {
		ret = hbn_vflow_stop(pipe_contex[i].vflow_fd);
		if (ret != 0) {
			printf("hbn_vflow_stop failed for sensor %d. ret = %d\n", sensor_indexes[i], ret);
		}
		hbn_vnode_close(pipe_contex[i].vin_node_handle);
		hbn_camera_destroy(pipe_contex[i].cam_fd);
		hbn_vflow_destroy(pipe_contex[i].vflow_fd);
	}
	hb_mem_module_close();
	return 0;
}

static int create_camera_node(pipe_contex_t *pipe_contex) {

	camera_config_t *camera_config = NULL;
	vp_sensor_config_t *sensor_config = NULL;
	int32_t ret = 0;

	sensor_config = pipe_contex->sensor_config;
	camera_config = sensor_config->camera_config;

	if (sensor_mode >= NORMAL_M && sensor_mode < INVALID_MOD) {
		camera_config->sensor_mode = sensor_mode;
		if(sensor_mode == SLAVE_M){
			sensor_config->vin_node_attr->lpwm_attr.enable = 1;
		}else{
			sensor_config->vin_node_attr->lpwm_attr.enable = 0;
		}
	}
	ret = hbn_camera_create(camera_config, &pipe_contex->cam_fd);
	ERR_CON_EQ(ret, 0);

	return 0;
}

static int create_deserial_node(pipe_contex_t *pipe_contex) {

	vp_sensor_config_t *sensor_config = NULL;
	deserial_config_t *deserial_config = NULL;
	deserial_handle_t *des_handle = NULL;

	int32_t ret = 0;
	des_handle = &pipe_contex->des_fd;

	sensor_config = pipe_contex->sensor_config;
	deserial_config = sensor_config->deserial_node_attr;

	ret = hbn_deserial_create(deserial_config, des_handle);
	if(ret != 0){
		printf("hbn_deserial_create failed ret = %d\n", ret);
		return ret;
	}
	printf("deserial_config:,%02x,%s, des_handle:%ld \n\r" ,deserial_config->addr,
	deserial_config->name, *des_handle);
	return 0;
}


static int create_vin_node(pipe_contex_t *pipe_contex, uint32_t target_fps) {
	vp_sensor_config_t *sensor_config = NULL;
	vin_node_attr_t *vin_node_attr = NULL;
	vin_ichn_attr_t *vin_ichn_attr = NULL;
	vin_ochn_attr_t *vin_ochn_attr = NULL;
	hbn_vnode_handle_t *vin_node_handle = NULL;
	vin_attr_ex_t vin_attr_ex = {0};
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

	hw_id = pipe_contex->csi_config.index;
	vin_node_handle = &pipe_contex->vin_node_handle;

	if(pipe_contex->csi_config.mclk_is_not_configed){
		// 设备树中没有配置 mclk：使用外部晶振
		printf("csi%d ignore mclk ex attr, because not config mclk.\n",
			pipe_contex->csi_config.index);
	}else{
		vin_attr_ex.vin_attr_ex_mask = sensor_config->vin_attr_ex->vin_attr_ex_mask;
		vin_attr_ex.mclk_ex_attr.mclk_freq = sensor_config->vin_attr_ex->mclk_ex_attr.mclk_freq;
	}

	/* merge per-sensor target_fps (from -f) into the same ex_attr channel */
	uint32_t sensor_fps = sensor_config->camera_config->fps;
	if (target_fps > 0 && target_fps < sensor_fps) {
		vin_attr_ex.vin_attr_ex_mask |= (1ULL << VIN_DYNAMIC_FPS_CTRL);
		vin_attr_ex.fps_ctrl.skip_mode = CIM_IN_RATIO_SKIP;
		vin_attr_ex.fps_ctrl.in_fps    = sensor_fps;
		vin_attr_ex.fps_ctrl.out_fps   = target_fps;
		printf("[FPS] Sensor[%s] %u -> %u fps (init)\n",
			sensor_config->camera_config->name, sensor_fps, target_fps);
	}
	vin_attr_ex_mask = vin_attr_ex.vin_attr_ex_mask;

	ret = hbn_vnode_open(HB_VIN, hw_id, AUTO_ALLOC_ID, vin_node_handle);
	ERR_CON_EQ(ret, 0);
	// 设置基本属性
	ret = hbn_vnode_set_attr(*vin_node_handle, vin_node_attr);
	ERR_CON_EQ(ret, 0);
	// 设置输入通道的属性
	ret = hbn_vnode_set_ichn_attr(*vin_node_handle, ichn_id, vin_ichn_attr);
	ERR_CON_EQ(ret, 0);
	// 设置输出通道的属性
	// 使能 DDR 输出
	vin_ochn_attr->ddr_en = 1;
	ret = hbn_vnode_set_ochn_attr(*vin_node_handle, ochn_id, vin_ochn_attr);
	ERR_CON_EQ(ret, 0);

	if (vin_attr_ex_mask) {
		for (uint8_t i = 0; i < VIN_ATTR_EX_INVALID; i ++) {
			if ((vin_attr_ex_mask & (1 << i)) == 0)
				continue;

			vin_attr_ex.ex_attr_type = i;
			/*we need to set hbn_vnode_set_attr_ex in a loop*/
			ret = hbn_vnode_set_attr_ex(*vin_node_handle, &vin_attr_ex);
			ERR_CON_EQ(ret, 0);
		}
	}
	alloc_attr.buffers_num = 3;
	alloc_attr.is_contig = 1;
	alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN
						| HB_MEM_USAGE_CPU_WRITE_OFTEN
						| HB_MEM_USAGE_CACHED;
	ret = hbn_vnode_set_ochn_buf_attr(*vin_node_handle, ochn_id, &alloc_attr);
	ERR_CON_EQ(ret, 0);

	return 0;
}

int create_and_run_vflow(pipe_contex_t *pipe_contex, uint32_t target_fps) {
	int32_t ret = 0;
	uint32_t sensor_type = pipe_contex->sensor_config->sensor_type;
	uint32_t link_port = pipe_contex->sensor_config->vin_node_attr->cim_attr.vc_index;

	// 创建 pipeline 中的每个 node
	ret = create_camera_node(pipe_contex);
	ERR_CON_EQ(ret, 0);
	ret = create_vin_node(pipe_contex, target_fps);
	ERR_CON_EQ(ret, 0);

	// 创建 HBN flow
	ret = hbn_vflow_create(&pipe_contex->vflow_fd);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_add_vnode(pipe_contex->vflow_fd,
							pipe_contex->vin_node_handle);
	ERR_CON_EQ(ret, 0);

	if((sensor_type != SENSOR_TYPE_NORMAL) && (sensor_type != SENSOR_TYPE_HSMT_RAW)) {
		ret = create_deserial_node(pipe_contex);
		ERR_CON_EQ(ret, 0);
		ret = hbn_camera_attach_to_deserial(pipe_contex->cam_fd, pipe_contex->des_fd, link_port);
		ERR_CON_EQ(ret, 0);
		ret = hbn_deserial_attach_to_vin(pipe_contex->des_fd, link_port, pipe_contex->vin_node_handle);
		ERR_CON_EQ(ret, 0);
	}else {
		ret = hbn_camera_attach_to_vin(pipe_contex->cam_fd,
							pipe_contex->vin_node_handle);
		ERR_CON_EQ(ret, 0);
	}

	ret = hbn_vflow_start(pipe_contex->vflow_fd);
	ERR_CON_EQ(ret, 0);

	return 0;
}


static void vin_dump_func(pipe_contex_t *pipe_contex) {
	int ret;
	char dst_file[128];
	uint32_t ochn_id = 0;
	uint32_t timeout = 10000;
	hbn_vnode_image_t out_img;
	hbn_vnode_handle_t vin_node_handle = pipe_contex->vin_node_handle;
	uint32_t date_type = pipe_contex->sensor_config->camera_config->format;
	uint32_t link_port = pipe_contex->sensor_config->vin_node_attr->cim_attr.vc_index;

	// 获取帧数据
	ret = hbn_vnode_getframe(vin_node_handle, ochn_id, timeout, &out_img);
	if (ret != 0) {
		printf("Error: hbn_vnode_getframe failed for sensor[%s]@csi%d vin chn:%d (ret=%d)\n",
		       pipe_contex->sensor_config->sensor_name, pipe_contex->csi_config.index, ochn_id, ret);
		return;
	}

	// 生成基础文件名
	snprintf(dst_file, sizeof(dst_file),
			 "handle_%d_chn%d_vc%d_%dx%d_stride_%d_frameid_%d_ts_%ld",
			 (int)vin_node_handle, ochn_id, link_port,
			 out_img.buffer.width, out_img.buffer.height, out_img.buffer.stride,
			 out_img.info.frame_id, out_img.info.timestamps);

	// 处理不同数据格式
	switch (date_type) {
		case SENSOR_DATA_TYPE_RAW10:
		case SENSOR_DATA_TYPE_RAW12:
		case SENSOR_DATA_TYPE_RAW14:
			strcat(dst_file, ".raw");
			printf("Dumping RAW data: handle %d, resolution: %dx%d (stride: %d), size: %ld, frame id: %d, timestamp: %ld\n",
					(int)vin_node_handle,
					out_img.buffer.width, out_img.buffer.height,
					out_img.buffer.stride,
					out_img.buffer.size[0],
					out_img.info.frame_id,
					out_img.info.timestamps);
			ret = dump_image_to_file(dst_file, out_img.buffer.virt_addr[0], out_img.buffer.size[0]);
			if (ret != 0)
				printf("Error: Failed to dump RAW to file %s (ret=%d)\n", dst_file, ret);
			break;

		case SENSOR_DATA_TYPE_YUV422:
			strcat(dst_file, ".yuv");
			printf("Dumping YUV data: handle %d, resolution: %dx%d (stride: %d), size: %ld + %ld, frame id: %d, timestamp: %ld\n",
					(int)vin_node_handle,
					out_img.buffer.width, out_img.buffer.height,
					out_img.buffer.stride,
					out_img.buffer.size[0], out_img.buffer.size[1],
					out_img.info.frame_id,
					out_img.info.timestamps);
			ret = dump_2plane_yuv_to_file(dst_file,
					out_img.buffer.virt_addr[0],
					out_img.buffer.virt_addr[1],
					out_img.buffer.size[0],
					out_img.buffer.size[1]);
			if (ret != 0) {
				printf("Error: Failed to dump YUV to file\n");
			} else {
				printf("Dump successful: %s\n", dst_file);
			}
			break;

		default:
			printf("Error: Unsupported data type %d\n", out_img.buffer.format);
			break;
	}

	// 释放帧数据
	hbn_vnode_releaseframe(vin_node_handle, ochn_id, &out_img);
}

static void set_target_fps(pipe_contex_t *pipe_contex, uint32_t fps)
{
	int ret = 0;
	uint32_t sensor_fps =
		pipe_contex->sensor_config->camera_config->fps;
	vin_attr_ex_t vin_attr_ex = {0};
	const char *name = pipe_contex->sensor_config->camera_config->name;

	vin_attr_ex.ex_attr_type = VIN_DYNAMIC_FPS_CTRL;

	if (fps <= 0 || fps > sensor_fps) {
		vin_attr_ex.fps_ctrl.skip_mode = CIM_DISABLE_SKIP;
		vin_attr_ex.fps_ctrl.in_fps = 0;
		vin_attr_ex.fps_ctrl.out_fps = 0;
	} else {
		vin_attr_ex.fps_ctrl.skip_mode = CIM_IN_RATIO_SKIP;
		vin_attr_ex.fps_ctrl.in_fps = sensor_fps;
		vin_attr_ex.fps_ctrl.out_fps = fps;
	}
	ret = hbn_vnode_set_attr_ex(pipe_contex->vin_node_handle, &vin_attr_ex);
	if (ret) {
		printf("Sensor[%s]: set dynamic FPS failed (ret=%d)\n",
		       name, ret);
		return;
	}

	if (fps > 0 && fps <= sensor_fps)
		printf("[FPS] Sensor[%s] %u -> %u fps\n",
		       name, sensor_fps, fps);
	else
		printf("[FPS] Sensor[%s] native %u fps\n",
		       name, sensor_fps);
}

static int check_frame_rate(uint32_t target_fps, uint32_t frame_count,
			uint64_t start_ts, uint64_t end_ts,
			uint32_t start_fid, uint32_t end_fid)
{
	double diff = 0;
	double avg_fps = 0;
	uint64_t ts_span = end_ts - start_ts;

	if (ts_span == 0)
		return -2;

	avg_fps = (double)frame_count / (double)(ts_span / 1000000000.0);


	printf("Statistics (timestamp): Average FPS = %.2f\n", avg_fps);
	printf("First frame_id = %u, Last frame_id = %u, Frame count = %d\n",
		start_fid, end_fid, frame_count);
	printf("First timestamp = %ld, Last timestamp = %ld\n", start_ts, end_ts);

	diff = avg_fps - target_fps;
	if (diff < 0)
		diff = -diff;
	if (diff > 1.0)
		return -1;

	return 0;
}

static int verify_sensor_framerate(pipe_contex_t* pipe_contex, uint32_t target_fps)
{
	uint32_t frame_count = 0, start_fid = 0, current_fid = 0;
	uint64_t start_ts = 0, current_ts = 0;
	hbn_vnode_image_t img = {0};
	hbn_vnode_handle_t vin = pipe_contex->vin_node_handle;

	// discard the underlying buffer
	for (int i = 0; i < 2; i++) {
		if (hbn_vnode_getframe(vin, 0, 10000, &img) != 0) {
			printf("Sensor[%s]@csi%d: getframe failed during discard, skip frame rate check\n",
			       pipe_contex->sensor_config->sensor_name, pipe_contex->csi_config.index);
			return -1;
		}
		hbn_vnode_releaseframe(vin, 0, &img);
	}

	// starting getframe...
	printf( "\n======================== Frame Rate Check "
	        "=======================\n");
	printf("Sensor[%s]@csi%d: Target FPS = %ufps\n",
	       pipe_contex->sensor_config->sensor_name, pipe_contex->csi_config.index, target_fps);
	fflush(stdout);
	for (frame_count = 0; ((current_ts - start_ts) < FRAME_CHECK_TIMEOUT_NS) &&
	                      (frame_count < 1000); frame_count++) {
		if (hbn_vnode_getframe(vin, 0, 10000, &img) != 0)
			break;
		if (frame_count == 0) {
			start_ts = img.info.timestamps;
			start_fid = img.info.frame_id;
		}
		current_ts = img.info.timestamps;
		current_fid = img.info.frame_id;
		hbn_vnode_releaseframe(vin, 0, &img);
	}

	// check fps
	int ret = check_frame_rate(target_fps, frame_count, start_ts, current_ts,
	                           start_fid, current_fid);
	printf("Sensor[%s]@csi%d: Frame rate check: ", pipe_contex->sensor_config->sensor_name, pipe_contex->csi_config.index);
	if (!ret) {
		printf("Success\n");
	} else {
		printf("Failed\n");
	}
	printf( "=================== Frame Rate Check Complete "
		"===================\n");

	return ret;
}

static void print_command_prompt(pipe_contex_t *pipe_contex, int sensor_count)
{
	printf("Command [");
	for (int k = 0; k < sensor_count; k++) {
		uint32_t d = sensors_target_fps[k];
		if (d == 0)
			d = pipe_contex[k].sensor_config->camera_config->fps;
		printf("%s%s:%ufps", k > 0 ? ", " : "",
			pipe_contex[k].sensor_config->camera_config->name, d);
	}
	printf("]: ");
}


static void handle_set_fps_cmd(pipe_contex_t *pipe_contex, int sensor_count)
{
	char input_buf[32] = {0};
	uint32_t sensor_idx = 0, fps_val = 0;
	int ch;

	while ((ch = getchar()) != '\n' && ch != EOF)
		;
	printf("\nEnter sensor index (0..%d): ", sensor_count - 1);
	fflush(stdout);
	if (fgets(input_buf, sizeof(input_buf), stdin) == NULL)
		return;
	sensor_idx = (uint32_t)atoi(input_buf);
	if (sensor_idx >= (uint32_t)sensor_count) {
		printf("Invalid sensor index %u (max %d)\n",
		       sensor_idx, sensor_count - 1);
		return;
	}

	printf("Enter target FPS (0 = disable skip, restore native): ");
	fflush(stdout);
	if (fgets(input_buf, sizeof(input_buf), stdin) == NULL)
		return;
	fps_val = (uint32_t)atoi(input_buf);
	uint32_t s_fps =
		pipe_contex[sensor_idx].sensor_config->camera_config->fps;
	if (fps_val == 0 || fps_val <= s_fps) {
		sensors_target_fps[sensor_idx] = (fps_val == 0) ? s_fps : fps_val;
		set_target_fps(&pipe_contex[sensor_idx], fps_val);
		printf("Use 'f' to check frame rate\n");
	} else {
		printf("Invalid FPS %u: valid range is 0 (disable) or 1..%u\n",
		       fps_val, s_fps);
	}
}

static void handle_user_command(pipe_contex_t *pipe_contex, int sensor_count)
{
	int i, j;
	char option = 'a';
	int running = 1;
	command_help();
	print_command_prompt(pipe_contex, sensor_count);

	while (running && ((option = getchar()) != EOF)) {
		switch (option) {
			case 'q':
				printf("quit\n");
				running = 0;
				return;
			case 'g':  // get a raw file for all sensors
				for (i = 0; i < sensor_count; i++) {
					vin_dump_func(&pipe_contex[i]);
				}
				break;
			case 'l': // get multiple frames for all sensors
				for (j = 0; j < 12; j++) {
					for (i = 0; i < sensor_count; i++) {
						vin_dump_func(&pipe_contex[i]);
					}
				}
				break;
			case 'f':  // check fps for all sensors
				for (i = 0; i < sensor_count; i++) {
				    int ret = verify_sensor_framerate(&pipe_contex[i],
				                              sensors_target_fps[i]);
				    if (ret != 0)
				        printf("Sensor[%s]@csi%d: frame rate check aborted\n",
				               pipe_contex[i].sensor_config->sensor_name, pipe_contex[i].csi_config.index);
				}
				break;
			case 's': // set target fps one by one
				handle_set_fps_cmd(pipe_contex, sensor_count);
				break;
			case 'h':
				command_help();
				break;
			case '\n':
			case '\r':
				continue;
			default:
				printf("Command not supported!\n");
				command_help();
				break;
		}

		print_command_prompt(pipe_contex, sensor_count);
	}

	return;
}
