/***************************************************************************
 * @COPYRIGHT NOTICE
 * @Copyright 2024 D-Robotics, Inc.
 * @All rights reserved.
 * @Date: 2023-02-23 14:01:59
 * @LastEditTime: 2023-03-05 15:57:48
 ***************************************************************************/
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "utils/utils_log.h"

#include "vp_wrap.h"
#include "vp_osd.h"
#define OSD_MAX_CHANNLE 1

static int region_init(vp_vflow_contex_t *vp_vflow_contex){

    hbn_rgn_attr_t region;
	int width = vp_vflow_contex->osd_info.width;
	int height = vp_vflow_contex->osd_info.height;
    region.type = OVERLAY_RGN;
	region.color = FONT_COLOR_ORANGE;
	region.alpha = 0;
	region.overlay_attr.size.width = width;
	region.overlay_attr.size.height = height;
	region.overlay_attr.pixel_fmt = PIXEL_FORMAT_VGA_8;

	SC_LOGI("osd region init %d*%d.", width, height);
	hbn_rgn_handle_t rgn_handle = vp_vflow_contex->osd_info.channel_id;
		//VSE硬件上最多支持4块OSD，其他多余的OSD通过软件操作图像数据完成。
		int ret = hbn_rgn_create(rgn_handle, &region);
        if(ret != 0){
            SC_LOGE("osd init region for channel %d failed %d.", rgn_handle, ret);
            return -1;
    }

 	hbn_rgn_bitmap_t *bitmap_p = &(vp_vflow_contex->osd_info.bitmap);
	int32_t size = width * height;
	memset(bitmap_p, 0, sizeof(hbn_rgn_bitmap_t));
	bitmap_p->pixel_fmt = PIXEL_FORMAT_VGA_8;
	bitmap_p->size.width = width;
	bitmap_p->size.height = height;
	bitmap_p->paddr = malloc(size);
	if(bitmap_p->paddr == NULL){
		SC_LOGE("regino init failed.");
		exit(-1);
	}
	memset(bitmap_p->paddr, 0x0F, size);
    return 0;
}

static int channel_attr_init(vp_vflow_contex_t *vp_vflow_contex){
    int vse_vnode_fd = vp_vflow_contex->vse_node_handle;

    hbn_rgn_chn_attr_t chn_attr = {0};
    memset(&chn_attr, 0, sizeof(hbn_rgn_chn_attr_t));
    chn_attr.show = true;
	chn_attr.invert_en = 0;
	chn_attr.display_level = 0;
	chn_attr.point.x = vp_vflow_contex->osd_info.x;
	chn_attr.point.y = vp_vflow_contex->osd_info.y;

	hbn_rgn_handle_t rgn_handle = vp_vflow_contex->osd_info.channel_id;
	/*
		1. region 和 VSE 绑定
		2. rgn_handle: 函数region_init中初始化中 rgn_handle从0开始
	*/
	int ret = hbn_rgn_attach_to_chn(rgn_handle, vse_vnode_fd, rgn_handle, &chn_attr);
	if(ret != 0){
		SC_LOGE("osd init attr for channel %d vse %d failed, ret: %d:%s", rgn_handle, vse_vnode_fd, ret, hbn_err_info(ret));
		return -1;
	}

    return 0;
}

int32_t vp_osd_init(vp_vflow_contex_t *vp_vflow_contex)
{
	int32_t ret = 0;
    ret = region_init(vp_vflow_contex); //320, 200
    if(ret != 0){
        return -1;
    }
	//(50, 50): 起始点坐标
    ret = channel_attr_init(vp_vflow_contex);
      if(ret != 0){
        return -1;
    }

	SC_LOGD("successful");
	return ret;
}

int32_t vp_osd_deinit(vp_vflow_contex_t *vp_vflow_contex)
{
    int vse_vnode_fd = vp_vflow_contex->vse_node_handle;

	hbn_rgn_handle_t rgn_handle = vp_vflow_contex->osd_info.channel_id;
	hbn_rgn_detach_from_chn(rgn_handle, vse_vnode_fd, rgn_handle);
	hbn_rgn_destroy(rgn_handle);

	hbn_rgn_bitmap_t *bitmap_p = &(vp_vflow_contex->osd_info.bitmap);
	if(bitmap_p->paddr != NULL){
		free(bitmap_p->paddr);
		bitmap_p->paddr = NULL;
	}
	SC_LOGD("successful");
	return 0;
}

int32_t vp_osd_start(vp_vflow_contex_t *vp_vflow_contex)
{
	int32_t ret = 0;

	SC_LOGD("successful");
	return ret;
}

int32_t vp_osd_stop(vp_vflow_contex_t *vp_vflow_contex)
{
	int32_t ret = 0;

	SC_LOGD("successful");
	return ret;
}
int32_t vp_osd_draw_world(vp_vflow_contex_t *vp_vflow_contex, char *str){

	hbn_rgn_handle_t handle = vp_vflow_contex->osd_info.channel_id;
    if((handle < 0)){
        SC_LOGE("osd draw world failed, handle is invalid %d.", handle);
        return -1;
    }

    hbn_rgn_bitmap_t *bitmap_p = &(vp_vflow_contex->osd_info.bitmap);
    hbn_rgn_draw_word_t draw_word = {0};
	draw_word.font_size = FONT_SIZE_MEDIUM;
	draw_word.font_color = FONT_COLOR_WHITE;
	draw_word.bg_color = FONT_COLOR_DARKGRAY;
    draw_word.font_alpha = 10;
	draw_word.bg_alpha = 5;
	draw_word.point.x = 0;
	draw_word.point.y = 0;
	draw_word.flush_en = false;
	draw_word.draw_string = (uint8_t*)str;
	draw_word.paddr = bitmap_p->paddr;
	draw_word.size = bitmap_p->size;

	//用户申请好的buffer(malloc)上画字
    int ret = hbn_rgn_draw_word(&draw_word);
    if(ret != 0){
        SC_LOGE("osd draw world for channel %d failed.", handle);
        return -1;
    }

	//将bitmap 中的数据，拷贝到物理内存中
    ret = hbn_rgn_setbitmap(handle, bitmap_p);
    if(ret != 0){
        SC_LOGE("osd set bitmap for channel %d failed.", handle);
        return -1;
    }
    return 0;
}
