/*
 * A program waiting by spinning, and the host thread asleep meanwhile.
 *
 * A classic Mac program often waits by asking over and over: a paused game
 * calls WaitNextEvent with no sleep (Apeiron), or polls Button, GetKeys,
 * TickCount or EventAvail, or reads Ticks or KeyMap in low memory in a loop
 * of its own.  On a Mac that cost nothing; here it keeps a host core busy.
 * When the program's time is on the wall clock (a host attached, or
 * --realtime), such a wait is found and the emulator thread sleeps through
 * it, until the next tick, a Time Manager task's time or input (a few
 * milliseconds at most while sound, I/O or a network must be served).
 *
 * An iteration is the stretch of the program between two of its polls
 * (an empty event poll, an unpressed button, an unchanged keyboard,
 * TickCount), or, for a loop that makes no calls at all, between two of
 * the emulation's slices.  It is idle when it did nothing the program or
 * the user could see:
 *   - it ran fewer than MAX_RUN instructions (a poll loop) or, with no
 *     calls, stayed within a few hundred bytes of code;
 *   - every Toolbox call it made is a query with no lasting effect
 *     (quiet_names, query_names: polls, FrontWindow, GetPort, LoadSeg,
 *     BitTst, FixMul, PtInRect ...: what only reads and answers; Mixed
 *     Mode's ways into code, which is the iteration's like the rest; a
 *     dispatcher's routine by its selector), one whose answer it writes
 *     into the program's memory and says so (answer_names, idle_wrote:
 *     SetRect, GlobalToLocal, FindWindow, EnableItem, BlockMove ..., taken
 *     as the program's own stores below), one making or disposing of blocks
 *     (heap_names: the heaps' shape is compared below), or drawing
 *     (draw_names: CopyBits, PaintRect, DrawString, PlotCIcon, RGBForeColor,
 *     SetEntries ...), no sound, file or memory work;
 *   - the registers are as they were at an iteration before ending at the
 *     same poll, from the same depth of the stack (a count kept in a
 *     register shows; a routine polling for callers at different depths is
 *     compared with itself for each; a loop going round in several states,
 *     a script interpreter's, is in one it was in before, as long as it
 *     draws nothing; a register holding Ticks as read then holds Ticks as
 *     read now) and, with no calls, no device was touched;
 *   - its stores, watched once the rest has held for a few iterations,
 *     left the places it stored to as the last iteration between the same
 *     two states left them, or as they were before it (the same arguments
 *     pushed again; a counter kept in memory shows), its own frames below
 *     the stack pointer apart, a place seen going between two values
 *     taking either; and if it made or disposed of blocks, the heaps'
 *     shape is as it was (a handle made and disposed of again; a handle
 *     kept each time shows);
 *   - what it drew, watched the same way, left the pixels as that iteration
 *     left them: QuickDraw's pixels are written from here, which the
 *     store watch doesn't see, so its blitters say which rectangle of which
 *     destination they wrote (qd_drawn), and the program's own stores into
 *     the screen or a destination drawn into are taken as pixels too; at the
 *     iteration's end each destination's rectangle is hashed, and so are the
 *     ports drawn with and the colours shown (the screen's table, the video
 *     hardware's and the palettes', by colour only).  A pause screen drawn
 *     again and again is the same; a game playing, text scrolling or a count
 *     drawn are not.
 * Such an iteration repeated is the program doing nothing over and over,
 * and how many times it does it can't show, so the host may sleep between
 * them: once a turn of the loop, at the poll it last slept at.  Interrupt-time work (VBL and Time Manager tasks, sound callbacks)
 * is not the program's iteration and goes on as the clock moves.
 * BLUEBOX_IDLE_SLEEP=0 turns it off.
 */
#include <stdlib.h>
#include <string.h>

#include "hw68k.h"
#include "lowmem_gen.h"
#include "m68k.h"
#include "ppc.h"
#include "qd_internal.h"
#include "structs_gen.h"
#include "tb_internal.h"
#include "trap.h"

enum {
    SAME_BEFORE_WATCH = 3, /* idle iterations in a row before the stores are watched */
    MOST_BEFORE_WATCH = 256, /* ... after watching found work again and again */
    MAX_RUN = 1000000,     /* instructions a poll loop's iteration may run */
    OWN_LOOP_AFTER = 20000, /* ... after which its slices may show a loop of its own, or calls end it */
    QUIET_RUN = 100000,    /* ... or calls that were all queries */
    MAX_STORES = 64,       /* places an idle iteration may store to, in the saved state ... */
    MOST_STORES = 256,     /* ... and beside it (Interiors' wait stores to 96) */
    MAX_RANGES = 8,        /* ... and longer stretches a call wrote (BlockMove) */
    RANGE_FROM = 17,       /* bytes from which a call's answer is a stretch */
    RANGE_MOST = 65536,    /* bytes a stretch may have */
    MAX_DRAWN = 16,        /* destinations it may draw into */
    MAX_PORTS = 8,         /* ports it may draw with */
    MAX_PALETTES = 16,     /* palettes whose colours are compared */
    STACK_SCRATCH = 0x10000, /* below the stack pointer: the iteration's own frames */
    PC_NEAR = 256,         /* a loop with no calls: how far apart its slices may end */
    NSITES = 8,            /* the places a loop polls from, in the saved state ... */
    MOST_SITES = 64,       /* ... and with those beside it, the states a loop may poll in */
    SLICE_SITE = MOST_SITES, /* what a loop of its own's iterations are compared with */
    MAX_SLEEP_NS = 4000000, /* the longest sleep while sound, I/O or a network needs serving */
    LOG_AFTER_NS = 100000000, /* --log idle: waits this long */
    BREAK_NS = 50000000,   /* busy this long between two waits: two of them */
};

enum { NONE, POLLS, SLICES };

/* What a call is (trap_kind's values: new ones go at the end): */
enum { UNSEEN, QUIET, DRAWS, SELECTOR, WORK, HEAP };

typedef struct {
    uint32_t r[36];
} regs;

/* The registers a loop polled with, each state at each place: a loop may
 * poll more than once around (Button, then TickCount), and an interpreter
 * waiting goes round its script polling from the same place in several
 * states (Frankenstein's: some 15 in a turn, at 9 places).  The first
 * NSITES are saved state, the rest beside it; the least recently seen is
 * the one a new state takes. */
typedef struct {
    bool used;
    regs r;
} site_regs;
static site_regs sites[NSITES];
BB_HOST static site_regs more_sites[MOST_SITES - NSITES];
BB_HOST static uint64_t site_seen[MOST_SITES], site_clock;
BB_HOST static uint32_t site_ticks[MOST_SITES]; /* Ticks then */

static site_regs *site_at(int i) { return i < NSITES ? &sites[i] : &more_sites[i - NSITES]; }

static void why_regs(const regs *was, const regs *now);

static bool same_place(const regs *a, const regs *b)
{
    const int sp = a->r[35] ? 1 : 15; /* r1, a7: a routine polling for callers at different depths */
    return a->r[34] == b->r[34] && a->r[32] == b->r[32] && a->r[35] == b->r[35] && a->r[sp] == b->r[sp];
}

/* The same registers, but for the clock: one holding Ticks as it was then
 * holds Ticks as it is now (a script interpreter keeps the time it read
 * in a register as it goes round; what it waits for is elsewhere). */
static bool same_regs(const regs *was, uint32_t was_ticks, const regs *now, uint32_t ticks)
{
    if (!memcmp(was, now, sizeof *now))
        return true;
    if (was_ticks == ticks)
        return false;
    for (int i = 0; i < 32; i++)
        if (was->r[i] != now->r[i] && !(was->r[i] == was_ticks && now->r[i] == ticks))
            return false;
    return !memcmp(was->r + 32, now->r + 32, sizeof was->r - 32 * sizeof was->r[0]);
}

/* Whether the registers are as they were at this poll before: the last
 * time there, or (*earlier) an earlier time, the loop having gone round
 * in other states since.  *site: the state's (-1 - it for a new place). */
static bool same_at_site(const regs *r, int *site, bool *earlier)
{
    const uint32_t ticks = rd32(LM_Ticks);
    int found = -1, last = -1, oldest = 0;
    for (int i = 0; i < MOST_SITES; i++) {
        const site_regs *s = site_at(i);
        if (!s->used) {
            if (site_at(oldest)->used)
                oldest = i;
            continue;
        }
        if (site_at(oldest)->used && site_seen[i] < site_seen[oldest])
            oldest = i;
        if (!same_place(&s->r, r))
            continue;
        if (last < 0 || site_seen[i] > site_seen[last])
            last = i;
        if (found < 0 && same_regs(&s->r, site_ticks[i], r, ticks))
            found = i;
    }
    site_clock++;
    *earlier = found >= 0 && found != last;
    if (found >= 0) {
        site_seen[found] = site_clock;
        site_at(found)->r = *r;
        site_ticks[found] = ticks;
        *site = found;
        return true;
    }
    if (last >= 0)
        why_regs(&site_at(last)->r, r);
    site_at(oldest)->used = true;
    site_at(oldest)->r = *r;
    site_seen[oldest] = site_clock;
    site_ticks[oldest] = ticks;
    *site = last >= 0 ? oldest : -1 - oldest;
    return false;
}

/* A place the program stored to in an iteration, and what it held at the
 * iteration's end. */
typedef struct {
    gaddr a;
    uint32_t len;
    uint8_t v[4];
} store;

/* A destination drawn into in an iteration (the screen, a GWorld, an
 * offscreen bitmap: by base address and rowBytes), the rectangle of it
 * drawn, as rows and the bytes of each, and what that held at the
 * iteration's end. */
typedef struct {
    gaddr base;
    uint32_t row_bytes, rows; /* rows: the destination's, for the program's stores into it */
    uint32_t r0, r1, b0, b1;
    uint64_t hash;
} drawn;

/* What the last iteration to end at a poll (or, the last, at a slice) left
 * in memory and drawn: a loop polling from more than one place (a frame
 * drawn, Button; the clock read, TickCount) is compared with itself round
 * the loop, each stretch with the same stretch last time. */
typedef struct {
    int n; /* -1: none yet in this wait */
    drawn d[MAX_DRAWN];
    uint64_t look;
    int nstores;
    store stores[MAX_STORES];
} shot;
static shot shots[NSITES + 1]; /* the saved places', and the last a loop of its own's */
BB_HOST static shot more_site_shots[MOST_SITES - NSITES];

static shot *shot_at(int site)
{
    return site == SLICE_SITE ? &shots[NSITES] : site < NSITES ? &shots[site] : &more_site_shots[site - NSITES];
}

/* The shots are kept for each stretch between two polls, by the states it
 * went from and to: a loop reaching one state by more than one way round
 * (Frankenstein's script, its stack used by other calls on one way)
 * compares each way with itself. */
BB_HOST static struct {
    int from, to;
    uint64_t seen; /* 0: unused */
} stretches[MOST_SITES];
BB_HOST static uint64_t stretch_clock;
BB_HOST static int last_site = -1; /* where the last iteration ended, -1 none (or a slice) */

static void forget_shot(int slot);

static int stretch_of(int from, int to)
{
    int oldest = 0;
    stretch_clock++;
    for (int i = 0; i < MOST_SITES; i++) {
        if (stretches[i].seen && stretches[i].from == from && stretches[i].to == to) {
            stretches[i].seen = stretch_clock;
            return i;
        }
        if (stretches[i].seen < stretches[oldest].seen)
            oldest = i;
    }
    stretches[oldest].from = from;
    stretches[oldest].to = to;
    stretches[oldest].seen = stretch_clock;
    forget_shot(oldest);
    return oldest;
}

/* Beside them (and not saved, being new): the longer stretches of the
 * program's memory calls wrote (BlockMove, HandToHand) and the heaps'
 * shape, for iterations that made or disposed of blocks; the same for the
 * iteration going on. */
typedef struct {
    gaddr a;
    uint32_t len;
    uint64_t hash;
    bool handle; /* a: a handle, its block whatever it is at the end (a region written) */
} range;
BB_HOST static struct {
    int n; /* -1: none yet in this wait */
    range r[MAX_RANGES];
    uint64_t heap; /* 0: the heaps untouched */
} more_shots[MOST_SITES + 1];
BB_HOST static struct {
    int n;
    bool overflow;
    range r[MAX_RANGES];
    bool heap;   /* blocks made or disposed of */
    uint64_t fp; /* ... and the heaps' shape at its end */
} more;

static void forget_shot(int slot) { shot_at(slot)->n = more_shots[slot].n = -1; }

static void forget_shots(void)
{
    for (int i = 0; i <= MOST_SITES; i++)
        forget_shot(i);
}

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
    int nstores;         /* this iteration's stores */
    bool overflow;
    store stores[MAX_STORES];
    bool drew;                 /* it made drawing calls */
    int ndrawn, nprev_drawn;
    bool drawn_overflow;
    drawn drawn[MAX_DRAWN], prev_drawn[MAX_DRAWN];
    int nports;
    gaddr ports[MAX_PORTS];    /* the ports it drew with */
    uint64_t look;             /* the ports' state and the colours shown, hashed */
} st;

/* The stores past the saved state's room, the iteration's and each
 * shot's. */
BB_HOST static store st_more_stores[MOST_STORES - MAX_STORES];
BB_HOST static store shot_more_stores[MOST_SITES + 1][MOST_STORES - MAX_STORES];

/* What each place held before the iteration's first store there, if the
 * program's own store was the first. */
BB_HOST static uint8_t st_before[MOST_STORES][4];
BB_HOST static bool st_had[MOST_STORES];

static store *st_store(int i) { return i < MAX_STORES ? &st.stores[i] : &st_more_stores[i - MAX_STORES]; }
static store *shot_store(int site, int i)
{
    return i < MAX_STORES ? &shot_at(site)->stores[i] : &shot_more_stores[site][i - MAX_STORES];
}

bool g_qd_drawn_watch;

/* Idle iterations in a row before the stores are watched: more each time
 * watching found the program at work after all (a game drawing each frame
 * with the registers as they were, its state in memory), so that playing
 * isn't slowed by the watch; fewer again once it sleeps. */
static unsigned watch_after = SAME_BEFORE_WATCH;

static uint64_t calls;          /* the program's Toolbox calls */
static int interrupt_depth;     /* inside interrupt-time work */
static uint64_t int_68k, int_ppc; /* instructions that work ran */
static uint64_t int_68k0, int_ppc0;
static uint64_t sleeps, slept_ns;
static uint64_t run_sleeps, run_ns; /* this wait's */
BB_HOST static int enabled = -1; /* the run's environment */

uint64_t trap_icount_passed(void);
bool machine_wall_clock(void);
bool display_watch_all_stores(void (*fn)(gaddr a, uint32_t len));
uint64_t time_next_due(void);
bool time_in_interrupt(void);
void ev_idle_sleep(uint64_t ns);
bool qd_region_recording(void);
void machine_pass_time(uint64_t ns);

static uint32_t pc_now(void);

/* BLUEBOX_IDLE_WHY=1 with --log idle: each second of the program's time
 * with no sleep, why its iterations weren't idle (for finding the loops
 * this doesn't recognise). */
enum { WHY_NAMES = 16 }; /* the calls named */
enum { W_MODE, W_WORK, W_NEWSITE, W_REGS, W_LONG, W_FAR, W_HW, W_STORES, W_PIXELS, W_UNKNOWN, W_BREAK, W_SLEPT, W_N };
static const char *const why_names[W_N] = { "first", "work", "new site", "registers", "long", "far", "device", "stores",
                                             "pixels", "unwatched", "broken", "slept" };
BB_HOST static struct {
    int on; /* -1: not yet asked */
    uint64_t since;
    uint64_t n[W_N];
    const char *name[WHY_NAMES];
    uint64_t count[WHY_NAMES];
    uint64_t reg[36]; /* the registers that differed */
    gaddr store;      /* the last place whose store differed */
    bool overflow;    /* ... or too many places stored to */
} why = { -1 };

static void why_regs(const regs *was, const regs *now)
{
    if (why.on > 0)
        for (int i = 0; i < 36; i++)
            if (was->r[i] != now->r[i])
                why.reg[i]++;
}

static void why_work(const char *name)
{
    if (why.on <= 0)
        return;
    name = name ? name : "?";
    for (int i = 0; i < WHY_NAMES; i++)
        if (!why.name[i] || why.name[i] == name || !strcmp(why.name[i], name)) {
            why.name[i] = name;
            why.count[i]++;
            return;
        }
}

static void why_count(int w)
{
    if (why.on < 0) {
        const char *e = getenv("BLUEBOX_IDLE_WHY");
        why.on = e && e[0] == '1';
    }
    if (!why.on)
        return;
    why.n[w]++;
    uint64_t now = machine_time_ns();
    if (now - why.since < 1000000000u)
        return;
    if (!why.n[W_SLEPT]) {
        char buf[1024];
        int k = snprintf(buf, sizeof buf, "idle: why at %08X (%s):", pc_now(), g_ppc_mode ? "ppc" : "68k");
        for (int i = 0; i < W_N && k < (int)sizeof buf; i++)
            if (why.n[i])
                k += snprintf(buf + k, sizeof buf - (size_t)k, " %s %llu", why_names[i], (unsigned long long)why.n[i]);
        for (int i = 0; i < WHY_NAMES && why.name[i] && k < (int)sizeof buf; i++)
            k += snprintf(buf + k, sizeof buf - (size_t)k, "%s%s %llu", i ? ", " : " (", why.name[i],
                          (unsigned long long)why.count[i]);
        if (why.name[0] && k < (int)sizeof buf)
            k += snprintf(buf + k, sizeof buf - (size_t)k, ")");
        if (why.store && k < (int)sizeof buf)
            k += snprintf(buf + k, sizeof buf - (size_t)k, " store at %08X", why.store);
        if (why.overflow && k < (int)sizeof buf)
            k += snprintf(buf + k, sizeof buf - (size_t)k, " stores to too many places");
        for (int i = 0; i < 36 && k < (int)sizeof buf; i++)
            if (why.reg[i] > why.n[W_REGS] / 2)
                k += snprintf(buf + k, sizeof buf - (size_t)k, " %s%d",
                              g_ppc_mode ? (i < 32 ? "r" : "spr") : (i < 8 ? "d" : "a"), g_ppc_mode ? i : i % 8);
        LOGF(LOG_IDLE, "%s\n", buf);
    }
    memset(why.n, 0, sizeof why.n);
    memset(why.name, 0, sizeof why.name);
    memset(why.count, 0, sizeof why.count);
    memset(why.reg, 0, sizeof why.reg);
    why.store = 0;
    why.overflow = false;
    why.since = now;
}

static bool on(void)
{
    if (enabled < 0) {
        const char *e = getenv("BLUEBOX_IDLE_SLEEP");
        enabled = !(e && e[0] == '0');
    }
    return enabled && g_cpu && !g_cpu->halted && machine_wall_clock();
}

static bool in_interrupt(void) { return interrupt_depth > 0 || time_in_interrupt(); }
bool idle_in_interrupt(void) { return in_interrupt(); }

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

static bool drawn_store(gaddr a, uint32_t len);
static void store_run(gaddr a, uint32_t len);

/* A place stored to: by the program (before the store, so what the place
 * held can be read) or by a call answering (after). */
static void store_place(gaddr a, uint32_t len, bool before)
{
    if (!st.watching || in_interrupt())
        return;
    if (drawn_store(a, len))
        return;
    if (a < st.sp && st.sp - a <= STACK_SCRATCH)
        return;
    if (st.overflow)
        return;
    if (len > 4)
        len = 4;
    BB_HOST static int last;
    for (int i = last < st.nstores && st_store(last)->a == a ? last : 0; i < st.nstores; i++) {
        store *s = st_store(i);
        if (s->a == a) {
            if (len > s->len)
                s->len = len;
            last = i + 1;
            return;
        }
    }
    if (st.nstores == MOST_STORES) {
        store_run(a, len);
        return;
    }
    st_had[st.nstores] = before;
    for (uint32_t k = 0; before && k < 4; k++)
        st_before[st.nstores][k] = rd8(a + k);
    st_store(st.nstores)->a = a;
    st_store(st.nstores++)->len = len;
    last = st.nstores;
}

static void store_seen(gaddr a, uint32_t len) { store_place(a, len, true); }

/* Stores past the places' room: taken as stretches of memory, each store
 * next to one already taken growing it (a block cleared or filled a long
 * at a time: Frankenstein's script clears 1 KB at a turn). */
static void store_run(gaddr a, uint32_t len)
{
    for (int i = 0; i < more.n; i++) {
        range *r = &more.r[i];
        if (r->handle || a + len < r->a || a > r->a + r->len)
            continue;
        gaddr lo = a < r->a ? a : r->a, hi = a + len > r->a + r->len ? a + len : r->a + r->len;
        if (hi - lo > RANGE_MOST)
            break;
        r->a = lo;
        r->len = hi - lo;
        return;
    }
    if (more.n == MAX_RANGES)
        st.overflow = true;
    else
        more.r[more.n++] = (range){ a, len, 0, false };
}

/* A quiet call's answer written into the program's memory (a point
 * converted in place, a rectangle set, intersected or offset): taken as a
 * store of the program's, so that the same answer again is the same, and
 * another is work. */
void idle_wrote(gaddr a, uint32_t len)
{
    if (!a)
        return; /* no place given for the answer */
    if (len < RANGE_FROM) {
        for (uint32_t k = 0; k < len; k += 4)
            store_place(a + k, len - k < 4 ? len - k : 4, false);
        return;
    }
    if (!st.watching || in_interrupt())
        return;
    for (int i = 0; i < more.n; i++)
        if (!more.r[i].handle && more.r[i].a == a && more.r[i].len == len)
            return;
    if (more.n == MAX_RANGES || len > RANGE_MOST)
        more.overflow = true;
    else
        more.r[more.n++] = (range){ a, len, 0, false };
}

/* The kind of the program's call going on (call_kind). */
BB_HOST static uint8_t call_now;

/* A quiet call's answer written into a handle (CopyRgn, UnionRgn,
 * GetPortVisibleRegion: a region): its block is compared at the
 * iteration's end, wherever it then is, and the heaps' shape with it (the
 * handle may have grown).  Only for the program's own call: the same
 * routines inside a drawing call work on regions of their own. */
void idle_wrote_handle(gaddr h)
{
    if (!h || !st.watching || in_interrupt() || call_now != QUIET)
        return;
    more.heap = true;
    for (int i = 0; i < more.n; i++)
        if (more.r[i].handle && more.r[i].a == h)
            return;
    if (more.n == MAX_RANGES)
        more.overflow = true;
    else
        more.r[more.n++] = (range){ h, 0, 0, true };
}

/* Whether this iteration's stores left memory as the last one's did (a
 * loop pushing the same arguments again), or changed it (a count kept in
 * memory), or there is no telling yet. */
enum { CHANGED, UNKNOWN, SAME };

/* Places whose stores have been seen to go back and forth between two
 * values, as the loop goes round (a frame's saved register, taken from
 * what another way round left on the stack): either is nothing new, as
 * long as nothing is drawn.  Kept by address, across waits. */
enum { TWO_WAYS = 256 };
BB_HOST static struct {
    gaddr a;
    uint32_t len;
    uint8_t v[2][4];
} two_ways[TWO_WAYS];

static unsigned two_ways_at(gaddr a) { return (a * 2654435761u) >> 24; /* < TWO_WAYS */ }

/* Whether the place is known to hold either value; if not, it is now. */
static bool either_way(gaddr a, uint32_t len, const uint8_t *was, const uint8_t *now)
{
    unsigned k = two_ways_at(a);
    if (two_ways[k].a == a && two_ways[k].len == len &&
        ((!memcmp(two_ways[k].v[0], was, len) && !memcmp(two_ways[k].v[1], now, len)) ||
         (!memcmp(two_ways[k].v[1], was, len) && !memcmp(two_ways[k].v[0], now, len))))
        return true;
    two_ways[k].a = a;
    two_ways[k].len = len;
    memcpy(two_ways[k].v[0], was, len);
    memcpy(two_ways[k].v[1], now, len);
    return false;
}

static int stores_verdict(int site)
{
    if (!st.watched)
        return UNKNOWN;
    if (st.overflow) {
        why.overflow = true;
        return CHANGED;
    }
    const int n = shot_at(site)->n < 0 ? -1 : shot_at(site)->nstores;
    int verdict = n < 0 ? UNKNOWN : SAME;
    for (int i = 0; i < st.nstores; i++) {
        store *s = st_store(i);
        for (uint32_t k = 0; k < s->len; k++)
            s->v[k] = rd8(s->a + k);
        /* Left as it was before the iteration: nothing new, whatever the
         * last one did there (a way round storing there only sometimes,
         * the same value again). */
        if (verdict != SAME || (st_had[i] && !memcmp(st_before[i], s->v, s->len)))
            continue;
        /* In the same order as last time, as a rule. */
        int j = i < n && shot_store(site, i)->a == s->a ? i : 0;
        while (j < n && (shot_store(site, j)->a != s->a || shot_store(site, j)->len != s->len))
            j++;
        if (j == n && !st_had[i]) {
            verdict = CHANGED; /* a new place, what it held unknown */
            why.store = s->a;
            continue;
        }
        const uint8_t *was = j < n ? shot_store(site, j)->v : st_before[i]; /* not stored last time: as it was */
        if (memcmp(was, s->v, s->len) && (!either_way(s->a, s->len, was, s->v) || st.drew || st.ndrawn)) {
            verdict = CHANGED;
            why.store = s->a;
        }
    }
    return verdict;
}

/* ---- What it drew ---- */

static uint64_t hash_bytes(uint64_t h, const uint8_t *p, size_t n)
{
    const uint64_t k = 0x9E3779B97F4A7C15ull;
    for (; n >= 8; p += 8, n -= 8) {
        uint64_t w;
        memcpy(&w, p, 8);
        h = (h ^ w) * k;
        h ^= h >> 32;
    }
    while (n--)
        h = (h ^ *p++) * k;
    return h ^ (h >> 29);
}

static uint64_t hash_guest(uint64_t h, gaddr a, uint32_t n)
{
    if ((uint64_t)a + n > 0x100000000ull)
        return h ^ 1;
    return hash_bytes(h, gptr(a), n);
}

/* The rectangle drawn into a destination, grown to take in rows [r0, r1)
 * and bytes [b0, b1). */
static void draw_into(gaddr base, uint32_t row_bytes, uint32_t rows, uint32_t r0, uint32_t r1, uint32_t b0,
                      uint32_t b1)
{
    static int last;
    if (r1 <= r0 || b1 <= b0)
        return;
    drawn *d = last < st.ndrawn && st.drawn[last].base == base && st.drawn[last].row_bytes == row_bytes
                   ? &st.drawn[last]
                   : NULL;
    for (int i = 0; !d && i < st.ndrawn; i++)
        if (st.drawn[i].base == base && st.drawn[i].row_bytes == row_bytes)
            d = &st.drawn[last = i];
    if (!d) {
        if (st.ndrawn == MAX_DRAWN) {
            st.drawn_overflow = true;
            return;
        }
        d = &st.drawn[last = st.ndrawn++];
        *d = (drawn){ base, row_bytes, rows, r0, r1, b0, b1, 0 };
        return;
    }
    if (rows > d->rows)
        d->rows = rows;
    if (r0 < d->r0)
        d->r0 = r0;
    if (r1 > d->r1)
        d->r1 = r1;
    if (b0 < d->b0)
        d->b0 = b0;
    if (b1 > d->b1)
        d->b1 = b1;
}

void qd_drawn(const pixdst *d, int c0, int r0, int c1, int r1)
{
    if (!st.watching || in_interrupt() || !d->row_bytes)
        return;
    int rows = d->bounds.bottom - d->bounds.top;
    if (c0 < 0)
        c0 = 0;
    if (r0 < 0)
        r0 = 0;
    if (r1 > rows)
        r1 = rows;
    if (c1 <= c0 || r1 <= r0)
        return;
    uint32_t b0 = (uint32_t)c0 * (uint32_t)d->depth / 8, b1 = ((uint32_t)c1 * (uint32_t)d->depth + 7) / 8;
    if (b1 > d->row_bytes)
        b1 = d->row_bytes;
    draw_into(d->base, d->row_bytes, (uint32_t)rows, (uint32_t)r0, (uint32_t)r1, b0, b1);
}

bool qd_drawn_area(const pixdst *d, qrect local)
{
    bool was = g_qd_drawn_watch;
    if (was)
        qd_drawn(d, local.left - d->bounds.left, local.top - d->bounds.top, local.right - d->bounds.left,
                 local.bottom - d->bounds.top);
    g_qd_drawn_watch = false;
    return was;
}

/* A store of the program's into the screen, or into a destination drawn
 * into: pixels, taken as QuickDraw's are (a game drawing on the screen
 * itself; a pause screen so drawn again is still the same). */
static bool store_into(const drawn *d, gaddr a, uint32_t len)
{
    uint32_t off = a - d->base;
    if (off >= d->row_bytes * d->rows)
        return false;
    uint32_t row = off / d->row_bytes, b = off % d->row_bytes, e = b + len;
    if (e > d->row_bytes) /* across the end of a row: all of both */
        draw_into(d->base, d->row_bytes, d->rows, row, row + 2, 0, d->row_bytes);
    else
        draw_into(d->base, d->row_bytes, d->rows, row, row + 1, b, e);
    return true;
}

static bool drawn_store(gaddr a, uint32_t len)
{
    drawn screen = { rd32(LM_ScrnBase), qd_screen_row_bytes(), (uint32_t)g_screen_height, 0, 0, 0, 0, 0 };
    if (screen.row_bytes && store_into(&screen, a, len))
        return true;
    for (int i = 0; i < st.ndrawn; i++)
        if (store_into(&st.drawn[i], a, len))
            return true;
    for (int i = 0; i < st.nprev_drawn; i++)
        if (store_into(&st.prev_drawn[i], a, len))
            return true;
    return false;
}

static uint64_t hash_region(uint64_t h, gaddr rgn)
{
    if (!rgn || !rd32(rgn))
        return h ^ 2;
    return hash_guest(h, rd32(rgn), rd16(rd32(rgn)) & 0x7FFF);
}

/* What the drawing calls left besides pixels: the ports' state (the pen,
 * colours, clipping) and the colours the screen shows, by colour only (a
 * palette or table set again to the same colours is the same, though its
 * seed moves on). */
static uint64_t look_hash(void)
{
    uint64_t h = 0x84222325CBF29CE4ull;
    for (int i = 0; i < st.nports; i++) {
        gaddr p = st.ports[i];
        h = hash_guest(h, p, CGrafPort_SIZEOF);
        h = hash_region(h, rd32(p + GrafPort_clipRgn_off));
        h = hash_region(h, rd32(p + GrafPort_visRgn_off));
        if (qd_port_is_color(p) && rd32(p + CGrafPort_portPixMap_off) && rd32(rd32(p + CGrafPort_portPixMap_off)))
            h = hash_guest(h, rd32(rd32(p + CGrafPort_portPixMap_off)), PixMap_SIZEOF);
    }
    gaddr ct = qd_screen_ctab();
    if (ct && rd32(ct)) {
        uint32_t n = (uint32_t)(int16_t)rd16(rd32(ct) + 6) + 1;
        if (n <= 256)
            h = hash_guest(h, rd32(ct) + 8, n * 8);
    }
    uint16_t hw[256][3];
    for (int i = 0; i < 256; i++)
        qd_screen_get_hw_entry(i, hw[i]);
    h = hash_bytes(h, (const uint8_t *)hw, sizeof hw);
    const uint8_t (*out)[256] = video_output_lut(); /* NULL: as they are */
    if (out)
        h = hash_bytes(h, (const uint8_t *)out, 3 * 256);
    gaddr pals[MAX_PALETTES];
    int npals = qd_palettes_in_use(pals, MAX_PALETTES);
    for (int i = 0; i < npals; i++) {
        if (!rd32(pals[i]))
            continue;
        int n = (int16_t)rd16(rd32(pals[i]) + Palette_pmEntries_off);
        for (int k = 0; k < n && k < 4096; k++) /* the colour and usage, not the slot kept */
            h = hash_guest(h, rd32(pals[i]) + Palette_pmInfo_off + (uint32_t)k * ColorInfo_SIZEOF, 8);
    }
    return h;
}

/* Whether this iteration's drawing left the pixels it drew, the ports and
 * the colours as the last one's did. */
static int drawn_verdict(int site)
{
    if (!st.watched)
        return UNKNOWN;
    if (st.drawn_overflow)
        return CHANGED;
    for (int i = 0; i < st.ndrawn; i++) {
        drawn *d = &st.drawn[i];
        uint64_t h = d->r0 * 0x100000001ull ^ d->b0 ^ (uint64_t)d->b1 << 20;
        for (uint32_t r = d->r0; r < d->r1; r++)
            h = hash_guest(h, d->base + r * d->row_bytes + d->b0, d->b1 - d->b0);
        d->hash = h;
    }
    st.look = st.drew ? look_hash() : 0;
    const int n = shot_at(site)->n;
    if (n < 0)
        return UNKNOWN;
    if (st.ndrawn != n || st.look != shot_at(site)->look)
        return CHANGED;
    for (int i = 0; i < st.ndrawn; i++) {
        const drawn *d = &st.drawn[i];
        int j = 0;
        while (j < n && (shot_at(site)->d[j].base != d->base || shot_at(site)->d[j].row_bytes != d->row_bytes))
            j++;
        if (j == n)
            return CHANGED;
        const drawn *e = &shot_at(site)->d[j];
        if (e->r0 != d->r0 || e->r1 != d->r1 || e->b0 != d->b0 || e->b1 != d->b1 || e->hash != d->hash)
            return CHANGED;
    }
    return SAME;
}

static uint64_t hash_range(const range *r)
{
    const uint64_t h = 0x9E3779B97F4A7C15ull;
    if (!r->handle)
        return hash_guest(h, r->a, r->len);
    if (!mm_handle_valid(r->a) || !rd32(r->a))
        return h ^ 3; /* disposed of, or empty */
    int32_t n = tb_GetHandleSize(r->a);
    return n < 0 || n > RANGE_MOST ? h ^ 5 : hash_guest(h ^ (uint64_t)n, rd32(r->a), (uint32_t)n);
}

/* Whether the stretches calls wrote, and the heaps' shape if the
 * iteration made or disposed of blocks, are as the last one left them. */
static int more_verdict(int site)
{
    if (!st.watched)
        return UNKNOWN;
    if (more.overflow)
        return CHANGED;
    more.fp = more.heap ? mm_heap_fingerprint() : 0;
    if (more.heap && !more.fp)
        return CHANGED;
    for (int i = 0; i < more.n; i++)
        more.r[i].hash = hash_range(&more.r[i]);
    const int n = more_shots[site].n;
    if (n < 0)
        return UNKNOWN;
    if (n != more.n || more_shots[site].heap != more.fp)
        return CHANGED;
    for (int i = 0; i < more.n; i++) {
        int j = 0;
        while (j < n && (more_shots[site].r[j].a != more.r[i].a || more_shots[site].r[j].len != more.r[i].len ||
                         more_shots[site].r[j].handle != more.r[i].handle))
            j++;
        if (j == n || more_shots[site].r[j].hash != more.r[i].hash)
            return CHANGED;
    }
    return SAME;
}

static void watch(bool w)
{
    if (st.watching != w)
        st.watching = display_watch_all_stores(w ? store_seen : NULL) && w;
    g_qd_drawn_watch = st.watching;
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

/* When the iteration began, on the machine's clock, and how long the
 * last one took the host. */
BB_HOST static uint64_t iteration_began, iteration_took;

/* The poll the host last slept at. */
BB_HOST static struct {
    bool set;
    regs r;
} anchor;

/* An iteration ends: whether it was idle (and enough of them in a row,
 * watched, to sleep); the next one starts. */
static bool iteration_end(int mode)
{
    regs r;
    take(&r, mode == POLLS);
    uint32_t pc = pc_now();
    int site = SLICE_SITE;
    bool earlier = false;
    bool same = mode == POLLS ? same_at_site(&r, &site, &earlier) : !memcmp(&r, &st.r, sizeof r);
    bool new_site = site < 0;
    if (new_site)
        site = -1 - site;
    /* Back in a state the loop was in before its last time here: going
     * round in several, an interpreter's script.  Drawing so is a game's
     * frames in turn, its phase in a register: work. */
    if (earlier && (st.drew || st.ndrawn))
        same = false;
    /* What this stretch is compared with: the last the same way round. */
    int slot = mode == POLLS ? stretch_of(last_site, site) : SLICE_SITE;
    last_site = mode == POLLS ? site : -1;
    bool idle = st.mode == mode && !st.busy && same;
    int w = st.mode != mode ? W_MODE : st.busy ? W_WORK : new_site ? W_NEWSITE : !same ? W_REGS : W_SLEPT;
    if (idle && mode == POLLS && !(idle = executed() - st.at < MAX_RUN))
        w = W_LONG;
    if (idle && mode == SLICES && !(idle = (pc > st.pc ? pc - st.pc : st.pc - pc) < PC_NEAR))
        w = W_FAR;
    if (idle && mode == SLICES && !(idle = hw_device_accesses() == st.hw))
        w = W_HW;
    int stores = stores_verdict(slot), pixels = drawn_verdict(slot), others = more_verdict(slot);
    if (others < stores)
        stores = others;
    if (idle && stores == CHANGED)
        w = W_STORES;
    else if (idle && pixels == CHANGED)
        w = W_PIXELS;
    else if (idle && (stores == UNKNOWN || pixels == UNKNOWN))
        w = W_UNKNOWN;
    if (pixels < stores)
        stores = pixels;
    if (stores == CHANGED)
        idle = false;
    why_count(w);
    st.same = idle ? st.same + 1 : 0;
    /* Once asleep, again only at the poll it slept at, the loop gone
     * round once in between: one polling from many places (an
     * interpreter's 23 polls a turn, a game's Button and TickCount) sees
     * each tick, not a poll a tick. */
    bool nothing_new = idle && stores == SAME;
    bool sleep = nothing_new && (mode != POLLS || !anchor.set || same_place(&anchor.r, &r));
    if (sleep && mode == POLLS)
        anchor.r = r;
    anchor.set = sleep ? mode == POLLS : anchor.set && idle;
    if (!idle)
        wait_broken();
    if (st.watched && stores == CHANGED && watch_after < MOST_BEFORE_WATCH)
        watch_after *= 2;
    else if (nothing_new)
        watch_after = SAME_BEFORE_WATCH;
    watch(st.same >= watch_after);
    if (idle && st.watched) {
        for (int i = 0; i < st.nstores; i++)
            *shot_store(slot, i) = *st_store(i);
        shot_at(slot)->nstores = st.nstores;
        memcpy(st.prev_drawn, st.drawn, (size_t)st.ndrawn * sizeof *st.drawn);
        st.nprev_drawn = st.ndrawn;
        memcpy(shot_at(slot)->d, st.drawn, (size_t)st.ndrawn * sizeof *st.drawn);
        shot_at(slot)->n = st.ndrawn;
        shot_at(slot)->look = st.look;
        memcpy(more_shots[slot].r, more.r, (size_t)more.n * sizeof *more.r);
        more_shots[slot].n = more.n;
        more_shots[slot].heap = more.fp;
    } else {
        st.nprev_drawn = 0;
        forget_shots();
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
    more.n = 0;
    more.overflow = more.heap = false;
    st.drew = false;
    st.ndrawn = st.nports = 0;
    st.drawn_overflow = false;
    st.watched = st.watching;
    st.at = executed();
    uint64_t t = machine_time_ns();
    iteration_took = t - iteration_began;
    iteration_began = t;
    return sleep;
}

static void chain_break(void)
{
    why_count(W_BREAK);
    wait_broken();
    anchor.set = false;
    last_site = -1;
    st.mode = NONE;
    st.same = 0;
    watch(false);
    st.watched = false;
    st.nprev_drawn = 0;
    forget_shots();
}

/* Sleep until the next tick, a Time Manager task's time or input.  While
 * interrupt-time work must be kept up with (sound playing, I/O completing,
 * a network open: machine_service_soon), a few milliseconds at most, or
 * four times as long as the iteration took the host if that is longer (one
 * computing for a while between its polls, MegaSeg's, woke after each
 * iteration's 2 ms as often as one of a few instructions); with none, a
 * program waiting at its main screen (MediaPresenter) no longer wakes 250
 * times a second for nothing. */
static void idle_sleep(void)
{
    uint64_t now = machine_time_ns();
    uint64_t tick = (now * 60 / 1000000000u + 1) * 1000000000u / 60 + 1;
    uint64_t until = tick, due = time_next_due();
    if (due && due < until)
        until = due;
    if (machine_service_soon()) {
        uint64_t most = iteration_took > MAX_SLEEP_NS / 4 ? iteration_took * 4 : MAX_SLEEP_NS;
        if (until > now + most)
            until = now + most;
    }
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
    /* Interrupt-time work comes as the clock moves, as on a Mac: with a
     * host attached the instructions don't move on while the thread
     * sleeps, so a poll loop could spend seconds asleep in one slice,
     * its VBL tasks, sound refills and completions held till the slice's
     * end (S.C.OUT's music stopped while "Perfect" waits for a key). */
    time_pump();
    st.at = executed();
    iteration_began = machine_time_ns();
}

/* Whether the last poll slept: a frame drawn the same again and again
 * (a pause screen) is this file's to wait through, not frame pacing's. */
BB_HOST static bool slept_at_poll;
bool idle_slept(void) { return slept_at_poll; }

bool idle_poll(void)
{
    slept_at_poll = false;
    if (!on() || in_interrupt())
        return false;
    if (!iteration_end(POLLS))
        return false;
    idle_sleep();
    slept_at_poll = true;
    return true;
}

void idle_work(void)
{
    if (!in_interrupt()) {
        st.busy = true;
        why_work("(input or a wait)");
    }
}

/* A poll loop's iteration may run long (MegaSeg's computing between its
 * polls); a loop of the program's own after a poll (Fastback's) shows as
 * two slices the same, registers and place, and is judged by its slices
 * from there. */
BB_HOST static struct {
    bool valid;
    regs r;
    uint32_t pc;
    uint64_t hw;
} probe;

static bool own_loop(void)
{
    if (executed() - st.at < OWN_LOOP_AFTER) {
        probe.valid = false;
        return false;
    }
    regs r;
    take(&r, false);
    uint32_t pc = pc_now();
    uint64_t hw = hw_device_accesses();
    bool same = probe.valid && !memcmp(&r, &probe.r, sizeof r) &&
                (pc > probe.pc ? pc - probe.pc : probe.pc - pc) < PC_NEAR && hw == probe.hw;
    probe.valid = !same;
    probe.r = r;
    probe.pc = pc;
    probe.hw = hw;
    return same;
}

void idle_slice(void)
{
    if (!on() || in_interrupt())
        return;
    check_over();
    if (calls != st.calls) {
        /* Calls in the slice: a poll loop's, its polls say; a long way from
         * the last poll, not a poll loop (only one calling nothing between
         * its polls may run long), unless every call was a query (an
         * interpreter's script between two of its polls: Frankenstein's
         * runs 33000 instructions and 156 calls). */
        if (st.mode != POLLS) {
            chain_break();
            st.calls = calls;
        } else if (executed() - st.at >= (st.busy ? OWN_LOOP_AFTER : QUIET_RUN)) {
            chain_break();
        }
        return;
    }
    if (st.mode == POLLS && executed() - st.at < MAX_RUN && !own_loop())
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
static const char *quiet_names[] = {
    "BitAnd", "BitNot", "BitOr", "BitShift", "BitTst", "BitXor", "Button", "CharWidth", "CountMItems", "DeltaPoint",
    "EmptyRect", "EmptyRgn", "EqualPt", "EqualRect", "EqualRgn", "EventAvail", "Fix2Frac", "Fix2Long", "FixATan2",
    "FixDiv", "FixMul", "FixRatio", "FixRound", "FlushEvents", "Frac2Fix", "FracCos", "FracDiv", "FracMul", "FracSin",
    "FracSqrt", "FrontNonFloatingWindow", "FrontWindow", "Gestalt", "GestaltDispatch", "GetBackColor", "GetCCursor",
    "GetCPixel", "GetCTSeed", "GetCaretTime", "GetClip", "GetControlValue", "GetCursor", "GetDateTime", "GetDblTime",
    "GetDeviceList", "GetEntryColor", "GetFontInfo", "GetForeColor", "GetGDevice", "GetGWorld", "GetGWorldDevice",
    "GetGWorldPixMap", "GetHandleSize", "GetKeys", "GetMainDevice", "GetMaxDevice", "GetMenuHandle", "GetMouse",
    "GetNextDevice", "GetNextEvent", "GetOSEvent", "GetOSTrapAddress", "GetPalette", "GetPenState", "GetPixBaseAddr",
    "GetPixel", "GetPixelsState", "GetPort", "GetPortBitMapForCopyBits", "GetPortBounds", "GetPortPixMap",
    "GetPtrSize", "GetQDGlobalsScreenBits", "GetQDGlobalsThePort", "GetToolTrapAddress", "GetToolboxTrapAddress",
    "GetTrapAddress", "GetWRefCon", "GetWindowPort", "GetWindowPortBounds", "GetZone", "HGetState", "HLock",
    "HNoPurge", "HSetState", "HUnlock", "HiWord", "InitCursor", "IsDialogEvent", "LoWord", "LoadSeg", "LockPixels",
    "Long2Fix", "Microseconds", "NGetTrapAddress", "OSEventAvail", "ObscureCursor", "PixMap32Bit", "PtInRect",
    "PtInRgn", "QDError", "ReadDateTime", "RectInRgn", "SetCCursor", "SetCursor", "SetEventMask", "SetGDevice",
    "SetGWorld", "SetPixelsState", "SetPort", "SetZone", "StillDown", "StringWidth", "StripAddress", "SystemTask",
    "TestDeviceAttribute", "TextWidth", "TickCount", "UnloadSeg", "UnlockPixels", "WaitMouseUp", "WaitNextEvent",
};

/* Calls whose only effect is the answer they write into the program's
 * memory, which they say (idle_wrote): the same answer again is the same;
 * or which say when they do more (idle_work: DialogSelect given an event
 * to handle, not a null one; HideControl a control shown).  In order, for
 * bsearch. */
static const char *const answer_names[] = {
    "ActivateControl", "AddPt", "BlockMove", "BlockMoveData", "CheckItem", "CopyRgn", "DeactivateControl",
    "DialogSelect", "DiffRgn", "DisableItem", "EnableItem", "FindControlUnderMouse", "FindWindow",
    "GetControlBounds", "GetControlCommandID", "GetControlData", "GetControlProperty", "GetDialogItem",
    "GetDialogItemAsControl", "GetKeyboardFocus", "GetListCellIndent", "GetListCellSize", "GetListClickLocation",
    "GetListDataBounds", "GetListMouseLocation", "GetListViewBounds", "GetListVisibleCells", "GetPixBounds",
    "GetPortBackColor", "GetPortBounds", "GetPortClipRegion", "GetPortForeColor", "GetPortHiliteColor",
    "GetPortOpColor", "GetPortPenLocation", "GetPortPenSize", "GetPortVisibleRegion", "GetQDGlobalsArrow",
    "GetQDGlobalsBlack", "GetQDGlobalsDarkGray", "GetQDGlobalsGray", "GetQDGlobalsLightGray",
    "GetQDGlobalsScreenBits", "GetQDGlobalsWhite", "GetRegionBounds", "GetRootControl", "GetWindowBounds",
    "GetWindowProperty", "GlobalToLocal", "HideControl", "InsTime", "InsXTime", "InsetRect", "InsetRgn",
    "LocalToGlobal", "MapPt", "MapRect", "MapRgn", "NumToString", "OffsetRect", "OffsetRgn", "PrimeTime", "Pt2Rect",
    "PtToAngle", "RectRgn", "RmvTime", "ScalePt", "SectRect", "SectRgn", "SetControlValue", "SetEmptyRgn",
    "SetItemCmd", "SetItemMark", "SetItemStyle", "SetPt", "SetRect", "SetRectRgn", "ShowControl",
    "SndChannelStatus", "StdFilterProc", "StringToNum", "SubPt", "TEIdle", "UnionRect", "UnionRgn", "UseResFile",
    "XorRgn",
};

/* More calls with no lasting effect (beside quiet_names, whose size is
 * saved state): queries, and Mixed Mode's ways into and out of code
 * (what runs there is the program's iteration, its calls and stores
 * watched as any; IdleControls's control definitions likewise, or ours,
 * whose drawing shows in the pixels).  In order, for bsearch. */
static const char *const query_names[] = {
    "CallOSTrapUniversalProc", "CallUniversalProc", "CheckUpdate", "CurResFile", "FindDialogItem", "FreeMem",
    "GetControl32BitMaximum", "GetControl32BitMinimum", "GetControl32BitValue", "GetControlAction",
    "GetControlDataHandle", "GetControlHilite", "GetControlMaximum", "GetControlMinimum", "GetControlOwner",
    "GetControlReference", "GetControlVariant", "GetDialogCancelItem", "GetDialogDefaultItem",
    "GetDialogFromWindow", "GetDialogKeyboardFocusItem", "GetDialogPort", "GetDialogWindow", "GetEvQHdr",
    "GetFrontWindowOfClass", "GetGrayRgn", "GetListActive", "GetListClickLoop", "GetListClickTime",
    "GetListDataHandle", "GetListDefinition", "GetListFlags", "GetListHorizontalScrollBar", "GetListPort",
    "GetListRefCon", "GetListSelectionFlags", "GetListUserHandle", "GetListVerticalScrollBar", "GetNextWindow",
    "GetNextWindowOfClass", "GetPixDepth", "GetPortChExtra", "GetPortFracHPenLocation", "GetPortGrafProcs",
    "GetPortPenMode", "GetPortPenVisibility", "GetPortSpExtra", "GetPortTextFace", "GetPortTextFont",
    "GetPortTextMode", "GetPortTextSize", "GetQDGlobalsRandomSeed", "GetWindowFromPort", "GetWindowKind",
    "GetWindowList", "GetWindowPic", "GetWindowStructurePort", "HMGetBalloons", "HMIsBalloon", "HPurge",
    "IUMagIDString", "IUMagString", "IdleControls", "InfoScrap", "IsControlActive", "IsControlHilited",
    "IsControlVisible", "IsMovieDone", "IsPortClipRegionEmpty", "IsPortColor", "IsPortOffscreen",
    "IsPortPictureBeingDefined", "IsPortPolyBeingDefined", "IsPortRegionBeingDefined", "IsPortVisibleRegionEmpty",
    "IsRegionRectangular", "IsValidPort", "IsWindowActive", "IsWindowHilited", "IsWindowUpdatePending",
    "IsWindowVisible", "MaxBlock", "Mixed Mode return", "MoviesTask", "SetPortWindowPort", "SetThemeCursor",
    "StackSpace", "YieldToAnyThread", "YieldToThread",
};

/* Calls that make or dispose of blocks, or move or resize them: what they
 * leave is the heaps' shape, compared at the iteration's end (a loop making
 * a handle for a moment and disposing of it, as Director's projectors do,
 * leaves it as it was); what they copy, HandToHand says.  In order, for
 * bsearch. */
static const char *const heap_names[] = {
    "DisposeHandle", "DisposePtr", "DisposeRgn", "HandToHand", "MoveHHi", "NewHandle", "NewHandleClear", "NewPtr",
    "NewPtrClear", "NewRgn", "SetHandleSize", "SetPtrSize",
};

/* Drawing, and the port and colour settings it is done with: what they
 * leave is the pixels drawn (qd_drawn), the ports drawn with and the
 * colours shown, which the iteration's end compares with the last's. */
static const char *draw_names[] = {
    "ActivatePalette", "AnimateEntry", "AnimatePalette", "BackColor", "BackPat", "BackPixPat", "CharExtra",
    "ClipRect", "ColorBit", "CopyBits", "CopyDeepMask", "CopyMask", "DrawChar", "DrawPicture", "DrawString",
    "DrawText", "EraseArc", "EraseOval", "ErasePoly", "EraseRect", "EraseRgn", "EraseRoundRect", "FillArc",
    "FillCArc", "FillCOval", "FillCPoly", "FillCRect", "FillCRgn", "FillCRoundRect", "FillOval", "FillPoly",
    "FillRect", "FillRgn", "FillRoundRect", "ForeColor", "FrameArc", "FrameOval", "FramePoly", "FrameRect",
    "FrameRgn", "FrameRoundRect", "HidePen", "HiliteColor", "InvertArc", "InvertOval", "InvertPoly",
    "InvertRect", "InvertRgn", "InvertRoundRect", "Line", "LineTo", "Move", "MoveTo", "OpColor",
    "PaintArc", "PaintOval", "PaintPoly", "PaintRect", "PaintRgn", "PaintRoundRect", "PenMode", "PenNormal",
    "PenPat", "PenPixPat", "PenSize", "PlotCIcon", "PlotCIconHandle", "PlotIcon", "PlotIconHandle", "PlotIconID",
    "PlotIconSuite", "PlotSICNHandle", "PmBackColor", "PmForeColor", "RGBBackColor", "RGBForeColor",
    "RestoreBack", "RestoreFore", "SetCPixel", "SetClip", "SetEntries", "SetOrigin", "SetPenState",
    "ShowPen", "SpaceExtra", "StdArc", "StdBits", "StdLine", "StdOval", "StdPoly",
    "StdRRect", "StdRect", "StdRgn", "StdText", "TextFace", "TextFont", "TextMode", "TextSize",
};

/* Dispatchers whose routines are told apart by selector (idle_selector). */
static const char *const by_selector[] = { "IconDispatch", "PaletteDispatch", "QDExtensions" };

static int by_name(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

static bool in_list(const char *name, const char **list, size_t n)
{
    static bool sorted;
    if (!sorted) {
        qsort(quiet_names, sizeof quiet_names / sizeof *quiet_names, sizeof *quiet_names, by_name);
        qsort(draw_names, sizeof draw_names / sizeof *draw_names, sizeof *draw_names, by_name);
        sorted = true;
    }
    return name && bsearch(&name, list, n, sizeof *list, by_name) != NULL;
}

static uint8_t kind_of(const char *name)
{
    /* The low-memory accessors' getters (LMGetTicks, LMGetMBarHeight) only read. */
    if (in_list(name, quiet_names, sizeof quiet_names / sizeof *quiet_names) || (name && !strncmp(name, "LMGet", 5)))
        return QUIET;
    if (name && (bsearch(&name, answer_names, sizeof answer_names / sizeof *answer_names, sizeof(char *), by_name) ||
                 bsearch(&name, query_names, sizeof query_names / sizeof *query_names, sizeof(char *), by_name)))
        return QUIET;
    if (name && bsearch(&name, heap_names, sizeof heap_names / sizeof *heap_names, sizeof(char *), by_name))
        return HEAP;
    if (in_list(name, draw_names, sizeof draw_names / sizeof *draw_names))
        return DRAWS;
    for (size_t i = 0; name && i < sizeof by_selector / sizeof *by_selector; i++)
        if (!strcmp(name, by_selector[i]))
            return SELECTOR;
    return WORK;
}

/* A drawing call: the port it draws with is compared at the iteration's
 * end.  One recording what is drawn (a picture, a region, a polygon, a
 * printed page) grows that, which is work. */
static void drawing(void)
{
    gaddr port = qd_port();
    if (!port || rd32(port + GrafPort_picSave_off) || rd32(port + GrafPort_rgnSave_off) ||
        rd32(port + GrafPort_polySave_off) || qd_region_recording() || qd_rec_active(port)) {
        st.busy = true;
        return;
    }
    st.drew = true;
    for (int i = 0; i < st.nports; i++)
        if (st.ports[i] == port)
            return;
    if (st.nports == MAX_PORTS)
        st.busy = true;
    else
        st.ports[st.nports++] = port;
}

static void call_kind(uint8_t kind, const char *name)
{
    call_now = kind;
    if (kind == WORK) {
        st.busy = true;
        why_work(name);
    } else if (kind == DRAWS) {
        pace_port(qd_port()); /* a frame, if on the screen */
        drawing();
    } else if (kind == HEAP) {
        more.heap = true;
    }
}

/* A 68k trap, by its number. */
static uint8_t trap_kind[2048];

void idle_trap(uint16_t op, bool by_selector)
{
    if (in_interrupt())
        return;
    calls++;
    call_now = UNSEEN;
    if (by_selector)
        return; /* a dispatcher of ours (SoundDispatch, Pack7 ...): idle_selector says */
    unsigned k = (op & 0x0800) ? 1024 + (op & 0x3FFu) : (op & 0xFFu);
    if (!trap_kind[k])
        trap_kind[k] = kind_of(trap_name(op));
    call_kind(trap_kind[k], why.on > 0 ? trap_name(op) : NULL); /* a dispatcher's routine: idle_selector says */
}

/* The routine a dispatcher (QDExtensions, IconDispatch ...) was asked for. */
void idle_selector(const char *name)
{
    if (in_interrupt())
        return;
    uint8_t kind = kind_of(name);
    call_kind(kind == SELECTOR ? WORK : kind, name);
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
        uint8_t *k = bb_realloc(kind, n, BB_BYTES);
        if (!k) {
            st.busy = true;
            return;
        }
        memset(k + nkind, 0, n - nkind);
        kind = k;
        nkind = n;
    }
    if (!kind[index])
        kind[index] = kind_of(name);
    call_kind(kind[index] == SELECTOR ? WORK : kind[index], name);
}

void idle_reset(void)
{
    watch(false);
    memset(&st, 0, sizeof st);
    watch_after = SAME_BEFORE_WATCH;
    memset(sites, 0, sizeof sites);
    memset(more_sites, 0, sizeof more_sites);
    memset(site_seen, 0, sizeof site_seen);
    memset(stretches, 0, sizeof stretches);
    memset(two_ways, 0, sizeof two_ways);
    last_site = -1;
    anchor.set = false;
    memset(&more, 0, sizeof more);
    forget_shots();
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
