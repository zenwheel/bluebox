/*
 * DisplayLib's remaining exports: the Display Manager's internals and the
 * calls the public interface (dmgr.c) leaves, on Blue Box's one display and
 * its GDevice.  Where a call took a configuration the screen can't change,
 * it says so as dmgr.c does.
 *
 *  - DMSetDisplayID gives the display another ID (dmgr.c's calls use it).
 *  - The AuxGDevice flags (DMSetAuxGDeviceFlag(gd, flag, on),
 *    DMTestAuxGDeviceFlag(gd, flag)) are kept per GDevice.
 *  - DMGetBestDepthModeInfo(gd, wantedDepth, *depthMode, *depth) is the
 *    smallest depth of 8, 16 and 32 bits (the ones the screen has modes
 *    for) that is at least the wanted one, else the largest.
 *  - The desktop icon regions (DMNewDeskIconRegionRef, ...ByRef,
 *    DMChangedDesktopIconRgn) are regions kept for a program; when one
 *    changes the desktop is drawn again at DMDoDelayedScreenUpdates, with
 *    the proc DMSetDeskDrawProc gave (proc(region)) or the way DMDrawDesktopRegion does.
 *  - DMNotifyAllProcesses(message, data) calls every registered extended
 *    notification procedure.
 *  - A ColorSync profile is kept for the display by DMSetProfileByAVID.
 */
#include <string.h>

#include "carbon_events.h"
#include "ppc_bridge.h"
#include "tb_internal.h"
#include "os9left.h"

enum { kDMNotFoundErr = -6229, kDMGenErr = -6220 };
enum { REF_DESKRGN = 140 };
#define LM_CrsrNew 0x08CEu /* as events.c has it: the cursor must be redrawn at RawMouse */

extern uint32_t dmgr_display_id;
int dmgr_notify_all(int16_t message, gaddr notifyData);
void dmgr_remove_all_notify(void);
int16_t tb_DMBeginConfigureDisplays(gaddr displayState);
void tb_DMDrawDesktopRegion(gaddr globalRgn);
int16_t tb_DMSendDependentNotification(uint32_t notifyType, uint32_t notifyClass, uint32_t displayID,
                                       uint32_t notifyComponent);

static bool is_screen_device(gaddr gd)
{
    for (gaddr d = rd32(LM_DeviceList); d; d = rd32(d) ? rd32(rd32(d) + GDevice_gdNextGD_off) : 0)
        if (d == gd)
            return true;
    return false;
}

/* The device a call named, nil for the main one. */
static gaddr device_or_main(gaddr gd) { return gd ? gd : rd32(LM_MainDevice); }

/* ---- Identity and flags ---- */

/* DMSetDisplayID(device, displayID) */
static void dm_DMSetDisplayID(ppc_cpu *c)
{
    if (!is_screen_device(R(0)))
        RETURN((int16_t)kDMNotFoundErr);
    if (!R(1))
        RETURN(paramErr);
    dmgr_display_id = R(1);
    RETURN(noErr);
}

#define MAX_FLAGS 8
static struct { gaddr gd; uint32_t flags; } aux_flags[MAX_FLAGS];

static void dm_DMSetAuxGDeviceFlag(ppc_cpu *c)
{
    gaddr gd = device_or_main(R(0));
    if (!is_screen_device(gd))
        RETURN((int16_t)kDMNotFoundErr);
    if (R(1) > 31)
        RETURN(paramErr);
    int slot = -1;
    for (int i = 0; i < MAX_FLAGS; i++)
        if (aux_flags[i].gd == gd)
            slot = i;
        else if (slot < 0 && !aux_flags[i].gd && i >= 0)
            slot = -2 - i; /* a free one, taken if the device has none */
    if (slot <= -2)
        slot = -2 - slot;
    if (slot < 0)
        RETURN(memFullErr);
    aux_flags[slot].gd = gd;
    if (R(2) & 0xFF)
        aux_flags[slot].flags |= 1u << R(1);
    else
        aux_flags[slot].flags &= ~(1u << R(1));
    RETURN(noErr);
}

static void dm_DMTestAuxGDeviceFlag(ppc_cpu *c)
{
    gaddr gd = device_or_main(R(0));
    for (int i = 0; i < MAX_FLAGS; i++)
        if (aux_flags[i].gd == gd && R(1) < 32)
            RETURN(aux_flags[i].flags >> R(1) & 1);
    RETURN(0);
}

/* ---- Modes ---- */

static int depth_mode_for(int depth)
{
    return 0x80 + (depth == 1 ? 0 : depth == 2 ? 1 : depth == 4 ? 2 : depth == 8 ? 3 : depth == 16 ? 4 : 5);
}

static void dm_DMGetBestDepthModeInfo(ppc_cpu *c)
{
    if (!is_screen_device(device_or_main(R(0))))
        RETURN((int16_t)kDMNotFoundErr);
    static const int depths[] = { 8, 16, 32 };
    int best = depths[2];
    for (int i = 2; i >= 0; i--)
        if (depths[i] >= (int)R(1))
            best = depths[i];
    if (R(2))
        wr32(R(2), (uint32_t)depth_mode_for(best));
    if (R(3))
        wr16(R(3), (uint16_t)best);
    RETURN(noErr);
}

/* DMGetROMMultiModeTable(): the modes the machine's ROM would list for its
 * own screen: a long count, then per mode the width and height words and
 * a long of the depth modes (bits 3 to 5: 8, 16 and 32 bits). */
static gaddr multimode;
static void dm_DMGetROMMultiModeTable(ppc_cpu *c)
{
    static const int16_t sizes[][2] = { { 640, 480 }, { 800, 600 }, { 832, 624 }, { 1024, 768 }, { 1152, 870 },
                                         { 1280, 1024 } };
    if (!multimode) {
        multimode = tb_NewPtr(4 + 8 * 6, true, true);
        if (!multimode)
            RETURN(0);
        uint32_t n = 0;
        for (unsigned i = 0; i < 6; i++)
            if (sizes[i][0] <= g_screen_width && sizes[i][1] <= g_screen_height) {
                gaddr e = multimode + 4 + 8 * n++;
                wr16(e, (uint16_t)sizes[i][0]);
                wr16(e + 2, (uint16_t)sizes[i][1]);
                wr32(e + 4, 0x38);
            }
        wr32(multimode, n);
    }
    RETURN(multimode);
}

/* ---- Starting up ---- */

enum { INIT_MANAGER = 1, INIT_INSTALLED = 2, INIT_DISPLAYS = 4, INIT_FINAL = 8, INIT_TOOLBOX = 16 };
static uint32_t init_flags;

static void dm_DMInitDisplayManager(ppc_cpu *c) { init_flags |= INIT_MANAGER; RETURN(noErr); }
static void dm_DMInstallDisplayManager(ppc_cpu *c) { init_flags |= INIT_INSTALLED; RETURN(noErr); }

/* DMInstallDisplays(): the screen's device is the one display. */
static void dm_DMInstallDisplays(ppc_cpu *c)
{
    init_flags |= INIT_DISPLAYS;
    RETURN(rd32(LM_MainDevice) ? noErr : kDMGenErr);
}
static void dm_DMFinalInit(ppc_cpu *c) { init_flags |= INIT_FINAL; RETURN(noErr); }
static void dm_DMDoToolboxPatch(ppc_cpu *c) { init_flags |= INIT_TOOLBOX; RETURN(noErr); }

static void dm_DMGetInitializationFlags(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    wr32(R(0), init_flags);
    RETURN(noErr);
}

/* DMPrivateBeginConfigureDisplays(*displayState) */
static void dm_DMPrivateBeginConfigureDisplays(ppc_cpu *c) { RETURN(tb_DMBeginConfigureDisplays(R(0))); }

/* DMConfigureHardware(displayState): the hardware is configured as it is. */
static void dm_DMConfigureHardware(ppc_cpu *c) { RETURN(rd32(LM_MainDevice) ? noErr : kDMGenErr); }

/* ---- The desktop ---- */

static gaddr desk_proc;
static bool desk_dirty;

static void dm_DMNewDeskIconRegionRef(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    gaddr cell = tb_NewPtr(8, true, true), rgn = tb_NewRgn();
    if (!cell || !rgn)
        RETURN(memFullErr);
    wr32(cell + 4, rgn);
    wr32(cell, ref_new(REF_DESKRGN, (void *)(uintptr_t)cell));
    wr32(R(0), rd32(cell));
    RETURN(noErr);
}

static gaddr cell_of(gaddr ref)
{
    void *p = ref_get(ref, REF_DESKRGN);
    return (gaddr)(uintptr_t)p;
}

static void dm_DMDisposeDeskIconRegionRef(ppc_cpu *c)
{
    gaddr cell = cell_of(R(0));
    if (!cell)
        RETURN(kDMNotFoundErr);
    tb_DisposeRgn(rd32(cell + 4));
    ref_free(R(0));
    tb_DisposePtr(cell);
    RETURN(noErr);
}

/* DMGetDeskIconRegionByRef(ref, *region): a copy to dispose of. */
static void dm_DMGetDeskIconRegionByRef(ppc_cpu *c)
{
    gaddr cell = cell_of(R(0));
    if (!cell)
        RETURN(kDMNotFoundErr);
    if (!R(1))
        RETURN(paramErr);
    gaddr copy = tb_NewRgn();
    if (!copy)
        RETURN(memFullErr);
    tb_CopyRgn(rd32(cell + 4), copy);
    wr32(R(1), copy);
    RETURN(noErr);
}

/* DMChangedDesktopIconRgn(ref, region): the region is now this; the
 * desktop is drawn again at the next DMDoDelayedScreenUpdates. */
static void dm_DMChangedDesktopIconRgn(ppc_cpu *c)
{
    gaddr cell = cell_of(R(0));
    if (!cell)
        RETURN(kDMNotFoundErr);
    if (R(1))
        tb_CopyRgn(R(1), rd32(cell + 4));
    desk_dirty = true;
    RETURN(noErr);
}

static void dm_DMSetDeskDrawProc(ppc_cpu *c)
{
    desk_proc = R(0);
    RETURN(noErr);
}

static void dm_DMDoDelayedScreenUpdates(ppc_cpu *c)
{
    if (!desk_dirty)
        RETURN(noErr);
    desk_dirty = false;
    gaddr gray = rd32(LM_GrayRgn);
    if (desk_proc) {
        uint32_t args[1] = { gray };
        if (mm_is_descriptor(desk_proc))
            mm_call(desk_proc, 0xC0, args, 1);
        else
            ppc_call(desk_proc, args, 1);
    } else if (gray) {
        tb_DMDrawDesktopRegion(gray);
    }
    RETURN(noErr);
}

/* DMDeskClick(globalPoint, modifiers): was the click on the desktop, in
 * none of the windows? */
static void dm_DMDeskClick(ppc_cpu *c)
{
    uint32_t pt = R(0);
    gaddr gray = rd32(LM_GrayRgn);
    if (!gray || !tb_PtInRgn(pt, gray))
        RETURN(0);
    for (gaddr w = rd32(LM_WindowList); w; w = rd32(w + WindowRecord_nextWindow_off))
        if (rd8(w + WindowRecord_visible_off) && rd32(w + WindowRecord_strucRgn_off) &&
            tb_PtInRgn(pt, rd32(w + WindowRecord_strucRgn_off)))
            RETURN(0);
    RETURN(1);
}

/* DMKeepWindowOnscreen(window): moved by the least that puts its
 * structure inside the desktop's bounds (if it fits), or so the
 * title bar is. */
static void dm_DMKeepWindowOnscreen(ppc_cpu *c)
{
    gaddr w = R(0), gray = rd32(LM_GrayRgn);
    if (!w || !gray || !rd32(w + WindowRecord_strucRgn_off))
        RETURN(paramErr);
    gaddr s = rd32(rd32(w + WindowRecord_strucRgn_off)) + 2, d = rd32(gray) + 2;
    int top = (int16_t)rd16(s), left = (int16_t)rd16(s + 2), bottom = (int16_t)rd16(s + 4), right = (int16_t)rd16(s + 6);
    int dt = (int16_t)rd16(d), dl = (int16_t)rd16(d + 2), db = (int16_t)rd16(d + 4), dr = (int16_t)rd16(d + 6);
    int dv = 0, dh = 0;
    if (bottom > db)
        dv = db - bottom;
    if (top + dv < dt)
        dv = dt - top;
    if (right > dr)
        dh = dr - right;
    if (left + dh < dl)
        dh = dl - left;
    if (dv || dh) {
        gaddr cont = rd32(rd32(w + WindowRecord_contRgn_off)) + 2;
        tb_MoveWindow(w, (int16_t)((int16_t)rd16(cont + 2) + dh), (int16_t)((int16_t)rd16(cont) + dv), 0);
    }
    RETURN(noErr);
}

/* DMMoveCursor(h, v): the cursor is where a program that sets the mouse
 * puts it. */
static void dm_DMMoveCursor(ppc_cpu *c)
{
    uint32_t pt = (uint32_t)(uint16_t)(int16_t)R(1) << 16 | (uint16_t)(int16_t)R(0);
    wr32(LM_MTemp, pt);
    wr32(LM_MouseLocation, pt);
    wr8(LM_CrsrNew, 1);
    RETURN(noErr);
}

/* ---- Notification ---- */

static void dm_DMNotifyAllProcesses(ppc_cpu *c)
{
    dmgr_notify_all((int16_t)R(0), R(1));
    RETURN(noErr);
}

static void dm_DMRemoveCurrentProcessProcs(ppc_cpu *c)
{
    dmgr_remove_all_notify();
    RETURN(noErr);
}

/* ---- Override files and base components ---- */

static uint8_t override_spec[70]; /* the FSSpec of the overrides file */
static bool have_override;

/* DMRegisterOverrideFile(fsspec): the file of display overrides must be there. */
static void dm_DMRegisterOverrideFile(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    gaddr info = tb_NewPtr(16, false, true);
    if (!info)
        RETURN(memFullErr);
    int16_t e = tb_FSpGetFInfo(R(0), info);
    tb_DisposePtr(info);
    if (e)
        RETURN(e);
    gget(override_spec, R(0), sizeof override_spec);
    have_override = true;
    RETURN(noErr);
}

static uint32_t next_comp = 0x4D424300;
static uint32_t comps[8];

/* DMNewDisplayBaseComponent(displayID, *component) */
static void dm_DMNewDisplayBaseComponent(ppc_cpu *c)
{
    if (R(0) != dmgr_display_id)
        RETURN(kDMNotFoundErr);
    if (!R(1))
        RETURN(paramErr);
    for (int i = 0; i < 8; i++)
        if (!comps[i]) {
            comps[i] = ++next_comp;
            wr32(R(1), comps[i]);
            RETURN(noErr);
        }
    RETURN(memFullErr);
}

static void dm_DMDisposeDisplayBaseComponent(ppc_cpu *c)
{
    for (int i = 0; i < 8; i++)
        if (R(0) && comps[i] == R(0)) {
            comps[i] = 0;
            RETURN(noErr);
        }
    RETURN(kDMNotFoundErr);
}

/* ---- Profiles ---- */

static struct { uint32_t avid; gaddr profile; } profiles[8];

/* DMSetProfileByAVID(avid, profileHandle): kept for the display; the
 * dependents are told ('prof'). */
static void dm_DMSetProfileByAVID(ppc_cpu *c)
{
    if (R(0) != dmgr_display_id)
        RETURN(kDMNotFoundErr);
    int slot = -1;
    for (int i = 7; i >= 0; i--)
        if (profiles[i].avid == R(0) || (!profiles[i].avid && slot < 0))
            slot = i;
    if (slot < 0)
        RETURN(memFullErr);
    profiles[slot].avid = R(0);
    profiles[slot].profile = R(1);
    tb_DMSendDependentNotification(0, 0x70726F66 /* 'prof' */, R(0), 0);
    RETURN(noErr);
}

static void dm_DMGetProfileByAVID(ppc_cpu *c)
{
    if (!R(1))
        RETURN(paramErr);
    for (int i = 0; i < 8; i++)
        if (profiles[i].avid == R(0) && profiles[i].profile) {
            wr32(R(1), profiles[i].profile);
            RETURN(noErr);
        }
    wr32(R(1), 0);
    RETURN(kDMNotFoundErr);
}

/* DMUpdateProfileCache(avid): the profile the display has is the one kept,
 * and there is no cache of it to bring up to date; an AVID with none is
 * the error. */
static void dm_DMUpdateProfileCache(ppc_cpu *c)
{
    for (int i = 0; i < 8; i++)
        if (profiles[i].avid == R(0) && profiles[i].profile)
            RETURN(noErr);
    RETURN(kDMNotFoundErr);
}

void os9left_display_reset(void)
{
    memset(aux_flags, 0, sizeof aux_flags);
    multimode = 0;
    init_flags = 0;
    desk_proc = 0;
    desk_dirty = false;
    have_override = false;
    memset(comps, 0, sizeof comps);
    memset(profiles, 0, sizeof profiles);
}

const ppc_export os9left_display_exports[] = {
    { "DMChangedDesktopIconRgn", dm_DMChangedDesktopIconRgn },
    { "DMConfigureHardware", dm_DMConfigureHardware },
    { "DMDeskClick", dm_DMDeskClick },
    { "DMDisposeDeskIconRegionRef", dm_DMDisposeDeskIconRegionRef },
    { "DMDisposeDisplayBaseComponent", dm_DMDisposeDisplayBaseComponent },
    { "DMDoDelayedScreenUpdates", dm_DMDoDelayedScreenUpdates },
    { "DMDoToolboxPatch", dm_DMDoToolboxPatch },
    { "DMFinalInit", dm_DMFinalInit },
    { "DMGetBestDepthModeInfo", dm_DMGetBestDepthModeInfo },
    { "DMGetDeskIconRegionByRef", dm_DMGetDeskIconRegionByRef },
    { "DMGetInitializationFlags", dm_DMGetInitializationFlags },
    { "DMGetProfileByAVID", dm_DMGetProfileByAVID },
    { "DMGetROMMultiModeTable", dm_DMGetROMMultiModeTable },
    { "DMInitDisplayManager", dm_DMInitDisplayManager },
    { "DMInstallDisplayManager", dm_DMInstallDisplayManager },
    { "DMInstallDisplays", dm_DMInstallDisplays },
    { "DMKeepWindowOnscreen", dm_DMKeepWindowOnscreen },
    { "DMMoveCursor", dm_DMMoveCursor },
    { "DMNewDeskIconRegionRef", dm_DMNewDeskIconRegionRef },
    { "DMNewDisplayBaseComponent", dm_DMNewDisplayBaseComponent },
    { "DMNotifyAllProcesses", dm_DMNotifyAllProcesses },
    { "DMPrivateBeginConfigureDisplays", dm_DMPrivateBeginConfigureDisplays },
    { "DMRegisterOverrideFile", dm_DMRegisterOverrideFile },
    { "DMRemoveCurrentProcessProcs", dm_DMRemoveCurrentProcessProcs },
    { "DMSetAuxGDeviceFlag", dm_DMSetAuxGDeviceFlag },
    { "DMSetDeskDrawProc", dm_DMSetDeskDrawProc },
    { "DMSetDisplayID", dm_DMSetDisplayID },
    { "DMSetProfileByAVID", dm_DMSetProfileByAVID },
    { "DMTestAuxGDeviceFlag", dm_DMTestAuxGDeviceFlag },
    { "DMUpdateProfileCache", dm_DMUpdateProfileCache },
};
const unsigned os9left_display_export_count = sizeof os9left_display_exports / sizeof os9left_display_exports[0];
