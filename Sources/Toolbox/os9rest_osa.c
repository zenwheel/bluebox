/*
 * AppleScriptLib's calls beyond ppc_libs.c's: the Open Scripting
 * Architecture's dialects, copies, the debugging calls (properties,
 * handlers and terminology), the generic scripting component's calls and
 * AppleScript's own (initialisation, source styles), each a call to the
 * scripting component instance it is given (osa.c has the component); and
 * the storage type OSAComp.h puts on a script's data, a 12-byte trailer:
 * the type, a version (1), 2 spare bytes and the mark 0xFADEDEAD.
 */
#include <string.h>

#include "os9rest.h"
#include "ppc_bridge.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(int32_t)(v); c->pc = c->lr; return; } while (0)

enum { errOSABadStorageType = -1752, typeOSAGenericStorage = 0x73637074 /* 'scpt' */ };
enum { TRAILER = 12, TRAILER_MARK = 0xFADEDEADu };

/* A component call (ComponentCallNow) on the instance in r3: the
 * parameters (sizes 4 or 2), the selector. */
static void instance_call(ppc_cpu *c, int16_t what, const char *args)
{
    ppc_trap_begin();
    ppc_trap_push(4, 0);
    ppc_trap_push(4, R(0));
    uint32_t size = 0;
    for (int i = 0; args[i]; i++) {
        int n = args[i] - '0';
        ppc_trap_push(n, R(1 + i));
        size += n == 4 ? 4 : 2;
    }
    ppc_trap_push(4, size << 16 | (uint16_t)what);
    ppc_trap_set(0, 0);
    ppc_trap(0xA82A);
    uint32_t r = ppc_trap_result(4);
    ppc_trap_end();
    RETURN(r);
}

#define CALL(name, what, args) static void as_##name(ppc_cpu *c) { instance_call(c, what, args); }
CALL(OSACopyScript, 0x0105, "44")
CALL(OSASetCurrentDialect, 0x0701, "2")
CALL(OSAGetCurrentDialect, 0x0702, "4")
CALL(OSAAvailableDialects, 0x0703, "4")
CALL(OSAGetDialectInfo, 0x0704, "244")
CALL(OSAAvailableDialectCodeList, 0x0705, "4")
CALL(ASInit, 0x1001, "4444444")
CALL(ASSetSourceStyles, 0x1002, "4")
CALL(ASGetSourceStyles, 0x1003, "4")
CALL(ASGetSourceStyleNames, 0x1004, "44")
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
CALL(OSAFlushTerminology, 0x110E, "4")
/* The generic scripting component's. */
CALL(OSAGetDefaultScriptingComponent, 0x1001, "4")
CALL(OSAGetScriptingComponent, 0x1003, "44")
CALL(OSAGetScriptingComponentFromStored, 0x1004, "44")
CALL(OSAGenericToRealID, 0x1005, "44")
CALL(OSARealToGenericID, 0x1006, "44")

/* ---- Storage types ---- */

/* The trailer of script data, if it has one: its offset in the handle. */
static bool trailer_of(gaddr h, uint32_t *at)
{
    if (!h || !rd32(h))
        return false;
    int32_t n = tb_GetHandleSize(h);
    if (n < TRAILER || (n & 1))
        return false;
    gaddr t = rd32(h) + (uint32_t)n - TRAILER;
    if (rd32(t + 8) != TRAILER_MARK || (int16_t)rd16(t + 4) != 1)
        return false;
    *at = (uint32_t)n - TRAILER;
    return true;
}

/* OSAGetStorageType(scriptData, *dscType): the type in the trailer, else
 * the generic script storage type. */
static void as_OSAGetStorageType(ppc_cpu *c)
{
    uint32_t at;
    if (!R(0) || !rd32(R(0)) || !R(1))
        RETURN(errOSABadStorageType);
    wr32(R(1), trailer_of(R(0), &at) ? rd32(rd32(R(0)) + at) : typeOSAGenericStorage);
    RETURN(noErr);
}

/* OSAAddStorageType(scriptData, dscType): the trailer put on, or its type
 * changed. */
static void as_OSAAddStorageType(ppc_cpu *c)
{
    uint32_t at;
    gaddr h = R(0);
    if (!h || !rd32(h))
        RETURN(errOSABadStorageType);
    if (!trailer_of(h, &at)) {
        int32_t n = tb_GetHandleSize(h);
        int32_t pad = n & 1;
        tb_SetHandleSize(h, n + pad + TRAILER);
        if (tb_GetHandleSize(h) != n + pad + TRAILER)
            RETURN(memFullErr);
        gaddr p = rd32(h);
        if (pad)
            wr8(p + (uint32_t)n, 0);
        at = (uint32_t)(n + pad);
        wr16(p + at + 4, 1);
        wr16(p + at + 6, 0);
        wr32(p + at + 8, TRAILER_MARK);
    }
    wr32(rd32(h) + at, R(1));
    RETURN(noErr);
}

/* OSARemoveStorageType(scriptData): the trailer taken off. */
static void as_OSARemoveStorageType(ppc_cpu *c)
{
    uint32_t at;
    if (!R(0) || !rd32(R(0)))
        RETURN(errOSABadStorageType);
    if (trailer_of(R(0), &at))
        tb_SetHandleSize(R(0), (int32_t)at);
    RETURN(noErr);
}

const ppc_export os9rest_osa_exports[] = {
#define E(n) { #n, as_##n }
    E(OSACopyScript), E(OSASetCurrentDialect), E(OSAGetCurrentDialect), E(OSAAvailableDialects),
    E(OSAGetDialectInfo), E(OSAAvailableDialectCodeList), E(ASInit), E(ASSetSourceStyles), E(ASGetSourceStyles),
    E(ASGetSourceStyleNames), E(ASSetProperty), E(ASGetProperty), E(ASSetHandler), E(ASGetHandler),
    E(ASGetAppTerminology), E(OSASetProperty), E(OSAGetProperty), E(OSASetHandler), E(OSAGetHandler),
    E(OSAGetAppTerminology), E(OSAGetSysTerminology), E(OSAGetPropertyNames), E(OSAGetHandlerNames),
    E(OSAFlushTerminology), E(OSAGetDefaultScriptingComponent), E(OSAGetScriptingComponent),
    E(OSAGetScriptingComponentFromStored), E(OSAGenericToRealID), E(OSARealToGenericID),
    E(OSAGetStorageType), E(OSAAddStorageType), E(OSARemoveStorageType),
#undef E
};
const unsigned os9rest_osa_export_count = sizeof os9rest_osa_exports / sizeof os9rest_osa_exports[0];
