#ifndef __PING_PANG_FILE_SAVER_H__
#define __PING_PANG_FILE_SAVER_H__

typedef struct {
	char ping_file_name[256];
	char pang_file_name[256];
	FILE *ping_fp;
	FILE *pang_fp;

	int current_is_ping_file;

	int per_file_max_count;
	int already_write_count;
} ping_pang_file_saver_t;

ping_pang_file_saver_t* ping_pang_file_saver_create(char *file_name, int per_file_max_count);
int ping_pang_file_saver_write(ping_pang_file_saver_t* file_saver, int size, void* data);
int ping_pang_file_saver_destroy(ping_pang_file_saver_t* file_saver);

#endif
