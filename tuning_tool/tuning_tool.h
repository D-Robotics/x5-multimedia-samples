/***************************************************************************
 *                      COPYRIGHT NOTICE
 *             Copyright(C) 2024, D-Robotics Co., Ltd.
 *                     All rights reserved.
 ***************************************************************************/

#ifndef __TUNING_TOOL_H__
#define __TUNING_TOOL_H__

#include "hbn_isp_api.h"
#include "isp_cfg.h"
#include "tuning_utils.h"
#include "tuning_cmd.h"

#define DEF_CAM_PATH "/app/platform_samples/tuning_tool/tuning_cfg/sc1330t_rx0/cam_x5_config.json"
#define DEF_VPM_PATH "/app/platform_samples/tuning_tool/tuning_cfg/sc1330t_rx0/vpm_x5_config.json"
#define DEF_DUMP_PATH "/userdata"

#define HBPLAYER_EN 1
#define FEEDBACK_MASK 0
#define START_DUMP_MASK 1

typedef struct tuning_context {
	char cam_json[128];
	char vpm_json[128];
	uint32_t dump_mask;
	uint32_t send_raw;
	uint32_t dump_stream_flag;
	char feedback_path[128];
	uint32_t work_mode;

	uint32_t yuv_dump_cnt;
	uint32_t is_offline;
	pthread_t main_thid;
	pthread_t api_thid;
	uint32_t err_cnt;
	hbn_vflow_handle_t vflow_fd;
	hbn_vnode_handle_t vnode_fd[2];	// 0-sif, 1-isp
	tool_event_t *hbplayer_event;
	uint32_t feedback_width;
	uint32_t feedback_height;
	hbn_vnode_image_t src_img;
} tuning_context_t;

typedef struct tuning_cmd_func {
	char cmd;
	void (*api_func)(tuning_context_t *ctx);
} tuning_cmd_func_t;

#define main_while_func_run(func, ctx) {	\
	unsigned int cmd;			\
	while ((cmd = getchar()) != EOF)	\
	{					\
		if (cmd == '\n') continue;	\
		if (cmd == 'q') break;		\
		(func(cmd, (ctx)));		\
		for(;;) {			\
			if (getchar() == '\n')	\
				break;		\
		}				\
	}					\
	}

#define read_p(text, type, pparam) do {	\
		printf("%s", text); \
		scanf(type, (pparam)); \
	} while(0)

#define VIO_ASSERT_FUNC_EQ(func, val, retfunc) do { \
		if ((func) != (val)) { \
			pr_tuning("error: %s(%d)[%s ne %d]\n", __func__, __LINE__, #func, (val)); \
			retfunc; \
		} \
	} while(0)

#define VIO_ASSERT_FUNC_NE(func, val, retfunc) do { \
		if ((func) == (val)) { \
			pr_tuning("error: %s(%d)[%s eq %d]\n", __func__, __LINE__, #func, (val)); \
			retfunc; \
		} \
	} while(0)

#endif // __TUNING_TOOL_H__