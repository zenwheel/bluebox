/*
 * MACE 3:1 and 6:1 decoding (LGPL 2.1 or later: see ../COPYING.LGPLv2.1).
 */
#ifndef MACE_H
#define MACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Decode a whole MACE stream (channels interleaved packet by packet, as in
 * a sound resource) into interleaved 16-bit samples; out holds 3 (MACE 3)
 * or 6 (MACE 6) frames per byte per channel.  Returns the frames written. */
size_t mace_decode(const uint8_t *buf, size_t size, int channels, bool mace3, int16_t *out);

#endif
