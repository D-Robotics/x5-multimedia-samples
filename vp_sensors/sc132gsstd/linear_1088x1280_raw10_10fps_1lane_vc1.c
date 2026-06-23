#include "vp_sensors.h"

#define SENSOR_WIDTH  1088
#define SENSOR_HEIGHT  1280
#define SENSOR_FPS 10
#define RAW10 0x2B
#define MICROSECONDS_PER_SECOND 1000000

static mipi_config_t mipi_config = {
	.rx_enable = 1,
	.rx_attr = {
		.phy = 0,
		.lane = 4,  // Desrializer 4lane
		.datatype = RAW10,
		.fps = SENSOR_FPS,
		.mclk = 1,
		.mipiclk = 4000,  // 4lane  // 1 lane 1200
		.width = SENSOR_WIDTH,
		.height = SENSOR_HEIGHT,
		.linelenth = 1400,
		.framelenth = 1500,
		.settle = 0,
		.channel_num = 4,  // use 4 virtual channels
		.channel_sel = {0, 1, 2, 3},  // 4 VC
	},
	.rx_ex_mask = 0x40,
	.rx_attr_ex = {
		.stop_check_instart = 1,
	}
	// Don't check high speed
	// .rx_ex_mask = 0x41,
	// .rx_attr_ex = {
	// 	.stop_check_instart = 1,
	// 	.nocheck = 1,
	// }
};

static camera_config_t camera_config = {
	.name = "sc132gsstd",
	.addr = 0x30,
	.sensor_mode = SLAVE_M,
	// .sensor_mode = NORMAL_M,
	.fps = SENSOR_FPS,
	.format = RAW10,
	.width = SENSOR_WIDTH,
	.height = SENSOR_HEIGHT,
	.mipi_cfg = &mipi_config,
	.gpio_enable_bit = 0x01,
	.gpio_level_bit = 0x00,
	.calib_lname = "disable",
	.extra_mode = 1,  // use this attribute to transfer virtual channel index
};

static vin_node_attr_t vin_node_attr = {
	.cim_attr = {
		.mipi_rx = 0,
		.vc_index = 1,  // Virtual channel index1, bind to serial tx1
		.ipi_channel = 1,
		.cim_isp_flyby = 0,  // 1 mcm\online, 2: offline
		.func = {
			.enable_frame_id = 1,
			.set_init_frame_id = 0,
			.hdr_mode = NOT_HDR,
			.time_stamp_en = 0,
		},
	},
	.lpwm_attr = {
		.enable = 1,
		.lpwm_chn_attr = {
			{	.trigger_source = 0,
				.trigger_mode = 0,
				.period = MICROSECONDS_PER_SECOND / SENSOR_FPS,  // 30fps: 333333;  10fps: 100000
				.offset = 10,
				.duty_time = 100,
				.threshold = 0,
				.adjust_step = 0,
			},
			{	.trigger_source = 0,
				.trigger_mode = 0,
				.period = MICROSECONDS_PER_SECOND / SENSOR_FPS,  // 30fps: 333333;  10fps: 100000
				.offset = 10,
				.duty_time = 100,
				.threshold = 0,
				.adjust_step = 0,
			},
			{	.trigger_source = 0,
				.trigger_mode = 0,
				.period = MICROSECONDS_PER_SECOND / SENSOR_FPS,  // 30fps: 333333;  10fps: 100000
				.offset = 10,
				.duty_time = 100,
				.threshold = 0,
				.adjust_step = 0,
			},
			{	.trigger_source = 0,
				.trigger_mode = 0,
				.period = MICROSECONDS_PER_SECOND / SENSOR_FPS,  // 30fps: 333333;  10fps: 100000
				.offset = 10,
				.duty_time = 100,
				.threshold = 0,
				.adjust_step = 0,
			},
		},
	},
};

static vin_attr_ex_t vin_attr_ex = {
	.vin_attr_ex_mask = 0x80,
	.mclk_ex_attr = {
		.mclk_freq = 24000000,
	},
};

static vin_ichn_attr_t vin_ichn_attr = {
	.width = SENSOR_WIDTH,
	.height = SENSOR_HEIGHT,
	.format = RAW10,
};

static vin_ochn_attr_t vin_ochn_attr = {
	.ddr_en = 1,
	.ochn_attr_type = VIN_BASIC_ATTR,
	.vin_basic_attr = {
		.format = RAW10,
		// 硬件 stride 跟格式匹配，通过行像素根据raw数据bit位数计算得来
		// 8bit：x1, 10bit: x2 12bit: x2 16bit: x2,例raw10，1920 x 2 = 3840
		.wstride = (SENSOR_WIDTH) * 2,
	},
};

static isp_attr_t isp_attr = {
	.input_mode = 2, // 0: online, 1: mcm, 类似offline, 2: offline
	.sensor_mode= ISP_NORMAL_M,
	.crop = {
		.x = 0,
		.y = 0,
		.h = SENSOR_HEIGHT,
		.w = SENSOR_WIDTH,
	},
};

static isp_ichn_attr_t isp_ichn_attr = {
	.width = SENSOR_WIDTH,
	.height = SENSOR_HEIGHT,
	.fmt = FRM_FMT_RAW,
	.bit_width = 10,
};

static isp_ochn_attr_t isp_ochn_attr = {
	.ddr_en = 1,
	.fmt = FRM_FMT_NV12,
	.bit_width = 8,
};

vp_sensor_config_t sc132gsstd_linear_1088x1280_raw10_10fps_1lane_vc1 = {
	.chip_id_reg = 0x3107,
	.chip_id = 0x0132,
	.sensor_i2c_addr_list = {0x30, 0x33},
	.sensor_name = "sc132gsstd_vc1",
	.sensor_type = SENSOR_TYPE_HSMT_RAW,
	.config_file = "linear_1088x1280_raw10_10fps_1lane_vc1.c",
	.camera_config = &camera_config,
	.vin_ichn_attr = &vin_ichn_attr,
	.vin_node_attr = &vin_node_attr,
	.vin_attr_ex   = &vin_attr_ex,
	.vin_ochn_attr = &vin_ochn_attr,
	.isp_attr      = &isp_attr,
	.isp_ichn_attr = &isp_ichn_attr,
	.isp_ochn_attr = &isp_ochn_attr,
};
