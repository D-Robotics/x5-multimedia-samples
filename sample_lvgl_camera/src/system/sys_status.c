#include "sys_status.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>

static int env_int(const char *name, int def)
{
	const char *v = getenv(name);

	if (v && v[0])
		return atoi(v);
	return def;
}

static int read_int_file(const char *path, int *out)
{
	FILE *fp = fopen(path, "r");
	int v = 0;

	if (!fp)
		return -1;
	if (fscanf(fp, "%d", &v) != 1) {
		fclose(fp);
		return -1;
	}
	fclose(fp);
	*out = v;
	return 0;
}

static int read_str_file(const char *path, char *buf, size_t len)
{
	FILE *fp = fopen(path, "r");

	if (!fp)
		return -1;
	if (!fgets(buf, (int)len, fp)) {
		fclose(fp);
		return -1;
	}
	fclose(fp);
	buf[strcspn(buf, "\r\n")] = '\0';
	return 0;
}

static void poll_battery(sys_status_t *st)
{
	DIR *d;
	struct dirent *de;
	char path[384];
	char type[64];
	char status[64];
	int capacity = -1;
	int best = -1;
	int charging = 0;
	int present = 0;

	st->battery_pct = -1;
	st->battery_present = 0;
	st->charging = 0;

	d = opendir("/sys/class/power_supply");
	if (!d)
		return;

	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.' || strlen(de->d_name) > 64)
			continue;
		snprintf(path, sizeof(path),
			"/sys/class/power_supply/%s/type", de->d_name);
		if (read_str_file(path, type, sizeof(type)) != 0)
			continue;
		if (strcasecmp(type, "Battery") != 0 &&
				strcasecmp(type, "USB") != 0 &&
				strcasecmp(type, "Mains") != 0)
			continue;

		present = 1;
		snprintf(path, sizeof(path),
			"/sys/class/power_supply/%s/capacity", de->d_name);
		if (read_int_file(path, &capacity) == 0 && capacity >= 0 &&
				capacity <= 100 && capacity > best)
			best = capacity;

		snprintf(path, sizeof(path),
			"/sys/class/power_supply/%s/status", de->d_name);
		if (read_str_file(path, status, sizeof(status)) == 0) {
			if (strcasecmp(status, "Charging") == 0 ||
					strcasecmp(status, "Full") == 0)
				charging = 1;
		}
		if (strcasecmp(type, "Mains") == 0 ||
				strcasecmp(type, "USB") == 0)
			charging = 1;
	}
	closedir(d);

	st->battery_present = present;
	st->battery_pct = best;
	st->charging = charging;
}

static void poll_temp(sys_status_t *st)
{
	static const char *const paths[] = {
		"/sys/class/thermal/thermal_zone0/temp",
		"/sys/class/thermal/thermal_zone1/temp",
		"/sys/devices/virtual/thermal/thermal_zone0/temp",
		NULL,
	};
	int i;
	int raw = 0;

	st->temp_c = -999;
	for (i = 0; paths[i]; i++) {
		if (read_int_file(paths[i], &raw) != 0)
			continue;
		/* millidegree C on most kernels */
		if (raw > 200)
			st->temp_c = raw / 1000;
		else
			st->temp_c = raw;
		return;
	}
}

int sys_status_free_mb(const char *path)
{
	struct statvfs st;
	const char *p = path && path[0] ? path : "/";
	unsigned long bsize;

	if (statvfs(p, &st) != 0)
		return -1;
	bsize = st.f_frsize ? st.f_frsize : st.f_bsize;
	if (bsize == 0)
		return -1;
	return (int)((unsigned long long)st.f_bavail * bsize /
		(1024ull * 1024ull));
}

/*
 * Prefer the filesystem that actually holds saves. For
 * /userdata/lvgl_camera, query /userdata (the mount) — some boards report
 * odd numbers on a deep subdir while df /userdata is correct.
 */
static void poll_storage(sys_status_t *st, const char *save_dir)
{
	struct statvfs vfs;
	char probe[256];
	const char *p;
	unsigned long bsize;
	static int logged;

	st->free_mb = -1;
	st->total_mb = -1;

	p = (save_dir && save_dir[0]) ? save_dir : "/userdata";
	snprintf(probe, sizeof(probe), "%s", p);

	/* /userdata/... → measure /userdata mount. */
	if (strncmp(probe, "/userdata", 9) == 0 &&
			(probe[9] == '\0' || probe[9] == '/')) {
		snprintf(probe, sizeof(probe), "/userdata");
	}

	while (statvfs(probe, &vfs) != 0) {
		char *slash;

		if (strcmp(probe, "/") == 0)
			return;
		slash = strrchr(probe, '/');
		if (!slash)
			return;
		if (slash == probe) {
			probe[1] = '\0';
		} else {
			*slash = '\0';
		}
	}

	bsize = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
	if (bsize == 0)
		return;

	st->free_mb = (long)((unsigned long long)vfs.f_bavail * bsize /
		(1024ull * 1024ull));
	st->total_mb = (long)((unsigned long long)vfs.f_blocks * bsize /
		(1024ull * 1024ull));

	if (!logged) {
		logged = 1;
		printf("[ui] storage: path=%s probe=%s free=%ldMB total=%ldMB "
			"(bavail=%llu bsize=%lu)\n",
			save_dir && save_dir[0] ? save_dir : "(null)",
			probe, st->free_mb, st->total_mb,
			(unsigned long long)vfs.f_bavail, bsize);
		fflush(stdout);
	}
}

void sys_status_poll(sys_status_t *st, const char *save_dir)
{
	int thresh;

	if (!st)
		return;
	memset(st, 0, sizeof(*st));
	st->battery_pct = -1;
	st->temp_c = -999;
	st->free_mb = -1;
	st->total_mb = -1;

	poll_battery(st);
	poll_temp(st);
	poll_storage(st, save_dir);

	thresh = env_int("CAM_STORAGE_MIN_MB", 64);
	st->storage_full = (st->free_mb >= 0 && st->free_mb < thresh) ? 1 : 0;
}
