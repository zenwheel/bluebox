/*
 * Regions.
 *
 * Host side: bands of x intervals, combined with a sweep over band
 * boundaries.  Guest side: the QuickDraw format apps can read directly:
 *   rgnSize (2), rgnBBox (8), then for non-rectangular regions a list of
 *   scan lines "y, x1, x2, ..., 0x7FFF" giving the x positions where
 *   membership flips relative to the line above, ended by 0x7FFF.
 */
#include <stdlib.h>
#include <string.h>

#include "qd_internal.h"
#include "tb_internal.h"

/* ---------------------------------------------------------------------- */
/* Host regions */

void hrgn_init(hrgn *r) { memset(r, 0, sizeof *r); }

void hrgn_free(hrgn *r)
{
    for (int i = 0; i < r->nbands; i++)
        free(r->bands[i].x);
    free(r->bands);
    memset(r, 0, sizeof *r);
}

static void clear(hrgn *r)
{
    for (int i = 0; i < r->nbands; i++)
        free(r->bands[i].x);
    r->nbands = 0;
}

static bool same_x(const int16_t *a, int na, const int16_t *b, int nb)
{
    return na == nb && (na == 0 || !memcmp(a, b, (size_t)na * sizeof *a));
}

/* Append a band, merging with the previous one when it continues it. */
static void add_band(hrgn *r, int top, int bottom, const int16_t *x, int n)
{
    if (n == 0 || bottom <= top)
        return;
    if (r->nbands) {
        rband *last = &r->bands[r->nbands - 1];
        if (last->bottom == top && same_x(last->x, last->n, x, n)) {
            last->bottom = (int16_t)bottom;
            return;
        }
    }
    if (r->nbands == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 8;
        r->bands = realloc(r->bands, (size_t)r->cap * sizeof *r->bands);
    }
    rband *b = &r->bands[r->nbands++];
    b->top = (int16_t)top;
    b->bottom = (int16_t)bottom;
    b->n = n;
    b->x = malloc((size_t)n * sizeof *b->x);
    memcpy(b->x, x, (size_t)n * sizeof *b->x);
}

void hrgn_rect(hrgn *r, qrect q)
{
    clear(r);
    if (qrect_empty(q))
        return;
    int16_t x[2] = { q.left, q.right };
    add_band(r, q.top, q.bottom, x, 2);
}

bool hrgn_empty(const hrgn *r) { return r->nbands == 0; }

qrect hrgn_bbox(const hrgn *r)
{
    if (!r->nbands)
        return (qrect){ 0, 0, 0, 0 };
    qrect b = { r->bands[0].top, 32767, r->bands[r->nbands - 1].bottom, -32768 };
    for (int i = 0; i < r->nbands; i++) {
        if (r->bands[i].x[0] < b.left)
            b.left = r->bands[i].x[0];
        if (r->bands[i].x[r->bands[i].n - 1] > b.right)
            b.right = r->bands[i].x[r->bands[i].n - 1];
    }
    return b;
}

void hrgn_copy(hrgn *dst, const hrgn *src)
{
    if (dst == src)
        return;
    clear(dst);
    for (int i = 0; i < src->nbands; i++)
        add_band(dst, src->bands[i].top, src->bands[i].bottom, src->bands[i].x, src->bands[i].n);
}

void hrgn_offset(hrgn *r, int dh, int dv)
{
    for (int i = 0; i < r->nbands; i++) {
        r->bands[i].top = (int16_t)(r->bands[i].top + dv);
        r->bands[i].bottom = (int16_t)(r->bands[i].bottom + dv);
        for (int k = 0; k < r->bands[i].n; k++)
            r->bands[i].x[k] = (int16_t)(r->bands[i].x[k] + dh);
    }
}

/* 1-D boolean operation on two sorted interval lists. */
static int op_1d(const int16_t *a, int na, const int16_t *b, int nb, int op, int16_t *out)
{
    int ia = 0, ib = 0, n = 0;
    bool in_a = false, in_b = false, was = false;
    while (ia < na || ib < nb) {
        int x;
        if (ib >= nb || (ia < na && a[ia] <= b[ib]))
            x = a[ia];
        else
            x = b[ib];
        while (ia < na && a[ia] == x) {
            in_a = !in_a;
            ia++;
        }
        while (ib < nb && b[ib] == x) {
            in_b = !in_b;
            ib++;
        }
        bool now;
        switch (op) {
        case RGN_UNION: now = in_a || in_b; break;
        case RGN_SECT: now = in_a && in_b; break;
        case RGN_DIFF: now = in_a && !in_b; break;
        default: now = in_a != in_b; break;
        }
        if (now != was) {
            out[n++] = (int16_t)x;
            was = now;
        }
    }
    return n;
}

void hrgn_op(hrgn *dst, const hrgn *a, const hrgn *b, int op)
{
    hrgn out;
    hrgn_init(&out);
    /* All band boundaries, in order. */
    int ny = 0;
    int16_t *ys = malloc((size_t)(2 * (a->nbands + b->nbands) + 1) * sizeof *ys);
    for (int i = 0; i < a->nbands; i++) {
        ys[ny++] = a->bands[i].top;
        ys[ny++] = a->bands[i].bottom;
    }
    for (int i = 0; i < b->nbands; i++) {
        ys[ny++] = b->bands[i].top;
        ys[ny++] = b->bands[i].bottom;
    }
    for (int i = 1; i < ny; i++) { /* insertion sort: lists are short and mostly sorted */
        int16_t v = ys[i];
        int j = i - 1;
        while (j >= 0 && ys[j] > v) {
            ys[j + 1] = ys[j];
            j--;
        }
        ys[j + 1] = v;
    }
    int maxx = 0;
    for (int i = 0; i < a->nbands; i++)
        maxx = a->bands[i].n > maxx ? a->bands[i].n : maxx;
    int maxb = 0;
    for (int i = 0; i < b->nbands; i++)
        maxb = b->bands[i].n > maxb ? b->bands[i].n : maxb;
    int16_t *xs = malloc((size_t)(maxx + maxb + 2) * sizeof *xs);
    int ia = 0, ib = 0;
    for (int k = 0; k + 1 < ny; k++) {
        int y0 = ys[k], y1 = ys[k + 1];
        if (y1 <= y0)
            continue;
        while (ia < a->nbands && a->bands[ia].bottom <= y0)
            ia++;
        while (ib < b->nbands && b->bands[ib].bottom <= y0)
            ib++;
        const rband *ba = (ia < a->nbands && a->bands[ia].top <= y0) ? &a->bands[ia] : NULL;
        const rband *bb = (ib < b->nbands && b->bands[ib].top <= y0) ? &b->bands[ib] : NULL;
        int n = op_1d(ba ? ba->x : NULL, ba ? ba->n : 0, bb ? bb->x : NULL, bb ? bb->n : 0, op, xs);
        add_band(&out, y0, y1, xs, n);
    }
    free(xs);
    free(ys);
    hrgn_free(dst);
    *dst = out;
}

void hrgn_inset(hrgn *r, int dh, int dv)
{
    /* Horizontal: shrink (or grow and merge) each interval. */
    if (dh) {
        hrgn out;
        hrgn_init(&out);
        for (int i = 0; i < r->nbands; i++) {
            rband *b = &r->bands[i];
            int16_t *x = malloc((size_t)b->n * sizeof *x);
            int n = 0;
            for (int k = 0; k < b->n; k += 2) {
                int x0 = b->x[k] + dh, x1 = b->x[k + 1] - dh;
                if (x1 <= x0)
                    continue;
                if (n && x0 <= x[n - 1]) { /* overlaps the previous interval (growing) */
                    if (x1 > x[n - 1])
                        x[n - 1] = (int16_t)x1;
                    continue;
                }
                x[n++] = (int16_t)x0;
                x[n++] = (int16_t)x1;
            }
            add_band(&out, b->top, b->bottom, x, n);
            free(x);
        }
        hrgn_free(r);
        *r = out;
    }
    /* Vertical: intersect (shrink) or unite (grow) shifted copies. */
    if (dv) {
        int k = dv > 0 ? dv : -dv;
        hrgn acc, shifted;
        hrgn_init(&acc);
        hrgn_init(&shifted);
        hrgn_copy(&acc, r);
        for (int s = -k; s <= k; s++) {
            if (!s)
                continue;
            hrgn_copy(&shifted, r);
            hrgn_offset(&shifted, 0, s);
            hrgn_op(&acc, &acc, &shifted, dv > 0 ? RGN_SECT : RGN_UNION);
        }
        hrgn_free(&shifted);
        hrgn_free(r);
        *r = acc;
    }
}

bool hrgn_contains(const hrgn *r, int h, int v)
{
    for (int i = 0; i < r->nbands; i++) {
        const rband *b = &r->bands[i];
        if (v < b->top)
            return false;
        if (v >= b->bottom)
            continue;
        for (int k = 0; k < b->n; k += 2)
            if (h >= b->x[k] && h < b->x[k + 1])
                return true;
        return false;
    }
    return false;
}

const int16_t *hrgn_row(const hrgn *r, int y, int *n)
{
    for (int i = 0; i < r->nbands; i++) {
        const rband *b = &r->bands[i];
        if (y < b->top)
            break;
        if (y < b->bottom) {
            *n = b->n;
            return b->x;
        }
    }
    *n = 0;
    return NULL;
}

bool hrgn_equal(const hrgn *a, const hrgn *b)
{
    if (a->nbands != b->nbands)
        return false;
    for (int i = 0; i < a->nbands; i++)
        if (a->bands[i].top != b->bands[i].top || a->bands[i].bottom != b->bands[i].bottom ||
            !same_x(a->bands[i].x, a->bands[i].n, b->bands[i].x, b->bands[i].n))
            return false;
    return true;
}

void hrgn_add_row(hrgn *r, int y, int x0, int x1)
{
    if (x1 <= x0)
        return;
    int16_t x[2] = { (int16_t)x0, (int16_t)x1 };
    add_band(r, y, y + 1, x, 2);
}

void hrgn_add_runs(hrgn *r, int y, const int16_t *x, int n) { add_band(r, y, y + 1, x, n); }

void hrgn_from_rows(hrgn *r, int top, int nrows, const int16_t (*rows)[2])
{
    clear(r);
    for (int i = 0; i < nrows; i++)
        hrgn_add_row(r, top + i, rows[i][0], rows[i][1]);
}

/* ---------------------------------------------------------------------- */
/* Guest regions */

static int toggle(int16_t *set, int n, int16_t x)
{
    int i = 0;
    while (i < n && set[i] < x)
        i++;
    if (i < n && set[i] == x) {
        memmove(set + i, set + i + 1, (size_t)(n - i - 1) * sizeof *set);
        return n - 1;
    }
    memmove(set + i + 1, set + i, (size_t)(n - i) * sizeof *set);
    set[i] = x;
    return n + 1;
}

void rgn_read(gaddr rh, hrgn *out)
{
    clear(out);
    if (!rh || !rd32(rh))
        return;
    gaddr p = rd32(rh);
    uint16_t size = rd16(p);
    qrect bbox = qrect_get(p + 2);
    if (size <= 10) {
        hrgn_rect(out, bbox);
        return;
    }
    gaddr end = p + size;
    gaddr q = p + 10;
    int16_t set[1024];
    int n = 0;
    int prev_y = 0;
    bool have_prev = false;
    while (q + 2 <= end) {
        int16_t y = (int16_t)rd16(q);
        q += 2;
        if (y == 0x7FFF)
            break;
        if (have_prev)
            add_band(out, prev_y, y, set, n);
        while (q + 2 <= end) {
            int16_t x = (int16_t)rd16(q);
            q += 2;
            if (x == 0x7FFF)
                break;
            if (n < (int)(sizeof set / sizeof set[0]) - 1)
                n = toggle(set, n, x);
        }
        prev_y = y;
        have_prev = true;
    }
}

/* Symmetric difference of two sorted lists. */
static int sym_diff(const int16_t *a, int na, const int16_t *b, int nb, int16_t *out)
{
    int i = 0, j = 0, n = 0;
    while (i < na || j < nb) {
        if (j >= nb || (i < na && a[i] < b[j]))
            out[n++] = a[i++];
        else if (i >= na || b[j] < a[i])
            out[n++] = b[j++];
        else {
            i++;
            j++;
        }
    }
    return n;
}

void rgn_write(gaddr rh, const hrgn *r)
{
    if (!rh)
        return;
    qrect bbox = hrgn_bbox(r);
    bool rect = r->nbands == 0 || (r->nbands == 1 && r->bands[0].n == 2);
    if (rect) {
        tb_SetHandleSize(rh, 10);
        if (tb_GetHandleSize(rh) < 10)
            return;
        gaddr p = rd32(rh);
        wr16(p, 10);
        qrect_put(p + 2, bbox);
        return;
    }
    /* Size: header + per boundary (y + xs + terminator) + final terminator. */
    int maxn = 0;
    for (int i = 0; i < r->nbands; i++)
        maxn = r->bands[i].n > maxn ? r->bands[i].n : maxn;
    /* Per band at most a gap line and a top line, each "y, xs, 0x7FFF". */
    size_t words = 4 + (size_t)(r->nbands + 1) * (size_t)(4 + 2 * maxn);
    int16_t *buf = malloc(words * sizeof *buf);
    int16_t *diff = malloc((size_t)(2 * maxn + 2) * sizeof *diff);
    size_t w = 0;
    const int16_t *prev = NULL;
    int prevn = 0;
    int prev_bottom = r->bands[0].top;
    for (int i = 0; i < r->nbands; i++) {
        const rband *b = &r->bands[i];
        if (b->top != prev_bottom && prevn) {
            /* Gap: the previous band ends on its own line. */
            buf[w++] = (int16_t)prev_bottom;
            for (int k = 0; k < prevn; k++)
                buf[w++] = prev[k];
            buf[w++] = 0x7FFF;
            prev = NULL;
            prevn = 0;
        }
        int n = sym_diff(prev, prevn, b->x, b->n, diff);
        buf[w++] = b->top;
        for (int k = 0; k < n; k++)
            buf[w++] = diff[k];
        buf[w++] = 0x7FFF;
        prev = b->x;
        prevn = b->n;
        prev_bottom = b->bottom;
    }
    buf[w++] = (int16_t)prev_bottom;
    for (int k = 0; k < prevn; k++)
        buf[w++] = prev[k];
    buf[w++] = 0x7FFF;
    buf[w++] = 0x7FFF;
    uint32_t size = 10 + (uint32_t)w * 2;
    if (size > 32767)
        size = 32767; /* QuickDraw's limit; damaged region rather than overflow */
    tb_SetHandleSize(rh, (int32_t)size);
    int32_t got = tb_GetHandleSize(rh);
    if (got < (int32_t)size) {
        /* A locked region that can't grow (a block after it): its bounding
         * box, which fits, rather than the region written over the next
         * block (Snoop's windows, sent behind, broke the heap). */
        if (got >= 10) {
            gaddr p = rd32(rh);
            wr16(p, 10);
            qrect_put(p + 2, bbox);
        }
        free(diff);
        free(buf);
        return;
    }
    gaddr p = rd32(rh);
    wr16(p, (uint16_t)size);
    qrect_put(p + 2, bbox);
    for (size_t k = 0; k < w && 10 + k * 2 + 2 <= size; k++)
        wr16(p + 10 + (uint32_t)k * 2, (uint16_t)buf[k]);
    free(diff);
    free(buf);
}

gaddr rgn_new_rect(qrect q)
{
    gaddr h = tb_NewHandle(10, false, false);
    if (!h)
        return 0;
    hrgn r;
    hrgn_init(&r);
    hrgn_rect(&r, q);
    rgn_write(h, &r);
    hrgn_free(&r);
    return h;
}

/* ---------------------------------------------------------------------- */
/* Region traps */

gaddr tb_NewRgn(void) { return rgn_new_rect((qrect){ 0, 0, 0, 0 }); }
void tb_DisposeRgn(gaddr rh) { tb_DisposeHandle(rh); }

void tb_CopyRgn(gaddr s, gaddr d)
{
    if (s == d || !d)
        return;
    /* A region that isn't one (a disposed port's clipRgn, whose size is an
     * error, which as a length ran past guest memory: Internet Explorer 4's
     * GetClip in a rootless run) copies as an empty one. */
    int32_t n = s && rd32(s) ? tb_GetHandleSize(s) : 0;
    if (n < 10) {
        tb_SetEmptyRgn(d);
        return;
    }
    tb_SetHandleSize(d, n);
    if (tb_GetHandleSize(d) != n)
        return;
    gmemcpy(rd32(d), rd32(s), (uint32_t)n);
}

void tb_SetEmptyRgn(gaddr rh) { tb_SetRectRgn(rh, 0, 0, 0, 0); }

void tb_SetRectRgn(gaddr rh, int16_t left, int16_t top, int16_t right, int16_t bottom)
{
    hrgn r;
    hrgn_init(&r);
    hrgn_rect(&r, (qrect){ top, left, bottom, right });
    rgn_write(rh, &r);
    hrgn_free(&r);
}

void tb_RectRgn(gaddr rh, gaddr rect)
{
    qrect q = qrect_get(rect);
    tb_SetRectRgn(rh, q.left, q.top, q.right, q.bottom);
}

void tb_OffsetRgn(gaddr rh, int16_t dh, int16_t dv)
{
    /* Offsetting only moves coordinates: patch the guest data in place. */
    gaddr p = rd32(rh);
    uint16_t size = rd16(p);
    qrect b = qrect_get(p + 2);
    b.top = (int16_t)(b.top + dv);
    b.bottom = (int16_t)(b.bottom + dv);
    b.left = (int16_t)(b.left + dh);
    b.right = (int16_t)(b.right + dh);
    qrect_put(p + 2, b);
    if (size <= 10)
        return;
    gaddr q = p + 10, end = p + size;
    while (q + 2 <= end) {
        int16_t y = (int16_t)rd16(q);
        if (y == 0x7FFF)
            break;
        wr16(q, (uint16_t)(y + dv));
        q += 2;
        while (q + 2 <= end) {
            int16_t x = (int16_t)rd16(q);
            if (x == 0x7FFF) {
                q += 2;
                break;
            }
            wr16(q, (uint16_t)(x + dh));
            q += 2;
        }
    }
}

void tb_InsetRgn(gaddr rh, int16_t dh, int16_t dv)
{
    hrgn r;
    hrgn_init(&r);
    rgn_read(rh, &r);
    hrgn_inset(&r, dh, dv);
    rgn_write(rh, &r);
    hrgn_free(&r);
}

static void rgn_op_trap(gaddr s1, gaddr s2, gaddr dest, int op)
{
    hrgn a, b;
    hrgn_init(&a);
    hrgn_init(&b);
    rgn_read(s1, &a);
    rgn_read(s2, &b);
    hrgn_op(&a, &a, &b, op);
    rgn_write(dest, &a);
    hrgn_free(&a);
    hrgn_free(&b);
}

void tb_SectRgn(gaddr s1, gaddr s2, gaddr dest) { rgn_op_trap(s1, s2, dest, RGN_SECT); }
void tb_UnionRgn(gaddr s1, gaddr s2, gaddr dest) { rgn_op_trap(s1, s2, dest, RGN_UNION); }
void tb_DiffRgn(gaddr s1, gaddr s2, gaddr dest) { rgn_op_trap(s1, s2, dest, RGN_DIFF); }
void tb_XorRgn(gaddr s1, gaddr s2, gaddr dest) { rgn_op_trap(s1, s2, dest, RGN_XOR); }

int8_t tb_PtInRgn(uint32_t p, gaddr rh)
{
    hrgn r;
    hrgn_init(&r);
    rgn_read(rh, &r);
    bool in = hrgn_contains(&r, pt_h(p), pt_v(p));
    hrgn_free(&r);
    return in;
}

int8_t tb_RectInRgn(gaddr rp, gaddr rh)
{
    hrgn r, q;
    hrgn_init(&r);
    hrgn_init(&q);
    rgn_read(rh, &r);
    hrgn_rect(&q, qrect_get(rp));
    hrgn_op(&q, &q, &r, RGN_SECT);
    bool any = !hrgn_empty(&q);
    hrgn_free(&r);
    hrgn_free(&q);
    return any;
}

int8_t tb_EqualRgn(gaddr r1, gaddr r2)
{
    hrgn a, b;
    hrgn_init(&a);
    hrgn_init(&b);
    rgn_read(r1, &a);
    rgn_read(r2, &b);
    bool eq = hrgn_equal(&a, &b);
    hrgn_free(&a);
    hrgn_free(&b);
    return eq;
}

int8_t tb_EmptyRgn(gaddr rh)
{
    gaddr p = rd32(rh);
    return qrect_empty(qrect_get(p + 2));
}

/* The area inside a set of edges (h0, v0, h1, v1), even-odd, sampled at
 * each row's centre: polygons, and the lines drawn while a region is open. */
void hrgn_even_odd(hrgn *out, const int (*e)[4], int n)
{
    hrgn_rect(out, (qrect){ 0, 0, 0, 0 });
    if (n < 2)
        return;
    int top = e[0][1], bottom = e[0][1];
    for (int i = 0; i < n; i++)
        for (int k = 1; k < 4; k += 2) {
            if (e[i][k] < top)
                top = e[i][k];
            if (e[i][k] > bottom)
                bottom = e[i][k];
        }
    double *xs = malloc((size_t)n * sizeof *xs);
    for (int y = top; y < bottom; y++) {
        double ys = y + 0.5;
        int k = 0;
        for (int i = 0; i < n; i++) {
            double y0 = e[i][1], y1 = e[i][3];
            if ((y0 <= ys && ys < y1) || (y1 <= ys && ys < y0))
                xs[k++] = e[i][0] + (ys - y0) * (e[i][2] - e[i][0]) / (y1 - y0);
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
    free(xs);
}

/* OpenRgn/CloseRgn record drawing into a region, as QuickDraw did: the
 * lines drawn bound it (even-odd, so a closed outline encloses its inside)
 * and framed shapes go in exclusive-or with the rest.  The pen is hidden
 * while the region is open. */
static bool recording;
static hrgn recorded;
static int (*rec_edges)[4];
static int rec_nedges, rec_cap;

bool qd_region_recording(void) { return recording; }
void qd_region_record(const hrgn *shape)
{
    if (recording)
        hrgn_op(&recorded, &recorded, shape, RGN_XOR);
}

void qd_region_record_line(int h0, int v0, int h1, int v1)
{
    if (!recording || v0 == v1) /* a level line crosses no row */
        return;
    if (rec_nedges == rec_cap) {
        rec_cap = rec_cap ? rec_cap * 2 : 64;
        rec_edges = realloc(rec_edges, (size_t)rec_cap * sizeof *rec_edges);
    }
    int *e = rec_edges[rec_nedges++];
    e[0] = h0;
    e[1] = v0;
    e[2] = h1;
    e[3] = v1;
}

void tb_OpenRgn(void)
{
    hrgn_free(&recorded);
    hrgn_init(&recorded);
    rec_nedges = 0;
    recording = true;
    tb_HidePen();
}

void tb_CloseRgn(gaddr rh)
{
    if (recording)
        tb_ShowPen();
    recording = false;
    hrgn lines;
    hrgn_init(&lines);
    hrgn_even_odd(&lines, (const int (*)[4])rec_edges, rec_nedges);
    hrgn_op(&recorded, &recorded, &lines, RGN_XOR);
    hrgn_free(&lines);
    rec_nedges = 0;
    rgn_write(rh, &recorded);
    hrgn_free(&recorded);
}

/* ---------------------------------------------------------------------- */
/* BitMapToRegion: the 1 bits of a bitmap.  Rows with the same runs make one
 * band. */

static void add_run_band(hrgn *acc, const int *runs, int n, int top, int bottom)
{
    hrgn r;
    hrgn_init(&r);
    for (int i = 0; i + 1 < n; i += 2) {
        hrgn_rect(&r, (qrect){ (int16_t)top, (int16_t)runs[i], (int16_t)bottom, (int16_t)runs[i + 1] });
        hrgn_op(acc, acc, &r, RGN_UNION);
    }
    hrgn_free(&r);
}

int16_t tb_BitMapToRegion(gaddr rgn, gaddr bm)
{
    /* A colour port's portBits (rowBytes' top two bits set) stands for its
     * PixMap, as for CopyBits: Bubblomania passes a copy of a 1-bit
     * GWorld's.  A PixMap must be one bit deep. */
    if ((rd16(bm + 4) & 0xC000) == 0xC000) {
        if (!rd32(bm) || !rd32(rd32(bm)))
            return paramErr;
        bm = rd32(rd32(bm));
    }
    uint16_t rb = rd16(bm + 4);
    if ((rb & 0x8000) && rd16(bm + PixMap_pixelSize_off) != 1)
        return -148; /* pixmapTooDeepErr */
    gaddr base = (rb & 0x8000) ? qd_pixmap_base(bm, true) : rd32(bm);
    uint32_t rowbytes = rb & 0x3FFF;
    qrect b = qrect_get(bm + 6);
    int w = b.right - b.left;
    hrgn acc;
    hrgn_init(&acc);
    hrgn_rect(&acc, (qrect){ 0, 0, 0, 0 });
    int *prev = malloc((size_t)(w + 2) * sizeof *prev), *cur = malloc((size_t)(w + 2) * sizeof *cur);
    int np = 0, band_top = b.top;
    for (int y = b.top; y <= b.bottom; y++) {
        int nc = 0;
        if (y < b.bottom) {
            gaddr row = base + (uint32_t)(y - b.top) * rowbytes;
            bool on = false;
            for (int x = 0; x < w; x++) {
                bool bit = rd8(row + (uint32_t)(x >> 3)) & (0x80 >> (x & 7));
                if (bit != on) {
                    cur[nc++] = b.left + x;
                    on = bit;
                }
            }
            if (on)
                cur[nc++] = b.left + w;
        }
        bool same = nc == np && !memcmp(cur, prev, (size_t)nc * sizeof *cur);
        if (!same) {
            if (np)
                add_run_band(&acc, prev, np, band_top, y);
            int *t = prev;
            prev = cur;
            cur = t;
            np = nc;
            band_top = y;
        }
    }
    free(prev);
    free(cur);
    rgn_write(rgn, &acc);
    hrgn_free(&acc);
    return noErr;
}
