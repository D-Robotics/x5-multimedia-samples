#ifndef __UNIX_SOCKET_H__
#define __UNIX_SOCKET_H__

#define FRAME_HEADER     0xDEADBEEF
#define FRAME_TAIL       0xBAADF00D
#define MSG_TYPE_FRAME   1
#define MSG_TYPE_RELEASE 2

#define SOCKET_PATH_VSE_ENCODE "/tmp/vse_encode.sock"
#define SOCKET_PATH_VSE_HDMI   "/tmp/vse_hdmi.sock"

typedef struct {
	uint32_t msg_type;
	uint32_t frame_id;
	hb_mem_graphic_buf_t img_buf;
} frame_meta_t;

typedef struct {
	uint32_t header;
	uint32_t data_len;
	frame_meta_t meta;
	uint32_t tail;
} socket_pkg_t;

int create_unix_socket(const char *path);
int accept_unix_client(int sock_fd);
int send_frame_pkg(int fd, const frame_meta_t *meta);
int recv_frame_pkg(int fd, frame_meta_t *meta);
int send_release_pkg(int fd, uint32_t frame_id);
int recv_release_pkg(int fd, uint32_t *frame_id);
void close_unix_socket(int fd, const char *path);

#endif