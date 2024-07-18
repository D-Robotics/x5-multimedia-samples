/***************************************************************************
 *                      COPYRIGHT NOTICE
 *             Copyright(C) 2024, D-Robotics Co., Ltd.
 *                     All rights reserved.
 ***************************************************************************/

#include <stdio.h>
#include <getopt.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <string.h>
#include <pthread.h>

#include "mqueue.h"
#include "vp_codec.h"
#include "vp_pipeline.h"
#include "uvc_gadget_wraper.h"
enum pipeline_thread_state_t{
	E_THREAD_STOPPED,
	E_THREAD_RUNNING,
	E_THREAD_STOPPING,
};
typedef struct uvc_gadget_camera_contex_s
{
	struct uvc_context *uvc_contex;
	pipe_contex_t pipe_contex;

	int vse_bind_codec_chn;
	int codec_buffer_count;
	vp_codec_info_t vp_codec_info;
	media_codec_context_t encode_context;

	int sensor_mode;
	tsQueue unused_queue;
	tsQueue inused_queue;
	pthread_t pipeline_thread;
	enum pipeline_thread_state_t pipeline_thread_state;

	vp_csi_config_t csi_config;
	vp_sensor_config_t* sensor_config;

} uvc_gadget_camera_contex_t;

static uvc_gadget_camera_contex_t g_uvc_gadget_camera_contex = {
	.uvc_contex = NULL,
	.codec_buffer_count = 5,
	.vse_bind_codec_chn = 0,
	.pipeline_thread_state = E_THREAD_STOPPED};

static struct option const long_options[] = {
	{"sensor", required_argument, NULL, 's'},
	{"settle", optional_argument, NULL, 't'},
	{"mode", optional_argument, NULL, 'm'},
	{NULL, 0, NULL, 0}};

static void print_help()
{
	printf("Usage: get_isp_data [OPTIONS]\n");
	printf("Options:\n");
	printf("  -s <sensor_index>      Specify sensor index\n");
	printf("  -t <settle_value>      Specify settle time for debug\n");
	printf("  -m <sensor_mode>       Specify sensor mode of camera_config_t\n");
	printf("  -h                     Show this help message\n");
	vp_show_sensors_list(); // Assuming this function displays sensor list
}

void *pipeline_porcess_func(void *data){
	uvc_gadget_camera_contex_t *uvc_gadget_camera_contex = (uvc_gadget_camera_contex_t *)data;
	pipe_contex_t *pipe_context = (pipe_contex_t *)&uvc_gadget_camera_contex->pipe_contex;
	media_codec_context_t *media_context = &uvc_gadget_camera_contex->encode_context;

	int ret = 0;
	hbn_vnode_handle_t vse_node_handle = pipe_context->vse_node_handle;
	hbn_vnode_image_t vse_chn_frame = {0};
	media_codec_buffer_t input_buffer = {0};
	media_codec_output_buffer_info_t info;

	uint8_t uuid[] = "dc45e9bd-e6d948b7-962cd820-d923eeef+SEI_D-Robotics";
	uint32_t length = sizeof(uuid) / sizeof(uuid[0]);
	ret = hb_mm_mc_insert_user_data(media_context, uuid, length);
	if (ret != 0)
	{
		printf("#### insert user data failed. ret(%d) ####\n", ret);
		return NULL;
	}
	printf("pipeline_porcess_func start.\n");
	while (uvc_gadget_camera_contex->pipeline_thread_state == E_THREAD_RUNNING)
	{
		//1. get one frame frome vse
		ret = hbn_vnode_getframe(vse_node_handle, uvc_gadget_camera_contex->vse_bind_codec_chn, 1000, &vse_chn_frame);
		if (ret != 0)
		{
			printf("hbn_vnode_getframe VSE channel %d failed, error code %d\n", 0, ret);
			continue;
		}

		////2. send one frame to codec
		memset(&input_buffer, 0x00, sizeof(media_codec_buffer_t));
		ret = hb_mm_mc_dequeue_input_buffer(media_context, &input_buffer,
											2000);
		if (ret != 0){
			printf("hb_mm_mc_dequeue_input_buffer failed\n");
			continue;
		}
		int frame_width = vse_chn_frame.buffer.width;
		int frame_height = vse_chn_frame.buffer.height;
		memcpy(input_buffer.vframe_buf.vir_ptr[0], vse_chn_frame.buffer.virt_addr[0],
			   frame_width * frame_height * 3 / 2);
		ret = hb_mm_mc_queue_input_buffer(media_context, &input_buffer, 2000);
		if (ret != 0){
			printf("hb_mm_mc_queue_input_buffer failed\n");
			continue;
		}

		//3. get one frame frome codec and send to queue
		media_codec_buffer_t *ouput_buffer_ptr = NULL;
		teQueueStatus status = mQueueDequeueTimed(&uvc_gadget_camera_contex->unused_queue, 2000, (void **)&ouput_buffer_ptr);
		if (status != E_QUEUE_OK){
			printf("get media_codec_buffer_t frome unused queue failed, so continue.\n");
			hbn_vnode_releaseframe(vse_node_handle, uvc_gadget_camera_contex->vse_bind_codec_chn, &vse_chn_frame);
			continue;
		}
		memset(ouput_buffer_ptr, 0x0, sizeof(media_codec_buffer_t));
		memset(&info, 0x0, sizeof(media_codec_output_buffer_info_t));
		ret = hb_mm_mc_dequeue_output_buffer(media_context, ouput_buffer_ptr,
											 &info, 2000);
		if (ret != 0){
			printf("hb_mm_mc_dequeue_output_buffer failed\n");
			continue;
		}
		mQueueEnqueue(&uvc_gadget_camera_contex->inused_queue, (void *)ouput_buffer_ptr);

		//4. release vse frame
		hbn_vnode_releaseframe(vse_node_handle, uvc_gadget_camera_contex->vse_bind_codec_chn, &vse_chn_frame);
	}
	printf("pipeline_porcess_func stop.\n");
	uvc_gadget_camera_contex->pipeline_thread_state = E_THREAD_STOPPED;
	return NULL;
}

static int get_codec_info_from_pipeline(pipe_contex_t *pipe_context, vp_codec_info_t *vp_codec_info){
	vse_ochn_attr_t vse_ochn_attr = {0};
	int ret = hbn_vnode_get_ochn_attr(pipe_context->vse_node_handle,
								g_uvc_gadget_camera_contex.vse_bind_codec_chn,
								&vse_ochn_attr);
	ERR_CON_EQ(ret, 0);
	vp_codec_info->fps = pipe_context->sensor_config->camera_config->fps;
	vp_codec_info->width = vse_ochn_attr.target_w;
	vp_codec_info->height = vse_ochn_attr.target_h;
	vp_codec_info->encode_type = MEDIA_CODEC_ID_H264;
	return 0;
}

static int uvc_gadget_camera_contex_init(){
	int codec_buffer_count = g_uvc_gadget_camera_contex.codec_buffer_count;
	teQueueStatus status = mQueueCreate(&g_uvc_gadget_camera_contex.inused_queue, codec_buffer_count + 1);
	if (status != E_QUEUE_OK){
		printf("mqueue create failed:%d\n", status);
		return -1;
	}
	status = mQueueCreate(&g_uvc_gadget_camera_contex.unused_queue, codec_buffer_count + 1);
	if (status != E_QUEUE_OK){
		printf("mqueue create failed:%d\n", status);
		return -1;
	}
	return 0;
}

static int pipeline_process_start(uvc_gadget_camera_contex_t *uvc_gadget_camera_contex){
	int ret = 0;
	int codec_buffer_count = uvc_gadget_camera_contex->codec_buffer_count;
		// 2. create pipeline:vin->isp->vse
	pipe_contex_t *pipe_contex = &uvc_gadget_camera_contex->pipe_contex;
	memset(pipe_contex, 0, sizeof(pipe_contex_t));

	pipe_contex->sensor_config = uvc_gadget_camera_contex->sensor_config;
	pipe_contex->csi_config = uvc_gadget_camera_contex->csi_config;

	ret = vp_create_and_start_pipeline(pipe_contex,
									   pipe_contex->csi_config.index,
									   uvc_gadget_camera_contex->vse_bind_codec_chn,
									   uvc_gadget_camera_contex->sensor_mode);
	ERR_CON_EQ(ret, 0);

	//3. create h264 codec
	get_codec_info_from_pipeline(pipe_contex, &uvc_gadget_camera_contex->vp_codec_info);
	media_codec_context_t *encode_context = &uvc_gadget_camera_contex->encode_context;
	ret = vp_codec_encoder_create_and_start(encode_context, &uvc_gadget_camera_contex->vp_codec_info);
	if (ret != 0){
		printf("create_encodec failed:%d\n", ret);
		return -1;
	}

	for (size_t i = 0; i < codec_buffer_count; i++){
		media_codec_buffer_t *ouput_buffer_ptr = (media_codec_buffer_t *)malloc(sizeof(media_codec_buffer_t));
		if (ouput_buffer_ptr == NULL){
			printf("malloc failed\n");
			return -1;
		}
		teQueueStatus status = mQueueEnqueue(&uvc_gadget_camera_contex->unused_queue, (void *)ouput_buffer_ptr);
		if (status != E_QUEUE_OK){
			printf("mqueue enqueue failed:%d\n", status);
			return -1;
		}
	}

	uvc_gadget_camera_contex->pipeline_thread_state = E_THREAD_RUNNING;
	ret = pthread_create(&uvc_gadget_camera_contex->pipeline_thread, NULL,
						 pipeline_porcess_func, &g_uvc_gadget_camera_contex);
	if (ret != 0){
		printf("pipeline thread create failed:%d\n", ret);
		return -1;
	}
	return 0;

}

static void pipeline_process_stop(uvc_gadget_camera_contex_t *uvc_gadget_camera_contex){

	//wait thread stop
	uvc_gadget_camera_contex->pipeline_thread_state = E_THREAD_STOPPING;
	while(uvc_gadget_camera_contex->pipeline_thread_state != E_THREAD_STOPPED){
		usleep(1000*100);
		printf("wait pipeline thread stop.\n");
	}

	int ret = 0;
	teQueueStatus status = E_QUEUE_OK;
	while(status == E_QUEUE_OK ){
		media_codec_buffer_t *ouput_buffer_ptr = NULL;
		status = mQueueDequeueTimed(&uvc_gadget_camera_contex->inused_queue, 0, (void **)&ouput_buffer_ptr);
		if (status != E_QUEUE_OK){
			break;
		}
		ret = hb_mm_mc_queue_output_buffer(&uvc_gadget_camera_contex->encode_context, ouput_buffer_ptr, 2000);
		if (ret != 0){
			printf("hb_mm_mc_queue_output_buffer failed when stop.\n");
			exit(-1);
		}
		free(ouput_buffer_ptr);
	}
	status = E_QUEUE_OK;
	while(status == E_QUEUE_OK ){
		media_codec_buffer_t *ouput_buffer_ptr = NULL;
		status = mQueueDequeueTimed(&uvc_gadget_camera_contex->unused_queue, 0, (void **)&ouput_buffer_ptr);
		if (status != E_QUEUE_OK){
			break;
		}
		free(ouput_buffer_ptr);
	}
	pipe_contex_t *pipe_contex = &uvc_gadget_camera_contex->pipe_contex;
	media_codec_context_t *encode_context = &uvc_gadget_camera_contex->encode_context;
	vp_destroy_and_stop_pipeline(pipe_contex);
	vp_codec_encoder_destroy_and_stop(encode_context);
	printf("pipeline thread stoped.\n");
}
int uvc_get_frame_cb_func(struct uvc_context *ctx,
						  void **buf_to, int *buf_len,
						  void **entity, void *userdata){
	uvc_gadget_camera_contex_t *g_uvc_gadget_camera_contex = (uvc_gadget_camera_contex_t *)userdata;
	media_codec_buffer_t *ouput_buffer_ptr = NULL;
	teQueueStatus status = E_QUEUE_ERROR_TIMEOUT;
	while (status != E_QUEUE_OK){
		status = mQueueDequeueTimed(&g_uvc_gadget_camera_contex->inused_queue, 2000, (void **)&ouput_buffer_ptr);
		if (status != E_QUEUE_OK)
		{
			printf("get media_codec_buffer_t frome inused queue failed, so continue.\n");
		}
	}

	*buf_to = ouput_buffer_ptr->vstream_buf.vir_ptr;
	*buf_len = ouput_buffer_ptr->vstream_buf.size;
	*entity = ouput_buffer_ptr;
	return 0;
}
void uvc_release_frame_cb_func(struct uvc_context *ctx,
							   void **entity, void *userdata){
	if (!ctx || !entity || !(*entity))
		return;
	uvc_gadget_camera_contex_t *g_uvc_gadget_camera_contex = (uvc_gadget_camera_contex_t *)userdata;
	media_codec_buffer_t *ouput_buffer_ptr = (media_codec_buffer_t *)*entity;
	int ret = hb_mm_mc_queue_output_buffer(&g_uvc_gadget_camera_contex->encode_context,
										   ouput_buffer_ptr, 2000);
	if (ret != 0)
	{
		printf("hb_mm_mc_queue_output_buffer failed\n");
		exit(-1);
	}
	mQueueEnqueue(&g_uvc_gadget_camera_contex->unused_queue, ouput_buffer_ptr);
	*entity = 0;
}

void uvc_streamon_on_or_off(struct uvc_context *ctx, int is_on, void *userdata){

	if (!ctx || !ctx->udev)
		return;

	struct uvc_device *dev = ctx->udev;
	uvc_gadget_camera_contex_t *uvc_gadget_camera_contex = (uvc_gadget_camera_contex_t *)userdata;

	int width = uvc_gadget_camera_contex->sensor_config->camera_config->width;
	int height = uvc_gadget_camera_contex->sensor_config->camera_config->height;
	if(is_on){
		if((width != dev->width)
			||(height != dev->height)
			||(dev->fcc != V4L2_PIX_FMT_H264)){

			printf("Camera info(%dx%d:h264) does not match the PC client configuration(%ux%u:%s), so exit.\n",
				width, height,
				dev->width, dev->height, fcc_to_string(dev->fcc));
			exit(-1);
		}
		printf("## uvc camera on(%d)## %s(%ux%u)\n", is_on, fcc_to_string(dev->fcc), dev->width, dev->height);
		pipeline_process_start(uvc_gadget_camera_contex);
	}else{
		printf("## uvc camera off(%d)## %s(%ux%u)\n", is_on, fcc_to_string(dev->fcc), dev->width, dev->height);
		pipeline_process_stop(uvc_gadget_camera_contex);
	}
}

int main(int argc, char *argv[])
{
	// 1. get user config param
	int c = 0;
	int ret = 0;
	int index = -1;
	int settle = -1;
	int opt_index = 0;
	int sensor_mode = 0;
	while ((c = getopt_long(argc, argv, "s:t:m:h",
							long_options, &opt_index)) != -1){
		switch (c){
		case 's':
			index = atoi(optarg);
			break;
		case 't':
			settle = atoi(optarg);
			break;
		case 'm':
			sensor_mode = atoi(optarg);
			break;
		case 'h':
		default:
			print_help();
			return 0;
		}
	}
	g_uvc_gadget_camera_contex.sensor_mode = sensor_mode;
	printf("index:%d settle=%d sensor_mode=%d\n", index, settle, sensor_mode);

	if (index < vp_get_sensors_list_number() && index >= 0){
		g_uvc_gadget_camera_contex.sensor_config = vp_sensor_config_list[index];
		printf("Using index:%d  sensor_name:%s  config_file:%s\n",
			   index,
			   vp_sensor_config_list[index]->sensor_name,
			   vp_sensor_config_list[index]->config_file);
		ret = vp_sensor_fixed_mipi_host(g_uvc_gadget_camera_contex.sensor_config,
			&g_uvc_gadget_camera_contex.csi_config);
		if (ret != 0)
		{
			printf("No Camera Sensor found. Please check if the specified "
				   "sensor is connected to the Camera interface.\n");
			return ret;
		}
	}else{
		printf("Unsupport sensor index:%d\n", index);
		print_help();
		return 0;
	}

	hb_mem_module_open();

	//4. create uvc dadget
	uvc_gadget_camera_info_t g_uvc_gadget_camera_info = {
		.frame_width = g_uvc_gadget_camera_contex.sensor_config->camera_config->width,
		.frame_height = g_uvc_gadget_camera_contex.sensor_config->camera_config->height,

		.prepare_frame_cb = {uvc_get_frame_cb_func, &g_uvc_gadget_camera_contex},
		.release_frame_cb = {uvc_release_frame_cb_func, &g_uvc_gadget_camera_contex},
		.stream_on_or_off_cb = {uvc_streamon_on_or_off, &g_uvc_gadget_camera_contex}
	};
	g_uvc_gadget_camera_contex.uvc_contex = uvc_gadget_create_and_start(&g_uvc_gadget_camera_info);
	if (g_uvc_gadget_camera_contex.uvc_contex == NULL){
		printf("uvc gadget camera create failed.\n");
		return -1;
	}

	//5. other init
	ret = uvc_gadget_camera_contex_init();
	if(ret != 0){
		printf("uvc_gadget_camera_contex_init failed.\n");
		return -1;
	}

	printf("'q' for exit\n");
	while (getchar() != 'q');

	uvc_gadget_destroy_and_stop(g_uvc_gadget_camera_contex.uvc_contex);
	hb_mem_module_close();
	return 0;
}
