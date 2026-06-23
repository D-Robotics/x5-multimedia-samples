/***************************************************************************
 *                      COPYRIGHT NOTICE
 *             Copyright(C) 2024-2025, D-Robotics Co., Ltd.
 *                     All rights reserved.
 ***************************************************************************/

#include "imu_interface.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdint.h>
#include <math.h>

#define SYSFS_PATH "/sys/bus/iio/devices/"
#define MAX_PATH_LEN 512

// BMI08x private context
typedef struct {
	int initialized;
	const char* params;
	float accel_scale;
	float gyro_scale;
	char dev_path[MAX_PATH_LEN];  // Device sysfs path
} BMI08xData;


/* Read raw value with signed conversion */
static int read_raw_signed(const char *dev_path, const char *type,
	const char *axis, int *val) {
	char raw_path[MAX_PATH_LEN];
	FILE *fp;
	unsigned raw_value;

	snprintf(raw_path, sizeof(raw_path), "%s/in_%s_%s_raw", dev_path, type, axis);

	fp = fopen(raw_path, "r");
	if (!fp) {
		perror("Failed to open raw file");
		return -1;
	}

	if (fscanf(fp, "%u", &raw_value) != 1) { // Raw value is exposed as unsigned text
		fclose(fp);
		return -1;
	}
	fclose(fp);

	/* Apply 16-bit two's complement conversion */
	*val = (int)(raw_value > 32767 ? raw_value - 65536 : raw_value);
	return 0;
}

/* Read sensor timestamp in microseconds */
static int read_sensor_time(const char *dev_path, uint64_t *timestamp) {
	char time_path[MAX_PATH_LEN];
	FILE *fp;
	uint32_t sensor_time;

	// Build the sensor_time node path
	snprintf(time_path, sizeof(time_path), "%s/sensor_time", dev_path);

	// Open the node
	fp = fopen(time_path, "r");
	if (!fp) {
		perror("Failed to open sensor_time file");
		return -1;
	}

	// Read the timestamp value, for example: sensor_time:8835002
	if (fscanf(fp, "sensor_time:%u", &sensor_time) != 1) {
		fclose(fp);
		return -1;
	}
	fclose(fp);

	// Convert sensor ticks to microseconds (1 tick = 39.0625 us)
	*timestamp = (uint64_t)sensor_time * 390625 / 10000;
	return 0;
}

/* Read current ranges and derive scale factors */
static int read_sensor_ranges(const char *dev_path, float *accel_scale, float *gyro_scale) {
	char path[MAX_PATH_LEN];
	FILE *fp;
	char line[256];
	int acc_range = -1, gyro_range = -1;

	// 1. Read accelerometer config
	snprintf(path, sizeof(path), "%s/acc_config", dev_path);
	fp = fopen(path, "r");
	if (fp) {
		if (fgets(line, sizeof(line), fp) != NULL) {
			// Parse format: "acc_conf odr:8 bw:10 range:1"
			char *range_ptr = strstr(line, "range:");
			if (range_ptr) {
				if (sscanf(range_ptr, "range:%d", &acc_range) != 1) {
					fprintf(stderr, "Failed to parse acc_range value\n");
					acc_range = -1;
			}
		}
	}
	fclose(fp);
	} else {
		perror("Failed to open acc_config file");
	}

	// 2. Read gyroscope config
	snprintf(path, sizeof(path), "%s/gyr_config", dev_path);
	fp = fopen(path, "r");
	if (fp) {
		if (fgets(line, sizeof(line), fp) != NULL) {
			// Parse format: "gyro_conf odr:8 bw:10 range:0"
			char *range_ptr = strstr(line, "range:");
			if (range_ptr) {
				if (sscanf(range_ptr, "range:%d", &gyro_range) != 1) {
					fprintf(stderr, "Failed to parse gyro_range value\n");
					gyro_range = -1;
			}
		}
	}
	fclose(fp);
	} else {
		perror("Failed to open gyro_config file");
	}

	// 3. Set accelerometer scale factor
	if (acc_range >= 0 && acc_range <= 3) {
		const float ranges[] = {3.0f, 6.0f, 12.0f, 24.0f};  // Mapping for range:0-3
		*accel_scale = (2 * ranges[acc_range]) / 65536.0f;
		printf("Accelerometer range: ±%dg, scale: %f g/LSB\n",(int)ranges[acc_range], *accel_scale);
	} else {
		*accel_scale = (2 * 6.0f) / 65536.0f;  // Default to +/-6g (range:1)
		fprintf(stderr, "Using default acc_range: ±6g\n");
	}

	// 4. Set gyroscope scale factor
	if (gyro_range >= 0 && gyro_range <= 4) {
		// Resolution table (LSB/deg/s) for range:0-4
		const float resolutions[] = {
			16.384f,    // +/-2000 deg/s (range:0)
			32.768f,    // +/-1000 deg/s (range:1)
			65.536f,    // +/-500 deg/s (range:2)
			131.072f,   // +/-250 deg/s (range:3)
			262.144f    // +/-125 deg/s (range:4)
		};
		const float ranges[] = {2000.0f, 1000.0f, 500.0f, 250.0f, 125.0f};

		*gyro_scale = (1.0f / resolutions[gyro_range]) * (M_PI / 180.0f);
		printf("Gyroscope range: ±%d°/s, scale: %f rad/s/LSB\n",(int)ranges[gyro_range], *gyro_scale);
	} else {
		*gyro_scale = (1.0f / 16.384f) * (M_PI / 180.0f);  // Default to +/-2000 deg/s
		fprintf(stderr, "Using default gyro_range: ±2000°/s\n");
	}

	return 0;
}

// BMI08x init
static SensorContext bmi08x_init(const char* params, const char* dev_path) {
	BMI08xData* data = (BMI08xData*)malloc(sizeof(BMI08xData));
	if (!data) {
		return NULL;
	}

	memset(data, 0, sizeof(BMI08xData));
	data->params = params;

	/*
	* Read the active accel and gyro ranges from sysfs, then derive
	* the conversion factors used to convert raw register values into
	* physical units.
	*/
	// Read current ranges and calculate scale factors
	if (read_sensor_ranges(dev_path, &data->accel_scale, &data->gyro_scale) != 0) {
		// Fall back to documented default ranges on read failure
		data->accel_scale = (2 * 6.0f) / 65536.0f;  // +/-6g
		data->gyro_scale = (1.0f / 16.384f) * (M_PI / 180.0f);  // +/-2000 deg/s
		fprintf(stderr, "Using default scale factors\n");
	}

	/* Enable for local debug if needed */
	// printf("Accelerometer scale: %f g/LSB\n", data->accel_scale);
	// printf("Gyroscope scale: %f rad/s/LSB\n", data->gyro_scale);

	printf("BMI08x: Initializing with params: %s\n", params);

	// Save the selected device path
	strncpy(data->dev_path, dev_path, MAX_PATH_LEN);

	data->initialized = 1;
	return data;
}

// BMI08x read
static int bmi08x_read(SensorContext context, ImuData* data) {
	BMI08xData* bmi_data = (BMI08xData*)context;
	if (!bmi_data || !bmi_data->initialized) {
		return -1;
	}

	int accel_x_raw, accel_y_raw, accel_z_raw;
	int gyro_x_raw, gyro_y_raw, gyro_z_raw;

	// Read accelerometer data
	if (read_raw_signed(bmi_data->dev_path, "accel", "x", &accel_x_raw) ||
		read_raw_signed(bmi_data->dev_path, "accel", "y", &accel_y_raw) ||
		read_raw_signed(bmi_data->dev_path, "accel", "z", &accel_z_raw)) {
		return -1;
	}

	// Read gyroscope data
	if (read_raw_signed(bmi_data->dev_path, "anglvel", "x", &gyro_x_raw) ||
		read_raw_signed(bmi_data->dev_path, "anglvel", "y", &gyro_y_raw) ||
		read_raw_signed(bmi_data->dev_path, "anglvel", "z", &gyro_z_raw)) {
		return -1;
	}

	// Read sensor timestamp
	if (read_sensor_time(bmi_data->dev_path, &data->timestamp) != 0) {
		return -1;
	}

	// Convert raw values to physical units
	data->ax = accel_x_raw * bmi_data->accel_scale * 9.80665f; // Convert to m/s^2
	data->ay = accel_y_raw * bmi_data->accel_scale * 9.80665f;
	data->az = accel_z_raw * bmi_data->accel_scale * 9.80665f;

	data->gx = gyro_x_raw * bmi_data->gyro_scale;
	data->gy = gyro_y_raw * bmi_data->gyro_scale;
	data->gz = gyro_z_raw * bmi_data->gyro_scale;

	// BMI08x does not provide a magnetometer here
	data->mx = 0.0f;
	data->my = 0.0f;
	data->mz = 0.0f;

	// Mark the frame as valid
	data->status = 0;

	return 0;
}

// BMI08x release
static void bmi08x_release(SensorContext context) {
	BMI08xData* bmi_data = (BMI08xData*)context;
	if (bmi_data) {
		printf("BMI08x: Releasing resources\n");
		free(bmi_data);
	}
}

// BMI08x driver descriptor
const SensorDriver bmi08x_driver = {
	.name = "bmi08x",
	.init = bmi08x_init,
	.read = bmi08x_read,
	.release = bmi08x_release
};