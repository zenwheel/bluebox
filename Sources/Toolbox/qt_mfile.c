/*
 * Movie files and where movies come from and go: CreateMovieFile, movies
 * from data references, handles, data forks at an offset and a program's
 * own reading procedure, movies put into handles and data forks,
 * flattening (every sample copied into one file, interleaved by time),
 * QuickTime 4's shortcut movie files, the movie's default and anchor data
 * references, pictures of a movie at a time, importing and exporting by
 * type through movie import and export components, and searching a
 * movie's text.
 */
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "filemgr_internal.h"
#include "qd_internal.h"
#include "qt_atoms.h"
#include "qt_comp.h"
#include "qt_dref.h"
#include "qt_errors.h"
#include "qt_model.h"
#include "qt_movie_internal.h"
#include "tb_internal.h"

enum {
    fsRdPerm = 1, fsRdWrPerm = 3, fsFromStart = 1, dupFNErr = -48,
    createMovieFileDeleteCurFile = (int32_t)0x80000000, createMovieFileDontCreateMovie = 1 << 30,
    createMovieFileDontOpenFile = 1 << 29, createMovieFileDontCreateResFile = 1 << 28,
    flattenAddMovieToDataFork = 1, flattenActiveTracksOnly = 4, flattenDontInterleaveFlatten = 8,
    flattenFSSpecPtrIsDataRefRecordPtr = 16, flattenCompressMovieResource = 32,
    flattenForceMovieResourceBeforeMovieData = 64,
    movieProgressOpen = 0, movieProgressUpdatePercent = 1, movieProgressClose = 2, progressOpFlatten = 1,
    newMovieActive = 1,
};

uint32_t mac_now(void);

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* An FSSpec's host path. */
static bool spec_path(gaddr spec, char *out, size_t outsize)
{
    return spec && fm_host_path((int16_t)rd16(spec), (int32_t)rd32(spec + 2), spec + 6, true, out, outsize);
}

/* ---- The movie atom among a source's top-level atoms ---- */

typedef bool (*reader_fn)(void *ctx, uint64_t off, void *buf, uint32_t n);

/* The 'moov' atom at or after offset (malloc'd), or NULL. */
static uint8_t *find_moov(reader_fn rd, void *ctx, uint64_t offset, uint64_t limit, uint32_t *len)
{
    uint64_t at = offset;
    uint8_t h[16];
    for (int guard = 0; guard < 100000 && (!limit || at + 8 <= limit); guard++) {
        if (!rd(ctx, at, h, 8))
            return NULL;
        uint64_t n = be32(h);
        if (n == 1) {
            if (!rd(ctx, at + 8, h + 8, 8))
                return NULL;
            n = (uint64_t)be32(h + 8) << 32 | be32(h + 12);
        }
        if (n < 8)
            return NULL;
        if (!memcmp(h + 4, "moov", 4)) {
            if (n > 64u << 20)
                return NULL;
            uint8_t *b = malloc((size_t)n);
            if (!rd(ctx, at, b, (uint32_t)n)) {
                free(b);
                return NULL;
            }
            *len = (uint32_t)n;
            return b;
        }
        at += n;
    }
    return NULL;
}

static bool read_src(void *ctx, uint64_t off, void *buf, uint32_t n) { return qt_src_read(ctx, off, buf, n); }

/* Self references made explicit: the place the movie came from. */
static void self_refs_to(qtm_movie *mv, uint32_t type, const uint8_t *data, uint32_t len)
{
    for (int i = 0; i < mv->ntracks; i++) {
        qtm_media *md = mv->tracks[i]->media;
        for (int k = 0; md && k < md->ndrefs; k++) {
            qtm_dref *d = &md->drefs[k];
            if (!(d->flags & 1))
                continue;
            free(d->data);
            d->data = malloc(len ? len : 1);
            memcpy(d->data, data, len);
            d->len = len;
            d->type = type;
            d->flags &= ~1u;
        }
    }
}

/* A movie from a description in memory whose own references mean the
 * data reference given (a handle, a file, ...). */
static int16_t movie_for_ref(uint8_t *moov, uint32_t len, uint32_t type, const uint8_t *ref, uint32_t rlen,
                             gaddr out, int16_t flags)
{
    moov = qt_moov_uncompress(moov, &len);
    if (!moov)
        return qt_set_error(invalidMovie);
    qtm_movie *mv = qtm_parse(moov, len);
    free(moov);
    if (!mv)
        return qt_set_error(invalidMovie);
    char path[PATH_MAX];
    bool file = qt_dref_path(type, ref, rlen, NULL, path, sizeof path);
    if (!file)
        self_refs_to(mv, type, ref, rlen);
    gaddr m = qt_new_movie_for(mv, file ? path : NULL);
    if (!m)
        return qt_set_error(memFullErr);
    if (flags & newMovieActive)
        tb_SetMovieActive(m, 1);
    wr32(out, m);
    return qt_set_error(noErr);
}

int16_t tb_NewMovieFromDataRef(gaddr m, int16_t flags, gaddr id, int32_t dataRef, int32_t dataRefType)
{
    if (m)
        wr32(m, 0);
    if (!m)
        return qt_set_error(paramErr);
    uint32_t n;
    uint8_t *ref = qt_dref_bytes((gaddr)dataRef, &n);
    char path[PATH_MAX];
    int16_t err;
    if (qt_dref_path((uint32_t)dataRefType, ref, n, NULL, path, sizeof path)) {
        int16_t rid = id ? (int16_t)rd16(id) : 0;
        err = qt_open_movie_path(path, &rid, 0, false, m, flags);
        if (err == noMovieFound)
            err = qt_import_file_as_movie(path, m, flags);
        if (!err && id)
            wr16(id, (uint16_t)rid);
    } else {
        qt_src src;
        if (!qt_src_open((uint32_t)dataRefType, 0, ref, n, NULL, false, &src)) {
            free(ref);
            return qt_set_error(couldNotResolveDataRef);
        }
        uint32_t len;
        uint8_t *moov = find_moov(read_src, &src, 0, (uint64_t)qt_src_size(&src), &len);
        qt_src_close(&src);
        err = moov ? movie_for_ref(moov, len, (uint32_t)dataRefType, ref, n, m, flags) : qt_set_error(noMovieFound);
    }
    free(ref);
    return qt_set_error(err);
}

int16_t tb_NewMovieForDataRefFromHandle(gaddr theMovie, int32_t h, int16_t newMovieFlags, gaddr dataRefWasChanged,
                                        int32_t dataRef, int32_t dataRefType)
{
    if (dataRefWasChanged)
        wr8(dataRefWasChanged, 0);
    if (theMovie)
        wr32(theMovie, 0);
    if (!theMovie || !h || !rd32((gaddr)h))
        return qt_set_error(invalidMovie);
    uint32_t len = (uint32_t)tb_GetHandleSize((gaddr)h), rn;
    uint8_t *b = malloc(len + 8);
    gget(b + 8, rd32((gaddr)h), len);
    uint8_t *moov = b;
    if (len >= 8 && !memcmp(b + 12, "moov", 4)) {
        memmove(b, b + 8, len);
    } else {
        put32(b, len + 8);
        memcpy(b + 4, "moov", 4);
        len += 8;
    }
    uint8_t *ref = qt_dref_bytes((gaddr)dataRef, &rn);
    int16_t err = movie_for_ref(moov, len, dataRef ? (uint32_t)dataRefType : 0, ref, rn, theMovie, newMovieFlags);
    free(ref);
    return err;
}

int16_t tb_NewMovieFromDataFork64(gaddr theMovie, int32_t fRefNum, gaddr fileOffset, int16_t newMovieFlags,
                                  gaddr dataRefWasChanged)
{
    if (dataRefWasChanged)
        wr8(dataRefWasChanged, 0);
    char path[PATH_MAX];
    if (!fm_refnum_host_path((int16_t)fRefNum, path, sizeof path)) {
        if (theMovie)
            wr32(theMovie, 0);
        return qt_set_error(-51 /* rfNumErr */);
    }
    uint64_t off = fileOffset ? (uint64_t)rd32(fileOffset) << 32 | rd32(fileOffset + 4) : 0;
    return qt_open_movie_path(path, NULL, off, true, theMovie, newMovieFlags);
}

/* A program's procedure reading the movie: pascal OSErr (long offset,
 * long size, void *dataPtr, void *refCon). */
typedef struct {
    gaddr proc, refcon;
} user_reader;

static bool read_user(void *ctx, uint64_t off, void *buf, uint32_t n)
{
    user_reader *u = ctx;
    gaddr g = tb_NewPtr((int32_t)(n ? n : 1), false, false);
    if (!g)
        return false;
    cpu68k *c = g_cpu;
    cpu_push16(c, 0);
    cpu_push32(c, (uint32_t)off);
    cpu_push32(c, n);
    cpu_push32(c, g);
    cpu_push32(c, u->refcon);
    guest_call(u->proc);
    int16_t err = (int16_t)cpu_pop16(c);
    if (!err)
        gget(buf, g, n);
    tb_DisposePtr(g);
    return !err;
}

int16_t tb_NewMovieFromUserProc(gaddr m, int16_t flags, gaddr dataRefWasChanged, int32_t getProc, gaddr refCon,
                                int32_t defaultDataRef, int32_t dataRefType)
{
    if (dataRefWasChanged)
        wr8(dataRefWasChanged, 0);
    if (m)
        wr32(m, 0);
    if (!m || !getProc)
        return qt_set_error(paramErr);
    user_reader u = { (gaddr)getProc, refCon };
    uint32_t len;
    uint8_t *moov = find_moov(read_user, &u, 0, 0, &len);
    if (!moov)
        return qt_set_error(noMovieFound);
    uint32_t rn;
    uint8_t *ref = qt_dref_bytes((gaddr)defaultDataRef, &rn);
    int16_t err = movie_for_ref(moov, len, defaultDataRef ? (uint32_t)dataRefType : 0, ref, rn, m, flags);
    free(ref);
    return err;
}

/* Through a data handler: DataHGetData(dh, h, hOffset, offset, size). */
typedef struct {
    uint32_t dh;
} dh_reader;

static bool read_dh(void *ctx, uint64_t off, void *buf, uint32_t n)
{
    dh_reader *r = ctx;
    gaddr h = tb_NewHandle((int32_t)(n ? n : 1), false, false);
    if (!h)
        return false;
    uint32_t args[] = { h, 0, (uint32_t)off, n };
    int32_t err = qtc_call_named(r->dh, "DataHGetData", args);
    if (!err)
        gget(buf, rd32(h), n);
    tb_DisposeHandle(h);
    return !err;
}

int16_t tb_NewMovieFromStorageOffset(gaddr theMovie, int32_t dh, gaddr fileOffset, int16_t newMovieFlags,
                                     gaddr dataRefWasChanged)
{
    if (dataRefWasChanged)
        wr8(dataRefWasChanged, 0);
    if (theMovie)
        wr32(theMovie, 0);
    if (!theMovie || !dh)
        return qt_set_error(paramErr);
    dh_reader r = { (uint32_t)dh };
    uint64_t off = fileOffset ? (uint64_t)rd32(fileOffset) << 32 | rd32(fileOffset + 4) : 0;
    uint32_t len;
    uint8_t *moov = find_moov(read_dh, &r, off, 0, &len);
    if (!moov)
        return qt_set_error(noMovieFound);
    /* the handler's data reference: what the movie's own references mean */
    gaddr out = tb_NewPtr(4, false, true);
    uint32_t args[] = { out };
    gaddr ref = qtc_call_named((uint32_t)dh, "DataHGetDataRef", args) ? 0 : rd32(out);
    tb_DisposePtr(out);
    uint32_t t = 0, rn = 0, ty, st, mf;
    if (components_describe(components_component_of((uint32_t)dh), &ty, &st, &mf))
        t = st; /* its type is the handler's subtype */
    uint8_t *rb = qt_dref_bytes(ref, &rn);
    int16_t err = movie_for_ref(moov, len, t, rb, rn, theMovie, newMovieFlags);
    free(rb);
    if (ref)
        tb_DisposeHandle(ref);
    return err;
}

/* ---- Movie files ---- */

int16_t tb_CreateMovieFile(gaddr fileSpec, int32_t creator, int16_t scriptTag, int32_t createMovieFileFlags,
                           gaddr resRefNum, gaddr newmovie)
{
    if (resRefNum)
        wr16(resRefNum, 0);
    if (newmovie)
        wr32(newmovie, 0);
    if (createMovieFileFlags & createMovieFileDeleteCurFile)
        tb_FSpDelete(fileSpec);
    int16_t err = tb_FSpCreate(fileSpec, creator, (int32_t)fourcc("MooV"), scriptTag);
    if (err && err != dupFNErr)
        return qt_set_error(err);
    if (!(createMovieFileFlags & createMovieFileDontCreateResFile)) {
        tb_FSpCreateResFile(fileSpec, creator, (int32_t)fourcc("MooV"), scriptTag);
        int16_t rerr = tb_ResError();
        if (rerr && rerr != dupFNErr)
            return qt_set_error(rerr);
    }
    if (!(createMovieFileFlags & createMovieFileDontOpenFile) && resRefNum &&
        !(createMovieFileFlags & createMovieFileDontCreateResFile)) {
        int16_t rn = tb_FSpOpenResFile(fileSpec, fsRdWrPerm);
        if (rn < 0)
            return qt_set_error(tb_ResError());
        wr16(resRefNum, (uint16_t)rn);
    }
    if (!(createMovieFileFlags & createMovieFileDontCreateMovie) && newmovie) {
        char path[PATH_MAX];
        if (!spec_path(fileSpec, path, sizeof path))
            return qt_set_error(fnfErr);
        qtm_movie *mv = qtm_new(600, mac_now());
        gaddr m = qt_new_movie_for(mv, path);
        if (!m)
            return qt_set_error(memFullErr);
        if (createMovieFileFlags & newMovieActive)
            tb_SetMovieActive(m, 1);
        wr32(newmovie, m);
    }
    return qt_set_error(noErr);
}

int16_t tb_DeleteMovieFile(gaddr fileSpec) { return qt_set_error(tb_FSpDelete(fileSpec)); }

int16_t tb_RemoveMovieResource(int16_t resRefNum, int16_t resId)
{
    int16_t saved = tb_CurResFile();
    tb_UseResFile(resRefNum);
    gaddr h = tb_Get1Resource((int32_t)fourcc("moov"), resId);
    int16_t err = h ? noErr : resNotFound;
    if (h) {
        tb_RemoveResource(h);
        err = tb_ResError();
        tb_DisposeHandle(h);
        tb_UpdateResFile(resRefNum);
    }
    tb_UseResFile(saved);
    return qt_set_error(err);
}

/* ---- Default and anchor data references ---- */

static void set_ref(uint8_t **data, uint32_t *len, int32_t dataRef)
{
    free(*data);
    *data = dataRef ? qt_dref_bytes((gaddr)dataRef, len) : NULL;
    if (!dataRef)
        *len = 0;
}

int16_t tb_SetMovieDefaultDataRef(int32_t theMovie, int32_t dataRef, int32_t dataRefType)
{
    qtm_movie *mv = qt_movie_model((gaddr)theMovie);
    if (!mv)
        return qt_set_error(invalidMovie);
    set_ref(&mv->default_dref, &mv->default_dref_len, dataRef);
    mv->default_dref_type = dataRef ? (uint32_t)dataRefType : 0;
    return noErr;
}

static int16_t get_ref(uint32_t type, const uint8_t *data, uint32_t len, gaddr dataRef, gaddr dataRefType)
{
    if (dataRef)
        wr32(dataRef, data ? qt_dref_handle(data, len) : 0);
    if (dataRefType)
        wr32(dataRefType, type);
    return noErr;
}

int16_t tb_GetMovieDefaultDataRef(int32_t theMovie, gaddr dataRef, gaddr dataRefType)
{
    qtm_movie *mv = qt_movie_model((gaddr)theMovie);
    if (!mv)
        return qt_set_error(invalidMovie);
    if (!mv->default_dref && qt_movie_path(mv)) { /* its own file */
        if (dataRef)
            wr32(dataRef, qt_dref_for_path(qt_movie_path(mv)));
        if (dataRefType)
            wr32(dataRefType, fourcc("alis"));
        return noErr;
    }
    if (!mv->default_dref)
        return qt_set_error(noDefaultDataRef);
    return get_ref(mv->default_dref_type, mv->default_dref, mv->default_dref_len, dataRef, dataRefType);
}

int16_t tb_SetMovieAnchorDataRef(int32_t theMovie, int32_t dataRef, int32_t dataRefType)
{
    qtm_movie *mv = qt_movie_model((gaddr)theMovie);
    if (!mv)
        return qt_set_error(invalidMovie);
    set_ref(&mv->anchor_dref, &mv->anchor_dref_len, dataRef);
    mv->anchor_dref_type = dataRef ? (uint32_t)dataRefType : 0;
    return noErr;
}

int16_t tb_GetMovieAnchorDataRef(int32_t theMovie, gaddr dataRef, gaddr dataRefType, gaddr outFlags)
{
    qtm_movie *mv = qt_movie_model((gaddr)theMovie);
    if (outFlags)
        wr32(outFlags, 0);
    if (!mv)
        return qt_set_error(invalidMovie);
    if (!mv->anchor_dref) /* the movie's file, when it has one */
        return tb_GetMovieDefaultDataRef(theMovie, dataRef, dataRefType);
    return get_ref(mv->anchor_dref_type, mv->anchor_dref, mv->anchor_dref_len, dataRef, dataRefType);
}

/* ---- Movies into handles and data forks ---- */

/* A movie's description with references to `path` made its own and its
 * own made references to its file (for a movie that will be elsewhere). */
static uint8_t *description_for(qtm_movie *mv, const char *path, uint32_t *len)
{
    const char *own = qt_movie_path(mv);
    if (!path || (own && !strcmp(path, own)))
        return qtm_serialize(mv, false, len);
    qtm_movie *c = qt_copy_model(mv);
    if (!c)
        return NULL;
    gaddr own_alias = own ? qt_dref_for_path(own) : 0;
    uint32_t an = 0;
    uint8_t *ab = own_alias ? qt_dref_bytes(own_alias, &an) : NULL;
    for (int i = 0; i < c->ntracks; i++) {
        qtm_media *md = c->tracks[i]->media;
        for (int k = 0; md && k < md->ndrefs; k++) {
            qtm_dref *d = &md->drefs[k];
            char p[PATH_MAX];
            if (d->flags & 1) {
                if (ab) {
                    free(d->data);
                    d->data = malloc(an);
                    memcpy(d->data, ab, an);
                    d->len = an;
                    d->type = fourcc("alis");
                    d->flags = 0;
                }
            } else if (qt_dref_path(d->type, d->data, d->len, NULL, p, sizeof p) && !strcmp(p, path)) {
                free(d->data);
                d->data = NULL;
                d->len = 0;
                d->type = fourcc("alis");
                d->flags = 1;
            }
        }
    }
    uint8_t *out = qtm_serialize(c, false, len);
    qtm_free(c);
    free(ab);
    if (own_alias)
        tb_DisposeHandle(own_alias);
    return out;
}

static int16_t into_handle(const uint8_t *b, uint32_t n, gaddr h)
{
    if (!h)
        return paramErr;
    tb_SetHandleSize(h, (int32_t)n);
    if ((uint32_t)tb_GetHandleSize(h) != n)
        return memFullErr;
    gput(rd32(h), b, n);
    return noErr;
}

int16_t tb_PutMovieIntoHandle(int32_t theMovie, int32_t publicMovie)
{
    qtm_movie *mv = qt_movie_model((gaddr)theMovie);
    if (!mv)
        return qt_set_error(invalidMovie);
    qtm_update_duration(mv);
    uint32_t n;
    uint8_t *b = qtm_serialize(mv, false, &n);
    int16_t err = into_handle(b, n, (gaddr)publicMovie);
    free(b);
    return qt_set_error(err);
}

int16_t tb_PutMovieForDataRefIntoHandle(int32_t theMovie, int32_t dataRef, int32_t dataRefType, int32_t publicMovie)
{
    qtm_movie *mv = qt_movie_model((gaddr)theMovie);
    if (!mv)
        return qt_set_error(invalidMovie);
    uint32_t rn, n;
    uint8_t *ref = qt_dref_bytes((gaddr)dataRef, &rn);
    char path[PATH_MAX];
    bool file = qt_dref_path((uint32_t)dataRefType, ref, rn, NULL, path, sizeof path);
    free(ref);
    qtm_update_duration(mv);
    uint8_t *b = description_for(mv, file ? path : NULL, &n);
    int16_t err = b ? into_handle(b, n, (gaddr)publicMovie) : memFullErr;
    free(b);
    return qt_set_error(err);
}

/* The description written at an offset of an open file, a 'free' atom
 * after it filling up to maxSize. */
static int16_t into_fork(qtm_movie *mv, int16_t refnum, uint64_t offset, int32_t maxSize)
{
    char path[PATH_MAX];
    bool rsrc, w;
    uint32_t pos, flen;
    if (!fm_fork_state(refnum, path, sizeof path, &rsrc, &w, &pos, &flen))
        return -51 /* rfNumErr */;
    qtm_update_duration(mv);
    uint32_t n;
    uint8_t *b = description_for(mv, path, &n);
    if (!b)
        return memFullErr;
    if (maxSize > 0 && n > (uint32_t)maxSize) {
        free(b);
        return paramErr;
    }
    uint32_t total = n;
    if (maxSize > 0 && (uint32_t)maxSize - n >= 8) { /* the rest marked free */
        total = (uint32_t)maxSize;
        b = realloc(b, total);
        memset(b + n, 0, total - n);
        put32(b + n, total - n);
        memcpy(b + n + 4, "free", 4);
    }
    gaddr g = tb_NewPtr((int32_t)total, false, false);
    if (!g) {
        free(b);
        return memFullErr;
    }
    gput(g, b, total);
    free(b);
    uint32_t actual;
    int16_t err = fm_fork_io(refnum, true, fsFromStart, (int64_t)offset, total, g, &actual);
    tb_DisposePtr(g);
    fm_fork_seek(refnum, fsFromStart, pos);
    return err;
}

int16_t tb_PutMovieIntoDataFork(int32_t theMovie, int16_t fRefNum, int32_t offset, int32_t maxSize)
{
    qtm_movie *mv = qt_movie_model((gaddr)theMovie);
    if (!mv)
        return qt_set_error(invalidMovie);
    return qt_set_error(into_fork(mv, fRefNum, (uint32_t)offset, maxSize));
}

int16_t tb_PutMovieIntoDataFork64(int32_t theMovie, int32_t fRefNum, gaddr offset, int32_t maxSize)
{
    qtm_movie *mv = qt_movie_model((gaddr)theMovie);
    if (!mv)
        return qt_set_error(invalidMovie);
    uint64_t off = offset ? (uint64_t)rd32(offset) << 32 | rd32(offset + 4) : 0;
    return qt_set_error(into_fork(mv, (int16_t)fRefNum, off, maxSize));
}

/* ---- Flattening: every sample into one file, then the description ---- */

typedef struct {
    qtm_movie *mv;
    gaddr movie;
    gaddr proc;
    int32_t refcon;
} progress;

/* pascal OSErr (Movie, short message, short whatOperation, Fixed percentDone, long refcon) */
static bool tell(progress *p, int16_t message, int32_t percent)
{
    if (!p->proc)
        return true;
    cpu68k *c = g_cpu;
    cpu_push16(c, 0);
    cpu_push32(c, p->movie);
    cpu_push16(c, (uint16_t)message);
    cpu_push16(c, progressOpFlatten);
    cpu_push32(c, (uint32_t)percent);
    cpu_push32(c, (uint32_t)p->refcon);
    guest_call(p->proc);
    return cpu_pop16(c) == noErr;
}

/* A chunk to copy: a track's runs from one to the next chunk start. */
typedef struct {
    int track, first, last;
    double when;  /* its start in seconds of media time, for interleaving */
} chunk_ref;

static int by_time(const void *a, const void *b)
{
    const chunk_ref *x = a, *y = b;
    if (x->when != y->when)
        return x->when < y->when ? -1 : 1;
    return x->track - y->track;
}

/* The flattened file's data: an 'mdat' atom with every chunk, the copy's
 * runs pointing into it and its references made the file's own. */
static int16_t write_flattened(qtm_movie *mv, qtm_movie *copy, const char *dst_path, int16_t refnum, bool interleave,
                               progress *pr, uint64_t *end)
{
    int nchunks = 0;
    for (int t = 0; t < copy->ntracks; t++)
        if (copy->tracks[t]->media)
            for (int i = 0; i < copy->tracks[t]->media->nruns; i++)
                nchunks += i == 0 || copy->tracks[t]->media->runs[i].chunk;
    chunk_ref *cr = malloc((size_t)(nchunks ? nchunks : 1) * sizeof *cr);
    int k = 0;
    for (int t = 0; t < copy->ntracks; t++) {
        qtm_media *md = copy->tracks[t]->media;
        if (!md)
            continue;
        int64_t at = 0;
        for (int i = 0; i < md->nruns;) {
            int j = i + 1;
            while (j < md->nruns && !md->runs[j].chunk)
                j++;
            cr[k++] = (chunk_ref){ t, i, j - 1, interleave ? (double)at / md->scale : (double)t * 1e12 + at };
            for (int r = i; r < j; r++)
                at += (int64_t)md->runs[r].count * md->runs[r].duration;
            i = j;
        }
    }
    qsort(cr, (size_t)nchunks, sizeof *cr, by_time);
    /* sources, per track and reference */
    int16_t err = noErr;
    uint8_t hdr[16] = { 0, 0, 0, 1 };
    memcpy(hdr + 4, "mdat", 4);
    uint64_t data_at = 16, pos = 16; /* a 64-bit 'mdat' header, sized at the end */
    gaddr buf = tb_NewPtr(65536, false, false);
    if (!buf) {
        free(cr);
        return memFullErr;
    }
    uint64_t total = 0, done = 0;
    for (int i = 0; i < nchunks; i++)
        for (int r = cr[i].first; r <= cr[i].last; r++)
            total += copy->tracks[cr[i].track]->media->runs[r].bytes;
    int last_percent = -1;
    for (int i = 0; i < nchunks && !err; i++) {
        qtm_track *ct = copy->tracks[cr[i].track];
        qtm_media *cm = ct->media, *om = mv->tracks[cr[i].track]->media;
        for (int r = cr[i].first; r <= cr[i].last && !err; r++) {
            qtm_run *run = &cm->runs[r];
            int16_t di = qtm_desc_dref(om, run->desc);
            const qtm_dref *d = di >= 1 && di <= om->ndrefs ? &om->drefs[di - 1] : NULL;
            qt_src src;
            if (!d || !qt_src_open(d->type, d->flags, d->data, d->len, NULL, false, &src)) {
                err = couldNotResolveDataRef;
                break;
            }
            for (uint64_t off = 0; off < run->bytes && !err;) {
                uint32_t n = run->bytes - off > 65536 ? 65536 : (uint32_t)(run->bytes - off);
                uint32_t actual;
                if (!qt_src_read_guest(&src, run->offset + off, buf, n))
                    err = couldNotResolveDataRef;
                else
                    err = fm_fork_io(refnum, true, fsFromStart, (int64_t)(pos + off), n, buf, &actual);
                off += n;
            }
            qt_src_close(&src);
            run->offset = pos;
            pos += run->bytes;
            done += run->bytes;
            int percent = total ? (int)(done * 100 / total) : 100;
            if (percent != last_percent) {
                last_percent = percent;
                if (!tell(pr, movieProgressUpdatePercent, (int32_t)((int64_t)percent * 0x10000 / 100)))
                    err = progressProcAborted;
            }
        }
    }
    tb_DisposePtr(buf);
    free(cr);
    if (err)
        return err;
    /* the 'mdat' header, now its size is known */
    uint64_t size = pos;
    put32(hdr + 8, (uint32_t)(size >> 32));
    put32(hdr + 12, (uint32_t)size);
    gaddr g = tb_NewPtr(16, false, false);
    gput(g, hdr, 16);
    uint32_t actual;
    err = fm_fork_io(refnum, true, fsFromStart, 0, 16, g, &actual);
    tb_DisposePtr(g);
    (void)data_at;
    /* every reference is now the file itself */
    for (int t = 0; t < copy->ntracks; t++) {
        qtm_media *md = copy->tracks[t]->media;
        if (!md)
            continue;
        for (int i = 0; i < md->ndrefs; i++)
            free(md->drefs[i].data);
        free(md->drefs);
        md->drefs = NULL;
        md->ndrefs = 0;
        qtm_add_dref(md, fourcc("alis"), 1, NULL, 0);
        md->default_dref = 1;
        for (int i = 0; i < md->ndescs; i++)
            if (md->desc_len[i] >= 16) {
                md->descs[i][14] = 0;
                md->descs[i][15] = 1;
            }
    }
    (void)dst_path;
    *end = pos;
    return err;
}

/* The flattened file's data and, when asked, the description after it;
 * the copy of the description that goes with the file. */
static int16_t flatten_into(qtm_movie *mv, gaddr movie, int32_t flags, const char *path, qtm_movie **out_copy)
{
    *out_copy = NULL;
    /* two copies in step: one to read the samples from (its own references
     * made explicit, since it is no movie of ours), one for the new file */
    qtm_movie *src = qt_copy_model(mv), *copy = qt_copy_model(mv);
    if (!src || !copy) {
        qtm_free(src);
        qtm_free(copy);
        return memFullErr;
    }
    if (flags & flattenActiveTracksOnly)
        for (int i = src->ntracks - 1; i >= 0; i--)
            if (!(src->tracks[i]->flags & 1)) {
                qtm_remove_track(src, src->tracks[i]);
                qtm_remove_track(copy, copy->tracks[i]);
            }
    const char *own = qt_movie_path(mv);
    if (own) {
        gaddr a = qt_dref_for_path(own);
        uint32_t an;
        uint8_t *ab = qt_dref_bytes(a, &an);
        self_refs_to(src, fourcc("alis"), ab, an);
        free(ab);
        if (a)
            tb_DisposeHandle(a);
    }
    int16_t refnum = 0;
    int16_t err = fm_open_fork(path, false, fsRdWrPerm, &refnum);
    if (err) {
        qtm_free(src);
        qtm_free(copy);
        return err;
    }
    gaddr pb = tb_NewPtr(IOParam_SIZEOF, false, true);
    wr16(pb + IOParam_ioRefNum_off, (uint16_t)refnum);
    wr32(pb + IOParam_ioMisc_off, 0);
    tb_PBSetEOF(pb, false);
    tb_DisposePtr(pb);
    progress pr = { mv, movie, mv->progress_proc, mv->progress_refcon };
    tell(&pr, movieProgressOpen, 0);
    uint64_t end = 0;
    err = write_flattened(src, copy, path, refnum, !(flags & flattenDontInterleaveFlatten), &pr, &end);
    if (!err && (flags & flattenAddMovieToDataFork)) {
        qtm_update_duration(copy);
        uint32_t n;
        uint8_t *b = qtm_serialize(copy, false, &n);
        gaddr g = tb_NewPtr((int32_t)n, false, false);
        uint32_t actual;
        if (g) {
            gput(g, b, n);
            err = fm_fork_io(refnum, true, fsFromStart, (int64_t)end, n, g, &actual);
            tb_DisposePtr(g);
        } else {
            err = memFullErr;
        }
        free(b);
    }
    fm_close_fork(refnum);
    tell(&pr, movieProgressClose, 0x10000);
    qtm_free(src);
    if (err) {
        qtm_free(copy);
        return err;
    }
    *out_copy = copy;
    return noErr;
}

/* The file to flatten into: created (or emptied), as CreateMovieFile would. */
static int16_t flat_file(gaddr theFile, int32_t creator, int16_t scriptTag, int32_t createFlags, bool want_rsrc,
                         char *path, size_t pathsize)
{
    if (createFlags & createMovieFileDeleteCurFile)
        tb_FSpDelete(theFile);
    int16_t err = tb_FSpCreate(theFile, creator, (int32_t)fourcc("MooV"), scriptTag);
    if (err && err != dupFNErr)
        return err;
    if (want_rsrc)
        tb_FSpCreateResFile(theFile, creator, (int32_t)fourcc("MooV"), scriptTag);
    return spec_path(theFile, path, pathsize) ? noErr : fnfErr;
}

int32_t tb_FlattenMovieData(int32_t theMovie, int32_t movieFlattenFlags, gaddr theFile, int32_t creator,
                            int16_t scriptTag, int32_t createMovieFileFlags)
{
    qtm_movie *mv = qt_movie_model((gaddr)theMovie);
    if (!mv) {
        qt_set_error(invalidMovie);
        return 0;
    }
    char path[PATH_MAX];
    int16_t err;
    if (movieFlattenFlags & flattenFSSpecPtrIsDataRefRecordPtr) { /* a DataReferenceRecord: type, Handle */
        uint32_t rn;
        uint8_t *ref = qt_dref_bytes(rd32(theFile + 4), &rn);
        bool ok = qt_dref_path(rd32(theFile), ref, rn, NULL, path, sizeof path);
        free(ref);
        if (!ok) {
            qt_set_error(couldNotResolveDataRef);
            return 0;
        }
        err = noErr;
    } else {
        err = flat_file(theFile, creator, scriptTag, createMovieFileFlags, false, path, sizeof path);
    }
    if (err) {
        qt_set_error(err);
        return 0;
    }
    qtm_movie *copy;
    err = flatten_into(mv, (gaddr)theMovie, movieFlattenFlags, path, &copy);
    if (err) {
        qt_set_error(err);
        return 0;
    }
    gaddr m = qt_new_movie_for(copy, path);
    qt_set_error(m ? noErr : memFullErr);
    return (int32_t)m;
}

int32_t tb_FlattenMovieDataToDataRef(int32_t theMovie, int32_t movieFlattenFlags, int32_t dataRef, int32_t dataRefType,
                                     int32_t creator, int16_t scriptTag, int32_t createMovieFileFlags)
{
    (void)creator, (void)scriptTag, (void)createMovieFileFlags;
    gaddr rec = tb_NewPtr(8, false, true);
    wr32(rec, (uint32_t)dataRefType);
    wr32(rec + 4, (uint32_t)dataRef);
    int32_t m = tb_FlattenMovieData(theMovie, movieFlattenFlags | flattenFSSpecPtrIsDataRefRecordPtr, rec, creator,
                                    scriptTag, createMovieFileFlags);
    tb_DisposePtr(rec);
    return m;
}

void tb_FlattenMovie(int32_t theMovie, int32_t movieFlattenFlags, gaddr theFile, int32_t creator, int16_t scriptTag,
                     int32_t createMovieFileFlags, gaddr resId, int32_t resName)
{
    qtm_movie *mv = qt_movie_model((gaddr)theMovie);
    if (!mv) {
        qt_set_error(invalidMovie);
        return;
    }
    char path[PATH_MAX];
    int16_t err = flat_file(theFile, creator, scriptTag, createMovieFileFlags, true, path, sizeof path);
    if (err) {
        qt_set_error(err);
        return;
    }
    qtm_movie *copy;
    err = flatten_into(mv, (gaddr)theMovie, movieFlattenFlags, path, &copy);
    if (err) {
        qt_set_error(err);
        return;
    }
    /* the description as the file's 'moov' resource too */
    gaddr m = qt_new_movie_for(copy, path);
    int16_t rn = tb_FSpOpenResFile(theFile, fsRdWrPerm);
    if (m && rn > 0) {
        err = tb_AddMovieResource((int32_t)m, rn, resId, resName);
        tb_CloseResFile(rn);
    } else {
        err = m ? tb_ResError() : memFullErr;
    }
    if (m)
        tb_DisposeMovie(m);
    qt_set_error(err);
}

/* QuickTime 4's shortcut movie file: a 'moov' holding only a reference
 * movie ('rmra' > 'rmda' > 'rdrf': flags, data reference type, its size
 * and bytes) naming the real one. */
int16_t tb_CreateShortcutMovieFile(gaddr fileSpec, int32_t creator, int16_t scriptTag, int32_t createMovieFileFlags,
                                   int32_t targetDataRef, int32_t targetDataRefType)
{
    char path[PATH_MAX];
    int16_t err = flat_file(fileSpec, creator, scriptTag, createMovieFileFlags, false, path, sizeof path);
    if (err)
        return qt_set_error(err);
    uint32_t rn;
    uint8_t *ref = qt_dref_bytes((gaddr)targetDataRef, &rn);
    uint32_t rdrf = 8 + 12 + rn, rmda = 8 + rdrf, rmra = 8 + rmda, moov = 8 + rmra;
    uint8_t *b = calloc(1, moov);
    put32(b, moov);
    memcpy(b + 4, "moov", 4);
    put32(b + 8, rmra);
    memcpy(b + 12, "rmra", 4);
    put32(b + 16, rmda);
    memcpy(b + 20, "rmda", 4);
    put32(b + 24, rdrf);
    memcpy(b + 28, "rdrf", 4);
    put32(b + 32, 0);
    put32(b + 36, (uint32_t)targetDataRefType);
    put32(b + 40, rn);
    memcpy(b + 44, ref, rn);
    free(ref);
    int16_t refnum = 0;
    err = fm_open_fork(path, false, fsRdWrPerm, &refnum);
    if (!err) {
        gaddr g = tb_NewPtr((int32_t)moov, false, false);
        gput(g, b, moov);
        uint32_t actual;
        err = fm_fork_io(refnum, true, fsFromStart, 0, moov, g, &actual);
        tb_DisposePtr(g);
        fm_close_fork(refnum);
    }
    free(b);
    return qt_set_error(err);
}

/* A reference movie's first reference: the movie it stands for. */
bool qt_reference_movie_target(const uint8_t *moov, uint32_t len, uint32_t *type, uint8_t **ref, uint32_t *rn)
{
    const uint8_t *p = moov + 8, *end = moov + len;
    for (int depth = 0; depth < 3 && p + 8 <= end;) {
        uint32_t n = be32(p);
        if (n < 8 || p + n > end)
            return false;
        const char *want = depth == 0 ? "rmra" : depth == 1 ? "rmda" : "rdrf";
        if (!memcmp(p + 4, want, 4)) {
            if (depth == 2) {
                if (n < 20)
                    return false;
                *type = be32(p + 12);
                *rn = be32(p + 16);
                if (20 + *rn > n)
                    return false;
                *ref = malloc(*rn ? *rn : 1);
                memcpy(*ref, p + 20, *rn);
                return true;
            }
            end = p + n;
            p += 8;
            depth++;
        } else {
            p += n;
        }
    }
    return false;
}

/* ---- Pictures of a movie ---- */

/* A picture of RGBA pixels: drawn into a GWorld, then copied into a
 * picture recorded there. */
gaddr qt_picture_of_rgba(const uint8_t *rgba, int w, int h)
{
    gaddr r = tb_NewPtr(8, false, true);
    wr16(r + 4, (uint16_t)h);
    wr16(r + 6, (uint16_t)w);
    gaddr out = tb_NewPtr(4, false, true);
    gaddr pic = 0;
    if (tb_NewGWorld(out, 32, r, 0, 0, 0) == noErr && rd32(out)) {
        gaddr gw = rd32(out);
        gaddr saved = tb_NewPtr(8, false, true);
        tb_GetGWorld(saved, saved + 4);
        tb_SetGWorld(gw, 0);
        tb_LockPixels(tb_GetGWorldPixMap(gw));
        qd_draw_rgba(rgba, w, h, (qrect){ 0, 0, (int16_t)h, (int16_t)w });
        pic = tb_OpenPicture(r);
        /* into the port's own bits, so that the picture records them */
        gaddr pm = rd32(tb_GetGWorldPixMap(gw));
        tb_CopyBits(pm, gw + GrafPort_portBits_off, r, r, 0, 0);
        tb_ClosePicture();
        tb_SetGWorld(rd32(saved), rd32(saved + 4));
        tb_DisposePtr(saved);
        tb_DisposeGWorld(gw);
    }
    tb_DisposePtr(out);
    tb_DisposePtr(r);
    return pic;
}

static int32_t picture_at(qtm_movie *mv, int64_t time)
{
    uint8_t *rgba = NULL;
    int w = 0, h = 0;
    if (!qt_movie_frame_at(mv, time, &rgba, &w, &h) || !w || !h) {
        free(rgba);
        return 0;
    }
    gaddr pic = qt_picture_of_rgba(rgba, w, h);
    free(rgba);
    return (int32_t)pic;
}

int32_t tb_GetMoviePict(int32_t theMovie, int32_t time)
{
    qtm_movie *mv = qt_movie_model((gaddr)theMovie);
    if (!mv) {
        qt_set_error(invalidMovie);
        return 0;
    }
    return picture_at(mv, time);
}

int32_t tb_GetMoviePosterPict(int32_t theMovie)
{
    qtm_movie *mv = qt_movie_model((gaddr)theMovie);
    if (!mv) {
        qt_set_error(invalidMovie);
        return 0;
    }
    return picture_at(mv, mv->poster_time);
}

int32_t tb_GetTrackPict(int32_t theTrack, int32_t time)
{
    qtm_movie *mv;
    qtm_track *t = qt_track_model((gaddr)theTrack, &mv);
    if (!t) {
        qt_set_error(invalidTrack);
        return 0;
    }
    return picture_at(mv, time);
}

/* ShowMoviePoster: the poster's picture drawn in the movie's poster box. */
void tb_ShowMoviePoster(int32_t theMovie)
{
    qtm_movie *mv = qt_movie_model((gaddr)theMovie);
    if (!mv) {
        qt_set_error(invalidMovie);
        return;
    }
    gaddr pic = (gaddr)tb_GetMoviePosterPict(theMovie);
    if (!pic)
        return;
    gaddr box = tb_NewPtr(8, false, true), port = tb_NewPtr(8, false, true), saved = tb_NewPtr(4, false, true);
    tb_GetPosterBox(theMovie, box);
    tb_GetMovieGWorld(theMovie, port, port + 4);
    tb_GetPort(saved);
    if (rd32(port))
        tb_SetPort(rd32(port));
    tb_DrawPicture(pic, box);
    tb_SetPort(rd32(saved));
    tb_KillPicture(pic);
    tb_DisposePtr(box);
    tb_DisposePtr(port);
    tb_DisposePtr(saved);
}

void tb_SetMoviePlayHints(int32_t theMovie, int32_t flags, int32_t flagsMask)
{
    qtm_movie *mv = qt_movie_model((gaddr)theMovie);
    if (mv)
        mv->play_hints = (mv->play_hints & ~flagsMask) | (flags & flagsMask);
}

/* ---- Importing and exporting by type ---- */

static uint32_t component_for(uint32_t kind, uint32_t subtype)
{
    return components_find_next(0, kind, subtype, 0);
}

int16_t tb_PasteHandleIntoMovie(int32_t h, int32_t handleType, int32_t theMovie, int32_t flags, int32_t userComp)
{
    qtm_movie *mv = qt_movie_model((gaddr)theMovie);
    if (!mv)
        return qt_set_error(invalidMovie);
    if (!h)
        return qt_set_error(paramErr);
    if (!handleType || (uint32_t)handleType == fourcc("moov")) {
        gaddr out = tb_NewPtr(4, false, true);
        int16_t err = tb_NewMovieFromHandle(out, h, 0, 0);
        if (!err) {
            tb_PasteMovieSelection(theMovie, (int32_t)rd32(out));
            err = tb_GetMoviesError();
            tb_DisposeMovie(rd32(out));
        }
        tb_DisposePtr(out);
        return qt_set_error(err);
    }
    uint32_t c = userComp ? (uint32_t)userComp : component_for(fourcc("eat "), (uint32_t)handleType);
    if (!c)
        return qt_set_error(cantFindHandler);
    int16_t err;
    uint32_t ci = components_component_of(c) ? c : components_open_component(c, &err);
    if (!ci)
        return qt_set_error(cantOpenHandler);
    gaddr used = tb_NewPtr(12, false, true);
    int32_t sel_time = mv->sel_time;
    /* MovieImportHandle(ci, dataH, theMovie, targetTrack, usedTrack*, atTime, addedDuration*, inFlags, outFlags*) */
    uint32_t args[] = { (uint32_t)h, (uint32_t)theMovie, 0, used, (uint32_t)sel_time, used + 4, (uint32_t)flags, used + 8 };
    int32_t r = qtc_call_named(ci, "MovieImportHandle", args);
    if (ci != c)
        components_close(ci);
    tb_DisposePtr(used);
    qt_movie_touch(mv);
    return qt_set_error((int16_t)r);
}

int16_t tb_PutMovieIntoTypedHandle(int32_t theMovie, int32_t targetTrack, int32_t handleType, int32_t publicMovie,
                                   int32_t start, int32_t dur, int32_t flags, int32_t userComp)
{
    qtm_movie *mv = qt_movie_model((gaddr)theMovie);
    if (!mv)
        return qt_set_error(invalidMovie);
    if ((uint32_t)handleType == fourcc("moov")) {
        int32_t s0 = mv->sel_time, d0 = mv->sel_duration;
        tb_SetMovieSelection(theMovie, start, dur);
        int32_t part = tb_CopyMovieSelection(theMovie);
        tb_SetMovieSelection(theMovie, s0, d0);
        if (!part)
            return qt_set_error(tb_GetMoviesError());
        int16_t err = tb_PutMovieIntoHandle(part, publicMovie);
        tb_DisposeMovie((gaddr)part);
        return err;
    }
    uint32_t c = userComp ? (uint32_t)userComp : component_for(fourcc("spit"), (uint32_t)handleType);
    if (!c)
        return qt_set_error(cantFindHandler);
    int16_t err;
    uint32_t ci = components_component_of(c) ? c : components_open_component(c, &err);
    if (!ci)
        return qt_set_error(cantOpenHandler);
    (void)flags;
    uint32_t args[] = { (uint32_t)publicMovie, (uint32_t)theMovie, (uint32_t)targetTrack, (uint32_t)start, (uint32_t)dur };
    int32_t r = qtc_call_named(ci, "MovieExportToHandle", args);
    if (ci != c)
        components_close(ci);
    return qt_set_error((int16_t)r);
}

/* What the scrap holds that QuickTime can make a movie of. */
int32_t tb_IsScrapMovie(int32_t targetTrack)
{
    (void)targetTrack;
    gaddr off = tb_NewPtr(4, false, true);
    static const char *types[] = { "moov", "PICT", "snd ", "TEXT", "PICT" };
    int32_t found = 0;
    for (int i = 0; i < 4 && !found; i++) {
        uint32_t t = fourcc(types[i]);
        if (tb_GetScrap(0, (int32_t)t, off) >= 0 && (i == 0 || component_for(fourcc("eat "), t)))
            found = (int32_t)t;
    }
    if (!found && qt_scrap_has_movie())
        found = (int32_t)fourcc("moov");
    tb_DisposePtr(off);
    return found;
}

/* A file's type: its Finder type. */
static uint32_t file_type(gaddr spec)
{
    gaddr info = tb_NewPtr(16, false, true);
    uint32_t t = tb_FSpGetFInfo(spec, info) == noErr ? rd32(info) : 0;
    tb_DisposePtr(info);
    return t;
}

int16_t tb_ConvertFileToMovieFile(gaddr inputFile, gaddr outputFile, int32_t creator, int16_t scriptTag, gaddr resID,
                                  int32_t flags, int32_t userComp, int32_t proc, int32_t refCon)
{
    (void)proc, (void)refCon;
    uint32_t t = file_type(inputFile);
    uint32_t c = userComp ? (uint32_t)userComp : component_for(fourcc("eat "), t);
    if (!c)
        return qt_set_error(noMovieFound);
    gaddr out = tb_NewPtr(8, false, true);
    int16_t err = tb_CreateMovieFile(outputFile, creator, scriptTag, (int32_t)createMovieFileDeleteCurFile, out, out + 4);
    if (err) {
        tb_DisposePtr(out);
        return err;
    }
    int16_t rn = (int16_t)rd16(out);
    gaddr movie = rd32(out + 4);
    int16_t cerr;
    uint32_t ci = components_component_of(c) ? c : components_open_component(c, &cerr);
    int32_t r = cantOpenHandler;
    if (ci) {
        gaddr used = tb_NewPtr(12, false, true);
        /* MovieImportFile(ci, theFile, theMovie, targetTrack, usedTrack*, atTime, addedDuration*, inFlags, outFlags*) */
        uint32_t args[] = { inputFile, movie, 0, used, 0, used + 4, (uint32_t)flags, used + 8 };
        r = qtc_call_named(ci, "MovieImportFile", args);
        tb_DisposePtr(used);
        if (ci != c)
            components_close(ci);
    }
    if (!r)
        r = tb_AddMovieResource((int32_t)movie, rn, resID, 0);
    tb_CloseMovieFile(rn);
    tb_DisposeMovie(movie);
    tb_DisposePtr(out);
    return qt_set_error((int16_t)r);
}

int16_t tb_ConvertMovieToFile(int32_t theMovie, int32_t onlyTrack, gaddr outputFile, int32_t fileType, int32_t creator,
                              int16_t scriptTag, gaddr resID, int32_t flags, int32_t userComp)
{
    if (!qt_movie_model((gaddr)theMovie))
        return qt_set_error(invalidMovie);
    if (!fileType || (uint32_t)fileType == fourcc("MooV")) {
        tb_FlattenMovie(theMovie, 0, outputFile, creator, scriptTag, (int32_t)createMovieFileDeleteCurFile, resID, 0);
        return tb_GetMoviesError();
    }
    uint32_t c = userComp ? (uint32_t)userComp : component_for(fourcc("spit"), (uint32_t)fileType);
    if (!c)
        return qt_set_error(cantFindHandler);
    int16_t err;
    uint32_t ci = components_component_of(c) ? c : components_open_component(c, &err);
    if (!ci)
        return qt_set_error(cantOpenHandler);
    (void)flags;
    gaddr dur = tb_NewPtr(4, false, true);
    /* MovieExportToFile(ci, theFile, theMovie, onlyThisTrack, startTime, duration) */
    uint32_t args[] = { outputFile, (uint32_t)theMovie, (uint32_t)onlyTrack, 0, (uint32_t)tb_GetMovieDuration(theMovie) };
    int32_t r = qtc_call_named(ci, "MovieExportToFile", args);
    tb_DisposePtr(dur);
    if (ci != c)
        components_close(ci);
    return qt_set_error((int16_t)r);
}

int16_t tb_GetMovieImporterForDataRef(int32_t dataRefType, int32_t dataRef, int32_t flags, gaddr importer)
{
    (void)flags;
    if (importer)
        wr32(importer, 0);
    uint32_t rn;
    uint8_t *ref = qt_dref_bytes((gaddr)dataRef, &rn);
    char path[PATH_MAX];
    uint32_t type = 0;
    if (qt_dref_path((uint32_t)dataRefType, ref, rn, NULL, path, sizeof path)) {
        gaddr spec = tb_NewPtr(FSSpec_SIZEOF, false, true);
        fm_spec_for_path(path, spec);
        type = file_type(spec);
        tb_DisposePtr(spec);
    }
    free(ref);
    uint32_t c = type ? component_for(fourcc("eat "), type) : 0;
    if (importer)
        wr32(importer, c);
    return qt_set_error(c ? noErr : noMovieFound);
}

/* What QuickTime can open: movies, pictures through a graphics importer,
 * the rest through movie importers. */
static void can_open(uint32_t type, const char *path, gaddr gi, gaddr movie, gaddr prefer)
{
    bool is_movie = type == fourcc("MooV");
    if (!is_movie && path) {
        int16_t id = 0;
        gaddr out = tb_NewPtr(4, false, true);
        if (qt_open_movie_path(path, &id, 0, false, out, 0) == noErr) {
            is_movie = true;
            tb_DisposeMovie(rd32(out));
        }
        tb_DisposePtr(out);
    }
    bool image = false;
    if (!is_movie && path) {
        gaddr spec = tb_NewPtr(FSSpec_SIZEOF, false, true), out = tb_NewPtr(4, false, true);
        fm_spec_for_path(path, spec);
        if (tb_GetGraphicsImporterForFile(spec, out) == noErr && rd32(out)) {
            image = true;
            components_close(rd32(out));
        }
        tb_DisposePtr(spec);
        tb_DisposePtr(out);
    }
    bool importable = !is_movie && type && component_for(fourcc("eat "), type);
    if (gi)
        wr8(gi, image);
    if (movie)
        wr8(movie, is_movie || importable || image);
    if (prefer)
        wr8(prefer, image);
}

int16_t tb_CanQuickTimeOpenFile(int32_t fileSpec, int32_t fileType, int32_t fileNameExtension,
                                gaddr outCanOpenWithGraphicsImporter, gaddr outCanOpenAsMovie,
                                gaddr outPreferGraphicsImporter, int32_t inFlags)
{
    (void)fileNameExtension, (void)inFlags;
    char path[PATH_MAX];
    bool have = fileSpec && spec_path((gaddr)fileSpec, path, sizeof path);
    uint32_t type = fileType ? (uint32_t)fileType : have ? file_type((gaddr)fileSpec) : 0;
    can_open(type, have ? path : NULL, outCanOpenWithGraphicsImporter, outCanOpenAsMovie, outPreferGraphicsImporter);
    return noErr;
}

int16_t tb_CanQuickTimeOpenDataRef(int32_t dataRef, int32_t dataRefType, gaddr outCanOpenWithGraphicsImporter,
                                   gaddr outCanOpenAsMovie, gaddr outPreferGraphicsImporter, int32_t inFlags)
{
    (void)inFlags;
    uint32_t rn;
    uint8_t *ref = qt_dref_bytes((gaddr)dataRef, &rn);
    char path[PATH_MAX];
    bool have = qt_dref_path((uint32_t)dataRefType, ref, rn, NULL, path, sizeof path);
    free(ref);
    uint32_t type = 0;
    if (have) {
        gaddr spec = tb_NewPtr(FSSpec_SIZEOF, false, true);
        fm_spec_for_path(path, spec);
        type = file_type(spec);
        tb_DisposePtr(spec);
    }
    can_open(type, have ? path : NULL, outCanOpenWithGraphicsImporter, outCanOpenAsMovie, outPreferGraphicsImporter);
    return noErr;
}

/* ---- Searching a movie's text ---- */

static bool text_match(const uint8_t *hay, uint32_t n, const uint8_t *needle, uint32_t m, bool cased, uint32_t from,
                       bool back, uint32_t *at)
{
    if (!m || m > n)
        return false;
    for (int64_t i = back ? (int64_t)(from < n - m ? from : n - m) : from; back ? i >= 0 : i + m <= n; back ? i-- : i++) {
        bool ok = true;
        for (uint32_t k = 0; k < m && ok; k++) {
            uint8_t a = hay[i + k], b = needle[k];
            ok = cased ? a == b : (a < 128 ? tolower(a) : a) == (b < 128 ? tolower(b) : b);
        }
        if (ok) {
            *at = (uint32_t)i;
            return true;
        }
    }
    return false;
}

int16_t tb_MovieSearchText(int32_t theMovie, int32_t text, int32_t size, int32_t searchFlags, gaddr searchTrack,
                           gaddr searchTime, gaddr searchOffset)
{
    qtm_movie *mv = qt_movie_model((gaddr)theMovie);
    if (!mv)
        return qt_set_error(invalidMovie);
    if (!text || size <= 0)
        return qt_set_error(paramErr);
    uint8_t *needle = malloc((size_t)size);
    gget(needle, (gaddr)text, (uint32_t)size);
    bool cased = searchFlags & 2, back = searchFlags & 4, wrap = searchFlags & 8, use_offset = searchFlags & 16;
    bool one = searchFlags & (1 << 18), enabled_only = searchFlags & (1 << 19);
    gaddr start_track = searchTrack ? rd32(searchTrack) : 0;
    int64_t start_time = searchTime ? (int32_t)rd32(searchTime) : 0;
    uint32_t start_off = use_offset && searchOffset ? rd32(searchOffset) + (back ? 0 : 1) : 0;
    int16_t err = movieTextNotFoundErr;
    for (int pass = 0; pass < (wrap ? 2 : 1) && err; pass++) {
        for (int ti = 0; ti < mv->ntracks && err; ti++) {
            qtm_track *t = mv->tracks[ti];
            if (!t->media || t->media->type != fourcc("text"))
                continue;
            if (enabled_only && !(t->flags & 1))
                continue;
            if (one && start_track && t->token != start_track)
                continue;
            qtm_media *md = t->media;
            uint32_t count = qtm_sample_count(md);
            int64_t mt = qtm_track_to_media(t, start_time, mv->scale, NULL);
            uint32_t first = mt >= 0 ? qtm_sample_at(md, mt, NULL, NULL) : 1;
            if (!first)
                first = back ? count : 1;
            if (pass == 1)
                first = back ? count : 1;
            for (int64_t s = first; s >= 1 && s <= count && err; back ? s-- : s++) {
                uint64_t off, bytes;
                uint32_t desc;
                if (!qtm_sample_info(md, (uint32_t)s, &off, &bytes, &desc, NULL, NULL) || bytes < 2 || bytes > 1 << 20)
                    continue;
                int16_t di = qtm_desc_dref(md, desc);
                if (di < 1 || di > md->ndrefs)
                    continue;
                qt_src src;
                const qtm_dref *d = &md->drefs[di - 1];
                if (!qt_src_open(d->type, d->flags, d->data, d->len, qt_movie_path(mv), false, &src))
                    continue;
                uint8_t *b = malloc((size_t)bytes);
                bool ok = qt_src_read(&src, off, b, (uint32_t)bytes);
                qt_src_close(&src);
                uint32_t n = ok ? (uint32_t)(b[0] << 8 | b[1]) : 0;
                if (n > bytes - 2)
                    n = (uint32_t)bytes - 2;
                uint32_t at;
                uint32_t from = (uint32_t)s == first && pass == 0 ? start_off : back ? n : 0;
                if (ok && text_match(b + 2, n, needle, (uint32_t)size, cased, from, back, &at)) {
                    int64_t st;
                    qtm_sample_time(md, (uint32_t)s, &st, NULL);
                    /* the media time back to movie time, through the first edit showing it */
                    int64_t movie_t = 0, edge = 0;
                    for (int e = 0; e < t->nedits; e++) {
                        qtm_edit *x = &t->edits[e];
                        int64_t mlen = x->duration * md->scale / mv->scale;
                        if (x->media_time >= 0 && st >= x->media_time && st < x->media_time + mlen) {
                            movie_t = edge + (st - x->media_time) * mv->scale / md->scale;
                            break;
                        }
                        edge += x->duration;
                    }
                    if (searchTrack)
                        wr32(searchTrack, t->token);
                    if (searchTime)
                        wr32(searchTime, (uint32_t)movie_t);
                    if (searchOffset)
                        wr32(searchOffset, at);
                    if (!(searchFlags & (1 << 16)))
                        tb_SetMovieTimeValue(theMovie, (int32_t)movie_t);
                    err = noErr;
                }
                free(b);
            }
        }
    }
    free(needle);
    return qt_set_error(err);
}

/* ---- Movie storage (QuickTime 6): a movie kept through a data handler ---- */

int16_t tb_CreateMovieStorage(int32_t dataRef, int32_t dataRefType, int32_t creator, int16_t scriptTag,
                              int32_t createMovieFileFlags, gaddr outDataHandler, gaddr newmovie)
{
    (void)scriptTag;
    if (outDataHandler)
        wr32(outDataHandler, 0);
    if (newmovie)
        wr32(newmovie, 0);
    gaddr dhp = tb_NewPtr(4, false, true);
    int16_t err = tb_OpenADataHandler(dataRef, dataRefType, 0, 0, 0, 0, dhp);
    uint32_t dh = rd32(dhp);
    tb_DisposePtr(dhp);
    if (err)
        return err;
    uint32_t cargs[] = { (uint32_t)creator, (createMovieFileFlags & createMovieFileDeleteCurFile) ? 1u : 0u, 0 };
    int32_t r = qtc_call_named(dh, "DataHCreateFileWithFlags", cargs);
    uint32_t none[1] = { 0 };
    if (!r)
        r = qtc_call_named(dh, "DataHOpenForWrite", none);
    if (r) {
        components_close(dh);
        return qt_set_error((int16_t)r);
    }
    if (!(createMovieFileFlags & createMovieFileDontCreateMovie) && newmovie) {
        uint32_t rn;
        uint8_t *ref = qt_dref_bytes((gaddr)dataRef, &rn);
        char path[PATH_MAX];
        bool file = qt_dref_path((uint32_t)dataRefType, ref, rn, NULL, path, sizeof path);
        qtm_movie *mv = qtm_new(600, mac_now());
        if (!file) { /* where its samples go: the storage */
            mv->default_dref = ref;
            mv->default_dref_len = rn;
            mv->default_dref_type = (uint32_t)dataRefType;
            ref = NULL;
        }
        free(ref);
        gaddr m = qt_new_movie_for(mv, file ? path : NULL);
        if (m && (createMovieFileFlags & newMovieActive))
            tb_SetMovieActive(m, 1);
        wr32(newmovie, m);
    }
    if (outDataHandler)
        wr32(outDataHandler, dh);
    else
        components_close(dh);
    return qt_set_error(noErr);
}

int16_t tb_OpenMovieStorage(int32_t dataRef, int32_t dataRefType, int32_t flags, gaddr outDataHandler)
{
    if (outDataHandler)
        wr32(outDataHandler, 0);
    /* kDataHCanRead (1), kDataHCanWrite (8): opened for those */
    return tb_OpenADataHandler(dataRef, dataRefType, 0, 0, 0, flags | 1, outDataHandler);
}

int16_t tb_CloseMovieStorage(int32_t dh)
{
    if (!dh)
        return qt_set_error(paramErr);
    uint32_t none[1] = { 0 };
    qtc_call_named((uint32_t)dh, "DataHCloseForWrite", none);
    qtc_call_named((uint32_t)dh, "DataHCloseForRead", none);
    components_close((uint32_t)dh);
    return qt_set_error(noErr);
}

int16_t tb_DeleteMovieStorage(int32_t dataRef, int32_t dataRefType)
{
    uint32_t rn;
    uint8_t *ref = qt_dref_bytes((gaddr)dataRef, &rn);
    char path[PATH_MAX];
    bool file = qt_dref_path((uint32_t)dataRefType, ref, rn, NULL, path, sizeof path);
    free(ref);
    if (!file)
        return qt_set_error(paramErr);
    gaddr spec = tb_NewPtr(FSSpec_SIZEOF, false, true);
    fm_spec_for_path(path, spec);
    int16_t err = tb_FSpDelete(spec);
    tb_DisposePtr(spec);
    return qt_set_error(err);
}

int16_t tb_AddMovieToStorage(int32_t theMovie, int32_t dh)
{
    if (!qt_movie_model((gaddr)theMovie))
        return qt_set_error(invalidMovie);
    gaddr id = tb_NewPtr(2, false, true);
    uint32_t args[] = { (uint32_t)theMovie, id };
    int32_t r = qtc_call_named((uint32_t)dh, "DataHAddMovie", args);
    tb_DisposePtr(id);
    return qt_set_error((int16_t)r);
}

int16_t tb_UpdateMovieInStorage(int32_t theMovie, int32_t dh)
{
    if (!qt_movie_model((gaddr)theMovie))
        return qt_set_error(invalidMovie);
    uint32_t args[] = { (uint32_t)theMovie, 0 };
    return qt_set_error((int16_t)qtc_call_named((uint32_t)dh, "DataHUpdateMovie", args));
}

int16_t tb_PutMovieIntoStorage(int32_t theMovie, int32_t dh, gaddr offset, int32_t maxSize)
{
    qtm_movie *mv = qt_movie_model((gaddr)theMovie);
    if (!mv)
        return qt_set_error(invalidMovie);
    qtm_update_duration(mv);
    uint32_t n;
    uint8_t *b = qtm_serialize(mv, false, &n);
    if (maxSize > 0 && n > (uint32_t)maxSize) {
        free(b);
        return qt_set_error(paramErr);
    }
    gaddr g = tb_NewPtr((int32_t)n, false, false);
    gput(g, b, n);
    free(b);
    uint32_t args[] = { g, offset, n, 0, 0 };
    int32_t r = qtc_call_named((uint32_t)dh, "DataHWrite64", args);
    tb_DisposePtr(g);
    return qt_set_error((int16_t)r);
}
