/*
 * The Appearance Manager's controls (CDEF 2 to 27), drawn in Platinum:
 * bevel buttons, sliders, disclosure triangles, progress bars, little
 * arrows, chasing arrows, tabs, separators, group boxes, image wells,
 * pop-up arrows, placards, clocks, user panes, edit text, static text,
 * pictures, icons, window headers, list boxes, push buttons, check boxes
 * and radio buttons (CDEF 23), and radio groups.  (CDEF 24 and 25, the
 * scroll bar and the pop-up button, are CDEF 1's and 63's in controls.c.)
 *
 * Each control's state beyond its record is an ap_ctl of ours, hung on
 * the Control Manager's entry for it; its data is set and got with the
 * Control Manager 2.0 data tags, as the Appearance Manager's CDEFs took
 * them.  A dialog item that is not a control has a stand-in control here
 * for GetDialogItemAsControl, kept out of the window's control list: its
 * text, selection and procedures are the item's.
 */
#include <stdlib.h>
#include <string.h>

#include "qd_internal.h"
#include "tb_internal.h"

enum { drawCntl = 0, testCntl, calcCRgns, initCntl, dispCntl, posCntl, thumbCntl, dragCntl, autoTrack };
enum {
    kControlMsgCalcBestRect = 14, kControlMsgHandleTracking = 15, kControlMsgFocus = 16, kControlMsgKeyDown = 17,
    kControlMsgIdle = 18, kControlMsgGetFeatures = 19, kControlMsgSetData = 20, kControlMsgGetData = 21,
    kControlMsgActivate = 22, kControlMsgSetUpBackground = 23, kControlMsgTestNewMsgSupport = 27,
    kControlMsgSubControlAdded = 28, kControlMsgSubControlRemoved = 29,
};
enum {
    kControlSupportsEmbedding = 1 << 1, kControlSupportsFocus = 1 << 2, kControlWantsIdle = 1 << 3,
    kControlWantsActivate = 1 << 4, kControlHandlesTracking = 1 << 5, kControlSupportsDataAccess = 1 << 6,
    kControlHasSpecialBackground = 1 << 7, kControlGetsFocusOnClick = 1 << 8, kControlSupportsCalcBestRect = 1 << 9,
    kControlSupportsLiveFeedback = 1 << 10,
};
enum {
    kControlLabelPart = 1, kControlMenuPart = 2, kControlTrianglePart = 4, kControlEditTextPart = 5,
    kControlPicturePart = 6, kControlIconPart = 7, kControlClockPart = 8, kControlListBoxPart = 24,
    kControlListBoxDoubleClickPart = 25, kControlImageWellPart = 26, kControlButtonPart = 10,
    kControlCheckBoxPart = 11, kControlUpButtonPart = 20, kControlDownButtonPart = 21, kControlPageUpPart = 22,
    kControlPageDownPart = 23, kControlIndicatorPart = 129,
};
enum { errDataNotSupported = -30581, errDataSizeMismatch = -30591 };
#define NEW_MSG_OK 0x206F6B20 /* ' ok ' */
#define TAG(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

enum { userItem = 0, statText = 8, editText = 16, iconItem = 32, picItem = 64 };

/* Platinum's content kinds (bevel buttons, image wells, icon controls). */
enum {
    kControlContentTextOnly = 0, kControlContentIconSuiteRes = 1, kControlContentCIconRes = 2,
    kControlContentPictRes = 3, kControlContentIconSuiteHandle = 129, kControlContentCIconHandle = 130,
    kControlContentPictHandle = 131,
};

typedef struct {
    uint8_t name[256];
    int16_t icon;
    bool disabled;
} tab_info;

/* A user pane's procedures, by their tags' order. */
enum { UP_DRAW, UP_HIT, UP_TRACK, UP_IDLE, UP_KEY, UP_ACTIVATE, UP_FOCUS, UP_BACK, UP_COUNT };
static const uint32_t up_tags[UP_COUNT] = {
    TAG('d', 'r', 'a', 'w'), TAG('h', 'i', 't', 't'), TAG('t', 'r', 'a', 'k'), TAG('i', 'd', 'l', 'e'),
    TAG('k', 'e', 'y', 'd'), TAG('a', 'c', 't', 'i'), TAG('f', 'o', 'c', 'i'), TAG('b', 'a', 'c', 'k'),
};

typedef struct {
    int id;
    uint32_t features;           /* a user pane's, given as its value */
    /* Content: bevel buttons, image wells, icons, pictures. */
    int16_t content_type;
    gaddr content;               /* handle (resource content is the control's) */
    int16_t transform, text_align, text_offset, graphic_align, graphic_h, graphic_v, text_place;
    int16_t behavior;
    gaddr menu;                  /* bevel buttons */
    int16_t menu_value, last_menu;
    bool center_glyph;
    bool is_default, is_cancel, indeterminate;
    uint8_t phase;               /* progress bars' stripes, chasing arrows */
    uint32_t last_tick;
    int16_t last_value;          /* triangles */
    uint8_t *text;               /* static text */
    int32_t text_len;
    gaddr te;                    /* edit text */
    bool focused, locked;
    uint8_t *clear;              /* a password's own text */
    int32_t clear_len;
    gaddr procs[UP_COUNT];
    gaddr key_filter;
    tab_info *tabs;
    int ntabs;
    gaddr list;                  /* list boxes */
    bool dbl;
    gaddr dialog;                /* a dialog item's stand-in */
    int item;
    int16_t icon_id;             /* push buttons with icons */
    /* CarbonLib's: a slider's tick marks, a menu the control disposes of,
     * an image well taking drags, animation stopped (progress bars,
     * chasing arrows, clocks), a clock's flags and the time it shows (0:
     * now), an edit text's validation procedure, a scrolling text box. */
    int16_t ticks;
    bool owned_menu, drag_dest, still;
    uint32_t clock_flags, clock_secs;
    gaddr validation;
    void *scroll_text;
} ap_ctl;

static gaddr cp(gaddr ch) { return rd32(ch); }
static qrect crect(gaddr ch) { return qrect_get(cp(ch) + ControlRecord_contrlRect_off); }
static int16_t cvalue(gaddr ch) { return ControlRecord_contrlValue(cp(ch)); }
static int16_t cmin(gaddr ch) { return ControlRecord_contrlMin(cp(ch)); }
static int16_t cmax(gaddr ch) { return ControlRecord_contrlMax(cp(ch)); }
static uint8_t chilite(gaddr ch) { return ControlRecord_contrlHilite(cp(ch)); }
static bool cvisible(gaddr ch) { return ControlRecord_contrlVis(cp(ch)) != 0; }
static gaddr cowner(gaddr ch) { return ControlRecord_contrlOwner(cp(ch)); }
static void set_value_raw(gaddr ch, int16_t v) { ControlRecord_set_contrlValue(cp(ch), v); }

static ap_ctl *ap_of(gaddr ch)
{
    ctl_info *c = ctl_find(ch);
    return c ? (ap_ctl *)c->ap : NULL;
}

static int16_t var_of(gaddr ch)
{
    ctl_info *c = ctl_find(ch);
    return c ? c->var : 0;
}

static bool in_rect(qrect r, uint32_t pt)
{
    return pt_h(pt) >= r.left && pt_h(pt) < r.right && pt_v(pt) >= r.top && pt_v(pt) < r.bottom;
}

static qrect inset(qrect r, int d)
{
    return (qrect){ (int16_t)(r.top + d), (int16_t)(r.left + d), (int16_t)(r.bottom - d), (int16_t)(r.right - d) };
}

/* ThemeDrawState from the hilite: 255 inactive, the part pressed, else active. */
static uint32_t draw_state(gaddr ch, int pressed_part)
{
    uint8_t h = chilite(ch);
    if (h == 255 || h == 254)
        return 0;
    return pressed_part && h == pressed_part ? 2 : 1;
}

static gaddr tmp_rect(qrect r)
{
    gaddr p = tb_NewPtr(8, false, false);
    qrect_put(p, r);
    return p;
}

static void erase(qrect r)
{
    gaddr p = tmp_rect(r);
    tb_EraseRect(p);
    tb_DisposePtr(p);
}

static void title_of(gaddr ch, uint8_t *buf, int *n)
{
    gaddr t = cp(ch) + ControlRecord_contrlTitle_off;
    *n = rd8(t);
    gget(buf, t + 1, (uint32_t)*n);
}

/* The text colour for a theme text colour, or the control's own. */
static void text_rgb(gaddr ch, int theme_color, uint16_t out[3])
{
    if (chilite(ch) != 255 && ctl_font_color(ch, out))
        return;
    ap_text_rgb(theme_color, out);
}

/* Data access: ControlDataAccessRec is tag, part, size, dataPtr. */
typedef struct {
    gaddr rec;
    uint32_t tag;
    int32_t part, size;
    gaddr data;
} access;

static access read_access(gaddr rec)
{
    return (access){ rec, rd32(rec), (int32_t)rd32(rec + 4), (int32_t)rd32(rec + 8), rd32(rec + 12) };
}

/* Answer a get with n bytes (copied when the buffer holds them). */
static int32_t give(const access *a, const void *bytes, int32_t n)
{
    if (a->data && a->size >= n)
        gput(a->data, bytes, (uint32_t)n);
    wr32(a->rec + 8, (uint32_t)n);
    return 0;
}

static int32_t give16(const access *a, int16_t v)
{
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    return give(a, b, 2);
}

static int32_t give32(const access *a, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    return give(a, b, 4);
}

static int32_t give_bool(const access *a, bool v)
{
    uint8_t b = v;
    return give(a, &b, 1);
}

static bool take16(const access *a, int16_t *v)
{
    if (a->size < 2 || !a->data)
        return false;
    *v = (int16_t)rd16(a->data);
    return true;
}

static bool take32(const access *a, uint32_t *v)
{
    if (a->size < 4 || !a->data)
        return false;
    *v = rd32(a->data);
    return true;
}

static bool take_bool(const access *a, bool *v)
{
    if (a->size < 1 || !a->data)
        return false;
    *v = rd8(a->data) != 0;
    return true;
}

/* ---------------------------------------------------------------------- */
/* Content: icon suites, colour icons and pictures */

static void dispose_content(ap_ctl *a)
{
    if (!a->content)
        return;
    if (a->content_type == kControlContentIconSuiteRes)
        tb_DisposeIconSuite(a->content, true);
    else if (a->content_type == kControlContentCIconRes)
        tb_DisposeCIcon(a->content);
    a->content = 0;
}

static void load_content(ap_ctl *a, int16_t type, int16_t res)
{
    dispose_content(a);
    a->content_type = type;
    if (type == kControlContentIconSuiteRes) {
        gaddr h = tb_NewPtr(4, false, true);
        if (tb_GetIconSuite(h, res, 0xFFFFFFFFu) == 0)
            a->content = rd32(h);
        tb_DisposePtr(h);
    } else if (type == kControlContentCIconRes) {
        a->content = tb_GetCIcon(res);
    } else if (type == kControlContentPictRes) {
        a->content = tb_GetPicture(res);
    }
}

/* ControlButtonContentInfo: contentType, then a resource ID or a handle. */
static void set_content_info(ap_ctl *a, gaddr info)
{
    int16_t type = (int16_t)rd16(info);
    if (type < 128)
        load_content(a, type, (int16_t)rd16(info + 2));
    else {
        dispose_content(a);
        a->content_type = type;
        a->content = rd32(info + 2);
    }
}

/* The content's natural size. */
static void content_size(const ap_ctl *a, int *w, int *h)
{
    *w = *h = 0;
    if (!a->content || !rd32(a->content))
        return;
    switch (a->content_type) {
    case kControlContentIconSuiteRes: case kControlContentIconSuiteHandle:
        *w = *h = 32;
        break;
    case kControlContentCIconRes: case kControlContentCIconHandle: {
        qrect b = qrect_get(rd32(a->content) + 6); /* iconPMap.bounds */
        *w = b.right - b.left;
        *h = b.bottom - b.top;
        break;
    }
    case kControlContentPictRes: case kControlContentPictHandle: {
        qrect b = qrect_get(rd32(a->content) + 2); /* picFrame */
        *w = b.right - b.left;
        *h = b.bottom - b.top;
        break;
    }
    default: break;
    }
}

/* Too big for the room: icons drop to their small size, the rest shrink
 * keeping their shape. */
static void fit_content(const ap_ctl *a, qrect room, int *w, int *h)
{
    int rw = room.right - room.left, rh = room.bottom - room.top;
    if (*w <= rw && *h <= rh)
        return;
    if (a->content_type != kControlContentPictRes && a->content_type != kControlContentPictHandle && rw >= 16 && rh >= 16) {
        *w = *h = 16;
        return;
    }
    if (*w <= 0 || *h <= 0 || rw <= 0 || rh <= 0)
        return;
    if ((long)*w * rh > (long)*h * rw) {
        *h = *h * rw / *w;
        *w = rw;
    } else {
        *w = *w * rh / *h;
        *h = rh;
    }
}

static void draw_content(const ap_ctl *a, qrect r, bool inactive)
{
    if (!a->content || !rd32(a->content))
        return;
    gaddr rp = tmp_rect(r);
    switch (a->content_type) {
    case kControlContentIconSuiteRes: case kControlContentIconSuiteHandle:
        tb_PlotIconSuite(rp, 0, (int16_t)(inactive ? 1 : a->transform), a->content); /* kTransformDisabled */
        break;
    case kControlContentCIconRes: case kControlContentCIconHandle:
        tb_PlotCIcon(rp, a->content);
        break;
    case kControlContentPictRes: case kControlContentPictHandle:
        tb_DrawPicture(a->content, rp);
        break;
    default: break;
    }
    tb_DisposePtr(rp);
}

/* A graphic of size w x h placed in r by an alignment (centre, left,
 * right, top, bottom, the corners). */
static qrect place(qrect r, int w, int h, int align, int dh, int dv)
{
    int x = r.left + (r.right - r.left - w) / 2, y = r.top + (r.bottom - r.top - h) / 2;
    switch (align) {
    case 1: case 5: case 6: x = r.left + dh; break;
    case 2: case 7: case 8: x = r.right - w - dh; break;
    default: break;
    }
    switch (align) {
    case 3: case 5: case 7: y = r.top + dv; break;
    case 4: case 6: case 8: y = r.bottom - h - dv; break;
    default: break;
    }
    return (qrect){ (int16_t)y, (int16_t)x, (int16_t)(y + h), (int16_t)(x + w) };
}

/* ---------------------------------------------------------------------- */
/* CDEF 23 (and CDEF 0 in Platinum): push buttons, check boxes, radio buttons */

void ap_draw_standard(gaddr ch, int kind)
{
    if (!cvisible(ch))
        return;
    ap_ctl *a = ap_of(ch);
    qrect r = crect(ch);
    uint8_t title[256];
    int n;
    title_of(ch, title, &n);
    uint8_t hil = chilite(ch);
    uint32_t state = hil == 255 || hil == 254 ? 0 : (hil == 10 || hil == 11) ? 2 : 1;
    uint16_t c[3];
    if (g_appearance != MACHINE_APPEARANCE_BW) { /* the host's controls, or a Kaleidoscope theme's */
        bool pressed = state == 2, disabled = state == 0;
        if (kind == 0 && (native_draw_button(r, title, n, pressed, disabled) || theme_draw_button(r, title, n, pressed, disabled)))
            return;
        int top = r.top + (r.bottom - r.top - 12) / 2;
        qrect box = { (int16_t)top, (int16_t)(r.left + 2), (int16_t)(top + 12), (int16_t)(r.left + 14) };
        if (kind != 0) {
            erase(r);
            if (native_draw_check(box, kind == 2, cvalue(ch) != 0, pressed, disabled) ||
                theme_draw_check(box, kind == 2, cvalue(ch) != 0, pressed, disabled)) {
                text_rgb(ch, disabled ? 2 : 1, c);
                ap_text_in((qrect){ r.top, (int16_t)(box.right + 5), r.bottom, r.right }, title, n, false, c);
                return;
            }
        }
    }
    if (kind == 0) {
        ap_push_button(r, state, a && a->is_default);
        text_rgb(ch, state == 0 ? 13 : state == 2 ? 14 : 12, c);
        qrect tr = inset(r, 3);
        if (a && (a->icon_id || a->content)) { /* a colour icon at the left (6) or right (7) */
            gaddr icon = a->content ? 0 : tb_GetCIcon(a->icon_id);
            int iy = r.top + (r.bottom - r.top - 16) / 2;
            bool right = (var_of(ch) & 7) == 7;
            qrect ir = right ? (qrect){ (int16_t)iy, (int16_t)(r.right - 22), (int16_t)(iy + 16), (int16_t)(r.right - 6) }
                             : (qrect){ (int16_t)iy, (int16_t)(r.left + 6), (int16_t)(iy + 16), (int16_t)(r.left + 22) };
            if (a->content) /* CreatePushButtonWithIconControl's */
                draw_content(a, ir, state == 0);
            if (icon) {
                gaddr rp = tmp_rect(ir);
                tb_PlotCIcon(rp, icon);
                tb_DisposePtr(rp);
                tb_DisposeCIcon(icon);
            }
            if (right)
                tr.right = (int16_t)(ir.left - 2);
            else
                tr.left = (int16_t)(ir.right + 2);
        }
        ap_text_in(tr, title, n, true, c);
        return;
    }
    erase(r);
    int top = r.top + (r.bottom - r.top - 12) / 2;
    qrect box = { (int16_t)top, (int16_t)(r.left + 2), (int16_t)(top + 12), (int16_t)(r.left + 14) };
    int v = cvalue(ch);
    ap_check_box(box, kind == 2, v == 1 ? 1 : v == 2 ? 2 : 0, state);
    text_rgb(ch, state == 0 ? 2 : 1, c);
    ap_text_in((qrect){ r.top, (int16_t)(box.right + 5), r.bottom, r.right }, title, n, false, c);
}

static int32_t standard_test(gaddr ch, uint32_t pt)
{
    if (!in_rect(crect(ch), pt))
        return 0;
    int kind = var_of(ch) & 7;
    return kind == 0 || kind >= 6 ? kControlButtonPart : kControlCheckBoxPart;
}

static int standard_kind(gaddr ch)
{
    int v = var_of(ch) & 7;
    return v == 1 || v == 3 ? 1 : v == 2 || v == 4 ? 2 : 0;
}

/* ---------------------------------------------------------------------- */
/* Tracks: sliders and progress bars, as ThemeTrackDrawInfo */

static gaddr track_info(gaddr ch, int kind, int press)
{
    gaddr info = tb_NewPtr(36, false, true);
    qrect r = crect(ch);
    ap_ctl *a = ap_of(ch);
    wr16(info, (uint16_t)kind);
    qrect_put(info + 2, r);
    wr32(info + 10, (uint32_t)(int32_t)cmin(ch));
    wr32(info + 14, (uint32_t)(int32_t)cmax(ch));
    wr32(info + 18, (uint32_t)(int32_t)cvalue(ch));
    int attrs = (r.right - r.left >= r.bottom - r.top) ? 1 : 0;
    if (kind == 2) {
        int v = var_of(ch);
        if (v & 4)
            attrs |= 2;
        attrs |= 4;
        wr8(info + 30, (v & 8) ? 0 : (v & 4) ? 1 : 2); /* thumb direction */
        wr8(info + 31, (uint8_t)press);
    } else {
        wr8(info + 30, a ? a->phase : 0);
    }
    wr16(info + 26, (uint16_t)attrs);
    wr8(info + 28, chilite(ch) == 255 ? 1 : 0);
    return info;
}

static void draw_track(gaddr ch, int kind)
{
    if (!cvisible(ch))
        return;
    gaddr info = track_info(ch, kind, chilite(ch) == kControlIndicatorPart ? 8 : 0);
    erase(crect(ch));
    ap_track(info);
    if (kind == 2 && (var_of(ch) & 2) && !(var_of(ch) & 8)) { /* tick marks: one per value up to 20 */
        int range = cmax(ch) - cmin(ch);
        ap_ctl *a = ap_of(ch);
        uint32_t n = a && a->ticks > 0 ? (uint32_t)a->ticks : (uint32_t)(range > 0 && range < 20 ? range + 1 : 11);
        tb_DrawThemeTrackTickMarks(info, n, 0, 0);
    }
    tb_DisposePtr(info);
}

static int32_t slider_test(gaddr ch, uint32_t pt)
{
    gaddr info = track_info(ch, 2, 0);
    gaddr part = tb_NewPtr(2, false, true);
    tb_HitTestThemeTrack(info, pt, part);
    int16_t p = (int16_t)rd16(part);
    tb_DisposePtr(part);
    tb_DisposePtr(info);
    if (!p && in_rect(crect(ch), pt))
        p = kControlPageUpPart;
    return p;
}

/* Drag a slider's thumb: the value follows the mouse, the action
 * procedure called as it changes with live feedback. */
static int16_t slider_drag(gaddr ch, uint32_t pt, gaddr action)
{
    gaddr info = track_info(ch, 2, 8);
    gaddr pos = tb_NewPtr(4, false, true), val = tb_NewPtr(4, false, true);
    tb_GetThemeTrackThumbPositionFromOffset(info, 0, pos);
    qrect r = crect(ch);
    bool h = r.right - r.left >= r.bottom - r.top;
    int32_t p0 = (int32_t)rd32(pos), start = h ? pt_h(pt) : pt_v(pt);
    bool live = (var_of(ch) & 1) && action && action != 0xFFFFFFFFu;
    tb_HiliteControl(ch, kControlIndicatorPart);
    do {
        uint32_t m = ctl_local_mouse(ch);
        int32_t at = p0 + (h ? pt_h(m) : pt_v(m)) - start;
        tb_GetThemeTrackLiveValue(info, at, val);
        int16_t v = (int16_t)(int32_t)rd32(val);
        if (var_of(ch) & 4)
            v = (int16_t)(cmax(ch) - (v - cmin(ch)));
        if (v != cvalue(ch)) {
            tb_SetControlValue(ch, v);
            if (live)
                ctl_call_action(action, ch, kControlIndicatorPart);
        }
        ev_wait_tick();
    } while (ev_still_down());
    tb_HiliteControl(ch, 0);
    tb_DisposePtr(val);
    tb_DisposePtr(pos);
    tb_DisposePtr(info);
    return kControlIndicatorPart;
}

/* ---------------------------------------------------------------------- */
/* CDEF 2: bevel buttons */

static void bevel_init(gaddr ch, ap_ctl *a)
{
    int16_t min = cmin(ch), max = cmax(ch), value = cvalue(ch);
    a->behavior = (int16_t)(min & 0xFF00);
    load_content(a, (int16_t)(min & 0xFF), max);
    if (value) {
        gaddr mh = tb_GetMenuHandle(value);
        if (!mh) {
            mh = tb_GetMenu(value);
            if (mh)
                tb_InsertMenu(mh, -1);
        }
        a->menu = mh;
    }
    a->graphic_align = 0;
    a->text_place = 0;
    set_value_raw(ch, 0);
    ControlRecord_set_contrlMin(cp(ch), 0);
    ControlRecord_set_contrlMax(cp(ch), 1);
}

static void bevel_draw(gaddr ch, ap_ctl *a)
{
    if (!cvisible(ch))
        return;
    qrect r = crect(ch);
    int var = var_of(ch);
    int kind = (var & 3) == 0 ? 8 : (var & 3) == 2 ? 9 : 3;
    uint32_t state = draw_state(ch, kControlButtonPart);
    if (chilite(ch) == kControlMenuPart)
        state = 2;
    bool on = cvalue(ch) != 0;
    ap_bevel_button(r, kind, state, on);
    qrect in = inset(r, kind == 9 ? 4 : kind == 8 ? 2 : 3);
    if ((a->behavior & 0x8000) && (state == 2 || on)) { /* kControlBehaviorOffsetContents */
        in.top++;
        in.left++;
        in.bottom++;
        in.right++;
    }
    uint8_t title[256];
    int n;
    title_of(ch, title, &n);
    int gw, gh;
    content_size(a, &gw, &gh);
    fit_content(a, in, &gw, &gh);
    qrect graphic = { 0, 0, 0, 0 }, text = in;
    if (gw && n) {
        int asc, desc;
        ap_font_info(&asc, &desc);
        switch (a->text_place) {
        case 3: /* below the graphic */
            graphic = place((qrect){ in.top, in.left, (int16_t)(in.bottom - asc - desc - 2), in.right }, gw, gh, 0, 0, 0);
            text.top = (int16_t)(graphic.bottom + 1);
            break;
        case 4: /* above */
            graphic = place((qrect){ (int16_t)(in.top + asc + desc + 2), in.left, in.bottom, in.right }, gw, gh, 0, 0, 0);
            text.bottom = (int16_t)(graphic.top - 1);
            break;
        case 2: /* to its left */
            graphic = place(in, gw, gh, 2, 2, 0);
            text.right = (int16_t)(graphic.left - 2);
            break;
        default: /* to its right */
            graphic = place(in, gw, gh, 1, 2, 0);
            text.left = (int16_t)(graphic.right + 2);
            break;
        }
    } else if (gw) {
        graphic = place(in, gw, gh, a->graphic_align, a->graphic_h, a->graphic_v);
    }
    if (gw)
        draw_content(a, graphic, state == 0);
    if (n) {
        uint16_t c[3];
        text_rgb(ch, state == 0 ? 16 : state == 2 ? 17 : 15, c);
        if (a->text_align == 1 || a->text_align == -1) /* flush left */
            text.left = (int16_t)(text.left + a->text_offset);
        ap_text_in(text, title, n, a->text_align == 0 || a->text_align == 1 /* teCenter */, c);
    }
    if (a->menu) { /* the pop-up glyph: down at the bottom right, or right */
        uint16_t c[3];
        ap_text_rgb(state == 0 ? 16 : 15, c);
        qrect g = (var & 4) ? (qrect){ (int16_t)((r.top + r.bottom) / 2 - 4), (int16_t)(r.right - 10), (int16_t)((r.top + r.bottom) / 2 + 4), (int16_t)(r.right - 3) }
                            : a->center_glyph ? (qrect){ (int16_t)(r.bottom - 8), (int16_t)((r.left + r.right) / 2 - 4), (int16_t)(r.bottom - 3), (int16_t)((r.left + r.right) / 2 + 4) }
                                              : (qrect){ (int16_t)(r.bottom - 9), (int16_t)(r.right - 11), (int16_t)(r.bottom - 3), (int16_t)(r.right - 3) };
        ap_arrow(g, (var & 4) ? 2 : 1, 7, c);
    }
}

/* Pop the menu up below the button (or to its right) and take a choice. */
static int16_t bevel_menu(gaddr ch, ap_ctl *a)
{
    qrect r = crect(ch);
    bool right = var_of(ch) & 4;
    gaddr pt = tb_NewPtr(4, false, true);
    wr32(pt, right ? make_pt(r.top, r.right) : make_pt(r.bottom, r.left));
    tb_LocalToGlobal(pt);
    uint32_t g = rd32(pt);
    tb_DisposePtr(pt);
    bool multi = (a->behavior & 0x4000) && !(a->behavior & 0x2000);
    if (!(a->behavior & 0x2000) && !multi) { /* single value: the current item marked */
        int items = tb_CountMItems(a->menu);
        for (int i = 1; i <= items; i++)
            tb_SetItemMark(a->menu, (int16_t)i, i == a->menu_value ? 0x12 : 0);
    }
    tb_HiliteControl(ch, kControlMenuPart);
    int32_t res = tb_PopUpMenuSelect(a->menu, (int16_t)pt_v(g), (int16_t)pt_h(g), 0);
    tb_HiliteControl(ch, 0);
    int16_t item = (int16_t)(res & 0xFFFF);
    if (!res || item <= 0)
        return 0;
    a->menu_value = item;
    a->last_menu = (int16_t)(res >> 16);
    if (multi) {
        gaddr m = tb_NewPtr(2, false, true);
        tb_GetItemMark(a->menu, item, m);
        tb_SetItemMark(a->menu, item, rd16(m) ? 0 : 0x12);
        tb_DisposePtr(m);
    }
    return kControlMenuPart;
}

static int32_t bevel_data(gaddr ch, ap_ctl *a, const access *x, bool set)
{
    int16_t v16;
    uint32_t v32;
    bool b;
    switch (x->tag) {
    case TAG('c', 'o', 'n', 't'):
        if (set) {
            if (x->size < 6)
                return errDataSizeMismatch;
            set_content_info(a, x->data);
            break;
        } else {
            uint8_t info[6] = { (uint8_t)(a->content_type >> 8), (uint8_t)a->content_type };
            info[2] = (uint8_t)(a->content >> 24);
            info[3] = (uint8_t)(a->content >> 16);
            info[4] = (uint8_t)(a->content >> 8);
            info[5] = (uint8_t)a->content;
            return give(x, info, 6);
        }
    case TAG('t', 'r', 'a', 'n'):
        if (set) { if (take16(x, &v16)) a->transform = v16; break; }
        return give16(x, a->transform);
    case TAG('t', 'a', 'l', 'i'):
        if (set) { if (take16(x, &v16)) a->text_align = v16; break; }
        return give16(x, a->text_align);
    case TAG('t', 'o', 'f', 'f'):
        if (set) { if (take16(x, &v16)) a->text_offset = v16; break; }
        return give16(x, a->text_offset);
    case TAG('g', 'a', 'l', 'i'):
        if (set) { if (take16(x, &v16)) a->graphic_align = v16; break; }
        return give16(x, a->graphic_align);
    case TAG('g', 'o', 'f', 'f'):
        if (set) {
            if (take32(x, &v32)) {
                a->graphic_v = pt_v(v32);
                a->graphic_h = pt_h(v32);
            }
            break;
        }
        return give32(x, make_pt(a->graphic_v, a->graphic_h));
    case TAG('t', 'p', 'l', 'c'):
        if (set) { if (take16(x, &v16)) a->text_place = v16; break; }
        return give16(x, a->text_place);
    case TAG('m', 'v', 'a', 'l'):
        if (set) { if (take16(x, &v16)) a->menu_value = v16; break; }
        return give16(x, a->menu_value);
    case TAG('m', 'h', 'n', 'd'): case TAG('o', 'm', 'r', 'f'):
        if (set) {
            if (take32(x, &v32)) {
                if (a->owned_menu && a->menu && a->menu != v32)
                    tb_DisposeMenu(a->menu);
                a->menu = v32;
                a->owned_menu = x->tag == TAG('o', 'm', 'r', 'f');
            }
            break;
        }
        return give32(x, a->menu);
    case TAG('p', 'g', 'l', 'c'):
        if (set) { if (take_bool(x, &b)) a->center_glyph = b; break; }
        return give_bool(x, a->center_glyph);
    case TAG('l', 'm', 'n', 'u'):
        if (set) break;
        return give16(x, a->last_menu);
    case TAG('m', 'd', 'l', 'y'): case TAG('s', 'c', 'a', 'l'):
        if (set) break;
        return give32(x, 0);
    default:
        return errDataNotSupported;
    }
    if (cvisible(ch))
        tb_Draw1Control(ch);
    return 0;
}

/* ---------------------------------------------------------------------- */
/* CDEF 8: tabs (north facing; the others are drawn so too) */

static int tab_height(gaddr ch) { return (var_of(ch) & 1) ? 19 : 24; }

/* The tabs' row along the top, from left + 8, each as wide as its title. */
static qrect tab_rect(gaddr ch, ap_ctl *a, int i)
{
    qrect r = crect(ch);
    int x = r.left + 8, th = tab_height(ch);
    for (int k = 1; k <= a->ntabs; k++) {
        int n = a->tabs[k - 1].name[0];
        int w = qd_text_width(qd_port(), a->tabs[k - 1].name + 1, n) + ((var_of(ch) & 1) ? 16 : 24);
        if (w < 40)
            w = 40;
        if (k == i)
            return (qrect){ r.top, (int16_t)x, (int16_t)(r.top + th), (int16_t)(x + w) };
        x += w - 1;
    }
    return (qrect){ 0, 0, 0, 0 };
}

static qrect tab_pane(gaddr ch)
{
    qrect r = crect(ch);
    r.top = (int16_t)(r.top + tab_height(ch) - 3);
    return r;
}

static void tab_font(gaddr ch, ctl_font_saved *fs)
{
    ctl_font_begin(ch, fs);
    if (var_of(ch) & 1) { /* small tabs: the small system font */
        GrafPort_set_txFont(qd_port(), 3);
        GrafPort_set_txSize(qd_port(), 10);
    }
}

static void tabs_init(gaddr ch, ap_ctl *a)
{
    int16_t id = cvalue(ch);
    a->ntabs = 0;
    gaddr h = id ? tb_GetResource((int32_t)fourcc("tab#"), id) : 0;
    if (h && rd32(h)) {
        gaddr p = rd32(h);
        uint32_t len = (uint32_t)tb_GetHandleSize(h);
        int n = len >= 4 ? (int16_t)rd16(p + 2) : 0;
        a->tabs = calloc((size_t)(n > 0 ? n : 1), sizeof *a->tabs);
        uint32_t o = 4;
        for (int i = 0; i < n && o + 3 <= len; i++) {
            a->tabs[i].icon = (int16_t)rd16(p + o);
            int sl = rd8(p + o + 2);
            gget(a->tabs[i].name, p + o + 2, (uint32_t)sl + 1);
            o += 2 + 1 + (uint32_t)sl + 6;
            a->ntabs++;
        }
    }
    ControlRecord_set_contrlMin(cp(ch), a->ntabs ? 1 : 0);
    ControlRecord_set_contrlMax(cp(ch), (int16_t)a->ntabs);
    set_value_raw(ch, (int16_t)(a->ntabs ? 1 : 0));
}

/* More tabs than names: SetControlMaximum adds them. */
static void tabs_fit(gaddr ch, ap_ctl *a)
{
    int want = cmax(ch);
    if (want > a->ntabs && want < 256) {
        a->tabs = realloc(a->tabs, (size_t)want * sizeof *a->tabs);
        memset(a->tabs + a->ntabs, 0, (size_t)(want - a->ntabs) * sizeof *a->tabs);
        a->ntabs = want;
    } else if (want >= 0 && want < a->ntabs) {
        a->ntabs = want;
    }
}

static void tabs_draw(gaddr ch, ap_ctl *a)
{
    if (!cvisible(ch))
        return;
    tabs_fit(ch, a);
    ctl_font_saved fs;
    tab_font(ch, &fs);
    qrect r = crect(ch);
    erase((qrect){ r.top, r.left, (int16_t)(r.top + tab_height(ch) - 3), r.right });
    bool active = chilite(ch) != 255;
    ap_tab_pane(tab_pane(ch), active);
    int front = cvalue(ch);
    for (int pass = 0; pass < 2; pass++)
        for (int i = 1; i <= a->ntabs; i++) {
            if ((i == front) != (pass == 1))
                continue;
            bool disabled = a->tabs[i - 1].disabled || !active;
            int style = i == front ? (disabled ? 4 : 3) : disabled ? 2 : chilite(ch) == i ? 1 : 0;
            ap_tab(tab_rect(ch, a, i), style, a->tabs[i - 1].name + 1, a->tabs[i - 1].name[0]);
        }
    ctl_font_end(&fs);
}

static int32_t tabs_test(gaddr ch, ap_ctl *a, uint32_t pt)
{
    ctl_font_saved fs;
    tab_font(ch, &fs);
    int hit = 0;
    for (int i = 1; i <= a->ntabs && !hit; i++)
        if (in_rect(tab_rect(ch, a, i), pt) && !a->tabs[i - 1].disabled)
            hit = i;
    ctl_font_end(&fs);
    return hit;
}

/* A tab pressed while the mouse stays on it; let go there, it comes to
 * the front (the value). */
static int16_t tabs_track(gaddr ch, ap_ctl *a, int16_t part)
{
    bool inside = true;
    tb_HiliteControl(ch, part);
    while (ev_still_down()) {
        ev_wait_tick();
        bool now = tabs_test(ch, a, ctl_local_mouse(ch)) == part;
        if (now != inside) {
            inside = now;
            tb_HiliteControl(ch, inside ? part : 0);
        }
    }
    tb_HiliteControl(ch, 0);
    if (!inside)
        return 0;
    if (cvalue(ch) != part)
        tb_SetControlValue(ch, part);
    return part;
}

static int32_t tabs_data(gaddr ch, ap_ctl *a, const access *x, bool set)
{
    tabs_fit(ch, a);
    int i = x->part;
    switch (x->tag) {
    case TAG('r', 'e', 'c', 't'): {
        qrect c = inset(tab_pane(ch), 4);
        uint8_t b[8] = { (uint8_t)(c.top >> 8), (uint8_t)c.top, (uint8_t)(c.left >> 8), (uint8_t)c.left,
                         (uint8_t)(c.bottom >> 8), (uint8_t)c.bottom, (uint8_t)(c.right >> 8), (uint8_t)c.right };
        return set ? errDataNotSupported : give(x, b, 8);
    }
    case TAG('e', 'n', 'a', 'b'): {
        if (i < 1 || i > a->ntabs)
            return -50;
        bool b;
        if (!set)
            return give_bool(x, !a->tabs[i - 1].disabled);
        if (take_bool(x, &b))
            a->tabs[i - 1].disabled = !b;
        break;
    }
    case TAG('t', 'a', 'b', 'i'): {
        /* ControlTabInfoRec: version, iconSuiteID, name (Str255) */
        if (i < 1 || i > a->ntabs)
            return -50;
        if (!set) {
            uint8_t b[260];
            memset(b, 0, sizeof b);
            b[2] = (uint8_t)(a->tabs[i - 1].icon >> 8);
            b[3] = (uint8_t)a->tabs[i - 1].icon;
            memcpy(b + 4, a->tabs[i - 1].name, 256);
            return give(x, b, 260);
        }
        if (x->size < 5)
            return errDataSizeMismatch;
        a->tabs[i - 1].icon = (int16_t)rd16(x->data + 2);
        if (rd16(x->data) == 1) /* ControlTabInfoRecV1: the name a CFString */
            carbon_cfstring_to_pstr(rd32(x->data + 4), a->tabs[i - 1].name);
        else
            gget(a->tabs[i - 1].name, x->data + 4, (uint32_t)rd8(x->data + 4) + 1);
        break;
    }
    default:
        return errDataNotSupported;
    }
    if (cvisible(ch))
        tb_Draw1Control(ch);
    return 0;
}

/* ---------------------------------------------------------------------- */
/* CDEF 10: group boxes */

static int group_title_height(void)
{
    int asc, desc;
    ap_font_info(&asc, &desc);
    return asc + desc;
}

/* A pop-up group box's title: its menu's current item. */
static void group_title_text(gaddr ch, uint8_t *t, int *n)
{
    ap_ctl *a = ap_of(ch);
    if ((var_of(ch) & 3) == 2 && a && a->menu) {
        gaddr s = tb_NewPtr(256, false, true);
        tb_GetMenuItemText(a->menu, cvalue(ch), s);
        *n = rd8(s);
        gget(t, s + 1, (uint32_t)*n);
        tb_DisposePtr(s);
        return;
    }
    title_of(ch, t, n);
}

static qrect group_title(gaddr ch)
{
    qrect r = crect(ch);
    uint8_t t[256];
    int n;
    group_title_text(ch, t, &n);
    int w = n ? qd_text_width(qd_port(), t, n) + 4 : 0;
    if ((var_of(ch) & 3) == 1)
        w += 18;
    else if ((var_of(ch) & 3) == 2)
        w += 16; /* the pop-up's arrow */
    return (qrect){ r.top, (int16_t)(r.left + 8), (int16_t)(r.top + group_title_height()), (int16_t)(r.left + 8 + w) };
}

static void group_draw(gaddr ch)
{
    if (!cvisible(ch))
        return;
    ctl_font_saved fs;
    ctl_font_begin(ch, &fs);
    qrect r = crect(ch);
    uint8_t t[256];
    int n;
    group_title_text(ch, t, &n);
    int th = n || (var_of(ch) & 3) ? group_title_height() : 0;
    uint32_t state = draw_state(ch, 0);
    qrect frame = { (int16_t)(r.top + th / 2), r.left, r.bottom, r.right };
    ap_group(frame, state, !(var_of(ch) & 4));
    if (th) {
        qrect tr = group_title(ch);
        erase(tr);
        uint16_t c[3];
        text_rgb(ch, state ? 1 : 2, c);
        if ((var_of(ch) & 3) == 1) { /* a check box for a title */
            qrect box = { (int16_t)(tr.top + (th - 12) / 2), (int16_t)(tr.left + 2), (int16_t)(tr.top + (th - 12) / 2 + 12), (int16_t)(tr.left + 14) };
            ap_check_box(box, false, cvalue(ch) ? 1 : 0, chilite(ch) == kControlCheckBoxPart ? 2 : state);
            tr.left = (int16_t)(box.right + 4);
        } else if ((var_of(ch) & 3) == 2) { /* a pop-up: the arrow after the item */
            ap_arrow((qrect){ (int16_t)(tr.top + (th - 9) / 2), (int16_t)(tr.right - 14), (int16_t)(tr.top + (th - 9) / 2 + 9),
                              (int16_t)(tr.right - 5) }, 3, 9, c);
            tr.left = (int16_t)(tr.left + 2);
            tr.right = (int16_t)(tr.right - 14);
        } else {
            tr.left = (int16_t)(tr.left + 2);
        }
        ap_text_in(tr, t, n, false, c);
    }
    ctl_font_end(&fs);
}

/* ---------------------------------------------------------------------- */
/* CDEF 16: user panes (and the root control); 'uidp' for dialog user items */

static int32_t call_up(ap_ctl *a, int which, gaddr ch, uint32_t pi, uint32_t arg2, uint32_t arg3, int nargs)
{
    uint32_t args[4] = { ch, arg2, arg3, 0 };
    return (int32_t)mm_call(a->procs[which], pi, args, nargs);
}

static void pane_draw(gaddr ch, ap_ctl *a, int part)
{
    if (!cvisible(ch))
        return;
    if (a->procs[UP_DRAW])
        call_up(a, UP_DRAW, ch, 0x000002C0 /* pascal (ControlHandle, SInt16) */, (uint32_t)part, 0, 2);
    else if (a->dialog)
        dialog_draw_item(a->dialog, a->item);
}

static int32_t pane_data(gaddr ch, ap_ctl *a, const access *x, bool set)
{
    for (int i = 0; i < UP_COUNT; i++)
        if (x->tag == up_tags[i]) {
            uint32_t v;
            if (!set)
                return give32(x, a->procs[i]);
            if (take32(x, &v))
                a->procs[i] = v;
            return 0;
        }
    if (x->tag == TAG('u', 'i', 'd', 'p') && a->dialog) { /* the user item's procedure */
        uint32_t v;
        if (set && take32(x, &v)) {
            gaddr r = tb_NewPtr(8, false, true), t = tb_NewPtr(2, false, true), h = tb_NewPtr(4, false, true);
            tb_GetDialogItem(a->dialog, (int16_t)a->item, t, h, r);
            tb_SetDialogItem(a->dialog, (int16_t)a->item, (int16_t)rd16(t), v, r);
            tb_DisposePtr(h);
            tb_DisposePtr(t);
            tb_DisposePtr(r);
        }
        return 0;
    }
    return errDataNotSupported;
}

static uint32_t pane_features(ap_ctl *a)
{
    uint32_t f = a->features | kControlSupportsEmbedding | kControlSupportsDataAccess;
    if (a->procs[UP_FOCUS])
        f |= kControlSupportsFocus;
    if (a->procs[UP_IDLE])
        f |= kControlWantsIdle;
    if (a->procs[UP_ACTIVATE])
        f |= kControlWantsActivate;
    if (a->procs[UP_TRACK])
        f |= kControlHandlesTracking;
    if (a->procs[UP_BACK])
        f |= kControlHasSpecialBackground;
    return f;
}

/* ---------------------------------------------------------------------- */
/* CDEF 17: edit text, with a TERec of its own (or a dialog item's) */

static gaddr te_of(gaddr ch, ap_ctl *a)
{
    if (a->dialog)
        return dialog_item_te(a->dialog, a->item);
    return a->te;
}

/* TextEdit erases with the port's background: white, inside the field. */
typedef struct {
    uint16_t back[3];
} bk_saved;

static void white_bk(bk_saved *s)
{
    gaddr c = tb_NewPtr(6, false, false);
    tb_GetBackColor(c);
    for (int i = 0; i < 3; i++)
        s->back[i] = rd16(c + (uint32_t)i * 2);
    wr16(c, 0xFFFF);
    wr16(c + 2, 0xFFFF);
    wr16(c + 4, 0xFFFF);
    tb_RGBBackColor(c);
    tb_DisposePtr(c);
}

static void restore_bk(const bk_saved *s)
{
    gaddr c = tb_NewPtr(6, false, false);
    for (int i = 0; i < 3; i++)
        wr16(c + (uint32_t)i * 2, s->back[i]);
    tb_RGBBackColor(c);
    tb_DisposePtr(c);
}

static void edit_init(gaddr ch, ap_ctl *a)
{
    gaddr saved = qd_port();
    tb_SetPort(cowner(ch));
    ctl_font_saved fs;
    ctl_font_begin(ch, &fs);
    gaddr r = tmp_rect(crect(ch));
    a->te = tb_TENew(r, r);
    tb_DisposePtr(r);
    ctl_font_end(&fs);
    tb_SetPort(saved);
}

static void edit_draw(gaddr ch, ap_ctl *a)
{
    if (!cvisible(ch))
        return;
    qrect r = crect(ch);
    uint32_t state = draw_state(ch, 0);
    if (a->dialog) {
        dialog_draw_item(a->dialog, a->item);
    } else {
        ap_fill_grey(inset(r, -1), 0xFFFF);
        bk_saved bk;
        white_bk(&bk);
        gaddr rp = tmp_rect(r);
        if (a->te)
            tb_TEUpdate(rp, a->te);
        tb_DisposePtr(rp);
        restore_bk(&bk);
    }
    ap_well_frame(inset(r, -1), state);
    if (a->focused)
        ap_focus_rect(inset(r, -3), state != 0);
}

/* Bullets for a password's characters. */
static void password_show(ap_ctl *a)
{
    if (!a->te)
        return;
    gaddr bullets = tb_NewPtr(a->clear_len ? a->clear_len : 1, false, false);
    gmemset(bullets, 0xA5, (uint32_t)a->clear_len);
    tb_TESetText(bullets, a->clear_len, a->te);
    tb_DisposePtr(bullets);
}

static int32_t edit_key(gaddr ch, ap_ctl *a, gaddr rec)
{
    int16_t mods = (int16_t)rd16(rec), chr = (int16_t)rd16(rec + 4);
    gaddr te = te_of(ch, a);
    if (!te || a->locked)
        return 0;
    if (a->key_filter) {
        /* ControlKeyFilterProcPtr(ControlHandle, SInt16 *keyCode, SInt16 *charCode, SInt16 *modifiers) */
        uint32_t args[4] = { ch, rec + 2, rec + 4, rec };
        if (!mm_call(a->key_filter, 0x00003FE0, args, 4))
            return 0;
        chr = (int16_t)rd16(rec + 4);
        mods = (int16_t)rd16(rec);
    }
    bk_saved bk;
    white_bk(&bk);
    if ((var_of(ch) & 2) && !a->dialog) { /* a password: the real text kept, bullets shown */
        gaddr t = rd32(te);
        int s0 = TERec_selStart(t), s1 = TERec_selEnd(t);
        uint8_t c = (uint8_t)chr;
        bool del = c == 8, printable = c >= 32 && c != 127;
        if (del || printable) {
            if (s0 > a->clear_len) s0 = a->clear_len;
            if (s1 > a->clear_len) s1 = a->clear_len;
            if (del && s0 == s1 && s0 > 0)
                s0--;
            int add = printable ? 1 : 0;
            uint8_t *n = malloc((size_t)(a->clear_len - (s1 - s0) + add + 1));
            memcpy(n, a->clear, (size_t)s0);
            if (add)
                n[s0] = c;
            memcpy(n + s0 + add, a->clear + s1, (size_t)(a->clear_len - s1));
            free(a->clear);
            a->clear = n;
            a->clear_len = a->clear_len - (s1 - s0) + add;
            tb_TEKey(printable ? (int16_t)0xA5 : chr, te);
        } else {
            tb_TEKey(chr, te);
        }
    } else {
        tb_TEKey(chr, te);
    }
    restore_bk(&bk);
    (void)mods;
    if (a->validation) /* ControlEditTextValidationProcPtr(ControlHandle) */
        mm_call(a->validation, 0x000000C0, (uint32_t[]){ ch }, 1);
    return kControlEditTextPart;
}

static int32_t edit_focus(gaddr ch, ap_ctl *a, int16_t part)
{
    if (part == 0) {
        if (a->focused) {
            a->focused = false;
            gaddr te = te_of(ch, a);
            if (te && !a->dialog)
                tb_TEDeactivate(te);
            if (cvisible(ch))
                ap_focus_rect(inset(crect(ch), -3), false);
            if (cvisible(ch))
                ap_well_frame(inset(crect(ch), -1), draw_state(ch, 0));
        }
        return 0;
    }
    if (a->focused && part < 0)
        return 0; /* one part only: on to the next control */
    a->focused = true;
    if (a->dialog) {
        dialog_select_item_text(a->dialog, a->item, 0, 32767);
    } else if (a->te) {
        bk_saved bk;
        white_bk(&bk);
        tb_TESetSelect(0, 32767, a->te);
        tb_TEActivate(a->te);
        restore_bk(&bk);
    }
    if (cvisible(ch))
        ap_focus_rect(inset(crect(ch), -3), true);
    return kControlEditTextPart;
}

static int32_t edit_data(gaddr ch, ap_ctl *a, const access *x, bool set)
{
    gaddr te = te_of(ch, a);
    switch (x->tag) {
    case TAG('t', 'e', 'x', 't'): case TAG('f', 't', 'x', 't'): {
        bool pass = (var_of(ch) & 2) && !a->dialog;
        if (!set) {
            if (pass)
                return give(x, a->clear, a->clear_len);
            gaddr h = a->dialog ? 0 : te ? TERec_hText(rd32(te)) : 0;
            if (a->dialog) {
                gaddr ih = tb_NewPtr(4, false, true), t = tb_NewPtr(2, false, true), r = tb_NewPtr(8, false, true);
                tb_GetDialogItem(a->dialog, (int16_t)a->item, t, ih, r);
                h = rd32(ih);
                tb_DisposePtr(r);
                tb_DisposePtr(t);
                tb_DisposePtr(ih);
            }
            int32_t n = h && rd32(h) ? tb_GetHandleSize(h) : 0;
            uint8_t *buf = malloc((size_t)(n ? n : 1));
            if (n)
                gget(buf, rd32(h), (uint32_t)n);
            give(x, buf, n);
            free(buf);
            return 0;
        }
        int32_t n = x->size > 0 ? x->size : 0;
        if (a->dialog) {
            gaddr ih = tb_NewPtr(4, false, true), t = tb_NewPtr(2, false, true), r = tb_NewPtr(8, false, true);
            tb_GetDialogItem(a->dialog, (int16_t)a->item, t, ih, r);
            gaddr h = rd32(ih);
            if (h) {
                tb_SetHandleSize(h, n);
                if (n)
                    gmemcpy(rd32(h), x->data, (uint32_t)n);
                if (te && TERec_hText(rd32(te)) == h)
                    tb_TECalText(te);
            }
            tb_DisposePtr(r);
            tb_DisposePtr(t);
            tb_DisposePtr(ih);
        } else if (pass) {
            free(a->clear);
            a->clear = malloc((size_t)(n ? n : 1));
            if (n)
                gget(a->clear, x->data, (uint32_t)n);
            a->clear_len = n;
            password_show(a);
        } else if (te) {
            tb_TESetText(x->data, n, te);
        }
        break;
    }
    case TAG('t', 'h', 'a', 'n'):
        return set ? errDataNotSupported : give32(x, te);
    case TAG('s', 'e', 'l', 'e'): {
        if (!set) {
            int16_t s0 = te ? TERec_selStart(rd32(te)) : 0, s1 = te ? TERec_selEnd(rd32(te)) : 0;
            uint8_t b[4] = { (uint8_t)(s0 >> 8), (uint8_t)s0, (uint8_t)(s1 >> 8), (uint8_t)s1 };
            return give(x, b, 4);
        }
        if (x->size < 4)
            return errDataSizeMismatch;
        int16_t s0 = (int16_t)rd16(x->data), s1 = (int16_t)rd16(x->data + 2);
        if (a->dialog)
            dialog_select_item_text(a->dialog, a->item, s0, s1);
        else if (te) {
            bk_saved bk;
            white_bk(&bk);
            tb_TESetSelect(s0, s1, te);
            restore_bk(&bk);
        }
        return 0;
    }
    case TAG('p', 'a', 's', 's'):
        return set ? errDataNotSupported : give(x, a->clear, a->clear_len);
    case TAG('l', 'o', 'c', 'k'): {
        bool b;
        if (!set)
            return give_bool(x, a->locked);
        if (take_bool(x, &b))
            a->locked = b;
        return 0;
    }
    case TAG('f', 'l', 't', 'r'): {
        uint32_t v;
        if (!set)
            return give32(x, a->key_filter);
        if (take32(x, &v))
            a->key_filter = v;
        return 0;
    }
    case TAG('v', 'a', 'l', 'i'): {
        uint32_t v;
        if (!set)
            return give32(x, a->validation);
        if (take32(x, &v))
            a->validation = v;
        return 0;
    }
    case TAG('k', 's', 'c', 'r'): case TAG('p', 'r', 'u', 'p'): case TAG('p', 'o', 'u', 'p'):
        return set ? 0 : give32(x, 0);
    default:
        return errDataNotSupported;
    }
    if (cvisible(ch))
        tb_Draw1Control(ch);
    return 0;
}

/* ---------------------------------------------------------------------- */
/* CDEF 18: static text */

static void static_text(gaddr ch, ap_ctl *a, uint8_t **text, int32_t *len)
{
    *text = a->text;
    *len = a->text_len;
}

static void static_draw(gaddr ch, ap_ctl *a)
{
    if (!cvisible(ch))
        return;
    if (a->dialog) {
        dialog_draw_item(a->dialog, a->item);
        return;
    }
    ctl_font_saved fs;
    ctl_font_begin(ch, &fs);
    qrect r = crect(ch);
    uint8_t *t;
    int32_t n;
    static_text(ch, a, &t, &n);
    gaddr p = tb_NewPtr(n ? n : 1, false, false);
    if (n)
        gput(p, t, (uint32_t)n);
    gaddr rp = tmp_rect(r);
    uint16_t c[3];
    text_rgb(ch, chilite(ch) == 255 ? 2 : 1, c);
    ctl_info *ci = ctl_find(ch);
    int16_t just = ci && ci->has_font && (ci->font[1] & 0x40) ? (int16_t)(ci->font[10] << 8 | ci->font[11]) : 0;
    qd_set_fg_override(ap_in_color() ? c : NULL);
    tb_TETextBox(p, n, rp, just);
    qd_set_fg_override(NULL);
    tb_DisposePtr(rp);
    tb_DisposePtr(p);
    ctl_font_end(&fs);
}

static int32_t static_data(gaddr ch, ap_ctl *a, const access *x, bool set)
{
    switch (x->tag) {
    case TAG('t', 'e', 'x', 't'): {
        if (a->dialog) { /* the item's own text */
            gaddr ih = tb_NewPtr(4, false, true), t = tb_NewPtr(2, false, true), r = tb_NewPtr(8, false, true);
            tb_GetDialogItem(a->dialog, (int16_t)a->item, t, ih, r);
            gaddr h = rd32(ih);
            tb_DisposePtr(r);
            tb_DisposePtr(t);
            tb_DisposePtr(ih);
            if (!h)
                return errDataNotSupported;
            if (!set) {
                int32_t n = rd32(h) ? tb_GetHandleSize(h) : 0;
                uint8_t *buf = malloc((size_t)(n ? n : 1));
                if (n)
                    gget(buf, rd32(h), (uint32_t)n);
                give(x, buf, n);
                free(buf);
                return 0;
            }
            tb_SetHandleSize(h, x->size);
            if (x->size)
                gmemcpy(rd32(h), x->data, (uint32_t)x->size);
            break;
        }
        if (!set)
            return give(x, a->text, a->text_len);
        free(a->text);
        a->text_len = x->size > 0 ? x->size : 0;
        a->text = malloc((size_t)(a->text_len ? a->text_len : 1));
        if (a->text_len)
            gget(a->text, x->data, (uint32_t)a->text_len);
        break;
    }
    case TAG('t', 'h', 'e', 'i'): { /* the height of the text, wrapped in the rectangle */
        if (set)
            return errDataNotSupported;
        int asc, desc;
        gaddr saved = qd_port();
        tb_SetPort(cowner(ch));
        ctl_font_saved fs;
        ctl_font_begin(ch, &fs);
        ap_font_info(&asc, &desc);
        qrect r = crect(ch);
        int lines = 1, x0 = 0, w = r.right - r.left;
        for (int32_t i = 0; i < a->text_len; i++) {
            if (a->text[i] == '\r') {
                lines++;
                x0 = 0;
                continue;
            }
            x0 += qd_text_width(qd_port(), a->text + i, 1);
            if (x0 > w) {
                lines++;
                x0 = 0;
            }
        }
        ctl_font_end(&fs);
        tb_SetPort(saved);
        return give16(x, (int16_t)(lines * (asc + desc + 2)));
    }
    case TAG('t', 'r', 'u', 'n'):
        return set ? 0 : give16(x, -1);
    default:
        return errDataNotSupported;
    }
    if (cvisible(ch)) {
        gaddr saved = qd_port();
        tb_SetPort(cowner(ch));
        erase(crect(ch));
        tb_SetPort(saved);
        tb_Draw1Control(ch);
    }
    return 0;
}

/* ---------------------------------------------------------------------- */
/* CDEF 22: list boxes ('ldes': rows, columns, cell size, scroll bars, LDEF) */

static void list_init(gaddr ch, ap_ctl *a)
{
    int16_t id = cvalue(ch);
    int rows = 0, cols = 1, ch_h = 0, ch_w = 0, ldef = 0;
    bool vs = true, hs = false, grow = false;
    gaddr h = id ? tb_GetResource((int32_t)fourcc("ldes"), id) : 0;
    if (h && rd32(h) && tb_GetHandleSize(h) >= 18) {
        gaddr p = rd32(h);
        rows = (int16_t)rd16(p + 2);
        cols = (int16_t)rd16(p + 4);
        ch_h = (int16_t)rd16(p + 6);
        ch_w = (int16_t)rd16(p + 8);
        vs = rd8(p + 10) != 0;
        hs = rd8(p + 12) != 0;
        ldef = (int16_t)rd16(p + 14);
        grow = tb_GetHandleSize(h) >= 17 && rd8(p + 16) != 0;
    }
    qrect r = crect(ch);
    qrect view = r;
    if (vs)
        view.right = (int16_t)(view.right - 15);
    if (hs)
        view.bottom = (int16_t)(view.bottom - 15);
    gaddr saved = qd_port();
    gaddr w = cowner(ch);
    tb_SetPort(w);
    gaddr rv = tmp_rect(view), db = tmp_rect((qrect){ 0, 0, (int16_t)rows, (int16_t)(cols > 0 ? cols : 1) });
    a->list = tb_LNew(rv, db, make_pt((int16_t)ch_h, (int16_t)ch_w), (int16_t)ldef, w, true, grow, hs, vs);
    tb_DisposePtr(db);
    tb_DisposePtr(rv);
    tb_SetPort(saved);
    set_value_raw(ch, 0);
}

static void list_draw(gaddr ch, ap_ctl *a)
{
    if (!cvisible(ch))
        return;
    qrect r = crect(ch);
    ap_well_frame(inset(r, -1), draw_state(ch, 0));
    if (a->list) {
        tb_LActivate(chilite(ch) != 255, a->list);
        tb_LUpdate(GrafPort_visRgn(qd_port()), a->list);
    }
    if (a->focused)
        ap_focus_rect(inset(r, -3), true);
}

/* ---------------------------------------------------------------------- */
/* Pictures (CDEF 19), icons (CDEF 20), image wells (CDEF 11), clocks (CDEF 15) */

static void picture_init(gaddr ch, ap_ctl *a)
{
    a->content_type = kControlContentPictRes;
    a->content = tb_GetPicture(cvalue(ch));
    set_value_raw(ch, 0);
}

static void icon_init(gaddr ch, ap_ctl *a)
{
    int16_t id = cvalue(ch);
    int v = var_of(ch);
    if (v == 2 || v == 3)
        load_content(a, kControlContentIconSuiteRes, id);
    else {
        load_content(a, kControlContentCIconRes, id);
        if (!a->content) { /* a black-and-white ICON */
            a->content_type = 0;
            a->icon_id = id;
        }
    }
    set_value_raw(ch, 0);
}

static void icon_draw(gaddr ch, ap_ctl *a)
{
    if (!cvisible(ch))
        return;
    qrect r = crect(ch);
    if (a->content) {
        if (a->content_type == kControlContentIconSuiteRes) {
            gaddr rp = tmp_rect(r);
            tb_PlotIconSuite(rp, a->graphic_align, (int16_t)(chilite(ch) == 255 ? 1 : a->transform), a->content);
            tb_DisposePtr(rp);
        } else {
            draw_content(a, r, chilite(ch) == 255);
        }
    } else if (a->icon_id) {
        gaddr icon = tb_GetIcon(a->icon_id);
        if (icon) {
            gaddr rp = tmp_rect(r);
            tb_PlotIcon(rp, icon);
            tb_DisposePtr(rp);
        }
    }
}

static void well_draw(gaddr ch, ap_ctl *a)
{
    if (!cvisible(ch))
        return;
    qrect r = crect(ch);
    gaddr rp = tmp_rect(r);
    tb_DrawThemeGenericWell(rp, draw_state(ch, 0), true);
    tb_DisposePtr(rp);
    int w, h;
    content_size(a, &w, &h);
    fit_content(a, inset(r, 2), &w, &h);
    if (w)
        draw_content(a, place(inset(r, 2), w, h, 0, 0, 0), chilite(ch) == 255);
}

static void clock_draw(gaddr ch, ap_ctl *a)
{
    if (!cvisible(ch))
        return;
    qrect r = crect(ch);
    ap_fill_grey(r, 0xFFFF);
    ap_well_frame(inset(r, -1), draw_state(ch, 0));
    gaddr now = tb_NewPtr(4, false, true), s = tb_NewPtr(256, false, true);
    if (a->clock_secs && !(a->clock_flags & 2))
        wr32(now, a->clock_secs);
    else
        tb_GetDateTime(now);
    int v = var_of(ch);
    if (v == 2 || v == 3)
        tb_IUDateString((int32_t)rd32(now), 0, s);
    else
        tb_IUTimeString((int32_t)rd32(now), v == 1, s);
    uint8_t buf[256];
    int n = rd8(s);
    gget(buf, s + 1, (uint32_t)n);
    ctl_font_saved fs;
    ctl_font_begin(ch, &fs);
    uint16_t c[3];
    text_rgb(ch, chilite(ch) == 255 ? 2 : 1, c);
    ap_text_in(inset(r, 2), buf, n, false, c);
    ctl_font_end(&fs);
    tb_DisposePtr(s);
    tb_DisposePtr(now);
}

/* ---------------------------------------------------------------------- */
/* CDEF 27: scrolling text boxes (Appearance 1.1).  The text is a 'TEXT'
 * and 'styl' resource's in a styled TERec; the plain variant scrolls by
 * a live scroll bar of its own at the right, the auto-scrolling one by
 * itself at idle time, a few pixels after each delay, from the bottom
 * again once its text has gone by. */

typedef struct {
    gaddr te, bar;
    uint32_t delay_before, delay_between, start, last;
    int16_t amount, offset, height;
    bool still;
} scroll_text;

static scroll_text *st_of(ap_ctl *a) { return a ? (scroll_text *)a->scroll_text : NULL; }

static qrect st_view(gaddr ch, scroll_text *t)
{
    qrect v = inset(crect(ch), 1);
    if (t && t->bar)
        v.right = (int16_t)(v.right - 15);
    return v;
}

static int st_text_height(scroll_text *t)
{
    gaddr p = rd32(t->te);
    return tb_TEGetHeight((int32_t)TERec_nLines(p), 1, t->te);
}

/* Scroll so that pixel `offset` of the text is at the view's top. */
static void st_scroll_to(gaddr ch, scroll_text *t, int offset)
{
    if (!t->te || offset == t->offset)
        return;
    gaddr saved = qd_port();
    tb_SetPort(cowner(ch));
    bk_saved bk;
    white_bk(&bk);
    tb_TEScroll(0, (int16_t)(t->offset - offset), t->te);
    restore_bk(&bk);
    tb_SetPort(saved);
    t->offset = (int16_t)offset;
}

/* The scroll bar's action procedure: ControlActionProcPtr(control, part). */
static void st_bar_action(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    int16_t part = (int16_t)cpu_pop16(c);
    gaddr bar = cpu_pop32(c);
    gaddr box = ctl_find(bar) ? (gaddr)tb_GetControlReference(bar) : 0; /* the text box, the bar's reference */
    ap_ctl *a = ctl_find(box) ? ap_of(box) : NULL;
    scroll_text *t = a && a->id == 27 ? st_of(a) : NULL;
    if (t && t->te) {
        int line = TERec_lineHeight(rd32(t->te));
        qrect v = st_view(box, t);
        int page = (v.bottom - v.top) - line;
        int value = tb_GetControlValue(bar), max = tb_GetControlMaximum(bar);
        switch (part) {
        case kControlUpButtonPart: value -= line; break;
        case kControlDownButtonPart: value += line; break;
        case kControlPageUpPart: value -= page; break;
        case kControlPageDownPart: value += page; break;
        default: break; /* the thumb: the value is where it is */
        }
        if (value < 0)
            value = 0;
        if (value > max)
            value = max;
        if (part != kControlIndicatorPart)
            tb_SetControlValue(bar, (int16_t)value);
        st_scroll_to(box, t, value);
    }
    cpu_push32(c, ret);
}

static gaddr st_action_stub;

static void st_load(gaddr ch, ap_ctl *a, int16_t id)
{
    scroll_text *t = st_of(a);
    gaddr text = tb_GetResource((int32_t)fourcc("TEXT"), id);
    if (!text || !rd32(text))
        return;
    gaddr styl = tb_GetResource((int32_t)fourcc("styl"), id);
    gaddr saved = qd_port();
    tb_SetPort(cowner(ch));
    if (t->te)
        tb_TEDispose(t->te);
    qrect v = st_view(ch, t);
    gaddr dst = tmp_rect((qrect){ v.top, (int16_t)(v.left + 2), v.bottom, (int16_t)(v.right - 2) });
    gaddr view = tmp_rect(v);
    t->te = tb_TEStyleNew(dst, view);
    tb_DisposePtr(view);
    tb_DisposePtr(dst);
    tb_HLock(text);
    tb_TEStyleInsert(rd32(text), tb_GetHandleSize(text), styl && rd32(styl) ? styl : 0, t->te);
    tb_HUnlock(text);
    tb_TESetSelect(0, 0, t->te);
    tb_TEDeactivate(t->te);
    t->offset = 0;
    t->height = (int16_t)st_text_height(t);
    if (!t->bar)
        t->start = t->last = rd32(LM_Ticks);
    tb_SetPort(saved);
}

static void st_init(gaddr ch, ap_ctl *a)
{
    scroll_text *t = calloc(1, sizeof *t);
    a->scroll_text = t;
    t->delay_before = (uint32_t)(uint16_t)cmin(ch);
    t->delay_between = (uint32_t)(uint16_t)cmax(ch);
    t->amount = 1;
    int16_t id = cvalue(ch);
    set_value_raw(ch, 0);
    ControlRecord_set_contrlMin(cp(ch), 0);
    ControlRecord_set_contrlMax(cp(ch), 0);
    if (!(var_of(ch) & 1)) { /* the plain variant: a scroll bar inside, at the right */
        if (!st_action_stub)
            st_action_stub = native_register(st_bar_action, "scrolling text box", 0);
        qrect r = crect(ch);
        gaddr br = tmp_rect((qrect){ r.top, (int16_t)(r.right - 16), r.bottom, r.right });
        t->bar = tb_NewControl(cowner(ch), br, 0, cvisible(ch), 0, 0, 0, 386, (int32_t)ch);
        tb_DisposePtr(br);
        if (t->bar)
            tb_SetControlAction(t->bar, st_action_stub);
    }
    st_load(ch, a, id);
    if (t->bar && t->te) {
        qrect v = st_view(ch, t);
        int over = t->height - (v.bottom - v.top);
        tb_SetControlMaximum(t->bar, (int16_t)(over > 0 ? over : 0));
    }
}

static void st_draw(gaddr ch, ap_ctl *a)
{
    scroll_text *t = st_of(a);
    qrect r = crect(ch);
    ap_well_frame(r, draw_state(ch, 0));
    qrect v = st_view(ch, t);
    ap_fill_grey(v, 0xFFFF);
    if (t && t->te) {
        bk_saved bk;
        white_bk(&bk);
        gaddr vr = tmp_rect(v);
        tb_TEUpdate(vr, t->te);
        tb_DisposePtr(vr);
        restore_bk(&bk);
    }
}

static void st_idle(gaddr ch, ap_ctl *a)
{
    scroll_text *t = st_of(a);
    uint32_t now = rd32(LM_Ticks);
    if (!t || t->bar || !t->te || t->still || now - t->start < t->delay_before || now - t->last < t->delay_between)
        return;
    t->last = now;
    qrect v = st_view(ch, t);
    int next = t->offset + t->amount;
    if (next > t->height) /* gone by: in again from the bottom */
        next = -(v.bottom - v.top);
    if (cvisible(ch))
        st_scroll_to(ch, t, next);
    else
        t->offset = (int16_t)next;
}

static int32_t st_data(gaddr ch, ap_ctl *a, const access *x, bool set)
{
    scroll_text *t = st_of(a);
    if (!t)
        return errDataNotSupported;
    uint32_t v32;
    int16_t v16;
    bool b;
    switch (x->tag) {
    case TAG('s', 't', 'd', 'l'):
        if (!set)
            return give32(x, t->delay_before);
        if (take32(x, &v32))
            t->delay_before = v32;
        return 0;
    case TAG('s', 'c', 'd', 'l'):
        if (!set)
            return give32(x, t->delay_between);
        if (take32(x, &v32))
            t->delay_between = v32;
        return 0;
    case TAG('s', 'a', 'm', 't'):
        if (!set)
            return give16(x, t->amount);
        if (take16(x, &v16))
            t->amount = v16 > 0 ? v16 : 1;
        return 0;
    case TAG('t', 'r', 'e', 's'): /* write only */
        if (!set)
            return errDataNotSupported;
        if (!take16(x, &v16))
            return errDataSizeMismatch;
        st_load(ch, a, v16);
        if (cvisible(ch))
            tb_Draw1Control(ch);
        return 0;
    case TAG('a', 'n', 'i', 'm'):
        if (!set)
            return give_bool(x, !t->still && !t->bar);
        if (take_bool(x, &b))
            t->still = !b;
        return 0;
    default:
        return errDataNotSupported;
    }
}

static void st_dispose(gaddr ch, ap_ctl *a)
{
    scroll_text *t = st_of(a);
    if (!t)
        return;
    if (t->te)
        tb_TEDispose(t->te);
    if (t->bar && ctl_find(t->bar))
        tb_DisposeControl(t->bar);
    free(t);
    a->scroll_text = NULL;
}

/* ---------------------------------------------------------------------- */
/* The entry: FUNCTION (varCode, theControl, message, param) */

static uint32_t kind_features(gaddr ch, ap_ctl *a)
{
    const uint32_t act = kControlWantsActivate, data = kControlSupportsDataAccess, embed = kControlSupportsEmbedding;
    switch (a->id) {
    case 2: return act | data | kControlSupportsCalcBestRect;
    case 3: return act | kControlSupportsLiveFeedback;
    case 4: return act | data;
    case 5: return act | data | kControlWantsIdle;
    case 7: return kControlWantsIdle | data;
    case 8: return act | data | embed | kControlHasSpecialBackground;
    case 10: return act | data | embed;
    case 11: return act | data;
    case 14: case 21: return act | embed | kControlHasSpecialBackground;
    case 15: return act | data | kControlSupportsFocus | kControlGetsFocusOnClick | (a->clock_flags & 2 ? kControlWantsIdle : 0);
    case 16: return pane_features(a);
    case 17:
        return act | data | kControlSupportsFocus | kControlWantsIdle | kControlHandlesTracking |
               kControlGetsFocusOnClick | kControlSupportsCalcBestRect;
    case 18: return act | data | kControlSupportsCalcBestRect;
    case 19: case 20: return act | data;
    case 22: return act | data | kControlSupportsFocus | kControlHandlesTracking | kControlGetsFocusOnClick;
    case 23: return act | data | kControlSupportsCalcBestRect;
    case 26: return act | embed;
    case 27: return act | data | (var_of(ch) & 1 ? kControlWantsIdle : 0);
    default: return act;
    }
}

static void draw(gaddr ch, ap_ctl *a, int part)
{
    if (!cvisible(ch))
        return;
    ctl_font_saved fs;
    ctl_font_begin(ch, &fs);
    qrect r = crect(ch);
    switch (a->id) {
    case 2: bevel_draw(ch, a); break;
    case 3: draw_track(ch, 2); break;
    case 4:
        erase(r);
        if (a->behavior & AP_DRAWS_TITLE) { /* the triangle, its title after it */
            int top = r.top + (r.bottom - r.top - 12) / 2;
            qrect tri = { (int16_t)top, r.left, (int16_t)(top + 12), (int16_t)(r.left + 12) };
            ap_disclosure(tri, cvalue(ch) != 0, draw_state(ch, kControlTrianglePart), var_of(ch) & 1);
            uint8_t t[256];
            int n;
            title_of(ch, t, &n);
            uint16_t c[3];
            text_rgb(ch, chilite(ch) == 255 ? 2 : 1, c);
            ap_text_in((qrect){ r.top, (int16_t)(tri.right + 4), r.bottom, r.right }, t, n, false, c);
        } else {
            ap_disclosure(r, cvalue(ch) != 0, draw_state(ch, kControlTrianglePart), var_of(ch) & 1);
        }
        break;
    case 5: draw_track(ch, a->indeterminate ? 4 : 3); break;
    case 6: {
        uint8_t h = chilite(ch);
        ap_little_arrows(r, h == 255 ? 0 : h == kControlUpButtonPart ? 2 : h == kControlDownButtonPart ? 3 : 1);
        break;
    }
    case 7: {
        gaddr rp = tmp_rect(r);
        tb_DrawThemeChasingArrows(rp, a->phase, draw_state(ch, 0), 0, 0);
        tb_DisposePtr(rp);
        break;
    }
    case 8: tabs_draw(ch, a); break;
    case 9: erase(r); ap_separator(r, draw_state(ch, 0)); break;
    case 10: group_draw(ch); break;
    case 11: well_draw(ch, a); break;
    case 12: {
        static const int dirs[4] = { 2, 0, 3, 1 }; /* east, west, north, south */
        uint16_t c[3];
        ap_brush_rgb(chilite(ch) == 255 ? 22 : 20, c);
        erase(r);
        ap_arrow(r, dirs[var_of(ch) & 3], (var_of(ch) & 4) ? 9 : 17, c);
        break;
    }
    case 14: ap_placard(r, draw_state(ch, 0)); break;
    case 15: clock_draw(ch, a); break;
    case 16: pane_draw(ch, a, part); break;
    case 17: edit_draw(ch, a); break;
    case 18: static_draw(ch, a); break;
    case 19:
        if (a->dialog)
            dialog_draw_item(a->dialog, a->item);
        else
            draw_content(a, r, false);
        break;
    case 20:
        if (a->dialog)
            dialog_draw_item(a->dialog, a->item);
        else
            icon_draw(ch, a);
        break;
    case 21: {
        gaddr rp = tmp_rect(r);
        if (var_of(ch) & 1)
            tb_DrawThemeWindowListViewHeader(rp, draw_state(ch, 0));
        else
            tb_DrawThemeWindowHeader(rp, draw_state(ch, 0));
        tb_DisposePtr(rp);
        break;
    }
    case 22: list_draw(ch, a); break;
    case 23: ap_draw_standard(ch, standard_kind(ch)); break;
    case 27: st_draw(ch, a); break;
    case 26: { /* the radio buttons follow the group's value */
        gaddr subs[256];
        int n = ctl_subcontrols(ch, subs, 256);
        for (int i = 0; i < n; i++)
            ControlRecord_set_contrlValue(cp(subs[i]), (int16_t)(i + 1 == cvalue(ch)));
        break;
    }
    default: break;
    }
    ctl_font_end(&fs);
}

static int32_t test(gaddr ch, ap_ctl *a, uint32_t pt)
{
    if (!cvisible(ch) || chilite(ch) == 255)
        return 0;
    qrect r = crect(ch);
    switch (a->id) {
    case 2:
        if (!in_rect(r, pt))
            return 0;
        return a->menu ? kControlMenuPart : kControlButtonPart;
    case 3: return slider_test(ch, pt);
    case 4: return in_rect(r, pt) ? kControlTrianglePart : 0;
    case 6:
        if (!in_rect(r, pt))
            return 0;
        return pt_v(pt) < (r.top + r.bottom) / 2 ? kControlUpButtonPart : kControlDownButtonPart;
    case 8: return tabs_test(ch, a, pt);
    case 10: {
        int kind = var_of(ch) & 3;
        if (kind != 1 && !(kind == 2 && a->menu))
            return 0;
        ctl_font_saved fs;
        ctl_font_begin(ch, &fs);
        bool hit = in_rect(group_title(ch), pt);
        ctl_font_end(&fs);
        return hit ? (kind == 1 ? kControlCheckBoxPart : kControlMenuPart) : 0;
    }
    case 11: return in_rect(r, pt) ? kControlImageWellPart : 0;
    case 15: return in_rect(r, pt) ? kControlClockPart : 0;
    case 16:
        if (a->procs[UP_HIT]) {
            /* ControlUserPaneHitTestProcPtr: ControlPartCode (ControlHandle, Point) */
            return (int16_t)call_up(a, UP_HIT, ch, 0x000003E0, pt, 0, 2);
        }
        return 0;
    case 17: return in_rect(r, pt) ? kControlEditTextPart : 0;
    case 19: return in_rect(r, pt) ? kControlPicturePart : 0;
    case 20: return in_rect(r, pt) ? kControlIconPart : 0;
    case 22: return in_rect(r, pt) ? kControlListBoxPart : 0;
    case 23: return standard_test(ch, pt);
    default: return 0;
    }
}

static int32_t data(gaddr ch, ap_ctl *a, gaddr rec, bool set)
{
    access x = read_access(rec);
    if (x.tag == TAG('f', 'o', 'n', 't')) { /* every control: its font style */
        ctl_info *c = ctl_find(ch);
        if (!set)
            return c && c->has_font ? give(&x, c->font, 24) : give(&x, (uint8_t[24]){ 0 }, 24);
        if (x.size < 24)
            return errDataSizeMismatch;
        tb_SetControlFontStyle(ch, x.data);
        return 0;
    }
    switch (a->id) {
    case 2: return bevel_data(ch, a, &x, set);
    case 4:
        if (x.tag == TAG('l', 'a', 's', 't')) {
            int16_t v;
            if (!set)
                return give16(&x, a->last_value);
            if (take16(&x, &v))
                a->last_value = v;
            return 0;
        }
        return errDataNotSupported;
    case 5:
        if (x.tag == TAG('i', 'n', 'd', 'e')) {
            bool b;
            if (!set)
                return give_bool(&x, a->indeterminate);
            if (take_bool(&x, &b) && b != a->indeterminate) {
                a->indeterminate = b;
                if (cvisible(ch))
                    tb_Draw1Control(ch);
            }
            return 0;
        }
        /* fall through: 'anim' */
    case 7:
        if (x.tag == TAG('a', 'n', 'i', 'm')) { /* animating (the default) or still */
            bool b;
            if (!set)
                return give_bool(&x, !a->still);
            if (take_bool(&x, &b))
                a->still = !b;
            return 0;
        }
        return errDataNotSupported;
    case 8: return tabs_data(ch, a, &x, set);
    case 10:
        if (x.tag == TAG('t', 'r', 'e', 'c') && !set) {
            ctl_font_saved fs;
            ctl_font_begin(ch, &fs);
            qrect t = group_title(ch);
            ctl_font_end(&fs);
            uint8_t b[8] = { (uint8_t)(t.top >> 8), (uint8_t)t.top, (uint8_t)(t.left >> 8), (uint8_t)t.left,
                             (uint8_t)(t.bottom >> 8), (uint8_t)t.bottom, (uint8_t)(t.right >> 8), (uint8_t)t.right };
            return give(&x, b, 8);
        }
        if (x.tag == TAG('m', 'h', 'a', 'n') || x.tag == TAG('o', 'm', 'r', 'f')) {
            uint32_t v;
            if (!set)
                return give32(&x, a->menu);
            if (take32(&x, &v)) {
                if (a->owned_menu && a->menu && a->menu != v)
                    tb_DisposeMenu(a->menu);
                a->menu = v;
                a->owned_menu = x.tag == TAG('o', 'm', 'r', 'f');
                ControlRecord_set_contrlMin(cp(ch), 1);
                ControlRecord_set_contrlMax(cp(ch), (int16_t)(v ? tb_CountMItems(v) : 0));
                if (cvisible(ch))
                    tb_Draw1Control(ch);
            }
            return 0;
        }
        return errDataNotSupported;
    case 11: case 19: case 20:
        if (x.tag == TAG('c', 'o', 'n', 't')) {
            if (!set) {
                uint8_t info[6] = { (uint8_t)(a->content_type >> 8), (uint8_t)a->content_type, (uint8_t)(a->content >> 24),
                                    (uint8_t)(a->content >> 16), (uint8_t)(a->content >> 8), (uint8_t)a->content };
                return give(&x, info, 6);
            }
            if (x.size < 6)
                return errDataSizeMismatch;
            set_content_info(a, x.data);
        } else if (x.tag == TAG('t', 'r', 'a', 'n') || x.tag == TAG('t', 'r', 'f', 'm')) {
            int16_t v;
            if (!set)
                return give16(&x, a->transform);
            if (take16(&x, &v))
                a->transform = v;
        } else if (x.tag == TAG('a', 'l', 'g', 'n')) {
            int16_t v;
            if (!set)
                return give16(&x, a->graphic_align);
            if (take16(&x, &v))
                a->graphic_align = v;
        } else if (x.tag == TAG('d', 'r', 'a', 'g') && a->id == 11) {
            bool b;
            if (!set)
                return give_bool(&x, a->drag_dest);
            if (take_bool(&x, &b))
                a->drag_dest = b;
            return 0;
        } else if (x.tag == TAG('p', 'i', 'c', 'h') && a->id == 19) {
            uint32_t v;
            if (!set)
                return give32(&x, a->content_type == kControlContentPictHandle || a->content_type == kControlContentPictRes
                                      ? a->content : 0);
            if (!take32(&x, &v))
                return errDataSizeMismatch;
            a->content_type = kControlContentPictHandle;
            a->content = v; /* the program's (resource pictures are the Resource Manager's) */
        } else if (x.tag == TAG('i', 'r', 'e', 's') && a->id == 20) {
            int16_t v;
            if (!set)
                return give16(&x, a->icon_id);
            if (take16(&x, &v)) {
                load_content(a, kControlContentCIconRes, v);
                a->icon_id = v;
            }
        } else {
            return errDataNotSupported;
        }
        if (cvisible(ch))
            tb_Draw1Control(ch);
        return 0;
    case 15:
        if (x.tag == TAG('d', 'a', 't', 'e')) { /* LongDateRec */
            gaddr ld = tb_NewPtr(28, false, true), secs = tb_NewPtr(8, false, true);
            int32_t err = 0;
            if (!set) {
                if (a->clock_secs)
                    wr32(secs + 4, a->clock_secs);
                else
                    tb_GetDateTime(secs + 4);
                tb_LongSecondsToDate(secs, ld);
                uint8_t b[28];
                gget(b, ld, 28);
                give(&x, b, 28);
            } else if (x.size < 28) {
                err = errDataSizeMismatch;
            } else {
                gmemcpy(ld, x.data, 28);
                tb_LongDateToSeconds(ld, secs);
                a->clock_secs = rd32(secs + 4);
                if (cvisible(ch))
                    tb_Draw1Control(ch);
            }
            tb_DisposePtr(secs);
            tb_DisposePtr(ld);
            return err;
        }
        if (x.tag == TAG('a', 'n', 'i', 'm')) {
            bool b;
            if (!set)
                return give_bool(&x, (a->clock_flags & 2) && !a->still);
            if (take_bool(&x, &b))
                a->still = !b;
            return 0;
        }
        return errDataNotSupported;
    case 16: return pane_data(ch, a, &x, set);
    case 27: return st_data(ch, a, &x, set);
    case 17: return edit_data(ch, a, &x, set);
    case 18: return static_data(ch, a, &x, set);
    case 22:
        switch (x.tag) {
        case TAG('l', 'h', 'a', 'n'): {
            /* A list the program made (CreateCustomList) in place of the
             * control's: the old one is the program's to dispose of, as
             * iView MediaPro does before it sets the new one. */
            uint32_t v;
            if (!set)
                return give32(&x, a->list);
            if (!take32(&x, &v))
                return errDataSizeMismatch;
            a->list = v;
            if (cvisible(ch))
                tb_Draw1Control(ch);
            return 0;
        }
        case TAG('d', 'b', 'l', 'c'): return set ? errDataNotSupported : give_bool(&x, a->dbl);
        case TAG('f', 'l', 't', 'r'): {
            uint32_t v;
            if (!set)
                return give32(&x, a->key_filter);
            if (take32(&x, &v))
                a->key_filter = v;
            return 0;
        }
        case TAG('l', 'd', 'e', 'f'): return 0;
        default: return errDataNotSupported;
        }
    case 23:
        if (x.tag == TAG('d', 'f', 'l', 't') || x.tag == TAG('c', 'n', 'c', 'l')) {
            bool *flag = x.tag == TAG('d', 'f', 'l', 't') ? &a->is_default : &a->is_cancel;
            bool b;
            if (!set)
                return give_bool(&x, *flag);
            if (take_bool(&x, &b) && b != *flag) {
                *flag = b;
                if (cvisible(ch) && flag == &a->is_default) {
                    if (!b) { /* the ring's area back to the background */
                        gaddr saved = qd_port();
                        tb_SetPort(cowner(ch));
                        erase(inset(crect(ch), -4));
                        tb_SetPort(saved);
                    }
                    tb_Draw1Control(ch);
                }
            }
            return 0;
        }
        return errDataNotSupported;
    default:
        return errDataNotSupported;
    }
}

/* The best size: the title's width and the standard height. */
static void best_rect(gaddr ch, ap_ctl *a, gaddr rec)
{
    ctl_font_saved fs;
    ctl_font_begin(ch, &fs);
    uint8_t t[256];
    int n;
    title_of(ch, t, &n);
    int asc, desc;
    ap_font_info(&asc, &desc);
    int tw = qd_text_width(qd_port(), t, n);
    int h = 20, w = tw + 24, base = 0;
    switch (a->id) {
    case 23:
        if (standard_kind(ch) == 0) {
            w = tw + 24 < 70 ? 70 : tw + 24;
            base = -((20 - asc - desc) / 2 + desc);
        } else {
            h = 16;
            w = tw + 20;
        }
        break;
    case 18: {
        h = asc + desc + 2;
        w = 0;
        int x = 0;
        for (int32_t i = 0; i < a->text_len; i++) {
            if (a->text[i] == '\r') {
                h += asc + desc + 2;
                x = 0;
                continue;
            }
            x += qd_text_width(qd_port(), a->text + i, 1);
            if (x > w)
                w = x;
        }
        break;
    }
    case 17: h = asc + desc + 2; w = (int16_t)rd16(rec + 2); break;
    default: break;
    }
    ctl_font_end(&fs);
    wr16(rec, (uint16_t)h);
    wr16(rec + 2, (uint16_t)w);
    wr16(rec + 4, (uint16_t)base);
}

static void idle(gaddr ch, ap_ctl *a)
{
    uint32_t now = rd32(LM_Ticks);
    switch (a->id) {
    case 27: st_idle(ch, a); break;
    case 15: /* a live clock: each second */
        if ((a->clock_flags & 2) && !a->still && now - a->last_tick >= 60) {
            a->last_tick = now;
            if (cvisible(ch))
                draw(ch, a, 0);
        }
        break;
    case 5: case 7:
        if ((a->id == 7 || a->indeterminate) && !a->still && now - a->last_tick >= 4) {
            a->last_tick = now;
            a->phase++;
            if (cvisible(ch))
                draw(ch, a, 0);
        }
        break;
    case 16:
        if (a->procs[UP_IDLE])
            call_up(a, UP_IDLE, ch, 0x000000C0, 0, 0, 1);
        break;
    case 17: {
        gaddr te = te_of(ch, a);
        if (a->focused && te && !a->dialog) {
            bk_saved bk;
            white_bk(&bk);
            tb_TEIdle(te);
            restore_bk(&bk);
        }
        break;
    }
    default: break;
    }
}

static void init(gaddr ch, ap_ctl *a)
{
    switch (a->id) {
    case 2: bevel_init(ch, a); break;
    case 8: tabs_init(ch, a); break;
    case 11:
        load_content(a, cmin(ch), cvalue(ch));
        set_value_raw(ch, 0);
        ControlRecord_set_contrlMin(cp(ch), 0);
        break;
    case 16:
        a->features = (uint32_t)(uint16_t)cvalue(ch);
        set_value_raw(ch, 0);
        break;
    case 15: /* the clock's flags come as its value */
        a->clock_flags = (uint32_t)(uint16_t)cvalue(ch);
        set_value_raw(ch, 0);
        break;
    case 17: edit_init(ch, a); break;
    case 19: picture_init(ch, a); break;
    case 20: icon_init(ch, a); break;
    case 22: list_init(ch, a); break;
    case 27: st_init(ch, a); break;
    case 23:
        if ((var_of(ch) & 7) >= 6) { /* a push button with an icon: its cicn from the maximum */
            a->icon_id = cmax(ch);
            ControlRecord_set_contrlMax(cp(ch), 1);
        }
        break;
    default: break;
    }
}

static void dispose(gaddr ch, ap_ctl *a)
{
    st_dispose(ch, a);
    if (a->owned_menu && a->menu)
        tb_DisposeMenu(a->menu);
    if (a->id != 19)
        dispose_content(a);
    if (a->te && !a->dialog)
        tb_TEDispose(a->te);
    if (a->list)
        tb_LDispose(a->list);
    free(a->text);
    free(a->clear);
    free(a->tabs);
}

static void entry(cpu68k *c, int id)
{
    uint32_t ret = cpu_pop32(c);
    int32_t param = (int32_t)cpu_pop32(c);
    int16_t msg = (int16_t)cpu_pop16(c);
    gaddr ch = cpu_pop32(c);
    cpu_pop16(c);
    int32_t result = 0;
    ctl_info *ci = ctl_find(ch);
    if (ci && !ci->ap && msg == initCntl) {
        ap_ctl *a = calloc(1, sizeof *a);
        a->id = id;
        ci->ap = a;
    }
    ap_ctl *a = ci ? (ap_ctl *)ci->ap : NULL;
    if (!a) {
        wr32(c->a[7], 0);
        cpu_push32(c, ret);
        return;
    }
    switch (msg) {
    case drawCntl: draw(ch, a, param & 0xFF); break;
    case testCntl: result = test(ch, a, (uint32_t)param); break;
    case calcCRgns: case 10: { /* calcCntlRgn: the control's rectangle */
        hrgn g;
        hrgn_init(&g);
        hrgn_rect(&g, crect(ch));
        rgn_write((gaddr)param & 0x7FFFFFFF, &g);
        hrgn_free(&g);
        break;
    }
    case initCntl: init(ch, a); break;
    case dispCntl: dispose(ch, a); break;
    case posCntl:
        if (a->te && !a->dialog) {
            qrect r = crect(ch);
            gaddr t = rd32(a->te);
            qrect_put(t + TERec_destRect_off, r);
            qrect_put(t + TERec_viewRect_off, r);
        }
        break;
    case kControlMsgCalcBestRect: best_rect(ch, a, (gaddr)param); break;
    case kControlMsgFocus:
        if (a->id == 17)
            result = edit_focus(ch, a, (int16_t)param);
        else if (a->id == 16 && a->procs[UP_FOCUS])
            result = (int16_t)call_up(a, UP_FOCUS, ch, 0x000002E0, (uint32_t)(uint16_t)param, 0, 2);
        else if (a->id == 22 || a->id == 15) {
            bool was = a->focused;
            a->focused = param != 0 && !(was && param < 0);
            if (cvisible(ch))
                ap_focus_rect(inset(crect(ch), -3), a->focused);
            result = a->focused ? (a->id == 22 ? kControlListBoxPart : kControlClockPart) : 0;
        }
        break;
    case kControlMsgKeyDown:
        if (a->id == 17)
            result = edit_key(ch, a, (gaddr)param);
        else if (a->id == 16 && a->procs[UP_KEY])
            result = (int16_t)mm_call(a->procs[UP_KEY], 0x00002AE0,
                                      (uint32_t[4]){ ch, rd16((gaddr)param + 2), rd16((gaddr)param + 4), rd16((gaddr)param) }, 4);
        else if (a->id == 22)
            result = kControlListBoxPart;
        break;
    case kControlMsgIdle: idle(ch, a); break;
    case kControlMsgGetFeatures: result = (int32_t)kind_features(ch, a); break;
    case kControlMsgSetData: result = data(ch, a, (gaddr)param, true); break;
    case kControlMsgGetData: result = data(ch, a, (gaddr)param, false); break;
    case kControlMsgActivate:
        if (a->id == 16 && a->procs[UP_ACTIVATE])
            call_up(a, UP_ACTIVATE, ch, 0x000001C0, (uint32_t)(param != 0), 0, 2);
        else if (a->id == 17 && a->focused && !a->dialog && a->te) {
            if (param)
                tb_TEActivate(a->te);
            else
                tb_TEDeactivate(a->te);
        }
        break;
    case kControlMsgSetUpBackground:
        if (a->id == 16 && a->procs[UP_BACK])
            call_up(a, UP_BACK, ch, 0x000003C0, (uint32_t)param, 0, 2);
        else if (a->id == 8 || a->id == 14 || a->id == 21)
            tb_SetThemeBackground(25, (int16_t)rd16((gaddr)param), (int8_t)rd8((gaddr)param + 2));
        break;
    case kControlMsgTestNewMsgSupport: result = NEW_MSG_OK; break;
    case kControlMsgSubControlAdded: case kControlMsgSubControlRemoved:
        if (a->id == 26) { /* a radio group counts its buttons */
            gaddr subs[256];
            int n = ctl_subcontrols(ch, subs, 256);
            ControlRecord_set_contrlMin(cp(ch), 0);
            ControlRecord_set_contrlMax(cp(ch), (int16_t)n);
            for (int i = 0; i < n; i++)
                if (cvalue(subs[i]) == 1)
                    set_value_raw(ch, (int16_t)(i + 1));
        }
        break;
    default: break;
    }
    wr32(c->a[7], (uint32_t)result);
    cpu_push32(c, ret);
}

#define AP_CDEF(n) \
    static void cdef##n##_ap(cpu68k *c, uint16_t tw) { entry(c, n); }
AP_CDEF(2)
AP_CDEF(3)
AP_CDEF(4)
AP_CDEF(5)
AP_CDEF(6)
AP_CDEF(7)
AP_CDEF(8)
AP_CDEF(9)
AP_CDEF(10)
AP_CDEF(11)
AP_CDEF(12)
AP_CDEF(14)
AP_CDEF(15)
AP_CDEF(16)
AP_CDEF(17)
AP_CDEF(18)
AP_CDEF(19)
AP_CDEF(20)
AP_CDEF(21)
AP_CDEF(22)
AP_CDEF(23)
AP_CDEF(26)
AP_CDEF(27)

void cdef24_native(cpu68k *c, uint16_t tw);
void cdef25_native(cpu68k *c, uint16_t tw);

const ap_cdef_stub ap_cdef_stubs[] = {
    { 2, cdef2_ap },   { 3, cdef3_ap },   { 4, cdef4_ap },   { 5, cdef5_ap },   { 6, cdef6_ap },
    { 7, cdef7_ap },   { 8, cdef8_ap },   { 9, cdef9_ap },   { 10, cdef10_ap }, { 11, cdef11_ap },
    { 12, cdef12_ap }, { 14, cdef14_ap }, { 15, cdef15_ap }, { 16, cdef16_ap }, { 17, cdef17_ap },
    { 18, cdef18_ap }, { 19, cdef19_ap }, { 20, cdef20_ap }, { 21, cdef21_ap }, { 22, cdef22_ap },
    { 23, cdef23_ap }, { 24, cdef24_native }, { 25, cdef25_native }, { 26, cdef26_ap }, { 27, cdef27_ap },
};
const int ap_cdef_stub_count = sizeof ap_cdef_stubs / sizeof ap_cdef_stubs[0];

/* ---------------------------------------------------------------------- */
/* Tracking and what follows a click */

bool ap_ctl_track(gaddr ch, int16_t part, uint32_t pt, gaddr action, int16_t mods, int16_t *result)
{
    ap_ctl *a = ap_of(ch);
    if (!a || !part)
        return false;
    switch (a->id) {
    case 2:
        if (part != kControlMenuPart || !a->menu)
            return false;
        *result = bevel_menu(ch, a);
        return true;
    case 3:
        if (part != kControlIndicatorPart)
            return false;
        *result = slider_drag(ch, pt, action);
        return true;
    case 8:
        *result = tabs_track(ch, a, part);
        return true;
    case 10: {
        if (part != kControlMenuPart || !a->menu)
            return false;
        ctl_font_saved fs;
        ctl_font_begin(ch, &fs);
        qrect t = group_title(ch);
        ctl_font_end(&fs);
        gaddr gp = tb_NewPtr(4, false, true);
        wr32(gp, make_pt(t.top, t.left));
        tb_LocalToGlobal(gp);
        uint32_t g = rd32(gp);
        tb_DisposePtr(gp);
        int items = tb_CountMItems(a->menu);
        for (int i = 1; i <= items; i++)
            tb_SetItemMark(a->menu, (int16_t)i, i == cvalue(ch) ? 0x12 : 0);
        tb_HiliteControl(ch, kControlMenuPart);
        int32_t r = tb_PopUpMenuSelect(a->menu, (int16_t)pt_v(g), (int16_t)pt_h(g), cvalue(ch));
        tb_HiliteControl(ch, 0);
        *result = 0;
        if (r && (int16_t)(r & 0xFFFF) > 0) {
            tb_SetControlValue(ch, (int16_t)(r & 0xFFFF));
            *result = kControlMenuPart;
        }
        return true;
    }
    case 16:
        if (!a->procs[UP_TRACK])
            return false;
        /* ControlUserPaneTrackingProcPtr: ControlPartCode (ControlHandle, Point, ControlActionUPP) */
        *result = (int16_t)mm_call(a->procs[UP_TRACK], 0x00000FE0, (uint32_t[3]){ ch, pt, action }, 3);
        return true;
    case 17: {
        gaddr te = te_of(ch, a);
        if (te) {
            gaddr saved = qd_port();
            tb_SetPort(cowner(ch));
            bk_saved bk;
            white_bk(&bk);
            if (a->dialog)
                dialog_select_item_text(a->dialog, a->item, TERec_selStart(rd32(te)), TERec_selEnd(rd32(te)));
            tb_TEClick(pt, (mods & 0x0200) != 0, te);
            restore_bk(&bk);
            tb_SetPort(saved);
        }
        *result = kControlEditTextPart;
        return true;
    }
    case 19: case 20:
        if (!(var_of(ch) & 1))
            return false;
        *result = part; /* the no-track variants answer at once */
        return true;
    case 22:
        if (!a->list)
            return false;
        a->dbl = tb_LClick(pt, mods, a->list) != 0;
        *result = kControlListBoxPart;
        return true;
    default:
        return false;
    }
}

/* Radio buttons in a radio group: the one clicked is on, the others off. */
static void radio_in_group(gaddr ch)
{
    ctl_info *c = ctl_find(ch);
    ap_ctl *g = c ? ap_of(c->parent) : NULL;
    if (!g || g->id != 26)
        return;
    gaddr subs[256];
    int n = ctl_subcontrols(c->parent, subs, 256);
    for (int i = 0; i < n; i++) {
        if (subs[i] == ch)
            set_value_raw(c->parent, (int16_t)(i + 1));
        int16_t want = subs[i] == ch;
        if (cvalue(subs[i]) != want)
            tb_SetControlValue(subs[i], want);
    }
}

void ap_after_track(gaddr ch, int16_t part)
{
    if (!part)
        return;
    ctl_info *c = ctl_find(ch);
    if (!c)
        return;
    ap_ctl *a = c->ap;
    bool radio = (c->cdef == 0 && (c->var & 3) == 2) || (c->cdef == 23 && ((c->var & 7) == 2 || (c->var & 7) == 4));
    if (radio)
        radio_in_group(ch);
    if (!a)
        return;
    switch (a->id) {
    case 2:
        if (part == kControlButtonPart || (part == kControlMenuPart && !a->menu)) {
            if (a->behavior & 0x0100)
                tb_SetControlValue(ch, (int16_t)!cvalue(ch));
            else if (a->behavior & 0x0200)
                tb_SetControlValue(ch, 1);
        }
        break;
    case 4:
        if (var_of(ch) & 2)
            tb_SetControlValue(ch, (int16_t)!cvalue(ch));
        break;
    case 10: /* a check box group box made to toggle (CreateCheckGroupBoxControl) */
        if (part == kControlCheckBoxPart && (a->behavior & AP_AUTO_TOGGLE))
            tb_SetControlValue(ch, (int16_t)!cvalue(ch));
        break;
    case 23:
        if ((var_of(ch) & 7) == 3)
            tb_SetControlValue(ch, (int16_t)(cvalue(ch) == 1 ? 0 : 1));
        else if ((var_of(ch) & 7) == 4)
            tb_SetControlValue(ch, 1);
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------------- */
/* Dialog items' stand-ins */

static struct {
    gaddr dialog, ch;
    int item;
} stand_ins[256];

gaddr ap_item_control(gaddr d, int item, int kind)
{
    for (int i = 0; i < 256; i++)
        if (stand_ins[i].dialog == d && stand_ins[i].item == item && stand_ins[i].ch && rd32(stand_ins[i].ch))
            return stand_ins[i].ch;
    int16_t proc = kind == statText ? 288 : kind == editText ? 272 : kind == iconItem ? 320 : kind == picItem ? 304 : 256;
    gaddr r = tb_NewPtr(8, false, true), t = tb_NewPtr(2, false, true), h = tb_NewPtr(4, false, true);
    tb_GetDialogItem(d, (int16_t)item, t, h, r);
    /* Out of the window's list, so that DrawControls and FindControl leave
     * the item to the Dialog Manager. */
    gaddr ch = tb_NewControl(d, r, 0, 0, 0, 0, 0, proc, 0);
    tb_DisposePtr(h);
    tb_DisposePtr(t);
    tb_DisposePtr(r);
    if (!ch)
        return 0;
    gaddr prev = 0;
    for (gaddr x = WindowRecord_controlList(d); x; prev = x, x = ControlRecord_nextControl(rd32(x)))
        if (x == ch) {
            if (prev)
                ControlRecord_set_nextControl(rd32(prev), ControlRecord_nextControl(rd32(ch)));
            else
                WindowRecord_set_controlList(d, ControlRecord_nextControl(rd32(ch)));
            break;
        }
    ControlRecord_set_nextControl(rd32(ch), 0);
    ControlRecord_set_contrlVis(rd32(ch), 255);
    ctl_info *ci = ctl_find(ch);
    if (ci)
        ci->parent = 0; /* nor drawn with the root control */
    ap_ctl *a = ap_of(ch);
    if (a) {
        if (a->te) {
            tb_TEDispose(a->te);
            a->te = 0;
        }
        dispose_content(a);
        a->dialog = d;
        a->item = item;
    }
    for (int i = 0; i < 256; i++)
        if (!stand_ins[i].ch || !rd32(stand_ins[i].ch) || (stand_ins[i].dialog == d && stand_ins[i].item == item)) {
            stand_ins[i].dialog = d;
            stand_ins[i].item = item;
            stand_ins[i].ch = ch;
            break;
        }
    return ch;
}

/* The dialog and item a stand-in control is for, if ch is one. */
bool ap_stand_in_item(gaddr ch, gaddr *dialog, int *item)
{
    for (int i = 0; ch && i < 256; i++)
        if (stand_ins[i].ch == ch && stand_ins[i].dialog) {
            *dialog = stand_ins[i].dialog;
            *item = stand_ins[i].item;
            return true;
        }
    return false;
}

/* The dialog goes: its stand-ins with it. */
void ap_forget_dialog(gaddr d)
{
    for (int i = 0; i < 256; i++)
        if (stand_ins[i].dialog == d) {
            gaddr ch = stand_ins[i].ch;
            stand_ins[i].dialog = 0;
            stand_ins[i].ch = 0;
            if (ch && rd32(ch))
                tb_DisposeControl(ch);
        }
}

/* Items from `from` on moved by delta (InsertDialogItem, RemoveDialogItems):
 * their stand-ins follow, and those of items removed (from - delta up to
 * from, for a negative delta) go. */
void ap_items_shifted(gaddr d, int from, int delta)
{
    for (int i = 0; i < 256; i++) {
        if (stand_ins[i].dialog != d || !stand_ins[i].ch || !rd32(stand_ins[i].ch))
            continue;
        int n = stand_ins[i].item;
        if (delta < 0 && n >= from + delta && n < from) {
            gaddr ch = stand_ins[i].ch;
            stand_ins[i].dialog = 0;
            stand_ins[i].ch = 0;
            tb_DisposeControl(ch);
            continue;
        }
        if (n >= from) {
            stand_ins[i].item = n + delta;
            ap_ctl *a = ap_of(stand_ins[i].ch);
            if (a)
                a->item = n + delta;
        }
    }
}

/* A user item's stand-in with a drawing procedure draws it. */
bool ap_item_draw(gaddr d, int item)
{
    for (int i = 0; i < 256; i++)
        if (stand_ins[i].dialog == d && stand_ins[i].item == item && stand_ins[i].ch && rd32(stand_ins[i].ch)) {
            ap_ctl *a = ap_of(stand_ins[i].ch);
            if (a && a->id == 16 && a->procs[UP_DRAW]) {
                call_up(a, UP_DRAW, stand_ins[i].ch, 0x000002C0, 0, 0, 2);
                return true;
            }
        }
    return false;
}

/* A click in a user item whose stand-in tracks or hit-tests: its part. */
int16_t ap_item_click(gaddr d, int item, uint32_t pt, int16_t mods)
{
    for (int i = 0; i < 256; i++)
        if (stand_ins[i].dialog == d && stand_ins[i].item == item && stand_ins[i].ch && rd32(stand_ins[i].ch)) {
            gaddr ch = stand_ins[i].ch;
            ap_ctl *a = ap_of(ch);
            if (!a || a->id != 16)
                return -1;
            int16_t part = a->procs[UP_HIT] ? (int16_t)call_up(a, UP_HIT, ch, 0x000003E0, pt, 0, 2) : 1;
            if (part && a->procs[UP_TRACK])
                part = (int16_t)mm_call(a->procs[UP_TRACK], 0x00000FE0, (uint32_t[3]){ ch, pt, 0 }, 3);
            return part;
        }
    return -1;
}

void ap_cdefs_reset(void) { memset(stand_ins, 0, sizeof stand_ins); }

bool ap_is_stand_in(gaddr ch)
{
    ap_ctl *a = ap_of(ch);
    return a && a->dialog;
}

/* ---------------------------------------------------------------------- */
/* AppearanceLib's helpers: SetControlData and GetControlData by another name */

static int16_t set_data(gaddr ch, int16_t part, uint32_t tag, const void *bytes, int32_t n)
{
    gaddr p = tb_NewPtr(n, false, false);
    gput(p, bytes, (uint32_t)n);
    int16_t err = tb_SetControlData(ch, part, (int32_t)tag, n, p);
    tb_DisposePtr(p);
    return err;
}

static int16_t set_data16(gaddr ch, int16_t part, uint32_t tag, int16_t v)
{
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    return set_data(ch, part, tag, b, 2);
}

int16_t tb_GetBevelButtonMenuValue(gaddr ch, gaddr out)
{
    return tb_GetControlData(ch, 0, (int32_t)TAG('m', 'v', 'a', 'l'), 2, out, 0);
}

int16_t tb_SetBevelButtonMenuValue(gaddr ch, int16_t v) { return set_data16(ch, 0, TAG('m', 'v', 'a', 'l'), v); }

int16_t tb_GetBevelButtonMenuHandle(gaddr ch, gaddr out)
{
    return tb_GetControlData(ch, 0, (int32_t)TAG('m', 'h', 'n', 'd'), 4, out, 0);
}

int16_t tb_GetBevelButtonContentInfo(gaddr ch, gaddr out)
{
    return tb_GetControlData(ch, 0, (int32_t)TAG('c', 'o', 'n', 't'), 6, out, 0);
}

int16_t tb_SetBevelButtonContentInfo(gaddr ch, gaddr info)
{
    return tb_SetControlData(ch, 0, (int32_t)TAG('c', 'o', 'n', 't'), 6, info);
}

int16_t tb_SetBevelButtonTransform(gaddr ch, int16_t t) { return set_data16(ch, 0, TAG('t', 'r', 'a', 'n'), t); }

int16_t tb_SetBevelButtonGraphicAlignment(gaddr ch, int16_t align, int16_t h, int16_t v)
{
    int16_t err = set_data16(ch, 0, TAG('g', 'a', 'l', 'i'), align);
    uint8_t b[4] = { (uint8_t)(v >> 8), (uint8_t)v, (uint8_t)(h >> 8), (uint8_t)h };
    return err ? err : set_data(ch, 0, TAG('g', 'o', 'f', 'f'), b, 4);
}

int16_t tb_SetBevelButtonTextAlignment(gaddr ch, int16_t align, int16_t offset)
{
    int16_t err = set_data16(ch, 0, TAG('t', 'a', 'l', 'i'), align);
    return err ? err : set_data16(ch, 0, TAG('t', 'o', 'f', 'f'), offset);
}

int16_t tb_SetBevelButtonTextPlacement(gaddr ch, int16_t where) { return set_data16(ch, 0, TAG('t', 'p', 'l', 'c'), where); }

int16_t tb_SetImageWellContentInfo(gaddr ch, gaddr info)
{
    return tb_SetControlData(ch, 0, (int32_t)TAG('c', 'o', 'n', 't'), 6, info);
}

int16_t tb_SetImageWellTransform(gaddr ch, int16_t t) { return set_data16(ch, 0, TAG('t', 'r', 'a', 'n'), t); }

int16_t tb_SetTabEnabled(gaddr ch, int16_t tab, int8_t enabled)
{
    uint8_t b = enabled != 0;
    return set_data(ch, tab, TAG('e', 'n', 'a', 'b'), &b, 1);
}

int16_t tb_SetDisclosureTriangleLastValue(gaddr ch, int16_t v) { return set_data16(ch, 0, TAG('l', 'a', 's', 't'), v); }

/* ---------------------------------------------------------------------- */
/* CarbonLib's Create...Control (carbon_create.c): what its arguments set
 * that NewControl's don't */

void ap_set_content(gaddr ch, gaddr info)
{
    ap_ctl *a = ap_of(ch);
    if (a && info)
        set_content_info(a, info);
}

void ap_set_behavior(gaddr ch, int16_t bits)
{
    ap_ctl *a = ap_of(ch);
    if (a)
        a->behavior = (int16_t)(a->behavior | bits);
}

void ap_set_ticks(gaddr ch, int n)
{
    ap_ctl *a = ap_of(ch);
    if (a)
        a->ticks = (int16_t)n;
}

/* A list box's list made anew to the arguments given: its rows, columns
 * and cell size, its scroll bars and grow box, and its definition (LDEF
 * 0, or a program's own through its UPP). */
bool ap_list_setup(gaddr ch, int rows, int cols, int cell_h, int cell_w, bool hs, bool vs, bool grow, gaddr ldef_upp)
{
    ap_ctl *a = ap_of(ch);
    if (!a || a->id != 22)
        return false;
    if (a->list)
        tb_LDispose(a->list);
    qrect view = crect(ch);
    if (vs)
        view.right = (int16_t)(view.right - 15);
    if (hs)
        view.bottom = (int16_t)(view.bottom - 15);
    gaddr saved = qd_port();
    gaddr w = cowner(ch);
    tb_SetPort(w);
    gaddr rv = tmp_rect(view), db = tmp_rect((qrect){ 0, 0, (int16_t)rows, (int16_t)(cols > 0 ? cols : 1) });
    a->list = tb_LNew(rv, db, make_pt((int16_t)cell_h, (int16_t)cell_w), 0, w, false, grow, hs, vs);
    tb_DisposePtr(db);
    tb_DisposePtr(rv);
    if (a->list && ldef_upp) {
        /* The program's definition: a handle whose code is its routine descriptor. */
        uint32_t n = mm_is_descriptor(ldef_upp) ? 12 + 20 * ((uint32_t)rd16(ldef_upp + 10) + 1) : 0;
        gaddr h = n ? tb_NewHandle((int32_t)n, true, false) : 0;
        if (h) {
            gmemcpy(rd32(h), ldef_upp, n);
            tb_HLock(h);
            gaddr old = ListRec_listDefProc(rd32(a->list));
            ListRec_set_listDefProc(rd32(a->list), h);
            (void)old; /* LDEF 0's, a resource */
            /* lInitMsg to the new definition */
            uint32_t args[7] = { 0, 0, 0, 0, 0, 0, a->list };
            mm_call(rd32(h), 0x000EBD80, args, 7);
        }
    }
    if (a->list)
        tb_LSetDrawingMode(true, a->list);
    tb_SetPort(saved);
    return a->list != 0;
}
