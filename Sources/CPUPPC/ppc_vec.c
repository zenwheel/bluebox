/*
 * AltiVec, the G4's vector unit ("Velocity Engine"), interpreted: the
 * whole instruction set of Motorola's AltiVec Technology Programming
 * Environments Manual (opcode 4, and the vector loads, stores and data
 * stream touches of opcode 31).  See ppc.h.
 *
 * A vector register is its 16 bytes in the guest's order; elements are
 * numbered from the most significant end (element 0 is bytes 0-3 of a
 * word vector) and read big-endian, so memory and registers agree byte for
 * byte as on the real processor, whatever the host's order.  Plain C, the
 * reference for a later translation onto NEON and SSE.
 *
 * Where the manual leaves room, as a G4 does or, where that isn't known,
 * the choice that keeps results the same on every host:
 *   - lvebx, lvehx, lvewx load the whole aligned quadword (the element
 *     addressed is where the manual says; the others are what memory holds
 *     around it, as the 7400 and 7450 load the quadword).
 *   - vsl and vsr shift by the count in byte 15 of vB (the manual's
 *     pseudocode; its result is undefined when the bytes' counts differ).
 *   - The estimates (vrefp, vrsqrtefp, vexptefp, vlogefp) give the exact
 *     result rounded to single precision, computed in double: well inside
 *     the manual's bounds (1/4096 relative; 1/16 for 2^x; 1/32 absolute for
 *     log2) and the same on every host.  The G4's own lookup tables aren't
 *     published; a program refining an estimate converges the same way.
 *   - NaNs: an operand NaN is returned quieted, the first of vA, vB, vC in
 *     that order (vmaddfp's and vnmsubfp's too, unnegated); an invalid
 *     operation gives the default NaN 0x7FC00000.
 *   - Non-Java mode (VSCR[NJ], set for a new program as Mac OS sets it):
 *     a denormal operand is read as a zero of its sign and a denormal
 *     result written as one, in every floating-point instruction
 *     (compares, min and max, conversions included).  Java mode (NJ clear)
 *     keeps denormals as IEEE does.  Rounding is to nearest throughout, as
 *     the vector unit has no other mode (but the conversions to integers
 *     and the vrfi* roundings, which say their own).
 *   - vctsxs and vctuxs turn a NaN into 0 without setting SAT.
 */
#include <math.h>
#include <string.h>

#include "ppc.h"

/* The program's stores, watched (mem.h). */
static inline void vwr8(gaddr a, uint8_t v) { watch_store(a, 1); wr8(a, v); }
static inline void vwr16(gaddr a, uint16_t v) { watch_store(a, 2); wr16(a, v); }
static inline void vwr32(gaddr a, uint32_t v) { watch_store(a, 4); wr32(a, v); }

bool ppc_altivec = true;
BB_HOST uint64_t ppc_vec_count;

uint32_t ppc_pvr(void) { return ppc_altivec ? 0x000C0209u : 0x00080200u; } /* a 7400 v2.9; a 750 */
/* User mode with FP, machine checks and translation on, as an application
 * ran; the vector unit (VEC) on a G4, as Mac OS gives it to every program. */
uint32_t ppc_msr(void) { return 0x0000D032u | (ppc_altivec ? 0x02000000u : 0); }

/* ---------------------------------------------------------------------- */
/* Fields */

#define OPCD(op) ((op) >> 26)
#define VD(op) (((op) >> 21) & 31)
#define VA(op) (((op) >> 16) & 31)
#define VB(op) (((op) >> 11) & 31)
#define VC(op) (((op) >> 6) & 31)
#define UIMM5(op) VA(op)
#define SIMM5(op) ((int32_t)(VA(op) ^ 16) - 16)

/* Elements, read and written big-endian. */
static inline uint32_t eh(const uint8_t *v, int i) { return (uint32_t)v[2 * i] << 8 | v[2 * i + 1]; }
static inline uint32_t ew(const uint8_t *v, int i)
{
    return (uint32_t)v[4 * i] << 24 | (uint32_t)v[4 * i + 1] << 16 | (uint32_t)v[4 * i + 2] << 8 | v[4 * i + 3];
}
static inline void sh16(uint8_t *v, int i, uint32_t x)
{
    v[2 * i] = (uint8_t)(x >> 8);
    v[2 * i + 1] = (uint8_t)x;
}
static inline void sw32(uint8_t *v, int i, uint32_t x)
{
    v[4 * i] = (uint8_t)(x >> 24);
    v[4 * i + 1] = (uint8_t)(x >> 16);
    v[4 * i + 2] = (uint8_t)(x >> 8);
    v[4 * i + 3] = (uint8_t)x;
}

#define S8(x) ((int64_t)(int8_t)(x))
#define S16(x) ((int64_t)(int16_t)(x))
#define S32(x) ((int64_t)(int32_t)(x))
#define MAX(x, y) ((x) > (y) ? (x) : (y))
#define MIN(x, y) ((x) < (y) ? (x) : (y))

/* Saturation, noting it. */
static uint32_t satu(int64_t v, uint32_t max, bool *sat)
{
    if (v < 0) {
        *sat = true;
        return 0;
    }
    if (v > (int64_t)max) {
        *sat = true;
        return max;
    }
    return (uint32_t)v;
}

static uint32_t sats(int64_t v, int bits, bool *sat)
{
    int64_t max = ((int64_t)1 << (bits - 1)) - 1, min = -max - 1;
    if (v < min) {
        *sat = true;
        v = min;
    } else if (v > max) {
        *sat = true;
        v = max;
    }
    return (uint32_t)v & (uint32_t)(((uint64_t)1 << bits) - 1);
}

/* ---------------------------------------------------------------------- */
/* Floating point */

static inline float bits_f(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static inline uint32_t f_bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static inline bool is_nan(uint32_t u) { return (u & 0x7F800000u) == 0x7F800000u && (u & 0x007FFFFFu); }
static inline bool is_denormal(uint32_t u) { return !(u & 0x7F800000u) && (u & 0x007FFFFFu); }

#define DEFAULT_NAN 0x7FC00000u

/* An operand: in non-Java mode a denormal is a zero of its sign. */
static inline uint32_t fin(const ppc_cpu *c, uint32_t u)
{
    return (c->vscr & PPC_VSCR_NJ) && is_denormal(u) ? u & 0x80000000u : u;
}

/* A result: a NaN made here is the default one; in non-Java mode a
 * denormal is written as a zero of its sign. */
static inline uint32_t fout(const ppc_cpu *c, float f)
{
    uint32_t u = f_bits(f);
    if (is_nan(u))
        return DEFAULT_NAN;
    if ((c->vscr & PPC_VSCR_NJ) && is_denormal(u))
        return u & 0x80000000u;
    return u;
}

static void fp_vx(ppc_cpu *c, int xo, const uint8_t *A, const uint8_t *B, uint8_t *R, uint32_t uimm)
{
    for (int i = 0; i < 4; i++) {
        uint32_t ua = fin(c, ew(A, i)), ub = fin(c, ew(B, i)), r;
        float a = bits_f(ua), b = bits_f(ub);
        bool two = xo == 10 || xo == 74 || xo == 1034 || xo == 1098;
        if (two && is_nan(ua)) {
            r = ua | 0x00400000u;
        } else if (is_nan(ub) && xo != 778 && xo != 842) { /* vcfux, vcfsx: vB holds integers */
            r = ub | 0x00400000u;
            if (xo == 906 || xo == 970)
                r = 0; /* vctuxs, vctsxs */
        } else {
            switch (xo) {
            case 10: r = fout(c, a + b); break;  /* vaddfp */
            case 74: r = fout(c, a - b); break;  /* vsubfp */
            case 1034:                           /* vmaxfp */
            case 1098: {                         /* vminfp */
                bool max = xo == 1034;
                if (a == b) /* +0 and -0: max is +0, min is -0 */
                    r = max ? (ua & ub) : (ua | ub);
                else
                    r = (a > b) == max ? ua : ub;
                break;
            }
            case 266: r = fout(c, (float)(1.0 / (double)b)); break; /* vrefp */
            case 330: /* vrsqrtefp */
                r = b < 0 ? DEFAULT_NAN : fout(c, (float)(1.0 / sqrt((double)b)));
                break;
            case 394: r = fout(c, (float)exp2((double)b)); break; /* vexptefp */
            case 458: /* vlogefp */
                r = b < 0 ? DEFAULT_NAN : b == 0 ? 0xFF800000u : fout(c, (float)log2((double)b));
                break;
            case 522: r = fout(c, nearbyintf(b)); break; /* vrfin */
            case 586: r = fout(c, truncf(b)); break;     /* vrfiz */
            case 650: r = fout(c, ceilf(b)); break;      /* vrfip */
            case 714: r = fout(c, floorf(b)); break;     /* vrfim */
            case 778: r = f_bits((float)((double)ew(B, i) / (double)(1u << uimm))); break;          /* vcfux */
            case 842: r = f_bits((float)((double)(int32_t)ew(B, i) / (double)(1u << uimm))); break; /* vcfsx */
            case 906: { /* vctuxs */
                double t = trunc((double)b * (double)(1u << uimm));
                bool sat = false;
                r = t < 0 ? (sat = true, 0) : t > 4294967295.0 ? (sat = true, 0xFFFFFFFFu) : (uint32_t)t;
                if (sat)
                    c->vscr |= PPC_VSCR_SAT;
                break;
            }
            case 970: { /* vctsxs */
                double t = trunc((double)b * (double)(1u << uimm));
                bool sat = false;
                r = t < -2147483648.0   ? (sat = true, 0x80000000u)
                    : t > 2147483647.0 ? (sat = true, 0x7FFFFFFFu)
                                       : (uint32_t)(int32_t)t;
                if (sat)
                    c->vscr |= PPC_VSCR_SAT;
                break;
            }
            default: r = 0; break;
            }
        }
        sw32(R, i, r);
    }
}

/* vmaddfp (vA*vC + vB) and vnmsubfp (-(vA*vC - vB)), rounded once. */
static void fp_madd(ppc_cpu *c, bool nmsub, const uint8_t *A, const uint8_t *B, const uint8_t *C, uint8_t *R)
{
    for (int i = 0; i < 4; i++) {
        uint32_t ua = fin(c, ew(A, i)), ub = fin(c, ew(B, i)), uc = fin(c, ew(C, i)), r;
        if (is_nan(ua))
            r = ua | 0x00400000u;
        else if (is_nan(ub))
            r = ub | 0x00400000u;
        else if (is_nan(uc))
            r = uc | 0x00400000u;
        else if (nmsub) {
            float f = fmaf(bits_f(ua), bits_f(uc), -bits_f(ub));
            r = isnan(f) ? DEFAULT_NAN : fout(c, -f);
        } else
            r = fout(c, fmaf(bits_f(ua), bits_f(uc), bits_f(ub)));
        sw32(R, i, r);
    }
}

/* ---------------------------------------------------------------------- */
/* Compares (VC form): all ones where true; with Rc, CR6 says all true
 * (LT) or none (EQ); vcmpbfp's EQ says every element in bounds. */
static bool compare(ppc_cpu *c, uint32_t op, const uint8_t *A, const uint8_t *B, uint8_t *R)
{
    int xo = (int)(op & 0x3FF);
    switch (xo) {
    case 6: case 518: case 774: /* vcmpequb, vcmpgtub, vcmpgtsb */
        for (int i = 0; i < 16; i++) {
            bool t = xo == 6 ? A[i] == B[i] : xo == 518 ? A[i] > B[i] : (int8_t)A[i] > (int8_t)B[i];
            R[i] = t ? 0xFF : 0;
        }
        break;
    case 70: case 582: case 838: /* vcmpequh, vcmpgtuh, vcmpgtsh */
        for (int i = 0; i < 8; i++) {
            uint32_t a = eh(A, i), b = eh(B, i);
            bool t = xo == 70 ? a == b : xo == 582 ? a > b : (int16_t)a > (int16_t)b;
            sh16(R, i, t ? 0xFFFF : 0);
        }
        break;
    case 134: case 646: case 902: /* vcmpequw, vcmpgtuw, vcmpgtsw */
        for (int i = 0; i < 4; i++) {
            uint32_t a = ew(A, i), b = ew(B, i);
            bool t = xo == 134 ? a == b : xo == 646 ? a > b : (int32_t)a > (int32_t)b;
            sw32(R, i, t ? 0xFFFFFFFFu : 0);
        }
        break;
    case 198: case 454: case 710: /* vcmpeqfp, vcmpgefp, vcmpgtfp (a NaN compares false) */
        for (int i = 0; i < 4; i++) {
            float a = bits_f(fin(c, ew(A, i))), b = bits_f(fin(c, ew(B, i)));
            bool t = xo == 198 ? a == b : xo == 454 ? a >= b : a > b;
            sw32(R, i, t ? 0xFFFFFFFFu : 0);
        }
        break;
    case 966: { /* vcmpbfp: bit 0 set when not vA <= vB, bit 1 when not vA >= -vB */
        bool inside = true;
        for (int i = 0; i < 4; i++) {
            float a = bits_f(fin(c, ew(A, i))), b = bits_f(fin(c, ew(B, i)));
            uint32_t r = (a <= b ? 0 : 0x80000000u) | (a >= -b ? 0 : 0x40000000u);
            if (r)
                inside = false;
            sw32(R, i, r);
        }
        if (op & 0x400)
            c->cr = (c->cr & ~0x000000F0u) | (inside ? 0x20u : 0);
        return true;
    }
    default:
        return false;
    }
    if (op & 0x400) {
        bool all = true, none = true;
        for (int i = 0; i < 16; i++) {
            if (R[i])
                none = false;
            else
                all = false;
        }
        c->cr = (c->cr & ~0x000000F0u) | (all ? 0x80u : 0) | (none ? 0x20u : 0);
    }
    return true;
}

/* ---------------------------------------------------------------------- */
/* VA form: the three-operand multiply-adds, select, permute, shift */

static bool va_form(ppc_cpu *c, uint32_t op, const uint8_t *A, const uint8_t *B, const uint8_t *C, uint8_t *R)
{
    bool sat = false;
    switch (op & 0x3F) {
    case 32: /* vmhaddshs */
    case 33: /* vmhraddshs */
        for (int i = 0; i < 8; i++) {
            int32_t p = (int32_t)(int16_t)eh(A, i) * (int16_t)eh(B, i);
            if ((op & 0x3F) == 33)
                p += 0x4000;
            sh16(R, i, sats((int64_t)(p >> 15) + (int16_t)eh(C, i), 16, &sat));
        }
        break;
    case 34: /* vmladduhm */
        for (int i = 0; i < 8; i++)
            sh16(R, i, (eh(A, i) * eh(B, i) + eh(C, i)) & 0xFFFF);
        break;
    case 36: /* vmsumubm */
        for (int i = 0; i < 4; i++) {
            uint32_t s = ew(C, i);
            for (int j = 4 * i; j < 4 * i + 4; j++)
                s += (uint32_t)A[j] * B[j];
            sw32(R, i, s);
        }
        break;
    case 37: /* vmsummbm: signed bytes of vA by unsigned bytes of vB */
        for (int i = 0; i < 4; i++) {
            uint32_t s = ew(C, i);
            for (int j = 4 * i; j < 4 * i + 4; j++)
                s += (uint32_t)((int32_t)(int8_t)A[j] * (int32_t)B[j]);
            sw32(R, i, s);
        }
        break;
    case 38: /* vmsumuhm */
    case 39: /* vmsumuhs */
        for (int i = 0; i < 4; i++) {
            uint64_t s = (uint64_t)ew(C, i) + (uint64_t)eh(A, 2 * i) * eh(B, 2 * i) +
                         (uint64_t)eh(A, 2 * i + 1) * eh(B, 2 * i + 1);
            sw32(R, i, (op & 0x3F) == 38 ? (uint32_t)s : satu((int64_t)MIN(s, 0x100000000ull), 0xFFFFFFFFu, &sat));
        }
        break;
    case 40: /* vmsumshm */
    case 41: /* vmsumshs */
        for (int i = 0; i < 4; i++) {
            int64_t s = (int64_t)(int32_t)ew(C, i) + (int64_t)(int16_t)eh(A, 2 * i) * (int16_t)eh(B, 2 * i) +
                        (int64_t)(int16_t)eh(A, 2 * i + 1) * (int16_t)eh(B, 2 * i + 1);
            sw32(R, i, (op & 0x3F) == 40 ? (uint32_t)s : sats(s, 32, &sat));
        }
        break;
    case 42: /* vsel */
        for (int i = 0; i < 16; i++)
            R[i] = (uint8_t)((A[i] & ~C[i]) | (B[i] & C[i]));
        break;
    case 43: /* vperm */
        for (int i = 0; i < 16; i++) {
            int k = C[i] & 31;
            R[i] = k < 16 ? A[k] : B[k - 16];
        }
        break;
    case 44: { /* vsldoi */
        int sh = (int)(op >> 6) & 15;
        for (int i = 0; i < 16; i++)
            R[i] = i + sh < 16 ? A[i + sh] : B[i + sh - 16];
        break;
    }
    case 46: /* vmaddfp */
        fp_madd(c, false, A, B, C, R);
        break;
    case 47: /* vnmsubfp */
        fp_madd(c, true, A, B, C, R);
        break;
    default:
        return false;
    }
    if (sat)
        c->vscr |= PPC_VSCR_SAT;
    return true;
}

/* ---------------------------------------------------------------------- */
/* VX form */

/* Element-wise over bytes, halfwords and words, a and b the elements of
 * vA and vB (unsigned; cast for the signed forms). */
#define EACH_B(expr)                                                                                                   \
    for (int i = 0; i < 16; i++) {                                                                                     \
        uint32_t a = A[i], b = B[i];                                                                                   \
        (void)a;                                                                                                       \
        (void)b;                                                                                                       \
        R[i] = (uint8_t)(expr);                                                                                        \
    }                                                                                                                  \
    break
#define EACH_H(expr)                                                                                                   \
    for (int i = 0; i < 8; i++) {                                                                                      \
        uint32_t a = eh(A, i), b = eh(B, i);                                                                           \
        (void)a;                                                                                                       \
        (void)b;                                                                                                       \
        sh16(R, i, (uint32_t)(expr));                                                                                  \
    }                                                                                                                  \
    break
#define EACH_W(expr)                                                                                                   \
    for (int i = 0; i < 4; i++) {                                                                                      \
        uint32_t a = ew(A, i), b = ew(B, i);                                                                           \
        (void)a;                                                                                                       \
        (void)b;                                                                                                       \
        sw32(R, i, (uint32_t)(expr));                                                                                  \
    }                                                                                                                  \
    break

static inline uint32_t rot(uint32_t v, int n, int bits)
{
    uint32_t m = bits == 32 ? 0xFFFFFFFFu : (1u << bits) - 1;
    v &= m;
    return n ? ((v << n) | (v >> (bits - n))) & m : v;
}

/* vpkpx: a 32-bit pixel to 1:5:5:5 (bit 7, then the top five bits of
 * each of bytes 1 to 3). */
static inline uint32_t pack_px(uint32_t w)
{
    return (w >> 24 & 1) << 15 | (w >> 19 & 0x1F) << 10 | (w >> 11 & 0x1F) << 5 | (w >> 3 & 0x1F);
}

/* vupkhpx, vupklpx: back to 8:8:8:8, the 1 sign-extended and each 5 bits
 * zero-extended. */
static inline uint32_t unpack_px(uint32_t h)
{
    return (h & 0x8000 ? 0xFF000000u : 0) | (h >> 10 & 0x1F) << 16 | (h >> 5 & 0x1F) << 8 | (h & 0x1F);
}

static bool vx_form(ppc_cpu *c, uint32_t op, const uint8_t *A, const uint8_t *B, uint8_t *R)
{
    bool sat = false;
    int xo = (int)(op & 0x7FF);
    switch (xo) {
    /* Modulo add and subtract, carries */
    case 0: EACH_B(a + b);    /* vaddubm */
    case 64: EACH_H(a + b);   /* vadduhm */
    case 128: EACH_W(a + b);  /* vadduwm */
    case 1024: EACH_B(a - b); /* vsububm */
    case 1088: EACH_H(a - b); /* vsubuhm */
    case 1152: EACH_W(a - b); /* vsubuwm */
    case 384: EACH_W(((uint64_t)a + b) >> 32); /* vaddcuw */
    case 1408: EACH_W(a >= b);                 /* vsubcuw: the carry, 1 when no borrow */
    /* Saturating add and subtract */
    case 512: EACH_B(satu((int64_t)a + b, 0xFF, &sat));          /* vaddubs */
    case 576: EACH_H(satu((int64_t)a + b, 0xFFFF, &sat));        /* vadduhs */
    case 640: EACH_W(satu((int64_t)a + b, 0xFFFFFFFFu, &sat));   /* vadduws */
    case 768: EACH_B(sats(S8(a) + S8(b), 8, &sat));              /* vaddsbs */
    case 832: EACH_H(sats(S16(a) + S16(b), 16, &sat));           /* vaddshs */
    case 896: EACH_W(sats(S32(a) + S32(b), 32, &sat));           /* vaddsws */
    case 1536: EACH_B(satu((int64_t)a - b, 0xFF, &sat));         /* vsububs */
    case 1600: EACH_H(satu((int64_t)a - b, 0xFFFF, &sat));       /* vsubuhs */
    case 1664: EACH_W(satu((int64_t)a - b, 0xFFFFFFFFu, &sat));  /* vsubuws */
    case 1792: EACH_B(sats(S8(a) - S8(b), 8, &sat));             /* vsubsbs */
    case 1856: EACH_H(sats(S16(a) - S16(b), 16, &sat));          /* vsubshs */
    case 1920: EACH_W(sats(S32(a) - S32(b), 32, &sat));          /* vsubsws */
    /* Maximum, minimum, average */
    case 2: EACH_B(MAX(a, b));                         /* vmaxub */
    case 66: EACH_H(MAX(a, b));                        /* vmaxuh */
    case 130: EACH_W(MAX(a, b));                       /* vmaxuw */
    case 258: EACH_B(MAX(S8(a), S8(b)));               /* vmaxsb */
    case 322: EACH_H(MAX(S16(a), S16(b)));             /* vmaxsh */
    case 386: EACH_W(MAX(S32(a), S32(b)));             /* vmaxsw */
    case 514: EACH_B(MIN(a, b));                       /* vminub */
    case 578: EACH_H(MIN(a, b));                       /* vminuh */
    case 642: EACH_W(MIN(a, b));                       /* vminuw */
    case 770: EACH_B(MIN(S8(a), S8(b)));               /* vminsb */
    case 834: EACH_H(MIN(S16(a), S16(b)));             /* vminsh */
    case 898: EACH_W(MIN(S32(a), S32(b)));             /* vminsw */
    case 1026: EACH_B(((uint64_t)a + b + 1) >> 1);     /* vavgub */
    case 1090: EACH_H(((uint64_t)a + b + 1) >> 1);     /* vavguh */
    case 1154: EACH_W(((uint64_t)a + b + 1) >> 1);     /* vavguw */
    case 1282: EACH_B((S8(a) + S8(b) + 1) >> 1);       /* vavgsb */
    case 1346: EACH_H((S16(a) + S16(b) + 1) >> 1);     /* vavgsh */
    case 1410: EACH_W((S32(a) + S32(b) + 1) >> 1);     /* vavgsw */
    /* Logical */
    case 1028: EACH_B(a & b);    /* vand */
    case 1092: EACH_B(a & ~b);   /* vandc */
    case 1156: EACH_B(a | b);    /* vor */
    case 1284: EACH_B(~(a | b)); /* vnor */
    case 1220: EACH_B(a ^ b);    /* vxor */
    /* Rotates and shifts, each element by the low bits of vB's */
    case 4: EACH_B(rot(a, (int)(b & 7), 8));      /* vrlb */
    case 68: EACH_H(rot(a, (int)(b & 15), 16));   /* vrlh */
    case 132: EACH_W(rot(a, (int)(b & 31), 32));  /* vrlw */
    case 260: EACH_B(a << (b & 7));               /* vslb */
    case 324: EACH_H(a << (b & 15));              /* vslh */
    case 388: EACH_W(a << (b & 31));              /* vslw */
    case 516: EACH_B(a >> (b & 7));               /* vsrb */
    case 580: EACH_H(a >> (b & 15));              /* vsrh */
    case 644: EACH_W(a >> (b & 31));              /* vsrw */
    case 772: EACH_B(S8(a) >> (b & 7));           /* vsrab */
    case 836: EACH_H(S16(a) >> (b & 15));         /* vsrah */
    case 900: EACH_W(S32(a) >> (b & 31));         /* vsraw */
    /* The whole register by bits (vsl, vsr) and by octets (vslo, vsro) */
    case 452: { /* vsl */
        int n = B[15] & 7;
        for (int i = 0; i < 16; i++)
            R[i] = (uint8_t)(A[i] << n | (i < 15 ? A[i + 1] >> (8 - n) : 0));
        break;
    }
    case 708: { /* vsr */
        int n = B[15] & 7;
        for (int i = 0; i < 16; i++)
            R[i] = (uint8_t)(A[i] >> n | (i > 0 ? A[i - 1] << (8 - n) : 0));
        break;
    }
    case 1036: { /* vslo */
        int n = (B[15] >> 3) & 15;
        for (int i = 0; i < 16; i++)
            R[i] = i + n < 16 ? A[i + n] : 0;
        break;
    }
    case 1100: { /* vsro */
        int n = (B[15] >> 3) & 15;
        for (int i = 0; i < 16; i++)
            R[i] = i >= n ? A[i - n] : 0;
        break;
    }
    /* Merges */
    case 12: case 268: { /* vmrghb, vmrglb */
        int o = xo == 12 ? 0 : 8;
        for (int i = 0; i < 8; i++) {
            R[2 * i] = A[o + i];
            R[2 * i + 1] = B[o + i];
        }
        break;
    }
    case 76: case 332: { /* vmrghh, vmrglh */
        int o = xo == 76 ? 0 : 4;
        for (int i = 0; i < 4; i++) {
            sh16(R, 2 * i, eh(A, o + i));
            sh16(R, 2 * i + 1, eh(B, o + i));
        }
        break;
    }
    case 140: case 396: { /* vmrghw, vmrglw */
        int o = xo == 140 ? 0 : 2;
        for (int i = 0; i < 2; i++) {
            sw32(R, 2 * i, ew(A, o + i));
            sw32(R, 2 * i + 1, ew(B, o + i));
        }
        break;
    }
    /* Splats */
    case 524: { /* vspltb */
        uint8_t v = B[UIMM5(op) & 15];
        memset(R, v, 16);
        break;
    }
    case 588: { /* vsplth */
        uint32_t v = eh(B, UIMM5(op) & 7);
        for (int i = 0; i < 8; i++)
            sh16(R, i, v);
        break;
    }
    case 652: { /* vspltw */
        uint32_t v = ew(B, UIMM5(op) & 3);
        for (int i = 0; i < 4; i++)
            sw32(R, i, v);
        break;
    }
    case 780: memset(R, (uint8_t)SIMM5(op), 16); break; /* vspltisb */
    case 844: /* vspltish */
        for (int i = 0; i < 8; i++)
            sh16(R, i, (uint32_t)SIMM5(op));
        break;
    case 908: /* vspltisw */
        for (int i = 0; i < 4; i++)
            sw32(R, i, (uint32_t)SIMM5(op));
        break;
    /* Packs: vA's elements then vB's, narrowed */
    case 14: case 142: case 270: case 398: { /* vpkuhum, vpkuhus, vpkshus, vpkshss */
        for (int i = 0; i < 16; i++) {
            uint32_t h = i < 8 ? eh(A, i) : eh(B, i - 8);
            R[i] = (uint8_t)(xo == 14    ? h
                             : xo == 142 ? satu(h, 0xFF, &sat)
                             : xo == 270 ? satu(S16(h), 0xFF, &sat)
                                         : sats(S16(h), 8, &sat));
        }
        break;
    }
    case 78: case 206: case 334: case 462: { /* vpkuwum, vpkuwus, vpkswus, vpkswss */
        for (int i = 0; i < 8; i++) {
            uint32_t w = i < 4 ? ew(A, i) : ew(B, i - 4);
            sh16(R, i, xo == 78    ? w & 0xFFFF
                       : xo == 206 ? satu(w, 0xFFFF, &sat)
                       : xo == 334 ? satu(S32(w), 0xFFFF, &sat)
                                   : sats(S32(w), 16, &sat));
        }
        break;
    }
    case 782: /* vpkpx */
        for (int i = 0; i < 8; i++)
            sh16(R, i, pack_px(i < 4 ? ew(A, i) : ew(B, i - 4)));
        break;
    /* Unpacks: the high or low half of vB, sign-extended */
    case 526: case 654: /* vupkhsb, vupklsb */
        for (int i = 0; i < 8; i++)
            sh16(R, i, (uint32_t)S8(B[(xo == 526 ? 0 : 8) + i]));
        break;
    case 590: case 718: /* vupkhsh, vupklsh */
        for (int i = 0; i < 4; i++)
            sw32(R, i, (uint32_t)S16(eh(B, (xo == 590 ? 0 : 4) + i)));
        break;
    case 846: case 974: /* vupkhpx, vupklpx */
        for (int i = 0; i < 4; i++)
            sw32(R, i, unpack_px(eh(B, (xo == 846 ? 0 : 4) + i)));
        break;
    /* Multiplies of the even or odd elements, to double width */
    case 520: case 8: case 776: case 264: { /* vmuleub, vmuloub, vmulesb, vmulosb */
        int o = xo == 520 || xo == 776 ? 0 : 1;
        bool s = xo == 776 || xo == 264;
        for (int i = 0; i < 8; i++) {
            uint32_t a = A[2 * i + o], b = B[2 * i + o];
            sh16(R, i, s ? (uint32_t)(S8(a) * S8(b)) : a * b);
        }
        break;
    }
    case 584: case 72: case 840: case 328: { /* vmuleuh, vmulouh, vmulesh, vmulosh */
        int o = xo == 584 || xo == 840 ? 0 : 1;
        bool s = xo == 840 || xo == 328;
        for (int i = 0; i < 4; i++) {
            uint32_t a = eh(A, 2 * i + o), b = eh(B, 2 * i + o);
            sw32(R, i, s ? (uint32_t)(S16(a) * S16(b)) : a * b);
        }
        break;
    }
    /* Sums across */
    case 1544: /* vsum4ubs */
        for (int i = 0; i < 4; i++)
            sw32(R, i, satu((int64_t)ew(B, i) + A[4 * i] + A[4 * i + 1] + A[4 * i + 2] + A[4 * i + 3], 0xFFFFFFFFu, &sat));
        break;
    case 1800: /* vsum4sbs */
        for (int i = 0; i < 4; i++)
            sw32(R, i, sats(S32(ew(B, i)) + S8(A[4 * i]) + S8(A[4 * i + 1]) + S8(A[4 * i + 2]) + S8(A[4 * i + 3]), 32, &sat));
        break;
    case 1608: /* vsum4shs */
        for (int i = 0; i < 4; i++)
            sw32(R, i, sats(S32(ew(B, i)) + S16(eh(A, 2 * i)) + S16(eh(A, 2 * i + 1)), 32, &sat));
        break;
    case 1672: /* vsum2sws */
        memset(R, 0, 16);
        for (int i = 1; i < 4; i += 2)
            sw32(R, i, sats(S32(ew(A, i - 1)) + S32(ew(A, i)) + S32(ew(B, i)), 32, &sat));
        break;
    case 1928: /* vsumsws */
        memset(R, 0, 16);
        sw32(R, 3, sats(S32(ew(A, 0)) + S32(ew(A, 1)) + S32(ew(A, 2)) + S32(ew(A, 3)) + S32(ew(B, 3)), 32, &sat));
        break;
    /* Floating point */
    case 10: case 74: case 1034: case 1098: case 266: case 330: case 394: case 458:
    case 522: case 586: case 650: case 714: case 778: case 842: case 906: case 970:
        fp_vx(c, xo, A, B, R, UIMM5(op));
        break;
    /* The status and control register */
    case 1540: /* mfvscr */
        memset(R, 0, 16);
        sw32(R, 3, c->vscr);
        break;
    default:
        return false;
    }
    if (sat)
        c->vscr |= PPC_VSCR_SAT;
    return true;
}

/* ---------------------------------------------------------------------- */
/* Decoding */

/* The VX-form extended opcodes (the low 11 bits) that exist. */
static bool vx_known(int xo)
{
    switch (xo) {
    case 0: case 2: case 4: case 8: case 10: case 12: case 14:
    case 64: case 66: case 68: case 72: case 74: case 76: case 78:
    case 128: case 130: case 132: case 140: case 142:
    case 206:
    case 258: case 260: case 264: case 266: case 268: case 270:
    case 322: case 324: case 328: case 330: case 332: case 334:
    case 384: case 386: case 388: case 394: case 396: case 398:
    case 452: case 458: case 462:
    case 512: case 514: case 516: case 520: case 522: case 524: case 526:
    case 576: case 578: case 580: case 584: case 586: case 588: case 590:
    case 640: case 642: case 644: case 650: case 652: case 654:
    case 708: case 714: case 718:
    case 768: case 770: case 772: case 776: case 778: case 780: case 782:
    case 832: case 834: case 836: case 840: case 842: case 844: case 846:
    case 896: case 898: case 900: case 906: case 908:
    case 970: case 974:
    case 1024: case 1026: case 1028: case 1034: case 1036:
    case 1088: case 1090: case 1092: case 1098: case 1100:
    case 1152: case 1154: case 1156:
    case 1220:
    case 1282: case 1284:
    case 1346:
    case 1408: case 1410:
    case 1536: case 1540: case 1544:
    case 1600: case 1604: case 1608:
    case 1664: case 1672:
    case 1792: case 1800:
    case 1856:
    case 1920: case 1928:
        return true;
    default:
        return false;
    }
}

static bool vc_known(int xo)
{
    switch (xo & 0x3FF) {
    case 6: case 70: case 134: case 198: case 454: case 518: case 582: case 646:
    case 710: case 774: case 838: case 902: case 966:
        return true;
    default:
        return false;
    }
}

static bool va_known(int xo) { return xo >= 32 && xo <= 47 && xo != 35 && xo != 45; }

static bool x31_known(int xo)
{
    switch (xo) {
    case 6: case 38: case 7: case 39: case 71: case 103: case 359: /* lvsl lvsr lvebx lvehx lvewx lvx lvxl */
    case 135: case 167: case 199: case 231: case 487:              /* stvebx stvehx stvewx stvx stvxl */
    case 342: case 374: case 822:                                  /* dst(t) dstst(t) dss(all) */
        return true;
    default:
        return false;
    }
}

bool ppc_vec_insn(uint32_t op)
{
    if (OPCD(op) == 4) {
        if (va_known((int)(op & 0x3F)))
            return true;
        if (vc_known((int)(op & 0x3FF)))
            return true;
        return vx_known((int)(op & 0x7FF));
    }
    return OPCD(op) == 31 && x31_known((int)((op >> 1) & 0x3FF));
}

bool ppc_vec_op4(ppc_cpu *c, uint32_t op)
{
    if (!ppc_altivec)
        return false;
    ppc_vec_count++;
    uint8_t A[16], B[16], C[16], R[16];
    memcpy(A, c->vr[VA(op)], 16);
    memcpy(B, c->vr[VB(op)], 16);
    int d = VD(op);
    if (op & 0x20) { /* VA form (extended opcodes 32-47) */
        memcpy(C, c->vr[VC(op)], 16);
        if (!va_form(c, op, A, B, C, R))
            return false;
    } else if (vc_known((int)(op & 0x3FF))) {
        /* VC form: compares, Rc in bit 21 (0x400).  Their extended
         * opcodes' low 4 bits are 6, which no VX-form one's are. */
        if (!compare(c, op, A, B, R))
            return false;
    } else if ((op & 0x7FF) == 1604) { /* mtvscr */
        c->vscr = ew(B, 3) & (PPC_VSCR_NJ | PPC_VSCR_SAT);
        return true;
    } else if (!vx_form(c, op, A, B, R)) {
        return false;
    }
    memcpy(c->vr[d], R, 16);
    return true;
}

bool ppc_vec_op31(ppc_cpu *c, uint32_t op)
{
    int xo = (int)((op >> 1) & 0x3FF);
    if (!ppc_altivec || !x31_known(xo))
        return false;
    ppc_vec_count++;
    int d = VD(op), a = VA(op);
    uint32_t ea = (a ? c->gpr[a] : 0) + c->gpr[VB(op)];
    uint8_t *v = c->vr[d];
    switch (xo) {
    case 6: /* lvsl */
    case 38: { /* lvsr */
        int sh = (int)(ea & 15);
        for (int i = 0; i < 16; i++)
            v[i] = (uint8_t)(xo == 6 ? sh + i : 16 - sh + i);
        return true;
    }
    case 7: case 39: case 71: /* lvebx, lvehx, lvewx: the quadword, as the G4 loads it */
    case 103: case 359: { /* lvx, lvxl */
        uint32_t q = ea & ~15u;
        for (int i = 0; i < 4; i++)
            sw32(v, i, rd32(q + 4 * (uint32_t)i));
        return true;
    }
    case 135: vwr8(ea, v[ea & 15]); return true;                                   /* stvebx */
    case 167: ea &= ~1u; vwr16(ea, (uint16_t)eh(v, (int)(ea & 15) >> 1)); return true; /* stvehx */
    case 199: ea &= ~3u; vwr32(ea, ew(v, (int)(ea & 15) >> 2)); return true;           /* stvewx */
    case 231: case 487: { /* stvx, stvxl */
        uint32_t q = ea & ~15u;
        for (int i = 0; i < 4; i++)
            vwr32(q + 4 * (uint32_t)i, ew(v, i));
        return true;
    }
    default: /* dst, dstt, dstst, dststt, dss, dssall: hints, nothing to do */
        return true;
    }
}
