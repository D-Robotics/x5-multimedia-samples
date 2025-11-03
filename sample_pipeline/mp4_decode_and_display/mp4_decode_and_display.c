/***************************************************************************
 * COPYRIGHT NOTICE
 * Copyright 2025 D-Robotics, Inc.
 * All rights reserved.
 ***************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>
#include <signal.h>
#include <sys/select.h>
#include <errno.h>

// FFmpeg headers
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavcodec/bsf.h"
#include "libavutil/avutil.h"
#include "libavutil/time.h"
#include "libavutil/error.h"

#include "hbmem.h"
#include "hbn_api.h"

#include "vp_codec.h"
#include "vp_display.h"
#include "codec_helper.h"
#include "circular_queue_with_timestamp.h"

#define TAG "[MP4_PLAYER]"
#define MAX_LINE_LENGTH 256
#define ALIGN_32(v) ((v + (32 - 1)) / 32 * 32)

#define DECODEC_FRAME_BUFFER_COUNT 6
#define DECODEC_FRAME_BUFFER_RESERVE_COUNT 3
#define CIRCUAR_QUEUE_COUNT (DECODEC_FRAME_BUFFER_COUNT - DECODEC_FRAME_BUFFER_RESERVE_COUNT)

#define DISPLAY_DELAY_US (30 * 1000) // 30ms
typedef struct
{
    media_codec_id_t codec_type;
    int32_t width;
    int32_t height;
    media_codec_context_t decode_context;
} DecodeParams;

// 全局状态结构体
typedef struct
{
    // for file
    FILE *output_fp;
    char input_file[MAX_LINE_LENGTH];
    char output_file[MAX_LINE_LENGTH];

    // for seek
    atomic_bool seek_requested;
    int64_t seek_position; // 以秒为单位
    pthread_mutex_t seek_mutex;

    // for ffmpeg
    double fps;
    AVBSFContext *bsf_ctx;
    AVFormatContext *format_ctx;
    int video_stream_index;
    AVCodecParameters *codec_params;
    bool discard_until_keyframe;
    bool reset_output_after_seek;

    // for thread
    atomic_bool running;
    pthread_t parse_thread;
    pthread_t player_thread;
    pthread_t display_thread;

    // for debug
    int64_t frame_period_us;
    int64_t last_report_sec;
    int64_t last_pkt_ts;

    // for vpu
    DecodeParams decoder_param;

    //for circular queue
    CircularQueue* frame_queue;
} Mp4Player;

int enable_debug_info = 0;
int enable_h264_file_save = 1;
int enable_yuv_file_save = 0;
vp_drm_context_t g_vp_drm_context;
static Mp4Player g_mp4_player = {0};

// 信号处理函数
static void signal_handler(int sig)
{
    printf("%s Received signal %d, shutting down...\n", TAG, sig);
    g_mp4_player.running = false;
}

// MP4解析线程函数
static void *parser_thread_func(void *arg)
{
    printf("%s Parser thread start.\n", TAG);
    media_codec_buffer_t input_buffer = {0};


    if(enable_h264_file_save){
        if (g_mp4_player.codec_params->codec_id == AV_CODEC_ID_H264){
            strncpy(g_mp4_player.output_file, "output.h264", MAX_LINE_LENGTH - 1);  
        }
        else if (g_mp4_player.codec_params->codec_id == AV_CODEC_ID_HEVC){
            strncpy(g_mp4_player.output_file, "output.h265", MAX_LINE_LENGTH - 1);   
        }

        printf("%s Save raw h264 file to %s\n", TAG, g_mp4_player.output_file);
        g_mp4_player.output_fp = fopen(g_mp4_player.output_file, "wb");
        if (!g_mp4_player.output_fp)
        {
            printf("%s Failed to open output file: %s\n", TAG, g_mp4_player.output_file);
            return NULL;
        }
        // 写入头信息（SPS/PPS 或 VPS/SPS/PPS）确保输出文件可被VLC播放
        if (g_mp4_player.codec_params->codec_id == AV_CODEC_ID_H264){
            write_h264_header(g_mp4_player.codec_params, g_mp4_player.output_fp);
        }
        else if (g_mp4_player.codec_params->codec_id == AV_CODEC_ID_HEVC){
            write_h265_header(g_mp4_player.codec_params, g_mp4_player.output_fp);
        }
    }else{
        printf("dont %s Save  h264 file to %s\n", TAG, g_mp4_player.output_file);
        g_mp4_player.output_fp = NULL;
    }

    AVPacket *packet = av_packet_alloc();
    if (!packet)
    {
        printf("%s Failed to allocate packet\n", TAG);
        return NULL;
    }

    int64_t last_time_us = av_gettime();
    while (g_mp4_player.running)
    {
        // 检查是否有seek请求
        pthread_mutex_lock(&g_mp4_player.seek_mutex);
        if (g_mp4_player.seek_requested)
        {
            int64_t seek_pos = g_mp4_player.seek_position;
            printf("%s Seeking to position: %ld seconds\n", TAG, seek_pos);

            // 执行seek操作：seek_pos 为秒，需以 {1,1} 为基准转换到流的 time_base
            AVRational sec_q = (AVRational){1, 1};
            int64_t seek_ts = av_rescale_q(seek_pos, sec_q,
                                           g_mp4_player.format_ctx->streams[g_mp4_player.video_stream_index]->time_base);

            // AVSEEK_FLAG_BACKWARD： “不晚于目标时间”的最近关键帧
            int ret = av_seek_frame(g_mp4_player.format_ctx, g_mp4_player.video_stream_index,
                                    seek_ts, AVSEEK_FLAG_BACKWARD);
            if (ret < 0)
            {
                printf("%s Seek failed: %s\n", TAG, av_err2str(ret));
            }
            else
            {
                printf("%s Seek successful to %ld seconds.\n", TAG, seek_pos);
                // 刷新bitstream filter缓冲
                if (g_mp4_player.bsf_ctx)
                {
                    av_bsf_flush(g_mp4_player.bsf_ctx);
                }
                // 触发关键帧对齐与重写输出文件
                g_mp4_player.discard_until_keyframe = true;
                g_mp4_player.reset_output_after_seek = true;
                g_mp4_player.last_pkt_ts = AV_NOPTS_VALUE; // 重置时间戳，避免seek后的错误延迟
                g_mp4_player.last_report_sec = seek_pos;
            }

            // 重置seek标志
            g_mp4_player.seek_requested = false;
        }
        pthread_mutex_unlock(&g_mp4_player.seek_mutex);

        // 读取数据包
        int ret = av_read_frame(g_mp4_player.format_ctx, packet);
        if (ret < 0)
        {
            if (ret == AVERROR_EOF)
            {
                printf("%s End of file reached\n", TAG);
                g_mp4_player.running = false;
                break;
            }
            else
            {
                printf("%s Error reading frame: %s\n", TAG, av_err2str(ret));
                usleep(10000); // 10ms
                continue;
            }
        }

        // 只处理视频流
        if (packet->stream_index == g_mp4_player.video_stream_index)
        {
            // 通过bitstream filter转换为Annex B并写出，并实时打印播放时间（秒）
            if (packet->size > 0)
            {
                // 打印当前包时间（优先 PTS）
                int64_t pkt_ts = (packet->pts != AV_NOPTS_VALUE) ? packet->pts : packet->dts;
                if (pkt_ts != AV_NOPTS_VALUE)
                {
                    AVRational tb = g_mp4_player.format_ctx->streams[g_mp4_player.video_stream_index]->time_base;
                    int64_t cur_sec = av_rescale_q(pkt_ts, tb, (AVRational){1, 1});
                    if (cur_sec > g_mp4_player.last_report_sec)
                    {
                        g_mp4_player.last_report_sec = cur_sec;
                        printf("%s playback progress: %ld/%ld s\n", TAG, cur_sec, g_mp4_player.format_ctx->duration / AV_TIME_BASE);
                    }
                }
                // 如果刚刚seek，需要丢弃到下一个关键帧，同时重置输出文件并写入头
                if (g_mp4_player.reset_output_after_seek)
                {
                    // 仅在到达关键帧时重置输出
                    if (packet->flags & AV_PKT_FLAG_KEY)
                    {
                        if(enable_h264_file_save){
                            // 关闭并重新打开输出文件（清空文件）
                            if (g_mp4_player.output_fp){
                                fclose(g_mp4_player.output_fp);
                            }
                            printf("%s reopen file %s\n", TAG, g_mp4_player.output_file);
                            g_mp4_player.output_fp = fopen(g_mp4_player.output_file, "wb");
                            if (!g_mp4_player.output_fp)
                            {
                                printf("%s Failed to reopen output file: %s\n", TAG, g_mp4_player.output_file);
                                av_packet_unref(packet);
                                continue;
                            }
                            // 写入头信息
                            if (g_mp4_player.codec_params->codec_id == AV_CODEC_ID_H264)
                            {
                                write_h264_header(g_mp4_player.codec_params, g_mp4_player.output_fp);
                            }
                            else if (g_mp4_player.codec_params->codec_id == AV_CODEC_ID_HEVC)
                            {
                                write_h265_header(g_mp4_player.codec_params, g_mp4_player.output_fp);
                            }
                        }

                        g_mp4_player.reset_output_after_seek = false;
                    }
                    else
                    {
                        // 非关键帧直接丢弃
                        av_packet_unref(packet);
                        continue;
                    }
                }
                if (g_mp4_player.discard_until_keyframe)
                {
                    if (!(packet->flags & AV_PKT_FLAG_KEY))
                    {
                        // 丢弃直到关键帧
                        av_packet_unref(packet);
                        printf("%s Discarding non-keyframe packet\n", TAG);
                        continue;
                    }
                    else
                    {
                        // 碰到关键帧，停止丢弃
                        g_mp4_player.discard_until_keyframe = false;
                    }
                }
                int ret = av_bsf_send_packet(g_mp4_player.bsf_ctx, packet);
                if (ret < 0)
                {
                    printf("%s av_bsf_send_packet error: %s\n", TAG, av_err2str(ret));
                }
                while (ret >= 0)
                {
                    AVPacket *filtered = av_packet_alloc();
                    if (!filtered)
                    {
                        printf("%s av_packet_alloc failed\n", TAG);
                        break;
                    }
                    ret = av_bsf_receive_packet(g_mp4_player.bsf_ctx, filtered);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    {
                        av_packet_free(&filtered);
                        break;
                    }
                    else if (ret < 0)
                    {
                        printf("%s av_bsf_receive_packet error: %s\n", TAG, av_err2str(ret));
                        av_packet_free(&filtered);
                        break;
                    }
                    if ((filtered->size > 0) &&(enable_h264_file_save))
                    {
                        fwrite(filtered->data, 1, filtered->size, g_mp4_player.output_fp);
                        fflush(g_mp4_player.output_fp);
                    }
                    if (filtered->size > 0)
                    {
                        vp_codec_set_input(&g_mp4_player.decoder_param.decode_context,
                                           &input_buffer,
                                           filtered->data,
                                           filtered->size,
                                           0);
                    }

                    av_packet_free(&filtered);
                }
            }
            // 控制帧率
            int64_t current_time_us = av_gettime();
            int64_t elapsed_us = current_time_us - last_time_us;
            int64_t sleep_us = g_mp4_player.frame_period_us - elapsed_us;
            if (sleep_us > 0)
            {
                usleep(sleep_us);
                current_time_us = av_gettime();
            }
            last_time_us = current_time_us;
            av_packet_unref(packet);
        }
    }
    if (g_mp4_player.output_fp)
    {
        fclose(g_mp4_player.output_fp);
        g_mp4_player.output_fp = NULL;
    }
    av_packet_free(&packet);
    printf("%s Parse thread finished\n", TAG);
    return NULL;
}
uint64_t get_timestamp_ms()
{
	uint64_t timestamp;
	struct timeval ts;

	gettimeofday(&ts, NULL);
	timestamp = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_usec / 1000;
	return timestamp;
}

void old_data_handle_cb(void* old_data, void* user_handle){
    media_codec_context_t *context = (media_codec_context_t *)user_handle;
    media_codec_buffer_t* buffer = (media_codec_buffer_t*)old_data;

    // printf("%s old_data_handle_cb release buffer %p\n", TAG, buffer);
    vp_codec_release_output(context, buffer);
}
// 播放器线程函数
void *player_thread_func(void *arg)
{

    printf("%s Player thread start.\n", TAG);

    int32_t ret = 0;
    media_codec_buffer_t ouput_buffer = {0};
    media_codec_output_buffer_info_t info;
    DecodeParams *p_decode_params = &g_mp4_player.decoder_param;
    media_codec_context_t *context = &p_decode_params->decode_context;

    FILE *fp_output = NULL;

    if (enable_yuv_file_save)
    {
        fp_output = fopen("decoder_output.yuv", "w+b");
        if (NULL == fp_output)
        {
            printf("Failed to open output file: %s\n", "decoder_output.yuv");
            return NULL;
        }
    }       
    while (g_mp4_player.running)
    {
        // 1. get output frame from decoder
        ret = vp_codec_get_output(context, &ouput_buffer, &info, 2000);
        if (ret != 0)
        {
            if ((ret == -2) && (!g_mp4_player.running))
            { // timeout and endof : don't printf error log.
                break;
            }
            printf("decode output failed, so return\n");
            continue;
        }

        // 3. for debug: write yuv to file
        if (fp_output)
        {
            if (enable_debug_info)
            {
                static int frame_count = 0;
                frame_count++;
                printf("[%s][%d] frame_count:%d, size: %d, exp size: %d stride: %d vstride: %d\n",
                       __func__, __LINE__,
                       frame_count, ouput_buffer.vframe_buf.size,
                       ouput_buffer.vframe_buf.width * ouput_buffer.vframe_buf.height * 3 / 2,
                       ouput_buffer.vframe_buf.stride, ouput_buffer.vframe_buf.vstride);
                printf("ouput_buffer.vframe_buf.vir_ptr[0]: %p ouput_buffer.vframe_buf.vir_ptr[1]: %p\n",
                       ouput_buffer.vframe_buf.vir_ptr[0], ouput_buffer.vframe_buf.vir_ptr[1]);
            }
            fwrite(
                ouput_buffer.vframe_buf.vir_ptr[0],
                ouput_buffer.vframe_buf.width * ouput_buffer.vframe_buf.height,
                1,
                fp_output);
            fwrite(
                ouput_buffer.vframe_buf.vir_ptr[1],
                ouput_buffer.vframe_buf.width * ouput_buffer.vframe_buf.height / 2,
                1,
                fp_output);
            fflush(fp_output);
        }
        bool enqueue_success = cq_enqueue(g_mp4_player.frame_queue, &ouput_buffer, 
            old_data_handle_cb, context);
        if (!enqueue_success) {
            printf("%s ERR: Frame queue full, dropping frame\n", TAG);
            vp_codec_release_output(context, &ouput_buffer);
        }
    }
    printf("%s Player thread finished\n", TAG);

    // for debug
    if (fp_output != NULL)
    {
        fclose(fp_output);
    }

    pthread_exit(NULL);
}
void get_queue_cb(const void* data, void* user_arg){
    media_codec_buffer_t* frame_buffer = (media_codec_buffer_t*)data;
    media_codec_buffer_t* user_buffer = (media_codec_buffer_t*)user_arg;
    memcpy(user_buffer, frame_buffer, sizeof(media_codec_buffer_t));
}
void *display_thread_func(void *arg)
{
    int ret = 0;
    printf("%s Display thread start.\n", TAG);

    while (g_mp4_player.running)
    {
        int64_t display_target_timestamp_us =  get_current_timestamp_us() - DISPLAY_DELAY_US;
        media_codec_buffer_t frame_buffer = {0};
        bool got_frame = cq_get_by_timestamp(g_mp4_player.frame_queue,
            (DataCallback)get_queue_cb, &frame_buffer,
            display_target_timestamp_us ,
            2000);
        if (!got_frame) {
            printf("%s Warning: No frame available for display (timeout)\n", TAG);
            continue;
        }

        hb_mem_graphic_buf_t image_tmp;
        image_tmp.width = frame_buffer.vframe_buf.width;
        image_tmp.height = frame_buffer.vframe_buf.height;
        image_tmp.stride = frame_buffer.vframe_buf.stride;
        image_tmp.vstride = frame_buffer.vframe_buf.vstride;
        for (int i = 0; i < 3; i++)
        {
            image_tmp.fd[i] = frame_buffer.vframe_buf.fd[i];
        }

        // 2. display on HDMI
        ret = vp_display_set_frame(&g_vp_drm_context, &image_tmp);
        if (ret != 0)
        {
            printf("vp_display_set_frame for hdmi failed %d.\n", ret);
        }
    }
    printf("%s Display thread finished\n", TAG);
    return NULL;
}

// 清理资源
static void cleanup_resources(void)
{
    if (g_mp4_player.bsf_ctx)
    {
        av_bsf_free(&g_mp4_player.bsf_ctx);
        g_mp4_player.bsf_ctx = NULL;
    }

    if (g_mp4_player.format_ctx)
    {
        avformat_close_input(&g_mp4_player.format_ctx);
    }

    pthread_mutex_destroy(&g_mp4_player.seek_mutex);
}
static char* fgets_timeout(char* buf, int size, FILE* stream, int timeout_sec) {
    if (buf == NULL || size <= 0 || stream == NULL || timeout_sec < 0) {
        errno = EINVAL; // 参数无效
        return NULL;
    }

    // 1. 获取流对应的文件描述符
    int fd = fileno(stream);
    if (fd < 0) {
        // 流无效（如关闭的流），errno 由 fileno 设置
        return NULL;
    }

    // 2. 初始化 select 的文件描述符集合
    fd_set read_fds;
    FD_ZERO(&read_fds);       // 清空集合
    FD_SET(fd, &read_fds);    // 将目标文件描述符加入集合

    // 3. 设置超时时间（tv_sec：秒，tv_usec：微秒）
    struct timeval timeout;
    timeout.tv_sec = timeout_sec;
    timeout.tv_usec = 0;      // 微秒部分设为 0，仅精确到秒（可按需调整）

    // 4. 调用 select 监控“可读”状态
    int ret = select(fd + 1, &read_fds, NULL, NULL, &timeout);
    if (ret < 0) {
        // 发生错误（如被信号中断），errno 由 select 设置
        return NULL;
    } else if (ret == 0) {
        // 超时：select 未检测到可读状态，设置 errno 为 ETIMEDOUT
        errno = ETIMEDOUT;
        return NULL;
    }

    // 5. 检测到可读，调用 fgets 读取数据（此时不会阻塞）
    return fgets(buf, size, stream);
}

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        printf("Usage: %s <input_mp4_file> \n", argv[0]);
        printf("Example: %s input.mp4\n", argv[0]);
        return -1;
    }
    // 设置信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    int ret = hb_mem_module_open();
    if (ret != 0)
    {
        printf("hb_mem_module_open failed %d.\n", ret);
        return -1;
    }

    // 初始化全局状态
    strncpy(g_mp4_player.input_file, argv[1], MAX_LINE_LENGTH - 1);

    g_mp4_player.running = true;
    g_mp4_player.seek_position = 0;
    g_mp4_player.seek_requested = false;
    g_mp4_player.discard_until_keyframe = false;
    g_mp4_player.reset_output_after_seek = false;
    g_mp4_player.last_report_sec = -1;
    g_mp4_player.last_pkt_ts = AV_NOPTS_VALUE;
    pthread_mutex_init(&g_mp4_player.seek_mutex, NULL);

    // ffmpeg操作: 打开输入文件
    if (avformat_open_input(&g_mp4_player.format_ctx, g_mp4_player.input_file, NULL, NULL) < 0)
    {
        printf("%s Failed to open input file: %s, maybe file is not a mp4 format.\n", TAG, g_mp4_player.input_file);
        hb_mem_module_close();
        return -1;
    }

    // ffmpeg操作: 获取流信息
    if (avformat_find_stream_info(g_mp4_player.format_ctx, NULL) < 0)
    {
        printf("%s Failed to find stream info\n", TAG);
        cleanup_resources();
        return -1;
    }
    // ffmpeg操作: 判断容器格式是否是 mp4
    if (strcmp(g_mp4_player.format_ctx->iformat->name, "mov,mp4,m4a,3gp,3g2,mj2") != 0 &&
        strcmp(g_mp4_player.format_ctx->iformat->name, "mp4") != 0)
    {
        printf("%s Input file is not an MP4 file: %s (detected: %s)\n",
               TAG, g_mp4_player.input_file, g_mp4_player.format_ctx->iformat->name);
        cleanup_resources();
        return -1;
    }
    // ffmpeg操作:查找视频流
    g_mp4_player.video_stream_index = -1;
    for (int i = 0; i < g_mp4_player.format_ctx->nb_streams; i++)
    {
        if (g_mp4_player.format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            g_mp4_player.video_stream_index = i;
            g_mp4_player.codec_params = g_mp4_player.format_ctx->streams[i]->codecpar;
            break;
        }
    }

    if (g_mp4_player.video_stream_index == -1)
    {
        printf("%s No video stream found\n", TAG);
        cleanup_resources();
        return -1;
    }

    // 检查编解码器类型
    if (g_mp4_player.codec_params->codec_id != AV_CODEC_ID_H264 &&
        g_mp4_player.codec_params->codec_id != AV_CODEC_ID_HEVC)
    {
        printf("%s Unsupported codec: %s\n", TAG,
               avcodec_get_name(g_mp4_player.codec_params->codec_id));
        cleanup_resources();
        return -1;
    }

    // 打印视频信息
    int total_frames = g_mp4_player.format_ctx->streams[g_mp4_player.video_stream_index]->nb_frames;
    int duration_seconds = g_mp4_player.format_ctx->duration / AV_TIME_BASE;
    printf("%sPrintf video info:\n", TAG);
    printf("%s Input file: %s\n", TAG, g_mp4_player.input_file);
    printf("%s Output file: %s\n", TAG, g_mp4_player.output_file);
    printf("%s Codec: %s\n", TAG, avcodec_get_name(g_mp4_player.codec_params->codec_id));
    printf("%s Duration: %d seconds\n", TAG, duration_seconds);
    printf("%s Total frames: %d\n", TAG, total_frames);
    AVRational fr = g_mp4_player.format_ctx->streams[g_mp4_player.video_stream_index]->avg_frame_rate;
    if (fr.num && fr.den)
    {
        g_mp4_player.fps = av_q2d(fr);
        printf("%s Video FPS: %.2f or TotalFrames/Duration: %.2f\n",
               TAG, g_mp4_player.fps, total_frames / (float)duration_seconds);
    }
    else
    {
        g_mp4_player.fps = 30;
        printf("%s Frame rate not available, so usr default FPS: %.2f\n",
               TAG, g_mp4_player.fps);
    }
    g_mp4_player.frame_period_us = (int)(1000000 / g_mp4_player.fps);
    g_mp4_player.decoder_param.width = g_mp4_player.codec_params->width;
    g_mp4_player.decoder_param.height = g_mp4_player.codec_params->height;
    if (g_mp4_player.codec_params->codec_id == AV_CODEC_ID_H264)
    {
        printf("%s Codec: H264\n", TAG);
        g_mp4_player.decoder_param.codec_type = MEDIA_CODEC_ID_H264;
    }
    else if (g_mp4_player.codec_params->codec_id == AV_CODEC_ID_HEVC)
    {
        printf("%s Codec: H264\n", TAG);
        g_mp4_player.decoder_param.codec_type = MEDIA_CODEC_ID_H265;
    }
    else
    {
        printf("%s Codec: %d is not support.\n", TAG, g_mp4_player.codec_params->codec_id);
        cleanup_resources();
        return -1;
    }
    printf("%s Resolution: %dx%d\n", TAG,
           g_mp4_player.decoder_param.width, g_mp4_player.decoder_param.height);
    printf("\n\n");

    g_mp4_player.frame_queue = cq_init(CIRCUAR_QUEUE_COUNT, sizeof(media_codec_buffer_t));
    if (g_mp4_player.frame_queue == NULL)
    {
        printf("%s create frame queue failed\n", TAG);
        cleanup_resources();
        return -1;
    }

    // 初始化bitstream filter
    const AVBitStreamFilter *filter = NULL;
    if (g_mp4_player.codec_params->codec_id == AV_CODEC_ID_H264)
    {
        filter = av_bsf_get_by_name("h264_mp4toannexb");
    }
    else
    {
        filter = av_bsf_get_by_name("hevc_mp4toannexb");
    }
    if (!filter)
    {
        printf("%s Failed to get bitstream filter\n", TAG);
        cleanup_resources();
        return -1;
    }
    if (av_bsf_alloc(filter, &g_mp4_player.bsf_ctx) != 0)
    {
        printf("%s av_bsf_alloc failed\n", TAG);
        cleanup_resources();
        return -1;
    }
    if (avcodec_parameters_copy(g_mp4_player.bsf_ctx->par_in, g_mp4_player.codec_params) != 0)
    {
        printf("%s avcodec_parameters_copy failed\n", TAG);
        cleanup_resources();
        return -1;
    }
    g_mp4_player.bsf_ctx->time_base_in = g_mp4_player.format_ctx->streams[g_mp4_player.video_stream_index]->time_base;
    if (av_bsf_init(g_mp4_player.bsf_ctx) != 0)
    {
        printf("%s av_bsf_init failed\n", TAG);
        cleanup_resources();
        return -1;
    }

    ret = vp_display_check_hdmi_is_connected();
    if (ret == -1)
    {
        printf("\n\nFailed: not found hdmi connector.\n\n");
        return -1;
    }
    else if (ret == -2)
    {
        printf("display driver is not loaded, please execute the following command:\n\n");
        printf("\tmodprobe panel-jc-050hd134\n");
        printf("\tmodprobe galcore\n");
        printf("\tmodprobe vio_n2d\n");
        printf("\tmodprobe lontium_lt8618\n");
        printf("\tmodprobe vs-x5-syscon-bridge\n");
        printf("\tmodprobe vs_drm\n");
        printf("\n\n");
        return -1;
    }
    else
    {
        printf("%s hdmi is connected.\n", TAG);
    }
    DecodeParams *p_decode_params = &g_mp4_player.decoder_param;
    ret = vp_display_check_resolution(p_decode_params->width, p_decode_params->height);
    if (ret != 0)
    {
        printf("\n\nFailed: hdmi not support resolution:%d*%d .\n\n",
               p_decode_params->width, p_decode_params->height);
        return -1;
    }

    ret = vp_display_init(&g_vp_drm_context, p_decode_params->width, p_decode_params->height,
                          ALIGN_32(p_decode_params->width), ALIGN_32(p_decode_params->height));
    if (ret != 0)
    {
        printf("hdmi init failed.\n");
        return -1;
    }

    ret = vp_decode_config_param(&p_decode_params->decode_context,
                                 p_decode_params->codec_type,
                                 p_decode_params->width,
                                 p_decode_params->height,
                                DECODEC_FRAME_BUFFER_COUNT);
    if (ret != 0)
    {
        printf("Decode config param error, type:%d width:%d height:%d\n",
               p_decode_params->codec_type,
               p_decode_params->width,
               p_decode_params->height);
    }

    ret = vp_codec_init(&p_decode_params->decode_context);
    if (ret != 0)
    {
        printf("Decode vp_codec_init error(%d)\n", ret);
        return -1;
    }
    printf("%s Init video decode instance %d successful\n",
           TAG, p_decode_params->decode_context.instance_index);
    ret = vp_codec_start(&p_decode_params->decode_context);
    if (ret != 0)
    {
        printf("Encode vp_codec_start error\n");
        return -1;
    }

    // 创建解析线程
    if (pthread_create(&g_mp4_player.parse_thread, NULL, parser_thread_func, NULL) != 0)
    {
        printf("%s Failed to create parse thread\n", TAG);
        cleanup_resources();
        return -1;
    }
    if (pthread_create(&g_mp4_player.player_thread, NULL, player_thread_func, NULL) != 0)
    {
        printf("%s Failed to create player thread\n", TAG);
        cleanup_resources();
        return -1;
    }
    if(pthread_create(&g_mp4_player.display_thread, NULL, display_thread_func, NULL) != 0)
    {
        printf("%s Failed to create display thread\n", TAG);
        cleanup_resources();
        return -1;
    }

    printf("\n\n%s MP4 decoder started. Enter seek position in seconds (or 'q' to quit):\n", TAG);
    printf("%s Example: 30 (seek to 30 seconds)", TAG);

    // 主线程：处理用户输入
    char input[MAX_LINE_LENGTH];
    printf("%s Seek to (seconds): \n\n", TAG);
    while (g_mp4_player.running)
    {
        fflush(stdout);
        if (fgets_timeout(input, MAX_LINE_LENGTH, stdin, 1) == NULL){
            continue;
        }
        // 移除换行符
        input[strcspn(input, "\n")] = 0;

        // 如果直接回车（空输入），继续下一轮
        if (strlen(input) == 0)
        {
            printf("%s Empty input, please enter a number or 'q' to quit.\n", TAG);
            continue;
        }

        if (strcmp(input, "q") == 0 || strcmp(input, "quit") == 0)
        {
            printf("%s Quitting...\n", TAG);
            break;
        }

        // 解析seek位置
        char *endptr;
        long seek_pos = strtol(input, &endptr, 10);
        if (*endptr != '\0' || seek_pos < 0)
        {
            printf("%s Invalid input. Please enter a positive number or 'q' to quit.\n", TAG);
            continue;
        }

        // 检查seek位置是否超出文件长度
        int64_t duration = g_mp4_player.format_ctx->duration / AV_TIME_BASE;
        if (seek_pos >= duration)
        {
            printf("%s Seek position %ld exceeds file duration %ld seconds\n", TAG, seek_pos, duration);
            continue;
        }

        // 发送seek请求
        pthread_mutex_lock(&g_mp4_player.seek_mutex);
        g_mp4_player.seek_position = seek_pos;
        g_mp4_player.seek_requested = true;
        pthread_mutex_unlock(&g_mp4_player.seek_mutex);

        printf("%s Seek request sent to position %ld seconds\n", TAG, seek_pos);
        printf("%s Seek to (seconds): \n\n", TAG);
    }

    // 等待解析线程结束
    g_mp4_player.running = false;
    pthread_join(g_mp4_player.parse_thread, NULL);
    pthread_join(g_mp4_player.player_thread, NULL);

    vp_display_deinit(&g_vp_drm_context);
    printf("%s MP4 decoder stopped\n", TAG);
    cleanup_resources();

    return 0;
}
