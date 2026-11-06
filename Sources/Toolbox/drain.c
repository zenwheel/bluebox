/* Draining the machine to a safe point: where its whole state is in the
 * saved sections, the tracked blocks and the guest's memory, and none of
 * it in the host's stack, so a snapshot of it can be resumed in another
 * process (hibernate.c), and Suspend stops there too when it can.
 *
 * Asked for (machine_drain_begin), the machine's thread runs on to:
 *   - an idle call at the program's own level (WaitNextEvent,
 *     GetNextEvent, SystemTask, ModalDialog, an alert, Delay; for PowerPC
 *     code ReceiveNextEvent and RunApplicationEventLoop too): the CPU
 *     stops at the trap, or on the import's NATIVE word, the program
 *     counter back on it, the call not made, so a resumed program makes it
 *     again;
 *   - or the next boundary between emulation slices (for a snapshot the
 *     run ends with, only after a grace of DRAIN_GRACE instructions with no
 *     idle call): machine_run's own loop,
 *     with cpu_run or ppc_run returned, so nothing of the Toolbox is on
 *     the host's stack (a call into guest code or a nested trap has
 *     returned by then).
 * Meanwhile the Toolbox's waits end early (drain_wanted): WaitNextEvent's
 * sleep, Delay's, the mouse-tracking loops (the mouse reads as let go,
 * outside everything, so each cancels as if released there); a native
 * stopped mid-way with drain_abandon has the registers put back as they
 * were at its trap and the program counter on the trap (trap.c), or for
 * PowerPC code the registers as they were at the call and the program
 * counter on its NATIVE word (cfm.c), to be made again after resuming.
 *
 * The program's own level: its main thread runs (the Thread Manager's
 * others wait, each in a call of its own level, threadmgr.c) and no host
 * frame runs guest code under the call: no routine of the program's the
 * Toolbox called (guest_call), no PowerPC code a 68k program called
 * through Mixed Mode (ppc_call), no 68k trap run for PowerPC code
 * (ppc_trap: a patch of the program's own, or a trap with no PowerPC
 * glue).  A PowerPC call abandoned inside a trap run for it unwinds to the
 * PowerPC call, which is the one made again.
 *
 * Refused (with a reason the host can show) when no safe point comes in
 * DRAIN_TIMEOUT_NS of the host's time (the program in a callback from
 * the Toolbox that never returns, or a host wait), or when the program
 * has what a snapshot cannot carry (machine_hibernate_blocker).  A
 * suspend refused still suspends, wherever the machine stops; only a
 * snapshot needs the safe point.
 *
 * This is the host's control state, never saved: a resumed process starts
 * with none of it. */
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "machine.h"
#include "ppc.h"
#include "tb_internal.h"
#include "trap.h"

#define DRAIN_GRACE 5000000u                  /* instructions an idle call may take to come */
#define DRAIN_TIMEOUT_NS 2000000000ull         /* then refused, once the machine has run DRAIN_LEAST too */
#define DRAIN_LEAST 50000000u                  /* instructions: a busy host's slow machine gets as far */
#define DRAIN_LONGEST_NS 10000000000ull        /* refused then whatever it ran (a host wait) */

BB_HOST static atomic_int state;               /* MACHINE_DRAIN_* */
BB_HOST static bool exits;                     /* machine_run returns RUN_DRAINED at the safe point */
BB_HOST static uint64_t deadline;              /* the host's monotonic clock */
BB_HOST static uint64_t begun_ns, begun_at;    /* the host's clock and the instruction count when asked */
BB_HOST static uint64_t grace_until;           /* the instruction count */
BB_HOST static bool stopped_here;              /* the CPU stopped at a trap for the drain */
BB_HOST static bool rewind_wanted;             /* a native abandoned its call */
BB_HOST static char why[256];
BB_HOST static uint64_t hibernate_at;          /* --hibernate-at: the instruction count, or 0 */
BB_HOST static char stopped_at[64];            /* where it stopped (for the log) */
BB_HOST static char last_where[256];           /* what kept it from a safe point at its last wait */

static uint64_t host_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

void machine_drain_begin(bool exit_run)
{
    if (atomic_load(&state) == MACHINE_DRAIN_GOING) {
        exits = exits || exit_run;
        return;
    }
    exits = exit_run;
    begun_ns = host_ns();
    begun_at = g_cpu ? g_cpu->icount : 0;
    deadline = begun_ns + DRAIN_TIMEOUT_NS;
    /* A snapshot taken where the program waits for its next event is the
     * likeliest to go on well; Suspend wants the machine still now. */
    grace_until = exit_run && g_cpu ? g_cpu->icount + DRAIN_GRACE : 0;
    stopped_here = rewind_wanted = false;
    why[0] = stopped_at[0] = last_where[0] = 0;
    atomic_store(&state, MACHINE_DRAIN_GOING);
}

void machine_drain_end(void)
{
    atomic_store(&state, MACHINE_DRAIN_NONE);
    exits = stopped_here = rewind_wanted = false;
    g_ppc.stop = false;
}

int machine_drain_state(void) { return atomic_load(&state); }
const char *machine_drain_reason(void) { return why; }
const char *machine_drain_point(void) { return stopped_at; }
bool machine_hibernatable(void) { return atomic_load(&state) == MACHINE_DRAIN_PARKED; }

void machine_hibernate_at(uint64_t icount) { hibernate_at = icount; }

bool drain_wanted(void) { return atomic_load(&state) == MACHINE_DRAIN_GOING; }

bool drain_program_level(void)
{
    return thread_current_index() == 0 && trap_call_depth() == 0 && mm_call_depth() == 0 && ppc_trap_depth() == 0;
}

/* Where a native may abandon its call: at the program's level, or for a
 * PowerPC program inside a 68k trap run for its call at that level (the
 * PowerPC call is put back, cfm.c). */
static bool abandon_level(void)
{
    if (thread_current_index() != 0 || trap_call_depth() != 0 || mm_call_depth() != 0)
        return false;
    return g_ppc_mode ? cfm_level_call() >= 0 : ppc_trap_depth() == 0;
}

static void refuse(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void refuse(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof why, fmt, ap);
    va_end(ap);
    atomic_store(&state, MACHINE_DRAIN_REFUSED);
    LOGF(LOG_IDLE, "drain: refused: %s\n", why);
}

/* What keeps the machine from a safe point, said as a refusal's reason. */
static void describe(char *out, size_t len)
{
    uint16_t t = trap_current_word();
    int depth = trap_call_depth();
    int native = cfm_level_call();
    const char *in = t ? trap_name(t) : native >= 0 ? cfm_native_name((uint32_t)native) : NULL;
    if (thread_current_index() != 0)
        snprintf(out, len, "one of the program's threads (Thread Manager) runs and did not yield back to its main thread");
    else if (mm_call_depth() > 0 && !g_ppc_mode)
        snprintf(out, len, "the program runs PowerPC code it called through Mixed Mode%s%s and did not come back to its "
                 "event loop", in ? " inside " : "", in ? in : "");
    else if (depth > 0 && g_ppc_mode && native >= 0 && !strcmp(cfm_native_name((uint32_t)native), "CallUniversalProc"))
        /* Disk Copy's PowerPC code calls its dialogs' 68k code: a call from
         * native code, as Mixed Mode makes it. */
        snprintf(out, len, "the program waits in 68k code it called through Mixed Mode (CallUniversalProc)%s%s, which "
                 "a snapshot cannot carry yet", t ? ", inside " : "", t ? trap_name(t) : "");
    else if (depth > 0)
        snprintf(out, len, "the program is in a routine of its own called by the Toolbox (%s) and did not come back to "
                 "its event loop", in ? in : "an interrupt task");
    else if (mm_call_depth() > 0)
        snprintf(out, len, "the program is in a routine of its own called by the Toolbox (%s) and did not come back to "
                 "its event loop", in ? in : "a PowerPC callback");
    else if (ppc_trap_depth() > 0)
        snprintf(out, len, "the program is in 68k code run for its PowerPC call%s%s (a patch of its own, or a trap run "
                 "as 68k code), which did not come back", in ? " to " : "", in ? in : "");
    else if (in)
        snprintf(out, len, "the program is waiting inside %s, which could not be interrupted", in);
    else
        snprintf(out, len, "the program did not come to a point where it could be saved");
}

/* Past the deadline: refused, saying what the machine is doing.  The
 * deadline is the host's time, but a machine slowed by a busy host (a
 * sweep running eight at a time) still gets DRAIN_LEAST instructions, as a
 * screen saver drawing heavily needed to come to its event loop; a
 * machine not running at all (in a host wait) is refused at the longest. */
bool drain_expired(void)
{
    if (atomic_load(&state) != MACHINE_DRAIN_GOING)
        return false;
    uint64_t now = host_ns();
    if (now < deadline)
        return false;
    if (now - begun_ns < DRAIN_LONGEST_NS && g_cpu && g_cpu->icount - begun_at < DRAIN_LEAST)
        return false;
    char where[256];
    describe(where, sizeof where);
    refuse("%s", where);
    return true;
}

/* The idle calls a program's event loop makes, where it stops best. */
static bool idle_call(uint16_t op)
{
    if (op & 0x0800) {
        switch (op & 0x3FF) {
        case 0x060: /* WaitNextEvent */
        case 0x170: /* GetNextEvent */
        case 0x1B4: /* SystemTask */
        case 0x191: /* ModalDialog */
        case 0x185: /* Alert */
        case 0x186: /* StopAlert */
        case 0x187: /* NoteAlert */
        case 0x188: /* CautionAlert */
            return true;
        }
        return false;
    }
    return (op & 0xFF) == 0x3B; /* Delay */
}

bool drain_at_trap(cpu68k *c, uint16_t op)
{
    /* A PowerPC program's 68k code runs only under a host frame (a trap
     * or a callback run for it): never its own level. */
    if (atomic_load(&state) != MACHINE_DRAIN_GOING || g_ppc_mode || !drain_program_level())
        return false;
    if (!idle_call(op))
        return false;
    c->pc = c->op_pc;
    c->icount--; /* not made: made again when the machine goes on */
    c->stopped = true;
    stopped_here = true;
    snprintf(stopped_at, sizeof stopped_at, "%s", trap_name(op));
    return true;
}

bool drain_abandon(void)
{
    /* Only at the program's own level: a routine of its own the Toolbox
     * called would see the call return as if it had been answered. */
    if (atomic_load(&state) != MACHINE_DRAIN_GOING || !abandon_level())
        return false;
    rewind_wanted = true;
    return true;
}

bool drain_at_native(ppc_cpu *c, gaddr at, bool idle, const char *name)
{
    if (!idle || atomic_load(&state) != MACHINE_DRAIN_GOING || !drain_program_level())
        return false;
    c->pc = at;
    c->icount--; /* not made: made again when the machine goes on */
    c->tb--;
    c->stop = true;
    stopped_here = true;
    snprintf(stopped_at, sizeof stopped_at, "%s", name);
    return true;
}

bool drain_take_rewind_ppc(ppc_cpu *c, const char *name)
{
    if (!rewind_wanted)
        return false;
    rewind_wanted = false;
    if (atomic_load(&state) != MACHINE_DRAIN_GOING)
        return false;
    c->stop = true;
    stopped_here = true;
    snprintf(stopped_at, sizeof stopped_at, "%s (abandoned)", name);
    return true;
}

/* For the host, closing an alert or a file panel of its own for a drain:
 * only when the call would be abandoned and made again (drain_abandon),
 * never where the program would take the closing as its answer. */
bool machine_drain_abandonable(void) { return atomic_load(&state) == MACHINE_DRAIN_GOING && abandon_level(); }

bool drain_take_rewind(cpu68k *c, uint16_t op)
{
    if (!rewind_wanted)
        return false;
    rewind_wanted = false;
    if (atomic_load(&state) != MACHINE_DRAIN_GOING)
        return false;
    c->stopped = true;
    stopped_here = true;
    snprintf(stopped_at, sizeof stopped_at, "%s (abandoned)", trap_name(op));
    return true;
}

/* What a snapshot cannot carry: the reason, or NULL. */
const char *machine_hibernate_blocker(void)
{
    /* A 68000's address error unwinds to a host frame of cpu_step's: none
     * is under way at a safe point (each step puts back the one outside). */
    if (g_cpu && g_cpu->fault)
        return "the 68k processor is inside an instruction";
    const char *th = threads_blocker();
    if (th)
        return th;
    const char *io = hibernate_io_blocker();
    if (io)
        return io;
    return NULL;
}

/* Every Toolbox wait (machine_advance_ticks): a program waiting inside one
 * never comes back to machine_run's slices, so the instruction count that
 * starts a drain and the deadline that refuses one are looked at here too. */
void drain_poll(void)
{
    if (hibernate_at && g_cpu && g_cpu->icount >= hibernate_at) {
        hibernate_at = 0;
        machine_drain_begin(true);
    }
    if (atomic_load(&state) == MACHINE_DRAIN_GOING && !drain_program_level())
        describe(last_where, sizeof last_where);
    drain_expired();
}

/* machine_run, between slices: whether the machine stops here, and if so
 * the drain has parked (or been refused, when what it holds cannot be
 * saved). */
bool drain_slice_point(void)
{
    if (hibernate_at && g_cpu && g_cpu->icount >= hibernate_at) {
        hibernate_at = 0;
        machine_drain_begin(true);
    }
    if (atomic_load(&state) != MACHINE_DRAIN_GOING)
        return false;
    cpu68k *c = g_cpu;
    if (c->halted || (g_ppc_mode && g_ppc.halted)) {
        /* Stopped where it was (the instruction limit, a crash): whatever
         * it was inside unwound without finishing, so no safe point.  The
         * limit, reached as the program waited where no safe point comes
         * (a headless run waits faster than the deadline), is refused for
         * where it waited. */
        const char *h = c->halted ? c->halt_reason : g_ppc.halt_reason;
        if (last_where[0] && !strncmp(h, "instruction limit", 17))
            refuse("%s", last_where);
        else
            refuse("the machine stopped (%s) before it came to a point where it could be saved", h);
        return true;
    }
    bool here = stopped_here;
    if (here) {
        c->stopped = false;
        g_ppc.stop = false;
        stopped_here = false;
    } else if (drain_expired()) {
        return true;
    } else if (c->icount < grace_until || !drain_program_level()) {
        return false;
    } else {
        snprintf(stopped_at, sizeof stopped_at, "between instructions (pc %08X)", machine_pc());
    }
    const char *b = machine_hibernate_blocker();
    if (b) {
        snprintf(why, sizeof why, "%s", b);
        atomic_store(&state, MACHINE_DRAIN_REFUSED);
        LOGF(LOG_IDLE, "drain: at %s, refused: %s\n", stopped_at, why);
    } else {
        atomic_store(&state, MACHINE_DRAIN_PARKED);
        LOGF(LOG_IDLE, "drain: parked at %s, instruction %llu, %d of the program's threads waiting\n", stopped_at,
             (unsigned long long)c->icount, threads_parked());
    }
    return true;
}

bool drain_exits(void) { return exits; }
