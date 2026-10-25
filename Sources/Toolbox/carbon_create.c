/*
 * CarbonLib's Create...Control calls: each standard control made as
 * NewControl makes it, with the procID and the value, minimum and maximum
 * its CDEF reads at initCntl, then the rest of the arguments set through
 * its data tags, as the Appearance Manager's documentation gives them.
 * Titles and text come as CFStrings.  The controls are visible; in a
 * window with a root control they go in it.
 */
#include <stdlib.h>
#include <string.h>

#include "qd_internal.h"
#include "tb_internal.h"

#define TAG(a, b, d, e) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(d) << 8 | (uint32_t)(e))

static int16_t clamp16(int32_t v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

/* A CFString's text as a Pascal string in guest memory (the caller's). */
static gaddr pstr_of(gaddr cf)
{
    uint8_t t[256];
    carbon_cfstring_to_pstr(cf, t);
    gaddr p = tb_NewPtr(256, false, false);
    if (p)
        gput(p, t, (uint32_t)t[0] + 1);
    return p;
}

/* NewControl's control, the title a CFString (or none). */
static int32_t create(gaddr w, gaddr bounds, gaddr cf_title, int16_t value, int16_t min, int16_t max, int16_t procid,
                      gaddr out, gaddr *made)
{
    *made = 0;
    if (out)
        wr32(out, 0);
    if (!w || !bounds || !out)
        return paramErr;
    gaddr title = cf_title ? pstr_of(cf_title) : 0;
    gaddr ch = tb_NewControl(w, bounds, title, 1, value, min, max, procid, 0);
    if (title)
        tb_DisposePtr(title);
    if (!ch)
        return memFullErr;
    wr32(out, ch);
    *made = ch;
    return noErr;
}

static void set_data(gaddr ch, int16_t part, uint32_t tag, const void *bytes, uint32_t n)
{
    gaddr p = tb_NewPtr((int32_t)(n ? n : 1), false, true);
    gput(p, bytes, n);
    tb_SetControlData(ch, part, (int32_t)tag, (int32_t)n, p);
    tb_DisposePtr(p);
}

static void set_data16(gaddr ch, uint32_t tag, int16_t v)
{
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    set_data(ch, 0, tag, b, 2);
}

static void set_data32(gaddr ch, uint32_t tag, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    set_data(ch, 0, tag, b, 4);
}

static void set_bool(gaddr ch, uint32_t tag, bool v)
{
    uint8_t b = v;
    set_data(ch, 0, tag, &b, 1);
}

/* Text from a CFString through a data tag ('text'). */
static void set_text(gaddr ch, uint32_t tag, gaddr cf)
{
    uint8_t t[256];
    carbon_cfstring_to_pstr(cf, t);
    set_data(ch, 0, tag, t + 1, t[0]);
}

/* ---------------------------------------------------------------------- */
/* Buttons, check boxes, radio buttons */

int32_t tb_CreatePushButtonControl(gaddr w, gaddr r, gaddr title, gaddr out)
{
    gaddr ch;
    return create(w, r, title, 0, 0, 1, 368, out, &ch); /* kControlPushButtonProc */
}

/* The icon at the left (kControlPushButtonIconOnLeft 6) or right (7). */
int32_t tb_CreatePushButtonWithIconControl(gaddr w, gaddr r, gaddr title, gaddr icon, uint16_t align, gaddr out)
{
    gaddr ch;
    int32_t err = create(w, r, title, 0, 0, 1, align == 7 ? 375 : 374, out, &ch);
    if (!err && icon) {
        ap_set_content(ch, icon);
        tb_Draw1Control(ch);
    }
    return err;
}

int32_t tb_CreateCheckBoxControl(gaddr w, gaddr r, gaddr title, int32_t value, int8_t auto_toggle, gaddr out)
{
    gaddr ch;
    return create(w, r, title, clamp16(value), 0, 2, auto_toggle ? 371 : 369, out, &ch);
}

int32_t tb_CreateRadioButtonControl(gaddr w, gaddr r, gaddr title, int32_t value, int8_t auto_toggle, gaddr out)
{
    gaddr ch;
    return create(w, r, title, clamp16(value), 0, 2, auto_toggle ? 372 : 370, out, &ch);
}

int32_t tb_CreateRadioGroupControl(gaddr w, gaddr r, gaddr out)
{
    gaddr ch;
    return create(w, r, 0, 0, 0, 0, 416, out, &ch);
}

/* Bevel buttons: the thickness is the variant (and the menu at the right
 * one more); the minimum holds the behaviours and a resource's content
 * type, the maximum its ID, the value the menu's ID. */
int32_t tb_CreateBevelButtonControl(gaddr w, gaddr r, gaddr title, uint16_t thickness, uint16_t behavior, gaddr info,
                                    int16_t menu_id, uint16_t menu_behavior, uint16_t placement, gaddr out)
{
    int16_t type = info ? (int16_t)rd16(info) : 0, res = 0;
    if (info && type > 0 && type < 128)
        res = (int16_t)rd16(info + 2);
    int16_t min = (int16_t)((behavior | menu_behavior) & 0xFF00);
    if (res)
        min = (int16_t)(min | (type & 0xFF));
    int16_t var = (int16_t)((thickness & 3) | (placement ? 4 : 0));
    gaddr ch;
    int32_t err = create(w, r, title, menu_id, min, res, (int16_t)(32 + var), out, &ch);
    if (!err && info && type >= 128) /* a handle of the program's */
        tb_SetControlData(ch, 0, (int32_t)TAG('c', 'o', 'n', 't'), 6, info);
    return err;
}

/* ---------------------------------------------------------------------- */
/* Scroll bars, sliders, progress bars, little arrows */

int32_t tb_CreateScrollBarControl(gaddr w, gaddr r, int32_t value, int32_t min, int32_t max, int32_t view, int8_t live,
                                  gaddr action, gaddr out)
{
    gaddr ch;
    int32_t err = create(w, r, 0, clamp16(value), clamp16(min), clamp16(max), live ? 386 : 384, out, &ch);
    if (!err) {
        tb_SetControlViewSize(ch, view);
        if (action)
            tb_SetControlAction(ch, action);
    }
    return err;
}

/* kControlSliderPointsDownOrRight 0, PointsUpOrLeft 1, DoesNotPoint 2 */
int32_t tb_CreateSliderControl(gaddr w, gaddr r, int32_t value, int32_t min, int32_t max, uint16_t orientation,
                               uint16_t ticks, int8_t live, gaddr action, gaddr out)
{
    int var = live ? 1 : 0;
    if (ticks)
        var |= 2;
    if (orientation == 1)
        var |= 4;
    else if (orientation == 2)
        var |= 8;
    gaddr ch;
    int32_t err = create(w, r, 0, clamp16(value), clamp16(min), clamp16(max), (int16_t)(48 + var), out, &ch);
    if (!err) {
        if (ticks)
            ap_set_ticks(ch, ticks);
        if (action)
            tb_SetControlAction(ch, action);
    }
    return err;
}

int32_t tb_CreateProgressBarControl(gaddr w, gaddr r, int32_t value, int32_t min, int32_t max, int8_t indeterminate,
                                    gaddr out)
{
    gaddr ch;
    int32_t err = create(w, r, 0, clamp16(value), clamp16(min), clamp16(max), 80, out, &ch);
    if (!err && indeterminate)
        set_bool(ch, TAG('i', 'n', 'd', 'e'), true);
    return err;
}

int32_t tb_CreateLittleArrowsControl(gaddr w, gaddr r, int32_t value, int32_t min, int32_t max, int32_t increment,
                                     gaddr out)
{
    gaddr ch;
    int32_t err = create(w, r, 0, clamp16(value), clamp16(min), clamp16(max), 96, out, &ch);
    (void)increment; /* CarbonLib's little arrows leave the value to the action procedure */
    return err;
}

int32_t tb_CreateChasingArrowsControl(gaddr w, gaddr r, gaddr out)
{
    gaddr ch;
    return create(w, r, 0, 0, 0, 0, 112, out, &ch);
}

/* ---------------------------------------------------------------------- */
/* Triangles, tabs, separators, group boxes */

/* kControlDisclosureTrianglePointRight 1, PointLeft 2 (Default 0: right) */
int32_t tb_CreateDisclosureTriangleControl(gaddr w, gaddr r, uint16_t orientation, gaddr title, int32_t value,
                                           int8_t draw_title, int8_t auto_toggle, gaddr out)
{
    int var = (orientation == 2 ? 1 : 0) | (auto_toggle ? 2 : 0);
    gaddr ch;
    int32_t err = create(w, r, title, clamp16(value), 0, 1, (int16_t)(64 + var), out, &ch);
    if (!err && draw_title) {
        ap_set_behavior(ch, AP_DRAWS_TITLE);
        tb_Draw1Control(ch);
    }
    return err;
}

/* ControlTabEntry: icon (a ControlButtonContentInfo pointer), name (a
 * CFString), enabled; ten bytes each. */
int32_t tb_CreateTabsControl(gaddr w, gaddr r, uint16_t size, uint16_t direction, uint16_t count, gaddr tabs, gaddr out)
{
    int16_t proc = (int16_t)(128 + (direction & 3) * 2 + (size == 1 ? 1 : 0)); /* kControlTabSizeSmall 1 */
    gaddr ch;
    int32_t err = create(w, r, 0, 0, 0, 0, proc, out, &ch);
    if (err)
        return err;
    tb_SetControlMaximum(ch, (int16_t)count);
    for (int i = 0; i < count && tabs; i++) {
        gaddr e = tabs + (gaddr)i * 10;
        gaddr icon = rd32(e);
        int16_t suite = icon && rd16(icon) == 1 ? (int16_t)rd16(icon + 2) : 0; /* kControlContentIconSuiteRes */
        uint8_t info[8] = { 0, 1, (uint8_t)(suite >> 8), (uint8_t)suite };
        gaddr name = rd32(e + 4);
        info[4] = (uint8_t)(name >> 24);
        info[5] = (uint8_t)(name >> 16);
        info[6] = (uint8_t)(name >> 8);
        info[7] = (uint8_t)name;
        set_data(ch, (int16_t)(i + 1), TAG('t', 'a', 'b', 'i'), info, 8); /* ControlTabInfoRecV1 */
        uint8_t on = rd8(e + 8) != 0;
        set_data(ch, (int16_t)(i + 1), TAG('e', 'n', 'a', 'b'), &on, 1);
    }
    if (count)
        tb_SetControlValue(ch, 1);
    return noErr;
}

int32_t tb_CreateSeparatorControl(gaddr w, gaddr r, gaddr out)
{
    gaddr ch;
    return create(w, r, 0, 0, 0, 0, 144, out, &ch);
}

int32_t tb_CreateGroupBoxControl(gaddr w, gaddr r, gaddr title, int8_t primary, gaddr out)
{
    gaddr ch;
    return create(w, r, title, 0, 0, 0, primary ? 160 : 164, out, &ch);
}

int32_t tb_CreateCheckGroupBoxControl(gaddr w, gaddr r, gaddr title, int32_t value, int8_t primary, int8_t auto_toggle,
                                      gaddr out)
{
    gaddr ch;
    int32_t err = create(w, r, title, clamp16(value), 0, 1, primary ? 161 : 165, out, &ch);
    if (!err && auto_toggle)
        ap_set_behavior(ch, AP_AUTO_TOGGLE);
    return err;
}

/* The title a pop-up: its menu (by ID), the current item 1. */
int32_t tb_CreatePopupGroupBoxControl(gaddr w, gaddr r, gaddr title, int8_t primary, int16_t menu_id, int8_t variable,
                                      int16_t title_width, int16_t just, uint8_t style, gaddr out)
{
    gaddr ch;
    int32_t err = create(w, r, title, 0, 0, 0, primary ? 162 : 166, out, &ch);
    if (err)
        return err;
    gaddr menu = tb_GetMenuHandle(menu_id);
    if (!menu) {
        menu = tb_GetMenu(menu_id);
        if (menu)
            tb_InsertMenu(menu, -1);
    }
    if (menu) {
        set_data32(ch, TAG('m', 'h', 'a', 'n'), menu);
        tb_SetControlValue(ch, 1);
    }
    (void)variable;
    (void)title_width;
    (void)just;
    (void)style;
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* Images, pop-ups, placards, clocks, panes */

int32_t tb_CreateImageWellControl(gaddr w, gaddr r, gaddr info, gaddr out)
{
    gaddr ch;
    int32_t err = create(w, r, 0, 0, 0, 0, 176, out, &ch);
    if (!err && info)
        tb_SetControlData(ch, 0, (int32_t)TAG('c', 'o', 'n', 't'), 6, info);
    return err;
}

int32_t tb_CreatePictureControl(gaddr w, gaddr r, gaddr info, int8_t dont_track, gaddr out)
{
    gaddr ch;
    int32_t err = create(w, r, 0, 0, 0, 0, dont_track ? 305 : 304, out, &ch);
    if (!err && info)
        tb_SetControlData(ch, 0, (int32_t)TAG('c', 'o', 'n', 't'), 6, info);
    return err;
}

int32_t tb_CreateIconControl(gaddr w, gaddr r, gaddr info, int8_t dont_track, gaddr out)
{
    gaddr ch;
    int32_t err = create(w, r, 0, 0, 0, 0, dont_track ? 321 : 320, out, &ch);
    if (!err && info)
        tb_SetControlData(ch, 0, (int32_t)TAG('c', 'o', 'n', 't'), 6, info);
    return err;
}

/* kControlPopupArrowOrientationEast 0, West 1, North 2, South 3; size
 * kControlPopupArrowSizeNormal 0, Small 1. */
int32_t tb_CreatePopupArrowControl(gaddr w, gaddr r, uint16_t orientation, uint16_t size, gaddr out)
{
    gaddr ch;
    return create(w, r, 0, 0, 0, 0, (int16_t)(192 + (orientation & 3) + (size == 1 ? 4 : 0)), out, &ch);
}

/* Pop-up buttons: the value is the title's justification and style, the
 * minimum the menu's ID, the maximum the title's width. */
int32_t tb_CreatePopupButtonControl(gaddr w, gaddr r, gaddr title, int16_t menu_id, int8_t variable, int16_t width,
                                    int16_t just, uint8_t style, gaddr out)
{
    gaddr ch;
    int16_t value = (int16_t)((style << 8) | (just & 0xFF));
    return create(w, r, title, value, menu_id, width, (int16_t)(400 + (variable ? 2 : 0)), out, &ch);
}

int32_t tb_CreatePlacardControl(gaddr w, gaddr r, gaddr out)
{
    gaddr ch;
    return create(w, r, 0, 0, 0, 0, 224, out, &ch);
}

/* The clock's flags go in its value. */
int32_t tb_CreateClockControl(gaddr w, gaddr r, uint16_t type, uint32_t flags, gaddr out)
{
    gaddr ch;
    return create(w, r, 0, (int16_t)flags, 0, 0, (int16_t)(240 + (type & 3)), out, &ch);
}

/* A user pane's features go in its value. */
int32_t tb_CreateUserPaneControl(gaddr w, gaddr r, uint32_t features, gaddr out)
{
    gaddr ch;
    return create(w, r, 0, (int16_t)features, 0, 0, 256, out, &ch);
}

int32_t tb_CreateWindowHeaderControl(gaddr w, gaddr r, int8_t list_header, gaddr out)
{
    gaddr ch;
    return create(w, r, 0, 0, 0, 0, list_header ? 337 : 336, out, &ch);
}

/* ---------------------------------------------------------------------- */
/* Text */

int32_t tb_CreateEditTextControl(gaddr w, gaddr r, gaddr text, int8_t password, int8_t inline_input, gaddr style,
                                 gaddr out)
{
    gaddr ch;
    int32_t err = create(w, r, 0, 0, 0, 0, password ? 274 : inline_input ? 276 : 272, out, &ch);
    if (err)
        return err;
    if (style)
        tb_SetControlFontStyle(ch, style);
    if (text)
        set_text(ch, TAG('t', 'e', 'x', 't'), text);
    return noErr;
}

int32_t tb_CreateStaticTextControl(gaddr w, gaddr r, gaddr text, gaddr style, gaddr out)
{
    gaddr ch;
    int32_t err = create(w, r, 0, 0, 0, 0, 288, out, &ch);
    if (err)
        return err;
    if (style)
        tb_SetControlFontStyle(ch, style);
    if (text)
        set_text(ch, TAG('t', 'e', 'x', 't'), text);
    return noErr;
}

/* The scrolling text box reads its 'TEXT' and 'styl' by the value's ID,
 * its delays from the minimum and maximum. */
int32_t tb_CreateScrollingTextBoxControl(gaddr w, gaddr r, int16_t res_id, int8_t auto_scroll, uint32_t delay_before,
                                         uint32_t delay_between, uint16_t amount, gaddr out)
{
    gaddr ch;
    int32_t err = create(w, r, 0, res_id, clamp16((int32_t)delay_before), clamp16((int32_t)delay_between),
                         auto_scroll ? 433 : 432, out, &ch);
    if (!err && auto_scroll) {
        set_data32(ch, TAG('s', 't', 'd', 'l'), delay_before);
        set_data32(ch, TAG('s', 'c', 'd', 'l'), delay_between);
        set_data16(ch, TAG('s', 'a', 'm', 't'), (int16_t)amount);
    }
    return err;
}

/* ---------------------------------------------------------------------- */
/* List boxes */

/* ListDefSpec: defType (kListDefProcPtr 0 with a UPP, the standard text
 * definition 1, icons 2), then the UPP. */
int32_t tb_CreateListBoxControl(gaddr w, gaddr r, int8_t auto_size, int16_t rows, int16_t cols, int8_t hscroll,
                                int8_t vscroll, int16_t cell_h, int16_t cell_w, int8_t grow, gaddr def, gaddr out)
{
    gaddr ch;
    int32_t err = create(w, r, 0, 0, 0, 0, auto_size ? 353 : 352, out, &ch);
    if (err)
        return err;
    gaddr upp = def && rd32(def) == 0 ? rd32(def + 4) : 0;
    ap_list_setup(ch, rows, cols, cell_h, cell_w, hscroll != 0, vscroll != 0, grow != 0, upp);
    tb_Draw1Control(ch);
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* The helpers of the Appearance controls that have no trap */

int16_t tb_GetTabContentRect(gaddr ch, gaddr r)
{
    gaddr size = tb_NewPtr(4, false, true);
    int16_t err = tb_GetControlData(ch, 0, (int32_t)TAG('r', 'e', 'c', 't'), 8, r, size);
    tb_DisposePtr(size);
    return err;
}

int16_t tb_GetImageWellContentInfo(gaddr ch, gaddr info)
{
    gaddr size = tb_NewPtr(4, false, true);
    int16_t err = tb_GetControlData(ch, 0, (int32_t)TAG('c', 'o', 'n', 't'), 6, info, size);
    tb_DisposePtr(size);
    return err;
}
