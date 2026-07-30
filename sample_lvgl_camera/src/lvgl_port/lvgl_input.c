/***************************************************************************
 * Touch via custom EVDEV reader (Goodix MT + SYN_REPORT framing).
 *
 * Default: trust EVIOCGABS (origin = abs min, span = abs max - min).
 * Map abs → FB with stretch when spans differ; identity when they match.
 * Board-specific quirks belong in env overrides — not hard-coded here.
 *
 * SYN_REPORT framing + continue_reading keeps press/release coherent.
 ***************************************************************************/
#include "lvgl_input.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "lvgl.h"

static lv_indev_t *g_indev;
static int g_evdev_fd = -1;
static lv_coord_t g_hor_res;
static lv_coord_t g_ver_res;
static char g_dev_path[64];

static int g_abs_x_min;
static int g_abs_x_max;
static int g_abs_y_min;
static int g_abs_y_max;
static int g_origin_x;
static int g_origin_y;
static int g_span_x;
static int g_span_y;
static int g_stretch_x;
static int g_stretch_y;
static int g_swap_xy;
static int g_invert_x;
static int g_invert_y;
static int g_offset_x;
static int g_offset_y;
static int g_touch_debug;
static int g_logged_first_touch;

#define TOUCH_QUEUE_SIZE 64

typedef struct {
	lv_coord_t x;
	lv_coord_t y;
	lv_indev_state_t state;
} touch_point_t;

static touch_point_t g_queue[TOUCH_QUEUE_SIZE];
static int g_q_head;
static int g_q_tail;

static int evdev_env_on(const char *name)
{
	const char *v = getenv(name);

	if (!v || !v[0])
		return -1;
	return atoi(v) != 0;
}

static int evdev_env_int(const char *name, int fallback)
{
	const char *v = getenv(name);

	if (!v || !v[0])
		return fallback;
	return atoi(v);
}

static int evdev_test_bit(int bit, const unsigned long *bits)
{
	unsigned long word = bits[bit / (8 * sizeof(long))];

	return (int)((word >> (bit % (8 * sizeof(long)))) & 1UL);
}

static int evdev_is_touchscreen(int fd)
{
	unsigned long bits[(ABS_MAX + 1) / (8 * sizeof(long)) + 1] = {0};

	if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(bits)), bits) < 0)
		return 0;
	if (evdev_test_bit(ABS_X, bits) || evdev_test_bit(ABS_Y, bits))
		return 1;
	if (evdev_test_bit(ABS_MT_POSITION_X, bits) ||
	    evdev_test_bit(ABS_MT_POSITION_Y, bits))
		return 1;
	return 0;
}

static int evdev_name_has(const char *name, const char *key)
{
	size_t i, nlen, klen;

	if (!name || !key || !key[0])
		return 0;
	nlen = strlen(name);
	klen = strlen(key);
	if (klen > nlen)
		return 0;
	for (i = 0; i + klen <= nlen; i++) {
		size_t j;

		for (j = 0; j < klen; j++) {
			if (tolower((unsigned char)name[i + j]) !=
					tolower((unsigned char)key[j]))
				break;
		}
		if (j == klen)
			return 1;
	}
	return 0;
}

static int evdev_name_is_touch(const char *name)
{
	static const char *keys[] = {
		"goodix", "gt9", "touch", "ts", "ft5", "ft6", NULL
	};
	int i;

	if (!name || !name[0])
		return 0;
	for (i = 0; keys[i]; i++) {
		if (evdev_name_has(name, keys[i]))
			return 1;
	}
	return 0;
}

static void evdev_list_inputs(void)
{
	char path[64];
	char name[256];
	int i;
	int found = 0;
	int fd;

	fprintf(stderr, "lvgl_input: scanning /dev/input/event* ...\n");
	for (i = 0; i < 32; i++) {
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		found = 1;
		name[0] = '\0';
		(void)ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
		if (evdev_is_touchscreen(fd))
			fprintf(stderr, "  %s: %s (touch)\n", path,
				name[0] ? name : "?");
		else
			fprintf(stderr, "  %s: %s (not touch)\n", path,
				name[0] ? name : "?");
		close(fd);
	}
	if (!found)
		fprintf(stderr, "  (none — load gt9xx/goodix and input-evdev)\n");
}

static int evdev_find_path(const char *prefer, char *out, size_t out_len)
{
	char path[64];
	char name[256];
	char fallback[64] = {0};
	int fd;
	int i;

	if (prefer && prefer[0]) {
		fd = open(prefer, O_RDONLY | O_NONBLOCK);
		if (fd >= 0 && evdev_is_touchscreen(fd)) {
			name[0] = '\0';
			(void)ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
			close(fd);
			snprintf(out, out_len, "%s", prefer);
			printf("lvgl_input: using %s (%s)\n", out,
				name[0] ? name : "?");
			return 0;
		}
		if (fd >= 0)
			close(fd);
		fprintf(stderr, "lvgl_input: %s unavailable, scanning...\n",
			prefer);
	}

	for (i = 0; i < 32; i++) {
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		if (!evdev_is_touchscreen(fd)) {
			close(fd);
			continue;
		}
		name[0] = '\0';
		(void)ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
		close(fd);

		if (evdev_name_is_touch(name)) {
			snprintf(out, out_len, "%s", path);
			printf("lvgl_input: using %s (%s)\n", out, name);
			return 0;
		}
		if (!fallback[0])
			snprintf(fallback, sizeof(fallback), "%s", path);
	}

	if (fallback[0]) {
		snprintf(out, out_len, "%s", fallback);
		printf("lvgl_input: using fallback %s\n", out);
		return 0;
	}
	return -1;
}

static void evdev_read_abs_range(int fd)
{
	struct input_absinfo absinfo;

	g_abs_x_min = 0;
	g_abs_y_min = 0;
	g_abs_x_max = g_hor_res > 0 ? g_hor_res - 1 : 719;
	g_abs_y_max = g_ver_res > 0 ? g_ver_res - 1 : 1279;

	if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &absinfo) == 0) {
		g_abs_x_min = absinfo.minimum;
		g_abs_x_max = absinfo.maximum;
	} else if (ioctl(fd, EVIOCGABS(ABS_X), &absinfo) == 0) {
		g_abs_x_min = absinfo.minimum;
		g_abs_x_max = absinfo.maximum;
	}

	if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &absinfo) == 0) {
		g_abs_y_min = absinfo.minimum;
		g_abs_y_max = absinfo.maximum;
	} else if (ioctl(fd, EVIOCGABS(ABS_Y), &absinfo) == 0) {
		g_abs_y_min = absinfo.minimum;
		g_abs_y_max = absinfo.maximum;
	}

	if (g_abs_x_max < g_abs_x_min)
		g_abs_x_max = g_hor_res > 0 ? g_hor_res - 1 : 719;
	if (g_abs_y_max < g_abs_y_min)
		g_abs_y_max = g_ver_res > 0 ? g_ver_res - 1 : 1279;
}

static void evdev_apply_calibration(void)
{
	int swap_env = evdev_env_on("LVGL_TOUCH_SWAP_XY");
	int invx_env = evdev_env_on("LVGL_TOUCH_INVERT_X");
	int invy_env = evdev_env_on("LVGL_TOUCH_INVERT_Y");
	int stretch_env = evdev_env_on("LVGL_TOUCH_STRETCH");
	int stretch_x_env = evdev_env_on("LVGL_TOUCH_STRETCH_X");
	int stretch_y_env = evdev_env_on("LVGL_TOUCH_STRETCH_Y");
	int touch_w = g_abs_x_max - g_abs_x_min + 1;
	int touch_h = g_abs_y_max - g_abs_y_min + 1;
	int default_ox = 0;
	int def_ox = g_abs_x_min;
	int def_oy = g_abs_y_min;
	int def_xmax = g_abs_x_max;
	int def_ymax = g_abs_y_max;
	int x_max;
	int y_max;
	int product_720x1280 = (g_hor_res == 720 && g_ver_res == 1280);

	/*
	 * This product module: usable touch Y is ~294..1167, but EVIOCGABS often
	 * reports y[0..1280]. Without remapping, raw y≈330 stays FB y≈330 and
	 * misses the ctrl bar at FB y≈22..118. Apply even when env overrides are
	 * missing (direct ./lvgl_camera). Override anytime with LVGL_TOUCH_*.
	 */
	if (product_720x1280 && g_abs_y_min < 200) {
		def_oy = 294;
		if (g_abs_y_max >= 1200)
			def_ymax = 1167;
	}

	g_origin_x = evdev_env_int("LVGL_TOUCH_ORIGIN_X", def_ox);
	g_origin_y = evdev_env_int("LVGL_TOUCH_ORIGIN_Y", def_oy);
	x_max = evdev_env_int("LVGL_TOUCH_X_MAX", def_xmax);
	y_max = evdev_env_int("LVGL_TOUCH_Y_MAX", def_ymax);
	g_span_x = x_max - g_origin_x;
	g_span_y = y_max - g_origin_y;
	if (g_span_x < 1)
		g_span_x = 1;
	if (g_span_y < 1)
		g_span_y = 1;

	/*
	 * Stretch abs→FB when spans differ from the display, or on this product
	 * panel (usable band must cover full FB height for album + ctrl bar).
	 */
	g_stretch_x = (g_hor_res > 1 &&
		(g_span_x > g_hor_res + 2 || g_span_x + 2 < g_hor_res));
	g_stretch_y = (g_ver_res > 1 &&
		(g_span_y > g_ver_res + 2 || g_span_y + 2 < g_ver_res));
	if (product_720x1280 && g_origin_y >= 200)
		g_stretch_y = 1;
	if (stretch_env >= 0) {
		g_stretch_x = stretch_env;
		g_stretch_y = stretch_env;
	}
	if (stretch_x_env >= 0)
		g_stretch_x = stretch_x_env;
	if (stretch_y_env >= 0)
		g_stretch_y = stretch_y_env;

	g_swap_xy = 0;
	g_invert_x = 0;
	g_invert_y = 0;

	if (swap_env >= 0)
		g_swap_xy = swap_env;
	else if (touch_w > touch_h && g_hor_res < g_ver_res)
		g_swap_xy = 1;
	else if (touch_w < touch_h && g_hor_res > g_ver_res)
		g_swap_xy = 1;

	if (invx_env >= 0)
		g_invert_x = invx_env;
	else if (product_720x1280)
		g_invert_x = 1;
	if (invy_env >= 0)
		g_invert_y = invy_env;

	/* Product mount: LCD vs digitizer X mirror + right overscan. */
	if (product_720x1280 && g_invert_x)
		default_ox = -40;
	g_offset_x = evdev_env_int("LVGL_TOUCH_OFFSET_X", default_ox);
	g_offset_y = evdev_env_int("LVGL_TOUCH_OFFSET_Y", 0);
}

/*
 * Origin → optional stretch onto FB → swap / invert / pixel offset.
 */
static void evdev_map_point(int raw_x, int raw_y, lv_coord_t *x, lv_coord_t *y)
{
	int rx = raw_x - g_origin_x;
	int ry = raw_y - g_origin_y;
	lv_coord_t mx;
	lv_coord_t my;

	if (g_stretch_x && g_span_x > 1 && g_hor_res > 1)
		rx = (int)((int64_t)rx * (g_hor_res - 1) / g_span_x);
	if (g_stretch_y && g_span_y > 1 && g_ver_res > 1)
		ry = (int)((int64_t)ry * (g_ver_res - 1) / g_span_y);

	if (g_swap_xy) {
		mx = (lv_coord_t)ry;
		my = (lv_coord_t)rx;
	} else {
		mx = (lv_coord_t)rx;
		my = (lv_coord_t)ry;
	}

	if (g_invert_x)
		mx = (lv_coord_t)(g_hor_res - 1 - mx);
	if (g_invert_y)
		my = (lv_coord_t)(g_ver_res - 1 - my);

	mx = (lv_coord_t)(mx + g_offset_x);
	my = (lv_coord_t)(my + g_offset_y);

	if (mx < 0)
		mx = 0;
	if (my < 0)
		my = 0;
	if (g_hor_res > 0 && mx >= g_hor_res)
		mx = (lv_coord_t)(g_hor_res - 1);
	if (g_ver_res > 0 && my >= g_ver_res)
		my = (lv_coord_t)(g_ver_res - 1);

	*x = mx;
	*y = my;
}

static void evdev_log_touch(lv_coord_t mx, lv_coord_t my, int raw_x, int raw_y,
		lv_indev_state_t state)
{
	if (!g_touch_debug && g_logged_first_touch)
		return;
	if (!g_logged_first_touch) {
		printf("lvgl_input: first touch raw(%d,%d) -> (%d,%d) %s\n",
			raw_x, raw_y, (int)mx, (int)my,
			state == LV_INDEV_STATE_PRESSED ? "DOWN" : "UP");
		g_logged_first_touch = 1;
	} else if (g_touch_debug) {
		printf("lvgl_input: raw(%d,%d) -> (%d,%d) %s\n",
			raw_x, raw_y, (int)mx, (int)my,
			state == LV_INDEV_STATE_PRESSED ? "DOWN" : "UP");
	}
	fflush(stdout);
}

static int evdev_queue_empty(void)
{
	return g_q_head == g_q_tail;
}

static void evdev_queue_push(lv_coord_t x, lv_coord_t y, lv_indev_state_t state)
{
	int next = (g_q_tail + 1) % TOUCH_QUEUE_SIZE;

	if (!evdev_queue_empty()) {
		int last = (g_q_tail + TOUCH_QUEUE_SIZE - 1) % TOUCH_QUEUE_SIZE;

		if (g_queue[last].state == state && g_queue[last].x == x &&
				g_queue[last].y == y)
			return;
	}

	if (next == g_q_head)
		return;

	g_queue[g_q_tail].x = x;
	g_queue[g_q_tail].y = y;
	g_queue[g_q_tail].state = state;
	g_q_tail = next;
}

/*
 * Commit one sample per SYN_REPORT so X/Y/state belong to the same frame.
 * Replay via continue_reading so press+release in one kernel burst still
 * produce a CLICK / PRESSED sequence for LVGL.
 */
static void evdev_drain_events(void)
{
	static lv_coord_t cur_x;
	static lv_coord_t cur_y;
	static lv_indev_state_t cur_state = LV_INDEV_STATE_RELEASED;
	static int frame_raw_x;
	static int frame_raw_y;
	static int frame_have_x;
	static int frame_have_y;
	static int frame_touched;
	struct input_event ev;
	int rd;

	if (g_evdev_fd < 0)
		return;

	while ((rd = read(g_evdev_fd, &ev, sizeof(ev))) == (int)sizeof(ev)) {
		if (ev.type == EV_ABS) {
			if (ev.code == ABS_X || ev.code == ABS_MT_POSITION_X) {
				frame_raw_x = ev.value;
				frame_have_x = 1;
			} else if (ev.code == ABS_Y ||
					ev.code == ABS_MT_POSITION_Y) {
				frame_raw_y = ev.value;
				frame_have_y = 1;
			} else if (ev.code == ABS_MT_TRACKING_ID) {
				cur_state = ev.value >= 0 ?
					LV_INDEV_STATE_PRESSED :
					LV_INDEV_STATE_RELEASED;
				frame_touched = 1;
			}
		} else if (ev.type == EV_KEY &&
				(ev.code == BTN_TOUCH || ev.code == BTN_LEFT)) {
			cur_state = ev.value ? LV_INDEV_STATE_PRESSED :
				LV_INDEV_STATE_RELEASED;
			frame_touched = 1;
		} else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
			static int last_raw_x;
			static int last_raw_y;
			static int seen_x;
			static int seen_y;

			if (frame_have_x) {
				last_raw_x = frame_raw_x;
				seen_x = 1;
			}
			if (frame_have_y) {
				last_raw_y = frame_raw_y;
				seen_y = 1;
			}

			if (seen_x && seen_y &&
					(frame_have_x || frame_have_y || frame_touched)) {
				evdev_map_point(last_raw_x, last_raw_y, &cur_x, &cur_y);
				if (!frame_touched &&
						cur_state == LV_INDEV_STATE_RELEASED)
					cur_state = LV_INDEV_STATE_PRESSED;
				evdev_queue_push(cur_x, cur_y, cur_state);
				evdev_log_touch(cur_x, cur_y, last_raw_x, last_raw_y,
					cur_state);
			}
			frame_have_x = 0;
			frame_have_y = 0;
			frame_touched = 0;
		}
	}
	(void)rd;
}

static void evdev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
	static lv_coord_t last_x;
	static lv_coord_t last_y;
	static lv_indev_state_t last_state = LV_INDEV_STATE_RELEASED;

	(void)indev;

	if (evdev_queue_empty())
		evdev_drain_events();

	if (!evdev_queue_empty()) {
		touch_point_t *p = &g_queue[g_q_head];

		last_x = p->x;
		last_y = p->y;
		last_state = p->state;
		g_q_head = (g_q_head + 1) % TOUCH_QUEUE_SIZE;
	}

	data->point.x = last_x;
	data->point.y = last_y;
	data->state = last_state;
	data->continue_reading = !evdev_queue_empty();
}

int lvgl_input_init(const char *dev_path)
{
	const char *env = getenv("LVGL_EVDEV_DEVICE");
	lv_display_t *disp = lv_display_get_default();
	const char *prefer = dev_path ? dev_path : env;
	int attempt;

	g_hor_res = disp ? lv_display_get_horizontal_resolution(disp) : 720;
	g_ver_res = disp ? lv_display_get_vertical_resolution(disp) : 1280;
	g_dev_path[0] = '\0';
	g_indev = NULL;
	g_evdev_fd = -1;
	g_q_head = 0;
	g_q_tail = 0;
	g_touch_debug = evdev_env_on("LVGL_TOUCH_DEBUG") > 0;
	g_logged_first_touch = 0;

	for (attempt = 0; attempt < 20; attempt++) {
		if (evdev_find_path(prefer, g_dev_path,
				sizeof(g_dev_path)) == 0)
			break;
		if (attempt == 0)
			evdev_list_inputs();
		usleep(100000);
	}

	if (!g_dev_path[0]) {
		fprintf(stderr, "lvgl_input: no touch device found "
			"(set LVGL_EVDEV_DEVICE or check gt9xx driver)\n");
		return -1;
	}

	g_evdev_fd = open(g_dev_path, O_RDONLY | O_NONBLOCK);
	if (g_evdev_fd < 0) {
		fprintf(stderr, "lvgl_input: open(%s) failed: %s\n",
			g_dev_path, strerror(errno));
		g_dev_path[0] = '\0';
		return -1;
	}

	evdev_read_abs_range(g_evdev_fd);
	evdev_apply_calibration();

	g_indev = lv_indev_create();
	if (!g_indev) {
		close(g_evdev_fd);
		g_evdev_fd = -1;
		g_dev_path[0] = '\0';
		return -1;
	}
	lv_indev_set_type(g_indev, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(g_indev, evdev_read_cb);
	if (disp)
		lv_indev_set_display(g_indev, disp);

	printf("lvgl_input: touch ready (fd=%d, abs x[%d..%d] y[%d..%d], "
		"origin=(%d,%d) span=(%d,%d) stretch=(%d,%d) swap=%d "
		"inv=(%d,%d) off=(%d,%d) disp %dx%d)\n",
		g_evdev_fd, g_abs_x_min, g_abs_x_max, g_abs_y_min, g_abs_y_max,
		g_origin_x, g_origin_y, g_span_x, g_span_y, g_stretch_x,
		g_stretch_y, g_swap_xy, g_invert_x, g_invert_y, g_offset_x,
		g_offset_y, g_hor_res, g_ver_res);
	fflush(stdout);
	return 0;
}

void lvgl_input_deinit(void)
{
	if (g_indev) {
		lv_indev_delete(g_indev);
		g_indev = NULL;
	}
	if (g_evdev_fd >= 0) {
		close(g_evdev_fd);
		g_evdev_fd = -1;
	}
	g_dev_path[0] = '\0';
}
