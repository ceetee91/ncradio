#pragma once
#include <stdint.h>

#define MAX_PRESETS  64
#define CONFIG_FILE  "/.ncradio.conf"

typedef struct {
    uint32_t freqs[MAX_PRESETS];  /* Hz */
    int count;
} Config;

int  config_load(Config *c);
int  config_save(const Config *c);
void config_add(Config *c, uint32_t hz);
void config_del(Config *c, int idx);
int  config_find(const Config *c, uint32_t hz);
