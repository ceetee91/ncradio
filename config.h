#pragma once
#include <stdint.h>

#define MAX_PRESETS   64
#define NAME_MAX_LEN  32
#define CONFIG_FILE   "/.ncradio.conf"

typedef struct {
    uint32_t freqs[MAX_PRESETS];
    char     names[MAX_PRESETS][NAME_MAX_LEN + 1];  /* optional display name */
    int count;
} Config;

int  config_load(Config *c);
int  config_save(const Config *c);
/* name may be "" — inserted in sorted order, no-op if freq already present */
void config_add(Config *c, uint32_t hz, const char *name);
void config_del(Config *c, int idx);
int  config_find(const Config *c, uint32_t hz);
