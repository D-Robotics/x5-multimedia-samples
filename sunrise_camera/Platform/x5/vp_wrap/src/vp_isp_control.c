
#include <math.h>
#include "vp_isp_control.h"
#include "utils/utils_log.h"
/**
 * 将0-100的整数转换为指定float范围的值
 * @param input - 输入值（0-100的整数）
 * @param min_output - 输出范围的最小值（float）
 * @param max_output - 输出范围的最大值（float）
 * @return 转换后的float值，若输入无效则返回NaN
 */
static float int_to_float_range(int input, float min_output, float max_output) {
	// 验证输入范围
	if (input < 0 || input > 100) {
		printf("错误：输入必须是0-100之间的整数\n");
		return NAN; // 返回NaN表示无效值
	}

	// 计算比例（0-1之间）
	float ratio = (float)input / 100.0f;

	// 转换到目标范围
	return min_output + (max_output - min_output) * ratio;
}

/**
 * 将float范围的值转换为0-100的整数
 * @param input - 输入的float值
 * @param min_input - 输入范围的最小值（float）
 * @param max_input - 输入范围的最大值（float）
 * @return 转换后的0-100整数，若输入无效则返回-1
 */
static int float_to_int_range(float input, float min_input, float max_input) {
	// 验证输入范围
	if (input < min_input || input > max_input) {
		printf("错误：输入值超出指定范围 [%.2f, %.2f]\n", min_input, max_input);
		return -1; // 返回-1表示无效值
	}

	// 计算比例（0-1之间）
	float ratio = (input - min_input) / (max_input - min_input);

	// 转换到0-100范围并四舍五入
	int result = (int)round(ratio * 100.0f);

	// 确保结果在0-100范围内（处理浮点精度问题）
	if (result < 0) return 0;
	if (result > 100) return 100;

	return result;
}

static const char* vp_isp_mode_to_str(hbn_isp_mode_e mode) {
	switch (mode) {
		case HBN_ISP_MODE_AUTO:
			return "auto";
		case HBN_ISP_MODE_MANUAL:
			return "manual";
		default:
			return "unknown";
	}
}
//image
int vp_isp_set_image_param(hbn_vnode_handle_t handle, hbn_isp_mode_e mode, vp_isp_image_param_t *param){
	int32_t ret = 0;
	hbn_isp_cproc_attr_t cproc_attr;
	ret = hbn_isp_get_cproc_attr(handle, &cproc_attr);
	SC_ERR_CON_EQ(ret, 0, "hbn_isp_get_cproc_attr");
	const char* mode_name = vp_isp_mode_to_str(mode);
	SC_LOGI("set image param: mode:%s brightness:%d contrast:%d saturation:%d sharpness:%d",
	   mode_name, param->brightness, param->contrast, param->saturation, param->sharpness);
	if(mode == HBN_ISP_MODE_MANUAL){
		cproc_attr.manual_attr.bright = int_to_float_range(param->brightness, -128, 127);
		cproc_attr.manual_attr.contrast = int_to_float_range(param->contrast, 0, 1.992);
		cproc_attr.manual_attr.saturation = int_to_float_range(param->saturation , 0, 1.992);
	}
	cproc_attr.mode = mode;
	ret = hbn_isp_set_cproc_attr(handle, &cproc_attr);
	SC_ERR_CON_EQ(ret, 0, "hbn_isp_get_cproc_attr");

	hbn_isp_dmsc_attr_t dmsc_attr;
	ret = hbn_isp_get_dmsc_attr(handle, &dmsc_attr);
	SC_ERR_CON_EQ(ret, 0, "hbn_isp_get_dmsc_attr");
	if(mode == HBN_ISP_MODE_MANUAL){
		for (size_t i = 0; i < HBN_DMSC_SHARPEN_LAYER_NUM; i++){
			dmsc_attr.manual_attr.sharpen_attr.factor[i].white = int_to_float_range(param->sharpness, 0, 511);
			dmsc_attr.manual_attr.sharpen_attr.factor[i].black = int_to_float_range(param->sharpness, 0, 511);
		}
	}
	dmsc_attr.mode = mode;
	ret = hbn_isp_set_dmsc_attr(handle, &dmsc_attr);
	SC_ERR_CON_EQ(ret, 0, "hbn_isp_get_dmsc_attr");

	return 0;
}
int vp_isp_get_image_param(hbn_vnode_handle_t handle, vp_isp_image_t *param){
	int32_t ret = 0;
	hbn_isp_cproc_attr_t cproc_attr;
	ret = hbn_isp_get_cproc_attr(handle, &cproc_attr);
	SC_ERR_CON_EQ(ret, 0, "hbn_isp_get_cproc_attr");
	//对比度 0 - 1.992
	//亮度 -128 - 127
	//饱和度 0 - 1.992
	vp_isp_image_param_t param_tmp;
	param_tmp.brightness = float_to_int_range(cproc_attr.manual_attr.bright, -128, 127);
	param_tmp.contrast = float_to_int_range(cproc_attr.manual_attr.contrast, 0, 1.992);
	param_tmp.saturation = float_to_int_range(cproc_attr.manual_attr.saturation, 0, 1.992);

	hbn_isp_dmsc_attr_t dmsc_attr;
	ret = hbn_isp_get_dmsc_attr(handle, &dmsc_attr);
	SC_ERR_CON_EQ(ret, 0, "hbn_isp_get_dmsc_attr");

	//锐度：0 - 511
	float sum = 0.0;
	int denominator_count = 0;
	for (size_t i = 0; i < HBN_DMSC_SHARPEN_LAYER_NUM; i++){
		sum += dmsc_attr.manual_attr.sharpen_attr.factor[i].white;
		denominator_count++;
		sum += dmsc_attr.manual_attr.sharpen_attr.factor[i].black;
		denominator_count++;
	}
	param_tmp.sharpness = float_to_int_range(sum / denominator_count, 0, 511);

	param->mode = cproc_attr.mode;
#if 0
	if(param->mode == HBN_ISP_MODE_MANUAL){
		param->manual = param_tmp;
	}else{
		param->state = param_tmp;
	}
#else
	param->manual = param_tmp;
	param->state = param_tmp;
#endif
	return 0;
}
typedef struct {
	float sensor_again_max;
	float sensor_again_min;
	float sensor_dgain_max;
	float sensor_dgain_min;
	float sensor_exp_min;
	float sensor_exp_max;
}vp_isp_exposure_limit_t;

int vp_isp_get_exposure_param_limit(hbn_vnode_handle_t handle, vp_isp_exposure_limit_t *limit){
	hbn_isp_sensor_param_t sensor_param;

	int ret = hbn_isp_get_sensor_attr(handle, &sensor_param);
	SC_ERR_CON_EQ(ret, 0, "hbn_isp_get_sensor_attr");

	limit->sensor_again_max = pow(2, sensor_param.again_max / 32.0f);
	limit->sensor_again_min = 1.0;
#if 0
	limit->sensor_dgain_max = pow(2, sensor_param.dgain_max / 32.0f);

#else
	limit->sensor_dgain_min = 1.0;
	limit->sensor_dgain_max = 255.99;
#endif
	float exp_time_per_line = 1.0f / sensor_param.lines_per_second;
	limit->sensor_exp_min = exp_time_per_line * sensor_param.exp_time_min;
	limit->sensor_exp_max = exp_time_per_line * sensor_param.exp_time_max;

	SC_LOGI("Sensor Exposure Limits: Analog Gain: %.2f ~ %.2f Digital Gain: %.2f ~ %.2f Exposure Time: %.2f ~ %.2f",
		limit->sensor_again_min, limit->sensor_again_max,
		limit->sensor_dgain_min, limit->sensor_dgain_max,
		limit->sensor_exp_min, limit->sensor_exp_max);
	return 0;
}
//exposure
int vp_isp_set_exposure_param(hbn_vnode_handle_t handle, hbn_isp_mode_e mode, vp_isp_exposure_param_t *param){
	int ret = 0;
	hbn_isp_exposure_attr_t exposure_attr;
	ret = hbn_isp_get_exposure_attr(handle, &exposure_attr);
	SC_ERR_CON_EQ(ret, 0, "hbn_isp_get_exposure_attr");

	const char* mode_name = vp_isp_mode_to_str(mode);
	SC_LOGI("set exposure param: mode:%s exposureTime:%d again:%d dgain:%d.",
			mode_name, param->exposureTime, param->again, param->dgain);

	vp_isp_exposure_limit_t exposure_limit;
	ret = vp_isp_get_exposure_param_limit(handle, &exposure_limit);
	SC_ERR_CON_EQ(ret, 0, "vp_isp_get_exposure_param_limit");

	if(mode == HBN_ISP_MODE_MANUAL){
		exposure_attr.manual_attr.exp_time = int_to_float_range(param->exposureTime,
			exposure_limit.sensor_exp_min,
			exposure_limit.sensor_exp_max);

		exposure_attr.manual_attr.again = int_to_float_range(param->again,
			exposure_limit.sensor_again_min,
			exposure_limit.sensor_again_max);
		exposure_attr.manual_attr.ispgain = int_to_float_range(param->dgain,
			exposure_limit.sensor_dgain_min,
			exposure_limit.sensor_dgain_max);
	}
	exposure_attr.mode = mode;
	ret = hbn_isp_set_exposure_attr(handle, &exposure_attr);
	SC_ERR_CON_EQ(ret, 0, "hbn_isp_set_exposure_attr");
	return 0;
}

int vp_isp_get_exposure_param(hbn_vnode_handle_t handle, vp_isp_exposure_t *param){
	int ret = 0;
	hbn_isp_exposure_attr_t exposure_attr;
	ret = hbn_isp_get_exposure_attr(handle, &exposure_attr);
	SC_ERR_CON_EQ(ret, 0, "hbn_isp_get_exposure_attr");

	SC_LOGI("get exposure param from api: exposureTime:%f again:%f dgain:%f.",
			exposure_attr.manual_attr.exp_time,
			exposure_attr.manual_attr.again,
			exposure_attr.manual_attr.ispgain);

	vp_isp_exposure_limit_t exposure_limit;
	ret = vp_isp_get_exposure_param_limit(handle, &exposure_limit);
	SC_ERR_CON_EQ(ret, 0, "vp_isp_get_exposure_param_limit");


	vp_isp_exposure_param_t param_tmp;
	param_tmp.exposureTime = float_to_int_range(
			exposure_attr.manual_attr.exp_time,
			exposure_limit.sensor_exp_min,
			exposure_limit.sensor_exp_max);
	param_tmp.again = float_to_int_range(exposure_attr.manual_attr.again,
			exposure_limit.sensor_again_min,
			exposure_limit.sensor_again_max);
	param_tmp.dgain = float_to_int_range(exposure_attr.manual_attr.ispgain,
			exposure_limit.sensor_dgain_min,
			exposure_limit.sensor_dgain_max);

	//update
	param->mode = exposure_attr.mode;
	param->manual = param_tmp;
	param->state = param_tmp;

	return 0;
}

//awb
int vp_isp_set_awb_param(hbn_vnode_handle_t handle, hbn_isp_mode_e mode, vp_isp_awb_param_t *param){
	int ret = 0;
	hbn_isp_awb_attr_t awb_attr;
	ret = hbn_isp_get_awb_attr(handle, &awb_attr);
	SC_ERR_CON_EQ(ret, 0, "hbn_isp_get_awb_attr");
	//1 - 3.996
	const char* mode_name = vp_isp_mode_to_str(mode);
	SC_LOGI("set awb param: mode:%s redGain:%d blueGain:%d.", mode_name, param->redGain, param->blueGain);

	if(mode == HBN_ISP_MODE_MANUAL){
		awb_attr.manual_attr.gain.rgain = int_to_float_range(param->redGain, 1, 3.996);
		awb_attr.manual_attr.gain.bgain = int_to_float_range(param->blueGain, 1, 3.996);
	}
	awb_attr.mode = mode;

	ret = hbn_isp_set_awb_attr(handle, &awb_attr);
	SC_ERR_CON_EQ(ret, 0, "hbn_isp_set_awb_attr");
	return 0;
}

int vp_isp_get_awb_param(hbn_vnode_handle_t handle, vp_isp_awb_t *param){
	int ret = 0;
	hbn_isp_awb_attr_t awb_attr;
	ret = hbn_isp_get_awb_attr(handle, &awb_attr);
	SC_ERR_CON_EQ(ret, 0, "hbn_isp_get_awb_attr");

	//1 - 3.996
	vp_isp_awb_param_t param_tmp;
	param_tmp.blueGain = float_to_int_range(awb_attr.manual_attr.gain.bgain, 1, 3.996);
	param_tmp.redGain = float_to_int_range(awb_attr.manual_attr.gain.rgain, 1, 3.996);

	//update
	param->mode = awb_attr.mode;
	param->manual = param_tmp;
	param->state = param_tmp;
	return 0;
}
//image_enhancement 2dnr
int vp_isp_set_image_enhancement_2dnr_param(hbn_vnode_handle_t handle, hbn_isp_mode_e mode, vp_isp_image_enhancement_param_t *param){
	int ret = 0;
	hbn_isp_2dnr_attr_t attr;
	ret = hbn_isp_get_2dnr_attr(handle, &attr);
	SC_ERR_CON_EQ(ret, 0, "hbn_isp_get_2dnr_attr");

	//0-128
	const char* mode_name = vp_isp_mode_to_str(mode);
	SC_LOGI("set 2dnr param: mode:%s level:%d.", mode_name, param->level);
	if(mode == HBN_ISP_MODE_MANUAL){
		attr.manual_attr.vst_factor = int_to_float_range(param->level, 1.0, 1000.0);
	}
	attr.mode = mode;

	ret = hbn_isp_set_2dnr_attr(handle, &attr);
	SC_ERR_CON_EQ(ret, 0, "hbn_isp_set_2dnr_attr");
	return 0;
}

int vp_isp_get_image_enhancement_2dnr_param(hbn_vnode_handle_t handle, vp_isp_image_enhancement_2dnr_t *param){
	int ret = 0;
	hbn_isp_2dnr_attr_t attr;
	ret = hbn_isp_get_2dnr_attr(handle, &attr);
	SC_ERR_CON_EQ(ret, 0, "hbn_isp_get_2dnr_attr");
	//1.0, 1000.0
	vp_isp_image_enhancement_param_t param_tmp;
	param_tmp.level = float_to_int_range(attr.manual_attr.vst_factor, 1.0, 1000.0);

	param->mode = attr.mode;
	param->manual = param_tmp;
	param->state = param_tmp;

	return 0;
}
//image_enhancement 3dnr
int vp_isp_set_image_enhancement_3dnr_param(hbn_vnode_handle_t handle, hbn_isp_mode_e mode, vp_isp_image_enhancement_param_t *param){
	int ret = 0;
	hbn_isp_3dnr_attr_t attr;
	ret = hbn_isp_get_3dnr_attr(handle, &attr);
	SC_ERR_CON_EQ(ret, 0, "hbn_isp_get_3dnr_attr");

	//0-128
	const char* mode_name = vp_isp_mode_to_str(mode);
	SC_LOGI("set 2dnr param: mode:%s level:%d.", mode_name, param->level);
	if(mode == HBN_ISP_MODE_MANUAL){
		attr.manual_attr.tnr_strength = int_to_float_range(param->level, 0, 128);
		attr.manual_attr.tnr_strength2 = int_to_float_range(param->level, 0, 128);
	}
	attr.mode = mode;

	ret = hbn_isp_set_3dnr_attr(handle, &attr);
	SC_ERR_CON_EQ(ret, 0, "hbn_isp_set_3dnr_attr");
	return 0;
}

int vp_isp_get_image_enhancement_3dnr_param(hbn_vnode_handle_t handle, vp_isp_image_enhancement_3dnr_t *param){
	int ret = 0;
	hbn_isp_3dnr_attr_t attr;
	ret = hbn_isp_get_3dnr_attr(handle, &attr);
	SC_ERR_CON_EQ(ret, 0, "hbn_isp_get_3dnr_attr");

	//0-128
	vp_isp_image_enhancement_param_t param_tmp;
	int32_t level_tmp = (attr.manual_attr.tnr_strength + attr.manual_attr.tnr_strength2) / 2;
	param_tmp.level = float_to_int_range(level_tmp, 0, 128);

	param->mode = attr.mode;
	param->manual = param_tmp;
	param->state = param_tmp;
	return 0;
}
//all params
int vp_isp_get_all_param(hbn_vnode_handle_t handle, vp_isp_all_param_t *param){

	int ret = 0;
	ret = vp_isp_get_image_param(handle, &param->image);
	if(ret != 0){
		return ret;
	}

	ret = vp_isp_get_exposure_param(handle, &param->exposure);
	if(ret != 0){
		return ret;
	}

	ret = vp_isp_get_awb_param(handle, &param->whiteBalance);
	if(ret != 0){
		return ret;
	}

	ret = vp_isp_get_image_enhancement_2dnr_param(handle, &param->nr2d);
	if(ret != 0){
		return ret;
	}

	ret = vp_isp_get_image_enhancement_3dnr_param(handle, &param->nr3d);
	if(ret != 0){
		return ret;
	}
	return 0;
}
