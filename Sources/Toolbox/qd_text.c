/*
 * Font Manager and QuickDraw text: bitmap fonts (FONT/NFNT, found through
 * FOND families) drawn with the port's text mode, style and clipping.
 */
#include <stdlib.h>
#include <string.h>

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

/* The font resource ID for a family and size, through its FOND if any. */
static gaddr font_resource(int family, int size)
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

int qd_text_width(gaddr port, const uint8_t *text, int len)
{
    strike s;
    load_strike(port, &s);
    int w = 0;
    for (int i = 0; i < len; i++)
        w += char_advance(&s, text[i], port);
    return w;
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

void qd_draw_text(gaddr port, const uint8_t *text, int len)
{
    strike s;
    bool have = load_strike(port, &s);
    uint32_t loc = rd32(port + GrafPort_pnLoc_off);
    int x = pt_h(loc), base = pt_v(loc);
    int start = x;
    if (have) {
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
            draw_glyph(&s, idx, x + off, base - s.ascent, &d, &clip, mode, fg, bk);
            x += char_advance(&s, text[i], port);
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
void tb_CharExtra(int32_t extra) {}

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
    }
    wr8(LM_ResLoad, saved);
    tb_DisposePtr(id);
    tb_DisposePtr(type);
}

int8_t tb_RealFont(int16_t fnum, int16_t sz)
{
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
