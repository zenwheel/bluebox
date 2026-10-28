/*
 * 68020 integer interpreter (user and supervisor mode, no MMU, no FPU yet).
 *
 * Special opcodes:
 *   0xAxxx            A-line: handed to the aline hook (the trap dispatcher).
 *   0x71FF nnnn       NATIVE: calls the native hook with index nnnn.  This is
 *                     an invalid MOVEQ encoding, so it never occurs in real code.
 */
#ifndef M68K_H
#define M68K_H

#include <stdbool.h>
#include <stdint.h>

#include "mem.h"

#define CPU_NATIVE_OP 0x71FFu

typedef struct cpu68k cpu68k;

struct cpu68k {
    uint32_t d[8];
    uint32_t a[8];       /* a[7] is the active stack pointer */
    uint32_t pc;
    uint16_t sr;         /* full status register, CCR in the low byte */
    uint32_t usp, isp;   /* inactive stack pointers */
    uint32_t vbr, sfc, dfc, cacr, caar;
    uint32_t addr_mask;  /* 0xFFFFFFFF (32-bit mode) or 0x00FFFFFF */
    int model;           /* 68000, 68020 (the default), 68030 or 68040 */
    int fpu;             /* 0 (none), 68881, 68882 or 68040 (its own) */
    /* Floating point, kept as doubles (53-bit precision). */
    double fp[8];
    uint32_t fpcr, fpsr, fpiar;

    uint32_t op_pc;      /* address of the instruction being executed */
    uint16_t op;         /* its first word */
    uint64_t icount;     /* instructions executed */
    bool stopped;        /* STOP executed */
    bool halted;         /* emulation must stop; see halt_reason */
    char halt_reason[160];

    /* Hooks. aline: pc already points past the A-line word.  native: pc
     * points past the index word.  Both may change any state. */
    void (*aline)(cpu68k *c, uint16_t op);
    void (*native)(cpu68k *c, uint16_t index);
    /* Called before every instruction when non-NULL (tracing). */
    void (*trace)(cpu68k *c);
    void *user;
};

enum { SR_C = 1, SR_V = 2, SR_Z = 4, SR_N = 8, SR_X = 16, SR_S = 0x2000, SR_T = 0x8000 };

void cpu_init(cpu68k *c);
/* Run until halted or `max` instructions have executed. Returns executed count. */
uint64_t cpu_run(cpu68k *c, uint64_t max);
/* Execute one instruction. */
void cpu_step(cpu68k *c);
/* Execute the instruction at pc without counting it (the JIT's way to run
 * one it doesn't translate). */
void cpu_execute_at(cpu68k *c, uint32_t pc);
void cpu_set_sr(cpu68k *c, uint16_t sr);
/* Take exception `vector` with the given return PC (format 0 frame). */
void cpu_exception(cpu68k *c, int vector, uint32_t return_pc);
void cpu_halt(cpu68k *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Stack helpers for native code. */
static inline void cpu_push16(cpu68k *c, uint16_t v) { c->a[7] -= 2; wr16(c->a[7] & c->addr_mask, v); }
static inline void cpu_push32(cpu68k *c, uint32_t v) { c->a[7] -= 4; wr32(c->a[7] & c->addr_mask, v); }
static inline uint16_t cpu_pop16(cpu68k *c) { uint16_t v = rd16(c->a[7] & c->addr_mask); c->a[7] += 2; return v; }
static inline uint32_t cpu_pop32(cpu68k *c) { uint32_t v = rd32(c->a[7] & c->addr_mask); c->a[7] += 4; return v; }

/* The JIT (m68k_jit.c): off by default.  When on, cpu_run translates the
 * blocks it meets into host code and runs them, the interpreter taking the
 * instructions it doesn't translate.  Only on arm64 hosts for now. */
extern bool cpu_jit_enabled;
bool cpu_jit_available(void);
/* Turn it on or off; `cache_dir`, when not NULL, keeps translations
 * between runs in a file there named `cache_name`. */
void cpu_jit_enable(bool on, const char *cache_dir, const char *cache_name);
uint64_t cpu_jit_run(cpu68k *c, uint64_t max);
/* One instruction through the JIT, as a block of its own (tests). */
void cpu_jit_step1(cpu68k *c);
void cpu_jit_flush(void);
typedef struct cpu_jit_stats {
    uint64_t blocks, translated, from_cache, invalidated, flushes, chained, interpreted, jitted;
    uint64_t translate_ns;
} cpu_jit_stats;
void cpu_jit_get_stats(cpu_jit_stats *s);
/* Differential test: random blocks of the instructions the JIT translates,
 * run by the interpreter and by the JIT from the same state, registers and
 * a scratch area of memory (at `scratch`, 512 KB) compared.  Returns the
 * number of mismatches; describes the first in `why`. */
int cpu_jit_selftest(uint32_t seed, int blocks, uint32_t code_at, uint32_t scratch, char *why, int whylen);

/* Disassemble one instruction at addr into buf; returns its length in bytes. */
int cpu_disasm(uint32_t addr, char *buf, int bufsize);

#endif
