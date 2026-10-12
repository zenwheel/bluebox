/*
 * List Manager (Pack0).
 *
 * Lists keep the original layout: a ListRec whose cellArray holds, for each
 * cell in row-major order, the offset of its data in the cells handle (bit
 * 15 set when the cell is selected), followed by the end offset.  Changes
 * that move cells around read the grid into host memory, edit it and write
 * it back.
 *
 * Cells are drawn by the list's LDEF, called like the ROM did; LDEF 0 is
 * native (see ldef0_native) and draws each cell's data as text.  Scroll bars
 * are ordinary scroll bar controls outside rView.
 */
#include <stdlib.h>
#include <string.h>

#include "m68k.h"
#include "qd_internal.h"
#include "tb_internal.h"
#include "trap.h"

enum { lInitMsg = 0, lDrawMsg, lHiliteMsg, lCloseMsg };
enum { lOnlyOne = 0x80, lExtendDrag = 0x40, lNoDisjoint = 0x20, lNoExtend = 0x10, lNoRect = 0x08,
       lUseSense = 0x04, lNoNilHilite = 0x02 };
enum { lDoVAutoscroll = 2, lDoHAutoscroll = 1 };
#define NO_DRAW 0x01 /* in lReserved: LSetDrawingMode(false) */
#define SELECTED 0x8000

static gaddr lp(gaddr list) { return rd32(list); }
static qrect bounds_of(gaddr list) { return qrect_get(lp(list) + ListRec_dataBounds_off); }
static qrect view_of(gaddr list) { return qrect_get(lp(list) + ListRec_rView_off); }
static qrect visible_of(gaddr list) { return qrect_get(lp(list) + ListRec_visible_off); }
static int ncols(gaddr list) { qrect b = bounds_of(list); return b.right - b.left; }
static int nrows(gaddr list) { qrect b = bounds_of(list); return b.bottom - b.top; }
static int cell_h(gaddr list) { return (int16_t)rd16(lp(list) + ListRec_cellSize_off); }
static int cell_w(gaddr list) { return (int16_t)rd16(lp(list) + ListRec_cellSize_off + 2); }
static bool drawing(gaddr list) { return !(rd8(lp(list) + ListRec_lReserved_off) & NO_DRAW); }

static gaddr array_entry(gaddr list, int i) { return lp(list) + ListRec_cellArray_off + (uint32_t)i * 2; }

/* Index of a cell, or -1 when it is outside the data bounds. */
static int index_of(gaddr list, uint32_t cell)
{
    qrect b = bounds_of(list);
    int v = pt_v(cell), h = pt_h(cell);
    if (v < b.top || v >= b.bottom || h < b.left || h >= b.right)
        return -1;
    return (v - b.top) * (b.right - b.left) + (h - b.left);
}

static uint32_t cell_at(gaddr list, int i)
{
    qrect b = bounds_of(list);
    int w = b.right - b.left;
    return make_pt((int16_t)(b.top + i / w), (int16_t)(b.left + i % w));
}

static void data_range(gaddr list, int i, uint32_t *off, uint32_t *len)
{
    uint32_t a = rd16(array_entry(list, i)) & 0x7FFF, b = rd16(array_entry(list, i + 1)) & 0x7FFF;
    *off = a;
    *len = b > a ? b - a : 0;
}

static bool is_selected(gaddr list, int i) { return rd16(array_entry(list, i)) & SELECTED; }

/* ---------------------------------------------------------------------- */
/* The grid in host memory */

typedef struct {
    uint8_t *data;
    uint16_t len;
    bool sel;
} hcell;

typedef struct {
    int rows, cols;
    hcell *c;
} grid;

static void grid_load(gaddr list, grid *g)
{
    g->rows = nrows(list);
    g->cols = ncols(list);
    int n = g->rows * g->cols;
    g->c = calloc((size_t)(n ? n : 1), sizeof *g->c);
    gaddr cells = rd32(lp(list) + ListRec_cells_off);
    for (int i = 0; i < n; i++) {
        uint32_t off, len;
        data_range(list, i, &off, &len);
        g->c[i].sel = is_selected(list, i);
        g->c[i].len = (uint16_t)len;
        if (len) {
            g->c[i].data = malloc(len);
            gget(g->c[i].data, rd32(cells) + off, len);
        }
    }
}

static void grid_free(grid *g)
{
    for (int i = 0; i < g->rows * g->cols; i++)
        free(g->c[i].data);
    free(g->c);
}

/* Write the grid back; the data bounds' top-left corner is kept. */
static void grid_store(gaddr list, grid *g)
{
    qrect b = bounds_of(list);
    b.bottom = (int16_t)(b.top + g->rows);
    b.right = (int16_t)(b.left + g->cols);
    int n = g->rows * g->cols;
    tb_SetHandleSize(list, (int32_t)(ListRec_cellArray_off + (uint32_t)(n + 1) * 2));
    qrect_put(lp(list) + ListRec_dataBounds_off, b);
    uint32_t total = 0;
    for (int i = 0; i < n; i++)
        total += g->c[i].len;
    gaddr cells = rd32(lp(list) + ListRec_cells_off);
    tb_SetHandleSize(cells, (int32_t)total);
    uint32_t off = 0;
    for (int i = 0; i < n; i++) {
        wr16(array_entry(list, i), (uint16_t)(off | (g->c[i].sel ? SELECTED : 0)));
        if (g->c[i].len)
            gput(rd32(cells) + off, g->c[i].data, g->c[i].len);
        off += g->c[i].len;
    }
    wr16(array_entry(list, n), (uint16_t)off);
    wr16(lp(list) + ListRec_maxIndex_off, (uint16_t)(n * 2));
}

/* ---------------------------------------------------------------------- */
/* Geometry and scrolling */

static int rows_shown(gaddr list)
{
    qrect v = view_of(list);
    int h = cell_h(list);
    return h > 0 ? (v.bottom - v.top + h - 1) / h : 0;
}

static int cols_shown(gaddr list)
{
    qrect v = view_of(list);
    int w = cell_w(list);
    return w > 0 ? (v.right - v.left + w - 1) / w : 0;
}

/* Fully visible rows and columns: how far scrolling can go. */
static int rows_full(gaddr list)
{
    qrect v = view_of(list);
    int h = cell_h(list);
    return h > 0 ? (v.bottom - v.top) / h : 0;
}

static int cols_full(gaddr list)
{
    qrect v = view_of(list);
    int w = cell_w(list);
    return w > 0 ? (v.right - v.left) / w : 0;
}

static gaddr vscroll(gaddr list) { return rd32(lp(list) + ListRec_vScroll_off); }
static gaddr hscroll(gaddr list) { return rd32(lp(list) + ListRec_hScroll_off); }

/* Keep visible inside the data and the scroll bars in step with it. */
static void set_visible(gaddr list, int top, int left)
{
    qrect b = bounds_of(list);
    int maxtop = b.bottom - rows_full(list), maxleft = b.right - cols_full(list);
    if (top > maxtop)
        top = maxtop;
    if (top < b.top)
        top = b.top;
    if (left > maxleft)
        left = maxleft;
    if (left < b.left)
        left = b.left;
    qrect vis = { (int16_t)top, (int16_t)left, (int16_t)(top + rows_shown(list)), (int16_t)(left + cols_shown(list)) };
    qrect_put(lp(list) + ListRec_visible_off, vis);
    gaddr vs = vscroll(list), hs = hscroll(list);
    if (vs) {
        int max = b.bottom - rows_full(list) - b.top;
        tb_SetControlMaximum(vs, (int16_t)(max > 0 ? max : 0));
        tb_SetControlValue(vs, (int16_t)(top - b.top));
    }
    if (hs) {
        int max = b.right - cols_full(list) - b.left;
        tb_SetControlMaximum(hs, (int16_t)(max > 0 ? max : 0));
        tb_SetControlValue(hs, (int16_t)(left - b.left));
    }
}

static qrect cell_rect(gaddr list, uint32_t cell)
{
    qrect v = view_of(list), vis = visible_of(list);
    int h = cell_h(list), w = cell_w(list);
    int top = v.top + (pt_v(cell) - vis.top) * h, left = v.left + (pt_h(cell) - vis.left) * w;
    return (qrect){ (int16_t)top, (int16_t)left, (int16_t)(top + h), (int16_t)(left + w) };
}

/* ---------------------------------------------------------------------- */
/* Calling the LDEF, in the list's port and clipped to rView */

typedef struct {
    gaddr port, clip;
} port_state;

static port_state enter(gaddr list)
{
    port_state s;
    gaddr pp = tb_NewPtr(4, false, false);
    tb_GetPort(pp);
    s.port = rd32(pp);
    tb_DisposePtr(pp);
    tb_SetPort(rd32(lp(list) + ListRec_port_off));
    s.clip = tb_NewRgn();
    tb_GetClip(s.clip);
    gaddr view = tb_NewRgn(), r = tb_NewPtr(8, false, false);
    qrect_put(r, view_of(list));
    tb_RectRgn(view, r);
    tb_SectRgn(view, s.clip, view);
    tb_SetClip(view);
    tb_DisposeRgn(view);
    tb_DisposePtr(r);
    return s;
}

static void leave(port_state s)
{
    tb_SetClip(s.clip);
    tb_DisposeRgn(s.clip);
    tb_SetPort(s.port);
}

static void call_ldef(gaddr list, int16_t msg, bool sel, qrect r, uint32_t cell, uint32_t off, uint32_t len)
{
    gaddr defh = rd32(lp(list) + ListRec_listDefProc_off);
    if (!defh)
        return;
    if (!rd32(defh))
        tb_LoadResource(defh);
    if (!rd32(defh))
        return;
    cpu68k *c = g_cpu;
    gaddr rect = tb_NewPtr(8, false, false);
    qrect_put(rect, r);
    uint8_t state = tb_HGetState(defh);
    tb_HLock(defh);
    tb_HLock(list);
    cpu_push16(c, (uint16_t)msg);
    cpu_push16(c, sel ? 0x0100 : 0);
    cpu_push32(c, rect);
    cpu_push32(c, cell);
    cpu_push16(c, (uint16_t)off);
    cpu_push16(c, (uint16_t)len);
    cpu_push32(c, list);
    guest_call(rd32(defh));
    tb_HUnlock(list);
    tb_HSetState(defh, state);
    tb_DisposePtr(rect);
}

static void draw_cell(gaddr list, int i)
{
    uint32_t cell = cell_at(list, i), off, len;
    qrect vis = visible_of(list);
    if (pt_v(cell) < vis.top || pt_v(cell) >= vis.bottom || pt_h(cell) < vis.left || pt_h(cell) >= vis.right)
        return;
    data_range(list, i, &off, &len);
    call_ldef(list, lDrawMsg, is_selected(list, i), cell_rect(list, cell), cell, off, len);
}

static void hilite_cell(gaddr list, int i)
{
    uint32_t cell = cell_at(list, i), off, len;
    qrect vis = visible_of(list);
    if (pt_v(cell) < vis.top || pt_v(cell) >= vis.bottom || pt_h(cell) < vis.left || pt_h(cell) >= vis.right)
        return;
    data_range(list, i, &off, &len);
    call_ldef(list, lHiliteMsg, is_selected(list, i), cell_rect(list, cell), cell, off, len);
}

/* Erase rView and draw every visible cell. */
static void draw_all(gaddr list)
{
    if (!drawing(list))
        return;
    port_state s = enter(list);
    gaddr r = tb_NewPtr(8, false, false);
    qrect_put(r, view_of(list));
    tb_EraseRect(r);
    tb_DisposePtr(r);
    int n = nrows(list) * ncols(list);
    for (int i = 0; i < n; i++)
        draw_cell(list, i);
    leave(s);
}

static void redraw_one(gaddr list, int i, bool hilite_only)
{
    if (!drawing(list))
        return;
    port_state s = enter(list);
    if (hilite_only)
        hilite_cell(list, i);
    else
        draw_cell(list, i);
    leave(s);
}

static void set_selected(gaddr list, int i, bool on)
{
    if (is_selected(list, i) == on)
        return;
    gaddr e = array_entry(list, i);
    wr16(e, (uint16_t)((rd16(e) & 0x7FFF) | (on ? SELECTED : 0)));
    redraw_one(list, i, true);
}

/* ---------------------------------------------------------------------- */
/* LDEF 0: cells as text */

static void ldef0_draw(gaddr list, bool sel, gaddr rect, uint32_t off, uint32_t len)
{
    tb_EraseRect(rect);
    qrect r = qrect_get(rect);
    gaddr l = lp(list);
    tb_MoveTo((int16_t)(r.left + (int16_t)rd16(l + ListRec_indent_off + 2)),
              (int16_t)(r.top + (int16_t)rd16(l + ListRec_indent_off)));
    gaddr cells = rd32(l + ListRec_cells_off);
    if (len) {
        tb_HLock(cells);
        tb_DrawText(rd32(cells), (int16_t)off, (int16_t)len);
        tb_HUnlock(cells);
    }
    if (sel)
        tb_InvertRect(rect);
}

void ldef0_native(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    gaddr list = cpu_pop32(c);
    uint32_t len = cpu_pop16(c), off = cpu_pop16(c);
    cpu_pop32(c); /* cell */
    gaddr rect = cpu_pop32(c);
    bool sel = cpu_pop16(c) >> 8;
    int16_t msg = (int16_t)cpu_pop16(c);
    switch (msg) {
    case lInitMsg: {
        /* Text starts 5 pixels in, on the font's baseline. */
        gaddr fi = tb_NewPtr(8, false, false);
        tb_GetFontInfo(fi);
        wr16(lp(list) + ListRec_indent_off, rd16(fi));
        wr16(lp(list) + ListRec_indent_off + 2, 5);
        tb_DisposePtr(fi);
        break;
    }
    case lDrawMsg:
        ldef0_draw(list, sel, rect, off, len);
        break;
    case lHiliteMsg:
        tb_InvertRect(rect);
        break;
    default:
        break;
    }
    cpu_push32(c, ret);
}

/* ---------------------------------------------------------------------- */
/* Creating and disposing */

static void place_scroll_bars(gaddr list)
{
    qrect v = view_of(list);
    bool grow = rd8(lp(list) + ListRec_lReserved_off) & 0x02;
    gaddr vs = vscroll(list), hs = hscroll(list);
    if (vs) {
        int bottom = v.bottom + 1 - (grow && !hs ? 15 : 0);
        tb_MoveControl(vs, (int16_t)(v.right), (int16_t)(v.top - 1));
        tb_SizeControl(vs, 16, (int16_t)(bottom - (v.top - 1)));
    }
    if (hs) {
        int right = v.right + 1 - (grow && !vs ? 15 : 0);
        tb_MoveControl(hs, (int16_t)(v.left - 1), (int16_t)(v.bottom));
        tb_SizeControl(hs, (int16_t)(right - (v.left - 1)), 16);
    }
}

gaddr tb_LNew(gaddr rview, gaddr bounds, uint32_t csize, int16_t proc, gaddr wind, int8_t draw, int8_t grow,
              int8_t scrollh, int8_t scrollv)
{
    qrect b = qrect_get(bounds), v = qrect_get(rview);
    int rows = b.bottom - b.top, cols = b.right - b.left;
    LOGF(LOG_TRAPS, "  LNew view %d,%d,%d,%d bounds %d,%d,%d,%d proc %d draw %d grow %d h %d v %d\n", v.top, v.left,
         v.bottom, v.right, b.top, b.left, b.bottom, b.right, proc, draw, grow, scrollh, scrollv);
    if (rows < 0 || cols < 0)
        return 0;
    gaddr list = tb_NewHandle((int32_t)(ListRec_cellArray_off + (uint32_t)(rows * cols + 1) * 2), false, true);
    gaddr cells = tb_NewHandle(0, false, true);
    if (!list || !cells)
        return 0;
    gaddr l = lp(list);
    qrect_put(l + ListRec_rView_off, v);
    wr32(l + ListRec_port_off, wind);
    qrect_put(l + ListRec_dataBounds_off, b);
    wr32(l + ListRec_cells_off, cells);
    wr16(l + ListRec_maxIndex_off, (uint16_t)(rows * cols * 2));
    wr8(l + ListRec_lActive_off, 1);
    wr8(l + ListRec_listFlags_off, (uint8_t)((scrollv ? lDoVAutoscroll : 0) | (scrollh ? lDoHAutoscroll : 0)));
    wr8(l + ListRec_lReserved_off, (uint8_t)((draw ? 0 : NO_DRAW) | (grow ? 0x02 : 0)));
    wr32(l + ListRec_lastClick_off, make_pt(-1, -1));

    /* Cell size: given, or the font's line height by rView's width shared out. */
    gaddr saved = tb_NewPtr(4, false, false);
    tb_GetPort(saved);
    tb_SetPort(wind);
    int16_t ch = pt_v(csize), cw = pt_h(csize);
    if (ch <= 0) {
        gaddr fi = tb_NewPtr(8, false, false);
        tb_GetFontInfo(fi);
        ch = (int16_t)((int16_t)rd16(fi) + (int16_t)rd16(fi + 2) + (int16_t)rd16(fi + 6));
        tb_DisposePtr(fi);
    }
    if (cw <= 0)
        cw = (int16_t)((v.right - v.left) / (cols > 0 ? cols : 1));
    l = lp(list);
    wr16(l + ListRec_cellSize_off, (uint16_t)ch);
    wr16(l + ListRec_cellSize_off + 2, (uint16_t)cw);

    /* The definition procedure: the LDEF resource, else the built-in one. */
    gaddr defh = tb_GetResource((int32_t)fourcc("LDEF"), proc);
    if (!defh)
        defh = tb_GetResource((int32_t)fourcc("LDEF"), 0);
    wr32(lp(list) + ListRec_listDefProc_off, defh);

    gaddr empty = tb_NewPtr(2, false, true);
    gaddr r = tb_NewPtr(8, false, true);
    qrect_put(r, (qrect){ 0, 0, 16, 16 });
    if (scrollv)
        wr32(lp(list) + ListRec_vScroll_off, tb_NewControl(wind, r, empty, draw, 0, 0, 0, 16, (int32_t)list));
    if (scrollh)
        wr32(lp(list) + ListRec_hScroll_off, tb_NewControl(wind, r, empty, draw, 0, 0, 0, 16, (int32_t)list));
    tb_DisposePtr(r);
    tb_DisposePtr(empty);
    place_scroll_bars(list);
    set_visible(list, b.top, b.left);

    call_ldef(list, lInitMsg, false, (qrect){ 0, 0, 0, 0 }, 0, 0, 0);
    tb_SetPort(rd32(saved));
    tb_DisposePtr(saved);
    if (draw)
        draw_all(list);
    return list;
}

void tb_LDispose(gaddr list)
{
    if (!list || !rd32(list))
        return;
    call_ldef(list, lCloseMsg, false, (qrect){ 0, 0, 0, 0 }, 0, 0, 0);
    if (vscroll(list))
        tb_DisposeControl(vscroll(list));
    if (hscroll(list))
        tb_DisposeControl(hscroll(list));
    tb_DisposeHandle(rd32(lp(list) + ListRec_cells_off));
    tb_DisposeHandle(list);
}

/* ---------------------------------------------------------------------- */
/* Rows and columns */

static int16_t add_lines(gaddr list, int count, int at, bool rows)
{
    qrect b = bounds_of(list);
    int lo = rows ? b.top : b.left, hi = rows ? b.bottom : b.right;
    if (at < lo)
        at = lo;
    if (at > hi)
        at = hi;
    if (count <= 0)
        return (int16_t)at;
    grid g, n;
    grid_load(list, &g);
    n.rows = g.rows + (rows ? count : 0);
    n.cols = g.cols + (rows ? 0 : count);
    /* A list with no columns gains one when rows are added, and vice versa. */
    if (rows && n.cols == 0)
        n.cols = 1;
    if (!rows && n.rows == 0)
        n.rows = 1;
    n.c = calloc((size_t)(n.rows * n.cols ? n.rows * n.cols : 1), sizeof *n.c);
    int k = at - lo;
    for (int r = 0; r < g.rows; r++)
        for (int c = 0; c < g.cols; c++) {
            int nr = rows && r >= k ? r + count : r, nc = !rows && c >= k ? c + count : c;
            n.c[nr * n.cols + nc] = g.c[r * g.cols + c];
        }
    free(g.c);
    grid_store(list, &n);
    grid_free(&n);
    qrect vis = visible_of(list);
    set_visible(list, vis.top, vis.left);
    draw_all(list);
    return (int16_t)at;
}

int16_t tb_LAddRow(int16_t count, int16_t rown, gaddr list) { return add_lines(list, count, rown, true); }
int16_t tb_LAddColumn(int16_t count, int16_t coln, gaddr list) { return add_lines(list, count, coln, false); }

static void del_lines(gaddr list, int count, int at, bool rows)
{
    qrect b = bounds_of(list);
    int lo = rows ? b.top : b.left, hi = rows ? b.bottom : b.right;
    if (count == 0) { /* everything */
        at = lo;
        count = hi - lo;
    }
    if (at < lo || at >= hi)
        return;
    if (at + count > hi)
        count = hi - at;
    grid g, n;
    grid_load(list, &g);
    n.rows = g.rows - (rows ? count : 0);
    n.cols = g.cols - (rows ? 0 : count);
    n.c = calloc((size_t)(n.rows * n.cols ? n.rows * n.cols : 1), sizeof *n.c);
    int k = at - lo;
    for (int r = 0; r < g.rows; r++)
        for (int c = 0; c < g.cols; c++) {
            hcell *src = &g.c[r * g.cols + c];
            bool gone = rows ? (r >= k && r < k + count) : (c >= k && c < k + count);
            if (gone) {
                free(src->data);
                continue;
            }
            int nr = rows && r >= k + count ? r - count : r, nc = !rows && c >= k + count ? c - count : c;
            n.c[nr * n.cols + nc] = *src;
        }
    free(g.c);
    grid_store(list, &n);
    grid_free(&n);
    qrect vis = visible_of(list);
    set_visible(list, vis.top, vis.left);
    draw_all(list);
}

void tb_LDelRow(int16_t count, int16_t rown, gaddr list) { del_lines(list, count, rown, true); }
void tb_LDelColumn(int16_t count, int16_t coln, gaddr list) { del_lines(list, count, coln, false); }

/* ---------------------------------------------------------------------- */
/* Cell data */

static void replace_data(gaddr list, int i, gaddr src, uint32_t n, bool append)
{
    uint32_t off, len;
    data_range(list, i, &off, &len);
    gaddr cells = rd32(lp(list) + ListRec_cells_off);
    uint32_t total = (uint32_t)tb_GetHandleSize(cells);
    uint32_t keep = append ? len : 0;
    int32_t delta = (int32_t)(keep + n) - (int32_t)len;
    if (total + delta > 0x7FFF)
        return; /* cell data is limited to 32K in all */
    uint8_t *buf = malloc(total + (uint32_t)(delta > 0 ? delta : 0) + 1);
    gget(buf, rd32(cells), total);
    memmove(buf + off + keep + n, buf + off + len, total - off - len);
    if (n)
        gget(buf + off + keep, src, n);
    tb_SetHandleSize(cells, (int32_t)(total + delta));
    gput(rd32(cells), buf, (uint32_t)((int32_t)total + delta));
    free(buf);
    int count = nrows(list) * ncols(list);
    for (int k = i + 1; k <= count; k++) {
        gaddr e = array_entry(list, k);
        uint16_t v = rd16(e);
        wr16(e, (uint16_t)((v & SELECTED) | (((v & 0x7FFF) + delta) & 0x7FFF)));
    }
    redraw_one(list, i, false);
}

void tb_LSetCell(gaddr dp, int16_t dl, uint32_t cell, gaddr list)
{
    int i = index_of(list, cell);
    if (i >= 0)
        replace_data(list, i, dp, dl > 0 ? (uint32_t)dl : 0, false);
}

void tb_LAddToCell(gaddr dp, int16_t dl, uint32_t cell, gaddr list)
{
    int i = index_of(list, cell);
    if (i >= 0)
        replace_data(list, i, dp, dl > 0 ? (uint32_t)dl : 0, true);
}

void tb_LClrCell(uint32_t cell, gaddr list)
{
    int i = index_of(list, cell);
    if (i >= 0)
        replace_data(list, i, 0, 0, false);
}

void tb_LGetCell(gaddr dp, gaddr dlp, uint32_t cell, gaddr list)
{
    int i = index_of(list, cell);
    if (i < 0) {
        wr16(dlp, 0);
        return;
    }
    uint32_t off, len;
    data_range(list, i, &off, &len);
    uint32_t want = (uint16_t)rd16(dlp);
    uint32_t n = len < want ? len : want;
    gmemcpy(dp, rd32(rd32(lp(list) + ListRec_cells_off)) + off, n);
    wr16(dlp, (uint16_t)n);
}

void tb_LGetCellDataLocation(gaddr offsetp, gaddr lenp, uint32_t cell, gaddr list)
{
    int i = index_of(list, cell);
    uint32_t off = 0, len = 0;
    if (i >= 0)
        data_range(list, i, &off, &len);
    else
        off = len = 0xFFFF;
    wr16(offsetp, (uint16_t)off);
    wr16(lenp, (uint16_t)len);
}

void tb_LCellSize(uint32_t csize, gaddr list)
{
    wr32(lp(list) + ListRec_cellSize_off, csize);
    qrect vis = visible_of(list);
    set_visible(list, vis.top, vis.left);
    draw_all(list);
}

/* ---------------------------------------------------------------------- */
/* Selection */

int8_t tb_LGetSelect(int8_t next, gaddr cellp, gaddr list)
{
    int n = nrows(list) * ncols(list);
    int i = index_of(list, rd32(cellp));
    if (!next)
        return i >= 0 && is_selected(list, i);
    if (i < 0) {
        /* From the start when the cell is before the data. */
        qrect b = bounds_of(list);
        uint32_t c = rd32(cellp);
        if (pt_v(c) >= b.bottom || n == 0)
            return 0;
        i = 0;
    }
    for (; i < n; i++)
        if (is_selected(list, i)) {
            wr32(cellp, cell_at(list, i));
            return 1;
        }
    return 0;
}

void tb_LSetSelect(int8_t setit, uint32_t cell, gaddr list)
{
    int i = index_of(list, cell);
    if (i >= 0)
        set_selected(list, i, setit != 0);
}

int8_t tb_LNextCell(int8_t hnext, int8_t vnext, gaddr cellp, gaddr list)
{
    qrect b = bounds_of(list);
    uint32_t c = rd32(cellp);
    int v = pt_v(c), h = pt_h(c);
    if (hnext && vnext) {
        if (++h >= b.right) {
            h = b.left;
            v++;
        }
    } else if (hnext) {
        h++;
    } else if (vnext) {
        v++;
    } else {
        return 0;
    }
    if (v < b.top || v >= b.bottom || h < b.left || h >= b.right)
        return 0;
    wr32(cellp, make_pt((int16_t)v, (int16_t)h));
    return 1;
}

int32_t tb_LLastClick(gaddr list) { return (int32_t)rd32(lp(list) + ListRec_lastClick_off); }

/* Default comparison for LSearch: equal ignoring case. */
static bool same_text(gaddr a, int alen, gaddr b, int blen)
{
    if (alen != blen)
        return false;
    for (int k = 0; k < alen; k++) {
        uint8_t x = rd8(a + (uint32_t)k), y = rd8(b + (uint32_t)k);
        if (x >= 'a' && x <= 'z')
            x -= 32;
        if (y >= 'a' && y <= 'z')
            y -= 32;
        if (x != y)
            return false;
    }
    return true;
}

int8_t tb_LSearch(gaddr dp, int16_t dl, gaddr proc, gaddr cellp, gaddr list)
{
    int n = nrows(list) * ncols(list);
    int i = index_of(list, rd32(cellp));
    if (i < 0)
        i = 0;
    gaddr cells = rd32(lp(list) + ListRec_cells_off);
    for (; i < n; i++) {
        uint32_t off, len;
        data_range(list, i, &off, &len);
        bool match;
        if (proc) {
            /* FUNCTION (aPtr, bPtr: Ptr; aLen, bLen: INTEGER): INTEGER; 0 = match */
            cpu68k *c = g_cpu;
            tb_HLock(cells);
            cpu_push16(c, 0);
            cpu_push32(c, dp);
            cpu_push32(c, rd32(cells) + off);
            cpu_push16(c, (uint16_t)dl);
            cpu_push16(c, (uint16_t)len);
            guest_call(proc);
            match = cpu_pop16(c) == 0;
            tb_HUnlock(cells);
        } else {
            match = same_text(dp, dl, rd32(cells) + off, (int)len);
        }
        if (match) {
            wr32(cellp, cell_at(list, i));
            return 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Drawing and scrolling */

void tb_LSetDrawingMode(int8_t draw, gaddr list)
{
    gaddr a = lp(list) + ListRec_lReserved_off;
    wr8(a, (uint8_t)(draw ? rd8(a) & ~NO_DRAW : rd8(a) | NO_DRAW));
}

void tb_LDraw(uint32_t cell, gaddr list)
{
    int i = index_of(list, cell);
    if (i >= 0)
        redraw_one(list, i, false);
}

void tb_LUpdate(gaddr rgn, gaddr list)
{
    port_state s = enter(list);
    int n = nrows(list) * ncols(list);
    gaddr r = tb_NewPtr(8, false, false);
    qrect_put(r, view_of(list));
    if (!rgn || tb_RectInRgn(r, rgn))
        tb_EraseRect(r);
    for (int i = 0; i < n; i++) {
        qrect vis = visible_of(list);
        uint32_t cell = cell_at(list, i);
        if (pt_v(cell) < vis.top || pt_v(cell) >= vis.bottom || pt_h(cell) < vis.left || pt_h(cell) >= vis.right)
            continue;
        qrect_put(r, cell_rect(list, cell));
        if (!rgn || tb_RectInRgn(r, rgn))
            draw_cell(list, i);
    }
    tb_DisposePtr(r);
    leave(s);
    if (vscroll(list))
        tb_Draw1Control(vscroll(list));
    if (hscroll(list))
        tb_Draw1Control(hscroll(list));
}

void tb_LScroll(int16_t dcols, int16_t drows, gaddr list)
{
    qrect vis = visible_of(list);
    set_visible(list, vis.top + drows, vis.left + dcols);
    qrect now = visible_of(list);
    if (now.top != vis.top || now.left != vis.left)
        draw_all(list);
}

void tb_LAutoScroll(gaddr list)
{
    int n = nrows(list) * ncols(list);
    for (int i = 0; i < n; i++)
        if (is_selected(list, i)) {
            uint32_t cell = cell_at(list, i);
            qrect vis = visible_of(list);
            int top = vis.top, left = vis.left;
            if (pt_v(cell) < vis.top || pt_v(cell) >= vis.top + rows_full(list))
                top = pt_v(cell);
            if (pt_h(cell) < vis.left || pt_h(cell) >= vis.left + cols_full(list))
                left = pt_h(cell);
            tb_LScroll((int16_t)(left - vis.left), (int16_t)(top - vis.top), list);
            return;
        }
}

void tb_LActivate(int8_t act, gaddr list)
{
    wr8(lp(list) + ListRec_lActive_off, act ? 1 : 0);
    if (vscroll(list))
        tb_HiliteControl(vscroll(list), act ? 0 : 255);
    if (hscroll(list))
        tb_HiliteControl(hscroll(list), act ? 0 : 255);
}

void tb_LSize(int16_t width, int16_t height, gaddr list)
{
    qrect v = view_of(list);
    v.right = (int16_t)(v.left + width);
    v.bottom = (int16_t)(v.top + height);
    qrect_put(lp(list) + ListRec_rView_off, v);
    place_scroll_bars(list);
    qrect vis = visible_of(list);
    set_visible(list, vis.top, vis.left);
}

void tb_LRect(gaddr cellrect, uint32_t cell, gaddr list)
{
    if (index_of(list, cell) < 0) {
        qrect_put(cellrect, (qrect){ 0, 0, 0, 0 });
        return;
    }
    qrect_put(cellrect, cell_rect(list, cell));
}

/* ---------------------------------------------------------------------- */
/* Clicks */

/* Track a scroll bar: the thumb by dragging, arrows and page areas by
 * repeating while the button is down. */
static void track_scroll(gaddr list, gaddr ctl, uint32_t pt, int16_t part, bool vertical)
{
    if (part == 129 /* inThumb */) {
        if (tb_TrackControl(ctl, pt, 0)) {
            qrect b = bounds_of(list), vis = visible_of(list);
            int v = tb_GetControlValue(ctl);
            if (vertical)
                set_visible(list, b.top + v, vis.left);
            else
                set_visible(list, vis.top, b.left + v);
            draw_all(list);
        }
        return;
    }
    int page = vertical ? rows_full(list) - 1 : cols_full(list) - 1;
    if (page < 1)
        page = 1;
    int step = part == 20 ? -1 : part == 21 ? 1 : part == 22 ? -page : part == 23 ? page : 0;
    if (!step)
        return;
    tb_HiliteControl(ctl, part);
    uint32_t next = tb_TickCount();
    do {
        if (tb_TickCount() >= next) {
            tb_LScroll(vertical ? 0 : (int16_t)step, vertical ? (int16_t)step : 0, list);
            next = tb_TickCount() + 4;
        }
        machine_advance_ticks();
    } while (tb_StillDown());
    tb_HiliteControl(ctl, 0);
}

/* Select from the anchor to `to` (a rectangle of cells), deselecting the rest
 * unless `keep` holds the selection as it was before the click. */
static void select_range(gaddr list, uint32_t anchor, uint32_t to, const uint8_t *keep)
{
    int n = nrows(list) * ncols(list);
    int v0 = pt_v(anchor), h0 = pt_h(anchor), v1 = pt_v(to), h1 = pt_h(to);
    if (v0 > v1) { int t = v0; v0 = v1; v1 = t; }
    if (h0 > h1) { int t = h0; h0 = h1; h1 = t; }
    for (int i = 0; i < n; i++) {
        uint32_t c = cell_at(list, i);
        bool in = pt_v(c) >= v0 && pt_v(c) <= v1 && pt_h(c) >= h0 && pt_h(c) <= h1;
        set_selected(list, i, in || (keep && keep[i]));
    }
}

int8_t tb_LClick(uint32_t pt, int16_t mods, gaddr list)
{
    /* Scroll bars first. */
    gaddr chp = tb_NewPtr(4, false, true);
    int16_t part = tb_FindControl(pt, rd32(lp(list) + ListRec_port_off), chp);
    gaddr ctl = rd32(chp);
    tb_DisposePtr(chp);
    if (part && ctl && (ctl == vscroll(list) || ctl == hscroll(list))) {
        track_scroll(list, ctl, pt, part, ctl == vscroll(list));
        return 0;
    }

    gaddr l = lp(list);
    uint8_t flags = rd8(l + ListRec_selFlags_off);
    qrect v = view_of(list), vis = visible_of(list);
    int n = nrows(list) * ncols(list);
    uint32_t prev = rd32(l + ListRec_lastClick_off);
    uint32_t when = tb_TickCount();
    bool dbl = when - rd32(l + ListRec_clikTime_off) <= (uint32_t)tb_GetDblTime();
    wr32(l + ListRec_clikTime_off, when);
    wr32(l + ListRec_clikLoc_off, pt);

    bool shift = (mods & 0x0200) && !(flags & lOnlyOne) && !(flags & lNoExtend);
    bool cmd = (mods & 0x0100) && !(flags & lOnlyOne);
    uint8_t *before = calloc((size_t)(n ? n : 1), 1);
    for (int i = 0; i < n; i++)
        before[i] = is_selected(list, i);
    uint32_t anchor = prev;
    if (shift) {
        /* Extend from the first selected cell when there was no click yet. */
        for (int i = 0; i < n && index_of(list, anchor) < 0; i++)
            if (before[i])
                anchor = cell_at(list, i);
    }
    bool toggle_on = true;
    uint32_t start = 0, last = 0xFFFFFFFFu;
    bool started = false;
    gaddr saved = tb_NewPtr(4, false, false), mp = tb_NewPtr(4, false, false);
    tb_GetPort(saved);
    tb_SetPort(rd32(l + ListRec_port_off));
    uint8_t lflags = rd8(l + ListRec_listFlags_off);
    bool in_view = pt_v(pt) >= v.top && pt_v(pt) < v.bottom && pt_h(pt) >= v.left && pt_h(pt) < v.right;
    uint32_t cur = pt;
    while (in_view) {
        int pv = pt_v(cur), ph = pt_h(cur);
        /* Dragging past the edges scrolls. */
        if ((lflags & lDoVAutoscroll) && pv < v.top)
            tb_LScroll(0, -1, list);
        else if ((lflags & lDoVAutoscroll) && pv >= v.bottom)
            tb_LScroll(0, 1, list);
        if ((lflags & lDoHAutoscroll) && ph < v.left)
            tb_LScroll(-1, 0, list);
        else if ((lflags & lDoHAutoscroll) && ph >= v.right)
            tb_LScroll(1, 0, list);
        vis = visible_of(list);
        int cv = pv < v.top ? v.top : pv >= v.bottom ? v.bottom - 1 : pv;
        int chh = ph < v.left ? v.left : ph >= v.right ? v.right - 1 : ph;
        int h = cell_h(list), w = cell_w(list);
        uint32_t cell = make_pt((int16_t)(vis.top + (cv - v.top) / (h ? h : 1)),
                                (int16_t)(vis.left + (chh - v.left) / (w ? w : 1)));
        int i = index_of(list, cell);
        if (i < 0 && !started && !shift && !cmd) {
            for (int k = 0; k < n; k++)
                set_selected(list, k, false); /* a click on no cell clears the selection */
        }
        if (i >= 0 && cell != last) {
            if (!started) {
                start = cell;
                started = true;
                toggle_on = !before[i];
            }
            if (flags & lOnlyOne) {
                for (int k = 0; k < n; k++)
                    set_selected(list, k, k == i);
            } else if (cmd) {
                int v0 = pt_v(start), v1 = pt_v(cell), h0 = pt_h(start), h1 = pt_h(cell);
                if (v0 > v1) { int t = v0; v0 = v1; v1 = t; }
                if (h0 > h1) { int t = h0; h0 = h1; h1 = t; }
                for (int k = 0; k < n; k++) {
                    uint32_t c = cell_at(list, k);
                    bool in = pt_v(c) >= v0 && pt_v(c) <= v1 && pt_h(c) >= h0 && pt_h(c) <= h1;
                    set_selected(list, k, in ? toggle_on : before[k]);
                }
            } else if (shift && index_of(list, anchor) >= 0) {
                select_range(list, anchor, cell, NULL);
            } else {
                select_range(list, start, cell, NULL);
            }
            last = cell;
            wr32(lp(list) + ListRec_lastClick_off, cell);
        }
        gaddr loop = rd32(lp(list) + ListRec_lClikLoop_off);
        if (loop) {
            cpu_push16(g_cpu, 0);
            guest_call(loop);
            if (!(cpu_pop16(g_cpu) >> 8))
                break;
        }
        if (!tb_StillDown())
            break;
        tb_GetMouse(mp);
        cur = rd32(mp);
        machine_advance_ticks();
    }
    tb_SetPort(rd32(saved));
    tb_DisposePtr(saved);
    tb_DisposePtr(mp);
    free(before);
    /* A double click: the second click of a pair in the same cell. */
    return started && dbl && last == prev && !shift && !cmd;
}
