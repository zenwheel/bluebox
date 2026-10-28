/*
 * QuickDraw 3D's storage objects (QD3DStorage.h): where a file object
 * reads and writes.  Memory storage (a buffer of our own that grows, or
 * the program's, which doesn't), handle storage (a Memory Manager handle,
 * resized as it is written), Macintosh storage (a file the program has
 * open, by its reference number) and FSSpec storage (a file opened while
 * a file object has it open, or for the one call).  Windows and UNIX
 * path storage are not on the Macintosh.
 */
#include <stdlib.h>

#include "filemgr_internal.h"
#include "q3_view.h"
#include "tb_internal.h"

#define MEMORY Q3T('m', 'e', 'm', 's')
#define HANDLE Q3T('h', 'n', 'd', 'l')
#define MAC Q3T('m', 'a', 'c', 'n')
#define FSSPEC Q3T('m', 'a', 'c', 'p')

enum { kQ3ErrorStorageInUse = -28447, kQ3ErrorMacintoshError = -28485, fsFromStart = 1, fsRdPerm = 1, fsRdWrPerm = 3 };

typedef struct {
    gaddr buffer;     /* memory: the bytes (guest memory) */
    uint32_t valid, size;
    bool own;         /* ours to grow and free */
    gaddr handle;     /* handle storage */
    int16_t refnum;   /* Macintosh storage, or an FSSpec's while open */
    uint8_t spec[70]; /* FSSpec storage */
    int opens;        /* FSSpec: how many have it open */
    bool writable;
} q3storage;

static q3storage *storage_of(gaddr g, uint32_t type)
{
    q3obj *o = q3_of(g, type);
    return o ? o->data : NULL;
}

static uint32_t leaf_of(gaddr g)
{
    q3obj *o = q3_get(g);
    return o ? o->type : 0;
}

static void st_dispose(q3obj *o)
{
    q3storage *s = o->data;
    if (!s)
        return;
    if (s->own && s->buffer)
        tb_DisposePtr(s->buffer);
    if (o->type == FSSPEC && s->opens > 0)
        fm_close_fork(s->refnum);
    free(s);
}

static bool st_duplicate(q3obj *to, const q3obj *from)
{
    const q3storage *f = from->data;
    q3storage *s = calloc(1, sizeof *s);
    if (!s)
        return false;
    *s = *f;
    s->opens = 0;
    if (from->type == MEMORY && f->buffer) { /* a copy of our own */
        s->own = true;
        s->size = f->valid ? f->valid : 4;
        s->buffer = tb_NewPtr((int32_t)s->size, false, false);
        if (!s->buffer) {
            free(s);
            return false;
        }
        gmemcpy(s->buffer, f->buffer, f->valid);
    }
    to->data = s;
    return true;
}

void q3_storage_classes(void)
{
    q3_class_ops(MEMORY, st_dispose, st_duplicate, NULL, false);
    q3_class_ops(HANDLE, st_dispose, st_duplicate, NULL, false);
    q3_class_ops(MAC, st_dispose, st_duplicate, NULL, false);
    q3_class_ops(FSSPEC, st_dispose, st_duplicate, NULL, false);
}

/* Open an FSSpec storage's file (counted); false if it can't be. */
static bool spec_open(q3storage *s)
{
    if (s->opens++ > 0)
        return true;
    gaddr spec = tb_NewPtr(72, false, true), ref = spec ? spec + 70 : 0;
    if (!spec) {
        s->opens--;
        return false;
    }
    gput(spec, s->spec, 70);
    int16_t e = tb_FSpOpenDF(spec, fsRdWrPerm, ref);
    s->writable = e == 0;
    if (e)
        e = tb_FSpOpenDF(spec, fsRdPerm, ref);
    s->refnum = (int16_t)rd16(ref);
    tb_DisposePtr(spec);
    if (e) {
        s->opens--;
        q3_error(kQ3ErrorMacintoshError);
        return false;
    }
    return true;
}

static void spec_close(q3storage *s)
{
    if (s->opens > 0 && --s->opens == 0)
        fm_close_fork(s->refnum);
}

bool q3_storage_open(gaddr storage)
{
    q3obj *o = q3_get(storage);
    if (!o || !q3_type_is(o->type, Q3_STORAGE))
        return false;
    return o->type != FSSPEC || spec_open(o->data);
}

void q3_storage_close(gaddr storage)
{
    q3obj *o = q3_get(storage);
    if (o && o->type == FSSPEC)
        spec_close(o->data);
}

static uint32_t file_size(int16_t refnum)
{
    uint32_t pos = 0, len = 0;
    char path[16];
    bool rsrc, write;
    if (!fm_fork_state(refnum, path, sizeof path, &rsrc, &write, &pos, &len))
        return 0;
    return len;
}

uint32_t q3_storage_size(gaddr storage)
{
    q3obj *o = q3_get(storage);
    q3storage *s = o ? o->data : NULL;
    if (!s)
        return 0;
    switch (o->type) {
    case HANDLE: return s->handle && rd32(s->handle) ? (s->valid <= (uint32_t)tb_GetHandleSize(s->handle) ? s->valid : (uint32_t)tb_GetHandleSize(s->handle)) : 0;
    case MAC: return file_size(s->refnum);
    case FSSPEC: {
        if (!spec_open(s))
            return 0;
        uint32_t n = file_size(s->refnum);
        spec_close(s);
        return n;
    }
    default: return s->valid;
    }
}

/* Through a file: count bytes at off, to or from host memory, in chunks
 * through a guest buffer. */
static uint32_t file_io(int16_t refnum, bool write, uint32_t off, uint32_t count, uint8_t *host)
{
    gaddr buf = tb_NewPtr(8192, false, false);
    uint32_t done = 0;
    while (buf && done < count) {
        uint32_t n = count - done > 8192 ? 8192 : count - done, actual = 0;
        if (write)
            gput(buf, host + done, n);
        int16_t e = fm_fork_io(refnum, write, fsFromStart, (int64_t)(off + done), n, buf, &actual);
        if (!write && actual)
            gget(host + done, buf, actual);
        done += actual;
        if (e || actual < n)
            break;
    }
    if (buf)
        tb_DisposePtr(buf);
    return done;
}

uint32_t q3_storage_read(gaddr storage, uint32_t off, uint32_t count, uint8_t *out)
{
    q3obj *o = q3_get(storage);
    q3storage *s = o ? o->data : NULL;
    if (!s)
        return 0;
    uint32_t size = q3_storage_size(storage);
    if (off >= size)
        return 0;
    if (count > size - off)
        count = size - off;
    switch (o->type) {
    case HANDLE: gget(out, rd32(s->handle) + off, count); return count;
    case MAC: return file_io(s->refnum, false, off, count, out);
    case FSSPEC: {
        if (!spec_open(s))
            return 0;
        uint32_t n = file_io(s->refnum, false, off, count, out);
        spec_close(s);
        return n;
    }
    default: gget(out, s->buffer + off, count); return count;
    }
}

/* Make room for a memory or handle storage's bytes up to end. */
static bool grow(q3obj *o, q3storage *s, uint32_t end)
{
    if (o->type == HANDLE) {
        if (!s->handle) {
            s->handle = tb_NewHandle((int32_t)end, false, false);
            return s->handle != 0;
        }
        if ((uint32_t)tb_GetHandleSize(s->handle) < end)
            tb_SetHandleSize(s->handle, (int32_t)end);
        return (uint32_t)tb_GetHandleSize(s->handle) >= end;
    }
    if (end <= s->size)
        return true;
    if (!s->own && s->buffer)
        return false; /* the program's buffer doesn't grow */
    uint32_t cap = s->size ? s->size : 1024;
    while (cap < end)
        cap *= 2;
    gaddr n = tb_NewPtr((int32_t)cap, false, false);
    if (!n)
        return false;
    if (s->buffer) {
        gmemcpy(n, s->buffer, s->valid);
        tb_DisposePtr(s->buffer);
    }
    s->buffer = n;
    s->size = cap;
    s->own = true;
    return true;
}

uint32_t q3_storage_write(gaddr storage, uint32_t off, uint32_t count, const uint8_t *in)
{
    q3obj *o = q3_get(storage);
    q3storage *s = o ? o->data : NULL;
    if (!s)
        return 0;
    switch (o->type) {
    case MAC: return file_io(s->refnum, true, off, count, (uint8_t *)in);
    case FSSPEC: {
        if (!spec_open(s))
            return 0;
        uint32_t n = file_io(s->refnum, true, off, count, (uint8_t *)in);
        spec_close(s);
        return n;
    }
    default:
        if (!grow(o, s, off + count))
            return 0;
        gput((o->type == HANDLE ? rd32(s->handle) : s->buffer) + off, in, count);
        if (off + count > s->valid)
            s->valid = off + count;
        return count;
    }
}

/* The storage's end set at size (what was written replaces all). */
void q3_storage_truncate(gaddr storage, uint32_t size)
{
    q3obj *o = q3_get(storage);
    q3storage *s = o ? o->data : NULL;
    if (!s)
        return;
    if (o->type == MAC || o->type == FSSPEC) {
        if (o->type == FSSPEC && !spec_open(s))
            return;
        fm_fork_set_eof(s->refnum, fsFromStart, size);
        if (o->type == FSSPEC)
            spec_close(s);
    } else {
        s->valid = size;
    }
}

uint8_t *q3_storage_bytes(gaddr storage, uint32_t *size)
{
    *size = 0;
    if (!q3_get(storage))
        return NULL;
    uint32_t n = q3_storage_size(storage);
    uint8_t *b = malloc(n ? n : 1);
    if (!b)
        return NULL;
    *size = q3_storage_read(storage, 0, n, b);
    return b;
}

/* ---------------------------------------------------------------------- */
/* The calls */

static gaddr new_storage(uint32_t type, q3storage *s)
{
    q3obj *o = q3_new(type, s);
    if (!o) {
        if (s->own && s->buffer)
            tb_DisposePtr(s->buffer);
        free(s);
        return 0;
    }
    return o->g;
}

/* Our own copy of the program's bytes. */
static bool copy_in(q3storage *s, gaddr buffer, uint32_t valid)
{
    if (s->own && s->buffer)
        tb_DisposePtr(s->buffer);
    s->own = true;
    s->size = valid ? valid : 1024;
    s->buffer = tb_NewPtr((int32_t)s->size, false, true);
    s->valid = buffer ? valid : 0;
    if (s->buffer && buffer && valid)
        gmemcpy(s->buffer, buffer, valid);
    return s->buffer != 0;
}

/* A memory storage of its own with these bytes (the 3DMF reader's images). */
gaddr q3_memory_storage_new(const uint8_t *b, uint32_t n)
{
    q3storage *s = calloc(1, sizeof *s);
    if (!s || !copy_in(s, 0, n)) {
        free(s);
        return 0;
    }
    if (n)
        gput(s->buffer, b, n);
    s->valid = n;
    return new_storage(MEMORY, s);
}

static void Q3Storage_GetType(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), Q3_STORAGE);
    Q3RETURN(o ? q3_type_below(o->type, Q3_STORAGE) : 0);
}

static void Q3Storage_GetSize(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), Q3_STORAGE);
    if (o && Q3R(1))
        wr32(Q3R(1), q3_storage_size(o->g));
    Q3RETURN(o && Q3R(1) ? kQ3Success : kQ3Failure);
}

static void Q3Storage_GetData(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), Q3_STORAGE);
    uint32_t off = Q3R(1), n = Q3R(2);
    gaddr data = Q3R(3), out = Q3R(4);
    uint8_t *tmp = o && data ? malloc(n ? n : 1) : NULL;
    uint32_t got = tmp ? q3_storage_read(o->g, off, n, tmp) : 0;
    if (tmp && got)
        gput(data, tmp, got);
    free(tmp);
    if (out)
        wr32(out, got);
    Q3RETURN(o && data ? kQ3Success : kQ3Failure);
}

static void Q3Storage_SetData(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), Q3_STORAGE);
    uint32_t off = Q3R(1), n = Q3R(2);
    gaddr data = Q3R(3), out = Q3R(4);
    uint8_t *tmp = o && data ? malloc(n ? n : 1) : NULL;
    if (tmp && n)
        gget(tmp, data, n);
    uint32_t put = tmp ? q3_storage_write(o->g, off, n, tmp) : 0;
    free(tmp);
    if (out)
        wr32(out, put);
    Q3RETURN(o && data && put == n ? kQ3Success : kQ3Failure);
}

static void Q3MemoryStorage_GetType(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), MEMORY);
    Q3RETURN(o ? q3_type_below(o->type, MEMORY) : 0);
}

static void Q3MemoryStorage_New(ppc_cpu *c)
{
    q3storage *s = calloc(1, sizeof *s);
    if (!s || !copy_in(s, Q3R(0), Q3R(1))) {
        free(s);
        Q3RETURN(0);
        return;
    }
    Q3RETURN(new_storage(MEMORY, s));
}

static void Q3MemoryStorage_Set(ppc_cpu *c)
{
    q3storage *s = storage_of(Q3R(0), MEMORY);
    bool ok = s && leaf_of(Q3R(0)) == MEMORY && copy_in(s, Q3R(1), Q3R(2));
    Q3RETURN(ok ? kQ3Success : kQ3Failure);
}

/* The program's buffer, used in place. */
static bool use_buffer(q3storage *s, gaddr buffer, uint32_t valid, uint32_t size)
{
    if (s->own && s->buffer)
        tb_DisposePtr(s->buffer);
    s->own = false;
    s->buffer = buffer;
    s->valid = valid;
    s->size = size < valid ? valid : size;
    return true;
}

static void Q3MemoryStorage_NewBuffer(ppc_cpu *c)
{
    q3storage *s = calloc(1, sizeof *s);
    if (!s) {
        Q3RETURN(0);
        return;
    }
    s->buffer = 0;
    if (!Q3R(0)) {
        s->own = true;
        s->size = Q3R(2) ? Q3R(2) : 1024;
        s->buffer = tb_NewPtr((int32_t)s->size, false, true);
    } else {
        use_buffer(s, Q3R(0), Q3R(1), Q3R(2));
    }
    Q3RETURN(s->buffer ? new_storage(MEMORY, s) : (free(s), 0));
}

static void Q3MemoryStorage_SetBuffer(ppc_cpu *c)
{
    q3storage *s = storage_of(Q3R(0), MEMORY);
    if (!s || leaf_of(Q3R(0)) != MEMORY) {
        Q3RETURN(kQ3Failure);
        return;
    }
    if (!Q3R(1)) {
        if (s->own && s->buffer)
            tb_DisposePtr(s->buffer);
        s->own = true;
        s->size = Q3R(3) ? Q3R(3) : 1024;
        s->buffer = tb_NewPtr((int32_t)s->size, false, true);
        s->valid = 0;
    } else {
        use_buffer(s, Q3R(1), Q3R(2), Q3R(3));
    }
    Q3RETURN(s->buffer ? kQ3Success : kQ3Failure);
}

static void Q3MemoryStorage_GetBuffer(ppc_cpu *c)
{
    q3storage *s = storage_of(Q3R(0), MEMORY);
    if (s && leaf_of(Q3R(0)) == HANDLE) { /* a handle storage's bytes */
        if (Q3R(1))
            wr32(Q3R(1), s->handle ? rd32(s->handle) : 0);
        if (Q3R(2))
            wr32(Q3R(2), s->valid);
        if (Q3R(3))
            wr32(Q3R(3), s->handle ? (uint32_t)tb_GetHandleSize(s->handle) : 0);
        Q3RETURN(kQ3Success);
        return;
    }
    if (s) {
        if (Q3R(1))
            wr32(Q3R(1), s->buffer);
        if (Q3R(2))
            wr32(Q3R(2), s->valid);
        if (Q3R(3))
            wr32(Q3R(3), s->size);
    }
    Q3RETURN(s ? kQ3Success : kQ3Failure);
}

static void Q3HandleStorage_New(ppc_cpu *c)
{
    q3storage *s = calloc(1, sizeof *s);
    if (!s) {
        Q3RETURN(0);
        return;
    }
    s->handle = Q3R(0) ? Q3R(0) : tb_NewHandle(0, false, false);
    s->valid = Q3R(0) ? Q3R(1) : 0;
    Q3RETURN(s->handle ? new_storage(HANDLE, s) : (free(s), 0));
}

static void Q3HandleStorage_Set(ppc_cpu *c)
{
    q3storage *s = storage_of(Q3R(0), HANDLE);
    if (s) {
        s->handle = Q3R(1) ? Q3R(1) : tb_NewHandle(0, false, false);
        s->valid = Q3R(1) ? Q3R(2) : 0;
    }
    Q3RETURN(s ? kQ3Success : kQ3Failure);
}

static void Q3HandleStorage_Get(ppc_cpu *c)
{
    q3storage *s = storage_of(Q3R(0), HANDLE);
    if (s && Q3R(1))
        wr32(Q3R(1), s->handle);
    if (s && Q3R(2))
        wr32(Q3R(2), s->valid);
    Q3RETURN(s ? kQ3Success : kQ3Failure);
}

static void Q3MacintoshStorage_New(ppc_cpu *c)
{
    q3storage *s = calloc(1, sizeof *s);
    if (!s) {
        Q3RETURN(0);
        return;
    }
    s->refnum = (int16_t)Q3R(0);
    Q3RETURN(new_storage(MAC, s));
}

static void Q3MacintoshStorage_Set(ppc_cpu *c)
{
    q3storage *s = storage_of(Q3R(0), MAC);
    if (s && leaf_of(Q3R(0)) == MAC)
        s->refnum = (int16_t)Q3R(1);
    Q3RETURN(s && leaf_of(Q3R(0)) == MAC ? kQ3Success : kQ3Failure);
}

static void Q3MacintoshStorage_Get(ppc_cpu *c)
{
    q3storage *s = storage_of(Q3R(0), MAC);
    if (s && Q3R(1))
        wr16(Q3R(1), (uint16_t)s->refnum);
    Q3RETURN(s && Q3R(1) ? kQ3Success : kQ3Failure);
}

static void Q3MacintoshStorage_GetType(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), MAC);
    Q3RETURN(o ? q3_type_below(o->type, MAC) : 0);
}

static void Q3FSSpecStorage_New(ppc_cpu *c)
{
    q3storage *s = Q3R(0) ? calloc(1, sizeof *s) : NULL;
    if (!s) {
        Q3RETURN(0);
        return;
    }
    gget(s->spec, Q3R(0), 70);
    Q3RETURN(new_storage(FSSPEC, s));
}

static void Q3FSSpecStorage_Set(ppc_cpu *c)
{
    q3storage *s = storage_of(Q3R(0), FSSPEC);
    if (s && s->opens) {
        q3_error(kQ3ErrorStorageInUse);
        Q3RETURN(kQ3Failure);
        return;
    }
    if (s && Q3R(1))
        gget(s->spec, Q3R(1), 70);
    Q3RETURN(s && Q3R(1) ? kQ3Success : kQ3Failure);
}

static void Q3FSSpecStorage_Get(ppc_cpu *c)
{
    q3storage *s = storage_of(Q3R(0), FSSPEC);
    if (s && Q3R(1))
        gput(Q3R(1), s->spec, 70);
    Q3RETURN(s && Q3R(1) ? kQ3Success : kQ3Failure);
}

#define E(n) { #n, n }
const ppc_export q3_storage_exports[] = {
    E(Q3Storage_GetType), E(Q3Storage_GetSize), E(Q3Storage_GetData), E(Q3Storage_SetData),
    E(Q3MemoryStorage_GetType), E(Q3MemoryStorage_New), E(Q3MemoryStorage_Set), E(Q3MemoryStorage_NewBuffer),
    E(Q3MemoryStorage_SetBuffer), E(Q3MemoryStorage_GetBuffer), E(Q3HandleStorage_New), E(Q3HandleStorage_Set),
    E(Q3HandleStorage_Get), E(Q3MacintoshStorage_New), E(Q3MacintoshStorage_Set), E(Q3MacintoshStorage_Get),
    E(Q3MacintoshStorage_GetType), E(Q3FSSpecStorage_New), E(Q3FSSpecStorage_Set), E(Q3FSSpecStorage_Get),
};
const unsigned q3_storage_export_count = sizeof q3_storage_exports / sizeof q3_storage_exports[0];
