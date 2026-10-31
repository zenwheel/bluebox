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

enum { unimpErr = -4, procNotFound = -600, kDMNoDeviceTableclothErr = -6231 };
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

/* MoveHLow(h): down to the bottom of the free space and unlocked
 * relocatable blocks below it, as MoveHHi moves one up. */
static void p_MoveHLow(ppc_cpu *c)
{
    wr16(LM_MemErr, (uint16_t)mm_move_handle(R(0), false));
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

/* ---- Virtual memory and the sound manager ---- */

/* Blue Box's memory is all there and nothing pages: the physical and
 * logical sizes are the same, nothing needs holding, there is nothing to
 * release, no file maps into memory and no page can be protected (a
 * Mac with virtual memory off, or without the hardware, answered
 * unimpErr to the calls that need it). */
static void p_RamBytes(ppc_cpu *c)
{
    uint32_t ram = 0;
    tb_Gestalt((int32_t)fourcc("ram "), &ram);
    RETURN(ram);
}

static void p_Unimplemented(ppc_cpu *c) { RETURN(unimpErr); }
static void p_NoPagingDone(ppc_cpu *c) { RETURN(noErr); }
static void p_Nothing(ppc_cpu *c) { DONE(); }

/* PMScaledBattery(whichBattery): the BatteryInfo GetScaledBatteryInfo
 * stores, returned (a Mac with no battery installed says zero). */
static void p_PMScaledBattery(ppc_cpu *c)
{
    uint32_t info = 0;
    tb_GetScaledBatteryInfo((int16_t)R(0), &info);
    RETURN(info);
}

/* The Speech Manager's clean-up: the program's channels are disposed of;
 * there are no private channels of the system's own to kill. */
void speech_kill_channels(void);
static void p_KillProcessChannels(ppc_cpu *c)
{
    speech_kill_channels();
    RETURN(noErr);
}

/* The TrueType scaler (FontManager's fs_* and the Font Manager's own
 * fs__ names for the same routines).  Blue Box draws only bitmap strikes
 * (qd_text.c reads an 'sfnt' for metrics alone) and has no scaler to run:
 * fs_OpenFonts and fs_CloseFonts are what Mac OS 9's are (the first clears
 * the scaler's input and gives the sizes of the memory it wants, the second
 * does nothing), and the entry points that need a scaler to give a
 * transformation or scan a glyph with answer unimpErr. */
static void p_fs_OpenFonts(ppc_cpu *c)
{
    gaddr in = R(0), out = R(1);
    for (int i = 0; in && i < 9; i++)
        wr32(in + 4u * (uint32_t)i, 0);
    if (out) {
        static const uint32_t sizes[8] = { 0x33C, 0x404, 0x834, 0, 0, 0, 0, 0 };
        for (int i = 0; i < 8; i++)
            wr32(out + 4u * (uint32_t)i, sizes[i]);
    }
    RETURN(0);
}
static void p_NoScaler(ppc_cpu *c) { RETURN(unimpErr); }

/* The Font Manager's caches and disk cache, partial fonts and glyph
 * retrieval: Mac OS 9's own routines for them are an empty answer, and
 * an sfnt always matches the size asked. */
static void p_FontZero(ppc_cpu *c) { RETURN(0); }
static void p_FontOne(ppc_cpu *c) { RETURN(1); }

void sound_app_dead(void);
static void p_SndAppDead(ppc_cpu *c)
{
    sound_app_dead();
    DONE();
}

const ppc_export os9rest_priv_exports[] = {
    { "fs_OpenFonts", p_fs_OpenFonts }, { "fs__OpenFonts", p_fs_OpenFonts },
    { "fs_Initialize", p_NoScaler }, { "fs__Initialize", p_NoScaler },
    { "fs_NewSfnt", p_NoScaler }, { "fs__NewSfnt", p_NoScaler },
    { "fs_NewTransformation", p_NoScaler }, { "fs__NewTransformation", p_NoScaler },
    { "fs_NewGlyph", p_NoScaler }, { "fs__NewGlyph", p_NoScaler },
    { "fs_ContourGridFit", p_NoScaler }, { "fs__ContourGridFit", p_NoScaler },
    { "fs_ContourNoGridFit", p_NoScaler }, { "fs__ContourNoGridFit", p_NoScaler },
    { "fs_ContourScan", p_NoScaler }, { "fs__ContourScan", p_NoScaler },
    { "fs_CloseFonts", p_FontZero }, { "fs__CloseFonts", p_FontZero },
    { "fs_GetAdvanceWidth", p_NoScaler }, { "fs__GetAdvanceWidth", p_NoScaler },
    { "fs_SizeOfOutlines", p_NoScaler }, { "fs__SizeOfOutlines", p_NoScaler },
    { "fs_SaveOutlines", p_NoScaler }, { "fs__SaveOutlines", p_NoScaler },
    { "fs_RestoreOutlines", p_NoScaler }, { "fs__RestoreOutlines", p_NoScaler },
    { "fs_FindBitMapSize", p_NoScaler },
    { "__FillWidthTab", p_FontZero }, { "sb__FillWidthTab", p_FontZero },
    { "__InitializeDiskCache", p_FontZero }, { "InitializeDiskCache", p_FontZero },
    { "__InitializePartialFonts", p_FontZero }, { "InitializePartialFonts", p_FontZero },
    { "__KillSomeCaches", p_FontZero }, { "sb__KillSomeCaches", p_FontZero },
    { "__PreFlightFontMem", p_FontZero }, { "sb__PreFlightFontMem", p_FontZero },
    { "__RetrieveGlyph", p_FontZero }, { "sb__RetrieveGlyph", p_FontZero },
    { "__SearchForCache", p_FontZero }, { "sb__SearchForCache", p_FontZero },
    { "__SFNTMatchSize", p_FontOne }, { "sb__SFNTMatchSize", p_FontOne },
    { "CleanUpGlyphRecordArray", p_FontZero },
    { "PMScaledBattery", p_PMScaledBattery }, { "VM_GetPhysicalBytes", p_RamBytes }, { "VM_GetLogicalBytes", p_RamBytes },
    { "VM_HoldSystemHeap", p_NoPagingDone }, { "VM_ReleaseMemory", p_NoPagingDone },
    { "VM_IterateMappedRanges", p_NoPagingDone }, { "VM_FinishInstall", p_Nothing },
    { "VM_SetVMJCrsrTask", p_Nothing }, { "VM_GrowFileMapping", p_Unimplemented },
    { "WriteProtectMemory", p_Unimplemented }, { "UnWriteProtectMemory", p_Unimplemented },
    { "VMWriteProtectMemory", p_Unimplemented }, { "VMUnWriteProtectMemory", p_Unimplemented },
    { "MapFile", p_Unimplemented }, { "UnMapFile", p_Unimplemented },
    { "SndAppDead", p_SndAppDead }, { "KillProcessChannels", p_KillProcessChannels },
    { "ShutDownSpeechManager", p_KillProcessChannels }, { "KillIdlePrivateChannels", p_AlreadyDone }, { "SynchIdleTime", p_Nothing },
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
    { "DragInitProc", p_AlreadyDone }, { "ReloadContextualMenuPlugins", p_Nothing },
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
