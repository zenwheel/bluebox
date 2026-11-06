/* Draining the machine to a safe point: where its whole state is in the
 * saved sections, the tracked blocks and the guest's memory, and none of
 * it in the host's stack, so a snapshot of it can be resumed in another
 * process (hibernate.c), and Suspend stops there too when it can.
 *
 * Asked for (machine_drain_begin), the machine's thread runs on to:
 *   - an idle call at the program's own level (WaitNextEvent,
 *     GetNextEvent, SystemTask, ModalDialog, an alert, Delay): the CPU
 *     stops at the trap, the program counter back on it, the call not made,
 *     so a resumed program makes it again;
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
 * were at its trap and the program counter on the trap (trap.c), to be
 * made again after resuming.
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
#define DRAIN_TIMEOUT_NS 2000000000ull         /* then refused */

BB_HOST static atomic_int state;               /* MACHINE_DRAIN_* */
BB_HOST static bool exits;                     /* machine_run returns RUN_DRAINED at the safe point */
BB_HOST static uint64_t deadline;              /* the host's monotonic clock */
BB_HOST static uint64_t grace_until;           /* the instruction count */
BB_HOST static bool stopped_here;              /* the CPU stopped at a trap for the drain */
BB_HOST static bool rewind_wanted;             /* a native abandoned its call */
BB_HOST static char why[256];
BB_HOST static uint64_t hibernate_at;          /* --hibernate-at: the instruction count, or 0 */
BB_HOST static char stopped_at[64];            /* where it stopped (for the log) */

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
    deadline = host_ns() + DRAIN_TIMEOUT_NS;
    /* A snapshot taken where the program waits for its next event is the
     * likeliest to go on well; Suspend wants the machine still now. */
    grace_until = exit_run && g_cpu ? g_cpu->icount + DRAIN_GRACE : 0;
    stopped_here = rewind_wanted = false;
    why[0] = stopped_at[0] = 0;
    atomic_store(&state, MACHINE_DRAIN_GOING);
}

void machine_drain_end(void)
{
    atomic_store(&state, MACHINE_DRAIN_NONE);
    exits = stopped_here = rewind_wanted = false;
}

int machine_drain_state(void) { return atomic_load(&state); }
const char *machine_drain_reason(void) { return why; }
const char *machine_drain_point(void) { return stopped_at; }
bool machine_hibernatable(void) { return atomic_load(&state) == MACHINE_DRAIN_PARKED; }

void machine_hibernate_at(uint64_t icount) { hibernate_at = icount; }

bool drain_wanted(void) { return atomic_load(&state) == MACHINE_DRAIN_GOING; }

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

/* Past the deadline: refused, saying what the machine is doing. */
bool drain_expired(void)
{
    if (atomic_load(&state) != MACHINE_DRAIN_GOING || host_ns() < deadline)
        return false;
    uint16_t t = trap_current_word();
    int depth = trap_call_depth();
    if (depth > 0)
        refuse("the program is in a routine of its own called by the Toolbox (%s) and did not come back to its "
               "event loop", t ? trap_name(t) : "an interrupt task");
    else if (t)
        refuse("the program is waiting inside %s, which could not be interrupted", trap_name(t));
    else
        refuse("the program did not come to a point where it could be saved");
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
    if (atomic_load(&state) != MACHINE_DRAIN_GOING || trap_call_depth() != 0 || threads_live())
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
    if (atomic_load(&state) != MACHINE_DRAIN_GOING || trap_call_depth() != 0)
        return false;
    rewind_wanted = true;
    return true;
}

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
    BB_HOST static char reason[200];
    int n = threads_live();
    if (n > 0) {
        snprintf(reason, sizeof reason, "the program runs %d thread%s of its own (Thread Manager)", n,
                 n == 1 ? "" : "s");
        return reason;
    }
    const char *io = hibernate_io_blocker();
    if (io)
        return io;
    return NULL;
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
    bool here = stopped_here;
    if (here) {
        c->stopped = false;
        stopped_here = false;
    } else if (drain_expired()) {
        return true;
    } else if (c->icount < grace_until || trap_call_depth() != 0) {
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
        LOGF(LOG_IDLE, "drain: parked at %s, instruction %llu\n", stopped_at,
             (unsigned long long)c->icount);
    }
    return true;
}

bool drain_exits(void) { return exits; }
