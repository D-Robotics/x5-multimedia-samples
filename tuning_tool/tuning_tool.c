/***************************************************************************
 *                      COPYRIGHT NOTICE
 *             Copyright(C) 2024-2025, D-Robotics Co., Ltd.
 *                     All rights reserved.
 ***************************************************************************/

#include <pthread.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "tuning_cmd.h"
#include "tuning_tool.h"
#include "common_utils.h"
#include "vp_sensors.h"
#include <fcntl.h>
#include <string.h>
#include <ctype.h>

#include "common_utils.h"
#include "hb_media_codec.h"
#include "hb_media_error.h"

static void print_help() {
	printf("Usage: isp_tuning [OPTIONS]\n");
	printf("Options:\n");
	printf("  -s <sensor_index>      Specify sensor index\n");
	printf("  -t <settle_value>      Specify settle time for debug\n");
	printf("  -m <sensor_mode>       Specify sensor mode of camera_config_t\n");
	printf("  -r 1                   Send raw to hbplayer\n");
	printf("  -w 2                   Dump 20 yuv from the start\n");
	printf("  -f -H -W -F            feedback raw file xx with specified height, width, and format(raw8/raw10/raw12)\n");
	printf("  -h                     Show this help message\n");
	vp_show_sensors_list(); // Assuming this function displays sensor list
}

tuning_context_t *global_ctx;
static int settle = -1;
static uint32_t sensor_mode = 0; // 1: NORMAL_M; 2: DOL2_M; 6: SLAVE_M
static uint32_t pipelinemode = 2; // 0: Online ; 1: MCM; 2: Offline
static uint32_t feedback_raw_hight;
static uint32_t feedback_raw_width;
static char feedback_raw_format[32] = {0};
static int create_and_run_vflow(pipe_contex_t *pipe_contex, uint32_t pipelinemode, tuning_context_t * ctx);
static int32_t used_mipi_host = 0;

static int is_number(const char *str) {
	while (*str) {
		if (!isdigit(*str)) return 0;
		str++;
	}
	return 1;
}

// 分割字符串并返回数组的个数
static int split_string(const char *str, const char *delim, char *out[], int max_parts) {
	int count = 0;
	char *token;
	char *str_copy = strdup(str);
	char *rest = str_copy;

	while ((token = strtok_r(rest, delim, &rest)) && count < max_parts) {
		out[count++] = strdup(token);
	}

	free(str_copy);
	return count;
}

void parse_config(pipe_contex_info_t *pipe_contex_info, const char *config, int pipeline_idx) {
	char *parts[4];
	int sensor_idx = -1;
	int count = split_string(config, " ", parts, 4);

	for (int i = 0; i < count; i++) {
		char *key_value[2];
		int kv_count = split_string(parts[i], "=", key_value, 2);
		if (kv_count != 2) {
			fprintf(stderr, "Invalid format in config: %s\n", parts[i]);
			continue;
		}

		if (strcmp(key_value[0], "sensor") == 0) {
			if (!is_number(key_value[1])) {
				fprintf(stderr, "Invalid sensor ID: %s\n", key_value[1]);
				continue;
			}
			sensor_idx = atoi(key_value[1]);

			if (sensor_idx < vp_get_sensors_list_number() && sensor_idx >= 0) {
				pipe_contex_info->pipe_contex.sensor_config = vp_sensor_config_list[sensor_idx];
				printf("Using index:%d  sensor_name:%s  config_file:%s\n",
						sensor_idx,
						vp_sensor_config_list[sensor_idx]->sensor_name,
						vp_sensor_config_list[sensor_idx]->config_file);
			} else {
				printf("Unsupport sensor index:%d\n", sensor_idx);
				vp_show_sensors_list();
				exit(0);
			}
			if(strcmp(pipe_contex_info->pipe_contex.sensor_config->sensor_name, "dummy") != 0){
				if(vp_sensor_multi_fixed_mipi_host(pipe_contex_info->pipe_contex.sensor_config, used_mipi_host,
					&pipe_contex_info->pipe_contex.csi_config) < 0) {
					printf("vp sensor fixed mipi host fail, sensor id %d."
						"Maybe No Camera Sensor found. Please check if the specified "
						"sensor is connected to the Camera interface.\n\n", sensor_idx);
					exit(0);
				}
			}
			pipe_contex_info->select_sensor_id = sensor_idx;
			// active_mipi_host 的配置在create_vin_node函数中需要再配置一下
			pipe_contex_info->active_mipi_host = pipe_contex_info->pipe_contex.sensor_config->vin_node_attr->cim_attr.mipi_rx;
			used_mipi_host |= (1 << pipe_contex_info->pipe_contex.sensor_config->vin_node_attr->cim_attr.mipi_rx);
		} else {
			fprintf(stderr, "Unknown key: %s\n", key_value[0]);
			exit(0);
		}

		for (int j = 0; j < kv_count; j++) {
			free(key_value[j]);
		}
	}

	for (int i = 0; i < count; i++) {
		free(parts[i]);
	}
}

static int parse_opts(int argc, char *argv[], tuning_context_t *ctx, int *sensor_indexes)
{
	int32_t cmd_ret;
	int32_t raw_type;
	const char short_options[] = PARSE_SHORT_OPTS;
	const struct option long_options[] = PARSE_LONG_OPTS;
	int option_index = 0;

	if (argc == 1) {
		vp_show_sensors_list();
		return 0;
	}

	while (1) {
		cmd_ret = getopt_long(argc, argv, short_options, long_options, &option_index);
		if (cmd_ret == -1){
			break;
		}

		switch (cmd_ret) {
		case 's':
			if (ctx->sensor_count >= MAX_SENSORS) {
				fprintf(stderr, "Too many configurations. Maximum allowed is %d.\n", MAX_SENSORS);
				return 1;
			}
			parse_config(&ctx->pipe_contex_info[ctx->sensor_count], optarg, ctx->sensor_count);
			ctx->sensor_count++;
			break;
		case 't':
			settle = atoi(optarg);
			break;
		case 'm':
			sensor_mode = atoi(optarg);
			break;
		case 'r':
			ctx->send_raw = atoi(optarg);
			break;
		case 'l':
			ctx->dump_stream_flag = atoi(optarg);
			break;
		case 'w':
			ctx->work_mode = atoi(optarg);
			break;
		case 0:
			if (strcmp(long_options[option_index].name, "online") == 0) {
				printf("Online mode enabled!!!\n");
				pipelinemode = 0;
			}
			if (strcmp(long_options[option_index].name, "offline") == 0) {
				printf("Offline mode enabled!!!\n");
				pipelinemode = 2;
			}
			if (strcmp(long_options[option_index].name, "mcm") == 0) {
				printf("MCM mode enabled!!!\n");
				pipelinemode = 1;
			}
			break;
		case 'f':
			ctx->feedback_times = atoi(optarg);
			break;
		case 'H':
			feedback_raw_hight = atoi(optarg);
			break;
		case 'W':
			feedback_raw_width = atoi(optarg);
			break;
		case 'F':
			strncpy(feedback_raw_format, optarg, sizeof(feedback_raw_format) - 1);
			feedback_raw_format[sizeof(feedback_raw_format) - 1] = '\0';
			break;
		case 'h':
			print_help();
			return 0;
		default:
			print_help();
			return 0;
		}
	}

	// 处理后的参数在这里可以使用
	for (int i = 0; i < ctx->sensor_count ; i++) {
		printf("Pipeline index %d:\n", i);
		printf("\tSensor index: %d\n", ctx->pipe_contex_info[i].select_sensor_id);
		printf("\tSensor name: %s\n", ctx->pipe_contex_info[i].pipe_contex.sensor_config->sensor_name);
		printf("\tUse mipi host: %d\n", ctx->pipe_contex_info[i].active_mipi_host);
		raw_type = (!strcmp(feedback_raw_format, "raw8")) ? 0x2A : 
			(!strcmp(feedback_raw_format, "raw10")) ? 0x2B :
			(!strcmp(feedback_raw_format, "raw12")) ? 0x2C : 0x2B;

		if(strcmp(ctx->pipe_contex_info[i].pipe_contex.sensor_config->sensor_name, "dummy") == 0){
			printf("feedback_raw_width: %d feedback_raw_hight:  %d raw_type: %#X\n",feedback_raw_width, feedback_raw_hight,raw_type);
			ctx->pipe_contex_info[i].pipe_contex.sensor_config->camera_config->format = raw_type;
			ctx->pipe_contex_info[i].pipe_contex.sensor_config->camera_config->height = feedback_raw_hight;
			ctx->pipe_contex_info[i].pipe_contex.sensor_config->camera_config->width = feedback_raw_width;
			ctx->pipe_contex_info[i].pipe_contex.sensor_config->vin_ichn_attr->format = raw_type;
			ctx->pipe_contex_info[i].pipe_contex.sensor_config->vin_ichn_attr->height = feedback_raw_hight;
			ctx->pipe_contex_info[i].pipe_contex.sensor_config->vin_ichn_attr->width = feedback_raw_width;
			ctx->pipe_contex_info[i].pipe_contex.sensor_config->vin_ochn_attr->vin_basic_attr.format = raw_type;
			ctx->pipe_contex_info[i].pipe_contex.sensor_config->vin_ochn_attr->vin_basic_attr.wstride = feedback_raw_width * 2;
			ctx->pipe_contex_info[i].pipe_contex.sensor_config->vin_ochn_attr->vin_basic_attr.vstride = feedback_raw_hight;
			ctx->pipe_contex_info[i].pipe_contex.sensor_config->isp_attr->input_mode = 2;
			ctx->pipe_contex_info[i].pipe_contex.sensor_config->isp_attr->crop.w = feedback_raw_width;
			ctx->pipe_contex_info[i].pipe_contex.sensor_config->isp_attr->crop.h = feedback_raw_hight;
			ctx->pipe_contex_info[i].pipe_contex.sensor_config->isp_ichn_attr->height = feedback_raw_hight;
			ctx->pipe_contex_info[i].pipe_contex.sensor_config->isp_ichn_attr->width = feedback_raw_width;

		}
	}

	return 1;
}

static int32_t tuning_feeback_prepare_next(tuning_context_t *ctx)
{
	struct stat statbuf;
	FILE *file = NULL;

	stat(ctx->img_path[ctx->cur_img], &statbuf);
	if (!statbuf.st_size) {
		pr_tuning("No such file: %s\n", ctx->img_path[ctx->cur_img]);
		return -1;
	}
#ifdef TUNING_DEBUG
	pr_tuning("feedback file: %s, index %d, size %ld\n",
		ctx->img_path[ctx->cur_img], ctx->cur_img, statbuf.st_size);
#endif

	file = fopen(ctx->img_path[ctx->cur_img], "r");
	if (!file) {
		pr_tuning("open %s fail\n", ctx->img_path[ctx->cur_img]);
		return -1;
	}

	if ((uint32_t)statbuf.st_size > ctx->src_img.buffer.size[0]) {
		pr_tuning("alloc buffer donot match src file size!\n");
		return -1;
	}

	fread(ctx->src_img.buffer.virt_addr[0], 1, statbuf.st_size, file);
	fclose(file);

	if ((ctx->feedback_times <= 1) &&
		((ctx->cur_img + 1) >= ctx->img_num)) {
		pr_tuning("feedback raw list done!\n");
		return -1;
	}

	if ((ctx->cur_img + 1) >= ctx->img_num) {
		ctx->cur_img = 0;
		ctx->feedback_times -= 1;
	} else {
		ctx->cur_img += 1;
	}

	return 0;
}

static void *tuning_main_worker_thread(void *arg)
{
	tuning_context_t *ctx;
	int i = 0;
	int ret;
	hbn_vnode_image_t raw_img = {0};
	hbn_vnode_image_t yuv_img = {0};
	static int32_t yuv_stream_cnt = 0;
	hbn_vnode_handle_t isp_node_handle;
	hbn_vnode_handle_t vin_node_handle;
	vin_ochn_attr_t vin_oattr;
	int32_t select_sensor_id = 0;
	int32_t raw_type = 0;
	char file_name[32] = {0};

	ctx = (tuning_context_t *)arg;
	int32_t sensor_count = ctx->sensor_count;
	printf("specify connected sensors count:%d\n",ctx->sensor_count);
#ifdef TUNING_DEBUG
	pr_tuning("%s run \n", __func__);
#endif

	if (ctx->send_raw &&
		(BIT_ENABLE(ctx->work_mode, FEEDBACK_MASK) || !ctx->is_offline)) {
		pr_tuning("Cannot send raw in feedback mode or ddr disable!\n");
		goto out;
	}
	printf("Input cmd:");

	while (1) {
		for (i = 0; i < sensor_count; i++) {
			vin_node_handle = ctx->pipe_contex_info[i].pipe_contex.vin_node_handle;
			isp_node_handle = ctx->pipe_contex_info[i].pipe_contex.isp_node_handle;
			select_sensor_id = ctx->pipe_contex_info[i].select_sensor_id;
			ret = hbn_vnode_get_ochn_attr(vin_node_handle, 0, &vin_oattr);
			if (ret < 0) {
				pr_tuning("hbn_vnode_get_ochn_attr vin failed for sensor %d!\n", i);
				goto out;
			}

			ctx->is_offline = vin_oattr.ddr_en ? 1 : 0;
			ctx->vin_format = vin_oattr.vin_basic_attr.format;
			ctx->pipe_contex_info[i].pipe_contex.sensor_config->vin_ochn_attr = &vin_oattr;
			if (ctx->send_raw) {
				ret = hbn_vnode_getframe(vin_node_handle, 0, 1000, &raw_img);
				if (ret) {
					pr_tuning("get buffer from sif fail for sensor %d\n", select_sensor_id);
					goto out;
				}

				if (HBPLAYER_EN) {
					raw_type = (ctx->vin_format == 0x2A) ? RAW_8 : 
								(ctx->vin_format == 0x2B) ? RAW_10 :
								(ctx->vin_format == 0x2C) ? RAW_12 : RAW_10; // default RAW_10
					ret = tuning_send_raw_to_hbplayer(ctx->hbplayer_event, &raw_img, raw_type, i);
					if (ret)
						pr_tuning("send to hbplayer failed for sensor %d, skip it\n", i);
				}

				hbn_vnode_releaseframe(vin_node_handle, 0, &raw_img);
			}

			if (BIT_ENABLE(ctx->work_mode, FEEDBACK_MASK)) {
				ret = tuning_feeback_prepare_next(ctx);
				if (ret) goto out;
				ret = hbn_vnode_sendframe(isp_node_handle, 0, &ctx->src_img);
				if (ret) {
					pr_tuning("isp hbn_vnode_sendframe failed for sensor %d!\n", select_sensor_id);
					goto out;
				}
			}

			ret = hbn_vnode_getframe(isp_node_handle, 0, 1000, &yuv_img);
			if (ret) {
				pr_tuning("get buffer from isp fail for sensor %d\n", select_sensor_id);
				goto out;
			}

			if (ctx->yuv_dump_cnt) {
				snprintf(file_name, TUNING_PRINT_SIZE_MAX, "%s/sensor%d_stream%d.yuv", DEF_DUMP_PATH, select_sensor_id, ctx->yuv_dump_cnt);
				tuning_dump_file(file_name, &yuv_img);
				ctx->yuv_dump_cnt--;
				if (!ctx->yuv_dump_cnt) {
					yuv_stream_cnt++;
					pr_tuning("Input cmd:");
				}
			}

			if (HBPLAYER_EN) {
				ret = tuning_send_yuv_to_hbplayer(ctx->hbplayer_event, &yuv_img, i);
				if (ret)
					pr_tuning("send to hbplayer failed, skip it\n");
			}
	#ifdef TUNING_DEBUG
			// pr_tuning("get buffer size %ld-%ld\n", yuv_img.buffer.size[0], yuv_img.buffer.size[1]);
	#endif
			hbn_vnode_releaseframe(isp_node_handle, 0, &yuv_img);
			if (BIT_ENABLE(ctx->work_mode, FEEDBACK_MASK)) {
				usleep(20*1000);
			}
		}
	}
out:
	pr_tuning("typing [q] to quit\n");
	sleep(100000);

	return NULL;
}

tuning_cmd_func_t cmd_funcs[] = TUNING_CMD_FUNC_LIST;
static void tuning_api_func(int32_t cmd, tuning_context_t *ctx)
{
	int32_t i;

	for (i = 0; i < ARRAY_SIZE(cmd_funcs); i++) {
		if (cmd_funcs[i].cmd == cmd) {
			cmd_funcs[i].api_func(ctx);
			printf("Input cmd:");
			return;
		}
	}

	if (cmd != 'h')
		printf("Unknown cmd: %c!\n", (char)cmd);
	valid_cmd_print();
	printf("Input cmd:");
}

static void *tuning_api_worker_thread(void *arg)
{
	tuning_context_t *ctx = (tuning_context_t *)arg;

	main_while_func_run(tuning_api_func, ctx)

	return NULL;
}

static int32_t tuning_feeback_init(tuning_context_t *ctx)
{
	isp_ichn_attr_t ichn_attr = {0};
	char file_path[255] = {0};
	int i = 0;
	hbn_vnode_handle_t isp_node_handle;
	int32_t sensor_count = ctx->sensor_count;

	if (getcwd(file_path, sizeof(file_path)) != NULL) {
#ifdef TUNING_DEBUG
		pr_tuning("Current working directory: %s\n", file_path);
#endif
	} else {
		pr_tuning("getcwd fail\n");
		return -1;
	}

	FUNC_EQ(tuning_get_raw_list(file_path, ctx->img_path, ctx->img_name, &ctx->img_num), 0, return -1);
	if (ctx->img_num < 1) {
		pr_tuning("no raw file found in working directory\n");
		return -1;
	}

	for (i = 0; i < sensor_count; i++) {
		isp_node_handle = ctx->pipe_contex_info[i].pipe_contex.isp_node_handle;
		FUNC_EQ(hbn_vnode_get_ichn_attr(isp_node_handle, 0, &ichn_attr), 0, return -1);
		FUNC_EQ(tuning_alloc_feedback_buffer(&ctx->src_img.buffer, ichn_attr.width, ichn_attr.height, 0), 0, return -1);
	}

	return 0;
}

static int32_t tuning_case_run(tuning_context_t *ctx, int32_t *sensor_indexes)
{
	int32_t ret = 0;

	if (BIT_ENABLE(ctx->work_mode, FEEDBACK_MASK))
		FUNC_EQ(tuning_feeback_init(ctx), 0, goto destroy_cam);

	if (HBPLAYER_EN) {
		ctx->hbplayer_event = hb_tool_start_transfer(0);
		hb_tool_event_setcb(ctx->hbplayer_event, NULL, NULL, NULL, NULL, NULL);
	}

	if (BIT_ENABLE(ctx->work_mode, START_DUMP_MASK))
		ctx->yuv_dump_cnt = 20;

	FUNC_EQ(pthread_create(&ctx->main_thid, NULL, tuning_main_worker_thread, (void *)(ctx)), 0, goto destroy);
	FUNC_EQ(pthread_create(&ctx->api_thid, NULL, tuning_api_worker_thread, (void *)(ctx)), 0, goto destroy);

	pthread_join(ctx->api_thid, NULL);
#ifdef TUNING_DEBUG
	pr_tuning("api thread join done\n");
#endif

	pthread_cancel(ctx->main_thid);
#ifdef TUNING_DEBUG
	pr_tuning("main thread cancel done\n");
#endif

	if (HBPLAYER_EN) {
		hb_tool_stop_transfer(ctx->hbplayer_event);
	}
#ifdef TUNING_DEBUG
	pr_tuning("stop transfer done\n");
#endif

destroy:
	if (BIT_ENABLE(ctx->work_mode, FEEDBACK_MASK))
		tuning_free_feedback_buffer(&ctx->src_img.buffer);

destroy_cam:

	for (int i = 0; i < ctx->sensor_count; ++i) {
		ret = hbn_vflow_stop(ctx->pipe_contex_info[i].pipe_contex.vflow_fd);
		if (ret != 0) {
			pr_tuning("hbn_vflow_stop failed for sensor %d. ret = %d\n", sensor_indexes[i], ret);
		}
		hbn_camera_destroy(ctx->pipe_contex_info[i].pipe_contex.cam_fd);
		hbn_vflow_destroy(ctx->pipe_contex_info[i].pipe_contex.vflow_fd);
	}

	pr_tuning("vflow destory done\n");

	if (ctx->err_cnt) {
		pr_tuning("some error: %d occurred\n", ctx->err_cnt);
		return -1;
	}

	return ret;
}


static int create_camera_node(pipe_contex_t *pipe_contex) {

	camera_config_t *camera_config = NULL;
	vp_sensor_config_t *sensor_config = NULL;
	int32_t ret = 0;

	sensor_config = pipe_contex->sensor_config;
	camera_config = sensor_config->camera_config;
	/* Debug settle */
	if (settle >= 0 && settle <= 127) {
		camera_config->mipi_cfg->rx_attr.settle = settle;
	}
	if (sensor_mode >= NORMAL_M && sensor_mode < INVALID_MOD) {
		camera_config->sensor_mode = sensor_mode;
		sensor_config->vin_node_attr->lpwm_attr.enable = 1;
	}
	ret = hbn_camera_create(camera_config, &pipe_contex->cam_fd);
	ERR_CON_EQ(ret, 0);

	return 0;
}

static int create_vin_node(pipe_contex_t *pipe_contex, uint32_t pipelinemode) {
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

	if(pipelinemode == Online){
		vin_node_attr->cim_attr.cim_isp_flyby = 1;
		vin_ochn_attr->ddr_en = 0;
	}

	if(pipelinemode == MCM){
		vin_node_attr->cim_attr.cim_isp_flyby = 1;
		vin_ochn_attr->ddr_en = 1;
	}

	if(pipelinemode == Offline){
		vin_node_attr->cim_attr.cim_isp_flyby = 0;
		vin_ochn_attr->ddr_en = 1;
	}

	if(pipe_contex->csi_config.mclk_is_not_configed){
		//设备树中没有配置mclk：使用外部晶振
		printf("csi%d ignore mclk ex attr, because not config mclk.\n",
			pipe_contex->csi_config.index);
	}else{
		vin_attr_ex.vin_attr_ex_mask = sensor_config->vin_attr_ex->vin_attr_ex_mask;
		vin_attr_ex.mclk_ex_attr.mclk_freq = sensor_config->vin_attr_ex->mclk_ex_attr.mclk_freq;
	}

	ret = hbn_vnode_open(HB_VIN, hw_id, AUTO_ALLOC_ID, vin_node_handle);
	ERR_CON_EQ(ret, 0);
	// 设置基本属性
	ret = hbn_vnode_set_attr(*vin_node_handle, vin_node_attr);
	ERR_CON_EQ(ret, 0);
	// 设置输入通道的属性
	ret = hbn_vnode_set_ichn_attr(*vin_node_handle, ichn_id, vin_ichn_attr);
	ERR_CON_EQ(ret, 0);
	// 设置输出通道的属性
	ret = hbn_vnode_set_ochn_attr(*vin_node_handle, ochn_id, vin_ochn_attr);
	ERR_CON_EQ(ret, 0);

	vin_attr_ex_mask = vin_attr_ex.vin_attr_ex_mask;
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

	if(pipelinemode == Offline || pipelinemode == MCM){
		hbn_buf_alloc_attr_t alloc_attr = {0};
		alloc_attr.buffers_num = 3;
		alloc_attr.is_contig = 1;
		alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN
							| HB_MEM_USAGE_CPU_WRITE_OFTEN
							| HB_MEM_USAGE_CACHED
							| HB_MEM_USAGE_HW_CIM
							| HB_MEM_USAGE_GRAPHIC_CONTIGUOUS_BUF;
		ret = hbn_vnode_set_ochn_buf_attr(*vin_node_handle, ochn_id, &alloc_attr);
		ERR_CON_EQ(ret, 0);
	}
	return 0;
}

static int create_isp_node(pipe_contex_t *pipe_contex, uint32_t pipelinemode) {
	vp_sensor_config_t *sensor_config = NULL;
	isp_attr_t      *isp_attr = NULL;
	isp_ichn_attr_t *isp_ichn_attr = NULL;
	isp_ochn_attr_t *isp_ochn_attr = NULL;
	hbn_vnode_handle_t *isp_node_handle = NULL;
	hbn_buf_alloc_attr_t alloc_attr = {0};
	uint32_t ichn_id = 0;
	uint32_t ochn_id = 0;
	int ret = 0;

	sensor_config = pipe_contex->sensor_config;
	isp_attr = sensor_config->isp_attr;
	isp_ichn_attr = sensor_config->isp_ichn_attr;
	isp_ochn_attr = sensor_config->isp_ochn_attr;
	isp_node_handle = &pipe_contex->isp_node_handle;
	if(pipelinemode == Online){
		isp_attr->input_mode = 0;
	}

	if(pipelinemode == MCM){
		isp_attr->input_mode = 1;
	}

	if(pipelinemode == Offline){
		isp_attr->input_mode = 2;
	}

	ret = hbn_vnode_open(HB_ISP, 0, AUTO_ALLOC_ID, isp_node_handle);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_attr(*isp_node_handle, isp_attr);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_ochn_attr(*isp_node_handle, ochn_id, isp_ochn_attr);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_ichn_attr(*isp_node_handle, ichn_id, isp_ichn_attr);
	ERR_CON_EQ(ret, 0);

	alloc_attr.buffers_num = 3;
	alloc_attr.is_contig = 1;
	alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN
						| HB_MEM_USAGE_CPU_WRITE_OFTEN
						| HB_MEM_USAGE_CACHED;
	ret = hbn_vnode_set_ochn_buf_attr(*isp_node_handle, ochn_id, &alloc_attr);
	ERR_CON_EQ(ret, 0);

	return 0;
}


int create_and_run_vflow(pipe_contex_t *pipe_contex, uint32_t pipelinemode, tuning_context_t * ctx) {
	int32_t ret = 0;

	// 创建pipeline中的每个node
	ret = create_camera_node(pipe_contex);
	ERR_CON_EQ(ret, 0);
	ret = create_vin_node(pipe_contex, pipelinemode);
	ERR_CON_EQ(ret, 0);
	ret = create_isp_node(pipe_contex, pipelinemode);
	ERR_CON_EQ(ret, 0);

	// 创建HBN flow
	ret = hbn_vflow_create(&pipe_contex->vflow_fd);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_add_vnode(pipe_contex->vflow_fd,
							pipe_contex->vin_node_handle);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_add_vnode(pipe_contex->vflow_fd,
							pipe_contex->isp_node_handle);
	ERR_CON_EQ(ret, 0);

	if (!BIT_ENABLE(ctx->work_mode, FEEDBACK_MASK)) {
		if(pipelinemode == Online){
			ret = hbn_vflow_bind_vnode(pipe_contex->vflow_fd,
							pipe_contex->vin_node_handle,
							1,
							pipe_contex->isp_node_handle,
							0);
			ERR_CON_EQ(ret, 0);
		}

		if(pipelinemode == MCM){
			ret = hbn_vflow_bind_vnode(pipe_contex->vflow_fd,
							pipe_contex->vin_node_handle,
							1,
							pipe_contex->isp_node_handle,
							0);
			ERR_CON_EQ(ret, 0);
		}

		if(pipelinemode == Offline){
			ret = hbn_vflow_bind_vnode(pipe_contex->vflow_fd,
							pipe_contex->vin_node_handle,
							0,
							pipe_contex->isp_node_handle,
							0);
			ERR_CON_EQ(ret, 0);
		}
	}
	ret = hbn_camera_attach_to_vin(pipe_contex->cam_fd,
							pipe_contex->vin_node_handle);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_start(pipe_contex->vflow_fd);
	ERR_CON_EQ(ret, 0);

	return 0;
}

int32_t main(int argc, char *argv[])
{
	int32_t ret = 0;
	tuning_context_t ctx = {0};
	int32_t sensor_indexes[MAX_SENSORS] = {-1};
	global_ctx = &ctx;

	ret = parse_opts(argc, argv, &ctx, sensor_indexes);
	if (ret != 1) {
		return ret;
	}

	if (access(DEF_DUMP_PATH, 0)) {
		ret = mkdir(DEF_DUMP_PATH, 0777);
		if (ret < 0) {
			pr_tuning("mkdir %s for dump failed !\n", DEF_DUMP_PATH);
			return -1;
		}
	}

	if (BIT_ENABLE(ctx.work_mode, FEEDBACK_MASK)) {
		pr_tuning("tuning_tool run with feedback\n");
		ctx.feedback_times = ctx.feedback_times == 0 ? 0xFFFF : ctx.feedback_times;
	}

	lut3d_map_init();

	for (int i = 0; i < ctx.sensor_count; ++i) {
		// printf("pipelinemode: %d\n",pipelinemode);
		ret = create_and_run_vflow(&ctx.pipe_contex_info[i].pipe_contex, pipelinemode, &ctx);
		if (ret != 0) {
			printf("create_and_run_vflow failed for sensor %d. ret = %d\n", sensor_indexes[i], ret);
			return ret;
		}
	}

	FUNC_EQ(tuning_case_run(&ctx, sensor_indexes), 0, return -1);

	return ret;
}
