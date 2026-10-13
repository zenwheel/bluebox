/*
 * Thread Manager (ThreadDispatch, $ABF2): cooperative threads.
 *
 * Each thread has a stack of its own in the application heap and, while it
 * is not running, its registers saved here.  A thread gives up the
 * processor only in a Thread Manager call (YieldToThread, SetThreadState,
 * DisposeThread, or returning from its entry routine); the switch happens as
 * that call returns, once its glue has finished with the stack, so a saved
 * thread's A7 always points at the return address of the call it made.
 *
 * Preemptive threads are scheduled as cooperative ones: they run when
 * another thread yields, and not at interrupt time.  A custom scheduler set
 * with SetThreadScheduler is recorded but not consulted.
 */
#include <string.h>

#include "tb_internal.h"
#include "trap.h"

enum { kNoThreadID = 0, kCurrentThreadID = 1, kApplicationThreadID = 2 };
enum { kReadyThreadState = 0, kStoppedThreadState = 1, kRunningThreadState = 2 };
enum { kCooperativeThread = 1, kPreemptiveThread = 2 };
enum { kNewSuspend = 1, kUsePremadeThread = 2, kCreateIfNeeded = 4 };
enum { threadTooManyReqsErr = -617, threadNotFoundErr = -618, threadProtocolErr = -619 };

#define MAX_THREADS 64
#define DEFAULT_STACK 0x6000u

typedef struct {
    bool used;
    uint32_t id;
    int16_t state;
    int32_t style;
    gaddr stack;   /* NewPtr'd stack (0 for the application's) */
    uint32_t stack_size;
    gaddr result;  /* where the entry routine's result goes */
    gaddr terminator, term_param;
    gaddr switch_in, switch_in_param, switch_out, switch_out_param;
    uint32_t d[8], a[8];
    uint16_t sr;
} thread;

static thread threads[MAX_THREADS];
static int current; /* index of the running thread */
static int pending = -1; /* the thread to switch to as the call returns */
static uint32_t next_id;
static int pool[3]; /* premade threads by style */
static gaddr exit_stub;

static void thread_exit_native(cpu68k *c, uint16_t tw);

void threads_reset(void)
{
    memset(threads, 0, sizeof threads);
    threads[0] = (thread){ .used = true, .id = kApplicationThreadID, .state = kRunningThreadState,
                           .style = kCooperativeThread };
    current = 0;
    pending = -1;
    next_id = kApplicationThreadID + 1;
    memset(pool, 0, sizeof pool);
    exit_stub = native_register(thread_exit_native, "ThreadExit", 0); /* natives start afresh at each boot */
}

static int find(int32_t id)
{
    if (id == kCurrentThreadID)
        return current;
    for (int i = 0; i < MAX_THREADS; i++)
        if (threads[i].used && threads[i].id == (uint32_t)id)
            return i;
    return -1;
}

/* The next ready thread after the current one, round robin, or -1. */
static int next_ready(void)
{
    for (int k = 1; k <= MAX_THREADS; k++) {
        int i = (current + k) % MAX_THREADS;
        if (i != current && threads[i].used && threads[i].state == kReadyThreadState)
            return i;
    }
    return -1;
}

static void schedule(int to)
{
    if (to >= 0 && to != current)
        pending = to;
}

/* Call a switcher or terminator: pascal (ThreadID, void *). */
static void call_proc(gaddr fn, uint32_t id, gaddr param)
{
    if (!fn)
        return;
    cpu68k *c = g_cpu;
    cpu_push32(c, id);
    cpu_push32(c, param);
    guest_call(fn);
}

/* At the end of a Thread Manager call (and of a thread): switch threads. */
void threads_switch_if_pending(cpu68k *c)
{
    if (pending < 0 || c->halted)
        return;
    int to = pending;
    pending = -1;
    thread *from = &threads[current];
    if (from->used) {
        call_proc(from->switch_out, from->id, from->switch_out_param);
        memcpy(from->d, c->d, sizeof from->d);
        memcpy(from->a, c->a, sizeof from->a);
        from->sr = c->sr;
        if (from->state == kRunningThreadState)
            from->state = kReadyThreadState;
    }
    current = to;
    thread *t = &threads[to];
    t->state = kRunningThreadState;
    memcpy(c->d, t->d, sizeof t->d);
    memcpy(c->a, t->a, sizeof t->a);
    c->sr = t->sr;
    call_proc(t->switch_in, t->id, t->switch_in_param);
}

static void finish_thread(int i, uint32_t result_value, bool store)
{
    thread *t = &threads[i];
    if (store && t->result)
        wr32(t->result, result_value);
    call_proc(t->terminator, t->id, t->term_param);
    gaddr stack = t->stack;
    t->used = false;
    t->state = kStoppedThreadState;
    if (stack)
        tb_DisposePtr(stack); /* not touched again: the switch replaces A7 */
}

/* A thread's entry routine returned here: its result is on the stack. */
static void thread_exit_native(cpu68k *c, uint16_t tw)
{
    int i = current;
    finish_thread(i, rd32(c->a[7]), true);
    int to = next_ready();
    if (to < 0)
        to = 0; /* the application thread is always there */
    threads[0].state = threads[0].state == kStoppedThreadState && to == 0 ? kReadyThreadState : threads[0].state;
    pending = to;
    threads_switch_if_pending(c);
}

/* ---------------------------------------------------------------------- */

int16_t tb_CreateThreadPool(int32_t style, int16_t n, int32_t stack_size)
{
    if (style != kCooperativeThread && style != kPreemptiveThread)
        return paramErr;
    pool[style] += n;
    return noErr;
}

int16_t tb_GetFreeThreadCount(int32_t style, gaddr countp)
{
    if (countp)
        wr16(countp, (uint16_t)(style == kCooperativeThread || style == kPreemptiveThread ? pool[style] : 0));
    return noErr;
}

int16_t tb_GetSpecificFreeThreadCount(int32_t style, int32_t stack_size, gaddr countp)
{
    return tb_GetFreeThreadCount(style, countp);
}

int16_t tb_GetDefaultThreadStackSize(int32_t style, gaddr sizep)
{
    if (sizep)
        wr32(sizep, DEFAULT_STACK);
    return noErr;
}

int16_t tb_ThreadCurrentStackSpace(int32_t id, gaddr freep)
{
    int i = find(id);
    if (i < 0)
        return threadNotFoundErr;
    uint32_t sp = i == current ? g_cpu->a[7] : threads[i].a[7];
    uint32_t space = threads[i].stack ? sp - threads[i].stack : (uint32_t)tb_StackSpace();
    if (freep)
        wr32(freep, space);
    return noErr;
}

int16_t tb_NewThread(int32_t style, gaddr entry, gaddr param, int32_t stack_size, int32_t options, gaddr resultp,
                     gaddr madep)
{
    if (madep)
        wr32(madep, kNoThreadID);
    if ((style != kCooperativeThread && style != kPreemptiveThread) || !entry)
        return paramErr;
    if (options & kUsePremadeThread) {
        if (pool[style] > 0)
            pool[style]--;
        else if (!(options & kCreateIfNeeded))
            return threadTooManyReqsErr;
    }
    int i = 1;
    while (i < MAX_THREADS && threads[i].used)
        i++;
    if (i == MAX_THREADS)
        return threadTooManyReqsErr;
    uint32_t size = stack_size > 0 ? (uint32_t)stack_size : DEFAULT_STACK;
    gaddr stack = tb_NewPtr((int32_t)size, false, false);
    if (!stack)
        return memFullErr;
    cpu68k *c = g_cpu;
    thread *t = &threads[i];
    *t = (thread){ .used = true, .id = next_id++, .style = style, .stack = stack, .stack_size = size,
                   .result = resultp, .state = (options & kNewSuspend) ? kStoppedThreadState : kReadyThreadState };
    /* Its first switch returns into the entry routine, called as pascal
     * void *entry(void *param), which returns to the exit stub. */
    gaddr sp = (stack + size) & ~3u;
    sp -= 4; wr32(sp, 0);         /* the function result */
    sp -= 4; wr32(sp, param);
    sp -= 4; wr32(sp, exit_stub); /* entry's return address */
    sp -= 4; wr32(sp, entry);     /* where the switch "returns" */
    memcpy(t->a, c->a, sizeof t->a); /* A5 and the rest as the creator's */
    t->a[7] = sp;
    t->sr = c->sr;
    if (madep)
        wr32(madep, t->id);
    return noErr;
}

int16_t tb_SetThreadScheduler(gaddr proc) { return noErr; }

int16_t tb_SetThreadSwitcher(int32_t id, gaddr proc, gaddr param, int8_t in)
{
    int i = find(id);
    if (i < 0)
        return threadNotFoundErr;
    if (in) {
        threads[i].switch_in = proc;
        threads[i].switch_in_param = param;
    } else {
        threads[i].switch_out = proc;
        threads[i].switch_out_param = param;
    }
    return noErr;
}

int16_t tb_SetThreadTerminator(int32_t id, gaddr proc, gaddr param)
{
    int i = find(id);
    if (i < 0)
        return threadNotFoundErr;
    threads[i].terminator = proc;
    threads[i].term_param = param;
    return noErr;
}

int16_t tb_SetDebuggerNotificationProcs(gaddr a, gaddr b, gaddr c) { return noErr; }

int16_t tb_DisposeThread(int32_t id, gaddr result, int8_t recycle)
{
    int i = find(id);
    if (i < 0)
        return threadNotFoundErr;
    if (i == 0)
        return threadProtocolErr; /* not the application thread */
    int style = threads[i].style;
    finish_thread(i, result, true);
    if (recycle && (style == kCooperativeThread || style == kPreemptiveThread))
        pool[style]++;
    if (i == current) { /* a thread ending itself does not return */
        int to = next_ready();
        if (to < 0)
            to = 0;
        threads[0].state = threads[0].state == kStoppedThreadState && to == 0 ? kReadyThreadState : threads[0].state;
        pending = to;
    }
    return noErr;
}

int16_t tb_YieldToThread(int32_t suggested)
{
    int i = suggested == kNoThreadID ? -1 : find(suggested);
    if (i >= 0 && i != current && threads[i].state == kReadyThreadState)
        schedule(i);
    else
        schedule(next_ready());
    return noErr;
}

int16_t tb_GetCurrentThread(gaddr idp)
{
    if (idp)
        wr32(idp, threads[current].id);
    return noErr;
}

int16_t tb_GetThreadState(int32_t id, gaddr statep)
{
    int i = find(id);
    if (i < 0)
        return threadNotFoundErr;
    if (statep)
        wr16(statep, (uint16_t)threads[i].state);
    return noErr;
}

int16_t tb_SetThreadState(int32_t id, int16_t state, int32_t suggested)
{
    int i = find(id);
    if (i < 0)
        return threadNotFoundErr;
    if (state != kReadyThreadState && state != kStoppedThreadState && state != kRunningThreadState)
        return paramErr;
    if (i != current) {
        if (state == kRunningThreadState) { /* run it now */
            threads[i].state = kReadyThreadState;
            schedule(i);
        } else {
            threads[i].state = state;
        }
        return noErr;
    }
    if (state == kStoppedThreadState) {
        int to = suggested == kNoThreadID ? -1 : find(suggested);
        if (to < 0 || to == current || threads[to].state != kReadyThreadState)
            to = next_ready();
        if (to < 0)
            return threadProtocolErr; /* nothing else could run */
        threads[current].state = kStoppedThreadState;
        schedule(to);
    }
    return noErr;
}

int16_t tb_SetThreadStateEndCritical(int32_t id, int16_t state, int32_t suggested)
{
    return tb_SetThreadState(id, state, suggested);
}

int16_t tb_ThreadBeginCritical(void) { return noErr; }
int16_t tb_ThreadEndCritical(void) { return noErr; }

int16_t tb_GetThreadCurrentTaskRef(gaddr refp)
{
    if (refp)
        wr32(refp, 1); /* one application */
    return noErr;
}

int16_t tb_GetThreadStateGivenTaskRef(int32_t ref, int32_t id, gaddr statep) { return tb_GetThreadState(id, statep); }

int16_t tb_SetThreadReadyGivenTaskRef(int32_t ref, int32_t id)
{
    int i = find(id);
    if (i < 0)
        return threadNotFoundErr;
    if (threads[i].state == kStoppedThreadState)
        threads[i].state = kReadyThreadState;
    return noErr;
}
