#include <stdio.h>
#include <getopt.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <string.h>
#include <sys/prctl.h>
#include <sched.h>
#include <pthread.h>
#include <signal.h>
#include <ctype.h>
#include <errno.h>
#include <stdatomic.h>

#include "common_utils.h"
#include "wrapper.h"
#include "pipe_stream.h"
#include "imu.h"
#include "mipi_csi_tx.h"
#include "performance_test_util.h"
#include "synchronous_queue.h"

#include "hbn_isp_api.h"
#include "gdc_cfg.h"
#include "gdc_bin_cfg.h"
#include "cJSON.h"

#include "imu_manager.h"

/*
	VSE Channel
	┌─────────────────────┐
	│ CHN 0 - Right Cam   │
	├─────────────────────┤
	│ CHN 1 - Right Depth │
	├─────────────────────┤
	│ CHN 2 - LEFT Depth  │
	├─────────────────────┤
	│ CHN 3 - Idle        │
	├─────────────────────┤
	│ CHN 4 - Idle        │
	├─────────────────────┤
	│ CHN 5 - Left Cam    │
	└─────────────────────┘
*/
#define MAX_PIPE_NUM 			2
#define RIGHT_CAM_CHN			0
#define LEFT_CAM_CHN			1
#define RIGHT_CAM_VSE_CHN		0
#define LEFT_CAM_VSE_CHN		5
#define RIGHT_STEREO_VSE_CHN	1
#define LEFT_STEREO_VSE_CHN		2

#define DRM_MIPI_CSI_RES_WIDTH	1280
#define DRM_MIPI_CSI_RES_HEIGHT	1216

#define LEFT_CAM_GDC_BIN_PATH	"/tmp/left_camera_gdc.bin"
#define RIGHT_CAM_GDC_BIN_PATH	"/tmp/right_camera_gdc.bin"

#define DEBUG_INTFER_DUMP_FLLE
// #define DEBUG_INFER_TIME_CONSUM
#define IMU_ENABLE
#define USE_HBMEM

int stream_server_socket_init(void);
int stream_server_socket_destroy(void);
int stream_server_socket_process(void);
int stream_server_socket_flush(void);
int stream_server_mipi_init(void);
int stream_server_mipi_destroy(void);
int stream_server_mipi_process(void);
int stream_server_mipi_flush(void);
int stream_server_local_process(void);

enum STREAM_SERVER_TYPE_E {
	STREAM_SERVER_NULL = -1,
	STREAM_SERVER_SOCKET = 0,
	STREAM_SERVER_MIPI_CSI,
	STREAM_SERVER_LOCAL_TEST,
	STREAM_SERVER_TAIL
};
#define STREAM_SERVER_TYPE_NUM	STREAM_SERVER_TAIL

typedef int (*stream_server_init_func_t)(void);
typedef int (*stream_server_destroy_func_t)(void);
typedef int (*stream_server_process_func_t)(void);
typedef int (*stream_server_flush_func_t)(void);

typedef struct {
	char name[16];
	stream_server_init_func_t init;
	stream_server_destroy_func_t destroy;
	stream_server_process_func_t process;
	stream_server_flush_func_t flush;
} stream_server_func_t;

static stream_server_func_t stream_server_func_list[STREAM_SERVER_TYPE_NUM] = {
	{
		.name = "Socket",
		.init = stream_server_socket_init,
		.destroy = stream_server_socket_destroy,
		.process = stream_server_socket_process,
		.flush = stream_server_socket_flush,
	},
	{
		.name = "MIPI",
		.init = stream_server_mipi_init,
		.destroy = stream_server_mipi_destroy,
		.process = stream_server_mipi_process,
		.flush = stream_server_mipi_flush,
	},
	{
		.name = "local",
		.init = NULL,
		.destroy = NULL,
		.process = stream_server_local_process,
		.flush = NULL,
	},
};

typedef struct {
	int select_sensor_id;
	uint32_t sensor_mode;
	pipe_contex_t pipe_contexts;
	int active_mipi_host; // 根据实际的硬件连接情况确定使用对应的 mipi host
	int vse_chn;
	int infer_vse_chn;
	sync_queue_info_t sync_queue_info;
	sync_queue_t cam_to_server;
} pipeline_info_t;

pipeline_info_t pipeline_info[MAX_PIPE_NUM] = {0};

typedef struct {
	char *name;
	int available;
	uint32_t width;
	uint32_t height;
	uint32_t size;
	uint32_t offset;
	uint32_t sub_node_num;
	uint32_t sub_node_size;
	uint8_t *data_item;
}stream_node_info_t;

typedef struct {
	int stream_server_type;			/* STREAM_SERVER_TYPE_E */
	char ifname[32];
	stream_header_t *stream_header;
	uint32_t total_size;
	uint64_t alloc_size;
	hb_mem_common_buf_t hb_mem_buf;
	uint8_t *stream_buffer;
	uint32_t data_offset;
	uint8_t *data_ptr;
	stream_node_info_t stream_nodes[STREAM_NODE_TYPE_NUM];
}server_stream_info_t;

static pthread_t stereonet_infer_thread;
static pthread_t stereonet_process_thread;
static pthread_t cam_rawdata_thread;
static pthread_t imu_data_thread;
static pthread_t server_data_thread;
static int32_t total_pipeline_num = 0;
static int32_t used_mipi_host = 0;

static int32_t bpu_flag = 0;
static int32_t gdc_flag = 0;
static int32_t calibrate_type = 0;   /* 标定参数保存方式: 0 - eeprom, 1 = yaml文件 */
static int32_t imu_flag = 0;

/* Debug DumpFile */
#define DUMP_FILE_DIR		"/tmp"
static uint32_t dumpfile_mode = 0;
#define DUMP_LR_RAW_NV12	(1<<0)
#define DUMP_DEPTH_FILE		(1<<1)
#define DUMP_DISP_FILE		(1<<2)
#define DUMP_INFER_LR_FILE	(1<<3)
// static uint32_t sensor_type = 0;
// static uint32_t link_port[MAX_PIPE_NUM] = {};

/* Debug Verbose */
static uint32_t verbose_mode = 0;
#define VERBOSE_STREAM				(1<<0)
#define VERBOSE_LR_RAW_NV12			(1<<1)
#define VERBOSE_INFER_DEPTH			(1<<2)
#define VERBOSE_IMU					(1<<3)

static int main_running = 0;
static int client_running = 0;
static int process_running = 0;
static int server_fd = -1;
static int client_fd = -1;

typedef struct {
    int buf[MAX_TENSOR_ID];
    int head;
    int tail;
    int count;
    pthread_mutex_t mutex;
    pthread_cond_t not_full;
    pthread_cond_t not_empty;
} tensor_queue_t;

static int g_stereo_running = 0;
static tensor_queue_t g_tensor_queue;
// #define STEREO_MULTI_THREAD_ENABLE
static sync_queue_t depth_to_server;
atomic_int depth_event_cnt = ATOMIC_VAR_INIT(0);

static server_stream_info_t server_stream_info = {0};

#define WAIT_FOR_SIGNAL(var) \
    do { \
        (var) = 1; \
        while ((var)) { \
            pause(); \
        } \
    } while (0)

static int server_stream_add_node(server_stream_info_t *server_stream_info, int type, void *data);
static int server_stream_flush(server_stream_info_t *server_stream_info);

static struct option const long_options[] = {
	{"config", required_argument, NULL, 'c'},
	{"interface", required_argument, NULL, 'i'},
	{"imu", required_argument, NULL, 'u'},
	{"gdc", required_argument, NULL, 'g'},
	{"verbose", required_argument, NULL, 'v'},
	{"dump", required_argument, NULL, 'd'},
	{"help", no_argument, NULL, 'h'},
	{NULL, 0, NULL, 0}
};

static void print_help(void)
{
	printf("Usage: %s [Options]\n", get_program_name());
	printf("Options:\n");
	printf("-i, --interface=\"Configure Server interface, Supports network interfaces or MIPI interfaces.\"\n");
	printf("\t\tUsing network interface: -i usb0\n");
	printf("\t\tUsing MIPI CSI-TX: -i mipi\n");
	printf("-c, --config=\"sensor=id\"\n");
	printf("\t\tConfigure parameters for each video pipeline, can be repeated up to %d times.\n", MAX_PIPE_NUM);
	printf("\t\tsensor   --  Sensor index,can have multiple parameters, reference sensor list.\n");
	printf("-g, --gdc=\"Enable gdc and calibration type\"\n");
	printf("\t\t0   --  Calibration in eeprom\n");
	printf("\t\t1   --  Calibration in yaml file\n");
	printf("-v, --verbose\tEnable verbose mode\n");
	printf("-h, --help\tShow help message\n");
	printf("Support sensor list:\n");
	printf("==========================================\n");
	vp_show_sensors_list();
}

int dumpToFile(char *filename, char *srcBuf, unsigned int size)
{
	FILE *fd = NULL;
	fd = fopen(filename, "w+");
	if (fd == NULL) {
		printf("ERRopen(%s) fail", filename);
		return -1;
	}

	fwrite(srcBuf, 1, size, fd);
	fflush(fd);

	if (fd)
		fclose(fd);
	printf("filedump(%s, size(%d) is successed\n", filename, size);
	return 0;
}

int readFile(char *filename, char *Buf, unsigned int size)
{
    FILE* file = NULL;

    file = fopen(filename, "rb");
    if (file == NULL) {
        printf("open file [%s] Failed\n", filename);
        return -1;
    }

    size_t bytes_read = fread(Buf, 1, size, file);
    if (bytes_read != size) {
        fclose(file);
        return -1;
    }

    fclose(file);

    printf("Read file (%s), size(%d) is successed\n", filename, size);
    return 0;
}

void signal_handle(int signo) {
	main_running = 0;
	client_running = 0;
	process_running = 0;
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

	for (int i = 0; i < count; i++) {
		char *key_value[2];
		int kv_count = split_string(parts[i], "=", key_value, 2);
		if (kv_count != 2) {
			fprintf(stderr, "Invalid format in config: %s\n", parts[i]);
			continue;
		}

		if (strcmp(key_value[0], "sensor") == 0) {
			if (!is_number(key_value[1])) {
				fprintf(stderr, "Invalid sensor ID: %s\n", key_value[1]);
				continue;
			}
			sensor_idx = atoi(key_value[1]);

			if (sensor_idx < vp_get_sensors_list_number() && sensor_idx >= 0) {
				pipeline_info->pipe_contexts.sensor_config = vp_sensor_config_list[sensor_idx];
				printf("Using index:%d  sensor_name:%s  config_file:%s\n",
						sensor_idx,
						vp_sensor_config_list[sensor_idx]->sensor_name,
						vp_sensor_config_list[sensor_idx]->config_file);
				// sensor_type = pipeline_info->pipe_contexts.sensor_config->sensor_type;
				// printf("sensor_type:%d \n" , sensor_type);
			} else {
				printf("Unsupport sensor index:%d\n", sensor_idx);
				print_help();
				exit(0);
			}
			//gmsl 模组需要初始化后才能检测到 addr
			// if(sensor_type == SENSOR_TYPE_NORMAL) 
			{
				ret = vp_sensor_multi_fixed_mipi_host(pipeline_info->pipe_contexts.sensor_config, used_mipi_host,
													&pipeline_info->pipe_contexts.csi_config);
				if (ret < 0) {
					printf("vp sensor fixed mipi host fail, sensor id %d."
						"Maybe No Camera Sensor found. Please check if the specified "
						"sensor is connected to the Camera interface.\n\n", sensor_idx);
					exit(0);
				}
				pipeline_info->select_sensor_id = sensor_idx;
				pipeline_info->active_mipi_host = pipeline_info->pipe_contexts.sensor_config->vin_node_attr->cim_attr.mipi_rx;
				used_mipi_host |= (1 << pipeline_info->pipe_contexts.sensor_config->vin_node_attr->cim_attr.mipi_rx);
			}
		} else {
			fprintf(stderr, "Unknown key: %s\n", key_value[0]);
		}

		for (int j = 0; j < kv_count; j++) {
			free(key_value[j]);
		}
	}

	for (int i = 0; i < count; i++) {
		free(parts[i]);
	}
}

int32_t vflow_fd_start(pipe_contex_t *pipe_contex)
{
	int32_t ret = 0;

	ret = hbn_vflow_start(pipe_contex->vflow_fd);
	ERR_CON_EQ(ret, 0);
	printf("hbn_vflow_start\n");
	return 0;
}

static int create_camera_node(pipe_contex_t *pipe_contex, uint32_t sensor_mode)
{
	camera_config_t *camera_config = NULL;
	vp_sensor_config_t *sensor_config = NULL;
	int32_t ret = 0;

	sensor_config = pipe_contex->sensor_config;
	camera_config = sensor_config->camera_config;
	if (sensor_mode >= NORMAL_M && sensor_mode < INVALID_MOD) {
		camera_config->sensor_mode = sensor_mode;
		sensor_config->vin_node_attr->lpwm_attr.enable = 1;
	}
	ret = hbn_camera_create(camera_config, &pipe_contex->cam_fd);
	ERR_CON_EQ(ret, 0);

	return 0;
}

static int create_vin_node(pipe_contex_t *pipe_contex, int active_mipi_host, int index) {
	vp_sensor_config_t *sensor_config = NULL;
	vin_node_attr_t *vin_node_attr = NULL;
	vin_ichn_attr_t *vin_ichn_attr = NULL;
	vin_ochn_attr_t *vin_ochn_attr = NULL;
	hbn_vnode_handle_t *vin_node_handle = NULL;
	hbn_buf_alloc_attr_t alloc_attr = {0};
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
	// 调整 mipi_rx 的 index
	vin_node_attr->cim_attr.mipi_rx = active_mipi_host;
	hw_id = vin_node_attr->cim_attr.mipi_rx;
	vin_node_handle = &pipe_contex->vin_node_handle;

	// link_port[index] = vin_node_attr->cim_attr.vc_index;
	if(pipe_contex->csi_config.mclk_is_not_configed){
		// 设备树中没有配置 mclk：使用外部晶振
		printf("csi%d ignore mclk ex attr, because not config mclk.\n",
			pipe_contex->csi_config.index);
	}else{
		vin_attr_ex.vin_attr_ex_mask = sensor_config->vin_attr_ex->vin_attr_ex_mask;
		vin_attr_ex.mclk_ex_attr.mclk_freq = sensor_config->vin_attr_ex->mclk_ex_attr.mclk_freq;
		vin_attr_ex_mask = vin_attr_ex.vin_attr_ex_mask;
	}

	// offline
	sensor_config->vin_node_attr->cim_attr.cim_isp_flyby = 0;
	sensor_config->vin_ochn_attr->ddr_en = 1;

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

	memset(&alloc_attr, 0, sizeof(hbn_buf_alloc_attr_t));
	alloc_attr.buffers_num = 6;
	alloc_attr.is_contig = 1;
	alloc_attr.flags =
		HB_MEM_USAGE_CPU_READ_OFTEN | HB_MEM_USAGE_CPU_WRITE_OFTEN | HB_MEM_USAGE_CACHED;
	ret = hbn_vnode_set_ochn_buf_attr(*vin_node_handle, ochn_id, &alloc_attr);
	if (ret < 0) {
		printf("hbn_vnode_set_ochn_buf_attr fail ret %d\n", ret);
		return ret;
	}

	return 0;
}

static int create_isp_node(pipe_contex_t *pipe_contex) {
	vp_sensor_config_t *sensor_config = NULL;
	isp_attr_t      *isp_attr = NULL;
	isp_ichn_attr_t *isp_ichn_attr = NULL;
	isp_ochn_attr_t *isp_ochn_attr = NULL;
	hbn_vnode_handle_t *isp_node_handle = NULL;
	hbn_buf_alloc_attr_t alloc_attr = {0};
	uint32_t ichn_id = 0;
	uint32_t ochn_id = 0;
	int ret = 0;

	sensor_config = pipe_contex->sensor_config;
	isp_attr = sensor_config->isp_attr;
	isp_ichn_attr = sensor_config->isp_ichn_attr;
	isp_ochn_attr = sensor_config->isp_ochn_attr;
	isp_node_handle = &pipe_contex->isp_node_handle;

	ret = hbn_vnode_open(HB_ISP, 0, AUTO_ALLOC_ID, isp_node_handle);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_attr(*isp_node_handle, isp_attr);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_ochn_attr(*isp_node_handle, ochn_id, isp_ochn_attr);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_ichn_attr(*isp_node_handle, ichn_id, isp_ichn_attr);
	ERR_CON_EQ(ret, 0);

	alloc_attr.buffers_num = 8;
	alloc_attr.is_contig = 1;
	alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN
						| HB_MEM_USAGE_CPU_WRITE_OFTEN
						| HB_MEM_USAGE_CACHED;
	ret = hbn_vnode_set_ochn_buf_attr(*isp_node_handle, ochn_id, &alloc_attr);
	ERR_CON_EQ(ret, 0);

	return 0;
}

/* 注意: 增加 GDC 后图像旋转 分辨率为1280x1088 */
static int create_vse_node(pipe_contex_t *pipe_contex, int vse_bind_index, int infer_vse_chn) {
	int ret = 0;
	hbn_vnode_handle_t *vse_node_handle = &pipe_contex->vse_node_handle;
	isp_ichn_attr_t isp_ichn_attr = {0};
	vse_attr_t vse_attr = {0};
	vse_ichn_attr_t vse_ichn_attr = {0};
	vse_ochn_attr_t vse_ochn_attr[VSE_MAX_CHANNELS] = {0};
	// vse_ochn_attr_ex_t vse_ochn_attr_ex = {0};
	uint32_t ichn_id = 0;
	uint32_t hw_id = 0;
	uint32_t input_width = 0, input_height = 0;
	// uint32_t output_width = 0, output_height = 0;
	hbn_buf_alloc_attr_t alloc_attr = {0};

	ret = hbn_vnode_get_ichn_attr(pipe_contex->isp_node_handle, ichn_id, &isp_ichn_attr);
	ERR_CON_EQ(ret, 0);
	input_width = isp_ichn_attr.width;
	input_height = isp_ichn_attr.height;

	if (gdc_flag) {
		vse_ichn_attr.width = input_height;
		vse_ichn_attr.height = input_width;
	}
	else {
		vse_ichn_attr.width = input_width;
		vse_ichn_attr.height = input_height;
	}
	vse_ichn_attr.fmt = FRM_FMT_NV12;
	vse_ichn_attr.bit_width = 8;

	// 输出原分辨率
	vse_ochn_attr[vse_bind_index].chn_en = CAM_TRUE;
	vse_ochn_attr[vse_bind_index].roi.x = 0;
	vse_ochn_attr[vse_bind_index].roi.y = 0;

	if (gdc_flag) {
		vse_ochn_attr[vse_bind_index].roi.w = input_height;
		vse_ochn_attr[vse_bind_index].roi.h = input_width;
		vse_ochn_attr[vse_bind_index].target_w = input_height;
		vse_ochn_attr[vse_bind_index].target_h = input_width;
	}
	else {
		vse_ochn_attr[vse_bind_index].roi.w = input_width;
		vse_ochn_attr[vse_bind_index].roi.h = input_height;
		vse_ochn_attr[vse_bind_index].target_w = input_width;
		vse_ochn_attr[vse_bind_index].target_h = input_height;
	}
	vse_ochn_attr[vse_bind_index].fmt = FRM_FMT_NV12;
	vse_ochn_attr[vse_bind_index].bit_width = 8;

	// vse_ochn_attr[vse_bind_index].fps.src = 60;
	// vse_ochn_attr[vse_bind_index].fps.dst = 30;

	// 输出 640 x 352
	vse_ochn_attr[infer_vse_chn].chn_en = CAM_TRUE;
	vse_ochn_attr[infer_vse_chn].roi.x = 0;
	vse_ochn_attr[infer_vse_chn].roi.y = 0;

	if (gdc_flag) {
		vse_ochn_attr[infer_vse_chn].roi.w = input_height;
		vse_ochn_attr[infer_vse_chn].roi.h = input_width;
	}
	else {
		vse_ochn_attr[infer_vse_chn].roi.w = input_width;
		vse_ochn_attr[infer_vse_chn].roi.h = input_height;
	}
	vse_ochn_attr[infer_vse_chn].fmt = FRM_FMT_NV12;
	vse_ochn_attr[infer_vse_chn].bit_width = 8;
	vse_ochn_attr[infer_vse_chn].target_w = STEREO_RES_WIDTH;
	vse_ochn_attr[infer_vse_chn].target_h = STEREO_RES_HEIGHT;
	// vse_ochn_attr[infer_vse_chn].fps.src = 60;
	// vse_ochn_attr[infer_vse_chn].fps.dst = 30;

	ret = hbn_vnode_open(HB_VSE, hw_id, AUTO_ALLOC_ID, vse_node_handle);
	ERR_CON_EQ(ret, 0);

	ret = hbn_vnode_set_attr(*vse_node_handle, &vse_attr);
	ERR_CON_EQ(ret, 0);

	ret = hbn_vnode_set_ichn_attr(*vse_node_handle, ichn_id, &vse_ichn_attr);
	ERR_CON_EQ(ret, 0);

	alloc_attr.buffers_num = 10;
	alloc_attr.is_contig = 1;
	alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN | HB_MEM_USAGE_CPU_WRITE_OFTEN | HB_MEM_USAGE_CACHED;

	// 原分辨率
	printf("chn[%d] hbn_vnode_set_ochn_attr: %dx%d\n", vse_bind_index, vse_ochn_attr[vse_bind_index].target_w,
		vse_ochn_attr[vse_bind_index].target_h);
	ret = hbn_vnode_set_ochn_attr(*vse_node_handle, vse_bind_index, &vse_ochn_attr[vse_bind_index]);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_ochn_buf_attr(*vse_node_handle, vse_bind_index, &alloc_attr);
	ERR_CON_EQ(ret, 0);

	// 640 x 352
	printf("chn[%d] hbn_vnode_set_ochn_attr: %dx%d\n", infer_vse_chn, vse_ochn_attr[infer_vse_chn].target_w,
		vse_ochn_attr[infer_vse_chn].target_h);
	ret = hbn_vnode_set_ochn_attr(*vse_node_handle, infer_vse_chn, &vse_ochn_attr[infer_vse_chn]);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_ochn_buf_attr(*vse_node_handle, infer_vse_chn, &alloc_attr);
	ERR_CON_EQ(ret, 0);

	return 0;
}


static int get_gdc_config(const char *gdc_bin_file, hb_mem_common_buf_t *bin_buf) {
	int64_t alloc_flags = 0;
	int ret = 0;
	int offset = 0;
	char *cfg_buf = NULL;

	FILE *fp = fopen(gdc_bin_file, "r");
	if (fp == NULL) {
		printf("File %s open failed\n", gdc_bin_file);
		return -1;
	}
	fseek(fp, 0, SEEK_END);
	long file_size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	cfg_buf = malloc(file_size);
	int n = fread(cfg_buf, 1, file_size, fp);
	if (n != file_size) {
        free(cfg_buf);
		printf("Read file size failed\n");
        fclose(fp);
        return -1;
	}
	fclose(fp);

	memset(bin_buf, 0, sizeof(hb_mem_common_buf_t));
	alloc_flags = HB_MEM_USAGE_MAP_INITIALIZED | HB_MEM_USAGE_PRIV_HEAP_2_RESERVERD | HB_MEM_USAGE_CPU_READ_OFTEN |
				HB_MEM_USAGE_CPU_WRITE_OFTEN | HB_MEM_USAGE_CACHED;
	ret = hb_mem_alloc_com_buf(file_size, alloc_flags, bin_buf);
	if (ret != 0 || bin_buf->virt_addr == NULL) {
        free(cfg_buf);
		printf("hb_mem_alloc_com_buf for bin failed, ret = %d\n", ret);
		return -1;
	}

	memcpy(bin_buf->virt_addr, cfg_buf, file_size);
	ret = hb_mem_flush_buf(bin_buf->fd, offset, file_size);
	if (ret != 0 || bin_buf->virt_addr == NULL) {
        free(cfg_buf);
		printf("hb_mem_flush_buf for bin failed, ret = %d\n", ret);
		return -1;
	}

	free(cfg_buf);

	return ret;
}

static int create_gdc_node(pipe_contex_t *pipe_contex, char *sensor_name, int index) {
	int ret = 0;
	isp_ichn_attr_t isp_ichn_attr = {0};

	ret = hbn_vnode_get_ichn_attr(pipe_contex->isp_node_handle, 0, &isp_ichn_attr);
	ERR_CON_EQ(ret, 0);
	int input_width = isp_ichn_attr.width;
	int input_height = isp_ichn_attr.height;

#if 0
    const char* gdc_bin_file = vp_gdc_get_bin_file(sensor_name);
	if(gdc_bin_file == NULL){
		printf("%s is enable gdc, but gdc bin file is not set.\n", sensor_name);
		return -1;
	}
#else
	char* gdc_bin_file = NULL;
	if (index == RIGHT_CAM_CHN) {
		gdc_bin_file = RIGHT_CAM_GDC_BIN_PATH;
	}
	else {
		gdc_bin_file = LEFT_CAM_GDC_BIN_PATH;
	}
#endif
    ret = get_gdc_config(gdc_bin_file, &pipe_contex->bin_buf);
	if(ret != 0){
		printf("%s is enable gdc, but gdc bin file [%s] is not valid.\n",
			sensor_name, gdc_bin_file);
		return -1;
	}
	printf("gdc input resolution: %d*%d, camera name [%s], gdc bin [%s]\n", input_width, input_height, sensor_name, gdc_bin_file);
	uint32_t hw_id = 0;
	ret = hbn_vnode_open(HB_GDC, hw_id, AUTO_ALLOC_ID, &(pipe_contex->gdc_node_handle));
	if(ret != 0){
		printf("%s is enable gdc and gdc bin file [%s] is valid, but open failed %d.\n",
			sensor_name, gdc_bin_file, ret);
		return -1;
	}
	gdc_attr_t gdc_attr = {0};
	gdc_attr.config_addr = pipe_contex->bin_buf.phys_addr;
	gdc_attr.config_size = pipe_contex->bin_buf.size;
	gdc_attr.binary_ion_id = pipe_contex->bin_buf.share_id;
	gdc_attr.binary_offset = pipe_contex->bin_buf.offset;
	gdc_attr.total_planes = 2;
	gdc_attr.div_width = 0;
	gdc_attr.div_height = 0;
	ret = hbn_vnode_set_attr(pipe_contex->gdc_node_handle, &gdc_attr);
	if(ret != 0){
		printf("%s is enable gdc and gdc bin file [%s] is valid, but set attr failed %d.\n",
			sensor_name, gdc_bin_file, ret);
		return -1;
	}

	uint32_t chn_id = 0;

	gdc_ichn_attr_t gdc_ichn_attr = {0};
	gdc_ichn_attr.input_width = input_width;
	gdc_ichn_attr.input_height = input_height;
	gdc_ichn_attr.input_stride = input_width;
	ret = hbn_vnode_set_ichn_attr(pipe_contex->gdc_node_handle, chn_id, &gdc_ichn_attr);
	if(ret != 0){
		printf("%s is enable gdc and gdc bin file [%s] is valid, but set ichn failed %d.\n",
			sensor_name, gdc_bin_file, ret);
		return -1;
	}

	gdc_ochn_attr_t gdc_ochn_attr = {0};
#if 0
	gdc_ochn_attr.output_width = input_width;
	gdc_ochn_attr.output_height = input_height;
	gdc_ochn_attr.output_stride = input_width;
#else
	gdc_ochn_attr.output_width = input_height;
	gdc_ochn_attr.output_height = input_width;
	gdc_ochn_attr.output_stride = input_height;
#endif
	ret = hbn_vnode_set_ochn_attr(pipe_contex->gdc_node_handle, chn_id, &gdc_ochn_attr);
	if(ret != 0){
		printf("%s is enable gdc and gdc bin file [%s] is valid, but set ochn failed %d.\n",
			sensor_name, gdc_bin_file, ret);
		return -1;
	}
	hbn_buf_alloc_attr_t alloc_attr = {0};
	alloc_attr.buffers_num = 5;
	alloc_attr.is_contig = 1;
	alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN | HB_MEM_USAGE_CPU_WRITE_OFTEN
		| HB_MEM_USAGE_CACHED |HB_MEM_USAGE_GRAPHIC_CONTIGUOUS_BUF;

	ret = hbn_vnode_set_ochn_buf_attr(pipe_contex->gdc_node_handle, chn_id, &alloc_attr);
	if(ret != 0){
		printf("%s is enable gdc and gdc bin file [%s] is valid, but set ochn buffer failed %d.\n",
			sensor_name, gdc_bin_file, ret);
		return -1;
	}

    return 0;
}

static int create_and_run_vflow(pipe_contex_t *pipe_contex,
	int active_mipi_host, int vse_index, int infer_vse_chn, uint32_t sensor_mode, int index)
{
	int32_t ret = 0;

	// 创建 pipeline 中的每个 node
	ret = create_camera_node(pipe_contex, sensor_mode);
	ERR_CON_EQ(ret, 0);
	ret = create_vin_node(pipe_contex, active_mipi_host, index);
	ERR_CON_EQ(ret, 0);
	ret = create_isp_node(pipe_contex);
	ERR_CON_EQ(ret, 0);

	if (gdc_flag) {
		ret = create_gdc_node(pipe_contex, pipe_contex->sensor_config->sensor_name, index);
		ERR_CON_EQ(ret, 0);
	}

	ret = create_vse_node(pipe_contex, vse_index, infer_vse_chn);
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

	if(gdc_flag){
		ret = hbn_vflow_add_vnode(pipe_contex->vflow_fd,
								pipe_contex->gdc_node_handle);
		ERR_CON_EQ(ret, 0);
	}

	ret = hbn_vflow_add_vnode(pipe_contex->vflow_fd,
							pipe_contex->vse_node_handle);
	ERR_CON_EQ(ret, 0);
	// VIN offline ISP
	ret = hbn_vflow_bind_vnode(pipe_contex->vflow_fd,
							pipe_contex->vin_node_handle,
							0,
							pipe_contex->isp_node_handle,
							0);
	ERR_CON_EQ(ret, 0);

	if (gdc_flag) {
		ret = hbn_vflow_bind_vnode(pipe_contex->vflow_fd,
			pipe_contex->isp_node_handle,
			0,
			pipe_contex->gdc_node_handle,
			0);
		ERR_CON_EQ(ret, 0);

		ret = hbn_vflow_bind_vnode(pipe_contex->vflow_fd,
			pipe_contex->gdc_node_handle,
			0,
			pipe_contex->vse_node_handle,
			0);
		ERR_CON_EQ(ret, 0);
	}
	else {
		ret = hbn_vflow_bind_vnode(pipe_contex->vflow_fd,
			pipe_contex->isp_node_handle,
			0,
			pipe_contex->vse_node_handle,
			0);
		ERR_CON_EQ(ret, 0);
	}

	ret = hbn_camera_attach_to_vin(pipe_contex->cam_fd,
							pipe_contex->vin_node_handle);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_start(pipe_contex->vflow_fd);
	ERR_CON_EQ(ret, 0);

	return 0;
}

static int cam_vse_frame_cleanup(void *param0, void *param1)
{
	// sync_queue_t *sync_queue = (sync_queue_t *)param0;
	pipeline_info_t *pipeline_info = (pipeline_info_t *)param0;
	data_item_t *data_item = (data_item_t *)param1;
	hbn_vnode_image_t *vse_frame = (hbn_vnode_image_t*)data_item->items;
	// pipeline_info_t *pipeline_info = container_of(sync_queue, pipeline_info_t, cam_to_server);

	printf("cam_vse_frame_cleanup: [%ld] vse[%d]\n", pipeline_info->pipe_contexts.vse_node_handle, pipeline_info->vse_chn);

	hbn_vnode_releaseframe(pipeline_info->pipe_contexts.vse_node_handle, pipeline_info->vse_chn, vse_frame);

	printf("End\n");

	return 0;
}

static inline void tensor_queue_init(tensor_queue_t *q) {
	memset(q, 0, sizeof(*q));
	pthread_mutex_init(&q->mutex, NULL);
	pthread_cond_init(&q->not_full, NULL);
	pthread_cond_init(&q->not_empty, NULL);
}

static inline int tensor_queue_push(tensor_queue_t *q, int tensor_id) {
	pthread_mutex_lock(&q->mutex);
	while (q->count == MAX_TENSOR_ID && g_stereo_running) {
		pthread_cond_wait(&q->not_full, &q->mutex);
	}
	q->buf[q->tail] = tensor_id;
	q->tail = (q->tail + 1) % MAX_TENSOR_ID;
	q->count++;
	pthread_cond_signal(&q->not_empty);
	pthread_mutex_unlock(&q->mutex);
	return 0;
}

static inline int tensor_queue_pop(tensor_queue_t *q, int *tensor_id) {
	pthread_mutex_lock(&q->mutex);
	while (q->count == 0 && g_stereo_running) {
		pthread_cond_wait(&q->not_empty, &q->mutex);
	}
	*tensor_id = q->buf[q->head];
	q->head = (q->head + 1) % MAX_TENSOR_ID;
	q->count--;
	pthread_cond_signal(&q->not_full);
	pthread_mutex_unlock(&q->mutex);
	return 0;
}

static int do_stereonet_post_process(int tensor_id)
{
	int ret = -1;
	static int count = 0;
	char depth_file[64] = {0};
	char disp_file[64] = {0};
	data_item_t *data_item = NULL;

	ret = sync_queue_get_unused_object(&depth_to_server, 2000, &data_item);
	if(ret != 0){
		printf("sync_queue_get_unused_object Failed\n");
		return -1;
	}

	// Post-process
	stereo_process(tensor_id, data_item->items);
	if (verbose_mode & VERBOSE_INFER_DEPTH)
			printf("Stereo Post Process: tensor_id = %d <<<\n", tensor_id);

	ret = sync_queue_save_inused_object(&depth_to_server, 2000, data_item);
	if(ret != 0){
		printf("sync_queue_save_inused_object Failed\n");
		return -1;
	}
	atomic_fetch_add(&depth_event_cnt, 1);

	if (count++ % 100 == 0) {
		if (dumpfile_mode & DUMP_DEPTH_FILE) {
			#if 0
			// Dump Depth File : PNG Format
			sprintf(depth_file, "depth_server_cnt[%d].png", count);
			stereo_infer_dump_depth(depth_file);
			#else
			sprintf(depth_file, "%s/depth_server_cnt[%d].bin", DUMP_FILE_DIR, count);
			dumpToFile(depth_file, data_item->items, STEREO_RES_WIDTH * STEREO_RES_HEIGHT * 2);
			#endif
			printf("[Server] Infer Dump file: %s\n", depth_file);
		}
		if (dumpfile_mode & DUMP_DISP_FILE) {
			sprintf(disp_file, "%s/disp_server_cnt[%d].pfm", DUMP_FILE_DIR, count);
			stereo_infer_dump_disp(disp_file);
			printf("[Server] Infer Disp file: %s\n", disp_file);
		}
	}
	return 0;
}

void *stereonet_infer_process(void *context)
{
	int ret = 0;
	pipeline_info_t *pipeline_info_list = (pipeline_info_t *)context;

	pipeline_info_t *right_pipeline_info = (pipeline_info_t *)&pipeline_info_list[RIGHT_CAM_CHN];
	pipe_contex_t *right_pipe_context = (pipe_contex_t *)&right_pipeline_info->pipe_contexts;
	hbn_vnode_handle_t right_vse_handle = right_pipe_context->vse_node_handle;
	hbn_vnode_image_t right_vse_frame = {0};
	int right_vse_chn = right_pipeline_info->infer_vse_chn;

	pipeline_info_t *left_pipeline_info = (pipeline_info_t *)&pipeline_info_list[LEFT_CAM_CHN];
	pipe_contex_t *left_pipe_context = (pipe_contex_t *)&left_pipeline_info->pipe_contexts;
	hbn_vnode_handle_t left_vse_handle = left_pipe_context->vse_node_handle;
	hbn_vnode_image_t left_vse_frame = {0};
	int left_vse_chn = left_pipeline_info->infer_vse_chn;

#ifdef DEBUG_INFER_TIME_CONSUM
	struct timeval start, end;
	double elapsed_time;
#endif

	void *right_nv12 = NULL;
	void *left_nv12 = NULL;
	int right_done = 0, left_done = 0;

#ifdef DEBUG_INTFER_DUMP_FLLE
	char left_file[64] = {0};
	char right_file[64] = {0};
	int count = 0;
#endif

	uint64_t diff_pts;

#ifndef STEREO_MULTI_THREAD_ENABLE
	struct PerformanceTestParamSimple performace_total_stereo_infer = {
		.iteration_number = 30 * 60,
		.test_case = "stereo_infer_depth",
		.run_count = 0,
		.test_count = 0,
	};
#endif

	prctl(PR_SET_NAME, "stereonet_infer_thread");
	printf("[Thread] stereonet_infer_process ==> Start\n");

	while (g_stereo_running) {
		// 10fps test
		// hbn_vnode_getframe(right_vse_handle, right_vse_chn, 2000, &right_vse_frame);
		// hbn_vnode_releaseframe(right_vse_handle, right_vse_chn, &right_vse_frame);
		// hbn_vnode_getframe(left_vse_handle, left_vse_chn, 2000, &left_vse_frame);
		// hbn_vnode_releaseframe(left_vse_handle, left_vse_chn, &left_vse_frame);

		// hbn_vnode_getframe(right_vse_handle, right_vse_chn, 2000, &right_vse_frame);
		// hbn_vnode_releaseframe(right_vse_handle, right_vse_chn, &right_vse_frame);
		// hbn_vnode_getframe(left_vse_handle, left_vse_chn, 2000, &left_vse_frame);
		// hbn_vnode_releaseframe(left_vse_handle, left_vse_chn, &left_vse_frame);

infer_get_frame:
		/*Get Right Frame */
		if (right_done == 0) {
			ret = hbn_vnode_getframe(right_vse_handle, right_vse_chn, 2000, &right_vse_frame);
			if (ret != 0) {
				printf("hbn_vnode_getframe VSE channel %d failed, error code %d\n", right_vse_chn, ret);
				goto stereonet_thread_err0;
			}
			right_done = 1;
			right_nv12 = right_vse_frame.buffer.virt_addr[0];
		}

		/*Get Left Frame */
		if (left_done == 0) {
			ret = hbn_vnode_getframe(left_vse_handle, left_vse_chn, 2000, &left_vse_frame);
			if (ret != 0) {
				printf("hbn_vnode_getframe VSE channel %d failed, error code %d\n", left_vse_chn, ret);
				goto stereonet_thread_err0;
			}
			left_done = 1;
			left_nv12 = left_vse_frame.buffer.virt_addr[0];
		}

		diff_pts = abs(right_vse_frame.info.timestamps - left_vse_frame.info.timestamps);
		if (diff_pts >= 10000000) {
			if (right_vse_frame.info.timestamps < left_vse_frame.info.timestamps) {
				right_done = 0;
				hbn_vnode_releaseframe(right_vse_handle, right_vse_chn, &right_vse_frame);
				if (verbose_mode & VERBOSE_INFER_DEPTH) {
					printf("Drop Right Infer Data: Right[%ld] Left[%ld] Diff[%ld]\n", right_vse_frame.info.timestamps, left_vse_frame.info.timestamps, diff_pts);
				}
				goto infer_get_frame;
			}
			else {
				left_done = 0;
				hbn_vnode_releaseframe(left_vse_handle, left_vse_chn, &left_vse_frame);
				if (verbose_mode & VERBOSE_INFER_DEPTH) {
					printf("Drop Left Infer Data: Right[%ld] Left[%ld] Diff[%ld]\n", right_vse_frame.info.timestamps, left_vse_frame.info.timestamps, diff_pts);
				}
				goto infer_get_frame;
			}
		}

		/* Strero */
		if (right_done && left_done && bpu_flag) {

		#ifdef DEBUG_INFER_TIME_CONSUM
			gettimeofday(&start, NULL);
		#endif

			if (verbose_mode & VERBOSE_INFER_DEPTH) {
				printf("Right Infer: Get Frame VSE-Chn[%d] PTS[%ld] Frameid[%d]\n", right_vse_chn, right_vse_frame.info.timestamps, right_vse_frame.info.frame_id);
				printf("Left Infer: Get Frame VSE-Chn[%d] PTS[%ld] Frameid[%d]\n", left_vse_chn, left_vse_frame.info.timestamps, left_vse_frame.info.frame_id);
			}


			/* Stereo Infer */
			int tensor_id_tmp = -1;
			if (0 != stereo_infer(left_nv12, right_nv12, &tensor_id_tmp)) {
				printf("stereo_infer_process Failed \n");
				goto stereonet_thread_err0;
			}
			if (verbose_mode & VERBOSE_INFER_DEPTH)
				printf("Stereo Infer: tensor id = %d >>>\n", tensor_id_tmp);

		#ifdef STEREO_MULTI_THREAD_ENABLE
			// 通知后处理线程
			tensor_queue_push(&g_tensor_queue, tensor_id_tmp);
			if (!g_stereo_running) {
				goto stereonet_thread_err0;
			}
		#else
			performance_test_start_simple(&performace_total_stereo_infer);
			do_stereonet_post_process(tensor_id_tmp);
			performance_test_stop_simple(&performace_total_stereo_infer);
		#endif

		#ifdef DEBUG_INFER_TIME_CONSUM
			gettimeofday(&end, NULL);
			elapsed_time = (end.tv_sec - start.tv_sec) +
						(end.tv_usec - start.tv_usec) / 1000000.0;
			printf("stereo_infer_process: PTS = %ld\n", end.tv_sec * 1000000 + end.tv_usec);
			printf("stereo_infer_process: Cast Time %.9f s.\n", elapsed_time);
		#endif

		#ifdef DEBUG_INTFER_DUMP_FLLE
			if (count++ % 100 == 0) {
				if (dumpfile_mode & DUMP_INFER_LR_FILE) {
					sprintf(left_file, "%s/left_infer_nv12_cnt[%d].yuv", DUMP_FILE_DIR, count);
					sprintf(right_file, "%s/right_infer_nv12_cnt[%d].yuv", DUMP_FILE_DIR, count);
					dumpToFile(left_file, left_nv12, STEREO_RES_WIDTH * STEREO_RES_HEIGHT * 3 / 2);
					dumpToFile(right_file, right_nv12, STEREO_RES_WIDTH * STEREO_RES_HEIGHT * 3 / 2);
				}
			}
		#endif
		}

	stereonet_thread_err0:
		if (right_done) {
			hbn_vnode_releaseframe(right_vse_handle, right_vse_chn, &right_vse_frame);
			right_done = 0;
		}
		if (left_done) {
			hbn_vnode_releaseframe(left_vse_handle, left_vse_chn, &left_vse_frame);
			left_done = 0;
		}

	}

	client_running = 0;
	printf("[Thread] stereonet_infer_process ==> Quit\n");
	// vse_sync_queue_cleanup(pipeline_info_list);
	return NULL;
}

void *stereonet_post_process(void *context)
{
	int tensor_id = -1;
	struct PerformanceTestParamSimple performace_total_stereo_infer = {
		.iteration_number = 30 * 60,
		.test_case = "stereo_infer_depth",
		.run_count = 0,
		.test_count = 0,
	};

	prctl(PR_SET_NAME, "stereonet_process_thread");
	printf("[Thread] stereonet_post_process ==> Start\n");

	while (g_stereo_running) {

		performance_test_start_simple(&performace_total_stereo_infer);

		tensor_queue_pop(&g_tensor_queue, &tensor_id);
		if (!g_stereo_running)
			break;
		do_stereonet_post_process(tensor_id);

		performance_test_stop_simple(&performace_total_stereo_infer);
	}

	client_running = 0;
	printf("[Thread] stereonet_post_process ==> Quit\n");
	// vse_sync_queue_cleanup(pipeline_info_list);
	return NULL;
}

static int stereonet_infer_start(void)
{
	g_stereo_running = 1;
#ifdef STEREO_MULTI_THREAD_ENABLE
	tensor_queue_init(&g_tensor_queue);
	pthread_create(&stereonet_process_thread, NULL, (void *)stereonet_post_process, (void *)&pipeline_info);
#endif
	pthread_create(&stereonet_infer_thread, NULL, (void *)stereonet_infer_process, (void *)&pipeline_info);
	return 0;
}

static void stereonet_infer_stop(void)
{
	if (stereonet_infer_thread > 0 || stereonet_process_thread > 0) {

		g_stereo_running = 0;
#ifdef STEREO_MULTI_THREAD_ENABLE
		pthread_cond_broadcast(&g_tensor_queue.not_empty);
		pthread_cond_broadcast(&g_tensor_queue.not_full);
		pthread_join(stereonet_process_thread, NULL);
#endif
		pthread_join(stereonet_infer_thread, NULL);
		stereonet_infer_thread = 0;
		stereonet_process_thread = 0;
	}
}


static int cam_sync(pipeline_info_t *pipeline_info_list)
{
	int ret;
	pipeline_info_t *right_pipeline_info = (pipeline_info_t *)&pipeline_info_list[RIGHT_CAM_CHN];
	pipe_contex_t *right_pipe_context = (pipe_contex_t *)&right_pipeline_info->pipe_contexts;
	hbn_vnode_handle_t right_vse_handle = right_pipe_context->vse_node_handle;
	hbn_vnode_image_t right_vse_frame = {0};
	int right_vse_chn = right_pipeline_info->vse_chn;

	pipeline_info_t *left_pipeline_info = (pipeline_info_t *)&pipeline_info_list[LEFT_CAM_CHN];
	pipe_contex_t *left_pipe_context = (pipe_contex_t *)&left_pipeline_info->pipe_contexts;
	hbn_vnode_handle_t left_vse_handle = left_pipe_context->vse_node_handle;
	hbn_vnode_image_t left_vse_frame = {0};
	int left_vse_chn = left_pipeline_info->vse_chn;

	int right_done = 0, left_done = 0;
	uint64_t diff_pts;

	int do_sync = 1;

	while (do_sync) {
	sync_get_frame:
		/*Get Right Frame */
		if (right_done == 0) {
			ret = hbn_vnode_getframe(right_vse_handle, right_vse_chn, 2000, &right_vse_frame);
			if (ret != 0) {
				printf("hbn_vnode_getframe VSE channel %d failed, error code %d\n", right_vse_chn, ret);
				goto sync_err0;
			}
			right_done = 1;
		}

		/*Get Left Frame */
		if (left_done == 0) {
			ret = hbn_vnode_getframe(left_vse_handle, left_vse_chn, 2000, &left_vse_frame);
			if (ret != 0) {
				printf("hbn_vnode_getframe VSE channel %d failed, error code %d\n", left_vse_chn, ret);
				goto sync_err0;
			}
			left_done = 1;
		}

		diff_pts = abs(right_vse_frame.info.timestamps - left_vse_frame.info.timestamps);
		if (diff_pts >= 1000000) {
			if (right_vse_frame.info.timestamps < left_vse_frame.info.timestamps) {
				right_done = 0;
				hbn_vnode_releaseframe(right_vse_handle, right_vse_chn, &right_vse_frame);
				goto sync_get_frame;
			}
			else {
				left_done = 0;
				hbn_vnode_releaseframe(left_vse_handle, left_vse_chn, &left_vse_frame);
				goto sync_get_frame;
			}
		}
		else {
			do_sync = 0;
		}

	sync_err0:
		if (right_done) {
			hbn_vnode_releaseframe(right_vse_handle, right_vse_chn, &right_vse_frame);
			right_done = 0;
		}
		if (left_done) {
			hbn_vnode_releaseframe(left_vse_handle, left_vse_chn, &left_vse_frame);
			left_done = 0;
		}

	}
	return 0;
}

void *cam_rawdata_process(void *context)
{
	int ret = 0;
	int index;
	pipeline_info_t *pipeline_info_list = (pipeline_info_t *)context;

	pipeline_info_t *pipeline_info = NULL;
	pipe_contex_t *pipe_context = NULL;
	hbn_vnode_handle_t vse_handle;
	// hbn_vnode_image_t vse_frame_test = {0};

	int vse_chn = 0;
	data_item_t *data_item = NULL;

	char filename[128] = {0};
	int dump_right_cnt = 0, dump_left_cnt = 0;

	struct PerformanceTestParamSimple performace_total_cam_raw_right = {
		.iteration_number = 30 * 60,
		.test_case = "cam_raw_right",
		.run_count = 0,
		.test_count = 0,
	};
	struct PerformanceTestParamSimple performace_total_cam_raw_left = {
		.iteration_number = 30 * 60,
		.test_case = "cam_raw_left",
		.run_count = 0,
		.test_count = 0,
	};

	prctl(PR_SET_NAME, "rawdata_process");
	printf("[Thread] rawdata_process ==> Start\n");

	cam_sync(pipeline_info_list);

	while (client_running) {
		for (index = MAX_PIPE_NUM - 1; index >= 0; index--){

			pipeline_info = (pipeline_info_t *)&pipeline_info_list[index];
			pipe_context = (pipe_contex_t *)&pipeline_info->pipe_contexts;
			sync_queue_t *cam_to_server = (sync_queue_t *)&pipeline_info->cam_to_server;
			vse_handle = pipe_context->vse_node_handle;
			vse_chn = pipeline_info->vse_chn;

			if (index == RIGHT_CAM_CHN)
				performance_test_start_simple(&performace_total_cam_raw_right);
			else if (index == LEFT_CAM_CHN)
				performance_test_start_simple(&performace_total_cam_raw_left);

			ret = sync_queue_get_unused_object(cam_to_server, 2000, &data_item);
			if(ret != 0){
				printf("sync_queue_get_unused_object Failed: %d\n", index);
			}

			hbn_vnode_image_t *vse_frame = ((hbn_vnode_image_t*)data_item->items);
			memset(vse_frame, 0, sizeof(hbn_vnode_image_t));

			/*Get Frame */
			ret = hbn_vnode_getframe(vse_handle, vse_chn, 2000, vse_frame);
			if (ret != 0) {
				printf("hbn_vnode_getframe VSE channel %d failed, error code %d\n", 0, ret);
				continue;
			}

			if (verbose_mode & VERBOSE_LR_RAW_NV12) {
				printf("%s Cam Raw: Get Frame VSE-Chn[%d] PTS[%ld] Frameid[%d]\n", (index == 0)? "Right":"Left", vse_chn, vse_frame->info.timestamps, vse_frame->info.frame_id);
			}

			ret = sync_queue_save_inused_object(cam_to_server, 2000, data_item);
			if(ret != 0){
				printf("sync_queue_save_inused_object Failed: %d\n", index);
			}

			if (index == RIGHT_CAM_CHN)
				performance_test_stop_simple(&performace_total_cam_raw_right);
			else if (index == LEFT_CAM_CHN)
				performance_test_stop_simple(&performace_total_cam_raw_left);

			/* Dump NV12 */
			if (dumpfile_mode & DUMP_LR_RAW_NV12) {
				if (index == RIGHT_CAM_CHN) {
					dump_right_cnt++;
				}
				else {
					dump_left_cnt++;
				}
				if (dump_right_cnt >= 100) {
					dump_right_cnt = 0;
					snprintf(filename, 128, "%s/Right_Raw_%d_pts[%ld]_fid[%d].yuv", DUMP_FILE_DIR, dump_right_cnt, vse_frame->info.timestamps, vse_frame->info.frame_id);
					dumpToFile(filename, (char *)vse_frame->buffer.virt_addr[0], vse_frame->buffer.width * vse_frame->buffer.height * 3 / 2);
				}
				if (dump_left_cnt >= 100) {
					dump_left_cnt = 0;
					snprintf(filename, 128, "%s/Left_Raw_%d_pts[%ld]_fid[%d].yuv", DUMP_FILE_DIR, dump_left_cnt, vse_frame->info.timestamps, vse_frame->info.frame_id);
					dumpToFile(filename, (char *)vse_frame->buffer.virt_addr[0], vse_frame->buffer.width * vse_frame->buffer.height * 3 / 2);
				}
			}
		}
	}

	client_running = 0;
	printf("[Thread] cam_rawdata_process ==> Quit\n");
	return NULL;
}

void *imu_data_process(void *context)
{
	while (client_running) {
		imu_get_data();
	}

	client_running = 0;
	printf("[Thread] imu_data_process ==> Quit\n");
	return NULL;
}

void *server_data_process(void *context)
{
	int ret = 0;
	int index;

	server_stream_info_t *stream_info = (server_stream_info_t *)&server_stream_info;
	// stream_header_t *stream_header = (stream_header_t *)&stream_info->stream_header;

	pipeline_info_t *pipeline_info_list = (pipeline_info_t *)context;
	pipeline_info_t *pipeline_info = NULL;
	pipe_contex_t *pipe_context = NULL;
	hbn_vnode_handle_t vse_handle;

	int vse_chn = 0;
	data_item_t *data_item = NULL;

	struct PerformanceTestParamSimple performace_total_cam_raw_right = {
		.iteration_number = 30 * 60,
		.test_case = "cam_server_right",
		.run_count = 0,
		.test_count = 0,
	};
	struct PerformanceTestParamSimple performace_total_cam_raw_left = {
		.iteration_number = 30 * 60,
		.test_case = "cam_server_left",
		.run_count = 0,
		.test_count = 0,
	};
	struct PerformanceTestParamSimple performace_total_depth = {
		.iteration_number = 30 * 60,
		.test_case = "cam_server_depth",
		.run_count = 0,
		.test_count = 0,
	};

	prctl(PR_SET_NAME, "server_process");
	printf("[Thread] server_process ==> Start\n");

	while (client_running)
	{
		for (index = 0; index < MAX_PIPE_NUM; index++)
		{
			pipeline_info = (pipeline_info_t *)&pipeline_info_list[index];
			pipe_context = (pipe_contex_t *)&pipeline_info->pipe_contexts;
			sync_queue_t *cam_to_server = (sync_queue_t *)&pipeline_info->cam_to_server;
			vse_handle = pipe_context->vse_node_handle;
			vse_chn = pipeline_info->vse_chn;

			if (index == RIGHT_CAM_CHN){
				performance_test_start_simple(&performace_total_cam_raw_right);
			}
			else if (index == LEFT_CAM_CHN) {
				performance_test_start_simple(&performace_total_cam_raw_left);
			}

			ret = sync_queue_obtain_inused_object(cam_to_server, 2000, &data_item);
			if(ret != 0){
				printf("sync_queue_obtain_inused_object failed.\n");
				continue;
			}

			hbn_vnode_image_t *vse_frame = ((hbn_vnode_image_t*)data_item->items);
			// send_size = vse_frame->buffer.width * vse_frame->buffer.height * 3 / 2;
			server_stream_add_node(stream_info, (index == RIGHT_CAM_CHN)? STREAM_NODE_CAM_RIGHT:STREAM_NODE_CAM_LEFT, vse_frame);

			ret = sync_queue_repay_unused_object(cam_to_server, 2000, data_item);
			if(ret != 0){
				printf("sync_queue_repay_unused_object failed\n");
				break;
			}

			hbn_vnode_releaseframe(vse_handle, vse_chn, vse_frame);

			if (index == RIGHT_CAM_CHN)
				performance_test_stop_simple(&performace_total_cam_raw_right);
			else if (index == LEFT_CAM_CHN)
				performance_test_stop_simple(&performace_total_cam_raw_left);
		}

		int val = atomic_load(&depth_event_cnt);
        if (val <= 0) {
			server_stream_add_node(stream_info, STREAM_NODE_DEPTH, NULL);
		}
		else {
			val -= 1;
			atomic_store(&depth_event_cnt, val);

			performance_test_start_simple(&performace_total_depth);
			ret = sync_queue_obtain_inused_object(&depth_to_server, 2000, &data_item);
			if(ret != 0){
				printf("sync_queue_obtain_inused_object failed.\n");
				continue;
			}

			server_stream_add_node(stream_info, STREAM_NODE_DEPTH, (void *)data_item->items);

			ret = sync_queue_repay_unused_object(&depth_to_server, 2000, data_item);
			if(ret != 0){
				printf("sync_queue_repay_unused_object failed\n");
				break;
			}
			performance_test_stop_simple(&performace_total_depth);
		}

		if (imu_flag) {
			server_stream_add_node(stream_info, STREAM_NODE_IMU, NULL);
		}

		/* Send Stream Data */
		ret = server_stream_flush(stream_info);
		if (ret < 0) {
			printf("server_stream_flush Error\n");
			break;
		}
	}

	client_running = 0;
	printf("[Thread] server_data_process ==> Quit\n");
	// vse_sync_queue_cleanup(pipeline_info_list);

	return NULL;
}

static int server_stream_add_node(server_stream_info_t *server_stream_info, int type, void *data)
{
	if (type < 0 || type >= STREAM_NODE_TAIL) {
		printf("server_stream_add_node: type error[%d]\n", type);
		return -1;
	}

	hbn_vnode_image_t *vse_frame = NULL;
	stream_node_info_t *node_info = &server_stream_info->stream_nodes[type];
	stream_header_t *stream_header = server_stream_info->stream_header;
	stream_node_header_t *node_header = (stream_node_header_t *)&stream_header->node_headers[type];

	if (type == STREAM_NODE_CAM_RIGHT || type == STREAM_NODE_CAM_LEFT) {
		vse_frame = (hbn_vnode_image_t *)data;
		node_header->timestamps = vse_frame->info.timestamps;
		node_header->frame_id = vse_frame->info.frame_id;

		memcpy(node_info->data_item, vse_frame->buffer.virt_addr[0], node_info->size);
		if (verbose_mode & VERBOSE_STREAM)
			printf("[Server] %s Cam data Add to Stream\n", (type == STREAM_NODE_CAM_RIGHT)?"Right":"Left");
	}
	else if (type == STREAM_NODE_DEPTH)
	{
		if (data == NULL) {
			node_header->size = 0;
		}
		else {
			node_header->size = node_info->size;
			memcpy(node_info->data_item, data, node_info->size);
			if (verbose_mode & VERBOSE_STREAM)
				printf("[Server] Depth data Add to Stream\n");
		}
	}
	else if (type == STREAM_NODE_IMU)
	{
		int imu_subnode_num = 0;
		imu_subnode_num = imu_data_flush_buffer(node_info->data_item);
		node_header->size = imu_subnode_num * sizeof(imu_subnode_data_t);
		if (verbose_mode & VERBOSE_STREAM)
			printf("[Server] IMU data (%d) Add to Stream\n", imu_subnode_num);
	}

	return 0;
}

static int server_stream_flush(server_stream_info_t *server_stream_info)
{
	if (verbose_mode & VERBOSE_STREAM) {
		printf("[Server] Steam Flush: Size = %d\n", server_stream_info->total_size);
	}

#ifdef USE_HBMEM
	hb_mem_common_buf_t *hb_mem_buf = (hb_mem_common_buf_t *)&server_stream_info->hb_mem_buf;
	hb_mem_flush_buf(hb_mem_buf->fd, 0, server_stream_info->total_size);
#endif

	stream_server_func_t *stream_server_func = &stream_server_func_list[server_stream_info->stream_server_type];
	if (stream_server_func->flush != NULL) {
		if (0 != stream_server_func->flush()) {
			printf("[Server] Flush Error\n");
			return -1;
		}
	}

	return 0;
}

static int server_stream_info_json_str(server_stream_info_t *server_stream_info, char *info_json_str)
{
	int index;
	stream_node_info_t *node_info = NULL;
	cJSON *root = cJSON_CreateObject();
	if (!root) {
        fprintf(stderr, "[Server] JSON Root object creation failed\n");
        return -1;
    }

	struct _CameraIntrinsics intr;
    cJSON *stream_info = cJSON_CreateObject();
    cJSON_AddStringToObject(stream_info, "ModelVersion", STEREO_INFER_VERSION);
	cJSON_AddNumberToObject(stream_info, "TotalSize", server_stream_info->total_size);

	stereo_get_cam_intr(&intr);
	cJSON_AddNumberToObject(stream_info, "fx", intr.fx);
	cJSON_AddNumberToObject(stream_info, "fy", intr.fy);
	cJSON_AddNumberToObject(stream_info, "cx", intr.cx);
	cJSON_AddNumberToObject(stream_info, "cy", intr.cy);

    cJSON_AddItemToObject(root, "StreamInformation", stream_info);

	cJSON *node_json_info = NULL;
	for (index = 0; index < STREAM_NODE_TYPE_NUM; index++) {
		node_info = (stream_node_info_t *)&server_stream_info->stream_nodes[index];
		node_json_info = cJSON_CreateObject();
		cJSON_AddNumberToObject(node_json_info, "Available", node_info->available);
		cJSON_AddNumberToObject(node_json_info, "Width", node_info->width);
		cJSON_AddNumberToObject(node_json_info, "Height", node_info->height);
		cJSON_AddNumberToObject(node_json_info, "Size", node_info->size);
		cJSON_AddNumberToObject(node_json_info, "Offset", node_info->offset);

		cJSON_AddItemToObject(root, node_info->name, node_json_info);
	}

	// 格式化JSON字符串
	char *json_str = cJSON_Print(root);
	if (json_str) {
		if (info_json_str)
			strcpy(info_json_str, json_str);

		printf("%s\n", json_str);
		free(json_str);
	}

	cJSON_Delete(root);
	return 0;
}


static int server_stream_info_init(server_stream_info_t *server_stream_info,
									pipeline_info_t *pipeline_info_list,
									int pipe_num)
{
	int index = 0;
	uint32_t offset = 0;
	stream_node_info_t *node_info = NULL;
	pipeline_info_t *pipeline_info = NULL;
	stream_header_t *stream_header = NULL;
	stream_node_header_t *node_header = NULL;

	server_stream_info->data_offset = sizeof(stream_header_t);
	server_stream_info->total_size = sizeof(stream_header_t);

	/* Camera Node: Right & Left Camera */
	for (index = 0; index < pipe_num; index++) {
		node_info = (stream_node_info_t *)&server_stream_info->stream_nodes[index];
		pipeline_info = (pipeline_info_t *)&pipeline_info_list[index];

		node_info->name = (index == RIGHT_CAM_CHN)? "Right-Cam":"Left-Cam";
		node_info->available = 1;

		if (gdc_flag) {
			node_info->width = pipeline_info->pipe_contexts.sensor_config->camera_config->height;
			node_info->height = pipeline_info->pipe_contexts.sensor_config->camera_config->width;
		}
		else {
			node_info->width = pipeline_info->pipe_contexts.sensor_config->camera_config->width;
			node_info->height = pipeline_info->pipe_contexts.sensor_config->camera_config->height;
		}

		node_info->size = (node_info->width * node_info->height * 3 / 2);
		node_info->offset = offset;
		server_stream_info->total_size += node_info->size;
		offset += node_info->size;
	}

	/* Depth Data Node */
	node_info = (stream_node_info_t *)&server_stream_info->stream_nodes[STREAM_NODE_DEPTH];
	node_info->name = "Depth";
	node_info->available = 1;
	node_info->width = STEREO_RES_WIDTH;
	node_info->height = STEREO_RES_HEIGHT;

	node_info->size = DEPTH_SIZE;
	node_info->offset = offset;
	server_stream_info->total_size += node_info->size;
	offset += node_info->size;

	/* IMU Data Node */
	node_info = (stream_node_info_t *)&server_stream_info->stream_nodes[STREAM_NODE_IMU];
	node_info->name = "IMU";
	node_info->available = (imu_flag)? 1 : 0;
	node_info->width = 0;
	node_info->height = 0;
	node_info->sub_node_size = (imu_flag)? sizeof(imu_subnode_data_t) : 0;
	node_info->sub_node_num = (imu_flag)? IMU_SUBNODE_MAX_NUM : 0;

	node_info->size = node_info->sub_node_size * node_info->sub_node_num;
	node_info->offset = offset;
	server_stream_info->total_size += node_info->size;
	offset += node_info->size;
	/** */

	/* Alloc Memory For Stream Buffer */
#ifdef USE_HBMEM
	/* Alloc HB memory */
	int64_t flags = HB_MEM_USAGE_CPU_READ_OFTEN | HB_MEM_USAGE_CPU_WRITE_OFTEN | HB_MEM_USAGE_CACHED;   // Cached
	server_stream_info->alloc_size = (server_stream_info->stream_server_type == STREAM_SERVER_MIPI_CSI)? \
							(DRM_MIPI_CSI_RES_WIDTH * DRM_MIPI_CSI_RES_HEIGHT * 3) : server_stream_info->total_size;
	if (0 != hb_mem_alloc_com_buf(server_stream_info->alloc_size, flags, &server_stream_info->hb_mem_buf)) {
		printf("hb_mem_alloc_com_buf For Stream Buffer Failed: Size[%ld]\n", server_stream_info->alloc_size);
		return -1;
	}
	server_stream_info->stream_buffer = (uint8_t *)server_stream_info->hb_mem_buf.virt_addr;
#else
	/* Malloc Memeory */
	server_stream_info->stream_buffer = (uint8_t *)malloc(server_stream_info->total_size);
	if (!server_stream_info->stream_buffer) {
		printf("Malloc For Stream Buffer Failed\n");
		return -1;
	}
#endif

	server_stream_info->stream_header = (stream_header_t *)server_stream_info->stream_buffer;
	server_stream_info->data_ptr = server_stream_info->stream_buffer + server_stream_info->data_offset;
	for (index = 0; index < STREAM_NODE_TYPE_NUM; index++) {
		node_info = (stream_node_info_t *)&server_stream_info->stream_nodes[index];
		node_info->data_item = server_stream_info->data_ptr + node_info->offset;
	}

	/* Show Stream Info */
	printf("================ Stereo Infer Information ================\n");
	printf("Model Verion: %s\n", STEREO_INFER_VERSION);
	printf("Calibrate Type: %s\n", (calibrate_type == 0)?"EEPROM":"FILE");
	printf("================ Stream Data Information ================\n");
	printf("Stream Type: %s\n", (server_stream_info->stream_server_type == STREAM_SERVER_SOCKET)? "Socket":"MIPI");
	printf("Interface: %s\n", server_stream_info->ifname);
	printf("Verbose: 0x%02x\n", verbose_mode);
	printf("Debug Dump File: 0x%02x\n", dumpfile_mode);
	printf("GDC: %d\n", gdc_flag);
	printf("Stream Total Size: %d\n", server_stream_info->total_size);
	printf("Buffer Alloc Size: %ld\n", server_stream_info->alloc_size);
	printf("Header Size / Data Offset: %d\n", server_stream_info->data_offset);
	printf("Node Num: %d\n", STREAM_NODE_TYPE_NUM);
	for (index = 0; index < STREAM_NODE_TYPE_NUM; index++) {
		node_info = (stream_node_info_t *)&server_stream_info->stream_nodes[index];
		printf("+++++++++++++++++++++++++++++++++\n");
		printf("+    [%d] %s\n", index, node_info->name);
		printf("+    Available: %d\n", node_info->available);
		printf("+    Resolution: %d x %d\n", node_info->width, node_info->height);
		printf("+    Size: %d\n", node_info->size);
		printf("+    Offset: %d\n", node_info->offset);
		printf("+++++++++++++++++++++++++++++++++\n");
	}
	printf("=========================================================\n");

	/* Fixed Stream Header */
	stream_header = (stream_header_t *)server_stream_info->stream_header;
	stream_header->magic = STREAM_HEADER_MAGIC;
	stream_header->total_size = server_stream_info->total_size;
	for (index = 0; index < STREAM_NODE_TYPE_NUM; index++) {
		node_info = (stream_node_info_t *)&server_stream_info->stream_nodes[index];
		node_header = (stream_node_header_t *)&stream_header->node_headers[index];
		node_header->type = index;
		node_header->size = node_info->size;
		node_header->offset = node_info->offset;
		node_header->timestamps = 0;
		node_header->frame_id = 0;
	}

	server_stream_info_json_str(server_stream_info, NULL);

	return 0;
}

static int server_stream_info_destroy(server_stream_info_t *server_stream_info)
{
	if (!server_stream_info->stream_buffer) {
	#ifdef USE_HBMEM
	if (0 != hb_mem_free_buf(server_stream_info->hb_mem_buf.fd)) {
		printf("hb_mem_free_buf For Stream Buffer Failed\n");
		return -1;
	}
	#else
		free(server_stream_info->stream_buffer);
		server_stream_info->stream_buffer = NULL;
	#endif
	}

	return 0;
}

int stream_server_socket_init(void)
{
	if (0 != check_network(server_stream_info.ifname)) {
		printf("[Server:Socket] Network [%s] is not Ready\n", server_stream_info.ifname);
		return -1;
	}

	server_fd = server_init();
	if (server_fd < 0) {
		printf("[Server:Socket] init error\n");
		return -1;
	}
	printf("[Server:Socket] Init\n");
	return 0;
}
int stream_server_socket_destroy(void)
{
	server_close(server_fd);
	printf("[Server:Socket] Destroy\n");
	return 0;
}
int stream_server_socket_process(void)
{
	int ret;

	printf("[Server:Socket] Process ====> Enter.\n");

	main_running = 1;
	while (main_running)
	{
		if (client_fd > 0) {
			sleep(2);
			continue;
		}
		else
		{
			ret = server_wait_client_connect(server_fd);
			if (ret == 0) {
				printf("Server Wait: timeout\n");
				continue;
			}
			else if (ret < 0) {
				printf("Server Wait Error(%d): %s\n", ret, strerror(errno));
				break;
			}
			else {
				client_fd = ret;
				printf("Client[%d] Connected.\n", client_fd);
			}

			/* Stream Info JSON */
			char *info_json_str = malloc(STREAM_INFO_JSON_STR_LEN);
			if (!info_json_str) {
				printf("[Server] malloc for JSON info Failed\n");
				break;
			}
			if (0 != server_stream_info_json_str(&server_stream_info, info_json_str)) {
				printf("[Server] server_stream_info_json_str Failed\n");
				break;
			}
			ret = server_send_data(client_fd, info_json_str, STREAM_INFO_JSON_STR_LEN);
			if (ret < 0) {
				free(info_json_str);
				printf("[Server:Socket] server_send_data Error\n");
				break;
			}
			free(info_json_str);

			/* Stream Service */
			client_running = 1;
			if (bpu_flag) {
				stereonet_infer_start();
			}
			pthread_create(&cam_rawdata_thread, NULL, (void *)cam_rawdata_process, (void *)&pipeline_info);

			if (imu_flag)
				pthread_create(&imu_data_thread, NULL, (void *)imu_data_process, (void *)&pipeline_info);

			pthread_create(&server_data_thread, NULL, (void *)server_data_process, (void *)&pipeline_info);

			// 等待信号退出
			WAIT_FOR_SIGNAL(process_running);

			if (bpu_flag) {
				stereonet_infer_stop();
			}

			if (cam_rawdata_thread > 0) {
				pthread_join(cam_rawdata_thread, NULL);
				cam_rawdata_thread = 0;
			}

			if (imu_data_thread > 0) {
				pthread_join(imu_data_thread, NULL);
				imu_data_thread = 0;
			}

			if (server_data_thread > 0) {
				pthread_join(server_data_thread, NULL);
				server_data_thread = 0;
			}

			if (client_fd > 0){
				close(client_fd);
				client_fd = -1;
			}

			// for (index = 0; index < total_pipeline_num; index++) {
			// 	sync_queue_t *cam_to_server = (sync_queue_t *)&pipeline_info[index].cam_to_server;
			// 	sync_queue_reset(cam_to_server);
			// }
			main_running = 0;
		}
	}

	printf("[Server:Socket] Process ====> Quit\n");
	return 0;
}
int stream_server_socket_flush(void)
{
	int ret = server_send_data(client_fd, server_stream_info.stream_buffer, server_stream_info.total_size);
	if (ret < 0) {
		printf("[Server:Socket] server_send_data Error\n");
		return -1;
	}
	return 0;
}

int stream_server_mipi_init(void)
{
	if (0 != drm_mipi_tx_init(server_stream_info.hb_mem_buf.fd, DRM_MIPI_CSI_RES_WIDTH, DRM_MIPI_CSI_RES_HEIGHT))
	{
		printf("[Server] drm_mipi_tx_init error\n");
		return -1;
	}
	printf("[Server:MIPI] Init\n");
	return 0;
}
int stream_server_mipi_destroy(void)
{
	drm_mipi_tx_destroy();
	printf("[Server:MIPI] Destroy\n");
	return 0;
}
int stream_server_mipi_process(void)
{
	printf("[Server:MIPI] Process ====> Enter\n");

	client_running = 1;
	if (bpu_flag) {
		stereonet_infer_start();
	}
	pthread_create(&cam_rawdata_thread, NULL, (void *)cam_rawdata_process, (void *)&pipeline_info);

	if (imu_flag)
		pthread_create(&imu_data_thread, NULL, (void *)imu_data_process, (void *)&pipeline_info);

	pthread_create(&server_data_thread, NULL, (void *)server_data_process, (void *)&pipeline_info);

	// 等待信号退出
	WAIT_FOR_SIGNAL(process_running);

	if (bpu_flag) {
		stereonet_infer_stop();
	}

	if (cam_rawdata_thread > 0) {
		pthread_join(cam_rawdata_thread, NULL);
		cam_rawdata_thread = 0;
	}

	if (imu_data_thread > 0) {
		pthread_join(imu_data_thread, NULL);
		imu_data_thread = 0;
	}

	if (server_data_thread > 0) {
		pthread_join(server_data_thread, NULL);
		server_data_thread = 0;
	}

	printf("[Server:MIPI] Process ====> Quit\n");
	return 0;
}

int stream_server_mipi_flush(void)
{
	int ret = drm_mipi_tx_flush();
	if (ret != 0) {
		printf("[Server:MIPI] drm_mipi_tx_flush Error\n");
		return -1;
	}
	return 0;
}

int stream_server_local_process(void)
{
	printf("[Server:local] Process ====> Enter\n");

	client_running = 1;
	if (bpu_flag) {
		stereonet_infer_start();
	}
	pthread_create(&cam_rawdata_thread, NULL, (void *)cam_rawdata_process, (void *)&pipeline_info);

	if (imu_flag)
		pthread_create(&imu_data_thread, NULL, (void *)imu_data_process, (void *)&pipeline_info);

	pthread_create(&server_data_thread, NULL, (void *)server_data_process, (void *)&pipeline_info);

	// 等待信号退出
	WAIT_FOR_SIGNAL(process_running);

	if (bpu_flag) {
		stereonet_infer_stop();
	}

	if (cam_rawdata_thread > 0) {
		pthread_join(cam_rawdata_thread, NULL);
		cam_rawdata_thread = 0;
	}

	if (imu_data_thread > 0) {
		pthread_join(imu_data_thread, NULL);
		imu_data_thread = 0;
	}

	if (server_data_thread > 0) {
		pthread_join(server_data_thread, NULL);
		server_data_thread = 0;
	}

	printf("[Server:local] Process ====> Quit\n");
	return 0;
}

int main(int argc, char** argv) {

	int ret = 0;
	int c = 0;
	int index = -1;

	server_stream_info.stream_server_type = STREAM_SERVER_NULL;
	stream_server_func_t *stream_server_func = NULL;

	if (argc <= 1) {
		print_help();
		exit(0);
	}

	signal(SIGINT, signal_handle);

	while ((c = getopt_long(argc, argv, "i:c:g:uv:bd:h", long_options, NULL)) != -1) {
		switch (c) {
		case 'i':
			if (0 == strcmp("mipi", optarg)) {
				server_stream_info.stream_server_type = STREAM_SERVER_MIPI_CSI;
			} else if (0 == strcmp("local", optarg)) {
				server_stream_info.stream_server_type = STREAM_SERVER_LOCAL_TEST;
			} else {
				server_stream_info.stream_server_type = STREAM_SERVER_SOCKET;
			}
			strcpy(server_stream_info.ifname, optarg);
			break;
		case 'c':
			if (total_pipeline_num >= MAX_PIPE_NUM) {
				fprintf(stderr, "Too many configurations. Maximum allowed is %d.\n", MAX_PIPE_NUM);
				return 1;
			}
			parse_config(&pipeline_info[total_pipeline_num], optarg, total_pipeline_num);
			// pipeline_info[total_pipeline_num].vse_chn = total_pipeline_num;
			if (total_pipeline_num == RIGHT_CAM_CHN) {
				pipeline_info[total_pipeline_num].vse_chn = RIGHT_CAM_VSE_CHN;
				pipeline_info[total_pipeline_num].infer_vse_chn = RIGHT_STEREO_VSE_CHN;
			}
			else {
				pipeline_info[total_pipeline_num].vse_chn = LEFT_CAM_VSE_CHN;
				pipeline_info[total_pipeline_num].infer_vse_chn = LEFT_STEREO_VSE_CHN;
			}
			total_pipeline_num++;
			break;
		case 'g':
			gdc_flag = 1;
			calibrate_type = atoi(optarg);
			break;
		case 'u':
			imu_flag = 1;
			break;
		case 'v':
			verbose_mode = atoi(optarg);
			break;
		case 'b':
			bpu_flag = 1;
			break;
		case 'd':
			dumpfile_mode = atoi(optarg);
			break;
		case 'h':
		default:
			print_help();
			return 0;
		}
	}

	/* Check Param */
	if (total_pipeline_num != MAX_PIPE_NUM \
		|| server_stream_info.stream_server_type == STREAM_SERVER_NULL) {
		print_help();
		return 0;
	}

	/* IMU */
	if (imu_flag && (0 != imu_init((verbose_mode & VERBOSE_IMU)? 1 : 0))) {
		printf("IMU is not Ready\n");
		return 0;
	}

	// 处理后的参数在这里可以使用
	for (int i = 0; i < total_pipeline_num; i++) {
		if (i == 0) {
			pipeline_info[i].vse_chn = RIGHT_CAM_VSE_CHN;
			pipeline_info[i].infer_vse_chn = RIGHT_STEREO_VSE_CHN;
		}
		else if (i == 1) {
			pipeline_info[i].vse_chn = LEFT_CAM_VSE_CHN;
			pipeline_info[i].infer_vse_chn = LEFT_STEREO_VSE_CHN;
		}

		printf("Pipeline index %d:\n", i);
		printf("\tSensor index: %d\n", pipeline_info[i].select_sensor_id);
		printf("\tSensor name: %s\n", pipeline_info[i].pipe_contexts.sensor_config->sensor_name);
		printf("\tActive mipi host: %d\n", pipeline_info[i].active_mipi_host);
		printf("\tVse Channel: %d\n", pipeline_info[i].vse_chn);
	}

	printf("MIPI host: 0x%x\n", used_mipi_host);
	for (int i = 0; i < MAX_PIPE_NUM; i++) {
		if (used_mipi_host & (1 << i)) {
			printf("  Host %d: Used\n", i);
		}
	}

	hb_mem_module_open();

	// Generate GDC Bin
	if (0 != stereo_infer_gen_gdc_bin(calibrate_type, LEFT_CAM_GDC_BIN_PATH, RIGHT_CAM_GDC_BIN_PATH)) {
		printf("stereo_infer_gen_gdc_bin Failed\n");
		hb_mem_module_close();
		return 0;
	}


	for (index = 0; index < total_pipeline_num; index++) {
		ret = create_and_run_vflow(&pipeline_info[index].pipe_contexts,
			pipeline_info[index].active_mipi_host,
			pipeline_info[index].vse_chn,
			pipeline_info[index].infer_vse_chn,
			pipeline_info[index].sensor_mode,
			index);
		if (ret != 0) {
			for (int j = 0; j < index; j++) {
				hbn_vflow_stop(pipeline_info[j].pipe_contexts.vflow_fd);
				hbn_vflow_destroy(pipeline_info[j].pipe_contexts.vflow_fd);
			}
			return 0;
		}

		/* mQueue for Camera -> Server */
		sync_queue_info_t *sync_queue_info = (sync_queue_info_t *)&pipeline_info[index].sync_queue_info;
		sync_queue_t *cam_to_server = (sync_queue_t *)&pipeline_info[index].cam_to_server;
		sync_queue_info->productor_name = (index == RIGHT_CAM_CHN)? "right_cam":"left_cam";
		sync_queue_info->consumer_name = "server";
		sync_queue_info->is_need_malloc_in_advance = 0;
		sync_queue_info->is_external_buffer = 0;
		sync_queue_info->queue_len = 6;
		sync_queue_info->data_item_size = sizeof(hbn_vnode_image_t);
		sync_queue_info->data_item_count = 1;
		sync_queue_info->item_data_init_param = NULL;
		sync_queue_info->item_data_init_func = NULL;
		sync_queue_info->item_data_deinit_param = &pipeline_info[index];
		sync_queue_info->item_data_deinit_func = cam_vse_frame_cleanup;

		if(0 != sync_queue_create(cam_to_server, sync_queue_info)){
			printf("sync queue create failed for right cam.\n");
			return -1;
		}
	}

	/* mQueue for Depth -> Server*/
	sync_queue_info_t sync_queue_info_depth = {
		.productor_name = "depth",
		.consumer_name = "server",

		.is_need_malloc_in_advance = 0,
		.is_external_buffer = 0,

		.queue_len = 3,
		.data_item_size = DEPTH_SIZE,
		.data_item_count = 1,

		.item_data_init_param = NULL,
		.item_data_init_func = NULL,
		.item_data_deinit_param = NULL,
		.item_data_deinit_func = NULL,
	};
	ret = sync_queue_create(&depth_to_server, &sync_queue_info_depth);
	if(ret != 0){
		printf("sync queue create failed for left cam.\n");
		return -1;
	}

	ret = stereo_infer_init();
	if (ret != 0) {
		printf("stereo_infer_init failed\n");
		goto main_error_out;
	}

	/* Stream Info Init */
	server_stream_info_init(&server_stream_info, pipeline_info, total_pipeline_num);

	/* Stream Server Init */
	stream_server_func = (stream_server_func_t *)&stream_server_func_list[server_stream_info.stream_server_type];
	printf("Server: %s.\n", stream_server_func->name);
	if (stream_server_func->init != NULL) {
		if (0 != stream_server_func->init()) {
			printf("[Server] init error\n");
			goto main_error_out;
		}
	}

	/* Stream Server Process */
	stream_server_func->process();

main_error_out:
	for (index = 0; index < total_pipeline_num; index++) {
		ret = hbn_vflow_stop(pipeline_info[index].pipe_contexts.vflow_fd);
		ERR_CON_EQ(ret, 0);
		hbn_vflow_destroy(pipeline_info[index].pipe_contexts.vflow_fd);
	}

	for (index = 0; index < total_pipeline_num; index++) {
		sync_queue_t *cam_to_server = (sync_queue_t *)&pipeline_info[index].cam_to_server;
		sync_queue_destory(cam_to_server);
	}
	sync_queue_destory(&depth_to_server);

	server_stream_info_destroy(&server_stream_info);

	if (stream_server_func->destroy != NULL)
		stream_server_func->destroy();

	imu_destroy();

	hb_mem_module_close();

	return 0;
}

