#include "radio.h"
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

static uint32_t hz_to_v4l2(const Radio *r, uint32_t hz)
{
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

static void drain_rds(int fd, RdsDecoder *d)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN)) return;

    struct v4l2_rds_data buf[64];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n <= 0) return;

    int nb = (int)(n / (ssize_t)sizeof(struct v4l2_rds_data));
    for (int i = 0; i < nb; i++)
        rds_feed(d, buf[i].lsb, buf[i].msb, buf[i].block);
}

/* Poll and feed RDS for up to `ms` ms or until PS name is ready.
   Checks r->scanning each loop iteration to support scan abort. */
static void collect_rds(Radio *r, RdsDecoder *d, int ms)
{
    int ticks = ms / 50;
    for (int t = 0; t < ticks && r->scanning && !d->ps_ready; t++) {
        struct pollfd pfd = { .fd = r->fd, .events = POLLIN };
        int ret = poll(&pfd, 1, 50);
        if (ret > 0 && (pfd.revents & POLLIN)) {
            struct v4l2_rds_data buf[32];
            ssize_t n = read(r->fd, buf, sizeof(buf));
            if (n > 0) {
                int nb = (int)(n / (ssize_t)sizeof(struct v4l2_rds_data));
                for (int i = 0; i < nb; i++)
                    rds_feed(d, buf[i].lsb, buf[i].msb, buf[i].block);
            }
        }
    }
}

int radio_open(Radio *r, const char *device)
{
    memset(r, 0, sizeof(*r));
    pthread_mutex_init(&r->mutex, NULL);
    rds_init(&r->rds);

    r->fd = open(device, O_RDWR);
    if (r->fd < 0) return -1;

    r->freq_min_hz = FREQ_MIN_HZ;
    r->freq_max_hz = FREQ_MAX_HZ;

    struct v4l2_tuner tuner = { .index = 0 };
    if (ioctl(r->fd, VIDIOC_G_TUNER, &tuner) == 0) {
        r->cap_low     = !!(tuner.capability & V4L2_TUNER_CAP_LOW);
        r->rds_capable = !!(tuner.capability & V4L2_TUNER_CAP_RDS);
        if (tuner.rangelow && tuner.rangehigh) {
            r->freq_min_hz = v4l2_to_hz(r, tuner.rangelow);
            r->freq_max_hz = v4l2_to_hz(r, tuner.rangehigh);
        }
    }

    if (r->rds_capable) {
        struct v4l2_control ctrl = { .id = V4L2_CID_RDS_RECEPTION, .value = 1 };
        ioctl(r->fd, VIDIOC_S_CTRL, &ctrl);
    }

    memset(&r->volume_ctrl, 0, sizeof(r->volume_ctrl));
    r->volume_ctrl.id = V4L2_CID_AUDIO_VOLUME;
    if (ioctl(r->fd, VIDIOC_QUERYCTRL, &r->volume_ctrl) != 0)
        memset(&r->volume_ctrl, 0, sizeof(r->volume_ctrl));

    r->freq_hz = radio_get_freq(r);
    if (r->freq_hz < r->freq_min_hz || r->freq_hz > r->freq_max_hz)
        r->freq_hz = r->freq_min_hz + (r->freq_max_hz - r->freq_min_hz) / 2;

    r->volume = 80;
    radio_set_volume(r, r->volume);
    radio_mute(r, 0);
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
    if (hz < r->freq_min_hz) hz = r->freq_min_hz;
    if (hz > r->freq_max_hz) hz = r->freq_max_hz;
    struct v4l2_frequency vf = {
        .tuner = 0, .type = V4L2_TUNER_RADIO,
        .frequency = hz_to_v4l2(r, hz)
    };
    if (ioctl(r->fd, VIDIOC_S_FREQUENCY, &vf) < 0) return -1;
    r->freq_hz = hz;
    rds_init(&r->rds);
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
    r->stereo = !!(t.rxsubchans & V4L2_TUNER_SUB_STEREO);
    return (int)((t.signal * 100ULL) / 65535);
}

int radio_set_volume(Radio *r, int vol)
{
    if (vol < 0)   vol = 0;
    if (vol > 100) vol = 100;
    r->volume = vol;
    if (r->volume_ctrl.maximum <= r->volume_ctrl.minimum)
        return 0;
    int range = r->volume_ctrl.maximum - r->volume_ctrl.minimum;
    struct v4l2_control c = {
        .id    = V4L2_CID_AUDIO_VOLUME,
        .value = (int)(vol / 100.0 * range + r->volume_ctrl.minimum)
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
    rds_init(&r->rds);
    return 0;
}

void radio_read_rds(Radio *r)
{
    if (!r->rds_capable) return;
    drain_rds(r->fd, &r->rds);
}

static void *seek_fn(void *arg)
{
    Radio *r = arg;
    int      fwd       = r->seek_fwd;
    uint32_t step      = r->seek_step_hz  > 0 ? r->seek_step_hz  : FREQ_STEP_HZ;
    int      threshold = r->seek_threshold > 0 ? r->seek_threshold : SCAN_SIGNAL_THRESH;

    r->seek_result_hz = 0;

    /* Maximum steps for a full sweep of the FM band */
    int max_steps = (int)((r->freq_max_hz - r->freq_min_hz) / step) + 2;
    uint32_t f = r->freq_hz;

    for (int s = 0; s < max_steps && r->seeking; s++) {
        /* Advance first so we don't re-check the current frequency */
        if (fwd)
            f = (f + step > r->freq_max_hz) ? r->freq_min_hz : f + step;
        else
            f = (f < r->freq_min_hz + step) ? r->freq_max_hz : f - step;

        struct v4l2_frequency vf = {
            .tuner = 0, .type = V4L2_TUNER_RADIO,
            .frequency = hz_to_v4l2(r, f)
        };
        ioctl(r->fd, VIDIOC_S_FREQUENCY, &vf);
        msleep(80);

        if (!r->seeking) break;

        struct v4l2_tuner t = { .index = 0 };
        ioctl(r->fd, VIDIOC_G_TUNER, &t);

        if ((int)t.signal >= threshold) {
            r->seek_result_hz = f;
            break;
        }
    }

    r->seeking = 0;
    return NULL;
}

void radio_start_seek(Radio *r, int fwd, uint32_t step_hz, int threshold)
{
    if (r->seek_started) return;
    r->seek_fwd       = fwd;
    r->seek_step_hz   = step_hz;
    r->seek_threshold = threshold;
    r->seek_result_hz = 0;
    r->seeking        = 1;
    r->seek_started   = 1;
    pthread_create(&r->seek_thread, NULL, seek_fn, r);
}

void radio_stop_seek(Radio *r)
{
    if (!r->seek_started) return;
    r->seeking = 0;
    pthread_join(r->seek_thread, NULL);
    r->seek_started = 0;
}

static void *scan_fn(void *arg)
{
    Radio *r = arg;

    /* Resolve effective scan parameters (fallback to compiled defaults) */
    uint32_t step      = r->scan_step_hz  > 0 ? r->scan_step_hz  : FREQ_STEP_HZ;
    int      threshold = r->scan_threshold > 0 ? r->scan_threshold : SCAN_SIGNAL_THRESH;
    int      use_rds   = r->scan_rds_names;

    pthread_mutex_lock(&r->mutex);
    r->found_count = 0;
    pthread_mutex_unlock(&r->mutex);

    for (uint32_t f = r->freq_min_hz; f <= r->freq_max_hz && r->scanning; f += step) {
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

        if ((int)t.signal < threshold) continue;

        /* Add station to found list immediately so the UI can show it */
        int idx = -1;
        pthread_mutex_lock(&r->mutex);
        if (r->found_count < MAX_PRESETS) {
            idx = r->found_count++;
            r->found_freqs[idx] = f;
            r->found_names[idx][0] = '\0';
        }
        pthread_mutex_unlock(&r->mutex);

        if (idx < 0) continue;

        if (r->rds_capable && use_rds) {
            RdsDecoder local;
            rds_init(&local);
            collect_rds(r, &local, 1500);
            if (local.ps_ready) {
                pthread_mutex_lock(&r->mutex);
                strncpy(r->found_names[idx], local.ps, NAME_MAX_LEN);
                r->found_names[idx][NAME_MAX_LEN] = '\0';
                pthread_mutex_unlock(&r->mutex);
            }
        } else {
            msleep(200);
        }
    }

    r->scanning = 0;
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
    r->scanning = 0;
    pthread_join(r->scan_thread, NULL);
    r->scan_started = 0;
}
