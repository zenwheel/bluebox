/*
 * QuickTime's classic codecs: the table of video decoders, the standard
 * colours for palettized movies, and the sound formats.
 *
 * Host frameworks have dropped most of these codecs (macOS no longer
 * decodes Cinepak), so they are ours: Cinepak in qt_codec_cvid.c, Apple's
 * Animation, Graphics, Video and None in qt_codec_apple.c.  Photo-JPEG goes
 * to the host's image decoder, which reads baseline JPEG everywhere.  Sound tracks are
 * uncompressed, IMA 4:1 or MACE (through the MACE target).
 */
#include <stdlib.h>
#include <string.h>

#include "machine.h"
#include "mace_decoder.h"
#include "qd_internal.h"
#include "qt_codec.h"
#include "tb_internal.h"

/* ---- Video ---- */

/* Photo-JPEG ('jpeg'): each sample a JPEG image, through the host. */
static machine_image_fn image_fn;
static void *image_ctx;

void machine_set_image_decoder(machine_image_fn fn, void *ctx)
{
    image_fn = fn;
    image_ctx = ctx;
}

static bool jpeg_supports(int depth) { return image_fn && (depth == 24 || depth == 32 || depth == 40); }
static void *jpeg_open(const qt_video_format *f) { return (void *)1; }
static void jpeg_close(void *s) {}

static bool jpeg_decode(void *s, const qt_video_format *f, const uint32_t *palette, const uint8_t *data, size_t len,
                        uint8_t *frame)
{
    uint8_t *rgba = NULL;
    int w = 0, h = 0;
    if (!image_fn || !image_fn(data, len, &rgba, &w, &h, image_ctx) || !rgba)
        return false;
    for (int y = 0; y < f->height && y < h; y++)
        memcpy(frame + (size_t)y * (size_t)f->width * 4, rgba + (size_t)y * (size_t)w * 4,
               (size_t)(w < f->width ? w : f->width) * 4);
    free(rgba);
    return true;
}

static const qt_video_codec qt_codec_jpeg = { 0x6A706567, jpeg_supports, jpeg_open, jpeg_decode, jpeg_close };

static const qt_video_codec *const codecs[] = { &qt_codec_cvid, &qt_codec_rle, &qt_codec_smc, &qt_codec_rpza,
                                               &qt_codec_raw, &qt_codec_jpeg };

struct qt_vdec {
    const qt_video_codec *codec;
    qt_video_format format;
    uint32_t palette[256];
    void *state;
};

static const qt_video_codec *codec_for(uint32_t format, int depth)
{
    for (size_t i = 0; i < sizeof codecs / sizeof *codecs; i++)
        if (codecs[i]->format == format && codecs[i]->supports(depth))
            return codecs[i];
    return NULL;
}

bool qt_vdec_supported(uint32_t format, int depth) { return codec_for(format, depth) != NULL; }

qt_vdec *qt_vdec_new(const qt_video_format *f)
{
    const qt_video_codec *c = codec_for(f->format, f->depth);
    if (!c || f->width <= 0 || f->height <= 0 || f->width > 4096 || f->height > 4096)
        return NULL;
    qt_vdec *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->codec = c;
    d->format = *f;
    int bits = f->depth > 32 ? f->depth - 32 : f->depth;
    if (bits <= 8) {
        const uint32_t *p = f->palette ? f->palette : qt_standard_palette(f->depth);
        if (p)
            memcpy(d->palette, p, sizeof(uint32_t) << bits);
    }
    d->format.palette = d->palette;
    d->state = c->open(&d->format);
    if (!d->state) {
        free(d);
        return NULL;
    }
    return d;
}

bool qt_vdec_decode(qt_vdec *d, const uint8_t *data, size_t len, uint8_t *frame)
{
    return d && d->codec->decode(d->state, &d->format, d->palette, data, len, frame);
}

void qt_vdec_free(qt_vdec *d)
{
    if (!d)
        return;
    d->codec->close(d->state);
    free(d);
}

/* The standard colour tables (QuickDraw's clut 1, 2, 4 and 8) and grey ramps. */
const uint32_t *qt_standard_palette(int depth)
{
    static uint32_t tables[41][256];
    static bool made[41];
    if (depth < 1 || depth > 40 || (depth > 8 && depth < 33) || (depth <= 8 && (depth & (depth - 1))))
        return NULL;
    if (!made[depth]) {
        int bits = depth > 32 ? depth - 32 : depth;
        int n = 1 << bits;
        for (int v = 0; v < n; v++) {
            if (depth > 32) { /* white to black */
                uint32_t g = (uint32_t)(255 - v * 255 / (n - 1));
                tables[depth][v] = g << 16 | g << 8 | g;
            } else {
                uint16_t c[3];
                qd_px_rgb(depth, 0, (uint32_t)v, c);
                tables[depth][v] = (uint32_t)(c[0] >> 8) << 16 | (uint32_t)(c[1] >> 8) << 8 | (c[2] >> 8);
            }
        }
        made[depth] = true;
    }
    return tables[depth];
}

/* ---- Sound ---- */

static bool is(uint32_t format, const char *s) { return format == fourcc(s); }

bool qt_adec_supported(const qt_sound_format *f)
{
    if (f->channels < 1 || f->channels > 2 || f->rate < 1000 || f->rate > 200000)
        return false;
    if (is(f->format, "raw ") || is(f->format, "twos") || is(f->format, "sowt") || is(f->format, "NONE"))
        return f->bits == 8 || f->bits == 16;
    return is(f->format, "ima4") || is(f->format, "MAC3") || is(f->format, "MAC6");
}

size_t qt_adec_bytes(const qt_sound_format *f, uint32_t frames)
{
    size_t ch = (size_t)f->channels;
    if (is(f->format, "ima4"))
        return (frames + 63) / 64 * 34 * ch;
    if (is(f->format, "MAC3"))
        return (frames + 5) / 6 * 2 * ch;
    if (is(f->format, "MAC6"))
        return (frames + 5) / 6 * ch;
    return (size_t)frames * ch * (size_t)(f->bits / 8);
}

/* IMA 4:1: packets of 34 bytes per channel, a 2-byte header (the
 * predictor's top 9 bits and the step index) then 64 four-bit samples, low
 * nibble first; stereo packets alternate.  Tables from the IMA standard. */
static const int16_t ima_steps[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};
static const int8_t ima_index[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

static uint32_t ima4(const uint8_t *data, size_t len, int nch, int16_t *pcm)
{
    size_t packets = len / (34 * (size_t)nch);
    for (size_t p = 0; p < packets; p++)
        for (int ch = 0; ch < nch; ch++) {
            const uint8_t *pk = data + (p * (size_t)nch + (size_t)ch) * 34;
            int pred = (int16_t)((pk[0] << 8 | pk[1]) & 0xFF80), index = pk[1] & 0x7F;
            if (index > 88)
                index = 88;
            for (int k = 0; k < 64; k++) {
                int nib = (pk[2 + k / 2] >> ((k & 1) * 4)) & 15;
                int step = ima_steps[index], diff = step >> 3;
                if (nib & 1)
                    diff += step >> 2;
                if (nib & 2)
                    diff += step >> 1;
                if (nib & 4)
                    diff += step;
                pred += (nib & 8) ? -diff : diff;
                pred = pred < -32768 ? -32768 : pred > 32767 ? 32767 : pred;
                index += ima_index[nib];
                index = index < 0 ? 0 : index > 88 ? 88 : index;
                pcm[(p * 64 + (size_t)k) * (size_t)nch + (size_t)ch] = (int16_t)pred;
            }
        }
    return (uint32_t)(packets * 64);
}

uint32_t qt_adec_decode(const qt_sound_format *f, const uint8_t *data, size_t len, int16_t **pcm)
{
    *pcm = NULL;
    if (!qt_adec_supported(f) || !len)
        return 0;
    size_t nch = (size_t)f->channels;
    size_t frames;
    if (is(f->format, "ima4"))
        frames = len / (34 * nch) * 64;
    else if (is(f->format, "MAC3"))
        frames = len / nch * 3;
    else if (is(f->format, "MAC6"))
        frames = len / nch * 6;
    else
        frames = len / (nch * (size_t)(f->bits / 8));
    if (!frames || frames > 0x1000000)
        return 0;
    int16_t *out = malloc(frames * nch * sizeof *out);
    if (!out)
        return 0;
    *pcm = out;
    if (is(f->format, "ima4"))
        return ima4(data, len, (int)nch, out);
    if (is(f->format, "MAC3") || is(f->format, "MAC6"))
        return (uint32_t)mace_decode(data, len, (int)nch, is(f->format, "MAC3"), out);
    size_t n = frames * nch;
    for (size_t i = 0; i < n; i++) {
        if (f->bits == 16)
            out[i] = is(f->format, "sowt") ? (int16_t)(data[i * 2] | data[i * 2 + 1] << 8)
                                           : (int16_t)(data[i * 2] << 8 | data[i * 2 + 1]);
        else if (is(f->format, "twos") || is(f->format, "sowt"))
            out[i] = (int16_t)((int8_t)data[i] * 256);
        else /* 'raw ': offset binary */
            out[i] = (int16_t)((data[i] - 128) * 256);
    }
    return (uint32_t)frames;
}
