/*
 * Movie importers of our own ('eat ' components): files and handles that
 * aren't movies (sounds, pictures, MIDI files, text) made into tracks of a
 * movie, as QuickTime 3 did.  NewMovieFromFile on such a file, pasting a
 * picture or sound, ConvertFileToMovieFile and the programs' own
 * MovieImport calls all come here.
 *
 * Sounds QuickTime plays as they are (AIFF and AIFC, WAVE, System 7 sound
 * files and 'snd ' handles, Sun audio) are imported in place: the track's
 * samples are where the file has them.  Encodings our sound decoders
 * don't take are converted to 16-bit samples kept in a handle.  A JPEG
 * file is a Photo-JPEG frame in place; other pictures (PICT, PICS, GIF,
 * PNG, BMP, TIFF, Photoshop and the rest the host's image decoder reads)
 * become 32-bit frames.  MIDI files become music tracks, text files text
 * tracks (QTtext's descriptors and time stamps understood).
 */
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "filemgr_internal.h"
#include "qd_internal.h"
#include "qt_atoms.h"
#include "qt_codec.h"
#include "qt_comp.h"
#include "qt_dref.h"
#include "qt_errors.h"
#include "qt_import.h"
#include "qt_movie_internal.h"
#include "tb_internal.h"

enum {
    canImportHandles = 1 << 0, canImportFiles = 1 << 1, hasUserInterface = 1 << 2,
    canValidateHandles = 1 << 8, canValidateFile = 1 << 9, canImportInPlace = 1 << 11,
    subtypeIsExtension = 1 << 12, hasMIMEList = 1 << 14, canImportDataRefs = 1 << 18,
    canValidateDataRefs = 1 << 21,
    movieImportCreateTrack = 1, movieImportMustUseTrack = 4,
    movieImportResultUsedMultipleTracks = 8, movieImportResultComplete = 64,
    kMovieLoadStateComplete = 100000, newMovieActive = 1,
};

/* What a file or handle holds. */
enum {
    F_NONE, F_AIFF, F_WAVE, F_SND, F_AU, F_MIDI, F_PICT, F_PICS, F_JPEG, F_IMAGE, F_TEXT,
};

typedef struct {
    int32_t chunk_size;
    int32_t width, height;       /* Fixed, SetDimensions's */
    int32_t duration;            /* SetDuration's, in the movie's time scale */
    int32_t sample_duration, sample_scale;
    uint64_t offset, limit;      /* SetOffsetAndLimit's: 0, 0 all of it */
    gaddr media_file;            /* SetMediaFile's alias */
    gaddr progress_proc;
    int32_t progress_refcon;
    bool dont_block, from_scrap;
    gaddr aux;                   /* SetAuxiliaryData's handle */
    uint32_t aux_type;
    gaddr desc;                  /* SetSampleDescription's */
    uint32_t desc_type;
} importer;

/* ---- Bytes ---- */

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[1] << 8 | p[0]); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v; }
static void put32b(uint8_t *p, uint32_t v) { put16(p, v >> 16), put16(p + 2, v); }

/* An 80-bit IEEE extended number (AIFF's sample rate). */
static double extended(const uint8_t *p)
{
    int exp = (p[0] & 0x7F) << 8 | p[1];
    uint64_t mant = 0;
    for (int i = 0; i < 8; i++)
        mant = mant << 8 | p[2 + i];
    if (!exp && !mant)
        return 0;
    double v = ldexp((double)mant, exp - 16383 - 63);
    return p[0] & 0x80 ? -v : v;
}

/* What the bytes are, from their first ones (and the file's type). */
static int sniff(const uint8_t *b, size_t n, uint32_t type)
{
    if (n >= 12 && !memcmp(b, "FORM", 4) && (!memcmp(b + 8, "AIFF", 4) || !memcmp(b + 8, "AIFC", 4)))
        return F_AIFF;
    if (n >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4))
        return F_WAVE;
    if (n >= 24 && !memcmp(b, ".snd", 4))
        return F_AU;
    if (n >= 14 && !memcmp(b, "MThd", 4))
        return F_MIDI;
    if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF)
        return F_JPEG;
    if ((n >= 6 && !memcmp(b, "GIF8", 4)) || (n >= 8 && !memcmp(b, "\x89PNG", 4)) || (n >= 2 && !memcmp(b, "BM", 2) && type != fourcc("TEXT")) ||
        (n >= 4 && (!memcmp(b, "II*\0", 4) || !memcmp(b, "MM\0*", 4))) || (n >= 4 && !memcmp(b, "8BPS", 4)))
        return F_IMAGE;
    switch (type) {
    case 0x41494646: case 0x41494643: return F_AIFF;                  /* AIFF AIFC */
    case 0x57415645: return F_WAVE;                                   /* WAVE */
    case 0x7366696C: return F_SND;                                    /* sfil */
    case 0x554C4157: return F_AU;                                     /* ULAW */
    case 0x4D696469: return F_MIDI;                                   /* Midi */
    case 0x50494354: return F_PICT;                                   /* PICT */
    case 0x50494353: return F_PICS;                                   /* PICS */
    case 0x54455854: return F_TEXT;                                   /* TEXT */
    case 0x4A504547: return F_JPEG;                                   /* JPEG */
    case 0x47494666: case 0x504E4766: case 0x54494646: case 0x424D5066: case 0x38425053:
    case 0x504E5447: case 0x7174696D: case 0x54504943: case 0x2E534749: case 0x71746966:
        return F_IMAGE;                       /* GIFf PNGf TIFF BMPf 8BPS PNTG qtim TPIC .SGI qtif */
    case 0x736E6420: return F_SND;                                    /* 'snd ' handles */
    }
    /* a PICT file or handle: its frame then a version opcode */
    for (size_t at = 0; at <= 512; at += 512)
        if (n >= at + 12 && (!memcmp(b + at + 10, "\x11\x01", 2) || !memcmp(b + at + 10, "\x00\x11\x02\xFF", 4)))
            return F_PICT;
    return F_NONE;
}

/* ---- Where the bytes come from ---- */

typedef struct {
    uint8_t *b;
    size_t n;
    char path[PATH_MAX];  /* the file, when imported from one: samples stay there */
    bool file;
    gaddr spec;           /* its FSSpec (resource forks) */
    uint32_t type;        /* its Finder type, or the handle's */
} source;

static uint8_t *read_host_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(len > 0 ? (size_t)len : 1);
    if (b && len > 0 && fread(b, 1, (size_t)len, f) != (size_t)len) {
        free(b);
        b = NULL;
    }
    fclose(f);
    *n = len > 0 ? (size_t)len : 0;
    return b;
}

static uint32_t finder_type(gaddr spec)
{
    gaddr info = tb_NewPtr(16, false, true);
    uint32_t t = tb_FSpGetFInfo(spec, info) == noErr ? rd32(info) : 0;
    tb_DisposePtr(info);
    return t;
}

static void apply_limits(const importer *im, source *s)
{
    if (!im || (!im->offset && !im->limit) || im->offset >= s->n)
        return;
    size_t end = im->limit && im->limit < s->n ? (size_t)im->limit : s->n;
    if (end <= im->offset)
        return;
    memmove(s->b, s->b + im->offset, end - (size_t)im->offset);
    s->n = end - (size_t)im->offset;
    s->file = false; /* offsets no longer the file's */
}

static bool source_file(source *s, const char *path, gaddr spec)
{
    memset(s, 0, sizeof *s);
    s->b = read_host_file(path, &s->n);
    if (!s->b)
        return false;
    snprintf(s->path, sizeof s->path, "%s", path);
    s->file = true;
    s->spec = spec;
    s->type = spec ? finder_type(spec) : 0;
    return true;
}

static bool source_spec(source *s, gaddr spec)
{
    char path[PATH_MAX];
    if (!spec || !fm_host_path((int16_t)rd16(spec), (int32_t)rd32(spec + 2), spec + 6, true, path, sizeof path))
        return false;
    return source_file(s, path, spec);
}

static bool source_handle(source *s, gaddr h, uint32_t type)
{
    memset(s, 0, sizeof *s);
    if (!h || !rd32(h))
        return false;
    s->n = (size_t)tb_GetHandleSize(h);
    s->b = malloc(s->n ? s->n : 1);
    gget(s->b, rd32(h), (uint32_t)s->n);
    s->type = type;
    return true;
}

static bool source_dataref(source *s, gaddr dataRef, uint32_t type)
{
    memset(s, 0, sizeof *s);
    uint32_t rn;
    uint8_t *ref = qt_dref_bytes(dataRef, &rn);
    char path[PATH_MAX];
    bool ok = false;
    if (qt_dref_path(type, ref, rn, NULL, path, sizeof path)) {
        gaddr spec = tb_NewPtr(FSSpec_SIZEOF, false, true);
        fm_spec_for_path(path, spec);
        ok = source_file(s, path, spec);
        if (!ok)
            tb_DisposePtr(spec);
    } else {
        qt_src src;
        if (qt_src_open(type, 0, ref, rn, NULL, false, &src)) {
            int64_t n = qt_src_size(&src);
            s->b = malloc(n > 0 ? (size_t)n : 1);
            ok = s->b && n >= 0 && qt_src_read(&src, 0, s->b, (uint32_t)n);
            s->n = ok ? (size_t)n : 0;
            qt_src_close(&src);
        }
    }
    free(ref);
    return ok;
}

static void source_free(source *s, bool own_spec)
{
    free(s->b);
    if (own_spec && s->spec)
        tb_DisposePtr(s->spec);
    memset(s, 0, sizeof *s);
}

/* ---- Tracks, media and samples ---- */

typedef struct {
    gaddr movie, target;
    int32_t at, flags;
    gaddr used_track;
    int32_t added;               /* movie time */
    int ntracks;
} dest;

/* The track the samples go in: the target if it has media of the type and
 * a new one wasn't asked for, else a new track. */
static gaddr track_for(dest *d, uint32_t mtype, int32_t w, int32_t h, int16_t volume, int32_t scale, gaddr dref,
                       uint32_t dref_type, gaddr *media)
{
    gaddr t = 0;
    *media = 0;
    if (d->target && !(d->flags & movieImportCreateTrack)) {
        gaddr m = (gaddr)tb_GetTrackMedia((int32_t)d->target);
        qtm_media *md = m ? qt_media_model(m, NULL, NULL) : NULL;
        if (md && md->type == mtype) {
            t = d->target;
            *media = m;
            if (dref) { /* the data's reference among the media's own */
                uint32_t n;
                uint8_t *b = qt_dref_bytes(dref, &n);
                md->default_dref = (int16_t)qtm_add_dref(md, dref_type, 0, b, n);
                free(b);
            }
        } else if (d->flags & movieImportMustUseTrack) {
            return 0;
        }
    }
    if (!t) {
        t = (gaddr)tb_NewMovieTrack((int32_t)d->movie, w, h, volume);
        if (!t)
            return 0;
        *media = (gaddr)tb_NewTrackMedia((int32_t)t, (int32_t)mtype, scale, (int32_t)dref, (int32_t)dref_type);
        if (!*media)
            return 0;
    }
    if (!d->used_track || d->ntracks == 0)
        d->used_track = t;
    d->ntracks++;
    return t;
}

/* A reference to the file (in place), or to a new handle of the bytes. */
static gaddr dref_for(const source *s, bool in_place, const uint8_t *copy, size_t n, uint32_t *type)
{
    if (in_place && s->file) {
        *type = fourcc("alis");
        return qt_dref_for_path(s->path);
    }
    gaddr h = tb_NewHandle((int32_t)n, false, false);
    if (!h)
        return 0;
    if (n)
        gput(rd32(h), copy, (uint32_t)n);
    uint8_t ref[4];
    put32b(ref, h);
    *type = fourcc("hndl");
    return qt_dref_handle(ref, 4);
}

/* The media's new samples in the track at the import's time. */
static int16_t insert(dest *d, gaddr track, gaddr media, int64_t media_start)
{
    qtm_movie *mv;
    qtm_track *t;
    qtm_media *md = qt_media_model(media, &t, &mv);
    if (!md)
        return invalidMedia;
    md->duration = qtm_media_samples_duration(md);
    qt_movie_touch(mv);
    int64_t len = md->duration - media_start;
    int16_t err = tb_InsertMediaIntoTrack((int32_t)track, d->at < 0 ? 0 : d->at, (int32_t)media_start, (int32_t)len,
                                          0x10000);
    if (err)
        return err;
    int32_t movie_len = md->scale ? (int32_t)(len * (int64_t)mv->scale / md->scale) : 0;
    if (movie_len > d->added)
        d->added = movie_len;
    return noErr;
}

static int add_desc(gaddr media, uint8_t *desc, uint32_t len)
{
    qtm_media *md = qt_media_model(media, NULL, NULL);
    if (!md)
        return 0;
    desc[14] = (uint8_t)(md->default_dref >> 8);
    desc[15] = (uint8_t)md->default_dref;
    int i = qtm_find_desc(md, desc, len);
    return i ? i : qtm_add_desc(md, desc, len);
}

/* ---- Sound ---- */

typedef struct {
    uint32_t format;       /* as QuickTime names it */
    int channels, bits;
    double rate;
    size_t offset, bytes;  /* the samples in the source */
    uint32_t frames;
    int16_t *pcm;          /* or converted: 16-bit frames (malloc'd) */
} sound;

static uint32_t frames_per_packet(uint32_t f)
{
    return f == fourcc("ima4") ? 64 : f == fourcc("MAC3") || f == fourcc("MAC6") ? 6 : 1;
}

static uint32_t bytes_per_packet(uint32_t f, int bits)
{
    return f == fourcc("ima4") ? 34 : f == fourcc("MAC3") ? 2 : f == fourcc("MAC6") ? 1 : (uint32_t)(bits + 7) / 8;
}

/* Whether our sound decoders play the encoding as it is. */
static bool plays_as_is(const sound *s)
{
    qt_sound_format f = { .format = s->format, .channels = s->channels, .bits = s->bits, .rate = s->rate };
    return qt_adec_supported(&f);
}

static int16_t ulaw(uint8_t u)
{
    u = (uint8_t)~u;
    int t = ((u & 0x0F) << 3) + 0x84;
    t <<= (u & 0x70) >> 4;
    return (int16_t)(u & 0x80 ? 0x84 - t : t - 0x84);
}

static int16_t alaw(uint8_t a)
{
    a ^= 0x55;
    int t = (a & 0x0F) << 4, seg = (a & 0x70) >> 4;
    t = seg == 0 ? t + 8 : seg == 1 ? t + 0x108 : (t + 0x108) << (seg - 1);
    return (int16_t)(a & 0x80 ? t : -t);
}

static int16_t clip16(double v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

/* Samples of an encoding we don't decode, as 16-bit frames: 'ulaw',
 * 'alaw', 'fl32', 'fl64', 'in24', 'in32' (big-endian, or little with
 * little set) and 8-bit or 16-bit of either order. */
static bool to_pcm(sound *s, const uint8_t *d, size_t n, bool little)
{
    size_t per = s->format == fourcc("ulaw") || s->format == fourcc("alaw") ? 1
                 : s->format == fourcc("fl64") ? 8
                 : s->format == fourcc("fl32") || s->format == fourcc("in32") ? 4
                 : s->format == fourcc("in24") ? 3 : (size_t)(s->bits + 7) / 8;
    size_t count = n / per;
    count -= count % (size_t)s->channels;
    s->pcm = malloc((count ? count : 1) * sizeof *s->pcm);
    if (!s->pcm)
        return false;
    for (size_t i = 0; i < count; i++) {
        const uint8_t *p = d + i * per;
        int16_t v;
        if (s->format == fourcc("ulaw")) {
            v = ulaw(p[0]);
        } else if (s->format == fourcc("alaw")) {
            v = alaw(p[0]);
        } else if (s->format == fourcc("fl32")) {
            uint32_t w = little ? le32(p) : be32(p);
            float f;
            memcpy(&f, &w, 4);
            v = clip16(f * 32768.0);
        } else if (s->format == fourcc("fl64")) {
            uint64_t w = 0;
            for (int k = 0; k < 8; k++)
                w = w << 8 | p[little ? 7 - k : k];
            double f;
            memcpy(&f, &w, 8);
            v = clip16(f * 32768.0);
        } else if (per == 4) {
            v = (int16_t)((little ? le32(p) : be32(p)) >> 16);
        } else if (per == 3) {
            v = (int16_t)(little ? p[2] << 8 | p[1] : p[0] << 8 | p[1]);
        } else if (per == 2) {
            v = (int16_t)(little ? le16(p) : be16(p));
        } else {
            v = (int16_t)(s->format == fourcc("raw ") ? (p[0] - 128) << 8 : (int8_t)p[0] * 256);
        }
        s->pcm[i] = v;
    }
    s->frames = (uint32_t)(count / (size_t)s->channels);
    return true;
}

/* Microsoft's IMA ADPCM (WAVE format 0x11): blocks of blockAlign bytes per
 * all channels, each channel's header a sample and a step index, then
 * 4-byte groups of eight nibbles per channel in turn. */
static const int16_t ima_steps[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};
static const int8_t ima_index[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

static int16_t ima_step(int *pred, int *index, int nib)
{
    int step = ima_steps[*index], diff = step >> 3;
    if (nib & 1)
        diff += step >> 2;
    if (nib & 2)
        diff += step >> 1;
    if (nib & 4)
        diff += step;
    *pred += nib & 8 ? -diff : diff;
    *pred = *pred < -32768 ? -32768 : *pred > 32767 ? 32767 : *pred;
    *index += ima_index[nib];
    *index = *index < 0 ? 0 : *index > 88 ? 88 : *index;
    return (int16_t)*pred;
}

static bool ms_ima(sound *s, const uint8_t *d, size_t n, size_t align)
{
    size_t ch = (size_t)s->channels;
    if (align < 4 * ch + 4)
        return false;
    size_t per_block = (align - 4 * ch) * 2 / ch + 1, blocks = n / align;
    s->pcm = malloc((blocks * per_block * ch + 1) * sizeof *s->pcm);
    if (!s->pcm)
        return false;
    size_t out = 0;
    for (size_t b = 0; b < blocks; b++) {
        const uint8_t *p = d + b * align;
        int pred[2], index[2];
        for (size_t c = 0; c < ch; c++) {
            pred[c] = (int16_t)le16(p + 4 * c);
            index[c] = p[4 * c + 2] > 88 ? 88 : p[4 * c + 2];
            s->pcm[out + c] = (int16_t)pred[c];
        }
        const uint8_t *q = p + 4 * ch;
        size_t groups = (align - 4 * ch) / (4 * ch);
        for (size_t g = 0; g < groups; g++)
            for (size_t c = 0; c < ch; c++)
                for (int k = 0; k < 8; k++) {
                    int nib = (q[(g * ch + c) * 4 + (size_t)k / 2] >> ((k & 1) * 4)) & 15;
                    s->pcm[out + (1 + g * 8 + (size_t)k) * ch + c] = ima_step(&pred[c], &index[c], nib);
                }
        out += per_block * ch;
    }
    s->frames = (uint32_t)(out / ch);
    return true;
}

/* Microsoft ADPCM (WAVE format 2), with the standard coefficients. */
static bool ms_adpcm(sound *s, const uint8_t *d, size_t n, size_t align)
{
    static const int c1[7] = { 256, 512, 0, 192, 240, 460, 392 }, c2[7] = { 0, -256, 0, 64, 0, -208, -232 };
    static const int adapt[16] = { 230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230 };
    size_t ch = (size_t)s->channels;
    if (align < 7 * ch)
        return false;
    size_t per_block = (align - 7 * ch) * 2 / ch + 2, blocks = n / align;
    s->pcm = malloc((blocks * per_block * ch + 1) * sizeof *s->pcm);
    if (!s->pcm)
        return false;
    size_t out = 0;
    for (size_t b = 0; b < blocks; b++) {
        const uint8_t *p = d + b * align;
        int pred[2], delta[2], s1[2], s2[2];
        for (size_t c = 0; c < ch; c++) {
            pred[c] = p[c] > 6 ? 0 : p[c];
            delta[c] = (int16_t)le16(p + ch + 2 * c);
            s1[c] = (int16_t)le16(p + 3 * ch + 2 * c);
            s2[c] = (int16_t)le16(p + 5 * ch + 2 * c);
            s->pcm[out + c] = (int16_t)s2[c];
            s->pcm[out + ch + c] = (int16_t)s1[c];
        }
        size_t at = out + 2 * ch;
        for (size_t i = 7 * ch; i < align; i++)
            for (int half = 0; half < 2; half++) {
                size_t c = ch == 2 ? (size_t)half : 0;
                int nib = half ? p[i] & 15 : p[i] >> 4, sn = nib & 8 ? nib - 16 : nib;
                int v = (s1[c] * c1[pred[c]] + s2[c] * c2[pred[c]]) / 256 + sn * delta[c];
                v = v < -32768 ? -32768 : v > 32767 ? 32767 : v;
                s2[c] = s1[c];
                s1[c] = v;
                delta[c] = delta[c] * adapt[nib] / 256;
                if (delta[c] < 16)
                    delta[c] = 16;
                s->pcm[at++] = (int16_t)v;
            }
        out += per_block * ch;
    }
    s->frames = (uint32_t)(out / ch);
    return true;
}

static bool parse_aiff(const uint8_t *b, size_t n, sound *s)
{
    bool aifc = !memcmp(b + 8, "AIFC", 4), have_comm = false;
    size_t at = 12, end = 8 + (size_t)be32(b + 4) < n ? 8 + (size_t)be32(b + 4) : n;
    const uint8_t *ssnd = NULL;
    size_t ssnd_len = 0, ssnd_at = 0;
    s->format = fourcc("twos");
    while (at + 8 <= end) {
        uint32_t len = be32(b + at + 4);
        const uint8_t *c = b + at + 8;
        if (at + 8 + len > end)
            len = (uint32_t)(end - at - 8);
        if (!memcmp(b + at, "COMM", 4) && len >= 18) {
            s->channels = be16(c);
            s->frames = be32(c + 2);
            s->bits = be16(c + 6);
            s->rate = extended(c + 8);
            if (aifc && len >= 22)
                s->format = be32(c + 18);
            have_comm = true;
        } else if (!memcmp(b + at, "SSND", 4) && len >= 8) {
            ssnd_at = at + 16 + be32(c);
            ssnd = b + ssnd_at;
            ssnd_len = len - 8 - be32(c) <= len ? len - 8 - be32(c) : 0;
        }
        at += 8 + len + (len & 1);
    }
    if (!have_comm || !ssnd || s->channels < 1 || s->rate <= 0)
        return false;
    if (s->format == fourcc("NONE") || s->format == fourcc("twos") || s->format == fourcc("raw "))
        s->format = fourcc("twos");
    if (s->format == fourcc("sowt") && s->bits <= 8)
        s->format = fourcc("twos");
    s->offset = ssnd_at;
    s->bytes = ssnd_len;
    uint32_t fpp = frames_per_packet(s->format), bpp = bytes_per_packet(s->format, s->bits);
    if (fpp > 1) /* compressed: AIFC counts packets */
        s->frames = (uint32_t)(ssnd_len / (bpp * (size_t)s->channels)) * fpp;
    else if ((uint64_t)s->frames * bpp * (size_t)s->channels > ssnd_len)
        s->frames = (uint32_t)(ssnd_len / (bpp * (size_t)s->channels));
    if (plays_as_is(s))
        return true;
    return to_pcm(s, ssnd, ssnd_len, s->format == fourcc("sowt"));
}

static bool parse_wave(const uint8_t *b, size_t n, sound *s)
{
    size_t at = 12;
    uint16_t tag = 0, align = 0;
    const uint8_t *data = NULL;
    size_t data_len = 0, data_at = 0;
    while (at + 8 <= n) {
        uint32_t len = le32(b + at + 4);
        const uint8_t *c = b + at + 8;
        if (at + 8 + len > n)
            len = (uint32_t)(n - at - 8);
        if (!memcmp(b + at, "fmt ", 4) && len >= 16) {
            tag = le16(c);
            s->channels = le16(c + 2);
            s->rate = le32(c + 4);
            align = le16(c + 12);
            s->bits = le16(c + 14);
            if (tag == 0xFFFE && len >= 26)
                tag = le16(c + 24); /* WAVE_FORMAT_EXTENSIBLE: the subformat */
        } else if (!memcmp(b + at, "data", 4)) {
            data = c;
            data_at = at + 8;
            data_len = len;
        }
        at += 8 + len + (len & 1);
    }
    if (!tag || !data || s->channels < 1 || s->channels > 8 || s->rate <= 0)
        return false;
    s->offset = data_at;
    s->bytes = data_len;
    if (tag == 1 && (s->bits == 8 || s->bits == 16)) {
        s->format = fourcc(s->bits == 8 ? "raw " : "sowt");
        s->frames = (uint32_t)(data_len / ((size_t)s->channels * (size_t)(s->bits / 8)));
        if (plays_as_is(s))
            return true;
    }
    switch (tag) {
    case 1: s->format = fourcc(s->bits <= 8 ? "raw " : s->bits <= 16 ? "sowt" : s->bits <= 24 ? "in24" : "in32"); break;
    case 3: s->format = fourcc(s->bits == 64 ? "fl64" : "fl32"); break;
    case 6: s->format = fourcc("alaw"); break;
    case 7: s->format = fourcc("ulaw"); break;
    case 2: return ms_adpcm(s, data, data_len, align);
    case 0x11: return ms_ima(s, data, data_len, align);
    default: return false;
    }
    return to_pcm(s, data, data_len, true);
}

static bool parse_au(const uint8_t *b, size_t n, sound *s)
{
    uint32_t off = be32(b + 4), len = be32(b + 8), enc = be32(b + 12);
    s->rate = be32(b + 16);
    s->channels = (int)be32(b + 20);
    if (off > n || s->channels < 1 || s->channels > 8 || s->rate <= 0)
        return false;
    if (len == 0xFFFFFFFF || off + (size_t)len > n)
        len = (uint32_t)(n - off);
    s->offset = off;
    s->bytes = len;
    switch (enc) {
    case 2: s->format = fourcc("twos"), s->bits = 8; break;
    case 3: s->format = fourcc("twos"), s->bits = 16; break;
    case 1: s->format = fourcc("ulaw"), s->bits = 8; break;
    case 27: s->format = fourcc("alaw"), s->bits = 8; break;
    case 4: s->format = fourcc("in24"), s->bits = 24; break;
    case 5: s->format = fourcc("in32"), s->bits = 32; break;
    case 6: s->format = fourcc("fl32"), s->bits = 32; break;
    case 7: s->format = fourcc("fl64"), s->bits = 64; break;
    default: return false;
    }
    s->frames = (uint32_t)(len / ((size_t)s->channels * (size_t)((s->bits + 7) / 8)));
    if (plays_as_is(s))
        return true;
    return to_pcm(s, b + off, len, false);
}

/* A 'snd ' resource: format 1 or 2, its bufferCmd or soundCmd naming the
 * sampled sound header; standard, extended or compressed. */
static bool parse_snd(const uint8_t *b, size_t n, sound *s)
{
    if (n < 6)
        return false;
    size_t at;
    int fmt = be16(b);
    if (fmt == 1) {
        size_t mods = be16(b + 2);
        at = 4 + mods * 6;
    } else if (fmt == 2) {
        at = 4;
    } else {
        return false;
    }
    if (at + 2 > n)
        return false;
    size_t cmds = be16(b + at), hdr = 0;
    at += 2;
    for (size_t i = 0; i < cmds && at + 8 <= n; i++, at += 8)
        if ((be16(b + at) & 0x7FFF) == 80 || (be16(b + at) & 0x7FFF) == 81) /* soundCmd, bufferCmd */
            hdr = be32(b + at + 4);
    if (!hdr || hdr + 22 > n)
        return false;
    const uint8_t *h = b + hdr;
    uint32_t len = be32(h + 4);
    s->rate = be32(h + 8) / 65536.0;
    int enc = h[20];
    if (enc == 0) { /* standard: 8-bit offset binary, mono */
        s->format = fourcc("raw "), s->channels = 1, s->bits = 8;
        s->offset = hdr + 22;
        s->bytes = len <= n - s->offset ? len : n - s->offset;
        s->frames = (uint32_t)s->bytes;
        return s->rate > 0;
    }
    if (hdr + 64 > n)
        return false;
    s->channels = (int)len;
    s->frames = be32(h + 22);
    s->offset = hdr + 64;
    if (enc == 0xFF) { /* extended */
        s->bits = be16(h + 48);
        s->format = fourcc(s->bits == 8 ? "raw " : "twos");
        s->bytes = (size_t)s->frames * (size_t)s->channels * (size_t)(s->bits / 8);
    } else if (enc == 0xFE) { /* compressed */
        int16_t id = (int16_t)be16(h + 56);
        s->format = id == 3 ? fourcc("MAC3") : id == 4 ? fourcc("MAC6") : be32(h + 40);
        s->bits = be16(h + 62) ? be16(h + 62) : 16;
        uint32_t fpp = frames_per_packet(s->format), bpp = bytes_per_packet(s->format, s->bits);
        if (fpp > 1) {
            s->bytes = (size_t)s->frames * bpp * (size_t)s->channels; /* frames: packets */
            s->frames *= fpp;
        } else {
            s->bytes = (size_t)s->frames * (size_t)s->channels * bpp;
        }
    } else {
        return false;
    }
    if (s->offset + s->bytes > n)
        s->bytes = n - s->offset;
    if (s->rate <= 0 || s->channels < 1)
        return false;
    if (plays_as_is(s))
        return true;
    return to_pcm(s, b + s->offset, s->bytes, false);
}

/* SoundDescription: version 0, or 1 for compressed encodings. */
static int sound_desc(gaddr media, const sound *s, uint32_t format, int bits)
{
    uint32_t fpp = frames_per_packet(format);
    uint8_t d[52] = { 0 };
    uint32_t len = fpp > 1 ? 52 : 36;
    put32b(d, len);
    put32b(d + 4, format);
    put16(d + 16, fpp > 1 ? 1 : 0);
    put32b(d + 20, fourcc("appl"));
    put16(d + 24, (uint32_t)s->channels);
    put16(d + 26, (uint32_t)(fpp > 1 ? 16 : bits));
    put16(d + 28, fpp > 1 ? 0xFFFE : 0);
    put32b(d + 32, s->rate < 65536 ? (uint32_t)(s->rate * 65536.0) : 0);
    if (fpp > 1) {
        uint32_t bpp = bytes_per_packet(format, bits);
        put32b(d + 36, fpp);
        put32b(d + 40, bpp);
        put32b(d + 44, bpp * (uint32_t)s->channels);
        put32b(d + 48, 2);
    }
    return add_desc(media, d, len);
}

static int16_t import_sound(dest *d, const source *src, sound *s)
{
    bool converted = s->pcm != NULL;
    uint32_t format = converted ? fourcc("twos") : s->format;
    int bits = converted ? 16 : s->bits;
    size_t nbytes = converted ? (size_t)s->frames * (size_t)s->channels * 2 : s->bytes;
    uint8_t *copy = NULL;
    if (converted) {
        copy = malloc(nbytes ? nbytes : 1);
        for (size_t i = 0; i < (size_t)s->frames * (size_t)s->channels; i++)
            put16(copy + 2 * i, (uint16_t)s->pcm[i]);
    }
    uint32_t dtype;
    gaddr dref = dref_for(src, !converted, converted ? copy : src->b + s->offset, nbytes, &dtype);
    free(copy);
    int32_t scale = (int32_t)lround(s->rate);
    gaddr media;
    gaddr track = track_for(d, fourcc("soun"), 0, 0, 0x100, scale > 0 ? scale : 22050, dref, dtype, &media);
    if (dref)
        tb_DisposeHandle(dref);
    if (!track)
        return d->flags & movieImportMustUseTrack ? mediaTypesDontMatch : tb_GetMoviesError();
    qtm_media *md = qt_media_model(media, NULL, NULL);
    int desc = sound_desc(media, s, format, bits);
    int64_t start = qtm_media_samples_duration(md);
    uint64_t base = converted || !src->file ? 0 : s->offset;
    if (!converted && !src->file) { /* copied into a handle of its own */
        qt_src hs;
        const qtm_dref *r = &md->drefs[md->default_dref - 1];
        if (qt_src_open(r->type, r->flags, r->data, r->len, NULL, false, &hs)) {
            base = (uint64_t)qt_src_size(&hs) - nbytes;
            qt_src_close(&hs);
        }
    }
    /* chunks of about half a second, whole packets */
    uint32_t fpp = frames_per_packet(format), bpp = bytes_per_packet(format, bits) * (uint32_t)s->channels;
    uint32_t per = (uint32_t)(s->rate / 2) / fpp * fpp;
    if (per < fpp)
        per = fpp;
    for (uint32_t f = 0; f < s->frames; f += per) {
        uint32_t count = s->frames - f < per ? s->frames - f : per;
        uint64_t off = base + (uint64_t)(f / fpp) * bpp, bytes = (uint64_t)((count + fpp - 1) / fpp) * bpp;
        qtm_add_samples(md, off, bytes, count, fpp > 1 ? 1 : bpp, 1, (uint32_t)desc, true, true);
    }
    return insert(d, track, media, start);
}

/* ---- Pictures ---- */

/* ImageDescription of 86 bytes. */
static int image_desc(gaddr media, uint32_t ctype, int w, int h, int depth, uint32_t size, const char *name)
{
    uint8_t d[86] = { 0 };
    put32b(d, 86);
    put32b(d + 4, ctype);
    put32b(d + 20, fourcc("appl"));
    put32b(d + 28, 0x200);
    put16(d + 32, (uint32_t)w);
    put16(d + 34, (uint32_t)h);
    put32b(d + 36, 72u << 16);
    put32b(d + 40, 72u << 16);
    put32b(d + 44, size);
    put16(d + 48, 1);
    size_t n = strlen(name) > 31 ? 31 : strlen(name);
    d[50] = (uint8_t)n;
    memcpy(d + 51, name, n);
    put16(d + 82, (uint32_t)depth);
    put16(d + 84, 0xFFFF);
    return add_desc(media, d, 86);
}

/* A picture's pixels, drawn in a 32-bit GWorld: RGBA (malloc'd). */
static uint8_t *picture_rgba(gaddr pic, int *w, int *h)
{
    gaddr p = rd32(pic);
    qrect fr = qrect_get(p + 2);
    *w = fr.right - fr.left;
    *h = fr.bottom - fr.top;
    if (*w <= 0 || *h <= 0 || *w > 8192 || *h > 8192)
        return NULL;
    gaddr r = tb_NewPtr(8, false, true), out = tb_NewPtr(4, false, true), saved = tb_NewPtr(8, false, true);
    wr16(r + 4, (uint16_t)*h);
    wr16(r + 6, (uint16_t)*w);
    uint8_t *rgba = NULL;
    if (tb_NewGWorld(out, 32, r, 0, 0, 0) == noErr && rd32(out)) {
        gaddr gw = rd32(out);
        tb_GetGWorld(saved, saved + 4);
        tb_SetGWorld(gw, 0);
        gaddr pmh = tb_GetGWorldPixMap(gw);
        tb_LockPixels(pmh);
        tb_EraseRect(r);
        tb_DrawPicture(pic, r);
        gaddr pm = rd32(pmh);
        gaddr base = rd32(pm);
        uint32_t rb = rd16(pm + 4) & 0x3FFF;
        rgba = malloc((size_t)*w * (size_t)*h * 4);
        for (int y = 0; rgba && y < *h; y++)
            for (int x = 0; x < *w; x++) {
                uint32_t v = rd32(base + (uint32_t)y * rb + (uint32_t)x * 4);
                uint8_t *o = rgba + ((size_t)y * (size_t)*w + (size_t)x) * 4;
                o[0] = (uint8_t)(v >> 16), o[1] = (uint8_t)(v >> 8), o[2] = (uint8_t)v, o[3] = 255;
            }
        tb_SetGWorld(rd32(saved), rd32(saved + 4));
        tb_DisposeGWorld(gw);
    }
    tb_DisposePtr(r);
    tb_DisposePtr(out);
    tb_DisposePtr(saved);
    return rgba;
}

static gaddr picture_handle(const uint8_t *b, size_t n)
{
    gaddr h = tb_NewHandle((int32_t)n, false, false);
    if (h)
        gput(rd32(h), b, (uint32_t)n);
    return h;
}

/* The frames' duration in a media of scale 600. */
static int32_t frame_duration(const importer *im, const dest *d, int32_t fallback)
{
    if (im && im->sample_duration > 0 && im->sample_scale > 0)
        return (int32_t)((int64_t)im->sample_duration * 600 / im->sample_scale);
    if (im && im->duration > 0) {
        int32_t ms = tb_GetMovieTimeScale((int32_t)d->movie);
        return ms > 0 ? (int32_t)((int64_t)im->duration * 600 / ms) : im->duration;
    }
    return fallback;
}

/* Frames of 32-bit pixels ('raw '), one sample each. */
static int16_t import_frames(dest *d, const importer *im, uint8_t **frames, int nframes, int w, int h, int32_t dur)
{
    size_t fb = (size_t)w * (size_t)h * 4, total = fb * (size_t)nframes;
    uint8_t *all = malloc(total ? total : 1);
    if (!all)
        return memFullErr;
    for (int f = 0; f < nframes; f++)
        for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
            const uint8_t *s = frames[f] + i * 4;
            uint8_t *o = all + fb * (size_t)f + i * 4;
            o[0] = 0xFF, o[1] = s[0], o[2] = s[1], o[3] = s[2];
        }
    source none = { 0 };
    uint32_t dtype;
    gaddr dref = dref_for(&none, false, all, total, &dtype);
    free(all);
    int32_t fw = im && im->width ? im->width : w << 16, fh = im && im->height ? im->height : h << 16;
    gaddr media;
    gaddr track = track_for(d, fourcc("vide"), fw, fh, 0, 600, dref, dtype, &media);
    if (dref)
        tb_DisposeHandle(dref);
    if (!track)
        return d->flags & movieImportMustUseTrack ? mediaTypesDontMatch : tb_GetMoviesError();
    qtm_media *md = qt_media_model(media, NULL, NULL);
    int desc = image_desc(media, fourcc("raw "), w, h, 32, 0, "None");
    int64_t start = qtm_media_samples_duration(md);
    qt_src hs;
    uint64_t base = 0;
    const qtm_dref *r = &md->drefs[md->default_dref - 1];
    if (qt_src_open(r->type, r->flags, r->data, r->len, NULL, false, &hs)) {
        base = (uint64_t)qt_src_size(&hs) - total;
        qt_src_close(&hs);
    }
    for (int f = 0; f < nframes; f++)
        qtm_add_samples(md, base + fb * (size_t)f, fb, 1, (uint32_t)fb, (uint32_t)dur, (uint32_t)desc, true, true);
    return insert(d, track, media, start);
}

static int16_t import_jpeg(dest *d, const importer *im, const source *src)
{
    uint8_t *rgba;
    int w, h;
    if (!qt_decode_image(src->b, src->n, &rgba, &w, &h))
        return noMovieFound;
    free(rgba);
    uint32_t dtype;
    gaddr dref = dref_for(src, true, src->b, src->n, &dtype);
    int32_t fw = im && im->width ? im->width : w << 16, fh = im && im->height ? im->height : h << 16;
    gaddr media;
    gaddr track = track_for(d, fourcc("vide"), fw, fh, 0, 600, dref, dtype, &media);
    if (dref)
        tb_DisposeHandle(dref);
    if (!track)
        return d->flags & movieImportMustUseTrack ? mediaTypesDontMatch : tb_GetMoviesError();
    qtm_media *md = qt_media_model(media, NULL, NULL);
    int desc = image_desc(media, fourcc("jpeg"), w, h, 24, (uint32_t)src->n, "Photo - JPEG");
    int64_t start = qtm_media_samples_duration(md);
    uint64_t base = 0;
    if (!src->file) {
        qt_src hs;
        const qtm_dref *r = &md->drefs[md->default_dref - 1];
        if (qt_src_open(r->type, r->flags, r->data, r->len, NULL, false, &hs)) {
            base = (uint64_t)qt_src_size(&hs) - src->n;
            qt_src_close(&hs);
        }
    }
    qtm_add_samples(md, base, src->n, 1, (uint32_t)src->n, (uint32_t)frame_duration(im, d, 600), (uint32_t)desc,
                    true, true);
    return insert(d, track, media, start);
}

static int16_t import_image(dest *d, const importer *im, const source *src, int kind)
{
    uint8_t *rgba = NULL;
    int w = 0, h = 0;
    if (kind == F_PICT) {
        size_t skip = src->n > 512 + 12 && sniff(src->b + 512, src->n - 512, 0) == F_PICT && src->file ? 512 : 0;
        if (skip == 0 && src->n > 524 && (!memcmp(src->b + 522, "\x11\x01", 2) || !memcmp(src->b + 522, "\x00\x11\x02\xFF", 4)))
            skip = 512;
        gaddr pic = picture_handle(src->b + skip, src->n - skip);
        if (pic) {
            rgba = picture_rgba(pic, &w, &h);
            tb_DisposeHandle(pic);
        }
    } else {
        qt_decode_image(src->b, src->n, &rgba, &w, &h);
    }
    if (!rgba)
        return noMovieFound;
    int16_t err = import_frames(d, im, &rgba, 1, w, h, frame_duration(im, d, 600));
    free(rgba);
    return err;
}

/* PICS: the resource fork's pictures in order of their IDs, a frame each. */
static int16_t import_pics(dest *d, const importer *im, const source *src)
{
    if (!src->spec)
        return noMovieFound;
    int16_t saved = tb_CurResFile();
    int16_t rn = tb_FSpOpenResFile(src->spec, 1);
    if (rn < 0)
        return noMovieFound;
    tb_UseResFile(rn);
    int count = tb_Count1Resources((int32_t)fourcc("PICT"));
    uint8_t **frames = calloc((size_t)(count > 0 ? count : 1), sizeof *frames);
    int16_t *ids = calloc((size_t)(count > 0 ? count : 1), sizeof *ids);
    int n = 0, w = 0, h = 0;
    for (int i = 1; i <= count; i++) {
        gaddr pic = tb_Get1IndResource((int32_t)fourcc("PICT"), (int16_t)i);
        if (!pic)
            continue;
        gaddr idp = tb_NewPtr(8, false, true);
        tb_GetResInfo(pic, idp, idp + 2, 0);
        int16_t id = (int16_t)rd16(idp);
        tb_DisposePtr(idp);
        int fw, fh;
        uint8_t *rgba = picture_rgba(pic, &fw, &fh);
        if (!rgba || (n && (fw != w || fh != h))) {
            free(rgba);
            continue;
        }
        w = fw, h = fh;
        int k = n++;
        while (k > 0 && ids[k - 1] > id) { /* in ID order */
            ids[k] = ids[k - 1];
            frames[k] = frames[k - 1];
            k--;
        }
        ids[k] = id;
        frames[k] = rgba;
    }
    tb_CloseResFile(rn);
    tb_UseResFile(saved);
    int16_t err = n ? import_frames(d, im, frames, n, w, h, frame_duration(im, d, 40)) : noMovieFound;
    for (int i = 0; i < n; i++)
        free(frames[i]);
    free(frames);
    free(ids);
    return err;
}

/* ---- MIDI files: a music track ---- */

typedef struct {
    uint64_t tick;
    uint8_t st, d1, d2;
    uint32_t order;
    uint32_t tempo; /* a tempo event's microseconds per quarter note */
} smf_event;

typedef struct {
    uint32_t *w;
    size_t n, cap;
} words;

static void word(words *v, uint32_t x)
{
    if (v->n == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 256;
        uint32_t *nw = realloc(v->w, cap * sizeof *nw);
        if (!nw)
            return;
        v->w = nw;
        v->cap = cap;
    }
    v->w[v->n++] = x;
}

static uint32_t vlq(const uint8_t *b, size_t end, size_t *at)
{
    uint32_t v = 0;
    for (int i = 0; i < 4 && *at < end; i++) {
        uint8_t c = b[(*at)++];
        v = v << 7 | (c & 0x7F);
        if (!(c & 0x80))
            break;
    }
    return v;
}

static int cmp_event(const void *a, const void *b)
{
    const smf_event *x = a, *y = b;
    if (x->tick != y->tick)
        return x->tick < y->tick ? -1 : 1;
    return x->order < y->order ? -1 : x->order > y->order;
}

/* A general event: the note request naming a part's instrument. */
static void note_request(words *v, int part, int gm)
{
    uint8_t nr[84] = { 0 };
    nr[0] = 0;
    put16(nr + 2, 2);          /* polyphony */
    put32b(nr + 4, 0x00010000); /* typical polyphony */
    put32b(nr + 8, 0);          /* synthesizer type: any */
    put32b(nr + 76, 0);
    put32b(nr + 80, (uint32_t)gm);
    word(v, 0xF0000000u | (uint32_t)part << 16 | 23);
    for (int i = 0; i < 84; i += 4)
        word(v, be32(nr + i));
    word(v, 0xC0000000u | 1u << 16 | 23); /* kGeneralEventNoteRequest */
}

static void rest(words *v, uint64_t units)
{
    while (units) {
        uint32_t r = units > 0xFFFFFF ? 0xFFFFFF : (uint32_t)units;
        word(v, r);
        units -= r;
    }
}

static int16_t import_midi(dest *d, const source *src)
{
    const uint8_t *b = src->b;
    size_t n = src->n;
    if (n < 14 || memcmp(b, "MThd", 4))
        return noMovieFound;
    uint32_t hl = be32(b + 4);
    int ntracks = be16(b + 10);
    int division = (int16_t)be16(b + 12);
    if (division <= 0)
        division = 96; /* SMPTE time: taken as ticks of a 120 bpm quarter */
    smf_event *ev = NULL;
    size_t nev = 0, cap = 0;
    size_t at = 8 + hl;
    uint32_t order = 0;
    for (int t = 0; t < ntracks && at + 8 <= n; t++) {
        uint32_t len = be32(b + at + 4);
        if (memcmp(b + at, "MTrk", 4)) {
            at += 8 + len;
            continue;
        }
        size_t p = at + 8, end = at + 8 + len < n ? at + 8 + len : n;
        uint64_t tick = 0;
        uint8_t running = 0;
        while (p < end) {
            tick += vlq(b, end, &p);
            if (p >= end)
                break;
            uint8_t st = b[p];
            if (st & 0x80)
                p++;
            else
                st = running;
            if (st == 0xFF) {
                if (p >= end)
                    break;
                uint8_t type = b[p++];
                uint32_t ml = vlq(b, end, &p);
                if (type == 0x51 && ml == 3 && p + 3 <= end) {
                    if (nev == cap) {
                        cap = cap ? cap * 2 : 1024;
                        ev = realloc(ev, cap * sizeof *ev);
                    }
                    ev[nev++] = (smf_event){ tick, 0xFF, 0, 0, order++,
                                             (uint32_t)b[p] << 16 | (uint32_t)b[p + 1] << 8 | b[p + 2] };
                }
                if (type == 0x2F)
                    break;
                p += ml;
                continue;
            }
            if (st == 0xF0 || st == 0xF7) {
                p += vlq(b, end, &p);
                continue;
            }
            if (!(st & 0x80))
                break;
            running = st;
            int nd = (st & 0xF0) == 0xC0 || (st & 0xF0) == 0xD0 ? 1 : 2;
            if (p + (size_t)nd > end)
                break;
            uint8_t d1 = b[p], d2 = nd > 1 ? b[p + 1] : 0;
            p += (size_t)nd;
            if (nev == cap) {
                cap = cap ? cap * 2 : 1024;
                ev = realloc(ev, cap * sizeof *ev);
            }
            ev[nev++] = (smf_event){ tick, st, d1, d2, order++, 0 };
        }
        at += 8 + len;
    }
    if (!nev) {
        free(ev);
        return noMovieFound;
    }
    qsort(ev, nev, sizeof *ev, cmp_event);
    /* ticks to media time (600 per second) through the tempo map */
    const int scale = 600;
    uint64_t *when = malloc(nev * sizeof *when);
    double us_per_tick = 500000.0 / division, now_us = 0;
    uint64_t last_tick = 0;
    for (size_t i = 0; i < nev; i++) {
        now_us += (double)(ev[i].tick - last_tick) * us_per_tick;
        last_tick = ev[i].tick;
        when[i] = (uint64_t)llround(now_us * scale / 1e6);
        if (ev[i].st == 0xFF && ev[i].tempo)
            us_per_tick = (double)ev[i].tempo / division;
    }
    /* parts: one per channel used, its first program */
    int program[16], used[16] = { 0 };
    for (int c = 0; c < 16; c++)
        program[c] = -1;
    for (size_t i = 0; i < nev; i++) {
        int c = ev[i].st & 15;
        if ((ev[i].st & 0xF0) == 0x90 || (ev[i].st & 0xF0) == 0xC0)
            used[c] = 1;
        if ((ev[i].st & 0xF0) == 0xC0 && program[c] < 0)
            program[c] = ev[i].d1;
    }
    words header = { 0 }, body = { 0 };
    for (int c = 0; c < 16; c++)
        if (used[c])
            note_request(&header, c + 1, c == 9 ? 0x4001 : (program[c] < 0 ? 0 : program[c]) + 1);
    word(&header, 0x60000000); /* the end marker */
    /* notes with their durations: each note on matched with its off */
    uint64_t at_time = 0;
    bool seen_program[16] = { false };
    for (size_t i = 0; i < nev; i++) {
        int kind = ev[i].st & 0xF0, c = ev[i].st & 15;
        if (ev[i].st == 0xFF)
            continue;
        if (when[i] > at_time) {
            rest(&body, when[i] - at_time);
            at_time = when[i];
        }
        uint32_t part = (uint32_t)c + 1;
        if (kind == 0x90 && ev[i].d2) {
            uint64_t dur = 0;
            for (size_t j = i + 1; j < nev; j++)
                if (ev[j].st != 0xFF && (ev[j].st & 15) == c && ev[j].d1 == ev[i].d1 &&
                    ((ev[j].st & 0xF0) == 0x80 || ((ev[j].st & 0xF0) == 0x90 && !ev[j].d2))) {
                    dur = when[j] - when[i];
                    break;
                }
            if (!dur)
                dur = 1;
            if (dur > 0x3FFFFF)
                dur = 0x3FFFFF;
            word(&body, 0x90000000u | part << 16 | ev[i].d1);
            word(&body, 0x80000000u | (uint32_t)(ev[i].d2 & 0x7F) << 22 | (uint32_t)dur);
        } else if (kind == 0xB0) {
            word(&body, 0xA0000000u | part << 16 | (uint32_t)ev[i].d2 << 8);
            word(&body, 0x80000000u | ev[i].d1);
        } else if (kind == 0xE0) {
            int bend = (ev[i].d2 << 7 | ev[i].d1) - 8192, semis = bend * 512 / 8192; /* 8.8, a range of two */
            word(&body, 0xA0000000u | part << 16 | (uint16_t)semis);
            word(&body, 0x80000000u | 32);
        } else if (kind == 0xC0) {
            if (seen_program[c] || ev[i].d1 != program[c])
                note_request(&body, (int)part, c == 9 ? 0x4001 + ev[i].d1 : ev[i].d1 + 1);
            seen_program[c] = true;
        }
    }
    /* the notes still sounding at the end */
    uint64_t end_time = at_time;
    for (size_t i = 0; i < nev; i++)
        if (when[i] > end_time)
            end_time = when[i];
    rest(&body, end_time - at_time + 1);
    word(&body, 0x60000000);
    free(when);
    free(ev);
    /* the description: size, 'musi', reserved, data reference, flags, header */
    uint32_t dlen = 20 + (uint32_t)header.n * 4;
    uint8_t *desc = calloc(1, dlen);
    put32b(desc, dlen);
    put32b(desc + 4, fourcc("musi"));
    for (size_t i = 0; i < header.n; i++)
        put32b(desc + 20 + i * 4, header.w[i]);
    size_t sl = body.n * 4;
    uint8_t *sample = malloc(sl ? sl : 1);
    for (size_t i = 0; i < body.n; i++)
        put32b(sample + i * 4, body.w[i]);
    free(header.w);
    free(body.w);
    uint32_t dtype;
    source none = { 0 };
    gaddr dref = dref_for(&none, false, sample, sl, &dtype);
    gaddr media;
    gaddr track = track_for(d, fourcc("musi"), 0, 0, 0x100, scale, dref, dtype, &media);
    if (dref)
        tb_DisposeHandle(dref);
    int16_t err;
    if (!track) {
        err = d->flags & movieImportMustUseTrack ? mediaTypesDontMatch : tb_GetMoviesError();
    } else {
        qtm_media *md = qt_media_model(media, NULL, NULL);
        int di = add_desc(media, desc, dlen);
        int64_t start = qtm_media_samples_duration(md);
        qt_src hs;
        uint64_t base = 0;
        const qtm_dref *r = &md->drefs[md->default_dref - 1];
        if (qt_src_open(r->type, r->flags, r->data, r->len, NULL, false, &hs)) {
            base = (uint64_t)qt_src_size(&hs) - sl;
            qt_src_close(&hs);
        }
        qtm_add_samples(md, base, sl, 1, (uint32_t)sl, (uint32_t)(end_time + 1), (uint32_t)di, true, true);
        err = insert(d, track, media, start);
    }
    free(desc);
    free(sample);
    return err;
}

/* ---- Text ---- */

typedef struct {
    char *text;
    size_t len;
    int64_t time; /* -1: after the last */
} text_line;

/* QTtext time stamps: [hh:mm:ss.fraction], the fraction in the time scale. */
static int64_t stamp(const char *s, size_t n, int32_t scale)
{
    int h = 0, m = 0, sec = 0, frac = 0;
    char buf[64];
    if (n >= sizeof buf)
        return -1;
    memcpy(buf, s, n);
    buf[n] = 0;
    int got = sscanf(buf, "[%d:%d:%d.%d]", &h, &m, &sec, &frac);
    if (got < 3)
        return -1;
    return ((int64_t)h * 3600 + m * 60 + sec) * scale + (got == 4 ? frac : 0);
}

static int16_t import_text(dest *d, const importer *im, const source *src)
{
    /* lines (CR, LF or both), QTtext's {descriptors} and [time stamps] */
    const char *b = (const char *)src->b;
    size_t n = src->n;
    bool qttext = n >= 8 && !memcmp(b, "{QTtext}", 8);
    int32_t scale = 600;
    text_line *lines = NULL;
    size_t nl = 0, cap = 0;
    int64_t pending = -1;
    char *acc = NULL;
    size_t acc_len = 0;
    for (size_t i = 0; i <= n;) {
        size_t e = i;
        while (e < n && b[e] != '\r' && b[e] != '\n')
            e++;
        const char *line = b + i;
        size_t len = e - i;
        size_t next = e < n && b[e] == '\r' && e + 1 < n && b[e + 1] == '\n' ? e + 2 : e + 1;
        if (qttext && len && line[0] == '{') {
            const char *ts = strstr(line, "{timeScale:");
            if (ts && ts < line + len)
                scale = atoi(ts + 11) > 0 ? atoi(ts + 11) : 600;
        } else if (qttext && len && line[0] == '[') {
            int64_t t = stamp(line, len, scale);
            if (t >= 0) {
                if (acc || pending >= 0) {
                    if (nl == cap) {
                        cap = cap ? cap * 2 : 64;
                        lines = realloc(lines, cap * sizeof *lines);
                    }
                    lines[nl++] = (text_line){ acc, acc_len, pending };
                    acc = NULL, acc_len = 0;
                }
                pending = t;
            }
        } else if (qttext) {
            if (!len && !acc) /* blank lines between stamps */
                goto next_line;
            acc = realloc(acc, acc_len + len + 2);
            if (acc_len)
                acc[acc_len++] = '\r';
            memcpy(acc + acc_len, line, len);
            acc_len += len;
        } else if (len) {
            if (nl == cap) {
                cap = cap ? cap * 2 : 64;
                lines = realloc(lines, cap * sizeof *lines);
            }
            char *t = malloc(len);
            memcpy(t, line, len);
            lines[nl++] = (text_line){ t, len, -1 };
        }
    next_line:
        if (e >= n)
            break;
        i = next;
    }
    if (qttext && (acc || pending >= 0)) {
        if (nl == cap) {
            cap = cap ? cap * 2 : 64;
            lines = realloc(lines, cap * sizeof *lines);
        }
        lines[nl++] = (text_line){ acc, acc_len, pending };
        acc = NULL;
    }
    free(acc);
    if (!nl) {
        free(lines);
        return noMovieFound;
    }
    int32_t each = frame_duration(im, d, 0);
    gaddr media;
    int32_t w = im && im->width ? im->width : 200 << 16, h = im && im->height ? im->height : 48 << 16;
    gaddr track = track_for(d, fourcc("text"), w, h, 0, scale, 0, 0, &media);
    int16_t err = noErr;
    if (!track) {
        err = d->flags & movieImportMustUseTrack ? mediaTypesDontMatch : tb_GetMoviesError();
    } else {
        qtm_media *md = qt_media_model(media, NULL, NULL);
        int64_t start = qtm_media_samples_duration(md);
        uint32_t mh = (uint32_t)tb_GetMediaHandler((int32_t)media);
        gaddr box = tb_NewPtr(8, false, true), buf = tb_NewPtr(4, false, true);
        wr16(box + 4, (uint16_t)(h >> 16));
        wr16(box + 6, (uint16_t)(w >> 16));
        int64_t at = 0;
        for (size_t i = 0; i < nl && !err; i++) {
            int64_t t = lines[i].time >= 0 ? lines[i].time : at;
            bool end = !lines[i].text && i + 1 == nl; /* a last time stamp: where the text ends */
            if (end) {
                if (t > at) {
                    uint32_t a0[] = { 0, 0, 0, 12, 0, 0, 0, 0, box, 0, 0, 0, 0, 0, (uint32_t)(t - at), buf };
                    qtc_call_named(mh, "TextMediaAddTextSample", a0);
                }
                break;
            }
            int64_t next = i + 1 < nl && lines[i + 1].time >= 0 ? lines[i + 1].time
                           : each > 0 ? t + (int64_t)each * scale / 600 : t + 2 * (int64_t)scale;
            if (next <= t)
                next = t + 1;
            if (t > at) { /* a gap: an empty sample */
                uint32_t a0[] = { 0, 0, 0, 12, 0, 0, 0, 0, box, 0, 0, 0, 0, 0, (uint32_t)(t - at), buf };
                qtc_call_named(mh, "TextMediaAddTextSample", a0);
            }
            gaddr p = tb_NewPtr((int32_t)(lines[i].len ? lines[i].len : 1), false, false);
            gput(p, lines[i].text, (uint32_t)lines[i].len);
            /* TextMediaAddTextSample(mh, text, size, font, size, face, textColor, backColor, justification,
             * textBox, displayFlags, scrollDelay, hiliteStart, hiliteEnd, hiliteColor, duration, sampleTime) */
            uint32_t a[] = { p, (uint32_t)lines[i].len, 0, 12, 0, 0, 0, 0, box, 0, 0, 0, 0, 0,
                             (uint32_t)(next - t), buf };
            int32_t r = qtc_call_named(mh, "TextMediaAddTextSample", a);
            if (r)
                err = (int16_t)r;
            tb_DisposePtr(p);
            at = next;
        }
        tb_DisposePtr(box);
        tb_DisposePtr(buf);
        if (!err)
            err = insert(d, track, media, start);
    }
    for (size_t i = 0; i < nl; i++)
        free(lines[i].text);
    free(lines);
    return err;
}

/* ---- The import ---- */

static int16_t import_source(importer *im, source *src, int kind, dest *d)
{
    apply_limits(im, src);
    if (kind == F_NONE)
        kind = sniff(src->b, src->n, src->type);
    sound s = { 0 };
    int16_t err = noMovieFound;
    switch (kind) {
    case F_AIFF:
        if (src->n >= 12 && parse_aiff(src->b, src->n, &s))
            err = import_sound(d, src, &s);
        break;
    case F_WAVE:
        if (src->n >= 12 && parse_wave(src->b, src->n, &s))
            err = import_sound(d, src, &s);
        break;
    case F_AU:
        if (src->n >= 24 && parse_au(src->b, src->n, &s))
            err = import_sound(d, src, &s);
        break;
    case F_SND:
        if (src->file && src->spec) { /* a sound file: its first 'snd ' resource */
            int16_t saved = tb_CurResFile(), rn = tb_FSpOpenResFile(src->spec, 1);
            if (rn >= 0) {
                tb_UseResFile(rn);
                gaddr h = tb_Get1IndResource((int32_t)fourcc("snd "), 1);
                source hs;
                if (h && source_handle(&hs, h, fourcc("snd "))) {
                    if (parse_snd(hs.b, hs.n, &s))
                        err = import_sound(d, &hs, &s);
                    source_free(&hs, false);
                }
                tb_CloseResFile(rn);
                tb_UseResFile(saved);
            }
        } else if (parse_snd(src->b, src->n, &s)) {
            err = import_sound(d, src, &s);
        }
        break;
    case F_MIDI: err = import_midi(d, src); break;
    case F_JPEG: err = import_jpeg(d, im, src); break;
    case F_PICT:
    case F_IMAGE: err = import_image(d, im, src, kind); break;
    case F_PICS: err = import_pics(d, im, src); break;
    case F_TEXT: err = import_text(d, im, src); break;
    }
    free(s.pcm);
    return err;
}

/* The kind a component of ours imports, from its subtype. */
static int kind_of(uint32_t subtype)
{
    static const struct {
        const char *t;
        int kind;
    } map[] = {
        { "AIFF", F_AIFF }, { "AIFC", F_AIFF }, { "AIF ", F_AIFF }, { "WAVE", F_WAVE }, { "WAV ", F_WAVE },
        { "sfil", F_SND }, { "snd ", F_SND }, { "ULAW", F_AU }, { "AU  ", F_AU }, { "Midi", F_MIDI },
        { "MID ", F_MIDI }, { "PICT", F_PICT }, { "PCT ", F_PICT }, { "PICS", F_PICS }, { "TEXT", F_TEXT },
        { "TXT ", F_TEXT }, { "JPEG", F_JPEG }, { "JPG ", F_JPEG }, { "GIFf", F_IMAGE }, { "GIF ", F_IMAGE },
        { "PNGf", F_IMAGE }, { "PNG ", F_IMAGE }, { "TIFF", F_IMAGE }, { "TIF ", F_IMAGE },
        { "BMPf", F_IMAGE }, { "BMP ", F_IMAGE }, { "8BPS", F_IMAGE }, { "PSD ", F_IMAGE },
        { "qtif", F_IMAGE }, { "TPIC", F_IMAGE }, { "TGA ", F_IMAGE }, { ".SGI", F_IMAGE },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (fourcc(map[i].t) == subtype)
            return map[i].kind;
    return F_NONE;
}

static int32_t finish(int16_t err, dest *d, gaddr used, gaddr added, gaddr out_flags)
{
    if (used)
        wr32(used, err ? 0 : d->used_track);
    if (added)
        wr32(added, err ? 0 : (uint32_t)d->added);
    if (out_flags)
        wr32(out_flags, err ? 0 : (d->ntracks > 1 ? movieImportResultUsedMultipleTracks : 0));
    return err;
}

static bool validate(const source *src, int kind)
{
    int k = sniff(src->b, src->n, src->type);
    if (kind == F_TEXT) /* any text */
        return k == F_NONE || k == F_TEXT;
    return k == kind;
}

static int32_t eat_call(qtc_call *k)
{
    importer *im = k->self;
    const uint32_t *a = k->a;
    int kind = kind_of(k->cls->subtype);
    if (!k->call) {
        if (k->what == kQTCClose) {
            if (im->desc)
                tb_DisposeHandle(im->desc);
            if (im->aux)
                tb_DisposeHandle(im->aux);
        }
        return qtcBadSelector;
    }
    const char *name = k->call->name + 11; /* after "MovieImport" */
    if (!strcmp(name, "File") || !strcmp(name, "Handle") || !strcmp(name, "DataRef")) {
        bool ref = !strcmp(name, "DataRef");
        int o = ref ? 1 : 0;
        dest d = { .movie = a[1 + o], .target = a[2 + o], .at = (int32_t)a[4 + o], .flags = (int32_t)a[6 + o] };
        source src;
        bool ok = !strcmp(name, "File") ? source_spec(&src, a[0])
                  : ref                  ? source_dataref(&src, a[0], a[1])
                                         : source_handle(&src, a[0], im->desc_type ? im->desc_type : k->cls->subtype);
        if (!d.movie || !qt_movie_model(d.movie))
            return finish(invalidMovie, &d, a[3 + o], a[5 + o], a[7 + o]);
        if (!ok)
            return finish(ref ? couldNotResolveDataRef : !strcmp(name, "File") ? fnfErr : paramErr, &d, a[3 + o],
                          a[5 + o], a[7 + o]);
        int16_t err = import_source(im, &src, kind, &d);
        source_free(&src, ref);
        return finish(err, &d, a[3 + o], a[5 + o], a[7 + o]);
    }
    if (!strcmp(name, "Validate") || !strcmp(name, "ValidateDataRef")) {
        source src;
        bool ok = !strcmp(name, "Validate") ? (a[0] ? source_spec(&src, a[0]) : source_handle(&src, a[1], k->cls->subtype))
                                           : source_dataref(&src, a[0], a[1]);
        bool valid = ok && validate(&src, kind);
        if (ok)
            source_free(&src, !strcmp(name, "ValidateDataRef"));
        if (a[2])
            wr8(a[2], !strcmp(name, "Validate") ? valid : valid ? 128 : 0);
        return noErr;
    }
    if (!strcmp(name, "SetSampleDuration")) {
        im->sample_duration = (int32_t)a[0];
        im->sample_scale = (int32_t)a[1];
        return noErr;
    }
    if (!strcmp(name, "SetSampleDescription")) {
        if (im->desc)
            tb_DisposeHandle(im->desc);
        im->desc = 0;
        if (a[0]) {
            im->desc = a[0];
            tb_HandToHand(&im->desc);
        }
        im->desc_type = a[1];
        return noErr;
    }
    if (!strcmp(name, "GetSampleDescription")) {
        if (a[0])
            wr32(a[0], im->desc);
        if (a[1])
            wr32(a[1], im->desc_type);
        return noErr;
    }
    if (!strcmp(name, "SetMediaFile")) {
        im->media_file = a[0];
        return noErr;
    }
    if (!strcmp(name, "SetDimensions")) {
        im->width = (int32_t)a[0];
        im->height = (int32_t)a[1];
        return noErr;
    }
    if (!strcmp(name, "SetChunkSize")) {
        im->chunk_size = (int32_t)a[0];
        return noErr;
    }
    if (!strcmp(name, "SetProgressProc")) {
        im->progress_proc = a[0];
        im->progress_refcon = (int32_t)a[1];
        return noErr;
    }
    if (!strcmp(name, "SetAuxiliaryData")) {
        if (im->aux)
            tb_DisposeHandle(im->aux);
        im->aux = a[0];
        if (im->aux)
            tb_HandToHand(&im->aux);
        im->aux_type = a[1];
        return noErr;
    }
    if (!strcmp(name, "SetFromScrap")) {
        im->from_scrap = a[0] & 0xFF;
        return noErr;
    }
    if (!strcmp(name, "DoUserDialog")) {
        /* our importers have no settings to ask for */
        if (a[2])
            wr8(a[2], 0);
        return noErr;
    }
    if (!strcmp(name, "SetDuration")) {
        im->duration = (int32_t)a[0];
        return noErr;
    }
    if (!strcmp(name, "GetAuxiliaryDataType")) {
        if (a[0])
            wr32(a[0], im->aux_type);
        return noErr;
    }
    if (!strcmp(name, "GetFileType")) {
        static const char *types[] = { "", "AIFF", "WAVE", "sfil", "ULAW", "Midi", "PICT", "PICS", "JPEG", "", "TEXT" };
        if (a[0])
            wr32(a[0], *types[kind] ? fourcc(types[kind]) : k->cls->subtype);
        return noErr;
    }
    if (!strcmp(name, "SetOffsetAndLimit")) {
        im->offset = a[0];
        im->limit = a[1];
        return noErr;
    }
    if (!strcmp(name, "SetOffsetAndLimit64")) {
        im->offset = a[0] ? (uint64_t)rd32(a[0]) << 32 | rd32(a[0] + 4) : 0;
        im->limit = a[1] ? (uint64_t)rd32(a[1]) << 32 | rd32(a[1] + 4) : 0;
        return noErr;
    }
    if (!strcmp(name, "GetMIMETypeList")) {
        static const char *mime[] = { NULL, "audio/aiff", "audio/wav", NULL, "audio/basic", "audio/midi", "image/x-pict",
                                      NULL, "image/jpeg", NULL, "text/plain" };
        if (!a[0])
            return paramErr;
        wr32(a[0], 0);
        if (!mime[kind])
            return noErr;
        gaddr c = qt_atoms_new();
        if (c) {
            const char *m = mime[kind];
            int16_t e;
            qt_atoms_insert(c, 0, fourcc("mime"), 1, 0, m, (uint32_t)strlen(m), &e);
            wr32(a[0], c);
        }
        return noErr;
    }
    if (!strcmp(name, "GetSettingsAsAtomContainer")) {
        if (!a[0])
            return paramErr;
        wr32(a[0], qt_atoms_new());
        return noErr;
    }
    if (!strcmp(name, "SetSettingsFromAtomContainer"))
        return noErr;
    if (!strcmp(name, "Idle")) {
        if (a[1])
            wr32(a[1], movieImportResultComplete);
        return noErr;
    }
    if (!strcmp(name, "GetLoadState")) {
        if (a[0])
            wr32(a[0], kMovieLoadStateComplete);
        return noErr;
    }
    if (!strcmp(name, "GetMaxLoadedTime")) {
        if (a[0])
            wr32(a[0], 0x7FFFFFFF);
        return noErr;
    }
    if (!strcmp(name, "EstimateCompletionTime")) {
        if (a[0]) {
            wr32(a[0], 0);
            wr32(a[0] + 4, 0);
            wr32(a[0] + 8, 600);
            wr32(a[0] + 12, 0);
        }
        return noErr;
    }
    if (!strcmp(name, "SetDontBlock")) {
        im->dont_block = a[0] & 0xFF;
        return noErr;
    }
    if (!strcmp(name, "GetDontBlock")) {
        if (a[0])
            wr8(a[0], im->dont_block);
        return noErr;
    }
    return qtcBadSelector;
}

/* The importers, by subtype: files and handles of each kind; those named
 * by a file name extension say so. */
#define FILES (canImportFiles | canImportHandles | canValidateFile | canValidateHandles | canImportDataRefs | canValidateDataRefs)
#define EAT(sub, flags, name) \
    { 0x65617420, sub, 0x6170706C, flags, name, NULL, 0x00030000, { "MovieImport" }, sizeof(importer), eat_call, 0 }
static qtc_class importers[] = {
    EAT(0x41494646, FILES | canImportInPlace | hasMIMEList, "AIFF"),            /* AIFF */
    EAT(0x41494643, FILES | canImportInPlace, "AIFC"),                          /* AIFC */
    EAT(0x57415645, FILES | canImportInPlace | hasMIMEList, "WAVE"),            /* WAVE */
    EAT(0x7366696C, canImportFiles | canValidateFile | canImportInPlace, "System 7 Sound"), /* sfil */
    EAT(0x736E6420, canImportHandles | canValidateHandles, "Sound"),           /* 'snd ' */
    EAT(0x554C4157, FILES | canImportInPlace | hasMIMEList, "\xB5Law"),        /* ULAW */
    EAT(0x4D696469, FILES | hasMIMEList, "MIDI"),                               /* Midi */
    EAT(0x50494354, FILES | hasMIMEList, "Picture"),                            /* PICT */
    EAT(0x50494353, canImportFiles | canValidateFile, "PICS"),                  /* PICS */
    EAT(0x54455854, FILES | hasMIMEList, "Text"),                               /* TEXT */
    EAT(0x4A504547, FILES | canImportInPlace | hasMIMEList, "JPEG"),            /* JPEG */
    EAT(0x47494666, FILES, "GIF"),                                              /* GIFf */
    EAT(0x504E4766, FILES, "PNG"),                                              /* PNGf */
    EAT(0x54494646, FILES, "TIFF"),                                             /* TIFF */
    EAT(0x424D5066, FILES, "BMP"),                                              /* BMPf */
    EAT(0x38425053, FILES, "Photoshop"),                                        /* 8BPS */
    EAT(0x71746966, FILES, "QuickTime Image"),                                  /* qtif */
    EAT(0x54504943, FILES, "Targa"),                                            /* TPIC */
    EAT(0x2E534749, FILES, "SGI"),                                              /* .SGI */
    EAT(0x41494620, FILES | subtypeIsExtension | canImportInPlace, "AIFF"),     /* AIF */
    EAT(0x57415620, FILES | subtypeIsExtension | canImportInPlace, "WAVE"),     /* WAV */
    EAT(0x41552020, FILES | subtypeIsExtension | canImportInPlace, "\xB5Law"),  /* AU */
    EAT(0x4D494420, FILES | subtypeIsExtension, "MIDI"),                        /* MID */
    EAT(0x50435420, FILES | subtypeIsExtension, "Picture"),                     /* PCT */
    EAT(0x54585420, FILES | subtypeIsExtension, "Text"),                        /* TXT */
    EAT(0x4A504720, FILES | subtypeIsExtension | canImportInPlace, "JPEG"),     /* JPG */
    EAT(0x47494620, FILES | subtypeIsExtension, "GIF"),                         /* GIF */
    EAT(0x504E4720, FILES | subtypeIsExtension, "PNG"),                         /* PNG */
    EAT(0x54494620, FILES | subtypeIsExtension, "TIFF"),                        /* TIF */
    EAT(0x424D5020, FILES | subtypeIsExtension, "BMP"),                         /* BMP */
    EAT(0x50534420, FILES | subtypeIsExtension, "Photoshop"),                   /* PSD */
    EAT(0x54474120, FILES | subtypeIsExtension, "Targa"),                       /* TGA */
};

void qt_import_register(void)
{
    for (size_t i = 0; i < sizeof importers / sizeof importers[0]; i++)
        qtc_register(&importers[i]);
}

/* NewMovieFromFile on a file that isn't a movie: a new movie of what it
 * holds, through the importer for it. */
int16_t qt_import_file_as_movie(const char *path, gaddr moviep, int16_t flags)
{
    wr32(moviep, 0);
    gaddr spec = tb_NewPtr(FSSpec_SIZEOF, false, true);
    fm_spec_for_path(path, spec);
    source src;
    if (!source_file(&src, path, spec)) {
        tb_DisposePtr(spec);
        return noMovieFound;
    }
    int kind = sniff(src.b, src.n, src.type);
    if (kind == F_NONE) {
        source_free(&src, true);
        return noMovieFound;
    }
    gaddr m = (gaddr)tb_NewMovie(flags);
    if (!m) {
        source_free(&src, true);
        return memFullErr;
    }
    dest d = { .movie = m, .flags = movieImportCreateTrack };
    importer im = { 0 };
    int16_t err = import_source(&im, &src, kind, &d);
    source_free(&src, true);
    if (err) {
        tb_DisposeMovie(m);
        return err;
    }
    if (flags & newMovieActive)
        tb_SetMovieActive(m, 1);
    wr32(moviep, m);
    return noErr;
}

int16_t qt_import_bytes(const uint8_t *bytes, size_t len, uint32_t type, gaddr movie, int32_t at, gaddr *used)
{
    source src = { 0 };
    src.b = malloc(len ? len : 1);
    memcpy(src.b, bytes, len);
    src.n = len;
    src.type = type;
    dest d = { .movie = movie, .at = at, .flags = movieImportCreateTrack };
    importer im = { 0 };
    int16_t err = import_source(&im, &src, F_NONE, &d);
    source_free(&src, false);
    if (used)
        *used = d.used_track;
    return err;
}
