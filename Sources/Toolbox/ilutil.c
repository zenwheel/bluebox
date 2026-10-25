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

/* GetEventQueue: the Event Manager's queue header, in low memory. */
static void il_GetEventQueue(ppc_cpu *c) { RETURN(LM_EventQueue); }

#define LM(name) { "LMGet" #name, lm_get_##name }, { "LMSet" #name, lm_set_##name }
static const ppc_export lm_exports[] = {
    LM(ABusGlobals), LM(ATalkHk2), LM(CursorNew), LM(DiskFormatingHFSDefaults), LM(HighHeapMark),
    LM(KeyTime), LM(MBTicks), LM(MouseButtonState), LM(PortAInfo), LM(ROMMapHandle), LM(ROMMapInsert),
    LM(StackLowPoint), LM(UnitTableEntryCount), LM(WordRedraw), LM(MouseTemp), LM(RawMouseLocation),
    LM(TheCursor),
    { "GetEventQueue", il_GetEventQueue },
};
#undef LM
static const unsigned lm_export_count = sizeof lm_exports / sizeof lm_exports[0];

extern const ppc_export dsl_exports[];
extern const unsigned dsl_export_count;

typedef struct {
    const ppc_export *table;
    const unsigned *count;
} export_table;

static const export_table tables[] = {
    { dsl_exports, &dsl_export_count },
    { lm_exports, &lm_export_count },
};

void (*ilutil_export_fn(const char *name))(ppc_cpu *)
{
    for (unsigned t = 0; t < sizeof tables / sizeof tables[0]; t++)
        for (unsigned i = 0; i < *tables[t].count; i++)
            if (!strcmp(tables[t].table[i].name, name))
                return tables[t].table[i].fn;
    return NULL;
}
