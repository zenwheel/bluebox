/*
 * InterfaceLib's utility calls that PowerPC programs import by name with
 * no 68k trap behind them to glue to: the tables of the files that make
 * them (Driver Services' time and atomic operations, low memory, ...),
 * and those small enough to live here.
 */
#include <string.h>

#include "ppc_bridge.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; return; } while (0)

enum { badComponentSelector = (int32_t)0x80008002 };

/* ---------------------------------------------------------------------- */
/* Low memory: LowMem.h's accessors for the globals multiversal names no
 * accessor for, on the globals themselves. */

#define LM_SCALAR(name, addr, rd, wr, ext)                                      \
    static void lm_get_##name(ppc_cpu *c) { RETURN(ext(rd(addr))); }           \
    static void lm_set_##name(ppc_cpu *c)                                       \
    {                                                                           \
        wr(addr, R(0));                                                         \
        c->pc = c->lr;                                                          \
    }
#define U32(v) (uint32_t)(v)
#define S16(v) (uint32_t)(int32_t)(int16_t)(v)
#define U8(v) (uint32_t)(v)
#define W8(a, v) wr8(a, (uint8_t)(v))
#define W16(a, v) wr16(a, (uint16_t)(v))
LM_SCALAR(ABusGlobals, 0x02D8, rd32, wr32, U32)
LM_SCALAR(ATalkHk2, 0x0B18, rd32, wr32, U32)
LM_SCALAR(CursorNew, 0x08CE, rd8, W8, U8)
LM_SCALAR(DiskFormatingHFSDefaults, 0x039E, rd32, wr32, U32)
LM_SCALAR(HighHeapMark, 0x0BAE, rd32, wr32, U32)
LM_SCALAR(KeyTime, 0x0186, rd32, wr32, U32)
LM_SCALAR(MBTicks, 0x016E, rd32, wr32, U32)
LM_SCALAR(MouseButtonState, 0x0172, rd8, W8, U8)
LM_SCALAR(PortAInfo, 0x0290, rd8, W8, U8)
LM_SCALAR(ROMMapHandle, 0x0B06, rd32, wr32, U32)
LM_SCALAR(ROMMapInsert, 0x0B9E, rd8, W8, U8)
LM_SCALAR(StackLowPoint, 0x0110, rd32, wr32, U32)
LM_SCALAR(UnitTableEntryCount, 0x01D2, rd16, W16, S16)
LM_SCALAR(WordRedraw, 0x0BA5, rd8, W8, U8)
#undef LM_SCALAR

/* Points come back as a small struct result; set, one in r3 (v, h). */
#define LM_POINT(name, addr)                                                    \
    static void lm_get_##name(ppc_cpu *c)                                       \
    {                                                                           \
        ppc_struct_result(c, c->gpr[3], rd32(addr), 4);                         \
        c->pc = c->lr;                                                          \
    }                                                                           \
    static void lm_set_##name(ppc_cpu *c)                                       \
    {                                                                           \
        wr32(addr, R(0));                                                       \
        c->pc = c->lr;                                                          \
    }
LM_POINT(MouseTemp, 0x0828)
LM_POINT(RawMouseLocation, 0x082C)
#undef LM_POINT

/* TheCrsr ($844), a whole Cursor record, through a pointer. */
static void lm_get_TheCursor(ppc_cpu *c)
{
    ppc_copy(R(0), 0x0844, 68);
    c->pc = c->lr;
}
static void lm_set_TheCursor(ppc_cpu *c)
{
    ppc_copy(0x0844, R(0), 68);
    c->pc = c->lr;
}

/* DMDisposePortComponent: DMDisposeAVComponent's first name. */
static void il_DMDisposePortComponent(ppc_cpu *c) { RETURN((int32_t)tb_DMDisposeAVComponent(R(0))); }

/* GetEventQueue: the Event Manager's queue header, in low memory. */
static void il_GetEventQueue(ppc_cpu *c) { RETURN(LM_EventQueue); }

#define LM(name) { "LMGet" #name, lm_get_##name }, { "LMSet" #name, lm_set_##name }
static const ppc_export lm_exports[] = {
    LM(ABusGlobals), LM(ATalkHk2), LM(CursorNew), LM(DiskFormatingHFSDefaults), LM(HighHeapMark),
    LM(KeyTime), LM(MBTicks), LM(MouseButtonState), LM(PortAInfo), LM(ROMMapHandle), LM(ROMMapInsert),
    LM(StackLowPoint), LM(UnitTableEntryCount), LM(WordRedraw), LM(MouseTemp), LM(RawMouseLocation),
    LM(TheCursor),
    { "GetEventQueue", il_GetEventQueue }, { "DMDisposePortComponent", il_DMDisposePortComponent },
};
#undef LM
static const unsigned lm_export_count = sizeof lm_exports / sizeof lm_exports[0];

/* ---------------------------------------------------------------------- */
/* The Component Manager's calls of components (CallComponentOpen ...) and
 * its public resources (QuickTime 4). */

/* A call to an instance (ComponentCallNow): the instance and the
 * parameters (sizes 4 or 2, as a string), then the parameter size and
 * selector; _ComponentDispatch with D0 = 0. */
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

#define INSTANCE(name, what, args) static void cc_##name(ppc_cpu *c) { instance_call(c, what, args); }
INSTANCE(CallComponentOpen, -1, "4")
INSTANCE(CallComponentClose, -2, "4")
INSTANCE(CallComponentCanDo, -3, "2")
INSTANCE(CallComponentVersion, -4, "")
INSTANCE(CallComponentRegister, -5, "")
INSTANCE(CallComponentTarget, -6, "4")
INSTANCE(CallComponentUnregister, -7, "")
INSTANCE(CallComponentGetMPWorkFunction, -8, "44")
INSTANCE(CallComponentGetPublicResource, -10, "424")
/* Text services' calls (TextServices.h) */
INSTANCE(GetScriptLanguageSupport, 1, "4")
INSTANCE(InitiateTextService, 2, "")
INSTANCE(TerminateTextService, 3, "")
INSTANCE(ActivateTextService, 4, "")
INSTANCE(DeactivateTextService, 5, "")
INSTANCE(TextServiceEvent, 6, "24")
INSTANCE(GetTextServiceMenu, 7, "4")
INSTANCE(TextServiceMenuSelect, 8, "42")
INSTANCE(FixTextService, 9, "")
INSTANCE(SetTextServiceCursor, 10, "4")
INSTANCE(HidePaletteWindows, 11, "")
INSTANCE(UCTextServiceEvent, 14, "2444")
#undef INSTANCE

/* _ComponentDispatch with the selector in D0, Pascal arguments, an OSErr. */
static void component_trap(ppc_cpu *c, uint8_t sel, const char *args)
{
    ppc_trap_begin();
    ppc_trap_push(2, 0);
    for (int i = 0; args[i]; i++)
        ppc_trap_push(args[i] - '0', R(i));
    ppc_trap_set(0, sel);
    ppc_trap(0xA82A);
    uint32_t r = ppc_trap_result(2);
    ppc_trap_end();
    RETURN((int32_t)(int16_t)r);
}
static void cc_GetComponentPublicResource(ppc_cpu *c) { component_trap(c, 0x38, "4424"); }
static void cc_GetComponentPublicResourceList(ppc_cpu *c) { component_trap(c, 0x39, "4244444"); }
static void cc_GetComponentPublicIndString(ppc_cpu *c) { component_trap(c, 0x3A, "4422"); }

/* CallComponentFunction(params, func) and the WithStorage forms: the
 * function gets the original call's arguments (after the storage, for
 * those), as its procInfo describes them, from the ComponentParameters
 * (flags, size, what, then the arguments as a 68k caller pushed them:
 * the first highest, a byte or word in two bytes). */
enum { RD_PROCINFO = 12 };

static int call_args(gaddr params, uint32_t pi, int first, uint32_t *args, int max)
{
    uint32_t off = rd8(params + 1);
    int n = first;
    for (int i = first; i < max; i++) {
        unsigned code = (pi >> (6 + 2 * i)) & 3;
        if (!code)
            break;
        uint32_t sz = code == 3 ? 4 : 2;
        if (off < sz)
            break;
        off -= sz;
        gaddr at = params + 4 + off;
        args[i] = code == 3 ? rd32(at) : code == 2 ? (uint32_t)(int32_t)(int16_t)rd16(at) : rd8(at);
        n = i + 1;
    }
    return n;
}

static uint32_t call_function(gaddr func, uint32_t pi, gaddr params, bool storage, gaddr h)
{
    uint32_t args[13] = { 0 };
    if (mm_is_descriptor(func))
        pi = rd32(func + RD_PROCINFO);
    if (storage)
        args[0] = h;
    int n = call_args(params, pi, storage ? 1 : 0, args, 13);
    if (!func)
        return (uint32_t)badComponentSelector;
    /* A bare pointer from PowerPC code is a transition vector. */
    return mm_is_descriptor(func) ? mm_call(func, pi, args, n) : ppc_call(func, args, n);
}

static void cc_CallComponentFunction(ppc_cpu *c) { RETURN(call_function(R(1), 0, R(0), false, 0)); }
static void cc_CallComponentFunctionWithStorage(ppc_cpu *c) { RETURN(call_function(R(2), 0, R(1), true, R(0))); }
static void cc_CallComponentFunctionWithStorageProcInfo(ppc_cpu *c)
{
    RETURN(call_function(R(2), R(3), R(1), true, R(0)));
}

static const ppc_export cc_exports[] = {
    { "CallComponentOpen", cc_CallComponentOpen }, { "CallComponentClose", cc_CallComponentClose },
    { "CallComponentCanDo", cc_CallComponentCanDo }, { "ComponentFunctionImplemented", cc_CallComponentCanDo },
    { "CallComponentVersion", cc_CallComponentVersion }, { "CallComponentRegister", cc_CallComponentRegister },
    { "CallComponentTarget", cc_CallComponentTarget }, { "ComponentSetTarget", cc_CallComponentTarget },
    { "CallComponentUnregister", cc_CallComponentUnregister },
    { "CallComponentGetMPWorkFunction", cc_CallComponentGetMPWorkFunction },
    { "CallComponentGetPublicResource", cc_CallComponentGetPublicResource },
    { "GetComponentPublicResource", cc_GetComponentPublicResource },
    { "GetComponentPublicResourceList", cc_GetComponentPublicResourceList },
    { "GetComponentPublicIndString", cc_GetComponentPublicIndString },
    { "GetScriptLanguageSupport", cc_GetScriptLanguageSupport }, { "InitiateTextService", cc_InitiateTextService },
    { "TerminateTextService", cc_TerminateTextService }, { "ActivateTextService", cc_ActivateTextService },
    { "DeactivateTextService", cc_DeactivateTextService }, { "TextServiceEvent", cc_TextServiceEvent },
    { "GetTextServiceMenu", cc_GetTextServiceMenu }, { "TextServiceMenuSelect", cc_TextServiceMenuSelect },
    { "FixTextService", cc_FixTextService }, { "SetTextServiceCursor", cc_SetTextServiceCursor },
    { "HidePaletteWindows", cc_HidePaletteWindows }, { "UCTextServiceEvent", cc_UCTextServiceEvent },
    { "CallComponentFunction", cc_CallComponentFunction },
    { "CallComponentFunctionWithStorage", cc_CallComponentFunctionWithStorage },
    { "CallComponentFunctionWithStorageProcInfo", cc_CallComponentFunctionWithStorageProcInfo },
};
static const unsigned cc_export_count = sizeof cc_exports / sizeof cc_exports[0];

extern const ppc_export dsl_exports[];
extern const unsigned dsl_export_count;

typedef struct {
    const ppc_export *table;
    const unsigned *count;
} export_table;

static const export_table tables[] = {
    { dsl_exports, &dsl_export_count },
    { lm_exports, &lm_export_count },
    { cc_exports, &cc_export_count },
};

void dictionary_reset(void);
void tsm_reset(void);
void dmgr_reset(void);

/* A fresh machine. */
void ilutil_reset(void)
{
    dictionary_reset();
    tsm_reset();
    dmgr_reset();
}

void (*ilutil_export_fn(const char *name))(ppc_cpu *)
{
    for (unsigned t = 0; t < sizeof tables / sizeof tables[0]; t++)
        for (unsigned i = 0; i < *tables[t].count; i++)
            if (!strcmp(tables[t].table[i].name, name))
                return tables[t].table[i].fn;
    return NULL;
}
