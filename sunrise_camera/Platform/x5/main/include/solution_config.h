#ifndef SOLUTION_CONFIG_H_
#define SOLUTION_CONFIG_H_

#define STL_MAX_VPP_CAM_NUM 4
#define STL_MAX_VPP_BOX_NUM 6

typedef struct {
	char chip_type[16];
	char sensor_list[512];
	char model_list[512];
	char codec_type_list[64];
	int32_t encode_bit_rate_list[16];
	char display_dev_list[64];
} solution_hard_capability_t; // 硬件能力

typedef struct {
	char sensor[32]; // camera sensor型号
	int32_t encode_type; // 编码类型
	int32_t encode_bitrate; // 编码码率
	char model[32]; // 算法模型
} solution_cfg_cam_vpp_t;

typedef struct {
	int32_t pipeline_count; // 使能多少路camera
	int32_t max_pipeline_count;
	solution_cfg_cam_vpp_t cam_vpp[STL_MAX_VPP_CAM_NUM];
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
} solution_cfg_box_t;

typedef struct {
	char version[128];
	solution_hard_capability_t hardware_capability;
	char solution_name[32];
	solution_cfg_cam_t cam_solution;
	solution_cfg_box_t box_solution;
	char display_dev[16]; // 显示设备，支持hdmi和lcd
} solution_cfg_t;

int32_t solution_cfg_load_default_config();
int32_t solution_cfg_load();
int32_t solution_cfg_save();
char* solution_cfg_obj2string();
void solution_cfg_string2obj(char *in);

extern int32_t g_solution_cfg_is_load;
extern solution_cfg_t g_solution_config;

#endif
