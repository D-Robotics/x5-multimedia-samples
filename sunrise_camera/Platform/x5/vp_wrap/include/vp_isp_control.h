#ifndef VP_ISP_CONTROL_H_
#define VP_ISP_CONTROL_H_

#ifdef __cplusplus
extern "C" {
#endif
#include "hbn_isp_api.h"
//图像
typedef struct {
	int brightness; // 亮度
	int contrast;	// 对比度
	int saturation; // 饱和度
	int sharpness;  // 锐度
} vp_isp_image_param_t;
typedef struct {
	hbn_isp_mode_e mode;	// 图像调节模式
	vp_isp_image_param_t manual;
	vp_isp_image_param_t state;	// current values in auto mode
} vp_isp_image_t;

//曝光
typedef struct {
	int exposureTime; // 曝光时间
	int again;	// 模拟增益
	int dgain;	// 数字增益
} vp_isp_exposure_param_t;
typedef struct {
	hbn_isp_mode_e mode; // 曝光调节模式
	vp_isp_exposure_param_t manual;
	vp_isp_exposure_param_t state; // current values in auto mode
} vp_isp_exposure_t;

//白平衡
typedef struct {
	int redGain;	// 红色通道增益
	int blueGain;  // 蓝色通道增益
} vp_isp_awb_param_t;
typedef struct {
	hbn_isp_mode_e mode; // 白平衡调节模式
	vp_isp_awb_param_t manual;
	vp_isp_awb_param_t state; // current values in auto mode
} vp_isp_awb_t;

//2dnr
typedef struct {
	int level; // 图像增强等级
} vp_isp_image_enhancement_param_t;
typedef struct {
	hbn_isp_mode_e mode;
	vp_isp_image_enhancement_param_t manual;
	vp_isp_image_enhancement_param_t state; // current values
} vp_isp_image_enhancement_2dnr_t;

//3dnr
typedef struct {
	hbn_isp_mode_e mode;
	vp_isp_image_enhancement_param_t manual;
	vp_isp_image_enhancement_param_t state; // current values
} vp_isp_image_enhancement_3dnr_t;

//all params
typedef struct {
	vp_isp_image_t image;
	vp_isp_exposure_t exposure;
	vp_isp_awb_t whiteBalance;
	vp_isp_image_enhancement_3dnr_t nr3d;
	vp_isp_image_enhancement_2dnr_t nr2d;
} vp_isp_all_param_t;

/*
1. 页面刷新时，获取所有参数：
	a. 当前模式
	b. 所有模式的值
2. 设置接口：
	Manual mode setting functions:
	1. 模式切换：直接调用set, 其他参数从 步骤1中获取
	2. 其他参数设置时：同样直接调用set
	Auto mode setting functions:
	1. 模式切换：只切换为 auto 模式, 调用get 函数获取当前值 然后上报
	2. 其他参数设置时：不会发生
*/
//image
int vp_isp_set_image_param(hbn_vnode_handle_t handle, hbn_isp_mode_e mode, vp_isp_image_param_t *param);
int vp_isp_get_image_state(hbn_vnode_handle_t handle, vp_isp_image_t *state);
int vp_isp_get_image_param(hbn_vnode_handle_t handle, vp_isp_image_t *param);
//exposure
int vp_isp_set_exposure_param(hbn_vnode_handle_t handle, hbn_isp_mode_e mode, vp_isp_exposure_param_t *param);
int vp_isp_get_exposure_state(hbn_vnode_handle_t handle, vp_isp_exposure_t *state);
int vp_isp_get_exposurr_param(hbn_vnode_handle_t handle, vp_isp_exposure_t *param);
//awb
int vp_isp_set_awb_param(hbn_vnode_handle_t handle, hbn_isp_mode_e mode, vp_isp_awb_param_t *param);
int vp_isp_get_awb_state(hbn_vnode_handle_t handle, vp_isp_awb_t *state);
int vp_isp_get_awb_param(hbn_vnode_handle_t handle, vp_isp_awb_t *param);
//image_enhancement 2dnr
int vp_isp_set_image_enhancement_2dnr_param(hbn_vnode_handle_t handle, hbn_isp_mode_e mode, vp_isp_image_enhancement_param_t *param);
int vp_isp_get_image_enhancement_2dnr_state(hbn_vnode_handle_t handle, vp_isp_image_enhancement_2dnr_t *state);
int vp_isp_get_image_enhancement_2dnr_param(hbn_vnode_handle_t handle, vp_isp_image_enhancement_2dnr_t *param);
//image_enhancement 3dnr
int vp_isp_set_image_enhancement_3dnr_param(hbn_vnode_handle_t handle, hbn_isp_mode_e mode, vp_isp_image_enhancement_param_t *param);
int vp_isp_get_image_enhancement_3dnr_state(hbn_vnode_handle_t handle, vp_isp_image_enhancement_3dnr_t *state);
int vp_isp_get_image_enhancement_3dnr_param(hbn_vnode_handle_t handle, vp_isp_image_enhancement_3dnr_t *param);
//all params
int vp_isp_get_all_param(hbn_vnode_handle_t handle, vp_isp_all_param_t *param);

#ifdef __cplusplus
}
#endif /* extern "C" */

#endif // VP_ISP_CONTROL_H_
