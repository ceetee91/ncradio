#pragma once
#include <stdint.h>
#include <pthread.h>

#define FREQ_MIN_HZ   87500000U   /* 87.50 MHz */
#define FREQ_MAX_HZ  108000000U   /* 108.00 MHz */
#define FREQ_STEP_HZ    100000U   /* 0.10 MHz */
#define SCAN_SIGNAL_THRESH 20000  /* out of 65535 */

typedef struct {
    int fd;
    int cap_low;        /* V4L2 freq unit: 1=62.5 Hz, 0=62.5 kHz */
    uint32_t freq_hz;   /* current frequency in Hz (pre-scan value during scan) */
    int volume;         /* 0-100 */
    int muted;

    /* scan state (mutex protects found_* and scan_pos_hz) */
    volatile int scanning;    /* cleared by thread on completion, or by caller to abort */
    int          scan_started;/* thread was created and needs pthread_join */
    pthread_t    scan_thread;
    pthread_mutex_t mutex;
    uint32_t found_freqs[256];
    int      found_count;
    uint32_t scan_pos_hz;     /* frequency currently being probed */
} Radio;

int      radio_open(Radio *r, const char *device);
void     radio_close(Radio *r);
int      radio_set_freq(Radio *r, uint32_t hz);
uint32_t radio_get_freq(Radio *r);
int      radio_get_signal(Radio *r);    /* 0-100 */
int      radio_set_volume(Radio *r, int vol);
int      radio_mute(Radio *r, int mute);
int      radio_seek(Radio *r, int fwd); /* 1=forward, 0=backward; returns -1 on error */
void     radio_start_scan(Radio *r);
void     radio_stop_scan(Radio *r);     /* safe to call even if scan already done */
