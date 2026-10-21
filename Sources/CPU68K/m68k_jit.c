/*
 * 68k JIT: basic blocks of 68020 code translated to arm64, on the block
 * engine the PowerPC JIT uses (jit.h).  See m68k.h.
 *
 * A block runs from an address to its first change of flow (Bcc, BRA,
 * BSR, DBcc, JMP, JSR, RTS, RTD), at most 64 instructions.  Translated:
 * MOVE, MOVEA, MOVEQ, MOVEM, LEA, PEA, CLR, NEG, NOT, TST, EXT, EXTB,
 * SWAP, EXG, ADD, ADDA, ADDI, ADDQ, SUB, SUBA, SUBI, SUBQ, CMP, CMPA,
 * CMPI, AND, ANDI, OR, ORI, EOR, EORI, MULU.W, MULS.W, LSL, LSR and ASR by
 * an immediate count, Scc, LINK, UNLK and NOP, in every addressing mode
 * but the 68020's memory-indirect and full-format indexed ones.  Other
 * instructions whose length the translator can tell (bit operations,
 * shifts and rotates by a register, ASL, ROXd, ADDX, SUBX, NEGX, CMPM,
 * MOVE to and from CCR and SR, MULL, DIVL, DIVU, DIVS, CHK, BCD, bit
 * fields, TAS ...) run inside the block through the interpreter's
 * cpu_execute_at; when one takes an exception the block leaves at once.
 * A-line traps, the NATIVE opcode, TRAP, RTE, STOP and the floating point
 * unit end the block, and the interpreter runs them.
 *
 * Guest registers stay in the cpu68k structure (D0-D7 then A0-A7, which
 * the translator numbers 0-15); the five a block names most are kept in
 * x24-x28 while it runs.  Memory goes through the arena with the address
 * mask the block was translated for, stores through the screen watch and
 * the code map as the interpreter's do (pushes, as cpu_push32, unwatched).
 *
 * Condition codes are not computed where nothing reads them.  A pass
 * over the block finds, for each instruction, which of X N Z V C are read
 * after it before something sets them again; after the block, those its
 * successors may read before setting them (looked for along up to 16
 * instructions from each known successor; everything after an indirect
 * jump, a trap or an instruction the JIT doesn't know; X always).  The
 * block then depends on the code looked at, and goes when it is written.
 * An instruction whose flags nothing reads sets none.  A Bcc, DBcc or Scc
 * whose flags were set by an instruction in the block, the host flags
 * untouched since, branches on the host's (set from operands shifted to
 * the top of the register, so byte and word results flag as the 68k
 * does), and they are stored in SR only if read again later.  So between
 * blocks, SR's N Z V C may be stale where the code that follows sets them
 * before reading them, and op_pc and op name an earlier instruction when
 * a run stops at a block's entry after a chained exit; neither is read
 * there (traps and exceptions run in the interpreter, which sets them).
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "jit.h"
#include "m68k.h"

bool cpu_jit_enabled;

#if !(defined(__aarch64__) || defined(__arm64__))

bool cpu_jit_available(void) { return false; }
void cpu_jit_enable(bool on, const char *cache_dir, const char *cache_name) { cpu_jit_enabled = false; }
uint64_t cpu_jit_run(cpu68k *c, uint64_t max)
{
    uint64_t start = c->icount;
    while (!c->halted && !c->stopped && c->icount - start < max)
        cpu_step(c);
    return c->icount - start;
}
void cpu_jit_step1(cpu68k *c) { cpu_step(c); }
void cpu_jit_flush(void) {}
void cpu_jit_get_stats(cpu_jit_stats *s) { memset(s, 0, sizeof *s); }
int cpu_jit_selftest(uint32_t seed, int blocks, uint32_t code_at, uint32_t scratch, char *why, int whylen)
{
    snprintf(why, (size_t)whylen, "no JIT on this host");
    return -1;
}

#else

#include "jit_a64.h"

enum { HC = JR_CPU, HB = JR_BUDGET };
enum { MAX_INSN = 64, MAX_BYTES = 512, CODE_SIZE = 64 << 20 };
#define TRANSLATOR_VERSION 2

enum { H_ST8, H_ST16, H_ST32, H_PST8, H_PST16, H_PST32, H_EXEC };

/* Flags, as in the CCR. */
enum { FX = 0x10, FN = 8, FZ = 4, FV = 2, FC = 1, FNZVC = 0xF, FALL = 0x1F };

/* What the host flags hold. */
enum fkind { FK_NONE, FK_SUB, FK_ADD, FK_LOGIC };

static jit_engine E;
static uint32_t *scratch;
static cpu_jit_stats st;
static uint32_t cur_mask = 0xFFFFFFFFu;
static int cur_model = 68020;
static int max_insn = MAX_INSN; /* 1 for single-instruction tests */

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
static void pst8(uint32_t a, uint32_t v) { wr8(a, (uint8_t)v); }
static void pst16(uint32_t a, uint32_t v) { wr16(a, (uint16_t)v); }
static void pst32(uint32_t a, uint32_t v) { wr32(a, v); }

void cpu_jit_flush(void)
{
    if (E.ready)
        jit_flush(&E);
}

/* ------------------------------------------------------------------ */
/* Translation state */

#define REG(n) ((uint32_t)offsetof(cpu68k, d) + 4u * (uint32_t)(n)) /* 0-7 D, 8-15 A */
#define FIELD(f) ((uint32_t)offsetof(cpu68k, f))

typedef struct tctx {
    a64_buf b;
    bool scanning;           /* first pass: lengths, flags, register use */
    uint32_t pc0;            /* block start */
    uint32_t ipc;            /* the instruction being translated */
    uint16_t iop;
    uint32_t at;             /* where its next extension word is */
    uint32_t mask;           /* address mask */
    int model;
    uint32_t ninsn;
    uint8_t reads, writes;   /* the instruction's flags (scan) */
    bool ends;               /* it ends the block (scan) */
    uint8_t live;            /* flags read from SR after the instruction (emit) */
    bool host_need;          /* a later instruction branches on its host flags */
    enum fkind fk;           /* what the host flags hold */
    /* Scan: what the instruction did to the host flags, the condition it
     * tested, where it can go. */
    enum fkind setkind;
    bool clobber;            /* host flags not its flags at its end */
    int cc;
    uint32_t succ[2];
    int nsucc;
    bool indirect;
    bool stored;             /* the instruction stored */
    uint32_t last_pc;        /* for exits */
    uint16_t last_op;
    int uses[16];
    int8_t host[16];
    bool dirty[16];
} tctx;

static uint16_t ext16(tctx *t)
{
    uint16_t v = rd16(t->at & t->mask);
    t->at += 2;
    return v;
}
static uint32_t ext32(tctx *t)
{
    uint32_t v = rd32(t->at & t->mask);
    t->at += 4;
    return v;
}

/* Guest registers (through the cache). */
static void getr(tctx *t, int w, int r)
{
    t->uses[r]++;
    if (t->host[r] >= 0)
        a64_mov(&t->b, false, w, t->host[r]);
    else
        a64_ldst(&t->b, A64_LDRW, w, HC, REG(r));
}
static void setr(tctx *t, int w, int r)
{
    t->uses[r]++;
    if (t->host[r] >= 0) {
        a64_mov(&t->b, false, t->host[r], w);
        t->dirty[r] = true;
    } else {
        a64_ldst(&t->b, A64_STRW, w, HC, REG(r));
    }
}
/* The low sz bytes of register r from w (w8 used). */
static void setr_sz(tctx *t, int w, int r, int sz)
{
    if (sz == 4) {
        setr(t, w, r);
        return;
    }
    getr(t, 8, r);
    a64_bfi(&t->b, 8, w, 0, 8 * sz);
    setr(t, 8, r);
}
static void cache_flush(tctx *t)
{
    for (int r = 0; r < 16; r++)
        if (t->host[r] >= 0 && t->dirty[r]) {
            a64_ldst(&t->b, A64_STRW, t->host[r], HC, REG(r));
            t->dirty[r] = false;
        }
}
static void cache_flush_exit(tctx *t)
{
    for (int r = 0; r < 16; r++)
        if (t->host[r] >= 0 && t->dirty[r])
            a64_ldst(&t->b, A64_STRW, t->host[r], HC, REG(r));
}
static void cache_load(tctx *t)
{
    for (int r = 0; r < 16; r++)
        if (t->host[r] >= 0)
            a64_ldst(&t->b, A64_LDRW, t->host[r], HC, REG(r));
}

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

/* w1 &= the address mask (24-bit mode). */
static void mask_addr(tctx *t)
{
    if (t->mask != 0xFFFFFFFFu) {
        a64_mov32(&t->b, 9, t->mask);
        a64_log(&t->b, A64_AND, 1, 1, 9);
    }
}
/* w0 = the sz bytes at w1 (zero-extended). */
static void load(tctx *t, int sz)
{
    mask_addr(t);
    jit_load(&t->b, sz, false, false);
}
/* The sz bytes of w2 to w1 (flags and x0-x18 lost). */
static void store(tctx *t, int sz, bool watched)
{
    mask_addr(t);
    jit_store(&t->b, sz, watched, watched ? H_ST8 : H_PST8);
    t->fk = FK_NONE;
    t->stored = true;
    t->clobber = true;
}

/* wd = ws shifted so its sz bytes are at the top. */
static void top(tctx *t, int wd, int ws, int sz)
{
    if (sz == 4)
        a64_mov(&t->b, false, wd, ws);
    else
        a64_lsl(&t->b, wd, ws, 32 - 8 * sz);
}

/* ------------------------------------------------------------------ */
/* Flags */

static bool flags_needed(tctx *t, uint8_t writes)
{
    t->writes |= writes;
    if (t->scanning || (writes & t->live) || t->host_need)
        return true;
    t->fk = FK_NONE; /* the 68k's flags change; nothing reads them */
    return false;
}

/* Store the flags in `writes` from the host flags (of kind k) to SR; C
 * from host register creg instead when creg >= 0. */
static void flags_store(tctx *t, enum fkind k, uint8_t writes, int creg)
{
    a64_buf *b = &t->b;
    t->fk = k;
    t->setkind = k;
    t->clobber = false;
    if (!(writes & t->live) && !t->scanning)
        return;
    /* w10 = N Z V C (and X), each from the host flags. */
    a64_cset(b, 10, A64_MI);
    a64_cset(b, 11, A64_EQ);
    a64_logic(b, A64_ORR, false, 10, 11, 10, 0, 1);
    if (k == FK_LOGIC) {
        a64_lsl(b, 10, 10, 2);
    } else {
        a64_cset(b, 11, A64_VS);
        a64_logic(b, A64_ORR, false, 10, 11, 10, 0, 1);
        if (creg >= 0)
            a64_mov(b, false, 11, creg);
        else
            a64_cset(b, 11, k == FK_SUB ? A64_LO : A64_HS);
        a64_logic(b, A64_ORR, false, 10, 11, 10, 0, 1);
        if (writes & FX)
            a64_logic(b, A64_ORR, false, 10, 10, 11, 0, 4);
    }
    a64_ldst(b, A64_LDRH, 12, HC, FIELD(sr));
    a64_bfi(b, 12, 10, 0, (writes & FX) ? 5 : 4);
    a64_ldst(b, A64_STRH, 12, HC, FIELD(sr));
}

/* Logic flags (N Z from the sz-byte value in w, V = C = 0). */
static void flags_logic(tctx *t, int w, int sz)
{
    if (!flags_needed(t, FNZVC))
        return;
    top(t, 13, w, sz);
    a64_logic(&t->b, A64_ANDS, false, A64_ZR, 13, 13, 0, 0);
    flags_store(t, FK_LOGIC, FNZVC, -1);
}

/* Arm64 conditions for 68k condition cc given the host flags' kind, or -1
 * if they can't say it; A64_AL for always, 16 for never. */
enum { NEVER = 16 };
static int host_cond(enum fkind k, int cc)
{
    static const int same[16] = { A64_AL, NEVER, A64_HI, A64_LS, A64_HS, A64_LO, A64_NE, A64_EQ,
                                  A64_VC, A64_VS, A64_PL, A64_MI, A64_GE, A64_LT, A64_GT, A64_LE };
    if (cc < 2)
        return same[cc];
    switch (k) {
    case FK_SUB:
        return same[cc];
    case FK_ADD:
        if (cc == 2 || cc == 3)
            return -1;
        if (cc == 4)
            return A64_LO;
        if (cc == 5)
            return A64_HS;
        return same[cc];
    case FK_LOGIC:
        switch (cc) {
        case 2: return A64_NE;
        case 3: return A64_EQ;
        case 4: case 8: return A64_AL;
        case 5: case 9: return NEVER;
        default: return same[cc];
        }
    default:
        return -1;
    }
}

static uint8_t cc_reads(int cc)
{
    static const uint8_t r[16] = { 0, 0, FC | FZ, FC | FZ, FC, FC, FZ, FZ, FV, FV, FN, FN,
                                   FN | FV, FN | FV, FN | FV | FZ, FN | FV | FZ };
    return r[cc];
}

/* The host flags set for condition cc; returns the arm64 condition. */
static int cond_flags(tctx *t, int cc)
{
    t->reads |= cc_reads(cc);
    t->cc = cc;
    int h = host_cond(t->fk, cc);
    if (h >= 0)
        return h;
    /* From SR: N Z (V) and C inverted into NZCV, as after a subtract. */
    a64_buf *b = &t->b;
    a64_ldst(b, A64_LDRH, 9, HC, FIELD(sr));
    a64_ubfx(b, 10, 9, 2, 2);
    a64_lsl(b, 10, 10, 30);
    a64_ubfx(b, 11, 9, 1, 1);
    a64_logic(b, A64_ORR, false, 10, 10, 11, 0, 28);
    a64_ubfx(b, 11, 9, 0, 1);
    a64_movz(b, false, 12, 1, 0);
    a64_sub(b, 11, 12, 11);
    a64_logic(b, A64_ORR, false, 10, 10, 11, 0, 29);
    a64_emit(b, 0xD51B4200u | 10); /* msr nzcv, x10 */
    t->fk = FK_SUB;
    return host_cond(FK_SUB, cc);
}

/* ------------------------------------------------------------------ */
/* Effective addresses */

enum { EA_D, EA_A, EA_M, EA_I };
typedef struct {
    int kind;
    int reg;
    uint32_t imm;
    bool constant;           /* EA_M: the address is known: addr */
    uint32_t addr;
} jea;

/* Index from a brief extension word into w9 (added to w1). */
static bool index_brief(tctx *t, uint16_t ext)
{
    if ((ext & 0x0100) && t->model != 68000)
        return false; /* full format: the interpreter */
    int xr = (ext >> 12) & 7;
    getr(t, 9, (ext & 0x8000) ? 8 + xr : xr);
    if (!(ext & 0x0800))
        a64_sxth(&t->b, 9, 9);
    int scale = t->model == 68000 ? 0 : (ext >> 9) & 3;
    a64_addsub(&t->b, A64_ADD, false, 1, 1, 9, 0, scale);
    add_imm(t, 1, 1, (int8_t)(ext & 0xFF));
    return true;
}

/* Resolve an EA (the address of a memory operand into w1, with the
 * register updates of (An)+ and -(An)).  False if not translated. */
static bool ea(tctx *t, int mode, int reg, int sz, jea *e)
{
    *e = (jea){ EA_M, reg, 0, false, 0 };
    a64_buf *b = &t->b;
    uint32_t inc = (sz == 1 && reg == 7) ? 2 : (uint32_t)sz;
    switch (mode) {
    case 0: e->kind = EA_D; return true;
    case 1: e->kind = EA_A; return true;
    case 2: getr(t, 1, 8 + reg); return true;
    case 3:
        getr(t, 1, 8 + reg);
        a64_addsub_imm(b, A64_ADDI, false, 9, 1, inc);
        setr(t, 9, 8 + reg);
        return true;
    case 4:
        getr(t, 1, 8 + reg);
        a64_addsub_imm(b, A64_SUBI, false, 1, 1, inc);
        setr(t, 1, 8 + reg);
        return true;
    case 5:
        getr(t, 1, 8 + reg);
        add_imm(t, 1, 1, (int16_t)ext16(t));
        return true;
    case 6: {
        uint16_t x = ext16(t);
        getr(t, 1, 8 + reg);
        return index_brief(t, x);
    }
    case 7:
        switch (reg) {
        case 0:
            e->constant = true;
            e->addr = (uint32_t)(int32_t)(int16_t)ext16(t);
            a64_mov32(b, 1, e->addr);
            return true;
        case 1:
            e->constant = true;
            e->addr = ext32(t);
            a64_mov32(b, 1, e->addr);
            return true;
        case 2: {
            uint32_t base = t->at;
            e->constant = true;
            e->addr = base + (uint32_t)(int32_t)(int16_t)ext16(t);
            a64_mov32(b, 1, e->addr);
            return true;
        }
        case 3: {
            uint32_t base = t->at;
            uint16_t x = ext16(t);
            a64_mov32(b, 1, base);
            return index_brief(t, x);
        }
        case 4:
            e->kind = EA_I;
            e->imm = sz == 1 ? ext16(t) & 0xFFu : sz == 2 ? ext16(t) : ext32(t);
            return true;
        default:
            return false;
        }
    default:
        return false;
    }
}

/* The operand's sz-byte value into w0 (zero-extended); w1 keeps a memory
 * operand's address. */
static void ea_get(tctx *t, const jea *e, int sz)
{
    switch (e->kind) {
    case EA_D:
    case EA_A:
        getr(t, 0, e->kind == EA_D ? e->reg : 8 + e->reg);
        if (sz < 4)
            a64_ubfx(&t->b, 0, 0, 0, 8 * sz);
        break;
    case EA_M:
        a64_mov(&t->b, false, 3, 1); /* the unmasked address, kept */
        load(t, sz);
        a64_mov(&t->b, false, 1, 3);
        break;
    default:
        a64_mov32(&t->b, 0, e->imm);
        break;
    }
}

/* Store w2 to the operand (w1 its address). */
static void ea_put(tctx *t, const jea *e, int sz)
{
    switch (e->kind) {
    case EA_D:
        setr_sz(t, 2, e->reg, sz);
        break;
    case EA_A:
        setr(t, 2, 8 + e->reg);
        break;
    case EA_M:
        store(t, sz, true);
        break;
    default:
        break;
    }
}

static bool data_alterable(int mode, int reg) { return mode != 1 && (mode != 7 || reg < 2); }
static bool control(int mode, int reg) { return mode == 2 || mode == 5 || mode == 6 || (mode == 7 && reg < 4); }

/* Push w2 (32 bits), as cpu_push32: unwatched. */
static void push32(tctx *t)
{
    getr(t, 1, 15);
    a64_addsub_imm(&t->b, A64_SUBI, false, 1, 1, 4);
    setr(t, 1, 15);
    store(t, 4, false);
}

/* ------------------------------------------------------------------ */
/* Exits */

static void exit_pcs(tctx *t)
{
    a64_buf *b = &t->b;
    a64_mov32(b, 0, t->last_pc);
    a64_ldst(b, A64_STRW, 0, HC, FIELD(op_pc));
    a64_movz(b, false, 0, t->last_op, 0);
    a64_ldst(b, A64_STRH, 0, HC, FIELD(op));
}

static void exit_to(tctx *t, uint32_t target)
{
    a64_buf *b = &t->b;
    jit_exit_chainable_head(b);
    a64_mov32(b, 0, target);
    a64_ldst(b, A64_STRW, 0, HC, FIELD(pc));
    exit_pcs(t);
    jit_exit_chainable_tail(b);
}

/* To the address in c->pc (already stored). */
static void exit_indirect(tctx *t)
{
    exit_pcs(t);
    a64_ldst(&t->b, A64_LDRW, 12, HC, FIELD(pc));
    jit_fast_lookup(&t->b, 12, 1);
    jit_exit_plain(&t->b);
}

/* ------------------------------------------------------------------ */
/* Instructions */

static int size_bits(int bits) { return bits == 0 ? 1 : bits == 1 ? 2 : 4; }

/* add/sub of w0 (source) and the destination value in w4, both sz bytes,
 * result into w2; flags of kind ADD/SUB (CMP: no result, no X). */
static void arith(tctx *t, bool sub, int sz, bool cmp)
{
    a64_buf *b = &t->b;
    uint8_t w = cmp ? FNZVC : FALL;
    if (!cmp)
        a64_addsub(b, sub ? A64_SUB : A64_ADD, false, 2, 4, 0, 0, 0);
    if (flags_needed(t, w)) {
        top(t, 13, 4, sz);
        top(t, 14, 0, sz);
        a64_addsub(b, sub ? A64_SUBS : A64_ADDS, false, A64_ZR, 13, 14, 0, 0);
        flags_store(t, sub ? FK_SUB : FK_ADD, w, -1);
    }
}

/* Returns false if the instruction isn't translated. */
static bool insn(tctx *t)
{
    a64_buf *b = &t->b;
    uint16_t op = t->iop;
    int mode = (op >> 3) & 7, reg = op & 7, rn = (op >> 9) & 7;
    jea s, d;

    switch (op >> 12) {
    case 0x0: {
        if (op & 0x0100)
            return false;
        int kind = (op >> 9) & 7, szb = (op >> 6) & 3;
        if (kind == 4 || kind == 7 || szb == 3 || (mode == 7 && reg == 4))
            return false;
        if (kind != 0 && kind != 1 && kind != 2 && kind != 3 && kind != 5 && kind != 6)
            return false;
        int sz = size_bits(szb);
        uint32_t imm = sz == 1 ? ext16(t) & 0xFFu : sz == 2 ? ext16(t) : ext32(t);
        if (kind == 6 ? (mode == 1 || (mode == 7 && reg > 3)) : !data_alterable(mode, reg))
            return false;
        if (!ea(t, mode, reg, sz, &d))
            return false;
        ea_get(t, &d, sz);
        a64_mov(b, false, 4, 0);  /* the destination's value */
        a64_mov32(b, 0, imm);     /* the immediate */
        if (kind == 2 || kind == 3 || kind == 6) {
            arith(t, kind != 3, sz, kind == 6);
            if (kind == 6)
                return true;
        } else {
            a64_log(b, kind == 0 ? A64_ORR : kind == 1 ? A64_AND : A64_EOR, 2, 4, 0);
            flags_logic(t, 2, sz);
        }
        ea_put(t, &d, sz);
        return true;
    }
    case 0x1: case 0x2: case 0x3: { /* MOVE, MOVEA */
        int top4 = op >> 12;
        int sz = top4 == 1 ? 1 : top4 == 3 ? 2 : 4;
        int dmode = (op >> 6) & 7;
        if (mode == 7 && reg > 4)
            return false;
        if (sz == 1 && (mode == 1 || dmode == 1))
            return false;
        if (!ea(t, mode, reg, sz, &s))
            return false;
        ea_get(t, &s, sz);
        if (dmode == 1) {
            if (sz == 2)
                a64_sxth(b, 0, 0);
            setr(t, 0, 8 + rn);
            return true;
        }
        if (!data_alterable(dmode, rn))
            return false;
        a64_mov(b, false, 2, 0);
        if (!ea(t, dmode, rn, sz, &d))
            return false;
        flags_logic(t, 2, sz);
        ea_put(t, &d, sz);
        return true;
    }
    case 0x4:
        break;
    case 0x5: {
        int szb = (op >> 6) & 3;
        if (szb == 3) {
            int cc = (op >> 8) & 15;
            if (mode == 1) { /* DBcc: handled as a branch */
                return false;
            }
            if (!data_alterable(mode, reg))
                return false; /* TRAPcc, or not alterable */
            if (!ea(t, mode, reg, 1, &d))
                return false; /* Scc */
            if (d.kind == EA_M)
                a64_mov(b, false, 5, 1);
            int h = cond_flags(t, cc);
            if (h == A64_AL)
                a64_movz(b, false, 2, 0xFF, 0);
            else if (h == NEVER)
                a64_movz(b, false, 2, 0, 0);
            else {
                a64_cset(b, 2, h);
                a64_sub(b, 2, A64_ZR, 2);
            }
            if (d.kind == EA_M)
                a64_mov(b, false, 1, 5);
            ea_put(t, &d, 1);
            return true;
        }
        int sz = size_bits(szb);
        uint32_t q = (uint32_t)rn ? (uint32_t)rn : 8;
        bool sub = op & 0x0100;
        if (mode == 1) {
            if (sz == 1)
                return false;
            getr(t, 0, 8 + reg);
            a64_addsub_imm(b, sub ? A64_SUBI : A64_ADDI, false, 0, 0, q);
            setr(t, 0, 8 + reg);
            return true;
        }
        if (!data_alterable(mode, reg))
            return false;
        if (!ea(t, mode, reg, sz, &d))
            return false;
        ea_get(t, &d, sz);
        a64_mov(b, false, 4, 0);
        a64_movz(b, false, 0, (uint16_t)q, 0);
        arith(t, sub, sz, false);
        ea_put(t, &d, sz);
        return true;
    }
    case 0x7: {
        if (op & 0x0100)
            return false;
        a64_mov32(b, 0, (uint32_t)(int32_t)(int8_t)(op & 0xFF));
        setr(t, 0, rn);
        flags_logic(t, 0, 4);
        return true;
    }
    case 0x8: case 0x9: case 0xB: case 0xC: case 0xD: {
        int line = op >> 12, opmode = (op >> 6) & 7;
        if (opmode == 3 || opmode == 7) {
            int sz = opmode == 3 ? 2 : 4;
            if (line == 0x8)
                return false; /* DIVU/DIVS */
            if (mode == 7 && reg > 4)
                return false;
            if (line == 0xC) { /* MULU.W / MULS.W */
                if (mode == 1 || !ea(t, mode, reg, 2, &s))
                    return false;
                ea_get(t, &s, 2);
                getr(t, 4, rn);
                if (opmode == 7) {
                    a64_sxth(b, 0, 0);
                    a64_sxth(b, 4, 4);
                } else {
                    a64_ubfx(b, 4, 4, 0, 16);
                }
                a64_mul(b, 2, 0, 4);
                setr(t, 2, rn);
                flags_logic(t, 2, 4);
                return true;
            }
            /* ADDA, SUBA, CMPA */
            if (!ea(t, mode, reg, sz, &s))
                return false;
            ea_get(t, &s, sz);
            if (sz == 2)
                a64_sxth(b, 0, 0);
            getr(t, 4, 8 + rn);
            if (line == 0xB) {
                arith(t, true, 4, true);
                return true;
            }
            a64_addsub(b, line == 0xD ? A64_ADD : A64_SUB, false, 2, 4, 0, 0, 0);
            setr(t, 2, 8 + rn);
            return true;
        }
        int sz = size_bits(opmode & 3);
        bool to_ea = opmode & 4;
        if (to_ea && mode <= 1) {
            if (line == 0xC && mode <= 1 && (opmode == 5 || opmode == 6)) { /* EXG */
                int x, y;
                if (opmode == 5 && mode == 0) { x = rn; y = reg; }
                else if (opmode == 5 && mode == 1) { x = 8 + rn; y = 8 + reg; }
                else if (opmode == 6 && mode == 1) { x = rn; y = 8 + reg; }
                else return false;
                getr(t, 0, x);
                getr(t, 2, y);
                setr(t, 2, x);
                setr(t, 0, y);
                return true;
            }
            if (line != 0xB || mode != 0)
                return false; /* ABCD, SBCD, ADDX, SUBX, CMPM, PACK, UNPK */
        }
        if (line == 0xB) {
            if (to_ea) { /* EOR Dn,<ea> */
                if (!data_alterable(mode, reg) || !ea(t, mode, reg, sz, &d))
                    return false;
                ea_get(t, &d, sz);
                getr(t, 4, rn);
                a64_log(b, A64_EOR, 2, 0, 4);
                flags_logic(t, 2, sz);
                ea_put(t, &d, sz);
                return true;
            }
            /* CMP <ea>,Dn */
            if ((mode == 7 && reg > 4) || (sz == 1 && mode == 1) || !ea(t, mode, reg, sz, &s))
                return false;
            ea_get(t, &s, sz);
            getr(t, 4, rn);
            arith(t, true, sz, true);
            return true;
        }
        if (to_ea) {
            if (!data_alterable(mode, reg) || mode == 0 || !ea(t, mode, reg, sz, &d))
                return false;
            ea_get(t, &d, sz);       /* destination */
            a64_mov(b, false, 4, 0);
            getr(t, 0, rn);          /* source */
        } else {
            if ((mode == 7 && reg > 4) || (sz == 1 && mode == 1))
                return false;
            if ((line == 0x8 || line == 0xC) && mode == 1)
                return false;
            if (!ea(t, mode, reg, sz, &s))
                return false;
            ea_get(t, &s, sz);
            getr(t, 4, rn);
            d.kind = EA_D;
            d.reg = rn;
        }
        if (line == 0x8 || line == 0xC) {
            a64_log(b, line == 0x8 ? A64_ORR : A64_AND, 2, 0, 4);
            flags_logic(t, 2, sz);
        } else {
            arith(t, line == 0x9, sz, false);
        }
        ea_put(t, &d, sz);
        return true;
    }
    case 0xE: {
        if ((op & 0xF8C0) == 0xE8C0 || ((op >> 6) & 3) == 3 || (op & 0x20))
            return false; /* bit fields, memory shifts, counts in registers */
        int type = (op >> 3) & 3;
        bool left = op & 0x0100;
        if (type > 1 || (type == 0 && left))
            return false; /* ASL (its V), ROXd, ROd */
        int sz = size_bits((op >> 6) & 3), bits = 8 * sz;
        int cnt = rn ? rn : 8;
        getr(t, 0, reg);
        if (sz < 4)
            a64_ubfx(b, 0, 0, 0, bits);
        if (left) { /* LSL */
            a64_lsl(b, 2, 0, cnt);
            a64_ubfx(b, 6, 0, bits - cnt, 1);
        } else if (type == 1) { /* LSR */
            a64_lsr(b, 2, 0, cnt);
            a64_ubfx(b, 6, 0, cnt - 1, 1);
        } else { /* ASR */
            if (sz == 1)
                a64_sxtb(b, 2, 0);
            else if (sz == 2)
                a64_sxth(b, 2, 0);
            else
                a64_mov(b, false, 2, 0);
            a64_ubfx(b, 6, 2, cnt - 1, 1);
            a64_asr(b, 2, 2, cnt);
        }
        if (sz < 4)
            a64_ubfx(b, 2, 2, 0, bits);
        setr_sz(t, 2, reg, sz);
        if (flags_needed(t, FALL)) {
            top(t, 13, 2, sz);
            a64_logic(b, A64_ANDS, false, A64_ZR, 13, 13, 0, 0);
            flags_store(t, FK_NONE, FALL, 6);
            t->fk = FK_NONE;
        }
        return true;
    }
    default:
        return false;
    }

    /* Line 4 */
    if ((op & 0xF1C0) == 0x41C0) { /* LEA */
        if (!control(mode, reg)) {
            if ((op & 0xFFF8) == 0x49C0) { /* EXTB.L */
                getr(t, 0, reg);
                a64_sxtb(b, 0, 0);
                setr(t, 0, reg);
                flags_logic(t, 0, 4);
                return true;
            }
            return false;
        }
        if (!ea(t, mode, reg, 4, &s))
            return false;
        setr(t, 1, 8 + rn);
        return true;
    }
    if ((op & 0xF140) == 0x4100)
        return false; /* CHK */
    switch (op & 0x0FC0) {
    case 0x0840:
        if (mode == 0) { /* SWAP */
            getr(t, 0, reg);
            a64_ror(b, 0, 0, 16);
            setr(t, 0, reg);
            flags_logic(t, 0, 4);
            return true;
        }
        if (!control(mode, reg) || !ea(t, mode, reg, 4, &s))
            return false; /* PEA */
        a64_mov(b, false, 2, 1);
        push32(t);
        return true;
    case 0x0880:
    case 0x08C0:
        if (mode == 0) { /* EXT.W / EXT.L */
            getr(t, 0, reg);
            if (op & 0x40) {
                a64_sxth(b, 0, 0);
                setr(t, 0, reg);
                flags_logic(t, 0, 4);
            } else {
                a64_sxtb(b, 2, 0);
                setr_sz(t, 2, reg, 2);
                flags_logic(t, 2, 2);
            }
            return true;
        }
        /* fall through: MOVEM to memory */
    case 0x0C80:
    case 0x0CC0: { /* MOVEM */
        bool to_regs = op & 0x0400;
        int sz = (op & 0x40) ? 4 : 2;
        uint16_t list = ext16(t);
        a64_buf *bb = b;
        if (!to_regs && mode == 4) {
            getr(t, 1, 8 + reg);
            a64_ldst(bb, A64_STRW, 1, JR_CTX, JIT_CTX(keep));
            for (int i = 0; i < 16; i++) {
                if (!(list & (1u << i)))
                    continue;
                int r = 15 - i;
                a64_ldst(bb, A64_LDRW, 1, JR_CTX, JIT_CTX(keep));
                a64_addsub_imm(bb, A64_SUBI, false, 1, 1, (uint32_t)sz);
                a64_ldst(bb, A64_STRW, 1, JR_CTX, JIT_CTX(keep));
                getr(t, 2, r);
                store(t, sz, true);
            }
            a64_ldst(bb, A64_LDRW, 1, JR_CTX, JIT_CTX(keep));
            setr(t, 1, 8 + reg);
            return true;
        }
        if (to_regs ? !(control(mode, reg) || mode == 3) : !(control(mode, reg) && !(mode == 7 && reg >= 2)))
            return false;
        if (mode == 3)
            getr(t, 1, 8 + reg);
        else if (!ea(t, mode, reg, 4, &s))
            return false;
        if (to_regs) {
            for (int i = 0; i < 16; i++) {
                if (!(list & (1u << i)))
                    continue;
                a64_mov(bb, false, 3, 1);
                load(t, sz);
                a64_mov(bb, false, 1, 3);
                if (sz == 2)
                    a64_sxth(bb, 0, 0);
                setr(t, 0, i);
                a64_addsub_imm(bb, A64_ADDI, false, 1, 1, (uint32_t)sz);
            }
            if (mode == 3)
                setr(t, 1, 8 + reg);
            return true;
        }
        a64_ldst(bb, A64_STRW, 1, JR_CTX, JIT_CTX(keep));
        for (int i = 0; i < 16; i++) {
            if (!(list & (1u << i)))
                continue;
            a64_ldst(bb, A64_LDRW, 1, JR_CTX, JIT_CTX(keep));
            getr(t, 2, i);
            a64_addsub_imm(bb, A64_ADDI, false, 3, 1, (uint32_t)sz);
            a64_ldst(bb, A64_STRW, 3, JR_CTX, JIT_CTX(keep));
            store(t, sz, true);
        }
        return true;
    }
    default:
        break;
    }
    if ((op & 0xFFF8) == 0x4E50) { /* LINK.W */
        int16_t disp = (int16_t)ext16(t);
        getr(t, 2, 8 + reg);
        if (reg == 7)
            a64_addsub_imm(b, A64_SUBI, false, 2, 2, 4);
        push32(t);
        getr(t, 0, 15);
        setr(t, 0, 8 + reg);
        add_imm(t, 0, 0, disp);
        setr(t, 0, 15);
        return true;
    }
    if ((op & 0xFFF8) == 0x4E58) { /* UNLK */
        getr(t, 1, 8 + reg);
        a64_mov(b, false, 3, 1);
        load(t, 4);
        a64_addsub_imm(b, A64_ADDI, false, 3, 3, 4);
        setr(t, 3, 15);
        setr(t, 0, 8 + reg);
        return true;
    }
    if (op == 0x4E71) /* NOP */
        return true;
    /* NEGX / CLR / NEG / NOT / TST */
    int szb = (op >> 6) & 3;
    if (szb == 3)
        return false;
    int sz = size_bits(szb);
    switch (op & 0x0F00) {
    case 0x0200: /* CLR */
        if (!data_alterable(mode, reg) || !ea(t, mode, reg, sz, &d))
            return false;
        a64_movz(b, false, 2, 0, 0);
        flags_logic(t, 2, sz);
        ea_put(t, &d, sz);
        return true;
    case 0x0400: /* NEG */
        if (!data_alterable(mode, reg) || !ea(t, mode, reg, sz, &d))
            return false;
        ea_get(t, &d, sz);
        a64_movz(b, false, 4, 0, 0);
        arith(t, true, sz, false);
        ea_put(t, &d, sz);
        return true;
    case 0x0600: /* NOT */
        if (!data_alterable(mode, reg) || !ea(t, mode, reg, sz, &d))
            return false;
        ea_get(t, &d, sz);
        a64_log(b, A64_ORN, 2, A64_ZR, 0);
        flags_logic(t, 2, sz);
        ea_put(t, &d, sz);
        return true;
    case 0x0A00: /* TST */
        if ((mode == 7 && reg > 4) || (sz == 1 && mode == 1) || !ea(t, mode, reg, sz, &s))
            return false;
        ea_get(t, &s, sz);
        flags_logic(t, 0, sz);
        return true;
    default:
        return false;
    }
}

/* Branches end the block.  Returns false if not one we translate. */
static bool is_branch(uint16_t op, int model)
{
    if ((op & 0xF000) == 0x6000)
        return true;
    if ((op & 0xF0F8) == 0x50C8)
        return true; /* DBcc */
    if (op == 0x4E75 || op == 0x4E74)
        return true; /* RTS, RTD */
    if ((op & 0xFF80) == 0x4E80) { /* JSR, JMP */
        int mode = (op >> 3) & 7, reg = op & 7;
        return control(mode, reg);
    }
    (void)model;
    return false;
}

static bool branch(tctx *t)
{
    a64_buf *b = &t->b;
    uint16_t op = t->iop;
    uint32_t base = t->ipc + 2;
    if ((op & 0xF000) == 0x6000) {
        int cc = (op >> 8) & 15;
        int32_t disp = (int8_t)(op & 0xFF);
        if (disp == 0)
            disp = (int16_t)ext16(t);
        else if (disp == -1 && t->model != 68000)
            disp = (int32_t)ext32(t);
        uint32_t target = base + (uint32_t)disp, next = t->at;
        t->succ[0] = target;
        t->succ[1] = next;
        t->nsucc = cc < 2 ? 1 : 2;
        if (cc == 1) { /* BSR */
            a64_mov32(b, 2, next);
            push32(t);
            cache_flush(t);
            exit_to(t, target);
            return true;
        }
        if (cc == 0) {
            cache_flush(t);
            exit_to(t, target);
            return true;
        }
        cache_flush(t);
        int h = cond_flags(t, cc);
        if (h == NEVER) {
            exit_to(t, next);
            return true;
        }
        if (h == A64_AL) {
            exit_to(t, target);
            return true;
        }
        uint32_t nt = a64_here(b);
        a64_bcond(b, a64_invert(h), 0);
        exit_to(t, target);
        a64_patch_here(b, nt);
        exit_to(t, next);
        return true;
    }
    if ((op & 0xF0F8) == 0x50C8) { /* DBcc */
        int cc = (op >> 8) & 15, reg = op & 7;
        int16_t disp = (int16_t)ext16(t);
        uint32_t target = base + (uint32_t)(int32_t)disp, next = t->at;
        t->succ[0] = target;
        t->succ[1] = next;
        t->nsucc = 2;
        t->clobber = true; /* the count's compare */
        uint32_t to_next = UINT32_MAX;
        cache_flush(t);
        int h = cond_flags(t, cc);
        if (h == A64_AL) {
            cache_flush(t);
            exit_to(t, next);
            return true;
        }
        if (h != NEVER) {
            to_next = a64_here(b);
            a64_bcond(b, h, 0);
        }
        getr(t, 0, reg);
        a64_addsub_imm(b, A64_SUBI, false, 9, 0, 1);
        a64_bfi(b, 0, 9, 0, 16);
        setr(t, 0, reg);
        cache_flush(t);
        a64_ubfx(b, 9, 9, 0, 16);
        a64_mov32(b, 10, 0xFFFF);
        a64_cmp(b, 9, 10);
        uint32_t expired = a64_here(b);
        a64_bcond(b, A64_EQ, 0);
        exit_to(t, target);
        a64_patch_here(b, expired);
        if (to_next != UINT32_MAX) {
            uint32_t skip = a64_here(b);
            a64_b(b, 0);
            a64_patch_here(b, to_next);
            a64_patch_here(b, skip);
        }
        exit_to(t, next);
        return true;
    }
    if (op == 0x4E75 || op == 0x4E74) { /* RTS, RTD */
        int32_t disp = op == 0x4E74 ? (int16_t)ext16(t) : 0;
        t->indirect = true;
        getr(t, 1, 15);
        a64_mov(b, false, 3, 1);
        load(t, 4);
        a64_ldst(b, A64_STRW, 0, HC, FIELD(pc));
        add_imm(t, 3, 3, 4 + disp);
        setr(t, 3, 15);
        cache_flush(t);
        exit_indirect(t);
        return true;
    }
    /* JSR, JMP */
    int mode = (op >> 3) & 7, reg = op & 7;
    jea e;
    if (!ea(t, mode, reg, 4, &e))
        return false;
    bool jsr = !(op & 0x40);
    uint32_t next = t->at;
    if (jsr) {
        a64_ldst(b, A64_STRW, 1, HC, FIELD(pc));
        a64_mov32(b, 2, next);
        push32(t);
    }
    cache_flush(t);
    if (e.constant) {
        t->succ[0] = e.addr;
        t->nsucc = 1;
        exit_to(t, e.addr);
    } else {
        t->indirect = true;
        if (!jsr)
            a64_ldst(b, A64_STRW, 1, HC, FIELD(pc));
        exit_indirect(t);
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Instructions the interpreter runs inside a block */

/* Decode enough of an instruction left to the interpreter to know its
 * length (t->at moves past it); false if it isn't one the block may run:
 * it must neither change the flow (an exception taken is seen after it)
 * nor stop the processor. */
static bool exec_decode(tctx *t)
{
    uint16_t op = t->iop;
    int mode = (op >> 3) & 7, reg = op & 7, szb = (op >> 6) & 3;
    jea e;
    switch (op >> 12) {
    case 0x0:
        if (op & 0x0100) {
            if (mode == 1) { /* MOVEP */
                ext16(t);
                return true;
            }
            return ea(t, mode, reg, mode == 0 ? 4 : 1, &e); /* dynamic bit operation */
        }
        if (((op >> 9) & 7) == 4) { /* static bit operation */
            ext16(t);
            return ea(t, mode, reg, mode == 0 ? 4 : 1, &e);
        }
        if (szb == 3) { /* CHK2, CMP2, CAS */
            if (((op >> 9) & 7) == 3 || ((op >> 9) & 7) == 4)
                return false;
            ext16(t);
            return ea(t, mode, reg, 4, &e);
        }
        if (mode == 7 && reg == 4) { /* to CCR or SR */
            ext16(t);
            return true;
        }
        return false;
    case 0x4:
        if ((op & 0xF140) == 0x4100) /* CHK */
            return ea(t, mode, reg, (op & 0x80) ? 2 : 4, &e);
        switch (op & 0x0FC0) {
        case 0x00C0: case 0x02C0: case 0x04C0: case 0x06C0: /* MOVE from/to SR/CCR */
            return ea(t, mode, reg, 2, &e);
        case 0x0C00: case 0x0C40: /* MULL, DIVL */
            ext16(t);
            return ea(t, mode, reg, 4, &e);
        case 0x0800:
            if (mode == 1) { /* LINK.L */
                ext32(t);
                return true;
            }
            return ea(t, mode, reg, 1, &e); /* NBCD */
        case 0x0AC0:
            return op != 0x4AFC && ea(t, mode, reg, 1, &e); /* TAS */
        default:
            break;
        }
        if ((op & 0x0F00) == 0x0000 && szb != 3) /* NEGX */
            return ea(t, mode, reg, szb == 0 ? 1 : szb == 1 ? 2 : 4, &e);
        return false;
    case 0x8: case 0x9: case 0xB: case 0xC: case 0xD: {
        int line = op >> 12, opmode = (op >> 6) & 7;
        if (line == 0x8 && (opmode == 3 || opmode == 7)) /* DIVU, DIVS */
            return ea(t, mode, reg, 2, &e);
        if ((opmode & 4) && mode <= 1 && opmode != 7) {
            if (line == 0x8 && (opmode == 5 || opmode == 6)) { /* PACK, UNPK */
                ext16(t);
                return true;
            }
            if (line == 0xB)
                return mode == 1; /* CMPM */
            return (opmode & 3) != 3; /* SBCD, ABCD, ADDX, SUBX */
        }
        return false;
    }
    case 0xE:
        if ((op & 0xF8C0) == 0xE8C0) { /* bit fields */
            ext16(t);
            return ea(t, mode, reg, 4, &e);
        }
        if (szb == 3) /* memory shifts */
            return ea(t, mode, reg, 2, &e);
        return true; /* register shifts and rotates */
    default:
        return false;
    }
}

/* Its length in bytes (0: not run in the block), emitting nothing. */
static uint32_t exec_len(tctx *t)
{
    static uint32_t dummy[1];
    a64_buf saved = t->b;
    int uses[16];
    memcpy(uses, t->uses, sizeof uses);
    t->b = (a64_buf){ dummy, 0, 0, false };
    t->at = t->ipc + 2;
    bool ok = exec_decode(t);
    t->b = saved;
    memcpy(t->uses, uses, sizeof uses);
    return ok ? t->at - t->ipc : 0;
}

/* Run the instruction through the interpreter; leave the block if it took
 * an exception or stopped the processor (`left` instructions not run). */
static void exec_insn(tctx *t, uint32_t len, uint32_t left)
{
    a64_buf *b = &t->b;
    cache_flush(t);
    a64_mov(b, true, 0, HC);
    a64_mov32(b, 1, t->ipc);
    jit_call(b, H_EXEC);
    cache_load(t);
    a64_ldst(b, A64_LDRW, 0, HC, FIELD(pc));
    a64_mov32(b, 9, t->ipc + len);
    a64_cmp(b, 0, 9);
    uint32_t away = a64_here(b);
    a64_bcond(b, A64_NE, 0);
    a64_ldst(b, A64_LDRB, 0, HC, FIELD(halted));
    uint32_t stays = a64_here(b);
    a64_cbz(b, false, 0, 0);
    a64_patch_here(b, away);
    if (left)
        a64_addsub_imm(b, A64_ADDI, true, HB, HB, left);
    jit_exit_plain(b);
    a64_patch_here(b, stays);
    t->fk = FK_NONE;
    t->stored = true;
    t->clobber = true;
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
    snprintf(id, sizeof id, "68k %d %s %s %zu %zu %zu", TRANSLATOR_VERSION, __DATE__, __TIME__, sizeof(cpu68k),
             sizeof(jit_ctx), offsetof(cpu68k, sr));
    return jit_hash(id, strlen(id), 0);
}

static uint64_t seed(void) { return (uint64_t)cur_mask << 20 ^ (uint64_t)cur_model; }

typedef struct scan_info {
    uint32_t pc;
    uint16_t op;
    uint8_t reads, writes;
    bool branch;
    uint32_t exec;           /* run by the interpreter: its length */
    enum fkind kind;         /* the host flags it sets */
    bool clobber;            /* host flags not its own at its end */
    int cc;                  /* the condition it tests, or -1 */
} scan_info;

static bool lookahead = true; /* off: flags are taken to be read after every block */

/* Decode the instruction at `at` (t scanning): false if not translated
 * or run in a block. */
static bool scan_one(tctx *t, uint32_t at, scan_info *si)
{
    static uint32_t dummy[1];
    t->b = (a64_buf){ dummy, 0, 0, false };
    t->ipc = at;
    t->iop = rd16(at & t->mask);
    t->at = at + 2;
    t->reads = t->writes = 0;
    t->fk = FK_NONE;
    t->setkind = FK_NONE;
    t->clobber = false;
    t->cc = -1;
    bool br = is_branch(t->iop, t->model);
    bool ok = br ? branch(t) : insn(t);
    uint32_t elen = 0;
    if (!ok && !br && (elen = exec_len(t)) != 0) {
        ok = true;
        t->at = at + elen;
        t->reads = FALL; /* it may read any flag; it sets them in SR */
        t->writes = 0;
        t->clobber = true;
        t->cc = -1;
    }
    *si = (scan_info){ at, t->iop, t->reads, t->writes, br, elen, t->setkind, t->clobber, t->cc };
    return ok;
}

/* The flags the code at addr may read before it sets them: straight on,
 * up to 16 instructions, everything not yet set taken as read at a branch
 * or at an instruction the JIT doesn't know.  *len: the bytes looked at. */
static uint8_t flags_read_at(uint32_t addr, uint32_t *len)
{
    tctx t;
    memset(&t, 0, sizeof t);
    memset(t.host, -1, sizeof t.host);
    t.mask = cur_mask;
    t.model = cur_model;
    t.scanning = true;
    t.live = FALL;
    uint8_t need = 0, set = 0;
    uint32_t at = addr;
    for (int k = 0; k < 16 && set != FALL; k++) {
        scan_info si;
        if (!scan_one(&t, at, &si))
            break;
        need |= si.reads & ~set;
        set |= si.writes;
        at = t.at;
        if (si.branch)
            break;
    }
    *len = at - addr;
    return (uint8_t)(need | (FALL & ~set));
}

static jit_block *translate(uint32_t pc)
{
    uint64_t t0 = now_ns();
    tctx t;
    memset(&t, 0, sizeof t);
    memset(t.host, -1, sizeof t.host);
    t.mask = cur_mask;
    t.model = cur_model;
    t.pc0 = pc;

    /* Scan: how far the block goes, each instruction's flags. */
    scan_info info[MAX_INSN];
    uint32_t n = 0, at = pc;
    t.scanning = true;
    t.live = FALL;
    while (n < (uint32_t)max_insn && at - pc < MAX_BYTES) {
        if (!scan_one(&t, at, &info[n]))
            break;
        n++;
        at = t.at;
        if (info[n - 1].branch)
            break;
    }
    uint32_t len = at - pc;
    if (n == 0)
        return jit_marker(&E, pc, 2);

    jit_block *cached = jit_from_cache(&E, pc, seed());
    if (cached) {
        st.from_cache++;
        st.translate_ns += now_ns() - t0;
        return cached;
    }

    /* The flags read after the block: what its successors may read before
     * setting them (X is always kept).  The block then depends on the code
     * looked at. */
    uint8_t live_out = FALL;
    jit_dep deps[JIT_MAX_DEPS];
    int ndeps = 0;
    if (lookahead && max_insn == MAX_INSN && !t.indirect) {
        uint32_t succ[2] = { at, 0 };
        int nsucc = 1;
        if (info[n - 1].branch) {
            succ[0] = t.succ[0];
            succ[1] = t.succ[1];
            nsucc = t.nsucc;
        }
        live_out = FX;
        for (int k = 0; k < nsucc; k++) {
            uint32_t dlen;
            live_out |= flags_read_at(succ[k], &dlen);
            if (dlen)
                deps[ndeps++] = (jit_dep){ succ[k], dlen };
        }
        if (live_out == FALL)
            ndeps = 0;
    }

    /* Flags read from SR after each instruction.  A Bcc, DBcc or Scc
     * branching on the host flags of the instruction that set them (none
     * clobbered in between) doesn't need them in SR; that instruction
     * must set the host's (host_need). */
    uint8_t live_after[MAX_INSN];
    bool host_need[MAX_INSN] = { false };
    uint8_t live = live_out;
    for (int i = (int)n - 1; i >= 0; i--) {
        live_after[i] = live;
        uint8_t reads = info[i].reads;
        if (info[i].cc >= 2) {
            uint8_t r = cc_reads(info[i].cc);
            for (int k = i - 1; k >= 0; k--) {
                if (info[k].writes) {
                    if (!info[k].clobber && host_cond(info[k].kind, info[i].cc) != -1 && (info[k].writes & r) == r) {
                        host_need[k] = true;
                        reads &= (uint8_t)~r;
                    }
                    break;
                }
                if (info[k].clobber)
                    break;
            }
        }
        live = (uint8_t)((live & ~info[i].writes) | reads);
    }

    /* The registers used most live in x24-x28. */
    for (int h = 24; h <= 28; h++) {
        int best = -1;
        for (int g = 0; g < 16; g++)
            if (t.host[g] < 0 && t.uses[g] >= 2 && (best < 0 || t.uses[g] > t.uses[best]))
                best = g;
        if (best < 0)
            break;
        t.host[best] = (int8_t)h;
    }

    t.scanning = false;
    t.b = (a64_buf){ scratch, 0, 16384 - 1, false };
    t.ninsn = n;
    t.fk = FK_NONE;
    a64_buf *b = &t.b;
    a64_addsub_imm(b, A64_SUBSI, true, HB, HB, n);
    uint32_t to_over = a64_here(b);
    a64_bcond(b, A64_LT, 0);
    cache_load(&t);
    for (uint32_t i = 0; i < n; i++) {
        t.ipc = info[i].pc;
        t.iop = info[i].op;
        t.at = t.ipc + 2;
        t.live = live_after[i];
        t.host_need = host_need[i];
        t.last_pc = t.ipc;
        t.last_op = t.iop;
        t.stored = false;
        if (info[i].exec) {
            exec_insn(&t, info[i].exec, n - (i + 1));
            t.at = t.ipc + info[i].exec;
        } else {
            bool ok = info[i].branch ? branch(&t) : insn(&t);
            (void)ok; /* the scan said it translates */
        }
        if (t.stored && !info[i].branch && i + 1 < n) {
            /* A store into translated code: leave after this instruction,
             * the dispatcher translating what follows afresh. */
            uint32_t skip = jit_killed_check(b);
            cache_flush_exit(&t);
            a64_addsub_imm(b, A64_ADDI, true, HB, HB, n - (i + 1));
            a64_mov32(b, 0, t.at);
            a64_ldst(b, A64_STRW, 0, HC, FIELD(pc));
            exit_pcs(&t);
            jit_exit_plain(b);
            a64_patch_here(b, skip);
            t.fk = FK_NONE;
        }
    }
    if (!info[n - 1].branch) {
        cache_flush(&t);
        exit_to(&t, at);
    }
    a64_patch_here(b, to_over);
    a64_addsub_imm(b, A64_ADDI, true, HB, HB, n);
    uint32_t dead_at = a64_here(b);
    a64_mov32(b, 0, pc);
    a64_ldst(b, A64_STRW, 0, HC, FIELD(pc));
    jit_exit_plain(b);
    if (b->full)
        return jit_marker(&E, pc, 2);

    jit_block *blk = jit_install(&E, pc, n, len, b->p, b->n, dead_at, deps, ndeps);
    if (blk) {
        st.translated++;
        if (max_insn == MAX_INSN)
            jit_remember(&E, pc, n, len, seed(), b->p, b->n, dead_at, deps, ndeps);
    }
    st.translate_ns += now_ns() - t0;
    return blk;
}

static bool setup(void)
{
    if (E.ready)
        return true;
    scratch = malloc(16384 * 4);
    if (!scratch || !jit_engine_init(&E, CODE_68K, 1, CODE_SIZE))
        return false;
    E.build = build_id();
    E.ctx.helper[H_ST8] = (void *)st8;
    E.ctx.helper[H_ST16] = (void *)st16;
    E.ctx.helper[H_ST32] = (void *)st32;
    E.ctx.helper[H_PST8] = (void *)pst8;
    E.ctx.helper[H_PST16] = (void *)pst16;
    E.ctx.helper[H_PST32] = (void *)pst32;
    E.ctx.helper[H_EXEC] = (void *)cpu_execute_at;
    return true;
}

bool cpu_jit_available(void) { return true; }

void cpu_jit_enable(bool on, const char *cache_dir, const char *cache_name)
{
    cpu_jit_enabled = on;
    jit_engine_cache(&E, cache_dir, cache_name);
}

/* Blocks are translated for one address mask and processor model. */
static void check_mode(cpu68k *c)
{
    if (c->addr_mask != cur_mask || c->model != cur_model) {
        if (E.npool)
            jit_flush(&E);
        cur_mask = c->addr_mask;
        cur_model = c->model;
    }
}

/* One block run from the dispatcher; false if the interpreter must step. */
static bool run_block(cpu68k *c, uint64_t left)
{
    jit_block *b = jit_lookup(&E, c->pc);
    if (!b)
        b = translate(c->pc);
    if (!b || !b->ninsn || b->ninsn > left)
        return false;
    E.ctx.watch_lo = g_watch_lo;
    E.ctx.watch_len = g_watch_len;
    E.ctx.last_stub = NULL;
    E.ctx.killed = 0;
    uint64_t ran = left - jit_enter(&E, c, (int64_t)left, b->entry);
    c->icount += ran;
    st.jitted += ran;
    st.blocks++;
    if (E.ctx.last_stub) {
        uint64_t gen = E.generation;
        jit_block *n = jit_lookup(&E, c->pc);
        if (!n)
            n = translate(c->pc);
        jit_chain(&E, n, gen);
    }
    return true;
}

uint64_t cpu_jit_run(cpu68k *c, uint64_t max)
{
    uint64_t start = c->icount;
    if (!E.ready && !setup()) {
        cpu_jit_enabled = false;
        while (!c->halted && !c->stopped && c->icount - start < max)
            cpu_step(c);
        return c->icount - start;
    }
    E.ctx.mem = g_mem;
    check_mode(c);
    while (!c->halted && !c->stopped) {
        uint64_t done = c->icount - start;
        if (done >= max)
            break;
        if (!run_block(c, max - done)) {
            cpu_step(c);
            st.interpreted++;
            check_mode(c);
        }
    }
    return c->icount - start;
}

void cpu_jit_step1(cpu68k *c)
{
    if (!E.ready && !setup()) {
        cpu_step(c);
        return;
    }
    E.ctx.mem = g_mem;
    check_mode(c);
    int was = max_insn;
    max_insn = 1;
    /* A one-instruction block, made afresh (the test rewrites the code). */
    jit_block *b = jit_lookup(&E, c->pc);
    if (b && b->ninsn != 1)
        jit_flush(&E);
    if (!run_block(c, 1))
        cpu_step(c);
    max_insn = was;
}

void cpu_jit_get_stats(cpu_jit_stats *s)
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

typedef struct gen {
    uint32_t *s;
    uint16_t w[16];
    int n;
    uint32_t scratch;
} gen;

static void put(gen *g, uint16_t v) { g->w[g->n++] = v; }
static void put32(gen *g, uint32_t v)
{
    put(g, (uint16_t)(v >> 16));
    put(g, (uint16_t)v);
}
static int size_code(int sz) { return sz == 1 ? 0 : sz == 2 ? 1 : 2; }

/* A brief extension word: index D or A .W (so addresses stay within 64 KB
 * of their base), scale 1 or 2, a small displacement. */
static uint16_t brief(gen *g)
{
    uint32_t r = rnd(g->s);
    return (uint16_t)((r & 0xF000) | ((r >> 4) & 0x0200) | (rnd(g->s) & 0xFF));
}

/* An EA's mode and register (extension words added): memory operands
 * near the address registers (all pointing into the scratch area), or
 * abs.L inside it.  Sources may also be immediate, PC-relative or abs.W
 * (reads only). */
static int ea_gen(gen *g, int sz, bool dest, bool allow_an)
{
    for (;;) {
        int m = (int)(rnd(g->s) % 12), r = (int)(rnd(g->s) % 8);
        switch (m) {
        case 0: return 0 << 3 | r;
        case 1:
            if (!allow_an || sz == 1)
                continue;
            return 1 << 3 | r;
        case 2: case 3: case 4: return m << 3 | r;
        case 5:
            put(g, (uint16_t)((rnd(g->s) % 0x400) - 0x200));
            return 5 << 3 | r;
        case 6:
            put(g, brief(g));
            return 6 << 3 | r;
        case 7:
            put32(g, g->scratch + 0x40000 + (rnd(g->s) % 0x1000));
            return 7 << 3 | 1;
        case 8:
            if (dest)
                continue;
            put(g, (uint16_t)(rnd(g->s) % 0x100));
            return 7 << 3 | 0;
        case 9:
            if (dest)
                continue;
            put(g, (uint16_t)(rnd(g->s) % 0x40));
            return 7 << 3 | 2;
        case 10:
            if (dest)
                continue;
            put(g, (uint16_t)(brief(g) & 0x0FFF)); /* index D0 */
            return 7 << 3 | 3;
        default:
            if (dest)
                continue;
            if (sz == 4)
                put32(g, rnd(g->s));
            else
                put(g, (uint16_t)(sz == 1 ? rnd(g->s) & 0xFF : rnd(g->s)));
            return 7 << 3 | 4;
        }
    }
}

/* A control EA (for LEA, PEA): (An), d16(An), d8(An,Xn) (PEA only, so
 * the address registers stay near the middle of the area), abs.L. */
static int control_gen(gen *g, bool indexed)
{
    int r = (int)(rnd(g->s) % 8);
    switch (rnd(g->s) % 4) {
    case 0: return 2 << 3 | r;
    case 1: put(g, (uint16_t)((rnd(g->s) % 0x400) - 0x200)); return 5 << 3 | r;
    case 2:
        if (indexed) {
            put(g, brief(g));
            return 6 << 3 | r;
        }
        return 2 << 3 | r;
    default: put32(g, g->scratch + 0x40000 + (rnd(g->s) % 0x1000)); return 7 << 3 | 1;
    }
}

/* One instruction of the kinds translated (or now and then one that
 * isn't, to end a block early), into g->w. */
static void random_insn(gen *g)
{
    uint32_t *s = g->s;
    g->n = 0;
    int dn = (int)(rnd(s) % 8), an = (int)(rnd(s) % 8);
    int sz = (int[]){ 1, 2, 4 }[rnd(s) % 3];
    switch (rnd(s) % 22) {
    case 0: { /* MOVE */
        put(g, 0);
        int src = ea_gen(g, sz, false, true);
        int dst = ea_gen(g, sz, true, false);
        g->w[0] = (uint16_t)((sz == 1 ? 0x1000 : sz == 2 ? 0x3000 : 0x2000) | (dst & 7) << 9 | (dst >> 3) << 6 | src);
        return;
    }
    case 1: put(g, (uint16_t)(0x7000 | dn << 9 | (rnd(s) & 0xFF))); return; /* MOVEQ */
    case 2: { /* LEA, PEA */
        put(g, 0);
        bool lea = rnd(s) & 1;
        int e = control_gen(g, !lea);
        g->w[0] = (uint16_t)(lea ? 0x41C0 | an << 9 | e : 0x4840 | e);
        return;
    }
    case 3: { /* CLR NEG NOT TST */
        put(g, 0);
        static const uint16_t o[] = { 0x4200, 0x4400, 0x4600, 0x4A00 };
        uint16_t base = o[rnd(s) % 4];
        int e = ea_gen(g, sz, base != 0x4A00, base == 0x4A00);
        g->w[0] = (uint16_t)(base | size_code(sz) << 6 | e);
        return;
    }
    case 4: { /* EXT.W EXT.L EXTB.L SWAP */
        static const uint16_t o[] = { 0x4880, 0x48C0, 0x49C0, 0x4840 };
        put(g, (uint16_t)(o[rnd(s) % 4] | dn));
        return;
    }
    case 5: case 6: { /* ADD SUB AND OR CMP <ea>,Dn */
        static const uint16_t l[] = { 0xD000, 0x9000, 0xC000, 0x8000, 0xB000 };
        uint16_t line = l[rnd(s) % 5];
        put(g, 0);
        int e = ea_gen(g, sz, false, line == 0xD000 || line == 0x9000 || line == 0xB000);
        g->w[0] = (uint16_t)(line | dn << 9 | size_code(sz) << 6 | e);
        return;
    }
    case 7: { /* ADD SUB AND OR EOR Dn,<ea> (memory) */
        static const uint16_t l[] = { 0xD000, 0x9000, 0xC000, 0x8000, 0xB000 };
        uint16_t line = l[rnd(s) % 5];
        put(g, 0);
        int e;
        do {
            g->n = 1;
            e = ea_gen(g, sz, true, false);
        } while ((e >> 3) == 0 && line != 0xB000);
        g->w[0] = (uint16_t)(line | dn << 9 | (4 + size_code(sz)) << 6 | e);
        return;
    }
    case 8: { /* ADDA SUBA #small; CMPA <ea> */
        int w = rnd(s) & 1;
        if (rnd(s) & 1) {
            put(g, (uint16_t)((rnd(s) & 1 ? 0xD000 : 0x9000) | an << 9 | (w ? 3 : 7) << 6 | 0x3C));
            if (w)
                put(g, (uint16_t)((rnd(s) % 0x200) - 0x100));
            else
                put32(g, (rnd(s) % 0x200) - 0x100);
        } else {
            put(g, 0);
            int e = ea_gen(g, w ? 2 : 4, false, true);
            g->w[0] = (uint16_t)(0xB000 | an << 9 | (w ? 3 : 7) << 6 | e);
        }
        return;
    }
    case 9: { /* ADDI SUBI ANDI ORI EORI CMPI */
        static const uint16_t o[] = { 0x0600, 0x0400, 0x0200, 0x0000, 0x0A00, 0x0C00 };
        uint16_t base = o[rnd(s) % 6];
        put(g, 0);
        if (sz == 4)
            put32(g, rnd(s));
        else
            put(g, (uint16_t)(sz == 1 ? rnd(s) & 0xFF : rnd(s)));
        int e = ea_gen(g, sz, true, false);
        g->w[0] = (uint16_t)(base | size_code(sz) << 6 | e);
        return;
    }
    case 10: { /* ADDQ SUBQ */
        put(g, 0);
        int e = rnd(s) % 4 == 0 && sz != 1 ? 1 << 3 | an : ea_gen(g, sz, true, false);
        g->w[0] = (uint16_t)(0x5000 | (rnd(s) % 8) << 9 | (rnd(s) & 1) << 8 | size_code(sz) << 6 | e);
        return;
    }
    case 11: { /* MULU MULS */
        put(g, 0);
        int e = ea_gen(g, 2, false, false);
        g->w[0] = (uint16_t)(0xC000 | dn << 9 | (rnd(s) & 1 ? 7 : 3) << 6 | e);
        return;
    }
    case 12: case 13: { /* LSL LSR ASR #n (and now and then ASL, which isn't translated) */
        uint16_t kind = (uint16_t[]){ 0x0108, 0x0008, 0x0000, 0x0100 }[rnd(s) % 4];
        put(g, (uint16_t)(0xE000 | (rnd(s) % 8) << 9 | kind | size_code(sz) << 6 | dn));
        return;
    }
    case 14: { /* Scc */
        put(g, 0);
        int e = ea_gen(g, 1, true, false);
        g->w[0] = (uint16_t)(0x50C0 | (rnd(s) % 16) << 8 | e);
        return;
    }
    case 15: { /* MOVEM */
        int w = rnd(s) & 1;
        put(g, 0);
        if (rnd(s) & 1) { /* registers to memory */
            put(g, (uint16_t)rnd(s));
            int e;
            switch (rnd(s) % 3) {
            case 0: e = 4 << 3 | 7; break;
            case 1: e = 2 << 3 | an; break;
            default: put(g, (uint16_t)((rnd(s) % 0x100) - 0x80)); e = 5 << 3 | an; break;
            }
            g->w[0] = (uint16_t)(0x4880 | w << 6 | e);
        } else { /* memory to data registers */
            put(g, (uint16_t)(rnd(s) & 0xFF));
            int e = rnd(s) & 1 ? 3 << 3 | an : 2 << 3 | an;
            g->w[0] = (uint16_t)(0x4C80 | w << 6 | e);
        }
        return;
    }
    case 16: /* LINK */
        put(g, (uint16_t)(0x4E50 | an));
        put(g, (uint16_t)(-(int)(rnd(s) % 64) * 2));
        return;
    case 17: /* EXG */
        put(g, (uint16_t)(rnd(s) & 1 ? 0xC140 | dn << 9 | (rnd(s) % 8) : 0xC148 | an << 9 | (rnd(s) % 8)));
        return;
    case 18: put(g, 0x4E71); return; /* NOP */
    case 19: /* ADDX: the interpreter's, run in the block */
        put(g, (uint16_t)(0xD100 | dn << 9 | size_code(sz) << 6 | (rnd(s) % 8)));
        return;
    default: /* more of the interpreter's */
        switch (rnd(s) % 5) {
        case 0: { /* BTST BCHG BCLR BSET #n,<ea> */
            put(g, 0);
            put(g, (uint16_t)(rnd(s) % 64));
            int e = ea_gen(g, 1, true, false);
            g->w[0] = (uint16_t)(0x0800 | (rnd(s) % 4) << 6 | e);
            return;
        }
        case 1: /* shifts and rotates by a register */
            put(g, (uint16_t)(0xE020 | (rnd(s) % 8) << 9 | (rnd(s) & 1) << 8 | size_code(sz) << 6 |
                              (rnd(s) % 4) << 3 | dn));
            return;
        case 2: { /* MULU.L / MULS.L <ea>,Dl */
            put(g, 0x4C00);
            put(g, (uint16_t)(dn << 12 | (rnd(s) & 1) << 11));
            int e = ea_gen(g, 4, false, false);
            g->w[0] |= (uint16_t)e;
            return;
        }
        case 3: { /* MOVE <ea>,CCR */
            put(g, 0);
            int e = ea_gen(g, 2, false, false);
            g->w[0] = (uint16_t)(0x44C0 | e);
            return;
        }
        default: /* ASL, ROXL, ROXR #n */
            put(g, (uint16_t)(0xE000 | (rnd(s) % 8) << 9 | (rnd(s) & 1) << 8 | size_code(sz) << 6 |
                              (rnd(s) & 1 ? 0 : 2) << 3 | dn));
            return;
        }
    }
}

/* Random code at code_at: in the single-block form, one block of up to 30
 * instructions closed by a branch of any kind; in the chained form, four
 * blocks 0x200 bytes apart, each closed by a Bcc, DBcc, BSR or JMP to one
 * of the four and followed by a BRA to another (for a branch not taken).
 * Returns the end of the code; *n the instructions of the single block. */
static uint32_t random_code(gen *g, uint32_t code_at, bool chained, int *n)
{
    uint32_t *s = g->s;
    uint32_t end = code_at;
    for (int blk = 0; blk < (chained ? 4 : 1); blk++) {
        uint32_t at = code_at + (uint32_t)blk * 0x200;
        int k = 1 + (int)(rnd(s) % (chained ? 20 : 30));
        for (int i = 0; i < k; i++) {
            random_insn(g);
            for (int w = 0; w < g->n; w++, at += 2)
                wr16(at, g->w[w]);
        }
        *n = k + 1;
        uint32_t to = code_at + (rnd(s) % 4) * 0x200, other = code_at + (rnd(s) % 4) * 0x200;
        int16_t disp = (int16_t)(to - (at + 2));
        switch (rnd(s) % (chained ? 4 : 6)) {
        case 0: /* Bcc.W */
            wr16(at, (uint16_t)(0x6000 | (rnd(s) % 16) << 8));
            wr16(at + 2, chained ? (uint16_t)disp : 0x40);
            at += 4;
            break;
        case 1: /* DBcc */
            wr16(at, (uint16_t)(0x50C8 | (rnd(s) % 16) << 8 | (rnd(s) % 8)));
            wr16(at + 2, chained ? (uint16_t)disp : 0x40);
            at += 4;
            break;
        case 2: /* BSR.W */
            wr16(at, 0x6100);
            wr16(at + 2, chained ? (uint16_t)disp : 0x40);
            at += 4;
            break;
        case 3: /* JMP abs.L */
            wr16(at, 0x4EF9);
            wr32(at + 2, chained ? to : code_at + 0x1000);
            at += 6;
            break;
        case 4: wr16(at, 0x4E75); at += 2; break;                    /* RTS */
        default: wr16(at, (uint16_t)(0x4ED0 | (rnd(s) % 8))); at += 2; /* JMP (An) */
        }
        if (chained) { /* BRA.W, for a branch not taken */
            wr16(at, 0x6000);
            wr16(at + 2, (uint16_t)(int16_t)(other - (at + 2)));
            at += 4;
        }
        end = at;
    }
    return end;
}

int cpu_jit_selftest(uint32_t seed, int blocks, uint32_t code_at, uint32_t scratch, char *why, int whylen)
{
    uint32_t s = seed ? seed : 1;
    int bad = 0;
    why[0] = 0;
    enum { AREA = 0x80000 };
    uint8_t *before = malloc(AREA), *after_i = malloc(AREA);
    if (!before || !after_i) {
        free(before);
        free(after_i);
        return -1;
    }
    bool was = cpu_jit_enabled, was_lookahead = lookahead;
    gen g = { &s, { 0 }, 0, scratch };
    for (int iter = 0; iter < blocks; iter++) {
        /* Odd iterations chain blocks, with the flags looked for after
         * each: N Z V C may then be stale in SR where the code that
         * follows sets them before reading them (so the final SR's are
         * not compared), and any other difference shows in what runs.
         * op_pc and op are not compared either: a chained exit doesn't
         * set them, so when the run stops at the next block's budget
         * check they still name an earlier block's last instruction. */
        bool chained = iter & 1;
        int n;
        uint32_t end = random_code(&g, code_at, chained, &n);
        uint64_t count = chained ? 50 + rnd(&s) % 250 : (uint64_t)n;

        cpu68k start;
        cpu_init(&start);
        start.model = 68040;
        for (int i = 0; i < 8; i++) {
            start.d[i] = rnd(&s);
            if (rnd(&s) % 4 == 0)
                start.d[i] = rnd(&s) % 16;
            start.a[i] = scratch + 0x40000 + (rnd(&s) % 0x2000) * 2;
        }
        start.a[7] = scratch + 0x60000;
        start.sr = (uint16_t)(0x2700 | (rnd(&s) & 0x1F));
        start.pc = code_at;
        for (uint32_t i = 0; i < AREA; i++)
            before[i] = (uint8_t)rnd(&s);

        gput(scratch, before, AREA);
        cpu68k ci = start;
        cpu_jit_enabled = false;
        cpu_run(&ci, count);
        memcpy(after_i, g_mem + scratch, AREA);

        gput(scratch, before, AREA);
        cpu68k cj = start;
        cpu_jit_enabled = true;
        lookahead = chained;
        jit_flush(&E);
        cpu_run(&cj, count);
        lookahead = was_lookahead;

        uint16_t sr_mask = chained ? 0xFFF0 : 0xFFFF;
        const char *what = NULL;
        int which = -1;
        for (int i = 0; i < 8 && !what; i++)
            if (ci.d[i] != cj.d[i]) {
                what = "d";
                which = i;
            } else if (ci.a[i] != cj.a[i]) {
                what = "a";
                which = i;
            }
        if (!what) {
            if ((ci.sr & sr_mask) != (cj.sr & sr_mask)) what = "sr";
            else if (ci.pc != cj.pc) what = "pc";
            else if (ci.icount != cj.icount) what = "icount";
            else if (!chained && ci.op_pc != cj.op_pc) what = "op_pc";
            else if (!chained && ci.op != cj.op) what = "op";
            else if (ci.halted != cj.halted) what = "halted";
            else if (memcmp(after_i, g_mem + scratch, AREA)) what = "memory";
        }
        if (what) {
            if (!bad) {
                int w = snprintf(why, (size_t)whylen, "case %d (%s, %llu instructions): %s", iter,
                                 chained ? "chained" : "one block", (unsigned long long)count, what);
                if (which >= 0)
                    w += snprintf(why + w, (size_t)(whylen - w), "%d %08X vs %08X", which,
                                  what[0] == 'd' ? ci.d[which] : ci.a[which], what[0] == 'd' ? cj.d[which] : cj.a[which]);
                else if (!strcmp(what, "sr"))
                    w += snprintf(why + w, (size_t)(whylen - w), " %04X vs %04X", ci.sr, cj.sr);
                for (uint32_t a = code_at; a < end && a < code_at + 0x80 && w < whylen - 6; a += 2)
                    w += snprintf(why + w, (size_t)(whylen - w), " %04X", rd16(a));
            }
            bad++;
        }
    }
    cpu_jit_enabled = was;
    free(before);
    free(after_i);
    return bad;
}

#endif
