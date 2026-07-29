/***************************************************************************
 *                      COPYRIGHT NOTICE
 *             Copyright(C) 2024-2025, D-Robotics Co., Ltd.
 *                     All rights reserved.
 ***************************************************************************
 *
 * time_synchronization
 * --------------------
 * Demonstrates the four VIN frame timestamps when sensor works in SLAVE
 * mode (LPWM trigger on), to help users distinguish their meaning and usage.
 *
 * The four timestamps in hbn_frame_info_t (filled by SIF at frame start):
 *
 *   timestamps      - ktime_get_raw_ns(), raw monotonic ns (CLOCK_MONOTONIC_RAW),
 *                     unconditional. From system boot. NOT NTP-adjusted (unlike
 *                     CLOCK_MONOTONIC which is frequency-corrected by NTP), so
 *                     suitable as a stable sync baseline in userspace.
 *   sys_timestamps  - ktime_get_real_ns(), realtime ns (CLOCK_REALTIME),
 *                     unconditional. Wall clock, NTP-adjusted, may jump.
 *   trig_tv         - SIF IPI_TS_TRIG register, AON RTC, latched at sensor
 *                     exposure trigger. Requires time_stamp_en + TRIGGER mode.
 *   tv              - SIF IPI_TS_VSYNC register, AON RTC, latched at frame
 *                     start (mipi tx). Requires time_stamp_en + VSYNC mode.
 *
 * trig_tv and tv are both AON RTC (same domain); tv - trig_tv is the
 * exposure-to-mipi duration. sys_timestamps (realtime) and tv (AON RTC) are
 * stamped at the same sif frame-start interrupt, so sys_timestamps - tv is
 * the realtime<->AON_RTC offset.
 */

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common_utils.h"

/* ------------------------------------------------------------------------- */
/*          vcon device-tree probe: auto-detect ts_src for this CSI          */
/* ------------------------------------------------------------------------- */

static int dt_read_be32(const char *path, uint32_t *out)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return -1;
	unsigned char raw[4] = {0};
	size_t n = fread(raw, 1, sizeof(raw), fp);
	fclose(fp);
	if (n != sizeof(raw))
		return -1;
	*out = ((uint32_t)raw[0] << 24) | ((uint32_t)raw[1] << 16) |
	       ((uint32_t)raw[2] <<  8) | ((uint32_t)raw[3]);
	return 0;
}

/* Read lpwm_chn from device tree, set ts_src = lpwm_chn + 1, time_stamp_en=1.
 * time_stamp_mode depends on sensor_mode:
 *   SLAVE  -> 0x03 (VSYNC | TRIGGER): trig_tv = LPWM trigger time, valid.
 *   others -> 0x01 (VSYNC only):     no LPWM, so TRIGGER latching is disabled
 *                                    and trig_tv stays 0 (avoid stale register).
 */
static void auto_fill_ts_src(pipe_contex_t *pipe, uint32_t sensor_mode)
{
	vin_node_attr_t *na      = pipe->sensor_config->vin_node_attr;
	uint32_t         mipi_rx = na->cim_attr.mipi_rx;
	char             path[128];

	snprintf(path, sizeof(path),
		 "/proc/device-tree/soc/cam/vcon@%u/lpwm_chn", mipi_rx);

	uint32_t lpwm_chn = 0;
	if (dt_read_be32(path, &lpwm_chn) != 0) {
		printf("Warning: cannot read %s, keep static ts_src=%u.\n",
		       path, na->cim_attr.func.ts_src);
		return;
	}

	if (lpwm_chn > 7) {
		printf("Warning: %s reports lpwm_chn=%u (out of range).\n",
		       path, lpwm_chn);
		return;
	}

	uint32_t auto_ts_src = lpwm_chn + 1;
	uint32_t ts_mode = (sensor_mode == SLAVE_M) ? 0x03 : 0x01;
	na->cim_attr.func.time_stamp_en = 1;
	na->cim_attr.func.time_stamp_mode = ts_mode;
	na->cim_attr.func.ts_src = auto_ts_src;

	printf("vcon@%u: lpwm_chn=%u -> ts_src=%u, time_stamp_en=1, mode=0x%02x\n",
	       mipi_rx, lpwm_chn, auto_ts_src, ts_mode);
}

/* ------------------------------------------------------------------------- */
/*                              VIN pipeline                                 */
/* ------------------------------------------------------------------------- */

int32_t hbn_deserial_create(deserial_config_t *des_config, deserial_handle_t *des_fd);
int32_t hbn_deserial_attach_to_vin(deserial_handle_t des_fd, camera_des_link_t link,
				   vpf_handle_t vin_fd);

static int create_camera_node(pipe_contex_t *pipe, uint32_t sensor_mode)
{
	camera_config_t    *cc = pipe->sensor_config->camera_config;
	vp_sensor_config_t *sc = pipe->sensor_config;
	int32_t             ret;

	if (sensor_mode >= NORMAL_M && sensor_mode < INVALID_MOD) {
		cc->sensor_mode = sensor_mode;
		sc->vin_node_attr->lpwm_attr.enable = (sensor_mode == SLAVE_M) ? 1 : 0;
	}
	ret = hbn_camera_create(cc, &pipe->cam_fd);
	ERR_CON_EQ(ret, 0);
	return 0;
}

static int create_deserial_node(pipe_contex_t *pipe)
{
	deserial_config_t *dc  = pipe->sensor_config->deserial_node_attr;
	int32_t            ret = hbn_deserial_create(dc, &pipe->des_fd);

	if (ret != 0) {
		printf("hbn_deserial_create failed ret=%d\n", ret);
		return ret;
	}
	printf("deserial: 0x%02x %s des_handle=%ld\n",
	       dc->addr, dc->name, pipe->des_fd);
	return 0;
}

static int create_vin_node(pipe_contex_t *pipe)
{
	vp_sensor_config_t  *sc  = pipe->sensor_config;
	vin_node_attr_t     *na  = sc->vin_node_attr;
	vin_attr_ex_t        vax;
	hbn_buf_alloc_attr_t alloc = {0};
	uint64_t             vax_mask = 0;
	int32_t              ret;

	if (pipe->csi_config.mclk_is_not_configed) {
		printf("csi%d: no mclk configured, skip mclk attr.\n",
		       pipe->csi_config.index);
	} else {
		vax.vin_attr_ex_mask         = sc->vin_attr_ex->vin_attr_ex_mask;
		vax.mclk_ex_attr.mclk_freq   = sc->vin_attr_ex->mclk_ex_attr.mclk_freq;
		vax_mask                     = vax.vin_attr_ex_mask;
	}

	ret = hbn_vnode_open(HB_VIN, na->cim_attr.mipi_rx, AUTO_ALLOC_ID,
			     &pipe->vin_node_handle);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_attr(pipe->vin_node_handle, na);
	ERR_CON_EQ(ret, 0);

	ret = hbn_vnode_set_ichn_attr(pipe->vin_node_handle, 0, sc->vin_ichn_attr);
	ERR_CON_EQ(ret, 0);
	sc->vin_ochn_attr->ddr_en = 1;
	ret = hbn_vnode_set_ochn_attr(pipe->vin_node_handle, 0, sc->vin_ochn_attr);
	ERR_CON_EQ(ret, 0);

	if (vax_mask) {
		for (uint8_t i = 0; i < VIN_ATTR_EX_INVALID; i++) {
			if ((vax_mask & (1ULL << i)) == 0)
				continue;
			vax.ex_attr_type = i;
			ret = hbn_vnode_set_attr_ex(pipe->vin_node_handle, &vax);
			ERR_CON_EQ(ret, 0);
		}
	}

	alloc.buffers_num = 3;
	alloc.is_contig   = 1;
	alloc.flags = HB_MEM_USAGE_CPU_READ_OFTEN
		    | HB_MEM_USAGE_CPU_WRITE_OFTEN
		    | HB_MEM_USAGE_CACHED;
	ret = hbn_vnode_set_ochn_buf_attr(pipe->vin_node_handle, 0, &alloc);
	ERR_CON_EQ(ret, 0);
	return 0;
}

static int create_and_run_vflow(pipe_contex_t *pipe, uint32_t sensor_mode)
{
	uint32_t link_port   = pipe->sensor_config->vin_node_attr->cim_attr.vc_index;
	uint32_t sensor_type = pipe->sensor_config->sensor_type;
	int32_t  ret;

	ret = create_camera_node(pipe, sensor_mode);
	ERR_CON_EQ(ret, 0);
	ret = create_vin_node(pipe);
	ERR_CON_EQ(ret, 0);

	ret = hbn_vflow_create(&pipe->vflow_fd);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_add_vnode(pipe->vflow_fd, pipe->vin_node_handle);
	ERR_CON_EQ(ret, 0);

	if (sensor_type != SENSOR_TYPE_NORMAL && sensor_type != SENSOR_TYPE_HSMT_RAW) {
		ret = create_deserial_node(pipe);
		ERR_CON_EQ(ret, 0);
		ret = hbn_camera_attach_to_deserial(pipe->cam_fd, pipe->des_fd,
						    link_port);
		ERR_CON_EQ(ret, 0);
		ret = hbn_deserial_attach_to_vin(pipe->des_fd, link_port,
						 pipe->vin_node_handle);
		ERR_CON_EQ(ret, 0);
	} else {
		ret = hbn_camera_attach_to_vin(pipe->cam_fd, pipe->vin_node_handle);
		ERR_CON_EQ(ret, 0);
	}

	ret = hbn_vflow_start(pipe->vflow_fd);
	ERR_CON_EQ(ret, 0);
	return 0;
}

/* ------------------------------------------------------------------------- */
/*                    Single-frame grab + timestamp dump                     */
/* ------------------------------------------------------------------------- */

static void grab_and_print(pipe_contex_t *pipe)
{
	hbn_vnode_image_t img;
	int ret = hbn_vnode_getframe(pipe->vin_node_handle, 0, 10000, &img);

	if (ret != 0) {
		printf("hbn_vnode_getframe failed ret=%d\n", ret);
		return;
	}

	uint64_t trig_tv_ns = (uint64_t)img.info.trig_tv.tv_sec * 1000000000ULL
			    + (uint64_t)img.info.trig_tv.tv_usec * 1000ULL;
	uint64_t tv_ns = (uint64_t)img.info.tv.tv_sec * 1000000000ULL
		       + (uint64_t)img.info.tv.tv_usec * 1000ULL;

	printf("frame_id=%-6u  %dx%d stride=%d\n",
	       img.info.frame_id,
	       img.buffer.width, img.buffer.height, img.buffer.stride);

	printf("  timestamps       = %llu ns  (raw monotonic, ktime_get_raw_ns; NOT NTP-adjusted)\n",
	       (unsigned long long)img.info.timestamps);
	printf("  sys_timestamps    = %llu ns  (realtime, ktime_get_real_ns; wall clock, NTP-adjusted)\n",
	       (unsigned long long)img.info.sys_timestamps);
	printf("  trig_tv           = %lu.%06lu s  -> %llu ns  (AON RTC, exposure trigger)\n",
	       (unsigned long)img.info.trig_tv.tv_sec, (unsigned long)img.info.trig_tv.tv_usec,
	       (unsigned long long)trig_tv_ns);
	printf("  tv                = %lu.%06lu s  -> %llu ns  (AON RTC, frame start)\n",
	       (unsigned long)img.info.tv.tv_sec, (unsigned long)img.info.tv.tv_usec,
	       (unsigned long long)tv_ns);

	/* Derived relations to help distinguish the four timestamps. */
	if (trig_tv_ns == 0)
		printf("  [trig_tv is zero] time_stamp_en off or TRIGGER mode not set.\n");
	if (tv_ns == 0)
		printf("  [tv is zero] time_stamp_en off or VSYNC mode not set.\n");
	if (trig_tv_ns != 0 && tv_ns != 0) {
		int64_t exp_to_mipi_ns = (int64_t)(tv_ns - trig_tv_ns);
		printf("  exposure->mipi duration (tv - trig_tv)    = %+lld ns = %+.3f us\n",
		       (long long)exp_to_mipi_ns, exp_to_mipi_ns / 1000.0);
	}

	hbn_vnode_releaseframe(pipe->vin_node_handle, 0, &img);
}

/* ------------------------------------------------------------------------- */
/*                                CLI / main                                 */
/* ------------------------------------------------------------------------- */

static struct option long_options[] = {
	{"sensor", required_argument, 0, 's'},
	{"mode",   required_argument, 0, 'm'},
	{"help",   no_argument,       0, 'h'},
	{0, 0, 0, 0}
};

static void print_help(void)
{
	printf("Usage: time_synchronization [OPTIONS]\n");
	printf("  Open a single VIN pipeline and dump the four frame timestamps\n");
	printf("  on every getframe, to help distinguish their meaning and usage.\n\n");
	printf("Options:\n");
	printf("  -s <index>   Sensor index (required)\n");
	printf("  -m <m|s>     Sensor mode: m=Master, s=Slave (default: sensor config mode)\n");
	printf("               Slave mode enables LPWM trigger (trig_tv valid);\n");
	printf("               Master mode leaves LPWM off (trig_tv is zero).\n");
	printf("  -h           Show this help\n");
	vp_show_sensors_list();
}

static void print_command_help(void)
{
	printf("\n***************  Command Lists  ***************\n");
	printf(" g  -- grab one frame and print four timestamps\n");
	printf(" l  -- grab 12 frames and print timestamps\n");
	printf(" q  -- quit\n");
	printf(" h  -- this help\n");
}

static void handle_user_command(pipe_contex_t *pipe)
{
	int  running = 1;
	char option;

	print_command_help();
	printf("\nCommand: ");
	while (running && ((option = (char)getchar()) != EOF)) {
		switch (option) {
		case 'q':
			printf("quit\n");
			running = 0;
			return;
		case 'g':
			grab_and_print(pipe);
			break;
		case 'l':
			for (int j = 0; j < 12; j++)
				grab_and_print(pipe);
			break;
		case 'h':
			print_command_help();
			break;
		case '\n':
		case '\r':
			continue;
		default:
			printf("Unknown command.\n");
			print_command_help();
			break;
		}
		printf("\nCommand: ");
	}
}

int main(int argc, char **argv)
{
	int           ret         = 0;
	int           opt_index   = 0;
	int           c           = 0;
	int           sensor_idx  = -1;
	uint32_t      sensor_mode = INVALID_MOD;   /* default: use sensor config mode */
	pipe_contex_t pipe        = {0};

	while ((c = getopt_long(argc, argv, "s:m:h", long_options, &opt_index)) != -1) {
		switch (c) {
		case 's':
			sensor_idx = atoi(optarg);
			break;
		case 'm':
			if (strcmp(optarg, "m") == 0)
				sensor_mode = NORMAL_M;
			else if (strcmp(optarg, "s") == 0)
				sensor_mode = SLAVE_M;
			else {
				printf("Invalid sensor mode: %s (use 'm' or 's')\n", optarg);
				return -1;
			}
			break;
		case 'h':
		default:
			print_help();
			return 0;
		}
	}

	if (sensor_idx < 0) {
		printf("No sensor specified. Use -s <index>.\n");
		print_help();
		return 0;
	}

	if (sensor_idx >= vp_get_sensors_list_number() || sensor_idx < 0) {
		printf("Unsupported sensor index: %d\n", sensor_idx);
		print_help();
		return 0;
	}
	pipe.sensor_config = vp_sensor_config_list[sensor_idx];
	printf("Sensor index=%d  name=%s  config=%s\n",
	       sensor_idx,
	       pipe.sensor_config->sensor_name,
	       pipe.sensor_config->config_file);

	if (pipe.sensor_config->sensor_type == SENSOR_TYPE_NORMAL) {
		ret = vp_sensor_fixed_mipi_host(pipe.sensor_config, &pipe.csi_config);
		if (ret != 0) {
			printf("No camera sensor found for index %d.\n", sensor_idx);
			return ret;
		}
	}

	if (sensor_mode != INVALID_MOD) {
		int found = 0;
		for (int j = 0; j < SENSOR_MODE_SUPPORT_COUNT; j++) {
			if (sensor_mode == pipe.sensor_config->support_sensor_mode[j])
				found = 1;
		}
		int dflt = pipe.sensor_config->camera_config->sensor_mode;
		if (!found && sensor_mode != (uint32_t)dflt) {
			printf("Error: sensor(%s) does not support mode %s\n",
			       pipe.sensor_config->sensor_name,
			       sensor_mode_to_str(sensor_mode));
			return -1;
		}
		printf("Sensor mode: %s\n", sensor_mode_to_str(sensor_mode));
	} else {
		sensor_mode = pipe.sensor_config->camera_config->sensor_mode;
		printf("Sensor mode: %s (sensor default, not specified via -m)\n",
		       sensor_mode_to_str(sensor_mode));
	}
	if (sensor_mode != SLAVE_M)
		printf("Warning: not Slave mode - LPWM is OFF, trig_tv will be zero.\n");

	hb_mem_module_open();

	auto_fill_ts_src(&pipe, sensor_mode);

	ret = create_and_run_vflow(&pipe, sensor_mode);
	if (ret != 0) {
		printf("create_and_run_vflow failed ret=%d\n", ret);
		goto out;
	}

	handle_user_command(&pipe);

	hbn_vflow_stop(pipe.vflow_fd);
	hbn_vnode_close(pipe.vin_node_handle);
	hbn_camera_destroy(pipe.cam_fd);
	hbn_vflow_destroy(pipe.vflow_fd);
	hb_mem_module_close();

out:
	printf("VIN resources released.\n");
	return ret;
}
