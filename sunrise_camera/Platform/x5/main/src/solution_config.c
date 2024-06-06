#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/time.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>

#include "utils/common_utils.h"
#include "utils/cJSON.h"
#include "utils/cJSON_Direct.h"
#include "utils/utils_log.h"

#include "solution_config.h"
#include "vp_wrap.h"
#include "bpu_wrap.h"

#define SOLUTION_CONFIG_PATH "../test_data/"
#define SOLUTION_CONFIG_FILE SOLUTION_CONFIG_PATH "solution_config.json"

int32_t g_solution_cfg_is_load = 0;
solution_cfg_t g_solution_config;

key_info_t hard_capability_key[] = {
	MAKE_KEY_INFO(solution_hard_capability_t, KEY_TYPE_STRING, chip_type, NULL),
	MAKE_KEY_INFO(solution_hard_capability_t, KEY_TYPE_STRING, sensor_list, NULL),
	MAKE_KEY_INFO(solution_hard_capability_t, KEY_TYPE_STRING, model_list, NULL),
	MAKE_KEY_INFO(solution_hard_capability_t, KEY_TYPE_STRING, codec_type_list, NULL),
	MAKE_ARRAY_INFO(solution_hard_capability_t, KEY_TYPE_ARRAY, encode_bit_rate_list, NULL, 16, KEY_TYPE_S32),
	MAKE_KEY_INFO(solution_hard_capability_t, KEY_TYPE_STRING, display_dev_list, NULL),
	MAKE_END_INFO()};

static key_info_t cfg_cam_vpp_key[] = {
	MAKE_KEY_INFO(solution_cfg_cam_vpp_t, KEY_TYPE_STRING, sensor, NULL),
	MAKE_KEY_INFO(solution_cfg_cam_vpp_t, KEY_TYPE_S32, encode_type, NULL),
	MAKE_KEY_INFO(solution_cfg_cam_vpp_t, KEY_TYPE_S32, encode_bitrate, NULL),
	MAKE_KEY_INFO(solution_cfg_cam_vpp_t, KEY_TYPE_STRING, model, NULL),
	MAKE_END_INFO()};

static key_info_t cfg_cam_key[] = {
	MAKE_KEY_INFO(solution_cfg_cam_t, KEY_TYPE_S32, pipeline_count, NULL),
	MAKE_KEY_INFO(solution_cfg_cam_t, KEY_TYPE_S32, max_pipeline_count, NULL),
	MAKE_ARRAY_INFO(solution_cfg_cam_t, KEY_TYPE_ARRAY, cam_vpp, cfg_cam_vpp_key, STL_MAX_VPP_CAM_NUM, KEY_TYPE_OBJECT),
	MAKE_END_INFO()};

static key_info_t cfg_box_vpp_key[] = {
	MAKE_KEY_INFO(solution_cfg_box_vpp_t, KEY_TYPE_STRING, stream, NULL),
	MAKE_KEY_INFO(solution_cfg_box_vpp_t, KEY_TYPE_S32, decode_type, NULL),
	MAKE_KEY_INFO(solution_cfg_box_vpp_t, KEY_TYPE_S32, decode_width, NULL),
	MAKE_KEY_INFO(solution_cfg_box_vpp_t, KEY_TYPE_S32, decode_height, NULL),
	MAKE_KEY_INFO(solution_cfg_box_vpp_t, KEY_TYPE_S32, decode_frame_rate, NULL),
	MAKE_KEY_INFO(solution_cfg_box_vpp_t, KEY_TYPE_S32, encode_type, NULL),
	MAKE_KEY_INFO(solution_cfg_box_vpp_t, KEY_TYPE_S32, encode_width, NULL),
	MAKE_KEY_INFO(solution_cfg_box_vpp_t, KEY_TYPE_S32, encode_height, NULL),
	MAKE_KEY_INFO(solution_cfg_box_vpp_t, KEY_TYPE_S32, encode_frame_rate, NULL),
	MAKE_KEY_INFO(solution_cfg_box_vpp_t, KEY_TYPE_S32, encode_bitrate, NULL),
	MAKE_KEY_INFO(solution_cfg_box_vpp_t, KEY_TYPE_STRING, model, NULL),
	MAKE_END_INFO()};

static key_info_t cfg_box_key[] = {
	MAKE_KEY_INFO(solution_cfg_box_t, KEY_TYPE_S32, pipeline_count, NULL),
	MAKE_KEY_INFO(solution_cfg_cam_t, KEY_TYPE_S32, max_pipeline_count, NULL),
	MAKE_ARRAY_INFO(solution_cfg_box_t, KEY_TYPE_ARRAY, box_vpp, cfg_box_vpp_key, STL_MAX_VPP_BOX_NUM, KEY_TYPE_OBJECT),
	MAKE_END_INFO()};

static key_info_t solution_cfg_key[] = {
	MAKE_KEY_INFO(solution_cfg_t, KEY_TYPE_STRING, version, NULL),
	MAKE_KEY_INFO(solution_cfg_t, KEY_TYPE_OBJECT, hardware_capability, hard_capability_key),
	MAKE_KEY_INFO(solution_cfg_t, KEY_TYPE_STRING, solution_name, NULL),
	MAKE_KEY_INFO(solution_cfg_t, KEY_TYPE_OBJECT, cam_solution, cfg_cam_key),
	MAKE_KEY_INFO(solution_cfg_t, KEY_TYPE_OBJECT, box_solution, cfg_box_key),
	MAKE_KEY_INFO(solution_cfg_t, KEY_TYPE_STRING, display_dev, NULL),
	MAKE_END_INFO()};

// 打印 solution_cfg_t 结构体的所有值
void print_solution_cfg(const solution_cfg_t *config)
{
	printf("Version: %s\n", config->version);
	printf("Hardware Capability:\n");
	printf("  Chip Type: %s\n", config->hardware_capability.chip_type);
	printf("  Sensor List: %s\n", config->hardware_capability.sensor_list);
	printf("  Model List: %s\n", config->hardware_capability.model_list);
	printf("  Codec Type List: %s\n", config->hardware_capability.codec_type_list);
	printf("  Encode Bit Rate List: ");
	for (int i = 0; i < 16; i++)
	{
		printf("%d ", config->hardware_capability.encode_bit_rate_list[i]);
	}
	printf("\n");
	printf("  Display Device List: %s\n", config->hardware_capability.display_dev_list);
	printf("Solution Name: %s\n", config->solution_name);
	printf("Camera Solution:\n");
	printf("  Pipeline Count: %d\n", config->cam_solution.pipeline_count);
	printf("  Max Pipeline Count: %d\n", config->cam_solution.max_pipeline_count);
	for (int i = 0; i < config->cam_solution.pipeline_count; i++)
	{
		printf("  Camera VPP %d:\n", i + 1);
		printf("    Sensor: %s\n", config->cam_solution.cam_vpp[i].sensor);
		printf("    Encode Type: %d\n", config->cam_solution.cam_vpp[i].encode_type);
		printf("    Encode Bitrate: %d\n", config->cam_solution.cam_vpp[i].encode_bitrate);
		printf("    Model: %s\n", config->cam_solution.cam_vpp[i].model);
	}
	printf("Box Solution:\n");
	printf("  Pipeline Count: %d\n", config->box_solution.pipeline_count);
	printf("  Max Pipeline Count: %d\n", config->box_solution.max_pipeline_count);
	for (int i = 0; i < config->box_solution.pipeline_count; i++)
	{
		printf("  Box VPP %d:\n", i + 1);
		printf("    Stream: %s\n", config->box_solution.box_vpp[i].stream);
		printf("    Decode Type: %d\n", config->box_solution.box_vpp[i].decode_type);
		printf("    Decode Width: %d\n", config->box_solution.box_vpp[i].decode_width);
		printf("    Decode Height: %d\n", config->box_solution.box_vpp[i].decode_height);
		printf("    Decode Frame Rate: %d\n", config->box_solution.box_vpp[i].decode_frame_rate);
		printf("    Encode Type: %d\n", config->box_solution.box_vpp[i].encode_type);
		printf("    Encode Width: %d\n", config->box_solution.box_vpp[i].encode_width);
		printf("    Encode Height: %d\n", config->box_solution.box_vpp[i].encode_height);
		printf("    Encode Frame Rate: %d\n", config->box_solution.box_vpp[i].encode_frame_rate);
		printf("    Encode Bitrate: %d\n", config->box_solution.box_vpp[i].encode_bitrate);
		printf("    Model: %s\n", config->box_solution.box_vpp[i].model);
	}
	printf("Display Device: %s\n", config->display_dev);
}

static cJSON *open_json_file(char *filename)
{
	FILE *f;
	long len;
	char *data;
	cJSON *json;

	f = fopen(filename, "rb");
	if (NULL == f)
		return NULL;
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	data = (char *)malloc(len + 1);
	fread(data, 1, len, f);
	fclose(f);

	data[len] = '\0';
	json = cJSON_Parse(data);
	if (!json)
	{
		printf("Error before: [%s]\n", cJSON_GetErrorPtr());
		free(data);
		return NULL;
	}

	free(data);
	return json;
}

static int32_t write_json_file(char *filename, char *out)
{
	FILE *fp = NULL;

	fp = fopen(filename, "a+");
	if (fp == NULL)
	{
		fprintf(stderr, "open file failed\n");
		return -1;
	}
	fprintf(fp, "%s", out);

	if (fp != NULL)
		fclose(fp);

	return 0;
}
//不包含 "\0"
static int get_first_camera_name_from_camera_list(){
	int ret = -1;

	char *sensor_list_tmp = (char *)g_solution_config.hardware_capability.sensor_list;

	for(int i = 0; i< sizeof(g_solution_config.hardware_capability.sensor_list); i++){
		if(sensor_list_tmp[i] == '/'){
			if(i > 0){
				ret = i - 1;
			}
			break;
		}else if(sensor_list_tmp[i] == '\0'){
			if(i > 0){
				ret = i - 1;
			}
			break;
		}
	}
	return ret;
}

int32_t solution_cfg_load_default_config()
{
	//只清除静态的配置(运行时获取的参数比如能力列表 不清除)
	memset(&g_solution_config.solution_name, 0, sizeof(g_solution_config.solution_name));
	memset(&g_solution_config.cam_solution, 0, sizeof(g_solution_config.cam_solution));
	memset(&g_solution_config.box_solution, 0, sizeof(g_solution_config.box_solution));
	memset(&g_solution_config.display_dev, 0, sizeof(g_solution_config.display_dev));

	strcpy(g_solution_config.hardware_capability.codec_type_list, "H264/H265");
	// strcpy(g_solution_config.hardware_capability.codec_type_list, "H264/H265/Mjpeg");
	
	// 初始化编码码率列表
	// 标清视频（480p） 256, 512, 768, 1024, 1536, 2048,
	// 高清视频（720p） 512, 1024, 2048, 3072, 4096, 6144,
	// 全高清视频（1080p） 1024, 2048, 4096, 6144, 8192, 12288,
	// 2K视频 2048, 4096, 8192, 12288, 16384, 24576,
	// 4K视频  4096, 8192, 16384, 24576, 32768, 49152
	int32_t encode_bitrate_options[] = {
		256, 512, 768, 1024, 1536, 2048,
		3072, 4096, 6144, 8192, 12288,
		16384, 24576, 32768, 49152
	};
	int num_options = sizeof(encode_bitrate_options) / sizeof(encode_bitrate_options[0]);
	for (int i = 0; i < num_options && i < 16; i++)
	{
		g_solution_config.hardware_capability.encode_bit_rate_list[i] = encode_bitrate_options[i];
	}
	strcpy(g_solution_config.hardware_capability.display_dev_list, "hdmi");

	// 默认应用方案
	strcpy(g_solution_config.solution_name, "box_solution");

	// camera solution
	g_solution_config.cam_solution.pipeline_count = 1;
	g_solution_config.cam_solution.max_pipeline_count = STL_MAX_VPP_CAM_NUM;

	int first_camera_end_index = get_first_camera_name_from_camera_list();
	if(first_camera_end_index == -1){
		strcpy(g_solution_config.cam_solution.cam_vpp[0].sensor, "Null");
	}else{
		int dest_array_size = sizeof(g_solution_config.cam_solution.cam_vpp[0].sensor);

		// first_camera_end_index 不包含 '\0'
		if( dest_array_size <= (first_camera_end_index + 1)){ 
			SC_LOGW("sensor name is too short (%d < %d), so rest use NULL.");
			strcpy(g_solution_config.cam_solution.cam_vpp[0].sensor, "Null");
		}else{
			strncpy(g_solution_config.cam_solution.cam_vpp[0].sensor, 
				g_solution_config.hardware_capability.sensor_list,
				first_camera_end_index);
			g_solution_config.cam_solution.cam_vpp[0].sensor[first_camera_end_index + 1] = '\0';
		}
	}
	
	g_solution_config.cam_solution.cam_vpp[0].encode_type = 0;
	g_solution_config.cam_solution.cam_vpp[0].encode_bitrate = 8192;
	strcpy(g_solution_config.cam_solution.cam_vpp[0].model, "yolov5s");

	// video box
	g_solution_config.box_solution.pipeline_count = 1;
	g_solution_config.box_solution.max_pipeline_count = STL_MAX_VPP_BOX_NUM;
	strcpy(g_solution_config.box_solution.box_vpp[0].stream, "../test_data/1080P_test.h264");
	g_solution_config.box_solution.box_vpp[0].decode_type = 0;
	g_solution_config.box_solution.box_vpp[0].decode_width = 1920;
	g_solution_config.box_solution.box_vpp[0].decode_height = 1080;
	g_solution_config.box_solution.box_vpp[0].decode_frame_rate = 30;
	g_solution_config.box_solution.box_vpp[0].encode_type = 0;
	g_solution_config.box_solution.box_vpp[0].encode_width = 1920;
	g_solution_config.box_solution.box_vpp[0].encode_height = 1080;
	g_solution_config.box_solution.box_vpp[0].encode_frame_rate = 30;
	g_solution_config.box_solution.box_vpp[0].encode_bitrate = 8192;
	strcpy(g_solution_config.box_solution.box_vpp[0].model, "yolov5s");

	strcpy(g_solution_config.display_dev, "hdmi");

	bpu_wrap_get_model_list(g_solution_config.hardware_capability.model_list);

	// 保存到配置文件中
	return solution_cfg_save();
}

int32_t solution_cfg_load()
{
	if (g_solution_cfg_is_load == 1)
	{
		return 0;
	}

	if (is_file_exist(SOLUTION_CONFIG_FILE) != 0)
	{
		printf("config file %s not exist\n", SOLUTION_CONFIG_FILE);
		// 使用默认配置
		solution_cfg_load_default_config();
	}
	else
	{
		FILE *fd = fopen(SOLUTION_CONFIG_FILE, "r");
		if (fd != NULL)
		{
			fseek(fd, 0, SEEK_END);
			long file_size = ftell(fd);
			fseek(fd, 0, SEEK_SET);

			char *str_json = (char *)malloc(file_size + 1); // 动态分配内存
			if (str_json != NULL)
			{
				fread(str_json, 1, file_size, fd); // 读取文件内容到内存
				str_json[file_size] = '\0'; // 添加字符串结束符
				fclose(fd);

				SC_LOGI("read config from config file: [%s]\n", str_json);
				if (cjson_string2object(solution_cfg_key, str_json, &g_solution_config) == NULL)
				{
					SC_LOGW("config file parser failed, so use default config .");
					solution_cfg_load_default_config();
				}
				print_solution_cfg(&g_solution_config);

				free(str_json); // 释放内存
			}
			else
			{
				solution_cfg_load_default_config();
			}
		}
		else
		{
			// 文件打开失败，使用默认配置
			solution_cfg_load_default_config();
		}
	}

	sprintf(g_solution_config.version,
		"sunrise camera version: v3.0.1, build time:%s %s", __DATE__, __TIME__);
	// 获取芯片型号、接入的sensor型号、支持的算法模型清单
	vp_get_hard_capability(&g_solution_config);

	g_solution_cfg_is_load = 1;

	return 0;
}

int32_t solution_cfg_save()
{
	if (is_dir_exist(SOLUTION_CONFIG_PATH) != 0)
	{
		if (mkdir(SOLUTION_CONFIG_PATH, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) != 0)
		{
			printf("mkdir %s error", SOLUTION_CONFIG_PATH);
			return -1;
		}
	}

	char *out = cjson_object2string(solution_cfg_key, (void *)&g_solution_config);
	FILE *fd = fopen(SOLUTION_CONFIG_FILE, "w");
	if (fd != NULL)
	{
		fwrite(out, 1, strlen(out), fd);
		fclose(fd);
	}
	free(out);

	return 0;
}

char *solution_cfg_obj2string()
{
	return cjson_object2string(solution_cfg_key, (void *)&g_solution_config);
}

void solution_cfg_string2obj(char *in)
{
	cjson_string2object(solution_cfg_key, in, &g_solution_config);
}
