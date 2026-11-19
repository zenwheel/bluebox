/*
 * present.h's host-neutral half: the refreshes frame pacing goes by, and
 * the statistics (BLUEBOX_PRESENT_STATS=1).  The GPU's half is the host's
 * (present_metal.m); a host without one has the stand-ins at the end.
 */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "present.h"
#include "present_backend.h"

void ev_wake(void);

uint64_t present_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* ---- Refreshes ---- */

/* Refreshes begun and read, the last one's time, and whether a surface is
 * shown: all the host's, none of them saved. */
BB_HOST static _Atomic uint64_t vsync_seq, vsync_done, vsync_at;
BB_HOST static _Atomic bool shown;

/* Refreshes older than this mean the display link stopped (the display
 * asleep, the window on no display): pacing goes back to the wall clock. */
enum { LIVE_NS = 50000000 };

uint64_t present_vsync_begin(void)
{
    atomic_store(&vsync_at, present_now_ns());
    return atomic_fetch_add(&vsync_seq, 1) + 1;
}

void present_vsync_read(uint64_t seq)
{
    uint64_t done = atomic_load(&vsync_done);
    while (done < seq && !atomic_compare_exchange_weak(&vsync_done, &done, seq)) {
    }
    ev_wake(); /* a program waiting for the refresh goes on */
}

void present_set_shown(bool any)
{
    if (atomic_exchange(&shown, any) && !any)
        ev_wake(); /* not live any more: a wait for a refresh is over */
}

bool present_live(void)
{
    uint64_t at = atomic_load(&vsync_at);
    return atomic_load(&shown) && at && present_now_ns() - at < LIVE_NS;
}

uint64_t present_vsync_seq(void) { return atomic_load(&vsync_seq); }
bool present_read_done(uint64_t seq) { return atomic_load(&vsync_done) >= seq; }

/* ---- Statistics ---- */

BB_HOST static int stats = -1;
BB_HOST static _Atomic uint64_t st_main_ns, st_main_n, st_frames, st_rows, st_bytes, st_vsyncs, st_scans, st_skips,
    st_render_ns, st_render_max, st_hold_ns, st_hold_n, st_hold_max, st_read_ns, st_read_n, st_read_max;
BB_HOST static uint64_t st_at;

bool present_stats_on(void)
{
    if (stats < 0) {
        const char *e = getenv("BLUEBOX_PRESENT_STATS");
        stats = e && *e && *e != '0';
    }
    return stats;
}

static void note_max(_Atomic uint64_t *m, uint64_t v)
{
    uint64_t old = atomic_load(m);
    while (v > old && !atomic_compare_exchange_weak(m, &old, v)) {
    }
}

void present_stats_main(uint64_t ns)
{
    atomic_fetch_add(&st_main_ns, ns);
    atomic_fetch_add(&st_main_n, 1);
}

void present_stats_cpu_frame(int rows, int width)
{
    atomic_fetch_add(&st_frames, 1);
    atomic_fetch_add(&st_rows, (uint64_t)rows);
    atomic_fetch_add(&st_bytes, (uint64_t)rows * (uint64_t)width * 4);
}

void present_stats_gpu(bool scanned, bool skipped, int frames, int rows, uint64_t bytes, uint64_t render_ns)
{
    atomic_fetch_add(&st_vsyncs, 1);
    if (scanned)
        atomic_fetch_add(&st_scans, 1);
    if (skipped)
        atomic_fetch_add(&st_skips, 1);
    atomic_fetch_add(&st_frames, (uint64_t)frames);
    atomic_fetch_add(&st_rows, (uint64_t)rows);
    atomic_fetch_add(&st_bytes, bytes);
    atomic_fetch_add(&st_render_ns, render_ns);
    note_max(&st_render_max, render_ns);
}

void present_stats_read(uint64_t ns)
{
    atomic_fetch_add(&st_read_ns, ns);
    atomic_fetch_add(&st_read_n, 1);
    note_max(&st_read_max, ns);
}

BB_HOST static _Atomic uint64_t cpu_total, st_missed;
void present_stats_missed(uint64_t rows) { atomic_fetch_add(&st_missed, rows); }
void present_stats_cpu(uint64_t ns) { atomic_fetch_add(&cpu_total, ns); }
uint64_t present_cpu_ns(void) { return atomic_load(&cpu_total); }

void present_note_hold(uint64_t ns)
{
    atomic_fetch_add(&st_hold_ns, ns);
    atomic_fetch_add(&st_hold_n, 1);
    note_max(&st_hold_max, ns);
}

void present_stats_tick(void)
{
    if (!present_stats_on())
        return;
    uint64_t now = present_now_ns();
    if (!st_at) {
        st_at = now;
        return;
    }
    if (now - st_at < 1000000000u)
        return;
    double s = (double)(now - st_at) / 1e9;
    st_at = now;
    uint64_t main_ns = atomic_exchange(&st_main_ns, 0), main_n = atomic_exchange(&st_main_n, 0);
    uint64_t frames = atomic_exchange(&st_frames, 0), rows = atomic_exchange(&st_rows, 0);
    uint64_t bytes = atomic_exchange(&st_bytes, 0), vsyncs = atomic_exchange(&st_vsyncs, 0);
    uint64_t scans = atomic_exchange(&st_scans, 0), skips = atomic_exchange(&st_skips, 0);
    uint64_t render = atomic_exchange(&st_render_ns, 0), render_max = atomic_exchange(&st_render_max, 0);
    uint64_t hold = atomic_exchange(&st_hold_ns, 0), hold_n = atomic_exchange(&st_hold_n, 0);
    uint64_t hold_max = atomic_exchange(&st_hold_max, 0);
    uint64_t read = atomic_exchange(&st_read_ns, 0), read_n = atomic_exchange(&st_read_n, 0);
    uint64_t read_max = atomic_exchange(&st_read_max, 0);
    fprintf(stderr,
            "present: %s; main thread %.3f ms a frame over %llu; %.1f frames/s drawn, %llu rows (%.1f MB) copied; "
            "vsyncs %llu, full looks %llu, skipped %llu, unmarked rows found %llu; render %.3f ms avg %.3f max; "
            "held past the refresh %.3f ms avg %.3f max; paced waits %.3f ms avg %.3f max over %llu; pacing %s\n",
            present_path(), main_n ? (double)main_ns / (double)main_n / 1e6 : 0.0, (unsigned long long)main_n,
            (double)frames / s, (unsigned long long)rows, (double)bytes / 1e6, (unsigned long long)vsyncs,
            (unsigned long long)scans, (unsigned long long)skips, (unsigned long long)atomic_exchange(&st_missed, 0), vsyncs ? (double)render / (double)vsyncs / 1e6 : 0.0,
            (double)render_max / 1e6, read_n ? (double)read / (double)read_n / 1e6 : 0.0, (double)read_max / 1e6,
            hold_n ? (double)hold / (double)hold_n / 1e6 : 0.0, (double)hold_max / 1e6, (unsigned long long)hold_n,
            present_live() ? "on refreshes" : "wall clock");
}

/* ---- No GPU presenter on this host ---- */

#if !defined(__APPLE__)
bool present_start(void) { return false; }
const char *present_path(void) { return "cpu"; }
bool present_force_path(const char *path) { return false; }
present_surface *present_surface_new(void *layer) { return NULL; }
void present_surface_free(present_surface *s) {}
void present_surface_set(present_surface *s, double x, double y, double w, double h, int pixels_w, int pixels_h) {}
void present_surface_set_shape(present_surface *s, const uint8_t *mask, int w, int h) {}
void present_surface_set_desktop(present_surface *s, bool leave_out) {}
void present_surface_set_visible(present_surface *s, bool visible) {}
void present_surface_draw_now(present_surface *s) {}
void present_vsync(double hz) {}
void present_wake(void) {}
bool present_own_display_link(uint32_t display_id) { return false; }
void present_stop_display_link(void) {}
bool present_offscreen(double x, double y, double w, double h, int out_w, int out_h, const uint8_t *shape, bool desktop,
                       uint8_t *rgba)
{
    return false;
}
void present_memory_changing(void *lo, size_t len) {}
#endif
