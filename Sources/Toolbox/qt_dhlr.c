/*
 * QuickTime's data handlers ('dhlr' components): the alias data handler
 * for files, and those for handles, pointers, resources, URLs and no data
 * at all.  A data handler is given a data reference and reads and writes
 * what it names (qt_dref.c does the work); reads and writes asked for
 * asynchronously happen at once, their completion routines called before
 * the call returns, as a local disk's nearly did.
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "filemgr_internal.h"
#include "forks.h"
#include "qt_atoms.h"
#include "qt_comp.h"
#include "qt_dref.h"
#include "qt_errors.h"
#include "qt_movie_internal.h"
#include "tb_internal.h"

enum {
    kDataHCanRead = 1, kDataHCanWrite = 1 << 3, kDataHCanStreamingWrite = 1 << 6,
    kDataHInfoFlagNeverStreams = 1, fnfErrQ = -43, dupFNErr = -48, fsRdWrPerm = 3, fsFromStart = 1,
};

#define MAX_EXT 8

typedef struct {
    uint32_t type;         /* the handler's subtype: the kind of reference */
    uint8_t *ref;
    uint32_t reflen;
    bool have_ref;
    qt_src src;
    bool reading, writing;
    uint32_t file_type;    /* SetMacOSFileType's, for CreateFile */
    gaddr timebase;
    int32_t cache_limit;
    uint8_t *anchor;
    uint32_t anchor_len;
    struct {
        uint32_t type;
        uint8_t *data;
        uint32_t len;
    } ext[MAX_EXT];
    int next;
} dhlr;

static void close_source(dhlr *d)
{
    if (d->reading || d->writing)
        qt_src_close(&d->src);
    d->reading = d->writing = false;
}

/* The source, open for reading at least (writing if asked). */
static int16_t open_source(dhlr *d, bool write)
{
    if (!d->have_ref)
        return dataNoDataRef;
    if (d->writing || (d->reading && !write))
        return noErr;
    close_source(d);
    if (d->type == fourcc("null"))
        return write ? dataNotOpenForWrite : noErr;
    if (!qt_src_open(d->type, 0, d->ref, d->reflen, NULL, write, &d->src)) {
        char path[PATH_MAX];
        if (qt_dref_path(d->type, d->ref, d->reflen, NULL, path, sizeof path) && !fm_host_exists(path))
            return fnfErrQ;
        return write ? dataNotOpenForWrite : couldNotResolveDataRef;
    }
    if (write && d->src.kind != QSRC_FILE && d->src.kind != QSRC_HANDLE && d->src.kind != QSRC_PTR) {
        qt_src_close(&d->src);
        return dataNotOpenForWrite;
    }
    d->reading = true;
    d->writing = write;
    return noErr;
}

static bool read_at(dhlr *d, uint64_t off, gaddr dst, uint32_t n)
{
    if (d->type == fourcc("null")) {
        for (uint32_t i = 0; i < n; i++)
            wr8(dst + i, 0);
        return true;
    }
    return qt_src_read_guest(&d->src, off, dst, n);
}

static bool write_at(dhlr *d, uint64_t off, gaddr src, uint32_t n)
{
    uint8_t *b = malloc(n ? n : 1);
    if (n)
        gget(b, src, n);
    bool ok = qt_src_write(&d->src, off, b, n);
    free(b);
    return ok;
}

static int64_t size_of(dhlr *d) { return d->type == fourcc("null") ? 0 : qt_src_size(&d->src); }

/* DataHCompletionProc: pascal void (Ptr request, long refcon, OSErr err). */
static void complete(gaddr proc, gaddr request, uint32_t refcon, int16_t err)
{
    if (!proc)
        return;
    cpu68k *c = g_cpu;
    cpu_push32(c, request);
    cpu_push32(c, refcon);
    cpu_push16(c, (uint16_t)err);
    guest_call(proc);
}

static uint64_t wide_at(gaddr p) { return p ? (uint64_t)rd32(p) << 32 | rd32(p + 4) : 0; }
static void put_wide(gaddr p, uint64_t v)
{
    if (p) {
        wr32(p, (uint32_t)(v >> 32));
        wr32(p + 4, (uint32_t)v);
    }
}

static void put_pstr(gaddr p, const char *s)
{
    if (!p)
        return;
    size_t n = strlen(s) > 255 ? 255 : strlen(s);
    wr8(p, (uint8_t)n);
    gput(p + 1, s, (uint32_t)n);
}

/* The file the reference names, for the calls about files. */
static bool file_of(dhlr *d, char *path, size_t n)
{
    return d->have_ref && qt_dref_path(d->type, d->ref, d->reflen, NULL, path, n);
}

static gaddr spec_of(const char *path)
{
    gaddr spec = tb_NewPtr(FSSpec_SIZEOF, false, true);
    fm_spec_for_path(path, spec);
    return spec;
}

static const char *mime_of(const char *path)
{
    const char *dot = path ? strrchr(path, '.') : NULL;
    static const struct {
        const char *ext, *mime;
    } map[] = { { ".mov", "video/quicktime" }, { ".qt", "video/quicktime" }, { ".mp4", "video/mp4" },
                { ".mpg", "video/mpeg" }, { ".mpeg", "video/mpeg" }, { ".avi", "video/avi" },
                { ".aif", "audio/aiff" }, { ".aiff", "audio/aiff" }, { ".wav", "audio/wav" },
                { ".mid", "audio/midi" }, { ".mp3", "audio/mpeg" }, { ".jpg", "image/jpeg" },
                { ".jpeg", "image/jpeg" }, { ".gif", "image/gif" }, { ".png", "image/png" },
                { ".tif", "image/tiff" }, { ".bmp", "image/bmp" }, { ".pct", "image/pict" },
                { ".pict", "image/pict" }, { ".txt", "text/plain" } };
    for (size_t i = 0; dot && i < sizeof map / sizeof map[0]; i++)
        if (!strcasecmp(dot, map[i].ext))
            return map[i].mime;
    return "";
}

/* The movie in the data: its 'moov' resource for a file with one, else
 * the first 'moov' atom among the data's own. */
static int32_t get_movie(dhlr *d, gaddr movie_out, gaddr id_out, int16_t flags)
{
    if (movie_out)
        wr32(movie_out, 0);
    if (!d->have_ref)
        return dataNoDataRef;
    gaddr ref = qt_dref_handle(d->ref, d->reflen);
    int16_t err = tb_NewMovieFromDataRef(movie_out, flags, id_out, (int32_t)ref, (int32_t)d->type);
    tb_DisposeHandle(ref);
    return err;
}

/* The last 'moov' atom among the data's top-level ones (its offset), or -1. */
static int64_t last_moov(dhlr *d, uint32_t *len)
{
    int64_t size = size_of(d), at = 0, found = -1;
    uint8_t h[16];
    while (at + 8 <= size) {
        if (!qt_src_read(&d->src, (uint64_t)at, h, 8))
            break;
        uint64_t n = (uint32_t)(h[0] << 24 | h[1] << 16 | h[2] << 8 | h[3]);
        if (n == 1 && qt_src_read(&d->src, (uint64_t)at + 8, h + 8, 8))
            n = (uint64_t)(h[8] << 24 | h[9] << 16 | h[10] << 8 | h[11]) << 32 |
                (uint32_t)(h[12] << 24 | h[13] << 16 | h[14] << 8 | h[15]);
        if (n < 8)
            break;
        if (!memcmp(h + 4, "moov", 4)) {
            found = at;
            *len = (uint32_t)n;
        }
        at += (int64_t)n;
    }
    return found;
}

/* A movie's description appended to the data (an old one there made a
 * 'free' atom). */
static int32_t add_movie(dhlr *d, int32_t theMovie, bool replace)
{
    int16_t err = open_source(d, true);
    if (err)
        return err;
    uint32_t old_len = 0;
    int64_t old = replace ? last_moov(d, &old_len) : -1;
    gaddr h = tb_NewHandle(0, false, false);
    err = tb_PutMovieIntoHandle(theMovie, (int32_t)h);
    if (!err) {
        int64_t at = size_of(d);
        uint32_t n = (uint32_t)tb_GetHandleSize(h);
        if (!write_at(d, (uint64_t)at, rd32(h), n))
            err = dataNotOpenForWrite;
        if (!err && old >= 0) {
            gaddr f = tb_NewPtr(4, false, false);
            wr32(f, fourcc("free"));
            write_at(d, (uint64_t)old + 4, f, 4);
            tb_DisposePtr(f);
        }
        qt_src_flush(&d->src);
    }
    tb_DisposeHandle(h);
    return err;
}

static int32_t dh_call(qtc_call *k)
{
    dhlr *d = k->self;
    uint32_t *a = k->a;
    switch (k->what) {
    case kQTCOpen:
        d->type = k->cls->subtype;
        d->cache_limit = 0;
        return noErr;
    case kQTCClose:
        close_source(d);
        free(d->ref);
        free(d->anchor);
        for (int i = 0; i < MAX_EXT; i++)
            free(d->ext[i].data);
        return noErr;
    case kQTCVersion:
        return 0x00040000;
    }
    if (!k->call)
        return qtcBadSelector;
    switch (k->what) {
    case 0x000A: /* SetDataRef(dataRef) */
    case 0x0029: /* SetDataRefWithAnchor(anchor, type, dataRef) */
        close_source(d);
        free(d->ref);
        d->ref = qt_dref_bytes(k->what == 0x000A ? a[0] : a[2], &d->reflen);
        d->have_ref = true;
        if (k->what == 0x0029) {
            free(d->anchor);
            d->anchor = qt_dref_bytes(a[0], &d->anchor_len);
        }
        return noErr;
    case 0x000B: /* GetDataRef(*dataRef) */
    case 0x002A: /* GetDataRefWithAnchor(anchor, type, *dataRef) */ {
        gaddr out = k->what == 0x000B ? a[0] : a[2];
        if (!d->have_ref)
            return dataNoDataRef;
        if (out)
            wr32(out, qt_dref_handle(d->ref, d->reflen));
        return noErr;
    }
    case 0x003B: { /* GetDataRefAsType(type, *dataRef): a file as a file URL too */
        if (!d->have_ref)
            return dataNoDataRef;
        char path[PATH_MAX];
        if (a[0] == fourcc("url ") && d->type != fourcc("url ") && file_of(d, path, sizeof path)) {
            char url[PATH_MAX + 16];
            snprintf(url, sizeof url, "file://%s", path);
            if (a[1])
                wr32(a[1], qt_dref_handle((const uint8_t *)url, (uint32_t)strlen(url) + 1));
            return noErr;
        }
        if (a[0] != d->type)
            return paramErr;
        if (a[1])
            wr32(a[1], qt_dref_handle(d->ref, d->reflen));
        return noErr;
    }
    case 0x000C: { /* CompareDataRef(dataRef, *equal) */
        uint32_t n;
        uint8_t *b = qt_dref_bytes(a[0], &n);
        char p1[PATH_MAX], p2[PATH_MAX];
        bool eq = d->have_ref && n == d->reflen && !memcmp(b, d->ref, n);
        if (!eq && d->have_ref && qt_dref_path(d->type, b, n, NULL, p1, sizeof p1) && file_of(d, p2, sizeof p2))
            eq = !strcmp(p1, p2);
        free(b);
        if (a[1])
            wr8(a[1], eq);
        return noErr;
    }
    case 0x0013: { /* CanUseDataRef(dataRef, *useFlags) */
        uint32_t n;
        uint8_t *b = qt_dref_bytes(a[0], &n);
        qt_src s;
        int32_t flags = 0;
        if (d->type == fourcc("null")) {
            flags = kDataHCanRead;
        } else if (qt_src_open(d->type, 0, b, n, NULL, false, &s)) {
            flags = kDataHCanRead;
            if (s.kind == QSRC_FILE || s.kind == QSRC_HANDLE || s.kind == QSRC_PTR)
                flags |= kDataHCanWrite | kDataHCanStreamingWrite;
            qt_src_close(&s);
        } else if (d->type == fourcc("alis")) {
            char path[PATH_MAX]; /* a file still to be made can be written */
            if (qt_dref_path(d->type, b, n, NULL, path, sizeof path))
                flags = kDataHCanWrite;
        }
        free(b);
        if (a[1])
            wr32(a[1], (uint32_t)flags);
        return noErr;
    }
    case 0x0011: /* ResolveDataRef(theDataRef, *wasChanged, ui) */ {
        if (a[1])
            wr8(a[1], 0);
        uint32_t n;
        uint8_t *b = qt_dref_bytes(a[0], &n);
        qt_src s;
        bool ok = d->type == fourcc("null") || qt_src_open(d->type, 0, b, n, NULL, false, &s);
        if (ok && d->type != fourcc("null"))
            qt_src_close(&s);
        free(b);
        return ok ? noErr : couldNotResolveDataRef;
    }
    case 0x0008: /* OpenForRead */
        return open_source(d, false);
    case 0x0005: /* OpenForWrite */
        return open_source(d, true);
    case 0x0009: /* CloseForRead */
    case 0x0006: /* CloseForWrite */
        if (d->writing)
            qt_src_flush(&d->src);
        close_source(d);
        return noErr;
    case 0x0002: { /* GetData(h, hOffset, offset, size) */
        int16_t err = open_source(d, false);
        if (err)
            return err;
        gaddr h = a[0];
        if (!h || !rd32(h) || (int64_t)a[1] + (int32_t)a[3] > tb_GetHandleSize(h))
            return paramErr;
        return read_at(d, a[2], rd32(h) + a[1], a[3]) ? noErr : endOfDataReached;
    }
    case 0x0003: { /* PutData(h, hOffset, *offset, size): appended */
        int16_t err = open_source(d, true);
        if (err)
            return err;
        gaddr h = a[0];
        if (!h || !rd32(h))
            return paramErr;
        int64_t at = size_of(d);
        if (!write_at(d, (uint64_t)at, rd32(h) + a[1], a[3]))
            return dataNotOpenForWrite;
        if (a[2])
            wr32(a[2], (uint32_t)at);
        return noErr;
    }
    case 0x0034: { /* Append64(data, *fileOffset, size) */
        int16_t err = open_source(d, true);
        if (err)
            return err;
        int64_t at = size_of(d);
        if (!write_at(d, (uint64_t)at, a[0], a[2]))
            return dataNotOpenForWrite;
        put_wide(a[1], (uint64_t)at);
        return noErr;
    }
    case 0x0004: /* FlushData */
        if (d->writing)
            qt_src_flush(&d->src);
        return noErr;
    case 0x000E: /* ScheduleData(ptr, offset, size, refcon, scheduleRec, completion) */
    case 0x002E: { /* ScheduleData64(ptr, wide *offset, ...) */
        if (!a[0]) /* a nil place: the requests are cancelled (there are none) */
            return noErr;
        int16_t err = open_source(d, false);
        uint64_t off = k->what == 0x002E ? wide_at(a[1]) : a[1];
        if (!err && !read_at(d, off, a[0], a[2]))
            err = endOfDataReached;
        complete(a[5], a[0], a[3], err);
        return err;
    }
    case 0x0035: { /* ReadAsync(ptr, size, wide *offset, completion, refcon) */
        int16_t err = open_source(d, false);
        if (!err && !read_at(d, wide_at(a[2]), a[0], a[1]))
            err = endOfDataReached;
        complete(a[3], a[0], a[4], err);
        return err;
    }
    case 0x0036: /* PollRead(ptr, *sizeSoFar): (reads are done at once) */
        return noErr;
    case 0x000F: /* FinishData(ptr, cancel) */
    case 0x0010: /* FlushCache */
    case 0x000D: /* Task */
        return noErr;
    case 0x0015: /* Write(ptr, offset, size, completion, refcon) */
    case 0x002F: { /* Write64(ptr, wide *offset, size, completion, refcon) */
        int16_t err = open_source(d, true);
        uint64_t off = k->what == 0x002F ? wide_at(a[1]) : a[1];
        if (!err && !write_at(d, off, a[0], a[2]))
            err = dataNotOpenForWrite;
        complete(a[3], a[0], a[4], err);
        return err;
    }
    case 0x0012: /* GetFileSize(*size) */
    case 0x0026: /* GetAvailableFileSize(*size) */ {
        int16_t err = open_source(d, false);
        if (err)
            return err;
        int64_t n = size_of(d);
        if (a[0])
            wr32(a[0], (uint32_t)(n < 0 ? 0 : n));
        return n < 0 ? couldNotResolveDataRef : noErr;
    }
    case 0x0030: { /* GetFileSize64(wide *) */
        int16_t err = open_source(d, false);
        if (err)
            return err;
        int64_t n = size_of(d);
        put_wide(a[0], (uint64_t)(n < 0 ? 0 : n));
        return n < 0 ? couldNotResolveDataRef : noErr;
    }
    case 0x003A: { /* GetFileSizeAsync(wide *, completion, refcon) */
        int16_t err = open_source(d, false);
        int64_t n = err ? 0 : size_of(d);
        put_wide(a[0], (uint64_t)(n < 0 ? 0 : n));
        complete(a[1], a[0], a[2], err);
        return err;
    }
    case 0x0017: /* SetFileSize(size) */
    case 0x0032: { /* SetFileSize64(wide *) */
        int16_t err = open_source(d, true);
        if (err)
            return err;
        return qt_src_set_size(&d->src, k->what == 0x0017 ? a[0] : wide_at(a[0])) ? noErr : dataNotOpenForWrite;
    }
    case 0x0016: /* Preextend(maxToAdd, *added): room is never short */
        if (a[1])
            wr32(a[1], a[0]);
        return noErr;
    case 0x0031: /* Preextend64(wide *max, wide *added) */
        put_wide(a[1], wide_at(a[0]));
        return noErr;
    case 0x0018: /* GetFreeSpace(*) */
        if (a[0])
            wr32(a[0], 0x7FFFFFFF);
        return noErr;
    case 0x0033: /* GetFreeSpace64(wide *) */
        put_wide(a[0], 0x7FFFFFFFFFFFull);
        return noErr;
    case 0x0019: /* CreateFile(creator, deleteExisting) */
    case 0x0041: { /* CreateFileWithFlags(creator, deleteExisting, flags) */
        char path[PATH_MAX];
        if (!file_of(d, path, sizeof path)) {
            if (d->type == fourcc("hndl") || d->type == fourcc("ptr ")) /* (memory needs no making) */
                return noErr;
            return couldNotResolveDataRef;
        }
        close_source(d);
        gaddr spec = spec_of(path);
        if (a[1] & 0xFF)
            tb_FSpDelete(spec);
        int16_t err = tb_FSpCreate(spec, (int32_t)a[0], (int32_t)(d->file_type ? d->file_type : fourcc("MooV")), 0);
        tb_DisposePtr(spec);
        return err == dupFNErr && !(a[1] & 0xFF) ? noErr : err;
    }
    case 0x001A: /* GetPreferredBlockSize */
        if (a[0])
            wr32(a[0], 4096);
        return noErr;
    case 0x001B: /* GetDeviceIndex */
        if (a[0])
            wr32(a[0], 0);
        return noErr;
    case 0x001C: /* IsStreamingDataHandler */
        if (a[0])
            wr8(a[0], 0);
        return noErr;
    case 0x001D: { /* GetDataInBuffer(start, *size): all of it is here */
        int64_t n = open_source(d, false) ? 0 : size_of(d);
        if (a[1])
            wr32(a[1], (uint32_t)(n > (int64_t)a[0] ? n - (int64_t)a[0] : 0));
        return noErr;
    }
    case 0x0037: /* GetDataAvailability(offset, len, *missingOffset, *missingLen) */
        if (a[2])
            wr32(a[2], 0);
        if (a[3])
            wr32(a[3], 0);
        return noErr;
    case 0x001E: /* GetScheduleAheadTime(*ms) */
        if (a[0])
            wr32(a[0], 0);
        return noErr;
    case 0x001F: /* SetCacheSizeLimit */
        d->cache_limit = (int32_t)a[0];
        return noErr;
    case 0x0020: /* GetCacheSizeLimit */
        if (a[0])
            wr32(a[0], (uint32_t)d->cache_limit);
        return noErr;
    case 0x0021: /* GetMovie(*movie, *id) */
        return get_movie(d, a[0], a[1], 0);
    case 0x003E: /* GetMovieWithFlags(*movie, *id, flags) */
        return get_movie(d, a[0], a[1], (int16_t)a[2]);
    case 0x0022: /* AddMovie(movie, *id) */ {
        int32_t err = add_movie(d, (int32_t)a[0], false);
        if (!err && a[1])
            wr16(a[1], 0);
        return err;
    }
    case 0x0023: /* UpdateMovie(movie, id) */
        return add_movie(d, (int32_t)a[0], true);
    case 0x0024: /* DoesBuffer(*reads, *writes) */
        if (a[0])
            wr8(a[0], 0);
        if (a[1])
            wr8(a[1], 0);
        return noErr;
    case 0x0025: { /* GetFileName(str) */
        char path[PATH_MAX];
        if (!file_of(d, path, sizeof path)) {
            put_pstr(a[0], "");
            return noErr;
        }
        const char *slash = strrchr(path, '/');
        uint8_t mac[256];
        size_t n = utf8_to_macroman(slash ? slash + 1 : path, mac, sizeof mac);
        if (a[0]) {
            wr8(a[0], (uint8_t)(n > 255 ? 255 : n));
            gput(a[0] + 1, mac, (uint32_t)(n > 255 ? 255 : n));
        }
        return noErr;
    }
    case 0x0027: { /* GetMacOSFileType(*type) */
        char path[PATH_MAX];
        uint32_t t = d->file_type;
        if (!t && file_of(d, path, sizeof path)) {
            gaddr spec = spec_of(path), info = tb_NewPtr(16, false, true);
            if (tb_FSpGetFInfo(spec, info) == noErr)
                t = rd32(info);
            tb_DisposePtr(info);
            tb_DisposePtr(spec);
        }
        if (a[0])
            wr32(a[0], t);
        return t ? noErr : paramErr;
    }
    case 0x002B: /* SetMacOSFileType */
        d->file_type = a[0];
        return noErr;
    case 0x0028: /* GetMIMEType(str) */
    case 0x0042: { /* GetMIMETypeAsync(str, completion, refcon) */
        char path[PATH_MAX];
        const char *m = file_of(d, path, sizeof path) ? mime_of(path) : "";
        put_pstr(a[0], m);
        if (k->what == 0x0042)
            complete(a[1], a[0], a[2], noErr);
        return noErr;
    }
    case 0x002C: /* SetTimeBase */
        d->timebase = a[0];
        return noErr;
    case 0x002D: /* GetInfoFlags */
        if (a[0])
            wr32(a[0], kDataHInfoFlagNeverStreams);
        return noErr;
    case 0x0014: { /* GetVolumeList(*list): Macintosh HD */
        gaddr h = tb_NewHandle(6, false, true);
        if (h && rd32(h))
            wr16(rd32(h), (uint16_t)-1);
        if (a[0])
            wr32(a[0], h);
        return h ? noErr : memFullErr;
    }
    case 0x0040: { /* GetFileTypeOrdering(*handle): Mac OS type, extension, MIME type */
        gaddr h = tb_NewHandle(12, false, false);
        if (h && rd32(h)) {
            wr32(rd32(h), fourcc("ftyp"));
            wr32(rd32(h) + 4, fourcc("fext"));
            wr32(rd32(h) + 8, fourcc("mime"));
        }
        if (a[0])
            wr32(a[0], h);
        return h ? noErr : memFullErr;
    }
    case 0x003C: { /* SetDataRefExtension(extension, idType) */
        int at = -1;
        for (int i = 0; i < MAX_EXT; i++)
            if (d->ext[i].data && d->ext[i].type == a[1])
                at = i;
        for (int i = 0; i < MAX_EXT && at < 0; i++)
            if (!d->ext[i].data)
                at = i;
        if (at < 0)
            return memFullErr;
        free(d->ext[at].data);
        d->ext[at].type = a[1];
        d->ext[at].data = qt_dref_bytes(a[0], &d->ext[at].len);
        return noErr;
    }
    case 0x003D: { /* GetDataRefExtension(*extension, idType) */
        for (int i = 0; i < MAX_EXT; i++)
            if (d->ext[i].data && d->ext[i].type == a[1]) {
                if (a[0])
                    wr32(a[0], qt_dref_handle(d->ext[i].data, d->ext[i].len));
                return noErr;
            }
        if (a[0])
            wr32(a[0], 0);
        return paramErr;
    }
    case 0x0043: /* GetInfo(what, info): nothing more to say */
        return paramErr;
    case 0x0103: /* PlaybackHints */
    case 0x010E: /* PlaybackHints64 */
    case 0x0111: /* SetTimeHints */
        return noErr;
    case 0x0110: /* GetDataRate(flags, *bytesPerSecond): a disk's speed */
        if (a[1])
            wr32(a[1], 10 * 1024 * 1024);
        return noErr;
    }
    return qtcBadSelector;
}

static qtc_class handlers[] = {
    { 0x64686C72, 0x616C6973, 0x6170706C, 0, "Apple Alias Data Handler", "Files", 0x00040000, { "DataH" }, sizeof(dhlr), dh_call, 0 },
    { 0x64686C72, 0x686E646C, 0x6170706C, 0, "Apple Handle Data Handler", "Handles", 0x00040000, { "DataH" }, sizeof(dhlr), dh_call, 0 },
    { 0x64686C72, 0x70747220, 0x6170706C, 0, "Apple Pointer Data Handler", "Memory", 0x00040000, { "DataH" }, sizeof(dhlr), dh_call, 0 },
    { 0x64686C72, 0x72737263, 0x6170706C, 0, "Apple Resource Data Handler", "Resources", 0x00040000, { "DataH" }, sizeof(dhlr), dh_call, 0 },
    { 0x64686C72, 0x75726C20, 0x6170706C, 0, "Apple URL Data Handler", "File URLs", 0x00040000, { "DataH" }, sizeof(dhlr), dh_call, 0 },
    { 0x64686C72, 0x6E756C6C, 0x6170706C, 0, "Apple Null Data Handler", "No data", 0x00040000, { "DataH" }, sizeof(dhlr), dh_call, 0 },
};

void qt_dhlr_register(void)
{
    for (size_t i = 0; i < sizeof handlers / sizeof handlers[0]; i++)
        qtc_register(&handlers[i]);
}
