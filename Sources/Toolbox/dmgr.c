/*
 * The Display Manager (_DisplayDispatch, $ABEB) for the one screen, which
 * keeps its mode: its device and ID, its modes listed (the sizes up to the
 * screen's, each at 8, 16 and 32 bits), the desktop drawn, and no
 * mirroring with nothing to mirror to.  Displays can't be added, moved or
 * taken away; the configuration calls answer as for a configuration that
 * doesn't change, and notification procedures are kept but never called.
 */
#include <stdio.h>
#include <string.h>

#include "qd_internal.h"
#include "tb_internal.h"

enum {
    kDMGenErr = -6220,
    kDMWrongNumberOfDisplays = -6222,
    kDMMirroringBlocked = -6223,
    kDMMirroringNotOn = -6225,
    kDMDriverNotDisplayMgrAwareErr = -6228,
    kDMNotFoundErr = -6229,
    kDMDisplayAlreadyInstalledErr = -6230,
    kDMMainDisplayCannotMoveErr = -6231,
};
enum { MAIN_DISPLAY_ID = 1, MAX_NOTIFY = 16 };

static bool mirroring_blocked;
static uint32_t display_component; /* DMSetDisplayComponent's, for the one display */
static struct {
    gaddr proc, data;
} notify[MAX_NOTIFY]; /* extended notification procedures */

/* Is gd one of the screen devices (the only one)? */
static bool is_device(gaddr gd)
{
    for (gaddr d = rd32(LM_DeviceList); d; d = rd32(d) ? rd32(rd32(d) + GDevice_gdNextGD_off) : 0)
        if (d == gd)
            return true;
    return false;
}

static bool is_main(gaddr gd) { return gd && gd == rd32(LM_MainDevice); }

gaddr tb_DMGetFirstScreenDevice(int8_t activeOnly) { return rd32(LM_DeviceList); }

gaddr tb_DMGetNextScreenDevice(gaddr theDevice, int8_t activeOnly)
{
    return theDevice && rd32(theDevice) ? rd32(rd32(theDevice) + GDevice_gdNextGD_off) : 0;
}

/* The desktop drawn where the rectangle or region meets it. */
void tb_DMDrawDesktopRegion(gaddr globalRgn)
{
    if (!globalRgn)
        return;
    gaddr r = tb_NewRgn();
    tb_SectRgn(globalRgn, rd32(LM_GrayRgn), r);
    tb_PaintOne(0, r);
    tb_DisposeRgn(r);
}

void tb_DMDrawDesktopRect(gaddr globalRect)
{
    if (!globalRect)
        return;
    gaddr r = tb_NewRgn();
    tb_RectRgn(r, globalRect);
    tb_DMDrawDesktopRegion(r);
    tb_DisposeRgn(r);
}

/* A configuration: a state handle for the calls between, which change
 * nothing. */
int16_t tb_DMBeginConfigureDisplays(gaddr displayState)
{
    gaddr h = tb_NewHandle(4, true, true);
    if (displayState)
        wr32(displayState, h);
    return h ? noErr : memFullErr;
}

int16_t tb_DMEndConfigureDisplays(gaddr displayState)
{
    if (displayState)
        tb_DisposeHandle(displayState);
    return noErr;
}

int16_t tb_DMSuspendConfigure(gaddr displayState, uint32_t reserved1) { return noErr; }
int16_t tb_DMResumeConfigure(gaddr displayState, uint32_t reserved1) { return noErr; }

/* Displays come only from the screen's driver, which is in already. */
int16_t tb_DMAddDisplay(gaddr newDevice, int16_t driver, uint32_t mode, uint32_t reserved, uint32_t displayID,
                        uint32_t displayComponent, gaddr displayState)
{
    return is_device(newDevice) ? kDMDisplayAlreadyInstalledErr : kDMDriverNotDisplayMgrAwareErr;
}

int16_t tb_DMNewDisplay(gaddr newDevice, int16_t driverRefNum, uint32_t mode, uint32_t reserved, uint32_t displayID,
                        uint32_t displayComponent, gaddr displayState)
{
    if (newDevice)
        wr32(newDevice, 0);
    return kDMDriverNotDisplayMgrAwareErr;
}

/* The main display stays where it is, on and in. */
static int16_t main_stays(gaddr gd)
{
    if (!is_device(gd))
        return kDMNotFoundErr;
    return is_main(gd) ? kDMMainDisplayCannotMoveErr : kDMGenErr;
}

int16_t tb_DMMoveDisplay(gaddr moveDevice, int16_t x, int16_t y, gaddr displayState)
{
    if (is_main(moveDevice) && !x && !y)
        return noErr;
    return main_stays(moveDevice);
}

int16_t tb_DMDisableDisplay(gaddr disableDevice, gaddr displayState) { return main_stays(disableDevice); }
int16_t tb_DMRemoveDisplay(gaddr removeDevice, gaddr displayState) { return main_stays(removeDevice); }
int16_t tb_DMDisposeDisplay(gaddr disposeDevice, gaddr displayState) { return main_stays(disposeDevice); }

int16_t tb_DMEnableDisplay(gaddr enableDevice, gaddr displayState)
{
    return is_device(enableDevice) ? noErr : kDMNotFoundErr;
}

int16_t tb_DMSetMainDisplay(gaddr newMainDevice, gaddr displayState)
{
    return is_main(newMainDevice) ? noErr : is_device(newMainDevice) ? kDMGenErr : kDMNotFoundErr;
}

int16_t tb_DMGetDeskRegion(gaddr desktopRegion)
{
    if (desktopRegion)
        wr32(desktopRegion, rd32(LM_GrayRgn));
    return noErr;
}

/* Notification procedures: kept; the configuration never changes, so
 * none is called. */
int16_t tb_DMRegisterNotifyProc(gaddr proc, gaddr psn) { return noErr; }
int16_t tb_DMRemoveNotifyProc(gaddr proc, gaddr psn) { return noErr; }

int16_t tb_DMRegisterExtendedNotifyProc(gaddr notifyProc, gaddr notifyUserData, uint16_t notifyOnFlags,
                                        gaddr whichPSN)
{
    if (!notifyProc)
        return paramErr;
    for (int i = 0; i < MAX_NOTIFY; i++)
        if (!notify[i].proc) {
            notify[i].proc = notifyProc;
            notify[i].data = notifyUserData;
            return noErr;
        }
    return memFullErr;
}

int16_t tb_DMRemoveExtendedNotifyProc(gaddr notifyProc, gaddr notifyUserData, gaddr whichPSN, uint16_t removeFlags)
{
    for (int i = 0; i < MAX_NOTIFY; i++)
        if (notify[i].proc == notifyProc && notify[i].data == notifyUserData) {
            notify[i].proc = notify[i].data = 0;
            return noErr;
        }
    return kDMNotFoundErr;
}

int16_t tb_DMSendDependentNotification(uint32_t notifyType, uint32_t notifyClass, uint32_t displayID,
                                       uint32_t notifyComponent)
{
    return noErr;
}

/* Mirroring: QuickDraw could, but there is no second display. */
int16_t tb_DMQDIsMirroringCapable(gaddr qdIsMirroringCapable)
{
    if (qdIsMirroringCapable)
        wr8(qdIsMirroringCapable, 1);
    return noErr;
}

int16_t tb_DMCanMirrorNow(gaddr canMirrorNow)
{
    if (canMirrorNow)
        wr8(canMirrorNow, 0);
    return noErr;
}

int16_t tb_DMIsMirroringOn(gaddr isMirroringOn)
{
    if (isMirroringOn)
        wr8(isMirroringOn, 0);
    return noErr;
}

int16_t tb_DMMirrorDevices(gaddr gD1, gaddr gD2, gaddr displayState)
{
    return mirroring_blocked ? kDMMirroringBlocked : kDMWrongNumberOfDisplays;
}

int16_t tb_DMUnmirrorDevice(gaddr gDevice, gaddr displayState) { return kDMMirroringNotOn; }

int16_t tb_DMGetNextMirroredDevice(gaddr gDevice, gaddr mirroredDevice)
{
    if (mirroredDevice)
        wr32(mirroredDevice, 0);
    return is_device(gDevice) ? noErr : kDMNotFoundErr;
}

int16_t tb_DMBlockMirroring(void)
{
    mirroring_blocked = true;
    return noErr;
}

int16_t tb_DMUnblockMirroring(void)
{
    mirroring_blocked = false;
    return noErr;
}

/* The Display Manager runs in the application's A5 world. */
int16_t tb_DMGetDisplayMgrA5World(gaddr dmA5)
{
    if (dmA5)
        wr32(dmA5, rd32(LM_CurrentA5));
    return noErr;
}

/* The display's ID and device: one each. */
int16_t tb_DMGetDisplayIDByGDevice(gaddr displayDevice, gaddr displayID, int8_t failToMain)
{
    bool ok = is_device(displayDevice) || failToMain;
    if (displayID)
        wr32(displayID, ok ? MAIN_DISPLAY_ID : 0);
    return ok ? noErr : kDMNotFoundErr;
}

int16_t tb_DMGetGDeviceByDisplayID(uint32_t displayID, gaddr displayDevice, int8_t failToMain)
{
    bool ok = displayID == MAIN_DISPLAY_ID || failToMain;
    if (displayDevice)
        wr32(displayDevice, ok ? rd32(LM_MainDevice) : 0);
    return ok ? noErr : kDMNotFoundErr;
}

int16_t tb_DMSetDisplayComponent(gaddr theDevice, uint32_t displayComponent)
{
    if (!is_device(theDevice))
        return kDMNotFoundErr;
    display_component = displayComponent;
    return noErr;
}

int16_t tb_DMGetDisplayComponent(gaddr theDevice, gaddr displayComponent)
{
    if (displayComponent)
        wr32(displayComponent, is_device(theDevice) ? display_component : 0);
    return is_device(theDevice) ? noErr : kDMNotFoundErr;
}

int16_t tb_DMResolveDisplayComponents(void) { return noErr; }

/* No AV component is ever made, so none is ours to dispose of. */
int16_t tb_DMDisposeAVComponent(uint32_t theAVComponent) { return kDMNotFoundErr; }

/* ---- Modes ---- */

static int current_depth(gaddr gd)
{
    gaddr pm = gd && rd32(gd) ? rd32(rd32(gd) + GDevice_gdPMap_off) : 0;
    return pm && rd32(pm) ? (int16_t)rd16(rd32(pm) + PixMap_pixelSize_off) : 8;
}

static int depth_mode(int depth)
{
    return 0x80 + (depth == 1 ? 0 : depth == 2 ? 1 : depth == 4 ? 2 : depth == 8 ? 3 : depth == 16 ? 4 : 5);
}

/* VDSwitchInfoRec: csMode (word), csData (long: the display mode), csPage,
 * csBaseAddr. */
int16_t tb_DMGetDisplayMode(gaddr theDevice, gaddr switchInfo)
{
    gaddr gd = theDevice ? theDevice : rd32(LM_MainDevice);
    gaddr pm = gd && rd32(gd) ? rd32(rd32(gd) + GDevice_gdPMap_off) : 0;
    if (switchInfo) {
        wr16(switchInfo, (uint16_t)depth_mode(current_depth(gd)));
        wr32(switchInfo + 2, 0x80);
        wr16(switchInfo + 6, 0);
        wr32(switchInfo + 8, pm && rd32(pm) ? rd32(rd32(pm) + PixMap_baseAddr_off) : 0);
    }
    return noErr;
}

/* The screen keeps its mode: the one it is in is all that can be set. */
int16_t tb_DMSetDisplayMode(gaddr theDevice, uint32_t mode, gaddr depthMode, uint32_t reserved, gaddr displayState)
{
    gaddr gd = theDevice ? theDevice : rd32(LM_MainDevice);
    int want = depthMode ? (int)rd32(depthMode) : 0;
    if (mode == 0x80 && (!want || want == depth_mode(current_depth(gd))))
        return noErr;
    return paramErr;
}

int16_t tb_DMCheckDisplayMode(gaddr theDevice, uint32_t mode, uint32_t depthMode, gaddr switchFlags,
                              uint32_t reserved, gaddr modeOk)
{
    if (switchFlags)
        wr32(switchFlags, 0);
    if (modeOk)
        wr8(modeOk, mode == 0x80);
    return noErr;
}

/* Display mode lists: the sizes up to the screen's (a smaller display is
 * centred, as DrawSprocket does it), each at 8, 16 and 32 bits.  A list is
 * a guest block holding its count. */
static const struct { int16_t w, h; } mode_sizes[] = { { 640, 480 }, { 800, 600 }, { 832, 624 }, { 1024, 768 },
                                                       { 1152, 870 }, { 1280, 1024 } };

static int mode_count(int *sizes)
{
    int n = 0;
    for (unsigned i = 0; i < sizeof mode_sizes / sizeof mode_sizes[0]; i++)
        if (mode_sizes[i].w <= g_screen_width && mode_sizes[i].h <= g_screen_height)
            sizes[n++] = (int)i;
    return n;
}

int16_t tb_DMNewDisplayModeList(uint32_t displayID, uint32_t modeListFlags, uint32_t reserved, gaddr thePanelCount,
                                gaddr thePanelList)
{
    int sizes[8];
    int n = mode_count(sizes);
    gaddr list = tb_NewPtr(4, true, true);
    if (!list)
        return memFullErr;
    wr32(list, (uint32_t)n);
    if (thePanelCount)
        wr32(thePanelCount, (uint32_t)n);
    if (thePanelList)
        wr32(thePanelList, list);
    return noErr;
}

/* The iterator gets a DMDisplayModeListEntryRec describing the mode. */
int16_t tb_DMGetIndexedDisplayModeFromList(gaddr panelList, uint32_t itemIndex, uint32_t reserved, gaddr listIterator,
                                           gaddr userData)
{
    int sizes[8];
    int n = mode_count(sizes);
    if (itemIndex >= (uint32_t)n)
        return kDMNotFoundErr;
    int w = mode_sizes[sizes[itemIndex]].w, h = mode_sizes[sizes[itemIndex]].h;
    static const int depths[3] = { 8, 16, 32 };
    /* entry 28, name 256, timing 80 (20 used), depth block 20, 3 depth infos 20 each,
     * 3 switch infos 16 each, 3 VPBlocks 42 each, resolution 32 */
    gaddr m = tb_NewPtr(28 + 256 + 80 + 20 + 3 * (20 + 16 + 42) + 32, true, true);
    if (!m)
        return memFullErr;
    gaddr entry = m, name = m + 28, timing = name + 256, block = timing + 80, infos = block + 20;
    gaddr switches = infos + 3 * 20, vps = switches + 3 * 16;
    char text[32];
    int len = snprintf(text, sizeof text, "%d x %d", w, h);
    wr8(name, (uint8_t)len);
    gput(name + 1, text, (uint32_t)len);
    /* VDTimingInfoRec: the mode, the format ('decl'), and csTimingFlags
     * kModeValid and kModeSafe (StarCraft's Storm keeps only valid modes). */
    wr32(timing, 0x80 + itemIndex);
    wr32(timing + 8, 0x6465636C);
    wr32(timing + 16, 0x3);
    wr32(block, 3);         /* depthBlockCount */
    wr32(block + 4, infos); /* depthVPBlock */
    for (int i = 0; i < 3; i++) {
        gaddr info = infos + 20 * (gaddr)i, sw = switches + 16 * (gaddr)i, vp = vps + 42 * (gaddr)i;
        int d = depths[i];
        wr32(info, sw);
        wr32(info + 4, vp);
        wr16(sw, (uint16_t)(0x80 + i + 3)); /* csMode */
        wr32(sw + 2, 0x80 + itemIndex);      /* csData: the display mode */
        wr16(vp + 4, (uint16_t)(w * d / 8));
        wr16(vp + 10, (uint16_t)h);
        wr16(vp + 12, (uint16_t)w);
        wr32(vp + 22, 72u << 16);
        wr32(vp + 26, 72u << 16);
        wr16(vp + 30, d > 8 ? 16 : 0); /* RGBDirect or indexed */
        wr16(vp + 32, (uint16_t)d);
        wr16(vp + 34, d > 8 ? 3 : 1);
        wr16(vp + 36, (uint16_t)(d == 16 ? 5 : d == 32 ? 8 : d));
    }
    /* VDResolutionInfoRec: previous mode, mode, width, height, refresh
     * rate, deepest depth mode, flags. */
    gaddr res = vps + 3 * 42;
    wr32(res + 4, 0x80 + itemIndex);
    wr32(res + 8, (uint32_t)w);
    wr32(res + 12, (uint32_t)h);
    wr32(res + 16, 60u << 16);
    wr16(res + 20, 0x85);
    /* DMDisplayModeListEntryRec: flags, name, resolution, timing, depths, version */
    wr32(entry, 0);
    wr32(entry + 4, name);
    wr32(entry + 8, res);
    wr32(entry + 12, timing);
    wr32(entry + 16, block);
    /* pascal void (void *userData, DMListIndexType index, DMDisplayModeListEntryPtr) */
    uint32_t args[3] = { userData, itemIndex, entry };
    if (listIterator)
        mm_call(listIterator, 0x00000FC0, args, 3);
    tb_DisposePtr(m);
    return noErr;
}

int16_t tb_DMDisposeList(gaddr panelList)
{
    if (panelList)
        tb_DisposePtr(panelList);
    return noErr;
}

/* A fresh machine. */
void dmgr_reset(void)
{
    mirroring_blocked = false;
    display_component = 0;
    memset(notify, 0, sizeof notify);
}
