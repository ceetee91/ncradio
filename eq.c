#include "eq.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

const int EQ_FREQS[EQ_BANDS] = {
    32, 64, 125, 250, 500, 1000, 2000, 4000, 8000, 12000, 16000
};

const char *EQ_FREQ_LABELS[EQ_BANDS] = {
    "32", "64", "125", "250", "500", "1k", "2k", "4k", "8k", "12k", "16k"
};

const char *EQ_PRESET_NAMES[EQ_PRESET_COUNT] = {
    "Flat", "Rock", "Pop", "Classical", "Jazz", "Electronic"
};

/* Gains in dB for each preset, band order matches EQ_FREQS. */
const float EQ_PRESETS[EQ_PRESET_COUNT][EQ_BANDS] = {
    /* Flat */
    { 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0 },
    /* Rock — V-curve: punchy lows, scooped mids, crisp highs */
    { 4,  3,  2,  0, -1, -2, -1,  0,  2,  3,  3 },
    /* Pop — forward mids for vocal clarity */
    {-1, -1,  0,  2,  4,  4,  3,  1,  0, -1, -1 },
    /* Classical — warm and airy, gentle presence rolloff */
    { 0,  0,  0,  0,  0,  0, -2, -3, -3, -2,  1 },
    /* Jazz — rich low-mids, smooth top */
    { 2,  2,  0,  2,  3,  2,  0, -1,  0,  1,  1 },
    /* Electronic — deep bass, bright sparkle */
    { 6,  5,  3,  1,  0, -2, -2,  0,  3,  4,  5 },
};

/* Compute biquad peaking filter coefficients (Audio EQ Cookbook).
   Q = √2 ≈ 1.414, suitable for 1-octave graphic EQ bands. */
static void compute_band(EqBand *b, float gain_db, int freq_hz, double sr)
{
    double A     = pow(10.0, gain_db / 40.0);
    double w0    = 2.0 * M_PI * freq_hz / sr;
    double alpha = sin(w0) / (2.0 * 1.41421356);
    double a0    = 1.0 + alpha / A;

    b->b0 = (1.0 + alpha * A) / a0;
    b->b1 = (-2.0 * cos(w0))  / a0;
    b->b2 = (1.0 - alpha * A) / a0;
    b->a1 = (-2.0 * cos(w0))  / a0;
    b->a2 = (1.0 - alpha / A) / a0;

    memset(b->x1, 0, sizeof b->x1);
    memset(b->x2, 0, sizeof b->x2);
    memset(b->y1, 0, sizeof b->y1);
    memset(b->y2, 0, sizeof b->y2);
}

void eq_init(Eq *eq, double sample_rate)
{
    eq->sample_rate = sample_rate > 0 ? sample_rate : 48000.0;
    for (int i = 0; i < EQ_BANDS; i++)
        compute_band(&eq->bands[i], eq->gains_db[i], EQ_FREQS[i], eq->sample_rate);
}

void eq_set_gain(Eq *eq, int band, float gain_db)
{
    if (band < 0 || band >= EQ_BANDS) return;
    pthread_mutex_lock(&eq->lock);
    eq->gains_db[band] = gain_db;
    compute_band(&eq->bands[band], gain_db, EQ_FREQS[band], eq->sample_rate);
    pthread_mutex_unlock(&eq->lock);
}

void eq_load_preset(Eq *eq, int preset_idx)
{
    if (preset_idx < 0 || preset_idx >= EQ_PRESET_COUNT) return;
    pthread_mutex_lock(&eq->lock);
    for (int i = 0; i < EQ_BANDS; i++) {
        eq->gains_db[i] = EQ_PRESETS[preset_idx][i];
        compute_band(&eq->bands[i], eq->gains_db[i], EQ_FREQS[i], eq->sample_rate);
    }
    pthread_mutex_unlock(&eq->lock);
}

/* Process interleaved S16_LE in place using Direct Form I biquad cascade. */
void eq_process(Eq *eq, short *buf, int frames, int channels)
{
    pthread_mutex_lock(&eq->lock);
    if (!eq->enabled || frames <= 0) {
        pthread_mutex_unlock(&eq->lock);
        return;
    }
    if (channels < 1) channels = 1;
    if (channels > 2) channels = 2;

    for (int i = 0; i < frames * channels; i++) {
        float x  = buf[i] / 32768.0f;
        int   ch = i % channels;

        for (int b = 0; b < EQ_BANDS; b++) {
            EqBand *bd = &eq->bands[b];
            float y = (float)(bd->b0 * x
                             + bd->b1 * bd->x1[ch]
                             + bd->b2 * bd->x2[ch]
                             - bd->a1 * bd->y1[ch]
                             - bd->a2 * bd->y2[ch]);
            bd->x2[ch] = bd->x1[ch]; bd->x1[ch] = x;
            bd->y2[ch] = bd->y1[ch]; bd->y1[ch] = y;
            x = y;
        }

        buf[i] = x >  1.0f ?  32767 :
                 x < -1.0f ? -32768 : (short)(x * 32767.0f);
    }
    pthread_mutex_unlock(&eq->lock);
}
