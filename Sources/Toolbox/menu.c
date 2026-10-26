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

#include "carbon_hi.h"
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
gaddr hm_help_menu(void); /* help.c */
static int mbar_height(void) { return (int16_t)rd16(LM_MBarHeight); }

#define MAX_HIER 64
static gaddr hier[MAX_HIER]; /* InsertMenu(menu, -1)'s */
static int nhier;

/* Mac OS 8's item attributes (SetMenuItemCommandID and the rest, 'xmnu'),
 * and menus' fonts, by menu: kept beside the MenuInfo, which has no room
 * for them, and moved along when items come and go. */
typedef struct {
    uint32_t command, refcon, refcon2, encoding;
    gaddr icon;
    int16_t hier_id, font, glyph;
    uint8_t modifiers, icon_type;
    bool icon_disabled;
    hi_item hi; /* CarbonLib's (carbon_menu.c) */
} item_extra;

typedef struct {
    gaddr mh;
    int n;
    item_extra *items;
    int16_t font;
    uint16_t size;
    bool exclude_mark;
    hi_menu hi; /* CarbonLib's (carbon_menu.c) */
} menu_extra;

#define MAX_MENU_EXTRAS 128
static menu_extra mextras[MAX_MENU_EXTRAS];

static menu_extra *menu_extras(gaddr mh, bool create)
{
    for (int i = 0; i < MAX_MENU_EXTRAS; i++)
        if (mextras[i].mh == mh)
            return &mextras[i];
    if (!create)
        return NULL;
    for (int i = 0; i < MAX_MENU_EXTRAS; i++)
        if (!mextras[i].mh) {
            memset(&mextras[i], 0, sizeof mextras[i]);
            mextras[i].mh = mh;
            return &mextras[i];
        }
    return NULL;
}

static item_extra *item_extras(gaddr mh, int item, bool create)
{
    menu_extra *m = menu_extras(mh, create);
    if (!m || item < 1)
        return NULL;
    if (item > m->n) {
        if (!create)
            return NULL;
        m->items = realloc(m->items, (size_t)item * sizeof *m->items);
        memset(m->items + m->n, 0, (size_t)(item - m->n) * sizeof *m->items);
        m->n = item;
    }
    return &m->items[item - 1];
}

/* An item came in at index (later ones move up), or went. */
static void extras_inserted(gaddr mh, int index)
{
    menu_extra *m = menu_extras(mh, false);
    if (!m || index > m->n)
        return;
    m->items = realloc(m->items, (size_t)(m->n + 1) * sizeof *m->items);
    memmove(m->items + index, m->items + index - 1, (size_t)(m->n - index + 1) * sizeof *m->items);
    memset(&m->items[index - 1], 0, sizeof *m->items);
    m->n++;
}

static void extras_deleted(gaddr mh, int index)
{
    menu_extra *m = menu_extras(mh, false);
    if (!m || index > m->n)
        return;
    hi_item_free(&m->items[index - 1].hi);
    memmove(m->items + index - 1, m->items + index, (size_t)(m->n - index) * sizeof *m->items);
    m->n--;
}

static void extras_forget(gaddr mh)
{
    menu_extra *m = menu_extras(mh, false);
    if (m) {
        for (int i = 0; i < m->n; i++)
            hi_item_free(&m->items[i].hi);
        hi_menu_free(&m->hi);
        free(m->items);
        memset(m, 0, sizeof *m);
    }
}

void menu_reset(void)
{
    for (int i = 0; i < MAX_MENU_EXTRAS; i++) {
        for (int k = 0; k < mextras[i].n; k++)
            hi_item_free(&mextras[i].items[k].hi);
        hi_menu_free(&mextras[i].hi);
        free(mextras[i].items);
    }
    memset(mextras, 0, sizeof mextras);
    nhier = 0;
}

/* CarbonLib's fields of a menu and of an item (NULL when there's no
 * such menu or item, or none yet and create is false). */
hi_menu *menu_hi_menu(gaddr mh, bool create)
{
    menu_extra *m = mh && rd32(mh) ? menu_extras(mh, create) : NULL;
    return m ? &m->hi : NULL;
}

hi_item *menu_hi_item(gaddr mh, int item, bool create)
{
    if (!mh || !rd32(mh) || item < 1 || item > tb_CountMItems(mh))
        return NULL;
    item_extra *e = item_extras(mh, item, create);
    return e ? &e->hi : NULL;
}

/* An item's Mac OS 8 modifiers (kMenuShiftModifier ...), for the host's menus. */
uint8_t menu_item_modifiers(gaddr mh, int item)
{
    item_extra *e = item_extras(mh, item, false);
    return e ? e->modifiers : 0;
}

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
static bool menu_insert_bytes(gaddr mh, uint32_t at, const uint8_t *bytes, uint32_t n)
{
    int32_t size = tb_GetHandleSize(mh);
    if (size < 0 || at > (uint32_t)size)
        return false;
    /* A locked menu may not grow (a block after it): it stays as it is,
     * rather than the item spilling over the next block (Commotion's
     * Window menu, given a window's title, broke the heap). */
    tb_SetHandleSize(mh, size + (int32_t)n);
    if (tb_GetHandleSize(mh) < size + (int32_t)n)
        return false;
    gaddr p = rd32(mh);
    gmemcpy(p + at + n, p + at, (uint32_t)size - at);
    gput(p + at, bytes, n);
    return true;
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
void menu_apply_xmnu(gaddr mh, int16_t id);

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
    /* (a definition CarbonLib's RegisterMenuDefinition registered first) */
    gaddr mh = carbonmenu_registered_def(mdef);
    if (!mh)
        mh = tb_GetResource((int32_t)fourcc("MDEF"), mdef);
    if (!mh)
        mh = tb_GetResource((int32_t)fourcc("MDEF"), 0);
    MenuInfo_set_menuProc(rd32(h), mh);
    menu_apply_xmnu(h, rid);
    tb_CalcMenuSize(h);
    return h;
}

void tb_DisposeMenu(gaddr mh)
{
    if (!mh || !rd32(mh)) /* no menu (a failed GetMenuHandle) */
        return;
    for (int i = 0; i < nhier; i++)
        if (hier[i] == mh)
            hier[i--] = hier[--nhier];
    extras_forget(mh);
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
        if (!menu_insert_bytes(mh, at, rec, (uint32_t)nl + 5))
            break;
        int index = (after < 0 || after >= count) ? count + 1 : after + 1;
        extras_inserted(mh, index);
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
    extras_deleted(mh, item);
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
        if (!menu_insert_bytes(mh, at, rec, (uint32_t)n + 5))
            break;
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

/* The list, made if the program never called InitMenus (Test Drive II,
 * PICTshow): the empty list each application had at launch, where these
 * used to write through a nil handle into low memory and the host. */
static gaddr mlist_made(void)
{
    gaddr l = mlist();
    if (!l) {
        l = tb_NewHandle(6 + 6, true, true);
        if (l)
            wr32(LM_MenuList, l);
    }
    return l;
}

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
    nhier = 0;
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
    gaddr l = mlist_made();
    if (!l)
        return;
    int n = nmenus();
    int16_t id = MenuInfo_menuID(menu_ptr(mh));
    for (int i = 0; i < n; i++)
        if (menu_at(i) == mh || MenuInfo_menuID(menu_ptr(menu_at(i))) == id)
            return; /* already there */
    if (before == -1) {
        /* Submenus and pop-up menus: in a list of their own, which
         * GetMenuHandle looks in too (pop-up controls find theirs there). */
        for (int i = 0; i < nhier; i++)
            if (hier[i] == mh || (rd32(hier[i]) && MenuInfo_menuID(menu_ptr(hier[i])) == id))
                return;
        if (nhier < MAX_HIER)
            hier[nhier++] = mh;
        return;
    }
    int pos = n;
    for (int i = 0; i < n; i++)
        if (MenuInfo_menuID(menu_ptr(menu_at(i))) == before) {
            pos = i;
            break;
        }
    int32_t size = tb_GetHandleSize(l);
    tb_SetHandleSize(l, size + 6);
    if (tb_GetHandleSize(l) != size + 6)
        return; /* no room: the list stays as it was */
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
    for (int i = 0; i < nhier; i++)
        if (!rd32(hier[i]) || MenuInfo_menuID(menu_ptr(hier[i])) == mid)
            hier[i--] = hier[--nhier];
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
    nhier = 0;
    gaddr l = mlist_made();
    if (!l)
        return;
    tb_SetHandleSize(l, 12);
    gmemset(rd32(l), 0, 12);
}

gaddr tb_GetMenuHandle(int16_t mid)
{
    int n = nmenus();
    for (int i = 0; i < n; i++)
        if (MenuInfo_menuID(menu_ptr(menu_at(i))) == mid)
            return menu_at(i);
    for (int i = 0; i < nhier; i++)
        if (rd32(hier[i]) && MenuInfo_menuID(menu_ptr(hier[i])) == mid)
            return hier[i];
    /* The Help menu is in every program's menu bar on System 7, so its
     * ID finds it (Creative Minesweeper keeps an object for it). */
    if (mid == -16490) /* kHMHelpMenuID */
        return hm_help_menu();
    return 0;
}

gaddr tb_GetMenuBar(void)
{
    gaddr l = mlist_made();
    if (!l)
        return 0;
    int32_t size = tb_GetHandleSize(l);
    gaddr h = tb_NewHandle(size, false, false);
    if (!h)
        return 0;
    gmemcpy(rd32(h), rd32(l), (uint32_t)size);
    return h;
}

void tb_SetMenuBar(gaddr ml)
{
    display_menus_changed();
    gaddr l = mlist_made();
    if (!l || !ml || !rd32(ml))
        return;
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
    /* SetMenuFont's font, while the menu is drawn. */
    menu_extra *mx = menu_extras(mh, false);
    int16_t old_font = GrafPort_txFont(port), old_size = GrafPort_txSize(port);
    if (mx && (mx->font || mx->size)) {
        GrafPort_set_txFont(port, mx->font);
        if (mx->size)
            GrafPort_set_txSize(port, (int16_t)mx->size);
    }
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
    GrafPort_set_txFont(port, old_font);
    GrafPort_set_txSize(port, old_size);
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
    guest_call_defproc(rd32(proc));
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
    if (newlen > oldlen && !menu_insert_bytes(mh, at + 1, buf + 1, newlen - oldlen))
        return; /* a locked menu that can't grow keeps the old text */
    if (newlen < oldlen)
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

/* Mac OS 8: command IDs, modifiers, glyphs, icons and fonts of items */

enum { menuItemNotFoundErr = -5622, menuNotFoundErr = -5620 };
enum { kMenuShiftModifier = 1, kMenuOptionModifier = 2, kMenuControlModifier = 4, kMenuNoCommandModifier = 8 };

static bool item_ok(gaddr mh, int16_t item) { return mh && rd32(mh) && item >= 1 && item <= tb_CountMItems(mh); }

/* Set a field of an item's: the menus' snapshot for the host changes with it. */
static int16_t set_item(gaddr mh, int16_t item, item_extra **e)
{
    if (!item_ok(mh, item))
        return menuItemNotFoundErr;
    *e = item_extras(mh, item, true);
    if (!*e)
        return memFullErr;
    display_menus_changed();
    return noErr;
}

int16_t tb_SetMenuItemCommandID(gaddr mh, int16_t item, uint32_t v)
{
    item_extra *e;
    int16_t err = set_item(mh, item, &e);
    if (!err)
        e->command = v;
    return err;
}

int16_t tb_SetMenuItemModifiers(gaddr mh, int16_t item, uint8_t v)
{
    item_extra *e;
    int16_t err = set_item(mh, item, &e);
    if (!err)
        e->modifiers = v;
    return err;
}

int16_t tb_SetMenuItemTextEncoding(gaddr mh, int16_t item, uint32_t v)
{
    item_extra *e;
    int16_t err = set_item(mh, item, &e);
    if (!err)
        e->encoding = v;
    return err;
}

int16_t tb_SetMenuItemFontID(gaddr mh, int16_t item, int16_t v)
{
    item_extra *e;
    int16_t err = set_item(mh, item, &e);
    if (!err)
        e->font = v;
    return err;
}

int16_t tb_SetMenuItemRefCon(gaddr mh, int16_t item, uint32_t v)
{
    item_extra *e;
    int16_t err = set_item(mh, item, &e);
    if (!err)
        e->refcon = v;
    return err;
}

int16_t tb_SetMenuItemRefCon2(gaddr mh, int16_t item, uint32_t v)
{
    item_extra *e;
    int16_t err = set_item(mh, item, &e);
    if (!err)
        e->refcon2 = v;
    return err;
}

int16_t tb_SetMenuItemKeyGlyph(gaddr mh, int16_t item, int16_t v)
{
    item_extra *e;
    int16_t err = set_item(mh, item, &e);
    if (!err)
        e->glyph = v;
    return err;
}

static int16_t get_item(gaddr mh, int16_t item, item_extra *out)
{
    if (!item_ok(mh, item))
        return menuItemNotFoundErr;
    item_extra *e = item_extras(mh, item, false);
    if (e)
        *out = *e;
    else
        memset(out, 0, sizeof *out);
    return noErr;
}

int16_t tb_GetMenuItemCommandID(gaddr mh, int16_t item, gaddr out)
{
    item_extra e;
    int16_t err = get_item(mh, item, &e);
    if (!err && out)
        wr32(out, e.command);
    return err;
}

int16_t tb_GetMenuItemModifiers(gaddr mh, int16_t item, gaddr out)
{
    item_extra e;
    int16_t err = get_item(mh, item, &e);
    if (!err && out)
        wr8(out, e.modifiers);
    return err;
}

int16_t tb_GetMenuItemTextEncoding(gaddr mh, int16_t item, gaddr out)
{
    item_extra e;
    int16_t err = get_item(mh, item, &e);
    if (!err && out)
        wr32(out, e.encoding);
    return err;
}

int16_t tb_GetMenuItemFontID(gaddr mh, int16_t item, gaddr out)
{
    item_extra e;
    int16_t err = get_item(mh, item, &e);
    if (!err && out)
        wr16(out, (uint16_t)e.font);
    return err;
}

int16_t tb_GetMenuItemRefCon(gaddr mh, int16_t item, gaddr out)
{
    item_extra e;
    int16_t err = get_item(mh, item, &e);
    if (!err && out)
        wr32(out, e.refcon);
    return err;
}

int16_t tb_GetMenuItemRefCon2(gaddr mh, int16_t item, gaddr out)
{
    item_extra e;
    int16_t err = get_item(mh, item, &e);
    if (!err && out)
        wr32(out, e.refcon2);
    return err;
}

int16_t tb_GetMenuItemKeyGlyph(gaddr mh, int16_t item, gaddr out)
{
    item_extra e;
    int16_t err = get_item(mh, item, &e);
    if (!err && out)
        wr16(out, (uint16_t)e.glyph);
    return err;
}

/* A submenu: the key equivalent $1B and the menu's ID as the mark, as
 * System 7 had them. */
int16_t tb_SetMenuItemHierarchicalID(gaddr mh, int16_t item, int16_t id)
{
    if (!item_ok(mh, item))
        return menuItemNotFoundErr;
    tb_SetItemCmd(mh, item, id ? 0x1B : 0);
    tb_SetItemMark(mh, item, id);
    return noErr;
}

int16_t tb_GetMenuItemHierarchicalID(gaddr mh, int16_t item, gaddr out)
{
    if (!item_ok(mh, item))
        return menuItemNotFoundErr;
    gaddr it = item_addr(mh, item);
    int nl = rd8(it);
    bool sub = rd8(it + 1 + nl + 1) == 0x1B;
    wr16(out, sub ? rd8(it + 1 + nl + 2) : 0);
    return noErr;
}

/* Icons: an icon handle of a kind (kMenuIconType 1, kMenuShrinkIconType 2,
 * kMenuSmallIconType 3, kMenuColorIconType 4, kMenuIconSuiteType 5),
 * shown in the item as its icon number is in System 7's. */
int16_t tb_SetMenuItemIconHandle(gaddr mh, int16_t item, uint8_t type, gaddr icon)
{
    if (!item_ok(mh, item))
        return menuItemNotFoundErr;
    item_extra *e = item_extras(mh, item, true);
    if (!e)
        return memFullErr;
    e->icon_type = type;
    e->icon = icon;
    display_menus_changed();
    return noErr;
}

int16_t tb_GetMenuItemIconHandle(gaddr mh, int16_t item, gaddr type, gaddr icon)
{
    item_extra e;
    int16_t err = get_item(mh, item, &e);
    if (err)
        return err;
    if (type)
        wr8(type, e.icon_type);
    if (icon)
        wr32(icon, e.icon);
    return noErr;
}

/* The item and its menu enabled (item 0: the menu itself). */
int8_t tb_IsMenuItemEnabled(gaddr mh, uint16_t item)
{
    return mh && rd32(mh) && item_enabled(mh, item);
}

int8_t tb_IsMenuItemIconEnabled(gaddr mh, uint16_t item)
{
    item_extra e;
    return get_item(mh, (int16_t)item, &e) == noErr && !e.icon_disabled;
}

void tb_EnableMenuItemIcon(gaddr mh, uint16_t item)
{
    item_extra *e = item_ok(mh, (int16_t)item) ? item_extras(mh, item, true) : NULL;
    if (e)
        e->icon_disabled = false;
}

void tb_DisableMenuItemIcon(gaddr mh, uint16_t item)
{
    item_extra *e = item_ok(mh, (int16_t)item) ? item_extras(mh, item, true) : NULL;
    if (e)
        e->icon_disabled = true;
}

int32_t tb_GetMenuFont(gaddr mh, gaddr font, gaddr size)
{
    if (!mh || !rd32(mh))
        return menuNotFoundErr;
    menu_extra *m = menu_extras(mh, false);
    if (font)
        wr16(font, m ? (uint16_t)m->font : 0);
    if (size)
        wr16(size, m ? m->size : 0);
    return noErr;
}

int32_t tb_SetMenuFont(gaddr mh, int16_t font, uint16_t size)
{
    if (!mh || !rd32(mh))
        return menuNotFoundErr;
    menu_extra *m = menu_extras(mh, true);
    if (!m)
        return memFullErr;
    m->font = font;
    m->size = size;
    tb_CalcMenuSize(mh);
    return noErr;
}

int8_t tb_GetMenuExcludesMarkColumn(gaddr mh)
{
    menu_extra *m = mh ? menu_extras(mh, false) : NULL;
    return m && m->exclude_mark;
}

int32_t tb_SetMenuExcludesMarkColumn(gaddr mh, int8_t exclude)
{
    if (!mh || !rd32(mh))
        return menuNotFoundErr;
    menu_extra *m = menu_extras(mh, true);
    if (m)
        m->exclude_mark = exclude != 0;
    return noErr;
}

/* The unshifted character of a key on a US keyboard, by its ADB key code:
 * what an item's key equivalent is matched with when Option changes the
 * character (Option-S is ß). */
static uint8_t key_base_char(int code)
{
    static const char keys[49] = "asdfhgzxcv\0bqweryt123465=97-80]ou[ip\0lj'k;\\,/nm.";
    return code >= 0 && code < 48 ? (uint8_t)keys[code] : 0;
}

static uint8_t upper(uint8_t c) { return c >= 'a' && c <= 'z' ? (uint8_t)(c - 32) : c; }

/* MenuEvent: a key-down event's menu item, with the modifiers its item
 * asks for (Shift, Option, Control, and no Command at all); failing an
 * exact match, MenuKey's choice among items that ask for none (which
 * takes Command-Shift-S for Command-S). */
uint32_t tb_MenuEvent(gaddr evt)
{
    int what = (int16_t)rd16(evt);
    if (what != 3 && what != 5) /* keyDown, autoKey */
        return 0;
    uint32_t msg = rd32(evt + EventRecord_message_off);
    uint16_t mods = rd16(evt + EventRecord_modifiers_off);
    uint8_t ch = upper((uint8_t)msg), base = upper(key_base_char((int)(msg >> 8 & 0xFF)));
    bool cmd = mods & 0x0100;
    uint8_t have = (uint8_t)((mods & 0x0200 ? kMenuShiftModifier : 0) | (mods & 0x0800 ? kMenuOptionModifier : 0) |
                             (mods & 0x1000 ? kMenuControlModifier : 0) | (cmd ? 0 : kMenuNoCommandModifier));
    int n = nmenus();
    for (int pass = 0; pass < 2; pass++)
        for (int i = n - 1; i >= 0; i--) {
            gaddr mh = menu_at(i);
            if (!(MenuInfo_enableFlags(menu_ptr(mh)) & 1))
                continue;
            int items = tb_CountMItems(mh);
            for (int k = 1; k <= items; k++) {
                gaddr it = item_addr(mh, k);
                uint8_t key = upper(rd8(it + 1 + rd8(it) + 1));
                if (!key || (key >= 0x1B && key <= 0x1F) || (key != ch && key != base))
                    continue;
                item_extra *e = item_extras(mh, k, false);
                uint8_t want = e ? e->modifiers : 0;
                bool match = pass == 0 ? want == have : (!want && cmd && key == ch);
                if (!match || !item_enabled(mh, k))
                    continue;
                int16_t id = MenuInfo_menuID(menu_ptr(mh));
                tb_HiliteMenu(id);
                last_choice = ((int32_t)id << 16) | k;
                return (uint32_t)last_choice;
            }
        }
    return 0;
}

/* 'xmnu', by the menu's ID: for each item, nothing (0), or 1 and its
 * command ID, modifiers, a byte and a long unused, text encoding, refCon,
 * refCon2, submenu ID, font ID and key glyph. */
void menu_apply_xmnu(gaddr mh, int16_t id)
{
    gaddr h = tb_GetResource((int32_t)fourcc("xmnu"), id);
    if (!h || !rd32(h))
        return;
    uint32_t len = (uint32_t)tb_GetHandleSize(h);
    gaddr p = rd32(h);
    if (len < 4 || rd16(p) != 0)
        return;
    int n = (int16_t)rd16(p + 2), items = tb_CountMItems(mh);
    uint32_t o = 4;
    for (int i = 1; i <= n && o + 2 <= len; i++) {
        int type = (int16_t)rd16(p + o);
        o += 2;
        if (type != 1)
            continue;
        if (o + 28 > len)
            break;
        if (i <= items) {
            item_extra *e = item_extras(mh, i, true);
            if (e) {
                e->command = rd32(p + o);
                e->modifiers = rd8(p + o + 4);
                e->encoding = rd32(p + o + 10);
                e->refcon = rd32(p + o + 14);
                e->refcon2 = rd32(p + o + 18);
                e->font = (int16_t)rd16(p + o + 24);
                e->glyph = (int16_t)rd16(p + o + 26);
            }
            int16_t sub = (int16_t)rd16(p + o + 22);
            if (sub > 0)
                tb_SetMenuItemHierarchicalID(mh, (int16_t)i, sub);
            p = rd32(h);
        }
        o += 28;
    }
}

/* ---------------------------------------------------------------------- */
/* The menu bar as the menu walker (walker.c) sees it */

int menu_bar_count(void) { return mlist() && rd32(mlist()) ? nmenus() : 0; }

int16_t menu_bar_id(int i) { return MenuInfo_menuID(menu_ptr(menu_at(i))); }

/* Item n of the menu bar's menu i (1-based items): its text, whether it
 * can be chosen (enabled, in an enabled menu, not a separator) and whether
 * it opens a submenu.  False past the last item. */
bool menu_bar_item(int i, int n, char *text, size_t size, bool *enabled, bool *submenu)
{
    gaddr mh = menu_at(i);
    gaddr it = item_addr(mh, n);
    if (!it)
        return false;
    int len = rd8(it);
    int k = 0;
    for (; k < len && k + 1 < (int)size; k++)
        text[k] = (char)rd8(it + 1 + (gaddr)k);
    text[k] = 0;
    *submenu = rd8(it + 1 + (gaddr)len + 1) == 0x1B;
    *enabled = item_enabled(mh, n) && !(len && rd8(it + 1) == '-');
    return true;
}

/* A menu's title replaced (CarbonLib's SetMenuTitle). */
bool menu_set_title(gaddr mh, gaddr title)
{
    if (!mh || !rd32(mh) || !title)
        return false;
    uint8_t buf[256];
    uint32_t n = rd8(title) + 1u;
    gget(buf, title, n);
    uint32_t old = rd8(title_of(mh)) + 1u;
    if (!menu_insert_bytes(mh, MenuInfo_menuData_off, buf, n))
        return false;
    menu_delete_bytes(mh, MenuInfo_menuData_off + n, old);
    display_menus_changed();
    return true;
}
