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

int config_load(Config *c)
{
    memset(c, 0, sizeof(*c));
    FILE *f = fopen(path(), "r");
    if (!f) return 0;

    char line[128];
    while (fgets(line, sizeof(line), f) && c->count < MAX_PRESETS) {
        if (line[0] == '#' || line[0] == '\n') continue;

        double mhz;
        int off = 0;
        if (sscanf(line, "%lf%n", &mhz, &off) != 1) continue;
        if (mhz < 87.5 || mhz > 108.0) continue;

        int i = c->count++;
        c->freqs[i] = (uint32_t)(mhz * 1000000.0 + 0.5);

        /* optional name: rest of line after frequency, trimmed */
        char *p = line + off;
        while (*p == ' ' || *p == '\t') p++;
        /* strip trailing whitespace / newline */
        char *end = p + strlen(p);
        while (end > p && (*(end-1) == '\n' || *(end-1) == '\r' ||
                           *(end-1) == ' '  || *(end-1) == '\t'))
            *(--end) = '\0';
        strncpy(c->names[i], p, NAME_MAX_LEN);
        c->names[i][NAME_MAX_LEN] = '\0';
    }
    fclose(f);
    return 1;
}

int config_save(const Config *c)
{
    FILE *f = fopen(path(), "w");
    if (!f) return 0;
    fprintf(f, "# ncradio stations\n");
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
