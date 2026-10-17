/*
 * Mixed Mode: routine descriptors and calls between 68k and PowerPC code.
 *
 * A routine descriptor starts with the _MixedModeMagic trap word ($AAFE),
 * so 68k code (and the Toolbox, which calls callbacks as 68k code) that
 * jumps to one runs the trap; the trap reads the descriptor's procInfo,
 * takes the arguments from the 68k stack or registers, runs the PowerPC
 * routine on the interpreter and puts its result back where the 68k caller
 * expects it.  CallUniversalProc goes the other way: PowerPC code calling
 * a 68k routine (or a PowerPC one, through its descriptor).
 *
 * procInfo (Inside Macintosh: PowerPC System Software, 2-2):
 *   bits 0-3  calling convention: 0 Pascal, 1 C, 2 register based,
 *             15 special case (selector in bits 4-9)
 *   bits 4-5  result size: 0 none, 1 one byte, 2 two, 3 four
 *   stack based: 2 bits a parameter from bit 6
 *   register based: result register in bits 6-10, then 5 bits a parameter
 *             from bit 11 (size in the low 2 bits, register in the high 3:
 *             D0-D3, A0-A3)
 */
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "machine.h"
#include "ppc_bridge.h"
#include "tb_internal.h"
#include "trap.h"

enum { kM68kISA = 0, kPowerPCISA = 1 };
enum { kPascalStackBased = 0, kCStackBased = 1, kRegisterBased = 2, kThinkCStackBased = 5,
       kD0DispatchedPascalStackBased = 8, kD0DispatchedCStackBased = 9, kD1DispatchedPascalStackBased = 12,
       kStackDispatchedPascalStackBased = 14 };

/* The dispatched conventions: a selector (its size in bits 6-7) before the
 * parameters, which start at bit 8. */
static bool dispatched(int conv)
{
    return conv == kD0DispatchedPascalStackBased || conv == kD0DispatchedCStackBased ||
           conv == kD1DispatchedPascalStackBased || conv == kStackDispatchedPascalStackBased;
}
enum { RD_SIZE = 12, RR_SIZE = 20 };

/* Descriptor fields */
#define RD_ROUTINE_COUNT 10 /* index of the last record */
#define RD_RECORDS 12
#define RR_PROCINFO 0
#define RR_ISA 5
#define RR_FLAGS 6
#define RR_PROC 8
#define RR_SELECTOR 16

bool mm_is_descriptor(gaddr p) { return p && rd16(p) == 0xAAFE; }

/* The routine of a descriptor for the ISA wanted (else its first). */
static bool mm_routine(gaddr rd, int want, uint32_t *procinfo, int *isa, gaddr *proc)
{
    unsigned n = rd16(rd + RD_ROUTINE_COUNT) + 1u;
    if (n > 16)
        n = 16;
    int pick = 0;
    for (unsigned i = 0; i < n; i++)
        if ((rd8(rd + RD_RECORDS + i * RR_SIZE + RR_ISA) & 0x0F) == want) {
            pick = (int)i;
            break;
        }
    gaddr rr = rd + RD_RECORDS + (gaddr)pick * RR_SIZE;
    *procinfo = rd32(rr + RR_PROCINFO);
    *isa = rd8(rr + RR_ISA) & 0x0F;
    *proc = rd32(rr + RR_PROC);
    uint16_t flags = rd16(rr + RR_FLAGS);
    if (flags & 1) /* kProcDescriptorIsRelative */
        *proc += rd;
    if ((flags & 2) && *isa == kPowerPCISA) {
        /* kFragmentNeedsPreparing: the routine is a PEF container (an
         * accelerated code resource's); its main is the routine.  The
         * record keeps the result, as Mixed Mode's did. */
        gaddr tv = cfm_prepare_container(*proc);
        if (tv) {
            wr32(rr + RR_PROC, tv);
            wr16(rr + RR_FLAGS, (uint16_t)(flags & ~3u));
        }
        *proc = tv;
    }
    return *proc != 0;
}

static gaddr new_descriptor(int count)
{
    gaddr rd = tb_NewPtr(RD_SIZE + RR_SIZE * count, false, true);
    if (!rd)
        return 0;
    wr16(rd, 0xAAFE);
    wr8(rd + 2, 7); /* kRoutineDescriptorVersion */
    wr16(rd + RD_ROUTINE_COUNT, (uint16_t)(count - 1));
    return rd;
}

static void set_record(gaddr rd, int i, gaddr proc, uint32_t procinfo, int isa)
{
    gaddr rr = rd + RD_RECORDS + (gaddr)i * RR_SIZE;
    wr32(rr + RR_PROCINFO, procinfo);
    wr8(rr + RR_ISA, (uint8_t)isa);
    wr16(rr + RR_FLAGS, 0);
    wr32(rr + RR_PROC, proc);
}

/* ---------------------------------------------------------------------- */
/* Running PowerPC code from native code */

static bool ppc_returned;

bool mm_ctx_returned(void) { return ppc_returned; }
void mm_ctx_set_returned(bool r) { ppc_returned = r; }

static void ppc_return_native(ppc_cpu *c)
{
    ppc_returned = true;
    ppc_halt(c, "(returned)");
}

uint32_t ppc_call(gaddr tvector, const uint32_t *args, int nargs)
{
    static gaddr return_code;
    gaddr rtv = cfm_native_tvector(ppc_return_native, "Mixed Mode return");
    return_code = rtv ? rd32(rtv) : 0;
    ppc_cpu saved = g_ppc;
    uint32_t sp = (g_ppc.gpr[1] - 512) & ~15u;
    wr32(sp, g_ppc.gpr[1]);
    for (int i = 0; i < nargs; i++) {
        if (i < 8)
            g_ppc.gpr[3 + i] = args[i];
        else
            wr32(sp + 24 + 4 * (uint32_t)i, args[i]);
    }
    LOGF(LOG_TRAPS, "PowerPC call: %08X (code %08X, TOC %08X), %d arguments, r3 %08X\n", tvector, rd32(tvector),
         rd32(tvector + 4), nargs, nargs ? args[0] : 0);
    g_ppc.gpr[1] = sp;
    g_ppc.pc = rd32(tvector);
    g_ppc.gpr[2] = rd32(tvector + 4);
    g_ppc.lr = return_code;
    g_ppc.halted = false;
    uint64_t cap = trap_instruction_limit();
    while (!g_ppc.halted && !g_cpu->halted) {
        g_cpu->icount += ppc_run(&g_ppc, 20000);
        /* Interrupt-time work goes on inside callbacks and threads too:
         * a program may wait there for a task or a notifier to set a flag. */
        if (!g_ppc.halted) {
            machine_advance_ticks();
            time_pump();
        }
        if (cap && g_cpu->icount >= cap && !g_ppc.halted) {
            if (machine_waiting_for_input()) /* an event loop inside a callback */
                ppc_halt(&g_ppc, "instruction limit reached waiting for input");
            else
                ppc_halt(&g_ppc, "instruction limit reached in a call from the Toolbox (pc %08X)", g_ppc.pc);
        }
    }
    uint32_t result = g_ppc.gpr[3];
    bool returned = ppc_returned;
    ppc_returned = false;
    char reason[sizeof g_ppc.halt_reason];
    memcpy(reason, g_ppc.halt_reason, sizeof reason);
    uint64_t icount = g_ppc.icount;
    g_ppc = saved;
    g_ppc.icount = icount;
    if (!returned) {
        /* Stopped inside: the whole machine stops. */
        ppc_halt(&g_ppc, "%s", reason);
        if (!g_cpu->halted)
            cpu_halt(g_cpu, "%s", reason);
    }
    return result;
}

/* A value of size code `code` (1, 2 or 4 bytes) as a PowerPC register holds it. */
static uint32_t widen(int code, uint32_t v)
{
    return code == 1 ? (v & 0xFF) : code == 2 ? (uint32_t)(int32_t)(int16_t)v : v;
}

static uint32_t *reg68(int which)
{
    return which < 4 ? &g_cpu->d[which] : &g_cpu->a[which - 4];
}

/* The result register of a register-based procInfo (D0-D3, A0-A3, D4-D7, A4-A6). */
static uint32_t *result_reg68(int loc)
{
    if (loc < 8)
        return reg68(loc);
    if (loc < 12)
        return &g_cpu->d[loc - 4];
    if (loc < 15)
        return &g_cpu->a[loc - 8];
    return NULL;
}

static void store_sized(uint32_t *r, int code, uint32_t v)
{
    if (code == 1)
        *r = (*r & ~0xFFu) | (v & 0xFF);
    else if (code == 2)
        *r = (*r & ~0xFFFFu) | (v & 0xFFFF);
    else
        *r = v;
}

/* _MixedModeMagic: 68k code called a routine descriptor (at op_pc).  The
 * dispatcher pushed the address after the trap word and returns to
 * whatever is on top of the stack afterwards: that goes, leaving the
 * caller's return address on top. */
static void mixed_mode_native(cpu68k *c, uint16_t tw)
{
    (void)tw;
    gaddr rd = c->op_pc;
    cpu_pop32(c);
    uint32_t pi;
    int isa;
    gaddr proc;
    /* A fat descriptor: the PowerPC routine if PowerPC code is running. */
    if (!mm_routine(rd, g_ppc_mode ? kPowerPCISA : kM68kISA, &pi, &isa, &proc)) {
        cpu_halt(c, "Mixed Mode: empty routine descriptor at %08X", rd);
        return;
    }
    LOGF(LOG_TRAPS, "Mixed Mode: descriptor %08X, %s routine %08X, procInfo %08X, called from %08X\n", rd,
         isa == kM68kISA ? "68k" : "PowerPC", proc, pi, rd32(c->a[7]));
    if (isa == kM68kISA) {
        cpu_push32(c, proc); /* returned to: the 68k routine, the caller's return under it */
        return;
    }
    if (!g_ppc_mode) {
        cpu_halt(c, "Mixed Mode: PowerPC routine %08X called with no PowerPC fragment", proc);
        return;
    }
    int conv = (int)(pi & 0xF), rcode = (int)((pi >> 4) & 3);
    uint32_t args[13];
    int n = 0;
    uint32_t sp = c->a[7];
    if (conv == kPascalStackBased || conv == kCStackBased || conv == kThinkCStackBased) {
        int codes[13];
        while (n < 13 && ((pi >> (6 + 2 * n)) & 3))
            codes[n] = (int)((pi >> (6 + 2 * n)) & 3), n++;
        if (conv == kPascalStackBased) {
            /* Pushed left to right: the last argument is nearest. */
            uint32_t off = 4;
            for (int i = n - 1; i >= 0; i--) {
                if (codes[i] == 3) {
                    args[i] = rd32(sp + off);
                    off += 4;
                } else {
                    uint16_t w = rd16(sp + off);
                    args[i] = widen(codes[i], codes[i] == 1 ? (uint32_t)(w >> 8) : w);
                    off += 2;
                }
            }
            uint32_t r = ppc_call(proc, args, n);
            if (c->halted)
                return;
            gaddr result = sp + off;
            if (rcode == 1)
                wr8(result, (uint8_t)r);
            else if (rcode == 2)
                wr16(result, (uint16_t)r);
            else if (rcode == 3)
                wr32(result, r);
            uint32_t ret = rd32(sp);
            c->a[7] = sp + off;
            cpu_push32(c, ret);
        } else {
            /* C: first argument nearest; MPW C widens each to 4 bytes, THINK C
             * pushes shorts as 2. */
            uint32_t off = 4;
            for (int i = 0; i < n; i++) {
                if (codes[i] == 3 || conv == kCStackBased) {
                    args[i] = widen(codes[i], rd32(sp + off));
                    off += 4;
                } else {
                    args[i] = widen(codes[i], codes[i] == 1 ? (uint32_t)(rd16(sp + off) >> 8) : rd16(sp + off));
                    off += 2;
                }
            }
            uint32_t r = ppc_call(proc, args, n);
            if (rcode)
                c->d[0] = r;
        }
        return;
    }
    if (conv == kRegisterBased) {
        for (n = 0; n < 4; n++) {
            unsigned f = (pi >> (11 + 5 * n)) & 31;
            if (!(f & 3))
                break;
            args[n] = widen((int)(f & 3), *reg68((int)(f >> 2)));
        }
        uint32_t r = ppc_call(proc, args, n);
        uint32_t *out = result_reg68((int)((pi >> 6) & 31));
        if (rcode && out)
            store_sized(out, rcode, r);
        return;
    }
    cpu_halt(c, "Mixed Mode: calling convention %d (procInfo %08X) from 68k code", conv, pi);
}

/* ---------------------------------------------------------------------- */
/* PowerPC code calling 68k code */

/* Call 68k code at `code` as procInfo describes, with the arguments. */
static uint32_t call_68k(gaddr code, uint32_t pi, const uint32_t *args, int nargs)
{
    int conv = (int)(pi & 0xF), rcode = (int)((pi >> 4) & 3);
    uint32_t r = 0;
    LOGF(LOG_TRAPS, "Mixed Mode: 68k routine %08X (%s), procInfo %08X, %d arguments (%08X %08X)\n", code,
         native_name_at(native_index_for(code)) ? native_name_at(native_index_for(code)) : "guest code", pi, nargs,
         nargs > 0 ? args[0] : 0, nargs > 1 ? args[1] : 0);
    ppc_trap_begin();
    if (dispatched(conv) && nargs >= 1) {
        /* The selector in D0 or D1 or pushed last; Pascal or C parameters. */
        int scode = (int)((pi >> 6) & 3);
        uint32_t sel = args[0];
        bool pascal = conv != kD0DispatchedCStackBased;
        if (pascal && rcode)
            ppc_trap_push(rcode == 3 ? 4 : 2, 0);
        if (pascal) {
            for (int i = 1; i < nargs; i++) {
                int code_i = (int)((pi >> (8 + 2 * (i - 1))) & 3);
                ppc_trap_push(code_i == 3 ? 4 : code_i == 1 ? 1 : 2, args[i]);
            }
        } else {
            for (int i = nargs - 1; i >= 1; i--)
                ppc_trap_push(4, args[i]);
        }
        if (conv == kStackDispatchedPascalStackBased)
            ppc_trap_push(scode == 3 ? 4 : 2, sel);
        else
            ppc_trap_set(conv == kD1DispatchedPascalStackBased ? 1 : 0, sel);
        guest_call_regs(code);
        if (rcode)
            r = widen(rcode, pascal ? ppc_trap_result(rcode == 3 ? 4 : rcode == 1 ? 1 : 2) : g_cpu->d[0]);
    } else if (conv == kPascalStackBased) {
        if (rcode)
            ppc_trap_push(rcode == 3 ? 4 : 2, 0);
        for (int i = 0; i < nargs && i < 13; i++) {
            int code_i = (int)((pi >> (6 + 2 * i)) & 3);
            ppc_trap_push(code_i == 3 ? 4 : code_i == 1 ? 1 : 2, args[i]);
        }
        guest_call_regs(code);
        if (rcode)
            r = widen(rcode, ppc_trap_result(rcode == 3 ? 4 : rcode == 1 ? 1 : 2));
    } else if (conv == kCStackBased || conv == kThinkCStackBased) {
        for (int i = nargs - 1; i >= 0; i--) {
            int code_i = (int)((pi >> (6 + 2 * i)) & 3);
            ppc_trap_push(code_i == 3 || conv == kCStackBased ? 4 : code_i == 1 ? 1 : 2, args[i]);
        }
        guest_call_regs(code);
        r = widen(rcode ? rcode : 3, g_cpu->d[0]);
    } else if (conv == kRegisterBased) {
        for (int i = 0; i < nargs && i < 4; i++) {
            unsigned f = (pi >> (11 + 5 * i)) & 31;
            if (f & 3)
                *reg68((int)(f >> 2)) = args[i];
        }
        guest_call_regs(code);
        uint32_t *out = result_reg68((int)((pi >> 6) & 31));
        if (rcode && out)
            r = widen(rcode, *out);
    } else {
        ppc_halt(&g_ppc, "Mixed Mode: calling convention %d (procInfo %08X) to 68k code", conv, pi);
    }
    ppc_trap_end();
    if (g_cpu->halted && !g_ppc.halted)
        ppc_halt(&g_ppc, "%s", g_cpu->halt_reason);
    return r;
}

/* The number of arguments procInfo describes. */
static int arg_count(uint32_t pi)
{
    int conv = (int)(pi & 0xF), n = 0;
    if (dispatched(conv)) {
        while (n < 12 && ((pi >> (8 + 2 * n)) & 3))
            n++;
        return n + 1; /* and the selector */
    }
    if (conv == kRegisterBased) {
        while (n < 4 && ((pi >> (11 + 5 * n)) & 3))
            n++;
    } else {
        while (n < 13 && ((pi >> (6 + 2 * n)) & 3))
            n++;
    }
    return n;
}

/* Call a universal procedure pointer from native code: a descriptor's
 * PowerPC routine, or 68k code (a descriptor's or a bare pointer). */
uint32_t mm_call(gaddr upp, uint32_t pi, const uint32_t *args, int nargs)
{
    gaddr code = upp;
    if (mm_is_descriptor(upp)) {
        int isa;
        if (!mm_routine(upp, g_ppc_mode ? kPowerPCISA : kM68kISA, &pi, &isa, &code))
            return 0;
        if (isa == kPowerPCISA)
            return ppc_call(code, args, nargs);
    }
    return code ? call_68k(code, pi, args, nargs) : 0;
}

/* CallUniversalProc(theProc, procInfo, ...) */
static void ppc_CallUniversalProc(ppc_cpu *c)
{
    gaddr upp = c->gpr[3];
    uint32_t pi = c->gpr[4];
    int n = arg_count(pi);
    uint32_t args[13];
    for (int i = 0; i < n; i++)
        args[i] = i + 2 < 8 ? c->gpr[5 + i] : rd32(c->gpr[1] + 24 + 4 * (uint32_t)(i + 2));
    gaddr code = upp;
    if (mm_is_descriptor(upp)) {
        int isa;
        if (!mm_routine(upp, kPowerPCISA, &pi, &isa, &code)) {
            ppc_halt(c, "CallUniversalProc: empty routine descriptor at %08X", upp);
            return;
        }
        if (isa == kPowerPCISA) {
            /* A PowerPC routine: go straight there, returning to our caller. */
            for (int i = 0; i < n; i++) {
                if (i < 8)
                    c->gpr[3 + i] = args[i];
                else
                    wr32(c->gpr[1] + 24 + 4 * (uint32_t)i, args[i]);
            }
            c->pc = rd32(code);
            c->gpr[2] = rd32(code + 4);
            return;
        }
    }
    if (!code) {
        ppc_halt(c, "CallUniversalProc: NULL routine");
        return;
    }
    c->gpr[3] = call_68k(code, pi, args, n);
    c->pc = c->lr;
}

/* NewRoutineDescriptor(theProc, procInfo, ISA) */
static void ppc_NewRoutineDescriptor(ppc_cpu *c)
{
    gaddr proc = c->gpr[3];
    gaddr rd = proc ? new_descriptor(1) : 0;
    if (rd)
        set_record(rd, 0, proc, c->gpr[4], (int)(c->gpr[5] & 0xFF));
    c->gpr[3] = rd;
    c->pc = c->lr;
}

/* NewFatRoutineDescriptor(theM68kProc, thePowerPCProc, procInfo) */
static void ppc_NewFatRoutineDescriptor(ppc_cpu *c)
{
    gaddr rd = new_descriptor(2);
    if (rd) {
        set_record(rd, 0, c->gpr[3], c->gpr[5], kM68kISA);
        set_record(rd, 1, c->gpr[4], c->gpr[5], kPowerPCISA);
    }
    c->gpr[3] = rd;
    c->pc = c->lr;
}

static void ppc_DisposeRoutineDescriptor(ppc_cpu *c)
{
    if (mm_is_descriptor(c->gpr[3]))
        tb_DisposePtr(c->gpr[3]);
    c->pc = c->lr;
}

/* GetCurrentArchitecture / GetCurrentISA: PowerPC. */
static void ppc_GetCurrentISA(ppc_cpu *c)
{
    c->gpr[3] = kPowerPCISA;
    c->pc = c->lr;
}

const ppc_export mm_exports[] = {
    { "CallUniversalProc", ppc_CallUniversalProc },
    { "DisposeRoutineDescriptor", ppc_DisposeRoutineDescriptor },
    { "GetCurrentArchitecture", ppc_GetCurrentISA },
    { "GetCurrentISA", ppc_GetCurrentISA },
    { "NewFatRoutineDescriptor", ppc_NewFatRoutineDescriptor },
    { "NewRoutineDescriptor", ppc_NewRoutineDescriptor },
};
const unsigned mm_export_count = sizeof mm_exports / sizeof mm_exports[0];

/* ---------------------------------------------------------------------- */
/* CallComponentUPP: InterfaceLib's descriptor for calling components.
 * PowerPC glue passes it a ComponentParameters block, which is the stack
 * image of a 68k component call (flags, parameter size, selector, the
 * parameters, the instance): copied onto the 68k stack, it goes to
 * _ComponentDispatch as an inline call's would. */
static void call_component_68k(cpu68k *c, uint16_t tw)
{
    (void)tw;
    uint32_t ret = cpu_pop32(c);
    gaddr params = cpu_pop32(c);
    gaddr result_at = c->a[7]; /* the caller's result space */
    uint32_t n = 4u + rd8(params + 1) + 4u;
    c->a[7] -= n;
    gmemcpy(c->a[7], params, n);
    ppc_trap_begin();
    ppc_trap(0xA82A);
    ppc_trap_end();
    c->a[7] = result_at;
    cpu_push32(c, ret);
}

gaddr mm_call_component_upp(void)
{
    gaddr var = tb_NewPtr(4, true, true);
    gaddr rd = new_descriptor(1);
    if (var && rd) {
        /* pascal long CallComponent(ComponentParameters *) */
        set_record(rd, 0, native_register(call_component_68k, "CallComponent", 0), 0x3F0, kM68kISA);
        wr32(var, rd);
    }
    return var;
}

void mixedmode_init(void)
{
    wr32(TB_TRAP_TABLE + 0x2FEu * 4, native_register(mixed_mode_native, "MixedModeMagic", 0xAAFE));
}
