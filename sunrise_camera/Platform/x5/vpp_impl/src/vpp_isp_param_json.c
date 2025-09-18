#include <string.h>
#include "utils/utils_log.h"
#include "vpp_isp_param_json.h"
/**
 * 将hbn_isp_mode_e枚举转换为字符串
 */
const char* hbn_isp_mode_to_str(hbn_isp_mode_e mode) {
	switch (mode) {
		case HBN_ISP_MODE_AUTO:
			return "auto";
		case HBN_ISP_MODE_MANUAL:
			return "manual";
		default:
			return "unknown";
	}
}

/**
 * 将vp_isp_image_param_t结构体转换为cJSON对象
 */
cJSON* vp_isp_image_param_to_json(const vp_isp_image_param_t* param) {
	cJSON* json = cJSON_CreateObject();
	if (json == NULL) return NULL;

	cJSON_AddNumberToObject(json, "brightness", param->brightness);
	cJSON_AddNumberToObject(json, "contrast", param->contrast);
	cJSON_AddNumberToObject(json, "saturation", param->saturation);
	cJSON_AddNumberToObject(json, "sharpness", param->sharpness);

	return json;
}

/**
 * 将vp_isp_image_t结构体转换为cJSON对象
 */
cJSON* vp_isp_image_to_json(const vp_isp_image_t* image) {
	cJSON* json = cJSON_CreateObject();
	if (json == NULL) return NULL;

	cJSON_AddStringToObject(json, "mode", hbn_isp_mode_to_str(image->mode));
	cJSON_AddItemToObject(json, "manual", vp_isp_image_param_to_json(&image->manual));
	cJSON_AddItemToObject(json, "state", vp_isp_image_param_to_json(&image->state));

	return json;
}

/**
 * 将vp_isp_exposure_param_t结构体转换为cJSON对象
 */
cJSON* vp_isp_exposure_param_to_json(const vp_isp_exposure_param_t* param) {
	cJSON* json = cJSON_CreateObject();
	if (json == NULL) return NULL;

	cJSON_AddNumberToObject(json, "exposureTime", param->exposureTime);
	cJSON_AddNumberToObject(json, "again", param->again);
	cJSON_AddNumberToObject(json, "dgain", param->dgain);

	return json;
}

/**
 * 将vp_isp_exposure_t结构体转换为cJSON对象
 */
cJSON* vp_isp_exposure_to_json(const vp_isp_exposure_t* exposure) {
	cJSON* json = cJSON_CreateObject();
	if (json == NULL) return NULL;

	cJSON_AddStringToObject(json, "mode", hbn_isp_mode_to_str(exposure->mode));
	cJSON_AddItemToObject(json, "manual", vp_isp_exposure_param_to_json(&exposure->manual));
	cJSON_AddItemToObject(json, "state", vp_isp_exposure_param_to_json(&exposure->state));

	return json;
}

/**
 * 将vp_isp_awb_param_t结构体转换为cJSON对象
 */
cJSON* vp_isp_awb_param_to_json(const vp_isp_awb_param_t* param) {
	cJSON* json = cJSON_CreateObject();
	if (json == NULL) return NULL;

	cJSON_AddNumberToObject(json, "redGain", param->redGain);
	cJSON_AddNumberToObject(json, "blueGain", param->blueGain);

	return json;
}

/**
 * 将vp_isp_awb_t结构体转换为cJSON对象
 */
cJSON* vp_isp_awb_to_json(const vp_isp_awb_t* whiteBalance) {
	cJSON* json = cJSON_CreateObject();
	if (json == NULL) return NULL;

	cJSON_AddStringToObject(json, "mode", hbn_isp_mode_to_str(whiteBalance->mode));
	cJSON_AddItemToObject(json, "manual", vp_isp_awb_param_to_json(&whiteBalance->manual));
	cJSON_AddItemToObject(json, "state", vp_isp_awb_param_to_json(&whiteBalance->state));

	return json;
}

/**
 * 将vp_isp_image_enhancement_param_t结构体转换为cJSON对象
 */
cJSON* vp_isp_image_enhancement_param_to_json(const vp_isp_image_enhancement_param_t* param) {
	cJSON* json = cJSON_CreateObject();
	if (json == NULL) return NULL;

	cJSON_AddNumberToObject(json, "level", param->level);

	return json;
}

/**
 * 将vp_isp_image_enhancement_2dnr_t结构体转换为cJSON对象
 */
cJSON* vp_isp_image_enhancement_2dnr_to_json(const vp_isp_image_enhancement_2dnr_t* nr2d) {
	cJSON* json = cJSON_CreateObject();
	if (json == NULL) return NULL;

	cJSON_AddStringToObject(json, "mode", hbn_isp_mode_to_str(nr2d->mode));
	cJSON_AddItemToObject(json, "manual", vp_isp_image_enhancement_param_to_json(&nr2d->manual));
	cJSON_AddItemToObject(json, "state", vp_isp_image_enhancement_param_to_json(&nr2d->state));

	return json;
}

/**
 * 将vp_isp_image_enhancement_3dnr_t结构体转换为cJSON对象
 */
cJSON* vp_isp_image_enhancement_3dnr_to_json(const vp_isp_image_enhancement_3dnr_t* nr3d) {
	cJSON* json = cJSON_CreateObject();
	if (json == NULL) return NULL;

	cJSON_AddStringToObject(json, "mode", hbn_isp_mode_to_str(nr3d->mode));
	cJSON_AddItemToObject(json, "manual", vp_isp_image_enhancement_param_to_json(&nr3d->manual));
	cJSON_AddItemToObject(json, "state", vp_isp_image_enhancement_param_to_json(&nr3d->state));

	return json;
}

/**
 * 将vp_isp_all_param_t结构体转换为JSON字符串
 * @param video_id 视频ID
 * @param params 要转换的参数结构体
 * @return 生成的JSON字符串，需要调用者使用free()释放
 */
char* vp_isp_all_param_to_json(int video_id, const vp_isp_all_param_t* params) {
	if (params == NULL) return NULL;

	// 创建顶层JSON对象
	cJSON* root = cJSON_CreateObject();
	if (root == NULL) return NULL;

	// 添加video_id字段
	cJSON_AddNumberToObject(root, "video_id", video_id);

	// 创建param对象并添加所有参数
	cJSON* param = cJSON_CreateObject();
	if (param == NULL) {
		cJSON_Delete(root);
		return NULL;
	}

	// 添加各个参数模块
	cJSON_AddItemToObject(param, "image", vp_isp_image_to_json(&params->image));
	cJSON_AddItemToObject(param, "exposure", vp_isp_exposure_to_json(&params->exposure));
	cJSON_AddItemToObject(param, "whiteBalance", vp_isp_awb_to_json(&params->whiteBalance));
	cJSON_AddItemToObject(param, "nr2d", vp_isp_image_enhancement_2dnr_to_json(&params->nr2d));
	cJSON_AddItemToObject(param, "nr3d", vp_isp_image_enhancement_3dnr_to_json(&params->nr3d));

	// 将param添加到顶层对象
	cJSON_AddItemToObject(root, "params", param);

	// 转换为字符串
	char* json_str = cJSON_Print(root);
	// 释放cJSON对象
	cJSON_Delete(root);

	return json_str;
}


/**
 * 解析JSON到图像参数
 */
int vp_isp_json_to_image_param(cJSON* root, hbn_isp_mode_e *mode, vp_isp_image_param_t *param) {
	if (!root || !mode || !param) {
		SC_LOGE("Invalid input parameters\n");
		return -1;
	}

	// 解析模式
	cJSON* mode_json = cJSON_GetObjectItem(root, "mode");
	if (!cJSON_IsString(mode_json)) {
		SC_LOGE("Missing or invalid 'mode' in JSON\n");
		return -1;
	}
	if (strcmp(mode_json->valuestring, "auto") == 0) {
		*mode = HBN_ISP_MODE_AUTO;
	} else if (strcmp(mode_json->valuestring, "manual") == 0) {
		*mode = HBN_ISP_MODE_MANUAL;
	} else {
		SC_LOGE("Invalid mode value: %s\n", mode_json->valuestring);
		return -1;
	}

	// 解析图像参数
	cJSON* brightness = cJSON_GetObjectItem(root, "brightness");
	cJSON* contrast = cJSON_GetObjectItem(root, "contrast");
	cJSON* saturation = cJSON_GetObjectItem(root, "saturation");
	cJSON* sharpness = cJSON_GetObjectItem(root, "sharpness");

	if (!cJSON_IsNumber(brightness) || !cJSON_IsNumber(contrast) ||
		!cJSON_IsNumber(saturation) || !cJSON_IsNumber(sharpness)) {
		SC_LOGE("Missing or invalid image parameters in JSON\n");
		return -1;
	}

	param->brightness = brightness->valueint;
	param->contrast = contrast->valueint;
	param->saturation = saturation->valueint;
	param->sharpness = sharpness->valueint;

	return 0;
}

/**
 * 解析JSON到曝光参数
 */
int vp_isp_json_to_exposure_param(cJSON* root, hbn_isp_mode_e *mode, vp_isp_exposure_param_t *param) {
	if (!root || !mode || !param) {
		SC_LOGE("Invalid input parameters\n");
		return -1;
	}

	// 解析模式
	cJSON* mode_json = cJSON_GetObjectItem(root, "mode");
	if (!cJSON_IsString(mode_json)) {
		SC_LOGE("Missing or invalid 'mode' in JSON\n");
		return -1;
	}
	if (strcmp(mode_json->valuestring, "auto") == 0) {
		*mode = HBN_ISP_MODE_AUTO;
	} else if (strcmp(mode_json->valuestring, "manual") == 0) {
		*mode = HBN_ISP_MODE_MANUAL;
	} else {
		SC_LOGE("Invalid mode value: %s\n", mode_json->valuestring);
		return -1;
	}

	// 解析曝光参数
	cJSON* exposureTime = cJSON_GetObjectItem(root, "exposureTime");
	cJSON* again = cJSON_GetObjectItem(root, "again");
	cJSON* dgain = cJSON_GetObjectItem(root, "dgain");

	if (!cJSON_IsNumber(exposureTime) || !cJSON_IsNumber(again) ||
		!cJSON_IsNumber(dgain)) {
		SC_LOGE("Missing or invalid exposure parameters in JSON\n");
		return -1;
	}

	param->exposureTime = exposureTime->valueint;
	param->again = again->valueint;
	param->dgain = dgain->valueint;

	return 0;
}

/**
 * 解析JSON到白平衡参数
 */
int vp_isp_json_to_awb_param(cJSON* root, hbn_isp_mode_e *mode, vp_isp_awb_param_t *param) {
	if (!root || !mode || !param) {
		SC_LOGE("Invalid input parameters\n");
		return -1;
	}

	// 解析模式
	cJSON* mode_json = cJSON_GetObjectItem(root, "mode");
	if (!cJSON_IsString(mode_json)) {
		SC_LOGE("Missing or invalid 'mode' in JSON\n");
		return -1;
	}
	if (strcmp(mode_json->valuestring, "auto") == 0) {
		*mode = HBN_ISP_MODE_AUTO;
	} else if (strcmp(mode_json->valuestring, "manual") == 0) {
		*mode = HBN_ISP_MODE_MANUAL;
	} else {
		SC_LOGE("Invalid mode value: %s\n", mode_json->valuestring);
		return -1;
	}

	// 解析白平衡参数
	cJSON* redGain = cJSON_GetObjectItem(root, "redGain");
	cJSON* blueGain = cJSON_GetObjectItem(root, "blueGain");

	if (!cJSON_IsNumber(redGain) || !cJSON_IsNumber(blueGain)) {
		SC_LOGE("Missing or invalid AWB parameters in JSON\n");
		return -1;
	}

	param->redGain = redGain->valueint;
	param->blueGain = blueGain->valueint;

	return 0;
}

/**
 * 解析JSON到2D/3D降噪参数
 */
int vp_isp_json_to_2dnr_or_3dnr_param(cJSON* root, hbn_isp_mode_e *mode, vp_isp_image_enhancement_param_t *param) {
	if (!root || !mode || !param) {
		SC_LOGE("Invalid input parameters\n");
		return -1;
	}

	// 解析模式
	cJSON* mode_json = cJSON_GetObjectItem(root, "mode");
	if (!cJSON_IsString(mode_json)) {
		SC_LOGE("Missing or invalid 'mode' in JSON\n");
		return -1;
	}
	if (strcmp(mode_json->valuestring, "auto") == 0) {
		*mode = HBN_ISP_MODE_AUTO;
	} else if (strcmp(mode_json->valuestring, "manual") == 0) {
		*mode = HBN_ISP_MODE_MANUAL;
	} else {
		SC_LOGE("Invalid mode value: %s\n", mode_json->valuestring);
		return -1;
	}

	// 解析降噪强度参数
	cJSON* level = cJSON_GetObjectItem(root, "level");
	if (!cJSON_IsNumber(level)) {
		SC_LOGE("Missing or invalid 'level' in JSON\n");
		return -1;
	}

	param->level = level->valueint;

	return 0;
}
