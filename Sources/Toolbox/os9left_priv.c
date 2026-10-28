/*
 * PrivateInterfaceLib's remaining names that Blue Box can back.  Mac OS 9's
 * were calls into the ROM's private dispatchers; the arguments here are
 * what the names say and the state they act on is Blue Box's.
 *
 * The Memory Manager's: the Process Manager's zone is the application's
 * (GetProcessMgrLoFreeBytes, ProcessMgrMaxBlock), the system heap's free
 * space and slop, a zone's compatibility flags kept per zone, the blocks of
 * a heap walked (GetNextBlockInHeap(zone, block) gives the next block's
 * data, nil past the last), TrashMemoryArea.
 *
 * Offscreen buffers for windows: NewOffscreenBuffer(bounds, depth, *buffer)
 * is a GWorld; AttachOffscreenBuffer(window, buffer) ties one to a
 * window, BeginDrawingOffscreen(window) makes drawing go to it and
 * EndDrawingOffscreen(window) copies it to the window, DetachOffscreenBuffer
 * (window, *buffer) takes it back.
 *
 * Left out (see the report): the Code Fragment Manager's internals (the Frag*
 * and CFL* calls), the 68k emulator's (Emulated68KContext, EmulatorReturn),
 * the virtual memory manager's (VM_*), the Window Manager's layers and
 * window state, the Process Manager's debugger interface, the Palette
 * Manager's and the Script Manager's private helpers: they act on ROM
 * structures Blue Box does not keep.
 */
#include <stdlib.h>
#include <string.h>

#include "m68k.h"
#include "ppc.h"
#include "ppc_bridge.h"
#include "tb_internal.h"
#include "trap.h"
#include "os9rest.h"
#include "os9left.h"



/* ---- The Memory Manager's ---- */

static void pv_GetProcessMgrLoFreeBytes(ppc_cpu *c) { RETURN(tb_FreeMem(false)); }
static void pv_ProcessMgrMaxBlock(ppc_cpu *c) { RETURN(tb_MaxBlock(false)); }
static void pv_GetSystemHiFreeBytes(ppc_cpu *c) { RETURN(tb_FreeMem(true)); }
static void pv_EnoughSystemHeapSlop(ppc_cpu *c) { RETURN(tb_FreeMem(true) >= (int32_t)R(0)); }

/* ReclaimSpaceIfSysHeapLow(): the system heap compacted when it has little
 * free; the bytes now free. */
static void pv_ReclaimSpaceIfSysHeapLow(ppc_cpu *c)
{
    if (tb_FreeMem(true) < 64 * 1024)
        tb_CompactMem(0x7FFFFFFF, true);
    RETURN(tb_FreeMem(true));
}

#define MAX_ZONE_FLAGS 8
static struct { gaddr zone; uint32_t flags; } zone_flags[MAX_ZONE_FLAGS];

static void pv_GetZoneCompatibilityFlags(ppc_cpu *c)
{
    for (int i = 0; i < MAX_ZONE_FLAGS; i++)
        if (zone_flags[i].zone == R(0))
            RETURN(zone_flags[i].flags);
    RETURN(0);
}

static void pv_SetZoneCompatibilityFlags(ppc_cpu *c)
{
    int slot = -1;
    for (int i = 0; i < MAX_ZONE_FLAGS; i++) {
        if (zone_flags[i].zone == R(0)) {
            slot = i;
            break;
        }
        if (!zone_flags[i].zone && slot < 0)
            slot = i;
    }
    if (slot < 0 || !R(0))
        RETURN(slot < 0 ? memFullErr : paramErr);
    zone_flags[slot].zone = R(0);
    zone_flags[slot].flags = R(1);
    RETURN(noErr);
}

/* GetNextBlockInHeap(zone, block): the block after the one whose data is at
 * `block` (the first when 0), as the address of its data; 0 at the end. */
static void pv_GetNextBlockInHeap(ppc_cpu *c)
{
    gaddr zone = R(0) ? R(0) : rd32(LM_ApplZone), block = R(1);
    gaddr limit = rd32(zone), hdr = 12;
    gaddr at = zone + 52;
    if (block) {
        gaddr cur = block - hdr;
        if (cur < zone + 52 || cur >= limit)
            RETURN(0);
        uint32_t size = rd32(cur + 4);
        if (!size)
            RETURN(0);
        at = cur + size;
    }
    if (at >= limit)
        RETURN(0);
    RETURN(at + hdr);
}

/* TrashMemoryArea(start, size): the bytes made the pattern debugging fills
 * freed memory with. */
static void pv_TrashMemoryArea(ppc_cpu *c)
{
    if (!R(0) || !R(1))
        RETURN(paramErr);
    for (uint32_t i = 0; i < R(1); i++)
        wr8(R(0) + i, (i & 1) ? 0x61 : 0x72);
    RETURN(noErr);
}

/* ---- Offscreen buffers for windows ---- */

#define MAX_BUFFERS 16
static struct { gaddr window, buffer; bool drawing; gaddr saved_port, saved_dev; } attached[MAX_BUFFERS];

static void pv_NewOffscreenBuffer(ppc_cpu *c)
{
    if (!R(0) || !R(2))
        RETURN(paramErr);
    gaddr cell = tb_NewPtr(4, false, true);
    if (!cell)
        RETURN(memFullErr);
    int16_t e = tb_NewGWorld(cell, (int16_t)R(1), R(0), 0, 0, 0);
    wr32(R(2), e ? 0 : rd32(cell));
    tb_DisposePtr(cell);
    RETURN(e);
}

static void pv_DisposeOffscreenBuffer(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    for (int i = 0; i < MAX_BUFFERS; i++)
        if (attached[i].buffer == R(0))
            memset(&attached[i], 0, sizeof attached[i]);
    tb_DisposeGWorld(R(0));
    RETURN(noErr);
}

static int slot_for(gaddr window, bool make)
{
    int free_slot = -1;
    for (int i = 0; i < MAX_BUFFERS; i++) {
        if (attached[i].window == window)
            return i;
        if (!attached[i].window && free_slot < 0)
            free_slot = i;
    }
    return make ? free_slot : -1;
}

static void pv_AttachOffscreenBuffer(ppc_cpu *c)
{
    if (!R(0) || !R(1))
        RETURN(paramErr);
    int s = slot_for(R(0), true);
    if (s < 0)
        RETURN(memFullErr);
    attached[s].window = R(0);
    attached[s].buffer = R(1);
    RETURN(noErr);
}

static void pv_DetachOffscreenBuffer(ppc_cpu *c)
{
    int s = slot_for(R(0), false);
    if (s < 0)
        RETURN(paramErr);
    if (R(1))
        wr32(R(1), attached[s].buffer);
    memset(&attached[s], 0, sizeof attached[s]);
    RETURN(noErr);
}

static void pv_BeginDrawingOffscreen(ppc_cpu *c)
{
    int s = slot_for(R(0), false);
    if (s < 0)
        RETURN(paramErr);
    if (attached[s].drawing)
        RETURN(noErr);
    gaddr cells = tb_NewPtr(8, false, true);
    if (!cells)
        RETURN(memFullErr);
    tb_GetGWorld(cells, cells + 4);
    attached[s].saved_port = rd32(cells);
    attached[s].saved_dev = rd32(cells + 4);
    tb_DisposePtr(cells);
    tb_LockPixels(tb_GetGWorldPixMap(attached[s].buffer));
    tb_SetGWorld(attached[s].buffer, 0);
    attached[s].drawing = true;
    RETURN(noErr);
}

/* EndDrawingOffscreen(window): what was drawn is copied to the window
 * (the whole of the buffer's bounds), and drawing goes where it did. */
/* The bit or pixel map a port draws into. */
static gaddr port_bitmap(gaddr port)
{
    if ((rd16(port + 6) & 0xC000) == 0xC000) /* a CGrafPort: its pixel map's handle */
        return rd32(rd32(port + CGrafPort_portPixMap_off));
    return port + GrafPort_portBits_off;
}

static void pv_EndDrawingOffscreen(ppc_cpu *c)
{
    int s = slot_for(R(0), false);
    if (s < 0 || !attached[s].drawing)
        RETURN(paramErr);
    gaddr buf = attached[s].buffer, win = R(0);
    tb_SetGWorld(attached[s].saved_port, attached[s].saved_dev);
    gaddr bounds = buf + GrafPort_portRect_off;
    tb_SetPort(win);
    tb_CopyBits(port_bitmap(buf), port_bitmap(win), bounds, bounds, 0, 0);
    tb_SetGWorld(attached[s].saved_port, attached[s].saved_dev);
    attached[s].drawing = false;
    RETURN(noErr);
}

/* ---- The Process Manager's ---- */

static void pv_GetCurrentAppRefNum(ppc_cpu *c) { RETURN((int16_t)rd16(LM_CurApRefNum)); }

/* GetProcessTrapAddress(trapNum) and SetProcessTrapAddress(address,
 * trapNum): the one process's patches are the machine's. */
static void pv_GetProcessTrapAddress(ppc_cpu *c) { RETURN(os9rest_call(c, "GetTrapAddress", (uint32_t[]){ R(0) }, 1)); }

static void pv_SetProcessTrapAddress(ppc_cpu *c)
{
    os9rest_call(c, "SetTrapAddress", (uint32_t[]){ R(0), R(1) }, 2);
    c->pc = c->lr;
}

/* GetSystemClientProcess(*psn): the process the system serves is the one running. */
static void pv_GetSystemClientProcess(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    RETURN(tb_GetCurrentProcess(R(0)));
}

static void pv_ResumeProcess(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    RETURN(tb_WakeUpProcess(R(0)));
}

/* ReadWriteProcessMemory(psn, write, address, buffer, size): the process's
 * memory is the machine's. */
static void pv_ReadWriteProcessMemory(ppc_cpu *c)
{
    if (!R(0) || !R(2) || !R(3))
        RETURN(paramErr);
    gaddr psn = R(0);
    gaddr cur = tb_NewPtr(8, false, true);
    if (!cur)
        RETURN(memFullErr);
    tb_GetCurrentProcess(cur);
    bool same = rd32(cur) == rd32(psn) && rd32(cur + 4) == rd32(psn + 4);
    tb_DisposePtr(cur);
    if (!same)
        RETURN(-600); /* procNotFound: no other process */
    uint8_t *tmp = malloc(R(4) ? R(4) : 1);
    if (!tmp)
        RETURN(memFullErr);
    if (R(1) & 0xFF) {
        gget(tmp, R(3), R(4));
        gput(R(2), tmp, R(4));
    } else {
        gget(tmp, R(2), R(4));
        gput(R(3), tmp, R(4));
    }
    free(tmp);
    RETURN(noErr);
}

/* PostFakeKeyDown(character, modifiers): a key-down in the event queue. */
static void pv_PostFakeKeyDown(ppc_cpu *c)
{
    int32_t message = (int32_t)(R(0) & 0xFFFF);
    RETURN(tb_PostEvent(3, message));
}

/* ---- The Window Manager's, and others that are other calls ---- */

static void pv_GetDeskPort(ppc_cpu *c) { RETURN(rd32(LM_WMgrPort)); }

/* RedrawAll(): every window and the menu bar drawn again. */
static void pv_RedrawAll(ppc_cpu *c)
{
    tb_PaintBehind(0, rd32(LM_GrayRgn));
    tb_DrawMenuBar();
    c->pc = c->lr;
}

static void pv_FindFolderInternalExtended(ppc_cpu *c)
{
    uint32_t a[8];
    for (int i = 0; i < 7; i++)
        a[i] = R(i);
    RETURN(os9rest_call(c, "FindFolderExtended", a, 7));
}

/* RealScript(script): the script a script code means: the system's for
 * smSystemScript (-1), the current font's for smCurrentScript (-2). */
static void pv_RealScript(ppc_cpu *c)
{
    int16_t s = (int16_t)R(0);
    if (s == -1)
        RETURN((int16_t)tb_GetScriptManagerVariable(18 /* smSysScript */));
    if (s == -2)
        RETURN((int16_t)tb_FontScript());
    RETURN(s);
}

/* CheckWindow(window): is it a window of the window list? */
static void pv_CheckWindow(ppc_cpu *c)
{
    for (gaddr w = rd32(LM_WindowList); w; w = rd32(w + WindowRecord_nextWindow_off))
        if (w == R(0))
            RETURN(1);
    RETURN(0);
}

/* EachWindow(proc, refCon): proc(window, refCon) for the windows front to
 * back until it answers true; that window, or nil. */
static void pv_EachWindow(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    for (gaddr w = rd32(LM_WindowList); w; w = rd32(w + WindowRecord_nextWindow_off)) {
        uint32_t args[2] = { w, R(1) };
        uint32_t r = mm_is_descriptor(R(0)) ? mm_call(R(0), 0x3D0, args, 2) : ppc_call(R(0), args, 2);
        if (r & 0xFF)
            RETURN(w);
    }
    RETURN(0);
}

void os9left_priv_reset(void)
{
    memset(zone_flags, 0, sizeof zone_flags);
    memset(attached, 0, sizeof attached);
}

const ppc_export os9left_priv_exports[] = {
    { "AttachOffscreenBuffer", pv_AttachOffscreenBuffer },
    { "BeginDrawingOffscreen", pv_BeginDrawingOffscreen },
    { "CheckWindow", pv_CheckWindow },
    { "DetachOffscreenBuffer", pv_DetachOffscreenBuffer },
    { "DisposeOffscreenBuffer", pv_DisposeOffscreenBuffer },
    { "EachWindow", pv_EachWindow },
    { "EndDrawingOffscreen", pv_EndDrawingOffscreen },
    { "EnoughSystemHeapSlop", pv_EnoughSystemHeapSlop },
    { "FindFolderInternalExtended", pv_FindFolderInternalExtended },
    { "GetCurrentAppRefNum", pv_GetCurrentAppRefNum },
    { "GetDeskPort", pv_GetDeskPort },
    { "GetNextBlockInHeap", pv_GetNextBlockInHeap },
    { "GetProcessMgrLoFreeBytes", pv_GetProcessMgrLoFreeBytes },
    { "GetProcessTrapAddress", pv_GetProcessTrapAddress },
    { "GetSystemClientProcess", pv_GetSystemClientProcess },
    { "GetSystemHiFreeBytes", pv_GetSystemHiFreeBytes },
    { "GetZoneCompatibilityFlags", pv_GetZoneCompatibilityFlags },
    { "NewOffscreenBuffer", pv_NewOffscreenBuffer },
    { "PostFakeKeyDown", pv_PostFakeKeyDown },
    { "ProcessMgrMaxBlock", pv_ProcessMgrMaxBlock },
    { "ReadWriteProcessMemory", pv_ReadWriteProcessMemory },
    { "RealScript", pv_RealScript },
    { "RedrawAll", pv_RedrawAll },
    { "ReclaimSpaceIfSysHeapLow", pv_ReclaimSpaceIfSysHeapLow },
    { "ResumeProcess", pv_ResumeProcess },
    { "SetProcessTrapAddress", pv_SetProcessTrapAddress },
    { "SetZoneCompatibilityFlags", pv_SetZoneCompatibilityFlags },
    { "TrashMemoryArea", pv_TrashMemoryArea },
};
const unsigned os9left_priv_export_count = sizeof os9left_priv_exports / sizeof os9left_priv_exports[0];
