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

#include "hostvideo.h"
#include "machine.h"
#include "mace_decoder.h"
#include "qd_internal.h"
#include "qt_codec.h"
#include "tb_internal.h"

/* ---- Video ---- */

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return be16(p) << 16 | be16(p + 2); }

/* Photo-JPEG ('jpeg'): each sample a JPEG image, through the host. */
static machine_image_fn image_fn;
static void *image_ctx;

void machine_set_image_decoder(machine_image_fn fn, void *ctx)
{
    image_fn = fn;
    image_ctx = ctx;
}

/* A still image through the host's decoder: RGBA (malloc'd) and its size. */
bool qt_decode_image(const uint8_t *data, size_t len, uint8_t **rgba, int *w, int *h)
{
    *rgba = NULL;
    return image_fn && image_fn(data, len, rgba, w, h, image_ctx) && *rgba;
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

/* The host's decoders (hostvideo.h), for codecs we have none of our own for. */
static bool host_supports(int depth) { return depth > 0; }
static void *host_open(const qt_video_format *f)
{
    return hostvideo_open(f->format, f->width, f->height, f->desc, f->desc_len);
}
static bool host_decode(void *s, const qt_video_format *f, const uint32_t *palette, const uint8_t *data, size_t len,
                        uint8_t *frame)
{
    (void)f, (void)palette;
    return hostvideo_decode(s, data, len, frame);
}
static void host_close(void *s) { hostvideo_close(s); }
static const qt_video_codec qt_codec_host = { 0, host_supports, host_open, host_decode, host_close };

struct qt_vdec {
    const qt_video_codec *codec;
    qt_video_format format;
    uint32_t palette[256];
    void *state;
};

/* Ours first, then the host's, then a codec component the program brought. */
static const qt_video_codec *codec_for(uint32_t format, int depth)
{
    for (size_t i = 0; i < sizeof codecs / sizeof *codecs; i++)
        if (codecs[i]->format == format && codecs[i]->supports(depth))
            return codecs[i];
    if (format && hostvideo_supported(format))
        return &qt_codec_host;
    if (format && qt_codec_program_supports(format))
        return &qt_codec_program;
    return NULL;
}

bool qt_video_format_of_desc(const uint8_t *d, uint32_t len, qt_video_format *f, uint32_t palette[256])
{
    if (len < 86)
        return false;
    f->format = be32(d + 4);
    f->width = (int16_t)be16(d + 32);
    f->height = (int16_t)be16(d + 34);
    f->depth = (int16_t)be16(d + 82);
    f->palette = NULL;
    f->desc = d;
    f->desc_len = len;
    int16_t clut = (int16_t)be16(d + 84);
    if (clut == 0 && f->depth >= 1 && f->depth <= 8 && len >= 94) {
        const uint8_t *c = d + 86;
        int count = (int)be16(c + 6) + 1;
        memset(palette, 0, 256 * sizeof *palette);
        for (int k = 0; k < count && k < 256 && 94 + (uint32_t)k * 8 + 8 <= len; k++) {
            const uint8_t *e = c + 8 + k * 8;
            int v = (be16(c + 4) & 0x8000 ? k : (int)be16(e)) & 255;
            palette[v] = (uint32_t)e[2] << 16 | (uint32_t)e[4] << 8 | e[6];
        }
        f->palette = palette;
    }
    return f->width > 0 && f->height > 0;
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

/* The plain formats of QuickTime 4 and later, decoded here. */
static int pcm_bytes(uint32_t format)
{
    if (is(format, "ulaw") || is(format, "alaw"))
        return 1;
    if (is(format, "in24"))
        return 3;
    if (is(format, "in32") || is(format, "fl32"))
        return 4;
    return is(format, "fl64") ? 8 : 0;
}

bool qt_adec_host(const qt_sound_format *f)
{
    return !pcm_bytes(f->format) && !is(f->format, "ima4") && !is(f->format, "MAC3") && !is(f->format, "MAC6") &&
           !is(f->format, "raw ") && !is(f->format, "twos") && !is(f->format, "sowt") && !is(f->format, "NONE") &&
           hostaudio_supported(f->format);
}

bool qt_adec_supported(const qt_sound_format *f)
{
    if (f->channels < 1 || f->channels > 2 || f->rate < 1000 || f->rate > 200000)
        return false;
    if (is(f->format, "raw ") || is(f->format, "twos") || is(f->format, "sowt") || is(f->format, "NONE"))
        return f->bits == 8 || f->bits == 16;
    return is(f->format, "ima4") || is(f->format, "MAC3") || is(f->format, "MAC6") || pcm_bytes(f->format) ||
           qt_adec_host(f);
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
    if (pcm_bytes(f->format))
        return (size_t)frames * ch * (size_t)pcm_bytes(f->format);
    if (qt_adec_host(f)) /* a constant rate: whole packets */
        return f->frames_per_packet && f->bytes_per_packet
                   ? (frames + f->frames_per_packet - 1) / f->frames_per_packet * (size_t)f->bytes_per_packet
                   : 0;
    return (size_t)frames * ch * (size_t)(f->bits / 8);
}

/* G.711: the ITU's expansions. */
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
    t = seg ? (t + 0x108) << (seg - 1) : t + 8;
    return (int16_t)(a & 0x80 ? t : -t);
}

static int16_t pcm_sample(const qt_sound_format *f, const uint8_t *p)
{
    uint8_t b[8];
    int n = pcm_bytes(f->format);
    for (int i = 0; i < n; i++) /* big-endian, as the description says unless 'enda' */
        b[i] = f->little ? p[n - 1 - i] : p[i];
    if (is(f->format, "ulaw"))
        return ulaw(b[0]);
    if (is(f->format, "alaw"))
        return alaw(b[0]);
    if (is(f->format, "in24") || is(f->format, "in32"))
        return (int16_t)(b[0] << 8 | b[1]);
    double v;
    if (is(f->format, "fl32")) {
        uint32_t u = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
        float x;
        memcpy(&x, &u, 4);
        v = x;
    } else {
        uint64_t u = 0;
        for (int i = 0; i < 8; i++)
            u = u << 8 | b[i];
        memcpy(&v, &u, 8);
    }
    v *= 32767.0;
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

void qt_sound_desc_extras(const uint8_t *e, uint32_t len, qt_sound_format *f)
{
    if (len < 36)
        return;
    uint32_t version = (uint32_t)e[16] << 8 | e[17], at = 36;
    if (version == 1 && len >= 52) {
        f->frames_per_packet = (uint32_t)e[36] << 24 | (uint32_t)e[37] << 16 | (uint32_t)e[38] << 8 | e[39];
        f->bytes_per_packet = (uint32_t)e[44] << 24 | (uint32_t)e[45] << 16 | (uint32_t)e[46] << 8 | e[47];
        at = 52;
    }
    /* extension atoms, and a 'wave' atom's children: 'enda', 'esds' */
    for (int pass = 0; pass < 2; pass++) {
        uint32_t end = len, wave = 0;
        while (at + 8 <= end) {
            uint32_t sz = (uint32_t)e[at] << 24 | (uint32_t)e[at + 1] << 16 | (uint32_t)e[at + 2] << 8 | e[at + 3];
            if (sz < 8 || at + sz > end)
                break;
            const uint8_t *t = e + at + 4;
            if (!memcmp(t, "enda", 4) && sz >= 10)
                f->little = e[at + 9] || e[at + 8];
            else if (!memcmp(t, "esds", 4) && sz > 12) {
                f->cookie = e + at + 12; /* past the version and flags */
                f->cookie_len = sz - 12;
            } else if (!memcmp(t, "wave", 4))
                wave = at;
            at += sz;
        }
        if (!wave)
            break;
        uint32_t wsz = (uint32_t)e[wave] << 24 | (uint32_t)e[wave + 1] << 16 | (uint32_t)e[wave + 2] << 8 | e[wave + 3];
        at = wave + 8;
        len = wave + wsz;
    }
}

uint32_t qt_adec_decode_packets(const qt_sound_format *f, const uint8_t *data, size_t len, const uint32_t *sizes,
                                int npackets, int16_t **pcm)
{
    *pcm = NULL;
    hostaudio_dec *d = hostaudio_open(f->format, f->channels, f->rate, f->cookie, f->cookie_len);
    if (!d)
        return 0;
    uint32_t n = hostaudio_decode(d, data, len, sizes, npackets, pcm);
    hostaudio_close(d);
    return n;
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
    if (qt_adec_host(f))
        return qt_adec_decode_packets(f, data, len, NULL, 0, pcm);
    if (is(f->format, "ima4"))
        frames = len / (34 * nch) * 64;
    else if (is(f->format, "MAC3"))
        frames = len / nch * 3;
    else if (is(f->format, "MAC6"))
        frames = len / nch * 6;
    else if (pcm_bytes(f->format))
        frames = len / (nch * (size_t)pcm_bytes(f->format));
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
    if (pcm_bytes(f->format)) {
        for (size_t i = 0; i < n; i++)
            out[i] = pcm_sample(f, data + i * (size_t)pcm_bytes(f->format));
        return (uint32_t)frames;
    }
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
