/***************************************************************************
 *                      COPYRIGHT NOTICE
 *             Copyright(C) 2025, D-Robotics Co., Ltd.
 *                     All rights reserved.
 ***************************************************************************/

#include <stdio.h>
#include <stdlib.h>

#include "ping_pang_file_saver.h"

void close_and_delete(FILE **fp, const char *filename)
{
	if (*fp) {
		fclose(*fp);
		*fp = NULL;
	}
	remove(filename);
}

ping_pang_file_saver_t* ping_pang_file_saver_create(char *file_name, int per_file_max_count)
{
	if (!file_name || per_file_max_count <= 0) {
		fprintf(stderr, "Invalid parameters to create file saver\n");
		return NULL;
	}

	ping_pang_file_saver_t *saver = (ping_pang_file_saver_t *)malloc(sizeof(ping_pang_file_saver_t));
	if (!saver) {
		perror("Failed to allocate memory");
		return NULL;
	}

	snprintf(saver->ping_file_name, sizeof(saver->ping_file_name), "ping_%s", file_name);
	snprintf(saver->pang_file_name, sizeof(saver->pang_file_name), "pang_%s", file_name);

	saver->ping_fp = fopen(saver->ping_file_name, "w");
	saver->pang_fp = NULL;

	if (!saver->ping_fp) {
		perror("Failed to open ping file");
		free(saver);
		return NULL;
	}

	saver->per_file_max_count = per_file_max_count;
	saver->already_write_count = 0;
	saver->current_is_ping_file = 1;

	return saver;
}

int ping_pang_file_saver_write(ping_pang_file_saver_t* file_saver, int size, void* data)
{
	if (!file_saver || !data || size <= 0) {
		fprintf(stderr, "Invalid parameters for write\n");
		return -1;
	}

	FILE **current_fp = file_saver->current_is_ping_file ? &file_saver->ping_fp : &file_saver->pang_fp;
	const char *current_file_name = file_saver->current_is_ping_file ? file_saver->ping_file_name : file_saver->pang_file_name;
	const char *opposite_file_name = file_saver->current_is_ping_file ? file_saver->pang_file_name : file_saver->ping_file_name;
	FILE **opposite_fp = file_saver->current_is_ping_file ? &file_saver->pang_fp : &file_saver->ping_fp;

	if (fwrite(data, 1, size, *current_fp) != (size_t)size) {
		perror("Failed to write data");
		return -1;
	}

	fflush(*current_fp);

	file_saver->already_write_count++;

	if (file_saver->already_write_count >= file_saver->per_file_max_count) {
		fflush(*current_fp);
		fclose(*current_fp);
		*current_fp = NULL;

		printf("\n[Tester] pay attention to this message: file [%s] is ok, you can see it, clear [%s] and start write it .\n", current_file_name, opposite_file_name);
		remove(opposite_file_name);
		*opposite_fp = fopen(opposite_file_name, "w");

		if (!*opposite_fp) {
			perror("Failed to reopen opposite file");
			return -1;
		}

		file_saver->already_write_count = 0;
		file_saver->current_is_ping_file = !file_saver->current_is_ping_file;
	}

	return 0;
}

int ping_pang_file_saver_destroy(ping_pang_file_saver_t* file_saver)
{
	if (!file_saver) {
		fprintf(stderr, "Invalid file saver instance\n");
		return -1;
	}

	if (file_saver->ping_fp)
		fclose(file_saver->ping_fp);
	if (file_saver->pang_fp)
		fclose(file_saver->pang_fp);

	free(file_saver);

	return 0;
}