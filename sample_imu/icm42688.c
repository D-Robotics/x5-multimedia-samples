/***************************************************************************
 *                      COPYRIGHT NOTICE
 *             Copyright(C) 2024-2025, D-Robotics Co., Ltd.
 *                     All rights reserved.
 ***************************************************************************/

#include "imu_interface.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SYSFS_PATH "/sys/bus/iio/devices/"
#define MAX_PATH_LEN 512
#define MAX_DEVICES 16
#define ICM42688_ACCEL_NAME "icm42688-accel"
#define ICM42688_GYRO_NAME "icm42688-gyro"

typedef struct {
	int ref_count;
	int initialized;
	float accel_scale;
	float gyro_scale;
	char accel_dev_path[MAX_PATH_LEN];
	char gyro_dev_path[MAX_PATH_LEN];
} Icm42688Data;

static Icm42688Data *g_shared_data = NULL;

static int read_raw_signed(const char *dev_path, const char *type, const char *axis, int *value)
{
	char raw_path[MAX_PATH_LEN];
	FILE *fp = NULL;

	snprintf(raw_path, sizeof(raw_path), "%s/in_%s_%s_raw", dev_path, type, axis);
	fp = fopen(raw_path, "r");
	if (!fp) {
		fprintf(stderr, "Failed to open raw file: %s (error: %s)\n", raw_path, strerror(errno));
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

static int read_sensor_scale(const char *dev_path, const char *type, float *scale)
{
	char scale_path[MAX_PATH_LEN];
	FILE *fp = NULL;

	snprintf(scale_path, sizeof(scale_path), "%s/in_%s_scale", dev_path, type);
	fp = fopen(scale_path, "r");
	if (!fp) {
		fprintf(stderr, "Failed to open scale file: %s (error: %s)\n",
			scale_path, strerror(errno));
		return -1;
	}

	if (fscanf(fp, "%f", scale) != 1) {
		fclose(fp);
		fprintf(stderr, "Failed to read scale from: %s\n", scale_path);
		return -1;
	}

	fclose(fp);
	return 0;
}

static uint64_t get_current_timestamp_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static int read_device_name(const char *dev_path, char *name, size_t name_size)
{
	char name_path[MAX_PATH_LEN];
	FILE *fp = NULL;

	snprintf(name_path, sizeof(name_path), "%s/name", dev_path);
	fp = fopen(name_path, "r");
	if (!fp) {
		return -1;
	}

	if (!fgets(name, (int)name_size, fp)) {
		fclose(fp);
		return -1;
	}

	fclose(fp);
	name[strcspn(name, "\r\n")] = '\0';
	return 0;
}

static int find_all_iio_devices(char devices[][MAX_PATH_LEN], int max_devices)
{
	DIR *dir = NULL;
	struct dirent *ent = NULL;
	int count = 0;

	dir = opendir(SYSFS_PATH);
	if (!dir) {
		fprintf(stderr, "Failed to open %s: %s\n", SYSFS_PATH, strerror(errno));
		return -1;
	}

	while ((ent = readdir(dir)) != NULL && count < max_devices) {
		if (!strstr(ent->d_name, "iio:device")) {
			continue;
		}

		snprintf(devices[count], MAX_PATH_LEN, "%s%s", SYSFS_PATH, ent->d_name);
		++count;
	}

	closedir(dir);
	return count;
}

static int find_device_by_name(const char *name, char *found_path, size_t found_path_size)
{
	char devices[MAX_DEVICES][MAX_PATH_LEN];
	int count = find_all_iio_devices(devices, MAX_DEVICES);

	if (count <= 0) {
		return -1;
	}

	for (int i = 0; i < count; ++i) {
		char current_name[64];

		if (read_device_name(devices[i], current_name, sizeof(current_name)) != 0) {
			continue;
		}

		if (strcmp(current_name, name) != 0) {
			continue;
		}

		strncpy(found_path, devices[i], found_path_size - 1);
		found_path[found_path_size - 1] = '\0';
		return 0;
	}

	return -1;
}

static int device_has_channels(const char *dev_path, const char *type)
{
	char test_path[MAX_PATH_LEN];
	const char *axes[] = {"x", "y", "z"};

	for (int i = 0; i < 3; ++i) {
		snprintf(test_path, sizeof(test_path), "%s/in_%s_%s_raw", dev_path, type, axes[i]);
		if (access(test_path, R_OK) != 0) {
			return 0;
		}
	}

	return 1;
}

static int assign_device_paths(Icm42688Data *data, const char *dev_path)
{
	char peer_path[MAX_PATH_LEN];
	int has_accel = 0;
	int has_gyro = 0;

	has_accel = device_has_channels(dev_path, "accel");
	has_gyro = device_has_channels(dev_path, "anglvel");

	if (has_accel && has_gyro) {
		strncpy(data->accel_dev_path, dev_path, sizeof(data->accel_dev_path) - 1);
		strncpy(data->gyro_dev_path, dev_path, sizeof(data->gyro_dev_path) - 1);
		return 0;
	}

	if (has_accel) {
		if (find_device_by_name(ICM42688_GYRO_NAME, peer_path, sizeof(peer_path)) != 0) {
			fprintf(stderr, "Failed to find gyroscope device for ICM42688\n");
			return -1;
		}
		if (!device_has_channels(peer_path, "anglvel")) {
			fprintf(stderr, "Gyroscope device is missing required channels\n");
			return -1;
		}

		strncpy(data->accel_dev_path, dev_path, sizeof(data->accel_dev_path) - 1);
		strncpy(data->gyro_dev_path, peer_path, sizeof(data->gyro_dev_path) - 1);
		return 0;
	}

	if (has_gyro) {
		if (find_device_by_name(ICM42688_ACCEL_NAME, peer_path, sizeof(peer_path)) != 0) {
			fprintf(stderr, "Failed to find accelerometer device for ICM42688\n");
			return -1;
		}
		if (!device_has_channels(peer_path, "accel")) {
			fprintf(stderr, "Accelerometer device is missing required channels\n");
			return -1;
		}

		strncpy(data->gyro_dev_path, dev_path, sizeof(data->gyro_dev_path) - 1);
		strncpy(data->accel_dev_path, peer_path, sizeof(data->accel_dev_path) - 1);
		return 0;
	}

	fprintf(stderr, "Device has neither accelerometer nor gyroscope channels\n");
	return -1;
}

static SensorContext icm42688_common_init(const char *params, const char *dev_path)
{
	Icm42688Data *data = NULL;

	(void)params;

	if (g_shared_data) {
		++g_shared_data->ref_count;
		return g_shared_data;
	}

	data = (Icm42688Data *)malloc(sizeof(Icm42688Data));
	if (!data) {
		fprintf(stderr, "Failed to allocate memory for ICM42688 data\n");
		return NULL;
	}

	memset(data, 0, sizeof(Icm42688Data));
	data->ref_count = 1;

	if (assign_device_paths(data, dev_path) != 0) {
		free(data);
		return NULL;
	}

	if (read_sensor_scale(data->accel_dev_path, "accel", &data->accel_scale) != 0) {
		data->accel_scale = 0.004788403f;
		fprintf(stderr, "Using default accel scale: %f\n", data->accel_scale);
	}

	if (read_sensor_scale(data->gyro_dev_path, "anglvel", &data->gyro_scale) != 0) {
		data->gyro_scale = 0.001065f;
		fprintf(stderr, "Using default gyro scale: %f\n", data->gyro_scale);
	}

	data->initialized = 1;
	g_shared_data = data;
	return data;
}

static int icm42688_common_read(SensorContext context, ImuData *data)
{
	Icm42688Data *icm_data = (Icm42688Data *)context;
	int accel_x_raw = 0;
	int accel_y_raw = 0;
	int accel_z_raw = 0;
	int gyro_x_raw = 0;
	int gyro_y_raw = 0;
	int gyro_z_raw = 0;

	if (!icm_data || !icm_data->initialized || !data) {
		fprintf(stderr, "ICM42688 not initialized\n");
		return -1;
	}

	if (read_raw_signed(icm_data->accel_dev_path, "accel", "x", &accel_x_raw) != 0 ||
		read_raw_signed(icm_data->accel_dev_path, "accel", "y", &accel_y_raw) != 0 ||
		read_raw_signed(icm_data->accel_dev_path, "accel", "z", &accel_z_raw) != 0) {
		fprintf(stderr, "Failed to read accelerometer data\n");
		return -1;
	}

	if (read_raw_signed(icm_data->gyro_dev_path, "anglvel", "x", &gyro_x_raw) != 0 ||
		read_raw_signed(icm_data->gyro_dev_path, "anglvel", "y", &gyro_y_raw) != 0 ||
		read_raw_signed(icm_data->gyro_dev_path, "anglvel", "z", &gyro_z_raw) != 0) {
		fprintf(stderr, "Failed to read gyroscope data\n");
		return -1;
	}

	data->ax = accel_x_raw * icm_data->accel_scale;
	data->ay = accel_y_raw * icm_data->accel_scale;
	data->az = accel_z_raw * icm_data->accel_scale;
	data->gx = gyro_x_raw * icm_data->gyro_scale;
	data->gy = gyro_y_raw * icm_data->gyro_scale;
	data->gz = gyro_z_raw * icm_data->gyro_scale;
	data->timestamp = get_current_timestamp_us();
	data->mx = 0.0f;
	data->my = 0.0f;
	data->mz = 0.0f;
	data->status = 0;
	return 0;
}

static void icm42688_common_release(SensorContext context)
{
	Icm42688Data *icm_data = (Icm42688Data *)context;

	if (!icm_data) {
		return;
	}

	--icm_data->ref_count;
	if (icm_data->ref_count > 0) {
		return;
	}

	free(icm_data);
	g_shared_data = NULL;
}

const SensorDriver icm42688_driver_gyro = {
	.name = ICM42688_GYRO_NAME,
	.init = icm42688_common_init,
	.read = icm42688_common_read,
	.release = icm42688_common_release
};

const SensorDriver icm42688_driver_accel = {
	.name = ICM42688_ACCEL_NAME,
	.init = icm42688_common_init,
	.read = icm42688_common_read,
	.release = icm42688_common_release
};