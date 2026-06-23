#include "mqueue.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#ifndef NSEC_PER_SEC
#define NSEC_PER_SEC 1000000000UL
#endif

/*******************************************************************************
** 函 数 名  : mQueueCreate
** 功能描述  : 创建消息队列
** 输入参数  : tsQueue *psQueue
			 : uint32_t u32Length
** 返 回 值  :
*******************************************************************************/
teQueueStatus mQueueCreate(tsQueue *psQueue, uint32_t u32Length)
{
	if (psQueue == NULL || u32Length == 0)
		return E_QUEUE_ERROR_FAILED;

	psQueue->apvBuffer = malloc(sizeof(void *) * u32Length);
	if (!psQueue->apvBuffer) {
		return E_QUEUE_ERROR_NO_MEM;
	}

	if (pthread_mutex_init(&psQueue->mutex, NULL) != 0) {
		free(psQueue->apvBuffer);
		psQueue->apvBuffer = NULL;
		return E_QUEUE_ERROR_FAILED;
	}
	if (pthread_cond_init(&psQueue->cond_space_available, NULL) != 0) {
		pthread_mutex_destroy(&psQueue->mutex);
		free(psQueue->apvBuffer);
		psQueue->apvBuffer = NULL;
		return E_QUEUE_ERROR_FAILED;
	}
	if (pthread_cond_init(&psQueue->cond_data_available, NULL) != 0) {
		pthread_cond_destroy(&psQueue->cond_space_available);
		pthread_mutex_destroy(&psQueue->mutex);
		free(psQueue->apvBuffer);
		psQueue->apvBuffer = NULL;
		return E_QUEUE_ERROR_FAILED;
	}

	psQueue->u32Length = u32Length;
	psQueue->u32Front = 0;
	psQueue->u32Rear = 0;

	return E_QUEUE_OK;
}

/*******************************************************************************
** 函 数 名  : mQueueDestroy
** 功能描述  : 销毁创建的消息队列
** 输入参数  : tsQueue *psQueue
** 返 回 值  :
*******************************************************************************/
teQueueStatus mQueueDestroy(tsQueue *psQueue)
{
	if (psQueue == NULL)
		return E_QUEUE_ERROR_FAILED;

	if (NULL == psQueue->apvBuffer) {
		return E_QUEUE_ERROR_FAILED;
	}
	free(psQueue->apvBuffer);

	pthread_mutex_destroy(&psQueue->mutex);
	pthread_cond_destroy(&psQueue->cond_space_available);
	pthread_cond_destroy(&psQueue->cond_data_available);

	return E_QUEUE_OK;
}

/*******************************************************************************
** 函 数 名  : mQueueEnqueue
** 功能描述  : 入队函数，如果空间已满，需要等待空间释放，然后广播队列中有数
			   据可用，入队的内存需要手动申请，然后在出队地方释放
** 输入参数  : tsQueue *psQueue
			 : void *pvData
** 返 回 值  :
*******************************************************************************/
teQueueStatus mQueueEnqueue(tsQueue *psQueue, void *pvData)
{
	if (psQueue == NULL)
		return E_QUEUE_ERROR_FAILED;

	pthread_mutex_lock(&psQueue->mutex);
	while (((psQueue->u32Rear + 1) % psQueue->u32Length) == psQueue->u32Front)
		pthread_cond_wait(&psQueue->cond_space_available, &psQueue->mutex);
	psQueue->apvBuffer[psQueue->u32Rear] = pvData;

	psQueue->u32Rear = (psQueue->u32Rear + 1) % psQueue->u32Length;

	pthread_cond_broadcast(&psQueue->cond_data_available);
	pthread_mutex_unlock(&psQueue->mutex);
	return E_QUEUE_OK;
}

/*******************************************************************************
** 函 数 名  : mQueueEnqueueEx
** 功能描述  : 入队函数，如果空间已满，不入队，立即返回
** 输入参数  : tsQueue *psQueue
			 : void *pvData
** 返 回 值  :
*******************************************************************************/
teQueueStatus mQueueEnqueueEx(tsQueue *psQueue, void *pvData)
{
	if (psQueue == NULL)
		return E_QUEUE_ERROR_FAILED;

	pthread_mutex_lock(&psQueue->mutex);
	while (((psQueue->u32Rear + 1) % psQueue->u32Length) == psQueue->u32Front) {
		pthread_cond_broadcast(&psQueue->cond_data_available);
		pthread_mutex_unlock(&psQueue->mutex);
		return E_QUEUE_ERROR_FULL;
	}
	psQueue->apvBuffer[psQueue->u32Rear] = pvData;

	psQueue->u32Rear = (psQueue->u32Rear + 1) % psQueue->u32Length;

	pthread_cond_broadcast(&psQueue->cond_data_available);
	pthread_mutex_unlock(&psQueue->mutex);
	return E_QUEUE_OK;
}

/*******************************************************************************
** 函 数 名  : mQueueDequeue
** 功能描述  : 出队函数，需要等待队列中有数据可用，读出数据后需要广播队列中
			   空间可用，调用出队函数的地方需要释放入队分配的内存
** 输入参数  : tsQueue *psQueue
			 : void **ppvData
** 返 回 值  :
*******************************************************************************/
teQueueStatus mQueueDequeue(tsQueue *psQueue, void **ppvData)
{
	if (psQueue == NULL || ppvData == NULL)
		return E_QUEUE_ERROR_FAILED;

	pthread_mutex_lock(&psQueue->mutex);
	while (psQueue->u32Front == psQueue->u32Rear)
		pthread_cond_wait(&psQueue->cond_data_available, &psQueue->mutex);

	*ppvData = psQueue->apvBuffer[psQueue->u32Front];

	psQueue->u32Front = (psQueue->u32Front + 1) % psQueue->u32Length;
	pthread_cond_broadcast(&psQueue->cond_space_available);
	pthread_mutex_unlock(&psQueue->mutex);
	return E_QUEUE_OK;
}

/*******************************************************************************
** 函 数 名  : mQueueDequeueTimed
** 功能描述  : 具有延时等待功能的出队函数，可以设置等待时间然后返回，避免阻
			   塞
** 输入参数  : tsQueue *psQueue
			 : uint32_t u32WaitTimeout
			 : void **ppvData
** 返 回 值  :
*******************************************************************************/
teQueueStatus mQueueDequeueTimed(tsQueue *psQueue, uint32_t u32WaitTimeMil, void **ppvData)
{
	if (psQueue == NULL || ppvData == NULL)
		return E_QUEUE_ERROR_FAILED;

	pthread_mutex_lock(&psQueue->mutex);
	while (psQueue->u32Front == psQueue->u32Rear) {
		struct timeval sNow;
		struct timespec sTimeout;

		memset(&sNow, 0, sizeof(struct timeval));
		gettimeofday(&sNow, NULL);
		sTimeout.tv_sec = sNow.tv_sec + (u32WaitTimeMil / 1000);
		sTimeout.tv_nsec = (sNow.tv_usec + ((u32WaitTimeMil % 1000) * 1000)) * 1000;
		if (sTimeout.tv_nsec >= (long)NSEC_PER_SEC) {
			sTimeout.tv_sec++;
			sTimeout.tv_nsec -= (long)NSEC_PER_SEC;
		}
		/*printf("Dequeue timed: now	%lu s, %lu ns\n", sNow.tv_sec, sNow.tv_usec * 1000);*/
		/*printf("Dequeue timed: until  %lu s, %lu ns\n", sTimeout.tv_sec, sTimeout.tv_nsec);*/

		switch (pthread_cond_timedwait(&psQueue->cond_data_available, &psQueue->mutex, &sTimeout)) {
			case (0):
				break;

			case (ETIMEDOUT):
				pthread_mutex_unlock(&psQueue->mutex);
				return E_QUEUE_ERROR_TIMEOUT;
				break;

			default:
				pthread_mutex_unlock(&psQueue->mutex);
				return E_QUEUE_ERROR_FAILED;
		}
	}

	*ppvData = psQueue->apvBuffer[psQueue->u32Front];

	psQueue->u32Front = (psQueue->u32Front + 1) % psQueue->u32Length;
	pthread_cond_broadcast(&psQueue->cond_space_available);
	pthread_mutex_unlock(&psQueue->mutex);
	return E_QUEUE_OK;
}

/*******************************************************************************
** 函 数 名  : mQueuePeekFromRear
** 功能描述  : 从队尾窥视，只读最新一帧指针，不从队列移除（不修改 Front/Rear）
*******************************************************************************/
teQueueStatus mQueuePeekFromRear(tsQueue *psQueue, void **ppvData)
{
	if (psQueue == NULL || ppvData == NULL)
		return E_QUEUE_ERROR_FAILED;

	pthread_mutex_lock(&psQueue->mutex);
	if (psQueue->u32Front == psQueue->u32Rear) {
		pthread_mutex_unlock(&psQueue->mutex);
		return E_QUEUE_ERROR_FAILED;
	}
	{
		uint32_t idx = (psQueue->u32Rear + psQueue->u32Length - 1) % psQueue->u32Length;
		*ppvData = psQueue->apvBuffer[idx];
	}
	pthread_mutex_unlock(&psQueue->mutex);
	return E_QUEUE_OK;
}

/*******************************************************************************
** 函 数 名  : mQueuePeekFromRearTimed
** 功能描述  : 从队尾窥视（只读最新，不移除），带超时等待
*******************************************************************************/
teQueueStatus mQueuePeekFromRearTimed(tsQueue *psQueue, uint32_t u32WaitTimeMil, void **ppvData)
{
	if (psQueue == NULL || ppvData == NULL)
		return E_QUEUE_ERROR_FAILED;

	pthread_mutex_lock(&psQueue->mutex);
	while (psQueue->u32Front == psQueue->u32Rear) {
		struct timeval sNow;
		struct timespec sTimeout;
		memset(&sNow, 0, sizeof(struct timeval));
		gettimeofday(&sNow, NULL);
		sTimeout.tv_sec = sNow.tv_sec + (u32WaitTimeMil / 1000);
		sTimeout.tv_nsec = (sNow.tv_usec + ((u32WaitTimeMil % 1000) * 1000)) * 1000;
		if (sTimeout.tv_nsec >= (long)NSEC_PER_SEC) {
			sTimeout.tv_sec++;
			sTimeout.tv_nsec -= (long)NSEC_PER_SEC;
		}
		switch (pthread_cond_timedwait(&psQueue->cond_data_available, &psQueue->mutex, &sTimeout)) {
			case 0:
				break;
			case ETIMEDOUT:
				pthread_mutex_unlock(&psQueue->mutex);
				return E_QUEUE_ERROR_TIMEOUT;
			default:
				pthread_mutex_unlock(&psQueue->mutex);
				return E_QUEUE_ERROR_FAILED;
		}
	}
	{
		uint32_t idx = (psQueue->u32Rear + psQueue->u32Length - 1) % psQueue->u32Length;
		*ppvData = psQueue->apvBuffer[idx];
	}
	pthread_mutex_unlock(&psQueue->mutex);
	return E_QUEUE_OK;
}

int mQueueIsEmpty(tsQueue *psQueue)
{
	if (psQueue == NULL)
		return -1;

	pthread_mutex_lock(&psQueue->mutex);
	if (psQueue->u32Rear == psQueue->u32Front) {
		pthread_mutex_unlock(&psQueue->mutex);
		return 1;
	}
	pthread_mutex_unlock(&psQueue->mutex);
	return 0;
}

int mQueueIsFull(tsQueue *psQueue)
{
	int full;

	if (psQueue == NULL)
		return -1;

	pthread_mutex_lock(&psQueue->mutex);
	full = (((psQueue->u32Rear + 1) % psQueue->u32Length) == psQueue->u32Front);
	pthread_mutex_unlock(&psQueue->mutex);
	return full;
}

uint32_t mQueueGetCount(tsQueue *psQueue)
{
	uint32_t count;

	if (psQueue == NULL)
		return 0; /* 与空队列无法区分，调用方须保证 psQueue 有效 */

	pthread_mutex_lock(&psQueue->mutex);

	if (psQueue->u32Rear >= psQueue->u32Front) {
		count = psQueue->u32Rear - psQueue->u32Front;
	} else {
		count = psQueue->u32Length - psQueue->u32Front + psQueue->u32Rear;
	}

	pthread_mutex_unlock(&psQueue->mutex);
	return count;
}
