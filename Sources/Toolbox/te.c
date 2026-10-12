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
    /* Apps may replace the text by resizing hText; its size is the length. */
    int32_t hsize = tb_GetHandleSize(TERec_hText(te(teh)));
    set16(teh, TERec_teLength_off, (int16_t)(hsize > 32767 ? 32767 : hsize));
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

/* Left edge of a line, by the record's justification (teJustCenter 1,
 * teJustRight -1; left otherwise).  The line's return isn't counted. */
static int line_x(gaddr teh, int line)
{
    qrect dest = qrect_get(te(teh) + TERec_destRect_off);
    int just = te16(teh, TERec_just_off);
    if (just != 1 && just != -1)
        return dest.left;
    int ls = line_start(teh, line), le = line_start(teh, line + 1);
    if (le > ls && rd8(text_ptr(teh) + (uint32_t)le - 1) == '\r')
        le--;
    int room = (dest.right - dest.left) - width_of(teh, ls, le - ls);
    return dest.left + (just == 1 ? room / 2 : room);
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
    *h = (line < nlines(teh) ? line_x(teh, line) : dest.left) + width_of(teh, ls, offset - ls);
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
        int x = line_x(teh, line);
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
        int lx = line_x(teh, line);
        int x0 = lx + width_of(teh, ls, s - ls);
        int x1 = (b > le || (e == le && line < lb)) ? dest.right : lx + width_of(teh, ls, e - ls);
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
        tb_MoveTo((int16_t)line_x(teh, i), (int16_t)(top + asc));
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
    /* caretTime is when the caret next blinks; apps sleep until then. */
    uint32_t now = rd32(LM_Ticks);
    if ((int32_t)(now - (uint32_t)TERec_caretTime(te(teh))) < 0)
        return;
    port_state s = enter(teh);
    gaddr clip;
    clip_to_view(teh, &clip);
    draw_caret(teh);
    restore_clip(clip);
    leave(teh, s);
    wr8(te(teh) + TERec_caretState_off, rd8(te(teh) + TERec_caretState_off) == 0xFF ? 0 : 0xFF);
    TERec_set_caretTime(te(teh), (int32_t)(now + rd32(LM_CaretTime)));
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
/* Between the TextEdit scrap and the desk scrap (apps do this on resume
 * and suspend). */
int16_t tb_TEFromScrap(void)
{
    gaddr h = scrap_handle();
    gaddr off = tb_NewPtr(4, false, true);
    int32_t n = tb_GetScrap(h, (int32_t)fourcc("TEXT"), off);
    tb_DisposePtr(off);
    if (n < 0) {
        wr16(LM_TEScrpLength, 0);
        return (int16_t)n;
    }
    wr16(LM_TEScrpLength, (uint16_t)(n > 32767 ? 32767 : n));
    return noErr;
}

int16_t tb_TEToScrap(void)
{
    gaddr h = scrap_handle();
    int32_t n = (int16_t)rd16(LM_TEScrpLength);
    tb_ZeroScrap();
    tb_HLock(h);
    int32_t err = tb_PutScrap(n > 0 ? n : 0, (int32_t)fourcc("TEXT"), rd32(h));
    tb_HUnlock(h);
    return (int16_t)err;
}

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

/* As in the ROM, the text isn't redrawn: the caller updates it. */
void tb_TESetAlignment(int16_t j, gaddr teh) { set16(teh, TERec_just_off, j); }

void tb_TETextBox(gaddr p, int32_t ln, gaddr r, int16_t j)
{
    gaddr teh = tb_TENew(r, r);
    set16(teh, TERec_just_off, j);
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

/* ---------------------------------------------------------------------- */
/* Styled TextEdit, with one style for the whole record for now: style
 * calls change the record's font, face and size; queries report it. */

enum { doFont = 1, doFace = 2, doSize = 4, doColor = 8, addFace = 16, addSize = 32, doToggle = 64 };

static void put_style(gaddr teh, gaddr ts)
{
    wr16(ts + TextStyle_tsFont_off, (uint16_t)te16(teh, TERec_txFont_off));
    wr8(ts + TextStyle_tsFace_off, rd8(te(teh) + TERec_txFace_off));
    wr8(ts + TextStyle_filler_off, 0);
    wr16(ts + TextStyle_tsSize_off, (uint16_t)te16(teh, TERec_txSize_off));
    gmemset(ts + TextStyle_tsColor_off, 0, 6); /* black */
}

static void redraw_view(gaddr teh)
{
    gaddr r = tb_NewPtr(8, false, false);
    gmemcpy(r, te(teh) + TERec_viewRect_off, 8);
    port_state s = enter(teh);
    tb_EraseRect(r);
    leave(teh, s);
    tb_TEUpdate(r, teh);
    tb_DisposePtr(r);
}

static void apply_style(gaddr teh, int mode, gaddr ns, bool draw)
{
    if (mode & doFont)
        set16(teh, TERec_txFont_off, (int16_t)rd16(ns + TextStyle_tsFont_off));
    if (mode & (doFace | addFace | doToggle)) {
        uint8_t cur = rd8(te(teh) + TERec_txFace_off), f = rd8(ns + TextStyle_tsFace_off);
        uint8_t v = (mode & doToggle) ? (uint8_t)(cur ^ f) : (mode & addFace) ? (uint8_t)(cur | f) : f;
        wr8(te(teh) + TERec_txFace_off, v);
    }
    if (mode & doSize)
        set16(teh, TERec_txSize_off, (int16_t)rd16(ns + TextStyle_tsSize_off));
    if (mode & addSize)
        set16(teh, TERec_txSize_off, (int16_t)(te16(teh, TERec_txSize_off) + (int16_t)rd16(ns + TextStyle_tsSize_off)));
    set_font_metrics(teh);
    tb_TECalText(teh);
    if (draw)
        redraw_view(teh);
}

void tb_TESetStyle(int16_t mode, gaddr newStyle, int8_t redraw_now, gaddr teh) { apply_style(teh, mode, newStyle, redraw_now); }

void tb_TEReplaceStyle(int16_t mode, gaddr oldStyle, gaddr newStyle, int8_t redraw_now, gaddr teh)
{
    gaddr cur = tb_NewPtr(TextStyle_SIZEOF, false, true);
    put_style(teh, cur);
    bool match = (!(mode & doFont) || rd16(cur) == rd16(oldStyle + TextStyle_tsFont_off)) &&
                 (!(mode & doSize) || rd16(cur + 4) == rd16(oldStyle + TextStyle_tsSize_off)) &&
                 (!(mode & doFace) || rd8(cur + 2) == rd8(oldStyle + TextStyle_tsFace_off));
    tb_DisposePtr(cur);
    if (match)
        apply_style(teh, mode, newStyle, redraw_now);
}

void tb_TEGetStyle(int16_t offset, gaddr theStyle, gaddr lineHeight, gaddr fontAscent, gaddr teh)
{
    if (theStyle)
        put_style(teh, theStyle);
    if (lineHeight)
        wr16(lineHeight, (uint16_t)te16(teh, TERec_lineHeight_off));
    if (fontAscent)
        wr16(fontAscent, (uint16_t)te16(teh, TERec_fontAscent_off));
}

int8_t tb_TEContinuousStyle(gaddr modep, gaddr thestyle, gaddr teh)
{
    if (thestyle)
        put_style(teh, thestyle);
    return 1; /* one style throughout */
}

int32_t tb_TEGetHeight(int32_t endLine, int32_t startLine, gaddr teh)
{
    if (endLine < startLine) {
        int32_t t = endLine;
        endLine = startLine;
        startLine = t;
    }
    int n = nlines(teh);
    if (endLine > n)
        endLine = n;
    if (startLine < 1)
        startLine = 1;
    return endLine >= startLine ? (endLine - startLine + 1) * te16(teh, TERec_lineHeight_off) : 0;
}

void tb_TEStylePaste(gaddr teh) { tb_TEPaste(teh); }
void tb_TEStyleInsert(gaddr text, int32_t length, gaddr hST, gaddr teh) { tb_TEInsert(text, length, teh); }
void tb_TEUseStyleScrap(int32_t start, int32_t stop, gaddr newstyles, int8_t redraw_now, gaddr teh) {}
void tb_TECustomHook(int16_t sel, gaddr addr, gaddr teh) {}
int32_t tb_TENumStyles(int32_t start, int32_t stop, gaddr teh) { return 1; }

/* A style scrap with the record's one style (StScrpRec). */
gaddr tb_TEGetStyleScrapHandle(gaddr teh)
{
    gaddr h = tb_NewHandle(2 + 20, false, true);
    gaddr p = rd32(h);
    wr16(p, 1);                                                /* scrpNStyles */
    wr32(p + 2, 0);                                            /* scrpStartChar */
    wr16(p + 6, (uint16_t)te16(teh, TERec_lineHeight_off));   /* scrpHeight */
    wr16(p + 8, (uint16_t)te16(teh, TERec_fontAscent_off));   /* scrpAscent */
    wr16(p + 10, (uint16_t)te16(teh, TERec_txFont_off));
    wr8(p + 12, rd8(te(teh) + TERec_txFace_off));
    wr16(p + 14, (uint16_t)te16(teh, TERec_txSize_off));
    return h;
}

/* The style record (TEStyleRec) describing one run. */
gaddr tb_TEGetStyleHandle(gaddr teh)
{
    gaddr h = tb_NewHandle(32, false, true);
    gaddr p = rd32(h);
    wr16(p, 1);              /* nRuns */
    wr16(p + 2, 1);          /* nStyles */
    gaddr st = tb_NewHandle(18, false, true);
    gaddr s = rd32(st);
    wr16(s, 1);
    wr16(s + 2, (uint16_t)te16(teh, TERec_lineHeight_off));
    wr16(s + 4, (uint16_t)te16(teh, TERec_fontAscent_off));
    wr16(s + 6, (uint16_t)te16(teh, TERec_txFont_off));
    wr8(s + 8, rd8(te(teh) + TERec_txFace_off));
    wr16(s + 10, (uint16_t)te16(teh, TERec_txSize_off));
    wr32(rd32(h) + 4, st);   /* styleTab */
    wr32(rd32(h) + 8, tb_NewHandle(0, false, true)); /* lhTab */
    wr16(rd32(h) + 20, 0);   /* runs[0]: start 0, style 0 */
    wr16(rd32(h) + 22, 0);
    wr16(rd32(h) + 24, (uint16_t)text_len(teh)); /* end marker */
    wr16(rd32(h) + 26, 0);
    return h;
}

void tb_TESetStyleHandle(gaddr theHandle, gaddr teh) {}
