#include "radio.h"
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

static uint32_t hz_to_v4l2(const Radio *r, uint32_t hz)
{
    /* V4L2 freq unit is 62.5 Hz (cap_low=1) or 62.5 kHz (cap_low=0) */
    return r->cap_low ? (uint32_t)(hz / 62.5) : hz / 62500;
}

static uint32_t v4l2_to_hz(const Radio *r, uint32_t v)
{
    return r->cap_low ? (uint32_t)(v * 62.5) : v * 62500;
}

static void msleep(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

int radio_open(Radio *r, const char *device)
{
    memset(r, 0, sizeof(*r));
    pthread_mutex_init(&r->mutex, NULL);

    r->fd = open(device, O_RDWR);
    if (r->fd < 0) return -1;

    struct v4l2_tuner tuner = { .index = 0 };
    if (ioctl(r->fd, VIDIOC_G_TUNER, &tuner) == 0)
        r->cap_low = !!(tuner.capability & V4L2_TUNER_CAP_LOW);

    r->freq_hz = radio_get_freq(r);
    if (r->freq_hz < FREQ_MIN_HZ || r->freq_hz > FREQ_MAX_HZ)
        r->freq_hz = 98500000;

    r->volume = 80;
    radio_set_volume(r, r->volume);
    return 0;
}

void radio_close(Radio *r)
{
    radio_stop_scan(r);
    if (r->fd >= 0) { close(r->fd); r->fd = -1; }
    pthread_mutex_destroy(&r->mutex);
}

int radio_set_freq(Radio *r, uint32_t hz)
{
    if (hz < FREQ_MIN_HZ) hz = FREQ_MIN_HZ;
    if (hz > FREQ_MAX_HZ) hz = FREQ_MAX_HZ;
    struct v4l2_frequency vf = {
        .tuner = 0, .type = V4L2_TUNER_RADIO,
        .frequency = hz_to_v4l2(r, hz)
    };
    if (ioctl(r->fd, VIDIOC_S_FREQUENCY, &vf) < 0) return -1;
    r->freq_hz = hz;
    return 0;
}

uint32_t radio_get_freq(Radio *r)
{
    struct v4l2_frequency vf = { .tuner = 0, .type = V4L2_TUNER_RADIO };
    if (ioctl(r->fd, VIDIOC_G_FREQUENCY, &vf) < 0) return r->freq_hz;
    return v4l2_to_hz(r, vf.frequency);
}

int radio_get_signal(Radio *r)
{
    struct v4l2_tuner t = { .index = 0 };
    if (ioctl(r->fd, VIDIOC_G_TUNER, &t) < 0) return 0;
    return (int)((t.signal * 100ULL) / 65535);
}

int radio_set_volume(Radio *r, int vol)
{
    if (vol < 0)   vol = 0;
    if (vol > 100) vol = 100;
    r->volume = vol;
    struct v4l2_control c = {
        .id = V4L2_CID_AUDIO_VOLUME,
        .value = (vol * 65535) / 100
    };
    return ioctl(r->fd, VIDIOC_S_CTRL, &c);
}

int radio_mute(Radio *r, int mute)
{
    r->muted = !!mute;
    struct v4l2_control c = { .id = V4L2_CID_AUDIO_MUTE, .value = mute };
    return ioctl(r->fd, VIDIOC_S_CTRL, &c);
}

int radio_seek(Radio *r, int fwd)
{
    struct v4l2_hw_freq_seek seek = {
        .tuner       = 0,
        .type        = V4L2_TUNER_RADIO,
        .seek_upward = fwd ? 1 : 0,
        .wrap_around = 1
    };
    if (ioctl(r->fd, VIDIOC_S_HW_FREQ_SEEK, &seek) < 0) return -1;
    r->freq_hz = radio_get_freq(r);
    return 0;
}

static void *scan_fn(void *arg)
{
    Radio *r = arg;

    pthread_mutex_lock(&r->mutex);
    r->found_count = 0;
    pthread_mutex_unlock(&r->mutex);

    for (uint32_t f = FREQ_MIN_HZ; f <= FREQ_MAX_HZ && r->scanning; f += FREQ_STEP_HZ) {
        pthread_mutex_lock(&r->mutex);
        r->scan_pos_hz = f;
        pthread_mutex_unlock(&r->mutex);

        struct v4l2_frequency vf = {
            .tuner = 0, .type = V4L2_TUNER_RADIO,
            .frequency = hz_to_v4l2(r, f)
        };
        ioctl(r->fd, VIDIOC_S_FREQUENCY, &vf);
        msleep(120);

        if (!r->scanning) break;

        struct v4l2_tuner t = { .index = 0 };
        ioctl(r->fd, VIDIOC_G_TUNER, &t);

        if (t.signal >= SCAN_SIGNAL_THRESH) {
            pthread_mutex_lock(&r->mutex);
            if (r->found_count < 256)
                r->found_freqs[r->found_count++] = f;
            pthread_mutex_unlock(&r->mutex);
            msleep(200); /* brief dwell on found station */
        }
    }

    r->scanning = 0; /* signal natural completion */
    return NULL;
}

void radio_start_scan(Radio *r)
{
    if (r->scan_started) return;
    r->scanning = 1;
    r->scan_started = 1;
    pthread_create(&r->scan_thread, NULL, scan_fn, r);
}

void radio_stop_scan(Radio *r)
{
    if (!r->scan_started) return;
    r->scanning = 0;           /* request abort if still running */
    pthread_join(r->scan_thread, NULL);
    r->scan_started = 0;
}
