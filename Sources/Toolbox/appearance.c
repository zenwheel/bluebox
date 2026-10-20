/*
 * The Appearance Manager, as Mac OS 8.5 had it (1.1): theme brushes and
 * text colours, the Platinum look's drawing primitives, metrics, fonts and
 * cursors, and which programs are its clients.
 *
 * Platinum is drawn here with our QuickDraw: greys, one-pixel bevels and
 * the accent colour (the host's), in the current port through its clip.
 * Below 4 bits a pixel it is black and white, as the Appearance Manager
 * drew it on such screens.  The highlight brushes follow the host's
 * selection and accent colours, and HiliteRGB follows them too.
 */
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "qd_internal.h"
#include "tb_internal.h"

enum {
    themeInvalidBrushErr = -30560,
    themeProcessRegisteredErr = -30561,
    themeProcessNotRegisteredErr = -30562,
    themeBadTextColorErr = -30563,
    themeHasNoAccentsErr = -30564,
    themeBadCursorIndexErr = -30565,
};
enum { patCopy = 8 };
enum { kThemeStateInactive = 0, kThemeStateActive = 1, kThemeStatePressed = 2 };

/* ---------------------------------------------------------------------- */
/* Colours */

/* The host's selection and accent colours: set from any thread, taken up
 * by the machine at its next event call. */
static _Atomic uint64_t host_hilite = 0, host_accent = 0;
static atomic_bool host_colors_changed;
static uint16_t accent[3] = { 0x6666, 0x6666, 0xCCCC }; /* Mac OS 8's lavender, without a host */

static uint64_t pack_rgb(uint16_t r, uint16_t g, uint16_t b)
{
    return 1ull << 48 | (uint64_t)r << 32 | (uint64_t)g << 16 | b;
}

void machine_set_accent_color(uint16_t r, uint16_t g, uint16_t b)
{
    atomic_store(&host_accent, pack_rgb(r, g, b));
    atomic_store(&host_colors_changed, true);
}

/* The highlight colour changed after boot (machine.c keeps the one for
 * boot): HiliteRGB follows at the next event call. */
void appearance_hilite_changed(uint16_t r, uint16_t g, uint16_t b)
{
    atomic_store(&host_hilite, pack_rgb(r, g, b));
    atomic_store(&host_colors_changed, true);
}

static void unpack_rgb(uint64_t v, uint16_t out[3])
{
    out[0] = (uint16_t)(v >> 32);
    out[1] = (uint16_t)(v >> 16);
    out[2] = (uint16_t)v;
}

/* At event calls: the host's colours as they are now. */
void appearance_poll(void)
{
    if (!atomic_exchange(&host_colors_changed, false))
        return;
    uint64_t a = atomic_load(&host_accent), h = atomic_load(&host_hilite);
    if (a)
        unpack_rgb(a, accent);
    if (h) {
        uint16_t rgb[3];
        unpack_rgb(h, rgb);
        wr16(LM_HiliteRGB, rgb[0]);
        wr16(LM_HiliteRGB + 2, rgb[1]);
        wr16(LM_HiliteRGB + 4, rgb[2]);
    }
}

static void grey(uint16_t out[3], uint16_t v) { out[0] = out[1] = out[2] = v; }

/* The accent colour mixed towards white (t > 0) or black (t < 0), t in 1/16ths. */
static void accent_shade(int t, uint16_t out[3])
{
    for (int i = 0; i < 3; i++) {
        int v = accent[i];
        out[i] = (uint16_t)(t >= 0 ? v + (0xFFFF - v) * t / 16 : v * (16 + t) / 16);
    }
}

/* A brush's colour in Platinum. */
bool ap_brush_rgb(int brush, uint16_t out[3])
{
    switch (brush) {
    case -1: grey(out, 0); return true;           /* kThemeBrushBlack */
    case -2: grey(out, 0xFFFF); return true;      /* kThemeBrushWhite */
    case -3:                                      /* kThemeBrushPrimaryHighlightColor */
        out[0] = rd16(LM_HiliteRGB);
        out[1] = rd16(LM_HiliteRGB + 2);
        out[2] = rd16(LM_HiliteRGB + 4);
        return true;
    case -4: grey(out, 0xCCCC); return true;      /* kThemeBrushSecondaryHighlightColor */
    case -5: accent_shade(0, out); return true;   /* kThemeBrushAlternatePrimaryHighlightColor */
    case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8: /* dialog, alert, modeless, utility */
        grey(out, 0xDDDD);
        return true;
    case 9: grey(out, 0xEEEE); return true;       /* list view sort column */
    case 10: case 11: case 15: case 16: grey(out, 0xFFFF); return true;
    case 12: grey(out, 0xDDDD); return true;      /* list view separator */
    case 13: grey(out, 0); return true;           /* chasing arrows */
    case 14: accent_shade(4, out); return true;   /* drag hilite */
    case 17: grey(out, 0); return true;           /* scroll bar delimiter */
    case 18: grey(out, 0x8888); return true;
    case 19: accent_shade(4, out); return true;   /* focus highlight */
    case 20: grey(out, 0); return true;           /* pop-up arrow */
    case 21: grey(out, 0xFFFF); return true;
    case 22: grey(out, 0x8888); return true;
    case 23: out[0] = 0xFFFF; out[1] = 0x3333; out[2] = 0x3333; return true; /* coachmark */
    case 24: grey(out, 0); return true;           /* selected icon label */
    case 25: grey(out, 0xDDDD); return true;      /* static area */
    case 26: grey(out, 0xEEEE); return true;      /* active area */
    case 27: grey(out, 0); return true;           /* button frame */
    case 28: grey(out, 0x8888); return true;
    case 29: case 30: grey(out, 0xDDDD); return true; /* button face */
    case 31: grey(out, 0x7777); return true;
    case 32: grey(out, 0x8888); return true;      /* active dark shadow */
    case 33: grey(out, 0xFFFF); return true;
    case 34: grey(out, 0xAAAA); return true;
    case 35: grey(out, 0xEEEE); return true;
    case 36: grey(out, 0xBBBB); return true;      /* inactive */
    case 37: grey(out, 0xEEEE); return true;
    case 38: grey(out, 0xCCCC); return true;
    case 39: grey(out, 0xEEEE); return true;
    case 40: grey(out, 0x5555); return true;      /* pressed */
    case 41: grey(out, 0x9999); return true;
    case 42: grey(out, 0x6666); return true;
    case 43: grey(out, 0x8888); return true;
    case 44: grey(out, 0xFFFF); return true;      /* bevel */
    case 45: grey(out, 0x8888); return true;
    case 46: grey(out, 0xEEEE); return true;
    case 47: grey(out, 0xBBBB); return true;
    default: return false;
    }
}

/* A text colour in Platinum. */
bool ap_text_rgb(int color, uint16_t out[3])
{
    switch (color) {
    case -1: grey(out, 0); return true;
    case -2: grey(out, 0xFFFF); return true;
    /* Pressed buttons, placards and tabs, selected menus and icon labels: white. */
    case 11: case 14: case 17: case 20: case 32: case 35: case 41: case 44:
        grey(out, 0xFFFF);
        return true;
    /* Inactive and disabled ones: grey. */
    case 2: case 4: case 6: case 8: case 10: case 13: case 16: case 19: case 24: case 26: case 28: case 30:
    case 33: case 36: case 38: case 42: case 43: case 46:
        grey(out, 0x7777);
        return true;
    default:
        if (color >= 1 && color <= 46) {
            grey(out, 0);
            return true;
        }
        return false;
    }
}

/* ---------------------------------------------------------------------- */
/* Drawing: fills in RGB through the port's clip, whatever its kind */

/* Colour (4 bits a pixel and more), or black and white. */
bool ap_in_color(void) { return qd_port_pixels(qd_port()).depth >= 4; }

void ap_fill_rgn(const hrgn *g, const uint16_t rgb[3])
{
    uint16_t c[3] = { rgb[0], rgb[1], rgb[2] };
    if (!ap_in_color()) { /* black, or white */
        uint16_t v = (uint16_t)((rgb[0] * 3u + rgb[1] * 6u + rgb[2]) / 10u) >= 0x6000 ? 0xFFFF : 0;
        grey(c, v);
    }
    qd_set_fg_override(c);
    qd_fill_region(qd_port(), g, qd_pat_black, patCopy);
    qd_set_fg_override(NULL);
}

void ap_fill(qrect r, const uint16_t rgb[3])
{
    if (qrect_empty(r))
        return;
    hrgn g;
    hrgn_init(&g);
    hrgn_rect(&g, r);
    ap_fill_rgn(&g, rgb);
    hrgn_free(&g);
}

void ap_fill_grey(qrect r, uint16_t v)
{
    uint16_t c[3] = { v, v, v };
    ap_fill(r, c);
}

/* A one-pixel frame just inside r. */
void ap_frame(qrect r, const uint16_t rgb[3])
{
    ap_fill((qrect){ r.top, r.left, (int16_t)(r.top + 1), r.right }, rgb);
    ap_fill((qrect){ (int16_t)(r.bottom - 1), r.left, r.bottom, r.right }, rgb);
    ap_fill((qrect){ r.top, r.left, r.bottom, (int16_t)(r.left + 1) }, rgb);
    ap_fill((qrect){ r.top, (int16_t)(r.right - 1), r.bottom, r.right }, rgb);
}

void ap_frame_grey(qrect r, uint16_t v)
{
    uint16_t c[3] = { v, v, v };
    ap_frame(r, c);
}

/* Light on the top and left edges, dark on the bottom and right. */
void ap_bevel(qrect r, uint16_t light, uint16_t dark)
{
    if (!ap_in_color())
        return;
    ap_fill_grey((qrect){ r.top, r.left, (int16_t)(r.top + 1), (int16_t)(r.right - 1) }, light);
    ap_fill_grey((qrect){ r.top, r.left, (int16_t)(r.bottom - 1), (int16_t)(r.left + 1) }, light);
    ap_fill_grey((qrect){ (int16_t)(r.bottom - 1), (int16_t)(r.left + 1), r.bottom, r.right }, dark);
    ap_fill_grey((qrect){ (int16_t)(r.top + 1), (int16_t)(r.right - 1), r.bottom, r.right }, dark);
}

static qrect inset(qrect r, int d)
{
    return (qrect){ (int16_t)(r.top + d), (int16_t)(r.left + d), (int16_t)(r.bottom - d), (int16_t)(r.right - d) };
}

/* A rounded rectangle's ring: the shape of r less the shape inset by w. */
static void rrect_ring(qrect r, int w, int round, const uint16_t rgb[3])
{
    hrgn a, b;
    hrgn_init(&a);
    hrgn_init(&b);
    qd_rrect_region(&a, r, round, round);
    qrect in = inset(r, w);
    if (!qrect_empty(in)) {
        qd_rrect_region(&b, in, round - 2 * w > 0 ? round - 2 * w : 0, round - 2 * w > 0 ? round - 2 * w : 0);
        hrgn_op(&a, &a, &b, RGN_DIFF);
    }
    ap_fill_rgn(&a, rgb);
    hrgn_free(&a);
    hrgn_free(&b);
}

static void rrect_fill(qrect r, int round, const uint16_t rgb[3])
{
    hrgn a;
    hrgn_init(&a);
    qd_rrect_region(&a, r, round, round);
    ap_fill_rgn(&a, rgb);
    hrgn_free(&a);
}

/* Text at (x, y) in a colour, in the port's font. */
void ap_text(int x, int y, const uint8_t *s, int n, const uint16_t rgb[3])
{
    gaddr port = qd_port();
    uint16_t c[3] = { rgb[0], rgb[1], rgb[2] };
    if (!ap_in_color() && (c[0] | c[1] | c[2]) && (c[0] & c[1] & c[2]) != 0xFFFF)
        grey(c, 0); /* grey text is black (dimmed by the caller) */
    tb_MoveTo((int16_t)x, (int16_t)y);
    qd_set_fg_override(c);
    qd_draw_text(port, s, n);
    qd_set_fg_override(NULL);
}

/* The port's font's ascent and descent. */
void ap_font_info(int *ascent, int *descent)
{
    gaddr fi = tb_NewPtr(8, false, false);
    tb_GetFontInfo(fi);
    *ascent = (int16_t)rd16(fi);
    *descent = (int16_t)rd16(fi + 2);
    tb_DisposePtr(fi);
}

/* Text centred in r (or from its left edge), clipped to it. */
void ap_text_in(qrect r, const uint8_t *s, int n, bool center, const uint16_t rgb[3])
{
    int asc, desc;
    ap_font_info(&asc, &desc);
    gaddr port = qd_port();
    while (n > 0 && qd_text_width(port, s, n) > r.right - r.left)
        n--;
    int w = qd_text_width(port, s, n);
    int x = center ? r.left + (r.right - r.left - w) / 2 : r.left;
    int y = r.top + (r.bottom - r.top + asc - desc + 1) / 2;
    ap_text(x, y, s, n, rgb);
}

/* A filled triangle pointing in dir (0 left, 1 down, 2 right, 3 up: the
 * ThemeArrowOrientation order) with its base `size` pixels, centred in r. */
void ap_arrow(qrect r, int dir, int size, const uint16_t rgb[3])
{
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    int h = (size + 1) / 2;
    hrgn g;
    hrgn_init(&g);
    if (dir == 1 || dir == 3) {
        int top = cy - h / 2;
        for (int i = 0; i < h; i++) {
            int half = dir == 1 ? h - 1 - i : i;
            int x0 = cx - half - (size % 2 ? 0 : 1), x1 = cx + half + 1;
            hrgn_add_row(&g, top + i, x0, x1);
        }
    } else {
        /* Columns: build it row by row. */
        int left = cx - h / 2;
        for (int y = cy - h + 1; y <= cy + h - 1; y++) {
            int d = abs(y - cy), len = h - d;
            if (len <= 0)
                continue;
            int x0 = dir == 2 ? left : left + h - len, x1 = dir == 2 ? left + len : left + h;
            hrgn_add_row(&g, y, x0, x1);
        }
    }
    ap_fill_rgn(&g, rgb);
    hrgn_free(&g);
}

/* Erase with the port's background (pattern and colour). */
static void erase_rgn(const hrgn *g)
{
    uint8_t bk[8];
    gaddr port = qd_port();
    qd_port_pattern(port, 1, bk);
    qd_fill_region(port, g, bk, patCopy);
}

/* ---------------------------------------------------------------------- */
/* Clients */

static bool client;

bool appearance_client(void) { return client; }

/* At boot: the accent as the host gave it; the highlight colour is in
 * HiliteRGB already (machine.c), and only a later change is taken up, so
 * that a program's own HiliteRGB stays until the host's colour changes. */
void appearance_reset(void)
{
    client = false;
    atomic_store(&host_colors_changed, false);
    atomic_store(&host_hilite, 0);
    uint64_t a = atomic_load(&host_accent);
    if (a)
        unpack_rgb(a, accent);
}

int32_t tb_RegisterAppearanceClient(void)
{
    if (client)
        return themeProcessRegisteredErr;
    client = true;
    return noErr;
}

int32_t tb_UnregisterAppearanceClient(void)
{
    if (!client)
        return themeProcessNotRegisteredErr;
    client = false;
    return noErr;
}

int8_t tb_IsAppearanceClient(gaddr process) { return client; }

/* ---------------------------------------------------------------------- */
/* Brushes and text colours in the port */

static void set_rgb(bool fore, const uint16_t rgb[3])
{
    gaddr c = tb_NewPtr(6, false, false);
    wr16(c, rgb[0]);
    wr16(c + 2, rgb[1]);
    wr16(c + 4, rgb[2]);
    if (fore)
        tb_RGBForeColor(c);
    else
        tb_RGBBackColor(c);
    tb_DisposePtr(c);
}

/* The brush, or its nearest in black and white. */
static bool brush_for_depth(int brush, int depth, bool color, uint16_t out[3])
{
    if (!ap_brush_rgb(brush, out))
        return false;
    if (depth > 0 && depth < 4) {
        uint16_t v = (uint16_t)((out[0] * 3u + out[1] * 6u + out[2]) / 10u) >= 0x6000 ? 0xFFFF : 0;
        grey(out, v);
    }
    return true;
}

int32_t tb_SetThemePen(int16_t brush, int16_t depth, int8_t color)
{
    uint16_t rgb[3];
    if (!brush_for_depth(brush, depth, color, rgb))
        return themeInvalidBrushErr;
    gaddr black = tb_NewPtr(8, false, false);
    gmemset(black, 0xFF, 8);
    tb_PenPat(black);
    tb_DisposePtr(black);
    set_rgb(true, rgb);
    return noErr;
}

int32_t tb_SetThemeBackground(int16_t brush, int16_t depth, int8_t color)
{
    uint16_t rgb[3];
    if (!brush_for_depth(brush, depth, color, rgb))
        return themeInvalidBrushErr;
    gaddr white = tb_NewPtr(8, false, true);
    tb_BackPat(white);
    tb_DisposePtr(white);
    set_rgb(false, rgb);
    return noErr;
}

int32_t tb_SetThemeTextColor(int16_t color, int16_t depth, int8_t is_color)
{
    uint16_t rgb[3];
    if (!ap_text_rgb(color, rgb))
        return themeBadTextColorErr;
    if (depth > 0 && depth < 4 && (rgb[0] | rgb[1] | rgb[2]) && (rgb[0] & rgb[1] & rgb[2]) != 0xFFFF)
        grey(rgb, 0);
    set_rgb(true, rgb);
    return noErr;
}

int32_t tb_GetThemeBrushAsColor(int16_t brush, int16_t depth, int8_t color, gaddr out)
{
    uint16_t rgb[3];
    if (!brush_for_depth(brush, depth, color, rgb))
        return themeInvalidBrushErr;
    wr16(out, rgb[0]);
    wr16(out + 2, rgb[1]);
    wr16(out + 4, rgb[2]);
    return noErr;
}

int32_t tb_GetThemeTextColor(int16_t color, int16_t depth, int8_t is_color, gaddr out)
{
    uint16_t rgb[3];
    if (!ap_text_rgb(color, rgb))
        return themeBadTextColorErr;
    wr16(out, rgb[0]);
    wr16(out + 2, rgb[1]);
    wr16(out + 4, rgb[2]);
    return noErr;
}

/* The window's background: its port's background colour, which the Window
 * Manager erases uncovered content with. */
int32_t tb_SetThemeWindowBackground(gaddr w, int16_t brush, int8_t update)
{
    uint16_t rgb[3];
    if (!ap_brush_rgb(brush, rgb))
        return themeInvalidBrushErr;
    if (!w)
        return paramErr;
    gaddr saved = qd_port();
    tb_SetPort(w);
    gaddr white = tb_NewPtr(8, false, true);
    tb_BackPat(white);
    tb_DisposePtr(white);
    set_rgb(false, rgb);
    if (update) {
        gaddr r = tb_NewPtr(8, false, false);
        gmemcpy(r, w + GrafPort_portRect_off, 8);
        tb_EraseRect(r);
        tb_InvalRect(r);
        tb_DisposePtr(r);
    }
    tb_SetPort(saved);
    return noErr;
}

int32_t tb_SetThemeTextColorForWindow(gaddr w, int8_t active, int16_t depth, int8_t color)
{
    return tb_SetThemeTextColor(active ? 1 : 2, depth, color);
}

int8_t tb_IsThemeInColor(int16_t depth, int8_t color) { return depth >= 4; }

/* The accent colours: a ramp from nearly white to dark. */
int32_t tb_GetThemeAccentColors(gaddr out)
{
    enum { n = 8 };
    gaddr h = tb_NewHandle(8 + n * 8, false, true);
    if (!h)
        return memFullErr;
    gaddr p = rd32(h);
    wr32(p, 0);
    wr16(p + 4, 0x8000);
    wr16(p + 6, n - 1);
    static const int8_t shades[n] = { 14, 12, 9, 6, 3, 0, -4, -8 };
    for (int i = 0; i < n; i++) {
        uint16_t rgb[3];
        accent_shade(shades[i], rgb);
        wr16(p + 8 + i * 8, (uint16_t)i);
        wr16(p + 10 + i * 8, rgb[0]);
        wr16(p + 12 + i * 8, rgb[1]);
        wr16(p + 14 + i * 8, rgb[2]);
    }
    wr32(out, h);
    return noErr;
}

/* The drawing state: pen, colours, patterns and text mode, to put back. */
typedef struct {
    uint8_t pen[18];
    uint16_t fore[3], back[3];
    int16_t mode;
    gaddr bk_pixpat;
    uint8_t bk_pat[8];
} drawing_state;

static void get_state(drawing_state *s)
{
    gaddr port = qd_port();
    gaddr ps = tb_NewPtr(18, false, false);
    tb_GetPenState(ps);
    gget(s->pen, ps, 18);
    gaddr c = tb_NewPtr(6, false, false);
    tb_GetForeColor(c);
    for (int i = 0; i < 3; i++)
        s->fore[i] = rd16(c + i * 2u);
    tb_GetBackColor(c);
    for (int i = 0; i < 3; i++)
        s->back[i] = rd16(c + i * 2u);
    tb_DisposePtr(c);
    tb_DisposePtr(ps);
    s->mode = GrafPort_txMode(port);
    qd_port_pattern(port, 1, s->bk_pat);
    s->bk_pixpat = qd_port_is_color(port) ? CGrafPort_bkPixPat(port) : 0;
}

static void set_state(const drawing_state *s)
{
    gaddr ps = tb_NewPtr(18, false, false);
    gput(ps, s->pen, 18);
    tb_SetPenState(ps);
    tb_DisposePtr(ps);
    set_rgb(true, s->fore);
    set_rgb(false, s->back);
    tb_TextMode(s->mode);
    if (s->bk_pixpat)
        tb_BackPixPat(s->bk_pixpat);
    else {
        gaddr p = tb_NewPtr(8, false, false);
        gput(p, s->bk_pat, 8);
        tb_BackPat(p);
        tb_DisposePtr(p);
    }
}

int32_t tb_GetThemeDrawingState(gaddr out)
{
    gaddr p = tb_NewPtr((int32_t)sizeof(drawing_state), false, true);
    if (!p)
        return memFullErr;
    drawing_state s;
    get_state(&s);
    gput(p, &s, sizeof s);
    wr32(out, p);
    return noErr;
}

int32_t tb_SetThemeDrawingState(gaddr state, int8_t dispose)
{
    if (!state)
        return paramErr;
    drawing_state s;
    gget(&s, state, sizeof s);
    set_state(&s);
    if (dispose)
        tb_DisposePtr(state);
    return noErr;
}

int32_t tb_DisposeThemeDrawingState(gaddr state)
{
    if (state)
        tb_DisposePtr(state);
    return noErr;
}

/* Black pen, white background, black on white text in srcOr. */
int32_t tb_NormalizeThemeDrawingState(void)
{
    tb_PenNormal();
    uint16_t black[3] = { 0, 0, 0 }, white[3] = { 0xFFFF, 0xFFFF, 0xFFFF };
    set_rgb(true, black);
    gaddr pat = tb_NewPtr(8, false, true);
    tb_BackPat(pat);
    tb_DisposePtr(pat);
    set_rgb(false, white);
    tb_TextMode(1);
    return noErr;
}

int32_t tb_ApplyThemeBackground(uint32_t kind, gaddr bounds, uint32_t state, int16_t depth, int8_t color)
{
    /* Tab panes, placards and headers are all Platinum's grey. */
    return tb_SetThemeBackground(kind == 0 ? 1 : 25, depth, color);
}

int8_t tb_IsValidAppearanceFileType(int32_t type)
{
    return type == (int32_t)fourcc("thme") || type == (int32_t)fourcc("pltn") || type == (int32_t)fourcc("scen") ||
           type == (int32_t)fourcc("tsnd");
}

/* ---------------------------------------------------------------------- */
/* Primitives */

static qrect rect_arg(gaddr r) { return qrect_get(r); }

/* A window header: a grey band with a light top edge and a dark bottom one. */
static void window_header(qrect r, uint32_t state, bool list_view)
{
    bool active = state != kThemeStateInactive;
    ap_fill_grey(r, 0xDDDD);
    if (ap_in_color()) {
        ap_fill_grey((qrect){ r.top, r.left, (int16_t)(r.top + 1), r.right }, 0xFFFF);
        ap_fill_grey((qrect){ (int16_t)(r.bottom - 2), r.left, (int16_t)(r.bottom - 1), r.right }, active ? 0xAAAA : 0xCCCC);
    }
    if (!list_view)
        ap_fill_grey((qrect){ (int16_t)(r.bottom - 1), r.left, r.bottom, r.right }, active ? 0 : 0x8888);
}

int32_t tb_DrawThemeWindowHeader(gaddr r, uint32_t state)
{
    window_header(rect_arg(r), state, false);
    return noErr;
}

int32_t tb_DrawThemeWindowListViewHeader(gaddr r, uint32_t state)
{
    window_header(rect_arg(r), state, true);
    return noErr;
}

/* A placard: a raised grey panel in a frame. */
void ap_placard(qrect r, uint32_t state)
{
    bool active = state != kThemeStateInactive, pressed = state == kThemeStatePressed;
    ap_frame_grey(r, active ? 0 : 0x8888);
    qrect in = inset(r, 1);
    ap_fill_grey(in, pressed ? 0x8888 : 0xDDDD);
    if (pressed)
        ap_bevel(in, 0x6666, 0xAAAA);
    else if (active)
        ap_bevel(in, 0xFFFF, 0xAAAA);
}

int32_t tb_DrawThemePlacard(gaddr r, uint32_t state)
{
    ap_placard(rect_arg(r), state);
    return noErr;
}

/* An edit text or list box frame, outside r: a dark ring in an etched one. */
void ap_well_frame(qrect r, uint32_t state)
{
    bool active = state != kThemeStateInactive;
    qrect outer = inset(r, -2), ring = inset(r, -1);
    if (ap_in_color()) {
        ap_bevel(outer, active ? 0x8888 : 0xAAAA, 0xFFFF);
        ap_frame_grey(ring, active ? 0 : 0x8888);
    } else {
        ap_frame_grey(ring, 0);
    }
}

int32_t tb_DrawThemeEditTextFrame(gaddr r, uint32_t state)
{
    ap_well_frame(rect_arg(r), state);
    return noErr;
}

int32_t tb_DrawThemeListBoxFrame(gaddr r, uint32_t state)
{
    ap_well_frame(rect_arg(r), state);
    return noErr;
}

int32_t tb_DrawThemeGenericWell(gaddr rp, uint32_t state, int8_t fill)
{
    qrect r = rect_arg(rp);
    ap_well_frame(inset(r, 2), state);
    if (fill)
        ap_fill_grey(inset(r, 2), 0xFFFF);
    return noErr;
}

int32_t tb_DrawThemeModelessDialogFrame(gaddr rp, uint32_t state)
{
    qrect r = rect_arg(rp);
    ap_frame_grey(r, state != kThemeStateInactive ? 0 : 0x8888);
    ap_bevel(inset(r, 1), 0xFFFF, 0xAAAA);
    return noErr;
}

/* The keyboard focus: a ring in the accent colour, two pixels wide and
 * a pixel outside the shape; without the focus, the background over it. */
static void focus_ring(const hrgn *shape, bool focus)
{
    hrgn outer, inner;
    hrgn_init(&outer);
    hrgn_init(&inner);
    hrgn_copy(&outer, shape);
    hrgn_inset(&outer, -3, -3);
    hrgn_copy(&inner, shape);
    hrgn_inset(&inner, -1, -1);
    hrgn_op(&outer, &outer, &inner, RGN_DIFF);
    if (focus) {
        uint16_t c[3];
        accent_shade(4, c);
        if (!ap_in_color())
            grey(c, 0);
        ap_fill_rgn(&outer, c);
    } else {
        erase_rgn(&outer);
    }
    hrgn_free(&outer);
    hrgn_free(&inner);
}

int32_t tb_DrawThemeFocusRect(gaddr rp, int8_t focus)
{
    hrgn g;
    hrgn_init(&g);
    hrgn_rect(&g, rect_arg(rp));
    focus_ring(&g, focus);
    hrgn_free(&g);
    return noErr;
}

int32_t tb_DrawThemeFocusRegion(gaddr rh, int8_t focus)
{
    hrgn g;
    hrgn_init(&g);
    rgn_read(rh, &g);
    focus_ring(&g, focus);
    hrgn_free(&g);
    return noErr;
}

void ap_focus_rect(qrect r, bool focus)
{
    hrgn g;
    hrgn_init(&g);
    hrgn_rect(&g, r);
    focus_ring(&g, focus);
    hrgn_free(&g);
}

/* Group boxes: an etched frame (primary) or a lighter one (secondary). */
void ap_group(qrect r, uint32_t state, bool primary)
{
    bool active = state != kThemeStateInactive;
    if (!ap_in_color()) {
        ap_frame_grey(r, 0);
        return;
    }
    uint16_t dark = active ? (primary ? 0x8888 : 0xAAAA) : 0xBBBB;
    ap_frame_grey((qrect){ r.top, r.left, (int16_t)(r.bottom - 1), (int16_t)(r.right - 1) }, dark);
    ap_frame_grey((qrect){ (int16_t)(r.top + 1), (int16_t)(r.left + 1), r.bottom, r.right }, 0xFFFF);
    ap_fill_grey((qrect){ (int16_t)(r.top + 1), (int16_t)(r.left + 1), (int16_t)(r.bottom - 2), (int16_t)(r.left + 2) }, 0xFFFF);
    ap_fill_grey((qrect){ (int16_t)(r.top + 1), (int16_t)(r.left + 1), (int16_t)(r.top + 2), (int16_t)(r.right - 2) }, 0xFFFF);
    ap_frame_grey((qrect){ r.top, r.left, (int16_t)(r.bottom - 1), (int16_t)(r.right - 1) }, dark);
}

int32_t tb_DrawThemePrimaryGroup(gaddr r, uint32_t state)
{
    ap_group(rect_arg(r), state, true);
    return noErr;
}

int32_t tb_DrawThemeSecondaryGroup(gaddr r, uint32_t state)
{
    ap_group(rect_arg(r), state, false);
    return noErr;
}

/* A separator: an etched line along the rectangle's longer side. */
void ap_separator(qrect r, uint32_t state)
{
    uint16_t dark = state != kThemeStateInactive ? 0x8888 : 0xBBBB;
    if (!ap_in_color())
        dark = 0;
    if (r.right - r.left >= r.bottom - r.top) {
        int y = r.top + (r.bottom - r.top - 2) / 2;
        ap_fill_grey((qrect){ (int16_t)y, r.left, (int16_t)(y + 1), r.right }, dark);
        if (ap_in_color())
            ap_fill_grey((qrect){ (int16_t)(y + 1), r.left, (int16_t)(y + 2), r.right }, 0xFFFF);
    } else {
        int x = r.left + (r.right - r.left - 2) / 2;
        ap_fill_grey((qrect){ r.top, (int16_t)x, r.bottom, (int16_t)(x + 1) }, dark);
        if (ap_in_color())
            ap_fill_grey((qrect){ r.top, (int16_t)(x + 1), r.bottom, (int16_t)(x + 2) }, 0xFFFF);
    }
}

int32_t tb_DrawThemeSeparator(gaddr r, uint32_t state)
{
    ap_separator(rect_arg(r), state);
    return noErr;
}

int32_t tb_DrawThemeTickMark(gaddr rp, uint32_t state)
{
    qrect r = rect_arg(rp);
    ap_fill_grey(r, state != kThemeStateInactive ? 0 : 0x8888);
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* Tabs */

/* A tab pane: the frame the front tab joins, its inside Platinum grey. */
void ap_tab_pane(qrect r, uint32_t state)
{
    bool active = state != kThemeStateInactive;
    ap_frame_grey(r, active ? 0 : 0x8888);
    qrect in = inset(r, 1);
    ap_fill_grey(in, 0xDDDD);
    if (active)
        ap_bevel(in, 0xFFFF, 0xAAAA);
}

int32_t tb_DrawThemeTabPane(gaddr r, uint32_t state)
{
    ap_tab_pane(rect_arg(r), state);
    return noErr;
}

/* A north-facing tab in r (its bottom on the pane's top edge, which the
 * front tab covers by kThemeTabPaneOverlap). */
void ap_tab(qrect r, int style, const uint8_t *title, int n)
{
    bool front = style == 3 || style == 4, inactive = style == 2 || style == 4, pressed = style == 1;
    qrect t = r;
    if (!front)
        t.bottom = (int16_t)(t.bottom - 3);
    uint16_t frame = inactive ? 0x8888 : 0;
    uint16_t face = front ? 0xDDDD : pressed ? 0x7777 : 0xBBBB;
    hrgn g;
    hrgn_init(&g);
    /* Corners cut by two pixels at the top. */
    for (int y = t.top; y < t.bottom; y++) {
        int cut = y == t.top ? 2 : y == t.top + 1 ? 1 : 0;
        hrgn_add_row(&g, y, t.left + cut, t.right - cut);
    }
    uint16_t fc[3] = { frame, frame, frame }, bc[3] = { face, face, face };
    ap_fill_rgn(&g, fc);
    hrgn_free(&g);
    hrgn_init(&g);
    for (int y = t.top + 1; y < t.bottom - (front ? 0 : 0); y++) {
        int cut = y == t.top + 1 ? 2 : 1;
        hrgn_add_row(&g, y, t.left + cut, t.right - cut);
    }
    ap_fill_rgn(&g, bc);
    hrgn_free(&g);
    if (ap_in_color() && !pressed) {
        ap_fill_grey((qrect){ (int16_t)(t.top + 1), (int16_t)(t.left + 2), (int16_t)(t.top + 2), (int16_t)(t.right - 2) }, 0xFFFF);
        ap_fill_grey((qrect){ (int16_t)(t.top + 2), (int16_t)(t.left + 1), t.bottom, (int16_t)(t.left + 2) }, 0xFFFF);
        ap_fill_grey((qrect){ (int16_t)(t.top + 2), (int16_t)(t.right - 2), t.bottom, (int16_t)(t.right - 1) }, 0xAAAA);
    }
    if (n > 0) {
        uint16_t tc[3];
        grey(tc, inactive ? 0x7777 : pressed ? 0xFFFF : 0);
        ap_text_in((qrect){ (int16_t)(t.top + 2), (int16_t)(t.left + 4), t.bottom, (int16_t)(t.right - 4) }, title, n, true, tc);
    }
}

int32_t tb_DrawThemeTab(gaddr rp, uint16_t style, uint16_t direction, gaddr label_proc, uint32_t data)
{
    qrect r = rect_arg(rp);
    ap_tab(r, style, NULL, 0);
    if (label_proc) {
        /* ThemeTabTitleDrawProcPtr(const Rect *bounds, ThemeTabStyle, ThemeTabDirection, SInt16 depth,
         * Boolean isColorDev, UInt32 userData) */
        qrect lr = { (int16_t)(r.top + 2), (int16_t)(r.left + 4), (int16_t)(r.bottom - 3), (int16_t)(r.right - 4) };
        gaddr b = tb_NewPtr(8, false, false);
        qrect_put(b, lr);
        int depth = qd_port_pixels(qd_port()).depth;
        uint32_t args[6] = { b, style, direction, (uint32_t)depth, depth >= 4, data };
        mm_call(label_proc, 0x0003EAC0 /* pascal (ptr, short, short, short, byte, long) */, args, 6);
        tb_DisposePtr(b);
    }
    return noErr;
}

int32_t tb_GetThemeTabRegion(gaddr rp, uint16_t style, uint16_t direction, gaddr rgn)
{
    qrect r = rect_arg(rp);
    hrgn g;
    hrgn_init(&g);
    for (int y = r.top; y < r.bottom; y++) {
        int cut = y == r.top ? 2 : y == r.top + 1 ? 1 : 0;
        hrgn_add_row(&g, y, r.left + cut, r.right - cut);
    }
    rgn_write(rgn, &g);
    hrgn_free(&g);
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* Buttons */

enum {
    kThemePushButton = 0, kThemeCheckBox = 1, kThemeRadioButton = 2, kThemeBevelButton = 3, kThemeArrowButton = 4,
    kThemePopupButton = 5, kThemeDisclosureButton = 6, kThemeIncDecButton = 7, kThemeSmallBevelButton = 8,
    kThemeLargeBevelButton = 9,
};

/* A push button's face and frame: rounded, grey, bevelled; pressed, dark. */
void ap_push_button(qrect r, uint32_t state, bool is_default)
{
    bool active = state != kThemeStateInactive, pressed = state == kThemeStatePressed;
    int round = 8;
    if (is_default) {
        qrect ring = inset(r, -4);
        uint16_t g[3];
        grey(g, 0xDDDD);
        rrect_ring(ring, 4, round + 6, g);
        grey(g, active ? 0 : 0x8888);
        rrect_ring(ring, 1, round + 6, g);
        if (ap_in_color()) {
            ap_fill_grey((qrect){ (int16_t)(ring.top + 1), (int16_t)(ring.left + 4), (int16_t)(ring.top + 2), (int16_t)(ring.right - 4) }, 0xFFFF);
            ap_fill_grey((qrect){ (int16_t)(ring.bottom - 2), (int16_t)(ring.left + 4), (int16_t)(ring.bottom - 1), (int16_t)(ring.right - 4) }, 0xAAAA);
        }
    }
    uint16_t face[3], frame[3];
    grey(face, pressed ? 0x7777 : 0xDDDD);
    grey(frame, active ? 0 : 0x8888);
    rrect_fill(r, round, face);
    rrect_ring(r, 1, round, frame);
    if (ap_in_color()) {
        uint16_t hi = pressed ? 0x5555 : active ? 0xFFFF : 0xEEEE, lo = pressed ? 0x9999 : active ? 0x8888 : 0xBBBB;
        ap_fill_grey((qrect){ (int16_t)(r.top + 1), (int16_t)(r.left + 3), (int16_t)(r.top + 2), (int16_t)(r.right - 3) }, hi);
        ap_fill_grey((qrect){ (int16_t)(r.top + 3), (int16_t)(r.left + 1), (int16_t)(r.bottom - 3), (int16_t)(r.left + 2) }, hi);
        ap_fill_grey((qrect){ (int16_t)(r.bottom - 2), (int16_t)(r.left + 3), (int16_t)(r.bottom - 1), (int16_t)(r.right - 3) }, lo);
        ap_fill_grey((qrect){ (int16_t)(r.top + 3), (int16_t)(r.right - 2), (int16_t)(r.bottom - 3), (int16_t)(r.right - 1) }, lo);
    }
}

/* A check box or radio button's 12-pixel box: value 0 off, 1 on, 2 mixed. */
void ap_check_box(qrect box, bool radio, int value, uint32_t state)
{
    bool active = state != kThemeStateInactive, pressed = state == kThemeStatePressed;
    uint16_t frame[3], face[3], mark[3];
    grey(frame, active ? 0 : 0x8888);
    grey(face, pressed ? 0x8888 : active ? 0xDDDD : 0xEEEE);
    grey(mark, active ? 0 : 0x8888);
    if (radio) {
        hrgn g, in;
        hrgn_init(&g);
        hrgn_init(&in);
        qd_oval_region(&g, box);
        ap_fill_rgn(&g, frame);
        qd_oval_region(&in, inset(box, 1));
        ap_fill_rgn(&in, face);
        if (ap_in_color() && !pressed) {
            uint16_t hi[3];
            grey(hi, 0xFFFF);
            hrgn spot;
            hrgn_init(&spot);
            qd_oval_region(&spot, (qrect){ (int16_t)(box.top + 2), (int16_t)(box.left + 2), (int16_t)(box.top + 6), (int16_t)(box.left + 6) });
            hrgn_op(&spot, &spot, &in, RGN_SECT);
            ap_fill_rgn(&spot, hi);
            hrgn_free(&spot);
        }
        if (value == 1) {
            hrgn dot;
            hrgn_init(&dot);
            qd_oval_region(&dot, inset(box, 3));
            ap_fill_rgn(&dot, mark);
            hrgn_free(&dot);
        } else if (value == 2) {
            ap_fill((qrect){ (int16_t)(box.top + 5), (int16_t)(box.left + 3), (int16_t)(box.top + 7), (int16_t)(box.right - 3) }, mark);
        }
        hrgn_free(&g);
        hrgn_free(&in);
        return;
    }
    ap_frame(box, frame);
    qrect in = inset(box, 1);
    ap_fill(in, face);
    if (!pressed)
        ap_bevel(in, active ? 0xFFFF : 0xEEEE, active ? 0xAAAA : 0xDDDD);
    if (value == 1) {
        /* A check mark, from the lower left up past the right edge. */
        static const char *const rows[] = {
            "..........##", ".........##.", "........##..", "..##...##...",
            "...##.##....", "....###.....", ".....#......",
        };
        hrgn g;
        hrgn_init(&g);
        for (int y = 0; y < 7; y++)
            for (int x = 0; x < 12; x++)
                if (rows[y][x] == '#') {
                    int x1 = x;
                    while (x1 < 12 && rows[y][x1] == '#')
                        x1++;
                    hrgn_add_row(&g, box.top + 2 + y, box.left + 1 + x, box.left + 1 + x1);
                    x = x1;
                }
        ap_fill_rgn(&g, mark);
        hrgn_free(&g);
    } else if (value == 2) {
        ap_fill((qrect){ (int16_t)(box.top + 5), (int16_t)(box.left + 3), (int16_t)(box.top + 7), (int16_t)(box.right - 3) }, mark);
    }
}

/* A bevel button: square, grey, with a raised (or, pressed or on, sunk) edge. */
void ap_bevel_button(qrect r, int kind, uint32_t state, bool on)
{
    bool active = state != kThemeStateInactive, down = state == kThemeStatePressed || on;
    int w = kind == kThemeSmallBevelButton ? 1 : kind == kThemeLargeBevelButton ? 3 : 2;
    ap_frame_grey(r, active ? 0 : 0x8888);
    qrect in = inset(r, 1);
    ap_fill_grey(in, state == kThemeStatePressed ? 0x8888 : on ? 0xBBBB : 0xDDDD);
    for (int i = 0; i < w && ap_in_color(); i++) {
        qrect b = inset(in, i);
        if (down)
            ap_bevel(b, i == 0 ? 0x6666 : 0x9999, i == 0 ? 0xCCCC : 0xAAAA);
        else
            ap_bevel(b, i == 0 ? 0xFFFF : 0xEEEE, i == 0 ? (active ? 0x8888 : 0xBBBB) : 0xAAAA);
    }
}

/* A disclosure triangle: value 0 points right, 1 down. */
void ap_disclosure(qrect r, int value, uint32_t state, bool left_facing)
{
    uint16_t c[3], fill[3];
    grey(c, state != kThemeStateInactive ? 0 : 0x8888);
    grey(fill, state == kThemeStatePressed ? 0x4444 : 0x9999);
    qrect b = { r.top, r.left, (int16_t)(r.top + 12), (int16_t)(r.left + 12) };
    int dir = value ? 1 : left_facing ? 0 : 2;
    ap_arrow(b, dir, 11, c);
    if (ap_in_color()) {
        qrect in = inset(b, 2);
        ap_arrow(in, dir, 7, fill);
    }
}

/* The little arrows: two stacked halves of a small bevel button. */
void ap_little_arrows(qrect r, uint32_t state)
{
    bool active = state != kThemeStateInactive;
    int mid = (r.top + r.bottom) / 2;
    qrect up = { r.top, r.left, (int16_t)(mid + 1), r.right }, down = { (int16_t)mid, r.left, r.bottom, r.right };
    ap_bevel_button(up, kThemeSmallBevelButton, state == 2 ? kThemeStatePressed : active, false);
    ap_bevel_button(down, kThemeSmallBevelButton, state == 3 ? kThemeStatePressed : active, false);
    uint16_t c[3];
    grey(c, active ? 0 : 0x8888);
    ap_arrow(inset(up, 2), 3, 5, c);
    ap_arrow(inset(down, 2), 1, 5, c);
}

/* A pop-up button's frame and arrow part (the title is the caller's). */
void ap_popup_button(qrect r, uint32_t state, bool arrow_only)
{
    bool active = state != kThemeStateInactive, pressed = state == kThemeStatePressed;
    uint16_t face[3], frame[3];
    grey(face, pressed ? 0x7777 : 0xDDDD);
    grey(frame, active ? 0 : 0x8888);
    rrect_fill(r, 6, face);
    rrect_ring(r, 1, 6, frame);
    if (ap_in_color() && !pressed) {
        ap_fill_grey((qrect){ (int16_t)(r.top + 1), (int16_t)(r.left + 2), (int16_t)(r.top + 2), (int16_t)(r.right - 2) }, 0xFFFF);
        ap_fill_grey((qrect){ (int16_t)(r.top + 2), (int16_t)(r.left + 1), (int16_t)(r.bottom - 2), (int16_t)(r.left + 2) }, 0xFFFF);
        ap_fill_grey((qrect){ (int16_t)(r.bottom - 2), (int16_t)(r.left + 2), (int16_t)(r.bottom - 1), (int16_t)(r.right - 2) }, 0x8888);
    }
    qrect box = { (int16_t)(r.top + 1), (int16_t)(r.right - 19), (int16_t)(r.bottom - 1), (int16_t)(r.right - 1) };
    if (!arrow_only) {
        if (ap_in_color())
            ap_fill_grey((qrect){ (int16_t)(r.top + 2), (int16_t)(box.left - 1), (int16_t)(r.bottom - 2), box.left },
                         pressed ? 0x5555 : 0xAAAA);
    } else {
        box.left = (int16_t)(r.left + 1);
    }
    uint16_t c[3];
    grey(c, !active ? 0x8888 : pressed ? 0xFFFF : 0);
    qrect up = box, down = box;
    up.bottom = (int16_t)((box.top + box.bottom) / 2);
    down.top = up.bottom;
    ap_arrow((qrect){ (int16_t)(up.top + 2), up.left, (int16_t)(up.bottom + 1), up.right }, 3, 7, c);
    ap_arrow((qrect){ (int16_t)(down.top - 1), down.left, (int16_t)(down.bottom - 2), down.right }, 1, 7, c);
}

/* ThemeButtonDrawInfo: state (long), value, adornment. */
static void button_info(gaddr p, uint32_t *state, int *value, int *adorn)
{
    *state = p ? rd32(p) : kThemeStateActive;
    *value = p ? (int16_t)rd16(p + 4) : 0;
    *adorn = p ? rd16(p + 6) : 0;
}

/* The part of the bounds a button's face takes (check boxes and radio
 * buttons: their box, at the left), and where its label goes. */
static qrect button_face(qrect r, int kind)
{
    if (kind == kThemeCheckBox || kind == kThemeRadioButton) {
        int top = r.top + (r.bottom - r.top - 12) / 2;
        return (qrect){ (int16_t)top, r.left, (int16_t)(top + 12), (int16_t)(r.left + 12) };
    }
    if (kind == kThemeDisclosureButton)
        return (qrect){ r.top, r.left, (int16_t)(r.top + 12), (int16_t)(r.left + 12) };
    return r;
}

static qrect button_content(qrect r, int kind)
{
    switch (kind) {
    case kThemeCheckBox: case kThemeRadioButton: case kThemeDisclosureButton:
        return (qrect){ r.top, (int16_t)(r.left + 16), r.bottom, r.right };
    case kThemePopupButton:
        return (qrect){ (int16_t)(r.top + 2), (int16_t)(r.left + 8), (int16_t)(r.bottom - 2), (int16_t)(r.right - 22) };
    case kThemeArrowButton: case kThemeIncDecButton:
        return (qrect){ r.top, r.left, r.top, r.left };
    case kThemePushButton:
        return inset(r, 3);
    default:
        return inset(r, kind == kThemeLargeBevelButton ? 4 : kind == kThemeSmallBevelButton ? 2 : 3);
    }
}

void ap_button(qrect r, int kind, uint32_t state, int value, int adorn)
{
    switch (kind) {
    case kThemePushButton: ap_push_button(r, state, adorn & 1); break;
    case kThemeCheckBox: ap_check_box(button_face(r, kind), false, value, state); break;
    case kThemeRadioButton: ap_check_box(button_face(r, kind), true, value, state); break;
    case kThemeDisclosureButton: ap_disclosure(r, value == 1, state, value == 2); break;
    case kThemeIncDecButton: ap_little_arrows(r, state); break;
    case kThemePopupButton: ap_popup_button(r, state, false); break;
    case kThemeArrowButton: ap_popup_button(r, state, true); break;
    default: ap_bevel_button(r, kind, state, value == 1); break;
    }
    if (adorn & 4) /* kThemeAdornmentFocus */
        ap_focus_rect(button_face(r, kind), true);
}

int32_t tb_DrawThemeButton(gaddr rp, uint16_t kind, gaddr info, gaddr prev, gaddr erase_proc, gaddr label_proc,
                           uint32_t data)
{
    qrect r = rect_arg(rp);
    uint32_t state;
    int value, adorn;
    button_info(info, &state, &value, &adorn);
    ap_button(r, kind, state, value, adorn);
    if (label_proc && !(adorn & 0x20)) {
        /* ThemeButtonDrawProcPtr(const Rect *bounds, ThemeButtonKind, const ThemeButtonDrawInfo *,
         * UInt32 userData, SInt16 depth, Boolean isColorDev) */
        gaddr b = tb_NewPtr(8, false, false);
        qrect_put(b, button_content(r, kind));
        int depth = qd_port_pixels(qd_port()).depth;
        uint32_t args[6] = { b, kind, info, data, (uint32_t)depth, depth >= 4 };
        mm_call(label_proc, 0x00016EC0 /* pascal (ptr, short, ptr, long, short, byte) */, args, 6);
        tb_DisposePtr(b);
    }
    return noErr;
}

int32_t tb_GetThemeButtonRegion(gaddr rp, uint16_t kind, gaddr info, gaddr rgn)
{
    qrect r = rect_arg(rp);
    uint32_t state;
    int value, adorn;
    button_info(info, &state, &value, &adorn);
    hrgn g;
    hrgn_init(&g);
    if (kind == kThemePushButton)
        qd_rrect_region(&g, (adorn & 1) ? inset(r, -4) : r, 8, 8);
    else
        hrgn_rect(&g, kind == kThemeCheckBox || kind == kThemeRadioButton ? button_face(r, kind) : r);
    rgn_write(rgn, &g);
    hrgn_free(&g);
    return noErr;
}

int32_t tb_GetThemeButtonContentBounds(gaddr rp, uint16_t kind, gaddr info, gaddr out)
{
    qrect_put(out, button_content(rect_arg(rp), kind));
    return noErr;
}

int32_t tb_GetThemeButtonBackgroundBounds(gaddr rp, uint16_t kind, gaddr info, gaddr out)
{
    uint32_t state;
    int value, adorn;
    button_info(info, &state, &value, &adorn);
    qrect r = rect_arg(rp);
    qrect_put(out, kind == kThemePushButton && (adorn & 1) ? inset(r, -4) : r);
    return noErr;
}

int32_t tb_DrawThemePopupArrow(gaddr rp, uint16_t orient, uint16_t size, uint32_t state, gaddr erase_proc,
                               uint32_t data)
{
    static const int sizes[4] = { 5, 9, 13, 17 }; /* 3, 5, 7 and 9 points tall */
    uint16_t c[3];
    ap_brush_rgb(state == kThemeStateInactive ? 22 : state == kThemeStatePressed ? 21 : 20, c);
    ap_arrow(rect_arg(rp), orient & 3, sizes[size & 3], c);
    return noErr;
}

/* Chasing arrows: a ring with a gap that turns a step at each index. */
int32_t tb_DrawThemeChasingArrows(gaddr rp, uint32_t index, uint32_t state, gaddr erase_proc, uint32_t data)
{
    qrect r = rect_arg(rp);
    hrgn ring, hole, gap;
    hrgn_init(&ring);
    hrgn_init(&hole);
    hrgn_init(&gap);
    hrgn_rect(&ring, r);
    erase_rgn(&ring);
    qd_oval_region(&ring, r);
    qd_oval_region(&hole, inset(r, 3));
    hrgn_op(&ring, &ring, &hole, RGN_DIFF);
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, w = (r.right - r.left) / 2;
    static const int8_t dx[8] = { 0, 1, 1, 1, 0, -1, -1, -1 }, dy[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };
    for (int k = 0; k < 2; k++) { /* two gaps, opposite */
        int i = (int)((index + (uint32_t)k * 4) % 8);
        int gx = cx + dx[i] * w * 3 / 4, gy = cy + dy[i] * w * 3 / 4;
        hrgn g;
        hrgn_init(&g);
        hrgn_rect(&g, (qrect){ (int16_t)(gy - 2), (int16_t)(gx - 2), (int16_t)(gy + 2), (int16_t)(gx + 2) });
        hrgn_op(&gap, &gap, &g, RGN_UNION);
        hrgn_free(&g);
    }
    hrgn_op(&ring, &ring, &gap, RGN_DIFF);
    uint16_t c[3];
    ap_brush_rgb(13, c);
    if (state == kThemeStateInactive)
        grey(c, 0x8888);
    ap_fill_rgn(&ring, c);
    hrgn_free(&ring);
    hrgn_free(&hole);
    hrgn_free(&gap);
    return noErr;
}

/* Grow boxes standing on their own (outside a window's frame). */
static qrect grow_box(uint32_t origin, int dir, bool small)
{
    int s = small ? 11 : 15, h = pt_h(origin), v = pt_v(origin);
    int left = (dir & 1) ? h - s : h, top = (dir & 4) ? v - s : v;
    return (qrect){ (int16_t)top, (int16_t)left, (int16_t)(top + s), (int16_t)(left + s) };
}

int32_t tb_GetThemeStandaloneGrowBoxBounds(uint32_t origin, uint16_t dir, int8_t small, gaddr out)
{
    qrect_put(out, grow_box(origin, dir, small));
    return noErr;
}

int32_t tb_DrawThemeStandaloneNoGrowBox(uint32_t origin, uint16_t dir, int8_t small, uint32_t state)
{
    qrect r = grow_box(origin, dir, small);
    ap_fill_grey(r, 0xDDDD);
    ap_bevel(r, 0xFFFF, 0xAAAA);
    return noErr;
}

int32_t tb_DrawThemeStandaloneGrowBox(uint32_t origin, uint16_t dir, int8_t small, uint32_t state)
{
    tb_DrawThemeStandaloneNoGrowBox(origin, dir, small, state);
    qrect r = grow_box(origin, dir, small);
    if (state == kThemeStateInactive)
        return noErr;
    /* Three ridges across the corner. */
    for (int i = 0; i < 3; i++) {
        int o = 3 + i * 3;
        for (int k = 0; k < 2; k++) {
            hrgn g;
            hrgn_init(&g);
            for (int y = 0; y < r.bottom - r.top; y++) {
                int x = r.right - r.left - 1 - (y - o) - k;
                if (y >= o && x >= o && x < r.right - r.left - 1)
                    hrgn_add_row(&g, r.top + y, r.left + x, r.left + x + 1);
            }
            uint16_t c[3];
            grey(c, k ? 0xFFFF : 0x8888);
            ap_fill_rgn(&g, c);
            hrgn_free(&g);
        }
    }
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* Tracks: scroll bars, sliders and progress bars */

/* ThemeTrackDrawInfo: kind, bounds, min, max, value, reserved, attributes,
 * enableState, filler, then the kind's: viewsize and pressState (scroll
 * bars), thumbDir and pressState (sliders), phase (progress). */
typedef struct {
    int kind;
    qrect bounds;
    int32_t min, max, value, viewsize;
    int attributes, enable, press, thumb_dir, phase;
} track_info;

enum { kThemeScrollBar = 0, kThemeSmallScrollBar = 1, kThemeSlider = 2, kThemeProgressBar = 3, kThemeIndeterminateBar = 4 };
enum { kThemeTrackHorizontal = 1, kThemeTrackRightToLeft = 2, kThemeTrackShowThumb = 4 };
enum { kThemeTrackActive = 0, kThemeTrackDisabled = 1, kThemeTrackNothingToScroll = 2 };

static track_info read_track(gaddr p)
{
    track_info t;
    memset(&t, 0, sizeof t);
    t.kind = rd16(p);
    t.bounds = qrect_get(p + 2);
    t.min = (int32_t)rd32(p + 10);
    t.max = (int32_t)rd32(p + 14);
    t.value = (int32_t)rd32(p + 18);
    t.attributes = rd16(p + 26);
    t.enable = rd8(p + 28);
    if (t.kind <= kThemeSmallScrollBar) {
        t.viewsize = (int32_t)rd32(p + 30);
        t.press = rd8(p + 34);
    } else if (t.kind == kThemeSlider) {
        t.thumb_dir = rd8(p + 30);
        t.press = rd8(p + 31);
    } else {
        t.phase = rd8(p + 30);
    }
    return t;
}

static bool horizontal(const track_info *t) { return t->attributes & kThemeTrackHorizontal; }

/* Scroll bars: arrows at both ends (16 pixels, or 11 for small ones). */
static int arrow_len(const track_info *t) { return t->kind == kThemeSmallScrollBar ? 11 : 16; }

static qrect track_area(const track_info *t)
{
    qrect r = t->bounds;
    if (t->kind <= kThemeSmallScrollBar) {
        int a = arrow_len(t);
        if (horizontal(t)) {
            r.left = (int16_t)(r.left + a - 1);
            r.right = (int16_t)(r.right - a + 1);
        } else {
            r.top = (int16_t)(r.top + a - 1);
            r.bottom = (int16_t)(r.bottom - a + 1);
        }
    } else if (t->kind == kThemeSlider) {
        /* The groove the thumb rides along: thumb-wide margins at the ends. */
        if (horizontal(t)) {
            r.left = (int16_t)(r.left + 7);
            r.right = (int16_t)(r.right - 7);
        } else {
            r.top = (int16_t)(r.top + 7);
            r.bottom = (int16_t)(r.bottom - 7);
        }
    }
    return r;
}

/* The thumb's length along the track. */
static int thumb_len(const track_info *t, int track)
{
    if (t->kind == kThemeSlider)
        return 15;
    int len = t->kind == kThemeSmallScrollBar ? 11 : 16;
    if (t->viewsize > 0 && t->max > t->min) {
        int64_t prop = (int64_t)track * t->viewsize / ((int64_t)t->max - t->min + t->viewsize);
        if (prop > len)
            len = (int)prop;
    }
    return len < track ? len : track;
}

static qrect track_thumb(const track_info *t)
{
    qrect a = track_area(t);
    bool h = horizontal(t);
    int track = h ? a.right - a.left : a.bottom - a.top;
    if (t->kind == kThemeSlider) {
        int x = t->max > t->min ? (int)((int64_t)(t->value - t->min) * track / (t->max - t->min)) : 0;
        if (t->attributes & kThemeTrackRightToLeft)
            x = track - x;
        qrect b = t->bounds;
        if (h)
            return (qrect){ b.top, (int16_t)(a.left + x - 7), (int16_t)(b.top + 16), (int16_t)(a.left + x + 8) };
        return (qrect){ (int16_t)(a.top + x - 7), b.left, (int16_t)(a.top + x + 8), (int16_t)(b.left + 16) };
    }
    int len = thumb_len(t, track);
    int room = track - len;
    int pos = t->max > t->min ? (int)((int64_t)(t->value - t->min) * room / (t->max - t->min)) : 0;
    if (pos < 0)
        pos = 0;
    if (pos > room)
        pos = room;
    if (h)
        return (qrect){ t->bounds.top, (int16_t)(a.left + pos), t->bounds.bottom, (int16_t)(a.left + pos + len) };
    return (qrect){ (int16_t)(a.top + pos), t->bounds.left, (int16_t)(a.top + pos + len), t->bounds.right };
}

static bool track_has_thumb(const track_info *t)
{
    return t->enable == kThemeTrackActive && t->max > t->min &&
           (t->kind != kThemeSlider || true) && t->kind != kThemeProgressBar && t->kind != kThemeIndeterminateBar;
}

/* A scroll bar's arrow box. */
static void scroll_arrow(qrect r, int dir, bool active, bool pressed)
{
    ap_frame_grey(r, 0);
    qrect in = inset(r, 1);
    ap_fill_grey(in, pressed ? 0x7777 : 0xDDDD);
    if (active && !pressed)
        ap_bevel(in, 0xFFFF, 0xAAAA);
    else if (pressed)
        ap_bevel(in, 0x5555, 0x9999);
    uint16_t c[3];
    grey(c, !active ? 0x8888 : pressed ? 0xFFFF : 0);
    ap_arrow(in, dir, r.right - r.left > 12 && r.bottom - r.top > 12 ? 7 : 5, c);
}

/* A Platinum scroll bar thumb: the accent colour with grip lines. */
static void scroll_thumb(qrect r, bool horiz, bool pressed)
{
    ap_frame_grey(r, 0);
    qrect in = inset(r, 1);
    uint16_t face[3], hi[3], lo[3];
    accent_shade(pressed ? -2 : 4, face);
    accent_shade(10, hi);
    accent_shade(-6, lo);
    ap_fill(in, face);
    if (!ap_in_color())
        return;
    ap_fill((qrect){ in.top, in.left, (int16_t)(in.top + 1), in.right }, hi);
    ap_fill((qrect){ in.top, in.left, in.bottom, (int16_t)(in.left + 1) }, hi);
    ap_fill((qrect){ (int16_t)(in.bottom - 1), in.left, in.bottom, in.right }, lo);
    ap_fill((qrect){ in.top, (int16_t)(in.right - 1), in.bottom, in.right }, lo);
    int len = horiz ? in.right - in.left : in.bottom - in.top;
    if (len < 12)
        return;
    int c = horiz ? (in.left + in.right) / 2 : (in.top + in.bottom) / 2;
    for (int k = -3; k <= 3; k += 2) {
        if (horiz) {
            ap_fill((qrect){ (int16_t)(in.top + 3), (int16_t)(c + k), (int16_t)(in.bottom - 3), (int16_t)(c + k + 1) }, hi);
            ap_fill((qrect){ (int16_t)(in.top + 3), (int16_t)(c + k + 1), (int16_t)(in.bottom - 3), (int16_t)(c + k + 2) }, lo);
        } else {
            ap_fill((qrect){ (int16_t)(c + k), (int16_t)(in.left + 3), (int16_t)(c + k + 1), (int16_t)(in.right - 3) }, hi);
            ap_fill((qrect){ (int16_t)(c + k + 1), (int16_t)(in.left + 3), (int16_t)(c + k + 2), (int16_t)(in.right - 3) }, lo);
        }
    }
}

/* A scroll bar as the Control Manager lays it out (CDEF 1's geometry):
 * arrows of 16 pixels at the ends, the thumb where the caller puts it.
 * hilite is the part pressed (20, 21; 22 and 23 the track; 129 the thumb). */
void ap_scroll_bar(qrect r, bool vertical, int hilite, bool active, bool live, qrect thumb)
{
    qrect lo = vertical ? (qrect){ r.top, r.left, (int16_t)(r.top + 16), r.right } : (qrect){ r.top, r.left, r.bottom, (int16_t)(r.left + 16) };
    qrect hi = vertical ? (qrect){ (int16_t)(r.bottom - 16), r.left, r.bottom, r.right }
                        : (qrect){ r.top, (int16_t)(r.right - 16), r.bottom, r.right };
    qrect area = vertical ? (qrect){ (int16_t)(r.top + 15), r.left, (int16_t)(r.bottom - 15), r.right }
                          : (qrect){ r.top, (int16_t)(r.left + 15), r.bottom, (int16_t)(r.right - 15) };
    ap_frame_grey(area, 0);
    qrect in = inset(area, 1);
    bool on = active && live;
    ap_fill_grey(in, on ? 0xBBBB : 0xEEEE);
    if (on && ap_in_color())
        ap_fill_grey(vertical ? (qrect){ in.top, in.left, in.bottom, (int16_t)(in.left + 1) }
                              : (qrect){ in.top, in.left, (int16_t)(in.top + 1), in.right }, 0x8888);
    scroll_arrow(lo, vertical ? 3 : 0, on, hilite == 20);
    scroll_arrow(hi, vertical ? 1 : 2, on, hilite == 21);
    if (on && !qrect_empty(thumb))
        scroll_thumb(thumb, !vertical, hilite == 129);
}

/* Scroll bars: the press state's bits are kTheme...Pressed. */
static void draw_scroll_bar(const track_info *t)
{
    qrect r = t->bounds;
    bool h = horizontal(t), active = t->enable != kThemeTrackDisabled;
    int a = arrow_len(t);
    qrect lo = h ? (qrect){ r.top, r.left, r.bottom, (int16_t)(r.left + a) } : (qrect){ r.top, r.left, (int16_t)(r.top + a), r.right };
    qrect hi = h ? (qrect){ r.top, (int16_t)(r.right - a), r.bottom, r.right }
                 : (qrect){ (int16_t)(r.bottom - a), r.left, r.bottom, r.right };
    qrect area = track_area(t);
    ap_frame_grey(area, 0);
    qrect in = inset(area, 1);
    bool live = t->enable == kThemeTrackActive && t->max > t->min;
    ap_fill_grey(in, live ? 0xBBBB : 0xEEEE);
    if (live && ap_in_color()) { /* a sunken groove */
        ap_fill_grey(h ? (qrect){ in.top, in.left, (int16_t)(in.top + 1), in.right }
                       : (qrect){ in.top, in.left, in.bottom, (int16_t)(in.left + 1) }, 0x8888);
    }
    scroll_arrow(lo, h ? 0 : 3, active && live, t->press & 0x01);
    scroll_arrow(hi, h ? 2 : 1, active && live, t->press & 0x40);
    if (live)
        scroll_thumb(track_thumb(t), h, t->press & 0x08);
}

/* A slider: a groove with a pointed (or plain) thumb. */
static void draw_slider(const track_info *t)
{
    bool h = horizontal(t), active = t->enable == kThemeTrackActive;
    qrect a = track_area(t), b = t->bounds;
    qrect groove = h ? (qrect){ (int16_t)(b.top + 5), (int16_t)(a.left - 2), (int16_t)(b.top + 10), (int16_t)(a.right + 2) }
                     : (qrect){ (int16_t)(a.top - 2), (int16_t)(b.left + 5), (int16_t)(a.bottom + 2), (int16_t)(b.left + 10) };
    ap_frame_grey(groove, active ? 0 : 0x8888);
    ap_fill_grey(inset(groove, 1), active ? 0xAAAA : 0xDDDD);
    if (ap_in_color())
        ap_bevel(inset(groove, 1), 0x8888, 0xDDDD);
    qrect th = track_thumb(t);
    uint16_t face[3];
    if (active)
        accent_shade(t->press & 0x08 ? -2 : 4, face);
    else
        grey(face, 0xDDDD);
    hrgn g, in;
    hrgn_init(&g);
    hrgn_init(&in);
    /* Square with a point down (or right) unless plain. */
    int w = h ? th.right - th.left : th.bottom - th.top;
    bool pointed = t->thumb_dir != 0;
    for (int y = th.top; y < th.bottom; y++) {
        int x0 = th.left, x1 = th.right;
        if (h && pointed) {
            int d = y - (th.bottom - w / 2);
            if (d > 0) {
                x0 += d;
                x1 -= d;
            }
        }
        if (x1 > x0)
            hrgn_add_row(&g, y, x0, x1);
    }
    if (!h && pointed) { /* vertical: pointing right */
        hrgn_free(&g);
        hrgn_init(&g);
        int cy = (th.top + th.bottom) / 2, side = th.right - th.left - 4;
        for (int y = th.top; y < th.bottom; y++) {
            int d = abs(y - cy);
            hrgn_add_row(&g, y, th.left, th.left + side + (4 - d > 0 ? 4 - d : 0));
        }
    }
    uint16_t frame[3];
    grey(frame, active ? 0 : 0x8888);
    ap_fill_rgn(&g, frame);
    hrgn_copy(&in, &g);
    hrgn_inset(&in, 1, 1);
    ap_fill_rgn(&in, face);
    hrgn_free(&g);
    hrgn_free(&in);
}

/* A progress bar: a framed, sunken track filled with the accent colour;
 * indeterminate ones are diagonal stripes that move with the phase. */
static void draw_progress(const track_info *t)
{
    qrect r = t->bounds;
    bool active = t->enable == kThemeTrackActive;
    ap_frame_grey(r, active ? 0 : 0x8888);
    qrect in = inset(r, 1);
    ap_fill_grey(in, 0xEEEE);
    if (ap_in_color())
        ap_bevel(in, 0xAAAA, 0xFFFF);
    uint16_t fill[3], hi[3];
    if (active) {
        accent_shade(0, fill);
        accent_shade(8, hi);
    } else {
        grey(fill, 0xAAAA);
        grey(hi, 0xCCCC);
    }
    if (t->kind == kThemeIndeterminateBar) {
        hrgn g;
        hrgn_init(&g);
        int period = 16;
        for (int y = in.top; y < in.bottom; y++) {
            int16_t runs[256];
            int n = 0;
            int shift = (y - in.top) + t->phase * 2;
            for (int x = in.left - period + (shift % period); x < in.right && n < 254; x += period) {
                int x0 = x < in.left ? in.left : x, x1 = x + period / 2 > in.right ? in.right : x + period / 2;
                if (x1 > x0) {
                    runs[n++] = (int16_t)x0;
                    runs[n++] = (int16_t)x1;
                }
            }
            if (n)
                hrgn_add_runs(&g, y, runs, n);
        }
        ap_fill_rgn(&g, fill);
        hrgn_free(&g);
        return;
    }
    if (t->max <= t->min)
        return;
    int w = in.right - in.left;
    int x = (int)((int64_t)(t->value - t->min) * w / (t->max - t->min));
    if (x < 0)
        x = 0;
    if (x > w)
        x = w;
    qrect bar = (t->attributes & kThemeTrackRightToLeft) ? (qrect){ in.top, (int16_t)(in.right - x), in.bottom, in.right }
                                                         : (qrect){ in.top, in.left, in.bottom, (int16_t)(in.left + x) };
    ap_fill(bar, fill);
    if (ap_in_color() && x > 2)
        ap_fill((qrect){ (int16_t)(bar.top + 1), bar.left, (int16_t)(bar.top + 3), bar.right }, hi);
}

void ap_track(gaddr info)
{
    track_info t = read_track(info);
    if (t.kind <= kThemeSmallScrollBar)
        draw_scroll_bar(&t);
    else if (t.kind == kThemeSlider)
        draw_slider(&t);
    else
        draw_progress(&t);
}

int32_t tb_DrawThemeTrack(gaddr info, gaddr ghost, gaddr erase_proc, uint32_t data)
{
    ap_track(info);
    if (ghost) {
        hrgn g;
        hrgn_init(&g);
        rgn_read(ghost, &g);
        uint16_t c[3];
        accent_shade(10, c);
        ap_fill_rgn(&g, c);
        hrgn_free(&g);
    }
    return noErr;
}

int32_t tb_GetThemeTrackBounds(gaddr info, gaddr out)
{
    track_info t = read_track(info);
    qrect_put(out, track_area(&t));
    return noErr;
}

int32_t tb_GetThemeTrackThumbRgn(gaddr info, gaddr rgn)
{
    track_info t = read_track(info);
    hrgn g;
    hrgn_init(&g);
    if (track_has_thumb(&t))
        hrgn_rect(&g, track_thumb(&t));
    rgn_write(rgn, &g);
    hrgn_free(&g);
    return noErr;
}

/* Where the thumb may be dragged: the track, a little beyond it across. */
int32_t tb_GetThemeTrackDragRect(gaddr info, gaddr out)
{
    track_info t = read_track(info);
    qrect a = track_area(&t);
    qrect_put(out, inset(a, -32));
    return noErr;
}

int32_t tb_DrawThemeTrackTickMarks(gaddr info, uint32_t count, gaddr erase_proc, uint32_t data)
{
    track_info t = read_track(info);
    if (count < 2)
        return noErr;
    qrect a = track_area(&t);
    bool h = horizontal(&t);
    uint16_t c[3];
    grey(c, t.enable == kThemeTrackActive ? 0 : 0x8888);
    for (uint32_t i = 0; i < count; i++) {
        if (h) {
            int x = a.left + (int)((int64_t)i * (a.right - a.left) / (count - 1));
            ap_fill((qrect){ (int16_t)(t.bounds.top + 18), (int16_t)x, (int16_t)(t.bounds.top + 22), (int16_t)(x + 1) }, c);
        } else {
            int y = a.top + (int)((int64_t)i * (a.bottom - a.top) / (count - 1));
            ap_fill((qrect){ (int16_t)y, (int16_t)(t.bounds.left + 18), (int16_t)(y + 1), (int16_t)(t.bounds.left + 22) }, c);
        }
    }
    return noErr;
}

/* Hit testing: the scroll bar's parts by the Control Manager's part codes. */
int8_t tb_HitTestThemeTrack(gaddr info, uint32_t pt, gaddr part)
{
    track_info t = read_track(info);
    int h = pt_h(pt), v = pt_v(pt);
    qrect b = t.bounds;
    if (h < b.left || h >= b.right || v < b.top || v >= b.bottom)
        return false;
    int16_t p = 0;
    if (track_has_thumb(&t)) {
        qrect th = track_thumb(&t), a = track_area(&t);
        bool hz = horizontal(&t);
        int c = hz ? h : v;
        if (h >= th.left && h < th.right && v >= th.top && v < th.bottom)
            p = 129; /* kControlIndicatorPart */
        else if (t.kind <= kThemeSmallScrollBar && c < (hz ? a.left : a.top))
            p = 20;
        else if (t.kind <= kThemeSmallScrollBar && c >= (hz ? a.right : a.bottom))
            p = 21;
        else if (c < (hz ? th.left : th.top))
            p = 22;
        else
            p = 23;
    }
    if (part)
        wr16(part, (uint16_t)p);
    return p != 0;
}

/* The value the thumb at `position` pixels along the track stands for. */
static int32_t value_at(const track_info *t, int32_t position)
{
    qrect a = track_area(t);
    int track = horizontal(t) ? a.right - a.left : a.bottom - a.top;
    int room = t->kind == kThemeSlider ? track : track - thumb_len(t, track);
    if (room <= 0)
        return t->min;
    int32_t v = t->min + (int32_t)((int64_t)position * (t->max - t->min) / room);
    return v < t->min ? t->min : v > t->max ? t->max : v;
}

int32_t tb_GetThemeTrackThumbPositionFromOffset(gaddr info, uint32_t offset, gaddr out)
{
    track_info t = read_track(info);
    qrect th = track_thumb(&t), a = track_area(&t);
    int32_t pos = horizontal(&t) ? th.left - a.left + pt_h(offset) : th.top - a.top + pt_v(offset);
    if (t.kind == kThemeSlider)
        pos += 7;
    wr32(out, (uint32_t)pos);
    return noErr;
}

int32_t tb_GetThemeTrackThumbPositionFromRegion(gaddr info, gaddr rgn, gaddr out)
{
    track_info t = read_track(info);
    hrgn g;
    hrgn_init(&g);
    rgn_read(rgn, &g);
    qrect bb = hrgn_bbox(&g), a = track_area(&t);
    hrgn_free(&g);
    int32_t pos = horizontal(&t) ? bb.left - a.left : bb.top - a.top;
    if (t.kind == kThemeSlider)
        pos += 7;
    wr32(out, (uint32_t)pos);
    return noErr;
}

int32_t tb_GetThemeTrackLiveValue(gaddr info, int32_t position, gaddr out)
{
    track_info t = read_track(info);
    wr32(out, (uint32_t)value_at(&t, position));
    return noErr;
}

/* Scroll bar arrows alone (around a track of the caller's). */
static track_info arrows_info(gaddr bounds, uint8_t enable, uint8_t press, bool horiz)
{
    track_info t;
    memset(&t, 0, sizeof t);
    t.kind = kThemeScrollBar;
    t.bounds = qrect_get(bounds);
    t.enable = enable;
    t.press = press;
    t.attributes = horiz ? kThemeTrackHorizontal : 0;
    return t;
}

int32_t tb_GetThemeScrollBarTrackRect(gaddr bounds, uint8_t enable, uint8_t press, int8_t horiz, gaddr out)
{
    track_info t = arrows_info(bounds, enable, press, horiz);
    qrect_put(out, track_area(&t));
    return noErr;
}

int32_t tb_DrawThemeScrollBarArrows(gaddr bounds, uint8_t enable, uint8_t press, int8_t horiz, gaddr out)
{
    track_info t = arrows_info(bounds, enable, press, horiz);
    qrect r = t.bounds;
    bool active = enable == kThemeTrackActive;
    if (horiz) {
        scroll_arrow((qrect){ r.top, r.left, r.bottom, (int16_t)(r.left + 16) }, 0, active, press & 0x01);
        scroll_arrow((qrect){ r.top, (int16_t)(r.right - 16), r.bottom, r.right }, 2, active, press & 0x40);
    } else {
        scroll_arrow((qrect){ r.top, r.left, (int16_t)(r.top + 16), r.right }, 3, active, press & 0x01);
        scroll_arrow((qrect){ (int16_t)(r.bottom - 16), r.left, r.bottom, r.right }, 1, active, press & 0x40);
    }
    if (out)
        qrect_put(out, track_area(&t));
    return noErr;
}

int8_t tb_HitTestThemeScrollBarArrows(gaddr bounds, uint8_t enable, uint8_t press, int8_t horiz, uint32_t pt,
                                      gaddr out, gaddr part)
{
    track_info t = arrows_info(bounds, enable, press, horiz);
    qrect a = track_area(&t);
    if (out)
        qrect_put(out, a);
    int c = horiz ? pt_h(pt) : pt_v(pt);
    int16_t p = 0;
    qrect r = t.bounds;
    if (pt_h(pt) >= r.left && pt_h(pt) < r.right && pt_v(pt) >= r.top && pt_v(pt) < r.bottom) {
        if (c < (horiz ? a.left : a.top))
            p = 20;
        else if (c >= (horiz ? a.right : a.bottom))
            p = 21;
    }
    if (part)
        wr16(part, (uint16_t)p);
    return p != 0;
}

/* ---------------------------------------------------------------------- */
/* Windows (for definition procedures of programs' own) */

enum { kThemeDocumentWindow = 0, kThemeUtilityWindow = 8, kThemeUtilitySideWindow = 9 };

static int title_height(int flavor) { return flavor == kThemeUtilityWindow ? 13 : flavor == kThemeUtilitySideWindow ? 0 : 19; }

static bool has_title(int flavor) { return flavor == kThemeDocumentWindow || flavor == 2 || flavor == 4 || flavor >= 7; }

/* The frame's rectangle (structure) around the content. */
static qrect window_struct(int flavor, qrect c)
{
    int th = has_title(flavor) ? title_height(flavor) : 0;
    if (flavor == kThemeUtilitySideWindow)
        return (qrect){ (int16_t)(c.top - 1), (int16_t)(c.left - 13), (int16_t)(c.bottom + 1), (int16_t)(c.right + 1) };
    if (!has_title(flavor))
        return inset(c, flavor == 5 ? -1 : -8);
    return (qrect){ (int16_t)(c.top - th - 1), (int16_t)(c.left - 1), (int16_t)(c.bottom + 2), (int16_t)(c.right + 2) };
}

static qrect widget_rect(int flavor, qrect c, int widget)
{
    qrect s = window_struct(flavor, c);
    int th = title_height(flavor), sz = th - 8;
    int top = s.top + (th - sz) / 2 + 1;
    if (widget == 0)
        return (qrect){ (int16_t)top, (int16_t)(s.left + 8), (int16_t)(top + sz), (int16_t)(s.left + 8 + sz) };
    int right = s.right - 8 - (widget == 1 && flavor == kThemeDocumentWindow ? sz + 4 : 0);
    if (widget == 2)
        right = s.right - 8;
    else if (widget == 1)
        right = s.right - 8 - sz - 4;
    return (qrect){ (int16_t)top, (int16_t)(right - sz), (int16_t)(top + sz), (int16_t)right };
}

int32_t tb_GetThemeWindowRegion(uint16_t flavor, gaddr content, uint32_t state, gaddr metrics, uint32_t attrs,
                                uint16_t code, gaddr rgn)
{
    qrect c = qrect_get(content), s = window_struct(flavor, c);
    hrgn g;
    hrgn_init(&g);
    switch (code) {
    case 0: /* kWindowTitleBarRgn */
        hrgn_rect(&g, (qrect){ s.top, s.left, (int16_t)(c.top - 1), s.right });
        break;
    case 1: /* kWindowTitleTextRgn */
        hrgn_rect(&g, (qrect){ (int16_t)(s.top + 2), (int16_t)(s.left + 30), (int16_t)(c.top - 3), (int16_t)(s.right - 50) });
        break;
    case 2: case 3: case 4: /* close, zoom, grow boxes */
        if (code == 4)
            hrgn_rect(&g, (qrect){ (int16_t)(c.bottom - 15), (int16_t)(c.right - 15), c.bottom, c.right });
        else
            hrgn_rect(&g, widget_rect(flavor, c, code == 2 ? 0 : 1));
        break;
    case 5: /* kWindowDragRgn */
        hrgn_rect(&g, (qrect){ s.top, s.left, (int16_t)(c.top - 1), s.right });
        break;
    case 6: /* kWindowContentRgn */
        hrgn_rect(&g, c);
        break;
    default: /* kWindowStructureRgn and the rest */
        hrgn_rect(&g, s);
        break;
    }
    rgn_write(rgn, &g);
    hrgn_free(&g);
    return noErr;
}

int32_t tb_DrawThemeTitleBarWidget(uint16_t flavor, gaddr content, uint32_t state, gaddr metrics, uint32_t attrs,
                                   uint16_t widget)
{
    qrect r = widget_rect(flavor, qrect_get(content), widget);
    ap_bevel_button(r, kThemeSmallBevelButton, state, false);
    if (widget == 1) /* zoom: a smaller box inside */
        ap_frame_grey((qrect){ (int16_t)(r.top + 2), (int16_t)(r.left + 2), (int16_t)(r.top + 7), (int16_t)(r.left + 7) }, 0);
    else if (widget == 2) /* collapse: a bar */
        ap_fill_grey((qrect){ (int16_t)(r.top + 4), (int16_t)(r.left + 2), (int16_t)(r.top + 6), (int16_t)(r.right - 2) }, 0);
    return noErr;
}

int32_t tb_DrawThemeWindowFrame(uint16_t flavor, gaddr content, uint32_t state, gaddr metrics, uint32_t attrs,
                                gaddr title_proc, uint32_t title_data)
{
    qrect c = qrect_get(content), s = window_struct(flavor, c);
    bool active = state != kThemeStateInactive;
    ap_frame_grey(s, active ? 0 : 0x8888);
    if (has_title(flavor)) {
        qrect bar = { (int16_t)(s.top + 1), (int16_t)(s.left + 1), (int16_t)(c.top - 1), (int16_t)(s.right - 1) };
        ap_fill_grey(bar, active ? 0xCCCC : 0xEEEE);
        ap_bevel(bar, 0xFFFF, 0x8888);
        ap_fill_grey((qrect){ (int16_t)(c.top - 1), (int16_t)(s.left + 1), c.top, (int16_t)(s.right - 1) }, active ? 0 : 0x8888);
        if (active) {
            /* Racing stripes either side of the title. */
            for (int y = bar.top + 3; y < bar.bottom - 3; y += 2) {
                ap_fill_grey((qrect){ (int16_t)y, (int16_t)(bar.left + 4), (int16_t)(y + 1), (int16_t)(bar.right - 4) }, 0xFFFF);
                ap_fill_grey((qrect){ (int16_t)(y + 1), (int16_t)(bar.left + 5), (int16_t)(y + 2), (int16_t)(bar.right - 4) }, 0x8888);
            }
            if (attrs & 0x20)
                tb_DrawThemeTitleBarWidget(flavor, content, state, metrics, attrs, 0);
            if (attrs & 0x18)
                tb_DrawThemeTitleBarWidget(flavor, content, state, metrics, attrs, 1);
            if (attrs & 0x40)
                tb_DrawThemeTitleBarWidget(flavor, content, state, metrics, attrs, 2);
        }
        if (title_proc) {
            /* WindowTitleDrawingProcPtr(const Rect *bounds, SInt16 depth, Boolean colorDevice, UInt32 userData) */
            gaddr b = tb_NewPtr(8, false, false);
            qrect_put(b, (qrect){ (int16_t)(bar.top + 1), (int16_t)(bar.left + 30), (int16_t)(bar.bottom - 1), (int16_t)(bar.right - 30) });
            int depth = qd_port_pixels(qd_port()).depth;
            uint32_t args[4] = { b, (uint32_t)depth, depth >= 4, title_data };
            mm_call(title_proc, 0x00003EC0 /* pascal (ptr, short, byte, long) */, args, 4);
            tb_DisposePtr(b);
        }
    } else {
        ap_fill_grey(inset(s, 1), 0xDDDD);
        ap_bevel(inset(s, 1), 0xFFFF, 0x8888);
        ap_frame_grey(inset(c, -1), active ? 0 : 0x8888);
    }
    return noErr;
}

int8_t tb_GetThemeWindowRegionHit(uint16_t flavor, gaddr content, uint32_t state, gaddr metrics, uint32_t attrs,
                                  uint32_t pt, gaddr out)
{
    qrect c = qrect_get(content), s = window_struct(flavor, c);
    int h = pt_h(pt), v = pt_v(pt);
    int16_t code = -1;
    if (h < s.left || h >= s.right || v < s.top || v >= s.bottom)
        code = -1;
    else if (h >= c.left && h < c.right && v >= c.top && v < c.bottom)
        code = (attrs & 1) && h >= c.right - 15 && v >= c.bottom - 15 ? 4 : 6;
    else if (v < c.top) {
        qrect cb = widget_rect(flavor, c, 0), zb = widget_rect(flavor, c, 1), xb = widget_rect(flavor, c, 2);
        if ((attrs & 0x20) && h >= cb.left && h < cb.right && v >= cb.top && v < cb.bottom)
            code = 2;
        else if ((attrs & 0x18) && h >= zb.left && h < zb.right && v >= zb.top && v < zb.bottom)
            code = 3;
        else if ((attrs & 0x40) && h >= xb.left && h < xb.right && v >= xb.top && v < xb.bottom)
            code = 7; /* kWindowCollapseBoxRgn */
        else
            code = 5;
    } else
        code = 5;
    if (out)
        wr16(out, (uint16_t)code);
    return code >= 0;
}

int32_t tb_DrawThemeScrollBarDelimiters(uint16_t flavor, gaddr content, uint32_t state, uint32_t attrs)
{
    qrect c = qrect_get(content);
    uint16_t v = state != kThemeStateInactive ? 0 : 0x8888;
    ap_fill_grey((qrect){ c.top, (int16_t)(c.right - 16), c.bottom, (int16_t)(c.right - 15) }, v);
    ap_fill_grey((qrect){ (int16_t)(c.bottom - 16), c.left, (int16_t)(c.bottom - 15), c.right }, v);
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* Menus */

int32_t tb_DrawThemeMenuBarBackground(gaddr rp, uint16_t state, uint32_t attrs)
{
    qrect r = rect_arg(rp);
    ap_fill_grey(r, 0xDDDD);
    if (ap_in_color())
        ap_fill_grey((qrect){ r.top, r.left, (int16_t)(r.top + 1), r.right }, 0xFFFF);
    ap_fill_grey((qrect){ (int16_t)(r.bottom - 1), r.left, r.bottom, r.right }, 0);
    return noErr;
}

/* A selected menu title or item: the accent colour, darkened. */
static void selection_fill(qrect r)
{
    uint16_t c[3];
    accent_shade(-6, c);
    ap_fill(r, c);
}

int32_t tb_DrawThemeMenuTitle(gaddr bar, gaddr title, uint16_t state, uint32_t attrs, gaddr proc, uint32_t data)
{
    qrect r = rect_arg(title), b = rect_arg(bar);
    r.top = (int16_t)(b.top + 1);
    r.bottom = (int16_t)(b.bottom - 1);
    if (state == 1)
        selection_fill(r);
    else
        ap_fill_grey(r, 0xDDDD);
    if (proc) {
        /* MenuTitleDrawingProcPtr(const Rect *bounds, SInt16 depth, Boolean colorDevice, SInt32 userData) */
        gaddr p = tb_NewPtr(8, false, false);
        qrect_put(p, (qrect){ r.top, (int16_t)(r.left + 7), r.bottom, (int16_t)(r.right - 7) });
        int depth = qd_port_pixels(qd_port()).depth;
        uint32_t args[4] = { p, (uint32_t)depth, depth >= 4, data };
        mm_call(proc, 0x00003EC0, args, 4);
        tb_DisposePtr(p);
    }
    return noErr;
}

int32_t tb_GetThemeMenuBarHeight(gaddr out)
{
    wr16(out, 20);
    return noErr;
}

int32_t tb_DrawThemeMenuBackground(gaddr rp, uint16_t type)
{
    qrect r = rect_arg(rp);
    ap_fill_grey(r, 0xDDDD);
    if (ap_in_color())
        ap_bevel(r, 0xFFFF, 0xAAAA);
    return noErr;
}

int32_t tb_GetThemeMenuBackgroundRegion(gaddr rp, uint16_t type, gaddr rgn)
{
    hrgn g;
    hrgn_init(&g);
    hrgn_rect(&g, rect_arg(rp));
    rgn_write(rgn, &g);
    hrgn_free(&g);
    return noErr;
}

int32_t tb_DrawThemeMenuItem(gaddr menu_rect, gaddr item_rect, int16_t top, int16_t bottom, uint16_t state,
                             uint16_t type, gaddr proc, uint32_t data)
{
    qrect r = rect_arg(item_rect);
    if (state == 1)
        selection_fill(r);
    else
        ap_fill_grey(r, 0xDDDD);
    int kind = type & 0xFF;
    uint16_t c[3];
    grey(c, state == 1 ? 0xFFFF : state == 3 ? 0x8888 : 0);
    if (kind == 1) /* hierarchical: an arrow at the right */
        ap_arrow((qrect){ r.top, (int16_t)(r.right - 14), r.bottom, (int16_t)(r.right - 4) }, 2, 9, c);
    else if (kind == 2 || kind == 3)
        ap_arrow(r, kind == 2 ? 3 : 1, 9, c);
    if (proc && kind <= 1) {
        /* MenuItemDrawingProcPtr(const Rect *bounds, SInt16 depth, Boolean colorDevice, SInt32 userData) */
        gaddr p = tb_NewPtr(8, false, false);
        qrect_put(p, r);
        int depth = qd_port_pixels(qd_port()).depth;
        uint32_t args[4] = { p, (uint32_t)depth, depth >= 4, data };
        mm_call(proc, 0x00003EC0, args, 4);
        tb_DisposePtr(p);
    }
    return noErr;
}

int32_t tb_DrawThemeMenuSeparator(gaddr rp)
{
    qrect r = rect_arg(rp);
    ap_fill_grey(r, 0xDDDD);
    ap_separator(r, kThemeStateActive);
    return noErr;
}

int32_t tb_GetThemeMenuSeparatorHeight(gaddr out)
{
    wr16(out, 9);
    return noErr;
}

int32_t tb_GetThemeMenuItemExtra(uint16_t type, gaddr height, gaddr width)
{
    if (height)
        wr16(height, 2);
    if (width)
        wr16(width, (type & 0xFF) == 1 ? 18 : 6);
    return noErr;
}

int32_t tb_GetThemeMenuTitleExtra(gaddr width, int8_t squished)
{
    wr16(width, squished ? 12 : 16);
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* Metrics, styles and fonts */

int32_t tb_GetThemeMetric(uint32_t metric, gaddr out)
{
    static const int16_t values[] = {
        16, /* scroll bar width */
        11, /* small scroll bar width */
        12, /* check box height */
        12, /* radio button height */
        3,  /* edit text whitespace */
        2,  /* edit text frame outset */
        2,  /* list box frame outset */
        3,  /* focus rect outset */
        2,  /* image well thickness */
        -1, /* scroll bar overlap */
        21, /* large tab height */
        8,  /* large tab caps width */
        3,  /* tab frame overlap */
        0,  /* tab indent or style */
        0,  /* tab overlap */
        16, /* small tab height */
        6,  /* small tab caps width */
        12, /* disclosure button height */
        20, /* round button size */
        20, /* push button height */
        18, /* list header height */
        12, /* small check box height */
        12, /* disclosure button width */
        12, /* small disclosure button height */
        12, /* small disclosure button width */
        12, /* disclosure triangle height */
        12, /* disclosure triangle width */
        22, /* little arrows height */
        13, /* little arrows width */
        7,  /* pane splitter height */
        20, /* pop-up button height */
        17, /* small pop-up button height */
        14, /* large progress bar thickness */
        20, /* pull-down height */
        17, /* small pull-down height */
        17, /* small push button height */
        12, /* small radio button height */
        12, /* relevance indicator height */
        15, /* resize control height */
        11, /* small resize control height */
        25, /* large round button size */
        16, /* horizontal slider height */
        5,  /* horizontal slider tick height */
    };
    if (metric >= sizeof values / sizeof values[0])
        return paramErr;
    wr32(out, (uint32_t)(int32_t)values[metric]);
    return noErr;
}

int32_t tb_GetThemeScrollBarThumbStyle(gaddr out)
{
    wr16(out, 1); /* proportional */
    return noErr;
}

int32_t tb_GetThemeScrollBarArrowStyle(gaddr out)
{
    wr16(out, 0); /* single arrows */
    return noErr;
}

int32_t tb_GetThemeCheckBoxStyle(gaddr out)
{
    wr16(out, 1); /* check marks */
    return noErr;
}

/* The theme's fonts: Charcoal 12 for the system font, Geneva 10 (bold
 * for the emphasized one) for the small ones and views. */
static void theme_font(int id, const char **name, int *size, int *style)
{
    *name = id == 0 ? "Charcoal" : "Geneva";
    *size = id == 0 ? 12 : 10;
    *style = id == 2 ? 1 : 0;
}

int32_t tb_GetThemeFont(uint16_t id, int16_t script, gaddr name_out, gaddr size_out, gaddr style_out)
{
    if (id > 3 && id != 100 && id != 101 && id != 102)
        return paramErr;
    const char *name;
    int size, style;
    theme_font(id >= 100 ? id - 100 : id, &name, &size, &style);
    if (name_out) {
        size_t n = strlen(name);
        wr8(name_out, (uint8_t)n);
        gput(name_out + 1, name, (uint32_t)n);
    }
    if (size_out)
        wr16(size_out, (uint16_t)size);
    if (style_out)
        wr8(style_out, (uint8_t)style);
    return noErr;
}

int32_t tb_UseThemeFont(uint16_t id, int16_t script)
{
    const char *name;
    int size, style;
    if (id > 3)
        return paramErr;
    theme_font(id, &name, &size, &style);
    gaddr s = tb_NewPtr(256, false, false);
    gaddr num = tb_NewPtr(2, false, true);
    size_t n = strlen(name);
    wr8(s, (uint8_t)n);
    gput(s + 1, name, (uint32_t)n);
    tb_GetFNum(s, num);
    tb_TextFont((int16_t)rd16(num));
    tb_TextSize((int16_t)size);
    tb_TextFace((int16_t)style);
    tb_DisposePtr(num);
    tb_DisposePtr(s);
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* Cursors and sounds */

/* The System's cursors where there is one (I-beam, cross, plus, watch);
 * the arrow for the rest. */
static int32_t theme_cursor(uint32_t cursor)
{
    int16_t id = 0;
    switch (cursor) {
    case 4: id = 1; break;                           /* kThemeIBeamCursor */
    case 5: id = 2; break;                           /* kThemeCrossCursor */
    case 6: id = 3; break;                           /* kThemePlusCursor */
    case 7: case 11: case 12: case 13: case 14: id = 4; break; /* the watch, counting and spinning */
    default:
        if (cursor > 17)
            return themeBadCursorIndexErr;
        break;
    }
    if (!id) {
        tb_InitCursor();
        return noErr;
    }
    gaddr h = tb_GetCursor(id);
    if (h && rd32(h))
        tb_SetCursor(rd32(h));
    else
        tb_InitCursor();
    return noErr;
}

int32_t tb_SetThemeCursor(uint32_t cursor) { return theme_cursor(cursor); }
int32_t tb_SetAnimatedThemeCursor(uint32_t cursor, uint32_t step) { return theme_cursor(cursor); }

/* The theme's sounds: Platinum's are off unless a sound track is chosen. */
int32_t tb_PlayThemeSound(int32_t kind) { return noErr; }
int32_t tb_BeginThemeDragSound(int32_t kind) { return noErr; }
int32_t tb_EndThemeDragSound(void) { return noErr; }
