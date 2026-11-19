/*
 * The screen on the host's display: the classic framebuffer decoded and
 * scaled by the GPU in the presentation layer.  A host (a window, a
 * rootless window, the screen saver's full screen) has a surface: a layer
 * the presenter draws a rectangle of the Mac screen into, at the layer's
 * size, with the window's shape and the desktop left out as it asks.  The
 * presenter keeps one copy of the screen for every surface, finds the rows
 * that changed and draws only the surfaces they touch, once a refresh of
 * the display (present_vsync).  Everything here is host-neutral: macOS
 * implements it with Metal (present_metal.m); Linux will with OpenGL or
 * Vulkan (PLAN M12).  A host without it (no GPU, BLUEBOX_METAL=0) converts
 * on the CPU (machine_screen_rgba_bands).
 *
 * Frame pacing (pace.c) takes its refreshes from here while a surface is
 * shown and the display's refreshes come (present_live): a program that
 * finished a frame and waits for the next refresh is let go once that
 * refresh's read of the screen is done, so what is shown is a whole frame.
 */
#ifndef PRESENT_H
#define PRESENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The screen as it is shown: its page, its form and the colours it is
 * shown through (the video hardware's table, the gamma and fades, After
 * Dark's brightness). */
typedef struct {
    int32_t width, height, depth, brightness;
    uint32_t row_bytes, base; /* base: the guest address of the page shown */
    uint8_t clut[256][3];
    uint8_t lut[3][256];
    bool lut_on;
} present_state;
void present_screen_state(present_state *s);
/* The page's rows compared with a copy of them (row_bytes a row, height
 * rows) and the copy brought up to date: the changed rows as bands
 * [y0, y1) in bands (pairs, at most max, the last widened past that).
 * The number of bands, or -1 if s is no longer the screen's form. */
int present_scan_rows(const present_state *s, uint8_t *rows, int32_t *bands, int max);
/* ... only the rows in the n bands in (the rows written). */
int present_scan_bands(const present_state *s, uint8_t *rows, const int32_t *in, int n, int32_t *bands, int max);

/* The rows [y0, y1) of the page shown written since *seen (dirty.c), as
 * bands (pairs, at most max, the last widened past that); *seen brought
 * up to date (0 at first).  The number of bands, PRESENT_DIRTY_ALL when
 * everything is to be taken as changed, PRESENT_DIRTY_UNKNOWN when writes
 * aren't tracked (BLUEBOX_DIRTY=0, or not yet): compare instead. */
enum { PRESENT_DIRTY_ALL = -1, PRESENT_DIRTY_UNKNOWN = -2 };
int present_dirty_rows(uint64_t *seen, int y0, int y1, int32_t *bands, int max);
/* Rows [y0, y1) added to bands as above (n of them so far). */
void present_add_band(int32_t *bands, int *n, int max, int y0, int y1);

/* ---- The presenter (a host's GPU) ---- */

/* Started (once): false when there is none (no device, BLUEBOX_METAL=0),
 * and the host converts on the CPU. */
bool present_start(void);
/* How the screen reaches the GPU: "zero-copy" (the GPU reads guest memory
 * in place), "shared" (the changed rows copied into memory the GPU
 * shares), "managed" (copied and uploaded: a GPU with its own memory),
 * or "cpu" (no presenter). */
const char *present_path(void);
/* Tests: the path to use from the next frame ("zero-copy", "shared",
 * "managed"); false if the device can't. */
bool present_force_path(const char *path);

typedef struct present_surface present_surface;
/* A surface drawing into layer (a CAMetalLayer on macOS). */
present_surface *present_surface_new(void *layer);
void present_surface_free(present_surface *s);
/* The Mac screen's rectangle it shows (in pixels, x y its top left),
 * stretched over the layer's pixels (w x h). */
void present_surface_set(present_surface *s, double x, double y, double w, double h, int pixels_w, int pixels_h);
/* Clear outside the shape: w*h bytes, 0 outside, at the rectangle's top
 * left (NULL: none). */
void present_surface_set_shape(present_surface *s, const uint8_t *mask, int w, int h);
/* Clear where the program's desktop shows (machine_desktop_mask). */
void present_surface_set_desktop(present_surface *s, bool leave_out);
/* Shown on the display (an occluded or minimized window isn't drawn). */
void present_surface_set_visible(present_surface *s, bool visible);
/* Drawn now on the calling thread: its window was just moved or resized. */
void present_surface_draw_now(present_surface *s);

/* A refresh of the display, from the host's display link: hz its rate. */
void present_vsync(double hz);
/* A wake-up at the next refresh asked for (the host's own work changed
 * something, as a drag). */
void present_wake(void);
/* The refreshes come from a display link of the presenter's own (macOS 13:
 * CVDisplayLink on that display); false if there is none. */
bool present_own_display_link(uint32_t display_id);
void present_stop_display_link(void);

/* Tests and benchmarks: the rectangle drawn into an offscreen target of
 * out_w x out_h, read back as RGBA (premultiplied, as the layer has it). */
bool present_offscreen(double x, double y, double w, double h, int out_w, int out_h, const uint8_t *shape, bool desktop,
                       uint8_t *rgba);

/* ---- Frame pacing on the display's refreshes ---- */

/* Refreshes are coming and a surface is shown. */
bool present_live(void);
/* Refreshes begun, and whether refresh seq's read of the screen is done. */
uint64_t present_vsync_seq(void);
bool present_read_done(uint64_t seq);
/* How long the emulator waited for a refresh, for the statistics. */
void present_note_hold(uint64_t ns);

/* ---- Guest memory moving under the presenter ---- */

/* The host pages [lo, lo + len) are about to be unmapped or mapped again
 * (mem.c), or the screen's page moved (qd_screen.c): the GPU's view of
 * them is dropped, its reads finished first. */
void present_memory_changing(void *lo, size_t len);

/* ---- Statistics: BLUEBOX_PRESENT_STATS=1, once a second ---- */

bool present_stats_on(void);
/* Main-thread time spent presenting a frame (any path). */
void present_stats_main(uint64_t ns);
/* A frame converted on the CPU: rows converted. */
void present_stats_cpu_frame(int rows, int width);
/* Logged when a second has passed (call from the host's tick). */
void present_stats_tick(void);
uint64_t present_now_ns(void);
/* Benchmarks: the presenter thread's processor time so far. */
uint64_t present_cpu_ns(void);

#endif
