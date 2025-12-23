/***************************************************************************
 *                      COPYRIGHT NOTICE
 *             Copyright(C) 2025, D-Robotics Co., Ltd.
 *                     All rights reserved.
 ***************************************************************************/

#include <stdio.h>
#include <getopt.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <string.h>
#include <pthread.h>

#include "hbn_api.h"
#include "common_utils.h"
#include "bmp.h"

static n2d_config_t g_gpu2d_attr = {0};
static pthread_mutex_t g_gpu2d_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct image_info
{
	char* image_file;
	hb_mem_common_buf_t bin_buf;
} image_info_s;

typedef struct gpu_2d_info
{
	hbn_vflow_handle_t vflow_fd;
	hbn_vnode_handle_t gpu_2d_vnode_fd;
	uint32_t input_width;
	uint32_t input_height;
	uint32_t input_format;
	uint32_t output_width;
	uint32_t output_height;
	uint32_t output_format;
	char* gpu_2d_bin_file;
	char* input_file;
	char* output_file;
	int gpu_2d_vnode_mode;
} gpu_2d_info_s;

static struct option const long_options[] = {
	{NULL, 0, NULL, 0}
};

static void print_help() {
	printf("Usage: %s [OPTIONS]\n", get_program_name());
	printf("Options:\n");
	printf("  -i <input_img_name>       input image name\n");
	printf("  -o <output_img_name>      output image name\n");
	printf("  -w <image_width>          input image width\n");
	printf("  -h <image_height>         input image height\n");
	printf("  -j <input_img_format>     intput image format index\n");
	printf("  -k <output_img_format>    output image format index\n");
	printf("   MEM_PIX_FMT_ARGB    index is 4\n");
	printf("   MEM_PIX_FMT_NV12    index is 8\n");
	printf("   MEM_PIX_FMT_YUYV422 index is 13\n");
	printf("  -h                        Show this help message\n");
	printf("Such as: nv12 -> bmp yuv422:\n \
	./single_format_convert_gpu2d -i ./res/nv12_1920x1080.yuv  -o ./test_nv12_1920x1080.bmp -w 1920 -h 1080 -j 8 -k 3\n\
	./single_format_convert_gpu2d -i ./res/nv12_1920x1080.yuv  -o ./test_nv12_1920x1080.yuv422 -w 1920 -h 1080 -j 8 -k 13\n\
	 yuv422 -> bmp nv12:\n\
	./single_format_convert_gpu2d -i ./res/yuv422_1920x1080.yuv422 -o ./test_yuv422_1920x1080.bmp -w 1920 -h 1080 -j 13 -k 3\n\
	./single_format_convert_gpu2d -i ./res/yuv422_1920x1080.yuv422 -o ./test_yuv422_1920x1080.yuv -w 1920 -h 1080 -j 13 -k 8\n\n");
}

static int create_gpu2d_node(pipe_contex_t *pipe_contex, gpu_2d_info_s *gpu_2d_info)
{
	int ret = 0;
	hbn_vnode_handle_t *gpu2d_node_handle = &pipe_contex->gpu2d_node_handle;
	hbn_buf_alloc_attr_t alloc_attr = {0};
	uint32_t ichn_id = 0;
	uint32_t ochn_id = 0;
	uint32_t hw_id = 0;
	uint32_t input_width  = 0, input_height  = 0;
	uint32_t output_width = 0, output_height = 0;
	uint32_t input_stride = 0, output_stride = 0;

	input_width  = gpu_2d_info->input_width;
	input_height = gpu_2d_info->input_height;
	output_width  = input_width;
	output_height = input_height;

	if (gpu_2d_info->input_format == MEM_PIX_FMT_NV12) {
		printf("input_format is MEM_PIX_FMT_NV12\n");
	} else if (gpu_2d_info->input_format == MEM_PIX_FMT_YUYV422) {
		printf("input_format is MEM_PIX_FMT_YUYV422\n");
	} else {
		printf("input_format unsupport.\n");
		return -1;
	}

	if (gpu_2d_info->output_format == MEM_PIX_FMT_ARGB) {
		printf("output_format is MEM_PIX_FMT_ARGB\n");
	} else if (gpu_2d_info->output_format == MEM_PIX_FMT_NV12) {
		printf("output_format is MEM_PIX_FMT_NV12\n");
	} else if (gpu_2d_info->output_format == MEM_PIX_FMT_YUYV422) {
		printf("output_format is MEM_PIX_FMT_YUYV422\n");
	} else {
		printf("output_format unsupport.\n");
		return -1;
	}

	// ==== Step 1. 检查输入宽度是否满足 64 字节对齐 ====
	if ((input_width % 64) != 0) {
		printf("[ERROR] GPU2D input width (%u) is not 64-byte aligned\n", input_width);
		return -1;
	}

	input_stride  = input_width; // 直接用原宽度
	output_stride = output_width;
	printf("GPU2D input  %ux%u stride=%u\n", input_width, input_height, input_stride);
	printf("GPU2D output %ux%u stride=%u\n", output_width, output_height, output_stride);

	// ==== Step 2. 打开 GPU2D 节点 ====
	ret = hbn_vnode_open(HB_N2D, hw_id, AUTO_ALLOC_ID, gpu2d_node_handle);
	ERR_CON_EQ(ret, 0);
	pthread_mutex_lock(&g_gpu2d_mutex);

	// ==== Step 3. 设置 GPU2D 主属性 ====
	n2d_config_t gpu2d_attr = {0};
	gpu2d_attr.command = N2D_CSC; 	// 操作类型
	gpu2d_attr.ninputs = 1; 	// 输入通道数

	// 输入通道属性
	gpu2d_attr.input_width[0]  = input_width;
	gpu2d_attr.input_height[0] = input_height;
	if (gpu_2d_info->input_format == MEM_PIX_FMT_YUYV422) {
		gpu2d_attr.input_stride[0] = input_stride * 2;
	} else
		gpu2d_attr.input_stride[0] = input_stride;
	gpu2d_attr.input_format = gpu_2d_info->input_format;

	// 输出通道属性
	gpu2d_attr.output_width  = output_width;
	gpu2d_attr.output_height = output_height;
	if (gpu_2d_info->output_format == MEM_PIX_FMT_YUYV422) {
		gpu2d_attr.output_stride = output_stride * 2;
	} else
		gpu2d_attr.output_stride = output_stride;

	gpu2d_attr.output_format = gpu_2d_info->output_format;

	// 默认不裁剪
	gpu2d_attr.crop_x = 0;
	gpu2d_attr.crop_y = 0;
	gpu2d_attr.crop_width = 0;
	gpu2d_attr.crop_height = 0;

	memcpy(&g_gpu2d_attr, &gpu2d_attr, sizeof(n2d_config_t));
	pthread_mutex_unlock(&g_gpu2d_mutex);

	ret = hbn_vnode_set_attr(*gpu2d_node_handle, &gpu2d_attr);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_ichn_attr(*gpu2d_node_handle, ichn_id, &gpu2d_attr);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_ochn_attr(*gpu2d_node_handle, ochn_id, &gpu2d_attr);
	ERR_CON_EQ(ret, 0);

	// ==== Step 4. 设置输出 buffer 属性 ====
	alloc_attr.buffers_num = 3;
	alloc_attr.is_contig = 1;
	alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN |
			   HB_MEM_USAGE_CPU_WRITE_OFTEN |
			   HB_MEM_USAGE_CACHED;
	ret = hbn_vnode_set_ochn_buf_attr(*gpu2d_node_handle, 0, &alloc_attr);
	if (ret < 0) {
		printf("hbn_vnode_set_ochn_buf_attr failed, ret = %d\n", ret);
		return -1;
	}

	return 0;
}

int read_nv12_image(gpu_2d_info_s *gpu_2d_info, hbn_vnode_image_t *input_image) {
	int64_t alloc_flags = 0;
	int ret = 0;

	char *input_image_path = gpu_2d_info->input_file;
	memset(input_image, 0, sizeof(hbn_vnode_image_t));
	alloc_flags = HB_MEM_USAGE_MAP_INITIALIZED |
		      HB_MEM_USAGE_PRIV_HEAP_2_RESERVERD |
		      HB_MEM_USAGE_CPU_READ_OFTEN |
		      HB_MEM_USAGE_CPU_WRITE_OFTEN |
		      HB_MEM_USAGE_CACHED |
		      HB_MEM_USAGE_GRAPHIC_CONTIGUOUS_BUF;

	ret = hb_mem_alloc_graph_buf(gpu_2d_info->input_width,
				     gpu_2d_info->input_height,
				     MEM_PIX_FMT_NV12,
				     alloc_flags,
				     gpu_2d_info->input_width,
				     gpu_2d_info->input_height,
				     &input_image->buffer);
	ERR_CON_EQ(ret, 0);
	printf("buffer.addr:0x%08lx size:%ld\n", (uint64_t)input_image->buffer.virt_addr[0], input_image->buffer.size[0]);
	read_yuvv_nv12_file(input_image_path,
			    (char *)(input_image->buffer.virt_addr[0]),
			    (char *)(input_image->buffer.virt_addr[1]),
			    input_image->buffer.size[0]);
	ret = hb_mem_flush_buf_with_vaddr((uint64_t)input_image->buffer.virt_addr[0], input_image->buffer.size[0]);
	ret |= hb_mem_flush_buf_with_vaddr((uint64_t)input_image->buffer.virt_addr[1], input_image->buffer.size[1]);

	return ret;
}

int32_t read_yuvv_422_file(const char *filename, char *addr0, uint32_t y_size)
{
	if (filename == NULL || addr0 == NULL || y_size == 0) {
		printf("ERR(%s):null param.\n", __func__);
		return -1;
	}

	FILE *Fd = NULL;
	Fd = fopen(filename, "r");
	char *buffer = NULL;

	if (Fd == NULL) {
		printf("ERR(%s):open(%s) fail\n", __func__, filename);
		return -1;
	}

	buffer = (char *)malloc(y_size);

	if (fread(buffer, 1, y_size, Fd) != y_size) {
		printf("read bin(%s) to addr fail #1\n", filename);
		return -1;
	}

	memcpy(addr0, buffer, y_size);

	fflush(Fd);

	if (Fd)
		fclose(Fd);
	if (buffer)
		free(buffer);

	printf("(%s):file read(%s), y-size(%d)\n", __func__, filename, y_size);
	return 0;
}

int read_yuv422_image(gpu_2d_info_s *gpu_2d_info, hbn_vnode_image_t *input_image) {
	int64_t alloc_flags = 0;
	int ret = 0;
	char *input_image_path = gpu_2d_info->input_file;

	memset(input_image, 0, sizeof(hbn_vnode_image_t));
	alloc_flags = HB_MEM_USAGE_MAP_INITIALIZED |
		      HB_MEM_USAGE_PRIV_HEAP_2_RESERVERD |
		      HB_MEM_USAGE_CPU_READ_OFTEN |
		      HB_MEM_USAGE_CPU_WRITE_OFTEN |
		      HB_MEM_USAGE_CACHED |
		      HB_MEM_USAGE_GRAPHIC_CONTIGUOUS_BUF;
	ret = hb_mem_alloc_graph_buf(gpu_2d_info->input_width,
				     gpu_2d_info->input_height,
				     MEM_PIX_FMT_YUYV422,
				     alloc_flags,
				     0,
				     0,
				     &input_image->buffer);
	ERR_CON_EQ(ret, 0);
	printf("buffer.addr:0x%08lx size:%ld\n", (uint64_t)input_image->buffer.virt_addr[0], input_image->buffer.size[0]);
	read_yuvv_422_file(input_image_path,
			   (char *)(input_image->buffer.virt_addr[0]),
			   input_image->buffer.size[0]);

	ret = hb_mem_flush_buf_with_vaddr((uint64_t)input_image->buffer.virt_addr[0], input_image->buffer.size[0]);

	return ret;
}

uint sensor_mode = 0;

int main(int argc, char** argv)
{
	int ret = 0;
	pipe_contex_t image_pipe_contex = {0};
	gpu_2d_info_s gpu_2d_info = {0};
	hbn_vnode_image_t input_image = {0};
	int opt_index = 0;
	int c = 0;

	while((c = getopt_long(argc, argv, "i:o:w:h:j:k:m",
			       long_options, &opt_index)) != -1) {
		switch (c)
		{
		case 'i':
			gpu_2d_info.input_file = optarg;
			break;
		case 'o':
			gpu_2d_info.output_file = optarg;
			break;
		case 'w':
			gpu_2d_info.input_width = atoi(optarg);
			gpu_2d_info.output_width = atoi(optarg);
			break;
		case 'h':
			gpu_2d_info.input_height = atoi(optarg);
			gpu_2d_info.output_height = atoi(optarg);
			break;
		case 'j':
			gpu_2d_info.input_format = atoi(optarg); // 4 8 13
			break;
		case 'k':
			gpu_2d_info.output_format = atoi(optarg); // 4 8 13
			break;
		case 'm':
			sensor_mode = atoi(optarg);
			break;
		default:
			print_help();
			return 0;
		}
	}

	hb_mem_module_open();

	if (gpu_2d_info.input_format == MEM_PIX_FMT_NV12) {
		ret = read_nv12_image(&gpu_2d_info, &input_image);
		ERR_CON_EQ(ret, 0);
	} else if (gpu_2d_info.input_format == MEM_PIX_FMT_YUYV422) {
		ret = read_yuv422_image(&gpu_2d_info, &input_image);
		ERR_CON_EQ(ret, 0);
	} else {
		printf("input_format error\n");
		return -1;
	}

	ret = create_gpu2d_node(&image_pipe_contex, &gpu_2d_info);
	ERR_CON_EQ(ret, 0);

	ret = hbn_vflow_create(&image_pipe_contex.vflow_fd);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_add_vnode(image_pipe_contex.vflow_fd,
		image_pipe_contex.gpu2d_node_handle);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_start(image_pipe_contex.vflow_fd);
	ERR_CON_EQ(ret, 0);

	uint32_t chn_id = 0;
	hbn_vnode_image_t output_img = {0};
	int timeout = 1000;

	ret = hbn_vnode_sendframe(image_pipe_contex.gpu2d_node_handle, chn_id, &input_image);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_getframe(image_pipe_contex.gpu2d_node_handle, chn_id, timeout, &output_img);
	ERR_CON_EQ(ret, 0);

	if (g_gpu2d_attr.output_format == MEM_PIX_FMT_NV12) {
		dump_2plane_yuv_to_file(gpu_2d_info.output_file,
					output_img.buffer.virt_addr[0],
					output_img.buffer.virt_addr[1],
					output_img.buffer.size[0],
					output_img.buffer.size[1]);
	} else if (g_gpu2d_attr.output_format == MEM_PIX_FMT_YUYV422) {
		dump_image_to_file(gpu_2d_info.output_file, output_img.buffer.virt_addr[0], output_img.buffer.size[0]);
	} else if (g_gpu2d_attr.output_format == MEM_PIX_FMT_ARGB) {
		SaveBmpImage(gpu_2d_info.output_file, gpu_2d_info.output_width, gpu_2d_info.output_height, output_img.buffer.virt_addr[0]);
	} else {
		printf("output_format unsupport.\n");
	}

	ret = hbn_vflow_stop(image_pipe_contex.vflow_fd);
	ERR_CON_EQ(ret, 0);
	hbn_vflow_destroy(image_pipe_contex.vflow_fd);
	hb_mem_module_close();

	return 0;
}
