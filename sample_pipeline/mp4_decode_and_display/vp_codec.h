#ifndef _VP_CODEC_H__
#define _VP_CODEC_H__
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "hbmem.h"
#include "hbn_api.h"
#include "hb_media_codec.h"
#include "hb_media_error.h"

int32_t vp_codec_init(media_codec_context_t *context);
int32_t vp_codec_start(media_codec_context_t *context);
int32_t vp_codec_stop(media_codec_context_t *context);
int32_t vp_codec_restart(media_codec_context_t *context);
int32_t vp_codec_set_input(media_codec_context_t *context,
	media_codec_buffer_t *frame_buffer, uint8_t *data,
	uint32_t data_size, int32_t eos);

int32_t vp_decode_config_param(media_codec_context_t *context, media_codec_id_t codec_type,
	int32_t width, int32_t height);
int32_t vp_codec_get_output(media_codec_context_t *context, media_codec_buffer_t *frame_buffer, media_codec_output_buffer_info_t *buffer_info, int32_t timeout);
int32_t vp_codec_release_output(media_codec_context_t *context, media_codec_buffer_t *frame_buffer);

#endif // _VP_CODEC_H__