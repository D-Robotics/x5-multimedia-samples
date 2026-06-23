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
#include <stdlib.h>
#include "cjson/cJSON.h"

#include "common_utils.h"
#include "hb_media_codec.h"
#include "hb_media_error.h"
#include "vp_display.h"

// 由于不需要预览多路，一次只需要初始化一个 channel， 默认使用 vse channel 0 ，如果需要使用 channel 1 ，那就更改 VSE_CHANNELS_USED 为 1 。
#define VSE_CHANNELS_USED 0
#define VSE_WIDTH_TARGET 640

static void print_help() {
	printf("Usage: isp_tuning [OPTIONS]\n");
	printf("Options:\n");
	printf("  -s <sensor_index>      Specify sensor index\n");
	printf("  -t <settle_value>      Specify settle time for debug\n");
	printf("  -m <sensor_mode>       Specify sensor mode of camera_config_t\n");
	printf("  -r 1                   Send raw to hbplayer\n");
	printf("  -w 2                   Dump 20 yuv from the start\n");
	printf("  -f -H -W -F            feedback raw file xx with specified height, width, and format(raw8/raw10/raw12)\n");
	printf("  -J file                set feedback json file for dummy sensor\n");
	printf("  -h                     Show this help message\n");
	vp_show_sensors_list(); // Assuming this function displays sensor list
}

tuning_context_t *global_ctx;
static int settle = -1;
static int pdaf_en = 0;
static uint32_t sensor_mode = 0; // 1: NORMAL_M; 2: DOL2_M; 6: SLAVE_M
static uint32_t pipelinemode = 2; // 0: Online ; 1: MCM; 2: Offline
static uint32_t enable_vse = 0; // 0: disable_vse ; 1: enable_vse
static uint32_t enable_hdmi = 0; // 0: disable ; 1: enable hdmi preview (via VSE->DRM)
static uint32_t feedback_raw_hight = 1080;
static uint32_t feedback_raw_width = 1920;
static char feedback_raw_format[32] = "raw10";
static char feedback_param_file[256] = {0};
static char feedback_calib_lname[128] = {0};
static int32_t used_mipi_host = 0;
static uint32_t link_port = 0;

static vp_drm_context_t g_drm_context;
static int32_t g_drm_inited = 0;
static uint32_t g_hdmi_width = 0;
static uint32_t g_hdmi_height = 0;
static int32_t g_hdmi_modes_printed = 0;

static int32_t parse_feedback_param_from_file(const char *file_path, char *sensor_param_buf, size_t sensor_param_buf_size)
{
	FILE *fp = NULL;
	long file_size = 0;
	size_t read_size = 0;
	char *json_str = NULL;
	cJSON *root = NULL;
	cJSON *item = NULL;
	char *sensor_param_compact = NULL;

	if (file_path == NULL || file_path[0] == '\0') {
		return RET_FAILURE;
	}

	fp = fopen(file_path, "r");
	if (fp == NULL) {
		printf("Failed to open feedback json file: %s\n", file_path);
		return RET_FAILURE;
	}
	fseek(fp, 0, SEEK_END);
	file_size = ftell(fp);
	if (file_size <= 0) {
		fclose(fp);
		printf("Invalid feedback json file size: %ld\n", file_size);
		return RET_FAILURE;
	}
	fseek(fp, 0, SEEK_SET);

	json_str = (char *)malloc((size_t)file_size + 1);
	if (json_str == NULL) {
		fclose(fp);
		return RET_FAILURE;
	}
	read_size = fread(json_str, 1, (size_t)file_size, fp);
	fclose(fp);
	if (read_size != (size_t)file_size) {
		free(json_str);
		return RET_FAILURE;
	}
	json_str[file_size] = '\0';

	root = cJSON_Parse(json_str);
	free(json_str);
	if (root == NULL) {
		printf("Invalid json format in feedback file: %s\n", file_path);
		return RET_FAILURE;
	}

	item = cJSON_GetObjectItem(root, "height");
	if (cJSON_IsNumber(item)) {
		feedback_raw_hight = (uint32_t)item->valueint;
	}
	item = cJSON_GetObjectItem(root, "width");
	if (cJSON_IsNumber(item)) {
		feedback_raw_width = (uint32_t)item->valueint;
	}
	item = cJSON_GetObjectItem(root, "format");
	if (cJSON_IsString(item) && item->valuestring) {
		strncpy(feedback_raw_format, item->valuestring, sizeof(feedback_raw_format) - 1);
		feedback_raw_format[sizeof(feedback_raw_format) - 1] = '\0';
	}
	item = cJSON_GetObjectItem(root, "calib_lname");
	if (cJSON_IsString(item) && item->valuestring) {
		strncpy(feedback_calib_lname, item->valuestring, sizeof(feedback_calib_lname) - 1);
		feedback_calib_lname[sizeof(feedback_calib_lname) - 1] = '\0';
	}

	sensor_param_compact = cJSON_PrintUnformatted(root);
	if (sensor_param_compact != NULL) {
		strncpy(sensor_param_buf, sensor_param_compact, sensor_param_buf_size - 1);
		sensor_param_buf[sensor_param_buf_size - 1] = '\0';
		cJSON_free(sensor_param_compact);
	}
	cJSON_Delete(root);
	return RET_SUCCESS;
}

static int32_t tuning_pick_hdmi_resolution(int32_t input_width, int32_t input_height,
	int32_t *out_width, int32_t *out_height)
{
	int32_t ret;

	ret = vp_display_get_fit_smaller_resolution(input_width, input_height, out_width, out_height);
	if (ret >= 0) {
		return ret;
	}

	/* 若 HDMI 只支持比输入更大的分辨率，则选择“最小的可用模式”，避免直接失败 */
	int drm_fd = drmOpen("vs-drm", NULL);
	if (drm_fd < 0) {
		perror("drmOpen failed");
		return -1;
	}

	drmModeRes *resources = drmModeGetResources(drm_fd);
	if (!resources) {
		perror("drmModeGetResources failed");
		close(drm_fd);
		return -1;
	}

	drmModeConnector *best_conn = NULL;
	int best_area = 0;
	int best_w = 0;
	int best_h = 0;

	for (int i = 0; i < resources->count_connectors; i++) {
		drmModeConnector *conn = drmModeGetConnector(drm_fd, resources->connectors[i]);
		if (!conn) {
			continue;
		}
		if (conn->connector_type != DRM_MODE_CONNECTOR_HDMIA ||
			conn->connection != DRM_MODE_CONNECTED ||
			conn->count_modes <= 0) {
			drmModeFreeConnector(conn);
			continue;
		}

		for (int m = 0; m < conn->count_modes; m++) {
			int w = conn->modes[m].hdisplay;
			int h = conn->modes[m].vdisplay;
			int area = w * h;
			if (best_area == 0 || area < best_area) {
				best_area = area;
				best_w = w;
				best_h = h;
			}
		}
		best_conn = conn;
		break;
	}

	if (best_conn) {
		drmModeFreeConnector(best_conn);
	}
	drmModeFreeResources(resources);
	close(drm_fd);

	if (best_area > 0) {
		*out_width = best_w;
		*out_height = best_h;
		return 0;
	}

	return -1;
}

unsigned short lut3d_map[LUT_SIZE][LUT_SIZE][LUT_SIZE][3];

int32_t hbn_deserial_create(deserial_config_t *des_config, deserial_handle_t *des_fd);
int32_t hbn_deserial_attach_to_vin(deserial_handle_t des_fd, camera_des_link_t link, vpf_handle_t vin_fd);

static int is_number(const char *str) {
	while (*str) {
		if (!isdigit(*str)) return RET_SUCCESS;
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
			uint32_t sensor_type = pipe_contex_info->pipe_contex.sensor_config->sensor_type;
			if(sensor_type != SENSOR_TYPE_NORMAL)
				continue;
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

static int parse_opts(int argc, char *argv[], tuning_context_t *ctx)
{
	int32_t cmd_ret;
	int32_t raw_type;
	int32_t bit_width;
	const char short_options[] = PARSE_SHORT_OPTS;
	const struct option long_options[] = PARSE_LONG_OPTS;
	int option_index = 0;

	if (argc == 1) {
		vp_show_sensors_list();
		return RET_SUCCESS;
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
			if (strcmp(long_options[option_index].name, "enable_vse") == 0) {
				printf("VSE enabled!!!\n");
				enable_vse = 1;
			}
			if (strcmp(long_options[option_index].name, "hdmi") == 0) {
				printf("HDMI preview enabled!!!\n");
				enable_hdmi = 1;
				enable_vse = 1; // HDMI 预览依赖 VSE 输出到适配的分辨率
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
		case 'J':
			strncpy(feedback_param_file, optarg, sizeof(feedback_param_file) - 1);
			feedback_param_file[sizeof(feedback_param_file) - 1] = '\0';
			break;
		case 'a':
			bit_mask(ctx->work_mode, LUT3D_MASK);
			break;
		case 'h':
			print_help();
			return RET_SUCCESS;
		default:
			print_help();
			return RET_SUCCESS;
		}
	}

	// 处理后的参数在这里可以使用
	for (int i = 0; i < ctx->sensor_count ; i++) {
		printf("Pipeline index %d:\n", i);
		printf("\tSensor index: %d\n", ctx->pipe_contex_info[i].select_sensor_id);
		printf("\tSensor name: %s\n", ctx->pipe_contex_info[i].pipe_contex.sensor_config->sensor_name);
		printf("\tUse mipi host: %d\n", ctx->pipe_contex_info[i].active_mipi_host);
		if(strcmp(ctx->pipe_contex_info[i].pipe_contex.sensor_config->sensor_name, "dummy") == 0){
			static char feedback_sensor_param_json[2048] = {0};
			if (feedback_param_file[0] != '\0') {
				parse_feedback_param_from_file(feedback_param_file, feedback_sensor_param_json, sizeof(feedback_sensor_param_json));
			}
			raw_type = (!strcmp(feedback_raw_format, "raw8")) ? 0x2A :
				(!strcmp(feedback_raw_format, "raw10")) ? 0x2B :
				(!strcmp(feedback_raw_format, "raw12")) ? 0x2C :
				(!strcmp(feedback_raw_format, "raw14")) ? 0x2D :
				(!strcmp(feedback_raw_format, "raw16")) ? 0x2E :0x2B;
			bit_width = (!strcmp(feedback_raw_format, "raw8")) ? 8 :
				(!strcmp(feedback_raw_format, "raw10")) ? 10 :
				(!strcmp(feedback_raw_format, "raw12")) ? 12 :
				(!strcmp(feedback_raw_format, "raw14")) ? 14 :
				(!strcmp(feedback_raw_format, "raw16")) ? 16 : 10;
			printf("feedback_raw_width: %d feedback_raw_hight:  %d raw_type: %#X\n",
				feedback_raw_width, feedback_raw_hight, raw_type);
			ctx->pipe_contex_info[i].pipe_contex.sensor_config->camera_config->format = raw_type;
			ctx->pipe_contex_info[i].pipe_contex.sensor_config->camera_config->height = feedback_raw_hight;
			ctx->pipe_contex_info[i].pipe_contex.sensor_config->camera_config->width = feedback_raw_width;
			if (feedback_sensor_param_json[0] != '\0') {
				ctx->pipe_contex_info[i].pipe_contex.sensor_config->camera_config->sensor_param = feedback_sensor_param_json;
			}
			if (feedback_calib_lname[0] != '\0') {
				strncpy(ctx->pipe_contex_info[i].pipe_contex.sensor_config->camera_config->calib_lname,
					feedback_calib_lname,
					sizeof(ctx->pipe_contex_info[i].pipe_contex.sensor_config->camera_config->calib_lname) - 1);
				ctx->pipe_contex_info[i].pipe_contex.sensor_config->camera_config->calib_lname[
					sizeof(ctx->pipe_contex_info[i].pipe_contex.sensor_config->camera_config->calib_lname) - 1] = '\0';
			}
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
			ctx->pipe_contex_info[i].pipe_contex.sensor_config->isp_ichn_attr->bit_width = bit_width;

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
		return RET_FAILURE;
	}
#ifdef TUNING_DEBUG
	pr_tuning("feedback file: %s, index %d, size %ld\n",
		ctx->img_path[ctx->cur_img], ctx->cur_img, statbuf.st_size);
#endif

	file = fopen(ctx->img_path[ctx->cur_img], "r");
	if (!file) {
		pr_tuning("open %s fail\n", ctx->img_path[ctx->cur_img]);
		return RET_FAILURE;
	}

	if ((uint32_t)statbuf.st_size > ctx->src_img.buffer.size[0]) {
		pr_tuning("alloc buffer donot match src file size!\n");
		return RET_FAILURE;
	}

	fread(ctx->src_img.buffer.virt_addr[0], 1, statbuf.st_size, file);
	fclose(file);

	return RET_SUCCESS;
}

static void *tuning_main_worker_thread(void *arg)
{
	int i = 0;
	int ret;
	hbn_vnode_image_t raw_img_pdaf = {0};
	hbn_vnode_image_t raw_img_main = {0};
	hbn_vnode_image_t yuv_img = {0};
	hbn_vnode_image_t vse_img[VSE_CHANNELS_USED + 1] = {0};
	static int32_t yuv_stream_cnt = 0;
	hbn_vnode_handle_t vse_node_handle;
	hbn_vnode_handle_t isp_node_handle;
	hbn_vnode_handle_t vin_node_handle;
	enum RAW_BIT raw_type;
	char file_name[32] = {0};
	tuning_context_t *ctx = (tuning_context_t *)arg;;
	int32_t dump_index = 0;
	hbn_isp_af_attr_t af_attr = {0};

#ifdef TUNING_DEBUG
	pr_tuning("%s run sensor count: %d\n", __func__, ctx->sensor_count);
#endif

	if (ctx->send_raw) {
		if (BIT_ENABLE(ctx->work_mode, FEEDBACK_MASK)) {
			pr_tuning("Cannot send raw in feedback mode!\n");
			goto out;
		}
		for (i = 0; i < ctx->sensor_count; i++) {
			if (!ctx->pipe_contex_info[i].is_offline) {
				pr_tuning("Sensor-%d ddr disable, cannot send raw!\n", i);
				goto out;
			}
		}
	}
	printf("Input cmd:");

	while (1) {
	for (i = 0; i < ctx->sensor_count; i++) {
		vin_node_handle = ctx->pipe_contex_info[i].pipe_contex.vin_node_handle;
		isp_node_handle = ctx->pipe_contex_info[i].pipe_contex.isp_node_handle;
		vse_node_handle = ctx->pipe_contex_info[i].pipe_contex.vse_node_handle;

		if (ctx->send_raw) {
			raw_type = (ctx->pipe_contex_info[i].vin_format == 0x2A) ? RAW_8 :
				(ctx->pipe_contex_info[i].vin_format == 0x2B) ? RAW_10 :
				(ctx->pipe_contex_info[i].vin_format == 0x2C) ? RAW_12 :
				(ctx->pipe_contex_info[i].vin_format == 0x2D) ? RAW_14 : RAW_10;

			if (pdaf_en) {
				ret = hbn_vnode_getframe(vin_node_handle, VIN_PDAF, 1500, &raw_img_pdaf);
				if (ret) {
					pr_tuning("Sensor-%d get PDAF buffer from vin fail\n", i);
					goto out;
				}
				if (HBPLAYER_EN) {
					ret = tuning_send_raw_to_hbplayer(ctx->hbplayer_event, &raw_img_pdaf, raw_type, i, 1);
					if (ret)
						pr_tuning("send PDAF to hbplayer failed for sensor %d, skip it\n", i);
				}
				hbn_vnode_releaseframe(vin_node_handle, VIN_PDAF, &raw_img_pdaf);

				ret = hbn_vnode_getframe(vin_node_handle, VIN_MAIN_FRAME, 1500, &raw_img_main);
				if (ret) {
					pr_tuning("Sensor-%d get MAIN_FRAME buffer from vin fail\n", i);
					goto out;
				}
				if (HBPLAYER_EN) {
					ret = tuning_send_raw_to_hbplayer(ctx->hbplayer_event, &raw_img_main, raw_type, i, 0);
					if (ret)
						pr_tuning("send MAIN_FRAME to hbplayer failed for sensor %d, skip it\n", i);
				}
				hbn_vnode_releaseframe(vin_node_handle, VIN_MAIN_FRAME, &raw_img_main);
			} else {
				ret = hbn_vnode_getframe(vin_node_handle, VIN_MAIN_FRAME, 1500, &raw_img_main);
				if (ret) {
					pr_tuning("Sensor-%d get MAIN_FRAME buffer from vin fail\n", i);
					goto out;
				}
				if (HBPLAYER_EN) {
					ret = tuning_send_raw_to_hbplayer(ctx->hbplayer_event, &raw_img_main, raw_type, i, 0);
					if (ret)
						pr_tuning("send MAIN_FRAME to hbplayer failed for sensor %d, skip it\n", i);
				}
				hbn_vnode_releaseframe(vin_node_handle, VIN_MAIN_FRAME, &raw_img_main);
			}
		}

		if (BIT_ENABLE(ctx->work_mode, FEEDBACK_MASK)) {
			if ((ctx->feedback_times >= 1) && (ctx->cur_img < ctx->img_num)) {
				ret = tuning_feeback_prepare_next(ctx);
				if (ret == RET_FAILURE) goto out;
			} else {
				if (i == 0 && ctx->run_feedback_fv == 1) {
					ctx->cur_img = 0;
					ctx->feedback_times = 1;
					ctx->run_feedback_fv = 0;
					ret = tuning_feeback_prepare_next(ctx);
					if (ret == RET_FAILURE) goto out;
				} else {
					usleep(20*1000);
					continue;
				}
			}

			ret = hbn_vnode_sendframe(isp_node_handle, 0, &ctx->src_img);
			if (ret) {
				pr_tuning("isp hbn_vnode_sendframe failed!\n");
				goto out;
			}

			if ((ctx->cur_img + 1) >= ctx->img_num) {
				if (ctx->feedback_times <= 1)
					pr_tuning("feedback raw list done!\n");
				ctx->cur_img = 0;
				ctx->feedback_times -= 1;
			} else {
				ctx->cur_img += 1;
			}
		}

		if (i == 0 && (ctx->run_fv == 1 || ctx->run_fv > FV_DELAY_FRAME - 2) && ctx->feedback_fv == 0) {
			af_attr.mode = HBN_ISP_MODE_MANUAL;
			af_attr.position = ctx->cur_focal;

			if (!BIT_ENABLE(ctx->work_mode, FEEDBACK_MASK)) {
				ret = hbn_isp_set_af_attr(isp_node_handle, &af_attr);
				if (ret) {
					pr_tuning("Set Focal fail\n");
					goto out;
				}
			}
		}

		if(enable_vse)
		{
			ret = hbn_vnode_getframe(vse_node_handle, i, 2000, &vse_img[VSE_CHANNELS_USED]);
			if (ret != 0) {
				printf("hbn_vnode_getframe VSE channel %d failed\n", i);
				continue;
			}
		}
		ret = hbn_vnode_getframe(isp_node_handle, 0, 1500, &yuv_img);
		if (ret) {
			pr_tuning("Sensor-%d get buffer from isp fail\n", i);
			goto out;
		}
		if (ctx->pipe_contex_info[i].yuv_dump_cnt) {
			snprintf(file_name, TUNING_PRINT_SIZE_MAX, "%s/ISP_S%d_STREAM%d.yuv", DEF_DUMP_PATH, i, dump_index++);
			tuning_dump_yuv_file(file_name, &yuv_img);
			ctx->pipe_contex_info[i].yuv_dump_cnt--;
			if (!ctx->pipe_contex_info[i].yuv_dump_cnt) {
				yuv_stream_cnt++;
				pr_tuning("Input cmd:");
			}
		}

		if (HBPLAYER_EN) {
			if(enable_vse)
			{
				ret = tuning_send_yuv_to_hbplayer(ctx->hbplayer_event, &vse_img[VSE_CHANNELS_USED], i);
			}
			else
			{
				ret = tuning_send_yuv_to_hbplayer(ctx->hbplayer_event, &yuv_img, i);
			}
			if (ret)
				pr_tuning("send to hbplayer failed, skip it\n");
		}

		if (enable_hdmi && enable_vse) {
			if (!g_drm_inited) {
				int32_t drm_w = (int32_t)g_hdmi_width;
				int32_t drm_h = (int32_t)g_hdmi_height;

				if (!g_hdmi_modes_printed) {
					vp_display_print_supported_resolutions();
					g_hdmi_modes_printed = 1;
				}

				ret = vp_display_check_hdmi_is_connected();
				if (ret < 0) {
					printf("\n\nFailed: output form is hdmi, but not found hdmi connector.\n\n");
				} else {
					/*
					 * 优先使用 create_vse_node() 已经选好的 HDMI 输出分辨率；
					 * 避免 img_width/img_height 尚未就绪时传入 0 导致反复失败。
					 */
					if (drm_w <= 0 || drm_h <= 0) {
						int32_t input_width = (int32_t)ctx->pipe_contex_info[i].img_width;
						int32_t input_height = (int32_t)ctx->pipe_contex_info[i].img_height;
						if (input_width > 0 && input_height > 0) {
							int32_t out_w = 0, out_h = 0;
							if (tuning_pick_hdmi_resolution(input_width, input_height, &out_w, &out_h) >= 0) {
								drm_w = out_w;
								drm_h = out_h;
								g_hdmi_width = (uint32_t)out_w;
								g_hdmi_height = (uint32_t)out_h;
							}
						}
					}

					if (drm_w > 0 && drm_h > 0) {
						ret = vp_display_init(&g_drm_context, drm_w, drm_h);
						if (ret == 0) {
							g_drm_inited = 1;
							printf("vp_display_init ok: %dx%d\n", drm_w, drm_h);
						} else {
							printf("hdmi init failed.\n");
						}
					} else {
						printf("hdmi resolution not ready, skip init this round\n");
					}
				}
			}

			if (g_drm_inited) {
				ret = vp_display_set_frame(&g_drm_context, &vse_img[VSE_CHANNELS_USED].buffer);
				if (ret) {
					printf("vp_display_set_frame for hdmi failed %d.\n", ret);
				}
			}
		}
#ifdef TUNING_DEBUG
		// pr_tuning("get buffer size %ld-%ld\n", yuv_img.buffer.size[0], yuv_img.buffer.size[1]);
#endif
		hbn_vnode_releaseframe(isp_node_handle, 0, &yuv_img);
		if(enable_vse)
		{
			hbn_vnode_releaseframe(vse_node_handle, i, &vse_img[VSE_CHANNELS_USED]);
		}
		if (BIT_ENABLE(ctx->work_mode, FEEDBACK_MASK)) {
			usleep(20*1000);
		}

		if (i == 0 && (ctx->run_fv == 1 || ctx->feedback_fv == 1)) {
			cmd_header_new_t head = {0};
			tuning_fv_buffer_t fv_buf = {0};
			int raw, col;
			hbn_isp_af_statistics_t af_statistics = {0};
			hbn_isp_afmv1_statistics_t afmv1_statistics= {0};

			if (ctx->afm_version == 0) {
				hbn_isp_get_afmv1_statistics(isp_node_handle, &afmv1_statistics);
				fv_buf.fv += afmv1_statistics.sharpness_a;
				fv_buf.fv += afmv1_statistics.sharpness_b;
				fv_buf.fv += afmv1_statistics.sharpness_c;
			} else {
				hbn_isp_get_af_statistics(isp_node_handle, &af_statistics);
				if (ctx->block_select == 0) {
					for (raw = 0; raw < 15; raw++) {
					for (col = 0; col < 15; col++) {
						if (ctx->afm_version == 1) {
							fv_buf.fv += af_statistics.sharpnessHighPass[raw * 15 + col];
						} else {
							fv_buf.fv += af_statistics.sharpnessLowPass[raw * 15 + col];
						}
					}
					}
				} else {
					ctx->block_select = ctx->block_select < 0 ? 1: ctx->block_select;
					ctx->block_select = ctx->block_select > 225 ? 225: ctx->block_select;
					if (ctx->afm_version == 1) {
						fv_buf.fv = af_statistics.sharpnessHighPass[ctx->block_select - 1];
					} else {
						fv_buf.fv = af_statistics.sharpnessLowPass[ctx->block_select - 1];
					}
				}
			}
			fv_buf.pos = ctx->cur_focal;

			head.len = sizeof(tuning_fv_buffer_t);
			head.type = STATS_AF_DATA;
			head.format = FV_CURVE_RUN;
			hb_tool_used_define_pic(ctx->hbplayer_event, &head, &fv_buf, sizeof(tuning_fv_buffer_t));

			if (ctx->cur_focal + ctx->step >= ctx->max_focal && ctx->feedback_fv == 0) {
				ctx->run_fv = 0;
				continue;
			}
			ctx->cur_focal += ctx->step;
		}
		if (i == 0 && ctx->run_fv > 1) ctx->run_fv--;
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

	select_id(ctx, &ctx->handle_id);
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
	hbn_vnode_handle_t isp_node_handle;

	if (getcwd(file_path, sizeof(file_path)) != NULL) {
#ifdef TUNING_DEBUG
		pr_tuning("Current working directory: %s\n", file_path);
#endif
	} else {
		pr_tuning("Getcwd fail\n");
		return RET_FAILURE;
	}

	FUNC_EQ(tuning_get_raw_list(file_path, ctx->img_path, ctx->img_name, &ctx->img_num), 0, return RET_FAILURE);
	if (ctx->img_num < 1) {
		pr_tuning("No raw file found in working directory\n");
		return RET_FAILURE;
	}

	isp_node_handle = ctx->pipe_contex_info[0].pipe_contex.isp_node_handle;
	FUNC_EQ(hbn_vnode_get_ichn_attr(isp_node_handle, 0, &ichn_attr), 0, return RET_FAILURE);
	FUNC_EQ(tuning_alloc_feedback_buffer(&ctx->src_img.buffer, ichn_attr.width, ichn_attr.height, 0), 0, return RET_FAILURE);

	return RET_SUCCESS;
}

static const char *kernelSource =
"inline float3 lut_lookup(float3 rgb, __global const unsigned short* lut_ptr, int lut_size) {\n"
"    float3 coord = rgb * (float)(lut_size - 1);\n"
"    int3 idx0 = convert_int3(coord);\n"
"    int3 idx1 = idx0 + (int3)(1, 1, 1);\n"
"    idx1 = min(idx1, (int3)(lut_size - 1));\n"
"\n"
"    float3 d = coord - convert_float3(idx0);\n"
"\n"
"    int y_stride = lut_size;\n"
"    int x_stride = lut_size * lut_size;\n"
"\n"
"    int off000 = idx0.x * x_stride + idx0.y * y_stride + idx0.z;\n"
"    int off001 = idx0.x * x_stride + idx0.y * y_stride + idx1.z;\n"
"    int off010 = idx0.x * x_stride + idx1.y * y_stride + idx0.z;\n"
"    int off011 = idx0.x * x_stride + idx1.y * y_stride + idx1.z;\n"
"    int off100 = idx1.x * x_stride + idx0.y * y_stride + idx0.z;\n"
"    int off101 = idx1.x * x_stride + idx0.y * y_stride + idx1.z;\n"
"    int off110 = idx1.x * x_stride + idx1.y * y_stride + idx0.z;\n"
"    int off111 = idx1.x * x_stride + idx1.y * y_stride + idx1.z;\n"
"\n"
"    float3 c000 = convert_float3(vload3(off000, lut_ptr));\n"
"    float3 c001 = convert_float3(vload3(off001, lut_ptr));\n"
"    float3 c010 = convert_float3(vload3(off010, lut_ptr));\n"
"    float3 c011 = convert_float3(vload3(off011, lut_ptr));\n"
"    float3 c100 = convert_float3(vload3(off100, lut_ptr));\n"
"    float3 c101 = convert_float3(vload3(off101, lut_ptr));\n"
"    float3 c110 = convert_float3(vload3(off110, lut_ptr));\n"
"    float3 c111 = convert_float3(vload3(off111, lut_ptr));\n"
"\n"
"    float3 c00 = mix(c000, c001, d.z);\n"
"    float3 c01 = mix(c010, c011, d.z);\n"
"    float3 c10 = mix(c100, c101, d.z);\n"
"    float3 c11 = mix(c110, c111, d.z);\n"
"\n"
"    float3 c0 = mix(c00, c01, d.y);\n"
"    float3 c1 = mix(c10, c11, d.y);\n"
"\n"
"    return mix(c0, c1, d.x);\n"
"}\n"
"\n"
"__kernel void apply_3dlut(__global const unsigned char* buf_src,\n"
"                          __global unsigned char* buf_dst,\n"
"                          __global const unsigned short* lut3d_map,\n"
"                          int lut_size,\n"
"                          int img_height,\n"
"                          int img_width) {\n"
"    int block_y = get_global_id(0);\n"
"    int block_x = get_global_id(1);\n"
"    int x = block_x * 2;\n"
"    int y = block_y * 2;\n"
"\n"
"    if (x >= img_width || y >= img_height) return;\n"
"\n"
"    int stride = img_width;\n"
"    int uv_idx = stride * img_height + block_y * stride + block_x * 2;\n"
"\n"
"    unsigned char u_val = buf_src[uv_idx];\n"
"    unsigned char v_val = buf_src[uv_idx + 1];\n"
"    float u_f = (float)u_val - 128.0f;\n"
"    float v_f = (float)v_val - 128.0f;\n"
"\n"
"    int y_idx_00 = y * stride + x;\n"
"    int y_idx_01 = y_idx_00 + 1;\n"
"    int y_idx_10 = (y + 1) * stride + x;\n"
"    int y_idx_11 = y_idx_10 + 1;\n"
"\n"
"    float4 Y;\n"
"    Y.s0 = (float)buf_src[y_idx_00];\n"
"    Y.s1 = (float)buf_src[y_idx_01];\n"
"    Y.s2 = (float)buf_src[y_idx_10];\n"
"    Y.s3 = (float)buf_src[y_idx_11];\n"
"\n"
"    float4 R = Y + (float4)(1.403f * v_f);\n"
"    float4 G = Y - (float4)(0.344f * u_f) - (float4)(0.714f * v_f);\n"
"    float4 B = Y + (float4)(1.772f * u_f);\n"
"\n"
"    R = clamp(R, 0.0f, 255.0f) * (1.0f / 255.0f);\n"
"    G = clamp(G, 0.0f, 255.0f) * (1.0f / 255.0f);\n"
"    B = clamp(B, 0.0f, 255.0f) * (1.0f / 255.0f);\n"
"\n"
"    float3 res00 = lut_lookup((float3)(R.s0, G.s0, B.s0), lut3d_map, lut_size);\n"
"    float3 res01 = lut_lookup((float3)(R.s1, G.s1, B.s1), lut3d_map, lut_size);\n"
"    float3 res10 = lut_lookup((float3)(R.s2, G.s2, B.s2), lut3d_map, lut_size);\n"
"    float3 res11 = lut_lookup((float3)(R.s3, G.s3, B.s3), lut3d_map, lut_size);\n"
"\n"
"    res00 *= (1.0f / 256.0f); res01 *= (1.0f / 256.0f);\n"
"    res10 *= (1.0f / 256.0f); res11 *= (1.0f / 256.0f);\n"
"\n"
"    float4 Rf = (float4)(res00.x, res01.x, res10.x, res11.x);\n"
"    float4 Gf = (float4)(res00.y, res01.y, res10.y, res11.y);\n"
"    float4 Bf = (float4)(res00.z, res01.z, res10.z, res11.z);\n"
"\n"
"    float4 Y_out = 0.299f * Rf + 0.587f * Gf + 0.114f * Bf;\n"
"    float u_out_f = 128.0f + (-0.169f * Rf.s0 - 0.331f * Gf.s0 + 0.5f * Bf.s0);\n"
"    float v_out_f = 128.0f + (0.5f * Rf.s0 - 0.419f * Gf.s0 - 0.081f * Bf.s0);\n"
"\n"
"    buf_dst[y_idx_00] = (unsigned char)clamp((int)Y_out.s0, 0, 255);\n"
"    buf_dst[y_idx_01] = (unsigned char)clamp((int)Y_out.s1, 0, 255);\n"
"    buf_dst[y_idx_10] = (unsigned char)clamp((int)Y_out.s2, 0, 255);\n"
"    buf_dst[y_idx_11] = (unsigned char)clamp((int)Y_out.s3, 0, 255);\n"
"\n"
"    buf_dst[uv_idx]     = (unsigned char)clamp((int)u_out_f, 0, 255);\n"
"    buf_dst[uv_idx + 1] = (unsigned char)clamp((int)v_out_f, 0, 255);\n"
"}\n";

static int32_t tuning_opencl_3dlut_init(pipe_contex_info_t *pipe_info)
{
	cl_int ret;
	size_t log_size;
	char *log;
	size_t img_size;
	tuning_opencl_ctx_t *opencl_ctx = &pipe_info->opencl_ctx;

	ret = clGetPlatformIDs(1, &opencl_ctx->platform, NULL);
	if (ret != CL_SUCCESS) {
		pr_tuning("clGetPlatformIDs fail, ret: %d\n", ret);
		return RET_FAILURE;
	}

	ret = clGetDeviceIDs(opencl_ctx->platform, CL_DEVICE_TYPE_GPU, 1, &opencl_ctx->device, NULL);
	if (ret != CL_SUCCESS) {
		pr_tuning("clGetDeviceIDs fail, ret: %d\n", ret);
		return RET_FAILURE;
	}

	opencl_ctx->context = clCreateContext(NULL, 1, &opencl_ctx->device, NULL, NULL, &ret);
	if (ret != CL_SUCCESS) {
		pr_tuning("clCreateContext fail, ret: %d\n", ret);
		return RET_FAILURE;
	}

	opencl_ctx->queue = clCreateCommandQueue(opencl_ctx->context, opencl_ctx->device, 0, &ret);
	if (ret != CL_SUCCESS) {
		pr_tuning("clCreateCommandQueue fail, ret: %d\n", ret);
		return RET_FAILURE;
	}

	opencl_ctx->program = clCreateProgramWithSource(opencl_ctx->context, 1, &kernelSource, NULL, &ret);
	if (ret != CL_SUCCESS) {
		pr_tuning("clCreateCommandQueue fail, ret: %d\n", ret);
		return RET_FAILURE;
	}

	ret = clBuildProgram(opencl_ctx->program, 1, &opencl_ctx->device, NULL, NULL, NULL);
	if (ret != CL_SUCCESS) {
		pr_tuning("clBuildProgram fail, ret: %d\n", ret);
		clGetProgramBuildInfo(opencl_ctx->program, opencl_ctx->device, CL_PROGRAM_BUILD_LOG, 0, NULL, &log_size);

		log = (char*)malloc(log_size + 1);
		if (log) {
			clGetProgramBuildInfo(opencl_ctx->program, opencl_ctx->device, CL_PROGRAM_BUILD_LOG, log_size, log, NULL);
			log[log_size] = '\0';
			pr_tuning("Build Log: \n%s\n", log);
			free(log);
		}
		return RET_FAILURE;
	}

	opencl_ctx->kernel = clCreateKernel(opencl_ctx->program, "apply_3dlut", &ret);
	if (ret != CL_SUCCESS) {
		pr_tuning("clCreateKernel fail, ret: %d\n", ret);
		return RET_FAILURE;
	}

	img_size = pipe_info->img_width * pipe_info->img_height * 1.5;
	opencl_ctx->input_image = clCreateBuffer(opencl_ctx->context, CL_MEM_READ_ONLY, img_size, NULL, &ret);
	if (ret != CL_SUCCESS) {
		pr_tuning("clCreateBuffer fail, ret: %d\n", ret);
		return RET_FAILURE;
	}

	opencl_ctx->output_image = clCreateBuffer(opencl_ctx->context, CL_MEM_WRITE_ONLY, img_size, NULL, &ret);
	if (ret != CL_SUCCESS) {
		pr_tuning("clCreateBuffer fail, ret: %d\n", ret);
		return RET_FAILURE;
	}

	opencl_ctx->lut_buffer = clCreateBuffer(opencl_ctx->context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
						LUT_SIZE * LUT_SIZE * LUT_SIZE * 3 * sizeof(unsigned short), lut3d_map, &ret);
	if (ret != CL_SUCCESS) {
		pr_tuning("clCreateBuffer fail, ret: %d\n", ret);
		return RET_FAILURE;
	}

	int lut_size = LUT_SIZE;
	int img_width = pipe_info->img_width;
	int img_height = pipe_info->img_height;
	ret = clSetKernelArg(opencl_ctx->kernel, 0, sizeof(cl_mem), &opencl_ctx->input_image);
	ret |= clSetKernelArg(opencl_ctx->kernel, 1, sizeof(cl_mem), &opencl_ctx->output_image);
	ret |= clSetKernelArg(opencl_ctx->kernel, 2, sizeof(cl_mem), &opencl_ctx->lut_buffer);
	ret |= clSetKernelArg(opencl_ctx->kernel, 3, sizeof(int), &lut_size);
	ret |= clSetKernelArg(opencl_ctx->kernel, 4, sizeof(int), &img_height);
	ret |= clSetKernelArg(opencl_ctx->kernel, 5, sizeof(int), &img_width);
	if (ret != CL_SUCCESS) {
		pr_tuning("clSetKernelArg fail, ret: %d\n", ret);
		return RET_FAILURE;
	}

	return RET_SUCCESS;
}

static void tuning_opencl_3dlut_deinit(tuning_opencl_ctx_t *opencl_ctx)
{
	clReleaseMemObject(opencl_ctx->input_image);
	clReleaseMemObject(opencl_ctx->output_image);
	clReleaseMemObject(opencl_ctx->lut_buffer);
	clReleaseKernel(opencl_ctx->kernel);
	clReleaseProgram(opencl_ctx->program);
	clReleaseCommandQueue(opencl_ctx->queue);
	clReleaseContext(opencl_ctx->context);
}

void lut3d_map_init()
{
	int r, g, b;

	for (r = 0; r < LUT_SIZE; r++) {
	for (g = 0; g < LUT_SIZE; g++) {
	for (b = 0; b < LUT_SIZE; b++) {
	if (r < LUT_SIZE / 2) {
		lut3d_map[r][g][b][0] = FLOAT288INT((LUT_KNEE * r) / ((LUT_SIZE - 1) / 2.0));
	} else {
		lut3d_map[r][g][b][0] = FLOAT288INT(((255.0 - LUT_KNEE) * (r - LUT_SIZE / 2.0)) / ((LUT_SIZE - 1) / 2.0) + LUT_KNEE);
	}
	if (g < LUT_SIZE / 2) {
		lut3d_map[r][g][b][1] = FLOAT288INT((LUT_KNEE * g) / ((LUT_SIZE - 1) / 2.0));
	} else {
		lut3d_map[r][g][b][1] = FLOAT288INT(((255.0 - LUT_KNEE) * (g - LUT_SIZE / 2.0)) / ((LUT_SIZE - 1) / 2.0) + LUT_KNEE);
	}
	if (b < LUT_SIZE / 2) {
		lut3d_map[r][g][b][2] = FLOAT288INT((LUT_KNEE * b) / ((LUT_SIZE - 1) / 2.0));
	} else {
		lut3d_map[r][g][b][2] = FLOAT288INT(((255.0 - LUT_KNEE) * (b - LUT_SIZE / 2.0)) / ((LUT_SIZE - 1) / 2.0) + LUT_KNEE);
	}
	// lut3d_map[r][g][b][0] = FLOAT288INT((255.0 * r) / ((LUT_SIZE - 1)));
	// lut3d_map[r][g][b][1] = FLOAT288INT((255.0 * g) / ((LUT_SIZE - 1)));
	// lut3d_map[r][g][b][2] = FLOAT288INT((255.0 * b) / ((LUT_SIZE - 1)));
	}
	}
	}
}

static int32_t create_camera_node(pipe_contex_t *pipe_contex)
{
	camera_config_t *camera_config = NULL;
	vp_sensor_config_t *sensor_config = NULL;

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

	FUNC_EQ(hbn_camera_create(camera_config, &pipe_contex->cam_fd), 0, return RET_FAILURE);

	return RET_SUCCESS;
}

static int32_t create_vin_node(pipe_contex_t *pipe_contex, uint32_t pipelinemode)
{
	vp_sensor_config_t *sensor_config = NULL;
	vin_node_attr_t *vin_node_attr = NULL;
	vin_ichn_attr_t *vin_ichn_attr = NULL;
	vin_ochn_attr_t *vin_ochn_attr = NULL;
	vin_ochn_attr_t *vin_pdaf_ochn_attr = NULL;
	hbn_vnode_handle_t *vin_node_handle = NULL;
	hbn_buf_alloc_attr_t alloc_pdaf_attr = {0};
	vin_attr_ex_t vin_attr_ex;
	uint32_t hw_id = 0;
	uint32_t ichn_id = 0;
	uint32_t ochn_id = 0;
	uint64_t vin_attr_ex_mask = 0;

	sensor_config = pipe_contex->sensor_config;
	vin_node_attr = sensor_config->vin_node_attr;
	vin_ichn_attr = sensor_config->vin_ichn_attr;
	vin_ochn_attr = sensor_config->vin_ochn_attr;
	vin_pdaf_ochn_attr = sensor_config->vin_pdaf_ochn_attr;
	hw_id = vin_node_attr->cim_attr.mipi_rx;
	vin_node_handle = &pipe_contex->vin_node_handle;
	link_port = vin_node_attr->cim_attr.vc_index;

	switch (pipelinemode) {
	case Online:
		vin_node_attr->cim_attr.cim_isp_flyby = 1;
		vin_ochn_attr->ddr_en = 0;
		break;
	case MCM:
		vin_node_attr->cim_attr.cim_isp_flyby = 1;
		vin_ochn_attr->ddr_en = 1;
		break;
	case Offline:
		vin_node_attr->cim_attr.cim_isp_flyby = 0;
		vin_ochn_attr->ddr_en = 1;
		break;
	default:
		break;
	}

	if(pipe_contex->csi_config.mclk_is_not_configed){
		//设备树中没有配置mclk：使用外部晶振
		pr_tuning("csi%d ignore mclk ex attr, because not config mclk.\n",
			pipe_contex->csi_config.index);
	}else{
		vin_attr_ex.vin_attr_ex_mask = sensor_config->vin_attr_ex->vin_attr_ex_mask;
		vin_attr_ex.mclk_ex_attr.mclk_freq = sensor_config->vin_attr_ex->mclk_ex_attr.mclk_freq;
		vin_attr_ex_mask = vin_attr_ex.vin_attr_ex_mask;
	}

	FUNC_EQ(hbn_vnode_open(HB_VIN, hw_id, AUTO_ALLOC_ID, vin_node_handle), 0, return RET_FAILURE);
	FUNC_EQ(hbn_vnode_set_attr(*vin_node_handle, vin_node_attr), 0, return RET_FAILURE);
	FUNC_EQ(hbn_vnode_set_ichn_attr(*vin_node_handle, ichn_id, vin_ichn_attr), 0, return RET_FAILURE);
	FUNC_EQ(hbn_vnode_set_ochn_attr(*vin_node_handle, ochn_id, vin_ochn_attr), 0, return RET_FAILURE);
	if (vin_pdaf_ochn_attr && vin_pdaf_ochn_attr->pdaf_en == 1) {
		pdaf_en = vin_pdaf_ochn_attr->pdaf_en;
		FUNC_EQ(hbn_vnode_set_ochn_attr(*vin_node_handle, VIN_PDAF, vin_pdaf_ochn_attr), 0, return RET_FAILURE);
	}

	if (vin_attr_ex_mask) {
		for (uint8_t i = 0; i < VIN_ATTR_EX_INVALID; i ++) {
			if ((vin_attr_ex_mask & (1 << i)) == 0)
				continue;

			vin_attr_ex.ex_attr_type = i;
			/*we need to set hbn_vnode_set_attr_ex in a loop*/
			FUNC_EQ(hbn_vnode_set_attr_ex(*vin_node_handle, &vin_attr_ex), 0, return RET_FAILURE);
		}
	}

	if(pipelinemode == Offline || pipelinemode == MCM){
		hbn_buf_alloc_attr_t alloc_attr = {0};
		alloc_attr.buffers_num = 4;
		alloc_attr.is_contig = 1;
		alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN
				| HB_MEM_USAGE_CPU_WRITE_OFTEN
				| HB_MEM_USAGE_CACHED
				| HB_MEM_USAGE_HW_CIM
				| HB_MEM_USAGE_GRAPHIC_CONTIGUOUS_BUF;
		FUNC_EQ(hbn_vnode_set_ochn_buf_attr(*vin_node_handle, ochn_id, &alloc_attr), 0, return RET_FAILURE);
	}

	if (vin_pdaf_ochn_attr && vin_pdaf_ochn_attr->pdaf_en == 1) {
		memset(&alloc_pdaf_attr, 0, sizeof(hbn_buf_alloc_attr_t));
		alloc_pdaf_attr.buffers_num = 6;
		alloc_pdaf_attr.is_contig   = 1;
		alloc_pdaf_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN | HB_MEM_USAGE_CPU_WRITE_OFTEN | HB_MEM_USAGE_CACHED;
		FUNC_EQ(hbn_vnode_set_ochn_buf_attr(*vin_node_handle, VIN_PDAF, &alloc_pdaf_attr), 0, return RET_FAILURE);
	}
	return RET_SUCCESS;
}

static int32_t create_isp_node(pipe_contex_t *pipe_contex, uint32_t pipelinemode) {
	vp_sensor_config_t *sensor_config = NULL;
	isp_attr_t      *isp_attr = NULL;
	isp_ichn_attr_t *isp_ichn_attr = NULL;
	isp_ochn_attr_t *isp_ochn_attr = NULL;
	hbn_vnode_handle_t *isp_node_handle = NULL;
	hbn_buf_alloc_attr_t alloc_attr = {0};
	uint32_t ichn_id = 0;
	uint32_t ochn_id = 0;

	sensor_config = pipe_contex->sensor_config;
	isp_attr = sensor_config->isp_attr;
	isp_ichn_attr = sensor_config->isp_ichn_attr;
	isp_ochn_attr = sensor_config->isp_ochn_attr;
	isp_node_handle = &pipe_contex->isp_node_handle;

	switch (pipelinemode) {
	case Online:
		isp_attr->input_mode = 0;
		break;
	case MCM:
		isp_attr->input_mode = 1;
		break;
	case Offline:
		isp_attr->input_mode = 2;
		break;
	default:
		break;
	}

	FUNC_EQ(hbn_vnode_open(HB_ISP, 0, AUTO_ALLOC_ID, isp_node_handle), 0, return RET_FAILURE);
	FUNC_EQ(hbn_vnode_set_attr(*isp_node_handle, isp_attr), 0, return RET_FAILURE);
	FUNC_EQ(hbn_vnode_set_ochn_attr(*isp_node_handle, ochn_id, isp_ochn_attr), 0, return RET_FAILURE);
	FUNC_EQ(hbn_vnode_set_ichn_attr(*isp_node_handle, ichn_id, isp_ichn_attr), 0, return RET_FAILURE);
	if (isp_attr->af_mode) {
		FUNC_EQ(hbn_vnode_set_ichn_attr(*isp_node_handle, ISP_PDAF_DATA, isp_ichn_attr), 0, return RET_FAILURE);
	}

	alloc_attr.buffers_num = 6;
	alloc_attr.is_contig = 1;
	alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN
			| HB_MEM_USAGE_CPU_WRITE_OFTEN
			| HB_MEM_USAGE_CACHED;
	FUNC_EQ(hbn_vnode_set_ochn_buf_attr(*isp_node_handle, ochn_id, &alloc_attr), 0, return RET_FAILURE);

	return RET_SUCCESS;
}

static int32_t create_vse_node(pipe_contex_t *pipe_contex, uint32_t pipelinemode)
{
	int ret = 0;
	hbn_vnode_handle_t *vse_node_handle = &pipe_contex->vse_node_handle;
	isp_ichn_attr_t isp_ichn_attr = {0};
	vse_attr_t vse_attr = {0};
	vse_ichn_attr_t vse_ichn_attr = {0};
	vse_ochn_attr_t vse_ochn_attr[VSE_CHANNELS_USED + 1] = {0};
	uint32_t ichn_id = 0;
	uint32_t hw_id = 0;
	uint32_t input_width = 0;
	uint32_t input_height = 0;
	uint32_t ratio = 0;
	hbn_buf_alloc_attr_t alloc_attr = {0};

	// 获取 ISP 输入属性来确定 VSE 输入尺寸
	ret = hbn_vnode_get_ichn_attr(pipe_contex->isp_node_handle, ichn_id, &isp_ichn_attr);
	ERR_CON_EQ(ret, 0);
	input_width = isp_ichn_attr.width;
	input_height = isp_ichn_attr.height;

	// 配置 VSE 输入通道属性
	vse_ichn_attr.width = input_width;
	vse_ichn_attr.height = input_height;
	vse_ichn_attr.fmt = FRM_FMT_NV12;
	vse_ichn_attr.bit_width = 8;

	// 配置 VSE 输出通道属性
	vse_ochn_attr[VSE_CHANNELS_USED].chn_en = CAM_TRUE;
	vse_ochn_attr[VSE_CHANNELS_USED].roi.x = 0;
	vse_ochn_attr[VSE_CHANNELS_USED].roi.y = 0;
	vse_ochn_attr[VSE_CHANNELS_USED].roi.w = input_width;
	vse_ochn_attr[VSE_CHANNELS_USED].roi.h = input_height;
	vse_ochn_attr[VSE_CHANNELS_USED].fmt = FRM_FMT_NV12;
	vse_ochn_attr[VSE_CHANNELS_USED].bit_width = 8;
	// 全部设置到宽为 640 的像素，保证流畅，但是要注意，每个通道的功能和限制不同，如果修改 VSE_CHANNELS_USED 的数值，可能导致功能异常，需要参考 VSE 文档，了解每个通道的功能再进行修改。
	if (enable_hdmi) {
		int32_t hdmi_output_width = 0;
		int32_t hdmi_output_height = 0;

		if (vp_display_check_hdmi_is_connected() < 0) {
			printf("\n\nFailed: output form is hdmi, but not found hdmi connector.\n\n");
			printf("Please run insmode_driver.sh to load HDMI related drivers before using --hdmi.\n");
			printf("（使用 HDMI 功能前，请先执行 insmode_driver.sh 加载显示相关驱动。）\n\n");
			return RET_FAILURE;
		}

		ret = tuning_pick_hdmi_resolution((int32_t)input_width, (int32_t)input_height,
			&hdmi_output_width, &hdmi_output_height);
		if (ret < 0) {
			printf("hdmi not found appropriate resolution\n");
			goto vse_default_target;
		}

		g_hdmi_width = (uint32_t)hdmi_output_width;
		g_hdmi_height = (uint32_t)hdmi_output_height;
		global_ctx->vse_ratio = 1;
		vse_ochn_attr[VSE_CHANNELS_USED].target_w = (uint32_t)hdmi_output_width;
		vse_ochn_attr[VSE_CHANNELS_USED].target_h = (uint32_t)hdmi_output_height;
	} else {
vse_default_target:
		ratio = input_width / VSE_WIDTH_TARGET;
		global_ctx->vse_ratio = ratio;
		vse_ochn_attr[VSE_CHANNELS_USED].target_w = VSE_WIDTH_TARGET;
		vse_ochn_attr[VSE_CHANNELS_USED].target_h = input_height / ratio;
	}


	// 创建 VSE 节点
	FUNC_EQ(hbn_vnode_open(HB_VSE, hw_id, AUTO_ALLOC_ID, vse_node_handle), 0, return RET_FAILURE);
	FUNC_EQ(hbn_vnode_set_attr(*vse_node_handle, &vse_attr), 0, return RET_FAILURE);
	FUNC_EQ(hbn_vnode_set_ichn_attr(*vse_node_handle, ichn_id, &vse_ichn_attr), 0, return RET_FAILURE);
	// 设置缓冲区属性
	alloc_attr.buffers_num = 3;
	alloc_attr.is_contig = 1;
	alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN | HB_MEM_USAGE_CPU_WRITE_OFTEN | HB_MEM_USAGE_CACHED;

	printf("hbn_vnode_set_ochn_attr: %d, %dx%d\n", VSE_CHANNELS_USED, vse_ochn_attr[VSE_CHANNELS_USED].target_w, vse_ochn_attr[VSE_CHANNELS_USED].target_h);
	FUNC_EQ(hbn_vnode_set_ochn_attr(*vse_node_handle, VSE_CHANNELS_USED, &vse_ochn_attr[VSE_CHANNELS_USED]), 0, return RET_FAILURE);
	FUNC_EQ(hbn_vnode_set_ochn_buf_attr(*vse_node_handle, VSE_CHANNELS_USED, &alloc_attr), 0, return RET_FAILURE);


    return RET_SUCCESS;
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
	printf("deserial_config: addr=0x%02x, name=%s, des_handle=%ld\n",
		deserial_config->addr, deserial_config->name, *des_handle);

	return ret;
}

static int32_t multi_pipe_create(tuning_context_t *ctx, uint32_t pipelinemode)
{
	int32_t i, ret = 0;
	pipe_contex_t *pipe_contex;

	for (i = 0; i < ctx->sensor_count; i++) {
		pipe_contex = &ctx->pipe_contex_info[i].pipe_contex;

		FUNC_EQ(create_camera_node(pipe_contex), 0, return RET_FAILURE);
		FUNC_EQ(create_vin_node(pipe_contex, pipelinemode), 0, return RET_FAILURE);
		FUNC_EQ(create_isp_node(pipe_contex, pipelinemode), 0, return RET_FAILURE);
		if(enable_vse)
			FUNC_EQ(create_vse_node(pipe_contex, pipelinemode), 0, return RET_FAILURE);
		FUNC_EQ(hbn_vflow_create(&pipe_contex->vflow_fd), 0, return RET_FAILURE);
		FUNC_EQ(hbn_vflow_add_vnode(pipe_contex->vflow_fd, pipe_contex->vin_node_handle), 0, return RET_FAILURE);
		FUNC_EQ(hbn_vflow_add_vnode(pipe_contex->vflow_fd, pipe_contex->isp_node_handle), 0, return RET_FAILURE);
		if(enable_vse)
			FUNC_EQ(hbn_vflow_add_vnode(pipe_contex->vflow_fd, pipe_contex->vse_node_handle), 0, return RET_FAILURE);

		if (!BIT_ENABLE(ctx->work_mode, FEEDBACK_MASK)) {
			switch (pipelinemode) {
			case Online:
			case MCM:
				ret = hbn_vflow_bind_vnode(pipe_contex->vflow_fd,
					pipe_contex->vin_node_handle, 1,
					pipe_contex->isp_node_handle, 0);
					if (pdaf_en) {
						ret = hbn_vflow_bind_vnode(pipe_contex->vflow_fd,
								pipe_contex->vin_node_handle,
								VIN_PDAF,
								pipe_contex->isp_node_handle,
								ISP_PDAF_DATA);
						ERR_CON_EQ(ret, 0);
					}
					if(enable_vse)
					{
						ret = hbn_vflow_bind_vnode(pipe_contex->vflow_fd,
								pipe_contex->isp_node_handle,
								0,
								pipe_contex->vse_node_handle,
								0);
					}
				break;
			case Offline:
				ret = hbn_vflow_bind_vnode(pipe_contex->vflow_fd,
					pipe_contex->vin_node_handle, 0,
					pipe_contex->isp_node_handle, 0);
					if (pdaf_en) {
						ret = hbn_vflow_bind_vnode(pipe_contex->vflow_fd,
								pipe_contex->vin_node_handle,
								VIN_PDAF,
								pipe_contex->isp_node_handle,
								ISP_PDAF_DATA);
						ERR_CON_EQ(ret, 0);
					}
					if(enable_vse)
					{
						ret = hbn_vflow_bind_vnode(pipe_contex->vflow_fd,
								pipe_contex->isp_node_handle,
								0,
								pipe_contex->vse_node_handle,
								0);
					}
				break;
			default:
				break;
			}
			if (ret < 0) {
				printf("sensor-%d hbn_vflow_bind_vnode fail\n", i);
				return ret;
			}
		}

		/* === 4. 根据 sensor 类型执行 attach === */
		if (pipe_contex->sensor_config &&
			pipe_contex->sensor_config->sensor_type != SENSOR_TYPE_NORMAL) {
			FUNC_EQ(create_deserial_node(pipe_contex), 0, return RET_FAILURE);
			ret = hbn_camera_attach_to_deserial(pipe_contex->cam_fd,
												pipe_contex->des_fd,
												link_port);
			if (ret < 0) {
				printf("sensor-%d hbn_camera_attach_to_deserial fail\n", i);
				return RET_FAILURE;
			}

			ret = hbn_deserial_attach_to_vin(pipe_contex->des_fd,
											link_port,
											pipe_contex->vin_node_handle);
			if (ret < 0) {
				printf("sensor-%d hbn_deserial_attach_to_vin fail\n", i);
				return RET_FAILURE;
			}
		} else {
			/* 普通 sensor：Camera 直接 attach 到 vin */
			ret = hbn_camera_attach_to_vin(pipe_contex->cam_fd,
										pipe_contex->vin_node_handle);
			if (ret < 0) {
				printf("sensor-%d hbn_camera_attach_to_vin fail\n", i);
				return RET_FAILURE;
			}
		}
	}
	return ret;
}

int32_t tuning_common_ctrl_cb(cmd_header_new_t *message, void *ptr, uint32_t size, void *arg)
{
	int32_t ret;
	hbn_isp_afmv1_attr_t afmv1_attr = {0};

	switch (message->format) {
	case FV_CURVE_RUN:
		global_ctx->min_focal = message->metadata.calib_rw.id;		// min_focal
		global_ctx->max_focal = message->metadata.calib_rw.type;	// max_focal
		global_ctx->step = message->metadata.calib_rw.size;		// step
		global_ctx->afm_version = message->metadata.calib_rw.width;	// afm_version
		global_ctx->block_select = message->metadata.calib_rw.rows;	// block
		global_ctx->feedback_fv = message->metadata.calib_rw.cols;	// feedback_enbale
		global_ctx->run_fv = FV_DELAY_FRAME;
		if (global_ctx->feedback_fv) {
			global_ctx->run_feedback_fv = 1;
			global_ctx->run_fv = 0;
		}
		global_ctx->cur_focal = global_ctx->min_focal;
		break;
	case FV_AFM_WIN:
		ret = hbn_isp_get_afmv1_attr(global_ctx->pipe_contex_info[0].pipe_contex.isp_node_handle, &afmv1_attr);
		if (ret) {
			pr_tuning("Get AFMV1 attr fail\n");
			return 0;
		}
		afmv1_attr.threshold = message->metadata.calib_rw.id;	// fv_afmwin_on
		if (enable_vse) {
			for (int32_t i = 0; i < HBN_ISP_AFMV1_WINDOW_NUM; i++) {
				afmv1_attr.afm_windows[i].h_offset = afmv1_attr.afm_windows[i].h_offset / global_ctx->vse_ratio;
				afmv1_attr.afm_windows[i].v_offset = afmv1_attr.afm_windows[i].v_offset / global_ctx->vse_ratio;
				afmv1_attr.afm_windows[i].height = afmv1_attr.afm_windows[i].height / global_ctx->vse_ratio;
				afmv1_attr.afm_windows[i].width = afmv1_attr.afm_windows[i].width / global_ctx->vse_ratio;
			}
		}
		cmd_header_new_t head = {0};

		head.len = sizeof(hbn_isp_afmv1_attr_t);
		head.type = STATS_AF_DATA;
		head.format = FV_AFM_WIN;
		hb_tool_used_define_pic(global_ctx->hbplayer_event, &head, &afmv1_attr, sizeof(hbn_isp_afmv1_attr_t));
		break;
	default:
		break;
	}

	return 0;
}

static int32_t tuning_case_run(tuning_context_t *ctx)
{
	int32_t i, ret = 0;
	isp_ichn_attr_t ichn_attr = {0};
	vin_ochn_attr_t ochn_attr = {0};
	pipe_contex_t *pipe_contex;

	for (i = 0; i < ctx->sensor_count; i++) {
		pipe_contex = &ctx->pipe_contex_info[i].pipe_contex;
		FUNC_EQ(hbn_vnode_get_ichn_attr(pipe_contex->isp_node_handle, 0, &ichn_attr), 0, return RET_FAILURE);
		FUNC_EQ(hbn_vnode_get_ochn_attr(pipe_contex->vin_node_handle, 0, &ochn_attr), 0, return RET_FAILURE);

		ctx->pipe_contex_info[i].vin_format = ochn_attr.vin_basic_attr.format;
		ctx->pipe_contex_info[i].is_offline = ochn_attr.ddr_en ? 1 : 0;
		ctx->pipe_contex_info[i].img_width = ichn_attr.width;
		ctx->pipe_contex_info[i].img_height = ichn_attr.height;

		if (BIT_ENABLE(ctx->work_mode, START_DUMP_MASK))
			ctx->pipe_contex_info[i].yuv_dump_cnt = 20;
	}

	if (BIT_ENABLE(ctx->work_mode, FEEDBACK_MASK)) {
		pr_tuning("tuning_tool run with feedback\n");
		ctx->feedback_times = ctx->feedback_times == 0 ? 0xFFFF : ctx->feedback_times;
		FUNC_EQ(tuning_feeback_init(ctx), 0, goto destroy_cam);
	}

	if (HBPLAYER_EN) {
		ctx->hbplayer_event = hb_tool_start_transfer(0);
		hb_tool_event_setcb(ctx->hbplayer_event, NULL, NULL, NULL, tuning_common_ctrl_cb, NULL);
		uint32_t event = EV_COMMON_FLAG;
		hb_tool_event_enable(ctx->hbplayer_event, event);
	}

	if (BIT_ENABLE(ctx->work_mode, LUT3D_MASK)) {
		pr_tuning("tuning_tool can support 3dlut now\n");
		lut3d_map_init();
		for (i = 0; i < ctx->sensor_count; i++) {
			tuning_opencl_3dlut_init(&ctx->pipe_contex_info[i]);
		}
	}

	for (i = 0; i < ctx->sensor_count; i++) {
		pipe_contex = &ctx->pipe_contex_info[i].pipe_contex;
		hbn_vflow_start(pipe_contex->vflow_fd);
	}

	// todo: create multi pthread
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

destroy:
	if (BIT_ENABLE(ctx->work_mode, LUT3D_MASK)) {
		for (i = 0; i < ctx->sensor_count; i++) {
			tuning_opencl_3dlut_deinit(&ctx->pipe_contex_info[i].opencl_ctx);
		}
	}

	if (HBPLAYER_EN)
		hb_tool_stop_transfer(ctx->hbplayer_event);
#ifdef TUNING_DEBUG
	pr_tuning("stop transfer done\n");
#endif

	if (BIT_ENABLE(ctx->work_mode, FEEDBACK_MASK))
		tuning_free_feedback_buffer(&ctx->src_img.buffer);

destroy_cam:
	for (i = 0; i < ctx->sensor_count; i++) {
		pipe_contex = &ctx->pipe_contex_info[i].pipe_contex;
		hbn_vflow_stop(pipe_contex->vflow_fd);
		hbn_camera_destroy(pipe_contex->cam_fd);
		hbn_vflow_destroy(pipe_contex->vflow_fd);
	}

	pr_tuning("camsys destory done\n");
	return ret;
}

int32_t main(int argc, char *argv[])
{
	int32_t ret = 0;
	tuning_context_t ctx = {0};
	global_ctx = &ctx;

	ret = parse_opts(argc, argv, &ctx);
	if (ret != 1) {
		return ret;
	}

	if (access(DEF_DUMP_PATH, 0)) {
		ret = mkdir(DEF_DUMP_PATH, 0777);
		if (ret < 0) {
			pr_tuning("mkdir %s for dump failed !\n", DEF_DUMP_PATH);
			return RET_FAILURE;
		}
	}

	FUNC_EQ(multi_pipe_create(&ctx, pipelinemode), 0, return RET_FAILURE);
	FUNC_EQ(tuning_case_run(&ctx), 0, return RET_FAILURE);

	return ret;
}
