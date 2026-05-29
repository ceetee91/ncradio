#pragma once
#include <stdint.h>

#define MAX_PRESETS   64
#define NAME_MAX_LEN  32
#define CONFIG_FILE   "/.ncradio.conf"

/* Default scan settings (used when config has no stored value) */
#define DEFAULT_SCAN_STEP_HZ        100000  /* 0.10 MHz */
#define DEFAULT_SIGNAL_THRESH_PCT       30  /* 30% of full scale */
#define DEFAULT_RDS_NAMES                1  /* save RDS names during scan */

typedef struct {
    /* stations */
    uint32_t freqs[MAX_PRESETS];
    char     names[MAX_PRESETS][NAME_MAX_LEN + 1];
    int      count;

    /* scan settings */
    uint32_t scan_step_hz;        /* frequency step during scan, Hz */
    int      signal_threshold_pct;/* minimum signal to record a station, 0-100 */
    int      rds_names;           /* 1 = collect RDS name during scan */
} Config;

int  config_load(Config *c);
int  config_save(const Config *c);
/* name may be "" — inserted in sorted order, no-op if freq already present */
void config_add(Config *c, uint32_t hz, const char *name);
void config_del(Config *c, int idx);
int  config_find(const Config *c, uint32_t hz);
