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
#include "legacy.h"
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
bool time_in_interrupt(void) { return in_interrupt; }

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
    /* tmCount gets the time left: in negated microseconds when it fits,
     * otherwise in positive milliseconds, as the Time Manager kept it.
     * A Director 4 projector (Total Distortion) times its frames this
     * way, priming a task for 16777215 ms and taking what is left from
     * that: clamped to 2147 seconds of microseconds, every frame looked
     * hours long and the score raced past its tempo settings. */
    int32_t left = 0;
    if (timers[i].active) {
        uint64_t t = now();
        uint64_t us = timers[i].due > t ? (timers[i].due - t) / 1000u : 0;
        if (us <= 0x7FFFFFFF)
            left = -(int32_t)us;
        else
            left = us / 1000u > 0x7FFFFFFF ? 0x7FFFFFFF : (int32_t)(us / 1000u);
    }
    wr32(taskp + TMTask_tmCount_off, (uint32_t)left);
    wr16(taskp + TMTask_qType_off, rd16(taskp + TMTask_qType_off) & (uint16_t)~TM_ACTIVE);
    memset(&timers[i], 0, sizeof timers[i]);
}

/* The VBL queue in low memory (VBLQueue: qFlags, qHead, qTail), which
 * programs walk to find their tasks: the installed tasks, linked through
 * their qLink fields in the order they run. */
static gaddr system_vbl; /* stands for the system's own tasks, ahead of any program's */

static void link_vbl_queue(void)
{
    gaddr head = system_vbl, prev = system_vbl;
    if (system_vbl)
        wr32(system_vbl, 0);
    for (int i = 0; i < MAX_TASKS; i++) {
        gaddr v = vbls[i];
        if (!v)
            continue;
        wr32(v, 0);
        if (prev)
            wr32(prev, v);
        else
            head = v;
        prev = v;
    }
    wr16(LM_VBLQueue, 0);
    wr32(LM_VBLQueue + 2, head);
    wr32(LM_VBLQueue + 6, prev);
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
            link_vbl_queue();
            return noErr;
        }
    return -2;
}

int16_t tb_VRemove(gaddr vtaskp)
{
    for (int i = 0; i < MAX_TASKS; i++)
        if (vbls[i] == vtaskp) {
            vbls[i] = 0;
            link_vbl_queue();
            return noErr;
        }
    return -1; /* qErr for a task not in the queue */
}

int16_t tb_SlotVInstall(gaddr vtaskp, int16_t slot) { return tb_VInstall(vtaskp); }
int16_t tb_SlotVRemove(gaddr vtaskp, int16_t slot) { return tb_VRemove(vtaskp); }

/* Run guest code as an interrupt handler would: everything preserved. */
static void interrupt_call_d0(gaddr fn, gaddr a0, gaddr a1, uint32_t d0)
{
    cpu68k *c = g_cpu;
    uint32_t d[8], a[8];
    memcpy(d, c->d, sizeof d);
    memcpy(a, c->a, sizeof a);
    uint16_t sr = c->sr;
    c->d[0] = d0;
    c->a[0] = a0;
    c->a[1] = a1;
    c->sr = (uint16_t)((c->sr & ~0x0700) | 0x0100); /* level 1, as the VIA interrupt */
    guest_call(fn);
    memcpy(c->d, d, sizeof d);
    memcpy(c->a, a, sizeof a);
    c->sr = sr;
}

static void interrupt_call(gaddr fn, gaddr a0, gaddr a1) { interrupt_call_d0(fn, a0, a1, 0); }

/* Completion routines of asynchronous File and Device Manager calls: our
 * calls finish at once, and their completions run soon after, at interrupt
 * level, with the parameter block in A0 and the result in D0, as when the
 * I/O finished. Not from inside the call: a completion that queues the
 * next read would otherwise recurse. */
enum { MAX_COMPLETIONS = 64 };
typedef struct {
    gaddr pb;
    int16_t result;
} completion;
static completion completions[MAX_COMPLETIONS];
static int ncompletions;

void io_complete_later(gaddr pb, int16_t result)
{
    if (ncompletions < MAX_COMPLETIONS)
        completions[ncompletions++] = (completion){ pb, result };
}

/* Deferred tasks (DTInstall): run once at the next interrupt pump. */
static gaddr deferred[MAX_COMPLETIONS];
static int ndeferred;

int16_t tb_DTInstall(gaddr task)
{
    if (!task || rd16(task + 4) != 7) /* dtQType */
        return -1; /* vTypErr */
    if (ndeferred >= MAX_COMPLETIONS)
        return -1;
    deferred[ndeferred++] = task;
    return noErr;
}

static void run_deferred(void)
{
    int n = ndeferred;
    gaddr due[MAX_COMPLETIONS];
    memcpy(due, deferred, sizeof due[0] * (size_t)n);
    ndeferred = 0;
    for (int i = 0; i < n; i++) {
        gaddr fn = rd32(due[i] + 8);
        if (fn)
            interrupt_call(fn, due[i], rd32(due[i] + 12));
    }
}

/* A request killed (KillIO): its completion runs now, inside the call, as
 * the Device Manager finished killed requests, with any still waiting for
 * the same parameter block first.  The program may dispose of the block
 * as soon as KillIO returns (Continuum and The Colony do, then reuse the
 * memory), so later would read a stale ioCompletion. */
/* KillIO on a driver: the completions of its requests that finished but
 * have not run yet run now, for the same reason. */
void io_complete_now_for(int16_t refnum)
{
    for (int i = 0; i < ncompletions;) {
        gaddr pb = completions[i].pb;
        if ((int16_t)rd16(pb + 24) == refnum) { /* ioRefNum */
            int16_t r = completions[i].result;
            memmove(completions + i, completions + i + 1, sizeof completions[0] * (size_t)(--ncompletions - i));
            gaddr fn = rd32(pb + 12);
            if (fn)
                interrupt_call_d0(fn, pb, 0, (uint32_t)(int32_t)r);
            i = 0; /* a completion may have queued more */
        } else {
            i++;
        }
    }
}

void io_complete_now(gaddr pb, int16_t result)
{
    for (int i = 0; i < ncompletions;) {
        if (completions[i].pb == pb) {
            int16_t r = completions[i].result;
            memmove(completions + i, completions + i + 1, sizeof completions[0] * (size_t)(--ncompletions - i));
            gaddr fn = rd32(pb + 12);
            if (fn)
                interrupt_call_d0(fn, pb, 0, (uint32_t)(int32_t)r);
        } else {
            i++;
        }
    }
    gaddr fn = rd32(pb + 12); /* ioCompletion */
    if (fn)
        interrupt_call_d0(fn, pb, 0, (uint32_t)(int32_t)result);
}

static void run_completions(void)
{
    /* Those a completion queues wait for the next pump. */
    int n = ncompletions;
    completion due[MAX_COMPLETIONS];
    memcpy(due, completions, sizeof due[0] * (size_t)n);
    ncompletions = 0;
    for (int i = 0; i < n; i++) {
        gaddr fn = rd32(due[i].pb + 12); /* ioCompletion */
        if (fn)
            interrupt_call_d0(fn, due[i].pb, 0, (uint32_t)(int32_t)due[i].result);
    }
}

/* One tick's VBL tasks: each count down, those reaching zero run. */
static void vbl_tick(void)
{
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

/* Whether a Time Manager or VBL task is waiting to run. */
bool time_busy(void)
{
    for (int i = 0; i < MAX_TASKS; i++)
        if ((timers[i].tm && timers[i].active) || vbls[i])
            return true;
    return false;
}

void time_pump(void)
{
    cpu68k *c = g_cpu;
    if (in_interrupt || !c || c->halted || (c->sr & 0x0700))
        return;
    in_interrupt = true;
    run_completions();
    run_deferred();
    sound_pump(); /* sound completion ran at interrupt time too */
    midi_pump();
    opentransport_poll(); /* Open Transport's notifiers, at deferred task time */
    ot_tasks_pump();      /* and its timer, deferred and system tasks */
    mactcp_poll();        /* MacTCP's calls in progress and its notification routines */
    serial_poll();        /* serial reads waiting for their bytes */
    ctb_poll();           /* Communications Toolbox connections and their completion routines */
    appletalk_poll();     /* datagrams for socket listeners, NBP's answers */
    ppc_toolbox_poll();   /* PPC Toolbox sessions: requests, answers and blocks */
    adb_pump();
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
    while (n--)
        vbl_tick();
    in_interrupt = false;
}

/* DoVBLTask: a slot's retrace, its tasks run as on a tick (all are the
 * built-in video's: SlotVInstall is VInstall's). */
void time_vbl_task(void)
{
    if (in_interrupt)
        return;
    in_interrupt = true;
    vbl_tick();
    in_interrupt = false;
}

void time_reset(void)
{
    midi_reset();
    memset(timers, 0, sizeof timers);
    memset(vbls, 0, sizeof vbls);
    /* A real Mac's queue always holds system tasks (the cursor's, ADB's), so
     * programs that find theirs by its predecessor's link find it.  Ours
     * waits for ever (vblCount 0) and would only return. */
    system_vbl = mm_new_ptr_in(rd32(LM_SysZone), VBLTask_SIZEOF + 2, true);
    if (system_vbl) {
        wr16(system_vbl + 4, 1); /* qType: vType */
        wr16(system_vbl + VBLTask_SIZEOF, 0x4E75); /* RTS */
        wr32(system_vbl + VBLTask_vblAddr_off, system_vbl + VBLTask_SIZEOF);
    }
    link_vbl_queue();
    vbl_ticks = 0;
    ncompletions = 0;
    ndeferred = 0;
    in_interrupt = false;
}
