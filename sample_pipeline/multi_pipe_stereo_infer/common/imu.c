#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <pthread.h>

#include "imu.h"
#include "imu_manager.h"


typedef struct {
	imu_subnode_data_t *data;      // 存储队列元素的数组
	int data_size;
	int front;      // 队首指针（指向第一个元素）
	int rear;       // 队尾指针（指向下一个插入位置）
	int capacity;   // 队列容量（实际最多存储 capacity-1 个元素）
} CircularQueue;

typedef struct {
	SensorHandle imu_hdl;
	pthread_mutex_t mutex;
	CircularQueue *queue;
	imu_subnode_data_t imu_data[IMU_SUBNODE_MAX_NUM];
}IMU_CFG_T;

static int verbose_flag = 0;
static IMU_CFG_T imu_cfg = {0};

// 初始化队列
CircularQueue* createQueue(int capacity, void *data_ptr, int data_size) {
	CircularQueue *queue = (CircularQueue*)malloc(sizeof(CircularQueue));
	queue->data = data_ptr;
	queue->data_size = data_size;
	queue->front = 0;
	queue->rear = 0;
	queue->capacity = capacity;
	return queue;
}

// 判断队列是否为空
bool isEmpty(CircularQueue *queue) {
	return queue->front == queue->rear;
}

// 判断队列是否已满
bool isFull(CircularQueue *queue) {
	return (queue->rear + 1) % queue->capacity == queue->front;
}

// 入队操作（队列满时覆盖最早的数据）
void enqueue(CircularQueue *queue, void *item) {
	if (isFull(queue)) {
		// 队列已满：丢弃队首元素（覆盖最早数据）
		queue->front = (queue->front + 1) % queue->capacity; // 移动 front 丢弃旧数据
	}
	// 插入新数据到队尾
	memcpy((void *)&queue->data[queue->rear], item, queue->data_size);
	// queue->data[queue->rear] = item;
	queue->rear = (queue->rear + 1) % queue->capacity; // 循环移动 rear
}

// 出队操作（返回被移除的元素）
int dequeue(CircularQueue *queue, void *item) {
	if (isEmpty(queue)) {
		printf("Queue is empty!\n");
		return -1; // 错误码
	}
	// int item = queue->data[queue->front];
	memcpy(item, (void *)&queue->data[queue->front], queue->data_size);
	queue->front = (queue->front + 1) % queue->capacity;
	return 0;
}

// 获取队首元素（不移除）
// int front(CircularQueue *queue) {
//     if (isEmpty(queue)) {
//         printf("Queue is empty!\n");
//         return -1;
//     }
//     return queue->data[queue->front];
// }

// 释放队列内存
void freeQueue(CircularQueue *queue) {
	// free(queue->data);
	free(queue);
}

/**
 * 清空队列并将所有元素按FIFO顺序存入新缓冲区
 * @param queue 队列指针
 * @param returnSize 返回元素个数的指针
 * @return 
 */
int flushQueue(CircularQueue *queue, void *buffer) {
	// 计算当前队列元素数量
	int returnSize = (queue->rear - queue->front + queue->capacity) % queue->capacity;

	// 复制元素到新缓冲区（保持FIFO顺序）
	int current = queue->front;
	uint8_t *ptr = (uint8_t *)buffer;
	for (int i = 0; i < returnSize; i++) {
		// buffer[i] = queue->data[current];
		memcpy(ptr, &queue->data[current], queue->data_size);
		current = (current + 1) % queue->capacity;  // 循环移动指针[1,3](@ref)
		ptr += queue->data_size;
	}

	// 清空队列
	queue->front = 0;
	queue->rear = 0;

	return returnSize;
}

/* 获取当前时间戳（微秒） */
static uint64_t get_current_timestamp_us() {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

int imu_init(int verbose)
{
	IMU_CFG_T *pimu_cfg = &imu_cfg;
	pimu_cfg->imu_hdl = init_sensor("bmi08x", "");
	if (!pimu_cfg->imu_hdl) {
		printf("IMU is not Ready\n");
		return -1;
	}

	pimu_cfg->queue = createQueue(IMU_SUBNODE_MAX_NUM, (void *)&pimu_cfg->imu_data, sizeof(imu_subnode_data_t));
	if (!pimu_cfg->queue) {
		printf("[IMU] createQueue failed\n");
		return -1;
	}
	verbose_flag = verbose;
	pthread_mutex_init(&pimu_cfg->mutex, NULL);

	return 0;
}

void imu_destroy(void)
{
	IMU_CFG_T *pimu_cfg = &imu_cfg;
	pthread_mutex_destroy(&pimu_cfg->mutex);

	freeQueue(pimu_cfg->queue);

	if (pimu_cfg->imu_hdl)
		release_sensor(pimu_cfg->imu_hdl);
}

int imu_get_data()
{
	IMU_CFG_T *pimu_cfg = &imu_cfg;
	ImuData imu_data;
	imu_subnode_data_t imu_subnode_data;

	if (read_sensor_data(pimu_cfg->imu_hdl, &imu_data) == 0) {

		imu_subnode_data.ax = imu_data.ax;
		imu_subnode_data.ay = imu_data.ay;
		imu_subnode_data.az = imu_data.az;
		imu_subnode_data.gx = imu_data.gx;
		imu_subnode_data.gy = imu_data.gy;
		imu_subnode_data.gz = imu_data.gz;
		// 使用系统时间作为时间戳
		imu_subnode_data.timestamp = get_current_timestamp_us();

		pthread_mutex_lock(&pimu_cfg->mutex);
		enqueue(pimu_cfg->queue, &imu_subnode_data);
		pthread_mutex_unlock(&pimu_cfg->mutex);

		/* Debug */
		if (verbose_flag) {
			printf("[IMU] Data received pts[%ld]: =======> \n", imu_subnode_data.timestamp);
			printf("  Accelerometer: [%f, %f, %f] m/s²\n", imu_subnode_data.ax, imu_subnode_data.ay, imu_subnode_data.az);
			printf("  Gyroscope:     [%f, %f, %f] rad/s\n", imu_subnode_data.gx, imu_subnode_data.gy, imu_subnode_data.gz);
		}
//		print_imu_data(&data);

	} else {
		printf("Error: Failed to read data from IMU (Frame)\n");
		return -1;
	}

	return 0;
}

int imu_data_flush_buffer(void *buffer)
{
	IMU_CFG_T *pimu_cfg = &imu_cfg;
	int imu_data_num = 0;

	pthread_mutex_lock(&pimu_cfg->mutex);

	if (isEmpty(pimu_cfg->queue)) {
		pthread_mutex_unlock(&pimu_cfg->mutex);
		return 0;
	}

	imu_data_num = flushQueue(pimu_cfg->queue, buffer);

	pthread_mutex_unlock(&pimu_cfg->mutex);

	if (verbose_flag) {
		printf("[IMU] Flush Data: num = %d\n", imu_data_num);
	}

	return imu_data_num;
}

