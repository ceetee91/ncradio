#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static const char *path(void)
{
    static char buf[512];
    if (!buf[0]) {
        const char *home = getenv("HOME");
        snprintf(buf, sizeof(buf), "%s%s", home ? home : "/tmp", CONFIG_FILE);
    }
    return buf;
}

static void settings_defaults(Config *c)
{
    if (c->scan_step_hz < 50000 || c->scan_step_hz > 1000000)
        c->scan_step_hz = DEFAULT_SCAN_STEP_HZ;
    if (c->signal_threshold_pct < 5 || c->signal_threshold_pct > 95)
        c->signal_threshold_pct = DEFAULT_SIGNAL_THRESH_PCT;
    c->rds_names       = c->rds_names       ? 1 : 0;
    c->audio_enabled   = c->audio_enabled   ? 1 : 0;
    c->audio_mute_scan = c->audio_mute_scan ? 1 : 0;
    c->audio_mute_seek = c->audio_mute_seek ? 1 : 0;
#ifdef HAVE_PIPEWIRE
    {
        static const int valid[] = {128, 256, 512, 1024, 2048};
        int ok = 0;
        for (int i = 0; i < 5; i++)
            if (c->audio_buffer_frames == valid[i]) { ok = 1; break; }
        if (!ok) c->audio_buffer_frames = DEFAULT_AUDIO_BUFFER_FRAMES;
    }
#else
    if (c->audio_buffer_frames < 256 || c->audio_buffer_frames > 65536)
        c->audio_buffer_frames = DEFAULT_AUDIO_BUFFER_FRAMES;
#endif
}

int config_load(Config *c)
{
    memset(c, 0, sizeof(*c));
    c->scan_step_hz         = DEFAULT_SCAN_STEP_HZ;
    c->signal_threshold_pct = DEFAULT_SIGNAL_THRESH_PCT;
    c->rds_names            = DEFAULT_RDS_NAMES;
    c->audio_enabled        = DEFAULT_AUDIO_ENABLED;
    c->audio_buffer_frames  = DEFAULT_AUDIO_BUFFER_FRAMES;
    c->audio_mute_scan      = DEFAULT_AUDIO_MUTE_SCAN;
    c->audio_mute_seek      = DEFAULT_AUDIO_MUTE_SEEK;
    c->record_bitrate       = DEFAULT_RECORD_BITRATE;
    c->record_stereo        = DEFAULT_RECORD_STEREO;
    c->record_samplerate    = DEFAULT_RECORD_SAMPLERATE;

    FILE *f = fopen(path(), "r");
    if (!f) return 0;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;

        /* Settings line: key=value (first char not a digit) */
        if (strchr(line, '=') && !isdigit((unsigned char)line[0])) {
            char key[64]   = {0};
            char sval[192] = {0};
            if (sscanf(line, "%63[^=]=%191[^\n]", key, sval) < 1) continue;
            int ival = atoi(sval);

            if      (strcmp(key, "scan_step")        == 0) c->scan_step_hz = (uint32_t)ival;
            else if (strcmp(key, "signal_threshold")  == 0) c->signal_threshold_pct = ival;
            else if (strcmp(key, "rds_names")         == 0) c->rds_names = ival ? 1 : 0;
            else if (strcmp(key, "volume")            == 0) c->volume = ival;
            else if (strcmp(key, "last_freq")         == 0) c->last_freq_hz = (uint32_t)ival;
            else if (strcmp(key, "audio_enabled")     == 0) c->audio_enabled = ival ? 1 : 0;
            else if (strcmp(key, "audio_mute_scan")    == 0) c->audio_mute_scan = ival ? 1 : 0;
            else if (strcmp(key, "audio_mute_seek")    == 0) c->audio_mute_seek = ival ? 1 : 0;
            else if (strcmp(key, "audio_buffer_frames")== 0) c->audio_buffer_frames = ival;
            else if (strcmp(key, "record_bitrate")     == 0) c->record_bitrate = ival;
            else if (strcmp(key, "record_stereo")      == 0) c->record_stereo = ival ? 1 : 0;
            else if (strcmp(key, "record_samplerate")  == 0) c->record_samplerate = ival;
            else if (strcmp(key, "audio_device")       == 0) {
                strncpy(c->audio_device, sval, sizeof(c->audio_device) - 1);
                c->audio_device[sizeof(c->audio_device) - 1] = '\0';
            } else if (strcmp(key, "audio_play_device") == 0) {
                strncpy(c->audio_play_device, sval, sizeof(c->audio_play_device) - 1);
                c->audio_play_device[sizeof(c->audio_play_device) - 1] = '\0';
            } else if (strcmp(key, "eq_enabled") == 0) {
                c->eq_enabled = ival ? 1 : 0;
            } else if (strcmp(key, "eq_active_preset") == 0) {
                size_t plen = strlen(sval);
                if (plen >= EQ_CUSTOM_NAMELEN) plen = EQ_CUSTOM_NAMELEN - 1;
                memcpy(c->eq_active_preset, sval, plen);
                c->eq_active_preset[plen] = '\0';
            } else if (strcmp(key, "eq_gains") == 0) {
                char *tok = sval;
                for (int i = 0; i < EQ_BANDS && tok; i++) {
                    c->eq_gains[i] = (float)atof(tok);
                    tok = strchr(tok, ',');
                    if (tok) tok++;
                }
            } else if (strncmp(key, "eq_preset_", 10) == 0) {
                if (c->eq_custom_count < EQ_CUSTOM_MAX) {
                    int p = c->eq_custom_count++;
                    size_t nlen = strlen(key + 10);
                    if (nlen >= EQ_CUSTOM_NAMELEN) nlen = EQ_CUSTOM_NAMELEN - 1;
                    memcpy(c->eq_custom[p].name, key + 10, nlen);
                    c->eq_custom[p].name[nlen] = '\0';
                    char *tok = sval;
                    for (int i = 0; i < EQ_BANDS && tok; i++) {
                        c->eq_custom[p].gains[i] = (float)atof(tok);
                        tok = strchr(tok, ',');
                        if (tok) tok++;
                    }
                }
            }
            continue;
        }

        /* Station line: float [optional name] */
        if (c->count >= MAX_PRESETS) continue;
        double mhz;
        int off = 0;
        if (sscanf(line, "%lf%n", &mhz, &off) != 1) continue;
        if (mhz < 87.5 || mhz > 108.0) continue;

        int i = c->count++;
        c->freqs[i] = (uint32_t)(mhz * 1000000.0 + 0.5);

        char *p = line + off;
        while (*p == ' ' || *p == '\t') p++;
        char *end = p + strlen(p);
        while (end > p && (*(end-1) == '\n' || *(end-1) == '\r' ||
                           *(end-1) == ' '  || *(end-1) == '\t'))
            *(--end) = '\0';
        strncpy(c->names[i], p, NAME_MAX_LEN);
        c->names[i][NAME_MAX_LEN] = '\0';
    }
    fclose(f);
    settings_defaults(c);
    return 1;
}

int config_save(const Config *c)
{
    FILE *f = fopen(path(), "w");
    if (!f) return 0;

    fprintf(f, "# ncradio configuration\n");
    fprintf(f, "scan_step=%u\n",        c->scan_step_hz);
    fprintf(f, "signal_threshold=%d\n", c->signal_threshold_pct);
    fprintf(f, "rds_names=%d\n",        c->rds_names);
    fprintf(f, "volume=%d\n",           c->volume);
    if (c->last_freq_hz)
        fprintf(f, "last_freq=%u\n",    c->last_freq_hz);
    fprintf(f, "audio_enabled=%d\n",         c->audio_enabled);
    fprintf(f, "audio_buffer_frames=%d\n",   c->audio_buffer_frames);
    fprintf(f, "audio_mute_scan=%d\n",       c->audio_mute_scan);
    fprintf(f, "audio_mute_seek=%d\n",       c->audio_mute_seek);
    fprintf(f, "record_bitrate=%d\n",        c->record_bitrate);
    fprintf(f, "record_stereo=%d\n",         c->record_stereo);
    fprintf(f, "record_samplerate=%d\n",     c->record_samplerate);
    if (c->audio_device[0])
        fprintf(f, "audio_device=%s\n",      c->audio_device);
    fprintf(f, "audio_play_device=%s\n",     c->audio_play_device);
    fprintf(f, "eq_enabled=%d\n", c->eq_enabled);
    fprintf(f, "eq_active_preset=%s\n", c->eq_active_preset);
    fprintf(f, "eq_gains=");
    for (int i = 0; i < EQ_BANDS; i++)
        fprintf(f, i ? ",%.1f" : "%.1f", (double)c->eq_gains[i]);
    fputc('\n', f);
    for (int p = 0; p < c->eq_custom_count; p++) {
        fprintf(f, "eq_preset_%s=", c->eq_custom[p].name);
        for (int i = 0; i < EQ_BANDS; i++)
            fprintf(f, i ? ",%.1f" : "%.1f", (double)c->eq_custom[p].gains[i]);
        fputc('\n', f);
    }
    fprintf(f, "# stations\n");
    for (int i = 0; i < c->count; i++) {
        if (c->names[i][0])
            fprintf(f, "%.2f %s\n", c->freqs[i] / 1000000.0, c->names[i]);
        else
            fprintf(f, "%.2f\n", c->freqs[i] / 1000000.0);
    }
    fclose(f);
    return 1;
}

void config_add(Config *c, uint32_t hz, const char *name)
{
    if (c->count >= MAX_PRESETS || config_find(c, hz) >= 0) return;
    int i;
    for (i = c->count; i > 0 && c->freqs[i-1] > hz; i--) {
        c->freqs[i] = c->freqs[i-1];
        memcpy(c->names[i], c->names[i-1], NAME_MAX_LEN + 1);
    }
    c->freqs[i] = hz;
    strncpy(c->names[i], name ? name : "", NAME_MAX_LEN);
    c->names[i][NAME_MAX_LEN] = '\0';
    c->count++;
}

void config_del(Config *c, int idx)
{
    if (idx < 0 || idx >= c->count) return;
    for (int i = idx; i < c->count - 1; i++) {
        c->freqs[i] = c->freqs[i+1];
        memcpy(c->names[i], c->names[i+1], NAME_MAX_LEN + 1);
    }
    c->count--;
}

int config_find(const Config *c, uint32_t hz)
{
    for (int i = 0; i < c->count; i++)
        if (c->freqs[i] == hz) return i;
    return -1;
}

int config_eq_preset_add(Config *c, const char *name, const float gains[EQ_BANDS])
{
    if (c->eq_custom_count >= EQ_CUSTOM_MAX || !name || !name[0]) return -1;
    int idx = c->eq_custom_count++;
    strncpy(c->eq_custom[idx].name, name, EQ_CUSTOM_NAMELEN - 1);
    c->eq_custom[idx].name[EQ_CUSTOM_NAMELEN - 1] = '\0';
    for (int i = 0; i < EQ_BANDS; i++)
        c->eq_custom[idx].gains[i] = gains[i];
    return idx;
}

void config_eq_preset_del(Config *c, int idx)
{
    if (idx < 0 || idx >= c->eq_custom_count) return;
    for (int i = idx; i < c->eq_custom_count - 1; i++)
        c->eq_custom[i] = c->eq_custom[i + 1];
    c->eq_custom_count--;
}
