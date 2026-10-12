/*
 * QuickDraw drawing: every shape becomes a region, is clipped to the port,
 * and is filled with a pattern and transfer mode.  Pixels are 1-bit (and
 * 8-bit for offscreen pixmaps); colour goes through the port's foreground
 * and background.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "m68k.h"
#include "machine.h"
#include "qd_internal.h"
#include "tb_internal.h"
#include "trap.h"

enum { srcCopy = 0, srcOr, srcXor, srcBic, notSrcCopy, notSrcOr, notSrcXor, notSrcBic };
enum { patCopy = 8, patOr, patXor, patBic, notPatCopy, notPatOr, notPatXor, notPatBic };

bool qd_region_recording(void);
void qd_region_record(const hrgn *shape);

/* Fill one row span [c0, c1) of bitmap row `row`. */
static void fill_span_1bpp(const pixdst *d, int row, int c0, int c1, uint8_t pat, int mode, uint32_t fg, uint32_t bk)
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
    uint32_t fg, bk, inv = px_mask(d.depth);
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
                        put_px(&d, c, row, apply_mode(mode, s, get_px(&d, c, row), fg, bk, inv));
                    }
                }
            }
        }
    }
    hrgn_free(&r);
}

/* Highlighting (HiliteMode's bit cleared before an invert, or the pen's
 * hilite mode): the background colour and the highlight colour (HiliteRGB)
 * change places; other colours stay.  On one bit it is an ordinary invert. */
enum { hilite = 50 };

static bool hilite_requested(gaddr port)
{
    return !(rd8(LM_HiliteMode) & 0x80) || (GrafPort_pnMode(port) & 0x3F) == hilite;
}

static void hilite_region(gaddr port, const hrgn *shape)
{
    wr8(LM_HiliteMode, (uint8_t)(rd8(LM_HiliteMode) | 0x80)); /* for one operation only */
    pixdst d = qd_port_pixels(port);
    if (d.depth == 1) {
        qd_fill_region(port, shape, qd_pat_black, patXor);
        return;
    }
    hrgn r;
    hrgn_init(&r);
    qd_port_clip(port, &r);
    hrgn_op(&r, shape, &r, RGN_SECT);
    uint32_t fg, bk;
    qd_port_bits(port, &fg, &bk);
    uint16_t rgb[3] = { rd16(LM_HiliteRGB), rd16(LM_HiliteRGB + 2), rd16(LM_HiliteRGB + 4) };
    uint32_t hi = qd_rgb_px(d.depth, d.ctab, rgb);
    for (int i = 0; i < r.nbands; i++) {
        const rband *b = &r.bands[i];
        for (int y = b->top; y < b->bottom; y++)
            for (int k = 0; k < b->n; k += 2)
                for (int x = b->x[k]; x < b->x[k + 1]; x++) {
                    int c = x - d.bounds.left, row = y - d.bounds.top;
                    uint32_t v = get_px(&d, c, row);
                    if (v == bk)
                        put_px(&d, c, row, hi);
                    else if (v == hi)
                        put_px(&d, c, row, bk);
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

/* The shape being drawn, for recording ports (printing). */
static struct {
    int kind;
    qrect r;
    int a, b;
} geom;

static void set_geom(int kind, qrect r, int a, int b)
{
    geom.kind = kind;
    geom.r = r;
    geom.a = a;
    geom.b = b;
}

static void record_verb(gaddr port, int verb, const hrgn *shape, const hrgn *frame, gaddr pat)
{
    uint8_t p[8];
    int mode;
    switch (verb) {
    case V_FRAME:
    case V_PAINT:
        if (verb == V_FRAME && GrafPort_pnVis(port) < 0)
            return;
        qd_port_pattern(port, 0, p);
        mode = GrafPort_pnMode(port);
        break;
    case V_ERASE:
        qd_port_pattern(port, 1, p);
        mode = patCopy;
        break;
    case V_INVERT:
        memcpy(p, qd_pat_black, 8);
        mode = patXor;
        break;
    default:
        gget(p, pat, 8);
        mode = patCopy;
        break;
    }
    qd_rec_shape(port, geom.kind, verb, geom.r, geom.a, geom.b, verb == V_FRAME ? frame : shape, p, mode);
}

/* Perform a verb on a filled shape; `frame` is the shape's outline region. */
static void do_verb(int verb, const hrgn *shape, const hrgn *frame, gaddr pat)
{
    gaddr port = qd_port();
    if (!port)
        return;
    if (qd_rec_active(port))
        record_verb(port, verb, shape, frame, pat);
    uint8_t p[8];
    switch (verb) {
    case V_FRAME:
        if (GrafPort_pnVis(port) < 0)
            return;
        qd_port_pattern(port, 0, p);
        qd_fill_region(port, frame, p, GrafPort_pnMode(port));
        break;
    case V_PAINT:
        if ((GrafPort_pnMode(port) & 0x3F) == hilite) {
            hilite_region(port, shape);
            break;
        }
        qd_port_pattern(port, 0, p);
        qd_fill_region(port, shape, p, GrafPort_pnMode(port));
        break;
    case V_ERASE:
        qd_port_pattern(port, 1, p);
        qd_fill_region(port, shape, p, patCopy);
        break;
    case V_INVERT:
        if (hilite_requested(port))
            hilite_region(port, shape);
        else
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

/* ---------------------------------------------------------------------- */
/* Bottlenecks: an application that puts its own procedures in the port's
 * grafProcs gets every drawing call; its procedures usually draw through
 * the Std routines.  Our own routines in the table are called directly. */

static gaddr bottleneck(uint32_t off)
{
    gaddr port = qd_port();
    gaddr procs = port ? GrafPort_grafProcs(port) : 0;
    gaddr p = procs ? rd32(procs + off) : 0;
    return p && native_index_for(p) < 0 ? p : 0;
}

/* The pattern a Std routine fills with: the port's fill pattern. */
static gaddr port_fill(int verb)
{
    gaddr port = qd_port();
    if (verb != V_FILL || !port)
        return 0;
    if (qd_port_is_color(port)) {
        gaddr pp = rd32(port + CGrafPort_fillPixPat_off);
        if (pp && rd32(pp))
            return rd32(pp) + PixPat_pat1Data_off;
    }
    return port + GrafPort_fillPat_off;
}

/* Before a Fill call goes to a bottleneck, the pattern goes in the port. */
static void set_fill(int verb, gaddr pat)
{
    gaddr dst = port_fill(verb);
    if (dst && pat)
        gmemcpy(dst, pat, 8);
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
    set_geom(MACHINE_PRINT_RECT, q, 0, 0);
    do_verb(verb, &shape, &frame, pat);
    hrgn_free(&shape);
    hrgn_free(&frame);
    hrgn_free(&inner);
}

static void rect_entry(int verb, gaddr r, gaddr pat)
{
    gaddr proc = bottleneck(QDProcs_rectProc_off);
    if (!proc) {
        verb_rect(verb, r, pat);
        return;
    }
    set_fill(verb, pat);
    cpu_push16(g_cpu, (uint16_t)(verb << 8));
    cpu_push32(g_cpu, r);
    guest_call(proc);
}

void tb_FrameRect(gaddr r) { rect_entry(V_FRAME, r, 0); }
void tb_PaintRect(gaddr r) { rect_entry(V_PAINT, r, 0); }
void tb_EraseRect(gaddr r) { rect_entry(V_ERASE, r, 0); }
void tb_InvertRect(gaddr r) { rect_entry(V_INVERT, r, 0); }
void tb_FillRect(gaddr r, gaddr pat) { rect_entry(V_FILL, r, pat); }
void tb_StdRect(int8_t v, gaddr r) { verb_rect(v, r, port_fill(v)); }

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
    set_geom(MACHINE_PRINT_OVAL, q, 0, 0);
    do_verb(verb, &shape, &frame, pat);
    hrgn_free(&shape);
    hrgn_free(&frame);
    hrgn_free(&inner);
}

static void oval_entry(int verb, gaddr r, gaddr pat)
{
    gaddr proc = bottleneck(QDProcs_ovalProc_off);
    if (!proc) {
        verb_oval(verb, r, pat);
        return;
    }
    set_fill(verb, pat);
    cpu_push16(g_cpu, (uint16_t)(verb << 8));
    cpu_push32(g_cpu, r);
    guest_call(proc);
}

void tb_FrameOval(gaddr r) { oval_entry(V_FRAME, r, 0); }
void tb_PaintOval(gaddr r) { oval_entry(V_PAINT, r, 0); }
void tb_EraseOval(gaddr r) { oval_entry(V_ERASE, r, 0); }
void tb_InvertOval(gaddr r) { oval_entry(V_INVERT, r, 0); }
void tb_FillOval(gaddr r, gaddr pat) { oval_entry(V_FILL, r, pat); }
void tb_StdOval(int8_t v, gaddr r) { verb_oval(v, r, port_fill(v)); }

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
    set_geom(MACHINE_PRINT_RRECT, q, ow, oh);
    do_verb(verb, &shape, &frame, pat);
    hrgn_free(&shape);
    hrgn_free(&frame);
    hrgn_free(&inner);
}

static void rrect_entry(int verb, gaddr r, int ow, int oh, gaddr pat)
{
    gaddr proc = bottleneck(QDProcs_rRectProc_off);
    if (!proc) {
        verb_rrect(verb, r, ow, oh, pat);
        return;
    }
    set_fill(verb, pat);
    cpu_push16(g_cpu, (uint16_t)(verb << 8));
    cpu_push32(g_cpu, r);
    cpu_push16(g_cpu, (uint16_t)ow);
    cpu_push16(g_cpu, (uint16_t)oh);
    guest_call(proc);
}

void tb_FrameRoundRect(gaddr r, int16_t ow, int16_t oh) { rrect_entry(V_FRAME, r, ow, oh, 0); }
void tb_PaintRoundRect(gaddr r, int16_t ow, int16_t oh) { rrect_entry(V_PAINT, r, ow, oh, 0); }
void tb_EraseRoundRect(gaddr r, int16_t ow, int16_t oh) { rrect_entry(V_ERASE, r, ow, oh, 0); }
void tb_InvertRoundRect(gaddr r, int16_t ow, int16_t oh) { rrect_entry(V_INVERT, r, ow, oh, 0); }
void tb_FillRoundRect(gaddr r, int16_t ow, int16_t oh, gaddr pat) { rrect_entry(V_FILL, r, ow, oh, pat); }
void tb_StdRRect(int8_t v, gaddr r, int16_t ow, int16_t oh) { verb_rrect(v, r, ow, oh, port_fill(v)); }

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
    set_geom(MACHINE_PRINT_ARC, q, start, angle);
    do_verb(verb, &shape, &frame, pat);
    hrgn_free(&shape);
    hrgn_free(&frame);
    hrgn_free(&inner);
}

static void arc_entry(int verb, gaddr r, int start, int angle, gaddr pat)
{
    gaddr proc = bottleneck(QDProcs_arcProc_off);
    if (!proc) {
        verb_arc(verb, r, start, angle, pat);
        return;
    }
    set_fill(verb, pat);
    cpu_push16(g_cpu, (uint16_t)(verb << 8));
    cpu_push32(g_cpu, r);
    cpu_push16(g_cpu, (uint16_t)start);
    cpu_push16(g_cpu, (uint16_t)angle);
    guest_call(proc);
}

void tb_FrameArc(gaddr r, int16_t start, int16_t angle) { arc_entry(V_FRAME, r, start, angle, 0); }
void tb_PaintArc(gaddr r, int16_t start, int16_t angle) { arc_entry(V_PAINT, r, start, angle, 0); }
void tb_EraseArc(gaddr r, int16_t start, int16_t angle) { arc_entry(V_ERASE, r, start, angle, 0); }
void tb_InvertArc(gaddr r, int16_t start, int16_t angle) { arc_entry(V_INVERT, r, start, angle, 0); }
void tb_FillArc(gaddr r, int16_t start, int16_t angle, gaddr pat) { arc_entry(V_FILL, r, start, angle, pat); }
void tb_StdArc(int8_t v, gaddr r, int16_t start, int16_t angle) { verb_arc(v, r, start, angle, port_fill(v)); }

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
    set_geom(MACHINE_PRINT_REGION, hrgn_bbox(&shape), 0, 0);
    do_verb(verb, &shape, &frame, pat);
    hrgn_free(&shape);
    hrgn_free(&frame);
    hrgn_free(&inner);
}

static void rgn_entry(int verb, gaddr rh, gaddr pat)
{
    gaddr proc = bottleneck(QDProcs_rgnProc_off);
    if (!proc) {
        verb_rgn(verb, rh, pat);
        return;
    }
    set_fill(verb, pat);
    cpu_push16(g_cpu, (uint16_t)(verb << 8));
    cpu_push32(g_cpu, rh);
    guest_call(proc);
}

void tb_FrameRgn(gaddr rh) { rgn_entry(V_FRAME, rh, 0); }
void tb_PaintRgn(gaddr rh) { rgn_entry(V_PAINT, rh, 0); }
void tb_EraseRgn(gaddr rh) { rgn_entry(V_ERASE, rh, 0); }
void tb_InvertRgn(gaddr rh) { rgn_entry(V_INVERT, rh, 0); }
void tb_FillRgn(gaddr rh, gaddr pat) { rgn_entry(V_FILL, rh, pat); }
void tb_StdRgn(int8_t v, gaddr rh) { verb_rgn(v, rh, port_fill(v)); }

/* ---------------------------------------------------------------------- */
/* Lines */

static void poly_add(gaddr poly, int h, int v);

static void line_to(int h, int v)
{
    gaddr port = qd_port();
    uint32_t loc = rd32(port + GrafPort_pnLoc_off);
    gaddr poly = GrafPort_polySave(port);
    if (poly) {
        if (rd16(rd32(poly)) == 10)
            poly_add(poly, pt_h(loc), pt_v(loc));
        poly_add(poly, h, v);
    }
    if (GrafPort_pnVis(port) >= 0) {
        int pw, ph;
        pen_size(port, &pw, &ph);
        hrgn shape;
        hrgn_init(&shape);
        qd_line_region(&shape, pt_h(loc), pt_v(loc), h, v, pw, ph);
        uint8_t p[8];
        qd_port_pattern(port, 0, p);
        if (qd_rec_active(port))
            qd_rec_line(port, pt_h(loc), pt_v(loc), h, v, p, GrafPort_pnMode(port));
        qd_fill_region(port, &shape, p, GrafPort_pnMode(port));
        hrgn_free(&shape);
    }
    wr32(port + GrafPort_pnLoc_off, make_pt((int16_t)v, (int16_t)h));
}

static void line_entry(int h, int v)
{
    gaddr proc = bottleneck(QDProcs_lineProc_off);
    if (!proc) {
        line_to(h, v);
        return;
    }
    cpu_push32(g_cpu, make_pt((int16_t)v, (int16_t)h));
    guest_call(proc);
}

void tb_LineTo(int16_t h, int16_t v) { line_entry(h, v); }
void tb_StdLine(uint32_t p) { line_to(pt_h(p), pt_v(p)); }

void tb_Line(int16_t dh, int16_t dv)
{
    uint32_t loc = rd32(qd_port() + GrafPort_pnLoc_off);
    line_entry(pt_h(loc) + dh, pt_v(loc) + dv);
}

/* ---------------------------------------------------------------------- */
/* Polygons: polySize (2), polyBBox (8), then the points (v, h).  While a
 * polygon is open (polySave), lines add points and the pen is hidden. */

static void poly_add(gaddr poly, int h, int v)
{
    int32_t size = (int16_t)rd16(rd32(poly));
    tb_SetHandleSize(poly, size + 4);
    gaddr p = rd32(poly);
    wr32(p + (uint32_t)size, make_pt((int16_t)v, (int16_t)h));
    wr16(p, (uint16_t)(size + 4));
}

static void poly_bbox(gaddr poly)
{
    gaddr p = rd32(poly);
    int n = ((int16_t)rd16(p) - 10) / 4;
    qrect b = { 0, 0, 0, 0 };
    for (int i = 0; i < n; i++) {
        uint32_t pt = rd32(p + 10 + (uint32_t)i * 4);
        int v = pt_v(pt), h = pt_h(pt);
        if (i == 0 || v < b.top)
            b.top = (int16_t)v;
        if (i == 0 || v > b.bottom)
            b.bottom = (int16_t)v;
        if (i == 0 || h < b.left)
            b.left = (int16_t)h;
        if (i == 0 || h > b.right)
            b.right = (int16_t)h;
    }
    qrect_put(p + 2, b);
}

gaddr tb_OpenPoly(void)
{
    gaddr port = qd_port();
    gaddr poly = tb_NewHandle(10, false, true);
    if (!poly)
        return 0;
    wr16(rd32(poly), 10);
    GrafPort_set_polySave(port, poly);
    tb_HidePen();
    return poly;
}

void tb_ClosePoly(void)
{
    gaddr port = qd_port();
    gaddr poly = GrafPort_polySave(port);
    if (!poly)
        return;
    poly_bbox(poly);
    GrafPort_set_polySave(port, 0);
    tb_ShowPen();
}

void tb_KillPoly(gaddr poly)
{
    if (poly)
        tb_DisposeHandle(poly);
}

void tb_OffsetPoly(gaddr poly, int16_t dh, int16_t dv)
{
    if (!poly || !rd32(poly))
        return;
    gaddr p = rd32(poly);
    int n = ((int16_t)rd16(p) - 10) / 4;
    for (int i = -1; i < n; i++) {
        /* i == -1: the bounding box's top-left; its bottom-right follows. */
        gaddr a = i < 0 ? p + 2 : p + 10 + (uint32_t)i * 4;
        for (int k = 0; k < (i < 0 ? 2 : 1); k++, a += 4) {
            uint32_t pt = rd32(a);
            wr32(a, make_pt((int16_t)(pt_v(pt) + dv), (int16_t)(pt_h(pt) + dh)));
        }
    }
}

/* The polygon's interior, even-odd, sampled at each row's centre. */
static void poly_region(gaddr poly, hrgn *out)
{
    gaddr p = rd32(poly);
    int n = ((int16_t)rd16(p) - 10) / 4;
    hrgn_rect(out, (qrect){ 0, 0, 0, 0 });
    if (n < 3)
        return;
    int *vs = malloc((size_t)n * sizeof *vs), *hs = malloc((size_t)n * sizeof *hs);
    for (int i = 0; i < n; i++) {
        uint32_t pt = rd32(p + 10 + (uint32_t)i * 4);
        vs[i] = pt_v(pt);
        hs[i] = pt_h(pt);
    }
    qrect b = qrect_get(p + 2);
    double *xs = malloc((size_t)n * sizeof *xs);
    hrgn row;
    hrgn_init(&row);
    for (int y = b.top; y < b.bottom; y++) {
        double ys = y + 0.5;
        int k = 0;
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            double y0 = vs[i], y1 = vs[j];
            if ((y0 <= ys && ys < y1) || (y1 <= ys && ys < y0))
                xs[k++] = hs[i] + (ys - y0) * (hs[j] - hs[i]) / (y1 - y0);
        }
        for (int a = 1; a < k; a++) /* insertion sort: few crossings */
            for (int c = a; c > 0 && xs[c - 1] > xs[c]; c--) {
                double t = xs[c];
                xs[c] = xs[c - 1];
                xs[c - 1] = t;
            }
        int16_t runs[64];
        int nr = 0;
        for (int a = 0; a + 1 < k && nr + 2 <= 64; a += 2) {
            int x0 = (int)(xs[a] + 0.5), x1 = (int)(xs[a + 1] + 0.5);
            if (x1 <= x0)
                continue;
            if (nr && runs[nr - 1] >= x0) { /* touching: extend the last run */
                if (x1 > runs[nr - 1])
                    runs[nr - 1] = (int16_t)x1;
                continue;
            }
            runs[nr++] = (int16_t)x0;
            runs[nr++] = (int16_t)x1;
        }
        hrgn_add_runs(out, y, runs, nr);
    }
    hrgn_free(&row);
    free(xs);
    free(vs);
    free(hs);
}

static void verb_poly(int verb, gaddr poly, gaddr pat)
{
    if (!poly || !rd32(poly))
        return;
    gaddr port = qd_port();
    hrgn shape, frame, seg;
    hrgn_init(&shape);
    hrgn_init(&frame);
    hrgn_init(&seg);
    if (verb == V_FRAME) {
        /* The pen along each edge in turn; the polygon isn't closed for you. */
        gaddr p = rd32(poly);
        int n = ((int16_t)rd16(p) - 10) / 4, pw, ph;
        pen_size(port, &pw, &ph);
        for (int i = 0; i + 1 < n; i++) {
            uint32_t a = rd32(p + 10 + (uint32_t)i * 4), c = rd32(p + 14 + (uint32_t)i * 4);
            qd_line_region(&seg, pt_h(a), pt_v(a), pt_h(c), pt_v(c), pw, ph);
            hrgn_op(&frame, &frame, &seg, RGN_UNION);
        }
        if (n > 0) {
            uint32_t last = rd32(p + 10 + (uint32_t)(n - 1) * 4);
            wr32(port + GrafPort_pnLoc_off, last);
        }
    } else {
        poly_region(poly, &shape);
    }
    set_geom(MACHINE_PRINT_REGION, qrect_get(rd32(poly) + 2), 0, 0);
    do_verb(verb, &shape, &frame, pat);
    hrgn_free(&shape);
    hrgn_free(&frame);
    hrgn_free(&seg);
}

static void poly_entry(int verb, gaddr poly, gaddr pat)
{
    gaddr proc = bottleneck(QDProcs_polyProc_off);
    if (!proc) {
        verb_poly(verb, poly, pat);
        return;
    }
    set_fill(verb, pat);
    cpu_push16(g_cpu, (uint16_t)(verb << 8));
    cpu_push32(g_cpu, poly);
    guest_call(proc);
}

void tb_FramePoly(gaddr poly) { poly_entry(V_FRAME, poly, 0); }
void tb_PaintPoly(gaddr poly) { poly_entry(V_PAINT, poly, 0); }
void tb_ErasePoly(gaddr poly) { poly_entry(V_ERASE, poly, 0); }
void tb_InvertPoly(gaddr poly) { poly_entry(V_INVERT, poly, 0); }
void tb_FillPoly(gaddr poly, gaddr pat) { poly_entry(V_FILL, poly, pat); }
void tb_StdPoly(int8_t v, gaddr poly) { verb_poly(v, poly, port_fill(v)); }

int8_t tb_GetPixel(int16_t h, int16_t v)
{
    pixdst d = qd_port_pixels(qd_port());
    if (h < d.bounds.left || h >= d.bounds.right || v < d.bounds.top || v >= d.bounds.bottom)
        return 0;
    uint32_t v0 = get_px(&d, h - d.bounds.left, v - d.bounds.top);
    if (d.depth == 1)
        return (int8_t)v0;
    uint16_t rgb[3];
    qd_px_rgb(d.depth, d.ctab, v0, rgb);
    return (rgb[0] * 30u + rgb[1] * 59u + rgb[2] * 11u) / 100u < 0x8000u; /* dark */
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
    d.ctab = (rb & 0x8000) ? PixMap_pmTable(bm) : 0;
    if (!(rb & 0x8000) && d.base == GA_SCREEN) { /* screenBits on a deep screen */
        d.depth = qd_screen_depth();
        d.ctab = qd_screen_ctab();
    }
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

/* One source pixel onto a destination pixel.  A 1-bit source is drawn in
 * the port's colours; a deep one is converted through RGB. */
static uint32_t transfer(const pixdst *s, const pixdst *d, uint32_t sv, uint32_t dv, int m, uint32_t fg, uint32_t bk)
{
    uint32_t inv = px_mask(d->depth);
    if (s->depth == 1)
        return apply_mode(m, (int)sv, dv, fg, bk, inv);
    bool same = s->depth == d->depth && s->ctab == d->ctab;
    uint16_t rgb[3];
    qd_px_rgb(s->depth, s->ctab, sv, rgb);
    if (m & 4) {
        rgb[0] = (uint16_t)~rgb[0];
        rgb[1] = (uint16_t)~rgb[1];
        rgb[2] = (uint16_t)~rgb[2];
        same = false;
    }
    bool white = rgb[0] == 0xFFFF && rgb[1] == 0xFFFF && rgb[2] == 0xFFFF;
    uint32_t conv = same ? sv : qd_rgb_px(d->depth, d->ctab, rgb);
    switch (m & 3) {
    case 0: return conv;
    case 1: return white ? dv : conv;
    case 2: return white ? dv : dv ^ inv;
    default: return white ? dv : bk;
    }
}

static void copy_bits(gaddr src_bm, gaddr dst_bm, qrect sr, qrect dr, int mode, const hrgn *mask)
{
    /* Callers pass a PixMap via its handle's master pointer or directly; a
     * CGrafPort's portBits field holds the pixmap handle. */
    pixdst s = bits_at(src_bm), d = bits_at(dst_bm);
    gaddr port = qd_port();
    uint32_t fg = 1, bk = 0;
    if (port) {
        uint32_t pf, pb;
        qd_port_bits(port, &pf, &pb);
        /* The port's colours, at the destination's depth. */
        pixdst pd = qd_port_pixels(port);
        if (pd.depth == d.depth) {
            fg = pf;
            bk = pb;
        } else {
            uint16_t c[3];
            qd_px_rgb(pd.depth, pd.ctab, pf, c);
            fg = qd_rgb_px(d.depth, d.ctab, c);
            qd_px_rgb(pd.depth, pd.ctab, pb, c);
            bk = qd_rgb_px(d.depth, d.ctab, c);
        }
    } else if (d.depth != 1) {
        static const uint16_t black[3] = { 0, 0, 0 }, white[3] = { 0xFFFF, 0xFFFF, 0xFFFF };
        fg = qd_rgb_px(d.depth, d.ctab, black);
        bk = qd_rgb_px(d.depth, d.ctab, white);
    }
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
    if (mode >= 32) /* arithmetic and dither modes: a plain copy */
        m = srcCopy;
    /* Indexed colour sources: each source value converted once. */
    uint32_t *lut_conv = NULL;
    uint8_t *lut_white = NULL;
    if (s.depth > 1 && s.depth <= 8) {
        int nv = 1 << s.depth;
        lut_conv = malloc(sizeof *lut_conv * (size_t)nv);
        lut_white = malloc((size_t)nv);
        bool same_table = s.depth == d.depth && s.ctab == d.ctab && !(m & 4);
        for (int v = 0; v < nv; v++) {
            uint16_t rgb[3];
            qd_px_rgb(s.depth, s.ctab, (uint32_t)v, rgb);
            if (m & 4) {
                rgb[0] = (uint16_t)~rgb[0];
                rgb[1] = (uint16_t)~rgb[1];
                rgb[2] = (uint16_t)~rgb[2];
            }
            lut_white[v] = rgb[0] == 0xFFFF && rgb[1] == 0xFFFF && rgb[2] == 0xFFFF;
            lut_conv[v] = same_table ? (uint32_t)v : qd_rgb_px(d.depth, d.ctab, rgb);
        }
    }
    uint32_t inv = px_mask(d.depth);
    /* Read source rows first when source and destination overlap. */
    bool same = s.base == d.base;
    uint32_t *snap = NULL;
    if (same) {
        snap = malloc((size_t)sw * (size_t)sh * sizeof *snap);
        for (int y = 0; y < sh; y++)
            for (int x = 0; x < sw; x++) {
                int sx = sr.left + x - s.bounds.left, sy = sr.top + y - s.bounds.top;
                bool ok = sx >= 0 && sy >= 0 && sx < s.bounds.right - s.bounds.left && sy < s.bounds.bottom - s.bounds.top;
                snap[(size_t)y * (size_t)sw + (size_t)x] = ok ? get_px(&s, sx, sy) : 0;
            }
    }
    for (int i = 0; i < clip.nbands; i++) {
        const rband *b = &clip.bands[i];
        for (int y = b->top; y < b->bottom; y++) {
            int syo = (int)((int64_t)(y - dr.top) * sh / dh);
            for (int k = 0; k < b->n; k += 2) {
                for (int x = b->x[k]; x < b->x[k + 1]; x++) {
                    int sxo = (int)((int64_t)(x - dr.left) * sw / dw);
                    uint32_t sv;
                    if (snap) {
                        sv = snap[(size_t)syo * (size_t)sw + (size_t)sxo];
                    } else {
                        int sx = sr.left + sxo - s.bounds.left, sy = sr.top + syo - s.bounds.top;
                        if (sx < 0 || sy < 0 || sx >= s.bounds.right - s.bounds.left || sy >= s.bounds.bottom - s.bounds.top)
                            continue;
                        sv = get_px(&s, sx, sy);
                    }
                    int col = x - d.bounds.left, row = y - d.bounds.top;
                    if (lut_conv) {
                        uint32_t conv = lut_conv[sv], v;
                        bool white = lut_white[sv];
                        switch (m & 3) {
                        case 0: v = conv; break;
                        case 1: v = white ? get_px(&d, col, row) : conv; break;
                        case 2: v = white ? get_px(&d, col, row) : get_px(&d, col, row) ^ inv; break;
                        default: v = white ? get_px(&d, col, row) : bk; break;
                        }
                        put_px(&d, col, row, v);
                    } else {
                        put_px(&d, col, row, transfer(&s, &d, sv, get_px(&d, col, row), m, fg, bk));
                    }
                }
            }
        }
    }
    free(snap);
    free(lut_conv);
    free(lut_white);
    hrgn_free(&clip);
    hrgn_free(&tmp);
}

static gaddr resolve_bits(gaddr bm)
{
    if ((rd16(bm + 4) & 0xC000) == 0xC000)
        return rd32(rd32(bm));
    return bm;
}

/* Copying into a printing port: record the source pixels as an image. */
static void record_copy(gaddr src_bitmap, gaddr src_rect, gaddr dst_rect)
{
    pixdst s = bits_at(resolve_bits(src_bitmap));
    qrect sr = qrect_get(src_rect);
    int w = sr.right - sr.left, h = sr.bottom - sr.top;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
        return;
    uint8_t *rgba = malloc((size_t)w * (size_t)h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int sx = sr.left + x - s.bounds.left, sy = sr.top + y - s.bounds.top;
            bool in = sx >= 0 && sy >= 0 && sx < s.bounds.right - s.bounds.left && sy < s.bounds.bottom - s.bounds.top;
            uint16_t c[3] = { 0xFFFF, 0xFFFF, 0xFFFF };
            if (in)
                qd_px_rgb(s.depth, s.ctab, get_px(&s, sx, sy), c);
            uint8_t *p = rgba + ((size_t)y * (size_t)w + (size_t)x) * 4;
            p[0] = (uint8_t)(c[0] >> 8);
            p[1] = (uint8_t)(c[1] >> 8);
            p[2] = (uint8_t)(c[2] >> 8);
            p[3] = 255;
        }
    qd_rec_image(qd_port(), w, h, rgba, (qrect){ 0, 0, (int16_t)h, (int16_t)w }, qrect_get(dst_rect));
    free(rgba);
}

static void copy_bits_entry(gaddr src_bitmap, gaddr dst_bitmap, gaddr src_rect, gaddr dst_rect, int16_t mode,
                            gaddr mask);

void tb_CopyBits(gaddr src_bitmap, gaddr dst_bitmap, gaddr src_rect, gaddr dst_rect, int16_t mode, gaddr mask)
{
    if (g_log_mask & LOG_TRAPS) {
        gaddr sb = resolve_bits(src_bitmap), db = resolve_bits(dst_bitmap);
        qrect sr = qrect_get(src_rect), dr = qrect_get(dst_rect), sbb = qrect_get(sb + 6), dbb = qrect_get(db + 6);
        log_printf("  CopyBits %08X rb %04X (%d,%d,%d,%d) [%d,%d,%d,%d] -> %08X rb %04X (%d,%d,%d,%d) [%d,%d,%d,%d] "
                   "mode %d\n", rd32(sb), rd16(sb + 4), sbb.top, sbb.left, sbb.bottom, sbb.right, sr.top, sr.left,
                   sr.bottom, sr.right, rd32(db), rd16(db + 4), dbb.top, dbb.left, dbb.bottom, dbb.right, dr.top, dr.left,
                   dr.bottom, dr.right, mode);
    }
    /* Into the current port: through its bitsProc, when it has one. */
    gaddr port = qd_port();
    gaddr proc = bottleneck(QDProcs_bitsProc_off);
    if (proc && port && resolve_bits(dst_bitmap) == port + GrafPort_portBits_off) {
        cpu_push32(g_cpu, src_bitmap);
        cpu_push32(g_cpu, src_rect);
        cpu_push32(g_cpu, dst_rect);
        cpu_push16(g_cpu, (uint16_t)mode);
        cpu_push32(g_cpu, mask);
        guest_call(proc);
        return;
    }
    copy_bits_entry(src_bitmap, dst_bitmap, src_rect, dst_rect, mode, mask);
}

void tb_StdBits(gaddr src_bitmap, gaddr src_rect, gaddr dst_rect, int16_t mode, gaddr mask)
{
    copy_bits_entry(src_bitmap, qd_port() + GrafPort_portBits_off, src_rect, dst_rect, mode, mask);
}

static void copy_bits_entry(gaddr src_bitmap, gaddr dst_bitmap, gaddr src_rect, gaddr dst_rect, int16_t mode,
                            gaddr mask)
{
    gaddr port = qd_port();
    if (port && qd_rec_active(port) && !qd_rec_suppressed() && resolve_bits(dst_bitmap) == port + GrafPort_portBits_off)
        record_copy(src_bitmap, src_rect, dst_rect);
    hrgn m;
    hrgn_init(&m);
    if (mask)
        rgn_read(mask, &m);
    copy_bits(resolve_bits(src_bitmap), resolve_bits(dst_bitmap), qrect_get(src_rect), qrect_get(dst_rect), mode,
              mask ? &m : NULL);
    hrgn_free(&m);
}

/* The nonzero pixels of a mask's rectangle as a region placed at dst. */
static void mask_region(gaddr mask_bm, qrect mr, qrect dr, hrgn *out)
{
    pixdst mk = bits_at(resolve_bits(mask_bm));
    int w = mr.right - mr.left;
    int16_t *runs = malloc(sizeof *runs * (size_t)(w + 2));
    for (int y = mr.top; y < mr.bottom; y++) {
        int n = 0;
        bool on = false;
        for (int x = mr.left; x <= mr.right; x++) {
            bool px = x < mr.right && y >= mk.bounds.top && y < mk.bounds.bottom && x >= mk.bounds.left &&
                      x < mk.bounds.right && get_px(&mk, x - mk.bounds.left, y - mk.bounds.top) != 0;
            if (px != on) {
                runs[n++] = (int16_t)(x - mr.left + dr.left);
                on = px;
            }
        }
        hrgn_add_runs(out, y - mr.top + dr.top, runs, n);
    }
    free(runs);
}

void tb_CopyMask(gaddr srcbp, gaddr mskbp, gaddr dstbp, gaddr srcrp, gaddr mskrp, gaddr dstrp)
{
    /* Mask bitmap -> region, then a masked copy. */
    qrect dr = qrect_get(dstrp);
    hrgn mask;
    hrgn_init(&mask);
    mask_region(mskbp, qrect_get(mskrp), dr, &mask);
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
        uint32_t *buf = malloc((size_t)w * (size_t)h * sizeof *buf);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                buf[(size_t)y * (size_t)w + (size_t)x] = get_px(
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

/* ---------------------------------------------------------------------- */
/* Colour masks and copying through them */

/* CopyDeepMask: the mask's nonzero pixels, placed on dstRect, limit the
 * copy (with maskRgn too).  The mask is not scaled. */
void tb_CopyDeepMask(gaddr src, gaddr mask, gaddr dst, gaddr src_rect, gaddr mask_rect, gaddr dst_rect, int16_t mode,
                     gaddr mask_rgn)
{
    qrect dr = qrect_get(dst_rect);
    hrgn shape, row;
    hrgn_init(&shape);
    hrgn_init(&row);
    mask_region(mask, qrect_get(mask_rect), dr, &shape);
    if (mask_rgn) {
        rgn_read(mask_rgn, &row);
        hrgn_op(&shape, &shape, &row, RGN_SECT);
    }
    copy_bits(resolve_bits(src), resolve_bits(dst), qrect_get(src_rect), dr, mode, &shape);
    hrgn_free(&shape);
    hrgn_free(&row);
}

/* Flood fills over a colour image, producing a 1-bit mask in dst. */
static void set_mask_bit(gaddr dst, int col, int row, bool on)
{
    pixdst d = bits_at(resolve_bits(dst));
    if (col >= 0 && row >= 0 && col < d.bounds.right - d.bounds.left && row < d.bounds.bottom - d.bounds.top)
        put_px(&d, col, row, on ? 1 : 0);
}

static void flood(const pixdst *s, qrect sr, uint8_t *mark, bool (*pass)(uint32_t, uint32_t), uint32_t key,
                  int sx, int sy)
{
    /* Pixels are marked as they are pushed, so each is on the stack once. */
    int w = sr.right - sr.left, h = sr.bottom - sr.top;
    if (sx < 0 || sy < 0 || sx >= w || sy >= h || mark[sy * w + sx] ||
        !pass(get_px(s, sr.left + sx - s->bounds.left, sr.top + sy - s->bounds.top), key))
        return;
    int *stack = malloc(sizeof(int) * 2 * (size_t)(w * h));
    int n = 0;
    mark[sy * w + sx] = 1;
    stack[n++] = sx;
    stack[n++] = sy;
    while (n) {
        int y = stack[--n], x = stack[--n];
        int nb[8] = { x + 1, y, x - 1, y, x, y + 1, x, y - 1 };
        for (int k = 0; k < 8; k += 2) {
            int nx = nb[k], ny = nb[k + 1];
            if (nx < 0 || ny < 0 || nx >= w || ny >= h || mark[ny * w + nx])
                continue;
            if (!pass(get_px(s, sr.left + nx - s->bounds.left, sr.top + ny - s->bounds.top), key))
                continue;
            mark[ny * w + nx] = 1;
            stack[n++] = nx;
            stack[n++] = ny;
        }
    }
    free(stack);
}

static bool same_px(uint32_t v, uint32_t key) { return v == key; }
static bool other_px(uint32_t v, uint32_t key) { return v != key; }

/* SeedCFill: the area around (seedH, seedV) of the seed pixel's colour. */
void tb_SeedCFill(gaddr src, gaddr dst, gaddr src_rect, gaddr dst_rect, int16_t seed_h, int16_t seed_v, gaddr proc,
                  int32_t data)
{
    pixdst s = bits_at(resolve_bits(src));
    qrect sr = qrect_get(src_rect);
    int w = sr.right - sr.left, h = sr.bottom - sr.top;
    if (w <= 0 || h <= 0 || seed_h < sr.left || seed_v < sr.top || seed_h >= sr.right || seed_v >= sr.bottom)
        return;
    uint8_t *mark = calloc((size_t)(w * h), 1);
    uint32_t key = get_px(&s, seed_h - s.bounds.left, seed_v - s.bounds.top);
    flood(&s, sr, mark, same_px, key, seed_h - sr.left, seed_v - sr.top);
    qrect dr = qrect_get(dst_rect);
    pixdst d = bits_at(resolve_bits(dst));
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            set_mask_bit(dst, dr.left - d.bounds.left + x, dr.top - d.bounds.top + y, mark[y * w + x]);
    free(mark);
}

/* CalcCMask: what can't be reached from the rectangle's edges without
 * crossing pixels of the seed colour. */
void tb_CalcCMask(gaddr src, gaddr dst, gaddr src_rect, gaddr dst_rect, gaddr seed_rgb, gaddr proc, int32_t data)
{
    pixdst s = bits_at(resolve_bits(src));
    qrect sr = qrect_get(src_rect);
    int w = sr.right - sr.left, h = sr.bottom - sr.top;
    if (w <= 0 || h <= 0)
        return;
    uint16_t rgb[3] = { rd16(seed_rgb), rd16(seed_rgb + 2), rd16(seed_rgb + 4) };
    uint32_t key = qd_rgb_px(s.depth, s.ctab, rgb);
    uint8_t *mark = calloc((size_t)(w * h), 1);
    for (int x = 0; x < w; x++) {
        flood(&s, sr, mark, other_px, key, x, 0);
        flood(&s, sr, mark, other_px, key, x, h - 1);
    }
    for (int y = 0; y < h; y++) {
        flood(&s, sr, mark, other_px, key, 0, y);
        flood(&s, sr, mark, other_px, key, w - 1, y);
    }
    qrect dr = qrect_get(dst_rect);
    pixdst d = bits_at(resolve_bits(dst));
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            set_mask_bit(dst, dr.left - d.bounds.left + x, dr.top - d.bounds.top + y, !mark[y * w + x]);
    free(mark);
}
