/*
 * The pointer map's tables (statemap_gen.c, made by scripts/state-map.py).
 */
#ifndef TOOLBOX_HIBERNATE_MAP_H
#define TOOLBOX_HIBERNATE_MAP_H

#include <stdint.h>

/* Fields of one kind at off + i*stride + j*stride2, i < count, j < count2.
 * kind: 'P' pointer, 'F' function pointer, 'U' a pointer in a union,
 * 'H' a library's handle, 'Y' a lock, 'D' a file descriptor, 'X' and
 * 'x' a value of the host's (8 and 4 bytes, bbstate.h), 'I' an integer
 * as wide as a pointer. */
typedef struct hb_run {
    uint32_t off;
    char kind;
    uint32_t count, stride, count2, stride2;
} hb_run;

typedef struct hb_layout {
    uint32_t first, n; /* runs */
} hb_layout;

/* A saved static: its file (as Target/path), its symbol (a static local
 * as function.name), size and layout. */
typedef struct hb_static {
    const char *file, *sym;
    uint32_t size, layout;
} hb_static;

/* A tagged type: the file the tag is written in (its base name), the
 * type as written, its size and layout. */
typedef struct hb_typemap {
    const char *file, *name;
    uint32_t size, layout;
} hb_typemap;

extern const hb_layout hb_layouts[];
extern const hb_run *const hb_runs;
extern const unsigned hb_nlayouts;
extern const hb_static hb_statics[];
extern const unsigned hb_nstatics;
extern const hb_typemap hb_types[];
extern const unsigned hb_ntypes;

#endif
