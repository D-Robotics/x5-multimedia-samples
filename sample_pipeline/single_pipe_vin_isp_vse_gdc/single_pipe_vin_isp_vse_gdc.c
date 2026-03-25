/***************************************************************************
 *                      COPYRIGHT NOTICE
 *             Copyright(C) 2024, D-Robotics
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
#include <signal.h>

#include "hbn_api.h"
#include "gdc_cfg.h"
#include "gdc_bin_cfg.h"
#include "common_utils.h"
#include "channel_param_parser.h"

#define VSE_MAX_CHANNELS 6

typedef struct gdc_info {
	hb_mem_common_buf_t bin_buf[4];
	uint32_t bin_size[4];
	uint32_t input_width;
	uint32_t input_height;
} gdc_info_s;

static struct option const long_options[] = {
	{"sensor", required_argument, NULL, 's'},
	{"channel-type", optional_argument, NULL, 'c'},
	{"help", no_argument, NULL, 'h'},
	{NULL, 0, NULL, 0}
};

extern int vin_isp_is_online;
extern int isp_vse_is_online;

static uint32_t sensor_type = 0;
static uint32_t link_port = 0;
static int32_t running = 0;

int32_t hbn_deserial_create(deserial_config_t *des_config, deserial_handle_t *des_fd);
int32_t hbn_deserial_attach_to_vin(deserial_handle_t des_fd, camera_des_link_t link, vpf_handle_t vin_fd);

static void print_help(const char *argv0) {
	printf("Usage: %s [OPTIONS]\n", argv0);
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

static int create_camera_node(pipe_contex_t *pipe_contex) {
	camera_config_t *camera_config = NULL;
	vp_sensor_config_t *sensor_config = NULL;
	int32_t ret = 0;

	sensor_config = pipe_contex->sensor_config;
	camera_config = sensor_config->camera_config;
	ret = hbn_camera_create(camera_config, &pipe_contex->cam_fd);
	ERR_CON_EQ(ret, 0);

	return 0;
}

static int create_deserial_node(pipe_contex_t *pipe_contex) {
	vp_sensor_config_t *sensor_config = NULL;
	deserial_config_t *deserial_config = NULL;
	deserial_handle_t *des_handle = NULL;
	int32_t ret = 0;

	des_handle = &pipe_contex->des_fd;
	sensor_config = pipe_contex->sensor_config;
	deserial_config = sensor_config->deserial_node_attr;

	ret = hbn_deserial_create(deserial_config, des_handle);
	if (ret != 0) {
		printf("hbn_deserial_create failed ret = %d\n", ret);
		return ret;
	}
	printf("deserial_config:%02x_%s, des_handle:%ld\n",
		   deserial_config->addr, deserial_config->name, *des_handle);
	return 0;
}

static int create_vin_node(pipe_contex_t *pipe_contex) {
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
	hw_id = vin_node_attr->cim_attr.mipi_rx;
	vin_node_handle = &pipe_contex->vin_node_handle;

	link_port = vin_node_attr->cim_attr.vc_index;

	if (pipe_contex->csi_config.mclk_is_not_configed) {
		printf("csi%d ignore mclk ex attr, because not config mclk.\n",
			   pipe_contex->csi_config.index);
	} else {
		vin_attr_ex.vin_attr_ex_mask = sensor_config->vin_attr_ex->vin_attr_ex_mask;
		vin_attr_ex.mclk_ex_attr.mclk_freq = sensor_config->vin_attr_ex->mclk_ex_attr.mclk_freq;
		vin_attr_ex_mask = vin_attr_ex.vin_attr_ex_mask;
	}

	if (vin_isp_is_online) {
		sensor_config->vin_node_attr->cim_attr.cim_isp_flyby = 1;
		sensor_config->vin_ochn_attr->ddr_en = 0;
	} else {
		sensor_config->vin_node_attr->cim_attr.cim_isp_flyby = 0;
		sensor_config->vin_ochn_attr->ddr_en = 1;
	}

	ret = hbn_vnode_open(HB_VIN, hw_id, AUTO_ALLOC_ID, vin_node_handle);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_attr(*vin_node_handle, vin_node_attr);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_ichn_attr(*vin_node_handle, ichn_id, vin_ichn_attr);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_ochn_attr(*vin_node_handle, ochn_id, vin_ochn_attr);
	ERR_CON_EQ(ret, 0);

	if (vin_attr_ex_mask) {
		for (uint8_t i = 0; i < VIN_ATTR_EX_INVALID; i++) {
			if ((vin_attr_ex_mask & (1 << i)) == 0)
				continue;
			vin_attr_ex.ex_attr_type = i;
			ret = hbn_vnode_set_attr_ex(*vin_node_handle, &vin_attr_ex);
			ERR_CON_EQ(ret, 0);
		}
	}

	if (!vin_isp_is_online) {
		hbn_buf_alloc_attr_t alloc_attr = {0};
		alloc_attr.buffers_num = 3;
		alloc_attr.is_contig = 1;
		alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN | HB_MEM_USAGE_CPU_WRITE_OFTEN | HB_MEM_USAGE_CACHED;
		ret = hbn_vnode_set_ochn_buf_attr(*vin_node_handle, ochn_id, &alloc_attr);
		ERR_CON_EQ(ret, 0);
	}

	return 0;
}

static int create_isp_node(pipe_contex_t *pipe_contex) {
	vp_sensor_config_t *sensor_config = NULL;
	isp_attr_t *isp_attr = NULL;
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

	if (vin_isp_is_online) {
		sensor_config->isp_attr->input_mode = PASSTHROUGH_MODE;
	} else {
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
		alloc_attr.buffers_num = 3;
		alloc_attr.is_contig = 1;
		alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN | HB_MEM_USAGE_CPU_WRITE_OFTEN | HB_MEM_USAGE_CACHED;
		ret = hbn_vnode_set_ochn_buf_attr(*isp_node_handle, ochn_id, &alloc_attr);
		ERR_CON_EQ(ret, 0);
	}

	return 0;
}

static int check_vse_scale_ratio(uint32_t input_width, uint32_t input_height,
								 uint32_t output_width, uint32_t output_height,
								 uint32_t roi_width, uint32_t roi_height) {
	uint32_t effective_input_width = (roi_width > 0) ? roi_width : input_width;
	uint32_t effective_input_height = (roi_height > 0) ? roi_height : input_height;

	int width_scale = (output_width > effective_input_width) ? 1 :
					  (output_width < effective_input_width) ? -1 : 0;
	int height_scale = (output_height > effective_input_height) ? 1 :
					   (output_height < effective_input_height) ? -1 : 0;

	if ((width_scale == 1 && height_scale == -1) || (width_scale == -1 && height_scale == 1)) {
		printf("VSE does not allow one dimension to scale up while the other scales down\n");
		return -1;
	}

	return 0;
}

static int check_vse_output_valid(uint32_t input_width, uint32_t input_height, vse_ochn_attr_t *vse_ochn_attr) {
	for (int i = 0; i < VSE_MAX_CHANNELS; ++i) {
		int ret = check_vse_scale_ratio(input_width, input_height,
										vse_ochn_attr[i].target_w, vse_ochn_attr[i].target_h,
										vse_ochn_attr[i].roi.w, vse_ochn_attr[i].roi.h);
		if (ret != 0) {
			printf("Error: VSE Output channel %d resolution %dx%d scaling ratio invalid!\n",
				   i, vse_ochn_attr[i].target_w, vse_ochn_attr[i].target_h);
			return -1;
		}
	}

	return 0;
}

static int create_vse_node(pipe_contex_t *pipe_contex) {
	int ret = 0;
	hbn_vnode_handle_t *vse_node_handle = &pipe_contex->vse_node_handle;
	isp_ichn_attr_t isp_ichn_attr = {0};
	vse_attr_t vse_attr = {0};
	vse_ichn_attr_t vse_ichn_attr = {0};
	vse_ochn_attr_t vse_ochn_attr[VSE_MAX_CHANNELS] = {0};
	uint32_t ichn_id = 0;
	uint32_t hw_id = 0;
	uint32_t input_width = 0;
	uint32_t input_height = 0;
	hbn_buf_alloc_attr_t alloc_attr = {0};

	ret = hbn_vnode_get_ichn_attr(pipe_contex->isp_node_handle, ichn_id, &isp_ichn_attr);
	ERR_CON_EQ(ret, 0);
	input_width = isp_ichn_attr.width;
	input_height = isp_ichn_attr.height;

	vse_ichn_attr.width = input_width;
	vse_ichn_attr.height = input_height;
	vse_ichn_attr.fmt = FRM_FMT_NV12;
	vse_ichn_attr.bit_width = 8;

	for (int i = 0; i < VSE_MAX_CHANNELS; ++i) {
		vse_ochn_attr[i].chn_en = CAM_TRUE;
		vse_ochn_attr[i].roi.x = 0;
		vse_ochn_attr[i].roi.y = 0;
		vse_ochn_attr[i].roi.w = input_width;
		vse_ochn_attr[i].roi.h = input_height;
		vse_ochn_attr[i].fmt = FRM_FMT_NV12;
		vse_ochn_attr[i].bit_width = 8;
	}

	vse_ochn_attr[0].target_w = input_width;
	vse_ochn_attr[0].target_h = input_height;
	vse_ochn_attr[1].target_w = 512;
	vse_ochn_attr[1].target_h = 480;
	vse_ochn_attr[2].target_w = 224;
	vse_ochn_attr[2].target_h = 224;
	vse_ochn_attr[3].roi.x = input_width / 2 - input_width / 4;
	vse_ochn_attr[3].roi.y = input_height / 2 - input_height / 4;
	vse_ochn_attr[3].roi.w = input_width / 2;
	vse_ochn_attr[3].roi.h = input_height / 2;
	vse_ochn_attr[3].target_w = 64;
	vse_ochn_attr[3].target_h = 64;
	vse_ochn_attr[4].target_w = 480;
	vse_ochn_attr[4].target_h = 480;
	vse_ochn_attr[5].target_w = (input_width * 2) > 4096 ? 4096 : (input_width * 2);
	vse_ochn_attr[5].target_h = (input_height * 2) > 3076 ? 3076 : (input_height * 2);

	ret = check_vse_output_valid(input_width, input_height, vse_ochn_attr);
	if (ret != 0) {
		return -1;
	}

	ret = hbn_vnode_open(HB_VSE, hw_id, AUTO_ALLOC_ID, vse_node_handle);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_attr(*vse_node_handle, &vse_attr);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_ichn_attr(*vse_node_handle, ichn_id, &vse_ichn_attr);
	ERR_CON_EQ(ret, 0);

	alloc_attr.buffers_num = 3;
	alloc_attr.is_contig = 1;
	alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN | HB_MEM_USAGE_CPU_WRITE_OFTEN | HB_MEM_USAGE_CACHED;

	for (int i = 0; i < VSE_MAX_CHANNELS; ++i) {
		ret = hbn_vnode_set_ochn_attr(*vse_node_handle, i, &vse_ochn_attr[i]);
		ERR_CON_EQ(ret, 0);
		ret = hbn_vnode_set_ochn_buf_attr(*vse_node_handle, i, &alloc_attr);
		ERR_CON_EQ(ret, 0);
	}

	return 0;
}

static void init_gdc_windows(window_t *windows, uint32_t width, uint32_t height, int index) {
	memset(windows, 0, sizeof(*windows));
	windows->strength = 1.0;
	windows->strengthY = 1.0;
	windows->angle = 0;
	windows->elevation = 0;
	windows->azimuth = 0;
	windows->keep_ratio = 1;
	windows->FOV_h = 90;
	windows->FOV_w = 90;
	windows->cylindricity_y = 0;
	windows->cylindricity_x = 0;
	windows->trapezoid_left_angle = 90;
	windows->trapezoid_right_angle = 90;
	windows->pan = 0;
	windows->tilt = 0;
	windows->zoom = 1.0;
	windows->out_r.x = 0;
	windows->out_r.y = 0;
	windows->out_r.w = width;
	windows->out_r.h = height;
	windows->transform = AFFINE;

	switch (index) {
	case 0:
		windows->input_roi_r.x = 0;
		windows->input_roi_r.y = 0;
		windows->input_roi_r.w = width / 2;
		windows->input_roi_r.h = height / 2;
		break;
	case 1:
		windows->input_roi_r.x = width / 2;
		windows->input_roi_r.y = 0;
		windows->input_roi_r.w = width / 2;
		windows->input_roi_r.h = height / 2;
		break;
	case 2:
		windows->input_roi_r.x = 0;
		windows->input_roi_r.y = height / 2;
		windows->input_roi_r.w = width / 2;
		windows->input_roi_r.h = height / 2;
		break;
	case 3:
	default:
		windows->input_roi_r.x = width / 2;
		windows->input_roi_r.y = height / 2;
		windows->input_roi_r.w = width / 2;
		windows->input_roi_r.h = height / 2;
		break;
	}
}

static int32_t create_gdc_node(pipe_contex_t *pipe_contex, gdc_info_s *gdc_info) {
	int ret = 0;
	uint32_t hw_id = 0;
	uint32_t ichn_id = 0;
	uint32_t ochn_id = 0;
	gdc_attr_t gdc_attr = {0};
	gdc_ochn_attr_t gdc_ochn_attr = {0};
	hbn_buf_alloc_attr_t alloc_attr = {0};
	int64_t alloc_flags = 0;
	param_t gdc_param = {0};
	window_t windows = {0};
	uint32_t wnd_num = 1;
	int offset = 0;
	vse_ochn_attr_t vse_ochn_attr = {0};

	ret = hbn_vnode_get_ochn_attr(pipe_contex->vse_node_handle, 0, &vse_ochn_attr);
	ERR_CON_EQ(ret, 0);
	gdc_info->input_width = vse_ochn_attr.target_w;
	gdc_info->input_height = vse_ochn_attr.target_h;

	memset(gdc_info->bin_buf, 0, sizeof(gdc_info->bin_buf));
	memset(gdc_info->bin_size, 0, sizeof(gdc_info->bin_size));
	alloc_flags = HB_MEM_USAGE_MAP_INITIALIZED
				| HB_MEM_USAGE_PRIV_HEAP_2_RESERVERD
				| HB_MEM_USAGE_CPU_READ_OFTEN
				| HB_MEM_USAGE_CPU_WRITE_OFTEN
				| HB_MEM_USAGE_CACHED;

	for (int i = 0; i < 4; ++i) {
		uint32_t *cfg_buf = NULL;
		uint64_t size = 0;

		init_gdc_windows(&windows, gdc_info->input_width, gdc_info->input_height, i);
		memset(&gdc_param, 0, sizeof(gdc_param));
		gdc_param.in.w = gdc_info->input_width;
		gdc_param.in.h = gdc_info->input_height;
		gdc_param.out.w = gdc_info->input_width;
		gdc_param.out.h = gdc_info->input_height;
		gdc_param.fov = 180;
		gdc_param.diameter = gdc_info->input_height;
		gdc_param.x_offset = 0;
		gdc_param.y_offset = 0;
		gdc_param.format = FMT_SEMIPLANAR_420;

		ret = hbn_gen_gdc_bin(&gdc_param, &windows, wnd_num, &cfg_buf, &size);
		if (ret != 0 || cfg_buf == NULL) {
			printf("hbn_gen_gdc_bin failed for bin[%d]\n", i);
			return -1;
		}

		ret = hb_mem_alloc_com_buf(size, alloc_flags, &gdc_info->bin_buf[i]);
		if (ret != 0 || gdc_info->bin_buf[i].virt_addr == NULL) {
			printf("hb_mem_alloc_com_buf for bin[%d] failed, ret = %d\n", i, ret);
			hbn_free_gdc_bin(cfg_buf);
			return -1;
		}

		memcpy(gdc_info->bin_buf[i].virt_addr, cfg_buf, size);
		ret = hb_mem_flush_buf(gdc_info->bin_buf[i].fd, offset, size);
		ERR_CON_EQ(ret, 0);
		gdc_info->bin_size[i] = (uint32_t)size;

		hbn_free_gdc_bin(cfg_buf);
	}

	ret = hbn_vnode_open(HB_GDC, hw_id, AUTO_ALLOC_ID, &(pipe_contex->gdc_node_handle));
	if (ret != 0) {
		printf("open GDC vnode failed %d\n", ret);
		return -1;
	}

	gdc_attr.config_addr = gdc_info->bin_buf[0].phys_addr;
	gdc_attr.config_size = gdc_info->bin_size[0];
	gdc_attr.binary_ion_id = gdc_info->bin_buf[0].share_id;
	gdc_attr.binary_offset = gdc_info->bin_buf[0].offset;
	gdc_attr.total_planes = 2;
	gdc_attr.div_width = 0;
	gdc_attr.div_height = 0;
	ret = hbn_vnode_set_attr(pipe_contex->gdc_node_handle, &gdc_attr);
	if (ret != 0) {
		printf("gdc set attr failed %d\n", ret);
		return -1;
	}

	gdc_ichn_attr_t gdc_ichn_attr = {0};
	gdc_ichn_attr.input_width = gdc_info->input_width;
	gdc_ichn_attr.input_height = gdc_info->input_height;
	gdc_ichn_attr.input_stride = gdc_info->input_width;
	ret = hbn_vnode_set_ichn_attr(pipe_contex->gdc_node_handle, ichn_id, &gdc_ichn_attr);
	if (ret != 0) {
		printf("gdc set ichn failed %d\n", ret);
		return -1;
	}

	gdc_ochn_attr.output_width = gdc_info->input_width;
	gdc_ochn_attr.output_height = gdc_info->input_height;
	gdc_ochn_attr.output_stride = gdc_info->input_width;
	ret = hbn_vnode_set_ochn_attr(pipe_contex->gdc_node_handle, ochn_id, &gdc_ochn_attr);
	if (ret != 0) {
		printf("gdc set ochn failed %d\n", ret);
		return -1;
	}

	alloc_attr.buffers_num = 3;
	alloc_attr.is_contig = 1;
	alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN | HB_MEM_USAGE_CPU_WRITE_OFTEN | HB_MEM_USAGE_CACHED;
	ret = hbn_vnode_set_ochn_buf_attr(pipe_contex->gdc_node_handle, ochn_id, &alloc_attr);
	if (ret != 0) {
		printf("gdc set ochn buffer failed %d\n", ret);
		return -1;
	}

	return 0;
}

static int capture_gdc_quad_frames(pipe_contex_t *pipe_contex, gdc_info_s *gdc_info) {
	int ret = 0;
	uint32_t gdc_chn = 0;
	uint32_t timeout = 2000;
	hbn_vnode_image_t gdc_img = {0};
	gdc_attr_t gdc_attr = {0};

	for (uint32_t quad = 0; quad < 4 && running; ++quad) {
		memset(&gdc_img, 0, sizeof(gdc_img));
		ret = hbn_vnode_getframe(pipe_contex->gdc_node_handle, gdc_chn, timeout, &gdc_img);
		if (ret != 0) {
			printf("hbn_vnode_getframe from GDC failed, quad=%u ret=%d\n", quad, ret);
			running = 0;
			break;
		}

		for (int j = 0; j < 2; ++j) {
			if (gdc_img.buffer.virt_addr[j] != NULL) {
				hb_mem_invalidate_buf_with_vaddr((uint64_t)gdc_img.buffer.virt_addr[j], gdc_img.buffer.size[j]);
			}
		}

		{
			char dst_file[128] = {0};
			snprintf(dst_file, sizeof(dst_file), "gdc_quad%u_%dx%d_stride_%d.yuv",
					 quad, gdc_img.buffer.width, gdc_img.buffer.height, gdc_img.buffer.stride);
			if (gdc_img.buffer.virt_addr[0] != NULL && gdc_img.buffer.virt_addr[1] != NULL) {
				(void)dump_2plane_yuv_to_file(dst_file,
											  (uint8_t *)gdc_img.buffer.virt_addr[0],
											  (uint8_t *)gdc_img.buffer.virt_addr[1],
											  (uint32_t)gdc_img.buffer.size[0],
											  (uint32_t)gdc_img.buffer.size[1]);
			}
		}

		if (quad < 3 && running) {
			ret = hbn_vnode_get_attr_ex(pipe_contex->gdc_node_handle, &gdc_attr);
			if (ret != 0) {
				printf("hbn_vnode_get_attr_ex failed, quad=%u ret=%d\n", quad, ret);
				running = 0;
			} else {
				uint32_t next = quad + 1;
				gdc_attr.config_addr = gdc_info->bin_buf[next].phys_addr;
				gdc_attr.config_size = gdc_info->bin_size[next];
				gdc_attr.binary_ion_id = gdc_info->bin_buf[next].share_id;
				gdc_attr.binary_offset = gdc_info->bin_buf[next].offset;
				gdc_attr.total_planes = 2;
				gdc_attr.div_width = 0;
				gdc_attr.div_height = 0;
				ret = hbn_vnode_set_attr_ex(pipe_contex->gdc_node_handle, &gdc_attr);
				if (ret != 0) {
					printf("hbn_vnode_set_attr_ex failed, next_bin=%u ret=%d\n", next, ret);
					running = 0;
				}
			}
		}

		hbn_vnode_releaseframe(pipe_contex->gdc_node_handle, gdc_chn, &gdc_img);
		printf("Captured GDC frame for quad=%u\n", quad);
	}

	return 0;
}

static int create_and_run_vflow(pipe_contex_t *pipe_contex, gdc_info_s *gdc_info) {
	int32_t ret = 0;
	uint32_t vin_src_out = 0;
	uint32_t vin_dst_in = 0;
	uint32_t isp_to_vse_src = 0;
	uint32_t vse_to_gdc_src = 0;
	uint32_t dst_input_channel = 0;

	ret = create_camera_node(pipe_contex);
	ERR_CON_EQ(ret, 0);
	ret = create_vin_node(pipe_contex);
	ERR_CON_EQ(ret, 0);
	ret = create_isp_node(pipe_contex);
	ERR_CON_EQ(ret, 0);
	ret = create_vse_node(pipe_contex);
	ERR_CON_EQ(ret, 0);
	ret = create_gdc_node(pipe_contex, gdc_info);
	ERR_CON_EQ(ret, 0);

	ret = hbn_vflow_create(&pipe_contex->vflow_fd);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_add_vnode(pipe_contex->vflow_fd, pipe_contex->vin_node_handle);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_add_vnode(pipe_contex->vflow_fd, pipe_contex->isp_node_handle);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_add_vnode(pipe_contex->vflow_fd, pipe_contex->vse_node_handle);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_add_vnode(pipe_contex->vflow_fd, pipe_contex->gdc_node_handle);
	ERR_CON_EQ(ret, 0);

	if (vin_isp_is_online) {
		vin_src_out = 1;
	} else {
		vin_src_out = 0;
	}
	if (isp_vse_is_online) {
		isp_to_vse_src = 1;
	} else {
		isp_to_vse_src = 0;
	}

	ret = hbn_vflow_bind_vnode(pipe_contex->vflow_fd,
							   pipe_contex->vin_node_handle, vin_src_out,
							   pipe_contex->isp_node_handle, vin_dst_in);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_bind_vnode(pipe_contex->vflow_fd,
							   pipe_contex->isp_node_handle, isp_to_vse_src,
							   pipe_contex->vse_node_handle, 0);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_bind_vnode(pipe_contex->vflow_fd,
							   pipe_contex->vse_node_handle, vse_to_gdc_src,
							   pipe_contex->gdc_node_handle, dst_input_channel);
	ERR_CON_EQ(ret, 0);

	if (sensor_type != SENSOR_TYPE_NORMAL) {
		ret = create_deserial_node(pipe_contex);
		ERR_CON_EQ(ret, 0);
		ret = hbn_camera_attach_to_deserial(pipe_contex->cam_fd, pipe_contex->des_fd, link_port);
		ERR_CON_EQ(ret, 0);
		ret = hbn_deserial_attach_to_vin(pipe_contex->des_fd, link_port, pipe_contex->vin_node_handle);
		ERR_CON_EQ(ret, 0);
	} else {
		ret = hbn_camera_attach_to_vin(pipe_contex->cam_fd, pipe_contex->vin_node_handle);
		ERR_CON_EQ(ret, 0);
	}

	ret = hbn_vflow_start(pipe_contex->vflow_fd);
	ERR_CON_EQ(ret, 0);

	return 0;
}

int main(int argc, char **argv) {
	int ret = 0;
	pipe_contex_t pipe_contex = {0};
	gdc_info_s gdc_info = {0};
	int opt_index = 0;
	int c = 0;
	int index = -1;

	while ((c = getopt_long(argc, argv, "s:c:h", long_options, &opt_index)) != -1) {
		switch (c) {
		case 's':
			index = atoi(optarg);
			break;
		case 'c':
			if (parse_channel_string(optarg) != 0) {
				printf("Invalid channel type %s.\n", optarg);
				return -1;
			}
			break;
		case 'h':
		default:
			print_help(argv[0]);
			return 0;
		}
	}

	if (index < vp_get_sensors_list_number() && index >= 0) {
		pipe_contex.sensor_config = vp_sensor_config_list[index];
		printf("Using index:%d  sensor_name:%s  config_file:%s\n",
			   index,
			   vp_sensor_config_list[index]->sensor_name,
			   vp_sensor_config_list[index]->config_file);
		sensor_type = pipe_contex.sensor_config->sensor_type;
		if (sensor_type == SENSOR_TYPE_NORMAL) {
			ret = vp_sensor_fixed_mipi_host(pipe_contex.sensor_config, &pipe_contex.csi_config);
			if (ret != 0) {
				printf("No Camera Sensor found. Please check if the specified sensor is connected to the Camera interface.\n");
				return ret;
			}
		}
	} else {
		printf("Unsupport sensor index:%d\n", index);
		print_help(argv[0]);
		return 0;
	}

	if ((pipe_contex.sensor_config->camera_config->sensor_mode == DOL2_M) && (vin_isp_is_online == 0)) {
		printf("\nError:%s's sensor_mode is DOL2_M, must work in online mode.\n\n",
			   pipe_contex.sensor_config->sensor_name);
		return -1;
	}

	if (pipe_contex.sensor_config->isp_ochn_attr->ddr_en == 0) {
		printf("\nError: %s's isp_attr ddr_en is 0, isp and vse/gdc only support offline mode, so must be 1.\n\n",
			   pipe_contex.sensor_config->sensor_name);
		return -1;
	}

	printf("\n");
	printf("Connection method from VIN to ISP: %s\n",
		   (vin_isp_is_online == 1) ? "vin online isp" : "vin offline isp");
	printf("Connection method from ISP to VSE: %s\n",
		   (isp_vse_is_online == 1) ? "isp online vse" : "isp offline vse");
	printf("Connection method from VSE to GDC: offline\n");
	printf("\n");

	hb_mem_module_open();
	ret = create_and_run_vflow(&pipe_contex, &gdc_info);
	ERR_CON_EQ(ret, 0);
	running = 1;
	ret = capture_gdc_quad_frames(&pipe_contex, &gdc_info);
	ERR_CON_EQ(ret, 0);
	running = 0;

	ret = hbn_vflow_stop(pipe_contex.vflow_fd);
	ERR_CON_EQ(ret, 0);
	hbn_vnode_close(pipe_contex.gdc_node_handle);
	hbn_vnode_close(pipe_contex.vse_node_handle);
	hbn_vnode_close(pipe_contex.isp_node_handle);
	hbn_vnode_close(pipe_contex.vin_node_handle);
	hbn_camera_destroy(pipe_contex.cam_fd);
	hbn_vflow_destroy(pipe_contex.vflow_fd);
	hb_mem_module_close();

	return 0;
}
