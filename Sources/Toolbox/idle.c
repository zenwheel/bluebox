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
 *     (quiet_names: polls, FrontWindow, GetPort, LoadSeg ...) or drawing
 *     (draw_names: CopyBits, PaintRect, DrawString, PlotCIcon, RGBForeColor,
 *     SetEntries ...), no sound, file or memory work;
 *   - the registers are as they were at the iteration before ending at the
 *     same poll (a count kept in a register shows) and, with no calls, no
 *     device was touched;
 *   - its stores, watched once the rest has held for a few iterations,
 *     left the places it stored to as that iteration left them (the
 *     same arguments pushed again; a counter kept in memory shows), its own
 *     frames below the stack pointer apart;
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
 * them.  Interrupt-time work (VBL and Time Manager tasks, sound callbacks)
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
    MAX_RUN = 20000,       /* instructions a poll loop's iteration may run */
    MAX_STORES = 64,       /* places an idle iteration may store to */
    MAX_DRAWN = 16,        /* destinations it may draw into */
    MAX_PORTS = 8,         /* ports it may draw with */
    MAX_PALETTES = 16,     /* palettes whose colours are compared */
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
static bool same_at_site(const regs *r, int *site)
{
    for (int i = 0; i < NSITES; i++)
        if (sites[i].used && sites[i].r.r[34] == r->r[34] && sites[i].r.r[32] == r->r[32] &&
            sites[i].r.r[35] == r->r[35]) {
            bool same = !memcmp(&sites[i].r, r, sizeof *r);
            sites[i].r = *r;
            *site = i;
            return same;
        }
    *site = (int)next_site;
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
static struct {
    int n; /* -1: none yet in this wait */
    drawn d[MAX_DRAWN];
    uint64_t look;
    int nstores;
    store stores[MAX_STORES];
} shots[NSITES + 1];

static void forget_shots(void)
{
    for (int i = 0; i <= NSITES; i++)
        shots[i].n = -1;
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

bool g_qd_drawn_watch;

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
bool qd_region_recording(void);
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

static bool drawn_store(gaddr a, uint32_t len);

static void store_seen(gaddr a, uint32_t len)
{
    if (!st.watching || in_interrupt())
        return;
    if (drawn_store(a, len))
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

static int stores_verdict(int site)
{
    if (!st.watched)
        return UNKNOWN;
    if (st.overflow)
        return CHANGED;
    const int n = shots[site].n < 0 ? -1 : shots[site].nstores;
    const store *prev = shots[site].stores;
    int verdict = n < 0 ? UNKNOWN : SAME;
    for (int i = 0; i < st.nstores; i++) {
        store *s = &st.stores[i];
        for (uint32_t k = 0; k < s->len; k++)
            s->v[k] = rd8(s->a + k);
        if (verdict != SAME)
            continue;
        int j = 0;
        while (j < n && (prev[j].a != s->a || prev[j].len != s->len))
            j++;
        if (j == n || memcmp(prev[j].v, s->v, s->len))
            verdict = CHANGED;
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
    const int n = shots[site].n;
    if (n < 0)
        return UNKNOWN;
    if (st.ndrawn != n || st.look != shots[site].look)
        return CHANGED;
    for (int i = 0; i < st.ndrawn; i++) {
        const drawn *d = &st.drawn[i];
        int j = 0;
        while (j < n && (shots[site].d[j].base != d->base || shots[site].d[j].row_bytes != d->row_bytes))
            j++;
        if (j == n)
            return CHANGED;
        const drawn *e = &shots[site].d[j];
        if (e->r0 != d->r0 || e->r1 != d->r1 || e->b0 != d->b0 || e->b1 != d->b1 || e->hash != d->hash)
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

/* An iteration ends: whether it was idle (and enough of them in a row,
 * watched, to sleep); the next one starts. */
static bool iteration_end(int mode)
{
    regs r;
    take(&r, mode == POLLS);
    uint32_t pc = pc_now();
    int site = NSITES;
    bool same = mode == POLLS ? same_at_site(&r, &site) : !memcmp(&r, &st.r, sizeof r);
    bool idle = st.mode == mode && !st.busy && same;
    if (idle && mode == POLLS)
        idle = executed() - st.at < MAX_RUN;
    if (idle && mode == SLICES)
        idle = (pc > st.pc ? pc - st.pc : st.pc - pc) < PC_NEAR && hw_device_accesses() == st.hw;
    int stores = stores_verdict(site), pixels = drawn_verdict(site);
    if (pixels < stores)
        stores = pixels;
    if (stores == CHANGED)
        idle = false;
    st.same = idle ? st.same + 1 : 0;
    bool sleep = idle && stores == SAME;
    if (!idle)
        wait_broken();
    watch(st.same >= SAME_BEFORE_WATCH);
    if (idle && st.watched) {
        memcpy(shots[site].stores, st.stores, (size_t)st.nstores * sizeof *st.stores);
        shots[site].nstores = st.nstores;
        memcpy(st.prev_drawn, st.drawn, (size_t)st.ndrawn * sizeof *st.drawn);
        st.nprev_drawn = st.ndrawn;
        memcpy(shots[site].d, st.drawn, (size_t)st.ndrawn * sizeof *st.drawn);
        shots[site].n = st.ndrawn;
        shots[site].look = st.look;
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
    st.drew = false;
    st.ndrawn = st.nports = 0;
    st.drawn_overflow = false;
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
    st.nprev_drawn = 0;
    forget_shots();
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
static const char *quiet_names[] = {
    "Button", "CharWidth", "EventAvail", "FlushEvents", "FrontNonFloatingWindow", "FrontWindow", "Gestalt",
    "GestaltDispatch", "GetBackColor", "GetCCursor", "GetCPixel", "GetCTSeed", "GetCaretTime", "GetClip",
    "GetCursor", "GetDateTime", "GetDblTime", "GetDeviceList", "GetEntryColor", "GetFontInfo", "GetForeColor",
    "GetGDevice", "GetGWorld", "GetGWorldDevice", "GetGWorldPixMap", "GetHandleSize", "GetKeys", "GetMainDevice",
    "GetMaxDevice", "GetMouse", "GetNextDevice", "GetNextEvent", "GetOSEvent", "GetOSTrapAddress", "GetPalette",
    "GetPenState", "GetPixBaseAddr", "GetPixel", "GetPixelsState", "GetPort", "GetPortBitMapForCopyBits",
    "GetPortBounds", "GetPortPixMap", "GetPtrSize", "GetQDGlobalsScreenBits", "GetQDGlobalsThePort",
    "GetToolTrapAddress", "GetToolboxTrapAddress", "GetTrapAddress", "GetWindowPort", "GetWindowPortBounds",
    "GetZone", "HGetState", "HLock", "HNoPurge", "HSetState", "HUnlock", "InitCursor", "LMGetTicks", "LMGetTime",
    "LoadSeg", "LockPixels", "Microseconds", "NGetTrapAddress", "OSEventAvail", "ObscureCursor", "PixMap32Bit",
    "QDError", "ReadDateTime", "SetCCursor", "SetCursor", "SetEventMask", "SetGDevice", "SetGWorld", "SetPixelsState",
    "SetPort", "SetZone", "StillDown", "StringWidth", "StripAddress", "SystemTask", "TestDeviceAttribute", "TextWidth",
    "TickCount", "UnloadSeg", "UnlockPixels", "WaitMouseUp", "WaitNextEvent",
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

/* What a call is: */
enum { UNSEEN, QUIET, DRAWS, SELECTOR, WORK };

static uint8_t kind_of(const char *name)
{
    if (in_list(name, quiet_names, sizeof quiet_names / sizeof *quiet_names))
        return QUIET;
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

static void call_kind(uint8_t kind)
{
    if (kind == WORK)
        st.busy = true;
    else if (kind == DRAWS)
        drawing();
}

/* A 68k trap, by its number. */
static uint8_t trap_kind[2048];

void idle_trap(uint16_t op)
{
    if (in_interrupt())
        return;
    calls++;
    unsigned k = (op & 0x0800) ? 1024 + (op & 0x3FFu) : (op & 0xFFu);
    if (!trap_kind[k])
        trap_kind[k] = kind_of(trap_name(op));
    call_kind(trap_kind[k]); /* a dispatcher's routine: idle_selector says */
}

/* The routine a dispatcher (QDExtensions, IconDispatch ...) was asked for. */
void idle_selector(const char *name)
{
    if (in_interrupt())
        return;
    uint8_t kind = kind_of(name);
    call_kind(kind == SELECTOR ? WORK : kind);
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
        kind[index] = kind_of(name);
    call_kind(kind[index] == SELECTOR ? WORK : kind[index]);
}

void idle_reset(void)
{
    watch(false);
    memset(&st, 0, sizeof st);
    memset(sites, 0, sizeof sites);
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
