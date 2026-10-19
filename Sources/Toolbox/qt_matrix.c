/*
 * QuickTime's matrix and fixed-point utilities ($ABC2).
 *
 * MatrixRecord: 3x3, row major; the first two columns Fixed (16.16), the
 * third Fract (2.30).  A point (x, y) maps to (x*a + y*c + tx, x*b + y*d +
 * ty), divided by x*u + y*v + w when the matrix has perspective.  The
 * arithmetic is done in doubles and rounded back.
 *
 * wide: a 64-bit signed integer as hi (long) then lo (unsigned long).
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "tb_internal.h"

enum {
    identityMatrixType = 0, translateMatrixType = 1, scaleMatrixType = 2, scaleTranslateMatrixType = 3,
    linearMatrixType = 4, linearTranslateMatrixType = 5, perspectiveMatrixType = 6,
};

typedef double mat[3][3];

static void get(gaddr p, mat m)
{
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) {
            int32_t v = (int32_t)rd32(p + (uint32_t)(r * 12 + c * 4));
            m[r][c] = c == 2 ? v / 1073741824.0 : v / 65536.0;
        }
}

static int32_t clamp_fixed(double v, double one)
{
    double x = v * one;
    x = x > 2147483647.0 ? 2147483647.0 : x < -2147483648.0 ? -2147483648.0 : x;
    return (int32_t)lround(x);
}

static void put(gaddr p, mat m)
{
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            wr32(p + (uint32_t)(r * 12 + c * 4), (uint32_t)clamp_fixed(m[r][c], c == 2 ? 1073741824.0 : 65536.0));
}

static void identity(mat m)
{
    memset(m, 0, sizeof(mat));
    m[0][0] = m[1][1] = m[2][2] = 1;
}

/* r = a * b: a's mapping, then b's. */
static void mul(mat a, mat b, mat r)
{
    mat t;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
    memcpy(r, t, sizeof(mat));
}

static void then(gaddr p, mat by)
{
    mat m;
    get(p, m);
    mul(m, by, m);
    put(p, m);
}

static void apply(mat m, double x, double y, double *ox, double *oy)
{
    double w = x * m[0][2] + y * m[1][2] + m[2][2];
    *ox = x * m[0][0] + y * m[1][0] + m[2][0];
    *oy = x * m[0][1] + y * m[1][1] + m[2][1];
    if (w != 0 && w != 1) {
        *ox /= w;
        *oy /= w;
    }
}

int16_t tb_GetMatrixType(gaddr p)
{
    mat m;
    get(p, m);
    if (m[0][2] != 0 || m[1][2] != 0 || m[2][2] != 1)
        return perspectiveMatrixType;
    bool translate = m[2][0] != 0 || m[2][1] != 0;
    if (m[0][1] != 0 || m[1][0] != 0)
        return translate ? linearTranslateMatrixType : linearMatrixType;
    if (m[0][0] != 1 || m[1][1] != 1)
        return translate ? scaleTranslateMatrixType : scaleMatrixType;
    return translate ? translateMatrixType : identityMatrixType;
}

void tb_CopyMatrix(gaddr m1, gaddr m2) { gmemcpy(m2, m1, 36); }

int8_t tb_EqualMatrix(gaddr m1, gaddr m2)
{
    for (uint32_t i = 0; i < 36; i += 4)
        if (rd32(m1 + i) != rd32(m2 + i))
            return 0;
    return 1;
}

void tb_SetIdentityMatrix(gaddr matrix)
{
    mat m;
    identity(m);
    put(matrix, m);
}

void tb_TranslateMatrix(gaddr p, int32_t deltaH, int32_t deltaV)
{
    mat t;
    identity(t);
    t[2][0] = deltaH / 65536.0;
    t[2][1] = deltaV / 65536.0;
    then(p, t);
}

/* A mapping about a point: there to the origin, by, and back. */
static void about(gaddr p, mat by, int32_t x, int32_t y)
{
    mat to, back;
    identity(to);
    identity(back);
    to[2][0] = -x / 65536.0;
    to[2][1] = -y / 65536.0;
    back[2][0] = x / 65536.0;
    back[2][1] = y / 65536.0;
    mul(to, by, to);
    mul(to, back, to);
    then(p, to);
}

void tb_RotateMatrix(gaddr p, int32_t degrees, int32_t aboutX, int32_t aboutY)
{
    double a = degrees / 65536.0 * M_PI / 180.0;
    mat r;
    identity(r);
    r[0][0] = r[1][1] = cos(a);
    r[0][1] = sin(a);
    r[1][0] = -sin(a);
    about(p, r, aboutX, aboutY);
}

void tb_ScaleMatrix(gaddr p, int32_t scaleX, int32_t scaleY, int32_t aboutX, int32_t aboutY)
{
    mat s;
    identity(s);
    s[0][0] = scaleX / 65536.0;
    s[1][1] = scaleY / 65536.0;
    about(p, s, aboutX, aboutY);
}

void tb_SkewMatrix(gaddr p, int32_t skewX, int32_t skewY, int32_t aboutX, int32_t aboutY)
{
    mat s;
    identity(s);
    s[1][0] = skewX / 65536.0;
    s[0][1] = skewY / 65536.0;
    about(p, s, aboutX, aboutY);
}

void tb_ConcatMatrix(gaddr a, gaddr b)
{
    mat ma, mb;
    get(a, ma);
    get(b, mb);
    mul(ma, mb, mb);
    put(b, mb);
}

int8_t tb_InverseMatrix(gaddr p, gaddr im)
{
    mat m, r;
    get(p, m);
    double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                 m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                 m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (fabs(det) < 1e-12)
        return 0;
    r[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det;
    r[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
    r[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
    r[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det;
    r[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
    r[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
    r[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det;
    r[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
    r[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;
    put(im, r);
    return 1;
}

/* The mapping of one rectangle onto another. */
static void rect_map(gaddr src, gaddr dst, mat m)
{
    double st = (int16_t)rd16(src), sl = (int16_t)rd16(src + 2), sb = (int16_t)rd16(src + 4),
           sr = (int16_t)rd16(src + 6);
    double dt = (int16_t)rd16(dst), dl = (int16_t)rd16(dst + 2), db = (int16_t)rd16(dst + 4),
           dr = (int16_t)rd16(dst + 6);
    identity(m);
    m[0][0] = sr != sl ? (dr - dl) / (sr - sl) : 1;
    m[1][1] = sb != st ? (db - dt) / (sb - st) : 1;
    m[2][0] = dl - sl * m[0][0];
    m[2][1] = dt - st * m[1][1];
}

void tb_RectMatrix(gaddr matrix, gaddr srcRect, gaddr dstRect)
{
    mat m;
    rect_map(srcRect, dstRect, m);
    put(matrix, m);
}

void tb_MapMatrix(gaddr matrix, gaddr fromRect, gaddr toRect)
{
    mat m;
    rect_map(fromRect, toRect, m);
    then(matrix, m);
}

int16_t tb_TransformFixedPoints(gaddr p, gaddr fpt, int32_t count)
{
    mat m;
    get(p, m);
    for (int32_t i = 0; i < count; i++) { /* FixedPoint: x, y */
        gaddr q = fpt + (uint32_t)i * 8;
        double x, y;
        apply(m, (int32_t)rd32(q) / 65536.0, (int32_t)rd32(q + 4) / 65536.0, &x, &y);
        wr32(q, (uint32_t)clamp_fixed(x, 65536.0));
        wr32(q + 4, (uint32_t)clamp_fixed(y, 65536.0));
    }
    return noErr;
}

int16_t tb_TransformPoints(gaddr p, gaddr pt1, int32_t count)
{
    mat m;
    get(p, m);
    for (int32_t i = 0; i < count; i++) { /* Point: v, h */
        gaddr q = pt1 + (uint32_t)i * 4;
        double x, y;
        apply(m, (int16_t)rd16(q + 2), (int16_t)rd16(q), &x, &y);
        wr16(q, (uint16_t)(int16_t)lround(y));
        wr16(q + 2, (uint16_t)(int16_t)lround(x));
    }
    return noErr;
}

/* The corners mapped, the rectangle around them; true if still upright. */
static bool transform_corners(gaddr p, double l, double t, double r, double b, double out[4], gaddr fpp)
{
    mat m;
    get(p, m);
    double xs[4], ys[4];
    apply(m, l, t, &xs[0], &ys[0]);
    apply(m, r, t, &xs[1], &ys[1]);
    apply(m, r, b, &xs[2], &ys[2]);
    apply(m, l, b, &xs[3], &ys[3]);
    out[0] = out[2] = xs[0];
    out[1] = out[3] = ys[0];
    for (int i = 0; i < 4; i++) {
        out[0] = xs[i] < out[0] ? xs[i] : out[0];
        out[2] = xs[i] > out[2] ? xs[i] : out[2];
        out[1] = ys[i] < out[1] ? ys[i] : out[1];
        out[3] = ys[i] > out[3] ? ys[i] : out[3];
        if (fpp) {
            wr32(fpp + (uint32_t)i * 8, (uint32_t)clamp_fixed(xs[i], 65536.0));
            wr32(fpp + (uint32_t)i * 8 + 4, (uint32_t)clamp_fixed(ys[i], 65536.0));
        }
    }
    int type = tb_GetMatrixType(p);
    return type != linearMatrixType && type != linearTranslateMatrixType && type != perspectiveMatrixType;
}

int8_t tb_TransformFixedRect(gaddr p, gaddr fr, gaddr fpp)
{ /* FixedRect: left, top, right, bottom */
    double o[4];
    bool upright = transform_corners(p, (int32_t)rd32(fr) / 65536.0, (int32_t)rd32(fr + 4) / 65536.0,
                                     (int32_t)rd32(fr + 8) / 65536.0, (int32_t)rd32(fr + 12) / 65536.0, o, fpp);
    for (int i = 0; i < 4; i++)
        wr32(fr + (uint32_t)i * 4, (uint32_t)clamp_fixed(o[i], 65536.0));
    return upright;
}

int8_t tb_TransformRect(gaddr p, gaddr r, gaddr fpp)
{ /* Rect: top, left, bottom, right */
    double o[4];
    bool upright = transform_corners(p, (int16_t)rd16(r + 2), (int16_t)rd16(r), (int16_t)rd16(r + 6),
                                     (int16_t)rd16(r + 4), o, fpp);
    wr16(r, (uint16_t)(int16_t)lround(o[1]));
    wr16(r + 2, (uint16_t)(int16_t)lround(o[0]));
    wr16(r + 4, (uint16_t)(int16_t)lround(o[3]));
    wr16(r + 6, (uint16_t)(int16_t)lround(o[2]));
    return upright;
}

/* ---- wide arithmetic ---- */

static int64_t wget(gaddr w) { return (int64_t)((uint64_t)rd32(w) << 32 | rd32(w + 4)); }
static void wput(gaddr w, int64_t v)
{
    wr32(w, (uint32_t)((uint64_t)v >> 32));
    wr32(w + 4, (uint32_t)v);
}

void tb_CompAdd(gaddr src, gaddr dst) { wput(dst, (int64_t)((uint64_t)wget(dst) + (uint64_t)wget(src))); }
void tb_CompSub(gaddr src, gaddr dst) { wput(dst, (int64_t)((uint64_t)wget(dst) - (uint64_t)wget(src))); }
void tb_CompNeg(gaddr dst) { wput(dst, (int64_t)(0 - (uint64_t)wget(dst))); }

void tb_CompShift(gaddr src, int16_t shift)
{ /* positive: right (arithmetic), negative: left */
    int64_t v = wget(src);
    if (shift >= 64 || shift <= -64)
        v = shift > 0 ? (v < 0 ? -1 : 0) : 0;
    else
        v = shift >= 0 ? v >> shift : (int64_t)((uint64_t)v << -shift);
    wput(src, v);
}

void tb_CompMul(int32_t src1, int32_t src2, gaddr dst) { wput(dst, (int64_t)src1 * src2); }

int32_t tb_CompDiv(gaddr numerator, int32_t denominator, gaddr remainder)
{
    int64_t n = wget(numerator);
    if (!denominator) {
        if (remainder)
            wr32(remainder, 0);
        return n < 0 ? INT32_MIN : INT32_MAX;
    }
    if (remainder)
        wr32(remainder, (uint32_t)(int32_t)(n % denominator));
    int64_t q = n / denominator;
    return q > INT32_MAX ? INT32_MAX : q < INT32_MIN ? INT32_MIN : (int32_t)q;
}

void tb_CompFixMul(gaddr compSrc, int32_t fixSrc, gaddr compDst)
{
    __int128 v = (__int128)wget(compSrc) * fixSrc;
    wput(compDst, (int64_t)(v >> 16));
}

void tb_CompMulDiv(gaddr co, int32_t mul, int32_t divisor)
{
    if (!divisor)
        return;
    __int128 v = (__int128)wget(co) * mul;
    __int128 q = (v + (v >= 0 ? divisor / 2 : -divisor / 2)) / divisor; /* rounded */
    wput(co, (int64_t)q);
}

void tb_CompMulDivTrunc(gaddr co, int32_t mul, int32_t divisor, gaddr remainder)
{
    if (!divisor)
        return;
    __int128 v = (__int128)wget(co) * mul;
    if (remainder)
        wr32(remainder, (uint32_t)(int32_t)(v % divisor));
    wput(co, (int64_t)(v / divisor));
}

int32_t tb_CompCompare(gaddr a, gaddr minusb)
{ /* a against -minusb: the sign of a + minusb */
    __int128 s = (__int128)wget(a) + wget(minusb);
    return s > 0 ? 1 : s < 0 ? -1 : 0;
}

uint32_t tb_CompSquareRoot(gaddr src)
{
    uint64_t v = (uint64_t)wget(src), r = (uint64_t)sqrtl((long double)v);
    while (r * r > v)
        r--;
    while ((r + 1) * (r + 1) <= v)
        r++;
    return (uint32_t)r;
}

int32_t tb_FixMulDiv(int32_t src, int32_t mul, int32_t divisor)
{
    if (!divisor)
        return (src < 0) != (mul < 0) ? INT32_MIN : INT32_MAX;
    double q = round((double)src * mul / divisor);
    return q > INT32_MAX ? INT32_MAX : q < INT32_MIN ? INT32_MIN : (int32_t)q;
}

int32_t tb_UnsignedFixMulDiv(int32_t src, int32_t mul, int32_t divisor)
{
    if (!divisor)
        return (int32_t)0xFFFFFFFFu;
    uint64_t v = (uint64_t)(uint32_t)src * (uint32_t)mul, d = (uint32_t)divisor;
    uint64_t q = (v + d / 2) / d;
    return (int32_t)(uint32_t)(q > 0xFFFFFFFFu ? 0xFFFFFFFFu : q);
}

int32_t tb_FracSinCos(int32_t degree, gaddr cosOut)
{
    double a = degree / 65536.0 * M_PI / 180.0;
    if (cosOut)
        wr32(cosOut, (uint32_t)clamp_fixed(cos(a), 1073741824.0));
    return clamp_fixed(sin(a), 1073741824.0);
}

int32_t tb_FixExp2(int32_t src) { return clamp_fixed(exp2(src / 65536.0), 65536.0); }
int32_t tb_FixLog2(int32_t src) { return src > 0 ? clamp_fixed(log2(src / 65536.0), 65536.0) : INT32_MIN; }
int32_t tb_FixPow(int32_t base, int32_t exp) { return clamp_fixed(pow(base / 65536.0, exp / 65536.0), 65536.0); }
