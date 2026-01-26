#ifndef __IMX415_COMMON_H__
#define __IMX415_COMMON_H__

#define IMX415_MODE 			0x3000
#define IMX415_MODE_OPERATING 		0
#define IMX415_MODE_STANDBY 		1 // BIT(0)
#define IMX415_SENSOR_INFO 		0x3f12
#define IMX415_SENSOR_INFO_MASK 	0xfff
#define IMX415_CHIP_ID 			0x514

extern int32_t imx415_read_chip_id(vcon_propertie_t vcon_props,
				   void *sensor_config_vo,
				   uint32_t addr,
				   int32_t *chip_id);

#endif