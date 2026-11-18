/*
 * Between present.c and a host's GPU presenter (present_metal.m).
 */
#ifndef PRESENT_BACKEND_H
#define PRESENT_BACKEND_H

#include <stdbool.h>
#include <stdint.h>

/* A refresh begun (its number), and its read of the screen done: a
 * program waiting for it goes on. */
uint64_t present_vsync_begin(void);
void present_vsync_read(uint64_t seq);
/* Whether any surface is shown. */
void present_set_shown(bool any);

/* A refresh handled: whether the screen was compared, or the comparison
 * skipped (nothing ran since), the surfaces drawn, rows and bytes that
 * went up, and the presenter's time. */
void present_stats_gpu(bool scanned, bool skipped, int frames, int rows, uint64_t bytes, uint64_t render_ns);
/* The GPU's read of the screen in place: from commit to done. */
void present_stats_read(uint64_t ns);

/* The emulator's progress: what it ran, for skipping the comparison of
 * an unchanged screen. */
uint64_t present_machine_progress(void);

#endif
