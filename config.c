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

/* Apply defaults and clamp settings to valid ranges. */
static void settings_defaults(Config *c)
{
    if (c->scan_step_hz < 25000 || c->scan_step_hz > 1000000)
        c->scan_step_hz = DEFAULT_SCAN_STEP_HZ;
    if (c->signal_threshold_pct < 5 || c->signal_threshold_pct > 95)
        c->signal_threshold_pct = DEFAULT_SIGNAL_THRESH_PCT;
    c->rds_names = c->rds_names ? 1 : 0;
}

int config_load(Config *c)
{
    memset(c, 0, sizeof(*c));
    /* Seed defaults so unset values are usable even if file is absent */
    c->scan_step_hz         = DEFAULT_SCAN_STEP_HZ;
    c->signal_threshold_pct = DEFAULT_SIGNAL_THRESH_PCT;
    c->rds_names            = DEFAULT_RDS_NAMES;

    FILE *f = fopen(path(), "r");
    if (!f) return 0;

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;

        /* Settings line: key=value (first char is not a digit) */
        char *eq = strchr(line, '=');
        if (eq && !isdigit((unsigned char)line[0])) {
            char key[40] = {0};
            int  val = 0;
            if (sscanf(line, "%39[^=]=%d", key, &val) == 2) {
                if      (strcmp(key, "scan_step")         == 0) c->scan_step_hz = (uint32_t)val;
                else if (strcmp(key, "signal_threshold")  == 0) c->signal_threshold_pct = val;
                else if (strcmp(key, "rds_names")         == 0) c->rds_names = val ? 1 : 0;
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
