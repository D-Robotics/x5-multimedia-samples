#include "vp_sensors.h"

#define SENSOR_WIDTH  1920
#define SENSOR_HEIGHT  1080
#define SENSOE_FPS 30
#define RAW10 0x2B

static mipi_config_t sc230ai_mipi_config = {
	.rx_enable = 1,
	.rx_attr = {
		.phy = 0,
		.lane = 2,
		.datatype = RAW10,
		.fps = SENSOE_FPS,
		.mclk = 24,
		.mipiclk = 1000,
		.width = SENSOR_WIDTH,
		.height = SENSOR_HEIGHT,
		.linelenth = 2400,
		// .framelenth = 2250,
		.framelenth = 2350,
		.settle = 22,
		.channel_num = 2,
		.channel_sel = {0,1},
		.hsdTime = 0,
		.hsaTime = 0,
		.hbpTime = 0,
	},
	.rx_ex_mask = 0x40,
	.rx_attr_ex = {
		.stop_check_instart = 1,
	}
};

static camera_config_t sc230ai_camera_config = {
	.name = "sc230ai",
	.addr = 0x30,
	.sensor_mode = DOL2_M,
	.fps = SENSOE_FPS,
	.format = RAW10,
	.width = SENSOR_WIDTH,
	.height = SENSOR_HEIGHT,
	.gpio_enable_bit = 0x01,
	.gpio_level_bit = 0x00,
	.mipi_cfg = &sc230ai_mipi_config,
	.calib_lname = "sc230ai_hdr_tuning.json",
	.config_index = 0, //2lane
};

static vin_node_attr_t sc230ai_vin_node_attr = {
	.cim_attr = {
		.mipi_rx = 0,
		.vc_index = 0,
		.ipi_channel = 2,
		.cim_isp_flyby = 1,
		.func = {
			.enable_frame_id = 1,
			.set_init_frame_id = 0,
			.hdr_mode = DOL_2,
			.time_stamp_en = 0,
		},

	},
};

static vin_attr_ex_t sc230ai_vin_attr_ex = {
	.vin_attr_ex_mask = 0x80,
	.mclk_ex_attr = {
		.mclk_freq = 24000000,
	},
};

static vin_ichn_attr_t sc230ai_vin_ichn_attr = {
	.width = SENSOR_WIDTH,
	.height = SENSOR_HEIGHT,
	.format = RAW10,
};

static vin_ochn_attr_t sc230ai_vin_ochn_attr = {
	.ddr_en = 1,
	.ochn_attr_type = VIN_BASIC_ATTR,
	.vin_basic_attr = {
		.format = RAW10,
		// 硬件 stride 跟格式匹配，通过行像素根据raw数据bit位数计算得来
		// 8bit：x1, 10bit: x2 12bit: x2 16bit: x2,例raw10，1920 x 2 = 3840
		.wstride = (SENSOR_WIDTH) * 2,
	},
};

static isp_attr_t sc230ai_isp_attr = {
	.input_mode = 1, // 0: online, 1: mcm, 类似offline
	.sensor_mode = ISP_DOL2_M,
	.tile_mode = 0,
	.crop = {
		.x = 0,
		.y = 0,
		.h = SENSOR_HEIGHT,
		.w = SENSOR_WIDTH,
		// .h = 0,
		// .w = 0,
	},
};

static isp_ichn_attr_t sc230ai_isp_ichn_attr = {
	.width = SENSOR_WIDTH,
	.height = SENSOR_HEIGHT,
	.fmt = FRM_FMT_RAW,
	.bit_width = 10,
};

static isp_ochn_attr_t sc230ai_isp_ochn_attr = {
	.ddr_en = 1,
	.fmt = FRM_FMT_NV12,
	.bit_width = 8,
};

vp_sensor_config_t sc230ai_dol2_1920x1080_raw10_30fps_2lane = {
	.chip_id_reg = 0x3107,
	.chip_id = 0xcb34,
	.sensor_i2c_addr_list = {0x30, 0x32},
	.sensor_name = "sc230ai-dol2-30fps",
	.config_file = "dol2_1920x1080_raw10_30fps_2lane.c",
	.camera_config = &sc230ai_camera_config,
	.vin_ichn_attr = &sc230ai_vin_ichn_attr,
	.vin_node_attr = &sc230ai_vin_node_attr,
	.vin_attr_ex   = &sc230ai_vin_attr_ex,
	.vin_ochn_attr = &sc230ai_vin_ochn_attr,
	.isp_attr      = &sc230ai_isp_attr,
	.isp_ichn_attr = &sc230ai_isp_ichn_attr,
	.isp_ochn_attr = &sc230ai_isp_ochn_attr,
};
