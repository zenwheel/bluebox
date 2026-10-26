/*
 * Dialog Manager: dialogs and their item lists.
 *
 * A dialog is a window (windowKind dialogKind) with an item list in the DITL
 * format.  Each item's handle field holds its control, its text handle
 * (static and edit text), its icon or picture, or a user item's procedure.
 * The active edit text item is edited by the dialog's TextEdit record,
 * whose hText is that item's own text handle, so the two never disagree.
 *
 * Alerts are in dialog.c (shown by the host).
 */
#include <stdlib.h>
#include <string.h>

#include "machine.h"
#include "qd_internal.h"
#include "tb_internal.h"
#include "trap.h"

enum {
    userItem = 0, ctrlItem = 4, btnCtrl = 0, chkCtrl = 1, radCtrl = 2, resCtrl = 3, statText = 8, editText = 16,
    iconItem = 32, picItem = 64, itemDisable = 128,
};
enum { dialogKind = 2, inContent = 3, srcCopy = 0, teJustLeft = 0 };
enum { nullEvent = 0, mouseDown = 1, keyDown = 3, autoKey = 5, updateEvt = 6, activateEvt = 8 };

#define ITEM_HEADER 14

/* Per-dialog settings from SetDialogDefaultItem and friends, and Mac OS
 * 8's: the 'dlgx' flags and the 'dftb' font styles of the items. */
#define MAX_EXTRAS 32
enum { kDialogFlagsUseThemeBackground = 1, kDialogFlagsUseControlHierarchy = 2, kDialogFlagsUseThemeControls = 8 };
typedef struct {
    gaddr dialog;
    int16_t cancel;
    bool ring; /* draw the default button's outline */
    uint32_t flags;
    uint8_t (*fonts)[24]; /* ControlFontStyleRec by item, flags 0 for none */
    int nfonts;
    /* SetDialogTimeout's button, seconds and start (ticks), and
     * SetModalDialogEventMask's events. */
    int16_t timeout_item;
    uint32_t timeout_secs, timeout_start;
    uint16_t event_mask;
    bool has_mask;
} dialog_extra;
static dialog_extra extras[MAX_EXTRAS];

static int extra_index(gaddr d, bool create)
{
    for (int i = 0; i < MAX_EXTRAS; i++)
        if (extras[i].dialog == d)
            return i;
    if (!create)
        return -1;
    for (int i = 0; i < MAX_EXTRAS; i++)
        if (!extras[i].dialog) {
            extras[i] = (dialog_extra){ .dialog = d };
            return i;
        }
    return -1;
}

/* ---------------------------------------------------------------------- */
/* Item list access */

static gaddr items_handle(gaddr d) { return rd32(d + DialogRecord_items_off); }

static int item_count(gaddr d)
{
    gaddr h = items_handle(d);
    return h && rd32(h) ? (int16_t)rd16(rd32(h)) + 1 : 0;
}

/* Address of item n (1-based); valid until memory moves. */
static gaddr item_at(gaddr d, int n)
{
    gaddr p = rd32(items_handle(d)) + 2;
    for (int i = 1; i < n; i++)
        p += ITEM_HEADER + ((rd8(p + 13) + 1u) & ~1u);
    return p;
}

static int item_type(gaddr d, int n) { return rd8(item_at(d, n) + 12); }
static gaddr item_handle(gaddr d, int n) { return rd32(item_at(d, n)); }
static qrect item_rect(gaddr d, int n) { return qrect_get(item_at(d, n) + 4); }
static uint32_t dialog_flags(gaddr d)
{
    int x = extra_index(d, false);
    return x >= 0 ? extras[x].flags : 0;
}

/* Edit text in a theme-background dialog is white: TextEdit erases with
 * the port's background, so it is white while TextEdit works. */
typedef struct {
    bool on;
    uint16_t back[3];
} edit_bk;

static void edit_bk_begin(gaddr d, edit_bk *b)
{
    b->on = (dialog_flags(d) & kDialogFlagsUseThemeBackground) != 0;
    if (!b->on)
        return;
    gaddr c = tb_NewPtr(6, false, false);
    tb_GetBackColor(c);
    for (int i = 0; i < 3; i++)
        b->back[i] = rd16(c + (uint32_t)i * 2);
    wr16(c, 0xFFFF);
    wr16(c + 2, 0xFFFF);
    wr16(c + 4, 0xFFFF);
    tb_RGBBackColor(c);
    tb_DisposePtr(c);
}

static void edit_bk_end(const edit_bk *b)
{
    if (!b->on)
        return;
    gaddr c = tb_NewPtr(6, false, false);
    for (int i = 0; i < 3; i++)
        wr16(c + (uint32_t)i * 2, b->back[i]);
    tb_RGBBackColor(c);
    tb_DisposePtr(c);
}

/* An item's 'dftb' font style, if it has one. */
static const uint8_t *item_font(gaddr d, int n)
{
    int x = extra_index(d, false);
    if (x < 0 || n < 1 || n > extras[x].nfonts || !extras[x].fonts)
        return NULL;
    const uint8_t *f = extras[x].fonts[n - 1];
    return (f[0] | f[1]) ? f : NULL;
}

typedef struct {
    int16_t font, size;
    uint8_t face;
    bool on;
} item_font_saved;

/* The item's font in the port while it is drawn. */
static void item_font_begin(gaddr d, int n, item_font_saved *s)
{
    const uint8_t *f = item_font(d, n);
    s->on = f != NULL;
    if (!f)
        return;
    s->font = GrafPort_txFont(d);
    s->size = GrafPort_txSize(d);
    s->face = GrafPort_txFace(d);
    int flags = f[0] << 8 | f[1];
    int16_t font = (int16_t)(f[2] << 8 | f[3]), size = (int16_t)(f[4] << 8 | f[5]);
    if (flags & 1) {
        if (font == -1) {
            GrafPort_set_txFont(d, 0);
            GrafPort_set_txSize(d, 12);
        } else if (font == -2 || font == -3) {
            GrafPort_set_txFont(d, 3);
            GrafPort_set_txSize(d, 10);
            GrafPort_set_txFace(d, font == -3 ? 1 : 0);
        } else {
            GrafPort_set_txFont(d, font);
        }
    }
    if (flags & 2)
        GrafPort_set_txFace(d, f[7]);
    if (flags & 4)
        GrafPort_set_txSize(d, (int16_t)((flags & 0x100) ? GrafPort_txSize(d) + size : size));
}

static void item_font_end(gaddr d, const item_font_saved *s)
{
    if (!s->on)
        return;
    GrafPort_set_txFont(d, s->font);
    GrafPort_set_txSize(d, s->size);
    GrafPort_set_txFace(d, s->face);
}

/* A dialog: windowKind dialogKind, or a window whose dialog fields hold
 * an item list.  A program may give its dialogs a windowKind of its own,
 * or make one from a window it filled in itself (ResEdit's editors, kind
 * 135, from NewWindow), and still pass it to DialogSelect, GetDialogItem
 * and the rest, which took its word for it; only IsDialogEvent goes by
 * windowKind. */
static bool kind_dialog(gaddr w) { return w && WindowRecord_windowKind(w) == dialogKind; }

static bool is_dialog(gaddr w)
{
    if (kind_dialog(w))
        return true;
    gaddr items = w ? rd32(w + DialogRecord_items_off) : 0;
    gaddr p = items ? rd32(items) : 0;
    if (!p)
        return false;
    uint16_t err = rd16(LM_MemErr);
    bool handle = tb_RecoverHandle(p, false) == items;
    wr16(LM_MemErr, err);
    return handle;
}

static int item_for_handle(gaddr d, gaddr h)
{
    int n = item_count(d);
    for (int i = 1; i <= n; i++)
        if (item_handle(d, i) == h)
            return i;
    return 0;
}

static gaddr text_handle_from(gaddr data, uint32_t len)
{
    gaddr h = tb_NewHandle((int32_t)len, false, false);
    if (h && len)
        gmemcpy(rd32(h), data, len);
    return h;
}

/* ---------------------------------------------------------------------- */
/* Edit text */

static gaddr edit_te(gaddr d) { return rd32(d + DialogRecord_textH_off); }
static int edit_item(gaddr d) { return (int16_t)rd16(d + DialogRecord_editField_off) + 1; }

/* Make item n the active edit text item, selecting [start, end). */
static void focus_ring(gaddr d, int n, bool on);

static void activate_edit(gaddr d, int n, int start, int end)
{
    gaddr te = edit_te(d);
    if (!te)
        return;
    gaddr saved = qd_port();
    tb_SetPort(d);
    edit_bk bk;
    edit_bk_begin(d, &bk);
    tb_TEDeactivate(te);
    int old = edit_item(d);
    if (old != n)
        focus_ring(d, old, false);
    qrect r = item_rect(d, n);
    gaddr t = rd32(te);
    qrect_put(t + TERec_destRect_off, r);
    qrect_put(t + TERec_viewRect_off, r);
    TERec_set_hText(t, item_handle(d, n));
    /* The item's font ('dftb'), or the dialog's. */
    item_font_saved fs;
    item_font_begin(d, n, &fs);
    gaddr fi = tb_NewPtr(8, false, false);
    tb_GetFontInfo(fi);
    t = rd32(te);
    TERec_set_txFont(t, GrafPort_txFont(d));
    TERec_set_txSize(t, GrafPort_txSize(d));
    TERec_set_txFace(t, (int8_t)GrafPort_txFace(d));
    int asc = (int16_t)rd16(fi), desc = (int16_t)rd16(fi + 2), lead = (int16_t)rd16(fi + 6);
    TERec_set_lineHeight(t, (int16_t)(asc + desc + lead));
    TERec_set_fontAscent(t, (int16_t)asc);
    tb_DisposePtr(fi);
    item_font_end(d, &fs);
    tb_TECalText(te);
    wr16(d + DialogRecord_editField_off, (uint16_t)(n - 1));
    tb_TESetSelect(start, end, te);
    if (WindowRecord_hilited(d))
        tb_TEActivate(te);
    edit_bk_end(&bk);
    if (WindowRecord_visible(d))
        focus_ring(d, n, true);
    tb_SetPort(saved);
}

/* ---------------------------------------------------------------------- */
/* Creating and disposing */

/* Make the items from `from` on: controls, text handles, icons, pictures;
 * the first edit text item (if none is active yet) gets the TERec. */
static gaddr make_item(gaddr d, int i)
{
    gaddr it = item_at(d, i);
    int type = rd8(it + 12) & 0x7F, len = rd8(it + 13);
    gaddr made = 0;
    if (type & ctrlItem) {
        int kind = type & 3;
        gaddr r = tb_NewPtr(8, false, false);
        gaddr title = tb_NewPtr(256, false, false);
        it = item_at(d, i);
        gmemcpy(r, it + 4, 8);
        if (kind == resCtrl) {
            made = tb_GetNewControl((int16_t)rd16(it + 14), d);
            if (made) {
                qrect q = qrect_get(r);
                tb_MoveControl(made, q.left, q.top);
                tb_SizeControl(made, (int16_t)(q.right - q.left), (int16_t)(q.bottom - q.top));
            }
        } else {
            wr8(title, (uint8_t)len);
            gmemcpy(title + 1, it + 14, (uint32_t)len);
            made = tb_NewControl(d, r, title, 1, 0, 0, 1, (int16_t)kind, 0);
        }
        tb_DisposePtr(r);
        tb_DisposePtr(title);
    } else if (type == statText || type == editText) {
        gaddr data = tb_NewPtr(len ? len : 1, false, false);
        gmemcpy(data, item_at(d, i) + 14, (uint32_t)len);
        made = text_handle_from(data, (uint32_t)len);
        tb_DisposePtr(data);
    } else if (type == iconItem) {
        made = tb_GetResource((int32_t)fourcc("ICON"), (int16_t)rd16(item_at(d, i) + 14));
    } else if (type == picItem) {
        made = tb_GetPicture((int16_t)rd16(item_at(d, i) + 14));
    }
    return made;
}

/* The dialog's TERec, made for edit text item n when it has none yet. */
static void make_edit(gaddr d, int n)
{
    if (rd32(d + DialogRecord_textH_off))
        return;
    gaddr r = tb_NewPtr(8, false, false);
    qrect_put(r, item_rect(d, n));
    gaddr saved = qd_port();
    tb_SetPort(d);
    gaddr te = tb_TENew(r, r);
    tb_SetPort(saved);
    tb_DisposePtr(r);
    tb_DisposeHandle(TERec_hText(rd32(te)));
    wr32(d + DialogRecord_textH_off, te);
    activate_edit(d, n, 0, 32767);
}

static void make_items_from(gaddr d, int from)
{
    int n = item_count(d);
    int first_edit = 0;
    for (int i = from; i <= n; i++) {
        gaddr made = make_item(d, i);
        if ((item_type(d, i) & 0x7F) == editText && !first_edit)
            first_edit = i;
        wr32(item_at(d, i), made); /* user items keep their (nil) procedure */
    }
    if (from == 1) {
        wr16(d + DialogRecord_editField_off, (uint16_t)-1);
        wr32(d + DialogRecord_textH_off, 0);
    }
    if (first_edit)
        make_edit(d, first_edit);
}

static void make_items(gaddr d) { make_items_from(d, 1); }

/* The size of an item list's items, after its count word. */
static uint32_t ditl_items_size(gaddr p)
{
    int n = (int16_t)rd16(p) + 1;
    gaddr at = p + 2;
    for (int i = 0; i < n; i++)
        at += ITEM_HEADER + ((rd8(at + 13) + 1u) & ~1u);
    return at - (p + 2);
}

/* AppendDITL(dialog, theDITL, method): the list's items added after the
 * dialog's, placed over the dialog (overlayDITL 0), to the right of it
 * (appendDITLRight 1, the window growing to hold them), below it
 * (appendDITLBottom 2), or relative to item -method's top left corner. */
void tb_AppendDITL(gaddr d, gaddr ditl, int16_t method)
{
    gaddr items = items_handle(d);
    if (!d || !items || !rd32(items) || !ditl || !rd32(ditl))
        return;
    int old = item_count(d), added = (int16_t)rd16(rd32(ditl)) + 1;
    qrect port = qrect_get(d + GrafPort_portRect_off);
    int dh = 0, dv = 0;
    if (method == 1)
        dh = port.right;
    else if (method == 2)
        dv = port.bottom;
    else if (method < 0 && -method <= old) {
        qrect r = item_rect(d, -method);
        dh = r.left;
        dv = r.top;
    }
    uint32_t have = (uint32_t)tb_GetHandleSize(items), more = ditl_items_size(rd32(ditl));
    tb_SetHandleSize(items, (int32_t)(have + more));
    if (rd16(LM_MemErr))
        return;
    gmemcpy(rd32(items) + have, rd32(ditl) + 2, more);
    wr16(rd32(items), (uint16_t)(old + added - 1));
    int right = port.right, bottom = port.bottom;
    for (int i = old + 1; i <= old + added; i++) {
        gaddr it = item_at(d, i);
        qrect r = qrect_get(it + 4);
        r.left += dh;
        r.right += dh;
        r.top += dv;
        r.bottom += dv;
        qrect_put(it + 4, r);
        if (r.right + 8 > right)
            right = r.right + 8;
        if (r.bottom + 8 > bottom)
            bottom = r.bottom + 8;
    }
    make_items_from(d, old + 1);
    if (method == 1 || method == 2)
        tb_SizeWindow(d, (int16_t)(right - port.left), (int16_t)(bottom - port.top), true);
}

/* ShortenDITL(dialog, numberItems): the last items go. */
void tb_ShortenDITL(gaddr d, int16_t count)
{
    int n = item_count(d);
    if (count <= 0 || !n)
        return;
    if (count > n)
        count = (int16_t)n;
    int keep = n - count;
    for (int i = keep + 1; i <= n; i++) {
        int type = item_type(d, i) & 0x7F;
        gaddr h = item_handle(d, i);
        if ((type & ctrlItem) && h)
            tb_DisposeControl(h);
        else if ((type == statText || type == editText) && h)
            tb_DisposeHandle(h);
        if (edit_item(d) == i) {
            wr16(d + DialogRecord_editField_off, (uint16_t)-1);
        }
    }
    gaddr items = items_handle(d);
    gaddr end = keep ? item_at(d, keep) + ITEM_HEADER + ((rd8(item_at(d, keep) + 13) + 1u) & ~1u) : rd32(items) + 2;
    tb_SetHandleSize(items, (int32_t)(end - rd32(items)));
    wr16(rd32(items), (uint16_t)(keep - 1));
}

int16_t tb_CountDITL(gaddr d) { return (int16_t)item_count(d); }

/* A dialog, with Mac OS 8's flags ('dlgx', NewFeaturesDialog): the
 * theme's background, a root control the item controls go in, and the
 * standard controls in Platinum.  As the Dialog Manager did, NewDialog
 * makes a plain window (a GrafPort) and NewColorDialog a colour one;
 * GetNewDialog makes a colour one when the dialog has a 'dctb' (Microsoft
 * Word 5.0 centres its dialogs from portBits.bounds, which a CGrafPort
 * does not have).  Dialogs with Mac OS 8's features are colour windows. */
static gaddr new_dialog(gaddr dst, gaddr r, gaddr tit, int8_t vis, int16_t procid, gaddr behind, int8_t gaflag,
                        int32_t rc, gaddr items, uint32_t flags, bool color)
{
    gaddr d = dst ? dst : tb_NewPtr(DialogRecord_SIZEOF, false, true);
    if (!d)
        return 0;
    if (color || flags)
        tb_NewCWindow(d, r, tit, 0, procid, behind, gaflag, rc);
    else
        tb_NewWindow(d, r, tit, 0, procid, behind, gaflag, rc);
    WindowRecord_set_windowKind(d, dialogKind);
    wr32(d + DialogRecord_items_off, items);
    wr16(d + DialogRecord_aDefItem_off, 1);
    wr16(d + DialogRecord_editOpen_off, 0);
    gaddr saved = qd_port();
    tb_SetPort(d);
    tb_TextFont((int16_t)rd16(LM_DlgFont));
    tb_TextSize(12);
    tb_SetPort(saved);
    if (flags) {
        int x = extra_index(d, true);
        if (x >= 0)
            extras[x].flags = flags;
        if (flags & kDialogFlagsUseThemeBackground)
            tb_SetThemeWindowBackground(d, 1, false); /* kThemeBrushDialogBackgroundActive */
        if (flags & (kDialogFlagsUseThemeControls | kDialogFlagsUseThemeBackground))
            ctl_set_window_themed(d, true);
        if (flags & kDialogFlagsUseControlHierarchy)
            tb_CreateRootControl(d, 0);
    }
    make_items(d);
    if (vis)
        tb_ShowWindow(d);
    return d;
}

gaddr tb_NewDialog(gaddr dst, gaddr r, gaddr tit, int8_t vis, int16_t procid, gaddr behind, int8_t gaflag, int32_t rc,
                   gaddr items)
{
    return new_dialog(dst, r, tit, vis, procid, behind, gaflag, rc, items, 0, false);
}

gaddr tb_NewFeaturesDialog(gaddr dst, gaddr r, gaddr tit, int8_t vis, int16_t procid, gaddr behind, int8_t gaflag,
                           int32_t rc, gaddr items, uint32_t flags)
{
    return new_dialog(dst, r, tit, vis, procid, behind, gaflag, rc, items, flags, true);
}

/* 'dftb': version, count, then for each item 0 (none) or 1 and a
 * ControlFontStyleRec's fields with a font name after them. */
static void apply_dftb(gaddr d, int16_t id)
{
    gaddr h = tb_GetResource((int32_t)fourcc("dftb"), id);
    if (!h || !rd32(h))
        return;
    uint32_t len = (uint32_t)tb_GetHandleSize(h);
    gaddr p = rd32(h);
    if (len < 4 || rd16(p) != 0)
        return;
    int n = (int16_t)rd16(p + 2);
    int x = extra_index(d, true);
    if (x < 0 || n <= 0)
        return;
    free(extras[x].fonts);
    extras[x].fonts = calloc((size_t)n, 24);
    extras[x].nfonts = n;
    uint32_t o = 4;
    for (int i = 0; i < n && o + 2 <= len; i++) {
        int type = (int16_t)rd16(p + o);
        o += 2;
        if (type != 1)
            continue;
        if (o + 24 + 1 > len)
            break;
        uint8_t *f = extras[x].fonts[i];
        gget(f, p + o, 24);
        int flags = f[0] << 8 | f[1];
        uint32_t nl = rd8(p + o + 24);
        if ((flags & 0x200) && nl) { /* kDialogFontUseFontNameMask: the font by name */
            gaddr name = tb_NewPtr(256, false, true), num = tb_NewPtr(2, false, true);
            gmemcpy(name, p + o + 24, nl + 1);
            tb_GetFNum(name, num);
            f[2] = (uint8_t)(rd16(num) >> 8);
            f[3] = (uint8_t)rd16(num);
            flags |= 1;
            f[0] = (uint8_t)(flags >> 8);
            f[1] = (uint8_t)flags;
            tb_DisposePtr(num);
            tb_DisposePtr(name);
        }
        o += 24 + 1 + nl;
        if (i < item_count(d) && (item_type(d, i + 1) & ctrlItem) && item_handle(d, i + 1)) {
            gaddr rec = tb_NewPtr(24, false, false);
            gput(rec, f, 24);
            tb_SetControlFontStyle(item_handle(d, i + 1), rec);
            tb_DisposePtr(rec);
        }
        h = tb_GetResource((int32_t)fourcc("dftb"), id); /* (memory may have moved) */
        p = h && rd32(h) ? rd32(h) : 0;
        if (!p)
            break;
        x = extra_index(d, true);
    }
}

/* 'ictb' (the item list's ID, with a 'dctb'): for each item its data and
 * offset words; a text item's data says which fields of the 20-byte text
 * style at the offset apply (bit 0 font, 1 face, 2 size, 3 colour, 4 the
 * size added, 15 the font by the name at an offset).  Text items take the
 * font, face and size, kept as 'dftb' styles are; an item 'dftb' styled
 * keeps that. */
static void apply_ictb(gaddr d, int16_t id)
{
    gaddr h = tb_GetResource((int32_t)fourcc("ictb"), id);
    if (!h || !rd32(h))
        return;
    uint32_t len = (uint32_t)tb_GetHandleSize(h);
    int n = item_count(d);
    int x = extra_index(d, true);
    if (x < 0 || n <= 0)
        return;
    if (extras[x].nfonts < n) {
        extras[x].fonts = realloc(extras[x].fonts, (size_t)n * 24);
        memset(extras[x].fonts + extras[x].nfonts, 0, (size_t)(n - extras[x].nfonts) * 24);
        extras[x].nfonts = n;
    }
    for (int i = 1; i <= n && (uint32_t)i * 4 <= len; i++) {
        int type = item_type(d, i) & 0x7F;
        gaddr p = rd32(h);
        int data = rd16(p + (uint32_t)(i - 1) * 4);
        uint32_t off = rd16(p + (uint32_t)(i - 1) * 4 + 2);
        uint8_t *f = extras[x].fonts[i - 1];
        if ((type != statText && type != editText) || !(data & 0x8017) || off + 20 > len || (f[0] | f[1]))
            continue;
        int flags = 0;
        int16_t font = (int16_t)rd16(p + off);
        if (data & 0x8000) { /* the font by name */
            uint32_t at = (uint16_t)font;
            if (at < len && at + 1 + rd8(p + at) <= len) {
                gaddr name = tb_NewPtr(256, false, true), num = tb_NewPtr(2, false, true);
                gmemcpy(name, rd32(h) + at, rd8(rd32(h) + at) + 1u);
                tb_GetFNum(name, num);
                font = (int16_t)rd16(num);
                flags |= 1;
                tb_DisposePtr(num);
                tb_DisposePtr(name);
                p = rd32(h);
            }
        } else if (data & 1) {
            flags |= 1;
        }
        if (data & 2)
            flags |= 2;
        if (data & 0x14)
            flags |= (data & 0x10) ? 0x104 : 4;
        int16_t size = (int16_t)rd16(p + off + 4);
        f[0] = (uint8_t)(flags >> 8);
        f[1] = (uint8_t)flags;
        f[2] = (uint8_t)(font >> 8);
        f[3] = (uint8_t)font;
        f[4] = (uint8_t)(size >> 8);
        f[5] = (uint8_t)size;
        f[6] = 0;
        f[7] = (uint8_t)rd16(p + off + 2);
    }
}

gaddr tb_NewColorDialog(gaddr dst, gaddr r, gaddr tit, int8_t vis, int16_t procid, gaddr behind, int8_t gaflag,
                        int32_t rc, gaddr items)
{
    return new_dialog(dst, r, tit, vis, procid, behind, gaflag, rc, items, 0, true);
}

/* DLOG: bounds(8), procID(2), visible(1), pad, goAway(1), pad, refCon(4), itemsID(2), title. */
gaddr dialog_from_resource(int16_t id, gaddr dst, gaddr behind, int vis_override, int32_t refcon_override,
                           bool use_refcon)
{
    gaddr dlog = tb_GetResource((int32_t)fourcc("DLOG"), id);
    if (!dlog || !rd32(dlog))
        return 0;
    int16_t ditl_id = (int16_t)rd16(rd32(dlog) + 18);
    gaddr ditl = tb_GetResource((int32_t)fourcc("DITL"), ditl_id);
    if (!ditl || !rd32(ditl))
        return 0;
    uint32_t items = ditl;
    tb_HandToHand(&items);
    tb_HLock(dlog);
    gaddr p = rd32(dlog);
    int vis = vis_override >= 0 ? vis_override : rd8(p + 10);
    int32_t rc = use_refcon ? refcon_override : (int32_t)rd32(p + 14);
    /* 'dlgx' (version 0, flags) asks for Mac OS 8's features. */
    gaddr x = tb_GetResource((int32_t)fourcc("dlgx"), id);
    uint32_t flags = x && rd32(x) && tb_GetHandleSize(x) >= 6 && rd16(rd32(x)) == 0 ? rd32(rd32(x) + 2) : 0;
    gaddr dctb = tb_GetResource((int32_t)fourcc("dctb"), id);
    bool color = dctb && rd32(dctb);
    p = rd32(dlog);
    /* Shown after its fonts and colours are in. */
    gaddr d = new_dialog(dst, p, p + 20, 0, (int16_t)rd16(p + 8), behind, (int8_t)rd8(p + 12), rc, items, flags,
                         color);
    tb_HUnlock(dlog);
    if (d && color && rd32(dctb) && (int16_t)rd16(rd32(dctb) + 6) != -1) /* ctSize -1: the default colours */
        tb_SetWinColor(d, dctb);
    if (d)
        apply_dftb(d, id);
    if (d && color)
        apply_ictb(d, ditl_id);
    if (d && vis)
        tb_ShowWindow(d);
    return d;
}

gaddr tb_GetNewDialog(int16_t id, gaddr dst, gaddr behind) { return dialog_from_resource(id, dst, behind, -1, 0, false); }

void tb_CloseDialog(gaddr d)
{
    ap_forget_dialog(d);
    gaddr te = edit_te(d);
    if (te) {
        TERec_set_hText(rd32(te), tb_NewHandle(0, false, false)); /* the item owns its text */
        tb_TEDispose(te);
        wr32(d + DialogRecord_textH_off, 0);
    }
    int n = item_count(d);
    for (int i = 1; i <= n; i++) {
        int type = item_type(d, i) & 0x7F;
        if (type == statText || type == editText)
            tb_DisposeHandle(item_handle(d, i));
    }
    tb_CloseWindow(d); /* disposes the controls */
    if (items_handle(d))
        tb_DisposeHandle(items_handle(d));
    wr32(d + DialogRecord_items_off, 0);
    int x = extra_index(d, false);
    if (x >= 0) {
        free(extras[x].fonts);
        extras[x] = (dialog_extra){ 0 };
    }
}

void tb_DisposeDialog(gaddr d)
{
    tb_CloseDialog(d);
    tb_DisposePtr(d);
}

/* ---------------------------------------------------------------------- */
/* Items */

void tb_GetDialogItem(gaddr d, int16_t n, gaddr itype, gaddr item, gaddr r)
{
    if (n < 1 || n > item_count(d))
        return;
    gaddr it = item_at(d, n);
    if (itype)
        wr16(itype, rd8(it + 12));
    if (item)
        wr32(item, rd32(it));
    if (r)
        gmemcpy(r, it + 4, 8);
}

void tb_SetDialogItem(gaddr d, int16_t n, int16_t itype, gaddr item, gaddr r)
{
    if (n < 1 || n > item_count(d))
        return;
    gaddr it = item_at(d, n);
    wr32(it, item);
    gmemcpy(it + 4, r, 8);
    wr8(it + 12, (uint8_t)itype);
}

/* A text item's handle, given it or (with the Appearance Manager) the
 * control GetDialogItemAsControl gave for the item: MacMAME sets its
 * static text through the control, whose record was resized to the text. */
static gaddr text_handle(gaddr h)
{
    gaddr d;
    int n;
    if (ap_stand_in_item(h, &d, &n) && is_dialog(d) && n >= 1 && n <= item_count(d))
        return item_handle(d, n);
    return h;
}

void tb_GetDialogItemText(gaddr item, gaddr text)
{
    item = text_handle(item);
    uint32_t n = item && rd32(item) ? (uint32_t)tb_GetHandleSize(item) : 0;
    if (n > 255)
        n = 255;
    wr8(text, (uint8_t)n);
    if (n)
        gmemcpy(text + 1, rd32(item), n);
}

static void draw_item(gaddr d, int n);

void tb_SetDialogItemText(gaddr item, gaddr text)
{
    item = text_handle(item);
    if (!item)
        return;
    uint32_t n = rd8(text);
    tb_SetHandleSize(item, (int32_t)n);
    if (n)
        gmemcpy(rd32(item), text + 1, n);
    /* Redraw it if it belongs to the current port's dialog. */
    gaddr d = qd_port();
    if (!is_dialog(d))
        return;
    int i = item_for_handle(d, item);
    if (!i)
        return;
    if (i == edit_item(d) && edit_te(d)) {
        tb_TECalText(edit_te(d));
        tb_TESetSelect(32767, 32767, edit_te(d));
    }
    if (WindowRecord_visible(d)) {
        gaddr r = tb_NewPtr(8, false, false);
        qrect_put(r, item_rect(d, i));
        tb_EraseRect(r);
        tb_DisposePtr(r);
        draw_item(d, i);
    }
}

void tb_SelectDialogItemText(gaddr d, int16_t n, int16_t start, int16_t stop)
{
    if (n >= 1 && n <= item_count(d) && (item_type(d, n) & 0x7F) == editText)
        activate_edit(d, n, start, stop);
}

void tb_HideDialogItem(gaddr d, int16_t n)
{
    if (n < 1 || n > item_count(d))
        return;
    qrect r = item_rect(d, n);
    if (r.left > 8192)
        return;
    gaddr h = item_handle(d, n);
    if (item_type(d, n) & ctrlItem)
        tb_HideControl(h);
    r.left += 16384;
    r.right += 16384;
    qrect_put(item_at(d, n) + 4, r);
}

void tb_ShowDialogItem(gaddr d, int16_t n)
{
    if (n < 1 || n > item_count(d))
        return;
    qrect r = item_rect(d, n);
    if (r.left <= 8192)
        return;
    r.left -= 16384;
    r.right -= 16384;
    qrect_put(item_at(d, n) + 4, r);
    if (item_type(d, n) & ctrlItem)
        tb_ShowControl(item_handle(d, n));
    else
        draw_item(d, n);
}

int16_t tb_FindDialogItem(gaddr d, uint32_t pt)
{
    int n = item_count(d), h = pt_h(pt), v = pt_v(pt);
    for (int i = 1; i <= n; i++) {
        qrect r = item_rect(d, i);
        if (h >= r.left && h < r.right && v >= r.top && v < r.bottom)
            return (int16_t)(i - 1);
    }
    return -1;
}

/* ---------------------------------------------------------------------- */
/* Drawing */

/* Static text with ^0..^3 replaced by the ParamText strings. */
static void draw_text_item(gaddr h, qrect r, bool substitute)
{
    uint32_t n = h && rd32(h) ? (uint32_t)tb_GetHandleSize(h) : 0;
    uint8_t *src = malloc(n ? n : 1), *out = malloc(n + 4 * 256 + 1);
    gget(src, rd32(h), n);
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (substitute && src[i] == '^' && i + 1 < n && src[i + 1] >= '0' && src[i + 1] <= '3') {
            gaddr s = rd32(LM_DAStrings + (uint32_t)(src[i + 1] - '0') * 4);
            if (s && rd32(s)) {
                uint32_t sl = rd8(rd32(s));
                gget(out + k, rd32(s) + 1, sl);
                k += sl;
            }
            i++;
        } else {
            out[k++] = src[i];
        }
    }
    gaddr text = tb_NewPtr((int32_t)(k ? k : 1), false, false);
    gput(text, out, k);
    gaddr rp = tb_NewPtr(8, false, false);
    qrect_put(rp, r);
    tb_TETextBox(text, (int32_t)k, rp, teJustLeft);
    tb_DisposePtr(rp);
    tb_DisposePtr(text);
    free(src);
    free(out);
}

/* Platinum's edit text: white inside a well frame, the one being edited
 * with the focus ring (theme dialogs, in colour). */
static bool platinum_edit(gaddr d)
{
    return (dialog_flags(d) & (kDialogFlagsUseThemeBackground | kDialogFlagsUseThemeControls)) &&
           g_appearance == MACHINE_APPEARANCE_BW && ap_in_color();
}

static void focus_ring(gaddr d, int n, bool on)
{
    if (n < 1 || n > item_count(d) || (item_type(d, n) & 0x7F) != editText || !platinum_edit(d))
        return;
    qrect r = item_rect(d, n);
    if (r.left > 8192)
        return;
    gaddr saved = qd_port();
    tb_SetPort(d);
    ap_focus_rect((qrect){ (int16_t)(r.top - 4), (int16_t)(r.left - 4), (int16_t)(r.bottom + 4), (int16_t)(r.right + 4) },
                  on && WindowRecord_hilited(d));
    tb_SetPort(saved);
}

static void draw_item(gaddr d, int n)
{
    int type = item_type(d, n) & 0x7F;
    qrect r = item_rect(d, n);
    if (r.left > 8192)
        return; /* hidden */
    gaddr h = item_handle(d, n);
    gaddr rp = tb_NewPtr(8, false, false);
    qrect_put(rp, r);
    item_font_saved fs;
    item_font_begin(d, n, &fs);
    if (type & ctrlItem) {
        if (h)
            tb_Draw1Control(h);
    } else if (type == statText) {
        draw_text_item(h, r, true);
    } else if (type == editText) {
        qrect f = { (int16_t)(r.top - 3), (int16_t)(r.left - 3), (int16_t)(r.bottom + 3), (int16_t)(r.right + 3) };
        bool platinum = platinum_edit(d);
        if (platinum) {
            ap_fill_grey((qrect){ (int16_t)(r.top - 2), (int16_t)(r.left - 2), (int16_t)(r.bottom + 2), (int16_t)(r.right + 2) }, 0xFFFF);
            ap_well_frame((qrect){ (int16_t)(r.top - 2), (int16_t)(r.left - 2), (int16_t)(r.bottom + 2), (int16_t)(r.right + 2) },
                          WindowRecord_hilited(d) ? 1 : 0);
        } else if (!native_draw_edit_frame(f, n == edit_item(d))) {
            gaddr fp = tb_NewPtr(8, false, false);
            qrect_put(fp, f);
            tb_PenNormal();
            tb_FrameRect(fp);
            tb_DisposePtr(fp);
        }
        edit_bk bk;
        edit_bk_begin(d, &bk);
        if (n == edit_item(d) && edit_te(d))
            tb_TEUpdate(rp, edit_te(d));
        else
            draw_text_item(h, r, false);
        edit_bk_end(&bk);
        if (platinum && n == edit_item(d) && edit_te(d))
            focus_ring(d, n, true);
    } else if (type == userItem && ap_item_draw(d, n)) {
        /* its stand-in's drawing procedure (a user pane's) */
    } else if (type == iconItem) {
        if (h)
            tb_PlotIcon(rp, h);
    } else if (type == picItem) {
        if (h)
            tb_DrawPicture(h, rp);
    } else if (type == userItem && h) {
        cpu_push32(g_cpu, d);
        cpu_push16(g_cpu, (uint16_t)n);
        guest_call(h);
    }
    item_font_end(d, &fs);
    tb_DisposePtr(rp);
}

/* For the items' stand-in controls (cdefs_appearance.c). */
void dialog_draw_item(gaddr d, int n)
{
    if (n < 1 || n > item_count(d))
        return;
    gaddr saved = qd_port();
    tb_SetPort(d);
    draw_item(d, n);
    tb_SetPort(saved);
}

void dialog_select_item_text(gaddr d, int n, int start, int end)
{
    if (n >= 1 && n <= item_count(d) && (item_type(d, n) & 0x7F) == editText)
        activate_edit(d, n, start, end);
}

gaddr dialog_item_te(gaddr d, int n) { return n == edit_item(d) ? edit_te(d) : 0; }

static void draw_default_ring(gaddr d)
{
    int x = extra_index(d, false);
    if (x < 0 || !extras[x].ring)
        return;
    int n = (int16_t)rd16(d + DialogRecord_aDefItem_off);
    if (n < 1 || n > item_count(d) || !(item_type(d, n) & ctrlItem))
        return;
    qrect r = item_rect(d, n);
    gaddr button = item_handle(d, n);
    if (button && rd32(button) && ctl_platinum(button)) { /* Platinum's ring */
        uint8_t hil = ControlRecord_contrlHilite(rd32(button));
        ap_default_ring(qrect_get(rd32(button) + ControlRecord_contrlRect_off), hil == 255 ? 0 : 1);
        return;
    }
    gaddr rp = tb_NewPtr(8, false, false);
    qrect_put(rp, (qrect){ (int16_t)(r.top - 4), (int16_t)(r.left - 4), (int16_t)(r.bottom + 4), (int16_t)(r.right + 4) });
    tb_PenSize(3, 3);
    tb_FrameRoundRect(rp, 16, 16);
    tb_PenNormal();
    tb_DisposePtr(rp);
}

void tb_DrawDialog(gaddr d)
{
    gaddr saved = qd_port();
    tb_SetPort(d);
    /* In item order, controls among the rest, as the Dialog Manager draws
     * them: a later item over an earlier one (SimpleText's save dialog
     * puts its icons over wide, untitled radio buttons). */
    int n = item_count(d);
    for (int i = 1; i <= n; i++) {
        if (item_type(d, i) & ctrlItem) {
            gaddr c = item_handle(d, i);
            if (c && rd32(c))
                tb_Draw1Control(c);
        } else {
            draw_item(d, i);
        }
    }
    /* Controls the program added with NewControl, outside the item list,
     * are the dialog's too (DrawControls draws them all): Installer VISE's
     * Install Location pop-up.  With a root control, those in it that are
     * not items (an embedded control is drawn with its embedder). */
    gaddr root = ctl_window_root(d);
    for (gaddr c = WindowRecord_controlList(d); c && rd32(c); c = ControlRecord_nextControl(rd32(c))) {
        bool item = false;
        for (int i = 1; i <= n && !item; i++)
            item = (item_type(d, i) & ctrlItem) && item_handle(d, i) == c;
        if (item)
            continue;
        ctl_info *ci = ctl_find(c);
        if (c == root) {
            gaddr subs[256];
            int k = ctl_subcontrols(root, subs, 256);
            for (int j = 0; j < k; j++) {
                bool sub_item = false;
                for (int i = 1; i <= n && !sub_item; i++)
                    sub_item = (item_type(d, i) & ctrlItem) && item_handle(d, i) == subs[j];
                if (!sub_item)
                    tb_Draw1Control(subs[j]);
            }
        } else if (!ci || !ci->parent) {
            tb_Draw1Control(c);
        }
    }
    draw_default_ring(d);
    tb_SetPort(saved);
}

void tb_UpdateDialog(gaddr d, gaddr rgn) { tb_DrawDialog(d); }

void tb_PlotIcon(gaddr rect, gaddr icon)
{
    if (!icon || !rd32(icon))
        return;
    gaddr bm = tb_NewPtr(14, false, true);
    gaddr src = tb_NewPtr(8, false, false);
    tb_HLock(icon);
    wr32(bm, rd32(icon));
    wr16(bm + 4, 4);
    qrect_put(bm + 6, (qrect){ 0, 0, 32, 32 });
    qrect_put(src, (qrect){ 0, 0, 32, 32 });
    gaddr port = qd_port();
    tb_CopyBits(bm, port + GrafPort_portBits_off, src, rect, srcCopy, 0);
    tb_HUnlock(icon);
    tb_DisposePtr(src);
    tb_DisposePtr(bm);
}

/* ---------------------------------------------------------------------- */
/* Events */

static bool item_enabled(gaddr d, int n) { return n >= 1 && n <= item_count(d) && !(item_type(d, n) & itemDisable); }

int8_t tb_IsDialogEvent(gaddr evt)
{
    int what = (int16_t)rd16(evt);
    if (what == updateEvt || what == activateEvt)
        return kind_dialog(rd32(evt + EventRecord_message_off));
    gaddr front = tb_FrontWindow();
    if (!kind_dialog(front))
        return 0;
    if (what == mouseDown) {
        gaddr wp = tb_NewPtr(4, false, true);
        int16_t part = tb_FindWindow(rd32(evt + EventRecord_where_off), wp);
        bool mine = part == inContent && rd32(wp) == front;
        tb_DisposePtr(wp);
        return mine;
    }
    return what == keyDown || what == autoKey || what == nullEvent;
}

static bool host_alert_instead(gaddr d, gaddr itemp);

int8_t tb_DialogSelect(gaddr evt, gaddr dpp, gaddr itemp)
{
    int what = (int16_t)rd16(evt);
    gaddr d = (what == updateEvt || what == activateEvt) ? rd32(evt + EventRecord_message_off) : tb_FrontWindow();
    if (!is_dialog(d))
        return 0;
    /* A program's own loop for a movable modal alert (SimpleText's "Save
     * changes?"): the host's alert answers it. */
    if (d == tb_FrontWindow() && host_alert_instead(d, itemp)) {
        if (dpp)
            wr32(dpp, d);
        return 1;
    }
    if (dpp)
        wr32(dpp, d);
    gaddr saved = qd_port();
    tb_SetPort(d);
    int8_t result = 0;
    gaddr te = edit_te(d);
    switch (what) {
    case updateEvt:
        tb_BeginUpdate(d);
        tb_DrawDialog(d);
        tb_EndUpdate(d);
        break;
    case activateEvt: {
        bool on = rd16(evt + EventRecord_modifiers_off) & 1;
        if (te) {
            edit_bk bk;
            edit_bk_begin(d, &bk);
            if (on)
                tb_TEActivate(te);
            else
                tb_TEDeactivate(te);
            edit_bk_end(&bk);
            focus_ring(d, edit_item(d), on);
        }
        gaddr root = ctl_window_root(d); /* Mac OS 8's dialogs dim their controls when inactive */
        if (root) {
            if (on)
                tb_ActivateControl(root);
            else
                tb_DeactivateControl(root);
        }
        break;
    }
    case mouseDown: {
        gaddr pt = tb_NewPtr(4, false, false);
        wr32(pt, rd32(evt + EventRecord_where_off));
        tb_GlobalToLocal(pt);
        uint32_t p = rd32(pt);
        tb_DisposePtr(pt);
        gaddr chp = tb_NewPtr(4, false, true);
        int16_t part = tb_FindControl(p, d, chp);
        gaddr ch = rd32(chp);
        tb_DisposePtr(chp);
        int n = 0;
        if (part) {
            if (tb_TrackControl(ch, p, 0xFFFFFFFFu)) /* as the Dialog Manager did */
                n = item_for_handle(d, ch);
        } else {
            n = tb_FindDialogItem(d, p) + 1;
            if (n > 0 && (item_type(d, n) & 0x7F) == editText && te) {
                if (n != edit_item(d))
                    activate_edit(d, n, 0, 0);
                edit_bk bk;
                edit_bk_begin(d, &bk);
                tb_TEClick(p, (rd16(evt + EventRecord_modifiers_off) & 0x0200) != 0, te);
                edit_bk_end(&bk);
            } else if (n > 0 && (item_type(d, n) & 0x7F) == userItem &&
                       !ap_item_click(d, n, p, (int16_t)rd16(evt + EventRecord_modifiers_off))) {
                n = 0; /* a user pane's hit test said no */
            }
        }
        if (n > 0 && item_enabled(d, n)) {
            wr16(itemp, (uint16_t)n);
            result = 1;
        }
        break;
    }
    case keyDown:
    case autoKey: {
        if (!te || (rd16(evt + EventRecord_modifiers_off) & 0x0100))
            break;
        int c = (int)(rd32(evt + EventRecord_message_off) & 0xFF);
        if (c == '\t') { /* next edit text item */
            int n = item_count(d), cur = edit_item(d);
            for (int k = 1; k <= n; k++) {
                int i = (cur - 1 + k) % n + 1;
                if ((item_type(d, i) & 0x7F) == editText) {
                    activate_edit(d, i, 0, 32767);
                    break;
                }
            }
            break;
        }
            edit_bk bk;
            edit_bk_begin(d, &bk);
            tb_TEKey((int16_t)c, te);
            edit_bk_end(&bk);
            int n = edit_item(d);
        if (item_enabled(d, n)) {
            wr16(itemp, (uint16_t)n);
            result = 1;
        }
        break;
    }
    default:
        if (te) {
            edit_bk bk;
            edit_bk_begin(d, &bk);
            tb_TEIdle(te);
            edit_bk_end(&bk);
        }
        break;
    }
    tb_SetPort(saved);
    return result;
}

/* ---------------------------------------------------------------------- */
/* Modal dialogs */

static void flash_button(gaddr d, int n)
{
    if (!(item_type(d, n) & ctrlItem))
        return;
    gaddr ch = item_handle(d, n);
    uint32_t ticks;
    tb_HiliteControl(ch, 1);
    tb_Delay(8, &ticks);
    tb_HiliteControl(ch, 0);
}

/* The standard filter: Return and Enter press the default button; Escape
 * and Command-period the cancel button (when one is set). */
static bool std_filter(gaddr d, gaddr evt, gaddr itemp)
{
    int what = (int16_t)rd16(evt);
    if (what != keyDown && what != autoKey)
        return false;
    int c = (int)(rd32(evt + EventRecord_message_off) & 0xFF);
    bool cmd = (rd16(evt + EventRecord_modifiers_off) & 0x0100) != 0;
    int n = 0;
    if (c == '\r' || c == 3) {
        n = (int16_t)rd16(d + DialogRecord_aDefItem_off);
    } else if (c == 27 || (cmd && c == '.')) {
        int x = extra_index(d, false);
        n = x >= 0 ? extras[x].cancel : 0;
    }
    if (!item_enabled(d, n))
        return false;
    flash_button(d, n);
    wr16(itemp, (uint16_t)n);
    return true;
}

static void std_filter_native(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    gaddr itemp = cpu_pop32(c), evt = cpu_pop32(c), d = cpu_pop32(c);
    bool r = std_filter(d, evt, itemp);
    wr16(c->a[7], r ? 0x0100 : 0);
    cpu_push32(c, ret);
}

static gaddr std_filter_stub;

void dialogs_reset(void)
{
    std_filter_stub = 0;
    memset(extras, 0, sizeof extras);
}

int16_t tb_GetStdFilterProc(gaddr proc)
{
    if (!std_filter_stub)
        std_filter_stub = native_register(std_filter_native, "StdFilterProc", 0);
    wr32(proc, std_filter_stub);
    return noErr;
}

int16_t tb_SetDialogDefaultItem(gaddr d, int16_t n)
{
    wr16(d + DialogRecord_aDefItem_off, (uint16_t)n);
    int x = extra_index(d, true);
    if (x >= 0)
        extras[x].ring = true;
    return noErr;
}

int16_t tb_SetDialogCancelItem(gaddr d, int16_t n)
{
    int x = extra_index(d, true);
    if (x >= 0)
        extras[x].cancel = n;
    return noErr;
}

int16_t tb_SetDialogTracksCursor(gaddr d, int8_t tracks) { return noErr; }

/* A program's own dialog saying it needs another screen depth, checked the
 * first time ModalDialog runs it. */
static bool depth_request(gaddr d)
{
    static gaddr checked;
    if (d == checked)
        return false;
    checked = d;
    char msg[1024];
    size_t n = 0;
    msg[0] = 0;
    for (int i = 1; i <= item_count(d); i++) {
        if ((item_type(d, i) & 0x7F) != statText)
            continue;
        gaddr h = item_handle(d, i);
        if (!h || !rd32(h))
            continue;
        uint32_t len = (uint32_t)tb_GetHandleSize(h);
        uint8_t text[512];
        if (len > sizeof text)
            len = sizeof text;
        gget(text, rd32(h), len);
        if (n)
            n = append_text(msg, n, sizeof msg, (const uint8_t *)" ", 1);
        n = append_text(msg, n, sizeof msg, text, len);
    }
    return n && depth_alert(msg);
}

bool dialog_host_alert(machine_alert *a, int *result);
bool dialog_host_alerts(void);

/* A dialog that is an alert in all but name ("Save changes before
 * closing?"): static text, push buttons, perhaps a stop, note or caution
 * icon, and user items with nothing to draw or that only ring a button.
 * The host shows it as one of its own alerts. */
static bool as_host_alert(gaddr d, machine_alert *a)
{
    memset(a, 0, sizeof *a);
    a->kind = MACHINE_ALERT_PLAIN;
    int n = item_count(d);
    size_t tn = 0;
    for (int i = 1; i <= n; i++) {
        int type = item_type(d, i) & 0x7F;
        gaddr h = item_handle(d, i);
        qrect r = item_rect(d, i);
        if (r.left >= 8192) /* hidden */
            continue;
        if (type == (ctrlItem | btnCtrl)) {
            if (a->nbuttons >= (int)(sizeof a->buttons / sizeof a->buttons[0]))
                return false;
            machine_alert_button *b = &a->buttons[a->nbuttons++];
            b->item = (int16_t)i;
            gaddr title = tb_NewPtr(256, false, true);
            tb_GetControlTitle(h, title);
            uint8_t text[256];
            gget(text, title + 1, rd8(title));
            append_text(b->title, 0, sizeof b->title, text, rd8(title));
            tb_DisposePtr(title);
        } else if (type == statText) {
            if (!h || !rd32(h))
                continue;
            uint32_t len = (uint32_t)tb_GetHandleSize(h);
            uint8_t text[1024];
            if (len > sizeof text)
                len = sizeof text;
            gget(text, rd32(h), len);
            if (!len)
                continue;
            if (tn)
                tn = append_text(a->message, tn, sizeof a->message, (const uint8_t *)"\r\r", 2);
            tn = append_text(a->message, tn, sizeof a->message, text, len);
        } else if (type == iconItem) {
            int id = (int16_t)rd16(item_at(d, i) + 14);
            a->kind = id == 0 ? MACHINE_ALERT_STOP : id == 1 ? MACHINE_ALERT_NOTE
                    : id == 2 ? MACHINE_ALERT_CAUTION : MACHINE_ALERT_PLAIN;
        } else if (type == userItem) {
            /* A ring around a button, or nothing to draw. */
            bool ring = false;
            for (int k = 1; k <= n && !ring; k++) {
                qrect b = item_rect(d, k);
                ring = (item_type(d, k) & 0x7F) == (ctrlItem | btnCtrl) && r.left <= b.left && r.top <= b.top &&
                       r.right >= b.right && r.bottom >= b.bottom;
            }
            if (h && !ring)
                return false;
        } else if (type == 1) {
            continue; /* a help item */
        } else {
            return false;
        }
    }
    if (!tn || a->nbuttons < 1)
        return false;
    int16_t def = (int16_t)rd16(d + DialogRecord_aDefItem_off);
    a->default_item = def;
    for (int i = 0; i < a->nbuttons; i++)
        a->buttons[i].is_default = a->buttons[i].item == def;
    return true;
}

/* For a visible dialog box (not a modeless window) that is an alert in all
 * but name: the host shows it, the dialog out of sight meanwhile, and the
 * button chosen is the item hit. */
static bool host_alert_instead(gaddr d, gaddr itemp)
{
    if (!dialog_host_alerts()) /* headless: the dialog as it is */
        return false;
    int variant = tb_GetWVariant(d);
    if (!rd8(d + WindowRecord_visible_off) || !(variant == 1 || variant == 2 || variant == 3 || variant == 5))
        return false;
    machine_alert a;
    int chosen;
    if (!as_host_alert(d, &a))
        return false;
    tb_HideWindow(d);
    if (!dialog_host_alert(&a, &chosen)) {
        tb_ShowWindow(d);
        return false;
    }
    wr16(itemp, (uint16_t)(chosen > 0 ? chosen : a.default_item));
    return true;
}

/* The dialog a ModalDialog call is waiting in, for the menu walker. */
static gaddr in_modal;

bool dialog_modal(gaddr d) { return d && d == in_modal; }

void tb_ModalDialog(gaddr filter, gaddr itemp)
{
    gaddr d = tb_FrontWindow();
    if (!is_dialog(d)) {
        /* No dialog to take events (SnoodEdit's About box, whose DLOG it
         * doesn't have): time passes as it waits, so a program calling
         * again till an item is hit waits rather than spins. */
        gaddr evt = tb_NewPtr(EventRecord_SIZEOF, false, true);
        tb_WaitNextEvent(0, evt, 6, 0);
        tb_DisposePtr(evt);
        wr16(itemp, 0);
        return;
    }
    if (depth_request(d)) {
        wr16(itemp, 1);
        return;
    }
    if (host_alert_instead(d, itemp))
        return;
    gaddr evt = tb_NewPtr(EventRecord_SIZEOF, false, true);
    gaddr dp = tb_NewPtr(4, false, true);
    gaddr outer = in_modal;
    in_modal = d;
    int x = extra_index(d, false);
    while (!g_cpu->halted) {
        /* Every event but high-level ones, which wait for the application,
         * or those SetModalDialogEventMask asked for. */
        uint16_t mask = x >= 0 && extras[x].has_mask ? extras[x].event_mask : (uint16_t)(0xFFFF & ~0x0400);
        bool got = tb_WaitNextEvent((int16_t)mask, evt, 6, 0) != 0;
        if (x >= 0 && extras[x].timeout_secs) {
            int what = (int16_t)rd16(evt);
            if (got && (what == mouseDown || what == keyDown || what == autoKey)) /* the user is there: start again */
                extras[x].timeout_start = rd32(LM_Ticks);
            else if (rd32(LM_Ticks) - extras[x].timeout_start >= extras[x].timeout_secs * 60) {
                extras[x].timeout_secs = 0;
                flash_button(d, extras[x].timeout_item);
                wr16(itemp, (uint16_t)extras[x].timeout_item);
                break;
            }
        }
        if (filter) {
            cpu68k *c = g_cpu;
            cpu_push16(c, 0);
            cpu_push32(c, d);
            cpu_push32(c, evt);
            cpu_push32(c, itemp);
            guest_call(filter);
            if (cpu_pop16(c) & 0xFF00)
                break;
        } else if (std_filter(d, evt, itemp)) {
            break;
        }
        if (got && (int16_t)rd16(evt) == updateEvt && !is_dialog(rd32(evt + EventRecord_message_off))) {
            /* Nobody else will handle it during the dialog; don't spin on it. */
            tb_BeginUpdate(rd32(evt + EventRecord_message_off));
            tb_EndUpdate(rd32(evt + EventRecord_message_off));
            continue;
        }
        if (tb_IsDialogEvent(evt) && tb_DialogSelect(evt, dp, itemp) && rd32(dp) == d)
            break;
    }
    in_modal = outer;
    tb_DisposePtr(dp);
    tb_DisposePtr(evt);
}

/* ---------------------------------------------------------------------- */
/* Mac OS 8's additions */

enum { errItemNotControl = -30596 };

/* The item's control: a control item's own, or for the others a stand-in
 * whose data is the item's (static and edit text, icons, pictures, user
 * items as user panes). */
int16_t tb_GetDialogItemAsControl(gaddr d, int16_t n, gaddr out)
{
    if (out)
        wr32(out, 0);
    if (!is_dialog(d) || n < 1 || n > item_count(d))
        return errItemNotControl;
    int type = item_type(d, n) & 0x7F;
    gaddr ch = (type & ctrlItem) ? item_handle(d, n) : 0;
    if (!ch && type != 1) /* help items have none */
        ch = ap_item_control(d, n, type);
    if (!ch)
        return errItemNotControl;
    if (out)
        wr32(out, ch);
    return noErr;
}

/* Move an item to (h, v) (window coordinates), its control or text with it. */
int16_t tb_MoveDialogItem(gaddr d, int16_t n, int16_t h, int16_t v)
{
    if (!is_dialog(d) || n < 1 || n > item_count(d))
        return paramErr;
    qrect r = item_rect(d, n);
    bool hidden = r.left > 8192;
    if (hidden) {
        r.left -= 16384;
        r.right -= 16384;
    }
    qrect nr = { v, h, (int16_t)(v + r.bottom - r.top), (int16_t)(h + r.right - r.left) };
    if (hidden) {
        nr.left += 16384;
        nr.right += 16384;
    }
    qrect_put(item_at(d, n) + 4, nr);
    if ((item_type(d, n) & ctrlItem) && item_handle(d, n))
        tb_MoveControl(item_handle(d, n), h, v); /* (a hidden item's control is hidden, where it was) */
    if (n == edit_item(d) && edit_te(d)) {
        gaddr t = rd32(edit_te(d));
        qrect_put(t + TERec_destRect_off, nr);
        qrect_put(t + TERec_viewRect_off, nr);
        tb_TECalText(edit_te(d));
    }
    return noErr;
}

int16_t tb_SizeDialogItem(gaddr d, int16_t n, int16_t width, int16_t height)
{
    if (!is_dialog(d) || n < 1 || n > item_count(d))
        return paramErr;
    qrect r = item_rect(d, n);
    r.right = (int16_t)(r.left + width);
    r.bottom = (int16_t)(r.top + height);
    qrect_put(item_at(d, n) + 4, r);
    if ((item_type(d, n) & ctrlItem) && item_handle(d, n))
        tb_SizeControl(item_handle(d, n), width, height);
    if (n == edit_item(d) && edit_te(d)) {
        gaddr t = rd32(edit_te(d));
        qrect_put(t + TERec_destRect_off, r);
        qrect_put(t + TERec_viewRect_off, r);
        tb_TECalText(edit_te(d));
    }
    return noErr;
}

/* AutoSizeDialog: static text items grow to hold their text (wrapped in
 * their width), the items below move down, and the window grows. */
int16_t tb_AutoSizeDialog(gaddr d)
{
    if (!is_dialog(d))
        return paramErr;
    gaddr saved = qd_port();
    tb_SetPort(d);
    int n = item_count(d), grown = 0;
    gaddr fi = tb_NewPtr(8, false, false);
    for (int i = 1; i <= n; i++) {
        if ((item_type(d, i) & 0x7F) != statText)
            continue;
        qrect r = item_rect(d, i);
        if (r.left > 8192)
            continue;
        gaddr h = item_handle(d, i);
        uint32_t len = h && rd32(h) ? (uint32_t)tb_GetHandleSize(h) : 0;
        if (!len)
            continue;
        uint8_t *text = malloc(len);
        gget(text, rd32(h), len);
        item_font_saved fs;
        item_font_begin(d, i, &fs);
        tb_GetFontInfo(fi);
        int line = (int16_t)rd16(fi) + (int16_t)rd16(fi + 2) + (int16_t)rd16(fi + 6);
        /* Lines as TextBox breaks them: at returns, and at the last space that fits. */
        int lines = 1, width = r.right - r.left;
        uint32_t start = 0, space = 0;
        for (uint32_t k = 0; k < len; k++) {
            if (text[k] == '\r') {
                lines++;
                start = k + 1;
                space = 0;
                continue;
            }
            if (text[k] == ' ')
                space = k;
            if (qd_text_width(d, text + start, (int)(k + 1 - start)) > width) {
                lines++;
                start = space > start ? space + 1 : k;
                space = 0;
            }
        }
        item_font_end(d, &fs);
        free(text);
        int need = lines * line - (r.bottom - r.top);
        if (need <= 0)
            continue;
        qrect_put(item_at(d, i) + 4, (qrect){ r.top, r.left, (int16_t)(r.bottom + need), r.right });
        for (int k = 1; k <= n; k++) {
            qrect o = item_rect(d, k);
            if (k == i || o.top < r.bottom)
                continue;
            tb_MoveDialogItem(d, (int16_t)k, o.left > 8192 ? (int16_t)(o.left - 16384) : o.left, (int16_t)(o.top + need));
        }
        grown += need;
    }
    tb_DisposePtr(fi);
    if (grown) {
        qrect port = qrect_get(d + GrafPort_portRect_off);
        tb_SizeWindow(d, (int16_t)(port.right - port.left), (int16_t)(port.bottom - port.top + grown), true);
    }
    tb_SetPort(saved);
    return noErr;
}

int16_t tb_AppendDialogItemList(gaddr d, int16_t id, int16_t method)
{
    gaddr ditl = tb_GetResource((int32_t)fourcc("DITL"), id);
    if (!ditl || !rd32(ditl))
        return resNotFound;
    tb_AppendDITL(d, ditl, method);
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* CarbonLib's: the accessors, items inserted and removed, timeouts and
 * the events ModalDialog takes */

enum { dialogNoTimeoutErr = -5640, errItemNotFound = -30597 };

gaddr tb_GetDialogWindow(gaddr d) { return d; }
gaddr tb_GetDialogPort(gaddr d) { return d; }
gaddr tb_GetDialogFromWindow(gaddr w) { return w; }
gaddr tb_GetDialogTextEditHandle(gaddr d) { return d ? edit_te(d) : 0; }
int16_t tb_GetDialogKeyboardFocusItem(gaddr d) { return d ? (int16_t)edit_item(d) : 0; }
int16_t tb_GetDialogDefaultItem(gaddr d) { return d ? (int16_t)rd16(d + DialogRecord_aDefItem_off) : 0; }

int16_t tb_GetDialogCancelItem(gaddr d)
{
    int x = extra_index(d, false);
    return x >= 0 ? extras[x].cancel : 0;
}

void tb_SetPortDialogPort(gaddr d) { tb_SetPort(d); }

/* InsertDialogItem(dialog, afterItem, type, handle, box): a new item after
 * afterItem (0 for the first), with the handle given, or what its kind
 * makes (a control with no title, empty text). */
int32_t tb_InsertDialogItem(gaddr d, int16_t after, int16_t type, gaddr h, gaddr box)
{
    if (!is_dialog(d) || !box)
        return paramErr;
    int n = item_count(d);
    if (after < 0 || after > n)
        after = (int16_t)n;
    gaddr items = items_handle(d);
    uint32_t have = (uint32_t)tb_GetHandleSize(items);
    uint32_t at = after ? item_at(d, after) + ITEM_HEADER + ((rd8(item_at(d, after) + 13) + 1u) & ~1u) - rd32(items)
                        : 2;
    tb_SetHandleSize(items, (int32_t)(have + ITEM_HEADER));
    if (rd16(LM_MemErr))
        return memFullErr;
    gaddr p = rd32(items);
    gmemcpy(p + at + ITEM_HEADER, p + at, have - at);
    wr32(p + at, h);
    gmemcpy(p + at + 4, box, 8);
    wr8(p + at + 12, (uint8_t)type);
    wr8(p + at + 13, 0);
    wr16(p, (uint16_t)n); /* count - 1 */
    int item = after + 1;
    int edit = edit_item(d);
    if (edit >= item)
        wr16(d + DialogRecord_editField_off, (uint16_t)edit); /* one further on */
    ap_items_shifted(d, item, 1);
    int kind = type & 0x7F;
    if (!h && kind != userItem) {
        gaddr made = make_item(d, item);
        wr32(item_at(d, item), made);
    }
    if ((kind & ctrlItem) && h && ctl_window_root(d)) /* a control of the program's own goes in the root */
        tb_AutoEmbedControl(h, d);
    if (kind == editText)
        make_edit(d, item);
    if (WindowRecord_visible(d)) {
        qrect r = qrect_get(box);
        gaddr saved = qd_port();
        tb_SetPort(d);
        gaddr rr = tb_NewPtr(8, false, false);
        qrect_put(rr, r);
        tb_InvalRect(rr);
        tb_DisposePtr(rr);
        tb_SetPort(saved);
    }
    return noErr;
}

/* RemoveDialogItems(dialog, itemNo, amountToRemove, disposeItemData): the
 * items go; their controls and text with them when disposeItemData says. */
int32_t tb_RemoveDialogItems(gaddr d, int16_t first, int16_t count, int8_t dispose)
{
    int n = item_count(d);
    if (!is_dialog(d) || first < 1 || first > n || count < 1)
        return paramErr;
    if (first + count - 1 > n)
        count = (int16_t)(n - first + 1);
    int last = first + count - 1;
    int edit = edit_item(d);
    gaddr saved = qd_port();
    tb_SetPort(d);
    for (int i = first; i <= last; i++) {
        int type = item_type(d, i) & 0x7F;
        gaddr h = item_handle(d, i);
        gaddr rr = tb_NewPtr(8, false, false);
        qrect_put(rr, item_rect(d, i));
        tb_InvalRect(rr);
        tb_DisposePtr(rr);
        if (i == edit) { /* the TERec gives the item's text back */
            gaddr te = edit_te(d);
            if (te) {
                TERec_set_hText(rd32(te), tb_NewHandle(0, false, false));
                tb_TEDispose(te);
                wr32(d + DialogRecord_textH_off, 0);
            }
            wr16(d + DialogRecord_editField_off, (uint16_t)-1);
        }
        if ((type & ctrlItem) && h) {
            if (dispose)
                tb_DisposeControl(h);
            else
                tb_HideControl(h);
        } else if ((type == statText || type == editText) && h && dispose) {
            tb_DisposeHandle(h);
        }
    }
    tb_SetPort(saved);
    gaddr items = items_handle(d);
    gaddr from = item_at(d, first);
    gaddr to = item_at(d, last) + ITEM_HEADER + ((rd8(item_at(d, last) + 13) + 1u) & ~1u);
    uint32_t size = (uint32_t)tb_GetHandleSize(items), tail = rd32(items) + size - to;
    gmemcpy(from, to, tail);
    tb_SetHandleSize(items, (int32_t)(size - (to - from)));
    wr16(rd32(items), (uint16_t)(n - count - 1));
    if (edit > last)
        wr16(d + DialogRecord_editField_off, (uint16_t)(edit - count - 1));
    ap_items_shifted(d, last + 1, -count);
    if (!edit_te(d)) /* the next edit text item takes the TERec */
        for (int i = 1; i <= n - count; i++)
            if ((item_type(d, i) & 0x7F) == editText) {
                make_edit(d, i);
                break;
            }
    return noErr;
}

int32_t tb_SetDialogTimeout(gaddr d, int16_t item, uint32_t seconds)
{
    int x = extra_index(d, true);
    if (x < 0)
        return memFullErr;
    extras[x].timeout_item = item;
    extras[x].timeout_secs = seconds;
    extras[x].timeout_start = rd32(LM_Ticks);
    return noErr;
}

int32_t tb_GetDialogTimeout(gaddr d, gaddr item, gaddr seconds, gaddr remaining)
{
    int x = extra_index(d, false);
    if (x < 0 || !extras[x].timeout_secs)
        return dialogNoTimeoutErr;
    uint32_t elapsed = (rd32(LM_Ticks) - extras[x].timeout_start) / 60;
    if (item)
        wr16(item, (uint16_t)extras[x].timeout_item);
    if (seconds)
        wr32(seconds, extras[x].timeout_secs);
    if (remaining)
        wr32(remaining, elapsed < extras[x].timeout_secs ? extras[x].timeout_secs - elapsed : 0);
    return noErr;
}

int32_t tb_SetModalDialogEventMask(gaddr d, uint16_t mask)
{
    int x = extra_index(d, true);
    if (x < 0)
        return memFullErr;
    extras[x].event_mask = mask;
    extras[x].has_mask = true;
    return noErr;
}

int32_t tb_GetModalDialogEventMask(gaddr d, gaddr mask)
{
    int x = extra_index(d, false);
    if (mask)
        wr16(mask, x >= 0 && extras[x].has_mask ? extras[x].event_mask : (uint16_t)(0xFFFF & ~0x0400));
    return noErr;
}

/* The front window if it is a dialog (the menu walker dismisses it), or 0. */
gaddr dialog_in_front(void)
{
    gaddr w = tb_FrontWindow();
    return is_dialog(w) ? w : 0;
}

/* What tells the walker one dialog from another: its size and its items'
 * kinds and places, the same when a program puts the dialog up anew (a
 * registration notice asking again after a wrong code). */
uint32_t dialog_signature(gaddr d)
{
    qrect pr = qrect_get(d + GrafPort_portRect_off);
    uint32_t h = 2166136261u;
    int16_t v[5];
    int n = item_count(d);
    for (int i = 0; i <= n; i++) {
        qrect r = i ? item_rect(d, i) : pr;
        v[0] = (int16_t)(i ? item_type(d, i) & 0x7F : n);
        v[1] = r.top, v[2] = r.left, v[3] = r.bottom, v[4] = r.right;
        for (int k = 0; k < 5; k++)
            h = (h ^ (uint16_t)v[k]) * 16777619u;
    }
    return h;
}

/* Whether item n is a push button a click would press now (2: shown,
 * active and enabled), one that may become so (1: dimmed, as a nag's "Not
 * Yet" while it counts down), or neither (0); -1 past the last item.  For
 * a button, its title and its centre on the screen. */
int dialog_button(gaddr d, int n, char *title, size_t size, int *x, int *y)
{
    if (n > item_count(d))
        return -1;
    int type = item_type(d, n);
    gaddr h = item_handle(d, n);
    qrect r = item_rect(d, n);
    if ((type & 0x7F) != (ctrlItem | btnCtrl) || !h || !rd32(h) || r.left >= 8192 || !ControlRecord_contrlVis(rd32(h)))
        return 0;
    gaddr t = tb_NewPtr(256, false, true);
    tb_GetControlTitle(h, t);
    uint8_t text[256];
    gget(text, t + 1, rd8(t));
    title[0] = 0;
    append_text(title, 0, size, text, rd8(t));
    tb_DisposePtr(t);
    pixdst px = qd_port_pixels(d);
    *x = (r.left + r.right) / 2 - px.bounds.left;
    *y = (r.top + r.bottom) / 2 - px.bounds.top;
    return (type & itemDisable) || ControlRecord_contrlHilite(rd32(h)) == 255 ? 1 : 2;
}
