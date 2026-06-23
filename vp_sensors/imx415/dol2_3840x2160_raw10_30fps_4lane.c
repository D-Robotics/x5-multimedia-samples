#include "vp_sensors.h"
#include "imx415_common.h"

#define SENSOR_WIDTH  3840
#define SENSOR_HEIGHT  2160
#define SENSOR_FPS 30
#define RAW10 0x2B

static mipi_config_t imx415_mipi_config = {
	.rx_enable = 1,
	.rx_attr = {
		.phy = 0,
		.lane = 4,
		.datatype = RAW10,
		.fps = SENSOR_FPS,
		.mclk = 1,
		.mipiclk = 7128,
		.width = SENSOR_WIDTH,
		.height = SENSOR_HEIGHT,
		.linelenth = 6400,
		.framelenth = 4700,
		.settle = 10,
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

static camera_config_t imx415_camera_config = {
	.name = "imx415",
	.addr = 0x1a,
	.sensor_mode = DOL2_M,
	.fps = SENSOR_FPS,
	.format = RAW10,
	.width = SENSOR_WIDTH,
	.height = SENSOR_HEIGHT,
	.gpio_enable_bit = 0x01,
	.gpio_level_bit = 0x00,
	.mipi_cfg = &imx415_mipi_config,
	.calib_lname = "imx415_hdr_tuning.json",
	// .calib_lname = "disable",
};

static vin_node_attr_t imx415_vin_node_attr = {
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

static vin_attr_ex_t imx415_vin_attr_ex = {
	.vin_attr_ex_mask = 0x80,
	.mclk_ex_attr = {
		.mclk_freq = 24000000,
	},
};

static vin_ichn_attr_t imx415_vin_ichn_attr = {
	.width = SENSOR_WIDTH,
	.height = SENSOR_HEIGHT,
	.format = RAW10,
};

static vin_ochn_attr_t imx415_vin_ochn_attr = {
	.ddr_en = 0,
	.ochn_attr_type = VIN_BASIC_ATTR,
	.vin_basic_attr = {
		.format = RAW10,
		// 硬件 stride 跟格式匹配，通过行像素根据raw数据bit位数计算得来
		// 8bit：x1, 10bit: x2 12bit: x2 16bit: x2,例raw10，1920 x 2 = 3840
		.wstride = (SENSOR_WIDTH) * 2,
	},
};

static isp_attr_t imx415_isp_attr = {
	.input_mode = PASSTHROUGH_MODE ,// PASSTHROUGH_MODE : online, MCM_MODE: 用于调试，DDR_MODE: offline
	.sensor_mode= ISP_DOL2_M,
	.tile_mode = 0,
	.crop = {
		.x = 0,
		.y = 0,
		.h = SENSOR_HEIGHT,
		.w = SENSOR_WIDTH,
	},
};

static isp_ichn_attr_t imx415_isp_ichn_attr = {
	.width = SENSOR_WIDTH,
	.height = SENSOR_HEIGHT,
	.fmt = FRM_FMT_RAW,
	.bit_width = 10,
};

static isp_ochn_attr_t imx415_isp_ochn_attr = {
	.ddr_en = 1,
	.fmt = FRM_FMT_NV12,
	.bit_width = 8,
};

vp_sensor_config_t imx415_dol2_3840x2160_raw10_30fps_4lane = {
	.chip_id_reg = IMX415_SENSOR_INFO,
	.chip_id     = IMX415_CHIP_ID,
	.read_chip_id_cb = imx415_read_chip_id,
	.sensor_i2c_addr_list = {0x1A},
	.sensor_name = "imx415-30fps-4lane-dol2",
	.support_sensor_mode  = {DOL2_M},
	.config_file = "dol2_3840x2160_raw10_30fps_4lane.c",
	.camera_config = &imx415_camera_config,
	.vin_ichn_attr = &imx415_vin_ichn_attr,
	.vin_node_attr = &imx415_vin_node_attr,
	.vin_attr_ex   = &imx415_vin_attr_ex,
	.vin_ochn_attr = &imx415_vin_ochn_attr,
	.isp_attr      = &imx415_isp_attr,
	.isp_ichn_attr = &imx415_isp_ichn_attr,
	.isp_ochn_attr = &imx415_isp_ochn_attr,
};
