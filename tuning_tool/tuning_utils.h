/***************************************************************************
 *                      COPYRIGHT NOTICE
 *             Copyright(C) 2024, D-Robotics Co., Ltd.
 *                     All rights reserved.
 ***************************************************************************/

#ifndef __TUNING_UTILS_H__
#define __TUNING_UTILS_H__

#include <stdio.h>
#include <string.h>
#include "hbn_api.h"
#include "hb_tool_server.h"
#include "hb_camera_interface.h"

#define osd_fmt(fmt) "[tuning_tool]%s: " fmt
#define osd_pr_warp(p_func_, fmt, ...) do { p_func_(osd_fmt(fmt), __func__, ##__VA_ARGS__); } while(0)
#define pr_tuning(fmt, ...)	osd_pr_warp(printf, fmt, ##__VA_ARGS__)

#define TUNING_PRINT_SIZE_MAX	128

#define DEF_MODULE_NAME { \
	"com", \
	"sif", \
	"isp", \
	"vse", \
	"gdc", \
	"codec", \
	"n2d", \
}

typedef enum df_nmi_e {
	COM_MNI,
	SIF_MNI,
	ISP_MNI,
	VSE_MNI,
	GDC_MNI,
	CODEC_MNI,
	N2D_MNI,
} df_nmi_t;

void tuning_get_filename(char *name, char *path, hbn_vnode_image_t *out_img, df_nmi_t mni);
int32_t tuning_send_raw_to_hbplayer(tool_event_t *event, const hbn_vnode_image_t *normal_buf,
				enum RAW_BIT format, int32_t pipe_id);
int32_t tuning_send_yuv_to_hbplayer(tool_event_t *event, const hbn_vnode_image_t *normal_buf,
				int32_t pipe_id);
int32_t tuning_dump_file(char *filename, hbn_vnode_image_t *out_img);


#endif // __TUNING_UTILS_H__