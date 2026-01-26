#include <stdio.h>
#include <fcntl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>

#include "vp_sensors.h"
#include "imx415_common.h"

int32_t vp_i2c_write_reg16_data8(uint32_t bus, uint8_t i2c_addr, uint16_t reg_addr, uint8_t value)
{
	int32_t ret;
	struct i2c_rdwr_ioctl_data data;
	uint8_t sendbuf[3] = {0};
	struct i2c_msg msgs[I2C_RDRW_IOCTL_MAX_MSGS] = {0};
	char filename[20];
	int file;

	snprintf(filename, sizeof(filename), "/dev/i2c-%d", bus);
	file = open(filename, O_RDWR);
	if (file < 0) {
		printf("WARN: Failed to open the I2C bus");
		return -1;
	}

	sendbuf[0] = (uint8_t)((reg_addr >> 8u) & 0xffu);
	sendbuf[1] = (uint8_t)(reg_addr & 0xffu);
	sendbuf[2] = value;

	data.msgs = msgs;
	data.nmsgs = 1;

	data.msgs[0].len = 3;
	data.msgs[0].addr = i2c_addr;
	data.msgs[0].flags = 0;
	data.msgs[0].buf = sendbuf;

	ret = ioctl(file, I2C_RDWR, (uint64_t)&data);
	if (ret < 0) {
		printf("WARN: Failed to write to the I2C bus");
		close(file);
		return -1;
	}

	close(file);

	return 0;
}

int32_t vp_i2c_read_reg16_data16(uint32_t bus, uint8_t i2c_addr, uint16_t reg_addr, uint16_t *value)
{
	int32_t ret;
	struct i2c_rdwr_ioctl_data data;
	uint8_t sendbuf[2] = {0};
	uint8_t readbuf[2] = {0};
	struct i2c_msg msgs[I2C_RDRW_IOCTL_MAX_MSGS] = {0};
	char filename[20];
	int file;

	// Open the I2C bus
	snprintf(filename, sizeof(filename), "/dev/i2c-%d", bus);
	file = open(filename, O_RDWR);
	if (file < 0) {
		printf("WARN: Failed to open the I2C bus\n");
		return -1;
	}

	sendbuf[0] = (uint8_t)((reg_addr >> 8u) & 0xffu);
	sendbuf[1] = (uint8_t)(reg_addr & 0xffu);

	data.msgs = msgs; /*PRQA S 5118*/
	data.nmsgs = 2;

	data.msgs[0].len = 2;
	data.msgs[0].addr = i2c_addr;
	data.msgs[0].flags = 0;
	data.msgs[0].buf = sendbuf;

	data.msgs[1].len = 2;
	data.msgs[1].addr = i2c_addr;
	data.msgs[1].flags = I2C_M_RD;
	data.msgs[1].buf = readbuf;

	ret = ioctl(file, I2C_RDWR, (uint64_t)&data);
	if (ret < 0) {
		printf("WARN: Failed to read from the I2C bus\n");
		*value = 0;
		close(file);
		return -1;
	}
	*value = (uint16_t)((readbuf[0] << 8) | readbuf[1]);

	close(file);

	return 0;
}

int32_t imx415_read_chip_id(vcon_propertie_t vcon_props,
			    void *sensor_config_vo,
			    uint32_t addr,
			    int32_t *chip_id)
{
	vp_sensor_config_t *sensor_config = (vp_sensor_config_t *)sensor_config_vo;

	// imx415 need wakeup at first, IMX415_MODE_OPERATING:0x0
	if (vp_i2c_write_reg16_data8(vcon_props.bus, addr, IMX415_MODE, IMX415_MODE_OPERATING) != 0) {
		printf("WARN: Sensor Name: %s, Addr: 0x%02x wakeup failed\n", sensor_config->sensor_name, addr);
	}
	/*
	 * According to the datasheet we have to wait at least 63 us after
	 * leaving standby mode. But this doesn't work even after 30 ms.
	 * So probably this should be 63 ms and therefore we wait for 80 ms.
	 */
	usleep(80 * 1000);

	vp_i2c_read_reg16_data16(vcon_props.bus, addr, sensor_config->chip_id_reg, (uint16_t*)chip_id);

	// IMX415_MODE_STANDBY:0x1
	if (vp_i2c_write_reg16_data8(vcon_props.bus, addr, IMX415_MODE, IMX415_MODE_STANDBY) != 0) {
		printf("WARN: Sensor Name: %s, Addr: 0x%02x standby failed\n", sensor_config->sensor_name, addr);
	}

	*chip_id = ((chip_id[0] >> 8 & 0xff) | (chip_id[0] << 8 & 0xff00)) & IMX415_SENSOR_INFO_MASK;

	printf("WARN: Sensor Name: %s, sensor_config->chip_id:0x%04x read chip_id:0x%04x\n",
		sensor_config->sensor_name, sensor_config->chip_id, *chip_id);

	if ((*chip_id & 0xFFFF) == (sensor_config->chip_id & 0xFFFF)) {
		return 0;  // 匹配成功
	} else {
		return -1; // 匹配失败
	}
}
