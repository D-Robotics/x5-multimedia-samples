#include <string.h>
#include "solution_check.h"
#include "utils/utils_log.h"

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/avutil.h"

solution_ion_context_t solution_ion_context = {
	.need_check_ion_theory = 0,
};


int solution_check_ion_is_enough(solution_ion_param_info_t *solution_param_info){
	int ret = 0;
	vp_ion_all_info_t *ion_info = &solution_ion_context.ion_info;
	vp_ion_theory_calc_result_t *theory_result = &solution_ion_context.vp_ion_theory_calc_result;
	memset(theory_result, 0, sizeof(vp_ion_theory_calc_result_t));

	SC_LOGI("solution_check_ion ...");
	//1. 获取当前的ION 内存占用情况
	vp_ion_get_current_status(ion_info);

	//2. 理论计算: 动态变化的参数
	vp_ion_theory_calc_result_t tmp_theory_result;
	for (int i = 0; i < solution_param_info->pipeline_param_vaild_count; i++){
		vp_ion_pipeline_calculator(&solution_param_info->pipeline_params[i], &tmp_theory_result);

		theory_result->osd_size += tmp_theory_result.osd_size;
		theory_result->vpu_size += tmp_theory_result.vpu_size;
		theory_result->bpu_size += tmp_theory_result.bpu_size;
		theory_result->vflow_size += tmp_theory_result.vflow_size;
		theory_result->camera_service_size += tmp_theory_result.camera_service_size;
	}

	//3. 理论计算：固定的参数
	vp_ion_pipeline_fixed_calculator(&solution_param_info->extern_param, &tmp_theory_result);
	theory_result->osd_size += tmp_theory_result.osd_size;
	theory_result->vpu_size += tmp_theory_result.vpu_size;
	theory_result->bpu_size += tmp_theory_result.bpu_size;
	theory_result->vflow_size += tmp_theory_result.vflow_size;
	theory_result->camera_service_size += tmp_theory_result.camera_service_size;

	//4. 打印理算计算结果
	vp_ion_pipeline_theory_result_printf(theory_result);

	//5. 计算ION资源是否足够
	ret = vp_ion_check_is_enough(ion_info, theory_result);
	if(ret == 0){
		SC_LOGI("found ion is enough.");
		//只有在足够的情况下，才需要检查：不够时会用以前的配置
		solution_ion_context.need_check_ion_theory = 1;
	}else{
		if(ret < 0){
			ret = -ret;
		}
		SC_LOGI("found ion is lack %d", ret);
		solution_ion_context.need_check_ion_theory = 0;
	}
	return ret;
}

int solution_check_ion_theory_calc_result(){



	if(solution_ion_context.need_check_ion_theory){
		solution_ion_context.need_check_ion_theory = 0;
//TODO: 盒子模式完善后再打开
#if 0
		vp_ion_all_info_t *ion_info = &solution_ion_context.ion_info;
		vp_ion_theory_calc_result_t *theory_result = &solution_ion_context.vp_ion_theory_calc_result;
		vp_ion_check_theory_result(ion_info, theory_result);
#endif
	}
	return 0;
}

float solution_check_vpu_is_enough(solution_vpu_param_info_t *solution_param_info){
	int vpu_capbility = 3840 * 2160 * 60;
	int vpu_capbility_unit = 1920 * 1080 * 30;

	int theory_cal_result = 0;
	for (int i = 0; i < solution_param_info->valid_count; i++){
		vp_codec_usr_param_single_t *single_param = &solution_param_info->params[i];
		theory_cal_result += single_param->encode.fps * single_param->encode.height * single_param->encode.width;
		theory_cal_result += single_param->decode.fps * single_param->decode.height * single_param->decode.width;
	}

	float ret_tmp = (theory_cal_result - vpu_capbility) / vpu_capbility_unit;
	if(vpu_capbility >= theory_cal_result){
		SC_LOGI("vpu capbility is enough: remain:%d(equal:%f*1080P30) total:%d, theory:%d",
			vpu_capbility - theory_cal_result, -ret_tmp, vpu_capbility, theory_cal_result);
	}else{
		SC_LOGI("vpu capbility is not enough %f*1080P30(%d)", ret_tmp, theory_cal_result - vpu_capbility);
		return ret_tmp;
	}
	return 0.0;
}

const char *get_video_codec_type(const char *url)
{
	const char *codec_type = NULL;
	AVFormatContext *format_ctx = NULL;
	AVCodecParameters *codec_params = NULL;
	AVDictionary *option = NULL;
	av_dict_set(&option, "stimeout", "2000000", 0);
	av_dict_set(&option, "bufsize", "1024000", 0);
	av_dict_set(&option, "rtsp_transport", "tcp", 0);

	const AVCodec *codec = NULL;
	av_log_set_level(AV_LOG_FATAL); // 只输出严重错误
	avformat_network_init();
	if (avformat_open_input(&format_ctx, url, NULL, &option) < 0)
	{
		fprintf(stderr, "无法打开文件或 URL: %s\n", url);
		return "error";
	}
	if (avformat_find_stream_info(format_ctx, NULL) < 0)
	{
		fprintf(stderr, "无法获取流信息: %s\n", url);
		avformat_close_input(&format_ctx);
		return "unsupport";
	}

	for (unsigned int i = 0; i < format_ctx->nb_streams; i++)
	{
		codec_params = format_ctx->streams[i]->codecpar;
		if (codec_params->codec_type == AVMEDIA_TYPE_VIDEO)
		{
			codec = avcodec_find_decoder(codec_params->codec_id);
			if (codec == NULL)
			{
				codec_type = "unsupport";
				fprintf(stderr, "未找到解码器: %s\n", url);
				break;
			}

			if (codec_params->codec_id == AV_CODEC_ID_H264)
			{
				codec_type = "h264";
			}
			else if (codec_params->codec_id == AV_CODEC_ID_H265)
			{
				codec_type = "h265";
			}
			else if (codec_params->codec_id == AV_CODEC_ID_MJPEG)
			{
				codec_type = "jpeg";
			}
			else
			{
				codec_type = "unsupport";
			}
			break;
		}
	}
	avformat_close_input(&format_ctx);
	return codec_type;
}

void solution_check_decode_param_is_match(solution_decode_param_info_t* decode_param,
	solution_decode_param_check_info_t *check_result){
	check_result->not_match_count = 0;
	for(int i = 0; i< decode_param->valid_count; i++){
		const char *codec_type = get_video_codec_type(decode_param->params[i].input_file);
		if(strcmp(codec_type, "error") == 0){
			check_result->codec_info[i].actual_codec_type = "error";
		}else if(strcmp(codec_type, "unsupport") == 0){
			check_result->codec_info[i].actual_codec_type = "unsupport";
		}else if(strcmp(codec_type, decode_param->params[i].codec_type) == 0){
			continue;
		}else{
			check_result->codec_info[i].actual_codec_type = codec_type;
		}
		check_result->codec_info[i].pipeline_id = i;
		check_result->codec_info[i].config_codec_type = decode_param->params[i].codec_type;

		check_result->not_match_count++;
	}
}


static int is_string_not_match(const char *str1, const char *str2) {
	// 若任一字符串为空，或内容不一致，返回1；否则返回0
	if (str1 == NULL || str2 == NULL) {
		return 1;
	}
	return strcmp(str1, str2) != 0 ? 1 : 0;
}
static int is_param_not_match(display_base_info_t *sensor, display_base_info_t *display) {
	return (sensor->width != display->width) ||
		   (sensor->height != display->height) ||
		   (sensor->fps != display->fps);
}
void solution_check_display_param_is_match(solution_display_param_info_t* display_param,
										   solution_display_param_check_info_t *check_result) {
	// 入参合法性校验
	if (display_param == NULL || check_result == NULL) {
		return;
	}

	// 初始化检测结果：清空计数和数组（避免脏数据）
	check_result->not_match_count = 0;
	memset(check_result->dispaly_info, 0, sizeof(check_result->dispaly_info));

	// 遍历所有有效流水线参数
	for (int i = 0; i < display_param->valid_count; i++) {
		// 跳过超出最大显示计数的情况，避免数组越界
		if (check_result->not_match_count >= SOLUTION_MAX_DISPLAY_COUNT) {
			break;
		}

		solution_display_param_single_t *single_param = &display_param->params[i];
		solution_display_param_check_single_t *check_single = &check_result->dispaly_info[check_result->not_match_count];
		int has_error = 0;

		// 初始化检测结果的流水线ID（关联错误到具体流水线）
		check_single->pipeline_id = single_param->pipeline_id;
		check_single->error_type = SentinelDisplayErrorType;

		// 1. 检测1：显示器是否断开（display_cur_is_connected为0表示断开）
		if (single_param->display_cur_is_connected == 0) {
			check_single->error_type = DisplayIsDisconnect;
			has_error = 1;
		}
		// 2. 检测2：显示器是否更换（配置的type与当前的type不一致）
		else if (is_string_not_match(single_param->display_dev_from_config.type, single_param->display_dev_current.type)||
			(is_string_not_match(single_param->display_dev_from_config.resolution_list, single_param->display_dev_current.resolution_list))) {
			check_single->error_type = DisplayIsChange;
			// 拷贝当前和配置的分辨率列表（用于排查问题）
			strncpy(check_single->current_type, single_param->display_dev_current.type, sizeof(check_single->current_type) - 1);
			strncpy(check_single->config_type, single_param->display_dev_from_config.type, sizeof(check_single->config_type) - 1);

			strncpy(check_single->current_display_resolution_list, single_param->display_dev_current.resolution_list, sizeof(check_single->current_display_resolution_list) - 1);
			strncpy(check_single->config_display_resolution_list, single_param->display_dev_from_config.resolution_list, sizeof(check_single->config_display_resolution_list) - 1);
			has_error = 1;
		}
		// 3. 检测3：显示器参数与Sensor参数是否匹配
		else {
			// 3.1 对比Sensor和Display的宽、高、帧率
			if (is_param_not_match(&single_param->sensor, &single_param->display)) {
				check_single->error_type = DisplayParamIsNotMatch;
				// 记录不匹配的具体参数
				check_single->sensor_width = single_param->sensor.width;
				check_single->sensor_height = single_param->sensor.height;
				check_single->sensor_fps = single_param->sensor.fps;
				check_single->display_width = single_param->display.width;
				check_single->display_height = single_param->display.height;
				check_single->display_fps = single_param->display.fps;
				// 拷贝分辨率列表（用于排查分辨率不匹配问题）
				strncpy(check_single->current_display_resolution_list, single_param->display_dev_current.resolution_list, sizeof(check_single->current_display_resolution_list) - 1);
				strncpy(check_single->config_display_resolution_list, single_param->display_dev_from_config.resolution_list, sizeof(check_single->config_display_resolution_list) - 1);
				has_error = 1;
			}
		}

		// 若存在错误，计数+1
		if (has_error) {
			check_result->not_match_count++;
		}
	}
}

void solution_check_bpu_param_is_match(solution_bpu_param_info_t* bpu_param,
										   solution_bpu_param_check_info_t *check_result){
	check_result->not_match_count = 0;
	for (int i = 0; i < bpu_param->valid_count; i++){
		solution_bpu_param_single_t *param_single = &bpu_param->params[i];
		if(((param_single->input_width < param_single->model_width)
			&& (param_single->input_height > param_single->model_height))
			|| ((param_single->input_width > param_single->model_width)
			&& (param_single->input_height < param_single->model_height))){
				solution_bpu_param_check_single_t *check_single = &check_result->bpu_info[check_result->not_match_count];
				check_single->input_width = param_single->input_width;
				check_single->input_height = param_single->input_height;
				strcpy(check_single->sensor_name, param_single->sensor_name);

				check_single->model_width = param_single->model_width;
				check_single->model_height = param_single->model_height;
				strcpy(check_single->model_name, param_single->model_name);
				check_result->not_match_count++;
			}
	}
}