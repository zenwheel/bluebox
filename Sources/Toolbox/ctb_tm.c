/*
 * The Terminal Manager, with two tools of our own:
 *
 *   TTY Tool    a glass teletype: printable characters, carriage return,
 *               line feed, backspace, tab and bell
 *   VT102 Tool  a DEC VT102 as BBSes and Unix hosts expect one: cursor
 *               movement, erasing, insert and delete, scrolling regions,
 *               bold, underline, blink and inverse, the line drawing set,
 *               the cursor and keypad modes, device attributes and status
 *               reports; ANSI colours when Color is on
 *
 * A terminal is a grid of cells in host memory, drawn in Monaco into the
 * owner window's termRect (clipped to viewRect) after each TMStream, only
 * the rows that changed.  Keys typed go out through the sendProc; lines
 * scrolling off the top go to the cacheProc.  Lines and the cursor are
 * numbered from 1 (TMGetLine, TMGetCursor, the selection).
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ctb.h"
#include "qd_internal.h"

enum { tmNoErr = 0, tmGenericError = -1 };
enum { tmInvisible = 1 << 0, tmSaveBeforeClear = 1 << 1, tmAutoScroll = 1 << 3 };
enum { selTextNormal = 1 << 0, tmSearchNoCase = 1 << 9 };
enum { cursorText = 1, cursorGraphics = 2, tmTextTerminal = 1 };
enum { T_TTY = 1, T_VT102 };
enum { MAX_TERMS = 8, MAX_ROWS = 66, MAX_COLS = 160, MAX_SEARCH = 8 };
enum { A_BOLD = 1, A_UNDER = 2, A_BLINK = 4, A_INVERSE = 8 };
enum { COLOR_DEFAULT = 0xFF };
enum { mouseDown = 1, controlKey = 0x1000, cmdKey = 0x0100 };
/* sendProc (thePtr, theSize, refCon, flags): long */
#define PI_SEND 0x00002FF0u
/* breakProc (duration, refCon) */
#define PI_BREAK 0x000003C0u
/* cacheProc (refCon, TermDataBlock): long */
#define PI_CACHE 0x000003F0u
/* clikLoop (refCon): Boolean */
#define PI_CLIKLOOP 0x000000D0u
/* search callback (hTerm, refNum, foundRect) */
#define PI_SEARCH 0x00000EC0u

/* ---------------------------------------------------------------------- */
/* The tools' configurations */

static const char *const cursors[] = { "Block", "Underline", NULL };
static const char *const backspaces[] = { "Backspace", "Delete", NULL };

enum { C_ROWS = 4, C_COLS = 8, C_WRAP = 12, C_NEWLINE = 13, C_ECHO = 14, C_COLOR = 15, C_CURSOR = 16, C_BS = 18,
       C_ANSWER = 20, C_SIZE = 52, CONFIG_SIZE = 56 };

static const ctb_field tty_fields[] = {
    { "Rows", CF_INT, C_ROWS, 2, MAX_ROWS, NULL, "24" },
    { "Columns", CF_INT, C_COLS, 10, MAX_COLS, NULL, "80" },
    { "NewLine", CF_BOOL, C_NEWLINE, 0, 0, NULL, "False" },
    { "LocalEcho", CF_BOOL, C_ECHO, 0, 0, NULL, "False" },
    { "Backspace", CF_ENUM, C_BS, 0, 0, backspaces, "Backspace" },
    { "FontSize", CF_INT, C_SIZE, 6, 24, NULL, "9" },
};
static const ctb_field vt_fields[] = {
    { "Rows", CF_INT, C_ROWS, 2, MAX_ROWS, NULL, "24" },
    { "Columns", CF_INT, C_COLS, 10, MAX_COLS, NULL, "80" },
    { "AutoWrap", CF_BOOL, C_WRAP, 0, 0, NULL, "True" },
    { "NewLine", CF_BOOL, C_NEWLINE, 0, 0, NULL, "False" },
    { "LocalEcho", CF_BOOL, C_ECHO, 0, 0, NULL, "False" },
    { "Color", CF_BOOL, C_COLOR, 0, 0, NULL, "False" },
    { "Cursor", CF_ENUM, C_CURSOR, 0, 0, cursors, "Block" },
    { "Backspace", CF_ENUM, C_BS, 0, 0, backspaces, "Backspace" },
    { "Answerback", CF_STR, C_ANSWER, 0, 31, NULL, "" },
    { "FontSize", CF_INT, C_SIZE, 6, 24, NULL, "9" },
};

#define COUNT(a) (int)(sizeof a / sizeof a[0])
static const ctb_tool tty_tool = { "TTY Tool", 0x74747920 /* 'tty ' */, CONFIG_SIZE, tty_fields, COUNT(tty_fields) };
static const ctb_tool vt_tool = { "VT102 Tool", 0x76743132 /* 'vt12' */, CONFIG_SIZE, vt_fields, COUNT(vt_fields) };
static const ctb_tool *const tm_tools[] = { &tty_tool, &vt_tool };

/* The keys TMDoTermKey knows, as a VT102's keypad has them. */
static const char *const term_keys[] = { "Up Arrow", "Down Arrow", "Right Arrow", "Left Arrow", "PF1", "PF2", "PF3",
                                         "PF4", "Keypad 0", "Keypad 1", "Keypad 2", "Keypad 3", "Keypad 4",
                                         "Keypad 5", "Keypad 6", "Keypad 7", "Keypad 8", "Keypad 9", "Keypad -",
                                         "Keypad ,", "Keypad .", "Enter", "Break" };
enum { K_UP, K_DOWN, K_RIGHT, K_LEFT, K_PF1, K_KP0 = 8, K_MINUS = 18, K_COMMA, K_PERIOD, K_ENTER, K_BREAK, NKEYS };

/* ---------------------------------------------------------------------- */
/* Terminals */

typedef struct {
    uint8_t ch, attr, fg, bg;
} cell;

typedef struct {
    int id;
    uint8_t s[256];
    int len;
    bool nocase;
    qrect where; /* rows and columns, from 1; empty: the whole screen */
    gaddr callback;
} tm_search;

enum { P_GROUND, P_ESC, P_CSI, P_SCS0, P_SCS1, P_HASH };

typedef struct {
    bool used;
    gaddr h;
    int tool;
    int rows, cols;
    cell *screen;
    bool dirty[MAX_ROWS];
    int cr, cc;
    bool wrap_pending;
    uint8_t attr, fg, bg;
    int top, bottom;
    bool origin, autowrap, insert, newline, app_cursor, app_keypad, cursor_visible;
    bool tabs[MAX_COLS];
    int charset[2], gl;
    struct {
        int cr, cc, charset[2], gl;
        uint8_t attr, fg, bg;
        bool origin;
    } saved;
    int pstate;
    int params[16], nparams;
    bool priv;
    /* the display */
    int cw, ch, ascent;
    bool active;
    int drawn_cr, drawn_cc;
    gaddr scratch;  /* guest memory for QuickDraw's arguments and the sendProc's bytes */
    gaddr line;     /* TMPeekLine's copies */
    gaddr cache_h;  /* the line the cacheProc is given */
    /* the selection, rows and columns from 0, start inclusive, end exclusive */
    bool sel;
    int sr0, sc0, sr1, sc1;
    tm_search search[MAX_SEARCH];
    int nsearch, next_search;
} term;

static term terms[MAX_TERMS];

static gaddr rec(term *t) { return rd32(t->h); }
static gaddr cfg(term *t) { return rd32(rec(t) + TermRecord_config_off); }

static term *term_of(gaddr h)
{
    if (!h || !rd32(h))
        return NULL;
    uint32_t i = rd32(rd32(h) + TermRecord_tmPrivate_off);
    if (i < 1 || i > MAX_TERMS || !terms[i - 1].used || terms[i - 1].h != h)
        return NULL;
    return &terms[i - 1];
}

static cell *at(term *t, int r, int c) { return &t->screen[r * t->cols + c]; }

static cell blank(term *t) { return (cell){ ' ', 0, COLOR_DEFAULT, t->bg }; }

static void dirty_all(term *t)
{
    for (int r = 0; r < t->rows; r++)
        t->dirty[r] = true;
}

/* ---------------------------------------------------------------------- */
/* Drawing */

static qrect term_rect(term *t) { return qrect_get(rec(t) + TermRecord_termRect_off); }
static qrect view_rect(term *t) { return qrect_get(rec(t) + TermRecord_viewRect_off); }

static bool visible(term *t)
{
    gaddr r = rec(t);
    return rd32(r + TermRecord_owner_off) && !(rd32(r + TermRecord_flags_off) & tmInvisible);
}

static void metrics(term *t)
{
    t->cw = 6;
    t->ch = 11;
    t->ascent = 9;
    gaddr r = rec(t);
    gaddr owner = rd32(r + TermRecord_owner_off);
    if (!owner)
        return;
    gaddr s = t->scratch;
    tb_GetPort(s);
    gaddr saved = rd32(s);
    tb_SetPort(owner);
    int16_t font = GrafPort_txFont(owner), size = GrafPort_txSize(owner), face = (int16_t)rd8(owner + GrafPort_txFace_off);
    tb_TextFont(4); /* Monaco */
    tb_TextSize((int16_t)rd32(cfg(t) + C_SIZE));
    tb_TextFace(0);
    tb_GetFontInfo(s);
    int asc = (int16_t)rd16(s), desc = (int16_t)rd16(s + 2), lead = (int16_t)rd16(s + 6);
    int w = tb_CharWidth('M');
    if (asc > 0 && w > 0) {
        t->ascent = asc;
        t->ch = asc + desc + lead;
        t->cw = w;
    }
    tb_TextFont(font);
    tb_TextSize(size);
    tb_TextFace((int8_t)face);
    tb_SetPort(saved);
}

static bool selected(term *t, int r, int c)
{
    if (!t->sel)
        return false;
    int a = t->sr0 * t->cols + t->sc0, b = t->sr1 * t->cols + t->sc1, i = r * t->cols + c;
    return i >= a && i < b;
}

static const uint16_t ansi_rgb[8][3] = {
    { 0, 0, 0 },          { 0xBBBB, 0, 0 },      { 0, 0xBBBB, 0 },      { 0xBBBB, 0xBBBB, 0 },
    { 0, 0, 0xBBBB },     { 0xBBBB, 0, 0xBBBB }, { 0, 0xBBBB, 0xBBBB }, { 0xBBBB, 0xBBBB, 0xBBBB },
};
static const uint16_t ansi_bright[8][3] = {
    { 0x5555, 0x5555, 0x5555 }, { 0xFFFF, 0x5555, 0x5555 }, { 0x5555, 0xFFFF, 0x5555 }, { 0xFFFF, 0xFFFF, 0x5555 },
    { 0x5555, 0x5555, 0xFFFF }, { 0xFFFF, 0x5555, 0xFFFF }, { 0x5555, 0xFFFF, 0xFFFF }, { 0xFFFF, 0xFFFF, 0xFFFF },
};

static void rgb(gaddr s, const uint16_t c[3])
{
    wr16(s, c[0]);
    wr16(s + 2, c[1]);
    wr16(s + 4, c[2]);
}

/* A run of cells alike, from column c0 to c1 on row r. */
static void draw_run(term *t, int r, int c0, int c1, bool colors)
{
    gaddr s = t->scratch;
    qrect tr = term_rect(t);
    cell *first = at(t, r, c0);
    int x = tr.left + c0 * t->cw, y = tr.top + r * t->ch;
    qrect box = { (int16_t)y, (int16_t)x, (int16_t)(y + t->ch), (int16_t)(x + (c1 - c0) * t->cw) };
    bool inverse = (first->attr & A_INVERSE) != 0;
    if (colors) {
        int fg = first->fg == COLOR_DEFAULT ? 7 : first->fg, bg = first->bg == COLOR_DEFAULT ? 0 : first->bg;
        if (inverse) {
            int k = fg;
            fg = bg;
            bg = k;
            inverse = false;
        }
        rgb(s + 16, (first->attr & A_BOLD) ? ansi_bright[fg] : ansi_rgb[fg]);
        tb_RGBForeColor(s + 16);
        rgb(s + 16, ansi_rgb[bg]);
        tb_RGBBackColor(s + 16);
    }
    qrect_put(s, box);
    tb_EraseRect(s);
    int n = c1 - c0;
    for (int i = 0; i < n; i++)
        wr8(s + 32 + (uint32_t)i, first[i].ch);
    tb_MoveTo((int16_t)x, (int16_t)(y + t->ascent));
    tb_DrawText(s + 32, 0, (int16_t)n);
    if (first->attr & A_BOLD) { /* overstruck, so the columns stay put */
        tb_MoveTo((int16_t)(x + 1), (int16_t)(y + t->ascent));
        tb_DrawText(s + 32, 0, (int16_t)n);
    }
    if (first->attr & A_UNDER) {
        tb_MoveTo((int16_t)x, (int16_t)(y + t->ascent + 1));
        tb_LineTo((int16_t)(x + n * t->cw - 1), (int16_t)(y + t->ascent + 1));
    }
    if (inverse) {
        qrect_put(s, box);
        tb_InvertRect(s);
    }
}

static void paint_row(term *t, int r, bool colors)
{
    int c = 0;
    while (c < t->cols) {
        cell *a = at(t, r, c);
        int e = c + 1;
        while (e < t->cols && at(t, r, e)->attr == a->attr && at(t, r, e)->fg == a->fg && at(t, r, e)->bg == a->bg)
            e++;
        draw_run(t, r, c, e, colors);
        c = e;
    }
    if (colors) {
        tb_ForeColor(33); /* blackColor */
        tb_BackColor(30); /* whiteColor */
    }
    /* The selection, then the cursor, inverted over the text. */
    qrect tr = term_rect(t);
    gaddr s = t->scratch;
    for (c = 0; c < t->cols; c++) {
        if (!selected(t, r, c))
            continue;
        int e = c;
        while (e < t->cols && selected(t, r, e))
            e++;
        int x = tr.left + c * t->cw, y = tr.top + r * t->ch;
        qrect_put(s, (qrect){ (int16_t)y, (int16_t)x, (int16_t)(y + t->ch), (int16_t)(tr.left + e * t->cw) });
        tb_InvertRect(s);
        c = e;
    }
    if (r == t->cr && t->active && t->cursor_visible) {
        int x = tr.left + t->cc * t->cw, y = tr.top + r * t->ch;
        bool bar = t->tool == T_VT102 && rd16(cfg(t) + C_CURSOR) == 1;
        qrect_put(s, (qrect){ (int16_t)(bar ? y + t->ch - 2 : y), (int16_t)x, (int16_t)(y + t->ch),
                              (int16_t)(x + t->cw) });
        tb_InvertRect(s);
    }
}

/* The rows that changed, drawn. */
static void paint(term *t)
{
    if (t->drawn_cr != t->cr || t->drawn_cc != t->cc) {
        if (t->drawn_cr >= 0 && t->drawn_cr < t->rows)
            t->dirty[t->drawn_cr] = true;
        t->dirty[t->cr] = true;
    }
    if (!visible(t))
        return;
    bool any = false;
    for (int r = 0; r < t->rows && !any; r++)
        any = t->dirty[r];
    if (!any)
        return;
    gaddr s = t->scratch;
    gaddr owner = rd32(rec(t) + TermRecord_owner_off);
    tb_GetPort(s);
    gaddr saved = rd32(s);
    tb_SetPort(owner);
    int16_t font = GrafPort_txFont(owner), size = GrafPort_txSize(owner), mode = GrafPort_txMode(owner);
    uint8_t face = rd8(owner + GrafPort_txFace_off);
    gaddr clip = tb_NewRgn();
    tb_GetClip(clip);
    qrect_put(s, view_rect(t));
    tb_ClipRect(s);
    tb_PenNormal();
    tb_TextFont(4);
    tb_TextSize((int16_t)rd32(cfg(t) + C_SIZE));
    tb_TextFace(0);
    tb_TextMode(1); /* srcOr */
    bool colors = t->tool == T_VT102 && rd8(cfg(t) + C_COLOR) && qd_port_is_color(owner);
    for (int r = 0; r < t->rows; r++)
        if (t->dirty[r])
            paint_row(t, r, colors);
    tb_SetClip(clip);
    tb_DisposeRgn(clip);
    tb_TextFont(font);
    tb_TextSize(size);
    tb_TextFace((int8_t)face);
    tb_TextMode(mode);
    tb_SetPort(saved);
    memset(t->dirty, 0, sizeof t->dirty);
    t->drawn_cr = t->cr;
    t->drawn_cc = t->cc;
}

/* ---------------------------------------------------------------------- */
/* The screen */

static void send(term *t, const void *b, int n)
{
    gaddr r = rec(t);
    if (n <= 0 || n > 200)
        return;
    gput(t->scratch + 64, b, (uint32_t)n);
    uint32_t args[4] = { t->scratch + 64, (uint32_t)n, rd32(r + TermRecord_refCon_off), 0 };
    ctb_callback(rd32(r + TermRecord_sendProc_off), PI_SEND, args, 4);
}

static void send_str(term *t, const char *s) { send(t, s, (int)strlen(s)); }

/* A line leaving the screen, to the program's cache. */
static void cache_line(term *t, int r)
{
    gaddr rr = rec(t);
    gaddr proc = rd32(rr + TermRecord_cacheProc_off);
    if (!proc)
        return;
    if (!t->cache_h)
        t->cache_h = tb_NewHandle(t->cols, false, false);
    tb_SetHandleSize(t->cache_h, t->cols);
    int n = t->cols;
    while (n > 0 && at(t, r, n - 1)->ch == ' ')
        n--;
    tb_SetHandleSize(t->cache_h, n);
    for (int c = 0; c < n; c++)
        wr8(rd32(t->cache_h) + (uint32_t)c, at(t, r, c)->ch);
    gaddr block = t->scratch + 224;
    gmemset(block, 0, TermDataBlock_SIZEOF);
    wr16(block + TermDataBlock_flags_off, tmTextTerminal);
    wr32(block + TermDataBlock_theData_off, t->cache_h);
    uint32_t args[2] = { rd32(rr + TermRecord_refCon_off), block };
    ctb_callback(proc, PI_CACHE, args, 2);
}

static void clear_cells(term *t, int r, int c0, int c1)
{
    for (int c = c0; c < c1; c++)
        *at(t, r, c) = blank(t);
    t->dirty[r] = true;
}

static void scroll_up(term *t, int top, int bottom, int n)
{
    if (n > bottom - top + 1)
        n = bottom - top + 1;
    if (top == 0 && bottom == t->rows - 1)
        for (int i = 0; i < n; i++)
            cache_line(t, i);
    memmove(at(t, top, 0), at(t, top + n, 0), sizeof(cell) * (size_t)((bottom - top + 1 - n) * t->cols));
    for (int r = bottom - n + 1; r <= bottom; r++)
        clear_cells(t, r, 0, t->cols);
    for (int r = top; r <= bottom; r++)
        t->dirty[r] = true;
}

static void scroll_down(term *t, int top, int bottom, int n)
{
    if (n > bottom - top + 1)
        n = bottom - top + 1;
    memmove(at(t, top + n, 0), at(t, top, 0), sizeof(cell) * (size_t)((bottom - top + 1 - n) * t->cols));
    for (int r = top; r < top + n; r++)
        clear_cells(t, r, 0, t->cols);
    for (int r = top; r <= bottom; r++)
        t->dirty[r] = true;
}

static void line_feed(term *t)
{
    t->wrap_pending = false;
    if (t->cr == t->bottom)
        scroll_up(t, t->top, t->bottom, 1);
    else if (t->cr < t->rows - 1)
        t->cr++;
}

static void reverse_index(term *t)
{
    t->wrap_pending = false;
    if (t->cr == t->top)
        scroll_down(t, t->top, t->bottom, 1);
    else if (t->cr > 0)
        t->cr--;
}

static void clear_screen(term *t)
{
    if (rd32(rec(t) + TermRecord_flags_off) & tmSaveBeforeClear)
        for (int r = 0; r < t->rows; r++)
            cache_line(t, r);
    for (int r = 0; r < t->rows; r++)
        clear_cells(t, r, 0, t->cols);
}

static void reset_state(term *t)
{
    gaddr k = cfg(t);
    t->cr = t->cc = 0;
    t->wrap_pending = false;
    t->attr = 0;
    t->fg = t->bg = COLOR_DEFAULT;
    t->top = 0;
    t->bottom = t->rows - 1;
    t->origin = t->insert = t->app_cursor = t->app_keypad = false;
    t->autowrap = t->tool == T_TTY || rd8(k + C_WRAP);
    t->newline = rd8(k + C_NEWLINE) != 0;
    t->cursor_visible = true;
    for (int c = 0; c < MAX_COLS; c++)
        t->tabs[c] = c && c % 8 == 0;
    t->charset[0] = t->charset[1] = t->gl = 0;
    t->saved.cr = t->saved.cc = 0;
    t->saved.attr = 0;
    t->saved.fg = t->saved.bg = COLOR_DEFAULT;
    t->saved.origin = false;
    t->saved.charset[0] = t->saved.charset[1] = t->saved.gl = 0;
    t->pstate = P_GROUND;
}

/* The screen at the configuration's size, what fits of it kept. */
static void resize(term *t)
{
    gaddr k = cfg(t);
    int rows = (int)rd32(k + C_ROWS), cols = (int)rd32(k + C_COLS);
    rows = rows < 2 ? 2 : rows > MAX_ROWS ? MAX_ROWS : rows;
    cols = cols < 10 ? 10 : cols > MAX_COLS ? MAX_COLS : cols;
    if (t->screen && rows == t->rows && cols == t->cols)
        return;
    cell *s = malloc(sizeof(cell) * (size_t)(rows * cols));
    for (int i = 0; i < rows * cols; i++)
        s[i] = (cell){ ' ', 0, COLOR_DEFAULT, COLOR_DEFAULT };
    for (int r = 0; t->screen && r < rows && r < t->rows; r++)
        memcpy(&s[r * cols], at(t, r, 0), sizeof(cell) * (size_t)(cols < t->cols ? cols : t->cols));
    free(t->screen);
    t->screen = s;
    t->rows = rows;
    t->cols = cols;
    t->top = 0;
    t->bottom = rows - 1;
    if (t->cr >= rows)
        t->cr = rows - 1;
    if (t->cc >= cols)
        t->cc = cols - 1;
    t->sel = false;
    dirty_all(t);
}

/* The VT100's special graphics, as MacRoman has them or nearly. */
static uint8_t line_drawing(uint8_t c)
{
    static const char *const from = "`afgjklmnqtuvwx~yz{|}";
    static const uint8_t to[] = { 0xD7, '#', 0xA1, 0xB1, '+', '+', '+', '+', '+', '-', '+',
                                  '+', '+', '+', '|', 0xA5, 0xB2, 0xB3, 0xB9, 0xAD, 0xA3 };
    const char *p = strchr(from, c);
    return p && c ? to[p - from] : c;
}

static void put_char(term *t, uint8_t c)
{
    if (t->wrap_pending && t->autowrap) {
        t->cc = 0;
        line_feed(t);
    }
    t->wrap_pending = false;
    if (t->charset[t->gl] == 1)
        c = line_drawing(c);
    if (t->insert)
        memmove(at(t, t->cr, t->cc + 1), at(t, t->cr, t->cc), sizeof(cell) * (size_t)(t->cols - t->cc - 1));
    *at(t, t->cr, t->cc) = (cell){ c, t->attr, t->fg, t->bg };
    t->dirty[t->cr] = true;
    if (t->cc < t->cols - 1)
        t->cc++;
    else if (t->autowrap)
        t->wrap_pending = true;
}

static void tab(term *t)
{
    while (t->cc < t->cols - 1) {
        t->cc++;
        if (t->tabs[t->cc])
            break;
    }
}

static void control(term *t, uint8_t c)
{
    switch (c) {
    case 5: /* ENQ: the answerback */
        if (t->tool == T_VT102) {
            char s[64];
            ctb_config_str(cfg(t), C_ANSWER, s, sizeof s);
            send_str(t, s);
        }
        break;
    case 7: tb_SysBeep(1); break;
    case 8:
        if (t->cc > 0)
            t->cc--;
        t->wrap_pending = false;
        break;
    case 9: tab(t); break;
    case 10: case 11: case 12:
        line_feed(t);
        if (t->newline)
            t->cc = 0;
        break;
    case 13:
        t->cc = 0;
        t->wrap_pending = false;
        break;
    case 14: t->gl = 1; break; /* SO */
    case 15: t->gl = 0; break; /* SI */
    }
}

/* ---------------------------------------------------------------------- */
/* The VT102 */

static int param(term *t, int i, int def)
{
    return i < t->nparams && t->params[i] > 0 ? t->params[i] : def;
}

static void move_to(term *t, int r, int c)
{
    int lo = t->origin ? t->top : 0, hi = t->origin ? t->bottom : t->rows - 1;
    r += lo;
    t->cr = r < lo ? lo : r > hi ? hi : r;
    t->cc = c < 0 ? 0 : c >= t->cols ? t->cols - 1 : c;
    t->wrap_pending = false;
}

static void save_cursor(term *t)
{
    t->saved.cr = t->cr;
    t->saved.cc = t->cc;
    t->saved.attr = t->attr;
    t->saved.fg = t->fg;
    t->saved.bg = t->bg;
    t->saved.origin = t->origin;
    memcpy(t->saved.charset, t->charset, sizeof t->charset);
    t->saved.gl = t->gl;
}

static void restore_cursor(term *t)
{
    t->cr = t->saved.cr < t->rows ? t->saved.cr : t->rows - 1;
    t->cc = t->saved.cc < t->cols ? t->saved.cc : t->cols - 1;
    t->attr = t->saved.attr;
    t->fg = t->saved.fg;
    t->bg = t->saved.bg;
    t->origin = t->saved.origin;
    memcpy(t->charset, t->saved.charset, sizeof t->charset);
    t->gl = t->saved.gl;
    t->wrap_pending = false;
}

static void sgr(term *t)
{
    if (!t->nparams)
        t->params[t->nparams++] = 0;
    for (int i = 0; i < t->nparams; i++) {
        int p = t->params[i];
        if (p == 0) {
            t->attr = 0;
            t->fg = t->bg = COLOR_DEFAULT;
        } else if (p == 1) {
            t->attr |= A_BOLD;
        } else if (p == 4) {
            t->attr |= A_UNDER;
        } else if (p == 5) {
            t->attr |= A_BLINK;
        } else if (p == 7) {
            t->attr |= A_INVERSE;
        } else if (p == 22) {
            t->attr &= (uint8_t)~A_BOLD;
        } else if (p == 24) {
            t->attr &= (uint8_t)~A_UNDER;
        } else if (p == 25) {
            t->attr &= (uint8_t)~A_BLINK;
        } else if (p == 27) {
            t->attr &= (uint8_t)~A_INVERSE;
        } else if (p >= 30 && p <= 37) {
            t->fg = (uint8_t)(p - 30);
        } else if (p == 39) {
            t->fg = COLOR_DEFAULT;
        } else if (p >= 40 && p <= 47) {
            t->bg = (uint8_t)(p - 40);
        } else if (p == 49) {
            t->bg = COLOR_DEFAULT;
        } else if (p >= 90 && p <= 97) {
            t->fg = (uint8_t)(p - 90);
            t->attr |= A_BOLD;
        } else if (p >= 100 && p <= 107) {
            t->bg = (uint8_t)(p - 100);
        }
    }
}

static void set_mode(term *t, bool on)
{
    for (int i = 0; i < t->nparams || i == 0; i++) {
        int p = i < t->nparams ? t->params[i] : 0;
        if (t->priv) {
            switch (p) {
            case 1: t->app_cursor = on; break;
            case 6:
                t->origin = on;
                move_to(t, 0, 0);
                break;
            case 7: t->autowrap = on; break;
            case 25:
                t->cursor_visible = on;
                t->dirty[t->cr] = true;
                break;
            }
        } else if (p == 4) {
            t->insert = on;
        } else if (p == 20) {
            t->newline = on;
        }
    }
}

static void erase_display(term *t, int how)
{
    if (how == 2) {
        clear_screen(t);
        return;
    }
    if (how == 0) {
        clear_cells(t, t->cr, t->cc, t->cols);
        for (int r = t->cr + 1; r < t->rows; r++)
            clear_cells(t, r, 0, t->cols);
    } else if (how == 1) {
        for (int r = 0; r < t->cr; r++)
            clear_cells(t, r, 0, t->cols);
        clear_cells(t, t->cr, 0, t->cc + 1);
    }
}

static void csi(term *t, uint8_t f)
{
    int n = param(t, 0, 1), r = t->cr, c = t->cc;
    int lo = t->cr >= t->top ? t->top : 0, hi = t->cr <= t->bottom ? t->bottom : t->rows - 1;
    switch (f) {
    case '@': /* ICH */
        if (n > t->cols - c)
            n = t->cols - c;
        memmove(at(t, r, c + n), at(t, r, c), sizeof(cell) * (size_t)(t->cols - c - n));
        clear_cells(t, r, c, c + n);
        break;
    case 'A': t->cr = r - n < lo ? lo : r - n; t->wrap_pending = false; break;
    case 'B': t->cr = r + n > hi ? hi : r + n; t->wrap_pending = false; break;
    case 'C': t->cc = c + n >= t->cols ? t->cols - 1 : c + n; t->wrap_pending = false; break;
    case 'D': t->cc = c - n < 0 ? 0 : c - n; t->wrap_pending = false; break;
    case 'E': t->cr = r + n > hi ? hi : r + n; t->cc = 0; t->wrap_pending = false; break;
    case 'F': t->cr = r - n < lo ? lo : r - n; t->cc = 0; t->wrap_pending = false; break;
    case 'G': case '`': t->cc = n - 1 >= t->cols ? t->cols - 1 : n - 1; t->wrap_pending = false; break;
    case 'd': move_to(t, n - 1 - (t->origin ? 0 : 0), t->cc); break;
    case 'H': case 'f': move_to(t, param(t, 0, 1) - 1, param(t, 1, 1) - 1); break;
    case 'J': erase_display(t, t->nparams ? t->params[0] : 0); break;
    case 'K': {
        int how = t->nparams ? t->params[0] : 0;
        clear_cells(t, r, how == 0 ? c : 0, how == 1 ? c + 1 : t->cols);
        break;
    }
    case 'L': /* IL */
        if (r >= t->top && r <= t->bottom)
            scroll_down(t, r, t->bottom, n);
        t->cc = 0;
        break;
    case 'M': /* DL */
        if (r >= t->top && r <= t->bottom) {
            if (n > t->bottom - r + 1)
                n = t->bottom - r + 1;
            memmove(at(t, r, 0), at(t, r + n, 0), sizeof(cell) * (size_t)((t->bottom - r + 1 - n) * t->cols));
            for (int i = t->bottom - n + 1; i <= t->bottom; i++)
                clear_cells(t, i, 0, t->cols);
            for (int i = r; i <= t->bottom; i++)
                t->dirty[i] = true;
        }
        t->cc = 0;
        break;
    case 'P': /* DCH */
        if (n > t->cols - c)
            n = t->cols - c;
        memmove(at(t, r, c), at(t, r, c + n), sizeof(cell) * (size_t)(t->cols - c - n));
        clear_cells(t, r, t->cols - n, t->cols);
        break;
    case 'S': scroll_up(t, t->top, t->bottom, n); break;
    case 'T': scroll_down(t, t->top, t->bottom, n); break;
    case 'X': clear_cells(t, r, c, c + n > t->cols ? t->cols : c + n); break;
    case 'c': /* DA: a VT102 */
        if (!t->priv && param(t, 0, 0) == 0)
            send_str(t, "\033[?6c");
        break;
    case 'g':
        if (param(t, 0, 0) == 0)
            t->tabs[c] = false;
        else if (param(t, 0, 0) == 3)
            memset(t->tabs, 0, sizeof t->tabs);
        break;
    case 'h': set_mode(t, true); break;
    case 'l': set_mode(t, false); break;
    case 'm': sgr(t); break;
    case 'n':
        if (param(t, 0, 0) == 5) {
            send_str(t, "\033[0n");
        } else if (param(t, 0, 0) == 6) {
            char s[32];
            snprintf(s, sizeof s, "\033[%d;%dR", t->cr - (t->origin ? t->top : 0) + 1, t->cc + 1);
            send_str(t, s);
        }
        break;
    case 'r': { /* DECSTBM */
        int top = param(t, 0, 1) - 1, bottom = param(t, 1, t->rows) - 1;
        if (bottom >= t->rows)
            bottom = t->rows - 1;
        if (top < bottom) {
            t->top = top;
            t->bottom = bottom;
            move_to(t, 0, 0);
        }
        break;
    }
    case 's': save_cursor(t); break;
    case 'u': restore_cursor(t); break;
    }
}

static void vt_byte(term *t, uint8_t b)
{
    if (b == 24 || b == 26) { /* CAN, SUB: the sequence abandoned */
        t->pstate = P_GROUND;
        return;
    }
    if (b == 27) {
        t->pstate = P_ESC;
        return;
    }
    if (b < 32) {
        control(t, b);
        return;
    }
    switch (t->pstate) {
    case P_GROUND:
        if (b != 127)
            put_char(t, b);
        break;
    case P_ESC:
        t->pstate = P_GROUND;
        switch (b) {
        case '[':
            t->pstate = P_CSI;
            t->nparams = 0;
            t->priv = false;
            memset(t->params, 0, sizeof t->params);
            break;
        case '(': t->pstate = P_SCS0; break;
        case ')': t->pstate = P_SCS1; break;
        case '#': t->pstate = P_HASH; break;
        case '7': save_cursor(t); break;
        case '8': restore_cursor(t); break;
        case 'D': line_feed(t); break;
        case 'E':
            t->cc = 0;
            line_feed(t);
            break;
        case 'M': reverse_index(t); break;
        case 'H': t->tabs[t->cc] = true; break;
        case 'c':
            reset_state(t);
            clear_screen(t);
            break;
        case '=': t->app_keypad = true; break;
        case '>': t->app_keypad = false; break;
        case 'Z': send_str(t, "\033[?6c"); break;
        }
        break;
    case P_CSI:
        if (b >= '0' && b <= '9') {
            if (!t->nparams)
                t->nparams = 1;
            int *p = &t->params[t->nparams - 1];
            if (*p < 10000)
                *p = *p * 10 + (b - '0');
        } else if (b == ';') {
            if (!t->nparams)
                t->nparams = 1;
            if (t->nparams < 16)
                t->params[t->nparams++] = 0;
        } else if (b == '?') {
            t->priv = true;
        } else if (b >= 0x40 && b <= 0x7E) {
            t->pstate = P_GROUND;
            csi(t, b);
        }
        break;
    case P_SCS0:
    case P_SCS1:
        t->charset[t->pstate == P_SCS1] = b == '0' || b == '2';
        t->pstate = P_GROUND;
        break;
    case P_HASH:
        if (b == '8') /* DECALN: the screen full of Es */
            for (int r = 0; r < t->rows; r++) {
                for (int c = 0; c < t->cols; c++)
                    *at(t, r, c) = (cell){ 'E', 0, COLOR_DEFAULT, COLOR_DEFAULT };
                t->dirty[r] = true;
            }
        t->pstate = P_GROUND;
        break;
    }
}

static void tty_byte(term *t, uint8_t b)
{
    if (b < 32)
        control(t, b == 14 || b == 15 || b == 5 ? 0 : b);
    else if (b != 127)
        put_char(t, b);
}

/* ---------------------------------------------------------------------- */
/* Searches */

static void run_searches(term *t, const bool *changed)
{
    for (int k = 0; k < t->nsearch; k++) {
        tm_search *s = &t->search[k];
        qrect w = s->where;
        int r0 = 0, r1 = t->rows, c0 = 0, c1 = t->cols;
        if (!qrect_empty(w)) {
            r0 = w.top > 1 ? w.top - 1 : 0;
            r1 = w.bottom < t->rows ? w.bottom : t->rows;
            c0 = w.left > 1 ? w.left - 1 : 0;
            c1 = w.right < t->cols ? w.right : t->cols;
        }
        for (int r = r0; r < r1; r++) {
            if (!changed[r])
                continue;
            for (int c = c0; c + s->len <= c1; c++) {
                bool hit = s->len > 0;
                for (int j = 0; j < s->len && hit; j++) {
                    uint8_t a = at(t, r, c + j)->ch, b = s->s[j];
                    hit = s->nocase ? tolower(a) == tolower(b) : a == b;
                }
                if (!hit)
                    continue;
                gaddr fr = t->scratch + 240;
                qrect_put(fr, (qrect){ (int16_t)(r + 1), (int16_t)(c + 1), (int16_t)(r + 1),
                                       (int16_t)(c + 1 + s->len) });
                uint32_t args[3] = { t->h, (uint32_t)s->id, fr };
                ctb_callback(s->callback, PI_SEARCH, args, 3);
                if (!term_of(t->h) || k >= t->nsearch)
                    return;
            }
        }
    }
}

/* ---------------------------------------------------------------------- */
/* The manager's calls */

static void tm_config_changed(gaddr h)
{
    term *t = term_of(h);
    if (!t)
        return;
    resize(t);
    t->autowrap = t->tool == T_TTY || rd8(cfg(t) + C_WRAP);
    t->newline = rd8(cfg(t) + C_NEWLINE) != 0;
    metrics(t);
    dirty_all(t);
    paint(t);
}

uint32_t tm_init(uint16_t sel, const uint32_t *a) { return tmNoErr; }

/* (termRect, viewRect, flags, procID, owner, sendProc, cacheProc, breakProc, clikLoop, environsProc, refCon,
 *  userData) */
uint32_t tm_new(uint16_t sel, const uint32_t *a)
{
    int16_t id = (int16_t)a[3];
    if (!ctb_tool_of(MGR_TM, id))
        return 0;
    int slot = -1;
    for (int i = 0; i < MAX_TERMS && slot < 0; i++)
        if (!terms[i].used)
            slot = i;
    if (slot < 0)
        return 0;
    gaddr h = tb_NewHandle(TermRecord_SIZEOF, false, true);
    if (!h)
        return 0;
    ctb_record_init(h, MGR_TM, id, a[2], (int32_t)a[10], (int32_t)a[11]);
    term *t = &terms[slot];
    memset(t, 0, sizeof *t);
    t->used = true;
    t->h = h;
    t->tool = id;
    t->scratch = tb_NewPtr(512, false, true);
    t->next_search = 1;
    t->active = true;
    t->drawn_cr = -1;
    gaddr r = rd32(h);
    wr32(r + TermRecord_tmPrivate_off, (uint32_t)slot + 1);
    wr32(r + TermRecord_owner_off, a[4]);
    wr32(r + TermRecord_sendProc_off, a[5]);
    wr32(r + TermRecord_cacheProc_off, a[6]);
    wr32(r + TermRecord_breakProc_off, a[7]);
    wr32(r + TermRecord_clikLoop_off, a[8]);
    wr32(r + TermRecord_environsProc_off, a[9]);
    if (a[0])
        gmemcpy(r + TermRecord_termRect_off, a[0], 8);
    if (a[1]) {
        gmemcpy(r + TermRecord_viewRect_off, a[1], 8);
        gmemcpy(r + TermRecord_visRect_off, a[1], 8);
    }
    wr16(r + TermRecord_selType_off, selTextNormal);
    resize(t);
    reset_state(t);
    metrics(t);
    return h;
}

static void free_term(term *t)
{
    free(t->screen);
    t->screen = NULL;
    if (t->scratch)
        tb_DisposePtr(t->scratch);
    if (t->line)
        tb_DisposePtr(t->line);
    if (t->cache_h)
        tb_DisposeHandle(t->cache_h);
    t->used = false;
    ctb_record_free(t->h);
}

uint32_t tm_dispose(uint16_t sel, const uint32_t *a)
{
    term *t = term_of(a[0]);
    if (t)
        free_term(t);
    return 0;
}

/* Choose picked another tool: a new terminal in the same place. */
static gaddr tm_renew(gaddr h, int16_t proc_id)
{
    gaddr r = rd32(h);
    gaddr rects = tb_NewPtr(16, false, false);
    gmemcpy(rects, r + TermRecord_termRect_off, 8);
    gmemcpy(rects + 8, r + TermRecord_viewRect_off, 8);
    r = rd32(h);
    uint32_t args[12] = { rects, rects + 8, rd32(r + TermRecord_flags_off), (uint32_t)proc_id,
                          rd32(r + TermRecord_owner_off), rd32(r + TermRecord_sendProc_off),
                          rd32(r + TermRecord_cacheProc_off), rd32(r + TermRecord_breakProc_off),
                          rd32(r + TermRecord_clikLoop_off), rd32(r + TermRecord_environsProc_off),
                          rd32(r + TermRecord_refCon_off), rd32(r + TermRecord_userData_off) };
    gaddr nh = tm_new(0x0302, args);
    tb_DisposePtr(rects);
    if (nh) {
        term *t = term_of(h);
        if (t)
            free_term(t);
    }
    return nh;
}

static void tm_dispose_record(gaddr h)
{
    term *t = term_of(h);
    if (t)
        free_term(t);
}

const ctb_manager tm_manager = { tm_tools, COUNT(tm_tools), tm_config_changed, tm_renew, tm_dispose_record };

/* (hTerm, buffer, length, flags): what was taken */
uint32_t tm_stream(uint16_t sel, const uint32_t *a)
{
    term *t = term_of(a[0]);
    if (!t || !a[1])
        return 0;
    int32_t n = (int32_t)a[2];
    bool before[MAX_ROWS];
    memcpy(before, t->dirty, sizeof before);
    memset(t->dirty, 0, sizeof t->dirty);
    for (int32_t i = 0; i < n; i++) {
        uint8_t b = rd8(a[1] + (uint32_t)i);
        if (t->tool == T_VT102)
            vt_byte(t, b);
        else
            tty_byte(t, b);
    }
    bool changed[MAX_ROWS];
    memcpy(changed, t->dirty, sizeof changed);
    for (int r = 0; r < MAX_ROWS; r++)
        t->dirty[r] |= before[r];
    if (t->nsearch)
        run_searches(t, changed);
    t = term_of(a[0]);
    if (t)
        paint(t);
    return (uint32_t)(n < 0 ? 0 : n);
}

static void stream_local(term *t, const uint8_t *b, int n)
{
    gaddr buf = t->scratch + 300;
    gput(buf, b, (uint32_t)n);
    uint32_t args[4] = { t->h, buf, (uint32_t)n, 0 };
    tm_stream(0x030A, args);
}

/* What a key sends; NULL for nothing. */
static int key_bytes(term *t, int key, char *out)
{
    bool vt = t->tool == T_VT102;
    if (key <= K_LEFT) {
        if (!vt)
            return 0;
        return snprintf(out, 8, "\033%c%c", t->app_cursor ? 'O' : '[', "ABCD"[key]);
    }
    if (key >= K_PF1 && key < K_PF1 + 4)
        return vt ? snprintf(out, 8, "\033O%c", 'P' + (key - K_PF1)) : 0;
    if (key == K_BREAK)
        return 0;
    static const char plain[] = "0123456789-,.\r";
    char ch = plain[key - K_KP0];
    if (vt && t->app_keypad)
        return snprintf(out, 8, "\033O%c", key == K_ENTER ? 'M' : key == K_MINUS ? 'm' : key == K_COMMA ? 'l'
                                                                              : key == K_PERIOD ? 'n'
                                                                                                : 'p' + (key - K_KP0));
    out[0] = ch;
    return 1;
}

static void send_key(term *t, const char *b, int n)
{
    if (n <= 0)
        return;
    send(t, b, n);
    if (rd8(cfg(t) + C_ECHO))
        stream_local(t, (const uint8_t *)b, n);
}

static void do_break(term *t)
{
    gaddr r = rec(t);
    uint32_t args[2] = { 15, rd32(r + TermRecord_refCon_off) };
    ctb_callback(rd32(r + TermRecord_breakProc_off), PI_BREAK, args, 2);
}

/* (hTerm, the key's event) */
uint32_t tm_key(uint16_t sel, const uint32_t *a)
{
    term *t = term_of(a[0]);
    if (!t || !a[1])
        return 0;
    uint32_t msg = rd32(a[1] + EventRecord_message_off);
    uint16_t mods = rd16(a[1] + EventRecord_modifiers_off);
    uint8_t ch = msg & 0xFF, code = (msg >> 8) & 0xFF;
    if (mods & cmdKey)
        return 0;
    char out[16];
    int n = 0, key = -1;
    switch (code) {
    case 0x7A: key = K_PF1; break; /* F1 to F4 */
    case 0x78: key = K_PF1 + 1; break;
    case 0x63: key = K_PF1 + 2; break;
    case 0x76: key = K_PF1 + 3; break;
    case 0x47: key = K_PF1; break; /* clear, =, / and * on the keypad */
    case 0x51: key = K_PF1 + 1; break;
    case 0x4B: key = K_PF1 + 2; break;
    case 0x43: key = K_PF1 + 3; break;
    case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x59: key = K_KP0 + code - 0x52; break;
    case 0x5B: key = K_KP0 + 8; break;
    case 0x5C: key = K_KP0 + 9; break;
    case 0x4E: key = K_MINUS; break;
    case 0x45: key = K_COMMA; break; /* keypad + */
    case 0x41: key = K_PERIOD; break;
    case 0x4C: key = K_ENTER; break;
    }
    if (key >= K_KP0 && !(t->tool == T_VT102 && t->app_keypad) && key != K_ENTER)
        key = -1; /* the keypad types its digits */
    if (key < 0) {
        switch (ch) {
        case 0x1C: key = K_LEFT; break;
        case 0x1D: key = K_RIGHT; break;
        case 0x1E: key = K_UP; break;
        case 0x1F: key = K_DOWN; break;
        }
    }
    if (key >= 0) {
        n = key_bytes(t, key, out);
        if (key == K_ENTER && n == 1 && t->newline)
            out[n++] = '\n';
    } else if (ch == '\r' || ch == 3) {
        out[n++] = '\r';
        if (t->newline)
            out[n++] = '\n';
    } else if (ch == 8) {
        out[n++] = rd16(cfg(t) + C_BS) ? 127 : 8;
    } else if (ch == 0x7F) {
        out[n++] = 127;
    } else if ((mods & controlKey) && ((ch >= '@' && ch <= '_') || (ch >= 'a' && ch <= 'z'))) {
        out[n++] = (char)(ch & 0x1F);
    } else if (ch == 0x01 || ch == 0x04 || ch == 0x0B || ch == 0x0C || ch == 0x10 || ch == 0x05) {
        n = 0; /* home, end, page up, page down, function, help */
    } else {
        out[n++] = (char)ch;
    }
    send_key(t, out, n);
    return 0;
}

/* (hTerm, theKey): false for a key the tool hasn't */
uint32_t tm_do_term_key(uint16_t sel, const uint32_t *a)
{
    term *t = term_of(a[0]);
    if (!t || !a[1])
        return 0;
    char name[64], out[16];
    size_t len = rd8(a[1]);
    if (len >= sizeof name)
        len = sizeof name - 1;
    gget(name, a[1] + 1, (uint32_t)len);
    name[len] = 0;
    int nkeys = t->tool == T_VT102 ? NKEYS : 1;
    for (int i = 0; i < nkeys; i++) {
        int key = t->tool == T_VT102 ? i : K_BREAK;
        if (strcasecmp(term_keys[key], name))
            continue;
        if (key == K_BREAK)
            do_break(t);
        else
            send_key(t, out, key_bytes(t, key, out));
        return 1;
    }
    return 0;
}

uint32_t tm_count_term_keys(uint16_t sel, const uint32_t *a)
{
    term *t = term_of(a[0]);
    return t ? (t->tool == T_VT102 ? NKEYS : 1) : 0;
}

/* (hTerm, id from 1, theKey) */
uint32_t tm_get_ind_term_key(uint16_t sel, const uint32_t *a)
{
    term *t = term_of(a[0]);
    int i = (int16_t)a[1];
    if (!a[2])
        return 0;
    wr8(a[2], 0);
    if (!t || i < 1 || i > (t->tool == T_VT102 ? NKEYS : 1))
        return 0;
    const char *s = term_keys[t->tool == T_VT102 ? i - 1 : K_BREAK];
    wr8(a[2], (uint8_t)strlen(s));
    gput(a[2] + 1, s, (uint32_t)strlen(s));
    return 0;
}

/* (hTerm, visRgn) */
uint32_t tm_update(uint16_t sel, const uint32_t *a)
{
    term *t = term_of(a[0]);
    if (t) {
        dirty_all(t);
        paint(t);
    }
    return 0;
}

/* (hTerm, theTermData, theRect): text drawn in the terminal's font */
uint32_t tm_paint(uint16_t sel, const uint32_t *a)
{
    term *t = term_of(a[0]);
    if (!t || !a[1] || !a[2] || !visible(t))
        return 0;
    gaddr data = rd32(a[1] + TermDataBlock_theData_off);
    if (!data || !rd32(data))
        return 0;
    qrect box = qrect_get(a[2]);
    gaddr s = t->scratch;
    gaddr owner = rd32(rec(t) + TermRecord_owner_off);
    tb_GetPort(s);
    gaddr saved = rd32(s);
    tb_SetPort(owner);
    int16_t font = GrafPort_txFont(owner), size = GrafPort_txSize(owner);
    tb_TextFont(4);
    tb_TextSize((int16_t)rd32(cfg(t) + C_SIZE));
    qrect_put(s, box);
    tb_EraseRect(s);
    int32_t n = tb_GetHandleSize(data);
    int y = box.top + t->ascent, start = 0;
    for (int32_t i = 0; i <= n; i++) {
        if (i < n && rd8(rd32(data) + (uint32_t)i) != '\r')
            continue;
        int len = i - start > 200 ? 200 : i - start;
        gmemcpy(s + 32, rd32(data) + (uint32_t)start, (uint32_t)len);
        tb_MoveTo(box.left, (int16_t)y);
        tb_DrawText(s + 32, 0, (int16_t)len);
        y += t->ch;
        start = i + 1;
    }
    tb_TextFont(font);
    tb_TextSize(size);
    tb_SetPort(saved);
    return 0;
}

/* (hTerm, activate) */
uint32_t tm_activate(uint16_t sel, const uint32_t *a)
{
    term *t = term_of(a[0]);
    if (t) {
        t->active = a[1] != 0;
        t->dirty[t->cr] = true;
        paint(t);
    }
    return 0;
}

uint32_t tm_idle(uint16_t sel, const uint32_t *a)
{
    term *t = term_of(a[0]);
    if (t)
        paint(t);
    return 0;
}

uint32_t tm_reset(uint16_t sel, const uint32_t *a)
{
    term *t = term_of(a[0]);
    if (t) {
        reset_state(t);
        clear_screen(t);
        paint(t);
    }
    return 0;
}

uint32_t tm_clear(uint16_t sel, const uint32_t *a)
{
    term *t = term_of(a[0]);
    if (t) {
        clear_screen(t);
        t->cr = t->cc = 0;
        t->wrap_pending = false;
        paint(t);
    }
    return 0;
}

/* (hTerm, newViewRect) */
uint32_t tm_resize(uint16_t sel, const uint32_t *a)
{
    term *t = term_of(a[0]);
    if (!t || !a[1])
        return 0;
    gmemcpy(rec(t) + TermRecord_viewRect_off, a[1], 8);
    gmemcpy(rec(t) + TermRecord_visRect_off, a[1], 8);
    dirty_all(t);
    paint(t);
    return 0;
}

/* (hTerm, dh, dv): there is nothing off the screen to scroll to */
uint32_t tm_scroll(uint16_t sel, const uint32_t *a)
{
    return tm_update(sel, a);
}

static void store_selection(term *t)
{
    gaddr r = rec(t);
    qrect q = t->sel ? (qrect){ (int16_t)(t->sr0 + 1), (int16_t)(t->sc0 + 1), (int16_t)(t->sr1 + 1),
                                (int16_t)(t->sc1 + 1) }
                     : (qrect){ 0, 0, 0, 0 };
    qrect_put(r + TermRecord_selection_off, q);
    wr16(r + TermRecord_selType_off, selTextNormal);
}

/* A cell under a local point, clamped to the screen. */
static void cell_at(term *t, int h, int v, int *r, int *c)
{
    qrect tr = term_rect(t);
    *r = (v - tr.top) / t->ch;
    *c = (h - tr.left + t->cw / 2) / t->cw;
    *r = *r < 0 ? 0 : *r >= t->rows ? t->rows - 1 : *r;
    *c = *c < 0 ? 0 : *c > t->cols ? t->cols : *c;
}

static void select_cells(term *t, int ar, int ac, int r, int c)
{
    int a = ar * t->cols + ac, b = r * t->cols + c;
    if (b < a) {
        int k = a;
        a = b;
        b = k;
    }
    t->sel = a != b;
    t->sr0 = a / t->cols;
    t->sc0 = a % t->cols;
    t->sr1 = b / t->cols;
    t->sc1 = b % t->cols;
    dirty_all(t);
    paint(t);
}

/* (hTerm, the mouse-down event): a selection dragged out */
uint32_t tm_click(uint16_t sel, const uint32_t *a)
{
    term *t = term_of(a[0]);
    if (!t || !a[1] || !visible(t))
        return 0;
    gaddr s = t->scratch;
    gaddr owner = rd32(rec(t) + TermRecord_owner_off);
    tb_GetPort(s);
    gaddr saved = rd32(s);
    tb_SetPort(owner);
    wr32(s + 4, rd32(a[1] + EventRecord_where_off));
    tb_GlobalToLocal(s + 4);
    int ar, ac, r, c;
    cell_at(t, pt_h(rd32(s + 4)), pt_v(rd32(s + 4)), &ar, &ac);
    select_cells(t, ar, ac, ar, ac);
    tb_SetPort(owner);
    while (tb_StillDown()) {
        gaddr loop = rd32(rec(t) + TermRecord_clikLoop_off);
        if (loop) {
            uint32_t args[1] = { rd32(rec(t) + TermRecord_refCon_off) };
            if (!(ctb_callback(loop, PI_CLIKLOOP, args, 1) & 0xFF))
                break;
            t = term_of(a[0]);
            if (!t)
                return 0;
            tb_SetPort(owner);
        }
        tb_GetMouse(s + 4);
        cell_at(t, pt_h(rd32(s + 4)), pt_v(rd32(s + 4)), &r, &c);
        select_cells(t, ar, ac, r, c);
        tb_SetPort(owner);
    }
    store_selection(t);
    tb_SetPort(saved);
    return 0;
}

/* (hTerm, theData, &theType): the selection's text, lines ending in CR */
uint32_t tm_get_select(uint16_t sel, const uint32_t *a)
{
    term *t = term_of(a[0]);
    if (!t || !t->sel || !a[1])
        return 0;
    char *out = malloc((size_t)(t->rows * (t->cols + 1)));
    int n = 0;
    for (int r = t->sr0; r <= t->sr1 && r < t->rows; r++) {
        int c0 = r == t->sr0 ? t->sc0 : 0, c1 = r == t->sr1 ? t->sc1 : t->cols;
        int e = c1;
        while (e > c0 && at(t, r, e - 1)->ch == ' ')
            e--;
        for (int c = c0; c < e; c++)
            out[n++] = (char)at(t, r, c)->ch;
        if (r < t->sr1)
            out[n++] = '\r';
    }
    tb_SetHandleSize(a[1], n);
    if (n)
        gput(rd32(a[1]), out, (uint32_t)n);
    free(out);
    if (a[2])
        wr32(a[2], fourcc("TEXT"));
    return (uint32_t)n;
}

/* (hTerm, &selection, selType): rows and columns from 1 */
uint32_t tm_set_selection(uint16_t sel, const uint32_t *a)
{
    term *t = term_of(a[0]);
    if (!t || !a[1])
        return 0;
    qrect q = qrect_get(a[1]);
    int r0 = q.top - 1, c0 = q.left - 1, r1 = q.bottom - 1, c1 = q.right - 1;
    r0 = r0 < 0 ? 0 : r0 >= t->rows ? t->rows - 1 : r0;
    r1 = r1 < 0 ? 0 : r1 >= t->rows ? t->rows - 1 : r1;
    c0 = c0 < 0 ? 0 : c0 > t->cols ? t->cols : c0;
    c1 = c1 < 0 ? 0 : c1 > t->cols ? t->cols : c1;
    select_cells(t, r0, c0, r1, c1);
    store_selection(t);
    return 0;
}

/* (hTerm, lineNo from 1, &theTermData): the line's text, its attributes in
 * auxData when there is one */
uint32_t tm_get_line(uint16_t sel, const uint32_t *a)
{
    term *t = term_of(a[0]);
    int line = (int16_t)a[1] - 1;
    if (!t || !a[2] || line < 0 || line >= t->rows)
        return 0;
    gaddr b = a[2];
    wr16(b + TermDataBlock_flags_off, tmTextTerminal);
    gaddr data = rd32(b + TermDataBlock_theData_off);
    if (!data) {
        data = tb_NewHandle(t->cols, false, false);
        wr32(b + TermDataBlock_theData_off, data);
    } else {
        tb_SetHandleSize(data, t->cols);
    }
    for (int c = 0; c < t->cols; c++)
        wr8(rd32(data) + (uint32_t)c, at(t, line, c)->ch);
    gaddr aux = rd32(b + TermDataBlock_auxData_off);
    if (aux) {
        tb_SetHandleSize(aux, t->cols);
        for (int c = 0; c < t->cols; c++)
            wr8(rd32(aux) + (uint32_t)c, at(t, line, c)->attr);
    }
    return 0;
}

/* (hTerm, lineNo, &LineDataBlock: dataPtr, dataSize, attrPtr, attrSize) */
uint32_t tm_peek_line(uint16_t sel, const uint32_t *a)
{
    term *t = term_of(a[0]);
    int line = (int16_t)a[1] - 1;
    if (!t || !a[2] || line < 0 || line >= t->rows)
        return 0;
    if (!t->line)
        t->line = tb_NewPtr(2 * MAX_COLS, false, true);
    for (int c = 0; c < t->cols; c++) {
        wr8(t->line + (uint32_t)c, at(t, line, c)->ch);
        wr8(t->line + MAX_COLS + (uint32_t)c, at(t, line, c)->attr);
    }
    wr32(a[2], t->line);
    wr32(a[2] + 4, (uint32_t)t->cols);
    wr32(a[2] + 8, t->line + MAX_COLS);
    wr32(a[2] + 12, (uint32_t)t->cols);
    return 0;
}

/* (hTerm, cursType): row and column from 1, or the cell's corner */
uint32_t tm_get_cursor(uint16_t sel, const uint32_t *a)
{
    term *t = term_of(a[0]);
    if (!t)
        return 0;
    if ((int16_t)a[1] == cursorGraphics) {
        qrect tr = term_rect(t);
        return make_pt((int16_t)(tr.top + t->cr * t->ch), (int16_t)(tr.left + t->cc * t->cw));
    }
    return make_pt((int16_t)(t->cr + 1), (int16_t)(t->cc + 1));
}

/* (hTerm, &environs) */
uint32_t tm_get_environs(uint16_t sel, const uint32_t *a)
{
    term *t = term_of(a[0]);
    if (!t || !a[1])
        return (uint32_t)tmGenericError;
    gaddr e = a[1];
    gmemset(e, 0, TermEnvironRec_SIZEOF);
    wr16(e + TermEnvironRec_termType_off, tmTextTerminal);
    wr16(e + TermEnvironRec_textRows_off, (uint16_t)t->rows);
    wr16(e + TermEnvironRec_textCols_off, (uint16_t)t->cols);
    wr32(e + TermEnvironRec_cellSize_off, make_pt((int16_t)t->ch, (int16_t)t->cw));
    return tmNoErr;
}

/* (hTerm, string, where, searchType, callBack): the search's number */
uint32_t tm_add_search(uint16_t sel, const uint32_t *a)
{
    term *t = term_of(a[0]);
    if (!t || !a[1] || t->nsearch == MAX_SEARCH)
        return (uint32_t)-1;
    tm_search *s = &t->search[t->nsearch++];
    s->id = t->next_search++;
    s->len = rd8(a[1]);
    gget(s->s, a[1] + 1, (uint32_t)s->len);
    s->where = a[2] ? qrect_get(a[2]) : (qrect){ 0, 0, 0, 0 };
    s->nocase = ((uint16_t)a[3] & tmSearchNoCase) != 0;
    s->callback = a[4];
    return (uint32_t)s->id;
}

/* (hTerm, refnum) */
uint32_t tm_remove_search(uint16_t sel, const uint32_t *a)
{
    term *t = term_of(a[0]);
    for (int i = 0; t && i < t->nsearch; i++)
        if (t->search[i].id == (int16_t)a[1]) {
            memmove(&t->search[i], &t->search[i + 1], sizeof t->search[0] * (size_t)(t->nsearch - i - 1));
            t->nsearch--;
            break;
        }
    return 0;
}

uint32_t tm_clear_search(uint16_t sel, const uint32_t *a)
{
    term *t = term_of(a[0]);
    if (t)
        t->nsearch = 0;
    return 0;
}

void tm_reset_all(void)
{
    for (int i = 0; i < MAX_TERMS; i++) {
        free(terms[i].screen);
        memset(&terms[i], 0, sizeof terms[i]);
    }
}
