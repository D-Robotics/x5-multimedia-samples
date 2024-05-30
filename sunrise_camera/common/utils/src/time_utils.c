#include"utils_log.h"
#include"time_utils.h"

uint64_t get_timestamp_ms() {

    uint64_t timestamp;
    struct timeval ts;

    gettimeofday(&ts, NULL);
    timestamp = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_usec / 1000;
    return timestamp;
}

void time_statistics_at_beginning_of_loop(struct TimeStatistics *statistics){
    statistics->start_ms = get_timestamp_ms();
    statistics->loop_period_ms = statistics->start_ms - statistics->last_ms;
    statistics->last_ms = statistics->start_ms;
}
void time_statistics_at_ending_of_loop(struct TimeStatistics *statistics){
    statistics->end_ms = get_timestamp_ms();
}
void time_statistics_info_show(struct TimeStatistics *statistics, const char *tag, bool is_open){
    if(!is_open)
        return;

    int64_t period_ms = statistics->loop_period_ms;
    int64_t consume_ms = statistics->end_ms - statistics->start_ms;

    SC_LOGI("[%s] period %lldms consume %lldms.", tag, period_ms, consume_ms);
}