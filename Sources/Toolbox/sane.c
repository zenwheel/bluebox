/*
 * SANE: the Standard Apple Numerics Environment, as the FP68K (Pack4) and
 * Elems68K (Pack5) packages (Apple Numerics Manual, 2nd ed., part III).
 *
 * Each call pushes operand addresses, then an operation word, then traps.
 * Bits 0-7 of the word select the operation and bits 11-13 the format of
 * the source operand (extended, double, single, -, integer, longint, comp).
 * Operands are pushed SRC2, SRC, DST: the destination's address is on top.
 *
 * FP68K's arithmetic is done here in software in the 80-bit extended
 * format itself (a 64-bit significand, an explicit integer bit), correctly
 * rounded in each of the four rounding directions, with the environment's
 * rounding precision, the five exceptions, NaN codes and halts as the
 * manual gives them.  The environment word and halt vector live where the
 * Mac keeps them, in FPState (low memory $0A4A).  Elems68K's functions are
 * computed in host doubles with their ranges reduced so that results in
 * extended's range beyond a double's still come out, then rounded here.
 */
#include <fenv.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "m68k.h"
#include "legacy.h"
#include "tb_internal.h"

enum { F_EXT = 0, F_DBL, F_SGL, F_EXT96, F_INT, F_LONG, F_COMP };

/* Exceptions, in environment-word order (flags at bit 8, halts at bit 0). */
enum { X_INVALID = 1, X_UNDERFLOW = 2, X_OVERFLOW = 4, X_DIVBYZERO = 8, X_INEXACT = 16 };

/* SANE's NaN codes, kept in the significand's second byte. */
enum {
    NAN_SQRT = 1, NAN_ADD = 2, NAN_DIV = 4, NAN_MUL = 8, NAN_REM = 9, NAN_ASCII = 17, NAN_COMP = 20,
    NAN_ZERO = 21, NAN_TRIG = 33, NAN_INVTRIG = 34, NAN_LOG = 36, NAN_POWER = 37, NAN_FINANCE = 38,
};

/* FPState: the environment word, then the halt vector. */
#define LM_FPSTATE 0x0A4Au

static uint16_t env_get(void) { return rd16(LM_FPSTATE); }
static void env_set(uint16_t e) { wr16(LM_FPSTATE, e); }

/* The exceptions the current operation raised. */
static uint16_t raised;

typedef unsigned __int128 u128;

/* ---------------------------------------------------------------------- */
/* Extended numbers, unpacked: a finite one's value is m * 2^(e - 63) with
 * m's top bit set; a NaN keeps its significand in m (bit 62 set when it is
 * quiet). */

enum { K_ZERO, K_FIN, K_INF, K_NAN };

typedef struct {
    int sign, kind;
    int32_t e;
    uint64_t m;
} xnum;

enum { EXT_EMIN = -16382, EXT_EMAX = 16383 };

static xnum x_zero(int sign) { return (xnum){ sign, K_ZERO, 0, 0 }; }
static xnum x_inf(int sign) { return (xnum){ sign, K_INF, 0, 0 }; }

/* A new NaN from an invalid operation: invalid is signalled. */
static xnum x_nan(int code)
{
    raised |= X_INVALID;
    return (xnum){ 0, K_NAN, 0, 0x4000000000000000ull | (uint64_t)(code & 0xFF) << 48 };
}

static bool x_snan(xnum x) { return x.kind == K_NAN && !(x.m & 0x4000000000000000ull); }

/* A NaN operand's result: quieted, with invalid signalled for a signaling one. */
static xnum x_quiet(xnum x)
{
    if (x_snan(x))
        raised |= X_INVALID;
    x.m |= 0x4000000000000000ull;
    return x;
}

/* Two operands of which one or both are NaNs: the one with the larger
 * NaN code (the destination's on a tie). */
static xnum x_nan2(xnum a, xnum b)
{
    if (x_snan(a) || x_snan(b))
        raised |= X_INVALID;
    xnum r = a;
    if (a.kind != K_NAN || (b.kind == K_NAN && ((b.m >> 48) & 0xFF) > ((a.m >> 48) & 0xFF)))
        r = b;
    r.m |= 0x4000000000000000ull;
    return r;
}

static int clz128(u128 v)
{
    uint64_t hi = (uint64_t)(v >> 64);
    return hi ? __builtin_clzll(hi) : 64 + __builtin_clzll((uint64_t)v);
}

/* The rounding direction: 0 to nearest, 1 upward, 2 downward, 3 toward zero. */
static int rounding(void) { return (env_get() >> 13) & 3; }

/* Round sig * 2^(e - 127) (sig need not be normalized; a sticky bit for
 * anything already dropped sits in its lowest bit) to prec significant
 * bits within the exponents [emin, emax], raising inexact, underflow and
 * overflow as they happen. */
static xnum x_round(int sign, int32_t e, u128 sig, int prec, int32_t emin, int32_t emax)
{
    if (!sig)
        return x_zero(sign);
    int k = clz128(sig);
    sig <<= k;
    e -= k;
    bool tiny = e < emin;
    int keep = tiny ? prec - (emin - e) : prec;
    u128 kept;
    bool half, sticky;
    if (keep >= 1) {
        kept = sig >> (128 - keep);
        u128 rest = sig << keep;
        half = (bool)(rest >> 127);
        sticky = (rest << 1) != 0;
    } else {
        /* Below the smallest denormal's unit, or half of it at most. */
        kept = 0;
        half = keep == 0;
        sticky = keep == 0 ? (sig << 1) != 0 : true;
    }
    bool inexact = half || sticky, up;
    switch (rounding()) {
    case 0: up = half && (sticky || (kept & 1)); break;
    case 1: up = inexact && !sign; break;
    case 2: up = inexact && sign; break;
    default: up = false; break;
    }
    kept += up;
    if (inexact) {
        raised |= X_INEXACT;
        if (tiny)
            raised |= X_UNDERFLOW;
    }
    if (!kept)
        return x_zero(sign);
    /* kept counts units of 2^(e + 1 - keep). */
    int len = 128 - clz128(kept);
    int32_t E = keep < 0 ? emin - prec + 1 : e - keep + len;
    if (E > emax) {
        raised |= X_OVERFLOW | X_INEXACT;
        int mode = rounding();
        if (mode == 0 || (mode == 1 && !sign) || (mode == 2 && sign))
            return x_inf(sign);
        uint64_t max = prec >= 64 ? ~0ull : ~0ull << (64 - prec);
        return (xnum){ sign, K_FIN, emax, max };
    }
    uint64_t m = len > 64 ? (uint64_t)(kept >> (len - 64)) : (uint64_t)(kept << (64 - len));
    return (xnum){ sign, K_FIN, E, m };
}

/* Rounding precision from the environment (bits 5-6): extended, double or single. */
static xnum x_round_env(int sign, int32_t e, u128 sig)
{
    switch ((env_get() >> 5) & 3) {
    case 1: return x_round(sign, e, sig, 53, -1022, 1023);
    case 2: return x_round(sign, e, sig, 24, -126, 127);
    default: return x_round(sign, e, sig, 64, EXT_EMIN, EXT_EMAX);
    }
}

/* An exact result (or one already rounded) put through the environment's
 * rounding precision. */
static xnum x_fit(xnum x)
{
    if (x.kind != K_FIN)
        return x;
    return x_round_env(x.sign, x.e, (u128)x.m << 64);
}

/* ---------------------------------------------------------------------- */
/* Formats in memory */

static xnum unpack_ext(uint16_t se, uint64_t m)
{
    xnum x = { se >> 15, K_FIN, 0, m };
    int ef = se & 0x7FFF;
    if (ef == 0x7FFF) {
        x.kind = (m << 1) ? K_NAN : K_INF;
        return x;
    }
    if (!m) {
        x.kind = K_ZERO;
        return x;
    }
    /* Denormals (and unnormals) are normalized. */
    int k = __builtin_clzll(m);
    x.m = m << k;
    x.e = (ef ? ef : 1) - 16383 - k;
    return x;
}

static xnum ext_load(gaddr p) { return unpack_ext(rd16(p), (uint64_t)rd32(p + 2) << 32 | rd32(p + 6)); }

static void ext_store(gaddr p, xnum x)
{
    uint16_t se = x.sign ? 0x8000 : 0;
    uint64_t m = 0;
    switch (x.kind) {
    case K_NAN: se |= 0x7FFF; m = x.m; break;
    case K_INF: se |= 0x7FFF; break;
    case K_FIN:
        if (x.e >= EXT_EMIN) {
            se |= (uint16_t)(x.e + 16383);
            m = x.m;
        } else { /* a denormal */
            int s = EXT_EMIN - x.e;
            m = s < 64 ? x.m >> s : 0;
        }
        break;
    }
    wr16(p, se);
    wr32(p + 2, (uint32_t)(m >> 32));
    wr32(p + 6, (uint32_t)m);
}

static xnum from_ieee(uint64_t bits, int fracbits, int expbits)
{
    int sign = (int)(bits >> (fracbits + expbits)) & 1;
    int bias = (1 << (expbits - 1)) - 1;
    int ef = (int)(bits >> fracbits) & ((1 << expbits) - 1);
    uint64_t f = bits & ((1ull << fracbits) - 1);
    if (ef == (1 << expbits) - 1) {
        if (!f)
            return x_inf(sign);
        return (xnum){ sign, K_NAN, 0, f << (63 - fracbits) };
    }
    if (!ef && !f)
        return x_zero(sign);
    uint64_t m = ef ? (f | 1ull << fracbits) : f;
    int k = __builtin_clzll(m);
    return (xnum){ sign, K_FIN, (ef ? ef : 1) - bias - fracbits + 63 - k, m << k };
}

/* The bits of x in an IEEE format: rounded to it first (with its own
 * exponent range), so no conversion here loses anything. */
static uint64_t to_ieee(xnum x, int fracbits, int expbits)
{
    int bias = (1 << (expbits - 1)) - 1;
    uint64_t sign = (uint64_t)x.sign << (fracbits + expbits), ones = (1ull << expbits) - 1;
    switch (x.kind) {
    case K_ZERO: return sign;
    case K_INF: return sign | ones << fracbits;
    case K_NAN: {
        uint64_t f = (x.m << 1) >> (64 - fracbits);
        f |= 1ull << (fracbits - 1); /* quiet */
        return sign | ones << fracbits | f;
    }
    }
    xnum r = x_round(x.sign, x.e, (u128)x.m << 64, fracbits + 1, 1 - bias, bias);
    if (r.kind == K_ZERO)
        return sign;
    if (r.kind == K_INF)
        return sign | ones << fracbits;
    if (r.e >= 1 - bias)
        return sign | (uint64_t)(r.e + bias) << fracbits | ((r.m >> (63 - fracbits)) & ((1ull << fracbits) - 1));
    int s = (1 - bias) - r.e;
    return sign | (r.m >> (63 - fracbits + s));
}

static xnum from_int(int64_t v)
{
    if (!v)
        return x_zero(0);
    uint64_t mag = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    int k = __builtin_clzll(mag);
    return (xnum){ v < 0, K_FIN, 63 - k, mag << k };
}

/* sig * 2^(e - 127) rounded to an integer in the current direction:
 * the magnitude, or false when it is 2^64 or more. */
static bool round_int(int sign, int32_t e, u128 sig, int mode, uint64_t *mag, bool *inexact)
{
    *inexact = false;
    if (!sig) {
        *mag = 0;
        return true;
    }
    int k = clz128(sig);
    sig <<= k;
    e -= k;
    if (e >= 64)
        return false;
    u128 ip;
    bool half, sticky;
    if (e >= 0) {
        int keep = e + 1;
        ip = sig >> (128 - keep);
        u128 rest = sig << keep;
        half = (bool)(rest >> 127);
        sticky = (rest << 1) != 0;
    } else {
        ip = 0;
        half = e == -1;
        sticky = e == -1 ? (sig << 1) != 0 : true;
    }
    *inexact = half || sticky;
    bool up;
    switch (mode) {
    case 0: up = half && (sticky || (ip & 1)); break;
    case 1: up = *inexact && !sign; break;
    case 2: up = *inexact && sign; break;
    default: up = false; break;
    }
    ip += up;
    if (ip >> 64)
        return false;
    *mag = (uint64_t)ip;
    return true;
}

/* x as an integer of the given width (16, 32 or 64 bits): out of range
 * or NaN gives the format's most negative value (comp's NaN) and signals
 * invalid. */
static int64_t to_int(xnum x, int bits)
{
    int64_t lo = bits == 64 ? INT64_MIN : -((int64_t)1 << (bits - 1));
    if (x.kind == K_ZERO)
        return 0;
    if (x.kind != K_FIN) {
        raised |= X_INVALID;
        return lo;
    }
    uint64_t mag;
    bool inexact;
    if (!round_int(x.sign, x.e, (u128)x.m << 64, rounding(), &mag, &inexact)) {
        raised |= X_INVALID;
        return lo;
    }
    /* comp's most negative value is its NaN: the range is symmetric. */
    uint64_t limit = bits == 64 ? (uint64_t)INT64_MAX : (uint64_t)1 << (bits - 1);
    if (mag > limit || (!x.sign && mag == limit && bits != 64)) {
        raised |= X_INVALID;
        return lo;
    }
    if (inexact)
        raised |= X_INEXACT;
    return x.sign ? (int64_t)(0 - mag) : (int64_t)mag;
}

/* An operand in any of the formats. */
static xnum load(int fmt, gaddr p)
{
    switch (fmt) {
    case F_DBL: return from_ieee((uint64_t)rd32(p) << 32 | rd32(p + 4), 52, 11);
    case F_SGL: return from_ieee(rd32(p), 23, 8);
    case F_INT: return from_int((int16_t)rd16(p));
    case F_LONG: return from_int((int32_t)rd32(p));
    case F_COMP: {
        int64_t v = (int64_t)((uint64_t)rd32(p) << 32 | rd32(p + 4));
        if (v == INT64_MIN) /* comp's NaN */
            return (xnum){ 0, K_NAN, 0, 0x4000000000000000ull | (uint64_t)NAN_COMP << 48 };
        return from_int(v);
    }
    case F_EXT96: /* 68881 layout: sign/exponent, a pad word, then the significand */
        return unpack_ext(rd16(p), (uint64_t)rd32(p + 4) << 32 | rd32(p + 8));
    default: return ext_load(p);
    }
}

static void store(int fmt, gaddr p, xnum x)
{
    switch (fmt) {
    case F_DBL: {
        uint64_t b = to_ieee(x, 52, 11);
        wr32(p, (uint32_t)(b >> 32));
        wr32(p + 4, (uint32_t)b);
        break;
    }
    case F_SGL: wr32(p, (uint32_t)to_ieee(x, 23, 8)); break;
    case F_INT: wr16(p, (uint16_t)to_int(x, 16)); break;
    case F_LONG: wr32(p, (uint32_t)to_int(x, 32)); break;
    case F_COMP: {
        uint64_t c = (uint64_t)to_int(x, 64);
        wr32(p, (uint32_t)(c >> 32));
        wr32(p + 4, (uint32_t)c);
        break;
    }
    case F_EXT96: {
        uint8_t b[10];
        gaddr t = p; /* the same fields, a pad word after the first */
        ext_store(t, x);
        gget(b, t, 10);
        gput(p + 4, b + 2, 8);
        wr16(p + 2, 0);
        break;
    }
    default: ext_store(p, x);
    }
}

/* Host doubles, for the elementary functions and the Toolbox's other users. */
static double x_to_double(xnum x)
{
    uint64_t b = to_ieee(x, 52, 11);
    double d;
    memcpy(&d, &b, 8);
    return d;
}

static xnum x_from_double(double d)
{
    uint64_t b;
    memcpy(&b, &d, 8);
    return from_ieee(b, 52, 11);
}

/* ---------------------------------------------------------------------- */
/* Arithmetic */

static xnum x_add(xnum a, xnum b)
{
    if (a.kind == K_NAN || b.kind == K_NAN)
        return x_nan2(a, b);
    if (a.kind == K_INF) {
        if (b.kind == K_INF && a.sign != b.sign)
            return x_nan(NAN_ADD);
        return a;
    }
    if (b.kind == K_INF)
        return b;
    if (a.kind == K_ZERO && b.kind == K_ZERO)
        return x_zero(a.sign == b.sign ? a.sign : rounding() == 2);
    if (a.kind == K_ZERO)
        return x_fit(b);
    if (b.kind == K_ZERO)
        return x_fit(a);
    if (a.e < b.e || (a.e == b.e && a.m < b.m)) {
        xnum t = a;
        a = b;
        b = t;
    }
    /* A headroom bit for the carry: value = A * 2^(e - 126). */
    u128 A = (u128)a.m << 63, B = (u128)b.m << 63;
    int32_t d = a.e - b.e;
    if (d >= 127)
        B = 1;
    else if (d > 0)
        B = (B >> d) | ((B & (((u128)1 << d) - 1)) != 0);
    u128 S = a.sign == b.sign ? A + B : A - B;
    if (!S)
        return x_zero(rounding() == 2);
    return x_round_env(a.sign, a.e + 1, S);
}

static xnum x_neg(xnum x)
{
    x.sign ^= 1;
    return x;
}

static xnum x_mul(xnum a, xnum b)
{
    int sign = a.sign ^ b.sign;
    if (a.kind == K_NAN || b.kind == K_NAN)
        return x_nan2(a, b);
    if ((a.kind == K_INF && b.kind == K_ZERO) || (a.kind == K_ZERO && b.kind == K_INF))
        return x_nan(NAN_MUL);
    if (a.kind == K_INF || b.kind == K_INF)
        return x_inf(sign);
    if (a.kind == K_ZERO || b.kind == K_ZERO)
        return x_zero(sign);
    return x_round_env(sign, a.e + b.e + 1, (u128)a.m * b.m);
}

static xnum x_div(xnum a, xnum b)
{
    int sign = a.sign ^ b.sign;
    if (a.kind == K_NAN || b.kind == K_NAN)
        return x_nan2(a, b);
    if ((a.kind == K_INF && b.kind == K_INF) || (a.kind == K_ZERO && b.kind == K_ZERO))
        return x_nan(NAN_DIV);
    if (a.kind == K_INF)
        return x_inf(sign);
    if (b.kind == K_INF || a.kind == K_ZERO)
        return x_zero(sign);
    if (b.kind == K_ZERO) {
        raised |= X_DIVBYZERO;
        return x_inf(sign);
    }
    /* 128 quotient bits and a sticky bit. */
    int32_t e;
    u128 num;
    if (a.m >= b.m) {
        num = (u128)a.m << 63;
        e = a.e - b.e;
    } else {
        num = (u128)a.m << 64;
        e = a.e - b.e - 1;
    }
    uint64_t q1 = (uint64_t)(num / b.m), r1 = (uint64_t)(num % b.m);
    u128 n2 = (u128)r1 << 64;
    uint64_t q2 = (uint64_t)(n2 / b.m), r2 = (uint64_t)(n2 % b.m);
    return x_round_env(sign, e, (u128)q1 << 64 | q2 | (r2 != 0));
}

static uint64_t isqrt128(u128 n)
{
    double est = sqrt((double)n);
    uint64_t r = est >= 18446744073709551615.0 ? ~0ull : (uint64_t)est;
    if (r) {
        u128 t = ((u128)r + n / r) >> 1;
        r = t >> 64 ? ~0ull : (uint64_t)t;
    }
    while ((u128)r * r > n)
        r--;
    while (r != ~0ull && (u128)(r + 1) * (r + 1) <= n)
        r++;
    return r;
}

static xnum x_sqrt(xnum x)
{
    if (x.kind == K_NAN)
        return x_quiet(x);
    if (x.kind == K_ZERO)
        return x;
    if (x.sign)
        return x_nan(NAN_SQRT);
    if (x.kind == K_INF)
        return x;
    int32_t t = x.e - 63;
    int s = ((t - 63) & 1) ? 64 : 63;
    u128 n = (u128)x.m << s;
    uint64_t r = isqrt128(n);
    u128 rem = n - (u128)r * r;
    /* Above or below the halfway point: (r + 1/2)^2 = r^2 + r + 1/4. */
    uint64_t lo = rem > r ? 0x8000000000000001ull : rem ? 1 : 0;
    return x_round_env(0, (t - s) / 2 + 63, (u128)r << 64 | lo);
}

/* Compare: -1, 0, 1, or 2 unordered. */
static int x_cmp(xnum a, xnum b)
{
    if (a.kind == K_NAN || b.kind == K_NAN)
        return 2;
    if (a.kind == K_ZERO && b.kind == K_ZERO)
        return 0;
    if (a.kind == K_ZERO)
        return b.sign ? 1 : -1;
    if (b.kind == K_ZERO)
        return a.sign ? -1 : 1;
    if (a.sign != b.sign)
        return a.sign ? -1 : 1;
    int mag;
    if (a.kind == K_INF || b.kind == K_INF)
        mag = a.kind == b.kind ? 0 : a.kind == K_INF ? 1 : -1;
    else if (a.e != b.e)
        mag = a.e > b.e ? 1 : -1;
    else
        mag = a.m == b.m ? 0 : a.m > b.m ? 1 : -1;
    return a.sign ? -mag : mag;
}

/* The IEEE remainder x - n*y, n the integer nearest x/y; *quo gets n's low
 * seven bits with n's sign. */
static xnum x_rem(xnum x, xnum y, int *quo)
{
    *quo = 0;
    if (x.kind == K_NAN || y.kind == K_NAN)
        return x_nan2(x, y);
    if (x.kind == K_INF || y.kind == K_ZERO)
        return x_nan(NAN_REM);
    if (x.kind == K_ZERO || y.kind == K_INF)
        return x_fit(x);
    int qsign = x.sign ^ y.sign;
    uint64_t q = 0, my = y.m;
    u128 r; /* the remainder's significand, at y's exponent */
    if (x.e < y.e - 1) {
        return x_fit(x);
    } else if (x.e == y.e - 1) {
        /* |x| / |y| in [1/4, 1): n is 1 only when |x| > |y| / 2, and then
         * the result is |y| - |x| with the sign turned over. */
        if (x.m > my) {
            *quo = qsign ? -1 : 1;
            return x_round_env(!x.sign, x.e, ((u128)my * 2 - x.m) << 64);
        }
        return x_fit(x);
    } else {
        u128 rr = x.m % my;
        q = x.m / my;
        int32_t d = x.e - y.e;
        while (d > 0) {
            int k = d > 63 ? 63 : d;
            u128 t = rr << k;
            q = (q << k) | (uint64_t)(t / my);
            rr = t % my;
            d -= k;
        }
        r = rr;
    }
    int rsign = x.sign;
    if (2 * r > (u128)my || (2 * r == (u128)my && (q & 1))) {
        r = my - r;
        q++;
        rsign ^= 1;
    }
    *quo = (int)(q & 0x7F);
    if (qsign)
        *quo = -*quo;
    if (!r)
        return x_zero(x.sign);
    /* r * 2^(re - 63) = (r << 64) * 2^(re - 127) */
    return x_round_env(rsign, y.e, r << 64);
}

/* Round to an integral value in the given direction (FRINT, FTINT). */
static xnum x_rint(xnum x, int mode)
{
    if (x.kind == K_NAN)
        return x_quiet(x);
    if (x.kind != K_FIN || x.e >= 63)
        return x;
    uint64_t mag;
    bool inexact;
    round_int(x.sign, x.e, (u128)x.m << 64, mode, &mag, &inexact);
    if (inexact)
        raised |= X_INEXACT;
    xnum r = from_int((int64_t)mag);
    if (mag >> 63) /* 2^63 exactly, after rounding up */
        r = (xnum){ 0, K_FIN, 63, mag };
    r.sign = x.sign;
    return r;
}

static xnum x_scalb(xnum x, int n)
{
    if (x.kind == K_NAN)
        return x_quiet(x);
    if (x.kind != K_FIN)
        return x;
    return x_round_env(x.sign, x.e + n, (u128)x.m << 64);
}

static xnum x_logb(xnum x)
{
    switch (x.kind) {
    case K_NAN: return x_quiet(x);
    case K_INF: return x_inf(0);
    case K_ZERO: raised |= X_DIVBYZERO; return x_inf(1);
    }
    return from_int(x.e);
}

/* The class of an operand, from its bits: 1 signaling NaN, 2 quiet NaN,
 * 3 infinite, 4 zero, 5 normal, 6 denormal; negative when the sign is. */
static int x_class(int fmt, gaddr p)
{
    xnum x = load(fmt, p);
    int c;
    switch (x.kind) {
    case K_NAN: c = x_snan(x) ? 1 : 2; break;
    case K_INF: c = 3; break;
    case K_ZERO: c = 4; break;
    default: {
        int32_t emin = fmt == F_DBL ? -1022 : fmt == F_SGL ? -126 : EXT_EMIN;
        c = fmt <= F_EXT96 && x.e < emin ? 6 : 5;
    }
    }
    if (fmt == F_COMP && x.kind == K_NAN)
        c = 2;
    return x.sign ? -c : c;
}

/* The next value after x toward y in x's format (single, double or
 * extended), in place of x's bits. */
static void x_next(int fmt, gaddr px, gaddr py)
{
    xnum x = load(fmt, px), y = load(fmt, py);
    if (x.kind == K_NAN || y.kind == K_NAN) {
        store(fmt, px, x_nan2(x, y));
        return;
    }
    int c = x_cmp(x, y);
    if (!c)
        return;
    bool away = (c < 0) != (bool)x.sign; /* the magnitude grows */
    if (x.kind == K_ZERO)
        away = true, x.sign = c > 0;
    if (fmt == F_DBL || fmt == F_SGL) {
        int bytes = fmt == F_DBL ? 8 : 4;
        uint64_t b = bytes == 8 ? (uint64_t)rd32(px) << 32 | rd32(px + 4) : rd32(px);
        uint64_t sbit = 1ull << (bytes * 8 - 1), mag = b & ~sbit;
        if (x.kind == K_ZERO)
            mag = 0, b = x.sign ? sbit : 0;
        mag = away ? mag + 1 : mag - 1;
        b = (b & sbit) | mag;
        uint64_t expmask = bytes == 8 ? 0x7FF0000000000000ull : 0x7F800000ull;
        if ((mag & expmask) == expmask)
            raised |= X_OVERFLOW | X_INEXACT;
        else if (!(mag & expmask))
            raised |= X_UNDERFLOW | X_INEXACT;
        if (bytes == 8) {
            wr32(px, (uint32_t)(b >> 32));
            wr32(px + 4, (uint32_t)b);
        } else {
            wr32(px, (uint32_t)b);
        }
        return;
    }
    /* Extended: step the normalized bits, the integer bit explicit. */
    ext_store(px, x);
    uint16_t se = rd16(px);
    uint64_t m = (uint64_t)rd32(px + 2) << 32 | rd32(px + 6);
    int ef = se & 0x7FFF;
    if (x.kind == K_ZERO) {
        ef = 0;
        m = 1;
    } else if (away) {
        if (++m == 0) {
            m = 0x8000000000000000ull;
            ef++;
        } else if (!ef && m == 0x8000000000000000ull) {
            ef = 1;
        }
    } else {
        if (ef > 1 && m == 0x8000000000000000ull) {
            m = ~0ull;
            ef--;
        } else if (ef == 1 && m == 0x8000000000000000ull) {
            m--;
            ef = 0;
        } else {
            m--;
        }
    }
    if (ef == 0x7FFF) {
        raised |= X_OVERFLOW | X_INEXACT;
        m = 0;
    } else if (!ef) {
        raised |= X_UNDERFLOW | X_INEXACT;
    }
    wr16(px, (uint16_t)((se & 0x8000) | ef));
    wr32(px + 2, (uint32_t)(m >> 32));
    wr32(px + 6, (uint32_t)m);
}

/* ---------------------------------------------------------------------- */
/* Decimal records: sgn (byte), exp (word at 2), sig (Str20 at 4).
 * A decform is style (0 float, 1 fixed) and digits (word at 2).
 *
 * Conversions between them and binary are exact before their one
 * rounding, done with integers as long as extended's range needs. */

enum { BIG_WORDS = 640 }; /* 20480 bits: 10^4951 times 2^16445 and some */

typedef struct {
    int n; /* words in use */
    uint32_t w[BIG_WORDS];
} big;

static void big_set(big *b, u128 v)
{
    b->n = 0;
    while (v) {
        b->w[b->n++] = (uint32_t)v;
        v >>= 32;
    }
}

static void big_mul_small(big *b, uint32_t k)
{
    uint64_t carry = 0;
    for (int i = 0; i < b->n; i++) {
        uint64_t t = (uint64_t)b->w[i] * k + carry;
        b->w[i] = (uint32_t)t;
        carry = t >> 32;
    }
    if (carry && b->n < BIG_WORDS)
        b->w[b->n++] = (uint32_t)carry;
}

static void big_pow5(big *b, int n)
{
    for (; n >= 13; n -= 13)
        big_mul_small(b, 1220703125u); /* 5^13 */
    static const uint32_t p5[13] = { 1, 5, 25, 125, 625, 3125, 15625, 78125, 390625, 1953125, 9765625,
                                     48828125, 244140625 };
    if (n)
        big_mul_small(b, p5[n]);
}

static void big_shl(big *b, int s)
{
    if (!b->n || s <= 0)
        return;
    int words = s / 32, bits = s % 32;
    int n = b->n + words + 1;
    if (n > BIG_WORDS)
        n = BIG_WORDS;
    for (int i = n - 1; i >= 0; i--) {
        int j = i - words;
        uint64_t v = 0;
        if (j >= 0 && j < b->n)
            v = (uint64_t)b->w[j] << bits;
        if (bits && j - 1 >= 0 && j - 1 < b->n)
            v |= b->w[j - 1] >> (32 - bits);
        b->w[i] = (uint32_t)v;
    }
    b->n = n;
    while (b->n && !b->w[b->n - 1])
        b->n--;
}

static int big_bits(const big *b)
{
    return b->n ? (b->n - 1) * 32 + 32 - __builtin_clz(b->w[b->n - 1]) : 0;
}

static int big_cmp(const big *a, const big *b)
{
    if (a->n != b->n)
        return a->n > b->n ? 1 : -1;
    for (int i = a->n - 1; i >= 0; i--)
        if (a->w[i] != b->w[i])
            return a->w[i] > b->w[i] ? 1 : -1;
    return 0;
}

static void big_sub(big *a, const big *b) /* a -= b, a >= b */
{
    int64_t borrow = 0;
    for (int i = 0; i < a->n; i++) {
        int64_t t = (int64_t)a->w[i] - (i < b->n ? b->w[i] : 0) - borrow;
        borrow = t < 0;
        a->w[i] = (uint32_t)t;
    }
    while (a->n && !a->w[a->n - 1])
        a->n--;
}

/* num / den for a quotient below 2^bits (bits <= 127); num keeps the
 * remainder. */
static u128 big_div(big *num, const big *den, int bits)
{
    static big d;
    d = *den;
    big_shl(&d, bits - 1);
    u128 q = 0;
    for (int i = bits - 1; i >= 0; i--) {
        if (big_cmp(num, &d) >= 0) {
            big_sub(num, &d);
            q |= (u128)1 << i;
        }
        /* d >>= 1 */
        for (int j = 0; j < d.n; j++)
            d.w[j] = d.w[j] >> 1 | (j + 1 < d.n ? d.w[j + 1] << 31 : 0);
        while (d.n && !d.w[d.n - 1])
            d.n--;
    }
    return q;
}

/* The top 128 bits of b and whether any below them are set. */
static u128 big_top(const big *b, int *shift)
{
    int bits = big_bits(b);
    *shift = bits > 128 ? bits - 128 : 0;
    u128 v = 0;
    bool sticky = false;
    for (int i = b->n - 1; i >= 0; i--) {
        for (int k = 31; k >= 0; k--) {
            int pos = i * 32 + k;
            if (pos >= *shift) {
                if ((b->w[i] >> k) & 1)
                    v |= (u128)1 << (pos - *shift);
            } else if ((b->w[i] >> k) & 1) {
                sticky = true;
            }
        }
    }
    return v | sticky;
}

/* A decimal record's value, unrounded: sig * 2^(e - 127).  Returns the
 * kind (K_NAN with *nan the significand). */
static int dec_value(gaddr d, int *sign, int32_t *e, u128 *sig, uint64_t *nan)
{
    *sign = rd8(d) & 1;
    int exp = (int16_t)rd16(d + 2), n = rd8(d + 4);
    char s[32] = { 0 };
    if (n > 20) /* SIGDIGLEN */
        n = 20;
    gget(s, d + 5, (uint32_t)n);
    if (n && s[0] == 'N') {
        uint64_t f = 0;
        int k = 0;
        for (int i = 1; i < n && k < 16; i++, k++) {
            int c = s[i], v = c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : 0;
            f |= (uint64_t)v << (60 - 4 * k);
        }
        f &= 0x7FFFFFFFFFFFFFFFull;
        if (!(f & 0x00FF000000000000ull) && !(f & 0x3FFFFFFFFFFFFFFFull))
            f |= (uint64_t)NAN_ZERO << 48; /* a NaN's code may not be zero */
        *nan = f | 0x4000000000000000ull;
        return K_NAN;
    }
    if (n && s[0] == 'I')
        return K_INF;
    u128 v = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            break;
        v = v * 10 + (uint32_t)(s[i] - '0');
    }
    if (!v)
        return K_ZERO;
    /* Far out of range: a value that overflows, or underflows to nothing. */
    if (exp > 5000) {
        *e = 20000;
        *sig = v;
        return K_FIN;
    }
    if (exp < -5100) {
        *e = -20000;
        *sig = v;
        return K_FIN;
    }
    static big num, den;
    big_set(&num, v);
    if (exp >= 0) {
        big_pow5(&num, exp);
        int shift;
        *sig = big_top(&num, &shift);
        *e = 127 + shift + exp; /* the 2^exp of 10^exp */
    } else {
        big_set(&den, 1);
        big_pow5(&den, -exp);
        int k = big_bits(&den) - big_bits(&num) + 68;
        if (k < 0)
            k = 0;
        big_shl(&num, k);
        u128 q = big_div(&num, &den, 72);
        *sig = q | (num.n != 0);
        *e = 127 - k + exp;
    }
    return K_FIN;
}

/* Decimal to binary, rounded once into the destination's format. */
static void dec_to_bin(int fmt, gaddr d, gaddr dst)
{
    int sign;
    int32_t e = 0;
    u128 sig = 0;
    uint64_t nan = 0;
    int kind = dec_value(d, &sign, &e, &sig, &nan);
    xnum x;
    switch (kind) {
    case K_NAN: x = (xnum){ sign, K_NAN, 0, nan }; store(fmt, dst, x); return;
    case K_INF: store(fmt, dst, x_inf(sign)); return;
    case K_ZERO: store(fmt, dst, x_zero(sign)); return;
    }
    switch (fmt) {
    case F_DBL: x = x_round(sign, e, sig, 53, -1022, 1023); break;
    case F_SGL: x = x_round(sign, e, sig, 24, -126, 127); break;
    case F_INT: case F_LONG: case F_COMP: {
        int bits = fmt == F_INT ? 16 : fmt == F_LONG ? 32 : 64;
        uint64_t mag;
        bool inexact;
        int64_t lo = bits == 64 ? INT64_MIN : -((int64_t)1 << (bits - 1));
        uint64_t limit = bits == 64 ? (uint64_t)INT64_MAX : (uint64_t)1 << (bits - 1);
        int64_t r;
        if (!round_int(sign, e, sig, rounding(), &mag, &inexact) || mag > limit ||
            (!sign && mag == limit && bits != 64)) {
            raised |= X_INVALID;
            r = lo;
        } else {
            if (inexact)
                raised |= X_INEXACT;
            r = sign ? (int64_t)(0 - mag) : (int64_t)mag;
        }
        if (bits == 16)
            wr16(dst, (uint16_t)r);
        else if (bits == 32)
            wr32(dst, (uint32_t)r);
        else {
            wr32(dst, (uint32_t)((uint64_t)r >> 32));
            wr32(dst + 4, (uint32_t)r);
        }
        return;
    }
    default: x = x_round_env(sign, e, sig); break;
    }
    store(fmt, dst, x);
}

/* The style is a byte, as Pascal lays out its enumerated type ($0100 in the
 * word); a style written as a whole word (1) counts too. */
static int decform_style(gaddr form)
{
    return rd8(form) || rd16(form) == 1;
}

/* round(|x| / 10^p) in the current direction, below 10^21. */
static uint64_t dec_scaled(xnum x, int p, bool *inexact)
{
    static big num, den;
    /* |x| = m * 2^(e - 63) = m * 2^t */
    int t = x.e - 63;
    big_set(&num, x.m);
    big_set(&den, 1);
    if (p >= 0)
        big_pow5(&den, p);
    else
        big_pow5(&num, -p);
    /* the powers of two: 2^t / 2^p */
    if (t - p >= 0)
        big_shl(&num, t - p);
    else
        big_shl(&den, p - t);
    u128 q = big_div(&num, &den, 72);
    /* the remainder against half the divisor */
    bool sticky = num.n != 0;
    int half = 0; /* -1 below, 0 exactly, 1 above */
    if (sticky) {
        big_shl(&num, 1);
        int c = big_cmp(&num, &den);
        half = c;
    }
    *inexact = sticky;
    bool up = false;
    switch (rounding()) {
    case 0: up = sticky && (half > 0 || (half == 0 && (q & 1))); break;
    case 1: up = sticky && !x.sign; break;
    case 2: up = sticky && x.sign; break;
    }
    return (uint64_t)(q + up);
}

static const uint64_t pow10_u64[20] = {
    1ull, 10ull, 100ull, 1000ull, 10000ull, 100000ull, 1000000ull, 10000000ull, 100000000ull,
    1000000000ull, 10000000000ull, 100000000000ull, 1000000000000ull, 10000000000000ull,
    100000000000000ull, 1000000000000000ull, 10000000000000000ull, 100000000000000000ull,
    1000000000000000000ull, 10000000000000000000ull,
};

/* Binary to decimal: up to 19 significant digits, rounded once. */
static void bin_to_dec(xnum x, gaddr form, gaddr d)
{
    int style = decform_style(form), digits = (int16_t)rd16(form + 2);
    char sig[40];
    int exp = 0;
    wr8(d, (uint8_t)x.sign);
    wr8(d + 1, 0);
    switch (x.kind) {
    case K_NAN:
        snprintf(sig, sizeof sig, "N%016llX", (unsigned long long)(x.m & 0x7FFFFFFFFFFFFFFFull));
        if (x_snan(x))
            raised |= X_INVALID;
        break;
    case K_INF: strcpy(sig, "I"); break;
    case K_ZERO: strcpy(sig, "0"); break;
    default: {
        /* The decimal exponent of x's leading digit, nearly. */
        double lg = (x.e + log2((double)x.m / 18446744073709551616.0) + 1) * 0.30102999566398119521;
        int lead = (int)floor(lg);
        uint64_t q;
        bool inexact;
        if (style == 1) {
            int p = -digits;
            if (lead - p + 1 > 19) {
                strcpy(sig, "?");
                break;
            }
            q = dec_scaled(x, p, &inexact);
            if (q >= pow10_u64[19]) {
                strcpy(sig, "?");
                break;
            }
            exp = p;
        } else {
            int n = digits < 1 ? 1 : digits > 19 ? 19 : digits;
            int p = lead - n + 1;
            q = dec_scaled(x, p, &inexact);
            if (q >= pow10_u64[n]) { /* the estimate was one low, or rounding carried */
                p++;
                q = dec_scaled(x, p, &inexact);
            } else if (q < pow10_u64[n - 1]) {
                p--;
                q = dec_scaled(x, p, &inexact);
            }
            if (q >= pow10_u64[n]) { /* 99..9 rounded up to 10..0 */
                q /= 10;
                p++;
            }
            exp = p;
        }
        if (inexact)
            raised |= X_INEXACT;
        snprintf(sig, sizeof sig, "%llu", (unsigned long long)q);
        if (!q)
            exp = 0;
    }
    }
    size_t n = strlen(sig);
    wr16(d + 2, (uint16_t)(int16_t)exp);
    wr8(d + 4, (uint8_t)n);
    gput(d + 5, sig, (uint32_t)n);
}

/* ---------------------------------------------------------------------- */
/* The environment and halts */

/* Signal the exceptions raised: their flags go into the environment, and
 * those with halts enabled call the halt vector (FPState + 2) as Pascal's
 * PROCEDURE Halt(VAR misc: MiscRec; src2, src, dst: Ptr; opcode: INTEGER),
 * after the result is delivered.  The MiscRec holds the halting
 * exceptions, then the CCR and D0 the operation leaves, which the halt
 * procedure may change. */
static void signal_raised(cpu68k *c, uint16_t op, gaddr src2, gaddr src, gaddr dst)
{
    uint16_t e = env_get();
    env_set((uint16_t)(e | (raised & 0x1F) << 8));
    uint16_t halts = raised & e & 0x1F;
    gaddr vector = rd32(LM_FPSTATE + 2);
    if (!halts || !vector)
        return;
    c->a[7] -= 8;
    gaddr misc = c->a[7];
    wr16(misc, halts);
    wr16(misc + 2, (uint16_t)(c->sr & 0x1F));
    wr32(misc + 4, c->d[0]);
    uint32_t args[5] = { misc, src2, src, dst, op };
    /* pascal (ptr, ptr, ptr, ptr, short) */
    mm_call(vector, 0x0000BFC0, args, 5);
    c->sr = (uint16_t)((c->sr & 0xFFE0) | (rd16(misc + 2) & 0x1F));
    c->d[0] = rd32(misc + 4);
    c->a[7] += 8;
}

/* ---------------------------------------------------------------------- */
/* FP68K */

static void set_ccr(cpu68k *c, int cmp)
{
    uint16_t ccr;
    switch (cmp) {
    case -1: ccr = SR_X | SR_N | SR_C; break;
    case 0: ccr = SR_Z; break;
    case 1: ccr = 0; break;
    default: ccr = SR_V; break;
    }
    c->sr = (uint16_t)((c->sr & 0xFFE0) | ccr);
}

/* Operands by operation (low five bits): how many addresses it pops. */
static int fp68k_operands(int code)
{
    switch (code) {
    case 0x0B: /* binary to decimal: decform, source, destination */
        return 3;
    case 0x00: case 0x02: case 0x04: case 0x06: case 0x08: case 0x0A: case 0x0C: case 0x0E:
    case 0x10: case 0x18: case 0x1C: case 0x09: case 0x11: case 0x13:
        return 2;
    default:
        return 1;
    }
}

void raw_FP68K(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    uint16_t op = cpu_pop16(c);
    int fmt = (op >> 11) & 7, code = op & 0xFF;
    if (code > 0x1C || fmt == 7) {
        cpu_halt(c, "FP68K: unknown operation %04X", op);
        return;
    }
    int nargs = fp68k_operands(code);
    gaddr dst = cpu_pop32(c), src = 0, src2 = 0;
    if (nargs > 1)
        src = cpu_pop32(c);
    if (nargs > 2)
        src2 = cpu_pop32(c);
    raised = 0;
    bool signal = true;
    switch (code) {
    case 0x00: ext_store(dst, x_add(ext_load(dst), load(fmt, src))); break;
    case 0x02: ext_store(dst, x_add(ext_load(dst), x_neg(load(fmt, src)))); break;
    case 0x04: ext_store(dst, x_mul(ext_load(dst), load(fmt, src))); break;
    case 0x06: ext_store(dst, x_div(ext_load(dst), load(fmt, src))); break;
    case 0x08: case 0x0A: { /* compare; FCPX signals invalid when unordered */
        xnum a = ext_load(dst), b = load(fmt, src);
        int r = x_cmp(a, b);
        if (x_snan(a) || x_snan(b) || (r == 2 && code == 0x0A))
            raised |= X_INVALID;
        set_ccr(c, r);
        break;
    }
    case 0x0C: { /* remainder; D0.W gets n's low seven bits, signed */
        int q;
        ext_store(dst, x_rem(ext_load(dst), load(fmt, src), &q));
        c->d[0] = (c->d[0] & 0xFFFF0000u) | (uint16_t)(int16_t)q;
        break;
    }
    case 0x0E: { /* to extended: exact, but a signaling NaN is quieted */
        xnum x = load(fmt, src);
        ext_store(dst, x.kind == K_NAN ? x_quiet(x) : x);
        break;
    }
    case 0x10: { /* from extended */
        xnum x = ext_load(src);
        if (x.kind == K_NAN && fmt != F_INT && fmt != F_LONG && fmt != F_COMP)
            x = x_quiet(x);
        store(fmt, dst, x);
        break;
    }
    case 0x12: ext_store(dst, x_sqrt(ext_load(dst))); break;
    case 0x14: ext_store(dst, x_rint(ext_load(dst), rounding())); break;
    case 0x16: ext_store(dst, x_rint(ext_load(dst), 3)); break;
    case 0x18: ext_store(dst, x_scalb(ext_load(dst), (int16_t)rd16(src))); break;
    case 0x1A: ext_store(dst, x_logb(ext_load(dst))); break;
    case 0x1C: wr16(dst, (uint16_t)(int16_t)x_class(fmt, src)); break;
    /* Negate, absolute value and copy-sign touch only the sign bit, so
     * any floating format will do; no exception, not even for a NaN. */
    case 0x0D: wr8(dst, rd8(dst) ^ 0x80); break;
    case 0x0F: wr8(dst, rd8(dst) & 0x7F); break;
    case 0x11: wr8(src, (uint8_t)((rd8(src) & 0x7F) | (rd8(dst) & 0x80))); break; /* SRC gets DST's sign */
    case 0x13: x_next(fmt, src, dst); break; /* SRC steps toward DST */
    case 0x09: dec_to_bin(fmt, src, dst); break;
    case 0x0B: bin_to_dec(load(fmt, src), src2, dst); break;
    case 0x01: env_set(rd16(dst)); signal = false; break; /* no halts from setting it */
    case 0x03: wr16(dst, env_get()); signal = false; break;
    case 0x05: wr32(LM_FPSTATE + 2, rd32(dst)); signal = false; break;
    case 0x07: wr32(dst, rd32(LM_FPSTATE + 2)); signal = false; break;
    case 0x15: { /* set exception, by its index in the word */
        int n = rd8(dst + 1);
        raised = n < 5 ? (uint16_t)(1 << n) : 0;
        break;
    }
    case 0x17: wr16(dst, env_get()); env_set(0); signal = false; break; /* procedure entry */
    case 0x19: { /* procedure exit: the saved environment, then the exceptions since */
        raised = (env_get() >> 8) & 0x1F;
        env_set(rd16(dst));
        break;
    }
    case 0x1B: { /* test exception: the word's low byte is its index; the answer goes in the high byte */
        int n = rd8(dst + 1);
        wr8(dst, n < 5 && (env_get() >> (8 + n)) & 1 ? 1 : 0);
        signal = false;
        break;
    }
    default:
        cpu_halt(c, "FP68K: unknown operation %04X", op);
        return;
    }
    if (signal)
        signal_raised(c, op, src2, src, dst);
    cpu_push32(c, ret);
}

/* ---------------------------------------------------------------------- */
/* Elems68K */

static const double LN2 = 0.69314718055994530942;
/* ln 2 in two parts, the first with its low bits clear (fdlibm's). */
static const double LN2_HI = 6.93147180369123816490e-01, LN2_LO = 1.90821492927058770002e-10;

/* d * 2^k, rounded once into extended (k may take it far past a double's range). */
static xnum scaled(double d, int32_t k)
{
    xnum x = x_from_double(d);
    if (x.kind != K_FIN)
        return x;
    return x_round_env(x.sign, x.e + k, (u128)x.m << 64);
}

/* Results too large or too small for any format: overflow or underflow. */
static xnum huge(int sign) { return x_round_env(sign, 20000, 1); }
static xnum tiny(int sign) { return x_round_env(sign, -20000, 1); }

/* 2^z for any double z: overflows and underflows as extended does. */
static xnum exp2_any(double z, int sign)
{
    if (z > 16400)
        return huge(sign);
    if (z < -16500)
        return tiny(sign);
    double k = nearbyint(z);
    xnum r = scaled(exp2(z - k), (int32_t)k);
    r.sign = sign;
    return r;
}

/* e^d for any double d. */
static xnum exp_any(double d)
{
    if (d > 11400)
        return huge(0);
    if (d < -11450)
        return tiny(0);
    double k = nearbyint(d / LN2);
    double r = (d - k * LN2_HI) - k * LN2_LO;
    return scaled(exp(r), (int32_t)k);
}

static xnum x_one(void);

/* ln and log2 of a finite x > 0, over extended's whole range; near 1 from
 * x - 1, which is exact there, so that nothing cancels. */
static double ln_x(xnum x)
{
    if (x.e == 0 || x.e == -1)
        return log1p(x_to_double(x_add(x, x_neg(x_one()))));
    return log((double)x.m / 9223372036854775808.0) + x.e * LN2;
}

static double log2_x(xnum x)
{
    if (x.e == 0 || x.e == -1)
        return ln_x(x) / LN2;
    return log2((double)x.m / 9223372036854775808.0) + x.e;
}

/* x as a double, or false when it is beyond a double's range or below its
 * smallest normal (the callers have answers of their own for those). */
static bool in_double(xnum x, double *d)
{
    if (x.kind == K_FIN && (x.e > 1023 || x.e < -1022))
        return false;
    *d = x_to_double(x);
    return true;
}

static bool x_integral(xnum x)
{
    if (x.kind == K_ZERO)
        return true;
    if (x.kind != K_FIN || x.e < 0)
        return false;
    return x.e >= 63 || !(x.m << (x.e + 1));
}

static bool x_odd(xnum x) /* an odd integer */
{
    return x_integral(x) && x.kind == K_FIN && x.e <= 63 && ((x.m >> (63 - x.e)) & 1);
}

static xnum x_one(void) { return from_int(1); }

/* x^n for an integer n, by squaring. */
static xnum x_pwri(xnum x, int n)
{
    if (!n)
        return x_one();
    if (x.kind == K_NAN)
        return x_quiet(x);
    if (x.kind == K_ZERO && n < 0) {
        raised |= X_DIVBYZERO;
        return x_inf(x.sign && (n & 1));
    }
    unsigned k = n < 0 ? (unsigned)-n : (unsigned)n;
    xnum r = x_one(), b = x;
    for (;;) {
        if (k & 1)
            r = x_mul(r, b);
        k >>= 1;
        if (!k)
            break;
        b = x_mul(b, b);
    }
    return n < 0 ? x_div(x_one(), r) : r;
}

static xnum x_pwry(xnum x, xnum y)
{
    if (y.kind == K_ZERO)
        return x_one();
    if (x.kind == K_NAN || y.kind == K_NAN)
        return x_nan2(x, y);
    if (x_integral(y) && y.kind == K_FIN && y.e < 15)
        return x_pwri(x, (int)to_int(y, 32));
    int sign = x.sign && x_odd(y);
    if (x.sign && x.kind != K_ZERO && !x_integral(y))
        return x_nan(NAN_POWER);
    if (x.kind == K_ZERO) {
        if (y.sign) {
            raised |= X_DIVBYZERO;
            return x_inf(sign);
        }
        return x_zero(sign);
    }
    if (x.kind == K_INF)
        return y.sign ? x_zero(sign) : x_inf(sign);
    x.sign = 0;
    int c1 = x_cmp(x, x_one());
    if (y.kind == K_INF) {
        if (!c1)
            return x_nan(NAN_POWER);
        return (c1 > 0) != (bool)y.sign ? x_inf(0) : x_zero(0);
    }
    double yd;
    if (!in_double(y, &yd))
        yd = y.e > 0 ? (y.sign ? -HUGE_VAL : HUGE_VAL) : 0;
    double l = log2_x(x);
    return exp2_any(l == 0 ? 0 : yd * l, sign);
}

/* compound(r, n) = (1 + r)^n */
static xnum x_compound(xnum r, xnum n)
{
    if (r.kind == K_NAN || n.kind == K_NAN)
        return x_nan2(r, n);
    if (n.kind == K_ZERO)
        return x_one();
    xnum m1 = x_neg(x_one());
    int c = x_cmp(r, m1);
    if (c < 0)
        return x_nan(NAN_FINANCE);
    if (c == 0) {
        if (n.sign) {
            raised |= X_DIVBYZERO;
            return x_inf(0);
        }
        return x_zero(0);
    }
    if (r.kind == K_ZERO)
        return x_one();
    if (r.kind == K_INF)
        return n.sign ? x_zero(0) : x_inf(0);
    double rd, nd, l;
    l = in_double(r, &rd) ? log1p(rd) / LN2 : (r.e > 0 ? log2_x(r) : x_to_double(r) / LN2);
    if (!in_double(n, &nd))
        nd = n.e > 0 ? (n.sign ? -HUGE_VAL : HUGE_VAL) : 0;
    if (isinf(nd))
        return (l > 0) != (nd < 0) ? x_inf(0) : x_zero(0);
    return exp2_any(nd * l, 0);
}

/* annuity(r, n) = (1 - (1 + r)^-n) / r */
static xnum x_annuity(xnum r, xnum n)
{
    if (r.kind == K_NAN || n.kind == K_NAN)
        return x_nan2(r, n);
    if (r.kind == K_ZERO)
        return n;
    xnum m1 = x_neg(x_one());
    int c = x_cmp(r, m1);
    if (c < 0)
        return x_nan(NAN_FINANCE);
    if (n.kind == K_ZERO)
        return x_zero(0);
    if (c == 0) { /* (1 - 0^-n) / -1 */
        if (!n.sign) {
            raised |= X_DIVBYZERO;
            return x_inf(0);
        }
        return m1;
    }
    if (r.kind == K_INF)
        return n.sign ? x_inf(1) : x_zero(0);
    double rd, nd;
    if (!in_double(r, &rd))
        rd = x_to_double(r);
    if (!in_double(n, &nd))
        nd = n.e > 0 ? (n.sign ? -HUGE_VAL : HUGE_VAL) : 0;
    double t = -nd * (r.e > 1023 ? ln_x(r) : log1p(rd)); /* ln (1 + r)^-n */
    if (isnan(t))
        return x_nan(NAN_FINANCE);
    if (t > 700) /* (1 + r)^-n dwarfs the 1: -(1 + r)^-n / r */
        return exp2_any(t / LN2 - log2_x((xnum){ 0, K_FIN, r.e, r.m }), !r.sign);
    if (rd == 0)
        return x_div(n, r);
    return x_from_double(-expm1(t) / rd);
}

/* pi/2 in extended, the one SANE reduces trigonometric arguments by. */
static xnum half_pi(void) { return (xnum){ 0, K_FIN, 0, 0xC90FDAA22168C235ull }; }

static xnum x_trig(int code, xnum x)
{
    if (x.kind == K_NAN)
        return x_quiet(x);
    if (x.kind == K_INF)
        return code == 0x1E ? x_from_double(x.sign ? -M_PI_2 : M_PI_2) : x_nan(NAN_TRIG);
    if (x.kind == K_ZERO)
        return code == 0x1A ? x_one() : x;
    if (x.e < -1000) { /* sin, tan and atan of a tiny x are x */
        raised |= X_INEXACT;
        return code == 0x1A ? x_one() : x;
    }
    if (code == 0x1E) {
        double d;
        if (!in_double(x, &d))
            d = x.sign ? -HUGE_VAL : HUGE_VAL;
        return x_from_double(atan(d));
    }
    /* x = n pi/2 + r exactly, |r| <= pi/4; n's last two bits pick the quadrant. */
    int q;
    xnum r = x_rem(x, half_pi(), &q);
    int quadrant = ((q % 4) + 4) % 4;
    double d = x_to_double(r), s = sin(d), c = cos(d);
    switch (code) {
    case 0x18: return x_from_double(quadrant == 0 ? s : quadrant == 1 ? c : quadrant == 2 ? -s : -c);
    case 0x1A: return x_from_double(quadrant == 0 ? c : quadrant == 1 ? -s : quadrant == 2 ? -c : s);
    default: return x_from_double(quadrant & 1 ? -1 / tan(d) : tan(d));
    }
}

static xnum x_elem(int code, xnum x, gaddr a2, gaddr a3)
{
    double d;
    switch (code) {
    case 0x00: case 0x02: case 0x04: case 0x06: { /* ln, log2, ln(1+x), log2(1+x) */
        bool plus1 = code & 4, base2 = code & 2;
        if (x.kind == K_NAN)
            return x_quiet(x);
        xnum lim = plus1 ? x_neg(x_one()) : x_zero(0);
        int c = x_cmp(x, lim);
        if (c < 0 && !(x.kind == K_ZERO && !plus1))
            return x_nan(NAN_LOG);
        if (c == 0 || (x.kind == K_ZERO && !plus1)) {
            raised |= X_DIVBYZERO;
            return x_inf(1);
        }
        if (x.kind == K_INF)
            return x;
        if (plus1) {
            if (x.e < -1000) { /* ln(1 + x) is x */
                raised |= X_INEXACT;
                return base2 ? x_mul(x, x_from_double(1 / LN2)) : x_fit(x);
            }
            if (x.e > 1000) /* 1 + x is x */
                return x_from_double(base2 ? log2_x(x) : ln_x(x));
            d = x_to_double(x);
            return x_from_double(base2 ? log1p(d) / LN2 : log1p(d));
        }
        return x_from_double(base2 ? log2_x(x) : ln_x(x));
    }
    case 0x08: case 0x0A: case 0x0C: case 0x0E: { /* e^x, 2^x, e^x - 1, 2^x - 1 */
        bool minus1 = code & 4, base2 = code & 2;
        if (x.kind == K_NAN)
            return x_quiet(x);
        if (x.kind == K_INF)
            return x.sign ? (minus1 ? x_neg(x_one()) : x_zero(0)) : x;
        if (x.kind == K_ZERO)
            return minus1 ? x : x_one();
        if (!in_double(x, &d)) {
            if (x.e < 0) { /* tiny: 1, or x (times ln 2) */
                raised |= X_INEXACT;
                return minus1 ? (base2 ? x_mul(x, x_from_double(LN2)) : x_fit(x)) : x_one();
            }
            if (!x.sign)
                return huge(0);
            if (minus1) {
                raised |= X_INEXACT;
                return x_neg(x_one());
            }
            return tiny(0);
        }
        if (minus1 && d < 700 && d > -1000)
            return x_from_double(base2 ? (fabs(d) < 1 ? expm1(d * LN2) : exp2(d) - 1) : expm1(d));
        if (minus1 && d <= -1000) {
            raised |= X_INEXACT;
            return x_neg(x_one());
        }
        return base2 ? exp2_any(d, 0) : exp_any(d);
    }
    case 0x10: return x_pwri(x, (int16_t)rd16(a2));
    case 0x12: return x_pwry(x, ext_load(a2));
    case 0x14: return x_compound(ext_load(a3), ext_load(a2));
    case 0x16: return x_annuity(ext_load(a3), ext_load(a2));
    case 0x18: case 0x1A: case 0x1C: case 0x1E: return x_trig(code, x);
    case 0x20: /* random: x = 7^5 * x mod (2^31 - 1) */
        if (x.kind == K_NAN)
            return x_quiet(x);
        if (!in_double(x, &d))
            d = 0;
        return x_from_double(fmod(16807.0 * fabs(d), 2147483647.0));
    }
    return x;
}

void raw_Elems68K(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    uint16_t op = cpu_pop16(c);
    int code = op & 0xFF;
    if (code > 0x20 || (code & 1)) {
        cpu_halt(c, "Elems68K: unknown operation %04X", op);
        return;
    }
    /* The operation word's top bits count the operands: two for the
     * powers, three for compound and annuity. */
    int nargs = op & 0x8000 ? (op & 0x4000 ? 3 : 2) : 1;
    gaddr dst = cpu_pop32(c), a2 = nargs > 1 ? cpu_pop32(c) : 0, a3 = nargs > 2 ? cpu_pop32(c) : 0;
    raised = 0;
    feclearexcept(FE_ALL_EXCEPT);
    xnum r = x_elem(code, ext_load(dst), a2, a3);
    if (fetestexcept(FE_INEXACT))
        raised |= X_INEXACT;
    fesetround(FE_TONEAREST);
    ext_store(dst, r);
    signal_raised(c, op, a3, a2, dst);
    cpu_push32(c, ret);
}

void sane_reset(void) { wr16(LM_FPSTATE, 0), wr32(LM_FPSTATE + 2, 0); }

/* Host doubles to and from extended and decimal records, for the rest of
 * the Toolbox (Pack7, MathLib), in the current rounding direction. */
static double ext_get(gaddr p) { return x_to_double(ext_load(p)); }
static void ext_put(gaddr p, double v) { ext_store(p, x_from_double(v)); }

static double dec_get(gaddr d)
{
    int sign;
    int32_t e = 0;
    u128 sig = 0;
    uint64_t nan = 0;
    switch (dec_value(d, &sign, &e, &sig, &nan)) {
    case K_NAN: return x_to_double((xnum){ sign, K_NAN, 0, nan });
    case K_INF: return sign ? -HUGE_VAL : HUGE_VAL;
    case K_ZERO: return sign ? -0.0 : 0.0;
    }
    return x_to_double(x_round(sign, e, sig, 53, -1022, 1023));
}

static void dec_put(gaddr d, gaddr form, double v) { bin_to_dec(x_from_double(v), form, d); }

/* ---------------------------------------------------------------------- */
/* Toolbox Utilities' conversions to and from extended (Fix2X and friends). */

/* FUNCTION Fix2X(x: Fixed): Extended: the caller pushed the result's
 * address before x; it stays on the stack as the function result. */
static void to_extended(cpu68k *c, double scale)
{
    uint32_t ret = cpu_pop32(c);
    int32_t v = (int32_t)cpu_pop32(c);
    gaddr result = cpu_pop32(c);
    ext_put(result, v / scale);
    cpu_push32(c, result);
    cpu_push32(c, ret);
}

void raw_Fix2X(cpu68k *c, uint16_t tw) { to_extended(c, 65536.0); }
void raw_Frac2X(cpu68k *c, uint16_t tw) { to_extended(c, 1073741824.0); }

static int32_t from_extended(gaddr x, double scale)
{
    double v = ext_get(x) * scale;
    if (isnan(v))
        return INT32_MIN;
    if (v >= 2147483647.0)
        return INT32_MAX;
    if (v <= -2147483648.0)
        return INT32_MIN;
    return (int32_t)lround(v);
}

int32_t tb_R_X2Fix(gaddr x) { return from_extended(x, 65536.0); }
int32_t tb_R_X2Frac(gaddr x) { return from_extended(x, 1073741824.0); }

/* Microseconds (A193): a 64-bit count, high half in A0 and low in D0.
 * It is the machine's clock, as Ticks is: a headless run's follows the
 * instructions, and Director 5 paces its frames by Microseconds while
 * Lingo's timer counts Ticks, so a host clock here ran Squark's splash
 * at a different speed from its scripts. */
void raw_Microseconds(cpu68k *c, uint16_t tw)
{
    uint64_t us = machine_time_ns() / 1000u;
    c->a[0] = (uint32_t)(us >> 32);
    c->d[0] = (uint32_t)us;
}

/* GetVideoDefault (A080): A0 the DefVideoRec, as SetVideoDefault left it
 * (power.c). */
void raw_GetVideoDefault(cpu68k *c, uint16_t tw)
{
    if (c->a[0])
        startmgr_get_video(c->a[0]);
    c->d[0] = 0;
}

/* ---------------------------------------------------------------------- */
/* Pack7's decimal string conversions */

/* Dec2Str: a decimal record as text, in the decform's style. */
void tb_ROMlib_Fdec2str(gaddr form, gaddr dec, gaddr out)
{
    double v = dec_get(dec);
    int style = decform_style(form), digits = (int16_t)rd16(form + 2);
    char buf[128];
    if (isnan(v))
        snprintf(buf, sizeof buf, "NAN(017)");
    else if (isinf(v))
        snprintf(buf, sizeof buf, v < 0 ? "-INF" : "INF");
    else if (style == 1) { /* fixed */
        if (digits < 0)
            digits = 0;
        if (digits > 60)
            digits = 60;
        snprintf(buf, sizeof buf, "%.*f", digits, v);
    } else { /* floating: no padding in the exponent */
        if (digits < 1)
            digits = 1;
        if (digits > 19)
            digits = 19;
        char tmp[64];
        snprintf(tmp, sizeof tmp, "%.*e", digits - 1, v);
        char *e = strchr(tmp, 'e');
        int x = e ? atoi(e + 1) : 0;
        if (e)
            *e = 0;
        snprintf(buf, sizeof buf, "%se%+d", tmp, x);
    }
    size_t n = strlen(buf);
    if (n > 80)
        n = 80;
    wr8(out, (uint8_t)n);
    gput(out + 1, buf, (uint32_t)n);
}

/* Str2Dec: the longest numeric prefix of s from *index on.  Pascal strings
 * count from 1, C strings from 0. */
static void str2dec(const char *s, int len, gaddr indexp, gaddr dec, gaddr valid, int base)
{
    int i = (int16_t)rd16(indexp) - base;
    int start = i;
    while (i < len && (s[i] == ' ' || s[i] == '\t'))
        i++;
    int sgn = 0;
    if (i < len && (s[i] == '+' || s[i] == '-'))
        sgn = s[i++] == '-';
    /* All the mantissa's digits; frac of them follow the point. */
    char all[256];
    int na = 0, frac = 0;
    bool point = false;
    while (i < len && ((s[i] >= '0' && s[i] <= '9') || (s[i] == '.' && !point))) {
        if (s[i] == '.')
            point = true;
        else if (na < 255) {
            all[na++] = s[i];
            frac += point;
        }
        i++;
    }
    int mant = na, lead = 0;
    while (lead < na && all[lead] == '0')
        lead++;
    char digits[32];
    int nd = na - lead < 19 ? na - lead : 19;
    memcpy(digits, all + lead, (size_t)(nd > 0 ? nd : 0));
    int exp = (na - lead - nd) - frac; /* digits dropped past 19 scale it up */
    bool ok = mant > 0;
    if (ok && i < len && (s[i] == 'e' || s[i] == 'E')) {
        int j = i + 1, esgn = 1, ev = 0, ed = 0;
        if (j < len && (s[j] == '+' || s[j] == '-'))
            esgn = s[j++] == '-' ? -1 : 1;
        while (j < len && s[j] >= '0' && s[j] <= '9' && ed < 5) {
            ev = ev * 10 + (s[j++] - '0');
            ed++;
        }
        if (ed) {
            exp += esgn * ev;
            i = j;
        }
    }
    if (!ok && i + 2 < len + 0 && !strncasecmp(s + i, "INF", 3)) {
        digits[0] = 'I';
        nd = 1;
        i += 3;
        ok = true;
        exp = 0;
    }
    if (!nd) {
        digits[0] = '0';
        nd = 1;
        exp = 0;
    }
    if (!ok)
        i = start;
    wr8(dec, (uint8_t)sgn);
    wr8(dec + 1, 0);
    wr16(dec + 2, (uint16_t)(int16_t)exp);
    wr8(dec + 4, (uint8_t)nd);
    gput(dec + 5, digits, (uint32_t)nd);
    wr16(indexp, (uint16_t)(i + base));
    if (valid)
        wr8(valid, ok ? 1 : 0);
}

void tb_ROMlib_Fpstr2dec(gaddr s, gaddr indexp, gaddr dec, gaddr valid)
{
    char buf[256];
    int len = rd8(s);
    gget(buf, s + 1, (uint32_t)len);
    str2dec(buf, len, indexp, dec, valid, 1);
}

void tb_ROMlib_Fcstr2dec(gaddr s, gaddr indexp, gaddr dec, gaddr valid)
{
    char buf[256];
    int len = 0;
    while (len < 255 && (buf[len] = (char)rd8(s + (uint32_t)len)))
        len++;
    str2dec(buf, len, indexp, dec, valid, 0);
}

/* ---------------------------------------------------------------------- */
/* Script Manager number formatting.  A NumFormatStringRec here simply keeps
 * the canonical format string (fVersion 1); conversions read it back: the
 * part before any ';' formats positives, the second part negatives.  Digits
 * after the '.' set the decimal places, a ',' before it groups thousands,
 * and other characters (or 'quoted' text) are copied as literals. */

enum { fFormatOK = 0, fEmptyFormatString = 13 };

static int fmt_get(gaddr formatp, char *out)
{
    unsigned n = rd8(formatp);
    if (n > 253)
        n = 253;
    for (unsigned i = 0; i < n; i++)
        out[i] = (char)rd8(formatp + 2 + i);
    out[n] = 0;
    return (int)n;
}

int16_t tb_StringToFormatRec(gaddr in_string, gaddr partsp, gaddr out_string)
{
    unsigned n = rd8(in_string);
    if (n > 253)
        n = 253;
    wr8(out_string, (uint8_t)n);
    wr8(out_string + 1, 1);
    for (unsigned i = 0; i < n; i++)
        wr8(out_string + 2 + i, rd8(in_string + 1 + i));
    return n ? fFormatOK : fEmptyFormatString;
}

/* The section of a format for this sign: positives, negatives, zero. */
static const char *fmt_part(const char *fmt, int which, int *len)
{
    const char *p = fmt;
    for (int i = 0; i < which; i++) {
        const char *semi = strchr(p, ';');
        if (!semi)
            return NULL;
        p = semi + 1;
    }
    const char *end = strchr(p, ';');
    *len = end ? (int)(end - p) : (int)strlen(p);
    return p;
}

int16_t tb_ExtendedToString(gaddr xp, gaddr formatp, gaddr partsp, gaddr string)
{
    char fmt[256], out[512];
    double v = ext_get(xp);
    int len = 0, which = 0;
    fmt_get(formatp, fmt);
    if (v < 0 && fmt_part(fmt, 1, &len))
        which = 1;
    else if (v == 0 && fmt_part(fmt, 2, &len))
        which = 2;
    const char *f = fmt_part(fmt, which, &len);
    double a = which ? fabs(v) : v;

    int decimals = 0, grouping = 0, seen_point = 0, any_digit = 0;
    for (int i = 0; i < len; i++) {
        char ch = f[i];
        if (ch == '\'') {
            while (++i < len && f[i] != '\'')
                ;
        } else if (ch == '.') {
            seen_point = 1;
        } else if (ch == '0' || ch == '#' || ch == '^') {
            any_digit = 1;
            if (seen_point)
                decimals++;
        } else if (ch == ',' && !seen_point) {
            grouping = 1;
        }
    }
    if (!any_digit)
        decimals = 6;

    char num[400], grouped[512];
    snprintf(num, sizeof num, "%.*f", decimals, a);
    const char *src = num;
    char *g = grouped;
    if (*src == '-')
        *g++ = *src++;
    int intlen = (int)strcspn(src, ".");
    for (int i = 0; i < intlen; i++) {
        *g++ = src[i];
        if (grouping && (intlen - i - 1) % 3 == 0 && i != intlen - 1)
            *g++ = ',';
    }
    strcpy(g, src + intlen);

    /* Literals before the first digit placeholder go first, the rest after. */
    int o = 0, placed = 0;
    for (int i = 0; i < len && o < 400; i++) {
        char ch = f[i];
        if (ch == '\'') {
            while (++i < len && f[i] != '\'' && o < 400)
                out[o++] = f[i];
        } else if (strchr("0#^.,", ch)) {
            if (!placed) {
                int gl = (int)strlen(grouped);
                memcpy(out + o, grouped, (size_t)gl);
                o += gl;
                placed = 1;
            }
        } else {
            out[o++] = ch;
        }
    }
    if (!placed) {
        int gl = (int)strlen(grouped);
        memcpy(out + o, grouped, (size_t)gl);
        o += gl;
    }
    if (o > 255)
        o = 255;
    wr8(string, (uint8_t)o);
    for (int i = 0; i < o; i++)
        wr8(string + 1 + (uint32_t)i, (uint8_t)out[i]);
    return fFormatOK;
}

int16_t tb_StringToExtended(gaddr string, gaddr formatp, gaddr partsp, gaddr xp)
{
    char fmt[256], buf[256];
    fmt_get(formatp, fmt);
    unsigned n = rd8(string), k = 0;
    int negative = 0, any = 0;
    for (unsigned i = 0; i < n; i++) {
        char ch = (char)rd8(string + 1 + i);
        if ((ch >= '0' && ch <= '9') || ch == '.' || ch == 'e' || ch == 'E' || (ch == '-' && any && (buf[k - 1] == 'e' || buf[k - 1] == 'E'))) {
            buf[k++] = ch;
            any = 1;
        } else if (ch == '-' || ch == '(') {
            negative = 1;
        }
    }
    buf[k] = 0;
    double v = any ? strtod(buf, NULL) : 0;
    ext_put(xp, negative ? -v : v);
    return any ? fFormatOK : 1; /* fBestGuess */
}

/* For MathLib (ppc_libs.c). */
double sane_ext_get(gaddr p) { return ext_get(p); }
void sane_ext_put(gaddr p, double v) { ext_put(p, v); }
double sane_dec_get(gaddr d) { return dec_get(d); }
void sane_dec_put(gaddr d, gaddr form, double v) { dec_put(d, form, v); }
