/*
 * Gestalt.  Values describe the machine we present; the environment profile
 * can override any selector (gestalt_set).
 */
#include "tb_internal.h"

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

void gestalt_init(void)
{
    count = 0;
    gestalt_set(fourcc("sysv"), 0x0710);      /* System 7.1 */
    gestalt_set(fourcc("sysa"), 1);           /* 68k architecture */
    gestalt_set(fourcc("mach"), 35);          /* Quadra 800 */
    gestalt_set(fourcc("proc"), 3);           /* 68020 */
    gestalt_set(fourcc("fpu "), 0);           /* no FPU (yet) */
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
}

int16_t tb_Gestalt(int32_t selector, uint32_t *responsep)
{
    for (unsigned i = 0; i < count; i++) {
        if (table[i].selector == (uint32_t)selector) {
            if (table[i].proc) {
                /* Calling guest selector functions needs nested execution. */
                *responsep = 0;
                return gestaltUnknownErr;
            }
            *responsep = table[i].value;
            return noErr;
        }
    }
    *responsep = 0;
    return gestaltUnknownErr;
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
