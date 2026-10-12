/*
 * SANE: the Standard Apple Numerics Environment, as the FP68K (Pack4) and
 * Elems68K (Pack5) packages.
 *
 * Each call pushes operand addresses, then an operation word, then traps.
 * Bits 0-7 of the word select the operation and bits 11-13 the format of
 * the source operand (extended, double, single, -, integer, longint, comp).
 *
 * Arithmetic is done in host doubles, so extended results carry a double's
 * 53-bit precision rather than 64 bits.  The environment word keeps the
 * rounding direction, exception flags and halt enables; halts are not taken.
 */
#include <fenv.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "m68k.h"
#include "tb_internal.h"

enum { F_EXT = 0, F_DBL, F_SGL, F_EXT96, F_INT, F_LONG, F_COMP };

/* Exceptions, in environment-word order (flags at bit 8, halts at bit 0). */
enum { X_INVALID = 1, X_UNDERFLOW = 2, X_OVERFLOW = 4, X_DIVBYZERO = 8, X_INEXACT = 16 };

static uint16_t env;

/* ---------------------------------------------------------------------- */
/* Formats */

static double ext_get(gaddr p)
{
    uint16_t se = rd16(p);
    uint64_t m = (uint64_t)rd32(p + 2) << 32 | rd32(p + 6);
    int sign = se >> 15, e = se & 0x7FFF;
    double v;
    if (e == 0x7FFF)
        v = (m << 1) ? NAN : INFINITY;
    else
        v = ldexp((double)m, e - 16383 - 63);
    return sign ? -v : v;
}

static void ext_put(gaddr p, double v)
{
    uint16_t se = signbit(v) ? 0x8000 : 0;
    uint64_t m = 0;
    if (isnan(v)) {
        se |= 0x7FFF;
        m = 0x4000000000000000ull | 0x0011000000000000ull; /* a quiet NaN */
    } else if (isinf(v)) {
        se |= 0x7FFF;
    } else if (v != 0) {
        int e;
        double f = frexp(fabs(v), &e); /* [0.5, 1) */
        m = (uint64_t)ldexp(f, 64);
        se |= (uint16_t)(e - 1 + 16383);
    }
    wr16(p, se);
    wr32(p + 2, (uint32_t)(m >> 32));
    wr32(p + 6, (uint32_t)m);
}

static double get(int fmt, gaddr p)
{
    switch (fmt) {
    case F_DBL: {
        uint64_t b = (uint64_t)rd32(p) << 32 | rd32(p + 4);
        double d;
        memcpy(&d, &b, 8);
        return d;
    }
    case F_SGL: {
        uint32_t b = rd32(p);
        float f;
        memcpy(&f, &b, 4);
        return f;
    }
    case F_INT:
        return (int16_t)rd16(p);
    case F_LONG:
        return (int32_t)rd32(p);
    case F_COMP: {
        int64_t c = (int64_t)((uint64_t)rd32(p) << 32 | rd32(p + 4));
        return c == INT64_MIN ? NAN : (double)c;
    }
    case F_EXT96: {
        /* 68881 layout: sign/exponent, a pad word, then the mantissa. */
        uint8_t b[10];
        gget(b, p, 2);
        gget(b + 2, p + 4, 8);
        gaddr t = tb_NewPtr(10, false, false);
        gput(t, b, 10);
        double v = ext_get(t);
        tb_DisposePtr(t);
        return v;
    }
    default:
        return ext_get(p);
    }
}

static uint16_t raised;

static int64_t to_int(double v, int64_t lo, int64_t hi, int64_t nan_value)
{
    if (isnan(v) || v < (double)lo || v > (double)hi) {
        raised |= X_INVALID;
        return nan_value;
    }
    double r = nearbyint(v);
    if (r != v)
        raised |= X_INEXACT;
    return (int64_t)r;
}

static void put(int fmt, gaddr p, double v)
{
    switch (fmt) {
    case F_DBL: {
        uint64_t b;
        memcpy(&b, &v, 8);
        wr32(p, (uint32_t)(b >> 32));
        wr32(p + 4, (uint32_t)b);
        break;
    }
    case F_SGL: {
        float f = (float)v;
        uint32_t b;
        memcpy(&b, &f, 4);
        wr32(p, b);
        break;
    }
    case F_INT:
        wr16(p, (uint16_t)to_int(v, INT16_MIN, INT16_MAX, INT16_MIN));
        break;
    case F_LONG:
        wr32(p, (uint32_t)to_int(v, INT32_MIN, INT32_MAX, INT32_MIN));
        break;
    case F_COMP: {
        uint64_t c = (uint64_t)to_int(v, -9.2233720368547748e18, 9.2233720368547748e18, INT64_MIN);
        wr32(p, (uint32_t)(c >> 32));
        wr32(p + 4, (uint32_t)c);
        break;
    }
    case F_EXT96: {
        gaddr t = tb_NewPtr(10, false, false);
        ext_put(t, v);
        uint8_t b[10];
        gget(b, t, 10);
        tb_DisposePtr(t);
        gput(p, b, 2);
        wr16(p + 2, 0);
        gput(p + 4, b + 2, 8);
        break;
    }
    default:
        ext_put(p, v);
    }
}

/* ---------------------------------------------------------------------- */
/* Environment: rounding direction in bits 13-14, flags 8-12, halts 0-4. */

static void begin(void)
{
    static const int modes[4] = { FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO };
    fesetround(modes[(env >> 13) & 3]);
    feclearexcept(FE_ALL_EXCEPT);
    raised = 0;
}

static void end(void)
{
    int e = fetestexcept(FE_ALL_EXCEPT);
    if (e & FE_INVALID)
        raised |= X_INVALID;
    if (e & FE_UNDERFLOW)
        raised |= X_UNDERFLOW;
    if (e & FE_OVERFLOW)
        raised |= X_OVERFLOW;
    if (e & FE_DIVBYZERO)
        raised |= X_DIVBYZERO;
    if (e & FE_INEXACT)
        raised |= X_INEXACT;
    env |= (uint16_t)(raised << 8);
    fesetround(FE_TONEAREST);
}

/* ---------------------------------------------------------------------- */
/* Decimal records: sgn (byte), exp (word at 2), sig (Str20 at 4).
 * A decform is style (0 float, 1 fixed) and digits. */

static double dec_get(gaddr d)
{
    int sgn = rd8(d) & 1;
    int16_t exp = (int16_t)rd16(d + 2);
    char sig[32] = { 0 };
    int n = rd8(d + 4);
    if (n > 28)
        n = 28;
    gget(sig, d + 5, (uint32_t)n);
    double v;
    if (sig[0] == 'N')
        v = NAN;
    else if (sig[0] == 'I')
        v = INFINITY;
    else {
        char buf[64];
        snprintf(buf, sizeof buf, "%se%d", n ? sig : "0", exp);
        v = strtod(buf, NULL);
    }
    return sgn ? -v : v;
}

static void dec_put(gaddr d, gaddr form, double v)
{
    int style = (int16_t)rd16(form), digits = (int16_t)rd16(form + 2);
    char sig[64];
    int exp = 0;
    wr8(d, signbit(v) ? 1 : 0);
    wr8(d + 1, 0);
    v = fabs(v);
    if (isnan(v)) {
        strcpy(sig, "N4000000000000000");
    } else if (isinf(v)) {
        strcpy(sig, "I");
    } else if (v == 0) {
        strcpy(sig, "0");
    } else {
        char buf[400];
        if (style == 1) {
            if (digits < 0)
                digits = 0;
            if (digits > 80)
                digits = 80;
            snprintf(buf, sizeof buf, "%.*f", digits, v);
            exp = -digits;
        } else {
            if (digits < 1)
                digits = 1;
            if (digits > 19)
                digits = 19;
            snprintf(buf, sizeof buf, "%.*e", digits - 1, v);
            char *e = strchr(buf, 'e');
            exp = atoi(e + 1) - (digits - 1);
            *e = 0;
        }
        /* Digits only, leading zeros dropped. */
        int k = 0;
        for (char *s = buf; *s; s++)
            if (*s >= '0' && *s <= '9' && (k || *s != '0'))
                sig[k++] = *s;
        sig[k] = 0;
        if (!k)
            strcpy(sig, "0"), exp = 0;
        if (k > 20) {
            /* Too many digits for fixed style: SANE gives '?'. */
            strcpy(sig, "?");
            exp = 0;
        }
    }
    size_t n = strlen(sig);
    wr16(d + 2, (uint16_t)(int16_t)exp);
    wr8(d + 4, (uint8_t)n);
    gput(d + 5, sig, (uint32_t)n);
}

/* ---------------------------------------------------------------------- */
/* FP68K */

static int classify(double v, bool snan)
{
    int c;
    switch (fpclassify(v)) {
    case FP_NAN: c = snan ? 1 : 2; break;
    case FP_INFINITE: c = 3; break;
    case FP_ZERO: c = 4; break;
    case FP_SUBNORMAL: c = 6; break;
    default: c = 5; break;
    }
    return signbit(v) ? -c : c;
}

static void compare(cpu68k *c, double a, double b, bool signal)
{
    uint16_t ccr;
    if (isnan(a) || isnan(b)) {
        ccr = SR_V;
        if (signal)
            raised |= X_INVALID;
    } else if (a < b) {
        ccr = SR_X | SR_N | SR_C;
    } else if (a > b) {
        ccr = 0;
    } else {
        ccr = SR_Z;
    }
    c->sr = (uint16_t)((c->sr & 0xFF00) | ccr);
}

void raw_FP68K(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    uint16_t op = cpu_pop16(c);
    int fmt = (op >> 11) & 7, code = op & 0xFF;
    gaddr dst = 0, src = 0, third = 0;
    /* Destination is pushed last; two-operand calls push a source first and
     * binary-to-decimal a decform before that. */
    int nargs;
    switch (code) {
    case 0x12: case 0x14: case 0x16: case 0x1A: case 0x0D: case 0x0F: case 0x01: case 0x03:
    case 0x05: case 0x07: case 0x15: case 0x17: case 0x19: case 0x1B:
        nargs = 1;
        break;
    case 0x0B:
        nargs = 3;
        break;
    default:
        nargs = 2;
    }
    dst = cpu_pop32(c);
    if (nargs > 1)
        src = cpu_pop32(c);
    if (nargs > 2)
        third = cpu_pop32(c);
    begin();
    switch (code) {
    case 0x00: ext_put(dst, ext_get(dst) + get(fmt, src)); break;
    case 0x02: ext_put(dst, ext_get(dst) - get(fmt, src)); break;
    case 0x04: ext_put(dst, ext_get(dst) * get(fmt, src)); break;
    case 0x06: ext_put(dst, ext_get(dst) / get(fmt, src)); break;
    case 0x08: compare(c, ext_get(dst), get(fmt, src), false); break;
    case 0x0A: compare(c, ext_get(dst), get(fmt, src), true); break;
    case 0x0C: { /* remainder; D0 gets the low 7 bits of the quotient */
        double x = ext_get(dst), y = get(fmt, src);
        double r = remainder(x, y);
        double q = nearbyint((x - r) / y);
        int qi = (int)fmod(fabs(q), 128.0);
        c->d[0] = (c->d[0] & 0xFFFF0000u) | (uint16_t)(q < 0 ? -qi : qi);
        ext_put(dst, r);
        break;
    }
    case 0x0E: ext_put(dst, get(fmt, src)); break;            /* to extended */
    case 0x10: put(fmt, dst, ext_get(src)); break;            /* from extended */
    case 0x12: ext_put(dst, sqrt(ext_get(dst))); break;
    case 0x14: ext_put(dst, nearbyint(ext_get(dst))); break;  /* round to integer */
    case 0x16: ext_put(dst, trunc(ext_get(dst))); break;
    case 0x18: ext_put(dst, ldexp(ext_get(dst), (int16_t)rd16(src))); break; /* scalb */
    case 0x1A: ext_put(dst, logb(ext_get(dst))); break;
    case 0x1C: wr16(dst, (uint16_t)(int16_t)classify(get(fmt, src), false)); break;
    case 0x0D: ext_put(dst, -ext_get(dst)); break;
    case 0x0F: ext_put(dst, fabs(ext_get(dst))); break;
    case 0x11: { /* copy the sign of the source to the destination */
        double s = ext_get(src), d = ext_get(dst);
        ext_put(dst, copysign(d, s));
        break;
    }
    case 0x13: ext_put(dst, nextafter(ext_get(dst), get(fmt, src))); break;
    case 0x09: put(fmt, dst, dec_get(src)); break; /* decimal to binary */
    case 0x0B: dec_put(dst, third, get(fmt, src)); break;
    case 0x01: env = rd16(dst); break;
    case 0x03: wr16(dst, env); break;
    case 0x05: case 0x07: /* halt vector: not used */
        if (code == 0x07)
            wr32(dst, 0);
        break;
    case 0x15: raised |= (uint16_t)(1 << rd16(dst)); break; /* set exception */
    case 0x17: wr16(dst, env); env = 0; break;            /* procedure entry */
    case 0x19: {                                           /* procedure exit */
        uint16_t flags = env & 0x1F00;
        env = rd16(dst) | flags;
        break;
    }
    case 0x1B: { /* test exception: the word holds an exception number */
        int n = (int16_t)rd16(dst) & 0xFF;
        wr8(dst, (env >> (8 + n)) & 1 ? 1 : 0);
        break;
    }
    default:
        cpu_halt(c, "FP68K: unknown operation %04X", op);
        return;
    }
    end();
    cpu_push32(c, ret);
}

/* ---------------------------------------------------------------------- */
/* Elems68K */

void raw_Elems68K(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    uint16_t op = cpu_pop16(c);
    int nargs = op & 0x8000 ? (op & 0x4000 ? 3 : 2) : 1;
    gaddr dst = cpu_pop32(c), a2 = nargs > 1 ? cpu_pop32(c) : 0, a3 = nargs > 2 ? cpu_pop32(c) : 0;
    int code = op & 0xFF;
    begin();
    double x = ext_get(dst), r;
    switch (code) {
    case 0x00: r = log(x); break;
    case 0x02: r = log2(x); break;
    case 0x04: r = log1p(x); break;
    case 0x06: r = log1p(x) / log(2.0); break;
    case 0x08: r = exp(x); break;
    case 0x0A: r = exp2(x); break;
    case 0x0C: r = expm1(x); break;
    case 0x0E: r = expm1(x * log(2.0)); break;
    case 0x10: r = pow(x, (int16_t)rd16(a2)); break;   /* x^i */
    case 0x12: r = pow(x, ext_get(a2)); break;          /* x^y */
    case 0x14: r = pow(1 + ext_get(a3), ext_get(a2)); break;  /* compound(rate, n) */
    case 0x16: {                                               /* annuity(rate, n) */
        double rate = ext_get(a3), n = ext_get(a2);
        r = rate == 0 ? n : (1 - pow(1 + rate, -n)) / rate;
        break;
    }
    case 0x18: r = sin(x); break;
    case 0x1A: r = cos(x); break;
    case 0x1C: r = tan(x); break;
    case 0x1E: r = atan(x); break;
    case 0x20: r = fmod(16807.0 * x, 2147483647.0); break; /* the next random seed */
    default:
        cpu_halt(c, "Elems68K: unknown operation %04X", op);
        return;
    }
    ext_put(dst, r);
    end();
    cpu_push32(c, ret);
}

void sane_reset(void) { env = 0; }

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

/* Microseconds (A193): a 64-bit count, high half in A0 and low in D0. */
void raw_Microseconds(cpu68k *c, uint16_t tw)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t us = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
    c->a[0] = (uint32_t)(us >> 32);
    c->d[0] = (uint32_t)us;
}

/* GetVideoDefault (A080): no slot video parameters to report. */
void raw_GetVideoDefault(cpu68k *c, uint16_t tw) { c->d[0] = 0; }

/* ---------------------------------------------------------------------- */
/* Pack7's decimal string conversions */

/* Dec2Str: a decimal record as text, in the decform's style. */
void tb_ROMlib_Fdec2str(gaddr form, gaddr dec, gaddr out)
{
    double v = dec_get(dec);
    int style = (int16_t)rd16(form), digits = (int16_t)rd16(form + 2);
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
