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
static uint32_t jt_size; /* the jump table's, from CODE 0 at the launch */

/* GetResource('CODE', seg) through the trap table when a program has
 * patched it, as the ROM's LoadSeg calls it: self-expanding applications
 * (Shufflepuck Café) patch GetResource to unpack each segment as it loads. */
static gaddr get_code_patched(int16_t seg)
{
    cpu68k *c = g_cpu;
    gaddr fn = rd32(TB_TRAP_TABLE + 0x1A0u * 4);
    if (!c || native_index_for(fn) >= 0)
        return 0;
    uint32_t d[8], a[8];
    memcpy(d, c->d, sizeof d);
    memcpy(a, c->a, sizeof a);
    cpu_push32(c, 0); /* the result */
    cpu_push32(c, fourcc("CODE"));
    cpu_push16(c, (uint16_t)seg);
    guest_call_regs(fn);
    gaddr h = cpu_pop32(c);
    memcpy(c->d, d, sizeof d);
    memcpy(c->a, a, sizeof a);
    return h;
}

/* Find the CODE resource for a segment in the application's resource file. */
static gaddr get_code(int16_t seg)
{
    int16_t saved = (int16_t)rd16(LM_CurMap);
    uint8_t saved_load = rd8(LM_ResLoad);
    wr16(LM_CurMap, rd16(LM_CurApRefNum));
    wr8(LM_ResLoad, 0xFF);
    gaddr h = get_code_patched(seg);
    if (!h)
        h = tb_Get1Resource((int32_t)fourcc("CODE"), seg);
    if (!h) {
        /* Not in the application's file: a program may keep segments in
         * resource files it opened itself (FrameMaker's modules), so look
         * through the whole chain; a CurMap no file has starts at the top. */
        wr16(LM_CurMap, 0xFFFF);
        h = tb_GetResource((int32_t)fourcc("CODE"), seg);
    }
    wr8(LM_ResLoad, saved_load);
    wr16(LM_CurMap, (uint16_t)saved);
    return h;
}

/* Load a segment and fill in its jump table entries; its code, or 0. */
static gaddr load_segment(int16_t seg)
{
    gaddr h = get_code(seg);
    if (!h || !rd32(h))
        return 0;
    tb_HLock(h);
    gaddr code = rd32(h);
    /* A program in a 24-bit partition may have set the master pointer's
     * flags in its high byte itself (Dungeon of Doom locks the segment so
     * in its own LoadSeg patch); a 24-bit Mac ignored that byte. */
    if (rd32(LM_CurrentA5) < GA_A5WORLD_TOP_24)
        code &= 0x00FFFFFFu;
    uint16_t first = rd16(code);
    uint16_t count = rd16(code + 2);
    if (first == 0xA89F) /* still compressed: should have been expanded on load */
        return 0;
    gaddr e = jt_base() + first;
    for (unsigned i = 0; i < count; i++, e += 8) {
        if (rd16(e + 2) == JMP_ABS_L)
            continue;
        /* An unloaded entry that names another segment is that segment's:
         * a Director projector's player lays its jump table over the
         * stub's, whose XInit entries it still calls once XMain has
         * unloaded XInit, and they must load XInit again. */
        if (rd16(e + 2) == MOVE_W_SP && rd16(e + 6) == LOADSEG_TRAP && (int16_t)rd16(e + 4) != seg)
            continue;
        uint16_t off = rd16(e);
        wr16(e, (uint16_t)seg);
        wr16(e + 2, JMP_ABS_L);
        wr32(e + 4, code + off + 4);
    }
    LOGF(LOG_SEG, "seg: loaded CODE %d at %08X (%u entries)\n", seg, code, count);
    return code;
}

/* _LoadSeg: stack is [return][segment]; the return address is just past
 * the trap in the jump table entry. */
static void raw_LoadSeg(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    int16_t seg = (int16_t)cpu_pop16(c);
    gaddr code = load_segment(seg);
    if (!code) {
        cpu_halt(c, "LoadSeg: cannot load CODE %d", seg);
        return;
    }
    /* The entry that called is patched too, as the ROM did: applications
     * that run with a copy of the jump table in an A5 world of their own
     * (Director projectors) call through entries not at CurrentA5. */
    gaddr e = ret - 8;
    if (rd16(e + 2) == 0x3F3C && (int16_t)rd16(e + 4) == seg && rd16(e + 6) == 0xA9F0) {
        uint16_t off = rd16(e);
        wr16(e, (uint16_t)seg);
        wr16(e + 2, JMP_ABS_L);
        wr32(e + 4, code + off + 4);
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
    uint32_t jtsize = jt_size;
    (void)a5;
    int16_t seg = -1;
    gaddr code = 0;
    /* The address of a routine as Pascal's @ gives it, its jump table
     * entry (the JMP in it, as System 7.1's Finder passes when its code
     * zone is full): the segment its entry names.  An entry still in its
     * unloaded form names a segment not loaded through the jump table,
     * so there is nothing to unload: getting the CODE resource would read
     * it in only to make it purgeable (HyperCard's standalones unload so
     * segments they never called, CODE 4's 28K each time). */
    if (addr >= jt && addr < jt + jtsize) {
        gaddr e = jt + (addr - jt) / 8 * 8;
        if (rd16(e + 2) != JMP_ABS_L)
            return;
        int16_t s = (int16_t)rd16(e);
        gaddr h = s >= 0 ? get_code(s) : 0;
        if (h && rd32(h)) {
            seg = s;
            code = rd32(h);
            if (rd32(LM_CurrentA5) < GA_A5WORLD_TOP_24)
                code &= 0x00FFFFFFu;
        }
    }
    for (gaddr e = jt; e < jt + jtsize && seg < 0; e += 8) {
        if (rd16(e + 2) != JMP_ABS_L)
            continue;
        int16_t s = (int16_t)rd16(e);
        gaddr h = get_code(s);
        if (!h || !rd32(h))
            continue;
        gaddr start = rd32(h);
        if (rd32(LM_CurrentA5) < GA_A5WORLD_TOP_24)
            start &= 0x00FFFFFFu; /* as in load_segment */
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
    rm_update_all();
    cpu_halt(c, "ExitToShell");
}

/* _Chain ($A9F3, System 6): Launch under another name; the program that
 * called it is replaced, as with Launch, its record the same. */
static void raw_Chain(cpu68k *c, uint16_t tw)
{
    c->d[0] = (uint32_t)(int32_t)tb_LaunchApplication(c->a[0]);
}

/* Return address for the application's main routine. */
static void app_returned(cpu68k *c, uint16_t tw)
{
    rm_update_all();
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
     * grows down from just below it.  A program whose SIZE resource doesn't
     * say it is 32-bit compatible gets its A5 world and stack below 16 MB,
     * where a Mac in 24-bit mode (or one with less memory) put them, and
     * the heap under them: such programs keep flags in the top byte of
     * addresses, or clear it.  Cribbage 2.5 clears one byte too many of a
     * local array, the top byte of its saved frame pointer, and returned
     * to address 0 when that byte wasn't already 0. */
    gaddr top = GA_A5WORLD_TOP;
    if ((g_mem_24bit || !(g_app_size_flags & 0x0080)) &&
        (uint64_t)above + below + GA_STACK_24 < GA_A5WORLD_TOP_24 - GA_APPHEAP) {
        gaddr limit = (((GA_A5WORLD_TOP_24 - above) & ~3u) - below - GA_STACK_24) & ~3u; /* whole longs */
        if (Zone_bkLim(rd32(LM_ApplZone)) < limit) {
            top = GA_A5WORLD_TOP_24;
            wr32(LM_BufPtr, top);
            wr32(LM_ApplLimit, limit);
            mm_set_appl_growth(limit);
        }
    }
    if (g_mem_24bit && top != GA_A5WORLD_TOP_24) {
        snprintf(err, errlen, "the application's globals and stack (%u bytes) don't fit in 24-bit addressing",
                 above + below);
        return 0;
    }
    gaddr a5 = (top - above) & ~3u;
    /* CurStackBase is exactly A5 less the globals: MPW's runtime unpacks
     * DATA 0 from there up to A5 and stops only on reaching it. */
    gaddr base = a5 - below;
    gmemset(base, 0, a5 - base);
    gmemset(a5, 0, above);
    gmemcpy(a5 + jtoff, c0 + 16, jtsize);
    jt_size = jtsize;
    /* CODE 0 is released once its jump table is in place, as the Segment
     * Loader did: a program unloading every purgeable CODE resource that is
     * in memory (Style) otherwise took CODE 0 for a segment, and its first
     * word, 0, for the main segment's jump table entries, unloading the
     * main segment under its own LoadSeg patch. */
    tb_HUnlock(c0h);
    tb_ReleaseResource(c0h);
    wr32(LM_CurrentA5, a5);
    wr16(LM_CurJTOffset, (uint16_t)jtoff);
    wr32(LM_CurStackBase, base);
    wr32(a5 + 16, rd32(LM_AppParmHandle)); /* Finder information handle */
    wr16(LM_CurApRefNum, (uint16_t)app_refnum);
    wr16(LM_CurPageOption, 0);
    *a5_out = a5;

    /* Stack: main's return address goes to a stub that ends the app. */
    uint32_t returned_stub = native_register(app_returned, "AppReturned", 0);
    g_cpu->a[7] = base & ~3u;
    cpu_push32(g_cpu, returned_stub);
    g_cpu->a[5] = a5;
    LOGF(LOG_SEG, "seg: A5 %08X, below %u, above %u, jump table %u bytes at +%u\n", a5, below, above, jtsize, jtoff);
    /* The first jump table entry is the entry point. */
    return a5 + jtoff + 2;
}

/* Hand-written trap natives (see trap.c). */
const raw_trap raw_traps[] = {
    { 0xA019, "InitZone", raw_InitZone },
    { 0xA02F, "PostEvent", raw_PostEvent },
    { 0xA03C, "CmpString", raw_CmpString },
    { 0xA03D, "DrvrInstall", raw_DrvrInstall },
    { 0xA04E, "AddDrive", raw_AddDrive },
    { 0xA051, "ReadXPRam", raw_ReadXPRam },
    { 0xA052, "WriteXPRam", raw_WriteXPRam },
    { 0xA056, "LowerText", raw_LowerText },
    { 0xA05C, "MemoryDispatch", raw_MemoryDispatch },
    { 0xA06E, "SlotManager", raw_SlotManager },
    { 0xA050, "RelString", raw_RelString },
    { 0xA054, "UprString", raw_UprString },
    { 0xA0FC, "ResourceStub", raw_ResourceStub },
    { 0xA080, "GetVideoDefault", raw_GetVideoDefault },
    { 0xA085, "IdleUpdate", raw_IdleUpdate },
    { 0xA093, "Microseconds", raw_Microseconds },
    { 0xA82A, "ComponentDispatch", raw_ComponentDispatch },
    { 0xA843, "Fix2X", raw_Fix2X },
    { 0xA845, "Frac2X", raw_Frac2X },
    { 0xAADB, "CursorDeviceDispatch", raw_CursorDeviceDispatch },
    { 0xA9EB, "FP68K", raw_FP68K },
    { 0xA9EC, "Elems68K", raw_Elems68K },
    { 0xA9F0, "LoadSeg", raw_LoadSeg },
    { 0xA9F1, "UnloadSeg", raw_UnloadSeg },
    { 0xA9F3, "Chain", raw_Chain },
    { 0xA9F4, "ExitToShell", raw_ExitToShell },
    { 0, NULL, NULL },
};
