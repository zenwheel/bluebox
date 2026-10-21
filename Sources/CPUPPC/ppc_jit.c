/*
 * PowerPC JIT: basic blocks of guest code translated to arm64.  See ppc.h.
 *
 * A block is straight-line code from an address up to and including its
 * first branch (or 64 instructions, or an instruction left to the
 * interpreter: sc, the NATIVE opcode, traps, the time base, anything
 * illegal).  The common integer, compare, rotate, load, store and branch
 * instructions become host code; the rest of the user instruction set
 * (floating point, string and multiple loads and stores, lwarx/stwcx.,
 * dcbz, condition register logic) is run inside the block by calling the
 * interpreter's ppc_execute for that one instruction.
 *
 * Guest state stays in the ppc_cpu structure; translated code loads and
 * stores it there, except that the five GPRs a block uses most are kept
 * in x24-x28 while it runs (loaded on entry, written back before an
 * instruction goes to the interpreter and before the block's exits).
 * Host registers while a block runs:
 *   x19  the ppc_cpu
 *   x20  the guest memory arena (g_mem)
 *   x21  the JIT context (helpers, the exit, the screen watch, lookups)
 *   x22  the instruction budget left (signed)
 *   x23  the code map (mem.h)
 * Translated code holds no absolute host addresses (helpers are called
 * through the context, branches inside a block are relative), so a block
 * can be copied anywhere: that is what the persistent cache stores.
 *
 * Instruction count: a block subtracts its length from the budget on
 * entry and leaves at once if the budget doesn't cover it, so the
 * dispatcher can add exactly what ran to icount and tb; the interpreter
 * takes the last few instructions of a slice.  mftb and mfspr of the time
 * base end blocks, so they see the same tb as under the interpreter.
 *
 * Condition register: compares and record forms set the CR field in the
 * ppc_cpu, unless a later instruction in the block sets the same field
 * before anything reads it; a conditional branch testing a field the host
 * flags still describe branches on the flags directly.
 *
 * Exits: a block ends in exit stubs, one per known successor.  A stub
 * stores the guest pc and returns to the dispatcher, which then patches
 * the stub's first instruction into a direct branch to the successor's
 * translation (chaining).  Indirect branches (blr, bctr) look the target
 * up in a direct-mapped table inline and go straight there when it has a
 * translation.
 *
 * Memory: loads and stores go to the same arena the interpreter uses.  A
 * store checks the screen watch range and the code map inline; when
 * either is hit it goes through a helper that does what the interpreter
 * does (watch_store, then wrN, whose code-map check drops translations of
 * the code written).  Any store into translated code, from guest code or
 * the Toolbox, so drops the blocks of that 256-byte region: their entry is
 * patched into an exit, and the dispatcher translates the new code when
 * it is next reached.  icbi drops the blocks of its cache line as well.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "jit.h"
#include "ppc.h"

bool ppc_jit_enabled;

#if !(defined(__aarch64__) || defined(__arm64__))

bool ppc_jit_available(void) { return false; }
void ppc_jit_enable(bool on, const char *cache_dir, const char *cache_name) { ppc_jit_enabled = false; }
uint64_t ppc_jit_run(ppc_cpu *c, uint64_t max)
{
    uint64_t start = c->icount;
    while (!c->halted && c->icount - start < max)
        ppc_step(c);
    return c->icount - start;
}
void ppc_jit_flush(void) {}
void ppc_jit_get_stats(ppc_jit_stats *s) { memset(s, 0, sizeof *s); }
int ppc_jit_selftest(uint32_t seed, int blocks, uint32_t code_at, uint32_t scratch, char *why, int whylen)
{
    snprintf(why, (size_t)whylen, "no JIT on this host");
    return -1;
}

#else

#include "jit_a64.h"

enum { HC = JR_CPU, HB = JR_BUDGET };
enum { MAX_INSN = 64, CODE_SIZE = 64 << 20 };
/* Bumped whenever translated code changes shape (the cache's build). */
#define TRANSLATOR_VERSION 3

/* The translator's helpers in the engine's context. */
enum { H_EXECUTE, H_ST8, H_ST16, H_ST32, H_ICBI };

static jit_engine E;
static uint32_t *scratch; /* translation buffer */
static ppc_jit_stats st;

/* ------------------------------------------------------------------ */
/* Helpers translated code calls */

static void st8(uint32_t a, uint32_t v)
{
    watch_store(a);
    wr8(a, (uint8_t)v);
}
static void st16(uint32_t a, uint32_t v)
{
    watch_store(a);
    wr16(a, (uint16_t)v);
}
static void st32(uint32_t a, uint32_t v)
{
    watch_store(a);
    wr32(a, v);
}
static void do_icbi(uint32_t a)
{
    a &= ~31u;
    if (g_code_map)
        mem_code_range(a, 32);
}

void ppc_jit_flush(void)
{
    if (E.ready)
        jit_flush(&E);
}

/* ------------------------------------------------------------------ */
/* Emitting */

#define GPR(n) ((uint32_t)offsetof(ppc_cpu, gpr) + 4u * (uint32_t)(n))
#define FIELD(f) ((uint32_t)offsetof(ppc_cpu, f))

typedef struct tctx {
    a64_buf b;
    uint32_t pc, ninsn;
    uint32_t last_pc;            /* of the instruction being translated */
    int flags_crf;               /* CR field the host flags hold, or -1 */
    bool flags_signed;
    bool keeps_flags;            /* the instruction just emitted left them */
    int sets_flags;              /* ... or set them for this field */
    uint8_t cr_dead[MAX_INSN];   /* fields written that nothing reads */
    int at;                      /* index of the instruction */
    bool stored;                 /* the instruction may have stored */
    int8_t host[32];             /* the host register caching a GPR, or -1 */
    bool dirty[32];              /* ... changed since it was loaded */
} tctx;

/* GPRs: through the host register caching them, if any. */
static void ldg(tctx *t, int w, int r)
{
    if (t->host[r] >= 0)
        a64_mov(&t->b, false, w, t->host[r]);
    else
        a64_ldst(&t->b, A64_LDRW, w, HC, GPR(r));
}
static void stg(tctx *t, int w, int r)
{
    if (t->host[r] >= 0) {
        a64_mov(&t->b, false, t->host[r], w);
        t->dirty[r] = true;
    } else {
        a64_ldst(&t->b, A64_STRW, w, HC, GPR(r));
    }
}
/* The cached GPRs written back to the ppc_cpu (before the interpreter
 * runs an instruction, and at the block's end). */
static void cache_flush(tctx *t)
{
    for (int r = 0; r < 32; r++)
        if (t->host[r] >= 0 && t->dirty[r]) {
            a64_ldst(&t->b, A64_STRW, t->host[r], HC, GPR(r));
            t->dirty[r] = false;
        }
}
/* The same, on a path leaving the block (the main path's state kept). */
static void cache_flush_exit(tctx *t)
{
    for (int r = 0; r < 32; r++)
        if (t->host[r] >= 0 && t->dirty[r])
            a64_ldst(&t->b, A64_STRW, t->host[r], HC, GPR(r));
}
static void cache_load(tctx *t)
{
    for (int r = 0; r < 32; r++)
        if (t->host[r] >= 0)
            a64_ldst(&t->b, A64_LDRW, t->host[r], HC, GPR(r));
}
static void ldf(tctx *t, int w, uint32_t off) { a64_ldst(&t->b, A64_LDRW, w, HC, off); }
static void stf(tctx *t, int w, uint32_t off) { a64_ldst(&t->b, A64_STRW, w, HC, off); }
/* rA, or 0 for register 0 where the instruction says so. */
static void ldg0(tctx *t, int w, int r)
{
    if (r)
        ldg(t, w, r);
    else
        a64_movz(&t->b, false, w, 0, 0);
}


/* w = w + imm (any 32-bit immediate; w9 is free to use). */
static void add_imm(tctx *t, int wd, int wn, int32_t imm)
{
    if (imm >= 0 && imm < 4096)
        a64_addsub_imm(&t->b, A64_ADDI, false, wd, wn, (uint32_t)imm);
    else if (imm < 0 && imm > -4096)
        a64_addsub_imm(&t->b, A64_SUBI, false, wd, wn, (uint32_t)-imm);
    else {
        a64_mov32(&t->b, 9, (uint32_t)imm);
        a64_add(&t->b, wd, wn, 9);
    }
}

/* The LT GT EQ bits of CR field bf from the host flags (a compare just
 * made), with SO from XER.  Uses w9-w11. */
static void cr_from_flags(tctx *t, int bf, bool is_signed)
{
    t->sets_flags = bf;
    t->flags_signed = is_signed;
    if (t->cr_dead[t->at] & (1 << bf))
        return;
    a64_buf *b = &t->b;
    a64_cset(b, 9, is_signed ? A64_LT : A64_LO);
    a64_cset(b, 10, is_signed ? A64_GT : A64_HI);
    a64_logic(b, A64_ORR, false, 9, 10, 9, 0, 1);  /* lt<<1 | gt */
    a64_cset(b, 10, A64_EQ);
    a64_logic(b, A64_ORR, false, 9, 10, 9, 0, 1);  /* lt<<2 | gt<<1 | eq */
    ldf(t, 10, FIELD(xer));
    a64_lsr(b, 10, 10, 31);
    a64_logic(b, A64_ORR, false, 9, 10, 9, 0, 1);  /* ... | so */
    ldf(t, 11, FIELD(cr));
    a64_bfi(b, 11, 9, 28 - 4 * bf, 4);
    stf(t, 11, FIELD(cr));
}

/* CR0 from a result in w0. */
static void record(tctx *t)
{
    a64_cmp_imm(&t->b, 0, 0);
    cr_from_flags(t, 0, true);
}

/* XER[CA] from the host carry flag. */
static void ca_from_flags(tctx *t)
{
    a64_cset(&t->b, 10, A64_HS);
    ldf(t, 9, FIELD(xer));
    a64_bfi(&t->b, 9, 10, 29, 1);
    stf(t, 9, FIELD(xer));
}
/* The host carry flag from XER[CA]. */
static void flags_from_ca(tctx *t)
{
    ldf(t, 9, FIELD(xer));
    a64_ubfx(&t->b, 9, 9, 29, 1);
    a64_cmp_imm(&t->b, 9, 1);
}

static void load(tctx *t, int size, bool algebraic, bool reversed) { jit_load(&t->b, size, algebraic, reversed); }

/* Guest stores: address in w1, value in w2 (in guest order: byte-reversed
 * stores reverse it first). */
static void store(tctx *t, int size, bool reversed)
{
    a64_buf *b = &t->b;
    if (reversed) {
        if (size == 4)
            a64_rev(b, 2, 2);
        else if (size == 2) {
            a64_rev16(b, 2, 2);
            a64_ubfx(b, 2, 2, 0, 16);
        }
    }
    jit_store(b, size, true, H_ST8);
    t->stored = true;
}

/* Run one instruction through the interpreter. */
static void execute(tctx *t, uint32_t op)
{
    a64_mov(&t->b, true, 0, HC);
    a64_mov32(&t->b, 1, op);
    jit_call(&t->b, H_EXECUTE);
}

static void exit_plain(tctx *t) { jit_exit_plain(&t->b); }

/* op_pc and prev_pc as the interpreter leaves them after the block. */
static void exit_pcs(tctx *t)
{
    a64_buf *b = &t->b;
    if (t->ninsn >= 2) {
        a64_mov32(b, 0, t->last_pc - 4);
    } else {
        ldf(t, 0, FIELD(op_pc));
    }
    stf(t, 0, FIELD(prev_pc));
    a64_mov32(b, 0, t->last_pc);
    stf(t, 0, FIELD(op_pc));
}

/* A chainable exit to a known guest address.  Its first instruction is
 * the one patched into a branch to the successor. */
static void exit_to(tctx *t, uint32_t target)
{
    a64_buf *b = &t->b;
    jit_exit_chainable_head(b);
    a64_mov32(b, 0, target);
    stf(t, 0, FIELD(pc));
    exit_pcs(t);
    jit_exit_chainable_tail(b);
}

/* An exit to the address in w12: straight to its translation when the
 * table knows it, else back to the dispatcher. */
static void exit_indirect(tctx *t)
{
    stf(t, 12, FIELD(pc));
    exit_pcs(t);
    jit_fast_lookup(&t->b, 12, 2);
    exit_plain(t);
}

/* ------------------------------------------------------------------ */
/* Decoding */

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

enum kind { K_STOP, K_NATIVE, K_EXEC, K_BRANCH };

static bool arith_xo9(uint32_t x)
{
    switch (x) {
    case 266: case 10: case 138: case 234: case 202: case 40: case 8: case 136:
    case 232: case 200: case 104: case 235: case 75: case 11: case 491: case 459:
        return true;
    default:
        return false;
    }
}

static bool fp_a_form(uint32_t x5)
{
    switch (x5) {
    case 18: case 20: case 21: case 22: case 23: case 24: case 25: case 26: case 28: case 29: case 30: case 31:
        return true;
    default:
        return false;
    }
}

static enum kind classify(uint32_t op)
{
    switch (OPCD(op)) {
    case 7: case 8: case 10: case 11: case 12: case 13: case 14: case 15:
    case 20: case 21: case 23: case 24: case 25: case 26: case 27: case 28: case 29:
    case 32: case 33: case 34: case 35: case 36: case 37: case 38: case 39:
    case 40: case 41: case 42: case 43: case 44: case 45:
        return K_NATIVE;
    case 46: case 47: case 48: case 49: case 50: case 51: case 52: case 53: case 54: case 55:
        return K_EXEC;
    case 16: case 18:
        return K_BRANCH;
    case 19:
        switch (XO10(op)) {
        case 16: case 528: return K_BRANCH;
        case 150: return K_NATIVE; /* isync */
        case 0: case 33: case 129: case 193: case 225: case 257: case 289: case 417: case 449:
            return K_EXEC;
        default: return K_STOP;
        }
    case 31: {
        if (arith_xo9(XO9(op)))
            return OEBIT(op) || XO9(op) == 491 ? K_EXEC : K_NATIVE;
        switch (XO10(op)) {
        case 0: case 32: case 19: case 144: case 83:
        case 23: case 55: case 87: case 119: case 279: case 311: case 343: case 375:
        case 151: case 183: case 215: case 247: case 407: case 439:
        case 534: case 790: case 662: case 918:
        case 28: case 60: case 444: case 412: case 316: case 476: case 124: case 284:
        case 954: case 922: case 26: case 24: case 536: case 792: case 824:
        case 54: case 86: case 246: case 278: case 470: case 982:
        case 598: case 854: case 306: case 370: case 566: case 146: case 210: case 242:
            return K_NATIVE;
        case 339: case 467: {
            uint32_t spr = ((op >> 16) & 31) | (((op >> 11) & 31) << 5);
            if (spr == 268 || spr == 269)
                return K_STOP; /* the time base: counted per instruction */
            return (spr == 1 || spr == 8 || spr == 9) ? K_NATIVE : K_EXEC;
        }
        case 512: case 595: case 659: case 20: case 150: case 533: case 597: case 661: case 725:
        case 535: case 567: case 599: case 631: case 663: case 695: case 727: case 759: case 983: case 1014:
            return K_EXEC;
        default:
            return K_STOP; /* tw, mftb, and anything illegal */
        }
    }
    case 59:
        return fp_a_form(XO5(op)) ? K_EXEC : K_STOP;
    case 63:
        if (fp_a_form(XO5(op)))
            return K_EXEC;
        switch (XO10(op)) {
        case 0: case 32: case 12: case 14: case 15: case 38: case 70: case 40: case 72: case 136: case 264:
        case 64: case 134: case 583: case 711:
            return K_EXEC;
        default:
            return K_STOP;
        }
    default:
        return K_STOP; /* sc, twi, NATIVE, illegal */
    }
}

/* Whether an instruction left to the interpreter may store. */
static bool exec_stores(uint32_t op)
{
    switch (OPCD(op)) {
    case 47: case 52: case 53: case 54: case 55:
        return true;
    case 31:
        switch (XO10(op)) {
        case 150: case 661: case 725: case 663: case 695: case 727: case 759: case 983: case 1014:
            return true;
        default:
            return false;
        }
    default:
        return false;
    }
}

/* Whether a translated instruction sets CR0 from its result. */
static bool records(uint32_t op)
{
    switch (OPCD(op)) {
    case 13: case 28: case 29:
        return true;
    case 20: case 21: case 23:
        return RCBIT(op);
    case 31:
        if (!RCBIT(op))
            return false;
        if (arith_xo9(XO9(op)))
            return true;
        switch (XO10(op)) {
        case 28: case 60: case 444: case 412: case 316: case 476: case 124: case 284:
        case 954: case 922: case 26: case 24: case 536: case 792: case 824:
            return true;
        default:
            return false;
        }
    default:
        return false;
    }
}

/* CR fields an instruction reads and wholly writes (a bit per field, 0x80
 * for field 0). */
static void cr_use(uint32_t op, enum kind k, uint8_t *reads, uint8_t *writes)
{
    *reads = 0;
    *writes = 0;
    if (k == K_EXEC) {
        *reads = 0xFF;
        return;
    }
    int o = (int)OPCD(op);
    if (k == K_BRANCH) {
        bool cond = o == 18 ? false : !(RD(op) & 16);
        if (cond)
            *reads = (uint8_t)(0x80 >> (RA(op) >> 2));
        return;
    }
    if (o == 10 || o == 11 || (o == 31 && (XO10(op) == 0 || XO10(op) == 32))) {
        *writes = (uint8_t)(0x80 >> (RD(op) >> 2));
        return;
    }
    if (o == 31 && XO10(op) == 19) { /* mfcr */
        *reads = 0xFF;
        return;
    }
    if (o == 31 && XO10(op) == 144) { /* mtcrf: fields it sets are written */
        uint32_t fxm = (op >> 12) & 0xFF;
        *writes = (uint8_t)fxm;
        return;
    }
    if (records(op))
        *writes = 0x80;
}

/* ------------------------------------------------------------------ */
/* Translation of one instruction */

static uint32_t rot_mask(int mb, int me)
{
    uint32_t m1 = 0xFFFFFFFFu >> mb, m2 = 0xFFFFFFFFu << (31 - me);
    return mb <= me ? m1 & m2 : m1 | m2;
}

/* w0 = rotl(w1, sh) & mask(mb, me) */
static void rotate_mask(tctx *t, int sh, int mb, int me)
{
    a64_buf *b = &t->b;
    uint32_t m = rot_mask(mb, me);
    if (mb <= me) {
        int width = me - mb + 1, dst = 31 - me;
        int src = ((dst - sh) % 32 + 32) % 32;
        if (src + width <= 32) {
            if (dst == 0) {
                a64_ubfx(b, 0, 1, src, width);
                return;
            }
            if (src == 0) {
                a64_ubfm(b, false, 0, 1, (32 - dst) & 31, width - 1);
                return;
            }
        }
    }
    a64_ror(b, 0, 1, (32 - sh) & 31);
    if (m != 0xFFFFFFFFu) {
        a64_mov32(b, 9, m);
        a64_log(b, A64_AND, 0, 0, 9);
    }
}

static void cond_branch(tctx *t, uint32_t op, uint32_t *not_taken)
{
    /* The CTR test (decremented first) and the CR test, each branching to
     * the not-taken path; the not-taken branches are collected. */
    a64_buf *b = &t->b;
    int bo = RD(op), bi = RA(op);
    int n = 0;
    if (!(bo & 4)) {
        ldf(t, 9, FIELD(ctr));
        a64_addsub_imm(b, A64_SUBI, false, 9, 9, 1);
        stf(t, 9, FIELD(ctr));
        not_taken[n++] = a64_here(b);
        a64_cbz(b, (bo & 2) != 0, 9, 0); /* bo&2: taken when ctr == 0 */
    }
    if (!(bo & 16)) {
        bool want = (bo >> 3) & 1;
        int field = bi >> 2, bit = bi & 3;
        if (t->flags_crf == field && bit != 3) {
            int c = bit == 0 ? (t->flags_signed ? A64_LT : A64_LO)
                  : bit == 1 ? (t->flags_signed ? A64_GT : A64_HI)
                             : A64_EQ;
            not_taken[n++] = a64_here(b);
            a64_bcond(b, want ? a64_invert(c) : c, 0);
        } else {
            ldf(t, 10, FIELD(cr));
            not_taken[n++] = a64_here(b);
            a64_tbz(b, !want, 10, 31 - bi, 0);
        }
    }
    not_taken[n] = UINT32_MAX;
}

/* Returns false when the instruction can't be translated after all. */
static bool emit(tctx *t, uint32_t op, uint32_t pc)
{
    a64_buf *b = &t->b;
    int d = RD(op), a = RA(op), rb = RB(op);
    t->keeps_flags = false;
    t->sets_flags = -1;
    switch (OPCD(op)) {
    case 7: /* mulli */
        ldg(t, 1, a);
        a64_mov32(b, 9, (uint32_t)SIMM(op));
        a64_mul(b, 0, 1, 9);
        stg(t, 0, d);
        t->keeps_flags = true;
        return true;
    case 8: /* subfic */
        ldg(t, 1, a);
        a64_mov32(b, 2, (uint32_t)SIMM(op));
        a64_addsub(b, A64_SUBS, false, 0, 2, 1, 0, 0);
        stg(t, 0, d);
        ca_from_flags(t);
        return true;
    case 10: /* cmpli */
        ldg(t, 1, a);
        a64_mov32(b, 2, UIMM(op));
        a64_cmp(b, 1, 2);
        cr_from_flags(t, d >> 2, false);
        return true;
    case 11: /* cmpi */
        ldg(t, 1, a);
        if (SIMM(op) >= 0 && SIMM(op) < 4096) {
            a64_cmp_imm(b, 1, (uint32_t)SIMM(op));
        } else {
            a64_mov32(b, 2, (uint32_t)SIMM(op));
            a64_cmp(b, 1, 2);
        }
        cr_from_flags(t, d >> 2, true);
        return true;
    case 12: /* addic */
    case 13: /* addic. */
        ldg(t, 1, a);
        a64_mov32(b, 2, (uint32_t)SIMM(op));
        a64_addsub(b, A64_ADDS, false, 0, 1, 2, 0, 0);
        stg(t, 0, d);
        ca_from_flags(t);
        if (OPCD(op) == 13)
            record(t);
        return true;
    case 14: /* addi */
    case 15: /* addis */ {
        int32_t imm = OPCD(op) == 14 ? SIMM(op) : (int32_t)(UIMM(op) << 16);
        if (a) {
            ldg(t, 1, a);
            add_imm(t, 0, 1, imm);
        } else {
            a64_mov32(b, 0, (uint32_t)imm);
        }
        stg(t, 0, d);
        t->keeps_flags = true;
        return true;
    }
    case 20: { /* rlwimi */
        int sh = rb, mb = RC_(op), me = XO5(op);
        ldg(t, 1, d);
        ldg(t, 2, a);
        uint32_t m = rot_mask(mb, me);
        int width = me - mb + 1, dst = 31 - me;
        int src = mb <= me ? ((dst - sh) % 32 + 32) % 32 : -1;
        if (mb <= me && src + width <= 32 && (src == 0 || dst == 0)) {
            if (src == 0)
                a64_bfi(b, 2, 1, dst, width);
            else
                a64_bfm(b, 2, 1, src, src + width - 1); /* bfxil */
            a64_mov(b, false, 0, 2);
        } else {
            a64_ror(b, 3, 1, (32 - sh) & 31);
            a64_mov32(b, 9, m);
            a64_log(b, A64_AND, 3, 3, 9);
            a64_log(b, A64_BIC, 2, 2, 9);
            a64_log(b, A64_ORR, 0, 2, 3);
        }
        stg(t, 0, a);
        if (RCBIT(op))
            record(t);
        else
            t->keeps_flags = true;
        return true;
    }
    case 21: /* rlwinm */
        ldg(t, 1, d);
        rotate_mask(t, rb, RC_(op), XO5(op));
        stg(t, 0, a);
        if (RCBIT(op))
            record(t);
        else
            t->keeps_flags = true;
        return true;
    case 23: { /* rlwnm */
        ldg(t, 1, d);
        ldg(t, 2, rb);
        a64_sub(b, 9, A64_ZR, 2);
        a64_shiftv(b, false, 3, 0, 1, 9); /* rorv by -n: rotl by n */
        uint32_t m = rot_mask(RC_(op), XO5(op));
        if (m != 0xFFFFFFFFu) {
            a64_mov32(b, 9, m);
            a64_log(b, A64_AND, 0, 0, 9);
        }
        stg(t, 0, a);
        if (RCBIT(op))
            record(t);
        else
            t->keeps_flags = true;
        return true;
    }
    case 24: case 25: case 26: case 27: case 28: case 29: { /* ori oris xori xoris andi. andis. */
        uint32_t imm = (OPCD(op) & 1) ? UIMM(op) << 16 : UIMM(op);
        if (OPCD(op) == 24 && d == a && imm == 0) { /* nop */
            t->keeps_flags = true;
            return true;
        }
        ldg(t, 1, d);
        a64_mov32(b, 9, imm);
        a64_log(b, OPCD(op) <= 25 ? A64_ORR : OPCD(op) <= 27 ? A64_EOR : A64_AND, 0, 1, 9);
        stg(t, 0, a);
        if (OPCD(op) >= 28)
            record(t);
        else
            t->keeps_flags = true;
        return true;
    }
    case 32: case 33: case 34: case 35: case 40: case 41: case 42: case 43: { /* lwz lbz lhz lha (u) */
        bool update = OPCD(op) & 1;
        int o = (int)OPCD(op) & ~1;
        ldg0(t, 1, a);
        add_imm(t, 1, 1, SIMM(op));
        load(t, o == 32 ? 4 : o == 34 ? 1 : 2, o == 42, false);
        stg(t, 0, d);
        if (update)
            stg(t, 1, a);
        t->keeps_flags = true;
        return true;
    }
    case 36: case 37: case 38: case 39: case 44: case 45: { /* stw stb sth (u) */
        bool update = OPCD(op) & 1;
        int o = (int)OPCD(op) & ~1;
        ldg0(t, 1, a);
        add_imm(t, 1, 1, SIMM(op));
        ldg(t, 2, d);
        if (update)
            stg(t, 1, a); /* before the store's helper call clobbers w1 */
        store(t, o == 36 ? 4 : o == 38 ? 1 : 2, false);
        return true;
    }
    case 19: /* isync */
        t->keeps_flags = true;
        return XO10(op) == 150;
    case 16: case 18:
        return false; /* branches: emit_branch */
    case 31:
        break;
    default:
        return false;
    }

    /* Opcode 31 */
    if (arith_xo9(XO9(op))) {
        ldg(t, 1, a);
        ldg(t, 2, rb);
        switch (XO9(op)) {
        case 266: a64_add(b, 0, 1, 2); break;                                    /* add */
        case 10: a64_addsub(b, A64_ADDS, false, 0, 1, 2, 0, 0); ca_from_flags(t); break; /* addc */
        case 138: flags_from_ca(t); a64_carry(b, A64_ADCS, 0, 1, 2); ca_from_flags(t); break; /* adde */
        case 202: flags_from_ca(t); a64_carry(b, A64_ADCS, 0, 1, A64_ZR); ca_from_flags(t); break; /* addze */
        case 234: /* addme */
            flags_from_ca(t);
            a64_movn(b, false, 3, 0, 0);
            a64_carry(b, A64_ADCS, 0, 1, 3);
            ca_from_flags(t);
            break;
        case 40: a64_sub(b, 0, 2, 1); break;                                     /* subf */
        case 8: a64_addsub(b, A64_SUBS, false, 0, 2, 1, 0, 0); ca_from_flags(t); break; /* subfc */
        case 136: flags_from_ca(t); a64_carry(b, A64_SBCS, 0, 2, 1); ca_from_flags(t); break; /* subfe */
        case 200: flags_from_ca(t); a64_carry(b, A64_SBCS, 0, A64_ZR, 1); ca_from_flags(t); break; /* subfze */
        case 232: /* subfme */
            flags_from_ca(t);
            a64_movn(b, false, 3, 0, 0);
            a64_carry(b, A64_SBCS, 0, 3, 1);
            ca_from_flags(t);
            break;
        case 104: a64_sub(b, 0, A64_ZR, 1); break;                               /* neg */
        case 235: a64_mul(b, 0, 1, 2); break;                                    /* mullw */
        case 75: a64_smull(b, 0, 1, 2); a64_ubfm(b, true, 0, 0, 32, 63); break;   /* mulhw */
        case 11: a64_umull(b, 0, 1, 2); a64_ubfm(b, true, 0, 0, 32, 63); break;   /* mulhwu */
        case 459: a64_udiv(b, 0, 1, 2); break;                                   /* divwu: 0 for /0 */
        default: return false;
        }
        stg(t, 0, d);
        int x = (int)XO9(op);
        bool carries = x == 10 || x == 138 || x == 202 || x == 234 || x == 8 || x == 136 || x == 200 || x == 232;
        if (RCBIT(op))
            record(t);
        else
            t->keeps_flags = !carries;
        return true;
    }

    switch (XO10(op)) {
    case 0: /* cmp */
    case 32: /* cmpl */
        ldg(t, 1, a);
        ldg(t, 2, rb);
        a64_cmp(b, 1, 2);
        cr_from_flags(t, d >> 2, XO10(op) == 0);
        return true;
    case 19: /* mfcr */
        ldf(t, 0, FIELD(cr));
        stg(t, 0, d);
        t->keeps_flags = true;
        return true;
    case 144: { /* mtcrf */
        uint32_t fxm = (op >> 12) & 0xFF, m = 0;
        for (int i = 0; i < 8; i++)
            if (fxm & (0x80 >> i))
                m |= 0xF0000000u >> (4 * i);
        ldg(t, 1, d);
        ldf(t, 2, FIELD(cr));
        a64_mov32(b, 9, m);
        a64_log(b, A64_AND, 1, 1, 9);
        a64_log(b, A64_BIC, 2, 2, 9);
        a64_log(b, A64_ORR, 0, 1, 2);
        stf(t, 0, FIELD(cr));
        return true; /* the flags no longer describe any field */
    }
    case 83: /* mfmsr */
        a64_mov32(b, 0, 0x0000D032u);
        stg(t, 0, d);
        t->keeps_flags = true;
        return true;
    case 339: { /* mfspr: XER, LR, CTR */
        uint32_t spr = ((op >> 16) & 31) | (((op >> 11) & 31) << 5);
        ldf(t, 0, spr == 1 ? FIELD(xer) : spr == 8 ? FIELD(lr) : FIELD(ctr));
        stg(t, 0, d);
        t->keeps_flags = true;
        return true;
    }
    case 467: { /* mtspr */
        uint32_t spr = ((op >> 16) & 31) | (((op >> 11) & 31) << 5);
        ldg(t, 0, d);
        stf(t, 0, spr == 1 ? FIELD(xer) : spr == 8 ? FIELD(lr) : FIELD(ctr));
        t->keeps_flags = true;
        return true;
    }
    /* Indexed loads and stores */
    case 23: case 55: case 87: case 119: case 279: case 311: case 343: case 375: case 534: case 790: {
        int x = (int)XO10(op);
        bool update = x == 55 || x == 119 || x == 311 || x == 375;
        int size = (x == 23 || x == 55 || x == 534) ? 4 : (x == 87 || x == 119) ? 1 : 2;
        ldg0(t, 1, a);
        ldg(t, 2, rb);
        a64_add(b, 1, 1, 2);
        load(t, size, x == 343 || x == 375, x == 534 || x == 790);
        stg(t, 0, d);
        if (update)
            stg(t, 1, a);
        t->keeps_flags = true;
        return true;
    }
    case 151: case 183: case 215: case 247: case 407: case 439: case 662: case 918: {
        int x = (int)XO10(op);
        bool update = x == 183 || x == 247 || x == 439;
        int size = (x == 151 || x == 183 || x == 662) ? 4 : (x == 215 || x == 247) ? 1 : 2;
        ldg0(t, 1, a);
        ldg(t, 2, rb);
        a64_add(b, 1, 1, 2);
        ldg(t, 2, d);
        if (update)
            stg(t, 1, a);
        store(t, size, x == 662 || x == 918);
        return true;
    }
    /* Logical, shifts, extends: result to rA */
    case 28: case 60: case 444: case 412: case 316: case 476: case 124: case 284: {
        static const struct { int x; uint32_t kind; bool invert; } L[] = {
            { 28, A64_AND, false }, { 60, A64_BIC, false }, { 444, A64_ORR, false }, { 412, A64_ORN, false },
            { 316, A64_EOR, false }, { 476, A64_AND, true }, { 124, A64_ORR, true }, { 284, A64_EON, false },
        };
        int i = 0;
        while (L[i].x != (int)XO10(op))
            i++;
        ldg(t, 1, d);
        if (rb == d && (XO10(op) == 444 || XO10(op) == 28)) { /* mr */
            a64_mov(b, false, 0, 1);
        } else {
            ldg(t, 2, rb);
            a64_log(b, L[i].kind, 0, 1, 2);
        }
        if (L[i].invert)
            a64_log(b, A64_ORN, 0, A64_ZR, 0);
        break;
    }
    case 954: ldg(t, 1, d); a64_sxtb(b, 0, 1); break; /* extsb */
    case 922: ldg(t, 1, d); a64_sxth(b, 0, 1); break; /* extsh */
    case 26: ldg(t, 1, d); a64_clz(b, 0, 1); break;   /* cntlzw */
    case 24: case 536: /* slw srw: by 0-63, 32 and up giving 0 */
        ldg(t, 1, d);
        ldg(t, 2, rb);
        a64_ubfx(b, 9, 2, 0, 6);
        a64_shiftv(b, true, XO10(op) == 24 ? 0 : 1, 0, 1, 9);
        break;
    case 792: /* sraw */
        ldg(t, 1, d);
        ldg(t, 2, rb);
        a64_ubfx(b, 9, 2, 0, 6);
        a64_sxtw(b, 10, 1);
        a64_shiftv(b, true, 2, 0, 10, 9);
        a64_shiftv(b, true, 0, 11, 0, 9);
        a64_addsub(b, A64_SUBS, true, A64_ZR, 11, 10, 0, 0);
        a64_cset(b, 12, A64_NE);
        a64_logic(b, A64_AND, false, 12, 12, 1, 1, 31);
        ldf(t, 9, FIELD(xer));
        a64_bfi(b, 9, 12, 29, 1);
        stf(t, 9, FIELD(xer));
        stg(t, 0, a);
        if (RCBIT(op))
            record(t);
        return true;
    case 824: { /* srawi */
        int n = rb;
        ldg(t, 1, d);
        if (n == 0) {
            a64_mov(b, false, 0, 1);
            a64_mov(b, false, 12, A64_ZR);
        } else {
            a64_asr(b, 0, 1, n);
            a64_lsl(b, 9, 1, 32 - n);
            a64_logic(b, A64_AND, false, 9, 9, 1, 2, 31);
            a64_cmp_imm(b, 9, 0);
            a64_cset(b, 12, A64_NE);
        }
        ldf(t, 9, FIELD(xer));
        a64_bfi(b, 9, 12, 29, 1);
        stf(t, 9, FIELD(xer));
        stg(t, 0, a);
        if (RCBIT(op))
            record(t);
        return true;
    }
    case 982: /* icbi */
        ldg0(t, 1, a);
        ldg(t, 2, rb);
        a64_add(b, 0, 1, 2);
        jit_call(&t->b, H_ICBI);
        return true;
    case 54: case 86: case 246: case 278: case 470: /* data cache hints */
    case 598: case 854: case 306: case 370: case 566: case 146: case 210: case 242: /* sync and supervisor no-ops */
        t->keeps_flags = true;
        return true;
    default:
        return false;
    }
    /* The logical group's result. */
    stg(t, 0, a);
    if (RCBIT(op))
        record(t);
    else
        t->keeps_flags = true;
    return true;
}

/* The block's last instruction, a branch: its exits. */
static void emit_branch(tctx *t, uint32_t op, uint32_t pc)
{
    a64_buf *b = &t->b;
    uint32_t next = pc + 4;
    uint32_t not_taken[3];
    switch (OPCD(op)) {
    case 18: { /* b */
        int32_t li = (int32_t)(op << 6) >> 6 & ~3;
        uint32_t target = (op & 2) ? (uint32_t)li : pc + (uint32_t)li;
        if (op & 1) {
            a64_mov32(b, 0, next);
            stf(t, 0, FIELD(lr));
        }
        exit_to(t, target);
        return;
    }
    case 16: { /* bc */
        uint32_t target = (uint32_t)(int32_t)(int16_t)(op & 0xFFFC);
        if (!(op & 2))
            target += pc;
        if (op & 1) {
            a64_mov32(b, 0, next);
            stf(t, 0, FIELD(lr));
        }
        cond_branch(t, op, not_taken);
        exit_to(t, target);
        for (int i = 0; not_taken[i] != UINT32_MAX; i++)
            a64_patch_here(b, not_taken[i]);
        if (not_taken[0] != UINT32_MAX)
            exit_to(t, next);
        return;
    }
    default: { /* bclr, bcctr */
        bool to_lr = XO10(op) == 16;
        ldf(t, 12, to_lr ? FIELD(lr) : FIELD(ctr));
        a64_lsr(b, 12, 12, 2);
        a64_lsl(b, 12, 12, 2);
        if (to_lr) {
            cond_branch(t, op, not_taken);
        } else {
            uint32_t no_ctr = op | (4u << 21); /* bcctr doesn't count */
            cond_branch(t, no_ctr, not_taken);
        }
        /* LR is set either way: before the paths split, after the test
         * (which may have read the old LR's target into w12 already). */
        if (op & 1) {
            /* The tests branch away before this point; the not-taken path
             * sets LR itself below. */
            a64_mov32(b, 0, next);
            stf(t, 0, FIELD(lr));
        }
        exit_indirect(t);
        if (not_taken[0] != UINT32_MAX) {
            for (int i = 0; not_taken[i] != UINT32_MAX; i++)
                a64_patch_here(b, not_taken[i]);
            if (op & 1) {
                a64_mov32(b, 0, next);
                stf(t, 0, FIELD(lr));
            }
            exit_to(t, next);
        }
        return;
    }
    }
}

/* ------------------------------------------------------------------ */
/* Blocks */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static uint64_t build_id(void)
{
    char id[256];
    snprintf(id, sizeof id, "ppc %d %s %s %zu %zu %zu", TRANSLATOR_VERSION, __DATE__, __TIME__, sizeof(ppc_cpu),
             sizeof(jit_ctx), offsetof(ppc_cpu, cr));
    return jit_hash(id, strlen(id), 0);
}

/* Translate the block at pc (or take it from the cache).  A cache record
 * holds the host code followed by one word: the offset of the block's dead
 * path, where kill() points its entry. */
static jit_block *translate(uint32_t pc)
{
    uint64_t t0 = now_ns();

    /* How far the block goes. */
    uint32_t ops[MAX_INSN];
    enum kind kinds[MAX_INSN];
    uint32_t n = 0;
    bool ends_in_branch = false;
    while (n < MAX_INSN) {
        uint32_t op = rd32(pc + 4 * n);
        enum kind k = classify(op);
        if (k == K_STOP)
            break;
        ops[n] = op;
        kinds[n] = k;
        n++;
        if (k == K_BRANCH) {
            ends_in_branch = true;
            break;
        }
    }
    if (n == 0)
        return jit_marker(&E, pc, 4);

    const jit_cache_rec *r = jit_cached(&E, pc, 0);
    if (r) {
        uint32_t nwords = r->code_len / 4 - 1, dead_at;
        memcpy(&dead_at, r->code + 4 * nwords, 4);
        jit_block *b = jit_install(&E, pc, r->ninsn, r->guest_len, (const uint32_t *)(const void *)r->code, nwords,
                                   dead_at);
        if (b) {
            st.from_cache++;
            st.translate_ns += now_ns() - t0;
            return b;
        }
    }

    tctx t = { 0 };
    t.b.p = scratch;
    t.b.cap = 16384 - 1;
    t.pc = pc;
    t.ninsn = n;
    t.flags_crf = -1;

    /* CR fields written and never read before the block ends or writes
     * them again. */
    uint8_t live = 0xFF;
    for (int i = (int)n - 1; i >= 0; i--) {
        uint8_t rd, wr;
        cr_use(ops[i], kinds[i], &rd, &wr);
        uint8_t dead_fields = (uint8_t)(wr & ~live);
        uint8_t m = 0;
        for (int f = 0; f < 8; f++)
            if (dead_fields & (0x80 >> f))
                m |= (uint8_t)(1 << f);
        t.cr_dead[i] = m;
        live = (uint8_t)((live & ~wr) | rd);
    }

    /* The GPRs used most in the block's translated instructions live in
     * x24-x28 while it runs. */
    int uses[32] = { 0 };
    for (uint32_t i = 0; i < n; i++)
        if (kinds[i] == K_NATIVE) {
            uses[RD(ops[i])]++;
            uses[RA(ops[i])]++;
            uses[RB(ops[i])]++;
        }
    memset(t.host, -1, sizeof t.host);
    for (int h = 24; h <= 28; h++) {
        int best = -1;
        for (int r = 0; r < 32; r++)
            if (t.host[r] < 0 && uses[r] >= 2 && (best < 0 || uses[r] > uses[best]))
                best = r;
        if (best < 0)
            break;
        t.host[best] = (int8_t)h;
    }

    a64_buf *b = &t.b;
    a64_addsub_imm(b, A64_SUBSI, true, HB, HB, n);
    uint32_t to_over = a64_here(b);
    a64_bcond(b, A64_LT, 0);
    cache_load(&t);

    for (uint32_t i = 0; i < n; i++) {
        uint32_t op = ops[i], ipc = pc + 4 * i;
        t.at = (int)i;
        t.last_pc = ipc;
        if (kinds[i] == K_BRANCH) {
            cache_flush(&t);
            emit_branch(&t, op, ipc);
            break;
        }
        t.stored = false;
        bool ok = kinds[i] == K_NATIVE && emit(&t, op, ipc);
        if (!ok) {
            cache_flush(&t);
            execute(&t, op);
            cache_load(&t);
            t.keeps_flags = false;
            t.sets_flags = -1;
            t.stored = exec_stores(op);
        }
        if (t.stored && i + 1 < n) {
            /* A store into translated code: leave after this instruction,
             * the dispatcher translating what follows afresh. */
            uint32_t skip = jit_killed_check(b);
            cache_flush_exit(&t);
            a64_addsub_imm(b, A64_ADDI, true, HB, HB, n - (i + 1));
            uint32_t ninsn = t.ninsn;
            t.ninsn = i + 1;
            exit_pcs(&t);
            t.ninsn = ninsn;
            a64_mov32(b, 0, ipc + 4);
            stf(&t, 0, FIELD(pc));
            exit_plain(&t);
            a64_patch_here(b, skip);
            t.flags_crf = -1;
            t.keeps_flags = false;
            t.sets_flags = -1;
        }
        if (t.sets_flags >= 0)
            t.flags_crf = t.sets_flags;
        else if (!t.keeps_flags)
            t.flags_crf = -1;
    }
    if (!ends_in_branch) {
        cache_flush(&t);
        t.last_pc = pc + 4 * (n - 1);
        exit_to(&t, pc + 4 * n);
    }
    a64_patch_here(b, to_over);
    a64_addsub_imm(b, A64_ADDI, true, HB, HB, n);
    uint32_t dead_at = a64_here(b);
    a64_mov32(b, 0, pc);
    stf(&t, 0, FIELD(pc));
    exit_plain(&t);
    if (b->full)
        return jit_marker(&E, pc, 4); /* too big: leave it to the interpreter */

    jit_block *blk = jit_install(&E, pc, n, 4 * n, b->p, b->n, dead_at);
    if (blk) {
        st.translated++;
        jit_remember(&E, pc, n, 4 * n, 0, b->p, b->n, dead_at);
    }
    st.translate_ns += now_ns() - t0;
    return blk;
}

static bool setup(void)
{
    if (E.ready)
        return true;
    scratch = malloc(16384 * 4);
    if (!scratch || !jit_engine_init(&E, CODE_PPC, 2, CODE_SIZE))
        return false;
    E.build = build_id();
    E.ctx.helper[H_EXECUTE] = (void *)ppc_execute;
    E.ctx.helper[H_ST8] = (void *)st8;
    E.ctx.helper[H_ST16] = (void *)st16;
    E.ctx.helper[H_ST32] = (void *)st32;
    E.ctx.helper[H_ICBI] = (void *)do_icbi;
    return true;
}

bool ppc_jit_available(void) { return true; }

void ppc_jit_enable(bool on, const char *cache_dir, const char *cache_name)
{
    ppc_jit_enabled = on;
    jit_engine_cache(&E, cache_dir, cache_name);
}

uint64_t ppc_jit_run(ppc_cpu *c, uint64_t max)
{
    uint64_t start = c->icount;
    if (!E.ready && !setup()) {
        ppc_jit_enabled = false;
        while (!c->halted && c->icount - start < max)
            ppc_step(c);
        return c->icount - start;
    }
    E.ctx.mem = g_mem;
    while (!c->halted) {
        uint64_t done = c->icount - start;
        if (done >= max)
            break;
        uint64_t left = max - done;
        jit_block *b = jit_lookup(&E, c->pc);
        if (!b)
            b = translate(c->pc);
        if (!b || !b->ninsn || b->ninsn > left) {
            ppc_step(c);
            st.interpreted++;
            continue;
        }
        E.ctx.watch_lo = g_watch_lo;
        E.ctx.watch_len = g_watch_len;
        E.ctx.last_stub = NULL;
        E.ctx.killed = 0;
        uint64_t ran = left - jit_enter(&E, c, (int64_t)left, b->entry);
        c->icount += ran;
        c->tb += ran;
        st.jitted += ran;
        st.blocks++;
        /* Chain the exit just taken to its successor. */
        if (E.ctx.last_stub) {
            uint64_t gen = E.generation;
            jit_block *n = jit_lookup(&E, c->pc);
            if (!n)
                n = translate(c->pc);
            jit_chain(&E, n, gen);
        }
    }
    return c->icount - start;
}

void ppc_jit_get_stats(ppc_jit_stats *s)
{
    *s = st;
    s->invalidated = E.invalidated;
    s->flushes = E.flushes;
    s->chained = E.chained;
}

/* ------------------------------------------------------------------ */
/* Differential self-test */

static uint32_t rnd(uint32_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

/* A random instruction of the kinds translated, writing none of r29-r31
 * (the scratch area's bases and index), its memory operands inside the
 * scratch area. */
static uint32_t random_op(uint32_t *s)
{
    for (;;) {
        uint32_t r = rnd(s);
        uint32_t d = rnd(s) % 29, a = rnd(s) % 32, b = rnd(s) % 32, imm = rnd(s) & 0xFFFF;
        uint32_t rc = rnd(s) & 1;
        uint32_t crf = (rnd(s) % 8) << 2;
        if (rnd(s) % 4 == 0)
            imm = (uint32_t)(int32_t)(int16_t)(rnd(s) % 64 - 32) & 0xFFFF;
        switch (r % 24) {
        case 0: { static const uint32_t o[] = { 7, 8, 12, 13, 14, 15 };
            return o[rnd(s) % 6] << 26 | d << 21 | a << 16 | imm; }
        case 1: return (rnd(s) & 1 ? 10u : 11u) << 26 | crf << 21 | a << 16 | imm;
        case 2: { static const uint32_t o[] = { 24, 25, 26, 27, 28, 29 };
            return o[rnd(s) % 6] << 26 | a << 21 | (d) << 16 | imm; }
        case 3: case 4: { /* rotates */
            uint32_t o = (uint32_t[]){ 20, 21, 23 }[rnd(s) % 3];
            return o << 26 | a << 21 | d << 16 | b << 11 | (rnd(s) % 32) << 6 | (rnd(s) % 32) << 1 | rc;
        }
        case 5: case 6: case 7: { /* arithmetic */
            static const uint32_t x[] = { 266, 10, 138, 234, 202, 40, 8, 136, 232, 200, 104, 235, 75, 11, 459, 491 };
            uint32_t oe = rnd(s) % 8 == 0;
            return 31u << 26 | d << 21 | a << 16 | b << 11 | oe << 10 | x[rnd(s) % 16] << 1 | rc;
        }
        case 8: case 9: { /* logical, shifts */
            static const uint32_t x[] = { 28, 60, 444, 412, 316, 476, 124, 284, 954, 922, 26, 24, 536, 792, 824 };
            return 31u << 26 | a << 21 | d << 16 | b << 11 | x[rnd(s) % 15] << 1 | rc;
        }
        case 10: /* cmp, cmpl */
            return 31u << 26 | crf << 21 | a << 16 | b << 11 | (rnd(s) & 1 ? 32u : 0u) << 1;
        case 11: { /* mfcr, mtcrf, mfspr/mtspr xer lr ctr */
            uint32_t spr = (uint32_t[]){ 1, 8, 9 }[rnd(s) % 3];
            uint32_t sprf = (spr & 31) << 16 | (spr >> 5) << 11;
            switch (rnd(s) % 4) {
            case 0: return 31u << 26 | d << 21 | 19u << 1;
            case 1: return 31u << 26 | d << 21 | (rnd(s) & 0xFF) << 12 | 144u << 1;
            case 2: return 31u << 26 | d << 21 | sprf | 339u << 1;
            default: return 31u << 26 | d << 21 | sprf | 467u << 1;
            }
        }
        case 12: case 13: case 14: { /* D-form loads and stores off r31 */
            static const uint32_t o[] = { 32, 34, 40, 42, 36, 38, 44, 48, 50, 52, 54 };
            uint32_t oo = o[rnd(s) % 11];
            return oo << 26 | (oo >= 48 ? rnd(s) % 32 : d) << 21 | 31u << 16 | (rnd(s) & 0x7FF8);
        }
        case 15: case 16: { /* X-form loads and stores, r31 + r30 */
            static const uint32_t x[] = { 23, 87, 279, 343, 151, 215, 407, 534, 790, 662, 918 };
            return 31u << 26 | d << 21 | 31u << 16 | 30u << 11 | x[rnd(s) % 11] << 1;
        }
        case 17: { /* update forms off r29, kept pointing in the area below */
            static const uint32_t o[] = { 33, 35, 41, 43, 37, 39, 45 };
            uint32_t dd = rnd(s) % 28;
            return o[rnd(s) % 7] << 26 | dd << 21 | 29u << 16 | ((rnd(s) % 64) * 4);
        }
        case 18: { /* floating point through the interpreter */
            static const uint32_t x5[] = { 18, 20, 21, 25, 29, 28 };
            uint32_t o = rnd(s) & 1 ? 59u : 63u;
            return o << 26 | (rnd(s) % 32) << 21 | (rnd(s) % 32) << 16 | (rnd(s) % 32) << 11 | (rnd(s) % 32) << 6 |
                   x5[rnd(s) % 6] << 1 | rc;
        }
        case 19: /* cr logic through the interpreter */
            return 19u << 26 | (rnd(s) % 32) << 21 | (rnd(s) % 32) << 16 | (rnd(s) % 32) << 11 |
                   (uint32_t[]){ 33, 129, 193, 225, 257, 289, 417, 449 }[rnd(s) % 8] << 1;
        case 20: /* srawi with small shifts too */
            return 31u << 26 | a << 21 | d << 16 | (rnd(s) % 32) << 11 | 824u << 1 | rc;
        case 21: /* stmw off r31 */
            return 47u << 26 | (rnd(s) % 32) << 21 | 31u << 16 | (rnd(s) & 0xFF0);
        default:
            continue;
        }
    }
}

int ppc_jit_selftest(uint32_t seed, int blocks, uint32_t code_at, uint32_t scratch, char *why, int whylen)
{
    uint32_t s = seed ? seed : 1;
    int bad = 0;
    why[0] = 0;
    enum { AREA = 0x10000 };
    uint8_t *before = malloc(AREA), *after_i = malloc(AREA);
    if (!before || !after_i) {
        free(before);
        free(after_i);
        return -1;
    }
    bool was = ppc_jit_enabled;
    for (int iter = 0; iter < blocks; iter++) {
        int n = 1 + (int)(rnd(&s) % 40);
        uint32_t code[48];
        for (int i = 0; i < n; i++)
            code[i] = random_op(&s);
        /* A conditional branch on a random CR bit (or the counter) to
         * either of two addresses holding a NATIVE opcode. */
        uint32_t bo = (uint32_t[]){ 12, 4, 16, 18, 20, 8, 0 }[rnd(&s) % 7];
        uint32_t bi = rnd(&s) % 32;
        uint32_t end = code_at + 4 * (uint32_t)n;
        code[n] = 16u << 26 | bo << 21 | bi << 16 | 8u; /* bc to end + 8 */
        if (rnd(&s) % 4 == 0) /* or blr / bctr */
            code[n] = 19u << 26 | (bo | 0) << 21 | bi << 16 | ((rnd(&s) & 1) ? 16u : 528u) << 1;
        if ((code[n] >> 26) == 19 && ((code[n] >> 1) & 0x3FF) == 528)
            code[n] |= 4u << 21; /* bcctr with a counter test is invalid */
        for (int i = 0; i <= n; i++)
            wr32(code_at + 4 * (uint32_t)i, code[i]);
        wr32(end + 4, PPC_NATIVE_OP);
        wr32(end + 8, PPC_NATIVE_OP);

        ppc_cpu start;
        ppc_init(&start);
        for (int i = 0; i < 32; i++) {
            start.gpr[i] = rnd(&s);
            if (rnd(&s) % 4 == 0)
                start.gpr[i] = rnd(&s) % 8;
            start.fpr[i] = (uint64_t)rnd(&s) << 32 | rnd(&s);
            if (rnd(&s) % 2) {
                double v = (double)(int32_t)rnd(&s) / 64.0;
                memcpy(&start.fpr[i], &v, 8);
            }
        }
        start.gpr[31] = scratch + 0x100;
        start.gpr[30] = rnd(&s) % 0x8000;
        start.gpr[29] = scratch + 0x8000;
        start.cr = rnd(&s);
        start.xer = rnd(&s) & 0xE000007Fu;
        start.ctr = rnd(&s) % 3;
        start.lr = rnd(&s) % 2 ? end + 4 : end + 8;
        start.ctr = rnd(&s) % 2 ? start.ctr : (rnd(&s) % 2 ? end + 4 : end + 8);
        start.pc = code_at;
        start.op_pc = code_at - 4;
        for (uint32_t i = 0; i < AREA; i++)
            before[i] = (uint8_t)rnd(&s);

        /* The interpreter. */
        gput(scratch, before, AREA);
        ppc_cpu ci = start;
        ppc_jit_enabled = false;
        ppc_run(&ci, (uint64_t)n + 1);
        memcpy(after_i, g_mem + scratch, AREA);

        /* The JIT, from the same state. */
        gput(scratch, before, AREA);
        ppc_cpu cj = start;
        ppc_jit_enabled = true;
        ppc_run(&cj, (uint64_t)n + 1);

        const char *what = NULL;
        int which = -1;
        for (int i = 0; i < 32 && !what; i++)
            if (ci.gpr[i] != cj.gpr[i]) {
                what = "gpr";
                which = i;
            }
        for (int i = 0; i < 32 && !what; i++)
            if (ci.fpr[i] != cj.fpr[i]) {
                what = "fpr";
                which = i;
            }
        if (!what) {
            if (ci.cr != cj.cr) what = "cr";
            else if (ci.xer != cj.xer) what = "xer";
            else if (ci.lr != cj.lr) what = "lr";
            else if (ci.ctr != cj.ctr) what = "ctr";
            else if (ci.pc != cj.pc) what = "pc";
            else if (ci.fpscr != cj.fpscr) what = "fpscr";
            else if (ci.icount != cj.icount) what = "icount";
            else if (ci.tb != cj.tb) what = "tb";
            else if (ci.op_pc != cj.op_pc) what = "op_pc";
            else if (ci.prev_pc != cj.prev_pc) what = "prev_pc";
            else if (ci.halted != cj.halted) what = "halted";
            else if (memcmp(after_i, g_mem + scratch, AREA)) what = "memory";
        }
        if (what) {
            if (!bad) {
                int at = snprintf(why, (size_t)whylen, "block %d (%d ops): %s", iter, n + 1, what);
                if (which >= 0)
                    at += snprintf(why + at, (size_t)(whylen - at), "%d %08llX vs %08llX", which,
                                   (unsigned long long)(what[0] == 'g' ? ci.gpr[which] : ci.fpr[which]),
                                   (unsigned long long)(what[0] == 'g' ? cj.gpr[which] : cj.fpr[which]));
                else if (!strcmp(what, "cr"))
                    at += snprintf(why + at, (size_t)(whylen - at), " %08X vs %08X", ci.cr, cj.cr);
                else if (!strcmp(what, "xer"))
                    at += snprintf(why + at, (size_t)(whylen - at), " %08X vs %08X", ci.xer, cj.xer);
                for (int i = 0; i <= n && at < whylen - 10; i++)
                    at += snprintf(why + at, (size_t)(whylen - at), " %08X", code[i]);
            }
            bad++;
        }
    }
    ppc_jit_enabled = was;
    free(before);
    free(after_i);
    return bad;
}

#endif
