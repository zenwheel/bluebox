/*
 * An arm64 (A64) instruction encoder for the JITs: one function per
 * instruction form they use, each appending a 32-bit word to an a64_buf.
 * Registers are numbers 0-31; 31 is the zero register (wzr/xzr) in the
 * forms that read it so, and sp in the immediate add/sub and load/store
 * base forms (the architecture's rule, noted where it matters).
 */
#ifndef CCORE_A64_H
#define CCORE_A64_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct a64_buf {
    uint32_t *p;    /* start */
    uint32_t n;     /* words emitted */
    uint32_t cap;   /* words available */
    bool full;      /* ran out of room: the translation is abandoned */
} a64_buf;

enum { A64_ZR = 31, A64_SP = 31 };

enum a64_cond {
    A64_EQ = 0, A64_NE, A64_HS, A64_LO, A64_MI, A64_PL, A64_VS, A64_VC,
    A64_HI, A64_LS, A64_GE, A64_LT, A64_GT, A64_LE, A64_AL
};
static inline int a64_invert(int cond) { return cond ^ 1; }

static inline void a64_emit(a64_buf *b, uint32_t w)
{
    if (b->n >= b->cap) {
        b->full = true;
        return;
    }
    b->p[b->n++] = w;
}
static inline uint32_t a64_here(const a64_buf *b) { return b->n; }

#define A64_R(rd, rn, rm) ((uint32_t)(rd) | (uint32_t)(rn) << 5 | (uint32_t)(rm) << 16)
#define A64_SF(x64) ((x64) ? 0x80000000u : 0)

/* Moves */
static inline void a64_movz(a64_buf *b, bool x, int rd, uint16_t imm, int hw)
{
    a64_emit(b, A64_SF(x) | 0x52800000u | (uint32_t)hw << 21 | (uint32_t)imm << 5 | (uint32_t)rd);
}
static inline void a64_movk(a64_buf *b, bool x, int rd, uint16_t imm, int hw)
{
    a64_emit(b, A64_SF(x) | 0x72800000u | (uint32_t)hw << 21 | (uint32_t)imm << 5 | (uint32_t)rd);
}
static inline void a64_movn(a64_buf *b, bool x, int rd, uint16_t imm, int hw)
{
    a64_emit(b, A64_SF(x) | 0x12800000u | (uint32_t)hw << 21 | (uint32_t)imm << 5 | (uint32_t)rd);
}
/* A 32-bit constant in one or two instructions. */
static inline void a64_mov32(a64_buf *b, int rd, uint32_t v)
{
    if ((v >> 16) == 0)
        a64_movz(b, false, rd, (uint16_t)v, 0);
    else if ((v & 0xFFFF) == 0)
        a64_movz(b, false, rd, (uint16_t)(v >> 16), 1);
    else if ((v >> 16) == 0xFFFF)
        a64_movn(b, false, rd, (uint16_t)~v, 0);
    else {
        a64_movz(b, false, rd, (uint16_t)v, 0);
        a64_movk(b, false, rd, (uint16_t)(v >> 16), 1);
    }
}
/* orr rd, zr, rm */
static inline void a64_mov(a64_buf *b, bool x, int rd, int rm)
{
    a64_emit(b, A64_SF(x) | 0x2A0003E0u | (uint32_t)rm << 16 | (uint32_t)rd);
}

/* Add and subtract, shifted register (31 is zr).  shift: 0 lsl, 1 lsr, 2 asr. */
enum { A64_ADD = 0x0B000000u, A64_ADDS = 0x2B000000u, A64_SUB = 0x4B000000u, A64_SUBS = 0x6B000000u };
static inline void a64_addsub(a64_buf *b, uint32_t kind, bool x, int rd, int rn, int rm, int shift, int amount)
{
    a64_emit(b, A64_SF(x) | kind | (uint32_t)shift << 22 | (uint32_t)amount << 10 | A64_R(rd, rn, rm));
}
static inline void a64_add(a64_buf *b, int rd, int rn, int rm) { a64_addsub(b, A64_ADD, false, rd, rn, rm, 0, 0); }
static inline void a64_sub(a64_buf *b, int rd, int rn, int rm) { a64_addsub(b, A64_SUB, false, rd, rn, rm, 0, 0); }
static inline void a64_cmp(a64_buf *b, int rn, int rm) { a64_addsub(b, A64_SUBS, false, A64_ZR, rn, rm, 0, 0); }

/* Add and subtract, 12-bit immediate (31 is sp here, not zr). */
enum { A64_ADDI = 0x11000000u, A64_ADDSI = 0x31000000u, A64_SUBI = 0x51000000u, A64_SUBSI = 0x71000000u };
static inline void a64_addsub_imm(a64_buf *b, uint32_t kind, bool x, int rd, int rn, uint32_t imm12)
{
    a64_emit(b, A64_SF(x) | kind | (imm12 & 0xFFF) << 10 | (uint32_t)rn << 5 | (uint32_t)rd);
}
static inline void a64_cmp_imm(a64_buf *b, int rn, uint32_t imm12) { a64_addsub_imm(b, A64_SUBSI, false, A64_ZR, rn, imm12); }

/* Add and subtract with carry. */
enum { A64_ADC = 0x1A000000u, A64_ADCS = 0x3A000000u, A64_SBC = 0x5A000000u, A64_SBCS = 0x7A000000u };
static inline void a64_carry(a64_buf *b, uint32_t kind, int rd, int rn, int rm)
{
    a64_emit(b, kind | A64_R(rd, rn, rm));
}

/* Logical, shifted register (31 is zr).  shift: 0 lsl, 1 lsr, 2 asr, 3 ror. */
enum {
    A64_AND = 0x0A000000u, A64_BIC = 0x0A200000u, A64_ORR = 0x2A000000u, A64_ORN = 0x2A200000u,
    A64_EOR = 0x4A000000u, A64_EON = 0x4A200000u, A64_ANDS = 0x6A000000u
};
static inline void a64_logic(a64_buf *b, uint32_t kind, bool x, int rd, int rn, int rm, int shift, int amount)
{
    a64_emit(b, A64_SF(x) | kind | (uint32_t)shift << 22 | (uint32_t)amount << 10 | A64_R(rd, rn, rm));
}
static inline void a64_log(a64_buf *b, uint32_t kind, int rd, int rn, int rm) { a64_logic(b, kind, false, rd, rn, rm, 0, 0); }

/* Shifts by register: 0 lslv, 1 lsrv, 2 asrv, 3 rorv. */
static inline void a64_shiftv(a64_buf *b, bool x, int kind, int rd, int rn, int rm)
{
    a64_emit(b, A64_SF(x) | 0x1AC02000u | (uint32_t)kind << 10 | A64_R(rd, rn, rm));
}

/* Bitfield moves. */
static inline void a64_ubfm(a64_buf *b, bool x, int rd, int rn, int immr, int imms)
{
    a64_emit(b, (x ? 0xD3400000u : 0x53000000u) | (uint32_t)immr << 16 | (uint32_t)imms << 10 | (uint32_t)rn << 5 |
                    (uint32_t)rd);
}
static inline void a64_sbfm(a64_buf *b, bool x, int rd, int rn, int immr, int imms)
{
    a64_emit(b, (x ? 0x93400000u : 0x13000000u) | (uint32_t)immr << 16 | (uint32_t)imms << 10 | (uint32_t)rn << 5 |
                    (uint32_t)rd);
}
static inline void a64_bfm(a64_buf *b, int rd, int rn, int immr, int imms)
{
    a64_emit(b, 0x33000000u | (uint32_t)immr << 16 | (uint32_t)imms << 10 | (uint32_t)rn << 5 | (uint32_t)rd);
}
static inline void a64_lsl(a64_buf *b, int rd, int rn, int s) { a64_ubfm(b, false, rd, rn, (32 - s) & 31, 31 - s); }
static inline void a64_lsr(a64_buf *b, int rd, int rn, int s) { a64_ubfm(b, false, rd, rn, s, 31); }
static inline void a64_asr(a64_buf *b, int rd, int rn, int s) { a64_sbfm(b, false, rd, rn, s, 31); }
/* bfi rd, rn, #lsb, #width */
static inline void a64_bfi(a64_buf *b, int rd, int rn, int lsb, int width) { a64_bfm(b, rd, rn, (32 - lsb) & 31, width - 1); }
/* ubfx rd, rn, #lsb, #width */
static inline void a64_ubfx(a64_buf *b, int rd, int rn, int lsb, int width) { a64_ubfm(b, false, rd, rn, lsb, lsb + width - 1); }
/* extr rd, rn, rm, #lsb (32-bit); ror when rn == rm. */
static inline void a64_extr(a64_buf *b, int rd, int rn, int rm, int lsb)
{
    a64_emit(b, 0x13800000u | (uint32_t)lsb << 10 | A64_R(rd, rn, rm));
}
static inline void a64_ror(a64_buf *b, int rd, int rn, int s) { a64_extr(b, rd, rn, rn, s & 31); }
static inline void a64_sxtb(a64_buf *b, int rd, int rn) { a64_sbfm(b, false, rd, rn, 0, 7); }
static inline void a64_sxth(a64_buf *b, int rd, int rn) { a64_sbfm(b, false, rd, rn, 0, 15); }
static inline void a64_sxtw(a64_buf *b, int rd, int rn) { a64_sbfm(b, true, rd, rn, 0, 31); }

/* Multiply and divide. */
static inline void a64_mul(a64_buf *b, int rd, int rn, int rm) { a64_emit(b, 0x1B007C00u | A64_R(rd, rn, rm)); }
static inline void a64_smull(a64_buf *b, int rd, int rn, int rm) { a64_emit(b, 0x9B207C00u | A64_R(rd, rn, rm)); }
static inline void a64_umull(a64_buf *b, int rd, int rn, int rm) { a64_emit(b, 0x9BA07C00u | A64_R(rd, rn, rm)); }
static inline void a64_udiv(a64_buf *b, int rd, int rn, int rm) { a64_emit(b, 0x1AC00800u | A64_R(rd, rn, rm)); }
static inline void a64_sdiv(a64_buf *b, int rd, int rn, int rm) { a64_emit(b, 0x1AC00C00u | A64_R(rd, rn, rm)); }

/* Bit operations. */
static inline void a64_clz(a64_buf *b, int rd, int rn) { a64_emit(b, 0x5AC01000u | (uint32_t)rn << 5 | (uint32_t)rd); }
static inline void a64_rev(a64_buf *b, int rd, int rn) { a64_emit(b, 0x5AC00800u | (uint32_t)rn << 5 | (uint32_t)rd); }
static inline void a64_rev16(a64_buf *b, int rd, int rn) { a64_emit(b, 0x5AC00400u | (uint32_t)rn << 5 | (uint32_t)rd); }
static inline void a64_rev64(a64_buf *b, int rd, int rn) { a64_emit(b, 0xDAC00C00u | (uint32_t)rn << 5 | (uint32_t)rd); }

/* Conditional select. */
static inline void a64_csel(a64_buf *b, int rd, int rn, int rm, int cond)
{
    a64_emit(b, 0x1A800000u | (uint32_t)cond << 12 | A64_R(rd, rn, rm));
}
/* cset rd, cond: csinc rd, zr, zr, !cond */
static inline void a64_cset(a64_buf *b, int rd, int cond)
{
    a64_emit(b, 0x1A800400u | (uint32_t)a64_invert(cond) << 12 | A64_R(rd, A64_ZR, A64_ZR));
}

/* Loads and stores with an unsigned, scaled 12-bit offset (base 31 is sp). */
enum {
    A64_LDRB = 0x39400000u, A64_STRB = 0x39000000u, A64_LDRH = 0x79400000u, A64_STRH = 0x79000000u,
    A64_LDRW = 0xB9400000u, A64_STRW = 0xB9000000u, A64_LDRX = 0xF9400000u, A64_STRX = 0xF9000000u
};
static inline void a64_ldst(a64_buf *b, uint32_t kind, int rt, int rn, uint32_t byte_off)
{
    int scale = (int)(kind >> 30);
    a64_emit(b, kind | (byte_off >> scale) << 10 | (uint32_t)rn << 5 | (uint32_t)rt);
}
/* ldp/stp of 32-bit registers, signed offset (multiple of 4). */
static inline void a64_ldpw(a64_buf *b, int rt, int rt2, int rn, int off)
{
    a64_emit(b, 0x29400000u | ((uint32_t)(off / 4) & 0x7F) << 15 | (uint32_t)rt2 << 10 | (uint32_t)rn << 5 |
                    (uint32_t)rt);
}
/* Register-offset form: [xn, wm, uxtw] (a guest address added to the arena). */
enum {
    A64_LDRB_R = 0x38604800u, A64_STRB_R = 0x38204800u, A64_LDRH_R = 0x78604800u, A64_STRH_R = 0x78204800u,
    A64_LDRW_R = 0xB8604800u, A64_STRW_R = 0xB8204800u, A64_LDRX_R = 0xF8604800u, A64_STRX_R = 0xF8204800u
};
static inline void a64_ldst_r(a64_buf *b, uint32_t kind, int rt, int rn, int rm)
{
    a64_emit(b, kind | A64_R(rt, rn, rm));
}
/* stp/ldp of 64-bit registers: pre-index store and post-index load (sp frames). */
static inline void a64_stp_pre(a64_buf *b, int rt, int rt2, int rn, int off)
{
    a64_emit(b, 0xA9800000u | ((uint32_t)(off / 8) & 0x7F) << 15 | (uint32_t)rt2 << 10 | (uint32_t)rn << 5 |
                    (uint32_t)rt);
}
static inline void a64_ldp_post(a64_buf *b, int rt, int rt2, int rn, int off)
{
    a64_emit(b, 0xA8C00000u | ((uint32_t)(off / 8) & 0x7F) << 15 | (uint32_t)rt2 << 10 | (uint32_t)rn << 5 |
                    (uint32_t)rt);
}
static inline void a64_stp(a64_buf *b, int rt, int rt2, int rn, int off)
{
    a64_emit(b, 0xA9000000u | ((uint32_t)(off / 8) & 0x7F) << 15 | (uint32_t)rt2 << 10 | (uint32_t)rn << 5 |
                    (uint32_t)rt);
}
static inline void a64_ldp(a64_buf *b, int rt, int rt2, int rn, int off)
{
    a64_emit(b, 0xA9400000u | ((uint32_t)(off / 8) & 0x7F) << 15 | (uint32_t)rt2 << 10 | (uint32_t)rn << 5 |
                    (uint32_t)rt);
}

/* Branches.  Offsets are in words from the branch itself. */
static inline uint32_t a64_b_word(int32_t words) { return 0x14000000u | ((uint32_t)words & 0x03FFFFFFu); }
static inline void a64_b(a64_buf *b, int32_t words) { a64_emit(b, a64_b_word(words)); }
static inline void a64_bcond(a64_buf *b, int cond, int32_t words)
{
    a64_emit(b, 0x54000000u | ((uint32_t)words & 0x7FFFF) << 5 | (uint32_t)cond);
}
static inline void a64_cbz(a64_buf *b, bool nonzero, int rt, int32_t words)
{
    a64_emit(b, (nonzero ? 0x35000000u : 0x34000000u) | ((uint32_t)words & 0x7FFFF) << 5 | (uint32_t)rt);
}
static inline void a64_tbz(a64_buf *b, bool nonzero, int rt, int bit, int32_t words)
{
    a64_emit(b, (nonzero ? 0x37000000u : 0x36000000u) | (uint32_t)(bit >> 5) << 31 | (uint32_t)(bit & 31) << 19 |
                    ((uint32_t)words & 0x3FFF) << 5 | (uint32_t)rt);
}
static inline void a64_br(a64_buf *b, int rn) { a64_emit(b, 0xD61F0000u | (uint32_t)rn << 5); }
static inline void a64_blr(a64_buf *b, int rn) { a64_emit(b, 0xD63F0000u | (uint32_t)rn << 5); }
static inline void a64_ret(a64_buf *b) { a64_emit(b, 0xD65F03C0u); }
/* adr rd, words from here */
static inline void a64_adr(a64_buf *b, int rd, int32_t words)
{
    uint32_t imm = (uint32_t)(words * 4);
    a64_emit(b, 0x10000000u | (imm & 3) << 29 | ((imm >> 2) & 0x7FFFF) << 5 | (uint32_t)rd);
}

/* Patch a forward branch emitted at `at` (b, b.cond, cbz, tbz) to land here. */
static inline void a64_patch_here(a64_buf *b, uint32_t at)
{
    if (b->full)
        return;
    uint32_t w = b->p[at];
    int32_t d = (int32_t)(b->n - at);
    if ((w & 0xFC000000u) == 0x14000000u)
        w = a64_b_word(d);
    else if ((w & 0x7E000000u) == 0x36000000u) /* tbz/tbnz */
        w = (w & ~(0x3FFFu << 5)) | ((uint32_t)d & 0x3FFF) << 5;
    else /* b.cond, cbz/cbnz */
        w = (w & ~(0x7FFFFu << 5)) | ((uint32_t)d & 0x7FFFF) << 5;
    b->p[at] = w;
}

#endif
