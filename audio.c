#include "audio.h"
#include <alsa/asoundlib.h>
#include <alloca.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef HAVE_UDEV
#include <libudev.h>
#endif

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

        pthread_mutex_lock(&a->rec_lock);
        if (a->rec_fn)
            a->rec_fn(a->rec_ctx, buf, (int)n, a->channels);
        pthread_mutex_unlock(&a->rec_lock);
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
    pthread_mutex_init(&a->rec_lock, NULL);
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

const char *audio_alsa_version(void)
{
    return snd_asoundlib_version();
}

/* ── autodetect ──────────────────────────────────────────────────────── */

/* Join two path components into buf; returns 1 on success, 0 if it would overflow. */
static int path_join(char *buf, size_t size, const char *dir, const char *suffix)
{
    size_t d = strlen(dir), s = strlen(suffix);
    if (d + 1 + s + 1 > size) return 0;
    memcpy(buf, dir, d);
    buf[d] = '/';
    memcpy(buf + d + 1, suffix, s + 1);
    return 1;
}

/* Write the first ALSA card found under sound_dir into out. */
static int detect_card_in_sound_dir(const char *sound_dir, char *out, int out_size)
{
    DIR *d = opendir(sound_dir);
    if (!d) return 0;

    int found = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, "card", 4) != 0 || ent->d_name[4] < '0')
            continue;

        int card_num = atoi(ent->d_name + 4);
        char id_path[PATH_MAX];
        char id[64] = "";
        snprintf(id_path, sizeof(id_path), "%s/%s/id", sound_dir, ent->d_name);
        FILE *f = fopen(id_path, "r");
        if (f) {
            if (fgets(id, sizeof(id), f))
                id[strcspn(id, "\n")] = '\0';
            fclose(f);
        }

        if (id[0])
            snprintf(out, (size_t)out_size, "hw:CARD=%s,DEV=0", id);
        else
            snprintf(out, (size_t)out_size, "hw:%d,0", card_num);
        found = 1;
        break;
    }
    closedir(d);
    return found;
}

/* Check base/sound and base/<child>/sound for an ALSA card. */
static int detect_alsa_in(const char *base, char *out, int out_size)
{
    char sound_dir[PATH_MAX];
    struct stat st;

    if (path_join(sound_dir, sizeof(sound_dir), base, "sound") &&
        stat(sound_dir, &st) == 0 && S_ISDIR(st.st_mode) &&
        detect_card_in_sound_dir(sound_dir, out, out_size))
        return 1;

    DIR *d = opendir(base);
    if (!d) return 0;

    int found = 0;
    struct dirent *ent;
    char child[PATH_MAX];
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (!path_join(child, sizeof(child), base, ent->d_name)) continue;
        if (!path_join(sound_dir, sizeof(sound_dir), child, "sound")) continue;
        if (stat(sound_dir, &st) == 0 && S_ISDIR(st.st_mode) &&
            detect_card_in_sound_dir(sound_dir, out, out_size)) {
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

/* Resolve a sysfs symlink to its canonical path using chdir+getcwd.
   Saves and restores the process working directory. */
static int resolve_syslink(const char *link, char *out, int out_size)
{
    char saved[PATH_MAX];
    if (!getcwd(saved, sizeof(saved))) return 0;
    if (chdir(link) != 0) return 0;
    int ok = (getcwd(out, (size_t)out_size) != NULL);
    chdir(saved);
    return ok;
}

/* Sysfs-based detection: walk up the sysfs tree from the radio device. */
static int detect_sysfs(const char *radio_dev, char *out, int out_size)
{
    const char *devname = strrchr(radio_dev, '/');
    devname = devname ? devname + 1 : radio_dev;

    char syslink[PATH_MAX];
    snprintf(syslink, sizeof(syslink), "/sys/class/video4linux/%s", devname);

    char path[PATH_MAX];
    if (!resolve_syslink(syslink, path, sizeof(path))) return 0;

    /* Strip "<devname>" and "video4linux" to reach the kernel device dir. */
    char *p;
    p = strrchr(path, '/'); if (p) *p = '\0';
    p = strrchr(path, '/'); if (p) *p = '\0';

    char base[PATH_MAX];
    strncpy(base, path, sizeof(base) - 1);
    base[sizeof(base) - 1] = '\0';

    for (int level = 0; level < 3; level++) {
        if (detect_alsa_in(base, out, out_size)) return 1;
        p = strrchr(base, '/');
        if (!p) break;
        *p = '\0';
    }
    return 0;
}

#ifdef HAVE_UDEV
/* udev-based detection: match sound cards to the same USB device. */
static int detect_udev(const char *radio_dev, char *out, int out_size)
{
    struct udev *udev = udev_new();
    if (!udev) return 0;

    struct stat st;
    if (stat(radio_dev, &st) != 0) { udev_unref(udev); return 0; }

    struct udev_device *radio_udev =
        udev_device_new_from_devnum(udev, 'c', st.st_rdev);
    if (!radio_udev) { udev_unref(udev); return 0; }

    /* Walk to the USB device so sibling interfaces (audio + radio) are matched. */
    struct udev_device *usb = udev_device_get_parent_with_subsystem_devtype(
        radio_udev, "usb", "usb_device");
    if (!usb) {
        udev_device_unref(radio_udev);
        udev_unref(udev);
        return 0;
    }
    const char *usb_syspath = udev_device_get_syspath(usb);

    struct udev_enumerate *en = udev_enumerate_new(udev);
    udev_enumerate_add_match_subsystem(en, "sound");
    udev_enumerate_scan_devices(en);

    int found = 0;
    struct udev_list_entry *entry;
    udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(en)) {
        struct udev_device *snd = udev_device_new_from_syspath(
            udev, udev_list_entry_get_name(entry));
        if (!snd) continue;

        const char *sysname = udev_device_get_sysname(snd);
        if (strncmp(sysname, "card", 4) != 0 || sysname[4] < '0') {
            udev_device_unref(snd);
            continue;
        }

        struct udev_device *snd_usb = udev_device_get_parent_with_subsystem_devtype(
            snd, "usb", "usb_device");
        if (snd_usb && strcmp(usb_syspath, udev_device_get_syspath(snd_usb)) == 0) {
            const char *id = udev_device_get_sysattr_value(snd, "id");
            if (id && id[0])
                snprintf(out, (size_t)out_size, "hw:CARD=%s,DEV=0", id);
            else
                snprintf(out, (size_t)out_size, "hw:%d,0", atoi(sysname + 4));
            found = 1;
        }

        udev_device_unref(snd);
        if (found) break;
    }

    udev_enumerate_unref(en);
    udev_device_unref(radio_udev);
    udev_unref(udev);
    return found;
}
#endif /* HAVE_UDEV */

int audio_autodetect(const char *radio_dev, char *out, int out_size)
{
    if (out_size > 0) out[0] = '\0';
#ifdef HAVE_UDEV
    if (detect_udev(radio_dev, out, out_size)) return 1;
#endif
    return detect_sysfs(radio_dev, out, out_size);
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
