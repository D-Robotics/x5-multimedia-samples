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
#include <stddef.h>
#include <execinfo.h>
#include <ctype.h>

#include "common_utils.h"

#define MAX_SENSORS 4

typedef struct {
	int select_sensor_id;
	uint32_t sensor_mode;
	pipe_contex_t pipe_contexts;
	int active_mipi_host; // 根据实际的硬件连接情况确定使用对应的 mipi host
	int bind_chn;
	pthread_mutex_t config_mutex;  // 添加互斥锁字段

	struct {
		uint64_t frame_count;
		uint64_t drop_count;
	} stats;

} pipeline_info_t;

typedef struct {
	pipeline_info_t *pipeline_info[MAX_SENSORS]; // 使用数组存储指针
} thread_args_t;

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
static int32_t total_pipeline_num = 0;
static int32_t verbose_flag = 0;
static int32_t used_mipi_host = 0;
static uint32_t link_port[MAX_SENSORS] = {};
static int32_t running = 0;
static uint16_t sensor_type;
static n2d_config_t g_gpu2d_attr = {0};
static pthread_mutex_t g_gpu2d_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct option const long_options[] = {
	{"sensor", required_argument, NULL, 's'},
	{NULL, 0, NULL, 0}
};

static int create_and_run_vflow(pipe_contex_t *pipe_contex, int active_mipi_host,
	 int index, uint32_t sensor_mode);
int32_t hbn_deserial_create(deserial_config_t *des_config, deserial_handle_t *des_fd);
int32_t hbn_deserial_attach_to_vin(deserial_handle_t des_fd, camera_des_link_t link, vpf_handle_t vin_fd);
void parse_config(pipeline_info_t *pipeline_info, const char *config, int pipeline_idx);

static void frame_queue_init(frame_queue_t *q) {
	memset(q, 0, sizeof(frame_queue_t));
	pthread_mutex_init(&q->mutex, NULL);
	pthread_cond_init(&q->not_empty, NULL);
	pthread_cond_init(&q->not_full, NULL);
}

static int frame_queue_pop(frame_queue_t *q, hbn_vnode_image_t *out) {
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

static int frame_queue_push(frame_queue_t *q, hbn_vnode_image_t *frame) {
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

static void show_help() {
	printf("Usage: single_pipe_vin_isp_vse_gpu2d [options]\n");
	printf("Options:\n");
	printf("  -s \"sensor=index\"            Select sensor index to use\n");
	printf("  -v, --verbose                  Enable verbose mode\n");
	printf("  -h, --help                     Show this help message\n");
	printf("Available sensors:\n");
	vp_show_sensors_list();
}


static int is_number(const char *str) {
	while (*str) {
		if (!isdigit(*str)) return 0;
		str++;
	}
	return 1;
}

// 分割字符串并返回数组的个数
static int split_string(const char *str, const char *delim, char *out[], int max_parts) {
	int count = 0;
	char *token;
	char *str_copy = strdup(str);
	char *rest = str_copy;

	while ((token = strtok_r(rest, delim, &rest)) && count < max_parts) {
		out[count++] = strdup(token);
	}

	free(str_copy);
	return count;
}

void parse_config(pipeline_info_t *pipeline_info, const char *config, int pipeline_idx) {
	int ret = 0;
	char *parts[4];
	int sensor_idx = -1;
	int count = split_string(config, " ", parts, 4);
	pipeline_info->bind_chn = 0;

	for (int i = 0; i < count; i++) {
		char *key_value[2];
		int kv_count = split_string(parts[i], "=", key_value, 2);
		if (kv_count != 2) {
			fprintf(stderr, "Invalid config format: '%s'. Expected format: sensor=index\n", parts[i]);
			exit(1);
		}

		if (strcmp(key_value[0], "sensor") == 0) {
			if (!is_number(key_value[1])) {
				fprintf(stderr, "Invalid sensor ID: %s\n", key_value[1]);
				exit(1);
			}
			sensor_idx = atoi(key_value[1]);

			if (sensor_idx >= 0 && sensor_idx < vp_get_sensors_list_number()) {
				pipeline_info->pipe_contexts.sensor_config = vp_sensor_config_list[sensor_idx];
				printf("Using index:%d  sensor_name:%s  config_file:%s\n",
						sensor_idx,
						vp_sensor_config_list[sensor_idx]->sensor_name,
						vp_sensor_config_list[sensor_idx]->config_file);

				sensor_type = pipeline_info->pipe_contexts.sensor_config->sensor_type;

				if(sensor_type == SENSOR_TYPE_NORMAL) {
					ret = vp_sensor_multi_fixed_mipi_host(
						pipeline_info->pipe_contexts.sensor_config,
						used_mipi_host,
						&pipeline_info->pipe_contexts.csi_config
					);
					if (ret < 0) {
						fprintf(stderr,
							"Sensor %d init failed. No Camera Sensor found on the specified interface.\n",
							sensor_idx);
						exit(1);
					}
					pipeline_info->select_sensor_id = sensor_idx;
					pipeline_info->active_mipi_host = pipeline_info->pipe_contexts.sensor_config->vin_node_attr->cim_attr.mipi_rx;
					used_mipi_host |= (1 << pipeline_info->pipe_contexts.sensor_config->vin_node_attr->cim_attr.mipi_rx);
				}
			} else {
				fprintf(stderr, "Unsupported sensor index: %d\n", sensor_idx);
				show_help();
				exit(1);
			}
		} else if (strcmp(key_value[0], "mode") == 0) {
			if (!is_number(key_value[1])) {
				fprintf(stderr, "Invalid sensor mode number: %s\n", key_value[1]);
				continue;
			}
			pipeline_info->sensor_mode = atoi(key_value[1]);
		} else {
			fprintf(stderr, "Unknown key: %s\n", key_value[0]);
			exit(1);
		}

		for (int j = 0; j < kv_count; j++) {
			free(key_value[j]);
		}
	}

	for (int i = 0; i < count; i++) {
		free(parts[i]);
	}
}

static int create_camera_node(pipe_contex_t *pipe_contex, uint32_t sensor_mode) {
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
		if (sensor_cfg->vin_node_attr)
			sensor_cfg->vin_node_attr->lpwm_attr.enable = 1;
	}

	int32_t ret = hbn_camera_create(cam_cfg, &pipe_contex->cam_fd);
	ERR_CON_EQ(ret, 0);

	return 0;
}

static int create_vin_node(pipe_contex_t *pipe_contex, int active_mipi_host, int index) {
	vp_sensor_config_t *sensor_config = NULL;
	vin_node_attr_t *vin_node_attr = NULL;
	vin_ichn_attr_t *vin_ichn_attr = NULL;
	vin_ochn_attr_t *vin_ochn_attr = NULL;
	hbn_vnode_handle_t *vin_node_handle = NULL;
	vin_attr_ex_t vin_attr_ex;
	hbn_buf_alloc_attr_t alloc_attr = {0};
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

	link_port[index] = vin_node_attr->cim_attr.vc_index;
	if(pipe_contex->csi_config.mclk_is_not_configed){
		// 设备树中没有配置 mclk：使用外部晶振
		printf("csi%d ignore mclk ex attr, because not config mclk.\n",
			pipe_contex->csi_config.index);
	}else{
		vin_attr_ex.vin_attr_ex_mask = sensor_config->vin_attr_ex->vin_attr_ex_mask;
		vin_attr_ex.mclk_ex_attr.mclk_freq = sensor_config->vin_attr_ex->mclk_ex_attr.mclk_freq;
		vin_attr_ex_mask = vin_attr_ex.vin_attr_ex_mask;
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
	alloc_attr.buffers_num = 3;
	alloc_attr.is_contig = 1;
	alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN
						| HB_MEM_USAGE_CPU_WRITE_OFTEN
						| HB_MEM_USAGE_CACHED;
	ret = hbn_vnode_set_ochn_buf_attr(*vin_node_handle, ochn_id, &alloc_attr);

	return 0;
}


static int create_isp_node(pipe_contex_t *pipe_contex) {
	vp_sensor_config_t *sensor_config = NULL;
	isp_attr_t      *isp_attr = NULL;
	isp_ichn_attr_t *isp_ichn_attr = NULL;
	isp_ochn_attr_t *isp_ochn_attr = NULL;
	hbn_vnode_handle_t *isp_node_handle = NULL;
	vin_node_attr_t *vin_node_attr = NULL;
	hbn_buf_alloc_attr_t alloc_attr = {0};
	uint32_t chn_id = 0;
	int ret = 0;

	sensor_config = pipe_contex->sensor_config;
	vin_node_attr = sensor_config->vin_node_attr;
	isp_attr = sensor_config->isp_attr;
	isp_ichn_attr = sensor_config->isp_ichn_attr;
	isp_ochn_attr = sensor_config->isp_ochn_attr;
	isp_node_handle = &pipe_contex->isp_node_handle;

	/*—— 根据 isp_attr->input_mode 决定是否打开 VIN 的 fly-by ——*/
	switch (isp_attr->input_mode) {
		case SIF_ONLINE_ISP:
			vin_node_attr->cim_attr.cim_isp_flyby = 1;
			break;
		default:
			vin_node_attr->cim_attr.cim_isp_flyby = 0;
			break;
	}

	ret = hbn_vnode_open(HB_ISP, 0, AUTO_ALLOC_ID, isp_node_handle);
	ERR_CON_EQ(ret, 0);

	ret = hbn_vnode_set_attr(*isp_node_handle, isp_attr);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_ochn_attr(*isp_node_handle, chn_id, isp_ochn_attr);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_ichn_attr(*isp_node_handle, chn_id, isp_ichn_attr);
	ERR_CON_EQ(ret, 0);

	alloc_attr.buffers_num = 3;
	alloc_attr.is_contig = 1;
	alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN
		| HB_MEM_USAGE_CPU_WRITE_OFTEN
		| HB_MEM_USAGE_CACHED;
	ret = hbn_vnode_set_ochn_buf_attr(*isp_node_handle, chn_id, &alloc_attr);
	ERR_CON_EQ(ret, 0);

	return 0;
}

static int create_vse_node(pipe_contex_t *pipe_contex, int vse_bind_index) {
	int ret = 0;
	hbn_vnode_handle_t *vse_node_handle = &pipe_contex->vse_node_handle;
	isp_ichn_attr_t isp_ichn_attr = {0};
	vse_attr_t vse_attr = {0};
	vse_ichn_attr_t vse_ichn_attr = {0};
	vse_ochn_attr_t vse_ochn_attr[VSE_MAX_CHANNELS] = {0};
	uint32_t ichn_id = 0;
	uint32_t hw_id = 0;
	uint32_t input_width = 0, input_height = 0;
	uint32_t output_width = 0, output_height = 0;
	hbn_buf_alloc_attr_t alloc_attr = {0};

	ret = hbn_vnode_get_ichn_attr(pipe_contex->isp_node_handle, ichn_id, &isp_ichn_attr);
	ERR_CON_EQ(ret, 0);
	input_width = isp_ichn_attr.width;
	input_height = isp_ichn_attr.height;

	vse_ichn_attr.width = input_width;
	vse_ichn_attr.height = input_height;
	vse_ichn_attr.fmt = FRM_FMT_NV12;
	vse_ichn_attr.bit_width = 8;

	vse_ochn_attr[vse_bind_index].chn_en = CAM_TRUE;
	vse_ochn_attr[vse_bind_index].roi.x = 0;
	vse_ochn_attr[vse_bind_index].roi.y = 0;
	vse_ochn_attr[vse_bind_index].roi.w = input_width;
	vse_ochn_attr[vse_bind_index].roi.h = input_height;
	vse_ochn_attr[vse_bind_index].fmt = FRM_FMT_NV12;
	vse_ochn_attr[vse_bind_index].bit_width = 8;

	configure_vse_max_resolution(vse_bind_index,
		input_width, input_height,
		&output_width, &output_height);

	// 输出原分辨率
	vse_ochn_attr[vse_bind_index].target_w = output_width;
	vse_ochn_attr[vse_bind_index].target_h = output_height;

	ret = hbn_vnode_open(HB_VSE, hw_id, AUTO_ALLOC_ID, vse_node_handle);
	ERR_CON_EQ(ret, 0);

	ret = hbn_vnode_set_attr(*vse_node_handle, &vse_attr);
	ERR_CON_EQ(ret, 0);

	ret = hbn_vnode_set_ichn_attr(*vse_node_handle, ichn_id, &vse_ichn_attr);
	ERR_CON_EQ(ret, 0);

	alloc_attr.buffers_num = 3;
	alloc_attr.is_contig = 1;
	alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN | HB_MEM_USAGE_CPU_WRITE_OFTEN | HB_MEM_USAGE_CACHED;

	printf("hbn_vnode_set_ochn_attr: %dx%d\n", vse_ochn_attr[vse_bind_index].target_w,
		vse_ochn_attr[vse_bind_index].target_h);
	ret = hbn_vnode_set_ochn_attr(*vse_node_handle, vse_bind_index, &vse_ochn_attr[vse_bind_index]);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_ochn_buf_attr(*vse_node_handle, vse_bind_index, &alloc_attr);
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

	input_stride  = input_width;   // 直接用原宽度
	output_stride = output_width;
	printf("GPU2D input  %ux%u stride=%u\n", input_width, input_height, input_stride);
	printf("GPU2D output %ux%u stride=%u\n", output_width, output_height, output_stride);

	// ==== Step 2. 打开 GPU2D 节点 ====
	ret = hbn_vnode_open(HB_N2D, hw_id, AUTO_ALLOC_ID, gpu2d_node_handle);
	ERR_CON_EQ(ret, 0);
	pthread_mutex_lock(&g_gpu2d_mutex);

	// ==== Step 3. 设置 GPU2D 主属性 ====
	n2d_config_t gpu2d_attr = {0};
	gpu2d_attr.command = N2D_SCALE_CROP;  // 操作类型
	gpu2d_attr.ninputs = 1;				// 输入通道数

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
	alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN |
					HB_MEM_USAGE_CPU_WRITE_OFTEN |
					HB_MEM_USAGE_CACHED;
	ret = hbn_vnode_set_ochn_buf_attr(*gpu2d_node_handle, 0, &alloc_attr);
	if (ret < 0) {
		printf("hbn_vnode_set_ochn_buf_attr failed, ret = %d\n", ret);
		return -1;
	}

	return 0;
}

static int set_n2d_crop_region(pipe_contex_t *pipe_contex,
							int crop_x, int crop_y, int crop_w, int crop_h)
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

	ret = hbn_vnode_set_ochn_attr_ex(pipe_contex->gpu2d_node_handle, 0, n2d_setting);
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


int set_n2d_crop_region_safe(pipeline_info_t *pipeline, int crop_x, int crop_y, int crop_w, int crop_h) {
	pthread_mutex_lock(&pipeline->config_mutex);
	int ret = set_n2d_crop_region(&pipeline->pipe_contexts, crop_x, crop_y, crop_w, crop_h);
	pthread_mutex_unlock(&pipeline->config_mutex);
	return ret;
}

static int create_and_run_vflow(pipe_contex_t *pipe_contex, int active_mipi_host,
	 int index, uint32_t sensor_mode)
{
	vp_sensor_config_t *sensor_config = pipe_contex->sensor_config;
	int isp_mode = sensor_config->isp_attr->input_mode;
	int32_t ret = 0;
	// 创建 pipeline 中的每个 node
	ret = create_camera_node(pipe_contex, sensor_mode);
	ERR_CON_EQ(ret, 0);
	ret = create_vin_node(pipe_contex, active_mipi_host, index);
	ERR_CON_EQ(ret, 0);
	ret = create_isp_node(pipe_contex);
	ERR_CON_EQ(ret, 0);
	ret = create_vse_node(pipe_contex, 0);
	ERR_CON_EQ(ret, 0);
	ret = create_gpu2d_node(pipe_contex);
	ERR_CON_EQ(ret, 0);
	// 创建 HBN flow
	ret = hbn_vflow_create(&pipe_contex->vflow_fd);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_add_vnode(pipe_contex->vflow_fd,
							pipe_contex->vin_node_handle);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_add_vnode(pipe_contex->vflow_fd,
							pipe_contex->isp_node_handle);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_add_vnode(pipe_contex->vflow_fd,
							pipe_contex->vse_node_handle);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_add_vnode(pipe_contex->vflow_fd,
							pipe_contex->gpu2d_node_handle);
	ERR_CON_EQ(ret, 0);

	if (isp_mode == SIF_OFFLINE_ISP) {
		ret = hbn_vflow_bind_vnode(pipe_contex->vflow_fd,
								pipe_contex->vin_node_handle,
								0,
								pipe_contex->isp_node_handle,
								0);
	} else if (isp_mode == SIF_ONLINE_ISP || isp_mode == SIF_MCM_ISP) {
		ret = hbn_vflow_bind_vnode(pipe_contex->vflow_fd,
								pipe_contex->vin_node_handle,
								1,
								pipe_contex->isp_node_handle,
								0);
	} else {
		printf("Unsupported ISP mode: %d\n", isp_mode);
		return -1;
	}
	ret = hbn_vflow_bind_vnode(pipe_contex->vflow_fd,
							pipe_contex->isp_node_handle,
							1,
							pipe_contex->vse_node_handle,
							0);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_bind_vnode(pipe_contex->vflow_fd,
							pipe_contex->vse_node_handle,
							0,
							pipe_contex->gpu2d_node_handle,
							0);
	ERR_CON_EQ(ret, 0);

	ret = hbn_camera_attach_to_vin(pipe_contex->cam_fd,
							pipe_contex->vin_node_handle);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_start(pipe_contex->vflow_fd);
	ERR_CON_EQ(ret, 0);

	return 0;
}

void *producer_thread(void *context) {
	thread_args_t *args = (thread_args_t *)context;

	uint64_t frame_counters[MAX_SENSORS] = {0};
	const uint64_t adjust_interval = 3;

	while (running) {
		for (int i = 0; i < total_pipeline_num; i++) {
			pipeline_info_t *p = args->pipeline_info[i];
			hbn_vnode_image_t out_img = {0};
			int get_frame_ok = 0;

			if (hbn_vnode_getframe(p->pipe_contexts.gpu2d_node_handle, p->bind_chn, 2000, &out_img) == 0) {
				get_frame_ok = 1;

				int push_ret = frame_queue_push(&frame_queue, &out_img);
				if (push_ret != 0) {
					p->stats.drop_count++;
					printf("Pipeline %d: frame queue full, drop frame (drop count: %lu)\n", i, p->stats.drop_count);
				} else {
					frame_counters[i]++;

					if (frame_counters[i] % adjust_interval == 0) {
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
						set_n2d_crop_region_safe(p, crop_x, crop_y, crop_w, crop_h);

						if (verbose_flag) {
							printf("Adjusted crop region for pipeline %d: x=%d, y=%d, w=%d, h=%d\n",
								i, crop_x, crop_y, crop_w, crop_h);
						}
					}
				}
			} else {
				p->stats.drop_count++;
				printf("hbn_vnode_getframe failed for pipeline %d (drop count: %lu)\n", i, p->stats.drop_count);
			}

			if (get_frame_ok) {
				hbn_vnode_releaseframe(p->pipe_contexts.gpu2d_node_handle, p->bind_chn, &out_img);
			}
		}
	}

	return NULL;
}

void *consumer_thread(void *context) {
	uint32_t count = 0;
	char dst_file[128];
	hbn_vnode_image_t out_img;

	while (running) {
		if (frame_queue_pop(&frame_queue, &out_img) != 0)
			break;

		if (count % 3 == 0) {
			snprintf(dst_file, sizeof(dst_file),
				"frame_%dx%d_id%d_ts_%ld.yuv",
				out_img.buffer.width, out_img.buffer.height,
				out_img.info.frame_id, out_img.info.timestamps);
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

int main(int argc, char** argv) {
	int ret = 0;
	int c = 0;
	int index = -1;

	thread_args_t *args = malloc(sizeof(thread_args_t));
	pipeline_info_t pipeline_info[MAX_SENSORS] = {0};

	if (argc <= 1) {
		show_help();
		return 0;
	}

	while ((c = getopt_long(argc, argv, "s:vh:d", long_options, NULL)) != -1) {
		switch (c) {
		case 's':
			if (total_pipeline_num >= MAX_SENSORS) {
				fprintf(stderr, "Too many configurations. Maximum allowed is %d.\n", MAX_SENSORS);
				return 1;
			}
			parse_config(&pipeline_info[total_pipeline_num], optarg, total_pipeline_num);
			total_pipeline_num++;
			break;
		case 'v':
			verbose_flag = 1;
			break;
		case 'h':
		default:
			show_help();
			return 0;
		}
	}

	// 处理后的参数在这里可以使用
	for (int i = 0; i < total_pipeline_num; i++) {
		args->pipeline_info[i] = &pipeline_info[i];
		printf("Pipeline index %d:\n", i);
		printf("\tSensor index: %d\n", pipeline_info[i].select_sensor_id);
		printf("\tSensor name: %s\n", pipeline_info[i].pipe_contexts.sensor_config->sensor_name);
		printf("\tActive mipi host: %d\n", pipeline_info[i].active_mipi_host);
		printf("\tVse Channel: %d\n", pipeline_info[i].bind_chn);
	}

	printf("MIPI host: 0x%x\n", used_mipi_host);
	for (int i = 0; i < MAX_SENSORS; i++) {
		if (used_mipi_host & (1 << i)) {
			printf("  Host %d: Used\n", i);
		}
	}
	printf("Verbose: %d\n", verbose_flag);
	hb_mem_module_open();

	for (index = 0; index < total_pipeline_num; index++) {
		ret = create_and_run_vflow(&args->pipeline_info[index]->pipe_contexts,
			pipeline_info[index].active_mipi_host,
			index,
			pipeline_info[index].sensor_mode);
		if (ret != 0) {
			for (int j = 0; j < index; j++) {
				hbn_vflow_stop(args->pipeline_info[j]->pipe_contexts.vflow_fd);
				hbn_vflow_destroy(args->pipeline_info[j]->pipe_contexts.vflow_fd);
			}
			return 0;
		}
	}

	for (int i = 0; i < total_pipeline_num; i++) {
		printf("Before create_serdes_fd_and_attach: Pipeline %d, cam_fd = %ld\n",
		i, args->pipeline_info[i]->pipe_contexts.cam_fd);
	}

	printf("pthread_create\n");

	frame_queue_init(&frame_queue);
	running = 1;
	pthread_t producer_tid, consumer_tid;

	pthread_create(&producer_tid, NULL, producer_thread, args);
	pthread_create(&consumer_tid, NULL, consumer_thread, NULL);

	// 主线程等待退出信号
	printf("Running... Press Ctrl+C to exit\n");
	while (running) {
		sleep(1); // 保持主线程存活
	}

	running = 0;
	// 等待线程退出
	pthread_cond_broadcast(&frame_queue.not_empty);
	pthread_cond_broadcast(&frame_queue.not_full);

	pthread_join(producer_tid, NULL);
	pthread_join(consumer_tid, NULL);
	printf("pthread_join success\n");
	for (int i = 0; i < total_pipeline_num; i++) {
		ret = hbn_vflow_stop(pipeline_info[i].pipe_contexts.vflow_fd);
		if (ret != 0) {
			printf("hbn_vflow_stop failed for sensor %d. ret = %d\n", i, ret);
		}
		hbn_vflow_destroy(pipeline_info[i].pipe_contexts.vflow_fd);
	}
	free(args);
	hb_mem_module_close();
	return 0;
}