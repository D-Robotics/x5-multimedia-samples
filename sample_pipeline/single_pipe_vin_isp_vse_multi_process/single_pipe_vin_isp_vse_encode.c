/***************************************************************************
 *                      COPYRIGHT NOTICE
 *             Copyright(C) 2025, D-Robotics Co., Ltd.
 *                     All rights reserved.
 ***************************************************************************/

#include <stdio.h>
#include <getopt.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <signal.h>

#include "common_utils.h"
#include "hb_media_codec.h"
#include "hb_media_error.h"
#include "ping_pang_file_saver.h"
#include "unix_socket.h"

static int running = 0;
static media_codec_context_t media_context;

static int32_t get_rc_params(media_codec_context_t *context, mc_rate_control_params_t *rc_params)
{
	int32_t ret = 0;

	ret = hb_mm_mc_get_rate_control_config(context, rc_params);
	if (ret) {
		printf("Failed to get rc params ret=0x%x\n", ret);
		return ret;
	}

	switch (rc_params->mode) {
	case MC_AV_RC_MODE_H264CBR:
		rc_params->h264_cbr_params.intra_period = 30;
		rc_params->h264_cbr_params.intra_qp = 30;
		rc_params->h264_cbr_params.bit_rate = 5000;
		rc_params->h264_cbr_params.frame_rate = 30;
		rc_params->h264_cbr_params.initial_rc_qp = 20;
		rc_params->h264_cbr_params.vbv_buffer_size = 20;
		rc_params->h264_cbr_params.mb_level_rc_enalbe = 1;
		rc_params->h264_cbr_params.min_qp_I = 8;
		rc_params->h264_cbr_params.max_qp_I = 50;
		rc_params->h264_cbr_params.min_qp_P = 8;
		rc_params->h264_cbr_params.max_qp_P = 50;
		rc_params->h264_cbr_params.min_qp_B = 8;
		rc_params->h264_cbr_params.max_qp_B = 50;
		rc_params->h264_cbr_params.hvs_qp_enable = 1;
		rc_params->h264_cbr_params.hvs_qp_scale = 2;
		rc_params->h264_cbr_params.max_delta_qp = 10;
		rc_params->h264_cbr_params.qp_map_enable = 0;
		break;
	case MC_AV_RC_MODE_H264VBR:
		rc_params->h264_vbr_params.intra_qp = 20;
		rc_params->h264_vbr_params.intra_period = 30;
		rc_params->h264_vbr_params.intra_qp = 35;
		break;
	case MC_AV_RC_MODE_H264AVBR:
		rc_params->h264_avbr_params.intra_period = 15;
		rc_params->h264_avbr_params.intra_qp = 25;
		rc_params->h264_avbr_params.bit_rate = 2000;
		rc_params->h264_avbr_params.vbv_buffer_size = 3000;
		rc_params->h264_avbr_params.min_qp_I = 15;
		rc_params->h264_avbr_params.max_qp_I = 50;
		rc_params->h264_avbr_params.min_qp_P = 15;
		rc_params->h264_avbr_params.max_qp_P = 45;
		rc_params->h264_avbr_params.min_qp_B = 15;
		rc_params->h264_avbr_params.max_qp_B = 48;
		rc_params->h264_avbr_params.hvs_qp_enable = 0;
		rc_params->h264_avbr_params.hvs_qp_scale = 2;
		rc_params->h264_avbr_params.max_delta_qp = 5;
		rc_params->h264_avbr_params.qp_map_enable = 0;
		break;
	case MC_AV_RC_MODE_H264FIXQP:
		rc_params->h264_fixqp_params.force_qp_I = 23;
		rc_params->h264_fixqp_params.force_qp_P = 23;
		rc_params->h264_fixqp_params.force_qp_B = 23;
		rc_params->h264_fixqp_params.intra_period = 23;
		break;
	case MC_AV_RC_MODE_H264QPMAP:
		break;
	case MC_AV_RC_MODE_H265CBR:
		rc_params->h265_cbr_params.intra_period = 20;
		rc_params->h265_cbr_params.intra_qp = 30;
		rc_params->h265_cbr_params.bit_rate = 5000;
		rc_params->h265_cbr_params.frame_rate = 30;
		if (context->video_enc_params.width >= 480 ||
			context->video_enc_params.height >= 480) {
			rc_params->h265_cbr_params.initial_rc_qp = 30;
			rc_params->h265_cbr_params.vbv_buffer_size = 3000;
			rc_params->h265_cbr_params.ctu_level_rc_enalbe = 1;
		} else {
			rc_params->h265_cbr_params.initial_rc_qp = 20;
			rc_params->h265_cbr_params.vbv_buffer_size = 20;
			rc_params->h265_cbr_params.ctu_level_rc_enalbe = 1;
		}
		rc_params->h265_cbr_params.min_qp_I = 8;
		rc_params->h265_cbr_params.max_qp_I = 50;
		rc_params->h265_cbr_params.min_qp_P = 8;
		rc_params->h265_cbr_params.max_qp_P = 50;
		rc_params->h265_cbr_params.min_qp_B = 8;
		rc_params->h265_cbr_params.max_qp_B = 50;
		rc_params->h265_cbr_params.hvs_qp_enable = 1;
		rc_params->h265_cbr_params.hvs_qp_scale = 2;
		rc_params->h265_cbr_params.max_delta_qp = 10;
		rc_params->h265_cbr_params.qp_map_enable = 0;
		break;
	case MC_AV_RC_MODE_H265VBR:
		rc_params->h265_vbr_params.intra_qp = 20;
		rc_params->h265_vbr_params.intra_period = 30;
		rc_params->h265_vbr_params.intra_qp = 35;
		break;
	case MC_AV_RC_MODE_H265AVBR:
		rc_params->h265_avbr_params.intra_period = 15;
		rc_params->h265_avbr_params.intra_qp = 25;
		rc_params->h265_avbr_params.bit_rate = 2000;
		rc_params->h265_avbr_params.vbv_buffer_size = 3000;
		rc_params->h265_avbr_params.min_qp_I = 15;
		rc_params->h265_avbr_params.max_qp_I = 50;
		rc_params->h265_avbr_params.min_qp_P = 15;
		rc_params->h265_avbr_params.max_qp_P = 45;
		rc_params->h265_avbr_params.min_qp_B = 15;
		rc_params->h265_avbr_params.max_qp_B = 48;
		rc_params->h265_avbr_params.hvs_qp_enable = 0;
		rc_params->h265_avbr_params.hvs_qp_scale = 2;
		rc_params->h265_avbr_params.max_delta_qp = 5;
		rc_params->h265_avbr_params.qp_map_enable = 0;
		break;
	case MC_AV_RC_MODE_H265FIXQP:
		rc_params->h265_fixqp_params.force_qp_I = 23;
		rc_params->h265_fixqp_params.force_qp_P = 23;
		rc_params->h265_fixqp_params.force_qp_B = 23;
		rc_params->h265_fixqp_params.intra_period = 23;
		break;
	case MC_AV_RC_MODE_H265QPMAP:
		break;
	default:
		ret = HB_MEDIA_ERR_INVALID_PARAMS;
		break;
	}

	return ret;
}


int32_t vp_encode_config_param(media_codec_context_t *context, media_codec_id_t codec_type,
			       int32_t width, int32_t height, int32_t frame_rate, uint32_t bit_rate)
{
	mc_video_codec_enc_params_t *params;

	memset(context, 0x00, sizeof(media_codec_context_t));
	context->encoder = 1;
	params = &context->video_enc_params;
	params->width = width;
	params->height = height;
	params->pix_fmt = MC_PIXEL_FORMAT_NV12;
	params->bitstream_buf_size = (width * height * 3 / 2  + 0x3ff) & ~0x3ff;
	params->frame_buf_count = 5;
	params->external_frame_buf = 1;
	params->bitstream_buf_count = 8;
	params->gop_params.gop_preset_idx = 1;
	params->rot_degree = MC_CCW_0;
	params->mir_direction = MC_DIRECTION_NONE;
	params->frame_cropping_flag = 0;
	params->enable_user_pts = 1;
	params->gop_params.decoding_refresh_type = 2;

	switch (codec_type) {
	case MEDIA_CODEC_ID_H264:
		context->codec_id = MEDIA_CODEC_ID_H264;
		params->rc_params.mode = MC_AV_RC_MODE_H264CBR;
		get_rc_params(context, &params->rc_params);
		params->rc_params.h264_cbr_params.frame_rate = frame_rate;
		params->rc_params.h264_cbr_params.bit_rate = bit_rate;
		break;
	case MEDIA_CODEC_ID_H265:
		context->codec_id = MEDIA_CODEC_ID_H265;
		params->rc_params.mode = MC_AV_RC_MODE_H265CBR;
		get_rc_params(context, &params->rc_params);
		params->rc_params.h265_cbr_params.frame_rate = frame_rate;
		params->rc_params.h265_cbr_params.bit_rate = bit_rate;
		break;
	case MEDIA_CODEC_ID_MJPEG:
		context->codec_id = MEDIA_CODEC_ID_MJPEG;
		params->rc_params.mode = MC_AV_RC_MODE_MJPEGFIXQP;
		get_rc_params(context, &params->rc_params);
		params->mjpeg_enc_config.restart_interval = width / 16;
		break;
	case MEDIA_CODEC_ID_JPEG:
		context->codec_id = MEDIA_CODEC_ID_JPEG;
		params->jpeg_enc_config.quality_factor = 50;
		params->mjpeg_enc_config.restart_interval = width / 16;
		break;
	default:
		printf("Not Support encoding type: %d!\n", codec_type);
		return -1;
	}

	return 0;
}

int encode_init(int width, int height, int fps)
{
	int ret = 0;
	mc_av_codec_startup_params_t startup_params = {0};

	ret = vp_encode_config_param(&media_context, MEDIA_CODEC_ID_H264, width, height, fps, 8192);
	ERR_CON_EQ(ret, 0);
	ret = hb_mm_mc_initialize(&media_context);
	ERR_CON_EQ(ret, 0);
	ret = hb_mm_mc_configure(&media_context);
	ERR_CON_EQ(ret, 0);
	ret = hb_mm_mc_start(&media_context, &startup_params);
	printf("%s idx: %d, init successful (res: %dx%d, fps: %d)\n",
		media_context.encoder ? "Encode" : "Decode",
		media_context.instance_index, width, height, fps);

	uint8_t uuid[] = "dc45e9bd-e6d948b7-962cd820-d923eeef+SEI_D-Robotics";
	uint32_t length = sizeof(uuid) / sizeof(uuid[0]);
	ret = hb_mm_mc_insert_user_data(&media_context, uuid, length);
	if (ret != 0) {
		printf("#### insert user data failed. ret(%d) ####\n", ret);
		return -1;
	}

	return 0;
}

int encode_deinit(void)
{
	int ret = 0;

	ret = hb_mm_mc_pause(&media_context);
	ERR_CON_EQ(ret, 0);
	ret = hb_mm_mc_release(&media_context);
	ERR_CON_EQ(ret, 0);

	return 0;
}

void encode_process_main()
{
	int sock_fd = -1;
	frame_meta_t meta = {0};
	int ret = 0;
	ping_pang_file_saver_t *ping_pang_file_saver = NULL;

	hb_mem_module_open();

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, SOCKET_PATH_VSE_ENCODE, sizeof(addr.sun_path)-1);

	sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock_fd < 0) {
		perror("Encode socket create failed");
		exit(-1);
	}

	int retry = 10;
	while (retry-- > 0) {
		if (connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
			break;
		}
		usleep(100000);
	}
	if (retry < 0) {
		perror("Encode connect to VSE failed, please start VSE process first");
		close(sock_fd);
		exit(-1);
	}
	printf("Encode process connected to VSE\n");

	ret = recv_frame_pkg(sock_fd, &meta);
	if (ret != 0) {
		printf("Encode recv first frame failed\n");
		close(sock_fd);
		exit(-1);
	}

	ret = encode_init(meta.img_buf.width, meta.img_buf.height, 30);
	if (ret != 0) {
		printf("Encode init failed\n");
		close(sock_fd);
		exit(-1);
	}

	ret = send_release_pkg(sock_fd, meta.frame_id);
	if (ret != 0) {
		printf("Encode send first release failed\n");
	}

	ping_pang_file_saver = ping_pang_file_saver_create("single_pipe_vin_isp_vse_vpu.h264", 30 * 60);
	if (ping_pang_file_saver == NULL) {
		printf("Create ping-pang file saver failed\n");
		encode_deinit();
		close(sock_fd);
		exit(-1);
	}

	hb_mem_graphic_buf_t send_buf = {0};
	hb_mem_import_graph_buf(&meta.img_buf, &send_buf);

	running = 1;
	while (running) {
		media_codec_buffer_t input_buffer = {0};
		media_codec_buffer_t output_buffer = {0};
		media_codec_output_buffer_info_t info = {0};

		ret = recv_frame_pkg(sock_fd, &meta);
		if (ret != 0) {
			printf("Encode recv frame failed\n");
			usleep(10000);
			continue;
		}

		printf("Encode process get frame: id=%d, resolution=%dx%d, Y_plane=%lu bytes, UV_plane=%lu bytes\n",
			meta.frame_id, meta.img_buf.width, meta.img_buf.height, meta.img_buf.size[0], meta.img_buf.size[1]);

		ret = hb_mm_mc_dequeue_input_buffer(&media_context, &input_buffer, 2000);
		if (ret != 0) {
			printf("hb_mm_mc_dequeue_input_buffer failed\n");
			send_release_pkg(sock_fd, meta.frame_id);
			continue;
		}

		input_buffer.type = MC_VIDEO_FRAME_BUFFER;
		input_buffer.vframe_buf.width = meta.img_buf.width;
		input_buffer.vframe_buf.height = meta.img_buf.height;
		input_buffer.vframe_buf.pix_fmt = MC_PIXEL_FORMAT_NV12;
		input_buffer.vframe_buf.size = meta.img_buf.width * meta.img_buf.height * 3 / 2;

		input_buffer.vframe_buf.vir_ptr[0] = send_buf.virt_addr[0];
		input_buffer.vframe_buf.vir_ptr[1] = send_buf.virt_addr[1];
		input_buffer.vframe_buf.phy_ptr[0] = send_buf.phys_addr[0];
		input_buffer.vframe_buf.phy_ptr[1] = send_buf.phys_addr[1];

		input_buffer.user_ptr = &send_buf;

		ret = hb_mem_flush_buf_with_vaddr((uint64_t)send_buf.virt_addr[0], send_buf.size[0]);
		if (ret < 0)
			printf("cache flush failed. y_data(%p), y_size(%lu)\n", send_buf.virt_addr[0], send_buf.size[0]);

		ret = hb_mem_flush_buf_with_vaddr((uint64_t)send_buf.virt_addr[0], send_buf.size[1]);
		if (ret < 0)
			printf("cache flush failed. uv_data(%p), uv_size(%lu)\n", send_buf.virt_addr[1], send_buf.size[1]);

		ret = hb_mm_mc_queue_input_buffer(&media_context, &input_buffer, 2000);
		if (ret != 0) {
			printf("hb_mm_mc_queue_input_buffer failed\n");
			send_release_pkg(sock_fd, meta.frame_id);
			continue;
		}

		ret = hb_mm_mc_dequeue_output_buffer(&media_context, &output_buffer, &info, 2000);
		if (ret != 0) {
			printf("hb_mm_mc_dequeue_output_buffer failed\n");
			send_release_pkg(sock_fd, meta.frame_id);
			continue;
		}

		if (ping_pang_file_saver != NULL) {
			ret = ping_pang_file_saver_write(ping_pang_file_saver,
				output_buffer.vstream_buf.size, output_buffer.vstream_buf.vir_ptr);
			if (ret != 0) {
				printf("ping pang file saver failed\n");
			}
		}

		ret = hb_mm_mc_queue_output_buffer(&media_context, &output_buffer, 2000);
		if (ret != 0) {
			printf("hb_mm_mc_queue_output_buffer failed\n");
			send_release_pkg(sock_fd, meta.frame_id);
			continue;
		}

		ret = send_release_pkg(sock_fd, meta.frame_id);
		if (ret != 0) {
			printf("Encode send release failed\n");
		}
	}

	ping_pang_file_saver_destroy(ping_pang_file_saver);
	encode_deinit();
	close(sock_fd);
	unlink(SOCKET_PATH_VSE_ENCODE);
	hb_mem_module_close();
	exit(0);
}

void encode_signal_handle(int signo)
{
	printf("Encode process received signal %d, exiting...\n", signo);
	running = 0;

	unlink(SOCKET_PATH_VSE_HDMI);

	exit(0);
}

int main()
{
	signal(SIGINT, encode_signal_handle);
	signal(SIGTERM, encode_signal_handle);

	encode_process_main();
	return 0;
}