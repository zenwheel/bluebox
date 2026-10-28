/*
 * The Open Scripting Architecture's calls that are component calls and
 * were missing: dialects, the debugger, the context accessors and
 * terminology of ASDebugging.h, AppleScript.h's source styles and init,
 * the generic scripting component's calls, and OSAComp.h's storage types
 * (which only work on a handle).  The component answers (osa.c); the debugger
 * selectors it does not support come back badComponentSelector, as
 * AppleScript's did.
 */
#include "carbonrest.h"

#define T(s) fourcc(s)

/* A call to a component instance (ComponentCallNow): the instance and the
 * arguments (sizes 4 or 2, as a string) pushed, the parameter size and
 * selector last, then _ComponentDispatch with D0 = 0. */
static uint32_t instance_call(ppc_cpu *c, int16_t what, const char *sizes)
{
    ppc_trap_begin();
    ppc_trap_push(4, 0);
    ppc_trap_push(4, A(0));
    uint32_t size = 0;
    for (int i = 0; sizes[i]; i++) {
        int n = sizes[i] - '0';
        ppc_trap_push(n, n == 2 ? A(1 + (unsigned)i) & 0xFFFF : A(1 + (unsigned)i));
        size += n == 4 ? 4 : 2;
    }
    ppc_trap_push(4, size << 16 | (uint16_t)what);
    ppc_trap_set(0, 0);
    ppc_trap(0xA82A);
    uint32_t r = ppc_trap_result(4);
    ppc_trap_end();
    return r;
}

#define CALL(name, sel, sizes) static void os_##name(ppc_cpu *c) { RET(instance_call(c, sel, sizes)); }
/* OSA.h */
CALL(OSASetCurrentDialect, 0x0701, "2")
CALL(OSAGetCurrentDialect, 0x0702, "4")
CALL(OSAAvailableDialects, 0x0703, "4")
CALL(OSAGetDialectInfo, 0x0704, "244")
CALL(OSAAvailableDialectCodeList, 0x0705, "4")
CALL(OSADebuggerCreateSession, 0x0901, "444")
CALL(OSADebuggerGetSessionState, 0x0902, "44")
CALL(OSADebuggerSessionStep, 0x0903, "44")
CALL(OSADebuggerDisposeSession, 0x0904, "4")
CALL(OSADebuggerGetStatementRanges, 0x0905, "44")
CALL(OSADebuggerGetCurrentCallFrame, 0x0906, "44")
CALL(OSADebuggerGetCallFrameState, 0x0907, "44")
CALL(OSADebuggerGetVariable, 0x0908, "444")
CALL(OSADebuggerSetVariable, 0x0909, "444")
CALL(OSADebuggerGetPreviousCallFrame, 0x090A, "44")
CALL(OSADebuggerDisposeCallFrame, 0x090B, "4")
CALL(OSADebuggerGetBreakpoint, 0x0910, "444")
CALL(OSADebuggerSetBreakpoint, 0x0911, "444")
CALL(OSADebuggerGetDefaultBreakpoint, 0x0912, "44")
/* OSAGeneric.h */
CALL(OSAGetDefaultScriptingComponent, 0x1001, "4")
CALL(OSAGetScriptingComponent, 0x1003, "44")
CALL(OSAGetScriptingComponentFromStored, 0x1004, "44")
CALL(OSAGenericToRealID, 0x1005, "44")
CALL(OSARealToGenericID, 0x1006, "44")
/* AppleScript.h */
CALL(ASInit, 0x1001, "4444444")
CALL(ASSetSourceStyles, 0x1002, "4")
CALL(ASGetSourceStyles, 0x1003, "4")
CALL(ASGetSourceStyleNames, 0x1004, "44")
/* ASDebugging.h */
CALL(ASSetProperty, 0x1101, "444")
CALL(ASGetProperty, 0x1102, "444")
CALL(ASSetHandler, 0x1103, "444")
CALL(ASGetHandler, 0x1104, "444")
CALL(ASGetAppTerminology, 0x1105, "4244")
CALL(OSASetProperty, 0x1106, "4444")
CALL(OSAGetProperty, 0x1107, "4444")
CALL(OSASetHandler, 0x1108, "4444")
CALL(OSAGetHandler, 0x1109, "4444")
CALL(OSAGetAppTerminology, 0x110A, "44244")
CALL(OSAGetSysTerminology, 0x110B, "424")
CALL(OSAGetPropertyNames, 0x110C, "444")
CALL(OSAGetHandlerNames, 0x110D, "444")

/* OSAComp.h: the storage type of script data is kept after it, in the
 * handle's last four bytes. */
enum { errOSABadStorageType = -1752 };

static void os_OSAAddStorageType(ppc_cpu *c)
{
    gaddr h = A(0);
    if (!h || !rd32(h))
        RET(errOSABadStorageType);
    int32_t n = tb_GetHandleSize(h);
    tb_SetHandleSize(h, n + 4);
    if (tb_GetHandleSize(h) != n + 4)
        RET(memFullErr);
    wr32(rd32(h) + (uint32_t)n, A(1));
    RET(noErr);
}

static void os_OSAGetStorageType(ppc_cpu *c)
{
    gaddr h = A(0);
    int32_t n = h && rd32(h) ? tb_GetHandleSize(h) : 0;
    if (n < 4 || !A(1))
        RET(errOSABadStorageType);
    wr32(A(1), rd32(rd32(h) + (uint32_t)n - 4));
    RET(noErr);
}

static void os_OSARemoveStorageType(ppc_cpu *c)
{
    gaddr h = A(0);
    int32_t n = h && rd32(h) ? tb_GetHandleSize(h) : 0;
    if (n < 4)
        RET(errOSABadStorageType);
    tb_SetHandleSize(h, n - 4);
    RET(noErr);
}

void rest_osa_reset(void) {}

#define X(name) { #name, os_##name }
const ppc_export rest_osa_exports[] = {
    X(ASGetAppTerminology), X(ASGetHandler), X(ASGetProperty), X(ASGetSourceStyleNames), X(ASGetSourceStyles),
    X(ASInit), X(ASSetHandler), X(ASSetProperty), X(ASSetSourceStyles),
    X(OSAAddStorageType), X(OSAAvailableDialectCodeList), X(OSAAvailableDialects),
    X(OSADebuggerCreateSession), X(OSADebuggerDisposeCallFrame), X(OSADebuggerDisposeSession),
    X(OSADebuggerGetBreakpoint), X(OSADebuggerGetCallFrameState), X(OSADebuggerGetCurrentCallFrame),
    X(OSADebuggerGetDefaultBreakpoint), X(OSADebuggerGetPreviousCallFrame), X(OSADebuggerGetSessionState),
    X(OSADebuggerGetStatementRanges), X(OSADebuggerGetVariable), X(OSADebuggerSessionStep),
    X(OSADebuggerSetBreakpoint), X(OSADebuggerSetVariable),
    X(OSAGenericToRealID), X(OSAGetAppTerminology), X(OSAGetCurrentDialect), X(OSAGetDefaultScriptingComponent),
    X(OSAGetDialectInfo), X(OSAGetHandler), X(OSAGetHandlerNames), X(OSAGetProperty), X(OSAGetPropertyNames),
    X(OSAGetScriptingComponent), X(OSAGetScriptingComponentFromStored), X(OSAGetStorageType),
    X(OSAGetSysTerminology), X(OSARealToGenericID), X(OSARemoveStorageType), X(OSASetCurrentDialect),
    X(OSASetHandler), X(OSASetProperty),
};
const unsigned rest_osa_export_count = sizeof rest_osa_exports / sizeof rest_osa_exports[0];
