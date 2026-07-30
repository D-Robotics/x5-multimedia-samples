/***************************************************************************
 * SC230AI camera pipeline: VIN -> ISP -> VSE -> MIPI DRM preview (Plane 33).
 * Capture: JPEG still (+ watermark). REC: H264 encode + live MP4 mux.
 ***************************************************************************/
#include "camera_pipeline.h"
#include "drm_ui_plane.h"
#include "media_player.h"
#include "media_still.h"
#include "rec_mux.h"
#include "sys_status.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "channel_param_parser.h"
#include "hb_media_codec.h"
#include "hb_media_error.h"

extern int vin_isp_is_online;
extern int isp_vse_is_online;

static int env_u32_in_range(const char *name, int defv, int minv, int maxv)
{
	const char *v = getenv(name);
	char *end = NULL;
	long val;

	if (!v || !*v)
		return defv;
	val = strtol(v, &end, 10);
	if (*end != '\0' || val < minv || val > maxv)
		return defv;
	return (int)val;
}

static int env_int_default(const char *name, int defv)
{
	const char *v = getenv(name);
	char *end = NULL;
	long val;

	if (!v || !*v)
		return defv;
	val = strtol(v, &end, 10);
	if (end == v)
		return defv;
	return (int)val;
}

/*
 * Buffer-horizontal circular shift for NV12 plane. Default 0 — do not inherit
 * CAM_PREV_UNSHIFT (that is for ARGB UI). With DRM ROTATE_90, buffer-X wrap
 * shows as top/bottom on screen; that was the “bottom edge” bug.
 * Override: CAM_VIDEO_UNSHIFT=±N
 */
static int video_plane_h_unshift_px(int video_w)
{
	const char *v = getenv("CAM_VIDEO_UNSHIFT");
	int s;

	(void)video_w;
	if (!v || !v[0])
		return 0;
	s = env_int_default("CAM_VIDEO_UNSHIFT", 0);
	s &= ~1;
	return s;
}

/*
 * Buffer-vertical circular shift. With DRM CAM_VIDEO_ROTATE=90, buffer-Y wrap
 * shows as left/right on screen.
 *
 * Same panel calibration as album ARGB photos (CAM_PREV_UNSHIFT), scaled by
 * video_h/720 (~-52 @1080). Do not invent a separate VIDEO_PANEL offset —
 * photo and video must share one L/R tune. Override: CAM_VIDEO_V_UNSHIFT=±N.
 */
static int video_plane_v_unshift_px(int video_h)
{
	const char *v = getenv("CAM_VIDEO_V_UNSHIFT");
	int panel_u;
	int s;

	if (v && v[0]) {
		s = env_int_default("CAM_VIDEO_V_UNSHIFT", 0);
	} else {
		panel_u = env_int_default("CAM_PREV_UNSHIFT", -35);
		if (video_h <= 0)
			video_h = 1080;
		s = (panel_u * video_h) / 720;
	}
	s &= ~1;
	return s;
}

/* Left-circular unshift on active NV12 (same convention as ARGB CAM_PREV_UNSHIFT). */
static void nv12_h_unshift_graphic(hb_mem_graphic_buf_t *dst, int w, int h,
		int shift)
{
	uint8_t *y;
	uint8_t *uv;
	uint8_t *tmp;
	int stride;
	int vstride;
	int row;
	int x;

	if (!dst || w <= 1 || h <= 0 || shift == 0)
		return;
	shift %= w;
	if (shift < 0)
		shift += w;
	if (shift == 0)
		return;
	shift &= ~1;

	y = dst->virt_addr[0];
	stride = dst->stride > 0 ? dst->stride : w;
	vstride = dst->height > 0 ? dst->height : h;
	if (dst->vstride > 0 && dst->vstride == dst->height)
		vstride = dst->vstride;
	else if (dst->height > 0)
		vstride = dst->height;
	if (!y)
		return;
	uv = y + (size_t)stride * (size_t)vstride;

	tmp = malloc((size_t)w);
	if (!tmp)
		return;

	for (row = 0; row < h && row < vstride; row++) {
		uint8_t *row_y = y + (size_t)row * (size_t)stride;

		for (x = 0; x < w; x++)
			tmp[x] = row_y[(x - shift + w) % w];
		memcpy(row_y, tmp, (size_t)w);
	}
	for (row = 0; row < h / 2 && row < vstride / 2; row++) {
		uint8_t *row_uv = uv + (size_t)row * (size_t)stride;

		for (x = 0; x < w; x++)
			tmp[x] = row_uv[(x - shift + w) % w];
		memcpy(row_uv, tmp, (size_t)w);
	}
	free(tmp);
	(void)hb_mem_flush_buf_with_vaddr((uint64_t)y,
		(size_t)stride * (size_t)vstride +
			(size_t)stride * (size_t)vstride / 2);
}

/* Top-circular unshift (rows wrap): fixes right-edge strip under ROTATE_90. */
static void nv12_v_unshift_graphic(hb_mem_graphic_buf_t *dst, int w, int h,
		int shift)
{
	uint8_t *y;
	uint8_t *uv;
	uint8_t *tmp;
	int stride;
	int vstride;
	int row;
	int uh, us;
	size_t row_bytes;

	if (!dst || w <= 0 || h <= 1 || shift == 0)
		return;
	shift %= h;
	if (shift < 0)
		shift += h;
	if (shift == 0)
		return;
	shift &= ~1;

	y = dst->virt_addr[0];
	stride = dst->stride > 0 ? dst->stride : w;
	vstride = dst->height > 0 ? dst->height : h;
	if (dst->vstride > 0 && dst->vstride == dst->height)
		vstride = dst->vstride;
	else if (dst->height > 0)
		vstride = dst->height;
	if (!y || h > vstride)
		return;
	uv = y + (size_t)stride * (size_t)vstride;
	row_bytes = (size_t)w;

	tmp = malloc(row_bytes * (size_t)h);
	if (!tmp)
		return;

	for (row = 0; row < h; row++)
		memcpy(tmp + (size_t)row * row_bytes,
			y + (size_t)row * (size_t)stride, row_bytes);
	for (row = 0; row < h; row++)
		memcpy(y + (size_t)row * (size_t)stride,
			tmp + (size_t)((row - shift + h) % h) * row_bytes,
			row_bytes);

	uh = h / 2;
	us = shift / 2;
	for (row = 0; row < uh; row++)
		memcpy(tmp + (size_t)row * row_bytes,
			uv + (size_t)row * (size_t)stride, row_bytes);
	for (row = 0; row < uh; row++)
		memcpy(uv + (size_t)row * (size_t)stride,
			tmp + (size_t)((row - us + uh) % uh) * row_bytes,
			row_bytes);

	free(tmp);
	(void)hb_mem_flush_buf_with_vaddr((uint64_t)y,
		(size_t)stride * (size_t)vstride +
			(size_t)stride * (size_t)vstride / 2);
}

static void nv12_apply_plane_unshift(hb_mem_graphic_buf_t *dst, int w, int h,
		int video_w, int video_h)
{
	int hs = video_plane_h_unshift_px(video_w);
	int vs = video_plane_v_unshift_px(video_h);

	if (hs != 0)
		nv12_h_unshift_graphic(dst, w, h, hs);
	if (vs != 0)
		nv12_v_unshift_graphic(dst, w, h, vs);
}

static int ensure_still_buf_locked(camera_pipeline_t *cam);
static void free_still_bufs_locked(camera_pipeline_t *cam);
static int copy_strided_nv12_to_graphic(hb_mem_graphic_buf_t *dst,
		const uint8_t *src_y, const uint8_t *src_uv,
		int w, int h, int src_stride, int src_vstride);

/*
 * Copy live frame into still_buf + unshift. Caller may release the VSE
 * frame before DRM commit so ochn buffers are never held across display.
 * Returns still_buf index (>=0) or -1.
 */
static int live_preview_prepare_still(camera_pipeline_t *cam,
		hbn_vnode_image_t *frame)
{
	hb_mem_graphic_buf_t *dst = NULL;
	int idx = 0;
	int w, h, stride, vstride;
	const uint8_t *sy;
	const uint8_t *suv;
	int hs = video_plane_h_unshift_px(cam->video_w);
	int vs = video_plane_v_unshift_px(cam->video_h);

	if (!cam || !frame)
		return -1;

	w = (int)frame->buffer.width;
	h = (int)frame->buffer.height;
	stride = frame->buffer.stride > 0 ? frame->buffer.stride : w;
	vstride = frame->buffer.vstride > 0 ? frame->buffer.vstride : h;
	sy = frame->buffer.virt_addr[0];
	suv = frame->buffer.virt_addr[1];

	pthread_mutex_lock(&cam->lock);
	if (ensure_still_buf_locked(cam) == 0 && cam->still_buf_ok > 0) {
		idx = cam->still_idx % cam->still_buf_ok;
		dst = &cam->still_buf[idx];
	}
	pthread_mutex_unlock(&cam->lock);

	if (!dst || !sy ||
			copy_strided_nv12_to_graphic(dst, sy, suv, w, h,
				stride, vstride) != 0)
		return -1;

	nv12_apply_plane_unshift(dst, w, h, cam->video_w, cam->video_h);
	{
		static int once_u;

		if (!once_u) {
			once_u = 1;
			printf("[cam] live preview unshift h=%d v=%d "
				"(same CAM_PREV_UNSHIFT as album)\n",
				hs, vs);
			fflush(stdout);
		}
	}
	return idx;
}

static int live_preview_commit_buf(camera_pipeline_t *cam,
		hb_mem_graphic_buf_t *buf)
{
	if (!cam || !buf)
		return -1;
	if (cam->ui_overlay)
		return drm_ui_plane_show_video(cam->ui_overlay, &cam->drm, buf);
	return vp_display_set_frame(&cam->drm, buf);
}

static void set_env_default_if_absent(const char *name, const char *value)
{
	if (!getenv(name))
		(void)setenv(name, value, 0);
}

static void ensure_save_dir(camera_pipeline_t *cam)
{
	const char *env = getenv("CAM_SAVE_DIR");
	struct stat st;

	if (env && env[0])
		snprintf(cam->save_dir, sizeof(cam->save_dir), "%s", env);
	else if (stat("/userdata", &st) == 0 && S_ISDIR(st.st_mode))
		snprintf(cam->save_dir, sizeof(cam->save_dir),
			"/userdata/lvgl_camera");
	else
		snprintf(cam->save_dir, sizeof(cam->save_dir),
			"/tmp/lvgl_camera");

	if (mkdir(cam->save_dir, 0755) != 0 && errno != EEXIST)
		fprintf(stderr, "mkdir %s failed: %s\n", cam->save_dir,
			strerror(errno));
}

static void make_timestamped_path(camera_pipeline_t *cam, const char *prefix,
		const char *ext, char *out, size_t out_len)
{
	time_t now = time(NULL);
	struct tm tm_now;
	char tbuf[32];
	unsigned seq;

	localtime_r(&now, &tm_now);
	strftime(tbuf, sizeof(tbuf), "%Y%m%d_%H%M%S", &tm_now);
	seq = ++cam->capture_name_seq;
	/* Include seq so burst shots in the same second do not collide. */
	snprintf(out, out_len, "%s/%s_%s_%04u.%s", cam->save_dir, prefix, tbuf,
		seq, ext);
}

static void make_watermark_text(char *buf, size_t len)
{
	time_t now = time(NULL);
	struct tm tm_now;
	char tbuf[48];
	const char *prefix = getenv("CAM_WATERMARK_TEXT");

	localtime_r(&now, &tm_now);
	strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm_now);
	if (prefix && prefix[0])
		snprintf(buf, len, "%s %s", prefix, tbuf);
	else
		snprintf(buf, len, "%s", tbuf);
}

static int watermark_enabled(void)
{
	return env_u32_in_range("CAM_WATERMARK", 1, 0, 1);
}

static int storage_guard(camera_pipeline_t *cam)
{
	sys_status_t st;
	int min_mb;

	sys_status_poll(&st, cam->save_dir);
	min_mb = env_u32_in_range("CAM_STORAGE_MIN_MB", 64, 1, 1024 * 1024);
	/*
	 * Trust free_mb directly — do not rely only on storage_full bit
	 * (struct layout skew from partial rebuilds can misread that flag).
	 */
	if (st.free_mb >= 0 && st.free_mb < min_mb) {
		fprintf(stderr, "[cam] storage full: %ld MB free (min %d)\n",
			st.free_mb, min_mb);
		return -1;
	}
	return 0;
}

/* Caller holds cam->lock. Stops further encode; UI finalizes via stop_begin. */
static void rec_request_abort_locked(camera_pipeline_t *cam, int reason)
{
	if (!cam || !cam->encode_active)
		return;
	cam->encode_active = 0;
	if (cam->rec_abort == 0)
		cam->rec_abort = reason;
	fprintf(stderr, "[cam] REC auto-stop (%s) after %d frames — "
		"await UI finalize\n",
		reason == 1 ? "storage full" :
		reason == 2 ? "mux write fail" : "abort",
		cam->record_frames);
	fflush(stderr);
}

int camera_find_sc230ai_index(void)
{
	uint32_t n = vp_get_sensors_list_number();
	uint32_t i;
	int fallback = -1;

	/* Prefer linear 30fps over 10fps for smoother preview */
	for (i = 0; i < n; i++) {
		const char *name = vp_sensor_config_list[i]->sensor_name;

		if (!strstr(name, "sc230ai"))
			continue;
		if (strstr(name, "30fps") && !strstr(name, "dol2"))
			return (int)i;
		if (fallback < 0)
			fallback = (int)i;
	}
	return fallback;
}

static int create_camera_node(pipe_contex_t *pipe)
{
	return hbn_camera_create(pipe->sensor_config->camera_config,
			&pipe->cam_fd);
}

static int create_vin_node(pipe_contex_t *pipe, int active_mipi_host)
{
	vp_sensor_config_t *cfg = pipe->sensor_config;
	hbn_buf_alloc_attr_t alloc_attr = {0};
	uint64_t mask = 0;
	int ret;

	cfg->vin_node_attr->cim_attr.mipi_rx = active_mipi_host;
	if (vin_isp_is_online) {
		cfg->vin_node_attr->cim_attr.cim_isp_flyby = 1;
		cfg->vin_ochn_attr->ddr_en = 0;
	} else {
		cfg->vin_node_attr->cim_attr.cim_isp_flyby = 0;
		cfg->vin_ochn_attr->ddr_en = 1;
	}

	ret = hbn_vnode_open(HB_VIN, active_mipi_host, AUTO_ALLOC_ID,
			&pipe->vin_node_handle);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_attr(pipe->vin_node_handle, cfg->vin_node_attr);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_ichn_attr(pipe->vin_node_handle, 0,
			cfg->vin_ichn_attr);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_ochn_attr(pipe->vin_node_handle, 0,
			cfg->vin_ochn_attr);
	ERR_CON_EQ(ret, 0);

	mask = cfg->vin_attr_ex->vin_attr_ex_mask;
	if (mask) {
		vin_attr_ex_t ex = *cfg->vin_attr_ex;
		for (uint8_t t = 0; t < VIN_ATTR_EX_INVALID; t++) {
			if ((mask & (1U << t)) == 0)
				continue;
			ex.ex_attr_type = t;
			ret = hbn_vnode_set_attr_ex(pipe->vin_node_handle, &ex);
			ERR_CON_EQ(ret, 0);
		}
	}

	if (!vin_isp_is_online) {
		alloc_attr.buffers_num = 3;
		alloc_attr.is_contig = 1;
		alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN |
			HB_MEM_USAGE_CPU_WRITE_OFTEN | HB_MEM_USAGE_CACHED;
		ret = hbn_vnode_set_ochn_buf_attr(pipe->vin_node_handle, 0,
				&alloc_attr);
		ERR_CON_EQ(ret, 0);
	}
	return 0;
}

static int create_isp_node(pipe_contex_t *pipe)
{
	vp_sensor_config_t *cfg = pipe->sensor_config;
	hbn_buf_alloc_attr_t alloc_attr = {0};
	int ret;

	if (vin_isp_is_online)
		cfg->isp_attr->input_mode = PASSTHROUGH_MODE;
	else
		cfg->isp_attr->input_mode = DDR_MODE;

	cfg->isp_ochn_attr->ddr_en = isp_vse_is_online ? 0 : 1;

	ret = hbn_vnode_open(HB_ISP, 0, AUTO_ALLOC_ID, &pipe->isp_node_handle);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_attr(pipe->isp_node_handle, cfg->isp_attr);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_ochn_attr(pipe->isp_node_handle, 0,
			cfg->isp_ochn_attr);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_ichn_attr(pipe->isp_node_handle, 0,
			cfg->isp_ichn_attr);
	ERR_CON_EQ(ret, 0);

	if (!isp_vse_is_online) {
		alloc_attr.buffers_num = 3;
		alloc_attr.is_contig = 1;
		alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN |
			HB_MEM_USAGE_CPU_WRITE_OFTEN | HB_MEM_USAGE_CACHED;
		ret = hbn_vnode_set_ochn_buf_attr(pipe->isp_node_handle, 0,
				&alloc_attr);
		ERR_CON_EQ(ret, 0);
	}
	return 0;
}

static int create_vse_node(pipe_contex_t *pipe)
{
	vp_sensor_config_t *cfg = pipe->sensor_config;
	uint32_t w = cfg->isp_attr->crop.w;
	uint32_t h = cfg->isp_attr->crop.h;
	vse_attr_t attr = {0};
	vse_ichn_attr_t ichn = {0};
	vse_ochn_attr_t ochn = {0};
	hbn_buf_alloc_attr_t alloc = {0};
	int ret;

	ichn.width = w;
	ichn.height = h;
	ichn.fmt = FRM_FMT_NV12;
	ichn.bit_width = 8;

	ochn.chn_en = CAM_TRUE;
	ochn.roi.x = 0;
	ochn.roi.y = 0;
	ochn.roi.w = w;
	ochn.roi.h = h;
	ochn.fmt = FRM_FMT_NV12;
	ochn.bit_width = 8;
	ochn.target_w = w;
	ochn.target_h = h;

	ret = hbn_vnode_open(HB_VSE, 0, AUTO_ALLOC_ID, &pipe->vse_node_handle);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_attr(pipe->vse_node_handle, &attr);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_ichn_attr(pipe->vse_node_handle, 0, &ichn);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vnode_set_ochn_attr(pipe->vse_node_handle, 0, &ochn);
	ERR_CON_EQ(ret, 0);

	/* 4+ leaves headroom while DRM/LVGL init; 2 can stall flyby if no consumer. */
	alloc.buffers_num = env_u32_in_range("VP_QUICKSTART_VSE_BUF_NUM", 4, 2, 6);
	alloc.is_contig = 1;
	alloc.flags = HB_MEM_USAGE_CPU_READ_OFTEN |
		HB_MEM_USAGE_CPU_WRITE_OFTEN | HB_MEM_USAGE_CACHED;
	ret = hbn_vnode_set_ochn_buf_attr(pipe->vse_node_handle, 0, &alloc);
	ERR_CON_EQ(ret, 0);
	return 0;
}

static int create_and_run_vflow(pipe_contex_t *pipe, int active_mipi_host)
{
	int ret;

	ret = create_camera_node(pipe);
	ERR_CON_EQ(ret, 0);
	ret = create_vin_node(pipe, active_mipi_host);
	ERR_CON_EQ(ret, 0);
	ret = create_isp_node(pipe);
	ERR_CON_EQ(ret, 0);
	ret = create_vse_node(pipe);
	ERR_CON_EQ(ret, 0);

	ret = hbn_vflow_create(&pipe->vflow_fd);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_add_vnode(pipe->vflow_fd, pipe->vin_node_handle);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_add_vnode(pipe->vflow_fd, pipe->isp_node_handle);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_add_vnode(pipe->vflow_fd, pipe->vse_node_handle);
	ERR_CON_EQ(ret, 0);

	if (vin_isp_is_online)
		ret = hbn_vflow_bind_vnode(pipe->vflow_fd,
				pipe->vin_node_handle, 1,
				pipe->isp_node_handle, 0);
	else
		ret = hbn_vflow_bind_vnode(pipe->vflow_fd,
				pipe->vin_node_handle, 0,
				pipe->isp_node_handle, 0);
	ERR_CON_EQ(ret, 0);

	if (isp_vse_is_online)
		ret = hbn_vflow_bind_vnode(pipe->vflow_fd,
				pipe->isp_node_handle, 1,
				pipe->vse_node_handle, 0);
	else
		ret = hbn_vflow_bind_vnode(pipe->vflow_fd,
				pipe->isp_node_handle, 0,
				pipe->vse_node_handle, 0);
	ERR_CON_EQ(ret, 0);

	ret = hbn_camera_attach_to_vin(pipe->cam_fd, pipe->vin_node_handle);
	ERR_CON_EQ(ret, 0);
	ret = hbn_vflow_start(pipe->vflow_fd);
	ERR_CON_EQ(ret, 0);
	return 0;
}

static int display_init_mipi(camera_pipeline_t *cam)
{
	isp_attr_t *isp = cam->pipe.sensor_config->isp_attr;
	int ret;

	cam->drm.preferred_connector_type = VP_DISPLAY_OUTPUT_MIPI;
	ret = vp_display_init(&cam->drm, isp->crop.w, isp->crop.h);
	if (ret != 0) {
		fprintf(stderr, "MIPI display init failed\n");
		return -1;
	}
	cam->display_ready = 1;
	return 0;
}

static int encode_config_h264(media_codec_context_t *ctx, int width,
		int height, int fps, uint32_t bitrate_kbps)
{
	mc_video_codec_enc_params_t *params;

	memset(ctx, 0, sizeof(*ctx));
	ctx->encoder = 1;
	ctx->codec_id = MEDIA_CODEC_ID_H264;
	params = &ctx->video_enc_params;
	params->width = width;
	params->height = height;
	params->pix_fmt = MC_PIXEL_FORMAT_NV12;
	params->bitstream_buf_size = (width * height * 3 / 2 + 0x3ff) & ~0x3ff;
	params->frame_buf_count = 5;
	params->external_frame_buf = 0;
	params->bitstream_buf_count = 8;
	params->gop_params.gop_preset_idx = 1;
	/*
	 * Keep H264 sensor-native (no CAM_VIDEO_ROTATE). Album HW play blits
	 * onto the live video plane (video_w x video_h); a rotated bitstream
	 * (e.g. 1080x1920) fails every frame and leaves the poster stuck.
	 * JPEG stills still use CAM_VIDEO_ROTATE via media_still.
	 */
	params->rot_degree = MC_CCW_0;
	params->mir_direction = MC_DIRECTION_NONE;
	params->frame_cropping_flag = 0;
	params->enable_user_pts = 1;
	params->gop_params.decoding_refresh_type = 2;
	params->rc_params.mode = MC_AV_RC_MODE_H264CBR;

	if (hb_mm_mc_get_rate_control_config(ctx, &params->rc_params) != 0)
		return -1;

	params->rc_params.h264_cbr_params.intra_period = fps > 0 ? fps : 30;
	params->rc_params.h264_cbr_params.intra_qp = 30;
	params->rc_params.h264_cbr_params.bit_rate = bitrate_kbps;
	params->rc_params.h264_cbr_params.frame_rate = fps > 0 ? fps : 30;
	params->rc_params.h264_cbr_params.initial_rc_qp = 20;
	params->rc_params.h264_cbr_params.vbv_buffer_size = 3000;
	params->rc_params.h264_cbr_params.mb_level_rc_enalbe = 1;
	params->rc_params.h264_cbr_params.min_qp_I = 8;
	params->rc_params.h264_cbr_params.max_qp_I = 50;
	params->rc_params.h264_cbr_params.min_qp_P = 8;
	params->rc_params.h264_cbr_params.max_qp_P = 50;
	params->rc_params.h264_cbr_params.min_qp_B = 8;
	params->rc_params.h264_cbr_params.max_qp_B = 50;
	params->rc_params.h264_cbr_params.hvs_qp_enable = 1;
	params->rc_params.h264_cbr_params.hvs_qp_scale = 2;
	params->rc_params.h264_cbr_params.max_delta_qp = 10;
	params->rc_params.h264_cbr_params.qp_map_enable = 0;
	return 0;
}

static void encode_teardown_locked(camera_pipeline_t *cam)
{
	media_codec_context_t *mc;
	rec_mux_t *mux;

	/* Preview may still be in encode_one_frame outside this lock. */
	while (cam->encode_busy) {
		pthread_mutex_unlock(&cam->lock);
		usleep(1000);
		pthread_mutex_lock(&cam->lock);
	}

	mc = cam->mc;
	mux = cam->rec_mux;

	cam->recording = 0;
	cam->encode_active = 0;
	cam->mc = NULL;
	cam->rec_mux = NULL;

	if (mc) {
		(void)hb_mm_mc_pause(mc);
		(void)hb_mm_mc_release(mc);
		free(mc);
	}
	/* Pipeline deinit / abort — close mux here (normal stop uses finalize). */
	if (mux)
		rec_mux_close(mux);
}

/* Pack NV12 from optional strided graphic buf into tightly packed w*h. */
static int nv12_pack_graphic(const hb_mem_graphic_buf_t *buf,
		uint8_t *dst_y, uint8_t *dst_uv, int w, int h)
{
	int stride;
	int row;
	const uint8_t *src_y;
	const uint8_t *src_uv;

	if (!buf || !dst_y || !dst_uv || w <= 0 || h <= 0 || !buf->virt_addr[0])
		return -1;

	stride = buf->stride > 0 ? buf->stride : w;
	if (stride < w)
		stride = w;
	src_y = buf->virt_addr[0];
	src_uv = buf->virt_addr[1];
	if (!src_uv) {
		int y_rows = buf->vstride > 0 ? buf->vstride : h;

		src_uv = src_y + (size_t)stride * (size_t)y_rows;
	}

	if (stride == w) {
		memcpy(dst_y, src_y, (size_t)w * (size_t)h);
		memcpy(dst_uv, src_uv, (size_t)w * (size_t)h / 2);
		return 0;
	}

	for (row = 0; row < h; row++)
		memcpy(dst_y + (size_t)row * (size_t)w,
			src_y + (size_t)row * (size_t)stride, (size_t)w);
	for (row = 0; row < h / 2; row++)
		memcpy(dst_uv + (size_t)row * (size_t)w,
			src_uv + (size_t)row * (size_t)stride, (size_t)w);
	return 0;
}

/*
 * Codec + mux outside cam->lock (dequeue can block ~1s). Caller snapshots
 * mc/mux/pts/frames under lock and sets encode_busy; applies results after.
 * *abort_reason: 0=none, 1=storage, 2=mux fail.
 */
static int encode_one_frame(media_codec_context_t *mc, rec_mux_t *mux,
		hbn_vnode_image_t *frame, int64_t *record_pts, int *record_frames,
		int fps, const char *mp4_path, const char *save_dir,
		uint16_t *anim_frames, int *anim_nframes, int *abort_reason)
{
	media_codec_buffer_t in_buf = {0};
	media_codec_buffer_t out_buf = {0};
	media_codec_output_buffer_info_t info = {0};
	int w = frame->buffer.width;
	int h = frame->buffer.height;
	size_t y_sz = (size_t)w * (size_t)h;
	size_t uv_sz = y_sz / 2;
	size_t nv12_size = y_sz + uv_sz;
	uint8_t *dst_y;
	uint8_t *dst_uv;
	int ret;
	sys_status_t st;
	int min_mb;

	if (abort_reason)
		*abort_reason = 0;
	if (!mc || !mux || !record_pts || !record_frames)
		return -1;
	if (fps <= 0)
		fps = 30;

	ret = hb_mm_mc_dequeue_input_buffer(mc, &in_buf, 1000);
	if (ret != 0)
		return ret;

	dst_y = in_buf.vframe_buf.vir_ptr[0];
	if (!dst_y) {
		(void)hb_mm_mc_queue_input_buffer(mc, &in_buf, 100);
		return -1;
	}
	dst_uv = in_buf.vframe_buf.vir_ptr[1] ?
		in_buf.vframe_buf.vir_ptr[1] : (dst_y + y_sz);

	if (nv12_pack_graphic(&frame->buffer, dst_y, dst_uv, w, h) != 0) {
		(void)hb_mm_mc_queue_input_buffer(mc, &in_buf, 100);
		return -1;
	}

	if (env_u32_in_range("CAM_REC_WATERMARK", 0, 0, 1)) {
		char wtext[64];

		make_watermark_text(wtext, sizeof(wtext));
		media_still_nv12_watermark(dst_y, dst_uv, w, h, wtext);
	}

	in_buf.type = MC_VIDEO_FRAME_BUFFER;
	in_buf.vframe_buf.width = w;
	in_buf.vframe_buf.height = h;
	in_buf.vframe_buf.pix_fmt = MC_PIXEL_FORMAT_NV12;
	in_buf.vframe_buf.size = (hb_s32)nv12_size;
	in_buf.vframe_buf.pts = (hb_u64)(*record_pts);

	(void)hb_mem_flush_buf_with_vaddr((uint64_t)dst_y, y_sz);
	(void)hb_mem_flush_buf_with_vaddr((uint64_t)dst_uv, uv_sz);

	ret = hb_mm_mc_queue_input_buffer(mc, &in_buf, 1000);
	if (ret != 0)
		return ret;

	ret = hb_mm_mc_dequeue_output_buffer(mc, &out_buf, &info, 1000);
	if (ret != 0)
		return ret;

	if (out_buf.vstream_buf.size > 0 && out_buf.vstream_buf.vir_ptr) {
		if (rec_mux_write(mux, out_buf.vstream_buf.vir_ptr,
				(int)out_buf.vstream_buf.size, *record_pts,
				0) != 0) {
			fprintf(stderr, "[cam] REC mux write failed (frame %d)\n",
				*record_frames + 1);
			if (abort_reason)
				*abort_reason = 2;
			(void)hb_mm_mc_queue_output_buffer(mc, &out_buf, 1000);
			return -1;
		}
	}

	*record_pts += 90000 / fps;
	ret = hb_mm_mc_queue_output_buffer(mc, &out_buf, 1000);
	(*record_frames)++;

	/*
	 * Mid-REC storage poll ~1/sec. Start only checks once; long clips can
	 * fill the disk — stop encoding and let UI finalize + toast.
	 */
	if (save_dir && ((*record_frames) % fps) == 0) {
		sys_status_poll(&st, save_dir);
		min_mb = env_u32_in_range("CAM_STORAGE_MIN_MB", 64, 1,
			1024 * 1024);
		if (st.free_mb >= 0 && st.free_mb < min_mb) {
			fprintf(stderr, "[cam] storage full: %ld MB free (min %d)\n",
				st.free_mb, min_mb);
			if (abort_reason)
				*abort_reason = 1;
		}
	}

	if (*record_frames == 1 && mp4_path && mp4_path[0]) {
		uint16_t thumb[CAM_THUMB_W * CAM_THUMB_H];

		/*
		 * First frame → album preview + list thumb for the MP4 path
		 * (same orientation as stills; no UI_BAR_ROT180).
		 */
		(void)media_still_write_rgb565_preview(dst_y, dst_uv, w, h, w,
			mp4_path, CAM_PREV_W, CAM_PREV_H);
		media_still_nv12_thumb(dst_y, dst_uv, w, h, thumb);
		(void)media_still_write_list_thumb(mp4_path, thumb);
		/* Always write JPEG poster — board ffmpeg cannot extract frames. */
		(void)media_still_write_video_poster(dst_y, dst_uv, w, h, w,
			mp4_path);
		printf("[cam] REC first encoded frame ok (live MP4 mux)\n");
	}
	/* Sample list anim thumb: first CAM_ANIM_SECS @ CAM_ANIM_FPS. */
	if (anim_frames && anim_nframes) {
		int max_f = fps * CAM_ANIM_SECS;
		int stride = fps / CAM_ANIM_FPS;
		size_t px = (size_t)CAM_THUMB_W * (size_t)CAM_THUMB_H;

		if (stride < 1)
			stride = 1;
		if (*record_frames <= max_f &&
		    *anim_nframes < CAM_ANIM_MAX_FRAMES &&
		    ((*record_frames - 1) % stride) == 0) {
			media_still_nv12_thumb(dst_y, dst_uv, w, h,
				anim_frames + (size_t)(*anim_nframes) * px);
			(*anim_nframes)++;
		}
	}
	return ret;
}

/*
 * Snapshot REC handles under lock, encode outside, write-back counters.
 * Keeps capture_begin / stop_record from blocking on codec dequeue.
 */
static void preview_encode_frame(camera_pipeline_t *cam,
		hbn_vnode_image_t *frame)
{
	media_codec_context_t *mc;
	rec_mux_t *mux;
	int64_t pts;
	int frames;
	int fps;
	int anim_n;
	uint16_t *anim;
	char path[256];
	char save_dir[160];
	int abort_reason = 0;

	pthread_mutex_lock(&cam->lock);
	if (!cam->recording || !cam->encode_active || !cam->mc ||
			!cam->rec_mux) {
		pthread_mutex_unlock(&cam->lock);
		return;
	}
	mc = cam->mc;
	mux = cam->rec_mux;
	pts = cam->record_pts;
	frames = cam->record_frames;
	fps = cam->video_fps > 0 ? cam->video_fps : 30;
	anim = cam->anim_frames;
	anim_n = cam->anim_nframes;
	snprintf(path, sizeof(path), "%s", cam->last_path);
	snprintf(save_dir, sizeof(save_dir), "%s", cam->save_dir);
	cam->encode_busy = 1;
	pthread_mutex_unlock(&cam->lock);

	(void)encode_one_frame(mc, mux, frame, &pts, &frames, fps, path,
		save_dir, anim, &anim_n, &abort_reason);

	pthread_mutex_lock(&cam->lock);
	if (cam->mc == mc && cam->encode_active) {
		cam->record_pts = pts;
		cam->record_frames = frames;
		cam->anim_nframes = anim_n;
		if (abort_reason)
			rec_request_abort_locked(cam, abort_reason);
	}
	cam->encode_busy = 0;
	pthread_mutex_unlock(&cam->lock);
}

/*
 * Grab one queued capture from the current VSE frame (pack NV12 + thumb).
 * Must NOT encode here — JPEG holds the VSE dmabuf too long and can fault.
 * Encode runs in capture_encode_pending() AFTER releaseframe.
 * Called with cam->lock held.
 */
static void handle_capture_grab_locked(camera_pipeline_t *cam,
		hbn_vnode_image_t *frame)
{
	uint8_t *y;
	uint8_t *uv;
	uint8_t *copy = NULL;
	size_t y_sz, uv_sz, total;
	int w, h;
	char wtext[64];
	char path[256];

	if (cam->capture_req_count <= 0 || cam->capture_encoding ||
	    cam->capture_nv12)
		return;

	snprintf(path, sizeof(path), "%s",
		cam->capture_req[cam->capture_req_head]);
	cam->capture_req_head = (cam->capture_req_head + 1) % CAM_CAP_Q;
	cam->capture_req_count--;

	w = (int)frame->buffer.width;
	h = (int)frame->buffer.height;
	y_sz = (size_t)w * (size_t)h;
	uv_sz = y_sz / 2;
	total = y_sz + uv_sz;

	for (int j = 0; j < 2; j++)
		hb_mem_invalidate_buf_with_vaddr(
			(uint64_t)frame->buffer.virt_addr[j],
			frame->buffer.size[j]);

	copy = malloc(total);
	if (!copy)
		goto fail;

	y = copy;
	uv = copy + y_sz;
	if (nv12_pack_graphic(&frame->buffer, y, uv, w, h) != 0) {
		fprintf(stderr, "[cam] capture pack failed (%dx%d stride=%d)\n",
			w, h, frame->buffer.stride);
		free(copy);
		goto fail;
	}

	/* Cream first, then timestamp on the caption matte. */
	media_still_nv12_polaroid_frame(y, uv, w, h, w);
	if (watermark_enabled()) {
		make_watermark_text(wtext, sizeof(wtext));
		media_still_nv12_watermark(y, uv, w, h, wtext);
	}

	media_still_nv12_thumb(y, uv, w, h, cam->capture_enc_thumb);
	snprintf(cam->capture_enc_path, sizeof(cam->capture_enc_path), "%s",
		path);
	cam->capture_nv12_w = w;
	cam->capture_nv12_h = h;
	cam->capture_nv12 = copy;
	cam->capture_encoding = 1;

	printf("[cam] capture pack %dx%d stride=%d vstride=%d\n",
		w, h, frame->buffer.stride, frame->buffer.vstride);
	fflush(stdout);
	return;

fail:
	if (cam->capture_done_count < CAM_CAP_Q) {
		snprintf(cam->capture_done_path[cam->capture_done_tail],
			sizeof(cam->capture_done_path[0]), "%s", path);
		cam->capture_done_ok[cam->capture_done_tail] = 0;
		cam->capture_done_tail =
			(cam->capture_done_tail + 1) % CAM_CAP_Q;
		cam->capture_done_count++;
	}
	pthread_cond_broadcast(&cam->capture_cond);
}

/* JPEG + sidecars for a previously packed still. No VSE frame held. */
static void capture_encode_pending(camera_pipeline_t *cam)
{
	uint8_t *copy;
	uint8_t *y;
	uint8_t *uv;
	char path[256];
	uint16_t pack_thumb[CAM_THUMB_W * CAM_THUMB_H];
	uint16_t thumb[CAM_THUMB_W * CAM_THUMB_H];
	int w, h;
	int quality;
	int ret;
	int tw = CAM_THUMB_W;
	int th = CAM_THUMB_H;

	pthread_mutex_lock(&cam->lock);
	if (!cam->capture_nv12) {
		pthread_mutex_unlock(&cam->lock);
		return;
	}
	copy = cam->capture_nv12;
	w = cam->capture_nv12_w;
	h = cam->capture_nv12_h;
	snprintf(path, sizeof(path), "%s", cam->capture_enc_path);
	memcpy(pack_thumb, cam->capture_enc_thumb, sizeof(pack_thumb));
	cam->capture_nv12 = NULL;
	pthread_mutex_unlock(&cam->lock);

	y = copy;
	uv = copy + (size_t)w * (size_t)h;
	quality = env_u32_in_range("CAM_JPEG_QUALITY", 85, 1, 100);
	/* Border + watermark already applied at pack time. */
	ret = media_still_encode_jpeg(y, uv, w, h, w, quality, path);
	if (ret == 0) {
		const char *rv = getenv("CAM_VIDEO_ROTATE");
		int deg = rv && rv[0] ? atoi(rv) : 90;

		(void)media_still_write_rgb565_preview(y, uv, w, h, w,
			path, CAM_PREV_W, CAM_PREV_H);
		/* Gallery list thumb stays sensor-native 160x90. */
		(void)media_still_write_list_thumb(path, pack_thumb);
		/*
		 * Eject card: scale from encoded JPEG (already CAM_VIDEO_ROTATE)
		 * so orientation matches the saved file / album — not a CPU guess.
		 */
		if (deg == 90 || deg == 270) {
			tw = CAM_THUMB_H;
			th = CAM_THUMB_W;
		}
		if (media_still_decode_jpeg_rgb565(path, thumb, tw, th) != 0) {
			fprintf(stderr,
				"[cam] eject thumb from JPEG failed, keep pack thumb\n");
			memcpy(thumb, pack_thumb, sizeof(thumb));
			tw = CAM_THUMB_W;
			th = CAM_THUMB_H;
		} else {
			printf("[cam] eject thumb from JPEG %dx%d (deg=%d)\n",
				tw, th, deg);
		}
		printf("[cam] Captured JPEG: %s (%dx%d q=%d)\n", path, w, h,
			quality);
	} else {
		fprintf(stderr, "[cam] JPEG capture failed: %s (0x%x)\n",
			path, ret);
		memcpy(thumb, pack_thumb, sizeof(thumb));
	}
	fflush(stdout);
	free(copy);

	pthread_mutex_lock(&cam->lock);
	if (ret == 0) {
		memcpy(cam->last_thumb, thumb, sizeof(thumb));
		cam->last_thumb_w = tw;
		cam->last_thumb_h = th;
		cam->last_thumb_valid = 1;
		cam->capture_seq++;
		snprintf(cam->last_path, sizeof(cam->last_path), "%s", path);
	}
	if (cam->capture_done_count < CAM_CAP_Q) {
		snprintf(cam->capture_done_path[cam->capture_done_tail],
			sizeof(cam->capture_done_path[0]), "%s", path);
		cam->capture_done_ok[cam->capture_done_tail] = (ret == 0);
		cam->capture_done_tail =
			(cam->capture_done_tail + 1) % CAM_CAP_Q;
		cam->capture_done_count++;
	} else {
		fprintf(stderr, "[cam] capture done queue full, drop %s\n",
			path);
	}
	cam->capture_encoding = 0;
	pthread_cond_broadcast(&cam->capture_cond);
	pthread_mutex_unlock(&cam->lock);
}

static void *preview_thread_fn(void *arg)
{
	camera_pipeline_t *cam = arg;
	hbn_vnode_image_t frame = {0};
	int ret;
	unsigned long ok = 0;
	unsigned long fail_get = 0;
	unsigned long fail_disp = 0;

	while (cam->running) {
		int hs;
		int vs;
		int use_still;
		int still_idx = -1;
		int do_disp;

		ret = hbn_vnode_getframe(cam->pipe.vse_node_handle, 0, 1000,
				&frame);
		if (ret != 0) {
			fail_get++;
			if (fail_get == 1 || (fail_get % 30) == 0)
				fprintf(stderr,
					"[cam] getframe miss x%lu (last=0x%x)\n",
					fail_get, ret);
			continue;
		}

		ok++;
		if (ok == 1)
			printf("[cam] first VSE frame ok (%dx%d, display_ready=%d)\n",
				(int)frame.buffer.width,
				(int)frame.buffer.height,
				cam->display_ready);

		hs = video_plane_h_unshift_px(cam->video_w);
		vs = video_plane_v_unshift_px(cam->video_h);
		use_still = (hs != 0 || vs != 0);
		do_disp = cam->display_ready && cam->preview_live &&
			!cam->still_hold;

		if (do_disp) {
			int fw = (int)frame.buffer.width;
			int fh = (int)frame.buffer.height;

			for (int j = 0; j < 2; j++)
				hb_mem_invalidate_buf_with_vaddr(
					(uint64_t)frame.buffer.virt_addr[j],
					frame.buffer.size[j]);

			/*
			 * Unshift path: copy into still_buf first so VSE can be
			 * released before the (slow) DRM commit. Holding ochn
			 * buffers across display stalls the flyby path permanently.
			 */
			if (use_still)
				still_idx = live_preview_prepare_still(cam,
					&frame);

			/*
			 * Capture grab stays under lock (short). REC codec/mux runs
			 * outside via preview_encode_frame so UI is not blocked on
			 * ~1s dequeue / disk write.
			 */
			pthread_mutex_lock(&cam->lock);
			if (cam->capture_req_count > 0)
				handle_capture_grab_locked(cam, &frame);
			pthread_mutex_unlock(&cam->lock);
			preview_encode_frame(cam, &frame);

			if (use_still) {
				hbn_vnode_releaseframe(cam->pipe.vse_node_handle,
					0, &frame);
				if (still_idx >= 0) {
					ret = live_preview_commit_buf(cam,
						&cam->still_buf[still_idx]);
					if (ret == 0) {
						pthread_mutex_lock(&cam->lock);
						if (cam->still_buf_ok > 1)
							cam->still_idx =
								(still_idx + 1) %
								cam->still_buf_ok;
						pthread_mutex_unlock(&cam->lock);
					}
				} else {
					ret = -1;
				}
			} else {
				ret = live_preview_commit_buf(cam,
					&frame.buffer);
				hbn_vnode_releaseframe(cam->pipe.vse_node_handle,
					0, &frame);
			}

			if (ret != 0) {
				fail_disp++;
				if (fail_disp == 1 || (fail_disp % 30) == 0)
					fprintf(stderr,
						"[cam] display commit fail x%lu\n",
						fail_disp);
			} else {
				static int first_disp;

				if (!first_disp) {
					first_disp = 1;
					printf("[cam] first preview frame on screen "
						"(%dx%d)\n", fw, fh);
				}
			}
		} else {
			pthread_mutex_lock(&cam->lock);
			if (cam->capture_req_count > 0)
				handle_capture_grab_locked(cam, &frame);
			pthread_mutex_unlock(&cam->lock);
			preview_encode_frame(cam, &frame);
			hbn_vnode_releaseframe(cam->pipe.vse_node_handle, 0,
				&frame);
		}

		/* Encode after release so VSE dmabuf is never held across JPEG. */
		capture_encode_pending(cam);
	}
	return NULL;
}

/*
 * Tear down whatever init (or a live session) managed to start.
 * Safe on half-failed create: skips mutex/thread/DRM until their stage flags
 * are set. Used by normal exit and every init failure path.
 */
static void pipeline_unwind(camera_pipeline_t *cam)
{
	int remux_busy;

	if (!cam)
		return;

	cam->running = 0;
	cam->preview_live = 0;

	if (cam->preview_thread_started) {
		pthread_join(cam->preview_thread, NULL);
		cam->preview_thread_started = 0;
		cam->preview_thread = 0;
	}

	if (cam->sync_inited)
		camera_pipeline_stop_video(cam);

	if (cam->sync_inited) {
		pthread_mutex_lock(&cam->lock);
		if (cam->capture_nv12) {
			free(cam->capture_nv12);
			cam->capture_nv12 = NULL;
			cam->capture_encoding = 0;
		}
		pthread_mutex_unlock(&cam->lock);
	}

	if (cam->sync_inited && camera_pipeline_is_recording(cam))
		(void)camera_pipeline_stop_record_begin(cam);

	if (cam->sync_inited) {
		for (;;) {
			pthread_mutex_lock(&cam->lock);
			remux_busy = (cam->remux_job == 1);
			pthread_mutex_unlock(&cam->lock);
			if (!remux_busy)
				break;
			usleep(20 * 1000);
		}

		pthread_mutex_lock(&cam->lock);
		if (cam->recording)
			encode_teardown_locked(cam);
		pthread_mutex_unlock(&cam->lock);
	}

	if (cam->pipe.vflow_fd >= 0)
		(void)hbn_vflow_stop(cam->pipe.vflow_fd);

	if (cam->display_ready) {
		vp_display_deinit(&cam->drm);
		cam->display_ready = 0;
	}

	if (cam->still_buf_ok) {
		free_still_bufs_locked(cam);
		cam->still_hold = 0;
	}

	free(cam->anim_frames);
	cam->anim_frames = NULL;
	cam->anim_nframes = 0;

	if (cam->pipe.vse_node_handle >= 0) {
		(void)hbn_vnode_close(cam->pipe.vse_node_handle);
		cam->pipe.vse_node_handle = -1;
	}
	if (cam->pipe.isp_node_handle >= 0) {
		(void)hbn_vnode_close(cam->pipe.isp_node_handle);
		cam->pipe.isp_node_handle = -1;
	}
	if (cam->pipe.vin_node_handle >= 0) {
		(void)hbn_vnode_close(cam->pipe.vin_node_handle);
		cam->pipe.vin_node_handle = -1;
	}
	if (cam->pipe.cam_fd >= 0) {
		(void)hbn_camera_destroy(cam->pipe.cam_fd);
		cam->pipe.cam_fd = -1;
	}
	if (cam->pipe.vflow_fd >= 0) {
		(void)hbn_vflow_destroy(cam->pipe.vflow_fd);
		cam->pipe.vflow_fd = -1;
	}

	if (cam->hb_mem_opened) {
		hb_mem_module_close();
		cam->hb_mem_opened = 0;
	}

	if (cam->sync_inited) {
		pthread_mutex_destroy(&cam->lock);
		pthread_cond_destroy(&cam->capture_cond);
		cam->sync_inited = 0;
	}
}

camera_pipeline_t *camera_pipeline_create(int sensor_index)
{
	camera_pipeline_t *cam;

	cam = calloc(1, sizeof(*cam));
	if (!cam) {
		fprintf(stderr, "[cam] calloc(%zu) failed\n", sizeof(*cam));
		return NULL;
	}
	printf("[cam] pipeline heap %zu bytes (create)\n", sizeof(*cam));
	fflush(stdout);
	if (camera_pipeline_init(cam, sensor_index) != 0) {
		/* init already unwound; free the shell only. */
		free(cam);
		return NULL;
	}
	return cam;
}

void camera_pipeline_destroy(camera_pipeline_t *cam)
{
	if (!cam)
		return;
	camera_pipeline_stop(cam);
	free(cam);
}

int camera_pipeline_init(camera_pipeline_t *cam, int sensor_index)
{
	int active_mipi_host;
	int ret;

	if (!cam || sensor_index < 0 ||
			sensor_index >= (int)vp_get_sensors_list_number())
		return -1;

	/*
	 * Do not free(anim_frames) before zeroing on a fresh calloc: a stale
	 * main.o with a smaller sizeof could leave that field outside the
	 * allocated chunk (wild free → sysmalloc abort).
	 */
	if (cam->still_canary == CAM_STILL_CANARY)
		free(cam->anim_frames);
	memset(cam, 0, sizeof(*cam));
	cam->sensor_index = sensor_index;
	cam->pipe.vflow_fd = -1;
	cam->pipe.vin_node_handle = -1;
	cam->pipe.isp_node_handle = -1;
	cam->pipe.vse_node_handle = -1;
	cam->pipe.cam_fd = -1;
	ensure_save_dir(cam);

	printf("[cam] save_dir=%s\n", cam->save_dir);
	fflush(stdout);

	set_env_default_if_absent("CAM_QUICKSTART_SC230AI_DELAY_SCALE", "10");
	set_env_default_if_absent("VP_QUICKSTART_VSE_BUF_NUM", "4");

	printf("[cam] hb_mem_module_open...\n");
	fflush(stdout);
	hb_mem_module_open();
	cam->hb_mem_opened = 1;
	printf("[cam] hb_mem_module_open ok\n");
	fflush(stdout);

	cam->pipe.sensor_config = vp_sensor_config_list[sensor_index];
	printf("Camera sensor: %s (%s)\n",
		cam->pipe.sensor_config->sensor_name,
		cam->pipe.sensor_config->config_file);

	ret = vp_sensor_fixed_mipi_host(cam->pipe.sensor_config,
			&cam->pipe.csi_config);
	if (ret != 0) {
		fprintf(stderr, "Sensor detect failed; check SC230AI wiring\n");
		pipeline_unwind(cam);
		return -1;
	}
	active_mipi_host =
		cam->pipe.sensor_config->vin_node_attr->cim_attr.mipi_rx;

	if ((cam->pipe.sensor_config->camera_config->sensor_mode == DOL2_M) &&
			(vin_isp_is_online == 0)) {
		fprintf(stderr, "DOL2 sensor requires online VIN->ISP mode\n");
		pipeline_unwind(cam);
		return -1;
	}

	printf("Camera bind: vin_isp_online=%d isp_vse_online=%d mipi_rx=%d "
		"vse_buf=%d\n",
		vin_isp_is_online, isp_vse_is_online, active_mipi_host,
		env_u32_in_range("VP_QUICKSTART_VSE_BUF_NUM", 4, 2, 6));

	ret = create_and_run_vflow(&cam->pipe, active_mipi_host);
	if (ret != 0) {
		pipeline_unwind(cam);
		return ret;
	}

	pthread_mutex_init(&cam->lock, NULL);
	pthread_cond_init(&cam->capture_cond, NULL);
	cam->sync_inited = 1;

	/*
	 * Drain VSE immediately after hbn_vflow_start. Waiting for DRM/LVGL
	 * init with only a few ochn buffers can stall the flyby path so
	 * getframe never succeeds.
	 */
	cam->running = 1;
	ret = pthread_create(&cam->preview_thread, NULL, preview_thread_fn, cam);
	if (ret != 0) {
		cam->running = 0;
		fprintf(stderr, "preview thread create failed: %d\n", ret);
		pipeline_unwind(cam);
		return -1;
	}
	cam->preview_thread_started = 1;
	printf("[cam] VSE drain started (before display/UI init)\n");

	ret = display_init_mipi(cam);
	if (ret != 0) {
		pipeline_unwind(cam);
		return ret;
	}

	cam->video_w = (int)cam->pipe.sensor_config->isp_attr->crop.w;
	cam->video_h = (int)cam->pipe.sensor_config->isp_attr->crop.h;
	cam->video_fps = cam->pipe.sensor_config->camera_config->fps;
	if (cam->video_fps <= 0)
		cam->video_fps = 30;

	printf("Camera pipeline ready (%dx%d -> MIPI plane 33)\n",
		cam->video_w, cam->video_h);
	printf("Save directory: %s\n", cam->save_dir);
	return 0;
}

int camera_pipeline_get_video_info(camera_pipeline_t *cam, int *w, int *h,
		int *fps)
{
	if (!cam)
		return -1;
	if (w)
		*w = cam->video_w;
	if (h)
		*h = cam->video_h;
	if (fps)
		*fps = cam->video_fps;
	return 0;
}

int camera_pipeline_get_record_frames(camera_pipeline_t *cam)
{
	return cam ? cam->record_frames : 0;
}

const char *camera_pipeline_save_dir(camera_pipeline_t *cam)
{
	return cam ? cam->save_dir : "";
}

int camera_pipeline_storage_full(camera_pipeline_t *cam)
{
	sys_status_t st;
	int min_mb;

	if (!cam)
		return 0;
	sys_status_poll(&st, cam->save_dir);
	min_mb = env_u32_in_range("CAM_STORAGE_MIN_MB", 64, 1, 1024 * 1024);
	return (st.free_mb >= 0 && st.free_mb < min_mb) ? 1 : 0;
}

int camera_pipeline_rec_abort_reason(camera_pipeline_t *cam)
{
	int reason;

	if (!cam)
		return 0;
	pthread_mutex_lock(&cam->lock);
	reason = cam->rec_abort;
	pthread_mutex_unlock(&cam->lock);
	return reason;
}

int camera_pipeline_get_last_thumb(camera_pipeline_t *cam, uint16_t *dst,
		int *w, int *h, int *seq)
{
	if (!cam || !dst)
		return -1;
	pthread_mutex_lock(&cam->lock);
	if (!cam->last_thumb_valid) {
		pthread_mutex_unlock(&cam->lock);
		return -1;
	}
	memcpy(dst, cam->last_thumb, sizeof(cam->last_thumb));
	if (w)
		*w = cam->last_thumb_w > 0 ? cam->last_thumb_w : CAM_THUMB_W;
	if (h)
		*h = cam->last_thumb_h > 0 ? cam->last_thumb_h : CAM_THUMB_H;
	if (seq)
		*seq = cam->capture_seq;
	pthread_mutex_unlock(&cam->lock);
	return 0;
}

int camera_pipeline_start_preview(camera_pipeline_t *cam)
{
	int hs;
	int vs;

	/*
	 * Thread already drains VSE from camera_pipeline_init. Only enable
	 * DRM commits here — after LVGL/UI create — so unshift+display cannot
	 * starve ochn buffers during boot.
	 */
	if (!cam || !cam->running) {
		fprintf(stderr, "[cam] preview thread not running\n");
		return -1;
	}
	if (!cam->display_ready) {
		fprintf(stderr, "[cam] display not ready for live preview\n");
		return -1;
	}

	hs = video_plane_h_unshift_px(cam->video_w);
	vs = video_plane_v_unshift_px(cam->video_h);
	if (hs != 0 || vs != 0) {
		pthread_mutex_lock(&cam->lock);
		(void)ensure_still_buf_locked(cam);
		pthread_mutex_unlock(&cam->lock);
	}

	cam->preview_live = 1;
	printf("[cam] live preview enabled (after UI init)\n");
	fflush(stdout);
	return 0;
}

void camera_pipeline_set_ui_overlay(camera_pipeline_t *cam, drm_ui_plane_t *ui)
{
	if (!cam)
		return;
	cam->ui_overlay = ui;
}

int camera_pipeline_capture_begin(camera_pipeline_t *cam, char *out_path,
		size_t out_len)
{
	char path[256];

	if (!cam || !cam->running)
		return -1;
	if (storage_guard(cam) != 0)
		return -2;

	pthread_mutex_lock(&cam->lock);
	if (cam->capture_req_count >= CAM_CAP_Q) {
		pthread_mutex_unlock(&cam->lock);
		return -1;
	}
	make_timestamped_path(cam, "capture", "jpg", path, sizeof(path));
	snprintf(cam->capture_req[cam->capture_req_tail],
		sizeof(cam->capture_req[0]), "%s", path);
	cam->capture_req_tail = (cam->capture_req_tail + 1) % CAM_CAP_Q;
	cam->capture_req_count++;
	snprintf(cam->last_path, sizeof(cam->last_path), "%s", path);
	if (out_path && out_len)
		snprintf(out_path, out_len, "%s", path);
	pthread_mutex_unlock(&cam->lock);
	return 0;
}

int camera_pipeline_capture_poll(camera_pipeline_t *cam, char *out_path,
		size_t out_len)
{
	int ok;
	int ret;

	if (!cam)
		return -1;

	pthread_mutex_lock(&cam->lock);
	if (cam->capture_done_count > 0) {
		ok = cam->capture_done_ok[cam->capture_done_head];
		if (out_path && out_len)
			snprintf(out_path, out_len, "%s",
				cam->capture_done_path[cam->capture_done_head]);
		cam->capture_done_head =
			(cam->capture_done_head + 1) % CAM_CAP_Q;
		cam->capture_done_count--;
		ret = ok ? 0 : -1;
		pthread_mutex_unlock(&cam->lock);
		return ret;
	}
	if (cam->capture_req_count > 0 || cam->capture_encoding) {
		pthread_mutex_unlock(&cam->lock);
		return 1;
	}
	pthread_mutex_unlock(&cam->lock);
	return 2; /* idle */
}

int camera_pipeline_capture(camera_pipeline_t *cam, char *out_path,
		size_t out_len)
{
	struct timespec ts;
	int ret = -1;

	ret = camera_pipeline_capture_begin(cam, out_path, out_len);
	if (ret != 0)
		return ret;

	pthread_mutex_lock(&cam->lock);
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += 8;
	while (cam->capture_done_count == 0) {
		if (cam->capture_req_count == 0 && !cam->capture_encoding)
			break;
		ret = pthread_cond_timedwait(&cam->capture_cond, &cam->lock,
			&ts);
		if (ret == ETIMEDOUT)
			break;
	}
	if (cam->capture_done_count > 0) {
		ret = cam->capture_done_ok[cam->capture_done_head] ? 0 : -1;
		if (out_path && out_len)
			snprintf(out_path, out_len, "%s",
				cam->capture_done_path[cam->capture_done_head]);
		cam->capture_done_head =
			(cam->capture_done_head + 1) % CAM_CAP_Q;
		cam->capture_done_count--;
	} else {
		ret = -1;
	}
	pthread_mutex_unlock(&cam->lock);
	return ret;
}

int camera_pipeline_start_record(camera_pipeline_t *cam, char *out_path,
		size_t out_len)
{
	media_codec_context_t *mc = NULL;
	mc_av_codec_startup_params_t startup = {0};
	vse_ochn_attr_t ochn = {0};
	rec_mux_t *mux = NULL;
	char path[256];
	uint16_t *anim = NULL;
	int w, h, fps, bitrate;
	int ret;

	if (!cam || !cam->running) {
		fprintf(stderr, "[cam] REC start: pipeline not running\n");
		return -1;
	}
	if (storage_guard(cam) != 0)
		return -2;

	pthread_mutex_lock(&cam->lock);
	if (cam->recording || cam->remux_job == 1 || cam->rec_starting) {
		pthread_mutex_unlock(&cam->lock);
		fprintf(stderr, "[cam] REC start: already recording/remuxing\n");
		return -1;
	}
	cam->rec_starting = 1;
	w = cam->video_w;
	h = cam->video_h;
	fps = cam->pipe.sensor_config->camera_config->fps;
	pthread_mutex_unlock(&cam->lock);

	ret = hbn_vnode_get_ochn_attr(cam->pipe.vse_node_handle, 0, &ochn);
	if (ret == 0 && ochn.target_w > 0 && ochn.target_h > 0) {
		w = ochn.target_w;
		h = ochn.target_h;
	} else if (w <= 0 || h <= 0) {
		pthread_mutex_lock(&cam->lock);
		cam->rec_starting = 0;
		pthread_mutex_unlock(&cam->lock);
		fprintf(stderr, "[cam] REC start: get_ochn_attr failed (0x%x) "
			"and no video size\n", ret);
		return -1;
	} else {
		fprintf(stderr, "[cam] REC start: ochn attr fail 0x%x, "
			"using video %dx%d\n", ret, w, h);
	}
	if (fps <= 0)
		fps = 30;
	bitrate = env_u32_in_range("CAM_REC_BITRATE", 4096, 512, 16384);

	/*
	 * Mux open + codec init outside cam->lock — can take tens/hundreds of
	 * ms. Preview encode and capture_begin only need the lock briefly.
	 */
	make_timestamped_path(cam, "record", "mp4", path, sizeof(path));
	mux = rec_mux_open(path, w, h, fps);
	if (!mux) {
		pthread_mutex_lock(&cam->lock);
		cam->rec_starting = 0;
		pthread_mutex_unlock(&cam->lock);
		fprintf(stderr, "[cam] REC open mux %s failed\n", path);
		return -1;
	}

	mc = calloc(1, sizeof(*mc));
	if (!mc) {
		rec_mux_close(mux);
		pthread_mutex_lock(&cam->lock);
		cam->rec_starting = 0;
		pthread_mutex_unlock(&cam->lock);
		fprintf(stderr, "[cam] REC calloc encoder failed\n");
		return -1;
	}

	ret = encode_config_h264(mc, w, h, fps, (uint32_t)bitrate);
	if (ret == 0)
		ret = hb_mm_mc_initialize(mc);
	if (ret == 0)
		ret = hb_mm_mc_configure(mc);
	if (ret == 0)
		ret = hb_mm_mc_start(mc, &startup);
	if (ret != 0) {
		fprintf(stderr, "[cam] H264 encoder init failed: 0x%x "
			"(%dx%d @%dfps %dkbps)\n",
			ret, w, h, fps, bitrate);
		(void)hb_mm_mc_release(mc);
		free(mc);
		rec_mux_close(mux);
		unlink(path);
		pthread_mutex_lock(&cam->lock);
		cam->rec_starting = 0;
		pthread_mutex_unlock(&cam->lock);
		return -1;
	}

	if (!cam->anim_frames) {
		size_t px = (size_t)CAM_THUMB_W * (size_t)CAM_THUMB_H;

		anim = calloc((size_t)CAM_ANIM_MAX_FRAMES * px, sizeof(uint16_t));
		if (!anim)
			fprintf(stderr, "[cam] REC anim thumb alloc failed\n");
	}

	pthread_mutex_lock(&cam->lock);
	cam->rec_starting = 0;
	if (!cam->running || cam->recording) {
		pthread_mutex_unlock(&cam->lock);
		(void)hb_mm_mc_pause(mc);
		(void)hb_mm_mc_release(mc);
		free(mc);
		rec_mux_close(mux);
		unlink(path);
		free(anim);
		fprintf(stderr, "[cam] REC start aborted (pipeline gone)\n");
		return -1;
	}
	if (anim && !cam->anim_frames)
		cam->anim_frames = anim;
	else
		free(anim);
	cam->mc = mc;
	cam->rec_mux = mux;
	snprintf(cam->last_path, sizeof(cam->last_path), "%s", path);
	snprintf(cam->h264_path, sizeof(cam->h264_path), "%s", path);
	cam->record_frames = 0;
	cam->record_pts = 0;
	cam->rec_abort = 0;
	cam->anim_nframes = 0;
	cam->encode_active = 1;
	cam->recording = 1;
	if (out_path && out_len)
		snprintf(out_path, out_len, "%s", path);
	pthread_mutex_unlock(&cam->lock);

	printf("[cam] REC start: %s (live mux, %dx%d @%dfps, %dkbps)\n",
		path, w, h, fps, bitrate);
	fflush(stdout);
	return 0;
}

static void *remux_thread_fn(void *arg)
{
	camera_pipeline_t *cam = arg;
	rec_mux_t *mux;
	char mp4_path[256];
	int frames;

	if (!cam)
		return NULL;

	pthread_mutex_lock(&cam->lock);
	mux = cam->rec_mux;
	cam->rec_mux = NULL;
	frames = cam->remux_frames;
	snprintf(mp4_path, sizeof(mp4_path), "%s", cam->remux_mp4);
	pthread_mutex_unlock(&cam->lock);

	/* Encoder already stopped; only write MP4 trailer / close. */
	if (mux)
		rec_mux_close(mux);

	pthread_mutex_lock(&cam->lock);
	if (frames > 0) {
		snprintf(cam->last_path, sizeof(cam->last_path), "%s", mp4_path);
		cam->remux_result = 0;
		cam->remux_job = 2;
		printf("[cam] REC stop: %s (%d frames, live MP4)\n",
			mp4_path, frames);
	} else {
		unlink(mp4_path);
		cam->last_path[0] = '\0';
		cam->remux_result = -1;
		cam->remux_job = 3;
		fprintf(stderr, "[cam] REC stop: no frames, removed %s\n",
			mp4_path);
	}
	fflush(stdout);
	pthread_mutex_unlock(&cam->lock);
	return NULL;
}

int camera_pipeline_stop_record_begin(camera_pipeline_t *cam)
{
	int ret;
	char path[256];
	uint16_t *anim = NULL;
	int anim_n = 0;
	int dur_sec = 0;
	media_codec_context_t *mc;

	if (!cam)
		return -1;

	pthread_mutex_lock(&cam->lock);
	if (!cam->recording) {
		pthread_mutex_unlock(&cam->lock);
		return -1;
	}
	if (cam->remux_job == 1) {
		pthread_mutex_unlock(&cam->lock);
		return -1;
	}

	/*
	 * Snapshot remux + sidecar inputs under lock, then stop encode.
	 * .a565 / duration fwrite happens after unlock so preview/capture
	 * are not stalled (~432KB anim thumb).
	 */
	snprintf(cam->remux_mp4, sizeof(cam->remux_mp4), "%s", cam->last_path);
	snprintf(path, sizeof(path), "%s", cam->last_path);
	cam->remux_frames = cam->record_frames;
	cam->remux_fps = cam->video_fps > 0 ? cam->video_fps : 30;
	cam->remux_result = -1;
	cam->remux_job = 1;
	{
		int fps_r = cam->video_fps > 0 ? cam->video_fps : 30;

		/* Round up so UI duration is not shorter than real clip length. */
		dur_sec = (cam->record_frames + fps_r - 1) / fps_r;
		if (dur_sec < 1 && cam->record_frames > 0)
			dur_sec = 1;
	}
	anim = cam->anim_frames;
	anim_n = cam->anim_nframes;
	cam->anim_nframes = 0;

	cam->recording = 0;
	cam->encode_active = 0;
	cam->rec_abort = 0;
	while (cam->encode_busy) {
		pthread_mutex_unlock(&cam->lock);
		usleep(1000);
		pthread_mutex_lock(&cam->lock);
	}
	mc = cam->mc;
	cam->mc = NULL;
	if (mc) {
		(void)hb_mm_mc_pause(mc);
		(void)hb_mm_mc_release(mc);
		free(mc);
	}
	pthread_mutex_unlock(&cam->lock);

	if (anim && anim_n > 0) {
		(void)media_still_write_anim_thumb(path, anim, anim_n,
			CAM_ANIM_FPS);
	} else {
		printf("[cam] REC stop: no anim thumb frames (n=%d)\n", anim_n);
		fflush(stdout);
	}
	(void)media_still_write_video_duration(path, dur_sec);

	ret = pthread_create(&cam->remux_thread, NULL, remux_thread_fn, cam);
	if (ret != 0) {
		rec_mux_t *mux;

		fprintf(stderr, "[cam] finalize thread create failed: %d\n", ret);
		pthread_mutex_lock(&cam->lock);
		mux = cam->rec_mux;
		cam->rec_mux = NULL;
		cam->remux_job = 3;
		cam->remux_result = -1;
		pthread_mutex_unlock(&cam->lock);
		if (mux)
			rec_mux_close(mux);
		return -1;
	}
	pthread_detach(cam->remux_thread);
	cam->remux_thread = 0;
	return 0;
}

int camera_pipeline_stop_record_poll(camera_pipeline_t *cam, char *out_path,
		size_t out_len)
{
	int job;
	int ret;

	if (!cam)
		return -1;

	pthread_mutex_lock(&cam->lock);
	job = cam->remux_job;
	if (job == 1) {
		pthread_mutex_unlock(&cam->lock);
		return 1;
	}
	if (job == 2) {
		ret = 0;
		if (out_path && out_len)
			snprintf(out_path, out_len, "%s", cam->last_path);
		cam->remux_job = 0;
	} else if (job == 3) {
		ret = -1;
		if (out_path && out_len)
			snprintf(out_path, out_len, "%s", cam->last_path);
		cam->remux_job = 0;
	} else {
		ret = -1;
	}
	pthread_mutex_unlock(&cam->lock);
	return ret;
}

int camera_pipeline_stop_record(camera_pipeline_t *cam, char *out_path,
		size_t out_len)
{
	int ret;

	ret = camera_pipeline_stop_record_begin(cam);
	if (ret != 0)
		return ret;

	for (;;) {
		ret = camera_pipeline_stop_record_poll(cam, out_path, out_len);
		if (ret != 1)
			return ret;
		usleep(20 * 1000);
	}
}

int camera_pipeline_is_recording(camera_pipeline_t *cam)
{
	int on = 0;

	if (!cam)
		return 0;
	pthread_mutex_lock(&cam->lock);
	on = cam->recording;
	pthread_mutex_unlock(&cam->lock);
	return on;
}

int camera_pipeline_is_busy(camera_pipeline_t *cam)
{
	int busy = 0;

	if (!cam)
		return 0;
	pthread_mutex_lock(&cam->lock);
	busy = (cam->capture_req_count > 0) || cam->capture_encoding ||
		(cam->capture_done_count > 0) || (cam->remux_job == 1);
	pthread_mutex_unlock(&cam->lock);
	return busy;
}

static int alloc_one_still_buf(camera_pipeline_t *cam, hb_mem_graphic_buf_t *buf)
{
	int64_t flags;
	int ret;
	int stride;
	int vstride;
	ptrdiff_t uv_off;
	off_t dma_sz;
	size_t need;

	if (!cam || !buf)
		return -1;

	/*
	 * Tight NV12 for DRM: UV @ stride*height (not stride*align32).
	 * A 1080→1088 vstride left 8 Y-pad rows between luma and chroma;
	 * on the rot180 panel that often shows as a green strip at the
	 * user-visible bottom edge. Live VSE can use aligned vstride; our
	 * CPU-blitted still/play path does not need it.
	 */
	stride = cam->video_w;
	vstride = cam->video_h;
	flags = HB_MEM_USAGE_MAP_INITIALIZED |
		HB_MEM_USAGE_CPU_READ_OFTEN |
		HB_MEM_USAGE_CPU_WRITE_OFTEN |
		HB_MEM_USAGE_CACHED |
		HB_MEM_USAGE_GRAPHIC_CONTIGUOUS_BUF |
		HB_MEM_USAGE_HW_DISPLAY;
	memset(buf, 0, sizeof(*buf));
	ret = hb_mem_alloc_graph_buf(cam->video_w, cam->video_h,
		MEM_PIX_FMT_NV12, flags, stride, vstride, buf);
	if (ret != 0) {
		/* Fall back to auto stride/vstride, then force tight layout. */
		ret = hb_mem_alloc_graph_buf(cam->video_w, cam->video_h,
			MEM_PIX_FMT_NV12, flags, 0, 0, buf);
	}
	if (ret != 0) {
		fprintf(stderr, "[cam] still buf alloc failed 0x%x (%dx%d)\n",
			ret, cam->video_w, cam->video_h);
		return -1;
	}

	/* Report tight geometry to DRM regardless of allocator padding. */
	buf->width = cam->video_w;
	buf->height = cam->video_h;
	if (buf->stride < cam->video_w)
		buf->stride = cam->video_w;
	buf->vstride = cam->video_h;

	uv_off = 0;
	if (buf->virt_addr[0] && buf->virt_addr[1])
		uv_off = buf->virt_addr[1] - buf->virt_addr[0];

	need = (size_t)buf->stride * (size_t)buf->height * 3 / 2;
	dma_sz = -1;
	if (buf->fd[0] >= 0)
		dma_sz = lseek(buf->fd[0], 0, SEEK_END);

	printf("[cam] still NV12 buf %dx%d stride=%d vstride=%d "
		"plane_cnt=%d fd=%d,%d uv_off=%ld size0=%llu dma_sz=%lld "
		"need=%zu\n",
		buf->width, buf->height, buf->stride, buf->vstride,
		buf->plane_cnt, buf->fd[0], buf->fd[1], (long)uv_off,
		(unsigned long long)buf->size[0], (long long)dma_sz, need);
	fflush(stdout);

	if (dma_sz >= 0 && (size_t)dma_sz < need) {
		fprintf(stderr,
			"[cam] still dma-buf small (%lld < %zu)\n",
			(long long)dma_sz, need);
		(void)hb_mem_free_buf(buf->fd[0]);
		memset(buf, 0, sizeof(*buf));
		return -1;
	}

	/* Neutral fill (Y=16, UV=128) — never leave padding as 0/0/0 green. */
	if (buf->virt_addr[0]) {
		uint8_t *y = buf->virt_addr[0];
		uint8_t *uv = y + (size_t)buf->stride * (size_t)buf->height;
		size_t ysz = (size_t)buf->stride * (size_t)buf->height;
		size_t uvsz = ysz / 2;
		size_t fill = ysz + uvsz;

		if (dma_sz > 0 && (size_t)dma_sz < fill)
			fill = (size_t)dma_sz;
		if (fill >= ysz) {
			memset(y, 0x10, ysz);
			if (fill > ysz)
				memset(uv, 0x80, fill - ysz);
			(void)hb_mem_flush_buf_with_vaddr((uint64_t)y, fill);
		}
	}
	return 0;
}

static int ensure_still_buf_locked(camera_pipeline_t *cam)
{
	int i;

	if (!cam || cam->video_w <= 0 || cam->video_h <= 0)
		return -1;
	if (!cam->still_buf) {
		cam->still_buf = calloc(2, sizeof(*cam->still_buf));
		if (!cam->still_buf) {
			fprintf(stderr, "[cam] still_buf calloc failed\n");
			return -1;
		}
		cam->still_canary = CAM_STILL_CANARY;
		cam->still_buf_ok = 0;
		cam->still_idx = 0;
	}
	if (cam->still_canary != CAM_STILL_CANARY) {
		fprintf(stderr,
			"[cam] FATAL still_canary corrupt 0x%x (heap smash?)\n",
			cam->still_canary);
		return -1;
	}
	if (cam->still_buf_ok >= 2)
		return 0;

	for (i = cam->still_buf_ok; i < 2; i++) {
		if (alloc_one_still_buf(cam, &cam->still_buf[i]) != 0) {
			/* One buffer is enough for JPEG still; play prefers 2. */
			break;
		}
		cam->still_buf_ok = i + 1;
		if (cam->still_canary != CAM_STILL_CANARY) {
			fprintf(stderr,
				"[cam] FATAL still_canary smashed after alloc[%d]\n",
				i);
			return -1;
		}
	}
	return cam->still_buf_ok > 0 ? 0 : -1;
}

static void free_still_bufs_locked(camera_pipeline_t *cam)
{
	int i;

	if (!cam || !cam->still_buf)
		return;
	for (i = 0; i < cam->still_buf_ok && i < 2; i++) {
		if (cam->still_buf[i].fd[0] >= 0)
			(void)hb_mem_free_buf(cam->still_buf[i].fd[0]);
		memset(&cam->still_buf[i], 0, sizeof(cam->still_buf[i]));
	}
	cam->still_buf_ok = 0;
	cam->still_idx = 0;
	free(cam->still_buf);
	cam->still_buf = NULL;
	cam->still_canary = 0;
}

static int copy_packed_nv12_to_graphic(hb_mem_graphic_buf_t *dst,
		const uint8_t *src, int w, int h)
{
	uint8_t *dst_y;
	uint8_t *dst_uv;
	int stride;
	int vstride;
	int row;
	size_t ysz;

	if (!dst || !src || w <= 0 || h <= 0)
		return -1;
	dst_y = dst->virt_addr[0];
	stride = dst->stride > 0 ? dst->stride : w;
	/* Prefer height for UV base — matches tight still_buf DRM layout. */
	vstride = dst->height > 0 ? dst->height : h;
	if (dst->vstride > 0 && dst->vstride < vstride)
		vstride = dst->vstride;
	if (!dst_y)
		return -1;
	dst_uv = dst_y + (size_t)stride * (size_t)vstride;

	for (row = 0; row < h; row++) {
		memcpy(dst_y + (size_t)row * (size_t)stride,
			src + (size_t)row * (size_t)w, (size_t)w);
		if (stride > w)
			memset(dst_y + (size_t)row * (size_t)stride + (size_t)w,
				0x10, (size_t)(stride - w));
	}
	for (row = h; row < vstride; row++)
		memset(dst_y + (size_t)row * (size_t)stride, 0x10,
			(size_t)stride);
	for (row = 0; row < h / 2; row++) {
		memcpy(dst_uv + (size_t)row * (size_t)stride,
			src + (size_t)w * (size_t)h +
				(size_t)row * (size_t)w,
			(size_t)w);
		if (stride > w)
			memset(dst_uv + (size_t)row * (size_t)stride +
					(size_t)w,
				0x80, (size_t)(stride - w));
	}
	for (row = h / 2; row < vstride / 2; row++)
		memset(dst_uv + (size_t)row * (size_t)stride, 0x80,
			(size_t)stride);

	ysz = (size_t)stride * (size_t)vstride;
	(void)hb_mem_flush_buf_with_vaddr((uint64_t)dst_y, ysz + ysz / 2);
	return 0;
}

/* Decoder NV12 may use stride/vstride > width/height and split planes. */
static int copy_strided_nv12_to_graphic(hb_mem_graphic_buf_t *dst,
		const uint8_t *src_y, const uint8_t *src_uv,
		int w, int h, int src_stride, int src_vstride)
{
	uint8_t *dst_y;
	uint8_t *dst_uv;
	int dst_stride;
	int dst_vstride;
	int row;
	int copy_w;
	size_t ysz, uvsz;

	if (!dst || !src_y || w <= 0 || h <= 0)
		return -1;
	if (src_stride < w)
		src_stride = w;
	if (src_vstride < h)
		src_vstride = h;
	if (!src_uv)
		src_uv = src_y + (size_t)src_stride * (size_t)src_vstride;

	dst_y = dst->virt_addr[0];
	dst_stride = dst->stride > 0 ? dst->stride : w;
	/* Tight DRM layout: UV follows active height, not allocator pad. */
	dst_vstride = dst->height > 0 ? dst->height : h;
	if (dst->vstride > 0 && dst->vstride == dst->height)
		dst_vstride = dst->vstride;
	else if (dst->height > 0)
		dst_vstride = dst->height;
	if (!dst_y)
		return -1;
	/*
	 * Always derive UV from Y base + stride*height so the write lands in
	 * the same dma-buf DRM samples (virt_addr[1] can be a separate map
	 * at an aligned offset we intentionally ignore for still/play).
	 */
	dst_uv = dst_y + (size_t)dst_stride * (size_t)dst_vstride;

	copy_w = w;
	for (row = 0; row < h && row < dst_vstride; row++) {
		memcpy(dst_y + (size_t)row * (size_t)dst_stride,
			src_y + (size_t)row * (size_t)src_stride,
			(size_t)copy_w);
		if (dst_stride > copy_w)
			memset(dst_y + (size_t)row * (size_t)dst_stride +
					(size_t)copy_w,
				0x10, (size_t)(dst_stride - copy_w));
	}
	for (row = h; row < dst_vstride; row++)
		memset(dst_y + (size_t)row * (size_t)dst_stride, 0x10,
			(size_t)dst_stride);

	for (row = 0; row < h / 2 && row < dst_vstride / 2; row++) {
		memcpy(dst_uv + (size_t)row * (size_t)dst_stride,
			src_uv + (size_t)row * (size_t)src_stride,
			(size_t)copy_w);
		if (dst_stride > copy_w)
			memset(dst_uv + (size_t)row * (size_t)dst_stride +
					(size_t)copy_w,
				0x80, (size_t)(dst_stride - copy_w));
	}
	for (row = h / 2; row < dst_vstride / 2; row++)
		memset(dst_uv + (size_t)row * (size_t)dst_stride, 0x80,
			(size_t)dst_stride);

	ysz = (size_t)dst_stride * (size_t)dst_vstride;
	uvsz = ysz / 2;
	(void)hb_mem_flush_buf_with_vaddr((uint64_t)dst_y, ysz + uvsz);
	return 0;
}

/*
 * Undo encoder rotate for old 1080x1920 REC: src WxH -> dst HxW.
 *
 * Board: MC_CCW_90 on this NV12 path behaves like CW (same note as
 * media_still polaroid). Undoing CW90 needs CCW90:
 *   (sx,sy) -> (dx,dy) with dx = sy, dy = w-1-sx.
 * (Earlier CW90 undo left the frame 180° wrong: L↔R and T↔B.)
 */
static int copy_strided_nv12_ccw90_to_graphic(hb_mem_graphic_buf_t *dst,
		const uint8_t *src_y, const uint8_t *src_uv,
		int w, int h, int src_stride, int src_vstride)
{
	uint8_t *dst_y;
	uint8_t *dst_uv;
	int dst_stride;
	int dst_vstride;
	int dw, dh;
	int sx, sy, dx, dy;
	size_t ysz;

	if (!dst || !src_y || w <= 0 || h <= 0)
		return -1;
	if (src_stride < w)
		src_stride = w;
	if (src_vstride < h)
		src_vstride = h;
	if (!src_uv)
		src_uv = src_y + (size_t)src_stride * (size_t)src_vstride;

	dw = h;
	dh = w;
	dst_y = dst->virt_addr[0];
	dst_stride = dst->stride > 0 ? dst->stride : dw;
	dst_vstride = dst->height > 0 ? dst->height : dh;
	if (dst->vstride > 0 && dst->vstride == dst->height)
		dst_vstride = dst->vstride;
	else if (dst->height > 0)
		dst_vstride = dst->height;
	if (!dst_y || dst_stride < dw || dst_vstride < dh)
		return -1;
	dst_uv = dst_y + (size_t)dst_stride * (size_t)dst_vstride;

	for (sy = 0; sy < h; sy++) {
		const uint8_t *row = src_y + (size_t)sy * (size_t)src_stride;

		dx = sy;
		for (sx = 0; sx < w; sx++) {
			dy = w - 1 - sx;
			dst_y[(size_t)dy * (size_t)dst_stride + (size_t)dx] =
				row[sx];
		}
	}
	for (sy = h; sy < dst_vstride; sy++)
		memset(dst_y + (size_t)sy * (size_t)dst_stride, 0x10,
			(size_t)dst_stride);
	if (dst_stride > dw) {
		for (sy = 0; sy < dh; sy++)
			memset(dst_y + (size_t)sy * (size_t)dst_stride +
					(size_t)dw,
				0x10, (size_t)(dst_stride - dw));
	}

	for (sy = 0; sy < h / 2; sy++) {
		const uint8_t *row = src_uv + (size_t)sy * (size_t)src_stride;

		dx = sy;
		for (sx = 0; sx < w / 2; sx++) {
			dy = w / 2 - 1 - sx;
			dst_uv[(size_t)dy * (size_t)dst_stride +
				(size_t)dx * 2 + 0] = row[sx * 2 + 0];
			dst_uv[(size_t)dy * (size_t)dst_stride +
				(size_t)dx * 2 + 1] = row[sx * 2 + 1];
		}
	}
	for (sy = h / 2; sy < dst_vstride / 2; sy++)
		memset(dst_uv + (size_t)sy * (size_t)dst_stride, 0x80,
			(size_t)dst_stride);

	ysz = (size_t)dst_stride * (size_t)dst_vstride;
	(void)hb_mem_flush_buf_with_vaddr((uint64_t)dst_y, ysz + ysz / 2);
	return 0;
}

int camera_pipeline_show_nv12_frame(camera_pipeline_t *cam,
		const uint8_t *y, const uint8_t *uv, int width, int height,
		int stride, int vstride)
{
	hb_mem_graphic_buf_t *dst;
	int idx;
	int ret = -1;
	int cw, ch;
	int rotate90 = 0;

	if (!cam || !y || !cam->display_ready || width <= 0 || height <= 0)
		return -1;

	if (stride < width)
		stride = width;
	if (vstride < height)
		vstride = height;

	/*
	 * Do NOT hb_mem_invalidate decoder vir_ptr — VPU frame VAs are often
	 * not registered with hb_mem the same way as graphic allocs. Invalidating
	 * them has caused segfaults and corrupted nearby globals (player stop_req).
	 * CPU memcpy below is enough for display blit into still_buf.
	 */

	pthread_mutex_lock(&cam->lock);
	if (ensure_still_buf_locked(cam) != 0)
		goto out;

	/*
	 * Older REC used encoder CAM_VIDEO_ROTATE=90 → bitstream 1080x1920
	 * while the video plane is 1920x1080. Rotate CCW90 back to plane size
	 * (board MC_CCW_90 encode acts like CW; see media_still).
	 */
	if (width == cam->video_h && height == cam->video_w)
		rotate90 = 1;

	cw = width < cam->video_w ? width : cam->video_w;
	ch = height < cam->video_h ? height : cam->video_h;
	if (!rotate90 && (width != cam->video_w || height != cam->video_h)) {
		static int once;

		if (!once) {
			once = 1;
			fprintf(stderr,
				"[cam] play frame %dx%d != video %dx%d (reject oversized)\n",
				width, height, cam->video_w, cam->video_h);
		}
		if (width > cam->video_w || height > cam->video_h)
			goto out;
	}

	/* Ping-pong so we never write the buffer currently on scanout. */
	idx = cam->still_idx % cam->still_buf_ok;
	dst = &cam->still_buf[idx];
	if (rotate90) {
		static int once_r;

		if (!once_r) {
			once_r = 1;
			printf("[cam] play rotate CCW90 %dx%d -> %dx%d\n",
				width, height, cam->video_w, cam->video_h);
			fflush(stdout);
		}
		if (copy_strided_nv12_ccw90_to_graphic(dst, y, uv, width, height,
				stride, vstride) != 0)
			goto out;
		cw = cam->video_w;
		ch = cam->video_h;
	} else if (copy_strided_nv12_to_graphic(dst, y, uv, cw, ch, stride,
			vstride) != 0) {
		goto out;
	}
	nv12_apply_plane_unshift(dst, cw, ch, cam->video_w, cam->video_h);

	cam->still_hold = 1;
	if (cam->ui_overlay)
		ret = drm_ui_plane_show_video(cam->ui_overlay, &cam->drm, dst);
	else
		ret = vp_display_set_frame(&cam->drm, dst);
	if (ret != 0) {
		fprintf(stderr, "[cam] play display commit fail 0x%x\n", ret);
	} else if (cam->still_buf_ok > 1) {
		cam->still_idx = (idx + 1) % cam->still_buf_ok;
	}

out:
	pthread_mutex_unlock(&cam->lock);
	return ret;
}

int camera_pipeline_show_still_jpeg(camera_pipeline_t *cam, const char *path)
{
	uint8_t *nv12 = NULL;
	int w = 0, h = 0;
	int ret = -1;
	char safe[PATH_MAX];

	if (!cam || !path || !path[0] || !cam->display_ready)
		return -1;
	if (media_still_confine_path(cam->save_dir, path, safe, sizeof(safe),
			0) != 0) {
		fprintf(stderr,
			"[cam] still rejected path outside save_dir: %s\n",
			path);
		return -1;
	}
	path = safe;

	ret = media_still_decode_jpeg_nv12(path, &nv12, &w, &h);
	if (ret != 0 || !nv12)
		return -1;

	pthread_mutex_lock(&cam->lock);
	if (ensure_still_buf_locked(cam) != 0)
		goto out_unlock;
	if (w != cam->video_w || h != cam->video_h) {
		fprintf(stderr,
			"[cam] still size %dx%d != video %dx%d\n",
			w, h, cam->video_w, cam->video_h);
		ret = -1;
		goto out_unlock;
	}
	if (copy_packed_nv12_to_graphic(&cam->still_buf[0], nv12, w, h) != 0) {
		ret = -1;
		goto out_unlock;
	}
	nv12_apply_plane_unshift(&cam->still_buf[0], w, h, cam->video_w,
		cam->video_h);

	cam->still_hold = 1;
	if (cam->ui_overlay)
		ret = drm_ui_plane_show_video(cam->ui_overlay, &cam->drm,
			&cam->still_buf[0]);
	else
		ret = vp_display_set_frame(&cam->drm, &cam->still_buf[0]);
	if (ret != 0) {
		cam->still_hold = 0;
		fprintf(stderr, "[cam] still display commit fail 0x%x\n", ret);
		goto out_unlock;
	}
	printf("[cam] still on video plane: %s (%dx%d)\n", path, w, h);
	fflush(stdout);
	ret = 0;

out_unlock:
	pthread_mutex_unlock(&cam->lock);
	free(nv12);
	return ret;
}

void camera_pipeline_clear_still(camera_pipeline_t *cam)
{
	if (!cam)
		return;
	if (media_player_is_playing())
		media_player_stop(cam);
	pthread_mutex_lock(&cam->lock);
	if (cam->still_hold) {
		cam->still_hold = 0;
		printf("[cam] still cleared — live preview resumes\n");
		fflush(stdout);
	}
	pthread_mutex_unlock(&cam->lock);
}

int camera_pipeline_play_video(camera_pipeline_t *cam, const char *mp4_path)
{
	int ret;
	char poster[512];
	char safe[PATH_MAX];
	int poster_ok = 0;

	if (!cam || !mp4_path || !mp4_path[0] || !cam->display_ready) {
		printf("[cam] play_video reject (cam=%p path=%s ready=%d)\n",
			(void *)cam, mp4_path ? mp4_path : "(null)",
			cam ? cam->display_ready : -1);
		fflush(stdout);
		return -1;
	}
	/* Refuse garbage / ABI leftovers (e.g. fn ptr that prints as "@\\n"). */
	if (mp4_path[0] != '/') {
		fprintf(stderr, "[cam] play rejected bad path (not absolute)\n");
		return -1;
	}
	if (media_still_confine_path(cam->save_dir, mp4_path, safe,
			sizeof(safe), 0) != 0) {
		fprintf(stderr,
			"[cam] play rejected path outside save_dir: %s\n",
			mp4_path);
		return -1;
	}
	mp4_path = safe;

	/* Stop prior play without clearing still_hold (avoids live preview flash). */
	media_player_stop(cam);

	pthread_mutex_lock(&cam->lock);
	/*
	 * Rebuild still bufs if leftover from the old align32 layout
	 * (vstride=1088) — that padding showed as a green edge strip.
	 */
	if (cam->still_buf_ok > 0 && cam->still_buf &&
			(cam->still_buf[0].vstride != cam->video_h ||
			 cam->still_buf[0].height != cam->video_h))
		free_still_bufs_locked(cam);
	cam->still_hold = 1;
	pthread_mutex_unlock(&cam->lock);

	/*
	 * Put REC poster on the video plane before UI goes transparent.
	 * Opening blank over a live/frozen preview feels like a sudden rotate.
	 */
	media_still_video_poster_path(poster, sizeof(poster), mp4_path);
	if (access(poster, R_OK) == 0) {
		if (camera_pipeline_show_still_jpeg(cam, poster) == 0) {
			poster_ok = 1;
			printf("[cam] play poster on plane: %s\n", poster);
			fflush(stdout);
		}
	}
	if (!poster_ok) {
		fprintf(stderr,
			"[cam] play: no poster yet (%s) — may flash live until decode\n",
			poster);
		fflush(stderr);
	}

	ret = media_player_start(cam, mp4_path);
	if (ret != 0) {
		pthread_mutex_lock(&cam->lock);
		cam->still_hold = 0;
		pthread_mutex_unlock(&cam->lock);
		printf("[cam] video play start failed: %s\n", mp4_path);
		fflush(stdout);
		return -1;
	}
	printf("[cam] album video play: %s (unshift h=%d v=%d)%s\n", mp4_path,
		video_plane_h_unshift_px(cam->video_w),
		video_plane_v_unshift_px(cam->video_h),
		poster_ok ? " +poster" : "");
	fflush(stdout);
	return 0;
}

void camera_pipeline_stop_video(camera_pipeline_t *cam)
{
	if (!cam)
		return;
	media_player_stop(cam);
	pthread_mutex_lock(&cam->lock);
	if (cam->still_hold) {
		cam->still_hold = 0;
		printf("[cam] video stopped — live preview resumes\n");
		fflush(stdout);
	}
	pthread_mutex_unlock(&cam->lock);
}

void camera_pipeline_stop(camera_pipeline_t *cam)
{
	if (!cam)
		return;
	pipeline_unwind(cam);
	memset(cam, 0, sizeof(*cam));
}
