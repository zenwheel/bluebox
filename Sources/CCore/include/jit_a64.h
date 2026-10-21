/*
 * arm64 code both JITs emit (see jit.h for the registers translated code
 * runs with): helper calls, guest loads and stores, exits.
 */
#ifndef CCORE_JIT_A64_H
#define CCORE_JIT_A64_H

#include <stddef.h>

#include "a64.h"
#include "jit.h"
#include "mem.h"

enum { JR_CPU = 19, JR_MEM = 20, JR_CTX = 21, JR_BUDGET = 22, JR_MAP = 23 };

#define JIT_CTX(f) ((uint32_t)offsetof(jit_ctx, f))
#define JIT_HELPER(i) ((uint32_t)offsetof(jit_ctx, helper) + 8u * (uint32_t)(i))

/* Call the translator's helper i (arguments in w0..., per the host ABI;
 * x0-x18 and the flags are lost). */
static inline void jit_call(a64_buf *b, int i)
{
    a64_ldst(b, A64_LDRX, 16, JR_CTX, JIT_HELPER(i));
    a64_blr(b, 16);
}

/* Leave for the dispatcher, nothing to chain. */
static inline void jit_exit_plain(a64_buf *b)
{
    a64_mov(b, true, 1, A64_ZR);
    a64_ldst(b, A64_LDRX, 16, JR_CTX, JIT_CTX(exit));
    a64_br(b, 16);
}

/* Leave with x1 = the stub's own address (emitted first: the stub's first
 * word is the one chaining patches). */
static inline void jit_exit_chainable_head(a64_buf *b) { a64_adr(b, 1, 0); }
static inline void jit_exit_chainable_tail(a64_buf *b)
{
    a64_ldst(b, A64_LDRX, 16, JR_CTX, JIT_CTX(exit));
    a64_br(b, 16);
}

/* Go straight to the translation of the guest address in w`reg` when the
 * fast table has it; otherwise fall through.  Uses x13-x16. */
static inline void jit_fast_lookup(a64_buf *b, int reg, int align_shift)
{
    a64_ldst(b, A64_LDRX, 13, JR_CTX, JIT_CTX(fast));
    a64_ubfx(b, 14, reg, align_shift, JIT_FAST_BITS);
    a64_addsub(b, A64_ADD, true, 13, 13, 14, 0, 4);
    a64_ldst(b, A64_LDRW, 15, 13, 0);
    a64_cmp(b, 15, reg);
    uint32_t miss = a64_here(b);
    a64_bcond(b, A64_NE, 0);
    a64_ldst(b, A64_LDRX, 16, 13, 8);
    a64_br(b, 16);
    a64_patch_here(b, miss);
}

/* A big-endian load of 1, 2 or 4 bytes from the guest address in w1 into
 * w0 (sign-extended from 16 bits if asked; reversed: little-endian). */
static inline void jit_load(a64_buf *b, int size, bool algebraic, bool reversed)
{
    if (size == 1) {
        a64_ldst_r(b, A64_LDRB_R, 0, JR_MEM, 1);
    } else if (size == 2) {
        a64_ldst_r(b, A64_LDRH_R, 0, JR_MEM, 1);
        if (!reversed)
            a64_rev16(b, 0, 0);
        if (algebraic)
            a64_sxth(b, 0, 0);
    } else {
        a64_ldst_r(b, A64_LDRW_R, 0, JR_MEM, 1);
        if (!reversed)
            a64_rev(b, 0, 0);
    }
}

/* A big-endian store of the low 1, 2 or 4 bytes of w2 to the guest address
 * in w1.  Inline unless the address is in a region of the code map or (if
 * `watched`) the screen watch range; then through helper `slow` (+0, +1,
 * +2 for 1, 2, 4 bytes), called with (address, value), which does what
 * the interpreter does.  x0-x18 and the flags are lost. */
static inline void jit_store(a64_buf *b, int size, bool watched, int slow)
{
    uint32_t to_slow1 = UINT32_MAX;
    if (watched) {
        a64_ldpw(b, 9, 10, JR_CTX, (int)JIT_CTX(watch_lo));
        a64_sub(b, 9, 1, 9);
        a64_cmp(b, 9, 10);
        to_slow1 = a64_here(b);
        a64_bcond(b, A64_LO, 0);
    }
    a64_lsr(b, 9, 1, CODE_MAP_SHIFT);
    a64_ldst_r(b, A64_LDRB_R, 9, JR_MAP, 9);
    uint32_t to_slow2 = a64_here(b);
    a64_cbz(b, true, 9, 0);
    if (size == 1) {
        a64_ldst_r(b, A64_STRB_R, 2, JR_MEM, 1);
    } else if (size == 2) {
        a64_rev16(b, 9, 2);
        a64_ldst_r(b, A64_STRH_R, 9, JR_MEM, 1);
    } else {
        a64_rev(b, 9, 2);
        a64_ldst_r(b, A64_STRW_R, 9, JR_MEM, 1);
    }
    uint32_t to_done = a64_here(b);
    a64_b(b, 0);
    if (to_slow1 != UINT32_MAX)
        a64_patch_here(b, to_slow1);
    a64_patch_here(b, to_slow2);
    a64_mov(b, false, 0, 1);
    a64_mov(b, false, 1, 2);
    jit_call(b, slow + (size == 1 ? 0 : size == 2 ? 1 : 2));
    a64_patch_here(b, to_done);
}

#endif
