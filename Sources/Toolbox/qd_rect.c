/* QuickDraw rectangle and point arithmetic. */
#include <math.h>

#include "qd_internal.h"
#include "tb_internal.h"

void tb_SetRect(gaddr r, int16_t left, int16_t top, int16_t right, int16_t bottom)
{
    qrect_put(r, (qrect){ top, left, bottom, right });
}

void tb_OffsetRect(gaddr r, int16_t dh, int16_t dv)
{
    qrect q = qrect_get(r);
    qrect_put(r, (qrect){ (int16_t)(q.top + dv), (int16_t)(q.left + dh), (int16_t)(q.bottom + dv),
                          (int16_t)(q.right + dh) });
}

void tb_InsetRect(gaddr r, int16_t dh, int16_t dv)
{
    qrect q = qrect_get(r);
    q = (qrect){ (int16_t)(q.top + dv), (int16_t)(q.left + dh), (int16_t)(q.bottom - dv), (int16_t)(q.right - dh) };
    if (qrect_empty(q))
        q = (qrect){ 0, 0, 0, 0 }; /* QuickDraw empties rather than inverting */
    qrect_put(r, q);
}

int8_t tb_EmptyRect(gaddr r) { return qrect_empty(qrect_get(r)); }

int8_t tb_SectRect(gaddr s1, gaddr s2, gaddr dest)
{
    qrect a = qrect_get(s1), b = qrect_get(s2);
    qrect c = { a.top > b.top ? a.top : b.top, a.left > b.left ? a.left : b.left,
                a.bottom < b.bottom ? a.bottom : b.bottom, a.right < b.right ? a.right : b.right };
    bool any = !qrect_empty(c);
    qrect_put(dest, any ? c : (qrect){ 0, 0, 0, 0 });
    return any;
}

void tb_UnionRect(gaddr s1, gaddr s2, gaddr dest)
{
    qrect a = qrect_get(s1), b = qrect_get(s2);
    qrect c = { a.top < b.top ? a.top : b.top, a.left < b.left ? a.left : b.left,
                a.bottom > b.bottom ? a.bottom : b.bottom, a.right > b.right ? a.right : b.right };
    qrect_put(dest, c);
}

int8_t tb_PtInRect(uint32_t p, gaddr r)
{
    qrect q = qrect_get(r);
    int16_t h = pt_h(p), v = pt_v(p);
    return h >= q.left && h < q.right && v >= q.top && v < q.bottom;
}

void tb_Pt2Rect(uint32_t p1, uint32_t p2, gaddr dest)
{
    int16_t h1 = pt_h(p1), v1 = pt_v(p1), h2 = pt_h(p2), v2 = pt_v(p2);
    qrect_put(dest, (qrect){ v1 < v2 ? v1 : v2, h1 < h2 ? h1 : h2, v1 > v2 ? v1 : v2, h1 > h2 ? h1 : h2 });
}

int8_t tb_EqualRect(gaddr r1, gaddr r2)
{
    qrect a = qrect_get(r1), b = qrect_get(r2);
    return a.top == b.top && a.left == b.left && a.bottom == b.bottom && a.right == b.right;
}

void tb_PtToAngle(gaddr rp, uint32_t p, gaddr angle)
{
    /* Angle in degrees clockwise from 12 o'clock, scaled to the rectangle. */
    qrect q = qrect_get(rp);
    double cx = (q.left + q.right) / 2.0, cy = (q.top + q.bottom) / 2.0;
    double w = q.right - q.left, h = q.bottom - q.top;
    double dx = (pt_h(p) - cx) / (w ? w : 1), dy = (pt_v(p) - cy) / (h ? h : 1);
    double a = atan2(dx, -dy) * 180.0 / M_PI;
    if (a < 0)
        a += 360;
    wr16(angle, (uint16_t)(int16_t)lround(a) % 360);
}

void tb_AddPt(uint32_t src, gaddr dst)
{
    wr16(dst, (uint16_t)(rd16(dst) + (uint16_t)pt_v(src)));
    wr16(dst + 2, (uint16_t)(rd16(dst + 2) + (uint16_t)pt_h(src)));
}

void tb_SubPt(uint32_t src, gaddr dst)
{
    wr16(dst, (uint16_t)(rd16(dst) - (uint16_t)pt_v(src)));
    wr16(dst + 2, (uint16_t)(rd16(dst + 2) - (uint16_t)pt_h(src)));
}

void tb_SetPt(gaddr pt, int16_t h, int16_t v)
{
    wr16(pt, (uint16_t)v);
    wr16(pt + 2, (uint16_t)h);
}

int8_t tb_EqualPt(uint32_t p1, uint32_t p2) { return p1 == p2; }

static int16_t map1(int16_t x, int16_t s0, int16_t s1, int16_t d0, int16_t d1)
{
    int sw = s1 - s0;
    if (!sw)
        return d0;
    return (int16_t)(d0 + (int32_t)(x - s0) * (d1 - d0) / sw);
}

void tb_MapPt(gaddr pt, gaddr srcr, gaddr dstr)
{
    qrect s = qrect_get(srcr), d = qrect_get(dstr);
    wr16(pt, (uint16_t)map1((int16_t)rd16(pt), s.top, s.bottom, d.top, d.bottom));
    wr16(pt + 2, (uint16_t)map1((int16_t)rd16(pt + 2), s.left, s.right, d.left, d.right));
}

/* A polygon's bounding box and every point, mapped. */
void tb_MapPoly(gaddr poly, gaddr srcr, gaddr dstr)
{
    if (!poly || !rd32(poly))
        return;
    gaddr p = rd32(poly);
    int n = ((int)rd16(p) - 10) / 4;
    tb_MapRect(p + 2, srcr, dstr);
    for (int i = 0; i < n; i++)
        tb_MapPt(p + 10 + (uint32_t)i * 4, srcr, dstr);
}

void tb_MapRect(gaddr r, gaddr srcr, gaddr dstr)
{
    tb_MapPt(r, srcr, dstr);
    tb_MapPt(r + 4, srcr, dstr);
}

void tb_ScalePt(gaddr pt, gaddr srcr, gaddr dstr)
{
    qrect s = qrect_get(srcr), d = qrect_get(dstr);
    int sw = s.right - s.left, sh = s.bottom - s.top;
    int16_t v = (int16_t)rd16(pt), h = (int16_t)rd16(pt + 2);
    if (sw)
        h = (int16_t)((int32_t)h * (d.right - d.left) / sw);
    if (sh)
        v = (int16_t)((int32_t)v * (d.bottom - d.top) / sh);
    if (h < 1 && rd16(pt + 2))
        h = 1;
    if (v < 1 && rd16(pt))
        v = 1;
    wr16(pt, (uint16_t)v);
    wr16(pt + 2, (uint16_t)h);
}

void tb_MapRgn(gaddr rh, gaddr srcr, gaddr dstr)
{
    /* Scale each coordinate of the guest region data in place. */
    qrect s = qrect_get(srcr), d = qrect_get(dstr);
    gaddr p = rd32(rh);
    uint16_t size = rd16(p);
    qrect b = qrect_get(p + 2);
    b = (qrect){ map1(b.top, s.top, s.bottom, d.top, d.bottom), map1(b.left, s.left, s.right, d.left, d.right),
                 map1(b.bottom, s.top, s.bottom, d.top, d.bottom), map1(b.right, s.left, s.right, d.left, d.right) };
    qrect_put(p + 2, b);
    gaddr q = p + 10, end = p + size;
    while (q + 2 <= end) {
        int16_t y = (int16_t)rd16(q);
        if (y == 0x7FFF)
            break;
        wr16(q, (uint16_t)map1(y, s.top, s.bottom, d.top, d.bottom));
        q += 2;
        while (q + 2 <= end) {
            int16_t x = (int16_t)rd16(q);
            q += 2;
            if (x == 0x7FFF)
                break;
            wr16(q - 2, (uint16_t)map1(x, s.left, s.right, d.left, d.right));
        }
    }
}

int16_t tb_Random(void)
{
    /* Park-Miller minimal standard generator, as QuickDraw uses, on randSeed. */
    gaddr globals = rd32(rd32(LM_CurrentA5)) - QDGlobals_thePort_off;
    uint32_t seed = rd32(globals + QDGlobals_randSeed_off);
    if (!seed)
        seed = 1;
    seed = (uint32_t)(((uint64_t)seed * 16807u) % 0x7FFFFFFFu);
    wr32(globals + QDGlobals_randSeed_off, seed);
    int16_t r = (int16_t)(seed & 0xFFFF);
    return r == -32768 ? 0 : r;
}

void tb_StuffHex(gaddr p, gaddr s)
{
    uint8_t n = rd8(s);
    for (unsigned i = 0; i + 1 < n; i += 2) {
        int v = 0;
        for (int k = 0; k < 2; k++) {
            uint8_t c = rd8(s + 1 + i + (unsigned)k);
            v = v * 16 + (c >= '0' && c <= '9' ? c - '0' : (c | 0x20) >= 'a' && (c | 0x20) <= 'f' ? (c | 0x20) - 'a' + 10 : 0);
        }
        wr8(p + i / 2, (uint8_t)v);
    }
}
