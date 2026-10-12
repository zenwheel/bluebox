/* Bitmap fonts of the compatibility System file (generated). */
#ifndef SYSFONTS_H
#define SYSFONTS_H

#include <stdint.h>

typedef struct {
    uint32_t type;
    int16_t id;
    const char *name;
    const uint8_t *data;
    uint32_t len;
} sysfont_res;

extern const sysfont_res sysfonts[];
extern const unsigned sysfont_count;

#endif
