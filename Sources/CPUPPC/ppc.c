/*
 * PowerPC interpreter (32-bit user mode).  See ppc.h.
 *
 * Instruction fields follow the architecture books' names: rD/rS bits 6-10,
 * rA 11-15, rB 16-20, frC 21-25, the 10-bit extended opcode bits 21-30 and
 * Rc bit 31 (bit 0 being the most significant).
 */
#include <fenv.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ppc.h"

/* The program's stores, watched (mem.h). */
static inline void ppc_wr8(gaddr a, uint8_t v) { watch_store(a); wr8(a, v); }
static inline void ppc_wr16(gaddr a, uint16_t v) { watch_store(a); wr16(a, v); }
static inline void ppc_wr32(gaddr a, uint32_t v) { watch_store(a); wr32(a, v); }
#define wr8 ppc_wr8
#define wr16 ppc_wr16
#define wr32 ppc_wr32

/* ---------------------------------------------------------------------- */
/* Fields */

#define OPCD(op) ((op) >> 26)
#define RD(op) (((op) >> 21) & 31)
#define RA(op) (((op) >> 16) & 31)
#define RB(op) (((op) >> 11) & 31)
#define RC_(op) (((op) >> 6) & 31)
#define XO10(op) (((op) >> 1) & 0x3FF)
#define XO9(op) (((op) >> 1) & 0x1FF)
#define XO5(op) (((op) >> 1) & 31)
#define RCBIT(op) ((op) & 1)
#define OEBIT(op) (((op) >> 10) & 1)
#define SIMM(op) ((int32_t)(int16_t)((op) & 0xFFFF))
#define UIMM(op) ((op) & 0xFFFF)

void ppc_init(ppc_cpu *c)
{
    memset(c, 0, sizeof *c);
}

void ppc_halt(ppc_cpu *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->halt_reason, sizeof c->halt_reason, fmt, ap);
    va_end(ap);
    c->halted = true;
}

static void illegal(ppc_cpu *c)
{
    if (c->op_pc != c->prev_pc + 4)
        ppc_halt(c, "illegal PowerPC instruction %08X at %08X (came from %08X, lr %08X)", c->op, c->op_pc,
                 c->prev_pc, c->lr);
    else
        ppc_halt(c, "illegal PowerPC instruction %08X at %08X", c->op, c->op_pc);
    if (c->illegal)
        c->illegal(c);
}

/* ---------------------------------------------------------------------- */
/* Condition register and XER */

static void set_crf(ppc_cpu *c, int n, uint32_t bits)
{
    int sh = 28 - 4 * n;
    c->cr = (c->cr & ~(0xFu << sh)) | ((bits & 0xF) << sh);
}

static void set_cr0(ppc_cpu *c, uint32_t v)
{
    int32_t s = (int32_t)v;
    uint32_t bits = s < 0 ? 8 : s > 0 ? 4 : 2;
    if (c->xer & PPC_XER_SO)
        bits |= 1;
    set_crf(c, 0, bits);
}

static void set_ca(ppc_cpu *c, bool ca)
{
    if (ca)
        c->xer |= PPC_XER_CA;
    else
        c->xer &= ~PPC_XER_CA;
}

static void set_ov(ppc_cpu *c, bool ov)
{
    if (ov)
        c->xer |= PPC_XER_OV | PPC_XER_SO;
    else
        c->xer &= ~PPC_XER_OV;
}

static bool cr_bit(const ppc_cpu *c, int bit) { return (c->cr >> (31 - bit)) & 1; }

static void set_cr_bit(ppc_cpu *c, int bit, bool v)
{
    if (v)
        c->cr |= 1u << (31 - bit);
    else
        c->cr &= ~(1u << (31 - bit));
}

/* ---------------------------------------------------------------------- */
/* Memory */

static uint64_t rd64(uint32_t a) { return (uint64_t)rd32(a) << 32 | rd32(a + 4); }
static void wr64(uint32_t a, uint64_t v)
{
    wr32(a, (uint32_t)(v >> 32));
    wr32(a + 4, (uint32_t)v);
}

static uint32_t ea_d(ppc_cpu *c, uint32_t op) { return (RA(op) ? c->gpr[RA(op)] : 0) + (uint32_t)SIMM(op); }
static uint32_t ea_x(ppc_cpu *c, uint32_t op) { return (RA(op) ? c->gpr[RA(op)] : 0) + c->gpr[RB(op)]; }

static uint32_t bswap16(uint32_t v) { return ((v & 0xFF) << 8) | ((v >> 8) & 0xFF); }
static uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }

/* ---------------------------------------------------------------------- */
/* Floating point */

static double fget(ppc_cpu *c, int n) { return ppc_fpr(c, n); }
static void fset(ppc_cpu *c, int n, double d) { ppc_set_fpr(c, n, d); }
static double single(double d) { return (double)(float)d; }

static float u32_float(uint32_t v)
{
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static uint32_t float_u32(float f)
{
    uint32_t v;
    memcpy(&v, &f, 4);
    return v;
}

/* The FPSCR's floating-point result flags (FPRF, bits 15-19) for d. */
static void set_fprf(ppc_cpu *c, double d)
{
    uint32_t f;
    if (isnan(d))
        f = 0x11; /* quiet NaN */
    else if (isinf(d))
        f = d < 0 ? 0x09 : 0x05;
    else if (d == 0)
        f = signbit(d) ? 0x12 : 0x02;
    else if (fpclassify(d) == FP_SUBNORMAL)
        f = d < 0 ? 0x18 : 0x14;
    else
        f = d < 0 ? 0x08 : 0x04;
    c->fpscr = (c->fpscr & ~(0x1Fu << 12)) | (f << 12);
}

static void fp_result(ppc_cpu *c, uint32_t op, int d, double v, bool is_single)
{
    if (is_single)
        v = single(v);
    fset(c, d, v);
    set_fprf(c, v);
    if (RCBIT(op))
        set_crf(c, 1, c->fpscr >> 28);
}

/* Round to a 32-bit integer by the FPSCR's rounding mode (or toward zero). */
static uint32_t to_int32(ppc_cpu *c, double d, bool toward_zero)
{
    if (isnan(d)) {
        c->fpscr |= 0x80000100u; /* FX, VXCVI */
        return 0x80000000u;
    }
    double r;
    int rn = toward_zero ? 1 : (int)(c->fpscr & 3);
    switch (rn) {
    case 0: r = nearbyint(d); break; /* the host's default: nearest, ties to even */
    case 1: r = trunc(d); break;
    case 2: r = ceil(d); break;
    default: r = floor(d); break;
    }
    if (r > 2147483647.0) {
        c->fpscr |= 0x80000100u;
        return 0x7FFFFFFFu;
    }
    if (r < -2147483648.0) {
        c->fpscr |= 0x80000100u;
        return 0x80000000u;
    }
    if (r != d)
        c->fpscr |= 0x02000000u; /* XX (inexact) */
    return (uint32_t)(int32_t)r;
}

static void fcmp(ppc_cpu *c, uint32_t op)
{
    int bf = RD(op) >> 2;
    double a = fget(c, RA(op)), b = fget(c, RB(op));
    uint32_t bits = isnan(a) || isnan(b) ? 1 : a < b ? 8 : a > b ? 4 : 2;
    set_crf(c, bf, bits);
    c->fpscr = (c->fpscr & ~(0xFu << 12)) | (bits << 12);
}

static void op63_x(ppc_cpu *c, uint32_t op)
{
    int d = RD(op), b = RB(op);
    switch (XO10(op)) {
    case 0:   /* fcmpu */
    case 32:  /* fcmpo */
        fcmp(c, op);
        return;
    case 12: /* frsp */
        fp_result(c, op, d, fget(c, b), true);
        return;
    case 14: /* fctiw */
    case 15: /* fctiwz */
        c->fpr[d] = 0xFFF8000000000000ull | to_int32(c, fget(c, b), XO10(op) == 15);
        if (RCBIT(op))
            set_crf(c, 1, c->fpscr >> 28);
        return;
    case 38: /* mtfsb1 */
        c->fpscr |= 0x80000000u >> d;
        break;
    case 70: /* mtfsb0 */
        c->fpscr &= ~(0x80000000u >> d);
        break;
    case 40: /* fneg */
        c->fpr[d] = c->fpr[b] ^ 0x8000000000000000ull;
        break;
    case 72: /* fmr */
        c->fpr[d] = c->fpr[b];
        break;
    case 136: /* fnabs */
        c->fpr[d] = c->fpr[b] | 0x8000000000000000ull;
        break;
    case 264: /* fabs */
        c->fpr[d] = c->fpr[b] & ~0x8000000000000000ull;
        break;
    case 64: { /* mcrfs */
        int bf = d >> 2, bfa = RA(op) >> 2;
        uint32_t bits = (c->fpscr >> (28 - 4 * bfa)) & 0xF;
        set_crf(c, bf, bits);
        if (bfa < 4) /* the exception bits copied are cleared */
            c->fpscr &= ~((bits & (bfa == 0 ? 0x9u : 0xFu)) << (28 - 4 * bfa));
        return;
    }
    case 134: { /* mtfsfi */
        int bf = d >> 2;
        uint32_t imm = (op >> 12) & 0xF;
        c->fpscr = (c->fpscr & ~(0xFu << (28 - 4 * bf))) | (imm << (28 - 4 * bf));
        break;
    }
    case 583: /* mffs */
        c->fpr[d] = 0xFFF8000000000000ull | c->fpscr;
        break;
    case 711: { /* mtfsf */
        uint32_t fm = (op >> 17) & 0xFF, mask = 0;
        for (int i = 0; i < 8; i++)
            if (fm & (0x80 >> i))
                mask |= 0xF0000000u >> (4 * i);
        c->fpscr = (c->fpscr & ~mask) | ((uint32_t)c->fpr[b] & mask);
        break;
    }
    default:
        illegal(c);
        return;
    }
    if (RCBIT(op))
        set_crf(c, 1, c->fpscr >> 28);
}

/* The A-form arithmetic of opcodes 59 (single) and 63 (double). */
static void fp_arith(ppc_cpu *c, uint32_t op, bool is_single)
{
    int d = RD(op);
    double a = fget(c, RA(op)), b = fget(c, RB(op)), cc = fget(c, RC_(op));
    double r;
    switch (XO5(op)) {
    case 18: r = a / b; break;                   /* fdiv */
    case 20: r = a - b; break;                   /* fsub */
    case 21: r = a + b; break;                   /* fadd */
    case 22: r = sqrt(b); break;                 /* fsqrt */
    case 23: r = a >= 0.0 ? cc : b; break;       /* fsel (NaN takes b) */
    case 24: r = 1.0 / b; break;                 /* fres */
    case 25: r = a * cc; break;                  /* fmul */
    case 26: r = 1.0 / sqrt(b); break;           /* frsqrte */
    case 28: r = fma(a, cc, -b); break;          /* fmsub */
    case 29: r = fma(a, cc, b); break;           /* fmadd */
    case 30: r = -fma(a, cc, -b); break;         /* fnmsub */
    case 31: r = -fma(a, cc, b); break;          /* fnmadd */
    default:
        if (!is_single) {
            op63_x(c, op);
            return;
        }
        illegal(c);
        return;
    }
    if (XO5(op) == 23) { /* fsel sets no flags */
        fset(c, d, r);
        if (RCBIT(op))
            set_crf(c, 1, c->fpscr >> 28);
        return;
    }
    fp_result(c, op, d, r, is_single || XO5(op) == 24);
}

/* ---------------------------------------------------------------------- */
/* Rotates */

static uint32_t mask(int mb, int me)
{
    uint32_t m1 = 0xFFFFFFFFu >> mb, m2 = 0xFFFFFFFFu << (31 - me);
    return mb <= me ? m1 & m2 : m1 | m2;
}

static uint32_t rotl(uint32_t v, int n) { return n ? (v << n) | (v >> (32 - n)) : v; }

/* ---------------------------------------------------------------------- */
/* Branches */

static bool branch_cond(ppc_cpu *c, uint32_t op)
{
    int bo = RD(op), bi = RA(op);
    if (!(bo & 4))
        c->ctr--;
    bool ctr_ok = (bo & 4) || ((c->ctr != 0) ^ ((bo & 2) != 0));
    bool cond_ok = (bo & 16) || (cr_bit(c, bi) == ((bo >> 3) & 1));
    return ctr_ok && cond_ok;
}

static void op19(ppc_cpu *c, uint32_t op)
{
    int d = RD(op), a = RA(op), b = RB(op);
    switch (XO10(op)) {
    case 0: /* mcrf */
        set_crf(c, d >> 2, ppc_cr_field(c, a >> 2));
        return;
    case 16: { /* bclr */
        uint32_t target = c->lr & ~3u;
        if (branch_cond(c, op)) {
            if (RCBIT(op))
                c->lr = c->pc;
            c->pc = target;
        } else if (RCBIT(op)) {
            c->lr = c->pc;
        }
        return;
    }
    case 528: { /* bcctr */
        uint32_t target = c->ctr & ~3u;
        int bo = d;
        bool cond_ok = (bo & 16) || (cr_bit(c, a) == ((bo >> 3) & 1));
        if (RCBIT(op))
            c->lr = c->pc;
        if (cond_ok)
            c->pc = target;
        return;
    }
    case 33: set_cr_bit(c, d, !(cr_bit(c, a) || cr_bit(c, b))); return;  /* crnor */
    case 129: set_cr_bit(c, d, cr_bit(c, a) && !cr_bit(c, b)); return;    /* crandc */
    case 193: set_cr_bit(c, d, cr_bit(c, a) != cr_bit(c, b)); return;     /* crxor */
    case 225: set_cr_bit(c, d, !(cr_bit(c, a) && cr_bit(c, b))); return;  /* crnand */
    case 257: set_cr_bit(c, d, cr_bit(c, a) && cr_bit(c, b)); return;     /* crand */
    case 289: set_cr_bit(c, d, cr_bit(c, a) == cr_bit(c, b)); return;     /* creqv */
    case 417: set_cr_bit(c, d, cr_bit(c, a) || !cr_bit(c, b)); return;    /* crorc */
    case 449: set_cr_bit(c, d, cr_bit(c, a) || cr_bit(c, b)); return;     /* cror */
    case 150: return;                                                   /* isync */
    default:
        illegal(c);
    }
}

/* ---------------------------------------------------------------------- */
/* Opcode 31 */

static uint32_t spr_num(uint32_t op) { return ((op >> 16) & 31) | (((op >> 11) & 31) << 5); }

static void trap_if(ppc_cpu *c, int to, int32_t a, int32_t b)
{
    bool t = ((to & 16) && a < b) || ((to & 8) && a > b) || ((to & 4) && a == b) ||
             ((to & 2) && (uint32_t)a < (uint32_t)b) || ((to & 1) && (uint32_t)a > (uint32_t)b);
    if (t)
        ppc_halt(c, "PowerPC trap at %08X", c->op_pc);
}

/* XO-form arithmetic: the result, with CA and OV as the operation sets them. */
static bool arith31(ppc_cpu *c, uint32_t op)
{
    uint32_t a = c->gpr[RA(op)], b = c->gpr[RB(op)];
    uint32_t r;
    bool ov = false;
    bool ca_in = (c->xer & PPC_XER_CA) != 0;
    switch (XO9(op)) {
    case 266: /* add */
        r = a + b;
        ov = ((a ^ r) & (b ^ r)) >> 31;
        break;
    case 10: /* addc */
        r = a + b;
        set_ca(c, r < a);
        ov = ((a ^ r) & (b ^ r)) >> 31;
        break;
    case 138: { /* adde */
        uint64_t s = (uint64_t)a + b + ca_in;
        r = (uint32_t)s;
        set_ca(c, s >> 32);
        ov = ((a ^ r) & (b ^ r)) >> 31;
        break;
    }
    case 234: { /* addme */
        uint64_t s = (uint64_t)a + 0xFFFFFFFFu + ca_in;
        r = (uint32_t)s;
        set_ca(c, s >> 32);
        ov = ((a ^ r) & (0xFFFFFFFFu ^ r)) >> 31;
        break;
    }
    case 202: { /* addze */
        uint64_t s = (uint64_t)a + ca_in;
        r = (uint32_t)s;
        set_ca(c, s >> 32);
        ov = (~a & r) >> 31;
        break;
    }
    case 40: /* subf: b - a */
        r = b - a;
        ov = ((~a ^ r) & (b ^ r)) >> 31;
        break;
    case 8: /* subfc */
        r = b - a;
        set_ca(c, b >= a);
        ov = ((~a ^ r) & (b ^ r)) >> 31;
        break;
    case 136: { /* subfe: ~a + b + CA */
        uint64_t s = (uint64_t)(~a) + b + ca_in;
        r = (uint32_t)s;
        set_ca(c, s >> 32);
        ov = ((~a ^ r) & (b ^ r)) >> 31;
        break;
    }
    case 232: { /* subfme */
        uint64_t s = (uint64_t)(~a) + 0xFFFFFFFFu + ca_in;
        r = (uint32_t)s;
        set_ca(c, s >> 32);
        ov = ((~a ^ r) & (0xFFFFFFFFu ^ r)) >> 31;
        break;
    }
    case 200: { /* subfze */
        uint64_t s = (uint64_t)(~a) + ca_in;
        r = (uint32_t)s;
        set_ca(c, s >> 32);
        ov = (a & r) >> 31;
        break;
    }
    case 104: /* neg */
        r = (uint32_t)-(int64_t)(int32_t)a;
        ov = a == 0x80000000u;
        break;
    case 235: { /* mullw */
        int64_t p = (int64_t)(int32_t)a * (int32_t)b;
        r = (uint32_t)p;
        ov = p != (int32_t)p;
        break;
    }
    case 75: /* mulhw */
        r = (uint32_t)(((int64_t)(int32_t)a * (int32_t)b) >> 32);
        break;
    case 11: /* mulhwu */
        r = (uint32_t)(((uint64_t)a * b) >> 32);
        break;
    case 491: /* divw */
        if (b == 0 || (a == 0x80000000u && b == 0xFFFFFFFFu)) {
            r = ((int32_t)a < 0 && b == 0) ? 0xFFFFFFFFu : 0; /* undefined; as the 750 */
            ov = true;
        } else {
            r = (uint32_t)((int32_t)a / (int32_t)b);
        }
        break;
    case 459: /* divwu */
        if (b == 0) {
            r = 0;
            ov = true;
        } else {
            r = a / b;
        }
        break;
    /* The POWER instructions the 601 kept, which Mac OS ran on later
     * processors too: FutureBASIC^3 compiles division to divs. */
    case 360: /* abs */
        r = (int32_t)a < 0 ? 0 - a : a;
        ov = a == 0x80000000u;
        break;
    case 488: /* nabs */
        r = (int32_t)a > 0 ? 0 - a : a;
        break;
    case 264: /* doz: b - a, or 0 if a > b */
        r = (int32_t)a > (int32_t)b ? 0 : b - a;
        ov = (int32_t)a <= (int32_t)b && (int64_t)(int32_t)b - (int32_t)a != (int32_t)r;
        break;
    case 107: { /* mul: the high word, the low one to MQ (and CR0 by it) */
        int64_t p = (int64_t)(int32_t)a * (int32_t)b;
        r = (uint32_t)((uint64_t)p >> 32);
        c->mq = (uint32_t)p;
        ov = p != (int32_t)p;
        c->gpr[RD(op)] = r;
        if (OEBIT(op))
            set_ov(c, ov);
        if (RCBIT(op))
            set_cr0(c, c->mq);
        return true;
    }
    case 331:   /* div: (rA || MQ) / rB */
    case 363: { /* divs: rA / rB; the remainder to MQ (and CR0 by it) */
        int64_t n = XO9(op) == 331 ? (int64_t)((uint64_t)a << 32 | c->mq) : (int32_t)a;
        int64_t q = 0, m = 0;
        if (b == 0 || (n == INT64_MIN && b == 0xFFFFFFFFu)) {
            ov = true;
        } else {
            q = n / (int32_t)b;
            m = n % (int32_t)b;
            ov = q != (int32_t)q;
        }
        r = ov ? 0x80000000u : (uint32_t)q;
        c->mq = ov ? 0 : (uint32_t)m;
        c->gpr[RD(op)] = r;
        if (OEBIT(op))
            set_ov(c, ov);
        if (RCBIT(op))
            set_cr0(c, c->mq);
        return true;
    }
    default:
        return false;
    }
    c->gpr[RD(op)] = r;
    if (OEBIT(op) && XO9(op) != 75 && XO9(op) != 11)
        set_ov(c, ov);
    if (RCBIT(op))
        set_cr0(c, r);
    return true;
}

static void load_string(ppc_cpu *c, uint32_t ea, int rd, uint32_t n)
{
    int r = rd - 1, i = 4;
    for (uint32_t k = 0; k < n; k++) {
        if (i == 4) {
            r = (r + 1) & 31;
            c->gpr[r] = 0;
            i = 0;
        }
        c->gpr[r] |= (uint32_t)rd8(ea + k) << (24 - 8 * i);
        i++;
    }
}

static void store_string(ppc_cpu *c, uint32_t ea, int rs, uint32_t n)
{
    int r = rs - 1, i = 4;
    for (uint32_t k = 0; k < n; k++) {
        if (i == 4) {
            r = (r + 1) & 31;
            i = 0;
        }
        wr8(ea + k, (uint8_t)(c->gpr[r] >> (24 - 8 * i)));
        i++;
    }
}

static void op31(ppc_cpu *c, uint32_t op)
{
    if (arith31(c, op))
        return;
    int d = RD(op), a = RA(op), b = RB(op);
    uint32_t rs = c->gpr[d], rb = c->gpr[b], r = 0;
    bool logical = true; /* result to rA, CR0 by Rc */
    switch (XO10(op)) {
    case 0: /* cmp */
    case 32: { /* cmpl */
        int bf = d >> 2;
        uint32_t x = c->gpr[a], y = rb;
        uint32_t bits;
        if (XO10(op) == 0)
            bits = (int32_t)x < (int32_t)y ? 8 : (int32_t)x > (int32_t)y ? 4 : 2;
        else
            bits = x < y ? 8 : x > y ? 4 : 2;
        if (c->xer & PPC_XER_SO)
            bits |= 1;
        set_crf(c, bf, bits);
        return;
    }
    case 4: /* tw */
        trap_if(c, d, (int32_t)c->gpr[a], (int32_t)rb);
        return;
    case 19: /* mfcr */
        c->gpr[d] = c->cr;
        return;
    case 144: { /* mtcrf */
        uint32_t fxm = (op >> 12) & 0xFF, m = 0;
        for (int i = 0; i < 8; i++)
            if (fxm & (0x80 >> i))
                m |= 0xF0000000u >> (4 * i);
        c->cr = (c->cr & ~m) | (rs & m);
        return;
    }
    case 512: /* mcrxr */
        set_crf(c, d >> 2, c->xer >> 28);
        c->xer &= 0x0FFFFFFFu;
        return;
    case 83: /* mfmsr: user mode, as a program sees it */
        c->gpr[d] = 0x0000D032u;
        return;
    case 146: /* mtmsr */
    case 210: /* mtsr */
    case 242: /* mtsrin */
        return;
    case 595: /* mfsr */
    case 659: /* mfsrin */
        c->gpr[d] = 0;
        return;
    case 339: { /* mfspr */
        switch (spr_num(op)) {
        case 1: c->gpr[d] = c->xer; break;
        case 8: c->gpr[d] = c->lr; break;
        case 9: c->gpr[d] = c->ctr; break;
        case 268: c->gpr[d] = (uint32_t)c->tb; break;
        case 269: c->gpr[d] = (uint32_t)(c->tb >> 32); break;
        case 0: c->gpr[d] = c->mq; break;
        case 287: c->gpr[d] = 0x00080200u; break; /* PVR: a 750 (G3) */
        default: c->gpr[d] = 0; break;
        }
        return;
    }
    case 371: /* mftb */
        c->gpr[d] = spr_num(op) == 269 ? (uint32_t)(c->tb >> 32) : (uint32_t)c->tb;
        return;
    case 467: /* mtspr */
        switch (spr_num(op)) {
        case 0: c->mq = rs; break;
        case 1: c->xer = rs; break;
        case 8: c->lr = rs; break;
        case 9: c->ctr = rs; break;
        default: break;
        }
        return;
    /* Loads and stores */
    case 23: c->gpr[d] = rd32(ea_x(c, op)); return;                                   /* lwzx */
    case 55: { uint32_t e = ea_x(c, op); c->gpr[d] = rd32(e); c->gpr[a] = e; return; } /* lwzux */
    case 87: c->gpr[d] = rd8(ea_x(c, op)); return;                                    /* lbzx */
    case 119: { uint32_t e = ea_x(c, op); c->gpr[d] = rd8(e); c->gpr[a] = e; return; }
    case 279: c->gpr[d] = rd16(ea_x(c, op)); return;                                  /* lhzx */
    case 311: { uint32_t e = ea_x(c, op); c->gpr[d] = rd16(e); c->gpr[a] = e; return; }
    case 343: c->gpr[d] = (uint32_t)(int16_t)rd16(ea_x(c, op)); return;               /* lhax */
    case 375: { uint32_t e = ea_x(c, op); c->gpr[d] = (uint32_t)(int16_t)rd16(e); c->gpr[a] = e; return; }
    case 151: wr32(ea_x(c, op), rs); return;                                          /* stwx */
    case 183: { uint32_t e = ea_x(c, op); wr32(e, rs); c->gpr[a] = e; return; }
    case 215: wr8(ea_x(c, op), (uint8_t)rs); return;                                  /* stbx */
    case 247: { uint32_t e = ea_x(c, op); wr8(e, (uint8_t)rs); c->gpr[a] = e; return; }
    case 407: wr16(ea_x(c, op), (uint16_t)rs); return;                                /* sthx */
    case 439: { uint32_t e = ea_x(c, op); wr16(e, (uint16_t)rs); c->gpr[a] = e; return; }
    case 534: c->gpr[d] = bswap32(rd32(ea_x(c, op))); return;                         /* lwbrx */
    case 790: c->gpr[d] = bswap16(rd16(ea_x(c, op))); return;                         /* lhbrx */
    case 662: wr32(ea_x(c, op), bswap32(rs)); return;                                 /* stwbrx */
    case 918: wr16(ea_x(c, op), (uint16_t)bswap16(rs)); return;                       /* sthbrx */
    case 20: { /* lwarx */
        uint32_t e = ea_x(c, op);
        c->gpr[d] = rd32(e);
        c->reserved = true;
        c->reserve_addr = e;
        return;
    }
    case 150: { /* stwcx. */
        uint32_t e = ea_x(c, op);
        uint32_t bits = c->xer & PPC_XER_SO ? 1 : 0;
        if (c->reserved && c->reserve_addr == e) {
            wr32(e, rs);
            bits |= 2;
        }
        c->reserved = false;
        set_crf(c, 0, bits);
        return;
    }
    case 533: load_string(c, ea_x(c, op), d, c->xer & 0x7F); return;  /* lswx */
    case 597: load_string(c, RA(op) ? c->gpr[a] : 0, d, b ? (uint32_t)b : 32); return; /* lswi */
    case 661: store_string(c, ea_x(c, op), d, c->xer & 0x7F); return; /* stswx */
    case 725: store_string(c, RA(op) ? c->gpr[a] : 0, d, b ? (uint32_t)b : 32); return; /* stswi */
    case 535: fset(c, d, u32_float(rd32(ea_x(c, op)))); return;                       /* lfsx */
    case 567: { uint32_t e = ea_x(c, op); fset(c, d, u32_float(rd32(e))); c->gpr[a] = e; return; }
    case 599: c->fpr[d] = rd64(ea_x(c, op)); return;                                  /* lfdx */
    case 631: { uint32_t e = ea_x(c, op); c->fpr[d] = rd64(e); c->gpr[a] = e; return; }
    case 663: wr32(ea_x(c, op), float_u32((float)fget(c, d))); return;                /* stfsx */
    case 695: { uint32_t e = ea_x(c, op); wr32(e, float_u32((float)fget(c, d))); c->gpr[a] = e; return; }
    case 727: wr64(ea_x(c, op), c->fpr[d]); return;                                   /* stfdx */
    case 759: { uint32_t e = ea_x(c, op); wr64(e, c->fpr[d]); c->gpr[a] = e; return; }
    case 983: wr32(ea_x(c, op), (uint32_t)c->fpr[d]); return;                         /* stfiwx */
    case 1014: { /* dcbz: a 32-byte cache block */
        uint32_t e = ea_x(c, op) & ~31u;
        for (int i = 0; i < 32; i += 4)
            wr32(e + (uint32_t)i, 0);
        return;
    }
    case 54: case 86: case 246: case 278: case 470: case 982: /* dcbst dcbf dcbtst dcbt dcbi icbi */
    case 598: case 854: case 306: case 370: case 566:        /* sync eieio tlbie tlbia tlbsync */
        return;
    /* Logical, shifts, extends: result to rA */
    case 28: r = rs & rb; break;         /* and */
    case 60: r = rs & ~rb; break;        /* andc */
    case 444: r = rs | rb; break;        /* or (mr) */
    case 412: r = rs | ~rb; break;       /* orc */
    case 316: r = rs ^ rb; break;        /* xor */
    case 476: r = ~(rs & rb); break;     /* nand */
    case 124: r = ~(rs | rb); break;     /* nor */
    case 284: r = ~(rs ^ rb); break;     /* eqv */
    case 954: r = (uint32_t)(int8_t)rs; break;   /* extsb */
    case 922: r = (uint32_t)(int16_t)rs; break;  /* extsh */
    case 26: r = rs ? (uint32_t)__builtin_clz(rs) : 32; break; /* cntlzw */
    case 24: r = (rb & 0x20) ? 0 : rs << (rb & 31); break;     /* slw */
    case 536: r = (rb & 0x20) ? 0 : rs >> (rb & 31); break;    /* srw */
    case 792: { /* sraw */
        uint32_t n = rb & 0x3F;
        int32_t s = (int32_t)rs;
        if (n > 31) {
            r = s < 0 ? 0xFFFFFFFFu : 0;
            set_ca(c, s < 0);
        } else {
            r = (uint32_t)(s >> n);
            set_ca(c, s < 0 && (rs & ((1u << n) - 1)));
        }
        break;
    }
    case 824: { /* srawi */
        int n = b;
        int32_t s = (int32_t)rs;
        r = (uint32_t)(s >> n);
        set_ca(c, s < 0 && n && (rs & ((1u << n) - 1)));
        break;
    }
    default:
        logical = false;
        break;
    }
    if (!logical) {
        illegal(c);
        return;
    }
    c->gpr[a] = r;
    if (RCBIT(op))
        set_cr0(c, r);
}

/* ---------------------------------------------------------------------- */
/* The step */

void ppc_step(ppc_cpu *c)
{
    /* Instructions are words: a branch to an address with its low bits
     * set goes to the word they are in, as the processor ignores them
     * (MCL's tagged code pointers, called through transition vectors). */
    c->pc &= ~3u;
    if (c->trace)
        c->trace(c);
    uint32_t op = rd32(c->pc);
    c->prev_pc = c->op_pc;
    c->op_pc = c->pc;
    c->op = op;
    c->pc += 4;
    c->icount++;
    c->tb++;
    ppc_execute(c, op);
}

void ppc_execute(ppc_cpu *c, uint32_t op)
{
    int d = RD(op), a = RA(op);
    switch (OPCD(op)) {
    case 1: /* NATIVE */
        if (c->native)
            c->native(c, op & 0x03FFFFFFu);
        else
            illegal(c);
        return;
    case 3: /* twi */
        trap_if(c, d, (int32_t)c->gpr[a], SIMM(op));
        return;
    case 7: /* mulli */
        c->gpr[d] = (uint32_t)((int32_t)c->gpr[a] * SIMM(op));
        return;
    case 9: { /* dozi (POWER, as the 601 has it): SIMM - rA, or 0 */
        int32_t x = (int32_t)c->gpr[a], imm = SIMM(op);
        c->gpr[d] = x > imm ? 0 : (uint32_t)imm - (uint32_t)x;
        return;
    }
    case 8: { /* subfic */
        uint32_t x = c->gpr[a], imm = (uint32_t)SIMM(op);
        c->gpr[d] = imm - x;
        set_ca(c, imm >= x);
        return;
    }
    case 10: { /* cmpli */
        uint32_t x = c->gpr[a], y = UIMM(op);
        uint32_t bits = x < y ? 8 : x > y ? 4 : 2;
        if (c->xer & PPC_XER_SO)
            bits |= 1;
        set_crf(c, d >> 2, bits);
        return;
    }
    case 11: { /* cmpi */
        int32_t x = (int32_t)c->gpr[a], y = SIMM(op);
        uint32_t bits = x < y ? 8 : x > y ? 4 : 2;
        if (c->xer & PPC_XER_SO)
            bits |= 1;
        set_crf(c, d >> 2, bits);
        return;
    }
    case 12: /* addic */
    case 13: { /* addic. */
        uint32_t x = c->gpr[a], r = x + (uint32_t)SIMM(op);
        c->gpr[d] = r;
        set_ca(c, r < x);
        if (OPCD(op) == 13)
            set_cr0(c, r);
        return;
    }
    case 14: /* addi (li) */
        c->gpr[d] = (a ? c->gpr[a] : 0) + (uint32_t)SIMM(op);
        return;
    case 15: /* addis (lis) */
        c->gpr[d] = (a ? c->gpr[a] : 0) + (UIMM(op) << 16);
        return;
    case 16: { /* bc */
        uint32_t target = (uint32_t)(int32_t)(int16_t)(op & 0xFFFC);
        if (!(op & 2))
            target += c->op_pc;
        bool taken = branch_cond(c, op);
        if (op & 1)
            c->lr = c->pc;
        if (taken)
            c->pc = target;
        return;
    }
    case 17: /* sc */
        if (c->syscall)
            c->syscall(c);
        else
            ppc_halt(c, "sc at %08X", c->op_pc);
        return;
    case 18: { /* b */
        int32_t li = (int32_t)(op << 6) >> 6 & ~3;
        uint32_t target = (op & 2) ? (uint32_t)li : c->op_pc + (uint32_t)li;
        if (op & 1)
            c->lr = c->pc;
        c->pc = target;
        return;
    }
    case 19:
        op19(c, op);
        return;
    case 20: { /* rlwimi */
        int sh = RB(op), mb = RC_(op), me = XO5(op);
        uint32_t m = mask(mb, me);
        uint32_t r = (rotl(c->gpr[d], sh) & m) | (c->gpr[a] & ~m);
        c->gpr[a] = r;
        if (RCBIT(op))
            set_cr0(c, r);
        return;
    }
    case 21: { /* rlwinm */
        int sh = RB(op), mb = RC_(op), me = XO5(op);
        uint32_t r = rotl(c->gpr[d], sh) & mask(mb, me);
        c->gpr[a] = r;
        if (RCBIT(op))
            set_cr0(c, r);
        return;
    }
    case 23: { /* rlwnm */
        int mb = RC_(op), me = XO5(op);
        uint32_t r = rotl(c->gpr[d], (int)(c->gpr[RB(op)] & 31)) & mask(mb, me);
        c->gpr[a] = r;
        if (RCBIT(op))
            set_cr0(c, r);
        return;
    }
    case 24: c->gpr[a] = c->gpr[d] | UIMM(op); return;          /* ori (nop) */
    case 25: c->gpr[a] = c->gpr[d] | (UIMM(op) << 16); return;  /* oris */
    case 26: c->gpr[a] = c->gpr[d] ^ UIMM(op); return;          /* xori */
    case 27: c->gpr[a] = c->gpr[d] ^ (UIMM(op) << 16); return;  /* xoris */
    case 28: c->gpr[a] = c->gpr[d] & UIMM(op); set_cr0(c, c->gpr[a]); return;          /* andi. */
    case 29: c->gpr[a] = c->gpr[d] & (UIMM(op) << 16); set_cr0(c, c->gpr[a]); return;  /* andis. */
    case 31:
        op31(c, op);
        return;
    case 32: c->gpr[d] = rd32(ea_d(c, op)); return;                                    /* lwz */
    case 33: { uint32_t e = ea_d(c, op); c->gpr[d] = rd32(e); c->gpr[a] = e; return; }  /* lwzu */
    case 34: c->gpr[d] = rd8(ea_d(c, op)); return;                                     /* lbz */
    case 35: { uint32_t e = ea_d(c, op); c->gpr[d] = rd8(e); c->gpr[a] = e; return; }
    case 36: wr32(ea_d(c, op), c->gpr[d]); return;                                     /* stw */
    case 37: { uint32_t e = ea_d(c, op); wr32(e, c->gpr[d]); c->gpr[a] = e; return; }   /* stwu */
    case 38: wr8(ea_d(c, op), (uint8_t)c->gpr[d]); return;                             /* stb */
    case 39: { uint32_t e = ea_d(c, op); wr8(e, (uint8_t)c->gpr[d]); c->gpr[a] = e; return; }
    case 40: c->gpr[d] = rd16(ea_d(c, op)); return;                                    /* lhz */
    case 41: { uint32_t e = ea_d(c, op); c->gpr[d] = rd16(e); c->gpr[a] = e; return; }
    case 42: c->gpr[d] = (uint32_t)(int16_t)rd16(ea_d(c, op)); return;                 /* lha */
    case 43: { uint32_t e = ea_d(c, op); c->gpr[d] = (uint32_t)(int16_t)rd16(e); c->gpr[a] = e; return; }
    case 44: wr16(ea_d(c, op), (uint16_t)c->gpr[d]); return;                           /* sth */
    case 45: { uint32_t e = ea_d(c, op); wr16(e, (uint16_t)c->gpr[d]); c->gpr[a] = e; return; }
    case 46: { /* lmw */
        uint32_t e = ea_d(c, op);
        for (int r = d; r < 32; r++, e += 4)
            c->gpr[r] = rd32(e);
        return;
    }
    case 47: { /* stmw */
        uint32_t e = ea_d(c, op);
        for (int r = d; r < 32; r++, e += 4)
            wr32(e, c->gpr[r]);
        return;
    }
    case 48: fset(c, d, u32_float(rd32(ea_d(c, op)))); return;                         /* lfs */
    case 49: { uint32_t e = ea_d(c, op); fset(c, d, u32_float(rd32(e))); c->gpr[a] = e; return; }
    case 50: c->fpr[d] = rd64(ea_d(c, op)); return;                                    /* lfd */
    case 51: { uint32_t e = ea_d(c, op); c->fpr[d] = rd64(e); c->gpr[a] = e; return; }
    case 52: wr32(ea_d(c, op), float_u32((float)fget(c, d))); return;                  /* stfs */
    case 53: { uint32_t e = ea_d(c, op); wr32(e, float_u32((float)fget(c, d))); c->gpr[a] = e; return; }
    case 54: wr64(ea_d(c, op), c->fpr[d]); return;                                     /* stfd */
    case 55: { uint32_t e = ea_d(c, op); wr64(e, c->fpr[d]); c->gpr[a] = e; return; }
    case 59:
        fp_arith(c, op, true);
        return;
    case 63:
        fp_arith(c, op, false);
        return;
    default:
        illegal(c);
    }
}

uint64_t ppc_run(ppc_cpu *c, uint64_t max)
{
    if (ppc_jit_enabled && !c->trace)
        return ppc_jit_run(c, max);
    uint64_t start = c->icount;
    while (!c->halted && c->icount - start < max)
        ppc_step(c);
    return c->icount - start;
}
