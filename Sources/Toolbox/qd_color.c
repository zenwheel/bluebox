/*
 * Colour for QuickDraw: pixel values <-> RGB through colour tables, and the
 * standard Macintosh colour tables.
 */
#include <string.h>

#include "qd_internal.h"
#include "tb_internal.h"

/* The standard 8-bit table: a 6x6x6 cube from white, then ten-step ramps
 * of red, green, blue and grey, then black. */
void qd_std_rgb8(int i, uint16_t rgb[3])
{
    if (i < 215) {
        int r = 5 - i / 36, g = 5 - (i / 6) % 6, b = 5 - i % 6;
        rgb[0] = (uint16_t)(r * 0x3333);
        rgb[1] = (uint16_t)(g * 0x3333);
        rgb[2] = (uint16_t)(b * 0x3333);
        return;
    }
    if (i >= 255) {
        rgb[0] = rgb[1] = rgb[2] = 0;
        return;
    }
    static const uint16_t ramp[10] = { 0xEEEE, 0xDDDD, 0xBBBB, 0xAAAA, 0x8888, 0x7777, 0x5555, 0x4444, 0x2222, 0x1111 };
    uint16_t v = ramp[(i - 215) % 10];
    rgb[0] = rgb[1] = rgb[2] = 0;
    switch ((i - 215) / 10) {
    case 0: rgb[0] = v; break;
    case 1: rgb[1] = v; break;
    case 2: rgb[2] = v; break;
    default: rgb[0] = rgb[1] = rgb[2] = v; break;
    }
}

static void std_rgb(int depth, uint32_t v, uint16_t rgb[3])
{
    if (depth == 8) {
        qd_std_rgb8((int)v, rgb);
        return;
    }
    /* The standard 'clut' 4 and 'clut' 2 of the ROM: the sixteen system
     * colours, and four greys. */
    static const uint16_t clut4[16][3] = {
        { 0xFFFF, 0xFFFF, 0xFFFF }, { 0xFC00, 0xF37D, 0x052F }, { 0xFFFF, 0x648A, 0x028C }, { 0xDD6B, 0x08C2, 0x06A2 },
        { 0xF2D7, 0x0856, 0x84EC }, { 0x46E3, 0x0000, 0xA53E }, { 0x0000, 0x0000, 0xD400 }, { 0x0241, 0xAB54, 0xEAFF },
        { 0x1F21, 0xB793, 0x1431 }, { 0x0000, 0x64AF, 0x11B0 }, { 0x5600, 0x2C9D, 0x0524 }, { 0x90D7, 0x7160, 0x3A34 },
        { 0xC000, 0xC000, 0xC000 }, { 0x8000, 0x8000, 0x8000 }, { 0x4000, 0x4000, 0x4000 }, { 0x0000, 0x0000, 0x0000 },
    };
    static const uint16_t clut2[4] = { 0xFFFF, 0xACAC, 0x5555, 0x0000 };
    if (depth == 4) {
        memcpy(rgb, clut4[v & 15], sizeof clut4[0]);
        return;
    }
    rgb[0] = rgb[1] = rgb[2] = clut2[v & 3];
}

void qd_px_rgb(int depth, gaddr ctab, uint32_t v, uint16_t rgb[3])
{
    switch (depth) {
    case 1:
        rgb[0] = rgb[1] = rgb[2] = v ? 0 : 0xFFFF;
        return;
    case 16:
        rgb[0] = (uint16_t)(((v >> 10) & 31) * 0xFFFF / 31);
        rgb[1] = (uint16_t)(((v >> 5) & 31) * 0xFFFF / 31);
        rgb[2] = (uint16_t)((v & 31) * 0xFFFF / 31);
        return;
    case 32:
        rgb[0] = (uint16_t)(((v >> 16) & 0xFF) * 0x101);
        rgb[1] = (uint16_t)(((v >> 8) & 0xFF) * 0x101);
        rgb[2] = (uint16_t)((v & 0xFF) * 0x101);
        return;
    default:
        break;
    }
    if (ctab && rd32(ctab)) {
        gaddr t = rd32(ctab);
        int n = (int16_t)rd16(t + 6) + 1;
        bool device = (rd16(t + 4) & 0x8000) != 0;
        for (int i = 0; i < n; i++) {
            gaddr e = t + 8 + (uint32_t)i * 8;
            if ((device ? (uint32_t)i : rd16(e)) == v) {
                rgb[0] = rd16(e + 2);
                rgb[1] = rd16(e + 4);
                rgb[2] = rd16(e + 6);
                return;
            }
        }
    }
    std_rgb(depth, v, rgb);
}

static uint32_t dist(const uint16_t a[3], const uint16_t b[3])
{
    int dr = (a[0] >> 8) - (b[0] >> 8), dg = (a[1] >> 8) - (b[1] >> 8), db = (a[2] >> 8) - (b[2] >> 8);
    return (uint32_t)(dr * dr * 3 + dg * dg * 4 + db * db * 2);
}

/* Nearest entry; a one-entry cache keeps solid fills cheap. */
uint32_t qd_rgb_px(int depth, gaddr ctab, const uint16_t rgb[3])
{
    switch (depth) {
    case 1: return (rgb[0] * 30u + rgb[1] * 59u + rgb[2] * 11u) / 100u < 0x8000u ? 1 : 0;
    case 16: return (uint32_t)((rgb[0] >> 11) << 10 | (rgb[1] >> 11) << 5 | (rgb[2] >> 11));
    case 32: return (uint32_t)((rgb[0] >> 8) << 16 | (rgb[1] >> 8) << 8 | (rgb[2] >> 8));
    default: break;
    }
    static struct {
        int depth;
        gaddr ctab;
        uint32_t seed;
        uint16_t rgb[3];
        uint32_t v;
        bool valid;
    } cache;
    uint32_t seed = ctab && rd32(ctab) ? rd32(rd32(ctab)) : 0;
    if (cache.valid && cache.depth == depth && cache.ctab == ctab && cache.seed == seed && !memcmp(cache.rgb, rgb, 6))
        return cache.v;
    uint32_t best = 0, best_d = UINT32_MAX, n = px_mask(depth) + 1;
    /* Black is the last entry when that is black, as in a device's table,
     * even while a fade has made other entries black too. */
    uint16_t last[3];
    qd_px_rgb(depth, ctab, n - 1, last);
    if (!(rgb[0] | rgb[1] | rgb[2]) && !dist(last, rgb)) {
        best = n - 1;
        best_d = 0;
        n = 0;
    }
    for (uint32_t v = 0; v < n; v++) {
        uint16_t c[3];
        qd_px_rgb(depth, ctab, v, c);
        uint32_t d = dist(c, rgb);
        if (d < best_d) {
            best_d = d;
            best = v;
            if (!d)
                break;
        }
    }
    cache.depth = depth;
    cache.ctab = ctab;
    cache.seed = seed;
    memcpy(cache.rgb, rgb, 6);
    cache.v = best;
    cache.valid = true;
    return best;
}
