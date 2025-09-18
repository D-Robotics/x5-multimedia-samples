#ifndef VPP_ISP_PARAM_HH
#define VPP_ISP_PARAM_HH
#include "utils/cJSON.h"
#include "vp_isp_control.h"

/*
    返回值：需要调用者 释放内存
*/
char* vp_isp_all_param_to_json(int video_id, const vp_isp_all_param_t* params);
const char* hbn_isp_mode_to_str(hbn_isp_mode_e mode);
int vp_isp_json_to_awb_param(cJSON* root, hbn_isp_mode_e *mode, vp_isp_awb_param_t *param);
int vp_isp_json_to_image_param(cJSON* root, hbn_isp_mode_e *mode, vp_isp_image_param_t *param);
int vp_isp_json_to_exposure_param(cJSON* root, hbn_isp_mode_e *mode, vp_isp_exposure_param_t *param);
int vp_isp_json_to_2dnr_or_3dnr_param(cJSON* root, hbn_isp_mode_e *mode, vp_isp_image_enhancement_param_t *param);
#endif