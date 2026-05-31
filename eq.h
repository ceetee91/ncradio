#pragma once
#include <pthread.h>

#ifndef EQ_BANDS
#define EQ_BANDS 11
#endif

extern const int   EQ_FREQS[EQ_BANDS];
extern const char *EQ_FREQ_LABELS[EQ_BANDS];

#define EQ_PRESET_COUNT 6
extern const char  *EQ_PRESET_NAMES[EQ_PRESET_COUNT];
extern const float  EQ_PRESETS[EQ_PRESET_COUNT][EQ_BANDS];

typedef struct {
    double b0, b1, b2, a1, a2;
    float  x1[2], x2[2];
    float  y1[2], y2[2];
} EqBand;

typedef struct Eq {
    pthread_mutex_t lock;
    int     enabled;
    float   gains_db[EQ_BANDS];   /* −12 … +12 dB per band */
    EqBand  bands[EQ_BANDS];
    double  sample_rate;
} Eq;

void eq_init(Eq *eq, double sample_rate);
void eq_set_gain(Eq *eq, int band, float gain_db);
void eq_load_preset(Eq *eq, int preset_idx);
void eq_process(Eq *eq, short *samples, int frames, int channels);
