#ifndef CIRCULAR_QUEUE_WITH_TIMESTAMP_H
#define CIRCULAR_QUEUE_WITH_TIMESTAMP_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

// 队列成员结构体（含数据指针+时间戳）
typedef struct {
    void* data;               // 数据指针（指向外部传入的数据）
    int64_t timestamp_us;     // 入队时间戳（微秒，基于系统时间）
    bool is_valid;            // 标记该位置数据是否有效
} QueueNode;

// 循环队列核心结构体（对外隐藏实现，通过指针操作）
typedef struct {
    QueueNode* buffer;        // 队列缓冲区（动态分配）
    uint32_t capacity;        // 队列最大容量（可存储的成员数）
    uint32_t wr_idx;          // 写入索引（环形缓冲区的下一个写入位置）
    uint32_t valid_count;     // 当前有效成员数
    size_t data_size;         // 单个数据的字节大小（用于内存复制）

    // 多线程同步变量
    pthread_mutex_t mutex;    // 保护队列读写的互斥锁
    pthread_cond_t not_empty; // 队列非空条件变量（唤醒读线程）
} CircularQueue;

/**
 * @brief 初始化循环队列
 * @param capacity 队列最大容量（>0）
 * @param data_size 单个数据的字节大小（如sizeof(int)、sizeof(MyStruct)）
 * @return 成功返回队列指针，失败返回NULL
 */
CircularQueue* cq_init(uint32_t capacity, size_t data_size);

/**
 * @brief 入队（自动覆盖旧数据，线程安全）
 * @param queue 队列指针（不可为NULL）
 * @param data 待入队数据的指针（不可为NULL，需与初始化时data_size匹配）
 * @return 成功返回true，失败（参数无效）返回false
 */
typedef void (*OldDataHandleCallback)(void* old_data, void* user_handle);
bool cq_enqueue(CircularQueue* queue, const void* data, OldDataHandleCallback old_data_cb, void* user_handle);

/**
 * @brief 按时间戳获取队列成员（通过回调函数复制数据，线程安全）
 * @param queue 队列指针（不可为NULL）
 * @param callback 数据处理回调函数（参数：data-队列中的数据，user_arg-用户自定义参数）
 * @param user_arg 传给回调函数的自定义参数（可NULL）
 * @param timeout_ms 等待超时时间（ms）：0=立即返回，>0=等待指定时间，<0=永久等待
 * @return 成功获取到数据返回true，超时/失败返回false
 */
typedef void (*DataCallback)(const void* data, void* user_arg);
bool cq_get_by_timestamp(CircularQueue* queue, DataCallback callback, void* user_arg, 
        int64_t target_time_us, int timeout_ms);

/**
 * @brief 获取队列当前有效成员数（线程安全）
 * @param queue 队列指针（不可为NULL）
 * @return 有效成员数
 */
uint32_t cq_get_valid_count(CircularQueue* queue);

/**
 * @brief 销毁队列（释放所有资源，线程安全）
 * @param queue 队列指针（不可为NULL，销毁后指针置NULL）
 */
void cq_destroy(CircularQueue** queue);

int64_t get_current_timestamp_us(void);

#endif // CIRCULAR_QUEUE_WITH_TIMESTAMP_H