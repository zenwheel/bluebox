/*
 * InterfaceLib's utilities from the Driver Services Library and Mac OS 8's
 * memory calls: the time base (UpTime and its conversions), the atomic
 * operations, the Time Manager's PowerPC names, memory that is all real
 * and resident (virtual memory off), and the debugger utility trap.
 *
 * The time base runs at 25 MHz, as on a Mac with a 100 MHz bus: an
 * AbsoluteTime unit is 40 ns of the machine's clock (machine_time_ns), so
 * UpTime keeps step with Ticks and Microseconds, headless or not.
 */
#include <string.h>

#include "m68k.h"
#include "ppc_bridge.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; return; } while (0)

enum {
    NS_PER_ABS = 40,   /* a 25 MHz time base */
    CPU_MHZ = 300,     /* GetCPUSpeed: twelve processor cycles a time base tick */
    durationForever = 0x7FFFFFFF,
};

/* ---------------------------------------------------------------------- */
/* Time */

static uint64_t abs_now(void) { return machine_time_ns() / NS_PER_ABS; }

static uint64_t wide(uint32_t hi, uint32_t lo) { return (uint64_t)hi << 32 | lo; }

static void put_wide(gaddr p, uint64_t v)
{
    if (!p)
        return;
    wr32(p, (uint32_t)(v >> 32));
    wr32(p + 4, (uint32_t)v);
}

static uint64_t abs_to_ns(uint64_t a) { return a > UINT64_MAX / NS_PER_ABS ? UINT64_MAX : a * NS_PER_ABS; }

/* A Duration: positive milliseconds, or negative microseconds when that
 * fits, as Driver Services gives them. */
static int32_t ns_to_duration(uint64_t ns)
{
    uint64_t us = ns / 1000u;
    if (us <= 0x7FFFFFFFu)
        return -(int32_t)us;
    uint64_t ms = ns / 1000000u;
    return ms < durationForever ? (int32_t)ms : durationForever;
}

static uint64_t duration_to_ns(int32_t d)
{
    if (d > 0)
        return (uint64_t)d * 1000000u;
    return (uint64_t)-(int64_t)d * 1000u;
}

/* A struct result comes back through the address the caller passed in r3,
 * the arguments after it; an AbsoluteTime argument takes two registers. */
static void ds_UpTime(ppc_cpu *c)
{
    put_wide(R(0), abs_now());
    c->pc = c->lr;
}
static void ds_AbsoluteToNanoseconds(ppc_cpu *c)
{
    put_wide(R(0), abs_to_ns(wide(R(1), R(2))));
    c->pc = c->lr;
}
static void ds_AbsoluteToDuration(ppc_cpu *c) { RETURN(ns_to_duration(abs_to_ns(wide(R(0), R(1))))); }
static void ds_NanosecondsToAbsolute(ppc_cpu *c)
{
    put_wide(R(0), wide(R(1), R(2)) / NS_PER_ABS);
    c->pc = c->lr;
}
static void ds_DurationToAbsolute(ppc_cpu *c)
{
    put_wide(R(0), duration_to_ns((int32_t)R(1)) / NS_PER_ABS);
    c->pc = c->lr;
}
static void ds_NanosecondsToDuration(ppc_cpu *c) { RETURN(ns_to_duration(wide(R(0), R(1)))); }
static void ds_DurationToNanoseconds(ppc_cpu *c)
{
    put_wide(R(0), duration_to_ns((int32_t)R(1)));
    c->pc = c->lr;
}
static void ds_AddAbsoluteToAbsolute(ppc_cpu *c)
{
    put_wide(R(0), wide(R(1), R(2)) + wide(R(3), R(4)));
    c->pc = c->lr;
}
static void ds_SubAbsoluteFromAbsolute(ppc_cpu *c)
{
    put_wide(R(0), wide(R(1), R(2)) - wide(R(3), R(4)));
    c->pc = c->lr;
}
static void ds_AddDurationToAbsolute(ppc_cpu *c)
{
    put_wide(R(0), wide(R(2), R(3)) + duration_to_ns((int32_t)R(1)) / NS_PER_ABS);
    c->pc = c->lr;
}
static void ds_SubDurationFromAbsolute(ppc_cpu *c)
{
    put_wide(R(0), wide(R(2), R(3)) - duration_to_ns((int32_t)R(1)) / NS_PER_ABS);
    c->pc = c->lr;
}
static void ds_AddNanosecondsToAbsolute(ppc_cpu *c)
{
    put_wide(R(0), wide(R(3), R(4)) + wide(R(1), R(2)) / NS_PER_ABS);
    c->pc = c->lr;
}
static void ds_SubNanosecondsFromAbsolute(ppc_cpu *c)
{
    put_wide(R(0), wide(R(3), R(4)) - wide(R(1), R(2)) / NS_PER_ABS);
    c->pc = c->lr;
}
/* The deltas: left less right, nothing when right is the later. */
static void ds_AbsoluteDeltaToDuration(ppc_cpu *c)
{
    uint64_t l = wide(R(0), R(1)), r = wide(R(2), R(3));
    RETURN(l > r ? ns_to_duration(abs_to_ns(l - r)) : 0);
}
static void ds_AbsoluteDeltaToNanoseconds(ppc_cpu *c)
{
    uint64_t l = wide(R(1), R(2)), r = wide(R(3), R(4));
    put_wide(R(0), l > r ? abs_to_ns(l - r) : 0);
    c->pc = c->lr;
}
/* GetTimeBaseInfo(minDelta, absToNsNum, absToNsDenom, procToAbsNum,
 * procToAbsDenom) */
static void ds_GetTimeBaseInfo(ppc_cpu *c)
{
    static const uint32_t v[5] = { 1, NS_PER_ABS, 1, 1, CPU_MHZ * NS_PER_ABS / 1000 };
    for (int i = 0; i < 5; i++)
        if (R(i))
            wr32(R(i), v[i]);
    c->pc = c->lr;
}

/* GetCPUSpeed: the processor's clock in MHz.  68k code reaches it through
 * _IdleUpdateDispatch ($A485) with D0 = -1; _IdleUpdate ($A285) restarts
 * the Power Manager's idle timer, which on this machine never runs out. */
static void ds_GetCPUSpeed(ppc_cpu *c) { RETURN(CPU_MHZ); }

void raw_IdleUpdate(cpu68k *c, uint16_t tw)
{
    c->d[0] = (tw & 0x0400) && (int32_t)c->d[0] == -1 ? CPU_MHZ : rd32(LM_Ticks);
}

/* The Time Manager by its PowerPC names: _InsTime, _InsXTime, _PrimeTime
 * and _RmvTime, each answering noErr. */
static void ds_InstallTimeTask(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    tb_InsTime(R(0));
    RETURN(noErr);
}
static void ds_InstallXTimeTask(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    tb_InsXTime(R(0));
    RETURN(noErr);
}
static void ds_PrimeTimeTask(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    tb_PrimeTime(R(0), (int32_t)R(1));
    RETURN(noErr);
}
static void ds_RemoveTimeTask(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    tb_RmvTime(R(0));
    RETURN(noErr);
}

/* ---------------------------------------------------------------------- */
/* Atomic operations: one processor, and these calls are never interrupted
 * by guest code, so a read and a write are atomic already.  Each answers
 * the value from before. */

static uint32_t sext(uint32_t v, int size)
{
    return size == 1 ? (uint32_t)(int32_t)(int8_t)v : size == 2 ? (uint32_t)(int32_t)(int16_t)v : v;
}
static uint32_t load(gaddr p, int size) { return size == 1 ? rd8(p) : size == 2 ? rd16(p) : rd32(p); }
static void store(gaddr p, int size, uint32_t v)
{
    if (size == 1)
        wr8(p, (uint8_t)v);
    else if (size == 2)
        wr16(p, (uint16_t)v);
    else
        wr32(p, v);
}

enum { OP_ADD, OP_AND, OP_OR, OP_XOR };

static uint32_t atomic_op(gaddr p, int size, int op, uint32_t arg, bool is_signed)
{
    uint32_t old = load(p, size), v;
    switch (op) {
    case OP_ADD: v = old + arg; break;
    case OP_AND: v = old & arg; break;
    case OP_OR: v = old | arg; break;
    default: v = old ^ arg; break;
    }
    store(p, size, v);
    return is_signed ? sext(old, size) : old;
}

#define ATOMIC(name, size, op, is_signed) \
    static void ds_##name(ppc_cpu *c) { RETURN(atomic_op(R(1), size, op, R(0), is_signed)); }
#define STEP(name, size, by) \
    static void ds_##name(ppc_cpu *c) { RETURN(atomic_op(R(0), size, OP_ADD, (uint32_t)(by), true)); }
ATOMIC(AddAtomic, 4, OP_ADD, true)
ATOMIC(AddAtomic16, 2, OP_ADD, true)
ATOMIC(AddAtomic8, 1, OP_ADD, true)
ATOMIC(BitAndAtomic, 4, OP_AND, false)
ATOMIC(BitAndAtomic16, 2, OP_AND, false)
ATOMIC(BitAndAtomic8, 1, OP_AND, false)
ATOMIC(BitOrAtomic, 4, OP_OR, false)
ATOMIC(BitOrAtomic16, 2, OP_OR, false)
ATOMIC(BitOrAtomic8, 1, OP_OR, false)
ATOMIC(BitXorAtomic, 4, OP_XOR, false)
ATOMIC(BitXorAtomic16, 2, OP_XOR, false)
ATOMIC(BitXorAtomic8, 1, OP_XOR, false)
STEP(IncrementAtomic, 4, 1)
STEP(IncrementAtomic16, 2, 1)
STEP(IncrementAtomic8, 1, 1)
STEP(DecrementAtomic, 4, -1)
STEP(DecrementAtomic16, 2, -1)
STEP(DecrementAtomic8, 1, -1)
#undef ATOMIC
#undef STEP

/* CompareAndSwap(old, new, address): new stored only if old is there. */
static void ds_CompareAndSwap(ppc_cpu *c)
{
    if (rd32(R(2)) != R(0))
        RETURN(0);
    wr32(R(2), R(1));
    RETURN(1);
}

/* TestAndSet / TestAndClear(bit, address): bits numbered as BitTst's, the
 * high bit of the first byte first; the bit's value before. */
static void test_and(ppc_cpu *c, bool set)
{
    gaddr p = R(1) + (R(0) >> 3);
    uint8_t mask = (uint8_t)(0x80u >> (R(0) & 7)), b = rd8(p);
    wr8(p, set ? b | mask : b & (uint8_t)~mask);
    RETURN((b & mask) != 0);
}
static void ds_TestAndSet(ppc_cpu *c) { test_and(c, true); }
static void ds_TestAndClear(ppc_cpu *c) { test_and(c, false); }

/* ---------------------------------------------------------------------- */
/* Memory */

static void ds_BlockZero(ppc_cpu *c)
{
    if ((int32_t)R(1) > 0)
        gmemset(R(0), 0, R(1));
    c->pc = c->lr;
}
/* BlockMoveUncached is BlockMove (code moved is seen by the translators:
 * every store is); BlockMoveDataUncached BlockMoveData. */
static void ds_BlockMoveUncached(ppc_cpu *c)
{
    tb_BlockMove(R(0), R(1), (int32_t)R(2), true);
    c->pc = c->lr;
}
static void ds_BlockMoveDataUncached(ppc_cpu *c)
{
    tb_BlockMove(R(0), R(1), (int32_t)R(2), false);
    c->pc = c->lr;
}

/* PurgeSpaceTotal and PurgeSpaceContiguous: _PurgeSpace's D0 and A0, for
 * the current zone or (Sys) the system's. */
static uint32_t purge_space(bool sys, bool contiguous)
{
    uint32_t total = 0, contig = 0;
    tb_PurgeSpace(&total, &contig, sys);
    return contiguous ? contig : total;
}
static void ds_PurgeSpaceTotal(ppc_cpu *c) { RETURN(purge_space(false, false)); }
static void ds_PurgeSpaceContiguous(ppc_cpu *c) { RETURN(purge_space(false, true)); }
static void ds_PurgeSpaceSysTotal(ppc_cpu *c) { RETURN(purge_space(true, false)); }
static void ds_PurgeSpaceSysContiguous(ppc_cpu *c) { RETURN(purge_space(true, true)); }

/* ReallocateHandleSys: a block lives in its master pointer's zone, so the
 * system bit changes nothing. */
static void ds_ReallocateHandleSys(ppc_cpu *c)
{
    tb_ReallocateHandle(R(0), (int32_t)R(1));
    c->pc = c->lr;
}

/* _MemoryDispatch ($A05C), selector in D0, as on a Mac with virtual memory
 * off: every page real and resident, so holding, locking, making resident
 * or not and flushing have nothing to do, and an address is its own
 * physical address.  A0 and A1 are the arguments; *a0 is what A0 holds
 * after (GetPhysical's count). */
enum { kVolumeVirtualMemoryInfoVersion1 = 1 };

int16_t memory_dispatch(uint16_t sel, uint32_t a0, uint32_t a1, uint32_t *a0_out)
{
    switch (sel) {
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: /* Hold, Unhold, Lock, Unlock, LockContiguous */
    case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: /* LockForOutput, (Non)Resident, Release, Flush */
        return noErr;
    case 0x05: { /* GetPhysical(table, count): the logical range in one physical piece */
        if (!a0 || !a1)
            return paramErr;
        uint32_t len = rd32(a0 + 4);
        wr32(a0 + 8, rd32(a0));
        wr32(a0 + 12, len);
        wr32(a0, rd32(a0) + len); /* the logical range left: none */
        wr32(a0 + 4, 0);
        *a0_out = 1;
        return noErr;
    }
    case 0x0F: /* GetVolumeVirtualMemoryInfo(info): no volume backs memory */
        if (!a0 || rd16(a0) < kVolumeVirtualMemoryInfoVersion1)
            return paramErr;
        wr8(a0 + 4, 0); /* inUse */
        wr8(a0 + 5, 0);
        wr32(a0 + 6, 0); /* vmOptions */
        return noErr;
    }
    return paramErr;
}

static void vm_call(ppc_cpu *c, uint16_t sel)
{
    uint32_t a0 = R(0);
    RETURN((int32_t)memory_dispatch(sel, R(0), R(1), &a0));
}
static void ds_LockMemoryForOutput(ppc_cpu *c) { vm_call(c, 0x0A); }
static void ds_MakeMemoryResident(ppc_cpu *c) { vm_call(c, 0x0B); }
static void ds_ReleaseMemoryData(ppc_cpu *c) { vm_call(c, 0x0C); }
static void ds_MakeMemoryNonResident(ppc_cpu *c) { vm_call(c, 0x0D); }
static void ds_FlushMemory(ppc_cpu *c) { vm_call(c, 0x0E); }
static void ds_GetVolumeVirtualMemoryInfo(ppc_cpu *c) { vm_call(c, 0x0F); }
/* GetPhysical(table, *count): the glue keeps the count in memory. */
static void ds_GetPhysical(ppc_cpu *c)
{
    uint32_t n = R(1) ? rd32(R(1)) : 0;
    int16_t e = n ? memory_dispatch(0x05, R(0), n, &n) : paramErr;
    if (!e)
        wr32(R(1), n);
    RETURN((int32_t)e);
}

/* FlushInstructionCache: translations of code already go when it is
 * stored over (the code map), so there is nothing left to drop. */
static void ds_nothing(ppc_cpu *c) { c->pc = c->lr; }

/* TopMem: the end of RAM. */
gaddr tb_TopMem(void) { return rd32(LM_MemTop); }
/* GetSysPPtr: the low-memory copy of parameter RAM. */
gaddr tb_GetSysPPtr(void) { return LM_SPValid; }

/* ---------------------------------------------------------------------- */
/* _DebugUtil ($A08D): no debugger to enter; with virtual memory off no
 * page is paged and no page fault can be fatal. */
enum { kNotPaged = 2 };

int32_t tb_DebuggerGetMax(void) { return 8; }
void tb_DebuggerEnter(void) {}
void tb_DebuggerExit(void) {}
void tb_DebuggerPoll(void) {}
int16_t tb_GetPageState(gaddr address) { return kNotPaged; }
int8_t tb_PageFaultFatal(void) { return 0; }
int16_t tb_DebuggerLockMemory(gaddr address, uint32_t count) { return noErr; }
int16_t tb_DebuggerUnlockMemory(gaddr address, uint32_t count) { return noErr; }
/* EnterSupervisorMode: the status register from before; programs here run
 * in supervisor mode already. */
int16_t tb_EnterSupervisorMode(void) { return (int16_t)(g_cpu ? g_cpu->sr | 0x2000 : 0x2000); }

/* Debugger68k and DebugStr68k: the 68k debugger, as Debugger and DebugStr
 * from PowerPC code: nothing. */

const ppc_export dsl_exports[] = {
    { "UpTime", ds_UpTime }, { "AbsoluteToNanoseconds", ds_AbsoluteToNanoseconds },
    { "AbsoluteToDuration", ds_AbsoluteToDuration }, { "NanosecondsToAbsolute", ds_NanosecondsToAbsolute },
    { "DurationToAbsolute", ds_DurationToAbsolute }, { "NanosecondsToDuration", ds_NanosecondsToDuration },
    { "DurationToNanoseconds", ds_DurationToNanoseconds }, { "AddAbsoluteToAbsolute", ds_AddAbsoluteToAbsolute },
    { "SubAbsoluteFromAbsolute", ds_SubAbsoluteFromAbsolute }, { "AddDurationToAbsolute", ds_AddDurationToAbsolute },
    { "SubDurationFromAbsolute", ds_SubDurationFromAbsolute },
    { "AddNanosecondsToAbsolute", ds_AddNanosecondsToAbsolute },
    { "SubNanosecondsFromAbsolute", ds_SubNanosecondsFromAbsolute },
    { "AbsoluteDeltaToDuration", ds_AbsoluteDeltaToDuration },
    { "AbsoluteDeltaToNanoseconds", ds_AbsoluteDeltaToNanoseconds }, { "GetTimeBaseInfo", ds_GetTimeBaseInfo },
    { "GetCPUSpeed", ds_GetCPUSpeed },
    { "InstallTimeTask", ds_InstallTimeTask }, { "InstallXTimeTask", ds_InstallXTimeTask },
    { "PrimeTimeTask", ds_PrimeTimeTask }, { "RemoveTimeTask", ds_RemoveTimeTask },
    { "AddAtomic", ds_AddAtomic }, { "AddAtomic16", ds_AddAtomic16 }, { "AddAtomic8", ds_AddAtomic8 },
    { "BitAndAtomic", ds_BitAndAtomic }, { "BitAndAtomic16", ds_BitAndAtomic16 }, { "BitAndAtomic8", ds_BitAndAtomic8 },
    { "BitOrAtomic", ds_BitOrAtomic }, { "BitOrAtomic16", ds_BitOrAtomic16 }, { "BitOrAtomic8", ds_BitOrAtomic8 },
    { "BitXorAtomic", ds_BitXorAtomic }, { "BitXorAtomic16", ds_BitXorAtomic16 }, { "BitXorAtomic8", ds_BitXorAtomic8 },
    { "IncrementAtomic", ds_IncrementAtomic }, { "IncrementAtomic16", ds_IncrementAtomic16 },
    { "IncrementAtomic8", ds_IncrementAtomic8 }, { "DecrementAtomic", ds_DecrementAtomic },
    { "DecrementAtomic16", ds_DecrementAtomic16 }, { "DecrementAtomic8", ds_DecrementAtomic8 },
    { "CompareAndSwap", ds_CompareAndSwap }, { "TestAndSet", ds_TestAndSet }, { "TestAndClear", ds_TestAndClear },
    { "BlockZero", ds_BlockZero }, { "BlockZeroUncached", ds_BlockZero },
    { "BlockMoveUncached", ds_BlockMoveUncached }, { "BlockMoveDataUncached", ds_BlockMoveDataUncached },
    { "PurgeSpaceTotal", ds_PurgeSpaceTotal }, { "PurgeSpaceContiguous", ds_PurgeSpaceContiguous },
    { "PurgeSpaceSysTotal", ds_PurgeSpaceSysTotal }, { "PurgeSpaceSysContiguous", ds_PurgeSpaceSysContiguous },
    { "ReallocateHandleSys", ds_ReallocateHandleSys },
    { "LockMemoryForOutput", ds_LockMemoryForOutput }, { "MakeMemoryResident", ds_MakeMemoryResident },
    { "ReleaseMemoryData", ds_ReleaseMemoryData }, { "MakeMemoryNonResident", ds_MakeMemoryNonResident },
    { "FlushMemory", ds_FlushMemory }, { "GetVolumeVirtualMemoryInfo", ds_GetVolumeVirtualMemoryInfo },
    { "GetPhysical", ds_GetPhysical }, { "FlushInstructionCache", ds_nothing },
    { "Debugger68k", ds_nothing }, { "DebugStr68k", ds_nothing },
};
const unsigned dsl_export_count = sizeof dsl_exports / sizeof dsl_exports[0];
