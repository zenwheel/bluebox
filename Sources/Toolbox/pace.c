/*
 * Frame pacing: a program drawing frames faster than the screen shows them.
 *
 * A game or an animation drawing as fast as it can, never waiting between
 * frames, went as fast as the Mac it ran on; here the host is a hundred
 * times faster, and the program draws hundreds of frames a second on a
 * core of its own, of which the display shows sixty.  When the machine's
 * time is on the wall clock (a host attached, or --realtime), once the
 * program has finished a frame since the display's last refresh (the 60 Hz
 * tick), the next one it finishes waits for the next refresh before the
 * program goes on: what it computes for frames nobody could see isn't
 * computed, and a program timed by its frames goes at sixty a second.
 *
 * A frame is what the program drew on the screen (QuickDraw into a window
 * or the screen, CopyBits onto it, its own stores into the screen's memory,
 * an OpenGL or DrawSprocket swap) between two visits of the place its loop
 * polls from: the event poll (WaitNextEvent, GetNextEvent, EventAvail ...),
 * or with none, the input poll (GetKeys, Button, GetMouse ...), or a swap
 * of buffers, the highest of these the loop has made lately, and of those
 * the one call site first seen; other polls within the frame leave it
 * whole.  Drawing elsewhere (a GWorld, an offscreen bitmap) isn't a frame
 * until it reaches the screen, and a program computing without drawing (a
 * renderer, a chess engine thinking) is never slowed.  Nor is one already
 * slower than the display: its frames each take a refresh or more, so two
 * never end in one.
 *
 * The wait is the throttle's (machine_pace_wait): interrupt-time work (the
 * Time Manager, VBL tasks, sound, the network) goes on through it, the
 * clock moves on as ever, so TickCount and Microseconds read true, and
 * input ends it at once.  A key or a click that came while the frame was
 * drawn skips it, and so does a frame drawn the same again and again that
 * idle.c found the program waiting through and slept for.  The instruction clock (headless runs, the corpus walk)
 * never waits, so those runs stay the same.  BLUEBOX_PACE=0 turns it off;
 * the frames are still counted (--log idle says every five seconds how
 * many were drawn and how many refreshes showed a new one).
 */
#include <stdlib.h>

#include "lowmem_gen.h"
#include "m68k.h"
#include "machine.h"
#include "ppc.h"
#include "qd_internal.h"
#include "structs_gen.h"
#include "tb_internal.h"

enum {
    FORGET_TICKS = 30, /* a pacing place not polled for this long gives way to another */
    LOG_EVERY_NS = 5000000000u,
};

BB_HOST static int enabled = -1;  /* the run's environment, or machine_set_frame_pacing */
BB_HOST static bool drew;         /* the screen drawn on since the pacing place */
BB_HOST static uint32_t site;     /* the pacing place: the poll's call site ... */
BB_HOST static int site_rank;     /* ... what kind of poll it is ... */
BB_HOST static uint64_t site_tick; /* ... and the tick it was last polled in */
BB_HOST static uint64_t frame_tick; /* the tick the last frame ended in, plus one (0: none) */
BB_HOST static uint32_t presses;  /* keys and clicks at the last frame's end */
BB_HOST static uint64_t frames, shown, waits, waited_ns;
BB_HOST static struct {
    uint64_t at, frames, shown, waits, ns;
} logged;

static bool pacing(void)
{
    if (enabled < 0) {
        const char *e = getenv("BLUEBOX_PACE");
        enabled = !(e && e[0] == '0');
    }
    return enabled;
}

void machine_set_frame_pacing(bool on) { enabled = on; }

uint64_t machine_frames_paced(uint64_t *drawn, uint64_t *refreshes, uint64_t *ns)
{
    if (drawn)
        *drawn = frames;
    if (refreshes)
        *refreshes = shown;
    if (ns)
        *ns = waited_ns;
    return waits;
}

/* Frames are counted only on the wall clock, at the program's own level. */
static bool counting(void)
{
    return g_cpu && !g_cpu->halted && machine_wall_clock() && !idle_in_interrupt();
}

static bool on_screen(gaddr base)
{
    int w, h;
    machine_screen_size(&w, &h);
    gaddr lo = rd32(LM_ScrnBase);
    return base - lo < qd_screen_row_bytes() * (uint32_t)h;
}

void pace_dest(gaddr base)
{
    if (!drew && counting() && on_screen(base))
        drew = true;
}

void pace_port(gaddr port)
{
    if (drew || !port || !counting())
        return;
    gaddr base = qd_port_is_color(port) ? PixMap_baseAddr(rd32(rd32(port + CGrafPort_portPixMap_off)))
                                        : BitMap_baseAddr(port + GrafPort_portBits_off);
    if (on_screen(base))
        drew = true;
}

void pace_drew(void) { drew = true; }

static void log_now(uint64_t now)
{
    if (!logged.at) {
        logged.at = now;
        return;
    }
    if (now - logged.at < LOG_EVERY_NS)
        return;
    double s = (double)(now - logged.at) / 1e9;
    if (frames > logged.frames)
        LOGF(LOG_IDLE, "pace: %.1f frames a second drawn, %.1f shown; %llu waits, %.3f s\n",
             (double)(frames - logged.frames) / s, (double)(shown - logged.shown) / s,
             (unsigned long long)(waits - logged.waits), (double)(waited_ns - logged.ns) / 1e9);
    logged.at = now;
    logged.frames = frames;
    logged.shown = shown;
    logged.waits = waits;
    logged.ns = waited_ns;
}

void pace_poll(int rank)
{
    if (!counting())
        return;
    uint32_t at = g_ppc_mode && !mm_level_68k() ? g_ppc.lr : g_cpu->op_pc;
    uint64_t now = machine_time_ns(), tick = now * 60 / 1000000000u;
    log_now(now);
    if (at != site) {
        /* Another poll within the frame: the frame goes on. */
        if (rank <= site_rank && tick - site_tick <= FORGET_TICKS)
            return;
        site = at;
        site_rank = rank;
    }
    site_tick = tick;
    display_pace_watch();
    if (!drew)
        return;
    drew = false;
    frames++;
    uint32_t p = ev_press_serial();
    bool pressed = p != presses;
    presses = p;
    if (frame_tick != tick + 1 || pressed || idle_slept() || !pacing() || drain_wanted()) {
        if (frame_tick != tick + 1)
            shown++;
        frame_tick = tick + 1;
        return;
    }
    /* A second frame in this refresh: the next one comes after it. */
    uint64_t next = (tick + 1) * 1000000000u / 60 + 1;
    uint64_t slept = machine_pace_wait(next - now);
    if (!events_host_attached())
        machine_pass_time(slept);
    waits++;
    waited_ns += slept;
    frame_tick = machine_time_ns() * 60 / 1000000000u + 1;
    shown++;
}

void pace_reset(void)
{
    drew = false;
    site = 0;
    site_rank = 0;
    site_tick = frame_tick = 0;
}
