/***************************************************************************
 * Live annex-B H.264 → MP4 (libavformat). Avoids board ffmpeg CLI remux.
 ***************************************************************************/
#include "rec_mux.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/error.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"

struct rec_mux {
	AVFormatContext *oc;
	AVStream *st;
	int width;
	int height;
	int fps;
	int header_ok;
	int64_t frame_idx;
	uint8_t *avcc_buf;
	int avcc_size;
	pthread_mutex_t lock;
};

static const uint8_t *find_start_code(const uint8_t *p, const uint8_t *end,
		int *sc_len)
{
	while (p + 3 < end) {
		if (p[0] == 0 && p[1] == 0) {
			if (p[2] == 1) {
				*sc_len = 3;
				return p;
			}
			if (p[2] == 0 && p[3] == 1) {
				*sc_len = 4;
				return p;
			}
		}
		p++;
	}
	return NULL;
}

/* Build AVCC extradata from first SPS/PPS found in annex-B. */
static int build_avcc(const uint8_t *in, int in_size, uint8_t **out,
		int *out_size)
{
	const uint8_t *p = in;
	const uint8_t *end = in + in_size;
	const uint8_t *sps = NULL;
	const uint8_t *pps = NULL;
	int sps_len = 0;
	int pps_len = 0;
	int sc;
	uint8_t *cfg;
	int cfg_size;

	while (p < end) {
		const uint8_t *nal;
		const uint8_t *next;
		int ntype;
		int nlen;

		nal = find_start_code(p, end, &sc);
		if (!nal)
			break;
		nal += sc;
		next = find_start_code(nal, end, &sc);
		if (!next)
			next = end;
		nlen = (int)(next - nal);
		if (nlen < 1) {
			p = next;
			continue;
		}
		ntype = nal[0] & 0x1f;
		if (ntype == 7 && !sps) {
			sps = nal;
			sps_len = nlen;
		} else if (ntype == 8 && !pps) {
			pps = nal;
			pps_len = nlen;
		}
		if (sps && pps)
			break;
		p = next;
	}
	if (!sps || !pps || sps_len < 4 || pps_len < 1)
		return -1;

	cfg_size = 11 + sps_len + pps_len;
	cfg = malloc((size_t)cfg_size);
	if (!cfg)
		return -1;
	cfg[0] = 1;
	cfg[1] = sps[1];
	cfg[2] = sps[2];
	cfg[3] = sps[3];
	cfg[4] = 0xff; /* 4-byte NAL lengths */
	cfg[5] = 0xe1;
	cfg[6] = (uint8_t)((sps_len >> 8) & 0xff);
	cfg[7] = (uint8_t)(sps_len & 0xff);
	memcpy(cfg + 8, sps, (size_t)sps_len);
	cfg[8 + sps_len] = 1;
	cfg[9 + sps_len] = (uint8_t)((pps_len >> 8) & 0xff);
	cfg[10 + sps_len] = (uint8_t)(pps_len & 0xff);
	memcpy(cfg + 11 + sps_len, pps, (size_t)pps_len);
	*out = cfg;
	*out_size = cfg_size;
	return 0;
}

/* annex-B → AVCC length-prefixed NALs (skip AUD). */
static int annexb_to_avcc_au(const uint8_t *in, int in_size, uint8_t **out,
		int *out_size, int *is_key)
{
	const uint8_t *p = in;
	const uint8_t *end = in + in_size;
	uint8_t *dst;
	int cap = in_size + 32;
	int used = 0;
	int key = 0;

	dst = malloc((size_t)cap);
	if (!dst)
		return -1;

	while (p < end) {
		const uint8_t *nal;
		const uint8_t *next;
		int sc;
		int nlen;
		int ntype;

		nal = find_start_code(p, end, &sc);
		if (!nal)
			break;
		nal += sc;
		next = find_start_code(nal, end, &sc);
		if (!next)
			next = end;
		nlen = (int)(next - nal);
		p = next;
		if (nlen < 1)
			continue;
		ntype = nal[0] & 0x1f;
		if (ntype == 9) /* AUD */
			continue;
		if (ntype == 5)
			key = 1;
		if (used + 4 + nlen > cap) {
			int ncap = (used + 4 + nlen) * 2;
			uint8_t *nd = realloc(dst, (size_t)ncap);

			if (!nd) {
				free(dst);
				return -1;
			}
			dst = nd;
			cap = ncap;
		}
		dst[used + 0] = (uint8_t)((nlen >> 24) & 0xff);
		dst[used + 1] = (uint8_t)((nlen >> 16) & 0xff);
		dst[used + 2] = (uint8_t)((nlen >> 8) & 0xff);
		dst[used + 3] = (uint8_t)(nlen & 0xff);
		memcpy(dst + used + 4, nal, (size_t)nlen);
		used += 4 + nlen;
	}
	if (used <= 0) {
		free(dst);
		return -1;
	}
	*out = dst;
	*out_size = used;
	if (is_key)
		*is_key = key;
	return 0;
}

static int ensure_header(rec_mux_t *m, const uint8_t *annexb, int size)
{
	AVCodecParameters *par;
	uint8_t *avcc = NULL;
	int avcc_size = 0;
	int ret;

	if (m->header_ok)
		return 0;
	if (build_avcc(annexb, size, &avcc, &avcc_size) != 0)
		return 1; /* wait for SPS/PPS */

	par = m->st->codecpar;
	par->extradata = av_mallocz((size_t)avcc_size + AV_INPUT_BUFFER_PADDING_SIZE);
	if (!par->extradata) {
		free(avcc);
		return -1;
	}
	memcpy(par->extradata, avcc, (size_t)avcc_size);
	par->extradata_size = avcc_size;
	free(avcc);
	m->avcc_buf = NULL;
	m->avcc_size = 0;

	ret = avformat_write_header(m->oc, NULL);
	if (ret < 0) {
		char err[128];

		av_strerror(ret, err, sizeof(err));
		fprintf(stderr, "[rec_mux] write_header failed: %s\n", err);
		return -1;
	}
	m->header_ok = 1;
	printf("[rec_mux] MP4 header written (%dx%d @%dfps, tb=%d/%d)\n",
		m->width, m->height, m->fps,
		m->st->time_base.num, m->st->time_base.den);
	fflush(stdout);
	return 0;
}

rec_mux_t *rec_mux_open(const char *mp4_path, int width, int height, int fps)
{
	rec_mux_t *m;
	AVStream *st;
	int ret;

	if (!mp4_path || !mp4_path[0] || width <= 0 || height <= 0)
		return NULL;
	if (fps <= 0)
		fps = 30;

	m = calloc(1, sizeof(*m));
	if (!m)
		return NULL;
	pthread_mutex_init(&m->lock, NULL);
	m->width = width;
	m->height = height;
	m->fps = fps;

	ret = avformat_alloc_output_context2(&m->oc, NULL, "mp4", mp4_path);
	if (ret < 0 || !m->oc) {
		fprintf(stderr, "[rec_mux] alloc_output_context failed\n");
		goto fail;
	}
	st = avformat_new_stream(m->oc, NULL);
	if (!st) {
		fprintf(stderr, "[rec_mux] new_stream failed\n");
		goto fail;
	}
	m->st = st;
	st->id = 0;
	/*
	 * Hint only — mov/mp4 remaps st->time_base in write_header (often
	 * 1/15360). Packet PTS must be rescaled into that muxer time_base.
	 */
	st->time_base = (AVRational){ 1, fps };
	st->avg_frame_rate = (AVRational){ fps, 1 };
	st->r_frame_rate = (AVRational){ fps, 1 };
	st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
	st->codecpar->codec_id = AV_CODEC_ID_H264;
	st->codecpar->width = width;
	st->codecpar->height = height;
	st->codecpar->format = AV_PIX_FMT_NV12;
	st->codecpar->bit_rate = 0;

	if (!(m->oc->oformat->flags & AVFMT_NOFILE)) {
		ret = avio_open(&m->oc->pb, mp4_path, AVIO_FLAG_WRITE);
		if (ret < 0) {
			char err[128];

			av_strerror(ret, err, sizeof(err));
			fprintf(stderr, "[rec_mux] avio_open %s: %s\n",
				mp4_path, err);
			goto fail;
		}
	}
	printf("[rec_mux] open %s (%dx%d @%dfps) — live mux\n",
		mp4_path, width, height, fps);
	fflush(stdout);
	return m;

fail:
	rec_mux_close(m);
	return NULL;
}

int rec_mux_write(rec_mux_t *m, const uint8_t *annexb, int size,
		int64_t pts_90k, int is_key)
{
	uint8_t *avcc = NULL;
	int avcc_size = 0;
	int key = is_key;
	AVPacket *pkt;
	AVRational src_tb;
	int64_t src_pts;
	int ret;
	int hr;

	if (!m || !annexb || size <= 0)
		return -1;

	pthread_mutex_lock(&m->lock);
	hr = ensure_header(m, annexb, size);
	if (hr < 0) {
		pthread_mutex_unlock(&m->lock);
		return -1;
	}
	if (hr > 0) {
		/* SPS/PPS not ready yet — drop early SEI-only if any. */
		pthread_mutex_unlock(&m->lock);
		return 0;
	}

	if (annexb_to_avcc_au(annexb, size, &avcc, &avcc_size, &key) != 0) {
		pthread_mutex_unlock(&m->lock);
		return -1;
	}

	pkt = av_packet_alloc();
	if (!pkt) {
		free(avcc);
		pthread_mutex_unlock(&m->lock);
		return -1;
	}
	ret = av_packet_from_data(pkt, avcc, avcc_size);
	if (ret < 0) {
		av_packet_free(&pkt);
		free(avcc);
		pthread_mutex_unlock(&m->lock);
		return -1;
	}

	/*
	 * MUST rescale into post-write_header muxer time_base. Writing raw
	 * frame indices (0,1,2,…) into e.g. 1/15360 made a 14s clip look
	 * like ~1s @60fps on playback.
	 */
	if (pts_90k >= 0) {
		src_tb = (AVRational){ 1, 90000 };
		src_pts = pts_90k;
	} else {
		src_tb = (AVRational){ 1, m->fps > 0 ? m->fps : 30 };
		src_pts = m->frame_idx;
	}
	pkt->stream_index = m->st->index;
	pkt->pts = av_rescale_q(src_pts, src_tb, m->st->time_base);
	pkt->dts = pkt->pts;
	pkt->duration = av_rescale_q(1, (AVRational){ 1, m->fps > 0 ? m->fps : 30 },
		m->st->time_base);
	if (pkt->duration <= 0)
		pkt->duration = 1;
	pkt->flags = key ? AV_PKT_FLAG_KEY : 0;

	ret = av_interleaved_write_frame(m->oc, pkt);
	av_packet_free(&pkt);
	if (ret < 0) {
		char err[128];

		av_strerror(ret, err, sizeof(err));
		fprintf(stderr, "[rec_mux] write_frame failed: %s\n", err);
		pthread_mutex_unlock(&m->lock);
		return -1;
	}
	m->frame_idx++;
	pthread_mutex_unlock(&m->lock);
	return 0;
}

int rec_mux_close(rec_mux_t *m)
{
	if (!m)
		return 0;

	pthread_mutex_lock(&m->lock);
	if (m->oc) {
		if (m->header_ok)
			av_write_trailer(m->oc);
		if (m->oc->pb && !(m->oc->oformat->flags & AVFMT_NOFILE))
			avio_closep(&m->oc->pb);
		avformat_free_context(m->oc);
		m->oc = NULL;
	}
	free(m->avcc_buf);
	m->avcc_buf = NULL;
	pthread_mutex_unlock(&m->lock);
	pthread_mutex_destroy(&m->lock);
	free(m);
	return 0;
}
