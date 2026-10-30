#include "trap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "machine.h"
#include "lowmem_gen.h"
#include "tb_internal.h"

bool g_trap_census;

static uint16_t last_trapword; /* the trap word of the routine running now */

uint16_t trap_current_word(void) { return last_trapword; }
void trap_set_current_word(uint16_t w) { last_trapword = w; }

/* ---------------------------------------------------------------------- */
/* Natives and their ROM stubs */

#define STUB_BASE (GA_ROM + 0x1000u)
/* 68000 routines the ROM provides, below the stubs. */
#define METHOD_DISPATCH_CODE (GA_ROM + 0x800u)
/* Beyond the stubs (see trap_init): 8 MB up in 32-bit mode; half the
 * ROM up in 24-bit mode, where the ROM is smaller. */
#define EXCEPTION_JMP (GA_ROM + (g_mem_24bit ? GA_ROM_SIZE / 2 : 0x800000u))
#define STUB_SIZE 8u
#define MAX_NATIVES 8192

typedef struct {
    native_fn fn;
    const char *name;
    uint16_t trapword; /* trap this native implements, 0 if none */
} native_entry;

static native_entry natives[MAX_NATIVES];
static unsigned native_count;
static uint32_t unimpl_stub;
static uint32_t os_epilogue_stub;
/* Toolbox traps that take and return their values in registers. */
static bool tb_reg_traps[1024];
static uint32_t aline_vector_stub;

uint32_t native_register(native_fn fn, const char *name, uint16_t trapword)
{
    if (native_count >= MAX_NATIVES) {
        fprintf(stderr, "native_register: too many natives\n");
        abort();
    }
    unsigned i = native_count++;
    natives[i] = (native_entry){ fn, name, trapword };
    uint32_t a = STUB_BASE + i * STUB_SIZE;
    wr16(a, CPU_NATIVE_OP);
    wr16(a + 2, (uint16_t)i);
    wr16(a + 4, 0x4E75); /* RTS */
    wr16(a + 6, 0x4E71); /* NOP */
    return a;
}

const char *native_name_at(int index) { return index >= 0 && (unsigned)index < native_count ? natives[index].name : NULL; }

native_fn native_fn_at(int index) { return index >= 0 && (unsigned)index < native_count ? natives[index].fn : NULL; }

int native_index_for(uint32_t addr)
{
    if (addr < STUB_BASE || addr >= STUB_BASE + native_count * STUB_SIZE || (addr - STUB_BASE) % STUB_SIZE)
        return -1;
    return (int)((addr - STUB_BASE) / STUB_SIZE);
}

void trap_native(cpu68k *c, uint16_t index)
{
    if (index >= native_count) {
        cpu_halt(c, "NATIVE %u: no such native", index);
        return;
    }
    native_entry *n = &natives[index];
    /* Reached through a stub: an OS routine finds its trap word in D1. */
    uint16_t tw = n->trapword;
    if (tw && !(tw & 0x0800))
        tw = (uint16_t)c->d[1];
    uint16_t saved = last_trapword;
    last_trapword = tw;
    n->fn(c, tw);
    last_trapword = saved;
}

/* ---------------------------------------------------------------------- */
/* Names and census */

const char *trap_name(uint16_t tw)
{
    static char buf[8];
    const char *n = (tw & 0x0800) ? tb_trap_names[tw & 0x3FF] : os_trap_names[tw & 0xFF];
    if (n)
        return n;
    for (const raw_trap *r = raw_traps; r->fn; r++)
        if (((r->trap ^ tw) & 0x0800) == 0 && ((tw & 0x0800) ? (r->trap & 0x3FF) == (tw & 0x3FF) : (r->trap & 0xFF) == (tw & 0xFF)))
            return r->name;
    snprintf(buf, sizeof buf, "_%04X", tw);
    return buf;
}

#define CENSUS_SIZE 4096
static trap_count census[CENSUS_SIZE];
static unsigned census_n;

static void census_note(uint16_t trap, int32_t sel, bool implemented)
{
    /* Normalise the trap word so flag variants count together. */
    uint16_t key = (trap & 0x0800) ? (uint16_t)(0xA800 | (trap & 0x3FF)) : (uint16_t)(0xA000 | (trap & 0xFF));
    for (unsigned i = 0; i < census_n; i++) {
        if (census[i].trap == key && census[i].selector == sel) {
            census[i].calls++;
            return;
        }
    }
    if (census_n < CENSUS_SIZE)
        census[census_n++] = (trap_count){ key, sel, 1, implemented };
}

const trap_count *trap_census(unsigned *count)
{
    *count = census_n;
    return census;
}

void trap_census_reset(void) { census_n = 0; }

/* ---------------------------------------------------------------------- */
/* Selector dispatchers */

typedef struct {
    uint16_t trap;
    const char *name;
    const char *loc;
    const dispatch_sig *first;
    unsigned count;
} disp_entry;

/* Room for every dispatcher (66 now; a table that fills silently drops the
 * rest, whose traps then answer "unimplemented"). */
static disp_entry disps[256];
static unsigned ndisps;

static const disp_entry *find_disp(uint16_t tw)
{
    for (unsigned i = 0; i < ndisps; i++) {
        uint16_t t = disps[i].trap;
        bool tool = t & 0x0800;
        if (tool ? ((tw & 0x0800) && (tw & 0x3FF) == (t & 0x3FF)) : (!(tw & 0x0800) && (tw & 0xFF) == (t & 0xFF)))
            return &disps[i];
    }
    return NULL;
}

/* Discard the arguments of an unimplemented routine and return zero. */
static void census_skip(cpu68k *c, const trap_sig *s)
{
    if (s->conv == CONV_STACK) {
        uint32_t ret = cpu_pop32(c);
        c->a[7] += s->argbytes;
        if (s->retbytes == 2)
            wr16(c->a[7], 0);
        else if (s->retbytes == 4)
            wr32(c->a[7], 0);
        cpu_push32(c, ret);
    } else {
        c->d[0] = 0;
    }
}

/* QuickTime's managers: a call we do not implement answers zero (no error,
 * nothing found, nothing to show), as for a movie with nothing in it,
 * rather than stopping the application. */
static bool answers_zero(const char *dispatcher)
{
    return !strcmp(dispatcher, "QuickTime") || !strcmp(dispatcher, "ImageCompressionDispatch") ||
           !strncmp(dispatcher, "GX", 2) || !strcmp(dispatcher, "CollectionDispatch");
}

/* Dispatchers with no selector implemented, installed only for a census
 * run: GetTrapAddress still finds Unimplemented there, as it would in an
 * ordinary run, so a program testing for the trap (Tubular Worlds, for
 * the Code Fragment Manager) takes the same path. */
static uint32_t census_only[256];
static unsigned ncensus_only;

gaddr trap_visible_address(gaddr a)
{
    for (unsigned i = 0; i < ncensus_only; i++)
        if (census_only[i] == a)
            return unimpl_stub;
    return a;
}

static void dispatch_native(cpu68k *c, uint16_t tw)
{
    const disp_entry *d = find_disp(tw);
    if (!d) {
        cpu_halt(c, "dispatcher for %04X not found", tw);
        return;
    }
    const char *loc = d->loc;
    int32_t sel;
    uint32_t mask = 0xFFFFFFFFu; /* applied to the table's selectors too */
    bool on_stack = false;
    if (!strcmp(loc, "D0W"))
        sel = (int16_t)c->d[0];
    else if (!strcmp(loc, "D0L"))
        sel = (int32_t)c->d[0];
    else if (!strncmp(loc, "D0<", 3)) {
        /* SoundDispatch: older interfaces left the parameter size byte zero. */
        mask = (uint32_t)strtoul(loc + 3, NULL, 0);
        sel = (int32_t)(c->d[0] & mask);
    }
    else if (!strcmp(loc, "TrapBits"))
        sel = tw & 0x600; /* as the table gives them: Gestalt 0, NewGestalt $200, ReplaceGestalt $400 */
    else if (!strcmp(loc, "StackW") || !strncmp(loc, "StackWMasked", 12)) {
        sel = (int16_t)rd16(c->a[7] + 4);
        if (!strncmp(loc, "StackWMasked", 12))
            sel &= 0xFF;
        on_stack = true;
    } else if (!strcmp(loc, "StackL")) {
        sel = (int32_t)rd32(c->a[7] + 4);
        on_stack = true;
    } else if (!strncmp(loc, "StackWLookahead", 15)) {
        sel = rd16(c->a[7] + 4) & 0xFF;
    } else {
        cpu_halt(c, "%s: unsupported selector location %s", d->name, loc);
        return;
    }
    const dispatch_sig *s = NULL;
    for (unsigned i = 0; i < d->count; i++) {
        if (((uint32_t)d->first[i].selector & mask) == (uint32_t)sel) {
            s = &d->first[i];
            break;
        }
    }
    census_note(tw, sel, s && s->glue);
    LOGF(LOG_TRAPS, "%08X   %s selector %d -> %s\n", c->op_pc, d->name, sel, s ? s->name : "?");
    if (on_stack) {
        uint32_t ret = cpu_pop32(c);
        c->a[7] += strcmp(loc, "StackL") ? 2 : 4;
        cpu_push32(c, ret);
    }
    if (s && s->glue) {
        s->glue(c, tw);
        return;
    }
    if (!s) {
        cpu_halt(c, "%s: unknown selector %d (0x%X) at %08X", d->name, sel, (unsigned)sel, c->op_pc);
        return;
    }
    if (answers_zero(d->name)) {
        /* Called often (every frame, say): reported the first time only. */
        static const dispatch_sig *reported[256];
        static unsigned nreported;
        bool seen = false;
        for (unsigned i = 0; i < nreported && !seen; i++)
            seen = reported[i] == s;
        if (!seen) {
            if (nreported < 256)
                reported[nreported++] = s;
            LOGF(LOG_UNIMPL, "unimplemented: %s (%s selector %d) at %08X, answered zero\n", s->name, d->name, sel, c->op_pc);
        }
        census_skip(c, s);
        return;
    }
    LOGF(LOG_UNIMPL, "unimplemented: %s (%s selector %d) at %08X\n", s->name, d->name, sel, c->op_pc);
    if (!g_trap_census) {
        cpu_halt(c, "unimplemented: %s (%s selector %d)", s->name, d->name, sel);
        return;
    }
    census_skip(c, s);
}

/* ---------------------------------------------------------------------- */
/* Unimplemented traps, exceptions, OS trap epilogue */

static const trap_sig *find_sig(uint16_t tw)
{
    for (unsigned i = 0; i < trap_sig_count; i++) {
        uint16_t t = trap_sigs[i].trap;
        if ((tw & 0x0800) ? ((t & 0x0800) && (t & 0x3FF) == (tw & 0x3FF))
                          : (!(t & 0x0800) && (t & 0xFF) == (tw & 0xFF)))
            return &trap_sigs[i];
    }
    return NULL;
}

static uint16_t current_trap; /* set by the A-line fast path */

static void unimplemented_native(cpu68k *c, uint16_t tw)
{
    uint16_t t = current_trap;
    current_trap = 0;
    if (!t) {
        cpu_halt(c, "called _Unimplemented directly (pc %08X)", c->op_pc);
        return;
    }
    const trap_sig *s = find_sig(t);
    LOGF(LOG_UNIMPL, "unimplemented: %s (%04X) at %08X\n", trap_name(t), t, c->op_pc);
    if (!(t & 0x0800) && (t & 0xFF) < 0x60 && c->a[0] && rd32(c->a[0] + 18)) {
        /* File Manager calls: show the name in the parameter block. */
        char name[256];
        pstr_to_c(rd32(c->a[0] + 18), name, sizeof name);
        LOGF(LOG_UNIMPL, "  ioNamePtr \"%s\" ioVRefNum %d\n", name, (int16_t)rd16(c->a[0] + 22));
    }
    if (!g_trap_census || !s) {
        cpu_halt(c, "unimplemented trap %s (%04X)", trap_name(t), t);
        return;
    }
    census_skip(c, s);
}

static void exception_native(cpu68k *c, uint16_t tw)
{
    /* Frame: SR, PC, format/vector; a 68000's has no format word (the
     * CPU says which vector it took), and its address error frame has
     * the SR and PC after four words of its own. */
    unsigned at = c->model == 68000 && c->last_vector == 3 ? 8 : 0;
    uint16_t sr = rd16(c->a[7] + at);
    uint32_t pc = rd32(c->a[7] + at + 2);
    uint16_t fv = c->model == 68000 ? (uint16_t)(c->last_vector * 4) : rd16(c->a[7] + 6);
    static const char *const names[] = {
        [2] = "bus error", [3] = "address error", [4] = "illegal instruction",
        [5] = "divide by zero", [6] = "CHK", [7] = "TRAPV", [8] = "privilege violation",
        [9] = "trace", [10] = "A-line", [11] = "F-line (no FPU)",
    };
    unsigned v = (fv & 0xFFF) / 4;
    const char *n = v < sizeof names / sizeof names[0] && names[v] ? names[v] : "exception";
    machine_dump_68k_trace();
    cpu_halt(c, "%s (vector %u) at %08X, sr %04X, op %04X", n, v, pc, sr, rd16(pc));
}

/* Restore what the OS trap dispatcher saved, then return to the caller. */
static void os_epilogue_native(cpu68k *c, uint16_t tw)
{
    uint16_t saved_a0 = cpu_pop16(c);
    c->d[1] = cpu_pop32(c);
    c->d[2] = cpu_pop32(c);
    c->a[1] = cpu_pop32(c);
    if (saved_a0)
        c->a[0] = cpu_pop32(c);
    c->sr &= (uint16_t)~(SR_N | SR_Z | SR_V | SR_C);
    if (c->d[0] & 0x8000)
        c->sr |= SR_N;
    if ((c->d[0] & 0xFFFF) == 0)
        c->sr |= SR_Z;
    c->pc = cpu_pop32(c);
}

/* Low-memory jump vectors that a ROM fills with routines we do not need:
 * returning straight away is correct (the stub's RTS returns). */
static void noop_native(cpu68k *c, uint16_t tw) {}

/* ROMBase+$0A, the ROM's start: programs from before the Shutdown
 * Manager restart the Mac by jumping there (World Builder's games, the
 * Macintosh Basics tour, after ejecting their disks when no Finder is to
 * be found).  Here the run ends. */
static void restart_native(cpu68k *c, uint16_t tw)
{
    cpu_halt(c, "restart (jumped to the ROM's start)");
}

static void dispatch_aline(cpu68k *c, uint16_t op);

static void aline_vector_native(cpu68k *c, uint16_t tw)
{
    /* A program's own A-line handler that passes the trap on jumps here
     * with the exception frame on the stack, as to the ROM's dispatcher
     * (Lunar Commando and Grid Warrior watch for some traps and pass the
     * rest).  Take the frame and dispatch the trap at its PC. */
    uint16_t fv = c->model == 68000 ? 0x28 : rd16(c->a[7] + 6);
    uint32_t pc = rd32(c->a[7] + 2);
    if ((fv & 0xFFF) != 0x28 || (rd16(pc) & 0xF000) != 0xA000) {
        cpu_halt(c, "A-line vector entered directly");
        return;
    }
    uint16_t sr = cpu_pop16(c);
    cpu_pop32(c);
    if (c->model != 68000)
        cpu_pop16(c);
    cpu_set_sr(c, sr);
    c->op_pc = pc;
    c->pc = pc + 2;
    dispatch_aline(c, rd16(pc));
}

/* A package trap ($A9E7-$A9EE _Pack0-7, $A816 _Pack8, $A82B-$A831
 * _Pack9-15) that has no native code: as the Package Manager did, load
 * PACK n from the open resource files and run it, with the caller's
 * return address and arguments as they are (ResEdit's own PACK 1,
 * "BitEdit", for its icon editor). */
static int package_of(uint16_t tw)
{
    tw &= (uint16_t)~0x0400; /* auto-pop or not */
    if (tw >= 0xA9E7 && tw <= 0xA9EE)
        return tw - 0xA9E7;
    if (tw == 0xA816)
        return 8;
    if (tw >= 0xA82B && tw <= 0xA831)
        return tw - 0xA82B + 9;
    return -1;
}

static void package_native(cpu68k *c, uint16_t tw)
{
    int n = package_of(tw);
    gaddr h = tb_GetResource((int32_t)fourcc("PACK"), (int16_t)n);
    if (!h || !rd32(h)) {
        cpu_halt(c, "package %d (trap %04X) not found", n, tw);
        return;
    }
    /* The System's PACK is a stub that jumps through the trap table: back
     * here, for a package with no code of ours, round and round. */
    gaddr p = rd32(h);
    if (rd16(p) == 0x2F38 && rd16(p + 2) == (uint16_t)(TB_TRAP_TABLE + (tw & 0x3FFu) * 4) && rd16(p + 4) == 0x4E75) {
        current_trap = tw;
        unimplemented_native(c, tw);
        return;
    }
    tb_HNoPurge(h);
    tb_HLock(h);
    cpu_push32(c, rd32(h)); /* the dispatcher's return goes there */
}

/* ---------------------------------------------------------------------- */
/* Dispatch */

void io_complete_later(gaddr pb, int16_t result);

int16_t pb_call_done(gaddr pb, int16_t err, bool async)
{
    if (!pb)
        return err;
    wr16(pb + 16, (uint16_t)err); /* ioResult */
    /* async, with an ioCompletion; a driver call still in progress is
     * completed by the driver */
    if (async && rd32(pb + 12) && err != ioInProgress)
        io_complete_soon(pb, err);
    /* Queued: the call itself succeeds; ioResult says it runs on. */
    return async && err == ioInProgress ? noErr : err;
}
void key_trans_native(cpu68k *c, uint16_t tw);

/* The File and Device Manager traps, whose argument is a parameter block
 * (InitQueue, $A016, has none); $A8 is Mac OS 9's fork reads and writes. */
static bool param_block_trap(uint16_t op)
{
    unsigned t = op & 0xFF;
    return (t <= 0x18 && t != 0x16) || (t >= 0x41 && t <= 0x45) || t == 0x60 || t == 0xA8;
}

/* Calls from native code into the guest under way, and the one returned. */
static int call_depth, returned_depth;

void trap_aline(cpu68k *c, uint16_t op)
{
    if (rd32(0x28) != aline_vector_stub) {
        /* An app or debugger owns the A-line vector. */
        cpu_exception(c, 10, c->op_pc);
        return;
    }
    dispatch_aline(c, op);
}

static void dispatch_aline(cpu68k *c, uint16_t op)
{
    if (op & 0x0800) {
        uint32_t entry = rd32(TB_TRAP_TABLE + (op & 0x3FFu) * 4);
        int n = native_index_for(entry);
        if (n >= 0 && natives[n].fn != dispatch_native)
            census_note(op, -1, entry != unimpl_stub);
        LOGF(LOG_TRAPS, "%08X %s\n", c->op_pc, trap_name(op));
        if (!(op & 0x0400))
            cpu_push32(c, c->pc);
        if (n >= 0) {
            if (!mm_debug_before_call(trap_name(op))) {
                cpu_halt(c, "heap damaged before %s", trap_name(op));
                return;
            }
            if (!code_check_before_call(trap_name(op))) {
                cpu_halt(c, "code changed before %s", trap_name(op));
                return;
            }
            current_trap = op;
            uint16_t saved = last_trapword;
            last_trapword = op;
            natives[n].fn(c, op);
            last_trapword = saved;
            current_trap = 0;
            if (!mm_debug_after_call(trap_name(op)))
                cpu_halt(c, "heap damaged by %s", trap_name(op));
            else if (!code_check_after_call(trap_name(op)))
                cpu_halt(c, "code changed by %s", trap_name(op));
            if (tb_reg_traps[op & 0x3FF]) {
                /* Register-based routines with Toolbox numbers (HandToHand,
                 * PtrToHand ...) leave the condition codes from D0.W, as
                 * OS traps do: programs branch on them straight after. */
                c->sr &= (uint16_t)~(SR_N | SR_Z | SR_V | SR_C);
                if (c->d[0] & 0x8000)
                    c->sr |= SR_N;
                if ((c->d[0] & 0xFFFF) == 0)
                    c->sr |= SR_Z;
            }
            if (!c->halted)
                c->pc = cpu_pop32(c);
            if (call_depth == 0)
                display_trap_done();
        } else {
            c->pc = entry;
        }
        return;
    }

    /* OS trap: preserve A1, D1, D2 (and A0 unless bit 8), trap word in D1,
     * condition codes from D0.W on return. File and Device Manager calls
     * also leave their result in the parameter block's ioResult, and an
     * asynchronous one's completion routine runs. */
    uint32_t entry = rd32(OS_TRAP_TABLE + (op & 0xFFu) * 4);
    int n = native_index_for(entry);
    bool save_a0 = !(op & 0x0100);
    if (n >= 0 && natives[n].fn != dispatch_native)
        census_note(op, -1, entry != unimpl_stub);
    LOGF(LOG_TRAPS, "%08X %s\n", c->op_pc, trap_name(op));
    if (n >= 0) {
        if (!mm_debug_before_call(trap_name(op))) {
            cpu_halt(c, "heap damaged before %s", trap_name(op));
            return;
        }
        if (!code_check_before_call(trap_name(op))) {
            cpu_halt(c, "code changed before %s", trap_name(op));
            return;
        }
        uint32_t a0 = c->a[0], a1 = c->a[1], d1 = c->d[1], d2 = c->d[2];
        c->d[1] = op;
        current_trap = op;
        uint16_t saved = last_trapword;
        last_trapword = op;
        natives[n].fn(c, op);
        last_trapword = saved;
        current_trap = 0;
        if (!mm_debug_after_call(trap_name(op)))
            cpu_halt(c, "heap damaged by %s", trap_name(op));
        else if (!code_check_after_call(trap_name(op)))
            cpu_halt(c, "code changed by %s", trap_name(op));
        if (param_block_trap(op) && a0 && !c->halted)
            c->d[0] = (uint32_t)(int32_t)pb_call_done(a0, (int16_t)c->d[0], (op & 0x0400) != 0);
        c->a[1] = a1;
        c->d[1] = d1;
        c->d[2] = d2;
        if (save_a0)
            c->a[0] = a0;
        c->sr &= (uint16_t)~(SR_N | SR_Z | SR_V | SR_C);
        if (c->d[0] & 0x8000)
            c->sr |= SR_N;
        if ((c->d[0] & 0xFFFF) == 0)
            c->sr |= SR_Z;
        return;
    }
    /* Patched by guest code: build the dispatcher's frame on the guest stack. */
    cpu_push32(c, c->pc);
    if (save_a0)
        cpu_push32(c, c->a[0]);
    cpu_push32(c, c->a[1]);
    cpu_push32(c, c->d[2]);
    cpu_push32(c, c->d[1]);
    cpu_push16(c, save_a0);
    cpu_push32(c, os_epilogue_stub);
    c->d[1] = op;
    c->pc = entry;
}

/* ---------------------------------------------------------------------- */
/* Calling guest code from native code */

static uint32_t guest_return_stub;
static uint64_t icount_cap; /* 0: none */

void machine_set_instruction_limit(uint64_t total) { icount_cap = total; }
uint64_t trap_instruction_limit(void) { return icount_cap; }

/* A null event with no host attached: time passes as if the app had run,
 * so an app waiting for input (often in a Toolbox loop such as ModalDialog
 * that runs no guest code) still meets the instruction limit. */
static uint64_t last_idle; /* instruction count at the last idle moment */

bool machine_waiting_for_input(void) { return g_cpu && g_cpu->icount - last_idle < 2000000; }

void machine_idle(void) { machine_idle_for(-1); }

/* The wait for the user's answer to an alert that headless answers itself
 * (its default button): time passes as for any wait, but the menu walk
 * takes no step, as the alert is in front of everything and the window
 * behind (a dialog the program is between ModalDialog calls in: Moose
 * Frazer's) is not the program's to take input from now. */
static bool answering;

void machine_idle_answered(void)
{
    answering = true;
    machine_idle_for(-1);
    answering = false;
}

/* The same for WaitNextEvent's sleep, in ticks (negative: as long as the
 * script lets it): the clock moves on no further than the program asked
 * to sleep, so a program doing its work between null events (the Finder
 * filling its windows) gets them as often as it did on a Mac. */
void machine_idle_for(int32_t sleep)
{
    cpu68k *c = g_cpu;
    if (!c || c->halted)
        return;
    last_idle = c->icount;
    c->icount += 1000;
    uint32_t presses = ev_press_count();
    if (!answering)
        walker_idle(c->icount);
    if (c->halted)
        return;
    /* A click the walk just gave is the program's to see: the clock moving
     * on to its scripted release would let go of the button before a
     * program polling Button (a title waiting for a click) looked. */
    if (ev_press_count() != presses)
        return;
    /* Waiting: scripted input comes now, or the clock moves on to it. */
    uint64_t next = events_script_next_at();
    if (sleep >= 0) {
        uint64_t slept = c->icount + (uint64_t)(sleep ? sleep : 1) * 166667u; /* a tick at 10 MIPS */
        if (!next || slept < next)
            next = slept;
        /* The menu walk's next step is input too and ends the sleep there:
         * a program sleeping long between events (60 ticks, or 300) would
         * otherwise spend that long on each step and run out of
         * instructions before the walk was done. */
        uint64_t step = walker_next_in();
        if (step) {
            uint64_t at = c->icount + (step + 99) / 100; /* 100 ns an instruction */
            if (at < next)
                next = at;
        }
    }
    if (next > c->icount && (!icount_cap || next < icount_cap))
        c->icount = next;
    events_script_pump(c->icount);
    events_sync_lowmem();
    if (icount_cap && c->icount >= icount_cap)
        cpu_halt(c, "instruction limit reached waiting for input");
}
/* A tick spent in a Toolbox loop (mouse tracking) with no host attached. */
void machine_pass_tick(void)
{
    cpu68k *c = g_cpu;
    if (!c || c->halted)
        return;
    c->icount += 20000;
    events_script_pump(c->icount);
    if (icount_cap && c->icount >= icount_cap)
        cpu_halt(c, "instruction limit reached");
}

/* The nesting of calls into guest code belongs to the thread making them:
 * the Thread Manager switches it with the registers. */
void trap_ctx_save(trap_ctx *t)
{
    t->call_depth = call_depth;
    t->returned_depth = returned_depth;
    t->current_trap = current_trap;
    t->last_trapword = last_trapword;
}

void trap_ctx_load(const trap_ctx *t)
{
    call_depth = t->call_depth;
    returned_depth = t->returned_depth;
    current_trap = t->current_trap;
    last_trapword = t->last_trapword;
}

static void guest_return_native(cpu68k *c, uint16_t tw) { returned_depth = call_depth; }

/* Call fn and return with the registers it left (the caller saves what it needs). */
void guest_call_regs(gaddr fn)
{
    cpu68k *c = g_cpu;
    uint32_t saved_pc = c->pc;
    int n = native_index_for(fn);
    if (n >= 0) {
        /* Our own routine: run it directly; it leaves the return address. */
        cpu_push32(c, guest_return_stub);
        natives[n].fn(c, natives[n].trapword);
        cpu_pop32(c);
    } else {
        int depth = ++call_depth;
        cpu_push32(c, guest_return_stub);
        c->pc = fn;
        uint64_t pumped = c->icount;
        while (!c->halted && returned_depth != depth) {
            cpu_step(c);
            /* Interrupt-time work goes on inside callbacks too: a program
             * may wait there for Ticks to move or a VBL task's flag. */
            if (c->icount - pumped >= 20000) {
                pumped = c->icount;
                machine_advance_ticks();
                time_pump();
            }
            if (icount_cap && c->icount >= icount_cap) {
                if (machine_waiting_for_input()) /* an event loop inside a callback */
                    cpu_halt(c, "instruction limit reached waiting for input");
                else
                    cpu_halt(c, "instruction limit reached in a call from the Toolbox (pc %08X)", c->pc);
            }
        }
        returned_depth = 0;
        call_depth--;
    }
    c->pc = saved_pc;
}

/* Call a low-memory hook (MenuHook, DragHook, DeskHook ...): D0 and A0 as
 * given, any stack arguments already pushed; every register, A7 included,
 * is as before afterwards.  Returns D0 as the hook left it. */
uint32_t hook_call(gaddr fn, uint32_t d0, uint32_t a0, uint32_t sp)
{
    cpu68k *c = g_cpu;
    uint32_t d[8], a[8];
    memcpy(d, c->d, sizeof d);
    memcpy(a, c->a, sizeof a);
    a[7] = sp; /* below any arguments pushed for the hook */
    uint16_t sr = c->sr;
    c->d[0] = d0;
    c->a[0] = a0;
    guest_call_regs(fn);
    uint32_t result = c->d[0];
    memcpy(c->d, d, sizeof d);
    memcpy(c->a, a, sizeof a);
    c->sr = sr;
    return result;
}

/* Call fn with the registers kept, the status register (condition codes)
 * included: tasks run between two instructions, as an interrupt would,
 * must not change a comparison the interrupted code is about to branch on. */
void guest_call(gaddr fn)
{
    cpu68k *c = g_cpu;
    uint32_t d[8], a[7];
    uint16_t sr = c->sr;
    memcpy(d, c->d, sizeof d);
    memcpy(a, c->a, sizeof a);
    guest_call_regs(fn);
    memcpy(c->d, d, sizeof d);
    memcpy(c->a, a, sizeof a);
    c->sr = sr;
}

/* NewHandle (clear) through the trap table when a program has patched it,
 * as the Toolbox's own allocations went: the RampChamp demo, Kenzo and
 * MACBOY Organiser patch NewHandle around each NewGWorld to place its
 * pixels, and their patch takes itself out when it is called. */
static gaddr new_handle_stub; /* made when first needed, after each boot */

gaddr trap_new_handle_clear(int32_t size)
{
    cpu68k *c = g_cpu;
    if (!c || native_index_for(rd32(OS_TRAP_TABLE + 0x22u * 4)) >= 0)
        return tb_NewHandle(size, false, true);
    gaddr stub = new_handle_stub;
    if (!stub) {
        stub = mm_new_ptr_in(rd32(LM_SysZone), 4, false);
        if (!stub)
            return tb_NewHandle(size, false, true);
        wr16(stub, 0xA322); /* _NewHandle,CLEAR */
        wr16(stub + 2, 0x4E75); /* RTS */
        new_handle_stub = stub;
    }
    uint32_t d[8], a[8];
    memcpy(d, c->d, sizeof d);
    memcpy(a, c->a, sizeof a);
    uint16_t sr = c->sr;
    c->d[0] = (uint32_t)size;
    guest_call_regs(stub);
    gaddr h = (int16_t)c->d[0] == noErr ? c->a[0] : 0;
    memcpy(c->d, d, sizeof d);
    memcpy(c->a, a, sizeof a);
    c->sr = sr;
    return h;
}

void guest_call_defproc(gaddr fn)
{
    cpu68k *c = g_cpu;
    uint32_t a0 = c->a[0];
    c->a[0] = fn;
    guest_call(fn);
    c->a[0] = a0;
}

/* ---------------------------------------------------------------------- */
/* Setup */

/* OS traps with separate glue for a variant in their flag bits, as
 * [slot][bits $0600 >> 9]: the HFS variant (bit $0200: PBHOpen beside
 * PBOpen …), or InsXTime beside InsTime (bit $0400). */
static native_fn os_variants[256][4];

static void os_variant_native(cpu68k *c, uint16_t tw)
{
    /* Bit $0400 is otherwise a flag of the call (async, sys, clear): the
     * glue without it, then whichever there is. */
    unsigned v = (tw >> 9) & 3u;
    native_fn fn = os_variants[tw & 0xFF][v];
    if (!fn)
        fn = os_variants[tw & 0xFF][v & 1u];
    for (unsigned i = 0; !fn && i < 4; i++)
        fn = os_variants[tw & 0xFF][i];
    fn(c, tw);
}

void trap_init(void)
{
    native_count = 0;
    ndisps = 0;
    census_n = 0;
    current_trap = 0;

    unimpl_stub = native_register(unimplemented_native, "Unimplemented", 0xA89F);
    ncensus_only = 0;
    new_handle_stub = 0;
    guest_return_stub = native_register(guest_return_native, "GuestReturn", 0);
    call_depth = returned_depth = 0;
    os_epilogue_stub = native_register(os_epilogue_native, "OSTrapEpilogue", 0);
    aline_vector_stub = native_register(aline_vector_native, "ALineVector", 0);

    for (unsigned i = 0; i < 256; i++)
        wr32(OS_TRAP_TABLE + i * 4, unimpl_stub);
    for (unsigned i = 0; i < 1024; i++)
        wr32(TB_TRAP_TABLE + i * 4, unimpl_stub);

    /* Generated glue for implemented, undispatched traps.  OS traps whose
     * variant has its own glue share a slot, so the slot goes to a native
     * that picks by the flag bits.  Picking by bit $0200 alone gave
     * InsTime InsXTime's glue, and an ordinary task primed again went on
     * from its last wake-up: MacMan, primed after a three-second pause,
     * then ran a frame every slice until it had caught up. */
    memset(os_variants, 0, sizeof os_variants);
    for (unsigned i = 0; i < trap_sig_count; i++) {
        const trap_sig *s = &trap_sigs[i];
        if (!s->glue)
            continue;
        if (s->trap & 0x0800) {
            wr32(TB_TRAP_TABLE + (s->trap & 0x3FFu) * 4, native_register(s->glue, s->name, s->trap));
            tb_reg_traps[s->trap & 0x3FFu] = s->conv == CONV_REG;
            continue;
        }
        unsigned slot = s->trap & 0xFFu;
        os_variants[slot][(s->trap >> 9) & 3u] = s->glue;
        bool shared = false;
        for (unsigned v = 0; v < 4; v++)
            shared |= os_variants[slot][v] && os_variants[slot][v] != s->glue;
        if (shared)
            wr32(OS_TRAP_TABLE + slot * 4, native_register(os_variant_native, s->name, (uint16_t)(0xA000 | slot)));
        else
            wr32(OS_TRAP_TABLE + slot * 4, native_register(s->glue, s->name, s->trap));
    }

    /* Hand-written natives that manage the stack themselves. */
    for (const raw_trap *r = raw_traps; r->fn; r++) {
        uint32_t stub = native_register(r->fn, r->name, r->trap);
        if (r->trap & 0x0800)
            wr32(TB_TRAP_TABLE + (r->trap & 0x3FFu) * 4, stub);
        else
            wr32(OS_TRAP_TABLE + (r->trap & 0xFFu) * 4, stub);
    }

    /* Packages without native code come from PACK resources. */
    for (unsigned t = 0xA816; t <= 0xA9EE; t++)
        if (package_of((uint16_t)t) >= 0 && rd32(TB_TRAP_TABLE + (t & 0x3FFu) * 4) == unimpl_stub)
            wr32(TB_TRAP_TABLE + (t & 0x3FFu) * 4, native_register(package_native, "Package", (uint16_t)t));

    /* MethodDispatch (A9F8): the MPW Object Pascal method dispatcher that
     * the 128K and later ROMs carried.  The runtime library's own copy is
     * used on 64K ROMs; on later ones its start-up code stores the class
     * table pointer in 0xBEC and the error routine in 0xBE8 instead of in
     * its A5 world and calls the trap.  This is that same routine, in 68000
     * code, reading the two globals.  The caller's inline data after the
     * JSR is a count and a method cache of (class ID, offset) pairs; the
     * object handle is the second long on the stack. */
    static const uint16_t method_dispatch[] = {
        0x205F,         /* move.l (sp)+,a0      inline data */
        0x226F, 0x0004, /* move.l 4(sp),a1      the object */
        0x2251,         /* move.l (a1),a1 */
        0x3011,         /* move.w (a1),d0       its class ID */
        0x3218,         /* move.w (a0)+,d1      cache entries - 1 */
        0xB050,         /* cmp.w (a0),d0        the last class seen? */
        0x6612,         /* bne.s search */
        0x3028, 0x0002, /* found: move.w 2(a0),d0 */
        0x0880, 0x0000, /* bclr #0,d0           odd: relative to the cache */
        0x6604,         /* bne.s relative */
        0x4EF5, 0x0000, /* jmp 0(a5,d0.w)       even: a jump table entry */
        0x4EF0, 0x0002, /* relative: jmp 2(a0,d0.w) */
        0x3080,         /* search: move.w d0,(a0) */
        0x2408,         /* move.l a0,d2 */
        0x5888,         /* 1: addq.l #4,a0 */
        0xB050,         /* cmp.w (a0),d0 */
        0x54C9, 0xFFFA, /* dbcc d1,1b */
        0x6722,         /* beq.s hit */
        0x2278, 0x0BEC, /* move.l $BEC,a1       class table */
        0x2251,         /* move.l (a1),a1 */
        0x600A,         /* bra.s super */
        0x5888,         /* 2: addq.l #4,a0 */
        0xB050,         /* cmp.w (a0),d0 */
        0x54C9, 0xFFFA, /* dbcc d1,2b */
        0x6710,         /* beq.s hit */
        0x3031, 0x0000, /* super: move.w 0(a1,d0.w),d0  the superclass */
        0x66F2,         /* bne.s 2b */
        0x2242,         /* move.l d2,a1         not understood */
        0x4291,         /* clr.l (a1) */
        0x2278, 0x0BE8, /* move.l $BE8,a1       error routine */
        0x4ED1,         /* jmp (a1) */
        0x2242,         /* hit: move.l d2,a1 */
        0x3368, 0x0002, 0x0002, /* move.w 2(a0),2(a1)  cache it */
        0x60B4,         /* bra.s found */
    };
    for (unsigned i = 0; i < sizeof method_dispatch / sizeof method_dispatch[0]; i++)
        wr16(METHOD_DISPATCH_CODE + i * 2, method_dispatch[i]);
    wr32(TB_TRAP_TABLE + 0x1F8u * 4, METHOD_DISPATCH_CODE);

    /* Selector dispatchers: installed when any selector is implemented
     * (or always, in census mode, so arguments can be discarded). */
    for (unsigned i = 0; i < dispatcher_count; i++) {
        const dispatcher_info *di = &dispatchers[i];
        const dispatch_sig *first = NULL;
        unsigned count = 0;
        bool any = false;
        for (unsigned j = 0; j < dispatch_sig_count; j++) {
            if (dispatch_sigs[j].trap != di->trap)
                continue;
            if (!first)
                first = &dispatch_sigs[j];
            count++;
            if (dispatch_sigs[j].glue)
                any = true;
        }
        if (!first || ndisps >= sizeof disps / sizeof disps[0])
            continue;
        disps[ndisps++] = (disp_entry){ di->trap, di->name, di->selector_location, first, count };
        if (!any && !g_trap_census && !answers_zero(di->name))
            continue;
        /* A hand-written native for the whole trap (FP68K, Elems68K) wins. */
        bool raw = false;
        for (const raw_trap *r = raw_traps; r->fn && !raw; r++)
            raw = r->trap == di->trap;
        if (raw)
            continue;
        uint32_t stub = native_register(dispatch_native, di->name, di->trap);
        if (!any && !answers_zero(di->name) && ncensus_only < sizeof census_only / sizeof census_only[0])
            census_only[ncensus_only++] = stub;
        if (di->trap & 0x0800)
            wr32(TB_TRAP_TABLE + (di->trap & 0x3FFu) * 4, stub);
        else
            wr32(OS_TRAP_TABLE + (di->trap & 0xFFu) * 4, stub);
    }

    /* ROM vectors inside the OS trap table area.  jFlush (cache flush) is
     * called after code is loaded; there is no cache to flush. */
    uint32_t noop = native_register(noop_native, "NoOp", 0);
    wr32(LM_JFLUSH, noop);
    wr32(LM_JResUnknown1, noop);
    wr32(LM_JResUnknown2, noop);

    /* Jump vectors to ROM routines that are also traps, which programs
     * call directly (StarGuardian calls FMSwapFont through JSwapFont):
     * the trap's own entry, which a JSR reaches as the trap would. */
    static const struct {
        uint32_t lm;
        uint16_t trap;
    } jumps[] = {
        { LM_JInitCrsr, 0xA850 },   { LM_JSetCrsr, 0xA851 },      { LM_JHideCursor, 0xA852 },
        { LM_JShowCursor, 0xA853 }, { LM_JShieldCursor, 0xA855 }, { LM_JCrsrObscure, 0xA856 },
        { LM_JSwapFont, 0xA901 },
    };
    for (unsigned i = 0; i < sizeof jumps / sizeof jumps[0]; i++) {
        uint32_t entry = rd32(TB_TRAP_TABLE + (jumps[i].trap & 0x3FFu) * 4);
        if (entry != unimpl_stub)
            wr32(jumps[i].lm, entry);
    }
    /* $AB03: no routine, but its presence is how programs from before
     * Gestalt (Prince of Persia) learn that 32-Bit QuickDraw is there: its
     * address differs from Unimplemented's.  Called, it does nothing. */
    mixedmode_init();
    adb_install();
    if (rd32(TB_TRAP_TABLE + 0x303u * 4) == unimpl_stub)
        wr32(TB_TRAP_TABLE + 0x303u * 4, native_register(noop_native, "QD32Present", 0xAB03));
    uint32_t keytrans = native_register(key_trans_native, "KeyTrans", 0);
    wr32(LM_Key1Trans, keytrans);
    wr32(LM_Key2Trans, keytrans);
    /* DTInstall's entry for JDTInstall; the cursor and ADB tasks have
     * nothing to do. */
    uint32_t dt = rd32(OS_TRAP_TABLE + 0x82u * 4);
    if (dt != unimpl_stub)
        wr32(LM_JDTInstall, dt);
    wr32(LM_JCrsrTask, noop);
    wr32(LM_JADBProc, noop);

    /* Code that runs off the end of memory, or jumps wild into the ROM's
     * header, stops at ILLEGALs in the reset vector's place instead of
     * running on into the restart. */
    wr32(GA_ROM + 4, 0x4AFC4AFC);
    wr16(GA_ROM + 0x0A, 0x4EF9); /* JMP */
    wr32(GA_ROM + 0x0C, native_register(restart_native, "Restart", 0));

    /* Exception vectors 2..63 (A-line gets its own marker stub). */
    uint32_t exc = native_register(exception_native, "Exception", 0);
    /* Through a JMP at a ROM-like address ($xx8xxxxx, as a real ROM's
     * $408xxxxx): programs that read through a bad pointer into the
     * vectors find nonzero bytes where a Mac had them (Continuum's sound
     * task, at the end of a level, tests the byte at $39 and calls the
     * vector at $1C when it is zero). */
    wr16(EXCEPTION_JMP, 0x4EF9);
    wr32(EXCEPTION_JMP + 2, exc);
    for (unsigned v = 2; v < 64; v++)
        wr32(v * 4, EXCEPTION_JMP);
    wr32(10 * 4, aline_vector_stub);
}

/* The interrupt autovectors (25..31, $64..$7F), which nothing here takes,
 * through a JMP in the system heap: a Mac's pointed into ROM at $408xxxxx,
 * a positive word, and a program reading a window's fields through nil
 * finds windowKind ($6C) there.  Snoop, its only window hidden, sends the
 * front window behind while its kind is negative (a desk accessory's). */
void trap_autovectors(void)
{
    gaddr stub = mm_new_ptr_in(rd32(LM_SysZone), 6, false);
    if (!stub)
        return;
    wr16(stub, 0x4EF9); /* JMP */
    wr32(stub + 2, EXCEPTION_JMP);
    for (unsigned v = 25; v < 32; v++)
        wr32(v * 4, stub);
    /* TRAP #0-#15 (32..47) return at once: Mac OS used none of them, and
     * Apple HD SC Setup makes A/UX's getuid call (TRAP #0) whether or not
     * A/UX is running, then ignores the answer when it isn't. */
    gaddr rte = mm_new_ptr_in(rd32(LM_SysZone), 2, false);
    if (!rte)
        return;
    wr16(rte, 0x4E73); /* RTE */
    for (unsigned v = 32; v < 48; v++)
        wr32(v * 4, rte);
}
