/***************************************************************************
 *                      COPYRIGHT NOTICE
 *             Copyright(C) 2024-2025, D-Robotics Co., Ltd.
 *                     All rights reserved.
 ***************************************************************************/

#include "imu_manager.h"
#include "imu_interface.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SYSFS_PATH "/sys/bus/iio/devices/"
#define MAX_PATH_LEN 512

extern const struct SensorDriver bmi08x_driver;
extern const struct SensorDriver icm42688_driver_gyro;
extern const struct SensorDriver icm42688_driver_accel;

static const struct SensorDriver *drivers[] = {
	&bmi08x_driver,
	&icm42688_driver_gyro,
	&icm42688_driver_accel,
	NULL
};

static void trim_newline(char *text)
{
	if (!text) {
		return;
	}

	text[strcspn(text, "\r\n")] = '\0';
}

static int read_device_name(const char *dev_path, char *device_name, size_t device_name_size)
{
	char name_path[MAX_PATH_LEN];
	FILE *name_fp = NULL;

	if (!dev_path || !device_name || device_name_size == 0) {
		return -1;
	}

	snprintf(name_path, sizeof(name_path), "%s/name", dev_path);
	name_fp = fopen(name_path, "r");
	if (!name_fp) {
		return -1;
	}

	if (!fgets(device_name, (int)device_name_size, name_fp)) {
		fclose(name_fp);
		return -1;
	}

	fclose(name_fp);
	trim_newline(device_name);
	return 0;
}

// Print detected IIO devices for quick inspection
static void list_all_iio_devices(void)
{
	DIR *dp = NULL;
	struct dirent *ent = NULL;

	printf("\n=== Detected IIO Devices ===\n");

	dp = opendir(SYSFS_PATH);
	if (!dp) {
		fprintf(stderr, "Can't open %s: %s\n", SYSFS_PATH, strerror(errno));
		return;
	}

	while ((ent = readdir(dp)) != NULL) {
		if (!strstr(ent->d_name, "iio:device")) {
			continue;
		}

		char device_path[MAX_PATH_LEN];
		char device_name[64] = "unknown";

		snprintf(device_path, sizeof(device_path), SYSFS_PATH "%s", ent->d_name);
		if (read_device_name(device_path, device_name, sizeof(device_name)) != 0) {
			strcpy(device_name, "unknown");
		}

		printf("  Device: %-15s | Name: %s\n", ent->d_name, device_name);
	}

	closedir(dp);
	printf("============================\n\n");
}

// Check whether a sysfs node exists and is readable
static int check_file_access(const char *path, int verbose)
{
	if (access(path, F_OK) != 0) {
		if (verbose) {
			fprintf(stderr, "File not found: %s\n", path);
		}
		return -1;
	}

	if (access(path, R_OK) != 0) {
		if (verbose) {
			fprintf(stderr, "No read permission: %s (try with sudo)\n", path);
		}
		return -1;
	}

	return 0;
}

// Validate name and at least one IMU channel group
static int validate_iio_device(const char *dev_path, const char *expected_name, int verbose)
{
	char test_path[MAX_PATH_LEN];
	char actual_name[64];
	struct stat st;
	const char *axes[] = {"x", "y", "z"};
	int accel_found = 0;
	int gyro_found = 0;

	if (stat(dev_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
		if (verbose) {
			fprintf(stderr, "Invalid IIO device path: %s\n", dev_path);
		}
		return -1;
	}

	if (read_device_name(dev_path, actual_name, sizeof(actual_name)) != 0) {
		if (verbose) {
			fprintf(stderr, "Failed to read device name from: %s\n", dev_path);
		}
		return -1;
	}

	if (strcmp(actual_name, expected_name) != 0) {
		if (verbose) {
			fprintf(stderr, "Unexpected device name: %s\nyour chose imu: %s\n",
				actual_name, expected_name);
		}
		return -1;
	}

	for (int i = 0; i < 3; ++i) {
		snprintf(test_path, sizeof(test_path), "%s/in_accel_%s_raw", dev_path, axes[i]);
		if (check_file_access(test_path, verbose) == 0) {
			++accel_found;
		}
	}

	for (int i = 0; i < 3; ++i) {
		snprintf(test_path, sizeof(test_path), "%s/in_anglvel_%s_raw", dev_path, axes[i]);
		if (check_file_access(test_path, verbose) == 0) {
			++gyro_found;
		}
	}

	if (accel_found == 0 && gyro_found == 0) {
		if (verbose) {
			fprintf(stderr, "No valid IMU channels found at: %s\n", dev_path);
		}
		return -1;
	}

	if (verbose) {
		printf("Device validation passed at: %s\n", dev_path);
	}
	return 0;
}

// Find the first matching IIO device for the requested sensor
static int find_valid_iio_device(const char *sensor_type, char *dev_path, size_t dev_path_size,
	int verbose)
{
	DIR *dp = NULL;
	struct dirent *ent = NULL;

	dp = opendir(SYSFS_PATH);
	if (!dp) {
		if (verbose) {
			fprintf(stderr, "Can't open %s: %s\n", SYSFS_PATH, strerror(errno));
		}
		return -1;
	}

	while ((ent = readdir(dp)) != NULL) {
		char current_path[MAX_PATH_LEN];

		if (!strstr(ent->d_name, "iio:device")) {
			continue;
		}

		snprintf(current_path, sizeof(current_path), SYSFS_PATH "%s", ent->d_name);
		if (validate_iio_device(current_path, sensor_type, verbose) != 0) {
			continue;
		}

		strncpy(dev_path, current_path, dev_path_size - 1);
		dev_path[dev_path_size - 1] = '\0';
		closedir(dp);
		return 0;
	}

	closedir(dp);
	if (verbose) {
		fprintf(stderr, "\nNo valid IIO device name found for '%s'.\n", sensor_type);
	}
	return -1;
}

// Print all sensor drivers supported by this sample
void list_available_sensors(void)
{
	printf("Supported sensors: ");
	for (int i = 0; drivers[i] != NULL; ++i) {
		printf("%s ", drivers[i]->name);
	}
	printf("\n");
}

// Create a runtime sensor instance from a driver and device path
SensorHandle init_sensor(const char *sensor_type, const char *params)
{
	char dev_path[MAX_PATH_LEN];

	if (!sensor_type) {
		return NULL;
	}

	list_all_iio_devices();

	if (find_valid_iio_device(sensor_type, dev_path, sizeof(dev_path), 1) != 0) {
		return NULL;
	}

	for (int i = 0; drivers[i] != NULL; ++i) {
		SensorInstance *instance = NULL;

		if (strcmp(drivers[i]->name, sensor_type) != 0) {
			continue;
		}

		instance = (SensorInstance *)malloc(sizeof(SensorInstance));
		if (!instance) {
			fprintf(stderr, "Error: Failed to allocate sensor instance\n");
			return NULL;
		}

		instance->driver = drivers[i];
		instance->context = drivers[i]->init(params, dev_path);
		if (!instance->context) {
			free(instance);
			return NULL;
		}

		return (SensorHandle)instance;
	}

	fprintf(stderr, "Error: Unknown sensor type '%s'\n", sensor_type);
	return NULL;
}

// Read one IMU frame through the bound driver
int read_sensor_data(SensorHandle handle, ImuData *data)
{
	SensorInstance *instance = (SensorInstance *)handle;

	if (!instance || !instance->driver || !instance->driver->read || !data) {
		return -1;
	}

	return instance->driver->read(instance->context, data);
}

// Release the bound sensor instance
void release_sensor(SensorHandle handle)
{
	SensorInstance *instance = (SensorInstance *)handle;

	if (!instance) {
		return;
	}

	if (instance->driver && instance->driver->release) {
		instance->driver->release(instance->context);
	}

	free(instance);
}

// Format a microsecond timestamp as hh:mm:ss.mmm.uuu
static void print_formatted_timestamp(uint64_t microseconds)
{
	uint64_t hours = microseconds / 3600000000ULL;
	uint64_t minutes = (microseconds % 3600000000ULL) / 60000000ULL;
	uint64_t seconds = (microseconds % 60000000ULL) / 1000000ULL;
	uint64_t milliseconds = (microseconds % 1000000ULL) / 1000ULL;
	uint64_t microsec_part = microseconds % 1000ULL;

	printf("%02" PRIu64 ":%02" PRIu64 ":%02" PRIu64 ".%03" PRIu64 ".%03" PRIu64,
		hours, minutes, seconds, milliseconds, microsec_part);
}

// Print a compact IMU data summary
void print_imu_data(const ImuData *data)
{
	printf("  Accelerometer: [%f, %f, %f] m/s²\n", data->ax, data->ay, data->az);
	printf("  Gyroscope:     [%f, %f, %f] rad/s\n", data->gx, data->gy, data->gz);
	printf("  Timestamp:     ");
	print_formatted_timestamp(data->timestamp);
	printf("\n");
}

// Return the first supported sensor detected on the target
const char *get_default_imu_name(void)
{
	char dev_path[MAX_PATH_LEN];

	for (int i = 0; drivers[i] != NULL; ++i) {
		if (find_valid_iio_device(drivers[i]->name, dev_path, sizeof(dev_path), 0) == 0) {
			return drivers[i]->name;
		}
	}

	return NULL;
}