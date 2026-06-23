/***************************************************************************
 *                      COPYRIGHT NOTICE
 *             Copyright(C) 2024, D-Robotics Co., Ltd.
 *                     All rights reserved.
 ***************************************************************************/

#include <fcntl.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "common_utils.h"

#define VSE_MAX_CHANNELS 6
#define VSE_BUF_NUM_MIN 1
#define VSE_BUF_NUM_MAX 16
#define VSE_ASYNC_BUF_NUM_MIN 3

typedef struct scaler_info {
	hbn_vflow_handle_t vflow_fd;
	hbn_vnode_handle_t vnode_fd;
	uint32_t input_width;
	uint32_t input_height;
	char *yuv_file;
	int vse_vnode_mode;
	int32_t send_async;
	int32_t buf_num;
	int32_t run_frames;
	int32_t save_yuv;
} scaler_info_s;

static int verbose_flag = 0;

static struct option const long_options[] = {{"input_file", required_argument, NULL, 'i'},
					     {"input_width", required_argument, NULL, 'w'},
					     {"input_height", required_argument, NULL, 'h'},
					     {"feedback", no_argument, NULL, 'f'},
					     {"verbose", no_argument, NULL, 'V'},
					     {"async", no_argument, NULL, 'a'},
					     {"buf-num", required_argument, NULL, 'b'},
					     {"frames", required_argument, NULL, 'n'},
					     {"save-yuv", no_argument, NULL, 'y'},
					     {NULL, 0, NULL, 0}};

int read_nv12_inputs(scaler_info_s *scaler_info, hbn_vnode_image_t *inputs, int input_buffer_count);
int run_vse(scaler_info_s *scaler_info, hbn_vnode_image_t *input_images, int input_buf_num);
int create_start_vse_vnode(scaler_info_s *scaler_info);
int stop_destroy_vse_vnode(scaler_info_s *scaler_info);

static void print_help(void)
{
	printf("Usage: sample_vse [OPTIONS]\n");
	printf("Options:\n");
	printf("  -i, --input_file FILE   Input NV12 file path\n");
	printf("  -w, --input_width W     Input width in pixels\n");
	printf("  -h, --input_height H    Input height in pixels\n");
	printf("  -f, --feedback          Feedback mode (hbn_vnode_start)\n");
	printf("  -V, --verbose           Verbose dump of frame structures\n");
	printf("  -a, --async             Use hbn_vnode_sendframe_async (default: sync sendframe)\n");
	printf("  -b, --buf-num N         Ichn buffer index rotation count [%d..%d], default %d\n", VSE_BUF_NUM_MIN,
	       VSE_BUF_NUM_MAX, VSE_BUF_NUM_MIN);
	printf("  -n, --frames N          Send/Get frame loop iterations (default 1)\n");
	printf("  -y, --save-yuv          Save ochn NV12 to ./vse_output_nv12_run*_chn*.yuv\n");
}

int main(int argc, char **argv)
{
	int ret = 0;
	int opt_index = 0;
	int c = 0;
	int input_buffer_index;
	scaler_info_s scaler_info = {0};
	hbn_vnode_image_t input_images[VSE_BUF_NUM_MAX];

	scaler_info.send_async = 0;
	scaler_info.buf_num = VSE_BUF_NUM_MIN;
	scaler_info.run_frames = 1;
	scaler_info.save_yuv = 0;

	memset(input_images, 0, sizeof(input_images));

	// 检查标准输入是否来自终端
	if ((!isatty(fileno(stdin))) || (argc == 1)) {
		print_help();
		return 0;
	}

	while ((c = getopt_long(argc, argv, "i:w:h:Vfab:n:y", long_options, &opt_index)) != -1) {
		switch (c) {
			case 'i':
				scaler_info.yuv_file = optarg;
				break;
			case 'w':
				scaler_info.input_width = (uint32_t)atoi(optarg);
				break;
			case 'h':
				scaler_info.input_height = (uint32_t)atoi(optarg);
				break;
			case 'f':
				scaler_info.vse_vnode_mode = VNODE_WORK_MODE_FEEDBACK;
				break;
			case 'V':
				verbose_flag = 1;
				break;
			case 'a':
				scaler_info.send_async = 1;
				break;
			case 'b':
				scaler_info.buf_num = atoi(optarg);
				break;
			case 'n':
				scaler_info.run_frames = atoi(optarg);
				if (scaler_info.run_frames < 1)
					scaler_info.run_frames = 1;
				break;
			case 'y':
				scaler_info.save_yuv = 1;
				break;
			default:
				print_help();
				return 0;
		}
	}

	if (!scaler_info.yuv_file || scaler_info.input_width == 0 || scaler_info.input_height == 0) {
		fprintf(stderr, "sample_vse: require -i FILE, -w WIDTH, -h HEIGHT\n");
		print_help();
		return -1;
	}

	if (scaler_info.buf_num < VSE_BUF_NUM_MIN || scaler_info.buf_num > VSE_BUF_NUM_MAX) {
		fprintf(stderr, "sample_vse: -b must be in [%d, %d], got %d\n", VSE_BUF_NUM_MIN, VSE_BUF_NUM_MAX,
			scaler_info.buf_num);
		return -1;
	}

	if (scaler_info.send_async && scaler_info.buf_num < VSE_ASYNC_BUF_NUM_MIN) {
		fprintf(stderr, "sample_vse: async mode requires -b >= %d (got %d)\n", VSE_ASYNC_BUF_NUM_MIN,
			(int)scaler_info.buf_num);
		return -1;
	}

	printf("VSE work mode: %s\n", (scaler_info.vse_vnode_mode == VNODE_WORK_MODE_VFLOW) ? "vflow" : "feedback");
	printf("sendframe: %s\n", scaler_info.send_async ? "async" : "sync");
	printf("ichn buffer rotation count (-b): %d\n", scaler_info.buf_num);
	printf("run loop count (-n): %d\n", scaler_info.run_frames);
	printf("save_yuv: %s\n", scaler_info.save_yuv ? "yes (-y)" : "no");
	printf("input: %s, %ux%u\n", scaler_info.yuv_file, scaler_info.input_width, scaler_info.input_height);

	ret = hb_mem_module_open();
	ERR_CON_EQ(ret, 0);
	ret = read_nv12_inputs(&scaler_info, input_images, scaler_info.buf_num);
	ERR_CON_EQ(ret, 0);
	ret = create_start_vse_vnode(&scaler_info);
	ERR_CON_EQ(ret, 0);
	ret = run_vse(&scaler_info, input_images, scaler_info.buf_num);
	ERR_CON_EQ(ret, 0);
	ret = stop_destroy_vse_vnode(&scaler_info);
	ERR_CON_EQ(ret, 0);
	for (input_buffer_index = 0; input_buffer_index < scaler_info.buf_num; input_buffer_index++)
		hb_mem_free_buf(input_images[input_buffer_index].buffer.fd[0]);
	hb_mem_module_close();

	return 0;
}

int read_nv12_inputs(scaler_info_s *scaler_info, hbn_vnode_image_t *inputs, int input_buffer_count)
{
	int i, j;
	int ret = 0;
	int64_t alloc_flags = 0;
	char *input_image_path = scaler_info->yuv_file;

	alloc_flags = HB_MEM_USAGE_MAP_INITIALIZED | HB_MEM_USAGE_PRIV_HEAP_2_RESERVERD | HB_MEM_USAGE_CPU_READ_OFTEN
		      | HB_MEM_USAGE_CPU_WRITE_OFTEN | HB_MEM_USAGE_CACHED | HB_MEM_USAGE_GRAPHIC_CONTIGUOUS_BUF;

	for (i = 0; i < input_buffer_count; i++) {
		memset(&inputs[i], 0, sizeof(hbn_vnode_image_t));
		ret = hb_mem_alloc_graph_buf(scaler_info->input_width, scaler_info->input_height, MEM_PIX_FMT_NV12,
					     alloc_flags, scaler_info->input_width, scaler_info->input_height,
					     &inputs[i].buffer);
		if (ret != 0) {
			printf("%s(%d) hb_mem_alloc_graph_buf failed, ret %d\n", __func__, __LINE__,
			       (int32_t)ret);
			break;
		}
		read_yuvv_nv12_file(input_image_path, (char *)(inputs[i].buffer.virt_addr[0]),
				    (char *)(inputs[i].buffer.virt_addr[1]), inputs[i].buffer.size[0]);
		inputs[i].info.bufferindex = i;
		gettimeofday(&inputs[i].info.tv, NULL);
	}

	if (ret != 0) {
		for (j = 0; j < i; j++) {
			if (inputs[j].buffer.fd[0] > 0)
				hb_mem_free_buf(inputs[j].buffer.fd[0]);
			memset(&inputs[j], 0, sizeof(hbn_vnode_image_t));
		}
		return ret;
	}

	return 0;
}

void vp_vin_print_hbn_frame_info_t(const hbn_frame_info_t *frame_info);
void vp_vin_print_hb_mem_graphic_buf_t(const hb_mem_graphic_buf_t *graphic_buf);

// 打印 hbn_vnode_image_t 结构体的所有字段内容
void vp_vin_print_hbn_vnode_image_t(const hbn_vnode_image_t *frame)
{
	printf("=== Frame Info ===\n");
	vp_vin_print_hbn_frame_info_t(&(frame->info));
	printf("\n=== Graphic Buffer ===\n");
	vp_vin_print_hb_mem_graphic_buf_t(&(frame->buffer));
}

// 打印 hbn_frame_info_t 结构体的所有字段内容
void vp_vin_print_hbn_frame_info_t(const hbn_frame_info_t *frame_info)
{
	printf("Frame ID: %u\n", frame_info->frame_id);
	printf("Timestamps: %lu\n", frame_info->timestamps);
	printf("Systimestamps: %lu\n", frame_info->sys_timestamps);
	printf("tv: %ld.%06ld\n", frame_info->tv.tv_sec, frame_info->tv.tv_usec);
	printf("trig_tv: %ld.%06ld\n", frame_info->trig_tv.tv_sec, frame_info->trig_tv.tv_usec);
	printf("Frame Done: %u\n", frame_info->frame_done);
	printf("Buffer Index: %d\n", frame_info->bufferindex);
}

// 打印 hb_mem_graphic_buf_t 结构体的所有字段内容
void vp_vin_print_hb_mem_graphic_buf_t(const hb_mem_graphic_buf_t *graphic_buf)
{
	int i;

	printf("File Descriptors: ");
	for (i = 0; i < MAX_GRAPHIC_BUF_COMP; i++)
		printf("%d ", graphic_buf->fd[i]);
	printf("\n");

	printf("Plane Count: %d\n", graphic_buf->plane_cnt);
	printf("Format: %d\n", graphic_buf->format);
	printf("Width: %d\n", graphic_buf->width);
	printf("Height: %d\n", graphic_buf->height);
	printf("Stride: %d\n", graphic_buf->stride);
	printf("Vertical Stride: %d\n", graphic_buf->vstride);
	printf("Is Contiguous: %d\n", graphic_buf->is_contig);

	printf("Share IDs: ");
	for (i = 0; i < MAX_GRAPHIC_BUF_COMP; i++)
		printf("%d ", graphic_buf->share_id[i]);
	printf("\n");

	printf("Flags: %ld\n", graphic_buf->flags);

	printf("Sizes: ");
	for (i = 0; i < MAX_GRAPHIC_BUF_COMP; i++)
		printf("%lu ", graphic_buf->size[i]);
	printf("\n");

	printf("Virtual Addresses: ");
	for (i = 0; i < MAX_GRAPHIC_BUF_COMP; i++)
		printf("%p ", graphic_buf->virt_addr[i]);
	printf("\n");

	printf("Physical Addresses: ");
	for (i = 0; i < MAX_GRAPHIC_BUF_COMP; i++)
		printf("%lu ", graphic_buf->phys_addr[i]);
	printf("\n");

	printf("Offsets: ");
	for (i = 0; i < MAX_GRAPHIC_BUF_COMP; i++)
		printf("%lu ", graphic_buf->offset[i]);
	printf("\n");
}

int run_vse(scaler_info_s *scaler_info, hbn_vnode_image_t *input_images, int input_buf_num)
{
	int ret;
	int run_times;
	int buffer_index;
	uint32_t chn_id = 0;
	int timeout = 1000;
	hbn_vnode_image_t *input_img;
	hbn_vnode_image_t output_img = {0};
	char output_image_path[160] = {0};

	if (input_buf_num < VSE_BUF_NUM_MIN || input_buf_num > VSE_BUF_NUM_MAX)
		return -1;

	if (verbose_flag) {
		for (buffer_index = 0; buffer_index < input_buf_num; buffer_index++)
			vp_vin_print_hbn_vnode_image_t(&input_images[buffer_index]);
	}

	for (run_times = 0; run_times < scaler_info->run_frames; run_times++) {
		buffer_index = run_times % input_buf_num;
		input_img = &input_images[buffer_index];
		input_img->info.bufferindex = buffer_index;

		// 发送帧并获取输出图像
		if (scaler_info->send_async)
			ret = hbn_vnode_sendframe_async(scaler_info->vnode_fd, 0, input_img);
		else
			ret = hbn_vnode_sendframe(scaler_info->vnode_fd, 0, input_img);
		ERR_CON_EQ(ret, 0);

		// 循环处理每个输出通道
		for (chn_id = 0; chn_id < VSE_MAX_CHANNELS; chn_id++) {
			ret = hbn_vnode_getframe(scaler_info->vnode_fd, chn_id, timeout, &output_img);
			ERR_CON_EQ(ret, 0);

			if (verbose_flag)
				vp_vin_print_hbn_vnode_image_t(&output_img);

			if (scaler_info->save_yuv) {
				// 根据当前输出通道属性设置输出文件路径
				snprintf(output_image_path, sizeof(output_image_path),
					 "./"
					 "vse_output_nv12_run%d_chn%u_%dx%d_"
					 "stride_%d.yuv",
					 run_times, chn_id, output_img.buffer.width, output_img.buffer.height,
					 output_img.buffer.stride);
				// 保存输出图像到文件
				dump_2plane_yuv_to_file(output_image_path, output_img.buffer.virt_addr[0],
							output_img.buffer.virt_addr[1], output_img.buffer.size[0],
							output_img.buffer.size[1]);
			}

			// 释放输出图像内存
			ret = hbn_vnode_releaseframe(scaler_info->vnode_fd, chn_id, &output_img);
			ERR_CON_EQ(ret, 0);
		}
	}
	printf("sample_vse: completed %d send/get frame iteration(s)\n", scaler_info->run_frames);
	return 0;
}

int create_start_vse_vnode(scaler_info_s *scaler_info)
{
	int i;
	int ret = 0;
	uint32_t hw_id = 0;
	hbn_buf_alloc_attr_t alloc_attr = {0};

	vse_attr_t vse_attr = {0};
	vse_ichn_attr_t vse_ichn_attr = {0};
	vse_ochn_attr_t vse_ochn_attr[VSE_MAX_CHANNELS] = {0};
	uint32_t input_width = scaler_info->input_width;
	uint32_t input_height = scaler_info->input_height;

	printf("ichn input width = %u\n", input_width);
	printf("ichn input height = %u\n", input_height);

	vse_ichn_attr.width = input_width;
	vse_ichn_attr.height = input_height;
	vse_ichn_attr.fmt = FRM_FMT_NV12;
	vse_ichn_attr.bit_width = 8;

	for (i = 0; i < VSE_MAX_CHANNELS; ++i) {
		vse_ochn_attr[i].chn_en = CAM_TRUE;
		vse_ochn_attr[i].roi.x = 0;
		vse_ochn_attr[i].roi.y = 0;
		vse_ochn_attr[i].roi.w = input_width;
		vse_ochn_attr[i].roi.h = input_height;
		vse_ochn_attr[i].fmt = FRM_FMT_NV12;
		vse_ochn_attr[i].bit_width = 8;
	}

	// 输出原分辨率
	vse_ochn_attr[0].target_w = input_width;
	vse_ochn_attr[0].target_h = input_height;

	// 输出 16 像素对齐的常用算法图像使用的分辨率
	vse_ochn_attr[1].target_w = 512;
	vse_ochn_attr[1].target_h = 512;

	// 输出常用算法图像使用的分辨率
	vse_ochn_attr[2].target_w = 224;
	vse_ochn_attr[2].target_h = 224;

	// 设置VSE通道3输出属性，ROI为原图中心点不变，宽、高各裁剪一半，输出图像宽、高等于ROI区域宽高
	vse_ochn_attr[3].roi.x = input_width / 2 - input_width / 4;
	vse_ochn_attr[3].roi.y = input_height / 2 - input_height / 4;
	vse_ochn_attr[3].roi.w = input_width / 2;
	vse_ochn_attr[3].roi.h = input_height / 2;

	// 缩小到支持的最小分辨率
	vse_ochn_attr[3].target_w = 64;
	vse_ochn_attr[3].target_h = 64;

	vse_ochn_attr[4].target_w = 672;
	vse_ochn_attr[4].target_h = 672;

	// 放大到支持的最大分辨率
	vse_ochn_attr[5].target_w = (input_width * 2) > 4096 ? 4096 : (input_width * 2);
	vse_ochn_attr[5].target_h = (input_height * 2) > 3076 ? 3076 : (input_height * 2);

	ret = hbn_vnode_open(HB_VSE, hw_id, AUTO_ALLOC_ID, &scaler_info->vnode_fd);
	ERR_CON_EQ(ret, 0);

	ret = hbn_vnode_set_attr(scaler_info->vnode_fd, &vse_attr);
	ERR_CON_EQ(ret, 0);

	ret = hbn_vnode_set_ichn_attr(scaler_info->vnode_fd, 0, &vse_ichn_attr);
	ERR_CON_EQ(ret, 0);

	alloc_attr.buffers_num = 3;
	alloc_attr.is_contig = 1;
	alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN | HB_MEM_USAGE_CPU_WRITE_OFTEN | HB_MEM_USAGE_CACHED;

	for (i = 0; i < VSE_MAX_CHANNELS; ++i) {
		printf("hbn_vnode_set_ochn_attr: %d, %dx%d\n", i, vse_ochn_attr[i].target_w, vse_ochn_attr[i].target_h);
		ret = hbn_vnode_set_ochn_attr(scaler_info->vnode_fd, i, &vse_ochn_attr[i]);
		ERR_CON_EQ(ret, 0);
		ret = hbn_vnode_set_ochn_buf_attr(scaler_info->vnode_fd, i, &alloc_attr);
		ERR_CON_EQ(ret, 0);
	}

	switch (scaler_info->vse_vnode_mode) {
		case VNODE_WORK_MODE_VFLOW:
			ret = hbn_vflow_create(&scaler_info->vflow_fd);
			ERR_CON_EQ(ret, 0);
			ret = hbn_vflow_add_vnode(scaler_info->vflow_fd, scaler_info->vnode_fd);
			ERR_CON_EQ(ret, 0);
			ret = hbn_vflow_start(scaler_info->vflow_fd);
			ERR_CON_EQ(ret, 0);
			break;
		case VNODE_WORK_MODE_FEEDBACK:
			ret = hbn_vnode_start(scaler_info->vnode_fd);
			ERR_CON_EQ(ret, 0);
			break;
		default:
			printf("Unknown VSE vnode work mode[%d]\n", scaler_info->vse_vnode_mode);
			break;
	}

	return ret;
}

int stop_destroy_vse_vnode(scaler_info_s *scaler_info)
{
	int ret;

	switch (scaler_info->vse_vnode_mode) {
		case VNODE_WORK_MODE_VFLOW:
			ret = hbn_vflow_stop(scaler_info->vflow_fd);
			ERR_CON_EQ(ret, 0);
			hbn_vnode_close(scaler_info->vnode_fd);
			hbn_vflow_destroy(scaler_info->vflow_fd);
			break;
		case VNODE_WORK_MODE_FEEDBACK:
			ret = hbn_vnode_stop(scaler_info->vnode_fd);
			ERR_CON_EQ(ret, 0);
			hbn_vnode_close(scaler_info->vnode_fd);
			break;
		default:
			printf("Unknown VSE vnode work mode[%d]\n", scaler_info->vse_vnode_mode);
			break;
	}
	return 0;
}
