/*
 * Gestalt.  Values describe the machine we present; the environment profile
 * can override any selector (gestalt_set).
 */
#include "m68k.h"
#include "tb_internal.h"
#include "trap.h"

#define MAX_GESTALT 128

typedef struct {
    uint32_t selector;
    uint32_t value;
    gaddr proc; /* guest selector function, if installed with NewGestalt */
} gestalt_entry;

static gestalt_entry table[MAX_GESTALT];
static unsigned count;

void gestalt_set(uint32_t selector, uint32_t value)
{
    for (unsigned i = 0; i < count; i++) {
        if (table[i].selector == selector) {
            table[i].value = value;
            table[i].proc = 0;
            return;
        }
    }
    if (count < MAX_GESTALT)
        table[count++] = (gestalt_entry){ selector, value, 0 };
}

/* A selector's fixed value (not one answered by a guest function). */
uint32_t gestalt_value(uint32_t selector, uint32_t fallback)
{
    for (unsigned i = 0; i < count; i++)
        if (table[i].selector == selector && !table[i].proc)
            return table[i].value;
    return fallback;
}

void gestalt_init(void)
{
    count = 0;
    gestalt_set(fourcc("sysv"), 0x0710);      /* System 7.1 */
    gestalt_set(fourcc("thds"), 0x3);         /* the Thread Manager, with exact-match pools */
    gestalt_set(fourcc("sysa"), 1);           /* 68k architecture */
    gestalt_set(fourcc("mach"), 35);          /* Quadra 800 */
    bool m040 = g_cpu && g_cpu->model == 68040;
    gestalt_set(fourcc("proc"), m040 ? 5 : 3); /* 68040 or 68020 */
    gestalt_set(fourcc("fpu "), m040 ? 3 : 0); /* the 68040's own FPU, or none */
    gestalt_set(fourcc("mmu "), 0);
    gestalt_set(fourcc("qd  "), 0x0130);      /* 32-bit QuickDraw 1.3 */
    gestalt_set(fourcc("addr"), 0x7);         /* 32-bit addressing, sys zone, capable */
    gestalt_set(fourcc("ram "), g_mem_report_cap);
    gestalt_set(fourcc("qtim"), 0x02508000); /* QuickTime 2.5 (movies play as zero length) */
    gestalt_set(fourcc("lram"), g_mem_report_cap);
    gestalt_set(fourcc("vm  "), 0);
    gestalt_set(fourcc("rsrc"), 0x3);         /* Resource Manager: partial, has GetResourceSizeOnDisk */
    gestalt_set(fourcc("rom "), 0x077D);      /* ROM version */
    gestalt_set(fourcc("romv"), 0x077D);
    gestalt_set(fourcc("kbd "), 4);           /* Apple Extended Keyboard */
    gestalt_set(fourcc("os  "), 0x2);         /* Process Manager present (launch control) */
    gestalt_set(fourcc("evnt"), 0x1);         /* Apple Event Manager present */
    /* What else System 7.1 answered, for the managers we provide. */
    gestalt_set(fourcc("vers"), 1);           /* Gestalt itself */
    gestalt_set(fourcc("scri"), 0x0710);      /* Script Manager */
    gestalt_set(fourcc("te  "), 5);           /* TextEdit (gestaltTE5) */
    gestalt_set(fourcc("hdwr"), 0x009B);      /* VIA1, VIA2, ASC, SCC, SCSI */
    gestalt_set(fourcc("edtn"), 0x1);         /* Edition Manager present */
    gestalt_set(fourcc("help"), 0x1);         /* Help Manager present */
    gestalt_set(fourcc("alis"), 0x1);         /* Alias Manager present */
    gestalt_set(fourcc("fold"), 0x1);         /* Folder Manager (FindFolder) present */
    gestalt_set(fourcc("fs  "), 0x2);         /* FSSpec calls */
    gestalt_set(fourcc("stdf"), 0x1);         /* Standard File 5.8 calls */
    gestalt_set(fourcc("icon"), 0x1);         /* Icon Utilities present */
    gestalt_set(fourcc("tmgr"), 3);           /* extended Time Manager */
    gestalt_set(fourcc("snd "), 0x3);         /* stereo, stereo mixing */
    gestalt_set(fourcc("font"), 0);           /* bitmap fonts only */
}

static int16_t gestalt_lookup(int32_t selector, uint32_t *responsep)
{
    for (unsigned i = 0; i < count; i++) {
        if (table[i].selector == (uint32_t)selector) {
            if (table[i].proc) {
                /* FUNCTION selFunc(selector: OSType; VAR response: LONGINT): OSErr */
                cpu68k *c = g_cpu;
                gaddr resp = tb_NewPtr(4, false, true);
                cpu_push16(c, 0);
                cpu_push32(c, (uint32_t)selector);
                cpu_push32(c, resp);
                guest_call(table[i].proc);
                int16_t err = (int16_t)cpu_pop16(c);
                *responsep = rd32(resp);
                tb_DisposePtr(resp);
                return err;
            }
            *responsep = table[i].value;
            return noErr;
        }
    }
    *responsep = 0;
    return gestaltUnknownErr;
}

int16_t tb_Gestalt(int32_t selector, uint32_t *responsep)
{
    int16_t err = gestalt_lookup(selector, responsep);
    if (g_log_mask & LOG_TRAPS) {
        char tc[5];
        log_printf("  Gestalt '%s' -> %d, %08X\n", fourcc_str((uint32_t)selector, tc), err, *responsep);
    }
    return err;
}

int16_t tb_NewGestalt(int32_t selector, gaddr selFunc)
{
    for (unsigned i = 0; i < count; i++)
        if (table[i].selector == (uint32_t)selector)
            return -5552; /* gestaltDupSelectorErr */
    if (count >= MAX_GESTALT)
        return memFullErr;
    table[count++] = (gestalt_entry){ (uint32_t)selector, 0, selFunc };
    return noErr;
}

int16_t tb_ReplaceGestalt(int32_t selector, gaddr selFunc, uint32_t *oldSelFuncp)
{
    for (unsigned i = 0; i < count; i++) {
        if (table[i].selector == (uint32_t)selector) {
            *oldSelFuncp = table[i].proc;
            table[i].proc = selFunc;
            return noErr;
        }
    }
    *oldSelFuncp = 0;
    return gestaltUnknownErr;
}
