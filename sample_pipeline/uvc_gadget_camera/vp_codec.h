/***************************************************************************
 * @COPYRIGHT NOTICE
 * @Copyright 2024 D-Robotics, Inc.
 * @All rights reserved.
 * @Date: 2023-01-30 11:27:41
 * @LastEditTime: 2023-03-05 15:57:35
 ***************************************************************************/
#ifndef VP_CODEC_H_
#define VP_CODEC_H_

#include <semaphore.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "hb_media_codec.h"
#include "hb_media_error.h"

typedef struct vp_codec_info_s{
	int width;
	int height;
	int fps;
	media_codec_id_t encode_type;
}vp_codec_info_t;
int vp_codec_encoder_create_and_start(media_codec_context_t *media_context, vp_codec_info_t *vp_codec_info);
int vp_codec_encoder_destroy_and_stop(media_codec_context_t *media_context);

#ifdef __cplusplus
}
#endif /* extern "C" */

#endif // VP_CODEC_H_
