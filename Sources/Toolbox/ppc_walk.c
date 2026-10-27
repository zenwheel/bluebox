/*
 * Routines the archive walk's PowerPC programs imported weakly from
 * libraries Blue Box has, and called without checking: CarbonLib's
 * memory checks (IsPointerValid, IsHandleValid, IsHeapValid), UTCUtils
 * (the clock in UTC, and conversions between UTC and local time), and the
 * names CarbonLib 1.0 gave two of the Data Browser's callbacks.
 */
#include <string.h>

#include "ppc_bridge.h"
#include "tb_internal.h"

void (*cfm_export_fn(const char *name))(ppc_cpu *);

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(int32_t)(v); c->pc = c->lr; return; } while (0)

static void IsPointerValid(ppc_cpu *c) { RETURN(mm_pointer_valid(R(0))); }
static void IsHandleValid(ppc_cpu *c) { RETURN(mm_handle_valid(R(0))); }
static void IsHeapValid(ppc_cpu *c) { RETURN(mm_heap_valid()); }

/* UTCUtils.  The clock keeps local time; the machine's location (what
 * ReadLocation answers) says how far that is from UTC. */
enum { kUTCUnderflowErr = -8850, kUTCOverflowErr = -8851 };

static int64_t gmt_delta(void)
{
    int32_t d = machine_gmt_delta() & 0x00FFFFFF;
    return d & 0x00800000 ? d - 0x01000000 : d; /* a signed 24-bit field */
}

/* A UTCDateTime or LocalDateTime: 48 bits of seconds and a fraction. */
static int64_t date_seconds(gaddr p) { return (int64_t)rd16(p) << 32 | rd32(p + 2); }

static int16_t put_date(gaddr p, int64_t secs, uint16_t fraction)
{
    if (secs < 0)
        return kUTCUnderflowErr;
    if (secs >= (int64_t)1 << 48)
        return kUTCOverflowErr;
    wr16(p, (uint16_t)(secs >> 32));
    wr32(p + 2, (uint32_t)secs);
    wr16(p + 6, fraction);
    return noErr;
}

static int16_t put_seconds(gaddr p, int64_t secs)
{
    if (secs < 0)
        return kUTCUnderflowErr;
    if (secs > 0xFFFFFFFFll)
        return kUTCOverflowErr;
    wr32(p, (uint32_t)secs);
    return noErr;
}

static uint32_t local_now(void)
{
    gaddr t = tb_NewPtr(4, false, true);
    tb_GetDateTime(t);
    uint32_t v = rd32(t);
    tb_DisposePtr(t);
    return v;
}

/* GetUTCDateTime(utcDateTime, options) and GetLocalDateTime */
static void GetUTCDateTime(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    RETURN(put_date(R(0), (int64_t)local_now() - gmt_delta(), 0));
}

static void GetLocalDateTime(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    RETURN(put_date(R(0), local_now(), 0));
}

/* SetUTCDateTime(utcDateTime, options) and SetLocalDateTime: the clock
 * holds 32 bits of local seconds. */
static int16_t set_local(int64_t secs)
{
    if (secs < 0)
        return kUTCUnderflowErr;
    if (secs > 0xFFFFFFFFll)
        return kUTCOverflowErr;
    return tb_SetDateTime((uint32_t)secs);
}

static void SetUTCDateTime(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    RETURN(set_local(date_seconds(R(0)) + gmt_delta()));
}

static void SetLocalDateTime(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    RETURN(set_local(date_seconds(R(0))));
}

/* ConvertLocalTimeToUTC(localSeconds, *utcSeconds) and back */
static void ConvertLocalTimeToUTC(ppc_cpu *c)
{
    if (!R(1))
        RETURN(paramErr);
    RETURN(put_seconds(R(1), (int64_t)R(0) - gmt_delta()));
}

static void ConvertUTCToLocalTime(ppc_cpu *c)
{
    if (!R(1))
        RETURN(paramErr);
    RETURN(put_seconds(R(1), (int64_t)R(0) + gmt_delta()));
}

/* ConvertUTCToLocalDateTime(utcDateTime, localDateTime) and back */
static void ConvertUTCToLocalDateTime(ppc_cpu *c)
{
    if (!R(0) || !R(1))
        RETURN(paramErr);
    RETURN(put_date(R(1), date_seconds(R(0)) + gmt_delta(), rd16(R(0) + 6)));
}

static void ConvertLocalToUTCDateTime(ppc_cpu *c)
{
    if (!R(0) || !R(1))
        RETURN(paramErr);
    RETURN(put_date(R(1), date_seconds(R(0)) - gmt_delta(), rd16(R(0) + 6)));
}

/* CarbonLib 1.0's names for the item data and item compare callbacks' UPP
 * calls, which later versions call DataBrowserItemData and
 * DataBrowserItemCompare (ResKnife uses the first ones). */
static void forward(ppc_cpu *c, const char *name)
{
    void (*fn)(ppc_cpu *) = cfm_export_fn(name);
    if (fn)
        fn(c);
    else
        RETURN(0);
}
static void NewDataBrowserGetSetItemDataUPP(ppc_cpu *c) { forward(c, "NewDataBrowserItemDataUPP"); }
static void DisposeDataBrowserGetSetItemDataUPP(ppc_cpu *c) { forward(c, "DisposeDataBrowserItemDataUPP"); }
static void InvokeDataBrowserGetSetItemDataUPP(ppc_cpu *c) { forward(c, "InvokeDataBrowserItemDataUPP"); }
static void NewDataBrowserCompareUPP(ppc_cpu *c) { forward(c, "NewDataBrowserItemCompareUPP"); }
static void DisposeDataBrowserCompareUPP(ppc_cpu *c) { forward(c, "DisposeDataBrowserItemCompareUPP"); }
static void InvokeDataBrowserCompareUPP(ppc_cpu *c) { forward(c, "InvokeDataBrowserItemCompareUPP"); }

const ppc_export walk_exports[] = {
    { "IsPointerValid", IsPointerValid },
    { "IsHandleValid", IsHandleValid },
    { "IsHeapValid", IsHeapValid },
    { "GetUTCDateTime", GetUTCDateTime },
    { "SetUTCDateTime", SetUTCDateTime },
    { "GetLocalDateTime", GetLocalDateTime },
    { "SetLocalDateTime", SetLocalDateTime },
    { "ConvertLocalTimeToUTC", ConvertLocalTimeToUTC },
    { "ConvertUTCToLocalTime", ConvertUTCToLocalTime },
    { "ConvertUTCToLocalDateTime", ConvertUTCToLocalDateTime },
    { "ConvertLocalToUTCDateTime", ConvertLocalToUTCDateTime },
    { "NewDataBrowserGetSetItemDataUPP", NewDataBrowserGetSetItemDataUPP },
    { "DisposeDataBrowserGetSetItemDataUPP", DisposeDataBrowserGetSetItemDataUPP },
    { "InvokeDataBrowserGetSetItemDataUPP", InvokeDataBrowserGetSetItemDataUPP },
    { "NewDataBrowserCompareUPP", NewDataBrowserCompareUPP },
    { "DisposeDataBrowserCompareUPP", DisposeDataBrowserCompareUPP },
    { "InvokeDataBrowserCompareUPP", InvokeDataBrowserCompareUPP },
};
const unsigned walk_export_count = sizeof walk_exports / sizeof walk_exports[0];
