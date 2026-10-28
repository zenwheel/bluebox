/* QuickTime's movie importers and exporters of our own (qt_import.c, qt_export.c). */
#ifndef QT_IMPORT_H
#define QT_IMPORT_H

#include <stddef.h>
#include <stdint.h>

#include "mem.h"

/* The 'eat ' and 'spit' components registered (each run). */
void qt_import_register(void);
void qt_export_register(void);
/* Bytes of any kind we import (a sound, picture, MIDI file...) as new
 * tracks of a movie at a time; the first track made. */
int16_t qt_import_bytes(const uint8_t *bytes, size_t len, uint32_t type, gaddr movie, int32_t at, gaddr *used);

#endif
