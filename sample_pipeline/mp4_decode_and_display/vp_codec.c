#include "vp_codec.h"
extern int enable_debug_info;
#define TAG "[Decoder]"

int32_t vp_codec_init(media_codec_context_t *context)
{
	int32_t ret = 0;

	ret = hb_mm_mc_initialize(context);
	if (0 != ret)
	{
		printf("hb_mm_mc_initialize failed.\n");
		return -1;
	}

	ret = hb_mm_mc_configure(context);
	if (0 != ret)
	{
		printf("hb_mm_mc_configure failed.\n");
		hb_mm_mc_release(context);
		return -1;
	}

	printf("%s %s idx: %d, init successful\n", TAG, context->encoder ? "Encode" : "Decode", context->instance_index);
	return 0;
}

int32_t vp_codec_deinit(media_codec_context_t *context)
{
	int32_t ret = 0;

	ret = hb_mm_mc_release(context);
	if (ret != 0)
	{
		printf("Failed to hb_mm_mc_release ret = %d \n", ret);
		return -1;
	}

	printf("%s %s idx: %d, deinit successful\n", TAG, context->encoder ? "Encode" : "Decode", context->instance_index);
	return 0;
}

int32_t vp_codec_start(media_codec_context_t *context)
{
	int32_t ret = 0;
	mc_av_codec_startup_params_t startup_params = {0};

	ret = hb_mm_mc_start(context, &startup_params);
	if (ret != 0)
	{
		printf("%s:%d hb_mm_mc_start failed.\n", __FUNCTION__, __LINE__);
		return -1;
	}

	printf("%s %s idx: %d, start successful\n", TAG, context->encoder ? "Encode" : "Decode", context->instance_index);
	return ret;
}

int32_t vp_codec_stop(media_codec_context_t *context)
{
	int32_t ret = 0;
	ret = hb_mm_mc_pause(context);
	if (ret != 0)
	{
		printf("Failed to hb_mm_mc_pause ret = %d \n", ret);
		return -1;
	}

	printf("%s %s idx: %d, stop successful\n", TAG, context->encoder ? "Encode" : "Decode", context->instance_index);
	return ret;
}
int32_t vp_codec_restart(media_codec_context_t *context)
{
	int32_t ret = 0;

	ret = hb_mm_mc_stop(context);
	if (ret != 0)
	{
		printf("%s:%d Failed to hb_mm_mc_stop ret = %d \n",
				__FUNCTION__, __LINE__, ret);
		return -1;
	}
	printf("%s %s idx: %d, restart successful\n", TAG, context->encoder ? "Encode" : "Decode", context->instance_index);
	return 0;
}

int32_t vp_codec_set_input(media_codec_context_t *context,
	media_codec_buffer_t *frame_buffer, uint8_t *data,
	uint32_t data_size, int32_t eos)
{
	int32_t ret = 0;
	media_codec_buffer_t *buffer = NULL;

	if ((context == NULL) || (frame_buffer == NULL) || (!eos && (data == NULL)))
	{
		printf("codec param is NULL!\n");
		return -1;
	}

	buffer = frame_buffer;

	buffer->type = (context->encoder) ? MC_VIDEO_FRAME_BUFFER : MC_VIDEO_STREAM_BUFFER;
	ret = hb_mm_mc_dequeue_input_buffer(context, buffer, 2000);
	if (ret != 0)
	{
		printf("hb_mm_mc_dequeue_input_buffer failed ret = %d\n", ret);
		return -1;
	}

	if (context->encoder == false)
	{
		if (buffer->vstream_buf.size < data_size)
		{
			printf("The input stream/frame data is larger than the stream buffer size\n");
			hb_mm_mc_queue_input_buffer(context, buffer, 3000);
			return -1;
		}

		buffer->type = MC_VIDEO_STREAM_BUFFER;
		if (eos == 0)
		{
			buffer->vstream_buf.size = data_size;
			buffer->vstream_buf.stream_end = 0;
		}
		else
		{
			buffer->vstream_buf.size = 0;
			buffer->vstream_buf.stream_end = 1;
		}
		if (enable_debug_info) {
			printf("buffer->vstream_buf.size: %d\n", buffer->vstream_buf.size);
			printf("buffer->vstream_buf.vir_ptr: %p\n", buffer->vstream_buf.vir_ptr);
		}

		memcpy(buffer->vstream_buf.vir_ptr, data, data_size);
	}

	ret = hb_mm_mc_queue_input_buffer(context, buffer, 2000);
	if (ret != 0)
	{
		printf("hb_mm_mc_queue_input_buffer failed, ret = 0x%x\n", ret);
		return -1;
	}

	if (enable_debug_info)
		printf("%s idx: %d, set input successful\n", context->encoder ? "Encode" : "Decode", context->instance_index);
	return ret;
}

int32_t vp_codec_get_output(media_codec_context_t *context, media_codec_buffer_t *frame_buffer, media_codec_output_buffer_info_t *buffer_info, int32_t timeout)
{
	int32_t ret = 0;
	media_codec_output_buffer_info_t *info = NULL;
	media_codec_buffer_t *buffer = NULL;

	if ((context == NULL) || (frame_buffer == NULL) || (buffer_info == NULL))
	{
		printf("codec param is NULL\n");
		return -1;
	}
	buffer = frame_buffer;
	info = buffer_info;

	ret = hb_mm_mc_dequeue_output_buffer(context, buffer, info, timeout);
	if (ret != 0 && ret != -268435443)  // Check for timeout error
	{
		printf("%s idx: %d, %s ret = %d\n",
			context->encoder ? "Encode" : "Decode", context->instance_index,
			ret == -1 ? "hb_mm_mc_dequeue_output_buffer failed" : "hb_mm_mc_dequeue_output_buffer encountered an error",
			ret);
		return -1;
	}
	else if (ret == -268435443)
	{
        //timeout
		return -2;
	}
    // For decoder, if the output buffer type is not a frame buffer, return it directly
	if ((!context->encoder) && (buffer->type != MC_VIDEO_FRAME_BUFFER))
	{
		if (buffer != NULL)
		{
			ret = hb_mm_mc_queue_output_buffer(context, buffer, 0);
			if (ret != 0)
			{
				printf("idx: %d, hb_mm_mc_queue_output_buffer failed ret = %d \n", context->instance_index, ret);
				return -1;
			}
		}
		return -1;
	}

	if (context->codec_id >= MEDIA_CODEC_ID_H264 ||
		context->codec_id <= MEDIA_CODEC_ID_JPEG)
	{
		if (context->encoder == 0) 
		{
            //for decoder, if decode_result is 0 or frame size is 0, return it directly
			if (info->video_frame_info.decode_result == 0 || buffer->vframe_buf.size == 0)
			{
				if (buffer != NULL)
				{
					ret = hb_mm_mc_queue_output_buffer(context, buffer, 0);
					if (ret != 0)
					{
						printf("idx: %d, hb_mm_mc_queue_output_buffer failed ret = %d\n", context->instance_index, ret);
						return -1;
					}
				}
				return -1;
			}

			if (enable_debug_info) {
				printf("%s Decodec idx: %d type:%d get frame size:%d\n",
					TAG, context->instance_index, context->codec_id, buffer->vframe_buf.size);
			}
		}
	}

	return ret;
}

int32_t vp_codec_release_output(media_codec_context_t *context, media_codec_buffer_t *frame_buffer)
{
	int32_t ret = 0;
	media_codec_buffer_t *buffer = NULL;

	if ((context == NULL) || (frame_buffer == NULL))
	{
		printf("codec param is NULL!\n");
		return -1;
	}
	buffer = frame_buffer;

	if (enable_debug_info) {
		printf("%s idx: %d type:%d, buffer:%p\n",
			context->encoder ? "Encode" : "Decode", context->instance_index, context->codec_id, buffer);
	}

	if (buffer != NULL)
	{
		ret = hb_mm_mc_queue_output_buffer(context, buffer, 0);
		if (ret != 0)
		{
			printf("idx: %d, hb_mm_mc_queue_output_buffer failed ret = %d \n", context->instance_index, ret);
			return -1;
		}
	}

	return ret;
}

int32_t vp_decode_config_param(media_codec_context_t *context, media_codec_id_t codec_type,
	int32_t width, int32_t height, int32_t frame_buffer_count)
{
	mc_video_codec_dec_params_t *params;
	context->encoder = false; // decoder output
	params = &context->video_dec_params;
	params->feed_mode = MC_FEEDING_MODE_FRAME_SIZE;
	params->pix_fmt = MC_PIXEL_FORMAT_NV12;
	params->bitstream_buf_size = (width * height * 3 / 2  + 0x3ff) & ~0x3ff;
	params->bitstream_buf_count = 3;
	params->frame_buf_count = frame_buffer_count;

	switch (codec_type)
	{
	case MEDIA_CODEC_ID_H264:
		context->codec_id = MEDIA_CODEC_ID_H264;
		params->h264_dec_config.bandwidth_Opt = true;
		params->h264_dec_config.reorder_enable = true;
		params->h264_dec_config.skip_mode = 0;
		break;
	case MEDIA_CODEC_ID_H265:
		context->codec_id = MEDIA_CODEC_ID_H265;
		params->h265_dec_config.bandwidth_Opt = true;
		params->h265_dec_config.reorder_enable = true;
		params->h265_dec_config.skip_mode = 0;
		params->h265_dec_config.cra_as_bla = false;
		params->h265_dec_config.dec_temporal_id_mode = 0;
		params->h265_dec_config.target_dec_temporal_id_plus1 = 0;
		break;
	case MEDIA_CODEC_ID_MJPEG:
		context->codec_id = MEDIA_CODEC_ID_MJPEG;
		params->mjpeg_dec_config.rot_degree = MC_CCW_0;
		params->mjpeg_dec_config.mir_direction = MC_DIRECTION_NONE;
		params->mjpeg_dec_config.frame_crop_enable = false;
		break;
	case MEDIA_CODEC_ID_JPEG:
		context->codec_id = MEDIA_CODEC_ID_JPEG;
		params->jpeg_dec_config.frame_crop_enable = 0;
		params->jpeg_dec_config.rot_degree = MC_CCW_0;
		params->jpeg_dec_config.mir_direction = MC_DIRECTION_NONE;
		params->mjpeg_dec_config.frame_crop_enable = false;
		break;
	default:
		printf("Not Support decoding type: %d!\n",
				codec_type);
		return -1;
	}

	return 0;
}