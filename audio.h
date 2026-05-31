#pragma once
#include <pthread.h>

struct Eq;   /* forward declaration; include eq.h if you need the full type */

#define AUDIO_DEV_MAX      32
#define AUDIO_DEV_NAMELEN  64   /* device/node name */
#define AUDIO_DEV_DESCLEN  128  /* human-readable description */

typedef struct {
    char         device[AUDIO_DEV_NAMELEN];      /* capture device / PipeWire source node */
    char         play_device[AUDIO_DEV_NAMELEN]; /* playback device / PipeWire sink node; empty = default */
    unsigned int buffer_frames;  /* ALSA: period size in frames; PipeWire: ring buffer size in KB */
    volatile int running;    /* 1 while thread is alive */
    int          started;    /* pthread_create was called, needs join */
    pthread_t    thread;
    unsigned int rate;       /* detected sample rate (set by thread) */
    int          channels;   /* detected channel count (set by thread) */
    char         errmsg[128];/* last error; empty if none */

    /* Equalizer — applied in the playback path when non-NULL. */
    struct Eq   *eq;

    /* Recording hook — protected by rec_lock.
       The audio thread calls rec_fn(rec_ctx, pcm, frames, channels) after
       each capture period when rec_fn is non-NULL. */
    pthread_mutex_t rec_lock;
    void (*rec_fn)(void *ctx, const short *pcm, int frames, int channels);
    void  *rec_ctx;
    int    rec_eq_apply;  /* 1 = pass EQ-processed PCM to rec_fn (HAVE_EQ) */
} Audio;

/* Start audio pipe: capture from device → playback on play_device (or default).
   Stops any currently running pipe first. Returns 0 on success. */
int  audio_start(Audio *a, const char *device);

/* Stop audio pipe; safe to call when not running. */
void audio_stop(Audio *a);

/* Enumerate capture devices.
   PipeWire build: enumerates Audio/Source nodes.
   ALSA build:     enumerates hw:X,Y capture devices. */
void audio_enum_devices(char names[][AUDIO_DEV_NAMELEN],
                        char descs[][AUDIO_DEV_DESCLEN],
                        int *count, int max);

/* Enumerate playback devices. Index 0 is always "" / "(default)".
   PipeWire build: subsequent entries are Audio/Sink nodes.
   ALSA build:     subsequent entries are hw:CARD=X,DEV=Y devices. */
void audio_enum_play_devices(char names[][AUDIO_DEV_NAMELEN],
                             char descs[][AUDIO_DEV_DESCLEN],
                             int *count, int max);

#ifdef HAVE_PIPEWIRE
/* Runtime PipeWire library version string (e.g. "1.0.5"). */
const char *audio_pipewire_version(void);
#else
/* Runtime ALSA library version string (e.g. "1.2.10"). */
const char *audio_alsa_version(void);
#endif

/* Detect the audio capture device associated with a V4L2 radio device.
   radio_dev: path like "/dev/radio0"
   out: receives the device/node name on success
   Returns 1 if a device was found, 0 otherwise. */
int audio_autodetect(const char *radio_dev, char *out, int out_size);
