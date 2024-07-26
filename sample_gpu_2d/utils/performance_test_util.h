#ifndef __PERFORMANCE_HH__
#define __PERFORMANCE_HH__
#include <stdio.h>
#include <getopt.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdint.h>

#include "GC820/nano2D.h"
#include "GC820/nano2D_util.h"

struct PerformanceTestParam
{
	int mode;	  //0: sample, 1: performance test
	int iteration_number;
	int image_width;
	int image_height;
	char *test_case; //test case's name

	uint64_t test_start_time_us;
	uint64_t test_end_time_us;

};

void print_help(char *test_case);
int parser_params(int argc, char** argv, struct PerformanceTestParam *param);

void performance_test_stop(struct PerformanceTestParam *param);
void performance_test_start(struct PerformanceTestParam *param);

n2d_error_t performance_test_create_buffer_black(struct PerformanceTestParam *param,
	n2d_buffer_format_t format, n2d_buffer_t *src);
n2d_error_t performance_test_create_buffer_with_rect(struct PerformanceTestParam *param,
	n2d_buffer_format_t format, n2d_buffer_t *src);

n2d_error_t performance_test_save_to_file(struct PerformanceTestParam *param,
	n2d_buffer_t *src, char* file_name_suffix);
#endif
