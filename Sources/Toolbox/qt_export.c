/*
 * Movie exporters of our own ('spit' components): a movie, or one track or
 * a stretch of it, written as another kind of file, as QuickTime 3 did for
 * ConvertMovieToFile, PutMovieIntoTypedHandle and the programs' own
 * MovieExport calls.
 *
 *   'MooV'  a self-contained (flattened) movie
 *   'AIFF', 'WAVE', 'ULAW' (Sun audio), 'sfil' (a System 7 sound file)
 *           the movie's enabled sound tracks mixed, at the first one's rate
 *           or the settings' (rate, sample size, channels)
 *   'PICT'  the frame at the start time
 *   'TEXT'  the text of the first text track, a sample a line
 *   'Midi'  the first music track as a Standard MIDI File
 */
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "filemgr_internal.h"
#include "qt_atoms.h"
#include "qt_codec.h"
#include "qt_comp.h"
#include "qt_dref.h"
#include "qt_errors.h"
#include "qt_import.h"
#include "qt_movie_internal.h"
#include "qt_music.h"
#include "tb_internal.h"

enum {
    canExportHandles = 1 << 3, canExportFiles = 1 << 4, hasUserInterface = 1 << 5,
    canExportValidateMovie = 1 << 16, needsResourceFork = 1 << 17,
};

typedef struct {
    double rate;    /* 0: the source's */
    int bits;       /* 0: 16 */
    int channels;   /* 0: the source's */
    gaddr progress_proc;
    int32_t progress_refcon;
} exporter;

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v; }
static void put32b(uint8_t *p, uint32_t v) { put16(p, v >> 16), put16(p + 2, v); }
static void put16le(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void put32le(uint8_t *p, uint32_t v) { put16le(p, v), put16le(p + 2, v >> 16); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

/* A growing buffer of bytes. */
typedef struct {
    uint8_t *b;
    size_t n, cap;
} buf;

static void put(buf *o, const void *p, size_t n)
{
    if (o->n + n > o->cap) {
        size_t cap = o->cap ? o->cap : 4096;
        while (cap < o->n + n)
            cap *= 2;
        uint8_t *nb = realloc(o->b, cap);
        if (!nb)
            return;
        o->b = nb;
        o->cap = cap;
    }
    memcpy(o->b + o->n, p, n);
    o->n += n;
}

/* ---- Where the bytes go ---- */

/* A new file (any old one replaced) of the type, holding the bytes. */
static int16_t write_spec(gaddr spec, uint32_t type, uint32_t creator, const uint8_t *b, size_t n)
{
    tb_FSpDelete(spec);
    int16_t err = tb_FSpCreate(spec, (int32_t)creator, (int32_t)type, 0);
    if (err)
        return err;
    char path[PATH_MAX];
    if (!fm_host_path((int16_t)rd16(spec), (int32_t)rd32(spec + 2), spec + 6, true, path, sizeof path))
        return fnfErr;
    gaddr alias = qt_dref_for_path(path);
    uint32_t an;
    uint8_t *ab = qt_dref_bytes(alias, &an);
    tb_DisposeHandle(alias);
    qt_src src;
    bool ok = qt_src_open(fourcc("alis"), 0, ab, an, NULL, true, &src);
    free(ab);
    if (!ok)
        return dataNotOpenForWrite;
    ok = qt_src_set_size(&src, 0) && qt_src_write(&src, 0, b, (uint32_t)n) && qt_src_flush(&src);
    qt_src_close(&src);
    return ok ? noErr : ioErr;
}

static int16_t write_handle(gaddr h, const uint8_t *b, size_t n)
{
    if (!h)
        return paramErr;
    tb_SetHandleSize(h, (int32_t)n);
    if ((size_t)tb_GetHandleSize(h) != n)
        return memFullErr;
    gput(rd32(h), b, (uint32_t)n);
    return noErr;
}

/* A data reference naming a file or a handle. */
static int16_t write_dataref(gaddr dataRef, uint32_t type, uint32_t ftype, uint32_t creator, const uint8_t *b,
                             size_t n)
{
    uint32_t rn;
    uint8_t *ref = qt_dref_bytes(dataRef, &rn);
    char path[PATH_MAX];
    int16_t err;
    if (qt_dref_path(type, ref, rn, NULL, path, sizeof path)) {
        gaddr spec = tb_NewPtr(FSSpec_SIZEOF, false, true);
        fm_spec_for_path(path, spec);
        err = write_spec(spec, ftype, creator, b, n);
        tb_DisposePtr(spec);
    } else if (type == fourcc("hndl") && rn >= 4) {
        err = write_handle(be32(ref), b, n);
    } else {
        err = invalidDataRef;
    }
    free(ref);
    return err;
}

/* ---- What's exported ---- */

/* The tracks of a type, enabled, in the movie (or the one asked for). */
static int tracks_of(qtm_movie *mv, gaddr only, uint32_t type, qtm_track **out, int max)
{
    int n = 0;
    for (int i = 0; i < mv->ntracks && n < max; i++) {
        qtm_track *t = mv->tracks[i];
        if ((only && t->token != only) || !t->media || t->media->type != type)
            continue;
        if (!only && !(t->flags & 1))
            continue;
        out[n++] = t;
    }
    return n;
}

/* A sound media's samples decoded, whole: 16-bit frames of its channels. */
static int16_t *decode_media(qtm_movie *mv, qtm_media *md, int *channels, double *rate, uint64_t *frames)
{
    if (!md->ndescs || md->desc_len[0] < 36)
        return NULL;
    const uint8_t *d = md->descs[0];
    qt_sound_format f = { .format = be32(d + 4), .channels = be16(d + 24), .bits = be16(d + 26),
                          .rate = be32(d + 32) / 65536.0 };
    if (f.rate < 1000)
        f.rate = md->scale;
    if ((f.format == fourcc("raw ") || f.format == fourcc("twos") || f.format == fourcc("sowt")) && f.bits != 16)
        f.bits = 8;
    if (!qt_adec_supported(&f))
        return NULL;
    uint64_t total = 0;
    for (int r = 0; r < md->nruns; r++)
        total += md->runs[r].count;
    int16_t *pcm = calloc((size_t)(total ? total : 1) * (size_t)f.channels, sizeof *pcm);
    if (!pcm)
        return NULL;
    uint64_t at = 0;
    for (int r = 0; r < md->nruns; r++) {
        const qtm_run *run = &md->runs[r];
        int16_t di = qtm_desc_dref(md, run->desc);
        if (di < 1 || di > md->ndrefs) {
            at += run->count;
            continue;
        }
        const qtm_dref *dr = &md->drefs[di - 1];
        qt_src src;
        size_t bytes = qt_adec_bytes(&f, run->count);
        uint8_t *data = malloc(bytes ? bytes : 1);
        if (data && qt_src_open(dr->type, dr->flags, dr->data, dr->len, qt_movie_path(mv), false, &src)) {
            if (qt_src_read(&src, run->offset, data, (uint32_t)bytes)) {
                int16_t *out;
                uint32_t got = qt_adec_decode(&f, data, bytes, &out);
                if (got > run->count)
                    got = run->count;
                if (out)
                    memcpy(pcm + at * (uint64_t)f.channels, out, (size_t)got * (size_t)f.channels * sizeof *out);
                free(out);
            }
            qt_src_close(&src);
        }
        free(data);
        at += run->count;
    }
    *channels = f.channels;
    *rate = f.rate;
    *frames = total;
    return pcm;
}

/* A track's media time at a movie time, to fractions (-1: none): its
 * edits, or the media from the start without any. */
static double media_at(const qtm_track *t, double tm, int32_t movie_scale)
{
    int32_t mscale = t->media->scale;
    if (!t->nedits)
        return tm * mscale / movie_scale;
    double at = 0;
    for (int i = 0; i < t->nedits; i++) {
        const qtm_edit *e = &t->edits[i];
        if (tm < at + (double)e->duration) {
            if (e->media_time < 0)
                return -1;
            return (double)e->media_time + (tm - at) * mscale / movie_scale * (e->rate / 65536.0);
        }
        at += (double)e->duration;
    }
    return -1;
}

/* The sound of the tracks from start for duration (movie time) mixed at
 * rate into frames of channels. */
static int16_t *mix_sound(qtm_movie *mv, qtm_track **tracks, int nt, int32_t start, int32_t duration, double rate,
                          int channels, uint32_t *nframes)
{
    uint64_t n = (uint64_t)llround((double)duration * rate / mv->scale);
    int32_t *acc = calloc((size_t)(n ? n : 1) * (size_t)channels, sizeof *acc);
    if (!acc)
        return NULL;
    for (int k = 0; k < nt; k++) {
        int sch;
        double srate;
        uint64_t sframes;
        int16_t *src = decode_media(mv, tracks[k]->media, &sch, &srate, &sframes);
        if (!src)
            continue;
        int32_t mscale = tracks[k]->media->scale;
        for (uint64_t i = 0; i < n; i++) {
            double exact = start + (double)i * mv->scale / rate;
            double m = media_at(tracks[k], exact, mv->scale);
            if (m < 0)
                continue;
            double pos = m * srate / mscale + 1e-6;
            uint64_t f = (uint64_t)pos;
            if (f >= sframes)
                continue;
            for (int c = 0; c < channels; c++) {
                int sc = sch == 1 ? 0 : c < sch ? c : sch - 1;
                int32_t v = src[f * (uint64_t)sch + (uint64_t)sc];
                if (channels == 1 && sch == 2)
                    v = (src[f * 2] + src[f * 2 + 1]) / 2;
                acc[i * (uint64_t)channels + (uint64_t)c] += v * tracks[k]->volume / 0x100;
            }
        }
        free(src);
    }
    int16_t *out = malloc((size_t)(n ? n : 1) * (size_t)channels * sizeof *out);
    for (uint64_t i = 0; out && i < n * (uint64_t)channels; i++)
        out[i] = (int16_t)(acc[i] > 32767 ? 32767 : acc[i] < -32768 ? -32768 : acc[i]);
    free(acc);
    *nframes = (uint32_t)n;
    return out;
}

static void extended(uint8_t *p, double v)
{
    memset(p, 0, 10);
    if (v <= 0)
        return;
    int e;
    double m = frexp(v, &e); /* v = m * 2^e, 0.5 <= m < 1 */
    uint64_t mant = (uint64_t)ldexp(m, 64);
    int exp = e - 1 + 16383;
    p[0] = (uint8_t)(exp >> 8), p[1] = (uint8_t)exp;
    for (int i = 0; i < 8; i++)
        p[2 + i] = (uint8_t)(mant >> (56 - 8 * i));
}

static uint8_t ulaw_of(int16_t s)
{
    int sign = s < 0 ? 0x80 : 0, v = s < 0 ? -(int)s : s;
    if (v > 32635)
        v = 32635;
    v += 0x84;
    int exp = 7;
    for (int mask = 0x4000; !(v & mask) && exp > 0; mask >>= 1)
        exp--;
    int mant = (v >> (exp + 3)) & 15;
    return (uint8_t)~(sign | exp << 4 | mant);
}

/* The sound file of a kind: AIFF, WAVE, Sun audio, or a 'snd ' resource's bytes. */
static void sound_file(buf *o, uint32_t kind, const int16_t *pcm, uint32_t frames, int channels, int bits, double rate)
{
    size_t samples = (size_t)frames * (size_t)channels;
    uint8_t h[64];
    if (kind == fourcc("ULAW")) {
        memcpy(h, ".snd", 4);
        put32b(h + 4, 24);
        put32b(h + 8, (uint32_t)samples);
        put32b(h + 12, 1);
        put32b(h + 16, (uint32_t)lround(rate));
        put32b(h + 20, (uint32_t)channels);
        put(o, h, 24);
        for (size_t i = 0; i < samples; i++) {
            uint8_t u = ulaw_of(pcm[i]);
            put(o, &u, 1);
        }
        return;
    }
    size_t bytes = samples * (size_t)(bits / 8);
    if (kind == fourcc("WAVE")) {
        memcpy(h, "RIFF", 4);
        put32le(h + 4, (uint32_t)(36 + bytes + (bytes & 1)));
        memcpy(h + 8, "WAVEfmt ", 8);
        put32le(h + 16, 16);
        put16le(h + 20, 1);
        put16le(h + 22, (uint32_t)channels);
        put32le(h + 24, (uint32_t)lround(rate));
        put32le(h + 28, (uint32_t)lround(rate) * (uint32_t)channels * (uint32_t)(bits / 8));
        put16le(h + 32, (uint32_t)channels * (uint32_t)(bits / 8));
        put16le(h + 34, (uint32_t)bits);
        memcpy(h + 36, "data", 4);
        put32le(h + 40, (uint32_t)bytes);
        put(o, h, 44);
        for (size_t i = 0; i < samples; i++) {
            uint8_t s[2];
            if (bits == 8) {
                s[0] = (uint8_t)((pcm[i] >> 8) + 128);
                put(o, s, 1);
            } else {
                put16le(s, (uint16_t)pcm[i]);
                put(o, s, 2);
            }
        }
        if (bytes & 1)
            put(o, "", 1);
        return;
    }
    if (kind == fourcc("snd ")) { /* format 1: sampled synth, a bufferCmd, an extended header */
        put16(h, 1);
        put16(h + 2, 1);
        put16(h + 4, 5);           /* sampledSynth */
        put32b(h + 6, channels == 2 ? 0xC0 : 0x80); /* initStereo or initMono */
        put16(h + 10, 1);
        put16(h + 12, 0x8051);     /* bufferCmd, with its data offset */
        put16(h + 14, 0);
        put32b(h + 16, 20);
        put(o, h, 20);
        uint8_t x[64] = { 0 };
        put32b(x + 4, (uint32_t)channels);
        put32b(x + 8, rate < 65536 ? (uint32_t)(rate * 65536.0) : 0);
        x[20] = 0xFF;              /* extSH */
        x[21] = 60;
        put32b(x + 22, frames);
        extended(x + 26, rate);
        put16(x + 48, (uint32_t)bits);
        put(o, x, 64);
        for (size_t i = 0; i < samples; i++) {
            uint8_t s[2];
            if (bits == 8) {
                s[0] = (uint8_t)((pcm[i] >> 8) + 128); /* extended headers' 8-bit are offset binary */
                put(o, s, 1);
            } else {
                put16(s, (uint16_t)pcm[i]);
                put(o, s, 2);
            }
        }
        return;
    }
    /* AIFF */
    memcpy(h, "FORM", 4);
    put32b(h + 4, (uint32_t)(4 + 26 + 16 + bytes + (bytes & 1)));
    memcpy(h + 8, "AIFFCOMM", 8);
    put32b(h + 16, 18);
    put16(h + 20, (uint32_t)channels);
    put32b(h + 22, frames);
    put16(h + 26, (uint32_t)bits);
    extended(h + 28, rate);
    memcpy(h + 38, "SSND", 4);
    put32b(h + 42, (uint32_t)(8 + bytes));
    put32b(h + 46, 0);
    put32b(h + 50, 0);
    put(o, h, 54);
    for (size_t i = 0; i < samples; i++) {
        uint8_t s[2];
        if (bits == 8) {
            s[0] = (uint8_t)(pcm[i] >> 8);
            put(o, s, 1);
        } else {
            put16(s, (uint16_t)pcm[i]);
            put(o, s, 2);
        }
    }
    if (bytes & 1)
        put(o, "", 1);
}

static int16_t export_sound(exporter *ex, uint32_t kind, qtm_movie *mv, gaddr only, int32_t start, int32_t dur,
                            buf *o)
{
    qtm_track *tracks[32];
    int nt = tracks_of(mv, only, fourcc("soun"), tracks, 32);
    if (!nt)
        return invalidTrack; /* nothing to hear */
    int sch = 1;
    double srate = tracks[0]->media->scale;
    const qtm_media *md = tracks[0]->media;
    if (md->ndescs && md->desc_len[0] >= 36) {
        sch = be16(md->descs[0] + 24) == 2 ? 2 : 1;
        double r = be32(md->descs[0] + 32) / 65536.0;
        if (r >= 1000)
            srate = r;
    }
    double rate = ex->rate > 0 ? ex->rate : kind == fourcc("ULAW") && !ex->rate ? 8000 : srate;
    int channels = ex->channels ? ex->channels : sch;
    int bits = kind == fourcc("ULAW") ? 8 : ex->bits == 8 ? 8 : 16;
    uint32_t frames;
    int16_t *pcm = mix_sound(mv, tracks, nt, start, dur, rate, channels, &frames);
    if (!pcm)
        return memFullErr;
    sound_file(o, kind, pcm, frames, channels, bits, rate);
    free(pcm);
    return noErr;
}

/* A text track's samples (2-byte length, then the text), a line each. */
static int16_t export_text(qtm_movie *mv, gaddr only, int32_t start, int32_t dur, buf *o)
{
    qtm_track *t;
    if (!tracks_of(mv, only, fourcc("text"), &t, 1))
        return invalidTrack;
    qtm_media *md = t->media;
    uint32_t count = qtm_sample_count(md), last = 0;
    for (int64_t tm = start; tm < (int64_t)start + dur;) {
        int32_t r;
        int64_t m = qtm_track_to_media(t, tm, mv->scale, &r);
        int64_t s0, sd;
        uint32_t n = m >= 0 ? qtm_sample_at(md, m, &s0, &sd) : 0;
        if (n && n != last && n <= count) {
            uint64_t off, bytes;
            uint32_t desc;
            bool sync;
            int run;
            if (qtm_sample_info(md, n, &off, &bytes, &desc, &sync, &run) && bytes >= 2) {
                int16_t di = qtm_desc_dref(md, desc);
                if (di >= 1 && di <= md->ndrefs) {
                    const qtm_dref *dr = &md->drefs[di - 1];
                    qt_src src;
                    uint8_t *b = malloc((size_t)bytes);
                    if (b && qt_src_open(dr->type, dr->flags, dr->data, dr->len, qt_movie_path(mv), false, &src)) {
                        if (qt_src_read(&src, off, b, (uint32_t)bytes)) {
                            uint32_t len = be16(b);
                            if (len > bytes - 2)
                                len = (uint32_t)bytes - 2;
                            if (len) {
                                put(o, b + 2, len);
                                put(o, "\r", 1);
                            }
                        }
                        qt_src_close(&src);
                    }
                    free(b);
                }
            }
            last = n;
        }
        /* on to the next sample's time */
        int64_t next = n ? s0 + sd : m + 1;
        int64_t step = md->scale ? (next - (m >= 0 ? m : 0)) * mv->scale / md->scale : 1;
        tm += step > 0 ? step : 1;
        if (!n && m < 0)
            tm += mv->scale / 10 + 1;
    }
    return noErr;
}

/* A music track as a Standard MIDI File: one track, 600 ticks a second
 * (a quarter note of a second at 600 ticks a quarter). */
static void vlq(buf *o, uint32_t v)
{
    uint8_t b[5];
    int n = 0;
    b[4 - n++] = (uint8_t)(v & 0x7F);
    while ((v >>= 7) && n < 5)
        b[4 - n++] = (uint8_t)(0x80 | (v & 0x7F));
    put(o, b + 5 - n, (size_t)n);
}

static int16_t export_midi(qtm_movie *mv, gaddr only, int32_t start, int32_t dur, buf *o)
{
    qtm_track *t;
    if (!tracks_of(mv, only, fourcc("musi"), &t, 1))
        return invalidTrack;
    qtm_media *md = t->media;
    if (!md->ndescs || md->desc_len[0] < 20)
        return invalidMedia;
    uint32_t count = qtm_sample_count(md);
    qt_music_sample *samples = calloc(count ? count : 1, sizeof *samples);
    uint8_t **datas = calloc(count ? count : 1, sizeof *datas);
    size_t ns = 0;
    for (uint32_t n = 1; n <= count; n++) {
        uint64_t off, bytes;
        uint32_t desc;
        bool sync;
        int run;
        int64_t s0, sd;
        if (!qtm_sample_info(md, n, &off, &bytes, &desc, &sync, &run) || !qtm_sample_time(md, n, &s0, &sd))
            continue;
        int16_t di = qtm_desc_dref(md, desc);
        if (di < 1 || di > md->ndrefs)
            continue;
        const qtm_dref *dr = &md->drefs[di - 1];
        qt_src src;
        uint8_t *b = malloc((size_t)(bytes ? bytes : 1));
        if (b && qt_src_open(dr->type, dr->flags, dr->data, dr->len, qt_movie_path(mv), false, &src)) {
            if (qt_src_read(&src, off, b, (uint32_t)bytes)) {
                datas[ns] = b;
                samples[ns++] = (qt_music_sample){ b, (size_t)bytes, s0 };
                b = NULL;
            }
            qt_src_close(&src);
        }
        free(b);
    }
    size_t nev;
    qt_midi_event *ev = qt_music_events(md->descs[0] + 20, md->desc_len[0] - 20, samples, ns, &nev);
    for (size_t i = 0; i < ns; i++)
        free(datas[i]);
    free(datas);
    free(samples);
    /* the events between start and start + duration, in movie time */
    int32_t r;
    int64_t m0 = qtm_track_to_media(t, start, mv->scale, &r);
    if (m0 < 0)
        m0 = 0;
    int64_t m1 = m0 + (int64_t)dur * md->scale / (mv->scale ? mv->scale : 600);
    buf trk = { 0 };
    uint8_t tempo[] = { 0, 0xFF, 0x51, 3, 0x0F, 0x42, 0x40 }; /* a quarter a second */
    put(&trk, tempo, sizeof tempo);
    int64_t last = 0;
    for (size_t i = 0; i < nev; i++) {
        if (ev[i].time < m0 || ev[i].time > m1)
            continue;
        int64_t tick = (ev[i].time - m0) * 600 / (md->scale ? md->scale : 600);
        vlq(&trk, (uint32_t)(tick - last));
        last = tick;
        uint8_t m[3] = { ev[i].status, ev[i].data1, ev[i].data2 };
        put(&trk, m, (ev[i].status & 0xF0) == 0xC0 || (ev[i].status & 0xF0) == 0xD0 ? 2 : 3);
    }
    free(ev);
    uint8_t end[] = { 0, 0xFF, 0x2F, 0 };
    put(&trk, end, sizeof end);
    uint8_t h[22];
    memcpy(h, "MThd", 4);
    put32b(h + 4, 6);
    put16(h + 8, 0);
    put16(h + 10, 1);
    put16(h + 12, 600);
    memcpy(h + 14, "MTrk", 4);
    put32b(h + 18, (uint32_t)trk.n);
    put(o, h, 22);
    put(o, trk.b, trk.n);
    free(trk.b);
    return noErr;
}

/* The movie, or the part of it asked for, in a movie of its own. */
static gaddr part_of(gaddr movie, gaddr only, int32_t start, int32_t dur, bool *copied)
{
    *copied = false;
    qtm_movie *mv = qt_movie_model(movie);
    if (!only && start <= 0 && dur >= tb_GetMovieDuration((int32_t)movie))
        return movie;
    gaddr sel = tb_NewPtr(8, false, true);
    tb_GetMovieSelection((int32_t)movie, sel, sel + 4);
    tb_SetMovieSelection((int32_t)movie, start, dur);
    gaddr part = (gaddr)tb_CopyMovieSelection((int32_t)movie);
    tb_SetMovieSelection((int32_t)movie, (int32_t)rd32(sel), (int32_t)rd32(sel + 4));
    tb_DisposePtr(sel);
    if (!part)
        return 0;
    *copied = true;
    if (only) {
        qtm_track *t = qt_track_model(only, NULL);
        qtm_movie *pm = qt_movie_model(part);
        for (int i = pm->ntracks - 1; t && i >= 0; i--)
            if (pm->tracks[i]->id != t->id)
                qtm_remove_track(pm, pm->tracks[i]);
        qt_movie_touch(pm);
    }
    (void)mv;
    return part;
}

/* A self-contained movie file's bytes: flattened into a file of the host's
 * temporary folder, and read back. */
static int16_t export_movie(gaddr movie, gaddr only, int32_t start, int32_t dur, buf *o)
{
    bool copied;
    gaddr m = part_of(movie, only, start, dur, &copied);
    if (!m)
        return tb_GetMoviesError() ? tb_GetMoviesError() : memFullErr;
    /* a file in the Temporary Items folder, by the File Manager's lights */
    gaddr where = tb_NewPtr(8, false, true), spec = tb_NewPtr(FSSpec_SIZEOF, false, true);
    char path[PATH_MAX];
    static int serial;
    char name[32];
    snprintf(name, sizeof name, "%cExport %d", 18, ++serial);
    name[0] = (char)(strlen(name) - 1);
    gaddr pname = tb_NewPtr(32, false, true);
    gput(pname, name, (uint32_t)strlen(name));
    bool ok = tb_FindFolder(-1, (int32_t)fourcc("temp"), 1, where, where + 2) == noErr;
    if (ok) {
        int16_t e = tb_FSMakeFSSpec((int16_t)rd16(where), (int32_t)rd32(where + 2), pname, spec);
        ok = (e == noErr || e == fnfErr) && (tb_FSpDelete(spec), tb_FSpCreate(spec, (int32_t)fourcc("TVOD"), (int32_t)fourcc("MooV"), 0) == noErr) &&
             fm_host_path((int16_t)rd16(spec), (int32_t)rd32(spec + 2), spec + 6, true, path, sizeof path);
    }
    tb_DisposePtr(where);
    tb_DisposePtr(pname);
    if (!ok) {
        tb_DisposePtr(spec);
        if (copied)
            tb_DisposeMovie(m);
        return ioErr;
    }
    gaddr ref = qt_dref_for_path(path);
    int32_t flat = tb_FlattenMovieDataToDataRef((int32_t)m, 1 /* flattenAddMovieToDataFork */, (int32_t)ref, (int32_t)fourcc("alis"),
                                                (int32_t)fourcc("TVOD"), 0, 0);
    int16_t err = flat ? noErr : tb_GetMoviesError();
    if (flat)
        tb_DisposeMovie((gaddr)flat);
    tb_DisposeHandle(ref);
    if (copied)
        tb_DisposeMovie(m);
    if (!err) {
        FILE *f = fopen(path, "rb");
        uint8_t chunk[65536];
        size_t got;
        while (f && (got = fread(chunk, 1, sizeof chunk, f)) > 0)
            put(o, chunk, got);
        if (f)
            fclose(f);
        else
            err = ioErr;
    }
    tb_FSpDelete(spec);
    tb_DisposePtr(spec);
    return err;
}

/* The picture at a time: a PICT file has 512 bytes before it. */
static int16_t export_pict(gaddr movie, int32_t at, bool file, buf *o)
{
    gaddr pic = (gaddr)tb_GetMoviePict((int32_t)movie, at);
    if (!pic)
        return tb_GetMoviesError() ? tb_GetMoviesError() : invalidTime;
    if (file) {
        uint8_t zero[512] = { 0 };
        put(o, zero, 512);
    }
    size_t n = (size_t)tb_GetHandleSize(pic);
    uint8_t *b = malloc(n ? n : 1);
    gget(b, rd32(pic), (uint32_t)n);
    put(o, b, n);
    free(b);
    tb_KillPicture(pic);
    return noErr;
}

static int16_t export_bytes(exporter *ex, uint32_t kind, gaddr movie, gaddr only, int32_t start, int32_t dur, bool file,
                            buf *o)
{
    qtm_movie *mv = qt_movie_model(movie);
    if (!mv)
        return invalidMovie;
    if (only && !qt_track_model(only, NULL))
        return invalidTrack;
    if (dur < 0 || start < 0)
        return paramErr;
    switch (kind) {
    case 0x4D6F6F56: return export_movie(movie, only, start, dur, o);                      /* MooV */
    case 0x50494354: return export_pict(movie, start, file, o);                            /* PICT */
    case 0x54455854: return export_text(mv, only, start, dur, o);                          /* TEXT */
    case 0x4D696469: return export_midi(mv, only, start, dur, o);                          /* Midi */
    case 0x7366696C: return export_sound(ex, fourcc("snd "), mv, only, start, dur, o);     /* sfil */
    default: return export_sound(ex, kind, mv, only, start, dur, o);                       /* AIFF WAVE ULAW */
    }
}

static uint32_t creator_of(uint32_t kind) { return kind == fourcc("MooV") ? fourcc("TVOD") : fourcc("TVOD"); }

/* A System 7 sound file: its 'snd ' resource. */
static int16_t write_sound_file(gaddr spec, const buf *o)
{
    tb_FSpDelete(spec);
    tb_FSpCreateResFile(spec, (int32_t)fourcc("movr"), (int32_t)fourcc("sfil"), 0);
    int16_t err = tb_ResError();
    if (err)
        return err;
    int16_t saved = tb_CurResFile(), rn = tb_FSpOpenResFile(spec, 3);
    if (rn < 0)
        return tb_ResError() ? tb_ResError() : fnfErr;
    tb_UseResFile(rn);
    gaddr h = tb_NewHandle((int32_t)o->n, false, false);
    if (h) {
        gput(rd32(h), o->b, (uint32_t)o->n);
        gaddr name = tb_NewPtr(8, false, true);
        tb_AddResource(h, (int32_t)fourcc("snd "), 128, name);
        err = tb_ResError();
        tb_DisposePtr(name);
    } else {
        err = memFullErr;
    }
    tb_CloseResFile(rn);
    tb_UseResFile(saved);
    return err;
}

/* Settings: a sound exporter's 'soun' atom with rate ('ssrt', unsigned
 * Fixed), sample size ('ssss') and channels ('sscc'). */
static gaddr settings_of(const exporter *ex, uint32_t kind)
{
    gaddr c = qt_atoms_new();
    if (!c || kind == fourcc("MooV") || kind == fourcc("PICT") || kind == fourcc("TEXT") || kind == fourcc("Midi"))
        return c;
    int16_t err;
    uint32_t parent = qt_atoms_insert(c, 0, fourcc("soun"), 1, 0, NULL, 0, &err);
    uint8_t v[4];
    put32b(v, (uint32_t)(ex->rate * 65536.0));
    qt_atoms_insert(c, parent, fourcc("ssrt"), 1, 0, v, 4, &err);
    put16(v, (uint32_t)(ex->bits ? ex->bits : 16));
    qt_atoms_insert(c, parent, fourcc("ssss"), 1, 0, v, 2, &err);
    put16(v, (uint32_t)ex->channels);
    qt_atoms_insert(c, parent, fourcc("sscc"), 1, 0, v, 2, &err);
    return c;
}

static void apply_settings(exporter *ex, gaddr c)
{
    if (!c || !rd32(c))
        return;
    size_t n = (size_t)tb_GetHandleSize(c);
    uint8_t *b = malloc(n ? n : 1);
    gget(b, rd32(c), (uint32_t)n);
    uint32_t root = qta_root(b, n);
    uint32_t s = qta_find(b, n, root, fourcc("soun"), 1);
    if (!s)
        s = qta_index(b, n, root, fourcc("soun"), 1, NULL);
    if (s) {
        uint32_t len;
        const uint8_t *d = qta_data(b, n, qta_find(b, n, s, fourcc("ssrt"), 1), &len);
        if (d && len >= 4)
            ex->rate = be32(d) / 65536.0;
        d = qta_data(b, n, qta_find(b, n, s, fourcc("ssss"), 1), &len);
        if (d && len >= 2)
            ex->bits = be16(d) == 8 ? 8 : 16;
        d = qta_data(b, n, qta_find(b, n, s, fourcc("sscc"), 1), &len);
        if (d && len >= 2)
            ex->channels = be16(d) == 1 || be16(d) == 2 ? be16(d) : 0;
    }
    free(b);
}

static int32_t spit_call(qtc_call *k)
{
    exporter *ex = k->self;
    const uint32_t *a = k->a;
    uint32_t kind = k->cls->subtype;
    if (!k->call)
        return qtcBadSelector;
    const char *name = k->call->name + 11; /* after "MovieExport" */
    if (!strcmp(name, "ToFile") || !strcmp(name, "ToHandle") || !strcmp(name, "ToDataRef")) {
        bool ref = !strcmp(name, "ToDataRef");
        int o = ref ? 1 : 0;
        gaddr movie = a[1 + o], only = a[2 + o];
        int32_t start = (int32_t)a[3 + o], dur = (int32_t)a[4 + o];
        bool to_file = !strcmp(name, "ToFile") || ref;
        buf out = { 0 };
        int16_t err = export_bytes(ex, kind, movie, only, start, dur, to_file && kind == fourcc("PICT"), &out);
        if (!err) {
            if (!strcmp(name, "ToHandle"))
                err = write_handle(a[0], out.b, out.n);
            else if (kind == fourcc("sfil") && !ref)
                err = write_sound_file(a[0], &out);
            else if (ref)
                err = write_dataref(a[0], a[1], kind, creator_of(kind), out.b, out.n);
            else
                err = write_spec(a[0], kind, creator_of(kind), out.b, out.n);
        }
        free(out.b);
        return err;
    }
    if (!strcmp(name, "Validate")) {
        qtm_movie *mv = qt_movie_model(a[0]);
        qtm_track *t[1];
        bool valid = mv != NULL;
        uint32_t need = kind == fourcc("TEXT") ? fourcc("text") : kind == fourcc("Midi") ? fourcc("musi")
                        : kind == fourcc("PICT") ? fourcc("vide") : kind == fourcc("MooV") ? 0 : fourcc("soun");
        if (valid && need)
            valid = tracks_of(mv, a[1], need, t, 1) > 0 || (need == fourcc("vide") && mv->ntracks);
        if (a[2])
            wr8(a[2], valid);
        return noErr;
    }
    if (!strcmp(name, "DoUserDialog")) {
        /* our exporters' settings come from their atom containers */
        if (a[4])
            wr8(a[4], 0);
        return noErr;
    }
    if (!strcmp(name, "GetCreatorType")) {
        if (a[0])
            wr32(a[0], creator_of(kind));
        return noErr;
    }
    if (!strcmp(name, "GetFileNameExtension")) {
        static const char *const ext[][2] = { { "MooV", "mov " }, { "AIFF", "aif " }, { "WAVE", "wav " },
                                              { "ULAW", "au  " }, { "sfil", "snd " }, { "PICT", "pct " },
                                              { "TEXT", "txt " }, { "Midi", "mid " } };
        for (size_t i = 0; i < sizeof ext / sizeof ext[0]; i++)
            if (fourcc(ext[i][0]) == kind && a[0])
                wr32(a[0], fourcc(ext[i][1]));
        return noErr;
    }
    if (!strcmp(name, "GetShortFileTypeString")) {
        const char *s = k->cls->name;
        if (a[0]) {
            size_t n = strlen(s);
            wr8(a[0], (uint8_t)n);
            gput(a[0] + 1, s, (uint32_t)n);
        }
        return noErr;
    }
    if (!strcmp(name, "GetSourceMediaType")) {
        uint32_t t = kind == fourcc("TEXT") ? fourcc("text") : kind == fourcc("Midi") ? fourcc("musi")
                     : kind == fourcc("PICT") ? fourcc("vide") : kind == fourcc("MooV") ? 0 : fourcc("soun");
        if (a[0])
            wr32(a[0], t);
        return noErr;
    }
    if (!strcmp(name, "GetSettingsAsAtomContainer")) {
        if (!a[0])
            return paramErr;
        wr32(a[0], settings_of(ex, kind));
        return noErr;
    }
    if (!strcmp(name, "SetSettingsFromAtomContainer")) {
        apply_settings(ex, a[0]);
        return noErr;
    }
    if (!strcmp(name, "SetProgressProc")) {
        ex->progress_proc = a[0];
        ex->progress_refcon = (int32_t)a[1];
        return noErr;
    }
    if (!strcmp(name, "GetAuxiliaryData"))
        return auxiliaryExportDataUnavailable;
    if (!strcmp(name, "SetSampleDescription"))
        return noErr;
    return qtcBadSelector;
}

#define FILES (canExportFiles | canExportHandles | canExportValidateMovie)
#define SPIT(sub, flags, name) \
    { 0x73706974, sub, 0x6170706C, flags, name, NULL, 0x00030000, { "MovieExport" }, sizeof(exporter), spit_call, 0 }
static qtc_class exporters[] = {
    SPIT(0x4D6F6F56, FILES, "QuickTime Movie"),                        /* MooV */
    SPIT(0x41494646, FILES, "AIFF"),                                   /* AIFF */
    SPIT(0x57415645, FILES, "Wave"),                                   /* WAVE */
    SPIT(0x554C4157, FILES, "\xB5Law"),                                /* ULAW */
    SPIT(0x7366696C, canExportFiles | canExportValidateMovie | needsResourceFork, "System 7 Sound"), /* sfil */
    SPIT(0x50494354, FILES, "Picture"),                                /* PICT */
    SPIT(0x54455854, FILES, "Text"),                                   /* TEXT */
    SPIT(0x4D696469, FILES, "MIDI"),                                   /* Midi */
};

void qt_export_register(void)
{
    for (size_t i = 0; i < sizeof exporters / sizeof exporters[0]; i++)
        qtc_register(&exporters[i]);
}
