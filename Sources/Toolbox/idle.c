/*
 * A program waiting by spinning, and the host thread asleep meanwhile.
 *
 * A classic Mac program often waits by asking over and over: a paused game
 * calls WaitNextEvent with no sleep (Apeiron), or polls Button, GetKeys,
 * TickCount or EventAvail, or reads Ticks or KeyMap in low memory in a loop
 * of its own.  On a Mac that cost nothing; here it keeps a host core busy.
 * When the program's time is on the wall clock (a host attached, or
 * --realtime), such a wait is found and the emulator thread sleeps through
 * it, until the next tick, a Time Manager task's time, input, or a few
 * milliseconds at most.
 *
 * An iteration is the stretch of the program between two of its polls
 * (an empty event poll, an unpressed button, an unchanged keyboard,
 * TickCount), or, for a loop that makes no calls at all, between two of
 * the emulation's slices.  It is idle when it did nothing the program or
 * the user could see:
 *   - it ran fewer than MAX_RUN instructions (a poll loop) or, with no
 *     calls, stayed within a few hundred bytes of code;
 *   - every Toolbox call it made is a query with no lasting effect
 *     (quiet_names: polls, FrontWindow, GetPort, LoadSeg ...), no drawing,
 *     sound, file or memory work;
 *   - the registers are as they were at the iteration before (a count kept
 *     in a register shows) and, with no calls, no device was touched;
 *   - its stores, watched once the rest has held for a few iterations,
 *     left the places it stored to as the iteration before left them (the
 *     same arguments pushed again; a counter kept in memory shows), its own
 *     frames below the stack pointer apart.
 * Such an iteration repeated is the program doing nothing over and over,
 * and how many times it does it can't show, so the host may sleep between
 * them.  Interrupt-time work (VBL and Time Manager tasks, sound callbacks)
 * is not the program's iteration and goes on as the clock moves.
 * BLUEBOX_IDLE_SLEEP=0 turns it off.
 */
#include <stdlib.h>
#include <string.h>

#include "hw68k.h"
#include "m68k.h"
#include "ppc.h"
#include "tb_internal.h"
#include "trap.h"

enum {
    SAME_BEFORE_WATCH = 3, /* idle iterations in a row before the stores are watched */
    MAX_RUN = 20000,       /* instructions a poll loop's iteration may run */
    MAX_STORES = 64,       /* places an idle iteration may store to */
    STACK_SCRATCH = 0x10000, /* below the stack pointer: the iteration's own frames */
    PC_NEAR = 256,         /* a loop with no calls: how far apart its slices may end */
    NSITES = 8,            /* the polls a loop may make, each from its own place */
    MAX_SLEEP_NS = 4000000, /* the longest sleep: async completions, sound, network polls */
    LOG_AFTER_NS = 100000000, /* --log idle: waits this long */
    BREAK_NS = 50000000,   /* busy this long between two waits: two of them */
};

enum { NONE, POLLS, SLICES };

typedef struct {
    uint32_t r[36];
} regs;

/* The registers at each place a loop polls from, last time there: a loop
 * may poll more than once around (Button, then TickCount). */
static struct {
    bool used;
    regs r;
} sites[NSITES];
static unsigned next_site;

/* Whether the registers are as they were last time at this poll. */
static bool same_at_site(const regs *r)
{
    for (int i = 0; i < NSITES; i++)
        if (sites[i].used && sites[i].r.r[34] == r->r[34] && sites[i].r.r[32] == r->r[32] &&
            sites[i].r.r[35] == r->r[35]) {
            bool same = !memcmp(&sites[i].r, r, sizeof *r);
            sites[i].r = *r;
            return same;
        }
    sites[next_site].used = true;
    sites[next_site].r = *r;
    next_site = (next_site + 1) % NSITES;
    return false;
}

/* A place the program stored to in an iteration, and what it held at the
 * iteration's end. */
typedef struct {
    gaddr a;
    uint32_t len;
    uint8_t v[4];
} store;

static struct {
    int mode;            /* what ends an iteration: NONE, POLLS or SLICES */
    uint64_t at;         /* the program's instructions at the iteration's start */
    bool busy;           /* it did something */
    regs r;              /* the registers then */
    uint32_t pc;         /* SLICES: where the slice ended */
    gaddr sp;            /* the stack pointer then */
    uint64_t hw;         /* device accesses then */
    uint64_t calls;      /* Toolbox calls then */
    unsigned same;       /* idle iterations in a row */
    bool watching;       /* the program's stores are watched */
    bool watched;        /* ... for the whole of this iteration */
    int nstores, nprev;  /* this iteration's stores; the last one's, if it was watched (else -1) */
    bool overflow;
    store stores[MAX_STORES], prev[MAX_STORES];
} st;

static uint64_t calls;          /* the program's Toolbox calls */
static int interrupt_depth;     /* inside interrupt-time work */
static uint64_t int_68k, int_ppc; /* instructions that work ran */
static uint64_t int_68k0, int_ppc0;
static uint64_t sleeps, slept_ns;
static uint64_t run_sleeps, run_ns; /* this wait's */
static int enabled = -1;

uint64_t trap_icount_passed(void);
bool machine_wall_clock(void);
bool display_watch_all_stores(void (*fn)(gaddr a, uint32_t len));
uint64_t time_next_due(void);
bool time_in_interrupt(void);
void ev_idle_sleep(uint64_t ns);
void machine_pass_time(uint64_t ns);

static bool on(void)
{
    if (enabled < 0) {
        const char *e = getenv("BLUEBOX_IDLE_SLEEP");
        enabled = !(e && e[0] == '0');
    }
    return enabled && g_cpu && !g_cpu->halted && machine_wall_clock();
}

static bool in_interrupt(void) { return interrupt_depth > 0 || time_in_interrupt(); }

/* The program's own instructions, the time a wait passed and interrupt-time
 * work's left out. */
static uint64_t executed(void)
{
    if (g_ppc_mode)
        return g_ppc.icount - int_ppc;
    return g_cpu->icount - trap_icount_passed() - int_68k;
}

static void take(regs *r, bool at_call)
{
    memset(r, 0, sizeof *r);
    if (g_ppc_mode) {
        memcpy(r->r, g_ppc.gpr, sizeof g_ppc.gpr);
        r->r[32] = g_ppc.lr;
        r->r[33] = g_ppc.ctr;
        r->r[34] = at_call ? g_ppc.pc : 0;
    } else {
        memcpy(r->r, g_cpu->d, sizeof g_cpu->d);
        memcpy(r->r + 8, g_cpu->a, sizeof g_cpu->a);
        r->r[34] = at_call ? g_cpu->pc : 0;
    }
    r->r[35] = g_ppc_mode;
}

static uint32_t pc_now(void) { return g_ppc_mode ? g_ppc.pc : g_cpu->pc; }
static gaddr sp_now(void) { return g_ppc_mode ? g_ppc.gpr[1] : g_cpu->a[7]; }

static void store_seen(gaddr a, uint32_t len)
{
    if (!st.watching || in_interrupt())
        return;
    if (a < st.sp && st.sp - a <= STACK_SCRATCH)
        return;
    if (len > 4)
        len = 4;
    for (int i = 0; i < st.nstores; i++)
        if (st.stores[i].a == a) {
            if (len > st.stores[i].len)
                st.stores[i].len = len;
            return;
        }
    if (st.nstores == MAX_STORES) {
        st.overflow = true;
        return;
    }
    st.stores[st.nstores].a = a;
    st.stores[st.nstores++].len = len;
}

/* Whether this iteration's stores left memory as the last one's did (a
 * loop pushing the same arguments again), or changed it (a count kept in
 * memory), or there is no telling yet. */
enum { CHANGED, UNKNOWN, SAME };

static int stores_verdict(void)
{
    if (!st.watched)
        return UNKNOWN;
    if (st.overflow)
        return CHANGED;
    int verdict = st.nprev < 0 ? UNKNOWN : SAME;
    for (int i = 0; i < st.nstores; i++) {
        store *s = &st.stores[i];
        for (uint32_t k = 0; k < s->len; k++)
            s->v[k] = rd8(s->a + k);
        if (verdict != SAME)
            continue;
        int j = 0;
        while (j < st.nprev && (st.prev[j].a != s->a || st.prev[j].len != s->len))
            j++;
        if (j == st.nprev || memcmp(st.prev[j].v, s->v, s->len))
            verdict = st.nprev < 0 ? UNKNOWN : CHANGED;
    }
    if (st.nprev >= 0 && verdict == UNKNOWN)
        verdict = CHANGED;
    return verdict;
}

static void watch(bool w)
{
    if (st.watching == w)
        return;
    st.watching = display_watch_all_stores(w ? store_seen : NULL) && w;
}

static void wait_over(void)
{
    if (run_ns >= LOG_AFTER_NS)
        LOGF(LOG_IDLE, "idle: the program goes on at %08X after %llu sleeps, %.3f s\n", pc_now(),
             (unsigned long long)run_sleeps, (double)run_ns / 1e9);
    run_sleeps = run_ns = 0;
}

/* The program busy again: the wait is over unless it waits again soon
 * (a loop starting again at each tick). */
static uint64_t broke_at;

static void wait_broken(void)
{
    if (run_sleeps && !broke_at)
        broke_at = machine_time_ns() | 1;
}

static void check_over(void)
{
    if (broke_at && machine_time_ns() - broke_at > BREAK_NS) {
        broke_at = 0;
        wait_over();
    }
}

/* An iteration ends: whether it was idle (and enough of them in a row,
 * watched, to sleep); the next one starts. */
static bool iteration_end(int mode)
{
    regs r;
    take(&r, mode == POLLS);
    uint32_t pc = pc_now();
    bool same = mode == POLLS ? same_at_site(&r) : !memcmp(&r, &st.r, sizeof r);
    bool idle = st.mode == mode && !st.busy && same;
    if (idle && mode == POLLS)
        idle = executed() - st.at < MAX_RUN;
    if (idle && mode == SLICES)
        idle = (pc > st.pc ? pc - st.pc : st.pc - pc) < PC_NEAR && hw_device_accesses() == st.hw;
    int stores = stores_verdict();
    if (stores == CHANGED)
        idle = false;
    st.same = idle ? st.same + 1 : 0;
    bool sleep = idle && stores == SAME;
    if (!idle)
        wait_broken();
    watch(st.same >= SAME_BEFORE_WATCH);
    if (idle && st.watched) {
        memcpy(st.prev, st.stores, (size_t)st.nstores * sizeof *st.stores);
        st.nprev = st.nstores;
    } else {
        st.nprev = -1;
    }
    st.mode = mode;
    st.r = r;
    st.pc = pc;
    st.sp = sp_now();
    st.hw = hw_device_accesses();
    st.calls = calls;
    st.busy = false;
    st.nstores = 0;
    st.overflow = false;
    st.watched = st.watching;
    st.at = executed();
    return sleep;
}

static void chain_break(void)
{
    wait_broken();
    st.mode = NONE;
    st.same = 0;
    watch(false);
    st.watched = false;
    st.nprev = -1;
}

/* Sleep until the next tick, a Time Manager task's time or input, a few
 * milliseconds at most. */
static void idle_sleep(void)
{
    uint64_t now = machine_time_ns();
    uint64_t tick = (now * 60 / 1000000000u + 1) * 1000000000u / 60 + 1;
    uint64_t until = tick, due = time_next_due();
    if (due && due < until)
        until = due;
    if (until > now + MAX_SLEEP_NS)
        until = now + MAX_SLEEP_NS;
    if (until <= now)
        return;
    ev_idle_sleep(until - now);
    uint64_t slept = machine_time_ns() - now;
    /* Said once the wait has gone on a tenth of a second: a loop that
     * starts again at every tick (keeping Ticks in a register) is one wait. */
    check_over();
    broke_at = 0;
    bool said = run_ns >= LOG_AFTER_NS;
    run_sleeps++;
    run_ns += slept;
    if (!said && run_ns >= LOG_AFTER_NS)
        LOGF(LOG_IDLE, "idle: the program waits at %08X (%s), the host sleeps\n", pc_now(),
             st.mode == POLLS ? "polling" : "a loop of its own");
    sleeps++;
    slept_ns += slept;
    if (!events_host_attached())
        machine_pass_time(slept);
    machine_advance_ticks();
    st.at = executed();
}

bool idle_poll(void)
{
    if (!on() || in_interrupt())
        return false;
    if (!iteration_end(POLLS))
        return false;
    idle_sleep();
    return true;
}

void idle_work(void)
{
    if (!in_interrupt())
        st.busy = true;
}

void idle_slice(void)
{
    if (!on() || in_interrupt())
        return;
    check_over();
    if (calls != st.calls) {
        /* Calls in the slice: a poll loop's, its polls say; a long way from
         * the last poll, not a poll loop. */
        if (st.mode != POLLS) {
            chain_break();
            st.calls = calls;
        } else if (executed() - st.at >= MAX_RUN) {
            chain_break();
        }
        return;
    }
    if (st.mode == POLLS && executed() - st.at < MAX_RUN)
        return; /* between two polls */
    if (iteration_end(SLICES))
        idle_sleep();
}

/* Interrupt-time work: the program's iteration doesn't include it. */
void idle_interrupt_enter(void)
{
    if (interrupt_depth++ == 0) {
        int_68k0 = g_cpu ? g_cpu->icount - trap_icount_passed() : 0;
        int_ppc0 = g_ppc.icount;
    }
}

void idle_interrupt_leave(void)
{
    if (--interrupt_depth == 0) {
        int_68k += (g_cpu ? g_cpu->icount - trap_icount_passed() : 0) - int_68k0;
        int_ppc += g_ppc.icount - int_ppc0;
    }
}

/* Calls with no lasting effect, made again and again by a program waiting:
 * the polls themselves and the queries around them. */
/* (in strcmp order, for bsearch) */
static const char *const quiet_names[] = {
    "Button", "EventAvail", "FlushEvents", "FrontNonFloatingWindow", "FrontWindow", "Gestalt",
    "GestaltDispatch", "GetCCursor", "GetCTSeed", "GetCaretTime", "GetCursor", "GetDateTime", "GetDblTime",
    "GetGDevice", "GetGWorld", "GetHandleSize", "GetKeys", "GetMainDevice", "GetMouse", "GetNextEvent",
    "GetOSEvent", "GetOSTrapAddress", "GetPort", "GetPtrSize", "GetToolTrapAddress", "GetToolboxTrapAddress",
    "GetTrapAddress", "GetZone", "HGetState", "HLock", "HNoPurge", "HSetState", "HUnlock", "InitCursor",
    "LMGetTicks", "LMGetTime", "LoadSeg", "Microseconds", "NGetTrapAddress", "OSEventAvail", "ObscureCursor",
    "ReadDateTime", "SetCCursor", "SetCursor", "SetEventMask", "SetGDevice", "SetGWorld", "SetPort", "SetZone",
    "StillDown", "StripAddress", "SystemTask", "TickCount", "UnloadSeg", "WaitMouseUp", "WaitNextEvent",
};

static int by_name(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

static bool quiet(const char *name)
{
    return name && bsearch(&name, quiet_names, sizeof quiet_names / sizeof *quiet_names, sizeof *quiet_names,
                           by_name) != NULL;
}

/* A 68k trap: 0 not yet looked at, 1 quiet, 2 not. */
static uint8_t trap_kind[2048];

void idle_trap(uint16_t op)
{
    if (in_interrupt())
        return;
    calls++;
    unsigned k = (op & 0x0800) ? 1024 + (op & 0x3FFu) : (op & 0xFFu);
    if (!trap_kind[k])
        trap_kind[k] = quiet(trap_name(op)) ? 1 : 2;
    if (trap_kind[k] == 2)
        st.busy = true;
}

/* A PowerPC import, by its index among the natives. */
void idle_native(uint32_t index, const char *name)
{
    static uint8_t *kind;
    static uint32_t nkind;
    if (in_interrupt())
        return;
    calls++;
    if (index >= nkind) {
        uint32_t n = index + 256;
        uint8_t *k = realloc(kind, n);
        if (!k) {
            st.busy = true;
            return;
        }
        memset(k + nkind, 0, n - nkind);
        kind = k;
        nkind = n;
    }
    if (!kind[index])
        kind[index] = quiet(name) ? 1 : 2;
    if (kind[index] == 2)
        st.busy = true;
}

void idle_reset(void)
{
    watch(false);
    memset(&st, 0, sizeof st);
    st.nprev = -1;
    memset(sites, 0, sizeof sites);
    interrupt_depth = 0;
    int_68k = int_ppc = 0;
}

uint64_t machine_idle_sleeps(uint64_t *ns)
{
    if (ns)
        *ns = slept_ns;
    return sleeps;
}

void machine_set_idle_sleep(bool allowed) { enabled = allowed; }
