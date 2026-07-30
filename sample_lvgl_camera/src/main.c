/***************************************************************************
 * LVGL camera UI demo: SC230AI preview (Plane 33) + overlay UI (Plane 40).
 ***************************************************************************/
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "camera_pipeline.h"
#include "channel_param_parser.h"
#include "drm_ui_plane.h"
#include "lvgl.h"
#include "lvgl_display.h"
#include "lvgl_input.h"
#include "ui_camera.h"

/*
 * Heap (not BSS): a large BSS g_cam used to sit next to other globals;
 * overflows past its end could smash neighbors. still_buf is also heap.
 */
static camera_pipeline_t *g_cam;
static drm_ui_plane_t g_ui_plane;
static ui_camera_t g_ui;
static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig)
{
	(void)sig;
	g_running = 0;
}

static int on_capture(void *user, char *path, size_t path_len)
{
	(void)user;
	return camera_pipeline_capture_begin(g_cam, path, path_len);
}

static int on_capture_poll(void *user, char *path, size_t path_len)
{
	(void)user;
	return camera_pipeline_capture_poll(g_cam, path, path_len);
}

static int on_record(void *user, int start, char *path, size_t path_len)
{
	(void)user;
	if (start)
		return camera_pipeline_start_record(g_cam, path, path_len);
	/* Stop: tear down encode now; remux finishes via on_record_poll. */
	if (camera_pipeline_stop_record_begin(g_cam) != 0)
		return -1;
	if (path && path_len)
		path[0] = '\0';
	return 0;
}

static int on_record_poll(void *user, char *path, size_t path_len)
{
	(void)user;
	return camera_pipeline_stop_record_poll(g_cam, path, path_len);
}

static int on_rec_frames(void *user)
{
	(void)user;
	return camera_pipeline_get_record_frames(g_cam);
}

static int on_rec_abort(void *user)
{
	(void)user;
	return camera_pipeline_rec_abort_reason(g_cam);
}

static int on_thumb(void *user, uint16_t *rgb565, int *w, int *h, int *seq)
{
	(void)user;
	return camera_pipeline_get_last_thumb(g_cam, rgb565, w, h, seq);
}

static const char *on_save_dir(void *user)
{
	(void)user;
	return camera_pipeline_save_dir(g_cam);
}

static int on_still_show(void *user, const char *path)
{
	(void)user;
	return camera_pipeline_show_still_jpeg(g_cam, path);
}

static void on_still_hide(void *user)
{
	(void)user;
	camera_pipeline_clear_still(g_cam);
}

/*
 * path == NULL / empty → stop (clear still_hold, join player).
 * Otherwise play an absolute *.mp4. Do not tail-call: a mistaken 1-arg call
 * used to leave x1 = &on_video_play, which prints as the path "@\\n".
 */
static int on_video_play(void *user, const char *path)
{
	size_t n;

	(void)user;
	if (!path || !path[0]) {
		camera_pipeline_clear_still(g_cam);
		return 0;
	}
	if (path[0] != '/') {
		printf("[cam] play rejected non-absolute path '%s'\n", path);
		fflush(stdout);
		return -1;
	}
	n = strlen(path);
	if (n < 5 || strcasecmp(path + n - 4, ".mp4") != 0) {
		printf("[cam] play rejected non-mp4 path '%s'\n", path);
		fflush(stdout);
		return -1;
	}
	printf("[cam] on_video_play → pipeline %s\n", path);
	fflush(stdout);
	return camera_pipeline_play_video(g_cam, path);
}

static void print_help(const char *argv0)
{
	printf("Usage: %s [OPTIONS]\n", argv0);
	printf("LVGL camera UI on MIPI panel (SC230AI).\n");
	printf("Options:\n");
	printf("  -s <index>   Sensor index (default: first sc230ai)\n");
	printf("  -c <type>    Channel topology, default vf:if\n");
	printf("  -i <dev>     Touch input device (default: auto)\n");
	printf("  -h           Show help\n");
	printf("Env:\n");
	printf("  CAM_SAVE_DIR       Capture/REC output dir (default /userdata/lvgl_camera)\n");
	printf("  CAM_REC_BITRATE    H264 bitrate kbps (default 4096)\n");
	printf("  CAM_JPEG_QUALITY   JPEG quality 1-100 (default 85)\n");
	printf("  CAM_WATERMARK      1=timestamp watermark (default 1)\n");
	printf("  CAM_PREV_UNSHIFT   Album photo+video panel L/R (default -35)\n");
	printf("  CAM_GALLERY_N      Album list rows, newest first (default 30; title show/total)\n");
	printf("  CAM_STORAGE_MIN_MB Refuse save / auto-stop REC below this free MB (default 64)\n");
	vp_show_sensors_list();
}

/*
 * Product defaults so `./lvgl_camera` works without a wrapper script.
 * Only set when unset; clear legacy one-off layout overrides.
 */
static void apply_product_env_defaults(void)
{
	struct {
		const char *k;
		const char *v;
	} defs[] = {
		{ "UI_BAR_ROT180", "1" },
		{ "UI_POLAROID", "1" },
		{ "UI_FRAME_L", "16" },
		{ "UI_FRAME_R", "16" },
		{ "UI_OVERSCAN_L", "0" },
		{ "UI_OVERSCAN_R", "40" },
		{ "CAM_VIDEO_ROTATE", "90" },
		{ "CAM_PREV_UNSHIFT", "-35" },
		{ "UI_SWAP_BUTTONS", "1" },
		{ "UI_BTN_HIT_PAD", "20" },
		{ "UI_REC_BTN", "56" },
		{ "LVGL_TOUCH_INVERT_X", "1" },
		{ "LVGL_TOUCH_OFFSET_X", "-40" },
	};
	size_t i;
	const char *v;

	/* Legacy experiments — must not win over unified photo/video layout. */
	unsetenv("CAM_VIDEO_PANEL_UNSHIFT");
	unsetenv("UI_VIDEO_OVERSCAN_R");

	for (i = 0; i < sizeof(defs) / sizeof(defs[0]); i++) {
		v = getenv(defs[i].k);
		if (v && v[0])
			continue;
		setenv(defs[i].k, defs[i].v, 0);
	}

	printf("[ui] product env: BAR_ROT180=%s FRAME_L/R=%s/%s "
		"PREV_UNSHIFT=%s VIDEO_ROTATE=%s\n",
		getenv("UI_BAR_ROT180") ? getenv("UI_BAR_ROT180") : "?",
		getenv("UI_FRAME_L") ? getenv("UI_FRAME_L") : "?",
		getenv("UI_FRAME_R") ? getenv("UI_FRAME_R") : "?",
		getenv("CAM_PREV_UNSHIFT") ? getenv("CAM_PREV_UNSHIFT") : "?",
		getenv("CAM_VIDEO_ROTATE") ? getenv("CAM_VIDEO_ROTATE") : "?");
	fflush(stdout);
}

int main(int argc, char **argv)
{
	int sensor_index = -1;
	const char *input_dev = NULL;
	int opt;
	int ret;
	void *heap_probe;

	/* Early heap canary: if this aborts, a .init/.ctor already smashed glibc. */
	heap_probe = malloc(64);
	if (!heap_probe) {
		fprintf(stderr, "[main] malloc probe failed before env\n");
		return 1;
	}
	free(heap_probe);
	printf("[main] heap probe ok, ui_camera BSS ~%zu\n", sizeof(g_ui));
	fflush(stdout);

	apply_product_env_defaults();

	while ((opt = getopt(argc, argv, "s:c:i:h")) != -1) {
		switch (opt) {
		case 's':
			sensor_index = atoi(optarg);
			break;
		case 'c':
			if (parse_channel_string(optarg) != 0) {
				fprintf(stderr, "Invalid channel type: %s\n", optarg);
				return 1;
			}
			break;
		case 'i':
			input_dev = optarg;
			break;
		case 'h':
		default:
			print_help(argv[0]);
			return 0;
		}
	}

	printf("[main] getopt done, find sensor...\n");
	fflush(stdout);
	if (sensor_index < 0)
		sensor_index = camera_find_sc230ai_index();
	if (sensor_index < 0) {
		fprintf(stderr, "No SC230AI sensor in vp_sensors list\n");
		print_help(argv[0]);
		return 1;
	}
	printf("[main] sensor_index=%d, pipeline create...\n", sensor_index);
	fflush(stdout);

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	/* calloc lives in camera_pipeline.c — same sizeof as init memset. */
	g_cam = camera_pipeline_create(sensor_index);
	if (!g_cam) {
		fprintf(stderr, "camera_pipeline_create failed\n");
		return 1;
	}

	ret = drm_ui_plane_init(&g_ui_plane, &g_cam->drm);
	if (ret != 0) {
		fprintf(stderr, "drm_ui_plane_init failed\n");
		camera_pipeline_destroy(g_cam);
		g_cam = NULL;
		return 1;
	}

	ret = lvgl_display_init(&g_ui_plane);
	if (ret != 0) {
		fprintf(stderr, "lvgl_display_init failed\n");
		drm_ui_plane_deinit(&g_ui_plane);
		camera_pipeline_destroy(g_cam);
		g_cam = NULL;
		return 1;
	}

	if (lvgl_input_init(input_dev) != 0)
		fprintf(stderr, "Warning: running without touch input\n");

	ui_camera_create(&g_ui, g_cam->pipe.sensor_config->sensor_name);
	ui_camera_set_ops(&g_ui, on_capture, on_record, NULL);
	ui_camera_set_poll_ops(&g_ui, on_capture_poll, on_record_poll);
	ui_camera_set_rec_frames_cb(&g_ui, on_rec_frames);
	ui_camera_set_rec_abort_cb(&g_ui, on_rec_abort);
	ui_camera_set_thumb_cb(&g_ui, on_thumb);
	ui_camera_set_save_dir_cb(&g_ui, on_save_dir);
	ui_gallery_set_still_ops(&g_ui.gallery, on_still_show, on_still_hide,
		NULL);
	ui_gallery_set_video_play(&g_ui.gallery, on_video_play);
	ui_camera_set_video_info(&g_ui, g_cam->video_w, g_cam->video_h,
		g_cam->video_fps);
	drm_ui_plane_clear(&g_ui_plane);
	printf("[main] UI ready (%dx%d @%dfps), first refresh...\n",
		g_cam->video_w, g_cam->video_h,
		g_cam->video_fps > 0 ? g_cam->video_fps : 30);
	fflush(stdout);
	lv_refr_now(NULL);
	printf("[main] UI refreshed\n");
	fflush(stdout);

	/* Link overlay, then allow DRM video commits (VSE was drain-only until now). */
	camera_pipeline_set_ui_overlay(g_cam, &g_ui_plane);
	if (camera_pipeline_start_preview(g_cam) != 0) {
		fprintf(stderr, "camera_pipeline_start_preview failed\n");
		g_running = 0;
	} else {
		printf("[main] preview + UI overlay linked\n");
		fflush(stdout);
	}

	printf("sample_lvgl_camera running (Ctrl+C to exit)\n");
	printf("Capture -> JPEG; REC -> MP4 under %s\n", g_cam->save_dir);
	fflush(stdout);
	while (g_running) {
		uint32_t delay = lv_timer_handler();

		if (lvgl_display_take_dirty())
			(void)drm_ui_plane_flip(&g_ui_plane);

		if (delay < 1)
			delay = 1;
		if (delay > 20)
			delay = 20;
		usleep(delay * 1000);
	}

	if (camera_pipeline_is_recording(g_cam))
		(void)camera_pipeline_stop_record(g_cam, NULL, 0);

	lvgl_input_deinit();
	lvgl_display_deinit();
	drm_ui_plane_deinit(&g_ui_plane);
	camera_pipeline_destroy(g_cam);
	g_cam = NULL;
	printf("sample_lvgl_camera exited\n");
	return 0;
}
