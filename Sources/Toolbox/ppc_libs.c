/*
 * The system's PowerPC libraries that aren't traps: MathLib (the C math
 * functions and SANE's decimal conversions), StdCLib (MPW's C library:
 * strings, memory, formatted output, setjmp), and the InterfaceLib calls
 * that 68k programs had as glue linked into them (GetIndString, c2pstr,
 * EqualString, MemError ...).
 *
 * The PowerPC calling convention: integer and pointer arguments in r3 to
 * r10, floating-point ones in f1 to f13 (each also taking one GPR slot for
 * a float, two for a double), the rest at 24(r1) on; results in r3 or f1.
 */
#include <math.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "forks.h"
#include "machine.h"
#include "ppc_bridge.h"
#include "qd_internal.h"
#include "tb_internal.h"

double sane_ext_get(gaddr p);
void sane_ext_put(gaddr p, double v);
double sane_dec_get(gaddr d);
void sane_dec_put(gaddr d, gaddr form, double v);

#define R(n) (c->gpr[3 + (n)])
#define F(n) ppc_fpr(c, 1 + (n))
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; } while (0)
#define RETURN_F(v) do { ppc_set_fpr(c, 1, (v)); c->pc = c->lr; } while (0)

/* Argument word n of a call (GPR slots, then the parameter area). */
static uint32_t arg_word(const ppc_cpu *c, unsigned n)
{
    return n < 8 ? c->gpr[3 + n] : rd32(c->gpr[1] + 24 + 4 * n);
}

static double rd_double(gaddr p)
{
    uint64_t bits = (uint64_t)rd32(p) << 32 | rd32(p + 4);
    double d;
    memcpy(&d, &bits, 8);
    return d;
}

static void wr_double(gaddr p, double d)
{
    uint64_t bits;
    memcpy(&bits, &d, 8);
    wr32(p, (uint32_t)(bits >> 32));
    wr32(p + 4, (uint32_t)bits);
}

static size_t gstrlen(gaddr s)
{
    size_t n = 0;
    while (rd8(s + (gaddr)n))
        n++;
    return n;
}

/* ---------------------------------------------------------------------- */
/* MathLib */

#define UNARY(name, expr) \
    static void m_##name(ppc_cpu *c) { double x = F(0); (void)x; RETURN_F(expr); }
#define BINARY(name, expr) \
    static void m_##name(ppc_cpu *c) { double x = F(0), y = F(1); (void)y; RETURN_F(expr); }

UNARY(sin, sin(x))
UNARY(cos, cos(x))
UNARY(tan, tan(x))
UNARY(asin, asin(x))
UNARY(acos, acos(x))
UNARY(atan, atan(x))
UNARY(sinh, sinh(x))
UNARY(cosh, cosh(x))
UNARY(tanh, tanh(x))
UNARY(asinh, asinh(x))
UNARY(acosh, acosh(x))
UNARY(atanh, atanh(x))
UNARY(exp, exp(x))
UNARY(exp2, exp2(x))
UNARY(expm1, expm1(x))
UNARY(log, log(x))
UNARY(log2, log2(x))
UNARY(log10, log10(x))
UNARY(log1p, log1p(x))
UNARY(logb, logb(x))
UNARY(sqrt, sqrt(x))
UNARY(cbrt, cbrt(x))
UNARY(fabs, fabs(x))
UNARY(floor, floor(x))
UNARY(ceil, ceil(x))
UNARY(trunc, trunc(x))
UNARY(round, round(x))
UNARY(rint, rint(x))
UNARY(nearbyint, nearbyint(x))
UNARY(erf, erf(x))
UNARY(erfc, erfc(x))
UNARY(gamma, tgamma(x))
UNARY(lgamma, lgamma(x))
BINARY(pow, pow(x, y))
BINARY(atan2, atan2(x, y))
BINARY(fmod, fmod(x, y))
BINARY(hypot, hypot(x, y))
BINARY(fmin, fmin(x, y))
BINARY(fmax, fmax(x, y))
BINARY(fdim, fdim(x, y))
BINARY(remainder, remainder(x, y))
BINARY(copysign, copysign(x, y))
BINARY(nextafterd, nextafter(x, y))
/* compound(rate, periods) = (1 + rate)^periods; annuity = (1 - (1 + rate)^-periods) / rate */
BINARY(compound, pow(1 + x, y))
BINARY(annuity, x == 0 ? y : (1 - pow(1 + x, -y)) / x)

/* The double after a double argument: its GPR slots are 0 and 1, so an int
 * or pointer after it is in r5. */
static void m_ldexp(ppc_cpu *c) { RETURN_F(ldexp(F(0), (int32_t)R(2))); }
static void m_scalb(ppc_cpu *c) { RETURN_F(scalbn(F(0), (int32_t)R(2))); }
static void m_frexp(ppc_cpu *c)
{
    int e;
    double m = frexp(F(0), &e);
    if (R(2))
        wr32(R(2), (uint32_t)e);
    RETURN_F(m);
}
static void m_modf(ppc_cpu *c)
{
    double ip;
    double f = modf(F(0), &ip);
    if (R(2))
        wr_double(R(2), ip);
    RETURN_F(f);
}
static void m_remquo(ppc_cpu *c)
{
    int q;
    double r = remquo(F(0), F(1), &q);
    if (R(4))
        wr32(R(4), (uint32_t)q);
    RETURN_F(r);
}
static void m_rinttol(ppc_cpu *c) { RETURN((int32_t)lrint(F(0))); }
static void m_roundtol(ppc_cpu *c) { RETURN((int32_t)lround(F(0))); }
static void m_inf(ppc_cpu *c) { RETURN_F(INFINITY); }
static void m_nan(ppc_cpu *c) { RETURN_F(NAN); }
static void m_isnand(ppc_cpu *c) { RETURN(isnan(F(0)) ? 1 : 0); }
static void m_isfinited(ppc_cpu *c) { RETURN(isfinite(F(0)) ? 1 : 0); }
static void m_isnormald(ppc_cpu *c) { RETURN(isnormal(F(0)) ? 1 : 0); }
static void m_signbitd(ppc_cpu *c) { RETURN(signbit(F(0)) ? 1 : 0); }
/* MathLib's classes: FP_SNAN 0, FP_QNAN 1, FP_INFINITE 2, FP_ZERO 3,
 * FP_NORMAL 4, FP_SUBNORMAL 5. */
static void m_fpclassifyd(ppc_cpu *c)
{
    double x = F(0);
    int k = isnan(x) ? 1 : isinf(x) ? 2 : x == 0 ? 3 : isnormal(x) ? 4 : 5;
    RETURN(k);
}
/* randomx(double *x): the next of the Lehmer sequence, as SANE has it. */
static void m_randomx(ppc_cpu *c)
{
    double x = rd_double(R(0));
    x = fmod(16807.0 * x, 2147483647.0);
    wr_double(R(0), x);
    RETURN_F(x);
}
/* The floating-point environment: round to nearest, no exceptions kept. */
static void m_zero(ppc_cpu *c) { RETURN(0); }
static void m_x80tod(ppc_cpu *c) { RETURN_F(sane_ext_get(R(0))); }
static void m_dtox80(ppc_cpu *c)
{
    sane_ext_put(R(1), rd_double(R(0)));
    c->pc = c->lr;
}
/* ldtox80(const long double *, extended80 *): MathLib's long double is a
 * pair of doubles; their sum is close enough. */
static void m_ldtox80(ppc_cpu *c)
{
    sane_ext_put(R(1), rd_double(R(0)) + rd_double(R(0) + 8));
    c->pc = c->lr;
}
static void m_x80told(ppc_cpu *c)
{
    wr_double(R(1), sane_ext_get(R(0)));
    wr_double(R(1) + 8, 0);
    c->pc = c->lr;
}

/* num2dec(const decform *f, double x, decimal *d): x takes r4 and r5. */
static void m_num2dec(ppc_cpu *c)
{
    sane_dec_put(R(3), R(0), F(0));
    c->pc = c->lr;
}
static void m_dec2num(ppc_cpu *c) { RETURN_F(sane_dec_get(R(0))); }
/* dec2str(const decform *f, const decimal *d, char *s): a C string. */
static void m_dec2str(ppc_cpu *c)
{
    gaddr tmp = tb_NewPtr(256, false, true);
    if (tmp) {
        tb_ROMlib_Fdec2str(R(0), R(1), tmp);
        uint8_t n = rd8(tmp);
        ppc_copy(R(2), tmp + 1, n);
        wr8(R(2) + n, 0);
        tb_DisposePtr(tmp);
    }
    c->pc = c->lr;
}
/* str2dec(const char *s, short *ix, decimal *d, short *vp) */
static void m_str2dec(ppc_cpu *c)
{
    gaddr valid = tb_NewPtr(2, true, true);
    tb_ROMlib_Fcstr2dec(R(0), R(1), R(2), valid);
    if (R(3))
        wr16(R(3), rd8(valid));
    tb_DisposePtr(valid);
    c->pc = c->lr;
}

/* ---------------------------------------------------------------------- */
/* StdCLib */

static void s_strlen(ppc_cpu *c) { RETURN(gstrlen(R(0))); }
static void s_strcpy(ppc_cpu *c)
{
    gaddr d = R(0), s = R(1);
    size_t n = gstrlen(s);
    memmove(gptr(d), gptr(s), n + 1);
    RETURN(d);
}
static void s_strncpy(ppc_cpu *c)
{
    gaddr d = R(0), s = R(1);
    uint32_t n = R(2), i = 0;
    for (; i < n && rd8(s + i); i++)
        wr8(d + i, rd8(s + i));
    for (; i < n; i++)
        wr8(d + i, 0);
    RETURN(d);
}
static void s_strcat(ppc_cpu *c)
{
    gaddr d = R(0), s = R(1);
    size_t at = gstrlen(d), n = gstrlen(s);
    memmove(gptr(d + (gaddr)at), gptr(s), n + 1);
    RETURN(d);
}
static void s_strncat(ppc_cpu *c)
{
    gaddr d = R(0), s = R(1);
    size_t at = gstrlen(d), i = 0;
    for (; i < R(2) && rd8(s + (gaddr)i); i++)
        wr8(d + (gaddr)(at + i), rd8(s + (gaddr)i));
    wr8(d + (gaddr)(at + i), 0);
    RETURN(d);
}
static int gstrncmp(gaddr a, gaddr b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int x = rd8(a + (gaddr)i), y = rd8(b + (gaddr)i);
        if (x != y)
            return x < y ? -1 : 1;
        if (!x)
            return 0;
    }
    return 0;
}
static void s_strcmp(ppc_cpu *c) { RETURN((int32_t)gstrncmp(R(0), R(1), SIZE_MAX)); }
static void s_strncmp(ppc_cpu *c) { RETURN((int32_t)gstrncmp(R(0), R(1), R(2))); }
static void s_strchr(ppc_cpu *c)
{
    gaddr s = R(0);
    uint8_t ch = (uint8_t)R(1);
    for (;; s++) {
        if (rd8(s) == ch)
            RETURN(s);
        else if (!rd8(s))
            break;
        else
            continue;
        return;
    }
    RETURN(0);
}
static void s_strrchr(ppc_cpu *c)
{
    gaddr s = R(0), hit = 0;
    uint8_t ch = (uint8_t)R(1);
    for (;; s++) {
        if (rd8(s) == ch)
            hit = s;
        if (!rd8(s))
            break;
    }
    RETURN(hit);
}
static void s_strstr(ppc_cpu *c)
{
    gaddr h = R(0), n = R(1);
    size_t nl = gstrlen(n), hl = gstrlen(h);
    for (size_t i = 0; i + nl <= hl; i++)
        if (!memcmp(gptr(h + (gaddr)i), gptr(n), nl))
            RETURN(h + (gaddr)i);
    RETURN(0);
}
static void s_memcpy(ppc_cpu *c)
{
    if (R(2))
        memmove(gptr(R(0)), gptr(R(1)), R(2));
    RETURN(R(0));
}
static void s_memset(ppc_cpu *c)
{
    if (R(2))
        memset(gptr(R(0)), (int)(R(1) & 0xFF), R(2));
    RETURN(R(0));
}
static void s_memcmp(ppc_cpu *c)
{
    int r = R(2) ? memcmp(gptr(R(0)), gptr(R(1)), R(2)) : 0;
    RETURN((int32_t)(r < 0 ? -1 : r > 0));
}
static void s_memchr(ppc_cpu *c)
{
    for (uint32_t i = 0; i < R(2); i++)
        if (rd8(R(0) + i) == (uint8_t)R(1))
            RETURN(R(0) + i);
    RETURN(0);
}
/* malloc and friends: Memory Manager pointers with the size in front. */
static void s_malloc(ppc_cpu *c) { RETURN(tb_NewPtr((int32_t)R(0), false, false)); }
static void s_calloc(ppc_cpu *c) { RETURN(tb_NewPtr((int32_t)(R(0) * R(1)), false, true)); }
static void s_free(ppc_cpu *c)
{
    if (R(0))
        tb_DisposePtr(R(0));
    c->pc = c->lr;
}
static void s_realloc(ppc_cpu *c)
{
    gaddr p = R(0);
    uint32_t n = R(1);
    gaddr q = tb_NewPtr((int32_t)n, false, false);
    if (q && p) {
        uint32_t old = (uint32_t)tb_GetPtrSize(p);
        ppc_copy(q, p, old < n ? old : n);
        tb_DisposePtr(p);
    }
    RETURN(q);
}
static void s_abs(ppc_cpu *c) { RETURN((uint32_t)labs((int32_t)R(0))); }
static uint32_t g_strtol(ppc_cpu *c, bool is_unsigned)
{
    char buf[64];
    size_t n = 0;
    gaddr s = R(0);
    while (n < sizeof buf - 1 && rd8(s + (gaddr)n))
        buf[n] = (char)rd8(s + (gaddr)n), n++;
    buf[n] = 0;
    char *end;
    uint32_t v = is_unsigned ? (uint32_t)strtoul(buf, &end, (int)R(2)) : (uint32_t)(int32_t)strtol(buf, &end, (int)R(2));
    if (R(1))
        wr32(R(1), s + (gaddr)(end - buf));
    return v;
}
static void s_strtol(ppc_cpu *c) { RETURN(g_strtol(c, false)); }
static void s_strtoul(ppc_cpu *c) { RETURN(g_strtol(c, true)); }
static void s_atoi(ppc_cpu *c)
{
    c->gpr[5] = 10;
    c->gpr[4] = 0;
    RETURN(g_strtol(c, false));
}
static void s_atof(ppc_cpu *c)
{
    char buf[128];
    size_t n = 0;
    while (n < sizeof buf - 1 && rd8(R(0) + (gaddr)n))
        buf[n] = (char)rd8(R(0) + (gaddr)n), n++;
    buf[n] = 0;
    RETURN_F(atof(buf));
}
static void s_toupper(ppc_cpu *c)
{
    int32_t ch = (int32_t)R(0);
    RETURN(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
}
static void s_tolower(ppc_cpu *c)
{
    int32_t ch = (int32_t)R(0);
    RETURN(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
}
/* The character classes (ASCII; the rest of MacRoman is none of them). */
#define CTYPE(name, test) \
    static void s_##name(ppc_cpu *c) { int32_t ch = (int32_t)R(0); RETURN(ch >= 0 && ch < 128 && (test) ? 1 : 0); }
CTYPE(isalnum, (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'))
CTYPE(isalpha, (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'))
CTYPE(isdigit, ch >= '0' && ch <= '9')
CTYPE(isxdigit, (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f'))
CTYPE(isspace, ch == ' ' || (ch >= 9 && ch <= 13))
CTYPE(isupper, ch >= 'A' && ch <= 'Z')
CTYPE(islower, ch >= 'a' && ch <= 'z')
CTYPE(ispunct, ch > 32 && ch < 127 && !((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')))
CTYPE(isprint, ch >= 32 && ch < 127)
CTYPE(isgraph, ch > 32 && ch < 127)
CTYPE(iscntrl, ch < 32 || ch == 127)
CTYPE(isascii, 1)
#undef CTYPE

/* sscanf(s, format, ...): integers, floating point, strings and characters
 * into the pointers that follow; the count of conversions. */
static void s_sscanf(ppc_cpu *c)
{
    char in[1024];
    size_t n = 0;
    for (gaddr p = R(0); n < sizeof in - 1 && rd8(p); p++)
        in[n++] = (char)rd8(p);
    in[n] = 0;
    const char *at = in;
    unsigned w = 2, done = 0;
    for (gaddr f = R(1); rd8(f); f++) {
        char fc = (char)rd8(f);
        if (fc == ' ' || (fc >= 9 && fc <= 13)) {
            while (*at == ' ' || (*at >= 9 && *at <= 13))
                at++;
            continue;
        }
        if (fc != '%') {
            if (*at != fc)
                break;
            at++;
            continue;
        }
        f++;
        bool skip = false, longer = false, shorter = false;
        int width = 0;
        if (rd8(f) == '*')
            skip = true, f++;
        while (rd8(f) >= '0' && rd8(f) <= '9')
            width = width * 10 + (rd8(f++) - '0');
        while (rd8(f) == 'l' || rd8(f) == 'h' || rd8(f) == 'L') {
            if (rd8(f) == 'h')
                shorter = true;
            else
                longer = true;
            f++;
        }
        char conv = (char)rd8(f);
        if (conv == '%') {
            if (*at != '%')
                break;
            at++;
            continue;
        }
        if (conv == 'n') {
            if (!skip)
                wr32(arg_word(c, w++), (uint32_t)(at - in));
            continue;
        }
        if (conv != 'c')
            while (*at == ' ' || (*at >= 9 && *at <= 13))
                at++;
        if (!*at)
            break;
        char *end = NULL;
        if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
            int base = conv == 'x' || conv == 'X' ? 16 : conv == 'o' ? 8 : conv == 'i' ? 0 : 10;
            long v = strtol(at, &end, base);
            if (end == at)
                break;
            if (!skip) {
                gaddr dst = arg_word(c, w++);
                if (shorter)
                    wr16(dst, (uint16_t)v);
                else
                    wr32(dst, (uint32_t)v);
                done++;
            }
        } else if (conv == 'f' || conv == 'e' || conv == 'g' || conv == 'E' || conv == 'G') {
            double v = strtod(at, &end);
            if (end == at)
                break;
            if (!skip) {
                gaddr dst = arg_word(c, w++);
                if (longer)
                    wr_double(dst, v);
                else {
                    float fv = (float)v;
                    uint32_t bits;
                    memcpy(&bits, &fv, 4);
                    wr32(dst, bits);
                }
                done++;
            }
        } else if (conv == 's') {
            const char *b = at;
            while (*at && *at != ' ' && !(*at >= 9 && *at <= 13) && (!width || at - b < width))
                at++;
            if (!skip) {
                gaddr dst = arg_word(c, w++);
                gput(dst, b, (uint32_t)(at - b));
                wr8(dst + (gaddr)(at - b), 0);
                done++;
            }
            continue;
        } else if (conv == 'c') {
            int k = width ? width : 1;
            if (!skip) {
                gaddr dst = arg_word(c, w++);
                for (int i = 0; i < k && at[i]; i++)
                    wr8(dst + (gaddr)i, (uint8_t)at[i]);
                done++;
            }
            for (int i = 0; i < k && *at; i++)
                at++;
            continue;
        } else {
            break;
        }
        at = end;
    }
    RETURN(done);
}

/* The cursor devices' calls: no devices besides the mouse. */
static void g_CrsrDevNextDevice(ppc_cpu *c)
{
    if (R(0))
        wr32(R(0), 0);
    RETURN(0);
}

static uint32_t rand_state = 1;
static void s_rand(ppc_cpu *c)
{
    rand_state = rand_state * 1103515245u + 12345u;
    RETURN((rand_state >> 16) & 0x7FFF);
}
static void s_srand(ppc_cpu *c)
{
    rand_state = R(0);
    c->pc = c->lr;
}
/* time_t is seconds since 1904, as the Mac keeps time. */
static void s_time(ppc_cpu *c)
{
    uint32_t t = rd32(LM_Time);
    if (R(0))
        wr32(R(0), t);
    RETURN(t);
}
static void s_clock(ppc_cpu *c) { RETURN(rd32(LM_Ticks)); }
static void s_exit(ppc_cpu *c)
{
    rm_update_all();
    cpu_halt(g_cpu, "ExitToShell (exit %d)", (int)(int32_t)R(0));
    ppc_halt(c, "ExitToShell (exit %d)", (int)(int32_t)R(0));
}
static void s_getenv(ppc_cpu *c) { RETURN(0); }

/* setjmp/longjmp: the nonvolatile registers, the stack and the return. */
static void s_setjmp(ppc_cpu *c)
{
    gaddr b = R(0);
    wr32(b, c->lr);
    wr32(b + 4, c->gpr[1]);
    wr32(b + 8, c->gpr[2]);
    wr32(b + 12, c->cr);
    for (int i = 13; i < 32; i++)
        wr32(b + 16 + 4 * (gaddr)(i - 13), c->gpr[i]);
    for (int i = 14; i < 32; i++) {
        wr32(b + 92 + 8 * (gaddr)(i - 14), (uint32_t)(c->fpr[i] >> 32));
        wr32(b + 96 + 8 * (gaddr)(i - 14), (uint32_t)c->fpr[i]);
    }
    RETURN(0);
}
static void s_longjmp(ppc_cpu *c)
{
    gaddr b = R(0);
    uint32_t v = R(1) ? R(1) : 1;
    c->gpr[1] = rd32(b + 4);
    c->gpr[2] = rd32(b + 8);
    c->cr = rd32(b + 12);
    for (int i = 13; i < 32; i++)
        c->gpr[i] = rd32(b + 16 + 4 * (gaddr)(i - 13));
    for (int i = 14; i < 32; i++)
        c->fpr[i] = (uint64_t)rd32(b + 92 + 8 * (gaddr)(i - 14)) << 32 | rd32(b + 96 + 8 * (gaddr)(i - 14));
    c->gpr[3] = v;
    c->pc = rd32(b);
}

/* Formatted output: the format's arguments are words from `first` on (a
 * double takes two). */
static size_t format(const ppc_cpu *c, gaddr fmt, unsigned first, char *out, size_t outsize)
{
    size_t o = 0;
    unsigned w = first;
#define EMIT(str) do { size_t l_ = strlen(str); if (o + l_ >= outsize) l_ = outsize - 1 - o; memcpy(out + o, str, l_); o += l_; } while (0)
    for (gaddr p = fmt; rd8(p) && o < outsize - 1; p++) {
        char ch = (char)rd8(p);
        if (ch != '%') {
            out[o++] = ch;
            continue;
        }
        char spec[32] = "%";
        size_t sl = 1;
        p++;
        while (strchr("-+ #0", (char)rd8(p)) && sl < 20)
            spec[sl++] = (char)rd8(p++);
        for (int part = 0; part < 2; part++) {
            if (part == 1) {
                if (rd8(p) != '.')
                    break;
                spec[sl++] = '.';
                p++;
            }
            if (rd8(p) == '*') {
                sl += (size_t)snprintf(spec + sl, sizeof spec - sl, "%d", (int)(int32_t)arg_word(c, w++));
                p++;
            }
            while (rd8(p) >= '0' && rd8(p) <= '9' && sl < 28)
                spec[sl++] = (char)rd8(p++);
        }
        bool is_short = false;
        while (strchr("hlLqj", (char)rd8(p)) && rd8(p)) {
            if (rd8(p) == 'h')
                is_short = true;
            p++;
        }
        char conv = (char)rd8(p);
        char piece[512];
        piece[0] = 0;
        spec[sl] = conv;
        spec[sl + 1] = 0;
        switch (conv) {
        case 'd': case 'i': {
            int32_t v = (int32_t)arg_word(c, w++);
            if (is_short)
                v = (int16_t)v;
            snprintf(piece, sizeof piece, spec, v);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': case 'c': {
            uint32_t v = arg_word(c, w++);
            if (is_short)
                v &= 0xFFFF;
            snprintf(piece, sizeof piece, spec, v);
            break;
        }
        case 'p':
            snprintf(piece, sizeof piece, "%08X", arg_word(c, w++));
            break;
        case 's': case 'P': {
            gaddr s = arg_word(c, w++);
            char str[256];
            size_t n = 0;
            if (conv == 'P' || (s && 0)) { /* a Pascal string (MPW's %P) */
                n = rd8(s);
                memcpy(str, gptr(s + 1), n);
                spec[sl] = 's';
            } else if (s) {
                while (n < sizeof str - 1 && rd8(s + (gaddr)n))
                    str[n] = (char)rd8(s + (gaddr)n), n++;
            }
            str[n] = 0;
            snprintf(piece, sizeof piece, spec, str);
            break;
        }
        case 'f': case 'e': case 'E': case 'g': case 'G': {
            uint64_t bits = (uint64_t)arg_word(c, w) << 32 | arg_word(c, w + 1);
            w += 2;
            double d;
            memcpy(&d, &bits, 8);
            snprintf(piece, sizeof piece, spec, d);
            break;
        }
        case 'n':
            w++;
            break;
        case '%':
            strcpy(piece, "%");
            break;
        default:
            if (!conv)
                p--;
            break;
        }
        EMIT(piece);
    }
#undef EMIT
    out[o] = 0;
    return o;
}

static void s_sprintf(ppc_cpu *c)
{
    char buf[4096];
    size_t n = format(c, R(1), 2, buf, sizeof buf);
    gput(R(0), buf, (uint32_t)n + 1);
    RETURN(n);
}
/* vsprintf(s, fmt, va_list): the va_list points at the argument words. */
static size_t vformat(gaddr fmt, gaddr va, char *out, size_t outsize)
{
    /* A pretend call whose argument words are all in memory. */
    ppc_cpu v;
    memset(&v, 0, sizeof v);
    v.gpr[1] = va - 24 - 4 * 8; /* word 8 of the parameter area is va */
    for (int i = 0; i < 8; i++)
        v.gpr[3 + i] = rd32(va + 4 * (gaddr)i);
    /* Words 0-7 come from the registers (copied above), the rest from memory
     * right after: arg_word(n >= 8) reads 24 + 4n past r1 = va + 4(n - 8). */
    v.gpr[1] = va - 24;
    return format(&v, fmt, 0, out, outsize);
}
static void s_vsprintf(ppc_cpu *c)
{
    char buf[4096];
    size_t n = vformat(R(1), R(2), buf, sizeof buf);
    gput(R(0), buf, (uint32_t)n + 1);
    RETURN(n);
}
/* Console output: to the log. */
static void s_printf(ppc_cpu *c)
{
    char buf[4096];
    size_t n = format(c, R(0), 1, buf, sizeof buf);
    LOGF(LOG_UNIMPL, "printf: %s", buf);
    RETURN(n);
}
static void s_fprintf(ppc_cpu *c)
{
    char buf[4096];
    size_t n = format(c, R(1), 2, buf, sizeof buf);
    LOGF(LOG_UNIMPL, "fprintf: %s", buf);
    RETURN(n);
}
static void s_puts(ppc_cpu *c) { RETURN(0); }
static void s_fflush(ppc_cpu *c) { RETURN(0); }

/* qsort(base, n, size, compare): compare is PowerPC code. */
static gaddr qsort_fn;
static uint32_t qsort_size;
static gaddr qsort_tmp;
static void s_qsort(ppc_cpu *c)
{
    gaddr base = R(0);
    uint32_t n = R(1), size = R(2);
    qsort_fn = R(3);
    qsort_size = size;
    gaddr tmp = tb_NewPtr((int32_t)size, false, true);
    qsort_tmp = tmp;
    /* Insertion sort: simple, stable, and calls compare in guest code. */
    for (uint32_t i = 1; i < n && !c->halted; i++) {
        ppc_copy(tmp, base + i * size, size);
        uint32_t j = i;
        while (j > 0) {
            uint32_t args[2] = { base + (j - 1) * size, tmp };
            int32_t r = (int32_t)ppc_call(qsort_fn, args, 2);
            if (r <= 0 || g_cpu->halted)
                break;
            ppc_copy(base + j * size, base + (j - 1) * size, size);
            j--;
        }
        ppc_copy(base + j * size, tmp, size);
    }
    tb_DisposePtr(tmp);
    c->pc = c->lr;
}

/* ---------------------------------------------------------------------- */
/* InterfaceLib glue */

/* Pascal and C strings */
static void g_c2pstr(ppc_cpu *c)
{
    gaddr s = R(0);
    size_t n = gstrlen(s);
    if (n > 255)
        n = 255;
    memmove(gptr(s + 1), gptr(s), n);
    wr8(s, (uint8_t)n);
    RETURN(s);
}
static void g_p2cstr(ppc_cpu *c)
{
    gaddr s = R(0);
    uint8_t n = rd8(s);
    memmove(gptr(s), gptr(s + 1), n);
    wr8(s + n, 0);
    RETURN(s);
}
/* c2pstrcpy(dst, src) / CopyCStringToPascal(src, dst) */
static void c_to_p(gaddr src, gaddr dst)
{
    size_t n = gstrlen(src);
    if (n > 255)
        n = 255;
    memmove(gptr(dst + 1), gptr(src), n);
    wr8(dst, (uint8_t)n);
}
static void p_to_c(gaddr src, gaddr dst)
{
    uint8_t n = rd8(src);
    memmove(gptr(dst), gptr(src + 1), n);
    wr8(dst + n, 0);
}
static void g_c2pstrcpy(ppc_cpu *c) { c_to_p(R(1), R(0)); c->pc = c->lr; }
static void g_p2cstrcpy(ppc_cpu *c) { p_to_c(R(1), R(0)); c->pc = c->lr; }
static void g_CopyCStringToPascal(ppc_cpu *c) { c_to_p(R(0), R(1)); c->pc = c->lr; }
static void g_CopyPascalStringToC(ppc_cpu *c) { p_to_c(R(0), R(1)); c->pc = c->lr; }

/* GetIndString(theString, strListID, index) */
static void g_GetIndString(ppc_cpu *c)
{
    gaddr out = R(0);
    int16_t index = (int16_t)R(2);
    wr8(out, 0);
    gaddr h = tb_GetResource((int32_t)fourcc("STR#"), (int16_t)R(1));
    if (h && rd32(h) && index >= 1) {
        gaddr p = rd32(h);
        uint16_t count = rd16(p);
        if (index <= count) {
            p += 2;
            for (int i = 1; i < index; i++)
                p += 1u + rd8(p);
            ppc_copy(out, p, 1u + rd8(p));
        }
    }
    c->pc = c->lr;
}

/* The string comparisons, through _CmpString, _RelString and _UprString:
 * bit 9 of the trap ignores diacriticals, bit 10 respects case. */
static uint32_t compare_trap(uint16_t trap, gaddr a, gaddr b, bool case_sens, bool diac_sens)
{
    ppc_trap_begin();
    ppc_trap_set(8, a + 1);
    ppc_trap_set(9, b + 1);
    ppc_trap_set(0, (uint32_t)rd8(a) << 16 | rd8(b));
    ppc_trap((uint16_t)(trap | (case_sens ? 0x0400 : 0) | (diac_sens ? 0 : 0x0200)));
    uint32_t r = ppc_trap_get(0);
    ppc_trap_end();
    return r;
}
static void g_EqualString(ppc_cpu *c) { RETURN(compare_trap(0xA03C, R(0), R(1), R(2) & 0xFF, R(3) & 0xFF) == 0); }
static void g_RelString(ppc_cpu *c)
{
    RETURN((int32_t)(int16_t)compare_trap(0xA050, R(0), R(1), R(2) & 0xFF, R(3) & 0xFF));
}
static void g_IUEqualString(ppc_cpu *c) { RETURN(compare_trap(0xA03C, R(0), R(1), false, true) == 0 ? 0 : 1); }
static void g_IUCompString(ppc_cpu *c)
{
    RETURN((int32_t)(int16_t)compare_trap(0xA050, R(0), R(1), false, true));
}
static void g_UpperString(ppc_cpu *c)
{
    gaddr s = R(0);
    ppc_trap_begin();
    ppc_trap_set(8, s + 1);
    ppc_trap_set(0, rd8(s));
    ppc_trap((uint16_t)(0xA054 | ((R(1) & 0xFF) ? 0 : 0x0200)));
    ppc_trap_end();
    c->pc = c->lr;
}

/* Case in MacRoman: ASCII, and the accented letters' pairs. */
static const uint8_t roman_pairs[][2] = {
    { 0x80, 0x8A }, { 0x81, 0x8C }, { 0x82, 0x8D }, { 0x83, 0x8E }, { 0x84, 0x96 }, { 0x85, 0x9A },
    { 0x86, 0x9F }, { 0xCB, 0x88 }, { 0xCC, 0x8B }, { 0xCD, 0x9B }, { 0xCE, 0xCF }, { 0xAE, 0xBE },
    { 0xAF, 0xBF }, { 0xE5, 0x89 }, { 0xE6, 0x90 }, { 0xE7, 0x87 }, { 0xE8, 0x91 }, { 0xE9, 0x8F },
    { 0xEA, 0x92 }, { 0xEB, 0x94 }, { 0xEC, 0x95 }, { 0xED, 0x93 }, { 0xEE, 0x97 }, { 0xEF, 0x99 },
    { 0xF1, 0x98 }, { 0xF2, 0x9C }, { 0xF3, 0x9E }, { 0xF4, 0x9D }, { 0xD9, 0xD8 },
};

static uint8_t roman_case(uint8_t ch, bool upper)
{
    if (upper && ch >= 'a' && ch <= 'z')
        return (uint8_t)(ch - 32);
    if (!upper && ch >= 'A' && ch <= 'Z')
        return (uint8_t)(ch + 32);
    for (unsigned i = 0; i < sizeof roman_pairs / sizeof roman_pairs[0]; i++)
        if (roman_pairs[i][upper ? 1 : 0] == ch)
            return roman_pairs[i][upper ? 0 : 1];
    return ch;
}

static void change_case(gaddr text, int32_t len, bool upper)
{
    for (int32_t i = 0; i < len; i++)
        wr8(text + (gaddr)i, roman_case(rd8(text + (gaddr)i), upper));
}
/* UpperText(text, len) / UppercaseText(text, len, script) and the lower ones */
static void g_UpperText(ppc_cpu *c) { change_case(R(0), (int16_t)R(1), true); c->pc = c->lr; }
static void g_LowerText(ppc_cpu *c) { change_case(R(0), (int16_t)R(1), false); c->pc = c->lr; }

/* Low-memory queue headers and globals that have accessor functions. */
static void g_GetEvQHdr(ppc_cpu *c) { RETURN(LM_EventQueue); }
static void g_GetVCBQHdr(ppc_cpu *c) { RETURN(LM_VCBQHdr); }
static void g_GetDrvQHdr(ppc_cpu *c) { RETURN(LM_DrvQHdr); }
static void g_GetFSQHdr(ppc_cpu *c) { RETURN(LM_FSQHdr); }
static void g_GetVBLQHdr(ppc_cpu *c) { RETURN(LM_VBLQueue); }
static void g_GetApplLimit(ppc_cpu *c) { RETURN(rd32(LM_ApplLimit)); }
static void g_GetGrayRgn(ppc_cpu *c) { RETURN(rd32(LM_GrayRgn)); }
static void g_GetAppFont(ppc_cpu *c) { RETURN((int32_t)(int16_t)rd16(LM_ApFontID)); }
static void g_GetSysFont(ppc_cpu *c) { RETURN((int32_t)(int16_t)rd16(LM_SysFontFam)); }
static void g_GetDefFontSize(ppc_cpu *c) { RETURN(12); }
/* GetTime(DateTimeRec *) */
static void g_GetTime(ppc_cpu *c)
{
    tb_SecondsToDate(rd32(LM_Time), R(0));
    c->pc = c->lr;
}
/* Microseconds(UnsignedWide *): _Microseconds leaves the high half in A0. */
static void g_Microseconds(ppc_cpu *c)
{
    ppc_trap_begin();
    ppc_trap(0xA193);
    uint32_t hi = ppc_trap_get(8), lo = ppc_trap_get(0);
    ppc_trap_end();
    if (R(0)) {
        wr32(R(0), hi);
        wr32(R(0) + 4, lo);
    }
    c->pc = c->lr;
}
/* ReadLocation(MachineLocation *): Greenwich, no daylight saving. */
static void g_ReadLocation(ppc_cpu *c)
{
    if (R(0))
        gmemset(R(0), 0, 12);
    c->pc = c->lr;
}
/* Control/Status(refNum, csCode, csParamPtr) and KillIO(refNum): the
 * device calls with a parameter block made here. */
static int16_t device_call(int16_t (*fn)(gaddr, bool), int16_t ref, int16_t code, gaddr params)
{
    gaddr pb = tb_NewPtr(80, true, true);
    wr16(pb + 24, (uint16_t)ref);  /* ioCRefNum */
    wr16(pb + 26, (uint16_t)code); /* csCode */
    if (params)
        ppc_copy(pb + 28, params, 22); /* csParam */
    int16_t e = fn(pb, false);
    if (params)
        ppc_copy(params, pb + 28, 22);
    tb_DisposePtr(pb);
    return e;
}
static void g_Control(ppc_cpu *c) { RETURN((int32_t)device_call(tb_PBControl, (int16_t)R(0), (int16_t)R(1), R(2))); }
static void g_Status(ppc_cpu *c) { RETURN((int32_t)device_call(tb_PBStatus, (int16_t)R(0), (int16_t)R(1), R(2))); }
static void g_KillIO(ppc_cpu *c) { RETURN((int32_t)device_call(tb_PBKillIO, (int16_t)R(0), 0, 0)); }
/* CountDITL(dialog): the item list's count is one less than the number. */
static void g_CountDITL(ppc_cpu *c)
{
    gaddr items = R(0) ? rd32(R(0) + DialogRecord_items_off) : 0;
    RETURN(items && rd32(items) ? (int32_t)(int16_t)rd16(rd32(items)) + 1 : 0);
}
/* StdFilterProc(dialog, event, itemHit): ModalDialog does the standard
 * filtering itself, so here nothing is handled. */
static void g_StdFilterProc(ppc_cpu *c) { RETURN(0); }

/* A one-register OS trap: A0 = a; returns D0. */
static uint32_t a0_trap(uint16_t trap, gaddr a)
{
    ppc_trap_begin();
    ppc_trap_set(8, a);
    ppc_trap(trap);
    uint32_t r = ppc_trap_get(0);
    ppc_trap_end();
    return r;
}

static void g_MemError(ppc_cpu *c) { RETURN((int32_t)(int16_t)rd16(LM_MemErr)); }
static void g_GetMBarHeight(ppc_cpu *c) { RETURN((int32_t)(int16_t)rd16(LM_MBarHeight)); }
static void g_ApplicationZone(ppc_cpu *c) { RETURN(rd32(LM_ApplZone)); }
static void g_SystemZone(ppc_cpu *c) { RETURN(rd32(LM_SysZone)); }
static void g_HLockHi(ppc_cpu *c)
{
    a0_trap(0xA064, R(0)); /* MoveHHi */
    a0_trap(0xA029, R(0)); /* HLock */
    c->pc = c->lr;
}
/* The 68k world's A5: PowerPC code keeps its globals elsewhere. */
static void g_SetCurrentA5(ppc_cpu *c)
{
    g_cpu->a[5] = rd32(LM_CurrentA5);
    RETURN(g_cpu->a[5]);
}
static void g_SetA5(ppc_cpu *c)
{
    uint32_t old = g_cpu->a[5];
    g_cpu->a[5] = R(0);
    RETURN(old);
}
static void g_nothing(ppc_cpu *c) { c->pc = c->lr; }
static void g_noErr(ppc_cpu *c) { RETURN(0); }

/* Trap addresses: _GetTrapAddress with the new-style bit, and the
 * Toolbox bit for Toolbox traps. */
static uint32_t trap_address(uint16_t trap, bool toolbox)
{
    ppc_trap_begin();
    ppc_trap_set(0, trap);
    ppc_trap((uint16_t)(0xA346 | (toolbox ? 0x0400 : 0)));
    uint32_t r = ppc_trap_get(8);
    ppc_trap_end();
    return r;
}
static void set_trap_address(uint32_t addr, uint16_t trap, bool toolbox)
{
    ppc_trap_begin();
    ppc_trap_set(8, addr);
    ppc_trap_set(0, trap);
    ppc_trap((uint16_t)(0xA247 | (toolbox ? 0x0400 : 0)));
    ppc_trap_end();
}
/* TrapType: OSTrap 0, ToolTrap 1. */
static void g_NGetTrapAddress(ppc_cpu *c) { RETURN(trap_address((uint16_t)R(0), (R(1) & 0xFF) == 1)); }
static void g_NSetTrapAddress(ppc_cpu *c)
{
    set_trap_address(R(0), (uint16_t)R(1), (R(2) & 0xFF) == 1);
    c->pc = c->lr;
}
static void g_GetToolboxTrapAddress(ppc_cpu *c) { RETURN(trap_address((uint16_t)R(0), true)); }
static void g_GetOSTrapAddress(ppc_cpu *c) { RETURN(trap_address((uint16_t)R(0), false)); }
static void g_SetToolboxTrapAddress(ppc_cpu *c)
{
    set_trap_address(R(0), (uint16_t)R(1), true);
    c->pc = c->lr;
}
static void g_SetOSTrapAddress(ppc_cpu *c)
{
    set_trap_address(R(0), (uint16_t)R(1), false);
    c->pc = c->lr;
}
/* GetTrapAddress(trap): the old call, Toolbox numbers above $A800 by the trap word. */
static void g_GetTrapAddress(ppc_cpu *c)
{
    uint16_t t = (uint16_t)R(0);
    RETURN(trap_address(t, (t & 0x0800) != 0));
}

static void g_SetSoundVol(ppc_cpu *c)
{
    wr8(LM_SdVolume, (uint8_t)(R(0) & 7));
    c->pc = c->lr;
}
static void g_GetSoundVol(ppc_cpu *c)
{
    if (R(0))
        wr16(R(0), rd8(LM_SdVolume) & 7);
    c->pc = c->lr;
}

/* Fixed-point to and from double. */
static void g_Fix2X(ppc_cpu *c) { RETURN_F((int32_t)R(0) / 65536.0); }
static void g_Frac2X(ppc_cpu *c) { RETURN_F((int32_t)R(0) / 1073741824.0); }
static int32_t to_fixed(double v, double scale)
{
    v *= scale;
    if (isnan(v))
        return 0;
    if (v >= 2147483647.0)
        return 0x7FFFFFFF;
    if (v <= -2147483648.0)
        return (int32_t)0x80000000;
    return (int32_t)lrint(v);
}
static void g_X2Fix(ppc_cpu *c) { RETURN(to_fixed(F(0), 65536.0)); }
static void g_X2Frac(ppc_cpu *c) { RETURN(to_fixed(F(0), 1073741824.0)); }

/* The dialog editing calls: TextEdit on the dialog's edit field. */
static void dialog_te(ppc_cpu *c, void (*fn)(gaddr))
{
    gaddr d = R(0);
    if (d && (int16_t)rd16(d + DialogRecord_editField_off) >= 0 && rd32(d + DialogRecord_textH_off))
        fn(rd32(d + DialogRecord_textH_off));
    c->pc = c->lr;
}
static void g_DialogCut(ppc_cpu *c) { dialog_te(c, tb_TECut); }
static void g_DialogCopy(ppc_cpu *c) { dialog_te(c, tb_TECopy); }
static void g_DialogPaste(ppc_cpu *c) { dialog_te(c, tb_TEPaste); }
static void g_DialogDelete(ppc_cpu *c) { dialog_te(c, tb_TEDelete); }

/* GetWDInfo(wdRefNum, *vRefNum, *dirID, *procID) */
static void g_GetWDInfo(ppc_cpu *c)
{
    gaddr pb = tb_NewPtr(128, true, true);
    wr16(pb + 24, (uint16_t)R(0)); /* ioVRefNum */
    int16_t e = tb_PBGetWDInfo(pb, false);
    if (R(1))
        wr16(R(1), rd16(pb + 32)); /* ioWDVRefNum */
    if (R(2))
        wr32(R(2), rd32(pb + 48)); /* ioWDDirID */
    if (R(3))
        wr32(R(3), rd32(pb + 28)); /* ioWDProcID */
    tb_DisposePtr(pb);
    RETURN((int32_t)e);
}
/* OpenDriver(name, *refNum) / CloseDriver(refNum) */
static void g_OpenDriver(ppc_cpu *c)
{
    gaddr pb = tb_NewPtr(128, true, true);
    wr32(pb + 18, R(0)); /* ioNamePtr */
    int16_t e = tb_PBOpen(pb, false);
    if (R(1))
        wr16(R(1), rd16(pb + 24)); /* ioRefNum */
    tb_DisposePtr(pb);
    RETURN((int32_t)e);
}
static void g_CloseDriver(ppc_cpu *c)
{
    gaddr pb = tb_NewPtr(128, true, true);
    wr16(pb + 24, (uint16_t)R(0));
    int16_t e = tb_PBClose(pb, false);
    tb_DisposePtr(pb);
    RETURN((int32_t)e);
}
static void g_GetDCtlEntry(ppc_cpu *c)
{
    int16_t ref = (int16_t)R(0);
    gaddr table = rd32(LM_UTableBase);
    RETURN(table && ref < 0 ? rd32(table + 4 * (gaddr)(-ref - 1)) : 0);
}

/* The Component Manager: _ComponentDispatch with the selector in D0,
 * Pascal arguments (sizes 4 or 2, as a string) and result (0, 2, 4). */
typedef struct {
    uint8_t selector;
    const char *args;
    int result;
} component_call;

static void component_manager(ppc_cpu *c, const component_call *k)
{
    ppc_trap_begin();
    if (k->result)
        ppc_trap_push(k->result, 0);
    for (int i = 0; k->args[i]; i++)
        ppc_trap_push(k->args[i] - '0', R(i));
    ppc_trap_set(0, k->selector);
    ppc_trap(0xA82A);
    uint32_t r = k->result ? ppc_trap_result(k->result) : 0;
    ppc_trap_end();
    if (k->result == 2)
        r = (uint32_t)(int32_t)(int16_t)r;
    RETURN(r);
}

#define COMPONENT(name, sel, args, result) \
    static void cm_##name(ppc_cpu *c) { static const component_call k = { sel, args, result }; component_manager(c, &k); }
COMPONENT(RegisterComponent, 0x01, "444244", 4)
COMPONENT(UnregisterComponent, 0x02, "4", 2)
COMPONENT(CountComponents, 0x03, "4", 4)
COMPONENT(FindNextComponent, 0x04, "44", 4)
COMPONENT(GetComponentInfo, 0x05, "44444", 2)
COMPONENT(GetComponentListModSeed, 0x06, "", 4)
COMPONENT(OpenComponent, 0x07, "4", 4)
COMPONENT(CloseComponent, 0x08, "4", 2)
COMPONENT(GetComponentInstanceError, 0x0A, "4", 2)
COMPONENT(SetComponentInstanceError, 0x0B, "42", 0)
COMPONENT(GetComponentInstanceStorage, 0x0C, "4", 4)
COMPONENT(SetComponentInstanceStorage, 0x0D, "44", 0)
COMPONENT(GetComponentInstanceA5, 0x0E, "4", 4)
COMPONENT(SetComponentInstanceA5, 0x0F, "44", 0)
COMPONENT(GetComponentRefcon, 0x10, "4", 4)
COMPONENT(SetComponentRefcon, 0x11, "44", 0)
COMPONENT(RegisterComponentResource, 0x12, "42", 4)
COMPONENT(CountComponentInstances, 0x13, "4", 4)
COMPONENT(RegisterComponentResourceFile, 0x14, "22", 4)
COMPONENT(OpenComponentResFile, 0x15, "4", 2)
COMPONENT(CloseComponentResFile, 0x18, "2", 2)
COMPONENT(CaptureComponent, 0x1C, "44", 4)
COMPONENT(UncaptureComponent, 0x1D, "4", 2)
COMPONENT(SetDefaultComponent, 0x1E, "42", 2)
COMPONENT(ResolveComponentAlias, 0x20, "4", 4)
COMPONENT(OpenDefaultComponent, 0x21, "44", 4)
COMPONENT(DelegateComponentCall, 0x24, "44", 4)
COMPONENT(GetComponentIconSuite, 0x29, "44", 2)
COMPONENT(GetComponentTypeModSeed, 0x2C, "4", 4)
COMPONENT(OpenAComponent, 0x2D, "44", 2)
COMPONENT(OpenADefaultComponent, 0x2E, "444", 2)
COMPONENT(OpenAComponentResFile, 0x2F, "44", 2)
COMPONENT(GetComponentResource, 0x35, "4424", 2)
COMPONENT(GetComponentIndString, 0x36, "4422", 2)
#undef COMPONENT

/* Component calls (ComponentCallNow): the instance and the parameters
 * (sizes 4, 2, or 1 for a Boolean or char pushed as a word), then the
 * parameter size and selector; _ComponentDispatch with D0 = 0. */
static void instance_call(ppc_cpu *c, int16_t what, const char *args)
{
    ppc_trap_begin();
    ppc_trap_push(4, 0);
    ppc_trap_push(4, R(0));
    uint32_t size = 0;
    for (int i = 0; args[i]; i++) {
        int n = args[i] - '0';
        ppc_trap_push(n, R(1 + i));
        size += n == 4 ? 4 : 2;
    }
    ppc_trap_push(4, size << 16 | (uint16_t)what);
    ppc_trap_set(0, 0);
    ppc_trap(0xA82A);
    uint32_t r = ppc_trap_result(4);
    ppc_trap_end();
    RETURN(r);
}

#define CCALL(name, what, args) static void cc_##name(ppc_cpu *c) { instance_call(c, what, args); }
CCALL(MCSetMovie, 2, "444")
CCALL(MCRemoveAMovie, 3, "4")
CCALL(MCGetIndMovie, 5, "2")
CCALL(MCRemoveAllMovies, 6, "")
CCALL(MCIsPlayerEvent, 7, "4")
CCALL(MCSetActionFilter, 8, "4")
CCALL(MCDoAction, 9, "24")
CCALL(MCSetControllerAttached, 10, "1")
CCALL(MCIsControllerAttached, 11, "")
CCALL(MCSetControllerPort, 12, "4")
CCALL(MCGetControllerPort, 13, "")
CCALL(MCSetVisible, 14, "1")
CCALL(MCGetVisible, 15, "")
CCALL(MCGetControllerBoundsRect, 16, "4")
CCALL(MCSetControllerBoundsRect, 17, "4")
CCALL(MCGetControllerBoundsRgn, 18, "")
CCALL(MCGetWindowRgn, 19, "4")
CCALL(MCMovieChanged, 20, "4")
CCALL(MCSetDuration, 21, "4")
CCALL(MCGetCurrentTime, 22, "4")
CCALL(MCNewAttachedController, 23, "444")
CCALL(MCDraw, 24, "4")
CCALL(MCActivate, 25, "41")
CCALL(MCIdle, 26, "")
CCALL(MCKey, 27, "14")
CCALL(MCClick, 28, "4444")
CCALL(MCEnableEditing, 29, "1")
CCALL(MCIsEditingEnabled, 30, "")
CCALL(MCCopy, 31, "")
CCALL(MCCut, 32, "")
CCALL(MCPaste, 33, "4")
CCALL(MCClear, 34, "")
CCALL(MCUndo, 35, "")
CCALL(MCPositionController, 36, "444")
CCALL(MCGetControllerInfo, 37, "4")
CCALL(MCSetClip, 40, "44")
CCALL(MCGetClip, 41, "44")
CCALL(MCDrawBadge, 42, "44")
CCALL(MCSetUpEditMenu, 43, "44")
CCALL(MCGetMenuString, 44, "424")
CCALL(MCSetActionFilterWithRefCon, 45, "44")
CCALL(MCPtInController, 46, "44")
CCALL(MCInvalidate, 47, "44")
CCALL(MCAdjustCursor, 48, "444")
CCALL(MCGetInterfaceElement, 49, "44")
#undef CCALL

/* ObjectSupportLib: the object model's resolution isn't there, so
 * accessors install and resolving answers errAEEventNotHandled. */
static void os_notHandled(ppc_cpu *c) { RETURN((uint32_t)(int32_t)-1708); }
static void os_accessorNotFound(ppc_cpu *c) { RETURN((uint32_t)(int32_t)-1723); }
static void os_coercionFail(ppc_cpu *c) { RETURN((uint32_t)(int32_t)-1700); }
/* FindWordBreaks(text, len, offset, leadingEdge, breaks, offsets, script):
 * FindWord's, the script aside. */
static void g_FindWordBreaks(ppc_cpu *c)
{
    tb_FindWord(R(0), (int16_t)R(1), (int16_t)R(2), (int8_t)R(3), R(4), R(5));
    c->pc = c->lr;
}

/* UppercaseStripDiacritics(text, len, script): in place. */
static void g_UppercaseStripDiacritics(ppc_cpu *c)
{
    int16_t n = (int16_t)R(1);
    for (int16_t i = 0; i < n && R(0); i++)
        wr8(R(0) + (uint32_t)i, (uint8_t)mac_char_key(rd8(R(0) + (uint32_t)i), false, true));
    c->pc = c->lr;
}

/* PBControlImmed(pb): a Control call, done at once like the others. */
static void g_PBControlImmed(ppc_cpu *c) { RETURN((uint32_t)(int32_t)tb_PBControl(R(0), false)); }

/* TruncText(width, textPtr, short *length, truncWhere): the text cut to
 * fit width with an ellipsis, as TruncString cuts a Pascal string. */
/* GetSoundOutputInfo(outputDevice, selector, infoPtr): the sound output
 * device's settings, as the Sound Manager 3 output component reports them
 * (SndGetInfo's selectors, for the device rather than a channel). */
static void g_GetSoundOutputInfo(ppc_cpu *c)
{
    gaddr info = R(2);
    int16_t err = noErr;
    switch (R(1)) {
    case 0x73726174: wr32(info, 0xAC440000u); break; /* 'srat': 44100 Hz, UnsignedFixed */
    case 0x7373697A: wr16(info, 16); break;          /* 'ssiz': 16-bit samples */
    case 0x6368616E: wr16(info, 2); break;           /* 'chan': stereo */
    case 0x766F6C75: wr32(info, 0x01000100); break;  /* 'volu': full, both sides */
    case 0x71756C74: wr32(info, 0x00000004); break;  /* 'qual': the best interpolation */
    default: err = -231;                             /* siUnknownInfoType */
    }
    RETURN((uint32_t)(int32_t)err);
}

/* The Script Manager's older names: GetScript and SetScript are
 * Get/SetScriptVariable, GetEnvirons and SetEnvirons the Script Manager
 * variables (the same traps and selectors). */
static void g_GetScript(ppc_cpu *c) { RETURN((uint32_t)tb_GetScriptVariable((int16_t)R(0), (int16_t)R(1))); }
static void g_SetScript(ppc_cpu *c)
{
    RETURN((uint32_t)(int32_t)tb_SetScriptVariable((int16_t)R(0), (int16_t)R(1), (int32_t)R(2)));
}
static void g_GetEnvirons(ppc_cpu *c) { RETURN((uint32_t)tb_GetScriptManagerVariable((int16_t)R(0))); }
static void g_SetEnvirons(ppc_cpu *c)
{
    RETURN((uint32_t)(int32_t)tb_SetScriptManagerVariable((int16_t)R(0), (int32_t)R(1)));
}

/* StandardAlert(type, error, explanation, param, &itemHit) (AppearanceLib) */
static void g_StandardAlert(ppc_cpu *c)
{
    RETURN((uint32_t)(int32_t)dialog_standard_alert((int16_t)R(0), R(1), R(2), R(3), R(4)));
}

/* SetSoundOutputInfo: settings are the host's; accepted and kept nowhere. */
static void g_SetSoundOutputInfo(ppc_cpu *c) { RETURN(noErr); }

static void g_TruncText(ppc_cpu *c)
{
    gaddr text = R(1), lenp = R(2);
    int16_t len = lenp ? (int16_t)rd16(lenp) : 0;
    if (len <= 0 || !text) {
        RETURN(0);
        return;
    }
    if (len > 255)
        len = 255;
    gaddr tmp = tb_NewPtr(256, false, false);
    if (!tmp) {
        RETURN((uint32_t)-1);
        return;
    }
    wr8(tmp, (uint8_t)len);
    gmemcpy(tmp + 1, text, (uint32_t)len);
    int16_t r = tb_TruncString((int16_t)R(0), tmp, (int16_t)R(3));
    if (r > 0) {
        gmemcpy(text, tmp + 1, rd8(tmp));
        wr16(lenp, rd8(tmp));
    }
    tb_DisposePtr(tmp);
    RETURN((uint32_t)(int32_t)r);
}

/* GetDefaultUser(&userRef, name): Sharing Setup's owner, the host
 * account's full name (Netscape Navigator names its profile after it). */
static void g_GetDefaultUser(ppc_cpu *c)
{
    const struct passwd *pw = getpwuid(getuid());
    const char *name = pw && pw->pw_gecos && *pw->pw_gecos ? pw->pw_gecos : pw ? pw->pw_name : "User";
    uint8_t mac[256];
    size_t n = utf8_to_macroman(name, mac, strlen(name));
    if (n > 31)
        n = 31;
    if (R(3))
        wr16(R(3), 0);
    if (R(4)) {
        wr8(R(4), (uint8_t)n);
        gput(R(4) + 1, mac, (uint32_t)n);
    }
    RETURN(0);
}

static void os_AEResolve(ppc_cpu *c) { RETURN((uint32_t)(int32_t)tb_AEResolve(R(3), (int16_t)R(4), R(5))); }

/* Internet Config: an instance with no preferences in it. */
static void ic_ICStart(ppc_cpu *c)
{
    if (R(0))
        wr32(R(0), 0x49430001); /* any non-zero instance */
    RETURN(0);
}
static void ic_prefNotFound(ppc_cpu *c) { RETURN((uint32_t)(int32_t)-666); }

/* The Display Manager: the one screen, which keeps its mode. */
static void dm_GetFirstScreenDevice(ppc_cpu *c) { RETURN(rd32(LM_DeviceList)); }
static void dm_GetNextScreenDevice(ppc_cpu *c) { RETURN(R(0) && rd32(R(0)) ? rd32(rd32(R(0)) + 30) : 0); }
static void dm_GetDisplayIDByGDevice(ppc_cpu *c)
{
    if (R(1))
        wr32(R(1), 1);
    RETURN(0);
}
static void dm_GetGDeviceByDisplayID(ppc_cpu *c)
{
    if (R(1))
        wr32(R(1), rd32(LM_MainDevice));
    RETURN(0);
}
static void dm_GetDeskRegion(ppc_cpu *c)
{
    if (R(0))
        wr32(R(0), rd32(LM_GrayRgn));
    RETURN(0);
}
static void dm_paramErr(ppc_cpu *c) { RETURN((uint32_t)(int32_t)-50); }

/* C-string forms of the comparisons. */
static gaddr temp_pstring(gaddr cstr)
{
    gaddr p = tb_NewPtr(256, false, true);
    if (p)
        c_to_p(cstr, p);
    return p;
}
static void c_compare(ppc_cpu *c, uint16_t trap, bool equal)
{
    gaddr a = temp_pstring(R(0)), b = temp_pstring(R(1));
    uint32_t r = compare_trap(trap, a, b, R(2) & 0xFF, R(3) & 0xFF);
    tb_DisposePtr(a);
    tb_DisposePtr(b);
    RETURN(equal ? (uint32_t)(r == 0) : (uint32_t)(int32_t)(int16_t)r);
}
static void g_equalstring(ppc_cpu *c) { c_compare(c, 0xA03C, true); }
static void g_relstring(ppc_cpu *c) { c_compare(c, 0xA050, false); }

/* The Sound Manager's version: 3.2.1. */
static void g_SVersion(ppc_cpu *c)
{
    /* NumVersion results come back through the pointer in r3. */
    if (R(0))
        wr32(R(0), 0x03218000);
    c->pc = c->lr;
}
/* No AppleTalk: the port isn't configured. */
static void g_portNotCf(ppc_cpu *c) { RETURN((uint32_t)(int32_t)-98); }
/* FindServiceWindow(where, *window): no input method windows. */
static void g_FindServiceWindow(ppc_cpu *c)
{
    if (R(1))
        wr32(R(1), 0);
    RETURN(0);
}
static void g_GetVideoDefault(ppc_cpu *c)
{
    if (R(0))
        gmemset(R(0), 0, 4);
    c->pc = c->lr;
}

/* ---------------------------------------------------------------------- */
/* Wide (64-bit) arithmetic: hi long then lo long in memory. */

static int64_t wide_get(gaddr w) { return (int64_t)((uint64_t)rd32(w) << 32 | rd32(w + 4)); }
static void wide_put(gaddr w, int64_t v)
{
    wr32(w, (uint32_t)((uint64_t)v >> 32));
    wr32(w + 4, (uint32_t)v);
}
static void w_WideAdd(ppc_cpu *c) { wide_put(R(0), (int64_t)((uint64_t)wide_get(R(0)) + (uint64_t)wide_get(R(1)))); RETURN(R(0)); }
static void w_WideSubtract(ppc_cpu *c) { wide_put(R(0), (int64_t)((uint64_t)wide_get(R(0)) - (uint64_t)wide_get(R(1)))); RETURN(R(0)); }
static void w_WideNegate(ppc_cpu *c) { wide_put(R(0), (int64_t)(0 - (uint64_t)wide_get(R(0)))); RETURN(R(0)); }
static void w_WideCompare(ppc_cpu *c)
{
    int64_t a = wide_get(R(0)), b = wide_get(R(1));
    RETURN((int32_t)(a < b ? -1 : a > b));
}
static void w_WideMultiply(ppc_cpu *c) { wide_put(R(2), (int64_t)(int32_t)R(0) * (int32_t)R(1)); RETURN(R(2)); }
/* WideDivide(dividend, divisor, *remainder): the quotient (a long). */
static void w_WideDivide(ppc_cpu *c)
{
    int64_t a = wide_get(R(0));
    int32_t d = (int32_t)R(1);
    if (!d) {
        if (R(2))
            wr32(R(2), 0x80000000u);
        RETURN(0x80000000u);
        return;
    }
    if (R(2))
        wr32(R(2), (uint32_t)(int32_t)(a % d));
    RETURN((uint32_t)(int32_t)(a / d));
}
/* WideWideDivide(dividend, divisor, *remainder): the quotient in place. */
static void w_WideWideDivide(ppc_cpu *c)
{
    int64_t a = wide_get(R(0));
    int32_t d = (int32_t)R(1);
    if (d) {
        if (R(2))
            wr32(R(2), (uint32_t)(int32_t)(a % d));
        wide_put(R(0), a / d);
    }
    RETURN(R(0));
}
/* WideShift(src, shift): arithmetic, right for a positive shift, rounded. */
static void w_WideShift(ppc_cpu *c)
{
    int64_t a = wide_get(R(0));
    int32_t n = (int32_t)R(1);
    if (n > 0 && n < 64)
        a = (a + ((int64_t)1 << (n - 1))) >> n;
    else if (n < 0 && n > -64)
        a = (int64_t)((uint64_t)a << -n);
    wide_put(R(0), a);
    RETURN(R(0));
}
static void w_WideBitShift(ppc_cpu *c)
{
    int64_t a = wide_get(R(0));
    int32_t n = (int32_t)R(1);
    if (n > 0 && n < 64)
        a >>= n;
    else if (n < 0 && n > -64)
        a = (int64_t)((uint64_t)a << -n);
    wide_put(R(0), a);
    RETURN(R(0));
}
static void w_WideSquareRoot(ppc_cpu *c)
{
    int64_t a = wide_get(R(0));
    RETURN(a > 0 ? (uint32_t)sqrt((double)a) : 0);
}

/* ---------------------------------------------------------------------- */
/* The C-string forms of Pascal-string calls: the strings go over to
 * Pascal, the Pascal routine runs, and strings it fills come back. */

/* Call the export `name` with argument `in` (a C string) made Pascal, and
 * argument `out` (a buffer) made C after; -1 for none. */
static void with_pascal(ppc_cpu *c, const char *name, int in, int out)
{
    void (*fn)(ppc_cpu *) = cfm_export_fn(name);
    if (!fn) {
        ppc_halt(c, "unimplemented: %s (for the C-string form)", name);
        return;
    }
    gaddr tmp = 0, cbuf = out >= 0 ? R(out) : 0;
    if (in >= 0 && R(in)) {
        tmp = temp_pstring(R(in));
        c->gpr[3 + in] = tmp;
    }
    if (cbuf) {
        tmp = tmp ? tmp : tb_NewPtr(256, true, true);
        c->gpr[3 + out] = tmp;
    }
    uint32_t lr = c->lr;
    fn(c);
    if (cbuf)
        p_to_c(tmp, cbuf);
    if (tmp)
        tb_DisposePtr(tmp);
    c->pc = lr;
}
/* The same with several string arguments: bit n of `ins` for argument n. */
static void with_pascals(ppc_cpu *c, const char *name, unsigned ins)
{
    void (*fn)(ppc_cpu *) = cfm_export_fn(name);
    if (!fn) {
        ppc_halt(c, "unimplemented: %s (for the C-string form)", name);
        return;
    }
    gaddr t[8] = { 0 };
    for (int i = 0; i < 8; i++)
        if ((ins >> i) & 1 && R(i)) {
            t[i] = temp_pstring(R(i));
            c->gpr[3 + i] = t[i];
        }
    uint32_t lr = c->lr;
    fn(c);
    for (int i = 0; i < 8; i++)
        if (t[i])
            tb_DisposePtr(t[i]);
    c->pc = lr;
}
static void cs_getfinfo(ppc_cpu *c) { with_pascals(c, "GetFInfo", 1); }
static void cs_setfinfo(ppc_cpu *c) { with_pascals(c, "SetFInfo", 1); }
static void cs_fsopen(ppc_cpu *c) { with_pascals(c, "FSOpen", 1); }
static void cs_openrf(ppc_cpu *c) { with_pascals(c, "OpenRF", 1); }
static void cs_create(ppc_cpu *c) { with_pascals(c, "Create", 1); }
static void cs_fsdelete(ppc_cpu *c) { with_pascals(c, "FSDelete", 1); }
static void cs_rename(ppc_cpu *c) { with_pascals(c, "Rename", 1 | 4); }
static void cs_hopen(ppc_cpu *c) { with_pascals(c, "HOpen", 4); }
static void cs_fsmakefsspec(ppc_cpu *c) { with_pascals(c, "FSMakeFSSpec", 4); }
static void cs_opendeskacc(ppc_cpu *c) { with_pascals(c, "OpenDeskAcc", 1); }
static void cs_opendriver(ppc_cpu *c) { with_pascals(c, "OpenDriver", 1); }
static void cs_newwindow(ppc_cpu *c) { with_pascals(c, "NewWindow", 4); }
static void cs_newcwindow(ppc_cpu *c) { with_pascals(c, "NewCWindow", 4); }
static void cs_setwtitle(ppc_cpu *c) { with_pascals(c, "SetWTitle", 2); }
static void cs_getwtitle(ppc_cpu *c) { with_pascal(c, "GetWTitle", -1, 1); }
static void cs_newcontrol(ppc_cpu *c) { with_pascals(c, "NewControl", 8); }
/* The Gestalt value calls (Mac OS 7.6): selectors with plain values. */
static void g_NewGestaltValue(ppc_cpu *c)
{
    if (gestalt_exists(R(0))) {
        RETURN((uint32_t)(int32_t)-5552); /* gestaltDupSelectorErr */
        return;
    }
    gestalt_set(R(0), R(1));
    RETURN(0);
}
static void g_ReplaceGestaltValue(ppc_cpu *c)
{
    if (!gestalt_exists(R(0))) {
        RETURN((uint32_t)(int32_t)-5551); /* gestaltUndefSelectorErr */
        return;
    }
    gestalt_set(R(0), R(1));
    RETURN(0);
}
static void g_SetGestaltValue(ppc_cpu *c)
{
    gestalt_set(R(0), R(1));
    RETURN(0);
}
static void g_DeleteGestaltValue(ppc_cpu *c)
{
    if (!gestalt_exists(R(0))) {
        RETURN((uint32_t)(int32_t)-5551);
        return;
    }
    gestalt_delete(R(0));
    RETURN(0);
}
/* The alert sound's 'snd ' ID (Sound control panel): kept, not used. */
static int16_t alert_beep_id = 1;
static void g_GetAlertBeepResID(ppc_cpu *c) { RETURN((int32_t)alert_beep_id); }
static void g_SetAlertBeepResID(ppc_cpu *c)
{
    alert_beep_id = (int16_t)R(0);
    c->pc = c->lr;
}

/* YieldToAnyThread(): YieldToThread(kNoThreadID). */
static void g_YieldToAnyThread(ppc_cpu *c)
{
    void (*fn)(ppc_cpu *) = cfm_export_fn("YieldToThread");
    c->gpr[3] = 0;
    if (fn)
        fn(c);
    else
        RETURN(0);
}
static void cs_openresfile(ppc_cpu *c) { with_pascals(c, "OpenResFile", 1); }
static void cs_createresfile(ppc_cpu *c) { with_pascals(c, "CreateResFile", 1); }
static void cs_getnamedresource(ppc_cpu *c) { with_pascals(c, "GetNamedResource", 2); }
/* InitZone(growProc, masterCount, limitPtr, startPtr): _InitZone with its
 * parameter block. */
static void g_InitZone(ppc_cpu *c)
{
    gaddr pb = tb_NewPtr(14, true, true);
    wr32(pb, R(3));
    wr32(pb + 4, R(2));
    wr16(pb + 8, (uint16_t)R(1));
    wr32(pb + 10, R(0));
    ppc_trap_begin();
    ppc_trap_set(8, pb);
    ppc_trap(0xA019);
    ppc_trap_end();
    tb_DisposePtr(pb);
    c->pc = c->lr;
}
static void cs_setdialogitemtext(ppc_cpu *c) { with_pascal(c, "SetDialogItemText", 1, -1); }
static void cs_getdialogitemtext(ppc_cpu *c) { with_pascal(c, "GetDialogItemText", -1, 1); }
static void cs_numtostring(ppc_cpu *c) { with_pascal(c, "NumToString", -1, 1); }
static void cs_stringtonum(ppc_cpu *c) { with_pascal(c, "StringToNum", 0, -1); }
static void cs_setcontroltitle(ppc_cpu *c) { with_pascal(c, "SetControlTitle", 1, -1); }
static void cs_getcontroltitle(ppc_cpu *c) { with_pascal(c, "GetControlTitle", -1, 1); }
static void cs_insertmenuitem(ppc_cpu *c) { with_pascal(c, "InsertMenuItem", 1, -1); }
static void cs_appendmenu(ppc_cpu *c) { with_pascal(c, "AppendMenu", 1, -1); }
static void cs_setmenuitemtext(ppc_cpu *c) { with_pascal(c, "SetMenuItemText", 2, -1); }
static void cs_getmenuitemtext(ppc_cpu *c) { with_pascal(c, "GetMenuItemText", -1, 2); }
static void cs_drawstring(ppc_cpu *c) { with_pascal(c, "DrawString", 0, -1); }
static void cs_stringwidth(ppc_cpu *c) { with_pascal(c, "StringWidth", 0, -1); }
static void cs_upperstring(ppc_cpu *c) { with_pascal(c, "UpperString", 0, 0); }
static void cs_debugstr(ppc_cpu *c) { c->pc = c->lr; }
static void cs_newmenu(ppc_cpu *c) { with_pascal(c, "NewMenu", 1, -1); }
static void cs_getindstring(ppc_cpu *c) { with_pascal(c, "GetIndString", -1, 0); }
/* paramtext(a, b, c, d): four strings in. */
static void cs_paramtext(ppc_cpu *c)
{
    gaddr t[4];
    for (int i = 0; i < 4; i++) {
        t[i] = R(i) ? temp_pstring(R(i)) : 0;
        c->gpr[3 + i] = t[i];
    }
    uint32_t lr = c->lr;
    void (*fn)(ppc_cpu *) = cfm_export_fn("ParamText");
    if (fn)
        fn(c);
    for (int i = 0; i < 4; i++)
        if (t[i])
            tb_DisposePtr(t[i]);
    c->pc = lr;
}

/* ---------------------------------------------------------------------- */
/* Assorted */

/* The Display Manager's modes: the one mode the screen is in. */
static void dm_GetDisplayMode(ppc_cpu *c)
{
    /* VDSwitchInfoRec: csMode (word), csData (long), csPage, csBaseAddr */
    gaddr r = R(1);
    gaddr gd = R(0) ? R(0) : rd32(LM_MainDevice);
    gaddr pm = gd && rd32(gd) ? rd32(rd32(gd) + GDevice_gdPMap_off) : 0;
    int depth = pm && rd32(pm) ? (int16_t)rd16(rd32(pm) + PixMap_pixelSize_off) : 8;
    if (r) {
        wr16(r, (uint16_t)(0x80 + (depth == 1 ? 0 : depth == 2 ? 1 : depth == 4 ? 2 : depth == 8 ? 3 : depth == 16 ? 4 : 5)));
        wr32(r + 2, 0x80); /* the display mode */
        wr16(r + 6, 0);
        wr32(r + 8, pm && rd32(pm) ? rd32(rd32(pm) + PixMap_baseAddr_off) : 0);
    }
    RETURN(0);
}
/* DMCheckDisplayMode(gd, mode, depth, *switchFlags, reserved, *modeOk) */
static void dm_CheckDisplayMode(ppc_cpu *c)
{
    if (R(3))
        wr32(R(3), 0);
    if (R(5))
        wr8(R(5), R(1) == 0x80);
    RETURN(0);
}
/* Display mode lists: the sizes up to the screen's (a smaller display is
 * centred, as DrawSprocket does it), each at 8, 16 and 32 bits.  A list is
 * a guest block holding its count. */
static const struct { int16_t w, h; } mode_sizes[] = { { 640, 480 }, { 800, 600 }, { 832, 624 }, { 1024, 768 },
                                                       { 1152, 870 }, { 1280, 1024 } };

static int mode_count(int *sizes)
{
    int n = 0;
    for (unsigned i = 0; i < sizeof mode_sizes / sizeof mode_sizes[0]; i++)
        if (mode_sizes[i].w <= g_screen_width && mode_sizes[i].h <= g_screen_height)
            sizes[n++] = (int)i;
    return n;
}

/* DMNewDisplayModeList(displayID, flags, info, *count, *list) */
static void dm_NewDisplayModeList(ppc_cpu *c)
{
    int sizes[8];
    int n = mode_count(sizes);
    gaddr list = tb_NewPtr(4, true, true);
    wr32(list, (uint32_t)n);
    if (R(3))
        wr32(R(3), (uint32_t)n);
    if (R(4))
        wr32(R(4), list);
    RETURN(0);
}

/* DMGetIndexedDisplayModeFromList(list, index, flags, iterator, userData):
 * the iterator gets a DMDisplayModeListEntryRec describing the mode. */
static void dm_GetIndexedDisplayModeFromList(ppc_cpu *c)
{
    int sizes[8];
    int n = mode_count(sizes);
    uint32_t index = R(1);
    if (index >= (uint32_t)n) {
        RETURN((uint32_t)(int32_t)-6229); /* kDMModeNotFoundErr */
        return;
    }
    int w = mode_sizes[sizes[index]].w, h = mode_sizes[sizes[index]].h;
    static const int depths[3] = { 8, 16, 32 };
    /* entry 28, name 256, timing 80, depth block 20, 3 depth infos 20 each,
     * 3 switch infos 16 each, 3 VPBlocks 42 each, resolution 32 */
    gaddr m = tb_NewPtr(28 + 256 + 80 + 20 + 3 * (20 + 16 + 42) + 32, true, true);
    gaddr entry = m, name = m + 28, timing = name + 256, block = timing + 80, infos = block + 20;
    gaddr switches = infos + 3 * 20, vps = switches + 3 * 16;
    char text[32];
    int len = snprintf(text, sizeof text, "%d x %d", w, h);
    wr8(name, (uint8_t)len);
    gput(name + 1, text, (uint32_t)len);
    wr32(timing + 4, 0x3);      /* timingInfoAttributes: kModeValid, kModeSafe */
    wr32(block, 3);             /* depthBlockCount */
    wr32(block + 4, infos);     /* depthVPBlock */
    for (int i = 0; i < 3; i++) {
        gaddr info = infos + 20 * (gaddr)i, sw = switches + 16 * (gaddr)i, vp = vps + 42 * (gaddr)i;
        int d = depths[i];
        wr32(info, sw);
        wr32(info + 4, vp);
        wr16(sw, (uint16_t)(0x80 + i + 3)); /* csMode */
        wr32(sw + 2, 0x80 + index);          /* csData: the display mode */
        wr16(vp + 4, (uint16_t)(w * d / 8));
        wr16(vp + 10, (uint16_t)h);
        wr16(vp + 12, (uint16_t)w);
        wr32(vp + 22, 72u << 16);
        wr32(vp + 26, 72u << 16);
        wr16(vp + 30, d > 8 ? 16 : 0); /* RGBDirect or indexed */
        wr16(vp + 32, (uint16_t)d);
        wr16(vp + 34, d > 8 ? 3 : 1);
        wr16(vp + 36, (uint16_t)(d == 16 ? 5 : d == 32 ? 8 : d));
    }
    /* VDResolutionInfoRec: previous mode, mode, width, height, refresh
     * rate, deepest depth mode, flags. */
    gaddr res = vps + 3 * 42;
    wr32(res + 4, 0x80 + index);
    wr32(res + 8, (uint32_t)w);
    wr32(res + 12, (uint32_t)h);
    wr32(res + 16, 60u << 16);
    wr16(res + 20, 0x85);
    /* DMDisplayModeListEntryRec: flags, name, resolution, timing, depths, version */
    wr32(entry, 0);
    wr32(entry + 4, name);
    wr32(entry + 8, res);
    wr32(entry + 12, timing);
    wr32(entry + 16, block);
    /* pascal void (void *userData, DMListIndexType index, DMDisplayModeListEntryPtr) */
    uint32_t args[3] = { R(4), index, entry };
    mm_call(R(3), 0x00000FC0, args, 3);
    tb_DisposePtr(m);
    RETURN(0);
}
static void dm_DisposeList(ppc_cpu *c)
{
    if (R(0))
        tb_DisposePtr(R(0));
    RETURN(0);
}

/* TextEdit's click loop is a field. */
static void g_TESetClickLoop(ppc_cpu *c)
{
    if (R(1) && rd32(R(1)))
        wr32(rd32(R(1)) + TERec_wordBreak_off + 4, R(0));
    c->pc = c->lr;
}
static void g_InlineGetHandleSize(ppc_cpu *c) { RETURN(R(0) ? (uint32_t)tb_GetHandleSize(R(0)) : 0); }
static void g_GZSaveHnd(ppc_cpu *c) { RETURN(rd32(LM_GZRootHnd)); }
static void g_LMGetSysFontSize(ppc_cpu *c) { RETURN((int32_t)(int16_t)rd16(0x0BA8)); }
static void g_LMSetSysFontSize(ppc_cpu *c) { wr16(0x0BA8, (uint16_t)R(0)); c->pc = c->lr; }
static void g_LMGetGNEFilter(ppc_cpu *c) { RETURN(rd32(0x029A)); }
static void g_LMSetGNEFilter(ppc_cpu *c) { wr32(0x029A, R(0)); c->pc = c->lr; }
static void g_IUEqualPString(ppc_cpu *c) { g_IUEqualString(c); }
static void g_IUCompPString(ppc_cpu *c) { g_IUCompString(c); }
static void g_GetComponentVersion(ppc_cpu *c) { instance_call(c, -4, ""); }
static void g_false(ppc_cpu *c) { RETURN(0); }
/* NewTSMDocument(count, types, *docID, refcon): a document that does nothing. */
static void g_NewTSMDocument(ppc_cpu *c)
{
    if (R(2))
        wr32(R(2), 1);
    RETURN(0);
}
/* The serial driver's calls, on a port that is always idle. */
/* The Serial Manager: Control and Status calls on the driver, csParam
 * filled before and read after. */
static gaddr ser_pb(ppc_cpu *c, int16_t code)
{
    gaddr pb = tb_NewPtr(CntrlParam_SIZEOF, false, true);
    wr16(pb + CntrlParam_ioCRefNum_off, (uint16_t)R(0));
    wr16(pb + CntrlParam_csCode_off, (uint16_t)code);
    return pb;
}
static void ser_call(ppc_cpu *c, gaddr pb, bool status, gaddr out, uint32_t n)
{
    int16_t r = status ? tb_PBStatus(pb, false) : tb_PBControl(pb, false);
    if (out && !r)
        gmemcpy(out, pb + CntrlParam_csParam_off, n);
    tb_DisposePtr(pb);
    RETURN((int32_t)r);
}
static void g_SerReset(ppc_cpu *c)
{
    gaddr pb = ser_pb(c, 8);
    wr16(pb + CntrlParam_csParam_off, (uint16_t)R(1));
    ser_call(c, pb, false, 0, 0);
}
static void g_SerSetBuf(ppc_cpu *c)
{
    gaddr pb = ser_pb(c, 9);
    wr32(pb + CntrlParam_csParam_off, R(1));
    wr16(pb + CntrlParam_csParam_off + 4, (uint16_t)R(2));
    ser_call(c, pb, false, 0, 0);
}
static void g_SerHShake(ppc_cpu *c)
{
    gaddr pb = ser_pb(c, 10);
    if (R(1))
        gmemcpy(pb + CntrlParam_csParam_off, R(1), 8);
    ser_call(c, pb, false, 0, 0);
}
static void g_SerSetBrk(ppc_cpu *c) { ser_call(c, ser_pb(c, 12), false, 0, 0); }
static void g_SerClrBrk(ppc_cpu *c) { ser_call(c, ser_pb(c, 11), false, 0, 0); }
static void g_SerGetBuf(ppc_cpu *c) { ser_call(c, ser_pb(c, 2), true, R(1), 4); }
static void g_SerStatus(ppc_cpu *c) { ser_call(c, ser_pb(c, 8), true, R(1), 6); }
static void g_CRMSearch(ppc_cpu *c) { RETURN(serial_crm_search(R(0))); }
/* The Communications Toolbox: no tools. */
static void g_cmNoTools(ppc_cpu *c) { RETURN((uint32_t)(int32_t)-2); }
static void g_zero_handle(ppc_cpu *c) { RETURN(0); }
/* The AppleTalk calls: AppleTalk isn't there. */
static void g_noAppleTalk(ppc_cpu *c) { RETURN((uint32_t)(int32_t)-98); }

/* AppleTalk's preferred calls: (pb, async) as a Control call on .MPP or
 * .ATP.  Without the trap layer, an answer given at once is written to
 * ioResult and an asynchronous call's completion routine queued here. */
static void at_call(ppc_cpu *c, int16_t refnum, int16_t code)
{
    gaddr pb = R(0);
    bool async = R(1) != 0;
    wr16(pb + IOParam_ioRefNum_off, (uint16_t)refnum);
    wr16(pb + CntrlParam_csCode_off, (uint16_t)code);
    int16_t r = tb_PBControl(pb, async);
    if (r == ioInProgress) {
        RETURN(0);
        return;
    }
    wr16(pb + IOParam_ioResult_off, (uint16_t)r);
    if (async && rd32(pb + IOParam_ioCompletion_off))
        io_complete_later(pb, r);
    RETURN((int32_t)r);
}
#define MPP_CALL(name, code) static void g_##name(ppc_cpu *c) { at_call(c, -10, code); }
#define ATP_CALL(name, code) static void g_##name(ppc_cpu *c) { at_call(c, -11, code); }
MPP_CALL(POpenSkt, 248) MPP_CALL(PCloseSkt, 247) MPP_CALL(PWriteDDP, 246) MPP_CALL(PRegisterName, 253)
MPP_CALL(PLookupName, 251) MPP_CALL(PConfirmName, 250) MPP_CALL(PRemoveName, 252) MPP_CALL(PKillNBP, 254)
MPP_CALL(PSetSelfSend, 256) MPP_CALL(PGetAppleTalkInfo, 258) MPP_CALL(PATalkClosePrep, 259)
ATP_CALL(POpenATPSkt, 254) ATP_CALL(PCloseATPSkt, 250) ATP_CALL(PSendRequest, 255) ATP_CALL(PNSendRequest, 248)
ATP_CALL(PGetRequest, 253) ATP_CALL(PSendResponse, 252) ATP_CALL(PAddResponse, 251) ATP_CALL(PRelTCB, 256)
ATP_CALL(PRelRspCB, 249) ATP_CALL(PKillSendReq, 258) ATP_CALL(PKillGetReq, 257)

/* A Str32 packed after another (NBP's entity names). */
static uint32_t put_str32(gaddr dst, gaddr s)
{
    uint32_t n = s ? rd8(s) : 0;
    if (n > 32)
        n = 32;
    wr8(dst, (uint8_t)n);
    if (n)
        gmemcpy(dst + 1, s + 1, n);
    return n + 1;
}

/* NBPSetEntity(buffer, object, type, zone) */
static void g_NBPSetEntity(ppc_cpu *c)
{
    gaddr p = R(0);
    p += put_str32(p, R(1));
    p += put_str32(p, R(2));
    put_str32(p, R(3));
    RETURN(0);
}

/* NBPSetNTE(ntePtr, object, type, zone, socket): a names table entry,
 * the entity after its link, address and enumerator. */
static void g_NBPSetNTE(ppc_cpu *c)
{
    gaddr nte = R(0);
    gmemset(nte, 0, 9);
    wr8(nte + 7, (uint8_t)R(4));
    gaddr p = nte + 9;
    p += put_str32(p, R(1));
    p += put_str32(p, R(2));
    put_str32(p, R(3));
    RETURN(0);
}

/* NBPExtract(buffer, numInBuf, whichOne, EntityName *, AddrBlock *): the
 * whichOne'th tuple of a lookup's answers. */
static void g_NBPExtract(ppc_cpu *c)
{
    gaddr p = R(0);
    int count = (int16_t)R(1), which = (int16_t)R(2);
    if (which < 1 || which > count) {
        RETURN((int32_t)-3104); /* extractErr */
        return;
    }
    for (int i = 1; i < which; i++) {
        p += 5;
        for (int k = 0; k < 3; k++)
            p += 1 + (uint32_t)rd8(p);
    }
    if (R(4))
        wr32(R(4), rd32(p));
    p += 5;
    for (int k = 0; k < 3; k++) { /* EntityName: three Str32 fields of 34 bytes */
        if (R(3))
            put_str32(R(3) + (uint32_t)k * 34, p);
        p += 1 + (uint32_t)rd8(p);
    }
    RETURN(0);
}

/* BuildDDPwds(wds, header, data, AddrBlock dest, type, length): the
 * header's pointer (an odd address: the buffer's second byte) with the
 * destination and type in it, then the data, then the end. */
static void g_BuildDDPwds(ppc_cpu *c)
{
    gaddr wds = R(0), hdr = R(1) + 1, dest = R(3);
    gmemset(hdr, 0, 16);
    wr16(hdr + 7, (uint16_t)(dest >> 16));
    wr8(hdr + 11, (uint8_t)(dest >> 8));
    wr8(hdr + 13, (uint8_t)dest);
    wr8(hdr + 15, (uint8_t)R(4));
    wr16(wds, 0);
    wr32(wds + 2, hdr);
    wr16(wds + 6, (uint16_t)R(5));
    wr32(wds + 8, R(2));
    wr16(wds + 12, 0);
    RETURN(0);
}

/* BuildBDSBuffer(buffer, bds, size, count): the buffer cut into count
 * elements of up to 578 bytes; returns how many. */
static void g_BuildBDSBuffer(ppc_cpu *c)
{
    gaddr buf = R(0), bds = R(1);
    int left = (int16_t)R(2), n = (int16_t)R(3), i = 0;
    for (; i < n && i < 8 && left > 0; i++) {
        int k = left > 578 ? 578 : left;
        gaddr e = bds + (uint32_t)i * 12;
        wr16(e, (uint16_t)k);
        wr32(e + 2, buf);
        wr16(e + 6, 0);
        wr32(e + 8, 0);
        buf += (uint32_t)k;
        left -= k;
    }
    RETURN(i);
}

/* GetNodeAddress(short *node, short *net) */
static void g_GetNodeAddress(ppc_cpu *c)
{
    if (!at_opened()) {
        RETURN((int32_t)-28); /* notOpenErr */
        return;
    }
    if (R(0))
        wr16(R(0), at_node());
    if (R(1))
        wr16(R(1), 0);
    RETURN(0);
}
static void g_IsMPPOpen(ppc_cpu *c) { RETURN(at_opened() ? 1 : 0); }
static void g_MPPOpen(ppc_cpu *c) { RETURN((int32_t)at_open()); }

const ppc_export lib_exports[] = {
    /* Wide arithmetic */
    { "WideAdd", w_WideAdd }, { "WideBitShift", w_WideBitShift }, { "WideCompare", w_WideCompare },
    { "WideDivide", w_WideDivide }, { "WideMultiply", w_WideMultiply }, { "WideNegate", w_WideNegate },
    { "WideShift", w_WideShift }, { "WideSquareRoot", w_WideSquareRoot }, { "WideSubtract", w_WideSubtract },
    { "WideWideDivide", w_WideWideDivide },
    /* C-string forms */
    { "getfinfo", cs_getfinfo }, { "setfinfo", cs_setfinfo }, { "fsopen", cs_fsopen }, { "openrf", cs_openrf },
    { "create", cs_create }, { "fsdelete", cs_fsdelete }, { "rename", cs_rename }, { "hopen", cs_hopen },
    { "fsmakefsspec", cs_fsmakefsspec }, { "opendeskacc", cs_opendeskacc }, { "opendriver", cs_opendriver },
    { "newwindow", cs_newwindow }, { "newcwindow", cs_newcwindow }, { "setwtitle", cs_setwtitle },
    { "getwtitle", cs_getwtitle }, { "newcontrol", cs_newcontrol },
    { "GetAlertBeepResID", g_GetAlertBeepResID }, { "SetAlertBeepResID", g_SetAlertBeepResID },
    { "YieldToAnyThread", g_YieldToAnyThread }, { "NewGestaltValue", g_NewGestaltValue }, { "ReplaceGestaltValue", g_ReplaceGestaltValue },
    { "SetGestaltValue", g_SetGestaltValue }, { "DeleteGestaltValue", g_DeleteGestaltValue },
    { "openresfile", cs_openresfile }, { "createresfile", cs_createresfile },
    { "getnamedresource", cs_getnamedresource }, { "InitZone", g_InitZone },
    { "appendmenu", cs_appendmenu }, { "debugstr", cs_debugstr }, { "drawstring", cs_drawstring },
    { "getcontroltitle", cs_getcontroltitle }, { "getdialogitemtext", cs_getdialogitemtext },
    { "getindstring", cs_getindstring }, { "getmenuitemtext", cs_getmenuitemtext },
    { "insertmenuitem", cs_insertmenuitem }, { "newmenu", cs_newmenu }, { "numtostring", cs_numtostring },
    { "paramtext", cs_paramtext }, { "setcontroltitle", cs_setcontroltitle },
    { "setdialogitemtext", cs_setdialogitemtext }, { "setmenuitemtext", cs_setmenuitemtext },
    { "stringtonum", cs_stringtonum }, { "stringwidth", cs_stringwidth }, { "upperstring", cs_upperstring },
    /* Display Manager modes */
    { "DMGetDisplayMode", dm_GetDisplayMode }, { "DMCheckDisplayMode", dm_CheckDisplayMode },
    { "DMNewDisplayModeList", dm_NewDisplayModeList }, { "DMGetIndexedDisplayModeFromList", dm_GetIndexedDisplayModeFromList },
    { "DMDisposeList", dm_DisposeList }, { "DMSuspendConfigure", g_noErr }, { "DMResumeConfigure", g_noErr },
    /* Assorted */
    { "TESetClickLoop", g_TESetClickLoop }, { "InlineGetHandleSize", g_InlineGetHandleSize },
    { "GZSaveHnd", g_GZSaveHnd }, { "LMGetSysFontSize", g_LMGetSysFontSize }, { "LMSetSysFontSize", g_LMSetSysFontSize },
    { "LMGetGNEFilter", g_LMGetGNEFilter }, { "LMSetGNEFilter", g_LMSetGNEFilter },
    { "IUEqualPString", g_IUEqualPString }, { "IUCompPString", g_IUCompPString },
    { "GetComponentVersion", g_GetComponentVersion },
    { "HoldMemory", g_noErr }, { "UnholdMemory", g_noErr }, { "LockMemory", g_noErr }, { "UnlockMemory", g_noErr },
    { "LockMemoryContiguous", g_noErr },
    /* The Text Services Manager, with no input methods */
    { "InitTSMAwareApplication", g_noErr }, { "CloseTSMAwareApplication", g_noErr },
    { "NewTSMDocument", g_NewTSMDocument }, { "DeleteTSMDocument", g_noErr }, { "ActivateTSMDocument", g_noErr },
    { "DeactivateTSMDocument", g_noErr }, { "FixTSMDocument", g_noErr }, { "TSMEvent", g_false },
    { "TSMMenuSelect", g_false }, { "SetTSMCursor", g_false }, { "UseInputWindow", g_noErr },
    /* Serial ports */
    { "SerReset", g_SerReset }, { "SerHShake", g_SerHShake }, { "SerSetBuf", g_SerSetBuf }, { "SerSetBrk", g_SerSetBrk },
    { "SerClrBrk", g_SerClrBrk }, { "SerGetBuf", g_SerGetBuf }, { "SerStatus", g_SerStatus },
    /* The Communications Toolbox */
    { "InitCTBUtilities", g_noErr }, { "InitCM", g_cmNoTools }, { "InitCRM", g_noErr }, { "InitTM", g_cmNoTools },
    { "InitFT", g_cmNoTools }, { "CRMSearch", g_CRMSearch }, { "CMNew", g_zero_handle },
    { "CMGetProcID", g_cmNoTools },
    /* AppleTalk and the PPC Toolbox's network side */
    { "GetMyZone", g_noAppleTalk }, { "PPCBrowser", g_noAppleTalk }, { "IPCListPortsSync", g_noAppleTalk },
    { "NBPSetNTE", g_NBPSetNTE }, { "NBPSetEntity", g_NBPSetEntity }, { "NBPExtract", g_NBPExtract },
    { "BuildDDPwds", g_BuildDDPwds }, { "BuildBDSBuffer", g_BuildBDSBuffer }, { "GetNodeAddress", g_GetNodeAddress },
    { "IsMPPOpen", g_IsMPPOpen }, { "IsATPOpen", g_IsMPPOpen }, { "MPPOpen", g_MPPOpen },
    { "POpenSkt", g_POpenSkt }, { "PCloseSkt", g_PCloseSkt }, { "PWriteDDP", g_PWriteDDP },
    { "PRegisterName", g_PRegisterName }, { "PLookupName", g_PLookupName }, { "PConfirmName", g_PConfirmName },
    { "PRemoveName", g_PRemoveName }, { "PKillNBP", g_PKillNBP }, { "PSetSelfSend", g_PSetSelfSend },
    { "PGetAppleTalkInfo", g_PGetAppleTalkInfo }, { "PATalkClosePrep", g_PATalkClosePrep },
    { "POpenATPSkt", g_POpenATPSkt }, { "PCloseATPSkt", g_PCloseATPSkt }, { "PSendRequest", g_PSendRequest },
    { "PNSendRequest", g_PNSendRequest }, { "PGetRequest", g_PGetRequest }, { "PSendResponse", g_PSendResponse },
    { "PAddResponse", g_PAddResponse }, { "PRelTCB", g_PRelTCB }, { "PRelRspCB", g_PRelRspCB },
    { "PKillSendReq", g_PKillSendReq }, { "PKillGetReq", g_PKillGetReq },
    /* Display Manager */
    { "DMGetFirstScreenDevice", dm_GetFirstScreenDevice }, { "DMGetNextScreenDevice", dm_GetNextScreenDevice },
    { "DMGetDisplayIDByGDevice", dm_GetDisplayIDByGDevice }, { "DMGetGDeviceByDisplayID", dm_GetGDeviceByDisplayID },
    { "DMGetDeskRegion", dm_GetDeskRegion }, { "DMRegisterNotifyProc", g_noErr }, { "DMRemoveNotifyProc", g_noErr },
    { "DMBeginConfigureDisplays", g_noErr }, { "DMEndConfigureDisplays", g_noErr },
    { "DMSetDisplayMode", dm_paramErr }, { "DMSetMainDisplay", dm_paramErr },
    /* Odds and ends */
    { "equalstring", g_equalstring }, { "relstring", g_relstring }, { "SVersion", g_SVersion },
    { "MIDIVersion", g_noErr }, { "InstallExceptionHandler", g_noErr }, { "FindServiceWindow", g_FindServiceWindow },
    { "GetVideoDefault", g_GetVideoDefault }, { "MPPOpen", g_portNotCf }, { "PPCInit", g_noErr },
    { "OpenXPP", g_portNotCf }, { "ATPLoad", g_portNotCf }, { "MPPClose", g_noErr },
    /* ObjectSupportLib */
    { "AEObjectInit", g_noErr }, { "AESetObjectCallbacks", g_noErr }, { "AEInstallObjectAccessor", g_noErr },
    { "AERemoveObjectAccessor", g_noErr }, { "AEGetObjectAccessor", os_accessorNotFound },
    { "AEResolve", os_AEResolve }, { "GetDefaultUser", g_GetDefaultUser }, { "TruncText", g_TruncText },
    { "GetSoundOutputInfo", g_GetSoundOutputInfo }, { "StandardAlert", g_StandardAlert },
    { "GetScript", g_GetScript }, { "SetScript", g_SetScript }, { "GetEnvirons", g_GetEnvirons },
    { "SetEnvirons", g_SetEnvirons }, { "SetSoundOutputInfo", g_SetSoundOutputInfo },
    { "FindWordBreaks", g_FindWordBreaks }, { "UppercaseStripDiacritics", g_UppercaseStripDiacritics },
    { "PBControlImmed", g_PBControlImmed }, { "AECallObjectAccessor", os_notHandled }, { "AEDisposeToken", g_noErr },
    { "CreateObjSpecifier", os_coercionFail }, { "CreateOffsetDescriptor", os_coercionFail },
    { "CreateCompDescriptor", os_coercionFail }, { "CreateLogicalDescriptor", os_coercionFail },
    { "CreateRangeDescriptor", os_coercionFail },
    /* Internet Config */
    { "ICStart", ic_ICStart }, { "ICStop", g_noErr }, { "ICFindConfigFile", g_noErr },
    { "ICFindUserConfigFile", g_noErr }, { "ICGeneralFindConfigFile", g_noErr }, { "ICChooseConfig", g_noErr },
    { "ICBegin", g_noErr }, { "ICEnd", g_noErr }, { "ICGetPref", ic_prefNotFound },
    { "ICFindPrefHandle", ic_prefNotFound }, { "ICGetPrefHandle", ic_prefNotFound },
    { "ICSetPref", g_noErr }, { "ICSetPrefHandle", g_noErr }, { "ICCountPref", g_noErr },
    { "ICLaunchURL", ic_prefNotFound }, { "ICMapFilename", ic_prefNotFound }, { "ICMapTypeCreator", ic_prefNotFound },
    { "ICGetSeed", g_noErr }, { "ICGetVersion", g_noErr }, { "ICGetConfigName", g_noErr },
    /* Movie controllers */
    { "MCActivate", cc_MCActivate }, { "MCAdjustCursor", cc_MCAdjustCursor }, { "MCClear", cc_MCClear },
    { "MCClick", cc_MCClick }, { "MCCopy", cc_MCCopy }, { "MCCut", cc_MCCut }, { "MCDoAction", cc_MCDoAction },
    { "MCDraw", cc_MCDraw }, { "MCDrawBadge", cc_MCDrawBadge }, { "MCEnableEditing", cc_MCEnableEditing },
    { "MCGetClip", cc_MCGetClip }, { "MCGetControllerBoundsRect", cc_MCGetControllerBoundsRect },
    { "MCGetControllerBoundsRgn", cc_MCGetControllerBoundsRgn }, { "MCGetControllerInfo", cc_MCGetControllerInfo },
    { "MCGetControllerPort", cc_MCGetControllerPort }, { "MCGetCurrentTime", cc_MCGetCurrentTime },
    { "MCGetIndMovie", cc_MCGetIndMovie }, { "MCGetInterfaceElement", cc_MCGetInterfaceElement },
    { "MCGetMenuString", cc_MCGetMenuString }, { "MCGetVisible", cc_MCGetVisible },
    { "MCGetWindowRgn", cc_MCGetWindowRgn }, { "MCIdle", cc_MCIdle }, { "MCInvalidate", cc_MCInvalidate },
    { "MCIsControllerAttached", cc_MCIsControllerAttached }, { "MCIsEditingEnabled", cc_MCIsEditingEnabled },
    { "MCIsPlayerEvent", cc_MCIsPlayerEvent }, { "MCKey", cc_MCKey }, { "MCMovieChanged", cc_MCMovieChanged },
    { "MCNewAttachedController", cc_MCNewAttachedController }, { "MCPaste", cc_MCPaste },
    { "MCPositionController", cc_MCPositionController }, { "MCPtInController", cc_MCPtInController },
    { "MCRemoveAMovie", cc_MCRemoveAMovie }, { "MCRemoveAllMovies", cc_MCRemoveAllMovies },
    { "MCRemoveMovie", cc_MCRemoveAllMovies }, { "MCSetActionFilter", cc_MCSetActionFilter },
    { "MCSetActionFilterWithRefCon", cc_MCSetActionFilterWithRefCon }, { "MCSetClip", cc_MCSetClip },
    { "MCSetControllerAttached", cc_MCSetControllerAttached },
    { "MCSetControllerBoundsRect", cc_MCSetControllerBoundsRect }, { "MCSetControllerPort", cc_MCSetControllerPort },
    { "MCSetDuration", cc_MCSetDuration }, { "MCSetMovie", cc_MCSetMovie }, { "MCSetUpEditMenu", cc_MCSetUpEditMenu },
    { "MCSetVisible", cc_MCSetVisible }, { "MCUndo", cc_MCUndo },
    /* Component Manager */
    { "CaptureComponent", cm_CaptureComponent }, { "CloseComponent", cm_CloseComponent },
    { "CloseComponentResFile", cm_CloseComponentResFile }, { "CountComponentInstances", cm_CountComponentInstances },
    { "CountComponents", cm_CountComponents }, { "DelegateComponentCall", cm_DelegateComponentCall },
    { "FindNextComponent", cm_FindNextComponent }, { "GetComponentIconSuite", cm_GetComponentIconSuite },
    { "GetComponentIndString", cm_GetComponentIndString }, { "GetComponentInfo", cm_GetComponentInfo },
    { "GetComponentInstanceA5", cm_GetComponentInstanceA5 },
    { "GetComponentInstanceError", cm_GetComponentInstanceError },
    { "GetComponentInstanceStorage", cm_GetComponentInstanceStorage },
    { "GetComponentListModSeed", cm_GetComponentListModSeed }, { "GetComponentRefcon", cm_GetComponentRefcon },
    { "GetComponentResource", cm_GetComponentResource }, { "GetComponentTypeModSeed", cm_GetComponentTypeModSeed },
    { "OpenAComponent", cm_OpenAComponent }, { "OpenAComponentResFile", cm_OpenAComponentResFile },
    { "OpenADefaultComponent", cm_OpenADefaultComponent }, { "OpenComponent", cm_OpenComponent },
    { "OpenComponentResFile", cm_OpenComponentResFile }, { "OpenDefaultComponent", cm_OpenDefaultComponent },
    { "RegisterComponent", cm_RegisterComponent }, { "RegisterComponentResource", cm_RegisterComponentResource },
    { "RegisterComponentResourceFile", cm_RegisterComponentResourceFile },
    { "ResolveComponentAlias", cm_ResolveComponentAlias }, { "SetComponentInstanceA5", cm_SetComponentInstanceA5 },
    { "SetComponentInstanceError", cm_SetComponentInstanceError },
    { "SetComponentInstanceStorage", cm_SetComponentInstanceStorage },
    { "SetComponentRefcon", cm_SetComponentRefcon }, { "SetDefaultComponent", cm_SetDefaultComponent },
    { "UncaptureComponent", cm_UncaptureComponent }, { "UnregisterComponent", cm_UnregisterComponent },
    /* MathLib */
    { "acos", m_acos }, { "acosh", m_acosh }, { "annuity", m_annuity }, { "asin", m_asin },
    { "asinh", m_asinh }, { "atan", m_atan }, { "atan2", m_atan2 }, { "atanh", m_atanh },
    { "cbrt", m_cbrt }, { "ceil", m_ceil }, { "compound", m_compound }, { "copysign", m_copysign },
    { "cos", m_cos }, { "cosh", m_cosh }, { "dec2num", m_dec2num }, { "dec2str", m_dec2str },
    { "dtox80", m_dtox80 }, { "erf", m_erf }, { "erfc", m_erfc }, { "exp", m_exp }, { "exp2", m_exp2 },
    { "expm1", m_expm1 }, { "fabs", m_fabs }, { "fdim", m_fdim }, { "feclearexcept", m_zero },
    { "fegetenv", m_zero }, { "fegetround", m_zero }, { "feholdexcept", m_zero }, { "feraiseexcept", m_zero },
    { "fesetenv", m_zero }, { "fesetround", m_zero }, { "fetestexcept", m_zero }, { "feupdateenv", m_zero },
    { "floor", m_floor }, { "fmax", m_fmax }, { "fmin", m_fmin }, { "fmod", m_fmod }, { "frexp", m_frexp },
    { "gamma", m_gamma }, { "hypot", m_hypot }, { "ldexp", m_ldexp }, { "ldtox80", m_ldtox80 },
    { "lgamma", m_lgamma }, { "log", m_log }, { "log10", m_log10 }, { "log1p", m_log1p },
    { "log2", m_log2 }, { "logb", m_logb }, { "modf", m_modf }, { "nan", m_nan },
    { "nearbyint", m_nearbyint }, { "nextafterd", m_nextafterd }, { "num2dec", m_num2dec }, { "pow", m_pow },
    { "randomx", m_randomx }, { "remainder", m_remainder }, { "remquo", m_remquo }, { "rint", m_rint },
    { "rinttol", m_rinttol }, { "round", m_round }, { "roundtol", m_roundtol }, { "scalb", m_scalb },
    { "sin", m_sin }, { "sinh", m_sinh }, { "sqrt", m_sqrt }, { "str2dec", m_str2dec }, { "tan", m_tan },
    { "tanh", m_tanh }, { "trunc", m_trunc }, { "x80tod", m_x80tod }, { "x80told", m_x80told },
    { "__fpclassifyd", m_fpclassifyd }, { "__inf", m_inf }, { "__isfinited", m_isfinited },
    { "__isnan", m_isnand }, { "__isnand", m_isnand }, { "__isnanf", m_isnand }, { "__isnormald", m_isnormald },
    { "__signbitd", m_signbitd },
    /* StdCLib */
    { "isalnum", s_isalnum }, { "isalpha", s_isalpha }, { "isdigit", s_isdigit }, { "isxdigit", s_isxdigit },
    { "isspace", s_isspace }, { "isupper", s_isupper }, { "islower", s_islower }, { "ispunct", s_ispunct },
    { "isprint", s_isprint }, { "isgraph", s_isgraph }, { "iscntrl", s_iscntrl }, { "isascii", s_isascii },
    { "sscanf", s_sscanf }, { "CrsrDevNextDevice", g_CrsrDevNextDevice }, { "CrsrDevMoveTo", g_noErr },
    { "CrsrDevMove", g_noErr }, { "EnableIdle", g_nothing }, { "DisableIdle", g_nothing },
    { "abs", s_abs }, { "atof", s_atof }, { "atoi", s_atoi }, { "atol", s_atoi }, { "calloc", s_calloc },
    { "clock", s_clock }, { "exit", s_exit }, { "abort", s_exit }, { "_exit", s_exit }, { "fflush", s_fflush },
    { "fprintf", s_fprintf }, { "free", s_free }, { "getenv", s_getenv }, { "labs", s_abs },
    { "longjmp", s_longjmp }, { "malloc", s_malloc }, { "memchr", s_memchr }, { "memcmp", s_memcmp },
    { "memcpy", s_memcpy }, { "memmove", s_memcpy }, { "memset", s_memset }, { "printf", s_printf },
    { "puts", s_puts }, { "qsort", s_qsort }, { "rand", s_rand }, { "realloc", s_realloc },
    { "setjmp", s_setjmp }, { "__setjmp", s_setjmp }, { "sprintf", s_sprintf }, { "srand", s_srand },
    { "strcat", s_strcat }, { "strchr", s_strchr }, { "strcmp", s_strcmp }, { "strcpy", s_strcpy },
    { "strlen", s_strlen }, { "strncat", s_strncat }, { "strncmp", s_strncmp }, { "strncpy", s_strncpy },
    { "strrchr", s_strrchr }, { "strstr", s_strstr }, { "strtol", s_strtol }, { "strtoul", s_strtoul },
    { "time", s_time }, { "tolower", s_tolower }, { "toupper", s_toupper }, { "vsprintf", s_vsprintf },
    /* InterfaceLib glue */
    { "ApplicationZone", g_ApplicationZone }, { "C2PStr", g_c2pstr }, { "CloseDriver", g_CloseDriver },
    { "CopyCStringToPascal", g_CopyCStringToPascal }, { "CopyPascalStringToC", g_CopyPascalStringToC },
    { "DialogCopy", g_DialogCopy }, { "DialogCut", g_DialogCut }, { "DialogDelete", g_DialogDelete },
    { "DialogPaste", g_DialogPaste }, { "EqualString", g_EqualString }, { "Fix2X", g_Fix2X },
    { "Frac2X", g_Frac2X }, { "GetDCtlEntry", g_GetDCtlEntry }, { "GetIndString", g_GetIndString },
    { "GetMBarHeight", g_GetMBarHeight }, { "GetOSTrapAddress", g_GetOSTrapAddress },
    { "GetSoundVol", g_GetSoundVol }, { "GetToolboxTrapAddress", g_GetToolboxTrapAddress },
    { "GetTrapAddress", g_GetTrapAddress }, { "GetWDInfo", g_GetWDInfo }, { "HLockHi", g_HLockHi },
    { "IUCompString", g_IUCompString }, { "IUEqualString", g_IUEqualString }, { "MakeDataExecutable", g_nothing },
    { "MemError", g_MemError }, { "NGetTrapAddress", g_NGetTrapAddress }, { "NSetTrapAddress", g_NSetTrapAddress },
    { "OpenDriver", g_OpenDriver }, { "P2CStr", g_p2cstr }, { "RelString", g_RelString },
    { "SetA5", g_SetA5 }, { "SetCurrentA5", g_SetCurrentA5 }, { "SetOSTrapAddress", g_SetOSTrapAddress },
    { "SetSoundVol", g_SetSoundVol }, { "SetToolboxTrapAddress", g_SetToolboxTrapAddress },
    { "SysBreak", g_nothing }, { "SysBreakFunc", g_nothing }, { "SysBreakStr", g_nothing },
    { "SystemZone", g_SystemZone }, { "UpperString", g_UpperString }, { "X2Fix", g_X2Fix },
    { "UpperText", g_UpperText }, { "UppercaseText", g_UpperText }, { "LowerText", g_LowerText },
    { "LowercaseText", g_LowerText }, { "GetEvQHdr", g_GetEvQHdr }, { "GetVCBQHdr", g_GetVCBQHdr },
    { "GetDrvQHdr", g_GetDrvQHdr }, { "GetFSQHdr", g_GetFSQHdr }, { "GetVBLQHdr", g_GetVBLQHdr },
    { "GetApplLimit", g_GetApplLimit }, { "GetGrayRgn", g_GetGrayRgn }, { "GetAppFont", g_GetAppFont },
    { "GetSysFont", g_GetSysFont }, { "GetDefFontSize", g_GetDefFontSize }, { "GetTime", g_GetTime },
    { "Microseconds", g_Microseconds }, { "ReadLocation", g_ReadLocation }, { "Control", g_Control },
    { "Status", g_Status }, { "KillIO", g_KillIO },
    { "StdFilterProc", g_StdFilterProc },
    { "X2Frac", g_X2Frac }, { "_BreakPoint", g_nothing }, { "c2pstr", g_c2pstr },
    { "c2pstrcpy", g_c2pstrcpy }, { "p2cstr", g_p2cstr }, { "p2cstrcpy", g_p2cstrcpy },
    { "FlushCodeCache", g_nothing }, { "FlushDataCache", g_nothing }, { "FlushCodeCacheRange", g_noErr },
};
const unsigned lib_export_count = sizeof lib_exports / sizeof lib_exports[0];

/* ---------------------------------------------------------------------- */
/* Data */

/* StdCLib's character classes, MPW style: upper 1, lower 2, digit 4,
 * space 8, punctuation 16, control 32, blank 64, hex digit 128.
 * __p_CType points at the entry for 0, with one for EOF before it. */
static gaddr ctype_table(void)
{
    gaddr t = tb_NewPtr(258, true, true);
    for (int ch = 0; ch < 256; ch++) {
        uint8_t k = 0;
        if (ch < 128) {
            if (ch >= 'A' && ch <= 'Z')
                k |= 1;
            if (ch >= 'a' && ch <= 'z')
                k |= 2;
            if (ch >= '0' && ch <= '9')
                k |= 4 | 128;
            if ((ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))
                k |= 128;
            if (ch == ' ' || (ch >= 9 && ch <= 13))
                k |= 8;
            if (ch == ' ')
                k |= 64;
            if (ch < 32 || ch == 127)
                k |= 32;
            if (ch > 32 && ch < 127 && !(k & 7))
                k |= 16;
        }
        wr8(t + 1 + (gaddr)ch, k);
    }
    return t + 1;
}

gaddr ppc_data_export(const char *name)
{
    if (!strcmp(name, "__p_CType")) {
        gaddr p = tb_NewPtr(4, true, true);
        wr32(p, ctype_table());
        return p;
    }
    if (!strcmp(name, "CallComponentUPP"))
        return mm_call_component_upp();
    if (!strcmp(name, "_DBL_EPSILON") || !strcmp(name, "_DBL_MIN") || !strcmp(name, "_DBL_MAX")) {
        gaddr p = tb_NewPtr(8, true, true);
        wr_double(p, !strcmp(name, "_DBL_EPSILON") ? 2.220446049250313e-16
                     : !strcmp(name, "_DBL_MIN") ? 2.2250738585072014e-308 : 1.7976931348623157e308);
        return p;
    }
    if (!strcmp(name, "pi")) {
        gaddr p = tb_NewPtr(8, true, true);
        wr_double(p, M_PI);
        return p;
    }
    static const char *const zeroed[] = { "_IntEnv", "_exit_status", "__target_for_exit", "__NubAt3",
                                          "__C_phase", "_iob", "StandAlone", "errno", "__huge_val" };
    for (unsigned i = 0; i < sizeof zeroed / sizeof zeroed[0]; i++)
        if (!strcmp(name, zeroed[i]))
            return tb_NewPtr(1024, true, true);
    return 0;
}
