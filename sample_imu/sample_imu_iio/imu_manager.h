/***************************************************************************
 *                      COPYRIGHT NOTICE
 *             Copyright(C) 2024-2025, D-Robotics Co., Ltd.
 *                     All rights reserved.
 ***************************************************************************/

#ifndef SENSOR_MANAGER_H
#define SENSOR_MANAGER_H

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

/**
 * @brief Unified IMU sample data (one frame).
 *
 * All physical values use SI units where applicable. Magnetometer fields
 * may be zero if the driver does not support them.
 */
typedef struct {
	float ax, ay, az;     /**< Accelerometer (m/s²) */
	float gx, gy, gz;     /**< Gyroscope (rad/s) */
	float mx, my, mz;     /**< Magnetometer (driver-defined units; 0 if N/A) */
	uint64_t timestamp;   /**< Timestamp (microseconds) */
	int32_t status;       /**< Status code; 0 = valid, non-zero = error/flag */
} ImuData;

/**
 * @brief Opaque sensor handle.
 *
 * Obtained from init_sensor(); must be passed to read_sensor_data() and
 * finally to release_sensor(). Do not dereference or cast.
 */
typedef void* SensorHandle;

/**
 * @brief List all supported sensor names to stdout.
 *
 * Prints the names of sensor drivers built into this library (e.g. bmi08x,
 * icm42688-gyro, icm42688-accel) for user reference. Does not probe hardware.
 */
void list_available_sensors(void);

/**
 * @brief Initialize the selected sensor and return an opaque handle.
 *
 * Scans IIO devices under /sys/bus/iio/devices/, validates the first device
 * matching sensor_type, and initializes the corresponding driver. The handle
 * must be released with release_sensor() when no longer needed.
 *
 * @param[in] sensor_type  Sensor driver name (e.g. "bmi08x", "icm42688-gyro",
 *                         "icm42688-accel"). Must not be NULL.
 * @param[in] params       Driver-specific init parameters; may be NULL if the
 *                         driver does not require params.
 *
 * @retval non-NULL  Valid SensorHandle on success.
 * @retval NULL      Invalid sensor_type, no matching IIO device found,
 *                   allocation failure, or driver init failure.
 */
SensorHandle init_sensor(const char* sensor_type, const char* params);

/**
 * @brief Read one frame of IMU data from the sensor.
 *
 * Fills the provided ImuData with accelerometer, gyroscope, and timestamp.
 * Magnetometer fields may be zero if the driver does not support them.
 *
 * @param[in]  handle  Sensor handle from init_sensor(). Must not be NULL.
 * @param[out] data    Output buffer for one sample. Must not be NULL.
 *
 * @retval  0   Success, data has been filled.
 * @retval -1   Invalid handle or data, or driver read error.
 */
int read_sensor_data(SensorHandle handle, ImuData* data);

/**
 * @brief Release sensor resources and invalidate the handle.
 *
 * Calls the driver's release callback and frees the handle. After return,
 * handle must not be used. Safe to call with NULL (no-op).
 *
 * @param[in] handle  Sensor handle from init_sensor(); may be NULL.
 */
void release_sensor(SensorHandle handle);

/**
 * @brief Print one IMU data frame in human-readable form to stdout.
 *
 * Outputs accelerometer (m/s²), gyroscope (rad/s), and timestamp. Intended
 * for debugging or demo output.
 *
 * @param[in] data  Pointer to one ImuData frame; must not be NULL.
 */
void print_imu_data(const ImuData* data);

/**
 * @brief Get the name of the first detected supported sensor.
 *
 * Scans IIO devices and returns the driver name of the first sensor that
 * matches a built-in driver (e.g. for auto-selection in demos).
 *
 * @retval non-NULL  Constant string of the sensor name (e.g. "bmi08x").
 * @retval NULL      No supported sensor was found on the system.
 */
const char* get_default_imu_name(void);

#endif // SENSOR_MANAGER_H