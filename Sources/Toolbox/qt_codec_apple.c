/*
 * Apple's codecs: Animation ('rle '), Graphics ('smc '), Video ('rpza') and
 * None ('raw ').
 *
 * Animation runs lengths along each line, in units of a few pixels for the
 * indexed depths; Graphics and Video work in 4x4 blocks (Graphics with
 * caches of colour pairs, quads and octets, Video with two RGB555 colours
 * and two blends between them).  All three code most frames as changes to
 * the one before.  None is plain rows of pixels.  The formats follow
 * QuickTime's own, as libavcodec's qtrle, smc and rpza decoders read them.
 */
#include <stdlib.h>
#include <string.h>

#include "qt_codec.h"

/* A cursor over a sample; reading past the end marks it bad and gives 0. */
typedef struct {
    const uint8_t *p, *end;
    bool bad;
} reader;

static unsigned get8(reader *r)
{
    if (r->p >= r->end) {
        r->bad = true;
        return 0;
    }
    return *r->p++;
}

static unsigned get16(reader *r)
{
    unsigned hi = get8(r);
    return hi << 8 | get8(r);
}

static bool skip(reader *r, size_t n)
{
    if ((size_t)(r->end - r->p) < n) {
        r->bad = true;
        return false;
    }
    r->p += n;
    return true;
}

static unsigned peek8(const reader *r) { return r->p < r->end ? *r->p : 0; }

static int bits_of(int depth) { return depth > 32 ? depth - 32 : depth; }

static void put(uint8_t *px, uint32_t rgb)
{
    px[0] = (uint8_t)(rgb >> 16);
    px[1] = (uint8_t)(rgb >> 8);
    px[2] = (uint8_t)rgb;
    px[3] = 255;
}

static uint32_t rgb555(unsigned c)
{
    unsigned r = c >> 10 & 31, g = c >> 5 & 31, b = c & 31;
    return (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
}

/* Decoder state: the block codecs keep their picture at whole blocks, so a
 * block repeated from the right or bottom edge keeps its clipped pixels. */
typedef struct {
    int bw, bh;              /* size in 4x4 blocks */
    uint8_t *index;          /* 'smc ': bw * 4 by bh * 4 colour indices */
    uint16_t *rgb;           /* 'rpza': bw * 4 by bh * 4 RGB555 pixels */
    uint8_t pairs[256 * 2], quads[256 * 4], octets[256 * 8];
} state;

static state *new_state(const qt_video_format *f)
{
    state *s = calloc(1, sizeof *s);
    if (s) {
        s->bw = (f->width + 3) / 4;
        s->bh = (f->height + 3) / 4;
    }
    return s;
}

static void close_(void *s)
{
    state *st = s;
    if (st) {
        free(st->index);
        free(st->rgb);
        free(st);
    }
}

static void *open_plain(const qt_video_format *f) { return new_state(f); }

/* ---- Animation ('rle ') ----
 *
 * A sample is a chunk size, a header (0x0008: a start line and a line count
 * follow), then per line a skip count and codes: -1 ends the line, 0 skips
 * again, a negative count repeats one unit, a positive one copies that many
 * units.  Units are 4 bytes for depths 2-8, a pixel above.  Depth 1 is
 * different: pairs of skip and code bytes, with a skip's top bit starting a
 * new line, and units of 16 pixels.
 */
static bool rle_supports(int depth)
{
    switch (depth) {
    case 1: case 2: case 4: case 8: case 16: case 24: case 32: case 33: case 34: case 36: case 40:
        return true;
    default:
        return false;
    }
}

/* Reads one unit into up to 16 colours; returns the pixels in it. */
static int rle_unit(reader *r, int bits, const uint32_t *pal, uint32_t *out)
{
    switch (bits) {
    case 16:
        out[0] = rgb555(get16(r));
        return 1;
    case 24: {
        uint32_t c = get8(r) << 16;
        c |= get8(r) << 8;
        out[0] = c | get8(r);
        return 1;
    }
    case 32: {
        get8(r);
        uint32_t c = get8(r) << 16;
        c |= get8(r) << 8;
        out[0] = c | get8(r);
        return 1;
    }
    default: {
        /* 4 bytes of 2, 4 or 8-bit indices, high bits first */
        int n = 0, mask = (1 << bits) - 1;
        for (int i = 0; i < 4; i++) {
            unsigned b = get8(r);
            for (int sh = 8 - bits; sh >= 0; sh -= bits)
                out[n++] = pal[b >> sh & (unsigned)mask];
        }
        return n;
    }
    }
}

/* Writes n pixels at x on a line, dropping those past the right edge;
 * false if they start left of it or run past the last whole unit. */
static bool rle_emit(uint8_t *line, int width, int limit, int x, const uint32_t *c, int n)
{
    if (x < 0 || x + n > limit)
        return false;
    for (int i = 0; i < n && x + i < width; i++)
        put(line + (size_t)(x + i) * 4, c[i]);
    return true;
}

static bool rle_decode1(reader *r, const qt_video_format *f, const uint32_t *pal, int y, int lines,
                        uint8_t *frame)
{
    int limit = (f->width + 15) & ~15, x = 0;
    uint32_t c[16];
    /* The first line is begun by a skip with the top bit set like any other. */
    y--;
    lines++;
    while (lines > 0) {
        unsigned sk = get8(r);
        int code = (int8_t)get8(r);
        if (r->bad)
            return false;
        if (code == 0)
            break;
        if (sk & 0x80) {
            lines--;
            y++;
            x = 16 * (int)(sk & 0x7F);
        } else
            x += 16 * (int)sk;
        if (code == -1)
            continue;
        if (y < 0 || y >= f->height)
            return false;
        uint8_t *line = frame + (size_t)y * (size_t)f->width * 4;
        if (code < 0) {
            unsigned a = get8(r), b = get8(r), v = a << 8 | b;
            for (int i = 0; i < 16; i++)
                c[i] = pal[v >> (15 - i) & 1];
            for (int k = 0; k < -code; k++, x += 16)
                if (!rle_emit(line, f->width, limit, x, c, 16))
                    return false;
        } else {
            for (int k = 0; k < code; k++, x += 16) {
                unsigned a = get8(r), b = get8(r), v = a << 8 | b;
                for (int i = 0; i < 16; i++)
                    c[i] = pal[v >> (15 - i) & 1];
                if (!rle_emit(line, f->width, limit, x, c, 16))
                    return false;
            }
        }
        if (r->bad)
            return false;
    }
    return !r->bad;
}

static bool rle_decode(void *s, const qt_video_format *f, const uint32_t *pal, const uint8_t *data, size_t len,
                       uint8_t *frame)
{
    (void)s;
    if (len <= 8)
        return true; /* no change */
    reader r = { data, data + len, false };
    skip(&r, 4); /* the chunk size */
    int y = 0, lines = f->height;
    if (get16(&r) & 0x0008) {
        y = (int)get16(&r);
        skip(&r, 2);
        lines = (int)get16(&r);
        skip(&r, 2);
        if (r.bad || y + lines > f->height)
            return false;
    }
    int bits = bits_of(f->depth);
    if (bits == 1)
        return rle_decode1(&r, f, pal, y, lines, frame);
    int group = bits == 2 ? 16 : bits == 4 ? 8 : bits == 8 ? 4 : 1;
    int limit = (f->width + group - 1) / group * group;
    uint32_t c[16];
    for (; lines > 0; lines--, y++) {
        uint8_t *line = frame + (size_t)y * (size_t)f->width * 4;
        int x = group * ((int)get8(&r) - 1);
        for (;;) {
            int code = (int8_t)get8(&r);
            if (r.bad)
                return false;
            if (code == -1)
                break;
            if (code == 0) {
                x += group * ((int)get8(&r) - 1);
            } else if (code < 0) {
                int n = rle_unit(&r, bits, pal, c);
                for (int k = 0; k < -code; k++, x += n)
                    if (!rle_emit(line, f->width, limit, x, c, n))
                        return false;
            } else {
                for (int k = 0; k < code; k++) {
                    int n = rle_unit(&r, bits, pal, c);
                    if (r.bad || !rle_emit(line, f->width, limit, x, c, n))
                        return false;
                    x += n;
                }
            }
        }
    }
    return true;
}

/* ---- Block pictures ---- */

/* Copies the block picture (rows of bw * 4 pixels) to the frame. */
static void show_index(const state *s, const qt_video_format *f, const uint32_t *pal, uint8_t *frame)
{
    int stride = s->bw * 4;
    for (int y = 0; y < f->height; y++) {
        const uint8_t *src = s->index + (size_t)y * (size_t)stride;
        uint8_t *dst = frame + (size_t)y * (size_t)f->width * 4;
        for (int x = 0; x < f->width; x++)
            put(dst + x * 4, pal[src[x]]);
    }
}

static void show_rgb(const state *s, const qt_video_format *f, uint8_t *frame)
{
    int stride = s->bw * 4;
    for (int y = 0; y < f->height; y++) {
        const uint16_t *src = s->rgb + (size_t)y * (size_t)stride;
        uint8_t *dst = frame + (size_t)y * (size_t)f->width * 4;
        for (int x = 0; x < f->width; x++)
            put(dst + x * 4, rgb555(src[x]));
    }
}

/* ---- Graphics ('smc ') ----
 *
 * After a flags byte and 24-bit size, opcodes over the blocks in order: the
 * high nibble picks skip, repeat the last block, repeat the last pair, one
 * colour, then 2, 4 and 8 colours (new to the cache, or an entry from it)
 * and 16 colours.  Counts are the low nibble plus one, or for the first
 * four operations with 0x10 set, the next byte plus one.
 */
static bool smc_supports(int depth) { return depth == 8; }

static void *smc_open(const qt_video_format *f)
{
    state *s = new_state(f);
    if (s && !(s->index = calloc((size_t)s->bw * 16, (size_t)s->bh))) {
        free(s);
        return NULL;
    }
    return s;
}

static uint8_t *smc_block(state *s, int b)
{
    return s->index + ((size_t)(b / s->bw) * 4 * (size_t)s->bw + (size_t)(b % s->bw)) * 4;
}

static void smc_copy(state *s, int to, int from)
{
    uint8_t *d = smc_block(s, to), *p = smc_block(s, from);
    for (int y = 0; y < 4; y++)
        memcpy(d + y * s->bw * 4, p + y * s->bw * 4, 4);
}

static bool smc_decode(void *st, const qt_video_format *f, const uint32_t *pal, const uint8_t *data, size_t len,
                       uint8_t *frame)
{
    state *s = st;
    reader r = { data, data + len, false };
    if (!skip(&r, 4))
        return false;
    int total = s->bw * s->bh, b = 0, stride = s->bw * 4;
    int pair_at = 0, quad_at = 0, octet_at = 0;
    bool ok = true;
    while (b < total && ok) {
        unsigned op = get8(&r);
        if (r.bad) {
            ok = false;
            break;
        }
        int n = (int)(op & 0x0F) + 1;
        if (op < 0x80 && (op & 0x10))
            n = (int)get8(&r) + 1;
        if (n > total - b) {
            ok = false;
            break;
        }
        switch (op & 0xE0) {
        case 0x00: /* skip */
            b += n;
            break;
        case 0x20: /* repeat the last block */
            if (b < 1) {
                ok = false;
                break;
            }
            for (int k = 0, from = b - 1; k < n; k++, b++)
                smc_copy(s, b, from);
            break;
        case 0x40: { /* repeat the last pair */
            if (b < 2) {
                ok = false;
                break;
            }
            int first = b - 2;
            n *= 2;
            if (n > total - b) {
                ok = false;
                break;
            }
            for (int k = 0; k < n; k++, b++)
                smc_copy(s, b, first + (k & 1));
            break;
        }
        case 0x60: { /* one colour */
            uint8_t c = (uint8_t)get8(&r);
            for (int k = 0; k < n; k++, b++) {
                uint8_t *d = smc_block(s, b);
                for (int y = 0; y < 4; y++)
                    memset(d + y * stride, c, 4);
            }
            break;
        }
        case 0x80: { /* two colours, sixteen 1-bit flags per block */
            const uint8_t *cs;
            if (!(op & 0x10)) {
                uint8_t *e = s->pairs + pair_at * 2;
                e[0] = (uint8_t)get8(&r);
                e[1] = (uint8_t)get8(&r);
                cs = e;
                pair_at = (pair_at + 1) & 255;
            } else
                cs = s->pairs + get8(&r) * 2;
            for (int k = 0; k < n; k++, b++) {
                unsigned flags = get16(&r);
                uint8_t *d = smc_block(s, b);
                for (int i = 0; i < 16; i++)
                    d[(i >> 2) * stride + (i & 3)] = cs[flags >> (15 - i) & 1];
            }
            break;
        }
        case 0xA0: { /* four colours, 2-bit flags */
            const uint8_t *cs;
            if (!(op & 0x10)) {
                uint8_t *e = s->quads + quad_at * 4;
                for (int i = 0; i < 4; i++)
                    e[i] = (uint8_t)get8(&r);
                cs = e;
                quad_at = (quad_at + 1) & 255;
            } else
                cs = s->quads + get8(&r) * 4;
            for (int k = 0; k < n; k++, b++) {
                unsigned hi = get16(&r), flags = hi << 16 | get16(&r);
                uint8_t *d = smc_block(s, b);
                for (int i = 0; i < 16; i++)
                    d[(i >> 2) * stride + (i & 3)] = cs[flags >> (30 - 2 * i) & 3];
            }
            break;
        }
        case 0xC0: { /* eight colours, 3-bit flags in a shuffled order */
            const uint8_t *cs;
            if (!(op & 0x10)) {
                uint8_t *e = s->octets + octet_at * 8;
                for (int i = 0; i < 8; i++)
                    e[i] = (uint8_t)get8(&r);
                cs = e;
                octet_at = (octet_at + 1) & 255;
            } else
                cs = s->octets + get8(&r) * 8;
            for (int k = 0; k < n; k++, b++) {
                /* Three words hold the top two rows' flags in their high 12
                 * bits and the bottom two rows' in their low nibbles. */
                unsigned v1 = get16(&r), v2 = get16(&r), v3 = get16(&r);
                uint32_t top = (v1 & 0xFFF0) << 8 | v2 >> 4;
                uint32_t bottom = (v3 & 0xFFF0) << 8 | (v1 & 0x0F) << 8 | (v2 & 0x0F) << 4 | (v3 & 0x0F);
                uint8_t *d = smc_block(s, b);
                for (int i = 0; i < 16; i++) {
                    uint32_t flags = i < 8 ? top : bottom;
                    d[(i >> 2) * stride + (i & 3)] = cs[flags >> (21 - 3 * (i & 7)) & 7];
                }
            }
            break;
        }
        case 0xE0: /* sixteen colours */
            if (op & 0x10) {
                ok = false;
                break;
            }
            for (int k = 0; k < n; k++, b++) {
                uint8_t *d = smc_block(s, b);
                for (int i = 0; i < 16; i++)
                    d[(i >> 2) * stride + (i & 3)] = (uint8_t)get8(&r);
            }
            break;
        }
        if (r.bad)
            ok = false;
    }
    show_index(s, f, pal, frame);
    return ok;
}

/* ---- Video ('rpza') ----
 *
 * After 0xE1 and a 24-bit size, opcodes with a count of the low five bits
 * plus one: 0x80 skip, 0xA0 one colour, 0xC0 two colours and their blends
 * with 2-bit indices per pixel.  A byte without the top bit starts a
 * colour: if the next word has its top bit set it is the second of a
 * one-block 0xC0, else the block is 16 raw colours.
 */
static bool rpza_supports(int depth) { return depth == 16; }

static void *rpza_open(const qt_video_format *f)
{
    state *s = new_state(f);
    if (s && !(s->rgb = calloc((size_t)s->bw * 16 * (size_t)s->bh, sizeof *s->rgb))) {
        free(s);
        return NULL;
    }
    return s;
}

static uint16_t *rpza_block(state *s, int b)
{
    return s->rgb + ((size_t)(b / s->bw) * 4 * (size_t)s->bw + (size_t)(b % s->bw)) * 4;
}

static unsigned blend(unsigned a, unsigned b, int sh)
{
    unsigned ca = a >> sh & 31, cb = b >> sh & 31;
    return ((11 * ca + 21 * cb) >> 5) << sh;
}

static bool rpza_decode(void *st, const qt_video_format *f, const uint32_t *pal, const uint8_t *data,
                        size_t len, uint8_t *frame)
{
    (void)pal;
    state *s = st;
    reader r = { data, data + len, false };
    if (!skip(&r, 4))
        return false;
    int total = s->bw * s->bh, b = 0, stride = s->bw * 4;
    bool ok = true;
    while (b < total && r.p < r.end) {
        unsigned op = get8(&r), a = 0, c = 0;
        int n = (int)(op & 0x1F) + 1;
        if (!(op & 0x80)) {
            a = op << 8 | get8(&r);
            if (peek8(&r) & 0x80) {
                op = 0x20;
                n = 1;
            } else
                op = 0x00;
        }
        if (n > total - b)
            n = total - b;
        switch (op & 0xE0) {
        case 0x80:
            b += n;
            break;
        case 0xA0:
            a = get16(&r);
            for (int k = 0; k < n; k++, b++) {
                uint16_t *d = rpza_block(s, b);
                for (int i = 0; i < 16; i++)
                    d[(i >> 2) * stride + (i & 3)] = (uint16_t)a;
            }
            break;
        case 0xC0:
            a = get16(&r);
            /* fall through */
        case 0x20: {
            c = get16(&r);
            uint16_t c4[4] = { (uint16_t)c, (uint16_t)(blend(a, c, 10) | blend(a, c, 5) | blend(a, c, 0)),
                               (uint16_t)(blend(c, a, 10) | blend(c, a, 5) | blend(c, a, 0)), (uint16_t)a };
            for (int k = 0; k < n; k++, b++) {
                uint16_t *d = rpza_block(s, b);
                for (int y = 0; y < 4; y++) {
                    unsigned idx = get8(&r);
                    for (int x = 0; x < 4; x++)
                        d[y * stride + x] = c4[idx >> (6 - 2 * x) & 3];
                }
            }
            break;
        }
        case 0x00: {
            uint16_t *d = rpza_block(s, b++);
            d[0] = (uint16_t)a;
            for (int i = 1; i < 16; i++)
                d[(i >> 2) * stride + (i & 3)] = (uint16_t)get16(&r);
            break;
        }
        default:
            ok = false;
            break;
        }
        if (!ok || r.bad) {
            ok = false;
            break;
        }
    }
    show_rgb(s, f, frame);
    return ok;
}

/* ---- None ('raw ') ----
 *
 * Rows of pixels, high bits first, each row padded; the padding is
 * whatever the sample's size says, as QuickTime's rowBytes varied.
 */
static bool raw_supports(int depth)
{
    switch (depth) {
    case 1: case 2: case 4: case 8: case 16: case 24: case 32: case 33: case 34: case 36: case 40:
        return true;
    default:
        return false;
    }
}

static bool raw_decode(void *s, const qt_video_format *f, const uint32_t *pal, const uint8_t *data, size_t len,
                       uint8_t *frame)
{
    (void)s;
    int bits = bits_of(f->depth);
    size_t row = ((size_t)f->width * (size_t)bits + 7) / 8, stride = len / (size_t)f->height;
    if (stride < row)
        return false;
    for (int y = 0; y < f->height; y++) {
        const uint8_t *src = data + (size_t)y * stride;
        uint8_t *dst = frame + (size_t)y * (size_t)f->width * 4;
        for (int x = 0; x < f->width; x++, dst += 4) {
            switch (bits) {
            case 16:
                put(dst, rgb555((unsigned)src[x * 2] << 8 | src[x * 2 + 1]));
                break;
            case 24:
                put(dst, (uint32_t)src[x * 3] << 16 | (uint32_t)src[x * 3 + 1] << 8 | src[x * 3 + 2]);
                break;
            case 32: /* ARGB */
                put(dst, (uint32_t)src[x * 4 + 1] << 16 | (uint32_t)src[x * 4 + 2] << 8 | src[x * 4 + 3]);
                break;
            default: {
                int bit = x * bits, mask = (1 << bits) - 1;
                put(dst, pal[src[bit >> 3] >> (8 - bits - (bit & 7)) & mask]);
                break;
            }
            }
        }
    }
    return true;
}

const qt_video_codec qt_codec_rle = { 0x726C6520, rle_supports, open_plain, rle_decode, close_ };
const qt_video_codec qt_codec_smc = { 0x736D6320, smc_supports, smc_open, smc_decode, close_ };
const qt_video_codec qt_codec_rpza = { 0x72707A61, rpza_supports, rpza_open, rpza_decode, close_ };
const qt_video_codec qt_codec_raw = { 0x72617720, raw_supports, open_plain, raw_decode, close_ };
