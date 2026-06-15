/***************************************************************************
 *                      COPYRIGHT NOTICE
 *             Copyright(C) 2024-2025, D-Robotics Co., Ltd.
 *                     All rights reserved.
 ***************************************************************************/

#ifndef SAMPLE_IMU_H
#define SAMPLE_IMU_H

#include "imu_manager.h"

/**
 * @brief Opaque driver-defined private context.
 *
 * Each sensor driver allocates and owns a context that holds its runtime state
 * (e.g. device paths, scales). The manager passes this pointer to read/release
 * callbacks; the driver must not expose its internal structure to the caller.
 */
typedef void* SensorContext;

/**
 * @brief Driver callback: initialize sensor and return context.
 *
 * @param[in] params   Driver-specific init parameters; may be NULL.
 * @param[in] dev_path IIO device sysfs path (e.g. /sys/bus/iio/devices/iio:device0).
 *
 * @retval non-NULL  Success, context to be used for read/release.
 * @retval NULL      Init failed (e.g. invalid path, open error).
 */
typedef SensorContext (*SensorInitFunc)(const char* params, const char* dev_path);

/**
 * @brief Driver callback: read one IMU frame into ImuData.
 *
 * @param[in]  context  Context from SensorInitFunc.
 * @param[out] data     Output buffer; driver must fill ax/ay/az, gx/gy/gz,
 *                      timestamp, and optionally mx/my/mz and status.
 *
 * @retval  0  Success.
 * @retval -1  Read error (e.g. sysfs read failed).
 */
typedef int (*SensorReadFunc)(SensorContext context, ImuData* data);

/**
 * @brief Driver callback: release context and free resources.
 *
 * Called when the sensor handle is released. After return, context must not
 * be used. Safe to call with context NULL (no-op).
 *
 * @param[in] context  Context from SensorInitFunc; may be NULL.
 */
typedef void (*SensorReleaseFunc)(SensorContext context);

/**
 * @brief Descriptor of a sensor driver (name + callbacks).
 *
 * Used to register a driver with the manager. name must match the IIO device
 * "name" attribute for auto-discovery.
 */
typedef struct SensorDriver {
	const char* name;   /**< Driver name, must match IIO device name */
	SensorInitFunc init;     /**< Initialize and return context */
	SensorReadFunc read;     /**< Read one frame into ImuData */
	SensorReleaseFunc release; /**< Release context and free resources */
} SensorDriver;

/**
 * @brief Bound driver instance (driver + context).
 *
 * Created by the manager when init_sensor() succeeds; internal use only.
 */
typedef struct SensorInstance {
	const SensorDriver* driver; /**< Driver that owns the context */
	SensorContext context;     /**< Opaque context from driver->init() */
} SensorInstance;

#endif // SENSOR_INTERFACE_H