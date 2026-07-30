/***************************************************************************
 * Live annex-B H.264 → MP4 (libavformat). Avoids board ffmpeg CLI remux.
 ***************************************************************************/
#ifndef REC_MUX_H
#define REC_MUX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rec_mux rec_mux_t;

/* Open MP4 output; returns NULL on failure. fps <= 0 defaults to 30. */
rec_mux_t *rec_mux_open(const char *mp4_path, int width, int height, int fps);

/*
 * Write one annex-B access unit.
 * pts_90k: PTS in 90 kHz; pass < 0 to use frame index / fps.
 * is_key: non-zero if keyframe (also inferred from NAL type when needed).
 * Returns 0 on success / early drop waiting for SPS/PPS; -1 on error.
 */
int rec_mux_write(rec_mux_t *m, const uint8_t *annexb, int size,
		int64_t pts_90k, int is_key);

/* Write trailer (if header was written) and free muxer. */
int rec_mux_close(rec_mux_t *m);

#ifdef __cplusplus
}
#endif

#endif /* REC_MUX_H */
