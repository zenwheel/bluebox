/*
 * PowerPC interpreter: 32-bit user mode (the 601/603/604/G3 instruction set
 * application programs use: integer, branch, condition register, load and
 * store, floating point), big-endian, on the guest memory of mem.h.
 *
 * Special opcode:
 *   0x04000000 | n    NATIVE: primary opcode 1, unused by the architecture;
 *                     calls the native hook with index n (26 bits).  The
 *                     hook returns with pc set where execution continues
 *                     (a stub standing for a function sets pc = lr).
 */
#ifndef PPC_H
#define PPC_H

#include <stdbool.h>
#include <stdint.h>

#include "mem.h"

#define PPC_NATIVE_OP 0x04000000u

typedef struct ppc_cpu ppc_cpu;

struct ppc_cpu {
    uint32_t gpr[32];
    uint64_t fpr[32];         /* bit patterns (fctiw, stfiwx and moves keep them exact) */
    uint32_t cr, lr, ctr, xer, fpscr;
    uint32_t pc;
    uint32_t op_pc;           /* address of the instruction being executed */
    uint32_t prev_pc;         /* and of the one before it */
    uint32_t op;              /* the instruction */
    uint64_t icount;          /* instructions executed */
    uint32_t reserve_addr;    /* lwarx / stwcx. */
    bool reserved;
    bool halted;              /* execution must stop; see halt_reason */
    char halt_reason[160];
    uint64_t tb;              /* time base */

    /* native: pc already points past the NATIVE word. */
    void (*native)(ppc_cpu *c, uint32_t index);
    /* sc (system call): pc points past it. */
    void (*syscall)(ppc_cpu *c);
    /* Called before every instruction when non-NULL (tracing). */
    void (*trace)(ppc_cpu *c);
    void *user;
};

/* XER bits */
enum { PPC_XER_SO = 0x80000000u, PPC_XER_OV = 0x40000000u, PPC_XER_CA = 0x20000000u };

void ppc_init(ppc_cpu *c);
/* Run until halted or `max` instructions have executed; returns the count. */
uint64_t ppc_run(ppc_cpu *c, uint64_t max);
void ppc_step(ppc_cpu *c);
void ppc_halt(ppc_cpu *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#include <string.h>
static inline double ppc_fpr(const ppc_cpu *c, int n)
{
    double d;
    memcpy(&d, &c->fpr[n], 8);
    return d;
}
static inline void ppc_set_fpr(ppc_cpu *c, int n, double d) { memcpy(&c->fpr[n], &d, 8); }

/* Condition register field n (0 to 7), as 4 bits LT GT EQ SO. */
static inline uint32_t ppc_cr_field(const ppc_cpu *c, int n) { return (c->cr >> (28 - 4 * n)) & 0xF; }

#endif
