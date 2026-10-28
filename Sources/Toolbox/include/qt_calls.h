/* QuickTime's component calls (qt_calls_gen.c from Generators/defs/qt_calls.tsv)
 * and the QuickTime libraries' exports (qt_calls.c). */
#ifndef QT_CALLS_H
#define QT_CALLS_H

#include <stdint.h>

#include "ppc.h"

typedef struct {
    const char *name;
    uint16_t what;      /* the selector */
    const char *sizes;  /* the arguments after the instance: 4, 2, or 1 (a byte, passed as a word) */
    const char *family; /* MovieExport, DataH, Media, TextMedia, ... */
    void (*fn)(ppc_cpu *c);
} qt_call_info;

/* Sorted by name. */
extern const qt_call_info qt_calls[];
extern const unsigned qt_call_count;

/* A PowerPC program's call qt_calls[index] to the instance in r3. */
void qt_call_ppc(ppc_cpu *c, unsigned index);
/* The call of a family with a selector, or NULL. */
const qt_call_info *qt_call_find(const char *family, int16_t what);
/* The bytes the arguments take on the 68k stack. */
uint32_t qt_call_param_size(const char *sizes);

/* QuickTimeLib's, QuickTimeVRLib's and QTStreamLib's exports of ours. */
void (*qt_export_fn(const char *name))(ppc_cpu *);

#endif
