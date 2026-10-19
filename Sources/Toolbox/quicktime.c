/*
 * QuickTime's Movie Toolbox, with movies decoded by libvlc (qt_vlc.c).
 *
 * A movie's description (its 'moov' atom: time scale, duration, tracks,
 * natural size) is read here, from the file's resource fork as classic
 * movies kept it or from the data fork of a flattened one, so timing is
 * right with or without VLC.  For playing, VLC gets the file itself when it
 * is flattened, or else a temporary flattened copy: the data fork behind an
 * 'mdat' header, then the 'moov' atom with its chunk offsets moved to match.
 * With a host attached and libvlc available, frames are drawn into the
 * movie's GWorld when the application gives the movie time (MoviesTask,
 * UpdateMovie, the movie controller) and VLC plays the sound; otherwise
 * (headless runs) movies keep time without pictures or sound.
 *
 * Movies whose codecs we have (Cinepak, Animation, Graphics, Apple Video,
 * None; uncompressed, IMA and MACE sound) play through our own decoders
 * instead (qt_native.c), headless runs included: frames for the movie's
 * clock, and sound sent to the host a second ahead.  VLC is the fallback.
 *
 * The movie controller is a component of our own: a play/pause button and a
 * time slider under the movie, with the standard actions and action filters.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include "forks.h"
#include "m68k.h"
#include "qd_internal.h"
#include "qt_edit.h"
#include "qt_native.h"
#include "qt_vlc.h"
#include "tb_internal.h"
#include "trap.h"

enum {
    noMovieFound = -2048, invalidMovie = -2010, invalidTrack = -2009, badImageDescription = -2001,
    movieToolboxUninitialized = -2020,
};
enum { newMovieActive = 1 };

static int16_t movies_error, sticky_error;

static int16_t set_error(int16_t err)
{
    movies_error = err;
    if (err && !sticky_error)
        sticky_error = err;
    return err;
}

/* ---------------------------------------------------------------------- */
/* Movies */

#define MAX_MOVIES 32
#define MAX_TRACKS 16

typedef struct {
    uint32_t type; /* 'vide', 'soun', ... */
    int16_t width, height;
    gaddr handle;  /* the Track (and its Media, handle + 2) */
    bool disabled; /* SetTrackEnabled(false) */
} track;

typedef struct {
    bool used;
    gaddr handle;        /* the Movie the application holds */
    char *play_path;     /* what VLC plays */
    bool temp_file;      /* play_path is ours to delete */
    int32_t scale, duration;
    qrect natural, box;
    gaddr port, gdh;
    bool active, playing, looping;
    int32_t rate, pref_rate; /* Fixed */
    int16_t volume, pref_volume;
    int64_t base_ms;     /* time when stopped, or at start_ns */
    uint64_t start_ns;
    qt_vlc_player *player;
    qt_native *native;   /* our decoders, when they cover the movie */
    uint8_t *moov;       /* the description, edited in place of the file's */
    uint32_t moov_len;
    char *data_path;     /* the file holding the samples */
    int32_t sel_start, sel_duration; /* the selection, movie time units */
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
    track tracks[MAX_TRACKS];
    int ntracks;
} movie;

/* Whether any enabled track has this media type (or there are no tracks of
 * it to go by). */
static bool plays(const movie *m, const char *type)
{
    bool any = false;
    for (int k = 0; k < m->ntracks; k++)
        if (m->tracks[k].type == fourcc(type)) {
            any = true;
            if (!m->tracks[k].disabled)
                return true;
        }
    return !any;
}

/* VLC's volume (0-100) for the movie's: silent with its sound disabled. */
static int vlc_volume(const movie *m)
{
    return m->volume < 0 || !plays(m, "soun") ? 0 : m->volume * 100 / 256;
}

static movie movies[MAX_MOVIES];

static movie *movie_of(gaddr h)
{
    for (int i = 0; h && i < MAX_MOVIES; i++)
        if (movies[i].used && movies[i].handle == h)
            return &movies[i];
    return NULL;
}

static movie *movie_of_track(gaddr t)
{
    for (int i = 0; t && i < MAX_MOVIES; i++)
        for (int k = 0; movies[i].used && k < movies[i].ntracks; k++)
            if (movies[i].tracks[k].handle == t || movies[i].tracks[k].handle + 2 == t)
                return &movies[i];
    return NULL;
}

static track *track_of(gaddr t)
{
    movie *m = movie_of_track(t);
    for (int k = 0; m && k < m->ntracks; k++)
        if (m->tracks[k].handle == t || m->tracks[k].handle + 2 == t)
            return &m->tracks[k];
    return NULL;
}

/* Pictures and sound only with a person to see and hear them. */
static bool use_vlc(void) { return events_host_attached() || getenv("BLUEBOX_QUICKTIME_HEADLESS"); }

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

/* A video track whose first sample description is palettized (1 to 8 bits)
 * without a colour table of its own: QuickTime copied such pixels to a
 * screen of that depth as they are, so they take the screen's colours. */
static void index_depth(movie *m, uint8_t *mdia, uint32_t len)
{
    static const char *const path[] = { "minf", "stbl", "stsd" };
    uint8_t *p = mdia, *end = mdia + len;
    uint32_t n = len;
    for (int i = 0; i < 3 && p; i++) {
        p = find_atom(p + 8, end, path[i], &n);
        end = p ? p + n : NULL;
    }
    if (!p || n < 16 + 86 || be32(p + 12) < 1)
        return;
    uint8_t *e = p + 16;
    int16_t depth = (int16_t)(e[82] << 8 | e[83]), ctab = (int16_t)(e[84] << 8 | e[85]);
    if ((depth == 1 || depth == 2 || depth == 4 || depth == 8) && ctab != 0)
        m->index_depth = depth;
}

/* Read time scale, duration and tracks from a 'moov' atom. */
static bool describe(movie *m, uint8_t *moov, uint32_t len)
{
    uint8_t *end = moov + len;
    uint32_t n;
    uint8_t *mvhd = find_atom(moov + 8, end, "mvhd", &n);
    if (!mvhd || n < 32)
        return false;
    if (mvhd[8] == 1 && n >= 44) { /* 64-bit times */
        m->scale = (int32_t)be32(mvhd + 28);
        m->duration = (int32_t)be32(mvhd + 36);
    } else {
        m->scale = (int32_t)be32(mvhd + 20);
        m->duration = (int32_t)be32(mvhd + 24);
    }
    if (m->scale <= 0)
        m->scale = 600;
    int w = 0, h = 0;
    for (uint8_t *t = moov + 8; t + 8 <= end;) {
        uint32_t tn = be32(t);
        if (tn < 8 || t + tn > end)
            break;
        if (!memcmp(t + 4, "trak", 4) && m->ntracks < MAX_TRACKS) {
            track *tr = &m->tracks[m->ntracks++];
            uint32_t k;
            uint8_t *tkhd = find_atom(t + 8, t + tn, "tkhd", &k);
            int right = 0, bottom = 0; /* as far as the track's matrix puts it */
            if (tkhd && k >= 92) {
                int off = tkhd[8] == 1 ? 96 : 84;
                if ((uint32_t)off + 8 <= k) {
                    tr->width = (int16_t)(be32(tkhd + off) >> 16);
                    tr->height = (int16_t)(be32(tkhd + off + 4) >> 16);
                    const uint8_t *mx = tkhd + off - 36;
                    int64_t sx = (int32_t)be32(mx), sy = (int32_t)be32(mx + 16);
                    if (!sx && !sy) /* no matrix at all: as the identity */
                        sx = sy = 0x10000;
                    right = (int)((tr->width * sx + (int32_t)be32(mx + 24) + 0x8000) >> 16);
                    bottom = (int)((tr->height * sy + (int32_t)be32(mx + 28) + 0x8000) >> 16);
                }
            }
            uint8_t *mdia = find_atom(t + 8, t + tn, "mdia", &k);
            uint32_t hk;
            uint8_t *hdlr = mdia ? find_atom(mdia + 8, mdia + k, "hdlr", &hk) : NULL;
            if (hdlr && hk >= 20)
                tr->type = be32(hdlr + 16);
            if (tr->type == fourcc("vide"))
                index_depth(m, mdia, k);
            if (tr->type == fourcc("vide") || tr->type == fourcc("text") || tr->type == fourcc("sprt")) {
                if (right > w)
                    w = right;
                if (bottom > h)
                    h = bottom;
            }
        }
        t += tn;
    }
    m->natural = (qrect){ 0, 0, (int16_t)h, (int16_t)w };
    return true;
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
static uint8_t *moov_in_data(const char *path, uint32_t offset, uint32_t *len)
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
 * 'cmvd' the uncompressed size and the data) as the 'moov' atom it holds,
 * so the movie's size and length are known and VLC can play it (QuickTime
 * 2.5 itself refused these).  Takes moov; NULL if it can't be read. */
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

static movie *new_movie(void)
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
        m->handle = tb_NewHandle(4, false, true);
        wr32(rd32(m->handle), 0x6D6F6F76); /* 'moov', for anyone looking */
        m->scale = 600;
        m->rate = m->pref_rate = 0x00010000;
        m->volume = m->pref_volume = 0x0100;
        m->port = qd_port();
        m->gdh = tb_GetGDevice();
        return m;
    }
    return NULL;
}

static void stop_sound(movie *m);

static void free_movie(movie *m)
{
    stop_sound(m);
    qt_native_close(m->native);
    free(m->moov);
    free(m->data_path);
    if (m->player)
        qt_vlc_close(m->player);
    if (m->temp_file && m->play_path)
        unlink(m->play_path);
    free(m->play_path);
    free(m->frame);
    for (int k = 0; k < m->ntracks; k++)
        if (m->tracks[k].handle)
            tb_DisposeHandle(m->tracks[k].handle);
    if (m->handle)
        tb_DisposeHandle(m->handle);
    m->used = false;
}

/* Our decoders for the movie's description (none: VLC, or timing alone).
 * Palettized video without colours of its own takes the screen's. */
static void open_native(movie *m)
{
    qt_native_close(m->native);
    m->native = NULL;
    if (!m->moov || !m->data_path)
        return;
    uint32_t screen[256];
    pixdst d = qd_port_pixels(m->port);
    int sd = d.depth <= 8 ? d.depth : 0;
    for (int v = 0; sd && v < (1 << sd); v++) {
        uint16_t c[3];
        qd_px_rgb(sd, d.ctab, (uint32_t)v, c);
        screen[v] = (uint32_t)(c[0] >> 8) << 16 | (uint32_t)(c[1] >> 8) << 8 | (c[2] >> 8);
    }
    m->native = qt_native_open(m->moov, m->moov_len, m->data_path, m->scale, sd ? screen : NULL, sd);
}

/* A movie from a host file: the description from the resource fork (resid,
 * 0 for the first) or the data fork at offset; flattened for playing. */
static int16_t open_movie(const char *path, int16_t *resid, uint32_t data_offset, bool from_data, gaddr moviep)
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
    moov = uncompress_moov(moov, &len);
    if (!moov)
        return set_error(invalidMovie);
    movie *m = new_movie();
    if (!m) {
        free(moov);
        return set_error(memFullErr);
    }
    describe(m, moov, len);
    m->box = m->natural;
    for (int k = 0; k < m->ntracks; k++) {
        m->tracks[k].handle = tb_NewHandle(4, false, true);
    }
    m->moov = moov;
    m->moov_len = len;
    m->data_path = strdup(path);
    open_native(m);
    LOGF(LOG_TRAPS, "  movie %s: %s\n", path, m->native ? "our decoders" : "VLC");
    if (m->native) {
        m->play_path = NULL;
    } else if (flattened) {
        m->play_path = strdup(path);
    } else { /* (flatten moves the copy's chunk offsets) */
        uint8_t *copy = malloc(len);
        if (copy) {
            memcpy(copy, moov, len);
            m->play_path = flatten(path, copy, len);
            free(copy);
        }
        m->temp_file = m->play_path != NULL;
    }
    if (resid)
        *resid = id;
    wr32(moviep, m->handle);
    return set_error(noErr);
}

/* ---- Time ---- */

static int64_t now_ms(void) { return (int64_t)(machine_time_ns() / 1000000u); }

static int64_t duration_ms(const movie *m) { return (int64_t)m->duration * 1000 / m->scale; }

static int64_t movie_ms(movie *m)
{
    int64_t t;
    if (m->player && m->playing)
        t = qt_vlc_time(m->player);
    else if (m->playing)
        t = m->base_ms + (now_ms() - (int64_t)(m->start_ns / 1000000u)) * m->rate / 0x10000;
    else
        t = m->base_ms;
    int64_t d = duration_ms(m);
    return t > d ? d : t < 0 ? 0 : t;
}

static bool movie_done(movie *m)
{
    if (!m->playing)
        return movie_ms(m) >= duration_ms(m);
    if (m->player)
        return qt_vlc_ended(m->player);
    return movie_ms(m) >= duration_ms(m);
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
    if (!m->native || !qt_native_has_music(m->native) || !m->playing || m->rate != 0x10000 || m->volume <= 0)
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
    if (!m->native || !qt_native_has_sound(m->native) || !m->playing || m->rate != 0x10000 || !plays(m, "soun") ||
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
    m->rate = rate;
    if (movie_done(m) && !m->playing)
        m->base_ms = 0; /* playing a finished movie starts it again */
    ensure_player(m);
    if (m->player) {
        if (m->base_ms == 0)
            qt_vlc_rewind(m->player);
        qt_vlc_play(m->player);
        if (m->base_ms)
            qt_vlc_seek(m->player, m->base_ms);
        if (rate != 0x10000)
            qt_vlc_set_rate(m->player, (float)rate / 65536.0f);
    }
    m->start_ns = machine_time_ns();
    m->playing = true;
    feed_sound(m);
    feed_music(m);
}

static void seek(movie *m, int64_t ms)
{
    stop_sound(m);
    m->base_ms = ms;
    m->start_ns = machine_time_ns();
    if (m->player) {
        if (ms == 0 && !m->playing)
            qt_vlc_rewind(m->player);
        else
            qt_vlc_seek(m->player, ms);
    }
}

/* ---- Pictures ---- */

static void draw_frame(movie *m, bool force)
{
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
    tb_SetPort(saved);
}

static void task(movie *m)
{
    if (!m->playing)
        return;
    draw_frame(m, false);
    feed_sound(m);
    feed_music(m);
    if (movie_done(m)) {
        if (m->looping) {
            m->base_ms = 0;
            if (m->player)
                qt_vlc_rewind(m->player);
            start(m, m->rate);
        } else {
            m->base_ms = duration_ms(m);
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
    int16_t err = open_movie(path, &id, 0, false, moviep);
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
    /* The description alone: its data is elsewhere (an alias in its data
     * references), so the movie keeps time without pictures. */
    if (changed)
        wr8(changed, 0);
    wr32(moviep, 0);
    if (!h || !rd32((gaddr)h))
        return set_error(invalidMovie);
    uint32_t len = (uint32_t)tb_GetHandleSize((gaddr)h);
    uint8_t *buf = malloc(len + 8);
    uint8_t *moov = buf;
    gget(buf + 8, rd32((gaddr)h), len);
    if (len >= 8 && !memcmp(buf + 12, "moov", 4)) {
        moov = buf + 8;
    } else {
        put_be32(buf, len + 8);
        memcpy(buf + 4, "moov", 4);
        len += 8;
    }
    movie *m = new_movie();
    if (!m) {
        free(buf);
        return set_error(memFullErr);
    }
    describe(m, moov, moov == buf ? len : len);
    m->box = m->natural;
    for (int k = 0; k < m->ntracks; k++)
        m->tracks[k].handle = tb_NewHandle(4, false, true);
    m->active = (flags & newMovieActive) != 0;
    free(buf);
    wr32(moviep, m->handle);
    return set_error(noErr);
}

int32_t tb_NewMovie(int32_t flags)
{
    movie *m = new_movie();
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
 * Edits change the movie's description, each track's edit list (qt_edit.c),
 * and the movie plays from it again: the samples stay where they are.
 * Pasting takes another movie's media only when it is in the same file
 * (a movie and copies made from it); saving writes the description as the
 * file's 'moov' resource.
 */

enum { invalidDuration = -2014, invalidTime = -2015 };

static bool read_edits(const movie *m, qt_edits *e) { return m->moov && qt_edits_read(m->moov, m->moov_len, e); }

/* Keep the description before an edit, for undo. */
static uint8_t *undo_moov;
static uint32_t undo_len;
static gaddr undo_movie;

static void remember(movie *m)
{
    free(undo_moov);
    undo_moov = malloc(m->moov_len);
    if (undo_moov)
        memcpy(undo_moov, m->moov, m->moov_len);
    undo_len = m->moov_len;
    undo_movie = m->handle;
}

/* The movie with a new description: its length, and its player on it. */
static void take_moov(movie *m, uint8_t *moov, uint32_t len)
{
    free(m->moov);
    m->moov = moov;
    m->moov_len = len;
    qt_edits e;
    if (qt_edits_read(moov, len, &e)) {
        m->duration = (int32_t)qt_edits_duration(&e);
        qt_edits_free(&e);
    }
    stop_sound(m);
    open_native(m);
    m->serial = 0;
    m->changed = true;
    if (m->base_ms > duration_ms(m))
        m->base_ms = duration_ms(m);
}

static bool edited(movie *m, qt_edits *e)
{
    uint32_t n;
    uint8_t *nm = qt_edits_write(m->moov, m->moov_len, e, &n);
    if (!nm)
        return false;
    take_moov(m, nm, n);
    return true;
}

/* A new movie of another's edits (the same file and tracks). */
static movie *movie_from_edits(movie *from, qt_edits *e)
{
    uint32_t n;
    uint8_t *nm = qt_edits_write(from->moov, from->moov_len, e, &n);
    movie *m = nm ? new_movie() : NULL;
    if (!m) {
        free(nm);
        return NULL;
    }
    describe(m, nm, n);
    m->scale = from->scale;
    m->duration = (int32_t)qt_edits_duration(e);
    m->box = m->natural;
    for (int k = 0; k < m->ntracks; k++)
        m->tracks[k].handle = tb_NewHandle(4, false, true);
    m->moov = nm;
    m->moov_len = n;
    m->data_path = from->data_path ? strdup(from->data_path) : NULL;
    open_native(m);
    return m;
}

void tb_GetMovieSelection(int32_t theMovie, gaddr selectionTime, gaddr selectionDuration)
{
    movie *m = movie_of((gaddr)theMovie);
    if (selectionTime)
        wr32(selectionTime, m ? (uint32_t)m->sel_start : 0);
    if (selectionDuration)
        wr32(selectionDuration, m ? (uint32_t)m->sel_duration : 0);
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
    m->sel_start = selectionTime < 0 ? 0 : selectionTime > m->duration ? m->duration : selectionTime;
    m->sel_duration = m->sel_start + selectionDuration > m->duration ? m->duration - m->sel_start : selectionDuration;
}

int32_t tb_CopyMovieSelection(int32_t theMovie)
{
    movie *m = movie_of((gaddr)theMovie);
    qt_edits e, part;
    if (!m || !read_edits(m, &e)) {
        set_error(invalidMovie);
        return 0;
    }
    bool ok = qt_edits_copy(&e, m->sel_start, m->sel_duration, &part);
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
    if (copy && m && m->sel_duration > 0) {
        remember(m);
        delete_segment(m, m->sel_start, m->sel_duration);
        m->sel_duration = 0;
    }
    return copy;
}

void tb_ClearMovieSelection(int32_t theMovie)
{
    movie *m = movie_of((gaddr)theMovie);
    if (!m || m->sel_duration <= 0)
        return;
    remember(m);
    set_error(delete_segment(m, m->sel_start, m->sel_duration) ? noErr : invalidMovie);
    m->sel_duration = 0;
}

static int16_t insert_segment(movie *src, movie *dst, int64_t src_in, int64_t len, int64_t dst_in)
{
    if (!src || !dst)
        return invalidMovie;
    if (!src->data_path || !dst->data_path || strcmp(src->data_path, dst->data_path)) {
        LOGF(LOG_UNIMPL, "QuickTime: pasting a movie from another file is not done\n");
        return paramErr;
    }
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
    if (m->sel_duration > 0)
        delete_segment(m, m->sel_start, m->sel_duration);
    int16_t err = insert_segment(s, m, 0, s->duration, m->sel_start);
    if (!err)
        m->sel_duration = (int32_t)((int64_t)s->duration * m->scale / s->scale);
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
    if (startTime < 0 || startTime > m->duration)
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
    if (!m || !m->moov)
        return set_error(invalidMovie);
    free(scrap_moov);
    free(scrap_path);
    scrap_moov = malloc(m->moov_len);
    if (!scrap_moov)
        return set_error(memFullErr);
    memcpy(scrap_moov, m->moov, m->moov_len);
    scrap_len = m->moov_len;
    scrap_path = m->data_path ? strdup(m->data_path) : NULL;
    if (!(movieScrapFlags & 1)) /* movieScrapDontZeroScrap */
        tb_ZeroScrap();
    gaddr p = tb_NewPtr((int32_t)m->moov_len, false, false);
    if (p) {
        gput(p, m->moov, m->moov_len);
        tb_PutScrap((int32_t)m->moov_len, 0x6D6F6F76, p);
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
    movie *m = new_movie();
    if (!m) {
        set_error(memFullErr);
        return 0;
    }
    describe(m, scrap_moov, scrap_len);
    m->box = m->natural;
    for (int k = 0; k < m->ntracks; k++)
        m->tracks[k].handle = tb_NewHandle(4, false, true);
    m->moov = malloc(scrap_len);
    if (m->moov) {
        memcpy(m->moov, scrap_moov, scrap_len);
        m->moov_len = scrap_len;
    }
    m->data_path = scrap_path ? strdup(scrap_path) : NULL;
    m->active = (newMovieFlags & newMovieActive) != 0;
    open_native(m);
    set_error(noErr);
    return (int32_t)m->handle;
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
    if (!m || !m->moov)
        return set_error(invalidMovie);
    int16_t saved = tb_CurResFile();
    tb_UseResFile(resRefNum);
    if (tb_CurResFile() != resRefNum) {
        tb_UseResFile(saved);
        return set_error(-51 /* rfNumErr */);
    }
    gaddr h = tb_Get1Resource(0x6D6F6F76, id);
    if (h) {
        tb_SetHandleSize(h, (int32_t)m->moov_len);
        gput(rd32(h), m->moov, m->moov_len);
        tb_ChangedResource(h);
    } else {
        h = tb_NewHandle((int32_t)m->moov_len, false, false);
        if (h) {
            gput(rd32(h), m->moov, m->moov_len);
            gaddr empty = tb_NewPtr(1, false, true);
            tb_AddResource(h, 0x6D6F6F76, id, name ? name : empty);
            tb_DisposePtr(empty);
        }
    }
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
    uint8_t *mv = undo_moov;
    uint32_t len = undo_len;
    undo_moov = NULL;
    take_moov(m, mv, len);
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
    start(m, m->pref_rate ? m->pref_rate : 0x10000);
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
    return m ? m->pref_volume : 0;
}

void tb_SetMoviePreferredVolume(int32_t h, int16_t volume)
{
    movie *m = movie_of((gaddr)h);
    if (m)
        m->pref_volume = volume;
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
    return m ? m->pref_rate : 0x10000;
}

void tb_SetMoviePreferredRate(int32_t h, int32_t rate)
{
    movie *m = movie_of((gaddr)h);
    if (m)
        m->pref_rate = rate;
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
    qrect_put(r, m ? m->natural : (qrect){ 0, 0, 0, 0 });
}

int32_t tb_GetMovieTimeScale(int32_t h)
{
    movie *m = movie_of((gaddr)h);
    return m ? m->scale : 600;
}

int32_t tb_GetMovieDuration(int32_t h)
{
    movie *m = movie_of((gaddr)h);
    return m ? m->duration : 0;
}

/* TimeRecord: value (wide), scale, base. */
int32_t tb_GetMovieTime(int32_t h, gaddr tr)
{
    movie *m = movie_of((gaddr)h);
    if (!m)
        return 0;
    int32_t v = (int32_t)(movie_ms(m) * m->scale / 1000);
    if (tr) {
        wr32(tr, 0);
        wr32(tr + 4, (uint32_t)v);
        wr32(tr + 8, (uint32_t)m->scale);
        wr32(tr + 12, (uint32_t)h);
    }
    return v;
}

void tb_SetMovieTimeValue(int32_t h, int32_t t)
{
    movie *m = movie_of((gaddr)h);
    if (m)
        seek(m, (int64_t)t * 1000 / m->scale);
}

void tb_SetMovieTime(int32_t h, gaddr tr)
{
    movie *m = movie_of((gaddr)h);
    if (!m || !tr)
        return;
    int32_t scale = (int32_t)rd32(tr + 8);
    seek(m, (int64_t)(int32_t)rd32(tr + 4) * 1000 / (scale > 0 ? scale : m->scale));
}

int32_t tb_GetMovieTimeBase(int32_t h) { return h; } /* the movie stands for its time base */

int32_t tb_GetTimeBaseTime(int32_t tb, int32_t scale, gaddr tr)
{
    movie *m = movie_of((gaddr)tb);
    if (!m)
        return 0;
    int32_t v = (int32_t)(movie_ms(m) * (scale > 0 ? scale : m->scale) / 1000);
    if (tr) {
        wr32(tr, 0);
        wr32(tr + 4, (uint32_t)v);
        wr32(tr + 8, (uint32_t)scale);
        wr32(tr + 12, (uint32_t)tb);
    }
    return v;
}

void tb_SetTimeBaseRate(int32_t tb, int32_t rate) { tb_SetMovieRate(tb, rate); }
void tb_SetMovieMasterTimeBase(int32_t h, int32_t tb, gaddr zero) {}

int32_t tb_GetMovieTrackCount(int32_t h)
{
    movie *m = movie_of((gaddr)h);
    return m ? m->ntracks : 0;
}

int32_t tb_GetMovieIndTrack(int32_t h, int32_t index)
{
    movie *m = movie_of((gaddr)h);
    if (!m || index < 1 || index > m->ntracks) {
        set_error(invalidTrack);
        return 0;
    }
    return (int32_t)m->tracks[index - 1].handle;
}

/* flags: 1 the type is a media type, 2 a characteristic ('eyes', 'ears'). */
int32_t tb_GetMovieIndTrackType(int32_t h, int32_t index, int32_t type, int32_t flags)
{
    movie *m = movie_of((gaddr)h);
    for (int k = 0, n = 0; m && k < m->ntracks; k++) {
        uint32_t t = m->tracks[k].type;
        bool match = (flags & 2) ? ((uint32_t)type == fourcc("eyes") && t == fourcc("vide")) ||
                                       ((uint32_t)type == fourcc("ears") && t == fourcc("soun"))
                                 : t == (uint32_t)type;
        if (match && ++n == index)
            return (int32_t)m->tracks[k].handle;
    }
    return 0;
}

int32_t tb_GetTrackMovie(int32_t t)
{
    movie *m = movie_of_track((gaddr)t);
    return m ? (int32_t)m->handle : 0;
}

int8_t tb_GetTrackEnabled(int32_t t)
{
    track *tr = track_of((gaddr)t);
    return tr && !tr->disabled;
}

void tb_SetTrackEnabled(int32_t t, int8_t enabled)
{
    track *tr = track_of((gaddr)t);
    movie *m = movie_of_track((gaddr)t);
    if (!tr || !m)
        return;
    tr->disabled = !enabled;
    if (m->player)
        qt_vlc_set_volume(m->player, vlc_volume(m));
}

int32_t tb_GetTrackMedia(int32_t t)
{
    track *tr = track_of((gaddr)t);
    return tr ? (int32_t)(tr->handle + 2) : 0; /* the media: inside its track's handle */
}

void tb_GetMediaHandlerDescription(int32_t media, gaddr type, int32_t name, gaddr manufacturer)
{
    track *tr = track_of((gaddr)media);
    if (type)
        wr32(type, tr ? tr->type : 0);
    if (name)
        wr8((gaddr)name, 0);
    if (manufacturer)
        wr32(manufacturer, fourcc("appl"));
}

int32_t tb_GetMovieDataSize(int32_t h, int32_t start_time, int32_t dur)
{
    movie *m = movie_of((gaddr)h);
    struct stat st;
    return m && m->play_path && stat(m->play_path, &st) == 0 ? (int32_t)st.st_size : 0;
}

void tb_GetMovieNextInterestingTime(int32_t h, int16_t flags, int16_t ntypes, gaddr types, int32_t time, int32_t rate,
                                    gaddr itime, gaddr idur)
{
    if (itime)
        wr32(itime, (uint32_t)-1); /* nothing more of interest */
    if (idur)
        wr32(idur, 0);
}

int32_t tb_GetMovieStatus(int32_t h, gaddr track_out)
{
    if (track_out)
        wr32(track_out, 0);
    return noErr;
}

int32_t tb_GetMovieCreationTime(int32_t h) { return 0; }
void tb_SetMoviePlayHints(int32_t h, int32_t flags, int32_t mask) {}
void tb_SetMovieDisplayClipRgn(int32_t h, int32_t clip) {}
void tb_ShowMoviePoster(int32_t h) {}
int32_t tb_GetMoviePict(int32_t h, int32_t time) { return 0; }
int32_t tb_GetMoviePosterPict(int32_t h) { return 0; }

/* ---------------------------------------------------------------------- */
/* The movie controller: a component of our own */

enum {
    mcTopLeftMovie = 1, mcScaleMovieToFit = 2, mcNotVisible = 8,
    mcActionIdle = 1, mcActionDraw = 2, mcActionActivate = 3, mcActionDeactivate = 4, mcActionMouseDown = 5,
    mcActionKey = 6, mcActionPlay = 8, mcActionGoToTime = 12, mcActionSetVolume = 14, mcActionGetVolume = 15,
    mcActionStep = 18, mcActionSetLooping = 21, mcActionGetLooping = 22, mcActionSetFlags = 38,
    mcActionGetFlags = 39, mcActionGetPlayRate = 42, mcActionSuspend = 46, mcActionResume = 47,
    mcInfoIsPlaying = 1 << 6, mcInfoIsLooping = 1 << 7, mcInfoHasSound = 1 << 2,
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
} controller;

static controller controllers[MAX_CONTROLLERS];
static uint32_t mc_component;

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
    if (!m || m->duration <= 0 || right <= left)
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
    if (m && m->sel_duration > 0 && m->duration > 0 && right > left) {
        int x0 = left + (int)((int64_t)(right - left) * m->sel_start / m->duration);
        int x1 = left + (int)((int64_t)(right - left) * (m->sel_start + m->sel_duration) / m->duration);
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
    if (!c->visible || !c->attached)
        return;
    int x = thumb_x(c, m), playing = m && m->playing;
    if (playing != c->drawn_playing || c->drawn_x < 0 || x < 0 || (m && m->sel_duration > 0)) {
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
    if (in_bar && h >= b.left + 18) {
        int left = b.left + 18, right = b.right - 6;
        if (right > left) {
            int64_t ms = duration_ms(m) * (h < left ? 0 : h > right ? right - left : h - left) / (right - left);
            if (shift && c->editing) { /* the selection from the current time (or its far end) to here */
                int64_t at = ms * m->scale / 1000, now = movie_ms(m) * m->scale / 1000;
                int64_t anchor = m->sel_duration > 0
                                     ? (llabs(at - m->sel_start) > llabs(at - (m->sel_start + m->sel_duration))
                                            ? m->sel_start
                                            : m->sel_start + m->sel_duration)
                                     : now;
                tb_SetMovieSelection((int32_t)m->handle, (int32_t)(at < anchor ? at : anchor),
                                     (int32_t)llabs(at - anchor));
            } else {
                seek(m, ms);
                if (c->editing)
                    tb_SetMovieSelection((int32_t)m->handle, (int32_t)(ms * m->scale / 1000), 0);
            }
        }
    } else {
        mc_play(c, m->playing ? 0 : (m->pref_rate ? m->pref_rate : 0x10000));
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
    int w = m->natural.right - m->natural.left, h = m->natural.bottom - m->natural.top;
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
                if (!filter(c, mcActionMouseDown, e) && mc_click_with(c, rd32(pt), rd16(e + 14) & 0x0200))
                    result = 1;
                tb_DisposePtr(pt);
            } else if ((ew == 3 || ew == 5) && c->active && (rd32(e + 2) & 0xFF) == ' ') { /* space */
                if (!filter(c, mcActionKey, e) && m)
                    mc_play(c, m->playing ? 0 : m->pref_rate);
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
            case mcActionStep:
                if (m)
                    seek(m, movie_ms(m) + (int16_t)(int32_t)p * 100);
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
                wr32(rd32(a), (uint32_t)(m ? m->scale : 600));
            result = m ? (int32_t)(movie_ms(m) * m->scale / 1000) : 0;
            break;
        case 24: mc_draw(c); break;                       /* MCDraw(w) */
        case 25: c->active = (rd16(a) >> 8) != 0; break; /* MCActivate(w, activate) */
        case 26: mc_idle(c); break;                       /* MCIdle */
        case 27:                                          /* MCKey(key, modifiers) */
            if (c->active && (rd16(a + 4) & 0xFF) == ' ' && m) {
                mc_play(c, m->playing ? 0 : m->pref_rate);
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
            bool sel = m && m->sel_duration > 0 && c->editing;
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
            for (int k = 0; m && k < m->ntracks; k++)
                if (m->tracks[k].type == fourcc("soun"))
                    f |= mcInfoHasSound;
            wr32(rd32(a), (uint32_t)f);
            break;
        }
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
    memset(movies, 0, sizeof movies);
    memset(controllers, 0, sizeof controllers);
    mc_component = 0;
    movies_error = sticky_error = 0;
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

