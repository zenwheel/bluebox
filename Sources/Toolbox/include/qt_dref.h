/* Where a QuickTime data reference's data is, read and written (qt_dref.c). */
#ifndef QT_DREF_H
#define QT_DREF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mem.h"

enum { QSRC_NONE, QSRC_FILE, QSRC_HANDLE, QSRC_PTR, QSRC_BYTES };

typedef struct {
    int kind;
    char path[1024];      /* QSRC_FILE: the host file (its data fork) */
    int16_t refnum;       /* a File Manager refnum open on it for writing, or 0 */
    bool own_refnum;      /* ours to close */
    gaddr handle;         /* QSRC_HANDLE: the data's handle */
    gaddr ptr;            /* QSRC_PTR: the data, ptr_len bytes */
    uint32_t ptr_len;
    uint8_t *bytes;       /* QSRC_BYTES: a resource's data, copied */
    uint32_t nbytes;
} qt_src;

/* A data reference (its type and bytes, as a 'dref' entry or a program's
 * data reference handle holds them) as a source; self_path for one that
 * names the movie's own file (flags & 1, or an alias with nothing in it).
 * Writing opens a file for writing (through the File Manager). */
bool qt_src_open(uint32_t type, uint32_t flags, const uint8_t *data, uint32_t len, const char *self_path, bool writing,
                 qt_src *out);
void qt_src_close(qt_src *s);
bool qt_src_read(const qt_src *s, uint64_t off, void *buf, uint32_t n);
bool qt_src_read_guest(const qt_src *s, uint64_t off, gaddr buf, uint32_t n);
int64_t qt_src_size(const qt_src *s);
/* Bytes written at off (past the end grows it); false if it can't be. */
bool qt_src_write(qt_src *s, uint64_t off, const void *buf, uint32_t n);
bool qt_src_set_size(qt_src *s, uint64_t n);
bool qt_src_flush(qt_src *s);

/* A data reference handle's bytes (malloc'd), and a new handle of bytes. */
uint8_t *qt_dref_bytes(gaddr dataRef, uint32_t *len);
gaddr qt_dref_handle(const uint8_t *data, uint32_t len);
/* A data reference naming a host file: an alias handle. */
gaddr qt_dref_for_path(const char *path);
/* Its file's host path, if it names one. */
bool qt_dref_path(uint32_t type, const uint8_t *data, uint32_t len, const char *self_path, char *out, size_t outsize);

#endif
