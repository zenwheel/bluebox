/*
 * DriverServicesLib as Mac OS 9 exports it, beyond the time and atomic
 * calls (driverservices.c): the string and block calls, pools and
 * physically contiguous memory (the machine's memory is its own physical
 * memory), parameter block queues, software interrupts, secondary interrupt
 * handlers, interrupt timers and interrupt sets.  There is no interrupt
 * hardware, so nothing raises a hardware interrupt: sets and their
 * functions are kept, and software interrupts and timers run from the
 * Time Manager's pump (os9svc_pump).
 */
#include <stdlib.h>
#include <string.h>

#include "machine.h"
#include "os9svc.h"
#include "ppc_bridge.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; return; } while (0)

enum { qErr = -1, noHardwareErr = -200 };
enum { kTaskLevel = 0, kSecondaryInterruptLevel = 5 };
enum { NS_PER_ABS = 40 };

const os9_alias os9_driver_aliases[] = {
    { "BlockCopy", "MPBlockCopy" },
    { "CurrentTaskID", "MPCurrentTaskID" },
    { "SysDebug", "Debugger" },
    { "SysDebugStr", "DebugStr" },
};
const unsigned os9_driver_alias_count = sizeof os9_driver_aliases / sizeof os9_driver_aliases[0];

/* ---- Strings ---- */

static uint32_t cstr_len(gaddr s)
{
    uint32_t n = 0;
    while (rd8(s + n))
        n++;
    return n;
}

static void cstr_put(gaddr dst, const uint8_t *src, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        wr8(dst + i, src[i]);
}

static int cmp_bytes(gaddr a, uint32_t alen, gaddr b, uint32_t blen, uint32_t max)
{
    for (uint32_t i = 0; i < max; i++) {
        int x = i < alen ? rd8(a + i) : -1, y = i < blen ? rd8(b + i) : -1;
        if (x != y)
            return x < y ? -1 : 1;
        if (x < 0)
            break;
    }
    return 0;
}

static void ds_CStrCopy(ppc_cpu *c)
{
    gaddr dst = R(0), src = R(1);
    uint32_t n = cstr_len(src);
    for (uint32_t i = 0; i <= n; i++)
        wr8(dst + i, rd8(src + i));
    RETURN(dst);
}

static void ds_CStrNCopy(ppc_cpu *c)
{
    gaddr dst = R(0), src = R(1);
    uint32_t max = R(2), i = 0;
    for (; i < max; i++) {
        uint8_t b = rd8(src + i);
        wr8(dst + i, b);
        if (!b)
            break;
    }
    RETURN(dst);
}

static void ds_PStrCopy(ppc_cpu *c)
{
    gaddr dst = R(0), src = R(1);
    uint32_t n = rd8(src);
    for (uint32_t i = 0; i <= n; i++)
        wr8(dst + i, rd8(src + i));
    RETURN(dst);
}

static void ds_PStrNCopy(ppc_cpu *c)
{
    gaddr dst = R(0), src = R(1);
    uint32_t n = rd8(src), max = R(2) > 255 ? 255 : R(2);
    if (n > max)
        n = max;
    wr8(dst, (uint8_t)n);
    for (uint32_t i = 1; i <= n; i++)
        wr8(dst + i, rd8(src + i));
    RETURN(dst);
}

static void ds_CStrCat(ppc_cpu *c)
{
    gaddr dst = R(0), src = R(1);
    uint32_t at = cstr_len(dst), n = cstr_len(src);
    for (uint32_t i = 0; i <= n; i++)
        wr8(dst + at + i, rd8(src + i));
    RETURN(dst);
}

static void ds_CStrNCat(ppc_cpu *c)
{
    gaddr dst = R(0), src = R(1);
    uint32_t at = cstr_len(dst), n = cstr_len(src);
    if (n > R(2))
        n = R(2);
    for (uint32_t i = 0; i < n; i++)
        wr8(dst + at + i, rd8(src + i));
    wr8(dst + at + n, 0);
    RETURN(dst);
}

/* Append up to max characters of src to the Pascal string dst, which
 * stays within 255. */
static gaddr pstr_cat(gaddr dst, gaddr src, uint32_t max)
{
    uint32_t at = rd8(dst), n = rd8(src);
    if (n > max)
        n = max;
    if (at + n > 255)
        n = 255 - at;
    for (uint32_t i = 0; i < n; i++)
        wr8(dst + 1 + at + i, rd8(src + 1 + i));
    wr8(dst, (uint8_t)(at + n));
    return dst;
}

static void ds_PStrCat(ppc_cpu *c) { RETURN(pstr_cat(R(0), R(1), 255)); }
static void ds_PStrNCat(ppc_cpu *c) { RETURN(pstr_cat(R(0), R(1), R(2))); }

static void ds_PStrToCStr(ppc_cpu *c)
{
    gaddr dst = R(0), src = R(1);
    uint32_t n = rd8(src);
    for (uint32_t i = 0; i < n; i++)
        wr8(dst + i, rd8(src + 1 + i));
    wr8(dst + n, 0);
    c->pc = c->lr;
}

static void ds_CStrToPStr(ppc_cpu *c)
{
    gaddr dst = R(0), src = R(1);
    uint32_t n = cstr_len(src);
    if (n > 255)
        n = 255;
    for (uint32_t i = 0; i < n; i++)
        wr8(dst + 1 + i, rd8(src + i));
    wr8(dst, (uint8_t)n);
    c->pc = c->lr;
}

static void ds_CStrCmp(ppc_cpu *c) { RETURN((int16_t)cmp_bytes(R(0), cstr_len(R(0)), R(1), cstr_len(R(1)), UINT32_MAX)); }
static void ds_CStrNCmp(ppc_cpu *c) { RETURN((int16_t)cmp_bytes(R(0), cstr_len(R(0)), R(1), cstr_len(R(1)), R(2))); }
static void ds_PStrCmp(ppc_cpu *c) { RETURN((int16_t)cmp_bytes(R(0) + 1, rd8(R(0)), R(1) + 1, rd8(R(1)), UINT32_MAX)); }
static void ds_PStrNCmp(ppc_cpu *c) { RETURN((int16_t)cmp_bytes(R(0) + 1, rd8(R(0)), R(1) + 1, rd8(R(1)), R(2))); }
static void ds_CStrLen(ppc_cpu *c) { RETURN(cstr_len(R(0))); }
static void ds_PStrLen(ppc_cpu *c) { RETURN(rd8(R(0))); }

/* ---- Memory ---- */

/* PoolAllocateResident / MemAllocatePhysicallyContiguous (size, clear):
 * a block of the machine's memory, which is physical memory as it is. */
static void ds_PoolAllocate(ppc_cpu *c) { RETURN(tb_NewPtr((int32_t)R(0), false, (R(1) & 0xFF) != 0)); }

static void ds_PoolDeallocate(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    tb_DisposePtr(R(0));
    RETURN(noErr);
}

static void ds_GetLogicalPageSize(ppc_cpu *c) { RETURN(4096); }
static void ds_GetDataCacheLineSize(ppc_cpu *c) { RETURN(32); }

/* FlushProcessorCache and SetProcessorCacheMode: the machine has no cache
 * of its own to flush or set apart from memory. */
static void ds_NoCache(ppc_cpu *c) { RETURN(noErr); }

/* SynchronizeIO(): the I/O barrier; memory is always in order here. */
static void ds_SynchronizeIO(ppc_cpu *c) { c->pc = c->lr; }

/* DeviceProbe(src, dest, accessType): read 1, 2 or 4 bytes where there may
 * be no device.  Only memory is there. */
static void ds_DeviceProbe(ppc_cpu *c)
{
    gaddr src = R(0), dst = R(1);
    uint32_t size = R(2);
    if (size != 1 && size != 2 && size != 4)
        RETURN(paramErr);
    if (src >= rd32(LM_MemTop) || dst >= rd32(LM_MemTop))
        RETURN(noHardwareErr);
    if (size == 1)
        wr8(dst, rd8(src));
    else if (size == 2)
        wr16(dst, rd16(src));
    else
        wr32(dst, rd32(src));
    RETURN(noErr);
}

/* ---- Time ---- */

/* Wait for a number of time base units: the Multiprocessing delay on the
 * machine's clock. */
static int32_t delay_abs(ppc_cpu *c, uint64_t units)
{
    void (*delay)(ppc_cpu *) = cfm_export_fn("MPDelayUntil");
    gaddr t = tb_NewPtr(8, false, true);
    if (!delay || !t)
        return memFullErr;
    uint64_t until = machine_time_ns() / NS_PER_ABS + units;
    wr32(t, (uint32_t)(until >> 32));
    wr32(t + 4, (uint32_t)until);
    uint32_t args[2] = { c->gpr[3], c->gpr[4] }, pc = c->pc;
    c->gpr[3] = t;
    delay(c);
    int32_t r = (int32_t)c->gpr[3];
    c->gpr[3] = args[0];
    c->gpr[4] = args[1];
    c->pc = pc;
    tb_DisposePtr(t);
    return r;
}

static uint64_t duration_units(int32_t d)
{
    uint64_t ns = d > 0 ? (uint64_t)d * 1000000u : (uint64_t)-(int64_t)d * 1000u;
    return ns / NS_PER_ABS;
}

static void ds_DelayFor(ppc_cpu *c)
{
    int32_t d = (int32_t)R(0);
    if (d == 0x7FFFFFFF)
        RETURN(paramErr);
    RETURN(delay_abs(c, duration_units(d)));
}

/* DelayForHardware(absoluteTime): a delay as a time base count (a 64-bit
 * value in r3 and r4). */
static void ds_DelayForHardware(ppc_cpu *c) { RETURN(delay_abs(c, (uint64_t)R(0) << 32 | R(1))); }

/* ---- I/O preparation and page information ---- */

/* PrepareMemoryForIO(table): the memory's physical pages, which are its
 * logical ones.  IOPreparationTable: options 0, state 4, preparationID 8,
 * addressSpace 12, granularity 16, firstPrepared 20, lengthPrepared 24,
 * mappingEntryCount 28, logicalMapping 32, physicalMapping 36, then one
 * range (base, length) or several (count, table of ranges). */
enum { kIOMultipleRanges = 1, kIOStateDone = 1, PAGE = 4096 };

#define MAX_PREPARATIONS 64
static uint32_t preparations[MAX_PREPARATIONS];
static uint32_t next_preparation = 1;

static void ds_PrepareMemoryForIO(ppc_cpu *c)
{
    gaddr t = R(0);
    if (!t)
        RETURN(paramErr);
    uint32_t options = rd32(t), capacity = rd32(t + 28), entries = 0, covered = 0, total = 0;
    gaddr logical = rd32(t + 32), physical = rd32(t + 36);
    uint32_t nranges = options & kIOMultipleRanges ? rd32(t + 40) : 1;
    gaddr table = options & kIOMultipleRanges ? rd32(t + 44) : 0;
    if (nranges > 1024 || (nranges > 1 && !table))
        RETURN(paramErr);
    bool full = false;
    for (uint32_t r = 0; r < nranges && !full; r++) {
        gaddr base = table ? rd32(table + r * 8) : rd32(t + 40);
        uint32_t len = table ? rd32(table + r * 8 + 4) : rd32(t + 44);
        total += len;
        if (base + len > rd32(LM_MemTop) || base + len < base)
            RETURN(paramErr);
        gaddr page = base & ~(gaddr)(PAGE - 1);
        for (; page < base + len; page += PAGE) {
            if (logical || physical) {
                if (entries == capacity) {
                    full = true;
                    break;
                }
                if (logical)
                    wr32(logical + entries * 4, page);
                if (physical)
                    wr32(physical + entries * 4, page);
            }
            entries++;
            uint32_t end = page + PAGE < base + len ? page + PAGE : base + len;
            covered += end - (page < base ? base : page);
        }
    }
    uint32_t id = next_preparation++;
    for (int i = 0; i < MAX_PREPARATIONS; i++)
        if (!preparations[i]) {
            preparations[i] = id;
            break;
        }
    wr32(t + 8, id);
    wr32(t + 20, 0);
    wr32(t + 24, covered);
    wr32(t + 28, entries);
    wr32(t + 4, covered == total ? kIOStateDone : 0);
    RETURN(noErr);
}

/* CheckpointIO(preparationID, options): the preparation is finished
 * unless more transfers follow (kMoreIOTransfers). */
static void ds_CheckpointIO(ppc_cpu *c)
{
    for (int i = 0; i < MAX_PREPARATIONS; i++)
        if (preparations[i] && preparations[i] == R(0)) {
            if (!(R(1) & 4))
                preparations[i] = 0;
            RETURN(noErr);
        }
    RETURN(paramErr);
}

/* GetPageInformation(space, base, length, version, *info): PageInformation
 * is the area (0), the count of pages (in: room for, out: given) and a
 * state word each. */
static void ds_GetPageInformation(ppc_cpu *c)
{
    gaddr base = R(1), info = R(4);
    uint32_t len = R(2), version = R(3);
    if (!info || version > 1)
        RETURN(paramErr);
    gaddr first = base & ~(gaddr)(PAGE - 1);
    uint32_t pages = (base + len - first + PAGE - 1) / PAGE, room = rd32(info + 4);
    if (!len)
        pages = 0;
    if (pages > room)
        pages = room;
    for (uint32_t i = 0; i < pages; i++)
        wr32(info + 8 + i * 4, first + i * PAGE < rd32(LM_MemTop) ? 0x20 /* kPageIsInMemory */ : 0);
    wr32(info, 0);
    wr32(info + 4, pages);
    RETURN(noErr);
}

/* ---- Parameter block queues ---- */

enum { QHdr_head = 2, QHdr_tail = 6, QHdr_SIZE = 10, QElem_link = 0 };

static void ds_PBQueueInit(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    wr16(R(0), 0);
    wr32(R(0) + QHdr_head, 0);
    wr32(R(0) + QHdr_tail, 0);
    RETURN(noErr);
}

static void ds_PBQueueCreate(ppc_cpu *c)
{
    gaddr q = tb_NewPtr(QHdr_SIZE, false, true);
    if (!R(0))
        RETURN(paramErr);
    if (!q)
        RETURN(memFullErr);
    wr32(R(0), q);
    RETURN(noErr);
}

static void ds_PBQueueDelete(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    tb_DisposePtr(R(0));
    RETURN(noErr);
}

/* PBEnqueue and PBEnqueueLast add at the end, as _Enqueue does. */
static void ds_PBEnqueue(ppc_cpu *c)
{
    if (R(0) && R(1))
        tb_Enqueue(R(0), R(1));
    c->pc = c->lr;
}

static void ds_PBEnqueueLast(ppc_cpu *c)
{
    if (!R(0) || !R(1))
        RETURN(paramErr);
    tb_Enqueue(R(0), R(1));
    RETURN(noErr);
}

static void ds_PBDequeue(ppc_cpu *c)
{
    if (!R(0) || !R(1))
        RETURN(paramErr);
    RETURN(tb_Dequeue(R(0), R(1)));
}

/* Remove the first or last element, and give it: qErr for an empty queue. */
static void dequeue_end(ppc_cpu *c, bool last)
{
    gaddr q = R(0), out = R(1), e = q ? rd32(q + (last ? QHdr_tail : QHdr_head)) : 0;
    if (!q || !out)
        RETURN(paramErr);
    wr32(out, e);
    if (!e)
        RETURN(qErr);
    RETURN(tb_Dequeue(e, q));
}

static void ds_PBDequeueFirst(ppc_cpu *c) { dequeue_end(c, false); }
static void ds_PBDequeueLast(ppc_cpu *c) { dequeue_end(c, true); }

/* ---- Software interrupts, secondary handlers and timers ---- */

static uint32_t exec_level = kTaskLevel;

static void ds_CurrentExecutionLevel(ppc_cpu *c) { RETURN(exec_level); }

/* TaskLevel's answer (DebugLib): 0 at task level, else the masks of
 * Debugging.h; the handlers here run as secondary interrupt handlers. */
uint32_t os9svc_task_level(void) { return exec_level == kTaskLevel ? 0 : 0x40; }

/* Call a handler (void *p1, void *p2) at secondary interrupt level. */
static uint32_t call_handler(gaddr handler, uint32_t p1, uint32_t p2)
{
    uint32_t args[2] = { p1, p2 };
    uint32_t saved = exec_level, r;
    exec_level = kSecondaryInterruptLevel;
    r = mm_is_descriptor(handler) ? mm_call(handler, 0x3F1, args, 2) : ppc_call(handler, args, 2);
    exec_level = saved;
    return r;
}

#define MAX_SOFT 32
static struct { bool used, persistent; gaddr handler; uint32_t p1; } softs[MAX_SOFT];

/* CreateSoftwareInterrupt(handler, task, p1, persistent, *id) */
static void ds_CreateSoftwareInterrupt(ppc_cpu *c)
{
    if (!R(0) || !R(4))
        RETURN(paramErr);
    for (int i = 0; i < MAX_SOFT; i++)
        if (!softs[i].used) {
            softs[i].used = true;
            softs[i].handler = R(0);
            softs[i].p1 = R(2);
            softs[i].persistent = (R(3) & 0xFF) != 0;
            wr32(R(4), 0x44530000u | (uint32_t)(i + 1));
            RETURN(noErr);
        }
    RETURN(memFullErr);
}

static int soft_index(uint32_t id)
{
    uint32_t i = (id & 0xFFFF) - 1;
    return (id & 0xFFFF0000u) == 0x44530000u && i < MAX_SOFT && softs[i].used ? (int)i : -1;
}

/* SendSoftwareInterrupt(id, p2): the handler runs (a one-shot interrupt
 * is used up). */
static void ds_SendSoftwareInterrupt(ppc_cpu *c)
{
    int i = soft_index(R(0));
    if (i < 0)
        RETURN(paramErr);
    gaddr handler = softs[i].handler;
    uint32_t p1 = softs[i].p1;
    if (!softs[i].persistent)
        softs[i].used = false;
    uint32_t saved = exec_level;
    uint32_t args[2] = { p1, R(1) };
    exec_level = 1; /* kSoftwareInterruptLevel */
    if (mm_is_descriptor(handler))
        mm_call(handler, 0x3C1, args, 2);
    else
        ppc_call(handler, args, 2);
    exec_level = saved;
    RETURN(noErr);
}

static void ds_DeleteSoftwareInterrupt(ppc_cpu *c)
{
    int i = soft_index(R(0));
    if (i < 0)
        RETURN(paramErr);
    softs[i].used = false;
    RETURN(noErr);
}

/* CallSecondaryInterruptHandler2(handler, exceptionHandler, p1, p2) */
static void ds_CallSecondary(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    RETURN(call_handler(R(0), R(2), R(3)));
}

#define MAX_QUEUED 32
static struct { gaddr handler; uint32_t p1, p2; } queued[MAX_QUEUED];
static int nqueued;

/* QueueSecondaryInterruptHandler(handler, exceptionHandler, p1, p2): runs
 * at the next chance. */
static void ds_QueueSecondary(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    if (nqueued == MAX_QUEUED)
        RETURN(memFullErr);
    queued[nqueued].handler = R(0);
    queued[nqueued].p1 = R(2);
    queued[nqueued++].p2 = R(3);
    RETURN(noErr);
}

#define MAX_TIMERS 32
static struct {
    bool used, persistent;
    gaddr handler;
    uint32_t p1;
    uint64_t due, period; /* time base units */
} timers[MAX_TIMERS];

static int timer_index(uint32_t id)
{
    uint32_t i = (id & 0xFFFF) - 1;
    return (id & 0xFFFF0000u) == 0x44540000u && i < MAX_TIMERS && timers[i].used ? (int)i : -1;
}

static int timer_new(gaddr handler, uint32_t p1, uint64_t due, uint64_t period, bool persistent)
{
    for (int i = 0; i < MAX_TIMERS; i++)
        if (!timers[i].used) {
            timers[i].used = true;
            timers[i].handler = handler;
            timers[i].p1 = p1;
            timers[i].due = due;
            timers[i].period = period;
            timers[i].persistent = persistent;
            return i;
        }
    return -1;
}

static uint64_t abs_now(void) { return machine_time_ns() / NS_PER_ABS; }

/* SetInterruptTimer(&expirationTime, handler, p1, *timer) */
static void ds_SetInterruptTimer(ppc_cpu *c)
{
    if (!R(0) || !R(1) || !R(3))
        RETURN(paramErr);
    int i = timer_new(R(1), R(2), (uint64_t)rd32(R(0)) << 32 | rd32(R(0) + 4), 0, false);
    if (i < 0)
        RETURN(memFullErr);
    wr32(R(3), 0x44540000u | (uint32_t)(i + 1));
    RETURN(noErr);
}

/* SetPersistentTimer(frequency, handler, p1, *timer) */
static void ds_SetPersistentTimer(ppc_cpu *c)
{
    int32_t d = (int32_t)R(0);
    if (!R(1) || !R(3) || d == 0 || d == 0x7FFFFFFF)
        RETURN(paramErr);
    uint64_t period = duration_units(d);
    if (!period)
        period = 1;
    int i = timer_new(R(1), R(2), abs_now() + period, period, true);
    if (i < 0)
        RETURN(memFullErr);
    wr32(R(3), 0x44540000u | (uint32_t)(i + 1));
    RETURN(noErr);
}

/* CancelTimer(timer, *timeRemaining): what was left of the time. */
static void ds_CancelTimer(ppc_cpu *c)
{
    int i = timer_index(R(0));
    if (i < 0)
        RETURN(paramErr);
    if (R(1)) {
        uint64_t now = abs_now(), left = timers[i].due > now ? timers[i].due - now : 0;
        wr32(R(1), (uint32_t)(left >> 32));
        wr32(R(1) + 4, (uint32_t)left);
    }
    timers[i].used = false;
    RETURN(noErr);
}

/* From the Time Manager's pump: queued handlers, then timers due. */
void os9svc_pump(void)
{
    while (nqueued) {
        gaddr h = queued[0].handler;
        uint32_t p1 = queued[0].p1, p2 = queued[0].p2;
        memmove(&queued[0], &queued[1], (size_t)(--nqueued) * sizeof queued[0]);
        call_handler(h, p1, p2);
    }
    uint64_t now = abs_now();
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!timers[i].used || timers[i].due > now)
            continue;
        gaddr h = timers[i].handler;
        uint32_t p1 = timers[i].p1;
        if (timers[i].persistent)
            timers[i].due += timers[i].period;
        else
            timers[i].used = false;
        call_handler(h, p1, 0);
    }
}

/* ---- Interrupt sets ---- */

#define MAX_SETS 16
#define MAX_MEMBERS 32
typedef struct {
    bool used;
    int32_t size;
    uint32_t options;
    struct { gaddr refcon, handler, enabler, disabler; } member[MAX_MEMBERS];
    uint32_t parent;     /* the set this one is a member of, and which member */
    int32_t parent_member;
    bool tree;           /* a root made a source of interrupts */
    int32_t vector;      /* the hardware vector it is active on; 0 when inactive */
} intset;
static intset sets[MAX_SETS];

static intset *set_of(uint32_t id)
{
    uint32_t i = (id & 0xFFFF) - 1;
    return (id & 0xFFFF0000u) == 0x49530000u && i < MAX_SETS && sets[i].used ? &sets[i] : NULL;
}

/* CreateInterruptSet(parentSet, parentMember, setSize, *setID, options) */
static void ds_CreateInterruptSet(ppc_cpu *c)
{
    int32_t size = (int32_t)R(2);
    if (!R(3) || size < 1 || size > MAX_MEMBERS)
        RETURN(paramErr);
    if (R(0) && (!set_of(R(0)) || (int32_t)R(1) < 1 || (int32_t)R(1) > set_of(R(0))->size))
        RETURN(paramErr);
    for (int i = 0; i < MAX_SETS; i++)
        if (!sets[i].used) {
            memset(&sets[i], 0, sizeof sets[i]);
            sets[i].used = true;
            sets[i].size = size;
            sets[i].options = R(4);
            sets[i].parent = R(0);
            sets[i].parent_member = (int32_t)R(1);
            wr32(R(3), 0x49530000u | (uint32_t)(i + 1));
            RETURN(noErr);
        }
    RETURN(memFullErr);
}

static void ds_DeleteInterruptSet(ppc_cpu *c)
{
    intset *s = set_of(R(0));
    if (!s)
        RETURN(paramErr);
    s->used = false;
    RETURN(noErr);
}

/* InstallInterruptFunctions(setID, member, refCon, handler, enabler, disabler) */
static void ds_InstallInterruptFunctions(ppc_cpu *c)
{
    intset *s = set_of(R(0));
    int32_t m = (int32_t)R(1);
    if (!s || m < 1 || m > s->size)
        RETURN(paramErr);
    s->member[m - 1].refcon = R(2);
    s->member[m - 1].handler = R(3);
    s->member[m - 1].enabler = R(4);
    s->member[m - 1].disabler = R(5);
    RETURN(noErr);
}

/* GetInterruptFunctions(setID, member, *refCon, *handler, *enabler, *disabler) */
static void ds_GetInterruptFunctions(ppc_cpu *c)
{
    intset *s = set_of(R(0));
    int32_t m = (int32_t)R(1);
    if (!s || m < 1 || m > s->size)
        RETURN(paramErr);
    if (R(2))
        wr32(R(2), s->member[m - 1].refcon);
    if (R(3))
        wr32(R(3), s->member[m - 1].handler);
    if (R(4))
        wr32(R(4), s->member[m - 1].enabler);
    if (R(5))
        wr32(R(5), s->member[m - 1].disabler);
    RETURN(noErr);
}

/* RemoveInterruptFunctions(setID, member) */
static void ds_RemoveInterruptFunctions(ppc_cpu *c)
{
    intset *s = set_of(R(0));
    int32_t m = (int32_t)R(1);
    if (!s || m < 1 || m > s->size)
        RETURN(paramErr);
    memset(&s->member[m - 1], 0, sizeof s->member[0]);
    RETURN(noErr);
}

static void ds_ChangeInterruptSetOptions(ppc_cpu *c)
{
    intset *s = set_of(R(0));
    if (!s)
        RETURN(paramErr);
    s->options = R(1);
    RETURN(noErr);
}

static void ds_GetInterruptSetOptions(ppc_cpu *c)
{
    intset *s = set_of(R(0));
    if (!s || !R(1))
        RETURN(paramErr);
    wr32(R(1), s->options);
    RETURN(noErr);
}

/* ---- Interrupt source trees ---- */

/* A root set that interrupts arrive at, through one of the interrupt
 * controller's vectors: a tree is made for the set, activated to be given
 * a vector and deactivated to give it back.  Mac OS 9 has room for 24. */
enum { kMaxTrees = 24, kTooManyTreesErr = -2420, kTreeActiveErr = -2416, kTreeNotActiveErr = -2421,
       kEmptyTreeErr = -2401 };

static intset *tree_of(uint32_t id)
{
    intset *s = set_of(id);
    return s && s->tree ? s : NULL;
}

/* CreateInterruptSourceTree(rootSet) */
static void ds_CreateInterruptSourceTree(ppc_cpu *c)
{
    intset *s = set_of(R(0));
    if (!s || s->parent)
        RETURN(paramErr);
    int trees = 0;
    for (int i = 0; i < MAX_SETS; i++)
        trees += sets[i].used && sets[i].tree;
    if (!s->tree && trees >= kMaxTrees)
        RETURN(kTooManyTreesErr);
    s->tree = true;
    s->vector = 0;
    RETURN(noErr);
}

/* ActivateInterruptSourceTree(rootSet): the lowest vector no tree has. */
static void ds_ActivateInterruptSourceTree(ppc_cpu *c)
{
    intset *s = tree_of(R(0));
    if (!s)
        RETURN(paramErr);
    if (s->vector)
        RETURN(kTreeActiveErr);
    bool any = false;
    for (int m = 0; m < s->size; m++)
        any |= s->member[m].handler != 0;
    if (!any)
        RETURN(kEmptyTreeErr);
    for (int v = 1; v <= kMaxTrees; v++) {
        bool taken = false;
        for (int i = 0; i < MAX_SETS; i++)
            taken |= sets[i].used && sets[i].tree && sets[i].vector == v;
        if (!taken) {
            s->vector = v;
            RETURN(noErr);
        }
    }
    RETURN(kTooManyTreesErr);
}

static void ds_DeactivateInterruptSourceTree(ppc_cpu *c)
{
    intset *s = tree_of(R(0));
    if (!s)
        RETURN(paramErr);
    if (!s->vector)
        RETURN(kTreeNotActiveErr);
    s->vector = 0;
    RETURN(noErr);
}

/* GetInterruptSourceTreeVector(rootSet, *vector): -1 when not active. */
static void ds_GetInterruptSourceTreeVector(ppc_cpu *c)
{
    intset *s = tree_of(R(0));
    if (!s || !R(1))
        RETURN(paramErr);
    wr32(R(1), s->vector ? (uint32_t)s->vector : 0xFFFFFFFFu);
    RETURN(noErr);
}

/* LookupRootSet(set, *root): the set at the top of the one it is in. */
static void ds_LookupRootSet(ppc_cpu *c)
{
    intset *s = set_of(R(0));
    uint32_t id = R(0);
    if (!s || !R(1))
        RETURN(paramErr);
    while (s->parent && set_of(s->parent)) {
        id = s->parent;
        s = set_of(id);
    }
    wr32(R(1), id);
    RETURN(noErr);
}

/* LookupParentMember(set, *parentSet, *parentMember): 0 and 0 for a root. */
static void ds_LookupParentMember(ppc_cpu *c)
{
    intset *s = set_of(R(0));
    if (!s || !R(1) || !R(2))
        RETURN(paramErr);
    bool has = s->parent && set_of(s->parent);
    wr32(R(1), has ? s->parent : 0);
    wr32(R(2), has ? (uint32_t)s->parent_member : 0);
    RETURN(noErr);
}

/* ---- I/O commands ---- */

/* An I/O command is a record the Device Manager makes for a driver's
 * entry point: the command code in the low byte of the word at +6 and
 * its kind in the high byte, the result stored at +8 when the driver
 * reports it done (and the flag at +10 that it has). */
static void ds_IOCommandIsComplete(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    wr16(R(0) + 8, (uint16_t)R(1));
    wr16(R(0) + 10, 1);
    RETURN(noErr);
}

/* GetIOCommandInfo(id, *contents, *command, *kind) */
static void ds_GetIOCommandInfo(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    uint16_t w = rd16(R(0) + 6);
    if (R(1))
        wr32(R(1), R(0));
    if (R(2))
        wr32(R(2), w & 0xFF);
    if (R(3))
        wr32(R(3), w >> 8);
    RETURN(noErr);
}

/* DSL_Init(): the library's own start-up, done by the time a program runs. */
static void ds_DSL_Init(ppc_cpu *c) { RETURN(noErr); }

void os9svc_driver_reset(void)
{
    memset(softs, 0, sizeof softs);
    memset(timers, 0, sizeof timers);
    memset(sets, 0, sizeof sets);
    memset(preparations, 0, sizeof preparations);
    next_preparation = 1;
    nqueued = 0;
    exec_level = kTaskLevel;
}

const ppc_export os9_driver_exports[] = {
    { "CStrCopy", ds_CStrCopy }, { "CStrNCopy", ds_CStrNCopy }, { "PStrCopy", ds_PStrCopy },
    { "PStrNCopy", ds_PStrNCopy }, { "CStrCat", ds_CStrCat }, { "CStrNCat", ds_CStrNCat },
    { "PStrCat", ds_PStrCat }, { "PStrNCat", ds_PStrNCat }, { "PStrToCStr", ds_PStrToCStr },
    { "CStrToPStr", ds_CStrToPStr }, { "CStrCmp", ds_CStrCmp }, { "CStrNCmp", ds_CStrNCmp },
    { "PStrCmp", ds_PStrCmp }, { "PStrNCmp", ds_PStrNCmp }, { "CStrLen", ds_CStrLen }, { "PStrLen", ds_PStrLen },
    { "PoolAllocateResident", ds_PoolAllocate }, { "PoolDeallocate", ds_PoolDeallocate },
    { "MemAllocatePhysicallyContiguous", ds_PoolAllocate },
    { "MemDeallocatePhysicallyContiguous", ds_PoolDeallocate },
    { "GetLogicalPageSize", ds_GetLogicalPageSize }, { "GetDataCacheLineSize", ds_GetDataCacheLineSize },
    { "FlushProcessorCache", ds_NoCache }, { "SetProcessorCacheMode", ds_NoCache },
    { "SynchronizeIO", ds_SynchronizeIO }, { "DeviceProbe", ds_DeviceProbe },
    { "DelayFor", ds_DelayFor }, { "DelayForHardware", ds_DelayForHardware },
    { "PBQueueInit", ds_PBQueueInit }, { "PBQueueCreate", ds_PBQueueCreate }, { "PBQueueDelete", ds_PBQueueDelete },
    { "PBEnqueue", ds_PBEnqueue }, { "PBEnqueueLast", ds_PBEnqueueLast }, { "PBDequeue", ds_PBDequeue },
    { "PBDequeueFirst", ds_PBDequeueFirst }, { "PBDequeueLast", ds_PBDequeueLast },
    { "PrepareMemoryForIO", ds_PrepareMemoryForIO }, { "CheckpointIO", ds_CheckpointIO },
    { "GetPageInformation", ds_GetPageInformation },
    { "RemoveInterruptFunctions", ds_RemoveInterruptFunctions },
    { "CreateInterruptSourceTree", ds_CreateInterruptSourceTree },
    { "ActivateInterruptSourceTree", ds_ActivateInterruptSourceTree },
    { "DeactivateInterruptSourceTree", ds_DeactivateInterruptSourceTree },
    { "GetInterruptSourceTreeVector", ds_GetInterruptSourceTreeVector },
    { "LookupRootSet", ds_LookupRootSet }, { "LookupParentMember", ds_LookupParentMember },
    { "IOCommandIsComplete", ds_IOCommandIsComplete }, { "GetIOCommandInfo", ds_GetIOCommandInfo },
    { "DSL_Init", ds_DSL_Init },
    { "CurrentExecutionLevel", ds_CurrentExecutionLevel },
    { "CreateSoftwareInterrupt", ds_CreateSoftwareInterrupt },
    { "SendSoftwareInterrupt", ds_SendSoftwareInterrupt },
    { "DeleteSoftwareInterrupt", ds_DeleteSoftwareInterrupt },
    { "CallSecondaryInterruptHandler2", ds_CallSecondary },
    { "QueueSecondaryInterruptHandler", ds_QueueSecondary },
    { "SetInterruptTimer", ds_SetInterruptTimer }, { "SetPersistentTimer", ds_SetPersistentTimer },
    { "CancelTimer", ds_CancelTimer },
    { "CreateInterruptSet", ds_CreateInterruptSet }, { "DeleteInterruptSet", ds_DeleteInterruptSet },
    { "InstallInterruptFunctions", ds_InstallInterruptFunctions },
    { "GetInterruptFunctions", ds_GetInterruptFunctions },
    { "ChangeInterruptSetOptions", ds_ChangeInterruptSetOptions },
    { "GetInterruptSetOptions", ds_GetInterruptSetOptions },
};
const unsigned os9_driver_export_count = sizeof os9_driver_exports / sizeof os9_driver_exports[0];

/* LookupInterruptMembers(setID, firstMember, count, records): the
 * functions of `count` members from the first (as GetInterruptFunctions
 * gives one's), each 4 longs: refCon, handler, enabler, disabler
 * (os9left_misc.c). */
int32_t os9svc_lookup_members(uint32_t setID, int32_t first, int32_t count, gaddr out)
{
    intset *s = set_of(setID);
    if (!s || first < 1 || count < 1 || first + count - 1 > s->size || !out)
        return paramErr;
    for (int32_t i = 0; i < count; i++) {
        gaddr r = out + 16u * (uint32_t)i;
        wr32(r, s->member[first - 1 + i].refcon);
        wr32(r + 4, s->member[first - 1 + i].handler);
        wr32(r + 8, s->member[first - 1 + i].enabler);
        wr32(r + 12, s->member[first - 1 + i].disabler);
    }
    return noErr;
}
