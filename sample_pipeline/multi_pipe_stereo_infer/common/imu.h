#ifndef _IMU_H_
#define _IMU_H_

#include <stdint.h>

#define IMU_SUBNODE_MAX_NUM		16

typedef struct {
	uint64_t timestamp;       // 时间戳
	float ax, ay, az;     // 加速度计数据
	float gx, gy, gz;     // 陀螺仪数据
} imu_subnode_data_t;

int imu_init(int verbose);
void imu_destroy(void);
int imu_get_data(void);
int imu_data_flush_buffer(void *buffer);

#endif

