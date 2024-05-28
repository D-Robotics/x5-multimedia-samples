#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#include "communicate/sdk_common_cmd.h"
#include "communicate/sdk_common_struct.h"
#include "communicate/sdk_communicate.h"

#include "utils/utils_log.h"
#include "utils/cqueue.h"
#include "utils/common_utils.h"
#include "utils/stream_define.h"
#include "utils/stream_manager.h"
#include "utils/mthread.h"
#include "utils/mqueue.h"

#include "bpu_wrap.h"
#include "vp_wrap.h"
#include "vp_codec.h"
#include "vp_sensors.h"

#include "solution_handle.h"
#include "solution_config.h"

#include "vpp_preparam.h"
#include "vpp_camera_impl.h"

#define VPP_CAM_MAX_CHANNELS 32

typedef struct
{
	int pipline_id;
	vp_vflow_contex_t vp_vflow_contex;
	media_codec_context_t m_encode_context;

	bpu_handle_t	m_bpu_handle;

	shm_stream_t 	*venc_shm; /* H264 H265 码流，最大可能是32路 */
	tsThread 		m_venc_thread; /* 图像编码、输出给vo、算法图像前处理 */
	tsThread		m_osd_thread;
	tsThread		m_bpu_thread;
} vpp_camera_t;

static vpp_camera_t g_vpp_camera[VPP_CAM_MAX_CHANNELS];

static void vpp_camera_push_stream(vpp_camera_t *vpp_camera, ImageFrame *stream)
{
	int32_t frame_rate = 0;
	int32_t venc_ist_id = 0;
	media_codec_id_t codec_type;
	media_codec_context_t *codec_context = &vpp_camera->m_encode_context;

	media_codec_output_buffer_info_t *buffer_info = NULL;
	media_codec_buffer_t *buffer = NULL;

	if(codec_context == NULL || stream == NULL) {
		SC_LOGE("Param is NULL");
		return;
	}

	venc_ist_id = codec_context->instance_index;
	codec_type = codec_context->codec_id;
	frame_rate = vpp_camera->m_encode_context.video_enc_params.rc_params.h264_cbr_params.frame_rate;

	buffer = (media_codec_buffer_t *)(stream->frame_buffer);
	buffer_info = (media_codec_output_buffer_info_t *)(stream->buffer_info);

	if(vpp_camera->venc_shm == NULL) {
		char shm_id[32] = {0}, shm_name[32] = {0};
		sprintf(shm_id, "cam_id_%s_chn%d", codec_type == MEDIA_CODEC_ID_H264 ? "h264" :
				(codec_type == MEDIA_CODEC_ID_H265 ? "h265" :
				 (codec_type == MEDIA_CODEC_ID_JPEG) ? "jpeg" : "other"), venc_ist_id);
		sprintf(shm_name, "name_%s_chn%d", codec_type == MEDIA_CODEC_ID_H264 ? "h264" :
				(codec_type == MEDIA_CODEC_ID_H265 ? "h265" :
				 (codec_type == MEDIA_CODEC_ID_JPEG) ? "jpeg" : "other"), venc_ist_id);
		SC_LOGI("venc_ist_id:%d, codec_type: %d, shm_id: %s, shm_name: %s", venc_ist_id, codec_type, shm_id, shm_name);
		vpp_camera->venc_shm = shm_stream_create(shm_id, shm_name,
				STREAM_MAX_USER, frame_rate,
				buffer_info->video_stream_info.frame_size,
				SHM_STREAM_WRITE, SHM_STREAM_MALLOC);
		SC_LOGI("shm_stream_create: venc_shm(shm_stream_t): %p, shm_id(id): %s, shm_name(name): %s,"
				" users: %d, frameRate(infos): %d frame_size(size): %d",
			vpp_camera->venc_shm, shm_id, shm_name, STREAM_MAX_USER,
			frame_rate, buffer_info->video_stream_info.frame_size);
		SC_LOGD("param->stream_buf_size:%d", vpp_camera->m_encode_context.video_enc_params.bitstream_buf_size);
	}

	frame_info info;
	info.type		= codec_type;
	info.key		= venc_ist_id;
	info.seq		= buffer->vstream_buf.src_idx;
	info.pts		= buffer->vstream_buf.pts;
	info.length		= buffer->vstream_buf.size;
	info.t_time		= (unsigned int)time(0);
	info.framerate	= frame_rate;
	info.width		= vpp_camera->m_encode_context.video_enc_params.width;
	info.height		= vpp_camera->m_encode_context.video_enc_params.height;

	shm_stream_put(vpp_camera->venc_shm, info, (unsigned char*)buffer->vstream_buf.vir_ptr, buffer->vstream_buf.size);
}

/******************************************************************************
 * funciton : get stream from each channels
 ******************************************************************************/
static void* venc_get_stream_proc(void *ptr)
{
	tsThread *privThread = (tsThread*)ptr;
	int32_t ret = 0;
	ImageFrame vse_frame = {0};
	ImageFrame encode_frame = {0};
	ImageFrame encode_stream = {0};
	hbn_vnode_image_t *hbn_vnode_image = NULL;

	vpp_camera_t *vpp_camera = (vpp_camera_t *)privThread->pvThreadData;

	if (vp_allocate_image_frame(&vse_frame) == NULL) {
		SC_LOGE("vp_allocate_image_frame for vse_frame failed, so exit program.");
		exit(-1);
	}
	if (vp_allocate_image_frame(&encode_frame) == NULL) {
		SC_LOGE("vp_allocate_image_frame for encode_frame failed, so exit program.");
		exit(-1);
	}
	if (vp_allocate_image_frame(&encode_stream) == NULL) {
		SC_LOGE("vp_allocate_image_frame for encode_stream failed, so exit program.");
		exit(-1);
	}

	hbn_vnode_image = (hbn_vnode_image_t *)vse_frame.hbn_vnode_image;

	mThreadSetName(privThread, __func__);
#if 0
	char enc_file_name [100];
	FILE *enc_data_file = NULL;
#endif
	while (privThread->eState == E_THREAD_RUNNING)
	{
		ret = vp_vse_get_frame(&vpp_camera->vp_vflow_contex, 0, &vse_frame);
		if (ret != 0) {
			SC_LOGE("vp_vse_get_frame failed.");
			break;
		}

		// vp_vin_print_hbn_vnode_image_t(hbn_vnode_image);

		// 送进编码器
		encode_frame.data[0] = hbn_vnode_image->buffer.virt_addr[0];
		if (hbn_vnode_image->buffer.plane_cnt == 1) {
			encode_frame.data_size[0] = hbn_vnode_image->buffer.size[0];
		} else if (hbn_vnode_image->buffer.plane_cnt == 2) {
			encode_frame.data_size[0] = hbn_vnode_image->buffer.size[0] + hbn_vnode_image->buffer.size[1];
		}
		encode_frame.image_timestamp = vse_frame.hbn_vnode_image->info.timestamps / 1000;
		ret = vp_codec_set_input(&vpp_camera->m_encode_context, &encode_frame, 0);
		if(ret != 0){
			SC_LOGE("vp_codec_set_input failed.");
			break;
		}
		// 从编码器获取码流
		ret = vp_codec_get_output(&vpp_camera->m_encode_context, &encode_stream, 2000);
		if(ret != 0){
			SC_LOGE("vp_codec_get_output failed.");
			break;
		}


		//for debug 
		{
			#if 0
			if(enc_data_file == NULL){
				sprintf(enc_file_name, "/tmp/ch_%d_box_enc_output_%dx%d_nv12_.h265",
					vpp_camera->pipline_id, vse_frame.width, vse_frame.height);

				enc_data_file = fopen(enc_file_name, "wb");
				if(enc_data_file == NULL){
					SC_LOGE("open file %s failed.", (char *)enc_file_name);
				}
			}
			if(enc_data_file != NULL){
				size_t elementsWritten = fwrite((unsigned char*)encode_stream.frame_buffer->vstream_buf.vir_ptr,
					1, encode_stream.frame_buffer->vstream_buf.size, enc_data_file);
				if (elementsWritten != encode_stream.frame_buffer->vstream_buf.size) {
					SC_LOGE("write file %s failed, size %d, return %d.", 
						(char *)enc_file_name, encode_stream.frame_buffer->vstream_buf.size, elementsWritten);
				}
			}
			#endif
		}

		// rtsp 推流
		vpp_camera_push_stream(vpp_camera, &encode_stream);

		ret = vp_codec_release_output(&vpp_camera->m_encode_context, &encode_stream);
		if (ret != 0) {
			SC_LOGE("vp_codec_release_output failed.");
			break;
		}
		ret = vp_vse_release_frame(&vpp_camera->vp_vflow_contex, 0, &vse_frame);
		if (ret != 0) {
			SC_LOGE("vp_vse_release_frame failed.");
			break;
		}
	}

	vp_free_image_frame(&vse_frame);
	vp_free_image_frame(&encode_frame);
	vp_free_image_frame(&encode_stream);

	mThreadFinish(privThread);
	return NULL;
}

// 从vse的chn1获取yuv数据送给bpu进行算法运行
static void *send_yuv_to_bpu(void *ptr) {
	tsThread *privThread = (tsThread*)ptr;
	int ret = 0;
	ImageFrame vse_frame = {0};
	hbn_vnode_image_t *hbn_vnode_image = NULL;
	bpu_buffer_info_t bpu_input_buffer;

	vpp_camera_t *vpp_camera = (vpp_camera_t *)privThread->pvThreadData;

	if (vp_allocate_image_frame(&vse_frame) == NULL) {
		SC_LOGE("vp_allocate_image_frame for vse_frame failed, so exit program.");
		exit(-1);
	};

	mThreadSetName(privThread, __func__);

	while(privThread->eState == E_THREAD_RUNNING) {
		ret = vp_vse_get_frame(&vpp_camera->vp_vflow_contex, 1, &vse_frame);
		if (ret != 0) {
			SC_LOGE("vp_vse_get_frame failed");
			break;
		}

		hbn_vnode_image = (hbn_vnode_image_t *)vse_frame.hbn_vnode_image;
		// vp_vin_print_hbn_vnode_image_t(hbn_vnode_image);

		// 把yuv数据送进bpu进行算法运算
		memset(&bpu_input_buffer, 0, sizeof(bpu_buffer_info_t));
		vpp_graphic_buf_to_bpu_buffer_info(hbn_vnode_image, &bpu_input_buffer);
		// print_bpu_buffer_info(&bpu_input_buffer);

		bpu_wrap_send_frame(&vpp_camera->m_bpu_handle, &bpu_input_buffer);
		ret = vp_vse_release_frame(&vpp_camera->vp_vflow_contex, 1, &vse_frame);
		if (ret != 0) {
			SC_LOGE("vp_vse_release_frame failed");
			break;
		}
	}

	vp_free_image_frame(&vse_frame);

	mThreadFinish(privThread);
	return NULL;
}

int32_t vpp_camera_init_param(void)
{
	int32_t i = 0, ret = 0;
	char sensor_name[32] = {0};

	camera_config_t *camera_config = NULL;
	isp_ichn_attr_t *isp_ichn_attr = NULL;
	vse_config_t *vse_config = NULL;
	int32_t input_width = 0, input_height = 0;
	int32_t model_width = 0, model_height = 0;

	memset(&g_vpp_camera, 0, sizeof(g_vpp_camera));

	for (i = 0; i < VPP_CAM_MAX_CHANNELS; i++) {
		g_vpp_camera[i].m_encode_context.codec_id = MEDIA_CODEC_ID_NONE;
	}

	// 根据camera solution的配置设置vin、vse、venc、bpu模块的使能和参数
	for (i = 0; i < g_solution_config.cam_solution.pipeline_count; i++) {
		// 1. 配置 vin
		memset(sensor_name, 0, sizeof(sensor_name));
		strcpy(sensor_name, g_solution_config.cam_solution.cam_vpp[i].sensor);

		// 提取 sensor 名称
		char sensor_name_only[256]; // 适当地调整数组大小以适应 sensor 名称的最大长度
		// 关键： 设置vin的mipi_rx，使sensor能够使用正确的 mipi-rx
		// 兼容相同型号的sensor同时接入系统
		// 在vp_vin.c 的 vp_vin_init 中将 mipi_csi_rx_index 号码赋值给 mipi_rx
		sscanf(sensor_name, "CSI_%d-%s", &g_vpp_camera[i].vp_vflow_contex.mipi_csi_rx_index, sensor_name_only);

		SC_LOGI("Enable %s camera sensor", sensor_name_only);
		g_vpp_camera[i].vp_vflow_contex.sensor_config = vp_get_sensor_config_by_name(sensor_name_only);
		if (g_vpp_camera[i].vp_vflow_contex.sensor_config == NULL) {
			SC_LOGE("sensor name not found(%s)", sensor_name_only);
			return -1;
		}

		// 2. 配置算法模型
		if (strlen(g_solution_config.cam_solution.cam_vpp[i].model) > 1
			&& strcmp(g_solution_config.cam_solution.cam_vpp[i].model, "null") != 0) {
			g_vpp_camera[i].m_bpu_handle.m_vpp_id = i;
			strncpy(g_vpp_camera[i].m_bpu_handle.m_model_name,
				g_solution_config.cam_solution.cam_vpp[i].model,
				sizeof(g_vpp_camera[i].m_bpu_handle.m_model_name) - 1);
			g_vpp_camera[i].m_bpu_handle.m_model_name[sizeof(g_vpp_camera[i].m_bpu_handle.m_model_name) - 1] = '\0';
		}

		// 3. 配置 vse
		vse_config = &g_vpp_camera[i].vp_vflow_contex.vse_config;
		isp_ichn_attr = g_vpp_camera[i].vp_vflow_contex.sensor_config->isp_ichn_attr;

		input_width = isp_ichn_attr->width;
		input_height = isp_ichn_attr->height;
		SC_LOGD("VSE %d: input_width: %d input_height: %d",
			i, input_width, input_height);

		vse_config->vse_ichn_attr.width = input_width;
		vse_config->vse_ichn_attr.height = input_height;
		vse_config->vse_ichn_attr.fmt = FRM_FMT_NV12;
		vse_config->vse_ichn_attr.bit_width = 8;

		// 第一个通道给编码器使用
		// 设置VSE通道0输出属性，ROI为原图大小，保持原始输入大小
		vse_config->vse_ochn_attr[0].chn_en = CAM_TRUE;
		vse_config->vse_ochn_attr[0].roi.x = 0;
		vse_config->vse_ochn_attr[0].roi.y = 0;
		vse_config->vse_ochn_attr[0].roi.w = input_width;
		vse_config->vse_ochn_attr[0].roi.h = input_height;
		vse_config->vse_ochn_attr[0].target_w = input_width;
		vse_config->vse_ochn_attr[0].target_h = input_height;
		vse_config->vse_ochn_attr[0].fmt = FRM_FMT_NV12;
		vse_config->vse_ochn_attr[0].bit_width = 8;

		// 第二个通道的数据给BPU使用
		if (strlen(g_vpp_camera[i].m_bpu_handle.m_model_name) > 1
			&& strcmp(g_vpp_camera[i].m_bpu_handle.m_model_name, "null") != 0) {
			ret = bpu_wrap_get_model_hw(g_vpp_camera[i].m_bpu_handle.m_model_name, &model_width, &model_height);
			vse_config->vse_ochn_attr[1].chn_en = CAM_TRUE;
			vse_config->vse_ochn_attr[1].roi.x = 0;
			vse_config->vse_ochn_attr[1].roi.y = 0;
			vse_config->vse_ochn_attr[1].roi.w = input_width;
			vse_config->vse_ochn_attr[1].roi.h = input_height;
			vse_config->vse_ochn_attr[1].target_w = model_width;
			vse_config->vse_ochn_attr[1].target_h = model_height;
			vse_config->vse_ochn_attr[1].fmt = FRM_FMT_NV12;
			vse_config->vse_ochn_attr[1].bit_width = 8;
		}

		// 第三个通道的数据给显示器使用，默认 1080P
		// 需要根据输入分辨率来设置不同的通道，待实现

		// 配置编码通道
		camera_config = g_vpp_camera[i].vp_vflow_contex.sensor_config->camera_config;
		ret = vp_encode_config_param(&g_vpp_camera[i].m_encode_context,
			VP_GET_MD_CODEC_TYPE(g_solution_config.cam_solution.cam_vpp[i].encode_type),
			input_width, input_height, camera_config->fps,
			g_solution_config.cam_solution.cam_vpp[i].encode_bitrate);
		if (ret != 0)
		{
			SC_LOGE("Encode config param error");
		}
	}

	return ret;
}

int32_t vpp_camera_init(void)
{
	int32_t ret = 0;
	int32_t i = 0;
	vp_vflow_contex_t *vp_vflow_contex = NULL;

	hb_mem_module_open();

	for (i = 0; i < VPP_CAM_MAX_CHANNELS; i++) {
		if (g_vpp_camera[i].vp_vflow_contex.sensor_config == NULL)
			continue;

		vp_vflow_contex = &g_vpp_camera[i].vp_vflow_contex;

		ret = vp_vin_init(vp_vflow_contex);
		ret |= vp_isp_init(vp_vflow_contex);
		ret |= vp_vse_init(vp_vflow_contex);
		ret |= vp_vflow_init(vp_vflow_contex);
		SC_ERR_CON_EQ(ret, 0, "vpp_camera_init");

		ret = vp_codec_init(&g_vpp_camera[i].m_encode_context);
		if (ret != 0)
		{
			SC_LOGE("Encode vp_codec_init error");
			return -1;
		}
		SC_LOGI("Init video encode instance %d successful", g_vpp_camera[i].m_encode_context.instance_index);

		// 初始化算法模块， 从vse的chn1通道get yuv数据
		// 初始化bpu
		if (strlen(g_vpp_camera[i].m_bpu_handle.m_model_name) == 0)
			continue;
		ret = bpu_wrap_model_init(&g_vpp_camera[i].m_bpu_handle, g_vpp_camera[i].m_bpu_handle.m_model_name);
		if (ret != 0) {
			SC_LOGE("bpu_wrap_model_init failed");
			return -1;
		}
		// 注册算法结果回调函数
		bpu_wrap_callback_register(&g_vpp_camera[i].m_bpu_handle,
			bpu_wrap_general_result_handle, &g_vpp_camera[i].m_bpu_handle.m_vpp_id);
	}

	SC_LOGD("successful");
	return 0;
}

int32_t vpp_camera_uninit(void)
{
	int32_t ret = 0, i = 0;
	vp_vflow_contex_t *vp_vflow_contex = NULL;

	for (i = 0; i < VPP_CAM_MAX_CHANNELS; i++) {
		if (g_vpp_camera[i].vp_vflow_contex.sensor_config == NULL)
			continue;

		vp_vflow_contex = &g_vpp_camera[i].vp_vflow_contex;

		ret = vp_codec_deinit(&g_vpp_camera[i].m_encode_context);
		ret |= vp_vflow_deinit(vp_vflow_contex);
		ret |= vp_vin_deinit(vp_vflow_contex);
		ret |= vp_isp_deinit(vp_vflow_contex);
		ret |= vp_vse_deinit(vp_vflow_contex);

		SC_ERR_CON_EQ(ret, 0, "vpp_camera_uninit");

		if (strlen(g_vpp_camera[i].m_bpu_handle.m_model_name) == 0)
			continue;
		ret = bpu_wrap_deinit(&g_vpp_camera[i].m_bpu_handle);
		if (ret != 0) {
			SC_LOGE("bpu_wrap_model_init failed");
			return -1;
		}
	}

	hb_mem_module_close();

	vp_print_debug_infos();
	return ret;
}

int32_t vpp_camera_start(void)
{
	int32_t ret = 0;
	int32_t i = 0;
	vp_vflow_contex_t *vp_vflow_contex = NULL;

	for (i = 0; i < VPP_CAM_MAX_CHANNELS; i++) {
		if (g_vpp_camera[i].vp_vflow_contex.sensor_config == NULL)
			continue;

		vp_vflow_contex = &g_vpp_camera[i].vp_vflow_contex;

		ret = vp_codec_start(&g_vpp_camera[i].m_encode_context);
		if (ret != 0)
		{
			SC_LOGE("Encode vp_codec_start error");
			return -1;
		}
		SC_LOGI("Start video encode instance %d successful", g_vpp_camera[i].m_encode_context.instance_index);

		g_vpp_camera[i].pipline_id = i;
		ret = vp_vin_start(vp_vflow_contex);
		ret |= vp_isp_start(vp_vflow_contex);
		ret |= vp_vse_start(vp_vflow_contex);
		ret |= vp_vflow_start(vp_vflow_contex);
		SC_ERR_CON_EQ(ret, 0, "vpp_camera_start");

		g_vpp_camera[i].m_venc_thread.pvThreadData = (void*)&g_vpp_camera[i];
		mThreadStart(venc_get_stream_proc, &g_vpp_camera[i].m_venc_thread, E_THREAD_JOINABLE);

		if (strlen(g_vpp_camera[i].m_bpu_handle.m_model_name) == 0)
			continue;
		// 设置bpu后处理的原始图像大小为推流图像大小
		bpu_wrap_set_ori_hw(&g_vpp_camera[i].m_bpu_handle,
			g_vpp_camera[i].m_encode_context.video_enc_params.width,
			g_vpp_camera[i].m_encode_context.video_enc_params.height);

		ret = bpu_wrap_start(&g_vpp_camera[i].m_bpu_handle);
		if (ret != 0) {
			SC_LOGE("bpu_wrap_start failed");
			return -1;
		}

		// 启动一个线程从 vse 获取 yuv 数据给 bpu 进行算法运算
		g_vpp_camera[i].m_bpu_thread.pvThreadData = (void*)&g_vpp_camera[i];
		mThreadStart(send_yuv_to_bpu, &g_vpp_camera[i].m_bpu_thread, E_THREAD_JOINABLE);
		SC_LOGI("Start BPU %d process successful, %s", i, g_vpp_camera[i].m_bpu_handle.m_model_name);
	}
	vp_print_debug_infos();
	return ret;
}

int32_t vpp_camera_stop(void)
{
	int32_t ret = 0, i = 0;
	vp_vflow_contex_t *vp_vflow_contex = NULL;

	// 先把所有线程停掉
	for (i = 0; i < VPP_CAM_MAX_CHANNELS; i++) {
		if (g_vpp_camera[i].vp_vflow_contex.sensor_config == NULL)
			continue;

		vp_vflow_contex = &g_vpp_camera[i].vp_vflow_contex;
		mThreadStop(&g_vpp_camera[i].m_venc_thread);

		if (strlen(g_vpp_camera[i].m_bpu_handle.m_model_name) == 0)
			continue;
		mThreadStop(&g_vpp_camera[i].m_bpu_thread);
	}

	for (i = 0; i < VPP_CAM_MAX_CHANNELS; i++) {
		if (g_vpp_camera[i].vp_vflow_contex.sensor_config == NULL)
			continue;

		vp_vflow_contex = &g_vpp_camera[i].vp_vflow_contex;

		ret = vp_codec_stop(&g_vpp_camera[i].m_encode_context);
		ret |= vp_vflow_stop(vp_vflow_contex);
		ret |= vp_vin_stop(vp_vflow_contex);
		ret |= vp_isp_stop(vp_vflow_contex);
		ret |= vp_vse_stop(vp_vflow_contex);
		SC_ERR_CON_EQ(ret, 0, "vpp_camera_stop");

		if (strlen(g_vpp_camera[i].m_bpu_handle.m_model_name) == 0)
			continue;
		ret = bpu_wrap_stop(&g_vpp_camera[i].m_bpu_handle);
		if (ret != 0) {
			SC_LOGE("bpu_wrap_start failed");
			return -1;
		}
	}

	return ret;
}

int32_t vpp_camera_param_set(SOLUTION_PARAM_E type, char* val, uint32_t length)
{
	int32_t ret = 0;
	return ret;
}

int32_t vpp_camera_param_get(SOLUTION_PARAM_E type, char* val, uint32_t* length)
{
	int32_t i= 0, ret = 0;
	mc_video_codec_enc_params_t *enc_params;

	switch(type)
	{
	case SOLUTION_VENC_CHN_PARAM_GET: // 获取某个编码通道的配置
		{
			venc_info_t* param = (venc_info_t*)val;
			param->enable = 0;
			SC_LOGI("param->channel: %d", param->channel);
			for (i = 0; i < VPP_CAM_MAX_CHANNELS; i++) {
				if (g_vpp_camera[i].m_encode_context.codec_id == MEDIA_CODEC_ID_NONE) {
					continue;
				}
				if (g_vpp_camera[i].m_encode_context.instance_index == param->channel) {
					// 填充对外的信息
					enc_params = &g_vpp_camera[i].m_encode_context.video_enc_params;
					param->enable = 1;
					param->width = enc_params->width;
					param->height = enc_params->height;
					param->stream_buf_size = enc_params->bitstream_buf_size;
					SC_LOGD("param->stream_buf_size:%d", param->stream_buf_size);
					if (g_vpp_camera[i].m_encode_context.codec_id == MEDIA_CODEC_ID_H264) {
						param->type = 96;
						param->bitrate = enc_params->rc_params.h264_cbr_params.bit_rate;
						param->framerate = enc_params->rc_params.h264_cbr_params.frame_rate;
					} else if (g_vpp_camera[i].m_encode_context.codec_id == MEDIA_CODEC_ID_H265) {
						param->type = 265;
						param->bitrate = enc_params->rc_params.h265_cbr_params.bit_rate;
						param->framerate = enc_params->rc_params.h265_cbr_params.frame_rate;
					}
				}
			}
			break;
		}
	case SOLUTION_GET_VENC_CHN_STATUS: // 获取哪些编码通道被使能了
		{
			// 32位的整形，每个通道的状态占其中一个bit
			// 注： 64bit的值位与会有异常，待查
			unsigned int *status = (unsigned int *)val;
			*status = 0;
			for (i = 0; i < VPP_CAM_MAX_CHANNELS; i++) {
				if (g_vpp_camera[i].m_encode_context.instance_index != -1) {
					*status |= (1 << g_vpp_camera[i].m_encode_context.instance_index);
				}
			}
			SC_LOGI("venc status: 0x%x", *status);
			break;
		}
	default:
		{
			ret= -1;
			break;
		}
	}
	return ret;
}
