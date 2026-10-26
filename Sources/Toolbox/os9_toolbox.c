/*
 * The Mac OS 8.5 to 9 Toolbox libraries' own calls.  Programs built for
 * 8.5 and later import the Window, Control and Menu Managers and the
 * Appearance Manager from WindowsLib, ControlsLib, MenusLib and
 * AppearanceLib, fragments of their own beside InterfaceLib.  Most of
 * their exports are the public calls under the same names (served from
 * the other tables); these are the rest, the libraries' private calls,
 * which Universal Interfaces never declared: the Control Manager's entry
 * points behind its traps, properties kept for the System, the Window
 * Manager's floating windows, desktop and saved screen bits, the menu bar
 * clock and cached menus, and the Appearance control panel's settings.
 *
 * Their arguments are as Mac OS 9's code takes them.  Where that code
 * doesn't show what a call does (the desktop picture lists, window
 * notifications), the call answers unimpErr rather than pretend.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "carbon_hi.h"
#include "lowmem_gen.h"
#include "m68k.h"
#include "machine.h"
#include "os9_toolbox.h"
#include "ppc.h"
#include "ppc_bridge.h"
#include "qd_internal.h"
#include "tb_internal.h"
#include "trap.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; } while (0)
#define DONE() (c->pc = c->lr)

enum {
    unimpErr = -4,

    errInvalidWindowRef = -5600,
    menuNotFoundErr = -5620,     /* (DisableMenuBarClock, the clock being off) */
    menuUsesSystemDefErr = -5621, /* (EnableMenuBarClock, the clock being on) */
    themeBadCursorIndexErr = -30565,
    kNoSoundTrackErr = -30569,   /* GetThemePlaylist with no sound track chosen */
    kFontNotInstalledErr = -30574, /* Set...Font naming a font that isn't installed */
};

/* Another export's body, run on these registers as they stand. */
static void forward(ppc_cpu *c, const char *name)
{
    void (*fn)(ppc_cpu *) = cfm_export_fn(name);
    if (fn)
        fn(c);
    else
        RETURN(unimpErr);
}

static void pstr_put(gaddr p, const char *s)
{
    size_t n = strlen(s);
    if (n > 255)
        n = 255;
    wr8(p, (uint8_t)n);
    gput(p + 1, s, (uint32_t)n);
}

static void pstr_get(gaddr p, char *out, size_t size)
{
    size_t n = p ? rd8(p) : 0;
    if (n >= size)
        n = size - 1;
    if (n)
        gget(out, p + 1, (uint32_t)n);
    out[n] = 0;
}

/* ---------------------------------------------------------------------- */
/* System properties.  The System keeps properties of its own on windows,
 * controls and menu items, under its creator 'macs' (as ControlsLib and
 * MenusLib do); a program's properties are under its own creator, so the
 * two never meet.  Each call is the public one with the creator put in
 * as argument pos, the rest moved up. */

enum { kSystemCreator = 0x6D616373 }; /* 'macs' */

static void with_creator(ppc_cpu *c, const char *name, int pos, int nargs)
{
    for (int i = nargs - 1; i >= pos; i--)
        R(i + 1) = R(i);
    R(pos) = kSystemCreator;
    forward(c, name);
}

#define SYSTEM_PROPERTY(fn, target, pos, nargs) \
    static void fn(ppc_cpu *c) { with_creator(c, target, pos, nargs); }

SYSTEM_PROPERTY(GetWindowSystemProperty, "GetWindowProperty", 1, 5)
SYSTEM_PROPERTY(SetWindowSystemProperty, "SetWindowProperty", 1, 4)
SYSTEM_PROPERTY(GetWindowSystemPropertySize, "GetWindowPropertySize", 1, 3)
SYSTEM_PROPERTY(RemoveWindowSystemProperty, "RemoveWindowProperty", 1, 2)
SYSTEM_PROPERTY(GetWindowSystemPropertyAttributes, "GetWindowPropertyAttributes", 1, 3)
SYSTEM_PROPERTY(ChangeWindowSystemPropertyAttributes, "ChangeWindowPropertyAttributes", 1, 4)
SYSTEM_PROPERTY(GetControlSystemProperty, "GetControlProperty", 1, 5)
SYSTEM_PROPERTY(SetControlSystemProperty, "SetControlProperty", 1, 4)
SYSTEM_PROPERTY(GetControlSystemPropertySize, "GetControlPropertySize", 1, 3)
SYSTEM_PROPERTY(RemoveControlSystemProperty, "RemoveControlProperty", 1, 2)
SYSTEM_PROPERTY(GetControlSystemPropertyAttributes, "GetControlPropertyAttributes", 1, 3)
SYSTEM_PROPERTY(ChangeControlSystemPropertyAttributes, "ChangeControlPropertyAttributes", 1, 4)
SYSTEM_PROPERTY(GetMenuItemSystemProperty, "GetMenuItemProperty", 2, 6)
SYSTEM_PROPERTY(SetMenuItemSystemProperty, "SetMenuItemProperty", 2, 5)
SYSTEM_PROPERTY(GetMenuItemSystemPropertySize, "GetMenuItemPropertySize", 2, 4)
SYSTEM_PROPERTY(RemoveMenuItemSystemProperty, "RemoveMenuItemProperty", 2, 3)
SYSTEM_PROPERTY(GetMenuItemSystemPropertyAttributes, "GetMenuItemPropertyAttributes", 2, 4)
SYSTEM_PROPERTY(ChangeMenuItemSystemPropertyAttributes, "ChangeMenuItemPropertyAttributes", 2, 5)

/* A window's 32-bit system property, or dflt. */
static uint32_t window_value(gaddr w, uint32_t tag, uint32_t dflt)
{
    gaddr v = tb_NewPtr(4, false, true);
    uint32_t r = v && tb_GetWindowProperty(w, kSystemCreator, tag, 4, 0, v) == noErr ? rd32(v) : dflt;
    tb_DisposePtr(v);
    return r;
}

static int16_t set_window_value(gaddr w, uint32_t tag, uint32_t value)
{
    gaddr v = tb_NewPtr(4, false, true);
    if (!v)
        return memFullErr;
    wr32(v, value);
    int16_t err = tb_SetWindowProperty(w, kSystemCreator, tag, 4, v);
    tb_DisposePtr(v);
    return err;
}

/* A menu item's system property kept with its CarbonLib fields: a byte. */
static hi_prop *item_prop(gaddr mh, int item, uint32_t tag, bool create)
{
    hi_item *it = menu_hi_item(mh, item, create);
    if (!it)
        return NULL;
    for (hi_prop *p = it->props; p; p = p->next)
        if (p->creator == kSystemCreator && p->tag == tag)
            return p;
    if (!create)
        return NULL;
    hi_prop *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->data = calloc(1, 1);
    if (!p->data) {
        free(p);
        return NULL;
    }
    p->creator = kSystemCreator;
    p->tag = tag;
    p->size = 1;
    p->next = it->props;
    it->props = p;
    return p;
}

static bool item_flag(gaddr mh, int item, uint32_t tag)
{
    hi_prop *p = item_prop(mh, item, tag, false);
    return p && p->size && p->data[0];
}

static int16_t set_item_flag(gaddr mh, int item, uint32_t tag, bool on)
{
    if (!mh || !rd32(mh) || item < 1 || item > tb_CountMItems(mh))
        return paramErr;
    hi_prop *p = item_prop(mh, item, tag, true);
    if (!p)
        return memFullErr;
    p->data[0] = on;
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* The Control Manager's entry points: ControlsLib exports the body of
 * each call its trap or InterfaceLib glue reaches as "__" and the call's
 * name, with the same arguments. */

static const char *const control_entry_points[] = {
    "__ActivateControl", "__AdvanceKeyboardFocus", "__AutoEmbedControl", "__ClearKeyboardFocus",
    "__CountSubControls", "__CreateRootControl", "__DeactivateControl", "__DisposeControl", "__DragControl",
    "__Draw1Control", "__DrawControlInCurrentPort", "__DrawControls", "__DumpControlHierarchy", "__EmbedControl",
    "__FindControl", "__FindControlUnderMouse", "__GetAuxiliaryControlRecord", "__GetBestControlRect",
    "__GetControlAction", "__GetControlData", "__GetControlDataSize", "__GetControlFeatures",
    "__GetControlMaximum", "__GetControlMinimum", "__GetControlReference", "__GetControlTitle",
    "__GetControlValue", "__GetControlVariant", "__GetIndexedSubControl", "__GetKeyboardFocus", "__GetNewControl",
    "__GetRootControl", "__GetSuperControl", "__HandleControlClick", "__HandleControlKey", "__HideControl",
    "__HiliteControl", "__IdleControls", "__IsControlActive", "__IsControlVisible", "__KillControls",
    "__MoveControl", "__NewControl", "__ReverseKeyboardFocus", "__SendControlMessage", "__SetControlAction",
    "__SetControlData", "__SetControlFontStyle", "__SetControlMaximum", "__SetControlMinimum",
    "__SetControlReference", "__SetControlSupervisor", "__SetControlTitle", "__SetControlValue",
    "__SetControlVisibility", "__SetKeyboardFocus", "__SetUpControlBackground", "__ShowControl", "__SizeControl",
    "__TestControl", "__TrackControl", "__UpdateControls",
};

/* __ScrollDelay(startTicks, nowTicks): the pause between a scroll
 * arrow's repeats, shorter the longer the arrow has been held (48 ticks
 * over the time held plus 4), cut short when the button comes up. */
static void ScrollDelay(ppc_cpu *c)
{
    uint32_t held = R(1) - R(0);
    uint32_t until = R(1) + 48 / (held + 4);
    uint32_t ticks;
    while ((int32_t)(tb_TickCount() - until) < 0 && tb_StillDown() && !g_cpu->halted)
        tb_Delay(1, &ticks);
    RETURN(noErr);
}

/* ---------------------------------------------------------------------- */
/* The Window Manager */

/* The "WM" forms of Appearance 1.0's six, which its trap selectors reach. */
static void WMCollapseWindow(ppc_cpu *c) { forward(c, "CollapseWindow"); }
static void WMCollapseAllWindows(ppc_cpu *c) { forward(c, "CollapseAllWindows"); }
static void WMGetWindowFeatures(ppc_cpu *c) { forward(c, "GetWindowFeatures"); }
static void WMGetWindowRegion(ppc_cpu *c) { forward(c, "GetWindowRegion"); }
static void WMIsWindowCollapsable(ppc_cpu *c) { forward(c, "IsWindowCollapsable"); }
static void WMIsWindowCollapsed(ppc_cpu *c) { forward(c, "IsWindowCollapsed"); }

/* Floating windows highlighted while the program is in front: the System
 * deactivated them as it went to the back. */
static void ActivateFloatingWindows(ppc_cpu *c)
{
    wm_set_floats_active(true);
    RETURN(noErr);
}
static void DeactivateFloatingWindows(ppc_cpu *c)
{
    wm_set_floats_active(false);
    RETURN(noErr);
}
static void AreFloatingWindowsActive(ppc_cpu *c) { RETURN(wm_floats_active()); }

/* One program's windows: there are no others to bring forward or hide
 * (as Mac OS 9's own do nothing), and no buffering. */
static void BringProcessWindowsToFront(ppc_cpu *c) { DONE(); }
static void HideProcessWindows(ppc_cpu *c) { DONE(); }
static void ShowProcessWindows(ppc_cpu *c) { DONE(); }
static void EnableWindowBuffering(ppc_cpu *c) { DONE(); }
static void DisableWindowBuffering(ppc_cpu *c) { DONE(); }
static void IsWindowBufferingEnabled(ppc_cpu *c) { RETURN(0); }

/* Unicode titles: none, as in Mac OS 9 (CarbonLib's CFString titles are
 * its own calls). */
static void WindowHasUnicodeTitle(ppc_cpu *c) { RETURN(0); }
static void GetWindowUnicodeTitle(ppc_cpu *c) { RETURN(paramErr); }
static void SetWindowUnicodeTitle(ppc_cpu *c) { RETURN(paramErr); }

/* GetWindowProcess(window, &psn): the program's own. */
static void GetWindowProcess(ppc_cpu *c)
{
    if (!wm_is_window(R(0))) {
        RETURN(errInvalidWindowRef);
        return;
    }
    if (!R(1)) {
        RETURN(paramErr);
        return;
    }
    RETURN(tb_GetCurrentProcess(R(1)));
}

/* GetWindowTitleLength(window, &length): its title's characters. */
static void GetWindowTitleLength(ppc_cpu *c)
{
    gaddr w = R(0);
    if (!wm_is_window(w)) {
        RETURN(errInvalidWindowRef);
        return;
    }
    if (!R(1)) {
        RETURN(paramErr);
        return;
    }
    gaddr t = WindowRecord_titleHandle(w);
    wr32(R(1), t && rd32(t) ? rd8(rd32(t)) : 0);
    RETURN(noErr);
}

/* GetWindowVisualClass(window, &class): the class it looks like, which
 * is the class it is. */
static void GetWindowVisualClass(ppc_cpu *c)
{
    if (!wm_is_window(R(0))) {
        RETURN(errInvalidWindowRef);
        return;
    }
    RETURN(tb_GetWindowClass(R(0), R(1)));
}

/* SetWindowFeatures(window, features): what its definition can do, as
 * GetWindowFeatures will answer. */
enum { kFeaturesTag = 0x66656174, kDropThresholdTag = 0x67647468 }; /* 'feat' 'gdth' */

static void SetWindowFeatures(ppc_cpu *c)
{
    if (!wm_is_window(R(0))) {
        RETURN(errInvalidWindowRef);
        return;
    }
    RETURN(set_window_value(R(0), kFeaturesTag, R(1)));
}

/* The distance a title bar gadget's click may stray before it is a drag. */
static void SetWindowGadgetDropThreshold(ppc_cpu *c)
{
    if (!wm_is_window(R(0))) {
        RETURN(errInvalidWindowRef);
        return;
    }
    RETURN(set_window_value(R(0), kDropThresholdTag, R(1)));
}

static void GetWindowGadgetDropThreshold(ppc_cpu *c)
{
    if (!wm_is_window(R(0))) {
        RETURN(errInvalidWindowRef);
        return;
    }
    if (!R(1)) {
        RETURN(paramErr);
        return;
    }
    wr32(R(1), window_value(R(0), kDropThresholdTag, 0));
    RETURN(noErr);
}

/* What double-clicking a title bar does (0 nothing, 1 collapse; up to 3
 * accepted), and doing it. */
static void SetWindowDoubleClickAction(ppc_cpu *c)
{
    if (R(0) > 3) {
        RETURN(paramErr);
        return;
    }
    ap_settings()->dbl_click = R(0);
    RETURN(noErr);
}

static void GetWindowDoubleClickAction(ppc_cpu *c) { RETURN(ap_settings()->dbl_click); }

/* DoubleClickWindowTitle(window, ...): whether the double-click did
 * something. */
static void DoubleClickWindowTitle(ppc_cpu *c)
{
    gaddr w = R(0);
    if (ap_settings()->dbl_click != 1 || !wm_is_window(w) || !tb_IsWindowCollapsable(w)) {
        RETURN(0);
        return;
    }
    tb_CollapseWindow(w, !tb_IsWindowCollapsed(w));
    RETURN(1);
}

/* GlobalFindWindow(where, &window): FindWindow over every program's
 * windows, which are this one's. */
static void GlobalFindWindow(ppc_cpu *c)
{
    gaddr out = R(1) ? R(1) : tb_NewPtr(4, false, true);
    int16_t part = tb_FindWindow(R(0), out);
    if (!R(1))
        tb_DisposePtr(out);
    RETURN((uint32_t)(int32_t)part);
}

/* ConstrainedDragRgn(rgn, startPt, &constraint, actionProc, &offset):
 * DragGrayRgn within the constraint's limit and slop rectangles along its
 * axis (the long after them); the distance moved, or none if the mouse
 * came up outside the slop. */
static void ConstrainedDragRgn(ppc_cpu *c)
{
    gaddr rgn = R(0), info = R(2), offset = R(4);
    if (offset)
        wr32(offset, 0);
    if (!rgn || !info) {
        RETURN(paramErr);
        return;
    }
    int32_t moved = tb_DragGrayRgn(rgn, R(1), info, info + 8, (int16_t)rd32(info + 16), R(3));
    if (offset && (uint32_t)moved != 0x80008000u)
        wr32(offset, (uint32_t)moved);
    RETURN(noErr);
}

/* EraseWindowRegion(window, rgn): erased to the window's background. */
static void EraseWindowRegion(ppc_cpu *c)
{
    gaddr w = R(0), rgn = R(1);
    if (!w || !rgn) {
        RETURN(paramErr);
        return;
    }
    if (!wm_is_window(w)) {
        RETURN(errInvalidWindowRef);
        return;
    }
    gaddr saved = qd_port();
    tb_SetPort(w);
    tb_EraseRgn(rgn);
    tb_SetPort(saved);
    RETURN(noErr);
}

static void WindowManagerSetThemeWindowBackground(ppc_cpu *c)
{
    RETURN(tb_SetThemeWindowBackground(R(0), (int16_t)R(1), (int8_t)R(2)));
}

/* PostWindowActivateEvent(window, active): an activate or deactivate event
 * for it, as the Window Manager queues them. */
static void PostWindowActivateEvent(ppc_cpu *c)
{
    if (wm_is_window(R(0)))
        wr32((uint8_t)R(1) ? LM_CurActivate : LM_CurDeactive, R(0));
    DONE();
}

/* The events the Window Manager queues itself go out through GetNextEvent
 * (CurActivate and CurDeactive); none wait here. */
static void GetNextWindowActivateEvent(ppc_cpu *c) { RETURN(0); }

/* Windows' shapes made again (after the screen or menu bar changed): the
 * desk's region, and everything drawn again.  Delayed or not, now. */
static void ReshapeWindows(ppc_cpu *c)
{
    wm_rebuild_desk();
    wm_repaint_all();
    DONE();
}
static void PostDelayedWindowReshape(ppc_cpu *c)
{
    wm_rebuild_desk();
    wm_repaint_all();
    RETURN(noErr);
}
static void RebuildDesktopRegion(ppc_cpu *c)
{
    wm_rebuild_desk();
    DONE();
}
/* (Titles are measured as they're drawn.) */
static void RecalculateAllWindowTitleWidths(ppc_cpu *c) { DONE(); }

/* DrawDesktop: the desk painted again, under the windows. */
static void DrawDesktop(ppc_cpu *c)
{
    gaddr gray = rd32(LM_GrayRgn);
    if (gray)
        tb_PaintOne(0, gray);
    DONE();
}

/* Window notification procedures: what they are told, and when, isn't
 * known. */
static void RegisterWindowNotificationProc(ppc_cpu *c)
{
    RETURN(wm_is_window(R(0)) ? unimpErr : errInvalidWindowRef);
}
static void UnregisterWindowNotificationProc(ppc_cpu *c)
{
    RETURN(wm_is_window(R(0)) ? unimpErr : errInvalidWindowRef);
}

/* ---- The desktop pattern ---- */

/* The desk's pattern as a pixel pattern (made from DeskPattern), and one
 * set: its black-and-white pattern becomes the desk's (the desk being
 * drawn in one). */
static gaddr desk_pixpat;

static gaddr desktop_pixpat(void)
{
    if (!desk_pixpat || !rd32(desk_pixpat)) {
        desk_pixpat = tb_NewPixPat();
        if (!desk_pixpat)
            return 0;
        wr16(rd32(desk_pixpat), 0); /* patType: an old-style pattern */
    }
    gmemcpy(rd32(desk_pixpat) + PixPat_pat1Data_off, LM_DeskPattern, 8);
    return desk_pixpat;
}

/* SetDesktopPattern(pixPat, &?): the second argument is checked and not
 * otherwise used. */
static void SetDesktopPattern(ppc_cpu *c)
{
    gaddr pp = R(0);
    if (!pp || !R(1) || !rd32(pp)) {
        RETURN(paramErr);
        return;
    }
    gmemcpy(LM_DeskPattern, rd32(pp) + PixPat_pat1Data_off, 8);
    gaddr gray = rd32(LM_GrayRgn);
    if (gray)
        tb_PaintOne(0, gray);
    RETURN(noErr);
}

/* GetDesktopPattern(&pixPat, &isPicture) and GetDesktopPixPat(&pixPat,
 * &?): the desk's pattern; there is no picture. */
static void GetDesktopPattern(ppc_cpu *c)
{
    if (R(1))
        wr8(R(1), 0);
    if (R(0))
        wr32(R(0), desktop_pixpat());
    RETURN(R(0) ? noErr : paramErr);
}
static void GetDesktopPixPat(ppc_cpu *c) { GetDesktopPattern(c); }

/* ---- Desktop pictures ---- */

/* GetPictureFromFile(&spec, &picture): the picture a file holds, by the
 * graphics importer for it (a PICT file's own, or the image drawn). */
static void GetPictureFromFile(ppc_cpu *c)
{
    gaddr spec = R(0), out = R(1);
    if (!spec || !out) {
        RETURN(paramErr);
        return;
    }
    wr32(out, 0);
    gaddr gi = tb_NewPtr(4, false, true);
    int16_t err = tb_GetGraphicsImporterForFile(spec, gi);
    if (err == noErr && rd32(gi)) {
        uint32_t args[] = { rd32(gi), out };
        err = (int16_t)carbon_call_export("GraphicsImportGetAsPicture", args, 2);
        uint32_t close[] = { rd32(gi) };
        carbon_call_export("CloseComponent", close, 1);
    }
    tb_DisposePtr(gi);
    RETURN((uint32_t)(int32_t)(err ? err : rd32(out) ? noErr : paramErr));
}

/* FileContainsPossibleDesktopPicture(&spec, &yes): whether there's an
 * importer for it. */
static void FileContainsPossibleDesktopPicture(ppc_cpu *c)
{
    gaddr spec = R(0), out = R(1);
    if (!spec || !out) {
        RETURN(paramErr);
        return;
    }
    gaddr gi = tb_NewPtr(4, false, true);
    bool yes = tb_GetGraphicsImporterForFile(spec, gi) == noErr && rd32(gi);
    if (yes) {
        uint32_t close[] = { rd32(gi) };
        carbon_call_export("CloseComponent", close, 1);
    }
    tb_DisposePtr(gi);
    wr8(out, yes);
    RETURN(noErr);
}

/* The Desktop Pictures control panel's lists of monitors' pictures: their
 * records' layout isn't known, so there are none to give out.  The desk
 * is the pattern's. */
static void desktop_list_unknown(ppc_cpu *c) { RETURN(unimpErr); }
static void InternalMonitorDesktopListChanged(ppc_cpu *c) { DONE(); }

/* ---- Saved screen bits ---- */

/* SaveScreenBits(&rect, purgeable, &bits): what the screen shows in rect,
 * in an offscreen world, so that it can be put back (as the menus' and
 * help balloons' are).  Bits are a list of records: the rectangle at 4,
 * the next record at 16 and the world at 20. */
enum { SB_RECT = 4, SB_NEXT = 16, SB_WORLD = 20, SB_SIZE = 24 };

static bool bit_saving = true;

static gaddr screen_pixmap(void)
{
    gaddr cp = rd32(LM_WMgrCPort);
    gaddr pm = cp ? rd32(cp + CGrafPort_portPixMap_off) : 0;
    return pm ? rd32(pm) : 0;
}

/* Copy between the screen's rect and a world (its port rectangle; to_screen:
 * the world's back), with the Window Manager's colour port current and
 * unclipped. */
static void copy_screen(gaddr world, gaddr rect, bool to_screen)
{
    gaddr screen = screen_pixmap(), pm = tb_GetGWorldPixMap(world);
    if (!screen || !pm)
        return;
    gaddr saved = qd_port();
    gaddr wp = rd32(LM_WMgrCPort);
    tb_SetPort(wp);
    gaddr clip = tb_NewRgn();
    tb_GetClip(clip);
    tb_SetRectRgn(GrafPort_clipRgn(wp), -32767, -32767, 32767, 32767);
    gaddr fg = tb_NewPtr(12, false, true);
    tb_GetForeColor(fg);
    tb_GetBackColor(fg + 6);
    gaddr black = tb_NewPtr(6, false, true), white = tb_NewPtr(6, false, false);
    gmemset(white, 0xFF, 6);
    tb_RGBForeColor(black);
    tb_RGBBackColor(white);
    gaddr world_rect = world + GrafPort_portRect_off;
    tb_LockPixels(pm);
    if (to_screen)
        tb_CopyBits(rd32(pm), screen, world_rect, rect, 0 /* srcCopy */, 0);
    else
        tb_CopyBits(screen, rd32(pm), rect, world_rect, 0 /* srcCopy */, 0);
    tb_UnlockPixels(pm);
    tb_RGBForeColor(fg);
    tb_RGBBackColor(fg + 6);
    tb_SetClip(clip);
    tb_DisposeRgn(clip);
    tb_DisposePtr(fg);
    tb_DisposePtr(black);
    tb_DisposePtr(white);
    tb_SetPort(saved);
}

static gaddr save_bits(gaddr rect)
{
    gaddr b = tb_NewPtr(SB_SIZE, true, true);
    if (!b)
        return 0;
    gmemcpy(b + SB_RECT, rect, 8);
    gaddr world = tb_NewPtr(4, false, true);
    int16_t err = tb_NewGWorld(world, 0, b + SB_RECT, 0, 0, 0);
    gaddr w = rd32(world);
    tb_DisposePtr(world);
    if (err || !w) {
        tb_DisposePtr(b);
        return 0;
    }
    wr32(b + SB_WORLD, w);
    copy_screen(w, b + SB_RECT, false);
    return b;
}

static void SaveScreenBits(ppc_cpu *c)
{
    gaddr rect = R(0), out = R(2);
    if (!rect || !out) {
        RETURN(paramErr);
        return;
    }
    wr32(out, 0);
    if (!bit_saving || qrect_empty(qrect_get(rect))) {
        RETURN(noErr);
        return;
    }
    gaddr b = save_bits(rect);
    wr32(out, b);
    RETURN(b ? noErr : memFullErr);
}

/* RestoreScreenBits(bits, ...): the screen as it was, where the bits are
 * now (MoveSavedScreenBits). */
static void RestoreScreenBits(ppc_cpu *c)
{
    for (gaddr b = R(0); b; b = rd32(b + SB_NEXT))
        if (rd32(b + SB_WORLD))
            copy_screen(rd32(b + SB_WORLD), b + SB_RECT, true);
    RETURN(noErr);
}

static void DisposeSavedScreenBits(ppc_cpu *c)
{
    for (gaddr b = R(0); b;) {
        gaddr next = rd32(b + SB_NEXT);
        if (rd32(b + SB_WORLD))
            tb_DisposeGWorld(rd32(b + SB_WORLD));
        tb_DisposePtr(b);
        b = next;
    }
    RETURN(noErr);
}

/* MoveSavedScreenBits(bits, h, v): to be put back with their top left
 * there. */
static void MoveSavedScreenBits(ppc_cpu *c)
{
    for (gaddr b = R(0); b; b = rd32(b + SB_NEXT)) {
        qrect r = qrect_get(b + SB_RECT);
        tb_OffsetRect(b + SB_RECT, (int16_t)((int16_t)R(1) - r.left), (int16_t)((int16_t)R(2) - r.top));
    }
    DONE();
}

static void GetSavedScreenBitsRepresentativePixMap(ppc_cpu *c)
{
    gaddr b = R(0);
    RETURN(b && rd32(b + SB_WORLD) ? tb_GetGWorldPixMap(rd32(b + SB_WORLD)) : 0);
}

static void SetScreenBitSavingEnabled(ppc_cpu *c)
{
    bit_saving = (uint8_t)R(0) != 0;
    RETURN(noErr);
}
static void IsScreenBitSavingEnabled(ppc_cpu *c) { RETURN(bit_saving); }

/* ---------------------------------------------------------------------- */
/* The Menu Manager */

static void StandardMDEF(ppc_cpu *c)
{
    menu_standard_mdef((int16_t)R(0), R(1), R(2), R(3), R(4));
    DONE();
}

/* StandardMBDF(selector, message, param1, param2) */
static void StandardMBDF(ppc_cpu *c) { RETURN(menu_standard_mbdf((int16_t)R(1), (int16_t)R(2), (int32_t)R(3))); }

/* CallMenuBar(message, param1, param2): the bar's definition, if there's a
 * menu bar. */
static void CallMenuBar(ppc_cpu *c)
{
    gaddr ml = rd32(LM_MenuList);
    RETURN(ml && ml != 0xFFFFFFFFu ? menu_standard_mbdf((int16_t)R(0), (int16_t)R(1), (int32_t)R(2)) : 0);
}

/* CallMenuDef(menu, message, &rect, hitPt, &item) */
static void CallMenuDef(ppc_cpu *c)
{
    if (R(0) && rd32(R(0)))
        menu_call_def(R(0), (int16_t)R(1), R(2), R(3), R(4));
    DONE();
}

static void SetMenuBarHeight(ppc_cpu *c)
{
    wm_set_menu_bar_height((int16_t)R(0));
    DONE();
}

static void SetMenuBarSkipDrawCount(ppc_cpu *c)
{
    menu_skip_draws((int32_t)R(0));
    DONE();
}

/* (The bar is drawn when it's invalidated: nothing waits.) */
static void ValidateMenuBar(ppc_cpu *c) { DONE(); }
static void MenuBarVisibleKludgeNotification(ppc_cpu *c) { DONE(); }

/* InvalidateAllMenuSizes: every menu in the bar measured again. */
static void InvalidateAllMenuSizes(ppc_cpu *c)
{
    gaddr ml = rd32(LM_MenuList);
    if (ml && rd32(ml)) {
        int n = (int16_t)rd16(rd32(ml)) / 6;
        for (int i = 0; i < n; i++)
            tb_CalcMenuSize(rd32(rd32(ml) + 6 + (uint32_t)i * 6));
    }
    DONE();
}

/* AdjustDesktopRgnForMenuBar(rgn, device): the menu bar taken out of a
 * desktop region (the main screen's top). */
static void AdjustDesktopRgnForMenuBar(ppc_cpu *c)
{
    gaddr rgn = R(0);
    int16_t h = (int16_t)rd16(LM_MBarHeight);
    if (rgn && rd32(rgn) && h > 0) {
        gaddr bar = tb_NewRgn();
        tb_SetRectRgn(bar, 0, 0, (int16_t)g_screen_width, h);
        tb_DiffRgn(rgn, bar, rgn);
        tb_DisposeRgn(bar);
    }
    RETURN(noErr);
}

/* Submenus whose parent item can be chosen itself. */
enum { kChoosableTag = 0x63686F73 }; /* 'chos' */

static void SetSubmenuParentChoosable(ppc_cpu *c)
{
    RETURN(set_item_flag(R(0), (int16_t)R(1), kChoosableTag, (uint8_t)R(2) != 0));
}

static void GetSubmenuParentChoosable(ppc_cpu *c)
{
    gaddr mh = R(0), out = R(2);
    if (!out || !mh || !rd32(mh)) {
        RETURN(paramErr);
        return;
    }
    wr8(out, item_flag(mh, (int16_t)R(1), kChoosableTag));
    RETURN(noErr);
}

/* ---- Cached menu pixels ---- */

/* A menu's image as drawn, kept so that it can be shown again without
 * drawing (CacheMenuPixels after drawing it, RedrawCachedMenuPixels). */
#define MAX_CACHED_MENUS 16
static struct {
    gaddr menu, bits;
} menu_cache[MAX_CACHED_MENUS];

static int cached_menu(gaddr mh)
{
    for (int i = 0; i < MAX_CACHED_MENUS; i++)
        if (mh && menu_cache[i].menu == mh)
            return i;
    return -1;
}

static void uncache_menu(int i)
{
    if (menu_cache[i].bits) {
        tb_DisposeGWorld(rd32(menu_cache[i].bits + SB_WORLD));
        tb_DisposePtr(menu_cache[i].bits);
    }
    menu_cache[i].menu = menu_cache[i].bits = 0;
}

/* CacheMenuPixels(menu, &rect) */
static void CacheMenuPixels(ppc_cpu *c)
{
    gaddr mh = R(0), rect = R(1);
    if (!mh || !rect || qrect_empty(qrect_get(rect))) {
        RETURN(paramErr);
        return;
    }
    /* Its own slot, a free one, or the first one's taken over. */
    int i = cached_menu(mh);
    for (int k = 0; i < 0 && k < MAX_CACHED_MENUS; k++)
        if (!menu_cache[k].menu)
            i = k;
    if (i < 0)
        i = 0;
    uncache_menu(i);
    gaddr b = save_bits(rect);
    if (!b) {
        RETURN(memFullErr);
        return;
    }
    menu_cache[i].menu = mh;
    menu_cache[i].bits = b;
    RETURN(noErr);
}

/* RedrawCachedMenuPixels(menu, &rect): whether it was cached (at this
 * size) and so shown. */
static void RedrawCachedMenuPixels(ppc_cpu *c)
{
    int i = cached_menu(R(0));
    if (i < 0 || !R(1)) {
        RETURN(0);
        return;
    }
    gaddr b = menu_cache[i].bits;
    qrect was = qrect_get(b + SB_RECT), now = qrect_get(R(1));
    if (was.bottom - was.top != now.bottom - now.top || was.right - was.left != now.right - now.left) {
        RETURN(0);
        return;
    }
    gmemcpy(b + SB_RECT, R(1), 8);
    copy_screen(rd32(b + SB_WORLD), b + SB_RECT, true);
    RETURN(1);
}

static void InvalidateCachedMenuPixels(ppc_cpu *c)
{
    int i = cached_menu(R(0));
    if (i >= 0)
        uncache_menu(i);
    DONE();
}

static void InvalidateAllCachedMenuPixels(ppc_cpu *c)
{
    for (int i = 0; i < MAX_CACHED_MENUS; i++)
        uncache_menu(i);
    DONE();
}

/* ---- The menu bar clock ---- */

/* The Date & Time control panel's clock at the menu bar's right: the time
 * drawn with the bar and again as the minute changes (TickleMenuBarClock,
 * which the clock's owner calls).  Its configuration, a version word
 * (0) and 16 bytes, is kept as given; the time is drawn as h:mm AM. */
static struct {
    bool on;
    uint8_t config[16];
    int minute; /* drawn for, or -1 */
} clock_state = { .minute = -1 };

void os9tbx_draw_clock(void)
{
    if (!clock_state.on || !rd32(LM_WMgrPort))
        return;
    int16_t bar = (int16_t)rd16(LM_MBarHeight);
    if (bar <= 0)
        return;
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    char text[16];
    int hour = lt.tm_hour % 12 ? lt.tm_hour % 12 : 12;
    snprintf(text, sizeof text, "%d:%02d %s", hour, lt.tm_min, lt.tm_hour < 12 ? "AM" : "PM");
    clock_state.minute = lt.tm_hour * 60 + lt.tm_min;
    gaddr saved = qd_port();
    gaddr wp = rd32(LM_WMgrPort);
    tb_SetPort(wp);
    tb_SetRectRgn(GrafPort_clipRgn(wp), -32767, -32767, 32767, 32767);
    GrafPort_set_txFont(wp, 0);
    GrafPort_set_txSize(wp, 12);
    GrafPort_set_txFace(wp, 0);
    gaddr s = tb_NewPtr(256, false, false);
    pstr_put(s, text);
    int width = tb_StringWidth(s);
    int right = g_screen_width - 10, left = right - width - 6;
    if (left < menu_bar_titles_end()) {
        tb_DisposePtr(s);
        tb_SetPort(saved);
        return; /* the titles reach it */
    }
    gaddr r = tb_NewPtr(8, false, false);
    qrect_put(r, (qrect){ 0, (int16_t)left, (int16_t)(bar - 1), (int16_t)right });
    tb_EraseRect(r);
    tb_MoveTo((int16_t)(left + 3), 14);
    tb_DrawString(s);
    tb_DisposePtr(r);
    tb_DisposePtr(s);
    tb_SetPort(saved);
}

static void EnableMenuBarClock(ppc_cpu *c)
{
    if (clock_state.on) {
        RETURN(menuUsesSystemDefErr);
        return;
    }
    clock_state.on = true;
    clock_state.minute = -1;
    os9tbx_draw_clock();
    RETURN(noErr);
}

static void DisableMenuBarClock(ppc_cpu *c)
{
    if (!clock_state.on) {
        RETURN(menuNotFoundErr);
        return;
    }
    clock_state.on = false;
    tb_DrawMenuBar();
    RETURN(noErr);
}

static void IsMenuBarClockEnabled(ppc_cpu *c) { RETURN(clock_state.on); }

/* Set/GetMenuBarClockConfig(&config): version 0 only. */
static void SetMenuBarClockConfig(ppc_cpu *c)
{
    gaddr cfg = R(0);
    if (!cfg || rd16(cfg) != 0) {
        RETURN(paramErr);
        return;
    }
    gget(clock_state.config, cfg + 2, sizeof clock_state.config);
    clock_state.minute = -1;
    RETURN(noErr);
}

static void GetMenuBarClockConfig(ppc_cpu *c)
{
    gaddr cfg = R(0);
    if (!cfg || rd16(cfg) != 0) {
        RETURN(paramErr);
        return;
    }
    gput(cfg + 2, clock_state.config, sizeof clock_state.config);
    RETURN(noErr);
}

/* RefreshMenuBarClock: drawn at the next tickle, whatever the time. */
static void RefreshMenuBarClock(ppc_cpu *c)
{
    clock_state.minute = -1;
    RETURN(noErr);
}

static void TickleMenuBarClock(ppc_cpu *c)
{
    if (!clock_state.on) {
        RETURN(menuNotFoundErr);
        return;
    }
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    if (lt.tm_hour * 60 + lt.tm_min != clock_state.minute)
        os9tbx_draw_clock();
    RETURN(noErr);
}

/* ---------------------------------------------------------------------- */
/* The Appearance Manager: the control panel's settings */

static appearance_settings settings;
static bool settings_made;

static void settings_default(void)
{
    memset(&settings, 0, sizeof settings);
    settings_made = true;
    snprintf(settings.font[kThemeFontSystem], sizeof settings.font[0], "Charcoal");
    snprintf(settings.font[kThemeFontSmallSystem], sizeof settings.font[0], "Geneva");
    snprintf(settings.font[kThemeFontViews], sizeof settings.font[0], "Geneva");
    settings.views_size = 10;
    settings.arrow_style = 0; /* kThemeScrollBarArrowsSingle */
    settings.thumb_style = 1; /* kThemeScrollBarThumbProportional */
    settings.check_style = 1; /* kThemeCheckBoxCheckMark */
    settings.smoothing_size = 12;
    settings.dbl_click = 1;
}

appearance_settings *ap_settings(void)
{
    if (!settings_made)
        settings_default();
    return &settings;
}

/* Whether a font of that name is installed (the system font's name being
 * the one GetFNum answers 0 for). */
static bool font_installed(gaddr name)
{
    if (!name || !rd8(name))
        return false;
    gaddr num = tb_NewPtr(2, false, true), sys = tb_NewPtr(256, false, true);
    tb_GetFNum(name, num);
    bool ok = rd16(num) != 0;
    if (!ok) {
        tb_GetFontName(0, sys);
        char a[64], b[64];
        pstr_get(name, a, sizeof a);
        pstr_get(sys, b, sizeof b);
        ok = !strcasecmp(a, b);
    }
    tb_DisposePtr(num);
    tb_DisposePtr(sys);
    return ok;
}

/* Everything drawn again in the new look (a setter's update flag). */
static void redraw_all(void)
{
    tb_DrawMenuBar();
    wm_repaint_all();
}

static int16_t set_font(int which, gaddr name)
{
    if (!font_installed(name))
        return kFontNotInstalledErr;
    pstr_get(name, ap_settings()->font[which], sizeof settings.font[0]);
    return noErr;
}

/* Get/SetSystemFont and Get/SetSmallSystemFont(script, name) and
 * (name, script, update); Get/SetViewsFont with a size. */
static void get_font(ppc_cpu *c, int which, gaddr name, gaddr size)
{
    if (!name || (which == kThemeFontViews && !size)) {
        RETURN(paramErr);
        return;
    }
    pstr_put(name, ap_settings()->font[which]);
    if (size)
        wr16(size, (uint16_t)ap_settings()->views_size);
    RETURN(noErr);
}

static void GetSystemFont(ppc_cpu *c) { get_font(c, kThemeFontSystem, R(1), 0); }
static void GetSmallSystemFont(ppc_cpu *c) { get_font(c, kThemeFontSmallSystem, R(1), 0); }
static void GetViewsFont(ppc_cpu *c) { get_font(c, kThemeFontViews, R(1), R(2)); }

static void SetSystemFont(ppc_cpu *c) { RETURN(set_font(kThemeFontSystem, R(0))); }
static void SetSmallSystemFont(ppc_cpu *c) { RETURN(set_font(kThemeFontSmallSystem, R(0))); }

static void SetViewsFont(ppc_cpu *c)
{
    if (!R(0) || (int16_t)R(1) < 9) {
        RETURN(paramErr);
        return;
    }
    int16_t err = set_font(kThemeFontViews, R(0));
    if (err == noErr)
        ap_settings()->views_size = (int16_t)R(1);
    RETURN(err);
}

/* Font smoothing: on or off, from a size of 12 to 24 points (0: the size
 * it has). */
static void SetThemeFontSmoothing(ppc_cpu *c)
{
    uint16_t size = (uint16_t)R(1) ? (uint16_t)R(1) : ap_settings()->smoothing_size;
    if (size < 12 || size > 24) {
        RETURN(paramErr);
        return;
    }
    ap_settings()->smoothing = (uint8_t)R(0) != 0;
    ap_settings()->smoothing_size = size;
    RETURN(noErr);
}

static void GetThemeFontSmoothing(ppc_cpu *c)
{
    if (!R(0) && !R(1)) {
        RETURN(paramErr);
        return;
    }
    if (R(0))
        wr8(R(0), ap_settings()->smoothing);
    if (R(1))
        wr16(R(1), ap_settings()->smoothing_size);
    RETURN(noErr);
}

/* Scroll bars' arrows (single, double, both ends) and thumbs (fixed or
 * proportional), and check boxes' marks (check or cross), with everything
 * drawn again if asked. */
static void set_style(ppc_cpu *c, uint16_t *field, uint16_t max)
{
    uint16_t v = (uint16_t)R(0);
    if (v > max) {
        RETURN(paramErr);
        return;
    }
    *field = v;
    if ((uint8_t)R(1))
        redraw_all();
    RETURN(noErr);
}

static void SetThemeScrollBarArrowStyle(ppc_cpu *c) { set_style(c, &ap_settings()->arrow_style, 2); }
static void SetThemeScrollBarThumbStyle(ppc_cpu *c) { set_style(c, &ap_settings()->thumb_style, 1); }
static void SetThemeCheckBoxStyle(ppc_cpu *c) { set_style(c, &ap_settings()->check_style, 1); }

/* SetHighlightColor(&rgb, ...): the text highlight colour. */
static void SetHighlightColor(ppc_cpu *c)
{
    gaddr rgb = R(0);
    if (!rgb) {
        RETURN(paramErr);
        return;
    }
    gmemcpy(LM_HiliteRGB, rgb, 6);
    appearance_hilite_changed(rd16(rgb), rd16(rgb + 2), rd16(rgb + 4));
    RETURN(noErr);
}

/* SetThemeCursorWithOptions(cursor, options, ...) */
static void SetThemeCursorWithOptions(ppc_cpu *c)
{
    if (R(0) > 17) {
        RETURN(themeBadCursorIndexErr);
        return;
    }
    RETURN(tb_SetThemeCursor(R(0)));
}

/* SetThemeBackgroundForWindow(?, window, update): the window's background
 * the theme's for windows, Platinum's dialog grey. */
static void SetThemeBackgroundForWindow(ppc_cpu *c)
{
    if (wm_is_window(R(1)))
        tb_SetThemeWindowBackground(R(1), 1 /* kThemeBrushDialogBackgroundActive */, (int8_t)R(2));
    RETURN(noErr);
}

/* ---- The appearance, its variants, and themes ---- */

/* The one appearance there is (built in, not a file), and its one
 * variant, as GetTheme names them. */
static const char appearance_name[] = "Apple platinum", variant_name[] = "Platinum";

/* GetAppearance(&appearance, &variant), GetCurrentTheme alike */
static void GetAppearance(ppc_cpu *c)
{
    if (!R(0) && !R(1)) {
        RETURN(paramErr);
        return;
    }
    if (R(0))
        pstr_put(R(0), appearance_name);
    if (R(1))
        pstr_put(R(1), variant_name);
    RETURN(noErr);
}

/* SetAppearance(appearance, variant, ...): only the one there is. */
static bool names_appearance(gaddr name)
{
    char s[256];
    pstr_get(name, s, sizeof s);
    return !name || !s[0] || !strcasecmp(s, appearance_name);
}

static void SetAppearance(ppc_cpu *c)
{
    char v[256];
    pstr_get(R(1), v, sizeof v);
    RETURN(names_appearance(R(0)) && (!v[0] || !strcasecmp(v, variant_name)) ? noErr : fnfErr);
}

/* A variant list: a handle of 4 bytes, a count and Str63 names. */
enum { VL_COUNT = 4, VL_NAMES = 6, VL_NAME_SIZE = 64 };

/* GetThemeVariantList(appearance, &list) */
static void GetThemeVariantList(ppc_cpu *c)
{
    if (!R(1)) {
        RETURN(paramErr);
        return;
    }
    wr32(R(1), 0);
    if (!names_appearance(R(0))) {
        RETURN(fnfErr);
        return;
    }
    gaddr h = tb_NewHandle(VL_NAMES + VL_NAME_SIZE, true, true);
    if (!h) {
        RETURN(memFullErr);
        return;
    }
    wr16(rd32(h) + VL_COUNT, 1);
    pstr_put(rd32(h) + VL_NAMES, variant_name);
    wr32(R(1), h);
    RETURN(noErr);
}

static void CountThemeVariants(ppc_cpu *c)
{
    if (!R(0) || !rd32(R(0)) || !R(1)) {
        RETURN(paramErr);
        return;
    }
    wr16(R(1), rd16(rd32(R(0)) + VL_COUNT));
    RETURN(noErr);
}

/* GetIndThemeVariant(list, index, &name): 1 up. */
static void GetIndThemeVariant(ppc_cpu *c)
{
    gaddr h = R(0);
    int16_t i = (int16_t)R(1);
    if (!h || !rd32(h) || !R(2) || i < 1 || i > (int16_t)rd16(rd32(h) + VL_COUNT)) {
        RETURN(paramErr);
        return;
    }
    gmemcpy(R(2), rd32(h) + VL_NAMES + (uint32_t)(i - 1) * VL_NAME_SIZE, VL_NAME_SIZE);
    RETURN(noErr);
}

/* GetThemeVariantByName(list, name, &index) */
static void GetThemeVariantByName(ppc_cpu *c)
{
    gaddr h = R(0);
    if (!h || !rd32(h) || !R(2)) {
        RETURN(paramErr);
        return;
    }
    char want[256], have[256];
    pstr_get(R(1), want, sizeof want);
    int n = (int16_t)rd16(rd32(h) + VL_COUNT);
    for (int i = 1; i <= n; i++) {
        pstr_get(rd32(h) + VL_NAMES + (uint32_t)(i - 1) * VL_NAME_SIZE, have, sizeof have);
        if (!strcasecmp(want, have)) {
            wr16(R(2), (uint16_t)i);
            RETURN(noErr);
            return;
        }
    }
    RETURN(paramErr);
}

/* Themes (scenarios): GetScenario and SetScenario are GetTheme and
 * SetTheme; the built-in theme is no file, so there are none to iterate,
 * delete or save to. */
static void GetScenario(ppc_cpu *c) { forward(c, "GetTheme"); }
static void SetScenario(ppc_cpu *c) { forward(c, "SetTheme"); }
static void IterateThemeScenarios(ppc_cpu *c) { RETURN(noErr); }
static void DeleteUserScenario(ppc_cpu *c) { RETURN(fnfErr); }
static void GetUserScenarioFileName(ppc_cpu *c)
{
    if (R(0))
        pstr_put(R(0), "Custom Theme");
    RETURN(noErr);
}

/* (What an iteration procedure is passed isn't known; a theme file isn't
 * written; the credits are the theme file's.) */
static void IterateThemeVariants(ppc_cpu *c) { RETURN(unimpErr); }
static void SaveUserScenario(ppc_cpu *c) { RETURN(unimpErr); }
static void IsThemeCreditDialogAvailable(ppc_cpu *c) { RETURN(0); }
static void DisplayThemeCreditDialog(ppc_cpu *c) { RETURN(unimpErr); }

static void IsValidThemeFileType(ppc_cpu *c) { RETURN(tb_IsValidAppearanceFileType((int32_t)R(0))); }

/* Platinum: drawn at any depth on any screen, best from 256 colours; a
 * plain menu bar, its titles from 10 pixels in. */
static void CanThemeDrawOnAllMonitors(ppc_cpu *c) { RETURN(1); }
static void GetThemeMinimumDepth(ppc_cpu *c) { RETURN(1); }
static void GetThemeRecommendedDepth(ppc_cpu *c) { RETURN(8); }
static void GetThemeMenuBarShape(ppc_cpu *c) { RETURN(0); }
static void GetThemeMenuTitleOffset(ppc_cpu *c)
{
    if (R(0))
        wr16(R(0), 10);
    RETURN(R(0) ? noErr : paramErr);
}

/* DrawThemeProcessMenuHandle(&rect, state): the grip beside the
 * Application menu's title, dark and light ridges. */
static void DrawThemeProcessMenuHandle(ppc_cpu *c)
{
    if (!R(0)) {
        RETURN(paramErr);
        return;
    }
    qrect r = qrect_get(R(0));
    int x = (r.left + r.right) / 2 - 2;
    for (int y = r.top + 3; y < r.bottom - 3; y += 2) {
        ap_fill_grey((qrect){ (int16_t)y, (int16_t)x, (int16_t)(y + 1), (int16_t)(x + 1) }, 0x5555);
        ap_fill_grey((qrect){ (int16_t)(y + 1), (int16_t)(x + 1), (int16_t)(y + 2), (int16_t)(x + 2) }, 0xFFFF);
        ap_fill_grey((qrect){ (int16_t)y, (int16_t)(x + 3), (int16_t)(y + 1), (int16_t)(x + 4) }, 0x5555);
        ap_fill_grey((qrect){ (int16_t)(y + 1), (int16_t)(x + 4), (int16_t)(y + 2), (int16_t)(x + 5) }, 0xFFFF);
    }
    RETURN(noErr);
}

static void RecordAnimationTimeout(ppc_cpu *c) { DONE(); }

/* ---- Theme sounds ---- */

/* On or off, which of them, and how widely they pan; there is no sound
 * track (none was installed), so none play. */
static int32_t sound_spread;
static char sound_track[64];

static void SetThemeSoundsEnabled(ppc_cpu *c)
{
    ap_settings()->sounds = (uint8_t)R(0) != 0;
    RETURN(noErr);
}
static void AreThemeSoundsEnabled(ppc_cpu *c) { RETURN(ap_settings()->sounds); }

static void SetThemeSoundMask(ppc_cpu *c)
{
    ap_settings()->sound_mask = R(0);
    RETURN(noErr);
}
static void GetThemeSoundMask(ppc_cpu *c)
{
    if (!R(0)) {
        RETURN(paramErr);
        return;
    }
    wr32(R(0), ap_settings()->sound_mask);
    RETURN(noErr);
}

static void SetThemeSoundPanningSpread(ppc_cpu *c)
{
    sound_spread = (int32_t)R(0);
    RETURN(noErr);
}
static void GetThemeSoundPanningSpread(ppc_cpu *c)
{
    if (R(0))
        wr32(R(0), (uint32_t)sound_spread);
    RETURN(R(0) ? noErr : paramErr);
}

/* (Mac OS 9's own takes no notice of the volume.) */
static void SetThemeSoundVolume(ppc_cpu *c) { RETURN(noErr); }
static void AreThemeSoundsPlaying(ppc_cpu *c) { RETURN(0); }
static void PlayThemeControlSound(ppc_cpu *c) { RETURN(noErr); }
static void BeginThemeDragIndicatorSound(ppc_cpu *c) { RETURN(noErr); }

/* GetThemePlaylist(&name): the sound track chosen; SetThemePlaylist(name):
 * none but "none" (an empty name) can be, there being no sound track
 * files. */
static void GetThemePlaylist(ppc_cpu *c)
{
    if (!R(0)) {
        RETURN(paramErr);
        return;
    }
    pstr_put(R(0), sound_track);
    RETURN(sound_track[0] ? noErr : kNoSoundTrackErr);
}

static void SetThemePlaylist(ppc_cpu *c)
{
    char name[256];
    pstr_get(R(0), name, sizeof name);
    if (name[0]) {
        RETURN(fnfErr);
        return;
    }
    sound_track[0] = 0;
    RETURN(noErr);
}

/* SetTheme's collection: the settings in it taken. */
enum { kThemeScrollBarArrowStyleTag = 0x73626172, kThemeScrollBarThumbStyleTag = 0x73627468,
       kThemeSoundsEnabledTag = 0x736E6473, kThemeDblClickCollapseTag = 0x636F6C6C,
       kThemeSystemFontTag = 0x6C677366, kThemeSmallSystemFontTag = 0x736D7366, kThemeViewsFontTag = 0x76666E74,
       kThemeViewsFontSizeTag = 0x7666737A, kThemeSoundMaskTag = 0x736D736B, kThemeSmoothFontEnabledTag = 0x736D6F6F,
       kThemeSmoothFontMinSizeTag = 0x736D6F73 };

static bool theme_item(gaddr coll, uint32_t tag, gaddr buf, uint32_t max, uint32_t *size)
{
    gaddr sz = tb_NewPtr(4, false, true);
    wr32(sz, max);
    bool ok = tb_GetCollectionItem(coll, (int32_t)tag, 0, sz, buf) == noErr;
    *size = rd32(sz);
    tb_DisposePtr(sz);
    return ok && *size <= max;
}

int16_t ap_apply_theme(gaddr coll)
{
    gaddr buf = tb_NewPtr(256, false, true);
    if (!buf)
        return memFullErr;
    uint32_t n;
    int16_t err = noErr;
    if (theme_item(coll, kThemeScrollBarArrowStyleTag, buf, 2, &n) && n == 2 && rd16(buf) <= 2)
        ap_settings()->arrow_style = rd16(buf);
    if (theme_item(coll, kThemeScrollBarThumbStyleTag, buf, 2, &n) && n == 2 && rd16(buf) <= 1)
        ap_settings()->thumb_style = rd16(buf);
    if (theme_item(coll, kThemeSoundsEnabledTag, buf, 1, &n) && n == 1)
        ap_settings()->sounds = rd8(buf) != 0;
    if (theme_item(coll, kThemeDblClickCollapseTag, buf, 1, &n) && n == 1)
        ap_settings()->dbl_click = rd8(buf) != 0;
    if (theme_item(coll, kThemeSoundMaskTag, buf, 4, &n) && n == 4)
        ap_settings()->sound_mask = rd32(buf);
    if (theme_item(coll, kThemeSmoothFontEnabledTag, buf, 1, &n) && n == 1)
        ap_settings()->smoothing = rd8(buf) != 0;
    if (theme_item(coll, kThemeSmoothFontMinSizeTag, buf, 2, &n) && n == 2 && rd16(buf) >= 12 && rd16(buf) <= 24)
        ap_settings()->smoothing_size = rd16(buf);
    static const struct {
        uint32_t tag;
        int which;
    } fonts[] = { { kThemeSystemFontTag, kThemeFontSystem },
                  { kThemeSmallSystemFontTag, kThemeFontSmallSystem },
                  { kThemeViewsFontTag, kThemeFontViews } };
    for (unsigned i = 0; i < 3; i++)
        if (theme_item(coll, fonts[i].tag, buf, 256, &n) && n >= 1 && rd8(buf) + 1u <= n) {
            int16_t e = set_font(fonts[i].which, buf);
            if (e && !err)
                err = e;
        }
    if (theme_item(coll, kThemeViewsFontSizeTag, buf, 2, &n) && n == 2 && (int16_t)rd16(buf) >= 9)
        ap_settings()->views_size = (int16_t)rd16(buf);
    tb_DisposePtr(buf);
    return err;
}

/* ---- The Appearance Manager's calls the other managers reach ---- */

/* GetMenuItemBounds(menu, item, &rect): the item's rectangle in its menu,
 * the menu's top left at 0,0. */
int16_t tb_GetMenuItemBounds(gaddr mh, int16_t item, gaddr out)
{
    qrect r;
    if (!out || !menu_item_bounds(mh, item, &r))
        return paramErr;
    qrect_put(out, r);
    return noErr;
}

/* Set/GetMenuItemPencilGlyph(menu, item, on): whether an item shows the
 * glyph that marks it editable (kept; the standard MDEF here draws no
 * glyphs beside the command key's). */
enum { kPencilTag = 0x70636E6C }; /* 'pcnl' */

int16_t tb_SetMenuItemPencilGlyph(gaddr mh, int16_t item, int8_t on)
{
    return set_item_flag(mh, item, kPencilTag, on != 0);
}

int16_t tb_GetMenuItemPencilGlyph(gaddr mh, int16_t item, gaddr out)
{
    if (!out || !mh || !rd32(mh) || item < 1 || item > tb_CountMItems(mh))
        return paramErr;
    wr8(out, item_flag(mh, item, kPencilTag));
    return noErr;
}

/* The Control Manager's Drag Manager handlers (those automatic drag
 * tracking installs in a window), for the System to install itself; and
 * installed for every window. */
gaddr tb_GetControlManagerDragTrackingHandler(void) { return ctl_drag_tracking_handler(); }
gaddr tb_GetControlManagerDragReceiveHandler(void) { return ctl_drag_receive_handler(); }

static bool drag_handlers_installed;

int16_t tb_InstallControlManagerDragHandlers(void)
{
    bool installed = drag_handlers_installed;
    if (installed)
        return noErr;
    int16_t err = tb_InstallTrackingHandler(ctl_drag_tracking_handler(), 0, 0);
    if (err == noErr)
        err = tb_InstallReceiveHandler(ctl_drag_receive_handler(), 0, 0);
    drag_handlers_installed = err == noErr;
    return err;
}

/* ---------------------------------------------------------------------- */

#define E(name) { #name, name }
static const ppc_export os9tbx_exports[] = {
    /* WindowsLib */
    E(GetWindowSystemProperty), E(SetWindowSystemProperty), E(GetWindowSystemPropertySize),
    E(RemoveWindowSystemProperty), E(GetWindowSystemPropertyAttributes), E(ChangeWindowSystemPropertyAttributes),
    E(WMCollapseWindow), E(WMCollapseAllWindows), E(WMGetWindowFeatures), E(WMGetWindowRegion),
    E(WMIsWindowCollapsable), E(WMIsWindowCollapsed),
    E(ActivateFloatingWindows), E(DeactivateFloatingWindows), E(AreFloatingWindowsActive),
    E(BringProcessWindowsToFront), E(HideProcessWindows), E(ShowProcessWindows), E(EnableWindowBuffering),
    E(DisableWindowBuffering), E(IsWindowBufferingEnabled), E(WindowHasUnicodeTitle), E(GetWindowUnicodeTitle),
    E(SetWindowUnicodeTitle), E(GetWindowProcess), E(GetWindowTitleLength), E(GetWindowVisualClass),
    E(SetWindowFeatures), E(SetWindowGadgetDropThreshold), E(GetWindowGadgetDropThreshold),
    E(SetWindowDoubleClickAction), E(GetWindowDoubleClickAction), E(DoubleClickWindowTitle), E(GlobalFindWindow),
    E(ConstrainedDragRgn), E(EraseWindowRegion), E(WindowManagerSetThemeWindowBackground),
    E(PostWindowActivateEvent), E(GetNextWindowActivateEvent), E(ReshapeWindows), E(PostDelayedWindowReshape),
    E(RebuildDesktopRegion), E(RecalculateAllWindowTitleWidths), E(DrawDesktop),
    E(RegisterWindowNotificationProc), E(UnregisterWindowNotificationProc),
    E(SetDesktopPattern), E(GetDesktopPattern), E(GetDesktopPixPat), E(GetPictureFromFile),
    E(FileContainsPossibleDesktopPicture), E(InternalMonitorDesktopListChanged),
    { "CopyMonitorDesktopFile", desktop_list_unknown }, { "CountMonitorDesktopRecordsInList", desktop_list_unknown },
    { "DisposeMonitorDesktopList", desktop_list_unknown }, { "DrawMonitorDesktopFromList", desktop_list_unknown },
    { "DrawMonitorDesktopFromListWillTakeTime", desktop_list_unknown },
    { "GetCurrentMonitorDesktopList", desktop_list_unknown }, { "GetDesktopPictureForDevice", desktop_list_unknown },
    { "GetInternalMonitorDesktopList", desktop_list_unknown }, { "GetMonitorDesktopAlignment", desktop_list_unknown },
    { "GetMonitorDesktopRecordFromList", desktop_list_unknown }, { "MonitorDesktopHasPicture", desktop_list_unknown },
    { "RefreshMonitorDesktopList", desktop_list_unknown }, { "SetCurrentMonitorDesktopList", desktop_list_unknown },
    { "SetDesktopPictureForDevice", desktop_list_unknown }, { "SetMonitorDesktopAlignment", desktop_list_unknown },
    { "SetMonitorDesktopRecordFromList", desktop_list_unknown }, { "SetMonitorDesktopToFile", desktop_list_unknown },
    E(SaveScreenBits), E(RestoreScreenBits), E(DisposeSavedScreenBits), E(MoveSavedScreenBits),
    E(GetSavedScreenBitsRepresentativePixMap), E(SetScreenBitSavingEnabled), E(IsScreenBitSavingEnabled),
    /* ControlsLib */
    E(GetControlSystemProperty), E(SetControlSystemProperty), E(GetControlSystemPropertySize),
    E(RemoveControlSystemProperty), E(GetControlSystemPropertyAttributes),
    E(ChangeControlSystemPropertyAttributes), { "__ScrollDelay", ScrollDelay },
    /* MenusLib */
    E(GetMenuItemSystemProperty), E(SetMenuItemSystemProperty), E(GetMenuItemSystemPropertySize),
    E(RemoveMenuItemSystemProperty), E(GetMenuItemSystemPropertyAttributes),
    E(ChangeMenuItemSystemPropertyAttributes),
    E(StandardMDEF), E(StandardMBDF), E(CallMenuBar), E(CallMenuDef), E(SetMenuBarHeight),
    E(SetMenuBarSkipDrawCount), E(ValidateMenuBar), E(MenuBarVisibleKludgeNotification), E(InvalidateAllMenuSizes),
    E(AdjustDesktopRgnForMenuBar), E(SetSubmenuParentChoosable), E(GetSubmenuParentChoosable),
    E(CacheMenuPixels), E(RedrawCachedMenuPixels), E(InvalidateCachedMenuPixels), E(InvalidateAllCachedMenuPixels),
    E(EnableMenuBarClock), E(DisableMenuBarClock), E(IsMenuBarClockEnabled), E(SetMenuBarClockConfig),
    E(GetMenuBarClockConfig), E(RefreshMenuBarClock), E(TickleMenuBarClock),
    /* AppearanceLib */
    E(GetSystemFont), E(GetSmallSystemFont), E(GetViewsFont), E(SetSystemFont), E(SetSmallSystemFont),
    E(SetViewsFont), E(SetThemeFontSmoothing), E(GetThemeFontSmoothing), E(SetThemeScrollBarArrowStyle),
    E(SetThemeScrollBarThumbStyle), E(SetThemeCheckBoxStyle), E(SetHighlightColor), E(SetThemeCursorWithOptions),
    E(SetThemeBackgroundForWindow), E(GetAppearance), { "GetCurrentTheme", GetAppearance }, E(SetAppearance),
    { "SetCurrentTheme", SetAppearance }, E(GetThemeVariantList), E(CountThemeVariants), E(GetIndThemeVariant),
    E(GetThemeVariantByName), E(GetScenario), E(SetScenario), E(IterateThemeScenarios), E(DeleteUserScenario),
    E(GetUserScenarioFileName), E(IterateThemeVariants), E(SaveUserScenario), E(IsThemeCreditDialogAvailable),
    E(DisplayThemeCreditDialog), E(IsValidThemeFileType), E(CanThemeDrawOnAllMonitors), E(GetThemeMinimumDepth),
    E(GetThemeRecommendedDepth), E(GetThemeMenuBarShape), E(GetThemeMenuTitleOffset),
    E(DrawThemeProcessMenuHandle), E(RecordAnimationTimeout), E(SetThemeSoundsEnabled), E(AreThemeSoundsEnabled),
    E(SetThemeSoundMask), E(GetThemeSoundMask), E(SetThemeSoundPanningSpread), E(GetThemeSoundPanningSpread),
    E(SetThemeSoundVolume), E(AreThemeSoundsPlaying), E(PlayThemeControlSound), E(BeginThemeDragIndicatorSound),
    E(GetThemePlaylist), E(SetThemePlaylist),
};
#undef E

void (*os9tbx_export_fn(const char *name))(ppc_cpu *)
{
    for (unsigned i = 0; i < sizeof os9tbx_exports / sizeof os9tbx_exports[0]; i++)
        if (!strcmp(os9tbx_exports[i].name, name))
            return os9tbx_exports[i].fn;
    if (name[0] == '_' && name[1] == '_')
        for (unsigned i = 0; i < sizeof control_entry_points / sizeof control_entry_points[0]; i++)
            if (!strcmp(control_entry_points[i], name))
                return cfm_export_fn(name + 2);
    return NULL;
}

void os9tbx_reset(void)
{
    settings_default();
    desk_pixpat = 0;
    bit_saving = true;
    memset(menu_cache, 0, sizeof menu_cache);
    clock_state.on = false;
    memset(clock_state.config, 0, sizeof clock_state.config);
    clock_state.minute = -1;
    sound_spread = 0;
    sound_track[0] = 0;
    drag_handlers_installed = false;
}
