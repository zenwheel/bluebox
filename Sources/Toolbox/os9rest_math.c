/*
 * MathLib's complex arithmetic (the dcomplex routines of Apple's complex
 * header): a dcomplex is two doubles, the real part then the imaginary.
 * A routine returning one gets the address of its result in r3 and the
 * arguments after; a dcomplex argument takes four words of the GPR slots
 * and the parameter area in turn, a double argument one FPR and two slots.
 */
#include <complex.h>
#include <math.h>

#include "os9rest.h"
#include "tb_internal.h"

typedef double _Complex cx;

static double word_pair(const ppc_cpu *c, unsigned n)
{
    uint64_t b = (uint64_t)os9rest_word(c, n) << 32 | os9rest_word(c, n + 1);
    double d;
    __builtin_memcpy(&d, &b, 8);
    return d;
}

/* The dcomplex argument starting at slot n (slot 0 is r3). */
static cx arg_cx(const ppc_cpu *c, unsigned n) { return word_pair(c, n) + word_pair(c, n + 2) * I; }

static void put_cx(ppc_cpu *c, gaddr to, cx z)
{
    double d[2] = { creal(z), cimag(z) };
    for (int i = 0; i < 2; i++) {
        uint64_t b;
        __builtin_memcpy(&b, &d[i], 8);
        wr32(to + 8u * (uint32_t)i, (uint32_t)(b >> 32));
        wr32(to + 8u * (uint32_t)i + 4, (uint32_t)b);
    }
    c->pc = c->lr;
}

static void ret_real(ppc_cpu *c, double v)
{
    ppc_set_fpr(c, 1, v);
    c->pc = c->lr;
}

#define UNARY(name, expr) \
    static void m_##name(ppc_cpu *c) { cx z = arg_cx(c, 1); (void)z; put_cx(c, c->gpr[3], expr); }
#define BINARY(name, expr) \
    static void m_##name(ppc_cpu *c) { cx a = arg_cx(c, 1), b = arg_cx(c, 5); put_cx(c, c->gpr[3], expr); }

BINARY(cadd, a + b)
BINARY(csub, a - b)
BINARY(cmul, a * b)
BINARY(cdiv, a / b)
UNARY(cconj, conj(z))
UNARY(csquare, z * z)
UNARY(cexp, cexp(z))
UNARY(cLog, clog(z))
UNARY(csqrt, csqrt(z))
UNARY(csin, csin(z))
UNARY(ccos, ccos(z))
UNARY(ctan, ctan(z))
UNARY(casin, casin(z))
UNARY(cacos, cacos(z))
UNARY(catan, catan(z))
UNARY(csinh, csinh(z))
UNARY(ccosh, ccosh(z))
UNARY(ctanh, ctanh(z))
UNARY(casinh, casinh(z))
UNARY(cacosh, cacosh(z))
UNARY(catanh, catanh(z))

/* cabs(z) and carg(z) return a double and take z from r3 on. */
static void m_cabs(ppc_cpu *c) { ret_real(c, cabs(arg_cx(c, 0))); }
static void m_carg(ppc_cpu *c) { ret_real(c, carg(arg_cx(c, 0))); }

/* xdivc(x, z): the real x over z, x in f1. */
static void m_xdivc(ppc_cpu *c) { put_cx(c, c->gpr[3], ppc_fpr(c, 1) / arg_cx(c, 3)); }

/* cxpwri(z, n): z to the integer power n, by repeated squaring so that
 * exact cases stay exact. */
static void m_cxpwri(ppc_cpu *c)
{
    cx z = arg_cx(c, 1);
    int32_t n = (int32_t)os9rest_word(c, 5);
    uint32_t k = n < 0 ? 0u - (uint32_t)n : (uint32_t)n;
    cx r = 1;
    for (cx b = z; k; k >>= 1, b *= b)
        if (k & 1)
            r *= b;
    put_cx(c, c->gpr[3], n < 0 ? 1 / r : r);
}

/* cxpwry(z, w): z to the complex power w. */
static void m_cxpwry(ppc_cpu *c)
{
    cx z = arg_cx(c, 1), w = arg_cx(c, 5);
    put_cx(c, c->gpr[3], z == 0 ? (w == 0 ? 1 : 0) : cexp(w * clog(z)));
}

/* cxpwre(z, e): z to the real power e (in f1). */
static void m_cxpwre(ppc_cpu *c)
{
    cx z = arg_cx(c, 1);
    put_cx(c, c->gpr[3], z == 0 ? (ppc_fpr(c, 1) == 0 ? 1 : 0) : cexp(ppc_fpr(c, 1) * clog(z)));
}

/* cepwry(y, w): the real y (f1) to the complex power w. */
static void m_cepwry(ppc_cpu *c)
{
    double y = ppc_fpr(c, 1);
    cx w = arg_cx(c, 3);
    put_cx(c, c->gpr[3], y == 0 ? (w == 0 ? 1 : 0) : cexp(w * log(y)));
}

const ppc_export os9rest_math_exports[] = {
    { "cadd", m_cadd }, { "csub", m_csub }, { "cmul", m_cmul }, { "cdiv", m_cdiv },
    { "cconj", m_cconj }, { "csquare", m_csquare }, { "cexp", m_cexp }, { "cLog", m_cLog },
    { "csqrt", m_csqrt }, { "csin", m_csin }, { "ccos", m_ccos }, { "ctan", m_ctan },
    { "casin", m_casin }, { "cacos", m_cacos }, { "catan", m_catan }, { "csinh", m_csinh },
    { "ccosh", m_ccosh }, { "ctanh", m_ctanh }, { "casinh", m_casinh }, { "cacosh", m_cacosh },
    { "catanh", m_catanh }, { "cabs", m_cabs }, { "carg", m_carg }, { "xdivc", m_xdivc },
    { "cxpwri", m_cxpwri }, { "cxpwry", m_cxpwry }, { "cxpwre", m_cxpwre }, { "cepwry", m_cepwry },
};
const unsigned os9rest_math_export_count = sizeof os9rest_math_exports / sizeof os9rest_math_exports[0];
