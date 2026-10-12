/*
 * QuickDraw drawing: every shape becomes a region, is clipped to the port,
 * and is filled with a pattern and transfer mode.  Pixels are 1-bit (and
 * 8-bit for offscreen pixmaps); colour goes through the port's foreground
 * and background.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "qd_internal.h"
#include "tb_internal.h"

enum { srcCopy = 0, srcOr, srcXor, srcBic, notSrcCopy, notSrcOr, notSrcXor, notSrcBic };
enum { patCopy = 8, patOr, patXor, patBic, notPatCopy, notPatOr, notPatXor, notPatBic };

bool qd_region_recording(void);
void qd_region_record(const hrgn *shape);

/* Fill one row span [c0, c1) of bitmap row `row`. */
static void fill_span_1bpp(const pixdst *d, int row, int c0, int c1, uint8_t pat, int mode, int fg, int bk)
{
    gaddr rowp = d->base + (uint32_t)row * d->row_bytes;
    if (mode & 4)
        pat = (uint8_t)~pat;
    uint8_t fgm = fg ? 0xFF : 0x00, bkm = bk ? 0xFF : 0x00;
    for (int byte = c0 >> 3; byte <= (c1 - 1) >> 3; byte++) {
        int b0 = byte * 8 > c0 ? byte * 8 : c0;
        int b1 = byte * 8 + 8 < c1 ? byte * 8 + 8 : c1;
        uint8_t m = (uint8_t)((0xFF >> (b0 - byte * 8)) & (0xFF << (byte * 8 + 8 - b1)));
        gaddr a = rowp + (uint32_t)byte;
        uint8_t v = rd8(a), sel;
        switch (mode & 3) {
        case 0: v = (uint8_t)((v & ~m) | (((pat & fgm) | (~pat & bkm)) & m)); break;
        case 1: sel = pat & m; v = (uint8_t)((v & ~sel) | (fgm & sel)); break;
        case 2: v ^= pat & m; break;
        default: sel = pat & m; v = (uint8_t)((v & ~sel) | (bkm & sel)); break;
        }
        wr8(a, v);
    }
}

void qd_fill_region(gaddr port, const hrgn *shape, const uint8_t pat[8], int mode)
{
    if (qd_region_recording())
        qd_region_record(shape);
    hrgn r;
    hrgn_init(&r);
    qd_port_clip(port, &r);
    hrgn_op(&r, shape, &r, RGN_SECT);
    pixdst d = qd_port_pixels(port);
    int fg, bk;
    qd_port_bits(port, &fg, &bk);
    for (int i = 0; i < r.nbands; i++) {
        const rband *b = &r.bands[i];
        for (int y = b->top; y < b->bottom; y++) {
            int row = y - d.bounds.top;
            uint8_t p = pat[row & 7];
            for (int k = 0; k < b->n; k += 2) {
                int c0 = b->x[k] - d.bounds.left, c1 = b->x[k + 1] - d.bounds.left;
                if (d.depth == 1) {
                    fill_span_1bpp(&d, row, c0, c1, p, mode, fg, bk);
                } else {
                    for (int c = c0; c < c1; c++) {
                        int s = (p >> (7 - (c & 7))) & 1;
                        put_px(&d, c, row, apply_mode(mode, s, get_px(&d, c, row), fg, bk));
                    }
                }
            }
        }
    }
    hrgn_free(&r);
}

/* ---------------------------------------------------------------------- */
/* Shapes */

void qd_oval_region(hrgn *out, qrect r)
{
    hrgn_rect(out, (qrect){ 0, 0, 0, 0 });
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0)
        return;
    double a = w / 2.0, b = h / 2.0, cx = r.left + a, cy = r.top + b;
    for (int y = r.top; y < r.bottom; y++) {
        double dy = (y + 0.5 - cy) / b;
        double t = 1.0 - dy * dy;
        if (t < 0)
            continue;
        double dx = a * sqrt(t);
        int x0 = (int)lround(cx - dx), x1 = (int)lround(cx + dx);
        hrgn_add_row(out, y, x0, x1);
    }
}

void qd_rrect_region(hrgn *out, qrect r, int ow, int oh)
{
    int w = r.right - r.left, h = r.bottom - r.top;
    if (ow > w)
        ow = w;
    if (oh > h)
        oh = h;
    if (ow <= 0 || oh <= 0) {
        hrgn_rect(out, r);
        return;
    }
    hrgn_rect(out, (qrect){ 0, 0, 0, 0 });
    double a = ow / 2.0, b = oh / 2.0;
    for (int y = r.top; y < r.bottom; y++) {
        double inset = 0;
        double yc = y + 0.5;
        double dy = 0;
        if (yc < r.top + b)
            dy = (r.top + b - yc) / b;
        else if (yc > r.bottom - b)
            dy = (yc - (r.bottom - b)) / b;
        if (dy > 0) {
            double t = 1.0 - dy * dy;
            inset = a - (t > 0 ? a * sqrt(t) : 0);
        }
        hrgn_add_row(out, y, (int)lround(r.left + inset), (int)lround(r.right - inset));
    }
}

void qd_line_region(hrgn *out, int h0, int v0, int h1, int v1, int pw, int ph)
{
    hrgn_rect(out, (qrect){ 0, 0, 0, 0 });
    if (pw <= 0 || ph <= 0)
        return;
    int top = v0 < v1 ? v0 : v1, bottom = (v0 > v1 ? v0 : v1) + ph;
    int nrows = bottom - top;
    int16_t (*rows)[2] = malloc((size_t)nrows * sizeof *rows);
    for (int i = 0; i < nrows; i++) {
        rows[i][0] = 32767;
        rows[i][1] = -32768;
    }
    int dx = abs(h1 - h0), dy = -abs(v1 - v0);
    int sx = h0 < h1 ? 1 : -1, sy = v0 < v1 ? 1 : -1;
    int err = dx + dy, x = h0, y = v0;
    for (;;) {
        for (int k = 0; k < ph; k++) {
            int i = y + k - top;
            if (x < rows[i][0])
                rows[i][0] = (int16_t)x;
            if (x + pw > rows[i][1])
                rows[i][1] = (int16_t)(x + pw);
        }
        if (x == h1 && y == v1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y += sy;
        }
    }
    hrgn_from_rows(out, top, nrows, (const int16_t (*)[2])rows);
    free(rows);
}

/* Restrict a region to an arc: angles in degrees clockwise from 12 o'clock. */
static void arc_mask(hrgn *shape, qrect r, int start, int angle)
{
    if (angle >= 360 || angle <= -360)
        return;
    if (angle < 0) {
        start += angle;
        angle = -angle;
    }
    start = ((start % 360) + 360) % 360;
    double cx = (r.left + r.right) / 2.0, cy = (r.top + r.bottom) / 2.0;
    double w = r.right - r.left, h = r.bottom - r.top;
    hrgn out;
    hrgn_init(&out);
    for (int i = 0; i < shape->nbands; i++) {
        const rband *b = &shape->bands[i];
        for (int y = b->top; y < b->bottom; y++) {
            for (int k = 0; k < b->n; k += 2) {
                int run = -1;
                for (int x = b->x[k]; x <= b->x[k + 1]; x++) {
                    bool in = false;
                    if (x < b->x[k + 1]) {
                        double dx = (x + 0.5 - cx) / (w ? w : 1), dyy = (y + 0.5 - cy) / (h ? h : 1);
                        double a = atan2(dx, -dyy) * 180.0 / M_PI;
                        if (a < 0)
                            a += 360;
                        double rel = a - start;
                        if (rel < 0)
                            rel += 360;
                        in = rel <= angle;
                    }
                    if (in && run < 0)
                        run = x;
                    if (!in && run >= 0) {
                        hrgn tmp;
                        hrgn_init(&tmp);
                        hrgn_rect(&tmp, (qrect){ (int16_t)y, (int16_t)run, (int16_t)(y + 1), (int16_t)x });
                        hrgn_op(&out, &out, &tmp, RGN_UNION);
                        hrgn_free(&tmp);
                        run = -1;
                    }
                }
            }
        }
    }
    hrgn_free(shape);
    *shape = out;
}

/* ---------------------------------------------------------------------- */
/* Verbs: frame, paint, erase, invert, fill */

enum { V_FRAME, V_PAINT, V_ERASE, V_INVERT, V_FILL };

static void pen_size(gaddr port, int *pw, int *ph)
{
    uint32_t s = rd32(port + GrafPort_pnSize_off);
    *pw = pt_h(s);
    *ph = pt_v(s);
}

/* Perform a verb on a filled shape; `frame` is the shape's outline region. */
static void do_verb(int verb, const hrgn *shape, const hrgn *frame, gaddr pat)
{
    gaddr port = qd_port();
    if (!port)
        return;
    uint8_t p[8];
    switch (verb) {
    case V_FRAME:
        if (GrafPort_pnVis(port) < 0)
            return;
        qd_port_pattern(port, 0, p);
        qd_fill_region(port, frame, p, GrafPort_pnMode(port));
        break;
    case V_PAINT:
        qd_port_pattern(port, 0, p);
        qd_fill_region(port, shape, p, GrafPort_pnMode(port));
        break;
    case V_ERASE:
        qd_port_pattern(port, 1, p);
        qd_fill_region(port, shape, p, patCopy);
        break;
    case V_INVERT:
        qd_fill_region(port, shape, qd_pat_black, patXor);
        break;
    default:
        gget(p, pat, 8);
        if (!qd_port_is_color(port))
            gmemcpy(port + GrafPort_fillPat_off, pat, 8);
        qd_fill_region(port, shape, p, patCopy);
        break;
    }
}

static void verb_rect(int verb, gaddr r, gaddr pat)
{
    qrect q = qrect_get(r);
    hrgn shape, frame, inner;
    hrgn_init(&shape);
    hrgn_init(&frame);
    hrgn_init(&inner);
    hrgn_rect(&shape, q);
    if (verb == V_FRAME) {
        int pw, ph;
        pen_size(qd_port(), &pw, &ph);
        hrgn_rect(&inner, (qrect){ (int16_t)(q.top + ph), (int16_t)(q.left + pw), (int16_t)(q.bottom - ph),
                                   (int16_t)(q.right - pw) });
        hrgn_op(&frame, &shape, &inner, RGN_DIFF);
    }
    do_verb(verb, &shape, &frame, pat);
    hrgn_free(&shape);
    hrgn_free(&frame);
    hrgn_free(&inner);
}

void tb_FrameRect(gaddr r) { verb_rect(V_FRAME, r, 0); }
void tb_PaintRect(gaddr r) { verb_rect(V_PAINT, r, 0); }
void tb_EraseRect(gaddr r) { verb_rect(V_ERASE, r, 0); }
void tb_InvertRect(gaddr r) { verb_rect(V_INVERT, r, 0); }
void tb_FillRect(gaddr r, gaddr pat) { verb_rect(V_FILL, r, pat); }

static void verb_oval(int verb, gaddr r, gaddr pat)
{
    qrect q = qrect_get(r);
    hrgn shape, frame, inner;
    hrgn_init(&shape);
    hrgn_init(&frame);
    hrgn_init(&inner);
    qd_oval_region(&shape, q);
    if (verb == V_FRAME) {
        int pw, ph;
        pen_size(qd_port(), &pw, &ph);
        qd_oval_region(&inner, (qrect){ (int16_t)(q.top + ph), (int16_t)(q.left + pw), (int16_t)(q.bottom - ph),
                                        (int16_t)(q.right - pw) });
        hrgn_op(&frame, &shape, &inner, RGN_DIFF);
    }
    do_verb(verb, &shape, &frame, pat);
    hrgn_free(&shape);
    hrgn_free(&frame);
    hrgn_free(&inner);
}

void tb_FrameOval(gaddr r) { verb_oval(V_FRAME, r, 0); }
void tb_PaintOval(gaddr r) { verb_oval(V_PAINT, r, 0); }
void tb_EraseOval(gaddr r) { verb_oval(V_ERASE, r, 0); }
void tb_InvertOval(gaddr r) { verb_oval(V_INVERT, r, 0); }
void tb_FillOval(gaddr r, gaddr pat) { verb_oval(V_FILL, r, pat); }

static void verb_rrect(int verb, gaddr r, int ow, int oh, gaddr pat)
{
    qrect q = qrect_get(r);
    hrgn shape, frame, inner;
    hrgn_init(&shape);
    hrgn_init(&frame);
    hrgn_init(&inner);
    qd_rrect_region(&shape, q, ow, oh);
    if (verb == V_FRAME) {
        int pw, ph;
        pen_size(qd_port(), &pw, &ph);
        qd_rrect_region(&inner, (qrect){ (int16_t)(q.top + ph), (int16_t)(q.left + pw), (int16_t)(q.bottom - ph),
                                         (int16_t)(q.right - pw) },
                        ow - 2 * pw, oh - 2 * ph);
        hrgn_op(&frame, &shape, &inner, RGN_DIFF);
    }
    do_verb(verb, &shape, &frame, pat);
    hrgn_free(&shape);
    hrgn_free(&frame);
    hrgn_free(&inner);
}

void tb_FrameRoundRect(gaddr r, int16_t ow, int16_t oh) { verb_rrect(V_FRAME, r, ow, oh, 0); }
void tb_PaintRoundRect(gaddr r, int16_t ow, int16_t oh) { verb_rrect(V_PAINT, r, ow, oh, 0); }
void tb_EraseRoundRect(gaddr r, int16_t ow, int16_t oh) { verb_rrect(V_ERASE, r, ow, oh, 0); }
void tb_InvertRoundRect(gaddr r, int16_t ow, int16_t oh) { verb_rrect(V_INVERT, r, ow, oh, 0); }
void tb_FillRoundRect(gaddr r, int16_t ow, int16_t oh, gaddr pat) { verb_rrect(V_FILL, r, ow, oh, pat); }

static void verb_arc(int verb, gaddr r, int start, int angle, gaddr pat)
{
    qrect q = qrect_get(r);
    hrgn shape, frame, inner;
    hrgn_init(&shape);
    hrgn_init(&frame);
    hrgn_init(&inner);
    qd_oval_region(&shape, q);
    if (verb == V_FRAME) {
        int pw, ph;
        pen_size(qd_port(), &pw, &ph);
        qd_oval_region(&inner, (qrect){ (int16_t)(q.top + ph), (int16_t)(q.left + pw), (int16_t)(q.bottom - ph),
                                        (int16_t)(q.right - pw) });
        hrgn_op(&frame, &shape, &inner, RGN_DIFF);
        arc_mask(&frame, q, start, angle);
    }
    arc_mask(&shape, q, start, angle);
    do_verb(verb, &shape, &frame, pat);
    hrgn_free(&shape);
    hrgn_free(&frame);
    hrgn_free(&inner);
}

void tb_FrameArc(gaddr r, int16_t start, int16_t angle) { verb_arc(V_FRAME, r, start, angle, 0); }
void tb_PaintArc(gaddr r, int16_t start, int16_t angle) { verb_arc(V_PAINT, r, start, angle, 0); }
void tb_EraseArc(gaddr r, int16_t start, int16_t angle) { verb_arc(V_ERASE, r, start, angle, 0); }
void tb_InvertArc(gaddr r, int16_t start, int16_t angle) { verb_arc(V_INVERT, r, start, angle, 0); }
void tb_FillArc(gaddr r, int16_t start, int16_t angle, gaddr pat) { verb_arc(V_FILL, r, start, angle, pat); }

static void verb_rgn(int verb, gaddr rh, gaddr pat)
{
    hrgn shape, frame, inner;
    hrgn_init(&shape);
    hrgn_init(&frame);
    hrgn_init(&inner);
    rgn_read(rh, &shape);
    if (verb == V_FRAME) {
        int pw, ph;
        pen_size(qd_port(), &pw, &ph);
        hrgn_copy(&inner, &shape);
        hrgn_inset(&inner, pw, ph);
        hrgn_op(&frame, &shape, &inner, RGN_DIFF);
    }
    do_verb(verb, &shape, &frame, pat);
    hrgn_free(&shape);
    hrgn_free(&frame);
    hrgn_free(&inner);
}

void tb_FrameRgn(gaddr rh) { verb_rgn(V_FRAME, rh, 0); }
void tb_PaintRgn(gaddr rh) { verb_rgn(V_PAINT, rh, 0); }
void tb_EraseRgn(gaddr rh) { verb_rgn(V_ERASE, rh, 0); }
void tb_InvertRgn(gaddr rh) { verb_rgn(V_INVERT, rh, 0); }
void tb_FillRgn(gaddr rh, gaddr pat) { verb_rgn(V_FILL, rh, pat); }

/* ---------------------------------------------------------------------- */
/* Lines */

static void line_to(int h, int v)
{
    gaddr port = qd_port();
    uint32_t loc = rd32(port + GrafPort_pnLoc_off);
    if (GrafPort_pnVis(port) >= 0) {
        int pw, ph;
        pen_size(port, &pw, &ph);
        hrgn shape;
        hrgn_init(&shape);
        qd_line_region(&shape, pt_h(loc), pt_v(loc), h, v, pw, ph);
        uint8_t p[8];
        qd_port_pattern(port, 0, p);
        qd_fill_region(port, &shape, p, GrafPort_pnMode(port));
        hrgn_free(&shape);
    }
    wr32(port + GrafPort_pnLoc_off, make_pt((int16_t)v, (int16_t)h));
}

void tb_LineTo(int16_t h, int16_t v) { line_to(h, v); }

void tb_Line(int16_t dh, int16_t dv)
{
    uint32_t loc = rd32(qd_port() + GrafPort_pnLoc_off);
    line_to(pt_h(loc) + dh, pt_v(loc) + dv);
}

int8_t tb_GetPixel(int16_t h, int16_t v)
{
    pixdst d = qd_port_pixels(qd_port());
    if (h < d.bounds.left || h >= d.bounds.right || v < d.bounds.top || v >= d.bounds.bottom)
        return 0;
    return (int8_t)get_px(&d, h - d.bounds.left, v - d.bounds.top);
}

/* ---------------------------------------------------------------------- */
/* CopyBits */

/* A BitMap or PixMap given by address. */
static pixdst bits_at(gaddr bm)
{
    pixdst d;
    uint16_t rb = (uint16_t)rd16(bm + 4);
    d.base = rd32(bm);
    d.row_bytes = rb & 0x3FFF;
    d.bounds = qrect_get(bm + 6);
    d.depth = (rb & 0x8000) ? PixMap_pixelSize(bm) : 1;
    if (d.depth < 1)
        d.depth = 1;
    return d;
}

/* Address of the current port's bitmap/pixmap structure. */
static gaddr port_bits_addr(gaddr port)
{
    if (qd_port_is_color(port))
        return rd32(rd32(port + CGrafPort_portPixMap_off));
    return port + GrafPort_portBits_off;
}

static void copy_bits(gaddr src_bm, gaddr dst_bm, qrect sr, qrect dr, int mode, const hrgn *mask)
{
    /* Callers pass a PixMap via its handle's master pointer or directly; a
     * CGrafPort's portBits field holds the pixmap handle. */
    pixdst s = bits_at(src_bm), d = bits_at(dst_bm);
    gaddr port = qd_port();
    int fg = 1, bk = 0;
    if (port)
        qd_port_bits(port, &fg, &bk);
    hrgn clip, tmp;
    hrgn_init(&clip);
    hrgn_init(&tmp);
    hrgn_rect(&clip, dr);
    hrgn_rect(&tmp, d.bounds);
    hrgn_op(&clip, &clip, &tmp, RGN_SECT);
    if (port && port_bits_addr(port) == dst_bm) {
        qd_port_clip(port, &tmp);
        hrgn_op(&clip, &clip, &tmp, RGN_SECT);
    }
    if (mask)
        hrgn_op(&clip, &clip, mask, RGN_SECT);
    int sw = sr.right - sr.left, sh = sr.bottom - sr.top;
    int dw = dr.right - dr.left, dh = dr.bottom - dr.top;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) {
        hrgn_free(&clip);
        hrgn_free(&tmp);
        return;
    }
    int m = mode & 7;
    if (mode >= 32) /* arithmetic and dither modes: plain copy on 1-bit */
        m = srcCopy;
    /* Read source rows first when source and destination overlap. */
    bool same = s.base == d.base;
    uint8_t *snap = NULL;
    if (same) {
        snap = malloc((size_t)sw * (size_t)sh);
        for (int y = 0; y < sh; y++)
            for (int x = 0; x < sw; x++) {
                int sx = sr.left + x - s.bounds.left, sy = sr.top + y - s.bounds.top;
                bool ok = sx >= 0 && sy >= 0 && sx < s.bounds.right - s.bounds.left && sy < s.bounds.bottom - s.bounds.top;
                snap[(size_t)y * (size_t)sw + (size_t)x] = ok ? (uint8_t)get_px(&s, sx, sy) : 0;
            }
    }
    for (int i = 0; i < clip.nbands; i++) {
        const rband *b = &clip.bands[i];
        for (int y = b->top; y < b->bottom; y++) {
            int syo = (int)((int64_t)(y - dr.top) * sh / dh);
            for (int k = 0; k < b->n; k += 2) {
                for (int x = b->x[k]; x < b->x[k + 1]; x++) {
                    int sxo = (int)((int64_t)(x - dr.left) * sw / dw);
                    int bit;
                    if (snap) {
                        bit = snap[(size_t)syo * (size_t)sw + (size_t)sxo];
                    } else {
                        int sx = sr.left + sxo - s.bounds.left, sy = sr.top + syo - s.bounds.top;
                        if (sx < 0 || sy < 0 || sx >= s.bounds.right - s.bounds.left || sy >= s.bounds.bottom - s.bounds.top)
                            continue;
                        bit = get_px(&s, sx, sy);
                    }
                    int col = x - d.bounds.left, row = y - d.bounds.top;
                    put_px(&d, col, row, apply_mode(m, bit, get_px(&d, col, row), fg, bk));
                }
            }
        }
    }
    free(snap);
    hrgn_free(&clip);
    hrgn_free(&tmp);
}

/* A CGrafPort's "portBits" is a PixMap handle followed by 0xC000; CopyBits
 * receives &port->portBits.  Resolve that to the PixMap itself. */
static gaddr resolve_bits(gaddr bm)
{
    if ((rd16(bm + 4) & 0xC000) == 0xC000)
        return rd32(rd32(bm));
    return bm;
}

void tb_CopyBits(gaddr src_bitmap, gaddr dst_bitmap, gaddr src_rect, gaddr dst_rect, int16_t mode, gaddr mask)
{
    hrgn m;
    hrgn_init(&m);
    if (mask)
        rgn_read(mask, &m);
    copy_bits(resolve_bits(src_bitmap), resolve_bits(dst_bitmap), qrect_get(src_rect), qrect_get(dst_rect), mode,
              mask ? &m : NULL);
    hrgn_free(&m);
}

void tb_CopyMask(gaddr srcbp, gaddr mskbp, gaddr dstbp, gaddr srcrp, gaddr mskrp, gaddr dstrp)
{
    /* Mask bitmap -> region, then a masked copy. */
    pixdst mk = bits_at(resolve_bits(mskbp));
    qrect mr = qrect_get(mskrp), dr = qrect_get(dstrp);
    hrgn mask;
    hrgn_init(&mask);
    for (int y = mr.top; y < mr.bottom; y++) {
        int run = -1;
        for (int x = mr.left; x <= mr.right; x++) {
            bool on = x < mr.right && get_px(&mk, x - mk.bounds.left, y - mk.bounds.top);
            if (on && run < 0)
                run = x;
            if (!on && run >= 0) {
                hrgn t;
                hrgn_init(&t);
                hrgn_rect(&t, (qrect){ (int16_t)(y - mr.top + dr.top), (int16_t)(run - mr.left + dr.left),
                                       (int16_t)(y - mr.top + dr.top + 1), (int16_t)(x - mr.left + dr.left) });
                hrgn_op(&mask, &mask, &t, RGN_UNION);
                hrgn_free(&t);
                run = -1;
            }
        }
    }
    copy_bits(resolve_bits(srcbp), resolve_bits(dstbp), qrect_get(srcrp), dr, srcCopy, &mask);
    hrgn_free(&mask);
}

void tb_ScrollRect(gaddr rp, int16_t dh, int16_t dv, gaddr updatergn)
{
    gaddr port = qd_port();
    qrect r = qrect_get(rp);
    hrgn vis, moved, update;
    hrgn_init(&vis);
    hrgn_init(&moved);
    hrgn_init(&update);
    qd_port_clip(port, &vis);
    hrgn_rect(&moved, r);
    hrgn_op(&vis, &vis, &moved, RGN_SECT);
    hrgn_copy(&moved, &vis);
    hrgn_offset(&moved, dh, dv);
    hrgn_op(&moved, &moved, &vis, RGN_SECT);
    /* Copy the pixels that stay visible. */
    pixdst d = qd_port_pixels(port);
    qrect bb = hrgn_bbox(&moved);
    int w = bb.right - bb.left, h = bb.bottom - bb.top;
    if (w > 0 && h > 0) {
        uint8_t *buf = malloc((size_t)w * (size_t)h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                buf[(size_t)y * (size_t)w + (size_t)x] = (uint8_t)get_px(
                    &d, bb.left + x - dh - d.bounds.left, bb.top + y - dv - d.bounds.top);
        for (int i = 0; i < moved.nbands; i++) {
            const rband *b = &moved.bands[i];
            for (int y = b->top; y < b->bottom; y++)
                for (int k = 0; k < b->n; k += 2)
                    for (int x = b->x[k]; x < b->x[k + 1]; x++)
                        put_px(&d, x - d.bounds.left, y - d.bounds.top,
                               buf[(size_t)(y - bb.top) * (size_t)w + (size_t)(x - bb.left)]);
        }
        free(buf);
    }
    /* What was uncovered is erased and reported for updating. */
    hrgn_op(&update, &vis, &moved, RGN_DIFF);
    uint8_t p[8];
    qd_port_pattern(port, 1, p);
    qd_fill_region(port, &update, p, patCopy);
    if (updatergn)
        rgn_write(updatergn, &update);
    hrgn_free(&vis);
    hrgn_free(&moved);
    hrgn_free(&update);
}

void tb_SeedFill(gaddr srcp, gaddr dstp, int16_t srcr, int16_t dstr, int16_t height, int16_t width, int16_t seedh,
                 int16_t seedv)
{
    /* Flood fill of white pixels from the seed; destination gets the filled area. */
    int w = width * 16, h = height;
    uint8_t *vis = calloc((size_t)w * (size_t)h, 1);
    int *stack = malloc((size_t)w * (size_t)h * 2 * sizeof *stack);
    int sp = 0;
    stack[sp++] = seedh;
    stack[sp++] = seedv;
    while (sp) {
        int y = stack[--sp], x = stack[--sp];
        if (x < 0 || y < 0 || x >= w || y >= h || vis[y * w + x])
            continue;
        if ((rd8(srcp + (uint32_t)(y * srcr + (x >> 3))) >> (7 - (x & 7))) & 1)
            continue;
        vis[y * w + x] = 1;
        stack[sp++] = x + 1; stack[sp++] = y;
        stack[sp++] = x - 1; stack[sp++] = y;
        stack[sp++] = x; stack[sp++] = y + 1;
        stack[sp++] = x; stack[sp++] = y - 1;
    }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            gaddr a = dstp + (uint32_t)(y * dstr + (x >> 3));
            uint8_t m = (uint8_t)(0x80 >> (x & 7));
            wr8(a, vis[y * w + x] ? (uint8_t)(rd8(a) | m) : (uint8_t)(rd8(a) & ~m));
        }
    free(stack);
    free(vis);
}

void tb_CalcMask(gaddr srcp, gaddr dstp, int16_t srcr, int16_t dstr, int16_t height, int16_t width)
{
    /* Mask = everything not reachable from the edges through white pixels. */
    int w = width * 16, h = height;
    uint8_t *outside = calloc((size_t)w * (size_t)h, 1);
    int *stack = malloc((size_t)(w * h * 2 + 4 * (w + h)) * sizeof *stack);
    int sp = 0;
    for (int x = 0; x < w; x++) {
        stack[sp++] = x; stack[sp++] = 0;
        stack[sp++] = x; stack[sp++] = h - 1;
    }
    for (int y = 0; y < h; y++) {
        stack[sp++] = 0; stack[sp++] = y;
        stack[sp++] = w - 1; stack[sp++] = y;
    }
    while (sp) {
        int y = stack[--sp], x = stack[--sp];
        if (x < 0 || y < 0 || x >= w || y >= h || outside[y * w + x])
            continue;
        if ((rd8(srcp + (uint32_t)(y * srcr + (x >> 3))) >> (7 - (x & 7))) & 1)
            continue;
        outside[y * w + x] = 1;
        stack[sp++] = x + 1; stack[sp++] = y;
        stack[sp++] = x - 1; stack[sp++] = y;
        stack[sp++] = x; stack[sp++] = y + 1;
        stack[sp++] = x; stack[sp++] = y - 1;
    }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            gaddr a = dstp + (uint32_t)(y * dstr + (x >> 3));
            uint8_t m = (uint8_t)(0x80 >> (x & 7));
            wr8(a, outside[y * w + x] ? (uint8_t)(rd8(a) & ~m) : (uint8_t)(rd8(a) | m));
        }
    free(stack);
    free(outside);
}
