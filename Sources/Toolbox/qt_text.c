/*
 * Text tracks (QuickTime 1.5 and later): captions drawn as QuickTime drew
 * them, with TextEdit in an offscreen GWorld, so they come out in the
 * classic bitmap fonts.  qt_native.c composites the picture into the
 * movie's frame.
 *
 * The sample description ('text'), after the standard 16 bytes:
 *     16  displayFlags (long)          20  textJustification (long:
 *     24  bgColor (RGBColor)               0 left, 1 centre, -1 right)
 *     30  defaultTextBox (Rect)        38  defaultStyle, a ScrpSTElement:
 *         start (long), height, ascent, font, face (a byte and a filler),
 *         size, colour
 *     58  the font's name (Pascal string), which wins over its number
 * A sample is a 16-bit length and that many MacRoman characters, then
 * atoms: 'styl' (a TextEdit style scrap: a count and ScrpSTElements),
 * 'ftab' (font numbers and names for 'styl'), 'hlit' (highlighted range:
 * two longs), 'hclr' (its colour), 'drpo' (drop shadow offset: h, v),
 * 'drpt' (its opacity, 0-256), 'tbox' (the text box for this sample).
 *
 * Display flags drawn: dontDisplay, clipToTextBox, dropShadow, keyedText
 * (no background: what is behind shows), inverseHilite.  Scrolling and
 * karaoke are not; the text stands still in its box.
 */
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "qd_internal.h"
#include "qt_text.h"
#include "tb_internal.h"

enum {
    dfDontDisplay = 1 << 0, dfClipToTextBox = 1 << 2, dfDropShadow = 1 << 12, dfKeyedText = 1 << 14,
    dfInverseHilite = 1 << 15,
};
enum { SC_LEN = 20 }; /* a ScrpSTElement */

struct qt_text {
    int w, h;
    uint32_t flags;
    int16_t just;
    uint16_t bg[3];
    qrect box;
    uint8_t style[SC_LEN];  /* the default style */
    uint8_t font_name[256]; /* Pascal string */
    bool font_resolved;
};

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put16(uint8_t *p, int v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

qt_text *qt_text_new(const uint8_t *desc, uint32_t len, int w, int h)
{
    if (len < 58 || w <= 0 || h <= 0 || w > 4096 || h > 4096)
        return NULL;
    qt_text *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->w = w;
    t->h = h;
    t->flags = be32(desc + 16);
    t->just = (int16_t)be32(desc + 20);
    for (int k = 0; k < 3; k++)
        t->bg[k] = (uint16_t)be16(desc + 24 + k * 2);
    t->box = (qrect){ (int16_t)be16(desc + 30), (int16_t)be16(desc + 32), (int16_t)be16(desc + 34),
                      (int16_t)be16(desc + 36) };
    memcpy(t->style, desc + 38, SC_LEN);
    memset(t->style, 0, 4); /* starts at the first character */
    /* The face is a byte and a filler; some writers put it in the low byte. */
    if (!t->style[10] && t->style[11] && t->style[11] < 0x80) {
        t->style[10] = t->style[11];
        t->style[11] = 0;
    }
    if (!be16(t->style + 12))
        put16(t->style + 12, 12);
    if (len > 58 && desc[58] && 59u + desc[58] <= len)
        memcpy(t->font_name, desc + 58, 1u + desc[58]);
    return t;
}

void qt_text_free(qt_text *t) { free(t); }

/* What the description says, changed at run time (TextMediaSetTextProperty). */
uint32_t qt_text_flags(const qt_text *t) { return t->flags; }
void qt_text_set_flags(qt_text *t, uint32_t flags) { t->flags = flags; }
int16_t qt_text_justification(const qt_text *t) { return t->just; }
void qt_text_set_justification(qt_text *t, int16_t just) { t->just = just; }
void qt_text_background(const qt_text *t, uint16_t rgb[3]) { memcpy(rgb, t->bg, sizeof t->bg); }
void qt_text_set_background(qt_text *t, const uint16_t rgb[3]) { memcpy(t->bg, rgb, sizeof t->bg); }
void qt_text_box(const qt_text *t, int16_t r[4])
{
    r[0] = t->box.top, r[1] = t->box.left, r[2] = t->box.bottom, r[3] = t->box.right;
}
void qt_text_set_box(qt_text *t, const int16_t r[4]) { t->box = (qrect){ r[0], r[1], r[2], r[3] }; }

/* The installed family with this name, or -1. */
static int font_named(const uint8_t *name)
{
    if (!name[0])
        return -1;
    gaddr s = tb_NewPtr(256 + 2, false, true);
    if (!s)
        return -1;
    gput(s, name, 1u + name[0]);
    wr16(s + 256, 0xFFFF);
    tb_GetFNum(s, s + 256);
    int16_t num = (int16_t)rd16(s + 256);
    tb_DisposePtr(s);
    /* GetFNum answers 0 for a name it does not know, and for Chicago. */
    if (num == 0 && !(name[0] == 7 && !strncasecmp((const char *)name + 1, "Chicago", 7)))
        return -1;
    return num;
}

typedef struct {
    const uint8_t *text;
    int len;
    const uint8_t *styl, *ftab;
    uint32_t styl_len, ftab_len;
    bool hilite;
    int32_t hl_start, hl_end;
    bool has_hclr;
    uint16_t hclr[3];
    int shadow_h, shadow_v, shadow_alpha; /* 0-256 */
    bool has_box;
    qrect box;
} sample;

static bool parse_sample(const uint8_t *d, uint32_t len, sample *s)
{
    memset(s, 0, sizeof *s);
    s->shadow_h = s->shadow_v = 2;
    s->shadow_alpha = 256;
    if (len < 2)
        return len == 0;
    s->len = (int)be16(d);
    if ((uint32_t)s->len + 2 > len)
        s->len = (int)len - 2;
    s->text = d + 2;
    for (const uint8_t *p = d + 2 + s->len, *end = d + len; p + 8 <= end;) {
        uint32_t n = be32(p);
        if (n < 8 || n > (uint64_t)(end - p))
            break;
        const uint8_t *b = p + 8;
        uint32_t bn = n - 8;
        if (!memcmp(p + 4, "styl", 4) && bn >= 2) {
            s->styl = b;
            s->styl_len = bn;
        } else if (!memcmp(p + 4, "ftab", 4) && bn >= 2) {
            s->ftab = b;
            s->ftab_len = bn;
        } else if (!memcmp(p + 4, "hlit", 4) && bn >= 8) {
            s->hilite = true;
            s->hl_start = (int32_t)be32(b);
            s->hl_end = (int32_t)be32(b + 4);
        } else if (!memcmp(p + 4, "hclr", 4) && bn >= 6) {
            s->has_hclr = true;
            for (int k = 0; k < 3; k++)
                s->hclr[k] = (uint16_t)be16(b + k * 2);
        } else if (!memcmp(p + 4, "drpo", 4) && bn >= 4) {
            s->shadow_h = (int16_t)be16(b);
            s->shadow_v = (int16_t)be16(b + 2);
        } else if (!memcmp(p + 4, "drpt", 4) && bn >= 2) {
            s->shadow_alpha = (int16_t)be16(b);
            if (s->shadow_alpha < 0)
                s->shadow_alpha = 0;
            if (s->shadow_alpha > 256)
                s->shadow_alpha = 256;
        } else if (!memcmp(p + 4, "tbox", 4) && bn >= 8) {
            s->has_box = true;
            s->box = (qrect){ (int16_t)be16(b), (int16_t)be16(b + 2), (int16_t)be16(b + 4), (int16_t)be16(b + 6) };
        }
        p += n;
    }
    return true;
}

/* The font a sample's 'ftab' names for a number in its style runs, by name
 * on this machine; else the number as it is. */
static int ftab_font(const sample *s, int num)
{
    if (!s->ftab)
        return num;
    int n = (int)be16(s->ftab);
    const uint8_t *p = s->ftab + 2, *end = s->ftab + s->ftab_len;
    for (int i = 0; i < n && p + 3 <= end && p + 3 + p[2] <= end; i++) {
        if ((int16_t)be16(p) == num) {
            int f = font_named(p + 2);
            return f >= 0 ? f : num;
        }
        p += 3 + p[2];
    }
    return num;
}

/* The style scrap for a sample: its 'styl' runs, or the default style; all
 * in black for a shadow. */
static gaddr style_scrap(const qt_text *t, const sample *s, bool shadow)
{
    int n = 1;
    if (s->styl) {
        n = (int)be16(s->styl);
        if ((uint32_t)n * SC_LEN + 2 > s->styl_len)
            n = (int)((s->styl_len - 2) / SC_LEN);
    }
    gaddr h = tb_NewHandle(2 + (int32_t)(n > 0 ? n : 1) * SC_LEN, false, true);
    if (!h)
        return 0;
    uint8_t e[SC_LEN];
    if (!s->styl || n <= 0) {
        n = 1;
        memcpy(e, t->style, SC_LEN);
        gput(rd32(h) + 2, e, SC_LEN);
    } else {
        for (int i = 0; i < n; i++) {
            memcpy(e, s->styl + 2 + i * SC_LEN, SC_LEN);
            put16(e + 8, ftab_font(s, (int16_t)be16(e + 8)));
            gput(rd32(h) + 2 + (uint32_t)i * SC_LEN, e, SC_LEN);
        }
    }
    wr16(rd32(h), (uint16_t)n);
    if (shadow)
        for (int i = 0; i < n; i++)
            gmemset(rd32(h) + 2 + (uint32_t)i * SC_LEN + 14, 0, 6);
    return h;
}

/* One drawing of the text in the GWorld's port, over the background. */
static void draw_pass(const qt_text *t, const sample *s, gaddr text, qrect dest, qrect view, bool shadow)
{
    gaddr r = tb_NewPtr(16, false, true);
    qrect_put(r, dest);
    qrect_put(r + 8, view);
    gaddr bounds = tb_NewPtr(8, false, true);
    qrect_put(bounds, (qrect){ 0, 0, (int16_t)t->h, (int16_t)t->w });
    tb_EraseRect(bounds);
    gaddr teh = tb_TEStyleNew(r, r + 8);
    gaddr scrap = style_scrap(t, s, shadow);
    tb_TESetAlignment(t->just, teh);
    tb_TEStyleInsert(text, s->len, scrap, teh);
    tb_TEUpdate(r + 8, teh);
    if (s->hilite && !shadow && s->hl_end > s->hl_start) {
        /* The range's lines, as TextEdit highlights them, in the
         * highlight colour (or inverted). */
        tb_TESetSelect(s->hl_start, s->hl_end, teh);
        gaddr rgn = tb_NewRgn();
        tb_TEGetHiliteRgn(rgn, teh);
        uint8_t saved[6];
        gget(saved, LM_HiliteRGB, 6);
        if (s->has_hclr)
            for (int k = 0; k < 3; k++)
                wr16(LM_HiliteRGB + (uint32_t)k * 2, s->hclr[k]);
        if (!(t->flags & dfInverseHilite))
            wr8(LM_HiliteMode, (uint8_t)(rd8(LM_HiliteMode) & 0x7F));
        tb_InvertRgn(rgn);
        gput(LM_HiliteRGB, saved, 6);
        tb_DisposeRgn(rgn);
    }
    if (scrap)
        tb_DisposeHandle(scrap);
    tb_TEDispose(teh);
    tb_DisposePtr(bounds);
    tb_DisposePtr(r);
}

/* The GWorld's pixels (xRGB) into host memory. */
static void read_pixels(gaddr gw, int w, int h, uint32_t *out)
{
    gaddr pm = rd32(tb_GetGWorldPixMap(gw));
    gaddr base = qd_pixmap_base(pm, true);
    uint32_t row = rd16(pm + PixMap_rowBytes_off) & 0x3FFF;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            out[(size_t)y * (size_t)w + (size_t)x] = rd32(base + (uint32_t)y * row + (uint32_t)x * 4) & 0xFFFFFF;
}

bool qt_text_draw(qt_text *t, const uint8_t *data, uint32_t len, uint8_t *rgba)
{
    sample s;
    if ((t->flags & dfDontDisplay) || !g_mem || !parse_sample(data, len, &s))
        return false;
    if (!t->font_resolved) {
        int f = font_named(t->font_name);
        if (f >= 0)
            put16(t->style + 8, f);
        t->font_resolved = true;
    }
    size_t n = (size_t)t->w * (size_t)t->h;
    uint32_t *main = malloc(n * 4), *shade = (t->flags & dfDropShadow) ? malloc(n * 4) : NULL;
    if (!main || ((t->flags & dfDropShadow) && !shade)) {
        free(main);
        free(shade);
        return false;
    }
    gaddr saved = tb_NewPtr(8, false, true), gwp = tb_NewPtr(4, false, true), r = tb_NewPtr(8, false, true);
    qrect bounds = { 0, 0, (int16_t)t->h, (int16_t)t->w };
    qrect_put(r, bounds);
    tb_GetGWorld(saved, saved + 4);
    if (tb_NewGWorld(gwp, 32, r, 0, 0, 0) || !rd32(gwp)) {
        tb_DisposePtr(saved);
        tb_DisposePtr(gwp);
        tb_DisposePtr(r);
        free(main);
        free(shade);
        return false;
    }
    gaddr gw = rd32(gwp);
    tb_LockPixels(tb_GetGWorldPixMap(gw));
    tb_SetGWorld(gw, 0);
    gaddr c = tb_NewPtr(6, false, true);
    for (int k = 0; k < 3; k++)
        wr16(c + (uint32_t)k * 2, t->bg[k]);
    tb_RGBBackColor(c);
    gaddr text = tb_NewPtr(s.len > 0 ? s.len : 1, false, false);
    if (s.len > 0)
        gput(text, s.text, (uint32_t)s.len);
    qrect box = s.has_box ? s.box : t->box;
    if (qrect_empty(box))
        box = bounds;
    qrect view = (t->flags & dfClipToTextBox) ? box : bounds;
    tb_EraseRect(r);
    uint32_t bgpx = rd32(qd_pixmap_base(rd32(tb_GetGWorldPixMap(gw)), true)) & 0xFFFFFF;
    if (shade) {
        /* The shadow: black text on white, wherever it is not white. */
        gaddr white = tb_NewPtr(6, false, false);
        gmemset(white, 0xFF, 6);
        tb_RGBBackColor(white);
        tb_DisposePtr(white);
        qrect d = { (int16_t)(box.top + s.shadow_v), (int16_t)(box.left + s.shadow_h),
                    (int16_t)(box.bottom + s.shadow_v), (int16_t)(box.right + s.shadow_h) };
        draw_pass(t, &s, text, d, view, true);
        read_pixels(gw, t->w, t->h, shade);
        tb_RGBBackColor(c);
    }
    draw_pass(t, &s, text, box, view, false);
    read_pixels(gw, t->w, t->h, main);
    tb_SetGWorld(rd32(saved), rd32(saved + 4));
    tb_DisposeGWorld(gw);
    tb_DisposePtr(text);
    tb_DisposePtr(c);
    tb_DisposePtr(saved);
    tb_DisposePtr(gwp);
    tb_DisposePtr(r);

    /* The background (none for keyed text), the shadow, then the text. */
    bool keyed = t->flags & dfKeyedText;
    for (size_t i = 0; i < n; i++) {
        uint8_t *o = rgba + i * 4;
        o[0] = (uint8_t)(bgpx >> 16);
        o[1] = (uint8_t)(bgpx >> 8);
        o[2] = (uint8_t)bgpx;
        o[3] = keyed ? 0 : 255;
        if (shade && shade[i] != 0xFFFFFF) {
            int a = s.shadow_alpha;
            if (keyed && !o[3]) {
                o[0] = o[1] = o[2] = 0;
                o[3] = (uint8_t)(a >= 256 ? 255 : a);
            } else {
                for (int k = 0; k < 3; k++)
                    o[k] = (uint8_t)(o[k] * (256 - a) / 256);
            }
        }
        if (main[i] != bgpx) {
            o[0] = (uint8_t)(main[i] >> 16);
            o[1] = (uint8_t)(main[i] >> 8);
            o[2] = (uint8_t)main[i];
            o[3] = 255;
        }
    }
    free(main);
    free(shade);
    return true;
}
