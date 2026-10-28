/*
 * Movies played by our own decoders (qt_codec.c): the first video track's
 * frames and the first sound track's samples, found through each track's
 * sample tables and edit list and read from the file holding the movie's
 * data.  The Movie Toolbox (quicktime.c) keeps the movie's clock and asks
 * for the frame at a time and for the sound of a stretch of time.
 *
 * A music track's samples are read whole when the movie opens and turned
 * into MIDI events (qt_music.c), which the Movie Toolbox sends to the host's
 * General MIDI synthesizer a little ahead of the clock.
 *
 * Text and sprite tracks (qt_text.c, qt_sprite.c) draw into pictures of
 * their own, which are composited with the video frame in the movie's
 * frame: each track's box (its tkhd size) through its matrix, the higher
 * layers behind.  A movie with them is as big as its tracks reach.
 *
 * Sample tables: stsd (descriptions), stts (durations), stss (key frames),
 * stsc (samples per chunk), stsz (sizes), stco/co64 (chunk offsets).  Sound
 * samples are frames, and a chunk's bytes follow from its frame count and
 * the format, as QuickTime 2 wrote them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qt_codec.h"
#include "qt_music.h"
#include "qt_native.h"
#include "qt_sprite.h"
#include "qt_text.h"

#define MAX_DESCS 8
#define MAX_VISUAL 8

typedef struct {
    int64_t movie_start, movie_len; /* in the movie's time scale */
    int64_t media_time;             /* in the media's; -1: empty */
} edit;

typedef struct {
    bool present;
    int32_t scale;                  /* the media's time scale */
    int64_t media_duration;
    edit *edits;
    int nedits;
    /* samples */
    uint32_t nsamples;
    uint64_t *offset;
    uint32_t *size;
    int64_t *time;                  /* each sample's start in media time */
    uint8_t *desc;                  /* each sample's description (0-based) */
    bool *key;
    /* video */
    qt_video_format vf[MAX_DESCS];
    uint32_t palettes[MAX_DESCS][256];
    int ndescs;
    /* music: the tune header (the description's events) */
    uint8_t *tune_header;
    uint32_t tune_header_len;
    /* text and sprites: the first sample description, whole */
    uint8_t *raw_desc;
    uint32_t raw_desc_len;
    /* sound: chunks of frames */
    qt_sound_format sf;
    uint32_t nchunks;
    uint64_t *chunk_offset;
    uint64_t *chunk_first;          /* the chunk's first frame */
    uint32_t *chunk_frames;
} trak;

/* A track that draws: where its picture goes in the movie's frame. */
typedef struct {
    int kind;                       /* VIDEO, TEXT or SPRITE */
    trak *t;
    int16_t layer;
    double a, b, c, d, tx, ty;      /* the track's matrix */
    int tw, th;                     /* its size */
    qt_text *text;
    qt_sprite *sprite;
    int64_t shown;                  /* the sample in the picture, -1 none */
    int64_t key, applied;           /* sprites: the key frame read, the last override applied */
    uint8_t *rgba;                  /* text and sprites: tw x th */
    bool drawn;
} visual;

struct qt_native {
    FILE *fp;                       /* the movie's data, or */
    qt_native_reader reader;        /* a reader of every sample's place */
    void *reader_ctx;
    int32_t scale;                  /* the movie's */
    trak video, sound, music;
    trak extra[MAX_VISUAL];         /* text and sprite tracks */
    int nextra;
    visual vis[MAX_VISUAL + 1];     /* with the video track, in file order */
    int nvis;
    bool composite;                 /* text or sprites: frames are composited */
    bool composed;
    uint8_t *canvas;
    int cw, ch;
    qt_midi_event *midi;            /* the music track's events, times in movie ms */
    size_t nmidi;
    qt_vdec *dec;
    int dec_desc;
    int64_t decoded;                /* the last sample decoded into frame, -1 none */
    uint8_t *frame;
    int fw, fh;
    uint32_t serial;
};

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
static uint32_t four(const char *s) { return (uint32_t)s[0] << 24 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 8 | (uint8_t)s[3]; }

/* The child atom of a type in [p, end) (contents after the 8-byte header). */
static const uint8_t *child(const uint8_t *p, const uint8_t *end, const char *type, uint32_t *len)
{
    while (p + 8 <= end) {
        uint32_t n = be32(p);
        if (n < 8 || n > (uint64_t)(end - p))
            return NULL;
        if (!memcmp(p + 4, type, 4)) {
            *len = n - 8;
            return p + 8;
        }
        p += n;
    }
    return NULL;
}

static const uint8_t *path(const uint8_t *p, uint32_t len, const char *const *names, int n, uint32_t *out)
{
    for (int i = 0; i < n && p; i++)
        p = child(p, p + len, names[i], &len);
    *out = len;
    return p;
}

static void free_trak(trak *t)
{
    free(t->edits);
    free(t->offset);
    free(t->size);
    free(t->time);
    free(t->desc);
    free(t->key);
    free(t->chunk_offset);
    free(t->chunk_first);
    free(t->chunk_frames);
    free(t->tune_header);
    free(t->raw_desc);
    memset(t, 0, sizeof *t);
}

/* Video sample descriptions: codec, size, depth, colour table. */
static bool video_descs(trak *t, const uint8_t *stsd, uint32_t len)
{
    if (len < 8)
        return false;
    uint32_t n = be32(stsd + 4);
    const uint8_t *p = stsd + 8, *end = stsd + len;
    for (uint32_t i = 0; i < n && i < MAX_DESCS && p + 86 <= end; i++) {
        uint32_t sz = be32(p);
        if (sz < 86 || sz > (uint64_t)(end - p))
            return false;
        qt_video_format *f = &t->vf[i];
        f->format = be32(p + 4);
        f->width = (int)be16(p + 32);
        f->height = (int)be16(p + 34);
        f->depth = (int16_t)be16(p + 82);
        int16_t ctab = (int16_t)be16(p + 84);
        f->palette = NULL;
        int bits = f->depth > 32 ? f->depth - 32 : f->depth;
        if (ctab == 0 && bits >= 1 && bits <= 8 && f->depth <= 8 && sz >= 94) { /* the colour table follows */
            const uint8_t *c = p + 86;
            int count = (int)be16(c + 6) + 1;
            for (int k = 0; k < count && k < 256 && 94 + (uint32_t)k * 8 + 8 <= sz; k++) {
                const uint8_t *e = c + 8 + k * 8;
                int v = (int)(be16(c + 4) & 0x8000 ? (uint32_t)k : be16(e)) & 255;
                t->palettes[i][v] = (uint32_t)e[2] << 16 | (uint32_t)e[4] << 8 | e[6];
            }
            f->palette = t->palettes[i];
        }
        t->ndescs = (int)i + 1;
        p += sz;
    }
    return t->ndescs > 0;
}

static bool sound_desc(trak *t, const uint8_t *stsd, uint32_t len)
{
    if (len < 8 + 36 || be32(stsd + 4) < 1)
        return false;
    const uint8_t *p = stsd + 8;
    t->sf.format = be32(p + 4);
    t->sf.channels = (int)be16(p + 24);
    t->sf.bits = (int)be16(p + 26);
    t->sf.rate = be32(p + 32) / 65536.0;
    if (t->sf.rate < 1000)
        t->sf.rate = t->scale;
    if (t->sf.format == four("raw ") || t->sf.format == four("twos") || t->sf.format == four("sowt")) {
        if (t->sf.bits != 16)
            t->sf.bits = 8;
    }
    return qt_adec_supported(&t->sf);
}

/* A music description: size, 'musi', reserved, data reference, flags, then
 * the tune header's events. */
static bool music_desc(trak *t, const uint8_t *stsd, uint32_t len)
{
    if (len < 8 + 20 || be32(stsd + 4) < 1)
        return false;
    const uint8_t *p = stsd + 8;
    uint32_t sz = be32(p);
    if (sz < 20 || sz > len - 8 || memcmp(p + 4, "musi", 4))
        return false;
    t->tune_header_len = sz - 20;
    t->tune_header = malloc(t->tune_header_len ? t->tune_header_len : 1);
    if (!t->tune_header)
        return false;
    memcpy(t->tune_header, p + 20, t->tune_header_len);
    return true;
}

static int32_t mdhd_scale(const uint8_t *mdhd, uint32_t len, int64_t *duration)
{
    if (len >= 32 && mdhd[0] == 1) {
        *duration = (int64_t)be64(mdhd + 24);
        return (int32_t)be32(mdhd + 20);
    }
    if (len < 20)
        return 0;
    *duration = be32(mdhd + 16);
    return (int32_t)be32(mdhd + 12);
}

/* The track's tables; false if anything needed is missing or damaged. */
enum { SOUND, VIDEO, MUSIC, TEXT, SPRITE };

/* The first sample description, whole (from its size field). */
static bool raw_desc(trak *t, const uint8_t *stsd, uint32_t len)
{
    if (len < 8 + 16 || be32(stsd + 4) < 1)
        return false;
    uint32_t sz = be32(stsd + 8);
    if (sz < 16 || sz > len - 8)
        return false;
    t->raw_desc = malloc(sz);
    if (!t->raw_desc)
        return false;
    memcpy(t->raw_desc, stsd + 8, sz);
    t->raw_desc_len = sz;
    return true;
}

static bool read_trak(trak *t, const uint8_t *tr, uint32_t len, int kind)
{
    bool video = kind != SOUND; /* video and music: tables of whole samples */
    uint32_t n;
    static const char *const mdhd_path[] = { "mdia", "mdhd" };
    const uint8_t *mdhd = path(tr, len, mdhd_path, 2, &n);
    if (!mdhd || (t->scale = mdhd_scale(mdhd, n, &t->media_duration)) <= 0)
        return false;
    /* the edit list (none: the media from the start) */
    static const char *const elst_path[] = { "edts", "elst" };
    const uint8_t *elst = path(tr, len, elst_path, 2, &n);
    if (elst && n >= 8) {
        uint32_t count = be32(elst + 4);
        if (count && count <= (n - 8) / 12) {
            t->edits = calloc(count, sizeof *t->edits);
            int64_t at = 0;
            for (uint32_t i = 0; i < count; i++) {
                const uint8_t *e = elst + 8 + i * 12;
                t->edits[i] = (edit){ at, be32(e), (int32_t)be32(e + 4) };
                at += be32(e);
            }
            t->nedits = (int)count;
        }
    }
    static const char *const stbl_path[] = { "mdia", "minf", "stbl" };
    uint32_t sl;
    const uint8_t *stbl = path(tr, len, stbl_path, 3, &sl);
    if (!stbl)
        return false;
    const uint8_t *se = stbl + sl;
    uint32_t dl, tl, cl, zl, ol;
    const uint8_t *stsd = child(stbl, se, "stsd", &dl), *stts = child(stbl, se, "stts", &tl),
                  *stsc = child(stbl, se, "stsc", &cl), *stsz = child(stbl, se, "stsz", &zl);
    const uint8_t *stco = child(stbl, se, "stco", &ol);
    bool co64 = false;
    if (!stco) {
        stco = child(stbl, se, "co64", &ol);
        co64 = true;
    }
    if (!stsd || !stts || !stsc || !stco || tl < 8 || cl < 8 || ol < 8)
        return false;
    bool described = kind == VIDEO   ? video_descs(t, stsd, dl)
                     : kind == MUSIC ? music_desc(t, stsd, dl)
                     : kind == SOUND ? sound_desc(t, stsd, dl)
                                     : raw_desc(t, stsd, dl);
    if (!described)
        return false;
    uint32_t nchunks = be32(stco + 4), nsc = be32(stsc + 4), ntts = be32(stts + 4);
    if (nchunks > (ol - 8) / (co64 ? 8 : 4) || nsc > (cl - 8) / 12 || ntts > (tl - 8) / 8 || !nchunks || !nsc)
        return false;
    /* samples (sound: frames) per chunk, and each chunk's description */
    uint32_t *per = calloc(nchunks, sizeof *per);
    uint8_t *cdesc = calloc(nchunks, 1);
    for (uint32_t i = 0; i < nsc; i++) {
        const uint8_t *e = stsc + 8 + i * 12;
        uint32_t first = be32(e), count = be32(e + 4), d = be32(e + 8);
        uint32_t last = i + 1 < nsc ? be32(e + 12) : nchunks + 1;
        for (uint32_t c = first; c < last && c >= 1 && c <= nchunks; c++) {
            per[c - 1] = count;
            cdesc[c - 1] = (uint8_t)(d >= 1 && d <= MAX_DESCS ? d - 1 : 0);
        }
    }
    bool ok = true;
    if (video) {
        if (!stsz || zl < 12) {
            ok = false;
        } else {
            uint32_t fixed = be32(stsz + 4), count = be32(stsz + 8);
            if (!fixed && count > (zl - 12) / 4)
                count = (zl - 12) / 4;
            if (count > 10000000)
                count = 0;
            t->nsamples = count;
            t->offset = calloc(count + 1, sizeof *t->offset);
            t->size = calloc(count + 1, sizeof *t->size);
            t->time = calloc(count + 1, sizeof *t->time);
            t->desc = calloc(count + 1, 1);
            t->key = calloc(count + 1, sizeof *t->key);
            uint32_t k = 0;
            for (uint32_t c = 0; c < nchunks && k < count; c++) {
                uint64_t off = co64 ? be64(stco + 8 + c * 8) : be32(stco + 8 + c * 4);
                for (uint32_t j = 0; j < per[c] && k < count; j++, k++) {
                    t->size[k] = fixed ? fixed : be32(stsz + 12 + k * 4);
                    t->offset[k] = off;
                    t->desc[k] = cdesc[c];
                    off += t->size[k];
                }
            }
            t->nsamples = k;
            int64_t at = 0;
            k = 0;
            for (uint32_t i = 0; i < ntts && k < t->nsamples; i++)
                for (uint32_t j = 0; j < be32(stts + 8 + i * 8) && k < t->nsamples; j++, k++) {
                    t->time[k] = at;
                    at += be32(stts + 12 + i * 8);
                }
            for (; k < t->nsamples; k++)
                t->time[k] = at;
            uint32_t kn;
            const uint8_t *stss = child(stbl, se, "stss", &kn);
            if (stss && kn >= 8) {
                uint32_t count2 = be32(stss + 4);
                for (uint32_t i = 0; i < count2 && 8 + i * 4 + 4 <= kn; i++) {
                    uint32_t s = be32(stss + 8 + i * 4);
                    if (s >= 1 && s <= t->nsamples)
                        t->key[s - 1] = true;
                }
            } else {
                for (uint32_t i = 0; i < t->nsamples; i++)
                    t->key[i] = true;
            }
            if (t->nsamples)
                t->key[0] = true;
            ok = t->nsamples > 0;
        }
    } else {
        t->nchunks = nchunks;
        t->chunk_offset = calloc(nchunks, sizeof *t->chunk_offset);
        t->chunk_first = calloc(nchunks, sizeof *t->chunk_first);
        t->chunk_frames = calloc(nchunks, sizeof *t->chunk_frames);
        uint64_t at = 0;
        for (uint32_t c = 0; c < nchunks; c++) {
            t->chunk_offset[c] = co64 ? be64(stco + 8 + c * 8) : be32(stco + 8 + c * 4);
            t->chunk_first[c] = at;
            t->chunk_frames[c] = per[c];
            at += per[c];
        }
    }
    free(per);
    free(cdesc);
    t->present = ok;
    return ok;
}

static bool any_reader; /* the movie being opened has a reader: every reference reaches its data */

static void self_contained_check(const uint8_t *tr, uint32_t len, bool *ok)
{
    if (any_reader)
        return;
    static const char *const dref_path[] = { "mdia", "minf", "dinf", "dref" };
    uint32_t n;
    const uint8_t *dref = path(tr, len, dref_path, 4, &n);
    if (!dref || n < 8)
        return; /* no references: the data is in the movie's file */
    uint32_t count = be32(dref + 4);
    const uint8_t *e = dref + 8;
    for (uint32_t i = 0; i < count && e + 12 <= dref + n; i++) {
        uint32_t en = be32(e);
        if (en < 12)
            return;
        if (!(e[11] & 1))
            *ok = false; /* the data is in another file */
        e += en;
    }
}

static bool read_music(qt_native *p);

/* The track's layer, matrix and size from its header. */
static void place(visual *v, const uint8_t *tkhd, uint32_t n)
{
    v->a = v->d = 1;
    if (!tkhd || n < 84)
        return;
    int at = tkhd[0] == 1 ? 12 : 0;
    if ((uint32_t)at + 84 > n)
        return;
    v->layer = (int16_t)be16(tkhd + 32 + at);
    const uint8_t *m = tkhd + 40 + at;
    v->a = (int32_t)be32(m) / 65536.0;
    v->b = (int32_t)be32(m + 4) / 65536.0;
    v->c = (int32_t)be32(m + 12) / 65536.0;
    v->d = (int32_t)be32(m + 16) / 65536.0;
    v->tx = (int32_t)be32(m + 24) / 65536.0;
    v->ty = (int32_t)be32(m + 28) / 65536.0;
    if (!v->a && !v->b && !v->c && !v->d) /* no matrix at all: as the identity */
        v->a = v->d = 1;
    v->tw = (int)(be32(tkhd + 76 + at) >> 16);
    v->th = (int)(be32(tkhd + 80 + at) >> 16);
}

/* A track's box in the movie: (0, 0, tw, th) through its matrix. */
static void box_of(const visual *v, double *x0, double *y0, double *x1, double *y1)
{
    *x0 = *y0 = 1e9;
    *x1 = *y1 = -1e9;
    for (int k = 0; k < 4; k++) {
        double x = k & 1 ? v->tw : 0, y = k & 2 ? v->th : 0;
        double X = v->a * x + v->c * y + v->tx, Y = v->b * x + v->d * y + v->ty;
        *x0 = X < *x0 ? X : *x0;
        *x1 = X > *x1 ? X : *x1;
        *y0 = Y < *y0 ? Y : *y0;
        *y1 = Y > *y1 ? Y : *y1;
    }
}

/* The visual tracks' pictures and the frame they make. */
static bool set_up_composite(qt_native *p)
{
    double right = 1, bottom = 1;
    for (int i = 0; i < p->nvis; i++) {
        visual *v = &p->vis[i];
        v->shown = v->key = v->applied = -1;
        if (v->kind == VIDEO && (v->tw <= 0 || v->th <= 0)) {
            v->tw = p->video.vf[0].width;
            v->th = p->video.vf[0].height;
        }
        if (v->kind == TEXT && (v->tw <= 0 || v->th <= 0) && v->t->raw_desc_len >= 38) {
            v->tw = (int16_t)be16(v->t->raw_desc + 36); /* the default text box's right and bottom */
            v->th = (int16_t)be16(v->t->raw_desc + 34);
        }
        if (v->tw <= 0 || v->th <= 0 || v->tw > 4096 || v->th > 4096)
            return false;
        if (v->kind == TEXT && !(v->text = qt_text_new(v->t->raw_desc, v->t->raw_desc_len, v->tw, v->th)))
            return false;
        if (v->kind == SPRITE && !(v->sprite = qt_sprite_new(v->t->raw_desc, v->t->raw_desc_len, v->tw, v->th)))
            return false;
        if (v->kind != VIDEO && !(v->rgba = calloc((size_t)v->tw * (size_t)v->th, 4)))
            return false;
        double x0, y0, x1, y1;
        box_of(v, &x0, &y0, &x1, &y1);
        right = x1 > right ? x1 : right;
        bottom = y1 > bottom ? y1 : bottom;
    }
    p->cw = (int)(right + 0.5);
    p->ch = (int)(bottom + 0.5);
    if (p->cw > 4096 || p->ch > 4096)
        return false;
    p->canvas = calloc((size_t)p->cw * (size_t)p->ch, 4);
    return p->canvas != NULL;
}

static qt_native *open_native(const uint8_t *moov, uint32_t len, const char *data_path, qt_native_reader reader,
                              void *ctx, int32_t movie_scale, const uint32_t *screen_palette, int screen_depth);

qt_native *qt_native_open(const uint8_t *moov, uint32_t len, const char *data_path, int32_t movie_scale,
                          const uint32_t *screen_palette, int screen_depth)
{
    return open_native(moov, len, data_path, NULL, NULL, movie_scale, screen_palette, screen_depth);
}

qt_native *qt_native_open_reader(const uint8_t *moov, uint32_t len, qt_native_reader reader, void *ctx,
                                 int32_t movie_scale, const uint32_t *screen_palette, int screen_depth)
{
    return open_native(moov, len, NULL, reader, ctx, movie_scale, screen_palette, screen_depth);
}

static qt_native *open_native(const uint8_t *moov, uint32_t len, const char *data_path, qt_native_reader reader,
                              void *ctx, int32_t movie_scale, const uint32_t *screen_palette, int screen_depth)
{
    if (len < 8 || memcmp(moov + 4, "moov", 4))
        return NULL;
    any_reader = reader != NULL;
    qt_native *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->scale = movie_scale > 0 ? movie_scale : 600;
    p->decoded = -1;
    bool ok = true;
    const uint8_t *end = moov + len;
    for (const uint8_t *t = moov + 8; t + 8 <= end;) {
        uint32_t tn = be32(t);
        if (tn < 8 || tn > (uint64_t)(end - t))
            break;
        if (!memcmp(t + 4, "trak", 4)) {
            uint32_t hn, kn;
            static const char *const hdlr_path[] = { "mdia", "hdlr" };
            const uint8_t *hdlr = path(t + 8, tn - 8, hdlr_path, 2, &hn);
            const uint8_t *tkhd = child(t + 8, t + tn, "tkhd", &kn);
            bool enabled = !tkhd || kn < 4 || (tkhd[3] & 1);
            uint32_t type = hdlr && hn >= 12 ? be32(hdlr + 8) : 0;
            if (enabled && type == four("vide") && !p->video.present) {
                self_contained_check(t + 8, tn - 8, &ok);
                if (!read_trak(&p->video, t + 8, tn - 8, VIDEO))
                    ok = false;
                visual *v = &p->vis[p->nvis++];
                v->kind = VIDEO;
                v->t = &p->video;
                place(v, tkhd, kn);
            } else if (enabled && type == four("soun") && !p->sound.present) {
                self_contained_check(t + 8, tn - 8, &ok);
                if (!read_trak(&p->sound, t + 8, tn - 8, SOUND))
                    ok = false;
            } else if (enabled && type == four("musi") && !p->music.present) {
                self_contained_check(t + 8, tn - 8, &ok);
                if (!read_trak(&p->music, t + 8, tn - 8, MUSIC))
                    ok = false;
            } else if (enabled && (type == four("text") || type == four("sprt")) && p->nextra < MAX_VISUAL) {
                self_contained_check(t + 8, tn - 8, &ok);
                trak *x = &p->extra[p->nextra++];
                int kind = type == four("text") ? TEXT : SPRITE;
                if (!read_trak(x, t + 8, tn - 8, kind))
                    ok = false;
                visual *v = &p->vis[p->nvis++];
                v->kind = kind;
                v->t = x;
                place(v, tkhd, kn);
                p->composite = true;
            } else if (enabled && type != four("tmcd") && type != 0) {
                ok = false; /* text, sprites, MPEG: not ours */
            }
        }
        t += tn;
    }
    for (int i = 0; ok && i < p->video.ndescs; i++) {
        qt_video_format *f = &p->video.vf[i];
        if (!qt_vdec_supported(f->format, f->depth))
            ok = false;
        /* Palettized pictures without colours of their own took the screen's. */
        if (!f->palette && f->depth == screen_depth && screen_palette && f->depth <= 8) {
            memcpy(p->video.palettes[i], screen_palette, sizeof(uint32_t) << f->depth);
            f->palette = p->video.palettes[i];
        }
    }
    if (ok && p->composite)
        ok = set_up_composite(p);
    p->reader = reader;
    p->reader_ctx = ctx;
    if (ok && !reader && (p->video.present || p->sound.present || p->music.present || p->nextra))
        p->fp = fopen(data_path, "rb");
    if (ok && (p->fp || p->reader) && p->music.present)
        ok = read_music(p);
    if (!ok || (!p->fp && !p->reader)) {
        qt_native_close(p);
        return NULL;
    }
    return p;
}

static uint8_t *read_at(const qt_native *p, uint64_t off, uint32_t size);

/* Movie time (ms) of a moment of the track's media (the first edit that
 * shows media, or none). */
static int64_t media_to_ms(const trak *t, int32_t movie_scale, int64_t media)
{
    for (int i = 0; i < t->nedits; i++)
        if (t->edits[i].media_time >= 0)
            return t->edits[i].movie_start * 1000 / movie_scale + (media - t->edits[i].media_time) * 1000 / t->scale;
    return media * 1000 / t->scale;
}

/* The whole music track as MIDI events, in movie time. */
static bool read_music(qt_native *p)
{
    trak *t = &p->music;
    qt_music_sample *s = calloc(t->nsamples ? t->nsamples : 1, sizeof *s);
    if (!s)
        return false;
    bool ok = true;
    for (uint32_t k = 0; k < t->nsamples && ok; k++) {
        s[k].data = read_at(p, t->offset[k], t->size[k]);
        s[k].len = t->size[k];
        s[k].time = t->time[k];
        ok = s[k].data != NULL;
    }
    if (ok)
        p->midi = qt_music_events(t->tune_header, t->tune_header_len, s, t->nsamples, &p->nmidi);
    for (uint32_t k = 0; k < t->nsamples; k++)
        free((void *)s[k].data);
    free(s);
    for (size_t i = 0; i < p->nmidi; i++)
        p->midi[i].time = media_to_ms(t, p->scale, p->midi[i].time);
    return ok;
}

void qt_native_close(qt_native *p)
{
    if (!p)
        return;
    if (p->fp)
        fclose(p->fp);
    qt_vdec_free(p->dec);
    free_trak(&p->video);
    free_trak(&p->sound);
    free_trak(&p->music);
    for (int i = 0; i < p->nvis; i++) {
        qt_text_free(p->vis[i].text);
        qt_sprite_free(p->vis[i].sprite);
        free(p->vis[i].rgba);
    }
    for (int i = 0; i < p->nextra; i++)
        free_trak(&p->extra[i]);
    free(p->canvas);
    free(p->midi);
    free(p->frame);
    free(p);
}

bool qt_native_has_video(const qt_native *p) { return p && (p->video.present || p->composite); }
bool qt_native_has_sound(const qt_native *p) { return p && p->sound.present; }
bool qt_native_has_music(const qt_native *p) { return p && p->music.present; }

/* The music's events in movie time [from_ms, to_ms): *first and the count. */
size_t qt_native_music(qt_native *p, int64_t from_ms, int64_t to_ms, const qt_midi_event **first)
{
    size_t lo = 0, hi = p->nmidi;
    while (lo < hi) { /* the first at or after from_ms */
        size_t mid = (lo + hi) / 2;
        if (p->midi[mid].time < from_ms)
            lo = mid + 1;
        else
            hi = mid;
    }
    size_t end = lo;
    while (end < p->nmidi && p->midi[end].time < to_ms)
        end++;
    *first = p->midi + lo;
    return end - lo;
}

/* Movie time (ms) to the track's media time; -1 in an empty edit or past the end. */
static int64_t media_time(const trak *t, int32_t movie_scale, int64_t ms)
{
    int64_t mt = ms * movie_scale / 1000; /* movie time units */
    if (!t->nedits)
        return mt * t->scale / movie_scale;
    for (int i = 0; i < t->nedits; i++) {
        const edit *e = &t->edits[i];
        if (mt >= e->movie_start && mt < e->movie_start + e->movie_len) {
            if (e->media_time < 0)
                return -1;
            return e->media_time + (mt - e->movie_start) * t->scale / movie_scale;
        }
    }
    /* At the very end: the last edit's last moment. */
    const edit *e = &t->edits[t->nedits - 1];
    if (mt == e->movie_start + e->movie_len && e->media_time >= 0 && e->movie_len > 0)
        return e->media_time + (e->movie_len - 1) * t->scale / movie_scale;
    return -1;
}

/* The sample showing at media time mt. */
static int64_t sample_at(const trak *t, int64_t mt)
{
    uint32_t lo = 0, hi = t->nsamples;
    while (hi - lo > 1) {
        uint32_t mid = (lo + hi) / 2;
        if (t->time[mid] <= mt)
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

static uint8_t *read_at(const qt_native *p, uint64_t off, uint32_t size)
{
    if (size > 64u << 20)
        return NULL;
    uint8_t *buf = malloc(size ? size : 1);
    if (buf && p->reader) {
        if (!p->reader(p->reader_ctx, off, buf, size)) {
            free(buf);
            return NULL;
        }
        return buf;
    }
    if (!buf || !p->fp || fseeko(p->fp, (off_t)off, SEEK_SET) != 0 || fread(buf, 1, size, p->fp) != size) {
        free(buf);
        return NULL;
    }
    return buf;
}

static bool decode_sample(qt_native *p, int64_t k)
{
    trak *t = &p->video;
    int d = t->desc[k] < t->ndescs ? t->desc[k] : 0;
    if (!p->dec || p->dec_desc != d) {
        qt_vdec_free(p->dec);
        p->dec = qt_vdec_new(&t->vf[d]);
        p->dec_desc = d;
        int w = t->vf[d].width, h = t->vf[d].height;
        if (!p->dec || w <= 0 || h <= 0)
            return false;
        if (w != p->fw || h != p->fh) {
            free(p->frame);
            p->frame = calloc((size_t)w * (size_t)h, 4);
            p->fw = w;
            p->fh = h;
        }
    }
    uint8_t *data = read_at(p, t->offset[k], t->size[k]);
    if (!data)
        return false;
    bool ok = qt_vdec_decode(p->dec, data, t->size[k], p->frame);
    free(data);
    return ok;
}

/* Decode on to video sample want. */
static void video_to(qt_native *p, int64_t want)
{
    trak *t = &p->video;
    /* From the last key frame at or before it, unless decoding on from
     * the current picture gets there without passing one. */
    int64_t from = want;
    while (from > 0 && !t->key[from])
        from--;
    if (p->decoded >= 0 && p->decoded < want && p->decoded >= from)
        from = p->decoded + 1;
    for (int64_t k = from; k <= want; k++)
        decode_sample(p, k);
    p->decoded = want;
}

/* The sample of a text or sprite track showing at ms, or -1. */
static int64_t showing(const qt_native *p, const trak *t, int64_t ms)
{
    int64_t mt = media_time(t, p->scale, ms);
    if (mt < 0 || !t->nsamples || (t->media_duration > 0 && mt >= t->media_duration))
        return -1;
    return sample_at(t, mt);
}

static void text_to(qt_native *p, visual *v, int64_t want)
{
    v->drawn = false;
    if (want < 0)
        return;
    uint8_t *data = read_at(p, v->t->offset[want], v->t->size[want]);
    if (data)
        v->drawn = qt_text_draw(v->text, data, v->t->size[want], v->rgba);
    free(data);
}

static void sprite_apply(qt_native *p, visual *v, int64_t k)
{
    uint8_t *data = read_at(p, v->t->offset[k], v->t->size[k]);
    if (data)
        qt_sprite_override(v->sprite, data, v->t->size[k]);
    free(data);
}

static void sprite_to(qt_native *p, visual *v, int64_t want)
{
    v->drawn = false;
    if (want < 0)
        return;
    trak *t = v->t;
    /* The key frame: the last sync sample at or before it that has the
     * shared images. */
    int64_t k = want;
    while (k > 0 && !t->key[k])
        k--;
    if (k != v->key) {
        for (;;) {
            uint8_t *data = read_at(p, t->offset[k], t->size[k]);
            bool is_key = data && qt_sprite_key(v->sprite, data, t->size[k]);
            free(data);
            if (is_key || k == 0)
                break;
            for (k--; k > 0 && !t->key[k]; k--)
                ;
        }
        v->key = v->applied = k;
    }
    if (qt_sprite_all_overrides(v->sprite)) {
        if (v->applied > want || v->applied < k) {
            qt_sprite_reset(v->sprite);
            v->applied = k;
        }
        for (int64_t j = v->applied + 1; j <= want; j++)
            sprite_apply(p, v, j);
        v->applied = want;
    } else {
        qt_sprite_reset(v->sprite);
        if (want != k)
            sprite_apply(p, v, want);
    }
    v->drawn = qt_sprite_draw(v->sprite, v->rgba);
}

/* A picture (sw x sh) into box (x0, y0)-(x1, y1) of the frame, nearest
 * pixel, leaving the frame where the picture's alpha is 0. */
static void blit(qt_native *p, const uint8_t *src, int sw, int sh, double x0, double y0, double x1, double y1)
{
    if (x1 - x0 < 0.5 || y1 - y0 < 0.5)
        return;
    int c0 = (int)(x0 + 0.5), c1 = (int)(x1 + 0.5), r0 = (int)(y0 + 0.5), r1 = (int)(y1 + 0.5);
    for (int row = r0 < 0 ? 0 : r0; row < r1 && row < p->ch; row++) {
        int sy = (int)((row + 0.5 - y0) * sh / (y1 - y0));
        if (sy < 0 || sy >= sh)
            continue;
        for (int col = c0 < 0 ? 0 : c0; col < c1 && col < p->cw; col++) {
            int sx = (int)((col + 0.5 - x0) * sw / (x1 - x0));
            if (sx < 0 || sx >= sw)
                continue;
            const uint8_t *s = src + ((size_t)sy * (size_t)sw + (size_t)sx) * 4;
            if (s[3])
                memcpy(p->canvas + ((size_t)row * (size_t)p->cw + (size_t)col) * 4, s, 4);
        }
    }
}

/* The frame: white, then the tracks from the highest layer to the lowest
 * (later tracks in front within a layer). */
static void compose(qt_native *p)
{
    memset(p->canvas, 255, (size_t)p->cw * (size_t)p->ch * 4);
    int order[MAX_VISUAL + 1];
    for (int i = 0; i < p->nvis; i++)
        order[i] = i;
    for (int i = 1; i < p->nvis; i++)
        for (int j = i; j > 0 && p->vis[order[j - 1]].layer < p->vis[order[j]].layer; j--) {
            int t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    for (int i = 0; i < p->nvis; i++) {
        visual *v = &p->vis[order[i]];
        double x0, y0, x1, y1;
        box_of(v, &x0, &y0, &x1, &y1);
        if (v->kind == VIDEO && p->frame && p->decoded >= 0)
            blit(p, p->frame, p->fw, p->fh, x0, y0, x1, y1);
        else if (v->kind != VIDEO && v->drawn)
            blit(p, v->rgba, v->tw, v->th, x0, y0, x1, y1);
    }
}

static bool composite_frame(qt_native *p, int64_t ms)
{
    bool changed = !p->composed;
    for (int i = 0; i < p->nvis; i++) {
        visual *v = &p->vis[i];
        if (v->kind == VIDEO) {
            int64_t mt = media_time(v->t, p->scale, ms);
            if (mt < 0) /* an empty edit keeps the last picture */
                continue;
            int64_t want = sample_at(v->t, mt);
            if (want != p->decoded) {
                video_to(p, want);
                changed = true;
            }
            continue;
        }
        int64_t want = showing(p, v->t, ms);
        if (want == v->shown && p->composed)
            continue;
        if (v->kind == TEXT)
            text_to(p, v, want);
        else
            sprite_to(p, v, want);
        v->shown = want;
        changed = true;
    }
    if (changed) {
        compose(p);
        p->composed = true;
        p->serial++;
    }
    return true;
}

bool qt_native_frame(qt_native *p, int64_t ms, uint32_t *serial, uint8_t **rgba, int *w, int *h)
{
    trak *t = &p->video;
    const uint8_t *frame;
    int fw, fh;
    if (p->composite) {
        composite_frame(p, ms);
        frame = p->canvas;
        fw = p->cw;
        fh = p->ch;
    } else {
        if (!t->present)
            return false;
        int64_t mt = media_time(t, p->scale, ms);
        if (mt < 0) /* an empty edit keeps the last picture */
            return false;
        int64_t want = sample_at(t, mt);
        if (want != p->decoded) {
            video_to(p, want);
            p->serial++;
        }
        frame = p->frame;
        fw = p->fw;
        fh = p->fh;
    }
    if (!frame || *serial == p->serial)
        return false;
    size_t n = (size_t)fw * (size_t)fh * 4;
    if (*w != fw || *h != fh || !*rgba) {
        free(*rgba);
        *rgba = malloc(n);
    }
    memcpy(*rgba, frame, n);
    *w = fw;
    *h = fh;
    *serial = p->serial;
    return true;
}

/* The sound for movie time [from, to) ms (within one edit), as PCM. */
uint32_t qt_native_sound(qt_native *p, int64_t from_ms, int64_t to_ms, int16_t **pcm, int *channels, double *rate)
{
    *pcm = NULL;
    trak *t = &p->sound;
    if (!t->present || to_ms <= from_ms)
        return 0;
    *channels = t->sf.channels;
    *rate = t->sf.rate;
    int64_t a = media_time(t, p->scale, from_ms), b = media_time(t, p->scale, to_ms);
    if (b < 0) /* past the end of the edit */
        b = media_time(t, p->scale, to_ms - 1) + 1;
    if (a < 0 || b <= a)
        return 0;
    /* media time to frames */
    uint64_t fa = (uint64_t)((double)a * t->sf.rate / t->scale), fb = (uint64_t)((double)b * t->sf.rate / t->scale);
    if (fb <= fa)
        return 0;
    if (fb - fa > (uint64_t)t->sf.rate * 10)
        fb = fa + (uint64_t)t->sf.rate * 10;
    size_t nch = (size_t)t->sf.channels;
    int16_t *out = calloc((size_t)(fb - fa) * nch, sizeof *out);
    if (!out)
        return 0;
    for (uint32_t c = 0; c < t->nchunks; c++) {
        uint64_t c0 = t->chunk_first[c], c1 = c0 + t->chunk_frames[c];
        if (c1 <= fa || c0 >= fb)
            continue;
        size_t bytes = qt_adec_bytes(&t->sf, t->chunk_frames[c]);
        uint8_t *data = read_at(p, t->chunk_offset[c], (uint32_t)bytes);
        if (!data)
            continue;
        int16_t *dec = NULL;
        uint32_t got = qt_adec_decode(&t->sf, data, bytes, &dec);
        free(data);
        for (uint64_t f = c0 > fa ? c0 : fa; f < c1 && f < fb && f - c0 < got; f++)
            memcpy(out + (f - fa) * nch, dec + (f - c0) * nch, nch * sizeof *out);
        free(dec);
    }
    *pcm = out;
    return (uint32_t)(fb - fa);
}
