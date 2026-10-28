/*
 * The storage type OSAComp.h puts on a script's data (AppleScriptLib's
 * OSAGetStorageType, OSAAddStorageType and OSARemoveStorageType): a 12-byte
 * trailer on the handle, as Mac OS 9's own code looks for it: the type, a
 * version (1), 2 spare bytes and the mark 0xFADEDEAD.  (The component
 * calls of AppleScriptLib are the Carbon agent's, carbonrest_osa.c.)
 */
#include <string.h>

#include "os9rest.h"
#include "ppc_bridge.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(int32_t)(v); c->pc = c->lr; return; } while (0)

enum { errOSABadStorageType = -1752, typeOSAGenericStorage = 0x73637074 /* 'scpt' */ };
enum { TRAILER = 12, TRAILER_MARK = 0xFADEDEADu };

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
    { "OSAGetStorageType", as_OSAGetStorageType }, { "OSAAddStorageType", as_OSAAddStorageType },
    { "OSARemoveStorageType", as_OSARemoveStorageType },
};
const unsigned os9rest_osa_export_count = sizeof os9rest_osa_exports / sizeof os9rest_osa_exports[0];
