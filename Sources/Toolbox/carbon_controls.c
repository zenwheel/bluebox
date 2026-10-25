/*
 * CarbonLib's Control Manager additions, over the Control Manager in
 * controls.c and the Appearance CDEFs: the accessors for what the control
 * record holds, ControlIDs and command IDs, properties, control regions,
 * titles as CFStrings, custom definitions (CreateCustomControl and
 * RegisterControlDefinition), the colour procedure, contextual menus,
 * cursors, click activation and drag tracking, and the universal
 * procedure pointers of Controls.h, ControlDefinitions.h and Dialogs.h.
 *
 * What a control has beyond its record and the Control Manager's entry
 * hangs on that entry (ctl_info's carbon): a carbon_ctl, freed with it.
 *
 * A UPP is a routine descriptor, as CarbonLib made them on Mac OS 8 and 9,
 * so the Toolbox calls it as it calls any procedure; InvokeXxxUPP calls it
 * through Mixed Mode with the procInfo the interfaces give.
 */
#include <stdlib.h>
#include <string.h>

#include "forks.h"
#include "ppc_bridge.h"
#include "qd_internal.h"
#include "tb_internal.h"
#include "trap.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; } while (0)
#define TAG(a, b, d, e) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(d) << 8 | (uint32_t)(e))

enum {
    errDataNotSupported = -30581, errUnknownControl = -30584,
    errInvalidPartCode = -30588, errDataSizeMismatch = -30591, errMessageNotSupported = -30580,
    controlPropertyInvalid = -5603, controlPropertyNotFoundErr = -5604, controlHandleInvalidErr = -30599,
};
enum {
    initCntl = 3, posCntl = 5, kControlMsgSetUpBackground = 23, kControlMsgApplyTextColor = 30,
    kControlMsgGetRegion = 31, kControlMsgSetCursor = 33, kControlMsgDragEnter = 38, kControlMsgDragLeave = 39,
    kControlMsgDragWithin = 40, kControlMsgDragReceive = 41, kControlMsgContextualMenuClick = 47,
    kControlMsgGetClickActivation = 48,
};
enum {
    kControlHasSpecialBackground = 1 << 7, kControlSupportsDragAndDrop = 1 << 12, kControlSupportsGetRegion = 1 << 17,
    kControlSupportsSetCursor = 1 << 20, kControlSupportsContextualMenus = 1 << 21,
    kControlSupportsClickActivation = 1 << 22,
};
enum { kControlStructureMetaPart = -1, kControlContentMetaPart = -2 };
enum { kActivateAndIgnoreClick = 2 };
enum { kDragTrackingEnterControl = 2, kDragTrackingInControl = 3, kDragTrackingLeaveControl = 4 };
enum { kControlPropertyPersistent = 1 };
enum { uppControlDefProcInfo = 0x00003BB0, uppControlCNTLToCollectionProcInfo = 0x00FEA6F0,
       uppControlColorProcInfo = 0x00001AF0 };
enum { kPowerPCISA = 1 };

static gaddr cp(gaddr ch) { return rd32(ch); }

/* ---------------------------------------------------------------------- */
/* Core Foundation strings, through CarbonLib's own calls */

/* Call one of our exports as PowerPC code would, from native code. */
static uint32_t call_export(const char *name, const uint32_t *args, int n)
{
    void (*fn)(ppc_cpu *) = cfm_export_fn(name);
    if (!fn)
        return 0;
    ppc_cpu *c = calloc(1, sizeof *c);
    c->gpr[1] = (g_ppc.gpr[1] - 2048) & ~15u;
    for (int i = 0; i < n && i < 8; i++)
        c->gpr[3 + i] = args[i];
    fn(c);
    uint32_t r = c->gpr[3];
    free(c);
    return r;
}

/* A CFString's text in MacRoman, as a Pascal string (cut at 255). */
void carbon_cfstring_to_pstr(gaddr cf, uint8_t out[256])
{
    out[0] = 0;
    if (!cf)
        return;
    uint32_t len = call_export("CFStringGetLength", (uint32_t[]){ cf }, 1);
    if (len > 1024)
        len = 1024;
    if (!len)
        return;
    gaddr buf = tb_NewPtr((int32_t)(len * 2), false, false);
    if (!buf)
        return;
    call_export("CFStringGetCharacters", (uint32_t[]){ cf, 0, len, buf }, 4);
    char utf8[4 * 1024 + 1];
    size_t k = 0;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t u = rd16(buf + 2 * i);
        if (u >= 0xD800 && u < 0xDC00 && i + 1 < len) {
            uint32_t lo = rd16(buf + 2 * (i + 1));
            if (lo >= 0xDC00 && lo < 0xE000) {
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        if (u < 0x80) {
            utf8[k++] = (char)u;
        } else if (u < 0x800) {
            utf8[k++] = (char)(0xC0 | u >> 6);
            utf8[k++] = (char)(0x80 | (u & 0x3F));
        } else if (u < 0x10000) {
            utf8[k++] = (char)(0xE0 | u >> 12);
            utf8[k++] = (char)(0x80 | ((u >> 6) & 0x3F));
            utf8[k++] = (char)(0x80 | (u & 0x3F));
        } else {
            utf8[k++] = (char)(0xF0 | u >> 18);
            utf8[k++] = (char)(0x80 | ((u >> 12) & 0x3F));
            utf8[k++] = (char)(0x80 | ((u >> 6) & 0x3F));
            utf8[k++] = (char)(0x80 | (u & 0x3F));
        }
    }
    utf8[k] = 0;
    tb_DisposePtr(buf);
    uint8_t mac[1024];
    size_t m = utf8_to_macroman(utf8, mac, sizeof mac);
    if (m > 255)
        m = 255;
    out[0] = (uint8_t)m;
    memcpy(out + 1, mac, m);
}

/* A new CFString of MacRoman text. */
gaddr carbon_cfstring_from_text(const uint8_t *text, size_t n)
{
    gaddr s = tb_NewPtr((int32_t)n + 1, false, true);
    if (!s)
        return 0;
    gput(s, text, (uint32_t)n);
    gaddr cf = call_export("CFStringCreateWithCString", (uint32_t[]){ 0, s, 0 /* kCFStringEncodingMacRoman */ }, 3);
    tb_DisposePtr(s);
    return cf;
}

void carbon_cfrelease(gaddr cf)
{
    if (cf)
        call_export("CFRelease", (uint32_t[]){ cf }, 1);
}

/* ---------------------------------------------------------------------- */
/* What a control has beyond its record */

typedef struct {
    uint32_t creator, tag, attributes, size;
    uint8_t *data;
} property;

typedef struct {
    uint32_t id_signature;
    int32_t id;
    uint32_t command;
    gaddr color_proc;
    bool drag_tracking;
    property *props;
    int nprops;
} carbon_ctl;

static carbon_ctl *state(gaddr ch, bool create)
{
    ctl_info *c = ctl_find(ch);
    if (!c)
        return NULL;
    if (!c->carbon && create)
        c->carbon = calloc(1, sizeof(carbon_ctl));
    return c->carbon;
}

void carbon_ctl_free(ctl_info *c)
{
    carbon_ctl *s = c->carbon;
    if (!s)
        return;
    for (int i = 0; i < s->nprops; i++)
        free(s->props[i].data);
    free(s->props);
    free(s);
    c->carbon = NULL;
}

uint32_t carbon_ctl_command(gaddr ch)
{
    carbon_ctl *s = state(ch, false);
    return s ? s->command : 0;
}

bool carbon_ctl_id(gaddr ch, uint32_t *signature, int32_t *id)
{
    carbon_ctl *s = state(ch, false);
    if (!s)
        return false;
    *signature = s->id_signature;
    *id = s->id;
    return true;
}

/* Windows that route drags to their controls. */
#define MAX_DRAG_WINDOWS 32
static gaddr drag_windows[MAX_DRAG_WINDOWS];

/* Control definitions registered under CDEF IDs. */
#define MAX_REGISTERED 32
static struct {
    int16_t id;
    gaddr defh, conversion;
} registered[MAX_REGISTERED];
static int nregistered;

void carbon_controls_reset(void)
{
    memset(drag_windows, 0, sizeof drag_windows);
    memset(registered, 0, sizeof registered);
    nregistered = 0;
}

/* ---------------------------------------------------------------------- */
/* The accessors (CarbonAccessors.o's: the record's fields) */

gaddr tb_GetControlBounds(gaddr ch, gaddr bounds)
{
    if (ch && bounds)
        gmemcpy(bounds, cp(ch) + ControlRecord_contrlRect_off, 8);
    return bounds;
}

void tb_SetControlBounds(gaddr ch, gaddr bounds)
{
    if (ch && bounds)
        gmemcpy(cp(ch) + ControlRecord_contrlRect_off, bounds, 8);
}

int8_t tb_IsControlHilited(gaddr ch)
{
    uint8_t h = ControlRecord_contrlHilite(cp(ch));
    return h != 0 && h < 254; /* not inactive (255) or disabled (254) */
}

uint16_t tb_GetControlHilite(gaddr ch) { return ControlRecord_contrlHilite(cp(ch)); }
gaddr tb_GetControlOwner(gaddr ch) { return ControlRecord_contrlOwner(cp(ch)); }
gaddr tb_GetControlDataHandle(gaddr ch) { return ControlRecord_contrlData(cp(ch)); }
void tb_SetControlDataHandle(gaddr ch, gaddr h) { ControlRecord_set_contrlData(cp(ch), h); }

/* A pop-up's private data: the menu, its ID (popupPrivateData). */
static gaddr popup_private(gaddr ch)
{
    gaddr h = ControlRecord_contrlData(cp(ch));
    return h && rd32(h) && tb_GetHandleSize(h) >= 6 ? rd32(h) : 0;
}

gaddr tb_GetControlPopupMenuHandle(gaddr ch)
{
    gaddr p = popup_private(ch);
    return p ? rd32(p) : 0;
}

int16_t tb_GetControlPopupMenuID(gaddr ch)
{
    gaddr p = popup_private(ch);
    return p ? (int16_t)rd16(p + 4) : 0;
}

void tb_SetControlPopupMenuHandle(gaddr ch, gaddr menu)
{
    gaddr p = popup_private(ch);
    if (p)
        wr32(p, menu);
}

void tb_SetControlPopupMenuID(gaddr ch, int16_t id)
{
    gaddr p = popup_private(ch);
    if (p)
        wr16(p + 4, (uint16_t)id);
}

/* ---------------------------------------------------------------------- */
/* Control IDs and command IDs */

int32_t tb_SetControlID(gaddr ch, gaddr id)
{
    carbon_ctl *s = state(ch, true);
    if (!s)
        return errUnknownControl;
    if (!id)
        return paramErr;
    s->id_signature = rd32(id);
    s->id = (int32_t)rd32(id + 4);
    return noErr;
}

int32_t tb_GetControlID(gaddr ch, gaddr id)
{
    if (!ctl_find(ch))
        return errUnknownControl;
    if (!id)
        return paramErr;
    carbon_ctl *s = state(ch, false);
    wr32(id, s ? s->id_signature : 0);
    wr32(id + 4, s ? (uint32_t)s->id : 0);
    return noErr;
}

/* The window's control with the ID (embedded ones are in its list too). */
int32_t tb_GetControlByID(gaddr w, gaddr id, gaddr out)
{
    if (!w || !id || !out)
        return paramErr;
    uint32_t sig = rd32(id);
    int32_t n = (int32_t)rd32(id + 4);
    int k = 0;
    for (gaddr x = WindowRecord_controlList(w); x && rd32(x) && k < 4096; x = ControlRecord_nextControl(cp(x)), k++) {
        carbon_ctl *s = state(x, false);
        if (s && s->id_signature == sig && s->id == n) {
            wr32(out, x);
            return noErr;
        }
    }
    wr32(out, 0);
    return errUnknownControl;
}

int32_t tb_SetControlCommandID(gaddr ch, uint32_t command)
{
    carbon_ctl *s = state(ch, true);
    if (!s)
        return errUnknownControl;
    s->command = command;
    return noErr;
}

int32_t tb_GetControlCommandID(gaddr ch, gaddr out)
{
    if (!ctl_find(ch))
        return errUnknownControl;
    if (out)
        wr32(out, carbon_ctl_command(ch));
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* Properties: a program's data on a control, by creator and tag */

static property *find_property(gaddr ch, int32_t creator, int32_t tag)
{
    carbon_ctl *s = state(ch, false);
    for (int i = 0; s && i < s->nprops; i++)
        if (s->props[i].creator == (uint32_t)creator && s->props[i].tag == (uint32_t)tag)
            return &s->props[i];
    return NULL;
}

int32_t tb_SetControlProperty(gaddr ch, int32_t creator, int32_t tag, uint32_t size, gaddr data)
{
    carbon_ctl *s = state(ch, true);
    if (!s)
        return errUnknownControl;
    if (size && !data)
        return paramErr;
    if (size > 0x1000000)
        return memFullErr;
    property *p = find_property(ch, creator, tag);
    if (!p) {
        property *grown = realloc(s->props, (size_t)(s->nprops + 1) * sizeof *grown);
        if (!grown)
            return memFullErr;
        s->props = grown;
        p = &s->props[s->nprops++];
        memset(p, 0, sizeof *p);
        p->creator = (uint32_t)creator;
        p->tag = (uint32_t)tag;
    }
    uint8_t *bytes = malloc(size ? size : 1);
    if (!bytes)
        return memFullErr;
    gget(bytes, data, size);
    free(p->data);
    p->data = bytes;
    p->size = size;
    return noErr;
}

int32_t tb_GetControlProperty(gaddr ch, int32_t creator, int32_t tag, uint32_t size, gaddr actual, gaddr buffer)
{
    if (!ctl_find(ch))
        return errUnknownControl;
    property *p = find_property(ch, creator, tag);
    if (!p)
        return controlPropertyNotFoundErr;
    if (actual)
        wr32(actual, p->size);
    if (size < p->size)
        return errDataSizeMismatch;
    if (buffer)
        gput(buffer, p->data, p->size);
    return noErr;
}

int32_t tb_GetControlPropertySize(gaddr ch, int32_t creator, int32_t tag, gaddr size)
{
    if (!ctl_find(ch))
        return errUnknownControl;
    property *p = find_property(ch, creator, tag);
    if (!p)
        return controlPropertyNotFoundErr;
    if (size)
        wr32(size, p->size);
    return noErr;
}

int32_t tb_RemoveControlProperty(gaddr ch, int32_t creator, int32_t tag)
{
    if (!ctl_find(ch))
        return errUnknownControl;
    carbon_ctl *s = state(ch, false);
    property *p = find_property(ch, creator, tag);
    if (!p)
        return controlPropertyNotFoundErr;
    free(p->data);
    int i = (int)(p - s->props);
    memmove(&s->props[i], &s->props[i + 1], (size_t)(s->nprops - i - 1) * sizeof *p);
    s->nprops--;
    return noErr;
}

int32_t tb_GetControlPropertyAttributes(gaddr ch, int32_t creator, int32_t tag, gaddr attributes)
{
    if (!ctl_find(ch))
        return errUnknownControl;
    property *p = find_property(ch, creator, tag);
    if (!p)
        return controlPropertyNotFoundErr;
    if (attributes)
        wr32(attributes, p->attributes);
    return noErr;
}

int32_t tb_ChangeControlPropertyAttributes(gaddr ch, int32_t creator, int32_t tag, uint32_t set, uint32_t clear)
{
    if (!ctl_find(ch))
        return errUnknownControl;
    property *p = find_property(ch, creator, tag);
    if (!p)
        return controlPropertyNotFoundErr;
    p->attributes = (p->attributes | set) & ~clear;
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* Regions, titles, text colour */

static uint32_t ctl_features(gaddr ch)
{
    gaddr p = tb_NewPtr(4, false, true);
    uint32_t f = tb_GetControlFeatures(ch, p) == noErr ? rd32(p) : 0;
    tb_DisposePtr(p);
    return f;
}

/* GetControlRegion(control, part, region): the definition's answer where
 * it gives one (kControlMsgGetRegion); else, for the whole control and
 * its content, its rectangle. */
int32_t tb_GetControlRegion(gaddr ch, int16_t part, gaddr rgn)
{
    if (!ctl_find(ch))
        return errUnknownControl;
    if (!rgn)
        return paramErr;
    if (ctl_features(ch) & kControlSupportsGetRegion) {
        /* ControlGetRegionRec: region, part */
        gaddr rec = tb_NewPtr(6, false, true);
        wr32(rec, rgn);
        wr16(rec + 4, (uint16_t)part);
        int32_t err = tb_SendControlMessage(ch, kControlMsgGetRegion, (int32_t)rec);
        tb_DisposePtr(rec);
        if (err == noErr)
            return noErr;
    }
    if (part != kControlStructureMetaPart && part != kControlContentMetaPart)
        return errInvalidPartCode;
    qrect r = qrect_get(cp(ch) + ControlRecord_contrlRect_off);
    tb_SetRectRgn(rgn, r.left, r.top, r.right, r.bottom);
    return noErr;
}

int32_t tb_CopyControlTitleAsCFString(gaddr ch, gaddr out)
{
    if (!ctl_find(ch))
        return errUnknownControl;
    if (!out)
        return paramErr;
    gaddr t = cp(ch) + ControlRecord_contrlTitle_off;
    uint8_t text[256];
    gget(text, t, (uint32_t)rd8(t) + 1);
    gaddr cf = carbon_cfstring_from_text(text + 1, text[0]);
    wr32(out, cf);
    return cf ? noErr : memFullErr;
}

int32_t tb_SetControlTitleWithCFString(gaddr ch, gaddr cf)
{
    if (!ctl_find(ch))
        return errUnknownControl;
    uint8_t text[256];
    carbon_cfstring_to_pstr(cf, text);
    gaddr t = tb_NewPtr(256, false, false);
    gput(t, text, (uint32_t)text[0] + 1);
    tb_SetControlTitle(ch, t);
    tb_DisposePtr(t);
    return noErr;
}

/* The colour procedure a program set: true when it set the colours. */
bool carbon_ctl_color(gaddr ch, int16_t msg, int16_t depth, bool color)
{
    carbon_ctl *s = state(ch, false);
    if (!s || !s->color_proc)
        return false;
    uint32_t args[4] = { ch, (uint32_t)(uint16_t)msg, (uint32_t)(uint16_t)depth, color ? 1u : 0u };
    return (int32_t)mm_call(s->color_proc, uppControlColorProcInfo, args, 4) == noErr;
}

int32_t tb_SetControlColorProc(gaddr ch, gaddr proc)
{
    carbon_ctl *s = state(ch, true);
    if (!s)
        return controlHandleInvalidErr;
    s->color_proc = proc;
    return noErr;
}

/* SetUpControlTextColor: the colour procedure's; else an embedder with a
 * background of its own (placards, tab panes, headers) gives its text
 * colour; else the window's text colour, dimmed when the control is. */
int16_t tb_SetUpControlTextColor(gaddr ch, int16_t depth, int8_t color)
{
    ctl_info *c = ctl_find(ch);
    if (!c)
        return controlHandleInvalidErr;
    if (carbon_ctl_color(ch, kControlMsgApplyTextColor, depth, color != 0))
        return noErr;
    bool active = tb_IsControlActive(ch) != 0;
    for (ctl_info *a = ctl_find(c->parent); a; a = ctl_find(a->parent))
        if (ctl_features(a->ch) & kControlHasSpecialBackground) {
            /* ControlApplyTextColorRec: depth, colorDevice, active */
            gaddr rec = tb_NewPtr(4, false, true);
            wr16(rec, (uint16_t)depth);
            wr8(rec + 2, color != 0);
            wr8(rec + 3, active);
            int32_t err = tb_SendControlMessage(a->ch, kControlMsgApplyTextColor, (int32_t)rec);
            tb_DisposePtr(rec);
            if (err != noErr) /* kThemeTextColorPlacardActive, Inactive */
                tb_SetThemeTextColor(active ? 12 : 13, depth, color);
            return noErr;
        }
    tb_SetThemeTextColorForWindow(ControlRecord_contrlOwner(cp(ch)), active, depth, color);
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* Clicks, cursors, contextual menus: the definition's, where it has them */

int32_t tb_GetControlClickActivation(gaddr ch, uint32_t where, uint16_t modifiers, gaddr out)
{
    if (!ctl_find(ch))
        return errUnknownControl;
    uint32_t result = kActivateAndIgnoreClick;
    if (ctl_features(ch) & kControlSupportsClickActivation) {
        /* ControlClickActivationRec: localPoint, modifiers, result */
        gaddr rec = tb_NewPtr(10, false, true);
        wr32(rec, where);
        wr16(rec + 4, modifiers);
        wr32(rec + 6, result);
        int32_t err = tb_SendControlMessage(ch, kControlMsgGetClickActivation, (int32_t)rec);
        result = rd32(rec + 6);
        tb_DisposePtr(rec);
        if (err != noErr)
            return err;
    }
    if (out)
        wr32(out, result);
    return noErr;
}

int32_t tb_HandleControlContextualMenuClick(gaddr ch, uint32_t where, gaddr shown)
{
    if (!ctl_find(ch))
        return errUnknownControl;
    bool displayed = false;
    if (ctl_features(ch) & kControlSupportsContextualMenus) {
        /* ControlContextualMenuClickRec: localPoint, menuDisplayed */
        gaddr rec = tb_NewPtr(6, false, true);
        wr32(rec, where);
        int32_t err = tb_SendControlMessage(ch, kControlMsgContextualMenuClick, (int32_t)rec);
        displayed = err == noErr && rd8(rec + 4);
        tb_DisposePtr(rec);
    }
    if (shown)
        wr8(shown, displayed);
    return noErr;
}

int32_t tb_HandleControlSetCursor(gaddr ch, uint32_t where, uint16_t modifiers, gaddr was_set)
{
    if (!ctl_find(ch))
        return errUnknownControl;
    bool set = false;
    if (ctl_features(ch) & kControlSupportsSetCursor) {
        /* ControlSetCursorRec: localPoint, modifiers, cursorWasSet */
        gaddr rec = tb_NewPtr(8, false, true);
        wr32(rec, where);
        wr16(rec + 4, modifiers);
        int32_t err = tb_SendControlMessage(ch, kControlMsgSetCursor, (int32_t)rec);
        set = err == noErr && rd8(rec + 6);
        tb_DisposePtr(rec);
    }
    if (was_set)
        wr8(was_set, set);
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* Drag and drop: the control's messages, and windows that send them */

int32_t tb_SetControlDragTrackingEnabled(gaddr ch, int8_t tracks)
{
    carbon_ctl *s = state(ch, true);
    if (!s)
        return errUnknownControl;
    s->drag_tracking = tracks != 0;
    return noErr;
}

int32_t tb_IsControlDragTrackingEnabled(gaddr ch, gaddr out)
{
    if (!ctl_find(ch))
        return errUnknownControl;
    carbon_ctl *s = state(ch, false);
    if (out)
        wr8(out, s && s->drag_tracking);
    return noErr;
}

int32_t tb_HandleControlDragTracking(gaddr ch, int16_t msg, gaddr drag, gaddr likes)
{
    if (!ctl_find(ch))
        return errUnknownControl;
    bool ok = false;
    if (ctl_features(ch) & kControlSupportsDragAndDrop) {
        int16_t m = msg == kDragTrackingEnterControl ? kControlMsgDragEnter
                    : msg == kDragTrackingLeaveControl ? kControlMsgDragLeave : kControlMsgDragWithin;
        ok = tb_SendControlMessage(ch, m, (int32_t)drag) != 0;
    }
    if (likes)
        wr8(likes, ok);
    return noErr;
}

int32_t tb_HandleControlDragReceive(gaddr ch, gaddr drag)
{
    if (!ctl_find(ch))
        return errUnknownControl;
    if (!(ctl_features(ch) & kControlSupportsDragAndDrop))
        return errMessageNotSupported;
    return tb_SendControlMessage(ch, kControlMsgDragReceive, (int32_t)drag);
}

static int drag_window(gaddr w)
{
    for (int i = 0; i < MAX_DRAG_WINDOWS; i++)
        if (drag_windows[i] == w)
            return i;
    return -1;
}

/* The control under the drag's mouse that tracks drags, or 0. */
static gaddr drag_target(gaddr w, gaddr drag)
{
    gaddr pt = tb_NewPtr(8, false, true);
    tb_GetDragMouse(drag, pt, 0);
    gaddr saved = qd_port();
    tb_SetPort(w);
    tb_GlobalToLocal(pt);
    tb_SetPort(saved);
    uint32_t local = rd32(pt);
    gaddr part = pt + 4;
    gaddr ch = tb_FindControlUnderMouse(local, w, part);
    tb_DisposePtr(pt);
    for (; ch; ) { /* the control or an embedder that tracks */
        carbon_ctl *s = state(ch, false);
        if (s && s->drag_tracking)
            return ch;
        ctl_info *ci = ctl_find(ch);
        ch = ci ? ci->parent : 0;
    }
    return 0;
}

/* The window's tracking and receive handlers: messages to the control the
 * drag is over.  DragTrackingHandler(message, window, refCon, drag). */
static gaddr drag_current;

static void window_tracking(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    gaddr drag = cpu_pop32(c);
    cpu_pop32(c);
    gaddr w = cpu_pop32(c);
    int16_t msg = (int16_t)cpu_pop16(c);
    if (msg == 3 || msg == 4) { /* kDragTrackingInWindow, kDragTrackingLeaveWindow */
        gaddr now = msg == 3 ? drag_target(w, drag) : 0;
        if (now != drag_current) {
            if (drag_current && ctl_find(drag_current))
                tb_HandleControlDragTracking(drag_current, kDragTrackingLeaveControl, drag, 0);
            if (now)
                tb_HandleControlDragTracking(now, kDragTrackingEnterControl, drag, 0);
            drag_current = now;
        } else if (now) {
            tb_HandleControlDragTracking(now, kDragTrackingInControl, drag, 0);
        }
    }
    wr16(c->a[7], 0);
    cpu_push32(c, ret);
}

/* DragReceiveHandler(window, refCon, drag) */
static void window_receive(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    gaddr drag = cpu_pop32(c);
    cpu_pop32(c);
    gaddr w = cpu_pop32(c);
    gaddr ch = drag_target(w, drag);
    int32_t err = ch ? tb_HandleControlDragReceive(ch, drag) : -1846; /* dragNotAcceptedErr */
    drag_current = 0;
    wr16(c->a[7], (uint16_t)(err == noErr ? 0 : -1846));
    cpu_push32(c, ret);
}

static gaddr tracking_stub, receive_stub;

int32_t tb_SetAutomaticControlDragTrackingEnabledForWindow(gaddr w, int8_t tracks)
{
    if (!w)
        return paramErr;
    if (!tracking_stub) {
        tracking_stub = native_register(window_tracking, "control drag tracking", 0);
        receive_stub = native_register(window_receive, "control drag receive", 0);
    }
    int i = drag_window(w);
    if (tracks && i < 0) {
        i = drag_window(0);
        if (i < 0)
            return memFullErr;
        drag_windows[i] = w;
        tb_InstallTrackingHandler(tracking_stub, w, 0);
        tb_InstallReceiveHandler(receive_stub, w, 0);
    } else if (!tracks && i >= 0) {
        drag_windows[i] = 0;
        tb_RemoveTrackingHandler(tracking_stub, w);
        tb_RemoveReceiveHandler(receive_stub, w);
    }
    return noErr;
}

int32_t tb_IsAutomaticControlDragTrackingEnabledForWindow(gaddr w, gaddr out)
{
    if (!w)
        return paramErr;
    if (out)
        wr8(out, drag_window(w) >= 0);
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* Custom definitions */

/* A definition handle whose code is the UPP's routine descriptor: the
 * Control Manager calls it as 68k code, Mixed Mode runs the routine. */
static gaddr def_handle(gaddr upp)
{
    if (!upp)
        return 0;
    gaddr h;
    if (mm_is_descriptor(upp)) {
        uint32_t n = 12 + 20 * ((uint32_t)rd16(upp + 10) + 1);
        h = tb_NewHandle((int32_t)n, true, false);
        if (h)
            gmemcpy(rd32(h), upp, n);
    } else { /* a bare procedure pointer: PowerPC code's transition vector */
        h = tb_NewHandle(32, true, true);
        if (h) {
            gaddr d = rd32(h);
            wr16(d, 0xAAFE);
            wr8(d + 2, 7);
            wr32(d + 12, uppControlDefProcInfo);
            wr8(d + 17, kPowerPCISA);
            wr32(d + 20, upp);
        }
    }
    if (h)
        tb_HLock(h);
    return h;
}

/* The record's settings from a Collection's standard tags (ID 0). */
static bool collection_item(gaddr coll, uint32_t tag, void *out, uint32_t size)
{
    gaddr sz = tb_NewPtr((int32_t)(4 + size), false, true);
    wr32(sz, size);
    bool ok = tb_GetCollectionItem(coll, (int32_t)tag, 0, sz, sz + 4) == noErr && rd32(sz) == size;
    if (ok)
        gget(out, sz + 4, size);
    tb_DisposePtr(sz);
    return ok;
}

static int16_t clamp16(int32_t v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

int32_t tb_CreateCustomControl(gaddr w, gaddr bounds, gaddr spec, gaddr coll, gaddr out)
{
    if (out)
        wr32(out, 0);
    if (!w || !bounds || !spec || !out)
        return paramErr;
    if (rd32(spec) != 0) /* kControlDefObjectClass: the Carbon Event Manager's toolbox objects */
        return paramErr;
    gaddr defh = def_handle(rd32(spec + 4));
    if (!defh)
        return memFullErr;
    int32_t value = 0, min = 0, max = 0, refcon = 0;
    uint8_t vis = 1, b[4];
    int16_t var = 0;
    uint8_t title[256] = { 0 };
    if (coll) {
        if (collection_item(coll, TAG('v', 'a', 'l', 'u'), b, 4))
            value = (int32_t)(b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]);
        if (collection_item(coll, TAG('m', 'i', 'n', ' '), b, 4))
            min = (int32_t)(b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]);
        if (collection_item(coll, TAG('m', 'a', 'x', ' '), b, 4))
            max = (int32_t)(b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]);
        if (collection_item(coll, TAG('r', 'e', 'f', 'c'), b, 4))
            refcon = (int32_t)(b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]);
        if (collection_item(coll, TAG('v', 'i', 's', 'i'), b, 1))
            vis = b[0];
        if (collection_item(coll, TAG('v', 'a', 'r', 'c'), b, 2))
            var = (int16_t)(b[0] << 8 | b[1]);
        gaddr sz = tb_NewPtr(4, false, true);
        if (tb_GetCollectionItem(coll, (int32_t)TAG('t', 'i', 't', 'l'), 0, sz, 0) == noErr) {
            uint32_t n = rd32(sz);
            gaddr t = tb_NewPtr((int32_t)(n ? n : 1), false, true);
            if (tb_GetCollectionItem(coll, (int32_t)TAG('t', 'i', 't', 'l'), 0, sz, t) == noErr) {
                title[0] = (uint8_t)(n > 255 ? 255 : n);
                gget(title + 1, t, title[0]);
            }
            tb_DisposePtr(t);
        }
        tb_DisposePtr(sz);
    }
    gaddr t = tb_NewPtr(256, false, false);
    gput(t, title, (uint32_t)title[0] + 1);
    gaddr ch = ctl_new(w, bounds, t, false, clamp16(value), clamp16(min), clamp16(max), var, 0, defh, refcon,
                       (int32_t)coll);
    tb_DisposePtr(t);
    if (!ch)
        return memFullErr;
    if (coll) {
        uint32_t sig = 0;
        int32_t id = 0, cmd = 0;
        bool has_sig = collection_item(coll, TAG('i', 'd', 's', 'i'), b, 4);
        if (has_sig)
            sig = (uint32_t)(b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]);
        if (collection_item(coll, TAG('i', 'd', 'i', 'd'), b, 4))
            id = (int32_t)(b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]);
        if (has_sig || id) {
            carbon_ctl *s = state(ch, true);
            s->id_signature = sig;
            s->id = id;
        }
        if (collection_item(coll, TAG('c', 'm', 'd', ' '), b, 4)) {
            cmd = (int32_t)(b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]);
            state(ch, true)->command = (uint32_t)cmd;
        }
    }
    if (vis)
        tb_ShowControl(ch);
    wr32(out, ch);
    return noErr;
}

int32_t tb_RegisterControlDefinition(int16_t id, gaddr spec, gaddr conversion)
{
    int i = 0;
    while (i < nregistered && registered[i].id != id)
        i++;
    if (!spec) { /* unregistered */
        if (i < nregistered)
            registered[i] = registered[--nregistered];
        return noErr;
    }
    if (rd32(spec) != 0)
        return paramErr;
    if (i == nregistered) {
        if (nregistered == MAX_REGISTERED)
            return memFullErr;
        nregistered++;
    }
    registered[i].id = id;
    registered[i].defh = def_handle(rd32(spec + 4));
    registered[i].conversion = conversion;
    return registered[i].defh ? noErr : memFullErr;
}

/* NewControl's view of a registered definition: its handle, and the
 * Collection the conversion procedure makes from NewControl's arguments
 * (initCntl's parameter). */
gaddr carbon_registered_cdef(int16_t id, gaddr w, gaddr r, gaddr title, bool vis, int16_t value, int16_t min,
                             int16_t max, int16_t procid, int32_t rc, int32_t *init)
{
    *init = 0;
    for (int i = 0; i < nregistered; i++) {
        if (registered[i].id != id)
            continue;
        gaddr coll = tb_NewCollection();
        if (coll && registered[i].conversion) {
            uint32_t args[9] = { r, (uint32_t)(uint16_t)value, vis, (uint32_t)(uint16_t)max, (uint32_t)(uint16_t)min,
                                 (uint32_t)(uint16_t)procid, (uint32_t)rc, title, coll };
            mm_call(registered[i].conversion, uppControlCNTLToCollectionProcInfo, args, 9);
        }
        *init = (int32_t)coll;
        (void)w;
        return registered[i].defh;
    }
    return 0;
}

void carbon_registered_cdef_done(int32_t init)
{
    if (init)
        tb_DisposeCollection((gaddr)init);
}

/* ---------------------------------------------------------------------- */
/* Universal procedure pointers */

/* A descriptor for a PowerPC routine (its transition vector). */
static gaddr new_upp(gaddr proc, uint32_t procinfo)
{
    if (!proc)
        return 0;
    gaddr rd = tb_NewPtr(32, false, true);
    if (!rd)
        return 0;
    wr16(rd, 0xAAFE);
    wr8(rd + 2, 7);         /* kRoutineDescriptorVersion */
    wr32(rd + 12, procinfo);
    wr8(rd + 17, kPowerPCISA);
    wr32(rd + 20, proc);
    return rd;
}

static int upp_args(uint32_t pi)
{
    int n = 0;
    while (n < 13 && ((pi >> (6 + 2 * n)) & 3))
        n++;
    return n;
}

/* InvokeXxxUPP(arguments..., upp): the UPP follows the arguments. */
static void invoke_upp(ppc_cpu *c, uint32_t pi)
{
    int n = upp_args(pi);
    uint32_t args[14];
    for (int i = 0; i <= n; i++)
        args[i] = i < 8 ? c->gpr[3 + i] : rd32(c->gpr[1] + 24 + 4 * (uint32_t)i);
    gaddr upp = args[n];
    uint32_t r = upp ? mm_call(upp, pi, args, n) : 0;
    int size = (int)((pi >> 4) & 3);
    if (size == 1)
        r = (uint32_t)(int32_t)(int8_t)r;
    else if (size == 2)
        r = (uint32_t)(int32_t)(int16_t)r;
    RETURN(r);
}

static void dispose_upp(ppc_cpu *c)
{
    if (mm_is_descriptor(R(0)))
        tb_DisposePtr(R(0));
    c->pc = c->lr;
}

#define UPP(name, pi) \
    static void new_##name(ppc_cpu *c) { RETURN(new_upp(R(0), pi)); } \
    static void invoke_##name(ppc_cpu *c) { invoke_upp(c, pi); }
#define UPP_EXPORTS(name) \
    { "New" #name "UPP", new_##name }, { "Dispose" #name "UPP", dispose_upp }, { "Invoke" #name "UPP", invoke_##name },

/* Controls.h */
UPP(ControlAction, 0x000002C0)
UPP(ControlCNTLToCollection, 0x00FEA6F0)
UPP(ControlColor, 0x00001AF0)
UPP(ControlDef, 0x00003BB0)
UPP(ControlKeyFilter, 0x00003FE0)
/* ControlDefinitions.h */
UPP(ControlEditTextValidation, 0x000000C0)
UPP(ControlUserPaneDraw, 0x000002C0)
UPP(ControlUserPaneHitTest, 0x000003E0)
UPP(ControlUserPaneTracking, 0x00000FE0)
UPP(ControlUserPaneIdle, 0x000000C0)
UPP(ControlUserPaneKeyDown, 0x00002AE0)
UPP(ControlUserPaneActivate, 0x000001C0)
UPP(ControlUserPaneFocus, 0x000002E0)
UPP(ControlUserPaneBackground, 0x000003C0)
/* The Data Browser's callbacks (pascal, by their prototypes' sizes) */
UPP(DataBrowserItem, 0x00000FC0)
UPP(DataBrowserItemData, 0x00007FF0)
UPP(DataBrowserItemCompare, 0x00003FD0)
UPP(DataBrowserItemNotification, 0x00000FC0)
UPP(DataBrowserAddDragItem, 0x00003FD0)
UPP(DataBrowserAcceptDrag, 0x00000FD0)
UPP(DataBrowserReceiveDrag, 0x00000FD0)
UPP(DataBrowserPostProcessDrag, 0x00000FC0)
UPP(DataBrowserGetContextualMenu, 0x0000FFC0)
UPP(DataBrowserSelectContextualMenu, 0x0000AFC0)
UPP(DataBrowserItemHelpContent, 0x0003EFC0)
UPP(DataBrowserDrawItem, 0x0006FFC0)
UPP(DataBrowserEditItem, 0x0003FFD0)
UPP(DataBrowserHitTest, 0x0000FFD0)
UPP(DataBrowserTracking, 0x0002FFE0)
UPP(DataBrowserItemDragRgn, 0x0000FFC0)
UPP(DataBrowserItemAcceptDrag, 0x0000FFF0)
UPP(DataBrowserItemReceiveDrag, 0x0000FFD0)
/* Dialogs.h */
UPP(ModalFilter, 0x00000FD0)
UPP(ModalFilterYD, 0x00003FD0)
UPP(UserItem, 0x000002C0)

const ppc_export carbon_ctl_exports[] = {
    UPP_EXPORTS(ControlAction)
    UPP_EXPORTS(ControlCNTLToCollection)
    UPP_EXPORTS(ControlColor)
    UPP_EXPORTS(ControlDef)
    UPP_EXPORTS(ControlKeyFilter)
    UPP_EXPORTS(ControlEditTextValidation)
    UPP_EXPORTS(ControlUserPaneDraw)
    UPP_EXPORTS(ControlUserPaneHitTest)
    UPP_EXPORTS(ControlUserPaneTracking)
    UPP_EXPORTS(ControlUserPaneIdle)
    UPP_EXPORTS(ControlUserPaneKeyDown)
    UPP_EXPORTS(ControlUserPaneActivate)
    UPP_EXPORTS(ControlUserPaneFocus)
    UPP_EXPORTS(ControlUserPaneBackground)
    UPP_EXPORTS(DataBrowserItem)
    UPP_EXPORTS(DataBrowserItemData)
    UPP_EXPORTS(DataBrowserItemCompare)
    UPP_EXPORTS(DataBrowserItemNotification)
    UPP_EXPORTS(DataBrowserAddDragItem)
    UPP_EXPORTS(DataBrowserAcceptDrag)
    UPP_EXPORTS(DataBrowserReceiveDrag)
    UPP_EXPORTS(DataBrowserPostProcessDrag)
    UPP_EXPORTS(DataBrowserGetContextualMenu)
    UPP_EXPORTS(DataBrowserSelectContextualMenu)
    UPP_EXPORTS(DataBrowserItemHelpContent)
    UPP_EXPORTS(DataBrowserDrawItem)
    UPP_EXPORTS(DataBrowserEditItem)
    UPP_EXPORTS(DataBrowserHitTest)
    UPP_EXPORTS(DataBrowserTracking)
    UPP_EXPORTS(DataBrowserItemDragRgn)
    UPP_EXPORTS(DataBrowserItemAcceptDrag)
    UPP_EXPORTS(DataBrowserItemReceiveDrag)
    UPP_EXPORTS(ModalFilter)
    UPP_EXPORTS(ModalFilterYD)
    UPP_EXPORTS(UserItem)
};
const unsigned carbon_ctl_export_count = sizeof carbon_ctl_exports / sizeof carbon_ctl_exports[0];
