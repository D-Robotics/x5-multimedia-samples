#include "utils_mem.h"

#include <malloc.h>

#include "time_utils.h"

// static 计数器不做加锁/原子保护。若将来需多线程调用，需先加互斥保护
void utils_periodic_malloc_trim(uint32_t interval_ms)
{
	static uint64_t last_trim_ms = 0;
	uint64_t now;

	if (interval_ms == 0) {
		return;
	}

	now = get_timestamp_ms();

	if (last_trim_ms == 0 || (now - last_trim_ms) >= interval_ms) {
		last_trim_ms = now;
		malloc_trim(0);
	}
}
