/*
 * Mac OS 8.5 Appearance theme files ('thme'), read on the host.
 *
 * Apple never documented the format; this is what the files turned out to
 * hold (the Mac OS 8.5b6 betas Gizmo, Hi-Tech and Drawing Board, and some
 * two hundred third-party themes, all pure data):
 *
 *  - 'layo' (one per part: 128-143 windows, 256-263 menus, 300-345 tabs,
 *    400-440 scroll bars, sliders and progress bars, 500-600 buttons):
 *    u32 0, u32 coordinate count, coordinates of 16 bytes, u16 record
 *    count, records of 40 bytes.
 *    A coordinate is {kind, value, ref, op, param, ref2, 0, 0} (int16s),
 *    a ref being -1..-4 for the part's rectangle (top, left, bottom, right)
 *    or 1..n for an earlier coordinate.  Kind 0 is ref + value; kind 2 is
 *    the mid-point of ref and ref2, plus value; kind 1 is ref plus or minus
 *    a measure the caller gives (|value| picks it: a window's title width
 *    and height and their halves, a scroll bar's thumb).  Op 1 to 3 then
 *    keep it from passing ref2 + param (upwards for a negative value).
 *    A record is {u32 state bits it needs, u32 state bits it must not have,
 *    u16 0, u16 flags, int16 top, left, bottom, right (refs), u32 0,
 *    OSType what, int16 id, u16 anchor, u16 image index, 6 bytes 0}.
 *    Flags 0x0100 draw it; the low byte names the regions it makes (for a
 *    window: 0x01 title bar, 0x02 title text, 0x04 close box, 0x08 zoom
 *    box, 0x20 structure, 0x40 grow box, 0x80 collapse box; 0x0200 marks
 *    the content).  'what' is 'pxm#' (an image), 'plut' (a fill: a negative
 *    index into plut 387), 'fram' (a frme) or 'NULL' (only a region).
 *    The anchor (1 top left, 2 top right, 3 bottom left, 4 bottom right)
 *    is the corner an image or pattern is pinned to; it is cropped there
 *    or tiled from there to fill the rectangle.
 *  - 'pxm#': u16 version 1, u16 flags (1: one mask for all images), the
 *    bounds rect, u16 pixel size (1-8 indexed, 16 or 32 direct), u16, u32,
 *    u16 clut id, u16 image count; then the 1-bit masks (rows rounded to
 *    16 bits), then the images (rows rounded to 16 bits).
 *  - 'plut' 386 (the theme brushes, kThemeBrush 1-47) and 387 (the
 *    theme's own fills): u32, u16, u16 count, then {int16 ppat id or 0,
 *    RGB} entries.
 *  - 'frme': u16 version (2 or 3), u16, u8 flags, int8 first ring, u8,
 *    int8 last ring (0 is the rectangle's outermost row of pixels, negative
 *    ones outside it), int16 fill for the inside (0 none), u16 count, then
 *    entries {u8 flags, int8 ring, int16 fill,
 *    side masks}: a byte (version 2) or a word (version 3) per side, top,
 *    left, bottom, right, each going round counter-clockwise: the top bit
 *    is the corner the side starts at, then the first pixels, the middle,
 *    and the last pixels.  The flag bytes (the header's and each
 *    entry's) are 0x00, 0x40, 0x80 or 0xC0; in the 219 sample themes the
 *    entries' are set in some thirty third-party themes (Moby, Sosumi,
 *    LUCENT...), none of Apple's, often on a bevel's highlight or shadow
 *    sides with the same fills as unflagged entries.  What they change is
 *    not known (no theme here draws differently with them on a real Mac
 *    that we have seen), so they are ignored.
 *  - 'clr#' 384: the theme text colours (kThemeTextColor 1-46), RGB each.
 *  - 'tinf' 128: a flattened collection of the theme's metrics; 'tdat' 1
 *    lists the resources that make the theme; 'scen' is its desktop
 *    picture; 'crsr'/'acur' its cursors; 'dftb' its dialog fonts.
 *
 * Sound sets are 'tsnd' files whose 'snd#' maps the Appearance sound
 * events to their 'snd ' resources.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "forks.h"
#include "machine.h"
#include "qd_internal.h"
#include "tb_internal.h"
#include "thme.h"

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static const uint8_t *coll_item(const uint8_t *d, uint32_t len, const char *tag, uint32_t *ilen);

/* ---------------------------------------------------------------------- */
/* The theme */

typedef struct {
    int w, h, n;   /* n images of w x h, one after another */
    uint8_t *rgba; /* alpha 0 or 255 */
} timg;

typedef struct {
    int16_t kind, val, ref, op, param, ref2;
} tcoord;

typedef struct {
    uint32_t need, forbid;
    uint16_t flags;
    int16_t rc[4];
    uint32_t what;
    int16_t id;
    uint16_t anchor, index;
} trec;

typedef struct {
    int nc, nr;
    tcoord *c;
    trec *r;
} tlayo;

#define MAX_CACHE 1024

typedef struct {
    uint32_t type;
    int16_t id;
    bool ok;
    timg im;
    tlayo l;
} tcache;

static struct {
    bool on;
    uint8_t *fork;
    size_t len;
    rsrc_index idx;
    char name[64];
    tcache cache[MAX_CACHE];
    int ncache;
} T;

/* The sound set, loaded apart from the theme. */
static struct {
    uint8_t *fork;
    rsrc_index idx;
    char name[64];
} SND;

static const uint8_t *res(uint32_t type, int16_t id, uint32_t *len)
{
    if (!T.fork)
        return NULL;
    for (size_t i = 0; i < T.idx.count; i++) {
        rsrc_entry *e = &T.idx.entries[i];
        if (e->type == type && e->id == id) {
            *len = e->data_len;
            return rsrc_data(T.fork, &T.idx, e);
        }
    }
    return NULL;
}

static void forget(void)
{
    for (int i = 0; i < T.ncache; i++) {
        free(T.cache[i].im.rgba);
        free(T.cache[i].l.c);
        free(T.cache[i].l.r);
    }
    T.ncache = 0;
}

/* Is this resource fork a theme file's?  Every one has layouts. */
bool thme_fork_is_theme(const uint8_t *fork, size_t len)
{
    rsrc_index idx;
    char err[128];
    if (!rsrc_parse(fork, len, &idx, err, sizeof err))
        return false;
    bool yes = false;
    for (size_t i = 0; i < idx.count && !yes; i++)
        yes = idx.entries[i].type == fourcc("layo") && idx.entries[i].id == 128;
    rsrc_index_free(&idx);
    return yes;
}

bool thme_load(const char *path, const uint8_t *fork, size_t len, char *err, size_t errlen)
{
    thme_unload();
    T.fork = malloc(len ? len : 1);
    memcpy(T.fork, fork, len);
    T.len = len;
    if (!rsrc_parse(T.fork, T.len, &T.idx, err, errlen)) {
        free(T.fork);
        T.fork = NULL;
        return false;
    }
    const char *base = strrchr(path, '/');
    snprintf(T.name, sizeof T.name, "%s", base ? base + 1 : path);
    T.on = true;
    return true;
}

void thme_unload(void)
{
    forget();
    if (T.fork) {
        rsrc_index_free(&T.idx);
        free(T.fork);
    }
    memset(&T, 0, sizeof T);
}

bool thme_on(void) { return T.on; }
const char *thme_name(void) { return T.name; }

static tcache *cached(uint32_t type, int16_t id, bool *fresh)
{
    for (int i = 0; i < T.ncache; i++)
        if (T.cache[i].type == type && T.cache[i].id == id) {
            *fresh = false;
            return &T.cache[i];
        }
    if (T.ncache >= MAX_CACHE)
        forget();
    memset(&T.cache[T.ncache], 0, sizeof T.cache[0]);
    T.cache[T.ncache].type = type;
    T.cache[T.ncache].id = id;
    *fresh = true;
    return &T.cache[T.ncache++];
}

/* ---------------------------------------------------------------------- */
/* Images */

/* A 'clut' (or a colour table inside a resource) as 256 RGB triples. */
static void read_ctab(const uint8_t *d, uint32_t len, uint8_t tab[256][3])
{
    memset(tab, 0, 256 * 3);
    if (len < 8)
        return;
    uint16_t flags = be16(d + 4);
    int n = (int16_t)be16(d + 6) + 1;
    for (int i = 0; i < n && 8 + (uint32_t)i * 8 + 8 <= len; i++) {
        const uint8_t *e = d + 8 + i * 8;
        int v = (flags & 0x8000) ? i : be16(e);
        if (v >= 0 && v < 256) {
            tab[v][0] = e[2];
            tab[v][1] = e[4];
            tab[v][2] = e[6];
        }
    }
}

static void std_ctab(int depth, uint8_t tab[256][3])
{
    for (int i = 0; i < 256; i++) {
        uint16_t c[3];
        if (depth == 8)
            qd_std_rgb8(i, c);
        else
            c[0] = c[1] = c[2] = (uint16_t)(i & 1 ? 0 : 0xFFFF); /* black and white */
        for (int k = 0; k < 3; k++)
            tab[i][k] = (uint8_t)(c[k] >> 8);
    }
}

/* One pixel of an image row: an index, or direct 16- or 32-bit RGB. */
static void pixel(const uint8_t *row, int x, int ps, uint8_t tab[256][3], uint8_t *out)
{
    if (ps == 16) {
        int v = be16(row + x * 2);
        out[0] = (uint8_t)(((v >> 10) & 31) * 255 / 31);
        out[1] = (uint8_t)(((v >> 5) & 31) * 255 / 31);
        out[2] = (uint8_t)((v & 31) * 255 / 31);
        return;
    }
    if (ps == 32) {
        out[0] = row[x * 4 + 1];
        out[1] = row[x * 4 + 2];
        out[2] = row[x * 4 + 3];
        return;
    }
    int bit = x * ps;
    int v = (row[bit / 8] >> (8 - ps - bit % 8)) & ((1 << ps) - 1);
    memcpy(out, tab[v], 3);
}

static bool decode_pxm(const uint8_t *d, uint32_t len, timg *out)
{
    if (len < 24 || be16(d) != 1)
        return false;
    uint16_t flags = be16(d + 2);
    int top = (int16_t)be16(d + 4), left = (int16_t)be16(d + 6), bottom = (int16_t)be16(d + 8),
        right = (int16_t)be16(d + 10);
    int ps = be16(d + 12), n = be16(d + 22);
    int16_t clut = (int16_t)be16(d + 20);
    int w = right - left, h = bottom - top;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || n <= 0 || n > 256)
        return false;
    if (ps != 1 && ps != 2 && ps != 4 && ps != 8 && ps != 16 && ps != 32)
        return false;
    uint32_t mrb = (uint32_t)((w + 15) / 16) * 2, rb = (uint32_t)((w * ps + 15) / 16) * 2;
    int nmasks = (flags & 1) ? 1 : n;
    uint32_t need = 24 + mrb * (uint32_t)h * (uint32_t)nmasks + rb * (uint32_t)h * (uint32_t)n;
    if (need > len)
        return false;
    uint8_t tab[256][3];
    uint32_t cl;
    const uint8_t *ct = ps <= 8 ? res(fourcc("clut"), clut, &cl) : NULL;
    if (ct)
        read_ctab(ct, cl, tab);
    else
        std_ctab(ps, tab);
    out->w = w;
    out->h = h;
    out->n = n;
    out->rgba = malloc((size_t)w * (size_t)h * 4 * (size_t)n);
    const uint8_t *masks = d + 24, *pix = masks + mrb * (uint32_t)h * (uint32_t)nmasks;
    for (int k = 0; k < n; k++) {
        const uint8_t *m = masks + ((flags & 1) ? 0 : mrb * (uint32_t)h * (uint32_t)k);
        for (int y = 0; y < h; y++) {
            const uint8_t *row = pix + rb * (uint32_t)(h * k + y);
            for (int x = 0; x < w; x++) {
                uint8_t *p = out->rgba + (((size_t)k * (size_t)h + (size_t)y) * (size_t)w + (size_t)x) * 4;
                pixel(row, x, ps, tab, p);
                p[3] = (m[(uint32_t)y * mrb + (uint32_t)x / 8] >> (7 - x % 8)) & 1 ? 255 : 0;
            }
        }
    }
    return true;
}

/* A 'ppat' as an RGBA tile: its colour pixel map, or its 1-bit pattern. */
static bool decode_ppat(const uint8_t *d, uint32_t len, timg *out)
{
    if (len < 28)
        return false;
    int type = be16(d);
    uint32_t pm = be32(d + 2), pd = be32(d + 6);
    if (type == 1 && pm + 50 <= len) {
        int rb = be16(d + pm + 4) & 0x3FFF;
        int top = (int16_t)be16(d + pm + 6), left = (int16_t)be16(d + pm + 8), bottom = (int16_t)be16(d + pm + 10),
            right = (int16_t)be16(d + pm + 12);
        int ps = (int16_t)be16(d + pm + 32);
        uint32_t ct = be32(d + pm + 42);
        int w = right - left, h = bottom - top;
        if (w > 0 && h > 0 && w <= 512 && h <= 512 && pd + (uint32_t)(rb * h) <= len &&
            (ps == 1 || ps == 2 || ps == 4 || ps == 8 || ps == 16 || ps == 32)) {
            uint8_t tab[256][3];
            if (ps <= 8 && ct && ct < len)
                read_ctab(d + ct, len - ct, tab);
            else
                std_ctab(ps, tab);
            out->w = w;
            out->h = h;
            out->n = 1;
            out->rgba = malloc((size_t)w * (size_t)h * 4);
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    uint8_t *p = out->rgba + ((size_t)y * (size_t)w + (size_t)x) * 4;
                    pixel(d + pd + (uint32_t)(y * rb), x, ps, tab, p);
                    p[3] = 255;
                }
            return true;
        }
    }
    out->w = out->h = 8;
    out->n = 1;
    out->rgba = malloc(8 * 8 * 4);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            uint8_t *p = out->rgba + (y * 8 + x) * 4;
            uint8_t v = (d[20 + y] >> (7 - x)) & 1 ? 0 : 255;
            p[0] = p[1] = p[2] = v;
            p[3] = 255;
        }
    return true;
}

static const timg *image(uint32_t type, int16_t id)
{
    bool fresh;
    tcache *c = cached(type, id, &fresh);
    if (fresh) {
        uint32_t len;
        const uint8_t *d = res(type, id, &len);
        c->ok = d && (type == fourcc("ppat") ? decode_ppat(d, len, &c->im) : decode_pxm(d, len, &c->im));
    }
    return c->ok ? &c->im : NULL;
}

/* ---------------------------------------------------------------------- */
/* Fills: the theme's own (plut 387, negative indices) and its brushes
 * (plut 386, kThemeBrush 1-47) */

typedef struct {
    uint8_t rgb[3];
    const timg *pat;
} tfill;

static bool plut_entry(int16_t plut, int index, tfill *f)
{
    uint32_t len;
    const uint8_t *d = res(fourcc("plut"), plut, &len);
    if (!d || len < 8 || index < 0 || index >= be16(d + 6) || 8 + (uint32_t)index * 8 + 8 > len)
        return false;
    const uint8_t *e = d + 8 + index * 8;
    int16_t pat = (int16_t)be16(e);
    f->rgb[0] = e[2];
    f->rgb[1] = e[4];
    f->rgb[2] = e[6];
    f->pat = pat ? image(fourcc("ppat"), pat) : NULL;
    return true;
}

static bool fill_of(int16_t id, tfill *f)
{
    if (id < 0)
        return plut_entry(387, -id - 1, f);
    return id > 0 && plut_entry(386, id - 1, f);
}

bool thme_brush_rgb(int brush, uint16_t out[3])
{
    tfill f;
    if (!T.on || brush < 1 || !plut_entry(386, brush - 1, &f))
        return false;
    if (f.pat) { /* a pattern's colour: its first pixel */
        for (int i = 0; i < 3; i++)
            out[i] = (uint16_t)(f.pat->rgba[i] * 0x101);
        return true;
    }
    for (int i = 0; i < 3; i++)
        out[i] = (uint16_t)(f.rgb[i] * 0x101);
    return true;
}

bool thme_text_rgb(int color, uint16_t out[3])
{
    uint32_t len;
    const uint8_t *d = T.on ? res(fourcc("clr#"), 384, &len) : NULL;
    if (!d || len < 4 || color < 1 || color > (int)be16(d + 2) || 4 + (uint32_t)color * 6 > len)
        return false;
    const uint8_t *e = d + 4 + (color - 1) * 6;
    for (int i = 0; i < 3; i++)
        out[i] = be16(e + i * 2);
    return true;
}

/* ---------------------------------------------------------------------- */
/* Drawing surfaces: the current port (through its clip), an RGBA buffer
 * (bluebox themes --render), or a mask of what would be drawn (regions). */

typedef struct {
    int kind; /* 0 port, 1 buffer, 2 mask */
    pixdst d;
    hrgn clip;
    uint8_t *buf;
    qrect bounds; /* the buffer's or mask's, in drawing coordinates */
} surface;

static surface *S;

static void plot(int x, int y, const uint8_t *rgb)
{
    if (S->kind != 0) {
        if (x < S->bounds.left || x >= S->bounds.right || y < S->bounds.top || y >= S->bounds.bottom)
            return;
        size_t i = (size_t)(y - S->bounds.top) * (size_t)(S->bounds.right - S->bounds.left) + (size_t)(x - S->bounds.left);
        if (S->kind == 2) {
            S->buf[i] = 1;
            return;
        }
        memcpy(S->buf + i * 4, rgb, 3);
        S->buf[i * 4 + 3] = 255;
        return;
    }
    if (x < S->d.bounds.left || x >= S->d.bounds.right || y < S->d.bounds.top || y >= S->d.bounds.bottom)
        return;
    int n;
    const int16_t *xs = hrgn_row(&S->clip, y, &n);
    bool in = false;
    for (int k = 0; xs && k < n && !in; k += 2)
        in = x >= xs[k] && x < xs[k + 1];
    if (!in)
        return;
    uint16_t c[3] = { (uint16_t)(rgb[0] * 0x101), (uint16_t)(rgb[1] * 0x101), (uint16_t)(rgb[2] * 0x101) };
    put_px(&S->d, x - S->d.bounds.left, y - S->d.bounds.top, qd_rgb_px(S->d.depth, S->d.ctab, c));
}

static void port_surface(surface *s)
{
    memset(s, 0, sizeof *s);
    s->d = qd_port_pixels(qd_port());
    pixdst_clamp(&s->d);
    hrgn_init(&s->clip);
    qd_port_clip(qd_port(), &s->clip);
}

/* Where in an image or pattern of size n a pixel at p of [a, b) falls,
 * pinned to the start or the end. */
static int wrap(int p, int a, int b, int n, bool from_end)
{
    int k = from_end ? (b - 1 - p) % n : (p - a) % n;
    return from_end ? n - 1 - k : k;
}

static void draw_tile(const timg *im, int frame, qrect r, int anchor)
{
    if (!im || r.right <= r.left || r.bottom <= r.top)
        return;
    if (frame < 0 || frame >= im->n)
        frame = 0;
    bool right = anchor == 2 || anchor == 4, bottom = anchor == 3 || anchor == 4;
    const uint8_t *base = im->rgba + (size_t)frame * (size_t)im->w * (size_t)im->h * 4;
    for (int y = r.top; y < r.bottom; y++) {
        int sy = wrap(y, r.top, r.bottom, im->h, bottom);
        for (int x = r.left; x < r.right; x++) {
            const uint8_t *p = base + ((size_t)sy * (size_t)im->w + (size_t)wrap(x, r.left, r.right, im->w, right)) * 4;
            if (p[3])
                plot(x, y, p);
        }
    }
}

static void draw_fill(int16_t id, qrect r, int anchor)
{
    tfill f;
    if (!fill_of(id, &f))
        return;
    if (f.pat) {
        draw_tile(f.pat, 0, r, anchor);
        return;
    }
    for (int y = r.top; y < r.bottom; y++)
        for (int x = r.left; x < r.right; x++)
            plot(x, y, f.rgb);
}

/* ---------------------------------------------------------------------- */
/* Frames ('frme'): rings of pixels round a rectangle */

static void draw_frame(int16_t id, qrect r)
{
    uint32_t len;
    const uint8_t *d = res(fourcc("frme"), id, &len);
    if (!d || len < 12)
        return;
    int ver = be16(d);
    int esize = ver >= 3 ? 12 : 8, n = be16(d + 10);
    /* The inside, past the last ring, in the frame's own fill (patterns
     * pinned to its bottom: the betas' button faces are tall gradients). */
    int last = (int8_t)d[7];
    tfill in;
    if ((int16_t)be16(d + 8) && fill_of((int16_t)be16(d + 8), &in)) {
        qrect q = { (int16_t)(r.top + last + 1), (int16_t)(r.left + last + 1), (int16_t)(r.bottom - last - 1),
                    (int16_t)(r.right - last - 1) };
        if (q.right > q.left && q.bottom > q.top)
            draw_fill((int16_t)be16(d + 8), q, 3);
    }
    for (int i = 0; i < n && 12 + (uint32_t)(i + 1) * (uint32_t)esize <= len; i++) {
        const uint8_t *e = d + 12 + i * esize;
        int ring = (int8_t)e[1];
        int16_t fid = (int16_t)be16(e + 2);
        tfill f;
        if (!fill_of(fid, &f))
            continue;
        qrect q = { (int16_t)(r.top + ring), (int16_t)(r.left + ring), (int16_t)(r.bottom - ring), (int16_t)(r.right - ring) };
        int w = q.right - q.left, h = q.bottom - q.top;
        if (w < 1 || h < 1)
            continue;
        int bits = ver >= 3 ? 16 : 8, edge = (bits - 2) / 2; /* pixels told apart at each end */
        for (int side = 0; side < 4; side++) {
            uint32_t m = ver >= 3 ? be16(e + 4 + side * 2) : e[4 + side];
            if (!m)
                continue;
            /* Counter-clockwise: top from the right, left from the top,
             * bottom from the left, right from the bottom. */
            int len_side = (side & 1) ? h - 2 : w - 2;
            for (int k = -1; k < len_side; k++) {
                int bit;
                if (k < 0)
                    bit = bits - 1;
                else if (k < edge && k < len_side - edge)
                    bit = bits - 2 - k;
                else if (k >= len_side - edge && len_side - 1 - k < edge)
                    bit = len_side - 1 - k;
                else
                    bit = edge;
                if (!(m >> bit & 1))
                    continue;
                int x, y;
                switch (side) {
                case 0: x = q.right - 1 - (k + 1); y = q.top; break;
                case 1: x = q.left; y = q.top + (k + 1); break;
                case 2: x = q.left + (k + 1); y = q.bottom - 1; break;
                default: x = q.right - 1; y = q.bottom - 1 - (k + 1); break;
                }
                const uint8_t *c = f.rgb;
                if (f.pat)
                    c = f.pat->rgba + (((size_t)(y & 0x7FFF) % (size_t)f.pat->h) * (size_t)f.pat->w +
                                      (size_t)(x & 0x7FFF) % (size_t)f.pat->w) * 4;
                plot(x, y, c);
            }
        }
    }
}

bool thme_frame(int id, qrect r)
{
    uint32_t len;
    if (!T.on || !res(fourcc("frme"), (int16_t)id, &len))
        return false;
    surface s;
    port_surface(&s);
    surface *old = S;
    S = &s;
    draw_frame((int16_t)id, r);
    S = old;
    hrgn_free(&s.clip);
    return true;
}

/* ---------------------------------------------------------------------- */
/* Layouts */

static const tlayo *layout(int16_t id)
{
    bool fresh;
    tcache *c = cached(fourcc("layo"), id, &fresh);
    if (fresh) {
        uint32_t len;
        const uint8_t *d = res(fourcc("layo"), id, &len);
        if (d && len >= 10) {
            uint32_t nc = be32(d + 4);
            uint32_t o = 8 + nc * 16;
            if (nc < 4096 && o + 2 <= len) {
                uint32_t nr = be16(d + o);
                if (o + 2 + nr * 40 <= len) {
                    c->l.nc = (int)nc;
                    c->l.nr = (int)nr;
                    c->l.c = calloc(nc + 1, sizeof(tcoord));
                    c->l.r = calloc(nr + 1, sizeof(trec));
                    for (uint32_t i = 0; i < nc; i++) {
                        const uint8_t *e = d + 8 + i * 16;
                        c->l.c[i] = (tcoord){ (int16_t)be16(e), (int16_t)be16(e + 2), (int16_t)be16(e + 4),
                                              (int16_t)be16(e + 6), (int16_t)be16(e + 8), (int16_t)be16(e + 10) };
                    }
                    for (uint32_t i = 0; i < nr; i++) {
                        const uint8_t *e = d + o + 2 + i * 40;
                        trec *t = &c->l.r[i];
                        t->need = be32(e);
                        t->forbid = be32(e + 4);
                        t->flags = be16(e + 10);
                        for (int k = 0; k < 4; k++)
                            t->rc[k] = (int16_t)be16(e + 12 + k * 2);
                        t->what = be32(e + 24);
                        t->id = (int16_t)be16(e + 28);
                        t->anchor = be16(e + 30);
                        t->index = be16(e + 32);
                    }
                    c->ok = true;
                }
            }
        }
    }
    return c->ok ? &c->l : NULL;
}

/* The coordinates of a layout for a rectangle and measures. */
static int *evaluate(const tlayo *l, qrect b, const int *m)
{
    int *v = malloc(sizeof(int) * (size_t)(l->nc + 1));
#define REF(i) ((i) == -1 ? b.top : (i) == -2 ? b.left : (i) == -3 ? b.bottom : (i) == -4 ? b.right : \
                (i) >= 1 && (i) <= l->nc ? v[i] : 0)
    for (int i = 0; i < l->nc; i++) {
        const tcoord *c = &l->c[i];
        int x;
        if (c->kind == 2) {
            int a = REF(c->ref), z = REF(c->ref2);
            x = (a + z) / 2 + c->val;
        } else if (c->kind == 1) {
            int k = c->val < 0 ? -c->val : c->val;
            int mv = k < THME_MEASURES ? m[k] : 0;
            x = REF(c->ref) + (c->val < 0 ? -mv : mv);
        } else {
            x = REF(c->ref) + c->val;
        }
        if (c->kind != 2 && c->op >= 1 && c->op <= 3) {
            int lim = REF(c->ref2) + c->param;
            x = c->val < 0 ? imax(x, lim) : imin(x, lim);
        }
        v[i + 1] = x;
    }
    return v;
}

static qrect rec_rect(const tlayo *l, const int *v, qrect b, const trec *t)
{
    int q[4];
    for (int k = 0; k < 4; k++) {
        int i = t->rc[k];
        q[k] = REF(i);
    }
#undef REF
    return (qrect){ (int16_t)q[0], (int16_t)q[1], (int16_t)q[2], (int16_t)q[3] };
}

static bool applies(const trec *t, uint32_t state) { return (state & t->need) == t->need && !(state & t->forbid); }

bool thme_has_part(int id) { return T.on && layout((int16_t)id) != NULL; }

/* Draw a part: every record that applies to the state, in order.  Images
 * of records making a region in `pressed` show their next image. */
static bool draw_part_on(int id, qrect b, uint32_t state, const int *m, uint32_t pressed, uint32_t only)
{
    const tlayo *l = layout((int16_t)id);
    if (!l)
        return false;
    int *v = evaluate(l, b, m);
    for (int i = 0; i < l->nr; i++) {
        const trec *t = &l->r[i];
        if (!(t->flags & 0x100) || !applies(t, state) || (only && !(t->flags & only)))
            continue;
        qrect r = rec_rect(l, v, b, t);
        if (r.right <= r.left || r.bottom <= r.top)
            continue;
        if (t->what == fourcc("pxm#")) {
            const timg *im = image(fourcc("pxm#"), t->id);
            int k = t->index + ((t->flags & 0xFF & pressed) ? 1 : 0);
            if (im && k >= im->n)
                k = t->index;
            draw_tile(im, k, r, t->anchor);
        } else if (t->what == fourcc("plut")) {
            draw_fill(t->id, r, t->anchor);
        } else if (t->what == fourcc("fram")) {
            draw_frame(t->id, r);
        }
    }
    free(v);
    return true;
}

bool thme_draw_part(int id, qrect b, uint32_t state, const int *m, uint32_t pressed)
{
    if (!T.on || !layout((int16_t)id))
        return false;
    surface s;
    port_surface(&s);
    surface *old = S;
    S = &s;
    draw_part_on(id, b, state, m, pressed, 0);
    S = old;
    hrgn_free(&s.clip);
    return true;
}

/* The rectangle of the last record (that applies) making any of the
 * regions in `which`; false if none. */
bool thme_part_rect(int id, qrect b, uint32_t state, const int *m, uint32_t which, qrect *out)
{
    const tlayo *l = T.on ? layout((int16_t)id) : NULL;
    if (!l)
        return false;
    int *v = evaluate(l, b, m);
    bool found = false;
    for (int i = 0; i < l->nr; i++) {
        const trec *t = &l->r[i];
        if (!applies(t, state) || !((t->flags & 0x2FF) & which))
            continue;
        qrect r = rec_rect(l, v, b, t);
        if (r.right <= r.left || r.bottom <= r.top)
            continue;
        *out = r;
        found = true;
    }
    free(v);
    return found;
}

/* The regions (of the bits in `which`) a point is in. */
uint32_t thme_part_hit(int id, qrect b, uint32_t state, const int *m, uint32_t which, int h, int vv)
{
    const tlayo *l = T.on ? layout((int16_t)id) : NULL;
    if (!l)
        return 0;
    int *v = evaluate(l, b, m);
    uint32_t hit = 0;
    for (int i = 0; i < l->nr; i++) {
        const trec *t = &l->r[i];
        if (!applies(t, state) || !(t->flags & which))
            continue;
        qrect r = rec_rect(l, v, b, t);
        if (h >= r.left && h < r.right && vv >= r.top && vv < r.bottom)
            hit |= t->flags & which;
    }
    free(v);
    return hit;
}

/* Every pixel the part draws, plus the rectangles of the records making
 * the regions in `also` (the content), as a region. */
bool thme_part_region(int id, qrect b, uint32_t state, const int *m, uint32_t also, hrgn *out)
{
    const tlayo *l = T.on ? layout((int16_t)id) : NULL;
    if (!l)
        return false;
    int *v = evaluate(l, b, m);
    qrect bb = b;
    for (int i = 0; i < l->nr; i++) {
        const trec *t = &l->r[i];
        if (!applies(t, state))
            continue;
        qrect r = rec_rect(l, v, b, t);
        if (r.right <= r.left || r.bottom <= r.top)
            continue;
        if (t->what == fourcc("fram")) { /* rings may lie outside */
            r.top -= 8;
            r.left -= 8;
            r.bottom += 8;
            r.right += 8;
        }
        bb.top = (int16_t)imin(bb.top, r.top);
        bb.left = (int16_t)imin(bb.left, r.left);
        bb.bottom = (int16_t)imax(bb.bottom, r.bottom);
        bb.right = (int16_t)imax(bb.right, r.right);
    }
    int w = bb.right - bb.left, hgt = bb.bottom - bb.top;
    hrgn_init(out);
    if (w <= 0 || hgt <= 0 || (size_t)w * (size_t)hgt > 16u << 20) {
        free(v);
        return false;
    }
    surface s = { .kind = 2, .bounds = bb, .buf = calloc((size_t)w * (size_t)hgt, 1) };
    surface *old = S;
    S = &s;
    draw_part_on(id, b, state, m, 0, 0);
    for (int i = 0; i < l->nr; i++) {
        const trec *t = &l->r[i];
        if (!applies(t, state) || !((t->flags & 0x2FF) & also))
            continue;
        qrect r = rec_rect(l, v, b, t);
        for (int y = imax(r.top, bb.top); y < imin(r.bottom, bb.bottom); y++)
            memset(s.buf + (size_t)(y - bb.top) * (size_t)w + (size_t)(imax(r.left, bb.left) - bb.left), 1,
                   (size_t)imax(0, imin(r.right, bb.right) - imax(r.left, bb.left)));
    }
    S = old;
    int16_t *runs = malloc(sizeof(int16_t) * (size_t)(w + 2));
    for (int y = 0; y < hgt; y++) {
        int n = 0;
        const uint8_t *row = s.buf + (size_t)y * (size_t)w;
        for (int x = 0; x < w;) {
            if (!row[x]) {
                x++;
                continue;
            }
            int x0 = x;
            while (x < w && row[x])
                x++;
            runs[n++] = (int16_t)(bb.left + x0);
            runs[n++] = (int16_t)(bb.left + x);
        }
        if (n)
            hrgn_add_runs(out, bb.top + y, runs, n);
    }
    free(runs);
    free(s.buf);
    free(v);
    return true;
}

/* ---------------------------------------------------------------------- */
/* Off-screen rendering, for bluebox themes --render and the tests */

void thme_render_part(uint8_t *rgba, int w, int h, int id, qrect b, uint32_t state, const int *m, uint32_t pressed)
{
    surface s = { .kind = 1, .bounds = { 0, 0, (int16_t)h, (int16_t)w }, .buf = rgba };
    surface *old = S;
    S = &s;
    draw_part_on(id, b, state, m, pressed, 0);
    S = old;
}

void thme_render_image(uint8_t *rgba, int w, int h, uint32_t type, int16_t id, int frame, int x, int y)
{
    const timg *im = image(type, id);
    if (!im)
        return;
    surface s = { .kind = 1, .bounds = { 0, 0, (int16_t)h, (int16_t)w }, .buf = rgba };
    surface *old = S;
    S = &s;
    draw_tile(im, frame, (qrect){ (int16_t)y, (int16_t)x, (int16_t)(y + im->h), (int16_t)(x + im->w) }, 1);
    S = old;
}

bool thme_image_size(uint32_t type, int16_t id, int *w, int *h, int *n)
{
    const timg *im = image(type, id);
    if (!im)
        return false;
    *w = im->w;
    *h = im->h;
    *n = im->n;
    return true;
}

/* ---------------------------------------------------------------------- */
/* The dump: what the file holds, part by part */

static const char *res_name(uint32_t type, int16_t id)
{
    for (size_t i = 0; i < T.idx.count; i++)
        if (T.idx.entries[i].type == type && T.idx.entries[i].id == id)
            return T.idx.entries[i].name;
    return "";
}

/* A metric of 'tinf' 128 (a flattened collection of 16-bit values) by
 * its tag: 'mbht' the menu bar's height, 'lvth'/'lhtw' and 'svth'/'shtw'
 * the scroll bars' widths, and so on (thme_dump lists them). */
bool thme_metric(const char *tag, int *out)
{
    uint32_t len, il;
    const uint8_t *d = T.on ? res(fourcc("tinf"), 128, &len) : NULL;
    const uint8_t *v = d ? coll_item(d, len, tag, &il) : NULL;
    if (!v || il != 2)
        return false;
    *out = (int16_t)be16(v);
    return true;
}

static void dump_metrics(FILE *f)
{
    uint32_t len;
    const uint8_t *d = res(fourcc("tinf"), 128, &len);
    if (!d || len < 12)
        return;
    uint32_t n = be32(d + 8);
    uint32_t data = 12 + n * 16;
    fprintf(f, "metrics (tinf 128):");
    for (uint32_t i = 0; i < n && 12 + i * 16 + 16 <= len; i++) {
        const uint8_t *e = d + 12 + i * 16;
        uint32_t off = be32(e + 12);
        if (data + off + 6 > len)
            continue;
        uint32_t sz = be32(d + data + off);
        char tag[5];
        fourcc_str(be32(e), tag);
        if (sz == 2)
            fprintf(f, " %s=%d", tag, (int16_t)be16(d + data + off + 4));
        else
            fprintf(f, " %s=(%u bytes)", tag, sz);
    }
    fprintf(f, "\n");
}

/* The scenes: each item's tag and size, strings and 16-bit values shown. */
static void dump_scenes(FILE *f)
{
    for (size_t i = 0; i < T.idx.count; i++) {
        rsrc_entry *e = &T.idx.entries[i];
        if (e->type != fourcc("scen"))
            continue;
        const uint8_t *d = rsrc_data(T.fork, &T.idx, e);
        uint32_t len = e->data_len;
        if (len < 12)
            continue;
        uint32_t n = be32(d + 8), base = 12 + n * 16;
        fprintf(f, "scen %d \"%s\":", e->id, e->name);
        for (uint32_t k = 0; k < n && 12 + k * 16 + 16 <= len; k++) {
            const uint8_t *x = d + 12 + k * 16;
            uint32_t off = base + be32(x + 12);
            char tag[5];
            fourcc_str(be32(x), tag);
            if (off + 4 > len)
                continue;
            uint32_t sz = be32(d + off);
            const uint8_t *v = d + off + 4;
            if (off + 4 + sz > len)
                continue;
            fprintf(f, " %s", tag);
            if (sz == 2)
                fprintf(f, "=%d", (int16_t)be16(v));
            else if (sz == 4)
                fprintf(f, "=%08x", be32(v));
            else if (sz == 6)
                fprintf(f, "=%04x%04x%04x", be16(v), be16(v + 2), be16(v + 4));
            else if (sz && v[0] + 1u == sz)
                fprintf(f, "=\"%.*s\"", v[0], v + 1);
            else
                fprintf(f, "=(%u bytes)", sz);
        }
        fprintf(f, "\n");
    }
}

void thme_dump(FILE *f, bool coords)
{
    if (!T.on)
        return;
    fprintf(f, "theme %s\n", T.name);
    dump_metrics(f);
    dump_scenes(f);
    char ts[5];
    for (size_t i = 0; i < T.idx.count; i++) {
        rsrc_entry *e = &T.idx.entries[i];
        if (e->type != fourcc("layo"))
            continue;
        const tlayo *l = layout(e->id);
        if (!l) {
            fprintf(f, "layo %d \"%s\": unreadable\n", e->id, e->name);
            continue;
        }
        fprintf(f, "layo %d \"%s\": %d coordinates, %d records\n", e->id, e->name, l->nc, l->nr);
        if (coords)
            for (int k = 0; k < l->nc; k++) {
                const tcoord *c = &l->c[k];
                fprintf(f, "  c%d = ", k + 1);
                if (c->kind == 2)
                    fprintf(f, "mid(%d, %d) + %d", c->ref, c->ref2, c->val);
                else if (c->kind == 1)
                    fprintf(f, "%d %c measure %d", c->ref, c->val < 0 ? '-' : '+', c->val < 0 ? -c->val : c->val);
                else
                    fprintf(f, "%d + %d", c->ref, c->val);
                if (c->kind != 2 && c->op)
                    fprintf(f, ", op %d to %d + %d", c->op, c->ref2, c->param);
                fprintf(f, "\n");
            }
        for (int k = 0; k < l->nr; k++) {
            const trec *t = &l->r[k];
            fprintf(f, "  r%d needs %04x not %04x flags %04x rect (%d %d %d %d) %s %d", k, t->need, t->forbid, t->flags,
                    t->rc[0], t->rc[1], t->rc[2], t->rc[3], fourcc_str(t->what, ts), t->id);
            if (t->what == fourcc("pxm#") || t->what == fourcc("plut"))
                fprintf(f, " anchor %d", t->anchor);
            if (t->what == fourcc("pxm#")) {
                const timg *im = image(fourcc("pxm#"), t->id);
                fprintf(f, " image %d", t->index);
                if (im)
                    fprintf(f, " of %d (%dx%d)", im->n, im->w, im->h);
                else
                    fprintf(f, " (missing)");
                fprintf(f, " \"%s\"", res_name(t->what, t->id));
            } else if (t->what == fourcc("plut")) {
                tfill fl;
                if (fill_of(t->id, &fl)) {
                    if (fl.pat)
                        fprintf(f, " = pattern %dx%d", fl.pat->w, fl.pat->h);
                    else
                        fprintf(f, " = rgb %02x%02x%02x", fl.rgb[0], fl.rgb[1], fl.rgb[2]);
                }
            } else if (t->what == fourcc("fram")) {
                fprintf(f, " \"%s\"", res_name(fourcc("frme"), t->id));
            }
            fprintf(f, "\n");
        }
    }
    for (size_t i = 0; i < T.idx.count; i++) {
        rsrc_entry *e = &T.idx.entries[i];
        if (e->type != fourcc("frme"))
            continue;
        const uint8_t *d = rsrc_data(T.fork, &T.idx, e);
        if (e->data_len < 12)
            continue;
        int ver = be16(d), n = be16(d + 10), es = ver >= 3 ? 12 : 8;
        fprintf(f, "frme %d \"%s\": version %d, flags %02x, rings %d to %d, fill %d, %d entries:", e->id, e->name, ver,
                d[4], (int8_t)d[5], (int8_t)d[7], (int16_t)be16(d + 8), n);
        for (int k = 0; k < n && 12 + (uint32_t)(k + 1) * (uint32_t)es <= e->data_len; k++) {
            const uint8_t *x = d + 12 + k * es;
            fprintf(f, " [%d %d", (int8_t)x[1], (int16_t)be16(x + 2));
            if (x[0])
                fprintf(f, " flags %02x", x[0]);
            for (int s = 0; s < 4; s++)
                fprintf(f, ver >= 3 ? " %04x" : " %02x", ver >= 3 ? be16(x + 4 + s * 2) : x[4 + s]);
            fprintf(f, "]");
        }
        fprintf(f, "\n");
    }
    for (int pl = 386; pl <= 387; pl++) {
        uint32_t len;
        const uint8_t *d = res(fourcc("plut"), (int16_t)pl, &len);
        if (!d || len < 8)
            continue;
        int n = be16(d + 6);
        fprintf(f, "plut %d (%s): %d entries:", pl, pl == 386 ? "brushes 1.." : "fills -1..", n);
        for (int k = 0; k < n && 8 + (uint32_t)k * 8 + 8 <= len; k++) {
            const uint8_t *x = d + 8 + k * 8;
            if (be16(x))
                fprintf(f, " %d=ppat %d", pl == 386 ? k + 1 : -(k + 1), (int16_t)be16(x));
            else
                fprintf(f, " %d=%02x%02x%02x", pl == 386 ? k + 1 : -(k + 1), x[2], x[4], x[6]);
        }
        fprintf(f, "\n");
    }
}

/* ---------------------------------------------------------------------- */
/* Windows */

typedef struct {
    int id;          /* the active layout; the inactive one follows it */
    int text_color;  /* kThemeTextColor for the active title */
    bool small;      /* Geneva 9 titles (utility windows) */
} wkind;

static bool window_kind(int var, wkind *k)
{
    int v = var & ~0x200;
    if (v & 0x100) { /* WDEF 124's floating windows: title on top, or at the side */
        *k = (wkind){ v & 8 ? 138 : 136, 27, true };
        return true;
    }
    switch (v) {
    case 0: case 4: case 8: case 12: *k = (wkind){ 128, 23, false }; return true;
    case 1: *k = (wkind){ 130, 1, false }; return true;  /* dBoxProc */
    case 3: *k = (wkind){ 132, 3, false }; return true;  /* altDBoxProc */
    case 5: *k = (wkind){ 134, 25, false }; return true; /* movableDBoxProc */
    default: return false;
    }
}

/* Utility windows' titles are in the small system font (Geneva, unless
 * the theme's scene names another that is here). */
static int small_family(void)
{
    int fam = thme_scene_font("smsf", NULL, 0);
    return fam > 0 ? fam : 3;
}

/* The title, and its size in the title's font (measures 1-4: the width,
 * half of it, the height, half of it). */
static int window_title(gaddr w, const wkind *k, uint8_t *s, int *m)
{
    memset(m, 0, sizeof(int) * THME_MEASURES);
    gaddr th = w ? WindowRecord_titleHandle(w) : 0;
    int n = th && rd32(th) ? rd8(rd32(th)) : 0;
    if (n)
        gget(s, rd32(th) + 1, (uint32_t)n);
    gaddr port = qd_port();
    int16_t f = GrafPort_txFont(port), sz = GrafPort_txSize(port);
    uint8_t face = GrafPort_txFace(port);
    GrafPort_set_txFont(port, k->small ? small_family() : 0);
    GrafPort_set_txSize(port, k->small ? 9 : 12);
    GrafPort_set_txFace(port, 0);
    int tw = n ? qd_text_width(port, s, n) : 0;
    gaddr fi = tb_NewPtr(8, false, false);
    tb_GetFontInfo(fi);
    int th_ = (int16_t)rd16(fi) + (int16_t)rd16(fi + 2);
    tb_DisposePtr(fi);
    GrafPort_set_txFont(port, f);
    GrafPort_set_txSize(port, sz);
    GrafPort_set_txFace(port, face);
    m[1] = tw;
    m[2] = (tw + 1) / 2;
    m[3] = th_;
    m[4] = (th_ + 1) / 2;
    return n;
}

/* The system's script runs right to left (TESysJust -1): windows,
 * check boxes, radio buttons and pop-up buttons are drawn mirrored, from
 * the theme's own right-to-left layouts and images. */
static bool right_to_left(void) { return (int16_t)rd16(0x0BAC) == -1; }

static uint32_t window_state(int var, bool goaway, bool zoom, bool titled)
{
    int v = var & ~0x200;
    uint32_t st = 0;
    bool doc = !(v & 0x100) && (v == 0 || v == 4 || v == 8 || v == 12);
    if (doc ? (v == 0 || v == 8) : (v & 0x100) && (v & 2))
        st |= 0x01; /* grow box */
    if (zoom)
        st |= 0x18; /* full zoom */
    if (goaway)
        st |= 0x20;
    if (doc || (v & 0x100))
        st |= 0x40; /* collapse box */
    if (titled)
        st |= 0x80;
    if (var & 0x200)
        st |= 0x100;
    return st;
}

bool thme_draw_window(gaddr w, int var, qrect c, bool hilited, bool goaway, bool zoom, uint32_t pressed)
{
    wkind k;
    if (!T.on || !window_kind(var, &k))
        return false;
    int id = k.id + (hilited ? 0 : 1);
    if (!layout((int16_t)id))
        return false;
    uint8_t s[256];
    int m[THME_MEASURES];
    int n = window_title(w, &k, s, m);
    uint32_t st = window_state(var, goaway, zoom, n > 0) | (right_to_left() ? 0x02 : 0);
    thme_draw_part(id, c, st, m, pressed);
    qrect tr;
    if (n && thme_part_rect(id, c, st, m, THME_TITLE_TEXT, &tr)) {
        gaddr port = qd_port();
        GrafPort_set_txFont(port, k.small ? small_family() : 0);
        GrafPort_set_txSize(port, k.small ? 9 : 12);
        GrafPort_set_txFace(port, 0);
        GrafPort_set_txMode(port, 1);
        gaddr fi = tb_NewPtr(8, false, false);
        tb_GetFontInfo(fi);
        int asc = (int16_t)rd16(fi), desc = (int16_t)rd16(fi + 2);
        tb_DisposePtr(fi);
        int tw = m[1];
        /* Too wide for its place: cut short with an ellipsis. */
        while (n > 1 && tw > tr.right - tr.left) {
            s[n - 2] = 0xC9; /* … */
            n--;
            tw = qd_text_width(port, s, n);
        }
        tb_MoveTo((int16_t)(tr.left + (tr.right - tr.left - tw) / 2),
                  (int16_t)(tr.top + (tr.bottom - tr.top - asc - desc) / 2 + asc));
        uint16_t col[3] = { 0, 0, 0 };
        if (!thme_text_rgb(k.text_color + (hilited ? 0 : 1), col) && !hilited)
            col[0] = col[1] = col[2] = 0x7777;
        qd_set_fg_override(col);
        qd_draw_text(port, s, n);
        qd_set_fg_override(NULL);
    }
    return true;
}

bool thme_window_region(gaddr w, int var, qrect c, bool goaway, bool zoom, hrgn *out)
{
    wkind k;
    if (!T.on || !window_kind(var, &k) || !layout((int16_t)k.id))
        return false;
    uint8_t s[256];
    int m[THME_MEASURES];
    int n = window_title(w, &k, s, m);
    return thme_part_region(k.id, c, window_state(var, goaway, zoom, n > 0) | (right_to_left() ? 0x02 : 0), m, THME_CONTENT,
                            out);
}

/* 1 close box, 2 zoom box, 3 collapse box, 4 title bar or frame, 0 none, -1
 * no theme. */
int thme_window_hit(gaddr w, int var, qrect c, bool goaway, bool zoom, int h, int v)
{
    wkind k;
    if (!T.on || !window_kind(var, &k) || !layout((int16_t)k.id))
        return -1;
    uint8_t s[256];
    int m[THME_MEASURES];
    int n = window_title(w, &k, s, m);
    uint32_t hit =
        thme_part_hit(k.id, c, window_state(var, goaway, zoom, n > 0) | (right_to_left() ? 0x02 : 0), m, 0xFF, h, v);
    if (hit & THME_CLOSE)
        return 1;
    if (hit & THME_ZOOM)
        return 2;
    if (hit & THME_COLLAPSE)
        return 3;
    if (hit) {
        hrgn r;
        if (!thme_window_region(w, var, c, goaway, zoom, &r))
            return 4;
        bool in = hrgn_contains(&r, h, v);
        hrgn_free(&r);
        return in ? 4 : 0;
    }
    return 0;
}

/* DrawGrowIcon: the document window's grow box, in the port's clip. */
bool thme_draw_grow(qrect c, bool active)
{
    int id = active ? 128 : 129;
    if (!T.on || !layout((int16_t)id))
        return false;
    int m[THME_MEASURES] = { 0 };
    surface s;
    port_surface(&s);
    surface *old = S;
    S = &s;
    draw_part_on(id, c, 0x01 | 0x18 | 0x20 | 0x40 | 0x80, m, 0, THME_GROW);
    S = old;
    hrgn_free(&s.clip);
    return true;
}

/* ---------------------------------------------------------------------- */
/* Controls */

bool thme_push_button(qrect r, uint32_t state, bool is_default)
{
    int id = is_default && thme_has_part(501) ? 501 : 500;
    int m[THME_MEASURES] = { 0 };
    return T.on && thme_draw_part(id, r, (state == 0 ? 0x01 : 0) | (state == 2 ? 0x20 : 0), m, 0);
}

/* The default button's ring round r: the default button's layout, drawn
 * only outside the button (whose face and title are drawn already). */
bool thme_default_ring(qrect r, uint32_t state)
{
    if (!T.on || !layout(501))
        return false;
    int m[THME_MEASURES] = { 0 };
    surface s;
    port_surface(&s);
    hrgn in;
    hrgn_init(&in);
    hrgn_rect(&in, r);
    hrgn_op(&s.clip, &s.clip, &in, RGN_DIFF);
    hrgn_free(&in);
    surface *old = S;
    S = &s;
    draw_part_on(501, r, (state == 0 ? 0x01 : 0) | (state == 2 ? 0x20 : 0), m, 0, 0);
    S = old;
    hrgn_free(&s.clip);
    return true;
}

/* The 12-pixel box of a check box or radio button; value 0 off, 1 on, 2 mixed. */
bool thme_check_box(qrect box, bool radio, int value, uint32_t state)
{
    int m[THME_MEASURES] = { 0 };
    qrect b = { box.top, (int16_t)(box.left - 2), box.bottom, (int16_t)(box.right + 2) };
    uint32_t st = (state == 0 ? 0x01 : 0) | (state == 2 ? 0x20 : 0) | (value == 1 ? 0x02 : value == 2 ? 0x04 : 0);
    int id = (radio ? 515 : 510) + (right_to_left() && thme_has_part(radio ? 516 : 511) ? 1 : 0);
    return T.on && thme_draw_part(id, b, st, m, 0);
}

/* A scroll bar: the arrows' images at its ends, the track's layout between
 * them, with the thumb (measures 3 and 4, from the track's start).  hilite
 * is the part pressed: 20 up, 21 down, 22 and 23 the page areas, 129 the
 * thumb. */
bool thme_scroll_bar(qrect r, bool vertical, int hilite, bool enabled, bool active, qrect thumb)
{
    /* Small scroll bars (11 pixels across) have their own layouts and
     * arrows: 410 and 415, 'pxm#' 471/472 and 481/482. */
    int across = vertical ? r.right - r.left : r.bottom - r.top;
    bool small = across < 14 && T.on && layout(vertical ? 415 : 410) &&
                 thme_image_size(fourcc("pxm#"), vertical ? 471 : 481, &(int){ 0 }, &(int){ 0 }, &(int){ 0 });
    int id = (vertical ? 405 : 400) + (small ? 10 : 0);
    int16_t a0 = small ? (vertical ? 471 : 481) : vertical ? 321 : 331, a1 = (int16_t)(a0 + 1);
    int w0, h0, n0, w1, h1, n1;
    if (!T.on || !layout((int16_t)id) || !thme_image_size(fourcc("pxm#"), a0, &w0, &h0, &n0) ||
        !thme_image_size(fourcc("pxm#"), a1, &w1, &h1, &n1))
        return false;
    surface s;
    port_surface(&s);
    surface *old = S;
    S = &s;
    int l0 = vertical ? h0 : w0, l1 = vertical ? h1 : w1;
    int img0 = !enabled || !active ? 2 : hilite == 20 ? 1 : 0, img1 = !enabled || !active ? 2 : hilite == 21 ? 1 : 0;
    qrect q0 = vertical ? (qrect){ r.top, r.left, (int16_t)(r.top + l0), r.right }
                        : (qrect){ r.top, r.left, r.bottom, (int16_t)(r.left + l0) };
    qrect q1 = vertical ? (qrect){ (int16_t)(r.bottom - l1), r.left, r.bottom, r.right }
                        : (qrect){ r.top, (int16_t)(r.right - l1), r.bottom, r.right };
    draw_tile(image(fourcc("pxm#"), a0), img0 < n0 ? img0 : 0, q0, 1);
    draw_tile(image(fourcc("pxm#"), a1), img1 < n1 ? img1 : 0, q1, 4);
    qrect track = vertical ? (qrect){ q0.bottom, r.left, q1.top, r.right } : (qrect){ r.top, q0.right, r.bottom, q1.left };
    int m[THME_MEASURES] = { 0 };
    bool live = enabled && active && thumb.bottom > thumb.top && thumb.right > thumb.left;
    if (live) {
        m[3] = vertical ? thumb.top - track.top : thumb.left - track.left;
        m[4] = vertical ? thumb.bottom - track.top : thumb.right - track.left;
    }
    uint32_t st = (!active ? 0x02 : !enabled ? 0x01 : 0) | (live ? 0x10 : 0) | (hilite == 129 ? 0x100 : 0);
    draw_part_on(id, track, st, m, 0, 0);
    S = old;
    hrgn_free(&s.clip);
    return true;
}

static bool part(int id, qrect r, uint32_t st)
{
    int m[THME_MEASURES] = { 0 };
    return T.on && thme_draw_part(id, r, st, m, 0);
}

/* The layouts' common state bits for a ThemeDrawState. */
static uint32_t draw_state(uint32_t state) { return (state == 0 ? 0x01 : 0) | (state == 2 ? 0x20 : 0); }

/* Bevel buttons: size 0 small, 1 normal, 2 large; on, pushed in. */
bool thme_bevel(qrect r, int size, uint32_t state, bool on)
{
    return part(530 + (size < 0 ? 0 : size > 2 ? 2 : size), r, draw_state(state) | (on ? 0x02 : 0));
}

/* A disclosure triangle: value 1 open; facing left (right-to-left). */
bool thme_disclosure(qrect r, int value, uint32_t state, bool left)
{
    return part(550, r, draw_state(state) | (value ? 0x02 : 0) | (left ? 0x04 : 0));
}

/* DrawThemePopupArrow: the theme's arrow images ('pxm#' 360-364 down, 370
 * right, 380 left, 385 up, three to eleven pixels across), the one as tall
 * (or, sideways, as wide) as the size's points, else the nearest below,
 * centred; images 0 active, 1 pressed, 2 inactive.  orient is a
 * ThemeArrowOrientation (0 left, 1 down, 2 right, 3 up); size 0-3 the 3,
 * 5, 7 and 9 point arrows. */
bool thme_popup_arrow(qrect r, int orient, int size, uint32_t state)
{
    static const int16_t base[4] = { 380, 360, 370, 385 };
    static const int pts[4] = { 3, 5, 7, 9 };
    if (!T.on)
        return false;
    int16_t best = 0;
    int bw = 0, bh = 0, bn = 0, got = 0;
    for (int k = 0; k < 5; k++) {
        int w, h, n;
        int16_t id = (int16_t)(base[orient & 3] + k);
        if (!thme_image_size(fourcc("pxm#"), id, &w, &h, &n))
            continue;
        int along = (orient & 1) ? h : w, want = pts[size & 3];
        /* the largest that fits, else the smallest */
        bool better = !best || (along <= want ? got > want || along > got : got > want && along < got);
        if (better) {
            best = id;
            bw = w, bh = h, bn = n, got = along;
        }
    }
    if (!best)
        return false;
    int frame = state == 0 ? 2 : state == 2 ? 1 : 0;
    if (frame >= bn)
        frame = 0;
    int x = r.left + (r.right - r.left - bw) / 2, y = r.top + (r.bottom - r.top - bh) / 2;
    surface s;
    port_surface(&s);
    surface *old = S;
    S = &s;
    draw_tile(image(fourcc("pxm#"), best), frame, (qrect){ (int16_t)y, (int16_t)x, (int16_t)(y + bh), (int16_t)(x + bw) }, 1);
    S = old;
    hrgn_free(&s.clip);
    return true;
}

/* The keyboard focus round a rectangle: the theme's FocusGenericRect
 * frame ('frme' 560, its rings outside the rectangle). */
bool thme_focus_rect(qrect r) { return thme_frame(560, r); }

/* The little arrows: state 2 the upper one pressed, 3 the lower one. */
bool thme_little_arrows(qrect r, uint32_t state)
{
    return part(540, r, (state == 0 ? 0x01 : 0) | (state == 2 ? 0x20 : 0) | (state == 3 ? 0x40 : 0));
}

/* A pop-up button's frame and arrow, or the arrow button alone. */
bool thme_popup(qrect r, uint32_t state, bool arrow_only)
{
    if (arrow_only && thme_has_part(525))
        return part(525, r, draw_state(state));
    return part(right_to_left() && thme_has_part(521) ? 521 : 520, r, draw_state(state));
}

/* A tab: ThemeTabStyle (0 non-front, 1 pressed, 2 inactive, 3 front, 4
 * front inactive, 5 and 6 unavailable), facing dir (0 north, 1 south, 2
 * east, 3 west: layouts 310, 320, 340 and 330; the small ones 5 on). */
bool thme_tab(qrect r, int style, bool small, int dir)
{
    static const int ids[4] = { 310, 320, 340, 330 };
    bool front = style == 3 || style == 4 || style == 6, inactive = style == 2 || style == 4 || style >= 5;
    int id = ids[dir & 3];
    if (!thme_has_part(id))
        id = 310;
    if (small && thme_has_part(id + 5))
        id += 5;
    return part(id, r, (front ? 0x01 : 0) | (inactive ? 0x02 : 0) | (style == 1 ? 0x20 : 0));
}

bool thme_tab_pane(qrect r, uint32_t state) { return part(300, r, state == 0 ? 0x02 : 0); }

/* Sliders, progress and indeterminate bars (scroll bars go to
 * thme_scroll_bar): the thumb (from the caller's geometry) is measure 3,
 * from the bounds' start; a progress bar's fill ends at measure 3. */
bool thme_track(const thme_track_info *t)
{
    if (!T.on)
        return false;
    qrect b = t->bounds;
    int m[THME_MEASURES] = { 0 };
    uint32_t st = t->enable == 1 ? 0x01 : t->enable == 2 ? 0x02 : 0;
    int id;
    if (t->kind == 2) { /* slider */
        id = t->horizontal ? (t->thumb_dir == 1 ? 421 : t->thumb_dir == 2 ? 422 : 420)
                           : (t->thumb_dir == 1 ? 427 : t->thumb_dir == 2 ? 426 : 425);
        if (!thme_has_part(id))
            id = t->horizontal ? 420 : 425;
        m[3] = t->horizontal ? t->thumb.left - b.left : t->thumb.top - b.top;
        m[5] = m[3];
        st |= (t->enable == 0 && t->max > t->min ? 0x10 : 0) | (t->press & 0x08 ? 0x100 : 0);
    } else if (t->kind == 3) { /* progress */
        id = 430;
        int len = t->horizontal ? b.right - b.left : b.bottom - b.top;
        long range = (long)t->max - t->min;
        m[3] = range > 0 ? (int)((long)(t->value - t->min) * len / range) : 0;
        m[3] = imax(0, imin(len, m[3]));
        st |= 0x10;
    } else { /* indeterminate */
        id = 440;
        st |= 0x40u << (t->phase & 3);
    }
    return thme_draw_part(id, b, st, m, 0);
}

/* ---------------------------------------------------------------------- */
/* Menus */

bool thme_menu_bar(qrect r) { return part(256, r, 0); }

/* A menu title's background: the bar's (some themes' bars have a
 * selected look) under it, then the title's own (RootMenuItem). */
bool thme_menu_title(qrect bar, qrect r, bool selected, bool enabled)
{
    if (!T.on || !layout(256))
        return false;
    int m[THME_MEASURES] = { 0 };
    surface s;
    port_surface(&s);
    hrgn t;
    hrgn_init(&t);
    hrgn_rect(&t, r);
    hrgn_op(&s.clip, &s.clip, &t, RGN_SECT);
    hrgn_free(&t);
    surface *old = S;
    S = &s;
    draw_part_on(256, bar, selected ? 0x20 : 0, m, 0, 0);
    draw_part_on(260, r, (selected ? 0x20 : 0) | (enabled ? 0x02 : 0), m, 0, 0);
    S = old;
    hrgn_free(&s.clip);
    return true;
}

/* A menu's background: 0 pull-down, 1 pop-up, 2 hierarchical. */
static int menu_part(int type)
{
    int id = 257 + (type == 1 ? 1 : type == 2 ? 2 : 0);
    return thme_has_part(id) ? id : 257;
}

bool thme_menu_background(qrect r, int type) { return part(menu_part(type), r, 0); }

/* What a menu's background covers round r: most themes draw the frame and
 * shadow outside the menu's rectangle (System 7's a pixel left and above,
 * two right and below, as the classic menus; some a wide soft shadow), a
 * few inside it.  False when the theme has no menu background. */
bool thme_menu_extent(qrect r, int type, qrect *out)
{
    hrgn g;
    int m[THME_MEASURES] = { 0 };
    if (!T.on || !layout(257) || !thme_part_region(menu_part(type), r, 0, m, 0, &g))
        return false;
    qrect b = hrgn_bbox(&g);
    hrgn_free(&g);
    *out = b.right > b.left && b.bottom > b.top ? b : r;
    return true;
}

/* An item's background (and the arrow of a hierarchical one): 0x02 an
 * enabled item, 0x20 the selected one, 0x04 one with a submenu. */
bool thme_menu_item(qrect r, bool selected, bool enabled, bool hier)
{
    return part(261, r, (enabled ? 0x02 : 0) | (selected ? 0x20 : 0) | (hier ? 0x04 : 0));
}

bool thme_menu_separator(qrect r) { return part(263, r, 0); }

/* ---------------------------------------------------------------------- */
/* The scene ('scen'): the desktop pattern, highlight colour, fonts and
 * sound set, a flattened collection like 'tinf' */

/* An item of a flattened collection by its tag: u32 version, u32, u32
 * count, then {tag, id, attributes, offset} entries; the offsets count
 * from the u32 after them, and each item is a u32 length and the bytes. */
static const uint8_t *coll_item(const uint8_t *d, uint32_t len, const char *tag, uint32_t *ilen)
{
    if (!d || len < 12)
        return NULL;
    uint32_t n = be32(d + 8), base = 12 + n * 16;
    if (n > 4096 || base > len)
        return NULL;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = d + 12 + i * 16;
        if (be32(e) != fourcc(tag))
            continue;
        uint32_t off = base + be32(e + 12);
        if (off + 4 > len || off + 4 + be32(d + off) > len)
            return NULL;
        *ilen = be32(d + off);
        return d + off + 4;
    }
    return NULL;
}

/* An item of the theme's scene: the one its 'tdat' names, else the
 * first other scene that has it (Gizmo's names a desktop picture, which
 * the theme file doesn't hold; another scene of it has a pattern). */
static const uint8_t *scene_item(const char *tag, uint32_t *ilen)
{
    uint32_t tl, len;
    const uint8_t *td = res(fourcc("tdat"), 1, &tl), *d;
    for (uint32_t o = 0; td && o + 16 <= tl; o += 16)
        if (be32(td + o) == fourcc("scen") && (d = res(fourcc("scen"), (int16_t)be16(td + o + 4), &len)) &&
            (d = coll_item(d, len, tag, ilen)))
            return d;
    for (size_t i = 0; i < T.idx.count; i++)
        if (T.idx.entries[i].type == fourcc("scen") &&
            (d = coll_item(rsrc_data(T.fork, &T.idx, &T.idx.entries[i]), T.idx.entries[i].data_len, tag, ilen)))
            return d;
    return NULL;
}

/* A string item of the scene (a Pascal string), as C. */
bool thme_scene_string(const char *tag, char *out, size_t n)
{
    uint32_t il;
    const uint8_t *d = T.on ? scene_item(tag, &il) : NULL;
    if (!d || !il || d[0] + 1u > il || !n)
        return false;
    size_t k = d[0] < n - 1 ? d[0] : n - 1;
    memcpy(out, d + 1, k);
    out[k] = 0;
    return true;
}

/* A theme file draws the screen: loaded, and the appearance chosen. */
bool thme_in_use(void) { return T.on && g_appearance == MACHINE_APPEARANCE_THEME; }

/* A font of the scene ('lgsf' the system font, 'smsf' the small one,
 * 'vfnt' the views font) that is installed here, as its family. */
int thme_scene_font(const char *tag, char *out, size_t n)
{
    char name[64];
    if (!thme_scene_string(tag, name, sizeof name) || !name[0])
        return -1;
    int fam = qd_classic_family(name);
    if (fam >= 0 && out)
        snprintf(out, n, "%s", name);
    return fam;
}

/* The system font a theme's screen has: its scene's, else Charcoal (Mac
 * OS 8's, which stays when a theme names one that isn't installed, as
 * Gizmo's Gadget and Hi-Tech's Techno are not here). */
int thme_system_family(void)
{
    int fam = thme_scene_font("lgsf", NULL, 0);
    return fam >= 0 ? fam : qd_classic_family("Charcoal");
}

/* A 16-bit number of the scene, or a Boolean (a byte, sometimes padded
 * to two: the first). */
bool thme_scene_int(const char *tag, int *out)
{
    uint32_t il;
    const uint8_t *d = T.on ? scene_item(tag, &il) : NULL;
    if (!d || il != 2)
        return false;
    *out = (int16_t)be16(d);
    return true;
}

bool thme_scene_bool(const char *tag, bool *out)
{
    uint32_t il;
    const uint8_t *d = T.on ? scene_item(tag, &il) : NULL;
    if (!d || !il || il > 2)
        return false;
    *out = d[0] != 0;
    return true;
}

/* The sound mask the scene asks for ('smsk'), else every class. */
uint32_t thme_sound_mask(void)
{
    uint32_t il;
    const uint8_t *d = T.on ? scene_item("smsk", &il) : NULL;
    return d && il >= 4 ? be32(d) : 0xF;
}

/* The scene's highlight colour. */
bool thme_highlight_rgb(uint16_t out[3])
{
    uint32_t il;
    const uint8_t *d = T.on ? scene_item("hcol", &il) : NULL;
    if (!d || il < 6)
        return false;
    for (int i = 0; i < 3; i++)
        out[i] = be16(d + i * 2);
    return true;
}

/* The desktop: the scene's pattern (a flattened ppat), tiled from the
 * screen's top left, over the area in the current port. */
bool thme_paint_desk(const hrgn *area)
{
    if (!T.on)
        return false;
    bool fresh;
    tcache *c = cached(fourcc("patt"), 0, &fresh);
    if (fresh) {
        uint32_t il;
        const uint8_t *d = scene_item("patt", &il);
        c->ok = d && decode_ppat(d, il, &c->im);
    }
    if (!c->ok)
        return false;
    surface s;
    port_surface(&s);
    hrgn_op(&s.clip, &s.clip, area, RGN_SECT);
    qrect b = hrgn_bbox(&s.clip);
    surface *old = S;
    S = &s;
    if (b.right > b.left && b.bottom > b.top) {
        /* pinned to 0,0: tile from there to cover the area */
        qrect t = { 0, 0, b.bottom, b.right };
        if (b.top < 0 || b.left < 0)
            t = b;
        draw_tile(&c->im, 0, t, 1);
    }
    S = old;
    hrgn_free(&s.clip);
    return true;
}

/* ---------------------------------------------------------------------- */
/* Icons: 'tic#' 384 maps the system's icon types ('fldr', 'trsh', 'stop',
 * 'caut', 'note'...) to the theme's icon resources: u32 count, then
 * {OSType, int16 ID} entries.  The IDs are mostly the System's own (-3999
 * the folder), the theme holding its own icons at them. */

static const uint8_t *icon_table(uint32_t *n)
{
    uint32_t len;
    const uint8_t *d = T.on ? res(fourcc("tic#"), 384, &len) : NULL;
    if (!d || len < 4)
        return NULL;
    *n = be32(d);
    if (*n > (len - 4) / 6)
        *n = (len - 4) / 6;
    return d + 4;
}

/* Whether the theme has any of an icon family's resources at an ID. */
static bool has_icon(int16_t id)
{
    static const char *const kinds[] = { "icns", "ICN#", "ics#", "icl8", "ics8", "icm#" };
    uint32_t len;
    for (unsigned i = 0; i < sizeof kinds / sizeof kinds[0]; i++)
        if (res(fourcc(kinds[i]), id, &len))
            return true;
    return false;
}

bool thme_icon_id(uint32_t type, int16_t *id)
{
    uint32_t n;
    const uint8_t *e = icon_table(&n);
    for (uint32_t i = 0; e && i < n; i++, e += 6)
        if (be32(e) == type && has_icon((int16_t)be16(e + 4))) {
            *id = (int16_t)be16(e + 4);
            return true;
        }
    return false;
}

bool thme_icon_listed(int16_t id)
{
    uint32_t n;
    const uint8_t *e = icon_table(&n);
    for (uint32_t i = 0; e && i < n; i++, e += 6)
        if ((int16_t)be16(e + 4) == id)
            return has_icon(id);
    return false;
}

const uint8_t *thme_resource(uint32_t type, int16_t id, uint32_t *len) { return T.on ? res(type, id, len) : NULL; }

/* ---------------------------------------------------------------------- */
/* Cursors: 'crsr' 128-131 the arrows, 139 and 140 the closed and open
 * hands; 'acur' 128 the watch (spinning), 129 and 130 the counting hands. */

const uint8_t *thme_cursor(int cursor, int step, uint32_t *len)
{
    if (!T.on)
        return NULL;
    int16_t id = 0, anim = 0;
    switch (cursor) {
    case 0: case 1: case 2: case 3: id = (int16_t)(128 + cursor); break;
    case 4: id = 132; break;  /* kThemeIBeamCursor */
    case 5: id = 133; break;  /* kThemeCrossCursor */
    case 6: id = 134; break;  /* kThemePlusCursor */
    case 7: case 14: anim = 128; break; /* the watch, spinning */
    case 8: id = 139; break;  /* kThemeClosedHandCursor */
    case 9: id = 140; break;  /* kThemeOpenHandCursor */
    case 10: id = 141; break; /* kThemePointingHandCursor */
    case 11: anim = 129; break; /* counting up */
    case 12: case 13: anim = 130; break;
    default: return NULL;
    }
    if (anim) {
        uint32_t al;
        const uint8_t *a = res(fourcc("acur"), anim, &al);
        int n = a && al >= 4 ? be16(a) : 0;
        if (n <= 0 || 4 + (uint32_t)n * 4 > al)
            return NULL;
        id = (int16_t)be16(a + 4 + (step % n) * 4);
    }
    return res(fourcc("crsr"), id, len);
}

/* ---------------------------------------------------------------------- */
/* Sound sets ('tsnd' files): 'snd#' 1000 the effects, 1001 the drag sounds;
 * u16 version, u16 count, then entries of 44 bytes: the ThemeSoundKind
 * (an OSType: 'mnuo', 'wcls', 'btnp'...), u16 its class (1 window, 2 menu,
 * 4 control, 8 Finder: the bits of the sound mask), u16, u16, u16, then
 * four {u16 'snd ' id, u16, u32} of which the first that names one plays. */

bool thme_load_sounds(const char *path, const uint8_t *fork, size_t len, char *err, size_t errlen)
{
    thme_unload_sounds();
    rsrc_index idx;
    if (!rsrc_parse(fork, len, &idx, err, errlen))
        return false;
    bool ok = false;
    for (size_t i = 0; i < idx.count && !ok; i++)
        ok = idx.entries[i].type == fourcc("snd#");
    if (!ok) {
        rsrc_index_free(&idx);
        snprintf(err, errlen, "%s is not a sound set (no snd#)", path);
        return false;
    }
    SND.fork = malloc(len);
    memcpy(SND.fork, fork, len);
    rsrc_index_free(&idx);
    rsrc_parse(SND.fork, len, &SND.idx, err, errlen);
    const char *base = strrchr(path, '/');
    snprintf(SND.name, sizeof SND.name, "%s", base ? base + 1 : path);
    return true;
}

void thme_unload_sounds(void)
{
    if (SND.fork) {
        rsrc_index_free(&SND.idx);
        free(SND.fork);
    }
    memset(&SND, 0, sizeof SND);
}

const char *thme_sound_set(void) { return SND.fork ? SND.name : ""; }

static const uint8_t *snd_res(uint32_t type, int16_t id, uint32_t *len)
{
    for (size_t i = 0; SND.fork && i < SND.idx.count; i++) {
        rsrc_entry *e = &SND.idx.entries[i];
        if (e->type == type && e->id == id) {
            *len = e->data_len;
            return rsrc_data(SND.fork, &SND.idx, e);
        }
    }
    return NULL;
}

/* The 'snd ' for a ThemeSoundKind or ThemeDragSoundKind, and its class. */
const uint8_t *thme_sound(uint32_t kind, int *cls, int16_t *id, uint32_t *len)
{
    for (int16_t table = 1000; table <= 1001 && SND.fork; table++) {
        uint32_t tl;
        const uint8_t *d = snd_res(fourcc("snd#"), table, &tl);
        if (!d || tl < 4)
            continue;
        int n = be16(d + 2);
        for (int i = 0; i < n && 4 + (uint32_t)(i + 1) * 44 <= tl; i++) {
            const uint8_t *e = d + 4 + i * 44;
            if (be32(e) != kind)
                continue;
            for (int k = 0; k < 4; k++) {
                int16_t sid = (int16_t)be16(e + 12 + k * 8);
                const uint8_t *snd = sid ? snd_res(fourcc("snd "), sid, len) : NULL;
                if (snd) {
                    *cls = be16(e + 4);
                    *id = sid;
                    return snd;
                }
            }
            return NULL;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------- */
/* A sheet of the theme's parts, off-screen (bluebox themes --render) */

static void sheet_window(int id, int var, int x, int y, int w, int h, bool goaway, bool zoom, int title_w)
{
    int m[THME_MEASURES] = { 0, title_w, title_w / 2, 12, 6 };
    qrect c = { (int16_t)y, (int16_t)x, (int16_t)(y + h), (int16_t)(x + w) };
    draw_part_on(id, c, window_state(var, goaway, zoom, title_w > 0), m, 0, 0);
}

void thme_render_sheet(uint8_t *rgba, int w, int h)
{
    for (int i = 0; i < w * h; i++) {
        rgba[i * 4] = 0x66;
        rgba[i * 4 + 1] = 0x77;
        rgba[i * 4 + 2] = 0x99;
        rgba[i * 4 + 3] = 255;
    }
    surface s = { .kind = 1, .bounds = { 0, 0, (int16_t)h, (int16_t)w }, .buf = rgba };
    surface *old = S;
    S = &s;
    /* Windows: document (active, inactive, collapsed), dialog, alert,
     * movable modal, floating (top and side). */
    sheet_window(128, 0, 40, 60, 220, 120, true, true, 70);
    sheet_window(129, 0, 300, 60, 220, 120, true, true, 70);
    sheet_window(128, 0x200, 560, 60, 220, 0, true, true, 70);
    sheet_window(129, 0x200, 560, 110, 220, 0, true, true, 70);
    sheet_window(130, 1, 40, 230, 160, 70, false, false, 0);
    sheet_window(132, 3, 230, 230, 160, 70, false, false, 0);
    sheet_window(134, 5, 420, 230, 160, 70, false, false, 60);
    sheet_window(136, 0x100 | 4, 620, 230, 120, 70, true, true, 40);
    sheet_window(138, 0x100 | 8, 640, 340, 100, 70, true, true, 0);
    /* Menu bar, a menu and its items. */
    int m[THME_MEASURES] = { 0 };
    draw_part_on(256, (qrect){ 340, 40, 360, 600 }, 0, m, 0, 0);
    draw_part_on(257, (qrect){ 360, 60, 460, 220 }, 0, m, 0, 0);
    draw_part_on(261, (qrect){ 364, 60, 382, 220 }, 0x02, m, 0, 0);
    draw_part_on(261, (qrect){ 382, 60, 400, 220 }, 0x22, m, 0, 0);
    draw_part_on(263, (qrect){ 400, 60, 409, 220 }, 0, m, 0, 0);
    draw_part_on(261, (qrect){ 409, 60, 427, 220 }, 0x06, m, 0, 0);
    draw_part_on(261, (qrect){ 427, 60, 445, 220 }, 0, m, 0, 0);
    /* Push buttons (active, default, pressed, inactive), check boxes and
     * radio buttons (off, on, mixed; pressed; inactive). */
    draw_part_on(500, (qrect){ 490, 40, 510, 110 }, 0, m, 0, 0);
    draw_part_on(thme_has_part(501) ? 501 : 500, (qrect){ 490, 130, 510, 200 }, 0, m, 0, 0);
    draw_part_on(500, (qrect){ 490, 220, 510, 290 }, 0x20, m, 0, 0);
    draw_part_on(500, (qrect){ 490, 310, 510, 380 }, 0x01, m, 0, 0);
    static const uint32_t cs[] = { 0, 0x02, 0x04, 0x22, 0x01, 0x03 };
    for (int i = 0; i < 6; i++) {
        draw_part_on(510, (qrect){ 530, (int16_t)(40 + i * 40), 546, (int16_t)(76 + i * 40) }, cs[i], m, 0, 0);
        draw_part_on(515, (qrect){ 556, (int16_t)(40 + i * 40), 572, (int16_t)(76 + i * 40) }, cs[i], m, 0, 0);
    }
    S = old;
    /* Scroll bars, through the port-independent path. */
    surface s2 = s;
    S = &s2;
    int w0, h0, n0;
    if (thme_image_size(fourcc("pxm#"), 331, &w0, &h0, &n0)) {
        draw_tile(image(fourcc("pxm#"), 331), 0, (qrect){ 600, 40, (int16_t)(600 + h0), (int16_t)(40 + w0) }, 1);
        draw_tile(image(fourcc("pxm#"), 332), 0, (qrect){ 600, 300, (int16_t)(600 + h0), (int16_t)(300 + w0) }, 1);
        int mm[THME_MEASURES] = { 0, 0, 0, 60, 100 };
        draw_part_on(400, (qrect){ 600, (int16_t)(40 + w0), (int16_t)(600 + h0), 300 }, 0x10, mm, 0, 0);
        draw_part_on(400, (qrect){ 630, (int16_t)(40 + w0), (int16_t)(630 + h0), 300 }, 0x02, mm, 0, 0);
    }
    if (thme_image_size(fourcc("pxm#"), 321, &w0, &h0, &n0)) {
        draw_tile(image(fourcc("pxm#"), 321), 0, (qrect){ 490, 440, (int16_t)(490 + h0), (int16_t)(440 + w0) }, 1);
        draw_tile(image(fourcc("pxm#"), 322), 0, (qrect){ 640, 440, (int16_t)(640 + h0), (int16_t)(440 + w0) }, 1);
        int mm[THME_MEASURES] = { 0, 0, 0, 30, 70 };
        draw_part_on(405, (qrect){ (int16_t)(490 + h0), 440, 640, (int16_t)(440 + w0) }, 0x10, mm, 0, 0);
    }
    if (h < 900)
        goto done;
    /* Tabs facing each way (a front one and one behind, on their panes),
     * small scroll bars, pop-up arrows, a focus frame and a drawer. */
    static const int tab_ids[4] = { 310, 320, 340, 330 };
    for (int d = 0; d < 4; d++) {
        int x = 40 + d * 190, y = 700;
        bool side = d >= 2;
        qrect pane = { (int16_t)(y + (d == 0 ? 21 : 0)), (int16_t)(x + (d == 3 ? 21 : 0)),
                       (int16_t)(y + 120 - (d == 1 ? 21 : 0)), (int16_t)(x + 170 - (d == 2 ? 21 : 0)) };
        draw_part_on(300, pane, 0, m, 0, 0);
        for (int k = 0; k < 2; k++) {
            int p = (side ? y : x) + 8 + k * 50;
            qrect t = d == 0 ? (qrect){ (int16_t)y, (int16_t)p, (int16_t)(y + 24), (int16_t)(p + 51) }
                    : d == 1 ? (qrect){ (int16_t)(y + 96), (int16_t)p, (int16_t)(y + 120), (int16_t)(p + 51) }
                    : d == 2 ? (qrect){ (int16_t)p, (int16_t)(x + 146), (int16_t)(p + 51), (int16_t)(x + 170) }
                             : (qrect){ (int16_t)p, (int16_t)x, (int16_t)(p + 51), (int16_t)(x + 24) };
            draw_part_on(layout((int16_t)tab_ids[d]) ? tab_ids[d] : 310, t, k == 0 ? 0x01 : 0, m, 0, 0);
        }
    }
    int sw, sh, sn;
    if (layout(410) && thme_image_size(fourcc("pxm#"), 481, &sw, &sh, &sn)) {
        draw_tile(image(fourcc("pxm#"), 481), 0, (qrect){ 840, 40, (int16_t)(840 + sh), (int16_t)(40 + sw) }, 1);
        draw_tile(image(fourcc("pxm#"), 482), 0, (qrect){ 840, 240, (int16_t)(840 + sh), (int16_t)(240 + sw) }, 1);
        int mm[THME_MEASURES] = { 0, 0, 0, 40, 70 };
        draw_part_on(410, (qrect){ 840, (int16_t)(40 + sw), (int16_t)(840 + sh), 240 }, 0x10, mm, 0, 0);
    }
    for (int o = 0; o < 4; o++)
        for (int k = 0; k < 5; k++) {
            static const int16_t base[4] = { 380, 360, 370, 385 };
            const timg *im = image(fourcc("pxm#"), (int16_t)(base[o] + k));
            if (im)
                draw_tile(im, 0, (qrect){ (int16_t)(840 + o * 14), (int16_t)(300 + k * 16), (int16_t)(840 + o * 14 + im->h),
                                          (int16_t)(300 + k * 16 + im->w) }, 1);
        }
    draw_frame(560, (qrect){ 845, 420, 865, 480 });
    sheet_window(140, 0, 520, 830, 120, 50, false, false, 0);
    sheet_window(141, 0, 660, 830, 120, 50, false, false, 0);
done:
    S = old;
}

/* ---------------------------------------------------------------------- */
/* bluebox themes --dump and --render */

static bool open_file(const char *path, char *err, size_t errlen)
{
    mac_file mf;
    if (!appledouble_read(path, &mf, err, errlen))
        return false;
    bool ok = mf.rsrc_len && thme_fork_is_theme(mf.rsrc, mf.rsrc_len);
    if (!ok)
        snprintf(err, errlen, "%s is not an Appearance theme file", path);
    else
        ok = thme_load(path, mf.rsrc, mf.rsrc_len, err, errlen);
    mac_file_free(&mf);
    return ok;
}

bool machine_theme_dump(const char *path, bool coords, char *err, size_t errlen)
{
    if (!open_file(path, err, errlen))
        return false;
    thme_dump(stdout, coords);
    thme_unload();
    return true;
}

/* One part in each single state bit (and none), a row each: for working
 * out what the bits mean. */
static void render_states(uint8_t *rgba, int w, int h, int id, int pw, int ph)
{
    for (int i = 0; i < w * h; i++) {
        rgba[i * 4] = 0x66;
        rgba[i * 4 + 1] = 0x77;
        rgba[i * 4 + 2] = 0x99;
        rgba[i * 4 + 3] = 255;
    }
    int m[THME_MEASURES] = { 0, 40, 20, 12, 6, 0, 0 };
    if (id >= 400 && id < 500) {
        m[3] = pw > ph ? pw / 4 : ph / 4;
        m[4] = m[3] + 24;
    }
    surface s = { .kind = 1, .bounds = { 0, 0, (int16_t)h, (int16_t)w }, .buf = rgba };
    surface *old = S;
    S = &s;
    int cols = imax(1, (w - 20) / (pw + 40));
    for (int k = 0; k <= 12; k++) {
        uint32_t st = k ? 1u << (k - 1) : 0;
        int x = 30 + (k % cols) * (pw + 40), y = 30 + (k / cols) * (ph + 40);
        draw_part_on(id, (qrect){ (int16_t)y, (int16_t)x, (int16_t)(y + ph), (int16_t)(x + pw) }, st, m, 0, 0);
    }
    S = old;
}

bool machine_theme_render(const char *path, const char *png, int part, int pw, int ph, char *err, size_t errlen)
{
    if (!open_file(path, err, errlen))
        return false;
    int w = 800, h = part ? 680 : 900;
    uint8_t *rgba = malloc((size_t)w * (size_t)h * 4);
    if (part)
        render_states(rgba, w, h, part, pw, ph);
    else
        thme_render_sheet(rgba, w, h);
    bool ok = machine_write_png(png, rgba, w, h);
    if (!ok)
        snprintf(err, errlen, "could not write %s", png);
    free(rgba);
    thme_unload();
    return ok;
}

/* The menu bar's height: the theme's, when a theme file draws it. */
int machine_menu_bar_height(void)
{
    int h;
    if (T.on && g_appearance == MACHINE_APPEARANCE_THEME && thme_metric("mbht", &h) && h >= 12 && h <= 48)
        return h;
    return 20;
}

bool machine_load_theme_sounds(const char *path, char *err, size_t errlen)
{
    mac_file mf;
    if (!appledouble_read(path, &mf, err, errlen))
        return false;
    bool ok = mf.rsrc_len && thme_load_sounds(path, mf.rsrc, mf.rsrc_len, err, errlen);
    if (!mf.rsrc_len)
        snprintf(err, errlen, "%s has no resource fork", path);
    mac_file_free(&mf);
    return ok;
}

bool machine_theme_sound_set_name(char *out, size_t n) { return thme_scene_string("sndt", out, n); }
