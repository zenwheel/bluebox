/*
 * QuickTime's media handlers ('mhlr' components) for the media types Blue
 * Box plays: video, sound, text, sprites, music, timecode, movies in
 * movies and generic media.  The Movie Toolbox gives a program its media's
 * handler (GetMediaHandler), and the derived calls of each type work on
 * the media's samples and on what is showing: text samples are made from
 * text and styles (TextMediaAddTextSample and the rest), searched,
 * highlighted and changed as they show; sprites are found, hit, read and
 * moved as they show; timecode samples are read as times; a music
 * media's tune is given to a tune player of its own.
 *
 * The handlers' base calls (MediaHandlers.h) are the ones the Movie
 * Toolbox makes of a handler to play it.  Our own playing doesn't go
 * through them, so they keep what they are told and answer from the
 * media's records.
 */
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qd_internal.h"
#include "qt_atoms.h"
#include "qt_comp.h"
#include "qt_dref.h"
#include "qt_errors.h"
#include "qt_model.h"
#include "qt_movie_internal.h"
#include "qt_native.h"
#include "qt_sprite.h"
#include "qt_text.h"
#include "tb_internal.h"

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

enum {
    dfDontDisplay = 1, tcDropFrame = 1, tc24HourMax = 2, tcNegTimesOK = 4, tcCounter = 8, tctNegFlag = 0x80,
    mediaChanged = 1, hintsUseSoundInterp = 0x80,
};

#define MAX_PTRS 8

typedef struct {
    uint32_t type;
    gaddr media;                  /* the Media it was given */
    /* what the Movie Toolbox tells a handler */
    gaddr port, gd, clip, vout, sound_output;
    int32_t rate, hints, caps, width, height, mode;
    uint16_t op[3];
    int32_t matrix[9];
    int16_t volume, balance, bass, treble;
    bool active, metering, screen_lock;
    int32_t active_start, active_duration;
    gaddr actions_proc, mcaction_proc;
    uint32_t actions_refcon, mcaction_refcon;
    /* text */
    gaddr text_proc;
    uint32_t text_refcon;
    int64_t text_shown;           /* the sample the text procedure was last called for */
    uint8_t *pending;             /* atoms for the next text sample (TextMediaSetTextSampleData) */
    uint32_t npending;
    gaddr raw_gw, raw_gd, raw_desc;
    uint8_t *raw;                 /* TextMediaRawSetup's sample */
    uint32_t nraw;
    /* video */
    uint32_t frames_drawn, stalls;
    struct {
        uint32_t codec, id;
        uint8_t *data;
        uint32_t len;
    } codec_params[8];
    /* timecode */
    int32_t tc_flags;
    uint8_t tc_options[20];       /* TCTextOptions */
    /* sample data pointers given out */
    struct {
        int32_t sample;
        gaddr ptr;
    } ptrs[MAX_PTRS];
    /* sprites */
    float variables_value[64];
    uint32_t variables_id[64];
    int nvariables;
} mhlr;

/* ---- The media behind the handler ---- */

static qtm_media *media_of(const mhlr *h, qtm_track **t, qtm_movie **mv)
{
    return h->media ? qt_media_model(h->media, t, mv) : NULL;
}

/* The bytes of a sample (1-based), malloc'd. */
static uint8_t *sample_bytes(qtm_movie *mv, qtm_media *md, uint32_t n, uint32_t *len, uint32_t *desc)
{
    uint64_t off, bytes;
    uint32_t d;
    if (!qtm_sample_info(md, n, &off, &bytes, &d, NULL, NULL) || bytes > 64u << 20)
        return NULL;
    int16_t di = qtm_desc_dref(md, d);
    if (di < 1 || di > md->ndrefs)
        return NULL;
    const qtm_dref *r = &md->drefs[di - 1];
    qt_src src;
    if (!qt_src_open(r->type, r->flags, r->data, r->len, qt_movie_path(mv), false, &src))
        return NULL;
    uint8_t *b = malloc(bytes ? (size_t)bytes : 1);
    bool ok = qt_src_read(&src, off, b, (uint32_t)bytes);
    qt_src_close(&src);
    if (!ok) {
        free(b);
        return NULL;
    }
    *len = (uint32_t)bytes;
    if (desc)
        *desc = d;
    return b;
}

/* The movie's media time now, through the track's edits (-1: none showing). */
static int64_t media_now(qtm_movie *mv, qtm_track *t)
{
    return qtm_track_to_media(t, qt_movie_now(mv), mv->scale, NULL);
}

static void put_rgb(gaddr p, const uint16_t c[3])
{
    for (int k = 0; k < 3; k++)
        wr16(p + 2 * (uint32_t)k, c[k]);
}

/* ---- Text samples ---- */

/* An atom of a type and contents onto a growing sample. */
static void add_atom(uint8_t **b, uint32_t *n, const char *type, const void *data, uint32_t len)
{
    *b = realloc(*b, *n + 8 + len);
    put32(*b + *n, 8 + len);
    memcpy(*b + *n + 4, type, 4);
    if (len)
        memcpy(*b + *n + 8, data, len);
    *n += 8 + len;
}

/* A ScrpSTElement (20 bytes): start, height, ascent, font, face and
 * filler, size, colour. */
static void style_element(uint8_t *e, int32_t start, int16_t font, uint8_t face, int16_t size, const uint16_t c[3])
{
    memset(e, 0, 20);
    put32(e, (uint32_t)start);
    int16_t sz = size > 0 ? size : 12;
    put16(e + 4, (uint32_t)(sz + (sz + 3) / 4));  /* height: ascent, descent and leading */
    put16(e + 6, (uint32_t)(sz - sz / 5));        /* ascent */
    put16(e + 8, (uint32_t)font);
    e[10] = face;
    put16(e + 12, (uint32_t)sz);
    for (int k = 0; k < 3; k++)
        put16(e + 14 + k * 2, c[k]);
}

static void rgb_of(gaddr p, uint16_t c[3], uint16_t dflt)
{
    for (int k = 0; k < 3; k++)
        c[k] = p ? rd16(p + 2 * (uint32_t)k) : dflt;
}

/* A TextDescription for the sample's settings. */
static uint8_t *text_description(int16_t font, uint8_t face, int16_t size, const uint16_t fg[3], const uint16_t bg[3],
                                 int16_t just, const int16_t box[4], int32_t flags, uint32_t *len)
{
    uint8_t *d = calloc(1, 60);
    put32(d, 60);
    memcpy(d + 4, "text", 4);
    put16(d + 14, 1);
    put32(d + 16, (uint32_t)flags);
    put32(d + 20, (uint32_t)(int32_t)just);
    for (int k = 0; k < 3; k++)
        put16(d + 24 + k * 2, bg[k]);
    for (int k = 0; k < 4; k++)
        put16(d + 30 + k * 2, (uint16_t)box[k]);
    style_element(d + 38, 0, font, face, size, fg);
    d[58] = 0; /* no font name: the number stands */
    *len = 60;
    return d;
}

/* The track's box, for a text box not given. */
static void track_box(const qtm_track *t, int16_t box[4])
{
    box[0] = box[1] = 0;
    box[2] = (int16_t)(t ? t->height >> 16 : 0);
    box[3] = (int16_t)(t ? t->width >> 16 : 0);
}

/* Add a text sample: the text, then its atoms; the description from the
 * settings.  The sample's media time. */
static int32_t add_text_sample(mhlr *h, const uint8_t *text, uint32_t size, int16_t font, int16_t fsize, uint8_t face,
                               gaddr fg_p, gaddr bg_p, int16_t just, gaddr box_p, int32_t flags, int32_t scroll,
                               int16_t hl_start, int16_t hl_end, gaddr hl_color, int32_t duration, gaddr sample_time,
                               const uint8_t *style_scrap, uint32_t style_len)
{
    qtm_movie *mv;
    qtm_track *t;
    qtm_media *md = media_of(h, &t, &mv);
    if (!md)
        return invalidMedia;
    if (size > 0x7FFF)
        size = 0x7FFF;
    uint16_t fg[3], bg[3];
    rgb_of(fg_p, fg, 0);
    rgb_of(bg_p, bg, 0xFFFF);
    int16_t box[4];
    if (box_p)
        for (int k = 0; k < 4; k++)
            box[k] = (int16_t)rd16(box_p + 2 * (uint32_t)k);
    else
        track_box(t, box);
    uint32_t n = 2 + size;
    uint8_t *b = malloc(n);
    put16(b, size);
    memcpy(b + 2, text, size);
    if (style_scrap && style_len >= 2) {
        add_atom(&b, &n, "styl", style_scrap, style_len);
    } else {
        uint8_t st[22];
        put16(st, 1);
        style_element(st + 2, 0, font, face, fsize, fg);
        add_atom(&b, &n, "styl", st, 22);
    }
    if (hl_start != hl_end) {
        uint8_t hl[8];
        put32(hl, (uint32_t)(int32_t)hl_start);
        put32(hl + 4, (uint32_t)(int32_t)hl_end);
        add_atom(&b, &n, "hlit", hl, 8);
    }
    if (hl_color) {
        uint8_t c[6];
        for (int k = 0; k < 3; k++)
            put16(c + k * 2, rd16(hl_color + 2 * (uint32_t)k));
        add_atom(&b, &n, "hclr", c, 6);
    }
    if (scroll) {
        uint8_t dl[4];
        put32(dl, (uint32_t)scroll);
        add_atom(&b, &n, "dlay", dl, 4);
    }
    if (box_p) {
        uint8_t tb[8];
        for (int k = 0; k < 4; k++)
            put16(tb + k * 2, (uint16_t)box[k]);
        add_atom(&b, &n, "tbox", tb, 8);
    }
    if (h->npending) { /* TextMediaSetTextSampleData's */
        b = realloc(b, n + h->npending);
        memcpy(b + n, h->pending, h->npending);
        n += h->npending;
        free(h->pending);
        h->pending = NULL;
        h->npending = 0;
    }
    uint32_t dl;
    uint8_t *desc = text_description(font, face, fsize, fg, bg, just, box, flags, &dl);
    gaddr dh = qt_dref_handle(desc, dl), sh = qt_dref_handle(b, n);
    free(desc);
    free(b);
    int16_t err = tb_AddMediaSample((int32_t)h->media, (int32_t)sh, 0, (int32_t)n, duration, (int32_t)dh, 1, 0,
                                    sample_time);
    tb_DisposeHandle(sh);
    tb_DisposeHandle(dh);
    return err;
}

/* The text sample at a media time (its number), and its bytes. */
static uint8_t *text_at(mhlr *h, int64_t mtime, uint32_t *num, uint32_t *len, int64_t *start, int64_t *dur)
{
    qtm_movie *mv;
    qtm_media *md = media_of(h, NULL, &mv);
    if (!md)
        return NULL;
    uint32_t s = qtm_sample_at(md, mtime, start, dur);
    if (!s)
        return NULL;
    if (num)
        *num = s;
    return sample_bytes(mv, md, s, len, NULL);
}

/* A sample's parts: its text, and its atoms with one of a type replaced
 * (or removed, for data NULL). */
static uint8_t *with_atom(const uint8_t *b, uint32_t n, const char *type, const void *data, uint32_t len, uint32_t *out)
{
    uint32_t tl = n >= 2 ? be16(b) : 0;
    if (2 + tl > n)
        tl = n >= 2 ? n - 2 : 0;
    uint32_t m = 2 + tl;
    uint8_t *r = malloc(m);
    memcpy(r, b, m);
    if (n < 2)
        put16(r, 0);
    for (uint32_t p = 2 + tl; p + 8 <= n;) {
        uint32_t an = be32(b + p);
        if (an < 8 || p + an > n)
            break;
        if (memcmp(b + p + 4, type, 4)) {
            r = realloc(r, m + an);
            memcpy(r + m, b + p, an);
            m += an;
        }
        p += an;
    }
    if (data)
        add_atom(&r, &m, type, data, len);
    *out = m;
    return r;
}

static uint8_t *with_text(const uint8_t *b, uint32_t n, const uint8_t *text, uint32_t tl, uint32_t *out)
{
    uint32_t old = n >= 2 ? be16(b) : 0;
    if (2 + old > n)
        old = n >= 2 ? n - 2 : 0;
    uint32_t rest = n > 2 + old ? n - 2 - old : 0;
    uint8_t *r = malloc(2 + tl + rest);
    put16(r, tl);
    memcpy(r + 2, text, tl);
    memcpy(r + 2 + tl, b + 2 + old, rest);
    *out = 2 + tl + rest;
    return r;
}

/* The first style element of a sample (or a default one). */
static void first_style(const uint8_t *b, uint32_t n, uint8_t e[20])
{
    uint32_t tl = n >= 2 ? be16(b) : 0;
    uint16_t black[3] = { 0, 0, 0 };
    style_element(e, 0, 0, 0, 12, black);
    for (uint32_t p = 2 + tl; p + 8 <= n;) {
        uint32_t an = be32(b + p);
        if (an < 8 || p + an > n)
            break;
        if (!memcmp(b + p + 4, "styl", 4) && an >= 8 + 2 + 20) {
            memcpy(e, b + p + 10, 20);
            return;
        }
        p += an;
    }
}

/* Show a changed sample in place of the one at a media time. */
static void show_text(mhlr *h, int64_t mtime, const uint8_t *b, uint32_t n)
{
    qtm_movie *mv;
    qtm_track *t;
    qtm_media *md = media_of(h, &t, &mv);
    if (!md)
        return;
    uint32_t s = qtm_sample_at(md, mtime, NULL, NULL);
    qt_native *nat = qt_movie_native(mv);
    if (s && nat) {
        qt_native_text_override(nat, qt_track_index(mv, t), (int64_t)s - 1, b, n, false);
        qt_movie_redraw(mv);
    }
}

static qt_text *text_drawing(mhlr *h, int *track, qtm_movie **mvp)
{
    qtm_movie *mv;
    qtm_track *t;
    if (!media_of(h, &t, &mv))
        return NULL;
    *track = qt_track_index(mv, t);
    *mvp = mv;
    return qt_native_text(qt_movie_native(mv), *track);
}

static int32_t set_text_property(mhlr *h, int32_t mtime, int32_t prop, gaddr data, int32_t size)
{
    uint32_t n, num;
    int64_t start, dur;
    uint8_t *b = text_at(h, mtime, &num, &n, &start, &dur);
    if (!b)
        return timeNotInMedia;
    uint8_t *r = NULL;
    uint32_t rn = 0;
    uint8_t e[20];
    first_style(b, n, e);
    switch (prop) {
    case 1: { /* kTextTextHandle */
        gaddr th = data;
        uint32_t tl = th && rd32(th) ? (uint32_t)tb_GetHandleSize(th) : 0;
        uint8_t *tx = malloc(tl ? tl : 1);
        if (tl)
            gget(tx, rd32(th), tl);
        r = with_text(b, n, tx, tl, &rn);
        free(tx);
        break;
    }
    case 2: { /* kTextTextPtr, size bytes */
        uint32_t tl = size > 0 ? (uint32_t)size : 0;
        uint8_t *tx = malloc(tl ? tl : 1);
        if (tl && data)
            gget(tx, data, tl);
        r = with_text(b, n, tx, tl, &rn);
        free(tx);
        break;
    }
    case 3: /* kTextTEStyle: TextStyle (font, face and filler, size, colour) */
        if (!data)
            break;
        put16(e + 8, rd16(data));
        e[10] = rd8(data + 2);
        put16(e + 12, rd16(data + 4));
        for (int k = 0; k < 3; k++)
            put16(e + 14 + k * 2, rd16(data + 6 + 2 * (uint32_t)k));
        goto style;
    case 6: /* kTextForeColor */
        if (data)
            for (int k = 0; k < 3; k++)
                put16(e + 14 + k * 2, rd16(data + 2 * (uint32_t)k));
        goto style;
    case 7: /* kTextFace: the value */
        e[10] = (uint8_t)data;
        goto style;
    case 8: /* kTextFont */
        put16(e + 8, (uint32_t)data);
        goto style;
    case 9: /* kTextSize */
        put16(e + 12, (uint32_t)data);
        put16(e + 4, (uint32_t)(int16_t)data + ((int16_t)data + 3) / 4);
        put16(e + 6, (uint32_t)((int16_t)data - (int16_t)data / 5));
    style: {
        uint8_t st[22];
        put16(st, 1);
        memcpy(st + 2, e, 20);
        r = with_atom(b, n, "styl", st, 22, &rn);
        break;
    }
    case 4:   /* kTextSelection: two longs */
    case 11: { /* kTextHilite: hiliteRecord (two longs, then a colour) */
        if (!data)
            break;
        uint8_t hl[8];
        put32(hl, rd32(data));
        put32(hl + 4, rd32(data + 4));
        r = with_atom(b, n, "hlit", hl, 8, &rn);
        if (prop == 11) {
            uint8_t c[6];
            for (int k = 0; k < 3; k++)
                put16(c + k * 2, rd16(data + 8 + 2 * (uint32_t)k));
            uint32_t r2n;
            uint8_t *r2 = with_atom(r, rn, "hclr", c, 6, &r2n);
            free(r);
            r = r2;
            rn = r2n;
        }
        break;
    }
    case 12: { /* kTextDropShadow: dropShadowRecord (Point offset, short translucency) */
        if (!data)
            break;
        uint8_t off[4], tr[2];
        put16(off, rd16(data + 2)); /* h, v as QuickTime keeps them */
        put16(off + 2, rd16(data));
        put16(tr, rd16(data + 4));
        uint32_t r1n;
        uint8_t *r1 = with_atom(b, n, "drpo", off, 4, &r1n);
        r = with_atom(r1, r1n, "drpt", tr, 2, &rn);
        free(r1);
        break;
    }
    case 20: { /* kTextTextBox */
        if (!data)
            break;
        uint8_t tb[8];
        for (int k = 0; k < 4; k++)
            put16(tb + k * 2, rd16(data + 2 * (uint32_t)k));
        r = with_atom(b, n, "tbox", tb, 8, &rn);
        break;
    }
    case 5:   /* kTextBackColor */
    case 10:  /* kTextAlignment */
    case 13: { /* kTextDisplayFlags */
        int track;
        qtm_movie *mv;
        qt_text *tx = text_drawing(h, &track, &mv);
        if (!tx)
            break;
        if (prop == 5 && data) {
            uint16_t c[3];
            rgb_of(data, c, 0xFFFF);
            qt_text_set_background(tx, c);
        } else if (prop == 10 && data) {
            qt_text_set_justification(tx, (int16_t)rd16(data));
        } else if (prop == 13) {
            qt_text_set_flags(tx, (uint32_t)data);
        }
        qt_native_redraw(qt_movie_native(mv), track);
        qt_movie_redraw(mv);
        free(b);
        return noErr;
    }
    case 14: /* kTextScroll, kTextRelativeScroll: (we don't scroll text) */
    case 15:
    case 16: /* hypertext face and colour */
    case 17:
    case 18: /* kTextKeyEntry, kTextMouseDown, kTextEditState: no text editing in a movie here */
    case 19:
    case 21:
        free(b);
        return featureUnsupported;
    default:
        free(b);
        return paramErr;
    }
    free(b);
    if (!r)
        return paramErr;
    show_text(h, mtime, r, rn);
    free(r);
    return noErr;
}

static int32_t get_text_property(mhlr *h, int32_t mtime, int32_t prop, gaddr data, int32_t size)
{
    uint32_t n;
    uint8_t *b = text_at(h, mtime, NULL, &n, NULL, NULL);
    if (!b)
        return timeNotInMedia;
    uint32_t tl = n >= 2 ? be16(b) : 0;
    if (2 + tl > n)
        tl = n >= 2 ? n - 2 : 0;
    uint8_t e[20];
    first_style(b, n, e);
    int32_t err = noErr;
    switch (prop) {
    case 1: /* kTextTextHandle: into the handle given */
        if (data) {
            tb_SetHandleSize(data, (int32_t)tl);
            if (rd32(data) && tl)
                gput(rd32(data), b + 2, tl);
        }
        break;
    case 2: /* kTextTextPtr */
        if (data && size > 0)
            gput(data, b + 2, tl < (uint32_t)size ? tl : (uint32_t)size);
        break;
    case 22: /* kTextLength */
        if (data)
            wr32(data, tl);
        break;
    case 3: /* kTextTEStyle */
        if (data) {
            wr16(data, (uint16_t)be16(e + 8));
            wr8(data + 2, e[10]);
            wr8(data + 3, 0);
            wr16(data + 4, (uint16_t)be16(e + 12));
            for (int k = 0; k < 3; k++)
                wr16(data + 6 + 2 * (uint32_t)k, (uint16_t)be16(e + 14 + k * 2));
        }
        break;
    case 6: /* kTextForeColor */
        if (data)
            for (int k = 0; k < 3; k++)
                wr16(data + 2 * (uint32_t)k, (uint16_t)be16(e + 14 + k * 2));
        break;
    case 7:
        if (data)
            wr32(data, e[10]);
        break;
    case 8:
        if (data)
            wr32(data, be16(e + 8));
        break;
    case 9:
        if (data)
            wr32(data, be16(e + 12));
        break;
    case 5:
    case 10:
    case 13:
    case 20: {
        int track;
        qtm_movie *mv;
        qt_text *tx = text_drawing(h, &track, &mv);
        if (!tx) {
            err = paramErr;
            break;
        }
        if (prop == 5 && data) {
            uint16_t c[3];
            qt_text_background(tx, c);
            put_rgb(data, c);
        } else if (prop == 10 && data) {
            wr16(data, (uint16_t)qt_text_justification(tx));
        } else if (prop == 13 && data) {
            wr32(data, qt_text_flags(tx));
        } else if (prop == 20 && data) {
            int16_t r[4];
            qt_text_box(tx, r);
            for (int k = 0; k < 4; k++)
                wr16(data + 2 * (uint32_t)k, (uint16_t)r[k]);
        }
        break;
    }
    default:
        err = paramErr;
    }
    free(b);
    return err;
}

static bool chars_match(const uint8_t *a, const uint8_t *b, uint32_t n, bool cased)
{
    for (uint32_t i = 0; i < n; i++)
        if (cased ? a[i] != b[i] : tolower(a[i]) != tolower(b[i]))
            return false;
    return true;
}

/* TextMediaFindNextText(text, size, findFlags, startTime, *foundTime, *foundDuration, *offset) */
static int32_t find_next_text(mhlr *h, gaddr text, int32_t size, int16_t flags, int32_t start, gaddr found_time,
                              gaddr found_dur, gaddr offset)
{
    qtm_movie *mv;
    qtm_media *md = media_of(h, NULL, &mv);
    if (!md)
        return invalidMedia;
    if (found_time)
        wr32(found_time, (uint32_t)-1);
    if (!text || size <= 0)
        return paramErr;
    uint8_t *needle = malloc((size_t)size);
    gget(needle, text, (uint32_t)size);
    bool edge = flags & 1, cased = flags & 2, back = flags & 4, wrap = flags & 8, use_offset = flags & 16;
    uint32_t count = qtm_sample_count(md);
    int64_t s0;
    uint32_t first = qtm_sample_at(md, start, &s0, NULL);
    if (!first)
        first = back ? count : 1;
    uint32_t from_off = use_offset && offset ? rd32(offset) : 0;
    int32_t err = movieTextNotFoundErr;
    for (uint32_t step = 0; step < count && err; step++) {
        int64_t si = back ? (int64_t)first - step : (int64_t)first + step;
        if (si < 1 || si > count) {
            if (!wrap)
                break;
            si = back ? si + count : si - count;
        }
        if (step == 0 && !edge && !use_offset)
            continue; /* not the sample it starts in */
        uint32_t n;
        uint8_t *b = sample_bytes(mv, md, (uint32_t)si, &n, NULL);
        if (!b)
            continue;
        uint32_t tl = n >= 2 ? be16(b) : 0;
        if (2 + tl > n)
            tl = n >= 2 ? n - 2 : 0;
        uint32_t lo = step == 0 && use_offset ? from_off + (back ? 0 : 1) : 0;
        for (int64_t i = back ? (int64_t)(step == 0 && use_offset ? from_off : tl) - size : lo;
             back ? i >= 0 : i + size <= (int64_t)tl; back ? i-- : i++)
            if (chars_match(b + 2 + i, needle, (uint32_t)size, cased)) {
                int64_t st, du;
                qtm_sample_time(md, (uint32_t)si, &st, &du);
                if (found_time)
                    wr32(found_time, (uint32_t)st);
                if (found_dur)
                    wr32(found_dur, (uint32_t)du);
                if (offset)
                    wr32(offset, (uint32_t)i);
                err = noErr;
                break;
            }
        free(b);
    }
    free(needle);
    return err;
}

/* TextMediaDrawRaw / RawSetup / RawIdle: a sample drawn into a GWorld
 * directly. */
static int32_t draw_raw(gaddr gw, gaddr data, int32_t size, gaddr tdh)
{
    if (!gw || !tdh || !rd32(tdh) || !data || size < 2)
        return paramErr;
    uint32_t dl = (uint32_t)tb_GetHandleSize(tdh);
    uint8_t *desc = malloc(dl);
    gget(desc, rd32(tdh), dl);
    gaddr pm = rd32(tb_GetGWorldPixMap(gw));
    qrect bounds = qrect_get(pm + 6);
    int w = bounds.right - bounds.left, hgt = bounds.bottom - bounds.top;
    qt_text *t = qt_text_new(desc, dl, w, hgt);
    free(desc);
    if (!t)
        return paramErr;
    uint8_t *sample = malloc((size_t)size);
    gget(sample, data, (uint32_t)size);
    uint8_t *rgba = malloc((size_t)w * (size_t)hgt * 4);
    gaddr saved = tb_NewPtr(8, false, true);
    tb_GetGWorld(saved, saved + 4);
    bool drawn = qt_text_draw(t, sample, (uint32_t)size, rgba);
    if (drawn) {
        tb_SetGWorld(gw, 0);
        qd_draw_rgba(rgba, w, hgt, bounds);
        tb_SetGWorld(rd32(saved), rd32(saved + 4));
    }
    tb_DisposePtr(saved);
    qt_text_free(t);
    free(sample);
    free(rgba);
    return noErr;
}

/* The text procedure: called before each text sample shows (pascal OSErr
 * (Handle theText, Movie theMovie, short *displayFlag, long refcon)); a
 * displayFlag of dfDontDisplay keeps the sample off the screen. */
static void text_proc_check(mhlr *h)
{
    qtm_movie *mv;
    qtm_track *t;
    qtm_media *md = media_of(h, &t, &mv);
    if (!md || !h->text_proc)
        return;
    int64_t mt = media_now(mv, t);
    uint32_t s = mt >= 0 ? qtm_sample_at(md, mt, NULL, NULL) : 0;
    if ((int64_t)s == h->text_shown)
        return;
    h->text_shown = s;
    if (!s)
        return;
    uint32_t n;
    uint8_t *b = sample_bytes(mv, md, s, &n, NULL);
    if (!b)
        return;
    uint32_t tl = n >= 2 ? be16(b) : 0;
    if (2 + tl > n)
        tl = n >= 2 ? n - 2 : 0;
    gaddr text = qt_dref_handle(b + 2, tl), flag = tb_NewPtr(2, false, true);
    free(b);
    cpu68k *c = g_cpu;
    cpu_push16(c, 0);
    cpu_push32(c, text);
    cpu_push32(c, qt_movie_handle(mv));
    cpu_push32(c, flag);
    cpu_push32(c, h->text_refcon);
    guest_call(h->text_proc);
    cpu_pop16(c);
    if (rd16(flag) & dfDontDisplay) {
        qt_native *nat = qt_movie_native(mv);
        if (nat)
            qt_native_text_override(nat, qt_track_index(mv, t), (int64_t)s - 1, NULL, 0, true);
    }
    tb_DisposeHandle(text);
    tb_DisposePtr(flag);
}

static int32_t text_call(mhlr *h, qtc_call *k)
{
    uint32_t *a = k->a;
    switch (k->what) {
    case 0x0101: /* SetTextProc(proc, refcon) */
        h->text_proc = a[0];
        h->text_refcon = a[1];
        h->text_shown = -1;
        return noErr;
    case 0x0102: { /* AddTextSample(text, size, font, size, face, fg, bg, just, box, flags, scroll, hlS, hlE, hlColor, dur, *time) */
        uint32_t size = a[1];
        uint8_t *t = malloc(size ? size : 1);
        if (size && a[0])
            gget(t, a[0], size);
        int32_t r = add_text_sample(h, t, size, (int16_t)a[2], (int16_t)a[3], (uint8_t)a[4], a[5], a[6],
                                    (int16_t)a[7], a[8], (int32_t)a[9], (int32_t)a[10], (int16_t)a[11],
                                    (int16_t)a[12], a[13], (int32_t)a[14], a[15], NULL, 0);
        free(t);
        return r;
    }
    case 0x0103: { /* AddTESample(hTE, bg, just, box, flags, scroll, hlS, hlE, hlColor, dur, *time) */
        gaddr te = a[0];
        if (!te || !rd32(te))
            return paramErr;
        gaddr tp = rd32(te);
        gaddr hText = rd32(tp + 62); /* hText */
        uint32_t len = rd16(tp + 60); /* teLength */
        uint8_t *t = malloc(len ? len : 1);
        if (len && hText && rd32(hText))
            gget(t, rd32(hText), len);
        int16_t font = (int16_t)rd16(tp + 74), size = (int16_t)rd16(tp + 80); /* txFont, txSize */
        uint8_t face = rd8(tp + 76);
        /* a styled record's styles, as a style scrap */
        uint8_t *scrap = NULL;
        uint32_t scrap_len = 0;
        gaddr st = tb_TEGetStyleScrapHandle(te);
        if (st && rd32(st)) {
            scrap_len = (uint32_t)tb_GetHandleSize(st);
            scrap = malloc(scrap_len);
            gget(scrap, rd32(st), scrap_len);
            tb_DisposeHandle(st);
        }
        int32_t r = add_text_sample(h, t, len, font, size, face, 0, a[1], (int16_t)a[2], a[3], (int32_t)a[4],
                                    (int32_t)a[5], (int16_t)a[6], (int16_t)a[7], a[8], (int32_t)a[9], a[10], scrap,
                                    scrap_len);
        free(t);
        free(scrap);
        return r;
    }
    case 0x0104: { /* AddHiliteSample(hlS, hlE, hlColor, dur, *time): the last sample's text, highlighted */
        qtm_movie *mv;
        qtm_media *md = media_of(h, NULL, &mv);
        if (!md)
            return invalidMedia;
        uint32_t count = qtm_sample_count(md), n = 0, desc = 0;
        uint8_t *b = count ? sample_bytes(mv, md, count, &n, &desc) : NULL;
        if (!b)
            return invalidSampleNum;
        uint8_t hl[8];
        put32(hl, (uint32_t)(int16_t)a[0]);
        put32(hl + 4, (uint32_t)(int16_t)a[1]);
        uint32_t rn;
        uint8_t *r = with_atom(b, n, "hlit", hl, 8, &rn);
        free(b);
        if (a[2]) {
            uint8_t c[6];
            for (int i = 0; i < 3; i++)
                put16(c + i * 2, rd16(a[2] + 2 * (uint32_t)i));
            uint32_t r2n;
            uint8_t *r2 = with_atom(r, rn, "hclr", c, 6, &r2n);
            free(r);
            r = r2;
            rn = r2n;
        }
        gaddr sh = qt_dref_handle(r, rn), dh = tb_NewHandle(0, false, false);
        free(r);
        tb_GetMediaSampleDescription((int32_t)h->media, (int32_t)desc, (int32_t)dh);
        int16_t err = tb_AddMediaSample((int32_t)h->media, (int32_t)sh, 0, (int32_t)rn, (int32_t)a[3], (int32_t)dh,
                                        1, 0, a[4]);
        tb_DisposeHandle(sh);
        tb_DisposeHandle(dh);
        return err;
    }
    case 0x0109: /* DrawRaw(gw, gd, data, size, tdh) */
        return draw_raw(a[0], a[2], (int32_t)a[3], a[4]);
    case 0x010B: { /* RawSetup(gw, gd, data, size, tdh, duration) */
        h->raw_gw = a[0];
        h->raw_gd = a[1];
        free(h->raw);
        h->nraw = (uint32_t)a[3];
        h->raw = malloc(h->nraw ? h->nraw : 1);
        if (h->nraw && a[2])
            gget(h->raw, a[2], h->nraw);
        if (h->raw_desc)
            tb_DisposeHandle(h->raw_desc);
        uint32_t dl;
        uint8_t *d = qt_dref_bytes(a[4], &dl);
        h->raw_desc = qt_dref_handle(d, dl);
        free(d);
        return noErr;
    }
    case 0x010C: { /* RawIdle(gw, gd, sampleTime, flagsIn, *flagsOut) */
        int32_t r = noErr;
        if (h->raw && h->raw_desc) {
            gaddr p = tb_NewPtr((int32_t)(h->nraw ? h->nraw : 1), false, false);
            gput(p, h->raw, h->nraw);
            r = draw_raw(a[0] ? a[0] : h->raw_gw, p, (int32_t)h->nraw, h->raw_desc);
            tb_DisposePtr(p);
        }
        if (a[4])
            wr32(a[4], mediaChanged);
        return r;
    }
    case 0x010A: /* SetTextProperty(atMediaTime, type, data, size) */
        return set_text_property(h, (int32_t)a[0], (int32_t)a[1], a[2], (int32_t)a[3]);
    case 0x010D: /* GetTextProperty(atMediaTime, type, data, size) */
        return get_text_property(h, (int32_t)a[0], (int32_t)a[1], a[2], (int32_t)a[3]);
    case 0x0105: /* FindNextText */
        return find_next_text(h, a[0], (int32_t)a[1], (int16_t)a[2], (int32_t)a[3], a[4], a[5], a[6]);
    case 0x0106: { /* HiliteTextSample(sampleTime, hlS, hlE, color) */
        uint32_t n;
        uint8_t *b = text_at(h, (int32_t)a[0], NULL, &n, NULL, NULL);
        if (!b)
            return timeNotInMedia;
        uint8_t hl[8];
        put32(hl, (uint32_t)(int16_t)a[1]);
        put32(hl + 4, (uint32_t)(int16_t)a[2]);
        uint32_t rn;
        uint8_t *r = with_atom(b, n, "hlit", hl, 8, &rn);
        free(b);
        if (a[3]) {
            uint8_t c[6];
            for (int i = 0; i < 3; i++)
                put16(c + i * 2, rd16(a[3] + 2 * (uint32_t)i));
            uint32_t r2n;
            uint8_t *r2 = with_atom(r, rn, "hclr", c, 6, &r2n);
            free(r);
            r = r2;
            rn = r2n;
        }
        show_text(h, (int32_t)a[0], r, rn);
        free(r);
        return noErr;
    }
    case 0x0107: { /* SetTextSampleData(data, dataType): an atom for the next sample */
        uint32_t len;
        switch (a[1]) {
        case 0x6472706F: len = 4; break; /* 'drpo' dropShadowOffsetType: a Point */
        case 0x64727074: len = 2; break; /* 'drpt' dropShadowTranslucencyType: a short */
        case 0x74626F78: len = 8; break; /* 'tbox' */
        case 0x646C6179: len = 4; break; /* 'dlay' */
        case 0x74777270: len = 1; break; /* 'twrp' */
        default: return paramErr;
        }
        char type[4] = { (char)(a[1] >> 24), (char)(a[1] >> 16), (char)(a[1] >> 8), (char)a[1] };
        uint8_t v[8] = { 0 };
        if (a[0])
            gget(v, a[0], len);
        h->pending = realloc(h->pending, h->npending + 8 + len);
        put32(h->pending + h->npending, 8 + len);
        memcpy(h->pending + h->npending + 4, type, 4);
        memcpy(h->pending + h->npending + 8, v, len);
        h->npending += 8 + len;
        return noErr;
    }
    }
    return qtcBadSelector;
}

/* ---- Timecode ---- */

typedef struct {
    int32_t flags, scale, frame_dur;
    uint8_t frames;
} tcdef;

static bool read_tcdef(gaddr p, tcdef *d)
{
    if (!p)
        return false;
    d->flags = (int32_t)rd32(p);
    d->scale = (int32_t)rd32(p + 4);
    d->frame_dur = (int32_t)rd32(p + 8);
    d->frames = rd8(p + 12);
    return d->frames || (d->flags & tcCounter);
}

static void write_tcdef(gaddr p, const tcdef *d)
{
    if (!p)
        return;
    wr32(p, (uint32_t)d->flags);
    wr32(p + 4, (uint32_t)d->scale);
    wr32(p + 8, (uint32_t)d->frame_dur);
    wr8(p + 12, d->frames);
    wr8(p + 13, 0);
}

/* A frame number as hours, minutes, seconds and frames, drop frame or not. */
static void frame_to_tc(const tcdef *d, int32_t frame, uint8_t out[4])
{
    bool neg = frame < 0;
    int64_t f = neg ? -(int64_t)frame : frame;
    int fps = d->frames ? d->frames : 30;
    if ((d->flags & tcDropFrame) && fps == 30) {
        /* two frame numbers dropped every minute but the tenth */
        int64_t per10 = 17982, per1 = 1798;
        int64_t tens = f / per10, rem = f % per10;
        f += 18 * tens + (rem > 1 ? 2 * ((rem - 2) / per1) : 0);
    }
    int64_t h = f / (fps * 3600), m = f / (fps * 60) % 60, s = f / fps % 60, fr = f % fps;
    if (d->flags & tc24HourMax)
        h %= 24;
    out[0] = (uint8_t)h;
    out[1] = (uint8_t)(m | (neg ? tctNegFlag : 0));
    out[2] = (uint8_t)s;
    out[3] = (uint8_t)fr;
}

static int32_t tc_to_frame(const tcdef *d, const uint8_t in[4])
{
    int fps = d->frames ? d->frames : 30;
    bool neg = in[1] & tctNegFlag;
    int64_t h = in[0], m = in[1] & 0x7F, s = in[2], fr = in[3];
    int64_t f = ((h * 60 + m) * 60 + s) * fps + fr;
    if ((d->flags & tcDropFrame) && fps == 30) {
        int64_t mins = h * 60 + m;
        f -= 2 * (mins - mins / 10);
    }
    return (int32_t)(neg ? -f : f);
}

/* The timecode sample's frame number at a media time, and its description. */
static int32_t tc_at(mhlr *h, int64_t mt, int32_t *frame, tcdef *def, gaddr srcref)
{
    qtm_movie *mv;
    qtm_media *md = media_of(h, NULL, &mv);
    if (!md)
        return invalidMedia;
    int64_t start, dur;
    uint32_t s = qtm_sample_at(md, mt, &start, &dur);
    if (!s)
        return timeNotInMedia;
    uint32_t n, desc;
    uint8_t *b = sample_bytes(mv, md, s, &n, &desc);
    if (!b || n < 4 || desc < 1 || (int)desc > md->ndescs || md->desc_len[desc - 1] < 34) {
        free(b);
        return invalidSampleDescription;
    }
    const uint8_t *d = md->descs[desc - 1];
    def->flags = (int32_t)be32(d + 20);
    def->scale = (int32_t)be32(d + 24);
    def->frame_dur = (int32_t)be32(d + 28);
    def->frames = d[32];
    int64_t off = def->frame_dur > 0 && def->scale > 0
                      ? (mt - start) * def->scale / md->scale / def->frame_dur
                      : 0;
    *frame = (int32_t)be32(b) + (int32_t)off;
    free(b);
    if (srcref) { /* the source reference: user data after the definition */
        uint32_t dl = md->desc_len[desc - 1];
        gaddr ud = dl > 34 + 4 ? qt_udata_from(d + 34 + 4, dl - 34 - 4) : qt_udata_new();
        wr32(srcref, ud);
    }
    return noErr;
}

static int32_t tc_call(mhlr *h, qtc_call *k)
{
    uint32_t *a = k->a;
    tcdef d;
    int32_t frame;
    switch (k->what) {
    case 0x0101: /* GetCurrentTimeCode(*frameNum, *tcdef, *tcrec, *srcRef) */
    case 0x0102: { /* GetTimeCodeAtTime(mediaTime, ...) */
        int off = k->what == 0x0102;
        int64_t mt;
        if (off) {
            mt = (int32_t)a[0];
        } else {
            qtm_movie *mv;
            qtm_track *t;
            if (!media_of(h, &t, &mv))
                return invalidMedia;
            mt = media_now(mv, t);
        }
        int32_t err = tc_at(h, mt, &frame, &d, a[3 + off]);
        if (err)
            return err;
        if (a[off])
            wr32(a[off], (uint32_t)frame);
        write_tcdef(a[1 + off], &d);
        if (a[2 + off]) {
            if (d.flags & tcCounter) {
                wr32(a[2 + off], (uint32_t)(frame / (d.frames ? d.frames : 1)));
            } else {
                uint8_t t[4];
                frame_to_tc(&d, frame, t);
                gput(a[2 + off], t, 4);
            }
        }
        return noErr;
    }
    case 0x0103: { /* TimeCodeToString(tcdef, tcrec, str) */
        if (!read_tcdef(a[0], &d) || !a[1] || !a[2])
            return paramErr;
        char s[32];
        if (d.flags & tcCounter) {
            snprintf(s, sizeof s, "%d", (int32_t)rd32(a[1]));
        } else {
            uint8_t t[4];
            gget(t, a[1], 4);
            snprintf(s, sizeof s, "%s%02d:%02d:%02d%c%02d", (t[1] & tctNegFlag) ? "-" : "", t[0], t[1] & 0x7F, t[2],
                     (d.flags & tcDropFrame) ? ';' : ':', t[3]);
        }
        wr8(a[2], (uint8_t)strlen(s));
        gput(a[2] + 1, s, (uint32_t)strlen(s));
        return noErr;
    }
    case 0x0104: { /* TimeCodeToFrameNumber(tcdef, tcrec, *frame) */
        if (!read_tcdef(a[0], &d) || !a[1])
            return paramErr;
        uint8_t t[4];
        gget(t, a[1], 4);
        int32_t f = (d.flags & tcCounter) ? (int32_t)rd32(a[1]) * (d.frames ? d.frames : 1) : tc_to_frame(&d, t);
        if (a[2])
            wr32(a[2], (uint32_t)f);
        return noErr;
    }
    case 0x0105: { /* FrameNumberToTimeCode(frame, tcdef, *tcrec) */
        if (!read_tcdef(a[1], &d) || !a[2])
            return paramErr;
        if (d.flags & tcCounter) {
            wr32(a[2], (uint32_t)((int32_t)a[0] / (d.frames ? d.frames : 1)));
        } else {
            uint8_t t[4];
            frame_to_tc(&d, (int32_t)a[0], t);
            gput(a[2], t, 4);
        }
        return noErr;
    }
    case 0x0106: { /* GetSourceRef(tcdH, *srcRef) */
        uint32_t dl;
        uint8_t *b = qt_dref_bytes(a[0], &dl);
        gaddr ud = dl > 38 ? qt_udata_from(b + 38, dl - 38) : qt_udata_new();
        free(b);
        if (a[1])
            wr32(a[1], ud);
        return noErr;
    }
    case 0x0107: { /* SetSourceRef(tcdH, srcRef): the description with the user data after it */
        gaddr tcd = a[0];
        if (!tcd || !rd32(tcd))
            return paramErr;
        uint32_t un;
        uint8_t *u = qt_udata_flatten(a[1], &un);
        tb_SetHandleSize(tcd, (int32_t)(38 + un));
        if (rd32(tcd)) {
            wr32(rd32(tcd), 38 + un);
            gput(rd32(tcd) + 38, u, un);
        }
        free(u);
        return noErr;
    }
    case 0x0108: /* SetTimeCodeFlags(flags, mask) */
        h->tc_flags = (h->tc_flags & ~(int32_t)a[1]) | ((int32_t)a[0] & (int32_t)a[1]);
        return noErr;
    case 0x0109: /* GetTimeCodeFlags(*flags) */
        if (a[0])
            wr32(a[0], (uint32_t)h->tc_flags);
        return noErr;
    case 0x010A: /* SetDisplayOptions(options) */
        if (a[0])
            gget(h->tc_options, a[0], 20);
        return noErr;
    case 0x010B: /* GetDisplayOptions(options) */
        if (a[0])
            gput(a[0], h->tc_options, 20);
        return noErr;
    }
    return qtcBadSelector;
}

/* ---- Video ---- */

static int32_t video_call(mhlr *h, qtc_call *k)
{
    uint32_t *a = k->a;
    switch (k->what) {
    case 0x0105: /* ResetStatistics */
        h->frames_drawn = 0;
        h->stalls = 0;
        return noErr;
    case 0x0106: { /* GetStatistics: the frame rate (Fixed) of what played */
        qtm_movie *mv;
        qtm_media *md = media_of(h, NULL, &mv);
        if (!md || !md->duration)
            return 0;
        return (int32_t)((int64_t)qtm_sample_count(md) * md->scale * 0x10000 / md->duration);
    }
    case 0x010E: /* GetStallCount(*stalls) */
        if (a[0])
            wr32(a[0], h->stalls);
        return noErr;
    case 0x010F: { /* SetCodecParameter(cType, id, seed, data, size) */
        int at = -1;
        for (int i = 0; i < 8; i++)
            if (h->codec_params[i].data && h->codec_params[i].codec == a[0] && h->codec_params[i].id == a[1])
                at = i;
        for (int i = 0; i < 8 && at < 0; i++)
            if (!h->codec_params[i].data)
                at = i;
        if (at < 0)
            return memFullErr;
        free(h->codec_params[at].data);
        h->codec_params[at].codec = a[0];
        h->codec_params[at].id = a[1];
        h->codec_params[at].len = a[4];
        h->codec_params[at].data = malloc(a[4] ? a[4] : 1);
        if (a[4] && a[3])
            gget(h->codec_params[at].data, a[3], a[4]);
        return noErr;
    }
    case 0x0110: /* GetCodecParameter(cType, id, outHandle) */
        for (int i = 0; i < 8; i++)
            if (h->codec_params[i].data && h->codec_params[i].codec == a[0] && h->codec_params[i].id == a[1]) {
                if (a[2]) {
                    tb_SetHandleSize(a[2], (int32_t)h->codec_params[i].len);
                    if (rd32(a[2]))
                        gput(rd32(a[2]), h->codec_params[i].data, h->codec_params[i].len);
                }
                return noErr;
            }
        return paramErr;
    }
    return qtcBadSelector;
}

/* ---- Music ---- */

static int32_t music_call(mhlr *h, qtc_call *k)
{
    if (k->what != 0x0101) /* GetIndexedTunePlayer(sampleDescIndex, *tp) */
        return qtcBadSelector;
    qtm_movie *mv;
    qtm_media *md = media_of(h, NULL, &mv);
    if (!md)
        return invalidMedia;
    int32_t index = (int32_t)k->a[0];
    if (index < 1 || index > md->ndescs)
        return invalidSampleDescIndex;
    uint32_t tp = components_open_default(fourcc("tune"), 0);
    if (!tp)
        return cantOpenHandler;
    /* the description's tune header: after its 16 bytes and a flags long */
    uint32_t dl = md->desc_len[index - 1];
    if (dl > 20) {
        gaddr hdr = tb_NewPtr((int32_t)(dl - 20), false, false);
        gput(hdr, md->descs[index - 1] + 20, dl - 20);
        uint32_t args[] = { hdr, dl - 20 };
        qtc_call_named(tp, "TuneSetHeaderWithSize", args);
        tb_DisposePtr(hdr);
    }
    if (k->a[1])
        wr32(k->a[1], tp);
    return noErr;
}

/* ---- Movies in movies (QuickTime 4's movie media) ---- */

static int32_t movie_media_call(mhlr *h, qtc_call *k)
{
    uint32_t *a = k->a;
    switch (k->what) {
    case 0x0102: /* GetChildDoMCActionCallback(*proc, *refcon) */
    case 0x0103: /* GetDoMCActionCallback */
        if (a[0])
            wr32(a[0], h->mcaction_proc);
        if (a[1])
            wr32(a[1], h->mcaction_refcon);
        return noErr;
    case 0x0104: { /* GetCurrentMovieProperty(which, *value) */
        qtm_movie *mv;
        if (!media_of(h, NULL, &mv))
            return invalidMedia;
        gaddr m = qt_movie_handle(mv);
        switch (a[0]) {
        case 0x6D74696D: /* 'mtim' kMovieMediaCurrentTime? the movie's time */
            if (a[1])
                wr32(a[1], (uint32_t)qt_movie_now(mv));
            return noErr;
        case 0x6D62726E: /* 'mbrn' */
        default:
            (void)m;
            return paramErr;
        }
    }
    case 0x0105: /* GetCurrentTrackProperty(trackID, which, *value) */
        return paramErr;
    case 0x0106: /* GetChildMovieDataReference(id, index, *type, *ref, *idOut, *indexOut) */
    case 0x0107: /* SetChildMovieDataReference(id, type, ref) */
    case 0x0108: /* LoadChildMovieFromDataReference(id) */
        /* the samples' data references: movie media atoms Blue Box does not play */
        return featureUnsupported;
    }
    return qtcBadSelector;
}

/* ---- Sprites: what is showing ---- */

enum {
    kSpritePropertyMatrix = 1, kSpritePropertyImageDescription = 2, kSpritePropertyImageDataPtr = 3,
    kSpritePropertyVisible = 4, kSpritePropertyLayer = 5, kSpritePropertyGraphicsMode = 6,
    kSpritePropertyImageDataSize = 7, kSpritePropertyActionHandlingSpriteID = 8, kSpritePropertyCanBeHitTested = 9,
    kSpritePropertyImageIndex = 100, kSpriteTrackPropertyBackgroundColor = 101,
    kSpriteTrackPropertyOffscreenBitDepth = 102, kSpriteTrackPropertySampleFormat = 103,
    kSpriteTrackPropertyScaleSpritesToScaleWorld = 104, kSpriteTrackPropertyHasActions = 105,
    kSpriteTrackPropertyVisible = 106, kSpriteTrackPropertyQTIdleEventsFrequency = 107,
    kSpriteImagePropertyRegistrationPoint = 1000, kSpriteImagePropertyGroupID = 1001,
    spriteHitTestBounds = 1, spriteHitTestImage = 2, spriteHitTestInvisibleSprites = 4,
    spriteHitTestLocInDisplayCoordinates = 1 << 4,
};

static qt_sprite *sprites_of(mhlr *h, qtm_movie **mvp, int *track)
{
    qtm_movie *mv;
    qtm_track *t;
    if (!media_of(h, &t, &mv))
        return NULL;
    *mvp = mv;
    *track = qt_track_index(mv, t);
    return qt_native_sprite(qt_movie_native(mv), *track);
}

static void sprites_changed(qtm_movie *mv, int track)
{
    qt_native_redraw(qt_movie_native(mv), track);
    qt_movie_redraw(mv);
}

/* A property's value in, as the program gives it: by value for the short
 * ones (cast to a pointer), by pointer for the matrix and graphics mode. */
static bool property_in(uint32_t prop, gaddr v, uint8_t *out, uint32_t *len)
{
    switch (prop) {
    case kSpritePropertyMatrix:
        if (!v)
            return false;
        gget(out, v, 36);
        *len = 36;
        return true;
    case kSpritePropertyGraphicsMode:
        if (!v)
            return false;
        gget(out, v, 10);
        *len = 10;
        return true;
    case kSpritePropertyVisible:
    case kSpritePropertyLayer:
    case kSpritePropertyImageIndex:
        put16(out, (uint16_t)v);
        *len = 2;
        return true;
    }
    return false;
}

static int32_t get_sprite_property(qt_sprite *s, uint32_t id, uint32_t prop, gaddr v)
{
    uint8_t b[40];
    uint32_t n;
    if (prop == kSpritePropertyCanBeHitTested || prop == kSpritePropertyActionHandlingSpriteID) {
        if (!qt_sprite_index(s, id))
            return invalidSpriteIDErr;
        if (v)
            wr8(v, prop == kSpritePropertyCanBeHitTested ? 1 : 0);
        return noErr;
    }
    if (!qt_sprite_get(s, id, prop, b, &n))
        return qt_sprite_index(s, id) ? invalidSpritePropertyErr : invalidSpriteIDErr;
    if (!v)
        return noErr;
    if (prop == kSpritePropertyVisible) /* a Boolean */
        wr8(v, b[1]);
    else
        gput(v, b, n);
    return noErr;
}

static int32_t sprite_call(mhlr *h, qtc_call *k)
{
    uint32_t *a = k->a;
    qtm_movie *mv;
    int track;
    qt_sprite *s = sprites_of(h, &mv, &track);
    switch (k->what) {
    case 0x0102: /* GetProperty(spriteIndex, type, value): sprites by index (QuickTime 2) */
        if (!s)
            return invalidSpriteIndexErr;
        return get_sprite_property(s, qt_sprite_id(s, (int16_t)a[0]), a[1], a[2]);
    case 0x0101: /* SetProperty(spriteIndex, type, value) */
    case 0x010A: { /* SetSpriteProperty(spriteID, type, value) */
        if (!s)
            return k->what == 0x0101 ? invalidSpriteIndexErr : invalidSpriteIDErr;
        uint32_t id = k->what == 0x0101 ? qt_sprite_id(s, (int16_t)a[0]) : a[0];
        uint8_t b[40];
        uint32_t n;
        if (!qt_sprite_index(s, id))
            return k->what == 0x0101 ? invalidSpriteIndexErr : invalidSpriteIDErr;
        if (!property_in(a[1], a[2], b, &n) || !qt_sprite_set(s, id, a[1], b, n))
            return invalidSpritePropertyErr;
        sprites_changed(mv, track);
        return noErr;
    }
    case 0x010B: /* GetSpriteProperty(spriteID, type, value) */
        if (!s)
            return invalidSpriteIDErr;
        return get_sprite_property(s, a[0], a[1], a[2]);
    case 0x0103: { /* HitTestSprites(flags, loc, *spriteHitIndex) */
        if (a[2])
            wr16(a[2], 0);
        if (!s)
            return noErr;
        uint32_t id = qt_sprite_hit(s, (int16_t)a[1], (int16_t)(a[1] >> 16), a[0] & spriteHitTestImage,
                                    a[0] & spriteHitTestInvisibleSprites);
        if (a[2])
            wr16(a[2], (uint16_t)qt_sprite_index(s, id));
        return noErr;
    }
    case 0x010C: { /* HitTestAllSprites(flags, loc, *spriteHitID) */
        if (a[2])
            wr32(a[2], 0);
        if (!s)
            return noErr;
        uint32_t id = qt_sprite_hit(s, (int16_t)a[1], (int16_t)(a[1] >> 16), a[0] & spriteHitTestImage,
                                    a[0] & spriteHitTestInvisibleSprites);
        if (a[2])
            wr32(a[2], id);
        return noErr;
    }
    case 0x010D: /* HitTestOneSprite(spriteID, flags, loc, *wasHit) */
        if (a[3])
            wr8(a[3], s && qt_sprite_hits(s, a[0], (int16_t)a[2], (int16_t)(a[2] >> 16), a[1] & spriteHitTestImage,
                                          a[1] & spriteHitTestInvisibleSprites));
        return s && qt_sprite_index(s, a[0]) ? noErr : invalidSpriteIDErr;
    case 0x0104: /* CountSprites(*n) */
        if (a[0])
            wr16(a[0], s ? (uint16_t)qt_sprite_count(s) : 0);
        return noErr;
    case 0x0105: /* CountImages(*n) */
        if (a[0])
            wr16(a[0], s ? (uint16_t)qt_sprite_image_count(s) : 0);
        return noErr;
    case 0x010E: /* SpriteIndexToID(index, *id) */
        if (!s || !qt_sprite_id(s, (int16_t)a[0]))
            return invalidSpriteIndexErr;
        if (a[1])
            wr32(a[1], qt_sprite_id(s, (int16_t)a[0]));
        return noErr;
    case 0x010F: /* SpriteIDToIndex(id, *index) */
        if (!s || !qt_sprite_index(s, a[0]))
            return invalidSpriteIDErr;
        if (a[1])
            wr16(a[1], (uint16_t)qt_sprite_index(s, a[0]));
        return noErr;
    case 0x0107: { /* GetDisplayedSampleNumber(*n) */
        int64_t shown = qt_native_showing(qt_movie_native(mv), track);
        if (a[0])
            wr32(a[0], shown >= 0 ? (uint32_t)(shown + 1) : 0);
        return noErr;
    }
    case 0x0113: { /* GetIndImageProperty(imageIndex, type, *value) */
        int w, hgt;
        double rx, ry;
        if (!s || !qt_sprite_image_info(s, (int16_t)a[0], &w, &hgt, &rx, &ry))
            return invalidImageIndexErr;
        if (a[1] == kSpriteImagePropertyRegistrationPoint && a[2]) {
            wr32(a[2], (uint32_t)(int32_t)lround(rx * 65536));
            wr32(a[2] + 4, (uint32_t)(int32_t)lround(ry * 65536));
            return noErr;
        }
        if (a[1] == kSpriteImagePropertyGroupID && a[2]) {
            wr32(a[2], 0);
            return noErr;
        }
        return invalidSpritePropertyErr;
    }
    case 0x0106: { /* GetIndImageDescription(imageIndex, ImageDescriptionHandle) */
        int w, hgt;
        double rx, ry;
        if (!s || !qt_sprite_image_info(s, (int16_t)a[0], &w, &hgt, &rx, &ry))
            return invalidImageIndexErr;
        if (a[1]) { /* an uncompressed 32-bit picture of its size */
            tb_SetHandleSize(a[1], 86);
            gaddr d = rd32(a[1]);
            for (int i = 0; d && i < 86; i++)
                wr8(d + (uint32_t)i, 0);
            if (d) {
                wr32(d, 86);
                wr32(d + 4, fourcc("raw "));
                wr16(d + 32, (uint16_t)w);
                wr16(d + 34, (uint16_t)hgt);
                wr32(d + 36, 72 << 16);
                wr32(d + 40, 72 << 16);
                wr16(d + 48, 1);
                wr16(d + 82, 32);
                wr16(d + 84, 0xFFFF);
            }
        }
        return noErr;
    }
    case 0x0108: /* GetSpriteName(id, str) */
    case 0x0109: /* GetImageName(index, str) */
        if (a[1])
            wr8(a[1], 0); /* (names are in the key frame's atoms, which we don't keep) */
        if (k->what == 0x0108 ? !s || !qt_sprite_index(s, a[0]) : !s || (int16_t)a[0] < 1 ||
                                                                     (int16_t)a[0] > qt_sprite_image_count(s))
            return k->what == 0x0108 ? invalidSpriteIDErr : invalidImageIndexErr;
        return noErr;
    case 0x0114: { /* NewSprite(QTRuntimeSpriteDescPtr): version, ID, image index, matrix*, visible, layer, mode */
        if (!s || !a[0])
            return paramErr;
        gaddr d = a[0];
        uint32_t id = rd32(d + 4);
        int16_t image = (int16_t)rd16(d + 8);
        gaddr mp = rd32(d + 10);
        double m[6] = { 1, 0, 0, 1, 0, 0 };
        if (mp) {
            m[0] = (int32_t)rd32(mp) / 65536.0;
            m[1] = (int32_t)rd32(mp + 4) / 65536.0;
            m[2] = (int32_t)rd32(mp + 12) / 65536.0;
            m[3] = (int32_t)rd32(mp + 16) / 65536.0;
            m[4] = (int32_t)rd32(mp + 24) / 65536.0;
            m[5] = (int32_t)rd32(mp + 28) / 65536.0;
        }
        bool visible = rd8(d + 14) != 0;
        int16_t layer = (int16_t)rd16(d + 16);
        gaddr gm = rd32(d + 18);
        int32_t mode = gm ? (int32_t)rd32(gm) : 0;
        uint16_t op[3] = { gm ? rd16(gm + 4) : 0, gm ? rd16(gm + 6) : 0, gm ? rd16(gm + 8) : 0 };
        if (!qt_sprite_add(s, id, (uint32_t)image, m, visible, layer, mode, op))
            return invalidSpriteIDErr;
        sprites_changed(mv, track);
        return noErr;
    }
    case 0x0115: /* DisposeSprite(id) */
        if (!s || !qt_sprite_remove(s, a[0]))
            return invalidSpriteIDErr;
        sprites_changed(mv, track);
        return noErr;
    case 0x0110: /* GetSpriteActionsForQTEvent: (no actions without the wired-action engine's atoms) */
        if (a[2])
            wr32(a[2], 0);
        if (a[3])
            wr32(a[3], 0);
        return noErr;
    case 0x0111: { /* SetActionVariable(id, const float *value) */
        float f = 0;
        if (a[1]) {
            uint32_t bits = rd32(a[1]);
            memcpy(&f, &bits, 4);
        }
        for (int i = 0; i < h->nvariables; i++)
            if (h->variables_id[i] == a[0]) {
                h->variables_value[i] = f;
                return noErr;
            }
        if (h->nvariables == 64)
            return memFullErr;
        h->variables_id[h->nvariables] = a[0];
        h->variables_value[h->nvariables++] = f;
        return noErr;
    }
    case 0x0112: { /* GetActionVariable(id, float *value) */
        for (int i = 0; i < h->nvariables; i++)
            if (h->variables_id[i] == a[0]) {
                uint32_t bits;
                memcpy(&bits, &h->variables_value[i], 4);
                if (a[1])
                    wr32(a[1], bits);
                return noErr;
            }
        if (a[1])
            wr32(a[1], 0);
        return noErr;
    }
    case 0x0116: { /* SetActionVariableToString(id, cstring): its number */
        char buf[64] = "";
        for (int i = 0; a[1] && i < 63; i++)
            if (!(buf[i] = (char)rd8(a[1] + (uint32_t)i)))
                break;
        float f = (float)atof(buf);
        uint32_t bits;
        memcpy(&bits, &f, 4);
        gaddr p = tb_NewPtr(4, false, false);
        wr32(p, bits);
        qtc_call c2 = *k;
        c2.what = 0x0111;
        c2.a[1] = p;
        int32_t r = sprite_call(h, &c2);
        tb_DisposePtr(p);
        return r;
    }
    case 0x0117: { /* GetActionVariableAsString(id, *handle) */
        float f = 0;
        for (int i = 0; i < h->nvariables; i++)
            if (h->variables_id[i] == a[0])
                f = h->variables_value[i];
        char buf[64];
        snprintf(buf, sizeof buf, "%g", (double)f);
        if (a[1])
            wr32(a[1], qt_dref_handle((const uint8_t *)buf, (uint32_t)strlen(buf) + 1));
        return noErr;
    }
    }
    return qtcBadSelector;
}

/* ---- The base calls ---- */

static int32_t base_call(mhlr *h, qtc_call *k)
{
    uint32_t *a = k->a;
    qtm_movie *mv;
    qtm_track *t;
    qtm_media *md = media_of(h, &t, &mv);
    switch (k->what) {
    case 0x0501: /* Initialize(GetMovieCompleteParams *) */
        if (a[0]) {
            h->media = rd32(a[0] + 10);
            h->volume = (int16_t)rd16(a[0] + 42);
            h->width = (int32_t)rd32(a[0] + 44);
            h->height = (int32_t)rd32(a[0] + 48);
            for (int i = 0; i < 9; i++)
                h->matrix[i] = (int32_t)rd32(a[0] + 52 + 4 * (uint32_t)i);
            h->port = rd32(a[0] + 88);
            h->gd = rd32(a[0] + 92);
        }
        return noErr;
    case 0x0502: /* SetHandlerCapabilities(flags, mask) */
        h->caps = (h->caps & ~(int32_t)a[1]) | ((int32_t)a[0] & (int32_t)a[1]);
        return noErr;
    case 0x0503: /* Idle(atMediaTime, flagsIn, *flagsOut, movieTime) */
        if (h->type == fourcc("text"))
            text_proc_check(h);
        if (a[2])
            wr32(a[2], 0);
        return noErr;
    case 0x0504: /* GetMediaInfo(h): the media's property atom, if any */
    case 0x0505: /* PutMediaInfo(h) */
        if (!md)
            return invalidMedia;
        if (k->what == 0x0504) {
            gaddr pa = 0;
            gaddr out = tb_NewPtr(4, false, true);
            tb_GetMediaPropertyAtom((int32_t)h->media, out);
            pa = rd32(out);
            tb_DisposePtr(out);
            uint32_t n = pa ? (uint32_t)tb_GetHandleSize(pa) : 0;
            if (a[0]) {
                tb_SetHandleSize(a[0], (int32_t)n);
                if (n && rd32(a[0]))
                    gmemcpy(rd32(a[0]), rd32(pa), n);
            }
            if (pa)
                tb_DisposeHandle(pa);
            return noErr;
        }
        return tb_SetMediaPropertyAtom((int32_t)h->media, (int32_t)a[0]);
    case 0x0506: /* SetActive */
        h->active = (a[0] & 0xFF) != 0;
        return noErr;
    case 0x0507: /* SetRate */
        h->rate = (int32_t)a[0];
        return noErr;
    case 0x0508: /* GGetStatus(*statusErr) */
        if (a[0])
            wr32(a[0], 0);
        return noErr;
    case 0x0509: /* TrackEdited */
    case 0x050A: /* SetMediaTimeScale */
    case 0x050B: /* SetMovieTimeScale */
    case 0x0519: /* SampleDescriptionChanged */
    case 0x0525: /* TrackReferencesChanged */
    case 0x0528: /* TrackPropertyAtomChanged */
    case 0x054F: /* EnterEmptyEdit */
    case 0x055D: /* DoIdleActions */
    case 0x0560: /* TimeBaseChanged */
    case 0x0518: /* Preroll */
        return noErr;
    case 0x050C: /* SetGWorld(port, gd) */
        h->port = a[0];
        h->gd = a[1];
        return noErr;
    case 0x050D: /* SetDimensions(width, height) */
        h->width = (int32_t)a[0];
        h->height = (int32_t)a[1];
        return noErr;
    case 0x050E: /* SetClip(rgn) */
        if (h->clip)
            tb_DisposeRgn(h->clip);
        h->clip = 0;
        if (a[0]) {
            h->clip = tb_NewRgn();
            tb_CopyRgn(a[0], h->clip);
        }
        return noErr;
    case 0x050F: /* SetMatrix(matrix) */
        for (int i = 0; a[0] && i < 9; i++)
            h->matrix[i] = (int32_t)rd32(a[0] + 4 * (uint32_t)i);
        return noErr;
    case 0x0510: /* GetTrackOpaque(*opaque): video and sprites cover their box */
        if (a[0])
            wr8(a[0], h->type == fourcc("vide") || h->type == fourcc("sprt"));
        return noErr;
    case 0x0511: /* SetGraphicsMode(mode, opColor) */
        h->mode = (int32_t)a[0];
        for (int i = 0; a[1] && i < 3; i++)
            h->op[i] = rd16(a[1] + 2 * (uint32_t)i);
        return noErr;
    case 0x0512: /* GetGraphicsMode(*mode, *opColor) */
        if (a[0])
            wr32(a[0], (uint32_t)h->mode);
        if (a[1])
            put_rgb(a[1], h->op);
        return noErr;
    case 0x0513: /* GSetVolume */
        h->volume = (int16_t)a[0];
        return noErr;
    case 0x0514: /* SetSoundBalance */
        h->balance = (int16_t)a[0];
        return noErr;
    case 0x0515: /* GetSoundBalance */
    case 0x0556: /* GetEffectiveSoundBalance */
        if (a[0])
            wr16(a[0], (uint16_t)h->balance);
        return noErr;
    case 0x0551: /* GetEffectiveVolume */
        if (a[0])
            wr16(a[0], (uint16_t)h->volume);
        return noErr;
    case 0x0516: /* GetNextBoundsChange(*when): never */
        if (a[0])
            wr32(a[0], (uint32_t)-1);
        return noErr;
    case 0x0517: /* GetSrcRgn(rgn, atMediaTime): the track's box */
    case 0x051F: { /* GetDrawingRgn(*partialRgn): all of it (none given) */
        if (k->what == 0x051F) {
            if (a[0])
                wr32(a[0], 0);
            return noErr;
        }
        if (a[0] && t) {
            gaddr r = tb_NewPtr(8, false, true);
            wr16(r + 4, (uint16_t)(t->height >> 16));
            wr16(r + 6, (uint16_t)(t->width >> 16));
            tb_RectRgn(a[0], r);
            tb_DisposePtr(r);
        }
        return noErr;
    }
    case 0x051A: /* HasCharacteristic(characteristic, *hasIt) */
        if (a[1])
            wr8(a[1], qt_media_has_characteristic(h->type, a[0]));
        return noErr;
    case 0x051B: /* GetOffscreenBufferSize(bounds, depth, ctab): the bounds will do */
        return noErr;
    case 0x051C: /* SetHints */
        h->hints = (int32_t)a[0];
        return noErr;
    case 0x051D: { /* GetName(name, requestedLanguage, *actualLanguage) */
        if (a[0]) {
            const char *n = md ? md->handler_name : "";
            wr8(a[0], (uint8_t)strlen(n));
            gput(a[0] + 1, n, (uint32_t)strlen(n));
        }
        if (a[2])
            wr32(a[2], a[1]);
        return noErr;
    }
    case 0x051E: /* ForceUpdate */
    case 0x0521: /* InvalidateRegion */
        if (mv)
            qt_movie_redraw(mv);
        return noErr;
    case 0x0520: /* GSetActiveSegment(start, duration) */
        h->active_start = (int32_t)a[0];
        h->active_duration = (int32_t)a[1];
        return noErr;
    case 0x0522: { /* GetNextStepTime(flags, timeIn, *timeOut, rate) */
        if (!md)
            return invalidMedia;
        gaddr out = tb_NewPtr(8, false, true);
        tb_GetMediaNextInterestingTime((int32_t)h->media, 1, (int32_t)a[1], (int32_t)a[3] ? (int32_t)a[3] : 0x10000,
                                       out, out + 4);
        if (a[2])
            wr32(a[2], rd32(out));
        tb_DisposePtr(out);
        return noErr;
    }
    case 0x0526: { /* GetSampleDataPointer(sampleNum, *ptr, *size, *descIndex) */
        if (!md)
            return invalidMedia;
        uint32_t n, d;
        uint8_t *b = sample_bytes(mv, md, a[0], &n, &d);
        if (!b)
            return invalidSampleNum;
        int at = -1;
        for (int i = 0; i < MAX_PTRS && at < 0; i++)
            if (!h->ptrs[i].ptr)
                at = i;
        if (at < 0) {
            free(b);
            return memFullErr;
        }
        gaddr p = tb_NewPtr((int32_t)(n ? n : 1), false, false);
        gput(p, b, n);
        free(b);
        h->ptrs[at].sample = (int32_t)a[0];
        h->ptrs[at].ptr = p;
        if (a[1])
            wr32(a[1], p);
        if (a[2])
            wr32(a[2], n);
        if (a[3])
            wr32(a[3], d);
        return noErr;
    }
    case 0x0527: /* ReleaseSampleDataPointer(sampleNum) */
        for (int i = 0; i < MAX_PTRS; i++)
            if (h->ptrs[i].ptr && h->ptrs[i].sample == (int32_t)a[0]) {
                tb_DisposePtr(h->ptrs[i].ptr);
                h->ptrs[i].ptr = 0;
                return noErr;
            }
        return paramErr;
    case 0x0529: /* SetTrackInputMapReference */
        return noErr;
    case 0x052B: /* SetVideoParam / GetVideoParam: no adjustable picture here */
    case 0x052C:
        return featureUnsupported;
    case 0x052D: /* Compare(*isOK, srcMedia, srcComponent): the same kind of media fits */
        if (a[0]) {
            qtm_media *src = qt_media_model(a[1], NULL, NULL);
            wr8(a[0], src && md && src->type == md->type && src->scale == md->scale);
        }
        return noErr;
    case 0x052E: /* GetClock(*clock): ours keep the movie's time */
        if (a[0])
            wr32(a[0], 0);
        return noErr;
    case 0x052F: /* SetSoundOutputComponent */
        h->sound_output = a[0];
        return noErr;
    case 0x0530:
        if (a[0])
            wr32(a[0], h->sound_output);
        return noErr;
    case 0x0531: /* SetSoundLocalizationData */
        return noErr;
    case 0x053C: /* GetInvalidRegion(rgn): nothing waits to be drawn */
        if (a[0])
            tb_SetEmptyRgn(a[0]);
        return noErr;
    case 0x053E: /* SampleDescriptionB2N / N2B: this machine is big-endian */
    case 0x053F:
        return noErr;
    case 0x0523: /* SetNonPrimarySourceData, ChangedNonPrimarySource, Queue/Flush: (no effect inputs) */
    case 0x0524:
    case 0x0540:
    case 0x0541:
        return featureUnsupported;
    case 0x0543: /* GetURLLink(where, *url): none */
        if (a[1])
            wr32(a[1], 0);
        return noErr;
    case 0x0545: /* MakeMediaTimeTable(offsets, start, end, inc, first, last, *skew) */
        return tb_MakeMediaTimeTable((int32_t)h->media, a[0], (int32_t)a[1], (int32_t)a[2], (int32_t)a[3],
                                     (int16_t)a[4], (int16_t)a[5], a[6]);
    case 0x0546: /* HitTestForTargetRefCon(flags, loc, *refcon): a sprite's ID for sprites */
        if (a[2])
            wr32(a[2], 0);
        if (h->type == fourcc("sprt")) {
            qtc_call c2 = *k;
            c2.what = 0x010C; /* HitTestAllSprites */
            return sprite_call(h, &c2);
        }
        return noErr;
    case 0x0547: /* HitTestTargetRefCon(refcon, flags, loc, *wasHit) */
        if (h->type == fourcc("sprt")) {
            qtc_call c2 = *k;
            c2.what = 0x010D; /* HitTestOneSprite */
            return sprite_call(h, &c2);
        }
        if (a[3])
            wr8(a[3], 0);
        return noErr;
    case 0x0548: /* GetActionsForQTEvent(event, refcon, *container, *atom) */
        if (a[2])
            wr32(a[2], 0);
        if (a[3])
            wr32(a[3], 0);
        return noErr;
    case 0x0549: /* DisposeTargetRefCon */
        return noErr;
    case 0x054A: /* TargetRefConsEqual(a, b, *equal) */
        if (a[2])
            wr8(a[2], a[0] == a[1]);
        return noErr;
    case 0x054B: /* SetActionsCallback(proc, refcon) */
        h->actions_proc = a[0];
        h->actions_refcon = a[1];
        return noErr;
    case 0x054C: /* PrePrerollBegin(time, rate, completeProc, refcon): ready at once */
        if (a[2]) {
            cpu68k *c = g_cpu;
            cpu_push32(c, k->ci);
            cpu_push16(c, noErr);
            cpu_push32(c, a[3]);
            guest_call(a[2]);
        }
        return noErr;
    case 0x054D: /* PrePrerollCancel */
        return noErr;
    case 0x0550: /* CurrentMediaQueuedData(*ms) */
        if (a[0])
            wr32(a[0], 0);
        return noErr;
    case 0x0552: /* ResolveTargetRefCon(container, atom, *refcon) */
        if (a[2])
            wr32(a[2], 0);
        return noErr;
    case 0x0553: /* GetSoundLevelMeteringEnabled */
        if (a[0])
            wr8(a[0], h->metering);
        return noErr;
    case 0x0554:
        h->metering = (a[0] & 0xFF) != 0;
        return noErr;
    case 0x0555: /* GetSoundLevelMeterInfo: silence */
        if (a[0])
            for (uint32_t i = 0; i < 16; i++)
                wr8(a[0] + i, 0);
        return noErr;
    case 0x0557: /* SetScreenLock */
        h->screen_lock = (a[0] & 0xFF) != 0;
        return noErr;
    case 0x0558: /* SetDoMCActionCallback(proc, refcon) */
        h->mcaction_proc = a[0];
        h->mcaction_refcon = a[1];
        return noErr;
    case 0x0559: /* GetErrorString(error, str): none to give */
        if (a[1])
            wr8(a[1], 0);
        return paramErr;
    case 0x055A: /* the equalizer: none on this output */
    case 0x055B:
    case 0x055C:
        return featureUnsupported;
    case 0x055E: /* SetSoundBassAndTreble */
        h->bass = (int16_t)a[0];
        h->treble = (int16_t)a[1];
        return noErr;
    case 0x055F:
        if (a[0])
            wr16(a[0], (uint16_t)h->bass);
        if (a[1])
            wr16(a[1], (uint16_t)h->treble);
        return noErr;
    case 0x0561: /* MCIsPlayerEvent(event, *handled): nothing of ours to click in */
        if (a[1])
            wr8(a[1], 0);
        return noErr;
    case 0x0562: /* GetMediaLoadState */
        if (a[0])
            wr32(a[0], 100000);
        return noErr;
    case 0x0563: /* VideoOutputChanged */
        h->vout = a[0];
        return noErr;
    case 0x0564: /* EmptySampleCache */
        return noErr;
    case 0x0565: /* GetPublicInfo / SetPublicInfo: nothing public */
    case 0x0566:
    case 0x0567: /* Get/SetUserPreferredCodecs */
    case 0x0568:
        return paramErr;
    }
    return qtcBadSelector;
}

static int32_t mh_call(qtc_call *k)
{
    mhlr *h = k->self;
    switch (k->what) {
    case kQTCOpen:
        h->type = k->cls->subtype;
        h->active = true;
        h->rate = 0x10000;
        h->volume = 0x100;
        h->text_shown = -1;
        h->active_start = -1;
        return noErr;
    case kQTCClose:
        free(h->pending);
        free(h->raw);
        if (h->raw_desc)
            tb_DisposeHandle(h->raw_desc);
        if (h->clip)
            tb_DisposeRgn(h->clip);
        for (int i = 0; i < 8; i++)
            free(h->codec_params[i].data);
        for (int i = 0; i < MAX_PTRS; i++)
            if (h->ptrs[i].ptr)
                tb_DisposePtr(h->ptrs[i].ptr);
        return noErr;
    case kQTCVersion:
        return 0x00040000;
    }
    if (!k->call)
        return qtcBadSelector;
    const char *fam = k->call->family;
    if (!strcmp(fam, "Media"))
        return base_call(h, k);
    if (!strcmp(fam, "TextMedia"))
        return text_call(h, k);
    if (!strcmp(fam, "SpriteMedia"))
        return sprite_call(h, k);
    if (!strcmp(fam, "VideoMedia"))
        return video_call(h, k);
    if (!strcmp(fam, "TC"))
        return tc_call(h, k);
    if (!strcmp(fam, "MusicMedia"))
        return music_call(h, k);
    if (!strcmp(fam, "MovieMedia"))
        return movie_media_call(h, k);
    return qtcBadSelector;
}

#define MH(sub, name, fam) { 0x6D686C72, sub, 0x6170706C, 0, name, NULL, 0x00040000, { "Media", fam }, sizeof(mhlr), mh_call, 0 }
static qtc_class handlers[] = {
    MH(0x76696465, "Apple Video Media Handler", "VideoMedia"),   /* 'vide' */
    MH(0x736F756E, "Apple Sound Media Handler", NULL),           /* 'soun' */
    MH(0x74657874, "Apple Text Media Handler", "TextMedia"),     /* 'text' */
    MH(0x73707274, "Apple Sprite Media Handler", "SpriteMedia"), /* 'sprt' */
    MH(0x6D757369, "Apple Music Media Handler", "MusicMedia"),   /* 'musi' */
    MH(0x746D6364, "Apple Timecode Media Handler", "TC"),        /* 'tmcd' */
    MH(0x6D6F6F76, "Apple Movie Media Handler", "MovieMedia"),   /* 'moov' */
    MH(0x676E7263, "Apple Base Media Handler", NULL),            /* 'gnrc' */
};

void qt_mhlr_register(void)
{
    for (size_t i = 0; i < sizeof handlers / sizeof handlers[0]; i++)
        qtc_register(&handlers[i]);
}

/* The media handlers' part of MoviesTask: text procedures called as their
 * samples come up. */
static void task_one(void *self, uint32_t ci)
{
    (void)ci;
    mhlr *h = self;
    if (h->text_proc)
        text_proc_check(h);
}

void qt_mhlr_task(void) { qtc_each(&handlers[2], task_one); }
