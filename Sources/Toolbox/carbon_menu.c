/*
 * CarbonLib's Menu Manager: menus made without resources and given IDs,
 * titles and sizes through accessors, items by index or by command ID,
 * item attributes, properties, indents and virtual keys, items copied
 * between menus, menus duplicated and retained, definitions registered,
 * the menu bar as a root menu, standard font menus, and the CFString
 * forms of the text calls.  A menu is the MenuInfo the Menu Manager keeps
 * (menu.c); CarbonLib's additions to it and its items are kept with the
 * Menu Manager's own extras there.
 */
#include <stdlib.h>
#include <string.h>

#define CARBON_HI_EXPORTS
#include "carbon_hi.h"
#include "qd_internal.h"
#include "tb_internal.h"

enum { menuPropertyInvalidErr = -5603, menuPropertyNotFoundErr = -5604, menuNotFoundErr = -5620,
       menuUsesSystemDefErr = -5621, menuItemNotFoundErr = -5622, kFMInvalidFontFamilyErr = -981 };
enum { kMenuAttrExcludesMarkColumn = 1 << 0 };
enum { kMenuItemAttrDisabled = 1 << 0, kMenuItemAttrIconDisabled = 1 << 1, kMenuItemAttrSeparator = 1 << 6,
       kMenuItemAttrUseVirtualKey = 1 << 10 };
enum { kMenuDefProcPtr = 0 };
enum { kThemeMenuTypePullDown = 0, kThemeMenuTypePopUp = 1, kThemeMenuTypeHierarchical = 2 };
enum { HIER_MARK = 0x1B };

static bool menu_ok(gaddr m) { return m && rd32(m) && tb_GetHandleSize(m) > (int32_t)MenuInfo_menuData_off; }
static int count(gaddr m) { return tb_CountMItems(m); }
static bool item_in(gaddr m, int item) { return menu_ok(m) && item >= 1 && item <= count(m); }

void hi_item_free(hi_item *h)
{
    for (hi_prop *p = h->props, *next; p; p = next) {
        next = p->next;
        free(p->data);
        free(p);
    }
    memset(h, 0, sizeof *h);
}

void hi_menu_free(hi_menu *h) { memset(h, 0, sizeof *h); }

/* ---- Menus ---- */

/* CreateNewMenu(menuID, attributes, outMenu): an empty menu, untitled. */
static gaddr new_menu(int16_t id, uint32_t attributes)
{
    gaddr title = tb_NewPtr(2, true, true);
    gaddr m = title ? tb_NewMenu(id, title) : 0;
    if (title)
        tb_DisposePtr(title);
    if (m) {
        hi_menu *h = menu_hi_menu(m, true);
        if (h)
            h->attributes = attributes;
        if (attributes & kMenuAttrExcludesMarkColumn)
            tb_SetMenuExcludesMarkColumn(m, 1);
    }
    return m;
}

static void mn_CreateNewMenu(ppc_cpu *c)
{
    if (!R(2)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    gaddr m = new_menu((int16_t)R(0), R(1));
    wr32(R(2), m);
    RETURN(m ? noErr : (uint32_t)memFullErr);
}

/* A menu definition procedure as an MDEF: a handle holding a routine
 * descriptor, which 68k code runs as it runs a PowerPC MDEF resource. */
static gaddr def_handle(gaddr upp)
{
    if (!upp || !mm_is_descriptor(upp))
        return 0;
    uint32_t n = 12 + 20 * (rd16(upp + 10) + 1u);
    gaddr h = tb_NewHandle((int32_t)n, true, true);
    if (h)
        gmemcpy(rd32(h), upp, n);
    return h;
}

/* CreateCustomMenu(defSpec, menuID, attributes, outMenu) */
static void mn_CreateCustomMenu(ppc_cpu *c)
{
    gaddr spec = R(0);
    if (!spec || !R(3) || rd32(spec) != kMenuDefProcPtr) {
        RETURN((uint32_t)paramErr);
        return;
    }
    gaddr def = def_handle(rd32(spec + 4));
    gaddr m = def ? new_menu((int16_t)R(1), R(2)) : 0;
    if (!m) {
        if (def)
            tb_DisposeHandle(def);
        RETURN((uint32_t)(def ? memFullErr : paramErr));
        return;
    }
    MenuInfo_set_menuProc(rd32(m), def);
    wr32(R(3), m);
    RETURN(noErr);
}

/* GetMenuDefinition / SetMenuDefinition(menu, defSpec) */
static void mn_GetMenuDefinition(ppc_cpu *c)
{
    if (!menu_ok(R(0)) || !R(1)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    gaddr def = MenuInfo_menuProc(rd32(R(0)));
    wr32(R(1), kMenuDefProcPtr);
    /* the program's procedure: the descriptor in the handle */
    wr32(R(1) + 4, def && rd32(def) && mm_is_descriptor(rd32(def)) ? rd32(def) : 0);
    RETURN(def && rd32(def) && mm_is_descriptor(rd32(def)) ? noErr : (uint32_t)menuUsesSystemDefErr);
}

static void mn_SetMenuDefinition(ppc_cpu *c)
{
    gaddr spec = R(1);
    if (!menu_ok(R(0)) || !spec || rd32(spec) != kMenuDefProcPtr) {
        RETURN((uint32_t)paramErr);
        return;
    }
    gaddr def = def_handle(rd32(spec + 4));
    if (!def) {
        RETURN((uint32_t)paramErr);
        return;
    }
    MenuInfo_set_menuProc(rd32(R(0)), def);
    tb_CalcMenuSize(R(0));
    RETURN(noErr);
}

/* RegisterMenuDefinition(resID, defSpec): GetMenu finds this definition
 * for MENUs naming that MDEF. */
static struct {
    int16_t id;
    gaddr def;
} registered[16];

gaddr carbonmenu_registered_def(int16_t id)
{
    for (int i = 0; i < 16; i++)
        if (registered[i].def && registered[i].id == id)
            return registered[i].def;
    return 0;
}

static void mn_RegisterMenuDefinition(ppc_cpu *c)
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
    gaddr def = rd32(spec) == kMenuDefProcPtr ? def_handle(rd32(spec + 4)) : 0;
    if (!def) {
        RETURN((uint32_t)paramErr);
        return;
    }
    for (int i = 0; i < 16 && slot < 0; i++)
        if (!registered[i].def)
            slot = i;
    if (slot < 0) {
        tb_DisposeHandle(def);
        RETURN((uint32_t)memFullErr);
        return;
    }
    if (registered[slot].def)
        tb_DisposeHandle(registered[slot].def);
    registered[slot].id = id;
    registered[slot].def = def;
    RETURN(noErr);
}

static void mn_IsValidMenu(ppc_cpu *c)
{
    gaddr m = R(0);
    RETURN(menu_ok(m) && (uint32_t)tb_GetHandleSize(m) >= MenuInfo_menuData_off + rd8(rd32(m) + MenuInfo_menuData_off) + 2u);
}

static void mn_GetMenuID(ppc_cpu *c) { RETURN(menu_ok(R(0)) ? (int32_t)MenuInfo_menuID(rd32(R(0))) : 0); }
static void mn_GetMenuWidth(ppc_cpu *c) { RETURN(menu_ok(R(0)) ? (int32_t)MenuInfo_menuWidth(rd32(R(0))) : 0); }
static void mn_GetMenuHeight(ppc_cpu *c) { RETURN(menu_ok(R(0)) ? (int32_t)MenuInfo_menuHeight(rd32(R(0))) : 0); }

static void mn_SetMenuID(ppc_cpu *c)
{
    if (menu_ok(R(0)))
        MenuInfo_set_menuID(rd32(R(0)), (int16_t)R(1));
    DONE();
}

static void mn_SetMenuWidth(ppc_cpu *c)
{
    if (menu_ok(R(0)))
        MenuInfo_set_menuWidth(rd32(R(0)), (int16_t)R(1));
    DONE();
}

static void mn_SetMenuHeight(ppc_cpu *c)
{
    if (menu_ok(R(0)))
        MenuInfo_set_menuHeight(rd32(R(0)), (int16_t)R(1));
    DONE();
}

/* GetMenuTitle(menu, title): a copy, and the pointer to it. */
static void mn_GetMenuTitle(ppc_cpu *c)
{
    if (!menu_ok(R(0)) || !R(1)) {
        RETURN(0);
        return;
    }
    gaddr t = rd32(R(0)) + MenuInfo_menuData_off;
    gmemcpy(R(1), t, rd8(t) + 1u);
    RETURN(R(1));
}

static void mn_SetMenuTitle(ppc_cpu *c)
{
    if (!menu_ok(R(0)) || !R(1)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    RETURN(menu_set_title(R(0), R(1)) ? noErr : (uint32_t)memFullErr);
}

/* GetMenuAttributes / ChangeMenuAttributes: the mark column kept with
 * the Menu Manager's own setting. */
static uint32_t menu_attributes(gaddr m)
{
    hi_menu *h = menu_hi_menu(m, false);
    uint32_t a = h ? h->attributes : 0;
    a &= ~(uint32_t)kMenuAttrExcludesMarkColumn;
    if (tb_GetMenuExcludesMarkColumn(m))
        a |= kMenuAttrExcludesMarkColumn;
    return a;
}

static void mn_GetMenuAttributes(ppc_cpu *c)
{
    if (!menu_ok(R(0)) || !R(1)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    wr32(R(1), menu_attributes(R(0)));
    RETURN(noErr);
}

static void mn_ChangeMenuAttributes(ppc_cpu *c)
{
    hi_menu *h = menu_ok(R(0)) ? menu_hi_menu(R(0), true) : NULL;
    if (!h) {
        RETURN((uint32_t)paramErr);
        return;
    }
    uint32_t a = (menu_attributes(R(0)) | R(1)) & ~R(2);
    h->attributes = a;
    tb_SetMenuExcludesMarkColumn(R(0), (a & kMenuAttrExcludesMarkColumn) != 0);
    RETURN(noErr);
}

/* Retain counts: the last release disposes of the menu. */
static uint32_t retain_of(gaddr m)
{
    hi_menu *h = menu_hi_menu(m, false);
    return h && h->retained ? h->retain : 1;
}

static void mn_RetainMenu(ppc_cpu *c)
{
    hi_menu *h = menu_ok(R(0)) ? menu_hi_menu(R(0), true) : NULL;
    if (!h) {
        RETURN((uint32_t)paramErr);
        return;
    }
    h->retain = (h->retained ? h->retain : 1) + 1;
    h->retained = true;
    RETURN(noErr);
}

static void mn_ReleaseMenu(ppc_cpu *c)
{
    hi_menu *h = menu_ok(R(0)) ? menu_hi_menu(R(0), true) : NULL;
    if (!h) {
        RETURN((uint32_t)paramErr);
        return;
    }
    h->retain = (h->retained ? h->retain : 1) - 1;
    h->retained = true;
    if (!h->retain)
        tb_DisposeMenu(R(0));
    RETURN(noErr);
}

static void mn_GetMenuRetainCount(ppc_cpu *c) { RETURN(menu_ok(R(0)) ? retain_of(R(0)) : 0); }

/* ---- The menu list and the menu bar ---- */

static int bar_count(void)
{
    gaddr ml = rd32(LM_MenuList);
    return ml && rd32(ml) ? (int16_t)rd16(rd32(ml)) / 6 : 0;
}

static gaddr bar_menu(int i) { return rd32(rd32(rd32(LM_MenuList)) + 6 + (uint32_t)i * 6); }

static bool in_bar(gaddr m)
{
    for (int i = 0; i < bar_count(); i++)
        if (bar_menu(i) == m)
            return true;
    return false;
}

/* GetMenuType(menu, outType): pull-down in the bar, hierarchical when
 * it's some item's submenu, else pop-up. */
static void mn_GetMenuType(ppc_cpu *c)
{
    if (!menu_ok(R(0)) || !R(1)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    uint16_t type = kThemeMenuTypePopUp;
    if (in_bar(R(0)))
        type = kThemeMenuTypePullDown;
    else if (tb_GetMenuHandle(MenuInfo_menuID(rd32(R(0)))) == R(0))
        type = kThemeMenuTypeHierarchical;
    wr16(R(1), type);
    RETURN(noErr);
}

static void mn_HideMenuBar(ppc_cpu *c)
{
    wm_hide_menu_bar(true);
    DONE();
}

static void mn_ShowMenuBar(ppc_cpu *c)
{
    wm_hide_menu_bar(false);
    DONE();
}

static void mn_IsMenuBarVisible(ppc_cpu *c) { RETURN(rd16(LM_MBarHeight) != 0); }

static void mn_SetMenuFlashCount(ppc_cpu *c)
{
    tb_SetMenuFlash((int16_t)R(0));
    DONE();
}

/* DuplicateMenuBar(mbar, outMbar) / DisposeMenuBar(mbar): GetMenuBar's
 * handles. */
static void mn_DuplicateMenuBar(ppc_cpu *c)
{
    gaddr h = R(0);
    if (!h || !rd32(h) || !R(1)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    int32_t n = tb_GetHandleSize(h);
    gaddr copy = tb_NewHandle(n, false, true);
    if (!copy) {
        RETURN((uint32_t)memFullErr);
        return;
    }
    gmemcpy(rd32(copy), rd32(h), (uint32_t)n);
    wr32(R(1), copy);
    RETURN(noErr);
}

static void mn_DisposeMenuBar(ppc_cpu *c)
{
    if (!R(0)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    tb_DisposeHandle(R(0));
    RETURN(noErr);
}

/* AcquireRootMenu(): a menu standing for the menu bar, an item for each
 * of its menus with the menu its submenu; SetRootMenu(menu) makes a bar
 * of a root menu's submenus. */
static void mn_AcquireRootMenu(ppc_cpu *c)
{
    gaddr root = new_menu(0, 0);
    if (!root) {
        RETURN(0);
        return;
    }
    gaddr text = tb_NewPtr(256, true, true);
    for (int i = 0; i < bar_count(); i++) {
        gaddr m = bar_menu(i);
        if (!menu_ok(m))
            continue;
        pstr_from_c(text, " ");
        tb_AppendMenu(root, text);
        int n = count(root);
        gmemcpy(text, rd32(m) + MenuInfo_menuData_off, rd8(rd32(m) + MenuInfo_menuData_off) + 1u);
        tb_SetMenuItemText(root, (int16_t)n, text);
        int16_t id = MenuInfo_menuID(rd32(m));
        tb_SetItemCmd(root, (int16_t)n, HIER_MARK);
        tb_SetItemMark(root, (int16_t)n, id);
    }
    tb_DisposePtr(text);
    RETURN(root);
}

/* An item's submenu: by its hierarchical ID, among the menus the Menu
 * Manager knows. */
static gaddr submenu_of(gaddr m, int item)
{
    gaddr cmd = tb_NewPtr(4, false, true);
    gaddr sub = 0;
    if (cmd) {
        tb_GetItemCmd(m, (int16_t)item, cmd);
        if (rd16(cmd) == HIER_MARK) {
            tb_GetItemMark(m, (int16_t)item, cmd);
            sub = tb_GetMenuHandle((int16_t)rd16(cmd));
        }
        tb_DisposePtr(cmd);
    }
    return sub;
}

static void mn_SetRootMenu(ppc_cpu *c)
{
    gaddr root = R(0);
    if (!menu_ok(root)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    gaddr subs[64];
    int n = 0;
    for (int i = 1; i <= count(root) && n < 64; i++) {
        gaddr s = submenu_of(root, i);
        if (s)
            subs[n++] = s;
    }
    tb_ClearMenuBar();
    for (int i = 0; i < n; i++)
        tb_InsertMenu(subs[i], 0);
    tb_DrawMenuBar();
    RETURN(noErr);
}

/* The menu bar is drawn again whenever it changes here; there's nothing
 * left to draw later. */
static void mn_false(ppc_cpu *c) { RETURN(0); }

static void mn_InvalidateMenuSize(ppc_cpu *c)
{
    if (!menu_ok(R(0))) {
        RETURN((uint32_t)paramErr);
        return;
    }
    tb_CalcMenuSize(R(0));
    RETURN(noErr);
}

/* InvalidateMenuItems(menu, first, count) / IsMenuItemInvalid /
 * UpdateInvalidMenuItems: items marked for the program to bring up to
 * date (its kEventMenuEnableItems handler) before they're next shown. */
static void mn_InvalidateMenuItems(ppc_cpu *c)
{
    gaddr m = R(0);
    if (!menu_ok(m)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    for (uint32_t i = 0; i < R(2); i++) {
        hi_item *h = menu_hi_item(m, (int)(R(1) + i), true);
        if (h)
            h->invalid = true;
    }
    RETURN(noErr);
}

static void mn_IsMenuItemInvalid(ppc_cpu *c)
{
    hi_item *h = menu_hi_item(R(0), (int)(uint16_t)R(1), false);
    RETURN(h && h->invalid);
}

static void mn_UpdateInvalidMenuItems(ppc_cpu *c)
{
    gaddr m = R(0);
    if (!menu_ok(m)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    for (int i = 1; i <= count(m); i++) {
        hi_item *h = menu_hi_item(m, i, false);
        if (h)
            h->invalid = false;
    }
    RETURN(noErr);
}

static void mn_InvalidateMenuEnabling(ppc_cpu *c) { RETURN(noErr); }

/* GetMenuTrackingData(menu, outData): only while the menu is open, which
 * it isn't when the program is running. */
static void mn_GetMenuTrackingData(ppc_cpu *c) { RETURN((uint32_t)menuNotFoundErr); }

/* ---- Items ---- */

static void mn_CountMenuItems(ppc_cpu *c) { RETURN(menu_ok(R(0)) ? count(R(0)) : 0); }

static void mn_CheckMenuItem(ppc_cpu *c)
{
    if (menu_ok(R(0)))
        tb_CheckItem(R(0), (int16_t)R(1), (int8_t)((R(2) & 0xFF) != 0));
    DONE();
}

static void mn_EnableMenuItem(ppc_cpu *c)
{
    if (menu_ok(R(0)))
        tb_EnableItem(R(0), (int16_t)R(1));
    DONE();
}

static void mn_DisableMenuItem(ppc_cpu *c)
{
    if (menu_ok(R(0)))
        tb_DisableItem(R(0), (int16_t)R(1));
    DONE();
}

/* EnableAllMenuItems / DisableAllMenuItems: every item, not the title. */
static void set_all(gaddr m, bool on)
{
    if (!menu_ok(m))
        return;
    int32_t flags = MenuInfo_enableFlags(rd32(m));
    MenuInfo_set_enableFlags(rd32(m), on ? (int32_t)(flags | (int32_t)0xFFFFFFFE) : (int32_t)(flags & 1));
    for (int i = 32; i <= count(m); i++) {
        hi_item *h = menu_hi_item(m, i, on ? false : true);
        if (h)
            h->attributes = on ? h->attributes & ~(uint32_t)kMenuItemAttrDisabled
                               : h->attributes | kMenuItemAttrDisabled;
    }
    tb_CalcMenuSize(m);
}

static void mn_EnableAllMenuItems(ppc_cpu *c)
{
    set_all(R(0), true);
    DONE();
}

static void mn_DisableAllMenuItems(ppc_cpu *c)
{
    set_all(R(0), false);
    DONE();
}

static bool item_enabled(gaddr m, int i)
{
    hi_item *h = menu_hi_item(m, i, false);
    if (h && (h->attributes & kMenuItemAttrDisabled))
        return false;
    return tb_IsMenuItemEnabled(m, (uint16_t)i) != 0;
}

static void mn_MenuHasEnabledItems(ppc_cpu *c)
{
    gaddr m = R(0);
    bool any = false;
    for (int i = 1; menu_ok(m) && i <= count(m) && !any; i++)
        any = item_enabled(m, i);
    RETURN(any);
}

/* AppendMenuItemText(menu, string) / InsertMenuItemText(menu, string,
 * afterItem): the text taken as it is, metacharacters and all. */
static int insert_text(gaddr m, gaddr text, int after)
{
    gaddr blank = tb_NewPtr(4, true, true);
    pstr_from_c(blank, " ");
    int n = count(m);
    if (after < 0 || after > n)
        after = n;
    tb_InsertMenuItem(m, blank, (int16_t)after);
    tb_DisposePtr(blank);
    if (count(m) != n + 1)
        return 0;
    if (text)
        tb_SetMenuItemText(m, (int16_t)(after + 1), text);
    return after + 1;
}

static void mn_AppendMenuItemText(ppc_cpu *c)
{
    if (!menu_ok(R(0)) || !R(1)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    RETURN(insert_text(R(0), R(1), count(R(0))) ? noErr : (uint32_t)memFullErr);
}

static void mn_InsertMenuItemText(ppc_cpu *c)
{
    if (!menu_ok(R(0)) || !R(1)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    RETURN(insert_text(R(0), R(1), (int)(uint16_t)R(2)) ? noErr : (uint32_t)memFullErr);
}

/* DeleteMenuItems(menu, firstItem, numItems) */
static void mn_DeleteMenuItems(ppc_cpu *c)
{
    gaddr m = R(0);
    if (!menu_ok(m)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    int first = (int)(uint16_t)R(1);
    if (!item_in(m, first)) {
        RETURN((uint32_t)menuItemNotFoundErr);
        return;
    }
    for (uint32_t i = 0; i < R(2) && item_in(m, first); i++)
        tb_DeleteMenuItem(m, (int16_t)first);
    RETURN(noErr);
}

/* One item's text, marks, key, icon, style, enabling and Mac OS 8 and
 * CarbonLib fields copied over another's. */
static void copy_item(gaddr src, int si, gaddr dst, int di)
{
    gaddr buf = tb_NewPtr(256, true, true);
    if (!buf)
        return;
    tb_GetMenuItemText(src, (int16_t)si, buf);
    tb_SetMenuItemText(dst, (int16_t)di, buf);
    tb_GetItemCmd(src, (int16_t)si, buf);
    tb_SetItemCmd(dst, (int16_t)di, (int16_t)rd16(buf));
    tb_GetItemMark(src, (int16_t)si, buf);
    tb_SetItemMark(dst, (int16_t)di, (int16_t)rd16(buf));
    tb_GetItemIcon(src, (int16_t)si, buf);
    tb_SetItemIcon(dst, (int16_t)di, (uint8_t)rd16(buf));
    tb_GetItemStyle(src, (int16_t)si, buf);
    tb_SetItemStyle(dst, (int16_t)di, (int16_t)rd8(buf));
    if (tb_IsMenuItemEnabled(src, (uint16_t)si))
        tb_EnableItem(dst, (int16_t)di);
    else
        tb_DisableItem(dst, (int16_t)di);
    if (tb_GetMenuItemCommandID(src, (int16_t)si, buf) == noErr && rd32(buf))
        tb_SetMenuItemCommandID(dst, (int16_t)di, rd32(buf));
    if (tb_GetMenuItemModifiers(src, (int16_t)si, buf) == noErr && rd8(buf))
        tb_SetMenuItemModifiers(dst, (int16_t)di, rd8(buf));
    if (tb_GetMenuItemTextEncoding(src, (int16_t)si, buf) == noErr && rd32(buf))
        tb_SetMenuItemTextEncoding(dst, (int16_t)di, rd32(buf));
    if (tb_GetMenuItemFontID(src, (int16_t)si, buf) == noErr && rd16(buf))
        tb_SetMenuItemFontID(dst, (int16_t)di, (int16_t)rd16(buf));
    if (tb_GetMenuItemRefCon(src, (int16_t)si, buf) == noErr && rd32(buf))
        tb_SetMenuItemRefCon(dst, (int16_t)di, rd32(buf));
    if (tb_GetMenuItemKeyGlyph(src, (int16_t)si, buf) == noErr && rd16(buf))
        tb_SetMenuItemKeyGlyph(dst, (int16_t)di, (int16_t)rd16(buf));
    if (tb_GetMenuItemIconHandle(src, (int16_t)si, buf, buf + 4) == noErr && rd32(buf + 4))
        tb_SetMenuItemIconHandle(dst, (int16_t)di, rd8(buf), rd32(buf + 4));
    tb_DisposePtr(buf);
    hi_item *from = menu_hi_item(src, si, false);
    if (!from)
        return;
    hi_item copy = *from;
    copy.props = NULL;
    hi_prop **tail = &copy.props;
    for (hi_prop *p = from->props; p; p = p->next) {
        hi_prop *q = malloc(sizeof *q);
        if (!q)
            break;
        *q = *p;
        q->data = malloc(p->size ? p->size : 1);
        if (q->data && p->size)
            memcpy(q->data, p->data, p->size);
        q->next = NULL;
        *tail = q;
        tail = &q->next;
    }
    hi_item *to = menu_hi_item(dst, di, true);
    if (to) {
        hi_item_free(to);
        *to = copy;
    } else {
        hi_item_free(&copy);
    }
}

/* CopyMenuItems(source, firstItem, numItems, dest, insertAfter) */
static void mn_CopyMenuItems(ppc_cpu *c)
{
    gaddr src = R(0), dst = R(3);
    int first = (int)(uint16_t)R(1), after = (int)(uint16_t)R(4);
    if (!menu_ok(src) || !menu_ok(dst)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    if (!item_in(src, first) || (uint32_t)first + R(2) - 1 > (uint32_t)count(src)) {
        RETURN((uint32_t)menuItemNotFoundErr);
        return;
    }
    if (after > count(dst))
        after = count(dst);
    uint32_t n = R(2);
    /* (copying a menu's items into itself: the source items move along) */
    for (uint32_t i = 0; i < n; i++) {
        int at = insert_text(dst, 0, after + (int)i);
        if (!at) {
            RETURN((uint32_t)memFullErr);
            return;
        }
        int si = first + (int)i;
        if (src == dst && si >= at)
            si++;
        copy_item(src, si, dst, at);
    }
    tb_CalcMenuSize(dst);
    RETURN(noErr);
}

/* DuplicateMenu(source, outMenu): its ID, title, definition, attributes
 * and items. */
static void mn_DuplicateMenu(ppc_cpu *c)
{
    gaddr src = R(0);
    if (!menu_ok(src) || !R(1)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    gaddr m = new_menu(MenuInfo_menuID(rd32(src)), menu_attributes(src));
    if (!m) {
        RETURN((uint32_t)memFullErr);
        return;
    }
    menu_set_title(m, rd32(src) + MenuInfo_menuData_off);
    MenuInfo_set_menuProc(rd32(m), MenuInfo_menuProc(rd32(src)));
    for (int i = 1; i <= count(src); i++) {
        int at = insert_text(m, 0, i - 1);
        if (at)
            copy_item(src, i, m, at);
    }
    MenuInfo_set_enableFlags(rd32(m), MenuInfo_enableFlags(rd32(src)));
    tb_CalcMenuSize(m);
    wr32(R(1), m);
    RETURN(noErr);
}

/* ---- Item attributes, indents, keys and submenus ---- */

static uint32_t item_attributes(gaddr m, int i)
{
    hi_item *h = menu_hi_item(m, i, false);
    uint32_t a = h ? h->attributes : 0;
    a &= ~(uint32_t)(kMenuItemAttrDisabled | kMenuItemAttrIconDisabled);
    if (!tb_IsMenuItemEnabled(m, (uint16_t)i) || (h && (h->attributes & kMenuItemAttrDisabled)))
        a |= kMenuItemAttrDisabled;
    if (!tb_IsMenuItemIconEnabled(m, (uint16_t)i))
        a |= kMenuItemAttrIconDisabled;
    return a;
}

static void mn_GetMenuItemAttributes(ppc_cpu *c)
{
    int i = (int)(uint16_t)R(1);
    if (!item_in(R(0), i) || !R(2)) {
        RETURN((uint32_t)(menu_ok(R(0)) ? menuItemNotFoundErr : paramErr));
        return;
    }
    wr32(R(2), item_attributes(R(0), i));
    RETURN(noErr);
}

/* ChangeMenuItemAttributes: disabling through the Menu Manager's own
 * enabling (and, past the 31 items its flags hold, kept here). */
static int16_t change_item_attributes(gaddr m, int i, uint32_t set, uint32_t clear)
{
    if (!item_in(m, i))
        return menu_ok(m) ? menuItemNotFoundErr : paramErr;
    uint32_t a = (item_attributes(m, i) | set) & ~clear;
    hi_item *h = menu_hi_item(m, i, true);
    if (!h)
        return memFullErr;
    h->attributes = a;
    if (a & kMenuItemAttrDisabled)
        tb_DisableItem(m, (int16_t)i);
    else
        tb_EnableItem(m, (int16_t)i);
    if (a & kMenuItemAttrIconDisabled)
        tb_DisableMenuItemIcon(m, (uint16_t)i);
    else
        tb_EnableMenuItemIcon(m, (uint16_t)i);
    if (set & kMenuItemAttrSeparator) {
        gaddr dash = tb_NewPtr(4, true, true);
        pstr_from_c(dash, "-");
        tb_SetMenuItemText(m, (int16_t)i, dash);
        tb_DisposePtr(dash);
    }
    return noErr;
}

static void mn_ChangeMenuItemAttributes(ppc_cpu *c)
{
    RETURN((uint32_t)(int32_t)change_item_attributes(R(0), (int)(uint16_t)R(1), R(2), R(3)));
}

static void mn_GetMenuItemIndent(ppc_cpu *c)
{
    int i = (int)(uint16_t)R(1);
    if (!item_in(R(0), i) || !R(2)) {
        RETURN((uint32_t)(menu_ok(R(0)) ? menuItemNotFoundErr : paramErr));
        return;
    }
    hi_item *h = menu_hi_item(R(0), i, false);
    wr32(R(2), h ? h->indent : 0);
    RETURN(noErr);
}

static void mn_SetMenuItemIndent(ppc_cpu *c)
{
    int i = (int)(uint16_t)R(1);
    hi_item *h = item_in(R(0), i) ? menu_hi_item(R(0), i, true) : NULL;
    if (!h) {
        RETURN((uint32_t)(menu_ok(R(0)) ? menuItemNotFoundErr : paramErr));
        return;
    }
    h->indent = R(2);
    RETURN(noErr);
}

/* GetMenuItemCommandKey(menu, item, getVirtualKey, outKey) and
 * SetMenuItemCommandKey: the character is the item's key equivalent; the
 * virtual key is kept beside it. */
static void mn_GetMenuItemCommandKey(ppc_cpu *c)
{
    gaddr m = R(0);
    int i = (int)(uint16_t)R(1);
    if (!item_in(m, i) || !R(3)) {
        RETURN((uint32_t)(menu_ok(m) ? menuItemNotFoundErr : paramErr));
        return;
    }
    if (R(2) & 0xFF) {
        hi_item *h = menu_hi_item(m, i, false);
        if (!h || !(h->attributes & kMenuItemAttrUseVirtualKey)) {
            RETURN((uint32_t)paramErr);
            return;
        }
        wr16(R(3), h->virtual_key);
    } else {
        gaddr buf = tb_NewPtr(2, true, true);
        tb_GetItemCmd(m, (int16_t)i, buf);
        uint16_t k = rd16(buf);
        wr16(R(3), k == HIER_MARK ? 0 : k);
        tb_DisposePtr(buf);
    }
    RETURN(noErr);
}

static void mn_SetMenuItemCommandKey(ppc_cpu *c)
{
    gaddr m = R(0);
    int i = (int)(uint16_t)R(1);
    if (!item_in(m, i)) {
        RETURN((uint32_t)(menu_ok(m) ? menuItemNotFoundErr : paramErr));
        return;
    }
    if (R(2) & 0xFF) {
        hi_item *h = menu_hi_item(m, i, true);
        if (!h) {
            RETURN((uint32_t)memFullErr);
            return;
        }
        h->virtual_key = (uint16_t)R(3);
        h->attributes |= kMenuItemAttrUseVirtualKey;
    } else {
        hi_item *h = menu_hi_item(m, i, false);
        if (h)
            h->attributes &= ~(uint32_t)kMenuItemAttrUseVirtualKey;
        tb_SetItemCmd(m, (int16_t)i, (int16_t)(R(3) & 0xFF));
    }
    RETURN(noErr);
}

/* GetMenuItemHierarchicalMenu / SetMenuItemHierarchicalMenu(menu, item,
 * hierMenu): by the submenu's ID, the submenu put in the Menu Manager's
 * list of submenus. */
static void mn_GetMenuItemHierarchicalMenu(ppc_cpu *c)
{
    int i = (int)(uint16_t)R(1);
    if (!item_in(R(0), i) || !R(2)) {
        RETURN((uint32_t)(menu_ok(R(0)) ? menuItemNotFoundErr : paramErr));
        return;
    }
    wr32(R(2), submenu_of(R(0), i));
    RETURN(noErr);
}

static void mn_SetMenuItemHierarchicalMenu(ppc_cpu *c)
{
    gaddr m = R(0), sub = R(2);
    int i = (int)(uint16_t)R(1);
    if (!item_in(m, i)) {
        RETURN((uint32_t)(menu_ok(m) ? menuItemNotFoundErr : paramErr));
        return;
    }
    if (!sub) {
        tb_SetMenuItemHierarchicalID(m, (int16_t)i, 0);
        RETURN(noErr);
        return;
    }
    if (!menu_ok(sub)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    int16_t id = MenuInfo_menuID(rd32(sub));
    if (tb_GetMenuHandle(id) != sub)
        tb_InsertMenu(sub, -1);
    tb_SetMenuItemHierarchicalID(m, (int16_t)i, id);
    RETURN(noErr);
}

/* ---- Properties ---- */

static hi_prop *find_prop(gaddr m, int i, uint32_t creator, uint32_t tag)
{
    hi_item *h = menu_hi_item(m, i, false);
    for (hi_prop *p = h ? h->props : NULL; p; p = p->next)
        if (p->creator == creator && p->tag == tag)
            return p;
    return NULL;
}

static int16_t set_prop(gaddr m, int i, uint32_t creator, uint32_t tag, uint32_t size, gaddr data)
{
    if (!item_in(m, i) && !(menu_ok(m) && i == 0))
        return menu_ok(m) ? menuItemNotFoundErr : paramErr;
    if (!creator)
        return menuPropertyInvalidErr;
    hi_prop *p = find_prop(m, i, creator, tag);
    uint8_t *d = malloc(size ? size : 1);
    if (!d)
        return memFullErr;
    if (size && data)
        gget(d, data, size);
    if (!p) {
        hi_item *h = i ? menu_hi_item(m, i, true) : NULL;
        if (!h) {
            free(d);
            return i ? memFullErr : paramErr;
        }
        p = calloc(1, sizeof *p);
        if (!p) {
            free(d);
            return memFullErr;
        }
        p->creator = creator;
        p->tag = tag;
        p->next = h->props;
        h->props = p;
    }
    free(p->data);
    p->data = d;
    p->size = size;
    return noErr;
}

static int16_t remove_prop(gaddr m, int i, uint32_t creator, uint32_t tag)
{
    hi_item *h = menu_hi_item(m, i, false);
    for (hi_prop **pp = h ? &h->props : NULL; pp && *pp; pp = &(*pp)->next)
        if ((*pp)->creator == creator && (*pp)->tag == tag) {
            hi_prop *p = *pp;
            *pp = p->next;
            free(p->data);
            free(p);
            return noErr;
        }
    return menuPropertyNotFoundErr;
}

/* GetMenuItemProperty(menu, item, creator, tag, bufferSize, actualSize,
 * buffer) */
static void mn_GetMenuItemProperty(ppc_cpu *c)
{
    hi_prop *p = find_prop(R(0), (int)(uint16_t)R(1), R(2), R(3));
    if (!p) {
        RETURN((uint32_t)menuPropertyNotFoundErr);
        return;
    }
    uint32_t n = p->size < R(4) ? p->size : R(4);
    if (R(6) && n)
        gput(R(6), p->data, n);
    if (R(5))
        wr32(R(5), n);
    RETURN(noErr);
}

static void mn_GetMenuItemPropertySize(ppc_cpu *c)
{
    hi_prop *p = find_prop(R(0), (int)(uint16_t)R(1), R(2), R(3));
    if (p && R(4))
        wr32(R(4), p->size);
    RETURN(p ? noErr : (uint32_t)menuPropertyNotFoundErr);
}

static void mn_SetMenuItemProperty(ppc_cpu *c)
{
    RETURN((uint32_t)(int32_t)set_prop(R(0), (int)(uint16_t)R(1), R(2), R(3), R(4), R(5)));
}

static void mn_RemoveMenuItemProperty(ppc_cpu *c)
{
    RETURN((uint32_t)(int32_t)remove_prop(R(0), (int)(uint16_t)R(1), R(2), R(3)));
}

static void mn_GetMenuItemPropertyAttributes(ppc_cpu *c)
{
    hi_prop *p = find_prop(R(0), (int)(uint16_t)R(1), R(2), R(3));
    if (p && R(4))
        wr32(R(4), p->attributes);
    RETURN(p ? noErr : (uint32_t)menuPropertyNotFoundErr);
}

/* ChangeMenuItemPropertyAttributes(menu, item, creator, tag, set, clear) */
static void mn_ChangeMenuItemPropertyAttributes(ppc_cpu *c)
{
    hi_prop *p = find_prop(R(0), (int)(uint16_t)R(1), R(2), R(3));
    if (p)
        p->attributes = (p->attributes | R(4)) & ~R(5);
    RETURN(p ? noErr : (uint32_t)menuPropertyNotFoundErr);
}

/* ---- Command IDs ---- */

static uint32_t command_of(gaddr m, int i)
{
    gaddr v = tb_NewPtr(4, true, true);
    uint32_t cmd = 0;
    if (v && tb_GetMenuItemCommandID(m, (int16_t)i, v) == noErr)
        cmd = rd32(v);
    if (v)
        tb_DisposePtr(v);
    return cmd;
}

/* The nth item (1-based) with a command, searched from a menu down its
 * submenus (the menu bar's menus when menu is NULL); its index. */
static int find_command(gaddr m, uint32_t cmd, uint32_t *nth, gaddr *found, int depth)
{
    if (!menu_ok(m) || depth > 8)
        return 0;
    for (int i = 1; i <= count(m); i++) {
        if (command_of(m, i) == cmd && --*nth == 0) {
            *found = m;
            return i;
        }
        gaddr sub = submenu_of(m, i);
        if (sub && sub != m) {
            int k = find_command(sub, cmd, nth, found, depth + 1);
            if (k)
                return k;
        }
    }
    return 0;
}

static int lookup_command(gaddr m, uint32_t cmd, uint32_t index, gaddr *found)
{
    uint32_t nth = index ? index : 1;
    if (m)
        return find_command(m, cmd, &nth, found, 0);
    for (int b = 0; b < bar_count(); b++) {
        int k = find_command(bar_menu(b), cmd, &nth, found, 0);
        if (k)
            return k;
    }
    return 0;
}

/* CountMenuItemsWithCommandID(menu, commandID) */
static void mn_CountMenuItemsWithCommandID(ppc_cpu *c)
{
    uint32_t n = 0;
    gaddr found;
    while (n < 1000 && lookup_command(R(0), R(1), n + 1, &found))
        n++;
    RETURN(n);
}

/* GetIndMenuItemWithCommandID(menu, commandID, itemIndex, outMenu,
 * outIndex) */
static void mn_GetIndMenuItemWithCommandID(ppc_cpu *c)
{
    gaddr found = 0;
    int k = lookup_command(R(0), R(1), R(2), &found);
    if (!k) {
        RETURN((uint32_t)menuItemNotFoundErr);
        return;
    }
    if (R(3))
        wr32(R(3), found);
    if (R(4))
        wr16(R(4), (uint16_t)k);
    RETURN(noErr);
}

static void mn_EnableMenuCommand(ppc_cpu *c)
{
    gaddr found = 0;
    int k = lookup_command(R(0), R(1), 1, &found);
    if (k)
        change_item_attributes(found, k, 0, kMenuItemAttrDisabled);
    DONE();
}

static void mn_DisableMenuCommand(ppc_cpu *c)
{
    gaddr found = 0;
    int k = lookup_command(R(0), R(1), 1, &found);
    if (k)
        change_item_attributes(found, k, kMenuItemAttrDisabled, 0);
    DONE();
}

static void mn_IsMenuCommandEnabled(ppc_cpu *c)
{
    gaddr found = 0;
    int k = lookup_command(R(0), R(1), 1, &found);
    RETURN(k && item_enabled(found, k));
}

/* GetMenuCommandMark(menu, command, outMark) / SetMenuCommandMark */
static void mn_GetMenuCommandMark(ppc_cpu *c)
{
    gaddr found = 0;
    int k = lookup_command(R(0), R(1), 1, &found);
    if (!k) {
        RETURN((uint32_t)menuItemNotFoundErr);
        return;
    }
    gaddr buf = tb_NewPtr(2, true, true);
    tb_GetItemMark(found, (int16_t)k, buf);
    if (R(2))
        wr16(R(2), rd16(buf));
    tb_DisposePtr(buf);
    RETURN(noErr);
}

static void mn_SetMenuCommandMark(ppc_cpu *c)
{
    gaddr found = 0;
    int k = lookup_command(R(0), R(1), 1, &found);
    if (!k) {
        RETURN((uint32_t)menuItemNotFoundErr);
        return;
    }
    tb_SetItemMark(found, (int16_t)k, (int16_t)(R(2) & 0xFF));
    RETURN(noErr);
}

/* The command property calls: the first item with the command's. */
static void mn_GetMenuCommandProperty(ppc_cpu *c)
{
    gaddr found = 0;
    int k = lookup_command(R(0), R(1), 1, &found);
    hi_prop *p = k ? find_prop(found, k, R(2), R(3)) : NULL;
    if (!p) {
        RETURN((uint32_t)(k ? menuPropertyNotFoundErr : menuItemNotFoundErr));
        return;
    }
    uint32_t n = p->size < R(4) ? p->size : R(4);
    if (R(6) && n)
        gput(R(6), p->data, n);
    if (R(5))
        wr32(R(5), n);
    RETURN(noErr);
}

static void mn_GetMenuCommandPropertySize(ppc_cpu *c)
{
    gaddr found = 0;
    int k = lookup_command(R(0), R(1), 1, &found);
    hi_prop *p = k ? find_prop(found, k, R(2), R(3)) : NULL;
    if (p && R(4))
        wr32(R(4), p->size);
    RETURN(p ? noErr : (uint32_t)(k ? menuPropertyNotFoundErr : menuItemNotFoundErr));
}

static void mn_SetMenuCommandProperty(ppc_cpu *c)
{
    gaddr found = 0;
    int k = lookup_command(R(0), R(1), 1, &found);
    RETURN(k ? (uint32_t)(int32_t)set_prop(found, k, R(2), R(3), R(4), R(5)) : (uint32_t)menuItemNotFoundErr);
}

static void mn_RemoveMenuCommandProperty(ppc_cpu *c)
{
    gaddr found = 0;
    int k = lookup_command(R(0), R(1), 1, &found);
    RETURN(k ? (uint32_t)(int32_t)remove_prop(found, k, R(2), R(3)) : (uint32_t)menuItemNotFoundErr);
}

/* ---- SetMenuItemData / CopyMenuItemData ---- */

/* MenuItemDataRec (mac68k alignment) */
enum {
    MID_WHICH = 0, MID_TEXT = 4, MID_MARK = 8, MID_CMDKEY = 10, MID_GLYPH = 12, MID_MODIFIERS = 16, MID_STYLE = 20,
    MID_ENABLED = 21, MID_ICON_ENABLED = 22, MID_ICON_ID = 24, MID_ICON_TYPE = 28, MID_ICON_HANDLE = 32,
    MID_CMD_ID = 36, MID_ENCODING = 40, MID_SUBMENU_ID = 44, MID_SUBMENU = 46, MID_FONT = 50, MID_REFCON = 54,
    MID_ATTR = 58, MID_CFTEXT = 62, MID_PROPERTIES = 66, MID_INDENT = 70, MID_VIRTUAL_KEY = 74,
};
enum {
    kMenuItemDataText = 1 << 0, kMenuItemDataMark = 1 << 1, kMenuItemDataCmdKey = 1 << 2,
    kMenuItemDataCmdKeyGlyph = 1 << 3, kMenuItemDataCmdKeyModifiers = 1 << 4, kMenuItemDataStyle = 1 << 5,
    kMenuItemDataEnabled = 1 << 6, kMenuItemDataIconEnabled = 1 << 7, kMenuItemDataIconID = 1 << 8,
    kMenuItemDataIconHandle = 1 << 9, kMenuItemDataCommandID = 1 << 10, kMenuItemDataTextEncoding = 1 << 11,
    kMenuItemDataSubmenuID = 1 << 12, kMenuItemDataSubmenuHandle = 1 << 13, kMenuItemDataFontID = 1 << 14,
    kMenuItemDataRefcon = 1 << 15, kMenuItemDataAttributes = 1 << 16, kMenuItemDataCFString = 1 << 17,
    kMenuItemDataProperties = 1 << 18, kMenuItemDataIndent = 1 << 19, kMenuItemDataCmdVirtualKey = 1 << 20,
};

/* The item an ID names: an index, or (isCommandID) the first item with
 * that command, in the menu or its submenus. */
static int resolve_item(gaddr *m, uint32_t id, bool is_command)
{
    if (!is_command)
        return item_in(*m, (int)id) ? (int)id : 0;
    gaddr found = 0;
    int k = lookup_command(*m, id, 1, &found);
    if (k)
        *m = found;
    return k;
}

static void mn_SetMenuItemData(ppc_cpu *c)
{
    gaddr m = R(0), d = R(3);
    int i = resolve_item(&m, R(1), (R(2) & 0xFF) != 0);
    if (!d || !menu_ok(m)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    if (!i) {
        RETURN((uint32_t)menuItemNotFoundErr);
        return;
    }
    uint32_t which = rd32(d + MID_WHICH);
    int16_t item = (int16_t)i;
    if ((which & kMenuItemDataText) && rd32(d + MID_TEXT))
        tb_SetMenuItemText(m, item, rd32(d + MID_TEXT));
    if (which & kMenuItemDataCFString) {
        gaddr p = tb_NewPtr(256, true, true);
        hi_cf_to_pstr(rd32(d + MID_CFTEXT), p);
        tb_SetMenuItemText(m, item, p);
        tb_DisposePtr(p);
    }
    if (which & kMenuItemDataMark)
        tb_SetItemMark(m, item, (int16_t)(rd16(d + MID_MARK) & 0xFF));
    if (which & kMenuItemDataCmdKey)
        tb_SetItemCmd(m, item, (int16_t)(rd16(d + MID_CMDKEY) & 0xFF));
    if (which & kMenuItemDataCmdKeyGlyph)
        tb_SetMenuItemKeyGlyph(m, item, (int16_t)rd32(d + MID_GLYPH));
    if (which & kMenuItemDataCmdKeyModifiers)
        tb_SetMenuItemModifiers(m, item, (uint8_t)rd32(d + MID_MODIFIERS));
    if (which & kMenuItemDataStyle)
        tb_SetItemStyle(m, item, rd8(d + MID_STYLE));
    if (which & kMenuItemDataEnabled)
        change_item_attributes(m, i, rd8(d + MID_ENABLED) ? 0 : kMenuItemAttrDisabled,
                               rd8(d + MID_ENABLED) ? kMenuItemAttrDisabled : 0);
    if (which & kMenuItemDataIconEnabled) {
        if (rd8(d + MID_ICON_ENABLED))
            tb_EnableMenuItemIcon(m, (uint16_t)i);
        else
            tb_DisableMenuItemIcon(m, (uint16_t)i);
    }
    if (which & kMenuItemDataIconID)
        tb_SetItemIcon(m, item, (uint8_t)(rd32(d + MID_ICON_ID) - 256));
    if (which & kMenuItemDataIconHandle)
        tb_SetMenuItemIconHandle(m, item, (uint8_t)rd32(d + MID_ICON_TYPE), rd32(d + MID_ICON_HANDLE));
    if (which & kMenuItemDataCommandID)
        tb_SetMenuItemCommandID(m, item, rd32(d + MID_CMD_ID));
    if (which & kMenuItemDataTextEncoding)
        tb_SetMenuItemTextEncoding(m, item, rd32(d + MID_ENCODING));
    if (which & kMenuItemDataSubmenuID)
        tb_SetMenuItemHierarchicalID(m, item, (int16_t)rd16(d + MID_SUBMENU_ID));
    if ((which & kMenuItemDataSubmenuHandle) && menu_ok(rd32(d + MID_SUBMENU))) {
        gaddr sub = rd32(d + MID_SUBMENU);
        int16_t id = MenuInfo_menuID(rd32(sub));
        if (tb_GetMenuHandle(id) != sub)
            tb_InsertMenu(sub, -1);
        tb_SetMenuItemHierarchicalID(m, item, id);
    }
    if (which & kMenuItemDataFontID)
        tb_SetMenuItemFontID(m, item, (int16_t)rd32(d + MID_FONT));
    if (which & kMenuItemDataRefcon)
        tb_SetMenuItemRefCon(m, item, rd32(d + MID_REFCON));
    if (which & kMenuItemDataAttributes)
        change_item_attributes(m, i, rd32(d + MID_ATTR), ~rd32(d + MID_ATTR));
    hi_item *h = menu_hi_item(m, i, true);
    if (h && (which & kMenuItemDataIndent))
        h->indent = rd32(d + MID_INDENT);
    if (h && (which & kMenuItemDataCmdVirtualKey)) {
        h->virtual_key = rd16(d + MID_VIRTUAL_KEY);
        h->attributes |= kMenuItemAttrUseVirtualKey;
    }
    tb_CalcMenuSize(m);
    RETURN(noErr);
}

static void mn_CopyMenuItemData(ppc_cpu *c)
{
    gaddr m = R(0), d = R(3);
    int i = resolve_item(&m, R(1), (R(2) & 0xFF) != 0);
    if (!d || !menu_ok(m)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    if (!i) {
        RETURN((uint32_t)menuItemNotFoundErr);
        return;
    }
    uint32_t which = rd32(d + MID_WHICH);
    int16_t item = (int16_t)i;
    gaddr buf = tb_NewPtr(256, true, true);
    if ((which & kMenuItemDataText) && rd32(d + MID_TEXT))
        tb_GetMenuItemText(m, item, rd32(d + MID_TEXT));
    if (which & kMenuItemDataCFString) {
        tb_GetMenuItemText(m, item, buf);
        wr32(d + MID_CFTEXT, hi_cf_from_pstr(buf));
    }
    if (which & kMenuItemDataMark) {
        tb_GetItemMark(m, item, buf);
        wr16(d + MID_MARK, rd16(buf));
    }
    if (which & kMenuItemDataCmdKey) {
        tb_GetItemCmd(m, item, buf);
        wr16(d + MID_CMDKEY, rd16(buf) == HIER_MARK ? 0 : rd16(buf));
    }
    if (which & kMenuItemDataCmdKeyGlyph) {
        tb_GetMenuItemKeyGlyph(m, item, buf);
        wr32(d + MID_GLYPH, (uint32_t)(int32_t)(int16_t)rd16(buf));
    }
    if (which & kMenuItemDataCmdKeyModifiers) {
        tb_GetMenuItemModifiers(m, item, buf);
        wr32(d + MID_MODIFIERS, rd8(buf));
    }
    if (which & kMenuItemDataStyle) {
        tb_GetItemStyle(m, item, buf);
        wr8(d + MID_STYLE, rd8(buf));
    }
    if (which & kMenuItemDataEnabled)
        wr8(d + MID_ENABLED, item_enabled(m, i));
    if (which & kMenuItemDataIconEnabled)
        wr8(d + MID_ICON_ENABLED, tb_IsMenuItemIconEnabled(m, (uint16_t)i) != 0);
    if (which & kMenuItemDataIconID) {
        tb_GetItemIcon(m, item, buf);
        wr32(d + MID_ICON_ID, rd16(buf) ? rd16(buf) + 256u : 0);
    }
    if (which & kMenuItemDataIconHandle) {
        wr32(buf, 0);
        wr32(buf + 4, 0);
        tb_GetMenuItemIconHandle(m, item, buf + 3, buf + 4);
        wr32(d + MID_ICON_TYPE, rd8(buf + 3));
        wr32(d + MID_ICON_HANDLE, rd32(buf + 4));
    }
    if (which & kMenuItemDataCommandID)
        wr32(d + MID_CMD_ID, command_of(m, i));
    if (which & kMenuItemDataTextEncoding) {
        tb_GetMenuItemTextEncoding(m, item, buf);
        wr32(d + MID_ENCODING, rd32(buf));
    }
    if (which & kMenuItemDataSubmenuID) {
        tb_GetMenuItemHierarchicalID(m, item, buf);
        wr16(d + MID_SUBMENU_ID, rd16(buf));
    }
    if (which & kMenuItemDataSubmenuHandle)
        wr32(d + MID_SUBMENU, submenu_of(m, i));
    if (which & kMenuItemDataFontID) {
        wr16(buf, 0);
        tb_GetMenuItemFontID(m, item, buf);
        wr32(d + MID_FONT, (uint32_t)(int32_t)(int16_t)rd16(buf));
    }
    if (which & kMenuItemDataRefcon) {
        tb_GetMenuItemRefCon(m, item, buf);
        wr32(d + MID_REFCON, rd32(buf));
    }
    if (which & kMenuItemDataAttributes)
        wr32(d + MID_ATTR, item_attributes(m, i));
    hi_item *h = menu_hi_item(m, i, false);
    if (which & kMenuItemDataProperties)
        wr32(d + MID_PROPERTIES, 0); /* (no collection of them is made) */
    if (which & kMenuItemDataIndent)
        wr32(d + MID_INDENT, h ? h->indent : 0);
    if (which & kMenuItemDataCmdVirtualKey)
        wr16(d + MID_VIRTUAL_KEY, h ? h->virtual_key : 0);
    tb_DisposePtr(buf);
    RETURN(noErr);
}

/* ---- Title icons ---- */

static void mn_SetMenuTitleIcon(ppc_cpu *c)
{
    hi_menu *h = menu_ok(R(0)) ? menu_hi_menu(R(0), true) : NULL;
    if (!h) {
        RETURN((uint32_t)paramErr);
        return;
    }
    /* (kMenuNoIcon, kMenuIconSuiteType and kMenuIconRefType) */
    if (R(1) != 0 && R(1) != 5 && R(1) != 6) {
        RETURN((uint32_t)paramErr);
        return;
    }
    h->title_icon_type = R(1);
    h->title_icon = R(1) ? R(2) : 0;
    tb_InvalMenuBar();
    RETURN(noErr);
}

static void mn_GetMenuTitleIcon(ppc_cpu *c)
{
    if (!menu_ok(R(0))) {
        RETURN((uint32_t)paramErr);
        return;
    }
    hi_menu *h = menu_hi_menu(R(0), false);
    if (R(1))
        wr32(R(1), h ? h->title_icon_type : 0);
    if (R(2))
        wr32(R(2), h ? h->title_icon : 0);
    RETURN(noErr);
}

/* ---- Font menus ---- */

/* CreateStandardFontMenu(menu, afterItem, firstHierMenuID, options,
 * outHierMenuCount): the fonts' names, in order (one item a family). */
static void mn_CreateStandardFontMenu(ppc_cpu *c)
{
    if (!menu_ok(R(0))) {
        RETURN((uint32_t)paramErr);
        return;
    }
    tb_InsertResMenu(R(0), (int32_t)0x464F4E54 /* 'FONT' */, (int16_t)R(1));
    if (R(4))
        wr32(R(4), 0);
    RETURN(noErr);
}

static void mn_UpdateStandardFontMenu(ppc_cpu *c)
{
    if (R(1))
        wr32(R(1), 0);
    RETURN(menu_ok(R(0)) ? noErr : (uint32_t)paramErr);
}

/* GetFontFamilyFromMenuSelection(menu, item, outFontFamily, outStyle) */
static void mn_GetFontFamilyFromMenuSelection(ppc_cpu *c)
{
    int i = (int)(uint16_t)R(1);
    if (!item_in(R(0), i)) {
        RETURN((uint32_t)(menu_ok(R(0)) ? menuItemNotFoundErr : paramErr));
        return;
    }
    gaddr buf = tb_NewPtr(260, true, true);
    tb_GetMenuItemText(R(0), (int16_t)i, buf);
    wr16(buf + 256, 0xFFFF);
    tb_GetFNum(buf, buf + 256);
    int16_t fam = (int16_t)rd16(buf + 256);
    /* (GetFNum answers 0 for a name it doesn't know, and for the system
     * font's) */
    bool known = fam != 0;
    if (!known) {
        gaddr sys = tb_NewPtr(256, true, true);
        tb_GetFontName(0, sys);
        known = rd8(sys) && rd8(sys) == rd8(buf);
        for (uint32_t k = 1; known && k <= rd8(buf); k++)
            known = rd8(sys + k) == rd8(buf + k);
        tb_DisposePtr(sys);
    }
    tb_DisposePtr(buf);
    if (!known) {
        RETURN((uint32_t)kFMInvalidFontFamilyErr);
        return;
    }
    if (R(2))
        wr16(R(2), (uint16_t)fam);
    if (R(3))
        wr16(R(3), 0);
    RETURN(noErr);
}

/* ---- CFString text ---- */

static void mn_AppendMenuItemTextWithCFString(ppc_cpu *c)
{
    gaddr m = R(0);
    if (!menu_ok(m)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    gaddr p = tb_NewPtr(256, true, true);
    hi_cf_to_pstr(R(1), p);
    int at = insert_text(m, p, count(m));
    tb_DisposePtr(p);
    if (!at) {
        RETURN((uint32_t)memFullErr);
        return;
    }
    if (R(2))
        change_item_attributes(m, at, R(2), 0);
    if (R(3))
        tb_SetMenuItemCommandID(m, (int16_t)at, R(3));
    if (R(4))
        wr16(R(4), (uint16_t)at);
    RETURN(noErr);
}

/* InsertMenuItemTextWithCFString(menu, string, afterItem, attributes,
 * commandID) */
static void mn_InsertMenuItemTextWithCFString(ppc_cpu *c)
{
    gaddr m = R(0);
    if (!menu_ok(m)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    gaddr p = tb_NewPtr(256, true, true);
    hi_cf_to_pstr(R(1), p);
    int at = insert_text(m, p, (int)(uint16_t)R(2));
    tb_DisposePtr(p);
    if (!at) {
        RETURN((uint32_t)memFullErr);
        return;
    }
    if (R(3))
        change_item_attributes(m, at, R(3), 0);
    if (R(4))
        tb_SetMenuItemCommandID(m, (int16_t)at, R(4));
    RETURN(noErr);
}

static void mn_SetMenuItemTextWithCFString(ppc_cpu *c)
{
    int i = (int)(uint16_t)R(1);
    if (!item_in(R(0), i)) {
        RETURN((uint32_t)(menu_ok(R(0)) ? menuItemNotFoundErr : paramErr));
        return;
    }
    gaddr p = tb_NewPtr(256, true, true);
    hi_cf_to_pstr(R(2), p);
    tb_SetMenuItemText(R(0), (int16_t)i, p);
    tb_DisposePtr(p);
    RETURN(noErr);
}

static void mn_CopyMenuItemTextAsCFString(ppc_cpu *c)
{
    int i = (int)(uint16_t)R(1);
    if (!item_in(R(0), i) || !R(2)) {
        RETURN((uint32_t)(menu_ok(R(0)) ? menuItemNotFoundErr : paramErr));
        return;
    }
    gaddr p = tb_NewPtr(256, true, true);
    tb_GetMenuItemText(R(0), (int16_t)i, p);
    wr32(R(2), hi_cf_from_pstr(p));
    tb_DisposePtr(p);
    RETURN(rd32(R(2)) ? noErr : (uint32_t)memFullErr);
}

static void mn_SetMenuTitleWithCFString(ppc_cpu *c)
{
    if (!menu_ok(R(0))) {
        RETURN((uint32_t)paramErr);
        return;
    }
    gaddr p = tb_NewPtr(256, true, true);
    hi_cf_to_pstr(R(1), p);
    bool ok = menu_set_title(R(0), p);
    tb_DisposePtr(p);
    RETURN(ok ? noErr : (uint32_t)memFullErr);
}

static void mn_CopyMenuTitleAsCFString(ppc_cpu *c)
{
    if (!menu_ok(R(0)) || !R(1)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    wr32(R(1), hi_cf_from_pstr(rd32(R(0)) + MenuInfo_menuData_off));
    RETURN(rd32(R(1)) ? noErr : (uint32_t)memFullErr);
}

/* ---- Carbon events ---- */

enum { kEventClassMouse = 0x6D6F7573, kEventClassKeyboard = 0x6B657962 };
enum { kEventParamMouseButton = 0x6D62746E, typeMouseButton = 0x6D62746E, kEventParamKeyModifiers = 0x6B6D6F64,
       typeUInt32 = 0x6D61676E, kEventParamKeyMacCharCodes = 0x6B636872, typeChar = 0x54455854 };
enum { cmdKey = 0x0100, shiftKey = 0x0200, optionKey = 0x0800, controlKey = 0x1000 };
enum { kMenuEventIncludeDisabledItems = 1, kMenuEventQueryOnly = 2, kMenuEventDontCheckSubmenus = 4 };

/* A parameter of an event (through the Carbon Event Manager's exports),
 * or false. */
static bool event_param(gaddr event, uint32_t name, uint32_t type, uint32_t size, uint32_t *out)
{
    gaddr buf = tb_NewPtr(8, true, true);
    if (!buf)
        return false;
    uint32_t args[7] = { event, name, type, 0, size, 0, buf };
    bool ok = hi_call("GetEventParameter", args, 7, NULL) == noErr;
    *out = size == 1 ? rd8(buf) : size == 2 ? rd16(buf) : rd32(buf);
    tb_DisposePtr(buf);
    return ok;
}

static bool event_is(gaddr event, uint32_t cls, uint32_t kind1, uint32_t kind2)
{
    uint32_t args[1] = { event };
    bool have = false;
    uint32_t ec = hi_call("GetEventClass", args, 1, &have);
    uint32_t ek = hi_call("GetEventKind", args, 1, &have);
    return have && ec == cls && (ek == kind1 || ek == kind2);
}

/* IsShowContextualMenuEvent(event): a mouse-down with the secondary
 * button, or with the control key. */
static void mn_IsShowContextualMenuEvent(ppc_cpu *c)
{
    gaddr event = R(0);
    if (!event || !event_is(event, kEventClassMouse, 1 /* kEventMouseDown */, 1)) {
        RETURN(0);
        return;
    }
    uint32_t button = 1, mods = 0;
    event_param(event, kEventParamMouseButton, typeMouseButton, 2, &button);
    event_param(event, kEventParamKeyModifiers, typeUInt32, 4, &mods);
    RETURN(button == 2 || (button == 1 && (mods & controlKey)));
}

/* An item a key and its modifiers choose: its key equivalent the
 * character, its Mac OS 8 modifiers those held (the command key unless
 * kMenuNoCommandModifier). */
static int key_item(gaddr m, uint8_t ch, uint32_t mods, bool disabled_too, bool submenus, gaddr *found, int depth)
{
    if (!menu_ok(m) || depth > 8)
        return 0;
    if (ch >= 'a' && ch <= 'z')
        ch = (uint8_t)(ch - 32);
    gaddr buf = tb_NewPtr(4, true, true);
    for (int i = 1; i <= count(m); i++) {
        tb_GetItemCmd(m, (int16_t)i, buf);
        uint8_t key = (uint8_t)rd16(buf);
        if (key == HIER_MARK) {
            gaddr sub = submenus ? submenu_of(m, i) : 0;
            int k = sub && sub != m ? key_item(sub, ch, mods, disabled_too, submenus, found, depth + 1) : 0;
            if (k) {
                tb_DisposePtr(buf);
                return k;
            }
            continue;
        }
        if (key >= 'a' && key <= 'z')
            key = (uint8_t)(key - 32);
        if (!key || key != ch || (!disabled_too && !item_enabled(m, i)))
            continue;
        wr8(buf, 0);
        tb_GetMenuItemModifiers(m, (int16_t)i, buf);
        uint8_t want = rd8(buf);
        bool cmd_needed = !(want & 8);
        if (cmd_needed != ((mods & cmdKey) != 0) || ((want & 1) != 0) != ((mods & shiftKey) != 0) ||
            ((want & 2) != 0) != ((mods & optionKey) != 0) || ((want & 4) != 0) != ((mods & controlKey) != 0))
            continue;
        *found = m;
        tb_DisposePtr(buf);
        return i;
    }
    tb_DisposePtr(buf);
    return 0;
}

/* IsMenuKeyEvent(startMenu, event, options, outMenu, outMenuItem): a
 * key-down a menu item's key equivalent chooses (the title highlighted,
 * unless kMenuEventQueryOnly). */
static void mn_IsMenuKeyEvent(ppc_cpu *c)
{
    gaddr start = R(0), event = R(1);
    uint32_t options = R(2);
    if (!event || !event_is(event, kEventClassKeyboard, 1 /* kEventRawKeyDown */, 2 /* repeat */)) {
        RETURN(0);
        return;
    }
    uint32_t ch = 0, mods = 0;
    if (!event_param(event, kEventParamKeyMacCharCodes, typeChar, 1, &ch)) {
        RETURN(0);
        return;
    }
    event_param(event, kEventParamKeyModifiers, typeUInt32, 4, &mods);
    bool disabled_too = options & kMenuEventIncludeDisabledItems, subs = !(options & kMenuEventDontCheckSubmenus);
    gaddr found = 0;
    int k = 0;
    if (start)
        k = key_item(start, (uint8_t)ch, mods, disabled_too, subs, &found, 0);
    for (int b = bar_count() - 1; !start && b >= 0 && !k; b--)
        k = key_item(bar_menu(b), (uint8_t)ch, mods, disabled_too, subs, &found, 0);
    if (!k) {
        RETURN(0);
        return;
    }
    if (!(options & kMenuEventQueryOnly)) {
        /* the bar's menu the item is (under) highlighted, as MenuKey does */
        gaddr top = found;
        for (int b = 0; b < bar_count(); b++) {
            gaddr dummy = 0;
            if (bar_menu(b) == found || key_item(bar_menu(b), (uint8_t)ch, mods, disabled_too, subs, &dummy, 0))
                top = bar_menu(b);
        }
        if (in_bar(top))
            tb_HiliteMenu(MenuInfo_menuID(rd32(top)));
    }
    if (R(3))
        wr32(R(3), found);
    if (R(4))
        wr16(R(4), (uint16_t)k);
    RETURN(1);
}

/* ---- CarbonLib 1.5's ---- */

/* EraseMenuBackground(menu, eraseRect, context) and ScrollMenuImage(menu,
 * scrollRect, dh, dv, context): in the current port (the MDEF's). */
static void mn_EraseMenuBackground(ppc_cpu *c)
{
    if (!R(1)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    tb_EraseRect(R(1));
    RETURN(noErr);
}

static void mn_ScrollMenuImage(ppc_cpu *c)
{
    if (!R(1)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    gaddr rgn = tb_NewRgn();
    tb_ScrollRect(R(1), (int16_t)R(2), (int16_t)R(3), rgn);
    tb_DisposeRgn(rgn);
    RETURN(noErr);
}

void carbonmenu_reset(void) { memset(registered, 0, sizeof registered); }

const ppc_export carbonmenu_exports[] = {
    { "AcquireRootMenu", mn_AcquireRootMenu },
    { "AppendMenuItemText", mn_AppendMenuItemText },
    { "AppendMenuItemTextWithCFString", mn_AppendMenuItemTextWithCFString },
    { "ChangeMenuAttributes", mn_ChangeMenuAttributes },
    { "ChangeMenuItemAttributes", mn_ChangeMenuItemAttributes },
    { "ChangeMenuItemPropertyAttributes", mn_ChangeMenuItemPropertyAttributes },
    { "CheckMenuItem", mn_CheckMenuItem },
    { "CopyMenuItemData", mn_CopyMenuItemData },
    { "CopyMenuItemTextAsCFString", mn_CopyMenuItemTextAsCFString },
    { "CopyMenuItems", mn_CopyMenuItems },
    { "CopyMenuTitleAsCFString", mn_CopyMenuTitleAsCFString },
    { "CountMenuItems", mn_CountMenuItems },
    { "CountMenuItemsWithCommandID", mn_CountMenuItemsWithCommandID },
    { "CreateCustomMenu", mn_CreateCustomMenu },
    { "CreateNewMenu", mn_CreateNewMenu },
    { "CreateStandardFontMenu", mn_CreateStandardFontMenu },
    { "DeleteMenuItems", mn_DeleteMenuItems },
    { "DisableAllMenuItems", mn_DisableAllMenuItems },
    { "DisableMenuCommand", mn_DisableMenuCommand },
    { "DisableMenuItem", mn_DisableMenuItem },
    { "DisposeMenuBar", mn_DisposeMenuBar },
    { "DuplicateMenu", mn_DuplicateMenu },
    { "DuplicateMenuBar", mn_DuplicateMenuBar },
    { "EnableAllMenuItems", mn_EnableAllMenuItems },
    { "EnableMenuCommand", mn_EnableMenuCommand },
    { "EnableMenuItem", mn_EnableMenuItem },
    { "EraseMenuBackground", mn_EraseMenuBackground },
    { "GetFontFamilyFromMenuSelection", mn_GetFontFamilyFromMenuSelection },
    { "GetIndMenuItemWithCommandID", mn_GetIndMenuItemWithCommandID },
    { "GetMenuAttributes", mn_GetMenuAttributes },
    { "GetMenuCommandMark", mn_GetMenuCommandMark },
    { "GetMenuCommandProperty", mn_GetMenuCommandProperty },
    { "GetMenuCommandPropertySize", mn_GetMenuCommandPropertySize },
    { "GetMenuDefinition", mn_GetMenuDefinition },
    { "GetMenuHeight", mn_GetMenuHeight },
    { "GetMenuID", mn_GetMenuID },
    { "GetMenuItemAttributes", mn_GetMenuItemAttributes },
    { "GetMenuItemCommandKey", mn_GetMenuItemCommandKey },
    { "GetMenuItemHierarchicalMenu", mn_GetMenuItemHierarchicalMenu },
    { "GetMenuItemIndent", mn_GetMenuItemIndent },
    { "GetMenuItemProperty", mn_GetMenuItemProperty },
    { "GetMenuItemPropertyAttributes", mn_GetMenuItemPropertyAttributes },
    { "GetMenuItemPropertySize", mn_GetMenuItemPropertySize },
    { "GetMenuRetainCount", mn_GetMenuRetainCount },
    { "GetMenuTitle", mn_GetMenuTitle },
    { "GetMenuTitleIcon", mn_GetMenuTitleIcon },
    { "GetMenuTrackingData", mn_GetMenuTrackingData },
    { "GetMenuType", mn_GetMenuType },
    { "GetMenuWidth", mn_GetMenuWidth },
    { "HideMenuBar", mn_HideMenuBar },
    { "InsertMenuItemText", mn_InsertMenuItemText },
    { "InsertMenuItemTextWithCFString", mn_InsertMenuItemTextWithCFString },
    { "InvalidateMenuEnabling", mn_InvalidateMenuEnabling },
    { "InvalidateMenuItems", mn_InvalidateMenuItems },
    { "InvalidateMenuSize", mn_InvalidateMenuSize },
    { "IsMenuBarInvalid", mn_false },
    { "IsMenuBarVisible", mn_IsMenuBarVisible },
    { "IsMenuCommandEnabled", mn_IsMenuCommandEnabled },
    { "IsMenuItemInvalid", mn_IsMenuItemInvalid },
    { "IsMenuKeyEvent", mn_IsMenuKeyEvent },
    { "IsMenuSizeInvalid", mn_false },
    { "IsShowContextualMenuEvent", mn_IsShowContextualMenuEvent },
    { "IsValidMenu", mn_IsValidMenu },
    { "MenuHasEnabledItems", mn_MenuHasEnabledItems },
    { "RegisterMenuDefinition", mn_RegisterMenuDefinition },
    { "ReleaseMenu", mn_ReleaseMenu },
    { "RemoveMenuCommandProperty", mn_RemoveMenuCommandProperty },
    { "RemoveMenuItemProperty", mn_RemoveMenuItemProperty },
    { "RetainMenu", mn_RetainMenu },
    { "ScrollMenuImage", mn_ScrollMenuImage },
    { "SetMenuCommandMark", mn_SetMenuCommandMark },
    { "SetMenuCommandProperty", mn_SetMenuCommandProperty },
    { "SetMenuDefinition", mn_SetMenuDefinition },
    { "SetMenuFlashCount", mn_SetMenuFlashCount },
    { "SetMenuHeight", mn_SetMenuHeight },
    { "SetMenuID", mn_SetMenuID },
    { "SetMenuItemCommandKey", mn_SetMenuItemCommandKey },
    { "SetMenuItemData", mn_SetMenuItemData },
    { "SetMenuItemHierarchicalMenu", mn_SetMenuItemHierarchicalMenu },
    { "SetMenuItemIndent", mn_SetMenuItemIndent },
    { "SetMenuItemProperty", mn_SetMenuItemProperty },
    { "SetMenuItemTextWithCFString", mn_SetMenuItemTextWithCFString },
    { "SetMenuTitle", mn_SetMenuTitle },
    { "SetMenuTitleIcon", mn_SetMenuTitleIcon },
    { "SetMenuTitleWithCFString", mn_SetMenuTitleWithCFString },
    { "SetMenuWidth", mn_SetMenuWidth },
    { "SetRootMenu", mn_SetRootMenu },
    { "ShowMenuBar", mn_ShowMenuBar },
    { "UpdateInvalidMenuItems", mn_UpdateInvalidMenuItems },
    { "UpdateStandardFontMenu", mn_UpdateStandardFontMenu },
};
const unsigned carbonmenu_export_count = sizeof carbonmenu_exports / sizeof carbonmenu_exports[0];
