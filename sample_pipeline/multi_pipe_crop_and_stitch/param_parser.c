#include "param_parser.h"



static int is_number(const char *str) {
	while (*str) {
		if (!isdigit(*str)) return 0;
		str++;
	}
	return 1;
}
#if 0
int camera_config_is_same(multi_pipe_stitch_info_t *multi_pipe_stitch_info){

	int camera_width = 0;
	int camera_height = 0;
	int camera_fps = 0;
	char *camera_name = "unknown";
	pipeline_info_t *pipeline_info = multi_pipe_stitch_info->pipeline_info;
	for(int i = 0; i < multi_pipe_stitch_info->camera_count; i++){
		int width_tmp = pipeline_info[i].pipe_contexts.sensor_config->camera_config->width;
		int height_tmp = pipeline_info[i].pipe_contexts.sensor_config->camera_config->height;
		int fps_tmp = pipeline_info[i].pipe_contexts.sensor_config->camera_config->fps;
		char *camera_name_tmp = pipeline_info[i].pipe_contexts.sensor_config->sensor_name;

		if(i == 0){
			camera_width = width_tmp;
			camera_height = height_tmp;
			camera_fps = fps_tmp;
			camera_name = pipeline_info[i].pipe_contexts.sensor_config->sensor_name;
		}else{
			if((camera_width != width_tmp) || (camera_height != height_tmp)){
				printf("camera %s width %d height %d fps %dis different width camera %s width %d height %d fps %d.\n",
					camera_name, camera_width, camera_height, camera_fps,
					camera_name_tmp, width_tmp, height_tmp, fps_tmp);
				return -1;
			}
		}
	}
	return 0;
}
#endif

static void print_help(void) {
	printf("Usage: %s [Options]\n", get_program_name());
	printf("Options:\n");
#if 0
	printf("-c, --config=\"sensor=id channel=vse_chn\"\n");
	printf("\t\tConfigure parameters for each video pipeline, can be repeated up to %d times.\n", MAX_PIPE_NUM);
	printf("\t\tsensor   --  Sensor index,can have multiple parameters, reference sensor list.\n");
	printf("\t\tchannel  --  Vse channel index bind to encode, default 0, can be set to [0-5].\n");
#else
	printf("-c, --config=\"sensor=id \"\n");
	printf("\t\tConfigure parameters for each video pipeline, can be repeated up to %d times.\n", MAX_PIPE_NUM);
	printf("\t\tsensor   --  Sensor index,can have multiple parameters, reference sensor list.\n");
#endif
	printf("-v, --verbose\tEnable verbose mode\n");
	printf("-h, --help\tShow help message\n");

	printf("\n\n");

	printf("Support sensor list:\n");
	vp_show_sensors_list();

	printf("\n\nExample:(only support 2 cameras and 4 cameras)\n");
	printf("2 cameras:  ./multi_pipe_crop_and_stitch -c \"sensor=7\" -c \"sensor=3\"\n");
	printf("4 cameras:  ./multi_pipe_crop_and_stitch -c \"sensor=7\" -c \"sensor=3\" -c \"sensor=3\" -c \"sensor=3\"\n");
	printf("\n\n");
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

void parse_config(sensor_param_config_t *sensor_param_config, const char *config) {
	int ret = 0;
	char *parts[4];
	int sensor_idx = -1;
	int count = split_string(config, " ", parts, 4);

	static int32_t used_mipi_host = 0; //must is static
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
				sensor_param_config->sensor_config = vp_sensor_config_list[sensor_idx];
				printf("\n\nUsing index:%d  sensor_name:%s  config_file:%s\n",
						sensor_idx,
						vp_sensor_config_list[sensor_idx]->sensor_name,
						vp_sensor_config_list[sensor_idx]->config_file);
			} else {
				printf("Unsupport sensor index:%d\n", sensor_idx);
				print_help();
				exit(0);
			}
			ret = vp_sensor_multi_fixed_mipi_host(sensor_param_config->sensor_config, used_mipi_host,
				&sensor_param_config->csi_config);
			if (ret < 0) {
				printf("vp sensor fixed mipi host fail, sensor id %d."
					"Maybe No Camera Sensor found. Please check if the specified "
					"sensor is connected to the Camera interface.\n\n", sensor_idx);
				exit(0);
			}
			sensor_param_config->select_sensor_id = sensor_idx;
			// active_mipi_host 的配置在create_vin_node函数中需要再配置一下
			sensor_param_config->active_mipi_host = sensor_param_config->sensor_config->vin_node_attr->cim_attr.mipi_rx;
			used_mipi_host |= (1 << sensor_param_config->sensor_config->vin_node_attr->cim_attr.mipi_rx);

			printf("mipi host %d\n", sensor_param_config->active_mipi_host);

		} else if (strcmp(key_value[0], "channel") == 0) {
			if (!is_number(key_value[1])) {
				fprintf(stderr, "Invalid channel ID: %s\n", key_value[1]);
				continue;
			}
			sensor_param_config->vse_bind_n2d_chn = atoi(key_value[1]);
		} else if (strcmp(key_value[0], "mode") == 0) {
			if (!is_number(key_value[1])) {
				fprintf(stderr, "Invalid sensor mode number: %s\n", key_value[1]);
				continue;
			}
			sensor_param_config->sensor_mode = atoi(key_value[1]);
		} else {
			fprintf(stderr, "Unknown key: %s\n", key_value[0]);
		}

		for (int j = 0; j < kv_count; j++) {
			free(key_value[j]);
		}
	}

	for (int i = 0; i < count; i++) {
		free(parts[i]);
	}

	printf("MIPI host: 0x%x\n", used_mipi_host);
	for (int i = 0; i < MAX_PIPE_NUM; i++) {
		if (used_mipi_host & (1 << i)) {
			printf("  Host %d: Used\n", i);
		}
	}
}

int param_process(int argc, char** argv, param_config_t* param_config){

	static struct option const long_options[] = {
		{"config", required_argument, NULL, 'c'},
		{"verbose", no_argument, NULL, 'v'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0}
	};

	int c = 0;
	int32_t total_pipeline_num = 0;
	while ((c = getopt_long(argc, argv, "c:m:vh", long_options, NULL)) != -1) {
		switch (c) {
		case 'c':
			if (total_pipeline_num >= MAX_PIPE_NUM) {
				fprintf(stderr, "Too many configurations. Maximum allowed is %d.\n", MAX_PIPE_NUM);
				return -1;
			}

			parse_config(&param_config->sensor_param_config[total_pipeline_num], optarg);
			total_pipeline_num++;
			break;

		case 'v':
			param_config->verbose_flag = 1;
			break;
		case 'h':
		default:
			print_help();
			return -1;
		}
	}

	if((total_pipeline_num != 2) && (total_pipeline_num != 4)){
		printf("\n[%s] only support 2 or 4 cameras as input, current input %d cameras.\n\n",
			argv[0], total_pipeline_num);

		print_help();
		return -1;
	}

	param_config->sensor_config_count = total_pipeline_num;
	return 0;
}