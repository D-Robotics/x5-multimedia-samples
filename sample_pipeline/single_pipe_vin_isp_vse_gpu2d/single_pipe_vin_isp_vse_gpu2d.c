/***************************************************************************
 *                      COPYRIGHT NOTICE
 *             Copyright(C) 2024, D-Robotics Co., Ltd.
 *                     All rights reserved.
 ***************************************************************************/

#include <stdio.h>
#include <getopt.h>
#include <pthread.h>
#include <stdlib.h>
#include <ctype.h>
#include <signal.h>
#include <unistd.h>

#include "common_utils.h"
#include "channel_param_parser.h"

#define FRAME_QUEUE_SIZE 10

typedef struct {
	hbn_vnode_image_t frame;
	int valid;
} frame_node_t;

typedef struct {
	frame_node_t frames[FRAME_QUEUE_SIZE];
	int head;
	int tail;
	int count;
	pthread_mutex_t mutex;
	pthread_cond_t not_empty;
	pthread_cond_t not_full;
} frame_queue_t;

static frame_queue_t frame_queue;
static int32_t verbose_flag = 0;
static int32_t running = 0;
extern int vin_isp_is_online;
extern int isp_vse_is_online;
static n2d_config_t g_gpu2d_attr = {0};
static pthread_mutex_t g_gpu2d_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_crop_mutex  = PTHREAD_MUTEX_INITIALIZER;

static struct option const long_options[] = {
	{"sensor", required_argument, NULL, 's'},
	{"channel-type", required_argument, NULL, 'c'},
	{"verbose", no_argument, NULL, 'v'},
	{"help", no_argument, NULL, 'h'},
	{NULL, 0, NULL, 0}
};

static int create_and_run_vflow(pipe_contex_t *pipe_contex, int active_mipi_host, uint32_t sensor_mode);

static int is_number(const char *str)
{
	if (!str || *str == '\0')
		return 0;
	while (*str) {
		if (!isdigit((unsigned char)*str))
			return 0;
		str++;
	}
	return 1;
}

static void signal_handle(int signo)
{
	(void)signo;
	running = 0;
}

static void frame_queue_init(frame_queue_t *q)
{
	memset(q, 0, sizeof(frame_queue_t));
	pthread_mutex_init(&q->mutex, NULL);
	pthread_cond_init(&q->not_empty, NULL);
	pthread_cond_init(&q->not_full, NULL);
}

static int frame_queue_pop(frame_queue_t *q, hbn_vnode_image_t *out)
{
	pthread_mutex_lock(&q->mutex);
	while (q->count == 0 && running)
		pthread_cond_wait(&q->not_empty, &q->mutex);

	if (!running) {
		pthread_mutex_unlock(&q->mutex);
		return -1;
	}

	*out = q->frames[q->head].frame;
	q->frames[q->head].valid = 0;
	q->head = (q->head + 1) % FRAME_QUEUE_SIZE;
	q->count--;

	pthread_cond_signal(&q->not_full);
	pthread_mutex_unlock(&q->mutex);
	return 0;
}

static int frame_queue_push(frame_queue_t *q, hbn_vnode_image_t *frame)
{
	pthread_mutex_lock(&q->mutex);

	if (q->count >= FRAME_QUEUE_SIZE) {
		pthread_mutex_unlock(&q->mutex);
		printf("Frame queue full, drop frame! (current count: %d)\n", q->count);
		return -1;
	}

	q->frames[q->tail].frame = *frame;
	q->frames[q->tail].valid = 1;
	q->tail = (q->tail + 1) % FRAME_QUEUE_SIZE;
	q->count++;

	pthread_cond_signal(&q->not_empty);
	pthread_mutex_unlock(&q->mutex);

	return 0;
}

static int create_camera_node(pipe_contex_t *pipe_contex, uint32_t sensor_mode)
{
	if (!pipe_contex || !pipe_contex->sensor_config) {
		fprintf(stderr, "Invalid pipe_contex or sensor_config\n");
		return -1;
	}

	vp_sensor_config_t *sensor_cfg = pipe_contex->sensor_config;
	camera_config_t *cam_cfg = sensor_cfg->camera_config;

	if (!cam_cfg) {
		fprintf(stderr, "camera_config is NULL\n");
		return -1;
	}

	if (sensor_mode >= NORMAL_M && sensor_mode < INVALID_MOD) {
		cam_cfg->sensor_mode = sensor_mode;
		if (sensor_mode == SLAVE_M) {
			sensor_cfg->vin_node_attr->lpwm_attr.enable = 1;
		} else {
			sensor_cfg->vin_node_attr->lpwm_attr.enable = 0;
		}
	}

	int32_t ret = hbn_camera_create(cam_cfg, &pipe_contex->cam_fd);
	ERR_CON_EQ(ret, 0);

	return 0;
}

static int create_vin_node(pipe_contex_t *pipe_contex, int active_mipi_host)
{
	vp_sensor_config_t *sensor_config = NULL;
	vin_node_attr_t *vin_node_attr = NULL;
	vin_ichn_attr_t *vin_ichn_attr = NULL;
	vin_ochn_attr_t *vin_ochn_attr = NULL;
	hbn_vnode_handle_t *vin_node_handle = NULL;
	vin_attr_ex_t vin_attr_ex;
	uint32_t hw_id = 0;
	int32_t ret = 0;
	uint32_t ichn_id = 0;
	uint32_t ochn_id = 0;
	uint64_t vin_attr_ex_mask = 0;

	sensor_config = pipe_contex->sensor_config;
	vin_node_attr = sensor_config->vin_node_attr;
	vin_ichn_attr = sensor_config->vin_ichn_attr;
	vin_ochn_attr = sensor_config->vin_ochn_attr;
	vin_node_attr->cim_attr.mipi_rx = active_mipi_host;
	hw_id = vin_node_attr->cim_attr.mipi_rx;
	vin_node_handle = &pipe_contex->vin_node_handle;

	if (pipe_contex->csi_config.mclk_is_not_configed) {
		// 设备树中没有配置 mclk：使用外部晶振
		printf("csi%d ignore mclk ex attr, because not config mclk.\n",
			pipe_contex->csi_config.index);
	} else {
		vin_attr_ex.vin_attr_ex_mask = sensor_config->vin_attr_ex->vin_attr_ex_mask;
		vin_attr_ex.mclk_ex_attr.mclk_freq = sensor_config->vin_attr_ex->mclk_ex_attr.mclk_freq;
		vin_attr_ex_mask = vin_attr_ex.vin_attr_ex_mask;
	}
	if (vin_isp_is_online) { 	/*vin->isp: online mode*/
		sensor_config->vin_node_attr->cim_attr.cim_isp_flyby = 1;
		sensor_config->vin_ochn_attr->ddr_en = 0;
	} else { 			/* vin->isp: offline mode*/
		sensor_config->vin_node_attr->cim_attr.cim_isp_flyby = 0;
		sensor_config->vin_ochn_attr->ddr_en = 1;
	}

	ret = hbn_vnode_open(HB_VIN, hw_id, AUTO_ALLOC_ID, vin_node_handle);
	ERR_CON_EQ(ret, 0);
	// 设置基本属性
	ret = hbn_vnode_set_attr(*vin_node_handle, vin_node_attr);
	ERR_CON_EQ(ret, 0);
	// 设置输入通道的属性
	ret = hbn_vnode_set_ichn_attr(*vin_node_handle, ichn_id, vin_ichn_attr);
	ERR_CON_EQ(ret, 0);
	// 设置输出通道的属性
	ret = hbn_vnode_set_ochn_attr(*vin_node_handle, ochn_id, vin_ochn_attr);
	ERR_CON_EQ(ret, 0);

	if (vin_attr_ex_mask) {
		for (uint8_t i = 0; i < VIN_ATTR_EX_INVALID; i ++) {
			if ((vin_attr_ex_mask & (1 << i)) == 0)
				continue;
			vin_attr_ex.ex_attr_type = i;
			/*we need to set hbn_vnode_set_attr_ex in a loop*/
			ret = hbn_vnode_set_attr_ex(*vin_node_handle, &vin_attr_ex);
			ERR_CON_EQ(ret, 0);
		}
	}

	if (!vin_isp_is_online) {
		hbn_buf_alloc_attr_t alloc_attr = {0};
		alloc_attr.buffers_num = 3;
		alloc_attr.is_contig = 1;
		alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN  |
				   HB_MEM_USAGE_CPU_WRITE_OFTEN |
				   HB_MEM_USAGE_CACHED;
		ret = hbn_vnode_set_ochn_buf_attr(*vin_node_handle, ochn_id, &alloc_attr);
		ERR_CON_EQ(ret, 0);
	}

	return 0;
}

static int create_isp_node(pipe_contex_t *pipe_contex)
{
	vp_sensor_config_t *sensor_config = NULL;
	isp_attr_t      *isp_attr = NULL;
	isp_ichn_attr_t *isp_ichn_attr = NULL;
	isp_ochn_attr_t *isp_ochn_attr = NULL;
	hbn_vnode_handle_t *isp_node_handle = NULL;
	uint32_t ichn_id = 0;
	uint32_t ochn_id = 0;
	int ret = 0;

	sensor_config = pipe_contex->sensor_config;
	isp_attr = sensor_config->isp_attr;
	isp_ichn_attr = sensor_config->isp_ichn_attr;
	isp_ochn_attr = sensor_config->isp_ochn_attr;
	isp_node_handle = &pipe_contex->isp_node_handle;

	if (vin_isp_is_online) { 	/*vin->isp: online mode*/
		sensor_config->isp_attr->input_mode = PASSTHROUGH_MODE;
	} else { 			/* vin->isp: offline mode*/
		sensor_config->isp_attr->input_mode = DDR_MODE;
	}

	if (isp_vse_is_online) {
		isp_ochn_attr->ddr_en = 0;
	} else {
		isp_ochn_attr->ddr_en = 1;
	}

	ret = hbn_vnode_open(HB_ISP, 0, AUTO_ALLOC_ID, isp_node_handle);
	ERR_CON_EQ(ret, 0);

	ret = hbn_vnode_set_attr(*isp_node_handle, isp_attr);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_ochn_attr(*isp_node_handle, ochn_id, isp_ochn_attr);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_ichn_attr(*isp_node_handle, ichn_id, isp_ichn_attr);
	ERR_CON_EQ(ret, 0);

	if (!isp_vse_is_online) {
		hbn_buf_alloc_attr_t alloc_attr = {0};
		alloc_attr.buffers_num = 3;
		alloc_attr.is_contig = 1;
		alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN  |
				   HB_MEM_USAGE_CPU_WRITE_OFTEN |
				   HB_MEM_USAGE_CACHED;
		ret = hbn_vnode_set_ochn_buf_attr(*isp_node_handle, ochn_id, &alloc_attr);
		ERR_CON_EQ(ret, 0);
	}

	return 0;
}

static int create_vse_node(pipe_contex_t *pipe_contex)
{
	int ret = 0;
	hbn_vnode_handle_t *vse_node_handle = &pipe_contex->vse_node_handle;
	vp_sensor_config_t *sensor_config = pipe_contex->sensor_config;
	isp_attr_t *isp_attr = sensor_config->isp_attr;

	vse_attr_t vse_attr = {0};
	vse_ichn_attr_t vse_ichn_attr = {0};
	vse_ochn_attr_t vse_ochn_attr = {0};
	uint32_t ichn_id = 0;
	uint32_t hw_id = 0;
	uint32_t input_width = isp_attr->crop.w;
	uint32_t input_height = isp_attr->crop.h;
	hbn_buf_alloc_attr_t alloc_attr = {0};

	vse_ichn_attr.width = input_width;
	vse_ichn_attr.height = input_height;
	vse_ichn_attr.fmt = FRM_FMT_NV12;
	vse_ichn_attr.bit_width = 8;

	vse_ochn_attr.chn_en = CAM_TRUE;
	vse_ochn_attr.roi.x = 0;
	vse_ochn_attr.roi.y = 0;
	vse_ochn_attr.roi.w = input_width;
	vse_ochn_attr.roi.h = input_height;
	vse_ochn_attr.fmt = FRM_FMT_NV12;
	vse_ochn_attr.bit_width = 8;

	vse_ochn_attr.target_w = input_width;
	vse_ochn_attr.target_h = input_height;

	ret = hbn_vnode_open(HB_VSE, hw_id, AUTO_ALLOC_ID, vse_node_handle);
	ERR_CON_EQ(ret, 0);

	ret = hbn_vnode_set_attr(*vse_node_handle, &vse_attr);
	ERR_CON_EQ(ret, 0);

	ret = hbn_vnode_set_ichn_attr(*vse_node_handle, ichn_id, &vse_ichn_attr);
	ERR_CON_EQ(ret, 0);

	alloc_attr.buffers_num = 3;
	alloc_attr.is_contig = 1;
	alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN |
			   HB_MEM_USAGE_CPU_WRITE_OFTEN |
			   HB_MEM_USAGE_CACHED;

	printf("hbn_vnode_set_ochn_attr: %dx%d\n", vse_ochn_attr.target_w, vse_ochn_attr.target_h);
	ret = hbn_vnode_set_ochn_attr(*vse_node_handle, 0, &vse_ochn_attr);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_ochn_buf_attr(*vse_node_handle, 0, &alloc_attr);
	ERR_CON_EQ(ret, 0);

	return 0;
}

static int create_gpu2d_node(pipe_contex_t *pipe_contex)
{
	int ret = 0;
	hbn_vnode_handle_t *gpu2d_node_handle = &pipe_contex->gpu2d_node_handle;
	hbn_buf_alloc_attr_t alloc_attr = {0};
	uint32_t ichn_id = 0;
	uint32_t ochn_id = 0;
	uint32_t hw_id = 0;
	uint32_t input_width = 0, input_height = 0;
	uint32_t output_width = 0, output_height = 0;
	uint32_t input_stride = 0, output_stride = 0;

	isp_ichn_attr_t isp_ichn_attr = {0};
	ret = hbn_vnode_get_ichn_attr(pipe_contex->isp_node_handle, ichn_id, &isp_ichn_attr);
	ERR_CON_EQ(ret, 0);

	input_width  = isp_ichn_attr.width;
	input_height = isp_ichn_attr.height;
	output_width  = input_width;
	output_height = input_height;

	// ==== Step 1. 检查输入宽度是否满足 64 字节对齐 ====
	if ((input_width % 64) != 0) {
		printf("[ERROR] GPU2D input width (%u) is not 64-byte aligned\n", input_width);
		return -1;
	}

	input_stride  = input_width;
	output_stride = output_width;
	printf("GPU2D input  %ux%u stride=%u\n", input_width, input_height, input_stride);
	printf("GPU2D output %ux%u stride=%u\n", output_width, output_height, output_stride);

	// ==== Step 2. 打开 GPU2D 节点 ====
	ret = hbn_vnode_open(HB_N2D, hw_id, AUTO_ALLOC_ID, gpu2d_node_handle);
	ERR_CON_EQ(ret, 0);
	pthread_mutex_lock(&g_gpu2d_mutex);

	// ==== Step 3. 设置 GPU2D 主属性 ====
	n2d_config_t gpu2d_attr = {0};
	gpu2d_attr.command = N2D_SCALE_CROP; 	// 操作类型
	gpu2d_attr.ninputs = 1;			// 输入通道数

	// 输入通道属性
	gpu2d_attr.input_width[0]  = input_width;
	gpu2d_attr.input_height[0] = input_height;
	gpu2d_attr.input_stride[0] = input_stride;

	// 输出通道属性
	gpu2d_attr.output_width  = output_width;
	gpu2d_attr.output_height = output_height;
	gpu2d_attr.output_stride = output_stride;
	gpu2d_attr.output_format = MEM_PIX_FMT_NV12;

	// 默认不裁剪
	gpu2d_attr.crop_x = 0;
	gpu2d_attr.crop_y = 0;
	gpu2d_attr.crop_width = 0;
	gpu2d_attr.crop_height = 0;

	memcpy(&g_gpu2d_attr, &gpu2d_attr, sizeof(n2d_config_t));
	pthread_mutex_unlock(&g_gpu2d_mutex);

	ret = hbn_vnode_set_attr(*gpu2d_node_handle, &gpu2d_attr);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_ichn_attr(*gpu2d_node_handle, ichn_id, &gpu2d_attr);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_ochn_attr(*gpu2d_node_handle, ochn_id, &gpu2d_attr);
	ERR_CON_EQ(ret, 0);

	// ==== Step 4. 设置输出 buffer 属性 ====
	alloc_attr.buffers_num = 3;
	alloc_attr.is_contig = 1;
	alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN  |
			   HB_MEM_USAGE_CPU_WRITE_OFTEN |
			   HB_MEM_USAGE_CACHED;

	ret = hbn_vnode_set_ochn_buf_attr(*gpu2d_node_handle, 0, &alloc_attr);
	ERR_CON_EQ(ret, 0);

	return 0;
}

static int set_n2d_crop_region(pipe_contex_t *pipe_context, int crop_x, int crop_y, int crop_w, int crop_h)
{
	int ret = 0;
	pthread_mutex_lock(&g_gpu2d_mutex);

	n2d_config_t *n2d_setting = &g_gpu2d_attr;

	if (crop_x < 0 || crop_y < 0 || crop_w <= 0 || crop_h <= 0) {
		fprintf(stderr, "Invalid crop parameters: x=%d, y=%d, w=%d, h=%d\n",
				crop_x, crop_y, crop_w, crop_h);
		pthread_mutex_unlock(&g_gpu2d_mutex);
		return -1;
	}

	if (crop_x + crop_w > n2d_setting->input_width[0] ||
		crop_y + crop_h > n2d_setting->input_height[0]) {
		fprintf(stderr, "Crop region exceeds input image boundaries\n");
		pthread_mutex_unlock(&g_gpu2d_mutex);
		return -1;
	}

	n2d_setting->crop_x = crop_x;
	n2d_setting->crop_y = crop_y;
	n2d_setting->crop_width = crop_w;
	n2d_setting->crop_height = crop_h;

	ret = hbn_vnode_set_ochn_attr_ex(pipe_context->gpu2d_node_handle, 0, n2d_setting);
	pthread_mutex_unlock(&g_gpu2d_mutex);

	if (ret != 0) {
		fprintf(stderr, "Failed to set GPU2D attributes: %d\n", ret);
		return ret;
	}

	if (verbose_flag) {
		printf("GPU2D crop region updated: x=%d, y=%d, w=%d, h=%d\n",
			crop_x, crop_y, crop_w, crop_h);
	}

	return 0;
}

int set_n2d_crop_region_safe(pipe_contex_t *pipe_context, int crop_x, int crop_y, int crop_w, int crop_h)
{
	pthread_mutex_lock(&g_crop_mutex);
	int ret = set_n2d_crop_region(pipe_context, crop_x, crop_y, crop_w, crop_h);
	pthread_mutex_unlock(&g_crop_mutex);

	return ret;
}

static int create_and_run_vflow(pipe_contex_t *pipe_contex, int active_mipi_host, uint32_t sensor_mode)
{
	int32_t ret = 0;

	ret = create_camera_node(pipe_contex, sensor_mode);
	ERR_CON_EQ(ret, 0);
	ret = create_vin_node(pipe_contex, active_mipi_host);
	ERR_CON_EQ(ret, 0);
	ret = create_isp_node(pipe_contex);
	ERR_CON_EQ(ret, 0);
	ret = create_vse_node(pipe_contex);
	ERR_CON_EQ(ret, 0);
	ret = create_gpu2d_node(pipe_contex);
	ERR_CON_EQ(ret, 0);

	ret = hbn_vflow_create(&pipe_contex->vflow_fd);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_add_vnode(pipe_contex->vflow_fd, pipe_contex->vin_node_handle);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_add_vnode(pipe_contex->vflow_fd, pipe_contex->isp_node_handle);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_add_vnode(pipe_contex->vflow_fd, pipe_contex->vse_node_handle);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_add_vnode(pipe_contex->vflow_fd, pipe_contex->gpu2d_node_handle);
	ERR_CON_EQ(ret, 0);

	if (vin_isp_is_online) {
		ret = hbn_vflow_bind_vnode(pipe_contex->vflow_fd,
					   pipe_contex->vin_node_handle,
					   1,
					   pipe_contex->isp_node_handle,
					   0);
	} else {
		ret = hbn_vflow_bind_vnode(pipe_contex->vflow_fd,
					   pipe_contex->vin_node_handle,
					   0,
					   pipe_contex->isp_node_handle,
					   0);
	}
	ERR_CON_EQ(ret, 0);

	if (isp_vse_is_online) {
		ret = hbn_vflow_bind_vnode(pipe_contex->vflow_fd,
					   pipe_contex->isp_node_handle,
					   1,
					   pipe_contex->vse_node_handle,
					   0);
	} else {
		ret = hbn_vflow_bind_vnode(pipe_contex->vflow_fd,
					   pipe_contex->isp_node_handle,
					   0,
					   pipe_contex->vse_node_handle,
					   0);
	}
	ERR_CON_EQ(ret, 0);

	ret = hbn_vflow_bind_vnode(pipe_contex->vflow_fd,
				   pipe_contex->vse_node_handle,
				   0,
				   pipe_contex->gpu2d_node_handle,
				   0);
	ERR_CON_EQ(ret, 0);

	ret = hbn_camera_attach_to_vin(pipe_contex->cam_fd, pipe_contex->vin_node_handle);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_start(pipe_contex->vflow_fd);
	ERR_CON_EQ(ret, 0);

	return 0;
}

void *producer_thread(void *context)
{
	pipe_contex_t *pipe_context = (pipe_contex_t *)context;
	hbn_vnode_handle_t gpu2d_node_handle = pipe_context->gpu2d_node_handle;//vse_node_handle;//gpu2d_node_handle;

	uint64_t frame_counters = 0;
	uint64_t drop_count = 0;
	const uint64_t adjust_interval = 3;

	while (running) {
		hbn_vnode_image_t out_img = {0};
		int get_frame_ok = 0;

		if (hbn_vnode_getframe(gpu2d_node_handle, 0, 2000, &out_img) == 0) {
			get_frame_ok = 1;

			int push_ret = frame_queue_push(&frame_queue, &out_img);
			if (push_ret != 0) {
				drop_count++;
				printf("frame queue full, drop frame (drop count: %lu)\n", drop_count);
			} else {
				frame_counters++;

				if (frame_counters % adjust_interval == 0) {
					static int crop_x = 0;
					static int crop_y = 0;
					static int crop_w = 0;
					static int crop_h = 0;

					int input_w = g_gpu2d_attr.input_width[0];
					int input_h = g_gpu2d_attr.input_height[0];

					if (crop_w == 0 || crop_h == 0) {
						crop_w = input_w;
						crop_h = input_h;
					}

					crop_w -= 200;
					crop_h -= 100;

					if (crop_w < input_w / 2 || crop_h < input_h / 2) {
						crop_w = input_w;
						crop_h = input_h;
					}
					set_n2d_crop_region_safe(pipe_context, crop_x, crop_y, crop_w, crop_h);

					if (verbose_flag) {
						printf("Adjusted crop region for pipeline %d: x=%d, y=%d, w=%d, h=%d\n",
							0, crop_x, crop_y, crop_w, crop_h);
					}
				}
			}
		} else {
			drop_count++;
			printf("hbn_vnode_getframe failed (drop count: %lu)\n", drop_count);
		}

		if (get_frame_ok) {
			hbn_vnode_releaseframe(gpu2d_node_handle, 0, &out_img);
		}
	}

	return NULL;
}

void *consumer_thread(void *context)
{
	uint32_t count = 0;
	char dst_file[256];
	hbn_vnode_image_t out_img;
	pipe_contex_t *pipe_context = (pipe_contex_t *)context;

	while (running) {
		if (frame_queue_pop(&frame_queue, &out_img) != 0)
			break;

		if (count % 3 == 0) {
			snprintf(dst_file, sizeof(dst_file), "./%s_%dx%d_frameid%d_ts_%ld.yuv",
				 pipe_context->sensor_config->sensor_name,
				 out_img.buffer.width,
				 out_img.buffer.height,
				 out_img.info.frame_id,
				 out_img.info.timestamps);
			dump_2plane_yuv_to_file(dst_file,
				out_img.buffer.virt_addr[0], out_img.buffer.virt_addr[1],
				out_img.buffer.size[0], out_img.buffer.size[1]);
			printf("Saved frame to %s ok\n", dst_file);
		}

		if (verbose_flag) {
			printf("Received frame %d, frame_id: %d\n", count, out_img.info.frame_id);
		}

		count++;
	}

	return NULL;
}

static void print_help(const char *argv0)
{
	printf("Usage: %s [OPTIONS]\n", argv0);
	printf("Options:\n");
	printf("  -s <sensor_index>		Specify sensor index\n");
	printf("  -v 				Enable verbose mode\n");
	printf("  -c <channel_type>		Specify channel type: vo and vf and io and if, default: vf:if\n");
	printf("		Support both individual configuration and combined configuration.\n");
	printf("		The individual configuration supports four types:\n");
	printf("				1. vo: vin online isp\n");
	printf("				2. vf: vin offline isp\n");
	printf("				3. io: isp online vse\n");
	printf("				4. if: isp offline vse\n");
	printf("		The combination configuration supports four types:\n");
	printf("				1. vo:io  vin online isp + isp online vse\n");
	printf("				2. vo:if  vin online isp + isp offline vse\n");
	printf("				3. vf:io  vin offline isp + isp online vse\n");
	printf("				4. vf:if  vin offline isp + isp offline vse\n");
	printf("  -h	Show help message\n");
	vp_show_sensors_list();
}

int main(int argc, char** argv)
{
	int ret = 0;
	pipe_contex_t pipe_contex = {0};
	pthread_t producer_tid, consumer_tid;
	int c = 0;
	int index = -1;
	int active_mipi_host;

	signal(SIGINT, signal_handle);
	signal(SIGTERM, signal_handle);

	if (argc <= 1) {
		print_help(argv[0]);
		return 0;
	}

	while ((c = getopt_long(argc, argv, "s:c:vh", long_options, NULL)) != -1) {
		switch (c) {
		case 's':
			if (!is_number(optarg)) {
				printf("Invalid sensor index '%s'. Expected a number, e.g. -s 40\n", optarg);
				print_help(argv[0]);
				return -1;
			}
			index = atoi(optarg);
			break;
		case 'c':
			if (parse_channel_string(optarg) != 0) {
				printf("Invalid channel type %s.\n", optarg);
				return -1;
			}
			break;
		case 'v':
			verbose_flag = 1;
			break;
		case 'h':
		default:
			print_help(argv[0]);
			return 0;
		}
	}

	printf("Verbose: %d\n", verbose_flag);

	if (index < vp_get_sensors_list_number() && index >= 0) {
		pipe_contex.sensor_config = vp_sensor_config_list[index];
		printf("Using index:%d  sensor_name:%s  config_file:%s\n",
			index,
			vp_sensor_config_list[index]->sensor_name,
			vp_sensor_config_list[index]->config_file);
		ret = vp_sensor_fixed_mipi_host(pipe_contex.sensor_config, &pipe_contex.csi_config);
		if (ret != 0) {
			printf("No Camera Sensor found. Please check if the specified "
				"sensor is connected to the Camera interface.\n");
			return ret;
		}
		active_mipi_host = pipe_contex.sensor_config->vin_node_attr->cim_attr.mipi_rx;
	} else {
		printf("Unsupport sensor index:%d\n", index);
		print_help(argv[0]);
		return 0;
	}

	if ((pipe_contex.sensor_config->camera_config->sensor_mode == DOL2_M) && \
	    (vin_isp_is_online == 0)) {
		printf("\nError:%s's sensor_mode is DOL2_M, must work in online mode.\n\n",
			pipe_contex.sensor_config->sensor_name);
		return -1;
	}

	printf("\tSensor name: %s\n", pipe_contex.sensor_config->sensor_name);
	printf("\tActive mipi host: %d\n", active_mipi_host);

	printf("Connection method from VIN to ISP: %s\n",
		(vin_isp_is_online == 1) ? "vin online isp" : "vin offline isp");
	printf("Connection method from VIN to ISP: %s.\n",
		(isp_vse_is_online == 1) ? "isp online vse" : "isp offline vse");
	printf("\n");

	hb_mem_module_open();

	ret = create_and_run_vflow(&pipe_contex, active_mipi_host,
				   pipe_contex.sensor_config->camera_config->sensor_mode);
	ERR_CON_EQ(ret, 0);

	frame_queue_init(&frame_queue);
	running = 1;
	printf("pthread_create\n");

	pthread_create(&producer_tid, NULL, producer_thread, (void *)&pipe_contex);
	pthread_create(&consumer_tid, NULL, consumer_thread, (void *)&pipe_contex);

	printf("Running... Press Ctrl+C to exit\n");

	pthread_cond_broadcast(&frame_queue.not_empty);
	pthread_cond_broadcast(&frame_queue.not_full);

	pthread_join(producer_tid, NULL);
	pthread_join(consumer_tid, NULL);
	printf("pthread_join success\n");

	hbn_vflow_stop(pipe_contex.vflow_fd);
	hbn_camera_destroy(pipe_contex.cam_fd);
	hbn_vflow_destroy(pipe_contex.vflow_fd);

	hb_mem_module_close();

	return 0;
}
