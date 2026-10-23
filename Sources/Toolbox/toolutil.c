/* Toolbox Utilities: bit and fixed-point arithmetic, strings, Munger, PackBits. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "qd_internal.h"
#include "tb_internal.h"

/* Bits are numbered from the high-order bit of the first byte. */
int8_t tb_BitTst(gaddr bp, int32_t bn) { return (rd8(bp + (uint32_t)(bn >> 3)) >> (7 - (bn & 7))) & 1; }
void tb_BitSet(gaddr bp, int32_t bn) { wr8(bp + (uint32_t)(bn >> 3), (uint8_t)(rd8(bp + (uint32_t)(bn >> 3)) | (0x80 >> (bn & 7)))); }
void tb_BitClr(gaddr bp, int32_t bn) { wr8(bp + (uint32_t)(bn >> 3), (uint8_t)(rd8(bp + (uint32_t)(bn >> 3)) & ~(0x80 >> (bn & 7)))); }

int32_t tb_BitAnd(int32_t a, int32_t b) { return a & b; }
int32_t tb_BitOr(int32_t a, int32_t b) { return a | b; }
int32_t tb_BitXor(int32_t a, int32_t b) { return a ^ b; }
int32_t tb_BitNot(int32_t a) { return ~a; }

int32_t tb_BitShift(int32_t a, int16_t n)
{
    n &= 31; /* only the count modulo 32 matters; positive shifts left */
    int16_t s = (int16_t)(n >= 16 ? n - 32 : n);
    return s >= 0 ? (int32_t)((uint32_t)a << s) : (int32_t)((uint32_t)a >> -s);
}

int16_t tb_HiWord(int32_t a) { return (int16_t)((uint32_t)a >> 16); }
int16_t tb_LoWord(int32_t a) { return (int16_t)a; }

void tb_LongMul(int32_t a, int32_t b, gaddr c)
{
    int64_t r = (int64_t)a * b;
    wr32(c, (uint32_t)((uint64_t)r >> 32));
    wr32(c + 4, (uint32_t)r);
}

/* ---- Fixed (16.16) and Fract (2.30) ---- */

static int32_t sat(int64_t v)
{
    if (v > INT32_MAX)
        return INT32_MAX;
    if (v < -INT32_MAX)
        return -INT32_MAX;
    return (int32_t)v;
}

int32_t tb_FixMul(int32_t a, int32_t b) { return sat(((int64_t)a * b + 0x8000) >> 16); }

int32_t tb_FixRatio(int16_t n, int16_t d)
{
    if (!d)
        return n < 0 ? -INT32_MAX : INT32_MAX;
    return sat(((int64_t)n << 16) / d);
}

int16_t tb_FixRound(int32_t x) { return (int16_t)((x + 0x8000) >> 16); }

int32_t tb_FixDiv(int32_t x, int32_t y)
{
    if (!y)
        return x < 0 ? -INT32_MAX : INT32_MAX;
    return sat(((int64_t)x << 16) / y);
}

int32_t tb_FracMul(int32_t x, int32_t y) { return sat(((int64_t)x * y + (1 << 29)) >> 30); }

int32_t tb_FracDiv(int32_t x, int32_t y)
{
    if (!y)
        return x < 0 ? -INT32_MAX : INT32_MAX;
    return sat(((int64_t)x << 30) / y);
}

int32_t tb_FracSqrt(int32_t x) { return x <= 0 ? 0 : (int32_t)(sqrt((double)(uint32_t)x / (1 << 30)) * (1 << 30)); }
int32_t tb_Long2Fix(int32_t x) { return sat((int64_t)x << 16); }
int32_t tb_Fix2Long(int32_t x) { return (x + 0x8000) >> 16; }
int32_t tb_Fix2Frac(int32_t x) { return sat((int64_t)x << 14); }
int32_t tb_Frac2Fix(int32_t x) { return (x + 0x2000) >> 14; }

int32_t tb_SlopeFromAngle(int16_t a)
{
    double r = (90.0 - a) * M_PI / 180.0;
    double t = tan(r);
    return sat((int64_t)(-t * 65536.0));
}

int16_t tb_AngleFromSlope(int32_t s)
{
    double a = 90.0 + atan((double)s / 65536.0) * 180.0 / M_PI;
    return (int16_t)lround(a) % 180;
}

int32_t tb_DeltaPoint(uint32_t a, uint32_t b)
{
    return (int32_t)make_pt((int16_t)(pt_v(a) - pt_v(b)), (int16_t)(pt_h(a) - pt_h(b)));
}

/* ---- Strings ---- */

gaddr tb_NewString(gaddr s)
{
    uint32_t n = (uint32_t)rd8(s) + 1;
    gaddr h = tb_NewHandle((int32_t)n, false, false);
    if (h)
        gmemcpy(rd32(h), s, n);
    return h;
}

void tb_SetString(gaddr h, gaddr s)
{
    uint32_t n = (uint32_t)rd8(s) + 1;
    tb_SetHandleSize(h, (int32_t)n);
    gmemcpy(rd32(h), s, n);
}

void tb_GetIndPattern(gaddr op, int16_t plistid, int16_t index)
{
    gaddr h = tb_GetResource((int32_t)fourcc("PAT#"), plistid);
    if (!h || !rd32(h) || index < 1 || index > (int16_t)rd16(rd32(h)))
        return;
    gmemcpy(op, rd32(h) + 2 + (uint32_t)(index - 1) * 8, 8);
}

void tb_ScreenRes(gaddr h, gaddr v)
{
    wr16(h, rd16(LM_ScrHRes));
    wr16(v, rd16(LM_ScrVRes));
}

/* Munger: find p1 (len1) in h from off; replace it with p2 (len2). */
int32_t tb_Munger(gaddr h, int32_t off, gaddr p1, int32_t len1, gaddr p2, int32_t len2)
{
    int32_t size = tb_GetHandleSize(h);
    if (off < 0 || off > size)
        return -1;
    int32_t at = off;
    if (p1) {
        at = -1;
        for (int32_t i = off; i + len1 <= size; i++) {
            bool match = true;
            for (int32_t k = 0; k < len1 && match; k++)
                match = rd8(rd32(h) + (uint32_t)(i + k)) == rd8(p1 + (uint32_t)k);
            if (match) {
                at = i;
                break;
            }
        }
        if (at < 0)
            return -1;
    } else if (len1 < 0 || at + len1 > size) {
        len1 = size - at; /* nil target: replace from off to the end */
    }
    if (!p2)
        return at; /* search only */
    int32_t newsize = size - len1 + len2;
    if (len2 > len1)
        tb_SetHandleSize(h, newsize);
    gaddr base = rd32(h);
    gmemcpy(base + (uint32_t)(at + len2), base + (uint32_t)(at + len1), (uint32_t)(size - at - len1));
    gmemcpy(base + (uint32_t)at, p2, (uint32_t)len2);
    if (len2 < len1)
        tb_SetHandleSize(h, newsize);
    return at + len2;
}

/* PackBits takes and updates pointers to the source and destination pointers. */
void tb_PackBits(gaddr sp, gaddr dp, int16_t len)
{
    gaddr s = rd32(sp), d = rd32(dp);
    int i = 0;
    while (i < len) {
        int run = 1;
        while (i + run < len && run < 128 && rd8(s + (uint32_t)(i + run)) == rd8(s + (uint32_t)i))
            run++;
        if (run >= 2) {
            wr8(d++, (uint8_t)(1 - run));
            wr8(d++, rd8(s + (uint32_t)i));
            i += run;
            continue;
        }
        int lit = 1;
        while (i + lit < len && lit < 128 &&
               !(i + lit + 1 < len && rd8(s + (uint32_t)(i + lit)) == rd8(s + (uint32_t)(i + lit + 1))))
            lit++;
        wr8(d++, (uint8_t)(lit - 1));
        for (int k = 0; k < lit; k++)
            wr8(d++, rd8(s + (uint32_t)(i + k)));
        i += lit;
    }
    wr32(sp, s + (uint32_t)len);
    wr32(dp, d);
}

void tb_UnpackBits(gaddr sp, gaddr dp, int16_t len)
{
    gaddr s = rd32(sp), d = rd32(dp);
    int out = 0;
    while (out < len) {
        int8_t flag = (int8_t)rd8(s++);
        if (flag >= 0) {
            for (int k = 0; k <= flag && out < len; k++, out++)
                wr8(d++, rd8(s++));
        } else if (flag != -128) {
            uint8_t v = rd8(s++);
            for (int k = 0; k < 1 - flag && out < len; k++, out++)
                wr8(d++, v);
        }
    }
    wr32(sp, s);
    wr32(dp, d);
}

/* ---- Binary-decimal conversion (Pack7) ---- */

void tb_NumToString(int32_t l, gaddr s)
{
    char buf[16];
    int n = snprintf(buf, sizeof buf, "%d", (int)l);
    wr8(s, (uint8_t)n);
    gput(s + 1, buf, (uint32_t)n);
}

/* Like the ROM: no error checking; a leading sign, then digits, each
 * character's low four bits (so a space counts as 0: Slime Invaders' help
 * text gives " 137" for its picture's ID). */
void tb_StringToNum(gaddr s, uint32_t *lp)
{
    int n = rd8(s), i = 0;
    bool neg = false;
    if (n > 0 && (rd8(s + 1) == '-' || rd8(s + 1) == '+')) {
        neg = rd8(s + 1) == '-';
        i = 1;
    }
    uint32_t v = 0;
    for (; i < n; i++)
        v = v * 10 + (rd8(s + 1 + (uint32_t)i) & 15u);
    *lp = neg ? (uint32_t)-(int32_t)v : v;
}

/* Fixed-point trigonometry: angles in radians as Fixed, results as Fract. */
int32_t tb_FracSin(int32_t x) { return (int32_t)lround(sin(x / 65536.0) * (1 << 30)); }
int32_t tb_FracCos(int32_t x) { return (int32_t)lround(cos(x / 65536.0) * (1 << 30)); }
int32_t tb_FixATan2(int32_t x, int32_t y) { return (int32_t)lround(atan2((double)y, (double)x) * 65536.0); }
