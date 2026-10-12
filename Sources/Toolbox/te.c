/*
 * TextEdit (unstyled).
 *
 * The TERec holds the text handle, rectangles, selection and the line-start
 * table, which grows with the record.  Drawing goes through QuickDraw in the
 * record's port with its font.
 */
#include <stdlib.h>
#include <string.h>

#include "qd_internal.h"
#include "tb_internal.h"

enum { patXor = 10 };
enum { MAX_LINES_GROW = 16 };

bool ev_still_down(void);
uint32_t ev_mouse_global(void);
void ev_wait_tick(void);

static gaddr te(gaddr teh) { return rd32(teh); }
static int16_t te16(gaddr teh, uint32_t off) { return (int16_t)rd16(te(teh) + off); }
static void set16(gaddr teh, uint32_t off, int16_t v) { wr16(te(teh) + off, (uint16_t)v); }
static int text_len(gaddr teh) { return te16(teh, TERec_teLength_off); }
static gaddr text_ptr(gaddr teh) { return rd32(TERec_hText(te(teh))); }
static int nlines(gaddr teh) { return te16(teh, TERec_nLines_off); }
static int line_start(gaddr teh, int i) { return te16(teh, TERec_lineStarts_off + (uint32_t)i * 2); }

/* Run with the TE's port and font current; restores afterwards. */
typedef struct {
    gaddr port;
    int16_t font, size;
    uint8_t face;
    int16_t mode;
} port_state;

static port_state enter(gaddr teh)
{
    port_state s = { qd_port(), 0, 0, 0, 0 };
    gaddr p = TERec_inPort(te(teh));
    tb_SetPort(p);
    s.font = GrafPort_txFont(p);
    s.size = GrafPort_txSize(p);
    s.face = GrafPort_txFace(p);
    s.mode = GrafPort_txMode(p);
    GrafPort_set_txFont(p, te16(teh, TERec_txFont_off));
    GrafPort_set_txSize(p, te16(teh, TERec_txSize_off));
    GrafPort_set_txFace(p, rd8(te(teh) + TERec_txFace_off));
    GrafPort_set_txMode(p, 1);
    return s;
}

static void leave(gaddr teh, port_state s)
{
    gaddr p = TERec_inPort(te(teh));
    GrafPort_set_txFont(p, s.font);
    GrafPort_set_txSize(p, s.size);
    GrafPort_set_txFace(p, s.face);
    GrafPort_set_txMode(p, s.mode);
    tb_SetPort(s.port);
}

static int width_of(gaddr teh, int from, int n)
{
    if (n <= 0)
        return 0;
    uint8_t *buf = malloc((size_t)n);
    gget(buf, text_ptr(teh) + (uint32_t)from, (uint32_t)n);
    int w = qd_text_width(qd_port(), buf, n);
    free(buf);
    return w;
}

/* ---------------------------------------------------------------------- */
/* Line breaking */

static void set_line_count(gaddr teh, int n)
{
    int32_t need = (int32_t)(TERec_lineStarts_off + (uint32_t)(n + 1) * 2);
    if (tb_GetHandleSize(teh) < need)
        tb_SetHandleSize(teh, need + MAX_LINES_GROW * 2);
    set16(teh, TERec_nLines_off, (int16_t)n);
}

void tb_TECalText(gaddr teh)
{
    port_state s = enter(teh);
    int len = text_len(teh);
    qrect dest = qrect_get(te(teh) + TERec_destRect_off);
    int width = dest.right - dest.left;
    bool cr_only = te16(teh, TERec_crOnly_off) < 0;
    uint8_t *t = malloc(len ? (size_t)len : 1);
    gget(t, text_ptr(teh), (uint32_t)len);
    int *starts = malloc(((size_t)len + 2) * sizeof *starts);
    int n = 0, pos = 0;
    starts[n++] = 0;
    while (pos < len) {
        int lineb = pos, last_break = -1, w = 0, i = pos;
        while (i < len && t[i] != '\r') {
            int cw = qd_text_width(qd_port(), t + i, 1);
            if (!cr_only && w + cw > width && i > lineb)
                break;
            w += cw;
            if (t[i] == ' ' || t[i] == '\t')
                last_break = i + 1;
            i++;
        }
        if (i < len && t[i] == '\r') {
            pos = i + 1;
        } else if (i < len) { /* wrapped */
            pos = last_break > lineb ? last_break : i;
        } else {
            pos = len;
        }
        if (pos < len || (len && t[len - 1] == '\r' && pos == len))
            starts[n++] = pos;
        else
            break;
    }
    /* A trailing return starts an empty last line. */
    int lines = n;
    if (!(len && t[len - 1] == '\r') && n > 0 && starts[n - 1] == len && len > 0)
        lines = n - 1;
    if (len == 0)
        lines = 1;
    set_line_count(teh, lines);
    for (int i = 0; i < n && i <= lines; i++)
        set16(teh, TERec_lineStarts_off + (uint32_t)i * 2, (int16_t)starts[i]);
    set16(teh, TERec_lineStarts_off + (uint32_t)lines * 2, (int16_t)len);
    free(starts);
    free(t);
    leave(teh, s);
}

static int line_of(gaddr teh, int offset)
{
    int n = nlines(teh);
    for (int i = 0; i < n; i++)
        if (offset < line_start(teh, i + 1))
            return i;
    return n > 0 ? n - 1 : 0;
}

/* Point (local) of a text offset: left edge of the character, top of line. */
static void offset_point(gaddr teh, int offset, int *h, int *v)
{
    qrect dest = qrect_get(te(teh) + TERec_destRect_off);
    int line = line_of(teh, offset);
    int ls = line_start(teh, line);
    int len = text_len(teh);
    /* After a final return the caret sits on the next line. */
    if (offset == len && len > 0 && rd8(text_ptr(teh) + (uint32_t)len - 1) == '\r') {
        line = nlines(teh) - 1;
        ls = line_start(teh, line);
        if (ls < offset) {
            line++;
            ls = offset;
        }
    }
    *h = dest.left + width_of(teh, ls, offset - ls);
    *v = dest.top + line * te16(teh, TERec_lineHeight_off);
}

int16_t tb_TEGetOffset(uint32_t pt, gaddr teh)
{
    port_state s = enter(teh);
    qrect dest = qrect_get(te(teh) + TERec_destRect_off);
    int lh = te16(teh, TERec_lineHeight_off);
    int line = (pt_v(pt) - dest.top) / (lh ? lh : 16);
    int n = nlines(teh);
    int len = text_len(teh);
    int result;
    if (pt_v(pt) < dest.top)
        line = 0;
    if (line >= n) {
        result = len;
    } else {
        int ls = line_start(teh, line), le = line_start(teh, line + 1);
        if (le > ls && rd8(text_ptr(teh) + (uint32_t)le - 1) == '\r')
            le--;
        int x = dest.left;
        result = le;
        for (int i = ls; i < le; i++) {
            int cw = width_of(teh, i, 1);
            if (pt_h(pt) < x + cw / 2) {
                result = i;
                break;
            }
            x += cw;
        }
    }
    leave(teh, s);
    return (int16_t)result;
}

int32_t tb_TEGetPoint(int16_t offset, gaddr teh)
{
    port_state s = enter(teh);
    int h, v;
    offset_point(teh, offset, &h, &v);
    leave(teh, s);
    return (int32_t)make_pt((int16_t)(v + te16(teh, TERec_fontAscent_off)), (int16_t)h);
}

/* ---------------------------------------------------------------------- */
/* Drawing */

static void clip_to_view(gaddr teh, gaddr *saved_clip)
{
    gaddr port = qd_port();
    *saved_clip = tb_NewRgn();
    tb_GetClip(*saved_clip);
    gaddr view = tb_NewRgn();
    tb_RectRgn(view, te(teh) + TERec_viewRect_off);
    tb_SectRgn(view, GrafPort_clipRgn(port), GrafPort_clipRgn(port));
    tb_DisposeRgn(view);
}

static void restore_clip(gaddr saved_clip)
{
    tb_SetClip(saved_clip);
    tb_DisposeRgn(saved_clip);
}

/* Invert the selection range [a, b). */
static void invert_range(gaddr teh, int a, int b)
{
    if (a > b) {
        int t = a;
        a = b;
        b = t;
    }
    if (a == b)
        return;
    qrect dest = qrect_get(te(teh) + TERec_destRect_off);
    int lh = te16(teh, TERec_lineHeight_off);
    int la = line_of(teh, a), lb = line_of(teh, b);
    hrgn r, tmp;
    hrgn_init(&r);
    hrgn_init(&tmp);
    for (int line = la; line <= lb; line++) {
        int ls = line_start(teh, line), le = line_start(teh, line + 1);
        int s = a > ls ? a : ls, e = b < le ? b : le;
        int x0 = dest.left + width_of(teh, ls, s - ls);
        int x1 = (b > le || (e == le && line < lb)) ? dest.right : dest.left + width_of(teh, ls, e - ls);
        int top = dest.top + line * lh;
        hrgn_rect(&tmp, (qrect){ (int16_t)top, (int16_t)x0, (int16_t)(top + lh), (int16_t)x1 });
        hrgn_op(&r, &r, &tmp, RGN_UNION);
    }
    qd_fill_region(qd_port(), &r, qd_pat_black, patXor);
    hrgn_free(&r);
    hrgn_free(&tmp);
}

static void draw_caret(gaddr teh)
{
    int h, v;
    offset_point(teh, te16(teh, TERec_selStart_off), &h, &v);
    hrgn r;
    hrgn_init(&r);
    hrgn_rect(&r, (qrect){ (int16_t)v, (int16_t)(h - 1), (int16_t)(v + te16(teh, TERec_lineHeight_off)), (int16_t)h });
    qd_fill_region(qd_port(), &r, qd_pat_black, patXor);
    hrgn_free(&r);
}

/* Show or hide the selection/caret (it is drawn by inversion). */
static void toggle_hilite(gaddr teh)
{
    int a = te16(teh, TERec_selStart_off), b = te16(teh, TERec_selEnd_off);
    if (a == b)
        draw_caret(teh);
    else
        invert_range(teh, a, b);
}

static bool hilite_shown(gaddr teh)
{
    if (!te16(teh, TERec_active_off))
        return false;
    int a = te16(teh, TERec_selStart_off), b = te16(teh, TERec_selEnd_off);
    return a != b || rd8(te(teh) + TERec_caretState_off) == 0xFF;
}

static void hide(gaddr teh, bool *was)
{
    *was = hilite_shown(teh);
    if (*was)
        toggle_hilite(teh);
}

static void show(gaddr teh, bool was)
{
    if (was)
        toggle_hilite(teh);
}

static void draw_lines(gaddr teh, qrect area)
{
    qrect dest = qrect_get(te(teh) + TERec_destRect_off);
    int lh = te16(teh, TERec_lineHeight_off), asc = te16(teh, TERec_fontAscent_off);
    int n = nlines(teh);
    tb_EraseRect(te(teh) + TERec_viewRect_off);
    for (int i = 0; i < n; i++) {
        int top = dest.top + i * lh;
        if (top + lh <= area.top || top >= area.bottom)
            continue;
        int ls = line_start(teh, i), le = line_start(teh, i + 1);
        if (le > ls && rd8(text_ptr(teh) + (uint32_t)le - 1) == '\r')
            le--;
        tb_MoveTo(dest.left, (int16_t)(top + asc));
        int len = le - ls;
        if (len > 0) {
            uint8_t *buf = malloc((size_t)len);
            gget(buf, text_ptr(teh) + (uint32_t)ls, (uint32_t)len);
            qd_draw_text(qd_port(), buf, len);
            free(buf);
        }
    }
}

void tb_TEUpdate(gaddr r, gaddr teh)
{
    port_state s = enter(teh);
    gaddr clip;
    clip_to_view(teh, &clip);
    draw_lines(teh, qrect_get(r));
    if (hilite_shown(teh))
        toggle_hilite(teh);
    restore_clip(clip);
    leave(teh, s);
}

/* Redraw everything visible (after edits). */
static void redraw(gaddr teh)
{
    gaddr r = tb_NewPtr(8, false, false);
    gmemcpy(r, te(teh) + TERec_viewRect_off, 8);
    tb_TEUpdate(r, teh);
    tb_DisposePtr(r);
}

void tb_TEIdle(gaddr teh)
{
    if (!te16(teh, TERec_active_off) || te16(teh, TERec_selStart_off) != te16(teh, TERec_selEnd_off))
        return;
    uint32_t now = rd32(LM_Ticks);
    if (now - (uint32_t)TERec_caretTime(te(teh)) < rd32(LM_CaretTime))
        return;
    port_state s = enter(teh);
    gaddr clip;
    clip_to_view(teh, &clip);
    draw_caret(teh);
    restore_clip(clip);
    leave(teh, s);
    wr8(te(teh) + TERec_caretState_off, rd8(te(teh) + TERec_caretState_off) == 0xFF ? 0 : 0xFF);
    TERec_set_caretTime(te(teh), (int32_t)now);
}

/* ---------------------------------------------------------------------- */
/* Creating */

static void set_font_metrics(gaddr teh)
{
    gaddr info = tb_NewPtr(8, false, false);
    port_state s = enter(teh);
    tb_GetFontInfo(info);
    leave(teh, s);
    int asc = (int16_t)rd16(info), desc = (int16_t)rd16(info + 2), lead = (int16_t)rd16(info + 6);
    set16(teh, TERec_lineHeight_off, (int16_t)(asc + desc + lead));
    set16(teh, TERec_fontAscent_off, (int16_t)asc);
    tb_DisposePtr(info);
}

gaddr tb_TENew(gaddr dst, gaddr view)
{
    gaddr teh = tb_NewHandle((int32_t)(TERec_lineStarts_off + MAX_LINES_GROW * 2), false, true);
    gaddr t = te(teh);
    gaddr port = qd_port();
    gmemcpy(t + TERec_destRect_off, dst, 8);
    gmemcpy(t + TERec_viewRect_off, view, 8);
    TERec_set_hText(t, tb_NewHandle(0, false, false));
    t = te(teh);
    TERec_set_inPort(t, port);
    wr16(t + TERec_txFont_off, (uint16_t)GrafPort_txFont(port));
    wr8(t + TERec_txFace_off, GrafPort_txFace(port));
    wr16(t + TERec_txMode_off, (uint16_t)GrafPort_txMode(port));
    wr16(t + TERec_txSize_off, (uint16_t)GrafPort_txSize(port));
    wr8(t + TERec_caretState_off, 0);
    set_font_metrics(teh);
    tb_TECalText(teh);
    return teh;
}

gaddr tb_TEStyleNew(gaddr dst, gaddr view) { return tb_TENew(dst, view); }

void tb_TEDispose(gaddr teh)
{
    tb_DisposeHandle(TERec_hText(te(teh)));
    tb_DisposeHandle(teh);
}

void tb_TESetText(gaddr p, int32_t ln, gaddr teh)
{
    gaddr h = TERec_hText(te(teh));
    tb_SetHandleSize(h, ln);
    gmemcpy(rd32(h), p, (uint32_t)ln);
    set16(teh, TERec_teLength_off, (int16_t)ln);
    set16(teh, TERec_selStart_off, (int16_t)ln);
    set16(teh, TERec_selEnd_off, (int16_t)ln);
    tb_TECalText(teh);
}

gaddr tb_TEGetText(gaddr teh) { return TERec_hText(te(teh)); }

/* ---------------------------------------------------------------------- */
/* Selection and activation */

void tb_TESetSelect(int32_t start, int32_t stop, gaddr teh)
{
    int len = text_len(teh);
    if (start < 0)
        start = 0;
    if (stop < 0)
        stop = 0;
    if (start > len)
        start = len;
    if (stop > len)
        stop = len;
    if (start > stop) {
        int32_t t = start;
        start = stop;
        stop = t;
    }
    port_state s = enter(teh);
    gaddr clip;
    clip_to_view(teh, &clip);
    bool was;
    hide(teh, &was);
    set16(teh, TERec_selStart_off, (int16_t)start);
    set16(teh, TERec_selEnd_off, (int16_t)stop);
    if (te16(teh, TERec_active_off)) {
        wr8(te(teh) + TERec_caretState_off, 0xFF);
        toggle_hilite(teh);
    }
    restore_clip(clip);
    leave(teh, s);
}

void tb_TEActivate(gaddr teh)
{
    if (te16(teh, TERec_active_off))
        return;
    set16(teh, TERec_active_off, 1);
    wr8(te(teh) + TERec_caretState_off, 0xFF);
    port_state s = enter(teh);
    gaddr clip;
    clip_to_view(teh, &clip);
    toggle_hilite(teh);
    restore_clip(clip);
    leave(teh, s);
}

void tb_TEDeactivate(gaddr teh)
{
    if (!te16(teh, TERec_active_off))
        return;
    port_state s = enter(teh);
    gaddr clip;
    clip_to_view(teh, &clip);
    if (hilite_shown(teh))
        toggle_hilite(teh);
    restore_clip(clip);
    leave(teh, s);
    set16(teh, TERec_active_off, 0);
    wr8(te(teh) + TERec_caretState_off, 0);
}

/* ---------------------------------------------------------------------- */
/* Editing */

static void replace(gaddr teh, const uint8_t *bytes, int n)
{
    int a = te16(teh, TERec_selStart_off), b = te16(teh, TERec_selEnd_off);
    int len = text_len(teh);
    gaddr h = TERec_hText(te(teh));
    int newlen = len - (b - a) + n;
    if (newlen > 32767)
        return; /* TextEdit's limit */
    if (n > b - a)
        tb_SetHandleSize(h, newlen);
    gaddr p = rd32(h);
    gmemcpy(p + (uint32_t)(a + n), p + (uint32_t)b, (uint32_t)(len - b));
    if (n)
        gput(p + (uint32_t)a, bytes, (uint32_t)n);
    if (n < b - a)
        tb_SetHandleSize(h, newlen);
    set16(teh, TERec_teLength_off, (int16_t)newlen);
    set16(teh, TERec_selStart_off, (int16_t)(a + n));
    set16(teh, TERec_selEnd_off, (int16_t)(a + n));
    tb_TECalText(teh);
}

static void edit(gaddr teh, const uint8_t *bytes, int n)
{
    replace(teh, bytes, n);
    wr8(te(teh) + TERec_caretState_off, 0xFF);
    redraw(teh);
    tb_TESelView(teh);
}

void tb_TEKey(int16_t thec, gaddr teh)
{
    uint8_t c = (uint8_t)thec;
    int a = te16(teh, TERec_selStart_off), b = te16(teh, TERec_selEnd_off);
    int len = text_len(teh);
    switch (c) {
    case 8: /* backspace */
        if (a == b) {
            if (a == 0)
                return;
            set16(teh, TERec_selStart_off, (int16_t)(a - 1));
        }
        edit(teh, NULL, 0);
        return;
    case 28: /* left */
        tb_TESetSelect(a == b ? a - 1 : a, a == b ? a - 1 : a, teh);
        return;
    case 29: /* right */
        tb_TESetSelect(a == b ? b + 1 : b, a == b ? b + 1 : b, teh);
        return;
    case 30: /* up */
    case 31: /* down */ {
        port_state s = enter(teh);
        int h, v;
        offset_point(teh, a, &h, &v);
        leave(teh, s);
        int lh = te16(teh, TERec_lineHeight_off);
        int16_t off = tb_TEGetOffset(make_pt((int16_t)(v + (c == 30 ? -lh : lh) + 1), (int16_t)h), teh);
        if (c == 30 && v <= qrect_get(te(teh) + TERec_destRect_off).top)
            off = 0;
        tb_TESetSelect(off, off, teh);
        return;
    }
    default:
        if (len - (b - a) >= 32767)
            return;
        edit(teh, &c, 1);
    }
}

void tb_TEInsert(gaddr p, int32_t ln, gaddr teh)
{
    uint8_t *buf = malloc(ln > 0 ? (size_t)ln : 1);
    gget(buf, p, (uint32_t)(ln > 0 ? ln : 0));
    int a = te16(teh, TERec_selStart_off), b = te16(teh, TERec_selEnd_off);
    set16(teh, TERec_selEnd_off, (int16_t)a); /* insert before the selection */
    edit(teh, buf, ln > 0 ? ln : 0);
    set16(teh, TERec_selStart_off, (int16_t)(b + ln));
    set16(teh, TERec_selEnd_off, (int16_t)(b + ln));
    free(buf);
}

void tb_TEDelete(gaddr teh)
{
    if (te16(teh, TERec_selStart_off) != te16(teh, TERec_selEnd_off))
        edit(teh, NULL, 0);
}

/* The TextEdit scrap lives in low memory. */
static gaddr scrap_handle(void)
{
    gaddr h = rd32(LM_TEScrpHandle);
    if (!h) {
        h = tb_NewHandle(0, true, false);
        wr32(LM_TEScrpHandle, h);
    }
    return h;
}

void tb_TECopy(gaddr teh)
{
    int a = te16(teh, TERec_selStart_off), b = te16(teh, TERec_selEnd_off);
    gaddr h = scrap_handle();
    tb_SetHandleSize(h, b - a);
    gmemcpy(rd32(h), text_ptr(teh) + (uint32_t)a, (uint32_t)(b - a));
    wr16(LM_TEScrpLength, (uint16_t)(b - a));
}

void tb_TECut(gaddr teh)
{
    tb_TECopy(teh);
    tb_TEDelete(teh);
}

void tb_TEPaste(gaddr teh)
{
    gaddr h = scrap_handle();
    int n = (int16_t)rd16(LM_TEScrpLength);
    uint8_t *buf = malloc(n > 0 ? (size_t)n : 1);
    gget(buf, rd32(h), (uint32_t)(n > 0 ? n : 0));
    edit(teh, buf, n > 0 ? n : 0);
    free(buf);
}

gaddr tb_TEScrapHandle(void) { return scrap_handle(); }
int32_t tb_TEGetScrapLength(void) { return (int16_t)rd16(LM_TEScrpLength); }
int16_t tb_TEFromScrap(void) { return noErr; }
int16_t tb_TEToScrap(void) { return noErr; }

/* ---------------------------------------------------------------------- */
/* Clicking */

void tb_TEClick(uint32_t p, int8_t ext, gaddr teh)
{
    int16_t anchor = ext ? te16(teh, TERec_selStart_off) : tb_TEGetOffset(p, teh);
    int16_t off = tb_TEGetOffset(p, teh);
    /* Double-click selects a word. */
    uint32_t now = rd32(LM_Ticks);
    bool dbl = !ext && now - (uint32_t)TERec_clickTime(te(teh)) < rd32(LM_DoubleTime) &&
               te16(teh, TERec_clikStuff_off) == off;
    TERec_set_clickTime(te(teh), (int32_t)now);
    set16(teh, TERec_clikStuff_off, off);
    if (dbl) {
        gaddr t = text_ptr(teh);
        int len = text_len(teh), a = off, b = off;
        while (a > 0 && rd8(t + (uint32_t)a - 1) > ' ')
            a--;
        while (b < len && rd8(t + (uint32_t)b) > ' ')
            b++;
        tb_TESetSelect(a, b, teh);
        return;
    }
    tb_TESetSelect(anchor < off ? anchor : off, anchor < off ? off : anchor, teh);
    while (ev_still_down()) {
        uint32_t g = ev_mouse_global();
        gaddr port = TERec_inPort(te(teh));
        pixdst d = qd_port_pixels(port);
        uint32_t local = make_pt((int16_t)(pt_v(g) + d.bounds.top), (int16_t)(pt_h(g) + d.bounds.left));
        int16_t now_off = tb_TEGetOffset(local, teh);
        if (now_off != off) {
            off = now_off;
            tb_TESetSelect(anchor < off ? anchor : off, anchor < off ? off : anchor, teh);
        }
        ev_wait_tick();
    }
}

/* ---------------------------------------------------------------------- */
/* Scrolling */

void tb_TEScroll(int16_t dh, int16_t dv, gaddr teh)
{
    if (!dh && !dv)
        return;
    gaddr t = te(teh);
    qrect d = qrect_get(t + TERec_destRect_off);
    d.left = (int16_t)(d.left + dh);
    d.right = (int16_t)(d.right + dh);
    d.top = (int16_t)(d.top + dv);
    d.bottom = (int16_t)(d.bottom + dv);
    qrect_put(t + TERec_destRect_off, d);
    redraw(teh);
}

void tb_TEPinScroll(int16_t dh, int16_t dv, gaddr teh)
{
    /* Do not scroll past the last line. */
    qrect d = qrect_get(te(teh) + TERec_destRect_off), v = qrect_get(te(teh) + TERec_viewRect_off);
    int lh = te16(teh, TERec_lineHeight_off);
    int bottom = d.top + nlines(teh) * lh;
    if (dv < 0 && bottom + dv < v.bottom)
        dv = (int16_t)(v.bottom - bottom < 0 ? v.bottom - bottom : 0);
    if (dv > 0 && d.top + dv > v.top)
        dv = (int16_t)(v.top - d.top > 0 ? v.top - d.top : 0);
    tb_TEScroll(dh, dv, teh);
}

static bool auto_view;

void tb_TEAutoView(int8_t autoflag, gaddr teh) { auto_view = autoflag != 0; }

void tb_TESelView(gaddr teh)
{
    if (!auto_view)
        return;
    port_state s = enter(teh);
    int h, v;
    offset_point(teh, te16(teh, TERec_selStart_off), &h, &v);
    leave(teh, s);
    qrect view = qrect_get(te(teh) + TERec_viewRect_off);
    int lh = te16(teh, TERec_lineHeight_off);
    if (v < view.top)
        tb_TEScroll(0, (int16_t)(view.top - v), teh);
    else if (v + lh > view.bottom)
        tb_TEScroll(0, (int16_t)(view.bottom - v - lh), teh);
}

void tb_TETextBox(gaddr p, int32_t ln, gaddr r, int16_t j)
{
    gaddr teh = tb_TENew(r, r);
    tb_TESetText(p, ln, teh);
    tb_EraseRect(r);
    tb_TEUpdate(r, teh);
    tb_TEDispose(teh);
}

int16_t tb_TEFeatureFlag(int16_t feature, int16_t action, gaddr t) { return 0; }

void tb_TEInit(void)
{
    scrap_handle();
    wr16(LM_TEScrpLength, 0);
}
