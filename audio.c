#include "audio.h"
#include <alsa/asoundlib.h>
#include <alloca.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Candidate capture rates tried in preference order (highest first). */
static const unsigned int RATES[] = { 96000, 48000, 44100, 32000, 22050, 16000, 0 };

/* ── audio transfer thread ───────────────────────────────────────────── */

static void *audio_fn(void *arg)
{
    Audio *a = arg;
    snd_pcm_t *cap  = NULL;
    snd_pcm_t *play = NULL;
    void      *buf  = NULL;

    /* Open capture device */
    if (snd_pcm_open(&cap, a->device, SND_PCM_STREAM_CAPTURE, 0) < 0) {
        snprintf(a->errmsg, sizeof(a->errmsg),
                 "cannot open '%s'", a->device);
        goto done;
    }

    /* Probe supported sample rate (snd_pcm_hw_params_test_rate is non-modifying) */
    snd_pcm_hw_params_t *cap_hw;
    snd_pcm_hw_params_alloca(&cap_hw);
    snd_pcm_hw_params_any(cap, cap_hw);

    unsigned int rate = 44100;
    for (int i = 0; RATES[i]; i++) {
        if (snd_pcm_hw_params_test_rate(cap, cap_hw, RATES[i], 0) == 0) {
            rate = RATES[i];
            break;
        }
    }

    /* Configure capture — try stereo, fall back to mono */
    snd_pcm_hw_params_any(cap, cap_hw);
    snd_pcm_hw_params_set_access(cap, cap_hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(cap, cap_hw, SND_PCM_FORMAT_S16_LE);

    int channels = 2;
    if (snd_pcm_hw_params_set_channels(cap, cap_hw, 2) < 0) {
        channels = 1;
        snd_pcm_hw_params_set_channels(cap, cap_hw, 1);
    }

    snd_pcm_hw_params_set_rate(cap, cap_hw, rate, 0);

    snd_pcm_uframes_t period = 4096;
    snd_pcm_hw_params_set_period_size_near(cap, cap_hw, &period, 0);

    if (snd_pcm_hw_params(cap, cap_hw) < 0) {
        snprintf(a->errmsg, sizeof(a->errmsg),
                 "cannot configure capture device");
        goto done;
    }
    snd_pcm_hw_params_get_period_size(cap_hw, &period, NULL);
    snd_pcm_hw_params_get_rate(cap_hw, &rate, NULL);

    a->rate     = rate;
    a->channels = channels;

    if (snd_pcm_prepare(cap) < 0 || snd_pcm_start(cap) < 0) {
        snprintf(a->errmsg, sizeof(a->errmsg), "cannot start capture device");
        goto done;
    }

    /* Open and configure playback ("default" = PulseAudio/PipeWire/hw) */
    if (snd_pcm_open(&play, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        snprintf(a->errmsg, sizeof(a->errmsg),
                 "cannot open default playback device");
        goto done;
    }

    snd_pcm_hw_params_t *play_hw;
    snd_pcm_hw_params_alloca(&play_hw);
    snd_pcm_hw_params_any(play, play_hw);
    snd_pcm_hw_params_set_access(play, play_hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(play, play_hw, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(play, play_hw, (unsigned int)channels);
    snd_pcm_hw_params_set_rate_near(play, play_hw, &rate, 0);

    if (snd_pcm_hw_params(play, play_hw) < 0) {
        snprintf(a->errmsg, sizeof(a->errmsg),
                 "cannot configure playback device");
        goto done;
    }

    /* Transfer buffer: one period of S16_LE interleaved samples */
    size_t buf_bytes = period * (size_t)channels * 2;
    buf = malloc(buf_bytes);
    if (!buf) {
        snprintf(a->errmsg, sizeof(a->errmsg), "out of memory");
        goto done;
    }

    /* Main transfer loop — snd_pcm_readi blocks for one period (~100 ms),
       auto-starts capture after snd_pcm_recover, and checks running on wakeup. */
    while (a->running) {
        snd_pcm_sframes_t n = snd_pcm_readi(cap, buf, period);
        if (!a->running) break;
        if (n < 0) {
            if (snd_pcm_recover(cap, (int)n, 1) < 0) {
                snprintf(a->errmsg, sizeof(a->errmsg), "capture read error");
                break;
            }
            continue;
        }

        snd_pcm_sframes_t w = snd_pcm_writei(play, buf, (snd_pcm_uframes_t)n);
        if (w < 0)
            snd_pcm_recover(play, (int)w, 1);
    }

done:
    free(buf);
    if (play) snd_pcm_close(play);
    if (cap)  snd_pcm_close(cap);
    a->running = 0;
    return NULL;
}

/* ── public API ──────────────────────────────────────────────────────── */

int audio_start(Audio *a, const char *device)
{
    audio_stop(a);
    strncpy(a->device, device ? device : "", AUDIO_DEV_NAMELEN - 1);
    a->device[AUDIO_DEV_NAMELEN - 1] = '\0';
    a->errmsg[0] = '\0';
    a->rate      = 0;
    a->channels  = 0;
    a->running   = 1;
    a->started   = 1;
    if (pthread_create(&a->thread, NULL, audio_fn, a) != 0) {
        a->running = 0;
        a->started = 0;
        snprintf(a->errmsg, sizeof(a->errmsg), "pthread_create failed");
        return -1;
    }
    return 0;
}

void audio_stop(Audio *a)
{
    if (!a->started) return;
    a->running = 0;
    pthread_join(a->thread, NULL);
    a->started = 0;
}

void audio_enum_devices(char names[][AUDIO_DEV_NAMELEN],
                        char descs[][AUDIO_DEV_DESCLEN],
                        int *count, int max)
{
    *count = 0;
    int card = -1;

    while (*count < max && snd_card_next(&card) >= 0 && card >= 0) {
        char ctl_name[16];
        snprintf(ctl_name, sizeof(ctl_name), "hw:%d", card);

        snd_ctl_t *ctl;
        if (snd_ctl_open(&ctl, ctl_name, 0) < 0) continue;

        snd_ctl_card_info_t *ci;
        snd_ctl_card_info_alloca(&ci);
        if (snd_ctl_card_info(ctl, ci) < 0) { snd_ctl_close(ctl); continue; }

        const char *card_id   = snd_ctl_card_info_get_id(ci);
        const char *card_name = snd_ctl_card_info_get_name(ci);

        int dev = -1;
        while (*count < max && snd_ctl_pcm_next_device(ctl, &dev) >= 0 && dev >= 0) {
            snd_pcm_info_t *pi;
            snd_pcm_info_alloca(&pi);
            snd_pcm_info_set_device(pi, (unsigned int)dev);
            snd_pcm_info_set_subdevice(pi, 0);
            snd_pcm_info_set_stream(pi, SND_PCM_STREAM_CAPTURE);
            if (snd_ctl_pcm_info(ctl, pi) < 0) continue;

            const char *dev_name = snd_pcm_info_get_name(pi);

            snprintf(names[*count], AUDIO_DEV_NAMELEN,
                     "hw:CARD=%s,DEV=%d", card_id, dev);

            snprintf(descs[*count], AUDIO_DEV_DESCLEN,
                     "%s, %s", card_name, dev_name ? dev_name : "");

            (*count)++;
        }
        snd_ctl_close(ctl);
    }
}
