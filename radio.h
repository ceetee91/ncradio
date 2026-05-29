#pragma once
#include <stdint.h>
#include <pthread.h>
#include <linux/videodev2.h>
#include "rds.h"
#include "config.h"   /* NAME_MAX_LEN, MAX_PRESETS */

#define FREQ_MIN_HZ        87500000U  /* 87.50 MHz */
#define FREQ_MAX_HZ       108000000U  /* 108.00 MHz */
#define FREQ_STEP_HZ         100000U  /* 0.10 MHz  — fallback default */
#define SCAN_SIGNAL_THRESH    20000   /* ~30% of 65535 — fallback default */

typedef struct {
    int fd;
    int cap_low;        /* V4L2 freq unit: 1=62.5 Hz, 0=62.5 kHz */
    int rds_capable;    /* hardware supports RDS */
    int stereo;         /* 1 if currently receiving stereo */
    uint32_t freq_min_hz;  /* device-reported tunable range (from VIDIOC_G_TUNER) */
    uint32_t freq_max_hz;
    uint32_t freq_hz;   /* current tuned frequency in Hz (pre-scan during scan) */
    int volume;         /* 0-100 */
    int muted;
    struct v4l2_queryctrl volume_ctrl;  /* queried once at open; max>min means supported */
    RdsDecoder rds;     /* live RDS decoder state */

    /* scan parameters — set from config before calling radio_start_scan */
    uint32_t scan_step_hz;   /* frequency step; 0 → use FREQ_STEP_HZ */
    int      scan_threshold; /* signal threshold 0-65535; 0 → SCAN_SIGNAL_THRESH */
    int      scan_rds_names; /* 1 = collect RDS names during scan */

    /* seek state (software seek: step + threshold, not hardware VIDIOC_S_HW_FREQ_SEEK) */
    volatile int  seeking;
    int           seek_started;
    pthread_t     seek_thread;
    uint32_t      seek_result_hz;  /* found frequency, or 0 if none */
    uint32_t      seek_step_hz;
    int           seek_threshold;
    int           seek_fwd;

    /* scan state — mutex protects found_*, scan_pos_hz */
    volatile int    scanning;
    int             scan_started;
    pthread_t       scan_thread;
    pthread_mutex_t mutex;
    uint32_t found_freqs[MAX_PRESETS];
    char     found_names[MAX_PRESETS][NAME_MAX_LEN + 1];
    int      found_count;
    uint32_t scan_pos_hz;    /* frequency currently being probed */
} Radio;

int      radio_open(Radio *r, const char *device);
void     radio_close(Radio *r);
int      radio_set_freq(Radio *r, uint32_t hz);
uint32_t radio_get_freq(Radio *r);
int      radio_get_signal(Radio *r);    /* 0-100; also updates r->stereo */
int      radio_set_volume(Radio *r, int vol);
int      radio_mute(Radio *r, int mute);
int      radio_seek(Radio *r, int fwd); /* 1=fwd 0=back; -1 on hw error */
void     radio_read_rds(Radio *r);      /* non-blocking drain of pending RDS blocks */
void     radio_start_scan(Radio *r);
void     radio_stop_scan(Radio *r);     /* safe even if scan already done */
/* Software seek: step by step_hz, stop at first freq >= threshold (0-65535) */
void     radio_start_seek(Radio *r, int fwd, uint32_t step_hz, int threshold);
void     radio_stop_seek(Radio *r);     /* safe even if seek already done */
