/*
 * CarbonLib's small additions to older managers, over the code that
 * serves the 68k calls of the same selectors: the Apple Event Manager's
 * descriptor data accessors, the long date and time strings and text
 * comparison (Script Manager's international selectors under the names
 * Carbon gave them), MoreMasterPointers, CallComponentDispatch and the
 * generic scripting component's default.
 */
#include <stdint.h>

#include "ppc_bridge.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; } while (0)

static void AEGetDescData(ppc_cpu *c) { RETURN((int32_t)(int16_t)tb_AEGetDescData(R(0), R(1), (int32_t)R(2))); }
static void AEGetDescDataSize(ppc_cpu *c) { RETURN(tb_AEGetDescDataSize(R(0))); }

/* LongDateString and LongTimeString are IULDateString and IULTimeString (selectors 14h and 16h). */
static void LongDateString(ppc_cpu *c) { tb_IULDateString(R(0), (int8_t)R(1), R(2), R(3)); c->pc = c->lr; }
static void LongTimeString(ppc_cpu *c) { tb_IULTimeString(R(0), (int8_t)R(1), R(2), R(3)); c->pc = c->lr; }

/* CompareText(a, b, aLen, bLen, itl2): IUMagPString's ordering (selector 1Ah). */
static void CompareText(ppc_cpu *c)
{
    RETURN((int32_t)tb_IUMagPString(R(0), R(1), (int16_t)R(2), (int16_t)R(3), R(4)));
}

static void MoreMasterPointers(ppc_cpu *c)
{
    tb_MoreMasterPointers(R(0));
    c->pc = c->lr;
}

/* CallComponentDispatch(cp): the Component Manager's call, as the 68k glue
 * made it, with the ComponentParameters pushed whole (flags and size, the
 * routine selector, the arguments, the instance last). */
static void CallComponentDispatch(ppc_cpu *c)
{
    gaddr cp = R(0);
    uint32_t size = 4 + rd8(cp + 1) + 4; /* the instance follows the parameters */
    ppc_trap_begin();
    ppc_trap_push(4, 0);
    for (uint32_t at = size; at >= 2; at -= 2)
        ppc_trap_push(2, rd16(cp + at - 2));
    ppc_trap_set(0, 0);
    ppc_trap(0xA82A);
    uint32_t r = ppc_trap_result(4);
    ppc_trap_end();
    RETURN(r);
}

/* OSASetDefaultScriptingComponent(generic, subType): the generic scripting
 * component's selector 1002h; the instance is the first argument. */
static void OSASetDefaultScriptingComponent(ppc_cpu *c)
{
    ppc_trap_begin();
    ppc_trap_push(4, 0);
    ppc_trap_push(4, R(0));
    ppc_trap_push(4, R(1));
    ppc_trap_push(4, 4u << 16 | 0x1002);
    ppc_trap_set(0, 0);
    ppc_trap(0xA82A);
    uint32_t r = ppc_trap_result(4);
    ppc_trap_end();
    RETURN(r);
}

const ppc_export carbongaps_exports[] = {
    { "AEGetDescData", AEGetDescData },
    { "AEGetDescDataSize", AEGetDescDataSize },
    { "LongDateString", LongDateString },
    { "LongTimeString", LongTimeString },
    { "CompareText", CompareText },
    { "MoreMasterPointers", MoreMasterPointers },
    { "CallComponentDispatch", CallComponentDispatch },
    { "OSASetDefaultScriptingComponent", OSASetDefaultScriptingComponent },
};
const unsigned carbongaps_export_count = sizeof carbongaps_exports / sizeof carbongaps_exports[0];
