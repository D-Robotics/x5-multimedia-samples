#ifndef SOLUTION_CONFIG_H_
#define SOLUTION_CONFIG_H_

#define STL_MAX_VPP_CAM_NUM 4
#define STL_MAX_VPP_BOX_NUM 6
#define STL_MAX_VPP_DISPLAY_NUM 1
#include "vp_sensors.h"
#include "vp_common.h"
#include "solution_check.h"

typedef struct {
	char chip_type[16];
	int32_t sensor_info_valid_count;
	csi_list_info_t csi_list_info;
	char model_list[512];
	char codec_type_list[64];
	int32_t encode_bit_rate_list[16];
	char display_dev_list[64];
	char gdc_status_list[64];
} solution_hard_capability_t; // 硬件能力

typedef struct {
	int32_t is_valid;
	int32_t is_enable;
	int32_t csi_index;
	int32_t mclk_is_not_configed; //设备树中是否配置了 mclk
	char sensor[32]; // camera sensor型号
	int32_t encode_type; // 编码类型
	int32_t encode_bitrate; // 编码码率
	char model[32]; // 算法模型
	int32_t gdc_status; //0: 没有gdc file， 1： 关闭 gdc, 2： 打开gdc
} solution_cfg_cam_vpp_t;

typedef struct {
	int32_t is_valid;
	int32_t is_enable;
	char resolution[32];
	int data_source;
	int display_index;
}solution_cfg_display_t;

typedef struct {
	int32_t pipeline_count; // 使能多少路camera
	int32_t max_pipeline_count;
	solution_cfg_cam_vpp_t cam_vpp[STL_MAX_VPP_CAM_NUM];
	solution_cfg_display_t display_vpp[STL_MAX_VPP_DISPLAY_NUM];
} solution_cfg_cam_t;

// 示例代码的编解码配置，为了直观的呈现效果，先解码在编码推流
typedef struct {
	char stream[128]; // 视频码流，支持码流文件和rtsp码流
	// 编解码类型。0： H264; 1: H265； 2：Mjpeg
	int32_t decode_type; // 解码类型
	int32_t decode_width;
	int32_t decode_height;
	int32_t decode_frame_rate;
	int32_t encode_type; // 编码类型
	int32_t encode_width;
	int32_t encode_height;
	int32_t encode_frame_rate;
	int32_t encode_bitrate; // 编码码率
	char model[32]; // 算法模型
} solution_cfg_box_vpp_t;

typedef struct {
	int32_t pipeline_count; // 使能多少路视频编解码
	int32_t max_pipeline_count;
	solution_cfg_box_vpp_t box_vpp[STL_MAX_VPP_BOX_NUM];

	//todo: add solution_cfg_display_t
} solution_cfg_box_t;

typedef struct {
	int is_valid;
	char type[32]; //hdmi/dsi"
	char resolution_list[1024]; //1920:1080i*60/1920:1080*30
}solution_display_dev_t;
typedef struct {
	char version[128];
	solution_hard_capability_t hardware_capability;
	solution_display_dev_t display_devs[STL_MAX_VPP_DISPLAY_NUM];

	char solution_name[32];
	solution_cfg_cam_t cam_solution;
	solution_cfg_box_t box_solution;
} solution_cfg_t;

int32_t solution_cfg_load_default_config();
int32_t solution_cfg_load();
int32_t solution_cfg_save();
char* solution_cfg_obj2string();
void solution_cfg_string2obj(char *in);
int32_t solution_cfg_update_camera_config();
void solution_cfg_string2obj_width_param(char *in, solution_cfg_t *solution_cfg);
int32_t solution_cam_display_param_get(solution_cfg_t *solution_cfg, solution_display_param_info_t *param_info);
int32_t solution_cfg_update_display_config();

int32_t solution_bpu_param_get(solution_cfg_t *solution_cfg, solution_bpu_param_info_t *param_info);

extern int32_t g_solution_cfg_is_load;
extern solution_cfg_t g_solution_config;

#endif
