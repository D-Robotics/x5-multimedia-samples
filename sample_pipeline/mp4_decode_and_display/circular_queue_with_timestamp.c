#include "circular_queue_with_timestamp.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>

int64_t get_current_timestamp_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); // 用单调时钟，避免系统时间修改影响
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

// 初始化循环队列
CircularQueue* cq_init(uint32_t capacity, size_t data_size) {
    // 参数合法性检查
    if (capacity == 0 || data_size == 0) {
        return NULL;
    }

    // 分配队列核心结构体
    CircularQueue* queue = (CircularQueue*)malloc(sizeof(CircularQueue));
    if (queue == NULL) {
        return NULL;
    }

    // 分配队列缓冲区（capacity个QueueNode）
    queue->buffer = (QueueNode*)calloc(capacity, sizeof(QueueNode));
    if (queue->buffer == NULL) {
        free(queue);
        return NULL;
    }
    for (int i = 0; i < capacity; i++)
    {
        queue->buffer[i].data = NULL;
        queue->buffer[i].is_valid = false;
        queue->buffer[i].timestamp_us = 0;
    }

    // 初始化队列参数
    queue->capacity = capacity;
    queue->wr_idx = 0;
    queue->valid_count = 0;
    queue->data_size = data_size;

    // 初始化多线程同步变量
    if (pthread_mutex_init(&queue->mutex, NULL) != 0 ||
        pthread_cond_init(&queue->not_empty, NULL) != 0) {
        free(queue->buffer);
        free(queue);
        return NULL;
    }

    return queue;
}
bool cq_enqueue(CircularQueue* queue, const void* data,
                OldDataHandleCallback old_data_cb, void* user_handle) {
    // 1. 参数合法性检查（新增对queue和data的基础校验）
    if (queue == NULL || data == NULL) {
        return false;
    }

    // 2. 加锁保护队列操作（保持线程安全）
    pthread_mutex_lock(&queue->mutex);

    // printf("cq_enqueue wr_idx=%u valid_count=%u/%u\n",
    //     queue->wr_idx, queue->valid_count, queue->capacity);
    // 3. 处理旧数据：覆盖前先回调处理（若有回调），再释放内存
    if (queue->buffer[queue->wr_idx].is_valid) {
        // 3.1 若有旧数据处理回调，先调用回调处理旧数据（如释放内部资源）
        if (old_data_cb != NULL) {
            old_data_cb(queue->buffer[queue->wr_idx].data, user_handle);
        }
        // 3.2 释放旧数据的内存（无论是否有回调，最终都需释放队列管理的data指针）
        free(queue->buffer[queue->wr_idx].data);
        queue->valid_count--; // 有效数减1（后续写入新数据后加回）
    }

    // 4. 分配新数据内存并复制数据（保持原有浅拷贝逻辑）
    void* new_data = malloc(queue->data_size);
    if (new_data == NULL) {
        pthread_mutex_unlock(&queue->mutex); // 分配失败，解锁后返回
        return false;
    }
    memcpy(new_data, data, queue->data_size); // 浅拷贝：适用于纯数据结构

    // 5. 更新当前位置节点信息（时间戳、有效性标记等）
    queue->buffer[queue->wr_idx].data = new_data;
    queue->buffer[queue->wr_idx].timestamp_us = get_current_timestamp_us();
    queue->buffer[queue->wr_idx].is_valid = true;
    queue->valid_count++; // 有效数加回，标记新数据有效

    // 6. 更新写入索引（环形缓冲区循环逻辑，保持不变）
    queue->wr_idx = (queue->wr_idx + 1) % queue->capacity;

    // 7. 唤醒等待数据的读线程（保持原有同步逻辑）
    pthread_cond_signal(&queue->not_empty);

    // 8. 解锁并返回成功
    pthread_mutex_unlock(&queue->mutex);
    return true;
}

// 按时间戳获取队列成员（回调函数复制数据）
// 按目标时间获取最近的队列成员（回调函数复制数据）
bool cq_get_by_timestamp(CircularQueue* queue, DataCallback callback, void* user_arg,
                         int64_t target_time_us,
                         int timeout_ms) {
    // 参数合法性检查（新增对target_time_us的基本校验，允许0但需外部保证有效性）
    if (queue == NULL || callback == NULL) {
        return false;
    }

    // 加锁保护队列操作
    pthread_mutex_lock(&queue->mutex);

    // 1. 处理等待逻辑：队列空时，根据timeout_ms等待
    struct timespec wait_ts;
    if (timeout_ms > 0) {
        // 计算绝对等待时间（当前时间 + timeout_ms）
        clock_gettime(CLOCK_REALTIME, &wait_ts);
        wait_ts.tv_sec += timeout_ms / 1000;
        wait_ts.tv_nsec += (timeout_ms % 1000) * 1000000;
        if (wait_ts.tv_nsec >= 1000000000) {
            wait_ts.tv_sec++;
            wait_ts.tv_nsec -= 1000000000;
        }
        // 带超时等待条件变量
        while (queue->valid_count == 0) {
            if (pthread_cond_timedwait(&queue->not_empty, &queue->mutex, &wait_ts) == ETIMEDOUT) {
                pthread_mutex_unlock(&queue->mutex);
                return false; // 超时返回
            }
        }
    } else if (timeout_ms < 0) {
        // 永久等待，直到队列非空
        while (queue->valid_count == 0) {
            pthread_cond_wait(&queue->not_empty, &queue->mutex);
        }
    } else {
        // 立即返回：队列空则直接失败
        if (queue->valid_count == 0) {
            pthread_mutex_unlock(&queue->mutex);
            return false;
        }
    }

    // 2. 找到“距离目标时间最近”的成员（核心逻辑修改点）
    uint32_t nearest_idx = 0;
    int64_t min_diff_us = INT64_MAX;  // 最小时间差（初始设为最大）
    bool found = false;

    for (uint32_t i = 0; i < queue->capacity; i++) {
        if (queue->buffer[i].is_valid) {
            // 计算当前成员与目标时间的绝对差值
            int64_t diff_us = llabs(queue->buffer[i].timestamp_us - target_time_us);

            // 更新最小差值和索引
            if (diff_us < min_diff_us) {
                min_diff_us = diff_us;
                nearest_idx = i;
                found = true;
            }
        }
    }

    // 3. 调用回调函数，将最近的成员数据传递给外部
    if (found && queue->buffer[nearest_idx].is_valid) {
        callback(queue->buffer[nearest_idx].data, user_arg);
    } else {
        // 理论上不会走到这里（队列非空且valid_count>0）
        pthread_mutex_unlock(&queue->mutex);
        return false;
    }

    // 解锁
    pthread_mutex_unlock(&queue->mutex);

    return true;
}


// 获取当前有效成员数
uint32_t cq_get_valid_count(CircularQueue* queue) {
    if (queue == NULL) {
        return 0;
    }

    pthread_mutex_lock(&queue->mutex);
    uint32_t count = queue->valid_count;
    pthread_mutex_unlock(&queue->mutex);

    return count;
}

// 销毁队列
void cq_destroy(CircularQueue** queue) {
    if (queue == NULL || *queue == NULL) {
        return;
    }

    CircularQueue* q = *queue;

    // 加锁保护销毁操作
    pthread_mutex_lock(&q->mutex);

    // 1. 释放所有有效数据的内存
    for (uint32_t i = 0; i < q->capacity; i++) {
        if (q->buffer[i].is_valid) {
            free(q->buffer[i].data);
            q->buffer[i].is_valid = false;
        }
    }

    // 2. 释放缓冲区和队列结构体
    free(q->buffer);
    q->buffer = NULL;

    // 3. 销毁同步变量
    pthread_mutex_unlock(&q->mutex);
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->not_empty);

    // 4. 释放队列指针并置NULL
    free(q);
    *queue = NULL;
}