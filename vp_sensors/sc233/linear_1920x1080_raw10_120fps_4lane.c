#include "vp_sensors.h"

#define SENSOR_WIDTH  1920
#define SENSOR_HEIGHT  1080
#define SENSOE_FPS 120
#define RAW10 0x2B

static mipi_config_t sc233_mipi_config = {
	.rx_enable = 1,
	.rx_attr = {
		.phy = 0,
		.lane = 4,
		.datatype = RAW10,
		.fps = SENSOE_FPS,
		.mclk = 1,
		.mipiclk = 3240,
		.width = SENSOR_WIDTH,
		.height = SENSOR_HEIGHT,
		.linelenth = 2160,
		.framelenth = 1250,
		.settle = 0,
		.channel_num = 1,
		.channel_sel = {0},
	},
	.rx_ex_mask = 0x40,
	.rx_attr_ex = {
		.stop_check_instart = 1,
	},
};

static camera_config_t sc233_camera_config = {
	.name = "sc233",
	.addr = 0x30,
	.sensor_mode = NORMAL_M,
	.fps = SENSOE_FPS,
	.format = RAW10,
	.width = SENSOR_WIDTH,
	.height = SENSOR_HEIGHT,
	.gpio_enable_bit = 0x01,
	.gpio_level_bit = 0x00,
	.mipi_cfg = &sc233_mipi_config,
	.calib_lname = "/usr/hobot/lib/sensor/sc233_tuning.json",
	.config_index = 1, /* 4lane */
};

static vin_node_attr_t sc233_vin_node_attr = {
	.cim_attr = {
		.mipi_rx = 0,
		.vc_index = 0,
		.ipi_channel = 1,
		.cim_isp_flyby = 1,
		.func = {
			.enable_frame_id = 1,
			.set_init_frame_id = 0,
			.hdr_mode = NOT_HDR,
			.time_stamp_en = 0,
		},
	},
	.lpwm_attr = {
		.enable = 0,
		.lpwm_chn_attr = {
			{	.trigger_source = 0,
				.trigger_mode = 0,
				.period = 8333,
				.offset = 10,
				.duty_time = 100,
				.threshold = 0,
				.adjust_step = 0,
			},
			{	.trigger_source = 0,
				.trigger_mode = 0,
				.period = 8333,
				.offset = 10,
				.duty_time = 100,
				.threshold = 0,
				.adjust_step = 0,
			},
			{	.trigger_source = 0,
				.trigger_mode = 0,
				.period = 8333,
				.offset = 10,
				.duty_time = 100,
				.threshold = 0,
				.adjust_step = 0,
			},
			{	.trigger_source = 0,
				.trigger_mode = 0,
				.period = 8333,
				.offset = 10,
				.duty_time = 100,
				.threshold = 0,
				.adjust_step = 0,
			},
		},
	},
};

static vin_attr_ex_t sc233_vin_attr_ex = {
	.vin_attr_ex_mask = 0x80,
	.mclk_ex_attr = {
		.mclk_freq = 24000000,
	},
};

static vin_ichn_attr_t sc233_vin_ichn_attr = {
	.width = SENSOR_WIDTH,
	.height = SENSOR_HEIGHT,
	.format = RAW10,
};

static vin_ochn_attr_t sc233_vin_ochn_attr = {
	.ddr_en = 1,
	.ochn_attr_type = VIN_BASIC_ATTR,
	.vin_basic_attr = {
		.format = RAW10,
		/* raw10: stride = width * 2 */
		.wstride = (SENSOR_WIDTH) * 2,
	},
};

static isp_attr_t sc233_isp_attr = {
	.input_mode = PASSTHROUGH_MODE,
	.sensor_mode = ISP_NORMAL_M,
	.crop = {
		.x = 0,
		.y = 0,
		.h = SENSOR_HEIGHT,
		.w = SENSOR_WIDTH,
	},
};

static isp_ichn_attr_t sc233_isp_ichn_attr = {
	.width = SENSOR_WIDTH,
	.height = SENSOR_HEIGHT,
	.fmt = FRM_FMT_RAW,
	.bit_width = 10,
};

static isp_ochn_attr_t sc233_isp_ochn_attr = {
	.ddr_en = 1,
	.fmt = FRM_FMT_NV12,
	.bit_width = 8,
};

vp_sensor_config_t sc233_linear_1920x1080_raw10_120fps_4lane = {
	.chip_id_reg = 0x3107,
	.chip_id = 0xcb61,
	.sensor_i2c_addr_list = {0x30, 0x32},
	.sensor_name = "sc233-120fps",
	.support_sensor_mode = {NORMAL_M},
	.config_file = "linear_1920x1080_raw10_120fps_4lane.c",
	.camera_config = &sc233_camera_config,
	.vin_ichn_attr = &sc233_vin_ichn_attr,
	.vin_node_attr = &sc233_vin_node_attr,
	.vin_attr_ex = &sc233_vin_attr_ex,
	.vin_ochn_attr = &sc233_vin_ochn_attr,
	.isp_attr = &sc233_isp_attr,
	.isp_ichn_attr = &sc233_isp_ichn_attr,
	.isp_ochn_attr = &sc233_isp_ochn_attr,
};
