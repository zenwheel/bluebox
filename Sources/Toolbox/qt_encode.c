/*
 * QuickTime's compressors: None ('raw '), Animation ('rle ') and Photo-JPEG
 * ('jpeg', through the host's image encoder), writing what qt_codec_apple.c
 * and a real Mac's QuickTime read.
 *
 * Input is a frame of pixel values at the codec depth: colour indices at
 * 1-8 bits and 33-40 (grey), xRGB 1-5-5-5 at 16, 0x00RRGGBB at 24 and 32.
 *
 * None: rows of pixels, high bits first, each row padded to a whole byte;
 * 16-bit pixels big endian, 24-bit RGB, 32-bit ARGB (alpha 255).
 *
 * Animation: a 4-byte chunk size, a 2-byte header (0x0008, then the start
 * line, 2 bytes of 0, the line count and 2 bytes of 0), then per line a
 * skip byte (units to skip plus one) and codes: -1 ends the line, 0 and a
 * byte skips that many units less one, -n repeats the next unit n times,
 * +n copies n units.  Units are one pixel at 16, 24 and 32 bits (2 bytes,
 * RGB, ARGB), or 4 bytes of indices (16, 8 or 4 pixels at 2, 4 and 8
 * bits).  Depth 1 is pairs of a skip byte (the top bit starting a new line
 * at the unit in the rest, otherwise units to move on) and a code byte (-1
 * nothing, 0 the end, -n repeat, +n copy), in units of 16 pixels.  A
 * difference frame codes only the lines between the first and last that
 * changed, and skips the units that didn't; an unchanged frame is just a
 * size, a 0 header and a 0 byte (7 bytes: anything under 8 is no change).
 * A frame ends with a 0 byte (a 0, 0 pair at depth 1).
 */
#include <stdlib.h>
#include <string.h>

#include "machine.h"
#include "qt_codec.h"

enum { RAW = 0x72617720, RLE = 0x726C6520, JPEG = 0x6A706567 };

static machine_image_encode_fn encode_fn;
static void *encode_ctx;

void machine_set_image_encoder(machine_image_encode_fn fn, void *ctx)
{
    encode_fn = fn;
    encode_ctx = ctx;
}

static int bits_of(int depth) { return depth > 32 ? depth - 32 : depth; }

static bool plain_depth(int depth)
{
    switch (depth) {
    case 1: case 2: case 4: case 8: case 16: case 24: case 32: case 33: case 34: case 36: case 40:
        return true;
    default:
        return false;
    }
}

bool qt_venc_supported(uint32_t format, int depth)
{
    if (format == RAW || format == RLE)
        return plain_depth(depth);
    if (format == JPEG)
        return encode_fn && (depth == 24 || depth == 40);
    return false;
}

bool qt_venc_temporal(uint32_t format) { return format == RLE; }

/* ---- Output ---- */

typedef struct {
    uint8_t *p;
    size_t n, cap;
    bool bad;
} buf;

static void put8(buf *b, unsigned v)
{
    if (b->n == b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        uint8_t *p = realloc(b->p, cap);
        if (!p) {
            b->bad = true;
            return;
        }
        b->p = p;
        b->cap = cap;
    }
    b->p[b->n++] = (uint8_t)v;
}

static void put16(buf *b, unsigned v)
{
    put8(b, v >> 8 & 0xFF);
    put8(b, v & 0xFF);
}

static void put32(buf *b, uint32_t v)
{
    put16(b, v >> 16);
    put16(b, v & 0xFFFF);
}

/* ---- None ---- */

static size_t raw_row(const qt_venc_format *f) { return ((size_t)f->width * (size_t)bits_of(f->depth) + 7) / 8; }

static void raw_encode(const qt_venc_format *f, const uint32_t *px, buf *b)
{
    int bits = bits_of(f->depth);
    for (int y = 0; y < f->height; y++) {
        const uint32_t *row = px + (size_t)y * (size_t)f->width;
        if (bits >= 16) {
            for (int x = 0; x < f->width; x++) {
                uint32_t v = row[x];
                if (bits == 16) {
                    put16(b, v & 0x7FFF);
                } else {
                    if (bits == 32)
                        put8(b, 0xFF);
                    put8(b, v >> 16 & 0xFF);
                    put8(b, v >> 8 & 0xFF);
                    put8(b, v & 0xFF);
                }
            }
        } else {
            unsigned acc = 0, n = 0, mask = (1u << bits) - 1;
            for (int x = 0; x < f->width; x++) {
                acc = acc << bits | (row[x] & mask);
                n += (unsigned)bits;
                if (n == 8) {
                    put8(b, acc);
                    acc = n = 0;
                }
            }
            if (n)
                put8(b, acc << (8 - n));
        }
    }
}

/* ---- Animation ---- */

static int rle_group(int bits) { return bits == 1 || bits == 2 ? 16 : bits == 4 ? 8 : bits == 8 ? 4 : 1; }
static int rle_unit_bytes(int bits) { return bits == 1 || bits == 16 ? 2 : bits == 24 ? 3 : 4; }

/* A line's units: pixels, or indices packed high bits first (0 past the edge). */
static void rle_units(const uint32_t *row, int width, int bits, uint32_t *u, int nu)
{
    int group = rle_group(bits);
    if (group == 1) {
        for (int k = 0; k < nu; k++)
            u[k] = row[k] & (bits == 16 ? 0x7FFFu : 0xFFFFFFu);
        return;
    }
    uint32_t mask = (1u << bits) - 1;
    for (int k = 0; k < nu; k++) {
        uint32_t v = 0;
        for (int i = 0; i < group; i++) {
            int x = k * group + i;
            v = v << bits | (x < width ? row[x] & mask : 0);
        }
        u[k] = v;
    }
}

static void put_unit(buf *b, int bits, uint32_t u)
{
    switch (rle_unit_bytes(bits)) {
    case 2: put16(b, u); break;
    case 3:
        put8(b, u >> 16 & 0xFF);
        put16(b, u & 0xFFFF);
        break;
    default:
        if (bits == 32)
            u = 0xFF000000u | u;
        put32(b, u);
        break;
    }
}

/* Codes for units [i, j): runs of two or more repeated, the rest copied. */
static void rle_span(buf *b, const uint32_t *u, int i, int j, int bits)
{
    while (i < j) {
        int r = 1;
        while (i + r < j && r < 128 && u[i + r] == u[i])
            r++;
        if (r >= 2) {
            put8(b, (unsigned)(-r) & 0xFF);
            put_unit(b, bits, u[i]);
            i += r;
            continue;
        }
        int n = 1;
        while (i + n < j && n < 127 && !(i + n + 1 < j && u[i + n] == u[i + n + 1]))
            n++;
        put8(b, (unsigned)n);
        for (int k = 0; k < n; k++)
            put_unit(b, bits, u[i + k]);
        i += n;
    }
}

/* One line at 2 bits and up; pu, the line before, for a difference frame. */
static void rle_line(buf *b, const uint32_t *u, const uint32_t *pu, int nu, int bits)
{
    int i = 0;
    bool first = true;
    while (i < nu) {
        int k = 0;
        if (pu)
            while (i + k < nu && u[i + k] == pu[i + k])
                k++;
        if (pu && i + k == nu)
            break;
        do { /* the skip byte begins the line; later skips are 0 and a count */
            int s = k < 254 ? k : 254;
            if (!first)
                put8(b, 0);
            put8(b, (unsigned)s + 1);
            first = false;
            k -= s;
            i += s;
        } while (k > 0);
        int j = i + 1;
        if (pu) {
            while (j < nu && u[j] != pu[j])
                j++;
        } else {
            j = nu;
        }
        rle_span(b, u, i, j, bits);
        i = j;
    }
    if (first)
        put8(b, 1);
    put8(b, 0xFF);
}

/* Depth 1: moves to unit target, returning the skip byte for the next code. */
static unsigned rle1_move(buf *b, bool *first, int *pos, int target)
{
    unsigned sk;
    if (*first) {
        int a = target < 127 ? target : 127;
        sk = 0x80u | (unsigned)a;
        *pos = a;
        *first = false;
    } else {
        int d = target - *pos;
        sk = (unsigned)(d < 127 ? d : 127);
        *pos += (int)sk;
    }
    while (*pos < target) {
        put8(b, sk);
        put8(b, 0xFF);
        int d = target - *pos;
        sk = (unsigned)(d < 127 ? d : 127);
        *pos += (int)sk;
    }
    return sk;
}

static void rle1_line(buf *b, const uint32_t *u, const uint32_t *pu, int nu)
{
    int i = 0, pos = 0;
    bool first = true;
    while (i < nu) {
        int k = 0;
        if (pu)
            while (i + k < nu && u[i + k] == pu[i + k])
                k++;
        if (pu && i + k == nu)
            break;
        i += k;
        int j = i + 1;
        if (pu) {
            while (j < nu && u[j] != pu[j])
                j++;
        } else {
            j = nu;
        }
        unsigned sk = rle1_move(b, &first, &pos, i);
        while (i < j) {
            int r = 1;
            while (i + r < j && r < 128 && u[i + r] == u[i])
                r++;
            put8(b, sk);
            sk = 0;
            if (r >= 2) {
                put8(b, (unsigned)(-r) & 0xFF);
                put16(b, u[i]);
                i += r;
                pos += r;
                continue;
            }
            int n = 1;
            while (i + n < j && n < 127 && !(i + n + 1 < j && u[i + n] == u[i + n + 1]))
                n++;
            put8(b, (unsigned)n);
            for (int m = 0; m < n; m++)
                put16(b, u[i + m]);
            i += n;
            pos += n;
        }
    }
    if (first) { /* nothing changed: the line still begins */
        put8(b, 0x80);
        put8(b, 0xFF);
    }
}

static bool rle_encode(const qt_venc_format *f, const uint32_t *px, const uint32_t *prev, buf *b)
{
    int bits = bits_of(f->depth), group = rle_group(bits);
    int nu = (f->width + group - 1) / group, h = f->height;
    uint32_t *cur = malloc((size_t)nu * (size_t)h * sizeof *cur);
    uint32_t *old = prev ? malloc((size_t)nu * (size_t)h * sizeof *old) : NULL;
    if (!cur || (prev && !old)) {
        free(cur);
        free(old);
        return false;
    }
    for (int y = 0; y < h; y++) {
        rle_units(px + (size_t)y * (size_t)f->width, f->width, bits, cur + (size_t)y * (size_t)nu, nu);
        if (old)
            rle_units(prev + (size_t)y * (size_t)f->width, f->width, bits, old + (size_t)y * (size_t)nu, nu);
    }
    int start = 0, end = h;
    size_t line = (size_t)nu * sizeof *cur;
    if (old) {
        while (start < h && !memcmp(cur + (size_t)start * (size_t)nu, old + (size_t)start * (size_t)nu, line))
            start++;
        while (end > start && !memcmp(cur + (size_t)(end - 1) * (size_t)nu, old + (size_t)(end - 1) * (size_t)nu, line))
            end--;
    }
    put32(b, 0); /* the size, filled in at the end */
    if (start == end) {
        put16(b, 0);
    } else {
        put16(b, 0x0008);
        put16(b, (unsigned)start);
        put16(b, 0);
        put16(b, (unsigned)(end - start));
        put16(b, 0);
        for (int y = start; y < end; y++) {
            const uint32_t *u = cur + (size_t)y * (size_t)nu, *pu = old ? old + (size_t)y * (size_t)nu : NULL;
            if (bits == 1)
                rle1_line(b, u, pu, nu);
            else
                rle_line(b, u, pu, nu, bits);
        }
    }
    if (bits == 1 && start != end)
        put8(b, 0);
    put8(b, 0);
    free(cur);
    free(old);
    if (b->bad)
        return false;
    uint32_t n = (uint32_t)b->n;
    b->p[0] = (uint8_t)(n >> 24);
    b->p[1] = (uint8_t)(n >> 16);
    b->p[2] = (uint8_t)(n >> 8);
    b->p[3] = (uint8_t)n;
    return true;
}

/* ---- Photo-JPEG ---- */

static bool jpeg_encode(const qt_venc_format *f, const uint32_t *px, buf *b)
{
    size_t n = (size_t)f->width * (size_t)f->height;
    uint8_t *rgba = malloc(n * 4), *out = NULL;
    size_t len = 0;
    if (!rgba)
        return false;
    for (size_t i = 0; i < n; i++) {
        uint32_t v = px[i];
        if (f->depth == 40) { /* grey, white first */
            unsigned g = 255 - (v & 255);
            v = g << 16 | g << 8 | g;
        }
        rgba[i * 4] = (uint8_t)(v >> 16);
        rgba[i * 4 + 1] = (uint8_t)(v >> 8);
        rgba[i * 4 + 2] = (uint8_t)v;
        rgba[i * 4 + 3] = 255;
    }
    double q = f->quality >= 0x400 ? 1.0 : f->quality / 1023.0;
    q = q < 0.05 ? 0.05 : q > 1 ? 1 : q;
    bool ok = encode_fn(rgba, f->width, f->height, q, &out, &len, encode_ctx) && out && len;
    free(rgba);
    if (ok) {
        free(b->p);
        b->p = out;
        b->n = b->cap = len;
    } else {
        free(out);
    }
    return ok;
}

/* ---- Entry points ---- */

size_t qt_venc_max_size(const qt_venc_format *f)
{
    size_t w = (size_t)(f->width > 0 ? f->width : 0), h = (size_t)(f->height > 0 ? f->height : 0);
    if (f->format == RAW)
        return raw_row(f) * h;
    if (f->format == RLE) {
        int bits = bits_of(f->depth), group = rle_group(bits);
        size_t nu = (w + (size_t)group - 1) / (size_t)group, ub = (size_t)rle_unit_bytes(bits);
        /* every unit copied, a code per 127 of them, and the moves of a line */
        return 32 + h * (nu * ub + nu / 64 + 16);
    }
    if (f->format == JPEG)
        return 65536 + w * h * 4;
    return 0;
}

size_t qt_venc_encode(const qt_venc_format *f, const uint32_t *px, const uint32_t *prev, uint8_t **out)
{
    *out = NULL;
    if (!qt_venc_supported(f->format, f->depth) || f->width <= 0 || f->height <= 0 || f->width > 4096 ||
        f->height > 4096)
        return 0;
    buf b = { 0 };
    bool ok;
    if (f->format == RAW) {
        raw_encode(f, px, &b);
        ok = !b.bad;
    } else if (f->format == RLE) {
        ok = rle_encode(f, px, prev, &b);
    } else {
        ok = jpeg_encode(f, px, &b);
    }
    if (!ok || !b.n) {
        free(b.p);
        return 0;
    }
    *out = b.p;
    return b.n;
}
