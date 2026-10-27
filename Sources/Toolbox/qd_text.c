/*
 * Font Manager and QuickDraw text: bitmap fonts (FONT/NFNT, found through
 * FOND families) drawn with the port's text mode, style and clipping.
 */
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "m68k.h"
#include "qd_internal.h"
#include "tb_internal.h"
#include "trap.h"

enum { bold = 1, italic = 2, underline = 4, outline = 8, shadow = 16, condense = 32, extend = 64 };

typedef struct {
    gaddr rec;          /* FontRec in guest memory (locked while used) */
    gaddr handle;
    int first, last;
    int kern, ascent, descent, leading, widmax;
    int height, row_words;
    gaddr image, loc, ow;
    int face;
} strike;

static int16_t rd16s(gaddr a) { return (int16_t)rd16(a); }

/* The families a System 7 Mac had installed, by their classic names and
 * numbers, and the family of ours that draws each. */
static const struct {
    int16_t num;
    const char *name;
    int16_t drawn_as;
} classic_fonts[] = {
    { 0, "Chicago", 0 },  { 2, "New York", 2 },  { 3, "Geneva", 3 },  { 4, "Monaco", 4 },
    { 16, "Palatino", 2 }, /* HyperCard's stacks ask for it */
    { 20, "Times", 2 },   { 21, "Helvetica", 3 }, { 22, "Courier", 4 }, { 23, "Symbol", 3 },
    { 2002, "Charcoal", 2002 }, /* Mac OS 8's system font */
};

static int classic_index(int num)
{
    for (unsigned i = 0; i < sizeof classic_fonts / sizeof classic_fonts[0]; i++)
        if (classic_fonts[i].num == num)
            return (int)i;
    return -1;
}

/* The font resource ID for a family and size, through its FOND if any. */
static gaddr font_resource_of(int family, int size);

static gaddr font_resource_lookup(int family, int size)
{
    gaddr h = font_resource_of(family, size);
    int k;
    if (!h && (k = classic_index(family)) >= 0 && classic_fonts[k].drawn_as != family)
        h = font_resource_of(classic_fonts[k].drawn_as, size);
    return h;
}

/* The last few answers kept: every measurement loads the strike, and
 * finding it searched the open resource files each time (TextEdit placing
 * its caret measured the line again for every character inserted, and
 * Sirtet's Keys text took minutes).  An answer holds while the resource
 * files in use are the same and its handle still holds the same data. */
static gaddr font_resource(int family, int size)
{
    enum { NFONTS = 16 };
    static struct {
        int family, size;
        uint32_t top;
        uint16_t cur;
        gaddr h, data;
    } fonts[NFONTS];
    static int next;
    uint32_t top = rd32(LM_TopMapHndl);
    uint16_t cur = rd16(LM_CurMap);
    for (int i = 0; i < NFONTS; i++)
        if (fonts[i].h && fonts[i].family == family && fonts[i].size == size && fonts[i].top == top &&
            fonts[i].cur == cur && rd32(fonts[i].h) == fonts[i].data)
            return fonts[i].h;
    gaddr h = font_resource_lookup(family, size);
    if (h && rd32(h)) {
        int i = next++ % NFONTS;
        fonts[i].family = family;
        fonts[i].size = size;
        fonts[i].top = top;
        fonts[i].cur = cur;
        fonts[i].h = h;
        fonts[i].data = rd32(h);
    }
    return h;
}

static gaddr font_resource_of(int family, int size)
{
    gaddr fond = tb_GetResource((int32_t)fourcc("FOND"), (int16_t)family);
    if (fond && rd32(fond)) {
        gaddr f = rd32(fond);
        gaddr assoc = f + FamRec_SIZEOF;
        int n = (int)rd16(assoc) + 1;
        int best_id = -1, best_d = 1 << 30;
        for (int i = 0; i < n; i++) {
            gaddr e = assoc + 2 + (uint32_t)i * 6;
            int fsize = rd16s(e), fstyle = rd16s(e + 2), fid = rd16s(e + 4);
            if (fstyle != 0 || fsize == 0) /* plain bitmap strikes only (0 = outline) */
                continue;
            int d = abs(fsize - size) * 2 + (fsize > size);
            if (d < best_d) {
                best_d = d;
                best_id = fid;
            }
        }
        if (best_id >= 0) {
            gaddr h = tb_GetResource((int32_t)fourcc("NFNT"), (int16_t)best_id);
            if (!h)
                h = tb_GetResource((int32_t)fourcc("FONT"), (int16_t)best_id);
            if (h && rd32(h))
                return h;
        }
    }
    gaddr h = tb_GetResource((int32_t)fourcc("FONT"), (int16_t)(family * 128 + size));
    return h && rd32(h) ? h : 0;
}

static void port_font(gaddr port, int *family, int *size, int *face)
{
    *family = GrafPort_txFont(port);
    *size = GrafPort_txSize(port);
    *face = GrafPort_txFace(port);
    if (*family == 0) {
        *family = rd16s(LM_SysFontFam);
        if (*size == 0)
            *size = rd16s(LM_SysFontSiz) ? rd16s(LM_SysFontSiz) : 12;
    } else if (*family == 1) {
        *family = rd16s(LM_ApFontID);
    }
    if (*size <= 0)
        *size = 12;
}

static bool load_strike(gaddr port, strike *s)
{
    int family, size, face;
    port_font(port, &family, &size, &face);
    uint8_t saved = rd8(LM_ResLoad);
    wr8(LM_ResLoad, 1);
    gaddr h = font_resource(family, size);
    if (!h && family != rd16s(LM_SysFontFam))
        h = font_resource(rd16s(LM_SysFontFam), 12); /* fall back to the system font */
    wr8(LM_ResLoad, saved);
    memset(s, 0, sizeof *s);
    s->face = face;
    if (!h)
        return false;
    gaddr f = rd32(h);
    s->handle = h;
    s->rec = f;
    s->first = rd16s(f + 2);
    s->last = rd16s(f + 4);
    s->widmax = rd16s(f + 6);
    s->kern = rd16s(f + 8);
    int ndescent = rd16s(f + 10);
    s->height = rd16s(f + 14);
    uint32_t owtloc = rd16(f + 16);
    if (ndescent > 0) /* NFNT: high word of owTLoc */
        owtloc |= (uint32_t)ndescent << 16;
    s->ascent = rd16s(f + 18);
    s->descent = rd16s(f + 20);
    s->leading = rd16s(f + 22);
    s->row_words = rd16s(f + 24);
    s->image = f + 26;
    s->loc = s->image + (uint32_t)s->row_words * 2u * (uint32_t)s->height;
    s->ow = f + 16 + owtloc * 2;
    return true;
}

/* Index of a character's glyph (the missing-glyph slot if absent). */
static int glyph_index(const strike *s, uint8_t c, int *offset, int *width)
{
    int missing = s->last - s->first + 1;
    int i = (c < s->first || c > s->last) ? missing : c - s->first;
    uint16_t ow = rd16(s->ow + (uint32_t)i * 2);
    if (ow == 0xFFFF) {
        i = missing;
        ow = rd16(s->ow + (uint32_t)i * 2);
    }
    *offset = (ow >> 8) + s->kern;
    *width = ow & 0xFF;
    return i;
}

static int style_extra(int face)
{
    int e = 0;
    if (face & bold)
        e++;
    if (face & outline)
        e++;
    if (face & shadow)
        e += 2;
    if (face & condense)
        e--;
    if (face & extend)
        e++;
    return e;
}

static int char_advance(const strike *s, uint8_t c, gaddr port)
{
    if (!s->rec)
        return 7;
    int off, w;
    glyph_index(s, c, &off, &w);
    int adv = w + style_extra(s->face);
    if (c == ' ') {
        int32_t sp = (int32_t)GrafPort_spExtra(port);
        adv += (sp + 0x8000) >> 16;
    }
    return adv;
}

/* CharExtra's widening of every character but the space, as a Fixed
 * number of pixels. The colour port keeps it in chExtra as 4.12 per point
 * of txSize, so it scales with the size; a basic port has no chExtra. */
static int32_t char_extra(gaddr port)
{
    if (!qd_port_is_color(port))
        return 0;
    int family, size, face;
    port_font(port, &family, &size, &face);
    return (int32_t)CGrafPort_chExtra(port) * size * 16;
}

/* Where each character starts, from the start of the text: the extra is
 * fractional, so it accumulates as a Fixed and is rounded once per
 * character rather than lost on each one, and measuring and drawing step
 * through the same positions. */
static int pen_offset(int32_t fixed) { return (int)((fixed + 0x8000) >> 16); }

static int32_t char_step(const strike *s, uint8_t c, gaddr port, int32_t extra)
{
    return ((int32_t)char_advance(s, c, port) << 16) + (c == ' ' ? 0 : extra);
}

static void note_width_table(const strike *s, gaddr port);

int qd_text_width(gaddr port, const uint8_t *text, int len)
{
    strike s;
    if (load_strike(port, &s))
        note_width_table(&s, port);
    int32_t extra = char_extra(port), w = 0;
    for (int i = 0; i < len; i++)
        w += char_step(&s, text[i], port, extra);
    return pen_offset(w);
}

/* Draw one glyph's bitmap with its top-left at (x, top) in local coordinates. */
static void draw_glyph(const strike *s, int idx, int x, int top, const pixdst *d, const hrgn *clip, int mode,
                       uint32_t fg, uint32_t bk)
{
    int c0 = (int16_t)rd16(s->loc + (uint32_t)idx * 2), c1 = (int16_t)rd16(s->loc + (uint32_t)idx * 2 + 2);
    int gw = c1 - c0;
    if (gw <= 0)
        return;
    gaddr row_base = s->image;
    uint32_t row_bytes = (uint32_t)s->row_words * 2;
    for (int r = 0; r < s->height; r++) {
        int y = top + r;
        int n;
        const int16_t *xs = hrgn_row(clip, y, &n);
        if (!xs)
            continue;
        int italic_shift = (s->face & italic) ? (s->ascent - r) / 2 : 0;
        for (int k = 0; k < gw; k++) {
            int col = c0 + k;
            int bit = (rd8(row_base + (uint32_t)r * row_bytes + (uint32_t)(col >> 3)) >> (7 - (col & 7))) & 1;
            int xx = x + k + italic_shift;
            bool in = false;
            for (int j = 0; j < n; j += 2)
                if (xx >= xs[j] && xx < xs[j + 1])
                    in = true;
            if (!in)
                continue;
            if (!bit && (mode & 3) != 0) /* or/xor/bic leave unset pixels alone */
                continue;
            int dc = xx - d->bounds.left, dr = y - d->bounds.top;
            put_px(d, dc, dr, apply_mode(mode, bit, get_px(d, dc, dr), fg, bk, px_mask(d->depth)));
            if ((s->face & bold) && bit && xx + 1 < xs[n - 1]) {
                int dc2 = dc + 1;
                put_px(d, dc2, dr, apply_mode(mode, 1, get_px(d, dc2, dr), fg, bk, px_mask(d->depth)));
            }
        }
    }
}

/* srcCopy and notSrcCopy paint a character's whole cell, its advance wide
 * from the ascent to the descent, the glyph's unset pixels in the
 * background: programs redraw a number over the last one so (Maelstrom's
 * score and lives), and the old digits must not show through. */
static void paint_cell(const strike *s, int x, int w, int base, const pixdst *d, const hrgn *clip, int mode,
                       uint32_t fg, uint32_t bk)
{
    for (int y = base - s->ascent; y < base + s->descent; y++) {
        int n;
        const int16_t *xs = hrgn_row(clip, y, &n);
        if (!xs)
            continue;
        for (int j = 0; j < n; j += 2) {
            int x0 = xs[j] > x ? xs[j] : x, x1 = xs[j + 1] < x + w ? xs[j + 1] : x + w;
            for (int xx = x0; xx < x1; xx++) {
                int dc = xx - d->bounds.left, dr = y - d->bounds.top;
                put_px(d, dc, dr, apply_mode(mode, 0, get_px(d, dc, dr), fg, bk, px_mask(d->depth)));
            }
        }
    }
}

void qd_draw_text(gaddr port, const uint8_t *text, int len)
{
    /* A negative count (DrawText's is a signed integer) draws nothing. */
    if (len < 0)
        len = 0;
    strike s;
    bool have = load_strike(port, &s);
    if (have)
        note_width_table(&s, port);
    uint32_t loc = rd32(port + GrafPort_pnLoc_off);
    int x = pt_h(loc), base = pt_v(loc);
    int start = x;
    qd_pic_text(port, x, base, text, len);
    int32_t extra = char_extra(port), at = 0;
    if (have && GrafPort_pnVis(port) < 0) {
        /* A hidden pen (a picture being recorded) draws nothing. */
        for (int i = 0; i < len; i++)
            at += char_step(&s, text[i], port, extra);
        x = start + pen_offset(at);
    } else if (have) {
        int8_t st = tb_HGetState(s.handle);
        tb_HLock(s.handle);
        load_strike(port, &s); /* re-read after locking */
        pixdst d = qd_port_pixels(port);
        hrgn clip;
        hrgn_init(&clip);
        qd_port_clip(port, &clip);
        uint32_t fg, bk;
        qd_port_bits(port, &fg, &bk);
        int mode = GrafPort_txMode(port) & 7;
        for (int i = 0; i < len; i++) {
            int off, w;
            int idx = glyph_index(&s, text[i], &off, &w);
            at += char_step(&s, text[i], port, extra);
            int adv = start + pen_offset(at) - x;
            if ((mode & 3) == 0 && adv > 0)
                paint_cell(&s, x, adv, base, &d, &clip, mode, fg, bk);
            draw_glyph(&s, idx, x + off, base - s.ascent, &d, &clip, mode, fg, bk);
            x += adv;
        }
        if ((s.face & underline) && x > start) {
            hrgn line;
            hrgn_init(&line);
            hrgn_rect(&line, (qrect){ (int16_t)(base + 1), (int16_t)start, (int16_t)(base + 2), (int16_t)x });
            hrgn_op(&line, &line, &clip, RGN_SECT);
            qd_fill_region(port, &line, qd_pat_black, 8 + (mode & 3));
            hrgn_free(&line);
        }
        hrgn_free(&clip);
        tb_HSetState(s.handle, st);
        if (qd_rec_active(port)) {
            int family, size, face;
            port_font(port, &family, &size, &face);
            qd_rec_text(port, family, size, face, start, base, text, len, x - start);
        }
    } else {
        x += 7 * len;
    }
    wr32(port + GrafPort_pnLoc_off, make_pt((int16_t)base, (int16_t)x));
}

/* ---- Traps ---- */

void tb_TextFont(int16_t f) { GrafPort_set_txFont(qd_port(), f); }
void tb_TextFace(int16_t thef) { GrafPort_set_txFace(qd_port(), (uint8_t)thef); }
void tb_TextMode(int16_t m) { GrafPort_set_txMode(qd_port(), m); }
void tb_TextSize(int16_t s) { GrafPort_set_txSize(qd_port(), s); }
void tb_SpaceExtra(int32_t e) { GrafPort_set_spExtra(qd_port(), e); }
/* CharExtra keeps the extra per point of the current size, in 4.12 (Inside
 * Macintosh: Text), which is why programs set the size first; a colour port
 * alone has the field, and a basic port ignores the call. */
void tb_CharExtra(int32_t extra)
{
    gaddr port = qd_port();
    if (!qd_port_is_color(port))
        return;
    int family, size, face;
    port_font(port, &family, &size, &face);
    int64_t v = ((int64_t)extra / size + 8) >> 4;
    CGrafPort_set_chExtra(port, (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v));
}

static void draw_guest(gaddr p, int n)
{
    uint8_t *buf = malloc(n > 0 ? (size_t)n : 1);
    gget(buf, p, (uint32_t)(n > 0 ? n : 0));
    qd_draw_text(qd_port(), buf, n);
    free(buf);
}

/* Text goes through the port's textProc when the application installed one. */
static void text_entry(gaddr p, int n)
{
    gaddr port = qd_port();
    gaddr procs = port ? GrafPort_grafProcs(port) : 0;
    gaddr proc = procs ? rd32(procs + QDProcs_textProc_off) : 0;
    if (!proc || native_index_for(proc) >= 0) {
        draw_guest(p, n);
        return;
    }
    cpu_push16(g_cpu, (uint16_t)n);
    cpu_push32(g_cpu, p);
    cpu_push32(g_cpu, 0x00010001); /* numer, denom: 1:1 */
    cpu_push32(g_cpu, 0x00010001);
    guest_call(proc);
}

void tb_DrawChar(int16_t thec)
{
    gaddr c = tb_NewPtr(1, false, false);
    wr8(c, (uint8_t)thec);
    text_entry(c, 1);
    tb_DisposePtr(c);
}

void tb_DrawString(gaddr s) { text_entry(s + 1, rd8(s)); }
void tb_DrawText(gaddr tb, int16_t fb, int16_t bc) { text_entry(tb + (uint32_t)fb, bc); }
void tb_StdText(int16_t n, gaddr p, uint32_t numer, uint32_t denom) { draw_guest(p, n); }

int16_t tb_StdTxMeas(int16_t n, gaddr p, gaddr numer, gaddr denom, gaddr info)
{
    if (info)
        tb_GetFontInfo(info);
    return tb_TextWidth(p, 0, n);
}

int16_t tb_CharWidth(int16_t thec)
{
    uint8_t c = (uint8_t)thec;
    return (int16_t)qd_text_width(qd_port(), &c, 1);
}

static int width_guest(gaddr p, int n)
{
    uint8_t *buf = malloc(n > 0 ? (size_t)n : 1);
    gget(buf, p, (uint32_t)(n > 0 ? n : 0));
    int w = qd_text_width(qd_port(), buf, n);
    free(buf);
    return w;
}

int16_t tb_StringWidth(gaddr s) { return (int16_t)width_guest(s + 1, rd8(s)); }
int16_t tb_TextWidth(gaddr tb, int16_t fb, int16_t bc) { return (int16_t)width_guest(tb + (uint32_t)fb, bc); }

void tb_GetFontInfo(gaddr ip)
{
    strike s;
    gaddr port = qd_port();
    if (!load_strike(port, &s)) {
        wr16(ip, 9);
        wr16(ip + 2, 3);
        wr16(ip + 4, 7);
        wr16(ip + 6, 0);
        return;
    }
    wr16(ip + FontInfo_ascent_off, (uint16_t)s.ascent);
    wr16(ip + FontInfo_descent_off, (uint16_t)s.descent);
    wr16(ip + FontInfo_widMax_off, (uint16_t)(s.widmax + style_extra(s.face)));
    wr16(ip + FontInfo_leading_off, (uint16_t)s.leading);
}

void tb_InitFonts(void)
{
    /* Font data is found on demand; just make sure the defaults are sane. */
    if (!rd16(LM_SysFontSiz))
        wr16(LM_SysFontSiz, 12);
}

void tb_GetFontName(int16_t fnum, gaddr fnam)
{
    wr8(fnam, 0);
    int k = classic_index(fnum);
    if (k >= 0) {
        size_t n = strlen(classic_fonts[k].name);
        wr8(fnam, (uint8_t)n);
        gput(fnam + 1, classic_fonts[k].name, (uint32_t)n);
        return;
    }
    gaddr name = tb_NewPtr(256, false, true);
    gaddr id = tb_NewPtr(2, false, false), type = tb_NewPtr(4, false, false);
    uint8_t saved = rd8(LM_ResLoad);
    wr8(LM_ResLoad, 0);
    gaddr h = tb_GetResource((int32_t)fourcc("FOND"), fnum);
    if (!h)
        h = tb_GetResource((int32_t)fourcc("FONT"), (int16_t)(fnum * 128));
    wr8(LM_ResLoad, saved);
    if (h) {
        tb_GetResInfo(h, id, type, name);
        gmemcpy(fnam, name, (uint32_t)rd8(name) + 1);
    }
    tb_DisposePtr(name);
    tb_DisposePtr(id);
    tb_DisposePtr(type);
}

void tb_GetFNum(gaddr fnam, gaddr fnum)
{
    wr16(fnum, 0);
    uint8_t saved = rd8(LM_ResLoad);
    wr8(LM_ResLoad, 0);
    gaddr h = tb_GetNamedResource((int32_t)fourcc("FOND"), fnam);
    gaddr id = tb_NewPtr(2, false, false), type = tb_NewPtr(4, false, false);
    if (h) {
        tb_GetResInfo(h, id, type, 0);
        wr16(fnum, rd16(id));
    } else if ((h = tb_GetNamedResource((int32_t)fourcc("FONT"), fnam))) {
        tb_GetResInfo(h, id, type, 0);
        wr16(fnum, (uint16_t)((int16_t)rd16(id) / 128));
    } else {
        char name[256];
        pstr_to_c(fnam, name, sizeof name);
        for (unsigned i = 0; i < sizeof classic_fonts / sizeof classic_fonts[0]; i++)
            if (!strcasecmp(name, classic_fonts[i].name))
                wr16(fnum, (uint16_t)classic_fonts[i].num);
    }
    wr8(LM_ResLoad, saved);
    tb_DisposePtr(id);
    tb_DisposePtr(type);
}

int8_t tb_RealFont(int16_t fnum, int16_t sz)
{
    if (sz == 0) /* size 0 is the default size, as TextSize 0 is: the system font's */
        sz = rd16(LM_SysFontSiz) ? (int16_t)rd16(LM_SysFontSiz) : 12;
    int k = classic_index(fnum);
    if (k >= 0 && classic_fonts[k].drawn_as != fnum && !tb_GetResource((int32_t)fourcc("FOND"), fnum))
        fnum = classic_fonts[k].drawn_as;
    gaddr fond = tb_GetResource((int32_t)fourcc("FOND"), fnum);
    if (fond && rd32(fond)) {
        gaddr assoc = rd32(fond) + FamRec_SIZEOF;
        int n = (int)rd16(assoc) + 1;
        for (int i = 0; i < n; i++) {
            int size = rd16s(assoc + 2 + (uint32_t)i * 6);
            if (size == sz || size == 0)
                return 1;
        }
        return 0;
    }
    return tb_GetResource((int32_t)fourcc("FONT"), (int16_t)(fnum * 128 + sz)) != 0;
}

void tb_SetFontLock(int8_t lflag) {}
void tb_SetFScaleDisable(int8_t disable) { wr8(LM_FScaleDisable, disable ? 0xFF : 0); }
void tb_SetFractEnable(int8_t enable) { wr8(LM_FractEnable, enable ? 0xFF : 0); }

static gaddr fmoutput;

/* The width table of the font FMSwapFont last chose (WidthTabHandle, and
 * WidthPtr to it), as the Font Manager kept it for programs that measure
 * text themselves: each character's advance as a Fixed, and the font. */
static void fill_width_table(const strike *s, gaddr port, int16_t family, int16_t size, uint8_t face, uint16_t device,
                             uint32_t numer, uint32_t denom)
{
    gaddr h = rd32(LM_WidthTabHandle);
    if (!h || !rd32(h)) {
        h = mm_new_handle_in(rd32(LM_SysZone), WidthTable_SIZEOF, true);
        if (!h)
            return;
        wr32(LM_WidthTabHandle, h);
        gaddr list = mm_new_handle_in(rd32(LM_SysZone), 4, true); /* the cached tables: just this one */
        if (list) {
            wr32(rd32(list), h);
            wr32(LM_WidthListHand, list);
        }
    }
    gaddr t = rd32(h);
    for (int c = 0; c < 256; c++)
        wr32(t + WidthTable_tabData_off + (uint32_t)c * 4, (uint32_t)char_advance(s, (uint8_t)c, port) << 16);
    WidthTable_set_tabFont(t, s->handle);
    WidthTable_set_sExtra(t, (int32_t)GrafPort_spExtra(port));
    wr32(t + WidthTable_style_off, (uint32_t)style_extra(s->face) << 16);
    wr16(t + WidthTable_fID_off, (uint16_t)family);
    wr16(t + WidthTable_fSize_off, (uint16_t)size);
    wr16(t + WidthTable_face_off, face);
    wr16(t + WidthTable_device_off, device);
    wr32(t + WidthTable_inNumer_off, numer);
    wr32(t + WidthTable_inDenom_off, denom);
    wr16(t + WidthTable_aFID_off, (uint16_t)family);
    uint16_t res_err = rd16(LM_ResErr); /* measuring text leaves ResErr alone */
    wr32(t + WidthTable_fHand_off, tb_GetResource((int32_t)fourcc("FOND"), family));
    wr16(LM_ResErr, res_err);
    wr8(t + WidthTable_usedFam_off, rd32(t + WidthTable_fHand_off) ? 1 : 0);
    wr8(t + WidthTable_aFace_off, (uint8_t)s->face);
    wr16(t + WidthTable_vOutput_off, 256); /* no scaling: 1.0 as 8.8 */
    wr16(t + WidthTable_hOutput_off, 256);
    wr16(t + WidthTable_vFactor_off, 256);
    wr16(t + WidthTable_hFactor_off, 256);
    wr16(t + WidthTable_aSize_off, (uint16_t)size);
    wr16(t + WidthTable_tabSize_off, WidthTable_SIZEOF);
    wr32(LM_WidthPtr, t);
    wr32(LM_LastSPExtra, (uint32_t)GrafPort_spExtra(port));
}

/* Measuring and drawing go through FMSwapFont on a real Mac, so the width
 * table follows the port's font: rebuilt when that changes. */
static void note_width_table(const strike *s, gaddr port)
{
    gaddr h = rd32(LM_WidthTabHandle);
    gaddr t = h ? rd32(h) : 0;
    int16_t family = GrafPort_txFont(port), size = GrafPort_txSize(port);
    uint8_t face = GrafPort_txFace(port);
    if (t && rd32(LM_WidthPtr) == t && (int16_t)rd16(t + WidthTable_fID_off) == family &&
        (int16_t)rd16(t + WidthTable_fSize_off) == size && rd16(t + WidthTable_face_off) == face &&
        WidthTable_sExtra(t) == (int32_t)GrafPort_spExtra(port) && WidthTable_tabFont(t) == s->handle)
        return;
    fill_width_table(s, port, family, size, face, 0, 0x00010001, 0x00010001);
}

gaddr tb_FMSwapFont(gaddr fmip)
{
    if (!fmoutput)
        fmoutput = tb_NewPtr(FMOutput_SIZEOF, true, true);
    /* Evaluate the request in a scratch port state. */
    gaddr port = qd_port();
    int16_t f = GrafPort_txFont(port), sz = GrafPort_txSize(port);
    uint8_t face = GrafPort_txFace(port);
    GrafPort_set_txFont(port, (int16_t)rd16(fmip + FMInput_family_off));
    GrafPort_set_txSize(port, (int16_t)rd16(fmip + FMInput_size_off));
    GrafPort_set_txFace(port, rd8(fmip + FMInput_face_off));
    strike s;
    bool ok = load_strike(port, &s);
    if (ok)
        fill_width_table(&s, port, (int16_t)rd16(fmip + FMInput_family_off), (int16_t)rd16(fmip + FMInput_size_off),
                         rd8(fmip + FMInput_face_off), rd16(fmip + FMInput_device_off),
                         rd32(fmip + FMInput_numer_off), rd32(fmip + FMInput_denom_off));
    GrafPort_set_txFont(port, f);
    GrafPort_set_txSize(port, sz);
    GrafPort_set_txFace(port, face);
    gmemset(fmoutput, 0, FMOutput_SIZEOF);
    if (ok) {
        wr32(fmoutput + FMOutput_fontHandle_off, s.handle);
        wr8(fmoutput + FMOutput_bold_off, (s.face & bold) ? 1 : 0);
        wr8(fmoutput + FMOutput_ulOffset_off, 1);
        wr8(fmoutput + FMOutput_ulThick_off, 1);
        wr8(fmoutput + FMOutput_extra_off, (uint8_t)style_extra(s.face));
        wr8(fmoutput + FMOutput_ascent_off, (uint8_t)s.ascent);
        wr8(fmoutput + FMOutput_descent_off, (uint8_t)s.descent);
        wr8(fmoutput + FMOutput_widMax_off, (uint8_t)s.widmax);
        wr8(fmoutput + FMOutput_leading_off, (uint8_t)s.leading);
    } else {
        wr16(fmoutput + FMOutput_errNum_off, (uint16_t)-1);
    }
    wr32(fmoutput + FMOutput_numer_off, 0x00010001);
    wr32(fmoutput + FMOutput_denom_off, 0x00010001);
    return fmoutput;
}

void tb_FontMetrics(gaddr metrp)
{
    strike s;
    load_strike(qd_port(), &s);
    wr32(metrp, (uint32_t)s.ascent << 16);
    wr32(metrp + 4, (uint32_t)s.descent << 16);
    wr32(metrp + 8, (uint32_t)s.leading << 16);
    wr32(metrp + 12, (uint32_t)s.widmax << 16);
    wr32(metrp + 16, 0);
}

/* MeasureText: the left edge of each character and the end, in charLocs
 * (count + 1 words). */
void tb_MeasureText(int16_t count, gaddr text, gaddr locs)
{
    int x = 0;
    wr16(locs, 0);
    for (int i = 0; i < count; i++) {
        x += tb_TextWidth(text, (int16_t)i, 1);
        wr16(locs + (uint32_t)(i + 1) * 2, (uint16_t)x);
    }
}

/* ---------------------------------------------------------------------- */
/* The outline font calls (FontDispatch).
 *
 * Blue Box draws only bitmap strikes: there is no TrueType rasteriser, and
 * the built-in families have no 'sfnt'. A family a program or the System
 * file brings with an 'sfnt' (a FOND entry of size 0) still has its
 * outline metrics in the font's own tables, so IsOutline answers as the
 * Font Manager would choose (the outline when outlines are preferred or no
 * strike has the exact size), and OutlineMetrics measures the glyphs from
 * hmtx and the glyph headers in glyf: unhinted, and without the
 * algorithmic styles. The text itself is still drawn from the nearest
 * strike. */

enum { fontNotOutlineErr = -32615 };

static bool outline_preferred, preserve_glyph;

typedef struct {
    gaddr base;
    uint32_t len;
    uint32_t head, hhea, hmtx, maxp, cmap, loca, glyf;
    uint32_t glyf_len;
    int units_per_em, long_loca, num_glyphs, num_hmetrics;
    uint32_t map; /* the Macintosh Roman (1, 0) cmap subtable */
    gaddr handle;
    int8_t state;
} sfnt;

static uint32_t sf16(const sfnt *f, uint32_t at) { return at + 2 <= f->len ? rd16(f->base + at) : 0; }
static uint32_t sf32(const sfnt *f, uint32_t at) { return at + 4 <= f->len ? rd32(f->base + at) : 0; }

/* The 'sfnt' of a family, by the size-0 entry of its FOND. */
static gaddr sfnt_resource(int family)
{
    gaddr fond = tb_GetResource((int32_t)fourcc("FOND"), (int16_t)family);
    if (!fond || !rd32(fond))
        return 0;
    gaddr assoc = rd32(fond) + FamRec_SIZEOF;
    int n = (int)rd16(assoc) + 1;
    for (int i = 0; i < n; i++) {
        gaddr e = assoc + 2 + (uint32_t)i * 6;
        if (rd16s(e) == 0) {
            gaddr h = tb_GetResource((int32_t)fourcc("sfnt"), rd16s(e + 4));
            if (h && rd32(h))
                return h;
        }
    }
    return 0;
}

/* Find the tables the metrics come from; false if any is missing or the
 * font is not one this can read (no Macintosh Roman cmap of format 0, 4 or
 * 6), and then the family is not counted as an outline font. */
static bool sfnt_open(gaddr h, sfnt *f)
{
    int8_t state = f->state;
    memset(f, 0, sizeof *f);
    f->handle = h;
    f->state = state;
    f->base = rd32(h);
    f->len = (uint32_t)tb_GetHandleSize(h);
    uint32_t version = sf32(f, 0);
    if (version != 0x00010000 && version != fourcc("true"))
        return false;
    int n = (int)sf16(f, 4);
    for (int i = 0; i < n; i++) {
        uint32_t e = 12 + (uint32_t)i * 16, tag = sf32(f, e), off = sf32(f, e + 8), len = sf32(f, e + 12);
        if (off > f->len || len > f->len - off)
            continue;
        if (tag == fourcc("head"))
            f->head = off;
        else if (tag == fourcc("hhea"))
            f->hhea = off;
        else if (tag == fourcc("hmtx"))
            f->hmtx = off;
        else if (tag == fourcc("maxp"))
            f->maxp = off;
        else if (tag == fourcc("cmap"))
            f->cmap = off;
        else if (tag == fourcc("loca"))
            f->loca = off;
        else if (tag == fourcc("glyf"))
            f->glyf = off, f->glyf_len = len;
    }
    if (!f->head || !f->hhea || !f->hmtx || !f->maxp || !f->cmap || !f->loca || !f->glyf)
        return false;
    f->units_per_em = (int)sf16(f, f->head + 18);
    f->long_loca = sf16(f, f->head + 50) != 0;
    f->num_glyphs = (int)sf16(f, f->maxp + 4);
    f->num_hmetrics = (int)sf16(f, f->hhea + 34);
    if (f->units_per_em == 0 || f->num_hmetrics == 0)
        return false;
    int subtables = (int)sf16(f, f->cmap + 2);
    for (int i = 0; i < subtables; i++) {
        uint32_t e = f->cmap + 4 + (uint32_t)i * 8;
        if (sf16(f, e) == 1 && sf16(f, e + 2) == 0) {
            uint32_t at = f->cmap + sf32(f, e + 4);
            uint32_t format = sf16(f, at);
            if (format == 0 || format == 4 || format == 6)
                f->map = at;
        }
    }
    return f->map != 0;
}

/* A Macintosh Roman character's glyph, 0 (the missing glyph) if none. */
static int sfnt_glyph(const sfnt *f, uint8_t c)
{
    uint32_t m = f->map;
    int g = 0;
    switch (sf16(f, m)) {
    case 0:
        g = m + 6 + c < f->len ? rd8(f->base + m + 6 + c) : 0;
        break;
    case 6: {
        uint32_t first = sf16(f, m + 6), count = sf16(f, m + 8);
        if (c >= first && c < first + count)
            g = (int)sf16(f, m + 10 + (c - first) * 2);
        break;
    }
    case 4: {
        uint32_t segs = sf16(f, m + 6) / 2;
        uint32_t ends = m + 14, starts = ends + segs * 2 + 2, deltas = starts + segs * 2, ranges = deltas + segs * 2;
        for (uint32_t i = 0; i < segs; i++) {
            if (c > sf16(f, ends + i * 2))
                continue;
            uint32_t start = sf16(f, starts + i * 2), range = sf16(f, ranges + i * 2);
            if (c < start)
                break;
            uint32_t delta = sf16(f, deltas + i * 2);
            if (range == 0)
                g = (int)((c + delta) & 0xFFFF);
            else if ((g = (int)sf16(f, ranges + i * 2 + range + (c - start) * 2)) != 0)
                g = (int)((g + delta) & 0xFFFF);
            break;
        }
        break;
    }
    }
    return g < f->num_glyphs ? g : 0;
}

/* A glyph's advance and left side bearing in font units (hmtx), and its
 * bounding box from the glyph's header in glyf; false for a glyph with no
 * outline (a space), whose box is empty. */
static bool sfnt_metrics(const sfnt *f, int g, int *aw, int *lsb, int box[4])
{
    int last = f->num_hmetrics - 1;
    *aw = (int)sf16(f, f->hmtx + (uint32_t)(g < last ? g : last) * 4);
    *lsb = g <= last ? (int16_t)sf16(f, f->hmtx + (uint32_t)g * 4 + 2)
                     : (int16_t)sf16(f, f->hmtx + (uint32_t)f->num_hmetrics * 4 + (uint32_t)(g - f->num_hmetrics) * 2);
    uint32_t from, to;
    if (f->long_loca) {
        from = sf32(f, f->loca + (uint32_t)g * 4);
        to = sf32(f, f->loca + (uint32_t)g * 4 + 4);
    } else {
        from = sf16(f, f->loca + (uint32_t)g * 2) * 2;
        to = sf16(f, f->loca + (uint32_t)g * 2 + 2) * 2;
    }
    if (to <= from || to > f->glyf_len) {
        box[0] = box[1] = box[2] = box[3] = 0;
        return false;
    }
    uint32_t at = f->glyf + from;
    for (int k = 0; k < 4; k++)
        box[k] = (int16_t)sf16(f, at + 2 + (uint32_t)k * 2); /* xMin, yMin, xMax, yMax */
    return true;
}

/* Whether the family has a plain bitmap strike of exactly this size. */
static bool has_strike(int family, int size)
{
    gaddr fond = tb_GetResource((int32_t)fourcc("FOND"), (int16_t)family);
    if (fond && rd32(fond)) {
        gaddr assoc = rd32(fond) + FamRec_SIZEOF;
        int n = (int)rd16(assoc) + 1;
        for (int i = 0; i < n; i++) {
            gaddr e = assoc + 2 + (uint32_t)i * 6;
            if (rd16s(e) == size && rd16s(e + 2) == 0)
                return true;
        }
    }
    return tb_GetResource((int32_t)fourcc("FONT"), (int16_t)(family * 128 + size)) != 0;
}

/* The current port's outline font, if the Font Manager would draw this
 * size from it: numer and denom (Points, vertical first) scale the size
 * as they scale the text. The 'sfnt' is left locked until outline_done. */
static bool outline_font(uint32_t numer, uint32_t denom, sfnt *f, int *hsize, int *vsize)
{
    int family, size, face;
    port_font(qd_port(), &family, &size, &face);
    int nv = pt_v(numer), nh = pt_h(numer), dv = pt_v(denom), dh = pt_h(denom);
    *vsize = dv > 0 && nv > 0 ? size * nv / dv : size;
    *hsize = dh > 0 && nh > 0 ? size * nh / dh : size;
    uint8_t saved = rd8(LM_ResLoad);
    uint16_t res_err = rd16(LM_ResErr); /* asking leaves ResErr alone, as measuring does */
    wr8(LM_ResLoad, 1);
    gaddr h = sfnt_resource(family);
    int k;
    if (!h && (k = classic_index(family)) >= 0 && classic_fonts[k].drawn_as != family) {
        family = classic_fonts[k].drawn_as;
        h = sfnt_resource(family);
    }
    bool bitmap = h && !outline_preferred && has_strike(family, *vsize);
    wr8(LM_ResLoad, saved);
    wr16(LM_ResErr, res_err);
    if (!h || bitmap)
        return false;
    f->state = tb_HGetState(h);
    tb_HLock(h);
    if (!sfnt_open(h, f)) {
        tb_HSetState(h, f->state);
        return false;
    }
    return true;
}

static void outline_done(sfnt *f) { tb_HSetState(f->handle, f->state); }

/* Font units to a Fixed number of pixels at this many pixels per em. */
static int32_t units_px(const sfnt *f, int units, int ppem)
{
    return (int32_t)((int64_t)units * ppem * 65536 / f->units_per_em);
}

static int px_floor(int32_t fixed) { return (int)(fixed >> 16); }
static int px_ceil(int32_t fixed) { return (int)((fixed + 0xFFFF) >> 16); }

int8_t tb_IsOutline(uint32_t numer, uint32_t denom)
{
    sfnt f;
    int hsize, vsize;
    if (!outline_font(numer, denom, &f, &hsize, &vsize))
        return false;
    outline_done(&f);
    return true;
}

void tb_SetOutlinePreferred(int8_t on) { outline_preferred = on != 0; }
int8_t tb_GetOutlinePreferred(void) { return outline_preferred; }
void tb_SetPreserveGlyph(int8_t on) { preserve_glyph = on != 0; }
int8_t tb_GetPreserveGlyph(void) { return preserve_glyph; }
int16_t tb_FlushFonts(void) { return noErr; }

/* Each glyph's advance (whole pixels unless FractEnable), left side
 * bearing (never rounded) and bounding box, the box in pixels about the
 * glyph's origin with y growing upwards as IM: Text describes it: so top
 * is the glyph's highest point and bottom its lowest. yMax and yMin are
 * the extremes over the whole text. */
int16_t tb_OutlineMetrics(int16_t byte_count, gaddr text, uint32_t numer, uint32_t denom, gaddr y_max, gaddr y_min,
                          gaddr aw_array, gaddr lsb_array, gaddr bounds_array)
{
    sfnt f;
    int hsize, vsize;
    if (!outline_font(numer, denom, &f, &hsize, &vsize))
        return fontNotOutlineErr;
    bool fract = rd8(LM_FractEnable) != 0;
    int top = 0, bottom = 0;
    bool any = false;
    for (int i = 0; i < byte_count; i++) {
        int aw, lsb, box[4];
        bool inked = sfnt_metrics(&f, sfnt_glyph(&f, rd8(text + (uint32_t)i)), &aw, &lsb, box);
        int32_t adv = units_px(&f, aw, hsize);
        if (!fract)
            adv = (adv + 0x8000) & ~0xFFFF;
        if (aw_array)
            wr32(aw_array + (uint32_t)i * 4, (uint32_t)adv);
        if (lsb_array)
            wr32(lsb_array + (uint32_t)i * 4, (uint32_t)units_px(&f, lsb, hsize));
        qrect r = { 0, 0, 0, 0 };
        if (inked) {
            r.left = (int16_t)px_floor(units_px(&f, box[0], hsize));
            r.bottom = (int16_t)px_floor(units_px(&f, box[1], vsize));
            r.right = (int16_t)px_ceil(units_px(&f, box[2], hsize));
            r.top = (int16_t)px_ceil(units_px(&f, box[3], vsize));
            top = any && top > r.top ? top : r.top;
            bottom = any && bottom < r.bottom ? bottom : r.bottom;
            any = true;
        }
        if (bounds_array)
            qrect_put(bounds_array + (uint32_t)i * 8, r);
    }
    if (y_max)
        wr16(y_max, (uint16_t)top);
    if (y_min)
        wr16(y_min, (uint16_t)bottom);
    outline_done(&f);
    return noErr;
}
