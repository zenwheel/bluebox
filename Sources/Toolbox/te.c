/*
 * TextEdit, plain and styled.
 *
 * The TERec holds the text handle, rectangles, selection and the line-start
 * table, which grows with the record.  Drawing goes through QuickDraw in the
 * record's port.
 *
 * A styled record (TEStyleNew) keeps Apple's structures, which programs
 * read: txSize is -1 and txFont/txFace hold the TEStyleHandle, lineHeight
 * and fontAscent are -1.  The TEStyleRec has the style runs (start, style
 * index, ending with teLength + 1), the style table (STElement: reference
 * count, height, ascent, font, face, size, colour), the line-height table
 * (LHElement per line) and the null style (the style typed text gets
 * after TESetStyle with no selection).  A plain record works the same way
 * with one run whose style is the record's own font, face and size.
 */
#include <stdlib.h>
#include <string.h>

#include "qd_internal.h"
#include "tb_internal.h"

enum { patXor = 10 };
enum { MAX_LINES_GROW = 16 };
enum { doFont = 1, doFace = 2, doSize = 4, doColor = 8, addSize = 16, doToggle = 32 };
/* TEStyleRec */
enum { SR_NRUNS = 0, SR_NSTYLES = 2, SR_STYLETAB = 4, SR_LHTAB = 8, SR_REFCON = 12, SR_NULLSTYLE = 16, SR_RUNS = 20 };
/* STElement */
enum { ST_COUNT = 0, ST_HEIGHT = 2, ST_ASCENT = 4, ST_FONT = 6, ST_FACE = 8, ST_SIZE = 10, ST_COLOR = 12, ST_LEN = 18 };
/* StScrpRec: scrpNStyles, then ScrpSTElement (start, height, ascent, font, face, size, colour) */
enum { SC_START = 0, SC_HEIGHT = 4, SC_ASCENT = 6, SC_FONT = 8, SC_FACE = 10, SC_SIZE = 12, SC_COLOR = 14, SC_LEN = 20 };

bool ev_still_down(void);
uint32_t ev_mouse_global(void);
void ev_wait_tick(void);

static gaddr te(gaddr teh) { return rd32(teh); }
/* A TEHandle a call can use: programs pass nil ones (Mac OS Setup
 * Assistant's TECalText), which on a Mac read low memory as a record;
 * here the call does nothing. */
static bool te_ok(gaddr teh) { return teh && rd32(teh); }
static int16_t te16(gaddr teh, uint32_t off) { return (int16_t)rd16(te(teh) + off); }
static void set16(gaddr teh, uint32_t off, int16_t v) { wr16(te(teh) + off, (uint16_t)v); }
static int text_len(gaddr teh) { return te16(teh, TERec_teLength_off); }
static gaddr text_ptr(gaddr teh) { return rd32(TERec_hText(te(teh))); }
static int nlines(gaddr teh) { return te16(teh, TERec_nLines_off); }
static int line_start(gaddr teh, int i) { return te16(teh, TERec_lineStarts_off + (uint32_t)i * 2); }

/* ---------------------------------------------------------------------- */
/* Styles */

typedef struct {
    int16_t font, size;
    uint8_t face;
    uint16_t color[3];
    int16_t height, ascent;
} tstyle;

static bool styled(gaddr teh) { return te16(teh, TERec_txSize_off) == -1; }
static gaddr style_rec(gaddr teh) { return rd32(rd32(te(teh) + TERec_txFont_off)); } /* *TEStyleHandle */
static int nruns(gaddr teh) { return styled(teh) ? (int16_t)rd16(style_rec(teh) + SR_NRUNS) : 1; }
static int run_start(gaddr teh, int i)
{
    if (!styled(teh))
        return i ? text_len(teh) + 1 : 0;
    return (int16_t)rd16(style_rec(teh) + SR_RUNS + (uint32_t)i * 4);
}
static int run_style(gaddr teh, int i) { return styled(teh) ? (int16_t)rd16(style_rec(teh) + SR_RUNS + (uint32_t)i * 4 + 2) : 0; }
static gaddr style_entry(gaddr teh, int idx) { return rd32(rd32(style_rec(teh) + SR_STYLETAB)) + (uint32_t)idx * ST_LEN; }

static void get_style(gaddr teh, int idx, tstyle *s)
{
    memset(s, 0, sizeof *s);
    if (!styled(teh)) {
        s->font = te16(teh, TERec_txFont_off);
        s->face = rd8(te(teh) + TERec_txFace_off);
        s->size = te16(teh, TERec_txSize_off);
        s->height = te16(teh, TERec_lineHeight_off);
        s->ascent = te16(teh, TERec_fontAscent_off);
        return;
    }
    gaddr e = style_entry(teh, idx);
    s->height = (int16_t)rd16(e + ST_HEIGHT);
    s->ascent = (int16_t)rd16(e + ST_ASCENT);
    s->font = (int16_t)rd16(e + ST_FONT);
    s->face = rd8(e + ST_FACE);
    s->size = (int16_t)rd16(e + ST_SIZE);
    for (int k = 0; k < 3; k++)
        s->color[k] = rd16(e + ST_COLOR + (uint32_t)k * 2);
}

static int run_of(gaddr teh, int offset)
{
    int n = nruns(teh);
    for (int i = 0; i < n; i++)
        if (offset < run_start(teh, i + 1))
            return i;
    return n - 1;
}

/* The port's text set to a style (colour too, for a colour port). */
static void use_style(const tstyle *s)
{
    gaddr p = qd_port();
    GrafPort_set_txFont(p, s->font);
    GrafPort_set_txSize(p, s->size);
    GrafPort_set_txFace(p, s->face);
}

static void use_color(const tstyle *s)
{
    gaddr c = tb_NewPtr(6, false, false);
    for (int k = 0; k < 3; k++)
        wr16(c + (uint32_t)k * 2, s->color[k]);
    tb_RGBForeColor(c);
    tb_DisposePtr(c);
}

/* A style's line height and ascent, from its font. */
static void measure(tstyle *s)
{
    gaddr p = qd_port();
    int16_t f = GrafPort_txFont(p), z = GrafPort_txSize(p);
    uint8_t face = GrafPort_txFace(p);
    use_style(s);
    gaddr info = tb_NewPtr(8, false, false);
    tb_GetFontInfo(info);
    int asc = (int16_t)rd16(info), desc = (int16_t)rd16(info + 2), lead = (int16_t)rd16(info + 6);
    tb_DisposePtr(info);
    s->height = (int16_t)(asc + desc + lead);
    s->ascent = (int16_t)asc;
    GrafPort_set_txFont(p, f);
    GrafPort_set_txSize(p, z);
    GrafPort_set_txFace(p, face);
}

/* Run with the TE's port current (and a plain record's font); restores
 * afterwards. */
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
    tstyle st;
    get_style(teh, run_style(teh, 0), &st);
    use_style(&st);
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

/* The width of [from, from + n), run by run. */
static int width_of(gaddr teh, int from, int n)
{
    if (n <= 0)
        return 0;
    uint8_t *buf = malloc((size_t)n);
    gget(buf, text_ptr(teh) + (uint32_t)from, (uint32_t)n);
    int w = 0;
    if (!styled(teh)) {
        w = qd_text_width(qd_port(), buf, n);
    } else {
        for (int i = from; i < from + n;) {
            int r = run_of(teh, i);
            int end = run_start(teh, r + 1);
            if (end > from + n)
                end = from + n;
            tstyle st;
            get_style(teh, run_style(teh, r), &st);
            use_style(&st);
            w += qd_text_width(qd_port(), buf + (i - from), end - i);
            i = end;
        }
    }
    free(buf);
    return w;
}

/* ---------------------------------------------------------------------- */
/* Lines and their heights */

static gaddr lh_table(gaddr teh) { return rd32(style_rec(teh) + SR_LHTAB); }

static int line_height(gaddr teh, int line)
{
    if (!styled(teh))
        return te16(teh, TERec_lineHeight_off);
    gaddr h = lh_table(teh);
    if (line < 0 || (int32_t)((uint32_t)line * 4 + 4) > tb_GetHandleSize(h))
        line = line < 0 ? 0 : nlines(teh) - 1;
    if (line < 0 || (int32_t)((uint32_t)line * 4 + 4) > tb_GetHandleSize(h))
        return 16;
    return (int16_t)rd16(rd32(h) + (uint32_t)line * 4);
}

static int line_ascent(gaddr teh, int line)
{
    if (!styled(teh))
        return te16(teh, TERec_fontAscent_off);
    gaddr h = lh_table(teh);
    if (line < 0 || (int32_t)((uint32_t)line * 4 + 4) > tb_GetHandleSize(h))
        line = line < 0 ? 0 : nlines(teh) - 1;
    if (line < 0 || (int32_t)((uint32_t)line * 4 + 4) > tb_GetHandleSize(h))
        return 12;
    return (int16_t)rd16(rd32(h) + (uint32_t)line * 4 + 2);
}

/* The top of a line, from the destination rectangle's top. */
static int line_top(gaddr teh, int line)
{
    if (!styled(teh))
        return line * te16(teh, TERec_lineHeight_off);
    int y = 0;
    for (int i = 0; i < line; i++)
        y += line_height(teh, i);
    return y;
}

/* The line count, as many as the record has room for; returns it. */
static int set_line_count(gaddr teh, int n)
{
    int32_t need = (int32_t)(TERec_lineStarts_off + (uint32_t)(n + 1) * 2);
    if (tb_GetHandleSize(teh) < need) {
        tb_SetHandleSize(teh, need + MAX_LINES_GROW * 2);
        if (tb_GetHandleSize(teh) < need) /* no room: as many lines as fit */
            n = (int)((tb_GetHandleSize(teh) - (int32_t)TERec_lineStarts_off) / 2) - 1;
    }
    set16(teh, TERec_nLines_off, (int16_t)n);
    return n;
}

/* The styled record's line heights: each line's tallest style. */
static void set_line_heights(gaddr teh)
{
    int n = nlines(teh);
    gaddr h = lh_table(teh);
    tb_SetHandleSize(h, (n + 1) * 4);
    if (tb_GetHandleSize(h) < (n + 1) * 4)
        return;
    for (int i = 0; i < n; i++) {
        int ls = line_start(teh, i), le = line_start(teh, i + 1);
        int height = 0, ascent = 0;
        for (int r = run_of(teh, ls); r < nruns(teh); r++) {
            if (run_start(teh, r) >= le && le > ls)
                break;
            tstyle st;
            get_style(teh, run_style(teh, r), &st);
            if (st.height > height)
                height = st.height;
            if (st.ascent > ascent)
                ascent = st.ascent;
            if (le <= ls)
                break;
        }
        wr16(rd32(h) + (uint32_t)i * 4, (uint16_t)height);
        wr16(rd32(h) + (uint32_t)i * 4 + 2, (uint16_t)ascent);
    }
    /* After a final return: the empty line the caret goes to. */
    tstyle st;
    get_style(teh, run_style(teh, run_of(teh, text_len(teh) ? text_len(teh) - 1 : 0)), &st);
    wr16(rd32(h) + (uint32_t)n * 4, (uint16_t)st.height);
    wr16(rd32(h) + (uint32_t)n * 4 + 2, (uint16_t)st.ascent);
}

/* The last run ends at teLength + 1, whoever changed the text. */
static void fix_runs_end(gaddr teh)
{
    if (!styled(teh))
        return;
    gaddr sr = style_rec(teh);
    int n = (int16_t)rd16(sr + SR_NRUNS);
    wr16(sr + SR_RUNS + (uint32_t)n * 4, (uint16_t)(text_len(teh) + 1));
    wr16(sr + SR_RUNS + (uint32_t)n * 4 + 2, 0xFFFF);
}

void tb_TECalText(gaddr teh)
{
    if (!te_ok(teh))
        return;
    /* Apps may replace the text by resizing hText; its size is the length. */
    int32_t hsize = tb_GetHandleSize(TERec_hText(te(teh)));
    set16(teh, TERec_teLength_off, (int16_t)(hsize > 32767 ? 32767 : hsize));
    fix_runs_end(teh);
    port_state s = enter(teh);
    int len = text_len(teh);
    qrect dest = qrect_get(te(teh) + TERec_destRect_off);
    int width = dest.right - dest.left;
    bool cr_only = te16(teh, TERec_crOnly_off) < 0;
    uint8_t *t = malloc(len ? (size_t)len : 1);
    gget(t, text_ptr(teh), (uint32_t)len);
    /* Every character's width, in its run's style. */
    int *cw = malloc(((size_t)len + 1) * sizeof *cw);
    for (int i = 0; i < len;) {
        int r = run_of(teh, i), end = run_start(teh, r + 1);
        if (end > len)
            end = len;
        tstyle st;
        get_style(teh, run_style(teh, r), &st);
        use_style(&st);
        for (; i < end; i++)
            cw[i] = qd_text_width(qd_port(), t + i, 1);
    }
    int *starts = malloc(((size_t)len + 2) * sizeof *starts);
    int n = 0, pos = 0;
    starts[n++] = 0;
    while (pos < len) {
        int lineb = pos, last_break = -1, w = 0, i = pos;
        while (i < len && t[i] != '\r') {
            if (!cr_only && w + cw[i] > width && i > lineb)
                break;
            w += cw[i];
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
    lines = set_line_count(teh, lines);
    for (int i = 0; i < n && i <= lines; i++)
        set16(teh, TERec_lineStarts_off + (uint32_t)i * 2, (int16_t)starts[i]);
    set16(teh, TERec_lineStarts_off + (uint32_t)lines * 2, (int16_t)len);
    if (styled(teh))
        set_line_heights(teh);
    free(starts);
    free(cw);
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

/* Point (local) of a text offset: left edge of the character, top of its
 * line; and the line. */
static int offset_point(gaddr teh, int offset, int *h, int *v)
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
    *v = dest.top + line_top(teh, line);
    return line;
}

int16_t tb_TEGetOffset(uint32_t pt, gaddr teh)
{
    if (!te_ok(teh))
        return 0;
    port_state s = enter(teh);
    qrect dest = qrect_get(te(teh) + TERec_destRect_off);
    int n = nlines(teh), len = text_len(teh);
    int y = pt_v(pt) - dest.top, line = 0;
    if (y > 0)
        for (int top = 0; line < n; line++) {
            top += line_height(teh, line);
            if (y < top)
                break;
        }
    int result;
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
    if (!te_ok(teh))
        return 0;
    port_state s = enter(teh);
    int h, v;
    int line = offset_point(teh, offset, &h, &v);
    leave(teh, s);
    return (int32_t)make_pt((int16_t)(v + line_ascent(teh, line)), (int16_t)h);
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

/* The area of the range [a, b), as the selection highlights it. */
static void range_region(gaddr teh, int a, int b, hrgn *r)
{
    if (a > b) {
        int t = a;
        a = b;
        b = t;
    }
    if (a == b)
        return;
    qrect dest = qrect_get(te(teh) + TERec_destRect_off);
    int la = line_of(teh, a), lb = line_of(teh, b);
    hrgn tmp;
    hrgn_init(&tmp);
    for (int line = la; line <= lb; line++) {
        int ls = line_start(teh, line), le = line_start(teh, line + 1);
        int s = a > ls ? a : ls, e = b < le ? b : le;
        int lx = line_x(teh, line);
        int x0 = lx + width_of(teh, ls, s - ls);
        int x1 = (b > le || (e == le && line < lb)) ? dest.right : lx + width_of(teh, ls, e - ls);
        int top = dest.top + line_top(teh, line), lh = line_height(teh, line);
        hrgn_rect(&tmp, (qrect){ (int16_t)top, (int16_t)x0, (int16_t)(top + lh), (int16_t)x1 });
        hrgn_op(r, r, &tmp, RGN_UNION);
    }
    hrgn_free(&tmp);
}

/* Invert the selection range [a, b). */
static void invert_range(gaddr teh, int a, int b)
{
    hrgn r;
    hrgn_init(&r);
    range_region(teh, a, b, &r);
    if (!hrgn_empty(&r))
        qd_fill_region(qd_port(), &r, qd_pat_black, patXor);
    hrgn_free(&r);
}

/* TextEdit 7.5: the selection's highlighted area (empty at a caret), which
 * programs test clicks against to start dragging the selection. */
int16_t tb_TEGetHiliteRgn(gaddr region, gaddr teh)
{
    if (!te_ok(teh))
        return 0;
    if (!region || !teh)
        return paramErr;
    port_state s = enter(teh);
    hrgn r;
    hrgn_init(&r);
    range_region(teh, te16(teh, TERec_selStart_off), te16(teh, TERec_selEnd_off), &r);
    rgn_write(region, &r);
    hrgn_free(&r);
    leave(teh, s);
    return noErr;
}

static void draw_caret(gaddr teh)
{
    int h, v;
    int line = offset_point(teh, te16(teh, TERec_selStart_off), &h, &v);
    hrgn r;
    hrgn_init(&r);
    hrgn_rect(&r, (qrect){ (int16_t)v, (int16_t)(h - 1), (int16_t)(v + line_height(teh, line)), (int16_t)h });
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

/* A line's text, run by run, from the pen's position. */
static void draw_runs(gaddr teh, int ls, int le)
{
    if (le <= ls)
        return;
    uint8_t *buf = malloc((size_t)(le - ls));
    gget(buf, text_ptr(teh) + (uint32_t)ls, (uint32_t)(le - ls));
    if (!styled(teh)) {
        qd_draw_text(qd_port(), buf, le - ls);
    } else {
        gaddr saved = tb_NewPtr(6, false, false);
        tb_GetForeColor(saved);
        for (int i = ls; i < le;) {
            int r = run_of(teh, i), end = run_start(teh, r + 1);
            if (end > le)
                end = le;
            tstyle st;
            get_style(teh, run_style(teh, r), &st);
            use_style(&st);
            use_color(&st);
            qd_draw_text(qd_port(), buf + (i - ls), end - i);
            i = end;
        }
        tb_RGBForeColor(saved);
        tb_DisposePtr(saved);
    }
    free(buf);
}

static void draw_lines(gaddr teh, qrect area)
{
    qrect dest = qrect_get(te(teh) + TERec_destRect_off);
    int n = nlines(teh);
    tb_EraseRect(te(teh) + TERec_viewRect_off);
    for (int i = 0, top = dest.top; i < n; top += line_height(teh, i), i++) {
        int lh = line_height(teh, i);
        if (top + lh <= area.top || top >= area.bottom)
            continue;
        int ls = line_start(teh, i), le = line_start(teh, i + 1);
        if (le > ls && rd8(text_ptr(teh) + (uint32_t)le - 1) == '\r')
            le--;
        tb_MoveTo((int16_t)line_x(teh, i), (int16_t)(top + line_ascent(teh, i)));
        draw_runs(teh, ls, le);
    }
}

void tb_TEUpdate(gaddr r, gaddr teh)
{
    if (!te_ok(teh))
        return;
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
    if (!te_ok(teh))
        return;
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
/* The style table and runs */

/* A style's index in the table, added if it isn't there. */
static int style_index(gaddr teh, const tstyle *s)
{
    gaddr sr = style_rec(teh);
    int n = (int16_t)rd16(sr + SR_NSTYLES);
    for (int i = 0; i < n; i++) {
        tstyle t;
        get_style(teh, i, &t);
        if (t.font == s->font && t.size == s->size && t.face == s->face && !memcmp(t.color, s->color, sizeof t.color))
            return i;
    }
    for (int i = 0; i < n; i++) /* a free slot */
        if (rd16(style_entry(teh, i) + ST_COUNT) == 0) {
            n = i;
            goto put;
        }
    {
        gaddr tab = rd32(sr + SR_STYLETAB);
        tb_SetHandleSize(tab, (n + 1) * ST_LEN);
        sr = style_rec(teh);
        wr16(sr + SR_NSTYLES, (uint16_t)(n + 1));
    }
put:;
    tstyle m = *s;
    measure(&m);
    gaddr e = style_entry(teh, n);
    wr16(e + ST_COUNT, 0);
    wr16(e + ST_HEIGHT, (uint16_t)m.height);
    wr16(e + ST_ASCENT, (uint16_t)m.ascent);
    wr16(e + ST_FONT, (uint16_t)m.font);
    wr8(e + ST_FACE, m.face);
    wr8(e + ST_FACE + 1, 0);
    wr16(e + ST_SIZE, (uint16_t)m.size);
    for (int k = 0; k < 3; k++)
        wr16(e + ST_COLOR + (uint32_t)k * 2, m.color[k]);
    return n;
}

/* The runs as an array, to change and put back. */
typedef struct {
    int start, style;
} run;

static int load_runs(gaddr teh, run *out, int cap)
{
    int n = nruns(teh);
    if (n > cap)
        n = cap;
    for (int i = 0; i < n; i++) {
        out[i].start = run_start(teh, i);
        out[i].style = run_style(teh, i);
    }
    return n;
}

/* Runs back in the record: empty ones dropped, neighbours of one style
 * merged, the styles' reference counts recounted. */
static void store_runs(gaddr teh, run *r, int n)
{
    int len = text_len(teh), k = 0;
    for (int i = 0; i < n; i++) {
        int end = i + 1 < n ? r[i + 1].start : len;
        if (end <= r[i].start && !(len == 0 && k == 0 && i == n - 1))
            continue; /* empty */
        if (k && r[k - 1].style == r[i].style)
            continue;
        r[k++] = r[i];
    }
    if (!k) {
        r[0] = (run){ 0, n ? r[n - 1].style : 0 };
        k = 1;
    }
    r[0].start = 0;
    gaddr h = rd32(te(teh) + TERec_txFont_off);
    tb_SetHandleSize(h, SR_RUNS + (k + 1) * 4);
    gaddr sr = style_rec(teh);
    wr16(sr + SR_NRUNS, (uint16_t)k);
    for (int i = 0; i < k; i++) {
        wr16(sr + SR_RUNS + (uint32_t)i * 4, (uint16_t)r[i].start);
        wr16(sr + SR_RUNS + (uint32_t)i * 4 + 2, (uint16_t)r[i].style);
    }
    fix_runs_end(teh);
    int ns = (int16_t)rd16(sr + SR_NSTYLES);
    for (int i = 0; i < ns; i++)
        wr16(style_entry(teh, i) + ST_COUNT, 0);
    for (int i = 0; i < k; i++) {
        gaddr e = style_entry(teh, r[i].style);
        wr16(e + ST_COUNT, (uint16_t)(rd16(e + ST_COUNT) + 1));
    }
}

/* Runs split at offset `at` (a run starting there, if there wasn't). */
static int split_at(run *r, int n, int at, int cap)
{
    for (int i = 0; i < n; i++) {
        int end = i + 1 < n ? r[i + 1].start : 0x7FFF;
        if (r[i].start == at)
            return n;
        if (r[i].start < at && at < end && n < cap) {
            memmove(r + i + 2, r + i + 1, sizeof r[0] * (size_t)(n - i - 1));
            r[i + 1] = (run){ at, r[i].style };
            return n + 1;
        }
    }
    return n;
}

/* The null style: what text typed next will have, set by TESetStyle with
 * nothing selected.  Kept in the NullSTRec's scrap as one style. */
static gaddr null_scrap(gaddr teh)
{
    gaddr ns = rd32(style_rec(teh) + SR_NULLSTYLE);
    return ns ? rd32(rd32(ns) + 4) : 0;
}

static bool null_style(gaddr teh, tstyle *s)
{
    gaddr sc = styled(teh) ? null_scrap(teh) : 0;
    if (!sc || (int16_t)rd16(rd32(sc)) < 1)
        return false;
    gaddr e = rd32(sc) + 2;
    memset(s, 0, sizeof *s);
    s->font = (int16_t)rd16(e + SC_FONT);
    s->face = rd8(e + SC_FACE);
    s->size = (int16_t)rd16(e + SC_SIZE);
    for (int k = 0; k < 3; k++)
        s->color[k] = rd16(e + SC_COLOR + (uint32_t)k * 2);
    return true;
}

static void set_null_style(gaddr teh, const tstyle *s)
{
    gaddr sc = null_scrap(teh);
    if (!sc)
        return;
    if (!s) {
        tb_SetHandleSize(sc, 2);
        wr16(rd32(sc), 0);
        return;
    }
    tb_SetHandleSize(sc, 2 + SC_LEN);
    gaddr e = rd32(sc) + 2;
    wr16(rd32(sc), 1);
    wr32(e + SC_START, 0);
    wr16(e + SC_HEIGHT, (uint16_t)s->height);
    wr16(e + SC_ASCENT, (uint16_t)s->ascent);
    wr16(e + SC_FONT, (uint16_t)s->font);
    wr8(e + SC_FACE, s->face);
    wr8(e + SC_FACE + 1, 0);
    wr16(e + SC_SIZE, (uint16_t)s->size);
    for (int k = 0; k < 3; k++)
        wr16(e + SC_COLOR + (uint32_t)k * 2, s->color[k]);
}

/* The style text typed at offset gets: the null style if one is set, else
 * the character before's (the first's at the start). */
static void style_for_insert(gaddr teh, int at, tstyle *s)
{
    if (null_style(teh, s))
        return;
    get_style(teh, run_style(teh, run_of(teh, at > 0 ? at - 1 : 0)), s);
}

/* The runs after [a, b) of the old text became n characters of style
 * `with`: what was before a stays, the new text is a run of its own, what
 * was after b moves along (store_runs merges neighbours). */
static void runs_replace(gaddr teh, int a, int b, int n, const tstyle *with)
{
    run r[600], out[600];
    int k = load_runs(teh, r, 600), m = 0;
    int delta = n - (b - a), old_len = text_len(teh) - delta;
    int with_idx = style_index(teh, with);
    for (int i = 0; i < k && m < 598; i++) { /* the parts before a */
        int end = i + 1 < k ? r[i + 1].start : old_len;
        if (r[i].start < a && end > r[i].start)
            out[m++] = (run){ r[i].start, r[i].style };
    }
    if (n > 0)
        out[m++] = (run){ a, with_idx };
    for (int i = 0; i < k && m < 599; i++) { /* the parts after b */
        int end = i + 1 < k ? r[i + 1].start : old_len;
        int from = r[i].start > b ? r[i].start : b;
        if (end > from)
            out[m++] = (run){ from + delta, r[i].style };
    }
    if (!m)
        out[m++] = (run){ 0, n > 0 ? with_idx : run_style(teh, run_of(teh, a > 0 ? a - 1 : 0)) };
    store_runs(teh, out, m);
}

/* ---------------------------------------------------------------------- */
/* Creating */

static void set_font_metrics(gaddr teh)
{
    if (styled(teh))
        return;
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

/* A styled record: its TEStyleRec with one style, the port's, one run,
 * an empty line-height table and a null style. */
gaddr tb_TEStyleNew(gaddr dst, gaddr view)
{
    gaddr teh = tb_TENew(dst, view);
    gaddr port = qd_port();
    tstyle s = { GrafPort_txFont(port), GrafPort_txSize(port), GrafPort_txFace(port), { 0, 0, 0 }, 0, 0 };
    if (s.font == 0 && s.size == 0)
        s.size = 0; /* the system font at its size, as the port says */
    gaddr sh = tb_NewHandle(SR_RUNS + 2 * 4, false, true);
    gaddr tab = tb_NewHandle(0, false, true); /* style_index adds the first */
    gaddr lh = tb_NewHandle(4, false, true);
    gaddr ns = tb_NewHandle(8, false, true);
    gaddr nscrap = tb_NewHandle(2, false, true);
    wr32(rd32(ns) + 4, nscrap);
    gaddr sr = rd32(sh);
    wr16(sr + SR_NRUNS, 1);
    wr16(sr + SR_NSTYLES, 0);
    wr32(sr + SR_STYLETAB, tab);
    wr32(sr + SR_LHTAB, lh);
    wr32(sr + SR_NULLSTYLE, ns);
    gaddr t = te(teh);
    wr32(t + TERec_txFont_off, sh); /* txFont and txFace: the style handle */
    wr16(t + TERec_txSize_off, 0xFFFF);
    wr16(t + TERec_lineHeight_off, 0xFFFF);
    wr16(t + TERec_fontAscent_off, 0xFFFF);
    port_state ps = { qd_port(), 0, 0, 0, 0 };
    tb_SetPort(TERec_inPort(te(teh)));
    style_index(teh, &s);
    tb_SetPort(ps.port);
    wr16(style_entry(teh, 0) + ST_COUNT, 1);
    sr = style_rec(teh);
    wr16(sr + SR_RUNS, 0);
    wr16(sr + SR_RUNS + 2, 0);
    tb_TECalText(teh);
    return teh;
}

void tb_TEDispose(gaddr teh)
{
    if (!te_ok(teh))
        return;
    tb_TEAutoView(0, teh);
    if (styled(teh)) {
        gaddr sh = rd32(te(teh) + TERec_txFont_off), sr = rd32(sh);
        gaddr ns = rd32(sr + SR_NULLSTYLE);
        if (ns) {
            tb_DisposeHandle(rd32(rd32(ns) + 4));
            tb_DisposeHandle(ns);
        }
        tb_DisposeHandle(rd32(sr + SR_LHTAB));
        tb_DisposeHandle(rd32(sr + SR_STYLETAB));
        tb_DisposeHandle(sh);
    }
    tb_DisposeHandle(TERec_hText(te(teh)));
    tb_DisposeHandle(teh);
}

void tb_TESetText(gaddr p, int32_t ln, gaddr teh)
{
    if (!te_ok(teh))
        return;
    gaddr h = TERec_hText(te(teh));
    tb_SetHandleSize(h, ln);
    gmemcpy(rd32(h), p, (uint32_t)ln);
    set16(teh, TERec_teLength_off, (int16_t)ln);
    set16(teh, TERec_selStart_off, (int16_t)ln);
    set16(teh, TERec_selEnd_off, (int16_t)ln);
    if (styled(teh)) { /* the new text in the first style */
        run r = { 0, run_style(teh, 0) };
        store_runs(teh, &r, 1);
    }
    tb_TECalText(teh);
}

gaddr tb_TEGetText(gaddr teh) { return te_ok(teh) ? TERec_hText(te(teh)) : 0; }

/* ---------------------------------------------------------------------- */
/* Selection and activation */

void tb_TESetSelect(int32_t start, int32_t stop, gaddr teh)
{
    if (!te_ok(teh))
        return;
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
    if (styled(teh) && (start != te16(teh, TERec_selStart_off) || stop != te16(teh, TERec_selEnd_off)))
        set_null_style(teh, NULL); /* a pending style lasts only where it was set */
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
    if (!te_ok(teh))
        return;
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
    if (!te_ok(teh))
        return;
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

/* The selection replaced by n bytes; with a style for them, else the
 * one typing gets. */
static void replace_styled(gaddr teh, const uint8_t *bytes, int n, const tstyle *with)
{
    int a = te16(teh, TERec_selStart_off), b = te16(teh, TERec_selEnd_off);
    int len = text_len(teh);
    gaddr h = TERec_hText(te(teh));
    if (!h || !rd32(h))
        return;
    int size = tb_GetHandleSize(h);
    len = len < 0 ? 0 : len > size ? size : len;
    /* The selection pinned to the text, as TextEdit pinned it: a program
     * that set it past the end (Star Patrol, before Cut) would otherwise
     * move a negative length's worth of bytes. */
    a = a < 0 ? 0 : a > len ? len : a;
    b = b < a ? a : b > len ? len : b;
    int newlen = len - (b - a) + n;
    if (newlen > 32767)
        return; /* TextEdit's limit */
    tstyle ins;
    if (styled(teh)) {
        if (with)
            ins = *with;
        else
            style_for_insert(teh, a, &ins);
    }
    if (n > b - a) {
        /* A locked text handle may not grow (no room after it): then the
         * text stays as it is, as TextEdit left it, rather than spilling
         * over the next block (NCSA Mosaic's URL field). */
        tb_SetHandleSize(h, newlen);
        if (tb_GetHandleSize(h) < newlen)
            return;
    }
    gaddr p = rd32(h);
    gmemcpy(p + (uint32_t)(a + n), p + (uint32_t)b, (uint32_t)(len - b));
    if (n)
        gput(p + (uint32_t)a, bytes, (uint32_t)n);
    if (n < b - a)
        tb_SetHandleSize(h, newlen);
    set16(teh, TERec_teLength_off, (int16_t)newlen);
    if (styled(teh)) {
        port_state s = enter(teh);
        runs_replace(teh, a, b, n, &ins);
        leave(teh, s);
        if (n)
            set_null_style(teh, NULL);
    }
    set16(teh, TERec_selStart_off, (int16_t)(a + n));
    set16(teh, TERec_selEnd_off, (int16_t)(a + n));
    tb_TECalText(teh);
}

static void replace(gaddr teh, const uint8_t *bytes, int n) { replace_styled(teh, bytes, n, NULL); }

static void edit(gaddr teh, const uint8_t *bytes, int n)
{
    replace(teh, bytes, n);
    wr8(te(teh) + TERec_caretState_off, 0xFF);
    redraw(teh);
    tb_TESelView(teh);
}

void tb_TEKey(int16_t thec, gaddr teh)
{
    if (!te_ok(teh))
        return;
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
        int line = offset_point(teh, a, &h, &v);
        leave(teh, s);
        int step = c == 30 ? -line_height(teh, line > 0 ? line - 1 : 0) : line_height(teh, line);
        int16_t off = tb_TEGetOffset(make_pt((int16_t)(v + step + 1), (int16_t)h), teh);
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
    if (!te_ok(teh))
        return;
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
    if (!te_ok(teh))
        return;
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

/* A styled record's copy goes to the desk scrap as TEXT and styl too, as
 * TextEdit's did, for TEStylePaste and other programs. */
static void copy_styles_to_desk(gaddr teh)
{
    int a = te16(teh, TERec_selStart_off), b = te16(teh, TERec_selEnd_off);
    gaddr st = tb_TEGetStyleScrapHandle(teh);
    tb_ZeroScrap();
    tb_HLock(TERec_hText(te(teh)));
    tb_PutScrap(b - a, (int32_t)fourcc("TEXT"), text_ptr(teh) + (uint32_t)a);
    tb_HUnlock(TERec_hText(te(teh)));
    if (st) {
        tb_HLock(st);
        tb_PutScrap(tb_GetHandleSize(st), (int32_t)fourcc("styl"), rd32(st));
        tb_DisposeHandle(st);
    }
}

void tb_TECopy(gaddr teh)
{
    if (!te_ok(teh))
        return;
    int a = te16(teh, TERec_selStart_off), b = te16(teh, TERec_selEnd_off);
    gaddr h = scrap_handle();
    tb_SetHandleSize(h, b - a);
    gmemcpy(rd32(h), text_ptr(teh) + (uint32_t)a, (uint32_t)(b - a));
    wr16(LM_TEScrpLength, (uint16_t)(b - a));
    if (styled(teh) && b > a)
        copy_styles_to_desk(teh);
}

void tb_TECut(gaddr teh)
{
    if (!te_ok(teh))
        return;
    tb_TECopy(teh);
    tb_TEDelete(teh);
}

void tb_TEPaste(gaddr teh)
{
    if (!te_ok(teh))
        return;
    gaddr h = scrap_handle();
    int n = (int16_t)rd16(LM_TEScrpLength);
    uint8_t *buf = malloc(n > 0 ? (size_t)n : 1);
    gget(buf, rd32(h), (uint32_t)(n > 0 ? n : 0));
    edit(teh, buf, n > 0 ? n : 0);
    free(buf);
}

gaddr tb_TEScrapHandle(void) { return scrap_handle(); }
int32_t tb_TEGetScrapLength(void) { return (int16_t)rd16(LM_TEScrpLength); }
void tb_TESetScrapLength(int32_t ln) { wr16(LM_TEScrpLength, (uint16_t)(ln > 32767 ? 32767 : ln < 0 ? 0 : ln)); }
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
    if (!te_ok(teh))
        return;
    int16_t anchor = ext ? te16(teh, TERec_selStart_off) : tb_TEGetOffset(p, teh);
    int16_t off = tb_TEGetOffset(p, teh);
    /* Double-click selects a word. */
    uint32_t now = rd32(LM_Ticks);
    bool dbl = !ext && now - (uint32_t)TERec_clickTime(te(teh)) < rd32(LM_DoubleTime) &&
               te16(teh, TERec_clikStuff_off) == off;
    LOGF(LOG_TRAPS, "  TEClick at offset %d%s\n", off, dbl ? ", a double-click" : "");
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
    if (!te_ok(teh))
        return;
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
    if (!te_ok(teh))
        return;
    /* Do not scroll past the last line. */
    qrect d = qrect_get(te(teh) + TERec_destRect_off), v = qrect_get(te(teh) + TERec_viewRect_off);
    int bottom = d.top + line_top(teh, nlines(teh));
    if (dv < 0 && bottom + dv < v.bottom)
        dv = (int16_t)(v.bottom - bottom < 0 ? v.bottom - bottom : 0);
    if (dv > 0 && d.top + dv > v.top)
        dv = (int16_t)(v.top - d.top > 0 ? v.top - d.top : 0);
    tb_TEScroll(dh, dv, teh);
}

/* The records with automatic scrolling on: each record has its own flag
 * (the Installer's licence turns it on for one record, then makes another
 * that must not scroll to the end of the text it inserts). */
static gaddr auto_view[64];

static int auto_view_slot(gaddr teh)
{
    for (int i = 0; i < (int)(sizeof auto_view / sizeof auto_view[0]); i++)
        if (auto_view[i] == teh)
            return i;
    return -1;
}

void tb_TEAutoView(int8_t autoflag, gaddr teh)
{
    if (!te_ok(teh))
        return;
    int i = auto_view_slot(teh);
    if (!autoflag && i >= 0)
        auto_view[i] = 0;
    else if (autoflag && i < 0 && (i = auto_view_slot(0)) >= 0)
        auto_view[i] = teh;
}

void tb_TESelView(gaddr teh)
{
    if (!te_ok(teh))
        return;
    if (!teh || auto_view_slot(teh) < 0)
        return;
    port_state s = enter(teh);
    int h, v;
    int line = offset_point(teh, te16(teh, TERec_selStart_off), &h, &v);
    leave(teh, s);
    qrect view = qrect_get(te(teh) + TERec_viewRect_off);
    int lh = line_height(teh, line);
    if (v < view.top)
        tb_TEScroll(0, (int16_t)(view.top - v), teh);
    else if (v + lh > view.bottom)
        tb_TEScroll(0, (int16_t)(view.bottom - v - lh), teh);
}

/* As in the ROM, the text isn't redrawn: the caller updates it. */
void tb_TESetAlignment(int16_t j, gaddr teh)
{
    if (te_ok(teh))
        set16(teh, TERec_just_off, j);
}

void tb_TETextBox(gaddr p, int32_t ln, gaddr r, int16_t j)
{
    gaddr teh = tb_TENew(r, r);
    set16(teh, TERec_just_off, j);
    tb_TESetText(p, ln, teh);
    tb_EraseRect(r);
    tb_TEUpdate(r, teh);
    tb_TEDispose(teh);
}

/* teFAutoScroll (0) is TEAutoView's flag; the others are not kept. */
int16_t tb_TEFeatureFlag(int16_t feature, int16_t action, gaddr t)
{
    if (!te_ok(t))
        return 0;
    if (feature != 0)
        return 0;
    int16_t was = auto_view_slot(t) >= 0;
    if (action == 0 || action == 1) /* teBitClear, teBitSet; teBitTest is -1 */
        tb_TEAutoView((int8_t)action, t);
    return was;
}

void tb_TEInit(void)
{
    scrap_handle();
    wr16(LM_TEScrpLength, 0);
}

/* ---------------------------------------------------------------------- */
/* Style calls */

static void put_style(const tstyle *s, gaddr ts)
{
    wr16(ts + TextStyle_tsFont_off, (uint16_t)s->font);
    wr8(ts + TextStyle_tsFace_off, s->face);
    wr8(ts + TextStyle_filler_off, 0);
    wr16(ts + TextStyle_tsSize_off, (uint16_t)s->size);
    for (int k = 0; k < 3; k++)
        wr16(ts + TextStyle_tsColor_off + (uint32_t)k * 2, s->color[k]);
}

/* A style changed by TESetStyle's mode and new style.  doToggle with
 * doFace: `all_have` says whether every run already has the faces. */
static void modify(tstyle *s, int mode, gaddr ns, bool all_have)
{
    if (mode & doFont)
        s->font = (int16_t)rd16(ns + TextStyle_tsFont_off);
    if (mode & doFace) {
        uint8_t f = rd8(ns + TextStyle_tsFace_off);
        if (mode & doToggle)
            s->face = all_have && f ? (uint8_t)(s->face & ~f) : (uint8_t)(s->face | f);
        else
            s->face = f;
    }
    if (mode & doSize)
        s->size = (int16_t)rd16(ns + TextStyle_tsSize_off);
    if (mode & addSize)
        s->size = (int16_t)(s->size + (int16_t)rd16(ns + TextStyle_tsSize_off));
    if (mode & doColor)
        for (int k = 0; k < 3; k++)
            s->color[k] = rd16(ns + TextStyle_tsColor_off + (uint32_t)k * 2);
    if (s->size < 0)
        s->size = 0;
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

/* Restyle [a, b) by mode, run by run; `match`, when given, only runs of
 * that style (by the mode's attributes: TEReplaceStyle). */
static void restyle(gaddr teh, int a, int b, int mode, gaddr ns, gaddr match)
{
    port_state s = enter(teh);
    run r[600];
    int n = load_runs(teh, r, 600);
    n = split_at(r, n, a, 600);
    n = split_at(r, n, b, 600);
    uint8_t f = rd8(ns + TextStyle_tsFace_off);
    bool all_have = true;
    for (int i = 0; i < n; i++) {
        int end = i + 1 < n ? r[i + 1].start : text_len(teh);
        if (r[i].start >= a && end <= b && end > r[i].start) {
            tstyle st;
            get_style(teh, r[i].style, &st);
            if ((st.face & f) != f)
                all_have = false;
        }
    }
    for (int i = 0; i < n; i++) {
        int end = i + 1 < n ? r[i + 1].start : text_len(teh);
        if (r[i].start < a || end > b || end <= r[i].start)
            continue;
        tstyle st;
        get_style(teh, r[i].style, &st);
        if (match && (((mode & doFont) && st.font != (int16_t)rd16(match + TextStyle_tsFont_off)) ||
                      ((mode & doSize) && st.size != (int16_t)rd16(match + TextStyle_tsSize_off)) ||
                      ((mode & doFace) && st.face != rd8(match + TextStyle_tsFace_off))))
            continue;
        modify(&st, mode, ns, all_have);
        r[i].style = style_index(teh, &st);
    }
    store_runs(teh, r, n);
    leave(teh, s);
}

static void apply_style(gaddr teh, int mode, gaddr ns, bool draw, gaddr match)
{
    if (!styled(teh)) { /* one style: the record's */
        tstyle st;
        get_style(teh, 0, &st);
        uint8_t f = rd8(ns + TextStyle_tsFace_off);
        if (match && (((mode & doFont) && st.font != (int16_t)rd16(match + TextStyle_tsFont_off)) ||
                      ((mode & doSize) && st.size != (int16_t)rd16(match + TextStyle_tsSize_off))))
            return;
        modify(&st, mode, ns, (st.face & f) == f);
        set16(teh, TERec_txFont_off, st.font);
        wr8(te(teh) + TERec_txFace_off, st.face);
        set16(teh, TERec_txSize_off, st.size);
        set_font_metrics(teh);
    } else {
        int a = te16(teh, TERec_selStart_off), b = te16(teh, TERec_selEnd_off);
        if (a == b) { /* nothing selected: the style typing will have */
            tstyle st;
            style_for_insert(teh, a, &st);
            modify(&st, mode, ns, (st.face & rd8(ns + TextStyle_tsFace_off)) == rd8(ns + TextStyle_tsFace_off));
            port_state s = enter(teh);
            measure(&st);
            leave(teh, s);
            set_null_style(teh, &st);
            return;
        }
        restyle(teh, a, b, mode, ns, match);
    }
    tb_TECalText(teh);
    if (draw)
        redraw_view(teh);
}

void tb_TESetStyle(int16_t mode, gaddr newStyle, int8_t redraw_now, gaddr teh)
{
    if (!te_ok(teh))
        return;
    apply_style(teh, mode, newStyle, redraw_now, 0);
}

void tb_TEReplaceStyle(int16_t mode, gaddr oldStyle, gaddr newStyle, int8_t redraw_now, gaddr teh)
{
    if (!te_ok(teh))
        return;
    apply_style(teh, mode, newStyle, redraw_now, oldStyle);
}

/* The style at an offset: its character's (the one before at the end),
 * with its line's height and ascent. */
void tb_TEGetStyle(int16_t offset, gaddr theStyle, gaddr lineHeight, gaddr fontAscent, gaddr teh)
{
    if (!te_ok(teh))
        return;
    int len = text_len(teh);
    int at = offset >= len && len > 0 ? len - 1 : offset < 0 ? 0 : offset;
    tstyle st;
    get_style(teh, run_style(teh, run_of(teh, at)), &st);
    if (theStyle)
        put_style(&st, theStyle);
    int line = line_of(teh, offset);
    if (lineHeight)
        wr16(lineHeight, (uint16_t)line_height(teh, line));
    if (fontAscent)
        wr16(fontAscent, (uint16_t)line_ascent(teh, line));
}

/* Whether each attribute the mode asks about is the same across the
 * selection; those that aren't are taken out of the mode. */
int8_t tb_TEContinuousStyle(gaddr modep, gaddr thestyle, gaddr teh)
{
    if (!te_ok(teh))
        return 0;
    int a = te16(teh, TERec_selStart_off), b = te16(teh, TERec_selEnd_off);
    int mode = modep ? (int16_t)rd16(modep) : 0;
    tstyle first;
    if (a == b || !styled(teh)) {
        style_for_insert(teh, a, &first);
        if (!styled(teh))
            get_style(teh, 0, &first);
        if (thestyle)
            put_style(&first, thestyle);
        return 1;
    }
    get_style(teh, run_style(teh, run_of(teh, a)), &first);
    uint8_t common_face = first.face;
    for (int r = run_of(teh, a); r < nruns(teh) && run_start(teh, r) < b; r++) {
        tstyle st;
        get_style(teh, run_style(teh, r), &st);
        if (st.font != first.font)
            mode &= ~doFont;
        if (st.size != first.size)
            mode &= ~doSize;
        if (memcmp(st.color, first.color, sizeof st.color))
            mode &= ~doColor;
        common_face &= st.face;
    }
    first.face = common_face; /* the faces every run has */
    if (thestyle)
        put_style(&first, thestyle);
    int asked = modep ? (int16_t)rd16(modep) : 0;
    if (modep)
        wr16(modep, (uint16_t)mode);
    return (mode & (doFont | doSize | doColor)) == (asked & (doFont | doSize | doColor));
}

int32_t tb_TEGetHeight(int32_t endLine, int32_t startLine, gaddr teh)
{
    if (!te_ok(teh))
        return 0;
    if (endLine < startLine) {
        int32_t t = endLine;
        endLine = startLine;
        startLine = t;
    }
    int n = nlines(teh);
    /* At least a line's worth: no text is still the caret's line high,
     * and lines before the first are the first (Apple's Installer, scrolling
     * up a line at a time, asks for line 0 at the top and divides by it). */
    if (n == 0)
        return line_height(teh, 0);
    if (endLine > n)
        endLine = n;
    if (startLine < 1)
        startLine = 1;
    if (startLine > n)
        startLine = n;
    if (endLine < startLine)
        endLine = startLine;
    int32_t h = 0;
    for (int i = startLine - 1; i < endLine; i++)
        h += line_height(teh, i);
    return h;
}

int32_t tb_TENumStyles(int32_t start, int32_t stop, gaddr teh)
{
    if (!te_ok(teh))
        return 0;
    if (!styled(teh))
        return 1;
    int n = 0;
    for (int r = run_of(teh, start); r < nruns(teh) && (run_start(teh, r) < stop || n == 0); r++)
        n++;
    return n;
}

/* A style scrap (StScrpRec) for the selection: an element per run, its
 * start counted from the selection's start. */
gaddr tb_TEGetStyleScrapHandle(gaddr teh)
{
    if (!te_ok(teh))
        return 0;
    int a = te16(teh, TERec_selStart_off), b = te16(teh, TERec_selEnd_off);
    int first = run_of(teh, a), last = b > a ? run_of(teh, b - 1) : first;
    int n = last - first + 1;
    gaddr h = tb_NewHandle(2 + n * SC_LEN, false, true);
    wr16(rd32(h), (uint16_t)n);
    for (int i = 0; i < n; i++) {
        int r = first + i;
        tstyle st;
        get_style(teh, run_style(teh, r), &st);
        int start = run_start(teh, r) - a;
        gaddr e = rd32(h) + 2 + (uint32_t)i * SC_LEN;
        wr32(e + SC_START, (uint32_t)(start < 0 ? 0 : start));
        wr16(e + SC_HEIGHT, (uint16_t)st.height);
        wr16(e + SC_ASCENT, (uint16_t)st.ascent);
        wr16(e + SC_FONT, (uint16_t)st.font);
        wr8(e + SC_FACE, st.face);
        wr16(e + SC_SIZE, (uint16_t)st.size);
        for (int k = 0; k < 3; k++)
            wr16(e + SC_COLOR + (uint32_t)k * 2, st.color[k]);
    }
    return h;
}

/* A style scrap's styles over [start, stop). */
void tb_TEUseStyleScrap(int32_t start, int32_t stop, gaddr newstyles, int8_t redraw_now, gaddr teh)
{
    if (!te_ok(teh))
        return;
    if (!styled(teh) || !newstyles || !rd32(newstyles))
        return;
    int n = (int16_t)rd16(rd32(newstyles));
    gaddr ts = tb_NewPtr(TextStyle_SIZEOF, false, true);
    int a = te16(teh, TERec_selStart_off), b = te16(teh, TERec_selEnd_off);
    for (int i = 0; i < n; i++) {
        gaddr e = rd32(newstyles) + 2 + (uint32_t)i * SC_LEN;
        int from = start + (int32_t)rd32(e + SC_START);
        int to = i + 1 < n ? start + (int32_t)rd32(rd32(newstyles) + 2 + (uint32_t)(i + 1) * SC_LEN + SC_START) : stop;
        if (to > stop)
            to = stop;
        if (from >= to)
            continue;
        wr16(ts + TextStyle_tsFont_off, rd16(e + SC_FONT));
        wr8(ts + TextStyle_tsFace_off, rd8(e + SC_FACE));
        wr16(ts + TextStyle_tsSize_off, rd16(e + SC_SIZE));
        gmemcpy(ts + TextStyle_tsColor_off, e + SC_COLOR, 6);
        restyle(teh, from, to, doFont | doFace | doSize | doColor, ts, 0);
    }
    tb_DisposePtr(ts);
    set16(teh, TERec_selStart_off, (int16_t)a);
    set16(teh, TERec_selEnd_off, (int16_t)b);
    tb_TECalText(teh);
    if (redraw_now)
        redraw_view(teh);
}

/* Text inserted before the selection, in a style scrap's styles. */
void tb_TEStyleInsert(gaddr text, int32_t length, gaddr hST, gaddr teh)
{
    if (!te_ok(teh))
        return;
    int a = te16(teh, TERec_selStart_off);
    tb_TEInsert(text, length, teh);
    if (hST && styled(teh)) {
        tb_TEUseStyleScrap(a, a + length, hST, false, teh);
        redraw(teh);
    }
}

/* The desk scrap's text with its styl styles, or the TextEdit scrap's. */
void tb_TEStylePaste(gaddr teh)
{
    if (!te_ok(teh))
        return;
    if (!styled(teh)) {
        tb_TEPaste(teh);
        return;
    }
    gaddr text = tb_NewHandle(0, false, false), st = tb_NewHandle(0, false, false);
    gaddr off = tb_NewPtr(4, false, true);
    int32_t n = tb_GetScrap(text, (int32_t)fourcc("TEXT"), off);
    int32_t ns = n >= 0 ? tb_GetScrap(st, (int32_t)fourcc("styl"), off) : -1;
    tb_DisposePtr(off);
    if (n < 0) {
        tb_TEPaste(teh);
    } else {
        int a = te16(teh, TERec_selStart_off);
        tb_HLock(text);
        uint8_t *buf = malloc(n > 0 ? (size_t)n : 1);
        gget(buf, rd32(text), (uint32_t)n);
        replace(teh, buf, n);
        free(buf);
        if (ns > 0)
            tb_TEUseStyleScrap(a, a + n, st, false, teh);
        wr8(te(teh) + TERec_caretState_off, 0xFF);
        redraw(teh);
        tb_TESelView(teh);
    }
    tb_DisposeHandle(text);
    tb_DisposeHandle(st);
}

void tb_TECustomHook(int16_t sel, gaddr addr, gaddr teh) {}

gaddr tb_TEGetStyleHandle(gaddr teh) { return te_ok(teh) && styled(teh) ? rd32(te(teh) + TERec_txFont_off) : 0; }

void tb_TESetStyleHandle(gaddr theHandle, gaddr teh)
{
    if (!te_ok(teh))
        return;
    if (styled(teh) && theHandle) {
        wr32(te(teh) + TERec_txFont_off, theHandle);
        tb_TECalText(teh);
    }
}
