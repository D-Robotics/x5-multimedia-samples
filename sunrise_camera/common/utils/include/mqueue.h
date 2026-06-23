#ifndef MQUEUE_H_
#define MQUEUE_H_

#include <pthread.h>
#include <stdint.h>

typedef enum
{
	E_QUEUE_OK,
	E_QUEUE_ERROR_FAILED,
	E_QUEUE_ERROR_TIMEOUT,
	E_QUEUE_ERROR_NO_MEM,
	E_QUEUE_ERROR_FULL,
} teQueueStatus;

typedef struct {
	void **apvBuffer;
	uint32_t u32Length;
	uint32_t u32Front;
	uint32_t u32Rear;

	pthread_mutex_t mutex;
	pthread_cond_t cond_space_available;
	pthread_cond_t cond_data_available;
} tsQueue;

teQueueStatus mQueueCreate(tsQueue *psQueue, uint32_t u32Length);
teQueueStatus mQueueDestroy(tsQueue *psQueue);
teQueueStatus mQueueEnqueue(tsQueue *psQueue, void *pvData);
teQueueStatus mQueueEnqueueEx(tsQueue *psQueue, void *pvData);
teQueueStatus mQueueDequeue(tsQueue *psQueue, void **ppvData);
teQueueStatus mQueueDequeueTimed(tsQueue *psQueue, uint32_t u32WaitTimeMil, void **ppvData);

/** 从队尾窥视：只读最新一帧指针，不从队列移除，供 venc/drm 只用不取走，release 由 vflow 统一做 */
teQueueStatus mQueuePeekFromRear(tsQueue *psQueue, void **ppvData);
teQueueStatus mQueuePeekFromRearTimed(tsQueue *psQueue, uint32_t u32WaitTimeMil, void **ppvData);

/** 满 1 / 非满 0；psQueue 为 NULL 时返回 -1 */
int mQueueIsFull(tsQueue *psQueue);
/** 空 1 / 非空 0；psQueue 为 NULL 时返回 -1 */
int mQueueIsEmpty(tsQueue *psQueue);
/** 元素个数；psQueue 为 NULL 时返回 0（与空队列相同，调用方应保证指针有效） */
uint32_t mQueueGetCount(tsQueue *psQueue);

#endif	// MQUEUE_H_
