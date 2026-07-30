/***************************************************************************
 * Album MP4 playback: libavformat demux + HW H264 decode → NV12 plane.
 ***************************************************************************/
#ifndef MEDIA_PLAYER_H_
#define MEDIA_PLAYER_H_

struct camera_pipeline;
typedef struct camera_pipeline camera_pipeline_t;

/* Start async playback on plane 33. Returns 0 if thread started. */
int media_player_start(camera_pipeline_t *cam, const char *mp4_path);
/* Stop playback (join worker). Safe if not playing. */
void media_player_stop(camera_pipeline_t *cam);
int media_player_is_playing(void);

/*
 * Progress for album HUD. Returns 1 while a play session is active
 * (decoding, holding last frame, or failed-but-not-yet-stopped).
 * *ended=1 after EOF while holding last frame; *failed=1 if decode never
 * produced a frame (caller should fall back / dismiss).
 */
int media_player_get_status(int *elapsed_sec, int *duration_sec, int *ended,
		int *failed);
int media_player_is_paused(void);
/* Pause/resume decode loop (no-op if ended or not playing). */
void media_player_set_paused(int paused);
/* Decoded frames committed to the video plane (0 until first show). */
int media_player_frames_shown(void);

#endif /* MEDIA_PLAYER_H_ */
