#ifndef UTILS_MEM_H
#define UTILS_MEM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MALLOC_TRIM_INTERVAL_MS (60U * 1000U)

// 周期调用：每 interval_ms 最多一次，将 glibc arena 滞留的空闲堆归还 OS，interval_ms 为 0 则不修剪。
void utils_periodic_malloc_trim(uint32_t interval_ms);

#ifdef __cplusplus
}
#endif

#endif /* UTILS_MEM_H */
