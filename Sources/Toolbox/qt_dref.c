/*
 * QuickTime's data references: where a movie's samples (or a graphics
 * importer's image, or a data handler's data) are, and reading and writing
 * them.  An alias ('alis') names a file, or with nothing in it (or the
 * self-reference flag of a 'dref' entry) the movie's own; 'hndl' a handle
 * in memory, 'ptr ' a block of memory, 'rsrc' a resource (an alias, then
 * the resource's type and ID), 'url ' a URL (file URLs only: Blue Box
 * reaches no servers for movies).
 *
 * A file being written goes through the File Manager, which holds open
 * forks in memory: QuickTime's writes land in the same fork a program has
 * open (CreateMovieFile's data fork, say), and reads of a file open for
 * writing come from it rather than the disk.
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "filemgr_internal.h"
#include "forks.h"
#include "qt_dref.h"
#include "tb_internal.h"

enum { fsRdWrPerm = 3, fsFromStart = 1, opWrErr = -49 };

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t four(const char *s) { return (uint32_t)(uint8_t)s[0] << 24 | (uint32_t)(uint8_t)s[1] << 16 | (uint32_t)(uint8_t)s[2] << 8 | (uint8_t)s[3]; }

uint8_t *qt_dref_bytes(gaddr dataRef, uint32_t *len)
{
    *len = dataRef && rd32(dataRef) ? (uint32_t)tb_GetHandleSize(dataRef) : 0;
    uint8_t *b = malloc(*len ? *len : 1);
    if (*len)
        gget(b, rd32(dataRef), *len);
    return b;
}

gaddr qt_dref_handle(const uint8_t *data, uint32_t len)
{
    gaddr h = tb_NewHandle((int32_t)len, false, false);
    if (h && rd32(h) && len)
        gput(rd32(h), data, len);
    return h;
}

gaddr qt_dref_for_path(const char *path)
{
    gaddr out = tb_NewPtr(4, false, true);
    gaddr h = 0;
    if (out && alias_new_for_host(path, out) == noErr)
        h = rd32(out);
    if (out)
        tb_DisposePtr(out);
    return h;
}

bool alias_host_of_record(gaddr rec, uint32_t len, char *host, size_t hostsize); /* alias.c */

/* An alias record's host path (the record in host memory). */
static bool alias_path(const uint8_t *rec, uint32_t len, char *out, size_t outsize)
{
    if (len < 6)
        return false;
    gaddr g = tb_NewPtr((int32_t)len, false, false);
    if (!g)
        return false;
    gput(g, rec, len);
    bool ok = alias_host_of_record(g, len, out, outsize);
    tb_DisposePtr(g);
    return ok;
}

/* A file URL's host path: file:///Volume/folder/name, %-escaped. */
static bool file_url_path(const uint8_t *data, uint32_t len, char *out, size_t outsize)
{
    char url[2048];
    uint32_t n = len < sizeof url - 1 ? len : (uint32_t)sizeof url - 1;
    memcpy(url, data, n);
    url[n] = 0;
    if (strncmp(url, "file://", 7))
        return false;
    const char *p = url + 7;
    if (!strncmp(p, "localhost", 9))
        p += 9;
    uint8_t mac[1024];
    size_t m = 0;
    if (*p == '/')
        p++;
    for (; *p && m < sizeof mac; p++) {
        if (*p == '%' && p[1] && p[2]) {
            char hex[3] = { p[1], p[2], 0 };
            mac[m++] = (uint8_t)strtol(hex, NULL, 16);
            p += 2;
        } else {
            mac[m++] = *p == '/' ? ':' : (uint8_t)*p;
        }
    }
    return fm_host_of_mac_path(mac, m, out, outsize);
}

bool qt_dref_path(uint32_t type, const uint8_t *data, uint32_t len, const char *self_path, char *out, size_t outsize)
{
    if (type == four("alis") || type == four("rsrc")) {
        uint32_t alen = len >= 6 ? (uint32_t)(data[4] << 8 | data[5]) : 0;
        if (alen > len)
            alen = len;
        if (alen >= 6 && alias_path(data, alen, out, outsize))
            return true;
        if (!alen && self_path) {
            snprintf(out, outsize, "%s", self_path);
            return true;
        }
        return false;
    }
    if (type == four("url "))
        return file_url_path(data, len, out, outsize);
    return false;
}

bool qt_src_open(uint32_t type, uint32_t flags, const uint8_t *data, uint32_t len, const char *self_path, bool writing,
                 qt_src *out)
{
    memset(out, 0, sizeof *out);
    if (type == four("hndl")) {
        if (len < 4 || !be32(data))
            return false;
        out->kind = QSRC_HANDLE;
        out->handle = be32(data);
        return true;
    }
    if (type == four("ptr ")) {
        if (len < 8 || !be32(data))
            return false;
        out->kind = QSRC_PTR;
        out->ptr = be32(data);
        out->ptr_len = be32(data + 4);
        return true;
    }
    if (type == four("rsrc")) {
        char path[PATH_MAX];
        uint32_t alen = len >= 6 ? (uint32_t)(data[4] << 8 | data[5]) : 0;
        if (alen > len || alen + 6 > len || !qt_dref_path(type, data, len, self_path, path, sizeof path))
            return false;
        uint32_t rtype = be32(data + alen);
        int16_t rid = (int16_t)(data[alen + 4] << 8 | data[alen + 5]);
        mac_file mf;
        char err[256];
        if (!appledouble_read(path, &mf, err, sizeof err))
            return false;
        rsrc_index idx;
        bool found = false;
        if (mf.rsrc_len && rsrc_parse(mf.rsrc, mf.rsrc_len, &idx, err, sizeof err)) {
            for (size_t i = 0; i < idx.count && !found; i++)
                if (idx.entries[i].type == rtype && idx.entries[i].id == rid) {
                    const uint8_t *d = rsrc_data(mf.rsrc, &idx, &idx.entries[i]);
                    out->nbytes = idx.entries[i].data_len;
                    out->bytes = malloc(out->nbytes ? out->nbytes : 1);
                    memcpy(out->bytes, d, out->nbytes);
                    found = true;
                }
            rsrc_index_free(&idx);
        }
        mac_file_free(&mf);
        out->kind = found ? QSRC_BYTES : QSRC_NONE;
        return found;
    }
    char path[PATH_MAX];
    bool have = (flags & 1) && self_path ? (snprintf(path, sizeof path, "%s", self_path), true)
                                         : qt_dref_path(type, data, len, self_path, path, sizeof path);
    if (!have)
        return false;
    out->kind = QSRC_FILE;
    snprintf(out->path, sizeof out->path, "%s", path);
    out->refnum = fm_write_refnum(path, false);
    if (writing && !out->refnum) {
        int16_t rn = 0;
        int16_t err = fm_open_fork(path, false, fsRdWrPerm, &rn);
        if (err == noErr)
            out->refnum = rn, out->own_refnum = true;
        else if (err == opWrErr)
            out->refnum = rn;
        else
            return false;
    }
    return true;
}

void qt_src_close(qt_src *s)
{
    if (s->kind == QSRC_FILE && s->own_refnum && s->refnum)
        fm_close_fork(s->refnum);
    free(s->bytes);
    memset(s, 0, sizeof *s);
}

/* A guest scratch buffer for the File Manager's transfers. */
static gaddr scratch(uint32_t n)
{
    static gaddr buf;
    static uint32_t size;
    if (n > size || !buf) {
        if (buf)
            tb_DisposePtr(buf);
        size = n > 65536 ? n : 65536;
        buf = tb_NewPtr((int32_t)size, true, false);
        if (!buf)
            size = 0;
    }
    return buf;
}

/* Reads or writes through an open fork, its mark put back after. */
static bool fork_io(const qt_src *s, bool write, uint64_t off, void *host, gaddr guest, uint32_t n)
{
    char path[8];
    bool rsrc, w;
    uint32_t pos, len;
    if (!fm_fork_state(s->refnum, NULL, 0, &rsrc, &w, &pos, &len))
        return false;
    (void)path;
    bool ok = true;
    for (uint32_t done = 0; done < n && ok;) {
        uint32_t k = n - done > 65536 ? 65536 : n - done;
        gaddr buf = guest ? guest + done : scratch(k);
        if (!buf)
            return false;
        if (write && !guest)
            gput(buf, (const uint8_t *)host + done, k);
        uint32_t actual = 0;
        int16_t err = fm_fork_io(s->refnum, write, fsFromStart, (int64_t)(off + done), k, buf, &actual);
        ok = !err && actual == k;
        if (ok && !write && !guest)
            gget((uint8_t *)host + done, buf, k);
        done += k;
    }
    fm_fork_seek(s->refnum, fsFromStart, pos);
    return ok;
}

bool qt_src_read(const qt_src *s, uint64_t off, void *buf, uint32_t n)
{
    switch (s->kind) {
    case QSRC_FILE: {
        if (s->refnum)
            return fork_io(s, false, off, buf, 0, n);
        FILE *f = fopen(s->path, "rb");
        bool ok = f && fseeko(f, (off_t)off, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
        if (f)
            fclose(f);
        return ok;
    }
    case QSRC_HANDLE: {
        gaddr p = rd32(s->handle);
        uint64_t size = p ? (uint64_t)tb_GetHandleSize(s->handle) : 0;
        if (!p || off + n > size)
            return false;
        gget(buf, p + (uint32_t)off, n);
        return true;
    }
    case QSRC_PTR:
        if (off + n > s->ptr_len)
            return false;
        gget(buf, s->ptr + (uint32_t)off, n);
        return true;
    case QSRC_BYTES:
        if (off + n > s->nbytes)
            return false;
        memcpy(buf, s->bytes + off, n);
        return true;
    }
    return false;
}

bool qt_src_read_guest(const qt_src *s, uint64_t off, gaddr buf, uint32_t n)
{
    if (s->kind == QSRC_FILE && s->refnum)
        return fork_io(s, false, off, NULL, buf, n);
    uint8_t *tmp = malloc(n ? n : 1);
    bool ok = qt_src_read(s, off, tmp, n);
    if (ok && n)
        gput(buf, tmp, n);
    free(tmp);
    return ok;
}

int64_t qt_src_size(const qt_src *s)
{
    switch (s->kind) {
    case QSRC_FILE: {
        if (s->refnum) {
            bool rsrc, w;
            uint32_t pos, len;
            return fm_fork_state(s->refnum, NULL, 0, &rsrc, &w, &pos, &len) ? (int64_t)len : -1;
        }
        struct stat st;
        return stat(s->path, &st) == 0 ? (int64_t)st.st_size : -1;
    }
    case QSRC_HANDLE:
        return rd32(s->handle) ? tb_GetHandleSize(s->handle) : 0;
    case QSRC_PTR:
        return s->ptr_len;
    case QSRC_BYTES:
        return s->nbytes;
    }
    return -1;
}

bool qt_src_write(qt_src *s, uint64_t off, const void *buf, uint32_t n)
{
    switch (s->kind) {
    case QSRC_FILE:
        return s->refnum && fork_io(s, true, off, (void *)buf, 0, n);
    case QSRC_HANDLE: {
        int64_t size = tb_GetHandleSize(s->handle);
        if ((int64_t)(off + n) > size) {
            tb_SetHandleSize(s->handle, (int32_t)(off + n));
            if (tb_GetHandleSize(s->handle) < (int32_t)(off + n))
                return false;
        }
        if (n)
            gput(rd32(s->handle) + (uint32_t)off, buf, n);
        return true;
    }
    case QSRC_PTR:
        if (off + n > s->ptr_len)
            return false;
        gput(s->ptr + (uint32_t)off, buf, n);
        return true;
    }
    return false;
}

bool qt_src_set_size(qt_src *s, uint64_t n)
{
    if (s->kind == QSRC_HANDLE) {
        tb_SetHandleSize(s->handle, (int32_t)n);
        return (uint64_t)tb_GetHandleSize(s->handle) == n;
    }
    if (s->kind == QSRC_FILE && s->refnum) {
        gaddr pb = tb_NewPtr(IOParam_SIZEOF, false, true);
        wr16(pb + IOParam_ioRefNum_off, (uint16_t)s->refnum);
        wr32(pb + IOParam_ioMisc_off, (uint32_t)n);
        int16_t err = tb_PBSetEOF(pb, false);
        tb_DisposePtr(pb);
        return !err;
    }
    return false;
}

bool qt_src_flush(qt_src *s)
{
    if (s->kind == QSRC_FILE && s->refnum)
        return fm_flush_fork(s->refnum) == noErr;
    return true;
}
