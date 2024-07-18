#ifndef ___VP_PIPELINE__
#define ___VP_PIPELINE__
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include "common_utils.h"
int vp_create_and_start_pipeline(pipe_contex_t *pipe_contex, int active_mipi_host, int vse_bind_index, uint32_t sensor_mode);
int vp_destroy_and_stop_pipeline(pipe_contex_t *pipe_contex);
#endif //