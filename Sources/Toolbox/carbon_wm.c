/*
 * CarbonLib's Window Manager: accessors for the WindowRecord, windows'
 * bounds by region, resizing, ideal zooming, repositioning and keeping on
 * the screen, classes and attributes changed after a window is made,
 * modality, retain counts, custom definitions, content painting procs,
 * scrolling, sheets (shown app-modal, as CarbonLib showed them), titles
 * as CFStrings, and windows kept in collections.  The window's own state
 * is the WindowRecord's and the Window Manager's (wm.c); what CarbonLib
 * adds is kept here beside it, by window.
 */
#include <stdlib.h>
#include <string.h>

#define CARBON_HI_EXPORTS
#include "carbon_hi.h"
#include "qd_internal.h"
#include "tb_internal.h"

enum { unimpErr = -4 };
enum {
    errInvalidWindowRef = -5600, errUnsupportedWindowAttributesForClass = -5601, errWindowDoesNotHaveProxy = -5602,
    errInvalidWindowProperty = -5603, errWindowPropertyNotFound = -5604, errUnrecognizedWindowClass = -5605,
    errWindowRegionCodeInvalid = -30593, errWindowNotFound = -5610, windowAttributeImmutableErr = -5612,
};
enum {
    kAlertWindowClass = 1, kMovableAlertWindowClass, kModalWindowClass, kMovableModalWindowClass,
    kFloatingWindowClass, kDocumentWindowClass, kUtilityWindowClass = 8, kHelpWindowClass = 10, kSheetWindowClass,
    kToolbarWindowClass, kPlainWindowClass, kOverlayWindowClass, kSheetAlertWindowClass, kAltPlainWindowClass,
    kAllWindowClasses = 0xFFFFFFFF,
};
enum {
    kWindowCloseBoxAttribute = 1 << 0, kWindowHorizontalZoomAttribute = 1 << 1, kWindowVerticalZoomAttribute = 1 << 2,
    kWindowCollapseBoxAttribute = 1 << 3, kWindowResizableAttribute = 1 << 4, kWindowSideTitlebarAttribute = 1 << 5,
    /* what the definition draws */
    FRAME_ATTRIBUTES = 0x3F,
};
enum { kWindowStructureRgn = 32, kWindowContentRgn = 33, kWindowGlobalPortRgn = 40 };
enum { inDesk = 0, inContent = 3, inZoomIn = 7, inZoomOut = 8 };
enum { kWindowModalityNone = 0, kWindowModalityAppModal = 2 };
enum { userKind = 8, dialogKind = 2 };

/* ---- What CarbonLib keeps beside a window ---- */

typedef struct {
    gaddr w;
    uint32_t cls, attributes; /* class (0: worked out); attributes the frame doesn't show */
    uint32_t retain;
    uint32_t modality;
    gaddr unavailable, sheet_parent;
    gaddr paint_proc, paint_refcon;
    gaddr alternate_title; /* a CFString */
    bool has_ideal, has_user, has_std, drag_hilited;
    qrect ideal_user, user_state, std_state;
} win_extra;

static win_extra *extras;
static int nextras, maxextras;

static win_extra *extra_of(gaddr w, bool create)
{
    for (int i = 0; i < nextras; i++)
        if (extras[i].w == w)
            return &extras[i];
    if (!create || !w)
        return NULL;
    if (nextras == maxextras) {
        int n = maxextras ? maxextras * 2 : 16;
        win_extra *more = realloc(extras, sizeof *extras * (size_t)n);
        if (!more)
            return NULL;
        extras = more;
        maxextras = n;
    }
    win_extra *e = &extras[nextras++];
    memset(e, 0, sizeof *e);
    e->w = w;
    e->retain = 1;
    return e;
}

/* Property attributes (kWindowPropertyPersistent ...), by property. */
typedef struct {
    gaddr w;
    uint32_t creator, tag, attributes;
} prop_attr;
static prop_attr *pattrs;
static int npattrs, maxpattrs;

void carbonwm_forget(gaddr w)
{
    for (int i = 0; i < nextras; i++)
        if (extras[i].w == w) {
            if (extras[i].alternate_title)
                hi_cf_release(extras[i].alternate_title);
            extras[i] = extras[--nextras];
            break;
        }
    for (int i = 0; i < npattrs;)
        if (pattrs[i].w == w)
            pattrs[i] = pattrs[--npattrs];
        else
            i++;
}

uint32_t carbonwm_class(gaddr w)
{
    win_extra *e = extra_of(w, false);
    return e ? e->cls : 0;
}

uint32_t carbonwm_attributes(gaddr w)
{
    win_extra *e = extra_of(w, false);
    return e ? e->attributes & ~(uint32_t)FRAME_ATTRIBUTES : 0;
}

static uint32_t default_modality(uint32_t cls)
{
    switch (cls) {
    case kAlertWindowClass:
    case kMovableAlertWindowClass:
    case kModalWindowClass:
    case kMovableModalWindowClass:
    case kSheetWindowClass:
    case kSheetAlertWindowClass: return kWindowModalityAppModal;
    default: return kWindowModalityNone;
    }
}

void carbonwm_set_class(gaddr w, uint32_t cls, uint32_t attributes)
{
    win_extra *e = extra_of(w, true);
    if (e) {
        e->cls = cls;
        e->attributes = attributes;
        e->modality = default_modality(cls);
    }
}

static bool valid(gaddr w) { return wm_is_window(w); }

static uint32_t class_of(gaddr w)
{
    gaddr v = tb_NewPtr(4, false, true);
    uint32_t cls = 0;
    if (v && tb_GetWindowClass(w, v) == noErr)
        cls = rd32(v);
    if (v)
        tb_DisposePtr(v);
    return cls;
}

static uint32_t attributes_of(gaddr w)
{
    gaddr v = tb_NewPtr(4, false, true);
    uint32_t a = 0;
    if (v && tb_GetWindowAttributes(w, v) == noErr)
        a = rd32(v);
    if (v)
        tb_DisposePtr(v);
    return a;
}

/* ---- Geometry: global rectangles ---- */

static qrect bbox(gaddr rgn) { return qrect_get(rd32(rgn) + 2); }
static qrect structure_of(gaddr w) { return bbox(WindowRecord_strucRgn(w)); }
static qrect content_of(gaddr w) { return bbox(WindowRecord_contRgn(w)); }

/* The port's offset: local = global + (dh, dv). */
static void port_offset(gaddr w, int *dh, int *dv)
{
    pixdst d = qd_port_pixels(w);
    *dh = d.bounds.left;
    *dv = d.bounds.top;
}

static qrect global_port_rect(gaddr w)
{
    qrect r = qrect_get(w + GrafPort_portRect_off);
    int dh, dv;
    port_offset(w, &dh, &dv);
    return (qrect){ (int16_t)(r.top - dv), (int16_t)(r.left - dh), (int16_t)(r.bottom - dv), (int16_t)(r.right - dh) };
}

/* Content bounds placed and sized: MoveWindow, then SizeWindow. */
static void set_content(gaddr w, qrect r)
{
    qrect now = global_port_rect(w);
    if (now.left != r.left || now.top != r.top)
        tb_MoveWindow(w, r.left, r.top, 0);
    if (now.right - now.left != r.right - r.left || now.bottom - now.top != r.bottom - r.top)
        tb_SizeWindow(w, (int16_t)(r.right - r.left), (int16_t)(r.bottom - r.top), 1);
}

/* The frame's widths around the content: top, left, bottom, right. */
static qrect frame_widths(gaddr w)
{
    qrect s = structure_of(w), k = global_port_rect(w);
    return (qrect){ (int16_t)(k.top - s.top), (int16_t)(k.left - s.left), (int16_t)(s.bottom - k.bottom),
                    (int16_t)(s.right - k.right) };
}

static void set_structure(gaddr w, qrect s)
{
    qrect f = frame_widths(w);
    set_content(w, (qrect){ (int16_t)(s.top + f.top), (int16_t)(s.left + f.left), (int16_t)(s.bottom - f.bottom),
                            (int16_t)(s.right - f.right) });
}

/* A device's rectangle, less the menu bar on the main screen. */
static qrect available_bounds(gaddr gd)
{
    qrect r = qrect_get(rd32(gd) + GDevice_gdRect_off);
    if (gd == rd32(LM_MainDevice))
        r.top = (int16_t)(r.top + (int16_t)rd16(LM_MBarHeight));
    return r;
}

static int overlap(qrect a, qrect b)
{
    int w = (a.right < b.right ? a.right : b.right) - (a.left > b.left ? a.left : b.left);
    int h = (a.bottom < b.bottom ? a.bottom : b.bottom) - (a.top > b.top ? a.top : b.top);
    return w > 0 && h > 0 ? w * h : 0;
}

/* The device most of a rectangle is on (the main one when it's on none). */
static gaddr greatest_device(qrect r)
{
    gaddr best = rd32(LM_MainDevice);
    int most = 0;
    for (gaddr gd = rd32(LM_DeviceList); gd && rd32(gd); gd = GDevice_gdNextGD(rd32(gd))) {
        int a = overlap(r, qrect_get(rd32(gd) + GDevice_gdRect_off));
        if (a > most) {
            most = a;
            best = gd;
        }
    }
    return best;
}

static qrect region_bounds(gaddr w, uint16_t code, int16_t *err)
{
    gaddr rgn = tb_NewRgn();
    *err = tb_GetWindowRegion(w, code, rgn);
    qrect r = bbox(rgn);
    tb_DisposeRgn(rgn);
    return r;
}

/* ---- Accessors ---- */

static void wm_GetWindowPort(ppc_cpu *c) { RETURN(R(0)); }
static void wm_GetWindowFromPort(ppc_cpu *c) { RETURN(R(0)); }

static void wm_SetPortWindowPort(ppc_cpu *c)
{
    if (R(0))
        tb_SetPort(R(0));
    DONE();
}

static void wm_GetWindowPortBounds(ppc_cpu *c)
{
    if (R(0) && R(1))
        gmemcpy(R(1), R(0) + GrafPort_portRect_off, 8);
    RETURN(R(1));
}

static void wm_GetWindowKind(ppc_cpu *c) { RETURN(R(0) ? (int32_t)WindowRecord_windowKind(R(0)) : 0); }

static void wm_SetWindowKind(ppc_cpu *c)
{
    if (R(0))
        WindowRecord_set_windowKind(R(0), (int16_t)R(1));
    DONE();
}

static void wm_IsWindowVisible(ppc_cpu *c) { RETURN(R(0) && WindowRecord_visible(R(0))); }
static void wm_IsWindowHilited(ppc_cpu *c) { RETURN(R(0) && WindowRecord_hilited(R(0))); }
static void wm_IsWindowUpdatePending(ppc_cpu *c) { RETURN(R(0) && !tb_EmptyRgn(WindowRecord_updateRgn(R(0)))); }
static void wm_GetWindowList(ppc_cpu *c) { RETURN(rd32(LM_WindowList)); }
static void wm_GetNextWindow(ppc_cpu *c) { RETURN(R(0) ? WindowRecord_nextWindow(R(0)) : 0); }

/* The front window that isn't floating, and whether a window is the
 * active one (CarbonLib 1.5's ActiveNonFloatingWindow, IsWindowActive). */
static void wm_ActiveNonFloatingWindow(ppc_cpu *c) { RETURN(tb_FrontNonFloatingWindow()); }
static void wm_IsWindowActive(ppc_cpu *c) { RETURN(R(0) && WindowRecord_hilited(R(0))); }

/* ActivateWindow(window, activate): highlighted and told, or not. */
static void wm_ActivateWindow(ppc_cpu *c)
{
    gaddr w = R(0);
    if (!valid(w)) {
        RETURN((uint32_t)errInvalidWindowRef);
        return;
    }
    tb_HiliteWindow(w, (int8_t)((R(1) & 0xFF) != 0));
    RETURN(noErr);
}

/* InvalWindowRect and the rest: InvalRect and its kind in the window's
 * port. */
static void port_call(ppc_cpu *c, void (*fn)(gaddr))
{
    gaddr w = R(0);
    if (!valid(w) || !R(1)) {
        RETURN((uint32_t)(valid(w) ? paramErr : errInvalidWindowRef));
        return;
    }
    gaddr saved = qd_port();
    tb_SetPort(w);
    fn(R(1));
    tb_SetPort(saved);
    RETURN(noErr);
}

static void wm_InvalWindowRect(ppc_cpu *c) { port_call(c, tb_InvalRect); }
static void wm_InvalWindowRgn(ppc_cpu *c) { port_call(c, tb_InvalRgn); }
static void wm_ValidWindowRect(ppc_cpu *c) { port_call(c, tb_ValidRect); }
static void wm_ValidWindowRgn(ppc_cpu *c) { port_call(c, tb_ValidRgn); }

/* ---- Zoom states ---- */

static qrect std_state_of(gaddr w)
{
    gaddr z = wm_zoom_state(w);
    win_extra *e = extra_of(w, false);
    if (z)
        return qrect_get(rd32(z) + 8);
    return e && e->has_std ? e->std_state : wm_std_state();
}

static qrect user_state_of(gaddr w)
{
    gaddr z = wm_zoom_state(w);
    win_extra *e = extra_of(w, false);
    if (z)
        return qrect_get(rd32(z));
    return e && e->has_user ? e->user_state : global_port_rect(w);
}

static void set_std_state(gaddr w, qrect r)
{
    gaddr z = wm_zoom_state(w);
    win_extra *e = extra_of(w, true);
    if (z)
        qrect_put(rd32(z) + 8, r);
    if (e) {
        e->std_state = r;
        e->has_std = true;
    }
}

static void set_user_state(gaddr w, qrect r)
{
    gaddr z = wm_zoom_state(w);
    win_extra *e = extra_of(w, true);
    if (z)
        qrect_put(rd32(z), r);
    if (e) {
        e->user_state = r;
        e->has_user = true;
    }
}

static void wm_GetWindowStandardState(ppc_cpu *c)
{
    if (valid(R(0)) && R(1))
        qrect_put(R(1), std_state_of(R(0)));
    RETURN(R(1));
}

static void wm_GetWindowUserState(ppc_cpu *c)
{
    if (valid(R(0)) && R(1))
        qrect_put(R(1), user_state_of(R(0)));
    RETURN(R(1));
}

static void wm_SetWindowStandardState(ppc_cpu *c)
{
    if (valid(R(0)) && R(1))
        set_std_state(R(0), qrect_get(R(1)));
    DONE();
}

static void wm_SetWindowUserState(ppc_cpu *c)
{
    if (valid(R(0)) && R(1))
        set_user_state(R(0), qrect_get(R(1)));
    DONE();
}

static void wm_GetWindowIdealUserState(ppc_cpu *c)
{
    gaddr w = R(0);
    if (!valid(w) || !R(1)) {
        RETURN((uint32_t)(valid(w) ? paramErr : errInvalidWindowRef));
        return;
    }
    win_extra *e = extra_of(w, false);
    qrect_put(R(1), e && e->has_ideal ? e->ideal_user : (qrect){ 0, 0, 0, 0 });
    RETURN(noErr);
}

static void wm_SetWindowIdealUserState(ppc_cpu *c)
{
    gaddr w = R(0);
    win_extra *e = valid(w) && R(1) ? extra_of(w, true) : NULL;
    if (!e) {
        RETURN((uint32_t)(valid(w) ? paramErr : errInvalidWindowRef));
        return;
    }
    e->ideal_user = qrect_get(R(1));
    e->has_ideal = true;
    RETURN(noErr);
}

/* The standard state for an ideal size: the content where it is, that
 * size, kept on the window's screen (moved up and left, then cut down,
 * when it would leave it). */
static qrect ideal_standard(gaddr w, uint32_t size)
{
    qrect k = global_port_rect(w), area = available_bounds(greatest_device(structure_of(w)));
    qrect f = frame_widths(w);
    area.top = (int16_t)(area.top + f.top);
    area.left = (int16_t)(area.left + f.left);
    area.bottom = (int16_t)(area.bottom - f.bottom);
    area.right = (int16_t)(area.right - f.right);
    int width = pt_h(size), height = pt_v(size);
    if (width > area.right - area.left)
        width = area.right - area.left;
    if (height > area.bottom - area.top)
        height = area.bottom - area.top;
    qrect r = { k.top, k.left, (int16_t)(k.top + height), (int16_t)(k.left + width) };
    if (r.right > area.right) {
        r.left = (int16_t)(area.right - width);
        r.right = area.right;
    }
    if (r.bottom > area.bottom) {
        r.top = (int16_t)(area.bottom - height);
        r.bottom = area.bottom;
    }
    return r;
}

static bool same_rect(qrect a, qrect b)
{
    return a.top == b.top && a.left == b.left && a.bottom == b.bottom && a.right == b.right;
}

/* IsWindowInStandardState(window, idealSize, idealStandardState) */
static void wm_IsWindowInStandardState(ppc_cpu *c)
{
    gaddr w = R(0);
    if (!valid(w)) {
        RETURN(0);
        return;
    }
    qrect std = R(1) ? ideal_standard(w, rd32(R(1))) : std_state_of(w);
    if (R(2))
        qrect_put(R(2), std);
    RETURN(same_rect(std, global_port_rect(w)));
}

/* ZoomWindowIdeal(window, partCode, ioIdealSize): out to the ideal size,
 * in to the user state (the ideal one if the program set it). */
static void wm_ZoomWindowIdeal(ppc_cpu *c)
{
    gaddr w = R(0);
    int16_t part = (int16_t)R(1);
    if (!valid(w)) {
        RETURN((uint32_t)errInvalidWindowRef);
        return;
    }
    win_extra *e = extra_of(w, true);
    qrect to;
    if (part == inZoomOut) {
        if (!R(2)) {
            RETURN((uint32_t)paramErr);
            return;
        }
        to = ideal_standard(w, rd32(R(2)));
        set_user_state(w, global_port_rect(w));
        set_std_state(w, to);
        wr32(R(2), make_pt((int16_t)(to.bottom - to.top), (int16_t)(to.right - to.left)));
    } else {
        to = e && e->has_ideal && !qrect_empty(e->ideal_user) ? e->ideal_user : user_state_of(w);
    }
    set_content(w, to);
    RETURN(noErr);
}

/* ---- Bounds ---- */

/* GetWindowBounds(window, regionCode, globalBounds) */
static void wm_GetWindowBounds(ppc_cpu *c)
{
    gaddr w = R(0);
    if (!valid(w) || !R(2)) {
        RETURN((uint32_t)(valid(w) ? paramErr : errInvalidWindowRef));
        return;
    }
    int16_t err = noErr;
    qrect r = (uint16_t)R(1) == kWindowGlobalPortRgn ? global_port_rect(w) : region_bounds(w, (uint16_t)R(1), &err);
    if (!err)
        qrect_put(R(2), r);
    RETURN((uint32_t)(int32_t)err);
}

/* SetWindowBounds(window, regionCode, globalBounds): the structure or
 * the content. */
static void wm_SetWindowBounds(ppc_cpu *c)
{
    gaddr w = R(0);
    uint16_t code = (uint16_t)R(1);
    if (!valid(w) || !R(2)) {
        RETURN((uint32_t)(valid(w) ? paramErr : errInvalidWindowRef));
        return;
    }
    qrect r = qrect_get(R(2));
    if (code == kWindowStructureRgn)
        set_structure(w, r);
    else if (code == kWindowContentRgn || code == kWindowGlobalPortRgn)
        set_content(w, r);
    else {
        RETURN((uint32_t)(int32_t)errWindowRegionCodeInvalid);
        return;
    }
    RETURN(noErr);
}

/* MoveWindowStructure(window, hGlobal, vGlobal) */
static void wm_MoveWindowStructure(ppc_cpu *c)
{
    gaddr w = R(0);
    if (!valid(w)) {
        RETURN((uint32_t)errInvalidWindowRef);
        return;
    }
    qrect s = structure_of(w);
    int16_t h = (int16_t)R(1), v = (int16_t)R(2);
    set_structure(w, (qrect){ v, h, (int16_t)(v + s.bottom - s.top), (int16_t)(h + s.right - s.left) });
    RETURN(noErr);
}

/* GetWindowStructureWidths(window, outRect): the frame's width on each
 * side. */
static void wm_GetWindowStructureWidths(ppc_cpu *c)
{
    gaddr w = R(0);
    if (!valid(w) || !R(1)) {
        RETURN((uint32_t)(valid(w) ? paramErr : errInvalidWindowRef));
        return;
    }
    qrect_put(R(1), frame_widths(w));
    RETURN(noErr);
}

/* ResizeWindow(window, startPoint, sizeConstraints, newContentRect):
 * GrowWindow's tracking, then SizeWindow; whether the size changed. */
static void wm_ResizeWindow(ppc_cpu *c)
{
    gaddr w = R(0);
    if (!valid(w)) {
        RETURN(0);
        return;
    }
    gaddr limits = tb_NewPtr(8, false, true);
    if (!limits) {
        RETURN(0);
        return;
    }
    if (R(2))
        gmemcpy(limits, R(2), 8);
    else
        qrect_put(limits, (qrect){ 64, 64, 32767, 32767 });
    int32_t size = tb_GrowWindow(w, R(1), limits);
    tb_DisposePtr(limits);
    if (size)
        tb_SizeWindow(w, (int16_t)(size & 0xFFFF), (int16_t)(size >> 16), 1);
    if (R(3))
        qrect_put(R(3), global_port_rect(w));
    RETURN(size != 0);
}

/* GetAvailableWindowPositioningBounds(device, availableRect) */
static void wm_GetAvailableWindowPositioningBounds(ppc_cpu *c)
{
    if (!R(0) || !rd32(R(0)) || !R(1)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    qrect_put(R(1), available_bounds(R(0)));
    RETURN(noErr);
}

/* GetWindowGreatestAreaDevice(window, region, outGreatestDevice,
 * outGreatestDeviceRect) */
static void wm_GetWindowGreatestAreaDevice(ppc_cpu *c)
{
    gaddr w = R(0);
    if (!valid(w) || !R(2)) {
        RETURN((uint32_t)(valid(w) ? paramErr : errInvalidWindowRef));
        return;
    }
    int16_t err = noErr;
    qrect r = region_bounds(w, (uint16_t)R(1), &err);
    if (err) {
        RETURN((uint32_t)(int32_t)err);
        return;
    }
    gaddr gd = greatest_device(r);
    wr32(R(2), gd);
    if (R(3))
        gmemcpy(R(3), rd32(gd) + GDevice_gdRect_off, 8);
    RETURN(noErr);
}

/* ConstrainWindowToScreen(window, regionCode, options, screenRect,
 * outStructure): moved (and, when it may, made smaller) to be on the
 * screen; with kWindowConstrainAllowPartial only the title bar need be. */
enum { kWindowConstrainMayResize = 1, kWindowConstrainMoveRegardlessOfFit = 2, kWindowConstrainAllowPartial = 4,
       kWindowConstrainCalcOnly = 8 };

static qrect constrain(gaddr w, qrect s, qrect area, uint32_t options)
{
    int width = s.right - s.left, height = s.bottom - s.top;
    if (options & kWindowConstrainMayResize) {
        if (width > area.right - area.left)
            width = area.right - area.left;
        if (height > area.bottom - area.top)
            height = area.bottom - area.top;
    }
    bool fits = width <= area.right - area.left && height <= area.bottom - area.top;
    int left = s.left, top = s.top;
    if (options & kWindowConstrainAllowPartial) {
        /* the title bar's top on the screen, and some of its width */
        int bar = frame_widths(w).top > 0 ? frame_widths(w).top : 20;
        if (top < area.top)
            top = area.top;
        if (top > area.bottom - bar)
            top = area.bottom - bar;
        if (left + width < area.left + 20)
            left = area.left + 20 - width;
        if (left > area.right - 20)
            left = area.right - 20;
    } else if (fits || (options & kWindowConstrainMoveRegardlessOfFit)) {
        if (left + width > area.right)
            left = area.right - width;
        if (left < area.left)
            left = area.left;
        if (top + height > area.bottom)
            top = area.bottom - height;
        if (top < area.top)
            top = area.top;
    }
    return (qrect){ (int16_t)top, (int16_t)left, (int16_t)(top + height), (int16_t)(left + width) };
}

static void wm_ConstrainWindowToScreen(ppc_cpu *c)
{
    gaddr w = R(0);
    uint32_t options = R(2);
    if (!valid(w)) {
        RETURN((uint32_t)errInvalidWindowRef);
        return;
    }
    qrect s = structure_of(w);
    qrect area = R(3) ? qrect_get(R(3)) : available_bounds(greatest_device(s));
    qrect to = constrain(w, s, area, options);
    if (!(options & kWindowConstrainCalcOnly) && !same_rect(to, s))
        set_structure(w, to);
    if (R(4))
        qrect_put(R(4), (options & kWindowConstrainCalcOnly) ? to : structure_of(w));
    RETURN(noErr);
}

/* RepositionWindow(window, parentWindow, method): centred, cascaded or
 * placed as an alert is, on the main screen or the parent's, or over the
 * parent. */
enum { kWindowCenterOnMainScreen = 1, kWindowCenterOnParentWindow, kWindowCenterOnParentWindowScreen,
       kWindowCascadeOnMainScreen, kWindowCascadeOnParentWindow, kWindowCascadeOnParentWindowScreen,
       kWindowAlertPositionOnMainScreen, kWindowAlertPositionOnParentWindow,
       kWindowAlertPositionOnParentWindowScreen };

static void wm_RepositionWindow(ppc_cpu *c)
{
    gaddr w = R(0), parent = R(1);
    uint32_t method = R(2);
    if (!valid(w)) {
        RETURN((uint32_t)errInvalidWindowRef);
        return;
    }
    bool on_parent = method == kWindowCenterOnParentWindow || method == kWindowCascadeOnParentWindow ||
                     method == kWindowAlertPositionOnParentWindow;
    bool parent_screen = method == kWindowCenterOnParentWindowScreen || method == kWindowCascadeOnParentWindowScreen ||
                         method == kWindowAlertPositionOnParentWindowScreen;
    if ((on_parent || parent_screen) && !valid(parent)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    if (method < kWindowCenterOnMainScreen || method > kWindowAlertPositionOnParentWindowScreen) {
        RETURN((uint32_t)paramErr);
        return;
    }
    qrect s = structure_of(w);
    int width = s.right - s.left, height = s.bottom - s.top;
    qrect screen = available_bounds(parent_screen ? greatest_device(structure_of(parent)) : rd32(LM_MainDevice));
    qrect area = on_parent ? structure_of(parent) : screen;
    int left, top;
    if (method == kWindowCascadeOnMainScreen || method == kWindowCascadeOnParentWindow ||
        method == kWindowCascadeOnParentWindowScreen) {
        if (on_parent) {
            left = area.left + 20;
            top = area.top + 20;
        } else {
            /* stepped down and right for each other window of its class showing */
            int n = 0;
            uint32_t cls = class_of(w);
            for (gaddr o = rd32(LM_WindowList); o; o = WindowRecord_nextWindow(o))
                if (o != w && WindowRecord_visible(o) && class_of(o) == cls)
                    n++;
            left = area.left + 10 + 20 * (n % 10);
            top = area.top + 10 + 20 * (n % 10);
        }
    } else {
        left = area.left + (area.right - area.left - width) / 2;
        if (method == kWindowCenterOnMainScreen || method == kWindowCenterOnParentWindow ||
            method == kWindowCenterOnParentWindowScreen)
            top = area.top + (area.bottom - area.top - height) / 2;
        else
            top = area.top + (area.bottom - area.top - height) / 3; /* a third of the room above, as alerts */
    }
    qrect to = { (int16_t)top, (int16_t)left, (int16_t)(top + height), (int16_t)(left + width) };
    to = constrain(w, to, screen, kWindowConstrainMoveRegardlessOfFit);
    set_structure(w, to);
    RETURN(noErr);
}

/* TransitionWindow(window, effect, action, rect): shown or hidden (the
 * zoom rectangles aren't drawn), or moved or resized to the rectangle
 * (CarbonLib 1.5's actions). */
enum { kWindowShowTransitionAction = 1, kWindowHideTransitionAction, kWindowMoveTransitionAction,
       kWindowResizeTransitionAction };

static int16_t transition(gaddr w, uint32_t action, gaddr rect)
{
    if (!valid(w))
        return errInvalidWindowRef;
    switch (action) {
    case kWindowShowTransitionAction: tb_ShowWindow(w); return noErr;
    case kWindowHideTransitionAction: tb_HideWindow(w); return noErr;
    case kWindowMoveTransitionAction:
    case kWindowResizeTransitionAction:
        if (!rect)
            return paramErr;
        set_structure(w, qrect_get(rect));
        return noErr;
    default: return paramErr;
    }
}

static void wm_TransitionWindow(ppc_cpu *c) { RETURN((uint32_t)(int32_t)transition(R(0), R(2), R(3))); }
/* TransitionWindowAndParent(window, parentWindow, effect, action, rect) */
static void wm_TransitionWindowAndParent(ppc_cpu *c) { RETURN((uint32_t)(int32_t)transition(R(0), R(3), R(4))); }

/* ---- Classes and attributes ---- */

/* ChangeWindowAttributes(window, setThese, clearThese): the close box,
 * zoom box and grow box by the definition's variation, the rest kept. */
static void wm_ChangeWindowAttributes(ppc_cpu *c)
{
    gaddr w = R(0);
    uint32_t set = R(1), clear = R(2);
    if (!valid(w)) {
        RETURN((uint32_t)errInvalidWindowRef);
        return;
    }
    uint32_t cls = class_of(w), now = attributes_of(w);
    uint32_t want = (now | set) & ~clear;
    win_extra *e = extra_of(w, true);
    if (!e) {
        RETURN((uint32_t)memFullErr);
        return;
    }
    if ((want ^ now) & FRAME_ATTRIBUTES & ~(uint32_t)kWindowCloseBoxAttribute) {
        int16_t proc = wm_class_proc(cls, want);
        uint32_t frame = want & (kWindowHorizontalZoomAttribute | kWindowVerticalZoomAttribute |
                                 kWindowResizableAttribute | kWindowSideTitlebarAttribute);
        /* only document and floating windows have the boxes */
        if (proc < 0 || (frame && cls != kDocumentWindowClass && cls != kFloatingWindowClass &&
                         cls != kUtilityWindowClass)) {
            RETURN((uint32_t)windowAttributeImmutableErr);
            return;
        }
        wm_redefine(w, 0, (int16_t)(proc & 15));
    }
    if ((want ^ now) & kWindowCloseBoxAttribute) {
        WindowRecord_set_goAwayFlag(w, (want & kWindowCloseBoxAttribute) ? 1 : 0);
        wm_redefine(w, 0, -1);
    }
    e->attributes = want;
    if (!e->cls)
        e->cls = cls;
    RETURN(noErr);
}

/* The definition for a procID: one RegisterWindowDefinition registered,
 * else the WDEF resource. */
static gaddr definition_for(int16_t procid)
{
    gaddr def = carbonwm_registered_def((int16_t)(procid >> 4));
    if (!def)
        def = tb_GetResource((int32_t)0x57444546, (int16_t)(procid >> 4)); /* 'WDEF' */
    return def;
}

/* SetWindowClass(window, class): the class's definition and variation,
 * and its kind (dialogs' for the modal classes). */
static void wm_SetWindowClass(ppc_cpu *c)
{
    gaddr w = R(0);
    uint32_t cls = R(1);
    if (!valid(w)) {
        RETURN((uint32_t)errInvalidWindowRef);
        return;
    }
    uint32_t now = class_of(w);
    if (cls == now) {
        RETURN(noErr);
        return;
    }
    int16_t proc = wm_class_proc(cls, attributes_of(w));
    if (proc < 0) {
        RETURN((uint32_t)errUnrecognizedWindowClass);
        return;
    }
    gaddr def = definition_for(proc);
    if (!def) {
        RETURN((uint32_t)resNotFound);
        return;
    }
    wm_redefine(w, def != WindowRecord_windowDefProc(w) ? def : 0, (int16_t)(proc & 15));
    if (WindowRecord_windowKind(w) == userKind || WindowRecord_windowKind(w) == dialogKind)
        WindowRecord_set_windowKind(w, cls == kDocumentWindowClass || cls == kFloatingWindowClass ||
                                               cls == kUtilityWindowClass
                                           ? userKind
                                           : dialogKind);
    win_extra *e = extra_of(w, true);
    if (e) {
        e->cls = cls;
        e->modality = default_modality(cls);
    }
    RETURN(noErr);
}

/* IsValidWindowClass (CarbonLib 1.5) and GetAvailableWindowAttributes:
 * the attributes a class can have. */
static void wm_IsValidWindowClass(ppc_cpu *c) { RETURN(wm_class_proc(R(0), 0) >= 0); }

static void wm_GetAvailableWindowAttributes(ppc_cpu *c)
{
    switch (R(0)) {
    case kDocumentWindowClass:
        RETURN(kWindowCloseBoxAttribute | kWindowHorizontalZoomAttribute | kWindowVerticalZoomAttribute |
               kWindowCollapseBoxAttribute | kWindowResizableAttribute);
        break;
    case kFloatingWindowClass:
    case kUtilityWindowClass:
        RETURN(kWindowCloseBoxAttribute | kWindowHorizontalZoomAttribute | kWindowVerticalZoomAttribute |
               kWindowCollapseBoxAttribute | kWindowResizableAttribute | kWindowSideTitlebarAttribute);
        break;
    default: RETURN(0); break;
    }
}

/* GetWindowModality(window, outModalKind, outUnavailableWindow) and
 * SetWindowModality(window, modalKind, unavailableWindow) */
static void wm_GetWindowModality(ppc_cpu *c)
{
    gaddr w = R(0);
    if (!valid(w)) {
        RETURN((uint32_t)errInvalidWindowRef);
        return;
    }
    win_extra *e = extra_of(w, false);
    if (R(1))
        wr32(R(1), e ? e->modality : default_modality(class_of(w)));
    if (R(2))
        wr32(R(2), e ? e->unavailable : 0);
    RETURN(noErr);
}

static void wm_SetWindowModality(ppc_cpu *c)
{
    gaddr w = R(0);
    win_extra *e = valid(w) ? extra_of(w, true) : NULL;
    if (!e) {
        RETURN((uint32_t)errInvalidWindowRef);
        return;
    }
    if (R(1) > 3) {
        RETURN((uint32_t)paramErr);
        return;
    }
    if (!e->cls)
        e->cls = class_of(w);
    e->modality = R(1);
    e->unavailable = R(2);
    RETURN(noErr);
}

/* ---- Retain counts ---- */

static void wm_RetainWindow(ppc_cpu *c)
{
    win_extra *e = valid(R(0)) ? extra_of(R(0), true) : NULL;
    if (!e) {
        RETURN((uint32_t)errInvalidWindowRef);
        return;
    }
    e->retain++;
    RETURN(noErr);
}

/* ReleaseWindow: the last release disposes of the window. */
static void wm_ReleaseWindow(ppc_cpu *c)
{
    gaddr w = R(0);
    win_extra *e = valid(w) ? extra_of(w, true) : NULL;
    if (!e) {
        RETURN((uint32_t)errInvalidWindowRef);
        return;
    }
    if (--e->retain == 0)
        tb_DisposeWindow(w);
    RETURN(noErr);
}

static void wm_GetWindowRetainCount(ppc_cpu *c)
{
    win_extra *e = valid(R(0)) ? extra_of(R(0), false) : NULL;
    RETURN(e ? e->retain : valid(R(0)) ? 1 : 0);
}

static void wm_GetWindowOwnerCount(ppc_cpu *c)
{
    gaddr w = R(0);
    if (!valid(w)) {
        RETURN((uint32_t)errInvalidWindowRef);
        return;
    }
    win_extra *e = extra_of(w, false);
    if (R(1))
        wr32(R(1), e ? e->retain : 1);
    RETURN(noErr);
}

/* ---- Property attributes ---- */

static void wm_GetWindowPropertyAttributes(ppc_cpu *c)
{
    gaddr w = R(0);
    gaddr size = tb_NewPtr(4, false, true);
    int16_t err = valid(w) ? tb_GetWindowPropertySize(w, R(1), R(2), size) : errInvalidWindowRef;
    tb_DisposePtr(size);
    if (err) {
        RETURN((uint32_t)(int32_t)err);
        return;
    }
    uint32_t a = 0;
    for (int i = 0; i < npattrs; i++)
        if (pattrs[i].w == w && pattrs[i].creator == R(1) && pattrs[i].tag == R(2))
            a = pattrs[i].attributes;
    if (R(3))
        wr32(R(3), a);
    RETURN(noErr);
}

static void wm_ChangeWindowPropertyAttributes(ppc_cpu *c)
{
    gaddr w = R(0);
    gaddr size = tb_NewPtr(4, false, true);
    int16_t err = valid(w) ? tb_GetWindowPropertySize(w, R(1), R(2), size) : errInvalidWindowRef;
    tb_DisposePtr(size);
    if (err) {
        RETURN((uint32_t)(int32_t)err);
        return;
    }
    prop_attr *p = NULL;
    for (int i = 0; i < npattrs && !p; i++)
        if (pattrs[i].w == w && pattrs[i].creator == R(1) && pattrs[i].tag == R(2))
            p = &pattrs[i];
    if (!p) {
        if (npattrs == maxpattrs) {
            int n = maxpattrs ? maxpattrs * 2 : 16;
            prop_attr *more = realloc(pattrs, sizeof *pattrs * (size_t)n);
            if (!more) {
                RETURN((uint32_t)memFullErr);
                return;
            }
            pattrs = more;
            maxpattrs = n;
        }
        p = &pattrs[npattrs++];
        *p = (prop_attr){ w, R(1), R(2), 0 };
    }
    p->attributes = (p->attributes | R(3)) & ~R(4);
    RETURN(noErr);
}

/* ---- Proxies and paths: there are no proxy icons here ---- */

static void wm_no_proxy(ppc_cpu *c) { RETURN((uint32_t)(valid(R(0)) ? errWindowDoesNotHaveProxy : errInvalidWindowRef)); }
static void wm_false(ppc_cpu *c) { RETURN(0); }

/* GetWindowWidgetHilite(window, outHilite): no part of the frame is
 * pressed outside the Window Manager's own tracking. */
static void wm_GetWindowWidgetHilite(ppc_cpu *c)
{
    if (!valid(R(0))) {
        RETURN((uint32_t)errInvalidWindowRef);
        return;
    }
    if (R(1))
        wr16(R(1), 0); /* kWindowNoPart */
    RETURN(noErr);
}

/* HiliteWindowFrameForDrag(window, hilited): the Drag Manager's two-pixel
 * frame just inside the content, drawn or taken away. */
int32_t tb_HiliteWindowFrameForDrag(gaddr w, int8_t hilited)
{
    win_extra *e = valid(w) ? extra_of(w, true) : NULL;
    if (!e)
        return errInvalidWindowRef;
    if (e->drag_hilited == (hilited != 0))
        return noErr;
    e->drag_hilited = hilited != 0;
    hrgn outer, inner;
    hrgn_init(&outer);
    hrgn_init(&inner);
    hrgn_rect(&outer, qrect_get(w + GrafPort_portRect_off));
    hrgn_copy(&inner, &outer);
    hrgn_inset(&inner, 2, 2);
    hrgn_op(&outer, &outer, &inner, RGN_DIFF);
    gaddr saved = qd_port();
    tb_SetPort(w);
    static const uint8_t black[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    qd_fill_region(w, &outer, black, 10 /* patXor */);
    tb_SetPort(saved);
    hrgn_free(&outer);
    hrgn_free(&inner);
    return noErr;
}

/* ---- Content ---- */

/* GetWindowContentPattern / SetWindowContentPattern: the window port's
 * background pattern. */
static void wm_SetWindowContentPattern(ppc_cpu *c)
{
    gaddr w = R(0);
    if (!valid(w) || !R(1)) {
        RETURN((uint32_t)(valid(w) ? paramErr : errInvalidWindowRef));
        return;
    }
    gaddr saved = qd_port();
    tb_SetPort(w);
    if (qd_port_is_color(w))
        tb_BackPixPat(R(1));
    else if (rd32(R(1)))
        tb_BackPat(rd32(R(1)) + PixPat_pat1Data_off);
    tb_SetPort(saved);
    RETURN(noErr);
}

static void wm_GetWindowContentPattern(ppc_cpu *c)
{
    gaddr w = R(0);
    if (!valid(w) || !R(1)) {
        RETURN((uint32_t)(valid(w) ? paramErr : errInvalidWindowRef));
        return;
    }
    gaddr pp = qd_port_is_color(w) ? rd32(w + CGrafPort_bkPixPat_off) : 0;
    if (pp && rd32(pp)) {
        tb_CopyPixPat(pp, R(1));
    } else {
        uint8_t pat[8];
        gget(pat, w + GrafPort_bkPat_off, 8);
        gaddr tmp = qd_pixpat_for(pat);
        if (tmp) {
            tb_CopyPixPat(tmp, R(1));
            tb_DisposeHandle(tmp);
        }
    }
    RETURN(noErr);
}

/* InstallWindowContentPaintProc(window, paintProc, options, refCon): the
 * program paints uncovered content itself (in place of the background
 * the Window Manager erases it to); what it leaves in outSystemPaintRgn
 * the Window Manager erases. */
enum { uppWindowPaintProcInfo = 0x0003FFF0 };

static void wm_InstallWindowContentPaintProc(ppc_cpu *c)
{
    gaddr w = R(0);
    win_extra *e = valid(w) ? extra_of(w, true) : NULL;
    if (!e) {
        RETURN((uint32_t)errInvalidWindowRef);
        return;
    }
    e->paint_proc = R(1);
    e->paint_refcon = R(3);
    RETURN(noErr);
}

bool carbonwm_paint_content(gaddr w, const hrgn *content)
{
    win_extra *e = extra_of(w, false);
    if (!e || !e->paint_proc)
        return false;
    int dh, dv;
    port_offset(w, &dh, &dv);
    hrgn local;
    hrgn_init(&local);
    hrgn_copy(&local, content);
    hrgn_offset(&local, dh, dv);
    gaddr client = tb_NewRgn(), system = tb_NewRgn();
    rgn_write(client, &local);
    gaddr saved = qd_port();
    tb_SetPort(w);
    uint32_t args[6] = { rd32(LM_MainDevice), w, w, client, system, e->paint_refcon };
    int32_t err = (int32_t)mm_call(e->paint_proc, uppWindowPaintProcInfo, args, 6);
    tb_SetPort(w);
    /* the system's share (all of it, if the procedure failed) erased */
    gaddr erase = err ? client : system;
    if (!tb_EmptyRgn(erase))
        tb_EraseRgn(erase);
    tb_SetPort(saved);
    tb_DisposeRgn(client);
    tb_DisposeRgn(system);
    hrgn_free(&local);
    return true;
}

/* ScrollWindowRect(window, scrollRect, dh, dv, options, exposedRgn) and
 * ScrollWindowRegion: ScrollRect in the window's port; the uncovered area
 * erased (ScrollRect always does) and, when asked, added to the update
 * region. */
enum { kScrollWindowInvalidate = 1 };

static int16_t scroll_window(gaddr w, gaddr rect, gaddr area_rgn, int16_t dh, int16_t dv, uint32_t options,
                             gaddr exposed)
{
    if (!valid(w))
        return errInvalidWindowRef;
    gaddr saved = qd_port();
    tb_SetPort(w);
    gaddr clip = 0, r = rect;
    if (area_rgn) {
        /* the region's bounds scrolled, inside the region alone */
        clip = tb_NewRgn();
        tb_GetClip(clip);
        gaddr inside = tb_NewRgn();
        tb_SectRgn(clip, area_rgn, inside);
        tb_SetClip(inside);
        tb_DisposeRgn(inside);
        r = tb_NewPtr(8, false, true);
        gmemcpy(r, rd32(area_rgn) + 2, 8);
    }
    gaddr upd = tb_NewRgn();
    tb_ScrollRect(r, dh, dv, upd);
    if (area_rgn)
        tb_SectRgn(upd, area_rgn, upd);
    if (options & kScrollWindowInvalidate)
        tb_InvalRgn(upd);
    if (exposed)
        tb_CopyRgn(upd, exposed);
    tb_DisposeRgn(upd);
    if (clip) {
        tb_SetClip(clip);
        tb_DisposeRgn(clip);
        tb_DisposePtr(r);
    }
    tb_SetPort(saved);
    return noErr;
}

static void wm_ScrollWindowRect(ppc_cpu *c)
{
    if (!R(1)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    RETURN((uint32_t)(int32_t)scroll_window(R(0), R(1), 0, (int16_t)R(2), (int16_t)R(3), R(4), R(5)));
}

static void wm_ScrollWindowRegion(ppc_cpu *c)
{
    if (!R(1) || !rd32(R(1))) {
        RETURN((uint32_t)paramErr);
        return;
    }
    RETURN((uint32_t)(int32_t)scroll_window(R(0), 0, R(1), (int16_t)R(2), (int16_t)R(3), R(4), R(5)));
}

/* DisableScreenUpdates / EnableScreenUpdates: Mac OS X's; on Mac OS 9
 * drawing reaches the screen as it is done. */
static void wm_noErr(ppc_cpu *c) { RETURN(noErr); }

/* ---- Finding windows by class ---- */

static bool of_class(gaddr w, uint32_t cls) { return cls == kAllWindowClasses || class_of(w) == cls; }

/* FindWindowOfClass(where, class, outWindow, outWindowPart): the frontmost
 * window of the class under the point, and the part it's in. */
static void wm_FindWindowOfClass(ppc_cpu *c)
{
    uint32_t pt = R(0) ? rd32(R(0)) : 0;
    for (gaddr w = rd32(LM_WindowList); w; w = WindowRecord_nextWindow(w)) {
        if (!WindowRecord_visible(w) || !of_class(w, R(1)))
            continue;
        int16_t part = wm_hit_part(w, pt);
        if (part == inDesk)
            continue;
        if (R(2))
            wr32(R(2), w);
        if (R(3))
            wr16(R(3), (uint16_t)part);
        RETURN(noErr);
        return;
    }
    if (R(2))
        wr32(R(2), 0);
    RETURN((uint32_t)errWindowNotFound);
}

/* GetFrontWindowOfClass(class, mustBeVisible) */
static void wm_GetFrontWindowOfClass(ppc_cpu *c)
{
    for (gaddr w = rd32(LM_WindowList); w; w = WindowRecord_nextWindow(w))
        if ((!(R(1) & 0xFF) || WindowRecord_visible(w)) && of_class(w, R(0))) {
            RETURN(w);
            return;
        }
    RETURN(0);
}

/* GetNextWindowOfClass(window, class, mustBeVisible) */
static void wm_GetNextWindowOfClass(ppc_cpu *c)
{
    for (gaddr w = R(0) ? WindowRecord_nextWindow(R(0)) : 0; w; w = WindowRecord_nextWindow(w))
        if ((!(R(2) & 0xFF) || WindowRecord_visible(w)) && of_class(w, R(1))) {
            RETURN(w);
            return;
        }
    RETURN(0);
}

/* GetWindowIndex(window, startWindow, class, mustBeVisible, outIndex) and
 * GetIndexedWindow(startWindow, index, class, mustBeVisible, outWindow)
 * (CarbonLib 1.4): windows counted from the start one (or the front), by
 * class. */
static void wm_GetIndexedWindow(ppc_cpu *c)
{
    uint32_t n = 0;
    for (gaddr w = R(0) ? R(0) : rd32(LM_WindowList); w; w = WindowRecord_nextWindow(w))
        if ((!(R(3) & 0xFF) || WindowRecord_visible(w)) && of_class(w, R(2)) && ++n == R(1)) {
            if (R(4))
                wr32(R(4), w);
            RETURN(noErr);
            return;
        }
    RETURN((uint32_t)errWindowNotFound);
}

static void wm_GetWindowIndex(ppc_cpu *c)
{
    uint32_t n = 0;
    for (gaddr w = R(1) ? R(1) : rd32(LM_WindowList); w; w = WindowRecord_nextWindow(w))
        if ((!(R(3) & 0xFF) || WindowRecord_visible(w)) && of_class(w, R(2))) {
            n++;
            if (w == R(0)) {
                if (R(4))
                    wr32(R(4), n);
                RETURN(noErr);
                return;
            }
        }
    RETURN((uint32_t)errWindowNotFound);
}

/* ---- Definitions ---- */

/* A definition procedure as a WDEF: a handle holding a routine
 * descriptor, which 68k code runs (its first word is Mixed Mode's
 * trap) as it runs a PowerPC WDEF resource. */
static gaddr def_handle(gaddr upp)
{
    if (!upp)
        return 0;
    uint32_t n = mm_is_descriptor(upp) ? 12 + 20 * (rd16(upp + 10) + 1u) : 0;
    if (!n)
        return 0;
    gaddr h = tb_NewHandle((int32_t)n, true, true);
    if (h)
        gmemcpy(rd32(h), upp, n);
    return h;
}

enum { kWindowDefProcPtr = 0, kWindowDefObjectClass = 1, kWindowDefProcID = 2 };

static struct {
    int16_t id;
    gaddr def;
} registered[16];

gaddr carbonwm_registered_def(int16_t id)
{
    for (int i = 0; i < 16; i++)
        if (registered[i].def && registered[i].id == id)
            return registered[i].def;
    return 0;
}

/* RegisterWindowDefinition(resID, defSpec): NewWindow and the rest find
 * this definition for the procIDs of that resource ID (a NULL spec
 * forgets it). */
static void wm_RegisterWindowDefinition(ppc_cpu *c)
{
    int16_t id = (int16_t)R(0);
    gaddr spec = R(1);
    int slot = -1;
    for (int i = 0; i < 16; i++)
        if (registered[i].def && registered[i].id == id)
            slot = i;
    if (!spec) {
        if (slot >= 0) {
            tb_DisposeHandle(registered[slot].def);
            registered[slot].def = 0;
        }
        RETURN(noErr);
        return;
    }
    if (rd32(spec) != kWindowDefProcPtr) {
        RETURN((uint32_t)paramErr);
        return;
    }
    gaddr h = def_handle(rd32(spec + 4));
    if (!h) {
        RETURN((uint32_t)paramErr);
        return;
    }
    for (int i = 0; i < 16 && slot < 0; i++)
        if (!registered[i].def)
            slot = i;
    if (slot < 0) {
        tb_DisposeHandle(h);
        RETURN((uint32_t)memFullErr);
        return;
    }
    if (registered[slot].def)
        tb_DisposeHandle(registered[slot].def);
    registered[slot].id = id;
    registered[slot].def = h;
    RETURN(noErr);
}

/* CreateCustomWindow(defSpec, class, attributes, contentBounds,
 * outWindow): CreateNewWindow's window, given the program's definition
 * procedure or the procID asked for.  (Event-based definitions are
 * Carbon events' toolbox objects, which aren't here.) */
static void wm_CreateCustomWindow(ppc_cpu *c)
{
    gaddr spec = R(0), out = R(4);
    if (!spec || !out || !R(3)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    uint32_t kind = rd32(spec);
    gaddr def = 0;
    int16_t var = -1;
    if (kind == kWindowDefProcPtr) {
        def = def_handle(rd32(spec + 4));
        var = 0;
    } else if (kind == kWindowDefProcID) {
        int16_t proc = (int16_t)rd16(spec + 4);
        def = definition_for(proc);
        var = (int16_t)(proc & 15);
    }
    if (!def) {
        RETURN((uint32_t)paramErr);
        return;
    }
    int16_t err = tb_CreateNewWindow(R(1), R(2), R(3), out);
    if (err) {
        if (kind == kWindowDefProcPtr)
            tb_DisposeHandle(def);
        RETURN((uint32_t)(int32_t)err);
        return;
    }
    gaddr w = rd32(out);
    wm_redefine(w, def != WindowRecord_windowDefProc(w) ? def : 0, var);
    RETURN(noErr);
}

/* ReshapeCustomWindow(window): its definition asked for its regions
 * again, and the screen redrawn. */
static void wm_ReshapeCustomWindow(ppc_cpu *c)
{
    if (!valid(R(0))) {
        RETURN((uint32_t)errInvalidWindowRef);
        return;
    }
    wm_redefine(R(0), 0, -1);
    RETURN(noErr);
}

/* ---- Collections ---- */

/* A window in a collection: its class, attributes, content bounds,
 * title, kind and reference constant, by tag (Blue Box's own items: the
 * layout of Apple's 'wind' resources, which CarbonLib's Interface Builder
 * flattened, was never published). */
enum { TAG_WINDOW = 0x77696E64 /* 'wind' */ };
enum { ITEM_CLASS = 1, ITEM_ATTRIBUTES, ITEM_BOUNDS, ITEM_TITLE, ITEM_KIND, ITEM_REFCON, ITEM_VISIBLE };

static void wm_StoreWindowIntoCollection(ppc_cpu *c)
{
    gaddr w = R(0), coll = R(1);
    if (!valid(w) || !coll) {
        RETURN((uint32_t)(valid(w) ? paramErr : errInvalidWindowRef));
        return;
    }
    gaddr buf = tb_NewPtr(256, false, true);
    if (!buf) {
        RETURN((uint32_t)memFullErr);
        return;
    }
    int16_t err = noErr;
    wr32(buf, class_of(w));
    err = err ? err : tb_AddCollectionItem(coll, TAG_WINDOW, ITEM_CLASS, 4, buf);
    wr32(buf, attributes_of(w));
    err = err ? err : tb_AddCollectionItem(coll, TAG_WINDOW, ITEM_ATTRIBUTES, 4, buf);
    qrect_put(buf, global_port_rect(w));
    err = err ? err : tb_AddCollectionItem(coll, TAG_WINDOW, ITEM_BOUNDS, 8, buf);
    tb_GetWTitle(w, buf);
    err = err ? err : tb_AddCollectionItem(coll, TAG_WINDOW, ITEM_TITLE, rd8(buf) + 1, buf);
    wr16(buf, (uint16_t)WindowRecord_windowKind(w));
    err = err ? err : tb_AddCollectionItem(coll, TAG_WINDOW, ITEM_KIND, 2, buf);
    wr32(buf, (uint32_t)WindowRecord_refCon(w));
    err = err ? err : tb_AddCollectionItem(coll, TAG_WINDOW, ITEM_REFCON, 4, buf);
    wr8(buf, WindowRecord_visible(w));
    err = err ? err : tb_AddCollectionItem(coll, TAG_WINDOW, ITEM_VISIBLE, 1, buf);
    tb_DisposePtr(buf);
    RETURN((uint32_t)(int32_t)err);
}

static bool collection_item(gaddr coll, int32_t id, gaddr buf, uint32_t max)
{
    gaddr size = tb_NewPtr(4, false, true);
    bool ok = size && tb_GetCollectionItem(coll, TAG_WINDOW, id, size, 0) == noErr && rd32(size) <= max;
    if (ok) {
        wr32(size, max);
        ok = tb_GetCollectionItem(coll, TAG_WINDOW, id, size, buf) == noErr;
    }
    if (size)
        tb_DisposePtr(size);
    return ok;
}

static int16_t window_from_collection(gaddr coll, gaddr out)
{
    gaddr buf = tb_NewPtr(256, true, true);
    if (!buf)
        return memFullErr;
    int16_t err = errUnrecognizedWindowClass;
    if (collection_item(coll, ITEM_CLASS, buf, 4)) {
        uint32_t cls = rd32(buf), attrs = collection_item(coll, ITEM_ATTRIBUTES, buf, 4) ? rd32(buf) : 0;
        if (!collection_item(coll, ITEM_BOUNDS, buf, 8))
            qrect_put(buf, (qrect){ 60, 40, 260, 340 });
        err = tb_CreateNewWindow(cls, attrs, buf, out);
        gaddr w = err ? 0 : rd32(out);
        if (w) {
            if (collection_item(coll, ITEM_TITLE, buf, 256))
                tb_SetWTitle(w, buf);
            if (collection_item(coll, ITEM_KIND, buf, 2))
                WindowRecord_set_windowKind(w, (int16_t)rd16(buf));
            if (collection_item(coll, ITEM_REFCON, buf, 4))
                WindowRecord_set_refCon(w, (int32_t)rd32(buf));
            if (collection_item(coll, ITEM_VISIBLE, buf, 1) && rd8(buf))
                tb_ShowWindow(w);
        }
    }
    tb_DisposePtr(buf);
    return err;
}

static void wm_CreateWindowFromCollection(ppc_cpu *c)
{
    if (!R(0) || !R(1)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    RETURN((uint32_t)(int32_t)window_from_collection(R(0), R(1)));
}

/* CreateWindowFromResource(resID, outWindow): a 'wind' resource, which is
 * a flattened collection in a layout Apple kept to itself. */
static void wm_CreateWindowFromResource(ppc_cpu *c)
{
    gaddr h = tb_GetResource((int32_t)TAG_WINDOW, (int16_t)R(0));
    if (R(1))
        wr32(R(1), 0);
    RETURN((uint32_t)(int32_t)(h ? unimpErr : resNotFound));
}

/* ---- Titles as CFStrings ---- */

static void wm_SetWindowTitleWithCFString(ppc_cpu *c)
{
    gaddr w = R(0);
    if (!valid(w)) {
        RETURN((uint32_t)errInvalidWindowRef);
        return;
    }
    gaddr p = tb_NewPtr(256, true, true);
    if (!p) {
        RETURN((uint32_t)memFullErr);
        return;
    }
    hi_cf_to_pstr(R(1), p);
    tb_SetWTitle(w, p);
    tb_DisposePtr(p);
    RETURN(noErr);
}

static void wm_CopyWindowTitleAsCFString(ppc_cpu *c)
{
    gaddr w = R(0);
    if (!valid(w) || !R(1)) {
        RETURN((uint32_t)(valid(w) ? paramErr : errInvalidWindowRef));
        return;
    }
    gaddr p = tb_NewPtr(256, true, true);
    tb_GetWTitle(w, p);
    gaddr s = hi_cf_from_pstr(p);
    tb_DisposePtr(p);
    wr32(R(1), s);
    RETURN(s ? noErr : (uint32_t)memFullErr);
}

/* The alternate title (the one Mac OS X's Window menu shows), kept. */
static void wm_SetWindowAlternateTitle(ppc_cpu *c)
{
    win_extra *e = valid(R(0)) ? extra_of(R(0), true) : NULL;
    if (!e) {
        RETURN((uint32_t)errInvalidWindowRef);
        return;
    }
    if (R(1))
        hi_cf_retain(R(1));
    if (e->alternate_title)
        hi_cf_release(e->alternate_title);
    e->alternate_title = R(1);
    RETURN(noErr);
}

static void wm_CopyWindowAlternateTitle(ppc_cpu *c)
{
    win_extra *e = valid(R(0)) ? extra_of(R(0), false) : NULL;
    if (!valid(R(0)) || !R(1)) {
        RETURN((uint32_t)(valid(R(0)) ? paramErr : errInvalidWindowRef));
        return;
    }
    gaddr s = e ? e->alternate_title : 0;
    if (s)
        hi_cf_retain(s);
    wr32(R(1), s);
    RETURN(noErr);
}

/* ---- Sheets: app-modal, centred over the parent near its top ---- */

static void wm_ShowSheetWindow(ppc_cpu *c)
{
    gaddr sheet = R(0), parent = R(1);
    win_extra *e = valid(sheet) && valid(parent) ? extra_of(sheet, true) : NULL;
    if (!e) {
        RETURN((uint32_t)errInvalidWindowRef);
        return;
    }
    uint32_t cls = class_of(sheet);
    if (cls != kSheetWindowClass && cls != kSheetAlertWindowClass) {
        RETURN((uint32_t)errUnrecognizedWindowClass);
        return;
    }
    e->sheet_parent = parent;
    e->modality = kWindowModalityAppModal;
    qrect s = structure_of(sheet), p = content_of(parent);
    int width = s.right - s.left, height = s.bottom - s.top;
    int left = p.left + (p.right - p.left - width) / 2, top = p.top;
    qrect to = constrain(sheet, (qrect){ (int16_t)top, (int16_t)left, (int16_t)(top + height), (int16_t)(left + width) },
                         available_bounds(greatest_device(p)), kWindowConstrainMoveRegardlessOfFit);
    set_structure(sheet, to);
    tb_ShowWindow(sheet);
    tb_SelectWindow(sheet);
    RETURN(noErr);
}

static void wm_HideSheetWindow(ppc_cpu *c)
{
    gaddr sheet = R(0);
    win_extra *e = valid(sheet) ? extra_of(sheet, false) : NULL;
    if (!e || !e->sheet_parent) {
        RETURN((uint32_t)(valid(sheet) ? paramErr : errInvalidWindowRef));
        return;
    }
    gaddr parent = e->sheet_parent;
    e->sheet_parent = 0;
    tb_HideWindow(sheet);
    if (valid(parent))
        tb_SelectWindow(parent);
    RETURN(noErr);
}

static void wm_GetSheetWindowParent(ppc_cpu *c)
{
    win_extra *e = valid(R(0)) ? extra_of(R(0), false) : NULL;
    if (!valid(R(0)) || !R(1)) {
        RETURN((uint32_t)(valid(R(0)) ? paramErr : errInvalidWindowRef));
        return;
    }
    wr32(R(1), e ? e->sheet_parent : 0);
    RETURN(noErr);
}

/* ---- The standard Window menu ---- */

/* CreateStandardWindowMenu(options, outMenu): "Window", with commands to
 * collapse and zoom the front window and bring all to the front (their
 * command IDs Mac OS X's). */
static void wm_CreateStandardWindowMenu(ppc_cpu *c)
{
    if (!R(1)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    gaddr title = tb_NewPtr(64, true, true);
    static const char *const items[] = { "Collapse Window/M", "Zoom Window", "-", "Bring All to Front" };
    static const uint32_t commands[] = { 0x6D696E69 /* 'mini' */, 0x7A6F6F6D /* 'zoom' */, 0, 0x62667274 /* 'bfrt' */ };
    pstr_from_c(title, "Window");
    gaddr m = tb_NewMenu(-32000 + 2, title);  /* a menu ID of the system's range */
    for (int i = 0; m && i < 4; i++) {
        pstr_from_c(title, items[i]);
        tb_AppendMenu(m, title);
        if (commands[i])
            tb_SetMenuItemCommandID(m, (int16_t)(i + 1), commands[i]);
    }
    tb_DisposePtr(title);
    wr32(R(1), m);
    RETURN(m ? noErr : (uint32_t)memFullErr);
}

/* GetWindowStructurePort (CarbonLib 1.5): the port the frame is drawn in. */
static void wm_GetWindowStructurePort(ppc_cpu *c)
{
    RETURN(valid(R(0)) ? (rd32(LM_WMgrCPort) ? rd32(LM_WMgrCPort) : rd32(LM_WMgrPort)) : 0);
}

void carbonwm_reset(void)
{
    free(extras);
    extras = NULL;
    nextras = maxextras = 0;
    free(pattrs);
    pattrs = NULL;
    npattrs = maxpattrs = 0;
    memset(registered, 0, sizeof registered);
}

const ppc_export carbonwm_exports[] = {
    { "ActivateWindow", wm_ActivateWindow },
    { "ActiveNonFloatingWindow", wm_ActiveNonFloatingWindow },
    { "BeginWindowProxyDrag", wm_no_proxy },
    { "ChangeWindowAttributes", wm_ChangeWindowAttributes },
    { "ChangeWindowPropertyAttributes", wm_ChangeWindowPropertyAttributes },
    { "CloneWindow", wm_RetainWindow },
    { "ConstrainWindowToScreen", wm_ConstrainWindowToScreen },
    { "CopyWindowAlternateTitle", wm_CopyWindowAlternateTitle },
    { "CopyWindowTitleAsCFString", wm_CopyWindowTitleAsCFString },
    { "CreateCustomWindow", wm_CreateCustomWindow },
    { "CreateStandardWindowMenu", wm_CreateStandardWindowMenu },
    { "CreateWindowFromCollection", wm_CreateWindowFromCollection },
    { "CreateWindowFromResource", wm_CreateWindowFromResource },
    { "DisableScreenUpdates", wm_noErr },
    { "EnableScreenUpdates", wm_noErr },
    { "EndWindowProxyDrag", wm_no_proxy },
    { "FindWindowOfClass", wm_FindWindowOfClass },
    { "GetAvailableWindowAttributes", wm_GetAvailableWindowAttributes },
    { "GetAvailableWindowPositioningBounds", wm_GetAvailableWindowPositioningBounds },
    { "GetFrontWindowOfClass", wm_GetFrontWindowOfClass },
    { "GetIndexedWindow", wm_GetIndexedWindow },
    { "GetNextWindow", wm_GetNextWindow },
    { "GetNextWindowOfClass", wm_GetNextWindowOfClass },
    { "GetSheetWindowParent", wm_GetSheetWindowParent },
    { "GetWindowBounds", wm_GetWindowBounds },
    { "GetWindowContentPattern", wm_GetWindowContentPattern },
    { "GetWindowFromPort", wm_GetWindowFromPort },
    { "GetWindowGreatestAreaDevice", wm_GetWindowGreatestAreaDevice },
    { "GetWindowIdealUserState", wm_GetWindowIdealUserState },
    { "GetWindowIndex", wm_GetWindowIndex },
    { "GetWindowKind", wm_GetWindowKind },
    { "GetWindowList", wm_GetWindowList },
    { "GetWindowModality", wm_GetWindowModality },
    { "GetWindowOwnerCount", wm_GetWindowOwnerCount },
    { "GetWindowPort", wm_GetWindowPort },
    { "GetWindowPortBounds", wm_GetWindowPortBounds },
    { "GetWindowPropertyAttributes", wm_GetWindowPropertyAttributes },
    { "GetWindowRetainCount", wm_GetWindowRetainCount },
    { "GetWindowStandardState", wm_GetWindowStandardState },
    { "GetWindowStructurePort", wm_GetWindowStructurePort },
    { "GetWindowStructureWidths", wm_GetWindowStructureWidths },
    { "GetWindowUserState", wm_GetWindowUserState },
    { "GetWindowWidgetHilite", wm_GetWindowWidgetHilite },
    { "HideSheetWindow", wm_HideSheetWindow },
    { "InstallWindowContentPaintProc", wm_InstallWindowContentPaintProc },
    { "InvalWindowRect", wm_InvalWindowRect },
    { "InvalWindowRgn", wm_InvalWindowRgn },
    { "IsValidWindowClass", wm_IsValidWindowClass },
    { "IsWindowActive", wm_IsWindowActive },
    { "IsWindowHilited", wm_IsWindowHilited },
    { "IsWindowInStandardState", wm_IsWindowInStandardState },
    { "IsWindowPathSelectClick", wm_false },
    { "IsWindowPathSelectEvent", wm_false },
    { "IsWindowUpdatePending", wm_IsWindowUpdatePending },
    { "IsWindowVisible", wm_IsWindowVisible },
    { "MoveWindowStructure", wm_MoveWindowStructure },
    { "RegisterWindowDefinition", wm_RegisterWindowDefinition },
    { "ReleaseWindow", wm_ReleaseWindow },
    { "RepositionWindow", wm_RepositionWindow },
    { "ReshapeCustomWindow", wm_ReshapeCustomWindow },
    { "ResizeWindow", wm_ResizeWindow },
    { "RetainWindow", wm_RetainWindow },
    { "ScrollWindowRect", wm_ScrollWindowRect },
    { "ScrollWindowRegion", wm_ScrollWindowRegion },
    { "SetPortWindowPort", wm_SetPortWindowPort },
    { "SetWindowAlternateTitle", wm_SetWindowAlternateTitle },
    { "SetWindowBounds", wm_SetWindowBounds },
    { "SetWindowClass", wm_SetWindowClass },
    { "SetWindowContentPattern", wm_SetWindowContentPattern },
    { "SetWindowIdealUserState", wm_SetWindowIdealUserState },
    { "SetWindowKind", wm_SetWindowKind },
    { "SetWindowModality", wm_SetWindowModality },
    { "SetWindowStandardState", wm_SetWindowStandardState },
    { "SetWindowTitleWithCFString", wm_SetWindowTitleWithCFString },
    { "SetWindowUserState", wm_SetWindowUserState },
    { "ShowSheetWindow", wm_ShowSheetWindow },
    { "StoreWindowIntoCollection", wm_StoreWindowIntoCollection },
    { "TrackWindowProxyDrag", wm_no_proxy },
    { "TrackWindowProxyFromExistingDrag", wm_no_proxy },
    { "TransitionWindow", wm_TransitionWindow },
    { "TransitionWindowAndParent", wm_TransitionWindowAndParent },
    { "ValidWindowRect", wm_ValidWindowRect },
    { "ValidWindowRgn", wm_ValidWindowRgn },
    { "WindowPathSelect", wm_no_proxy },
    { "ZoomWindowIdeal", wm_ZoomWindowIdeal },
};
const unsigned carbonwm_export_count = sizeof carbonwm_exports / sizeof carbonwm_exports[0];
