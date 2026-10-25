/*
 * The rest of MathLib (the C math functions, SANE's decimal conversions
 * and the classification calls on doubles are in ppc_libs.c): the long
 * double routines, the float and long double classification calls, the
 * floating-point environment and the decimal conversions to and from
 * float, short, long and long double.
 *
 * MathLib's long double is a double-double: two doubles, a head and a
 * tail, whose sum is the value; the tail's exponent at least 54 below the
 * head's, and 0 when the head is 0, denormal, infinite or a NaN (PowerPC
 * Numerics, chapter 2).  A long double argument takes two FPRs in turn
 * (head, then tail) and four GPR slots; the result comes back in f1 (head)
 * and f2 (tail).  The routines that are exact or nearly so in a double-
 * double (rounding, comparison, scaling, square root, remainder, the
 * decimal conversions) keep the full precision; the transcendental ones
 * are computed on the value rounded to a double and come back with a tail
 * of 0, as accurate as the double routines are.
 *
 * The environment is the PowerPC's FPSCR, as MathLib's is: fenv_t is its
 * image, the exception flags are its sticky bits and the rounding
 * direction its RN field.
 */
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "machine.h"
#include "ppc_bridge.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define F(n) ppc_fpr(c, 1 + (n))

static void ret_int(ppc_cpu *c, uint32_t v)
{
    c->gpr[3] = v;
    c->pc = c->lr;
}

static void ret_double(ppc_cpu *c, double v)
{
    ppc_set_fpr(c, 1, v);
    c->pc = c->lr;
}

/* Argument word n of a call (GPR slots, then the parameter area). */
static uint32_t arg_word(const ppc_cpu *c, unsigned n)
{
    return n < 8 ? c->gpr[3 + n] : rd32(c->gpr[1] + 24 + 4 * n);
}

static uint64_t dbits(double d)
{
    uint64_t b;
    memcpy(&b, &d, 8);
    return b;
}

static double bitsd(uint64_t b)
{
    double d;
    memcpy(&d, &b, 8);
    return d;
}

static double rd_double(gaddr p) { return bitsd((uint64_t)rd32(p) << 32 | rd32(p + 4)); }

static void wr_double(gaddr p, double d)
{
    uint64_t b = dbits(d);
    wr32(p, (uint32_t)(b >> 32));
    wr32(p + 4, (uint32_t)b);
}

/* ---------------------------------------------------------------------- */
/* Double-double arithmetic */

typedef struct {
    double hi, lo;
} dd;

static dd d1(double x) { return (dd){ x, 0 }; }

static dd two_sum(double a, double b)
{
    double s = a + b, bb = s - a;
    return (dd){ s, (a - (s - bb)) + (b - bb) };
}

/* The recommended form: the head the sum rounded, the tail what is left,
 * and a tail of 0 when the head isn't a normal number. */
static dd norm(dd a)
{
    if (a.lo == 0)
        return a;
    dd s = two_sum(a.hi, a.lo);
    if (!isnormal(s.hi))
        return d1(a.hi + a.lo);
    return s;
}

static dd neg(dd a) { return (dd){ -a.hi, -a.lo }; }

static dd add(dd a, dd b)
{
    double h = a.hi + b.hi;
    if (!isfinite(h))
        return d1(h);
    dd s = two_sum(a.hi, b.hi), t = two_sum(a.lo, b.lo);
    s.lo += t.hi;
    s = two_sum(s.hi, s.lo);
    s.lo += t.lo;
    return norm(s);
}

static dd sub(dd a, dd b) { return add(a, neg(b)); }

static dd mul(dd a, dd b)
{
    double p = a.hi * b.hi;
    if (!isfinite(p) || p == 0)
        return d1(p);
    double e = fma(a.hi, b.hi, -p);
    e += a.hi * b.lo + a.lo * b.hi;
    return norm((dd){ p, e });
}

static dd dv(dd a, dd b)
{
    double q1 = a.hi / b.hi;
    if (!isfinite(q1) || q1 == 0)
        return d1(q1);
    dd r = sub(a, mul(b, d1(q1)));
    double q2 = r.hi / b.hi;
    r = sub(r, mul(b, d1(q2)));
    double q3 = r.hi / b.hi;
    return add(norm((dd){ q1, q2 }), d1(q3));
}

static double to_double(dd a) { return a.hi + a.lo; }

/* -1, 0 or 1 as a < b, a == b, a > b; 2 unordered. */
static int cmp(dd a, dd b)
{
    if (isnan(a.hi) || isnan(b.hi))
        return 2;
    if (a.hi != b.hi)
        return a.hi < b.hi ? -1 : 1;
    if (a.lo != b.lo)
        return a.lo < b.lo ? -1 : 1;
    return 0;
}

static dd dd_fabs(dd a) { return signbit(a.hi) ? neg(a) : a; }

static dd dd_floor(dd a)
{
    if (!isfinite(a.hi) || a.hi == 0)
        return a;
    double f = floor(a.hi);
    if (f != a.hi)
        return d1(f);
    return norm(two_sum(f, floor(a.lo)));
}

static dd dd_ceil(dd a) { return neg(dd_floor(neg(a))); }

static dd dd_trunc(dd a) { return signbit(a.hi) ? dd_ceil(a) : dd_floor(a); }

static dd with_zero_sign(dd r, dd a) { return r.hi == 0 ? d1(copysign(0, a.hi)) : r; }

static bool dd_odd(dd n) { return n.lo != 0 ? fmod(n.lo, 2) != 0 : fmod(n.hi, 2) != 0; }

/* To an integer in rounding direction mode (FE_TONEAREST 0, FE_TOWARDZERO
 * 1, FE_UPWARD 2, FE_DOWNWARD 3), halfway cases to even. */
static dd dd_rint(dd a, int mode)
{
    if (!isfinite(a.hi) || a.hi == 0)
        return a;
    switch (mode & 3) {
    case 1: return dd_trunc(a);
    case 2: return with_zero_sign(dd_ceil(a), a);
    case 3: return dd_floor(a);
    }
    dd f = dd_floor(a);
    int k = cmp(sub(a, f), d1(0.5));
    if (k > 0 || (k == 0 && dd_odd(f)))
        f = add(f, d1(1));
    return with_zero_sign(f, a);
}

/* Halfway cases away from zero. */
static dd dd_round(dd a)
{
    if (!isfinite(a.hi) || a.hi == 0)
        return a;
    dd t = dd_trunc(a);
    if (cmp(dd_fabs(sub(a, t)), d1(0.5)) >= 0)
        t = add(t, d1(signbit(a.hi) ? -1 : 1));
    return with_zero_sign(t, a);
}

static dd dd_ldexp(dd a, int n) { return norm((dd){ ldexp(a.hi, n), ldexp(a.lo, n) }); }

static dd dd_sqrt(dd a)
{
    if (!(a.hi > 0) || isinf(a.hi))
        return d1(sqrt(a.hi));
    double s = sqrt(a.hi);
    dd r = sub(a, mul(d1(s), d1(s)));
    return norm((dd){ s, r.hi / (2 * s) });
}

/* 10^k, 0 <= k <= 300, by squaring. */
static dd pow10_dd(int k)
{
    dd r = d1(1), p = d1(10);
    for (; k; k >>= 1) {
        if (k & 1)
            r = mul(r, p);
        if (k > 1)
            p = mul(p, p);
    }
    return r;
}

/* a * 10^k, in steps that keep the power of ten finite. */
static dd scale10(dd a, int k)
{
    while (k > 0) {
        int s = k > 280 ? 280 : k;
        a = mul(a, pow10_dd(s));
        k -= s;
    }
    while (k < 0) {
        int s = -k > 280 ? 280 : -k;
        a = dv(a, pow10_dd(s));
        k += s;
    }
    return a;
}

/* The rounding direction now in effect: the FPSCR's RN field. */
static int round_mode(const ppc_cpu *c) { return (int)(c->fpscr & 3); }

/* A double rounded to a 32-bit integer as fctiw does: saturated, and the
 * most negative for a NaN. */
static int32_t sat32(double v)
{
    if (isnan(v) || v < -2147483648.0)
        return INT32_MIN;
    if (v > 2147483647.0)
        return INT32_MAX;
    return (int32_t)v;
}

/* ---------------------------------------------------------------------- */
/* Long double: arguments and results */

/* The long double whose head is in FPR argument n (0 for f1). */
static dd ld(const ppc_cpu *c, int n) { return (dd){ F(n), F(n + 1) }; }

static void ret_ld(ppc_cpu *c, dd v)
{
    ppc_set_fpr(c, 1, v.hi);
    ppc_set_fpr(c, 2, v.lo);
    c->pc = c->lr;
}

/* Computed on the value as a double. */
#define LD_UNARY(name, expr) \
    static void ml_##name(ppc_cpu *c) { double x = to_double(ld(c, 0)); ret_ld(c, d1(expr)); }
#define LD_BINARY(name, expr)                                    \
    static void ml_##name(ppc_cpu *c)                            \
    {                                                            \
        double x = to_double(ld(c, 0)), y = to_double(ld(c, 2)); \
        ret_ld(c, d1(expr));                                     \
    }

LD_UNARY(sinl, sin(x))
LD_UNARY(cosl, cos(x))
LD_UNARY(tanl, tan(x))
LD_UNARY(asinl, asin(x))
LD_UNARY(acosl, acos(x))
LD_UNARY(atanl, atan(x))
LD_UNARY(sinhl, sinh(x))
LD_UNARY(coshl, cosh(x))
LD_UNARY(tanhl, tanh(x))
LD_UNARY(asinhl, asinh(x))
LD_UNARY(acoshl, acosh(x))
LD_UNARY(atanhl, atanh(x))
LD_UNARY(expl, exp(x))
LD_UNARY(exp2l, exp2(x))
LD_UNARY(expm1l, expm1(x))
LD_UNARY(logl, log(x))
LD_UNARY(log2l, log2(x))
LD_UNARY(log10l, log10(x))
LD_UNARY(log1pl, log1p(x))
LD_UNARY(erfl, erf(x))
LD_UNARY(erfcl, erfc(x))
LD_UNARY(gammal, tgamma(x))
LD_UNARY(lgammal, lgamma(x))
LD_BINARY(atan2l, atan2(x, y))
LD_BINARY(powl, pow(x, y))

/* Exact in the double-double. */
static void ml_fabsl(ppc_cpu *c) { ret_ld(c, dd_fabs(ld(c, 0))); }
static void ml_copysignl(ppc_cpu *c)
{
    dd x = ld(c, 0);
    ret_ld(c, signbit(x.hi) != signbit(F(2)) ? neg(x) : x);
}
static void ml_floorl(ppc_cpu *c) { ret_ld(c, dd_floor(ld(c, 0))); }
static void ml_ceill(ppc_cpu *c) { ret_ld(c, dd_ceil(ld(c, 0))); }
static void ml_truncl(ppc_cpu *c) { ret_ld(c, dd_trunc(ld(c, 0))); }
static void ml_roundl(ppc_cpu *c) { ret_ld(c, dd_round(ld(c, 0))); }
static void ml_rintl(ppc_cpu *c) { ret_ld(c, dd_rint(ld(c, 0), round_mode(c))); }
static void ml_rinttoll(ppc_cpu *c) { ret_int(c, (uint32_t)sat32(to_double(dd_rint(ld(c, 0), round_mode(c))))); }
static void ml_roundtoll(ppc_cpu *c) { ret_int(c, (uint32_t)sat32(to_double(dd_round(ld(c, 0))))); }
static void ml_sqrtl(ppc_cpu *c) { ret_ld(c, dd_sqrt(ld(c, 0))); }

/* NaNs are missing data: the other argument, or the first of two NaNs. */
static void ml_fmaxl(ppc_cpu *c)
{
    dd x = ld(c, 0), y = ld(c, 2);
    ret_ld(c, isnan(y.hi) ? x : isnan(x.hi) ? y : cmp(x, y) >= 0 ? x : y);
}
static void ml_fminl(ppc_cpu *c)
{
    dd x = ld(c, 0), y = ld(c, 2);
    ret_ld(c, isnan(y.hi) ? x : isnan(x.hi) ? y : cmp(x, y) <= 0 ? x : y);
}
static void ml_fdiml(ppc_cpu *c)
{
    dd x = ld(c, 0), y = ld(c, 2);
    ret_ld(c, isnan(x.hi) ? x : isnan(y.hi) ? y : cmp(x, y) > 0 ? sub(x, y) : d1(0));
}

/* relop: GREATERTHAN 0, LESSTHAN 1, EQUALTO 2, UNORDERED 3. */
static uint32_t relop(int k) { return k == 1 ? 0 : k == -1 ? 1 : k == 0 ? 2 : 3; }
static void ml_relation(ppc_cpu *c) { ret_int(c, relop(cmp(d1(F(0)), d1(F(1))))); }
static void ml_relationl(ppc_cpu *c) { ret_int(c, relop(cmp(ld(c, 0), ld(c, 2)))); }

/* frexpl(long double x, int *exponent): the pointer in r7.  The fraction
 * is in [0.5, 1): a head of exactly 0.5 with a tail against it is less. */
static void ml_frexpl(ppc_cpu *c)
{
    dd x = ld(c, 0);
    int e = 0;
    if (isfinite(x.hi) && x.hi != 0) {
        x.hi = frexp(x.hi, &e);
        x.lo = ldexp(x.lo, -e);
        if (fabs(x.hi) == 0.5 && x.lo != 0 && signbit(x.lo) != signbit(x.hi)) {
            x = (dd){ x.hi * 2, x.lo * 2 };
            e--;
        }
    }
    if (R(4))
        wr32(R(4), (uint32_t)e);
    ret_ld(c, x);
}

/* ldexpl(long double x, int n), scalbl(long double x, long n): n in r7. */
static void ml_ldexpl(ppc_cpu *c) { ret_ld(c, dd_ldexp(ld(c, 0), (int32_t)R(4))); }

static void ml_logbl(ppc_cpu *c)
{
    dd x = ld(c, 0);
    double e = logb(x.hi);
    int k;
    if (isfinite(x.hi) && x.hi != 0 && fabs(frexp(x.hi, &k)) == 0.5 && x.lo != 0 &&
        signbit(x.lo) != signbit(x.hi))
        e -= 1;
    ret_ld(c, d1(e));
}

/* modfl(long double x, long double *iptr): the pointer in r7. */
static void ml_modfl(ppc_cpu *c)
{
    dd x = ld(c, 0), i = dd_trunc(x);
    dd f = isinf(x.hi) ? d1(copysign(0, x.hi)) : with_zero_sign(sub(x, i), x);
    if (R(4)) {
        wr_double(R(4), i.hi);
        wr_double(R(4) + 8, i.lo);
    }
    ret_ld(c, f);
}

/* The neighbour in a 107-bit significand: one unit in the 107th bit of
 * the head's binade, or the double's neighbour near the bottom of the
 * range where the tail can't go. */
static void ml_nextafterl(ppc_cpu *c)
{
    dd x = ld(c, 0), y = ld(c, 2);
    int k = cmp(x, y);
    if (k == 2) {
        ret_ld(c, isnan(x.hi) ? x : y);
        return;
    }
    if (k == 0) {
        ret_ld(c, y);
        return;
    }
    double dir = k < 0 ? INFINITY : -INFINITY;
    if (isinf(x.hi)) {
        ret_ld(c, d1(copysign(DBL_MAX, x.hi)));
        return;
    }
    int e = ilogb(x.hi);
    if (x.hi == 0 || e - 106 < DBL_MIN_EXP - 1) {
        ret_ld(c, d1(nextafter(to_double(x), dir)));
        return;
    }
    ret_ld(c, add(x, d1(copysign(ldexp(1, e - 106), dir))));
}

static void ml_hypotl(ppc_cpu *c)
{
    dd x = dd_fabs(ld(c, 0)), y = dd_fabs(ld(c, 2));
    if (isinf(x.hi) || isinf(y.hi)) {
        ret_ld(c, d1(INFINITY));
        return;
    }
    if (isnan(x.hi) || isnan(y.hi)) {
        ret_ld(c, isnan(x.hi) ? x : y);
        return;
    }
    if (x.hi == 0 || y.hi == 0) {
        ret_ld(c, x.hi == 0 ? y : x);
        return;
    }
    int e = ilogb(x.hi) > ilogb(y.hi) ? ilogb(x.hi) : ilogb(y.hi);
    x = dd_ldexp(x, -e);
    y = dd_ldexp(y, -e);
    ret_ld(c, dd_ldexp(dd_sqrt(add(mul(x, x), mul(y, y))), e));
}

/* The remainder x - n*y, n the integer nearest x/y (halfway to even), and
 * n's seven low bits with the quotient's sign (SANE's remainder). */
static dd remquo_dd(dd x, dd y, int *quo)
{
    *quo = 0;
    if (isnan(x.hi) || isnan(y.hi))
        return isnan(x.hi) ? x : y;
    if (isinf(x.hi) || y.hi == 0)
        return d1(NAN);
    if (isinf(y.hi))
        return x;
    bool negx = signbit(x.hi), negq = negx != signbit(y.hi);
    dd ax = dd_fabs(x), ay = dd_fabs(y), r;
    double low; /* n mod 128 */
    if (ax.lo == 0 && ay.lo == 0) {
        /* The double remainder is exact; n mod 128 from the remainder
         * modulo 128y, which is too. */
        double rr = remainder(ax.hi, ay.hi), m = fmod(ax.hi, 128 * ay.hi);
        low = fmod(rint((m - rr) / ay.hi) + 128, 128);
        r = d1(rr);
    } else {
        dd n = dd_rint(dv(ax, ay), 0);
        if (n.hi >= 0x1p100) {
            /* Beyond the double-double's precision for n * y. */
            int q;
            r = d1(remquo(ax.hi, ay.hi, &q));
            low = abs(q) & 127;
        } else {
            r = sub(ax, mul(n, ay));
            dd half = dd_ldexp(ay, -1);
            int k = cmp(r, half), j = cmp(r, neg(half));
            if (k > 0 || (k == 0 && dd_odd(n))) {
                r = sub(r, ay);
                n = add(n, d1(1));
            } else if (j < 0 || (j == 0 && dd_odd(n))) {
                r = add(r, ay);
                n = sub(n, d1(1));
            }
            low = fmod(fmod(n.hi, 128) + fmod(n.lo, 128) + 256, 128);
        }
    }
    *quo = negq ? -(int)low : (int)low;
    if (negx)
        r = neg(r);
    return with_zero_sign(r, x);
}

static void ml_remainderl(ppc_cpu *c)
{
    int q;
    ret_ld(c, remquo_dd(ld(c, 0), ld(c, 2), &q));
}

/* remquol(long double x, long double y, int *quo): the pointer is the
 * ninth argument word, in the parameter area. */
static void ml_remquol(ppc_cpu *c)
{
    int q;
    dd r = remquo_dd(ld(c, 0), ld(c, 2), &q);
    gaddr p = arg_word(c, 8);
    if (p)
        wr32(p, (uint32_t)q);
    ret_ld(c, r);
}

/* ---------------------------------------------------------------------- */
/* Floats */

static uint32_t fbits(float f)
{
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
}

/* modff(float x, float *iptr): a float takes one GPR slot, so the pointer
 * is in r4. */
static void ml_modff(ppc_cpu *c)
{
    float i, f = modff((float)F(0), &i);
    if (R(1))
        wr32(R(1), fbits(i));
    ret_double(c, f);
}

static void ml_nextafterf(ppc_cpu *c) { ret_double(c, nextafterf((float)F(0), (float)F(1))); }

/* nan("n"): a quiet NaN with NaN code n (0 to 255; 0 is code 21, "a NaN
 * with a zero code") in the 8th to 15th bits of the fraction.  As a double
 * the float's fraction is the high bits of the double's, so one double
 * serves nan, nanf (rounded to a float as it is returned) and nanl. */
static double nan_code(gaddr tagp)
{
    char buf[16] = { 0 };
    for (int i = 0; tagp && i < 15 && (buf[i] = (char)rd8(tagp + (gaddr)i)); i++)
        ;
    unsigned code = (unsigned)strtoul(buf, NULL, 10) & 0xFF;
    if (!code)
        code = 21;
    return bitsd(0x7FF8000000000000ull | (uint64_t)code << 37);
}
static void ml_nan(ppc_cpu *c) { ret_double(c, nan_code(R(0))); }
static void ml_nanl(ppc_cpu *c) { ret_ld(c, d1(nan_code(R(0)))); }

/* ---------------------------------------------------------------------- */
/* Classification: FP_SNAN 0, FP_QNAN 1, FP_INFINITE 2, FP_ZERO 3,
 * FP_NORMAL 4, FP_SUBNORMAL 5.  A long double's is its head's. */

static uint32_t classify(double x)
{
    if (isnan(x))
        return dbits(x) & 0x0008000000000000ull ? 1 : 0;
    return isinf(x) ? 2 : x == 0 ? 3 : isnormal(x) ? 4 : 5;
}
static void ml_fpclassify(ppc_cpu *c) { ret_int(c, classify(F(0))); }
static void ml_isfinite(ppc_cpu *c) { ret_int(c, isfinite(F(0)) ? 1 : 0); }
static void ml_isnormal(ppc_cpu *c) { ret_int(c, isnormal(F(0)) ? 1 : 0); }
static void ml_signbit(ppc_cpu *c) { ret_int(c, signbit(F(0)) ? 1 : 0); }

/* ---------------------------------------------------------------------- */
/* The environment */

enum {
    FE_INEXACT = 0x02000000, FE_DIVBYZERO = 0x04000000, FE_UNDERFLOW = 0x08000000,
    FE_OVERFLOW = 0x10000000, FE_INVALID = 0x20000000, FE_ALL_EXCEPT = 0x3E000000,
    /* The FPSCR: exception summary, the invalid operation causes (one of
     * which, VXSOFT, is the software request), and the enables. */
    FPSCR_FX = 0x80000000, FPSCR_FEX = 0x40000000, FPSCR_VXALL = 0x01F80700, FPSCR_VXSOFT = 0x00000400,
    FPSCR_ENABLES = 0x000000F8
};

/* The flags set: invalid when any invalid operation cause is. */
static uint32_t fe_flags(const ppc_cpu *c)
{
    uint32_t f = c->fpscr & (FE_ALL_EXCEPT & ~FE_INVALID);
    if (c->fpscr & (FE_INVALID | FPSCR_VXALL))
        f |= FE_INVALID;
    return f;
}

static void fe_clear(ppc_cpu *c, uint32_t e)
{
    e &= FE_ALL_EXCEPT;
    c->fpscr &= ~e;
    if (e & FE_INVALID)
        c->fpscr &= ~FPSCR_VXALL;
    if (!fe_flags(c))
        c->fpscr &= ~(FPSCR_FX | FPSCR_FEX);
}

static void fe_set(ppc_cpu *c, uint32_t e)
{
    e &= FE_ALL_EXCEPT;
    if (!e)
        return;
    c->fpscr |= e | FPSCR_FX;
    if (e & FE_INVALID)
        c->fpscr |= FPSCR_VXSOFT;
}

static void ml_feclearexcept(ppc_cpu *c)
{
    fe_clear(c, R(0));
    c->pc = c->lr;
}
static void ml_feraiseexcept(ppc_cpu *c)
{
    fe_set(c, R(0));
    c->pc = c->lr;
}
static void ml_fetestexcept(ppc_cpu *c) { ret_int(c, fe_flags(c) & R(0)); }
/* fegetexcept(fexcept_t *flagp, int excepts) */
static void ml_fegetexcept(ppc_cpu *c)
{
    if (R(0))
        wr32(R(0), fe_flags(c) & R(1) & FE_ALL_EXCEPT);
    c->pc = c->lr;
}
/* fesetexcept(const fexcept_t *flagp, int excepts): sets the flags, raises
 * nothing. */
static void ml_fesetexcept(ppc_cpu *c)
{
    uint32_t e = R(1) & FE_ALL_EXCEPT, v = R(0) ? rd32(R(0)) : 0;
    fe_clear(c, e);
    fe_set(c, v & e);
    c->pc = c->lr;
}
static void ml_fegetround(ppc_cpu *c) { ret_int(c, c->fpscr & 3); }
/* Nonzero when round is a rounding direction, which it then sets. */
static void ml_fesetround(ppc_cpu *c)
{
    if (R(0) & ~3u) {
        ret_int(c, 0);
        return;
    }
    c->fpscr = (c->fpscr & ~3u) | R(0);
    ret_int(c, 1);
}
static void ml_fegetenv(ppc_cpu *c)
{
    if (R(0))
        wr32(R(0), c->fpscr);
    c->pc = c->lr;
}
/* Saves the environment, clears the flags and the exception enables. */
static void ml_feholdexcept(ppc_cpu *c)
{
    if (R(0))
        wr32(R(0), c->fpscr);
    fe_clear(c, FE_ALL_EXCEPT);
    c->fpscr &= ~FPSCR_ENABLES;
    ret_int(c, 0);
}
static void ml_fesetenv(ppc_cpu *c)
{
    if (R(0))
        c->fpscr = rd32(R(0));
    c->pc = c->lr;
}
/* Installs the environment and raises again the flags set before. */
static void ml_feupdateenv(ppc_cpu *c)
{
    uint32_t raised = fe_flags(c);
    if (R(0))
        c->fpscr = rd32(R(0));
    fe_set(c, raised);
    c->pc = c->lr;
}

/* ---------------------------------------------------------------------- */
/* Decimal conversions.  decimal: sgn 0, exp 2 (a power of ten), sig 4 (a
 * length byte, then up to SIGDIGLEN = 36 characters); decform: style 0
 * (FLOATDECIMAL 0, FIXEDDECIMAL 1), digits 2. */

enum { SIGDIGLEN = 36 };

/* A decimal record's value.  A significand starting with '0' is zero, 'I'
 * infinity, 'N' a NaN whose hex digits after it are its fraction (fewer
 * than four right-aligned in the fraction's high 15 bits, more left-aligned
 * in the significand). */
static dd dec_get_dd(gaddr d)
{
    bool negative = rd8(d) != 0;
    int exp = (int16_t)rd16(d + 2);
    int n = rd8(d + 4);
    if (n > SIGDIGLEN)
        n = SIGDIGLEN;
    char sig[SIGDIGLEN + 1] = { 0 };
    gget(sig, d + 5, (uint32_t)n);
    dd v;
    if (!n || sig[0] == '0') {
        v = d1(0);
    } else if (sig[0] == 'I') {
        v = d1(INFINITY);
    } else if (sig[0] == 'N') {
        uint64_t h = 0;
        int k = 0;
        for (const char *s = sig + 1; *s && k < 16; s++, k++) {
            int x = *s >= '0' && *s <= '9' ? *s - '0' : (*s | 0x20) >= 'a' && (*s | 0x20) <= 'f' ? (*s | 0x20) - 'a' + 10 : -1;
            if (x < 0)
                break;
            h = h << 4 | (uint64_t)x;
        }
        uint64_t frac = k < 4 ? h << 37 : (h << (4 * (16 - k))) >> 11;
        v = d1(bitsd(0x7FF8000000000000ull | (frac & 0x000FFFFFFFFFFFFFull)));
    } else {
        __int128 m = 0;
        for (int i = 0; i < n && sig[i] >= '0' && sig[i] <= '9'; i++)
            m = m * 10 + (sig[i] - '0');
        double hi = (double)m;
        v = norm((dd){ hi, (double)(m - (__int128)hi) });
        v = scale10(v, exp);
    }
    return negative ? neg(v) : v;
}

static void ml_dec2numl(ppc_cpu *c) { ret_ld(c, dec_get_dd(R(0))); }

/* To the nearest float, the tail deciding a head halfway between two. */
static float dd_to_float(dd v)
{
    float f = (float)v.hi;
    if (!isfinite(f) || v.lo == 0)
        return f;
    float g = nextafterf(f, ((double)f < v.hi || ((double)f == v.hi && v.lo > 0)) ? INFINITY : -INFINITY);
    int k = cmp(dd_fabs(sub(v, d1(g))), dd_fabs(sub(v, d1(f))));
    return k < 0 || (k == 0 && !(fbits(g) & 1)) ? g : f;
}

static void ml_dec2f(ppc_cpu *c) { ret_double(c, dd_to_float(dec_get_dd(R(0)))); }

/* To an integer in the rounding direction, saturated as fctiw does. */
static void ml_dec2l(ppc_cpu *c) { ret_int(c, (uint32_t)sat32(to_double(dd_rint(dec_get_dd(R(0)), round_mode(c))))); }

static void ml_dec2s(ppc_cpu *c)
{
    int32_t v = sat32(to_double(dd_rint(dec_get_dd(R(0)), round_mode(c))));
    ret_int(c, (uint32_t)(v < INT16_MIN ? INT16_MIN : v > INT16_MAX ? INT16_MAX : v));
}

static void dec_put_sig(gaddr d, int exp, const char *sig)
{
    size_t n = strlen(sig);
    wr16(d + 2, (uint16_t)(int16_t)exp);
    wr8(d + 4, (uint8_t)n);
    gput(d + 5, sig, (uint32_t)n);
}

/* The decimal digits of a nonnegative integer that fits the double-double
 * (its head and tail are integers). */
static int dd_digits(dd n, char *out)
{
    __int128 v = (__int128)n.hi + (__int128)n.lo;
    char tmp[48];
    int k = 0;
    do {
        tmp[k++] = (char)('0' + (int)(v % 10));
        v /= 10;
    } while (v > 0 && k < 47);
    for (int i = 0; i < k; i++)
        out[i] = tmp[k - 1 - i];
    out[k] = 0;
    return k;
}

/* x in a decimal record as decform f asks: floating style, digits
 * significant digits (1 to 36); fixed style, digits after the point
 * (negative rounds to the left of it), "?" when that needs more than 36. */
static void dec_put_dd(gaddr d, gaddr f, dd x, int mode)
{
    int style = rd8(f), digits = (int16_t)rd16(f + 2);
    bool negative = signbit(x.hi);
    wr8(d, negative ? 1 : 0);
    wr8(d + 1, 0);
    x = dd_fabs(x);
    char sig[48];
    if (isnan(x.hi)) {
        char hex[20];
        snprintf(hex, sizeof hex, "N%016llX", (unsigned long long)((dbits(x.hi) & 0x000FFFFFFFFFFFFFull) << 11));
        dec_put_sig(d, 0, hex);
        return;
    }
    if (isinf(x.hi)) {
        dec_put_sig(d, 0, "I");
        return;
    }
    if (x.hi == 0) {
        dec_put_sig(d, 0, "0");
        return;
    }
    /* The digits are rounded in the rounding direction; a negative number's
     * upward is its magnitude's downward. */
    if (negative && (mode == 2 || mode == 3))
        mode ^= 1;
    if (style & 1) {
        if (digits > 400)
            digits = 400;
        if (digits < -400)
            digits = -400;
        dd n = dd_rint(scale10(x, digits), mode);
        int len = n.hi == 0 ? 0 : (n.hi >= 1e36 ? SIGDIGLEN + 1 : dd_digits(n, sig));
        if (len > SIGDIGLEN)
            dec_put_sig(d, 0, "?");
        else if (!len)
            dec_put_sig(d, 0, "0");
        else
            dec_put_sig(d, -digits, sig);
        return;
    }
    if (digits < 1)
        digits = 1;
    if (digits > SIGDIGLEN)
        digits = SIGDIGLEN;
    int e10 = (int)floor(log10(x.hi));
    for (int tries = 0; tries < 4; tries++) {
        dd n = dd_rint(scale10(x, digits - 1 - e10), mode);
        int len = dd_digits(n, sig);
        if (len > digits) {
            e10++;
            continue;
        }
        if (len < digits && tries < 3) {
            e10--;
            continue;
        }
        break;
    }
    dec_put_sig(d, e10 - (digits - 1), sig);
}

/* num2decl(const decform *f, long double x, decimal *d): x in f1 and f2
 * (GPR slots 1 to 4), d in r8. */
static void ml_num2decl(ppc_cpu *c)
{
    dec_put_dd(R(5), R(0), ld(c, 0), round_mode(c));
    c->pc = c->lr;
}

/* ---------------------------------------------------------------------- */

const ppc_export ml_exports[] = {
    { "acoshl", ml_acoshl }, { "acosl", ml_acosl }, { "asinhl", ml_asinhl }, { "asinl", ml_asinl },
    { "atan2l", ml_atan2l }, { "atanhl", ml_atanhl }, { "atanl", ml_atanl }, { "ceill", ml_ceill },
    { "copysignl", ml_copysignl }, { "coshl", ml_coshl }, { "cosl", ml_cosl }, { "erfcl", ml_erfcl },
    { "erfl", ml_erfl }, { "exp2l", ml_exp2l }, { "expl", ml_expl }, { "expm1l", ml_expm1l },
    { "fabsl", ml_fabsl }, { "fdiml", ml_fdiml }, { "floorl", ml_floorl }, { "fmaxl", ml_fmaxl },
    { "fminl", ml_fminl }, { "frexpl", ml_frexpl }, { "gammal", ml_gammal }, { "hypotl", ml_hypotl },
    { "ldexpl", ml_ldexpl }, { "lgammal", ml_lgammal }, { "log10l", ml_log10l }, { "log1pl", ml_log1pl },
    { "log2l", ml_log2l }, { "logbl", ml_logbl }, { "logl", ml_logl }, { "modfl", ml_modfl },
    { "nanl", ml_nanl }, { "nearbyintl", ml_rintl }, { "nextafterl", ml_nextafterl }, { "powl", ml_powl },
    { "remainderl", ml_remainderl }, { "remquol", ml_remquol }, { "rintl", ml_rintl },
    { "rinttoll", ml_rinttoll }, { "roundl", ml_roundl }, { "roundtoll", ml_roundtoll },
    { "scalbl", ml_ldexpl }, { "sinhl", ml_sinhl }, { "sinl", ml_sinl }, { "sqrtl", ml_sqrtl },
    { "tanhl", ml_tanhl }, { "tanl", ml_tanl }, { "truncl", ml_truncl },
    { "relation", ml_relation }, { "relationl", ml_relationl },
    { "dec2f", ml_dec2f }, { "dec2l", ml_dec2l }, { "dec2s", ml_dec2s }, { "dec2numl", ml_dec2numl },
    { "num2decl", ml_num2decl },
    { "modff", ml_modff }, { "nextafterf", ml_nextafterf }, { "nan", ml_nan }, { "nanf", ml_nan },
    /* Long double classification: the head's. */
    { "__fpclassify", ml_fpclassify }, { "__isfinite", ml_isfinite }, { "__isnormal", ml_isnormal },
    { "__signbit", ml_signbit },
    { "feclearexcept", ml_feclearexcept }, { "fegetenv", ml_fegetenv }, { "fegetexcept", ml_fegetexcept },
    { "fegetround", ml_fegetround }, { "feholdexcept", ml_feholdexcept }, { "feraiseexcept", ml_feraiseexcept },
    { "fesetenv", ml_fesetenv }, { "fesetexcept", ml_fesetexcept }, { "fesetround", ml_fesetround },
    { "fetestexcept", ml_fetestexcept }, { "feupdateenv", ml_feupdateenv },
};
const unsigned ml_export_count = sizeof ml_exports / sizeof ml_exports[0];

/* _FE_DFL_ENV: the default environment, an FPSCR of 0 (round to nearest,
 * no flags, no enables). */
bool ml_data_export_known(const char *name) { return !strcmp(name, "_FE_DFL_ENV"); }

gaddr ml_data_export(const char *name)
{
    if (!strcmp(name, "_FE_DFL_ENV"))
        return tb_NewPtr(4, true, true);
    return 0;
}
