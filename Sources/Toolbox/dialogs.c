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

/* Per-dialog settings from SetDialogDefaultItem and friends. */
#define MAX_EXTRAS 32
typedef struct {
    gaddr dialog;
    int16_t cancel;
    bool ring; /* draw the default button's outline */
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
            extras[i] = (dialog_extra){ d, 0, false };
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
static bool is_dialog(gaddr w) { return w && WindowRecord_windowKind(w) == dialogKind; }

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
static void activate_edit(gaddr d, int n, int start, int end)
{
    gaddr te = edit_te(d);
    if (!te)
        return;
    gaddr saved = qd_port();
    tb_SetPort(d);
    tb_TEDeactivate(te);
    qrect r = item_rect(d, n);
    gaddr t = rd32(te);
    qrect_put(t + TERec_destRect_off, r);
    qrect_put(t + TERec_viewRect_off, r);
    TERec_set_hText(t, item_handle(d, n));
    tb_TECalText(te);
    wr16(d + DialogRecord_editField_off, (uint16_t)(n - 1));
    tb_TESetSelect(start, end, te);
    if (WindowRecord_hilited(d))
        tb_TEActivate(te);
    tb_SetPort(saved);
}

/* ---------------------------------------------------------------------- */
/* Creating and disposing */

static void make_items(gaddr d)
{
    int n = item_count(d);
    int first_edit = 0;
    for (int i = 1; i <= n; i++) {
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
            if (type == editText && !first_edit)
                first_edit = i;
        } else if (type == iconItem) {
            made = tb_GetResource((int32_t)fourcc("ICON"), (int16_t)rd16(item_at(d, i) + 14));
        } else if (type == picItem) {
            made = tb_GetPicture((int16_t)rd16(item_at(d, i) + 14));
        }
        wr32(item_at(d, i), made); /* user items keep their (nil) procedure */
    }
    wr16(d + DialogRecord_editField_off, (uint16_t)-1);
    wr32(d + DialogRecord_textH_off, 0);
    if (first_edit) {
        gaddr r = tb_NewPtr(8, false, false);
        qrect_put(r, item_rect(d, first_edit));
        gaddr saved = qd_port();
        tb_SetPort(d);
        gaddr te = tb_TENew(r, r);
        tb_SetPort(saved);
        tb_DisposePtr(r);
        tb_DisposeHandle(TERec_hText(rd32(te)));
        wr32(d + DialogRecord_textH_off, te);
        activate_edit(d, first_edit, 0, 32767);
    }
}

gaddr tb_NewDialog(gaddr dst, gaddr r, gaddr tit, int8_t vis, int16_t procid, gaddr behind, int8_t gaflag, int32_t rc,
                   gaddr items)
{
    gaddr d = dst ? dst : tb_NewPtr(DialogRecord_SIZEOF, false, true);
    if (!d)
        return 0;
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
    make_items(d);
    if (vis)
        tb_ShowWindow(d);
    return d;
}

gaddr tb_NewColorDialog(gaddr dst, gaddr r, gaddr tit, int8_t vis, int16_t procid, gaddr behind, int8_t gaflag,
                        int32_t rc, gaddr items)
{
    return tb_NewDialog(dst, r, tit, vis, procid, behind, gaflag, rc, items);
}

/* DLOG: bounds(8), procID(2), visible(1), pad, goAway(1), pad, refCon(4), itemsID(2), title. */
gaddr dialog_from_resource(int16_t id, gaddr dst, gaddr behind, int vis_override, int32_t refcon_override,
                           bool use_refcon)
{
    gaddr dlog = tb_GetResource((int32_t)fourcc("DLOG"), id);
    if (!dlog || !rd32(dlog))
        return 0;
    gaddr ditl = tb_GetResource((int32_t)fourcc("DITL"), (int16_t)rd16(rd32(dlog) + 18));
    if (!ditl || !rd32(ditl))
        return 0;
    uint32_t items = ditl;
    tb_HandToHand(&items);
    tb_HLock(dlog);
    gaddr p = rd32(dlog);
    int vis = vis_override >= 0 ? vis_override : rd8(p + 10);
    int32_t rc = use_refcon ? refcon_override : (int32_t)rd32(p + 14);
    gaddr d = tb_NewDialog(dst, p, p + 20, (int8_t)vis, (int16_t)rd16(p + 8), behind, (int8_t)rd8(p + 12), rc, items);
    tb_HUnlock(dlog);
    return d;
}

gaddr tb_GetNewDialog(int16_t id, gaddr dst, gaddr behind) { return dialog_from_resource(id, dst, behind, -1, 0, false); }

void tb_CloseDialog(gaddr d)
{
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
    if (x >= 0)
        extras[x].dialog = 0;
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

void tb_GetDialogItemText(gaddr item, gaddr text)
{
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

static void draw_item(gaddr d, int n)
{
    int type = item_type(d, n) & 0x7F;
    qrect r = item_rect(d, n);
    if (r.left > 8192)
        return; /* hidden */
    gaddr h = item_handle(d, n);
    gaddr rp = tb_NewPtr(8, false, false);
    qrect_put(rp, r);
    if (type & ctrlItem) {
        if (h)
            tb_Draw1Control(h);
    } else if (type == statText) {
        draw_text_item(h, r, true);
    } else if (type == editText) {
        qrect f = { (int16_t)(r.top - 3), (int16_t)(r.left - 3), (int16_t)(r.bottom + 3), (int16_t)(r.right + 3) };
        gaddr fp = tb_NewPtr(8, false, false);
        qrect_put(fp, f);
        tb_PenNormal();
        tb_FrameRect(fp);
        tb_DisposePtr(fp);
        if (n == edit_item(d) && edit_te(d))
            tb_TEUpdate(rp, edit_te(d));
        else
            draw_text_item(h, r, false);
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
    tb_DisposePtr(rp);
}

static void draw_default_ring(gaddr d)
{
    int x = extra_index(d, false);
    if (x < 0 || !extras[x].ring)
        return;
    int n = (int16_t)rd16(d + DialogRecord_aDefItem_off);
    if (n < 1 || n > item_count(d) || !(item_type(d, n) & ctrlItem))
        return;
    qrect r = item_rect(d, n);
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
    int n = item_count(d);
    for (int i = 1; i <= n; i++)
        if (!(item_type(d, i) & ctrlItem))
            draw_item(d, i);
    tb_DrawControls(d);
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
        return is_dialog(rd32(evt + EventRecord_message_off));
    gaddr front = tb_FrontWindow();
    if (!is_dialog(front))
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

int8_t tb_DialogSelect(gaddr evt, gaddr dpp, gaddr itemp)
{
    int what = (int16_t)rd16(evt);
    gaddr d = (what == updateEvt || what == activateEvt) ? rd32(evt + EventRecord_message_off) : tb_FrontWindow();
    if (!is_dialog(d))
        return 0;
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
    case activateEvt:
        if (te) {
            if (rd16(evt + EventRecord_modifiers_off) & 1)
                tb_TEActivate(te);
            else
                tb_TEDeactivate(te);
        }
        break;
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
            if (tb_TrackControl(ch, p, 0))
                n = item_for_handle(d, ch);
        } else {
            n = tb_FindDialogItem(d, p) + 1;
            if (n > 0 && (item_type(d, n) & 0x7F) == editText && te) {
                if (n != edit_item(d))
                    activate_edit(d, n, 0, 0);
                tb_TEClick(p, (rd16(evt + EventRecord_modifiers_off) & 0x0200) != 0, te);
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
        tb_TEKey((int16_t)c, te);
        int n = edit_item(d);
        if (item_enabled(d, n)) {
            wr16(itemp, (uint16_t)n);
            result = 1;
        }
        break;
    }
    default:
        if (te)
            tb_TEIdle(te);
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

void tb_ModalDialog(gaddr filter, gaddr itemp)
{
    gaddr d = tb_FrontWindow();
    if (!is_dialog(d)) {
        wr16(itemp, 0);
        return;
    }
    gaddr evt = tb_NewPtr(EventRecord_SIZEOF, false, true);
    gaddr dp = tb_NewPtr(4, false, true);
    while (!g_cpu->halted) {
        bool got = tb_WaitNextEvent(-1, evt, 6, 0) != 0;
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
    tb_DisposePtr(dp);
    tb_DisposePtr(evt);
}
