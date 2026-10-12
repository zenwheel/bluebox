/*
 * Colour appearances for the standard definition procedures: colour
 * System 7 and Mac OS 8 "Platinum".  The black-and-white look stays in
 * wm.c and controls.c; these are drawn instead when an 8-bit appearance is
 * chosen.  All artwork is our own, drawn to the same geometry, so window
 * and control layouts are unchanged.
 */
#include <stdlib.h>
#include <string.h>

#include "machine.h"
#include "qd_internal.h"
#include "tb_internal.h"

typedef struct {
    uint32_t frame, title_bg, title_bg_inactive, stripe_dark, stripe_light, light, shade, text_inactive;
    uint32_t track, thumb, thumb_light, thumb_shade, arrow_bg, button_bg, button_shade;
    bool shadow;   /* System 7's one-pixel window shadow */
    bool grips;    /* Platinum: grip lines only beside the title */
} palette;

static const palette system7 = {
    .frame = 0x000000, .title_bg = 0xDDDDDD, .title_bg_inactive = 0xFFFFFF, .stripe_dark = 0x777777,
    .stripe_light = 0xFFFFFF, .light = 0xFFFFFF, .shade = 0x999999, .text_inactive = 0x888888,
    .track = 0xEEEEEE, .thumb = 0xCCCCFF, .thumb_light = 0xEEEEFF, .thumb_shade = 0x8888AA, .arrow_bg = 0xDDDDDD,
    .button_bg = 0xFFFFFF, .button_shade = 0xFFFFFF, .shadow = true, .grips = false,
};

static const palette platinum = {
    .frame = 0x000000, .title_bg = 0xDDDDDD, .title_bg_inactive = 0xEEEEEE, .stripe_dark = 0x999999,
    .stripe_light = 0xFFFFFF, .light = 0xFFFFFF, .shade = 0x888888, .text_inactive = 0x999999,
    .track = 0xBBBBBB, .thumb = 0xCCCCFF, .thumb_light = 0xEEEEFF, .thumb_shade = 0x666699, .arrow_bg = 0xDDDDDD,
    .button_bg = 0xDDDDDD, .button_shade = 0x888888, .shadow = false, .grips = true,
};

bool app_colour(void) { return g_appearance != MACHINE_APPEARANCE_BW; }
static const palette *pal(void) { return g_appearance == MACHINE_APPEARANCE_PLATINUM ? &platinum : &system7; }

/* ---------------------------------------------------------------------- */
/* Drawing helpers: solid colours in the current port, clipped */

static void fill_rgb(qrect r, uint32_t rgb)
{
    if (qrect_empty(r))
        return;
    gaddr port = qd_port();
    pixdst d = qd_port_pixels(port);
    uint16_t c[3] = { (uint16_t)(((rgb >> 16) & 0xFF) * 0x101), (uint16_t)(((rgb >> 8) & 0xFF) * 0x101),
                      (uint16_t)((rgb & 0xFF) * 0x101) };
    uint32_t v = qd_rgb_px(d.depth, d.ctab, c);
    hrgn clip, shape;
    hrgn_init(&clip);
    hrgn_init(&shape);
    qd_port_clip(port, &clip);
    hrgn_rect(&shape, r);
    hrgn_op(&shape, &shape, &clip, RGN_SECT);
    for (int i = 0; i < shape.nbands; i++) {
        const rband *b = &shape.bands[i];
        for (int y = b->top; y < b->bottom; y++)
            for (int k = 0; k < b->n; k += 2)
                for (int x = b->x[k]; x < b->x[k + 1]; x++)
                    put_px(&d, x - d.bounds.left, y - d.bounds.top, v);
    }
    hrgn_free(&shape);
    hrgn_free(&clip);
}

static void hline(int x0, int x1, int y, uint32_t rgb) { fill_rgb((qrect){ (int16_t)y, (int16_t)x0, (int16_t)(y + 1), (int16_t)x1 }, rgb); }
static void vline(int x, int y0, int y1, uint32_t rgb) { fill_rgb((qrect){ (int16_t)y0, (int16_t)x, (int16_t)y1, (int16_t)(x + 1) }, rgb); }

static void frame_rgb(qrect r, uint32_t rgb)
{
    hline(r.left, r.right, r.top, rgb);
    hline(r.left, r.right, r.bottom - 1, rgb);
    vline(r.left, r.top, r.bottom, rgb);
    vline(r.right - 1, r.top, r.bottom, rgb);
}

/* Raised (light top-left, shade bottom-right) or sunken. */
static void bevel(qrect r, uint32_t light, uint32_t shade, bool sunken)
{
    uint32_t tl = sunken ? shade : light, br = sunken ? light : shade;
    hline(r.left, r.right - 1, r.top, tl);
    vline(r.left, r.top, r.bottom - 1, tl);
    hline(r.left + 1, r.right, r.bottom - 1, br);
    vline(r.right - 1, r.top + 1, r.bottom, br);
}

static qrect inset(qrect r, int d)
{
    return (qrect){ (int16_t)(r.top + d), (int16_t)(r.left + d), (int16_t)(r.bottom - d), (int16_t)(r.right - d) };
}

/* Draw text in a colour (the port's own colour is restored). */
static void text_rgb(gaddr port, const uint8_t *text, int len, uint32_t rgb)
{
    uint16_t c[3] = { (uint16_t)(((rgb >> 16) & 0xFF) * 0x101), (uint16_t)(((rgb >> 8) & 0xFF) * 0x101),
                      (uint16_t)((rgb & 0xFF) * 0x101) };
    qd_set_fg_override(c);
    qd_draw_text(port, text, len);
    qd_set_fg_override(NULL);
}

/* ---------------------------------------------------------------------- */
/* Windows */

/* Pressed boxes: colour boxes are redrawn rather than inverted. */
#define MAX_PRESSED 8
static struct {
    gaddr w;
    int part;
} pressed[MAX_PRESSED];

static bool is_pressed(gaddr w, int part)
{
    for (int i = 0; i < MAX_PRESSED; i++)
        if (pressed[i].w == w && pressed[i].part == part)
            return true;
    return false;
}

static bool toggle_pressed(gaddr w, int part)
{
    for (int i = 0; i < MAX_PRESSED; i++)
        if (pressed[i].w == w && pressed[i].part == part) {
            pressed[i].w = 0;
            return false;
        }
    for (int i = 0; i < MAX_PRESSED; i++)
        if (!pressed[i].w) {
            pressed[i].w = w;
            pressed[i].part = part;
            return true;
        }
    return false;
}

/* A close or zoom box: 13x13 area at (top, left). */
static void title_box(int top, int left, bool zoom, bool down)
{
    const palette *p = pal();
    qrect area = { (int16_t)top, (int16_t)left, (int16_t)(top + 13), (int16_t)(left + 13) };
    fill_rgb(area, p->title_bg);
    qrect box = inset(area, 1);
    frame_rgb(box, p->frame);
    qrect in = inset(box, 1);
    fill_rgb(in, down ? p->shade : p->title_bg);
    bevel(in, p->light, p->shade, down);
    if (zoom) {
        qrect small = { in.top, in.left, (int16_t)(in.top + 5), (int16_t)(in.left + 5) };
        frame_rgb(small, p->frame);
    }
    if (down && !p->grips) {
        /* System 7's pressed close box: a small starburst. */
        int cx = in.left + (in.right - in.left) / 2, cy = in.top + (in.bottom - in.top) / 2;
        hline(cx - 3, cx + 4, cy, p->frame);
        vline(cx, cy - 3, cy + 4, p->frame);
        for (int i = -3; i <= 3; i++) {
            fill_rgb((qrect){ (int16_t)(cy + i), (int16_t)(cx + i), (int16_t)(cy + i + 1), (int16_t)(cx + i + 1) }, p->frame);
            fill_rgb((qrect){ (int16_t)(cy + i), (int16_t)(cx - i), (int16_t)(cy + i + 1), (int16_t)(cx - i + 1) }, p->frame);
        }
    }
}

void app_draw_document(gaddr w, qrect c, qrect t, bool hilited, bool goaway, bool zoom, int param)
{
    const palette *p = pal();
    if (param == 4 || param == 5 || param == 6) { /* toggle a pressed box */
        bool down = toggle_pressed(w, param);
        if (param == 4)
            title_box(t.top + 2, t.left + 7, false, down);
        else
            title_box(t.top + 2, t.right - 21, true, down);
        return;
    }
    qrect outer = { (int16_t)(t.top - 1), (int16_t)(c.left - 1), (int16_t)(c.bottom + 1), (int16_t)(c.right + 1) };
    frame_rgb(outer, p->frame);
    if (p->shadow) {
        vline(c.right + 1, t.top, c.bottom + 2, p->frame);
        hline(c.left, c.right + 2, c.bottom + 1, p->frame);
    }
    hline(c.left, c.right, c.top - 1, p->frame); /* title bar / content separator */
    fill_rgb(t, hilited ? p->title_bg : p->title_bg_inactive);
    if (hilited)
        bevel(t, p->light, p->shade, false);

    /* Title text, centred, with room cleared around it. */
    gaddr th = WindowRecord_titleHandle(w);
    uint8_t title[256];
    int n = 0;
    if (th && rd32(th)) {
        n = rd8(rd32(th));
        gget(title, rd32(th) + 1, (uint32_t)n);
    }
    gaddr port = qd_port();
    GrafPort_set_txFont(port, 0);
    GrafPort_set_txSize(port, 12);
    GrafPort_set_txFace(port, 0);
    GrafPort_set_txMode(port, 1);
    int tw = n ? qd_text_width(port, title, n) : 0;
    int avail = t.right - t.left - 64;
    if (tw > avail)
        tw = avail;
    int tx = t.left + (t.right - t.left - tw) / 2;

    if (hilited) {
        int x0 = t.left + 2, x1 = t.right - 2;
        int gap0 = n ? tx - 6 : x1, gap1 = n ? tx + tw + 6 : x1;
        for (int y = t.top + 3; y <= t.top + 13; y += 2) {
            hline(x0, gap0, y, p->stripe_dark);
            hline(x0 + 1, gap0, y + 1, p->stripe_light);
            if (n) {
                hline(gap1, x1, y, p->stripe_dark);
                hline(gap1 + 1, x1, y + 1, p->stripe_light);
            }
        }
        if (goaway)
            title_box(t.top + 2, t.left + 7, false, is_pressed(w, 4));
        if (zoom)
            title_box(t.top + 2, t.right - 21, true, is_pressed(w, 5));
    }
    if (n) {
        tb_MoveTo((int16_t)tx, (int16_t)(t.top + 13));
        text_rgb(port, title, n, hilited ? 0x000000 : p->text_inactive);
    }
}

void app_draw_grow(qrect c, bool hilited)
{
    const palette *p = pal();
    qrect box = { (int16_t)(c.bottom - 15), (int16_t)(c.right - 15), c.bottom, c.right };
    vline(c.right - 16, c.top, c.bottom, p->frame);
    hline(c.left, c.right, c.bottom - 16, p->frame);
    fill_rgb(box, hilited ? p->arrow_bg : 0xFFFFFF);
    if (!hilited)
        return;
    bevel(box, p->light, p->shade, false);
    qrect big = { (int16_t)(box.top + 4), (int16_t)(box.left + 4), (int16_t)(box.bottom - 2), (int16_t)(box.right - 2) };
    qrect small = { (int16_t)(box.top + 3), (int16_t)(box.left + 3), (int16_t)(box.top + 8), (int16_t)(box.left + 8) };
    frame_rgb(big, p->shade);
    fill_rgb(inset(small, 0), p->arrow_bg);
    frame_rgb(small, p->shade);
}

/* ---------------------------------------------------------------------- */
/* Scroll bars */

static void arrow(qrect box, int dir, uint32_t rgb)
{
    int cx = (box.left + box.right) / 2, cy = (box.top + box.bottom) / 2;
    for (int i = 0; i < 4; i++) {
        if (dir < 2) {
            int y = dir == 0 ? cy - 2 + i : cy + 1 - i;
            hline(cx - i, cx + i + 1, y, rgb);
        } else {
            int x = dir == 2 ? cx - 2 + i : cx + 1 - i;
            vline(x, cy - i, cy + i + 1, rgb);
        }
    }
}

void app_draw_scrollbar(qrect r, bool vertical, int hilite, bool enabled, bool active, qrect thumb)
{
    const palette *p = pal();
    frame_rgb(r, p->frame);
    qrect in = inset(r, 1);
    if (!enabled) { /* inactive window: an empty, white bar */
        fill_rgb(in, 0xFFFFFF);
        return;
    }
    qrect a1 = vertical ? (qrect){ r.top, r.left, (int16_t)(r.top + 16), r.right }
                        : (qrect){ r.top, r.left, r.bottom, (int16_t)(r.left + 16) };
    qrect a2 = vertical ? (qrect){ (int16_t)(r.bottom - 16), r.left, r.bottom, r.right }
                        : (qrect){ r.top, (int16_t)(r.right - 16), r.bottom, r.right };
    qrect track = vertical ? (qrect){ (int16_t)(r.top + 16), (int16_t)(r.left + 1), (int16_t)(r.bottom - 16), (int16_t)(r.right - 1) }
                           : (qrect){ (int16_t)(r.top + 1), (int16_t)(r.left + 16), (int16_t)(r.bottom - 1), (int16_t)(r.right - 16) };
    fill_rgb(track, active ? p->track : p->arrow_bg);
    if (active) /* the track is sunken */
        bevel(track, p->shade, p->light, false);
    for (int k = 0; k < 2; k++) {
        qrect a = k ? a2 : a1;
        bool down = hilite == (k ? 21 : 20); /* inDownButton, inUpButton */
        frame_rgb(a, p->frame);
        qrect ai = inset(a, 1);
        fill_rgb(ai, down ? p->shade : p->arrow_bg);
        bevel(ai, p->light, p->shade, down);
        arrow(ai, vertical ? (k ? 1 : 0) : (k ? 3 : 2), active ? 0x000000 : p->shade);
    }
    if (!active)
        return;
    frame_rgb(thumb, p->frame);
    qrect ti = inset(thumb, 1);
    fill_rgb(ti, p->thumb);
    bevel(ti, p->thumb_light, p->thumb_shade, false);
    if (p->grips) { /* grip lines across the middle of the thumb */
        int cx = (ti.left + ti.right) / 2, cy = (ti.top + ti.bottom) / 2;
        for (int i = -2; i <= 2; i += 2) {
            if (vertical) {
                hline(ti.left + 3, ti.right - 3, cy + i, p->thumb_shade);
                hline(ti.left + 4, ti.right - 2, cy + i + 1, p->thumb_light);
            } else {
                vline(cx + i, ti.top + 3, ti.bottom - 3, p->thumb_shade);
                vline(cx + i + 1, ti.top + 4, ti.bottom - 2, p->thumb_light);
            }
        }
    }
}

/* ---------------------------------------------------------------------- */
/* Push buttons (Platinum only; colour System 7 kept black-and-white ones) */

bool app_draw_button(qrect r, const uint8_t *title, int n, bool pressed_in, bool disabled)
{
    if (g_appearance != MACHINE_APPEARANCE_PLATINUM)
        return false;
    const palette *p = pal();
    gaddr port = qd_port();
    /* A rounded frame: corners cut by a pixel. */
    hline(r.left + 2, r.right - 2, r.top, p->frame);
    hline(r.left + 2, r.right - 2, r.bottom - 1, p->frame);
    vline(r.left, r.top + 2, r.bottom - 2, p->frame);
    vline(r.right - 1, r.top + 2, r.bottom - 2, p->frame);
    fill_rgb((qrect){ (int16_t)(r.top + 1), (int16_t)(r.left + 1), (int16_t)(r.top + 2), (int16_t)(r.left + 2) }, p->frame);
    fill_rgb((qrect){ (int16_t)(r.top + 1), (int16_t)(r.right - 2), (int16_t)(r.top + 2), (int16_t)(r.right - 1) }, p->frame);
    fill_rgb((qrect){ (int16_t)(r.bottom - 2), (int16_t)(r.left + 1), (int16_t)(r.bottom - 1), (int16_t)(r.left + 2) }, p->frame);
    fill_rgb((qrect){ (int16_t)(r.bottom - 2), (int16_t)(r.right - 2), (int16_t)(r.bottom - 1), (int16_t)(r.right - 1) }, p->frame);
    qrect in = { (int16_t)(r.top + 1), (int16_t)(r.left + 1), (int16_t)(r.bottom - 1), (int16_t)(r.right - 1) };
    fill_rgb(in, pressed_in ? p->shade : p->button_bg);
    bevel(inset(in, 0), pressed_in ? p->shade : p->light, pressed_in ? p->light : p->button_shade, false);
    gaddr fi = tb_NewPtr(8, false, false);
    tb_GetFontInfo(fi);
    int asc = (int16_t)rd16(fi), desc = (int16_t)rd16(fi + 2);
    tb_DisposePtr(fi);
    int tw = qd_text_width(port, title, n);
    tb_MoveTo((int16_t)(r.left + (r.right - r.left - tw) / 2), (int16_t)(r.top + (r.bottom - r.top + asc - desc) / 2));
    text_rgb(port, title, n, disabled ? p->text_inactive : pressed_in ? 0xFFFFFF : 0x000000);
    return true;
}
