/*
 * Control Manager, with native CDEF 0 (buttons, check boxes, radio buttons),
 * CDEF 1 (scroll bars) and CDEF 63 (pop-up menus), and Control Manager
 * 2.0 (Mac OS 8's): embedding, activation, keyboard focus, data access
 * and the CDEF messages that go with them.  The Appearance CDEFs are in
 * cdefs_appearance.c.
 *
 * Each control has an entry in a table of ours (its variant, its CDEF, its
 * embedder, what it was before its embedder hid or deactivated it, its
 * font style), and each window with a root control or a focus has one too.
 * The standard controls draw in Platinum for appearance clients and in
 * windows that ask for theme controls (dialogs with 'dlgx' flags).
 */
#include <stdlib.h>
#include <string.h>

#include "qd_internal.h"
#include "tb_internal.h"

enum { drawCntl = 0, testCntl, calcCRgns, initCntl, dispCntl, posCntl, thumbCntl, dragCntl, autoTrack };
enum { inButton = 10, inCheckBox = 11, inUpButton = 20, inDownButton = 21, inPageUp = 22, inPageDown = 23,
       inThumb = 129 };
enum { patCopy = 8, patXor = 10, patBic = 11 };
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
};
enum {
    errMessageNotSupported = -30580, errDataNotSupported = -30581, errControlDoesntSupportFocus = -30582,
    errUnknownControl = -30584, errCouldntSetFocus = -30585, errNoRootControl = -30586,
    errRootAlreadyExists = -30587, errControlIsNotEmbedder = -30590, errCantEmbedIntoSelf = -30594,
    errCantEmbedRoot = -30595,
};
#define NEW_MSG_OK 0x206F6B20 /* ' ok ' */

bool ev_still_down(void);
uint32_t ev_mouse_global(void);
void ev_wait_tick(void);

static gaddr cp(gaddr ch) { return rd32(ch); }
static qrect crect(gaddr ch) { return qrect_get(cp(ch) + ControlRecord_contrlRect_off); }
static int16_t cvalue(gaddr ch) { return ControlRecord_contrlValue(cp(ch)); }
static int16_t cmin(gaddr ch) { return ControlRecord_contrlMin(cp(ch)); }
static int16_t cmax(gaddr ch) { return ControlRecord_contrlMax(cp(ch)); }
static uint8_t chilite(gaddr ch) { return ControlRecord_contrlHilite(cp(ch)); }
static gaddr cowner(gaddr ch) { return ControlRecord_contrlOwner(cp(ch)); }

/* ---------------------------------------------------------------------- */
/* Our table of controls and windows */

static ctl_info *ctls;
static int nctls, cap_ctls;
static uint32_t embed_seq;

ctl_info *ctl_find(gaddr ch)
{
    if (!ch)
        return NULL;
    for (int i = 0; i < nctls; i++)
        if (ctls[i].ch == ch)
            return &ctls[i];
    return NULL;
}

/* A new control's entry (a handle used before may still have one). */
static ctl_info *ctl_add(gaddr ch)
{
    ctl_info *c = ctl_find(ch);
    if (c) {
        free(c->ap);
        c->ap = NULL;
    }
    for (int i = 0; i < nctls && !c; i++)
        if (!ctls[i].ch)
            c = &ctls[i];
    if (!c) {
        if (nctls == cap_ctls) {
            cap_ctls = cap_ctls ? cap_ctls * 2 : 256;
            ctls = realloc(ctls, (size_t)cap_ctls * sizeof *ctls);
        }
        c = &ctls[nctls++];
    }
    memset(c, 0, sizeof *c);
    c->ch = ch;
    c->new_msgs = -1;
    return c;
}

static int16_t cvariant(gaddr ch)
{
    ctl_info *c = ctl_find(ch);
    return c ? c->var : 0;
}

#define MAX_WINDOWS 64
static struct {
    gaddr w, root, focus;
    int16_t focus_part;
    bool themed; /* standard controls drawn in Platinum (a 'dlgx' dialog's) */
} wins[MAX_WINDOWS];

static int win_index(gaddr w, bool create)
{
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (wins[i].w == w)
            return i;
    if (!create)
        return -1;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (!wins[i].w) {
            memset(&wins[i], 0, sizeof wins[i]);
            wins[i].w = w;
            return i;
        }
    return -1;
}

void controls_reset(void)
{
    for (int i = 0; i < nctls; i++)
        free(ctls[i].ap);
    free(ctls);
    ctls = NULL;
    nctls = cap_ctls = 0;
    memset(wins, 0, sizeof wins);
}

/* A window goes away: forget what we kept for it and its controls (which
 * stay as they are, as CloseWindow leaves them). */
void controls_forget_window(gaddr w)
{
    int i = win_index(w, false);
    if (i >= 0)
        wins[i].w = 0;
    int k = 0;
    for (gaddr x = WindowRecord_controlList(w); x && rd32(x) && k < 4096; x = ControlRecord_nextControl(cp(x)), k++) {
        ctl_info *c = ctl_find(x);
        if (c) {
            free(c->ap);
            c->ap = NULL;
            c->ch = 0;
        }
    }
}

gaddr ctl_window_root(gaddr w)
{
    int i = win_index(w, false);
    return i >= 0 ? wins[i].root : 0;
}

void ctl_set_window_themed(gaddr w, bool themed)
{
    int i = win_index(w, true);
    if (i >= 0)
        wins[i].themed = themed;
}

/* Whether the standard controls draw in Platinum: for appearance clients
 * and in themed windows, unless a Kaleidoscope theme or the host draws
 * them; in colour (the current port is the control's). */
bool ctl_platinum(gaddr ch)
{
    if (g_appearance != MACHINE_APPEARANCE_BW || !ap_in_color())
        return false;
    int i = win_index(cowner(ch), false);
    return appearance_client() || (i >= 0 && wins[i].themed);
}

/* The embedded controls of ch in the order they were embedded. */
int ctl_subcontrols(gaddr ch, gaddr *out, int max)
{
    int n = 0;
    for (int i = 0; i < nctls; i++)
        if (ctls[i].ch && ctls[i].parent == ch && n < max)
            out[n++] = ctls[i].ch;
    /* by seq */
    for (int a = 1; a < n; a++)
        for (int b = a; b > 0; b--) {
            ctl_info *x = ctl_find(out[b - 1]), *y = ctl_find(out[b]);
            if (x->seq <= y->seq)
                break;
            gaddr t = out[b - 1];
            out[b - 1] = out[b];
            out[b] = t;
        }
    return n;
}

static int depth_of(gaddr ch)
{
    int d = 0;
    for (ctl_info *c = ctl_find(ch); c && c->parent && d < 64; c = ctl_find(c->parent))
        d++;
    return d;
}

static int32_t call_cdef(gaddr ch, int16_t msg, int32_t param)
{
    gaddr h = ControlRecord_contrlDefProc(cp(ch));
    if (!h || !rd32(h))
        return 0;
    cpu68k *c = g_cpu;
    cpu_push32(c, 0);
    cpu_push16(c, (uint16_t)cvariant(ch));
    cpu_push32(c, ch);
    cpu_push16(c, (uint16_t)msg);
    cpu_push32(c, (uint32_t)param);
    tb_HLock(h);
    tb_HLock(ch);
    guest_call(rd32(h));
    tb_HUnlock(ch);
    tb_HUnlock(h);
    return (int32_t)cpu_pop32(c);
}

/* Whether the CDEF takes Control Manager 2.0's messages: ours do, others
 * say so to kControlMsgTestNewMsgSupport. */
static bool new_messages(gaddr ch)
{
    ctl_info *c = ctl_find(ch);
    if (!c)
        return false;
    if (c->new_msgs < 0)
        c->new_msgs = call_cdef(ch, kControlMsgTestNewMsgSupport, 0) == NEW_MSG_OK;
    return c->new_msgs;
}

/* A Control Manager 2.0 message, to a CDEF that takes them. */
int32_t ctl_call(gaddr ch, int16_t msg, int32_t param)
{
    if (msg >= 13 && !new_messages(ch))
        return 0;
    return call_cdef(ch, msg, param);
}

static uint32_t features(gaddr ch)
{
    return new_messages(ch) ? (uint32_t)call_cdef(ch, kControlMsgGetFeatures, 0) : 0;
}

/* ---------------------------------------------------------------------- */
/* Drawing helpers */

static gaddr tmp_rect(qrect r)
{
    gaddr p = tb_NewPtr(8, false, false);
    qrect_put(p, r);
    return p;
}

static void fill(qrect r, const uint8_t pat[8], int mode)
{
    hrgn g;
    hrgn_init(&g);
    hrgn_rect(&g, r);
    qd_fill_region(qd_port(), &g, pat, mode);
    hrgn_free(&g);
}

static void title_text(gaddr ch, uint8_t *buf, int *n)
{
    gaddr t = cp(ch) + ControlRecord_contrlTitle_off;
    *n = rd8(t);
    gget(buf, t + 1, (uint32_t)*n);
}

/* The control's font: the system font unless the variant asks for the
 * window's (8), then what SetControlFontStyle gave it. */
void ctl_font_begin(gaddr ch, ctl_font_saved *s)
{
    gaddr port = qd_port();
    s->font = GrafPort_txFont(port);
    s->size = GrafPort_txSize(port);
    s->face = GrafPort_txFace(port);
    s->mode = GrafPort_txMode(port);
    if (!(cvariant(ch) & 8)) {
        GrafPort_set_txFont(port, 0);
        GrafPort_set_txSize(port, 12);
        GrafPort_set_txFace(port, 0);
    }
    ctl_info *c = ctl_find(ch);
    if (!c || !c->has_font)
        return;
    const uint8_t *f = c->font;
    int flags = f[0] << 8 | f[1];
    int16_t font = (int16_t)(f[2] << 8 | f[3]), size = (int16_t)(f[4] << 8 | f[5]), style = (int16_t)(f[6] << 8 | f[7]);
    if (flags & 1) {
        if (font == -1) { /* kControlFontBigSystemFont */
            GrafPort_set_txFont(port, 0);
            GrafPort_set_txSize(port, 12);
        } else if (font == -2 || font == -3) { /* the small system font, plain or bold */
            GrafPort_set_txFont(port, 3);
            GrafPort_set_txSize(port, 10);
            GrafPort_set_txFace(port, font == -3 ? 1 : 0);
        } else {
            GrafPort_set_txFont(port, font);
        }
    }
    if (flags & 2)
        GrafPort_set_txFace(port, (uint8_t)style);
    if (flags & 4)
        GrafPort_set_txSize(port, (int16_t)((flags & 0x100) ? GrafPort_txSize(port) + size : size));
    if (flags & 0x20)
        GrafPort_set_txMode(port, (int16_t)(f[8] << 8 | f[9]));
}

void ctl_font_end(const ctl_font_saved *s)
{
    gaddr port = qd_port();
    GrafPort_set_txFont(port, s->font);
    GrafPort_set_txSize(port, s->size);
    GrafPort_set_txFace(port, s->face);
    GrafPort_set_txMode(port, s->mode);
}

/* The text colour SetControlFontStyle gave the control, if any. */
bool ctl_font_color(gaddr ch, uint16_t rgb[3])
{
    ctl_info *c = ctl_find(ch);
    if (!c || !c->has_font || !(((c->font[0] << 8) | c->font[1]) & 8))
        return false;
    for (int i = 0; i < 3; i++)
        rgb[i] = (uint16_t)(c->font[12 + i * 2] << 8 | c->font[13 + i * 2]);
    return true;
}

/* ---------------------------------------------------------------------- */
/* CDEF 0: push button, check box, radio button */

static void cdef0_draw(gaddr ch)
{
    if (!ControlRecord_contrlVis(cp(ch)))
        return;
    qrect r = crect(ch);
    int kind = cvariant(ch) & 3;
    uint8_t hil = chilite(ch);
    ctl_font_saved fs;
    ctl_font_begin(ch, &fs);
    tb_PenNormal();
    uint8_t title[256];
    int n;
    title_text(ch, title, &n);
    gaddr port = qd_port();
    int tw = qd_text_width(port, title, n);
    gaddr fi = tb_NewPtr(8, false, false);
    tb_GetFontInfo(fi);
    int asc = (int16_t)rd16(fi), desc = (int16_t)rd16(fi + 2);
    tb_DisposePtr(fi);
    int base = r.top + (r.bottom - r.top + asc - desc) / 2;
    if (kind == 0 && (native_draw_button(r, title, n, hil == inButton, hil == 255) ||
                      theme_draw_button(r, title, n, hil == inButton, hil == 255))) {
        ctl_font_end(&fs);
        return;
    }
    if (ctl_platinum(ch)) {
        ap_draw_standard(ch, kind == 0 ? 0 : kind == 1 ? 1 : 2);
        ctl_font_end(&fs);
        return;
    }
    if (kind == 0) {
        gaddr rr = tmp_rect(r);
        int round = (r.bottom - r.top) < 16 ? (r.bottom - r.top) : 16;
        tb_EraseRoundRect(rr, (int16_t)round, (int16_t)round);
        tb_FrameRoundRect(rr, (int16_t)round, (int16_t)round);
        tb_MoveTo((int16_t)(r.left + (r.right - r.left - tw) / 2), (int16_t)base);
        qd_draw_text(port, title, n);
        if (hil == inButton) {
            qrect in = { (int16_t)(r.top + 1), (int16_t)(r.left + 1), (int16_t)(r.bottom - 1), (int16_t)(r.right - 1) };
            qrect_put(rr, in);
            tb_InvertRoundRect(rr, (int16_t)(round - 2), (int16_t)(round - 2));
        }
        tb_DisposePtr(rr);
    } else {
        int bs = 12;
        int bt = r.top + (r.bottom - r.top - bs) / 2;
        qrect box = { (int16_t)bt, (int16_t)(r.left + 2), (int16_t)(bt + bs), (int16_t)(r.left + 2 + bs) };
        gaddr rr = tmp_rect(r);
        tb_EraseRect(rr);
        qrect_put(rr, box);
        if (native_draw_check(box, kind != 1, cvalue(ch) != 0, hil == inCheckBox, hil == 255) ||
            theme_draw_check(box, kind != 1, cvalue(ch) != 0, hil == inCheckBox, hil == 255)) {
            tb_MoveTo((int16_t)(box.right + 5), (int16_t)base);
            qd_draw_text(port, title, n);
            tb_DisposePtr(rr);
            if (hil == 255)
                fill((qrect){ r.top, (int16_t)(box.right + 1), r.bottom, r.right }, qd_pat_gray, patBic);
            ctl_font_end(&fs);
            return;
        }
        if (kind == 1) {
            tb_FrameRect(rr);
            if (cvalue(ch)) {
                tb_MoveTo(box.left, box.top);
                tb_LineTo((int16_t)(box.right - 1), (int16_t)(box.bottom - 1));
                tb_MoveTo((int16_t)(box.right - 1), box.top);
                tb_LineTo(box.left, (int16_t)(box.bottom - 1));
            }
            if (hil == inCheckBox) {
                qrect in = { (int16_t)(box.top + 1), (int16_t)(box.left + 1), (int16_t)(box.bottom - 1), (int16_t)(box.right - 1) };
                qrect_put(rr, in);
                tb_FrameRect(rr);
            }
        } else {
            tb_FrameOval(rr);
            if (cvalue(ch)) {
                qrect dot = { (int16_t)(box.top + 3), (int16_t)(box.left + 3), (int16_t)(box.bottom - 3), (int16_t)(box.right - 3) };
                qrect_put(rr, dot);
                tb_PaintOval(rr);
            }
            if (hil == inCheckBox) {
                qrect in = { (int16_t)(box.top + 1), (int16_t)(box.left + 1), (int16_t)(box.bottom - 1), (int16_t)(box.right - 1) };
                qrect_put(rr, in);
                tb_FrameOval(rr);
            }
        }
        tb_MoveTo((int16_t)(box.right + 5), (int16_t)base);
        qd_draw_text(port, title, n);
        tb_DisposePtr(rr);
    }
    if (hil == 255)
        fill(r, qd_pat_gray, patBic);
    ctl_font_end(&fs);
}

static int32_t cdef0_test(gaddr ch, uint32_t pt)
{
    qrect r = crect(ch);
    if (chilite(ch) == 255 || !ControlRecord_contrlVis(cp(ch)))
        return 0;
    if (pt_h(pt) >= r.left && pt_h(pt) < r.right && pt_v(pt) >= r.top && pt_v(pt) < r.bottom)
        return (cvariant(ch) & 3) == 0 ? inButton : inCheckBox;
    return 0;
}

/* ---------------------------------------------------------------------- */
/* CDEF 1: scroll bar */

static bool sb_vertical(qrect r) { return r.bottom - r.top > r.right - r.left; }

static qrect sb_thumb(gaddr ch)
{
    qrect r = crect(ch);
    bool v = sb_vertical(r);
    int klo, khi, slo, shi;
    if (native_scroll_knob(ch, &klo, &khi, &slo, &shi)) { /* where macOS drew its knob */
        if (v)
            return (qrect){ (int16_t)(r.top + klo), r.left, (int16_t)(r.top + khi), r.right };
        return (qrect){ r.top, (int16_t)(r.left + klo), r.bottom, (int16_t)(r.left + khi) };
    }
    int len = v ? r.bottom - r.top : r.right - r.left;
    int track = len - 2 * 16 - 16;
    int range = cmax(ch) - cmin(ch);
    int pos = range > 0 ? (int)((long)(cvalue(ch) - cmin(ch)) * track / range) : 0;
    if (v)
        return (qrect){ (int16_t)(r.top + 16 + pos), r.left, (int16_t)(r.top + 32 + pos), r.right };
    return (qrect){ r.top, (int16_t)(r.left + 16 + pos), r.bottom, (int16_t)(r.left + 32 + pos) };
}

static void draw_arrow(qrect box, int dir, bool hilited)
{
    /* dir: 0 up, 1 down, 2 left, 3 right.  A small outlined triangle. */
    int cx = (box.left + box.right) / 2, cy = (box.top + box.bottom) / 2;
    for (int i = 0; i < 6; i++) {
        int half = i;
        int y0, x0, x1;
        hrgn row;
        hrgn_init(&row);
        if (dir < 2) {
            y0 = dir == 0 ? cy - 4 + i : cy + 3 - i;
            x0 = cx - half;
            x1 = cx + half + 1;
            hrgn_rect(&row, (qrect){ (int16_t)y0, (int16_t)x0, (int16_t)(y0 + 1), (int16_t)x1 });
        } else {
            int xx = dir == 2 ? cx - 4 + i : cx + 3 - i;
            hrgn_rect(&row, (qrect){ (int16_t)(cy - half), (int16_t)xx, (int16_t)(cy + half + 1), (int16_t)(xx + 1) });
        }
        qd_fill_region(qd_port(), &row, qd_pat_black, hilited ? patCopy : patCopy);
        hrgn_free(&row);
    }
}

static void cdef1_draw(gaddr ch, int part)
{
    if (!ControlRecord_contrlVis(cp(ch)))
        return;
    qrect r = crect(ch);
    bool v = sb_vertical(r);
    if (native_draw_scrollbar(ch, r, v, chilite(ch), chilite(ch) != 255, chilite(ch) != 255 && cmax(ch) > cmin(ch), cvalue(ch),
                              cmin(ch), cmax(ch)) ||
        theme_draw_scrollbar(r, v, chilite(ch), chilite(ch) != 255, chilite(ch) != 255 && cmax(ch) > cmin(ch), sb_thumb(ch)))
        return;
    ctl_info *ci = ctl_find(ch);
    if ((ci && ci->cdef == 24 && g_appearance == MACHINE_APPEARANCE_BW) || ctl_platinum(ch)) {
        ap_scroll_bar(r, v, chilite(ch), chilite(ch) != 255, cmax(ch) > cmin(ch), sb_thumb(ch));
        return;
    }
    tb_PenNormal();
    gaddr rr = tmp_rect(r);
    tb_EraseRect(rr);
    tb_FrameRect(rr);
    bool active = chilite(ch) != 255 && cmax(ch) > cmin(ch);
    if (chilite(ch) != 255) {
        qrect a1 = v ? (qrect){ r.top, r.left, (int16_t)(r.top + 16), r.right }
                     : (qrect){ r.top, r.left, r.bottom, (int16_t)(r.left + 16) };
        qrect a2 = v ? (qrect){ (int16_t)(r.bottom - 16), r.left, r.bottom, r.right }
                     : (qrect){ r.top, (int16_t)(r.right - 16), r.bottom, r.right };
        qrect_put(rr, a1);
        tb_FrameRect(rr);
        qrect_put(rr, a2);
        tb_FrameRect(rr);
        draw_arrow(a1, v ? 0 : 2, chilite(ch) == inUpButton);
        draw_arrow(a2, v ? 1 : 3, chilite(ch) == inDownButton);
        if (chilite(ch) == inUpButton)
            fill((qrect){ (int16_t)(a1.top + 1), (int16_t)(a1.left + 1), (int16_t)(a1.bottom - 1), (int16_t)(a1.right - 1) },
                 qd_pat_black, patXor);
        if (chilite(ch) == inDownButton)
            fill((qrect){ (int16_t)(a2.top + 1), (int16_t)(a2.left + 1), (int16_t)(a2.bottom - 1), (int16_t)(a2.right - 1) },
                 qd_pat_black, patXor);
        if (active) {
            qrect track = v ? (qrect){ (int16_t)(r.top + 16), (int16_t)(r.left + 1), (int16_t)(r.bottom - 16), (int16_t)(r.right - 1) }
                            : (qrect){ (int16_t)(r.top + 1), (int16_t)(r.left + 16), (int16_t)(r.bottom - 1), (int16_t)(r.right - 16) };
            fill(track, qd_pat_gray, patCopy);
            qrect t = sb_thumb(ch);
            qrect_put(rr, t);
            tb_EraseRect(rr);
            tb_FrameRect(rr);
        }
    }
    tb_DisposePtr(rr);
}

static int32_t cdef1_test(gaddr ch, uint32_t pt)
{
    qrect r = crect(ch);
    if (chilite(ch) == 255 || !ControlRecord_contrlVis(cp(ch)))
        return 0;
    int h = pt_h(pt), v = pt_v(pt);
    if (h < r.left || h >= r.right || v < r.top || v >= r.bottom)
        return 0;
    bool vert = sb_vertical(r);
    int c = vert ? v : h, lo = vert ? r.top : r.left, hi = vert ? r.bottom : r.right;
    int klo, khi, slo, shi;
    bool native = native_scroll_knob(ch, &klo, &khi, &slo, &shi); /* macOS's bars have no arrows */
    if (!native && c < lo + 16)
        return inUpButton;
    if (!native && c >= hi - 16)
        return inDownButton;
    if (cmax(ch) <= cmin(ch))
        return 0;
    qrect t = sb_thumb(ch);
    int tlo = vert ? t.top : t.left, thi = vert ? t.bottom : t.right;
    if (c < tlo)
        return inPageUp;
    if (c >= thi)
        return inPageDown;
    return inThumb;
}

/* ---------------------------------------------------------------------- */
/* The CDEF entry points: FUNCTION (varCode, theControl, message, param). */

static int32_t standard_features(bool scroll) { return kControlWantsActivate; }

static void cdef_entry(cpu68k *c, bool scroll)
{
    uint32_t ret = cpu_pop32(c);
    int32_t param = (int32_t)cpu_pop32(c);
    int16_t msg = (int16_t)cpu_pop16(c);
    gaddr ch = cpu_pop32(c);
    cpu_pop16(c); /* varCode: we keep our own copy */
    int32_t result = 0;
    switch (msg) {
    case drawCntl:
        if (scroll)
            cdef1_draw(ch, param & 0xFF);
        else
            cdef0_draw(ch);
        break;
    case testCntl:
        result = scroll ? cdef1_test(ch, (uint32_t)param) : cdef0_test(ch, (uint32_t)param);
        break;
    case kControlMsgTestNewMsgSupport: result = NEW_MSG_OK; break;
    case kControlMsgGetFeatures: result = standard_features(scroll); break;
    default:
        break;
    }
    wr32(c->a[7], (uint32_t)result);
    cpu_push32(c, ret);
}

void cdef0_native(cpu68k *c, uint16_t tw) { cdef_entry(c, false); }
void cdef1_native(cpu68k *c, uint16_t tw) { cdef_entry(c, true); }

/* ---------------------------------------------------------------------- */
/* CDEF 63: pop-up menu (procIDs 1008-1015), as System 7's, and CDEF 25,
 * Appearance's pop-up button (400-415), which is the same with Platinum's
 * look, a menu ID of -12345 for no menu yet, a title width of -1 for one
 * worked out, and its menu by data access ('mhan', 'mnid').
 *
 * At creation the value is the title's style and justification, the
 * minimum the menu ID and the maximum the title's width; the CDEF then
 * takes the menu (into the hierarchical part of the menu list), keeps those
 * in contrlData (PopupPrivateData: menu handle, menu ID, then ours) and
 * makes the value the chosen item, from 1 to the number of items.
 * Variants: 1 fixed width, 2 variable width (fitting the item), 4 the
 * refCon is a resource type whose names are added, 8 the window's font. */

enum { popupFixedWidth = 1, popupVariableWidth = 2, popupUseAddResMenu = 4 };
enum { inPopUpMenu = 2 }; /* kControlMenuPart */
enum { popupTitleLeftJust = 0, popupTitleCenterJust = 1, popupTitleRightJust = 0xFF };
#define POPUP_DATA_SIZE 10 /* mHandle, mID, title style, title width */

static gaddr popup_data(gaddr ch)
{
    gaddr h = ControlRecord_contrlData(cp(ch));
    return h && rd32(h) ? rd32(h) : 0;
}

static gaddr popup_menu(gaddr ch)
{
    gaddr d = popup_data(ch);
    return d ? rd32(d) : 0;
}

static bool appearance_popup(gaddr ch)
{
    ctl_info *c = ctl_find(ch);
    return c && c->cdef == 25;
}

static void popup_init(gaddr ch)
{
    gaddr p = cp(ch);
    int16_t style = ControlRecord_contrlValue(p);
    int16_t mid = ControlRecord_contrlMin(p);
    int16_t width = ControlRecord_contrlMax(p);
    int32_t rc = ControlRecord_contrlRfCon(p);
    gaddr mh = 0;
    if (mid != -12345) {
        mh = tb_GetMenuHandle(mid);
        if (!mh) {
            mh = tb_GetMenu(mid);
            if (mh) {
                if (cvariant(ch) & popupUseAddResMenu)
                    tb_AppendResMenu(mh, rc);
                tb_InsertMenu(mh, -1);
            }
        }
    }
    if (width == -1) { /* worked out from the title */
        uint8_t t[256];
        int n;
        title_text(ch, t, &n);
        ctl_font_saved fs;
        gaddr saved = qd_port();
        tb_SetPort(cowner(ch));
        ctl_font_begin(ch, &fs);
        width = (int16_t)(n ? qd_text_width(qd_port(), t, n) + 4 : 0);
        ctl_font_end(&fs);
        tb_SetPort(saved);
    }
    gaddr dh = tb_NewHandle(POPUP_DATA_SIZE, false, true);
    if (dh && rd32(dh)) {
        gaddr d = rd32(dh);
        wr32(d, mh);
        wr16(d + 4, (uint16_t)mid);
        wr16(d + 6, (uint16_t)style);
        wr16(d + 8, (uint16_t)width);
    }
    p = cp(ch);
    ControlRecord_set_contrlData(p, dh);
    ControlRecord_set_contrlAction(p, 0xFFFFFFFFu); /* tracked by the CDEF itself */
    ControlRecord_set_contrlMin(p, 1);
    ControlRecord_set_contrlMax(p, (int16_t)(mh ? tb_CountMItems(mh) : 0));
    ControlRecord_set_contrlValue(p, 1);
}

/* The title area and the menu box. */
static void popup_parts(gaddr ch, qrect *title, qrect *box)
{
    qrect r = crect(ch);
    gaddr d = popup_data(ch);
    int tw = d ? (int16_t)rd16(d + 8) : 0;
    *title = (qrect){ r.top, r.left, r.bottom, (int16_t)(r.left + tw) };
    *box = (qrect){ (int16_t)(r.top + 1), (int16_t)(r.left + tw + (tw ? 2 : 0)), (int16_t)(r.bottom - 1), r.right };
    if (cvariant(ch) & popupVariableWidth) {
        gaddr mh = popup_menu(ch);
        if (mh) {
            gaddr s = tb_NewPtr(256, false, true);
            tb_GetMenuItemText(mh, cvalue(ch), s);
            uint8_t buf[256];
            gget(buf, s + 1, rd8(s));
            int w = qd_text_width(qd_port(), buf, rd8(s)) + 32;
            tb_DisposePtr(s);
            if (box->left + w < box->right)
                box->right = (int16_t)(box->left + w);
        }
    }
}

/* Platinum: the box a rounded grey button with the double arrow at its
 * right, the item in it. */
static void popup_draw_platinum(gaddr ch, qrect box, int base)
{
    uint8_t hil = chilite(ch);
    uint32_t state = hil == 255 ? 0 : hil == inPopUpMenu ? 2 : 1;
    qrect b = box;
    if (b.bottom - b.top > 20) { /* the button keeps its height, centred */
        int mid = (b.top + b.bottom) / 2;
        b.top = (int16_t)(mid - 10);
        b.bottom = (int16_t)(mid + 10);
    }
    ap_popup_button(b, state, false);
    gaddr mh = popup_menu(ch);
    if (!mh)
        return;
    gaddr s = tb_NewPtr(256, false, true);
    tb_GetMenuItemText(mh, cvalue(ch), s);
    uint8_t buf[256];
    int len = rd8(s);
    gget(buf, s + 1, (uint32_t)len);
    tb_DisposePtr(s);
    uint16_t c[3];
    ap_text_rgb(state == 0 ? 19 : state == 2 ? 20 : 18, c);
    ap_text_in((qrect){ b.top, (int16_t)(b.left + 8), b.bottom, (int16_t)(b.right - 22) }, buf, len, false, c);
}

static void popup_draw(gaddr ch)
{
    if (!ControlRecord_contrlVis(cp(ch)))
        return;
    ctl_font_saved fs;
    ctl_font_begin(ch, &fs);
    tb_PenNormal();
    gaddr port = qd_port();
    qrect title, box;
    popup_parts(ch, &title, &box);
    gaddr fi = tb_NewPtr(8, false, false);
    tb_GetFontInfo(fi);
    int asc = (int16_t)rd16(fi), desc = (int16_t)rd16(fi + 2);
    tb_DisposePtr(fi);
    int base = box.top + (box.bottom - box.top - 1 + asc - desc) / 2;
    gaddr rr = tmp_rect(crect(ch));
    tb_EraseRect(rr);
    bool platinum = (appearance_popup(ch) && g_appearance == MACHINE_APPEARANCE_BW) || ctl_platinum(ch);

    /* Title, in its style, justified in its width. */
    gaddr d = popup_data(ch);
    uint16_t style = d ? rd16(d + 6) : 0;
    uint8_t t[256];
    int n;
    title_text(ch, t, &n);
    if (n && title.right > title.left) {
        GrafPort_set_txFace(port, (uint8_t)(style >> 8 & 0x7F));
        int tw = qd_text_width(port, t, n);
        int x = title.left;
        if ((style & 0xFF) == popupTitleCenterJust)
            x = title.left + (title.right - title.left - tw) / 2;
        else if ((style & 0xFF) == popupTitleRightJust)
            x = title.right - tw;
        if (platinum) {
            uint16_t c[3];
            ap_text_rgb(chilite(ch) == 255 ? 38 : 37, c);
            ap_text(x, base, t, n, c);
        } else {
            tb_MoveTo((int16_t)x, (int16_t)base);
            qd_draw_text(port, t, n);
        }
        GrafPort_set_txFace(port, (cvariant(ch) & 8) ? fs.face : 0);
    }

    /* With the macOS appearance, macOS's pop-up button. */
    gaddr mh0 = popup_menu(ch);
    if (mh0) {
        gaddr s = tb_NewPtr(256, false, true);
        tb_GetMenuItemText(mh0, cvalue(ch), s);
        uint8_t buf[256];
        int len = rd8(s);
        gget(buf, s + 1, (uint32_t)len);
        tb_DisposePtr(s);
        if (native_draw_popup(box, buf, len, chilite(ch) == inPopUpMenu, chilite(ch) == 255)) {
            tb_DisposePtr(rr);
            ctl_font_end(&fs);
            return;
        }
    }
    if (platinum) {
        popup_draw_platinum(ch, box, base);
        tb_DisposePtr(rr);
        ctl_font_end(&fs);
        return;
    }

    /* The box with its drop shadow, the current item and the triangle. */
    qrect frame = { box.top, box.left, (int16_t)(box.bottom - 1), (int16_t)(box.right - 1) };
    qrect_put(rr, frame);
    tb_EraseRect(rr);
    tb_FrameRect(rr);
    tb_MoveTo((int16_t)(frame.left + 3), frame.bottom);
    tb_LineTo(frame.right, frame.bottom);
    tb_LineTo(frame.right, (int16_t)(frame.top + 3));
    gaddr mh = popup_menu(ch);
    if (mh) {
        gaddr s = tb_NewPtr(256, false, true);
        tb_GetMenuItemText(mh, cvalue(ch), s);
        uint8_t buf[256];
        int len = rd8(s);
        gget(buf, s + 1, (uint32_t)len);
        tb_DisposePtr(s);
        int room = frame.right - frame.left - 30;
        while (len > 0 && qd_text_width(port, buf, len) > room)
            len--;
        tb_MoveTo((int16_t)(frame.left + 13), (int16_t)base);
        qd_draw_text(port, buf, len);
    }
    int tx = frame.right - 17, ty = frame.top + (frame.bottom - frame.top) / 2 - 2;
    for (int i = 0; i < 6; i++) {
        tb_MoveTo((int16_t)(tx + i), (int16_t)(ty + i));
        tb_LineTo((int16_t)(tx + 10 - i), (int16_t)(ty + i));
    }
    if (chilite(ch) == inPopUpMenu) {
        qrect in = { (int16_t)(frame.top + 1), (int16_t)(frame.left + 1), frame.bottom, frame.right };
        qrect_put(rr, in);
        tb_InvertRect(rr);
    }
    tb_DisposePtr(rr);
    if (chilite(ch) == 255)
        fill(crect(ch), qd_pat_gray, patBic);
    ctl_font_end(&fs);
}

static int32_t popup_test(gaddr ch, uint32_t pt)
{
    if (chilite(ch) == 255 || !ControlRecord_contrlVis(cp(ch)))
        return 0;
    qrect title, box;
    popup_parts(ch, &title, &box);
    qrect r = { title.top, title.left, box.bottom, box.right };
    if (pt_h(pt) >= r.left && pt_h(pt) < r.right && pt_v(pt) >= r.top && pt_v(pt) < r.bottom)
        return inPopUpMenu;
    return 0;
}

/* autoTrack: the menu pops up over the box with the current item under the
 * mouse; the value becomes the item chosen. */
static void popup_track(gaddr ch)
{
    gaddr mh = popup_menu(ch);
    if (!mh)
        return;
    qrect title, box;
    popup_parts(ch, &title, &box);
    gaddr pt = tb_NewPtr(4, false, true);
    wr32(pt, make_pt(box.top, box.left));
    tb_LocalToGlobal(pt);
    uint32_t g = rd32(pt);
    tb_DisposePtr(pt);
    /* The item marked is the current one. */
    int16_t cur = cvalue(ch);
    int items = tb_CountMItems(mh);
    for (int i = 1; i <= items; i++)
        tb_SetItemMark(mh, (int16_t)i, i == cur ? 0x12 : 0);
    tb_HiliteControl(ch, inPopUpMenu);
    int32_t r = tb_PopUpMenuSelect(mh, (int16_t)pt_v(g), (int16_t)pt_h(g), cur);
    tb_HiliteControl(ch, 0);
    int16_t item = (int16_t)(r & 0xFFFF);
    if (r && item > 0) {
        ControlRecord_set_contrlValue(cp(ch), item);
        tb_Draw1Control(ch);
    }
}

/* ControlDataAccessRec: tag, part, size, dataPtr. */
static int32_t popup_data_access(gaddr ch, gaddr rec, bool set)
{
    uint32_t tag = rd32(rec);
    int32_t size = (int32_t)rd32(rec + 8);
    gaddr data = rd32(rec + 12);
    gaddr d = popup_data(ch);
    if (!d)
        return errDataNotSupported;
    if (tag == fourcc("mhan") || tag == fourcc("mnid")) {
        bool handle = tag == fourcc("mhan");
        int32_t want = handle ? 4 : 2;
        if (!set) {
            if (data && size >= want) {
                if (handle)
                    wr32(data, rd32(d));
                else
                    wr16(data, rd16(d + 4));
            }
            wr32(rec + 8, (uint32_t)want);
            return noErr;
        }
        if (size < want)
            return -30591; /* errDataSizeMismatch */
        gaddr mh = handle ? rd32(data) : tb_GetMenuHandle((int16_t)rd16(data));
        if (!handle && !mh) {
            mh = tb_GetMenu((int16_t)rd16(data));
            if (mh)
                tb_InsertMenu(mh, -1);
        }
        d = popup_data(ch);
        wr32(d, mh);
        wr16(d + 4, mh ? (uint16_t)MenuInfo_menuID(rd32(mh)) : 0);
        ControlRecord_set_contrlMax(cp(ch), (int16_t)(mh ? tb_CountMItems(mh) : 0));
        return noErr;
    }
    if (tag == fourcc("font")) {
        ctl_info *c = ctl_find(ch);
        if (set && c && size >= 24) {
            gget(c->font, data, 24);
            c->has_font = true;
        }
        return noErr;
    }
    return errDataNotSupported;
}

void cdef63_native(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    int32_t param = (int32_t)cpu_pop32(c);
    int16_t msg = (int16_t)cpu_pop16(c);
    gaddr ch = cpu_pop32(c);
    cpu_pop16(c);
    int32_t result = 0;
    switch (msg) {
    case drawCntl: popup_draw(ch); break;
    case testCntl: result = popup_test(ch, (uint32_t)param); break;
    case initCntl: popup_init(ch); break;
    case dispCntl: {
        gaddr dh = ControlRecord_contrlData(cp(ch));
        if (dh)
            tb_DisposeHandle(dh);
        ControlRecord_set_contrlData(cp(ch), 0);
        break;
    }
    case autoTrack: popup_track(ch); break;
    case kControlMsgTestNewMsgSupport: result = NEW_MSG_OK; break;
    case kControlMsgGetFeatures: result = kControlWantsActivate | kControlSupportsDataAccess; break;
    case kControlMsgSetData: result = popup_data_access(ch, (gaddr)param, true); break;
    case kControlMsgGetData: result = popup_data_access(ch, (gaddr)param, false); break;
    case kControlMsgCalcBestRect: {
        qrect title, box;
        popup_parts(ch, &title, &box);
        wr16((gaddr)param, 20);
        wr16((gaddr)param + 2, (uint16_t)(box.right - title.left));
        wr16((gaddr)param + 4, 0);
        break;
    }
    default: break;
    }
    wr32(c->a[7], (uint32_t)result);
    cpu_push32(c, ret);
}

/* CDEF 24 (Appearance's scroll bar, live with variant 2) is CDEF 1 in
 * Platinum; CDEF 25 is CDEF 63's. */
void cdef24_native(cpu68k *c, uint16_t tw) { cdef_entry(c, true); }
void cdef25_native(cpu68k *c, uint16_t tw) { cdef63_native(c, tw); }

/* ---------------------------------------------------------------------- */
/* Embedding */

static void embed(gaddr ch, gaddr container)
{
    ctl_info *c = ctl_find(ch);
    if (!c)
        return;
    if (c->parent == container)
        return;
    gaddr old = c->parent;
    c->parent = container;
    c->seq = ++embed_seq;
    if (old)
        ctl_call(old, kControlMsgSubControlRemoved, (int32_t)ch);
    if (container)
        ctl_call(container, kControlMsgSubControlAdded, (int32_t)ch);
}

int16_t tb_CreateRootControl(gaddr w, gaddr out)
{
    int i = win_index(w, true);
    if (i < 0)
        return memFullErr;
    if (wins[i].root) {
        if (out)
            wr32(out, wins[i].root);
        return errRootAlreadyExists;
    }
    /* A user pane the size of everything, drawing nothing. */
    gaddr r = tmp_rect((qrect){ -32767, -32767, 32767, 32767 });
    gaddr root = tb_NewControl(w, r, 0, 1, 0, 0, 0, 256, 0);
    tb_DisposePtr(r);
    i = win_index(w, true);
    if (!root || i < 0)
        return memFullErr;
    wins[i].root = root;
    ctl_info *rc = ctl_find(root);
    if (rc)
        rc->is_root = true;
    /* Controls made before it go in it, in the order they were made. */
    gaddr list[512];
    int n = 0;
    for (gaddr x = WindowRecord_controlList(w); x && n < 512; x = ControlRecord_nextControl(cp(x)))
        if (x != root)
            list[n++] = x;
    for (int k = n - 1; k >= 0; k--) {
        ctl_info *c = ctl_find(list[k]);
        if (c && !c->parent)
            embed(list[k], root);
    }
    if (out)
        wr32(out, root);
    return noErr;
}

int16_t tb_GetRootControl(gaddr w, gaddr out)
{
    gaddr root = ctl_window_root(w);
    if (out)
        wr32(out, root);
    return root ? noErr : errNoRootControl;
}

int16_t tb_EmbedControl(gaddr ch, gaddr container)
{
    ctl_info *c = ctl_find(ch), *k = ctl_find(container);
    if (!c || !k)
        return errUnknownControl;
    if (ch == container)
        return errCantEmbedIntoSelf;
    if (c->is_root)
        return errCantEmbedRoot;
    if (!k->is_root && !(features(container) & kControlSupportsEmbedding))
        return errControlIsNotEmbedder;
    for (ctl_info *a = k; a; a = ctl_find(a->parent)) /* not into its own descendant */
        if (a->ch == ch)
            return errCantEmbedIntoSelf;
    embed(ch, container);
    /* It takes on its new embedder's visibility and activity. */
    if (!ControlRecord_contrlVis(cp(container)) && ControlRecord_contrlVis(cp(ch))) {
        c = ctl_find(ch);
        c->hidden_with_parent = true;
        ControlRecord_set_contrlVis(cp(ch), 0);
    }
    if (chilite(container) == 255 && chilite(ch) != 255)
        tb_DeactivateControl(ch);
    return noErr;
}

/* The deepest embedder (the root, or one of its descendants) whose
 * rectangle holds the control's. */
int16_t tb_AutoEmbedControl(gaddr ch, gaddr w)
{
    gaddr root = ctl_window_root(w);
    if (!root)
        return errNoRootControl;
    qrect r = crect(ch);
    gaddr best = root;
    int best_depth = 0;
    for (int i = 0; i < nctls; i++) {
        gaddr x = ctls[i].ch;
        if (!x || x == ch || x == root || cowner(x) != w || ctls[i].is_root)
            continue;
        if (!(features(x) & kControlSupportsEmbedding))
            continue;
        qrect b = crect(x);
        if (r.left >= b.left && r.top >= b.top && r.right <= b.right && r.bottom <= b.bottom) {
            int d = depth_of(x);
            if (d >= best_depth) {
                best = x;
                best_depth = d;
            }
        }
    }
    return tb_EmbedControl(ch, best);
}

int16_t tb_GetSuperControl(gaddr ch, gaddr out)
{
    ctl_info *c = ctl_find(ch);
    if (!c)
        return errUnknownControl;
    wr32(out, c->parent);
    return c->parent ? noErr : errControlIsNotEmbedder;
}

int16_t tb_CountSubControls(gaddr ch, gaddr out)
{
    if (!ctl_find(ch))
        return errUnknownControl;
    gaddr list[512];
    int n = ctl_subcontrols(ch, list, 512);
    wr16(out, (uint16_t)n);
    return (features(ch) & kControlSupportsEmbedding) || n || ctl_find(ch)->is_root ? noErr : errControlIsNotEmbedder;
}

int16_t tb_GetIndexedSubControl(gaddr ch, uint16_t index, gaddr out)
{
    gaddr list[512];
    int n = ctl_subcontrols(ch, list, 512);
    if (index < 1 || index > n)
        return paramErr;
    wr32(out, list[index - 1]);
    return noErr;
}

int16_t tb_SetControlSupervisor(gaddr ch, gaddr boss)
{
    ctl_info *c = ctl_find(ch);
    if (!c)
        return errUnknownControl;
    c->supervisor = boss;
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* Creating and disposing */

gaddr tb_NewControl(gaddr wst, gaddr r, gaddr title, int8_t vis, int16_t value, int16_t min, int16_t max,
                    int16_t procid, int32_t rc)
{
    uint32_t tl = title ? (uint32_t)rd8(title) + 1 : 1;
    gaddr ch = tb_NewHandle((int32_t)(ControlRecord_contrlTitle_off + tl), false, true);
    if (!ch)
        return 0;
    gaddr p = cp(ch);
    ControlRecord_set_contrlOwner(p, wst);
    gmemcpy(p + ControlRecord_contrlRect_off, r, 8);
    ControlRecord_set_contrlVis(p, vis ? 255 : 0);
    ControlRecord_set_contrlHilite(p, 0);
    ControlRecord_set_contrlValue(p, value);
    ControlRecord_set_contrlMin(p, min);
    ControlRecord_set_contrlMax(p, max);
    gaddr cdef = tb_GetResource((int32_t)fourcc("CDEF"), (int16_t)(procid >> 4));
    p = cp(ch);
    ControlRecord_set_contrlDefProc(p, cdef);
    ControlRecord_set_contrlRfCon(p, rc);
    if (title)
        gmemcpy(p + ControlRecord_contrlTitle_off, title, tl);
    ctl_info *c = ctl_add(ch);
    c->var = (int16_t)(procid & 15);
    c->cdef = (int16_t)(procid >> 4);
    /* Link at the head of the window's control list. */
    ControlRecord_set_nextControl(p, WindowRecord_controlList(wst));
    WindowRecord_set_controlList(wst, ch);
    call_cdef(ch, initCntl, 0);
    /* With a root control, the window's new controls go in it. */
    gaddr root = ctl_window_root(wst);
    if (root)
        tb_AutoEmbedControl(ch, wst);
    if (vis && ControlRecord_contrlVis(cp(ch)))
        tb_Draw1Control(ch);
    return ch;
}

/* CNTL: rect(8), value(2), visible(2), max(2), min(2), procID(2), refCon(4), title. */
gaddr tb_GetNewControl(int16_t cid, gaddr wst)
{
    gaddr h = tb_GetResource((int32_t)fourcc("CNTL"), cid);
    if (!h || !rd32(h))
        return 0;
    tb_HLock(h);
    gaddr p = rd32(h);
    gaddr ch = tb_NewControl(wst, p, p + 22, rd16(p + 10) != 0, (int16_t)rd16(p + 8), (int16_t)rd16(p + 14),
                             (int16_t)rd16(p + 12), (int16_t)rd16(p + 16), (int32_t)rd32(p + 18));
    tb_HUnlock(h);
    return ch;
}

/* Out of the window's list (a control that is only a dialog item's stand-in is not in it). */
static void unlink_control(gaddr w, gaddr ch)
{
    gaddr prev = 0;
    for (gaddr x = WindowRecord_controlList(w); x; prev = x, x = ControlRecord_nextControl(cp(x))) {
        if (x == ch) {
            if (prev)
                ControlRecord_set_nextControl(cp(prev), ControlRecord_nextControl(cp(ch)));
            else
                WindowRecord_set_controlList(w, ControlRecord_nextControl(cp(ch)));
            break;
        }
    }
}

void tb_DisposeControl(gaddr ch)
{
    gaddr w = cowner(ch);
    /* Its embedded controls go first. */
    gaddr subs[512];
    int n = ctl_subcontrols(ch, subs, 512);
    for (int i = 0; i < n; i++)
        tb_DisposeControl(subs[i]);
    tb_HideControl(ch);
    call_cdef(ch, dispCntl, 0);
    native_forget_control(ch);
    unlink_control(w, ch);
    int wi = win_index(w, false);
    if (wi >= 0) {
        if (wins[wi].focus == ch)
            wins[wi].focus = 0;
        if (wins[wi].root == ch)
            wins[wi].root = 0;
    }
    ctl_info *c = ctl_find(ch);
    if (c) {
        if (c->parent)
            ctl_call(c->parent, kControlMsgSubControlRemoved, (int32_t)ch);
        free(c->ap);
        c->ap = NULL;
        c->ch = 0;
    }
    tb_DisposeHandle(ch);
}

void tb_KillControls(gaddr w)
{
    while (WindowRecord_controlList(w))
        tb_DisposeControl(WindowRecord_controlList(w));
    int wi = win_index(w, false);
    if (wi >= 0) {
        wins[wi].root = 0;
        wins[wi].focus = 0;
    }
}

/* ---------------------------------------------------------------------- */
/* Drawing */

/* A control, then those embedded in it. */
static void draw_tree(gaddr ch)
{
    call_cdef(ch, drawCntl, 0);
    if (!ControlRecord_contrlVis(cp(ch)))
        return;
    gaddr subs[512];
    int n = ctl_subcontrols(ch, subs, 512);
    for (int i = 0; i < n; i++)
        draw_tree(subs[i]);
}

void tb_Draw1Control(gaddr ch)
{
    gaddr saved = qd_port();
    tb_SetPort(cowner(ch));
    draw_tree(ch);
    tb_SetPort(saved);
}

void tb_DrawControlInCurrentPort(gaddr ch) { draw_tree(ch); }

void tb_DrawControls(gaddr w)
{
    /* Back to front: the list is front to back, so from its end; embedded
     * controls with their embedders. */
    gaddr list[512];
    int n = 0;
    for (gaddr x = WindowRecord_controlList(w); x && n < 512; x = ControlRecord_nextControl(cp(x)))
        list[n++] = x;
    for (int i = n - 1; i >= 0; i--) {
        ctl_info *c = ctl_find(list[i]);
        if (!c || !c->parent)
            tb_Draw1Control(list[i]);
    }
}

void tb_UpdateControls(gaddr wp, gaddr rh) { tb_DrawControls(wp); }

/* Embedded controls are hidden with their embedder and come back with it
 * (those hidden on their own stay hidden). */
static void hide_subs(gaddr ch)
{
    gaddr subs[512];
    int n = ctl_subcontrols(ch, subs, 512);
    for (int i = 0; i < n; i++) {
        ctl_info *c = ctl_find(subs[i]);
        if (ControlRecord_contrlVis(cp(subs[i]))) {
            c->hidden_with_parent = true;
            ControlRecord_set_contrlVis(cp(subs[i]), 0);
            hide_subs(subs[i]);
        }
    }
}

static void show_subs(gaddr ch)
{
    gaddr subs[512];
    int n = ctl_subcontrols(ch, subs, 512);
    for (int i = 0; i < n; i++) {
        ctl_info *c = ctl_find(subs[i]);
        if (c->hidden_with_parent) {
            c->hidden_with_parent = false;
            ControlRecord_set_contrlVis(cp(subs[i]), 255);
            show_subs(subs[i]);
        }
    }
}

void tb_ShowControl(gaddr ch)
{
    ctl_info *c = ctl_find(ch);
    if (c && c->parent && !ControlRecord_contrlVis(cp(c->parent))) {
        c->hidden_with_parent = true; /* it shows with its embedder */
        return;
    }
    if (ControlRecord_contrlVis(cp(ch)))
        return;
    ControlRecord_set_contrlVis(cp(ch), 255);
    show_subs(ch);
    tb_Draw1Control(ch);
}

void tb_HideControl(gaddr ch)
{
    ctl_info *c = ctl_find(ch);
    if (c)
        c->hidden_with_parent = false;
    if (!ControlRecord_contrlVis(cp(ch)))
        return;
    ControlRecord_set_contrlVis(cp(ch), 0);
    hide_subs(ch);
    if (c && c->is_root)
        return;
    gaddr saved = qd_port();
    tb_SetPort(cowner(ch));
    tb_EraseRect(cp(ch) + ControlRecord_contrlRect_off);
    tb_InvalRect(cp(ch) + ControlRecord_contrlRect_off);
    tb_SetPort(saved);
}

int16_t tb_SetControlVisibility(gaddr ch, int8_t visible, int8_t draw)
{
    if (!ctl_find(ch))
        return errUnknownControl;
    if (draw) {
        if (visible)
            tb_ShowControl(ch);
        else
            tb_HideControl(ch);
        return noErr;
    }
    ControlRecord_set_contrlVis(cp(ch), visible ? 255 : 0);
    if (visible)
        show_subs(ch);
    else
        hide_subs(ch);
    return noErr;
}

int8_t tb_IsControlVisible(gaddr ch) { return ch && ControlRecord_contrlVis(cp(ch)) != 0; }

/* Activation: an embedder's controls go inactive with it and come back
 * with it, unless they were deactivated on their own. */
static void set_active(gaddr ch, bool active, bool by_parent)
{
    ctl_info *c = ctl_find(ch);
    if (!c)
        return;
    bool was = chilite(ch) != 255;
    if (active) {
        if (by_parent && !c->inactive_with_parent)
            return; /* inactive on its own */
        c->inactive_with_parent = false;
        if (!was) {
            ControlRecord_set_contrlHilite(cp(ch), c->saved_hilite == 255 ? 0 : c->saved_hilite);
            if (features(ch) & kControlWantsActivate)
                call_cdef(ch, 22, 1);
        }
    } else {
        if (by_parent && !was)
            return;
        if (by_parent)
            c->inactive_with_parent = true;
        if (was) {
            c->saved_hilite = chilite(ch);
            ControlRecord_set_contrlHilite(cp(ch), 255);
            if (features(ch) & kControlWantsActivate)
                call_cdef(ch, 22, 0);
        }
    }
    gaddr subs[512];
    int n = ctl_subcontrols(ch, subs, 512);
    for (int i = 0; i < n; i++)
        set_active(subs[i], active, true);
}

int16_t tb_ActivateControl(gaddr ch)
{
    ctl_info *c = ctl_find(ch);
    if (!c)
        return errUnknownControl;
    bool was = chilite(ch) != 255;
    c->inactive_with_parent = false;
    set_active(ch, true, false);
    if (!was && ControlRecord_contrlVis(cp(ch)))
        tb_Draw1Control(ch);
    return noErr;
}

int16_t tb_DeactivateControl(gaddr ch)
{
    ctl_info *c = ctl_find(ch);
    if (!c)
        return errUnknownControl;
    bool was = chilite(ch) != 255;
    set_active(ch, false, false);
    if (was && ControlRecord_contrlVis(cp(ch)))
        tb_Draw1Control(ch);
    return noErr;
}

int8_t tb_IsControlActive(gaddr ch) { return ch && chilite(ch) != 255; }

void tb_HiliteControl(gaddr ch, int16_t state)
{
    if (chilite(ch) == (uint8_t)state)
        return;
    gaddr subs[1];
    bool embedder = ctl_subcontrols(ch, subs, 1) > 0;
    if (embedder && ((uint8_t)state == 255 || chilite(ch) == 255)) { /* an embedder's whole tree */
        if ((uint8_t)state == 255)
            tb_DeactivateControl(ch);
        else {
            tb_ActivateControl(ch);
            ControlRecord_set_contrlHilite(cp(ch), (uint8_t)state);
        }
        return;
    }
    ControlRecord_set_contrlHilite(cp(ch), (uint8_t)state);
    tb_Draw1Control(ch);
}

/* ---------------------------------------------------------------------- */
/* Values */

static void clamp_and_draw(gaddr ch)
{
    gaddr p = cp(ch);
    int16_t v = ControlRecord_contrlValue(p), lo = ControlRecord_contrlMin(p), hi = ControlRecord_contrlMax(p);
    if (hi < lo)
        ControlRecord_set_contrlMax(p, hi = lo);
    if (v < lo)
        v = lo;
    if (v > hi)
        v = hi;
    ControlRecord_set_contrlValue(p, v);
    tb_Draw1Control(ch);
}

void tb_SetControlValue(gaddr ch, int16_t v)
{
    ControlRecord_set_contrlValue(cp(ch), v);
    clamp_and_draw(ch);
}

int16_t tb_GetControlValue(gaddr ch) { return cvalue(ch); }

void tb_SetControlMinimum(gaddr ch, int16_t v)
{
    ControlRecord_set_contrlMin(cp(ch), v);
    clamp_and_draw(ch);
}

int16_t tb_GetControlMinimum(gaddr ch) { return cmin(ch); }

void tb_SetControlMaximum(gaddr ch, int16_t v)
{
    ControlRecord_set_contrlMax(cp(ch), v);
    clamp_and_draw(ch);
}

int16_t tb_GetControlMaximum(gaddr ch) { return cmax(ch); }

void tb_SetControlTitle(gaddr ch, gaddr t)
{
    uint32_t tl = (uint32_t)rd8(t) + 1;
    tb_SetHandleSize(ch, (int32_t)(ControlRecord_contrlTitle_off + tl));
    gmemcpy(cp(ch) + ControlRecord_contrlTitle_off, t, tl);
    tb_Draw1Control(ch);
}

void tb_GetControlTitle(gaddr ch, gaddr t)
{
    gaddr s = cp(ch) + ControlRecord_contrlTitle_off;
    gmemcpy(t, s, (uint32_t)rd8(s) + 1);
}

void tb_SetControlReference(gaddr ch, int32_t data) { ControlRecord_set_contrlRfCon(cp(ch), data); }
int32_t tb_GetControlReference(gaddr ch) { return ControlRecord_contrlRfCon(cp(ch)); }
void tb_SetControlAction(gaddr ch, gaddr a) { ControlRecord_set_contrlAction(cp(ch), a); }
gaddr tb_GetControlAction(gaddr ch) { return ControlRecord_contrlAction(cp(ch)); }

/* Move a control and those embedded in it by (dh, dv). */
static void offset_tree(gaddr ch, int dh, int dv)
{
    qrect r = crect(ch);
    qrect_put(cp(ch) + ControlRecord_contrlRect_off,
              (qrect){ (int16_t)(r.top + dv), (int16_t)(r.left + dh), (int16_t)(r.bottom + dv), (int16_t)(r.right + dh) });
    call_cdef(ch, posCntl, (int32_t)make_pt((int16_t)dv, (int16_t)dh));
    gaddr subs[512];
    int n = ctl_subcontrols(ch, subs, 512);
    for (int i = 0; i < n; i++)
        offset_tree(subs[i], dh, dv);
}

void tb_MoveControl(gaddr ch, int16_t h, int16_t v)
{
    qrect r = crect(ch);
    bool vis = ControlRecord_contrlVis(cp(ch)) != 0;
    if (vis)
        tb_HideControl(ch);
    gaddr subs[1];
    if (ctl_subcontrols(ch, subs, 1))
        offset_tree(ch, h - r.left, v - r.top);
    else
        qrect_put(cp(ch) + ControlRecord_contrlRect_off,
                  (qrect){ v, h, (int16_t)(v + r.bottom - r.top), (int16_t)(h + r.right - r.left) });
    if (vis)
        tb_ShowControl(ch);
}

void tb_SizeControl(gaddr ch, int16_t width, int16_t height)
{
    qrect r = crect(ch);
    bool vis = ControlRecord_contrlVis(cp(ch)) != 0;
    if (vis)
        tb_HideControl(ch);
    qrect_put(cp(ch) + ControlRecord_contrlRect_off,
              (qrect){ r.top, r.left, (int16_t)(r.top + height), (int16_t)(r.left + width) });
    if (vis)
        tb_ShowControl(ch);
}

/* ---------------------------------------------------------------------- */
/* Hit testing and tracking */

int16_t tb_TestControl(gaddr ch, uint32_t p)
{
    if (!ControlRecord_contrlVis(cp(ch)) || chilite(ch) == 255)
        return 0;
    return (int16_t)call_cdef(ch, testCntl, (int32_t)p);
}

/* The control hit: the most deeply embedded one, and of those the front. */
int16_t tb_FindControl(uint32_t p, gaddr w, gaddr cpp)
{
    wr32(cpp, 0);
    if (!w || !WindowRecord_visible(w))
        return 0;
    gaddr best = 0;
    int16_t best_part = 0;
    int best_depth = -1;
    for (gaddr x = WindowRecord_controlList(w); x; x = ControlRecord_nextControl(cp(x))) {
        int16_t part = tb_TestControl(x, p);
        if (!part)
            continue;
        int d = depth_of(x);
        if (d > best_depth) {
            best = x;
            best_part = part;
            best_depth = d;
        }
    }
    if (best)
        wr32(cpp, best);
    return best_part;
}

/* The deepest visible control under the point, hit or not (its part may be 0). */
gaddr tb_FindControlUnderMouse(uint32_t p, gaddr w, gaddr partp)
{
    if (partp)
        wr16(partp, 0);
    if (!w)
        return 0;
    gaddr best = 0;
    int best_depth = -1;
    for (gaddr x = WindowRecord_controlList(w); x; x = ControlRecord_nextControl(cp(x))) {
        ctl_info *c = ctl_find(x);
        if (!ControlRecord_contrlVis(cp(x)) || (c && c->is_root))
            continue;
        qrect r = crect(x);
        if (pt_h(p) < r.left || pt_h(p) >= r.right || pt_v(p) < r.top || pt_v(p) >= r.bottom)
            continue;
        int d = depth_of(x);
        if (d > best_depth) {
            best = x;
            best_depth = d;
        }
    }
    if (!best)
        best = ctl_window_root(w);
    if (best && partp)
        wr16(partp, (uint16_t)tb_TestControl(best, p));
    return best;
}

static uint32_t local_mouse(gaddr w)
{
    uint32_t g = ev_mouse_global();
    pixdst d = qd_port_pixels(w);
    return make_pt((int16_t)(pt_v(g) + d.bounds.top), (int16_t)(pt_h(g) + d.bounds.left));
}

uint32_t ctl_local_mouse(gaddr ch) { return local_mouse(cowner(ch)); }

static void call_action(gaddr action, gaddr ch, int16_t part)
{
    uint32_t args[2] = { ch, (uint32_t)(uint16_t)part };
    mm_call(action, 0x000002C0 /* pascal (long, short) */, args, 2);
}

void ctl_call_action(gaddr action, gaddr ch, int16_t part) { call_action(action, ch, part); }

int16_t tb_TrackControl(gaddr ch, uint32_t p, gaddr a)
{
    int16_t part = tb_TestControl(ch, p);
    if (!part)
        return 0;
    gaddr w = cowner(ch);
    /* -1: the control's own action procedure.  A control whose action is -1
     * tracks itself, even when the caller passes none. */
    if (a == 0xFFFFFFFFu || (!a && ControlRecord_contrlAction(cp(ch)) == 0xFFFFFFFFu))
        a = ControlRecord_contrlAction(cp(ch));
    if (a == 0xFFFFFFFFu) {
        /* The CDEF tracks the mouse itself (pop-up menus). */
        call_cdef(ch, autoTrack, part);
        return part;
    }
    /* The Appearance controls that track in a way of their own (bevel
     * buttons' menus, sliders' thumbs, edit text, lists, user panes). */
    int16_t done;
    if (ap_ctl_track(ch, part, p, a, 0, &done)) {
        ap_after_track(ch, done);
        return done;
    }
    if (part == inThumb) {
        /* Drag the thumb; the value follows the mouse along the track. */
        qrect r = crect(ch);
        bool v = sb_vertical(r);
        int len = (v ? r.bottom - r.top : r.right - r.left) - 48;
        int klo, khi, slo, shi;
        if (native_scroll_knob(ch, &klo, &khi, &slo, &shi)) /* the knob's travel along its slot */
            len = (shi - slo) - (khi - klo);
        int start = v ? pt_v(p) : pt_h(p);
        int16_t v0 = cvalue(ch);
        int range = cmax(ch) - cmin(ch);
        ctl_info *ci = ctl_find(ch);
        bool live = ci && ci->cdef == 24 && (ci->var & 2) && a;
        while (ev_still_down()) {
            uint32_t m = local_mouse(w);
            int delta = (v ? pt_v(m) : pt_h(m)) - start;
            if (len > 0 && range > 0) {
                int nv = v0 + (int)((long)delta * range / len);
                if (nv != cvalue(ch)) {
                    tb_SetControlValue(ch, (int16_t)nv);
                    if (live) /* live scrolling: the action procedure follows the thumb */
                        call_action(a, ch, inThumb);
                }
            }
            ev_wait_tick();
        }
        return inThumb;
    }
    bool inside = true;
    tb_HiliteControl(ch, part);
    do {
        if (inside && a && part != inButton && part != inCheckBox)
            call_action(a, ch, part);
        ev_wait_tick();
        bool now = tb_TestControl(ch, local_mouse(w)) == part;
        if (now != inside) {
            inside = now;
            tb_HiliteControl(ch, inside ? part : 0);
        }
    } while (ev_still_down());
    tb_HiliteControl(ch, 0);
    if (inside)
        ap_after_track(ch, part);
    return inside ? part : 0;
}

void tb_DragControl(gaddr ch, uint32_t p, gaddr limit, gaddr slop, int16_t axis)
{
    int32_t d = tb_DragGrayRgn(0, p, limit, slop, axis, 0);
    qrect r = crect(ch);
    tb_MoveControl(ch, (int16_t)(r.left + pt_h((uint32_t)d)), (int16_t)(r.top + pt_v((uint32_t)d)));
}

int16_t tb_GetControlVariant(gaddr ch) { return ch ? cvariant(ch) : 0; }

/* HandleControlClick: TrackControl, with the focus moving to controls that
 * take it on a click. */
int16_t tb_HandleControlClick(gaddr ch, uint32_t where, int16_t modifiers, gaddr action)
{
    if (!ch || !tb_TestControl(ch, where))
        return 0;
    uint32_t f = features(ch);
    if ((f & kControlGetsFocusOnClick) && (f & kControlSupportsFocus))
        tb_SetKeyboardFocus(cowner(ch), ch, -1);
    int16_t part = tb_TestControl(ch, where);
    int16_t done;
    if (ap_ctl_track(ch, part, where, action, modifiers, &done)) {
        ap_after_track(ch, done);
        return done;
    }
    if ((f & kControlHandlesTracking) && new_messages(ch)) {
        /* ControlTrackingRec: startPt, modifiers, action */
        gaddr rec = tb_NewPtr(10, false, true);
        wr32(rec, where);
        wr16(rec + 4, (uint16_t)modifiers);
        wr32(rec + 6, action);
        int16_t r = (int16_t)call_cdef(ch, kControlMsgHandleTracking, (int32_t)rec);
        tb_DisposePtr(rec);
        return r;
    }
    return tb_TrackControl(ch, where, action);
}

/* ---------------------------------------------------------------------- */
/* Keyboard focus */

int16_t tb_GetKeyboardFocus(gaddr w, gaddr out)
{
    int i = win_index(w, false);
    gaddr focus = i >= 0 ? wins[i].focus : 0;
    /* A dialog's edit text being edited has the focus (as its stand-in). */
    if (w && WindowRecord_windowKind(w) == 2 && (!focus || ap_is_stand_in(focus))) {
        int16_t field = (int16_t)rd16(w + DialogRecord_editField_off);
        if (field >= 0 && rd32(w + DialogRecord_textH_off))
            focus = ap_item_control(w, field + 1, 16);
    }
    wr32(out, focus);
    return noErr;
}

static int16_t send_focus(gaddr ch, int16_t part)
{
    return (int16_t)ctl_call(ch, kControlMsgFocus, part);
}

int16_t tb_ClearKeyboardFocus(gaddr w)
{
    int i = win_index(w, false);
    if (i < 0 || !wins[i].focus)
        return noErr;
    gaddr old = wins[i].focus;
    wins[i].focus = 0;
    gaddr saved = qd_port();
    tb_SetPort(w);
    send_focus(old, 0);
    tb_SetPort(saved);
    return noErr;
}

int16_t tb_SetKeyboardFocus(gaddr w, gaddr ch, int16_t part)
{
    if (!ch || part == 0)
        return tb_ClearKeyboardFocus(w);
    if (!(features(ch) & kControlSupportsFocus))
        return errControlDoesntSupportFocus;
    if (!ControlRecord_contrlVis(cp(ch)) || chilite(ch) == 255)
        return errCouldntSetFocus;
    int i = win_index(w, true);
    if (i < 0)
        return memFullErr;
    if (wins[i].focus && wins[i].focus != ch)
        tb_ClearKeyboardFocus(w);
    gaddr saved = qd_port();
    tb_SetPort(w);
    int16_t got = send_focus(ch, part);
    tb_SetPort(saved);
    i = win_index(w, true);
    wins[i].focus = got ? ch : 0;
    wins[i].focus_part = got;
    return got ? noErr : errCouldntSetFocus;
}

/* The controls that can take the focus, in embedding order. */
static void focus_order(gaddr ch, gaddr *out, int *n, int max)
{
    ctl_info *c = ctl_find(ch);
    if (!c || !ControlRecord_contrlVis(cp(ch)) || chilite(ch) == 255)
        return;
    if (!c->is_root && (features(ch) & kControlSupportsFocus) && *n < max)
        out[(*n)++] = ch;
    gaddr subs[512];
    int k = ctl_subcontrols(ch, subs, 512);
    for (int i = 0; i < k; i++)
        focus_order(subs[i], out, n, max);
}

static int16_t move_focus(gaddr w, bool forward)
{
    gaddr list[512];
    int n = 0;
    gaddr root = ctl_window_root(w);
    if (root)
        focus_order(root, list, &n, 512);
    else {
        gaddr all[512];
        int k = 0;
        for (gaddr x = WindowRecord_controlList(w); x && k < 512; x = ControlRecord_nextControl(cp(x)))
            all[k++] = x;
        for (int i = k - 1; i >= 0; i--)
            focus_order(all[i], list, &n, 512);
    }
    if (!n)
        return errControlDoesntSupportFocus;
    int i = win_index(w, true);
    gaddr cur = i >= 0 ? wins[i].focus : 0;
    /* Within the focused control first (a clock's fields), then the next control. */
    if (cur) {
        gaddr saved = qd_port();
        tb_SetPort(w);
        int16_t got = send_focus(cur, forward ? -1 : -2);
        tb_SetPort(saved);
        if (got)
            return noErr;
    }
    int at = -1;
    for (int k = 0; k < n; k++)
        if (list[k] == cur)
            at = k;
    int next = at < 0 ? (forward ? 0 : n - 1) : (at + (forward ? 1 : n - 1)) % n;
    if (cur)
        tb_ClearKeyboardFocus(w);
    return tb_SetKeyboardFocus(w, list[next], forward ? -1 : -2);
}

int16_t tb_AdvanceKeyboardFocus(gaddr w) { return move_focus(w, true); }
int16_t tb_ReverseKeyboardFocus(gaddr w) { return move_focus(w, false); }

int16_t tb_HandleControlKey(gaddr ch, int16_t key, int16_t chr, int16_t mods)
{
    if (!ch)
        return 0;
    /* ControlKeyDownRec: modifiers, keyCode, charCode */
    gaddr rec = tb_NewPtr(6, false, true);
    wr16(rec, (uint16_t)mods);
    wr16(rec + 2, (uint16_t)key);
    wr16(rec + 4, (uint16_t)chr);
    gaddr saved = qd_port();
    tb_SetPort(cowner(ch));
    int16_t part = (int16_t)ctl_call(ch, kControlMsgKeyDown, (int32_t)rec);
    tb_SetPort(saved);
    tb_DisposePtr(rec);
    return part;
}

void tb_IdleControls(gaddr w)
{
    gaddr list[512];
    int n = 0;
    for (gaddr x = WindowRecord_controlList(w); x && n < 512; x = ControlRecord_nextControl(cp(x)))
        list[n++] = x;
    gaddr saved = qd_port();
    tb_SetPort(w);
    for (int i = 0; i < n; i++)
        if (ControlRecord_contrlVis(cp(list[i])) && (features(list[i]) & kControlWantsIdle))
            call_cdef(list[i], 18, 0);
    tb_SetPort(saved);
}

/* ---------------------------------------------------------------------- */
/* Data access, features and the rest of Control Manager 2.0 */

int16_t tb_GetControlFeatures(gaddr ch, gaddr out)
{
    if (!ctl_find(ch))
        return errUnknownControl;
    if (!new_messages(ch))
        return errMessageNotSupported;
    wr32(out, features(ch));
    return noErr;
}

static int16_t data_access(gaddr ch, int16_t part, int32_t tag, int32_t size, gaddr data, gaddr out_size, bool set)
{
    if (!ctl_find(ch))
        return errUnknownControl;
    if (!(features(ch) & kControlSupportsDataAccess))
        return errDataNotSupported;
    gaddr rec = tb_NewPtr(16, false, true);
    wr32(rec, (uint32_t)tag);
    wr32(rec + 4, (uint32_t)(int32_t)part);
    wr32(rec + 8, (uint32_t)size);
    wr32(rec + 12, data);
    gaddr saved = qd_port();
    tb_SetPort(cowner(ch));
    int16_t err = (int16_t)call_cdef(ch, set ? 20 : 21, (int32_t)rec);
    tb_SetPort(saved);
    if (out_size)
        wr32(out_size, rd32(rec + 8));
    tb_DisposePtr(rec);
    return err;
}

int16_t tb_SetControlData(gaddr ch, int16_t part, int32_t tag, int32_t size, gaddr data)
{
    return data_access(ch, part, tag, size, data, 0, true);
}

int16_t tb_GetControlData(gaddr ch, int16_t part, int32_t tag, int32_t size, gaddr buffer, gaddr actual)
{
    return data_access(ch, part, tag, size, buffer, actual, false);
}

int16_t tb_GetControlDataSize(gaddr ch, int16_t part, int32_t tag, gaddr size)
{
    return data_access(ch, part, tag, 0, 0, size, false);
}

int32_t tb_SendControlMessage(gaddr ch, int16_t msg, int32_t param)
{
    if (!ch)
        return 0;
    gaddr saved = qd_port();
    tb_SetPort(cowner(ch));
    int32_t r = ctl_call(ch, msg, param);
    tb_SetPort(saved);
    return r;
}

int16_t tb_GetBestControlRect(gaddr ch, gaddr out, gaddr baseline)
{
    if (!ctl_find(ch))
        return errUnknownControl;
    qrect r = crect(ch);
    if (!(features(ch) & kControlSupportsCalcBestRect)) {
        qrect_put(out, r);
        if (baseline)
            wr16(baseline, 0);
        return errMessageNotSupported;
    }
    /* ControlCalcSizeRec: height, width, baseLine */
    gaddr rec = tb_NewPtr(6, false, true);
    wr16(rec, (uint16_t)(r.bottom - r.top));
    wr16(rec + 2, (uint16_t)(r.right - r.left));
    gaddr saved = qd_port();
    tb_SetPort(cowner(ch));
    call_cdef(ch, kControlMsgCalcBestRect, (int32_t)rec);
    tb_SetPort(saved);
    qrect_put(out, (qrect){ r.top, r.left, (int16_t)(r.top + (int16_t)rd16(rec)), (int16_t)(r.left + (int16_t)rd16(rec + 2)) });
    if (baseline)
        wr16(baseline, rd16(rec + 4));
    tb_DisposePtr(rec);
    return noErr;
}

/* ControlFontStyleRec: flags, font, size, style, mode, just, fore, back. */
int16_t tb_SetControlFontStyle(gaddr ch, gaddr style)
{
    ctl_info *c = ctl_find(ch);
    if (!c)
        return errUnknownControl;
    if (style) {
        gget(c->font, style, 24);
        c->has_font = (c->font[0] | c->font[1]) != 0;
    } else {
        c->has_font = false;
    }
    if (ControlRecord_contrlVis(cp(ch)))
        tb_Draw1Control(ch);
    return noErr;
}

/* The background a control erases with: its embedder's, where that has
 * one of its own (placards, tab panes, headers); else the window's. */
int16_t tb_SetUpControlBackground(gaddr ch, int16_t depth, int8_t color)
{
    ctl_info *c = ctl_find(ch);
    if (!c)
        return errUnknownControl;
    for (ctl_info *a = ctl_find(c->parent); a; a = ctl_find(a->parent))
        if (features(a->ch) & kControlHasSpecialBackground) {
            tb_SetThemeBackground(25, depth, color); /* kThemeBrushStaticAreaFill */
            return noErr;
        }
    return noErr;
}

int16_t tb_DumpControlHierarchy(gaddr w, gaddr file) { return noErr; }

/* ---------------------------------------------------------------------- */
/* Mac OS 8.5's ControlsLib: 32-bit settings, kept to the 16 bits the
 * control record holds, and a scroll bar's view size, kept (its thumb is
 * not drawn in proportion). */

static int16_t clamp16(int32_t v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

void tb_SetControl32BitValue(gaddr c, int32_t v) { tb_SetControlValue(c, clamp16(v)); }
int32_t tb_GetControl32BitValue(gaddr c) { return tb_GetControlValue(c); }
void tb_SetControl32BitMinimum(gaddr c, int32_t v) { tb_SetControlMinimum(c, clamp16(v)); }
int32_t tb_GetControl32BitMinimum(gaddr c) { return tb_GetControlMinimum(c); }
void tb_SetControl32BitMaximum(gaddr c, int32_t v) { tb_SetControlMaximum(c, clamp16(v)); }
int32_t tb_GetControl32BitMaximum(gaddr c) { return tb_GetControlMaximum(c); }

void tb_SetControlViewSize(gaddr c, int32_t size)
{
    ctl_info *i = ctl_find(c);
    if (i)
        i->view_size = size;
}

int32_t tb_GetControlViewSize(gaddr c)
{
    ctl_info *i = ctl_find(c);
    return i ? i->view_size : 0;
}

int8_t tb_IsValidControlHandle(gaddr c) { return ctl_find(c) != NULL; }
