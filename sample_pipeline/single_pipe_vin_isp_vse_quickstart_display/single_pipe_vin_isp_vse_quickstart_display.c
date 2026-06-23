/***************************************************************************
 *                      COPYRIGHT NOTICE
 *             Copyright(C) 2026, D-Robotics Co., Ltd.
 *                     All rights reserved.
 ***************************************************************************/
/*
 * Quickstart MIPI-only sample: sensor cache + parallel hb_mem/display init,
 * Output is always VP_DISPLAY_OUTPUT_MIPI (MIPI LCD).
 * SC230AI only — see README.md in this directory.
 */

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <sched.h>

#include "common_utils.h"
#include "vp_display.h"
#include "channel_param_parser.h"

static struct option const long_options[] = {
	{"sensor", required_argument, NULL, 's'},
	{"channel-type", optional_argument, NULL, 'c'},
	{NULL, 0, NULL, 0}
};

int32_t running = 0;
vp_drm_context_t vp_drm_context;
extern int vin_isp_is_online;
extern int isp_vse_is_online;

static int create_and_run_vflow(pipe_contex_t *pipe_contex, int active_mipi_host);
void *read_vse_data(void *contex);

/* Quickstart sample: set in main before create_and_run_vflow */
static int g_parallel_attach;
static int g_bench_exit_on_display;
static int g_bench_exit_on_frame;
static int g_first_display_logged;

static int display_init(pipe_contex_t *pipe_contex);

/* First-frame timing (CLOCK_MONOTONIC deltas, ms) */
static struct timespec g_t0_main;
static struct timespec g_t_after_vflow_start;
static struct timespec g_t_after_display_ready;
static struct timespec g_t_display_init_start;
static int g_have_display_ts;
static int g_display_init_done;
static int g_display_init_ret;

static double timespec_diff_ms(const struct timespec *end, const struct timespec *start)
{
	return (double)(end->tv_sec - start->tv_sec) * 1000.0 +
			(double)(end->tv_nsec - start->tv_nsec) / 1e6;
}

/* Split timing: returns ms since *anchor, then sets anchor to now. */
/* Compute lap time from anchor and move anchor to current time. */
static double qs_lap_ms(struct timespec *anchor)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	double ms = timespec_diff_ms(&now, anchor);
	*anchor = now;
	return ms;
}

/* Read whether to skip runtime sensor detect (aggressive fast path). */
static int fastboot_skip_sensor_detect_enabled(void)
{
	const char *v = getenv("VP_QUICKSTART_SKIP_SENSOR_DETECT");

	if (!v)
		return 0;
	return (!strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes"));
}

/* Read whether sensor detect cache is enabled (default: on). */
static int fastboot_use_sensor_cache_enabled(void)
{
	const char *v = getenv("VP_QUICKSTART_USE_SENSOR_CACHE");

	/* Default ON: biggest safe win comes from skipping repeated detect. */
	if (!v)
		return 1;
	if (!strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no"))
		return 0;
	return 1;
}

/* Load cached sensor routing info to avoid full detect on warm boots. */
static int load_sensor_fast_cache(const char *sensor_name,
		int *mipi_rx, uint32_t *i2c_addr)
{
	FILE *fp = NULL;
	char name[128];
	int mipi;
	unsigned int addr;
	const char *paths[] = {
		"/userdata/vp_sensor_quickstart.cache",
		"/tmp/vp_sensor_quickstart.cache",
	};
	int i;

	for (i = 0; i < (int)(sizeof(paths) / sizeof(paths[0])); i++) {
		fp = fopen(paths[i], "r");
		if (fp)
			break;
	}
	if (!fp)
		return -1;
	if (fscanf(fp, "%127s %d %x", name, &mipi, &addr) != 3) {
		fclose(fp);
		return -1;
	}
	fclose(fp);
	if (strcmp(name, sensor_name))
		return -1;
	*mipi_rx = mipi;
	*i2c_addr = (uint32_t)addr;
	return 0;
}

/* Save detected sensor routing info for next startup acceleration. */
static void save_sensor_fast_cache(const vp_sensor_config_t *cfg)
{
	FILE *fp = NULL;
	const char *paths[] = {
		"/userdata/vp_sensor_quickstart.cache",
		"/tmp/vp_sensor_quickstart.cache",
	};
	int i;

	for (i = 0; i < (int)(sizeof(paths) / sizeof(paths[0])); i++) {
		fp = fopen(paths[i], "w");
		if (fp)
			break;
	}
	if (!fp)
		return;
	fprintf(fp, "%s %d %x\n",
		cfg->sensor_name,
		cfg->vin_node_attr->cim_attr.mipi_rx,
		cfg->camera_config->addr);
	fclose(fp);
}

/* Cleanup partially created camera/vflow objects before retry paths. */
static void reset_partial_pipeline(pipe_contex_t *pipe_contex)
{
	if (pipe_contex->vflow_fd >= 0) {
		(void)hbn_vflow_stop(pipe_contex->vflow_fd);
		(void)hbn_vflow_destroy(pipe_contex->vflow_fd);
		pipe_contex->vflow_fd = -1;
	}
	if (pipe_contex->vse_node_handle >= 0) {
		(void)hbn_vnode_close(pipe_contex->vse_node_handle);
		pipe_contex->vse_node_handle = -1;
	}
	if (pipe_contex->isp_node_handle >= 0) {
		(void)hbn_vnode_close(pipe_contex->isp_node_handle);
		pipe_contex->isp_node_handle = -1;
	}
	if (pipe_contex->vin_node_handle >= 0) {
		(void)hbn_vnode_close(pipe_contex->vin_node_handle);
		pipe_contex->vin_node_handle = -1;
	}
	if (pipe_contex->cam_fd >= 0) {
		(void)hbn_camera_destroy(pipe_contex->cam_fd);
		pipe_contex->cam_fd = -1;
	}
}

struct display_init_task {
	pipe_contex_t *pipe_context;
	int ret;
	struct timespec t_done;
};

struct hbmem_open_task {
	struct timespec t_done;
};

/* Background display init thread to overlap with camera pipeline setup. */
static void *display_init_worker(void *arg)
{
	struct display_init_task *task = (struct display_init_task *)arg;
	struct sched_param sp = {0};

	/* Best-effort: prioritize display init on cold boot. */
	sp.sched_priority = 20;
	(void)pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);

	task->ret = display_init(task->pipe_context);
	g_display_init_ret = task->ret;
	g_display_init_done = 1;
	if (task->ret == 0) {
		clock_gettime(CLOCK_MONOTONIC, &task->t_done);
		g_t_after_display_ready = task->t_done;
		g_have_display_ts = 1;
	}
	return NULL;
}

/* Background hbmem module init to hide startup latency. */
static void *hbmem_open_worker(void *arg)
{
	struct hbmem_open_task *task = (struct hbmem_open_task *)arg;

	hb_mem_module_open();
	clock_gettime(CLOCK_MONOTONIC, &task->t_done);
	return NULL;
}

struct cam_attach_task {
	camera_handle_t cam_fd;
	hbn_vnode_handle_t vin;
	int ret;
};

/* Parse bounded integer env var with default fallback. */
static int env_u32_in_range(const char *name, int defv, int minv, int maxv)
{
	const char *v = getenv(name);
	char *end = NULL;
	long val;

	if (!v || !*v)
		return defv;
	val = strtol(v, &end, 10);
	if (*end != '\0' || val < minv || val > maxv)
		return defv;
	return (int)val;
}

/* Set fast-mode default env only when caller did not provide one. */
static void set_env_default_if_absent(const char *name, const char *value)
{
	if (!getenv(name))
		(void)setenv(name, value, 0);
}

/* Print CLI usage and sensor list. */
static void print_help(const char *argv0)
{
	printf("Usage: %s [OPTIONS]\n", argv0);
	printf("Fast MIPI display sample (VIN-ISP-VSE -> MIPI panel).\n");
	printf("Restriction: SC230AI sensor only (quickstart env defaults are SC230AI-specific).\n");
	printf("Options:\n");
	printf("  -s <sensor_index>		Specify sensor index\n");
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

/* Stop frame loop on termination signal. */
void signal_handle(int signo)
{
	running = 0;
}

/* Create camera device handle from selected sensor config. */
static int create_camera_node(pipe_contex_t *pipe_contex)
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
	int32_t ret = hbn_camera_create(cam_cfg, &pipe_contex->cam_fd);
	ERR_CON_EQ(ret, 0);

	return 0;
}

/* Create/configure VIN node and bind it to selected active mipi host. */
static int create_vin_node(pipe_contex_t *pipe_contex, int active_mipi_host)
{
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

	if (pipe_contex->csi_config.mclk_is_not_configed) {
		// 设备树中没有配置 mclk：使用外部晶振
		printf("csi%d ignore mclk ex attr, because not config mclk.\n",
			pipe_contex->csi_config.index);
	} else {
		vin_attr_ex.vin_attr_ex_mask = sensor_config->vin_attr_ex->vin_attr_ex_mask;
		vin_attr_ex.mclk_ex_attr.mclk_freq = sensor_config->vin_attr_ex->mclk_ex_attr.mclk_freq;
		vin_attr_ex_mask = vin_attr_ex.vin_attr_ex_mask;
	}
	if (vin_isp_is_online) { /*vin->isp: online mode*/
		sensor_config->vin_node_attr->cim_attr.cim_isp_flyby = 1;
		sensor_config->vin_ochn_attr->ddr_en = 0;
	} else { /* vin->isp: offline mode*/
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
		memset(&alloc_attr, 0, sizeof(hbn_buf_alloc_attr_t));
		alloc_attr.buffers_num = 3;
		alloc_attr.is_contig = 1;
		alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN | HB_MEM_USAGE_CPU_WRITE_OFTEN | HB_MEM_USAGE_CACHED;
		ret = hbn_vnode_set_ochn_buf_attr(*vin_node_handle, ochn_id, &alloc_attr);
		ERR_CON_EQ(ret, 0);
	}
	return 0;
}

/* Create/configure ISP node according to online/offline topology. */
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

	if (vin_isp_is_online) {	/*vin->isp: online mode*/
		sensor_config->isp_attr->input_mode = PASSTHROUGH_MODE;
	} else {	/* vin->isp: offline mode*/
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
		alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN | HB_MEM_USAGE_CPU_WRITE_OFTEN | HB_MEM_USAGE_CACHED;
		ret = hbn_vnode_set_ochn_buf_attr(*isp_node_handle, ochn_id, &alloc_attr);
		ERR_CON_EQ(ret, 0);
	}

	return 0;
}

/* Create/configure VSE output node and output buffer attributes. */
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

	// 输出原分辨率
	vse_ochn_attr.target_w = input_width;
	vse_ochn_attr.target_h = input_height;

	ret = hbn_vnode_open(HB_VSE, hw_id, AUTO_ALLOC_ID, vse_node_handle);
	ERR_CON_EQ(ret, 0);

	ret = hbn_vnode_set_attr(*vse_node_handle, &vse_attr);
	ERR_CON_EQ(ret, 0);

	ret = hbn_vnode_set_ichn_attr(*vse_node_handle, ichn_id, &vse_ichn_attr);
	ERR_CON_EQ(ret, 0);

	alloc_attr.buffers_num = env_u32_in_range("VP_QUICKSTART_VSE_BUF_NUM", 2, 2, 6);
	alloc_attr.is_contig = 1;
	alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN | HB_MEM_USAGE_CPU_WRITE_OFTEN | HB_MEM_USAGE_CACHED;
	printf("VSE ochn buffer num: %d\n", alloc_attr.buffers_num);

	printf("hbn_vnode_set_ochn_attr: %dx%d\n", vse_ochn_attr.target_w, vse_ochn_attr.target_h);
	ret = hbn_vnode_set_ochn_attr(*vse_node_handle, 0, &vse_ochn_attr);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_ochn_buf_attr(*vse_node_handle, 0, &alloc_attr);
	ERR_CON_EQ(ret, 0);

	return 0;
}


/* Build full VIN->ISP->VSE vflow, attach camera, then start streaming. */
static int create_and_run_vflow(pipe_contex_t *pipe_contex, int active_mipi_host)
{
	int32_t ret = 0;
	struct timespec lap;
	double ms_cam, ms_vin, ms_isp, ms_vse, ms_vf_create, ms_vf_add;
	double ms_bind_vi, ms_bind_is, ms_attach, ms_start, ms_sum;
	pthread_t attach_tid = 0;
	struct cam_attach_task attach_task = {0};
	int attach_thread_started = 0;

	clock_gettime(CLOCK_MONOTONIC, &lap);

	// 创建pipeline中的每个node
	ret = create_camera_node(pipe_contex);
	ERR_CON_EQ(ret, 0);
	ms_cam = qs_lap_ms(&lap);

	ret = create_vin_node(pipe_contex, active_mipi_host);
	ERR_CON_EQ(ret, 0);
	ms_vin = qs_lap_ms(&lap);

	ret = create_isp_node(pipe_contex);
	if (ret != 0)
		goto attach_cleanup;
	ms_isp = qs_lap_ms(&lap);

	ret = create_vse_node(pipe_contex);
	if (ret != 0)
		goto attach_cleanup;
	ms_vse = qs_lap_ms(&lap);

	// 创建HBN flow
	ret = hbn_vflow_create(&pipe_contex->vflow_fd);
	if (ret != 0)
		goto attach_cleanup;
	ms_vf_create = qs_lap_ms(&lap);

	ret = hbn_vflow_add_vnode(pipe_contex->vflow_fd, pipe_contex->vin_node_handle);
	if (ret != 0)
		goto attach_cleanup;
	ret = hbn_vflow_add_vnode(pipe_contex->vflow_fd, pipe_contex->isp_node_handle);
	if (ret != 0)
		goto attach_cleanup;
	ret = hbn_vflow_add_vnode(pipe_contex->vflow_fd, pipe_contex->vse_node_handle);
	if (ret != 0)
		goto attach_cleanup;
	ms_vf_add = qs_lap_ms(&lap);

	if (vin_isp_is_online) {
		ret = hbn_vflow_bind_vnode(pipe_contex->vflow_fd, pipe_contex->vin_node_handle, 1, pipe_contex->isp_node_handle, 0);
	} else {
		ret = hbn_vflow_bind_vnode(pipe_contex->vflow_fd, pipe_contex->vin_node_handle, 0, pipe_contex->isp_node_handle, 0);
	}
	if (ret != 0)
		goto attach_cleanup;
	ms_bind_vi = qs_lap_ms(&lap);

	if (isp_vse_is_online) {
		ret = hbn_vflow_bind_vnode(pipe_contex->vflow_fd, pipe_contex->isp_node_handle, 1, pipe_contex->vse_node_handle, 0);
	} else {
		ret = hbn_vflow_bind_vnode(pipe_contex->vflow_fd, pipe_contex->isp_node_handle, 0, pipe_contex->vse_node_handle, 0);
	}
	if (ret != 0)
		goto attach_cleanup;
	ms_bind_is = qs_lap_ms(&lap);

	if (attach_thread_started) {
		ret = pthread_join(attach_tid, NULL);
		attach_thread_started = 0;
		if (ret != 0)
			return -1;
		ret = attach_task.ret;
		if (ret != 0)
			return ret;
		ms_attach = qs_lap_ms(&lap);
	} else {
		ret = hbn_camera_attach_to_vin(pipe_contex->cam_fd, pipe_contex->vin_node_handle);
		if (ret != 0)
			return ret;
		ms_attach = qs_lap_ms(&lap);
	}

	ret = hbn_vflow_start(pipe_contex->vflow_fd);
	ERR_CON_EQ(ret, 0);
	ms_start = qs_lap_ms(&lap);

	ms_sum = ms_cam + ms_vin + ms_isp + ms_vse + ms_vf_create + ms_vf_add +
		 ms_bind_vi + ms_bind_is + ms_attach + ms_start;
	printf("[single_pipe_vin_isp_vse_quickstart_display] vflow build (CLOCK_MONOTONIC, ms):\n"
			"  create_camera_node     %8.2f\n"
			"  create_vin_node        %8.2f\n"
			"  create_isp_node        %8.2f\n"
			"  create_vse_node        %8.2f\n"
			"  hbn_vflow_create       %8.2f\n"
			"  hbn_vflow_add_vnode x3 %8.2f\n"
			"  bind VIN->ISP          %8.2f\n"
			"  bind ISP->VSE          %8.2f\n"
			"  %s%8.2f\n"
			"  hbn_vflow_start        %8.2f\n"
			"  --- subtotal           %8.2f\n",
			ms_cam, ms_vin, ms_isp, ms_vse, ms_vf_create, ms_vf_add,
			ms_bind_vi, ms_bind_is,
			g_parallel_attach ?
			"camera_attach_join     " : "camera_attach_to_vin   ",
			ms_attach, ms_start, ms_sum);

	return 0;

attach_cleanup:
	if (attach_thread_started) {
		(void)pthread_join(attach_tid, NULL);
		attach_thread_started = 0;
	}
	return -1;
}

void vp_vin_print_hbn_frame_info_t(const hbn_frame_info_t *frame_info);
void vp_vin_print_hb_mem_graphic_buf_t(const hb_mem_graphic_buf_t *graphic_buf);

// 打印 hbn_vnode_image_t 结构体的所有字段内容
void vp_vin_print_hbn_vnode_image_t(const hbn_vnode_image_t *frame)
{
	printf("=== Frame Info ===\n");
	vp_vin_print_hbn_frame_info_t(&(frame->info));
	printf("\n=== Graphic Buffer ===\n");
	vp_vin_print_hb_mem_graphic_buf_t(&(frame->buffer));
}

// 打印 hbn_frame_info_t 结构体的所有字段内容
void vp_vin_print_hbn_frame_info_t(const hbn_frame_info_t *frame_info)
{
	printf("Frame ID: %u\n", frame_info->frame_id);
	printf("Timestamps: %lu\n", frame_info->timestamps);
	printf("Systimestamps: %lu\n", frame_info->sys_timestamps);
	printf("tv: %ld.%06ld\n", frame_info->tv.tv_sec, frame_info->tv.tv_usec);
	printf("trig_tv: %ld.%06ld\n", frame_info->trig_tv.tv_sec, frame_info->trig_tv.tv_usec);
	printf("Frame Done: %u\n", frame_info->frame_done);
	printf("Buffer Index: %d\n", frame_info->bufferindex);
}

// 打印 hb_mem_graphic_buf_t 结构体的所有字段内容
void vp_vin_print_hb_mem_graphic_buf_t(const hb_mem_graphic_buf_t *graphic_buf)
{
	printf("File Descriptors: ");
	for (int i = 0; i < MAX_GRAPHIC_BUF_COMP; i++) {
		printf("%d ", graphic_buf->fd[i]);
	}
	printf("\n");

	printf("Plane Count: %d\n", graphic_buf->plane_cnt);
	printf("Format: %d\n", graphic_buf->format);
	printf("Width: %d\n", graphic_buf->width);
	printf("Height: %d\n", graphic_buf->height);
	printf("Stride: %d\n", graphic_buf->stride);
	printf("Vertical Stride: %d\n", graphic_buf->vstride);
	printf("Is Contiguous: %d\n", graphic_buf->is_contig);

	printf("Share IDs: ");
	for (int i = 0; i < MAX_GRAPHIC_BUF_COMP; i++) {
		printf("%d ", graphic_buf->share_id[i]);
	}
	printf("\n");

	printf("Flags: %ld\n", graphic_buf->flags);

	printf("Sizes: ");
	for (int i = 0; i < MAX_GRAPHIC_BUF_COMP; i++) {
		printf("%lu ", graphic_buf->size[i]);
	}
	printf("\n");

	printf("Virtual Addresses: ");
	for (int i = 0; i < MAX_GRAPHIC_BUF_COMP; i++) {
		printf("%p ", graphic_buf->virt_addr[i]);
	}
	printf("\n");

	printf("Physical Addresses: ");
	for (int i = 0; i < MAX_GRAPHIC_BUF_COMP; i++) {
		printf("%lu ", graphic_buf->phys_addr[i]);
	}
	printf("\n");

	printf("Offsets: ");
	for (int i = 0; i < MAX_GRAPHIC_BUF_COMP; i++) {
		printf("%lu ", graphic_buf->offset[i]);
	}
	printf("\n");
}


/* Frame loop: pull VSE frames, submit to display, and log first-frame. */
void *read_vse_data(void *context)
{
	pipe_contex_t *pipe_context = (pipe_contex_t *)context;
	hbn_vnode_handle_t vse_node_handle = pipe_context->vse_node_handle;
	hbn_vnode_image_t out_img = {0};
	int ret = 0;

	static int first_frame_logged;

	while (running) {

		ret = hbn_vnode_getframe(vse_node_handle, 0, 1000, &out_img);
		if (ret != 0) {
			printf("hbn_vnode_getframe VSE channel failed\n");
			continue;
		}

		if (!first_frame_logged) {
			struct timespec t_now;
			double ms_main, ms_vflow, ms_after_disp = 0.0, disp_init_ms = 0.0;

			clock_gettime(CLOCK_MONOTONIC, &t_now);
			ms_main = timespec_diff_ms(&t_now, &g_t0_main);
			ms_vflow = timespec_diff_ms(&t_now, &g_t_after_vflow_start);
			if (g_have_display_ts) {
				ms_after_disp = timespec_diff_ms(&t_now, &g_t_after_display_ready);
				disp_init_ms = timespec_diff_ms(&g_t_after_display_ready, &g_t_display_init_start);
			}

			printf("[single_pipe_vin_isp_vse_quickstart_display] first frame (CLOCK_MONOTONIC):\n");
			printf("  +%.3f ms  since main() entry (incl. args, hb_mem, vflow, display, wait)\n",
					ms_main);
			printf("  +%.3f ms  since hbn_vflow_start (incl. display_init + first VSE getframe)\n",
					ms_vflow);
			if (g_have_display_ts) {
				printf("  +%.3f ms  since display_init done (VSE->user first buffer)\n",
						ms_after_disp);
				printf("  %.3f ms  display_init duration (display thread start -> display ready)\n",
						disp_init_ms);
			}
			printf("  driver: frame_id=%u sys_ts=%lu",
					out_img.info.frame_id,
					(unsigned long)out_img.info.sys_timestamps);
			if (out_img.info.tv.tv_sec == 0 && out_img.info.tv.tv_usec == 0)
				printf(" tv=(unset)\n");
			else
				printf(" tv=%ld.%06ld\n",
						(long)out_img.info.tv.tv_sec,
						(long)out_img.info.tv.tv_usec);
			first_frame_logged = 1;
			if (g_bench_exit_on_frame && !g_bench_exit_on_display)
				running = 0;
		}

		for (int j = 0; j < 2; ++j) {
			hb_mem_invalidate_buf_with_vaddr((uint64_t)out_img.buffer.virt_addr[j], out_img.buffer.size[j]);
		}

		if (g_display_init_done && g_display_init_ret == 0) {
			ret = vp_display_set_frame(&vp_drm_context, &out_img.buffer);
			if (ret != 0) {
				printf("vp_display_set_frame failed %d.\n", ret);
			} else if (!g_first_display_logged) {
				struct timespec t_disp;
				double ms_main_d, ms_vflow_d;
				clock_gettime(CLOCK_MONOTONIC, &t_disp);
				ms_main_d = timespec_diff_ms(&t_disp, &g_t0_main);
				ms_vflow_d = timespec_diff_ms(&t_disp, &g_t_after_vflow_start);
				printf("[single_pipe_vin_isp_vse_quickstart_display] first frame displayed (CLOCK_MONOTONIC):\n"
						"  +%.3f ms  since main() entry\n"
						"  +%.3f ms  since hbn_vflow_start\n",
						ms_main_d, ms_vflow_d);
				g_first_display_logged = 1;
				if (g_bench_exit_on_display)
					running = 0;
			}
		}

		hbn_vnode_releaseframe(vse_node_handle, 0, &out_img);
	}

	return NULL;
}

/* Initialize MIPI display path with sensor output resolution. */
static int display_init(pipe_contex_t *pipe_contex)
{
	int ret;
	int output_width, output_height;
	vp_sensor_config_t *sensor_config = pipe_contex->sensor_config;
	isp_attr_t *isp_attr = sensor_config->isp_attr;

	output_width = isp_attr->crop.w;
	output_height = isp_attr->crop.h;

	vp_drm_context.preferred_connector_type = VP_DISPLAY_OUTPUT_MIPI;
	ret = vp_display_init(&vp_drm_context, output_width, output_height);
	if (ret != 0) {
		printf("mipi init failed.\n");
		return -1;
	}
	return 0;
}

/* Release DRM/display resources. */
static void display_deinit(void)
{
	vp_display_deinit(&vp_drm_context);
}

/* Application entry: parse args, init in parallel, run pipeline and display. */
int main(int argc, char** argv)
{
	int ret = 0;
	pipe_contex_t pipe_contex = {0};
	pthread_t read_thread;
	pthread_t display_thread;
	pthread_t hbmem_thread;
	int opt_index = 0;
	int c = 0;
	int index = -1;
	int active_mipi_host;
	int display_thread_started = 0;
	int hbmem_thread_started = 0;
	struct display_init_task display_task = {0};
	struct hbmem_open_task hbmem_task = {0};
	int fast_cache_used = 0;
	int fast_cache_enabled = fastboot_use_sensor_cache_enabled();

	pipe_contex.vflow_fd = -1;
	pipe_contex.vin_node_handle = -1;
	pipe_contex.isp_node_handle = -1;
	pipe_contex.vse_node_handle = -1;
	pipe_contex.cam_fd = -1;

	/*
	 * Power-on defaults for faster first display:
	 * - keep caller override capability via pre-set env vars
	 * - bias SC230AI init delay scale and VSE queue depth
	 */
	set_env_default_if_absent("CAM_QUICKSTART_SC230AI_DELAY_SCALE", "10");
	set_env_default_if_absent("VP_QUICKSTART_VSE_BUF_NUM", "2");

	g_display_init_done = 0;
	g_display_init_ret = -1;
	g_parallel_attach = 0;
	g_bench_exit_on_display = 0;
	g_bench_exit_on_frame = 0;
	g_first_display_logged = 0;

	clock_gettime(CLOCK_MONOTONIC, &g_t0_main);
	ret = pthread_create(&hbmem_thread, NULL, hbmem_open_worker, &hbmem_task);
	if (ret != 0) {
		printf("\nError: Failed to create hbmem init thread\n");
		goto cleanup;
	}
	hbmem_thread_started = 1;

	while ((c = getopt_long(argc, argv, "s:c:h", long_options, &opt_index)) != -1) {
		switch (c) {
		case 's':
			index = atoi(optarg);
			break;
		case 'c':
			if (parse_channel_string(optarg) != 0) {
				printf("Invalid channel type %s.\n", optarg);
				ret = -1;
				goto cleanup;
			}
			break;
		case 'h':
		default:
			print_help(argv[0]);
			ret = 0;
			goto cleanup;
		}
	}

	if (index < vp_get_sensors_list_number() && index >= 0) {
		int cache_mipi = -1;
		uint32_t cache_i2c_addr = 0;

		pipe_contex.sensor_config = vp_sensor_config_list[index];
		printf("Using index:%d  sensor_name:%s  config_file:%s\n",
				index,
				vp_sensor_config_list[index]->sensor_name,
				vp_sensor_config_list[index]->config_file);
		display_task.pipe_context = &pipe_contex;
		clock_gettime(CLOCK_MONOTONIC, &g_t_display_init_start);
		ret = pthread_create(&display_thread, NULL, display_init_worker, &display_task);
		if (ret != 0) {
			printf("\nError: Failed to create display init thread\n");
			goto cleanup;
		}
		display_thread_started = 1;
		if (fast_cache_enabled &&
		    load_sensor_fast_cache(pipe_contex.sensor_config->sensor_name, &cache_mipi, &cache_i2c_addr) == 0) {
			fast_cache_used = 1;
			active_mipi_host = cache_mipi;
			pipe_contex.sensor_config->vin_node_attr->cim_attr.mipi_rx = active_mipi_host;
			pipe_contex.sensor_config->camera_config->addr = cache_i2c_addr;
			pipe_contex.csi_config.index = active_mipi_host;
			pipe_contex.csi_config.mclk_is_not_configed = 0;
			printf("QUICKSTART(cache): mipi_rx=%d i2c_addr=0x%x\n", active_mipi_host, cache_i2c_addr);
		} else if (fastboot_skip_sensor_detect_enabled()) {
			/*
			 * Fast path for fixed hardware topology:
			 * skip runtime sensor detect/reset/I2C scan and trust
			 * static sensor config's mipi_rx.
			 */
			active_mipi_host = pipe_contex.sensor_config->vin_node_attr->cim_attr.mipi_rx;
			pipe_contex.csi_config.index = active_mipi_host;
			pipe_contex.csi_config.mclk_is_not_configed = 0;
			printf("QUICKSTART: skip vp_sensor_fixed_mipi_host, use configured mipi_rx=%d\n",
					active_mipi_host);
		} else {
			if (fast_cache_enabled)
				printf("QUICKSTART(cache): miss, run vp_sensor_fixed_mipi_host\n");
			ret = vp_sensor_fixed_mipi_host(pipe_contex.sensor_config, &pipe_contex.csi_config);
			if (ret != 0) {
				printf("No Camera Sensor found. Please check if the specified "
					"sensor is connected to the Camera interface.\n");
				goto cleanup;
			}
			active_mipi_host = pipe_contex.sensor_config->vin_node_attr->cim_attr.mipi_rx;
			if (fast_cache_enabled)
				save_sensor_fast_cache(pipe_contex.sensor_config);
		}
	} else {
		printf("Unsupport sensor index:%d\n", index);
		print_help(argv[0]);
		ret = 0;
		goto cleanup;
	}
	if ((pipe_contex.sensor_config->camera_config->sensor_mode == DOL2_M)
		&& (vin_isp_is_online == 0)) {
		printf("\nError:%s's sensor_mode is DOL2_M, must work in online mode.\n\n",
				pipe_contex.sensor_config->sensor_name);
		ret = -1;
		goto cleanup;
	}

	struct timespec t_before_hb, t_before_vflow, t_after_vflow;
	struct timespec t_after_hb_join;
	double ms_prep, ms_hbmem_wait, ms_vflow_fn, ms_wait_display;

	clock_gettime(CLOCK_MONOTONIC, &t_before_hb);
	ms_prep = timespec_diff_ms(&t_before_hb, &g_t0_main);
	clock_gettime(CLOCK_MONOTONIC, &t_before_vflow);

	int used_parallel = g_parallel_attach;
	ret = create_and_run_vflow(&pipe_contex, active_mipi_host);
	if (ret != 0 && used_parallel) {
		printf("[single_pipe_vin_isp_vse_quickstart_display] parallel attach path failed (ret=%d); retry with synchronous attach\n",
				ret);
		g_parallel_attach = 0;
		reset_partial_pipeline(&pipe_contex);
		ret = create_and_run_vflow(&pipe_contex, active_mipi_host);
	}
	if (ret != 0 && fast_cache_used) {
		printf("QUICKSTART(cache): create_and_run_vflow failed ret=%d, fallback to vp_sensor_fixed_mipi_host\n",
				ret);
		/*
		 * Aggressive camera quickstart skips may break attach path.
		 * Disable them for fallback retry to recover robustness.
		 */
		printf("QUICKSTART(cache): fallback retry\n");
		reset_partial_pipeline(&pipe_contex);
		ret = vp_sensor_fixed_mipi_host(pipe_contex.sensor_config, &pipe_contex.csi_config);
		if (ret == 0) {
			active_mipi_host = pipe_contex.sensor_config->vin_node_attr->cim_attr.mipi_rx;
			if (fast_cache_enabled)
				save_sensor_fast_cache(pipe_contex.sensor_config);
			ret = create_and_run_vflow(&pipe_contex, active_mipi_host);
		}
	}
	if (ret != 0) {
		reset_partial_pipeline(&pipe_contex);
		ret = vp_sensor_fixed_mipi_host(pipe_contex.sensor_config, &pipe_contex.csi_config);
		if (ret == 0) {
			active_mipi_host = pipe_contex.sensor_config->vin_node_attr->cim_attr.mipi_rx;
			ret = create_and_run_vflow(&pipe_contex, active_mipi_host);
		}
	}

	clock_gettime(CLOCK_MONOTONIC, &t_after_vflow);
	g_t_after_vflow_start = t_after_vflow;
	ms_vflow_fn = timespec_diff_ms(&t_after_vflow, &t_before_vflow);

	/* Sample display tail without blocking critical path. */
	if (g_have_display_ts) {
		ms_wait_display = timespec_diff_ms(&g_t_after_display_ready, &t_after_vflow);
		if (ms_wait_display < 0.0)
			ms_wait_display = 0.0;
	} else {
		ms_wait_display = 0.0;
	}
	ret = pthread_join(hbmem_thread, NULL);
	hbmem_thread_started = 0;
	if (ret != 0) {
		printf("\nError: Failed to join hbmem init thread\n");
		goto cleanup;
	}
	clock_gettime(CLOCK_MONOTONIC, &t_after_hb_join);
	/* Remaining hbmem tail after display/vflow overlap. */
	ms_hbmem_wait = timespec_diff_ms(&t_after_hb_join, &t_after_vflow);
	if (ms_hbmem_wait < 0.0)
		ms_hbmem_wait = 0.0;

	printf("[single_pipe_vin_isp_vse_quickstart_display] before first-frame (CLOCK_MONOTONIC, ms):\n"
		"  getopt+sensor+prints   %8.2f\n"
		"  hb_mem wait tail       %8.2f\n"
		"  create_and_run_vflow   %8.2f\n"
		"  wait display tail(s)   %8.2f\n"
		"  --- critical path      %8.2f\n",
		ms_prep, ms_hbmem_wait, ms_vflow_fn,
		ms_wait_display,
		ms_prep + ms_hbmem_wait + ms_vflow_fn + ms_wait_display);

	running = 1;
	ret = pthread_create(&read_thread, NULL, (void *)read_vse_data, (void *)&pipe_contex);
	if (ret != 0) {
		printf("\nError: Failed to create reader thread\n");
		goto cleanup;
	}
	pthread_join(read_thread, NULL);

cleanup:
	if (display_thread_started) {
		pthread_join(display_thread, NULL);
		display_thread_started = 0;
	}
	if (hbmem_thread_started) {
		pthread_join(hbmem_thread, NULL);
		hbmem_thread_started = 0;
	}
	if (pipe_contex.vflow_fd >= 0)
		(void)hbn_vflow_stop(pipe_contex.vflow_fd);
	display_deinit();
	if (pipe_contex.vse_node_handle >= 0)
		(void)hbn_vnode_close(pipe_contex.vse_node_handle);
	if (pipe_contex.isp_node_handle >= 0)
		(void)hbn_vnode_close(pipe_contex.isp_node_handle);
	if (pipe_contex.vin_node_handle >= 0)
		(void)hbn_vnode_close(pipe_contex.vin_node_handle);
	if (pipe_contex.cam_fd >= 0)
		(void)hbn_camera_destroy(pipe_contex.cam_fd);
	if (pipe_contex.vflow_fd >= 0)
		(void)hbn_vflow_destroy(pipe_contex.vflow_fd);
	hb_mem_module_close();

	return 0;
}
