/*
 * Segment Loader.
 *
 * CODE 0 describes the A5 world and holds the jump table.  An unloaded
 * jump table entry is
 *     offset(2)  MOVE.W #seg,-(SP)  _LoadSeg
 * and a loaded one is
 *     seg(2)     JMP abs.L
 * Calls go to entry+2, so after loading, LoadSeg returns there to take the JMP.
 */
#include <stdio.h>
#include <string.h>

#include "forks.h"
#include "tb_internal.h"

#define MOVE_W_SP 0x3F3C
#define LOADSEG_TRAP 0xA9F0
#define JMP_ABS_L 0x4EF9

static gaddr jt_base(void) { return rd32(LM_CurrentA5) + rd16(LM_CurJTOffset); }

/* Find the CODE resource for a segment in the application's resource file. */
static gaddr get_code(int16_t seg)
{
    int16_t saved = (int16_t)rd16(LM_CurMap);
    uint8_t saved_load = rd8(LM_ResLoad);
    wr16(LM_CurMap, rd16(LM_CurApRefNum));
    wr8(LM_ResLoad, 0xFF);
    gaddr h = tb_Get1Resource((int32_t)fourcc("CODE"), seg);
    wr8(LM_ResLoad, saved_load);
    wr16(LM_CurMap, (uint16_t)saved);
    return h;
}

static bool load_segment(int16_t seg)
{
    gaddr h = get_code(seg);
    if (!h || !rd32(h))
        return false;
    tb_HLock(h);
    gaddr code = rd32(h);
    uint16_t first = rd16(code);
    uint16_t count = rd16(code + 2);
    if (first == 0xA89F) /* still compressed: should have been expanded on load */
        return false;
    gaddr e = jt_base() + first;
    for (unsigned i = 0; i < count; i++, e += 8) {
        if (rd16(e + 2) == JMP_ABS_L)
            continue;
        uint16_t off = rd16(e);
        wr16(e, (uint16_t)seg);
        wr16(e + 2, JMP_ABS_L);
        wr32(e + 4, code + off + 4);
    }
    LOGF(LOG_SEG, "seg: loaded CODE %d at %08X (%u entries)\n", seg, code, count);
    return true;
}

/* _LoadSeg: stack is [return][segment]; the return address is just past
 * the trap in the jump table entry. */
static void raw_LoadSeg(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    int16_t seg = (int16_t)cpu_pop16(c);
    if (!load_segment(seg)) {
        cpu_halt(c, "LoadSeg: cannot load CODE %d", seg);
        return;
    }
    /* Re-execute the entry, now a JMP: entry+2 = trap address - 4. */
    cpu_push32(c, ret - 6);
}

/* _UnloadSeg(routineAddr): restore the jump table entries of its segment. */
static void raw_UnloadSeg(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    gaddr addr = cpu_pop32(c);
    cpu_push32(c, ret);
    gaddr a5 = rd32(LM_CurrentA5);
    gaddr jt = jt_base();
    uint32_t jtsize = 0;
    /* Jump table size from CODE 0 (kept loaded by the launch). */
    gaddr c0 = get_code(0);
    if (c0 && rd32(c0))
        jtsize = rd32(rd32(c0) + 8);
    (void)a5;
    int16_t seg = -1;
    gaddr code = 0;
    for (gaddr e = jt; e < jt + jtsize; e += 8) {
        if (rd16(e + 2) != JMP_ABS_L)
            continue;
        int16_t s = (int16_t)rd16(e);
        gaddr h = get_code(s);
        if (!h || !rd32(h))
            continue;
        gaddr start = rd32(h);
        int32_t size = tb_GetHandleSize(h);
        if (addr >= start && addr < start + (uint32_t)size) {
            seg = s;
            code = start;
            break;
        }
    }
    if (seg < 0)
        return;
    for (gaddr e = jt; e < jt + jtsize; e += 8) {
        if (rd16(e + 2) != JMP_ABS_L || (int16_t)rd16(e) != seg)
            continue;
        uint32_t target = rd32(e + 4);
        wr16(e, (uint16_t)(target - code - 4));
        wr16(e + 2, MOVE_W_SP);
        wr16(e + 4, (uint16_t)seg);
        wr16(e + 6, LOADSEG_TRAP);
    }
    gaddr h = get_code(seg);
    if (h) {
        tb_HUnlock(h);
        tb_HPurge(h);
    }
    LOGF(LOG_SEG, "seg: unloaded CODE %d\n", seg);
}

static void raw_ExitToShell(cpu68k *c, uint16_t tw)
{
    cpu_halt(c, "ExitToShell");
}

/* Return address for the application's main routine. */
static void app_returned(cpu68k *c, uint16_t tw)
{
    cpu_halt(c, "ExitToShell (main returned)");
}

void tb_GetAppParms(gaddr namep, gaddr rnp, gaddr aphandp)
{
    if (namep)
        gmemcpy(namep, LM_CurApName, (uint32_t)rd8(LM_CurApName) + 1);
    if (rnp)
        wr16(rnp, rd16(LM_CurApRefNum));
    if (aphandp)
        wr32(aphandp, rd32(LM_AppParmHandle));
}

uint32_t seg_launch(int16_t app_refnum, uint32_t *a5_out, char *err, size_t errlen)
{
    gaddr c0h = get_code(0);
    if (!c0h || !rd32(c0h)) {
        snprintf(err, errlen, "no CODE 0 resource");
        return 0;
    }
    tb_HLock(c0h);
    gaddr c0 = rd32(c0h);
    uint32_t above = rd32(c0), below = rd32(c0 + 4), jtsize = rd32(c0 + 8), jtoff = rd32(c0 + 12);
    if (above < jtoff + jtsize || above > 0x01000000 || below > 0x10000000) {
        snprintf(err, errlen, "implausible CODE 0 (above %u, below %u, jt %u@%u)", above, below, jtsize, jtoff);
        return 0;
    }
    /* A5 world at the top of the address space reserved for it; the stack
     * grows down from just below it. */
    gaddr a5 = (GA_A5WORLD_TOP - above) & ~3u;
    gaddr base = (a5 - below) & ~3u;
    gmemset(base, 0, a5 - base);
    gmemset(a5, 0, above);
    gmemcpy(a5 + jtoff, c0 + 16, jtsize);
    wr32(LM_CurrentA5, a5);
    wr16(LM_CurJTOffset, (uint16_t)jtoff);
    wr32(LM_CurStackBase, base);
    wr32(a5 + 16, rd32(LM_AppParmHandle)); /* Finder information handle */
    wr16(LM_CurApRefNum, (uint16_t)app_refnum);
    wr16(LM_CurPageOption, 0);
    *a5_out = a5;

    /* Stack: main's return address goes to a stub that ends the app. */
    uint32_t returned_stub = native_register(app_returned, "AppReturned", 0);
    g_cpu->a[7] = base;
    cpu_push32(g_cpu, returned_stub);
    g_cpu->a[5] = a5;
    LOGF(LOG_SEG, "seg: A5 %08X, below %u, above %u, jump table %u bytes at +%u\n", a5, below, above, jtsize, jtoff);
    /* The first jump table entry is the entry point. */
    return a5 + jtoff + 2;
}

/* Hand-written trap natives (see trap.c). */
const raw_trap raw_traps[] = {
    { 0xA9F0, "LoadSeg", raw_LoadSeg },
    { 0xA9F1, "UnloadSeg", raw_UnloadSeg },
    { 0xA9F4, "ExitToShell", raw_ExitToShell },
    { 0, NULL, NULL },
};
