#pragma once
#include <stdint.h>

#define MAX_PRESETS      64
#define EQ_BANDS         11
#define EQ_CUSTOM_MAX    16
#define EQ_CUSTOM_NAMELEN 32
#define NAME_MAX_LEN  32
#define CONFIG_FILE   "/.ncradio.conf"

/* Default scan settings */
#define DEFAULT_SCAN_STEP_HZ        100000
#define DEFAULT_SIGNAL_THRESH_PCT       50
#define DEFAULT_RDS_NAMES                1
#define DEFAULT_AUDIO_ENABLED            0
#define DEFAULT_AUDIO_MUTE_SCAN          1
#define DEFAULT_AUDIO_MUTE_SEEK          1
#define DEFAULT_AUDIO_BUFFER_FRAMES   1024  /* capture/playback period in frames */

/* Default MP3 recording settings */
#define DEFAULT_RECORD_BITRATE         128   /* kbps */
#define DEFAULT_RECORD_STEREO            1   /* 1=stereo 0=mono */
#define DEFAULT_RECORD_SAMPLERATE    44100   /* Hz */
#define DEFAULT_RECORD_EQ_ENABLED        1   /* 1=apply EQ to recordings */

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

    /* last tuned frequency in Hz (0 = not yet saved) */
    uint32_t last_freq_hz;

    /* audio settings */
    int      audio_enabled;
    char     audio_device[64];      /* capture device / PipeWire source node */
    char     audio_play_device[64]; /* playback device / PipeWire sink node; empty = default */
    int      audio_buffer_frames;   /* period size in frames for capture+playback */
    int      audio_mute_scan;       /* stop audio pipe while scanning */
    int      audio_mute_seek;       /* stop audio pipe while seeking */

    /* MP3 recording settings */
    int      record_bitrate;      /* kbps: 64/96/128/192/256/320 */
    int      record_stereo;       /* 1=stereo 0=mono */
    int      record_samplerate;   /* Hz: 22050/44100/48000 */
    int      record_eq_enabled;   /* 1=apply EQ curve to recordings when EQ is on */

    /* Equalizer settings */
    int      eq_enabled;
    float    eq_gains[EQ_BANDS];
    char     eq_active_preset[EQ_CUSTOM_NAMELEN]; /* name of active preset; "" = unsaved */

    /* User-saved EQ presets */
    struct {
        char  name[EQ_CUSTOM_NAMELEN];
        float gains[EQ_BANDS];
    }        eq_custom[EQ_CUSTOM_MAX];
    int      eq_custom_count;
} Config;

int  config_load(Config *c);
int  config_save(const Config *c);
void config_add(Config *c, uint32_t hz, const char *name);
void config_del(Config *c, int idx);
int  config_find(const Config *c, uint32_t hz);
int  config_eq_preset_add(Config *c, const char *name, const float gains[EQ_BANDS]);
void config_eq_preset_del(Config *c, int idx);
