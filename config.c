#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    char line[64];
    while (fgets(line, sizeof(line), f) && c->count < MAX_PRESETS) {
        if (line[0] == '#' || line[0] == '\n') continue;
        double mhz;
        if (sscanf(line, "%lf", &mhz) == 1 && mhz >= 87.5 && mhz <= 108.0)
            c->freqs[c->count++] = (uint32_t)(mhz * 1000000.0 + 0.5);
    }
    fclose(f);
    return 1;
}

int config_save(const Config *c)
{
    FILE *f = fopen(path(), "w");
    if (!f) return 0;
    fprintf(f, "# ncradio stations\n");
    for (int i = 0; i < c->count; i++)
        fprintf(f, "%.2f\n", c->freqs[i] / 1000000.0);
    fclose(f);
    return 1;
}

void config_add(Config *c, uint32_t hz)
{
    if (c->count >= MAX_PRESETS || config_find(c, hz) >= 0) return;
    int i;
    for (i = c->count; i > 0 && c->freqs[i-1] > hz; i--)
        c->freqs[i] = c->freqs[i-1];
    c->freqs[i] = hz;
    c->count++;
}

void config_del(Config *c, int idx)
{
    if (idx < 0 || idx >= c->count) return;
    for (int i = idx; i < c->count - 1; i++)
        c->freqs[i] = c->freqs[i+1];
    c->count--;
}

int config_find(const Config *c, uint32_t hz)
{
    for (int i = 0; i < c->count; i++)
        if (c->freqs[i] == hz) return i;
    return -1;
}
