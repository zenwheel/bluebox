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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "machine.h"
#include "ppc_bridge.h"
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
static void s_calloc(ppc_cpu *c) { RETURN(tb_NewPtr((int32_t)(R(0) * R(1)), true, false)); }
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

const ppc_export lib_exports[] = {
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
    { "Status", g_Status }, { "KillIO", g_KillIO }, { "CountDITL", g_CountDITL },
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
