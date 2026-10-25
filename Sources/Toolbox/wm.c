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
#include "carbon_hi.h"
#include "machine.h"
#include "tb_internal.h"

enum { wDraw = 0, wHit, wCalcRgns, wNew, wDispose, wGrow, wDrawGIcon };
enum { wNoHit = 0, wInContent, wInDrag, wInGrow, wInGoAway, wInZoomIn, wInZoomOut, wInCollapseBox = 9 };
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

static void set_collapsed(gaddr w, bool on);
static void forget_properties(gaddr w);

static void forget_varcode(gaddr w)
{
    set_collapsed(w, false);
    forget_properties(w);
    carbonwm_forget(w);
    for (int i = 0; i < 256; i++)
        if (varcodes[i].w == w)
            varcodes[i].w = 0;
}

static gaddr wlist(void) { return rd32(LM_WindowList); }
static gaddr wmgr_port(void) { return rd32(LM_WMgrPort); }
static bool wvisible(gaddr w) { return WindowRecord_visible(w) != 0; }

/* Global offset of a window port: local = global + (dh, dv). */
/* Collapsed windows (WindowShade, Mac OS 8's collapse box): only their
 * title bar shows; the definition lays them out with no content. */
static gaddr collapsed[64];
static int ncollapsed;

static bool is_collapsed(gaddr w)
{
    for (int i = 0; i < ncollapsed; i++)
        if (collapsed[i] == w)
            return true;
    return false;
}

static void set_collapsed(gaddr w, bool on)
{
    for (int i = 0; i < ncollapsed; i++)
        if (collapsed[i] == w) {
            if (!on)
                collapsed[i] = collapsed[--ncollapsed];
            return;
        }
    if (on && ncollapsed < 64)
        collapsed[ncollapsed++] = w;
}

/* Floating windows hidden together by HideFloatingWindows. */
static bool floats_hidden;
static gaddr hidden_floats[32];
static int nhidden_floats;

void wdef0_native(cpu68k *c, uint16_t tw);
void wdef124_native(cpu68k *c, uint16_t tw);

/* Which of our definitions draws the window: 0, 124, or -1 for the
 * application's own (which knows nothing of collapsing). */
static int our_wdef(gaddr w)
{
    gaddr h = WindowRecord_windowDefProc(w);
    if (!h || !rd32(h) || rd16(rd32(h)) != CPU_NATIVE_OP)
        return -1;
    native_fn fn = native_fn_at(rd16(rd32(h) + 2));
    return fn == wdef0_native ? 0 : fn == wdef124_native ? 124 : -1;
}

static bool is_floating(gaddr w) { return our_wdef(w) == 124; }

/* After InitFloatingWindows (Mac OS 8.5), the Window Manager keeps the
 * floating windows (WDEF 124's) in front of the others, all of them
 * highlighted, and the front document window the active one. */
static bool float_layer;

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
    /* A WDEF written for black-and-white ports finds the window's place on
     * the screen in portBits.bounds; in a colour window those bytes are the
     * grafVars handle.  While it works out the regions they hold the
     * PixMap's bounds' top left instead, which made Total Distortion's
     * stage (Director 4, FinderHider WDEF) visible on a Power Mac and here
     * left its content region thirty thousand pixels off the screen. */
    gaddr gv_at = w + CGrafPort_grafVars_off;
    uint32_t grafvars = rd32(gv_at);
    gaddr pmh = rd32(w + CGrafPort_portPixMap_off);
    bool stand_in = msg == wCalcRgns && qd_port_is_color(w) && pmh && rd32(pmh);
    if (stand_in)
        wr32(gv_at, rd32(rd32(pmh) + PixMap_bounds_off));
    guest_call(rd32(h));
    if (stand_in)
        wr32(gv_at, grafvars);
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

/* Zoom states: as the standard WDEF did, a zoomable window's dataHandle
 * holds a WStateData, userState then stdState (global content
 * rectangles), made at wNew; programs read and set them (Shards opens its
 * puzzle window at another window's userState). */
static gaddr zoom_state(gaddr w)
{
    gaddr h = WindowRecord_dataHandle(w);
    return h && rd32(h) && tb_GetHandleSize(h) >= 16 ? h : 0;
}

/* The standard state: the screen below the menu bar, room left for the
 * title bar, inset a little. */
static qrect std_state(void)
{
    int16_t top = (int16_t)(rd16(LM_MBarHeight) + TITLE_HEIGHT + 4);
    return (qrect){ top, 4, (int16_t)(g_screen_height - 6), (int16_t)(g_screen_width - 6) };
}

static void wdef_new(gaddr w, int var)
{
    if (!has_zoom(var) || WindowRecord_dataHandle(w))
        return;
    gaddr h = tb_NewHandle(16, false, true);
    if (!h)
        return;
    qrect_put(rd32(h), global_content(w));
    qrect_put(rd32(h) + 8, std_state());
    WindowRecord_set_dataHandle(w, h);
}

static void wdef_dispose(gaddr w, int var)
{
    if (has_zoom(var) && WindowRecord_dataHandle(w)) {
        tb_DisposeHandle(WindowRecord_dataHandle(w));
        WindowRecord_set_dataHandle(w, 0);
    }
}

static bool rect_eq(qrect a, qrect b)
{
    return a.top == b.top && a.left == b.left && a.bottom == b.bottom && a.right == b.right;
}

/* The zoom box's part: zoom in from the standard state, out otherwise. */
static int zoom_hit(gaddr w)
{
    gaddr z = zoom_state(w);
    return z && rect_eq(global_content(w), qrect_get(rd32(z) + 8)) ? wInZoomIn : wInZoomOut;
}

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
    if (getenv("BLUEBOX_DEBUG_HOST"))
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
        case MACHINE_ACTION_ZOOM: part = zoom_hit(pending.w) == wInZoomIn ? 7 : 8; break; /* inZoomIn, inZoomOut */
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

/* A collapsed document window: no content, the title bar's bottom edge
 * where the content's top was (the theme's samples take a zero height). */
static bool doc_variant(int var) { return var == 0 || var == 4 || var == 8 || var == 12; }

static bool collapse_content(gaddr w, int var, qrect *c, qrect *ct)
{
    *ct = *c;
    if (!is_collapsed(w) || !doc_variant(var) || native_frame(var))
        return false;
    c->bottom = (int16_t)(c->top - 1);
    ct->bottom = ct->top;
    return true;
}

static void wdef_calc(gaddr w, int var)
{
    qrect c = global_content(w), ct;
    bool coll = collapse_content(w, var, &c, &ct);
    hrgn s, k;
    hrgn_init(&s);
    hrgn_init(&k);
    if (!coll)
        hrgn_rect(&k, c);
    if (native_frame(var)) {
        hrgn_rect(&s, c); /* the host draws the frame */
    } else if (theme_window_region(w, coll ? var | 0x200 : var, ct, &s)) {
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
    if (n == 0)
        return; /* untitled: the stripes go all the way across */
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
    qrect c = global_content(w), ct;
    bool coll = collapse_content(w, var, &c, &ct);
    bool hilited = WindowRecord_hilited(w) != 0;
    if (native_frame(var) ||
        theme_draw_window(w, coll ? var | 0x200 : var, ct, hilited, WindowRecord_goAwayFlag(w) != 0, has_zoom(var),
                          param))
        return;
    gaddr r = tb_NewPtr(8, false, false);
    tb_PenNormal();
    if (is_dialog_variant(var)) {
        if (var == 1) {
            set_rect(r, c.top - 8, c.left - 8, c.bottom + 8, c.right + 8);
            tb_FrameRect(r);
            /* The frame's inside, not the content (which is the window's,
             * in its own background: a theme dialog's grey). */
            set_rect(r, c.top - 7, c.left - 7, c.top, c.right + 7);
            tb_EraseRect(r);
            set_rect(r, c.bottom, c.left - 7, c.bottom + 7, c.right + 7);
            tb_EraseRect(r);
            set_rect(r, c.top, c.left - 7, c.bottom, c.left);
            tb_EraseRect(r);
            set_rect(r, c.top, c.right, c.bottom, c.right + 7);
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
    qrect c = global_content(w), ct;
    bool coll = collapse_content(w, var, &c, &ct);
    int h = pt_h(pt), v = pt_v(pt);
    if (h >= c.left && h < c.right && v >= c.top && v < c.bottom) {
        if (has_grow(var) && WindowRecord_hilited(w) && h >= c.right - 15 && v >= c.bottom - 15)
            return wInGrow;
        return wInContent;
    }
    int part = theme_active() ? theme_window_hit(w, coll ? var | 0x200 : var, ct, h, v) : -1;
    if (part >= 0) {
        bool hil = WindowRecord_hilited(w) != 0;
        if (part == 1 && hil && WindowRecord_goAwayFlag(w))
            return wInGoAway;
        if (part == 2 && hil && has_zoom(var))
            return zoom_hit(w);
        if (part == 3 && doc_variant(var))
            return wInCollapseBox;
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
                return zoom_hit(w);
        }
        return wInDrag;
    }
    return wNoHit;
}

static void wdef_grow_icon(gaddr w, int var)
{
    if (!has_grow(var) || !wvisible(w) || is_collapsed(w))
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
    case wNew: wdef_new(w, var); break;
    case wDispose: wdef_dispose(w, var); break;
    case wDrawGIcon: wdef_grow_icon(w, var); break;
    default: break;
    }
    wr32(c->a[7], (uint32_t)result);
    cpu_push32(c, ret);
}

/* ---------------------------------------------------------------------- */
/* WDEF 124: floating windows (procIDs 1985 to 1999), as in Mac OS 8: a
 * narrow title bar on top, or on the left (variant bit 8), with a close
 * box, and a zoom box (bit 4) and grow box (bit 2) when asked for.  The
 * theme's floating samples draw them when a scheme is in use (variant
 * 0x100 | var there). */

#define FLOAT_BAR 11

static bool float_side(int var) { return (var & 8) != 0; }
static bool float_zoom(int var) { return (var & 4) != 0; }
static bool float_grow(int var) { return (var & 2) != 0; }

/* The title bar, in global coordinates. */
static qrect float_bar(qrect c, int var)
{
    if (float_side(var))
        return (qrect){ (int16_t)(c.top - 1), (int16_t)(c.left - FLOAT_BAR - 1), (int16_t)(c.bottom + 1),
                        (int16_t)(c.left - 1) };
    return (qrect){ (int16_t)(c.top - FLOAT_BAR - 1), (int16_t)(c.left - 1), (int16_t)(c.top - 1),
                    (int16_t)(c.right + 1) };
}

/* The close (first) and zoom (second) boxes: 7 pixels square, at the start
 * and the end of the title bar. */
static qrect float_box(qrect bar, int var, bool zoom)
{
    if (float_side(var)) {
        int v = zoom ? bar.bottom - 10 : bar.top + 3;
        return (qrect){ (int16_t)v, (int16_t)(bar.left + 3), (int16_t)(v + 7), (int16_t)(bar.left + 10) };
    }
    int h = zoom ? bar.right - 10 : bar.left + 3;
    return (qrect){ (int16_t)(bar.top + 3), (int16_t)h, (int16_t)(bar.top + 10), (int16_t)(h + 7) };
}

/* A collapsed floating window keeps only its title bar: no content height,
 * or for a side title no content width. */
static int float_collapse(gaddr w, int var, qrect *c, qrect *ct)
{
    *ct = *c;
    if (!is_collapsed(w))
        return 0x100 | var;
    if (float_side(var)) {
        c->right = (int16_t)(c->left - 1);
        ct->right = ct->left;
    } else {
        c->bottom = (int16_t)(c->top - 1);
        ct->bottom = ct->top;
    }
    return 0x300 | var;
}

static void wdef124_calc(gaddr w, int var)
{
    qrect c = global_content(w), ct;
    int tvar = float_collapse(w, var, &c, &ct);
    hrgn s, k;
    hrgn_init(&s);
    hrgn_init(&k);
    if (!is_collapsed(w))
        hrgn_rect(&k, c);
    if (!theme_window_region(w, tvar, ct, &s)) {
        qrect b = float_bar(c, var);
        /* Frame and title bar, and a one-pixel shadow. */
        qrect f = { (int16_t)(float_side(var) ? c.top - 1 : b.top), (int16_t)(float_side(var) ? b.left : c.left - 1),
                    (int16_t)(c.bottom + 2), (int16_t)(c.right + 2) };
        hrgn_rect(&s, f);
    }
    rgn_write(WindowRecord_strucRgn(w), &s);
    rgn_write(WindowRecord_contRgn(w), &k);
    hrgn_free(&s);
    hrgn_free(&k);
}

static void float_title_text(gaddr w, qrect bar)
{
    gaddr th = WindowRecord_titleHandle(w);
    if (!th || !rd32(th) || !rd8(rd32(th)))
        return;
    gaddr port = qd_port();
    GrafPort_set_txFont(port, 3); /* Geneva 9, as Mac OS 8's floating windows */
    GrafPort_set_txSize(port, 9);
    GrafPort_set_txFace(port, 0);
    GrafPort_set_txMode(port, 1);
    gaddr s = rd32(th);
    int n = rd8(s);
    if (n == 0)
        return; /* untitled: the stripes go all the way across */
    uint8_t buf[256];
    gget(buf, s + 1, (uint32_t)n);
    int tw = qd_text_width(port, buf, n);
    int avail = bar.right - bar.left - 30;
    if (tw > avail)
        return;
    int x = bar.left + (bar.right - bar.left - tw) / 2;
    gaddr r = tb_NewPtr(8, false, false);
    set_rect(r, bar.top + 1, x - 4, bar.bottom, x + tw + 4);
    tb_EraseRect(r);
    tb_DisposePtr(r);
    tb_MoveTo((int16_t)x, (int16_t)(bar.top + 9));
    tb_DrawText(s + 1, 0, (int16_t)n);
}

static void wdef124_draw(gaddr w, int var, int32_t param)
{
    if (!wvisible(w))
        return;
    qrect c = global_content(w), ct;
    int tvar = float_collapse(w, var, &c, &ct);
    bool hilited = WindowRecord_hilited(w) != 0, goaway = WindowRecord_goAwayFlag(w) != 0;
    if (theme_draw_window(w, tvar, ct, hilited, goaway, float_zoom(var), param))
        return;
    qrect b = float_bar(c, var);
    gaddr r = tb_NewPtr(8, false, false);
    tb_PenNormal();
    if (param == wInGoAway || param == wInZoomIn || param == wInZoomOut) {
        qrect x = float_box(b, var, param != wInGoAway);
        set_rect(r, x.top + 1, x.left + 1, x.bottom - 1, x.right - 1);
        tb_InvertRect(r);
        tb_DisposePtr(r);
        return;
    }
    /* Frame round title bar and content, the bar's edge, and the shadow. */
    qrect f = { (int16_t)(float_side(var) ? c.top - 1 : b.top), (int16_t)(float_side(var) ? b.left : c.left - 1),
                (int16_t)(c.bottom + 1), (int16_t)(c.right + 1) };
    set_rect(r, f.top, f.left, f.bottom, f.right);
    tb_FrameRect(r);
    tb_MoveTo((int16_t)(f.right), (int16_t)(f.top + 1));
    tb_LineTo((int16_t)(f.right), (int16_t)(f.bottom));
    tb_LineTo((int16_t)(f.left + 1), (int16_t)(f.bottom));
    set_rect(r, b.top + 1, b.left + 1, float_side(var) ? b.bottom - 1 : b.bottom, float_side(var) ? b.right : b.right - 1);
    tb_EraseRect(r);
    if (float_side(var)) {
        tb_MoveTo(b.right, b.top);
        tb_LineTo(b.right, (int16_t)(b.bottom - 1));
    } else {
        tb_MoveTo(b.left, b.bottom);
        tb_LineTo((int16_t)(b.right - 1), b.bottom);
    }
    if (hilited) {
        /* Stripes: every other row (column for a side bar), as in the
         * document windows' title bars. */
        if (float_side(var))
            for (int x = b.left + 2; x < b.right - 1; x += 2) {
                tb_MoveTo((int16_t)x, (int16_t)(b.top + 2));
                tb_LineTo((int16_t)x, (int16_t)(b.bottom - 3));
            }
        else
            for (int y = b.top + 2; y < b.bottom - 1; y += 2) {
                tb_MoveTo((int16_t)(b.left + 2), (int16_t)y);
                tb_LineTo((int16_t)(b.right - 3), (int16_t)y);
            }
        for (int k = 0; k < 2; k++) {
            if (k == 0 ? !goaway : !float_zoom(var))
                continue;
            qrect x = float_box(b, var, k == 1);
            set_rect(r, x.top - 1, x.left - 1, x.bottom + 1, x.right + 1);
            tb_EraseRect(r);
            set_rect(r, x.top, x.left, x.bottom, x.right);
            tb_FrameRect(r);
            if (k == 1) {
                set_rect(r, x.top, x.left, x.top + 4, x.left + 4);
                tb_FrameRect(r);
            }
        }
    }
    if (!float_side(var))
        float_title_text(w, b);
    tb_DisposePtr(r);
}

static int32_t wdef124_hit(gaddr w, int var, uint32_t pt)
{
    qrect c = global_content(w), ct;
    int tvar = float_collapse(w, var, &c, &ct);
    int h = pt_h(pt), v = pt_v(pt);
    bool hil = WindowRecord_hilited(w) != 0;
    if (h >= c.left && h < c.right && v >= c.top && v < c.bottom) {
        if (float_grow(var) && hil && h >= c.right - 15 && v >= c.bottom - 15)
            return wInGrow;
        return wInContent;
    }
    int part = theme_active() ? theme_window_hit(w, tvar, ct, h, v) : -1;
    if (part >= 0) {
        if (part == 1 && hil && WindowRecord_goAwayFlag(w))
            return wInGoAway;
        if (part == 2 && hil && float_zoom(var))
            return wInZoomIn;
        if (part == 3)
            return wInCollapseBox;
        return part == 0 ? wNoHit : wInDrag;
    }
    qrect b = float_bar(c, var);
    if (h < b.left || h >= b.right || v < b.top || v >= b.bottom)
        return wNoHit;
    for (int k = 0; k < 2 && hil; k++) {
        if (k == 0 ? !WindowRecord_goAwayFlag(w) : !float_zoom(var))
            continue;
        qrect x = float_box(b, var, k == 1);
        if (h >= x.left && h < x.right && v >= x.top && v < x.bottom)
            return k == 0 ? wInGoAway : wInZoomIn;
    }
    return wInDrag;
}

void wdef124_native(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    int32_t param = (int32_t)cpu_pop32(c);
    int16_t msg = (int16_t)cpu_pop16(c);
    gaddr w = cpu_pop32(c);
    int16_t var = (int16_t)cpu_pop16(c);
    int32_t result = 0;
    switch (msg) {
    case wDraw: wdef124_draw(w, var, param); break;
    case wHit: result = wdef124_hit(w, var, (uint32_t)param); break;
    case wCalcRgns: wdef124_calc(w, var); break;
    case wDrawGIcon:
        if (float_grow(var) && !is_collapsed(w))
            wdef_grow_icon(w, 0); /* the document window's grow box */
        break;
    default: break;
    }
    wr32(c->a[7], (uint32_t)result);
    cpu_push32(c, ret);
}

/* ---------------------------------------------------------------------- */
/* Visibility and painting */

static void gray_rgn(hrgn *out) { rgn_read(rd32(LM_GrayRgn), out); }

/* The desktop (GrayRgn): the screen below a menu bar this high, with
 * rounded corners; all of it with none. */
static void set_desk(int mbar)
{
    hrgn gray, bar;
    hrgn_init(&gray);
    hrgn_init(&bar);
    if (mbar) {
        qd_rrect_region(&gray, (qrect){ 0, 0, (int16_t)g_screen_height, (int16_t)g_screen_width }, 16, 16);
        hrgn_rect(&bar, (qrect){ 0, 0, (int16_t)mbar, (int16_t)g_screen_width });
        hrgn_op(&gray, &gray, &bar, RGN_DIFF);
    } else {
        hrgn_rect(&gray, (qrect){ 0, 0, (int16_t)g_screen_height, (int16_t)g_screen_width });
    }
    gaddr gh = rd32(LM_GrayRgn);
    if (!gh) {
        gh = tb_NewHandle(10, true, false);
        wr32(LM_GrayRgn, gh);
    }
    rgn_write(gh, &gray);
    hrgn_free(&gray);
    hrgn_free(&bar);
}

/* The menu bar hidden, as a full-screen game has it (MBarHeight 0, the
 * desk the whole screen), and shown again at the height it had. */
void wm_hide_menu_bar(bool hide)
{
    static int16_t saved_height;
    if (hide) {
        if (!rd16(LM_MBarHeight))
            return;
        saved_height = (int16_t)rd16(LM_MBarHeight);
        wr16(LM_MBarHeight, 0);
        set_desk(0);
    } else {
        if (!saved_height || rd16(LM_MBarHeight))
            return;
        wr16(LM_MBarHeight, (uint16_t)saved_height);
        set_desk(saved_height);
        tb_DrawMenuBar();
    }
}

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
    if (rd32(LM_DeskHook)) {
        /* The program paints the desktop itself: D0 zero, the area to paint
         * the WMgrPort's clipRgn. */
        hrgn clip;
        hrgn_init(&clip);
        rgn_read(GrafPort_clipRgn(wp), &clip);
        rgn_write(GrafPort_clipRgn(wp), &r);
        hook_call(rd32(LM_DeskHook), 0, 0, g_cpu->a[7]);
        tb_SetPort(wp);
        rgn_write(GrafPort_clipRgn(wp), &clip);
        hrgn_free(&clip);
    } else {
        uint8_t pat[8];
        gget(pat, LM_DeskPattern, 8);
        qd_fill_region(wp, &r, pat, patCopy);
    }
    hrgn_free(&r);
    tb_SetPort(saved);
}

/* DragHook: called on every pass of the Window Manager's mouse tracking
 * (DragWindow, DragGrayRgn, GrowWindow, TrackGoAway, TrackBox). */
static void drag_hook(void)
{
    if (rd32(LM_DragHook))
        hook_call(rd32(LM_DragHook), 0, 0, g_cpu->a[7]);
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
            /* (or the program paints it: CarbonLib's InstallWindowContentPaintProc) */
            if (!hrgn_empty(&cont) && rd16(LM_PaintWhite) && !carbonwm_paint_content(w, &cont)) {
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


/* Repaint the whole screen as if everything had been uncovered: desktop and
 * frames drawn, and every window's content erased and queued for update
 * (the redraw check: what the program then draws should match what was
 * there). */
void machine_redraw_all(void) { repaint_all(); }

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

/* The window to make active: with floating windows layered, the front one
 * of the others. */
static gaddr front_active(void)
{
    gaddr ghost = rd32(LM_GhostWindow);
    for (gaddr w = wlist(); w; w = WindowRecord_nextWindow(w))
        if (wvisible(w) && w != ghost && !(float_layer && is_floating(w)))
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

/* The window that was made active last.  The front window gets its
 * activate event when it becomes the active one, whether or not it was
 * highlighted already: HyperCard 2.4's message box (a palette it highlights
 * itself) shown in front of the card, which HyperCard's GetNextEvent patch
 * answers by highlighting the card again. */
static gaddr active_window;

/* The window SaveOld saved the regions of, and those regions. */
static gaddr old_window;
static hrgn old_struc, old_cont;

/* ShowHide and BringToFront: a program managing its windows itself (with
 * HiliteWindow) gets no activate events from them, as on a Mac, so the
 * window in front afterwards is taken as the active one.  Otherwise the
 * next window made would bring an activate for it (Squirt Derby! shows
 * its window that way, then rebuilt its buffers on the activate and
 * crashed disposing them). */
static void took_front(void) { active_window = front_active(); }

/* Make the frontmost visible window the active one, queuing activate events. */
static void update_active(void)
{
    gaddr front = front_active();
    for (gaddr w = wlist(); w; w = WindowRecord_nextWindow(w)) {
        if (float_layer && is_floating(w)) {
            set_hilite(w, wvisible(w) != 0);
            continue;
        }
        if (w != front && WindowRecord_hilited(w)) {
            set_hilite(w, false);
            wr32(LM_CurDeactive, w);
        }
    }
    if (front && (!WindowRecord_hilited(front) || front != active_window)) {
        set_hilite(front, true);
        wr32(LM_CurActivate, front);
    }
    active_window = front;
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
    if (behind == IN_FRONT && float_layer && !is_floating(w)) {
        /* In front of the others: behind the floating windows. */
        gaddr last = 0;
        for (gaddr x = wlist(); x && is_floating(x); x = WindowRecord_nextWindow(x))
            last = x;
        if (last)
            behind = last;
    }
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
    /* (CarbonLib's NewWindow made colour windows as NewCWindow does; a
     * Carbon program's storage, if any, is a CWindowRecord's size) */
    if (cfm_imports_carbonlib())
        color = true;
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
    /* (a definition CarbonLib's RegisterWindowDefinition registered first) */
    gaddr wdef = carbonwm_registered_def((int16_t)(procid >> 4));
    if (!wdef)
        wdef = tb_GetResource((int32_t)fourcc("WDEF"), (int16_t)(procid >> 4));
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

static void remove_aux(gaddr w);

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
    gaddr ctab = color && w ? tb_GetResource((int32_t)fourcc("wctb"), id) : 0;
    if (ctab && rd32(ctab))
        tb_SetWinColor(w, ctab); /* its colour table, by the WIND's ID */
    return w;
}

gaddr tb_GetNewWindow(int16_t wid, gaddr wst, gaddr behind) { return get_new_window(wid, wst, behind, false); }
/* GetNewCWindow: a colour window, given the 'pltt' palette of its own ID if
 * there is one, as the Palette Manager does (Graphing Calculator's colours
 * are entries of its pltt 128). */
gaddr tb_GetNewCWindow(int16_t wid, gaddr wst, gaddr behind)
{
    gaddr w = get_new_window(wid, wst, behind, true);
    if (w) {
        gaddr pal = tb_GetNewPalette(wid);
        if (pal)
            tb_SetPalette(w, pal, true);
    }
    return w;
}

/* What CloseWindow frees: the regions, the title and the port. */
static void free_window_parts(gaddr w)
{
    tb_DisposeRgn(WindowRecord_strucRgn(w));
    tb_DisposeRgn(WindowRecord_contRgn(w));
    tb_DisposeRgn(WindowRecord_updateRgn(w));
    tb_DisposeHandle(WindowRecord_titleHandle(w));
    if (qd_port_is_color(w))
        tb_CloseCPort(w);
    else
        tb_ClosePort(w);
}

static void close_window(gaddr w, bool keep_parts)
{
    bool was_visible = wvisible(w);
    hrgn s;
    hrgn_init(&s);
    rgn_read(WindowRecord_strucRgn(w), &s);
    call_wdef(w, wDispose, 0);
    unlink_window(w);
    remove_aux(w);
    if (active_window == w)
        active_window = 0;
    if (old_window == w)
        old_window = 0;
    controls_forget_window(w);
    WindowRecord_set_visible(w, 0);
    if (rd32(LM_CurActivate) == w)
        wr32(LM_CurActivate, 0);
    if (rd32(LM_CurDeactive) == w)
        wr32(LM_CurDeactive, 0);
    if (qd_port() == w)
        tb_SetPort(wmgr_port());
    if (!keep_parts)
        free_window_parts(w);
    forget_varcode(w);
    if (was_visible) {
        update_active();
        repaint(&s);
    }
    hrgn_free(&s);
}

void tb_CloseWindow(gaddr w) { close_window(w, false); }

/* Windows that DisposeWindow frees stay whole (record, regions, port) until
 * a few more have been disposed, as freed memory on a Mac stays as it was
 * until something else takes it.  Programs go on using a window they have
 * just disposed: Star Chaos keeps its CPU speed test's window as its main
 * one, then moves, sizes, shows and selects it and plays in it.  A window
 * brought back into the window list that way is never freed. */
enum { DISPOSED_KEPT = 4 };
static gaddr disposed[DISPOSED_KEPT];
static int ndisposed;

static bool in_window_list(gaddr w)
{
    for (gaddr x = wlist(); x; x = WindowRecord_nextWindow(x))
        if (x == w)
            return true;
    return false;
}

void tb_DisposeWindow(gaddr w)
{
    for (int i = 0; i < ndisposed; i++)
        if (disposed[i] == w) { /* disposed again: it was in use after all */
            disposed[i] = disposed[--ndisposed];
            break;
        }
    close_window(w, true);
    if (ndisposed == DISPOSED_KEPT) {
        gaddr old = disposed[0];
        memmove(disposed, disposed + 1, (DISPOSED_KEPT - 1) * sizeof *disposed);
        ndisposed--;
        if (!in_window_list(old)) {
            free_window_parts(old);
            tb_DisposePtr(old);
        }
    }
    disposed[ndisposed++] = w;
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
    took_front();
}

void tb_HiliteWindow(gaddr w, int8_t flag) { set_hilite(w, flag != 0); }

static void bring_to_front(gaddr w)
{
    if (wlist() == w)
        return;
    unlink_window(w);
    link_behind(w, IN_FRONT);
    repaint_rgn_of(w);
}

void tb_BringToFront(gaddr w)
{
    bring_to_front(w);
    took_front();
}

void tb_SelectWindow(gaddr w)
{
    bring_to_front(w);
    update_active();
}

void tb_SendBehind(gaddr w, gaddr behind)
{
    /* No window, or one to go behind that isn't in the list (Snoop, whose
     * only window was hidden, sent FrontWindow's nil behind a pointer that
     * was no window): nothing to move, and nothing written through them. */
    if (!w)
        return;
    if (behind && behind != IN_FRONT) {
        gaddr x = wlist();
        while (x && x != behind)
            x = WindowRecord_nextWindow(x);
        if (!x || behind == w)
            return;
    }
    hrgn s;
    hrgn_init(&s);
    rgn_read(WindowRecord_strucRgn(w), &s);
    unlink_window(w);
    link_behind(w, behind);
    update_active();
    if (wvisible(w))
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
    if (tb_GetHandleSize(h) < (int32_t)n)
        return; /* a locked title that can't grow keeps the old one */
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

/* Collapse or expand a window of ours: the frame is laid out again, what
 * it uncovered repainted, and expanded content updated. */
static void toggle_collapse(gaddr w)
{
    if (our_wdef(w) < 0)
        return;
    hrgn old;
    hrgn_init(&old);
    rgn_read(WindowRecord_strucRgn(w), &old);
    bool on = !is_collapsed(w);
    set_collapsed(w, on);
    call_wdef(w, wCalcRgns, 0);
    hrgn now;
    hrgn_init(&now);
    rgn_read(WindowRecord_strucRgn(w), &now);
    hrgn_op(&old, &old, &now, RGN_UNION);
    repaint(&old);
    if (!on) {
        gaddr saved = qd_port();
        tb_SetPort(w);
        tb_InvalRect(w + GrafPort_portRect_off);
        tb_SetPort(saved);
    }
    hrgn_free(&old);
    hrgn_free(&now);
}

/* Window Manager 2.0's collapsing (CollapseWindow, IsWindowCollapsed):
 * only windows our definitions draw can collapse. */
bool wm_is_collapsed(gaddr w) { return is_collapsed(w); }

bool wm_set_collapsed(gaddr w, bool on)
{
    if (our_wdef(w) < 0)
        return false;
    if (is_collapsed(w) != on)
        toggle_collapse(w);
    return true;
}

/* GetWindowFeatures' bits: kWindowCanGrow 1, CanZoom 2, CanCollapse 4,
 * IsModal 8, CanGetWindowRegion 16, HasTitleBar 64. */
uint32_t wm_window_features(gaddr w)
{
    int var = varcode(w), def = our_wdef(w);
    uint32_t f = 16;
    if (def == 0) {
        if (has_grow(var))
            f |= 1;
        if (has_zoom(var))
            f |= 2;
        if (doc_variant(var))
            f |= 4;
        if (is_dialog_variant(var) || var == 5)
            f |= 8;
        if (!is_dialog_variant(var))
            f |= 64;
    } else if (def == 124) {
        f |= 4 | 64;
    }
    return f;
}

uint32_t ev_press_count(void);
bool ev_still_down(void);
void ev_wait_tick(void);

/* The visible window whose structure holds the global point, or nil. */
gaddr wm_window_at(uint32_t p)
{
    for (gaddr w = wlist(); w; w = WindowRecord_nextWindow(w)) {
        if (!wvisible(w))
            continue;
        hrgn s;
        hrgn_init(&s);
        rgn_read(WindowRecord_strucRgn(w), &s);
        bool in = hrgn_contains(&s, pt_h(p), pt_v(p));
        hrgn_free(&s);
        if (in)
            return w;
    }
    return 0;
}

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
        if (hit == wInCollapseBox) {
            /* Handled here, once per press (programs also call FindWindow
             * to set the cursor): a click in the collapse box. */
            static uint32_t last_press;
            uint32_t press = ev_press_count();
            if (ev_still_down() && press != last_press) {
                last_press = press;
                toggle_collapse(w);
            }
            wr32(wpp, 0);
            return inDesk;
        }
        switch (hit) {
        case wInContent: return inContent;
        case wInDrag: return inDrag;
        case wInGrow: return inGrow;
        case wInGoAway: return inGoAway;
        case wInZoomIn: return inZoomIn;
        default: return inZoomOut;
        }
    }
    /* A desk hook takes clicks on the desktop, through SystemClick. */
    return rd32(LM_DeskHook) ? inSysWindow : inDesk;
}

/* ---------------------------------------------------------------------- */
/* Update regions */

/* Whether a port is a window (in the window list). */
static bool is_window(gaddr p)
{
    for (gaddr w = wlist(); w; w = WindowRecord_nextWindow(w))
        if (w == p)
            return true;
    return false;
}

static void port_window_update(gaddr r_local_rgn_or_rect, bool is_rect, bool add)
{
    gaddr w = qd_port();
    /* A port that is not a window has no update region (the Window
     * Manager's own port, an offscreen one): nothing to do. */
    if (!w || !is_window(w))
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

static gaddr update_window; /* the window between BeginUpdate and EndUpdate */
static int update_dh, update_dv; /* and its port's origin then */

void tb_BeginUpdate(gaddr w)
{
    /* visRgn := visRgn ∩ updateRgn (local); the old visRgn is saved. */
    gaddr save = rd32(LM_SaveVisRgn);
    if (!save) {
        save = tb_NewRgn();
        wr32(LM_SaveVisRgn, save);
    }
    tb_CopyRgn(GrafPort_visRgn(w), save);
    update_window = w;
    port_offset(w, &update_dh, &update_dv);
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

/* The visRgn as it was, in the coordinates the port has now: programs
 * that scroll with SetOrigin may end the update at another origin
 * (Netscape Navigator). */
void tb_EndUpdate(gaddr w)
{
    gaddr save = rd32(LM_SaveVisRgn);
    if (!save)
        return;
    tb_CopyRgn(save, GrafPort_visRgn(w));
    int dh, dv;
    port_offset(w, &dh, &dv);
    if (w == update_window && (dh != update_dh || dv != update_dv))
        tb_OffsetRgn(GrafPort_visRgn(w), (int16_t)(dh - update_dh), (int16_t)(dv - update_dv));
    update_window = 0;
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
    if (wvisible(w)) /* a hidden window changes nothing on the screen */
        repaint(&old);
    if (!flag) { /* caller handles updating the new content itself */
        gaddr cur = qd_port();
        tb_SetPort(w);
        tb_ValidRect(w + GrafPort_portRect_off);
        tb_SetPort(cur);
    }
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
    uint8_t pat[8];
    gget(pat, LM_DragPattern, 8);
    while (ev_still_down()) {
        drag_hook();
        if (proc) /* the caller's action procedure, on every pass */
            guest_call(proc);
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
                qd_fill_region(wmgr_port(), &moved, pat, patXor);
            hrgn_copy(&moved, &shape);
            hrgn_offset(&moved, ndh, ndv);
            qd_fill_region(wmgr_port(), &moved, pat, patXor);
            shown = true;
            dh = ndh;
            dv = ndv;
        }
        ev_wait_tick();
    }
    if (shown)
        qd_fill_region(wmgr_port(), &moved, pat, patXor);
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
    /* A second click in the title bar without moving it collapses or
     * expands the window (WindowShade). */
    static gaddr last_w;
    static uint32_t last_ticks, last_p;
    uint32_t now = rd32(LM_Ticks);
    if (wp == last_w && now - last_ticks <= rd32(LM_DoubleTime) && abs(pt_h(p) - pt_h(last_p)) <= 4 &&
        abs(pt_v(p) - pt_v(last_p)) <= 4 && our_wdef(wp) >= 0) {
        last_w = 0;
        while (ev_still_down())
            ev_wait_tick();
        toggle_collapse(wp);
        tb_SelectWindow(wp);
        return;
    }
    gaddr slop = tb_NewPtr(8, false, false);
    qrect_put(slop, qrect_get(rp));
    int32_t d = tb_DragGrayRgn(WindowRecord_strucRgn(wp), p, rp, slop, 0, 0);
    tb_DisposePtr(slop);
    int16_t dh = pt_h((uint32_t)d), dv = pt_v((uint32_t)d);
    last_w = dh || dv ? 0 : wp;
    last_ticks = now;
    last_p = p;
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
        drag_hook();
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
        drag_hook();
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
    /* As the Window Manager did, with the WStateData in the dataHandle:
     * zooming out keeps the current place as the user state and goes to
     * the standard state; zooming in goes back to the user state. */
    qrect c = global_content(wp), to;
    gaddr z = zoom_state(wp);
    if (z) {
        if (part == inZoomOut) {
            qrect_put(rd32(z), c);
            to = qrect_get(rd32(z) + 8);
        } else {
            to = qrect_get(rd32(z));
        }
    } else {
        /* No states (a definition of the program's own that keeps none):
         * toggle between the current size and the screen. */
        to = std_state();
        if (rect_eq(c, to)) {
            int16_t top = (int16_t)(to.top + 20);
            to = (qrect){ top, 40, (int16_t)(top + g_screen_height / 2), (int16_t)(40 + g_screen_width / 2) };
        }
    }
    tb_MoveWindow(wp, to.left, to.top, front);
    tb_SizeWindow(wp, (int16_t)(to.right - to.left), (int16_t)(to.bottom - to.top), 1);
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
/* SaveOld and DrawNew, for a program that moves or resizes a window by
 * changing its regions itself (HyperCard 2.4 going to a smaller stack):
 * what the window covered before and no longer covers, and what changed
 * between its old and new content, is repainted. */
void tb_SaveOld(gaddr w)
{
    rgn_read(WindowRecord_strucRgn(w), &old_struc);
    rgn_read(WindowRecord_contRgn(w), &old_cont);
    old_window = w;
}

void tb_DrawNew(gaddr w, int8_t flag)
{
    (void)flag;
    if (w != old_window) {
        repaint_rgn_of(w);
        return;
    }
    old_window = 0;
    hrgn area, now;
    hrgn_init(&area);
    hrgn_init(&now);
    rgn_read(WindowRecord_strucRgn(w), &now);
    hrgn_op(&area, &old_struc, &now, RGN_XOR);
    rgn_read(WindowRecord_contRgn(w), &now);
    hrgn_op(&now, &old_cont, &now, RGN_XOR);
    hrgn_op(&area, &area, &now, RGN_UNION);
    repaint(&area);
    hrgn_free(&area);
    hrgn_free(&now);
}
/* ---------------------------------------------------------------------- */
/* Auxiliary window records (AuxWinHead): handles of awNext, awOwner,
 * awCTable, dialogCItem, awFlags, awReserved, awRefCon, one for each window
 * with colours of its own, and last the default one (no owner) with the
 * default window colour table. */
enum { AUX_SIZE = 28, AW_NEXT = 0, AW_OWNER = 4, AW_CTABLE = 8 };

static gaddr aux_of(gaddr w)
{
    for (gaddr h = rd32(LM_AuxWinHead); h && rd32(h); h = rd32(rd32(h) + AW_NEXT))
        if (rd32(rd32(h) + AW_OWNER) == w)
            return h;
    return 0;
}

/* The default: content white, frame, text and highlight black, title bar
 * white (wContentColor .. wTitleBarColor). */
static void make_default_aux(void)
{
    if (aux_of(0))
        return;
    static const uint16_t rgb[5][3] = { { 0xFFFF, 0xFFFF, 0xFFFF }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 },
                                        { 0xFFFF, 0xFFFF, 0xFFFF } };
    gaddr ct = tb_NewHandle(8 + 5 * 8, true, true);
    gaddr t = rd32(ct);
    wr16(t + 6, 4); /* ctSize */
    for (int i = 0; i < 5; i++) {
        wr16(t + 8 + (uint32_t)i * 8, (uint16_t)i);
        for (int k = 0; k < 3; k++)
            wr16(t + 8 + (uint32_t)i * 8 + 2 + (uint32_t)k * 2, rgb[i][k]);
    }
    gaddr h = tb_NewHandle(AUX_SIZE, true, true);
    wr32(rd32(h) + AW_CTABLE, ct);
    wr32(LM_AuxWinHead, h);
}

int8_t tb_GetAuxWin(gaddr w, gaddr hp)
{
    gaddr h = w ? aux_of(w) : 0;
    wr32(hp, h ? h : aux_of(0));
    return h != 0;
}

void tb_SetWinColor(gaddr w, gaddr ctab)
{
    gaddr h = aux_of(w);
    if (!h) {
        h = tb_NewHandle(AUX_SIZE, false, true);
        if (!h)
            return;
        wr32(rd32(h) + AW_OWNER, w);
        wr32(rd32(h) + AW_NEXT, rd32(LM_AuxWinHead));
        wr32(LM_AuxWinHead, h);
    }
    wr32(rd32(h) + AW_CTABLE, ctab);
    if (w && wvisible(w))
        repaint_rgn_of(w);
}

static void remove_aux(gaddr w)
{
    gaddr prev = 0;
    for (gaddr h = rd32(LM_AuxWinHead); h && rd32(h); prev = h, h = rd32(rd32(h) + AW_NEXT)) {
        if (rd32(rd32(h) + AW_OWNER) != w)
            continue;
        gaddr next = rd32(rd32(h) + AW_NEXT);
        if (prev)
            wr32(rd32(prev) + AW_NEXT, next);
        else
            wr32(LM_AuxWinHead, next);
        tb_DisposeHandle(h);
        return;
    }
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
    if (rd32(LM_WindowList)) {
        /* Called again with windows open (Installer 4's licence viewer does
           this after making its dialog, and goes on drawing in it): the
           windows, the screen and the current port stay. */
        return;
    }
    wr32(LM_WindowList, 0);
    active_window = 0;
    old_window = 0;
    floats_hidden = false;
    float_layer = false;
    nhidden_floats = 0;
    ndisposed = 0;
    wr32(LM_CurActivate, 0);
    wr32(LM_CurDeactive, 0);
    gput(LM_DeskPattern, qd_pat_gray, 8);
    gput(LM_DragPattern, qd_pat_gray, 8);
    make_default_aux();
    wr32(LM_DeskHook, 0);
    wr32(LM_DragHook, 0);
    int mbar = (int16_t)rd16(LM_MBarHeight) ? (int16_t)rd16(LM_MBarHeight) : 20;
    wr16(LM_MBarHeight, (uint16_t)mbar);

    set_desk(mbar);
    hrgn gray, bar;
    hrgn_init(&gray);
    hrgn_init(&bar);
    gray_rgn(&gray);
    hrgn_rect(&bar, (qrect){ 0, 0, (int16_t)mbar, (int16_t)g_screen_width });

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
    /* As in the ROM, the Window Manager port is left current: with Color
       QuickDraw the colour one, whose pixel map programs read the screen's
       depth from (Sokoban 3D: GetGWorld, GetGWorldPixMap). */
    tb_SetPort(rd32(LM_WMgrCPort));
}

int16_t tb_GetWVariant(gaddr w) { return w ? varcode(w) : 0; }

/* ---------------------------------------------------------------------- */
/* Mac OS 8's additions (WindowsLib): classes, features and regions of
 * the windows our definitions draw, collapsing, floating windows (those
 * WDEF 124 draws), content colours, properties, and proxies (none). */

enum {
    kAlertWindowClass = 1, kMovableAlertWindowClass, kModalWindowClass, kMovableModalWindowClass,
    kFloatingWindowClass, kDocumentWindowClass, kUtilityWindowClass = 8, kHelpWindowClass = 10, kSheetWindowClass,
    kToolbarWindowClass, kPlainWindowClass, kSheetAlertWindowClass = 15, kAltPlainWindowClass,
};
enum {
    kWindowCloseBoxAttribute = 1 << 0, kWindowHorizontalZoomAttribute = 1 << 1, kWindowVerticalZoomAttribute = 1 << 2,
    kWindowCollapseBoxAttribute = 1 << 3, kWindowResizableAttribute = 1 << 4, kWindowSideTitlebarAttribute = 1 << 5,
};
enum {
    kWindowCanGrow = 1 << 0, kWindowCanZoom = 1 << 1, kWindowCanCollapse = 1 << 2, kWindowIsModal = 1 << 3,
    kWindowCanGetWindowRegion = 1 << 4, kWindowIsAlert = 1 << 5, kWindowHasTitleBar = 1 << 6,
};
enum {
    kWindowTitleBarRgn = 0, kWindowTitleTextRgn, kWindowCloseBoxRgn, kWindowZoomBoxRgn, kWindowDragRgn = 5,
    kWindowGrowRgn, kWindowCollapseBoxRgn, kWindowStructureRgn = 32, kWindowContentRgn, kWindowUpdateRgn,
    kWindowGlobalPortRgn = 40,
};
enum {
    errInvalidWindowPtr = -5600, errUnsupportedWindowAttributesForClass = -5601, errWindowDoesNotHaveProxy = -5602,
    errWindowPropertyNotFound = -5604, errUnrecognizedWindowClass = -5605,
};


gaddr tb_FrontNonFloatingWindow(void)
{
    gaddr ghost = rd32(LM_GhostWindow);
    for (gaddr w = wlist(); w; w = WindowRecord_nextWindow(w))
        if (wvisible(w) && w != ghost && !is_floating(w))
            return w;
    return 0;
}

int8_t tb_IsValidWindowPtr(gaddr w) { return w && is_window(w); }

int16_t tb_GetWindowClass(gaddr w, gaddr outClass)
{
    if (!is_window(w))
        return errInvalidWindowPtr;
    int var = varcode(w), def = our_wdef(w);
    uint32_t cls = carbonwm_class(w);
    if (cls) {
        if (outClass)
            wr32(outClass, cls);
        return noErr;
    }
    cls = kDocumentWindowClass;
    if (def == 124)
        cls = kFloatingWindowClass;
    else if (def == 0 && (var == 1 || var == 3))
        cls = WindowRecord_windowKind(w) == dialogKind ? kModalWindowClass : kPlainWindowClass;
    else if (def == 0 && var == 2)
        cls = kPlainWindowClass;
    else if (def == 0 && var == 5)
        cls = kMovableModalWindowClass;
    if (outClass)
        wr32(outClass, cls);
    return noErr;
}

static uint32_t attributes_of(gaddr w)
{
    int var = varcode(w), def = our_wdef(w);
    uint32_t a = WindowRecord_goAwayFlag(w) ? kWindowCloseBoxAttribute : 0;
    if (def == 0 && doc_variant(var)) {
        a |= kWindowCollapseBoxAttribute;
        if (has_zoom(var))
            a |= kWindowHorizontalZoomAttribute | kWindowVerticalZoomAttribute;
        if (has_grow(var))
            a |= kWindowResizableAttribute;
    } else if (def == 124) {
        a |= kWindowCollapseBoxAttribute;
        if (float_zoom(var))
            a |= kWindowHorizontalZoomAttribute | kWindowVerticalZoomAttribute;
        if (float_grow(var))
            a |= kWindowResizableAttribute;
        if (float_side(var))
            a |= kWindowSideTitlebarAttribute;
    }
    return a;
}

int16_t tb_GetWindowAttributes(gaddr w, gaddr outAttributes)
{
    if (!is_window(w))
        return errInvalidWindowPtr;
    if (outAttributes)
        wr32(outAttributes, attributes_of(w) | carbonwm_attributes(w));
    return noErr;
}

/* GetWindowFeatures(w, &features): what its definition does; nothing
 * for the program's own definitions. */
int16_t tb_GetWindowFeatures(gaddr w, gaddr outFeatures)
{
    if (!is_window(w))
        return errInvalidWindowPtr;
    int var = varcode(w), def = our_wdef(w);
    uint32_t a = attributes_of(w), f = 0;
    if (def >= 0) {
        f = kWindowCanGetWindowRegion;
        if (a & kWindowResizableAttribute)
            f |= kWindowCanGrow;
        if (a & kWindowHorizontalZoomAttribute)
            f |= kWindowCanZoom;
        if (a & kWindowCollapseBoxAttribute)
            f |= kWindowCanCollapse;
        if (def == 124 || doc_variant(var) || var == 5)
            f |= kWindowHasTitleBar;
        if (def == 0 && (var == 1 || var == 5))
            f |= kWindowIsModal;
    }
    if (outFeatures)
        wr32(outFeatures, f);
    return noErr;
}

/* The bounding box of the points in the title bar where the definition
 * reports a part (its hit test, the window taken as active). */
static qrect part_box(gaddr w, qrect bar, int part)
{
    qrect box = { 0, 0, 0, 0 };
    bool any = false;
    uint8_t hil = WindowRecord_hilited(w);
    WindowRecord_set_hilited(w, 1);
    for (int v = bar.top; v < bar.bottom; v++)
        for (int h = bar.left; h < bar.right; h++) {
            if (call_wdef(w, wHit, (int32_t)((uint32_t)(uint16_t)v << 16 | (uint16_t)h)) != part)
                continue;
            if (!any)
                box = (qrect){ (int16_t)v, (int16_t)h, (int16_t)(v + 1), (int16_t)(h + 1) };
            box.top = (int16_t)(v < box.top ? v : box.top);
            box.left = (int16_t)(h < box.left ? h : box.left);
            box.bottom = (int16_t)(v + 1 > box.bottom ? v + 1 : box.bottom);
            box.right = (int16_t)(h + 1 > box.right ? h + 1 : box.right);
            any = true;
        }
    WindowRecord_set_hilited(w, hil);
    return box;
}

/* GetWindowRegion(w, code, rgn): global coordinates. */
int16_t tb_GetWindowRegion(gaddr w, uint16_t code, gaddr rgn)
{
    if (!is_window(w) || !rgn)
        return errInvalidWindowPtr;
    hrgn r, s, c;
    hrgn_init(&r);
    hrgn_init(&s);
    hrgn_init(&c);
    rgn_read(WindowRecord_strucRgn(w), &s);
    rgn_read(WindowRecord_contRgn(w), &c);
    qrect sb = hrgn_bbox(&s), cb = hrgn_bbox(&c);
    /* the title bar: the frame above the content, or beside it */
    qrect bar = cb.top > sb.top ? (qrect){ sb.top, sb.left, cb.top, sb.right }
              : cb.left > sb.left ? (qrect){ sb.top, sb.left, sb.bottom, cb.left }
                                  : (qrect){ 0, 0, 0, 0 };
    if (is_collapsed(w))
        bar = sb;
    int16_t e = noErr;
    switch (code) {
    case kWindowStructureRgn: hrgn_op(&r, &s, &s, RGN_UNION); break;
    case kWindowContentRgn: hrgn_op(&r, &c, &c, RGN_UNION); break;
    case kWindowUpdateRgn: rgn_read(WindowRecord_updateRgn(w), &r); break;
    case kWindowGlobalPortRgn: hrgn_rect(&r, global_content(w)); break;
    case kWindowTitleBarRgn:
    case kWindowDragRgn:
    case kWindowTitleTextRgn:
        if (our_wdef(w) >= 0 && bar.bottom > bar.top)
            hrgn_rect(&r, bar);
        break;
    case kWindowCloseBoxRgn:
    case kWindowZoomBoxRgn:
    case kWindowCollapseBoxRgn:
        if (our_wdef(w) >= 0 && bar.bottom > bar.top)
            hrgn_rect(&r, part_box(w, bar, code == kWindowCloseBoxRgn ? wInGoAway
                                          : code == kWindowZoomBoxRgn  ? wInZoomIn
                                                                       : wInCollapseBox));
        break;
    case kWindowGrowRgn:
        if (attributes_of(w) & kWindowResizableAttribute && !is_collapsed(w))
            hrgn_rect(&r, (qrect){ (int16_t)(cb.bottom - 15), (int16_t)(cb.right - 15), cb.bottom, cb.right });
        break;
    default: e = paramErr; break;
    }
    rgn_write(rgn, &r);
    hrgn_free(&r);
    hrgn_free(&s);
    hrgn_free(&c);
    return e;
}

int8_t tb_IsWindowCollapsable(gaddr w) { return is_window(w) && (attributes_of(w) & kWindowCollapseBoxAttribute); }
int8_t tb_IsWindowCollapsed(gaddr w) { return is_window(w) && is_collapsed(w); }

int16_t tb_CollapseWindow(gaddr w, int8_t collapse)
{
    if (!is_window(w))
        return errInvalidWindowPtr;
    if (!tb_IsWindowCollapsable(w))
        return errUnsupportedWindowAttributesForClass;
    if (is_collapsed(w) != (collapse != 0))
        toggle_collapse(w);
    return noErr;
}

int16_t tb_CollapseAllWindows(int8_t collapse)
{
    for (gaddr w = wlist(); w; w = WindowRecord_nextWindow(w))
        if (wvisible(w) && tb_IsWindowCollapsable(w) && is_collapsed(w) != (collapse != 0))
            toggle_collapse(w);
    return noErr;
}

/* The procID a class and attributes call for, or -1. */
int16_t wm_class_proc(uint32_t windowClass, uint32_t attributes)
{
    bool zoom = attributes & (kWindowHorizontalZoomAttribute | kWindowVerticalZoomAttribute);
    bool grow = attributes & kWindowResizableAttribute;
    switch (windowClass) {
    case kDocumentWindowClass: return (int16_t)(zoom ? (grow ? 8 : 12) : (grow ? 0 : 4));
    /* (CarbonLib's utility windows float) */
    case kFloatingWindowClass:
    case kUtilityWindowClass:
        return (int16_t)(124 * 16 + (grow ? 2 : 0) + (zoom ? 4 : 0) + (attributes & kWindowSideTitlebarAttribute ? 8 : 0));
    /* (sheets, which CarbonLib shows app-modal, are modal dialog boxes) */
    case kAlertWindowClass:
    case kModalWindowClass:
    case kSheetWindowClass:
    case kSheetAlertWindowClass: return 1;
    case kMovableAlertWindowClass:
    case kMovableModalWindowClass: return 5;
    /* (help tags, toolbars and the other plain windows: plain boxes) */
    case kPlainWindowClass:
    case kHelpWindowClass:
    case kToolbarWindowClass:
    case kAltPlainWindowClass: return 2;
    default: return -1;
    }
}

/* CreateNewWindow(class, attributes, contentBounds, &window): a colour
 * window of the definition the class and attributes call for, hidden. */
int16_t tb_CreateNewWindow(uint32_t windowClass, uint32_t attributes, gaddr bounds, gaddr outWindow)
{
    if (!bounds || !outWindow)
        return paramErr;
    int16_t proc = wm_class_proc(windowClass, attributes);
    if (proc < 0)
        return errUnrecognizedWindowClass;
    gaddr title = tb_NewPtr(2, true, true);
    gaddr w = new_window(0, qrect_get(bounds), title, false, proc, IN_FRONT, (attributes & kWindowCloseBoxAttribute) != 0,
                         0, true);
    tb_DisposePtr(title);
    if (w && windowClass != kDocumentWindowClass && windowClass != kFloatingWindowClass &&
        windowClass != kUtilityWindowClass)
        WindowRecord_set_windowKind(w, dialogKind);
    if (w)
        carbonwm_set_class(w, windowClass, attributes);
    wr32(outWindow, w);
    return w ? noErr : memFullErr;
}

/* Floating windows hidden together (those that were showing), and shown
 * again. */
int16_t tb_InitFloatingWindows(void)
{
    float_layer = true;
    /* Floating windows already made come to the front, in their order. */
    gaddr floats[64];
    int n = 0;
    for (gaddr w = wlist(); w && n < 64; w = WindowRecord_nextWindow(w))
        if (is_floating(w))
            floats[n++] = w;
    for (int i = n - 1; i >= 0; i--) {
        unlink_window(floats[i]);
        link_behind(floats[i], IN_FRONT);
    }
    if (n)
        repaint_all();
    update_active();
    return noErr;
}
int8_t tb_AreFloatingWindowsVisible(void) { return !floats_hidden; }

static int16_t show_floats(bool show)
{
    if (floats_hidden == !show)
        return noErr;
    floats_hidden = !show;
    if (show) {
        for (int i = 0; i < nhidden_floats; i++)
            if (is_window(hidden_floats[i]))
                tb_ShowHide(hidden_floats[i], true);
        nhidden_floats = 0;
        return noErr;
    }
    for (gaddr w = wlist(); w; w = WindowRecord_nextWindow(w))
        if (is_floating(w) && wvisible(w) && nhidden_floats < 32)
            hidden_floats[nhidden_floats++] = w;
    for (int i = 0; i < nhidden_floats; i++)
        tb_ShowHide(hidden_floats[i], false);
    return noErr;
}

int16_t tb_ShowFloatingWindows(void) { return show_floats(true); }
int16_t tb_HideFloatingWindows(void) { return show_floats(false); }

/* Content colours: the window port's background, and what Get answers. */
static struct {
    gaddr w;
    uint16_t rgb[3];
} content_colors[32];

int16_t tb_SetWindowContentColor(gaddr w, gaddr color)
{
    if (!is_window(w) || !color)
        return errInvalidWindowPtr;
    int slot = -1;
    for (int i = 0; i < 32 && slot < 0; i++)
        if (content_colors[i].w == w || !content_colors[i].w || !is_window(content_colors[i].w))
            slot = i;
    if (slot >= 0) {
        content_colors[slot].w = w;
        for (int k = 0; k < 3; k++)
            content_colors[slot].rgb[k] = rd16(color + (uint32_t)k * 2);
    }
    gaddr saved = qd_port();
    tb_SetPort(w);
    tb_RGBBackColor(color);
    tb_SetPort(saved);
    return noErr;
}

int16_t tb_GetWindowContentColor(gaddr w, gaddr color)
{
    if (!is_window(w) || !color)
        return errInvalidWindowPtr;
    for (int i = 0; i < 32; i++)
        if (content_colors[i].w == w) {
            for (int k = 0; k < 3; k++)
                wr16(color + (uint32_t)k * 2, content_colors[i].rgb[k]);
            return noErr;
        }
    /* the window's colour table's content colour, or white */
    gaddr h = aux_of(w) ? aux_of(w) : aux_of(0);
    gaddr ct = h ? rd32(rd32(h) + AW_CTABLE) : 0;
    for (int k = 0; k < 3; k++)
        wr16(color + (uint32_t)k * 2, ct && rd32(ct) ? rd16(rd32(ct) + 8 + 2 + (uint32_t)k * 2) : 0xFFFF);
    return noErr;
}

/* Properties: a program's data kept with a window, by creator and tag. */
typedef struct {
    gaddr w;
    uint32_t creator, tag, size;
    uint8_t *data;
} window_property;
static window_property *props;
static int nprops, maxprops;

static window_property *find_property(gaddr w, uint32_t creator, uint32_t tag)
{
    for (int i = 0; i < nprops; i++)
        if (props[i].w == w && props[i].creator == creator && props[i].tag == tag)
            return &props[i];
    return NULL;
}

int16_t tb_SetWindowProperty(gaddr w, uint32_t creator, uint32_t tag, uint32_t size, gaddr buffer)
{
    if (!is_window(w))
        return errInvalidWindowPtr;
    window_property *p = find_property(w, creator, tag);
    if (!p) {
        if (nprops == maxprops) {
            int n = maxprops ? maxprops * 2 : 16;
            window_property *more = realloc(props, sizeof *props * (size_t)n);
            if (!more)
                return memFullErr;
            props = more;
            maxprops = n;
        }
        p = &props[nprops++];
        *p = (window_property){ w, creator, tag, 0, NULL };
    }
    uint8_t *d = malloc(size ? size : 1);
    if (!d)
        return memFullErr;
    if (size && buffer)
        gget(d, buffer, size);
    free(p->data);
    p->data = d;
    p->size = size;
    return noErr;
}

int16_t tb_GetWindowProperty(gaddr w, uint32_t creator, uint32_t tag, uint32_t bufferSize, gaddr actualSize,
                             gaddr buffer)
{
    window_property *p = find_property(w, creator, tag);
    if (!p)
        return errWindowPropertyNotFound;
    uint32_t n = p->size < bufferSize ? p->size : bufferSize;
    if (buffer && n)
        gput(buffer, p->data, n);
    if (actualSize)
        wr32(actualSize, n);
    return noErr;
}

int16_t tb_GetWindowPropertySize(gaddr w, uint32_t creator, uint32_t tag, gaddr size)
{
    window_property *p = find_property(w, creator, tag);
    if (!p)
        return errWindowPropertyNotFound;
    if (size)
        wr32(size, p->size);
    return noErr;
}

int16_t tb_RemoveWindowProperty(gaddr w, uint32_t creator, uint32_t tag)
{
    window_property *p = find_property(w, creator, tag);
    if (!p)
        return errWindowPropertyNotFound;
    free(p->data);
    *p = props[--nprops];
    return noErr;
}

/* A window's properties go with it. */
static void forget_properties(gaddr w)
{
    for (int i = 0; i < nprops;)
        if (props[i].w == w) {
            free(props[i].data);
            props[i] = props[--nprops];
        } else {
            i++;
        }
}

/* Proxies (the title bar's document icon) are not shown: there is none
 * to get, and setting one is taken. */
int16_t tb_SetWindowProxyFSSpec(gaddr w, gaddr spec) { return is_window(w) ? noErr : errInvalidWindowPtr; }
int16_t tb_GetWindowProxyFSSpec(gaddr w, gaddr spec) { return errWindowDoesNotHaveProxy; }
int16_t tb_SetWindowProxyAlias(gaddr w, gaddr alias) { return is_window(w) ? noErr : errInvalidWindowPtr; }
int16_t tb_GetWindowProxyAlias(gaddr w, gaddr alias) { return errWindowDoesNotHaveProxy; }
int16_t tb_SetWindowProxyCreatorAndType(gaddr w, uint32_t creator, uint32_t type, int16_t vref)
{
    return is_window(w) ? noErr : errInvalidWindowPtr;
}
int16_t tb_RemoveWindowProxy(gaddr w) { return is_window(w) ? noErr : errInvalidWindowPtr; }
int16_t tb_SetWindowProxyIcon(gaddr w, gaddr icon) { return is_window(w) ? noErr : errInvalidWindowPtr; }
int16_t tb_GetWindowProxyIcon(gaddr w, gaddr icon) { return errWindowDoesNotHaveProxy; }

/* The modified flag (shown by nothing here). */
static gaddr modified[32];

int16_t tb_SetWindowModified(gaddr w, int8_t on)
{
    if (!is_window(w))
        return errInvalidWindowPtr;
    int at = -1, free_slot = -1;
    for (int i = 0; i < 32; i++) {
        if (modified[i] == w)
            at = i;
        else if (free_slot < 0 && (!modified[i] || !is_window(modified[i])))
            free_slot = i;
    }
    if (!on && at >= 0)
        modified[at] = 0;
    else if (on && at < 0 && free_slot >= 0)
        modified[free_slot] = w;
    return noErr;
}

int8_t tb_IsWindowModified(gaddr w)
{
    for (int i = 0; i < 32; i++)
        if (w && modified[i] == w)
            return 1;
    return 0;
}

/* ---------------------------------------------------------------------- */
/* For CarbonLib's Window Manager calls (carbon_wm.c) */

bool wm_is_window(gaddr w) { return w && is_window(w); }

/* The FindWindow part code a global point has in a window (inDesk when
 * the window's definition says it's not in it). */
int16_t wm_hit_part(gaddr w, uint32_t p)
{
    if (!wvisible(w))
        return inDesk;
    hrgn s;
    hrgn_init(&s);
    rgn_read(WindowRecord_strucRgn(w), &s);
    bool in = hrgn_contains(&s, pt_h(p), pt_v(p));
    hrgn_free(&s);
    int32_t hit = in ? call_wdef(w, wHit, (int32_t)p) : wNoHit;
    switch (hit) {
    case wNoHit: return inDesk;
    case wInContent: return inContent;
    case wInDrag: return inDrag;
    case wInGrow: return inGrow;
    case wInGoAway: return inGoAway;
    case wInZoomIn: return inZoomIn;
    case wInZoomOut: return inZoomOut;
    case wInCollapseBox: return 11; /* inCollapseBox */
    default: return inDrag;
    }
}

/* A window's zoom states (its WStateData handle), or 0; the standard
 * state a window zooms to. */
gaddr wm_zoom_state(gaddr w) { return zoom_state(w); }
qrect wm_std_state(void) { return std_state(); }

/* A window given another definition (when defproc isn't 0, the old one
 * told it's disposed of and the new one that it's new) or variation (when
 * var isn't -1): its regions worked out again and the screen redrawn. */
void wm_redefine(gaddr w, gaddr defproc, int16_t var)
{
    hrgn old;
    hrgn_init(&old);
    rgn_read(WindowRecord_strucRgn(w), &old);
    gaddr saved = qd_port();
    if (defproc) {
        call_wdef(w, wDispose, 0);
        WindowRecord_set_windowDefProc(w, defproc);
    }
    if (var >= 0)
        set_varcode(w, var);
    if (defproc)
        call_wdef(w, wNew, 0);
    call_wdef(w, wCalcRgns, 0);
    tb_SetPort(saved);
    if (wvisible(w)) {
        hrgn now;
        hrgn_init(&now);
        rgn_read(WindowRecord_strucRgn(w), &now);
        hrgn_op(&old, &old, &now, RGN_UNION);
        repaint(&old);
        hrgn_free(&now);
    } else {
        calc_all_vis();
    }
    hrgn_free(&old);
}
