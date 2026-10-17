/*
 * InterfaceLib exports that are glue, not traps: the high-level File
 * Manager calls (FSOpen, FSRead, HOpen ...) that 68k programs link in and
 * that call the PB routines underneath.  PowerPC programs import them.
 */
#include <string.h>

#include "machine.h"
#include "ppc_bridge.h"
#include "tb_internal.h"

/* ---- Running 68k traps ---- */

static ppc_trap_regs saved[TRAP_DEPTH];
static int trap_depth;
static gaddr trap_code; /* a trap word and a native for each depth */

void ppc_trap_begin(void)
{
    if (trap_depth < TRAP_DEPTH) {
        memcpy(saved[trap_depth].d, g_cpu->d, sizeof g_cpu->d);
        memcpy(saved[trap_depth].a, g_cpu->a, sizeof g_cpu->a);
        saved[trap_depth].sr = g_cpu->sr;
    }
    trap_depth++;
}

void ppc_trap_set(int reg, uint32_t value)
{
    if (reg < 8)
        g_cpu->d[reg] = value;
    else
        g_cpu->a[reg - 8] = value;
}

uint32_t ppc_trap_get(int reg) { return reg < 8 ? g_cpu->d[reg] : g_cpu->a[reg - 8]; }

void ppc_trap_push(int size, uint32_t value)
{
    if (size == 4)
        cpu_push32(g_cpu, value);
    else if (size == 1)
        cpu_push16(g_cpu, (uint16_t)((value & 0xFF) << 8));
    else
        cpu_push16(g_cpu, (uint16_t)value);
}

/* The trap runs inline, as in 68k code: the trap word, then a native that
 * marks the depth done, so nothing comes between the arguments and the
 * trap (a stack-based trap's arguments are right above its return). */
static bool trap_finished[TRAP_DEPTH];
static uint16_t done_index;

/* The nesting of traps run for PowerPC code belongs to the thread running
 * them: the Thread Manager switches it with the registers. */
void ppc_trap_ctx_save(ppc_trap_ctx *t)
{
    t->depth = trap_depth;
    memcpy(t->saved, saved, sizeof saved);
    memcpy(t->finished, trap_finished, sizeof trap_finished);
}

void ppc_trap_ctx_load(const ppc_trap_ctx *t)
{
    trap_depth = t->depth;
    memcpy(saved, t->saved, sizeof saved);
    memcpy(trap_finished, t->finished, sizeof trap_finished);
}

static void trap_done_native(cpu68k *c, uint16_t tw)
{
    (void)tw;
    uint32_t depth = (c->op_pc - 2 - trap_code) / 8;
    if (depth < TRAP_DEPTH)
        trap_finished[depth] = true;
}

void ppc_trap(uint16_t trapword)
{
    if (!trap_code) {
        trap_code = tb_NewPtr(TRAP_DEPTH * 8, true, true);
        done_index = (uint16_t)native_index_for(native_register(trap_done_native, "PowerPCTrapDone", 0));
    }
    int depth = trap_depth > 0 ? trap_depth - 1 : 0;
    if (depth >= TRAP_DEPTH) {
        ppc_halt(&g_ppc, "PowerPC glue nested too deeply (trap %04X)", trapword);
        return;
    }
    gaddr code = trap_code + (gaddr)depth * 8;
    wr16(code, trapword);
    wr16(code + 2, CPU_NATIVE_OP);
    wr16(code + 4, done_index);
    wr16(code + 6, 0x4E71); /* NOP */
    cpu68k *c = g_cpu;
    uint32_t saved_pc = c->pc;
    trap_finished[depth] = false;
    c->pc = code;
    uint64_t cap = trap_instruction_limit();
    while (!c->halted && !trap_finished[depth]) {
        cpu_step(c);
        if (cap && c->icount >= cap && !trap_finished[depth]) {
            if (machine_waiting_for_input())
                cpu_halt(c, "instruction limit reached waiting for input");
            else
                cpu_halt(c, "instruction limit reached in a call from the Toolbox (pc %08X)", c->pc);
        }
    }
    c->pc = saved_pc;
    if (c->halted && !g_ppc.halted)
        ppc_halt(&g_ppc, "%s", c->halt_reason);
}

uint32_t ppc_trap_result(int size)
{
    gaddr sp = g_cpu->a[7];
    return size == 4 ? rd32(sp) : size == 1 ? (uint32_t)(rd16(sp) >> 8) : rd16(sp);
}

void ppc_trap_end(void)
{
    trap_depth--;
    if (trap_depth < TRAP_DEPTH) {
        memcpy(g_cpu->d, saved[trap_depth].d, sizeof g_cpu->d);
        memcpy(g_cpu->a, saved[trap_depth].a, sizeof g_cpu->a);
        g_cpu->sr = saved[trap_depth].sr;
    }
}

void ppc_copy(gaddr dst, gaddr src, uint32_t n)
{
    if (dst && src)
        for (uint32_t i = 0; i < n; i++)
            wr8(dst + i, rd8(src + i));
}

/* A parameter block in the system heap, cleared for each call. */
static gaddr pb_scratch(void)
{
    static gaddr pb;
    if (!pb)
        pb = tb_NewPtr(128, true, true);
    gmemset(pb, 0, 128);
    return pb;
}

#define R(n) (c->gpr[3 + (n)])
#define RET(v) do { c->gpr[3] = (uint32_t)(int32_t)(int16_t)(v); c->pc = c->lr; } while (0)

static void out16(gaddr p, uint32_t v) { if (p) wr16(p, (uint16_t)v); }
static void out32(gaddr p, uint32_t v) { if (p) wr32(p, v); }

/* FSOpen(fileName, vRefNum, *refNum) */
static void g_FSOpen(ppc_cpu *c)
{
    gaddr pb = pb_scratch();
    wr32(pb + IOParam_ioNamePtr_off, R(0));
    wr16(pb + IOParam_ioVRefNum_off, (uint16_t)R(1));
    int16_t e = tb_PBOpen(pb, false);
    out16(R(2), rd16(pb + IOParam_ioRefNum_off));
    RET(e);
}

/* OpenRF(fileName, vRefNum, *refNum) */
static void g_OpenRF(ppc_cpu *c)
{
    gaddr pb = pb_scratch();
    wr32(pb + IOParam_ioNamePtr_off, R(0));
    wr16(pb + IOParam_ioVRefNum_off, (uint16_t)R(1));
    int16_t e = tb_PBOpenRF(pb, false);
    out16(R(2), rd16(pb + IOParam_ioRefNum_off));
    RET(e);
}

/* HOpen / HOpenDF / HOpenRF(vRefNum, dirID, fileName, permission, *refNum) */
static void h_open(ppc_cpu *c, int16_t (*fn)(gaddr, bool))
{
    gaddr pb = pb_scratch();
    wr16(pb + IOParam_ioVRefNum_off, (uint16_t)R(0));
    wr32(pb + HFileParam_ioDirID_off, R(1));
    wr32(pb + IOParam_ioNamePtr_off, R(2));
    wr8(pb + IOParam_ioPermssn_off, (uint8_t)R(3));
    int16_t e = fn(pb, false);
    out16(R(4), rd16(pb + IOParam_ioRefNum_off));
    RET(e);
}
static void g_HOpen(ppc_cpu *c) { h_open(c, tb_PBHOpen); }
static void g_HOpenDF(ppc_cpu *c) { h_open(c, tb_PBHOpenDF); }
static void g_HOpenRF(ppc_cpu *c) { h_open(c, tb_PBHOpenRF); }

/* FSClose(refNum) */
static void g_FSClose(ppc_cpu *c)
{
    gaddr pb = pb_scratch();
    wr16(pb + IOParam_ioRefNum_off, (uint16_t)R(0));
    RET(tb_PBClose(pb, false));
}

/* FSRead / FSWrite(refNum, *count, buffPtr) */
static void rw(ppc_cpu *c, bool write)
{
    gaddr pb = pb_scratch();
    wr16(pb + IOParam_ioRefNum_off, (uint16_t)R(0));
    wr32(pb + IOParam_ioReqCount_off, R(1) ? rd32(R(1)) : 0);
    wr32(pb + IOParam_ioBuffer_off, R(2));
    wr16(pb + IOParam_ioPosMode_off, 0); /* fsAtMark */
    int16_t e = write ? tb_PBWrite(pb, false) : tb_PBRead(pb, false);
    out32(R(1), rd32(pb + IOParam_ioActCount_off));
    RET(e);
}
static void g_FSRead(ppc_cpu *c) { rw(c, false); }
static void g_FSWrite(ppc_cpu *c) { rw(c, true); }

/* GetEOF(refNum, *logEOF), GetFPos(refNum, *filePos) */
static void get_mark(ppc_cpu *c, int16_t (*fn)(gaddr, bool), uint32_t field)
{
    gaddr pb = pb_scratch();
    wr16(pb + IOParam_ioRefNum_off, (uint16_t)R(0));
    int16_t e = fn(pb, false);
    out32(R(1), rd32(pb + field));
    RET(e);
}
static void g_GetEOF(ppc_cpu *c) { get_mark(c, tb_PBGetEOF, IOParam_ioMisc_off); }
static void g_GetFPos(ppc_cpu *c) { get_mark(c, tb_PBGetFPos, IOParam_ioPosOffset_off); }

/* SetEOF(refNum, logEOF) */
static void g_SetEOF(ppc_cpu *c)
{
    gaddr pb = pb_scratch();
    wr16(pb + IOParam_ioRefNum_off, (uint16_t)R(0));
    wr32(pb + IOParam_ioMisc_off, R(1));
    RET(tb_PBSetEOF(pb, false));
}

/* SetFPos(refNum, posMode, posOff) */
static void g_SetFPos(ppc_cpu *c)
{
    gaddr pb = pb_scratch();
    wr16(pb + IOParam_ioRefNum_off, (uint16_t)R(0));
    wr16(pb + IOParam_ioPosMode_off, (uint16_t)R(1));
    wr32(pb + IOParam_ioPosOffset_off, R(2));
    RET(tb_PBSetFPos(pb, false));
}

/* Allocate(refNum, *count) */
static void g_Allocate(ppc_cpu *c)
{
    gaddr pb = pb_scratch();
    wr16(pb + IOParam_ioRefNum_off, (uint16_t)R(0));
    wr32(pb + IOParam_ioReqCount_off, R(1) ? rd32(R(1)) : 0);
    int16_t e = tb_PBAllocate(pb, false);
    out32(R(1), rd32(pb + IOParam_ioActCount_off));
    RET(e);
}

/* GetVRefNum(refNum, *vRefNum): the volume of an open file. */
static void g_GetVRefNum(ppc_cpu *c)
{
    gaddr pb = pb_scratch();
    wr16(pb + IOParam_ioRefNum_off, (uint16_t)R(0));
    int16_t e = tb_PBGetFCBInfo(pb, false);
    out16(R(1), rd16(pb + FCBPBRec_ioFCBVRefNum_off));
    RET(e);
}

/* GetVol(volName, *vRefNum), SetVol(volName, vRefNum), FlushVol(volName, vRefNum) */
static void g_GetVol(ppc_cpu *c)
{
    gaddr pb = pb_scratch();
    wr32(pb + IOParam_ioNamePtr_off, R(0));
    int16_t e = tb_PBGetVol(pb, false);
    out16(R(1), rd16(pb + IOParam_ioVRefNum_off));
    RET(e);
}

static void g_SetVol(ppc_cpu *c)
{
    gaddr pb = pb_scratch();
    wr32(pb + IOParam_ioNamePtr_off, R(0));
    wr16(pb + IOParam_ioVRefNum_off, (uint16_t)R(1));
    RET(tb_PBSetVol(pb, false));
}

static void g_FlushVol(ppc_cpu *c)
{
    gaddr pb = pb_scratch();
    wr32(pb + IOParam_ioNamePtr_off, R(0));
    wr16(pb + IOParam_ioVRefNum_off, (uint16_t)R(1));
    RET(tb_PBFlushVol(pb, false));
}

/* HGetVol(volName, *vRefNum, *dirID), HSetVol(volName, vRefNum, dirID) */
static void g_HGetVol(ppc_cpu *c)
{
    gaddr pb = pb_scratch();
    wr32(pb + IOParam_ioNamePtr_off, R(0));
    int16_t e = tb_PBHGetVol(pb, false);
    out16(R(1), rd16(pb + WDPBRec_ioWDVRefNum_off));
    out32(R(2), rd32(pb + WDPBRec_ioWDDirID_off));
    RET(e);
}

static void g_HSetVol(ppc_cpu *c)
{
    gaddr pb = pb_scratch();
    wr32(pb + IOParam_ioNamePtr_off, R(0));
    wr16(pb + IOParam_ioVRefNum_off, (uint16_t)R(1));
    wr32(pb + WDPBRec_ioWDDirID_off, R(2));
    RET(tb_PBHSetVol(pb, false));
}

/* Create(fileName, vRefNum, creator, fileType): the file, then its Finder info. */
static int16_t set_type(gaddr pb, uint32_t creator, uint32_t type, bool hfs)
{
    int16_t e = hfs ? tb_PBHGetFInfo(pb, false) : tb_PBGetFInfo(pb, false);
    if (e)
        return e;
    wr32(pb + FileParam_ioFlFndrInfo_off, type);
    wr32(pb + FileParam_ioFlFndrInfo_off + 4, creator);
    return hfs ? tb_PBHSetFInfo(pb, false) : tb_PBSetFInfo(pb, false);
}

static void g_Create(ppc_cpu *c)
{
    gaddr pb = pb_scratch();
    wr32(pb + IOParam_ioNamePtr_off, R(0));
    wr16(pb + IOParam_ioVRefNum_off, (uint16_t)R(1));
    int16_t e = tb_PBCreate(pb, false);
    if (!e)
        e = set_type(pb, R(2), R(3), false);
    RET(e);
}

/* HCreate(vRefNum, dirID, fileName, creator, fileType) */
static void g_HCreate(ppc_cpu *c)
{
    gaddr pb = pb_scratch();
    wr16(pb + IOParam_ioVRefNum_off, (uint16_t)R(0));
    wr32(pb + HFileParam_ioDirID_off, R(1));
    wr32(pb + IOParam_ioNamePtr_off, R(2));
    int16_t e = tb_PBHCreate(pb, false);
    if (!e)
        e = set_type(pb, R(3), R(4), true);
    RET(e);
}

/* FSDelete(fileName, vRefNum), HDelete(vRefNum, dirID, fileName) */
static void g_FSDelete(ppc_cpu *c)
{
    gaddr pb = pb_scratch();
    wr32(pb + IOParam_ioNamePtr_off, R(0));
    wr16(pb + IOParam_ioVRefNum_off, (uint16_t)R(1));
    RET(tb_PBDelete(pb, false));
}

static void g_HDelete(ppc_cpu *c)
{
    gaddr pb = pb_scratch();
    wr16(pb + IOParam_ioVRefNum_off, (uint16_t)R(0));
    wr32(pb + HFileParam_ioDirID_off, R(1));
    wr32(pb + IOParam_ioNamePtr_off, R(2));
    RET(tb_PBHDelete(pb, false));
}

/* Rename(oldName, vRefNum, newName), HRename(vRefNum, dirID, oldName, newName) */
static void g_Rename(ppc_cpu *c)
{
    gaddr pb = pb_scratch();
    wr32(pb + IOParam_ioNamePtr_off, R(0));
    wr16(pb + IOParam_ioVRefNum_off, (uint16_t)R(1));
    wr32(pb + IOParam_ioMisc_off, R(2));
    RET(tb_PBRename(pb, false));
}

static void g_HRename(ppc_cpu *c)
{
    gaddr pb = pb_scratch();
    wr16(pb + IOParam_ioVRefNum_off, (uint16_t)R(0));
    wr32(pb + HFileParam_ioDirID_off, R(1));
    wr32(pb + IOParam_ioNamePtr_off, R(2));
    wr32(pb + IOParam_ioMisc_off, R(3));
    RET(tb_PBHRename(pb, false));
}

/* GetFInfo / SetFInfo(fileName, vRefNum, *fndrInfo) */
static void g_GetFInfo(ppc_cpu *c)
{
    gaddr pb = pb_scratch();
    wr32(pb + IOParam_ioNamePtr_off, R(0));
    wr16(pb + IOParam_ioVRefNum_off, (uint16_t)R(1));
    int16_t e = tb_PBGetFInfo(pb, false);
    if (!e && R(2))
        gmemcpy(R(2), pb + FileParam_ioFlFndrInfo_off, 16);
    RET(e);
}

static void g_SetFInfo(ppc_cpu *c)
{
    gaddr pb = pb_scratch();
    wr32(pb + IOParam_ioNamePtr_off, R(0));
    wr16(pb + IOParam_ioVRefNum_off, (uint16_t)R(1));
    int16_t e = tb_PBGetFInfo(pb, false);
    if (!e && R(2)) {
        gmemcpy(pb + FileParam_ioFlFndrInfo_off, R(2), 16);
        e = tb_PBSetFInfo(pb, false);
    }
    RET(e);
}

/* HGetFInfo / HSetFInfo(vRefNum, dirID, fileName, *fndrInfo) */
static void g_HGetFInfo(ppc_cpu *c)
{
    gaddr pb = pb_scratch();
    wr16(pb + IOParam_ioVRefNum_off, (uint16_t)R(0));
    wr32(pb + HFileParam_ioDirID_off, R(1));
    wr32(pb + IOParam_ioNamePtr_off, R(2));
    int16_t e = tb_PBHGetFInfo(pb, false);
    if (!e && R(3))
        gmemcpy(R(3), pb + FileParam_ioFlFndrInfo_off, 16);
    RET(e);
}

static void g_HSetFInfo(ppc_cpu *c)
{
    gaddr pb = pb_scratch();
    wr16(pb + IOParam_ioVRefNum_off, (uint16_t)R(0));
    wr32(pb + HFileParam_ioDirID_off, R(1));
    wr32(pb + IOParam_ioNamePtr_off, R(2));
    int16_t e = tb_PBHGetFInfo(pb, false);
    if (!e && R(3)) {
        gmemcpy(pb + FileParam_ioFlFndrInfo_off, R(3), 16);
        e = tb_PBHSetFInfo(pb, false);
    }
    RET(e);
}

/* DirCreate(vRefNum, parentDirID, directoryName, *createdDirID) */
static void g_DirCreate(ppc_cpu *c)
{
    gaddr pb = pb_scratch();
    wr16(pb + IOParam_ioVRefNum_off, (uint16_t)R(0));
    wr32(pb + HFileParam_ioDirID_off, R(1));
    wr32(pb + IOParam_ioNamePtr_off, R(2));
    int16_t e = tb_PBDirCreate(pb, false);
    out32(R(3), rd32(pb + HFileParam_ioDirID_off));
    RET(e);
}

/* Sorted by name. */
/* OpenDF(fileName, vRefNum, *refNum) */
static void g_OpenDF(ppc_cpu *c)
{
    gaddr pb = pb_scratch();
    wr32(pb + IOParam_ioNamePtr_off, R(0));
    wr16(pb + IOParam_ioVRefNum_off, (uint16_t)R(1));
    int16_t e = tb_PBHOpenDF(pb, false);
    out16(R(2), rd16(pb + IOParam_ioRefNum_off));
    RET(e);
}

/* OpenWD(vRefNum, dirID, procID, *wdRefNum) / CloseWD(wdRefNum) */
static void g_OpenWD(ppc_cpu *c)
{
    gaddr pb = pb_scratch();
    wr16(pb + IOParam_ioVRefNum_off, (uint16_t)R(0));
    wr32(pb + 48, R(1)); /* ioWDDirID */
    wr32(pb + 28, R(2)); /* ioWDProcID */
    int16_t e = tb_PBOpenWD(pb, false);
    out16(R(3), rd16(pb + IOParam_ioVRefNum_off));
    RET(e);
}
static void g_CloseWD(ppc_cpu *c)
{
    gaddr pb = pb_scratch();
    wr16(pb + IOParam_ioVRefNum_off, (uint16_t)R(0));
    RET(tb_PBCloseWD(pb, false));
}

/* GetVInfo(drvNum, volName, *vRefNum, *freeBytes) */
static void g_GetVInfo(ppc_cpu *c)
{
    gaddr pb = pb_scratch();
    wr32(pb + IOParam_ioNamePtr_off, R(1));
    wr16(pb + IOParam_ioVRefNum_off, (uint16_t)R(0));
    wr16(pb + VolumeParam_ioVolIndex_off, 0);
    int16_t e = tb_PBGetVInfo(pb, false);
    out16(R(2), rd16(pb + IOParam_ioVRefNum_off));
    out32(R(3), (uint32_t)rd16(pb + VolumeParam_ioVFrBlk_off) * rd32(pb + VolumeParam_ioVAlBlkSiz_off));
    RET(e);
}

/* CatMove(vRefNum, dirID, oldName, newDirID, newName) */
static void g_CatMove(ppc_cpu *c)
{
    gaddr pb = pb_scratch();
    wr32(pb + IOParam_ioNamePtr_off, R(2));
    wr16(pb + IOParam_ioVRefNum_off, (uint16_t)R(0));
    wr32(pb + HFileParam_ioDirID_off, R(1));
    wr32(pb + CMovePBRec_ioNewName_off, R(4));
    wr32(pb + CMovePBRec_ioNewDirID_off, R(3));
    RET(tb_PBCatMove(pb, false));
}

/* Eject(volName, vRefNum) / UnmountVol(volName, vRefNum) */
static void g_Eject(ppc_cpu *c)
{
    gaddr pb = pb_scratch();
    wr32(pb + IOParam_ioNamePtr_off, R(0));
    wr16(pb + IOParam_ioVRefNum_off, (uint16_t)R(1));
    RET(tb_PBEject(pb));
}
static void g_UnmountVol(ppc_cpu *c)
{
    gaddr pb = pb_scratch();
    wr32(pb + IOParam_ioNamePtr_off, R(0));
    wr16(pb + IOParam_ioVRefNum_off, (uint16_t)R(1));
    RET(tb_PBUnmountVol(pb));
}

const ppc_export ppc_glue_exports[] = {
    { "CatMove", g_CatMove },
    { "CloseWD", g_CloseWD },
    { "Eject", g_Eject },
    { "GetVInfo", g_GetVInfo },
    { "OpenDF", g_OpenDF },
    { "OpenWD", g_OpenWD },
    { "UnmountVol", g_UnmountVol },
    { "Allocate", g_Allocate },   { "Create", g_Create },       { "DirCreate", g_DirCreate },
    { "FSClose", g_FSClose },     { "FSDelete", g_FSDelete },   { "FSOpen", g_FSOpen },
    { "FSRead", g_FSRead },       { "FSWrite", g_FSWrite },     { "FlushVol", g_FlushVol },
    { "GetEOF", g_GetEOF },       { "GetFInfo", g_GetFInfo },   { "GetFPos", g_GetFPos },
    { "GetVRefNum", g_GetVRefNum }, { "GetVol", g_GetVol },     { "HCreate", g_HCreate },
    { "HDelete", g_HDelete },     { "HGetFInfo", g_HGetFInfo }, { "HGetVol", g_HGetVol },
    { "HOpen", g_HOpen },         { "HOpenDF", g_HOpenDF },     { "HOpenRF", g_HOpenRF },
    { "HRename", g_HRename },     { "HSetFInfo", g_HSetFInfo }, { "HSetVol", g_HSetVol },
    { "OpenRF", g_OpenRF },       { "Rename", g_Rename },       { "SetEOF", g_SetEOF },
    { "SetFInfo", g_SetFInfo },   { "SetFPos", g_SetFPos },     { "SetVol", g_SetVol },
};
const unsigned ppc_glue_export_count = sizeof ppc_glue_exports / sizeof ppc_glue_exports[0];
