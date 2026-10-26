/*
 * Routines a walk of the Vintage Apple Macintosh archive found programs
 * calling that Blue Box lacked: the Window Manager's layers, Open
 * Transport's timer, deferred and system tasks, and Carbon's names for
 * older international string calls.
 */
#include <string.h>

#include "machine.h"
#include "ppc.h"
#include "ppc_bridge.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; } while (0)

/* ---------------------------------------------------------------------- */
/* Window Manager layers.  System 7 kept each process's windows in a layer
 * (a window record whose children are its windows) under the desk's root
 * layer.  One program runs here, so there is the root and the program's
 * layer, whose windows are the window list; a layer NewLayer makes is a
 * record of its own, the program's windows staying in the window list. */

#define MAX_LAYERS 16
static gaddr layers[MAX_LAYERS]; /* [0] the root, [1] the program's */
static gaddr layer_parent[MAX_LAYERS];
static int nlayers;
static gaddr cur_layer;

static int layer_index(gaddr w)
{
    for (int i = 0; i < nlayers; i++)
        if (w && layers[i] == w)
            return i;
    return -1;
}

static gaddr new_layer(gaddr parent, bool visible, int32_t refcon)
{
    if (nlayers == MAX_LAYERS)
        return 0;
    gaddr l = mm_new_ptr_in(rd32(LM_SysZone), WindowRecord_SIZEOF, true);
    if (!l)
        return 0;
    wr8(l + WindowRecord_visible_off, visible ? 0xFF : 0);
    wr32(l + WindowRecord_refCon_off, (uint32_t)refcon);
    layers[nlayers] = l;
    layer_parent[nlayers++] = parent;
    return l;
}

/* The root and the program's layer, made when first asked for. */
static void layers_init(void)
{
    if (nlayers)
        return;
    gaddr root = new_layer(0, true, 0);
    gaddr app = new_layer(root, true, 0);
    cur_layer = app;
}

int16_t tb_NewLayer(gaddr layer, int8_t visible, int8_t neverActive, gaddr behind, int32_t refCon)
{
    (void)neverActive;
    (void)behind;
    layers_init();
    gaddr l = new_layer(cur_layer, visible != 0, refCon);
    if (layer)
        wr32(layer, l);
    return l ? noErr : memFullErr;
}

gaddr tb_GetCurLayer(void)
{
    layers_init();
    return cur_layer;
}

int16_t tb_SetCurLayer(gaddr layer)
{
    layers_init();
    if (layer_index(layer) < 0)
        return paramErr;
    cur_layer = layer;
    return noErr;
}

gaddr tb_SwapCurLayer(gaddr layer)
{
    gaddr old = tb_GetCurLayer();
    tb_SetCurLayer(layer);
    return old;
}

/* A layer's first child: the root's is the program's layer, the
 * program's the front window. */
gaddr tb_GetSubWindows(gaddr layer)
{
    layers_init();
    int i = layer_index(layer);
    if (i == 1)
        return rd32(LM_WindowList);
    for (int k = 0; k < nlayers; k++)
        if (i >= 0 && layer_parent[k] == layer)
            return layers[k];
    return 0;
}

/* The layer a window or layer is in. */
gaddr tb_GetParent(gaddr window)
{
    layers_init();
    int i = layer_index(window);
    if (i >= 0)
        return layer_parent[i];
    for (gaddr w = rd32(LM_WindowList); w; w = rd32(w + WindowRecord_nextWindow_off))
        if (w == window)
            return layers[1];
    return 0;
}

int8_t tb_IsLayer(gaddr window)
{
    layers_init();
    return layer_index(window) >= 0;
}

/* ---------------------------------------------------------------------- */
/* Open Transport's tasks: a procedure (OTProcessUPP: void (*)(void *arg))
 * run later, at deferred task time.  A timer task runs once its time has
 * come, a deferred or system task at the next chance. */

enum { TASK_TIMER = 1, TASK_DEFERRED, TASK_SYSTEM };
#define MAX_OT_TASKS 64
typedef struct {
    int kind;
    gaddr proc, arg;
    bool scheduled;
    uint64_t due; /* machine_time_ns */
} ot_task;
static ot_task ot_tasks[MAX_OT_TASKS];

static uint32_t ot_task_new(int kind, gaddr proc, gaddr arg)
{
    for (int i = 0; i < MAX_OT_TASKS; i++)
        if (!ot_tasks[i].kind) {
            ot_tasks[i] = (ot_task){ kind, proc, arg, false, 0 };
            return (uint32_t)(0x4F540000u | (uint32_t)(i + 1)); /* 'OT', the index from 1 */
        }
    return 0;
}

static ot_task *ot_task_of(uint32_t id, int kind)
{
    uint32_t i = (id & 0xFFFF) - 1;
    if ((id & 0xFFFF0000u) != 0x4F540000u || i >= MAX_OT_TASKS || ot_tasks[i].kind != kind)
        return NULL;
    return &ot_tasks[i];
}

/* Schedule: true when it was not scheduled already (a timer task's
 * time is moved either way). */
static bool ot_task_schedule(uint32_t id, int kind, uint32_t ms)
{
    ot_task *t = ot_task_of(id, kind);
    if (!t)
        return false;
    bool was = t->scheduled;
    t->scheduled = true;
    if (kind == TASK_TIMER)
        t->due = machine_time_ns() + (uint64_t)ms * 1000000u;
    return kind == TASK_TIMER || !was;
}

static bool ot_task_cancel(uint32_t id, int kind)
{
    ot_task *t = ot_task_of(id, kind);
    bool was = t && t->scheduled;
    if (t)
        t->scheduled = false;
    return was;
}

static int32_t ot_task_destroy(uint32_t id, int kind)
{
    ot_task *t = ot_task_of(id, kind);
    if (!t)
        return -3150; /* kOTBadReferenceErr */
    memset(t, 0, sizeof *t);
    return noErr;
}

/* The tasks whose time has come, from the Time Manager's pump. */
void ot_tasks_pump(void)
{
    uint64_t now = machine_time_ns();
    for (int i = 0; i < MAX_OT_TASKS; i++) {
        ot_task *t = &ot_tasks[i];
        if (!t->kind || !t->scheduled || (t->kind == TASK_TIMER && t->due > now))
            continue;
        t->scheduled = false;
        uint32_t arg = t->arg;
        if (!t->proc)
            continue;
        if (mm_is_descriptor(t->proc))
            mm_call(t->proc, 0x000000C1 /* C, one long */, &arg, 1);
        else
            ppc_call(t->proc, &arg, 1);
    }
}

static void ot_CreateTimerTask(ppc_cpu *c) { RETURN(ot_task_new(TASK_TIMER, R(0), R(1))); }
static void ot_ScheduleTimerTask(ppc_cpu *c) { RETURN(ot_task_schedule(R(0), TASK_TIMER, R(1))); }
static void ot_CancelTimerTask(ppc_cpu *c) { RETURN(ot_task_cancel(R(0), TASK_TIMER)); }
static void ot_DestroyTimerTask(ppc_cpu *c)
{
    ot_task_destroy(R(0), TASK_TIMER);
    c->pc = c->lr;
}
static void ot_CreateDeferredTask(ppc_cpu *c) { RETURN(ot_task_new(TASK_DEFERRED, R(0), R(1))); }
static void ot_ScheduleDeferredTask(ppc_cpu *c) { RETURN(ot_task_schedule(R(0), TASK_DEFERRED, 0)); }
static void ot_DestroyDeferredTask(ppc_cpu *c) { RETURN(ot_task_destroy(R(0), TASK_DEFERRED)); }
static void ot_CreateSystemTask(ppc_cpu *c) { RETURN(ot_task_new(TASK_SYSTEM, R(0), R(1))); }
static void ot_ScheduleSystemTask(ppc_cpu *c) { RETURN(ot_task_schedule(R(0), TASK_SYSTEM, 0)); }
static void ot_CancelSystemTask(ppc_cpu *c) { RETURN(ot_task_cancel(R(0), TASK_SYSTEM)); }
static void ot_DestroySystemTask(ppc_cpu *c) { RETURN(ot_task_destroy(R(0), TASK_SYSTEM)); }

/* OTCompareAndSwap8 and 16 (oldValue, newValue, *dest): Boolean. */
static void ot_CompareAndSwap8(ppc_cpu *c)
{
    bool same = R(2) && rd8(R(2)) == (uint8_t)R(0);
    if (same)
        wr8(R(2), (uint8_t)R(1));
    RETURN(same);
}
static void ot_CompareAndSwap16(ppc_cpu *c)
{
    bool same = R(2) && rd16(R(2)) == (uint16_t)R(0);
    if (same)
        wr16(R(2), (uint16_t)R(1));
    RETURN(same);
}

/* ---------------------------------------------------------------------- */
/* Carbon's names for the International Utilities' Pascal-string calls. */

/* CompareString(a, b, itl2): IUCompPString. */
static void CompareString(ppc_cpu *c)
{
    gaddr a = R(0), b = R(1);
    RETURN((int32_t)tb_IUMagPString(a + 1, b + 1, a ? rd8(a) : 0, b ? rd8(b) : 0, R(2)));
}
/* IdenticalString(a, b, itl2): IUEqualPString's answer, 0 when equal. */
static void IdenticalString(ppc_cpu *c)
{
    gaddr a = R(0), b = R(1);
    RETURN((int32_t)tb_IUMagIDPString(a + 1, b + 1, a ? rd8(a) : 0, b ? rd8(b) : 0, R(2)));
}
/* DateString(dateTime, longFlag, result, intlHandle): IUDatePString. */
static void DateString(ppc_cpu *c)
{
    tb_IUDatePString((int32_t)R(0), (int8_t)R(1), R(2), R(3));
    c->pc = c->lr;
}
/* TimeString(dateTime, wantSeconds, result, intlHandle): IUTimePString. */
static void TimeString(ppc_cpu *c)
{
    tb_IUTimePString((int32_t)R(0), (int8_t)R(1), R(2), R(3));
    c->pc = c->lr;
}

const ppc_export vam_exports[] = {
    { "OTCreateTimerTask", ot_CreateTimerTask }, { "OTCreateTimerTaskInContext", ot_CreateTimerTask },
    { "OTScheduleTimerTask", ot_ScheduleTimerTask }, { "OTCancelTimerTask", ot_CancelTimerTask },
    { "OTDestroyTimerTask", ot_DestroyTimerTask },
    { "OTCreateDeferredTask", ot_CreateDeferredTask }, { "OTCreateDeferredTaskInContext", ot_CreateDeferredTask },
    { "OTScheduleDeferredTask", ot_ScheduleDeferredTask }, { "OTScheduleInterruptTask", ot_ScheduleDeferredTask },
    { "OTDestroyDeferredTask", ot_DestroyDeferredTask },
    { "OTCreateSystemTask", ot_CreateSystemTask }, { "OTScheduleSystemTask", ot_ScheduleSystemTask },
    { "OTCancelSystemTask", ot_CancelSystemTask }, { "OTDestroySystemTask", ot_DestroySystemTask },
    { "OTCompareAndSwap8", ot_CompareAndSwap8 }, { "OTCompareAndSwap16", ot_CompareAndSwap16 },
    { "CompareString", CompareString }, { "IdenticalString", IdenticalString },
    { "DateString", DateString }, { "TimeString", TimeString },
};
const unsigned vam_export_count = sizeof vam_exports / sizeof vam_exports[0];

/* A new run: no layers, no tasks. */
void vam_reset(void)
{
    nlayers = 0;
    cur_layer = 0;
    memset(ot_tasks, 0, sizeof ot_tasks);
}
