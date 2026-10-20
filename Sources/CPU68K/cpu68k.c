/*
 * 68020 integer interpreter.
 *
 * Straightforward decode-and-execute; correctness first.  Condition codes are
 * computed eagerly.  Memory goes through the guest arena with the current
 * address mask (24-bit or 32-bit mode).
 */
#include "m68k.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>

/* ---------------------------------------------------------------------- */
/* Memory */

static inline uint8_t mr8(cpu68k *c, uint32_t a) { return rd8(a & c->addr_mask); }
static inline uint16_t mr16(cpu68k *c, uint32_t a) { return rd16(a & c->addr_mask); }
static inline uint32_t mr32(cpu68k *c, uint32_t a) { return rd32(a & c->addr_mask); }
static inline void mw8(cpu68k *c, uint32_t a, uint8_t v) { watch_store(a & c->addr_mask); wr8(a & c->addr_mask, v); }
static inline void mw16(cpu68k *c, uint32_t a, uint16_t v) { watch_store(a & c->addr_mask); wr16(a & c->addr_mask, v); }
static inline void mw32(cpu68k *c, uint32_t a, uint32_t v) { watch_store(a & c->addr_mask); wr32(a & c->addr_mask, v); }

static inline uint32_t mread(cpu68k *c, uint32_t a, int sz)
{
    return sz == 1 ? mr8(c, a) : sz == 2 ? mr16(c, a) : mr32(c, a);
}
static inline void mwrite(cpu68k *c, uint32_t a, int sz, uint32_t v)
{
    if (sz == 1)
        mw8(c, a, (uint8_t)v);
    else if (sz == 2)
        mw16(c, a, (uint16_t)v);
    else
        mw32(c, a, v);
}

static inline uint16_t fetch16(cpu68k *c)
{
    uint16_t v = mr16(c, c->pc);
    c->pc += 2;
    return v;
}
static inline uint32_t fetch32(cpu68k *c)
{
    uint32_t v = mr32(c, c->pc);
    c->pc += 4;
    return v;
}

/* ---------------------------------------------------------------------- */
/* Size helpers */

static inline uint32_t szmask(int sz) { return sz == 1 ? 0xFFu : sz == 2 ? 0xFFFFu : 0xFFFFFFFFu; }
static inline uint32_t szmsb(int sz) { return sz == 1 ? 0x80u : sz == 2 ? 0x8000u : 0x80000000u; }
static inline int32_t sext(uint32_t v, int sz)
{
    return sz == 1 ? (int32_t)(int8_t)v : sz == 2 ? (int32_t)(int16_t)v : (int32_t)v;
}
/* Standard size field (bits 7-6): 0=byte 1=word 2=long. */
static inline int size_from_bits(int bits) { return bits == 0 ? 1 : bits == 1 ? 2 : 4; }

/* ---------------------------------------------------------------------- */
/* Flags */

#define CCR (c->sr)
static inline void setf(cpu68k *c, uint16_t f, bool on)
{
    if (on)
        c->sr |= f;
    else
        c->sr &= (uint16_t)~f;
}

static inline void flags_logic(cpu68k *c, uint32_t r, int sz)
{
    r &= szmask(sz);
    c->sr &= (uint16_t)~(SR_N | SR_Z | SR_V | SR_C);
    if (r & szmsb(sz))
        c->sr |= SR_N;
    if (r == 0)
        c->sr |= SR_Z;
}

/* d + s (+ x); sets NZVC and X; for addx, Z is only cleared. */
static uint32_t do_add(cpu68k *c, uint32_t s, uint32_t d, int sz, int x, bool extended)
{
    uint32_t m = szmask(sz), msb = szmsb(sz);
    s &= m;
    d &= m;
    uint64_t r64 = (uint64_t)s + d + (uint32_t)x;
    uint32_t r = (uint32_t)r64 & m;
    bool carry = (r64 >> (sz * 8)) & 1;
    bool ovf = ((~(s ^ d)) & (s ^ r) & msb) != 0;
    setf(c, SR_C | SR_X, false);
    if (carry)
        c->sr |= SR_C | SR_X;
    setf(c, SR_V, ovf);
    setf(c, SR_N, (r & msb) != 0);
    if (extended) {
        if (r)
            c->sr &= (uint16_t)~SR_Z;
    } else {
        setf(c, SR_Z, r == 0);
    }
    return r;
}

/* d - s (- x); sets NZVC, and X unless cmp. */
static uint32_t do_sub(cpu68k *c, uint32_t s, uint32_t d, int sz, int x, bool extended, bool setx)
{
    uint32_t m = szmask(sz), msb = szmsb(sz);
    s &= m;
    d &= m;
    uint32_t r = (d - s - (uint32_t)x) & m;
    bool borrow = ((uint64_t)s + (uint32_t)x) > d;
    bool ovf = ((s ^ d) & (d ^ r) & msb) != 0;
    setf(c, SR_C, borrow);
    if (setx)
        setf(c, SR_X, borrow);
    setf(c, SR_V, ovf);
    setf(c, SR_N, (r & msb) != 0);
    if (extended) {
        if (r)
            c->sr &= (uint16_t)~SR_Z;
    } else {
        setf(c, SR_Z, r == 0);
    }
    return r;
}

static bool cond(cpu68k *c, int cc)
{
    bool C = c->sr & SR_C, V = c->sr & SR_V, Z = c->sr & SR_Z, N = c->sr & SR_N;
    switch (cc) {
    case 0: return true;
    case 1: return false;
    case 2: return !C && !Z;       /* HI */
    case 3: return C || Z;         /* LS */
    case 4: return !C;             /* CC */
    case 5: return C;              /* CS */
    case 6: return !Z;             /* NE */
    case 7: return Z;              /* EQ */
    case 8: return !V;             /* VC */
    case 9: return V;              /* VS */
    case 10: return !N;            /* PL */
    case 11: return N;             /* MI */
    case 12: return N == V;        /* GE */
    case 13: return N != V;        /* LT */
    case 14: return !Z && N == V;  /* GT */
    default: return Z || N != V;   /* LE */
    }
}

/* ---------------------------------------------------------------------- */
/* Status register and exceptions */

void cpu_set_sr(cpu68k *c, uint16_t sr)
{
    sr &= c->model == 68000 ? 0xA71F : 0xF71F; /* the 68000 has no T0 or M */
    bool was_s = c->sr & SR_S, is_s = sr & SR_S;
    if (was_s != is_s) {
        if (was_s) {
            c->isp = c->a[7];
            c->a[7] = c->usp;
        } else {
            c->usp = c->a[7];
            c->a[7] = c->isp;
        }
    }
    c->sr = sr;
}

void cpu_exception(cpu68k *c, int vector, uint32_t return_pc)
{
    uint16_t old = c->sr;
    cpu_set_sr(c, (uint16_t)((c->sr | SR_S) & ~SR_T));
    if (c->model != 68000)
        cpu_push16(c, (uint16_t)(vector * 4)); /* format 0 */
    cpu_push32(c, return_pc);
    cpu_push16(c, old);
    uint32_t handler = mr32(c, c->vbr + (uint32_t)vector * 4);
    if (handler == 0) {
        cpu_halt(c, "exception %d at pc %08X with no handler", vector, c->op_pc);
        return;
    }
    c->pc = handler;
}

void cpu_halt(cpu68k *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->halt_reason, sizeof c->halt_reason, fmt, ap);
    va_end(ap);
    c->halted = true;
}

static void illegal(cpu68k *c)
{
    cpu_exception(c, 4, c->op_pc);
}

static void privilege(cpu68k *c)
{
    cpu_exception(c, 8, c->op_pc);
}

/* ---------------------------------------------------------------------- */
/* Effective addresses */

enum { EA_D, EA_A, EA_M, EA_I };
typedef struct {
    int kind;
    int reg;
    uint32_t addr; /* EA_M */
    uint32_t imm;  /* EA_I */
} ea_t;

/* Index/displacement extension (brief or full format). base is the An or
 * PC value; for PC-relative the caller passes the extension word address. */
static uint32_t ea_index(cpu68k *c, uint32_t base)
{
    uint16_t ext = fetch16(c);
    int xr = (ext >> 12) & 7;
    uint32_t xv = (ext & 0x8000) ? c->a[xr] : c->d[xr];
    if (!(ext & 0x0800))
        xv = (uint32_t)(int32_t)(int16_t)xv;
    if (c->model == 68000) /* no scale, no full format */
        return base + (uint32_t)(int32_t)(int8_t)ext + xv;
    xv <<= (ext >> 9) & 3;
    if (!(ext & 0x0100))
        return base + (uint32_t)(int32_t)(int8_t)ext + xv;

    /* Full format (68020). */
    if (ext & 0x80)
        base = 0;
    if (ext & 0x40)
        xv = 0;
    uint32_t bd = 0;
    switch ((ext >> 4) & 3) {
    case 2: bd = (uint32_t)(int32_t)(int16_t)fetch16(c); break;
    case 3: bd = fetch32(c); break;
    default: break;
    }
    int iis = ext & 7;
    if (iis == 0)
        return base + bd + xv;
    uint32_t od = 0;
    switch (iis & 3) {
    case 2: od = (uint32_t)(int32_t)(int16_t)fetch16(c); break;
    case 3: od = fetch32(c); break;
    default: break;
    }
    if (ext & 0x40) /* index suppressed: memory indirect */
        return mr32(c, base + bd) + od;
    if (iis & 4) /* postindexed */
        return mr32(c, base + bd) + xv + od;
    return mr32(c, base + bd + xv) + od; /* preindexed */
}

static ea_t ea_resolve(cpu68k *c, int mode, int reg, int sz)
{
    ea_t e = { EA_M, reg, 0, 0 };
    switch (mode) {
    case 0: e.kind = EA_D; break;
    case 1: e.kind = EA_A; break;
    case 2: e.addr = c->a[reg]; break;
    case 3:
        e.addr = c->a[reg];
        c->a[reg] += (sz == 1 && reg == 7) ? 2 : (uint32_t)sz;
        break;
    case 4:
        c->a[reg] -= (sz == 1 && reg == 7) ? 2 : (uint32_t)sz;
        e.addr = c->a[reg];
        break;
    case 5: e.addr = c->a[reg] + (uint32_t)(int32_t)(int16_t)fetch16(c); break;
    case 6: e.addr = ea_index(c, c->a[reg]); break;
    case 7:
        switch (reg) {
        case 0: e.addr = (uint32_t)(int32_t)(int16_t)fetch16(c); break;
        case 1: e.addr = fetch32(c); break;
        case 2: {
            uint32_t base = c->pc;
            e.addr = base + (uint32_t)(int32_t)(int16_t)fetch16(c);
            break;
        }
        case 3: e.addr = ea_index(c, c->pc); break;
        case 4:
            e.kind = EA_I;
            if (sz == 1)
                e.imm = fetch16(c) & 0xFF;
            else if (sz == 2)
                e.imm = fetch16(c);
            else
                e.imm = fetch32(c);
            break;
        default: e.kind = -1; break;
        }
        break;
    }
    return e;
}

static uint32_t ea_get(cpu68k *c, const ea_t *e, int sz)
{
    switch (e->kind) {
    case EA_D: return c->d[e->reg] & szmask(sz);
    case EA_A: return c->a[e->reg] & szmask(sz);
    case EA_M: return mread(c, e->addr, sz);
    default: return e->imm;
    }
}

static void ea_put(cpu68k *c, const ea_t *e, int sz, uint32_t v)
{
    switch (e->kind) {
    case EA_D: {
        uint32_t m = szmask(sz);
        c->d[e->reg] = (c->d[e->reg] & ~m) | (v & m);
        break;
    }
    case EA_A: c->a[e->reg] = v; break;
    case EA_M: mwrite(c, e->addr, sz, v); break;
    default: break;
    }
}

/* Control-addressing EA (for LEA, PEA, JMP, JSR, MOVEM ...). */
static bool ea_control(cpu68k *c, int mode, int reg, uint32_t *addr)
{
    if (mode < 2 || mode == 3 || mode == 4 || (mode == 7 && reg > 3))
        return false;
    ea_t e = ea_resolve(c, mode, reg, 4);
    *addr = e.addr;
    return true;
}

/* ---------------------------------------------------------------------- */
/* Shifts and rotates */

static uint32_t do_shift(cpu68k *c, int type, bool left, uint32_t v, int count, int sz)
{
    uint32_t m = szmask(sz), msb = szmsb(sz);
    int bits = sz * 8;
    v &= m;
    bool C = false, V = false, X = (c->sr & SR_X) != 0;
    if (count == 0) {
        if (type == 2) /* ROX: C = X */
            C = X;
        setf(c, SR_C, C);
        setf(c, SR_V, false);
        setf(c, SR_N, (v & msb) != 0);
        setf(c, SR_Z, v == 0);
        return v;
    }
    switch (type) {
    case 0: /* AS */
        if (left) {
            for (int i = 0; i < count; i++) {
                C = (v & msb) != 0;
                uint32_t nv = (v << 1) & m;
                if ((nv & msb) != (v & msb))
                    V = true;
                v = nv;
            }
        } else {
            for (int i = 0; i < count; i++) {
                C = v & 1;
                v = ((v >> 1) | (v & msb)) & m;
            }
        }
        X = C;
        break;
    case 1: /* LS */
        if (left) {
            for (int i = 0; i < count; i++) {
                C = (v & msb) != 0;
                v = (v << 1) & m;
            }
        } else {
            for (int i = 0; i < count; i++) {
                C = v & 1;
                v >>= 1;
            }
        }
        X = C;
        break;
    case 2: /* ROX */
        for (int i = 0; i < count; i++) {
            if (left) {
                C = (v & msb) != 0;
                v = ((v << 1) | (X ? 1u : 0u)) & m;
            } else {
                C = v & 1;
                v = (v >> 1) | (X ? msb : 0u);
            }
            X = C;
        }
        break;
    case 3: /* RO */
        count %= bits;
        for (int i = 0; i < count; i++) {
            if (left) {
                C = (v & msb) != 0;
                v = ((v << 1) | (C ? 1u : 0u)) & m;
            } else {
                C = v & 1;
                v = (v >> 1) | (C ? msb : 0u);
            }
        }
        if (count == 0)
            C = left ? (v & 1) : ((v & msb) != 0);
        break;
    }
    if (type != 3)
        setf(c, SR_X, X);
    setf(c, SR_C, C);
    setf(c, SR_V, V);
    setf(c, SR_N, (v & msb) != 0);
    setf(c, SR_Z, v == 0);
    return v;
}

/* ---------------------------------------------------------------------- */
/* BCD */

/* Bit-exact 68000 BCD arithmetic, including invalid digits and the
 * undocumented V flag: binary add/subtract, then a decimal correction
 * derived from the binary and decimal carries out of each nibble. */
static void bcd_flags(cpu68k *c, unsigned res, bool carry, bool ovf)
{
    setf(c, SR_C | SR_X, false);
    if (carry)
        c->sr |= SR_C | SR_X;
    setf(c, SR_V, ovf);
    setf(c, SR_N, res & 0x80);
    if (res & 0xFF)
        c->sr &= (uint16_t)~SR_Z;
}

static uint8_t do_abcd(cpu68k *c, uint8_t s, uint8_t d)
{
    unsigned x = (c->sr & SR_X) ? 1 : 0;
    unsigned ss = (unsigned)s + d + x;
    unsigned bc = ((s & d) | (~ss & s) | (~ss & d)) & 0x88;
    unsigned dc = (((ss + 0x66) ^ ss) & 0x110) >> 1;
    unsigned corf = (bc | dc) - ((bc | dc) >> 2);
    unsigned res = ss + corf;
    bcd_flags(c, res, ((bc | (ss & ~res)) >> 7) & 1, ((~ss & res) >> 7) & 1);
    return (uint8_t)res;
}

static uint8_t do_sbcd(cpu68k *c, uint8_t s, uint8_t d)
{
    unsigned x = (c->sr & SR_X) ? 1 : 0;
    unsigned dd = (unsigned)d - s - x;
    unsigned bc = ((~(unsigned)d & s) | (dd & ~(unsigned)d) | (dd & s)) & 0x88;
    unsigned corf = bc - (bc >> 2);
    unsigned res = dd - corf;
    bcd_flags(c, res, ((bc | (~dd & res)) >> 7) & 1, ((dd & ~res) >> 7) & 1);
    return (uint8_t)res;
}

/* ---------------------------------------------------------------------- */
/* Bit fields (68020) */

static void op_bitfield(cpu68k *c, uint16_t op)
{
    int type = (op >> 8) & 7; /* 0 TST 1 EXTU 2 CHG 3 EXTS 4 CLR 5 FFO 6 SET 7 INS */
    int mode = (op >> 3) & 7, reg = op & 7;
    uint16_t ext = fetch16(c);
    int dreg = (ext >> 12) & 7;
    int32_t offset = (ext & 0x0800) ? (int32_t)c->d[(ext >> 6) & 7] : (int32_t)((ext >> 6) & 31);
    int width = (ext & 0x0020) ? (int)(c->d[ext & 7] & 31) : (ext & 31);
    if (width == 0)
        width = 32;

    uint32_t field;
    uint64_t data = 0;
    uint32_t addr = 0;
    int shift = 0;
    if (mode == 0) {
        offset &= 31;
        uint32_t v = c->d[reg];
        /* Field starts `offset` bits from the MSB, wrapping around. */
        uint64_t dbl = ((uint64_t)v << 32) | v;
        field = (uint32_t)(dbl >> (64 - offset - width)) & (width == 32 ? 0xFFFFFFFFu : ((1u << width) - 1));
    } else {
        if (!ea_control(c, mode, reg, &addr)) {
            illegal(c);
            return;
        }
        addr += (uint32_t)(offset >> 3);
        offset &= 7;
        for (int i = 0; i < 5; i++)
            data = (data << 8) | mr8(c, addr + (uint32_t)i);
        shift = 40 - offset - width;
        field = (uint32_t)(data >> shift) & (width == 32 ? 0xFFFFFFFFu : ((1u << width) - 1));
    }

    uint32_t wmask = width == 32 ? 0xFFFFFFFFu : ((1u << width) - 1);
    /* Flags from the original field. */
    c->sr &= (uint16_t)~(SR_N | SR_Z | SR_V | SR_C);
    if (field & (1u << (width - 1)))
        c->sr |= SR_N;
    if (field == 0)
        c->sr |= SR_Z;

    uint32_t nf = field;
    bool write = false;
    switch (type) {
    case 0: break;
    case 1: c->d[dreg] = field; break;
    case 2: nf = ~field & wmask; write = true; break;
    case 3: c->d[dreg] = (field & (1u << (width - 1))) ? (field | ~wmask) : field; break;
    case 4: nf = 0; write = true; break;
    case 5: {
        int i;
        for (i = 0; i < width; i++)
            if (field & (1u << (width - 1 - i)))
                break;
        c->d[dreg] = (uint32_t)(offset + i);
        if (mode != 0) {
            /* For memory operands FFO reports the original (unmasked) offset. */
            int32_t orig = (ext & 0x0800) ? (int32_t)c->d[(ext >> 6) & 7] : (int32_t)((ext >> 6) & 31);
            c->d[dreg] = (uint32_t)(orig + i);
        }
        break;
    }
    case 6: nf = wmask; write = true; break;
    case 7:
        nf = c->d[dreg] & wmask;
        c->sr &= (uint16_t)~(SR_N | SR_Z);
        if (nf & (1u << (width - 1)))
            c->sr |= SR_N;
        if (nf == 0)
            c->sr |= SR_Z;
        write = true;
        break;
    }
    if (!write)
        return;
    if (mode == 0) {
        uint32_t v = c->d[reg];
        for (int i = 0; i < width; i++) {
            int bit = 31 - ((offset + i) & 31);
            uint32_t b = (nf >> (width - 1 - i)) & 1;
            v = (v & ~(1u << bit)) | (b << bit);
        }
        c->d[reg] = v;
    } else {
        uint64_t fm = (uint64_t)wmask << shift;
        data = (data & ~fm) | ((uint64_t)nf << shift);
        for (int i = 4; i >= 0; i--) {
            mw8(c, addr + (uint32_t)i, (uint8_t)data);
            data >>= 8;
        }
    }
}

/* ---------------------------------------------------------------------- */
/* MOVEM */

static void op_movem(cpu68k *c, uint16_t op)
{
    bool to_regs = op & 0x0400;
    int sz = (op & 0x40) ? 4 : 2;
    int mode = (op >> 3) & 7, reg = op & 7;
    uint16_t mask = fetch16(c);
    if (!to_regs && mode == 4) {
        /* Predecrement: mask is reversed (bit 0 = A7). */
        uint32_t addr = c->a[reg];
        for (int i = 0; i < 16; i++) {
            if (mask & (1u << i)) {
                int r = 15 - i;
                uint32_t v = r < 8 ? c->d[r] : c->a[r - 8];
                addr -= (uint32_t)sz;
                mwrite(c, addr, sz, v);
            }
        }
        c->a[reg] = addr;
        return;
    }
    uint32_t addr;
    if (mode == 3) {
        addr = c->a[reg];
    } else if (!ea_control(c, mode, reg, &addr)) {
        illegal(c);
        return;
    }
    for (int i = 0; i < 16; i++) {
        if (!(mask & (1u << i)))
            continue;
        if (to_regs) {
            uint32_t v = mread(c, addr, sz);
            if (sz == 2)
                v = (uint32_t)(int32_t)(int16_t)v;
            if (i < 8)
                c->d[i] = v;
            else
                c->a[i - 8] = v;
        } else {
            mwrite(c, addr, sz, i < 8 ? c->d[i] : c->a[i - 8]);
        }
        addr += (uint32_t)sz;
    }
    if (mode == 3)
        c->a[reg] = addr;
}

/* ---------------------------------------------------------------------- */
/* Multiply / divide */

static void op_divw(cpu68k *c, uint16_t op, bool is_signed)
{
    int dn = (op >> 9) & 7;
    ea_t e = ea_resolve(c, (op >> 3) & 7, op & 7, 2);
    uint32_t src = ea_get(c, &e, 2);
    if (src == 0) {
        c->sr &= (uint16_t)~(SR_N | SR_Z | SR_V | SR_C);
        cpu_exception(c, 5, c->pc);
        return;
    }
    c->sr &= (uint16_t)~SR_C;
    if (is_signed) {
        int32_t dividend = (int32_t)c->d[dn];
        int32_t divisor = (int16_t)src;
        if (dividend == INT32_MIN && divisor == -1) {
            c->sr |= SR_V;
            return;
        }
        int32_t q = dividend / divisor, r = dividend % divisor;
        if (q < -32768 || q > 32767) {
            c->sr |= SR_V;
            return;
        }
        c->d[dn] = ((uint32_t)(uint16_t)r << 16) | (uint16_t)q;
        flags_logic(c, (uint32_t)q, 2);
    } else {
        uint32_t dividend = c->d[dn];
        uint32_t q = dividend / src, r = dividend % src;
        if (q > 0xFFFF) {
            c->sr |= SR_V;
            return;
        }
        c->d[dn] = (r << 16) | q;
        flags_logic(c, q, 2);
    }
}

static void op_mull(cpu68k *c, uint16_t op)
{
    uint16_t ext = fetch16(c);
    ea_t e = ea_resolve(c, (op >> 3) & 7, op & 7, 4);
    uint32_t src = ea_get(c, &e, 4);
    int dl = (ext >> 12) & 7, dh = ext & 7;
    bool is_signed = ext & 0x0800, quad = ext & 0x0400;
    c->sr &= (uint16_t)~(SR_N | SR_Z | SR_V | SR_C);
    if (is_signed) {
        int64_t r = (int64_t)(int32_t)src * (int64_t)(int32_t)c->d[dl];
        if (quad) {
            c->d[dl] = (uint32_t)r;
            c->d[dh] = (uint32_t)((uint64_t)r >> 32);
            setf(c, SR_N, r < 0);
            setf(c, SR_Z, r == 0);
        } else {
            c->d[dl] = (uint32_t)r;
            setf(c, SR_N, (int32_t)r < 0);
            setf(c, SR_Z, (uint32_t)r == 0);
            setf(c, SR_V, r != (int64_t)(int32_t)r);
        }
    } else {
        uint64_t r = (uint64_t)src * c->d[dl];
        if (quad) {
            c->d[dl] = (uint32_t)r;
            c->d[dh] = (uint32_t)(r >> 32);
            setf(c, SR_N, (r >> 63) & 1);
            setf(c, SR_Z, r == 0);
        } else {
            c->d[dl] = (uint32_t)r;
            setf(c, SR_N, ((uint32_t)r >> 31) & 1);
            setf(c, SR_Z, (uint32_t)r == 0);
            setf(c, SR_V, (r >> 32) != 0);
        }
    }
}

static void op_divl(cpu68k *c, uint16_t op)
{
    uint16_t ext = fetch16(c);
    ea_t e = ea_resolve(c, (op >> 3) & 7, op & 7, 4);
    uint32_t src = ea_get(c, &e, 4);
    int dq = (ext >> 12) & 7, dr = ext & 7;
    bool is_signed = ext & 0x0800, quad = ext & 0x0400;
    if (src == 0) {
        cpu_exception(c, 5, c->pc);
        return;
    }
    c->sr &= (uint16_t)~(SR_C | SR_V);
    if (is_signed) {
        int64_t dividend = quad ? (int64_t)(((uint64_t)c->d[dr] << 32) | c->d[dq]) : (int64_t)(int32_t)c->d[dq];
        int64_t divisor = (int32_t)src;
        if (divisor == -1 && dividend == INT64_MIN) {
            c->sr |= SR_V;
            return;
        }
        int64_t q = dividend / divisor, r = dividend % divisor;
        if (q != (int64_t)(int32_t)q) {
            c->sr |= SR_V;
            return;
        }
        if (dr != dq)
            c->d[dr] = (uint32_t)r;
        c->d[dq] = (uint32_t)q;
        flags_logic(c, (uint32_t)q, 4);
    } else {
        uint64_t dividend = quad ? (((uint64_t)c->d[dr] << 32) | c->d[dq]) : c->d[dq];
        uint64_t q = dividend / src, r = dividend % src;
        if (q > 0xFFFFFFFFull) {
            c->sr |= SR_V;
            return;
        }
        if (dr != dq)
            c->d[dr] = (uint32_t)r;
        c->d[dq] = (uint32_t)q;
        flags_logic(c, (uint32_t)q, 4);
    }
}

/* ---------------------------------------------------------------------- */
/* Line 0: immediates, bit ops, MOVEP */

static void op_line0(cpu68k *c, uint16_t op)
{
    int mode = (op >> 3) & 7, reg = op & 7;

    if (op & 0x0100) {
        if (mode == 1) { /* MOVEP */
            int dn = (op >> 9) & 7;
            uint32_t addr = c->a[reg] + (uint32_t)(int32_t)(int16_t)fetch16(c);
            switch ((op >> 6) & 3) {
            case 0:
                c->d[dn] = (c->d[dn] & 0xFFFF0000u) | ((uint32_t)mr8(c, addr) << 8) | mr8(c, addr + 2);
                break;
            case 1:
                c->d[dn] = ((uint32_t)mr8(c, addr) << 24) | ((uint32_t)mr8(c, addr + 2) << 16) |
                           ((uint32_t)mr8(c, addr + 4) << 8) | mr8(c, addr + 6);
                break;
            case 2:
                mw8(c, addr, (uint8_t)(c->d[dn] >> 8));
                mw8(c, addr + 2, (uint8_t)c->d[dn]);
                break;
            case 3:
                mw8(c, addr, (uint8_t)(c->d[dn] >> 24));
                mw8(c, addr + 2, (uint8_t)(c->d[dn] >> 16));
                mw8(c, addr + 4, (uint8_t)(c->d[dn] >> 8));
                mw8(c, addr + 6, (uint8_t)c->d[dn]);
                break;
            }
            return;
        }
        /* Dynamic bit operation: bit number in Dn. */
        int type = (op >> 6) & 3;
        uint32_t bit = c->d[(op >> 9) & 7];
        int sz = mode == 0 ? 4 : 1;
        ea_t e = ea_resolve(c, mode, reg, sz);
        bit &= (uint32_t)(sz * 8 - 1);
        uint32_t v = ea_get(c, &e, sz);
        setf(c, SR_Z, !(v & (1u << bit)));
        if (type == 1)
            v ^= 1u << bit;
        else if (type == 2)
            v &= ~(1u << bit);
        else if (type == 3)
            v |= 1u << bit;
        if (type)
            ea_put(c, &e, sz, v);
        return;
    }

    int kind = (op >> 9) & 7;
    if (kind == 4) { /* static bit operation */
        int type = (op >> 6) & 3;
        uint32_t bit = fetch16(c) & 0xFF;
        int sz = mode == 0 ? 4 : 1;
        ea_t e = ea_resolve(c, mode, reg, sz);
        bit &= (uint32_t)(sz * 8 - 1);
        uint32_t v = ea_get(c, &e, sz);
        setf(c, SR_Z, !(v & (1u << bit)));
        if (type == 1)
            v ^= 1u << bit;
        else if (type == 2)
            v &= ~(1u << bit);
        else if (type == 3)
            v |= 1u << bit;
        if (type)
            ea_put(c, &e, sz, v);
        return;
    }

    int szbits = (op >> 6) & 3;
    if (szbits == 3) {
        /* CHK2/CMP2 (0x00C0, 0x02C0, 0x04C0) and CAS (0x0AC0...) */
        if (kind <= 2) {
            int sz = kind == 0 ? 1 : kind == 1 ? 2 : 4;
            uint16_t ext = fetch16(c);
            uint32_t addr;
            if (!ea_control(c, mode, reg, &addr)) {
                illegal(c);
                return;
            }
            int32_t lo = sext(mread(c, addr, sz), sz), hi = sext(mread(c, addr + (uint32_t)sz, sz), sz);
            int rn = (ext >> 12) & 7;
            int32_t v = (ext & 0x8000) ? (int32_t)c->a[rn] : sext(c->d[rn], sz);
            bool z = v == lo || v == hi;
            bool out = v < lo || v > hi;
            setf(c, SR_Z, z);
            setf(c, SR_C, out);
            if (out && (ext & 0x0800))
                cpu_exception(c, 6, c->pc);
            return;
        }
        if (kind == 5 || kind == 6 || kind == 7) { /* CAS */
            int sz = kind == 5 ? 1 : kind == 6 ? 2 : 4;
            uint16_t ext = fetch16(c);
            ea_t e = ea_resolve(c, mode, reg, sz);
            uint32_t dst = ea_get(c, &e, sz);
            int dc = ext & 7, du = (ext >> 6) & 7;
            do_sub(c, c->d[dc], dst, sz, 0, false, false);
            if (c->sr & SR_Z)
                ea_put(c, &e, sz, c->d[du]);
            else
                c->d[dc] = (c->d[dc] & ~szmask(sz)) | dst;
            return;
        }
        illegal(c);
        return;
    }

    int sz = size_from_bits(szbits);
    if (mode == 7 && reg == 4) {
        /* to CCR / SR */
        uint16_t imm = fetch16(c);
        if (sz == 2 && !(c->sr & SR_S)) {
            privilege(c);
            return;
        }
        uint16_t sr = c->sr;
        if (sz == 1)
            imm = (uint16_t)((imm & 0xFF) | (sz == 1 ? 0xFF00 : 0));
        switch (kind) {
        case 0: sr |= (sz == 1) ? (imm & 0x1F) : imm; break;
        case 1: sr &= (sz == 1) ? (uint16_t)(imm | 0xFF00) : imm; break;
        case 5: sr ^= (sz == 1) ? (imm & 0x1F) : imm; break;
        default: illegal(c); return;
        }
        cpu_set_sr(c, sr);
        return;
    }

    uint32_t imm = sz == 1 ? (fetch16(c) & 0xFF) : sz == 2 ? fetch16(c) : fetch32(c);
    ea_t e = ea_resolve(c, mode, reg, sz);
    uint32_t d = ea_get(c, &e, sz), r;
    switch (kind) {
    case 0: r = d | imm; flags_logic(c, r, sz); ea_put(c, &e, sz, r); break;   /* ORI */
    case 1: r = d & imm; flags_logic(c, r, sz); ea_put(c, &e, sz, r); break;   /* ANDI */
    case 2: r = do_sub(c, imm, d, sz, 0, false, true); ea_put(c, &e, sz, r); break; /* SUBI */
    case 3: r = do_add(c, imm, d, sz, 0, false); ea_put(c, &e, sz, r); break;  /* ADDI */
    case 5: r = d ^ imm; flags_logic(c, r, sz); ea_put(c, &e, sz, r); break;   /* EORI */
    case 6: do_sub(c, imm, d, sz, 0, false, false); break;                     /* CMPI */
    case 7: /* MOVES */
        illegal(c);
        break;
    default: illegal(c); break;
    }
}

/* ---------------------------------------------------------------------- */
/* MOVE */

static void op_move(cpu68k *c, uint16_t op)
{
    int top = op >> 12;
    int sz = top == 1 ? 1 : top == 3 ? 2 : 4;
    ea_t s = ea_resolve(c, (op >> 3) & 7, op & 7, sz);
    uint32_t v = ea_get(c, &s, sz);
    int dmode = (op >> 6) & 7, dreg = (op >> 9) & 7;
    if (dmode == 1) { /* MOVEA */
        c->a[dreg] = sz == 2 ? (uint32_t)(int32_t)(int16_t)v : v;
        return;
    }
    ea_t d = ea_resolve(c, dmode, dreg, sz);
    flags_logic(c, v, sz);
    ea_put(c, &d, sz, v);
}

/* ---------------------------------------------------------------------- */
/* Line 4: miscellaneous */

static void op_line4(cpu68k *c, uint16_t op)
{
    int mode = (op >> 3) & 7, reg = op & 7;

    if ((op & 0xF1C0) == 0x41C0) { /* LEA */
        uint32_t addr;
        if (!ea_control(c, mode, reg, &addr)) {
            if (op == 0x49C0 + reg && mode == 0) { /* EXTB.L */
                c->d[reg] = (uint32_t)(int32_t)(int8_t)c->d[reg];
                flags_logic(c, c->d[reg], 4);
                return;
            }
            illegal(c);
            return;
        }
        c->a[(op >> 9) & 7] = addr;
        return;
    }
    if ((op & 0xF140) == 0x4100) { /* CHK */
        int sz = (op & 0x80) ? 2 : 4;
        ea_t e = ea_resolve(c, mode, reg, sz);
        int32_t bound = sext(ea_get(c, &e, sz), sz);
        int32_t v = sext(c->d[(op >> 9) & 7], sz);
        /* Undocumented but consistent: V and C cleared, Z = (Dn == 0). */
        c->sr &= (uint16_t)~(SR_Z | SR_V | SR_C);
        if (v == 0)
            c->sr |= SR_Z;
        if (v < 0) {
            c->sr |= SR_N;
            cpu_exception(c, 6, c->pc);
        } else if (v > bound) {
            c->sr &= (uint16_t)~SR_N;
            cpu_exception(c, 6, c->pc);
        }
        return;
    }

    switch (op & 0x0FC0) {
    case 0x00C0: { /* MOVE from SR (privileged from the 68010 on) */
        if (c->model != 68000 && !(c->sr & SR_S)) {
            privilege(c);
            return;
        }
        ea_t e = ea_resolve(c, mode, reg, 2);
        ea_put(c, &e, 2, c->sr);
        return;
    }
    case 0x02C0: { /* MOVE from CCR */
        ea_t e = ea_resolve(c, mode, reg, 2);
        ea_put(c, &e, 2, c->sr & 0x1F);
        return;
    }
    case 0x04C0: { /* MOVE to CCR */
        ea_t e = ea_resolve(c, mode, reg, 2);
        c->sr = (uint16_t)((c->sr & 0xFF00) | (ea_get(c, &e, 2) & 0x1F));
        return;
    }
    case 0x06C0: { /* MOVE to SR */
        if (!(c->sr & SR_S)) {
            privilege(c);
            return;
        }
        ea_t e = ea_resolve(c, mode, reg, 2);
        cpu_set_sr(c, (uint16_t)ea_get(c, &e, 2));
        return;
    }
    case 0x0800: /* NBCD / LINK.L */
        if (mode == 1) {
            int32_t disp = (int32_t)fetch32(c);
            cpu_push32(c, reg == 7 ? c->a[7] - 4 : c->a[reg]);
            c->a[reg] = c->a[7];
            c->a[7] += (uint32_t)disp;
            return;
        } else {
            ea_t e = ea_resolve(c, mode, reg, 1);
            uint8_t v = (uint8_t)ea_get(c, &e, 1);
            ea_put(c, &e, 1, do_sbcd(c, v, 0));
            return;
        }
    case 0x0840: /* SWAP / PEA / BKPT */
        if (mode == 0) {
            c->d[reg] = (c->d[reg] >> 16) | (c->d[reg] << 16);
            flags_logic(c, c->d[reg], 4);
            return;
        }
        if (mode == 1) {
            illegal(c);
            return;
        } else {
            uint32_t addr;
            if (!ea_control(c, mode, reg, &addr)) {
                illegal(c);
                return;
            }
            cpu_push32(c, addr);
            return;
        }
    case 0x0880:
    case 0x08C0:
        if (mode == 0) { /* EXT.W / EXT.L */
            if (op & 0x40)
                c->d[reg] = (uint32_t)(int32_t)(int16_t)c->d[reg];
            else
                c->d[reg] = (c->d[reg] & 0xFFFF0000u) | (uint16_t)(int16_t)(int8_t)c->d[reg];
            flags_logic(c, c->d[reg], (op & 0x40) ? 4 : 2);
            return;
        }
        op_movem(c, op);
        return;
    case 0x0C80:
    case 0x0CC0:
        op_movem(c, op);
        return;
    case 0x0AC0: /* TAS / ILLEGAL */
        if (op == 0x4AFC) {
            illegal(c);
            return;
        } else {
            ea_t e = ea_resolve(c, mode, reg, 1);
            uint32_t v = ea_get(c, &e, 1);
            flags_logic(c, v, 1);
            ea_put(c, &e, 1, v | 0x80);
            return;
        }
    case 0x0C00: op_mull(c, op); return;
    case 0x0C40: op_divl(c, op); return;
    case 0x0E80: { /* JSR */
        uint32_t addr;
        if (!ea_control(c, mode, reg, &addr)) {
            illegal(c);
            return;
        }
        cpu_push32(c, c->pc);
        c->pc = addr;
        return;
    }
    case 0x0EC0: { /* JMP */
        uint32_t addr;
        if (!ea_control(c, mode, reg, &addr)) {
            illegal(c);
            return;
        }
        c->pc = addr;
        return;
    }
    default: break;
    }

    if ((op & 0xFFF0) == 0x4E40) { /* TRAP #n */
        cpu_exception(c, 32 + (op & 15), c->pc);
        return;
    }
    if ((op & 0xFFF8) == 0x4E50) { /* LINK.W */
        int16_t disp = (int16_t)fetch16(c);
        cpu_push32(c, reg == 7 ? c->a[7] - 4 : c->a[reg]);
        c->a[reg] = c->a[7];
        c->a[7] += (uint32_t)(int32_t)disp;
        return;
    }
    if ((op & 0xFFF8) == 0x4E58) { /* UNLK */
        c->a[7] = c->a[reg];
        c->a[reg] = cpu_pop32(c);
        return;
    }
    if ((op & 0xFFF0) == 0x4E60) { /* MOVE USP */
        if (!(c->sr & SR_S)) {
            privilege(c);
            return;
        }
        if (op & 8)
            c->a[reg] = c->usp;
        else
            c->usp = c->a[reg];
        return;
    }
    switch (op) {
    case 0x4E70: /* RESET */
        if (!(c->sr & SR_S))
            privilege(c);
        return;
    case 0x4E71: return; /* NOP */
    case 0x4E72: {       /* STOP */
        uint16_t sr = fetch16(c);
        if (!(c->sr & SR_S)) {
            privilege(c);
            return;
        }
        cpu_set_sr(c, sr);
        c->stopped = true;
        return;
    }
    case 0x4E73: { /* RTE */
        if (!(c->sr & SR_S)) {
            privilege(c);
            return;
        }
        uint16_t sr = cpu_pop16(c);
        uint32_t pc = cpu_pop32(c);
        uint16_t fmt = c->model == 68000 ? 0 : cpu_pop16(c);
        switch (fmt >> 12) {
        case 0: break;
        case 2: c->a[7] += 4; break;
        default: break;
        }
        cpu_set_sr(c, sr);
        c->pc = pc;
        return;
    }
    case 0x4E74: { /* RTD */
        int16_t disp = (int16_t)fetch16(c);
        c->pc = cpu_pop32(c);
        c->a[7] += (uint32_t)(int32_t)disp;
        return;
    }
    case 0x4E75: c->pc = cpu_pop32(c); return; /* RTS */
    case 0x4E76:                               /* TRAPV */
        if (c->sr & SR_V)
            cpu_exception(c, 7, c->pc);
        return;
    case 0x4E77: { /* RTR */
        uint16_t ccr = cpu_pop16(c);
        c->sr = (uint16_t)((c->sr & 0xFF00) | (ccr & 0x1F));
        c->pc = cpu_pop32(c);
        return;
    }
    case 0x4E7A:
    case 0x4E7B: { /* MOVEC */
        uint16_t ext = fetch16(c);
        if (!(c->sr & SR_S)) {
            privilege(c);
            return;
        }
        int rn = (ext >> 12) & 15;
        uint32_t *r = rn < 8 ? &c->d[rn] : &c->a[rn - 8];
        uint32_t *cr = NULL;
        switch (ext & 0xFFF) {
        case 0x000: cr = &c->sfc; break;
        case 0x001: cr = &c->dfc; break;
        case 0x002: cr = &c->cacr; break;
        case 0x800: cr = &c->usp; break;
        case 0x801: cr = &c->vbr; break;
        case 0x802: cr = &c->caar; break;
        case 0x803: /* MSP */
        case 0x804: cr = &c->isp; break;
        default:
            /* The 68040's MMU and cache registers: TC, ITT0/1, DTT0/1,
             * MMUSR, URP, SRP.  There is no MMU; they read back what was
             * written. */
            if (c->model >= 68040 && ((ext & 0xFFF) - 0x003 < 5 || (ext & 0xFFF) - 0x805 < 3)) {
                static uint32_t mmu_regs[16];
                cr = &mmu_regs[(ext & 0x800 ? 8 : 0) + (ext & 7)];
                break;
            }
            illegal(c);
            return;
        }
        if (op == 0x4E7A)
            *r = *cr;
        else
            *cr = *r;
        return;
    }
    default: break;
    }

    /* NEGX / CLR / NEG / NOT / TST */
    int szbits = (op >> 6) & 3;
    if (szbits == 3) {
        illegal(c);
        return;
    }
    int sz = size_from_bits(szbits);
    switch (op & 0x0F00) {
    case 0x0000: { /* NEGX */
        ea_t e = ea_resolve(c, mode, reg, sz);
        uint32_t v = ea_get(c, &e, sz);
        ea_put(c, &e, sz, do_sub(c, v, 0, sz, (c->sr & SR_X) ? 1 : 0, true, true));
        return;
    }
    case 0x0200: { /* CLR */
        ea_t e = ea_resolve(c, mode, reg, sz);
        ea_put(c, &e, sz, 0);
        flags_logic(c, 0, sz);
        return;
    }
    case 0x0400: { /* NEG */
        ea_t e = ea_resolve(c, mode, reg, sz);
        uint32_t v = ea_get(c, &e, sz);
        ea_put(c, &e, sz, do_sub(c, v, 0, sz, 0, false, true));
        return;
    }
    case 0x0600: { /* NOT */
        ea_t e = ea_resolve(c, mode, reg, sz);
        uint32_t v = ~ea_get(c, &e, sz);
        flags_logic(c, v, sz);
        ea_put(c, &e, sz, v);
        return;
    }
    case 0x0A00: { /* TST */
        ea_t e = ea_resolve(c, mode, reg, sz);
        flags_logic(c, ea_get(c, &e, sz), sz);
        return;
    }
    default: illegal(c); return;
    }
}

/* ---------------------------------------------------------------------- */
/* Line 5: ADDQ/SUBQ/Scc/DBcc/TRAPcc */

static void op_line5(cpu68k *c, uint16_t op)
{
    int mode = (op >> 3) & 7, reg = op & 7;
    if (((op >> 6) & 3) == 3) {
        int cc = (op >> 8) & 15;
        if (mode == 1) { /* DBcc */
            uint32_t base = c->pc;
            int16_t disp = (int16_t)fetch16(c);
            if (!cond(c, cc)) {
                uint16_t cnt = (uint16_t)(c->d[reg] - 1);
                c->d[reg] = (c->d[reg] & 0xFFFF0000u) | cnt;
                if (cnt != 0xFFFF)
                    c->pc = base + (uint32_t)(int32_t)disp;
            }
            return;
        }
        if (mode == 7 && reg >= 2) { /* TRAPcc */
            if (reg == 2)
                c->pc += 2;
            else if (reg == 3)
                c->pc += 4;
            if (cond(c, cc))
                cpu_exception(c, 7, c->pc);
            return;
        }
        ea_t e = ea_resolve(c, mode, reg, 1); /* Scc */
        ea_put(c, &e, 1, cond(c, cc) ? 0xFF : 0);
        return;
    }
    int sz = size_from_bits((op >> 6) & 3);
    uint32_t q = (op >> 9) & 7;
    if (q == 0)
        q = 8;
    if (mode == 1) { /* address register: whole register, no flags */
        if (op & 0x0100)
            c->a[reg] -= q;
        else
            c->a[reg] += q;
        return;
    }
    ea_t e = ea_resolve(c, mode, reg, sz);
    uint32_t d = ea_get(c, &e, sz);
    ea_put(c, &e, sz, (op & 0x0100) ? do_sub(c, q, d, sz, 0, false, true) : do_add(c, q, d, sz, 0, false));
}

/* ---------------------------------------------------------------------- */
/* Line 6: branches */

static void op_line6(cpu68k *c, uint16_t op)
{
    int cc = (op >> 8) & 15;
    uint32_t base = c->pc;
    int32_t disp = (int8_t)(op & 0xFF);
    if (disp == 0)
        disp = (int16_t)fetch16(c);
    else if (disp == -1 && c->model != 68000)
        disp = (int32_t)fetch32(c);
    if (cc == 1) { /* BSR */
        cpu_push32(c, c->pc);
        c->pc = base + (uint32_t)disp;
        return;
    }
    if (cond(c, cc))
        c->pc = base + (uint32_t)disp;
}

/* ---------------------------------------------------------------------- */
/* Lines 8, 9, B, C, D: arithmetic and logic */

static void op_arith(cpu68k *c, uint16_t op)
{
    int line = op >> 12;
    int rn = (op >> 9) & 7, mode = (op >> 3) & 7, reg = op & 7;
    int opmode = (op >> 6) & 7;

    if (opmode == 3 || opmode == 7) {
        int sz = opmode == 3 ? 2 : 4;
        switch (line) {
        case 0x8: op_divw(c, op, opmode == 7); return;
        case 0xC: { /* MULU / MULS .W */
            ea_t e = ea_resolve(c, mode, reg, 2);
            uint32_t s = ea_get(c, &e, 2);
            uint32_t r = opmode == 7 ? (uint32_t)((int32_t)(int16_t)s * (int32_t)(int16_t)c->d[rn])
                                     : (s & 0xFFFF) * (c->d[rn] & 0xFFFF);
            c->d[rn] = r;
            flags_logic(c, r, 4);
            return;
        }
        default: { /* ADDA / SUBA / CMPA */
            ea_t e = ea_resolve(c, mode, reg, sz);
            uint32_t s = ea_get(c, &e, sz);
            if (sz == 2)
                s = (uint32_t)(int32_t)(int16_t)s;
            if (line == 0xD)
                c->a[rn] += s;
            else if (line == 0x9)
                c->a[rn] -= s;
            else
                do_sub(c, s, c->a[rn], 4, 0, false, false);
            return;
        }
        }
    }

    int sz = size_from_bits(opmode & 3);
    bool to_ea = opmode & 4;

    /* Register-to-register / memory-to-memory special forms. */
    if (to_ea && mode <= 1) {
        switch (line) {
        case 0x8: /* SBCD / PACK / UNPK */
        case 0xC: /* ABCD / EXG */
            if ((opmode & 3) == 0) {
                uint8_t s, d;
                if (mode == 0) {
                    s = (uint8_t)c->d[reg];
                    d = (uint8_t)c->d[rn];
                    uint8_t r = line == 0xC ? do_abcd(c, s, d) : do_sbcd(c, s, d);
                    c->d[rn] = (c->d[rn] & ~0xFFu) | r;
                } else {
                    c->a[reg] -= reg == 7 ? 2 : 1;
                    s = mr8(c, c->a[reg]);
                    c->a[rn] -= rn == 7 ? 2 : 1;
                    d = mr8(c, c->a[rn]);
                    mw8(c, c->a[rn], line == 0xC ? do_abcd(c, s, d) : do_sbcd(c, s, d));
                }
                return;
            }
            if (line == 0xC) { /* EXG */
                uint32_t t;
                if (opmode == 5 && mode == 0) {
                    t = c->d[rn]; c->d[rn] = c->d[reg]; c->d[reg] = t;
                } else if (opmode == 5 && mode == 1) {
                    t = c->a[rn]; c->a[rn] = c->a[reg]; c->a[reg] = t;
                } else if (opmode == 6 && mode == 1) {
                    t = c->d[rn]; c->d[rn] = c->a[reg]; c->a[reg] = t;
                } else {
                    illegal(c);
                }
                return;
            }
            /* PACK (opmode 5) / UNPK (opmode 6) */
            if (opmode == 5 || opmode == 6) {
                uint16_t adj = fetch16(c);
                if (opmode == 5) {
                    uint16_t src;
                    if (mode == 0) {
                        src = (uint16_t)(c->d[reg] + adj);
                        c->d[rn] = (c->d[rn] & ~0xFFu) | (uint8_t)(((src >> 4) & 0xF0) | (src & 0xF));
                    } else {
                        c->a[reg] -= 2;
                        src = (uint16_t)(((uint16_t)mr8(c, c->a[reg]) << 8) | mr8(c, c->a[reg] + 1));
                        src = (uint16_t)(src + adj);
                        c->a[rn] -= rn == 7 ? 2 : 1;
                        mw8(c, c->a[rn], (uint8_t)(((src >> 4) & 0xF0) | (src & 0xF)));
                    }
                } else {
                    uint8_t src;
                    if (mode == 0) {
                        src = (uint8_t)c->d[reg];
                        uint16_t r = (uint16_t)((((src & 0xF0) << 4) | (src & 0xF)) + adj);
                        c->d[rn] = (c->d[rn] & ~0xFFFFu) | r;
                    } else {
                        c->a[reg] -= reg == 7 ? 2 : 1;
                        src = mr8(c, c->a[reg]);
                        uint16_t r = (uint16_t)((((src & 0xF0) << 4) | (src & 0xF)) + adj);
                        c->a[rn] -= 2;
                        mw16(c, c->a[rn], r);
                    }
                }
                return;
            }
            break;
        case 0x9: /* SUBX */
        case 0xD: /* ADDX */
        {
            int x = (c->sr & SR_X) ? 1 : 0;
            if (mode == 0) {
                uint32_t r = line == 0xD ? do_add(c, c->d[reg], c->d[rn], sz, x, true)
                                         : do_sub(c, c->d[reg], c->d[rn], sz, x, true, true);
                c->d[rn] = (c->d[rn] & ~szmask(sz)) | r;
            } else {
                c->a[reg] -= (sz == 1 && reg == 7) ? 2 : (uint32_t)sz;
                uint32_t s = mread(c, c->a[reg], sz);
                c->a[rn] -= (sz == 1 && rn == 7) ? 2 : (uint32_t)sz;
                uint32_t d = mread(c, c->a[rn], sz);
                uint32_t r = line == 0xD ? do_add(c, s, d, sz, x, true) : do_sub(c, s, d, sz, x, true, true);
                mwrite(c, c->a[rn], sz, r);
            }
            return;
        }
        case 0xB: /* CMPM (mode 1) / EOR to Dn (mode 0) */
            if (mode == 1) {
                uint32_t s = mread(c, c->a[reg], sz);
                c->a[reg] += (sz == 1 && reg == 7) ? 2 : (uint32_t)sz;
                uint32_t d = mread(c, c->a[rn], sz);
                c->a[rn] += (sz == 1 && rn == 7) ? 2 : (uint32_t)sz;
                do_sub(c, s, d, sz, 0, false, false);
                return;
            }
            break;
        default: break;
        }
    }

    if (line == 0xB) {
        if (to_ea) { /* EOR Dn,<ea> */
            ea_t e = ea_resolve(c, mode, reg, sz);
            uint32_t r = ea_get(c, &e, sz) ^ c->d[rn];
            flags_logic(c, r, sz);
            ea_put(c, &e, sz, r);
        } else { /* CMP <ea>,Dn */
            ea_t e = ea_resolve(c, mode, reg, sz);
            do_sub(c, ea_get(c, &e, sz), c->d[rn], sz, 0, false, false);
        }
        return;
    }

    ea_t e = ea_resolve(c, mode, reg, sz);
    uint32_t s, d, r;
    if (to_ea) {
        s = c->d[rn];
        d = ea_get(c, &e, sz);
    } else {
        s = ea_get(c, &e, sz);
        d = c->d[rn];
    }
    switch (line) {
    case 0x8: r = s | d; flags_logic(c, r, sz); break;
    case 0xC: r = s & d; flags_logic(c, r, sz); break;
    case 0x9: r = do_sub(c, s, d, sz, 0, false, true); break;
    default: r = do_add(c, s, d, sz, 0, false); break;
    }
    if (to_ea)
        ea_put(c, &e, sz, r);
    else
        c->d[rn] = (c->d[rn] & ~szmask(sz)) | (r & szmask(sz));
}

/* ---------------------------------------------------------------------- */
/* Line E: shifts and bit fields */

static void op_lineE(cpu68k *c, uint16_t op)
{
    if ((op & 0xF8C0) == 0xE8C0) {
        op_bitfield(c, op);
        return;
    }
    bool left = op & 0x0100;
    if (((op >> 6) & 3) == 3) { /* memory shift, word, by 1 */
        int type = (op >> 9) & 3;
        ea_t e = ea_resolve(c, (op >> 3) & 7, op & 7, 2);
        uint32_t v = ea_get(c, &e, 2);
        ea_put(c, &e, 2, do_shift(c, type, left, v, 1, 2));
        return;
    }
    int sz = size_from_bits((op >> 6) & 3);
    int type = (op >> 3) & 3;
    int reg = op & 7;
    int cnt = (op >> 9) & 7;
    if (op & 0x20)
        cnt = (int)(c->d[cnt] & 63);
    else if (cnt == 0)
        cnt = 8;
    uint32_t r = do_shift(c, type, left, c->d[reg], cnt, sz);
    c->d[reg] = (c->d[reg] & ~szmask(sz)) | r;
}

/* ---------------------------------------------------------------------- */
/* Main loop */

/* ---------------------------------------------------------------------- */
/* 68881 floating point (coprocessor 1), in doubles.  Gestalt reports no
 * FPU, so applications that check use SANE; this is for the ones that
 * assume a coprocessor.  Packed decimal operands are not supported. */

enum { FPCC_N = 1u << 27, FPCC_Z = 1u << 26, FPCC_I = 1u << 25, FPCC_NAN = 1u << 24 };

static double ext_read(cpu68k *c, uint32_t a)
{
    uint16_t se = mr16(c, a);
    uint64_t m = (uint64_t)mr32(c, a + 4) << 32 | mr32(c, a + 8);
    int e = se & 0x7FFF;
    double v = e == 0x7FFF ? ((m << 1) ? NAN : INFINITY) : ldexp((double)m, e - 16383 - 63);
    return (se & 0x8000) ? -v : v;
}

static void ext_write(cpu68k *c, uint32_t a, double v)
{
    uint16_t se = signbit(v) ? 0x8000 : 0;
    uint64_t m = 0;
    if (isnan(v)) {
        se |= 0x7FFF;
        m = 0xC000000000000000ull;
    } else if (isinf(v)) {
        se |= 0x7FFF;
    } else if (v != 0) {
        int e;
        double f = frexp(fabs(v), &e);
        m = (uint64_t)ldexp(f, 64);
        se |= (uint16_t)(e - 1 + 16383);
    }
    mw16(c, a, se);
    mw16(c, a + 2, 0);
    mw32(c, a + 4, (uint32_t)(m >> 32));
    mw32(c, a + 8, (uint32_t)m);
}

static const int fmt_size[8] = { 4, 4, 12, 12, 2, 8, 1, 0 };

/* An operand of format fmt (L S X P W D B) from an effective address. */
static bool fp_load(cpu68k *c, int mode, int reg, int fmt, double *out)
{
    int sz = fmt_size[fmt];
    if (fmt == 3 || !sz)
        return false;
    if (mode == 0) { /* Dn: integer and single formats only */
        uint32_t v = c->d[reg];
        switch (fmt) {
        case 0: *out = (int32_t)v; return true;
        case 1: { float f; memcpy(&f, &v, 4); *out = f; return true; }
        case 4: *out = (int16_t)v; return true;
        case 6: *out = (int8_t)v; return true;
        default: return false;
        }
    }
    uint32_t a;
    if (mode == 7 && reg == 4) { /* immediate: the operand follows */
        a = c->pc;
        c->pc += (uint32_t)(sz == 1 ? 2 : sz);
        if (sz == 1)
            a += 1;
    } else {
        ea_t e = ea_resolve(c, mode, reg, sz > 4 ? 4 : sz);
        if (e.kind != EA_M)
            return false;
        a = e.addr;
        if (sz > 4 && mode == 3)
            c->a[reg] += (uint32_t)sz - 4;
        if (sz > 4 && mode == 4) {
            c->a[reg] -= (uint32_t)sz - 4;
            a = c->a[reg];
        }
    }
    switch (fmt) {
    case 0: *out = (int32_t)mr32(c, a); break;
    case 1: { uint32_t v = mr32(c, a); float f; memcpy(&f, &v, 4); *out = f; break; }
    case 2: *out = ext_read(c, a); break;
    case 4: *out = (int16_t)mr16(c, a); break;
    case 5: { uint64_t v = (uint64_t)mr32(c, a) << 32 | mr32(c, a + 4); memcpy(out, &v, 8); break; }
    default: *out = (int8_t)mr8(c, a); break;
    }
    return true;
}

static int32_t fp_to_int(cpu68k *c, double v, double lo, double hi)
{
    if (isnan(v))
        return (int32_t)hi;
    int mode = (c->fpcr >> 4) & 3; /* rounding: nearest, zero, minus, plus */
    double r = mode == 1 ? trunc(v) : mode == 2 ? floor(v) : mode == 3 ? ceil(v) : nearbyint(v);
    if (r < lo)
        return (int32_t)lo;
    if (r > hi)
        return (int32_t)hi;
    return (int32_t)r;
}

static bool fp_store(cpu68k *c, int mode, int reg, int fmt, double v)
{
    int sz = fmt_size[fmt];
    if (fmt == 3 || !sz)
        return false;
    if (mode == 0) {
        switch (fmt) {
        case 0: c->d[reg] = (uint32_t)fp_to_int(c, v, -2147483648.0, 2147483647.0); return true;
        case 1: { float f = (float)v; memcpy(&c->d[reg], &f, 4); return true; }
        case 4: c->d[reg] = (c->d[reg] & 0xFFFF0000u) | (uint16_t)fp_to_int(c, v, -32768, 32767); return true;
        case 6: c->d[reg] = (c->d[reg] & 0xFFFFFF00u) | (uint8_t)fp_to_int(c, v, -128, 127); return true;
        default: return false;
        }
    }
    ea_t e = ea_resolve(c, mode, reg, sz > 4 ? 4 : sz);
    if (e.kind != EA_M)
        return false;
    uint32_t a = e.addr;
    if (sz > 4 && mode == 3)
        c->a[reg] += (uint32_t)sz - 4;
    if (sz > 4 && mode == 4) {
        c->a[reg] -= (uint32_t)sz - 4;
        a = c->a[reg];
    }
    switch (fmt) {
    case 0: mw32(c, a, (uint32_t)fp_to_int(c, v, -2147483648.0, 2147483647.0)); break;
    case 1: { float f = (float)v; uint32_t b; memcpy(&b, &f, 4); mw32(c, a, b); break; }
    case 2: ext_write(c, a, v); break;
    case 4: mw16(c, a, (uint16_t)fp_to_int(c, v, -32768, 32767)); break;
    case 5: { uint64_t b; memcpy(&b, &v, 8); mw32(c, a, (uint32_t)(b >> 32)); mw32(c, a + 4, (uint32_t)b); break; }
    default: mw8(c, a, (uint8_t)fp_to_int(c, v, -128, 127)); break;
    }
    return true;
}

static void fp_setcc(cpu68k *c, double v)
{
    uint32_t cc = 0;
    if (isnan(v))
        cc |= FPCC_NAN;
    else {
        if (v == 0)
            cc |= FPCC_Z;
        if (isinf(v))
            cc |= FPCC_I;
    }
    if (signbit(v))
        cc |= FPCC_N;
    c->fpsr = (c->fpsr & 0x00FFFFFFu) | cc;
}

static bool fp_cond(cpu68k *c, int cc)
{
    bool N = c->fpsr & FPCC_N, Z = c->fpsr & FPCC_Z, U = c->fpsr & FPCC_NAN;
    switch (cc & 15) {
    case 0: return false;
    case 1: return Z;
    case 2: return !(U || Z || N);
    case 3: return Z || !(U || N);
    case 4: return N && !(U || Z);
    case 5: return Z || (N && !U);
    case 6: return !(U || Z);
    case 7: return !U;
    case 8: return U;
    case 9: return U || Z;
    case 10: return U || !(N || Z);
    case 11: return U || Z || !N;
    case 12: return U || (N && !Z);
    case 13: return U || Z || N;
    case 14: return !Z;
    default: return true;
    }
}

/* The arithmetic of a general instruction's opmode; false if unknown. */
static bool fp_arith(cpu68k *c, int opmode, double src, int dst)
{
    double d = c->fp[dst], r;
    int op = opmode & 0x7F;
    if (op >= 0x40) { /* 68040 single (FS) and double (FD) rounding forms */
        switch (op) {
        case 0x40: case 0x44: op = 0x00; break; /* FMOVE */
        case 0x41: case 0x45: op = 0x04; break; /* FSQRT */
        case 0x58: case 0x5C: op = 0x18; break; /* FABS */
        case 0x5A: case 0x5E: op = 0x1A; break; /* FNEG */
        case 0x60: case 0x64: op = 0x20; break; /* FDIV */
        case 0x62: case 0x66: op = 0x22; break; /* FADD */
        case 0x63: case 0x67: op = 0x23; break; /* FMUL */
        case 0x68: case 0x6C: op = 0x28; break; /* FSUB */
        default: return false;
        }
    }
    switch (op) {
    case 0x00: r = src; break;                         /* FMOVE */
    case 0x01: r = nearbyint(src); break;              /* FINT */
    case 0x02: r = sinh(src); break;
    case 0x03: r = trunc(src); break;                  /* FINTRZ */
    case 0x04: r = sqrt(src); break;
    case 0x06: r = log1p(src); break;
    case 0x08: r = expm1(src); break;
    case 0x09: r = tanh(src); break;
    case 0x0A: r = atan(src); break;
    case 0x0C: r = asin(src); break;
    case 0x0D: r = atanh(src); break;
    case 0x0E: r = sin(src); break;
    case 0x0F: r = tan(src); break;
    case 0x10: r = exp(src); break;
    case 0x11: r = exp2(src); break;
    case 0x12: r = pow(10, src); break;
    case 0x14: r = log(src); break;
    case 0x15: r = log10(src); break;
    case 0x16: r = log2(src); break;
    case 0x18: r = fabs(src); break;
    case 0x19: r = cosh(src); break;
    case 0x1A: r = -src; break;
    case 0x1C: r = acos(src); break;
    case 0x1D: r = cos(src); break;
    case 0x1E: r = src == 0 || isinf(src) || isnan(src) ? src : (double)ilogb(src); break; /* FGETEXP */
    case 0x1F: { int e; r = src == 0 || isinf(src) || isnan(src) ? src : frexp(src, &e) * 2; break; } /* FGETMAN */
    case 0x20: r = d / src; break;
    case 0x21: r = fmod(d, src); break;
    case 0x22: r = d + src; break;
    case 0x23: r = d * src; break;
    case 0x24: r = (float)(d / src); break;
    case 0x25: r = remainder(d, src); break;
    case 0x26: r = ldexp(d, (int)src); break;
    case 0x27: r = (float)d * (float)src; break;
    case 0x28: r = d - src; break;
    case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37:
        c->fp[op & 7] = cos(src); /* FSINCOS: cosine to FPc, sine to FPs */
        r = sin(src);
        break;
    case 0x38: fp_setcc(c, d - src); return true;     /* FCMP */
    case 0x3A: fp_setcc(c, src); return true;         /* FTST */
    default: return false;
    }
    if ((opmode & 0x44) == 0x40) /* FSxxx: round to single */
        r = (float)r;
    c->fp[dst] = r;
    fp_setcc(c, r);
    return true;
}

static void fp_general(cpu68k *c, uint16_t op)
{
    uint16_t w = fetch16(c);
    int mode = (op >> 3) & 7, reg = op & 7;
    int opclass = w >> 13, rx = (w >> 10) & 7, ry = (w >> 7) & 7;
    switch (opclass) {
    case 0: /* FPm to FPn */
        if (!fp_arith(c, w & 0x7F, c->fp[rx], ry))
            cpu_exception(c, 11, c->op_pc);
        return;
    case 2: { /* <ea> to FPn */
        double src;
        if (rx == 7) { /* FMOVECR: constants from the ROM */
            static const struct { int off; double v; } k[] = {
                { 0x00, M_PI }, { 0x0B, 0.30102999566398120 }, { 0x0C, M_E }, { 0x0D, M_LOG2E }, { 0x0E, M_LOG10E },
                { 0x0F, 0.0 }, { 0x30, M_LN2 }, { 0x31, M_LN10 }, { 0x32, 1e0 }, { 0x33, 1e1 }, { 0x34, 1e2 },
                { 0x35, 1e4 }, { 0x36, 1e8 }, { 0x37, 1e16 }, { 0x38, 1e32 }, { 0x39, 1e64 }, { 0x3A, 1e128 },
                { 0x3B, 1e256 }, { 0x3C, INFINITY }, { 0x3D, INFINITY }, { 0x3E, INFINITY }, { 0x3F, INFINITY },
            };
            double v = 0;
            for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++)
                if (k[i].off == (w & 0x7F))
                    v = k[i].v;
            c->fp[ry] = v;
            fp_setcc(c, v);
            return;
        }
        if (!fp_load(c, mode, reg, rx, &src) || !fp_arith(c, w & 0x7F, src, ry))
            cpu_exception(c, 11, c->op_pc);
        return;
    }
    case 3: /* FMOVE FPn to <ea> */
        if (!fp_store(c, mode, reg, rx, c->fp[ry]))
            cpu_exception(c, 11, c->op_pc);
        return;
    case 4:
    case 5: { /* FMOVE(M) control registers */
        uint32_t *regs[3] = { &c->fpcr, &c->fpsr, &c->fpiar };
        int sel[3] = { 4, 2, 1 }, n = 0;
        for (int i = 0; i < 3; i++)
            n += (rx & sel[i]) != 0;
        bool to_mem = opclass == 5;
        if (mode == 0 || mode == 1) { /* one register to or from Dn/An */
            for (int i = 0; i < 3; i++)
                if (rx & sel[i]) {
                    uint32_t *r = mode == 0 ? &c->d[reg] : &c->a[reg];
                    if (to_mem)
                        *r = *regs[i];
                    else
                        *regs[i] = *r;
                }
            return;
        }
        if (mode == 7 && reg == 4 && !to_mem) {
            for (int i = 0; i < 3; i++)
                if (rx & sel[i])
                    *regs[i] = fetch32(c);
            return;
        }
        uint32_t a;
        if (mode == 4) {
            c->a[reg] -= (uint32_t)n * 4;
            a = c->a[reg];
        } else if (mode == 3) {
            a = c->a[reg];
            c->a[reg] += (uint32_t)n * 4;
        } else {
            ea_t e = ea_resolve(c, mode, reg, 4);
            a = e.addr;
        }
        for (int i = 0; i < 3; i++)
            if (rx & sel[i]) {
                if (to_mem)
                    mw32(c, a, *regs[i]);
                else
                    *regs[i] = mr32(c, a);
                a += 4;
            }
        return;
    }
    case 6:
    case 7: { /* FMOVEM data registers */
        bool to_mem = opclass == 7;
        int list = (w & 0x0800) ? (int)(c->d[(w >> 4) & 7] & 0xFF) : (w & 0xFF);
        bool predec = !(w & 0x1000);
        int count = 0;
        for (int i = 0; i < 8; i++)
            count += (list >> i) & 1;
        uint32_t a;
        if (mode == 4) {
            c->a[reg] -= (uint32_t)count * 12;
            a = c->a[reg];
        } else if (mode == 3) {
            a = c->a[reg];
            c->a[reg] += (uint32_t)count * 12;
        } else {
            ea_t e = ea_resolve(c, mode, reg, 4);
            a = e.addr;
        }
        for (int r = 0; r < 8; r++) {
            int bit = predec ? r : 7 - r; /* predecrement lists FP7 in bit 7 … FP0 in bit 0 */
            if (!((list >> bit) & 1))
                continue;
            if (to_mem)
                ext_write(c, a, c->fp[r]);
            else
                c->fp[r] = ext_read(c, a);
            a += 12;
        }
        return;
    }
    default:
        cpu_exception(c, 11, c->op_pc);
    }
}

/* MOVE16: 16 bytes, both addresses rounded down to a line. */
static void move16(cpu68k *c, uint32_t from, uint32_t to)
{
    from &= ~15u;
    to &= ~15u;
    uint32_t v[4];
    for (int i = 0; i < 4; i++)
        v[i] = mr32(c, from + (uint32_t)i * 4);
    for (int i = 0; i < 4; i++)
        mw32(c, to + (uint32_t)i * 4, v[i]);
}

/* The 68040's own line-F instructions: CINV/CPUSH and PFLUSH/PTEST (no
 * caches or MMU to act on) and MOVE16. */
static bool op_040(cpu68k *c, uint16_t op)
{
    if ((op & 0xFE00) == 0xF400) { /* CINV, CPUSH (F4xx); PFLUSH, PTEST (F5xx) */
        if (!(c->sr & SR_S))
            privilege(c);
        return true;
    }
    if ((op & 0xFFF8) == 0xF620) { /* MOVE16 (Ax)+,(Ay)+ */
        uint16_t ext = fetch16(c);
        int ax = op & 7, ay = (ext >> 12) & 7;
        move16(c, c->a[ax], c->a[ay]);
        c->a[ax] += 16;
        if (ay != ax)
            c->a[ay] += 16;
        return true;
    }
    if ((op & 0xFFE0) == 0xF600) { /* MOVE16 with an absolute address */
        uint32_t abs = fetch32(c);
        int ay = op & 7;
        switch ((op >> 3) & 3) {
        case 0: move16(c, c->a[ay], abs); c->a[ay] += 16; break; /* (Ay)+,(xxx).L */
        case 1: move16(c, abs, c->a[ay]); c->a[ay] += 16; break; /* (xxx).L,(Ay)+ */
        case 2: move16(c, c->a[ay], abs); break;                 /* (Ay),(xxx).L */
        case 3: move16(c, abs, c->a[ay]); break;                 /* (xxx).L,(Ay) */
        }
        return true;
    }
    return false;
}

static void op_lineF(cpu68k *c, uint16_t op)
{
    if (c->model >= 68040 && op_040(c, op))
        return;
    if ((op & 0x0E00) != 0x0200) { /* not coprocessor 1 */
        cpu_exception(c, 11, c->op_pc);
        return;
    }
    switch ((op >> 6) & 7) {
    case 0: fp_general(c, op); break;
    case 1: { /* FScc, FDBcc, FTRAPcc */
        uint16_t w = fetch16(c);
        bool t = fp_cond(c, w & 0x3F);
        int mode = (op >> 3) & 7, reg = op & 7;
        if (mode == 1) { /* FDBcc */
            int16_t disp = (int16_t)fetch16(c);
            if (!t) {
                uint16_t v = (uint16_t)(c->d[reg] - 1);
                c->d[reg] = (c->d[reg] & 0xFFFF0000u) | v;
                if (v != 0xFFFF)
                    c->pc = c->op_pc + 4 + (uint32_t)(int32_t)disp;
            }
        } else if (mode == 7 && reg >= 2) { /* FTRAPcc */
            if (reg == 2)
                fetch16(c);
            else if (reg == 3)
                fetch32(c);
            if (t)
                cpu_exception(c, 7, c->pc);
        } else {
            ea_t e = ea_resolve(c, mode, reg, 1);
            ea_put(c, &e, 1, t ? 0xFF : 0);
        }
        break;
    }
    case 2:
    case 3: { /* FBcc */
        uint32_t base = c->pc;
        int32_t disp = (op & 0x40) ? (int32_t)fetch32(c) : (int16_t)fetch16(c);
        if (fp_cond(c, op & 0x3F))
            c->pc = base + (uint32_t)disp;
        break;
    }
    case 4: { /* FSAVE: the 68040's idle frame, a format word of version
               * $41 and size 0 and nothing more, so FRESTORE takes back
               * just the 4 bytes pushed (Safari's sound interrupt saves
               * and restores the FPU around its work) */
        int mode = (op >> 3) & 7, reg = op & 7;
        if (mode == 4) {
            c->a[reg] -= 4;
            mw32(c, c->a[reg], 0x41000000u);
        } else {
            ea_t e = ea_resolve(c, mode, reg, 4);
            mw32(c, e.addr, 0x41000000u);
        }
        break;
    }
    case 5: { /* FRESTORE */
        int mode = (op >> 3) & 7, reg = op & 7;
        uint32_t a = mode == 3 ? c->a[reg] : ea_resolve(c, mode, reg, 4).addr;
        uint32_t fmt = mr32(c, a);
        if (mode == 3)
            c->a[reg] += 4 + ((fmt >> 16) & 0xFF);
        if ((fmt >> 24) == 0) { /* null frame: reset */
            c->fpcr = c->fpsr = c->fpiar = 0;
            for (int i = 0; i < 8; i++)
                c->fp[i] = NAN;
        }
        break;
    }
    default:
        cpu_exception(c, 11, c->op_pc);
    }
}

void cpu_init(cpu68k *c)
{
    memset(c, 0, sizeof *c);
    c->sr = 0x2700;
    c->addr_mask = 0xFFFFFFFFu;
    c->model = 68020;
}

void cpu_step(cpu68k *c)
{
    if (c->trace)
        c->trace(c);
    c->op_pc = c->pc;
    uint16_t op = fetch16(c);
    c->op = op;
    c->icount++;

    switch (op >> 12) {
    case 0x0: op_line0(c, op); break;
    case 0x1:
    case 0x2:
    case 0x3: op_move(c, op); break;
    case 0x4: op_line4(c, op); break;
    case 0x5: op_line5(c, op); break;
    case 0x6: op_line6(c, op); break;
    case 0x7:
        if (op & 0x0100) {
            if (op == CPU_NATIVE_OP && c->native) {
                uint16_t idx = fetch16(c);
                c->native(c, idx);
            } else {
                illegal(c);
            }
        } else { /* MOVEQ */
            uint32_t v = (uint32_t)(int32_t)(int8_t)(op & 0xFF);
            c->d[(op >> 9) & 7] = v;
            flags_logic(c, v, 4);
        }
        break;
    case 0xA:
        if (c->aline)
            c->aline(c, op);
        else
            cpu_exception(c, 10, c->op_pc);
        break;
    case 0xE: op_lineE(c, op); break;
    case 0xF: op_lineF(c, op); break;
    default: op_arith(c, op); break;
    }
}

uint64_t cpu_run(cpu68k *c, uint64_t max)
{
    uint64_t start = c->icount;
    while (!c->halted && !c->stopped && c->icount - start < max)
        cpu_step(c);
    return c->icount - start;
}
