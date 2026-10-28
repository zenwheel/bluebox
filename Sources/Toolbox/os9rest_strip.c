/*
 * The Control Strip's module services beyond vam.c's and controlstrip.c's
 * (ControlStrip.h): the strip's font and show/hide hot key, bar graphs,
 * popup menus and sliders that pop up from a module, help strings, modal
 * dialogs and detached icon suites.  Blue Box draws no strip, so there is
 * no module to pop anything up from: the popups are tracked where the
 * module's rectangle says, the slider by the mouse's travel from where it
 * went down, and a help string has no strip to be shown in.
 */
#include <string.h>

#include "tb_internal.h"

enum { kBarWidth = 4, kBarGap = 1, kBarHeight = 12, kPixelsPerTick = 6 };
enum { geneva = 3, defaultSize = 9, cmdKey = 0x0100, optionKey = 0x0800, controlKey = 0x1000 };

static struct {
    int16_t font, size;
    int16_t modifiers;
    uint8_t key_code;
    bool hot_key_enabled;
    bool set;
} strip = { geneva, defaultSize, cmdKey | optionKey | controlKey, 1, true, true };

int16_t tb_SBGetControlStripFontID(gaddr fontID)
{
    if (!fontID)
        return paramErr;
    wr16(fontID, (uint16_t)strip.font);
    return noErr;
}

int16_t tb_SBSetControlStripFontID(int16_t fontID)
{
    strip.font = fontID;
    return noErr;
}

int16_t tb_SBGetControlStripFontSize(gaddr fontSize)
{
    if (!fontSize)
        return paramErr;
    wr16(fontSize, (uint16_t)strip.size);
    return noErr;
}

int16_t tb_SBSetControlStripFontSize(int16_t fontSize)
{
    if (fontSize <= 0)
        return paramErr;
    strip.size = fontSize;
    return noErr;
}

/* The key combination that shows or hides the strip: the modifiers of an
 * event record and a virtual key code. */
int16_t tb_SBGetShowHideHotKey(gaddr modifiers, gaddr keyCode)
{
    if (!modifiers || !keyCode)
        return paramErr;
    wr16(modifiers, (uint16_t)strip.modifiers);
    wr8(keyCode, strip.key_code);
    return noErr;
}

int16_t tb_SBSetShowHideHotKey(int16_t modifiers, uint8_t keyCode)
{
    strip.modifiers = modifiers;
    strip.key_code = keyCode;
    return noErr;
}

int16_t tb_SBIsShowHideHotKeyEnabled(gaddr enabled)
{
    if (!enabled)
        return paramErr;
    wr8(enabled, strip.hot_key_enabled);
    return noErr;
}

int16_t tb_SBEnableShowHideHotKey(int8_t enabled)
{
    strip.hot_key_enabled = enabled != 0;
    return noErr;
}

/* A bar graph of barCount bars is that many bars of kBarWidth with
 * kBarGap between them. */
int16_t tb_SBGetBarGraphWidth(int16_t barCount)
{
    if (barCount <= 0)
        return 0;
    return (int16_t)(barCount * kBarWidth + (barCount - 1) * kBarGap);
}

/* SBDrawBarGraph(level, barCount, direction, topLeft): the bars up to level
 * filled and the rest framed, left to right (direction 0) or right to left
 * (any other), in the current port. */
void tb_SBDrawBarGraph(int16_t level, int16_t barCount, int16_t direction, uint32_t barGraphTopLeft)
{
    int16_t top = pt_v(barGraphTopLeft), left = pt_h(barGraphTopLeft);
    gaddr r = tb_NewPtr(8, false, false);
    if (!r)
        return;
    for (int i = 0; i < barCount; i++) {
        int pos = direction ? barCount - 1 - i : i;
        int16_t l = (int16_t)(left + pos * (kBarWidth + kBarGap));
        wr16(r, (uint16_t)top);
        wr16(r + 2, (uint16_t)l);
        wr16(r + 4, (uint16_t)(top + kBarHeight));
        wr16(r + 6, (uint16_t)(l + kBarWidth));
        if (i < level)
            tb_PaintRect(r);
        else
            tb_FrameRect(r);
    }
    tb_DisposePtr(r);
}

/* SBTrackPopupMenu(moduleRect, menu): the menu popped up from the module,
 * the item chosen, 0 for none. */
int16_t tb_SBTrackPopupMenu(gaddr moduleRect, gaddr theMenu)
{
    if (!moduleRect || !theMenu)
        return 0;
    int16_t id = (int16_t)rd16(rd32(theMenu)); /* MenuInfo's menuID */
    tb_InsertMenu(theMenu, -1);
    int32_t r = tb_PopUpMenuSelect(theMenu, (int16_t)rd16(moduleRect), (int16_t)rd16(moduleRect + 2), 1);
    tb_DeleteMenu(id);
    return (int16_t)(r & 0xFFFF);
}

/* The value a slider tracked by the mouse comes to: the initial value
 * moved by the mouse's travel up from where it started. */
static int16_t track(gaddr moduleRect, int16_t ticks, int16_t initial, bool *hit)
{
    (void)moduleRect;
    int16_t top = ticks > 0 ? (int16_t)(ticks - 1) : 0;
    int16_t value = initial < 0 ? 0 : initial > top ? top : initial;
    *hit = false;
    if (!tb_Button())
        return value;
    gaddr pt = tb_NewPtr(4, true, false);
    if (!pt)
        return value;
    tb_GetMouse(pt);
    int16_t start = (int16_t)rd16(pt);
    while (tb_StillDown()) {
        tb_GetMouse(pt);
        int v = initial + (start - (int16_t)rd16(pt)) / kPixelsPerTick;
        v = v < 0 ? 0 : v > top ? top : v;
        if (v != value)
            *hit = true;
        value = (int16_t)v;
    }
    tb_DisposePtr(pt);
    return value;
}

int16_t tb_SBTrackSlider(gaddr moduleRect, int16_t ticksOnSlider, int16_t initialValue)
{
    bool hit;
    return track(moduleRect, ticksOnSlider, initialValue, &hit);
}

/* SBHitTrackSlider: the same, and whether the slider was moved. */
int16_t tb_SBHitTrackSlider(gaddr moduleRect, int16_t ticksOnSlider, int16_t initialValue, gaddr hit)
{
    bool moved;
    int16_t v = track(moduleRect, ticksOnSlider, initialValue, &moved);
    if (hit)
        wr8(hit, moved);
    return v;
}

/* A help string shows beside the strip, which Blue Box does not draw. */
int16_t tb_SBShowHelpString(gaddr moduleRect, gaddr helpString)
{
    return moduleRect && helpString ? noErr : paramErr;
}

/* The module's dialog, as ModalDialog runs it. */
void tb_SBModalDialogInContext(gaddr filterProc, gaddr itemHit) { tb_ModalDialog(filterProc, itemHit); }

/* An icon suite from the module's resources. */
int16_t tb_SBGetDetachIconSuite(gaddr theIconSuite, int16_t theResID, uint32_t selector)
{
    if (!theIconSuite)
        return paramErr;
    return tb_GetIconSuite(theIconSuite, theResID, selector);
}
