#include "trap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "lowmem_gen.h"
#include "tb_internal.h"

bool g_trap_census;

static uint16_t last_trapword; /* the trap word of the routine running now */

uint16_t trap_current_word(void) { return last_trapword; }

/* ---------------------------------------------------------------------- */
/* Natives and their ROM stubs */

#define STUB_BASE (GA_ROM + 0x1000u)
/* 68000 routines the ROM provides, below the stubs. */
#define METHOD_DISPATCH_CODE (GA_ROM + 0x800u)
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

static disp_entry disps[64];
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
    return !strcmp(dispatcher, "QuickTime") || !strcmp(dispatcher, "ImageCompressionDispatch");
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
        sel = (tw >> 9) & 3;
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
    /* Frame: SR, PC, format/vector. */
    uint16_t sr = rd16(c->a[7]);
    uint32_t pc = rd32(c->a[7] + 2);
    uint16_t fv = rd16(c->a[7] + 6);
    static const char *const names[] = {
        [2] = "bus error", [3] = "address error", [4] = "illegal instruction",
        [5] = "divide by zero", [6] = "CHK", [7] = "TRAPV", [8] = "privilege violation",
        [9] = "trace", [10] = "A-line", [11] = "F-line (no FPU)",
    };
    unsigned v = (fv & 0xFFF) / 4;
    const char *n = v < sizeof names / sizeof names[0] && names[v] ? names[v] : "exception";
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

static void aline_vector_native(cpu68k *c, uint16_t tw)
{
    /* Only reached if something jumps to the A-line vector itself. */
    cpu_halt(c, "A-line vector entered directly");
}

/* ---------------------------------------------------------------------- */
/* Dispatch */

void trap_aline(cpu68k *c, uint16_t op)
{
    if (rd32(0x28) != aline_vector_stub) {
        /* An app or debugger owns the A-line vector. */
        cpu_exception(c, 10, c->op_pc);
        return;
    }
    if (op & 0x0800) {
        uint32_t entry = rd32(TB_TRAP_TABLE + (op & 0x3FFu) * 4);
        int n = native_index_for(entry);
        if (n >= 0 && natives[n].fn != dispatch_native)
            census_note(op, -1, entry != unimpl_stub);
        LOGF(LOG_TRAPS, "%08X %s\n", c->op_pc, trap_name(op));
        if (!(op & 0x0400))
            cpu_push32(c, c->pc);
        if (n >= 0) {
            current_trap = op;
            uint16_t saved = last_trapword;
            last_trapword = op;
            natives[n].fn(c, op);
            last_trapword = saved;
            current_trap = 0;
            if (!c->halted)
                c->pc = cpu_pop32(c);
        } else {
            c->pc = entry;
        }
        return;
    }

    /* OS trap: preserve A1, D1, D2 (and A0 unless bit 8), trap word in D1,
     * condition codes from D0.W on return. */
    uint32_t entry = rd32(OS_TRAP_TABLE + (op & 0xFFu) * 4);
    int n = native_index_for(entry);
    bool save_a0 = !(op & 0x0100);
    if (n >= 0 && natives[n].fn != dispatch_native)
        census_note(op, -1, entry != unimpl_stub);
    LOGF(LOG_TRAPS, "%08X %s\n", c->op_pc, trap_name(op));
    if (n >= 0) {
        uint32_t a0 = c->a[0], a1 = c->a[1], d1 = c->d[1], d2 = c->d[2];
        c->d[1] = op;
        current_trap = op;
        uint16_t saved = last_trapword;
        last_trapword = op;
        natives[n].fn(c, op);
        last_trapword = saved;
        current_trap = 0;
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

/* A null event with no host attached: time passes as if the app had run,
 * so an app waiting for input (often in a Toolbox loop such as ModalDialog
 * that runs no guest code) still meets the instruction limit. */
static uint64_t last_idle; /* instruction count at the last idle moment */

bool machine_waiting_for_input(void) { return g_cpu && g_cpu->icount - last_idle < 2000000; }

void machine_idle(void)
{
    cpu68k *c = g_cpu;
    if (!c || c->halted)
        return;
    last_idle = c->icount;
    c->icount += 1000;
    if (icount_cap && c->icount >= icount_cap)
        cpu_halt(c, "instruction limit reached waiting for input");
}
static int call_depth, returned_depth;

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
        while (!c->halted && returned_depth != depth) {
            cpu_step(c);
            if (icount_cap && c->icount >= icount_cap)
                cpu_halt(c, "instruction limit reached in a call from the Toolbox (pc %08X)", c->pc);
        }
        returned_depth = 0;
        call_depth--;
    }
    c->pc = saved_pc;
}

void guest_call(gaddr fn)
{
    cpu68k *c = g_cpu;
    uint32_t d[8], a[7];
    memcpy(d, c->d, sizeof d);
    memcpy(a, c->a, sizeof a);
    guest_call_regs(fn);
    memcpy(c->d, d, sizeof d);
    memcpy(c->a, a, sizeof a);
}

/* ---------------------------------------------------------------------- */
/* Setup */

/* OS traps with separate glue for their HFS variant: [slot][bit $0200]. */
static native_fn os_variants[256][2];

static void os_variant_native(cpu68k *c, uint16_t tw)
{
    native_fn fn = os_variants[tw & 0xFF][(tw & 0x0200) ? 1 : 0];
    if (!fn)
        fn = os_variants[tw & 0xFF][(tw & 0x0200) ? 0 : 1];
    fn(c, tw);
}

void trap_init(void)
{
    native_count = 0;
    ndisps = 0;
    census_n = 0;
    current_trap = 0;

    unimpl_stub = native_register(unimplemented_native, "Unimplemented", 0xA89F);
    guest_return_stub = native_register(guest_return_native, "GuestReturn", 0);
    call_depth = returned_depth = 0;
    os_epilogue_stub = native_register(os_epilogue_native, "OSTrapEpilogue", 0);
    aline_vector_stub = native_register(aline_vector_native, "ALineVector", 0);

    for (unsigned i = 0; i < 256; i++)
        wr32(OS_TRAP_TABLE + i * 4, unimpl_stub);
    for (unsigned i = 0; i < 1024; i++)
        wr32(TB_TRAP_TABLE + i * 4, unimpl_stub);

    /* Generated glue for implemented, undispatched traps.  OS traps whose
     * HFS variant (bit $0200: PBHOpen beside PBOpen …) has its own glue
     * share a slot, so the slot goes to a native that picks by that bit. */
    memset(os_variants, 0, sizeof os_variants);
    for (unsigned i = 0; i < trap_sig_count; i++) {
        const trap_sig *s = &trap_sigs[i];
        if (!s->glue)
            continue;
        if (s->trap & 0x0800) {
            wr32(TB_TRAP_TABLE + (s->trap & 0x3FFu) * 4, native_register(s->glue, s->name, s->trap));
            continue;
        }
        unsigned slot = s->trap & 0xFFu;
        os_variants[slot][(s->trap & 0x0200) ? 1 : 0] = s->glue;
        native_fn other = os_variants[slot][(s->trap & 0x0200) ? 0 : 1];
        if (other && other != s->glue)
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

    /* Exception vectors 2..63 (A-line gets its own marker stub). */
    uint32_t exc = native_register(exception_native, "Exception", 0);
    for (unsigned v = 2; v < 64; v++)
        wr32(v * 4, exc);
    wr32(10 * 4, aline_vector_stub);
}
