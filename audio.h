#pragma once
#include <pthread.h>

#define AUDIO_DEV_MAX      32
#define AUDIO_DEV_NAMELEN  64   /* "hw:X,Y" */
#define AUDIO_DEV_DESCLEN  128  /* human-readable card/device name */

typedef struct {
    char         device[AUDIO_DEV_NAMELEN]; /* ALSA capture device */
    volatile int running;    /* 1 while thread is alive */
    int          started;    /* pthread_create was called, needs join */
    pthread_t    thread;
    unsigned int rate;       /* detected sample rate (set by thread) */
    int          channels;   /* detected channel count (set by thread) */
    char         errmsg[128];/* last error; empty if none */
} Audio;

/* Start audio pipe: capture from device → playback on "default".
   Stops any currently running pipe first. Returns 0 on success. */
int  audio_start(Audio *a, const char *device);

/* Stop audio pipe; safe to call when not running. */
void audio_stop(Audio *a);

/* Enumerate physical ALSA capture devices (hw:X,Y only).
   Fills parallel names[]/descs[] arrays; sets *count. */
void audio_enum_devices(char names[][AUDIO_DEV_NAMELEN],
                        char descs[][AUDIO_DEV_DESCLEN],
                        int *count, int max);

/* Detect the ALSA capture device associated with a V4L2 radio device.
   radio_dev: path like "/dev/radio0"
   out: receives "hw:CARD=<id>,DEV=0" style string on success
   Returns 1 if a device was found, 0 otherwise. */
int audio_autodetect(const char *radio_dev, char *out, int out_size);
