/*
 * Time Manager and Vertical Retrace Manager tasks.
 *
 * Both ran at interrupt time.  Here they run between slices of emulation
 * (every few thousand instructions) and at the Event Manager's safe points,
 * unless the application has raised the interrupt mask.  A task runs with
 * all registers and the status register preserved around it, as an
 * interrupt handler would.
 *
 * Time Manager tasks get A1 = the task; a primed task runs once when its
 * time is up.  Extended tasks (InsXTime) are re-primed from their last
 * wake-up time rather than from now, so periodic tasks don't drift.
 * VBL tasks run at 60 Hz: each tick decrements vblCount and the task runs
 * when it reaches zero, getting A0 = the task.
 */
#include <string.h>
#include <time.h>

#include "m68k.h"
#include "tb_internal.h"

#define MAX_TASKS 64
#define TM_ACTIVE 0x8000
#define TM_EXTENDED 0x4000

static struct {
    gaddr tm;
    bool active;
    uint64_t due;  /* host ns */
    uint64_t wake; /* last wake-up time, for extended tasks */
} timers[MAX_TASKS];

static gaddr vbls[MAX_TASKS];
static uint32_t vbl_ticks; /* Ticks when VBL tasks last ran */
static bool in_interrupt;

static uint64_t now(void) { return machine_time_ns(); }

static int find_timer(gaddr tm)
{
    for (int i = 0; i < MAX_TASKS; i++)
        if (timers[i].tm == tm)
            return i;
    return -1;
}

static void ins_time(gaddr tm, bool extended)
{
    if (!tm || find_timer(tm) >= 0)
        return;
    for (int i = 0; i < MAX_TASKS; i++)
        if (!timers[i].tm) {
            timers[i].tm = tm;
            timers[i].active = false;
            timers[i].wake = 0;
            uint16_t type = rd16(tm + TMTask_qType_off) & (uint16_t)~TM_ACTIVE;
            if (extended)
                type |= TM_EXTENDED;
            wr16(tm + TMTask_qType_off, type);
            if (extended)
                wr32(tm + TMTask_tmWakeUp_off, 0);
            return;
        }
}

void tb_InsTime(gaddr taskp) { ins_time(taskp, false); }
void tb_InsXTime(gaddr taskp) { ins_time(taskp, true); }

/* count > 0: milliseconds; count < 0: microseconds, negated. */
static uint64_t count_ns(int32_t count)
{
    return count >= 0 ? (uint64_t)count * 1000000u : (uint64_t)(-(int64_t)count) * 1000u;
}

void tb_PrimeTime(gaddr taskp, int32_t count)
{
    int i = find_timer(taskp);
    if (i < 0)
        return;
    uint64_t base = now();
    bool extended = rd16(taskp + TMTask_qType_off) & TM_EXTENDED;
    if (extended && rd32(taskp + TMTask_tmWakeUp_off) && timers[i].wake)
        base = timers[i].wake;
    timers[i].due = base + count_ns(count);
    timers[i].active = true;
    wr16(taskp + TMTask_qType_off, rd16(taskp + TMTask_qType_off) | TM_ACTIVE);
}

void tb_RmvTime(gaddr taskp)
{
    int i = find_timer(taskp);
    if (i < 0)
        return;
    /* tmCount gets the time left, in negated microseconds. */
    int32_t left = 0;
    if (timers[i].active) {
        uint64_t t = now();
        uint64_t us = timers[i].due > t ? (timers[i].due - t) / 1000u : 0;
        left = us > 0x7FFFFFFF ? -0x7FFFFFFF : -(int32_t)us;
    }
    wr32(taskp + TMTask_tmCount_off, (uint32_t)left);
    wr16(taskp + TMTask_qType_off, rd16(taskp + TMTask_qType_off) & (uint16_t)~TM_ACTIVE);
    memset(&timers[i], 0, sizeof timers[i]);
}

int16_t tb_VInstall(gaddr vtaskp)
{
    if (!vtaskp)
        return -2; /* qErr */
    for (int i = 0; i < MAX_TASKS; i++)
        if (vbls[i] == vtaskp)
            return noErr;
    for (int i = 0; i < MAX_TASKS; i++)
        if (!vbls[i]) {
            vbls[i] = vtaskp;
            return noErr;
        }
    return -2;
}

int16_t tb_VRemove(gaddr vtaskp)
{
    for (int i = 0; i < MAX_TASKS; i++)
        if (vbls[i] == vtaskp) {
            vbls[i] = 0;
            return noErr;
        }
    return -1; /* qErr for a task not in the queue */
}

int16_t tb_SlotVInstall(gaddr vtaskp, int16_t slot) { return tb_VInstall(vtaskp); }
int16_t tb_SlotVRemove(gaddr vtaskp, int16_t slot) { return tb_VRemove(vtaskp); }

/* Run guest code as an interrupt handler would: everything preserved. */
static void interrupt_call(gaddr fn, gaddr a0, gaddr a1)
{
    cpu68k *c = g_cpu;
    uint32_t d[8], a[8];
    memcpy(d, c->d, sizeof d);
    memcpy(a, c->a, sizeof a);
    uint16_t sr = c->sr;
    c->a[0] = a0;
    c->a[1] = a1;
    c->sr = (uint16_t)((c->sr & ~0x0700) | 0x0100); /* level 1, as the VIA interrupt */
    guest_call(fn);
    memcpy(c->d, d, sizeof d);
    memcpy(c->a, a, sizeof a);
    c->sr = sr;
}

void time_pump(void)
{
    cpu68k *c = g_cpu;
    if (in_interrupt || !c || c->halted || (c->sr & 0x0700))
        return;
    in_interrupt = true;
    sound_pump(); /* sound completion ran at interrupt time too */
    uint64_t t = now();
    for (int i = 0; i < MAX_TASKS; i++) {
        if (!timers[i].tm || !timers[i].active || timers[i].due > t)
            continue;
        gaddr tm = timers[i].tm;
        timers[i].active = false;
        timers[i].wake = timers[i].due;
        wr16(tm + TMTask_qType_off, rd16(tm + TMTask_qType_off) & (uint16_t)~TM_ACTIVE);
        if (rd16(tm + TMTask_qType_off) & TM_EXTENDED)
            wr32(tm + TMTask_tmWakeUp_off, (uint32_t)(timers[i].due / 1000000u) | 1);
        gaddr fn = rd32(tm + TMTask_tmAddr_off);
        if (fn)
            interrupt_call(fn, fn, tm);
    }
    /* VBL tasks: once per elapsed tick, at most a few ticks' worth at once. */
    uint32_t ticks = rd32(LM_Ticks);
    uint32_t n = ticks - vbl_ticks;
    if (n > 4)
        n = 4;
    vbl_ticks = ticks;
    while (n--) {
        for (int i = 0; i < MAX_TASKS; i++) {
            gaddr v = vbls[i];
            if (!v)
                continue;
            int16_t count = (int16_t)rd16(v + VBLTask_vblCount_off);
            if (count <= 0)
                continue;
            wr16(v + VBLTask_vblCount_off, (uint16_t)(count - 1));
            if (count == 1)
                interrupt_call(rd32(v + VBLTask_vblAddr_off), v, 0);
        }
    }
    in_interrupt = false;
}

void time_reset(void)
{
    memset(timers, 0, sizeof timers);
    memset(vbls, 0, sizeof vbls);
    vbl_ticks = 0;
    in_interrupt = false;
}
