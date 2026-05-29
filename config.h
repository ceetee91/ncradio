#pragma once
#include <stdint.h>

#define MAX_PRESETS   64
#define NAME_MAX_LEN  32
#define CONFIG_FILE   "/.ncradio.conf"

/* Default scan settings */
#define DEFAULT_SCAN_STEP_HZ        100000
#define DEFAULT_SIGNAL_THRESH_PCT       30
#define DEFAULT_RDS_NAMES                1
#define DEFAULT_AUDIO_ENABLED            0
#define DEFAULT_AUDIO_MUTE_SCAN          1
#define DEFAULT_AUDIO_MUTE_SEEK          1

typedef struct {
    /* stations */
    uint32_t freqs[MAX_PRESETS];
    char     names[MAX_PRESETS][NAME_MAX_LEN + 1];
    int      count;

    /* scan settings */
    uint32_t scan_step_hz;
    int      signal_threshold_pct;
    int      rds_names;

    /* volume (0-100; 0 = not yet saved, use default) */
    int      volume;

    /* audio settings */
    int      audio_enabled;
    char     audio_device[64]; /* ALSA capture device, e.g. "hw:2,0" */
    int      audio_mute_scan;  /* stop audio pipe while scanning */
    int      audio_mute_seek;  /* stop audio pipe while seeking */
} Config;

int  config_load(Config *c);
int  config_save(const Config *c);
void config_add(Config *c, uint32_t hz, const char *name);
void config_del(Config *c, int idx);
int  config_find(const Config *c, uint32_t hz);
