/*
 * Window Manager.
 *
 * Windows are kept front to back in WindowList.  Structure and content
 * regions are global; visRgn is recomputed for every window whenever the
 * arrangement changes.  Frames are drawn by the window's WDEF (ours are
 * native, reached through 68k stubs in the System file) into the Window
 * Manager port; exposed content is added to updateRgn for update events.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qd_internal.h"
#include "machine.h"
#include "tb_internal.h"

enum { wDraw = 0, wHit, wCalcRgns, wNew, wDispose, wGrow, wDrawGIcon };
enum { wNoHit = 0, wInContent, wInDrag, wInGrow, wInGoAway, wInZoomIn, wInZoomOut };
enum { inDesk = 0, inMenuBar, inSysWindow, inContent, inDrag, inGrow, inGoAway, inZoomIn, inZoomOut };
enum { userKind = 8, dialogKind = 2 };
enum { patCopy = 8, patXor = 10 };

#define IN_FRONT 0xFFFFFFFFu
#define TITLE_HEIGHT 18

/* Variation codes, keyed by window (32-bit mode keeps them out of the handle). */
static struct {
    gaddr w;
    int16_t var;
} varcodes[256];

static int16_t varcode(gaddr w)
{
    for (int i = 0; i < 256; i++)
        if (varcodes[i].w == w)
            return varcodes[i].var;
    return 0;
}

static void set_varcode(gaddr w, int16_t v)
{
    for (int i = 0; i < 256; i++)
        if (varcodes[i].w == w || varcodes[i].w == 0) {
            varcodes[i].w = w;
            varcodes[i].var = v;
            return;
        }
}

static void forget_varcode(gaddr w)
{
    for (int i = 0; i < 256; i++)
        if (varcodes[i].w == w)
            varcodes[i].w = 0;
}

static gaddr wlist(void) { return rd32(LM_WindowList); }
static gaddr wmgr_port(void) { return rd32(LM_WMgrPort); }
static bool wvisible(gaddr w) { return WindowRecord_visible(w) != 0; }

/* Global offset of a window port: local = global + (dh, dv). */
static void port_offset(gaddr w, int *dh, int *dv)
{
    pixdst d = qd_port_pixels(w);
    *dh = d.bounds.left;
    *dv = d.bounds.top;
}

/* Content rectangle in global coordinates. */
static qrect global_content(gaddr w)
{
    qrect r = qrect_get(w + GrafPort_portRect_off);
    int dh, dv;
    port_offset(w, &dh, &dv);
    return (qrect){ (int16_t)(r.top - dv), (int16_t)(r.left - dh), (int16_t)(r.bottom - dv), (int16_t)(r.right - dh) };
}

/* ---------------------------------------------------------------------- */
/* Calling window definition procedures */

static int32_t call_wdef(gaddr w, int16_t msg, int32_t param)
{
    gaddr h = WindowRecord_windowDefProc(w);
    if (!h || !rd32(h))
        return 0;
    cpu68k *c = g_cpu;
    cpu_push32(c, 0);              /* result */
    cpu_push16(c, (uint16_t)varcode(w));
    cpu_push32(c, w);
    cpu_push16(c, (uint16_t)msg);
    cpu_push32(c, (uint32_t)param);
    tb_HLock(h);
    guest_call(rd32(h));
    tb_HUnlock(h);
    return (int32_t)cpu_pop32(c);
}

/* ---------------------------------------------------------------------- */
/* The standard WDEF (document windows and dialog boxes) */

static void set_rect(gaddr r, int top, int left, int bottom, int right)
{
    qrect_put(r, (qrect){ (int16_t)top, (int16_t)left, (int16_t)bottom, (int16_t)right });
}

static bool is_dialog_variant(int var) { return var == 1 || var == 2 || var == 3; }
static bool has_zoom(int var) { return var == 8 || var == 12; }
static bool has_grow(int var) { return var == 0 || var == 8; }

/* The native appearance: document windows' frames are the host's. */
static bool native_frame(int var)
{
    return g_appearance == MACHINE_APPEARANCE_NATIVE && (var == 0 || var == 4 || var == 8 || var == 12);
}

/* ---- Actions from the host's window frames ---- */

static pthread_mutex_t action_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    gaddr w;
    int action, a, b;
    bool waiting;  /* posted; the next FindWindow takes it */
    bool taken;    /* FindWindow reported it; the Track/Drag/Grow call completes it */
} pending;

void ev_post_click_at(int x, int y);

void machine_window_action(uint32_t id, int action, int a, int b, int x, int y)
{
    pthread_mutex_lock(&action_lock);
    pending.w = id;
    pending.action = action;
    pending.a = a;
    pending.b = b;
    pending.waiting = true;
    pending.taken = false;
    pthread_mutex_unlock(&action_lock);
    if (getenv("CLASSIC_DEBUG_HOST"))
        fprintf(stderr, "window action %d on %08X (%d, %d) click %d,%d\n", action, id, a, b, x, y);
    ev_post_click_at(x, y);
}

/* If a host action is pending for FindWindow, its part (else 0). */
static int16_t take_action(gaddr wpp)
{
    pthread_mutex_lock(&action_lock);
    int16_t part = 0;
    if (pending.waiting) {
        pending.waiting = false;
        pending.taken = true;
        wr32(wpp, pending.w);
        switch (pending.action) {
        case MACHINE_ACTION_CLOSE: part = 6; break;   /* inGoAway */
        case MACHINE_ACTION_ZOOM: part = 8; break;    /* inZoomOut */
        case MACHINE_ACTION_MOVE: part = 4; break;    /* inDrag */
        case MACHINE_ACTION_RESIZE: part = 5; break;  /* inGrow */
        default: pending.taken = false; break;
        }
    }
    pthread_mutex_unlock(&action_lock);
    return part;
}

/* The pending action for this window and kind, consumed; false if none. */
static bool complete_action(gaddr w, int action, int *a, int *b)
{
    pthread_mutex_lock(&action_lock);
    bool ok = pending.taken && pending.w == w && pending.action == action;
    if (ok) {
        pending.taken = false;
        if (a)
            *a = pending.a;
        if (b)
            *b = pending.b;
    }
    pthread_mutex_unlock(&action_lock);
    return ok;
}

/* Snapshot flags for the host. */
uint8_t wm_window_flags(gaddr w)
{
    int var = varcode(w);
    if (!native_frame(var))
        return 0;
    uint8_t f = MACHINE_WINDOW_NATIVE;
    if (WindowRecord_goAwayFlag(w))
        f |= MACHINE_WINDOW_CLOSE;
    if (has_zoom(var))
        f |= MACHINE_WINDOW_ZOOM;
    if (has_grow(var))
        f |= MACHINE_WINDOW_GROW;
    return f;
}

static void title_rect(qrect c, qrect *t) { *t = (qrect){ (int16_t)(c.top - TITLE_HEIGHT), c.left, (int16_t)(c.top - 1), c.right }; }

static void wdef_calc(gaddr w, int var)
{
    qrect c = global_content(w);
    hrgn s, k;
    hrgn_init(&s);
    hrgn_init(&k);
    hrgn_rect(&k, c);
    if (native_frame(var)) {
        hrgn_rect(&s, c); /* the host draws the frame */
    } else if (theme_window_region(w, var, c, &s)) {
        /* the theme's frame shape */
    } else if (var == 1) { /* dBoxProc: 8-pixel border */
        hrgn_rect(&s, (qrect){ (int16_t)(c.top - 8), (int16_t)(c.left - 8), (int16_t)(c.bottom + 8), (int16_t)(c.right + 8) });
    } else if (var == 2) { /* plainDBox */
        hrgn_rect(&s, (qrect){ (int16_t)(c.top - 1), (int16_t)(c.left - 1), (int16_t)(c.bottom + 1), (int16_t)(c.right + 1) });
    } else if (var == 3) { /* altDBoxProc: frame plus 2-pixel shadow */
        hrgn_rect(&s, (qrect){ (int16_t)(c.top - 1), (int16_t)(c.left - 1), (int16_t)(c.bottom + 3), (int16_t)(c.right + 3) });
    } else { /* documents: frame, title bar, 1-pixel shadow */
        hrgn_rect(&s, (qrect){ (int16_t)(c.top - TITLE_HEIGHT - 1), (int16_t)(c.left - 1), (int16_t)(c.bottom + 2),
                               (int16_t)(c.right + 2) });
    }
    rgn_write(WindowRecord_strucRgn(w), &s);
    rgn_write(WindowRecord_contRgn(w), &k);
    hrgn_free(&s);
    hrgn_free(&k);
}

/* Draw the title text centred in the title bar of the current port. */
static void draw_title(gaddr w, qrect t, bool clear_behind)
{
    gaddr th = WindowRecord_titleHandle(w);
    if (!th || !rd32(th))
        return;
    gaddr port = qd_port();
    GrafPort_set_txFont(port, 0);
    GrafPort_set_txSize(port, 12);
    GrafPort_set_txFace(port, 0);
    GrafPort_set_txMode(port, 1);
    gaddr s = rd32(th);
    int n = rd8(s);
    uint8_t buf[256];
    gget(buf, s + 1, (uint32_t)n);
    int tw = qd_text_width(port, buf, n);
    int avail = t.right - t.left - 64;
    if (tw > avail)
        tw = avail;
    int x = t.left + (t.right - t.left - tw) / 2;
    if (clear_behind) {
        gaddr r = tb_NewPtr(8, false, false);
        set_rect(r, t.top + 1, x - 6, t.bottom, x + tw + 6);
        tb_EraseRect(r);
        tb_DisposePtr(r);
    }
    tb_MoveTo((int16_t)x, (int16_t)(t.top + 13));
    tb_DrawText(s + 1, 0, (int16_t)n);
}

static void wdef_draw(gaddr w, int var, int32_t param)
{
    if (!wvisible(w))
        return;
    qrect c = global_content(w);
    bool hilited = WindowRecord_hilited(w) != 0;
    if (native_frame(var) ||
        theme_draw_window(w, var, c, hilited, WindowRecord_goAwayFlag(w) != 0, has_zoom(var), param))
        return;
    gaddr r = tb_NewPtr(8, false, false);
    tb_PenNormal();
    if (is_dialog_variant(var)) {
        if (var == 1) {
            set_rect(r, c.top - 8, c.left - 8, c.bottom + 8, c.right + 8);
            tb_FrameRect(r);
            set_rect(r, c.top - 7, c.left - 7, c.bottom + 7, c.right + 7);
            tb_EraseRect(r);
            set_rect(r, c.top - 4, c.left - 4, c.bottom + 4, c.right + 4);
            tb_PenSize(2, 2);
            tb_FrameRect(r);
            tb_PenNormal();
        } else {
            set_rect(r, c.top - 1, c.left - 1, c.bottom + 1, c.right + 1);
            tb_FrameRect(r);
            if (var == 3) {
                set_rect(r, c.bottom + 1, c.left + 1, c.bottom + 3, c.right + 3);
                tb_PaintRect(r);
                set_rect(r, c.top + 1, c.right + 1, c.bottom + 3, c.right + 3);
                tb_PaintRect(r);
            }
        }
        tb_DisposePtr(r);
        return;
    }
    qrect t;
    title_rect(c, &t);
    if (param == wInGoAway || param == wInZoomIn || param == wInZoomOut) {
        /* Toggle a pressed box: System 7 draws a starburst inside it, here
         * in XOR so a second call restores the box. */
        int left = param == wInGoAway ? t.left + 9 : t.right - 19;
        int top = t.top + 4;
        tb_PenMode(patXor);
        static const int spokes[8][4] = {
            { 0, 0, 2, 2 }, { 4, 0, 4, 2 }, { 8, 0, 6, 2 }, { 0, 4, 2, 4 },
            { 8, 4, 6, 4 }, { 0, 8, 2, 6 }, { 4, 8, 4, 6 }, { 8, 8, 6, 6 },
        };
        for (int i = 0; i < 8; i++) {
            tb_MoveTo((int16_t)(left + spokes[i][0]), (int16_t)(top + spokes[i][1]));
            tb_LineTo((int16_t)(left + spokes[i][2]), (int16_t)(top + spokes[i][3]));
        }
        tb_PenNormal();
        tb_DisposePtr(r);
        return;
    }
    /* Frame, title bar separator and shadow. */
    set_rect(r, c.top - TITLE_HEIGHT - 1, c.left - 1, c.bottom + 1, c.right + 1);
    tb_FrameRect(r);
    set_rect(r, t.top, t.left, t.bottom, t.right);
    tb_EraseRect(r);
    tb_MoveTo(c.left, (int16_t)(c.top - 1));
    tb_LineTo((int16_t)(c.right - 1), (int16_t)(c.top - 1));
    tb_MoveTo((int16_t)(c.right + 1), (int16_t)(c.top - TITLE_HEIGHT));
    tb_LineTo((int16_t)(c.right + 1), (int16_t)(c.bottom + 1));
    tb_LineTo((int16_t)(c.left - TITLE_HEIGHT + TITLE_HEIGHT), (int16_t)(c.bottom + 1));
    if (hilited) {
        /* Racing stripes. */
        for (int y = t.top + 3; y <= t.top + 13; y += 2) {
            tb_MoveTo((int16_t)(t.left + 1), (int16_t)y);
            tb_LineTo((int16_t)(t.right - 2), (int16_t)y);
        }
        if (WindowRecord_goAwayFlag(w)) {
            set_rect(r, t.top + 2, t.left + 7, t.top + 15, t.left + 20);
            tb_EraseRect(r);
            set_rect(r, t.top + 3, t.left + 8, t.top + 14, t.left + 19);
            tb_FrameRect(r);
        }
        if (has_zoom(var)) {
            set_rect(r, t.top + 2, t.right - 21, t.top + 15, t.right - 8);
            tb_EraseRect(r);
            set_rect(r, t.top + 3, t.right - 20, t.top + 14, t.right - 9);
            tb_FrameRect(r);
            set_rect(r, t.top + 3, t.right - 20, t.top + 9, t.right - 13);
            tb_FrameRect(r);
        }
    }
    draw_title(w, t, hilited);
    tb_DisposePtr(r);
    theme_decorate_window(w, var, c, hilited, WindowRecord_goAwayFlag(w) != 0, has_zoom(var));
}

static int32_t wdef_hit(gaddr w, int var, uint32_t pt)
{
    qrect c = global_content(w);
    int h = pt_h(pt), v = pt_v(pt);
    if (h >= c.left && h < c.right && v >= c.top && v < c.bottom) {
        if (has_grow(var) && WindowRecord_hilited(w) && h >= c.right - 15 && v >= c.bottom - 15)
            return wInGrow;
        return wInContent;
    }
    int part = theme_active() ? theme_window_hit(w, var, c, h, v) : -1;
    if (part >= 0) {
        bool hil = WindowRecord_hilited(w) != 0;
        if (part == 1 && hil && WindowRecord_goAwayFlag(w))
            return wInGoAway;
        if (part == 2 && hil && has_zoom(var))
            return wInZoomIn;
        if (part == 0)
            return wNoHit;
        return is_dialog_variant(var) ? wNoHit : wInDrag; /* the frame drags documents and movable dialogs */
    }
    if (is_dialog_variant(var))
        return wNoHit;
    qrect t;
    title_rect(c, &t);
    if (v >= t.top - 1 && v < c.top && h >= c.left - 1 && h <= c.right) {
        if (WindowRecord_hilited(w)) {
            if (WindowRecord_goAwayFlag(w) && h >= t.left + 8 && h < t.left + 19 && v >= t.top + 3 && v < t.top + 14)
                return wInGoAway;
            if (has_zoom(var) && h >= t.right - 20 && h < t.right - 9 && v >= t.top + 3 && v < t.top + 14)
                return wInZoomIn;
        }
        return wInDrag;
    }
    return wNoHit;
}

static void wdef_grow_icon(gaddr w, int var)
{
    if (!has_grow(var) || !wvisible(w))
        return;
    qrect c = qrect_get(w + GrafPort_portRect_off);
    if (native_frame(var) || theme_draw_grow(c, WindowRecord_hilited(w) != 0))
        return;
    gaddr r = tb_NewPtr(8, false, false);
    tb_PenNormal();
    tb_MoveTo((int16_t)(c.right - 16), c.top);
    tb_LineTo((int16_t)(c.right - 16), (int16_t)(c.bottom - 1));
    tb_MoveTo(c.left, (int16_t)(c.bottom - 16));
    tb_LineTo((int16_t)(c.right - 1), (int16_t)(c.bottom - 16));
    set_rect(r, c.bottom - 15, c.right - 15, c.bottom, c.right);
    tb_EraseRect(r);
    if (WindowRecord_hilited(w)) {
        set_rect(r, c.bottom - 12, c.right - 12, c.bottom - 3, c.right - 3);
        tb_FrameRect(r);
        set_rect(r, c.bottom - 12, c.right - 12, c.bottom - 6, c.right - 6);
        tb_EraseRect(r);
        tb_FrameRect(r);
    }
    tb_DisposePtr(r);
}

/* WDEF 0: Pascal FUNCTION (varCode, theWindow, message, param): LONGINT. */
void wdef0_native(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    int32_t param = (int32_t)cpu_pop32(c);
    int16_t msg = (int16_t)cpu_pop16(c);
    gaddr w = cpu_pop32(c);
    int16_t var = (int16_t)cpu_pop16(c);
    int32_t result = 0;
    switch (msg) {
    case wDraw: wdef_draw(w, var, param); break;
    case wHit: result = wdef_hit(w, var, (uint32_t)param); break;
    case wCalcRgns: wdef_calc(w, var); break;
    case wDrawGIcon: wdef_grow_icon(w, var); break;
    default: break;
    }
    wr32(c->a[7], (uint32_t)result);
    cpu_push32(c, ret);
}

/* ---------------------------------------------------------------------- */
/* Visibility and painting */

static void gray_rgn(hrgn *out) { rgn_read(rd32(LM_GrayRgn), out); }

void display_windows_changed(void);

/* Recompute every window's visRgn (local coordinates). */
static void calc_all_vis(void)
{
    display_windows_changed();
    hrgn covered, vis, tmp, gray;
    hrgn_init(&covered);
    hrgn_init(&vis);
    hrgn_init(&tmp);
    hrgn_init(&gray);
    gray_rgn(&gray);
    for (gaddr w = wlist(); w; w = WindowRecord_nextWindow(w)) {
        if (!wvisible(w)) {
            hrgn_rect(&vis, (qrect){ 0, 0, 0, 0 });
        } else {
            rgn_read(WindowRecord_contRgn(w), &vis);
            hrgn_op(&vis, &vis, &gray, RGN_SECT);
            hrgn_op(&vis, &vis, &covered, RGN_DIFF);
            rgn_read(WindowRecord_strucRgn(w), &tmp);
            hrgn_op(&covered, &covered, &tmp, RGN_UNION);
            int dh, dv;
            port_offset(w, &dh, &dv);
            hrgn_offset(&vis, dh, dv);
        }
        rgn_write(GrafPort_visRgn(w), &vis);
    }
    hrgn_free(&covered);
    hrgn_free(&vis);
    hrgn_free(&tmp);
    hrgn_free(&gray);
}

/* Draw a window's frame through its WDEF, clipped to `area` (global). */
static void draw_frame(gaddr w, const hrgn *area)
{
    gaddr saved = qd_port();
    gaddr wp = wmgr_port();
    tb_SetPort(wp);
    rgn_write(GrafPort_clipRgn(wp), area);
    call_wdef(w, wDraw, 0);
    tb_SetRectRgn(GrafPort_clipRgn(wp), -32767, -32767, 32767, 32767);
    tb_SetPort(saved);
}

static void paint_desk(const hrgn *area)
{
    gaddr saved = qd_port();
    gaddr wp = wmgr_port();
    tb_SetPort(wp);
    hrgn r;
    hrgn_init(&r);
    hrgn_copy(&r, area);
    uint8_t pat[8];
    gget(pat, LM_DeskPattern, 8);
    qd_fill_region(wp, &r, pat, patCopy);
    hrgn_free(&r);
    tb_SetPort(saved);
}

/* Repaint everything inside `clobbered` (global): frames, desk, and add
 * exposed content to update regions. */
static void repaint(const hrgn *clobbered)
{
    calc_all_vis();
    hrgn covered, area, tmp, gray;
    hrgn_init(&covered);
    hrgn_init(&area);
    hrgn_init(&tmp);
    hrgn_init(&gray);
    gray_rgn(&gray);
    for (gaddr w = wlist(); w; w = WindowRecord_nextWindow(w)) {
        if (!wvisible(w))
            continue;
        rgn_read(WindowRecord_strucRgn(w), &tmp);
        hrgn_op(&area, clobbered, &tmp, RGN_SECT);
        hrgn_op(&area, &area, &covered, RGN_DIFF);
        hrgn_op(&area, &area, &gray, RGN_SECT);
        if (!hrgn_empty(&area)) {
            /* Content: erase and queue an update (unless the program
             * cleared PaintWhite or SaveUpdate to ask for neither). */
            hrgn cont;
            hrgn_init(&cont);
            rgn_read(WindowRecord_contRgn(w), &cont);
            hrgn_op(&cont, &cont, &area, RGN_SECT);
            if (!hrgn_empty(&cont) && rd16(LM_SaveUpdate)) {
                hrgn upd;
                hrgn_init(&upd);
                rgn_read(WindowRecord_updateRgn(w), &upd);
                hrgn_op(&upd, &upd, &cont, RGN_UNION);
                rgn_write(WindowRecord_updateRgn(w), &upd);
                hrgn_free(&upd);
            }
            if (!hrgn_empty(&cont) && rd16(LM_PaintWhite)) {
                gaddr saved = qd_port();
                tb_SetPort(w);
                int dh, dv;
                port_offset(w, &dh, &dv);
                hrgn_offset(&cont, dh, dv);
                uint8_t bk[8];
                qd_port_pattern(w, 1, bk);
                /* The Window Manager erases what was uncovered whatever
                 * clip the program has left set, which may leave some of
                 * its window out; only the visRgn limits it. */
                hrgn clip, open;
                hrgn_init(&clip);
                hrgn_init(&open);
                rgn_read(GrafPort_clipRgn(w), &clip);
                hrgn_rect(&open, (qrect){ -32767, -32767, 32767, 32767 });
                rgn_write(GrafPort_clipRgn(w), &open);
                qd_fill_region(w, &cont, bk, patCopy);
                rgn_write(GrafPort_clipRgn(w), &clip);
                hrgn_free(&clip);
                hrgn_free(&open);
                tb_SetPort(saved);
            }
            hrgn_free(&cont);
            draw_frame(w, &area);
        }
        hrgn_op(&covered, &covered, &tmp, RGN_UNION);
    }
    hrgn_op(&area, clobbered, &gray, RGN_SECT);
    hrgn_op(&area, &area, &covered, RGN_DIFF);
    if (!hrgn_empty(&area))
        paint_desk(&area);
    hrgn_free(&covered);
    hrgn_free(&area);
    hrgn_free(&tmp);
    hrgn_free(&gray);
}

static void repaint_rgn_of(gaddr w)
{
    hrgn s;
    hrgn_init(&s);
    rgn_read(WindowRecord_strucRgn(w), &s);
    repaint(&s);
    hrgn_free(&s);
}

static void repaint_all(void)
{
    hrgn all;
    hrgn_init(&all);
    gray_rgn(&all);
    repaint(&all);
    hrgn_free(&all);
}

/* ---------------------------------------------------------------------- */
/* Activation */

static gaddr first_visible(void)
{
    /* GhostWindow (a floating window) is never the front one. */
    gaddr ghost = rd32(LM_GhostWindow);
    for (gaddr w = wlist(); w; w = WindowRecord_nextWindow(w))
        if (wvisible(w) && w != ghost)
            return w;
    return 0;
}

static void set_hilite(gaddr w, bool on)
{
    if (!w || (WindowRecord_hilited(w) != 0) == on)
        return;
    WindowRecord_set_hilited(w, on ? 1 : 0);
    display_windows_changed();
    if (wvisible(w)) {
        hrgn s, c;
        hrgn_init(&s);
        hrgn_init(&c);
        rgn_read(WindowRecord_strucRgn(w), &s);
        rgn_read(WindowRecord_contRgn(w), &c);
        hrgn_op(&s, &s, &c, RGN_DIFF);
        /* Only the parts not covered by windows in front. */
        for (gaddr f = wlist(); f && f != w; f = WindowRecord_nextWindow(f)) {
            if (!wvisible(f))
                continue;
            rgn_read(WindowRecord_strucRgn(f), &c);
            hrgn_op(&s, &s, &c, RGN_DIFF);
        }
        draw_frame(w, &s);
        hrgn_free(&s);
        hrgn_free(&c);
    }
}

/* Make the frontmost visible window the active one, queuing activate events. */
static void update_active(void)
{
    gaddr front = first_visible();
    for (gaddr w = wlist(); w; w = WindowRecord_nextWindow(w)) {
        if (w != front && WindowRecord_hilited(w)) {
            set_hilite(w, false);
            wr32(LM_CurDeactive, w);
        }
    }
    if (front && !WindowRecord_hilited(front)) {
        set_hilite(front, true);
        wr32(LM_CurActivate, front);
    }
}

/* ---------------------------------------------------------------------- */
/* List management */

static void unlink_window(gaddr w)
{
    gaddr prev = 0;
    for (gaddr x = wlist(); x; prev = x, x = WindowRecord_nextWindow(x)) {
        if (x == w) {
            if (prev)
                WindowRecord_set_nextWindow(prev, WindowRecord_nextWindow(w));
            else
                wr32(LM_WindowList, WindowRecord_nextWindow(w));
            WindowRecord_set_nextWindow(w, 0);
            return;
        }
    }
}

static void link_behind(gaddr w, gaddr behind)
{
    if (behind == IN_FRONT || !wlist()) {
        WindowRecord_set_nextWindow(w, wlist());
        wr32(LM_WindowList, w);
        return;
    }
    if (behind == 0) { /* at the back */
        gaddr x = wlist();
        while (WindowRecord_nextWindow(x))
            x = WindowRecord_nextWindow(x);
        WindowRecord_set_nextWindow(x, w);
        WindowRecord_set_nextWindow(w, 0);
        return;
    }
    WindowRecord_set_nextWindow(w, WindowRecord_nextWindow(behind));
    WindowRecord_set_nextWindow(behind, w);
}

/* ---------------------------------------------------------------------- */
/* Creating and disposing */

static gaddr new_string_handle(gaddr pstr)
{
    uint32_t n = pstr ? (uint32_t)rd8(pstr) + 1 : 1;
    gaddr h = tb_NewHandle((int32_t)n, false, false);
    if (pstr)
        gmemcpy(rd32(h), pstr, n);
    else
        wr8(rd32(h), 0);
    return h;
}

static gaddr new_window(gaddr storage, qrect bounds, gaddr title, bool vis, int16_t procid, gaddr behind, bool goaway,
                        int32_t refcon, bool color)
{
    LOGF(LOG_TRAPS, "  new window %d,%d,%d,%d proc %d vis %d\n", bounds.top, bounds.left, bounds.bottom, bounds.right,
         procid, vis);
    gaddr saved = qd_port();
    gaddr w = storage ? storage : tb_NewPtr(WindowRecord_SIZEOF, false, true);
    if (!w)
        return 0;
    if (color)
        tb_OpenCPort(w);
    else
        tb_OpenPort(w);
    tb_SetPort(w);
    tb_PortSize((int16_t)(bounds.right - bounds.left), (int16_t)(bounds.bottom - bounds.top));
    tb_MovePortTo(bounds.left, bounds.top);
    WindowRecord_set_windowKind(w, userKind);
    WindowRecord_set_visible(w, vis ? 1 : 0);
    WindowRecord_set_hilited(w, 0);
    WindowRecord_set_goAwayFlag(w, goaway ? 1 : 0);
    WindowRecord_set_spareFlag(w, (procid & 8) ? 1 : 0);
    WindowRecord_set_strucRgn(w, tb_NewRgn());
    WindowRecord_set_contRgn(w, tb_NewRgn());
    WindowRecord_set_updateRgn(w, tb_NewRgn());
    gaddr wdef = tb_GetResource((int32_t)fourcc("WDEF"), (int16_t)(procid >> 4));
    if (!wdef)
        wdef = tb_GetResource((int32_t)fourcc("WDEF"), 0);
    WindowRecord_set_windowDefProc(w, wdef);
    set_varcode(w, (int16_t)(procid & 15));
    WindowRecord_set_dataHandle(w, 0);
    WindowRecord_set_titleHandle(w, new_string_handle(title));
    WindowRecord_set_titleWidth(w, title ? tb_StringWidth(title) : 0);
    WindowRecord_set_controlList(w, 0);
    WindowRecord_set_windowPic(w, 0);
    WindowRecord_set_refCon(w, refcon);
    link_behind(w, behind);
    call_wdef(w, wNew, 0);
    call_wdef(w, wCalcRgns, 0);
    tb_SetPort(saved);
    if (vis) {
        update_active();
        repaint_rgn_of(w);
    } else {
        calc_all_vis();
    }
    return w;
}

gaddr tb_NewWindow(gaddr wst, gaddr r, gaddr title, int8_t vis, int16_t procid, gaddr behind, int8_t gaflag, int32_t rc)
{
    return new_window(wst, qrect_get(r), title, vis != 0, procid, behind, gaflag != 0, rc, false);
}

gaddr tb_NewCWindow(gaddr storage, gaddr r, gaddr title, int8_t vis, int16_t procid, gaddr behind, int8_t gaflag,
                    int32_t rc)
{
    return new_window(storage, qrect_get(r), title, vis != 0, procid, behind, gaflag != 0, rc, true);
}

/* WIND: bounds(8), procID(2), visible(2), goAway(2), refCon(4), title. */
static gaddr get_new_window(int16_t id, gaddr storage, gaddr behind, bool color)
{
    gaddr h = tb_GetResource((int32_t)fourcc("WIND"), id);
    if (!h || !rd32(h))
        return 0;
    tb_HLock(h);
    gaddr p = rd32(h);
    gaddr w = new_window(storage, qrect_get(p), p + 18, rd16(p + 10) != 0, (int16_t)rd16(p + 8), behind,
                         rd16(p + 12) != 0, (int32_t)rd32(p + 14), color);
    tb_HUnlock(h);
    return w;
}

gaddr tb_GetNewWindow(int16_t wid, gaddr wst, gaddr behind) { return get_new_window(wid, wst, behind, false); }
gaddr tb_GetNewCWindow(int16_t wid, gaddr wst, gaddr behind) { return get_new_window(wid, wst, behind, true); }

void tb_CloseWindow(gaddr w)
{
    bool was_visible = wvisible(w);
    hrgn s;
    hrgn_init(&s);
    rgn_read(WindowRecord_strucRgn(w), &s);
    call_wdef(w, wDispose, 0);
    unlink_window(w);
    WindowRecord_set_visible(w, 0);
    if (rd32(LM_CurActivate) == w)
        wr32(LM_CurActivate, 0);
    if (rd32(LM_CurDeactive) == w)
        wr32(LM_CurDeactive, 0);
    if (qd_port() == w)
        tb_SetPort(wmgr_port());
    tb_DisposeRgn(WindowRecord_strucRgn(w));
    tb_DisposeRgn(WindowRecord_contRgn(w));
    tb_DisposeRgn(WindowRecord_updateRgn(w));
    tb_DisposeHandle(WindowRecord_titleHandle(w));
    if (qd_port_is_color(w))
        tb_CloseCPort(w);
    else
        tb_ClosePort(w);
    forget_varcode(w);
    if (was_visible) {
        update_active();
        repaint(&s);
    }
    hrgn_free(&s);
}

void tb_DisposeWindow(gaddr w)
{
    tb_CloseWindow(w);
    tb_DisposePtr(w);
}

/* ---------------------------------------------------------------------- */
/* Showing, hiding, ordering */

void tb_ShowWindow(gaddr w)
{
    if (wvisible(w))
        return;
    WindowRecord_set_visible(w, 1);
    call_wdef(w, wCalcRgns, 0);
    update_active();
    repaint_rgn_of(w);
}

void tb_HideWindow(gaddr w)
{
    if (!wvisible(w))
        return;
    hrgn s;
    hrgn_init(&s);
    rgn_read(WindowRecord_strucRgn(w), &s);
    WindowRecord_set_visible(w, 0);
    set_hilite(w, false);
    update_active();
    repaint(&s);
    hrgn_free(&s);
}

void tb_ShowHide(gaddr w, int8_t flag)
{
    /* Like Show/HideWindow but without changing the highlighting. */
    if ((wvisible(w) != 0) == (flag != 0))
        return;
    hrgn s;
    hrgn_init(&s);
    WindowRecord_set_visible(w, flag ? 1 : 0);
    call_wdef(w, wCalcRgns, 0);
    rgn_read(WindowRecord_strucRgn(w), &s);
    repaint(&s);
    hrgn_free(&s);
}

void tb_HiliteWindow(gaddr w, int8_t flag) { set_hilite(w, flag != 0); }

void tb_BringToFront(gaddr w)
{
    if (wlist() == w)
        return;
    unlink_window(w);
    link_behind(w, IN_FRONT);
    repaint_rgn_of(w);
}

void tb_SelectWindow(gaddr w)
{
    tb_BringToFront(w);
    update_active();
}

void tb_SendBehind(gaddr w, gaddr behind)
{
    hrgn s;
    hrgn_init(&s);
    rgn_read(WindowRecord_strucRgn(w), &s);
    unlink_window(w);
    link_behind(w, behind);
    update_active();
    repaint(&s);
    hrgn_free(&s);
}

gaddr tb_FrontWindow(void) { return first_visible(); }

void tb_DrawGrowIcon(gaddr w)
{
    gaddr saved = qd_port();
    tb_SetPort(w);
    call_wdef(w, wDrawGIcon, 0);
    tb_SetPort(saved);
}

/* ---------------------------------------------------------------------- */
/* Titles and reference constants */

void tb_SetWTitle(gaddr w, gaddr t)
{
    gaddr h = WindowRecord_titleHandle(w);
    uint32_t n = (uint32_t)rd8(t) + 1;
    tb_SetHandleSize(h, (int32_t)n);
    gmemcpy(rd32(h), t, n);
    WindowRecord_set_titleWidth(w, tb_StringWidth(t));
    if (wvisible(w)) {
        hrgn s, c;
        hrgn_init(&s);
        hrgn_init(&c);
        rgn_read(WindowRecord_strucRgn(w), &s);
        rgn_read(WindowRecord_contRgn(w), &c);
        hrgn_op(&s, &s, &c, RGN_DIFF);
        draw_frame(w, &s);
        hrgn_free(&s);
        hrgn_free(&c);
    }
}

void tb_GetWTitle(gaddr w, gaddr t)
{
    gaddr h = WindowRecord_titleHandle(w);
    gmemcpy(t, rd32(h), (uint32_t)rd8(rd32(h)) + 1);
}

void tb_SetWRefCon(gaddr w, int32_t data) { WindowRecord_set_refCon(w, data); }
int32_t tb_GetWRefCon(gaddr w) { return WindowRecord_refCon(w); }
void tb_SetWindowPic(gaddr w, gaddr p) { WindowRecord_set_windowPic(w, p); }
gaddr tb_GetWindowPic(gaddr w) { return WindowRecord_windowPic(w); }

/* ---------------------------------------------------------------------- */
/* Finding windows */

int16_t tb_FindWindow(uint32_t p, gaddr wpp)
{
    wr32(wpp, 0);
    int16_t action_part = take_action(wpp);
    if (action_part)
        return action_part;
    if (pt_v(p) < (int16_t)rd16(LM_MBarHeight) && pt_v(p) >= 0)
        return inMenuBar;
    for (gaddr w = wlist(); w; w = WindowRecord_nextWindow(w)) {
        if (!wvisible(w))
            continue;
        hrgn s;
        hrgn_init(&s);
        rgn_read(WindowRecord_strucRgn(w), &s);
        bool in = hrgn_contains(&s, pt_h(p), pt_v(p));
        hrgn_free(&s);
        if (!in)
            continue;
        int32_t hit = call_wdef(w, wHit, (int32_t)p);
        if (hit == wNoHit)
            continue;
        wr32(wpp, w);
        if (WindowRecord_windowKind(w) < 0)
            return inSysWindow;
        switch (hit) {
        case wInContent: return inContent;
        case wInDrag: return inDrag;
        case wInGrow: return inGrow;
        case wInGoAway: return inGoAway;
        case wInZoomIn: return inZoomIn;
        default: return inZoomOut;
        }
    }
    return inDesk;
}

/* ---------------------------------------------------------------------- */
/* Update regions */

static void port_window_update(gaddr r_local_rgn_or_rect, bool is_rect, bool add)
{
    gaddr w = qd_port();
    if (!w)
        return;
    hrgn a, u;
    hrgn_init(&a);
    hrgn_init(&u);
    if (is_rect)
        hrgn_rect(&a, qrect_get(r_local_rgn_or_rect));
    else
        rgn_read(r_local_rgn_or_rect, &a);
    int dh, dv;
    port_offset(w, &dh, &dv);
    hrgn_offset(&a, -dh, -dv);
    rgn_read(WindowRecord_updateRgn(w), &u);
    hrgn_op(&u, &u, &a, add ? RGN_UNION : RGN_DIFF);
    rgn_write(WindowRecord_updateRgn(w), &u);
    hrgn_free(&a);
    hrgn_free(&u);
}

void tb_InvalRect(gaddr r) { port_window_update(r, true, true); }
void tb_InvalRgn(gaddr r) { port_window_update(r, false, true); }
void tb_ValidRect(gaddr r) { port_window_update(r, true, false); }
void tb_ValidRgn(gaddr r) { port_window_update(r, false, false); }

void tb_BeginUpdate(gaddr w)
{
    /* visRgn := visRgn ∩ updateRgn (local); the old visRgn is saved. */
    gaddr save = rd32(LM_SaveVisRgn);
    if (!save) {
        save = tb_NewRgn();
        wr32(LM_SaveVisRgn, save);
    }
    tb_CopyRgn(GrafPort_visRgn(w), save);
    hrgn v, u;
    hrgn_init(&v);
    hrgn_init(&u);
    rgn_read(GrafPort_visRgn(w), &v);
    rgn_read(WindowRecord_updateRgn(w), &u);
    int dh, dv;
    port_offset(w, &dh, &dv);
    hrgn_offset(&u, dh, dv);
    hrgn_op(&v, &v, &u, RGN_SECT);
    rgn_write(GrafPort_visRgn(w), &v);
    tb_SetEmptyRgn(WindowRecord_updateRgn(w));
    hrgn_free(&v);
    hrgn_free(&u);
}

void tb_EndUpdate(gaddr w)
{
    gaddr save = rd32(LM_SaveVisRgn);
    if (save)
        tb_CopyRgn(save, GrafPort_visRgn(w));
}

int8_t tb_CheckUpdate(gaddr ev)
{
    for (gaddr w = wlist(); w; w = WindowRecord_nextWindow(w)) {
        if (!wvisible(w))
            continue;
        if (!tb_EmptyRgn(WindowRecord_updateRgn(w))) {
            wr16(ev, 6); /* updateEvt */
            wr32(ev + 2, w);
            return 1;
        }
    }
    return 0;
}

/* Exposed so other managers can repaint after drawing over windows. */
void wm_repaint_all(void) { repaint_all(); }

/* ---------------------------------------------------------------------- */
/* Moving and sizing */

/* The part of a window's structure that shows: within the desktop and
 * not under a visible window in front of it. */
static void visible_struct(gaddr w, hrgn *out)
{
    hrgn tmp;
    hrgn_init(&tmp);
    rgn_read(WindowRecord_strucRgn(w), out);
    gray_rgn(&tmp);
    hrgn_op(out, out, &tmp, RGN_SECT);
    for (gaddr f = wlist(); f && f != w; f = WindowRecord_nextWindow(f)) {
        if (!wvisible(f))
            continue;
        rgn_read(WindowRecord_strucRgn(f), &tmp);
        hrgn_op(out, out, &tmp, RGN_DIFF);
    }
    hrgn_free(&tmp);
}

/* Copy the screen's pixels in rgn - (dh, dv) to rgn. */
static void copy_screen(const hrgn *rgn, int dh, int dv)
{
    pixdst d = qd_port_pixels(wmgr_port());
    qrect box = hrgn_bbox(rgn);
    int bw = box.right - box.left, bh = box.bottom - box.top;
    if (bw <= 0 || bh <= 0)
        return;
    uint32_t *buf = malloc((size_t)bw * (size_t)bh * sizeof *buf);
    if (!buf)
        return;
    /* Read everything first: source and destination overlap. */
    for (int pass = 0; pass < 2; pass++)
        for (int y = box.top; y < box.bottom; y++) {
            int n;
            const int16_t *xs = hrgn_row(rgn, y, &n);
            int sy = y - dv;
            if (!xs || y < d.bounds.top || y >= d.bounds.bottom || sy < d.bounds.top || sy >= d.bounds.bottom)
                continue;
            for (int k = 0; k < n; k += 2)
                for (int x = xs[k]; x < xs[k + 1]; x++) {
                    int sx = x - dh;
                    if (x < d.bounds.left || x >= d.bounds.right || sx < d.bounds.left || sx >= d.bounds.right)
                        continue;
                    uint32_t *p = &buf[(size_t)(y - box.top) * (size_t)bw + (size_t)(x - box.left)];
                    if (pass == 0)
                        *p = get_px(&d, sx - d.bounds.left, sy - d.bounds.top);
                    else
                        put_px(&d, x - d.bounds.left, y - d.bounds.top, *p);
                }
        }
    free(buf);
}

void tb_MoveWindow(gaddr w, int16_t h, int16_t v, int8_t front)
{
    /* As the Window Manager did: what showed of the window moves with it as
     * pixels, its pending update moves too, and only what that leaves
     * uncovered (where it was, and the parts of it that were hidden) is
     * repainted. Programs need not redraw a window that only moved. */
    hrgn old, shown, now;
    hrgn_init(&old);
    hrgn_init(&shown);
    hrgn_init(&now);
    rgn_read(WindowRecord_strucRgn(w), &old);
    bool vis = wvisible(w);
    if (vis)
        visible_struct(w, &shown);
    qrect before = global_content(w);
    gaddr saved = qd_port();
    tb_SetPort(w);
    tb_MovePortTo(h, v);
    tb_SetPort(saved);
    call_wdef(w, wCalcRgns, 0);
    qrect after = global_content(w);
    int dh = after.left - before.left, dv = after.top - before.top;
    hrgn upd;
    hrgn_init(&upd);
    rgn_read(WindowRecord_updateRgn(w), &upd);
    hrgn_offset(&upd, dh, dv);
    rgn_write(WindowRecord_updateRgn(w), &upd);
    hrgn_free(&upd);
    if (vis) {
        hrgn_offset(&shown, dh, dv);
        visible_struct(w, &now);
        hrgn_op(&shown, &shown, &now, RGN_SECT);
        copy_screen(&shown, dh, dv);
        rgn_read(WindowRecord_strucRgn(w), &now);
        hrgn_op(&old, &old, &now, RGN_UNION);
        hrgn_op(&old, &old, &shown, RGN_DIFF);
        repaint(&old);
    } else {
        calc_all_vis();
    }
    if (front)
        tb_SelectWindow(w);
    hrgn_free(&old);
    hrgn_free(&shown);
    hrgn_free(&now);
}

void tb_SizeWindow(gaddr w, int16_t width, int16_t height, int8_t flag)
{
    hrgn old;
    hrgn_init(&old);
    rgn_read(WindowRecord_strucRgn(w), &old);
    gaddr saved = qd_port();
    tb_SetPort(w);
    tb_PortSize(width, height);
    tb_SetPort(saved);
    call_wdef(w, wCalcRgns, 0);
    hrgn now;
    hrgn_init(&now);
    rgn_read(WindowRecord_strucRgn(w), &now);
    hrgn_op(&old, &old, &now, RGN_UNION);
    repaint(&old);
    if (!flag) /* caller handles updating the new content itself */
        tb_ValidRect(w + GrafPort_portRect_off);
    hrgn_free(&old);
    hrgn_free(&now);
}

int32_t tb_PinRect(gaddr r, uint32_t p)
{
    qrect q = qrect_get(r);
    int16_t h = pt_h(p), v = pt_v(p);
    if (h < q.left)
        h = q.left;
    if (h >= q.right)
        h = (int16_t)(q.right - 1);
    if (v < q.top)
        v = q.top;
    if (v >= q.bottom)
        v = (int16_t)(q.bottom - 1);
    return (int32_t)make_pt(v, h);
}

/* Mouse tracking comes from the Event Manager. */
bool ev_still_down(void);
uint32_t ev_mouse_global(void);
void ev_wait_tick(void);

int32_t tb_DragGrayRgn(gaddr rgn, uint32_t startp, gaddr limit, gaddr slop, int16_t axis, gaddr proc)
{
    /* Track the mouse until release; the gray outline is drawn with XOR. */
    hrgn shape, moved;
    hrgn_init(&shape);
    hrgn_init(&moved);
    rgn_read(rgn, &shape);
    hrgn_inset(&shape, 0, 0);
    hrgn inner;
    hrgn_init(&inner);
    hrgn_copy(&inner, &shape);
    hrgn_inset(&inner, 1, 1);
    hrgn_op(&shape, &shape, &inner, RGN_DIFF);
    gaddr saved = qd_port();
    tb_SetPort(wmgr_port());
    int dh = 0, dv = 0;
    bool shown = false;
    qrect lim = qrect_get(limit);
    while (ev_still_down()) {
        uint32_t m = ev_mouse_global();
        int ndh = pt_h(m) - pt_h(startp), ndv = pt_v(m) - pt_v(startp);
        if (axis == 1)
            ndv = 0;
        if (axis == 2)
            ndh = 0;
        if (pt_h(m) < lim.left || pt_h(m) >= lim.right || pt_v(m) < lim.top || pt_v(m) >= lim.bottom) {
            ev_wait_tick();
            continue;
        }
        if (ndh != dh || ndv != dv || !shown) {
            if (shown)
                qd_fill_region(wmgr_port(), &moved, qd_pat_gray, patXor);
            hrgn_copy(&moved, &shape);
            hrgn_offset(&moved, ndh, ndv);
            qd_fill_region(wmgr_port(), &moved, qd_pat_gray, patXor);
            shown = true;
            dh = ndh;
            dv = ndv;
        }
        ev_wait_tick();
    }
    if (shown)
        qd_fill_region(wmgr_port(), &moved, qd_pat_gray, patXor);
    tb_SetPort(saved);
    hrgn_free(&shape);
    hrgn_free(&moved);
    hrgn_free(&inner);
    return (int32_t)make_pt((int16_t)dv, (int16_t)dh);
}

void tb_DragWindow(gaddr wp, uint32_t p, gaddr rp)
{
    int nx, ny;
    if (complete_action(wp, MACHINE_ACTION_MOVE, &nx, &ny)) {
        tb_MoveWindow(wp, (int16_t)nx, (int16_t)ny, 0);
        tb_SelectWindow(wp);
        return;
    }
    gaddr slop = tb_NewPtr(8, false, false);
    qrect_put(slop, qrect_get(rp));
    int32_t d = tb_DragGrayRgn(WindowRecord_strucRgn(wp), p, rp, slop, 0, 0);
    tb_DisposePtr(slop);
    int16_t dh = pt_h((uint32_t)d), dv = pt_v((uint32_t)d);
    if (dh || dv) {
        qrect c = global_content(wp);
        tb_MoveWindow(wp, (int16_t)(c.left + dh), (int16_t)(c.top + dv), 0);
    }
    tb_SelectWindow(wp);
}

int32_t tb_GrowWindow(gaddr w, uint32_t startp, gaddr rp)
{
    int nw, nh;
    if (complete_action(w, MACHINE_ACTION_RESIZE, &nw, &nh)) {
        qrect lim = qrect_get(rp);
        if (nw < lim.left) nw = lim.left;
        if (nh < lim.top) nh = lim.top;
        if (nw > lim.right) nw = lim.right;
        if (nh > lim.bottom) nh = lim.bottom;
        return (int32_t)make_pt((int16_t)nh, (int16_t)nw);
    }
    qrect c = global_content(w);
    qrect lim = qrect_get(rp); /* min and max sizes */
    hrgn outline, inner;
    hrgn_init(&outline);
    hrgn_init(&inner);
    gaddr saved = qd_port();
    tb_SetPort(wmgr_port());
    int last_w = -1, last_h = -1;
    bool shown = false;
    while (ev_still_down()) {
        uint32_t m = ev_mouse_global();
        int nw = c.right - c.left + pt_h(m) - pt_h(startp), nh = c.bottom - c.top + pt_v(m) - pt_v(startp);
        if (nw < lim.left) nw = lim.left;
        if (nh < lim.top) nh = lim.top;
        if (nw > lim.right) nw = lim.right;
        if (nh > lim.bottom) nh = lim.bottom;
        if (nw != last_w || nh != last_h) {
            if (shown)
                qd_fill_region(wmgr_port(), &outline, qd_pat_gray, patXor);
            hrgn_rect(&outline, (qrect){ (int16_t)(c.top - TITLE_HEIGHT - 1), (int16_t)(c.left - 1),
                                         (int16_t)(c.top + nh + 1), (int16_t)(c.left + nw + 1) });
            hrgn_copy(&inner, &outline);
            hrgn_inset(&inner, 1, 1);
            hrgn_op(&outline, &outline, &inner, RGN_DIFF);
            qd_fill_region(wmgr_port(), &outline, qd_pat_gray, patXor);
            shown = true;
            last_w = nw;
            last_h = nh;
        }
        ev_wait_tick();
    }
    if (shown)
        qd_fill_region(wmgr_port(), &outline, qd_pat_gray, patXor);
    tb_SetPort(saved);
    hrgn_free(&outline);
    hrgn_free(&inner);
    if (last_w < 0 || (last_w == c.right - c.left && last_h == c.bottom - c.top))
        return 0;
    return (int32_t)make_pt((int16_t)last_h, (int16_t)last_w);
}

/* Ask the WDEF to toggle a box's pressed look, clipped to the window. */
static void toggle_box(gaddr w, int part)
{
    gaddr saved = qd_port();
    gaddr wp = wmgr_port();
    tb_SetPort(wp);
    tb_CopyRgn(WindowRecord_strucRgn(w), GrafPort_clipRgn(wp));
    call_wdef(w, wDraw, part);
    tb_SetRectRgn(GrafPort_clipRgn(wp), -32767, -32767, 32767, 32767);
    tb_SetPort(saved);
}

static int8_t track_box(gaddr w, uint32_t p, int part)
{
    /* Pressed while the mouse is in the box; true if released inside. */
    bool inside = true, shown = true;
    toggle_box(w, part);
    while (ev_still_down()) {
        uint32_t m = ev_mouse_global();
        inside = call_wdef(w, wHit, (int32_t)m) == part;
        if (inside != shown) {
            toggle_box(w, part);
            shown = inside;
        }
        ev_wait_tick();
    }
    if (shown)
        toggle_box(w, part);
    return inside;
}

int8_t tb_TrackGoAway(gaddr w, uint32_t p)
{
    if (complete_action(w, MACHINE_ACTION_CLOSE, NULL, NULL))
        return 1;
    return track_box(w, p, wInGoAway);
}

int8_t tb_TrackBox(gaddr wp, uint32_t pt, int16_t part)
{
    if (complete_action(wp, MACHINE_ACTION_ZOOM, NULL, NULL))
        return 1;
    return track_box(wp, pt, part == inZoomIn ? wInZoomIn : wInZoomOut);
}

void tb_ZoomWindow(gaddr wp, int16_t part, int8_t front)
{
    /* Toggle between the current size and the screen (standard state). */
    qrect c = global_content(wp);
    int16_t top = (int16_t)(rd16(LM_MBarHeight) + TITLE_HEIGHT + 4);
    if (c.left == 4 && c.top == top) {
        tb_MoveWindow(wp, 40, (int16_t)(top + 20), front);
        tb_SizeWindow(wp, (int16_t)(g_screen_width / 2), (int16_t)(g_screen_height / 2), 1);
    } else {
        tb_MoveWindow(wp, 4, top, front);
        tb_SizeWindow(wp, (int16_t)(g_screen_width - 8 - 2), (int16_t)(g_screen_height - top - 6), 1);
    }
}

/* ---------------------------------------------------------------------- */
/* Lower-level calls used by custom WDEFs and some apps */

void tb_CalcVis(gaddr w) { calc_all_vis(); }
void tb_CalcVisBehind(gaddr w, gaddr clobbered) { calc_all_vis(); }
void tb_PaintOne(gaddr w, gaddr clobbered)
{
    hrgn c;
    hrgn_init(&c);
    rgn_read(clobbered, &c);
    repaint(&c);
    hrgn_free(&c);
}
void tb_PaintBehind(gaddr w, gaddr clobbered) { tb_PaintOne(w, clobbered); }
void tb_ClipAbove(gaddr w)
{
    gaddr wp = wmgr_port();
    hrgn clip, s;
    hrgn_init(&clip);
    hrgn_init(&s);
    gray_rgn(&clip);
    for (gaddr f = wlist(); f && f != w; f = WindowRecord_nextWindow(f)) {
        if (!wvisible(f))
            continue;
        rgn_read(WindowRecord_strucRgn(f), &s);
        hrgn_op(&clip, &clip, &s, RGN_DIFF);
    }
    rgn_write(GrafPort_clipRgn(wp), &clip);
    hrgn_free(&clip);
    hrgn_free(&s);
}
void tb_SaveOld(gaddr w) {}
void tb_DrawNew(gaddr w, int8_t flag) { repaint_rgn_of(w); }
int8_t tb_GetAuxWin(gaddr w, gaddr h)
{
    wr32(h, 0);
    return 0;
}
void tb_SetDeskCPat(gaddr p) {}

/* ---------------------------------------------------------------------- */
/* Initialisation */

void tb_InitWindows(void)
{
    /* Window Manager ports covering the screen. */
    gaddr wp = rd32(LM_WMgrPort);
    if (!wp) {
        wp = tb_NewPtr(GrafPort_SIZEOF, true, true);
        wr32(LM_WMgrPort, wp);
        gaddr cp = tb_NewPtr(GrafPort_SIZEOF, true, true);
        wr32(LM_WMgrCPort, cp);
        gaddr saved = qd_port();
        tb_OpenCPort(cp);
        tb_OpenPort(wp);
        tb_SetPort(saved);
    }
    wr32(LM_WindowList, 0);
    wr32(LM_CurActivate, 0);
    wr32(LM_CurDeactive, 0);
    gput(LM_DeskPattern, qd_pat_gray, 8);
    int mbar = (int16_t)rd16(LM_MBarHeight) ? (int16_t)rd16(LM_MBarHeight) : 20;
    wr16(LM_MBarHeight, (uint16_t)mbar);

    /* Desktop: the screen below the menu bar, with rounded corners. */
    hrgn gray, bar;
    hrgn_init(&gray);
    hrgn_init(&bar);
    qd_rrect_region(&gray, (qrect){ 0, 0, (int16_t)g_screen_height, (int16_t)g_screen_width }, 16, 16);
    hrgn_rect(&bar, (qrect){ 0, 0, (int16_t)mbar, (int16_t)g_screen_width });
    hrgn_op(&gray, &gray, &bar, RGN_DIFF);
    gaddr gh = rd32(LM_GrayRgn);
    if (!gh) {
        gh = tb_NewHandle(10, true, false);
        wr32(LM_GrayRgn, gh);
    }
    rgn_write(gh, &gray);

    /* Paint: black screen corners, white menu bar, gray desk. */
    gaddr saved = qd_port();
    tb_SetPort(wp);
    hrgn all;
    hrgn_init(&all);
    hrgn_rect(&all, (qrect){ 0, 0, (int16_t)g_screen_height, (int16_t)g_screen_width });
    qd_fill_region(wp, &all, qd_pat_black, patCopy);
    hrgn rounded;
    hrgn_init(&rounded);
    qd_rrect_region(&rounded, (qrect){ 0, 0, (int16_t)g_screen_height, (int16_t)g_screen_width }, 16, 16);
    hrgn_op(&bar, &bar, &rounded, RGN_SECT);
    qd_fill_region(wp, &bar, qd_pat_white, patCopy);
    tb_SetPort(saved);
    paint_desk(&gray);
    hrgn_free(&all);
    hrgn_free(&rounded);
    hrgn_free(&gray);
    hrgn_free(&bar);
    wr8(LM_WWExist, 0);
    /* As in the ROM, the Window Manager port is left current. */
    tb_SetPort(wp);
}

int16_t tb_GetWVariant(gaddr w) { return w ? varcode(w) : 0; }
