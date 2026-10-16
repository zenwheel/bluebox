/*
 * Themes: Kaleidoscope 2 schemes (Mac OS 8 Appearance-style resources).
 *
 * A scheme draws each kind of window by slicing a sample picture (a cicn)
 * of that window.  Its 'wnd#' names the part rectangles in the sample
 * (content, close, zoom and collapse boxes, title) and cuts each side of
 * the frame into segments: fixed ones, ones that tile to fill the length
 * (8), and the title's (5), which takes the title's width.  Controls are
 * cicns too, sliced by the corner size in their 'cinf'.
 *
 * The scheme is read on the host before boot; images are decoded to RGBA
 * and drawn into the current port at any depth, through its clip.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "forks.h"
#include "machine.h"
#include "qd_internal.h"
#include "tb_internal.h"

typedef struct {
    int16_t id;
    int w, h;
    uint8_t *rgba; /* alpha 0 or 255 */
} image;

typedef struct {
    int type, pos;
} seg_entry;

typedef struct {
    bool valid;
    qrect part[5];          /* 0 content, 1 close, 2 zoom, 3 collapse, 4 title */
    bool has_part[5];
    seg_entry side[4][24];  /* top, bottom, left, right */
    int nside[4];
} wnd_desc;

typedef struct {
    int corner, thickness, tile;
    int text_y, text_x;
    bool valid;
} cinf_desc;

#define MAX_IMAGES 400
static image images[MAX_IMAGES];
static int nimages;
static uint8_t *scheme_fork;
static size_t scheme_len;
static rsrc_index scheme_idx;
static bool loaded;
static bool k1; /* a Kaleidoscope 1.x scheme: classic geometry, scheme stripes and colours */
static char scheme_name[64];

/* ---------------------------------------------------------------------- */
/* Reading the scheme */

static const uint8_t *find_res(uint32_t type, int16_t id, uint32_t *len)
{
    for (size_t i = 0; i < scheme_idx.count; i++) {
        rsrc_entry *e = &scheme_idx.entries[i];
        if (e->type == type && e->id == id) {
            *len = e->data_len;
            return rsrc_data(scheme_fork, &scheme_idx, e);
        }
    }
    return NULL;
}

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static bool decode_cicn(const uint8_t *d, uint32_t len, image *out)
{
    if (len < 82)
        return false;
    int rb = be16(d + 4) & 0x3FFF;
    int top = (int16_t)be16(d + 6), left = (int16_t)be16(d + 8), bottom = (int16_t)be16(d + 10),
        right = (int16_t)be16(d + 12);
    int psize = (int16_t)be16(d + 32);
    int mrb = be16(d + 50 + 4), brb = be16(d + 64 + 4);
    int h = bottom - top, w = right - left;
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048 || (psize != 1 && psize != 2 && psize != 4 && psize != 8))
        return false;
    uint32_t o = 82;
    const uint8_t *mask = d + o;
    o += (uint32_t)(mrb * h);
    o += (uint32_t)(brb * h);
    if (o + 8 > len)
        return false;
    uint16_t flags = be16(d + o + 4);
    int n = (int16_t)be16(d + o + 6) + 1;
    uint8_t ctab[256][3];
    memset(ctab, 0, sizeof ctab);
    const uint8_t *e = d + o + 8;
    for (int i = 0; i < n && i < 256; i++) {
        int v = (flags & 0x8000) ? i : be16(e + i * 8);
        if (v >= 0 && v < 256) {
            ctab[v][0] = e[i * 8 + 2];
            ctab[v][1] = e[i * 8 + 4];
            ctab[v][2] = e[i * 8 + 6];
        }
    }
    o += 8 + (uint32_t)n * 8;
    if (o + (uint32_t)(rb * h) > len)
        return false;
    const uint8_t *pix = d + o;
    out->w = w;
    out->h = h;
    out->rgba = malloc((size_t)w * (size_t)h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int bit = x * psize;
            int v = (pix[y * rb + bit / 8] >> (8 - psize - bit % 8)) & ((1 << psize) - 1);
            int m = mrb ? (mask[y * mrb + x / 8] >> (7 - x % 8)) & 1 : 1;
            uint8_t *p = out->rgba + ((size_t)y * (size_t)w + (size_t)x) * 4;
            p[0] = ctab[v][0];
            p[1] = ctab[v][1];
            p[2] = ctab[v][2];
            p[3] = m ? 255 : 0;
        }
    return true;
}

static const image *img(int16_t id)
{
    for (int i = 0; i < nimages; i++)
        if (images[i].id == id)
            return images[i].rgba ? &images[i] : NULL;
    if (!loaded || nimages >= MAX_IMAGES)
        return NULL;
    image *im = &images[nimages++];
    memset(im, 0, sizeof *im);
    im->id = id;
    uint32_t len;
    const uint8_t *d = find_res(fourcc("cicn"), id, &len);
    if (!d || !decode_cicn(d, len, im))
        im->rgba = NULL;
    return im->rgba ? im : NULL;
}

static wnd_desc wnd_for(int16_t id)
{
    wnd_desc w;
    memset(&w, 0, sizeof w);
    uint32_t len;
    const uint8_t *d = find_res(fourcc("wnd#"), id, &len);
    if (!d)
        return w;
    uint32_t o = 0;
    int n = (int16_t)be16(d) + 1;
    o = 2;
    for (int i = 0; i < n && o + 10 <= len; i++, o += 10) {
        int part = (int16_t)be16(d + o);
        if (part >= 0 && part < 5) {
            w.part[part] = (qrect){ (int16_t)be16(d + o + 2), (int16_t)be16(d + o + 4), (int16_t)be16(d + o + 6),
                                    (int16_t)be16(d + o + 8) };
            w.has_part[part] = true;
        }
    }
    for (int s = 0; s < 4; s++) {
        if (o + 2 > len)
            break;
        int k = (int16_t)be16(d + o) + 1;
        o += 2;
        for (int i = 0; i < k && o + 4 <= len; i++, o += 4)
            if (w.nside[s] < 24)
                w.side[s][w.nside[s]++] = (seg_entry){ (int16_t)be16(d + o), (int16_t)be16(d + o + 2) };
    }
    w.valid = w.has_part[0];
    return w;
}

static cinf_desc cinf_for(int16_t id)
{
    cinf_desc c;
    memset(&c, 0, sizeof c);
    uint32_t len;
    const uint8_t *d = find_res(fourcc("cinf"), id, &len);
    if (!d || len < 14)
        return c;
    c.corner = d[0];
    c.thickness = d[1];
    c.tile = d[2];
    c.text_y = (int16_t)be16(d + 10);
    c.text_x = (int16_t)be16(d + 12);
    c.valid = true;
    return c;
}

bool machine_load_theme(const char *path, char *err, size_t errlen)
{
    mac_file mf;
    if (!appledouble_read(path, &mf, err, errlen))
        return false;
    if (!mf.rsrc_len) {
        snprintf(err, errlen, "%s has no resource fork", path);
        mac_file_free(&mf);
        return false;
    }
    free(scheme_fork);
    scheme_fork = malloc(mf.rsrc_len);
    memcpy(scheme_fork, mf.rsrc, mf.rsrc_len);
    scheme_len = mf.rsrc_len;
    mac_file_free(&mf);
    if (scheme_idx.entries)
        rsrc_index_free(&scheme_idx);
    if (!rsrc_parse(scheme_fork, scheme_len, &scheme_idx, err, errlen))
        return false;
    for (int i = 0; i < nimages; i++)
        free(images[i].rgba);
    nimages = 0;
    loaded = true;
    const char *base = strrchr(path, '/');
    snprintf(scheme_name, sizeof scheme_name, "%s", base ? base + 1 : path);
    uint32_t len;
    k1 = !wnd_for(-14336).valid;
    if (k1 && !find_res(fourcc("cicn"), -14331, &len)) {
        snprintf(err, errlen, "%s is not a Kaleidoscope scheme (no window artwork)", path);
        loaded = false;
        return false;
    }
    machine_set_appearance(MACHINE_APPEARANCE_THEME);
    return true;
}

bool theme_active(void) { return loaded && g_appearance == MACHINE_APPEARANCE_THEME; }

/* A scheme's Finder and system icons (Kaleidoscope 2's "icxx" resources,
 * IDs -20999 to -3700) take the place of the System file's: put them into
 * the list it is built from. */
void theme_system_icons(rsrc_spec *specs, size_t *n, size_t max)
{
    if (!theme_active())
        return;
    static const char *const types[] = { "ICN#", "icl4", "icl8", "ics#", "ics4", "ics8", "icm#", "icm4", "icm8" };
    for (size_t i = 0; i < scheme_idx.count; i++) {
        rsrc_entry *e = &scheme_idx.entries[i];
        if (e->id < -20999 || e->id > -3700)
            continue;
        bool icon = false;
        for (size_t t = 0; t < sizeof types / sizeof types[0]; t++)
            icon |= e->type == fourcc(types[t]);
        if (!icon)
            continue;
        const uint8_t *d = rsrc_data(scheme_fork, &scheme_idx, e);
        size_t k = 0;
        while (k < *n && !(specs[k].type == e->type && specs[k].id == e->id))
            k++;
        if (k == *n) {
            if (*n >= max)
                continue;
            (*n)++;
        }
        specs[k] = (rsrc_spec){ e->type, e->id, NULL, RES_SYS_HEAP | RES_PURGEABLE, d, e->data_len };
    }
}

/* ---------------------------------------------------------------------- */
/* Blitting */

/* Draw part of an image (sx, sy, w, h) at (dx, dy) in the current port,
 * through its clip; transparent pixels are left alone. */
static void blit(const image *im, int sx, int sy, int w, int h, int dx, int dy, const hrgn *clip)
{
    if (!im || w <= 0 || h <= 0)
        return;
    gaddr port = qd_port();
    pixdst d = qd_port_pixels(port);
    for (int y = 0; y < h; y++) {
        int yy = sy + y;
        if (yy < 0 || yy >= im->h)
            continue;
        int ty = dy + y;
        int n;
        const int16_t *xs = hrgn_row(clip, ty, &n);
        if (!xs)
            continue;
        for (int x = 0; x < w; x++) {
            int xx = sx + x;
            if (xx < 0 || xx >= im->w)
                continue;
            const uint8_t *p = im->rgba + ((size_t)yy * (size_t)im->w + (size_t)xx) * 4;
            if (!p[3])
                continue;
            int tx = dx + x;
            bool in = false;
            for (int k = 0; k < n; k += 2)
                if (tx >= xs[k] && tx < xs[k + 1]) {
                    in = true;
                    break;
                }
            if (!in || tx < d.bounds.left || tx >= d.bounds.right || ty < d.bounds.top || ty >= d.bounds.bottom)
                continue;
            uint16_t c[3] = { (uint16_t)(p[0] * 0x101), (uint16_t)(p[1] * 0x101), (uint16_t)(p[2] * 0x101) };
            put_px(&d, tx - d.bounds.left, ty - d.bounds.top, qd_rgb_px(d.depth, d.ctab, c));
        }
    }
}

/* Tile a source span to fill a destination span. */
static void blit_tiled(const image *im, int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh, const hrgn *clip)
{
    if (sw <= 0 || sh <= 0)
        return;
    for (int y = 0; y < dh; y += sh)
        for (int x = 0; x < dw; x += sw)
            blit(im, sx, sy, dw - x < sw ? dw - x : sw, dh - y < sh ? dh - y : sh, dx + x, dy + y, clip);
}

/* Nine-slice: corners fixed, edges and middle tiled. */
static void blit_nine(const image *im, int corner, qrect r, const hrgn *clip)
{
    if (!im)
        return;
    int c = corner > 0 ? corner : im->w / 3;
    if (c * 2 > im->w)
        c = im->w / 2;
    int cy = c * 2 > im->h ? im->h / 2 : c;
    int w = r.right - r.left, h = r.bottom - r.top;
    int mw = im->w - 2 * c, mh = im->h - 2 * cy;
    blit(im, 0, 0, c, cy, r.left, r.top, clip);
    blit(im, im->w - c, 0, c, cy, r.right - c, r.top, clip);
    blit(im, 0, im->h - cy, c, cy, r.left, r.bottom - cy, clip);
    blit(im, im->w - c, im->h - cy, c, cy, r.right - c, r.bottom - cy, clip);
    blit_tiled(im, c, 0, mw, cy, r.left + c, r.top, w - 2 * c, cy, clip);
    blit_tiled(im, c, im->h - cy, mw, cy, r.left + c, r.bottom - cy, w - 2 * c, cy, clip);
    blit_tiled(im, 0, cy, c, mh, r.left, r.top + cy, c, h - 2 * cy, clip);
    blit_tiled(im, im->w - c, cy, c, mh, r.right - c, r.top + cy, c, h - 2 * cy, clip);
    blit_tiled(im, c, cy, mw, mh, r.left + c, r.top + cy, w - 2 * c, h - 2 * cy, clip);
}

static void port_clip(hrgn *clip) { qd_port_clip(qd_port(), clip); }

/* ---------------------------------------------------------------------- */
/* Window layout */

typedef struct {
    int16_t active_id, inactive_id, wnd_id, down_id;
} window_kind;

static bool kind_for(int var, window_kind *k)
{
    if (var & 0x200) { /* collapsed: the title bar alone */
        if (var & 0x100)
            *k = var & 8 ? (window_kind){ -14291, -14292, -14292, -14290 } : (window_kind){ -14299, -14300, -14300, -14298 };
        else
            *k = (window_kind){ -14331, -14332, -14332, -14330 };
        return true;
    }
    switch (var) {
    case 0: case 4: case 8: case 12: /* document windows */
        *k = (window_kind){ -14335, -14336, -14336, -14330 };
        return true;
    case 1: /* dBoxProc */
        *k = (window_kind){ -14327, -14328, -14328, 0 };
        return true;
    case 3: /* altDBoxProc: an alert */
        *k = (window_kind){ -14325, -14326, -14326, 0 };
        return true;
    case 5: /* movableDBoxProc */
        *k = (window_kind){ -14323, -14324, -14324, 0 };
        return true;
    default:
        break;
    }
    if (var & 0x100) { /* WDEF 124's floating windows: title on top, or at the side */
        *k = var & 8 ? (window_kind){ -14295, -14296, -14296, -14290 } : (window_kind){ -14303, -14304, -14304, -14298 };
        return true;
    }
    return false;
}

/* Lay out one side's segments along a length.  Writes (src0, src1, dst0,
 * dst1) for each segment; the title segment takes title_w if > 0. */
typedef struct {
    int type, s0, s1, d0, d1;
} placed;

static int layout(const seg_entry *e, int n, int sample_end, int start, int length, int title_w, placed *out)
{
    int count = 0, fixed = 0, stretch = 0;
    for (int i = 1; i < n; i++) {
        int s0 = e[i - 1].pos, s1 = e[i].pos > sample_end ? sample_end : e[i].pos;
        if (s1 <= s0)
            continue;
        out[count] = (placed){ e[i].type, s0, s1, 0, 0 };
        if (e[i].type == 8)
            stretch++;
        else if (e[i].type == 5 && title_w > 0)
            fixed += title_w;
        else
            fixed += s1 - s0;
        count++;
    }
    int origin = n ? e[0].pos : 0;
    int extra = length - fixed - (origin - start) * 0;
    int each = stretch ? (extra > 0 ? extra / stretch : 0) : 0, rem = stretch && extra > 0 ? extra % stretch : 0;
    int x = origin;
    for (int i = 0; i < count; i++) {
        int w;
        if (out[i].type == 8) {
            w = each + (rem-- > 0 ? 1 : 0);
        } else if (out[i].type == 5 && title_w > 0) {
            w = title_w;
        } else {
            w = out[i].s1 - out[i].s0;
        }
        out[i].d0 = x;
        out[i].d1 = x + w;
        x += w;
    }
    return count;
}

/* Insets of the frame around the content, from the sample. */
static bool insets(int var, int *l, int *t, int *r, int *b)
{
    window_kind k;
    if (!kind_for(var, &k))
        return false;
    wnd_desc w = wnd_for(k.wnd_id);
    const image *im = img(k.active_id);
    if (!w.valid || !im)
        return false;
    *l = w.part[0].left;
    *t = w.part[0].top;
    *r = im->w - w.part[0].right;
    *b = im->h - w.part[0].bottom;
    return true;
}

bool theme_window_insets(int var, int *l, int *t, int *r, int *b) { return theme_active() && insets(var, l, t, r, b); }

/* Where a sample x lands in a laid-out side. */
static int map_pos(const placed *p, int n, int s)
{
    for (int i = 0; i < n; i++)
        if (s >= p[i].s0 && s < p[i].s1)
            return p[i].d0 + (s - p[i].s0) * (p[i].d1 - p[i].d0 > 0 && p[i].type == 8 ? 1 : 1);
    return n ? p[n - 1].d1 : s;
}

static int title_width(gaddr w, const wnd_desc *wd)
{
    gaddr th = WindowRecord_titleHandle(w);
    if (!th || !rd32(th) || !wd->has_part[4])
        return 0;
    int n = rd8(rd32(th));
    if (!n)
        return 0;
    uint8_t s[256];
    gget(s, rd32(th) + 1, (uint32_t)n);
    gaddr port = qd_port();
    int16_t f = GrafPort_txFont(port), sz = GrafPort_txSize(port);
    uint8_t face = GrafPort_txFace(port);
    GrafPort_set_txFont(port, 0);
    GrafPort_set_txSize(port, 12);
    GrafPort_set_txFace(port, 0);
    int tw = qd_text_width(port, s, n);
    GrafPort_set_txFont(port, f);
    GrafPort_set_txSize(port, sz);
    GrafPort_set_txFace(port, face);
    int sample = wd->part[4].right - wd->part[4].left;
    return tw + 12 > sample ? tw + 12 : sample;
}

/* The title bar parts of a laid-out window, in global coordinates. */
typedef struct {
    placed top[24], bottom[24], left[24], right[24];
    int ntop, nbottom, nleft, nright;
    int ox, oy; /* structure origin */
    int W, H;
    wnd_desc wd;
    window_kind k;
    const image *sample;
} frame;

static bool frame_for(gaddr w, int var, qrect c, bool active, frame *f)
{
    memset(f, 0, sizeof *f);
    if (!kind_for(var, &f->k))
        return false;
    f->wd = wnd_for(f->k.wnd_id);
    f->sample = img(active ? f->k.active_id : f->k.inactive_id);
    if (!f->sample)
        f->sample = img(f->k.active_id);
    if (!f->wd.valid || !f->sample)
        return false;
    const wnd_desc *wd = &f->wd;
    int l = wd->part[0].left, t = wd->part[0].top;
    int r = f->sample->w - wd->part[0].right, b = f->sample->h - wd->part[0].bottom;
    f->ox = c.left - l;
    f->oy = c.top - t;
    f->W = (c.right - c.left) + l + r;
    f->H = (c.bottom - c.top) + t + b;
    int tw = title_width(w, wd);
    f->ntop = layout(wd->side[0], wd->nside[0], f->sample->w, 0, f->W, tw, f->top);
    int bstart = wd->nside[1] ? wd->side[1][0].pos : 0;
    f->nbottom = layout(wd->side[1], wd->nside[1], f->sample->w, 0, f->W - 2 * bstart, 0, f->bottom);
    int vstart = wd->nside[2] ? wd->side[2][0].pos : t;
    f->nleft = layout(wd->side[2], wd->nside[2], wd->part[0].bottom, vstart, (c.bottom - c.top), 0, f->left);
    f->nright = layout(wd->side[3], wd->nside[3], wd->part[0].bottom, vstart, (c.bottom - c.top), 0, f->right);
    return true;
}

static qrect widget_rect(const frame *f, int part)
{
    qrect s = f->wd.part[part];
    int x = map_pos(f->top, f->ntop, s.left);
    return (qrect){ (int16_t)(f->oy + s.top), (int16_t)(f->ox + x), (int16_t)(f->oy + s.bottom),
                    (int16_t)(f->ox + x + (s.right - s.left)) };
}

/* Draw a laid-out band of the sample: rows [sy0, sy1) of the sample along
 * a horizontal layout at target y. */
static void draw_hband(const frame *f, const placed *p, int n, int sy0, int sy1, int ty, const hrgn *clip)
{
    for (int i = 0; i < n; i++) {
        int sw = p[i].s1 - p[i].s0, dw = p[i].d1 - p[i].d0;
        if (p[i].type == 8 || (p[i].type == 5 && dw != sw))
            blit_tiled(f->sample, p[i].s0, sy0, sw, sy1 - sy0, f->ox + p[i].d0, ty, dw, sy1 - sy0, clip);
        else
            blit(f->sample, p[i].s0, sy0, sw, sy1 - sy0, f->ox + p[i].d0, ty, clip);
    }
}

static void draw_vband(const frame *f, const placed *p, int n, int sx0, int sx1, int tx, int content_top, const hrgn *clip)
{
    for (int i = 0; i < n; i++) {
        int sh = p[i].s1 - p[i].s0, dh = p[i].d1 - p[i].d0;
        if (p[i].type == 8)
            blit_tiled(f->sample, sx0, p[i].s0, sx1 - sx0, sh, tx, content_top + p[i].d0 - f->wd.part[0].top, sx1 - sx0, dh, clip);
        else
            blit(f->sample, sx0, p[i].s0, sx1 - sx0, sh, tx, content_top + p[i].d0 - f->wd.part[0].top, clip);
    }
}

/* Cover an absent widget with the title bar's stretch pattern. */
static void cover_widget(const frame *f, qrect wr, const hrgn *clip)
{
    for (int i = 0; i < f->ntop; i++)
        if (f->top[i].type == 8) {
            int sw = f->top[i].s1 - f->top[i].s0;
            blit_tiled(f->sample, f->top[i].s0, wr.top - f->oy, sw, wr.bottom - wr.top, wr.left, wr.top,
                       wr.right - wr.left, wr.bottom - wr.top, clip);
            return;
        }
}

#define MAX_DOWN 8
static struct {
    gaddr w;
    int part;
} down[MAX_DOWN];

static bool is_down(gaddr w, int part)
{
    for (int i = 0; i < MAX_DOWN; i++)
        if (down[i].w == w && down[i].part == part)
            return true;
    return false;
}

static bool toggle_down(gaddr w, int part)
{
    for (int i = 0; i < MAX_DOWN; i++)
        if (down[i].w == w && down[i].part == part) {
            down[i].w = 0;
            return false;
        }
    for (int i = 0; i < MAX_DOWN; i++)
        if (!down[i].w) {
            down[i].w = w;
            down[i].part = part;
            return true;
        }
    return false;
}

static void draw_widget(const frame *f, gaddr w, int part, bool present, bool pressed, const hrgn *clip)
{
    if (!f->wd.has_part[part])
        return;
    qrect wr = widget_rect(f, part);
    if (!present) {
        cover_widget(f, wr, clip);
        return;
    }
    const image *dn = f->k.down_id ? img(f->k.down_id) : NULL;
    if (pressed && dn) {
        int cw = dn->w / 3;
        blit(dn, (part - 1) * cw, 0, cw, dn->h, wr.left, wr.top, clip);
    } else {
        qrect s = f->wd.part[part];
        blit(f->sample, s.left, s.top, s.right - s.left, s.bottom - s.top, wr.left, wr.top, clip);
    }
}

/* Average luminance of the title area: dark titles get white text. */
static bool dark_title(const frame *f)
{
    qrect s = f->wd.part[4];
    long sum = 0, n = 0;
    for (int y = s.top; y < s.bottom; y++)
        for (int x = s.left; x < s.right; x++) {
            if (x < 0 || y < 0 || x >= f->sample->w || y >= f->sample->h)
                continue;
            const uint8_t *p = f->sample->rgba + ((size_t)y * (size_t)f->sample->w + (size_t)x) * 4;
            sum += p[0] * 30 + p[1] * 59 + p[2] * 11;
            n++;
        }
    return n && sum / n / 100 < 110;
}

/* Kaleidoscope 1.x: after the classic frame is drawn, lay the scheme's
 * racing stripes over the title bar's. */
void theme_decorate_window(gaddr w, int var, qrect c, bool hilited, bool goaway, bool zoom)
{
    if (!theme_active() || !k1 || !hilited || (var != 0 && var != 4 && var != 8 && var != 12))
        return;
    const image *st = img(-14331);
    if (!st)
        return;
    hrgn clip;
    hrgn_init(&clip);
    port_clip(&clip);
    int top = c.top - 18 + 3; /* the classic title bar's stripe rows */
    int x0 = c.left + 1, x1 = c.right - 1;
    int tw = 0, n = 0;
    uint8_t s[256];
    gaddr th = WindowRecord_titleHandle(w);
    if (th && rd32(th) && (n = rd8(rd32(th))) > 0) {
        gget(s, rd32(th) + 1, (uint32_t)n);
        tw = qd_text_width(qd_port(), s, n);
    }
    int tx = c.left + (c.right - c.left - tw) / 2;
    int cuts[3][2] = { { goaway ? c.left + 7 : 0, goaway ? c.left + 21 : 0 },
                       { n ? tx - 6 : 0, n ? tx + tw + 6 : 0 },
                       { zoom ? c.right - 22 : 0, zoom ? c.right - 7 : 0 } };
    int x = x0;
    for (int k = 0; k <= 3; k++) {
        int end = k < 3 && cuts[k][1] ? cuts[k][0] : (k == 3 ? x1 : x);
        if (k < 3 && !cuts[k][1])
            continue;
        if (end > x)
            blit_tiled(st, 0, 0, st->w, st->h, x, top, end - x, st->h, &clip);
        if (k < 3)
            x = cuts[k][1];
    }
    hrgn_free(&clip);
}

bool theme_draw_window(gaddr w, int var, qrect c, bool hilited, bool goaway, bool zoom, int param)
{
    if (!theme_active() || k1)
        return false;
    frame f;
    if (!frame_for(w, var, c, hilited, &f))
        return false;
    hrgn clip;
    hrgn_init(&clip);
    port_clip(&clip);
    if (param >= 4 && param <= 6) { /* a box pressed or released */
        int part = param == 4 ? 1 : 2;
        bool d = toggle_down(w, part);
        draw_widget(&f, w, part, true, d, &clip);
        hrgn_free(&clip);
        return true;
    }
    int t = f.wd.part[0].top, cb = f.wd.part[0].bottom;
    /* Top band (rows above the content), bottom band, sides. */
    draw_hband(&f, f.top, f.ntop, 0, t, f.oy, &clip);
    int bstart = f.wd.nside[1] ? f.wd.side[1][0].pos : 0;
    for (int i = 0; i < f.nbottom; i++) {
        placed p = f.bottom[i];
        int sw = p.s1 - p.s0, dw = p.d1 - p.d0;
        int ty = c.bottom, sh = f.sample->h - cb;
        if (p.type == 8)
            blit_tiled(f.sample, p.s0, cb, sw, sh, f.ox + bstart + (p.d0 - bstart), ty, dw, sh, &clip);
        else
            blit(f.sample, p.s0, cb, sw, sh, f.ox + bstart + (p.d0 - bstart), ty, &clip);
    }
    /* Bottom corners outside the bottom list (e.g. a body narrower than the title bar). */
    draw_vband(&f, f.left, f.nleft, 0, f.wd.part[0].left, f.ox, c.top, &clip);
    draw_vband(&f, f.right, f.nright, f.wd.part[0].right, f.sample->w, c.right, c.top, &clip);
    /* Widgets and title. */
    if (f.wd.has_part[1])
        draw_widget(&f, w, 1, goaway, is_down(w, 1), &clip);
    if (f.wd.has_part[2])
        draw_widget(&f, w, 2, zoom, is_down(w, 2), &clip);
    if (f.wd.has_part[3])
        draw_widget(&f, w, 3, !(var & 0x100) ? (var & 0xFF) == 0 || (var & 0xFF) == 4 || (var & 0xFF) == 8 ||
                                                     (var & 0xFF) == 12 : true,
                    false, &clip); /* the collapse box */
    gaddr th = WindowRecord_titleHandle(w);
    bool floating = var & 0x100;
    if (f.wd.has_part[4] && th && rd32(th) && rd8(rd32(th)) && !(floating && (var & 8))) {
        int n = rd8(rd32(th));
        uint8_t s[256];
        gget(s, rd32(th) + 1, (uint32_t)n);
        gaddr port = qd_port();
        GrafPort_set_txFont(port, floating ? 3 : 0);
        GrafPort_set_txSize(port, floating ? 9 : 12);
        GrafPort_set_txFace(port, 0);
        GrafPort_set_txMode(port, 1);
        qrect tr = widget_rect(&f, 4);
        int tw = qd_text_width(port, s, n);
        int x = tr.left + (tr.right - tr.left - tw) / 2;
        for (int i = 0; i < f.ntop; i++)
            if (f.top[i].type == 5)
                x = f.ox + (f.top[i].d0 + f.top[i].d1) / 2 - tw / 2;
        tb_MoveTo((int16_t)x, (int16_t)(tr.top + (tr.bottom - tr.top) / 2 + 4));
        uint16_t col[3];
        bool dark = dark_title(&f);
        uint16_t v = dark ? 0xFFFF : 0;
        if (!hilited)
            v = dark ? 0xBBBB : 0x7777;
        col[0] = col[1] = col[2] = v;
        qd_set_fg_override(col);
        qd_draw_text(port, s, n);
        qd_set_fg_override(NULL);
    }
    hrgn_free(&clip);
    return true;
}

/* The structure region: every opaque pixel of the composed frame plus the content. */
bool theme_window_region(gaddr w, int var, qrect c, hrgn *out)
{
    if (!theme_active() || k1)
        return false;
    frame f;
    if (!frame_for(w, var, c, true, &f))
        return false;
    hrgn_init(out);
    hrgn_rect(out, (qrect){ (int16_t)f.oy, (int16_t)f.ox, (int16_t)(f.oy + f.H), (int16_t)(f.ox + f.W) });
    /* Cut away the transparent pixels of the top band and its corners. */
    int t = f.wd.part[0].top;
    hrgn cut;
    hrgn_init(&cut);
    for (int y = 0; y < t; y++) {
        for (int i = 0; i < f.ntop; i++) {
            placed p = f.top[i];
            int sw = p.s1 - p.s0;
            for (int dx = p.d0; dx < p.d1; dx++) {
                int sx = p.s0 + (sw ? (dx - p.d0) % sw : 0);
                const uint8_t *px = f.sample->rgba + ((size_t)y * (size_t)f.sample->w + (size_t)sx) * 4;
                if (!px[3]) {
                    hrgn r;
                    hrgn_init(&r);
                    hrgn_rect(&r, (qrect){ (int16_t)(f.oy + y), (int16_t)(f.ox + dx), (int16_t)(f.oy + y + 1),
                                           (int16_t)(f.ox + dx + 1) });
                    hrgn_op(&cut, &cut, &r, RGN_UNION);
                    hrgn_free(&r);
                }
            }
        }
    }
    /* Side margins outside the sides' sample columns (e.g. indented bodies). */
    for (int y = t; y < f.sample->h; y++) {
        for (int sx = 0; sx < f.sample->w; sx++) {
            if (sx >= f.wd.part[0].left && sx < f.wd.part[0].right)
                continue;
            const uint8_t *px = f.sample->rgba + ((size_t)y * (size_t)f.sample->w + (size_t)sx) * 4;
            if (px[3])
                continue;
            int dx = sx < f.wd.part[0].left ? sx : f.W - (f.sample->w - sx);
            int y0 = y < f.wd.part[0].bottom ? f.oy + t + (y - t) : f.oy + f.H - (f.sample->h - y);
            int y1 = y < f.wd.part[0].bottom ? f.oy + f.H - (f.sample->h - f.wd.part[0].bottom) : y0 + 1;
            if (y < f.wd.part[0].bottom && y != t)
                continue; /* one span covers the whole side */
            hrgn r;
            hrgn_init(&r);
            hrgn_rect(&r, (qrect){ (int16_t)y0, (int16_t)(f.ox + dx), (int16_t)y1, (int16_t)(f.ox + dx + 1) });
            hrgn_op(&cut, &cut, &r, RGN_UNION);
            hrgn_free(&r);
        }
    }
    hrgn_op(out, out, &cut, RGN_DIFF);
    hrgn_free(&cut);
    return true;
}

/* Hit testing: 1 close, 2 zoom, 3 collapse, 4 title/drag, 0 none. */
int theme_window_hit(gaddr w, int var, qrect c, int h, int v)
{
    frame f;
    if (!theme_active() || k1 || !frame_for(w, var, c, true, &f))
        return -1;
    for (int part = 1; part <= 3; part++) {
        if (!f.wd.has_part[part])
            continue;
        qrect r = widget_rect(&f, part);
        if (h >= r.left && h < r.right && v >= r.top && v < r.bottom)
            return part;
    }
    if (h >= f.ox && h < f.ox + f.W && v >= f.oy && v < f.oy + f.H)
        return 4;
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Controls */

bool theme_draw_grow(qrect c, bool active)
{
    if (!theme_active())
        return false;
    const image *im = img(k1 ? (active ? -14330 : -14334) : (active ? -14333 : -14334));
    if (!im)
        return false;
    hrgn clip;
    hrgn_init(&clip);
    port_clip(&clip);
    /* Widen the clip to the window's frame: the grow box overlaps it. */
    if (k1)
        blit(im, 0, 0, im->w, im->h, c.right - 16, c.bottom - 16, &clip);
    else
        blit(im, 0, 0, im->w, im->h, c.right - 15, c.bottom - 15, &clip);
    hrgn_free(&clip);
    return true;
}

bool theme_draw_button(qrect r, const uint8_t *title, int n, bool pressed, bool disabled)
{
    if (!theme_active())
        return false;
    int16_t id = disabled ? -10240 : pressed ? -10238 : -10239;
    const image *im = img(id);
    if (!im)
        return false;
    cinf_desc ci = cinf_for(id);
    hrgn clip;
    hrgn_init(&clip);
    port_clip(&clip);
    blit_nine(im, ci.valid ? ci.corner : 4, r, &clip);
    hrgn_free(&clip);
    gaddr port = qd_port();
    gaddr fi = tb_NewPtr(8, false, false);
    tb_GetFontInfo(fi);
    int asc = (int16_t)rd16(fi), desc = (int16_t)rd16(fi + 2);
    tb_DisposePtr(fi);
    int tw = qd_text_width(port, title, n);
    tb_MoveTo((int16_t)(r.left + (r.right - r.left - tw) / 2), (int16_t)(r.top + (r.bottom - r.top + asc - desc) / 2));
    uint16_t col[3] = { 0, 0, 0 };
    if (ci.valid && ci.text_x >= 0 && ci.text_y >= 0 && ci.text_x < im->w && ci.text_y < im->h) {
        const uint8_t *p = im->rgba + ((size_t)ci.text_y * (size_t)im->w + (size_t)ci.text_x) * 4;
        col[0] = (uint16_t)(p[0] * 0x101);
        col[1] = (uint16_t)(p[1] * 0x101);
        col[2] = (uint16_t)(p[2] * 0x101);
    } else if (disabled) {
        col[0] = col[1] = col[2] = 0x8888;
    }
    qd_set_fg_override(col);
    qd_draw_text(port, title, n);
    qd_set_fg_override(NULL);
    return true;
}

bool theme_draw_check(qrect box, bool radio, bool on, bool pressed, bool disabled)
{
    if (!theme_active())
        return false;
    /* -9504.. check boxes (inactive, normal, pressed; unchecked then checked),
     * -9492.. radio buttons likewise. */
    int16_t base = radio ? -9492 : -9504;
    int16_t id = (int16_t)(base + (on ? 3 : 0) + (disabled ? 0 : pressed ? 2 : 1));
    const image *im = img(id);
    if (!im)
        return false;
    hrgn clip;
    hrgn_init(&clip);
    port_clip(&clip);
    blit(im, 0, 0, im->w, im->h, box.left, box.top + (box.bottom - box.top - im->h) / 2, &clip);
    hrgn_free(&clip);
    return true;
}

bool theme_draw_scrollbar(qrect r, bool vertical, int hilite, bool enabled, bool active, qrect thumb)
{
    if (!theme_active())
        return false;
    hrgn clip;
    hrgn_init(&clip);
    port_clip(&clip);
    int len = vertical ? r.bottom - r.top : r.right - r.left;
    if (!enabled || !active) {
        const image *im = img(vertical ? (enabled ? -8279 : -8280) : (enabled ? -8287 : -8288));
        if (!im) {
            hrgn_free(&clip);
            return false;
        }
        int ends = 4;
        if (vertical) {
            blit(im, 0, 0, im->w, ends, r.left, r.top, &clip);
            blit_tiled(im, 0, im->h / 2 - 2, im->w, 4, r.left, r.top + ends, im->w, len - 2 * ends, &clip);
            blit(im, 0, im->h - ends, im->w, ends, r.left, r.bottom - ends, &clip);
        } else {
            blit(im, 0, 0, ends, im->h, r.left, r.top, &clip);
            blit_tiled(im, im->w / 2 - 2, 0, 4, im->h, r.left + ends, r.top, len - 2 * ends, im->h, &clip);
            blit(im, im->w - ends, 0, ends, im->h, r.right - ends, r.top, &clip);
        }
        hrgn_free(&clip);
        return true;
    }
    const image *im = img(vertical ? -8278 : -8286), *pr = img(vertical ? -8277 : -8285);
    if (!im) {
        hrgn_free(&clip);
        return false;
    }
    int a = 16; /* arrow length in the sample */
    if (vertical) {
        int sl = im->h - 2 * a, cap = sl > 12 ? 4 : 0, mid = sl - 2 * cap;
        blit(hilite == 20 && pr ? pr : im, 0, 0, im->w, a, r.left, r.top, &clip);
        blit(im, 0, a, im->w, cap, r.left, r.top + a, &clip);
        blit_tiled(im, 0, a + cap + mid / 2 - 2, im->w, 4, r.left, r.top + a + cap, im->w, len - 2 * a - 2 * cap, &clip);
        blit(im, 0, a + sl - cap, im->w, cap, r.left, r.bottom - a - cap, &clip);
        blit(hilite == 21 && pr ? pr : im, 0, im->h - a, im->w, a, r.left, r.bottom - a, &clip);
    } else {
        int sl = im->w - 2 * a, cap = sl > 12 ? 4 : 0, mid = sl - 2 * cap;
        blit(hilite == 20 && pr ? pr : im, 0, 0, a, im->h, r.left, r.top, &clip);
        blit(im, a, 0, cap, im->h, r.left + a, r.top, &clip);
        blit_tiled(im, a + cap + mid / 2 - 2, 0, 4, im->h, r.left + a + cap, r.top, len - 2 * a - 2 * cap, im->h, &clip);
        blit(im, a + sl - cap, 0, cap, im->h, r.right - a - cap, r.top, &clip);
        blit(hilite == 21 && pr ? pr : im, im->w - a, 0, a, im->h, r.right - a, r.top, &clip);
    }
    const image *th = img(vertical ? (hilite == 129 ? -10207 : -10208) : (hilite == 129 ? -10205 : -10206));
    if (th) {
        if (vertical) {
            int half = (thumb.bottom - thumb.top) / 2;
            blit(th, 0, 0, th->w, half, thumb.left, thumb.top, &clip);
            blit(th, 0, th->h - half, th->w, half, thumb.left, thumb.top + half, &clip);
        } else {
            int half = (thumb.right - thumb.left) / 2;
            blit(th, 0, 0, half, th->h, thumb.left, thumb.top, &clip);
            blit(th, th->w - half, 0, half, th->h, thumb.left + half, thumb.top, &clip);
        }
    }
    hrgn_free(&clip);
    return true;
}

const char *theme_name(void) { return loaded ? scheme_name : ""; }

/* A cicn's image as RGBA (malloc'd), for PlotCIcon. */
uint8_t *qd_cicn_rgba(const uint8_t *d, uint32_t len, int *w, int *h)
{
    image im;
    if (!decode_cicn(d, len, &im))
        return NULL;
    *w = im.w;
    *h = im.h;
    return im.rgba;
}
