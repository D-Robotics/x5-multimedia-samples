/***************************************************************************
 * Album MP4 → Annex-B H264 → hb_mm_mc decode → DRM NV12 (plane 33).
 * Single worker thread (demux + decode + display). No audio.
 *
 * Stop uses a generation id in .data (not a bool a nearby smash could flip).
 * Worker runs until g_stop_id == its run_id.
 ***************************************************************************/
#include "media_player.h"
#include "camera_pipeline.h"
#include "drm_ui_plane.h"
#include "media_still.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hb_media_codec.h"
#include "hb_media_error.h"
#include "hbmem.h"

#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavcodec/bsf.h"
#include "libavutil/avutil.h"
#include "libavutil/error.h"
#include "libavutil/time.h"

#define DEC_FRAME_BUFS 6
#define TAG "[player]"
#define PLAY_ID_BASE 0xA5000001u

typedef struct {
	camera_pipeline_t *cam;
	pthread_t thread;
	volatile int alive;
	volatile int joinable;
	volatile int ended;
	volatile int failed;
	volatile int paused;
	volatile int frames_shown;
	volatile int elapsed_sec;
	volatile int duration_sec;
	unsigned run_id;
	char path[256];
} media_player_t;

/*
 * Keep stop state in .data. Generation stop: worker runs until
 * g_stop_id == its run_id. A smash that writes 0/1 cannot fake a match.
 */
static volatile unsigned g_run_id __attribute__((section(".data"), used));
static volatile unsigned g_stop_id __attribute__((section(".data"), used));

static media_player_t *g_player;
static pthread_mutex_t g_player_lock = PTHREAD_MUTEX_INITIALIZER;

static int should_stop(unsigned run_id)
{
	return g_stop_id == run_id;
}

static int decode_config(media_codec_context_t *ctx, int width, int height)
{
	mc_video_codec_dec_params_t *params;

	memset(ctx, 0, sizeof(*ctx));
	ctx->encoder = false;
	ctx->codec_id = MEDIA_CODEC_ID_H264;
	params = &ctx->video_dec_params;
	params->feed_mode = MC_FEEDING_MODE_FRAME_SIZE;
	params->pix_fmt = MC_PIXEL_FORMAT_NV12;
	params->bitstream_buf_size =
		(width * height * 3 / 2 + 0xfff) & ~0xfff;
	params->bitstream_buf_count = 5;
	params->frame_buf_count = DEC_FRAME_BUFS;
	params->h264_dec_config.bandwidth_Opt = true;
	params->h264_dec_config.reorder_enable = true;
	params->h264_dec_config.skip_mode = 0;
	return 0;
}

static int decode_init_start(media_codec_context_t *ctx)
{
	mc_av_codec_startup_params_t startup = {0};
	int ret;

	ret = hb_mm_mc_initialize(ctx);
	if (ret != 0) {
		fprintf(stderr, TAG " hb_mm_mc_initialize failed %d\n", ret);
		return -1;
	}
	ret = hb_mm_mc_configure(ctx);
	if (ret != 0) {
		fprintf(stderr, TAG " hb_mm_mc_configure failed %d\n", ret);
		hb_mm_mc_release(ctx);
		return -1;
	}
	ret = hb_mm_mc_start(ctx, &startup);
	if (ret != 0) {
		fprintf(stderr, TAG " hb_mm_mc_start failed %d\n", ret);
		hb_mm_mc_release(ctx);
		return -1;
	}
	return 0;
}

static void decode_teardown(media_codec_context_t *ctx, int started)
{
	if (!ctx)
		return;
	if (started) {
		(void)hb_mm_mc_pause(ctx);
		(void)hb_mm_mc_stop(ctx);
	}
	(void)hb_mm_mc_release(ctx);
	memset(ctx, 0, sizeof(*ctx));
}

static int feed_nal(media_codec_context_t *ctx, const uint8_t *data,
		uint32_t size, int eos)
{
	media_codec_buffer_t in = {0};
	int ret;

	in.type = MC_VIDEO_STREAM_BUFFER;
	ret = hb_mm_mc_dequeue_input_buffer(ctx, &in, 2000);
	if (ret != 0)
		return ret;

	if (!eos) {
		if (in.vstream_buf.size < size) {
			fprintf(stderr, TAG " NAL too large (%u > %u)\n",
				size, in.vstream_buf.size);
			hb_mm_mc_queue_input_buffer(ctx, &in, 3000);
			return -1;
		}
		memcpy(in.vstream_buf.vir_ptr, data, size);
		in.vstream_buf.size = size;
		in.vstream_buf.stream_end = 0;
	} else {
		in.vstream_buf.size = 0;
		in.vstream_buf.stream_end = 1;
	}
	return hb_mm_mc_queue_input_buffer(ctx, &in, 2000);
}

static int show_nv12(camera_pipeline_t *cam, media_codec_buffer_t *frame)
{
	const uint8_t *y;
	const uint8_t *uv;
	int w, h, stride, vstride;
	static int logged;

	if (!cam || !frame)
		return -1;

	y = frame->vframe_buf.vir_ptr[0];
	uv = frame->vframe_buf.vir_ptr[1];
	w = frame->vframe_buf.width;
	h = frame->vframe_buf.height;
	stride = frame->vframe_buf.stride;
	vstride = frame->vframe_buf.vstride;
	if (!y)
		return -1;
	if (!logged) {
		logged = 1;
		printf(TAG " dec frame %dx%d stride=%d vstride=%d fd=%d,%d "
			"uv=%p\n",
			w, h, stride, vstride,
			frame->vframe_buf.fd[0], frame->vframe_buf.fd[1],
			(void *)uv);
		fflush(stdout);
	}

	/*
	 * MIPI DRM AddFB2 needs contiguous NV12 (UV @ stride*vstride on
	 * fd[0]). VPU decode buffers usually are not — blit into still_buf.
	 */
	return camera_pipeline_show_nv12_frame(cam, y, uv, w, h, stride,
		vstride);
}

static int drain_one(media_codec_context_t *ctx, camera_pipeline_t *cam,
		int timeout_ms)
{
	media_codec_buffer_t out = {0};
	media_codec_output_buffer_info_t info = {0};
	int ret;

	ret = hb_mm_mc_dequeue_output_buffer(ctx, &out, &info, timeout_ms);
	if (ret != 0)
		return ret;

	if (out.type != MC_VIDEO_FRAME_BUFFER || out.vframe_buf.size <= 0) {
		(void)hb_mm_mc_queue_output_buffer(ctx, &out, 0);
		return -1;
	}
	(void)info;

	if (show_nv12(cam, &out) != 0) {
		static int fail_n;

		fail_n++;
		if (fail_n <= 3 || (fail_n % 60) == 0)
			fprintf(stderr, TAG " display fail x%d\n", fail_n);
		(void)hb_mm_mc_queue_output_buffer(ctx, &out, 0);
		return -2; /* decoded but not shown — do not count as shown */
	}

	(void)hb_mm_mc_queue_output_buffer(ctx, &out, 0);
	return 0;
}

static void *player_thread(void *arg)
{
	media_player_t *p = arg;
	camera_pipeline_t *cam = p->cam;
	unsigned run_id = p->run_id;
	AVFormatContext *fmt = NULL;
	AVBSFContext *bsf = NULL;
	AVPacket *pkt = NULL;
	media_codec_context_t dec;
	int dec_ok = 0;
	int video_idx = -1;
	int width = 0, height = 0;
	int fps = 30;
	int64_t period_us;
	int64_t last_us;
	int frames = 0;
	int hit_duration = 0;
	int ret;
	const AVBitStreamFilter *filter;
	AVStream *st;
	AVRational fr;

	memset(&dec, 0, sizeof(dec));

	ret = avformat_open_input(&fmt, p->path, NULL, NULL);
	if (ret < 0) {
		fprintf(stderr, TAG " open %s failed: %s\n", p->path,
			av_err2str(ret));
		p->failed = 1;
		/* Release still_hold so live preview can resume after a bad path. */
		if (cam) {
			pthread_mutex_lock(&cam->lock);
			cam->still_hold = 0;
			pthread_mutex_unlock(&cam->lock);
		}
		goto done;
	}
	ret = avformat_find_stream_info(fmt, NULL);
	if (ret < 0) {
		fprintf(stderr, TAG " find_stream_info failed\n");
		p->failed = 1;
		goto done;
	}
	video_idx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1,
		NULL, 0);
	if (video_idx < 0) {
		fprintf(stderr, TAG " no video stream in %s\n", p->path);
		p->failed = 1;
		goto done;
	}
	st = fmt->streams[video_idx];
	if (!st->codecpar || st->codecpar->codec_id != AV_CODEC_ID_H264) {
		fprintf(stderr, TAG " only H264 supported\n");
		p->failed = 1;
		goto done;
	}
	width = st->codecpar->width;
	height = st->codecpar->height;
	if (width <= 0 || height <= 0) {
		width = cam->video_w;
		height = cam->video_h;
	}
	fr = st->avg_frame_rate;
	if (fr.num > 0 && fr.den > 0)
		fps = (fr.num + fr.den / 2) / fr.den;
	else if (cam->video_fps > 0)
		fps = cam->video_fps;
	if (fps < 1)
		fps = 30;
	if (fps > 60)
		fps = 60;
	period_us = 1000000LL / fps;

	filter = av_bsf_get_by_name("h264_mp4toannexb");
	if (!filter) {
		fprintf(stderr, TAG " missing h264_mp4toannexb BSF\n");
		p->failed = 1;
		goto done;
	}
	ret = av_bsf_alloc(filter, &bsf);
	if (ret < 0) {
		p->failed = 1;
		goto done;
	}
	ret = avcodec_parameters_copy(bsf->par_in, st->codecpar);
	if (ret < 0) {
		p->failed = 1;
		goto done;
	}
	bsf->time_base_in = st->time_base;
	ret = av_bsf_init(bsf);
	if (ret < 0) {
		fprintf(stderr, TAG " bsf_init failed\n");
		p->failed = 1;
		goto done;
	}

	if (decode_config(&dec, width, height) != 0) {
		p->failed = 1;
		goto done;
	}
	if (decode_init_start(&dec) != 0) {
		p->failed = 1;
		goto done;
	}
	dec_ok = 1;

	pkt = av_packet_alloc();
	if (!pkt) {
		p->failed = 1;
		goto done;
	}

	/* Duration: container first, then REC sidecar (.vdur) as fallback. */
	if (fmt->duration > 0) {
		int sec = (int)((fmt->duration + AV_TIME_BASE / 2) /
			AV_TIME_BASE);

		if (sec < 1)
			sec = 1;
		p->duration_sec = sec;
	} else if (st->duration > 0 && st->time_base.den > 0) {
		int64_t sec64 = st->duration * (int64_t)st->time_base.num /
			(int64_t)st->time_base.den;

		if (sec64 < 1)
			sec64 = 1;
		if (sec64 > 24 * 3600)
			sec64 = 24 * 3600;
		p->duration_sec = (int)sec64;
	}
	{
		int side = media_still_load_video_duration(p->path);

		/*
		 * Prefer REC sidecar (.vdur = frames/fps). Container metadata can
		 * under-report (bad mux timebase → 14s clip shown as 1s). .vdur is
		 * written by our recorder and matches wall-clock REC length.
		 */
		if (side > 0)
			p->duration_sec = side;
	}

	printf(TAG " play %s (%dx%d @%dfps dur=%ds) run_id=0x%x\n", p->path,
		width, height, fps, p->duration_sec, run_id);
	fflush(stdout);

	{
		int64_t start_us = av_gettime();
		int64_t paused_total_us = 0;
		int64_t pause_mark_us = 0;

		last_us = start_us;
		while (!should_stop(run_id)) {
			int el;

			while (p->paused && !should_stop(run_id) && !p->ended) {
				if (pause_mark_us == 0)
					pause_mark_us = av_gettime();
				usleep(40 * 1000);
			}
			if (pause_mark_us != 0) {
				paused_total_us += av_gettime() - pause_mark_us;
				pause_mark_us = 0;
				last_us = av_gettime();
			}
			if (should_stop(run_id))
				break;

			el = (int)((av_gettime() - start_us - paused_total_us) /
				1000000LL);
			if (el < 0)
				el = 0;
			if (p->duration_sec > 0 && el >= p->duration_sec) {
				el = p->duration_sec;
				if (!p->paused)
					p->elapsed_sec = el;
				hit_duration = 1;
				printf(TAG " duration reached (%ds) after %d frames — stop\n",
					p->duration_sec, frames);
				fflush(stdout);
				break;
			}
			if (!p->paused)
				p->elapsed_sec = el;

			ret = av_read_frame(fmt, pkt);
			if (ret < 0) {
				if (ret == AVERROR_EOF)
					printf(TAG " EOF after %d frames\n",
						frames);
				else
					fprintf(stderr, TAG " read_frame %s\n",
						av_err2str(ret));
				break;
			}
			if (pkt->stream_index != video_idx) {
				av_packet_unref(pkt);
				continue;
			}
			ret = av_bsf_send_packet(bsf, pkt);
			av_packet_unref(pkt);
			if (ret < 0)
				continue;

			for (;;) {
				AVPacket *filt = av_packet_alloc();

				if (!filt)
					break;
				ret = av_bsf_receive_packet(bsf, filt);
				if (ret == AVERROR(EAGAIN) ||
						ret == AVERROR_EOF) {
					av_packet_free(&filt);
					break;
				}
				if (ret < 0) {
					av_packet_free(&filt);
					break;
				}
				if (filt->size > 0) {
					if (feed_nal(&dec, filt->data,
							(uint32_t)filt->size,
							0) != 0) {
						static int feed_fail;

						feed_fail++;
						if (feed_fail <= 3)
							fprintf(stderr,
								TAG " feed_nal fail\n");
					} else {
						while (!should_stop(run_id) &&
								drain_one(&dec,
									cam,
									30) == 0) {
							int64_t now;
							int64_t sleep_us;

							while (p->paused &&
									!should_stop(run_id)) {
								if (pause_mark_us == 0)
									pause_mark_us =
										av_gettime();
								usleep(40 * 1000);
							}
							if (pause_mark_us != 0) {
								paused_total_us +=
									av_gettime() -
									pause_mark_us;
								pause_mark_us = 0;
								last_us = av_gettime();
							}
							if (should_stop(run_id))
								break;

							now = av_gettime();
							sleep_us = period_us -
								(now - last_us);

							frames++;
							p->frames_shown = frames;
							el = (int)((now - start_us -
								paused_total_us) /
								1000000LL);
							if (el < 0)
								el = 0;
							if (p->duration_sec > 0 &&
									el >= p->duration_sec) {
								el = p->duration_sec;
								p->elapsed_sec = el;
								hit_duration = 1;
								printf(TAG " duration reached (%ds) after %d frames — stop\n",
									p->duration_sec, frames);
								fflush(stdout);
								break;
							}
							p->elapsed_sec = el;
							if ((frames % 30) == 1) {
								printf(TAG " shown %d\n",
									frames);
								fflush(stdout);
							}
							if (sleep_us > 0)
								usleep((useconds_t)sleep_us);
							last_us = av_gettime();
						}
						if (hit_duration)
							break;
					}
				}
				av_packet_free(&filt);
				if (should_stop(run_id) || hit_duration)
					break;
			}
			if (hit_duration)
				break;
		}
	}
	if (should_stop(run_id))
		printf(TAG " stop after %d frames (stop_id=0x%x)\n", frames,
			g_stop_id);
	fflush(stdout);

	/*
	 * Flush only on natural EOF — not when we already stopped at the
	 * declared duration (extra decoder drain would keep the picture moving
	 * past "15 / 15").
	 */
	if (dec_ok && !should_stop(run_id) && !hit_duration) {
		(void)feed_nal(&dec, NULL, 0, 1);
		while (!should_stop(run_id) &&
				drain_one(&dec, cam, 500) == 0) {
			frames++;
			usleep((useconds_t)period_us);
		}
	}

	if (!should_stop(run_id)) {
		if (frames == 0) {
			p->failed = 1;
			fprintf(stderr, TAG " no frames decoded — play failed\n");
			fflush(stderr);
			if (cam) {
				pthread_mutex_lock(&cam->lock);
				cam->still_hold = 0;
				pthread_mutex_unlock(&cam->lock);
			}
		} else {
			if (p->duration_sec > 0)
				p->elapsed_sec = p->duration_sec;
			p->ended = 1;
			printf(TAG " hold last frame (%d shown), wait close\n",
				frames);
			fflush(stdout);
			while (!should_stop(run_id))
				usleep(40 * 1000);
		}
	}

done:
	if (p->failed && cam) {
		pthread_mutex_lock(&cam->lock);
		cam->still_hold = 0;
		pthread_mutex_unlock(&cam->lock);
	}
	if (pkt)
		av_packet_free(&pkt);
	if (bsf)
		av_bsf_free(&bsf);
	if (fmt)
		avformat_close_input(&fmt);
	if (dec_ok)
		decode_teardown(&dec, 1);

	/* Keep joinable=1 until media_player_stop joins us. */
	p->alive = 0;
	printf(TAG " thread exit%s\n", p->failed ? " (failed)" : "");
	fflush(stdout);
	return NULL;
}

int media_player_is_playing(void)
{
	return g_player && (g_player->joinable || g_player->alive);
}

int media_player_get_status(int *elapsed_sec, int *duration_sec, int *ended,
		int *failed)
{
	int active;

	pthread_mutex_lock(&g_player_lock);
	active = g_player && g_player->joinable;
	if (!active) {
		pthread_mutex_unlock(&g_player_lock);
		return 0;
	}
	if (elapsed_sec)
		*elapsed_sec = g_player->elapsed_sec;
	if (duration_sec)
		*duration_sec = g_player->duration_sec;
	if (ended)
		*ended = g_player->ended;
	if (failed)
		*failed = g_player->failed;
	pthread_mutex_unlock(&g_player_lock);
	return 1;
}

int media_player_is_paused(void)
{
	int v = 0;

	pthread_mutex_lock(&g_player_lock);
	if (g_player && g_player->joinable)
		v = g_player->paused;
	pthread_mutex_unlock(&g_player_lock);
	return v;
}

int media_player_frames_shown(void)
{
	int n = 0;

	pthread_mutex_lock(&g_player_lock);
	if (g_player && g_player->joinable)
		n = g_player->frames_shown;
	pthread_mutex_unlock(&g_player_lock);
	return n;
}

void media_player_set_paused(int paused)
{
	pthread_mutex_lock(&g_player_lock);
	if (g_player && g_player->joinable && !g_player->ended)
		g_player->paused = paused ? 1 : 0;
	pthread_mutex_unlock(&g_player_lock);
}

int media_player_start(camera_pipeline_t *cam, const char *mp4_path)
{
	int ret;
	unsigned id;

	if (!cam || !mp4_path || !mp4_path[0] || !cam->display_ready) {
		printf(TAG " start reject (cam=%p path=%s ready=%d)\n",
			(void *)cam, mp4_path ? mp4_path : "(null)",
			cam ? cam->display_ready : -1);
		fflush(stdout);
		return -1;
	}

	pthread_mutex_lock(&g_player_lock);
	if (!g_player) {
		g_player = calloc(1, sizeof(*g_player));
		if (!g_player) {
			pthread_mutex_unlock(&g_player_lock);
			printf(TAG " start fail: OOM\n");
			fflush(stdout);
			return -1;
		}
	} else {
		memset(g_player, 0, sizeof(*g_player));
	}

	id = g_run_id ? (g_run_id + 1u) : PLAY_ID_BASE;
	if (id < PLAY_ID_BASE)
		id = PLAY_ID_BASE;
	g_run_id = id;
	g_stop_id = 0;

	g_player->cam = cam;
	snprintf(g_player->path, sizeof(g_player->path), "%s", mp4_path);
	g_player->run_id = id;
	g_player->alive = 1;
	g_player->joinable = 1;
	g_player->ended = 0;
	g_player->failed = 0;
	g_player->paused = 0;
	g_player->frames_shown = 0;
	g_player->elapsed_sec = 0;
	g_player->duration_sec = 0;
	ret = pthread_create(&g_player->thread, NULL, player_thread, g_player);
	if (ret != 0) {
		printf(TAG " pthread_create: %s\n", strerror(ret));
		fflush(stdout);
		memset(g_player, 0, sizeof(*g_player));
		pthread_mutex_unlock(&g_player_lock);
		return -1;
	}
	pthread_mutex_unlock(&g_player_lock);
	printf(TAG " thread started for %s\n", mp4_path);
	fflush(stdout);
	return 0;
}

void media_player_stop(camera_pipeline_t *cam)
{
	pthread_t tid;
	int need_join;
	unsigned id;

	(void)cam;
	pthread_mutex_lock(&g_player_lock);
	need_join = g_player && g_player->joinable;
	if (!need_join) {
		pthread_mutex_unlock(&g_player_lock);
		return;
	}
	id = g_player->run_id;
	printf(TAG " stop requested (run_id=0x%x)\n", id);
	fflush(stdout);
	g_stop_id = id;
	tid = g_player->thread;
	pthread_mutex_unlock(&g_player_lock);

	pthread_join(tid, NULL);

	pthread_mutex_lock(&g_player_lock);
	if (g_player)
		memset(g_player, 0, sizeof(*g_player));
	pthread_mutex_unlock(&g_player_lock);
}
