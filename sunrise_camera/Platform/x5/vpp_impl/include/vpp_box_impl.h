#ifndef VPP_BOX_IMPL_H_
#define VPP_BOX_IMPL_H_

#include "solution_check.h"
#include "solution_handle.h"
#include "solution_config.h"

int32_t vpp_box_init_param(void);

int32_t vpp_box_init(void);
int32_t vpp_box_uninit(void);
int32_t vpp_box_start(void);
int32_t vpp_box_stop(void);

int32_t vpp_box_param_set(SOLUTION_PARAM_E type, char* val, uint32_t length);
int32_t vpp_box_param_get(SOLUTION_PARAM_E type, char* val, uint32_t* length);
int32_t vpp_box_ion_param_get(solution_cfg_t* solution_cfg, solution_ion_param_info_t *solution_param_info);
int32_t vpp_box_vpu_param_get(solution_cfg_t* solution_cfg, solution_vpu_param_info_t *solution_param_info);
int32_t vpp_box_decode_param_get(solution_cfg_t* solution_cfg, solution_decode_param_info_t *solution_param_info);

#endif // VPP_BOX_IMPL_H_
