#include <string.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>

#include "stream_manager.h"
#include "utils_log.h"
#include "lock_utils.h"
#include "cmap.h"

static cmap* s_shmmap = NULL;
static CMtx s_shmmap_lock = NULL;
/**
 * 1. 读写锁 只保护了 关键结构体(shm_info_t 和 shm_user_t), 防止读端访问了 错误的地址
 * 2. 读写锁 并没有保护数据，因为只有在异常情况(读端被卡住)才会出现写端覆盖读端数据的问题
 * 		并且数据被覆盖不会导致程序奔溃，并且读端被卡住 无论如何 数据都会丢弃，所以可以对数据加锁
 * 		如果对数据加锁，会有两种情况
 * 		a. 内存拷贝后，交给其他模块，比如 rtsp 发送： 增加了内存拷贝
 * 		b. 直接使用数据，等待数据使用完成， 比如等待 rtsp发送完成，增加了锁的保护时间
*/
static pthread_rwlock_t s_shm_rwlock;

// 如果要多个模块共享同一块内存，id要不一样，name、user、infos参数需要一样
// mode和type根据具体读写情况配置
shm_stream_t* shm_stream_create(char* id, const char* name, int users, int infos, int size, SHM_STREAM_MODE_E mode, SHM_STREAM_TYPE_E type)
{
	int i;
	shm_stream_t* handle = (shm_stream_t*)malloc(sizeof(shm_stream_t));
	memset(handle, 0, sizeof(shm_stream_t));

	//映射共享内存
	void* addr = NULL;
	if(type == SHM_STREAM_MMAP)
		addr = shm_stream_mmap(handle, name, users*sizeof(shm_user_t)+infos*sizeof(shm_info_t)+size);
	else if(type == SHM_STREAM_MALLOC)
	{
		addr = shm_stream_malloc(handle, name, users*sizeof(shm_user_t)+infos*sizeof(shm_info_t)+size);
		if(addr) shm_stream_malloc_fix(handle, id, name, users, addr);
	}
	if(addr == NULL)
	{
		SC_LOGE("shm_stream_mmap error");
		free(handle);
		return NULL;
	}

	handle->mtx = cmtx_create();
	handle->mode = mode;
	handle->type = type;
	handle->max_frames = infos;
	handle->max_users = users;
	handle->size = size;
	handle->user_array = (char*)addr;
	handle->info_array = handle->user_array + users*sizeof(shm_user_t);
	handle->base_addr  = handle->info_array + infos*sizeof(shm_info_t);
	snprintf(handle->name, 20, "%s", name);
	SC_LOGI("[%s] name:%s handle addr: %p, addr:%p, size: %d, users: %d, infos: %d",
		handle->name, id, handle, addr, size, users, infos);

	cmtx_enter(handle->mtx);
	shm_user_t* user = (shm_user_t*)handle->user_array;

	if(mode == SHM_STREAM_WRITE || mode == SHM_STREAM_WRITE_BLOCK)
	{
		handle->index = 0;
		//写模式默认使用user[0]
		user[0].index = 0;
		user[0].offset = 0;
		user[0].users = 0;

		snprintf(user[0].id, 32, "%s", id);
		for(i=1; i<users; i++)	//	初始化其他模式的读下标
		{
			if(strlen(user[i].id) != 0)
			{
				SC_LOGI("[CreateWriter]reader user[%d].id:%s", i, user[i].id);
				user[i].index = user[0].index;
				user[0].users++;
			}
		}
	}
	else
	{
		//如果是重复注册
		for (i=1; i<users; i++)
		{
			if (strncmp(user[i].id, id, 32) == 0)
			{
				handle->index = i;
				user[i].index = user[0].index;
				goto shm_stream_create_done;
			}
		}

		//查找空位插入  若超过users则会出现异常
		for (i=1; i<users; i++)
		{
			if (strlen(user[i].id) == 0)
			{
				handle->index = i;
				user[i].index = user[0].index;
				user[i].callback = NULL;
				user[0].users++;
				snprintf(user[i].id, 32, "%s", id);
				SC_LOGI("[CreateReader]reader user[%d].id:%s", i, user[i].id);

				break;
			}
		}
	}

	printf("[%s] show all reader [", handle->name);
	for(i=1; i<users; i++)	//	初始化其他模式的读下标
	{
		printf("%d=>%s, ", i, user[i].id);
	}
	printf("]\n");

shm_stream_create_done:
	cmtx_leave(handle->mtx);
	return handle;
}

void shm_stream_destory(shm_stream_t* handle)
{
	if(handle == NULL){
		SC_LOGE("handle is null.");
		return;
	}

	cmtx_enter(handle->mtx);
	shm_user_t* user = (shm_user_t *)handle->user_array;
	SC_LOGI("[%s] name:%s handle addr: %p, index: %d",
		user[handle->index].id, handle->name, handle, handle->index);

	if(handle->mode == SHM_STREAM_READ)
		user[0].users--;

	memset(user[handle->index].id, 0, 32);
	if(handle->type == SHM_STREAM_MMAP)
		shm_stream_unmap(handle);
	else if(handle->type == SHM_STREAM_MALLOC)
	{
		shm_stream_unmalloc(handle);
	}

	cmtx_leave(handle->mtx);;
	cmtx_delete(handle->mtx);

	if(handle != NULL)
		free(handle);
}

int shm_stream_readers_callback(shm_stream_t* handle, frame_info info, unsigned char* data, unsigned int length)
{
	if(handle == NULL) return -1;
	int i;
	shm_user_t* users = (shm_user_t*)handle->user_array;
	for(i=1; i<handle->max_users; i++)	//	初始化其他模式的读下标
	{
		if(strlen(users[i].id) != 0 && users[i].callback != NULL)
		{
			SC_LOGI("max_users:%d i:%d user[i].id:%s user[i].callback:%p", handle->max_users, i, users[i].id, users[i].callback);
			users[i].callback(info, data, length);
		}
	}

	return 0;
}

int shm_stream_info_callback_register(shm_stream_t* handle, shm_stream_info_callback callback)
{
	cmtx_enter(handle->mtx);
	shm_user_t* user = (shm_user_t*)handle->user_array;
	user[handle->index].callback = callback;
	cmtx_leave(handle->mtx);

	return 0;
}

int shm_stream_info_callback_unregister(shm_stream_t* handle)
{
	cmtx_enter(handle->mtx);
	shm_user_t* user = (shm_user_t*)handle->user_array;
	user[handle->index].callback = NULL;
	cmtx_leave(handle->mtx);

	return 0;
}


int shm_stream_put(shm_stream_t* handle, frame_info info, unsigned char* data, unsigned int length)
{
	if(handle == NULL) return -1;
	//如果没有人想要数据 则不put
	if(shm_stream_readers(handle) == 0){
		return -1;
	}
	cmtx_enter(handle->mtx);
	unsigned int head;
	shm_user_t* users = (shm_user_t*)handle->user_array;
	shm_info_t* infos = (shm_info_t*)handle->info_array;

	pthread_rwlock_wrlock(&s_shm_rwlock);
	head = users[0].index % handle->max_frames;
	memcpy(&infos[head].info, &info, sizeof(frame_info));
	infos[head].lenght = length;
	if(length + users[0].offset > handle->size){ 	//addr不够存储了， 从头存储
		infos[head].offset = 0;
		users[0].offset = 0;
	}
	else{
		infos[head].offset = users[0].offset;
	}
	char* dst_data_addr = handle->base_addr+infos[head].offset;
	users[0].offset += length;
	users[0].index = (users[0].index + 1 ) % handle->max_frames;
	pthread_rwlock_unlock(&s_shm_rwlock);

	memcpy(dst_data_addr, data, length);
	cmtx_leave(handle->mtx);
	return 0;
}

int shm_stream_get(shm_stream_t* handle, frame_info* info, unsigned char** data, unsigned int* length)
{
	if(handle == NULL) return -1;

	unsigned int tail, head;

	cmtx_enter(handle->mtx);

	pthread_rwlock_rdlock(&s_shm_rwlock);
	shm_user_t* users = (shm_user_t*)handle->user_array;
	head = users[0].index % handle->max_frames;
	tail = users[handle->index].index % handle->max_frames;
//	SC_LOGI("users[0].index:%d users[handle->index].index:%d handle->index:%d head:%d tail:%d", users[0].index, users[handle->index].index, handle->index, head, tail);

	if (head != tail)
	{
		shm_info_t* infos = (shm_info_t*)handle->info_array;
		memcpy(info, &infos[tail].info, sizeof(frame_info));
		*data = (unsigned char*)(handle->base_addr + infos[tail].offset);
		*length = infos[tail].lenght;

		users[handle->index].index = (tail + 1 ) % handle->max_frames;
		pthread_rwlock_unlock(&s_shm_rwlock);
		cmtx_leave(handle->mtx);
		return 0;
	}
	else
	{
		*length = 0;
		pthread_rwlock_unlock(&s_shm_rwlock);
		cmtx_leave(handle->mtx);
		return -1;
	}
}

int shm_stream_front(shm_stream_t* handle, frame_info* info, unsigned char** data, unsigned int* length)
{
	if(handle == NULL) return -1;

	unsigned int tail, head;

	cmtx_enter(handle->mtx);
	pthread_rwlock_rdlock(&s_shm_rwlock);
	shm_user_t* users = (shm_user_t*)handle->user_array;
	head = users[0].index % handle->max_frames;
	tail = users[handle->index].index % handle->max_frames;
	/*SC_LOGI("handle: %p, handle->index:%d, head:%d tail:%d", handle, handle->index, head, tail);*/

	if (head != tail)
	{
		shm_info_t* infos = (shm_info_t*)handle->info_array;
		memcpy(info, &infos[tail].info, sizeof(frame_info));
		*data = (unsigned char*)(handle->base_addr + infos[tail].offset);
		/*SC_LOGI("handle->base_addr: %p, infos[tail].offset: %d", handle->base_addr, infos[tail].offset);*/
		*length = infos[tail].lenght;

		pthread_rwlock_unlock(&s_shm_rwlock);
		cmtx_leave(handle->mtx);
		return 0;
	}
	else
	{
		*length = 0;

		pthread_rwlock_unlock(&s_shm_rwlock);
		cmtx_leave(handle->mtx);
		return -1;
	}
	return 0;
}

int shm_stream_post(shm_stream_t* handle)
{
	if(handle == NULL) return -1;

	unsigned int tail, head;

	cmtx_enter(handle->mtx);
	pthread_rwlock_rdlock(&s_shm_rwlock);
	shm_user_t* users = (shm_user_t*)handle->user_array;
	head = users[0].index % handle->max_frames;
	tail = users[handle->index].index % handle->max_frames;

	if (head != tail)
	{
		users[handle->index].index = (tail + 1 ) % handle->max_frames;
	}
	pthread_rwlock_unlock(&s_shm_rwlock);
	cmtx_leave(handle->mtx);

	return 0;
}

int shm_stream_sync(shm_stream_t* handle)
{
	if(handle == NULL) return -1;

	unsigned int tail, head;
	shm_user_t *user;

	cmtx_enter(handle->mtx);
	user = (shm_user_t*)handle->user_array;
	head = user[0].index % handle->max_frames;
	tail = user[handle->index].index % handle->max_frames;
	if(head != tail)
	{
		user[handle->index].index = user[0].index % handle->max_frames;
	}
	cmtx_leave(handle->mtx);

	return 0;
}

int shm_stream_remains(shm_stream_t* handle)
{
	if(handle == NULL) return -1;

    int ret;
	cmtx_enter(handle->mtx);

	unsigned int tail, head;
	shm_user_t *user;

	user = (shm_user_t*)handle->user_array;
	head = user[0].index % handle->max_frames;
	tail = user[handle->index].index % handle->max_frames;

    ret = (head + handle->max_frames - tail)% handle->max_frames;

	cmtx_leave(handle->mtx);
	return ret;

}

int shm_stream_readers(shm_stream_t* handle)
{
	if(handle == NULL) return -1;

	int ret;
	cmtx_enter(handle->mtx);

	shm_user_t* user = (shm_user_t*)handle->user_array;
	ret = user[0].users;

	cmtx_leave(handle->mtx);
	return ret;
}

void* shm_stream_malloc(shm_stream_t* handle, const char* name, unsigned int size)
{
	if(handle == NULL) return NULL;

	if(s_shmmap == NULL)
	{
		SC_LOGI("shmmap fist access, so create it .");
		s_shmmap = (cmap*)malloc(sizeof(cmap));
		cmap_init(s_shmmap);
		s_shmmap_lock = cmtx_create();
		pthread_rwlock_init(&s_shm_rwlock, NULL);
	}

	cmtx_enter(s_shmmap_lock);
	void* memory = NULL;
	void* node = cmap_pkey_find(s_shmmap, name);
	if(node == NULL)
	{
		memory = (void*)malloc(size);
		memset(memory, 0, size);
		shmmap_node* n = (shmmap_node*)malloc(sizeof(shmmap_node));
		n->addr = memory;
		n->size = size;
		n->ref_count = 1;
		/*printf("if node - ref_count: %d\n", n->ref_count);*/
		snprintf(n->name, 64, "%s", name);
		int ret = cmap_pkey_insert(s_shmmap, name, (void*)n);
		if(ret != 0)
		{
			free(n);
			free(memory);
			memory = NULL;
			SC_LOGE("cmap_pkey_insert %s error", name);
		}
		SC_LOGI("[%s] node is null, so create, ref:[%d].", name, n->ref_count);
	}
	else
	{
		shmmap_node* n = (shmmap_node*)node;
		memory = n->addr;
		n->ref_count++;
		SC_LOGI("[%s] node is exit so add, ref:[%d].", name, n->ref_count);
		/*printf("else node - ref_count: %d\n", n->ref_count);*/
	}
	cmtx_leave(s_shmmap_lock);
	return memory;
}

/*
	为了避免同一个id未能正确调用destory而造成ref_count重复累计
*/
int  shm_stream_malloc_fix(shm_stream_t* handle, char* id, const char* name, int users, void* addr)
{
	if(handle == NULL) return -1;

	int i;
	shm_user_t* user = (shm_user_t*)addr;

	cmtx_enter(s_shmmap_lock);
	for (i=0; i<users; i++)
	{
		if (strncmp(user[i].id, id, 32) == 0)
		{
			void* node = cmap_pkey_find(s_shmmap, name);
			shmmap_node* n = (shmmap_node*)node;
			n->ref_count--;
		}
	}
	cmtx_leave(s_shmmap_lock);
	return 0;
}

void shm_stream_unmalloc(shm_stream_t* handle)
{
	if(handle == NULL) return;

	if(s_shmmap == NULL)
		return;

	cmtx_enter(s_shmmap_lock);
	void* node = cmap_pkey_find(s_shmmap, handle->name);\

	if(node == NULL){
		cmtx_leave(s_shmmap_lock);
		return;
	}

	shmmap_node* n = (shmmap_node*)node;
	n->ref_count--;
	if(n->ref_count == 0)
	{
		SC_LOGI("map key:%s ref_count:%d, so destroy shm", handle->name, n->ref_count);
		free(n->addr);//free(handle->user_array);
		free(n);
		cmap_pkey_erase(s_shmmap, handle->name);
	}
	else
	{
		SC_LOGI("map key:%s current ref_count:%d", handle->name, n->ref_count);
	}
	cmtx_leave(s_shmmap_lock);
}

void* shm_stream_mmap(shm_stream_t* handle, const char* name, unsigned int size)
{
	if(handle == NULL) return NULL;

	int fd, shmid;
	void *memory;
	struct shmid_ds buf;

	char filename[32];
	snprintf(filename, 32, ".%s", name);
	if((fd = open(filename, O_RDWR|O_CREAT|O_EXCL, 0777)) > 0)
	{
		close(fd);
	}
	else
	{
		SC_LOGW("open name:%s error errno:%d %s", filename, errno, strerror(errno));
	}

	shmid = shmget(ftok(filename, 'g'), size, IPC_CREAT|0666);
	if(shmid == -1)
	{
		SC_LOGE("shmget errno:%d %s", errno, strerror(errno));
		return NULL;
	}

	memory = shmat(shmid, NULL, 0);
	if (memory == (void *)-1)
    {
        SC_LOGE("shmat failed errno:%d %s", errno, strerror(errno));
        return NULL;
    }

	shmctl(shmid, IPC_STAT, &buf);
	if (buf.shm_nattch == 1)
	{
		SC_LOGI("shm_nattch:%d", buf.shm_nattch);
		memset(memory, 0, size);
	}
	else
	{
		SC_LOGI("shm_nattch:%d", buf.shm_nattch);
	}

	return memory;
}

void shm_stream_unmap(shm_stream_t* handle)
{
	if(handle == NULL) return;

	shmdt(handle->user_array);
}

