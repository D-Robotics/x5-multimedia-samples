/***************************************************************************
 *                      COPYRIGHT NOTICE
 *             Copyright(C) 2024, D-Robotics Co., Ltd.
 *                     All rights reserved.
 ***************************************************************************/

#include "hb_mem_mgr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <fcntl.h>

#include "common_utils.h"

int32_t dump_image_to_file(char *filename, uint8_t *src_buffer, uint32_t size)
{
	int yuv_fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);

	if (yuv_fd == -1) {
		printf("Error opening file(%s)\n", filename);
		return -1;
	}

	ssize_t bytes_written = write(yuv_fd, src_buffer, size);
	close(yuv_fd);

	if (bytes_written != size) {
		printf("Error writing to file\n");
		return -1;
	}

	printf("Dump image to file(%s), size(%d) succeeded\n", filename, size);
	return 0;
}

int32_t dump_2plane_yuv_to_file(char *filename, uint8_t *src_buffer, uint8_t *src_buffer1,
		uint32_t size, uint32_t size1)
{
	int yuv_fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);

	if (yuv_fd == -1) {
		printf("Error opening file(%s)\n", filename);
		return -1;
	}

	ssize_t bytes_written = write(yuv_fd, src_buffer, size);
	if (bytes_written != size) {
		printf("Error writing y to file\n");
		close(yuv_fd);
		return -1;
	}

	bytes_written = write(yuv_fd, src_buffer1, size1);
	if (bytes_written != size1) {
		printf("Error writing uv to file\n");
		close(yuv_fd);
		return -1;
	}

	close(yuv_fd);

	printf("Dump yuv to file(%s), size(%d) + size1(%d) succeeded\n", filename, size, size1);
	return 0;
}

// hbmem
static pthread_mutex_t ion_lock = PTHREAD_MUTEX_INITIALIZER; /*PRQA S 3004*/
static uint32_t g_ion_opened = 0;
static int32_t m_ionClient = -1;

int32_t vpm_hb_mem_init(void)
{
	uint32_t v_major, V_minor, v_patch;

	pthread_mutex_lock(&ion_lock);
	if (g_ion_opened == 0u) {
		m_ionClient = hb_mem_module_open();
		if (m_ionClient < 0) {
			printf("hb_mem_module_open failed ret %d\n", m_ionClient);
			m_ionClient = -1;
			g_ion_opened = 0;
			pthread_mutex_unlock(&ion_lock);
			return -1;
		}
		hb_mem_get_version(&v_major, &V_minor, &v_patch);
		printf("hb_mem_module_open success version: %u.%u.%u\n", v_major, V_minor, v_patch);
		g_ion_opened = 1;
	} else {
		g_ion_opened++;
		printf("skip hb mem open count %d.\n", g_ion_opened);
	}
	pthread_mutex_unlock(&ion_lock);
	return 0;
}

void vpm_hb_mem_deinit(void)
{
	int32_t ret;

	pthread_mutex_lock(&ion_lock);
	if (g_ion_opened > 0u) {
		g_ion_opened--;
		printf("Try release hb mem  open count %d.\n", g_ion_opened);
	}

	if ((g_ion_opened == 0u) && (m_ionClient == 0)) {
		ret = hb_mem_module_close();
		if (ret < 0) {
			printf("hb_mem_module_close failed ret %d\n", ret);
		} else {
			printf("vpm release hb mem done.\n");
		}
		m_ionClient = -1;
		g_ion_opened = 0;
	}
	pthread_mutex_unlock(&ion_lock);
	return;
}

int32_t alloc_graphic_buffer(hbn_vnode_image_t *img, uint32_t width, uint32_t height, uint32_t cached, int32_t format)
{
	int32_t ret;
	int64_t alloc_flags = 0;
	int32_t stride;
	/* int32_t format = MEM_PIX_FMT_NV12; */

	// vpm_hb_mem_init();

	alloc_flags = HB_MEM_USAGE_MAP_INITIALIZED | HB_MEM_USAGE_PRIV_HEAP_2_RESERVERD | HB_MEM_USAGE_CPU_READ_OFTEN |
		      HB_MEM_USAGE_CPU_WRITE_OFTEN;

	memset(img, 0, sizeof(hbn_vnode_image_t));
	if (cached == 1)
		alloc_flags = alloc_flags | HB_MEM_USAGE_CACHED;

	switch (format) {
		case MEM_PIX_FMT_RAW24:
			stride = width * 3;
			break;
		case MEM_PIX_FMT_RAW16:
			stride = width * 2;
			break;
		case MEM_PIX_FMT_RAW12:
			stride = width * 2;
			break;
		case MEM_PIX_FMT_RAW10:
			stride = width * 2;
			break;
		case MEM_PIX_FMT_RAW8:
			stride = width;
			break;
		default:
			stride = width;
			break;
	}
	ret = hb_mem_alloc_graph_buf(width, height, format, alloc_flags, stride, height, &img->buffer);
	if (ret < 0) {
		printf("hb_mem_alloc_graph_buf ret %d failed \n", ret);
		return ret;
	}

	return ret;
}

int32_t read_yuvv_nv12_file(const char *filename, char *addr0, char *addr1, uint32_t y_size)
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

	buffer = (char *)malloc(y_size + y_size / 2);

	if (fread(buffer, 1, y_size, Fd) != y_size) {
		printf("read bin(%s) to addr fail #1\n", filename);
		return -1;
	}

	if (fread(buffer + y_size, 1, y_size / 2, Fd) != y_size / 2) {
		printf("read bin(%s) to addr fail #2\n", filename);
		return -1;
	}

	memcpy(addr0, buffer, y_size);
	memcpy(addr1, buffer + y_size, y_size / 2);

	fflush(Fd);

	if (Fd)
		fclose(Fd);
	if (buffer)
		free(buffer);

	printf("(%s):file read(%s), y-size(%d)\n", __func__, filename, y_size);
	return 0;
}