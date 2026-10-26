/*
 * Thread Manager (ThreadDispatch, $ABF2): cooperative threads.
 *
 * Each thread runs on a host thread of its own, and only one runs at a
 * time: a Thread Manager call that switches threads (YieldToThread,
 * SetThreadState, DisposeThread, or the entry routine returning) saves the
 * running thread's registers and nesting (calls into guest code, traps run
 * for PowerPC code), loads the next thread's, hands it the baton and waits
 * for its own turn.  So a thread may switch from any depth: inside 68k or
 * PowerPC code, inside a callback, inside a trap a PowerPC program called.
 *
 * Each thread has a stack of its own in the application heap: the 68k
 * part at its top and, for PowerPC programs, the PowerPC part below.
 * Preemptive threads are scheduled as cooperative ones: they run when
 * another thread yields, and not at interrupt time.  A custom scheduler set
 * with SetThreadScheduler is recorded but not consulted.
 */
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

#include "carbon_events.h"
#include "machine.h"
#include "ppc.h"
#include "tb_internal.h"
#include "trap.h"

enum { kNoThreadID = 0, kCurrentThreadID = 1, kApplicationThreadID = 2 };
enum { kReadyThreadState = 0, kStoppedThreadState = 1, kRunningThreadState = 2 };
enum { kCooperativeThread = 1, kPreemptiveThread = 2 };
enum { kNewSuspend = 1, kUsePremadeThread = 2, kCreateIfNeeded = 4 };
enum { threadTooManyReqsErr = -617, threadNotFoundErr = -618, threadProtocolErr = -619 };

#define MAX_THREADS 64
#define DEFAULT_STACK 0x6000u
#define STACK_68K 0x4000u          /* the 68k part of a PowerPC program's thread stack */
#define HOST_STACK (16u << 20)     /* emulation nests deeply on the host's stack */

/* What a thread keeps while another runs. */
typedef struct {
    cpu68k cpu;
    ppc_cpu ppc;
    trap_ctx trap;
    ppc_trap_ctx ppc_trap;
    bool ppc_returned;
} exec_ctx;

/* The host thread a guest thread runs on: it may go on only when told. */
typedef struct {
    pthread_t th;
    pthread_cond_t cv;
    bool run;
    bool started;
    jmp_buf top;   /* where a thread that disposes of itself leaves from */
} host_thread;

typedef struct {
    bool used;
    uint32_t id;
    int16_t state;
    int32_t style;
    gaddr stack;   /* NewPtr'd stack (0 for the application's) */
    uint32_t stack_size;
    gaddr entry, param;
    gaddr result;  /* where the entry routine's result goes */
    gaddr terminator, term_param;
    gaddr switch_in, switch_in_param, switch_out, switch_out_param;
    exec_ctx *ctx;
    host_thread *host;
    void (*exit_hook)(uint32_t id, uint32_t result); /* native code told it ended */
    uint32_t exit_to;                                /* the thread to run then */
} thread;

static thread threads[MAX_THREADS];
static int current; /* index of the running thread */
static uint32_t next_id;
static int pool[3]; /* premade threads by style */
static pthread_mutex_t baton = PTHREAD_MUTEX_INITIALIZER;
static host_thread app_host;
static void (*main_pump)(void);

void machine_set_main_thread_pump(void (*fn)(void)) { main_pump = fn; }

static void free_thread(thread *t)
{
    /* A host thread still waiting (its guest thread never finished) keeps
     * its record: it is never told to go on. */
    if (t->ctx)
        free(t->ctx);
    t->ctx = NULL;
}

void threads_reset(void)
{
    for (int i = 1; i < MAX_THREADS; i++)
        free_thread(&threads[i]);
    memset(threads, 0, sizeof threads);
    if (!app_host.started) {
        pthread_cond_init(&app_host.cv, NULL);
        app_host.started = true;
    }
    app_host.run = true;
    threads[0] = (thread){ .used = true, .id = kApplicationThreadID, .state = kRunningThreadState,
                           .style = kCooperativeThread, .ctx = NULL, .host = &app_host };
    current = 0;
    next_id = kApplicationThreadID + 1;
    memset(pool, 0, sizeof pool);
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

/* PowerPC programs give ThreadsLib their routines as transition vectors;
 * routine descriptors and 68k programs' routines are 68k calls. */
static bool native_ppc(gaddr fn) { return g_ppc_mode && !mm_is_descriptor(fn); }

/* Call a switcher or terminator: pascal (ThreadID, void *). */
static void call_proc(gaddr fn, uint32_t id, gaddr param)
{
    if (!fn)
        return;
    if (native_ppc(fn)) {
        uint32_t args[2] = { id, param };
        ppc_call(fn, args, 2);
        return;
    }
    cpu68k *c = g_cpu;
    cpu_push32(c, id);
    cpu_push32(c, param);
    guest_call(fn);
}

/* ---- Saving and loading a thread's state ---- */

static void save_ctx(exec_ctx *x)
{
    x->cpu = *g_cpu;
    x->ppc = g_ppc;
    trap_ctx_save(&x->trap);
    ppc_trap_ctx_save(&x->ppc_trap);
    x->ppc_returned = mm_ctx_returned();
}

/* The registers and nesting; the instruction count, the machine's halt and
 * the hooks stay as they are. */
static void load_ctx(const exec_ctx *x)
{
    cpu68k *c = g_cpu;
    memcpy(c->d, x->cpu.d, sizeof c->d);
    memcpy(c->a, x->cpu.a, sizeof c->a);
    c->pc = x->cpu.pc;
    c->sr = x->cpu.sr;
    c->usp = x->cpu.usp;
    c->isp = x->cpu.isp;
    memcpy(c->fp, x->cpu.fp, sizeof c->fp);
    c->fpcr = x->cpu.fpcr;
    c->fpsr = x->cpu.fpsr;
    c->fpiar = x->cpu.fpiar;
    c->op_pc = x->cpu.op_pc;
    c->op = x->cpu.op;
    c->stopped = x->cpu.stopped;
    ppc_cpu *p = &g_ppc;
    memcpy(p->gpr, x->ppc.gpr, sizeof p->gpr);
    memcpy(p->fpr, x->ppc.fpr, sizeof p->fpr);
    p->cr = x->ppc.cr;
    p->lr = x->ppc.lr;
    p->ctr = x->ppc.ctr;
    p->xer = x->ppc.xer;
    p->fpscr = x->ppc.fpscr;
    p->pc = x->ppc.pc;
    p->op_pc = x->ppc.op_pc;
    p->prev_pc = x->ppc.prev_pc;
    p->op = x->ppc.op;
    p->reserve_addr = x->ppc.reserve_addr;
    p->reserved = x->ppc.reserved;
    p->halted = x->ppc.halted; /* a nested PowerPC call's own stop */
    memcpy(p->halt_reason, x->ppc.halt_reason, sizeof p->halt_reason);
    trap_ctx_load(&x->trap);
    ppc_trap_ctx_load(&x->ppc_trap);
    mm_ctx_set_returned(x->ppc_returned);
}

/* ---- Handing over ---- */

static void *host_main(void *arg);

/* Wait (holding the baton lock) until this host thread may go on.  The
 * process's main thread keeps serving the host meanwhile: host code that
 * a guest thread calls may wait for it. */
static bool on_main_thread(void)
{
#ifdef __APPLE__
    return pthread_main_np() != 0;
#else
    return false;
#endif
}

static void wait_turn(host_thread *h)
{
    bool serve = main_pump && on_main_thread();
    while (!h->run) {
        if (serve) {
            /* Pumped every 2 ms, but woken at once when its turn comes. */
            pthread_mutex_unlock(&baton);
            main_pump();
            pthread_mutex_lock(&baton);
            if (h->run)
                break;
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_nsec += 2000000;
            if (until.tv_nsec >= 1000000000) {
                until.tv_sec++;
                until.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&h->cv, &baton, &until);
        } else {
            pthread_cond_wait(&h->cv, &baton);
        }
    }
}

/* Run thread `to` instead of the current one.  Unless the current one is
 * leaving (it has finished), it waits here for its next turn. */
static void switch_to(int to, bool leaving)
{
    int from = current;
    thread *f = &threads[from], *t = &threads[to];
    host_thread *mine = f->host;
    if (to == from)
        return;
    if (!leaving) {
        call_proc(f->switch_out, f->id, f->switch_out_param);
        if (!f->ctx)
            f->ctx = calloc(1, sizeof *f->ctx);
        save_ctx(f->ctx);
        if (f->state == kRunningThreadState)
            f->state = kReadyThreadState;
    }
    current = to;
    t->state = kRunningThreadState;
    if (t->ctx)
        load_ctx(t->ctx);
    call_proc(t->switch_in, t->id, t->switch_in_param);

    pthread_mutex_lock(&baton);
    if (!leaving)
        mine->run = false;
    host_thread *h = t->host;
    h->run = true;
    if (!h->started) {
        h->started = true;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, HOST_STACK);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&h->th, &attr, host_main, t) != 0)
            cpu_halt(g_cpu, "Thread Manager: no host thread for thread %u", t->id);
        pthread_attr_destroy(&attr);
    } else {
        pthread_cond_signal(&h->cv);
    }
    if (!leaving)
        wait_turn(mine);
    pthread_mutex_unlock(&baton);
}

static void finish_thread(int i, uint32_t result_value, bool store)
{
    thread *t = &threads[i];
    if (store && t->result)
        wr32(t->result, result_value);
    call_proc(t->terminator, t->id, t->term_param);
    if (t->exit_hook)
        t->exit_hook(t->id, result_value);
    t->used = false;
    t->state = kStoppedThreadState;
}

/* After thread i has finished: the one it said to hand back to, the next
 * one, or the application's. */
static int successor(int i)
{
    if (g_cpu->halted)
        return 0; /* the machine is stopping: the application thread unwinds */
    int to = threads[i].exit_to ? find((int32_t)threads[i].exit_to) : -1;
    if (to >= 0 && to != i && threads[to].used && threads[to].state == kReadyThreadState)
        return to;
    to = next_ready();
    if (to < 0) {
        to = 0; /* the application thread is always there */
        if (threads[0].state == kStoppedThreadState)
            threads[0].state = kReadyThreadState;
    }
    return to;
}

/* A thread's host thread: its turn comes with its registers loaded (its
 * stack set up by NewThread); it calls the entry routine, as pascal
 * void *entry(void *param), and leaves when that returns. */
static void *host_main(void *arg)
{
    thread *t = arg;
    host_thread *h = t->host;
    pthread_mutex_lock(&baton);
    wait_turn(h);
    pthread_mutex_unlock(&baton);
    int i = (int)(t - threads);
    gaddr stack = t->stack;
    if (!setjmp(h->top)) {
        cpu68k *c = g_cpu;
        uint32_t result;
        if (native_ppc(t->entry)) {
            result = ppc_call(t->entry, &t->param, 1);
        } else {
            cpu_push32(c, 0); /* the result */
            cpu_push32(c, t->param);
            guest_call(t->entry);
            result = cpu_pop32(c);
        }
        if (!c->halted && threads[i].used)
            finish_thread(i, result, true);
    }
    /* Ended by its entry routine's return, by disposing of itself, or by
     * the machine stopping. */
    if (stack && !g_cpu->halted)
        tb_DisposePtr(stack);
    free_thread(&threads[i]);
    switch_to(successor(i), true);
    return NULL;
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

/* A thread's free stack: down to the bottom of its stack block, or of the
 * application's stack for the main thread.  A PowerPC program's stack is
 * r1's, below the 68k one in a thread's block (Chris' Puzzle fills what
 * is free with a pattern, from the bottom up: given the 68k stack's room,
 * it wrote over its own code). */
static uint32_t stack_space(int i)
{
    bool cur = i == current;
    if (g_ppc_mode) {
        uint32_t sp = cur ? g_ppc.gpr[1] : threads[i].ctx ? threads[i].ctx->ppc.gpr[1] : 0;
        gaddr base = threads[i].stack ? threads[i].stack : cfm_ppc_stack_lo;
        return sp > base ? sp - base : 0;
    }
    uint32_t sp = cur ? g_cpu->a[7] : threads[i].ctx ? threads[i].ctx->cpu.a[7] : 0;
    if (!threads[i].stack) {
        gaddr limit = rd32(LM_ApplLimit);
        return sp > limit ? sp - limit : 0;
    }
    return sp > threads[i].stack ? sp - threads[i].stack : 0;
}

uint32_t thread_stack_space(void) { return stack_space(current); }

int16_t tb_ThreadCurrentStackSpace(int32_t id, gaddr freep)
{
    int i = find(id);
    if (i < 0)
        return threadNotFoundErr;
    if (freep)
        wr32(freep, stack_space(i));
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
    if (g_ppc_mode)
        size += STACK_68K;
    gaddr stack = tb_NewPtr((int32_t)size, false, false);
    exec_ctx *x = calloc(1, sizeof *x);
    host_thread *h = calloc(1, sizeof *h);
    if (!stack || !x || !h) {
        if (stack)
            tb_DisposePtr(stack);
        free(x);
        free(h);
        return memFullErr;
    }
    pthread_cond_init(&h->cv, NULL);
    thread *t = &threads[i];
    *t = (thread){ .used = true, .id = next_id++, .style = style, .stack = stack, .stack_size = size,
                   .entry = entry, .param = param, .result = resultp, .ctx = x, .host = h,
                   .state = (options & kNewSuspend) ? kStoppedThreadState : kReadyThreadState };
    /* It starts with the creator's registers (A5 and the rest), its own
     * stacks, and nothing nested. */
    save_ctx(x);
    gaddr top = (stack + size) & ~15u;
    x->cpu.a[7] = top - 16;
    if (g_ppc_mode) {
        gaddr r1 = (top - STACK_68K - 64) & ~15u;
        wr32(r1, 0); /* the back chain ends here */
        x->ppc.gpr[1] = r1;
    }
    x->ppc.halted = false;
    memset(&x->trap, 0, sizeof x->trap);
    memset(&x->ppc_trap, 0, sizeof x->ppc_trap);
    x->ppc_returned = false;
    if (madep)
        wr32(madep, t->id);
    return noErr;
}

/* Multiprocessing Services' tasks run as threads (multiprocessing.c): its
 * code is told when one ends, and the thread to hand back to then. */
void thread_set_exit(uint32_t id, void (*hook)(uint32_t id, uint32_t result), uint32_t exit_to)
{
    int i = find((int32_t)id);
    if (i > 0) {
        threads[i].exit_hook = hook;
        threads[i].exit_to = exit_to;
    }
}

uint32_t thread_current_id(void) { return threads[current].id; }

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
    if (i == current)          /* a thread ending itself does not return: */
        longjmp(threads[i].host->top, 1); /* its host thread leaves */
    /* Another thread: its host thread waits for good. */
    if (threads[i].stack)
        tb_DisposePtr(threads[i].stack);
    free_thread(&threads[i]);
    return noErr;
}

int16_t tb_YieldToThread(int32_t suggested)
{
    int i = suggested == kNoThreadID ? -1 : find(suggested);
    if (!(i >= 0 && i != current && threads[i].state == kReadyThreadState))
        i = next_ready();
    if (i >= 0)
        switch_to(i, false);
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
            switch_to(i, false);
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
        switch_to(to, false);
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
