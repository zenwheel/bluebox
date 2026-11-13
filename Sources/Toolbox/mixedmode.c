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
 *             5 THINK C, 8/9/12/14 dispatched, 15 special case (selector
 *             in bits 4-9)
 *   bits 4-5  result size: 0 none, 1 one byte, 2 two, 3 four
 *   stack based: 2 bits a parameter from bit 6
 *   dispatched: the selector's size in bits 6-7, parameters from bit 8
 *   register based: result register in bits 6-10 (D0-D3 0-3, A0-A3 4-7,
 *             D4-D7 8-11, A4-A6 12-14, a CCR bit C V Z N X 16-20), then
 *             5 bits a parameter from bit 11 (size in the low 2 bits,
 *             register in the high 3: D0-D3, A0-A3)
 *   special cases: the hooks whose registers no procInfo can describe
 *             (TextEdit's, the menu bar's, GetNextEvent's filter, AppleTalk
 *             protocol handlers and socket listeners), as MixedMode.h lists
 *             them.
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
       kStackDispatchedPascalStackBased = 14, kSpecialCase = 15 };
enum { kSpecialCaseHighHook = 0, kSpecialCaseEOLHook, kSpecialCaseWidthHook, kSpecialCaseNWidthHook,
       kSpecialCaseDrawHook, kSpecialCaseHitTestHook, kSpecialCaseTEFindWord, kSpecialCaseProtocolHandler,
       kSpecialCaseSocketListener, kSpecialCaseTERecalc, kSpecialCaseTEDoText, kSpecialCaseGNEFilterProc,
       kSpecialCaseMBarHook, kSpecialCaseCount };
/* The PowerPC routine's parameters for each special case. */
static const int8_t special_nargs[kSpecialCaseCount] = { 2, 3, 5, 8, 5, 9, 6, 6, 7, 5, 6, 2, 1 };
enum { CCR_Z = 0x04 };

/* The dispatched conventions: a selector (its size in bits 6-7) before the
 * parameters, which start at bit 8. */
static bool dispatched(int conv)
{
    return conv == kD0DispatchedPascalStackBased || conv == kD0DispatchedCStackBased ||
           conv == kD1DispatchedPascalStackBased || conv == kStackDispatchedPascalStackBased;
}
enum { RD_SIZE = 12, RR_SIZE = 20 };

/* Descriptor fields */
#define RD_FLAGS 3          /* kSelectorsAreIndexable */
#define RD_SELECTOR_INFO 9  /* a dispatched descriptor's calling convention */
#define RD_ROUTINE_COUNT 10 /* index of the last record */
#define RD_RECORDS 12
#define RR_PROCINFO 0
#define RR_ISA 5
#define RR_FLAGS 6
#define RR_PROC 8
#define RR_RESERVED2 12
#define RR_SELECTOR 16

bool mm_is_descriptor(gaddr p) { return p && rd16(p) == 0xAAFE; }
bool mm_is_native(gaddr p) { return mem_strip(p) - GA_ROM < GA_ROM_SIZE; }

/* 68k code a PowerPC program runs through Mixed Mode (CallUniversalProc)
 * can install handlers of its own, plain 68k procedure pointers: the
 * handler calls note them, so the handlers aren't taken for transition
 * vectors when they are called while PowerPC code runs. */
static int running_68k; /* depth of 68k code called from PowerPC */
enum { MAX_68K_PROCS = 128 };
static gaddr procs_68k[MAX_68K_PROCS];
static int nprocs_68k;

void mm_note_proc(gaddr p)
{
    if (!p || !g_ppc_mode || !running_68k || mm_is_descriptor(p) || mm_is_68k_proc(p))
        return;
    if (nprocs_68k < MAX_68K_PROCS)
        procs_68k[nprocs_68k++] = p;
}

bool mm_is_68k_proc(gaddr p)
{
    for (int i = 0; i < nprocs_68k; i++)
        if (procs_68k[i] == p)
            return true;
    return false;
}

enum { kSelectorsAreIndexable = 0x01, kUseNativeISA = 0x04, kDontPassSelector = 0x08, kRoutineIsDispatchedDefaultRoutine = 0x10 };

/* The record a call takes: for a dispatched call (sel given), the one for
 * that selector (by index when the selectors are indexable), else the
 * dispatched default routine; otherwise the one for the ISA wanted, else
 * the first. */
static int pick_record(gaddr rd, int want, const uint32_t *sel)
{
    unsigned n = rd16(rd + RD_ROUTINE_COUNT) + 1u;
    if (n > 256)
        n = 256;
    if (sel && n > 1) {
        if ((rd8(rd + RD_FLAGS) & kSelectorsAreIndexable) && *sel < n)
            return (int)*sel;
        int fallback = -1;
        for (int pass = 0; pass < 2; pass++) /* the ISA wanted, then any */
            for (unsigned i = 0; i < n; i++) {
                gaddr rr = rd + RD_RECORDS + i * RR_SIZE;
                if (!pass && (rd8(rr + RR_ISA) & 0x0F) != want)
                    continue;
                if (rd32(rr + RR_SELECTOR) == *sel)
                    return (int)i;
                if (fallback < 0 && (rd16(rr + RR_FLAGS) & kRoutineIsDispatchedDefaultRoutine))
                    fallback = (int)i;
            }
        if (fallback >= 0)
            return fallback;
    }
    int found = -1, native = -1;
    for (unsigned i = 0; i < n; i++) {
        int isa = rd8(rd + RD_RECORDS + i * RR_SIZE + RR_ISA) & 0x0F;
        if (isa == want && found < 0)
            found = (int)i;
        if (isa == kPowerPCISA && native < 0)
            native = (int)i;
    }
    /* kUseNativeISA on the record for the running code: a Power Mac takes
     * the PowerPC routine instead (Morphin' Shapes' fat 'ADgm', called
     * from 68k code, runs its PowerPC half). */
    if (found >= 0 && want != kPowerPCISA && native >= 0 &&
        (rd16(rd + RD_RECORDS + (gaddr)found * RR_SIZE + RR_FLAGS) & kUseNativeISA) &&
        gestalt_value(fourcc("sysa"), 1) == 2)
        return native;
    return found >= 0 ? found : 0;
}

/* The routine of a descriptor for the ISA wanted (else its first); for a
 * dispatched call, the selector's.  *rflags gets the record's flags. */
static bool mm_routine_sel(gaddr rd, int want, const uint32_t *sel, uint32_t *procinfo, int *isa, gaddr *proc,
                           uint16_t *rflags)
{
    int pick = pick_record(rd, want, sel);
    gaddr rr = rd + RD_RECORDS + (gaddr)pick * RR_SIZE;
    *procinfo = rd32(rr + RR_PROCINFO);
    *isa = rd8(rr + RR_ISA) & 0x0F;
    *proc = rd32(rr + RR_PROC);
    uint16_t flags = rd16(rr + RR_FLAGS);
    if (rflags)
        *rflags = flags;
    if (flags & 1) /* kProcDescriptorIsRelative */
        *proc += rd;
    if ((flags & 2) && *isa == kPowerPCISA) {
        /* kFragmentNeedsPreparing: the routine is a PEF container (an
         * accelerated code resource's); its main is the routine.  The
         * record keeps the result, as Mixed Mode's did. */
        /* A descriptor an XCMD's tool saved after preparing it in memory
         * keeps stale addresses in procDescriptor, and the container's
         * offset in the reserved word after it; failing that the container
         * follows the records (QuickFiles, Bob's Bricks, Weather or Not?,
         * CallerID Remote, Type & Speak, EasyMail all do). */
        gaddr pef = *proc;
        if (rd32(pef) != 0x4A6F7921u /* Joy! */) {
            gaddr base = (flags & 1) ? rd : 0;
            gaddr alt = rd32(rr + RR_RESERVED2) ? base + rd32(rr + RR_RESERVED2) : 0;
            gaddr after = rd + RD_RECORDS + (rd16(rd + RD_ROUTINE_COUNT) + 1u) * RR_SIZE;
            if (alt && rd32(alt) == 0x4A6F7921u)
                pef = alt;
            else if (rd32(after) == 0x4A6F7921u)
                pef = after;
        }
        gaddr tv = cfm_prepare_container(pef);
        if (tv) {
            wr32(rr + RR_PROC, tv);
            wr16(rr + RR_FLAGS, (uint16_t)(flags & ~3u));
        }
        *proc = tv;
    }
    /* A routine that is itself a routine descriptor (List Files makes a
     * descriptor of its user item's UPP): Mixed Mode goes on to that
     * one's routine, as the descriptor's _MixedModeMagic would. */
    static int depth;
    if (*proc && mm_is_descriptor(*proc) && *proc != rd && depth < 4) {
        depth++;
        bool ok = mm_routine_sel(*proc, want, sel, procinfo, isa, proc, rflags);
        depth--;
        return ok;
    }
    return *proc != 0;
}

static bool mm_routine(gaddr rd, int want, uint32_t *procinfo, int *isa, gaddr *proc)
{
    return mm_routine_sel(rd, want, NULL, procinfo, isa, proc, NULL);
}

bool mm_descriptor_routine(gaddr rd, int want, uint32_t *procinfo, int *isa, gaddr *proc)
{
    return mm_routine(rd, want, procinfo, isa, proc);
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

/* A one-routine descriptor (CarbonLib's NewXxxUPP calls make these). */
gaddr mm_new_descriptor(gaddr proc, uint32_t procinfo, int isa)
{
    gaddr rd = proc ? new_descriptor(1) : 0;
    if (rd)
        set_record(rd, 0, proc, procinfo, isa);
    return rd;
}

/* ---------------------------------------------------------------------- */
/* Running PowerPC code from native code */

static bool ppc_returned;
static int ppc_depth; /* calls into PowerPC code from native code under way */

bool mm_ctx_returned(void) { return ppc_returned; }
void mm_ctx_set_returned(bool r) { ppc_returned = r; }
int mm_call_depth(void) { return ppc_depth; }
void mm_ctx_set_depth(int d) { ppc_depth = d; }

static void run_to_return(void);

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
    /* r12 holds the transition vector, as for a call through a pointer:
     * Hardwood Solitaire II's trap patches are vectors built in the heap
     * whose code reads the routine to go on to from the vector. */
    g_ppc.gpr[12] = tvector;
    g_ppc.lr = return_code;
    g_ppc.halted = false;
    ppc_depth++;
    run_to_return();
    ppc_depth--;
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

/* A resumed thread's entry routine goes on (threadmgr.c): its call was
 * made by the process that hibernated, its return address is the Mixed
 * Mode return its stack already holds, and the call is counted in the
 * depth the thread's context put back.  Returns r3 as the routine left
 * it; a stop inside stops the machine, as in ppc_call. */
uint32_t ppc_call_resume(void)
{
    g_ppc.halted = false;
    run_to_return();
    ppc_depth--;
    uint32_t result = g_ppc.gpr[3];
    bool returned = ppc_returned;
    ppc_returned = false;
    if (!returned && !g_cpu->halted)
        cpu_halt(g_cpu, "%s", g_ppc.halt_reason);
    return result;
}

static void run_to_return(void)
{
    int was_68k = running_68k;
    running_68k = 0;
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
    running_68k = was_68k;
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

/* A result in a condition code (kCCRegisterCBit to XBit, 16-20): C V Z N X
 * are the CCR's bits 0 to 4. */
static int result_ccr_bit(int loc) { return loc >= 16 && loc <= 20 ? loc - 16 : -1; }

static void store_sized(uint32_t *r, int code, uint32_t v)
{
    if (code == 1)
        *r = (*r & ~0xFFu) | (v & 0xFF);
    else if (code == 2)
        *r = (*r & ~0xFFFFu) | (v & 0xFFFF);
    else
        *r = v;
}

static void set_ccr_bit(cpu68k *c, int bit, bool on)
{
    c->sr = (uint16_t)(on ? c->sr | (1u << bit) : c->sr & ~(1u << bit));
}

/* The size codes of procInfo's stack parameters from bit `phase`; their count. */
static int stack_codes(uint32_t pi, int phase, int *codes)
{
    int n = 0;
    while (n < 13 && phase + 2 * n < 32 && ((pi >> (phase + 2 * n)) & 3))
        codes[n] = (int)((pi >> (phase + 2 * n)) & 3), n++;
    return n;
}

/* Pascal parameters at `at`, pushed left to right (the last one nearest);
 * the bytes they take. */
static uint32_t read_pascal(gaddr at, const int *codes, int n, uint32_t *args)
{
    uint32_t off = 0;
    for (int i = n - 1; i >= 0; i--) {
        if (codes[i] == 3) {
            args[i] = rd32(at + off);
            off += 4;
        } else {
            uint16_t w = rd16(at + off);
            args[i] = widen(codes[i], codes[i] == 1 ? (uint32_t)(w >> 8) : w);
            off += 2;
        }
    }
    return off;
}

/* C parameters at `at`, the first nearest: MPW C widens each to 4 bytes,
 * THINK C pushes shorts as 2. */
static void read_c(gaddr at, bool think, const int *codes, int n, uint32_t *args)
{
    uint32_t off = 0;
    for (int i = 0; i < n; i++) {
        if (codes[i] == 3 || !think) {
            args[i] = widen(codes[i], rd32(at + off));
            off += 4;
        } else {
            args[i] = widen(codes[i], codes[i] == 1 ? (uint32_t)(rd16(at + off) >> 8) : rd16(at + off));
            off += 2;
        }
    }
}

static void write_sized(gaddr at, int code, uint32_t v)
{
    if (code == 1)
        wr8(at, (uint8_t)v);
    else if (code == 2)
        wr16(at, (uint16_t)v);
    else if (code == 3)
        wr32(at, v);
}

/* A dispatched call's selector as the records hold it, by its size code. */
static uint32_t selector_value(uint32_t pi, uint32_t v)
{
    int code = (int)((pi >> 6) & 3);
    return code == 1 ? v & 0xFF : code == 2 ? v & 0xFFFF : v;
}

/* A dispatched procInfo for a routine that takes no selector
 * (kDontPassSelector): the plain stack-based call of the same parameters. */
static uint32_t undispatched(uint32_t pi)
{
    uint32_t conv = (pi & 0xF) == kD0DispatchedCStackBased ? kCStackBased : kPascalStackBased;
    return conv | (pi & 0x30) | ((pi >> 8) << 6);
}

/* ---------------------------------------------------------------------- */
/* The special cases (MixedMode.h's SPECIAL_CASE_PROCINFO): each hook's
 * PowerPC parameters and where its 68k form keeps them. */

static uint16_t lo16(uint32_t v) { return (uint16_t)v; }

/* 68k code called a special case's PowerPC routine: its parameters from the
 * registers (and the stack), its results back into them.  What the routine
 * hands back through pointers goes in room below the 68k stack. */
static void special_from_68k(cpu68k *c, int which, gaddr proc)
{
    uint32_t *d = c->d, *a = c->a;
    gaddr sp = a[7];
    gaddr out = (sp - 16) & ~1u;
    a[7] = out;
    gmemset(out, 0, 16);
    uint32_t x[9], r;
    switch (which) {
    case kSpecialCaseHighHook: /* (const Rect *r on the stack, TEPtr A3); the caller pops */
        x[0] = rd32(sp + 4), x[1] = a[3];
        ppc_call(proc, x, 2);
        break;
    case kSpecialCaseEOLHook: /* (char D0.B, TEPtr A3, TEHandle A4): Boolean in Z */
        x[0] = d[0] & 0xFF, x[1] = a[3], x[2] = a[4];
        r = ppc_call(proc, x, 3);
        set_ccr_bit(c, 2, r & 0xFF);
        break;
    case kSpecialCaseWidthHook: /* (len D0.W, offset D1.W, text A0, A3, A4): width in D1.W */
        x[0] = lo16(d[0]), x[1] = lo16(d[1]), x[2] = a[0], x[3] = a[3], x[4] = a[4];
        r = ppc_call(proc, x, 5);
        store_sized(&d[1], 2, r);
        break;
    case kSpecialCaseNWidthHook: /* slop D2.W low, direction high, lineStart A2 */
        x[0] = lo16(d[0]), x[1] = lo16(d[1]), x[2] = widen(2, d[2]), x[3] = widen(2, d[2] >> 16), x[4] = a[0];
        x[5] = a[2], x[6] = a[3], x[7] = a[4];
        r = ppc_call(proc, x, 8);
        store_sized(&d[1], 2, r);
        break;
    case kSpecialCaseDrawHook: /* (offset D0.W, len D1.W, text A0, A3, A4) */
        x[0] = lo16(d[0]), x[1] = lo16(d[1]), x[2] = a[0], x[3] = a[3], x[4] = a[4];
        ppc_call(proc, x, 5);
        break;
    case kSpecialCaseHitTestHook:
        /* D0.W, D1.W, D2.W, A0, A3, A4 in; pixel width and the Boolean in
         * D0's low and high words, the offset in D1.W, pixelInChar D2.W. */
        x[0] = lo16(d[0]), x[1] = lo16(d[1]), x[2] = lo16(d[2]), x[3] = a[0], x[4] = a[3], x[5] = a[4];
        x[6] = out, x[7] = out + 2, x[8] = out + 4;
        r = ppc_call(proc, x, 9);
        d[0] = ((r & 0xFF) ? 0x10000u : 0) | rd16(out);
        store_sized(&d[1], 2, rd16(out + 2));
        store_sized(&d[2], 2, rd8(out + 4) ? 1 : 0);
        break;
    case kSpecialCaseTEFindWord: /* (pos D0.W, caller D2.W, A3, A4): word start D0.W, end D1.W */
        x[0] = lo16(d[0]), x[1] = widen(2, d[2]), x[2] = a[3], x[3] = a[4], x[4] = out, x[5] = out + 2;
        ppc_call(proc, x, 6);
        store_sized(&d[0], 2, rd16(out));
        store_sized(&d[1], 2, rd16(out + 2));
        break;
    case kSpecialCaseProtocolHandler: /* (A0-A4, bytes left D1.W): Boolean in Z */
    case kSpecialCaseSocketListener:  /* (A0-A4, destination D0.B, bytes left D1.W) */
        for (int i = 0; i < 5; i++)
            x[i] = a[i];
        if (which == kSpecialCaseSocketListener)
            x[5] = d[0] & 0xFF, x[6] = widen(2, d[1]);
        else
            x[5] = widen(2, d[1]);
        r = ppc_call(proc, x, which == kSpecialCaseSocketListener ? 7 : 6);
        set_ccr_bit(c, 2, r & 0xFF);
        break;
    case kSpecialCaseTERecalc: /* (TEPtr A3, change D7.W): lineStart D2.W, firstChar D3.W, lastChar D4.W */
        x[0] = a[3], x[1] = lo16(d[7]), x[2] = out, x[3] = out + 2, x[4] = out + 4;
        wr16(out, lo16(d[2])), wr16(out + 2, lo16(d[3])), wr16(out + 4, lo16(d[4]));
        ppc_call(proc, x, 5);
        store_sized(&d[2], 2, rd16(out));
        store_sized(&d[3], 2, rd16(out + 2));
        store_sized(&d[4], 2, rd16(out + 4));
        break;
    case kSpecialCaseTEDoText: /* (A3, first D3.W, last D4.W, selector D7.W): port A0, position D0.W */
        x[0] = a[3], x[1] = lo16(d[3]), x[2] = lo16(d[4]), x[3] = widen(2, d[7]), x[4] = out, x[5] = out + 4;
        wr32(out, a[0]), wr16(out + 4, lo16(d[0]));
        ppc_call(proc, x, 6);
        a[0] = rd32(out);
        store_sized(&d[0], 2, rd16(out + 4));
        break;
    case kSpecialCaseGNEFilterProc: {
        /* (EventRecord * A1, Boolean *result): GetNextEvent's result is
         * the word at 4(SP), D0 holding it too; changed, it stays a Pascal
         * Boolean (the byte high). */
        bool was = rd16(sp + 4) != 0;
        wr8(out, was);
        x[0] = a[1], x[1] = out;
        ppc_call(proc, x, 2);
        bool now = rd8(out) != 0;
        if (now != was)
            wr16(sp + 4, now ? 0x0100 : 0);
        store_sized(&d[0], 2, rd16(sp + 4));
        break;
    }
    case kSpecialCaseMBarHook: /* (Rect *menuRect on the stack): short in D0; the caller pops */
        x[0] = rd32(sp + 4);
        r = ppc_call(proc, x, 1);
        d[0] = widen(2, r);
        break;
    }
    a[7] = sp;
}

static void put16(gaddr p, uint32_t v)
{
    if (p)
        wr16(p, (uint16_t)v);
}

/* PowerPC code calling a special case's 68k routine (registers saved by
 * the caller): the parameters into its registers (x, nine of them, the
 * ones not passed 0), or on its stack. */
static void special_set_up(int which, const uint32_t *x)
{
    cpu68k *c = g_cpu;
    uint32_t *d = c->d, *a = c->a;
    switch (which) {
    case kSpecialCaseHighHook:
        cpu_push32(c, x[0]);
        a[3] = x[1];
        break;
    case kSpecialCaseEOLHook:
        d[0] = x[0] & 0xFF, a[3] = x[1], a[4] = x[2];
        break;
    case kSpecialCaseWidthHook:
    case kSpecialCaseDrawHook:
        d[0] = lo16(x[0]), d[1] = lo16(x[1]), a[0] = x[2], a[3] = x[3], a[4] = x[4];
        break;
    case kSpecialCaseNWidthHook:
        d[0] = lo16(x[0]), d[1] = lo16(x[1]), d[2] = (x[3] << 16) | lo16(x[2]);
        a[0] = x[4], a[2] = x[5], a[3] = x[6], a[4] = x[7];
        break;
    case kSpecialCaseHitTestHook:
        d[0] = lo16(x[0]), d[1] = lo16(x[1]), d[2] = lo16(x[2]), a[0] = x[3], a[3] = x[4], a[4] = x[5];
        break;
    case kSpecialCaseTEFindWord:
        d[0] = lo16(x[0]), d[2] = lo16(x[1]), a[3] = x[2], a[4] = x[3];
        break;
    case kSpecialCaseProtocolHandler:
    case kSpecialCaseSocketListener:
        for (int i = 0; i < 5; i++)
            a[i] = x[i];
        if (which == kSpecialCaseSocketListener)
            d[0] = x[5] & 0xFF, d[1] = lo16(x[6]);
        else
            d[1] = lo16(x[5]);
        break;
    case kSpecialCaseTERecalc:
        a[3] = x[0], d[7] = lo16(x[1]);
        break;
    case kSpecialCaseTEDoText:
        a[3] = x[0], d[3] = lo16(x[1]), d[4] = lo16(x[2]), d[7] = lo16(x[3]);
        break;
    case kSpecialCaseGNEFilterProc: {
        bool b = x[1] && rd8(x[1]);
        cpu_push16(c, b ? 0x0100 : 0); /* GetNextEvent's result */
        a[1] = x[0], d[0] = b;
        break;
    }
    case kSpecialCaseMBarHook:
        cpu_push32(c, x[0]);
        break;
    }
}

/* A special case's result and its results through pointers, the routine
 * returned; sp is where the 68k stack was before special_set_up. */
static uint32_t special_result(int which, const uint32_t *x, gaddr sp)
{
    cpu68k *c = g_cpu;
    uint32_t *d = c->d, *a = c->a;
    switch (which) {
    case kSpecialCaseEOLHook:
    case kSpecialCaseProtocolHandler:
    case kSpecialCaseSocketListener:
        return (c->sr & CCR_Z) != 0;
    case kSpecialCaseWidthHook:
    case kSpecialCaseNWidthHook:
        return lo16(d[1]);
    case kSpecialCaseHitTestHook:
        put16(x[6], d[0]);
        put16(x[7], d[1]);
        if (x[8])
            wr8(x[8], lo16(d[2]) != 0);
        return (d[0] >> 16) != 0;
    case kSpecialCaseTEFindWord:
        put16(x[4], d[0]);
        put16(x[5], d[1]);
        return 0;
    case kSpecialCaseTERecalc:
        put16(x[2], d[2]);
        put16(x[3], d[3]);
        put16(x[4], d[4]);
        return 0;
    case kSpecialCaseTEDoText:
        if (x[4])
            wr32(x[4], a[0]);
        put16(x[5], d[0]);
        return lo16(d[0]);
    case kSpecialCaseGNEFilterProc:
        if (x[1])
            wr8(x[1], rd16(sp - 2) != 0);
        return 0;
    case kSpecialCaseMBarHook:
        return widen(2, d[0]);
    }
    return 0;
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
    uint16_t rflags = 0;
    /* A dispatched descriptor (selectorInfo its convention): the selector
     * in D0 or D1, or pushed last, picks the routine. */
    int sconv = rd8(rd + RD_SELECTOR_INFO) & 0x0F;
    uint32_t sel = 0;
    bool has_sel = dispatched(sconv);
    if (has_sel) {
        uint32_t pi0 = rd32(rd + RD_RECORDS + RR_PROCINFO);
        if (sconv == kStackDispatchedPascalStackBased)
            sel = ((pi0 >> 6) & 3) == 3 ? rd32(c->a[7] + 4) : rd16(c->a[7] + 4);
        else
            sel = sconv == kD1DispatchedPascalStackBased ? c->d[1] : c->d[0];
        sel = selector_value(pi0, sel);
    }
    /* A fat descriptor: the PowerPC routine if PowerPC code is running. */
    if (!mm_routine_sel(rd, g_ppc_mode ? kPowerPCISA : kM68kISA, has_sel ? &sel : NULL, &pi, &isa, &proc,
                        &rflags)) {
        cpu_halt(c, "Mixed Mode: empty routine descriptor at %08X", rd);
        return;
    }
    LOGF(LOG_TRAPS, "Mixed Mode: descriptor %08X, %s routine %08X, procInfo %08X, called from %08X\n", rd,
         isa == kM68kISA ? "68k" : "PowerPC", proc, pi, rd32(c->a[7]));
    if (isa == kM68kISA) {
        cpu_push32(c, proc); /* returned to: the 68k routine, the caller's return under it */
        return;
    }
    if (!g_ppc_mode && !cfm_ppc_cpu()) {
        cpu_halt(c, "Mixed Mode: no memory to run PowerPC routine %08X", proc);
        return;
    }
    int conv = (int)(pi & 0xF), rcode = (int)((pi >> 4) & 3);
    uint32_t args[14];
    int codes[13], n = 0;
    uint32_t sp = c->a[7];
    if (conv == kPascalStackBased) {
        n = stack_codes(pi, 6, codes);
        uint32_t off = 4 + read_pascal(sp + 4, codes, n, args);
        uint32_t r = ppc_call(proc, args, n);
        if (c->halted)
            return;
        write_sized(sp + off, rcode, r);
        uint32_t ret = rd32(sp);
        c->a[7] = sp + off;
        cpu_push32(c, ret);
        return;
    }
    if (conv == kCStackBased || conv == kThinkCStackBased) {
        n = stack_codes(pi, 6, codes);
        read_c(sp + 4, conv == kThinkCStackBased, codes, n, args);
        uint32_t r = ppc_call(proc, args, n);
        if (rcode)
            c->d[0] = r;
        return;
    }
    if (dispatched(conv)) {
        /* The selector first (unless the routine doesn't take it), then the
         * parameters, Pascal's or MPW C's. */
        int scode = (int)((pi >> 6) & 3), k = 0;
        uint32_t off = 4, s;
        if (conv == kStackDispatchedPascalStackBased) {
            s = scode == 3 ? rd32(sp + off) : scode == 1 ? (uint32_t)(rd16(sp + off) >> 8) : rd16(sp + off);
            off += scode == 3 ? 4 : 2;
        } else {
            s = conv == kD1DispatchedPascalStackBased ? c->d[1] : c->d[0];
        }
        if (!(rflags & kDontPassSelector))
            args[k++] = scode ? widen(scode, s) : s;
        n = stack_codes(pi, 8, codes);
        bool pascal = conv != kD0DispatchedCStackBased;
        if (pascal)
            off += read_pascal(sp + off, codes, n, args + k);
        else
            read_c(sp + off, false, codes, n, args + k);
        uint32_t r = ppc_call(proc, args, k + n);
        if (c->halted)
            return;
        if (pascal) {
            write_sized(sp + off, rcode, r);
            uint32_t ret = rd32(sp);
            c->a[7] = sp + off;
            cpu_push32(c, ret);
        } else if (rcode) {
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
        int loc = (int)((pi >> 6) & 31);
        uint32_t *out = result_reg68(loc);
        if (rcode && out)
            store_sized(out, rcode, r);
        else if (rcode && result_ccr_bit(loc) >= 0)
            set_ccr_bit(c, result_ccr_bit(loc), widen(rcode, r) != 0);
        return;
    }
    if (conv == kSpecialCase && (int)((pi >> 4) & 0x3F) < kSpecialCaseCount) {
        special_from_68k(c, (int)((pi >> 4) & 0x3F), proc);
        return;
    }
    cpu_halt(c, "Mixed Mode: calling convention %d (procInfo %08X) from 68k code", conv, pi);
}

/* ---------------------------------------------------------------------- */
/* PowerPC code calling 68k code */

enum { SPECIAL_ARGS = 9 };

/* A special case's arguments as special_set_up and special_result take
 * them. */
static void special_args(const uint32_t *args, int nargs, uint32_t x[SPECIAL_ARGS])
{
    for (int i = 0; i < SPECIAL_ARGS; i++)
        x[i] = i < nargs ? args[i] : 0;
}

/* A call to 68k code as procInfo describes it, made ready: the arguments
 * on the 68k stack (the result's room first for Pascal's) or in its
 * registers; false for a convention we don't know. */
static bool set_up_68k(uint32_t pi, const uint32_t *args, int nargs)
{
    int conv = (int)(pi & 0xF), rcode = (int)((pi >> 4) & 3);
    if (conv == kSpecialCase) {
        int which = (int)((pi >> 4) & 0x3F);
        if (which >= kSpecialCaseCount)
            return false;
        uint32_t x[SPECIAL_ARGS];
        special_args(args, nargs, x);
        special_set_up(which, x);
    } else if (dispatched(conv) && nargs >= 1) {
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
            ppc_trap_push(scode == 3 ? 4 : scode == 1 ? 1 : 2, sel);
        else
            ppc_trap_set(conv == kD1DispatchedPascalStackBased ? 1 : 0, sel);
    } else if (conv == kPascalStackBased) {
        if (rcode)
            ppc_trap_push(rcode == 3 ? 4 : 2, 0);
        for (int i = 0; i < nargs && i < 13; i++) {
            int code_i = (int)((pi >> (6 + 2 * i)) & 3);
            ppc_trap_push(code_i == 3 ? 4 : code_i == 1 ? 1 : 2, args[i]);
        }
    } else if (conv == kCStackBased || conv == kThinkCStackBased) {
        for (int i = nargs - 1; i >= 0; i--) {
            int code_i = (int)((pi >> (6 + 2 * i)) & 3);
            ppc_trap_push(code_i == 3 || conv == kCStackBased ? 4 : code_i == 1 ? 1 : 2, args[i]);
        }
    } else if (conv == kRegisterBased) {
        for (int i = 0; i < nargs && i < 4; i++) {
            unsigned f = (pi >> (11 + 5 * i)) & 31;
            if (f & 3)
                *reg68((int)(f >> 2)) = args[i];
        }
    } else {
        return false;
    }
    return true;
}

/* The result of a call set_up_68k made ready with these arguments, the
 * 68k stack at sp before, the routine returned. */
static uint32_t result_68k(uint32_t pi, const uint32_t *args, int nargs, gaddr sp)
{
    int conv = (int)(pi & 0xF), rcode = (int)((pi >> 4) & 3);
    if (conv == kSpecialCase) {
        uint32_t x[SPECIAL_ARGS];
        special_args(args, nargs, x);
        return special_result((int)((pi >> 4) & 0x3F), x, sp);
    }
    if (dispatched(conv)) {
        bool pascal = conv != kD0DispatchedCStackBased;
        if (!rcode)
            return 0;
        return widen(rcode, pascal ? ppc_trap_result(rcode == 3 ? 4 : rcode == 1 ? 1 : 2) : g_cpu->d[0]);
    }
    if (conv == kPascalStackBased)
        return rcode ? widen(rcode, ppc_trap_result(rcode == 3 ? 4 : rcode == 1 ? 1 : 2)) : 0;
    if (conv == kCStackBased || conv == kThinkCStackBased)
        return widen(rcode ? rcode : 3, g_cpu->d[0]);
    int loc = (int)((pi >> 6) & 31);
    uint32_t *out = result_reg68(loc);
    if (rcode && out)
        return widen(rcode, *out);
    if (rcode && result_ccr_bit(loc) >= 0)
        return (g_cpu->sr >> result_ccr_bit(loc)) & 1;
    return 0;
}

/* Call 68k code at `code` as procInfo describes, with the arguments. */
static uint32_t call_68k(gaddr code, uint32_t pi, const uint32_t *args, int nargs)
{
    int conv = (int)(pi & 0xF);
    uint32_t r = 0;
    LOGF(LOG_TRAPS, "Mixed Mode: 68k routine %08X (%s), procInfo %08X, %d arguments (%08X %08X)\n", code,
         native_name_at(native_index_for(code)) ? native_name_at(native_index_for(code)) : "guest code", pi, nargs,
         nargs > 0 ? args[0] : 0, nargs > 1 ? args[1] : 0);
    ppc_trap_begin();
    running_68k++;
    gaddr sp = g_cpu->a[7];
    if (set_up_68k(pi, args, nargs)) {
        guest_call_regs(code);
        r = result_68k(pi, args, nargs, sp);
    } else {
        ppc_halt(&g_ppc, "Mixed Mode: calling convention %d (procInfo %08X) to 68k code", conv, pi);
    }
    running_68k--;
    ppc_trap_end();
    if (g_cpu->halted && !g_ppc.halted)
        ppc_halt(&g_ppc, "%s", g_cpu->halt_reason);
    return r;
}

/* ---- 68k code called at the program's own level ----
 *
 * CallUniversalProc made by a PowerPC program at its own level (drain.c:
 * its main thread, nothing of ours on the host's stack) runs the 68k
 * routine on the guest rather than from native code: the routine's
 * arguments and a return address of ours go on the 68k stack, over a
 * frame keeping the 68k registers, the PowerPC processor waits at the
 * call's return, and machine_run goes on with the 68k processor.  When
 * the routine returns to our stub, its result goes to r3, the 68k
 * registers are put back as they were and machine_run goes on with the
 * PowerPC code.  So a program whose 68k code waits for the user (Disk
 * Copy's dialogs, in ModalDialog) is at its own level there and can be
 * hibernated: what the call needs is in the frame and in level_frame. */
enum {
    LF_PROCINFO = 0,  /* the call's procInfo */
    LF_PC = 4,        /* the 68k processor's, D0-D7, A0-A7 and SR as they were */
    LF_REGS = 8,
    LF_SR = 72,
    LF_NARGS = 74,    /* the arguments, for a special case's results through pointers */
    LF_ARGS = 76,
    LF_SIZE = 76 + 4 * SPECIAL_ARGS,
};
/* The frame is in the guest's memory, so a snapshot holds it. */
static uint64_t level_frame_layout(void)
{
    static const uint32_t v[] = { LF_PROCINFO, LF_PC, LF_REGS, LF_SR, LF_NARGS, LF_ARGS, LF_SIZE };
    return bb_layout_hash(0, v, sizeof v);
}
BB_LAYOUT_FN("mixedmode.c level frame", level_frame_layout);

static gaddr level_frame;          /* the call under way's frame, or 0 */
/* The 68k return address (mixedmode_init).  Saved, as a level frame on a
 * snapshot's guest stack returns to it: another build may register it at
 * another place among the natives. */
static gaddr level_stub;
BB_HOST static bool level_switched; /* the processor machine_run runs changed */

bool mm_level_68k(void) { return level_frame != 0; }
gaddr mm_level_ctx(void) { return level_frame; }
void mm_set_level_ctx(gaddr f) { level_frame = f; }

bool mm_take_switch(void)
{
    if (!level_switched)
        return false;
    level_switched = false;
    g_ppc.stop = false;
    g_cpu->stopped = false;
    return true;
}

static void ppc_CallUniversalProc(ppc_cpu *c);
static void ppc_CallOSTrapUniversalProc(ppc_cpu *c);

/* CallUniversalProc's 68k routine, run on the guest if the call is made at
 * the program's level: true if so, the machine going on into it. */
static bool level_call_68k(ppc_cpu *c, gaddr code, uint32_t pi, const uint32_t *args, int nargs)
{
    int conv = (int)(pi & 0xF);
    /* Made by the program's own code, at its level: not by ours (a native
     * calling the export), whose caller expects the routine to have run. */
    if (level_frame || !level_stub || !drain_program_level() || native_index_for(code) >= 0 || mm_is_native(code) ||
        !(cfm_level_call_is(ppc_CallUniversalProc) || cfm_level_call_is(ppc_CallOSTrapUniversalProc)))
        return false;
    if (conv == kSpecialCase ? (int)((pi >> 4) & 0x3F) >= kSpecialCaseCount
        : dispatched(conv)   ? nargs < 1
                             : conv != kPascalStackBased && conv != kCStackBased && conv != kThinkCStackBased &&
                                 conv != kRegisterBased)
        return false;
    cpu68k *m = g_cpu;
    gaddr f = (m->a[7] - LF_SIZE) & ~1u;
    wr32(f + LF_PROCINFO, pi);
    wr32(f + LF_PC, m->pc);
    for (int i = 0; i < 8; i++) {
        wr32(f + LF_REGS + 4 * (uint32_t)i, m->d[i]);
        wr32(f + LF_REGS + 32 + 4 * (uint32_t)i, m->a[i]);
    }
    wr16(f + LF_SR, m->sr);
    wr16(f + LF_NARGS, (uint16_t)nargs);
    for (int i = 0; i < SPECIAL_ARGS; i++)
        wr32(f + LF_ARGS + 4 * (uint32_t)i, i < nargs ? args[i] : 0);
    m->a[7] = f;
    LOGF(LOG_TRAPS, "Mixed Mode: 68k routine %08X on the guest, procInfo %08X, %d arguments (%08X %08X)\n", code, pi,
         nargs, nargs > 0 ? args[0] : 0, nargs > 1 ? args[1] : 0);
    set_up_68k(pi, args, nargs);
    cpu_push32(m, level_stub);
    m->pc = code;
    running_68k++;
    level_frame = f;
    c->pc = c->lr; /* where the PowerPC code goes on, r3 the result */
    c->stop = true;
    level_switched = true;
    return true;
}

/* The 68k routine returned to the stub: its result to r3, the 68k
 * registers as they were, and the PowerPC code goes on. */
static void level_returned(cpu68k *m, uint16_t tw)
{
    (void)tw;
    gaddr f = level_frame;
    if (!f) {
        cpu_halt(m, "Mixed Mode: a return to PowerPC code with no call from it under way");
        return;
    }
    uint32_t args[SPECIAL_ARGS];
    int nargs = rd16(f + LF_NARGS);
    for (int i = 0; i < SPECIAL_ARGS; i++)
        args[i] = rd32(f + LF_ARGS + 4 * (uint32_t)i);
    g_ppc.gpr[3] = result_68k(rd32(f + LF_PROCINFO), args, nargs < SPECIAL_ARGS ? nargs : SPECIAL_ARGS, f);
    m->pc = rd32(f + LF_PC);
    for (int i = 0; i < 8; i++) {
        m->d[i] = rd32(f + LF_REGS + 4 * (uint32_t)i);
        m->a[i] = rd32(f + LF_REGS + 32 + 4 * (uint32_t)i);
    }
    m->sr = rd16(f + LF_SR);
    running_68k--;
    level_frame = 0;
    m->stopped = true;
    level_switched = true;
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
    if (conv == kSpecialCase) {
        int which = (int)((pi >> 4) & 0x3F);
        return which < kSpecialCaseCount ? special_nargs[which] : 0;
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

/* A dispatched call's routine: the selector it is called with, which picks
 * the record. */
static bool routine_for_call(gaddr upp, int want, uint32_t *pi, const uint32_t *args, int nargs, int *isa,
                             gaddr *code, bool *drop_selector)
{
    bool has_sel = dispatched((int)(*pi & 0xF)) && nargs >= 1;
    uint32_t sel = has_sel ? selector_value(*pi, args[0]) : 0;
    uint16_t rflags = 0;
    if (!mm_routine_sel(upp, want, has_sel ? &sel : NULL, pi, isa, code, &rflags))
        return false;
    *drop_selector = has_sel && (rflags & kDontPassSelector);
    return true;
}

/* Call a universal procedure pointer from native code: a descriptor's
 * PowerPC routine, or 68k code (a descriptor's or a bare pointer). */
uint32_t mm_call(gaddr upp, uint32_t pi, const uint32_t *args, int nargs)
{
    gaddr code = upp;
    if (mm_is_descriptor(upp)) {
        int isa;
        bool drop = false;
        if (!routine_for_call(upp, g_ppc_mode ? kPowerPCISA : kM68kISA, &pi, args, nargs, &isa, &code, &drop))
            return 0;
        if (drop) {
            args++, nargs--;
            if (dispatched((int)(pi & 0xF)))
                pi = undispatched(pi);
        }
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
    int skip = 0; /* a selector the routine doesn't take */
    if (mm_is_descriptor(upp)) {
        int isa;
        bool drop = false;
        if (!routine_for_call(upp, kPowerPCISA, &pi, args, n, &isa, &code, &drop)) {
            ppc_halt(c, "CallUniversalProc: empty routine descriptor at %08X", upp);
            return;
        }
        skip = drop ? 1 : 0;
        if (isa == kPowerPCISA) {
            /* A PowerPC routine: go straight there, returning to our caller,
             * with every argument moved down two places whatever either
             * procInfo counts (Amadeus II passes its plug-ins six with a
             * procInfo that says five), and r12 at the vector. */
            int most = arg_count(pi) > n ? arg_count(pi) : n;
            if (most < 6)
                most = 6;
            for (int i = 0; i < most && i < 13; i++) {
                int from = i + 2 + skip;
                uint32_t a = from < 8 ? c->gpr[3 + from] : rd32(c->gpr[1] + 24 + 4 * (uint32_t)from);
                if (i < 8)
                    c->gpr[3 + i] = a;
                else
                    wr32(c->gpr[1] + 24 + 4 * (uint32_t)i, a);
            }
            c->pc = rd32(code);
            c->gpr[2] = rd32(code + 4);
            c->gpr[12] = code;
            return;
        }
        if (drop && dispatched((int)(pi & 0xF)))
            pi = undispatched(pi);
    }
    if (!code) {
        ppc_halt(c, "CallUniversalProc: NULL routine");
        return;
    }
    if (level_call_68k(c, code, pi, args + skip, n - skip))
        return;
    c->gpr[3] = call_68k(code, pi, args + skip, n - skip);
    c->pc = c->lr;
}

/* CallOSTrapUniversalProc(theProc, procInfo, ...): as CallUniversalProc,
 * with an OS trap's registers: A1, D1 and D2 kept across 68k code. */
static void ppc_CallOSTrapUniversalProc(ppc_cpu *c)
{
    uint32_t a1 = g_cpu->a[1], d1 = g_cpu->d[1], d2 = g_cpu->d[2];
    gaddr outer = level_frame;
    ppc_CallUniversalProc(c);
    if (level_frame != outer) /* run on the guest: every register put back when it returns */
        return;
    g_cpu->a[1] = a1;
    g_cpu->d[1] = d1;
    g_cpu->d[2] = d2;
}

/* GetCurrentArchitecture / GetCurrentISA: PowerPC. */
static void ppc_GetCurrentISA(ppc_cpu *c)
{
    c->gpr[3] = kPowerPCISA;
    c->pc = c->lr;
}

const ppc_export mm_exports[] = {
    { "CallOSTrapUniversalProc", ppc_CallOSTrapUniversalProc },
    { "CallUniversalProc", ppc_CallUniversalProc },
    { "GetCurrentArchitecture", ppc_GetCurrentISA },
    { "GetCurrentISA", ppc_GetCurrentISA },
};
const unsigned mm_export_count = sizeof mm_exports / sizeof mm_exports[0];

/* ---------------------------------------------------------------------- */
/* _MixedModeDispatch ($AA59): the calls classic 68k code makes to build
 * descriptors for code it loaded through the Code Fragment Manager, and
 * the Thread Manager's saving of Mixed Mode's state with a thread's. */

gaddr tb_NewRoutineDescriptor(gaddr proc, uint32_t info, uint8_t isa) { return mm_new_descriptor(proc, info, isa & 0x0F); }

void tb_DisposeRoutineDescriptor(gaddr ptr)
{
    if (mm_is_descriptor(ptr))
        tb_DisposePtr(ptr);
}

gaddr tb_NewFatRoutineDescriptor(gaddr m68k, gaddr ppc, uint32_t info)
{
    gaddr rd = new_descriptor(2);
    if (rd) {
        set_record(rd, 0, m68k, info, kM68kISA);
        set_record(rd, 1, ppc, info, kPowerPCISA);
    }
    return rd;
}

enum { kCurrentMixedModeStateRecord = 1, MM_STATE_TAG = 0x6D6D7374 /* 'mmst' */ };

/* MixedModeStateRecord: four longs; ours hold how deep 68k code called
 * from PowerPC code runs and whether a PowerPC call has returned. */
int16_t tb_SaveMixedModeState(gaddr statep, uint32_t vers)
{
    if (vers != kCurrentMixedModeStateRecord || !statep)
        return paramErr;
    wr32(statep, MM_STATE_TAG);
    wr32(statep + 4, (uint32_t)running_68k);
    wr32(statep + 8, ppc_returned);
    wr32(statep + 12, 0);
    return noErr;
}

int16_t tb_RestoreMixedModeState(gaddr statep, uint32_t vers)
{
    if (vers != kCurrentMixedModeStateRecord || !statep || rd32(statep) != MM_STATE_TAG)
        return paramErr;
    running_68k = (int)rd32(statep + 4);
    ppc_returned = rd32(statep + 8) != 0;
    return noErr;
}

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
    ppc_trap_set(0, 0); /* a component call's selector; the caller's D0 is anything (Eudora's mask) */
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
    nprocs_68k = running_68k = 0;
    level_frame = 0;
    level_switched = false;
    level_stub = native_register(level_returned, "Mixed Mode return to PowerPC", 0);
    wr32(TB_TRAP_TABLE + 0x2FEu * 4, native_register(mixed_mode_native, "MixedModeMagic", 0xAAFE));
}
