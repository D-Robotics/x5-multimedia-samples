#ifndef SYS_STATUS_H_
#define SYS_STATUS_H_

typedef struct {
	int battery_pct;     /* 0..100, -1 if unknown */
	int battery_present; /* 1 if a power_supply was found */
	int charging;        /* 1 charging / AC / full */
	int temp_c;          /* board/cpu temp °C, -999 if unknown */
	long free_mb;        /* free space on save dir filesystem */
	long total_mb;
	int storage_full;    /* 1 if free_mb below threshold */
} sys_status_t;

/* Prefers CAM_SAVE_DIR / path for storage check. */
void sys_status_poll(sys_status_t *st, const char *save_dir);

int sys_status_free_mb(const char *path);

#endif
