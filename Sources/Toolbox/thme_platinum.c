/*
 * Platinum, the look Apple's own theme files name.
 *
 * Mac OS 8.5 and 9 draw Platinum in the Appearance Manager's code: the
 * "Apple platinum" file ('tthm', creator 'pltn') holds only its scenes
 * (desktop pattern, highlight colour, fonts) and colour variations ('clut'
 * 200-220), and no Apple file holds pictures of its parts.  So Blue Box
 * draws them here, from scratch, to look as Mac OS 9.2 draws them: the
 * windows' frames and title bars, the menu bar and menus.  The controls
 * are appearance.c's Platinum ones (thme.c falls back to them for the
 * parts this file doesn't draw).
 *
 * The parts answer to thme.c's layout ids and state bits, so that the
 * Window and Menu Managers use them as they use a theme file's layouts:
 * plat_draw draws one through thme_plot (which mixes the translucent
 * shadow pixels with what is under them), and plat_areas gives the
 * rectangles of the regions it makes (title bar, boxes, content).
 *
 * The frames are bevels of one-pixel rings: the window is a raised slab
 * (light on its top and left, dark on its bottom and right) with the
 * content sunk into it, a dark line round both, a light shadow a pixel
 * wide above and left of it and a dark one below and right.
 */
#include <stdlib.h>
#include <string.h>

#include "qd_internal.h"
#include "tb_internal.h"
#include "thme.h"

/* ---------------------------------------------------------------------- */
/* The colour variation */

/* Lavender, Mac OS Default's: light to dark. */
static const uint8_t lavender[8][3] = {
    { 0xEE, 0xEE, 0xEE }, { 0xCC, 0xCC, 0xFF }, { 0x99, 0x99, 0xFF }, { 0x66, 0x66, 0xCC },
    { 0x33, 0x33, 0x99 }, { 0x00, 0x00, 0x88 }, { 0x00, 0x00, 0x55 }, { 0x00, 0x00, 0x00 },
};
static uint8_t ramp[8][3];

void plat_set_ramp(const uint8_t r[8][3]) { memcpy(ramp, r ? r : lavender, sizeof ramp); }

void plat_accent(int i, uint8_t out[3])
{
    if (!ramp[0][0] && !ramp[0][1] && !ramp[0][2])
        plat_set_ramp(NULL);
    memcpy(out, ramp[i < 0 ? 0 : i > 7 ? 7 : i], 3);
}

static uint32_t accent(int i)
{
    uint8_t c[3];
    plat_accent(i, c);
    return (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2];
}

/* ---------------------------------------------------------------------- */
/* Pixels */

static uint32_t grey(int v) { return (uint32_t)v * 0x010101u; }

/* c mixed with d by t/256. */
static uint32_t blend(uint32_t c, uint32_t d, int t)
{
    uint32_t out = 0;
    for (int s = 0; s <= 16; s += 8) {
        int a = (int)(c >> s & 0xFF), b = (int)(d >> s & 0xFF);
        out |= (uint32_t)(a + (b - a) * t / 256) << s;
    }
    return out;
}

/* Drawing limited to a rectangle (when right > left). */
static qrect only_in;

static void pxa(int x, int y, uint32_t c, int alpha)
{
    if (only_in.right > only_in.left &&
        (x < only_in.left || x >= only_in.right || y < only_in.top || y >= only_in.bottom))
        return;
    uint8_t rgb[3] = { (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
    thme_plot(x, y, rgb, alpha);
}

static void px(int x, int y, uint32_t c) { pxa(x, y, c, 255); }

static void hline(int x0, int x1, int y, uint32_t c)
{
    for (int x = x0; x < x1; x++)
        px(x, y, c);
}

static void vline(int x, int y0, int y1, uint32_t c)
{
    for (int y = y0; y < y1; y++)
        px(x, y, c);
}

static void fill(qrect r, uint32_t c)
{
    for (int y = r.top; y < r.bottom; y++)
        hline(r.left, r.right, y, c);
}

static qrect inset(qrect r, int d)
{
    return (qrect){ (int16_t)(r.top + d), (int16_t)(r.left + d), (int16_t)(r.bottom - d), (int16_t)(r.right - d) };
}

static qrect rect(int t, int l, int b, int r) { return (qrect){ (int16_t)t, (int16_t)l, (int16_t)b, (int16_t)r }; }

/* A ring of pixels just inside r: its top row and left column in tl, its
 * bottom row and right column in br, the corners as given (the top right
 * and bottom left ones are where the light and dark sides meet). */
typedef struct {
    uint32_t tl, br, c_tl, c_tr, c_bl, c_br;
} ring_c;

static void ring(qrect r, ring_c c)
{
    int x0 = r.left, y0 = r.top, x1 = r.right - 1, y1 = r.bottom - 1;
    if (x1 < x0 || y1 < y0)
        return;
    hline(x0 + 1, x1, y0, c.tl);
    vline(x0, y0 + 1, y1, c.tl);
    hline(x0 + 1, x1, y1, c.br);
    vline(x1, y0 + 1, y1, c.br);
    px(x0, y0, c.c_tl);
    px(x1, y0, c.c_tr);
    px(x0, y1, c.c_bl);
    px(x1, y1, c.c_br);
}

static ring_c plain(uint32_t tl, uint32_t br, uint32_t corner)
{
    return (ring_c){ tl, br, tl, corner, corner, br };
}

/* The shadow round a frame's outer rectangle o: a pixel above and left
 * darkened a little, one below and right darkened much (alphas out of
 * 255), the corners softer; the frame's own top right and bottom left
 * corner pixels are part of the shadow. */
static void shadow(qrect o, int light, int dark)
{
    int x0 = o.left, y0 = o.top, x1 = o.right - 1, y1 = o.bottom - 1;
    int soft = dark / 4, corner = dark * 3 / 4;
    pxa(x0 - 1, y0 - 1, 0, light / 4);
    for (int x = x0; x < x1; x++)
        pxa(x, y0 - 1, 0, light);
    pxa(x1, y0 - 1, 0, light * 3 / 4);
    for (int y = y0; y < y1; y++)
        pxa(x0 - 1, y, 0, light);
    pxa(x0 - 1, y1, 0, light * 3 / 4);
    pxa(x1 + 1, y0 + 1, 0, soft);
    for (int y = y0 + 2; y <= y1; y++)
        pxa(x1 + 1, y, 0, dark);
    pxa(x0 + 1, y1 + 1, 0, soft);
    for (int x = x0 + 2; x <= x1; x++)
        pxa(x, y1 + 1, 0, dark);
    pxa(x1 + 1, y1 + 1, 0, corner);
    /* the cut corners */
    pxa(x1, y0, 0, dark);
    pxa(x1, y0 + 1, 0, dark + (255 - dark) / 4);
    pxa(x0, y1, 0, dark);
    pxa(x0 + 1, y1, 0, dark + (255 - dark) / 4);
}

/* ---------------------------------------------------------------------- */
/* Title bar widgets: a box sunk into the bar, with a face graded from
 * dark at its top left to light at its bottom right; n is the well's
 * size (14 in a document's title bar, 11 in a utility window's). */

enum { GLYPH_CLOSE, GLYPH_ZOOM, GLYPH_COLLAPSE, GLYPH_COLLAPSE_SIDE };

static int face_value(int k, int kmax)
{
    return 0x9A + (0xEF - 0x9A) * k / (kmax ? kmax : 1);
}

static void widget(int x, int y, int n, int glyph, bool pressed)
{
    /* the well */
    ring(rect(y, x, y + n, x + n), (ring_c){ 0xBBBBBB, 0xF2F2F2, 0xC8C8C8, 0xCCCCCC, 0xCCCCCC, 0xE9E9E9 });
    px(x + n - 1, y + 1, 0xD6D6D6);
    px(x + 1, y + n - 1, 0xD6D6D6);
    px(x + n - 2, y, 0xBFBFBF);
    px(x, y + n - 2, 0xBFBFBF);
    /* the box's edge, and the ring inside it */
    ring(rect(y + 1, x + 1, y + n - 1, x + n - 1), (ring_c){ 0x6F6F6F, 0x595959, 0x828282, 0x898989, 0x898989, 0x838383 });
    int ix = x + 2, iy = y + 2, in = n - 4;
    ring(rect(iy, ix, iy + in, ix + in), (ring_c){ 0x4D4D4D, 0x6F6F6F, 0x303030, 0x424242, 0x424242, 0x5B5B5B });
    px(ix + 1, iy, 0x565656);
    px(ix, iy + 1, 0x565656);
    px(ix + in - 1, iy + 1, 0x959595);
    px(ix + 1, iy + in - 1, 0x959595);
    /* the face */
    int fx = ix + 1, fy = iy + 1, f = in - 2, kmax = 2 * (f - 2);
    for (int j = 0; j < f; j++)
        for (int i = 0; i < f; i++) {
            int v;
            if (i == 0 && j == 0)
                v = 0xE5;
            else if (i == 0 || j == 0)
                v = 0xBF + 2 * (i + j);
            else if (i == f - 1 && j == f - 1)
                v = face_value(kmax, kmax) * 85 / 100;
            else if (i == f - 1 || j == f - 1)
                v = face_value(i + j - 2 + 1, kmax) * 96 / 100;
            else
                v = face_value(i + j - 2, kmax);
            if (v > 0xFF)
                v = 0xFF;
            if (pressed)
                v = v * 62 / 100;
            px(fx + i, fy + j, grey(v));
        }
    /* The glyph: grooves cut into the face (a mid line, then a dark one),
     * running on into the ring round it. */
    int g = f > 6 ? 2 : 1; /* a zoom box's small box: 0..f-3 (two less in utility windows) */
    if (glyph == GLYPH_ZOOM) {
        int e = f - 1 - g; /* its right and bottom */
        for (int k = -1; k < e; k++) {
            int dk = 0x45 + 2 * (k < 0 ? 0 : k);
            /* the dark lines */
            px(fx + e, fy + k, k < 0 ? 0x2D2D2D : grey(dk));
            px(fx + k, fy + e, k < 0 ? 0x2D2D2D : grey(dk));
            /* the mid lines */
            if (k < e - 1) {
                int a = k < 0 ? 0x42 : face_value(k, kmax) * 79 / 100;
                px(fx + e - 1, fy + k, grey(a));
                px(fx + k, fy + e - 1, grey(a));
            }
        }
        px(fx + e - 1, fy + e - 1, 0x828282);
        px(fx + e, fy + e, 0x757575);
    } else if (glyph == GLYPH_COLLAPSE || glyph == GLYPH_COLLAPSE_SIDE) {
        bool turned = glyph == GLYPH_COLLAPSE_SIDE; /* bars down a side window's title bar */
        for (int b = 0; b < 2; b++) {
            int row = (f > 6 ? 2 : 1) + b * 2;
            for (int i = -1; i <= f; i++) {
                int a, d;
                if (i < 0) {
                    a = 0x42;
                    d = 0x2D;
                } else if (i >= f) {
                    a = 0x5B;
                    d = 0x35;
                } else {
                    a = face_value(i + row - 2 < 0 ? 0 : i + row - 2, kmax) * 79 / 100;
                    d = 0x45 + 2 * i;
                }
                if (pressed && i >= 0 && i < f) {
                    a = a * 62 / 100;
                    d = d * 62 / 100;
                }
                if (turned) {
                    px(fx + row, fy + i, grey(a));
                    px(fx + row + 1, fy + i, grey(d));
                } else {
                    px(fx + i, fy + row, grey(a));
                    px(fx + i, fy + row + 1, grey(d));
                }
            }
        }
    }
}

/* A document title bar's racing stripes, 13 rows from y, over [x0, x1):
 * grooves with a light end on the left and a dark one on the right. */
static void stripes(int x0, int x1, int y)
{
    if (x1 - x0 < 4)
        return;
    for (int r = 0; r < 13; r++) {
        bool top = r == 0, last = r == 12, light = !top && !last && (r & 1);
        uint32_t c0 = top ? 0xCFCFCF : last ? 0xCCCCCC : light ? 0xD6D6D6 : 0xCFCFCF;
        uint32_t c1 = top ? 0xD9D9D9 : last ? 0xBCBCBC : light ? 0xEDEDED : 0xC9C9C9;
        uint32_t mid = top ? 0xD9D9D9 : last ? 0x8C8C8C : light ? 0xDDDDDD : 0x999999;
        uint32_t e0 = top ? 0xD6D6D6 : last ? 0x8C8C8C : light ? 0xD3D3D3 : 0x969696;
        uint32_t e1 = top ? 0xCCCCCC : last ? 0x9C9C9C : light ? 0xBCBCBC : 0x9C9C9C;
        px(x0, y + r, c0);
        px(x0 + 1, y + r, c1);
        hline(x0 + 2, x1 - 2, y + r, mid);
        px(x1 - 2, y + r, e0);
        px(x1 - 1, y + r, e1);
    }
}

/* A utility window's title bar texture: little dimples, three pixels
 * apart each way, over [x0, x1) x [y0, y1). */
static void dimples(int x0, int x1, int y0, int y1, bool vertical)
{
    static const uint32_t t[3][3] = { { 0xCFCFCF, 0xD6D6D6, 0xCCCCCC },
                                      { 0xD6D6D6, 0xE3E3E3, 0xBCBCBC },
                                      { 0xCCCCCC, 0xBCBCBC, 0x9C9C9C } };
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            int u = vertical ? y - y0 : x - x0, v = vertical ? x - x0 : y - y0;
            px(x, y, vertical ? t[u % 3][v % 3] : t[v % 3][u % 3]);
        }
}

/* ---------------------------------------------------------------------- */
/* Windows */

/* The content's colour: a dialog's frame runs into it. */
static uint32_t content = 0xFFFFFF;

void plat_set_content(const uint8_t rgb[3]) { content = (uint32_t)rgb[0] << 16 | (uint32_t)rgb[1] << 8 | rgb[2]; }

enum {
    ST_GROW = 0x01, ST_RTL = 0x02, ST_ZOOM = 0x18, ST_CLOSE = 0x20, ST_COLLAPSE = 0x40, ST_TITLED = 0x80,
    ST_COLLAPSED = 0x100,
};

typedef struct {
    int kind;           /* 0 document, 1 dialog, 2 movable modal, 3 utility, 4 side utility */
    bool active;
    qrect o;            /* the frame's outer rectangle */
    qrect bar;          /* the title bar (drag) */
    qrect close, zoom, collapse, text;
    bool has_close, has_zoom, has_collapse, has_grow, collapsed;
    qrect grow;
} wgeom;

static bool window_part(int id) { return id >= 128 && id <= 139; }

static void geometry(int id, qrect c, uint32_t st, const int *m, wgeom *g)
{
    memset(g, 0, sizeof *g);
    g->kind = id < 130 ? 0 : id < 134 ? 1 : id < 136 ? 2 : id < 138 ? 3 : 4;
    g->active = !(id & 1);
    g->collapsed = st & ST_COLLAPSED;
    g->has_close = (st & ST_CLOSE) && g->kind != 1 && g->kind != 2;
    g->has_zoom = (st & ST_ZOOM) && g->kind != 1 && g->kind != 2;
    g->has_collapse = (st & ST_COLLAPSE) && g->kind != 1 && g->kind != 2;
    g->has_grow = (st & ST_GROW) && !g->collapsed;
    int tw = m ? m[1] : 0;
    switch (g->kind) {
    case 0: {
        g->o = rect(c.top - 22, c.left - 6, g->collapsed ? c.top : c.bottom + 6, c.right + 6);
        g->bar = rect(g->o.top, g->o.left, c.top - 1, g->o.right);
        int y = g->o.top + 3, r = g->o.right - 5;
        g->close = rect(y, g->o.left + 3, y + 14, g->o.left + 17);
        if (g->has_collapse) {
            g->collapse = rect(y, r - 13, y + 14, r + 1);
            r -= 16;
        }
        if (g->has_zoom)
            g->zoom = rect(y, r - 13, y + 14, r + 1);
        int mid = (g->o.left + g->o.right) / 2;
        g->text = rect(y, mid - tw / 2, y + 14, mid - tw / 2 + tw);
        break;
    }
    case 1:
        g->o = inset(c, -6);
        break;
    case 2: {
        g->o = rect(c.top - 27, c.left - 6, c.bottom + 6, c.right + 6);
        g->bar = rect(g->o.top, g->o.left, g->o.top + 21, g->o.right);
        int mid = (g->o.left + g->o.right) / 2;
        g->text = rect(g->o.top + 3, mid - tw / 2, g->o.top + 17, mid - tw / 2 + tw);
        break;
    }
    case 3: {
        g->o = rect(c.top - 16, c.left - 4, c.bottom + 4, c.right + 4);
        g->bar = rect(g->o.top, g->o.left, c.top - 1, g->o.right);
        int y = g->o.top + 2;
        g->close = rect(y, g->o.left + 1, y + 11, g->o.left + 12);
        int r = g->o.right - 3;
        if (g->has_collapse) {
            g->collapse = rect(y, r - 10, y + 11, r + 1);
            r -= 13;
        }
        if (g->has_zoom)
            g->zoom = rect(y, r - 10, y + 11, r + 1);
        int mid = (g->o.left + g->o.right) / 2;
        g->text = rect(y, mid - tw / 2, y + 11, mid - tw / 2 + tw);
        break;
    }
    default: {
        g->o = rect(c.top - 4, c.left - 16, c.bottom + 4, c.right + 4);
        g->bar = rect(g->o.top, g->o.left, g->o.bottom, c.left - 1);
        int x = g->o.left + 2;
        g->close = rect(g->o.top + 1, x, g->o.top + 12, x + 11);
        int b = g->o.bottom - 3;
        if (g->has_collapse) {
            g->collapse = rect(b - 10, x, b + 1, x + 11);
            b -= 13;
        }
        if (g->has_zoom)
            g->zoom = rect(b - 10, x, b + 1, x + 11);
        break;
    }
    }
    if (g->has_grow)
        g->grow = g->kind >= 3 ? rect(c.bottom - 10, c.right - 10, c.bottom, c.right)
                               : rect(c.bottom - 15, c.right - 15, c.bottom, c.right);
}

/* The slab and the well sunk into it, round a document window's content:
 * from the outer frame in, the rings of either side. */
static void doc_frame(const wgeom *g, qrect c)
{
    qrect o = g->o;
    bool a = g->active;
    uint32_t bar = a ? 0xCCCCCC : 0xDDDDDD;
    /* outer line */
    if (a)
        ring(o, (ring_c){ 0x404040, 0x000000, 0x101010, 0x000000, 0x000000, 0x000000 });
    else
        ring(o, plain(0x555555, 0x555555, 0x555555));
    /* the slab: two rings, light top and left, dark bottom and right */
    ring(inset(o, 1), a ? (ring_c){ 0xF2F2F2, 0x737373, 0xFCFCFC, 0x8F8F8F, 0x8F8F8F, 0x737373 }
                        : plain(0xDDDDDD, 0xBBBBBB, 0xCCCCCC));
    ring(inset(o, 2), a ? plain(bar, 0xBFBFBF, 0xBFBFBF) : plain(bar, 0xDDDDDD, 0xDDDDDD));
    if (a) {
        px(o.right - 2, o.top, 0x262626);
        px(o.right - 3, o.top, 0x3D3D3D);
        px(o.left, o.bottom - 2, 0x262626);
        px(o.left, o.bottom - 3, 0x3D3D3D);
        px(o.right - 3, o.top + 1, 0xE5E5E5);
        px(o.left + 1, o.bottom - 3, 0xE5E5E5);
    }
    /* the title bar */
    fill(rect(o.top + 3, o.left + 3, g->collapsed ? o.bottom - 3 : c.top - 3, o.right - 3), bar);
    if (g->collapsed)
        return;
    /* the well: dark top and left, light bottom and right, then the line */
    ring(inset(c, -3), a ? (ring_c){ 0xBFBFBF, 0xCCCCCC, 0xC9C9C9, 0xCCCCCC, 0xCCCCCC, 0xCCCCCC }
                         : plain(bar, bar, bar));
    ring(inset(c, -2), a ? (ring_c){ 0x737373, 0xF2F2F2, 0x8F8F8F, 0xD6D6D6, 0xD6D6D6, 0xF2F2F2 }
                         : (ring_c){ 0xBBBBBB, bar, 0xD4D4D4, bar, bar, bar });
    if (a) {
        px(c.right, c.top - 2, 0x8C8C8C);
        px(c.left - 2, c.bottom, 0x8C8C8C);
        px(c.right - 1, c.top - 3, 0xC2C2C2);
        px(c.right, c.top - 3, 0xCCCCCC);
        px(c.left - 3, c.bottom - 1, 0xC2C2C2);
        px(c.left - 3, c.bottom, 0xCCCCCC);
    }
    ring(inset(c, -1), a ? (ring_c){ 0x404040, 0x404040, 0x101010, 0x404040, 0x404040, 0x404040 }
                         : (ring_c){ 0x808080, 0x808080, 0x606060, 0x808080, 0x808080, 0x808080 });
}

/* The grow box: ridges across the corner, the box raised out of the
 * frame (so it covers the well's corner too), and the lines along the
 * scroll bars' sides. */
static void grow_box(qrect c, bool active, bool frame_only, bool small)
{
    if (!active)
        return;
    int n = small ? 10 : 15, out = small ? 1 : 2;
    qrect b = rect(c.bottom - (n - 1), c.right - (n - 1), c.bottom + out, c.right + out);
    for (int y = b.top; y < b.bottom; y++)
        for (int x = b.left; x < b.right; x++) {
            if (frame_only && x < c.right && y < c.bottom)
                continue;
            uint32_t v = 0xCCCCCC;
            if (small && (x == b.right - 1 || y == b.bottom - 1))
                v = 0xBFBFBF;
            if (y == b.top || x == b.left)
                v = (x == b.left && y == b.top) ? 0xFCFCFC
                    : (x == b.right - 1 || y == b.bottom - 1) ? (small ? 0xDCDCDC : 0xE9E9E9) : 0xF2F2F2;
            px(x, y, v);
        }
    if (frame_only)
        return;
    /* the ridges: bands across the diagonal, light then dark */
    static const uint32_t band[13] = { 0xCFCFCF, 0xDFDFDF, 0xE3E3E3, 0xACACAC, 0x9F9F9F, 0xDFDFDF, 0xE3E3E3,
                                       0xACACAC, 0x9F9F9F, 0xDFDFDF, 0xE3E3E3, 0xACACAC, 0x9C9C9C };
    int size = small ? 9 : 12, nb = small ? 9 : 13, skew = small ? 3 : 5;
    int x0 = c.right - (small ? 9 : 12), y0 = c.bottom - (small ? 8 : 12);
    for (int j = 0; j < size; j++)
        for (int i = 0; i < size; i++) {
            int u = i + j - skew; /* along the diagonal */
            int v = i - j;        /* across it */
            if (u < 0 || u >= nb || v < -7 || v > 7)
                continue;
            uint32_t col = u == nb - 1 ? 0x9C9C9C : band[u];
            /* the ends of each ridge, lit */
            int ridge = u < 5 ? 0 : (u - 5) / 4 + 1, len = 2 + ridge * 2 - (small ? 1 : 0);
            if (v >= len - 1 || v <= -len) {
                if (v > 0)
                    col = (u % 4 == 1 || u == 0) ? 0xD9D9D9 : 0xD6D6D6;
                else
                    col = (u % 4 == 3) ? 0xA9A9A9 : (u % 4 == 2) ? 0xC6C6C6 : 0xD3D3D3;
                if ((v > 0 && v > len) || (v < 0 && -v > len))
                    continue;
            }
            px(x0 + i, y0 + j, col);
        }
    /* the scroll bars' sides */
    hline(c.right - (n - 1), c.right, c.bottom - n, 0x404040);
    px(c.right, c.bottom - n, 0x707070);
    vline(c.right - n, c.bottom - (n - 1), c.bottom, 0x404040);
    px(c.right - n, c.bottom, 0x707070);
    px(c.right - n, c.bottom - n, 0x101010);
}

static void doc_title_bar(const wgeom *g, uint32_t pressed)
{
    if (!g->active)
        return;
    int y = g->o.top + 3;
    if (g->has_close)
        widget(g->close.left, g->close.top, 14, GLYPH_CLOSE, pressed & THME_CLOSE);
    if (g->has_zoom)
        widget(g->zoom.left, g->zoom.top, 14, GLYPH_ZOOM, pressed & THME_ZOOM);
    if (g->has_collapse)
        widget(g->collapse.left, g->collapse.top, 14, GLYPH_COLLAPSE, pressed & THME_COLLAPSE);
    int x0 = g->kind == 2 ? g->o.left + 4 : g->close.right + 3;
    int x1 = g->has_zoom ? g->zoom.left - 3 : g->has_collapse ? g->collapse.left - 3 : g->o.right - 5;
    if (g->text.right > g->text.left) {
        stripes(x0, g->text.left - 3, y);
        stripes(g->text.right + 3, x1, y);
    } else {
        stripes(x0, x1, y);
    }
}

static void dialog_frame(const wgeom *g)
{
    qrect o = g->o;
    if (g->active) {
        ring(o, (ring_c){ 0x2F2F2F, 0x000000, 0x0C0C0C, 0x000000, 0x000000, 0x000000 });
        ring(inset(o, 1), (ring_c){ 0xCCCCCC, 0x404040, 0xBFBFBF, 0x797979, 0x797979, 0x303030 });
        ring(inset(o, 2), (ring_c){ 0xF6F6F6, 0x888888, 0xFDFDFD, 0x9B9B9B, 0x9B9B9B, 0x7B7B7B });
        ring(inset(o, 3), plain(content, 0xCCCCCC, 0xCCCCCC));
        px(o.right - 2, o.top, 0x232323);
        px(o.left, o.bottom - 2, 0x232323);
        px(o.right - 3, o.top + 1, 0xB5B5B5);
        px(o.left + 1, o.bottom - 3, 0xB5B5B5);
        px(o.right - 4, o.top + 1, 0xC8C8C8);
        px(o.left + 1, o.bottom - 4, 0xC8C8C8);
        px(o.right - 4, o.top + 2, 0xE5E5E5);
        px(o.left + 2, o.bottom - 4, 0xE5E5E5);
    } else {
        ring(o, plain(0x555555, 0x555555, 0x555555));
        ring(inset(o, 1), plain(0xDDDDDD, 0xBBBBBB, 0xCCCCCC));
        ring(inset(o, 2), plain(0xDDDDDD, 0xDDDDDD, 0xDDDDDD));
        ring(inset(o, 3), plain(content, content, content));
    }
    ring(inset(o, 4), plain(content, content, content));
    ring(inset(o, 5), plain(content, content, content));
}

/* A movable modal dialog: a document's title bar (stripes, no boxes) on
 * a dialog's frame. */
static void movable_frame(const wgeom *g, qrect c)
{
    qrect o = g->o, top = rect(o.top, o.left, o.top + 22, o.right);
    bool a = g->active;
    uint32_t bar = a ? 0xCCCCCC : 0xDDDDDD;
    /* the title bar: the outer line, the slab's light rings, its dark ones
     * at the bottom */
    hline(o.left + 1, o.right - 1, o.top, a ? 0x404040 : 0x555555);
    px(o.left, o.top, a ? 0x101010 : 0x555555);
    vline(o.left, o.top + 1, top.bottom, a ? 0x404040 : 0x555555);
    vline(o.right - 1, o.top + 2, top.bottom, a ? 0x000000 : 0x555555);
    hline(o.left + 2, o.right - 2, o.top + 1, a ? 0xF2F2F2 : 0xDDDDDD);
    px(o.left + 1, o.top + 1, a ? 0xFCFCFC : 0xDDDDDD);
    vline(o.left + 1, o.top + 2, top.bottom - 2, a ? 0xF2F2F2 : 0xDDDDDD);
    px(o.right - 2, o.top + 1, a ? 0x8F8F8F : 0xCCCCCC);
    vline(o.right - 2, o.top + 2, top.bottom - 1, a ? 0x737373 : 0xBBBBBB);
    vline(o.right - 3, o.top + 2, top.bottom - 2, a ? 0xBFBFBF : bar);
    fill(rect(o.top + 2, o.left + 2, o.top + 19, o.right - 3), bar);
    hline(o.left + 1, o.right - 2, o.top + 19, a ? 0xBFBFBF : bar);
    hline(o.left + 1, o.right - 2, o.top + 20, a ? 0x737373 : 0xBBBBBB);
    if (a) {
        px(o.left, o.top + 19, 0x3D3D3D);
        px(o.left + 1, o.top + 19, 0xE5E5E5);
        px(o.left, o.top + 20, 0x262626);
        px(o.left + 1, o.top + 20, 0x8F8F8F);
        px(o.right - 3, o.top + 1, 0xE5E5E5);
        px(o.right - 3, o.top + 19, 0xB6B6B6);
        px(o.right - 2, o.top + 20, 0x565656);
        px(o.right - 2, o.top + 21, 0x292929);
    }
    /* the body: a dialog's rings, from the line under the title bar */
    qrect b = rect(o.top + 21, o.left, o.bottom, o.right);
    if (a) {
        ring(b, (ring_c){ 0x373737, 0x000000, 0x0E0E0E, 0x000000, 0x000000, 0x000000 });
        ring(inset(b, 1), (ring_c){ 0xE5E5E5, 0x404040, 0xDFDFDF, 0x8C8C8C, 0x404040, 0x303030 });
        ring(inset(b, 2), (ring_c){ content, 0x888888, content, 0xAEAEAE, 0x888888, 0x7B7B7B });
        ring(inset(b, 3), (ring_c){ content, 0xE5E5E5, content, 0xF2F2F2, 0xE5E5E5, 0xD2D2D2 });
    } else {
        ring(b, plain(0x555555, 0x555555, 0x555555));
        ring(inset(b, 1), plain(0xDDDDDD, 0xBBBBBB, 0xCCCCCC));
        ring(inset(b, 2), plain(content, content, content));
        ring(inset(b, 3), plain(content, content, content));
    }
    ring(inset(b, 4), plain(content, content, content));
    ring(inset(b, 5), plain(content, content, content));
    (void)c;
    if (a) {
        int x1 = o.right - 5, y = o.top + 3;
        if (g->text.right > g->text.left) {
            stripes(o.left + 4, g->text.left - 3, y);
            stripes(g->text.right + 3, x1, y);
        } else {
            stripes(o.left + 4, x1, y);
        }
    }
}

/* Utility windows: a thinner slab and well, a short title bar (on top, or
 * down the left side) with small boxes and a dimpled texture. */
static void utility_frame(const wgeom *g, qrect c, uint32_t pressed)
{
    qrect o = g->o;
    bool a = g->active, side = g->kind == 4;
    uint32_t bar = a ? 0xCCCCCC : 0xDDDDDD;
    if (a)
        ring(o, (ring_c){ 0x404040, 0x000000, 0x101010, 0x000000, 0x000000, 0x000000 });
    else
        ring(o, plain(0x777777, 0x777777, 0x777777));
    ring(inset(o, 1), a ? (ring_c){ 0xE5E5E5, 0x737373, 0xFCFCFC, 0x8F8F8F, 0x8F8F8F, 0x737373 }
                        : plain(0xDDDDDD, 0xDDDDDD, 0xDDDDDD));
    if (a) {
        /* the title bar's own light edge */
        if (side)
            vline(o.left + 1, o.top + 1, o.bottom - 2, 0xF2F2F2);
        else
            hline(o.left + 1, o.right - 2, o.top + 1, 0xF2F2F2);
        px(o.right - 2, o.top, 0x262626);
        px(o.right - 3, o.top, 0x3D3D3D);
        px(o.left, o.bottom - 2, 0x262626);
        px(o.left, o.bottom - 3, 0x3D3D3D);
        px(o.right - 3, o.top + 1, 0xE5E5E5);
        px(o.left + 1, o.bottom - 3, 0xE5E5E5);
    }
    /* the title bar */
    qrect t = side ? rect(o.top + 2, o.left + 2, o.bottom - 2, c.left - 3) : rect(o.top + 2, o.left + 2, c.top - 3, o.right - 2);
    fill(t, bar);
    if (side) {
        vline(c.left - 3, o.top + 2, o.bottom - 2, a ? 0xBFBFBF : bar);
        px(c.left - 3, o.bottom - 2, a ? 0xC9C9C9 : bar);
    } else {
        hline(o.left + 2, o.right - 2, c.top - 3, a ? 0xBFBFBF : bar);
        px(o.right - 2, c.top - 3, a ? 0xBFBFBF : 0xDDDDDD);
    }
    /* the well and the line round the content */
    ring(inset(c, -2), a ? (ring_c){ 0x737373, 0xE5E5E5, 0x8F8F8F, 0xC9C9C9, 0xC9C9C9, 0xE5E5E5 }
                         : (ring_c){ 0xBBBBBB, 0xDDDDDD, 0xD4D4D4, 0xDDDDDD, 0xDDDDDD, 0xDDDDDD });
    ring(inset(c, -1), a ? (ring_c){ 0x404040, 0x404040, 0x101010, 0x404040, 0x404040, 0x404040 }
                         : (ring_c){ 0x808080, 0x808080, 0x606060, 0x808080, 0x808080, 0x808080 });
    if (!a)
        return;
    if (g->has_close)
        widget(g->close.left, g->close.top, 11, GLYPH_CLOSE, pressed & THME_CLOSE);
    if (g->has_zoom)
        widget(g->zoom.left, g->zoom.top, 11, GLYPH_ZOOM, pressed & THME_ZOOM);
    if (g->has_collapse)
        widget(g->collapse.left, g->collapse.top, 11, side ? GLYPH_COLLAPSE_SIDE : GLYPH_COLLAPSE, pressed & THME_COLLAPSE);
    if (side) {
        int y0 = g->close.bottom + 2, y1 = g->has_zoom ? g->zoom.top - 2 : g->has_collapse ? g->collapse.top - 2 : o.bottom - 4;
        if (y1 > y0)
            dimples(g->close.left + 1, g->close.right - 1, y0, y1, true);
    } else {
        int x0 = g->close.right + 1, x1 = g->has_zoom ? g->zoom.left - 2 : g->has_collapse ? g->collapse.left - 2 : o.right - 4;
        int y0 = g->close.top + 1, y1 = g->close.bottom - 1;
        if (g->text.right > g->text.left) {
            if (g->text.left - 3 > x0)
                dimples(x0, g->text.left - 3, y0, y1, false);
            if (x1 > g->text.right + 3)
                dimples(g->text.right + 3, x1, y0, y1, false);
        } else if (x1 > x0) {
            dimples(x0, x1, y0, y1, false);
        }
    }
}

static void draw_window(int id, qrect c, uint32_t st, const int *m, uint32_t pressed, uint32_t only)
{
    wgeom g;
    geometry(id, c, st, m, &g);
    if (only) {
        if ((only & THME_GROW) && g.has_grow && (g.kind == 0 || g.kind >= 3))
            grow_box(c, g.active, false, g.kind >= 3);
        return;
    }
    shadow(g.o, g.active ? 64 : 26, g.active ? 191 : 26);
    switch (g.kind) {
    case 0:
        doc_frame(&g, c);
        doc_title_bar(&g, pressed);
        if (g.has_grow)
            grow_box(c, g.active, true, false);
        break;
    case 1: dialog_frame(&g); break;
    case 2: movable_frame(&g, c); break;
    default:
        utility_frame(&g, c, pressed);
        if (g.has_grow)
            grow_box(c, g.active, true, true);
        break;
    }
}

static int window_areas(int id, qrect c, uint32_t st, const int *m, plat_area *out, int max)
{
    wgeom g;
    geometry(id, c, st, m, &g);
    int n = 0;
#define AREA(f, r)                                                                                                 \
    do {                                                                                                           \
        if (n < max)                                                                                               \
            out[n++] = (plat_area){ (uint16_t)(f), (r) };                                                          \
    } while (0)
    AREA(THME_STRUCTURE, g.o);
    if (g.bar.right > g.bar.left)
        AREA(THME_TITLE_BAR, g.bar);
    if ((st & ST_TITLED) && g.text.right > g.text.left)
        AREA(THME_TITLE_TEXT, g.text);
    if (g.has_close)
        AREA(THME_CLOSE, g.close);
    if (g.has_zoom)
        AREA(THME_ZOOM, g.zoom);
    if (g.has_collapse)
        AREA(THME_COLLAPSE, g.collapse);
    if (g.has_grow)
        AREA(THME_GROW, g.grow);
    if (!g.collapsed)
        AREA(THME_CONTENT, c);
    return n;
}

/* ---------------------------------------------------------------------- */
/* Menus: 256 the bar, 257-259 a menu's background (pull-down, pop-up,
 * hierarchical), 260 a title, 261 an item, 263 a divider */

enum { MENU_BAR = 256, MENU_PULLDOWN = 257, MENU_POPUP = 258, MENU_HIER = 259, MENU_TITLE = 260, MENU_ITEM = 261,
       MENU_DIVIDER = 263 };

/* The screen's rounded top corners, in the bar's ends: black outside a
 * circle, mixed along its edge. */
static void bar_corner(int x0, int y0, bool right)
{
    const int rad = 5;
    for (int j = 0; j < rad + 1; j++)
        for (int i = 0; i < rad + 1; i++) {
            /* coverage of the pixel by the outside of the circle, sampled */
            int out = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    double fx = i + (sx + 0.5) / 4.0, fy = j + (sy + 0.5) / 4.0;
                    double dx = rad + 0.5 - fx, dy = rad + 0.5 - fy;
                    if (dx > 0 && dy > 0 && dx * dx + dy * dy > (rad + 0.5) * (rad + 0.5))
                        out++;
                }
            if (out)
                pxa(right ? x0 - i : x0 + i, y0 + j, 0, out * 255 / 16);
        }
}

static void menu_bar(qrect r, bool selected_only)
{
    (void)selected_only;
    hline(r.left, r.right, r.top, 0xB8B8B8);
    fill(rect(r.top + 1, r.left, r.bottom - 3, r.right), 0xDDDDDD);
    hline(r.left, r.right, r.bottom - 3, 0xCCCCCC);
    hline(r.left, r.right, r.bottom - 2, 0x737373);
    hline(r.left, r.right, r.bottom - 1, 0x333333);
    /* the corners' light rims, then the black outside them */
    if (r.left == 0 && r.top == 0) {
        bar_corner(r.left, r.top, false);
        bar_corner(r.right - 1, r.top, true);
    }
}

static void menu_title(qrect r, uint32_t st)
{
    if (!(st & 0x20))
        return;
    uint32_t a = accent(4);
    hline(r.left, r.right, r.top, blend(0xB8B8B8, a, 218));
    fill(rect(r.top + 1, r.left, r.bottom - 3, r.right), a);
    hline(r.left, r.right, r.bottom - 3, blend(a, accent(5), 64));
    hline(r.left, r.right, r.bottom - 2, blend(accent(5), accent(6), 200));
}

/* A menu's frame round its rectangle: a dark line, a light ring inside
 * on its top and left, two dark ones on its bottom and right, and the
 * shadow. */
static qrect menu_frame(qrect r) { return rect(r.top - 1, r.left - 1, r.bottom + 1, r.right + 1); }

/* The menus drawn lately (a submenu's over its parent's), for their items
 * to leave the frame's rings inside the menu alone. */
static qrect menus[4];

static void menu_rings(qrect o)
{
    ring(o, (ring_c){ 0x404040, 0x090909, 0x101010, 0x090909, 0x090909, 0x0F0F0F });
    px(o.right - 2, o.top, 0x292929);
    px(o.right - 3, o.top, 0x3E3E3E);
    px(o.left, o.bottom - 2, 0x292929);
    px(o.left, o.bottom - 3, 0x3E3E3E);
    ring(inset(o, 1), (ring_c){ 0xF6F6F6, 0x737373, 0xFDFDFD, 0x999999, 0x999999, 0x565656 });
    px(o.right - 3, o.top + 1, 0xECECEC);
    px(o.left + 1, o.bottom - 3, 0xECECEC);
    ring(inset(o, 2), plain(0xDDDDDD, 0xCCCCCC, 0xCCCCCC));
    px(o.right - 3, o.bottom - 3, 0xBFBFBF);
}

static void menu_background(qrect r)
{
    int k = 0;
    while (k < 3 && !(menus[k].left == r.left && menus[k].top == r.top))
        k++;
    memmove(&menus[1], &menus[0], sizeof menus[0] * (size_t)k);
    menus[0] = r;
    qrect o = menu_frame(r);
    shadow(o, 64, 166);
    menu_rings(o);
    fill(inset(o, 3), 0xDDDDDD);
}

/* An item's rect is the menu's width: its frame's rings over it again. */
static void item_rings(qrect r)
{
    for (int k = 0; k < 4; k++) {
        qrect m = menus[k];
        if (m.left == r.left && m.right == r.right && r.top >= m.top && r.bottom <= m.bottom) {
            only_in = r;
            menu_rings(menu_frame(m));
            only_in = (qrect){ 0, 0, 0, 0 };
            return;
        }
    }
}

static void menu_item(qrect r, uint32_t st)
{
    fill(r, (st & 0x20) && (st & 0x02) ? accent(4) : 0xDDDDDD);
    item_rings(r);
    /* a submenu's arrow */
    if (st & 0x04) {
        uint32_t c = (st & 0x20) ? 0xFFFFFF : (st & 0x02) ? 0x000000 : 0x8C8C8C;
        int x = r.right - 14, y = (r.top + r.bottom) / 2;
        for (int k = 0; k < 5; k++)
            vline(x + k, y - 4 + k, y + 5 - k, c);
    }
}

/* A divider: a dark groove across the menu, the light edge under it. */
static void menu_divider(qrect r)
{
    fill(r, 0xDDDDDD);
    int y = r.top + 2;
    item_rings(r);
    hline(r.left + 1, r.right - 2, y - 1, 0xC8C8C8);
    hline(r.left + 1, r.right - 2, y, 0xA6A6A6);
    hline(r.left + 1, r.right - 2, y + 1, 0xF6F6F6);
    /* where it meets the frame's rings */
    px(r.left, y - 1, 0xDBDBDB);
    px(r.left, y, 0xA6A6A6);
    px(r.left, y + 1, 0xFDFDFD);
    px(r.left - 1, y - 1, 0x383838);
    px(r.left - 1, y, 0x292929);
    px(r.right - 2, y - 1, 0xBBBBBB);
    px(r.right - 2, y, 0xA6A6A6);
    px(r.right - 2, y + 1, 0xF2F2F2);
    px(r.right - 1, y - 1, 0x707070);
    px(r.right - 1, y, 0x7C7C7C);
    px(r.right - 1, y + 1, 0xACACAC);
}

/* ---------------------------------------------------------------------- */

bool plat_metric(const char *tag, int *out)
{
    /* the menu bar's height and a menu divider's, as Mac OS 9 has them */
    static const struct {
        char tag[5];
        int v;
    } m[] = { { "mbht", 20 }, { "msht", 6 } };
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++)
        if (strcmp(tag, m[i].tag) == 0) {
            *out = m[i].v;
            return true;
        }
    return false;
}

bool plat_has_part(int id)
{
    return window_part(id) || id == MENU_BAR || id == MENU_PULLDOWN || id == MENU_POPUP || id == MENU_HIER ||
           id == MENU_TITLE || id == MENU_ITEM || id == MENU_DIVIDER;
}

void plat_draw(int id, qrect b, uint32_t state, const int *m, uint32_t pressed, uint32_t only)
{
    if (window_part(id)) {
        draw_window(id, b, state, m, pressed, only);
        return;
    }
    if (only)
        return;
    switch (id) {
    case MENU_BAR: menu_bar(b, state & 0x20); break;
    case MENU_TITLE: menu_title(b, state); break;
    case MENU_PULLDOWN: case MENU_POPUP: case MENU_HIER: menu_background(b); break;
    case MENU_ITEM: menu_item(b, state); break;
    case MENU_DIVIDER: menu_divider(b); break;
    default: break;
    }
}

int plat_areas(int id, qrect b, uint32_t state, const int *m, plat_area *out, int max)
{
    if (window_part(id))
        return window_areas(id, b, state, m, out, max);
    int n = 0;
    switch (id) {
    case MENU_PULLDOWN: case MENU_POPUP: case MENU_HIER:
        AREA(THME_STRUCTURE, menu_frame(b));
        AREA(THME_CONTENT, b);
        break;
    case MENU_ITEM:
        AREA(THME_CONTENT, b);
        break;
    default:
        AREA(THME_STRUCTURE, b);
        break;
    }
    return n;
#undef AREA
}
