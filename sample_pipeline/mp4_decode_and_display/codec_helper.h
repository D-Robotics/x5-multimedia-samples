#ifndef CODEC_HELPER_H
#define CODEC_HELPER_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"

int write_h264_header(AVCodecParameters *codec_params, FILE *fp);
int write_h265_header(AVCodecParameters *codec_params, FILE *fp);
#endif