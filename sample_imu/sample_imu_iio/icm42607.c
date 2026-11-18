/***************************************************************************
 *                      COPYRIGHT NOTICE
 *             Copyright(C) 2024-2025, D-Robotics Co., Ltd.
 *                     All rights reserved.
 ***************************************************************************/
/*
 * ICM-42607 backend (kernel inv_icm42607 driver, IIO name "icm43600").
 * Single IIO device with accel+gyro; the driver's *_scale attributes
 * report the full-scale range, so SI conversion is done here.
 */

#include "imu_interface.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SYSFS_PATH "/sys/bus/iio/devices/"
#define MAX_PATH_LEN 512
#define MAX_DEVICES 16
#define ICM42607_NAME "icm43600"

#define DEFAULT_FSR_G 4.0f    /* in_accel_scale reported by driver when unset */
#define DEFAULT_FSR_DPS 2000.0f
#define GRAVITY_MSS 9.80665f
#define DEG2RAD (3.14159265358979323846f / 180.0f)
#define LSB_FULL_SCALE 32768.0f
#define DEFAULT_ODR_HZ 50

typedef struct {
	int initialized;
	float fsr_g;            /* accelerometer full scale in g */
	float fsr_dps;          /* gyroscope full scale in dps */
	char dev_path[MAX_PATH_LEN];
} Icm42607Data;

static int read_int_attr(const char *dev_path, const char *attr, int *value)
{
	char path[MAX_PATH_LEN];
	FILE *fp;
	int v = 0;

	snprintf(path, sizeof(path), "%s/%s", dev_path, attr);
	fp = fopen(path, "r");
	if (!fp) {
		return -1;
	}
	if (fscanf(fp, "%d", &v) != 1) {
		fclose(fp);
		return -1;
	}
	fclose(fp);
	*value = v;
	return 0;
}

static int read_float_attr(const char *dev_path, const char *attr, float *value)
{
	char path[MAX_PATH_LEN];
	FILE *fp;
	float v = 0.0f;

	snprintf(path, sizeof(path), "%s/%s", dev_path, attr);
	fp = fopen(path, "r");
	if (!fp) {
		return -1;
	}
	if (fscanf(fp, "%f", &v) != 1) {
		fclose(fp);
		return -1;
	}
	fclose(fp);
	*value = v;
	return 0;
}

static int write_str_attr(const char *dev_path, const char *attr, const char *val)
{
	char path[MAX_PATH_LEN];
	FILE *fp;
	int ret;

	snprintf(path, sizeof(path), "%s/%s", dev_path, attr);
	fp = fopen(path, "w");
	if (!fp) {
		fprintf(stderr, "Failed to open for write: %s (%s)\n", path, strerror(errno));
		return -1;
	}
	ret = fputs(val, fp) == EOF ? -1 : 0;
	if (fclose(fp) != 0) {
		ret = -1;
	}
	return ret;
}

static int write_int_attr(const char *dev_path, const char *attr, int value)
{
	char buf[16];

	snprintf(buf, sizeof(buf), "%d\n", value);
	return write_str_attr(dev_path, attr, buf);
}

static int read_raw_axis(const char *dev_path, const char *type, const char *axis, int *value)
{
	char raw_path[MAX_PATH_LEN];
	FILE *fp;

	snprintf(raw_path, sizeof(raw_path), "%s/in_%s_%s_raw", dev_path, type, axis);
	fp = fopen(raw_path, "r");
	if (!fp) {
		fprintf(stderr, "Failed to open raw file: %s (%s)\n", raw_path, strerror(errno));
		return -1;
	}
	if (fscanf(fp, "%d", value) != 1) {
		fclose(fp);
		fprintf(stderr, "Failed to read value from: %s\n", raw_path);
		return -1;
	}
	fclose(fp);
	return 0;
}

static int device_has_channels(const char *dev_path, const char *type)
{
	const char *axes[] = {"x", "y", "z"};
	char test_path[MAX_PATH_LEN];
	int i;

	for (i = 0; i < 3; ++i) {
		snprintf(test_path, sizeof(test_path), "%s/in_%s_%s_raw", dev_path, type, axes[i]);
		if (access(test_path, R_OK) != 0) {
			return 0;
		}
	}
	return 1;
}

static int find_device_by_name(const char *name, char *found_path, size_t found_path_size)
{
	char devices[MAX_DEVICES][MAX_PATH_LEN];
	DIR *dir;
	struct dirent *ent;
	int count = 0;

	dir = opendir(SYSFS_PATH);
	if (!dir) {
		return -1;
	}
	while ((ent = readdir(dir)) != NULL && count < MAX_DEVICES) {
		if (!strstr(ent->d_name, "iio:device")) {
			continue;
		}
		snprintf(devices[count], MAX_PATH_LEN, "%s%s", SYSFS_PATH, ent->d_name);
		++count;
	}
	closedir(dir);

	for (int i = 0; i < count; ++i) {
		char name_path[MAX_PATH_LEN];
		char current_name[64];
		FILE *fp;

		snprintf(name_path, sizeof(name_path), "%s/name", devices[i]);
		fp = fopen(name_path, "r");
		if (!fp) {
			continue;
		}
		if (!fgets(current_name, sizeof(current_name), fp)) {
			fclose(fp);
			continue;
		}
		fclose(fp);
		current_name[strcspn(current_name, "\r\n")] = '\0';
		if (strcmp(current_name, name) == 0) {
			snprintf(found_path, found_path_size, "%s", devices[i]);
			return 0;
		}
	}
	return -1;
}

static uint64_t get_current_timestamp_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* Power on and enable both sensors if not already running. */
static int icm42607_ensure_running(const char *dev_path)
{
	int power_on = 0, acc_en = 0, gyr_en = 0, acc_rate = 0, gyr_rate = 0;

	if (read_int_attr(dev_path, "in_power_on", &power_on) == 0 && power_on == 0) {
		if (write_int_attr(dev_path, "in_power_on", 1) != 0) {
			return -1;
		}
	}
	if (read_int_attr(dev_path, "in_accel_enable", &acc_en) == 0 && acc_en == 0) {
		if (read_int_attr(dev_path, "in_accel_rate", &acc_rate) == 0 && acc_rate == 0) {
			if (write_int_attr(dev_path, "in_accel_rate", DEFAULT_ODR_HZ) != 0) {
				return -1;
			}
		}
		if (write_int_attr(dev_path, "in_accel_enable", 1) != 0) {
			return -1;
		}
	}
	if (read_int_attr(dev_path, "in_anglvel_enable", &gyr_en) == 0 && gyr_en == 0) {
		if (read_int_attr(dev_path, "in_anglvel_rate", &gyr_rate) == 0 && gyr_rate == 0) {
			if (write_int_attr(dev_path, "in_anglvel_rate", DEFAULT_ODR_HZ) != 0) {
				return -1;
			}
		}
		if (write_int_attr(dev_path, "in_anglvel_enable", 1) != 0) {
			return -1;
		}
	}
	return 0;
}

/*
 * Enable the buffer so the driver's FIFO parsing (poll func) runs
 * and keeps refreshing the samples served by in_*_raw.
 */
static int icm42607_buffer_setup(const char *dev_path)
{
	if (write_int_attr(dev_path, "buffer0/length", 64) != 0) {
		return -1;
	}
	if (write_int_attr(dev_path, "scan_elements/in_accel_en", 1) != 0) {
		return -1;
	}
	if (write_int_attr(dev_path, "buffer0/enable", 1) != 0) {
		return -1;
	}
	return 0;
}

static void icm42607_buffer_teardown(const char *dev_path)
{
	write_int_attr(dev_path, "buffer0/enable", 0);
	write_int_attr(dev_path, "scan_elements/in_accel_en", 0);
}

static SensorContext icm42607_init(const char *params, const char *dev_path)
{
	Icm42607Data *data;
	(void)params;

	data = (Icm42607Data *)malloc(sizeof(Icm42607Data));
	if (!data) {
		fprintf(stderr, "Failed to allocate memory for ICM42607 data\n");
		return NULL;
	}
	memset(data, 0, sizeof(Icm42607Data));

	if (!device_has_channels(dev_path, "accel") || !device_has_channels(dev_path, "anglvel")) {
		fprintf(stderr, "ICM42607 device must expose both accel and anglvel channels: %s\n",
			dev_path);
		free(data);
		return NULL;
	}
	strncpy(data->dev_path, dev_path, sizeof(data->dev_path) - 1);

	data->fsr_g = DEFAULT_FSR_G;
	read_float_attr(dev_path, "in_accel_scale", &data->fsr_g);
	data->fsr_dps = DEFAULT_FSR_DPS;
	read_float_attr(dev_path, "in_anglvel_scale", &data->fsr_dps);

	if (icm42607_ensure_running(dev_path) != 0) {
		fprintf(stderr, "Failed to power on/enable ICM42607 sensors\n");
		free(data);
		return NULL;
	}

	/* FIFO parsing only runs while buffer0 is enabled (poll func). */
	if (icm42607_buffer_setup(dev_path) != 0) {
		fprintf(stderr, "Failed to enable IIO buffer for ICM42607\n");
		free(data);
		return NULL;
	}

	/* wait for the first frame to populate the raw cache */
	for (int i = 0; i < 50; ++i) {
		int dummy;

		if (read_int_attr(dev_path, "in_accel_x_raw", &dummy) == 0) {
			break;
		}
		usleep(20000);
	}

	data->initialized = 1;
	printf("ICM42607 ready: %s (accel FSR %.0fg, gyro FSR %.0fdps)\n",
		dev_path, data->fsr_g, data->fsr_dps);
	return data;
}

static int icm42607_read(SensorContext context, ImuData *data)
{
	Icm42607Data *icm = (Icm42607Data *)context;
	int ax = 0, ay = 0, az = 0, gx = 0, gy = 0, gz = 0;

	if (!icm || !icm->initialized || !data) {
		fprintf(stderr, "ICM42607 not initialized\n");
		return -1;
	}

	if (read_raw_axis(icm->dev_path, "accel", "x", &ax) != 0 ||
		read_raw_axis(icm->dev_path, "accel", "y", &ay) != 0 ||
		read_raw_axis(icm->dev_path, "accel", "z", &az) != 0 ||
		read_raw_axis(icm->dev_path, "anglvel", "x", &gx) != 0 ||
		read_raw_axis(icm->dev_path, "anglvel", "y", &gy) != 0 ||
		read_raw_axis(icm->dev_path, "anglvel", "z", &gz) != 0) {
		fprintf(stderr, "Failed to read ICM42607 channels\n");
		return -1;
	}

	/* raw LSB -> SI against configured full scale */
	data->ax = ax * icm->fsr_g * GRAVITY_MSS / LSB_FULL_SCALE;
	data->ay = ay * icm->fsr_g * GRAVITY_MSS / LSB_FULL_SCALE;
	data->az = az * icm->fsr_g * GRAVITY_MSS / LSB_FULL_SCALE;
	data->gx = gx * icm->fsr_dps * DEG2RAD / LSB_FULL_SCALE;
	data->gy = gy * icm->fsr_dps * DEG2RAD / LSB_FULL_SCALE;
	data->gz = gz * icm->fsr_dps * DEG2RAD / LSB_FULL_SCALE;
	data->timestamp = get_current_timestamp_us();
	data->mx = 0.0f;
	data->my = 0.0f;
	data->mz = 0.0f;
	data->status = 0;
	return 0;
}

static void icm42607_release(SensorContext context)
{
	Icm42607Data *icm = (Icm42607Data *)context;

	if (!icm) {
		return;
	}
	/* restore quiet state */
	icm42607_buffer_teardown(icm->dev_path);
	write_int_attr(icm->dev_path, "in_accel_enable", 0);
	write_int_attr(icm->dev_path, "in_anglvel_enable", 0);
	write_int_attr(icm->dev_path, "in_power_on", 0);
	free(icm);
}

const SensorDriver icm42607_driver = {
	.name = ICM42607_NAME,
	.init = icm42607_init,
	.read = icm42607_read,
	.release = icm42607_release
};
