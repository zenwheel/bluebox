/*
 * Menu Manager.
 *
 * MenuInfo: menuID, menuWidth, menuHeight, menuProc (MDEF handle),
 * enableFlags, then the title and items: each item is a Pascal string
 * followed by icon, key equivalent, mark and style bytes; a zero byte ends
 * the list.
 *
 * MenuList (low memory): lastMenu, lastRight, mbResID, then 6-byte entries
 * (menu handle, left edge); hierarchical entries follow (lastHMenu,
 * menuTitleSave, then handle + reserved pairs).
 */
#include <stdlib.h>
#include <string.h>

#include "qd_internal.h"
#include "tb_internal.h"

enum { mDrawMsg = 0, mChooseMsg, mSizeMsg, mPopUpMsg };
enum { ITEM_HEIGHT = 16, MBAR_TITLE_GAP = 13, MBAR_FIRST = 10 };
enum { patCopy = 8, patXor = 10, patBic = 11 };

bool ev_still_down(void);
uint32_t ev_mouse_global(void);
void ev_wait_tick(void);

void display_menus_changed(void);
int32_t display_take_menu_choice(void);

static gaddr mlist(void) { return rd32(LM_MenuList); }
static int mbar_height(void) { return (int16_t)rd16(LM_MBarHeight); }

/* ---------------------------------------------------------------------- */
/* Menu data */

static gaddr menu_ptr(gaddr mh) { return rd32(mh); }
static gaddr title_of(gaddr mh) { return menu_ptr(mh) + MenuInfo_menuData_off; }

/* Address of item n's string (1-based), or 0. */
static gaddr item_addr(gaddr mh, int n)
{
    gaddr p = title_of(mh);
    p += rd8(p) + 1u;
    for (int i = 1; rd8(p); i++) {
        if (i == n)
            return p;
        p += rd8(p) + 1u + 4u;
    }
    return 0;
}

int16_t tb_CountMItems(gaddr mh)
{
    gaddr p = title_of(mh);
    p += rd8(p) + 1u;
    int n = 0;
    while (rd8(p)) {
        n++;
        p += rd8(p) + 1u + 4u;
    }
    return (int16_t)n;
}

static bool item_enabled(gaddr mh, int item)
{
    uint32_t flags = (uint32_t)MenuInfo_enableFlags(menu_ptr(mh));
    if (!(flags & 1))
        return false;
    return item > 31 || (flags & (1u << item));
}

/* Insert raw bytes into the menu at offset `at` (from the master pointer). */
static void menu_insert_bytes(gaddr mh, uint32_t at, const uint8_t *bytes, uint32_t n)
{
    int32_t size = tb_GetHandleSize(mh);
    tb_SetHandleSize(mh, size + (int32_t)n);
    gaddr p = rd32(mh);
    gmemcpy(p + at + n, p + at, (uint32_t)size - at);
    gput(p + at, bytes, n);
}

static void menu_delete_bytes(gaddr mh, uint32_t at, uint32_t n)
{
    int32_t size = tb_GetHandleSize(mh);
    gaddr p = rd32(mh);
    gmemcpy(p + at, p + at + n, (uint32_t)size - at - n);
    tb_SetHandleSize(mh, size - (int32_t)n);
}

/* Offset of the end-of-items zero byte. */
static uint32_t items_end(gaddr mh)
{
    gaddr p = title_of(mh);
    p += rd8(p) + 1u;
    while (rd8(p))
        p += rd8(p) + 1u + 4u;
    return p - rd32(mh);
}

static int16_t system_font_width(const uint8_t *s, int n);

void tb_CalcMenuSize(gaddr mh)
{
    if (!mh || !rd32(mh)) /* no menu (a failed GetMenuHandle) */
        return;
    display_menus_changed();
    int items = tb_CountMItems(mh);
    int w = 0;
    for (int i = 1; i <= items; i++) {
        gaddr it = item_addr(mh, i);
        uint8_t buf[256];
        int n = rd8(it);
        gget(buf, it + 1, (uint32_t)n);
        int iw = system_font_width(buf, n) + 30;
        if (rd8(it + 1 + n + 1))
            iw += 30; /* command key */
        if (iw > w)
            w = iw;
    }
    gaddr p = menu_ptr(mh);
    MenuInfo_set_menuWidth(p, (int16_t)(w < 40 ? 40 : w));
    MenuInfo_set_menuHeight(p, (int16_t)(items * ITEM_HEIGHT));
}

gaddr tb_NewMenu(int16_t mid, gaddr str)
{
    uint32_t tl = (uint32_t)rd8(str) + 1;
    gaddr h = tb_NewHandle((int32_t)(MenuInfo_menuData_off + tl + 1), false, true);
    gaddr p = rd32(h);
    MenuInfo_set_menuID(p, mid);
    MenuInfo_set_menuProc(p, tb_GetResource((int32_t)fourcc("MDEF"), 0));
    MenuInfo_set_enableFlags(p, -1);
    gmemcpy(p + MenuInfo_menuData_off, str, tl);
    wr8(p + MenuInfo_menuData_off + tl, 0);
    tb_CalcMenuSize(h);
    return h;
}

gaddr tb_GetMenu(int16_t rid)
{
    gaddr h = tb_GetResource((int32_t)fourcc("MENU"), rid);
    if (!h || !rd32(h))
        return 0;
    tb_HNoPurge(h);
    gaddr p = rd32(h);
    /* The resource holds the MDEF's ID where the handle will go. */
    int16_t mdef = (int16_t)(MenuInfo_menuProc(p) & 0xFFFF);
    gaddr mh = tb_GetResource((int32_t)fourcc("MDEF"), mdef);
    if (!mh)
        mh = tb_GetResource((int32_t)fourcc("MDEF"), 0);
    MenuInfo_set_menuProc(rd32(h), mh);
    tb_CalcMenuSize(h);
    return h;
}

void tb_DisposeMenu(gaddr mh)
{
    if (!mh || !rd32(mh)) /* no menu (a failed GetMenuHandle) */
        return;
    if (tb_HGetState(mh) & 0x20)
        tb_ReleaseResource(mh);
    else
        tb_DisposeHandle(mh);
}

/* AppendMenu metacharacters: ; or return separate items, ^ icon, ! mark,
 * < style, / key equivalent, ( disabled. */
static void append_items(gaddr mh, const uint8_t *s, int n, int after)
{
    if (!mh || !rd32(mh))
        return; /* a nil menu: nothing to add to (and not low memory to write over) */
    int pos = 0;
    while (pos <= n) {
        uint8_t name[256];
        int nl = 0;
        uint8_t icon = 0, key = 0, mark = 0, style = 0;
        bool disabled = false;
        while (pos < n && s[pos] != ';' && s[pos] != '\r') {
            uint8_t ch = s[pos++];
            if (ch == '^' && pos < n) {
                icon = (uint8_t)(s[pos++] - '0');
            } else if (ch == '!' && pos < n) {
                mark = s[pos++];
            } else if (ch == '<' && pos < n) {
                uint8_t st = s[pos++];
                style |= st == 'B' ? 1 : st == 'I' ? 2 : st == 'U' ? 4 : st == 'O' ? 8 : st == 'S' ? 16 : 0;
            } else if (ch == '/' && pos < n) {
                key = s[pos++];
            } else if (ch == '(') {
                disabled = true;
            } else if (nl < 255) {
                name[nl++] = ch;
            }
        }
        pos++;
        if (nl == 0 && pos > n)
            break;
        /* Insert after item `after` (or at the end). */
        int count = tb_CountMItems(mh);
        uint32_t at = after < 0 || after >= count ? items_end(mh) : (item_addr(mh, after + 1) - rd32(mh));
        uint8_t rec[260];
        rec[0] = (uint8_t)nl;
        memcpy(rec + 1, name, (size_t)nl);
        rec[1 + nl] = icon;
        rec[2 + nl] = key;
        rec[3 + nl] = mark;
        rec[4 + nl] = style;
        menu_insert_bytes(mh, at, rec, (uint32_t)nl + 5);
        int index = (after < 0 || after >= count) ? count + 1 : after + 1;
        gaddr p = menu_ptr(mh);
        uint32_t flags = (uint32_t)MenuInfo_enableFlags(p);
        if (index <= 31) {
            /* Shift the flags of later items up by one. */
            uint32_t low = flags & ((1u << index) - 1);
            uint32_t high = (flags >> index) << (index + 1);
            flags = low | high | (disabled ? 0 : (1u << index));
        }
        MenuInfo_set_enableFlags(p, (int32_t)flags);
        if (after >= 0)
            after++;
    }
    tb_CalcMenuSize(mh);
}

void tb_InsertMenuItem(gaddr mh, gaddr str, int16_t after)
{
    if (!mh || !rd32(mh)) /* no menu (a failed GetMenuHandle) */
        return;
    uint8_t buf[256];
    int n = rd8(str);
    gget(buf, str + 1, (uint32_t)n);
    append_items(mh, buf, n, after < 0 ? 0 : after);
}

void tb_DeleteMenuItem(gaddr mh, int16_t item)
{
    if (!mh || !rd32(mh)) /* no menu (a failed GetMenuHandle) */
        return;
    int count = tb_CountMItems(mh);
    if (item < 1 || item > count)
        return;
    gaddr it = item_addr(mh, item);
    menu_delete_bytes(mh, it - rd32(mh), (uint32_t)rd8(it) + 5);
    gaddr p = menu_ptr(mh);
    uint32_t flags = (uint32_t)MenuInfo_enableFlags(p);
    if (item <= 31) {
        /* Later items' flags move down; the top bit stays set. */
        uint32_t low = flags & ((1u << item) - 1);
        uint32_t high = (flags >> (item + 1)) << item;
        flags = low | high | 0x80000000u;
    }
    MenuInfo_set_enableFlags(p, (int32_t)flags);
    tb_CalcMenuSize(mh);
}

void tb_AppendMenu(gaddr mh, gaddr str)
{
    if (!mh || !rd32(mh)) /* no menu (a failed GetMenuHandle) */
        return;
    uint8_t buf[256];
    int n = rd8(str);
    gget(buf, str + 1, (uint32_t)n);
    append_items(mh, buf, n, -1);
}

/* Names of resources of a type (unique, sorted), for AppendResMenu. */
static int cmp_pstr(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }

static void res_menu(gaddr mh, int32_t restype, int after)
{
    char names[512][256];
    int count = 0;
    int32_t types[2] = { restype, 0 };
    if ((uint32_t)restype == fourcc("FONT"))
        types[1] = (int32_t)fourcc("FOND");
    uint8_t saved = rd8(LM_ResLoad);
    wr8(LM_ResLoad, 0);
    gaddr name = tb_NewPtr(256, false, true), id = tb_NewPtr(2, false, false), type = tb_NewPtr(4, false, false);
    for (int t = 0; t < 2 && types[t]; t++) {
        int n = tb_CountResources(types[t]);
        for (int i = 1; i <= n && count < 512; i++) {
            gaddr h = tb_GetIndResource(types[t], (int16_t)i);
            if (!h)
                continue;
            tb_GetResInfo(h, id, type, name);
            int nl = rd8(name);
            if (nl == 0)
                continue;
            if ((uint32_t)types[t] == fourcc("FONT") && ((int16_t)rd16(id) & 127))
                continue; /* only family entries (size 0) */
            if ((uint32_t)types[t] == fourcc("FOND")) /* the name applications know it by */
                tb_GetFontName((int16_t)rd16(id), name);
            char s[256];
            pstr_to_c(name, s, sizeof s);
            if (s[0] == '.' || s[0] == '%')
                continue;
            const char *shown = s[0] == 0 ? s + 1 : s; /* desk accessories start with a NUL */
            bool dup = false;
            for (int k = 0; k < count; k++)
                if (!strcmp(names[k], shown))
                    dup = true;
            if (!dup)
                strcpy(names[count++], shown);
        }
    }
    /* The families a System 7 Font menu listed that we draw with others. */
    static const int16_t classic[] = { 20, 21, 22, 23 };
    for (unsigned c = 0; (uint32_t)restype == fourcc("FONT") && c < sizeof classic / sizeof classic[0]; c++) {
        tb_GetFontName(classic[c], name);
        char s[256];
        pstr_to_c(name, s, sizeof s);
        bool dup = !s[0];
        for (int k = 0; k < count && !dup; k++)
            dup = !strcmp(names[k], s);
        if (!dup && count < 512)
            strcpy(names[count++], s);
    }
    /* The Apple menu (desk accessories) lists the Apple Menu Items folder
     * too, as in System 7. */
    if ((uint32_t)restype == fourcc("DRVR")) {
        static char items[64][256];
        int n = fm_apple_menu_items(items, 64);
        for (int k = 0; k < n && count < 512; k++) {
            bool dup = false;
            for (int j = 0; j < count && !dup; j++)
                dup = !strcmp(names[j], items[k]);
            if (!dup)
                strcpy(names[count++], items[k]);
        }
    }
    wr8(LM_ResLoad, saved);
    tb_DisposePtr(name);
    tb_DisposePtr(id);
    tb_DisposePtr(type);
    qsort(names, (size_t)count, sizeof names[0], cmp_pstr);
    for (int k = 0; k < count; k++) {
        uint8_t buf[256];
        int n = (int)strlen(names[k]);
        memcpy(buf, names[k], (size_t)n);
        /* Insert literally: no metacharacter parsing for resource names. */
        int c = tb_CountMItems(mh);
        int pos = after < 0 ? c : after + k;
        uint32_t at = pos >= c ? items_end(mh) : item_addr(mh, pos + 1) - rd32(mh);
        uint8_t rec[260];
        rec[0] = (uint8_t)n;
        memcpy(rec + 1, buf, (size_t)n);
        memset(rec + 1 + n, 0, 4);
        menu_insert_bytes(mh, at, rec, (uint32_t)n + 5);
        gaddr p = menu_ptr(mh);
        int index = pos + 1;
        if (index <= 31)
            MenuInfo_set_enableFlags(p, MenuInfo_enableFlags(p) | (int32_t)(1u << index));
    }
    tb_CalcMenuSize(mh);
}

void tb_AppendResMenu(gaddr mh, int32_t restype) { res_menu(mh, restype, -1); }
void tb_InsertResMenu(gaddr mh, int32_t restype, int16_t after) { res_menu(mh, restype, after); }

/* ---------------------------------------------------------------------- */
/* The menu bar list */

static int nmenus(void)
{
    gaddr l = mlist();
    return l ? (int16_t)rd16(rd32(l)) / 6 : 0;
}

static gaddr menu_at(int i) { return rd32(rd32(mlist()) + 6 + (uint32_t)i * 6); }

/* ---------------------------------------------------------------------- */
/* Menu colour table (MenuCInfo): kept for applications to read and write;
 * the native menus draw in the host's colours.  Entries are 30 bytes and
 * the table ends with an entry whose ID is -99. */

#define MC_END -99

static void mc_init(void)
{
    gaddr h = rd32(LM_MenuCInfo);
    if (!h || !rd32(h)) {
        h = tb_NewHandle(MCEntry_SIZEOF, true, true);
        wr32(LM_MenuCInfo, h);
    }
    tb_SetHandleSize(h, MCEntry_SIZEOF);
    gmemset(rd32(h), 0, MCEntry_SIZEOF);
    MCEntry_set_mctID(rd32(h), MC_END);
}

static int mc_count(void)
{
    gaddr h = rd32(LM_MenuCInfo);
    if (!h || !rd32(h))
        return 0;
    int n = 0;
    while (MCEntry_mctID(rd32(h) + (uint32_t)n * MCEntry_SIZEOF) != MC_END)
        n++;
    return n;
}

gaddr tb_GetMCEntry(int16_t mid, int16_t item)
{
    gaddr h = rd32(LM_MenuCInfo);
    int n = mc_count();
    for (int i = 0; i < n; i++) {
        gaddr e = rd32(h) + (uint32_t)i * MCEntry_SIZEOF;
        if (MCEntry_mctID(e) == mid && MCEntry_mctItem(e) == item)
            return e;
    }
    return 0;
}

void tb_SetMCEntries(int16_t n, gaddr entries)
{
    if (!rd32(LM_MenuCInfo))
        mc_init();
    for (int k = 0; k < n; k++) {
        gaddr src = entries + (uint32_t)k * MCEntry_SIZEOF;
        gaddr e = tb_GetMCEntry(MCEntry_mctID(src), MCEntry_mctItem(src));
        if (!e) {
            gaddr h = rd32(LM_MenuCInfo);
            int count = mc_count();
            tb_SetHandleSize(h, (count + 2) * MCEntry_SIZEOF);
            e = rd32(h) + (uint32_t)count * MCEntry_SIZEOF;
            gmemset(e + MCEntry_SIZEOF, 0, MCEntry_SIZEOF);
            MCEntry_set_mctID(e + MCEntry_SIZEOF, MC_END);
        }
        gmemcpy(e, src, MCEntry_SIZEOF);
    }
}

/* Item -98 (mctAllItems) removes every entry for the menu. */
void tb_DeleteMCEntries(int16_t mid, int16_t item)
{
    gaddr h = rd32(LM_MenuCInfo);
    int n = mc_count(), w = 0;
    for (int i = 0; i <= n; i++) { /* the end marker moves too */
        gaddr e = rd32(h) + (uint32_t)i * MCEntry_SIZEOF;
        bool drop = i < n && MCEntry_mctID(e) == mid && (item == -98 || MCEntry_mctItem(e) == item);
        if (!drop) {
            if (w != i)
                gmemcpy(rd32(h) + (uint32_t)w * MCEntry_SIZEOF, e, MCEntry_SIZEOF);
            w++;
        }
    }
    tb_SetHandleSize(h, w * MCEntry_SIZEOF);
}

gaddr tb_GetMCInfo(void)
{
    uint32_t h = rd32(LM_MenuCInfo);
    if (!h)
        return 0;
    tb_HandToHand(&h);
    return h;
}

void tb_SetMCInfo(gaddr table)
{
    if (!table || !rd32(table))
        return;
    uint32_t h = table;
    tb_HandToHand(&h);
    gaddr old = rd32(LM_MenuCInfo);
    wr32(LM_MenuCInfo, h);
    if (old)
        tb_DisposeHandle(old);
}

void tb_DisposeMCInfo(gaddr table)
{
    if (table && table != rd32(LM_MenuCInfo))
        tb_DisposeHandle(table);
}

void tb_InitMenus(void)
{
    gaddr l = mlist();
    if (!l) {
        l = tb_NewHandle(6 + 6, false, true);
        wr32(LM_MenuList, l);
    }
    tb_SetHandleSize(l, 12);
    gaddr p = rd32(l);
    gmemset(p, 0, 12);
    wr8(LM_MenuFlash, 3);
    wr16(LM_TheMenu, 0);
    mc_init();
    tb_DrawMenuBar();
}

void tb_InsertMenu(gaddr mh, int16_t before)
{
    if (!mh || !rd32(mh)) /* no menu (a failed GetMenuHandle) */
        return;
    display_menus_changed();
    gaddr l = mlist();
    int n = nmenus();
    int16_t id = MenuInfo_menuID(menu_ptr(mh));
    for (int i = 0; i < n; i++)
        if (menu_at(i) == mh || MenuInfo_menuID(menu_ptr(menu_at(i))) == id)
            return; /* already there */
    if (before == -1)
        return; /* hierarchical menus are tracked through the menu itself for now */
    int pos = n;
    for (int i = 0; i < n; i++)
        if (MenuInfo_menuID(menu_ptr(menu_at(i))) == before) {
            pos = i;
            break;
        }
    int32_t size = tb_GetHandleSize(l);
    tb_SetHandleSize(l, size + 6);
    gaddr p = rd32(l);
    uint32_t at = 6 + (uint32_t)pos * 6;
    gmemcpy(p + at + 6, p + at, (uint32_t)size - at);
    wr32(p + at, mh);
    wr16(p + at + 4, 0);
    wr16(p, (uint16_t)((n + 1) * 6));
}

void tb_DeleteMenu(int16_t mid)
{
    display_menus_changed();
    int n = nmenus();
    for (int i = 0; i < n; i++) {
        if (MenuInfo_menuID(menu_ptr(menu_at(i))) == mid) {
            gaddr l = mlist();
            int32_t size = tb_GetHandleSize(l);
            gaddr p = rd32(l);
            uint32_t at = 6 + (uint32_t)i * 6;
            gmemcpy(p + at, p + at + 6, (uint32_t)size - at - 6);
            tb_SetHandleSize(l, size - 6);
            wr16(rd32(l), (uint16_t)((n - 1) * 6));
            return;
        }
    }
}

void tb_ClearMenuBar(void)
{
    display_menus_changed();
    gaddr l = mlist();
    tb_SetHandleSize(l, 12);
    gmemset(rd32(l), 0, 12);
}

gaddr tb_GetMenuHandle(int16_t mid)
{
    int n = nmenus();
    for (int i = 0; i < n; i++)
        if (MenuInfo_menuID(menu_ptr(menu_at(i))) == mid)
            return menu_at(i);
    return 0;
}

gaddr tb_GetMenuBar(void)
{
    gaddr l = mlist();
    int32_t size = tb_GetHandleSize(l);
    gaddr h = tb_NewHandle(size, false, false);
    gmemcpy(rd32(h), rd32(l), (uint32_t)size);
    return h;
}

void tb_SetMenuBar(gaddr ml)
{
    display_menus_changed();
    gaddr l = mlist();
    int32_t size = tb_GetHandleSize(ml);
    tb_SetHandleSize(l, size);
    gmemcpy(rd32(l), rd32(ml), (uint32_t)size);
}

gaddr tb_GetNewMBar(int16_t mbarid)
{
    gaddr h = tb_GetResource((int32_t)fourcc("MBAR"), mbarid);
    if (!h || !rd32(h))
        return 0;
    gaddr saved = tb_GetMenuBar();
    tb_ClearMenuBar();
    int n = (int16_t)rd16(rd32(h));
    for (int i = 0; i < n; i++) {
        gaddr m = tb_GetMenu((int16_t)rd16(rd32(h) + 2 + (uint32_t)i * 2));
        if (m)
            tb_InsertMenu(m, 0);
    }
    gaddr result = tb_GetMenuBar();
    tb_SetMenuBar(saved);
    tb_DisposeHandle(saved);
    return result;
}

/* ---------------------------------------------------------------------- */
/* Drawing the menu bar */

static int16_t system_font_width(const uint8_t *s, int n)
{
    gaddr port = rd32(LM_WMgrPort);
    if (!port)
        return (int16_t)(7 * n);
    gaddr saved = qd_port();
    tb_SetPort(port);
    GrafPort_set_txFont(port, 0);
    GrafPort_set_txSize(port, 12);
    GrafPort_set_txFace(port, 0);
    int w = qd_text_width(port, s, n);
    tb_SetPort(saved);
    return (int16_t)w;
}

static int title_width(gaddr mh)
{
    gaddr t = title_of(mh);
    uint8_t buf[256];
    int n = rd8(t);
    gget(buf, t + 1, (uint32_t)n);
    return system_font_width(buf, n);
}

/* Lay out titles left to right; menuLeft for each, lastRight at the end. */
static void layout_titles(void)
{
    int x = MBAR_FIRST;
    int n = nmenus();
    gaddr p = rd32(mlist());
    for (int i = 0; i < n; i++) {
        wr16(p + 6 + (uint32_t)i * 6 + 4, (uint16_t)x);
        x += title_width(menu_at(i)) + MBAR_TITLE_GAP;
        p = rd32(mlist());
    }
    wr16(p + 2, (uint16_t)x);
}

static qrect title_rect(int i)
{
    gaddr p = rd32(mlist());
    int left = (int16_t)rd16(p + 6 + (uint32_t)i * 6 + 4);
    int right = left + title_width(menu_at(i)) + MBAR_TITLE_GAP;
    return (qrect){ 0, (int16_t)(left - 1), (int16_t)(mbar_height() - 1), (int16_t)(right - 1) };
}

static void with_wmgr_port(gaddr *saved)
{
    *saved = qd_port();
    gaddr wp = rd32(LM_WMgrPort);
    tb_SetPort(wp);
    tb_SetRectRgn(GrafPort_clipRgn(wp), -32767, -32767, 32767, 32767);
    tb_PenNormal();
    GrafPort_set_txFont(wp, 0);
    GrafPort_set_txSize(wp, 12);
    GrafPort_set_txFace(wp, 0);
    GrafPort_set_txMode(wp, 1);
}

static void draw_text_at(int x, int y, gaddr pstr)
{
    tb_MoveTo((int16_t)x, (int16_t)y);
    tb_DrawString(pstr);
}

static void dim_rect(qrect r)
{
    hrgn g;
    hrgn_init(&g);
    hrgn_rect(&g, r);
    qd_fill_region(qd_port(), &g, qd_pat_gray, patBic);
    hrgn_free(&g);
}

void tb_DrawMenuBar(void)
{
    display_menus_changed();
    if (!rd32(LM_WMgrPort) || !mlist())
        return;
    gaddr saved;
    with_wmgr_port(&saved);
    layout_titles();
    int h = mbar_height();
    hrgn bar;
    hrgn_init(&bar);
    hrgn round;
    hrgn_init(&round);
    hrgn_rect(&bar, (qrect){ 0, 0, (int16_t)(h - 1), (int16_t)g_screen_width });
    qd_rrect_region(&round, (qrect){ 0, 0, (int16_t)g_screen_height, (int16_t)g_screen_width }, 16, 16);
    hrgn_op(&bar, &bar, &round, RGN_SECT);
    qd_fill_region(qd_port(), &bar, qd_pat_white, patCopy);
    hrgn_rect(&bar, (qrect){ (int16_t)(h - 1), 0, (int16_t)h, (int16_t)g_screen_width });
    qd_fill_region(qd_port(), &bar, qd_pat_black, patCopy);
    hrgn_free(&bar);
    hrgn_free(&round);
    int n = nmenus();
    for (int i = 0; i < n; i++) {
        gaddr mh = menu_at(i);
        qrect r = title_rect(i);
        draw_text_at(r.left + 7, 14, title_of(mh));
        if (!(MenuInfo_enableFlags(menu_ptr(mh)) & 1))
            dim_rect(r);
    }
    int16_t hil = (int16_t)rd16(LM_TheMenu);
    if (hil) {
        for (int i = 0; i < n; i++)
            if (MenuInfo_menuID(menu_ptr(menu_at(i))) == hil) {
                hrgn t;
                hrgn_init(&t);
                hrgn_rect(&t, title_rect(i));
                qd_fill_region(qd_port(), &t, qd_pat_black, patXor);
                hrgn_free(&t);
            }
    }
    tb_SetPort(saved);
}

void tb_InvalMenuBar(void) { tb_DrawMenuBar(); }

static void invert_title(int16_t mid)
{
    int n = nmenus();
    for (int i = 0; i < n; i++) {
        if (MenuInfo_menuID(menu_ptr(menu_at(i))) != mid)
            continue;
        gaddr saved;
        with_wmgr_port(&saved);
        hrgn t;
        hrgn_init(&t);
        hrgn_rect(&t, title_rect(i));
        qd_fill_region(qd_port(), &t, qd_pat_black, patXor);
        hrgn_free(&t);
        tb_SetPort(saved);
    }
}

void tb_HiliteMenu(int16_t mid)
{
    int16_t cur = (int16_t)rd16(LM_TheMenu);
    if (cur == mid)
        return;
    if (cur)
        invert_title(cur);
    if (mid)
        invert_title(mid);
    wr16(LM_TheMenu, (uint16_t)mid);
}

void tb_FlashMenuBar(int16_t mid)
{
    if (mid)
        invert_title(mid);
}

void tb_SetMenuFlash(int16_t i) { wr16(LM_MenuFlash, (uint16_t)i); }

/* ---------------------------------------------------------------------- */
/* The standard MDEF */

static void mdef_draw(gaddr mh, qrect r, int hilite_item)
{
    gaddr port = qd_port();
    gaddr rr = tb_NewPtr(8, false, false);
    qrect_put(rr, r);
    tb_EraseRect(rr);
    int n = tb_CountMItems(mh);
    for (int i = 1; i <= n; i++) {
        gaddr it = item_addr(mh, i);
        int nl = rd8(it);
        int top = r.top + (i - 1) * ITEM_HEIGHT;
        qrect ir = { (int16_t)top, r.left, (int16_t)(top + ITEM_HEIGHT), r.right };
        if (nl > 0 && rd8(it + 1) == '-') { /* divider */
            hrgn line;
            hrgn_init(&line);
            hrgn_rect(&line, (qrect){ (int16_t)(top + 8), r.left, (int16_t)(top + 9), r.right });
            qd_fill_region(port, &line, qd_pat_gray, patCopy);
            hrgn_free(&line);
            continue;
        }
        uint8_t mark = rd8(it + 1 + nl + 2), key = rd8(it + 1 + nl + 1), style = rd8(it + 1 + nl + 3);
        GrafPort_set_txFace(port, style);
        draw_text_at(r.left + 15, top + 12, it);
        GrafPort_set_txFace(port, 0);
        if (mark) {
            tb_MoveTo((int16_t)(r.left + 2), (int16_t)(top + 12));
            tb_DrawChar(mark);
        }
        if (key) {
            tb_MoveTo((int16_t)(r.right - 24), (int16_t)(top + 12));
            tb_DrawChar(0x11); /* command symbol */
            tb_DrawChar(key);
        }
        if (!item_enabled(mh, i))
            dim_rect(ir);
        if (i == hilite_item) {
            hrgn h;
            hrgn_init(&h);
            hrgn_rect(&h, ir);
            qd_fill_region(port, &h, qd_pat_black, patXor);
            hrgn_free(&h);
        }
    }
    tb_DisposePtr(rr);
}

/* MDEF 0: PROCEDURE (message, theMenu, VAR menuRect, hitPt, VAR whichItem). */
void mdef0_native(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    gaddr which = cpu_pop32(c);
    uint32_t hit = cpu_pop32(c);
    gaddr rect = cpu_pop32(c);
    gaddr mh = cpu_pop32(c);
    int16_t msg = (int16_t)cpu_pop16(c);
    qrect r = qrect_get(rect);
    switch (msg) {
    case mDrawMsg:
        mdef_draw(mh, r, 0);
        break;
    case mChooseMsg: {
        int item = 0;
        int16_t h = pt_h(hit), v = pt_v(hit);
        if (h >= r.left && h < r.right && v >= r.top && v < r.bottom)
            item = (v - r.top) / ITEM_HEIGHT + 1;
        if (item) {
            gaddr it = item_addr(mh, item);
            if (!it || !item_enabled(mh, item) || (rd8(it) && rd8(it + 1) == '-'))
                item = 0;
        }
        int old = (int16_t)rd16(which);
        if (item != old)
            mdef_draw(mh, r, item);
        wr16(which, (uint16_t)item);
        break;
    }
    case mSizeMsg:
        tb_CalcMenuSize(mh);
        break;
    default:
        break;
    }
    cpu_push32(c, ret);
}

static void call_mdef(gaddr mh, int16_t msg, qrect r, uint32_t hit, gaddr which)
{
    gaddr proc = MenuInfo_menuProc(menu_ptr(mh));
    if (!proc || !rd32(proc))
        return;
    gaddr rect = tb_NewPtr(8, false, false);
    qrect_put(rect, r);
    cpu68k *c = g_cpu;
    cpu_push16(c, (uint16_t)msg);
    cpu_push32(c, mh);
    cpu_push32(c, rect);
    cpu_push32(c, hit);
    cpu_push32(c, which);
    tb_HLock(proc);
    guest_call(rd32(proc));
    tb_HUnlock(proc);
    tb_DisposePtr(rect);
}

/* ---------------------------------------------------------------------- */
/* Choosing */

static int32_t last_choice;

/* Save and restore the screen under a pulled-down menu. */
static uint32_t *save_bits(qrect r)
{
    int w = r.right - r.left, h = r.bottom - r.top;
    uint32_t *buf = malloc((size_t)w * (size_t)h * sizeof *buf);
    pixdst d = qd_port_pixels(rd32(LM_WMgrPort));
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            buf[(size_t)y * (size_t)w + (size_t)x] = get_px(&d, r.left + x, r.top + y);
    return buf;
}

static void restore_bits(qrect r, uint32_t *buf)
{
    int w = r.right - r.left, h = r.bottom - r.top;
    pixdst d = qd_port_pixels(rd32(LM_WMgrPort));
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            put_px(&d, r.left + x, r.top + y, buf[(size_t)y * (size_t)w + (size_t)x]);
    free(buf);
}

/* A choice made in the host's menu bar: valid only if still enabled now
 * that the app has had its chance to adjust its menus. */
static int32_t host_choice(void)
{
    int32_t c = display_take_menu_choice();
    if (!c)
        return -1;
    int16_t id = (int16_t)(c >> 16), item = (int16_t)(c & 0xFFFF);
    gaddr mh = tb_GetMenuHandle(id);
    if (!mh || !item_enabled(mh, item))
        return 0;
    tb_HiliteMenu(id);
    return c;
}

bool display_menu_tracking_wait(void);

int32_t tb_MenuSelect(uint32_t p)
{
    bool hosted = display_menu_tracking_wait(); /* a host menu is open: wait for it */
    int32_t chosen = host_choice();
    if (hosted && chosen < 0)
        chosen = 0; /* closed without a choice */
    if (chosen >= 0) {
        last_choice = chosen;
        return chosen;
    }
    gaddr saved;
    with_wmgr_port(&saved);
    layout_titles();
    int n = nmenus();
    int open = -1;
    qrect box = { 0, 0, 0, 0 }, saved_area = { 0, 0, 0, 0 };
    uint32_t *under = NULL;
    gaddr which = tb_NewPtr(2, false, true);
    int32_t result = 0;
    do {
        uint32_t m = ev_mouse_global();
        int16_t h = pt_h(m), v = pt_v(m);
        int title = -1;
        if (v >= 0 && v < mbar_height())
            for (int i = 0; i < n; i++) {
                qrect t = title_rect(i);
                if (h >= t.left && h < t.right)
                    title = i;
            }
        if (title >= 0 && title != open) {
            if (open >= 0) {
                restore_bits(saved_area, under);
                under = NULL;
                tb_HiliteMenu(0);
            }
            open = title;
            gaddr mh = menu_at(open);
            tb_HiliteMenu(MenuInfo_menuID(menu_ptr(mh)));
            if (MenuInfo_enableFlags(menu_ptr(mh)) & 1) {
                gaddr mp = menu_ptr(mh);
                qrect t = title_rect(open);
                box = (qrect){ (int16_t)mbar_height(), t.left, (int16_t)(mbar_height() + MenuInfo_menuHeight(mp)),
                               (int16_t)(t.left + MenuInfo_menuWidth(mp)) };
                /* MBarHook sees the menu's rectangle before it is drawn; a
                 * result other than zero abandons the choice. */
                if (rd32(LM_MBarHook)) {
                    cpu68k *c = g_cpu;
                    uint32_t sp = c->a[7];
                    gaddr r = tb_NewPtr(8, false, false);
                    qrect_put(r, box);
                    cpu_push32(c, r);
                    uint32_t d0 = hook_call(rd32(LM_MBarHook), 0, 0, sp);
                    tb_DisposePtr(r);
                    if (d0) {
                        tb_HiliteMenu(0);
                        open = -1;
                        break;
                    }
                }
                saved_area = (qrect){ (int16_t)(box.top - 1), (int16_t)(box.left - 1), (int16_t)(box.bottom + 2),
                                      (int16_t)(box.right + 2) };
                under = save_bits(saved_area);
                gaddr fr = tb_NewPtr(8, false, false);
                qrect_put(fr, (qrect){ (int16_t)(box.top - 1), (int16_t)(box.left - 1), (int16_t)(box.bottom + 1),
                                       (int16_t)(box.right + 1) });
                tb_FrameRect(fr);
                qrect_put(fr, (qrect){ (int16_t)(box.bottom + 1), box.left, (int16_t)(box.bottom + 2),
                                       (int16_t)(box.right + 2) });
                tb_PaintRect(fr);
                qrect_put(fr, (qrect){ box.top, (int16_t)(box.right + 1), (int16_t)(box.bottom + 2),
                                       (int16_t)(box.right + 2) });
                tb_PaintRect(fr);
                tb_DisposePtr(fr);
                wr16(which, 0);
                call_mdef(mh, mDrawMsg, box, 0, which);
            } else {
                box = (qrect){ 0, 0, 0, 0 };
            }
        }
        if (open >= 0 && !qrect_empty(box))
            call_mdef(menu_at(open), mChooseMsg, box, m, which);
        if (rd32(LM_MenuHook)) /* called on every pass while the menu is tracked */
            hook_call(rd32(LM_MenuHook), 0, 0, g_cpu->a[7]);
        if (!ev_still_down())
            break;
        ev_wait_tick();
    } while (1);
    if (open >= 0) {
        int item = (int16_t)rd16(which);
        gaddr mh = menu_at(open);
        if (under)
            restore_bits(saved_area, under);
        if (item) {
            result = ((int32_t)MenuInfo_menuID(menu_ptr(mh)) << 16) | item;
        } else {
            tb_HiliteMenu(0);
        }
    }
    tb_DisposePtr(which);
    tb_SetPort(saved);
    last_choice = result;
    return result;
}

int32_t tb_MenuKey(int16_t thec)
{
    uint8_t ch = (uint8_t)thec;
    if (ch >= 'a' && ch <= 'z')
        ch = (uint8_t)(ch - 32);
    int n = nmenus();
    for (int i = n - 1; i >= 0; i--) {
        gaddr mh = menu_at(i);
        if (!(MenuInfo_enableFlags(menu_ptr(mh)) & 1))
            continue;
        int items = tb_CountMItems(mh);
        for (int k = 1; k <= items; k++) {
            gaddr it = item_addr(mh, k);
            uint8_t key = rd8(it + 1 + rd8(it) + 1);
            if (key >= 'a' && key <= 'z')
                key = (uint8_t)(key - 32);
            if (key && key == ch && item_enabled(mh, k)) {
                int16_t id = MenuInfo_menuID(menu_ptr(mh));
                tb_HiliteMenu(id);
                last_choice = ((int32_t)id << 16) | k;
                return last_choice;
            }
        }
    }
    return 0;
}

int32_t tb_MenuChoice(void) { return last_choice; }

uint32_t ev_press_count(void);
uint32_t ev_key_count(void);

int32_t tb_PopUpMenuSelect(gaddr mh, int16_t top, int16_t left, int16_t item)
{
    gaddr saved;
    with_wmgr_port(&saved);
    gaddr mp = menu_ptr(mh);
    int16_t t = (int16_t)(top - (item > 0 ? (item - 1) * ITEM_HEIGHT : 0));
    qrect box = { t, left, (int16_t)(t + MenuInfo_menuHeight(mp)), (int16_t)(left + MenuInfo_menuWidth(mp)) };
    qrect area = { (int16_t)(box.top - 1), (int16_t)(box.left - 1), (int16_t)(box.bottom + 2), (int16_t)(box.right + 2) };
    uint32_t *under = save_bits(area);
    display_set_transient(true, area.top, area.left, area.bottom, area.right);
    gaddr fr = tb_NewPtr(8, false, false);
    qrect_put(fr, (qrect){ (int16_t)(box.top - 1), (int16_t)(box.left - 1), (int16_t)(box.bottom + 1), (int16_t)(box.right + 1) });
    tb_FrameRect(fr);
    tb_DisposePtr(fr);
    gaddr which = tb_NewPtr(2, false, true);
    call_mdef(mh, mDrawMsg, box, 0, which);
    /* At least once: a quick click chooses the item under the mouse. */
    uint32_t start = rd32(LM_Ticks);
    do {
        call_mdef(mh, mChooseMsg, box, ev_mouse_global(), which);
        if (!ev_still_down())
            break;
        ev_wait_tick();
    } while (true);
    /* With a person at the mouse, a click that let go at once leaves the
     * menu open, as Mac OS 8's did: the next click chooses (or, outside the
     * menu, cancels). */
    if (events_host_attached() && rd32(LM_Ticks) - start < 30) {
        /* Presses are counted, as a quick click can fall between two looks
         * at the button; a key cancels. */
        uint32_t p0 = ev_press_count(), k0 = ev_key_count();
        bool cancelled = false;
        while (ev_press_count() == p0) {
            if (ev_key_count() != k0) {
                cancelled = true;
                break;
            }
            call_mdef(mh, mChooseMsg, box, ev_mouse_global(), which);
            ev_wait_tick();
        }
        while (!cancelled && ev_still_down()) {
            call_mdef(mh, mChooseMsg, box, ev_mouse_global(), which);
            ev_wait_tick();
        }
        if (cancelled)
            wr16(which, 0);
        else
            call_mdef(mh, mChooseMsg, box, ev_mouse_global(), which);
    }
    int chosen = (int16_t)rd16(which);
    restore_bits(area, under);
    display_set_transient(false, 0, 0, 0, 0);
    tb_DisposePtr(which);
    tb_SetPort(saved);
    return chosen ? ((int32_t)MenuInfo_menuID(mp) << 16) | chosen : 0;
}

void tb_InitProcMenu(int16_t mbid) { tb_InitMenus(); }

/* ---------------------------------------------------------------------- */
/* Items */

void tb_EnableItem(gaddr mh, int16_t item)
{
    if (!mh || !rd32(mh)) /* no menu (a failed GetMenuHandle) */
        return;
    display_menus_changed();
    gaddr p = menu_ptr(mh);
    if (item <= 31)
        MenuInfo_set_enableFlags(p, MenuInfo_enableFlags(p) | (int32_t)(1u << item));
}

void tb_DisableItem(gaddr mh, int16_t item)
{
    if (!mh || !rd32(mh)) /* no menu (a failed GetMenuHandle) */
        return;
    display_menus_changed();
    gaddr p = menu_ptr(mh);
    if (item <= 31)
        MenuInfo_set_enableFlags(p, (int32_t)((uint32_t)MenuInfo_enableFlags(p) & ~(1u << item)));
}

static gaddr item_field(gaddr mh, int item, int field)
{
    gaddr it = item_addr(mh, item);
    return it ? it + 1 + rd8(it) + (uint32_t)field : 0;
}

void tb_CheckItem(gaddr mh, int16_t item, int8_t cflag) { tb_SetItemMark(mh, item, cflag ? 0x12 : 0); }

void tb_SetItemMark(gaddr mh, int16_t item, int16_t mark)
{
    if (!mh || !rd32(mh)) /* no menu (a failed GetMenuHandle) */
        return;
    display_menus_changed();
    gaddr f = item_field(mh, item, 2);
    if (f)
        wr8(f, (uint8_t)mark);
}

void tb_GetItemMark(gaddr mh, int16_t item, gaddr markp)
{
    if (!mh || !rd32(mh)) /* no menu (a failed GetMenuHandle) */
        return;
    gaddr f = item_field(mh, item, 2);
    wr16(markp, f ? rd8(f) : 0);
}

void tb_SetItemIcon(gaddr mh, int16_t item, uint8_t icon)
{
    if (!mh || !rd32(mh)) /* no menu (a failed GetMenuHandle) */
        return;
    gaddr f = item_field(mh, item, 0);
    if (f)
        wr8(f, icon);
}

void tb_GetItemIcon(gaddr mh, int16_t item, gaddr iconp)
{
    if (!mh || !rd32(mh)) /* no menu (a failed GetMenuHandle) */
        return;
    gaddr f = item_field(mh, item, 0);
    wr16(iconp, f ? rd8(f) : 0);
}

void tb_SetItemStyle(gaddr mh, int16_t item, int16_t style)
{
    if (!mh || !rd32(mh)) /* no menu (a failed GetMenuHandle) */
        return;
    display_menus_changed();
    gaddr f = item_field(mh, item, 3);
    if (f)
        wr8(f, (uint8_t)style);
}

void tb_GetItemStyle(gaddr mh, int16_t item, gaddr stylep)
{
    if (!mh || !rd32(mh)) /* no menu (a failed GetMenuHandle) */
        return;
    /* A word with the style in the low byte, as the ROM wrote it (MPW's
     * glue for C callers keeps only that byte). */
    gaddr f = item_field(mh, item, 3);
    wr16(stylep, f ? rd8(f) : 0);
}

void tb_SetItemCmd(gaddr mh, int16_t item, int16_t cmd)
{
    if (!mh || !rd32(mh)) /* no menu (a failed GetMenuHandle) */
        return;
    display_menus_changed();
    gaddr f = item_field(mh, item, 1);
    if (f)
        wr8(f, (uint8_t)cmd);
}

void tb_GetItemCmd(gaddr mh, int16_t item, gaddr cmdp)
{
    if (!mh || !rd32(mh)) /* no menu (a failed GetMenuHandle) */
        return;
    gaddr f = item_field(mh, item, 1);
    wr16(cmdp, f ? rd8(f) : 0);
}

void tb_GetMenuItemText(gaddr mh, int16_t item, gaddr str)
{
    if (!mh || !rd32(mh)) /* no menu (a failed GetMenuHandle) */
        return;
    gaddr it = item_addr(mh, item);
    if (it)
        gmemcpy(str, it, (uint32_t)rd8(it) + 1);
    else
        wr8(str, 0);
}

void tb_SetMenuItemText(gaddr mh, int16_t item, gaddr str)
{
    if (!mh || !rd32(mh)) /* no menu (a failed GetMenuHandle) */
        return;
    display_menus_changed();
    gaddr it = item_addr(mh, item);
    if (!it)
        return;
    uint32_t at = it - rd32(mh);
    uint32_t oldlen = rd8(it), newlen = rd8(str);
    uint8_t buf[256];
    gget(buf, str, newlen + 1);
    if (newlen > oldlen)
        menu_insert_bytes(mh, at + 1, buf + 1, newlen - oldlen);
    else if (newlen < oldlen)
        menu_delete_bytes(mh, at + 1, oldlen - newlen);
    gput(rd32(mh) + at, buf, newlen + 1);
    tb_CalcMenuSize(mh);
}

/* ---------------------------------------------------------------------- */
/* The Contextual Menu Manager (Mac OS 8): the program's menu as a pop-up
 * where it asks, Control-click the gesture.  No plug-ins add items, and
 * no help item is added. */

enum { kCMNothingSelected = 0, kCMMenuItemSelected = 1 };
enum { mouseDownEvt = 1, controlKeyMask = 0x1000, userCanceled = -128 };

static bool cm_client;

int16_t tb_InitContextualMenus(void)
{
    cm_client = true;
    return noErr;
}

int8_t tb_IsShowContextualMenuClick(gaddr ev)
{
    return ev && rd16(ev + EventRecord_what_off) == mouseDownEvt &&
           (rd16(ev + EventRecord_modifiers_off) & controlKeyMask) != 0;
}

/* ContextualMenuSelect(menu, where (global), reserved, helpType, helpItem,
 * selection, &selectionType, &menuID, &item): userCanceledErr when nothing
 * is chosen. */
int32_t tb_ContextualMenuSelect(gaddr inMenu, uint32_t inGlobalLocation, int8_t inReserved, uint32_t inHelpType,
                                gaddr inHelpItemString, gaddr inSelection, gaddr outUserSelectionType, gaddr outMenuID,
                                gaddr outMenuItem)
{
    if (!inMenu || !rd32(inMenu))
        return paramErr;
    tb_CalcMenuSize(inMenu);
    int32_t r = tb_PopUpMenuSelect(inMenu, pt_v(inGlobalLocation), pt_h(inGlobalLocation), 0);
    uint16_t item = (uint16_t)(r & 0xFFFF);
    if (outUserSelectionType)
        wr32(outUserSelectionType, item ? kCMMenuItemSelected : kCMNothingSelected);
    if (outMenuID)
        wr16(outMenuID, item ? (uint16_t)(r >> 16) : 0);
    if (outMenuItem)
        wr16(outMenuItem, item);
    return item ? noErr : userCanceled;
}

int8_t tb_ProcessIsContextualMenuClient(gaddr inPSN) { return cm_client; }
