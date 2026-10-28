/*
 * QuickTime's Movie Toolbox: movies opened, played, edited and saved.
 *
 * A movie's description (its 'moov' atom: time scale, duration, tracks and
 * their media) is read from the file's resource fork as classic movies
 * kept it, or from the data fork of a flattened one, into the records the
 * Movie Toolbox's calls read and change (qt_model.c; the track, media and
 * sample calls are qt_movies.c).  It is played from those records,
 * written out again as a 'moov' atom whenever they change.
 *
 * Movies whose codecs we have (Cinepak, Animation, Graphics, Apple Video,
 * None, Photo-JPEG and the host's; uncompressed, IMA and MACE sound; music,
 * text and sprite tracks) play through our own decoders (qt_native.c),
 * headless runs included: frames for the movie's clock, and sound sent to
 * the host a second ahead.  Samples are read wherever their data
 * references say (the movie's file, others, handles in memory).  Movies
 * that need something else are played by libvlc (qt_vlc.c), given the
 * file itself when it is flattened or else a flattened copy (the data
 * fork behind an 'mdat' header, then the 'moov' atom with its chunk
 * offsets moved to match); with a host attached and libvlc available,
 * frames are drawn into the movie's GWorld when the application gives the
 * movie time (MoviesTask, UpdateMovie, the movie controller) and VLC plays
 * the sound; otherwise (headless runs) such movies keep time without
 * pictures or sound.
 *
 * Time bases (NewTimeBase, GetMovieTimeBase) keep time from the machine's
 * clock or from a master time base, so one movie can follow another's time
 * and rate (SetMovieMasterTimeBase).
 *
 * The movie controller is a component of our own: a play/pause button and a
 * time slider under the movie, with the standard actions and action filters.
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "forks.h"
#include "m68k.h"
#include "qd_internal.h"
#include "qt_atoms.h"
#include "qt_dref.h"
#include "qt_edit.h"
#include "qt_model.h"
#include "qt_movie_internal.h"
#include "qt_native.h"
#include "qt_vlc.h"
#include "qt_vr.h"
#include "tb_internal.h"
#include "trap.h"

#include "qt_errors.h"
enum { newMovieActive = 1 };

static int16_t movies_error, sticky_error;

int16_t qt_set_error(int16_t err)
{
    movies_error = err;
    if (err && !sticky_error)
        sticky_error = err;
    return err;
}

static int16_t set_error(int16_t err) { return qt_set_error(err); }

/* ---------------------------------------------------------------------- */
/* Movies */

#define MAX_MOVIES 64
#define MAX_SOURCES 16

typedef struct {
    bool used;
    gaddr handle;        /* the Movie the application holds */
    qtm_movie *mv;       /* its description */
    char *data_path;     /* the file it came from (its own data's), or NULL */
    char *play_path;     /* what VLC plays */
    bool temp_file;      /* play_path is ours to delete */
    qrect box;
    gaddr port, gdh;
    bool active, playing, looping;
    int32_t rate;        /* Fixed */
    int16_t volume;
    int64_t base_ms;     /* time when stopped, or at start_ns */
    int64_t start_ns;    /* the time source's (the machine's, or the master's), ns */
    gaddr timebase;      /* the TimeBase GetMovieTimeBase gives */
    gaddr master;        /* SetMovieMasterTimeBase's, or 0 for the clock */
    gaddr clock;         /* SetMovieMasterClock's component (ours keep machine time) */
    bool vlc_held;       /* VLC paused while the master stands still */
    gaddr clip;          /* SetMovieClipRgn's, in the movie's coordinates (ours) */
    gaddr display_clip;  /* SetMovieDisplayClipRgn's, in the port's (ours) */
    qt_vlc_player *player;
    qt_native *native;   /* our decoders, when they cover the movie */
    uint8_t *moov;       /* the description as played (written from mv) */
    uint32_t moov_len;
    bool virtual_play;   /* its chunk offsets name their tracks and references (qtm_serialize) */
    bool stale;          /* mv changed since: played from it again */
    bool changed;        /* edited since opened or last saved */
    int64_t sound_ms;    /* native sound sent to the host up to here */
    bool sound_on;
    int64_t music_ms;    /* and the music track's MIDI */
    bool music_on;
    uint8_t *frame;
    int fw, fh;
    bool frame_indexed;
    int index_depth;     /* palettized video with the standard colours (0: none) */
    uint32_t serial;
    struct {             /* samples' sources, for a description played with virtual offsets */
        uint64_t key;    /* track << 8 | reference */
        qt_src src;
        bool ok;
    } srcs[MAX_SOURCES];
    int nsrcs;
} movie;

static movie movies[MAX_MOVIES];

static movie *movie_of(gaddr h)
{
    for (int i = 0; h && i < MAX_MOVIES; i++)
        if (movies[i].used && movies[i].handle == h)
            return &movies[i];
    return NULL;
}

static movie *movie_of_model(const qtm_movie *mv)
{
    for (int i = 0; mv && i < MAX_MOVIES; i++)
        if (movies[i].used && movies[i].mv == mv)
            return &movies[i];
    return NULL;
}

/* The track and media tokens the program holds. */
static qtm_track *find_track(gaddr t, movie **mp)
{
    for (int i = 0; t && i < MAX_MOVIES; i++)
        for (int k = 0; movies[i].used && k < movies[i].mv->ntracks; k++)
            if (movies[i].mv->tracks[k]->token == t) {
                if (mp)
                    *mp = &movies[i];
                return movies[i].mv->tracks[k];
            }
    return NULL;
}

static qtm_media *find_media(gaddr md, qtm_track **tp, movie **mp)
{
    for (int i = 0; md && i < MAX_MOVIES; i++)
        for (int k = 0; movies[i].used && k < movies[i].mv->ntracks; k++) {
            qtm_track *t = movies[i].mv->tracks[k];
            if (t->media && t->media->token == md) {
                if (tp)
                    *tp = t;
                if (mp)
                    *mp = &movies[i];
                return t->media;
            }
        }
    return NULL;
}

/* For qt_movies.c and the media handlers: a movie's, track's or media's records. */
qtm_movie *qt_movie_model(gaddr theMovie)
{
    movie *m = movie_of(theMovie);
    return m ? m->mv : NULL;
}

qtm_track *qt_track_model(gaddr theTrack, qtm_movie **mv)
{
    movie *m = NULL;
    qtm_track *t = find_track(theTrack, &m);
    if (mv)
        *mv = m ? m->mv : NULL;
    return t;
}

qtm_media *qt_media_model(gaddr theMedia, qtm_track **t, qtm_movie **mv)
{
    movie *m = NULL;
    qtm_media *md = find_media(theMedia, t, &m);
    if (mv)
        *mv = m ? m->mv : NULL;
    return md;
}

gaddr qt_movie_handle(const qtm_movie *mv)
{
    movie *m = movie_of_model(mv);
    return m ? m->handle : 0;
}

const char *qt_movie_path(const qtm_movie *mv)
{
    movie *m = movie_of_model(mv);
    return m ? m->data_path : NULL;
}

void qt_movie_set_path(qtm_movie *mv, const char *path)
{
    movie *m = movie_of_model(mv);
    if (!m)
        return;
    free(m->data_path);
    m->data_path = path ? strdup(path) : NULL;
}

/* Pictures and sound only with a person to see and hear them. */
static bool use_vlc(void) { return events_host_attached() || getenv("BLUEBOX_QUICKTIME_HEADLESS"); }

static int64_t duration_ms(const movie *m) { return m->mv->duration * 1000 / m->mv->scale; }

/* Where the movie plays from and to: its active segment (the preview, in
 * preview mode), else all of it. */
static int64_t start_ms(const movie *m)
{
    return m->mv->active_start >= 0 ? (int64_t)m->mv->active_start * 1000 / m->mv->scale : 0;
}

static int64_t end_ms(const movie *m)
{
    if (m->mv->active_start < 0)
        return duration_ms(m);
    int64_t e = ((int64_t)m->mv->active_start + m->mv->active_duration) * 1000 / m->mv->scale;
    return e < duration_ms(m) ? e : duration_ms(m);
}

/* Whether any enabled track has this media type (or there are no tracks of
 * it to go by). */
static bool plays(const movie *m, const char *type)
{
    bool any = false;
    for (int k = 0; k < m->mv->ntracks; k++) {
        const qtm_track *t = m->mv->tracks[k];
        if (t->media && t->media->type == fourcc(type)) {
            any = true;
            if (t->flags & 1)
                return true;
        }
    }
    return !any;
}

/* VLC's volume (0-100) for the movie's: silent with its sound disabled. */
static int vlc_volume(const movie *m)
{
    return m->volume < 0 || !plays(m, "soun") ? 0 : m->volume * 100 / 256;
}

/* The movie's natural size: as far as its visual tracks reach, through
 * their matrices. */
static qrect natural_rect(const movie *m)
{
    int w = 0, h = 0;
    /* Disabled tracks take no space (a VR movie's tiles) when enabled ones show. */
    bool any_enabled = false;
    for (int k = 0; k < m->mv->ntracks; k++)
        any_enabled |= m->mv->tracks[k]->width && m->mv->tracks[k]->height && (m->mv->tracks[k]->flags & 1);
    for (int k = 0; k < m->mv->ntracks; k++) {
        const qtm_track *t = m->mv->tracks[k];
        if (!t->width || !t->height || (any_enabled && !(t->flags & 1)))
            continue;
        if (t->media && (t->media->type == fourcc("soun") || t->media->type == fourcc("musi")))
            continue;
        int16_t top, left, bottom, right;
        qtm_track_bounds(t, &top, &left, &bottom, &right);
        if (right > w)
            w = right;
        if (bottom > h)
            h = bottom;
    }
    return (qrect){ 0, 0, (int16_t)h, (int16_t)w };
}

/* A video track whose first sample description is palettized (1 to 8 bits)
 * without a colour table of its own: QuickTime copied such pixels to a
 * screen of that depth as they are, so they take the screen's colours. */
static void find_index_depth(movie *m)
{
    m->index_depth = 0;
    for (int k = 0; k < m->mv->ntracks; k++) {
        const qtm_media *md = m->mv->tracks[k]->media;
        if (!md || md->type != fourcc("vide") || !md->ndescs || md->desc_len[0] < 86)
            continue;
        const uint8_t *e = md->descs[0];
        int16_t depth = (int16_t)(e[82] << 8 | e[83]), ctab = (int16_t)(e[84] << 8 | e[85]);
        if ((depth == 1 || depth == 2 || depth == 4 || depth == 8) && ctab != 0)
            m->index_depth = depth;
        return;
    }
}

/* ---- Atoms ---- */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* The child atom of a type inside [p, end), or NULL; *len gets its size. */
static uint8_t *find_atom(uint8_t *p, uint8_t *end, const char *type, uint32_t *len)
{
    while (p + 8 <= end) {
        uint32_t n = be32(p);
        if (n < 8 || p + n > end)
            return NULL;
        if (!memcmp(p + 4, type, 4)) {
            *len = n;
            return p;
        }
        p += n;
    }
    return NULL;
}

/* Move every chunk offset (stco, co64) by delta and make data references
 * point at the file they are in, for a flattened copy. */
static void relocate(uint8_t *p, uint8_t *end, uint32_t delta)
{
    while (p + 8 <= end) {
        uint32_t n = be32(p);
        if (n < 8 || p + n > end)
            return;
        if (!memcmp(p + 4, "trak", 4) || !memcmp(p + 4, "mdia", 4) || !memcmp(p + 4, "minf", 4) ||
            !memcmp(p + 4, "stbl", 4) || !memcmp(p + 4, "dinf", 4)) {
            relocate(p + 8, p + n, delta);
        } else if (!memcmp(p + 4, "stco", 4) && n >= 16) {
            uint32_t count = be32(p + 12);
            for (uint32_t i = 0; i < count && 16 + i * 4 + 4 <= n; i++)
                put_be32(p + 16 + i * 4, be32(p + 16 + i * 4) + delta);
        } else if (!memcmp(p + 4, "co64", 4) && n >= 16) {
            uint32_t count = be32(p + 12);
            for (uint32_t i = 0; i < count && 16 + i * 8 + 8 <= n; i++) {
                uint8_t *q = p + 16 + i * 8;
                uint64_t v = ((uint64_t)be32(q) << 32 | be32(q + 4)) + delta;
                put_be32(q, (uint32_t)(v >> 32));
                put_be32(q + 4, (uint32_t)v);
            }
        } else if (!memcmp(p + 4, "dref", 4) && n >= 16) {
            uint32_t count = be32(p + 12);
            uint8_t *e = p + 16;
            for (uint32_t i = 0; i < count && e + 12 <= p + n; i++) {
                uint32_t en = be32(e);
                if (en < 12)
                    break;
                e[11] |= 1; /* self-reference: the data is in this file */
                e += en;
            }
        }
        p += n;
    }
}

/* The movie's file (data fork) as a host path and its resource fork. */
static uint8_t *resource_fork(const char *path, size_t *len)
{
    mac_file mf;
    char err[256];
    if (!appledouble_read(path, &mf, err, sizeof err))
        return NULL;
    uint8_t *r = NULL;
    if (mf.rsrc_len) {
        r = malloc(mf.rsrc_len);
        memcpy(r, mf.rsrc, mf.rsrc_len);
        *len = mf.rsrc_len;
    }
    mac_file_free(&mf);
    return r;
}

/* A 'moov' atom from the resource fork (the resource with this ID, or the
 * first), copied; NULL if there is none. */
static uint8_t *moov_resource(const char *path, int16_t *id, uint32_t *len)
{
    size_t rl = 0;
    uint8_t *fork = resource_fork(path, &rl);
    if (!fork)
        return NULL;
    rsrc_index idx;
    char err[256];
    uint8_t *out = NULL;
    if (rsrc_parse(fork, rl, &idx, err, sizeof err)) {
        for (size_t i = 0; i < idx.count && !out; i++) {
            rsrc_entry *e = &idx.entries[i];
            if (e->type != fourcc("moov") || (*id && e->id != *id))
                continue;
            const uint8_t *d = rsrc_data(fork, &idx, e);
            /* The resource is the atom's contents or the whole atom. */
            if (e->data_len >= 8 && !memcmp(d + 4, "moov", 4) && be32(d) == e->data_len) {
                out = malloc(e->data_len);
                memcpy(out, d, e->data_len);
                *len = e->data_len;
            } else {
                *len = e->data_len + 8;
                out = malloc(*len);
                put_be32(out, *len);
                memcpy(out + 4, "moov", 4);
                memcpy(out + 8, d, e->data_len);
            }
            *id = e->id;
        }
        rsrc_index_free(&idx);
    }
    free(fork);
    return out;
}

/* A 'moov' atom among the data fork's top-level atoms from offset on. */
static uint8_t *moov_in_data(const char *path, uint64_t offset, uint32_t *len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    uint8_t *out = NULL;
    uint8_t h[16];
    uint64_t at = offset;
    while (!out && fseeko(fp, (off_t)at, SEEK_SET) == 0 && fread(h, 1, 8, fp) == 8) {
        uint64_t n = be32(h);
        if (n == 1) { /* 64-bit size */
            if (fread(h + 8, 1, 8, fp) != 8)
                break;
            n = (uint64_t)be32(h + 8) << 32 | be32(h + 12);
        }
        if (n < 8)
            break;
        if (!memcmp(h + 4, "moov", 4) && n < 64u << 20) {
            out = malloc((size_t)n);
            if (fseeko(fp, (off_t)at, SEEK_SET) != 0 || fread(out, 1, (size_t)n, fp) != n) {
                free(out);
                out = NULL;
                break;
            }
            *len = (uint32_t)n;
        }
        at += n;
    }
    fclose(fp);
    return out;
}

/* A compressed header (QuickTime 3's 'cmov': 'dcom' naming the method,
 * 'cmvd' the uncompressed size and the data) as the 'moov' atom it holds.
 * Takes moov; NULL if it can't be read. */
static uint8_t *uncompress_moov(uint8_t *moov, uint32_t *len)
{
    uint32_t n, dn, cn;
    uint8_t *cmov = find_atom(moov + 8, moov + *len, "cmov", &n);
    if (!cmov)
        return moov;
    uint8_t *dcom = find_atom(cmov + 8, cmov + n, "dcom", &dn), *cmvd = find_atom(cmov + 8, cmov + n, "cmvd", &cn);
    uint8_t *out = NULL;
    if (dcom && dn >= 12 && !memcmp(dcom + 8, "zlib", 4) && cmvd && cn >= 12) {
        uLongf size = be32(cmvd + 8);
        if (size >= 8 && size < 64u << 20 && (out = malloc(size)) &&
            (uncompress(out, &size, cmvd + 12, cn - 12) != Z_OK || size < 8 || memcmp(out + 4, "moov", 4) ||
             be32(out) > size)) {
            free(out);
            out = NULL;
        }
        if (out)
            *len = be32(out);
    }
    free(moov);
    return out;
}

uint8_t *qt_moov_uncompress(uint8_t *moov, uint32_t *len) { return uncompress_moov(moov, len); }

static int temp_count;

/* Write a flattened copy for VLC: 'mdat' header, the data fork, the atom. */
static char *flatten(const char *data_path, uint8_t *moov, uint32_t len)
{
    const char *tmp = getenv("TMPDIR");
    char *path = malloc(1024);
    snprintf(path, 1024, "%s/bluebox-movie-%d-%d.mov", tmp && *tmp ? tmp : "/tmp", (int)getpid(), ++temp_count);
    FILE *in = fopen(data_path, "rb"), *out = fopen(path, "wb");
    bool ok = in && out;
    struct stat st;
    uint64_t dl = ok && stat(data_path, &st) == 0 ? (uint64_t)st.st_size : 0;
    if (ok && dl + 8 > 0xFFFFFFFFu)
        ok = false;
    if (ok) {
        uint8_t hdr[8];
        put_be32(hdr, (uint32_t)(dl + 8));
        memcpy(hdr + 4, "mdat", 4);
        ok = fwrite(hdr, 1, 8, out) == 8;
        uint8_t buf[65536];
        size_t n;
        while (ok && (n = fread(buf, 1, sizeof buf, in)) > 0)
            ok = fwrite(buf, 1, n, out) == n;
        relocate(moov + 8, moov + len, 8);
        ok = ok && fwrite(moov, 1, len, out) == len;
    }
    if (in)
        fclose(in);
    if (out)
        fclose(out);
    if (!ok) {
        unlink(path);
        free(path);
        return NULL;
    }
    return path;
}

/* Flattened copies are removed when the run ends, however it ends. */
static void remove_temp_files(void)
{
    for (int i = 0; i < MAX_MOVIES; i++)
        if (movies[i].used && movies[i].temp_file && movies[i].play_path)
            unlink(movies[i].play_path);
}

uint32_t mac_now(void);

/* A movie for a description (taken), from a file or not. */
static movie *new_movie(qtm_movie *mv, const char *path)
{
    static bool registered;
    if (!registered) {
        atexit(remove_temp_files);
        registered = true;
    }
    for (int i = 0; i < MAX_MOVIES; i++) {
        if (movies[i].used)
            continue;
        movie *m = &movies[i];
        memset(m, 0, sizeof *m);
        m->used = true;
        m->mv = mv ? mv : qtm_new(600, mac_now());
        m->data_path = path ? strdup(path) : NULL;
        m->handle = tb_NewHandle(4, false, true);
        wr32(rd32(m->handle), 0x6D6F6F76); /* 'moov', for anyone looking */
        m->rate = 0x00010000;
        m->volume = m->mv->pref_volume;
        m->port = qd_port();
        m->gdh = tb_GetGDevice();
        m->timebase = tb_NewHandle(4, false, true);
        wr32(rd32(m->timebase), 0x74626173); /* 'tbas' */
        m->start_ns = (int64_t)machine_time_ns();
        m->box = natural_rect(m);
        m->stale = true;
        find_index_depth(m);
        return m;
    }
    qtm_free(mv);
    return NULL;
}

static void stop_sound(movie *m);
static void release_slaves(gaddr tb);

static void close_sources(movie *m)
{
    for (int i = 0; i < m->nsrcs; i++)
        if (m->srcs[i].ok)
            qt_src_close(&m->srcs[i].src);
    m->nsrcs = 0;
}

static void free_movie(movie *m)
{
    qt_vr_movie_gone(m->handle);
    qt_callbacks_timebase_gone(m->timebase);
    stop_sound(m);
    release_slaves(m->handle);
    release_slaves(m->timebase);
    if (m->timebase)
        tb_DisposeHandle(m->timebase);
    if (m->clip)
        tb_DisposeRgn(m->clip);
    if (m->display_clip)
        tb_DisposeRgn(m->display_clip);
    qt_native_close(m->native);
    close_sources(m);
    qt_movie_forget(m->mv);
    qtm_free(m->mv);
    free(m->moov);
    free(m->data_path);
    if (m->player)
        qt_vlc_close(m->player);
    if (m->temp_file && m->play_path)
        unlink(m->play_path);
    free(m->play_path);
    free(m->frame);
    if (m->handle)
        tb_DisposeHandle(m->handle);
    m->used = false;
}

/* ---- Playing from the description ---- */

/* A sample's bytes for our decoders, from a description with virtual
 * offsets: the track and data reference say where. */
static bool read_virtual(void *ctx, uint64_t offset, void *buf, uint32_t n)
{
    movie *m = ctx;
    uint64_t key = offset >> 40;
    int ti = (int)(key >> 8), di = (int)(key & 0xFF);
    uint64_t off = offset & ((1ull << 40) - 1);
    int s = 0;
    while (s < m->nsrcs && m->srcs[s].key != key)
        s++;
    if (s == m->nsrcs) {
        if (m->nsrcs == MAX_SOURCES)
            close_sources(m), s = 0;
        m->nsrcs++;
        m->srcs[s].key = key;
        m->srcs[s].ok = false;
        const qtm_media *md = ti >= 1 && ti <= m->mv->ntracks ? m->mv->tracks[ti - 1]->media : NULL;
        if (md && di >= 1 && di <= md->ndrefs) {
            const qtm_dref *d = &md->drefs[di - 1];
            m->srcs[s].ok = qt_src_open(d->type, d->flags, d->data, d->len, m->data_path, false, &m->srcs[s].src);
        }
    }
    return m->srcs[s].ok && qt_src_read(&m->srcs[s].src, off, buf, n);
}

/* Whether every sample is in the movie's own file (so its description can
 * be played, and flattened for VLC, as it is). */
static bool all_in_own_file(const movie *m)
{
    if (!m->data_path)
        return false;
    for (int k = 0; k < m->mv->ntracks; k++) {
        const qtm_media *md = m->mv->tracks[k]->media;
        if (!md || !md->nruns)
            continue;
        for (int i = 0; i < md->ndrefs; i++) {
            const qtm_dref *d = &md->drefs[i];
            char path[PATH_MAX];
            if (d->flags & 1)
                continue;
            if (!qt_dref_path(d->type, d->data, d->len, m->data_path, path, sizeof path) || strcmp(path, m->data_path))
                return false;
        }
    }
    return true;
}

/* Our decoders for the movie's description (none: VLC, or timing alone).
 * Palettized video without colours of its own takes the screen's. */
static void open_native(movie *m)
{
    qt_native_close(m->native);
    m->native = NULL;
    close_sources(m);
    free(m->moov);
    m->virtual_play = !all_in_own_file(m);
    m->moov = qtm_serialize(m->mv, m->virtual_play, &m->moov_len);
    m->stale = false;
    find_index_depth(m);
    uint32_t screen[256];
    pixdst d = qd_port_pixels(m->port);
    int sd = d.depth <= 8 ? d.depth : 0;
    for (int v = 0; sd && v < (1 << sd); v++) {
        uint16_t c[3];
        qd_px_rgb(sd, d.ctab, (uint32_t)v, c);
        screen[v] = (uint32_t)(c[0] >> 8) << 16 | (uint32_t)(c[1] >> 8) << 8 | (c[2] >> 8);
    }
    if (m->virtual_play)
        m->native = qt_native_open_reader(m->moov, m->moov_len, read_virtual, m, m->mv->scale, sd ? screen : NULL, sd);
    else
        m->native = qt_native_open(m->moov, m->moov_len, m->data_path, m->mv->scale, sd ? screen : NULL, sd);
}

/* VLC's copy of a movie our decoders don't play: the file itself when it
 * is flattened, else a flattened copy. */
static void prepare_vlc(movie *m, bool flattened)
{
    if (m->native || m->virtual_play || !m->data_path)
        return;
    if (flattened) {
        m->play_path = strdup(m->data_path);
    } else { /* (flatten moves the copy's chunk offsets) */
        uint8_t *copy = malloc(m->moov_len);
        if (copy) {
            memcpy(copy, m->moov, m->moov_len);
            m->play_path = flatten(m->data_path, copy, m->moov_len);
            free(copy);
        }
        m->temp_file = m->play_path != NULL;
    }
}

/* The description changed: played from it again. */
static void refresh(movie *m)
{
    if (!m->stale)
        return;
    qtm_update_duration(m->mv);
    stop_sound(m);
    open_native(m);
    m->serial = 0;
    if (m->base_ms > duration_ms(m))
        m->base_ms = duration_ms(m);
}

void qt_movie_touch(qtm_movie *mv)
{
    movie *m = movie_of_model(mv);
    if (!m)
        return;
    qtm_update_duration(mv);
    m->stale = true;
    m->changed = true;
}

/* A movie from a description (taken) and the file it came from. */
static int16_t open_movie(const char *path, int16_t *resid, uint64_t data_offset, bool from_data, gaddr moviep);

static int16_t take_description(uint8_t *moov, uint32_t len, const char *path, bool flattened, gaddr moviep)
{
    moov = uncompress_moov(moov, &len);
    if (!moov)
        return set_error(invalidMovie);
    /* a reference movie (QuickTime 4's shortcuts and alternates): the
     * movie its first reference names */
    uint32_t rtype, rn;
    uint8_t *ref;
    static int depth;
    if (depth < 4 && qt_reference_movie_target(moov, len, &rtype, &ref, &rn)) {
        char target[PATH_MAX];
        free(moov);
        bool ok = qt_dref_path(rtype, ref, rn, path, target, sizeof target);
        free(ref);
        if (!ok)
            return set_error(couldNotResolveDataRef);
        depth++;
        int16_t err = open_movie(target, NULL, 0, false, moviep);
        depth--;
        return err;
    }
    qtm_movie *mv = qtm_parse(moov, len);
    free(moov);
    if (!mv)
        return set_error(invalidMovie);
    movie *m = new_movie(mv, path);
    if (!m)
        return set_error(memFullErr);
    open_native(m);
    LOGF(LOG_TRAPS, "  movie %s: %s\n", path ? path : "(memory)", m->native ? "our decoders" : "VLC");
    prepare_vlc(m, flattened);
    wr32(moviep, m->handle);
    return set_error(noErr);
}

/* A movie from a host file: the description from the resource fork (resid,
 * 0 for the first) or the data fork at offset; flattened for playing. */
static int16_t open_movie(const char *path, int16_t *resid, uint64_t data_offset, bool from_data, gaddr moviep)
{
    wr32(moviep, 0);
    uint32_t len = 0;
    int16_t id = resid ? *resid : 0;
    uint8_t *moov = NULL;
    bool flattened = false;
    if (!from_data)
        moov = moov_resource(path, &id, &len);
    if (!moov) {
        moov = moov_in_data(path, data_offset, &len);
        flattened = moov != NULL;
    }
    if (!moov)
        return set_error(noMovieFound);
    int16_t err = take_description(moov, len, path, flattened, moviep);
    if (!err && resid)
        *resid = id;
    return err;
}

int16_t qt_open_movie_path(const char *path, int16_t *resid, uint64_t offset, bool from_data, gaddr moviep, int16_t flags)
{
    int16_t err = open_movie(path, resid, offset, from_data, moviep);
    if (!err && (flags & newMovieActive))
        movie_of(rd32(moviep))->active = true;
    return err;
}

int16_t qt_movie_from_bytes(const uint8_t *bytes, uint32_t len, const char *path, gaddr moviep, int16_t flags)
{
    wr32(moviep, 0);
    uint8_t *buf = malloc(len + 8);
    if (len >= 8 && !memcmp(bytes + 4, "moov", 4)) {
        memcpy(buf, bytes, len);
    } else {
        put_be32(buf, len + 8);
        memcpy(buf + 4, "moov", 4);
        memcpy(buf + 8, bytes, len);
        len += 8;
    }
    int16_t err = take_description(buf, len, path, false, moviep);
    if (!err && (flags & newMovieActive))
        movie_of(rd32(moviep))->active = true;
    return err;
}

gaddr qt_new_movie_for(qtm_movie *mv, const char *path)
{
    movie *m = new_movie(mv, path);
    return m ? m->handle : 0;
}

/* ---- Time ---- */

/* A movie keeps time from a source: the machine's clock, or the time base
 * it is slaved to (SetMovieMasterTimeBase), so that it follows that one's
 * time and rate.  Masters can chain; a loop of them ends at the clock. */
#define MAX_MASTERS 8

static int64_t timebase_ns(gaddr tb, int depth);
static int32_t timebase_effective_rate(gaddr tb, int depth);

static int64_t source_ns(const movie *m, int depth)
{
    if (m->master && depth < MAX_MASTERS)
        return timebase_ns(m->master, depth + 1);
    return (int64_t)machine_time_ns();
}

static int64_t movie_ms_at(movie *m, int depth)
{
    int64_t t;
    if (m->player && m->playing && !m->master)
        t = qt_vlc_time(m->player);
    else if (m->playing)
        t = m->base_ms + (int64_t)((__int128)(source_ns(m, depth) - m->start_ns) * m->rate / 0x10000 / 1000000);
    else
        t = m->base_ms;
    int64_t d = end_ms(m), s0 = start_ms(m);
    return t > d ? d : t < s0 ? s0 : t;
}

static int64_t movie_ms(movie *m) { return movie_ms_at(m, 0); }

/* The rate the movie's time really moves at: its own times its master's. */
static int32_t movie_effective_rate_at(movie *m, int depth)
{
    if (!m->playing)
        return 0;
    if (!m->master || depth >= MAX_MASTERS)
        return m->rate;
    return (int32_t)((int64_t)m->rate * timebase_effective_rate(m->master, depth + 1) / 0x10000);
}

static int32_t movie_effective_rate(movie *m) { return movie_effective_rate_at(m, 0); }

static bool movie_done(movie *m)
{
    if (!m->playing)
        return movie_ms(m) >= end_ms(m);
    if (m->player && !m->master && m->mv->active_start < 0)
        return qt_vlc_ended(m->player);
    return movie_ms(m) >= end_ms(m);
}

/* For qt_movies.c: the movie's time now, in its time scale. */
int64_t qt_movie_now(const qtm_movie *mv)
{
    movie *m = movie_of_model(mv);
    return m ? movie_ms(m) * mv->scale / 1000 : 0;
}

/* ---- Native sound ---- */

static int sound_id(const movie *m) { return 0x4D560000 + (int)(m - movies); }

static void stop_sound(movie *m)
{
    if (m->sound_on)
        sound_host_stop(sound_id(m));
    m->sound_on = false;
    if (m->music_on)
        sound_host_midi(sound_id(m), NULL, 0); /* all notes off */
    m->music_on = false;
}

/* A music track's MIDI, sent a quarter second ahead (normal speed only).
 * Notes the movie's volume makes quieter go out softer. */
static void feed_music(movie *m)
{
    if (!m->native || !qt_native_has_music(m->native) || movie_effective_rate(m) != 0x10000 || m->volume <= 0)
        return;
    int64_t now = movie_ms(m);
    if (!m->music_on || m->music_ms < now - 100 || m->music_ms > now + 1000) { /* starting, behind, or rewound */
        if (m->music_on)
            sound_host_midi(sound_id(m), NULL, 0);
        m->music_ms = now;
        m->music_on = true;
        /* each part's instrument, from the start of the tune */
        const qt_midi_event *e;
        size_t n = qt_native_music(m->native, INT64_MIN, now, &e);
        machine_midi_event setup[64];
        int k = 0;
        for (size_t i = 0; i < n && k < 64; i++)
            if ((e[i].status & 0xF0) == 0xC0 || (e[i].status & 0xF0) == 0xB0)
                setup[k++] = (machine_midi_event){ 0, e[i].status, e[i].data1, e[i].data2 };
        if (k)
            sound_host_midi(sound_id(m), setup, k);
    }
    int64_t to = now + 250;
    const qt_midi_event *e;
    size_t n = qt_native_music(m->native, m->music_ms, to, &e);
    if (n) {
        machine_midi_event *out = malloc(n * sizeof *out);
        for (size_t i = 0; i < n; i++) {
            uint8_t d2 = e[i].data2;
            if ((e[i].status & 0xF0) == 0x90 && m->volume < 0x100)
                d2 = (uint8_t)(d2 * m->volume / 0x100);
            out[i] = (machine_midi_event){ (int32_t)(e[i].time - now), e[i].status, e[i].data1, d2 };
        }
        sound_host_midi(sound_id(m), out, (int)n);
        free(out);
    }
    m->music_ms = to;
}

/* Keep a second of sound queued ahead of the clock (normal speed only). */
static void feed_sound(movie *m)
{
    if (!m->native || !qt_native_has_sound(m->native) || movie_effective_rate(m) != 0x10000 || !plays(m, "soun") ||
        m->volume <= 0)
        return;
    int64_t now = movie_ms(m), end = duration_ms(m);
    if (!m->sound_on || m->sound_ms < now || m->sound_ms > now + 1500) { /* starting, behind, or rewound */
        stop_sound(m);
        m->sound_ms = now;
    }
    while (m->sound_ms < now + 1000 && m->sound_ms < end) {
        int64_t to = m->sound_ms + 500 < end ? m->sound_ms + 500 : end;
        int16_t *pcm = NULL;
        int nch = 1;
        double rate = 22050;
        uint32_t frames = qt_native_sound(m->native, m->sound_ms, to, &pcm, &nch, &rate);
        if (frames)
            sound_host_pcm(sound_id(m), pcm, (int)frames, nch, rate, (float)m->volume / 256.0f);
        free(pcm);
        m->sound_on = true;
        m->sound_ms = to;
    }
}

static void ensure_player(movie *m)
{
    refresh(m);
    if (!m->player && !m->native && m->play_path && use_vlc()) {
        m->player = qt_vlc_open(m->play_path, m->index_depth != 0);
        if (m->player)
            qt_vlc_set_volume(m->player, vlc_volume(m));
    }
}

static void start(movie *m, int32_t rate)
{
    if (rate == 0) {
        if (m->playing) {
            m->base_ms = movie_ms(m);
            m->playing = false;
            if (m->player)
                qt_vlc_pause(m->player);
            stop_sound(m);
        }
        return;
    }
    if (rate != m->rate)
        stop_sound(m);
    if (m->playing)
        m->base_ms = movie_ms(m); /* a new rate from where it is */
    m->rate = rate;
    if (movie_done(m) && !m->playing && !m->master)
        m->base_ms = start_ms(m); /* playing a finished movie starts it again */
    ensure_player(m);
    if (m->player && m->master) {
        /* VLC keeps its own clock: put it where the master says; task keeps
         * it there. */
        qt_vlc_play(m->player);
        qt_vlc_seek(m->player, m->base_ms);
        m->vlc_held = false;
    } else if (m->player) {
        if (m->base_ms == 0)
            qt_vlc_rewind(m->player);
        qt_vlc_play(m->player);
        if (m->base_ms)
            qt_vlc_seek(m->player, m->base_ms);
        if (rate != 0x10000)
            qt_vlc_set_rate(m->player, (float)rate / 65536.0f);
    }
    m->start_ns = source_ns(m, 0);
    bool was = m->playing;
    m->playing = true;
    feed_sound(m);
    feed_music(m);
    if (!was)
        qt_task_needed_sooner();
}

static void seek(movie *m, int64_t ms)
{
    stop_sound(m);
    m->base_ms = ms;
    m->start_ns = source_ns(m, 0);
    if (m->player) {
        if (ms == 0 && !m->playing)
            qt_vlc_rewind(m->player);
        else
            qt_vlc_seek(m->player, ms);
    }
    qt_callbacks_jumped(m->timebase);
}

/* ---- Pictures ---- */

static void draw_frame(movie *m, bool force)
{
    if (m->active && qt_vr_movie_draw(m->handle, m->port, m->box, force)) /* a QuickTime VR movie: its view */
        return;
    refresh(m);
    bool fresh;
    if (m->native) {
        if (!qt_native_has_video(m->native))
            return;
        fresh = qt_native_frame(m->native, movie_ms(m), &m->serial, &m->frame, &m->fw, &m->fh);
        m->frame_indexed = false;
    } else if (m->player) {
        fresh = qt_vlc_frame(m->player, &m->serial, &m->frame, &m->fw, &m->fh, &m->frame_indexed);
    } else {
        return;
    }
    if ((!fresh && !force) || !m->frame || !m->active || !plays(m, "vide"))
        return;
    gaddr saved = qd_port();
    tb_SetPort(m->port);
    /* The movie's clip (in its own coordinates, moved to its box) and its
     * display clip (the port's) narrow the port's for the frame. */
    gaddr port_clip = 0;
    if (m->clip || m->display_clip) {
        port_clip = tb_NewRgn();
        tb_GetClip(port_clip);
        gaddr clip = tb_NewRgn();
        tb_CopyRgn(port_clip, clip);
        if (m->clip) {
            qrect natural = natural_rect(m);
            gaddr r = tb_NewRgn(), from = tb_NewPtr(8, false, false), to = tb_NewPtr(8, false, false);
            tb_CopyRgn(m->clip, r);
            qrect_put(from, natural);
            qrect_put(to, m->box);
            if (natural.right > natural.left && natural.bottom > natural.top)
                tb_MapRgn(r, from, to);
            tb_SectRgn(clip, r, clip);
            tb_DisposeRgn(r);
            tb_DisposePtr(from);
            tb_DisposePtr(to);
        }
        if (m->display_clip)
            tb_SectRgn(clip, m->display_clip, clip);
        tb_SetClip(clip);
        tb_DisposeRgn(clip);
    }
    if (m->frame_indexed) {
        /* The port's colours at the movie's depth, else the standard ones. */
        pixdst d = qd_port_pixels(m->port);
        gaddr ct = d.depth == m->index_depth ? d.ctab : 0;
        size_t n = (size_t)m->fw * m->fh;
        uint8_t *rgba = malloc(n * 4);
        uint8_t map[256][3];
        for (int v = 0; v < 256; v++) {
            uint16_t c[3];
            qd_px_rgb(m->index_depth, ct, (uint32_t)v & ((1u << m->index_depth) - 1), c);
            for (int k = 0; k < 3; k++)
                map[v][k] = (uint8_t)(c[k] >> 8);
        }
        for (size_t i = 0; i < n; i++) {
            memcpy(rgba + i * 4, map[m->frame[i]], 3);
            rgba[i * 4 + 3] = 255;
        }
        qd_draw_rgba(rgba, m->fw, m->fh, m->box);
        free(rgba);
    } else {
        qd_draw_rgba(m->frame, m->fw, m->fh, m->box);
    }
    if (port_clip) {
        tb_SetClip(port_clip);
        tb_DisposeRgn(port_clip);
    }
    tb_SetPort(saved);
    qt_movie_drawing_complete(m->mv);
}

/* For GetMoviePict and the like: the movie's picture at a time, RGBA. */
bool qt_movie_frame_at(const qtm_movie *mv, int64_t time, uint8_t **rgba, int *w, int *h)
{
    movie *m = movie_of_model(mv);
    if (!m)
        return false;
    refresh(m);
    if (!m->native || !qt_native_has_video(m->native))
        return false;
    uint32_t serial = 0;
    return qt_native_frame(m->native, time * 1000 / mv->scale, &serial, rgba, w, h) && *rgba;
}

/* For the media handlers: what plays the movie now (our decoders, or
 * none), a track's place in it, and the movie drawn again. */
qt_native *qt_movie_native(const qtm_movie *mv)
{
    movie *m = movie_of_model(mv);
    if (!m)
        return NULL;
    refresh(m);
    return m->native;
}

int qt_track_index(const qtm_movie *mv, const qtm_track *t)
{
    for (int i = 0; i < mv->ntracks; i++)
        if (mv->tracks[i] == t)
            return i;
    return -1;
}

void qt_movie_redraw(const qtm_movie *mv)
{
    movie *m = movie_of_model(mv);
    if (m)
        draw_frame(m, true);
}

/* The movie's port, GWorld device and box, for drawing beside it. */
void qt_movie_place(const qtm_movie *mv, gaddr *port, qrect *box)
{
    movie *m = movie_of_model(mv);
    *port = m ? m->port : 0;
    *box = m ? m->box : (qrect){ 0, 0, 0, 0 };
}

/* A slaved movie that VLC plays: VLC keeps its own clock, so it is held
 * while the master stands still and put back where the master says when
 * it strays. */
static void follow_master(movie *m)
{
    if (!m->player || !m->master || !m->playing)
        return;
    if (movie_effective_rate(m) == 0) {
        if (!m->vlc_held)
            qt_vlc_pause(m->player);
        m->vlc_held = true;
        return;
    }
    int64_t want = movie_ms(m);
    if (m->vlc_held) {
        qt_vlc_play(m->player);
        qt_vlc_seek(m->player, want);
        m->vlc_held = false;
    } else if (llabs(qt_vlc_time(m->player) - want) > 250) {
        qt_vlc_seek(m->player, want);
    }
}

static void task(movie *m)
{
    qt_vr_movie_idle(m->handle);
    if (!m->playing)
        return;
    follow_master(m);
    draw_frame(m, false);
    /* Sound plays a second ahead at normal speed: a master that stops or
     * changes speed leaves it behind. */
    if (movie_effective_rate(m) != 0x10000)
        stop_sound(m);
    feed_sound(m);
    feed_music(m);
    /* A slaved movie goes where its master takes it, back again included. */
    if (!m->master && movie_done(m)) {
        if (m->looping) {
            m->base_ms = start_ms(m);
            if (m->player)
                qt_vlc_rewind(m->player);
            start(m, m->rate);
        } else {
            m->base_ms = end_ms(m);
            m->playing = false;
        }
    }
}

/* ---------------------------------------------------------------------- */
/* The Movie Toolbox calls */

int16_t tb_EnterMovies(void) { return set_error(noErr); }

void tb_ExitMovies(void)
{
    for (int i = 0; i < MAX_MOVIES; i++)
        if (movies[i].used)
            free_movie(&movies[i]);
}

int16_t tb_GetMoviesError(void) { return movies_error; }
int16_t tb_GetMoviesStickyError(void) { return sticky_error; }
void tb_ClearMoviesStickyError(void) { sticky_error = 0; }

int16_t tb_OpenMovieFile(gaddr spec, gaddr refnum, uint8_t perm)
{
    int16_t err = tb_FSpOpenDF(spec, perm ? perm : 1, refnum);
    if (err != noErr)
        wr16(refnum, 0);
    return set_error(err);
}

int16_t tb_CloseMovieFile(int16_t refnum)
{
    char path[1024];
    if (!fm_refnum_host_path(refnum, path, sizeof path) && rm_host_path(refnum, path, sizeof path)) {
        /* CreateMovieFile's resource file */
        tb_CloseResFile(refnum);
        return set_error(tb_ResError());
    }
    gaddr pb = tb_NewPtr(IOParam_SIZEOF, false, true);
    wr16(pb + IOParam_ioRefNum_off, (uint16_t)refnum);
    int16_t err = tb_PBClose(pb, false);
    tb_DisposePtr(pb);
    return set_error(err);
}

int16_t tb_NewMovieFromFile(gaddr moviep, int16_t refnum, gaddr resid, gaddr resname, int16_t flags,
                            gaddr changed)
{
    char path[1024];
    if (changed)
        wr8(changed, 0);
    /* OpenMovieFile's reference number, or a resource file's (SimpleText
     * opens movies with FSpOpenResFile).  For a flattened movie, with no
     * resource fork to open, SimpleText passes -1; QuickTime takes it from
     * the file just opened (as a real Mac plays such movies). */
    if (!fm_refnum_host_path(refnum, path, sizeof path) && !rm_host_path(refnum, path, sizeof path) &&
        !(refnum == -1 && fm_last_data_fork(path, sizeof path))) {
        wr32(moviep, 0);
        return set_error(-51 /* rfNumErr */);
    }
    int16_t id = resid ? (int16_t)rd16(resid) : 0;
    /* QuickTime 3 opens files that aren't movies (pictures, sounds, MIDI)
     * through a movie importer, when one of ours takes the file */
    int16_t err = open_movie(path, &id, 0, false, moviep);
    if (err == noMovieFound && qt_import_file_as_movie(path, moviep, flags) == noErr)
        return set_error(noErr);
    if (!err && (flags & newMovieActive))
        movie_of(rd32(moviep))->active = true;
    if (!err && resid)
        wr16(resid, (uint16_t)id);
    if (!err && resname)
        wr8(resname, 0);
    return err;
}

int16_t tb_NewMovieFromDataFork(gaddr moviep, int16_t refnum, int32_t offset, int16_t flags, gaddr changed)
{
    char path[1024];
    if (changed)
        wr8(changed, 0);
    if (!fm_refnum_host_path(refnum, path, sizeof path)) {
        wr32(moviep, 0);
        return set_error(-51);
    }
    int16_t err = open_movie(path, NULL, (uint32_t)offset, true, moviep);
    if (!err && (flags & newMovieActive))
        movie_of(rd32(moviep))->active = true;
    return err;
}

int16_t tb_NewMovieFromHandle(gaddr moviep, int32_t h, int16_t flags, gaddr changed)
{
    /* The description alone: its data is wherever its data references say. */
    if (changed)
        wr8(changed, 0);
    wr32(moviep, 0);
    if (!h || !rd32((gaddr)h))
        return set_error(invalidMovie);
    uint32_t len = (uint32_t)tb_GetHandleSize((gaddr)h);
    uint8_t *buf = malloc(len ? len : 1);
    gget(buf, rd32((gaddr)h), len);
    int16_t err = qt_movie_from_bytes(buf, len, NULL, moviep, flags);
    free(buf);
    return err;
}

int32_t tb_NewMovie(int32_t flags)
{
    movie *m = new_movie(NULL, NULL);
    if (m && (flags & newMovieActive))
        m->active = true;
    set_error(m ? noErr : memFullErr);
    return m ? (int32_t)m->handle : 0;
}

void tb_DisposeMovie(gaddr h)
{
    movie *m = movie_of(h);
    if (m)
        free_movie(m);
}

void tb_MoviesTask(gaddr h, int32_t ms)
{
    qt_callbacks_poll();
    qt_mhlr_task();
    if (h) {
        movie *m = movie_of(h);
        if (m)
            task(m);
        return;
    }
    for (int i = 0; i < MAX_MOVIES; i++)
        if (movies[i].used)
            task(&movies[i]);
}

int16_t tb_PrerollMovie(gaddr h, int32_t time, int32_t rate)
{
    movie *m = movie_of(h);
    if (!m)
        return set_error(invalidMovie);
    ensure_player(m);
    return set_error(noErr);
}

void tb_SetMovieActive(gaddr h, int8_t active)
{
    movie *m = movie_of(h);
    if (m)
        m->active = active != 0;
}

/* ---- Editing ----
 *
 * Edits change each track's edit list (qt_edit.c), and the movie plays
 * from it again: the samples stay where they are.  Pasting takes another
 * movie's media only when it is in the same file (a movie and copies made
 * from it); saving writes the description as the file's 'moov' resource.
 */


/* Each track's edits, as qt_edit.c works on them. */
static bool read_edits(const movie *m, qt_edits *e)
{
    memset(e, 0, sizeof *e);
    e->scale = m->mv->scale;
    e->ntracks = m->mv->ntracks;
    e->tracks = calloc((size_t)(e->ntracks ? e->ntracks : 1), sizeof *e->tracks);
    for (int k = 0; k < e->ntracks; k++) {
        const qtm_track *t = m->mv->tracks[k];
        qt_edit_track *et = &e->tracks[k];
        et->movie_scale = m->mv->scale;
        et->media_scale = t->media ? t->media->scale : m->mv->scale;
        et->n = t->nedits;
        et->e = malloc((size_t)(t->nedits ? t->nedits : 1) * sizeof *et->e);
        for (int i = 0; i < t->nedits; i++)
            et->e[i] = (qt_edit){ t->edits[i].duration, t->edits[i].media_time, t->edits[i].rate };
    }
    return true;
}

/* The tracks' edits from qt_edit.c's, back in the description. */
static void apply_edits(movie *m, const qt_edits *e)
{
    for (int k = 0; k < e->ntracks && k < m->mv->ntracks; k++) {
        qtm_track *t = m->mv->tracks[k];
        const qt_edit_track *et = &e->tracks[k];
        qtm_edit *n = malloc((size_t)(et->n ? et->n : 1) * sizeof *n);
        for (int i = 0; i < et->n; i++)
            n[i] = (qtm_edit){ et->e[i].len, et->e[i].media_time, et->e[i].rate };
        qtm_set_edits(t, n, et->n);
        free(n);
    }
    qt_movie_touch(m->mv);
}

/* Keep the description before an edit, for undo. */
static uint8_t *undo_moov;
static uint32_t undo_len;
static gaddr undo_movie;

static void remember(movie *m)
{
    free(undo_moov);
    undo_moov = qtm_serialize(m->mv, false, &undo_len);
    undo_movie = m->handle;
}

static bool edited(movie *m, qt_edits *e)
{
    apply_edits(m, e);
    return true;
}

/* A copy of a movie's description: written out and read back. */
static qtm_movie *copy_model(const qtm_movie *mv)
{
    uint32_t n;
    uint8_t *b = qtm_serialize(mv, false, &n);
    qtm_movie *c = qtm_parse(b, n);
    free(b);
    return c;
}

qtm_movie *qt_copy_model(const qtm_movie *mv) { return copy_model(mv); }

/* A new movie of another's edits (the same file and tracks). */
static movie *movie_from_edits(movie *from, qt_edits *e)
{
    qtm_movie *mv = copy_model(from->mv);
    movie *m = mv ? new_movie(mv, from->data_path) : NULL;
    if (!m)
        return NULL;
    apply_edits(m, e);
    m->changed = false;
    m->box = natural_rect(m);
    open_native(m);
    return m;
}

void tb_GetMovieSelection(int32_t theMovie, gaddr selectionTime, gaddr selectionDuration)
{
    movie *m = movie_of((gaddr)theMovie);
    if (selectionTime)
        wr32(selectionTime, m ? (uint32_t)m->mv->sel_time : 0);
    if (selectionDuration)
        wr32(selectionDuration, m ? (uint32_t)m->mv->sel_duration : 0);
}

void tb_SetMovieSelection(int32_t theMovie, int32_t selectionTime, int32_t selectionDuration)
{
    movie *m = movie_of((gaddr)theMovie);
    if (!m)
        return;
    if (selectionDuration < 0) { /* a negative duration selects backwards */
        selectionTime += selectionDuration;
        selectionDuration = -selectionDuration;
    }
    int32_t d = (int32_t)m->mv->duration;
    m->mv->sel_time = selectionTime < 0 ? 0 : selectionTime > d ? d : selectionTime;
    m->mv->sel_duration = m->mv->sel_time + selectionDuration > d ? d - m->mv->sel_time : selectionDuration;
}

int32_t tb_CopyMovieSelection(int32_t theMovie)
{
    movie *m = movie_of((gaddr)theMovie);
    qt_edits e, part;
    if (!m || !read_edits(m, &e)) {
        set_error(invalidMovie);
        return 0;
    }
    bool ok = qt_edits_copy(&e, m->mv->sel_time, m->mv->sel_duration, &part);
    movie *c = ok ? movie_from_edits(m, &part) : NULL;
    qt_edits_free(&e);
    qt_edits_free(&part);
    set_error(c ? noErr : memFullErr);
    return c ? (int32_t)c->handle : 0;
}

static bool delete_segment(movie *m, int64_t from, int64_t len)
{
    qt_edits e;
    if (!read_edits(m, &e))
        return false;
    bool ok = qt_edits_delete(&e, from, len) && edited(m, &e);
    qt_edits_free(&e);
    return ok;
}

int32_t tb_CutMovieSelection(int32_t theMovie)
{
    movie *m = movie_of((gaddr)theMovie);
    int32_t copy = tb_CopyMovieSelection(theMovie);
    if (copy && m && m->mv->sel_duration > 0) {
        remember(m);
        delete_segment(m, m->mv->sel_time, m->mv->sel_duration);
        m->mv->sel_duration = 0;
    }
    return copy;
}

void tb_ClearMovieSelection(int32_t theMovie)
{
    movie *m = movie_of((gaddr)theMovie);
    if (!m || m->mv->sel_duration <= 0)
        return;
    remember(m);
    set_error(delete_segment(m, m->mv->sel_time, m->mv->sel_duration) ? noErr : invalidMovie);
    m->mv->sel_duration = 0;
}

static int16_t insert_segment(movie *src, movie *dst, int64_t src_in, int64_t len, int64_t dst_in)
{
    if (!src || !dst)
        return invalidMovie;
    if (!src->data_path || !dst->data_path || strcmp(src->data_path, dst->data_path) ||
        src->mv->ntracks != dst->mv->ntracks)
        return qt_insert_movie_segment(src->mv, dst->mv, src_in, len, dst_in);
    qt_edits s, d;
    if (!read_edits(src, &s))
        return invalidMovie;
    if (!read_edits(dst, &d)) {
        qt_edits_free(&s);
        return invalidMovie;
    }
    bool ok = qt_edits_insert(&d, &s, src_in, len, dst_in) && edited(dst, &d);
    qt_edits_free(&s);
    qt_edits_free(&d);
    return ok ? noErr : memFullErr;
}

void tb_PasteMovieSelection(int32_t theMovie, int32_t src)
{
    movie *m = movie_of((gaddr)theMovie), *s = movie_of((gaddr)src);
    if (!m || !s) {
        set_error(invalidMovie);
        return;
    }
    remember(m);
    if (m->mv->sel_duration > 0)
        delete_segment(m, m->mv->sel_time, m->mv->sel_duration);
    int16_t err = insert_segment(s, m, 0, s->mv->duration, m->mv->sel_time);
    if (!err)
        m->mv->sel_duration = (int32_t)(s->mv->duration * m->mv->scale / s->mv->scale);
    set_error(err);
}

int16_t tb_InsertMovieSegment(int32_t srcMovie, int32_t dstMovie, int32_t srcIn, int32_t srcDuration, int32_t dstIn)
{
    movie *d = movie_of((gaddr)dstMovie);
    if (d)
        remember(d);
    return set_error(insert_segment(movie_of((gaddr)srcMovie), d, srcIn, srcDuration, dstIn));
}

int16_t tb_InsertEmptyMovieSegment(int32_t dstMovie, int32_t dstIn, int32_t dstDuration)
{
    movie *m = movie_of((gaddr)dstMovie);
    qt_edits e;
    if (!m || !read_edits(m, &e))
        return set_error(invalidMovie);
    remember(m);
    bool ok = qt_edits_insert_empty(&e, dstIn, dstDuration) && edited(m, &e);
    qt_edits_free(&e);
    return set_error(ok ? noErr : memFullErr);
}

int16_t tb_DeleteMovieSegment(int32_t theMovie, int32_t startTime, int32_t duration)
{
    movie *m = movie_of((gaddr)theMovie);
    if (!m)
        return set_error(invalidMovie);
    if (startTime < 0 || startTime > m->mv->duration)
        return set_error(invalidTime);
    if (duration < 0)
        return set_error(invalidDuration);
    remember(m);
    return set_error(delete_segment(m, startTime, duration) ? noErr : invalidMovie);
}

/* The movie scrap: the last movie put there, kept here; the desk scrap
 * gets its description as 'moov'. */
static uint8_t *scrap_moov;
static uint32_t scrap_len;
static char *scrap_path;

int16_t tb_PutMovieOnScrap(int32_t theMovie, int32_t movieScrapFlags)
{
    movie *m = movie_of((gaddr)theMovie);
    if (!m)
        return set_error(invalidMovie);
    free(scrap_moov);
    free(scrap_path);
    scrap_moov = qtm_serialize(m->mv, false, &scrap_len);
    scrap_path = m->data_path ? strdup(m->data_path) : NULL;
    if (!(movieScrapFlags & 1)) /* movieScrapDontZeroScrap */
        tb_ZeroScrap();
    gaddr p = tb_NewPtr((int32_t)scrap_len, false, false);
    if (p) {
        gput(p, scrap_moov, scrap_len);
        tb_PutScrap((int32_t)scrap_len, 0x6D6F6F76, p);
        tb_DisposePtr(p);
    }
    return set_error(noErr);
}

int32_t tb_NewMovieFromScrap(int32_t newMovieFlags)
{
    if (!scrap_moov) {
        set_error(noMovieFound);
        return 0;
    }
    qtm_movie *mv = qtm_parse(scrap_moov, scrap_len);
    movie *m = mv ? new_movie(mv, scrap_path) : NULL;
    if (!m) {
        set_error(memFullErr);
        return 0;
    }
    m->active = (newMovieFlags & newMovieActive) != 0;
    open_native(m);
    set_error(noErr);
    return (int32_t)m->handle;
}

bool qt_scrap_has_movie(void) { return scrap_moov != NULL; }

bool qt_any_movie_playing(void)
{
    for (int i = 0; i < MAX_MOVIES; i++)
        if (movies[i].used && movies[i].playing)
            return true;
    return false;
}

int8_t tb_HasMovieChanged(int32_t h)
{
    movie *m = movie_of((gaddr)h);
    return m && m->changed;
}

void tb_ClearMovieChanged(int32_t h)
{
    movie *m = movie_of((gaddr)h);
    if (m)
        m->changed = false;
}

/* The description saved as the file's 'moov' resource (the whole atom). */
static int16_t save_moov(movie *m, int16_t resRefNum, int16_t id, gaddr name)
{
    if (!m)
        return set_error(invalidMovie);
    int16_t saved = tb_CurResFile();
    tb_UseResFile(resRefNum);
    if (tb_CurResFile() != resRefNum) {
        tb_UseResFile(saved);
        return set_error(-51 /* rfNumErr */);
    }
    qtm_update_duration(m->mv);
    m->mv->modified = mac_now();
    uint32_t len;
    uint8_t *moov = qtm_serialize(m->mv, false, &len);
    gaddr h = tb_Get1Resource(0x6D6F6F76, id);
    if (h) {
        tb_SetHandleSize(h, (int32_t)len);
        gput(rd32(h), moov, len);
        tb_ChangedResource(h);
    } else {
        h = tb_NewHandle((int32_t)len, false, false);
        if (h) {
            gput(rd32(h), moov, len);
            gaddr empty = tb_NewPtr(1, false, true);
            tb_AddResource(h, 0x6D6F6F76, id, name ? name : empty);
            tb_DisposePtr(empty);
        }
    }
    free(moov);
    int16_t err = tb_ResError();
    tb_UpdateResFile(resRefNum);
    tb_UseResFile(saved);
    if (!err)
        m->changed = false;
    return set_error(err);
}

int16_t tb_UpdateMovieResource(int32_t theMovie, int16_t resRefNum, int16_t resId, int32_t resName)
{
    return save_moov(movie_of((gaddr)theMovie), resRefNum, resId ? resId : 128, (gaddr)resName);
}

int16_t tb_AddMovieResource(int32_t theMovie, int16_t resRefNum, gaddr resId, int32_t resName)
{
    int16_t saved = tb_CurResFile();
    tb_UseResFile(resRefNum);
    int16_t id = resId && rd16(resId) ? (int16_t)rd16(resId) : tb_Unique1ID(0x6D6F6F76);
    tb_UseResFile(saved);
    if (resId)
        wr16(resId, (uint16_t)id);
    return save_moov(movie_of((gaddr)theMovie), resRefNum, id, (gaddr)resName);
}

/* Undo of the last edit (the controller's). */
static bool undo_edit(movie *m)
{
    if (!m || !undo_moov || undo_movie != m->handle)
        return false;
    qtm_movie *was = qtm_parse(undo_moov, undo_len);
    free(undo_moov);
    undo_moov = NULL;
    if (!was)
        return false;
    /* the tracks' edits go back; the rest stays as it is */
    for (int k = 0; k < was->ntracks && k < m->mv->ntracks; k++)
        qtm_set_edits(m->mv->tracks[k], was->tracks[k]->edits, was->tracks[k]->nedits);
    qtm_free(was);
    qt_movie_touch(m->mv);
    return true;
}

/* Private calls of QuickTime 2.5's MoviePlayer at startup, apparently
 * QuickTime's own preferences: 0x273 gives an atom container (none here),
 * 0x274 takes one back with a flag. */
int16_t tb_QTPrivateCall273(gaddr data)
{
    if (data)
        wr32(data, 0);
    return noErr;
}

int16_t tb_QTPrivateCall274(int32_t container, int8_t flag) { return noErr; }

int8_t tb_GetMovieActive(int32_t h)
{
    movie *m = movie_of((gaddr)h);
    return m && m->active;
}

void tb_StartMovie(gaddr h)
{
    movie *m = movie_of(h);
    if (!m)
        return;
    m->active = true;
    start(m, m->mv->pref_rate ? m->mv->pref_rate : 0x10000);
}

void tb_StopMovie(gaddr h)
{
    movie *m = movie_of(h);
    if (m)
        start(m, 0);
}

void tb_GoToBeginningOfMovie(gaddr h)
{
    movie *m = movie_of(h);
    if (!m)
        return;
    bool playing = m->playing;
    if (playing)
        start(m, 0);
    seek(m, 0);
    if (playing)
        start(m, m->rate);
}

void tb_SetMovieGWorld(gaddr h, gaddr port, gaddr gdh)
{
    movie *m = movie_of(h);
    if (!m)
        return;
    m->port = port ? port : qd_port();
    m->gdh = gdh;
}

void tb_GetMovieGWorld(int32_t h, gaddr port, gaddr gdh)
{
    movie *m = movie_of((gaddr)h);
    if (port)
        wr32(port, m ? m->port : 0);
    if (gdh)
        wr32(gdh, m ? m->gdh : 0);
}

int16_t tb_UpdateMovie(gaddr h)
{
    movie *m = movie_of(h);
    if (m)
        draw_frame(m, true);
    return set_error(noErr);
}

int16_t tb_GetMovieVolume(gaddr h)
{
    movie *m = movie_of(h);
    return m ? m->volume : 0;
}

void tb_SetMovieVolume(int32_t h, int16_t volume)
{
    movie *m = movie_of((gaddr)h);
    if (!m)
        return;
    m->volume = volume;
    if (m->player)
        qt_vlc_set_volume(m->player, vlc_volume(m));
}

int16_t tb_GetMoviePreferredVolume(int32_t h)
{
    movie *m = movie_of((gaddr)h);
    return m ? m->mv->pref_volume : 0;
}

void tb_SetMoviePreferredVolume(int32_t h, int16_t volume)
{
    movie *m = movie_of((gaddr)h);
    if (m)
        m->mv->pref_volume = volume;
}

int8_t tb_IsMovieDone(gaddr h)
{
    movie *m = movie_of(h);
    if (!m)
        return 1;
    task(m);
    return movie_done(m);
}

int32_t tb_GetMoviePreferredRate(gaddr h)
{
    movie *m = movie_of(h);
    return m ? m->mv->pref_rate : 0x10000;
}

void tb_SetMoviePreferredRate(int32_t h, int32_t rate)
{
    movie *m = movie_of((gaddr)h);
    if (m)
        m->mv->pref_rate = rate;
}

int32_t tb_GetMovieRate(int32_t h)
{
    movie *m = movie_of((gaddr)h);
    return m && m->playing ? m->rate : 0;
}

void tb_SetMovieRate(int32_t h, int32_t rate)
{
    movie *m = movie_of((gaddr)h);
    if (m)
        start(m, rate);
}

void tb_GetMovieBox(gaddr h, gaddr box)
{
    movie *m = movie_of(h);
    qrect_put(box, m ? m->box : (qrect){ 0, 0, 0, 0 });
}

void tb_SetMovieBox(gaddr h, gaddr box)
{
    movie *m = movie_of(h);
    if (m)
        m->box = qrect_get(box);
}

void tb_GetMovieNaturalBoundsRect(int32_t h, gaddr r)
{
    movie *m = movie_of((gaddr)h);
    qrect_put(r, m ? natural_rect(m) : (qrect){ 0, 0, 0, 0 });
}

int32_t tb_GetMovieTimeScale(int32_t h)
{
    movie *m = movie_of((gaddr)h);
    return m ? m->mv->scale : 600;
}

int32_t tb_GetMovieDuration(int32_t h)
{
    movie *m = movie_of((gaddr)h);
    return m ? (int32_t)m->mv->duration : 0;
}

/* TimeRecord: value (wide), scale, base. */
int32_t tb_GetMovieTime(int32_t h, gaddr tr)
{
    movie *m = movie_of((gaddr)h);
    if (!m)
        return 0;
    int32_t v = (int32_t)(movie_ms(m) * m->mv->scale / 1000);
    if (tr) {
        wr32(tr, 0);
        wr32(tr + 4, (uint32_t)v);
        wr32(tr + 8, (uint32_t)m->mv->scale);
        wr32(tr + 12, (uint32_t)h);
    }
    return v;
}

void tb_SetMovieTimeValue(int32_t h, int32_t t)
{
    movie *m = movie_of((gaddr)h);
    if (m)
        seek(m, (int64_t)t * 1000 / m->mv->scale);
}

void tb_SetMovieTime(int32_t h, gaddr tr)
{
    movie *m = movie_of((gaddr)h);
    if (!m || !tr)
        return;
    int32_t scale = (int32_t)rd32(tr + 8);
    seek(m, (int64_t)(int32_t)rd32(tr + 4) * 1000 / (scale > 0 ? scale : m->mv->scale));
}

/* A TimeRecord's value in another scale, rounded toward zero. */
static int64_t time_in_scale(gaddr tr, int32_t scale)
{
    int64_t v = (int64_t)((uint64_t)rd32(tr) << 32 | rd32(tr + 4));
    int32_t from = (int32_t)rd32(tr + 8);
    if (from <= 0 || scale <= 0 || from == scale)
        return v;
    return (int64_t)((__int128)v * scale / from);
}

static void set_time_value(gaddr tr, int64_t v)
{
    wr32(tr, (uint32_t)((uint64_t)v >> 32));
    wr32(tr + 4, (uint32_t)v);
}

void tb_ConvertTimeScale(gaddr inout, int32_t newScale)
{
    if (!inout || newScale <= 0)
        return;
    set_time_value(inout, time_in_scale(inout, newScale));
    wr32(inout + 8, (uint32_t)newScale);
}

/* AddTime and SubtractTime: the source in the destination's scale. */
void tb_AddTime(gaddr dst, gaddr src)
{
    if (dst && src)
        set_time_value(dst, time_in_scale(dst, 0) + time_in_scale(src, (int32_t)rd32(dst + 8)));
}

void tb_SubtractTime(gaddr dst, gaddr src)
{
    if (dst && src)
        set_time_value(dst, time_in_scale(dst, 0) - time_in_scale(src, (int32_t)rd32(dst + 8)));
}

/* ---- Time bases ---- */

/* A time base of the program's own (NewTimeBase).  Its time is value (in
 * scale) when its source stood at anchor_ns, moving on from there at rate
 * times the source's: the machine's clock, or a master time base.  Keeping
 * the value as it was set means a stopped time base reads back exactly. */
#define MAX_TIMEBASES 32
#define NS_SCALE 1000000000

enum { loopTimeBase = 1, palindromeLoopTimeBase = 2 };
enum { timeBaseBeforeStartTime = 1, timeBaseAfterStopTime = 2 };

typedef struct {
    bool used;
    gaddr handle; /* the TimeBase the program holds */
    int32_t rate; /* Fixed */
    int64_t value;
    int32_t scale;
    int64_t anchor_ns;
    int32_t own_scale; /* the program's last, for times asked for in scale 0 */
    gaddr master, clock;
    int64_t start, stop;
    int32_t start_scale, stop_scale; /* 0: not set */
    int32_t flags;
} timebase;

static timebase timebases[MAX_TIMEBASES];

static timebase *timebase_of(gaddr tb)
{
    for (int i = 0; tb && i < MAX_TIMEBASES; i++)
        if (timebases[i].used && timebases[i].handle == tb)
            return &timebases[i];
    return NULL;
}

/* A movie's time base: the one GetMovieTimeBase gave, or (as before there
 * were time bases) the movie itself. */
static movie *movie_of_timebase(gaddr tb)
{
    for (int i = 0; tb && i < MAX_MOVIES; i++)
        if (movies[i].used && (movies[i].timebase == tb || movies[i].handle == tb))
            return &movies[i];
    return NULL;
}

/* n / d to the nearest whole number. */
static int64_t div_round(__int128 n, __int128 d)
{
    if (d < 0)
        n = -n, d = -d;
    return (int64_t)(n >= 0 ? (n + d / 2) / d : (n - d / 2) / d);
}

static int64_t tbs_source_ns(const timebase *t, int depth)
{
    if (t->master && depth < MAX_MASTERS)
        return timebase_ns(t->master, depth + 1);
    return (int64_t)machine_time_ns();
}

/* The time in scale s, as it would be without start and stop times. */
static int64_t tbs_unpinned(const timebase *t, int32_t s, int depth)
{
    __int128 elapsed = (__int128)(tbs_source_ns(t, depth) - t->anchor_ns) * t->rate / 0x10000;
    return div_round(((__int128)t->value * NS_SCALE + elapsed * t->scale) * s, (__int128)t->scale * NS_SCALE);
}

/* The time in scale s: held between the start and stop times, or going
 * round between them when the time base loops. */
static int64_t tbs_time(const timebase *t, int32_t s, int depth, int32_t *status)
{
    int64_t v = tbs_unpinned(t, s, depth);
    bool has_start = t->start_scale > 0, has_stop = t->stop_scale > 0;
    int64_t start = has_start ? div_round((__int128)t->start * s, t->start_scale) : 0;
    int64_t stop = has_stop ? div_round((__int128)t->stop * s, t->stop_scale) : 0;
    if (has_start && has_stop && stop > start && (t->flags & (loopTimeBase | palindromeLoopTimeBase))) {
        int64_t span = stop - start, k = (v - start) / span, r = (v - start) % span;
        if (r < 0)
            r += span, k--;
        bool back = (t->flags & palindromeLoopTimeBase) && (k & 1);
        return back ? stop - r : start + r;
    }
    if (has_start && v < start) {
        if (status)
            *status |= timeBaseBeforeStartTime;
        return start;
    }
    if (has_stop && v > stop) {
        if (status)
            *status |= timeBaseAfterStopTime;
        return stop;
    }
    return v;
}

/* Any time base's time in scale s, and in nanoseconds. */
static int64_t timebase_time_in(gaddr tb, int32_t s, int depth)
{
    movie *m = movie_of_timebase(tb);
    if (m)
        return div_round((__int128)movie_ms_at(m, depth) * s, 1000);
    timebase *t = timebase_of(tb);
    return t ? tbs_time(t, s, depth, NULL) : 0;
}

static int64_t timebase_ns(gaddr tb, int depth) { return timebase_time_in(tb, NS_SCALE, depth); }

static int32_t timebase_effective_rate(gaddr tb, int depth)
{
    movie *m = movie_of_timebase(tb);
    if (m)
        return movie_effective_rate_at(m, depth);
    timebase *t = timebase_of(tb);
    if (!t)
        return 0;
    if (!t->master || depth >= MAX_MASTERS)
        return t->rate;
    return (int32_t)((int64_t)t->rate * timebase_effective_rate(t->master, depth + 1) / 0x10000);
}

/* Start counting from now, at the time it has now (for a new rate). */
static void tbs_reanchor(timebase *t)
{
    int64_t src = tbs_source_ns(t, 0);
    if (t->rate && src != t->anchor_ns) {
        t->value = tbs_time(t, NS_SCALE, 0, NULL);
        t->scale = NS_SCALE;
    }
    t->anchor_ns = src;
}

/* A TimeRecord's scale, or the fallback when it has none. */
static int32_t record_scale(gaddr tr, int32_t fallback)
{
    int32_t s = (int32_t)rd32(tr + 8);
    return s > 0 ? s : fallback;
}

/* Where a time base stands on a new source: its time is the source's less
 * slaveZero (in the master's scale), so with no zero a slave reads as its
 * master does.  A clock with no zero leaves the time where it was. */
static void tbs_set_source(timebase *t, gaddr master, gaddr clock, gaddr zero)
{
    if (master && (master == t->handle || timebase_of(master) == t))
        master = 0;
    int64_t was = tbs_time(t, NS_SCALE, 0, NULL);
    t->master = master;
    t->clock = master ? 0 : clock;
    int64_t src = tbs_source_ns(t, 0);
    if (zero) {
        int32_t zs = record_scale(zero, 600);
        t->value = div_round((__int128)src * zs, NS_SCALE) - (int64_t)((uint64_t)rd32(zero) << 32 | rd32(zero + 4));
        t->scale = zs;
    } else {
        t->value = master ? src : was;
        t->scale = NS_SCALE;
    }
    t->anchor_ns = src;
}

/* The same for a movie: its time from the new source on. */
static void movie_set_source(movie *m, gaddr master, gaddr clock, gaddr zero)
{
    if (master && movie_of_timebase(master) == m)
        master = 0;
    int64_t was = movie_ms(m);
    m->master = master;
    m->clock = master ? 0 : clock;
    int64_t src = source_ns(m, 0);
    if (zero) {
        int32_t zs = record_scale(zero, 600);
        int64_t z = (int64_t)((uint64_t)rd32(zero) << 32 | rd32(zero + 4));
        m->base_ms = src / 1000000 - div_round((__int128)z * 1000, zs);
    } else {
        m->base_ms = master ? src / 1000000 : was;
    }
    m->start_ns = src;
    stop_sound(m);
    if (m->player && !master && m->playing) {
        /* Back on VLC's own clock, from where the master left it. */
        qt_vlc_play(m->player);
        qt_vlc_seek(m->player, movie_ms(m));
        m->vlc_held = false;
    }
}

/* Time bases and movies slaved to one that is going: on the clock, where
 * they are. */
static void release_slaves(gaddr tb)
{
    if (!tb)
        return;
    for (int i = 0; i < MAX_MOVIES; i++)
        if (movies[i].used && movies[i].master == tb)
            movie_set_source(&movies[i], 0, 0, 0);
    for (int i = 0; i < MAX_TIMEBASES; i++)
        if (timebases[i].used && timebases[i].master == tb)
            tbs_set_source(&timebases[i], 0, 0, 0);
}

int32_t tb_GetMovieTimeBase(int32_t theMovie)
{
    movie *m = movie_of((gaddr)theMovie);
    return m ? (int32_t)m->timebase : 0;
}

int32_t tb_NewTimeBase(void)
{
    for (int i = 0; i < MAX_TIMEBASES; i++) {
        timebase *t = &timebases[i];
        if (t->used)
            continue;
        memset(t, 0, sizeof *t);
        t->handle = tb_NewHandle(4, false, true);
        if (!t->handle)
            break;
        wr32(rd32(t->handle), 0x74626173); /* 'tbas' */
        t->used = true;
        t->scale = t->own_scale = 600;
        t->anchor_ns = (int64_t)machine_time_ns();
        set_error(noErr);
        return (int32_t)t->handle;
    }
    set_error(memFullErr);
    return 0;
}

void tb_DisposeTimeBase(int32_t tb)
{
    timebase *t = timebase_of((gaddr)tb);
    if (!t) /* a movie's goes with the movie */
        return;
    qt_callbacks_timebase_gone(t->handle);
    release_slaves(t->handle);
    tb_DisposeHandle(t->handle);
    t->used = false;
}

/* TimeRecord: value (wide), scale, base.  A scale of 0 is the time base's own. */
int32_t tb_GetTimeBaseTime(int32_t tb, int32_t s, gaddr tr)
{
    movie *m = movie_of_timebase((gaddr)tb);
    timebase *t = m ? NULL : timebase_of((gaddr)tb);
    if (!m && !t)
        return 0;
    if (s <= 0)
        s = m ? m->mv->scale : t->own_scale;
    int64_t v = timebase_time_in((gaddr)tb, s, 0);
    if (tr) {
        set_time_value(tr, v);
        wr32(tr + 8, (uint32_t)s);
        wr32(tr + 12, (uint32_t)tb);
    }
    return (int32_t)v;
}

void tb_SetTimeBaseValue(int32_t tb, int32_t t, int32_t s)
{
    movie *m = movie_of_timebase((gaddr)tb);
    if (m) {
        seek(m, div_round((__int128)t * 1000, s > 0 ? s : m->mv->scale));
        return;
    }
    timebase *b = timebase_of((gaddr)tb);
    if (!b)
        return;
    b->value = t;
    b->scale = b->own_scale = s > 0 ? s : b->own_scale;
    b->anchor_ns = tbs_source_ns(b, 0);
    qt_callbacks_jumped(b->handle);
}

void tb_SetTimeBaseTime(int32_t tb, gaddr tr)
{
    if (!tr)
        return;
    movie *m = movie_of_timebase((gaddr)tb);
    if (m) {
        seek(m, div_round((__int128)time_in_scale(tr, 0) * 1000, record_scale(tr, m->mv->scale)));
        return;
    }
    timebase *b = timebase_of((gaddr)tb);
    if (!b)
        return;
    b->value = time_in_scale(tr, 0);
    b->scale = b->own_scale = record_scale(tr, b->own_scale);
    b->anchor_ns = tbs_source_ns(b, 0);
    qt_callbacks_jumped(b->handle);
}

int32_t tb_GetTimeBaseRate(int32_t tb)
{
    movie *m = movie_of_timebase((gaddr)tb);
    if (m)
        return m->playing ? m->rate : 0;
    timebase *t = timebase_of((gaddr)tb);
    return t ? t->rate : 0;
}

void tb_SetTimeBaseRate(int32_t tb, int32_t r)
{
    movie *m = movie_of_timebase((gaddr)tb);
    if (m) {
        start(m, r);
        return;
    }
    timebase *t = timebase_of((gaddr)tb);
    if (!t)
        return;
    tbs_reanchor(t);
    t->rate = r;
}

int32_t tb_GetTimeBaseEffectiveRate(int32_t tb) { return timebase_effective_rate((gaddr)tb, 0); }

/* Start and stop times: a movie's are its beginning and end. */
static int32_t get_limit(int32_t tb, int32_t s, gaddr tr, bool stop)
{
    movie *m = movie_of_timebase((gaddr)tb);
    timebase *t = m ? NULL : timebase_of((gaddr)tb);
    if (!m && !t)
        return 0;
    if (s <= 0)
        s = m ? m->mv->scale : t->own_scale;
    int64_t v = 0;
    if (m)
        v = stop ? div_round((__int128)m->mv->duration * s, m->mv->scale) : 0;
    else if (stop ? t->stop_scale : t->start_scale)
        v = div_round((__int128)(stop ? t->stop : t->start) * s, stop ? t->stop_scale : t->start_scale);
    if (tr) {
        set_time_value(tr, v);
        wr32(tr + 8, (uint32_t)s);
        wr32(tr + 12, (uint32_t)tb);
    }
    return (int32_t)v;
}

static void set_limit(int32_t tb, gaddr tr, bool stop)
{
    timebase *t = timebase_of((gaddr)tb);
    if (!t || !tr)
        return;
    tbs_reanchor(t);
    int64_t v = time_in_scale(tr, 0);
    int32_t s = record_scale(tr, t->own_scale);
    if (stop)
        t->stop = v, t->stop_scale = s;
    else
        t->start = v, t->start_scale = s;
}

int32_t tb_GetTimeBaseStartTime(int32_t tb, int32_t s, gaddr tr) { return get_limit(tb, s, tr, false); }
int32_t tb_GetTimeBaseStopTime(int32_t tb, int32_t s, gaddr tr) { return get_limit(tb, s, tr, true); }
void tb_SetTimeBaseStartTime(int32_t tb, gaddr tr) { set_limit(tb, tr, false); }
void tb_SetTimeBaseStopTime(int32_t tb, gaddr tr) { set_limit(tb, tr, true); }

int32_t tb_GetTimeBaseFlags(int32_t tb)
{
    movie *m = movie_of_timebase((gaddr)tb);
    if (m)
        return m->looping ? loopTimeBase : 0;
    timebase *t = timebase_of((gaddr)tb);
    return t ? t->flags : 0;
}

void tb_SetTimeBaseFlags(int32_t tb, int32_t timeBaseFlags)
{
    movie *m = movie_of_timebase((gaddr)tb);
    if (m) {
        m->looping = (timeBaseFlags & loopTimeBase) != 0;
        return;
    }
    timebase *t = timebase_of((gaddr)tb);
    if (t) {
        tbs_reanchor(t);
        t->flags = timeBaseFlags;
    }
}

/* Whether the time base is held at its start or stop time, and the time
 * it would have without them. */
int32_t tb_GetTimeBaseStatus(int32_t tb, gaddr unpinnedTime)
{
    movie *m = movie_of_timebase((gaddr)tb);
    timebase *t = m ? NULL : timebase_of((gaddr)tb);
    int32_t status = 0, s = m ? m->mv->scale : t ? t->own_scale : 600;
    int64_t v = 0;
    if (t) {
        tbs_time(t, s, 0, &status);
        v = tbs_unpinned(t, s, 0);
    } else if (m) {
        v = div_round((__int128)movie_ms(m) * s, 1000);
    }
    if (unpinnedTime) {
        set_time_value(unpinnedTime, v);
        wr32(unpinnedTime + 8, (uint32_t)s);
        wr32(unpinnedTime + 12, (uint32_t)tb);
    }
    return status;
}

void tb_SetTimeBaseMasterTimeBase(int32_t slave, int32_t master, gaddr slaveZero)
{
    movie *m = movie_of_timebase((gaddr)slave);
    timebase *t = m ? NULL : timebase_of((gaddr)slave);
    if (m)
        movie_set_source(m, (gaddr)master, 0, slaveZero);
    else if (t)
        tbs_set_source(t, (gaddr)master, 0, slaveZero);
}

int32_t tb_GetTimeBaseMasterTimeBase(int32_t tb)
{
    movie *m = movie_of_timebase((gaddr)tb);
    timebase *t = m ? NULL : timebase_of((gaddr)tb);
    return (int32_t)(m ? m->master : t ? t->master : 0);
}

/* A clock component as the source: whichever it is, the machine's clock
 * keeps the time (the tick clock's, finer). */
void tb_SetTimeBaseMasterClock(int32_t slave, int32_t clockMeister, gaddr slaveZero)
{
    movie *m = movie_of_timebase((gaddr)slave);
    timebase *t = m ? NULL : timebase_of((gaddr)slave);
    if (m)
        movie_set_source(m, 0, (gaddr)clockMeister, slaveZero);
    else if (t)
        tbs_set_source(t, 0, (gaddr)clockMeister, slaveZero);
}

int32_t tb_GetTimeBaseMasterClock(int32_t tb)
{
    movie *m = movie_of_timebase((gaddr)tb);
    timebase *t = m ? NULL : timebase_of((gaddr)tb);
    return (int32_t)(m ? m->clock : t ? t->clock : 0);
}

/* SetTimeBaseZero: a new offset from the same source. */
void tb_SetTimeBaseZero(int32_t tb, gaddr zero)
{
    movie *m = movie_of_timebase((gaddr)tb);
    timebase *t = m ? NULL : timebase_of((gaddr)tb);
    if (m)
        movie_set_source(m, m->master, m->clock, zero);
    else if (t)
        tbs_set_source(t, t->master, t->clock, zero);
}

/* ConvertTime: the time in another time base at the moment the record's
 * own reads that time, both going on at their present rates. */
void tb_ConvertTime(gaddr inout, int32_t newBase)
{
    if (!inout)
        return;
    gaddr from = rd32(inout + 12);
    int32_t s = record_scale(inout, 600);
    if (from && (movie_of_timebase(from) || timebase_of(from)) && newBase) {
        int64_t v = div_round((__int128)time_in_scale(inout, 0) * NS_SCALE, s);
        int64_t from_now = timebase_ns(from, 0), to_now = timebase_ns((gaddr)newBase, 0);
        int32_t from_rate = timebase_effective_rate(from, 0), to_rate = timebase_effective_rate((gaddr)newBase, 0);
        int64_t to = from_rate ? to_now + (int64_t)((__int128)(v - from_now) * to_rate / from_rate) : to_now;
        set_time_value(inout, div_round((__int128)to * s, NS_SCALE));
    }
    wr32(inout + 12, (uint32_t)newBase);
}

/* SetMovieMasterTimeBase(movie, master, slaveZero): the movie follows the
 * master's time and rate (its own rate still multiplying), nil for its own
 * clock again.  SetMovieMasterClock gives it a clock instead. */
void tb_SetMovieMasterTimeBase(int32_t theMovie, int32_t tb, gaddr slaveZero)
{
    movie *m = movie_of((gaddr)theMovie);
    if (m)
        movie_set_source(m, (gaddr)tb, 0, slaveZero);
}

void tb_SetMovieMasterClock(int32_t theMovie, int32_t clockMeister, gaddr slaveZero)
{
    movie *m = movie_of((gaddr)theMovie);
    if (m)
        movie_set_source(m, 0, (gaddr)clockMeister, slaveZero);
}

int32_t tb_GetMovieTrackCount(int32_t h)
{
    movie *m = movie_of((gaddr)h);
    return m ? m->mv->ntracks : 0;
}

int32_t tb_GetMovieIndTrack(int32_t h, int32_t index)
{
    movie *m = movie_of((gaddr)h);
    if (!m || index < 1 || index > m->mv->ntracks) {
        set_error(invalidTrack);
        return 0;
    }
    return (int32_t)m->mv->tracks[index - 1]->token;
}

/* flags: movieTrackMediaType (1) the type is a media type,
 * movieTrackCharacteristic (2) a characteristic ('eyes', 'ears', ...),
 * movieTrackEnabledOnly (4) only enabled tracks count. */
int32_t tb_GetMovieIndTrackType(int32_t h, int32_t index, int32_t type, int32_t flags)
{
    movie *m = movie_of((gaddr)h);
    for (int k = 0, n = 0; m && k < m->mv->ntracks; k++) {
        const qtm_track *t = m->mv->tracks[k];
        if ((flags & 4) && !(t->flags & 1))
            continue;
        uint32_t mt = t->media ? t->media->type : 0;
        bool match = (flags & 2) ? qt_media_has_characteristic(mt, (uint32_t)type) : mt == (uint32_t)type;
        if (match && ++n == index)
            return (int32_t)t->token;
    }
    return 0;
}

int32_t tb_GetTrackMovie(int32_t t)
{
    movie *m = NULL;
    find_track((gaddr)t, &m);
    if (!m)
        set_error(invalidTrack);
    return m ? (int32_t)m->handle : 0;
}

int8_t tb_GetTrackEnabled(int32_t t)
{
    qtm_track *tr = find_track((gaddr)t, NULL);
    return tr && (tr->flags & 1);
}

void tb_SetTrackEnabled(int32_t t, int8_t enabled)
{
    movie *m = NULL;
    qtm_track *tr = find_track((gaddr)t, &m);
    if (!tr) {
        set_error(invalidTrack);
        return;
    }
    tr->flags = enabled ? tr->flags | 1 : tr->flags & ~1u;
    qt_movie_touch(m->mv);
    if (m->player)
        qt_vlc_set_volume(m->player, vlc_volume(m));
}

int32_t tb_GetTrackMedia(int32_t t)
{
    qtm_track *tr = find_track((gaddr)t, NULL);
    if (!tr)
        set_error(invalidTrack);
    return tr && tr->media ? (int32_t)tr->media->token : 0;
}

void tb_GetMediaHandlerDescription(int32_t media, gaddr type, int32_t name, gaddr manufacturer)
{
    qtm_media *md = find_media((gaddr)media, NULL, NULL);
    if (type)
        wr32(type, md ? md->type : 0);
    if (name) {
        size_t n = md ? strlen(md->handler_name) : 0;
        wr8((gaddr)name, (uint8_t)n);
        if (n)
            gput((gaddr)name + 1, md->handler_name, (uint32_t)n);
    }
    if (manufacturer)
        wr32(manufacturer, md && md->manufacturer ? md->manufacturer : fourcc("appl"));
}

/* The 'mvhd' atom's dates; a movie made by NewMovie was made now. */
int32_t tb_GetMovieCreationTime(int32_t theMovie)
{
    movie *m = movie_of((gaddr)theMovie);
    return m ? (int32_t)m->mv->created : 0;
}

int32_t tb_GetMovieModificationTime(int32_t theMovie)
{
    movie *m = movie_of((gaddr)theMovie);
    return m ? (int32_t)m->mv->modified : 0;
}

/* Clip regions: ours are copies, and the Get calls give a copy of them
 * (nil for none), which the program disposes of. */
static void keep_clip(gaddr *mine, gaddr theirs)
{
    if (*mine)
        tb_DisposeRgn(*mine);
    *mine = 0;
    if (theirs) {
        *mine = tb_NewRgn();
        tb_CopyRgn(theirs, *mine);
    }
}

static int32_t copy_clip(gaddr mine)
{
    if (!mine)
        return 0;
    gaddr r = tb_NewRgn();
    tb_CopyRgn(mine, r);
    return (int32_t)r;
}

int32_t tb_GetMovieClipRgn(int32_t theMovie)
{
    movie *m = movie_of((gaddr)theMovie);
    return m ? copy_clip(m->clip) : 0;
}

void tb_SetMovieClipRgn(int32_t theMovie, int32_t theClip)
{
    movie *m = movie_of((gaddr)theMovie);
    if (m)
        keep_clip(&m->clip, (gaddr)theClip);
}

int32_t tb_GetMovieDisplayClipRgn(int32_t theMovie)
{
    movie *m = movie_of((gaddr)theMovie);
    return m ? copy_clip(m->display_clip) : 0;
}

void tb_SetMovieDisplayClipRgn(int32_t theMovie, int32_t theClip)
{
    movie *m = movie_of((gaddr)theMovie);
    if (m)
        keep_clip(&m->display_clip, (gaddr)theClip);
}


/* ---------------------------------------------------------------------- */
/* The movie controller: a component of our own */

enum {
    mcTopLeftMovie = 1, mcScaleMovieToFit = 2, mcNotVisible = 8,
    mcActionIdle = 1, mcActionDraw = 2, mcActionActivate = 3, mcActionDeactivate = 4, mcActionMouseDown = 5,
    mcActionKey = 6, mcActionPlay = 8, mcActionGoToTime = 12, mcActionSetVolume = 14, mcActionGetVolume = 15,
    mcActionStep = 18, mcActionSetLooping = 21, mcActionGetLooping = 22, mcActionSetFlags = 38,
    mcActionGetFlags = 39, mcActionGetPlayRate = 42, mcActionSuspend = 46, mcActionResume = 47,
    mcInfoUndoAvailable = 1, mcInfoCutAvailable = 2, mcInfoCopyAvailable = 4, mcInfoPasteAvailable = 8,
    mcInfoClearAvailable = 16, mcInfoHasSound = 1 << 5, mcInfoIsPlaying = 1 << 6, mcInfoIsLooping = 1 << 7,
    mcInfoIsInPalindrome = 1 << 8, mcInfoEditingEnabled = 1 << 9,
    BAR = 16,
};

#define MAX_CONTROLLERS 16

typedef struct {
    bool used;
    uint32_t ci;
    gaddr movie;
    gaddr port;
    qrect bounds; /* movie and bar */
    bool visible, active, attached;
    int32_t flags;
    gaddr filter;
    int32_t filter_refcon;
    int drawn_x;       /* where the slider's thumb was drawn (-1: none) */
    int drawn_playing; /* and the button, 1 pause or 0 play (-1: not drawn) */
    bool editing;      /* MCEnableEditing */
    /* what the actions set (QuickTime 2 to 4) */
    bool palindrome, keys_enabled, play_selection, use_badge, every_frame, drag_enabled, cursor_enabled;
    bool ctl_keys_enabled;
    int16_t volume_step;
    qrect grow_box;
    gaddr clip;        /* MCSetClip's for the controller */
    int32_t duration;  /* MCSetDuration's, with no movie */
} controller;

static controller controllers[MAX_CONTROLLERS];
static uint32_t mc_component;
static gaddr mc_do_actions_stub(void);

static controller *controller_of(uint32_t ci)
{
    for (int i = 0; i < MAX_CONTROLLERS; i++)
        if (controllers[i].used && controllers[i].ci == ci)
            return &controllers[i];
    return NULL;
}

static qrect bar_rect(const controller *c)
{
    return (qrect){ (int16_t)(c->bounds.bottom - BAR), c->bounds.left, c->bounds.bottom, c->bounds.right };
}

/* The application's action filter, if any: true if it handled the action. */
static bool filter(controller *c, int16_t action, gaddr params)
{
    if (!c->filter)
        return false;
    cpu68k *cpu = g_cpu;
    cpu_push16(cpu, 0);
    cpu_push32(cpu, c->ci);
    cpu_push16(cpu, (uint16_t)action);
    cpu_push32(cpu, params);
    cpu_push32(cpu, (uint32_t)c->filter_refcon);
    guest_call(c->filter);
    return (cpu_pop16(cpu) >> 8) != 0;
}

/* The slider thumb's centre for the movie's time, or -1 without one. */
static int thumb_x(const controller *c, movie *m)
{
    qrect b = bar_rect(c);
    int left = b.left + 18, right = b.right - 6;
    if (!m || m->mv->duration <= 0 || right <= left)
        return -1;
    return left + (int)((right - left) * movie_ms(m) / (duration_ms(m) ? duration_ms(m) : 1));
}

static qrect thumb_rect(const controller *c, int x)
{
    qrect b = bar_rect(c);
    return (qrect){ (int16_t)(b.top + 4), (int16_t)(x - 3), (int16_t)(b.top + 13), (int16_t)(x + 3) };
}

static void draw_bar(controller *c)
{
    movie *m = movie_of(c->movie);
    if (!c->visible || !c->attached)
        return;
    if (m && qt_vr_draw_bar(m->handle, c->port, bar_rect(c), c->flags)) /* QuickTime VR's buttons */
        return;
    gaddr saved = qd_port();
    tb_SetPort(c->port);
    qrect b = bar_rect(c);
    gaddr r = tb_NewPtr(8, false, false);
    qrect_put(r, b);
    tb_PenNormal();
    tb_EraseRect(r);
    tb_FrameRect(r);
    /* The play/pause button. */
    int bx = b.left + 4, by = b.top + 3;
    if (m && m->playing) {
        qrect_put(r, (qrect){ (int16_t)by, (int16_t)bx, (int16_t)(by + 10), (int16_t)(bx + 3) });
        tb_PaintRect(r);
        qrect_put(r, (qrect){ (int16_t)by, (int16_t)(bx + 5), (int16_t)(by + 10), (int16_t)(bx + 8) });
        tb_PaintRect(r);
    } else {
        for (int i = 0; i < 5; i++) {
            tb_MoveTo((int16_t)(bx + i * 2), (int16_t)(by + i));
            tb_LineTo((int16_t)(bx + i * 2), (int16_t)(by + 9 - i));
            tb_MoveTo((int16_t)(bx + i * 2 + 1), (int16_t)(by + i));
            tb_LineTo((int16_t)(bx + i * 2 + 1), (int16_t)(by + 9 - i));
        }
    }
    c->drawn_playing = m && m->playing;
    /* The time slider, the selection a thick stretch of it. */
    int left = b.left + 18, right = b.right - 6;
    tb_MoveTo((int16_t)left, (int16_t)(b.top + 8));
    tb_LineTo((int16_t)right, (int16_t)(b.top + 8));
    if (m && m->mv->sel_duration > 0 && m->mv->duration > 0 && right > left) {
        int x0 = left + (int)((int64_t)(right - left) * m->mv->sel_time / m->mv->duration);
        int x1 = left + (int)((int64_t)(right - left) * (m->mv->sel_time + m->mv->sel_duration) / m->mv->duration);
        qrect_put(r, (qrect){ (int16_t)(b.top + 6), (int16_t)x0, (int16_t)(b.top + 11), (int16_t)(x1 > x0 ? x1 : x0 + 1) });
        tb_PaintRect(r);
    }
    c->drawn_x = thumb_x(c, m);
    if (c->drawn_x >= 0) {
        qrect_put(r, thumb_rect(c, c->drawn_x));
        tb_EraseRect(r);
        tb_FrameRect(r);
    }
    tb_DisposePtr(r);
    tb_SetPort(saved);
}

/* While the movie plays: the thumb moved along, the rest left alone (a
 * whole bar redrawn at every idle flickers). */
static void update_bar(controller *c)
{
    movie *m = movie_of(c->movie);
    if (!c->visible || !c->attached || (m && qt_vr_movie(m->handle)))
        return;
    int x = thumb_x(c, m), playing = m && m->playing;
    if (playing != c->drawn_playing || c->drawn_x < 0 || x < 0 || (m && m->mv->sel_duration > 0)) {
        draw_bar(c);
        return;
    }
    if (x == c->drawn_x)
        return;
    gaddr saved = qd_port();
    tb_SetPort(c->port);
    tb_PenNormal();
    qrect b = bar_rect(c), old = thumb_rect(c, c->drawn_x);
    gaddr r = tb_NewPtr(8, false, false);
    qrect_put(r, old);
    tb_EraseRect(r);
    int left = b.left + 18, right = b.right - 6; /* the slider's line where the thumb was */
    int from = old.left > left ? old.left : left, to = old.right - 1 < right ? old.right - 1 : right;
    if (to >= from) {
        tb_MoveTo((int16_t)from, (int16_t)(b.top + 8));
        tb_LineTo((int16_t)to, (int16_t)(b.top + 8));
    }
    qrect_put(r, thumb_rect(c, x));
    tb_EraseRect(r);
    tb_FrameRect(r);
    tb_DisposePtr(r);
    c->drawn_x = x;
    tb_SetPort(saved);
}

static void mc_draw(controller *c)
{
    if (filter(c, mcActionDraw, c->port))
        return;
    movie *m = movie_of(c->movie);
    if (m)
        draw_frame(m, true);
    draw_bar(c);
}

static void mc_idle(controller *c)
{
    if (filter(c, mcActionIdle, 0))
        return;
    movie *m = movie_of(c->movie);
    if (!m)
        return;
    bool was = m->playing;
    task(m);
    if (m->playing || was)
        update_bar(c);
}

static void mc_play(controller *c, int32_t rate)
{
    movie *m = movie_of(c->movie);
    if (m) {
        start(m, rate);
        draw_bar(c);
    }
}

/* A click at a local point: the button plays and pauses, the slider moves
 * through the movie, and a click in the movie plays or pauses it. */
static bool mc_click_with(controller *c, uint32_t pt, bool shift);
static bool mc_click(controller *c, uint32_t pt) { return mc_click_with(c, pt, false); }

static bool mc_click_with(controller *c, uint32_t pt, bool shift)
{
    movie *m = movie_of(c->movie);
    if (!m)
        return false;
    int h = pt_h(pt), v = pt_v(pt);
    qrect b = bar_rect(c);
    bool in_bar = c->visible && c->attached && v >= b.top && v < b.bottom && h >= b.left && h < b.right;
    bool in_movie = v >= m->box.top && v < m->box.bottom && h >= m->box.left && h < m->box.right;
    if (!in_bar && !in_movie)
        return false;
    if (in_bar && qt_vr_bar_click(m->handle, c->ci, pt, b, c->flags))
        return true;
    if (in_bar && h >= b.left + 18) {
        int left = b.left + 18, right = b.right - 6;
        if (right > left) {
            int64_t ms = duration_ms(m) * (h < left ? 0 : h > right ? right - left : h - left) / (right - left);
            if (shift && c->editing) { /* the selection from the current time (or its far end) to here */
                int64_t at = ms * m->mv->scale / 1000, now = movie_ms(m) * m->mv->scale / 1000;
                int64_t anchor = m->mv->sel_duration > 0
                                     ? (llabs(at - m->mv->sel_time) > llabs(at - (m->mv->sel_time + m->mv->sel_duration))
                                            ? m->mv->sel_time
                                            : m->mv->sel_time + m->mv->sel_duration)
                                     : now;
                tb_SetMovieSelection((int32_t)m->handle, (int32_t)(at < anchor ? at : anchor),
                                     (int32_t)llabs(at - anchor));
            } else {
                seek(m, ms);
                if (c->editing)
                    tb_SetMovieSelection((int32_t)m->handle, (int32_t)(ms * m->mv->scale / 1000), 0);
            }
        }
    } else {
        mc_play(c, m->playing ? 0 : (m->mv->pref_rate ? m->mv->pref_rate : 0x10000));
    }
    draw_bar(c);
    return true;
}

static void place(controller *c, qrect r)
{
    c->drawn_x = -1; /* the bar is drawn whole next time */
    movie *m = movie_of(c->movie);
    int bar = c->visible && c->attached ? BAR : 0;
    if (!m) {
        c->bounds = r;
        return;
    }
    qrect natural = natural_rect(m);
    int w = natural.right - natural.left, h = natural.bottom - natural.top;
    if (c->flags & mcScaleMovieToFit || w <= 0 || h <= 0) {
        w = r.right - r.left;
        h = r.bottom - r.top - bar;
    }
    if (w < 0)
        w = 0;
    if (h < 0)
        h = 0;
    m->box = (qrect){ r.top, r.left, (int16_t)(r.top + h), (int16_t)(r.left + w) };
    int bw = w > 64 ? w : 64;
    c->bounds = (qrect){ r.top, r.left, (int16_t)(r.top + h + bar), (int16_t)(r.left + bw) };
}

/* ComponentResult (params, storage): params are flags, size, what, then the
 * arguments last first, then the controller. */
static void mc_entry(cpu68k *cpu, uint16_t tw)
{
    uint32_t ret = cpu_pop32(cpu);
    gaddr storage = cpu_pop32(cpu);
    gaddr params = cpu_pop32(cpu);
    int16_t what = (int16_t)rd16(params + 2);
    uint32_t size = rd8(params + 1);
    gaddr a = params + 4; /* the arguments */
    uint32_t ci = rd32(params + 4 + size);
    controller *c = controller_of(ci);
    int32_t result = 0;
    (void)storage;
    if (what < 0) {
        result = what == -3 ? 1 : what == -4 ? 0x00030000 : 0; /* can do, version */
    } else if (!c) {
        result = (int32_t)0x80008001; /* badComponentInstance */
    } else {
        movie *m = movie_of(c->movie);
        switch (what) {
        case 2: /* MCSetMovie(m, w, where) */
            c->movie = rd32(a + 8);
            c->port = rd32(a + 4) ? rd32(a + 4) : c->port;
            place(c, (qrect){ (int16_t)pt_v(rd32(a)), (int16_t)pt_h(rd32(a)), 0, 0 });
            break;
        case 5: result = (int32_t)c->movie; break; /* MCGetIndMovie */
        case 3:
        case 6: c->movie = 0; break;               /* MCRemove... */
        case 7: { /* MCIsPlayerEvent(e) */
            gaddr e = rd32(a);
            int ew = (int16_t)rd16(e);
            if (ew == 0) { /* null: time passes */
                if (m)
                    qt_vr_movie_modifiers(m->handle, rd16(e + 14));
                mc_idle(c);
            } else if (ew == 6 && rd32(e + 2) == c->port) { /* update */
                tb_BeginUpdate(c->port);
                mc_draw(c);
                tb_EndUpdate(c->port);
                result = 1;
            } else if (ew == 1 && c->active) { /* mouse down */
                gaddr pt = tb_NewPtr(4, false, false);
                wr32(pt, rd32(e + 10));
                gaddr saved = qd_port();
                tb_SetPort(c->port);
                tb_GlobalToLocal(pt);
                tb_SetPort(saved);
                if (!filter(c, mcActionMouseDown, e) &&
                    ((m && qt_vr_movie_click(m->handle, c->ci, rd32(pt), rd16(e + 14))) ||
                     mc_click_with(c, rd32(pt), rd16(e + 14) & 0x0200)))
                    result = 1;
                tb_DisposePtr(pt);
            } else if ((ew == 3 || ew == 5) && c->active && m && qt_vr_movie(m->handle)) { /* QuickTime VR's keys */
                if (!filter(c, mcActionKey, e) && qt_vr_movie_key(m->handle, (uint8_t)rd32(e + 2), rd16(e + 14)))
                    result = 1;
            } else if ((ew == 3 || ew == 5) && c->active && (rd32(e + 2) & 0xFF) == ' ') { /* space */
                if (!filter(c, mcActionKey, e) && m)
                    mc_play(c, m->playing ? 0 : m->mv->pref_rate);
                result = 1;
            } else if (ew == 8 && rd32(e + 2) == c->port) { /* activate */
                c->active = (rd16(e + 14) & 1) != 0;
            }
            break;
        }
        case 8: c->filter = rd32(a); c->filter_refcon = 0; break; /* MCSetActionFilter */
        case 45: c->filter = rd32(a + 4); c->filter_refcon = (int32_t)rd32(a); break;
        case 9: { /* MCDoAction(action, params) */
            int16_t action = (int16_t)rd16(a + 4);
            gaddr p = rd32(a);
            if (filter(c, action, p))
                break;
            switch (action) {
            case mcActionIdle: mc_idle(c); break;
            case mcActionDraw: mc_draw(c); break;
            case mcActionActivate: c->active = true; break;
            case mcActionDeactivate: c->active = false; break;
            case mcActionPlay: mc_play(c, (int32_t)p); break;
            case mcActionGoToTime:
                if (m && p)
                    tb_SetMovieTime((int32_t)c->movie, p);
                draw_bar(c);
                break;
            case mcActionSetVolume: tb_SetMovieVolume((int32_t)c->movie, (int16_t)(int32_t)p); break;
            case mcActionGetVolume: if (p) wr16(p, (uint16_t)(m ? m->volume : 0)); break;
            case mcActionSetLooping: if (m) m->looping = ((uint32_t)p & 0xFF) || ((uint32_t)p >> 8 & 0xFF); break;
            case mcActionGetLooping: if (p) wr8(p, m && m->looping); break;
            case mcActionSetFlags: c->flags = (int32_t)p; break;
            case mcActionGetFlags: if (p) wr32(p, (uint32_t)c->flags); break;
            case mcActionGetPlayRate: if (p) wr32(p, (uint32_t)(m && m->playing ? m->rate : 0)); break;
            case mcActionStep: /* frames: to the next sample boundaries */
                if (m) {
                    int16_t steps = (int16_t)(int32_t)p;
                    int32_t t = (int32_t)(movie_ms(m) * m->mv->scale / 1000);
                    gaddr it = tb_NewPtr(8, false, true);
                    for (int i = 0; i < (steps < 0 ? -steps : steps); i++) { /* sample starts, before or after */
                        tb_GetMovieNextInterestingTime((int32_t)m->handle, 1, 0, 0, t, steps < 0 ? -0x10000 : 0x10000,
                                                       it, it + 4);
                        if ((int32_t)rd32(it) < 0)
                            break;
                        t = (int32_t)rd32(it);
                    }
                    tb_DisposePtr(it);
                    if (m->playing)
                        start(m, 0);
                    seek(m, (int64_t)(t < 0 ? 0 : t) * 1000 / m->mv->scale);
                    draw_frame(m, true);
                    draw_bar(c);
                }
                break;
            case 23: /* mcActionSetLoopIsPalindrome */
                c->palindrome = ((uint32_t)p & 0xFF) || ((uint32_t)p >> 8 & 0xFF);
                if (m) {
                    int32_t f = tb_GetTimeBaseFlags((int32_t)m->timebase);
                    tb_SetTimeBaseFlags((int32_t)m->timebase, c->palindrome ? f | 2 : f & ~2);
                }
                break;
            case 24: if (p) wr8(p, c->palindrome); break;          /* mcActionGetLoopIsPalindrome */
            case 25: if (p) c->grow_box = qrect_get(p); break;      /* mcActionSetGrowBoxBounds */
            case 26: case 50: case 61: case 77: draw_bar(c); break; /* size changed, edited, time table, changed */
            case 29: /* mcActionSetSelectionBegin(TimeRecord) */
                if (m && p)
                    tb_SetMovieSelection((int32_t)m->handle,
                                         (int32_t)(time_in_scale(p, m->mv->scale) * 1), m->mv->sel_duration);
                draw_bar(c);
                break;
            case 30: /* mcActionSetSelectionDuration(TimeRecord) */
                if (m && p)
                    tb_SetMovieSelection((int32_t)m->handle, m->mv->sel_time, (int32_t)time_in_scale(p, m->mv->scale));
                draw_bar(c);
                break;
            case 53: case 54: /* mcActionGetSelectionBegin / Duration (TimeRecord) */
                if (m && p) {
                    set_time_value(p, action == 53 ? m->mv->sel_time : m->mv->sel_duration);
                    wr32(p + 8, (uint32_t)m->mv->scale);
                    wr32(p + 12, m->timebase);
                }
                break;
            case 32: c->keys_enabled = ((uint32_t)p & 0xFF) || ((uint32_t)p >> 8 & 0xFF); break;
            case 33: if (p) wr8(p, c->keys_enabled); break;
            case 34: /* mcActionSetPlaySelection: the active segment is the selection */
                c->play_selection = ((uint32_t)p & 0xFF) || ((uint32_t)p >> 8 & 0xFF);
                if (m)
                    tb_SetMovieActiveSegment((int32_t)m->handle, c->play_selection && m->mv->sel_duration > 0
                                                                     ? m->mv->sel_time : -1, m->mv->sel_duration);
                break;
            case 35: if (p) wr8(p, c->play_selection); break;
            case 36: c->use_badge = ((uint32_t)p & 0xFF) || ((uint32_t)p >> 8 & 0xFF); break;
            case 37: if (p) wr8(p, c->use_badge); break;
            case 40: c->every_frame = ((uint32_t)p & 0xFF) || ((uint32_t)p >> 8 & 0xFF); break;
            case 41: if (p) wr8(p, c->every_frame); break;
            case 43: case 44: case 45: case 60: case 65: break;    /* balloon, badge click, movie click, custom button, cursor */
            case mcActionSuspend: case mcActionResume: c->active = action == mcActionResume; break;
            case 48: c->ctl_keys_enabled = ((uint32_t)p & 0xFF) || ((uint32_t)p >> 8 & 0xFF); break;
            case 49: if (p) qrect_put(p, bar_rect(c)); break;      /* mcActionGetTimeSliderRect */
            case 51: if (p) wr8(p, c->drag_enabled); break;
            case 52: c->drag_enabled = ((uint32_t)p & 0xFF) || ((uint32_t)p >> 8 & 0xFF); break;
            case 55: /* mcActionPrerollAndPlay */
            case 83: /* mcActionAutoPlay */
                if (m) {
                    tb_PrerollMovie(m->handle, 0, (int32_t)p);
                    mc_play(c, (int32_t)p ? (int32_t)p : 0x10000);
                }
                break;
            case 56: if (p) wr8(p, c->cursor_enabled); break;
            case 57: c->cursor_enabled = ((uint32_t)p & 0xFF) || ((uint32_t)p >> 8 & 0xFF); break;
            case 58: if (m) tb_SetMovieColorTable((int32_t)m->handle, (int32_t)p); break;
            case 59: /* mcActionLinkToURL: a handle with the URL, to the program the user chose for it */
                if (p && rd32(p)) {
                    int32_t n = tb_GetHandleSize(p);
                    ic_launch_url(rd32(p), n && rd8(rd32(p) + (uint32_t)n - 1) == 0 ? n - 1 : n, 0, 0);
                }
                break;
            case 67: if (p) wr8(p, 0); break;                       /* mcActionClickAndHoldPoint: none */
            case 68: case 69: break;                                /* message and status strings: no status line */
            case 71: /* mcActionGetChapterTime(QTGetChapterTimeRecord: name, TimeRecord) */
                if (!p || !m || !qt_chapter_time(m->mv, p, p + 256))
                    result = paramErr;
                break;
            case 80: /* mcActionGetIndChapter(QTChapterInfoRec: index, time, name) */
                if (!p || !m || !qt_ind_chapter(m->mv, (int32_t)rd32(p), p + 4, p + 8))
                    result = paramErr;
                break;
            case 82: c->volume_step = (int16_t)(int32_t)p; break;  /* mcActionSetVolumeStep */
            case 84: if (m) mc_play(c, 0); break;                  /* mcActionPauseToBuffer: (never short) */
            case 90: /* mcActionGetMovieName(Str255 handle): the movie's name from its user data */
                if (p && m) {
                    gaddr text = tb_NewHandle(0, false, false);
                    bool got = tb_GetUserDataText((int32_t)m->mv->udata, (int32_t)text, (int32_t)0xA96E616D, 1, 0) == noErr;
                    int32_t n = got ? tb_GetHandleSize(text) : 0;
                    if (n > 255)
                        n = 255;
                    tb_SetHandleSize(p, n + 1);
                    if (rd32(p)) {
                        wr8(rd32(p), (uint8_t)n);
                        if (n)
                            gmemcpy(rd32(p) + 1, rd32(text), (uint32_t)n);
                    }
                    tb_DisposeHandle(text);
                }
                break;
            case 91: if (p) wr32(p, 0); break;                     /* mcActionGetMovieID */
            case 92: if (p) wr8(p, m && m->active); break;         /* mcActionGetMovieActive */
            case 62: case 66: case 85: break;                      /* internal ones and app messages */
            case 63: case 64: case 72: case 73: case 74: case 75: case 76: case 78: case 79: case 81: case 89:
            case 70: /* wired actions, expressions and scripts: none in our movies' playing */
                result = 0;
                break;
            default: break;
            }
            break;
        }
        case 10: c->attached = (rd16(a) >> 8) != 0; break; /* MCSetControllerAttached */
        case 11: result = c->attached; break;
        case 12: c->port = rd32(a); break;                /* MCSetControllerPort */
        case 13: result = (int32_t)c->port; break;
        case 14: c->visible = (rd16(a) >> 8) != 0; break; /* MCSetVisible */
        case 15: result = c->visible; break;
        case 16: qrect_put(rd32(a), c->bounds); break;    /* MCGetControllerBoundsRect */
        case 17: place(c, qrect_get(rd32(a))); break;     /* MCSetControllerBoundsRect */
        case 18:                                          /* MCGetControllerBoundsRgn */
        case 19: {                                        /* MCGetWindowRgn */
            gaddr rgn = tb_NewRgn();
            gaddr r = tb_NewPtr(8, false, false);
            qrect_put(r, c->bounds);
            tb_RectRgn(rgn, r);
            tb_DisposePtr(r);
            result = (int32_t)rgn;
            break;
        }
        case 20: draw_bar(c); break;                      /* MCMovieChanged */
        case 22:                                          /* MCGetCurrentTime(scale*) */
            if (rd32(a))
                wr32(rd32(a), (uint32_t)(m ? m->mv->scale : 600));
            result = m ? (int32_t)(movie_ms(m) * m->mv->scale / 1000) : 0;
            break;
        case 24: mc_draw(c); break;                       /* MCDraw(w) */
        case 25: c->active = (rd16(a) >> 8) != 0; break; /* MCActivate(w, activate) */
        case 26: mc_idle(c); break;                       /* MCIdle */
        case 27:                                          /* MCKey(key, modifiers) */
            if (c->active && (rd16(a + 4) & 0xFF) == ' ' && m) {
                mc_play(c, m->playing ? 0 : m->mv->pref_rate);
                result = 1;
            }
            break;
        case 28: result = mc_click_with(c, rd32(a + 8), rd32(a) & 0x0200); break; /* MCClick(w, where, when, mods) */
        case 29: c->editing = (rd16(a) >> 8) != 0; break; /* MCEnableEditing */
        case 30: result = c->editing; break;               /* MCIsEditingEnabled */
        case 31: result = c->editing ? tb_CopyMovieSelection((int32_t)c->movie) : 0; break; /* MCCopy */
        case 32:                                           /* MCCut */
            result = c->editing ? tb_CutMovieSelection((int32_t)c->movie) : 0;
            draw_bar(c);
            break;
        case 33: {                                         /* MCPaste(srcMovie): nil, the scrap's */
            if (!c->editing)
                break;
            int32_t src = (int32_t)rd32(a), temp = 0;
            if (!src)
                src = temp = tb_NewMovieFromScrap(0);
            if (src)
                tb_PasteMovieSelection((int32_t)c->movie, src);
            if (temp)
                tb_DisposeMovie((gaddr)temp);
            if (m)
                draw_frame(m, true);
            draw_bar(c);
            break;
        }
        case 34:                                           /* MCClear */
            if (c->editing)
                tb_ClearMovieSelection((int32_t)c->movie);
            if (m)
                draw_frame(m, true);
            draw_bar(c);
            break;
        case 35:                                           /* MCUndo */
            if (c->editing && undo_edit(m)) {
                draw_frame(m, true);
                draw_bar(c);
            }
            break;
        case 43: {                                         /* MCSetUpEditMenu(modifiers, mh) */
            gaddr mh = rd32(a);
            bool sel = m && m->mv->sel_duration > 0 && c->editing;
            if (mh) {
                bool on[7] = { false, c->editing && undo_moov && m && undo_movie == m->handle, false, sel, sel,
                               c->editing && scrap_moov, sel };
                for (int i = 1; i <= 6; i++)
                    if (i != 2) {
                        if (on[i])
                            tb_EnableItem(mh, (int16_t)i);
                        else
                            tb_DisableItem(mh, (int16_t)i);
                    }
            }
            break;
        }
        case 44: {                                         /* MCGetMenuString(modifiers, item, string) */
            gaddr s = rd32(a);
            int16_t item = (int16_t)rd16(a + 4);
            const char *t = item == 1 ? "Undo" : item == 3 ? "Cut" : item == 4 ? "Copy" : item == 5 ? "Paste" : "Clear";
            if (s) {
                wr8(s, (uint8_t)strlen(t));
                gput(s + 1, t, (uint32_t)strlen(t));
            }
            break;
        }
        case 36: {                                        /* MCPositionController(movieRect, ctrlRect, flags) */
            gaddr mr = rd32(a + 8);
            if (mr)
                place(c, qrect_get(mr));
            break;
        }
        case 37: {                                        /* MCGetControllerInfo(flags*) */
            int32_t f = 0;
            if (m && m->playing)
                f |= mcInfoIsPlaying;
            if (m && m->looping)
                f |= mcInfoIsLooping;
            if (c->palindrome)
                f |= mcInfoIsInPalindrome;
            if (c->editing) {
                f |= mcInfoEditingEnabled;
                if (undo_moov && m && undo_movie == m->handle)
                    f |= mcInfoUndoAvailable;
                if (m && m->mv->sel_duration > 0)
                    f |= mcInfoCutAvailable | mcInfoCopyAvailable | mcInfoClearAvailable;
                if (scrap_moov)
                    f |= mcInfoPasteAvailable;
            }
            for (int k = 0; m && k < m->mv->ntracks; k++)
                if (m->mv->tracks[k]->media && m->mv->tracks[k]->media->type == fourcc("soun"))
                    f |= mcInfoHasSound;
            wr32(rd32(a), (uint32_t)f);
            break;
        }
        case 21: c->duration = (int32_t)rd32(a); break;      /* MCSetDuration */
        case 23: {                                        /* MCNewAttachedController(m, w, where) */
            c->movie = rd32(a + 8);
            c->port = rd32(a + 4) ? rd32(a + 4) : c->port;
            c->attached = true;
            place(c, (qrect){ (int16_t)pt_v(rd32(a)), (int16_t)pt_h(rd32(a)), 0, 0 });
            break;
        }
        case 40: {                                        /* MCSetClip(theClip, movieClip) */
            if (c->clip)
                tb_DisposeRgn(c->clip);
            c->clip = 0;
            if (rd32(a + 4)) {
                c->clip = tb_NewRgn();
                tb_CopyRgn(rd32(a + 4), c->clip);
            }
            if (m)
                tb_SetMovieDisplayClipRgn((int32_t)m->handle, (int32_t)rd32(a));
            break;
        }
        case 41: {                                        /* MCGetClip(*theClip, *movieClip) */
            if (rd32(a + 4)) {
                gaddr r = 0;
                if (c->clip) {
                    r = tb_NewRgn();
                    tb_CopyRgn(c->clip, r);
                }
                wr32(rd32(a + 4), r);
            }
            if (rd32(a))
                wr32(rd32(a), m ? (uint32_t)tb_GetMovieDisplayClipRgn((int32_t)m->handle) : 0);
            break;
        }
        case 42: {                                        /* MCDrawBadge(movieRgn, *badgeRgn): a little film frame */
            qrect b = m ? m->box : c->bounds;
            qrect r = { (int16_t)(b.bottom - 14), (int16_t)(b.left + 2), (int16_t)(b.bottom - 2), (int16_t)(b.left + 16) };
            gaddr rp = tb_NewPtr(8, false, false), saved = qd_port();
            qrect_put(rp, r);
            tb_SetPort(c->port);
            tb_EraseRect(rp);
            tb_FrameRect(rp);
            tb_SetPort(saved);
            if (rd32(a)) {
                gaddr rgn = tb_NewRgn();
                tb_RectRgn(rgn, rp);
                wr32(rd32(a), rgn);
            }
            tb_DisposePtr(rp);
            break;
        }
        case 47: mc_draw(c); break;                       /* MCInvalidate(w, rgn) */
        case 48: result = 0; break;                       /* MCAdjustCursor(w, where, mods): the arrow stays */
        case 49: {                                        /* MCGetInterfaceElement(which, element) */
            int32_t which = (int32_t)rd32(a + 4);
            gaddr el = rd32(a);
            if (which == 257 && el)                       /* kMCIEIsFlatAppearance */
                wr8(el, 0);
            else if (which == 256 && el)                  /* kMCIEColorTable */
                wr32(el, 0);
            else
                result = paramErr;
            break;
        }
        case 50:                                          /* MCGetDoActionsProc(*proc, *refcon) */
            if (rd32(a + 4))
                wr32(rd32(a + 4), mc_do_actions_stub());
            if (rd32(a))
                wr32(rd32(a), c->ci);
            break;
        case 51: {                                        /* MCAddMovieSegment(src, scaled) */
            int32_t src = (int32_t)rd32(a + 2);
            if (!c->editing || !m || !src) {
                result = paramErr;
                break;
            }
            remember(m);
            if ((rd16(a) >> 8) && m->mv->sel_duration > 0) { /* scaled into the selection */
                int32_t at = m->mv->sel_time, sd = m->mv->sel_duration;
                insert_segment(movie_of((gaddr)src), m, 0, movie_of((gaddr)src)->mv->duration, at);
                int32_t inserted = (int32_t)(movie_of((gaddr)src)->mv->duration * m->mv->scale / movie_of((gaddr)src)->mv->scale);
                tb_ScaleMovieSegment((int32_t)m->handle, at, inserted, sd);
            } else {
                tb_AddMovieSelection((int32_t)m->handle, src);
            }
            draw_frame(m, true);
            draw_bar(c);
            break;
        }
        case 52:                                          /* MCTrimMovieSegment: all but the selection goes */
            if (!c->editing || !m || m->mv->sel_duration <= 0) {
                result = paramErr;
                break;
            }
            remember(m);
            delete_segment(m, m->mv->sel_time + m->mv->sel_duration, m->mv->duration);
            delete_segment(m, 0, m->mv->sel_time);
            m->mv->sel_time = 0;
            draw_frame(m, true);
            draw_bar(c);
            break;
        case 46: {                                        /* MCPtInController(pt, Boolean*) */
            uint32_t pt = rd32(a + 4);
            int h = pt_h(pt), v = pt_v(pt);
            wr8(rd32(a), (uint8_t)(v >= c->bounds.top && v < c->bounds.bottom && h >= c->bounds.left &&
                                   h < c->bounds.right));
            break;
        }
        default: break;
        }
    }
    wr32(cpu->a[7], (uint32_t)result);
    cpu_push32(cpu, ret);
}

/* The controller's own action procedure (MCGetDoActionsProc): pascal
 * OSErr (long refcon, short action, void *params), the refcon the
 * controller; its result is MCDoAction's. */
static void mc_do_actions(cpu68k *cpu, uint16_t tw)
{
    (void)tw;
    uint32_t ret = cpu_pop32(cpu);
    gaddr params = cpu_pop32(cpu);
    int16_t action = (int16_t)cpu_pop16(cpu);
    uint32_t ci = cpu_pop32(cpu);
    gaddr p = tb_NewPtr(12, false, true);
    /* MCDoAction through the controller's entry: ComponentParameters
     * (flags, size, what), the action's params, then the action, then
     * the instance */
    wr8(p + 1, 6);
    wr16(p + 2, 9);
    wr32(p + 4, params);
    wr16(p + 8, (uint16_t)action);
    wr32(p + 10, ci);
    int32_t r = components_call(ci, p);
    tb_DisposePtr(p);
    wr16(cpu->a[7], (uint16_t)r);
    cpu_push32(cpu, ret);
}

static gaddr do_actions_stub;

static gaddr mc_do_actions_stub(void)
{
    if (!do_actions_stub)
        do_actions_stub = native_register(mc_do_actions, "MCDoActionsProc", 0);
    return do_actions_stub;
}

gaddr tb_NewMovieController(gaddr movie_h, gaddr rect, int32_t flags)
{
    if (!mc_component) {
        uint32_t entry = native_register(mc_entry, "MovieController", 0);
        mc_component = components_register_native(fourcc("play"), 0, fourcc("appl"), entry);
    }
    int i = 0;
    while (i < MAX_CONTROLLERS && controllers[i].used)
        i++;
    if (i == MAX_CONTROLLERS)
        return 0;
    uint32_t ci = components_open(mc_component);
    if (!ci)
        return 0;
    controller *c = &controllers[i];
    memset(c, 0, sizeof *c);
    c->used = true;
    c->ci = ci;
    c->movie = movie_h;
    c->flags = flags;
    c->visible = !(flags & mcNotVisible);
    c->attached = true;
    c->active = true;
    c->drawn_x = c->drawn_playing = -1;
    c->keys_enabled = c->ctl_keys_enabled = c->cursor_enabled = true;
    c->volume_step = 64;
    c->use_badge = (flags & 4) != 0; /* mcWithBadge */
    movie *m = movie_of(movie_h);
    c->port = m ? m->port : qd_port();
    place(c, rect ? qrect_get(rect) : (qrect){ 0, 0, 0, 0 });
    return ci;
}

void tb_DisposeMovieController(gaddr ci)
{
    controller *c = controller_of(ci);
    if (c)
        c->used = false;
    components_close(ci);
}

/* A new run: nothing open. */
void quicktime_reset(void)
{
    for (int i = 0; i < MAX_MOVIES; i++)
        if (movies[i].used && movies[i].temp_file && movies[i].play_path)
            unlink(movies[i].play_path);
    memset(movies, 0, sizeof movies); /* (their descriptions' guest handles went with the old heap) */
    memset(timebases, 0, sizeof timebases);
    memset(controllers, 0, sizeof controllers);
    mc_component = 0;
    do_actions_stub = 0;
    movies_error = sticky_error = 0;
    qt_movies_reset();
    qt_callbacks_reset();
    qt_util_reset();
    qt_vr_reset();
}

/* ---------------------------------------------------------------------- */
/* Full screen */

/* BeginFullScreen: the screen keeps its size (which is the answer), and a
 * window covering it, if one is asked for, is filled with the erase colour
 * (black by default).  The state is that window. */
int16_t tb_BeginFullScreen(gaddr restoreState, int32_t whichGD, gaddr desiredWidth, gaddr desiredHeight,
                           gaddr newWindow, gaddr eraseColor, int32_t flags)
{
    (void)whichGD;
    if (desiredWidth)
        wr16(desiredWidth, (uint16_t)g_screen_width);
    if (desiredHeight)
        wr16(desiredHeight, (uint16_t)g_screen_height);
    if (flags & 8) /* fullScreenPreflightSize: only the size */
        return noErr;
    gaddr state = tb_NewPtr(8, true, false);
    if (!state)
        return memFullErr;
    if (newWindow) {
        gaddr r = tb_NewPtr(8 + 2 + 6, true, false);
        wr16(r + 4, (uint16_t)g_screen_height);
        wr16(r + 6, (uint16_t)g_screen_width);
        gaddr title = r + 8, black = r + 10;
        gaddr w = tb_NewCWindow(0, r, title, true, 2 /* plainDBox */, 0xFFFFFFFFu, false, 0);
        if (w) {
            gaddr saved = tb_NewPtr(4, false, false);
            tb_GetPort(saved);
            tb_SetPort(w);
            tb_RGBBackColor(eraseColor ? eraseColor : black);
            gaddr all = tb_NewPtr(8, true, false);
            wr16(all + 4, (uint16_t)g_screen_height);
            wr16(all + 6, (uint16_t)g_screen_width);
            tb_EraseRect(all);
            tb_DisposePtr(all);
            tb_SetPort(rd32(saved));
            tb_DisposePtr(saved);
        }
        tb_DisposePtr(r);
        wr32(newWindow, w);
        wr32(state, w);
    }
    if (restoreState)
        wr32(restoreState, state);
    return noErr;
}

int16_t tb_EndFullScreen(int32_t fullState, int32_t flags)
{
    (void)flags;
    gaddr state = (gaddr)fullState;
    if (state) {
        if (rd32(state))
            tb_DisposeWindow(rd32(state));
        tb_DisposePtr(state);
    }
    return noErr;
}


/* ---------------------------------------------------------------------- */
/* bluebox movie-speed: how fast our decoders play a movie.  Its pictures
 * and sound are decoded back to back from start to end, as playing it
 * would, and timed against its own length. */

static uint32_t mvhd_ms(const uint8_t *moov, uint32_t len)
{
    for (uint32_t at = 8; at + 8 <= len;) {
        uint32_t size = be32(moov + at);
        if (size < 8 || at + size > len)
            break;
        if (!memcmp(moov + at + 4, "mvhd", 4) && size >= 28) {
            uint32_t scale = be32(moov + at + 20), duration = be32(moov + at + 24);
            return scale ? (uint32_t)((uint64_t)duration * 1000 / scale) : 0;
        }
        at += size;
    }
    return 0;
}

bool machine_movie_speed(const char *path, machine_movie_speed_result *r, char *err, size_t errlen)
{
    memset(r, 0, sizeof *r);
    uint32_t len = 0;
    int16_t id = 0;
    uint8_t *moov = moov_resource(path, &id, &len);
    if (!moov)
        moov = moov_in_data(path, 0, &len);
    if (moov)
        moov = uncompress_moov(moov, &len);
    if (!moov) {
        snprintf(err, errlen, "no movie in %s", path);
        return false;
    }
    r->length_ms = mvhd_ms(moov, len);
    qt_native *p = qt_native_open(moov, len, path, 600, NULL, 0);
    free(moov);
    if (!p) {
        snprintf(err, errlen, "a track our decoders don't play (VLC would)");
        return false;
    }
    r->video = qt_native_has_video(p);
    r->sound = qt_native_has_sound(p);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint32_t serial = 0;
    uint8_t *rgba = NULL;
    int w = 0, h = 0;
    for (int64_t ms = 0; ms < r->length_ms; ms += 100) {
        for (int64_t f = ms; r->video && f < ms + 100; f += 5)
            if (qt_native_frame(p, f, &serial, &rgba, &w, &h))
                r->frames++;
        int16_t *pcm = NULL;
        int ch = 0;
        double rate = 0;
        if (r->sound)
            qt_native_sound(p, ms, ms + 100, &pcm, &ch, &rate);
        free(pcm);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    free(rgba);
    qt_native_close(p);
    r->width = w;
    r->height = h;
    r->decode_ms = (double)(t1.tv_sec - t0.tv_sec) * 1000 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
    return true;
}
