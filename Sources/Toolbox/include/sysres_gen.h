/* The standard System resources from Generators/sysres.py. */
#ifndef SYSRES_GEN_H
#define SYSRES_GEN_H

#include <stdint.h>

typedef struct {
    uint32_t type;
    int16_t id;
    const uint8_t *data;
    uint32_t len;
} sysres_entry;

extern const sysres_entry sysres[];
extern const unsigned sysres_count;

#endif
