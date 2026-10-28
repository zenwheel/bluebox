/*
 * PrivateInterfaceLib's and InterfaceLib's private names that Blue Box has
 * state to answer on beyond private_os9.c's: Mac OS 9's calls into the
 * Toolbox's own tables.  What is here is what the name and the glue around
 * the call show plainly: a resource file's map, QuickDraw's globals, the
 * 68k emulator's registers, the menu bar's rectangles, the start-up and
 * clean-up calls whose work Blue Box has done when a program runs, the
 * display's obsolete tablecloth.
 */
#include <string.h>

#include "m68k.h"
#include "os9rest.h"
#include "ppc.h"
#include "ppc_bridge.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(int32_t)(v); c->pc = c->lr; return; } while (0)
#define DONE() do { c->pc = c->lr; return; } while (0)

enum { procNotFound = -600, kDMNoDeviceTableclothErr = -6231 };
bool menu_title_rect_of(int16_t menuID, int16_t out[4]);
void menu_bar_rect(int16_t out[4]);
gaddr rm_map_handle(int16_t rn);

/* ---- Resources ---- */

/* GetMap(refNum): the map handle of an open resource file. */
static void p_GetMap(ppc_cpu *c) { RETURN(rm_map_handle((int16_t)R(0))); }

/* IsThisASystemResourceMap(map): the System file's. */
static void p_IsThisASystemResourceMap(ppc_cpu *c) { RETURN(R(0) && R(0) == rd32(LM_SysMapHndl)); }

/* ---- QuickDraw ---- */

/* GetQDGlobals(): the QuickDraw globals of the current program, which end
 * at thePort, A5's pointer to them. */
static void p_GetQDGlobals(ppc_cpu *c)
{
    gaddr a5 = rd32(LM_CurrentA5);
    gaddr g = a5 ? rd32(a5) : 0;
    RETURN(g ? g - QDGlobals_thePort_off : 0);
}

/* GlobalPortRect(port, *rect): the port's rectangle in global coordinates. */
static void p_GlobalPortRect(ppc_cpu *c)
{
    gaddr port = R(0), rect = R(1);
    if (!port || !rect)
        DONE();
    gaddr bounds = port + GrafPort_portBits_off + BitMap_bounds_off;
    int16_t dv = (int16_t)(-(int16_t)rd16(bounds)), dh = (int16_t)(-(int16_t)rd16(bounds + 2));
    gaddr pr = port + GrafPort_portRect_off;
    wr16(rect, (uint16_t)((int16_t)rd16(pr) + dv));
    wr16(rect + 2, (uint16_t)((int16_t)rd16(pr + 2) + dh));
    wr16(rect + 4, (uint16_t)((int16_t)rd16(pr + 4) + dv));
    wr16(rect + 6, (uint16_t)((int16_t)rd16(pr + 6) + dh));
    DONE();
}

/* ---- The 68k emulator ---- */

/* The 68k registers PowerPC code asks the emulator for: D0 to D7 are
 * 0 to 7, A0 to A6 are 8 to 14; any other number is returned as it came. */
static void p_GetEmulatorRegister(ppc_cpu *c)
{
    if (R(0) > 14 || !g_cpu)
        DONE();
    RETURN(R(0) < 8 ? g_cpu->d[R(0)] : g_cpu->a[R(0) - 8]);
}

static void p_SetEmulatorRegister(ppc_cpu *c)
{
    if (R(0) <= 14 && g_cpu) {
        if (R(0) < 8)
            g_cpu->d[R(0)] = R(1);
        else
            g_cpu->a[R(0) - 8] = R(1);
    }
    DONE();
}

/* Code written for the processor to run: translations go when the code is
 * stored over, so nothing is left to flush. */
static void p_NothingToFlush(ppc_cpu *c) { DONE(); }

/* The system's exception handler (a transition vector), swapped; the old
 * one comes back. */
static gaddr exception_handler;
static void p_InstallSystemExceptionHandler(ppc_cpu *c)
{
    gaddr old = exception_handler;
    exception_handler = R(0);
    RETURN(old);
}

/* ---- Start-up and clean-up ---- */

/* Calls the Toolbox makes for itself when it starts, when a program starts
 * and when it ends: the script system, layers, Folder Manager and the
 * rest are set up when the machine boots and a program runs. */
static void p_AlreadyDone(ppc_cpu *c) { RETURN(noErr); }
static void p_PrintCopyright(ppc_cpu *c) { DONE(); } /* to the debugger's console, which there is none of */

/* MoveHLow(h): to the low end of the heap, where nothing watches the
 * difference: as MoveHHi, the block stays and the call succeeds. */
static void p_MoveHLow(ppc_cpu *c)
{
    wr16(LM_MemErr, noErr);
    DONE();
}

/* ---- Menus ---- */

/* GetMBARRect(*rect): the menu bar. */
static void p_GetMBARRect(ppc_cpu *c)
{
    int16_t r[4];
    if (!R(0))
        RETURN(paramErr);
    menu_bar_rect(r);
    for (int i = 0; i < 4; i++)
        wr16(R(0) + 2u * (uint32_t)i, (uint16_t)r[i]);
    RETURN(noErr);
}

/* GetMenuTitleRect(menuID, *rect): a menu's title in the bar. */
static void p_GetMenuTitleRect(ppc_cpu *c)
{
    int16_t r[4];
    if (!R(1))
        RETURN(paramErr);
    if (!menu_title_rect_of((int16_t)R(0), r))
        RETURN(-127); /* menuPrgErr: the menu is not in the bar */
    for (int i = 0; i < 4; i++)
        wr16(R(1) + 2u * (uint32_t)i, (uint16_t)r[i]);
    RETURN(noErr);
}

/* ---- Processes ---- */

enum { OUR_PSN = 0x1000 };

/* GetProcessFromLayer(layer, *psn): the program that owns a layer; there
 * is the one (the application's layer), and the others are nobody's. */
static void p_GetProcessFromLayer(ppc_cpu *c)
{
    if (!R(1))
        RETURN(paramErr);
    if (!R(0) || R(0) != tb_GetCurLayer())
        RETURN(procNotFound);
    wr32(R(1), 0);
    wr32(R(1) + 4, OUR_PSN);
    RETURN(noErr);
}

/* KillProcess(psn): this program ends as ExitToShell ends it; the Finder
 * and other runs are not ours to kill. */
static void p_KillProcess(ppc_cpu *c)
{
    gaddr psn = R(0);
    if (!psn)
        RETURN(paramErr);
    if (rd32(psn) == 0 && (rd32(psn + 4) == OUR_PSN || rd32(psn + 4) == 2 /* kCurrentProcess */)) {
        void (*fn)(ppc_cpu *) = cfm_export_fn("ExitToShell");
        if (fn)
            fn(c);
        DONE();
    }
    RETURN(procNotFound);
}

const ppc_export os9rest_priv_exports[] = {
    { "GetMap", p_GetMap }, { "IsThisASystemResourceMap", p_IsThisASystemResourceMap },
    { "GetQDGlobals", p_GetQDGlobals }, { "GlobalPortRect", p_GlobalPortRect },
    { "GetEmulatorRegister", p_GetEmulatorRegister }, { "SetEmulatorRegister", p_SetEmulatorRegister },
    { "MakeExecutable", p_NothingToFlush }, { "FlushToExecute", p_NothingToFlush },
    { "FlushEmulatorCacheRange", p_NothingToFlush },
    { "InstallSystemExceptionHandler", p_InstallSystemExceptionHandler },
    { "InitScripts", p_AlreadyDone }, { "ReInitScripts", p_AlreadyDone }, { "InitScriptApp", p_AlreadyDone },
    { "CleanupScriptApp", p_AlreadyDone }, { "InitLayers", p_AlreadyDone },
    { "InitializeFolderManager", p_AlreadyDone }, { "InitKeybdMenu", p_AlreadyDone },
    { "InitApplication", p_AlreadyDone }, { "CleanupApplication", p_AlreadyDone },
    { "IUClearCacheSys", p_AlreadyDone },
    { "PrintCopyright", p_PrintCopyright }, { "MoveHLow", p_MoveHLow },
    { "GetMBARRect", p_GetMBARRect }, { "GetMenuTitleRect", p_GetMenuTitleRect },
    { "GetProcessFromLayer", p_GetProcessFromLayer }, { "KillProcess", p_KillProcess },
};
const unsigned os9rest_priv_export_count = sizeof os9rest_priv_exports / sizeof os9rest_priv_exports[0];

void os9rest_priv_reset(void) { exception_handler = 0; }

/* ---- The Display Manager's tablecloth ---- */

#define MAX_CLOTHS 8
static struct { gaddr device, cloth; } cloths[MAX_CLOTHS];

void os9rest_cloth_reset(void) { memset(cloths, 0, sizeof cloths); }

int16_t tb_DMGetGDeviceTablecloth(gaddr displayDevice, gaddr tablecloth)
{
    if (!displayDevice || !tablecloth)
        return paramErr;
    wr32(tablecloth, 0);
    for (int i = 0; i < MAX_CLOTHS; i++)
        if (cloths[i].device == displayDevice && cloths[i].cloth) {
            wr32(tablecloth, cloths[i].cloth);
            return noErr;
        }
    return kDMNoDeviceTableclothErr;
}

int16_t tb_DMSetGDeviceTablecloth(gaddr displayDevice, gaddr tablecloth)
{
    if (!displayDevice)
        return paramErr;
    int slot = -1;
    for (int i = 0; i < MAX_CLOTHS; i++) {
        if (cloths[i].device == displayDevice) {
            slot = i;
            break;
        }
        if (slot < 0 && !cloths[i].device)
            slot = i;
    }
    if (slot < 0)
        return memFullErr;
    cloths[slot].device = tablecloth ? displayDevice : 0;
    cloths[slot].cloth = tablecloth;
    return noErr;
}
