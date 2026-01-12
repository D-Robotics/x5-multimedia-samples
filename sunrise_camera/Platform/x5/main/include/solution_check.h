#ifndef _SOLUTION_CHECK_H_
#define _SOLUTION_CHECK_H_
#include "vp_ion.h"
#include "solution_struct_define.h"

typedef struct {
	int pipeline_param_vaild_count;
	vp_ion_pipeline_fixed_param_t extern_param;
	vp_ion_pipeline_param_t pipeline_params[SOLUTION_MAX_PIPELINE_COUNT];
}solution_ion_param_info_t;

typedef struct {
	int need_check_ion_theory;
	vp_ion_all_info_t ion_info;
	vp_ion_theory_calc_result_t vp_ion_theory_calc_result;
}solution_ion_context_t;

int solution_check_ion_theory_calc_result();
int solution_check_ion_is_enough(solution_ion_param_info_t *solution_param_info);

typedef struct{
	int width;
	int height;
	int fps;
}vp_codec_usr_param_t;

typedef struct{
	vp_codec_usr_param_t encode;
	vp_codec_usr_param_t decode;
}vp_codec_usr_param_single_t;

typedef struct{
	int valid_count;
	vp_codec_usr_param_single_t params[SOLUTION_MAX_PIPELINE_COUNT];
}solution_vpu_param_info_t;
float solution_check_vpu_is_enough(solution_vpu_param_info_t *solution_param_info);


// Decode 类型匹配检查
typedef struct {
	const char *input_file;
	const char* codec_type;
}solution_decode_param_single_t;

typedef struct {
	int valid_count;
	solution_decode_param_single_t params[SOLUTION_MAX_PIPELINE_COUNT];
}solution_decode_param_info_t;

void solution_check_decode_param_is_match(solution_decode_param_info_t* decode_param,
	solution_decode_param_check_info_t *check_result);

typedef struct {
	int width;
	int height;
	int fps;
}display_base_info_t;
typedef struct {
	char type[32]; //hdmi/dsi"
	char resolution_list[1024]; //1920:1080i*60/1920:1080*30
}display_dev_base_info_t;

typedef struct {
	int pipeline_id;
	char sensor_name[128];
	display_base_info_t sensor;
	display_base_info_t display;
	display_dev_base_info_t display_dev_from_config;
	int display_cur_is_connected;
	display_dev_base_info_t display_dev_current;
}solution_display_param_single_t;
typedef struct {
	int valid_count;
	solution_display_param_single_t params[SOLUTION_MAX_DISPLAY_COUNT];
}solution_display_param_info_t;
void solution_check_display_param_is_match(solution_display_param_info_t* decode_param,
	solution_display_param_check_info_t *check_result);

typedef struct {
	int pipeline_id;
	int input_width;
	int input_height;
	char sensor_name[128];

	int model_width;
	int model_height;
	char model_name[128];
}solution_bpu_param_single_t;

typedef struct {
	int valid_count;
	solution_bpu_param_single_t params[SOLUTION_MAX_DISPLAY_COUNT];
}solution_bpu_param_info_t;

void solution_check_bpu_param_is_match(solution_bpu_param_info_t* bpu_param,
	solution_bpu_param_check_info_t *check_result);
#endif
