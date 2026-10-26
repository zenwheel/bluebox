/*
 * Multiprocessing Services (Multiprocessing.h, MPLibrary 2.x as CarbonLib
 * exports it) on a Mac with one processor.
 *
 * A task is a Thread Manager thread (threadmgr.c) run cooperatively: it
 * gets a turn when the program waits on a queue, a semaphore, an event
 * group or a critical region, yields (MPYield) or calls WaitNextEvent or
 * ReceiveNextEvent, and it runs until it waits, yields or ends itself;
 * then the thread that gave it the turn goes on.  The main task is the
 * application with its cooperative threads.  Timers (MPArmTimer) fire at
 * those same moments, on the machine's clock.
 *
 * IDs are opaque references (ref_new, carbon_events.c).
 */
#include <stdlib.h>
#include <string.h>

#include "carbon_events.h"
#include "ppc_bridge.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; return; } while (0)
/* A wait the machine's stopping cut short is left unfinished (going on
 * runs it again), not answered with a timeout. */
#define RETURN_WAIT(v) do { if (g_cpu->halted) return; RETURN(v); } while (0)

enum {
    kMPIterationEndErr = -29275,
    kMPDeletedErr = -29295,
    kMPTimeoutErr = -29296,
    kMPTaskAbortedErr = -29297,
    kMPInsufficientResourcesErr = -29298,
    kMPInvalidIDErr = -29299,
};
enum { kDurationImmediate = 0, kDurationForever = 0x7FFFFFFF };
enum { kMPPreserveTimerIDMask = 1, kMPTimeIsDeltaMask = 2, kMPTimeIsDurationMask = 4 };
enum { kMPAllocateClearMask = 1 };
enum { kMPTaskStateTaskInfo = 5, kMPTaskBlocked = 0, kMPTaskReady = 1, kMPTaskRunning = 2 };
enum { kStoppedThreadState = 1, kRunningThreadState = 2, kPreemptiveThread = 2, kNewSuspend = 1 };
enum { kCurrentThreadID = 1, kApplicationThreadID = 2 };
enum { NS_PER_ABS = 40 }; /* UpTime's unit, as driverservices.c has it */

/* Reference kinds (carbon_events.c has its own, below 100). */
enum { REF_TASK = 101, REF_QUEUE, REF_SEMAPHORE, REF_REGION, REF_EVENT, REF_NOTIFICATION, REF_TIMER,
       REF_PROCESS, REF_CPU, REF_COHERENCE, REF_SPACE };

#define MAX_STORAGE 64
#define TASK_STACK 0x10000u

typedef struct task {
    gaddr id;
    uint32_t thread; /* its Thread Manager thread (0 for the main task) */
    gaddr notify_queue;
    uint32_t term1, term2;
    uint32_t type, weight;
    uint32_t resumer; /* the thread that gave it its turn */
    int32_t status;   /* what it ended with */
    bool terminated;  /* by MPTerminateTask: status as given */
    uint64_t created;
    gaddr exception_queue;
    uint32_t storage[MAX_STORAGE];
    struct task *next;
} task;

/* What a wait can be on: each kind keeps waiters from being freed under
 * them (dead once deleted). */
typedef struct {
    gaddr id;
    int waiters;
    bool dead;
} object;

typedef struct {
    object o;
    uint32_t (*msgs)[3];
    uint32_t head, count, cap;
} mpqueue;

typedef struct {
    object o;
    uint32_t value, max;
} semaphore;

typedef struct {
    object o;
    task *owner;
    uint32_t depth;
} region;

typedef struct {
    object o;
    uint32_t flags;
} eventgroup;

/* What a notification (or a timer) does: notify a queue, signal a
 * semaphore, set an event group's flags. */
typedef struct {
    gaddr queue, sem, event, notification;
    uint32_t qparam[3], flags;
} notify;

typedef struct {
    object o;
    notify n;
} notification;

typedef struct mptimer {
    object o;
    notify n;
    bool armed;
    uint64_t when; /* ns */
    uint32_t options;
    struct mptimer *next;
} mptimer;

static task main_task;
static task *tasks;
static mptimer *timers;
static bool storage_used[MAX_STORAGE];
static uint32_t progress; /* bumped whenever something could let a waiter go on */

static uint64_t now_ns(void) { return machine_time_ns(); }

static uint64_t duration_ns(int32_t d)
{
    return d > 0 ? (uint64_t)d * 1000000u : (uint64_t)-(int64_t)d * 1000u;
}

static uint64_t rd64(gaddr p) { return (uint64_t)rd32(p) << 32 | rd32(p + 4); }
static void wr64(gaddr p, uint64_t v)
{
    wr32(p, (uint32_t)(v >> 32));
    wr32(p + 4, (uint32_t)v);
}

/* ---- Tasks ---- */

static task *task_of_thread(uint32_t thread)
{
    for (task *t = tasks; t; t = t->next)
        if (t->thread == thread)
            return t;
    return NULL;
}

/* The task running now: a task's thread, or the main task. */
static task *current_task(void)
{
    task *t = task_of_thread(thread_current_id());
    if (t)
        return t;
    if (!main_task.id)
        main_task.id = ref_new(REF_TASK, &main_task);
    return &main_task;
}

static bool in_task(void) { return current_task() != &main_task; }

static void unlink_task(task *t)
{
    for (task **p = &tasks; *p; p = &(*p)->next)
        if (*p == t) {
            *p = t->next;
            break;
        }
}

static int32_t notify_queue(gaddr q, uint32_t p1, uint32_t p2, uint32_t p3);

/* A task's thread has ended: its termination message goes to its queue. */
static void task_ended(uint32_t thread, uint32_t result)
{
    task *t = task_of_thread(thread);
    if (!t)
        return;
    int32_t status = t->terminated ? t->status : (int32_t)result;
    unlink_task(t);
    if (t->notify_queue)
        notify_queue(t->notify_queue, t->term1, t->term2, (uint32_t)status);
    ref_free(t->id);
    free(t);
    progress++;
}

/* Give task t its turn; back here when it waits, yields or ends. */
static void run_task(task *t)
{
    uint32_t me = thread_current_id();
    t->resumer = me;
    thread_set_exit(t->thread, task_ended, me);
    tb_SetThreadState((int32_t)t->thread, kRunningThreadState, 0);
}

/* From a task: back to the thread that gave it its turn, stopped till the
 * next. */
static void task_yield(task *t)
{
    tb_SetThreadState(kCurrentThreadID, kStoppedThreadState, (int32_t)t->resumer);
}

static void fire_timers(void);

int mp_run_tasks(void)
{
    fire_timers();
    if (in_task() || !tasks)
        return 0;
    /* Each task once, those made meanwhile next time. */
    uint32_t ids[64];
    int n = 0;
    for (task *t = tasks; t && n < 64; t = t->next)
        ids[n++] = t->thread;
    for (int i = 0; i < n && !g_cpu->halted; i++) {
        task *t = task_of_thread(ids[i]);
        if (t)
            run_task(t);
    }
    return n;
}

bool mp_have_tasks(void) { return tasks != NULL; }

/* Waiting: a task hands back its turn; the main task gives the tasks
 * theirs, and when none of them got anywhere, time passes. */
static void pause_once(void)
{
    task *t = current_task();
    if (t != &main_task) {
        task_yield(t);
        return;
    }
    uint32_t before = progress;
    mp_run_tasks();
    if (progress == before && !g_cpu->halted)
        ev_wait_tick();
}

/* Wait until ready(arg) or the timeout: noErr, kMPTimeoutErr, or
 * kMPDeletedErr when the object goes. */
static int32_t wait_for(object *o, bool (*ready)(void *), void *arg, int32_t timeout)
{
    uint64_t deadline = timeout == kDurationForever ? UINT64_MAX : now_ns() + duration_ns(timeout);
    o->waiters++;
    int32_t err = noErr;
    for (;;) {
        if (o->dead) {
            err = kMPDeletedErr;
            break;
        }
        if (ready(arg))
            break;
        if (timeout == kDurationImmediate || now_ns() >= deadline || g_cpu->halted) {
            err = kMPTimeoutErr;
            break;
        }
        pause_once();
    }
    o->waiters--;
    return err;
}

/* An object deleted: freed now, or by its last waiter. */
static void object_delete(object *o)
{
    ref_free(o->id);
    o->dead = true;
    progress++;
}

static bool object_gone(object *o) { return o->dead && o->waiters == 0; }

/* MPProcessors / MPProcessorsScheduled: one. */
static void mp_MPProcessors(ppc_cpu *c) { RETURN(1); }

/* MPCreateTask(entryPoint, parameter, stackSize, notifyQueue, terminationParameter1,
 * terminationParameter2, options, &task) */
static void mp_MPCreateTask(ppc_cpu *c)
{
    gaddr entry = R(0), param = R(1), out = R(7);
    uint32_t stack = R(2) > TASK_STACK ? R(2) : TASK_STACK;
    if (!entry)
        RETURN(paramErr);
    if (R(3) && !ref_get(R(3), REF_QUEUE))
        RETURN(kMPInvalidIDErr);
    task *t = calloc(1, sizeof *t);
    if (!t)
        RETURN(kMPInsufficientResourcesErr);
    gaddr idp = tb_NewPtr(4, false, true);
    int16_t err = idp ? tb_NewThread(kPreemptiveThread, entry, param, (int32_t)stack, kNewSuspend, 0, idp) : memFullErr;
    if (err) {
        if (idp)
            tb_DisposePtr(idp);
        free(t);
        RETURN(kMPInsufficientResourcesErr);
    }
    t->thread = rd32(idp);
    tb_DisposePtr(idp);
    t->id = ref_new(REF_TASK, t);
    t->notify_queue = R(3);
    t->term1 = R(4);
    t->term2 = R(5);
    t->weight = 100;
    t->created = now_ns();
    t->next = tasks;
    tasks = t;
    if (out)
        wr32(out, t->id);
    RETURN(noErr);
}

/* MPTerminateTask(task, terminationStatus) */
static void mp_MPTerminateTask(ppc_cpu *c)
{
    task *t = ref_get(R(0), REF_TASK);
    if (!t || t == &main_task)
        RETURN(kMPInvalidIDErr);
    t->terminated = true;
    t->status = (int32_t)R(1);
    tb_DisposeThread((int32_t)t->thread, R(1), false); /* a task ending itself leaves here */
    RETURN(noErr);
}

/* MPExit(status): the running task ends. */
static void mp_MPExit(ppc_cpu *c)
{
    task *t = current_task();
    if (t != &main_task) {
        t->terminated = true;
        t->status = (int32_t)R(0);
        tb_DisposeThread((int32_t)t->thread, R(0), false);
    }
    c->pc = c->lr;
}

static void mp_MPYield(ppc_cpu *c)
{
    task *t = current_task();
    progress++; /* a task yielding is working, not waiting */
    if (t != &main_task)
        task_yield(t);
    else
        mp_run_tasks();
    c->pc = c->lr;
}

static void mp_MPCurrentTaskID(ppc_cpu *c) { RETURN(current_task()->id); }

static void mp_MPTaskIsPreemptive(ppc_cpu *c)
{
    task *t = R(0) ? ref_get(R(0), REF_TASK) : current_task();
    RETURN(t && t != &main_task);
}

static void mp_MPSetTaskWeight(ppc_cpu *c)
{
    task *t = ref_get(R(0), REF_TASK);
    if (!t)
        RETURN(kMPInvalidIDErr);
    t->weight = R(1);
    RETURN(noErr);
}

static void mp_MPSetTaskType(ppc_cpu *c)
{
    task *t = ref_get(R(0), REF_TASK);
    if (!t)
        RETURN(kMPInvalidIDErr);
    t->type = R(1);
    RETURN(noErr);
}

/* ---- Task storage ---- */

static void mp_MPAllocateTaskStorageIndex(ppc_cpu *c)
{
    for (uint32_t i = 0; i < MAX_STORAGE; i++)
        if (!storage_used[i]) {
            storage_used[i] = true;
            main_task.storage[i] = 0;
            for (task *t = tasks; t; t = t->next)
                t->storage[i] = 0;
            if (R(0))
                wr32(R(0), i);
            RETURN(noErr);
        }
    RETURN(kMPInsufficientResourcesErr);
}

static void mp_MPDeallocateTaskStorageIndex(ppc_cpu *c)
{
    if (R(0) >= MAX_STORAGE || !storage_used[R(0)])
        RETURN(paramErr);
    storage_used[R(0)] = false;
    RETURN(noErr);
}

static void mp_MPSetTaskStorageValue(ppc_cpu *c)
{
    if (R(0) >= MAX_STORAGE || !storage_used[R(0)])
        RETURN(paramErr);
    current_task()->storage[R(0)] = R(1);
    RETURN(noErr);
}

static void mp_MPGetTaskStorageValue(ppc_cpu *c)
{
    RETURN(R(0) < MAX_STORAGE && storage_used[R(0)] ? current_task()->storage[R(0)] : 0);
}

/* ---- Queues ---- */

static void mp_MPCreateQueue(ppc_cpu *c)
{
    mpqueue *q = calloc(1, sizeof *q);
    if (!q)
        RETURN(kMPInsufficientResourcesErr);
    q->o.id = ref_new(REF_QUEUE, q);
    if (R(0))
        wr32(R(0), q->o.id);
    RETURN(noErr);
}

static void queue_free(mpqueue *q)
{
    free(q->msgs);
    free(q);
}

static void mp_MPDeleteQueue(ppc_cpu *c)
{
    mpqueue *q = ref_get(R(0), REF_QUEUE);
    if (!q)
        RETURN(kMPInvalidIDErr);
    object_delete(&q->o);
    if (object_gone(&q->o))
        queue_free(q);
    RETURN(noErr);
}

static int32_t notify_queue(gaddr id, uint32_t p1, uint32_t p2, uint32_t p3)
{
    mpqueue *q = ref_get(id, REF_QUEUE);
    if (!q)
        return kMPInvalidIDErr;
    if (q->count == q->cap) {
        uint32_t cap = q->cap ? q->cap * 2 : 16;
        if (cap > 1u << 20)
            return kMPInsufficientResourcesErr;
        uint32_t (*m)[3] = malloc(sizeof *m * cap);
        if (!m)
            return kMPInsufficientResourcesErr;
        for (uint32_t i = 0; i < q->count; i++)
            memcpy(m[i], q->msgs[(q->head + i) % q->cap], sizeof m[i]);
        free(q->msgs);
        q->msgs = m;
        q->head = 0;
        q->cap = cap;
    }
    uint32_t *m = q->msgs[(q->head + q->count++) % q->cap];
    m[0] = p1, m[1] = p2, m[2] = p3;
    progress++;
    return noErr;
}

static void mp_MPNotifyQueue(ppc_cpu *c) { RETURN(notify_queue(R(0), R(1), R(2), R(3))); }

static bool queue_ready(void *q) { return ((mpqueue *)q)->count > 0; }

/* MPWaitOnQueue(queue, &param1, &param2, &param3, timeout) */
static void mp_MPWaitOnQueue(ppc_cpu *c)
{
    mpqueue *q = ref_get(R(0), REF_QUEUE);
    if (!q)
        RETURN(kMPInvalidIDErr);
    gaddr p1 = R(1), p2 = R(2), p3 = R(3);
    int32_t err = wait_for(&q->o, queue_ready, q, (int32_t)R(4));
    if (!err) {
        uint32_t *m = q->msgs[q->head];
        q->head = (q->head + 1) % q->cap;
        q->count--;
        if (p1)
            wr32(p1, m[0]);
        if (p2)
            wr32(p2, m[1]);
        if (p3)
            wr32(p3, m[2]);
    } else if (object_gone(&q->o)) {
        queue_free(q);
    }
    RETURN_WAIT(err);
}

/* MPSetQueueReserve(queue, count): notifications never fail for want of
 * room here, so there is nothing to set aside. */
static void mp_MPSetQueueReserve(ppc_cpu *c) { RETURN(ref_get(R(0), REF_QUEUE) ? noErr : kMPInvalidIDErr); }

/* ---- Semaphores ---- */

static void mp_MPCreateSemaphore(ppc_cpu *c)
{
    if (R(1) > R(0))
        RETURN(paramErr);
    semaphore *s = calloc(1, sizeof *s);
    if (!s)
        RETURN(kMPInsufficientResourcesErr);
    s->max = R(0);
    s->value = R(1);
    s->o.id = ref_new(REF_SEMAPHORE, s);
    if (R(2))
        wr32(R(2), s->o.id);
    RETURN(noErr);
}

static void mp_MPDeleteSemaphore(ppc_cpu *c)
{
    semaphore *s = ref_get(R(0), REF_SEMAPHORE);
    if (!s)
        RETURN(kMPInvalidIDErr);
    object_delete(&s->o);
    if (object_gone(&s->o))
        free(s);
    RETURN(noErr);
}

static int32_t signal_semaphore(gaddr id)
{
    semaphore *s = ref_get(id, REF_SEMAPHORE);
    if (!s)
        return kMPInvalidIDErr;
    if (s->value >= s->max)
        return kMPInsufficientResourcesErr;
    s->value++;
    progress++;
    return noErr;
}

static void mp_MPSignalSemaphore(ppc_cpu *c) { RETURN(signal_semaphore(R(0))); }

static bool semaphore_ready(void *s) { return ((semaphore *)s)->value > 0; }

static void mp_MPWaitOnSemaphore(ppc_cpu *c)
{
    semaphore *s = ref_get(R(0), REF_SEMAPHORE);
    if (!s)
        RETURN(kMPInvalidIDErr);
    int32_t err = wait_for(&s->o, semaphore_ready, s, (int32_t)R(1));
    if (!err)
        s->value--;
    else if (object_gone(&s->o))
        free(s);
    RETURN_WAIT(err);
}

/* ---- Critical regions: one task at a time, which may enter again ---- */

static void mp_MPCreateCriticalRegion(ppc_cpu *c)
{
    region *r = calloc(1, sizeof *r);
    if (!r)
        RETURN(kMPInsufficientResourcesErr);
    r->o.id = ref_new(REF_REGION, r);
    if (R(0))
        wr32(R(0), r->o.id);
    RETURN(noErr);
}

static void mp_MPDeleteCriticalRegion(ppc_cpu *c)
{
    region *r = ref_get(R(0), REF_REGION);
    if (!r)
        RETURN(kMPInvalidIDErr);
    object_delete(&r->o);
    if (object_gone(&r->o))
        free(r);
    RETURN(noErr);
}

typedef struct {
    region *r;
    task *me;
} region_wait;

static bool region_free(void *a)
{
    region_wait *w = a;
    return !w->r->owner || w->r->owner == w->me;
}

static void mp_MPEnterCriticalRegion(ppc_cpu *c)
{
    region *r = ref_get(R(0), REF_REGION);
    if (!r)
        RETURN(kMPInvalidIDErr);
    region_wait w = { r, current_task() };
    int32_t err = wait_for(&r->o, region_free, &w, (int32_t)R(1));
    if (!err) {
        r->owner = w.me;
        r->depth++;
    } else if (object_gone(&r->o)) {
        free(r);
    }
    RETURN_WAIT(err);
}

static void mp_MPExitCriticalRegion(ppc_cpu *c)
{
    region *r = ref_get(R(0), REF_REGION);
    if (!r)
        RETURN(kMPInvalidIDErr);
    if (r->owner != current_task())
        RETURN(kMPInsufficientResourcesErr);
    if (--r->depth == 0) {
        r->owner = NULL;
        progress++;
    }
    RETURN(noErr);
}

/* ---- Event groups ---- */

static void mp_MPCreateEvent(ppc_cpu *c)
{
    eventgroup *e = calloc(1, sizeof *e);
    if (!e)
        RETURN(kMPInsufficientResourcesErr);
    e->o.id = ref_new(REF_EVENT, e);
    if (R(0))
        wr32(R(0), e->o.id);
    RETURN(noErr);
}

static void mp_MPDeleteEvent(ppc_cpu *c)
{
    eventgroup *e = ref_get(R(0), REF_EVENT);
    if (!e)
        RETURN(kMPInvalidIDErr);
    object_delete(&e->o);
    if (object_gone(&e->o))
        free(e);
    RETURN(noErr);
}

static int32_t set_event(gaddr id, uint32_t flags)
{
    eventgroup *e = ref_get(id, REF_EVENT);
    if (!e)
        return kMPInvalidIDErr;
    e->flags |= flags;
    progress++;
    return noErr;
}

static void mp_MPSetEvent(ppc_cpu *c) { RETURN(set_event(R(0), R(1))); }

static bool event_ready(void *e) { return ((eventgroup *)e)->flags != 0; }

/* MPWaitForEvent(event, &flags, timeout): the flags set, all cleared. */
static void mp_MPWaitForEvent(ppc_cpu *c)
{
    eventgroup *e = ref_get(R(0), REF_EVENT);
    if (!e)
        RETURN(kMPInvalidIDErr);
    gaddr out = R(1);
    int32_t err = wait_for(&e->o, event_ready, e, (int32_t)R(2));
    if (!err) {
        if (out)
            wr32(out, e->flags);
        e->flags = 0;
    } else if (object_gone(&e->o)) {
        free(e);
    }
    RETURN_WAIT(err);
}

/* ---- Notifications ---- */

static int32_t cause(const notify *n)
{
    if (n->queue)
        notify_queue(n->queue, n->qparam[0], n->qparam[1], n->qparam[2]);
    if (n->sem)
        signal_semaphore(n->sem);
    if (n->event)
        set_event(n->event, n->flags);
    if (n->notification) {
        notification *m = ref_get(n->notification, REF_NOTIFICATION);
        if (m)
            cause(&m->n);
    }
    return noErr;
}

/* What anID is told, by its kind. */
static int32_t set_notify(notify *n, gaddr id, uint32_t p1, uint32_t p2, uint32_t p3, bool notifications)
{
    switch (ref_kind(id)) {
    case REF_QUEUE:
        n->queue = id;
        n->qparam[0] = p1, n->qparam[1] = p2, n->qparam[2] = p3;
        return noErr;
    case REF_SEMAPHORE:
        n->sem = id;
        return noErr;
    case REF_EVENT:
        n->event = id;
        n->flags = p1;
        return noErr;
    case REF_NOTIFICATION:
        if (notifications) {
            n->notification = id;
            return noErr;
        }
        return kMPInvalidIDErr;
    }
    return kMPInvalidIDErr;
}

static void mp_MPCreateNotification(ppc_cpu *c)
{
    notification *m = calloc(1, sizeof *m);
    if (!m)
        RETURN(kMPInsufficientResourcesErr);
    m->o.id = ref_new(REF_NOTIFICATION, m);
    if (R(0))
        wr32(R(0), m->o.id);
    RETURN(noErr);
}

static void mp_MPDeleteNotification(ppc_cpu *c)
{
    notification *m = ref_get(R(0), REF_NOTIFICATION);
    if (!m)
        RETURN(kMPInvalidIDErr);
    ref_free(m->o.id);
    free(m);
    RETURN(noErr);
}

/* MPModifyNotification(notification, anID, notifyParam1, notifyParam2, notifyParam3) */
static void mp_MPModifyNotification(ppc_cpu *c)
{
    notification *m = ref_get(R(0), REF_NOTIFICATION);
    if (!m)
        RETURN(kMPInvalidIDErr);
    RETURN(set_notify(&m->n, R(1), R(2), R(3), R(4), false));
}

/* MPModifyNotificationParameters(notification, kind, notifyParam1, notifyParam2, notifyParam3) */
static void mp_MPModifyNotificationParameters(ppc_cpu *c)
{
    notification *m = ref_get(R(0), REF_NOTIFICATION);
    if (!m)
        RETURN(kMPInvalidIDErr);
    switch (R(1)) {
    case 4: /* kOpaqueQueueID */
        m->n.qparam[0] = R(2), m->n.qparam[1] = R(3), m->n.qparam[2] = R(4);
        break;
    case 9: /* kOpaqueEventID */
        m->n.flags = R(2);
        break;
    case 5: /* kOpaqueSemaphoreID: no parameters */
        break;
    default:
        RETURN(paramErr);
    }
    RETURN(noErr);
}

static void mp_MPCauseNotification(ppc_cpu *c)
{
    notification *m = ref_get(R(0), REF_NOTIFICATION);
    RETURN(m ? cause(&m->n) : kMPInvalidIDErr);
}

/* ---- Timers ---- */

static void mp_MPCreateTimer(ppc_cpu *c)
{
    mptimer *t = calloc(1, sizeof *t);
    if (!t)
        RETURN(kMPInsufficientResourcesErr);
    t->o.id = ref_new(REF_TIMER, t);
    t->next = timers;
    timers = t;
    if (R(0))
        wr32(R(0), t->o.id);
    RETURN(noErr);
}

static void timer_delete(mptimer *t)
{
    for (mptimer **p = &timers; *p; p = &(*p)->next)
        if (*p == t) {
            *p = t->next;
            break;
        }
    ref_free(t->o.id);
    free(t);
}

static void mp_MPDeleteTimer(ppc_cpu *c)
{
    mptimer *t = ref_get(R(0), REF_TIMER);
    if (!t)
        RETURN(kMPInvalidIDErr);
    timer_delete(t);
    RETURN(noErr);
}

/* MPSetTimerNotify(timer, anID, notifyParam1, notifyParam2, notifyParam3) */
static void mp_MPSetTimerNotify(ppc_cpu *c)
{
    mptimer *t = ref_get(R(0), REF_TIMER);
    if (!t)
        RETURN(kMPInvalidIDErr);
    RETURN(set_notify(&t->n, R(1), R(2), R(3), R(4), true));
}

/* MPArmTimer(timer, &expirationTime, options): an AbsoluteTime, one from
 * now (kMPTimeIsDeltaMask), or a Duration in its first word
 * (kMPTimeIsDurationMask). */
static void mp_MPArmTimer(ppc_cpu *c)
{
    mptimer *t = ref_get(R(0), REF_TIMER);
    if (!t)
        RETURN(kMPInvalidIDErr);
    if (!R(1))
        RETURN(paramErr);
    uint32_t opts = R(2);
    uint64_t when;
    if (opts & kMPTimeIsDurationMask)
        when = now_ns() + duration_ns((int32_t)rd32(R(1)));
    else if (opts & kMPTimeIsDeltaMask)
        when = now_ns() + rd64(R(1)) * NS_PER_ABS;
    else
        when = rd64(R(1)) * NS_PER_ABS;
    t->when = when;
    t->options = opts;
    t->armed = true;
    RETURN(noErr);
}

/* MPCancelTimer(timer, &timeRemaining) */
static void mp_MPCancelTimer(ppc_cpu *c)
{
    mptimer *t = ref_get(R(0), REF_TIMER);
    if (!t)
        RETURN(kMPInvalidIDErr);
    if (!t->armed)
        RETURN(kMPInsufficientResourcesErr);
    uint64_t now = now_ns();
    if (R(1))
        wr64(R(1), t->when > now ? (t->when - now) / NS_PER_ABS : 0);
    t->armed = false;
    RETURN(noErr);
}

static void fire_timers(void)
{
    uint64_t now = now_ns();
    for (mptimer *t = timers, *next; t; t = next) {
        next = t->next;
        if (!t->armed || now < t->when)
            continue;
        t->armed = false;
        cause(&t->n);
        if (!(t->options & kMPPreserveTimerIDMask))
            timer_delete(t);
    }
}

/* MPDelayUntil(&expirationTime) */
static void mp_MPDelayUntil(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    uint64_t until = rd64(R(0)) * NS_PER_ABS;
    while (now_ns() < until && !g_cpu->halted)
        pause_once();
    RETURN_WAIT(noErr);
}

/* ---- Memory ---- */

/* A block aligned as asked (2^alignment bytes; the page or a cache line
 * for the pseudo values), with where it came from and its size before it. */
static gaddr allocate(uint32_t size, uint32_t align_log, uint32_t options)
{
    uint32_t align = align_log == 254 ? 4096 : align_log == 255 ? 32 : align_log > 16 ? 0 : 1u << align_log;
    if (align < 8)
        align = 8;
    if (align_log > 16 && align_log < 254)
        return 0;
    if (size > 0x40000000u)
        return 0;
    gaddr raw = tb_NewPtr((int32_t)(size + align + 8), false, (options & kMPAllocateClearMask) != 0);
    if (!raw)
        return 0;
    gaddr p = (raw + 8 + align - 1) & ~(align - 1);
    wr32(p - 8, raw);
    wr32(p - 4, size);
    return p;
}

static void mp_MPAllocateAligned(ppc_cpu *c) { RETURN(allocate(R(0), R(1) & 0xFF, R(2))); }
static void mp_MPAllocate(ppc_cpu *c) { RETURN(allocate(R(0), 5, kMPAllocateClearMask)); }

static void mp_MPFree(ppc_cpu *c)
{
    if (R(0))
        tb_DisposePtr(rd32(R(0) - 8));
    c->pc = c->lr;
}

static void mp_MPGetAllocatedBlockSize(ppc_cpu *c) { RETURN(R(0) ? rd32(R(0) - 4) : 0); }

/* MPBlockCopy(source, destination, size) */
static void mp_MPBlockCopy(ppc_cpu *c)
{
    gaddr s = R(0), d = R(1);
    uint32_t n = R(2);
    if (s && d && n) {
        uint8_t *buf = malloc(n);
        if (buf) {
            gget(buf, s, n);
            gput(d, buf, n);
            free(buf);
        }
    }
    c->pc = c->lr;
}

static void mp_MPBlockClear(ppc_cpu *c)
{
    if (R(0) && R(1))
        gmemset(R(0), 0, R(1));
    c->pc = c->lr;
}

/* MPDataToCode: the interpreters see stores to code as they happen. */
static void mp_nothing(ppc_cpu *c) { c->pc = c->lr; }

/* ---- Exceptions and debugging: tasks take no exceptions of their own
 * here (a fault stops the machine), so there is no state to give. ---- */

static void mp_MPSetExceptionHandler(ppc_cpu *c)
{
    task *t = ref_get(R(0), REF_TASK);
    if (!t)
        RETURN(kMPInvalidIDErr);
    if (R(1) && !ref_get(R(1), REF_QUEUE))
        RETURN(kMPInvalidIDErr);
    t->exception_queue = R(1);
    RETURN(noErr);
}

/* MPDisposeTaskException, MPSetTaskState, MPThrowException: the task has
 * no exception pending and is never stopped for one. */
static void mp_task_no_exception(ppc_cpu *c) { RETURN(ref_get(R(0), REF_TASK) ? kMPInsufficientResourcesErr : kMPInvalidIDErr); }

/* MPExtractTaskState(task, kind, info): the task information alone. */
static void mp_MPExtractTaskState(ppc_cpu *c)
{
    task *t = ref_get(R(0), REF_TASK);
    gaddr info = R(2);
    if (!t)
        RETURN(kMPInvalidIDErr);
    if (R(1) != kMPTaskStateTaskInfo || !info)
        RETURN(paramErr);
    gmemset(info, 0, 104);
    wr32(info + 0, 3);       /* version */
    wr32(info + 4, t->type); /* name */
    uint16_t state = t == current_task() ? kMPTaskRunning : kMPTaskReady;
    wr16(info + 12, state);
    wr32(info + 16, t->weight);
    wr64(info + 36, t->created / NS_PER_ABS);
    RETURN(noErr);
}

static void mp_MPRegisterDebugger(ppc_cpu *c) { RETURN(noErr); }

/* MPRemoteCall(remoteProc, parameter, context): run on the main task.
 * Tasks run only while it waits on them, so the call is made here. */
static void mp_MPRemoteCall(ppc_cpu *c)
{
    gaddr proc = R(0);
    uint32_t arg = R(1);
    RETURN(proc ? ppc_call(proc, &arg, 1) : 0);
}

/* ---- Iterating the IDs (MPDiagnostics' MPGetNext...ID) ---- */

enum { kOpaqueAnyID, kOpaqueProcessID, kOpaqueTaskID, kOpaqueTimerID, kOpaqueQueueID, kOpaqueSemaphoreID,
       kOpaqueCriticalRegionID, kOpaqueCpuID, kOpaqueAddressSpaceID, kOpaqueEventID, kOpaqueCoherenceID,
       kOpaqueAreaID, kOpaqueNotificationID, kOpaqueConsoleID };

/* The one process, address space, coherence group and processor there are:
 * a reference each, made when first asked for. */
static struct { int kind; gaddr id; } fixed_refs[4];

static gaddr fixed_ref(int kind)
{
    for (int i = 0; i < 4; i++) {
        if (fixed_refs[i].kind == kind)
            return fixed_refs[i].id;
        if (!fixed_refs[i].kind) {
            fixed_refs[i].kind = kind;
            fixed_refs[i].id = ref_new(kind, &fixed_refs[i]);
            return fixed_refs[i].id;
        }
    }
    return 0;
}

/* The next ID of a class after *id (0 for the first): kMPIterationEndErr
 * at the end, kMPInvalidIDErr where *id isn't one. */
static int32_t next_id(uint32_t cls, gaddr idp)
{
    gaddr after = rd32(idp), next = 0;
    int kind = 0;
    switch (cls) {
    case kOpaqueProcessID: kind = REF_PROCESS; break;
    case kOpaqueCpuID: kind = REF_CPU; break;
    case kOpaqueCoherenceID: kind = REF_COHERENCE; break;
    case kOpaqueAddressSpaceID: kind = REF_SPACE; break;
    case kOpaqueTimerID: kind = REF_TIMER; break;
    case kOpaqueQueueID: kind = REF_QUEUE; break;
    case kOpaqueSemaphoreID: kind = REF_SEMAPHORE; break;
    case kOpaqueCriticalRegionID: kind = REF_REGION; break;
    case kOpaqueEventID: kind = REF_EVENT; break;
    case kOpaqueNotificationID: kind = REF_NOTIFICATION; break;
    case kOpaqueTaskID: {
        /* The main task, then the others as they were made. */
        gaddr ids[65];
        int n = 0, at = -1;
        current_task(); /* makes the main task's ID */
        ids[n++] = main_task.id;
        task *list[64];
        int m = 0;
        for (task *t = tasks; t && m < 64; t = t->next)
            list[m++] = t;
        while (m)
            ids[n++] = list[--m]->id;
        if (after) {
            at = -2;
            for (int i = 0; i < n; i++)
                if (ids[i] == after)
                    at = i;
            if (at == -2)
                return kMPInvalidIDErr;
        }
        if (at + 1 >= n)
            return kMPIterationEndErr;
        wr32(idp, ids[at + 1]);
        return noErr;
    }
    default: return cls == kOpaqueAreaID || cls == kOpaqueConsoleID ? kMPIterationEndErr : paramErr;
    }
    if (kind == REF_PROCESS || kind == REF_CPU || kind == REF_COHERENCE || kind == REF_SPACE) {
        gaddr one = fixed_ref(kind);
        if (!after) {
            wr32(idp, one);
            return noErr;
        }
        return after == one ? kMPIterationEndErr : kMPInvalidIDErr;
    }
    if (after && ref_kind(after) != kind)
        return kMPInvalidIDErr;
    next = ref_next(kind, after);
    if (!next)
        return kMPIterationEndErr;
    wr32(idp, next);
    return noErr;
}

/* MPGetNextID(class, &id) and the per-class ones. */
static void mp_MPGetNextID(ppc_cpu *c) { RETURN(R(1) ? next_id(R(0), R(1)) : paramErr); }
static void mp_MPGetNextTaskID(ppc_cpu *c) { RETURN(R(1) ? next_id(kOpaqueTaskID, R(1)) : paramErr); }
static void mp_MPGetNextCpuID(ppc_cpu *c) { RETURN(R(1) ? next_id(kOpaqueCpuID, R(1)) : paramErr); }
static void mp_MPGetNextAreaID(ppc_cpu *c) { RETURN(R(1) ? next_id(kOpaqueAreaID, R(1)) : paramErr); }

/* The page size: one class, 4 KB pages. */
static void mp_MPGetPageSizeClasses(ppc_cpu *c) { RETURN(1); }
static void mp_MPGetPageSize(ppc_cpu *c) { RETURN(R(0) == 1 ? 4096 : 0); }

/* _MPTaskIsToolboxSafe(): tasks run on the application's thread of
 * control here, so they can make Toolbox calls. */
static void mp_TaskIsToolboxSafe(ppc_cpu *c) { RETURN(1); }

/* ---- The library ---- */

static void mp_IsFullyInitialized(ppc_cpu *c) { RETURN(1); }

/* _MPLibraryVersion(&versionCString, &major, &minor, &release, &revision): 2.2.0. */
static gaddr version_string;
static void mp_MPLibraryVersion(ppc_cpu *c)
{
    if (!version_string) {
        static const char v[] = "2.2.0";
        version_string = tb_NewPtr(sizeof v, true, true);
        if (version_string)
            gput(version_string, v, sizeof v);
    }
    if (R(0))
        wr32(R(0), version_string);
    uint32_t parts[4] = { 2, 2, 0, 0 };
    for (int i = 0; i < 4; i++)
        if (R(1 + i))
            wr32(R(1 + i), parts[i]);
    c->pc = c->lr;
}

/* _MPLibraryIsCompatible(versionCString, major, minor, release, revision):
 * a program built for 2.x or earlier. */
static void mp_MPLibraryIsCompatible(ppc_cpu *c) { RETURN(R(1) < 2 || (R(1) == 2 && R(2) <= 2)); }

void mp_reset(void)
{
    while (tasks) {
        task *t = tasks;
        tasks = t->next;
        free(t);
    }
    while (timers) {
        mptimer *t = timers;
        timers = t->next;
        free(t);
    }
    memset(&main_task, 0, sizeof main_task);
    memset(storage_used, 0, sizeof storage_used);
    memset(fixed_refs, 0, sizeof fixed_refs);
    version_string = 0;
    progress = 0;
}

const ppc_export mp_exports[] = {
    { "MPAllocate", mp_MPAllocate },
    { "MPAllocateAligned", mp_MPAllocateAligned },
    { "MPAllocateGlobalTaskStorageIndex", mp_MPAllocateTaskStorageIndex },
    { "MPAllocateTaskStorageIndex", mp_MPAllocateTaskStorageIndex },
    { "MPArmTimer", mp_MPArmTimer },
    { "MPBlockClear", mp_MPBlockClear },
    { "MPBlockCopy", mp_MPBlockCopy },
    { "MPCancelTimer", mp_MPCancelTimer },
    { "MPCauseNotification", mp_MPCauseNotification },
    { "MPCreateCriticalRegion", mp_MPCreateCriticalRegion },
    { "MPCreateEvent", mp_MPCreateEvent },
    { "MPCreateNotification", mp_MPCreateNotification },
    { "MPCreateQueue", mp_MPCreateQueue },
    { "MPCreateSemaphore", mp_MPCreateSemaphore },
    { "MPCreateTask", mp_MPCreateTask },
    { "MPCreateTimer", mp_MPCreateTimer },
    { "MPCurrentTaskID", mp_MPCurrentTaskID },
    { "MPDataToCode", mp_nothing },
    { "MPDeallocateTaskStorageIndex", mp_MPDeallocateTaskStorageIndex },
    { "MPDelayUntil", mp_MPDelayUntil },
    { "MPDelayUntilSys", mp_MPDelayUntil },
    { "MPDeleteCriticalRegion", mp_MPDeleteCriticalRegion },
    { "MPDeleteEvent", mp_MPDeleteEvent },
    { "MPDeleteNotification", mp_MPDeleteNotification },
    { "MPDeleteQueue", mp_MPDeleteQueue },
    { "MPDeleteSemaphore", mp_MPDeleteSemaphore },
    { "MPDeleteTimer", mp_MPDeleteTimer },
    { "MPDisposeTaskException", mp_task_no_exception },
    { "MPEnterCriticalRegion", mp_MPEnterCriticalRegion },
    { "MPExit", mp_MPExit },
    { "MPExitCriticalRegion", mp_MPExitCriticalRegion },
    { "MPExtractTaskState", mp_MPExtractTaskState },
    { "MPFree", mp_MPFree },
    { "MPGetAllocatedBlockSize", mp_MPGetAllocatedBlockSize },
    { "MPGetNextAreaID", mp_MPGetNextAreaID },
    { "MPGetNextCpuID", mp_MPGetNextCpuID },
    { "MPGetNextID", mp_MPGetNextID },
    { "MPGetNextTaskID", mp_MPGetNextTaskID },
    { "MPGetPageSize", mp_MPGetPageSize },
    { "MPGetPageSizeClasses", mp_MPGetPageSizeClasses },
    { "MPGetTaskStorageValue", mp_MPGetTaskStorageValue },
    { "MPModifyNotification", mp_MPModifyNotification },
    { "MPModifyNotificationParameters", mp_MPModifyNotificationParameters },
    { "MPNotifyQueue", mp_MPNotifyQueue },
    { "MPProcessors", mp_MPProcessors },
    { "MPProcessorsScheduled", mp_MPProcessors },
    { "MPRegisterDebugger", mp_MPRegisterDebugger },
    { "MPRemoteCall", mp_MPRemoteCall },
    { "MPSetEvent", mp_MPSetEvent },
    { "MPSetExceptionHandler", mp_MPSetExceptionHandler },
    { "MPSetQueueReserve", mp_MPSetQueueReserve },
    { "MPSetTaskState", mp_task_no_exception },
    { "MPSetTaskStorageValue", mp_MPSetTaskStorageValue },
    { "MPSetTaskType", mp_MPSetTaskType },
    { "MPSetTaskWeight", mp_MPSetTaskWeight },
    { "MPSetTimerNotify", mp_MPSetTimerNotify },
    { "MPSignalSemaphore", mp_MPSignalSemaphore },
    { "MPTaskIsPreemptive", mp_MPTaskIsPreemptive },
    { "MPTerminateTask", mp_MPTerminateTask },
    { "MPThrowException", mp_task_no_exception },
    { "MPUnregisterDebugger", mp_MPRegisterDebugger },
    { "MPWaitForEvent", mp_MPWaitForEvent },
    { "MPWaitOnQueue", mp_MPWaitOnQueue },
    { "MPWaitOnSemaphore", mp_MPWaitOnSemaphore },
    { "MPYield", mp_MPYield },
    { "MPYieldWithHint", mp_MPYield },
    { "_MPAllocateSys", mp_MPAllocate },
    { "_MPIsFullyInitialized", mp_IsFullyInitialized },
    { "_MPLibraryIsCompatible", mp_MPLibraryIsCompatible },
    { "_MPLibraryVersion", mp_MPLibraryVersion },
    { "_MPRPC", mp_MPRemoteCall },
    { "_MPTaskIsToolboxSafe", mp_TaskIsToolboxSafe },
};
const unsigned mp_export_count = sizeof mp_exports / sizeof mp_exports[0];
