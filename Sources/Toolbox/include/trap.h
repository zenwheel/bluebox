/*
 * A-line trap dispatch and native code stubs.
 *
 * Every native routine (trap glue, dispatcher, internal helper) gets a stub
 * in the fake ROM: `NATIVE index; RTS`.  Trap tables hold stub addresses, so
 * apps can read, patch and call through them exactly as on a real Mac.  When
 * a trap's table entry is still our stub, the dispatcher skips the stub and
 * calls the native code directly.
 */
#ifndef TOOLBOX_TRAP_H
#define TOOLBOX_TRAP_H

#include <stdbool.h>
#include <stdint.h>

#include "m68k.h"

/* Trap tables in low memory (Mac II layout). */
#define OS_TRAP_TABLE 0x0400u   /* 256 entries */
#define TB_TRAP_TABLE 0x0E00u   /* 1024 entries */

typedef void (*native_fn)(cpu68k *c, uint16_t trapword);

enum { CONV_STACK, CONV_REG };

/* One row per undispatched trap (generated). */
typedef struct {
    uint16_t trap;
    int32_t selector; /* -1 */
    const char *name;
    uint8_t conv;
    uint16_t argbytes, retbytes;
    native_fn glue; /* NULL if not implemented */
} trap_sig;

/* One row per dispatched routine (generated); same shape. */
typedef trap_sig dispatch_sig;

typedef struct {
    uint16_t trap;
    const char *name;
    const char *selector_location;
} dispatcher_info;

extern const char *const os_trap_names[256];
extern const char *const tb_trap_names[1024];
extern const trap_sig trap_sigs[];
extern const unsigned trap_sig_count;
extern const dispatch_sig dispatch_sigs[];
extern const unsigned dispatch_sig_count;
extern const dispatcher_info dispatchers[];
extern const unsigned dispatcher_count;

/* Install natives, stubs, trap tables and exception vectors. */
void trap_init(void);
void trap_autovectors(void); /* once the system zone is there */

/* Register a native routine; returns the guest address of its stub. */
uint32_t native_register(native_fn fn, const char *name, uint16_t trapword);
/* Native index of a stub address, or -1. */
native_fn native_fn_at(int index); /* the function of a native, or NULL */
const char *native_name_at(int index); /* and its name */
int native_index_for(uint32_t addr);

/* Call a Pascal-convention routine (guest code, or one of our stubs) whose
 * result space and arguments the caller has already pushed.  Registers other
 * than A7 are preserved. */
void guest_call(gaddr fn);
/* The same, leaving the registers as the routine left them. */
void guest_call_regs(gaddr fn);
/* Call a low-memory hook with D0 and A0 set; all registers (A7 = sp) are
 * restored afterwards.  Returns the D0 it left. */
uint32_t hook_call(gaddr fn, uint32_t d0, uint32_t a0, uint32_t sp);

/* The machine's instruction limit (0: none). */
uint64_t trap_instruction_limit(void);

/* Trap word of the routine currently executing (e.g. HFS bit 0x200). */
uint16_t trap_current_word(void);
void trap_set_current_word(uint16_t w); /* calls that come from PowerPC code: no trap */

/* The CPU hooks. */
void trap_aline(cpu68k *c, uint16_t op);
void trap_native(cpu68k *c, uint16_t index);

/* Name of a trap word ("GetResource", "_A9A0" if unknown). */
const char *trap_name(uint16_t trapword);

/* When true, unimplemented traps log, discard their arguments, return zero
 * and carry on (a "census" run); when false they halt the CPU. */
extern bool g_trap_census;

/* Census of traps seen during a run. */
typedef struct {
    uint16_t trap;
    int32_t selector;
    uint32_t calls;
    bool implemented;
} trap_count;
const trap_count *trap_census(unsigned *count);
void trap_census_reset(void);

#endif
