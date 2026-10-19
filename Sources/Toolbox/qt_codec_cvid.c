/*
 * Cinepak ('cvid').
 *
 * A frame (big-endian throughout) starts with a 10-byte header: flags,
 * a 24-bit length, width, height and the number of strips.  Each strip has
 * a 12-byte header (id 0x10 key or 0x11 inter, 24-bit length, top, left,
 * bottom, right) followed by chunks with 4-byte headers (id, 24-bit
 * length).  Chunks 0x20-0x27 load the strip's codebooks: bit 1 picks the
 * V1 book over the V4 one, bit 0 a partial update (a 32-bit flag word ahead
 * of each 32 entries says which are replaced), and bit 2 four-byte entries
 * of luminance only instead of six-byte ones with chrominance (y0-y3, u, v).
 * Chunks 0x30 (intra), 0x31 (inter, with skipped blocks) and 0x32 (V1
 * only) code the strip's 4x4 blocks: a V1 block is one entry scaled up, a
 * V4 block four entries, one per 2x2 quarter.
 *
 * Codebooks belong to strip positions and last from frame to frame; unless
 * the frame's flags have bit 0 set, a strip starts with the codebooks the
 * strip before it ended with.  At depth 8 the entries hold colour indices
 * into the palette instead of luminance.
 */
#include <stdlib.h>
#include <string.h>

#include "qt_codec.h"

enum { MAX_STRIPS = 32 };

typedef uint8_t cvid_codebook[256][12];   /* four RGB pixels, a 2x2 square */

typedef struct {
    cvid_codebook v4, v1;
} cvid_strip;

typedef struct {
    cvid_strip strips[MAX_STRIPS];
} cvid_state;

static unsigned rb16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static unsigned rb24(const uint8_t *p) { return (unsigned)p[0] << 16 | (unsigned)p[1] << 8 | p[2]; }
static uint32_t rb32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static uint8_t clip(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }

static bool supports(int depth) { return depth == 8 || depth == 24 || depth == 32 || depth == 40; }

static void *open_(const qt_video_format *f)
{
    (void)f;
    return calloc(1, sizeof(cvid_state));
}

static void close_(void *s) { free(s); }

static void load_codebook(cvid_codebook book, int id, const uint8_t *data, size_t len, const uint32_t *palette)
{
    const uint8_t *end = data + len;
    size_t n = (id & 0x04) ? 4 : 6;
    uint32_t flag = 0, mask = 0;

    for (int i = 0; i < 256; i++) {
        if ((id & 0x01) && !(mask >>= 1)) {
            if (end - data < 4)
                return;
            flag = rb32(data);
            data += 4;
            mask = 0x80000000u;
        }
        if ((id & 0x01) && !(flag & mask))
            continue;
        if ((size_t)(end - data) < n)
            return;
        uint8_t *p = book[i];
        if (n == 6) {
            int u = (int8_t)data[4], v = (int8_t)data[5];
            for (int k = 0; k < 4; k++) {
                int y = data[k];
                p[3 * k] = clip(y + 2 * v);
                p[3 * k + 1] = clip(y - u / 2 - v);
                p[3 * k + 2] = clip(y + 2 * u);
            }
        } else if (palette) {
            for (int k = 0; k < 4; k++) {
                uint32_t c = palette[data[k]];
                p[3 * k] = (uint8_t)(c >> 16);
                p[3 * k + 1] = (uint8_t)(c >> 8);
                p[3 * k + 2] = (uint8_t)c;
            }
        } else {
            for (int k = 0; k < 4; k++)
                p[3 * k] = p[3 * k + 1] = p[3 * k + 2] = data[k];
        }
        data += n;
    }
}

static void put(const qt_video_format *f, uint8_t *frame, int x, int y, const uint8_t *rgb)
{
    if (x >= f->width || y >= f->height)
        return;
    uint8_t *d = frame + ((size_t)y * (size_t)f->width + (size_t)x) * 4;
    d[0] = rgb[0];
    d[1] = rgb[1];
    d[2] = rgb[2];
    d[3] = 255;
}

/* Fill the 2x2 square at (x, y), scaled up by s (1 or 2), from one entry. */
static void put_entry(const qt_video_format *f, uint8_t *frame, int x, int y, int s, const uint8_t *e)
{
    for (int k = 0; k < 4; k++) {
        int px = x + (k & 1) * s, py = y + (k >> 1) * s;
        for (int dy = 0; dy < s; dy++)
            for (int dx = 0; dx < s; dx++)
                put(f, frame, px + dx, py + dy, e + 3 * k);
    }
}

static bool decode_vectors(const qt_video_format *f, cvid_strip *st, int id, const uint8_t *data, size_t len,
                           int x1, int y1, int x2, int y2, uint8_t *frame)
{
    const uint8_t *end = data + len;
    uint32_t flag = 0, mask = 0;

    for (int y = y1; y < y2; y += 4) {
        for (int x = x1; x < x2; x += 4) {
            if ((id & 0x01) && !(mask >>= 1)) {
                if (end - data < 4)
                    return false;
                flag = rb32(data);
                data += 4;
                mask = 0x80000000u;
            }
            if ((id & 0x01) && !(flag & mask))
                continue;
            if (!(id & 0x02) && !(mask >>= 1)) {
                if (end - data < 4)
                    return false;
                flag = rb32(data);
                data += 4;
                mask = 0x80000000u;
            }
            if ((id & 0x02) || !(flag & mask)) {
                if (data >= end)
                    return false;
                put_entry(f, frame, x, y, 2, st->v1[*data++]);
            } else {
                if (end - data < 4)
                    return false;
                put_entry(f, frame, x, y, 1, st->v4[data[0]]);
                put_entry(f, frame, x + 2, y, 1, st->v4[data[1]]);
                put_entry(f, frame, x, y + 2, 1, st->v4[data[2]]);
                put_entry(f, frame, x + 2, y + 2, 1, st->v4[data[3]]);
                data += 4;
            }
        }
    }
    return true;
}

static bool decode_strip(const qt_video_format *f, const uint32_t *palette, cvid_strip *st, const uint8_t *data,
                         size_t len, int x1, int y1, int x2, int y2, uint8_t *frame)
{
    const uint8_t *end = data + len;
    int w = (f->width + 3) & ~3, h = (f->height + 3) & ~3;

    if (x2 > w || y2 > h || x1 >= x2 || y1 >= y2)
        return false;
    while (end - data >= 4) {
        int id = data[0];
        size_t size = rb24(data + 1);
        if (size < 4)
            return false;
        size -= 4;
        data += 4;
        if (size > (size_t)(end - data))
            size = (size_t)(end - data);
        switch (id) {
        case 0x20: case 0x21: case 0x24: case 0x25:
            load_codebook(st->v4, id, data, size, f->depth == 8 ? palette : NULL);
            break;
        case 0x22: case 0x23: case 0x26: case 0x27:
            load_codebook(st->v1, id, data, size, f->depth == 8 ? palette : NULL);
            break;
        case 0x30: case 0x31: case 0x32:
            return decode_vectors(f, st, id, data, size, x1, y1, x2, y2, frame);
        }
        data += size;
    }
    return false;
}

static bool decode(void *s, const qt_video_format *f, const uint32_t *palette, const uint8_t *data, size_t len,
                   uint8_t *frame)
{
    cvid_state *cs = s;
    const uint8_t *end = data + len;

    if (!cs || len < 10)
        return false;
    int flags = data[0];
    int nstrips = (int)rb16(data + 8);
    if (nstrips > MAX_STRIPS)
        nstrips = MAX_STRIPS;
    data += 10;

    int y0 = 0;
    for (int i = 0; i < nstrips; i++) {
        if (end - data < 12)
            return false;
        size_t size = rb24(data + 1);
        int top = (int)rb16(data + 4), left = (int)rb16(data + 6);
        int bottom = (int)rb16(data + 8), right = (int)rb16(data + 10);
        /* A top of zero places the strip below the last, its bottom then a height. */
        if (top == 0) {
            top = y0;
            bottom += y0;
        }
        if (size < 12)
            return false;
        size -= 12;
        data += 12;
        if (size > (size_t)(end - data))
            size = (size_t)(end - data);
        if (i > 0 && !(flags & 0x01))
            cs->strips[i] = cs->strips[i - 1];
        if (!decode_strip(f, palette, &cs->strips[i], data, size, left, top, right, bottom, frame))
            return false;
        data += size;
        y0 = bottom;
    }
    return true;
}

const qt_video_codec qt_codec_cvid = { 0x63766964, supports, open_, decode, close_ };
