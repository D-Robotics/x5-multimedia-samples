#ifndef _PIPE_STREAM_H_
#define _PIPE_STREAM_H_

#define SERVER_PORT 		8100

#define STEREO_RES_WIDTH	640
#define STEREO_RES_HEIGHT	352
#define DEPTH_SIZE			(STEREO_RES_WIDTH * STEREO_RES_HEIGHT * sizeof(uint16_t))

#define STREAM_INFO_JSON_STR_LEN	2048

typedef enum {
	STREAM_NODE_CAM_RIGHT = 0,
	STREAM_NODE_CAM_LEFT,
	STREAM_NODE_DEPTH,
	STREAM_NODE_IMU,
	STREAM_NODE_TAIL
} stream_node_type_t;
#define STREAM_NODE_TYPE_NUM	STREAM_NODE_TAIL

#define STREAM_HEADER_MAGIC 0x0425BEEF

/* Node Header */
typedef struct {
	uint32_t type;
	uint32_t size;
	uint32_t offset;
	uint64_t timestamps;
	uint32_t frame_id;
	uint8_t  reserved[8];
} stream_node_header_t;

/* Stream Header */
typedef struct {
	uint32_t magic;
	uint32_t total_size;
	uint8_t reserved[24];
	stream_node_header_t node_headers[STREAM_NODE_TYPE_NUM];
} stream_header_t;

int check_network(const char *ifname);
int server_init(void);
int client_init(const char *server_ip);
void server_close(int server_fd);
int server_wait_client_connect(int server_fd);
void server_disconnect_client(int server_fd, int client_fd);
int server_send_data(int client_fd, void *buf, int size);


#endif

