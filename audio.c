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

    /* Main transfer loop */
    while (a->running) {
        /* Wait up to 100 ms so we check a->running frequently */
        int ready = snd_pcm_wait(cap, 100);
        if (!a->running) break;
        if (ready < 0) {
            if (snd_pcm_recover(cap, ready, 1) < 0) {
                snprintf(a->errmsg, sizeof(a->errmsg), "capture error");
                break;
            }
            continue;
        }
        if (ready == 0) continue; /* timeout — loop to re-check running */

        snd_pcm_sframes_t n = snd_pcm_readi(cap, buf, period);
        if (n < 0) {
            if (snd_pcm_recover(cap, (int)n, 1) < 0) {
                snprintf(a->errmsg, sizeof(a->errmsg), "capture read error");
                break;
            }
            continue;
        }

        snd_pcm_sframes_t w = snd_pcm_writei(play, buf, (snd_pcm_uframes_t)n);
        if (w < 0)
            snd_pcm_recover(play, (int)w, 1); /* underrun — drop and continue */
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
    void **hints = NULL;
    if (snd_device_name_hint(-1, "pcm", &hints) < 0 || !hints) return;

    for (void **h = hints; *h && *count < max; h++) {
        /* Only capture-capable devices */
        char *ioid = snd_device_name_get_hint(*h, "IOID");
        int is_input = !ioid || strcmp(ioid, "Input") == 0;
        free(ioid);
        if (!is_input) continue;

        char *name = snd_device_name_get_hint(*h, "NAME");
        if (!name) continue;

        /* Only physical hw: devices, not virtual plugins */
        if (strncmp(name, "hw:", 3) != 0) { free(name); continue; }

        char *desc = snd_device_name_get_hint(*h, "DESC");

        strncpy(names[*count], name, AUDIO_DEV_NAMELEN - 1);
        names[*count][AUDIO_DEV_NAMELEN - 1] = '\0';

        if (desc) {
            /* Flatten multi-line descriptions */
            for (char *p = desc; *p; p++) if (*p == '\n') *p = ' ';
            strncpy(descs[*count], desc, AUDIO_DEV_DESCLEN - 1);
            descs[*count][AUDIO_DEV_DESCLEN - 1] = '\0';
            free(desc);
        } else {
            descs[*count][0] = '\0';
        }

        free(name);
        (*count)++;
    }
    snd_device_name_free_hint(hints);
}
