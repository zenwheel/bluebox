/*
 * Control Manager, with native CDEF 0 (buttons, check boxes, radio buttons)
 * and CDEF 1 (scroll bars).
 */
#include <string.h>

#include "qd_internal.h"
#include "tb_internal.h"

enum { drawCntl = 0, testCntl, calcCRgns, initCntl, dispCntl, posCntl, thumbCntl, dragCntl, autoTrack };
enum { inButton = 10, inCheckBox = 11, inUpButton = 20, inDownButton = 21, inPageUp = 22, inPageDown = 23,
       inThumb = 129 };
enum { patCopy = 8, patXor = 10, patBic = 11 };

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

static struct {
    gaddr ch;
    int16_t var;
} variants[512];

static int16_t cvariant(gaddr ch)
{
    for (int i = 0; i < 512; i++)
        if (variants[i].ch == ch)
            return variants[i].var;
    return 0;
}

static void set_variant(gaddr ch, int16_t v)
{
    for (int i = 0; i < 512; i++)
        if (variants[i].ch == ch || !variants[i].ch) {
            variants[i].ch = ch;
            variants[i].var = v;
            return;
        }
}

static void forget_variant(gaddr ch)
{
    for (int i = 0; i < 512; i++)
        if (variants[i].ch == ch)
            variants[i].ch = 0;
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

/* Use the system font unless the variant asks for the window's. */
static void control_font(gaddr ch, int16_t *f, int16_t *s, uint8_t *face)
{
    gaddr port = qd_port();
    *f = GrafPort_txFont(port);
    *s = GrafPort_txSize(port);
    *face = GrafPort_txFace(port);
    if (!(cvariant(ch) & 8)) {
        GrafPort_set_txFont(port, 0);
        GrafPort_set_txSize(port, 12);
        GrafPort_set_txFace(port, 0);
    }
}

static void restore_font(int16_t f, int16_t s, uint8_t face)
{
    gaddr port = qd_port();
    GrafPort_set_txFont(port, f);
    GrafPort_set_txSize(port, s);
    GrafPort_set_txFace(port, face);
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
    int16_t f, s;
    uint8_t face;
    control_font(ch, &f, &s, &face);
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
    if (kind == 0 && app_draw_button(r, title, n, hil == inButton, hil == 255)) {
        restore_font(f, s, face);
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
    restore_font(f, s, face);
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
    if (app_colour()) {
        app_draw_scrollbar(r, v, chilite(ch), chilite(ch) != 255, chilite(ch) != 255 && cmax(ch) > cmin(ch), sb_thumb(ch));
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
    if (c < lo + 16)
        return inUpButton;
    if (c >= hi - 16)
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
    default:
        break;
    }
    wr32(c->a[7], (uint32_t)result);
    cpu_push32(c, ret);
}

void cdef0_native(cpu68k *c, uint16_t tw) { cdef_entry(c, false); }
void cdef1_native(cpu68k *c, uint16_t tw) { cdef_entry(c, true); }

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
    set_variant(ch, (int16_t)(procid & 15));
    /* Link at the head of the window's control list. */
    ControlRecord_set_nextControl(p, WindowRecord_controlList(wst));
    WindowRecord_set_controlList(wst, ch);
    call_cdef(ch, initCntl, 0);
    if (vis)
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

void tb_DisposeControl(gaddr ch)
{
    gaddr w = cowner(ch);
    tb_HideControl(ch);
    call_cdef(ch, dispCntl, 0);
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
    forget_variant(ch);
    tb_DisposeHandle(ch);
}

void tb_KillControls(gaddr w)
{
    while (WindowRecord_controlList(w))
        tb_DisposeControl(WindowRecord_controlList(w));
}

/* ---------------------------------------------------------------------- */
/* Drawing */

void tb_Draw1Control(gaddr ch)
{
    gaddr saved = qd_port();
    tb_SetPort(cowner(ch));
    call_cdef(ch, drawCntl, 0);
    tb_SetPort(saved);
}

void tb_DrawControls(gaddr w)
{
    /* Back to front: the list is front to back, so recurse. */
    gaddr list[256];
    int n = 0;
    for (gaddr x = WindowRecord_controlList(w); x && n < 256; x = ControlRecord_nextControl(cp(x)))
        list[n++] = x;
    for (int i = n - 1; i >= 0; i--)
        tb_Draw1Control(list[i]);
}

void tb_UpdateControls(gaddr wp, gaddr rh) { tb_DrawControls(wp); }

void tb_ShowControl(gaddr ch)
{
    if (ControlRecord_contrlVis(cp(ch)))
        return;
    ControlRecord_set_contrlVis(cp(ch), 255);
    tb_Draw1Control(ch);
}

void tb_HideControl(gaddr ch)
{
    if (!ControlRecord_contrlVis(cp(ch)))
        return;
    ControlRecord_set_contrlVis(cp(ch), 0);
    gaddr saved = qd_port();
    tb_SetPort(cowner(ch));
    tb_EraseRect(cp(ch) + ControlRecord_contrlRect_off);
    tb_InvalRect(cp(ch) + ControlRecord_contrlRect_off);
    tb_SetPort(saved);
}

void tb_HiliteControl(gaddr ch, int16_t state)
{
    if (chilite(ch) == (uint8_t)state)
        return;
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

void tb_MoveControl(gaddr ch, int16_t h, int16_t v)
{
    qrect r = crect(ch);
    bool vis = ControlRecord_contrlVis(cp(ch)) != 0;
    if (vis)
        tb_HideControl(ch);
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

int16_t tb_FindControl(uint32_t p, gaddr w, gaddr cpp)
{
    wr32(cpp, 0);
    if (!w || !WindowRecord_visible(w))
        return 0;
    for (gaddr x = WindowRecord_controlList(w); x; x = ControlRecord_nextControl(cp(x))) {
        int16_t part = tb_TestControl(x, p);
        if (part) {
            wr32(cpp, x);
            return part;
        }
    }
    return 0;
}

static uint32_t local_mouse(gaddr w)
{
    uint32_t g = ev_mouse_global();
    pixdst d = qd_port_pixels(w);
    return make_pt((int16_t)(pt_v(g) + d.bounds.top), (int16_t)(pt_h(g) + d.bounds.left));
}

static void call_action(gaddr action, gaddr ch, int16_t part)
{
    cpu68k *c = g_cpu;
    cpu_push32(c, ch);
    cpu_push16(c, (uint16_t)part);
    guest_call(action);
}

int16_t tb_TrackControl(gaddr ch, uint32_t p, gaddr a)
{
    int16_t part = tb_TestControl(ch, p);
    if (!part)
        return 0;
    gaddr w = cowner(ch);
    if (a == 0xFFFFFFFFu)
        a = ControlRecord_contrlAction(cp(ch));
    if (part == inThumb) {
        /* Drag the thumb; the value follows the mouse along the track. */
        qrect r = crect(ch);
        bool v = sb_vertical(r);
        int len = (v ? r.bottom - r.top : r.right - r.left) - 48;
        int start = v ? pt_v(p) : pt_h(p);
        int16_t v0 = cvalue(ch);
        int range = cmax(ch) - cmin(ch);
        while (ev_still_down()) {
            uint32_t m = local_mouse(w);
            int delta = (v ? pt_v(m) : pt_h(m)) - start;
            if (len > 0 && range > 0) {
                int nv = v0 + (int)((long)delta * range / len);
                if (nv != cvalue(ch))
                    tb_SetControlValue(ch, (int16_t)nv);
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
    return inside ? part : 0;
}

void tb_DragControl(gaddr ch, uint32_t p, gaddr limit, gaddr slop, int16_t axis)
{
    int32_t d = tb_DragGrayRgn(0, p, limit, slop, axis, 0);
    qrect r = crect(ch);
    tb_MoveControl(ch, (int16_t)(r.left + pt_h((uint32_t)d)), (int16_t)(r.top + pt_v((uint32_t)d)));
}
