/*
 * The Data Browser (CarbonLib 1.1's): a control showing a program's items
 * as rows of a list (the list view, with columns, sorting and containers
 * opening under disclosure triangles) or as Miller columns (the column
 * view).  The browser keeps the items' IDs, their containers and states;
 * their data is the program's, asked for through its item data callback
 * with a DataBrowserItemDataRef the Set/GetDataBrowserItemData calls fill
 * and read.  It is a control with a definition of ours, drawn in Platinum
 * with scroll bars of its own.
 */
#include <stdlib.h>
#include <string.h>

#include "ppc_bridge.h"
#include "qd_internal.h"
#include "tb_internal.h"
#include "trap.h"

bool ev_still_down(void);
void ev_wait_tick(void);

#define TAG(a, b, d, e) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(d) << 8 | (uint32_t)(e))

enum {
    errDataBrowserNotConfigured = -4970, errDataBrowserItemNotFound = -4971, errDataBrowserItemNotAdded = -4975,
    errDataBrowserPropertyNotFound = -4972, errDataBrowserInvalidPropertyPart = -4973,
    errDataBrowserInvalidPropertyData = -4974, errDataBrowserPropertyNotSupported = -4979,
    errDataNotSupported = -30581, errUnknownControl = -30584,
};
enum { kDataBrowserListView = TAG('l', 's', 't', 'v'), kDataBrowserColumnView = TAG('c', 'l', 'm', 'v') };
enum {
    kDataBrowserDragSelect = 1, kDataBrowserSelectOnlyOne = 2, kDataBrowserResetSelection = 4,
    kDataBrowserCmdTogglesSelection = 8, kDataBrowserNoDisjointSelection = 16, kDataBrowserAlwaysExtendSelection = 32,
    kDataBrowserNeverEmptySelectionSet = 64,
};
enum { kDataBrowserOrderUndefined = 0, kDataBrowserOrderIncreasing = 1, kDataBrowserOrderDecreasing = 2 };
enum { kDataBrowserItemAnyState = 0xFFFFFFFFu, kDataBrowserItemIsSelected = 1, kDataBrowserContainerIsOpen = 2 };
enum { kDataBrowserRevealAndCenterInView = 1 };
enum { kDataBrowserItemsAdd = 0, kDataBrowserItemsAssign = 1, kDataBrowserItemsToggle = 2, kDataBrowserItemsRemove = 3 };
enum {
    kDataBrowserItemAdded = 1, kDataBrowserItemRemoved = 2, kDataBrowserEditStarted = 3, kDataBrowserEditStopped = 4,
    kDataBrowserItemSelected = 5, kDataBrowserItemDeselected = 6, kDataBrowserItemDoubleClicked = 7,
    kDataBrowserContainerOpened = 8, kDataBrowserContainerClosing = 9, kDataBrowserContainerClosed = 10,
    kDataBrowserContainerSorting = 11, kDataBrowserContainerSorted = 12, kDataBrowserUserStateChanged = 13,
    kDataBrowserSelectionSetChanged = 14, kDataBrowserTargetChanged = 15, kDataBrowserUserToggledContainer = 16,
};
enum {
    kDataBrowserItemIsActiveProperty = 1, kDataBrowserItemIsSelectableProperty = 2,
    kDataBrowserItemIsEditableProperty = 3, kDataBrowserItemIsContainerProperty = 4,
    kDataBrowserContainerIsOpenableProperty = 5, kDataBrowserContainerIsClosableProperty = 6,
    kDataBrowserContainerIsSortableProperty = 7, kDataBrowserItemSelfIdentityProperty = 8,
};
enum {
    kDataBrowserCustomType = 0x3F3F3F3F, kDataBrowserIconType = TAG('i', 'c', 'n', 'r'),
    kDataBrowserTextType = TAG('t', 'e', 'x', 't'), kDataBrowserDateTimeType = TAG('d', 'a', 't', 'e'),
    kDataBrowserSliderType = TAG('s', 'l', 'd', 'r'), kDataBrowserCheckboxType = TAG('c', 'h', 'b', 'x'),
    kDataBrowserProgressBarType = TAG('p', 'r', 'o', 'g'), kDataBrowserRelevanceRankType = TAG('r', 'a', 'n', 'k'),
    kDataBrowserPopupMenuType = TAG('m', 'e', 'n', 'u'), kDataBrowserIconAndTextType = TAG('t', 'i', 'c', 'n'),
};
enum {
    kDataBrowserPropertyEnclosingPart = 0, kDataBrowserPropertyContentPart = TAG('-', '-', '-', '-'),
    kDataBrowserPropertyDisclosurePart = TAG('d', 'i', 's', 'c'),
};
enum { kDataBrowserPropertyIsMutable = 1, kDataBrowserListViewSortableColumn = 1 << 18 };
enum { kDataBrowserTableViewMinimalHilite = 0, kDataBrowserTableViewFillHilite = 1 };
enum { kDataBrowserEditMsgCut = TAG('c', 'u', 't', ' '), kDataBrowserEditMsgCopy = TAG('c', 'o', 'p', 'y'),
       kDataBrowserEditMsgPaste = TAG('p', 'a', 's', 't'), kDataBrowserEditMsgClear = TAG('c', 'l', 'e', 'a'),
       kDataBrowserEditMsgSelectAll = TAG('s', 'a', 'l', 'l') };

enum { drawCntl = 0, testCntl, calcCRgns, initCntl, dispCntl, posCntl };
enum {
    kControlMsgHandleTracking = 15, kControlMsgFocus = 16, kControlMsgKeyDown = 17, kControlMsgIdle = 18,
    kControlMsgGetFeatures = 19, kControlMsgSetData = 20, kControlMsgGetData = 21, kControlMsgActivate = 22,
    kControlMsgTestNewMsgSupport = 27, kControlMsgContextualMenuClick = 47, kControlMsgGetClickActivation = 48,
};
enum {
    kControlSupportsFocus = 1 << 2, kControlWantsIdle = 1 << 3, kControlWantsActivate = 1 << 4,
    kControlHandlesTracking = 1 << 5, kControlSupportsDataAccess = 1 << 6, kControlGetsFocusOnClick = 1 << 8,
    kControlSupportsContextualMenus = 1 << 21, kControlSupportsClickActivation = 1 << 22,
};
enum { kDataBrowserPart = 10 }; /* what a click in the browser answers */
#define NEW_MSG_OK 0x206F6B20

/* The procInfos of the callbacks we call. */
enum {
    piItemData = 0x00007FF0, piItemCompare = 0x00003FD0, piItemNotification = 0x00000FC0, piItem = 0x00000FC0,
    piDrawItem = 0x0006FFC0, piGetContextualMenu = 0x0000FFC0, piSelectContextualMenu = 0x0000AFC0,
};

/* Callbacks, in DataBrowserCallbacks' and DataBrowserCustomCallbacks' order. */
enum { CB_ITEM_DATA, CB_COMPARE, CB_NOTIFY, CB_ADD_DRAG, CB_ACCEPT_DRAG, CB_RECEIVE_DRAG, CB_POST_DRAG, CB_HELP,
       CB_GET_MENU, CB_SELECT_MENU, CB_COUNT };
enum { CC_DRAW, CC_EDIT, CC_HIT, CC_TRACK, CC_DRAG_RGN, CC_ACCEPT, CC_RECEIVE, CC_COUNT };

/* The DataBrowserItemDataRef we hand the program: the item and property
 * asked about, then a field for each kind of data, and which were set. */
enum {
    ID_ITEM = 0, ID_PROPERTY = 4, ID_TEXT = 8, ID_VALUE = 12, ID_MIN = 16, ID_MAX = 20, ID_BOOL = 24,
    ID_BUTTON = 26, ID_MENU = 28, ID_RGB = 32, ID_TRANSFORM = 38, ID_DRAW_STATE = 40, ID_DATE = 44,
    ID_LONG_DATE = 48, ID_ITEM_ID = 56, ID_ICON = 60, ID_SET = 64, ID_SIZE = 68,
};
enum { SET_TEXT = 1, SET_VALUE = 2, SET_MIN = 4, SET_MAX = 8, SET_BOOL = 16, SET_BUTTON = 32, SET_MENU = 64,
       SET_RGB = 128, SET_TRANSFORM = 256, SET_DRAW_STATE = 512, SET_DATE = 1024, SET_LONG_DATE = 2048,
       SET_ITEM_ID = 4096, SET_ICON = 8192 };

#define SCROLL_W 15
#define INDENT 16
#define DISCLOSURE_W 14

typedef struct {
    uint32_t id, parent, state;
    uint16_t row_height; /* 0: the browser's */
} db_item;

typedef struct {
    uint32_t prop, type, flags;
    uint16_t width, min_width, max_width;
    int16_t title_offset;
    uint8_t title[256];
    uint16_t initial_order;
} db_column;

typedef struct {
    uint32_t prop, flags;
} db_flags;

typedef struct {
    gaddr ch;
    uint32_t style;
    db_item *items;
    int nitems, cap_items;
    db_column *cols;
    int ncols;
    db_flags *pflags;
    int npflags;
    uint32_t *rows; /* the rows shown, top to bottom */
    uint8_t *depth;
    int nrows;
    bool rows_valid;
    uint32_t sort_prop;
    uint16_t sort_order;
    uint32_t sel_flags;
    gaddr cb[CB_COUNT], ccb[CC_COUNT];
    uint32_t target;
    uint16_t row_height, col_width, header_height;
    bool var_widths, var_heights;
    uint32_t hilite;
    uint32_t disclosure_col;
    bool expandable, plain_bg, items_active, hscroll, vscroll, frame;
    qrect bar_inset;
    int32_t top, left;
    uint32_t edit_item, edit_prop;
    gaddr edit_te;
    uint32_t display_type;
    uint32_t *path;
    int path_len;
    gaddr item_data;
    gaddr key_filter, validation;
    bool focused;
    uint32_t last_click, last_item;
    uint32_t anchor; /* the item a shift-click extends from */
} databrowser;

#define MAX_BROWSERS 64
static databrowser *browsers[MAX_BROWSERS];
static gaddr cdef_stub;

void databrowser_reset(void)
{
    for (int i = 0; i < MAX_BROWSERS; i++)
        browsers[i] = NULL; /* the machine's memory went with them; ours leaks no further */
    cdef_stub = 0;
}

static databrowser *db_of(gaddr ch)
{
    if (!ch)
        return NULL;
    for (int i = 0; i < MAX_BROWSERS; i++)
        if (browsers[i] && browsers[i]->ch == ch)
            return browsers[i];
    return NULL;
}

static gaddr cp(gaddr ch) { return rd32(ch); }
static qrect crect(gaddr ch) { return qrect_get(cp(ch) + ControlRecord_contrlRect_off); }
static bool cvisible(gaddr ch) { return ControlRecord_contrlVis(cp(ch)) != 0; }
static bool cactive(gaddr ch) { return ControlRecord_contrlHilite(cp(ch)) != 255; }
static qrect inset(qrect r, int d) { return (qrect){ (int16_t)(r.top + d), (int16_t)(r.left + d), (int16_t)(r.bottom - d), (int16_t)(r.right - d) }; }
static bool in_rect(qrect r, uint32_t pt)
{
    return pt_h(pt) >= r.left && pt_h(pt) < r.right && pt_v(pt) >= r.top && pt_v(pt) < r.bottom;
}

static gaddr tmp_rect(qrect r)
{
    gaddr p = tb_NewPtr(8, false, false);
    qrect_put(p, r);
    return p;
}

/* ---------------------------------------------------------------------- */
/* Items */

static db_item *find_item(databrowser *b, uint32_t id)
{
    for (int i = 0; i < b->nitems; i++)
        if (b->items[i].id == id)
            return &b->items[i];
    return NULL;
}

static void notify(databrowser *b, uint32_t item, uint32_t msg)
{
    if (b->cb[CB_NOTIFY])
        mm_call(b->cb[CB_NOTIFY], piItemNotification, (uint32_t[]){ b->ch, item, msg }, 3);
}

/* Ask the program for an item's property: the item data record filled. */
static int32_t fetch(databrowser *b, uint32_t item, uint32_t prop)
{
    gmemset(b->item_data, 0, ID_SIZE);
    wr32(b->item_data + ID_ITEM, item);
    wr32(b->item_data + ID_PROPERTY, prop);
    if (!b->cb[CB_ITEM_DATA])
        return errDataBrowserNotConfigured;
    return (int32_t)mm_call(b->cb[CB_ITEM_DATA], piItemData, (uint32_t[]){ b->ch, item, prop, b->item_data, 0 }, 5);
}

/* Give the program a value it set (setValue true): the record as filled. */
static int32_t store(databrowser *b, uint32_t item, uint32_t prop)
{
    wr32(b->item_data + ID_ITEM, item);
    wr32(b->item_data + ID_PROPERTY, prop);
    if (!b->cb[CB_ITEM_DATA])
        return errDataBrowserNotConfigured;
    return (int32_t)mm_call(b->cb[CB_ITEM_DATA], piItemData, (uint32_t[]){ b->ch, item, prop, b->item_data, 1 }, 5);
}

static bool item_bool(databrowser *b, uint32_t item, uint32_t prop, bool dflt)
{
    if (fetch(b, item, prop) != noErr || !(rd32(b->item_data + ID_SET) & SET_BOOL))
        return dflt;
    return rd8(b->item_data + ID_BOOL) != 0;
}

static bool is_container(databrowser *b, uint32_t item) { return item_bool(b, item, kDataBrowserItemIsContainerProperty, false); }
static bool is_selectable(databrowser *b, uint32_t item) { return item_bool(b, item, kDataBrowserItemIsSelectableProperty, true); }

static uint32_t prop_flags(databrowser *b, uint32_t prop)
{
    for (int i = 0; i < b->ncols; i++)
        if (b->cols[i].prop == prop)
            return b->cols[i].flags;
    for (int i = 0; i < b->npflags; i++)
        if (b->pflags[i].prop == prop)
            return b->pflags[i].flags;
    return 0;
}

static db_column *column(databrowser *b, uint32_t prop)
{
    for (int i = 0; i < b->ncols; i++)
        if (b->cols[i].prop == prop)
            return &b->cols[i];
    return NULL;
}

/* ---------------------------------------------------------------------- */
/* Layout */

static qrect content_rect(databrowser *b)
{
    qrect r = crect(b->ch);
    return b->frame ? inset(r, 1) : r;
}

static qrect vbar_rect(databrowser *b)
{
    qrect c = content_rect(b);
    return (qrect){ (int16_t)(c.top + b->bar_inset.top), (int16_t)(c.right - SCROLL_W - 1 - b->bar_inset.right),
                    (int16_t)(c.bottom - (b->hscroll ? SCROLL_W : 0) - b->bar_inset.bottom), (int16_t)(c.right - b->bar_inset.right) };
}

static qrect hbar_rect(databrowser *b)
{
    qrect c = content_rect(b);
    return (qrect){ (int16_t)(c.bottom - SCROLL_W - 1 - b->bar_inset.bottom), (int16_t)(c.left + b->bar_inset.left),
                    (int16_t)(c.bottom - b->bar_inset.bottom), (int16_t)(c.right - (b->vscroll ? SCROLL_W : 0) - b->bar_inset.right) };
}

static int header_h(databrowser *b) { return b->style == kDataBrowserListView && b->ncols ? b->header_height : 0; }

/* Where the rows go (below the header, inside the scroll bars). */
static qrect rows_rect(databrowser *b)
{
    qrect c = content_rect(b);
    c.top = (int16_t)(c.top + header_h(b));
    if (b->vscroll)
        c.right = (int16_t)(c.right - SCROLL_W);
    if (b->hscroll)
        c.bottom = (int16_t)(c.bottom - SCROLL_W);
    return c;
}

static qrect header_rect(databrowser *b)
{
    qrect c = content_rect(b);
    if (b->vscroll)
        c.right = (int16_t)(c.right - SCROLL_W);
    c.bottom = (int16_t)(c.top + header_h(b));
    return c;
}

static int row_h(databrowser *b, uint32_t item)
{
    if (b->var_heights) {
        db_item *it = find_item(b, item);
        if (it && it->row_height)
            return it->row_height;
    }
    return b->row_height;
}

/* The items in a container, in the browser's order. */
static int children(databrowser *b, uint32_t parent, uint32_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < b->nitems; i++)
        if (b->items[i].parent == parent) {
            if (out && n < max)
                out[n] = b->items[i].id;
            n++;
        }
    return n;
}

static void add_rows(databrowser *b, uint32_t parent, int depth)
{
    for (int i = 0; i < b->nitems; i++) {
        if (b->items[i].parent != parent)
            continue;
        if (b->nrows % 64 == 0) {
            b->rows = realloc(b->rows, (size_t)(b->nrows + 64) * sizeof *b->rows);
            b->depth = realloc(b->depth, (size_t)(b->nrows + 64));
        }
        uint32_t id = b->items[i].id, state = b->items[i].state;
        b->rows[b->nrows] = id;
        b->depth[b->nrows++] = (uint8_t)(depth < 255 ? depth : 255);
        if ((state & kDataBrowserContainerIsOpen) && depth < 64)
            add_rows(b, id, depth + 1);
    }
}

/* The rows shown: the target's items, open containers' under them (list
 * view); the last container of the path's (column view, its own columns
 * drawn from the path). */
static void layout(databrowser *b)
{
    if (b->rows_valid)
        return;
    b->nrows = 0;
    if (b->style == kDataBrowserColumnView && b->path_len)
        add_rows(b, b->path[b->path_len - 1], 0);
    else
        add_rows(b, b->target, 0);
    b->rows_valid = true;
}

static void changed(databrowser *b) { b->rows_valid = false; }

static int row_of(databrowser *b, uint32_t item)
{
    layout(b);
    for (int i = 0; i < b->nrows; i++)
        if (b->rows[i] == item)
            return i;
    return -1;
}

static int row_top(databrowser *b, int row)
{
    int y = 0;
    for (int i = 0; i < row && i < b->nrows; i++)
        y += row_h(b, b->rows[i]);
    return y;
}

static int total_height(databrowser *b)
{
    layout(b);
    return row_top(b, b->nrows);
}

static int col_w(databrowser *b, const db_column *c) { return c->width ? c->width : b->col_width; }

static int total_width(databrowser *b)
{
    if (b->style == kDataBrowserColumnView)
        return (b->path_len + 1) * b->col_width;
    int w = 0;
    for (int i = 0; i < b->ncols; i++)
        w += col_w(b, &b->cols[i]);
    return w;
}

static void clamp_scroll(databrowser *b)
{
    qrect r = rows_rect(b);
    int maxv = total_height(b) - (r.bottom - r.top), maxh = total_width(b) - (r.right - r.left);
    if (b->top > maxv)
        b->top = maxv;
    if (b->top < 0)
        b->top = 0;
    if (b->left > maxh)
        b->left = maxh;
    if (b->left < 0)
        b->left = 0;
}

/* The cell of an item's property: its row, that column. */
static bool cell_rect(databrowser *b, uint32_t item, uint32_t prop, qrect *out)
{
    int row = row_of(b, item);
    if (row < 0)
        return false;
    qrect r = rows_rect(b);
    int y = r.top + row_top(b, row) - b->top, x = r.left - b->left;
    if (b->style == kDataBrowserColumnView) {
        *out = (qrect){ (int16_t)y, (int16_t)(x + b->path_len * b->col_width), (int16_t)(y + row_h(b, item)),
                        (int16_t)(x + (b->path_len + 1) * b->col_width) };
        return true;
    }
    for (int i = 0; i < b->ncols; i++) {
        int w = col_w(b, &b->cols[i]);
        if (b->cols[i].prop == prop) {
            *out = (qrect){ (int16_t)y, (int16_t)x, (int16_t)(y + row_h(b, item)), (int16_t)(x + w) };
            return true;
        }
        x += w;
    }
    return false;
}

/* The indent and disclosure triangle in the disclosure column. */
static int disclosure_indent(databrowser *b, int row) { return b->depth[row] * INDENT; }

static bool has_disclosure(databrowser *b, uint32_t prop)
{
    return b->style == kDataBrowserListView && b->expandable && prop == b->disclosure_col;
}

/* ---------------------------------------------------------------------- */
/* Drawing */

static void text_of(databrowser *b, uint8_t out[256])
{
    out[0] = 0;
    if (rd32(b->item_data + ID_SET) & SET_TEXT)
        carbon_cfstring_to_pstr(rd32(b->item_data + ID_TEXT), out);
}

static void draw_text(qrect r, const uint8_t *t, bool selected, bool active)
{
    uint16_t c[3];
    ap_text_rgb(active ? (selected ? 1 : 1) : 2, c); /* kThemeTextColorDialogActive, Inactive */
    qrect tr = { r.top, (int16_t)(r.left + 3), r.bottom, (int16_t)(r.right - 2) };
    int n = t[0];
    gaddr port = qd_port();
    while (n > 0 && qd_text_width(port, t + 1, n) > tr.right - tr.left)
        n--;
    ap_text_in(tr, t + 1, n, false, c);
}

/* A track (progress, relevance, slider) of the cell's value in its range. */
static void draw_track(databrowser *b, qrect r, uint32_t type, bool active)
{
    int32_t min = (int32_t)rd32(b->item_data + ID_MIN), max = (int32_t)rd32(b->item_data + ID_MAX),
            v = (int32_t)rd32(b->item_data + ID_VALUE);
    gaddr info = tb_NewPtr(36, false, true);
    int h = r.bottom - r.top, th = type == kDataBrowserSliderType ? 16 : 12;
    qrect tr = { (int16_t)(r.top + (h - th) / 2), (int16_t)(r.left + 3), (int16_t)(r.top + (h - th) / 2 + th), (int16_t)(r.right - 3) };
    wr16(info, type == kDataBrowserSliderType ? 2 : type == kDataBrowserRelevanceRankType ? 1 : 0);
    qrect_put(info + 2, tr);
    wr32(info + 10, (uint32_t)min);
    wr32(info + 14, (uint32_t)max);
    wr32(info + 18, (uint32_t)v);
    wr16(info + 26, 1); /* horizontal */
    wr8(info + 28, active ? 0 : 1);
    ap_track(info);
    tb_DisposePtr(info);
}

static void draw_cell(databrowser *b, uint32_t item, const db_column *c, qrect cell, int row, bool selected, bool active)
{
    qrect r = cell;
    if (has_disclosure(b, c->prop)) {
        r.left = (int16_t)(r.left + disclosure_indent(b, row));
        if (is_container(b, item)) {
            db_item *it = find_item(b, item);
            int top = r.top + (r.bottom - r.top - 12) / 2;
            ap_disclosure((qrect){ (int16_t)top, (int16_t)(r.left + 1), (int16_t)(top + 12), (int16_t)(r.left + 13) },
                          it && (it->state & kDataBrowserContainerIsOpen), active ? 1 : 0, false);
        }
        r.left = (int16_t)(r.left + DISCLOSURE_W);
    }
    if (c->type == kDataBrowserCustomType) {
        if (b->ccb[CC_DRAW]) {
            gaddr rp = tmp_rect(r);
            int depth = qd_port_pixels(qd_port()).depth;
            db_item *it = find_item(b, item);
            mm_call(b->ccb[CC_DRAW], piDrawItem,
                    (uint32_t[]){ b->ch, item, c->prop, it ? it->state : 0, rp, (uint32_t)depth, depth >= 4 }, 7);
            tb_DisposePtr(rp);
        }
        return;
    }
    if (b->edit_te && item == b->edit_item && c->prop == b->edit_prop)
        return; /* the edit text's own */
    if (fetch(b, item, c->prop) != noErr)
        return;
    uint32_t set = rd32(b->item_data + ID_SET);
    uint8_t t[256];
    switch (c->type) {
    case kDataBrowserTextType:
        text_of(b, t);
        draw_text(r, t, selected, active);
        break;
    case kDataBrowserIconAndTextType:
    case kDataBrowserIconType: {
        int sz = r.bottom - r.top >= 32 ? 32 : 16;
        qrect ir = { (int16_t)(r.top + (r.bottom - r.top - sz) / 2), (int16_t)(r.left + 2), 0, 0 };
        ir.bottom = (int16_t)(ir.top + sz);
        ir.right = (int16_t)(ir.left + sz);
        if ((set & SET_ICON) && rd32(b->item_data + ID_ICON)) {
            gaddr rp = tmp_rect(ir);
            tb_PlotIconRef(rp, 0, (int16_t)((selected ? 0x4000 : 0) | (set & SET_TRANSFORM ? rd16(b->item_data + ID_TRANSFORM) : 0)),
                           0, rd32(b->item_data + ID_ICON));
            tb_DisposePtr(rp);
        }
        if (c->type == kDataBrowserIconAndTextType) {
            text_of(b, t);
            draw_text((qrect){ r.top, (int16_t)(ir.right + 2), r.bottom, r.right }, t, selected, active);
        }
        break;
    }
    case kDataBrowserCheckboxType: {
        int top = r.top + (r.bottom - r.top - 12) / 2;
        int v = set & SET_BUTTON ? (int16_t)rd16(b->item_data + ID_BUTTON) : set & SET_BOOL ? rd8(b->item_data + ID_BOOL) : 0;
        ap_check_box((qrect){ (int16_t)top, (int16_t)(r.left + 3), (int16_t)(top + 12), (int16_t)(r.left + 15) }, false,
                     v == 1 ? 1 : v == 2 ? 2 : 0, active ? 1 : 0);
        break;
    }
    case kDataBrowserProgressBarType: case kDataBrowserRelevanceRankType: case kDataBrowserSliderType:
        draw_track(b, r, c->type, active);
        break;
    case kDataBrowserDateTimeType: {
        uint32_t secs = set & SET_DATE ? rd32(b->item_data + ID_DATE)
                        : set & SET_LONG_DATE ? rd32(b->item_data + ID_LONG_DATE + 4) : 0;
        gaddr s = tb_NewPtr(256, false, true);
        if (c->flags & (4 << 8)) /* kDataBrowserDateTimeTimeOnly */
            tb_IUTimeString((int32_t)secs, (c->flags & (8 << 8)) != 0, s);
        else
            tb_IUDateString((int32_t)secs, 0, s);
        gget(t, s, (uint32_t)rd8(s) + 1);
        if (!(c->flags & ((2 | 4) << 8))) { /* the date, and the time after it */
            tb_IUTimeString((int32_t)secs, (c->flags & (8 << 8)) != 0, s);
            int n = t[0], m = rd8(s);
            if (n + 1 + m < 256) {
                t[n + 1] = ' ';
                gget(t + n + 2, s + 1, (uint32_t)m);
                t[0] = (uint8_t)(n + 1 + m);
            }
        }
        tb_DisposePtr(s);
        draw_text(r, t, selected, active);
        break;
    }
    case kDataBrowserPopupMenuType: {
        gaddr menu = rd32(b->item_data + ID_MENU);
        if (menu && (set & SET_VALUE)) {
            gaddr s = tb_NewPtr(256, false, true);
            tb_GetMenuItemText(menu, (int16_t)rd32(b->item_data + ID_VALUE), s);
            gget(t, s, (uint32_t)rd8(s) + 1);
            tb_DisposePtr(s);
            draw_text((qrect){ r.top, r.left, r.bottom, (int16_t)(r.right - 14) }, t, selected, active);
            uint16_t c3[3];
            ap_text_rgb(active ? 1 : 2, c3);
            int top = r.top + (r.bottom - r.top - 9) / 2;
            ap_arrow((qrect){ (int16_t)top, (int16_t)(r.right - 13), (int16_t)(top + 9), (int16_t)(r.right - 4) }, 3, 9, c3);
        }
        break;
    }
    default:
        break;
    }
}

static void draw_header(databrowser *b)
{
    qrect h = header_rect(b);
    if (h.bottom <= h.top)
        return;
    int x = h.left - b->left;
    bool active = cactive(b->ch);
    for (int i = 0; i < b->ncols; i++) {
        db_column *c = &b->cols[i];
        int w = col_w(b, c);
        qrect r = { h.top, (int16_t)x, h.bottom, (int16_t)(x + w) };
        x += w;
        if (r.right <= h.left || r.left >= h.right)
            continue;
        bool sorted = c->prop == b->sort_prop;
        ap_bevel_button(r, 8, active ? 1 : 0, sorted); /* kThemeSmallBevelButton: the list header's look */
        uint16_t col[3];
        ap_text_rgb(active ? 1 : 2, col);
        qrect tr = { r.top, (int16_t)(r.left + 4 + c->title_offset), r.bottom, (int16_t)(r.right - (sorted ? 14 : 4)) };
        int n = c->title[0];
        while (n > 0 && qd_text_width(qd_port(), c->title + 1, n) > tr.right - tr.left)
            n--;
        ap_text_in(tr, c->title + 1, n, false, col);
        if (sorted && b->sort_order != kDataBrowserOrderUndefined) {
            int top = r.top + (r.bottom - r.top - 7) / 2;
            ap_arrow((qrect){ (int16_t)top, (int16_t)(r.right - 12), (int16_t)(top + 7), (int16_t)(r.right - 5) },
                     b->sort_order == kDataBrowserOrderIncreasing ? 3 : 1, 7, col); /* up for increasing */
        }
    }
    if (x < h.right) /* the header's end */
        ap_bevel_button((qrect){ h.top, (int16_t)x, h.bottom, h.right }, 8, active ? 1 : 0, false);
}

/* A scroll bar's thumb: where the scroll position is in its range. */
static qrect thumb_of(qrect bar, bool vertical, int pos, int range, int page)
{
    int len = (vertical ? bar.bottom - bar.top : bar.right - bar.left) - 2 * SCROLL_W - 2;
    if (range <= 0 || len < 16)
        return (qrect){ 0, 0, 0, 0 };
    int t = 16, at = (int)((long)pos * (len - t) / range);
    (void)page;
    int a = (vertical ? bar.top : bar.left) + SCROLL_W + 1 + at;
    return vertical ? (qrect){ (int16_t)a, (int16_t)(bar.left + 1), (int16_t)(a + t), (int16_t)(bar.right - 1) }
                    : (qrect){ (int16_t)(bar.top + 1), (int16_t)a, (int16_t)(bar.bottom - 1), (int16_t)(a + t) };
}

static void draw_bars(databrowser *b, int hilite_v, int hilite_h)
{
    qrect rr = rows_rect(b);
    bool active = cactive(b->ch);
    if (b->vscroll) {
        qrect v = vbar_rect(b);
        int range = total_height(b) - (rr.bottom - rr.top);
        ap_scroll_bar(v, true, hilite_v, active, range > 0, thumb_of(v, true, b->top, range, rr.bottom - rr.top));
    }
    if (b->hscroll) {
        qrect h = hbar_rect(b);
        int range = total_width(b) - (rr.right - rr.left);
        ap_scroll_bar(h, false, hilite_h, active, range > 0, thumb_of(h, false, b->left, range, rr.right - rr.left));
    }
    if (b->vscroll && b->hscroll) {
        qrect c = content_rect(b);
        ap_fill_grey((qrect){ (int16_t)(c.bottom - SCROLL_W), (int16_t)(c.right - SCROLL_W), c.bottom, c.right }, 0xDDDD);
    }
}

static void draw_rows(databrowser *b)
{
    layout(b);
    qrect rr = rows_rect(b);
    gaddr clip = tb_NewRgn();
    tb_GetClip(clip);
    gaddr rp = tmp_rect(rr);
    gaddr mine = tb_NewRgn();
    tb_RectRgn(mine, rp);
    tb_SectRgn(mine, clip, mine);
    tb_SetClip(mine);
    if (b->plain_bg || b->style == kDataBrowserColumnView)
        ap_fill_grey(rr, 0xFFFF);
    else /* the list view's: the sort column shaded */
        ap_fill_grey(rr, 0xFFFF);
    bool active = cactive(b->ch) && b->items_active;
    uint16_t hil[3];
    tb_GetThemeBrushAsColor(active ? 3 : 4, 32, 1, rp); /* kThemeBrushPrimaryHighlightColor, Secondary */
    hil[0] = rd16(rp);
    hil[1] = rd16(rp + 2);
    hil[2] = rd16(rp + 4);
    if (b->style == kDataBrowserColumnView) {
        /* the path's columns, each the items of the container before it */
        int x = rr.left - b->left;
        for (int k = 0; k <= b->path_len; k++, x += b->col_width) {
            uint32_t parent = k == 0 ? b->target : b->path[k - 1];
            int y = rr.top - (k == b->path_len ? b->top : 0);
            for (int i = 0; i < b->nitems && y < rr.bottom; i++) {
                if (b->items[i].parent != parent)
                    continue;
                uint32_t id = b->items[i].id;
                int h = row_h(b, id);
                qrect cell = { (int16_t)y, (int16_t)x, (int16_t)(y + h), (int16_t)(x + b->col_width - 1) };
                bool on = (k < b->path_len && b->path[k] == id) || (b->items[i].state & kDataBrowserItemIsSelected);
                if (on)
                    ap_fill(cell, hil);
                db_column c = { kDataBrowserItemSelfIdentityProperty, b->display_type, 0, 0, 0, 0, 0, { 0 }, 0 };
                draw_cell(b, id, &c, (qrect){ cell.top, cell.left, cell.bottom, (int16_t)(cell.right - 10) }, 0, on, active);
                if (is_container(b, id)) {
                    uint16_t c3[3];
                    ap_text_rgb(active ? 1 : 2, c3);
                    int top = cell.top + (h - 9) / 2;
                    ap_arrow((qrect){ (int16_t)top, (int16_t)(cell.right - 9), (int16_t)(top + 9), (int16_t)(cell.right - 4) }, 2, 9, c3);
                }
                y += h;
            }
            ap_fill_grey((qrect){ rr.top, (int16_t)(x + b->col_width - 1), rr.bottom, (int16_t)(x + b->col_width) }, 0xAAAA);
        }
    } else {
        int y = rr.top - b->top;
        for (int i = 0; i < b->nrows && y < rr.bottom; i++) {
            uint32_t id = b->rows[i];
            int h = row_h(b, id);
            if (y + h > rr.top) {
                db_item *it = find_item(b, id);
                bool sel = it && (it->state & kDataBrowserItemIsSelected);
                int x = rr.left - b->left;
                if (sel && b->hilite == kDataBrowserTableViewFillHilite)
                    ap_fill((qrect){ (int16_t)y, rr.left, (int16_t)(y + h), rr.right }, hil);
                for (int k = 0; k < b->ncols; k++) {
                    int w = col_w(b, &b->cols[k]);
                    qrect cell = { (int16_t)y, (int16_t)x, (int16_t)(y + h), (int16_t)(x + w) };
                    if (sel && b->hilite != kDataBrowserTableViewFillHilite && k == 0)
                        ap_fill((qrect){ cell.top, (int16_t)(cell.left + (has_disclosure(b, b->cols[k].prop) ? disclosure_indent(b, i) + DISCLOSURE_W : 0)),
                                         cell.bottom, cell.right }, hil);
                    if (cell.right > rr.left && cell.left < rr.right)
                        draw_cell(b, id, &b->cols[k], cell, i, sel, active);
                    x += w;
                }
            }
            y += h;
        }
    }
    tb_SetClip(clip);
    tb_DisposeRgn(mine);
    tb_DisposeRgn(clip);
    tb_DisposePtr(rp);
    if (b->edit_te) {
        gaddr vr = tmp_rect(rr);
        tb_TEUpdate(vr, b->edit_te);
        tb_DisposePtr(vr);
    }
}

static void draw(databrowser *b)
{
    if (!cvisible(b->ch))
        return;
    clamp_scroll(b);
    qrect r = crect(b->ch);
    bool active = cactive(b->ch);
    if (b->frame)
        ap_well_frame(r, active ? 1 : 0);
    draw_header(b);
    draw_rows(b);
    draw_bars(b, 0, 0);
    if (b->frame && b->focused)
        ap_focus_rect(inset(r, -2), true);
}

static void redraw(databrowser *b)
{
    if (!cvisible(b->ch))
        return;
    gaddr saved = qd_port();
    tb_SetPort(ControlRecord_contrlOwner(cp(b->ch)));
    draw(b);
    tb_SetPort(saved);
}

/* ---------------------------------------------------------------------- */
/* Sorting */

static uint32_t sort_prop_now;

static int compare_items(databrowser *b, uint32_t x, uint32_t y)
{
    if (b->cb[CB_COMPARE]) {
        bool lt = (mm_call(b->cb[CB_COMPARE], piItemCompare, (uint32_t[]){ b->ch, x, y, sort_prop_now }, 4) & 0xFF) != 0;
        if (lt)
            return -1;
        lt = (mm_call(b->cb[CB_COMPARE], piItemCompare, (uint32_t[]){ b->ch, y, x, sort_prop_now }, 4) & 0xFF) != 0;
        return lt ? 1 : 0;
    }
    /* By the property's data: text alphabetically, the rest by number. */
    db_column *c = column(b, sort_prop_now);
    uint32_t type = c ? c->type : kDataBrowserTextType;
    if (type == kDataBrowserTextType || type == kDataBrowserIconAndTextType) {
        uint8_t a[256], d[256];
        fetch(b, x, sort_prop_now);
        text_of(b, a);
        fetch(b, y, sort_prop_now);
        text_of(b, d);
        int n = a[0] < d[0] ? a[0] : d[0];
        for (int i = 1; i <= n; i++) {
            int p = a[i] >= 'A' && a[i] <= 'Z' ? a[i] + 32 : a[i], q = d[i] >= 'A' && d[i] <= 'Z' ? d[i] + 32 : d[i];
            if (p != q)
                return p < q ? -1 : 1;
        }
        return a[0] < d[0] ? -1 : a[0] > d[0] ? 1 : 0;
    }
    int32_t va, vb;
    uint32_t off = type == kDataBrowserDateTimeType ? ID_DATE : type == kDataBrowserCheckboxType ? ID_BUTTON : ID_VALUE;
    fetch(b, x, sort_prop_now);
    va = off == ID_BUTTON ? (int16_t)rd16(b->item_data + off) : (int32_t)rd32(b->item_data + off);
    fetch(b, y, sort_prop_now);
    vb = off == ID_BUTTON ? (int16_t)rd16(b->item_data + off) : (int32_t)rd32(b->item_data + off);
    return va < vb ? -1 : va > vb ? 1 : 0;
}

/* Sort a container's items (stable, keeping the others where they are). */
static void sort_container(databrowser *b, uint32_t container, uint32_t prop, bool recurse)
{
    if (!prop || b->sort_order == kDataBrowserOrderUndefined)
        return;
    int n = children(b, container, NULL, 0);
    if (n < 2) {
        if (recurse && n == 1) {
            uint32_t only;
            children(b, container, &only, 1);
            sort_container(b, only, prop, true);
        }
        return;
    }
    notify(b, container, kDataBrowserContainerSorting);
    uint32_t *ids = malloc((size_t)n * sizeof *ids);
    children(b, container, ids, n);
    sort_prop_now = prop;
    for (int i = 1; i < n; i++) /* insertion sort: stable, and few compares for sorted input */
        for (int k = i; k > 0; k--) {
            int c = compare_items(b, ids[k - 1], ids[k]);
            if (b->sort_order == kDataBrowserOrderDecreasing)
                c = -c;
            if (c <= 0)
                break;
            uint32_t t = ids[k - 1];
            ids[k - 1] = ids[k];
            ids[k] = t;
        }
    /* The container's items back in that order, where they were. */
    int k = 0;
    db_item *copy = malloc((size_t)n * sizeof *copy);
    for (int i = 0; i < n; i++)
        copy[i] = *find_item(b, ids[i]);
    for (int i = 0; i < b->nitems; i++)
        if (b->items[i].parent == container)
            b->items[i] = copy[k++];
    free(copy);
    if (recurse)
        for (int i = 0; i < n; i++)
            sort_container(b, ids[i], prop, true);
    free(ids);
    changed(b);
    notify(b, container, kDataBrowserContainerSorted);
}

/* ---------------------------------------------------------------------- */
/* Selection */

static void set_selected(databrowser *b, db_item *it, bool on, bool *any)
{
    bool was = (it->state & kDataBrowserItemIsSelected) != 0;
    if (was == on)
        return;
    if (on && !is_selectable(b, it->id))
        return;
    if (on)
        it->state |= kDataBrowserItemIsSelected;
    else
        it->state &= ~(uint32_t)kDataBrowserItemIsSelected;
    *any = true;
    notify(b, it->id, on ? kDataBrowserItemSelected : kDataBrowserItemDeselected);
}

static int selected_count(databrowser *b)
{
    int n = 0;
    for (int i = 0; i < b->nitems; i++)
        n += (b->items[i].state & kDataBrowserItemIsSelected) != 0;
    return n;
}

static void select_items(databrowser *b, const uint32_t *ids, int n, uint32_t op)
{
    bool any = false;
    if (op == kDataBrowserItemsAssign) {
        for (int i = 0; i < b->nitems; i++) {
            bool keep = false;
            for (int k = 0; k < n && !keep; k++)
                keep = ids[k] == b->items[i].id;
            if (!keep)
                set_selected(b, &b->items[i], false, &any);
        }
    }
    for (int k = 0; k < n; k++) {
        db_item *it = find_item(b, ids[k]);
        if (!it)
            continue;
        bool on = op == kDataBrowserItemsRemove ? false
                  : op == kDataBrowserItemsToggle ? !(it->state & kDataBrowserItemIsSelected) : true;
        if (on && (b->sel_flags & kDataBrowserSelectOnlyOne)) /* one only: the others go */
            for (int i = 0; i < b->nitems; i++)
                if (b->items[i].id != it->id)
                    set_selected(b, &b->items[i], false, &any);
        set_selected(b, it, on, &any);
    }
    if (any)
        notify(b, 0, kDataBrowserSelectionSetChanged);
}

/* ---------------------------------------------------------------------- */
/* Editing */

static void end_edit(databrowser *b, bool keep)
{
    if (!b->edit_te)
        return;
    uint32_t item = b->edit_item, prop = b->edit_prop;
    if (keep) {
        gaddr t = TERec_hText(rd32(b->edit_te));
        int32_t n = rd16(rd32(b->edit_te) + TERec_teLength_off);
        uint8_t buf[256];
        if (n > 255)
            n = 255;
        gget(buf, rd32(t), (uint32_t)n);
        gaddr cf = carbon_cfstring_from_text(buf, (size_t)n);
        gmemset(b->item_data, 0, ID_SIZE);
        wr32(b->item_data + ID_TEXT, cf);
        wr32(b->item_data + ID_SET, SET_TEXT);
        store(b, item, prop);
        carbon_cfrelease(cf);
    }
    tb_TEDispose(b->edit_te);
    b->edit_te = 0;
    b->edit_item = b->edit_prop = 0;
    notify(b, item, kDataBrowserEditStopped);
    redraw(b);
}

static int32_t start_edit(databrowser *b, uint32_t item, uint32_t prop)
{
    qrect cell;
    if (!cell_rect(b, item, prop, &cell))
        return errDataBrowserItemNotFound;
    if (fetch(b, item, prop) != noErr)
        return errDataBrowserPropertyNotSupported;
    uint8_t t[256];
    text_of(b, t);
    gaddr saved = qd_port();
    tb_SetPort(ControlRecord_contrlOwner(cp(b->ch)));
    int row = row_of(b, item);
    if (has_disclosure(b, prop))
        cell.left = (int16_t)(cell.left + disclosure_indent(b, row) + DISCLOSURE_W);
    qrect tr = { (int16_t)(cell.top + 1), (int16_t)(cell.left + 2), (int16_t)(cell.bottom - 1), (int16_t)(cell.right - 2) };
    gaddr r = tmp_rect(tr);
    b->edit_te = tb_TENew(r, r);
    tb_DisposePtr(r);
    gaddr p = tb_NewPtr(256, false, false);
    gput(p, t + 1, t[0]);
    tb_TESetText(p, t[0], b->edit_te);
    tb_DisposePtr(p);
    b->edit_item = item;
    b->edit_prop = prop;
    ap_fill_grey(tr, 0xFFFF);
    tb_TESetSelect(0, 32767, b->edit_te);
    tb_TEActivate(b->edit_te);
    gaddr vr = tmp_rect(tr);
    tb_TEUpdate(vr, b->edit_te);
    tb_DisposePtr(vr);
    ap_frame_grey(inset(tr, -1), 0);
    tb_SetPort(saved);
    notify(b, item, kDataBrowserEditStarted);
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* Clicks and keys */

static void scroll_to(databrowser *b, int32_t top, int32_t left)
{
    int32_t t0 = b->top, l0 = b->left;
    b->top = top;
    b->left = left;
    clamp_scroll(b);
    if ((b->top != t0 || b->left != l0) && b->edit_te) {
        gaddr saved = qd_port();
        tb_SetPort(ControlRecord_contrlOwner(cp(b->ch)));
        tb_TEScroll((int16_t)(l0 - b->left), (int16_t)(t0 - b->top), b->edit_te);
        tb_SetPort(saved);
    }
    if (b->top != t0 || b->left != l0)
        redraw(b);
}

static void reveal(databrowser *b, uint32_t item, bool center)
{
    int row = row_of(b, item);
    if (row < 0)
        return;
    qrect rr = rows_rect(b);
    int y = row_top(b, row), h = row_h(b, item), page = rr.bottom - rr.top;
    int32_t top = b->top;
    if (center)
        top = y - (page - h) / 2;
    else if (y < top)
        top = y;
    else if (y + h > top + page)
        top = y + h - page;
    scroll_to(b, top, b->left);
}

static void toggle_container(databrowser *b, uint32_t item);

/* Track a scroll bar: arrows and page areas repeat while the mouse is
 * down in them; the thumb follows the mouse. */
static void track_bar(databrowser *b, bool vertical, uint32_t pt)
{
    qrect bar = vertical ? vbar_rect(b) : hbar_rect(b), rr = rows_rect(b);
    int page = vertical ? rr.bottom - rr.top : rr.right - rr.left;
    int range = (vertical ? total_height(b) : total_width(b)) - page;
    if (range <= 0)
        return;
    int line = vertical ? b->row_height : 16;
    int at = vertical ? pt_v(pt) : pt_h(pt);
    int lo = (vertical ? bar.top : bar.left) + SCROLL_W, hi = (vertical ? bar.bottom : bar.right) - SCROLL_W;
    qrect thumb = thumb_of(bar, vertical, vertical ? b->top : b->left, range, page);
    if (in_rect(thumb, pt)) {
        int start = at, pos0 = vertical ? b->top : b->left, len = hi - lo - 2 - 16;
        while (ev_still_down()) {
            uint32_t m = ctl_local_mouse(b->ch);
            int d = (vertical ? pt_v(m) : pt_h(m)) - start;
            int pos = len > 0 ? pos0 + (int)((long)d * range / len) : pos0;
            if (vertical)
                scroll_to(b, pos, b->left);
            else
                scroll_to(b, b->top, pos);
            ev_wait_tick();
        }
        return;
    }
    int delta = at < lo ? -line : at >= hi ? line : 0;
    if (!delta) {
        int tmid = vertical ? (thumb.top + thumb.bottom) / 2 : (thumb.left + thumb.right) / 2;
        delta = at < tmid ? -(page - line) : page - line;
    }
    do {
        if (vertical)
            scroll_to(b, b->top + delta, b->left);
        else
            scroll_to(b, b->top, b->left + delta);
        gaddr saved = qd_port();
        tb_SetPort(ControlRecord_contrlOwner(cp(b->ch)));
        draw_bars(b, vertical ? (delta < 0 ? 20 : 21) : 0, vertical ? 0 : (delta < 0 ? 20 : 21));
        tb_SetPort(saved);
        ev_wait_tick();
    } while (ev_still_down());
    redraw(b);
}

/* A click in the header: sort by that column (again: the other way). */
static void click_header(databrowser *b, uint32_t pt)
{
    qrect h = header_rect(b);
    int x = h.left - b->left;
    for (int i = 0; i < b->ncols; i++) {
        int w = col_w(b, &b->cols[i]);
        if (pt_h(pt) >= x && pt_h(pt) < x + w) {
            db_column *c = &b->cols[i];
            if (!(c->flags & kDataBrowserListViewSortableColumn))
                return;
            if (b->sort_prop == c->prop)
                b->sort_order = b->sort_order == kDataBrowserOrderIncreasing ? kDataBrowserOrderDecreasing
                                                                             : kDataBrowserOrderIncreasing;
            else {
                b->sort_prop = c->prop;
                if (c->initial_order)
                    b->sort_order = c->initial_order;
                else if (!b->sort_order)
                    b->sort_order = kDataBrowserOrderIncreasing;
            }
            sort_container(b, b->target, b->sort_prop, true);
            notify(b, 0, kDataBrowserUserStateChanged);
            redraw(b);
            return;
        }
        x += w;
    }
}

/* Which row and column a point is in; -1 for none. */
static int hit_row(databrowser *b, uint32_t pt, int *col)
{
    layout(b);
    qrect rr = rows_rect(b);
    if (!in_rect(rr, pt))
        return -1;
    int y = rr.top - b->top;
    for (int i = 0; i < b->nrows; i++) {
        int h = row_h(b, b->rows[i]);
        if (pt_v(pt) >= y && pt_v(pt) < y + h) {
            int x = rr.left - b->left;
            *col = -1;
            for (int k = 0; k < b->ncols; k++) {
                int w = col_w(b, &b->cols[k]);
                if (pt_h(pt) >= x && pt_h(pt) < x + w)
                    *col = k;
                x += w;
            }
            return i;
        }
        y += h;
    }
    return -1;
}

static void click_rows(databrowser *b, uint32_t pt, uint16_t mods)
{
    int col, row = hit_row(b, pt, &col);
    if (b->edit_te) {
        qrect er = qrect_get(rd32(b->edit_te) + TERec_viewRect_off);
        if (in_rect(er, pt)) {
            tb_TEClick(pt, (mods & 0x0200) != 0, b->edit_te);
            return;
        }
        end_edit(b, true);
    }
    if (row < 0) { /* below the rows: nothing selected (unless never empty) */
        if (!(b->sel_flags & kDataBrowserNeverEmptySelectionSet) && !(mods & 0x0300))
            select_items(b, NULL, 0, kDataBrowserItemsAssign);
        redraw(b);
        return;
    }
    uint32_t id = b->rows[row];
    db_column *c = col >= 0 ? &b->cols[col] : NULL;
    /* the disclosure triangle */
    if (c && has_disclosure(b, c->prop) && is_container(b, id)) {
        qrect cell;
        cell_rect(b, id, c->prop, &cell);
        int x = cell.left + disclosure_indent(b, row);
        if (pt_h(pt) >= x && pt_h(pt) < x + DISCLOSURE_W) {
            notify(b, id, kDataBrowserUserToggledContainer);
            toggle_container(b, id);
            return;
        }
    }
    /* a check box that can be changed */
    if (c && c->type == kDataBrowserCheckboxType && (c->flags & kDataBrowserPropertyIsMutable)) {
        fetch(b, id, c->prop);
        uint32_t set = rd32(b->item_data + ID_SET);
        int v = set & SET_BUTTON ? (int16_t)rd16(b->item_data + ID_BUTTON) : rd8(b->item_data + ID_BOOL);
        v = v == 1 ? 0 : 1;
        gmemset(b->item_data, 0, ID_SIZE);
        wr16(b->item_data + ID_BUTTON, (uint16_t)v);
        wr8(b->item_data + ID_BOOL, (uint8_t)v);
        wr32(b->item_data + ID_SET, SET_BUTTON | SET_BOOL);
        store(b, id, c->prop);
        redraw(b);
        return;
    }
    db_item *it = find_item(b, id);
    bool was = it && (it->state & kDataBrowserItemIsSelected);
    uint32_t now = rd32(LM_Ticks);
    bool dbl = id == b->last_item && now - b->last_click <= rd32(LM_DoubleTime);
    b->last_click = now;
    b->last_item = id;
    bool cmd = (mods & 0x0100) != 0, shift = (mods & 0x0200) != 0;
    if (b->sel_flags & kDataBrowserAlwaysExtendSelection)
        shift = true;
    if ((cmd && (b->sel_flags & kDataBrowserCmdTogglesSelection)) || (shift && was && !(b->sel_flags & kDataBrowserSelectOnlyOne))) {
        if (!(was && selected_count(b) == 1 && (b->sel_flags & kDataBrowserNeverEmptySelectionSet)))
            select_items(b, &id, 1, kDataBrowserItemsToggle);
    } else if (shift && !(b->sel_flags & kDataBrowserSelectOnlyOne) && b->anchor && row_of(b, b->anchor) >= 0) {
        int a = row_of(b, b->anchor), lo = a < row ? a : row, hi = a < row ? row : a;
        select_items(b, b->rows + lo, hi - lo + 1, kDataBrowserItemsAdd);
    } else if (!was || selected_count(b) > 1) {
        select_items(b, &id, 1, kDataBrowserItemsAssign);
    }
    if (!shift)
        b->anchor = id;
    redraw(b);
    if (dbl) {
        notify(b, id, kDataBrowserItemDoubleClicked);
        b->last_item = 0;
    } else if (was && c && (c->flags & kDataBrowserPropertyIsMutable) &&
               (c->type == kDataBrowserTextType || c->type == kDataBrowserIconAndTextType) &&
               item_bool(b, id, kDataBrowserItemIsEditableProperty, false)) {
        start_edit(b, id, c->prop);
    }
}

/* A click in the column view: the item chosen, a container's items in
 * the next column. */
static void click_columns(databrowser *b, uint32_t pt, uint16_t mods)
{
    qrect rr = rows_rect(b);
    if (!in_rect(rr, pt))
        return;
    int k = (pt_h(pt) - rr.left + b->left) / (b->col_width ? b->col_width : 1);
    if (k < 0 || k > b->path_len)
        return;
    uint32_t parent = k == 0 ? b->target : b->path[k - 1];
    int y = rr.top - (k == b->path_len ? b->top : 0);
    for (int i = 0; i < b->nitems; i++) {
        if (b->items[i].parent != parent)
            continue;
        uint32_t id = b->items[i].id;
        int h = row_h(b, id);
        if (pt_v(pt) >= y && pt_v(pt) < y + h) {
            b->path_len = k;
            uint32_t now = rd32(LM_Ticks);
            bool dbl = id == b->last_item && now - b->last_click <= rd32(LM_DoubleTime);
            b->last_click = now;
            b->last_item = id;
            select_items(b, &id, 1, kDataBrowserItemsAssign);
            if (is_container(b, id)) {
                b->path = realloc(b->path, (size_t)(b->path_len + 1) * sizeof *b->path);
                b->path[b->path_len++] = id;
                db_item *it = find_item(b, id);
                if (it && !(it->state & kDataBrowserContainerIsOpen)) {
                    it->state |= kDataBrowserContainerIsOpen;
                    notify(b, id, kDataBrowserContainerOpened);
                }
                notify(b, id, kDataBrowserTargetChanged);
            }
            changed(b);
            redraw(b);
            if (dbl)
                notify(b, id, kDataBrowserItemDoubleClicked);
            (void)mods;
            return;
        }
        y += h;
    }
}

static int16_t track(databrowser *b, uint32_t pt, uint16_t mods)
{
    if (!cactive(b->ch))
        return 0;
    if (b->vscroll && in_rect(vbar_rect(b), pt)) {
        track_bar(b, true, pt);
        return kDataBrowserPart;
    }
    if (b->hscroll && in_rect(hbar_rect(b), pt)) {
        track_bar(b, false, pt);
        return kDataBrowserPart;
    }
    if (header_h(b) && in_rect(header_rect(b), pt)) {
        click_header(b, pt);
        return kDataBrowserPart;
    }
    if (b->style == kDataBrowserColumnView)
        click_columns(b, pt, mods);
    else
        click_rows(b, pt, mods);
    return kDataBrowserPart;
}

/* Arrows move the selection; left and right close and open containers. */
static int16_t key(databrowser *b, gaddr rec)
{
    int16_t mods = (int16_t)rd16(rec), chr = (int16_t)rd16(rec + 4);
    if (b->edit_te) {
        if (chr == '\r' || chr == 3 || chr == '\t') {
            end_edit(b, true);
            return kDataBrowserPart;
        }
        if (chr == 27) {
            end_edit(b, false);
            return kDataBrowserPart;
        }
        gaddr saved = qd_port();
        tb_SetPort(ControlRecord_contrlOwner(cp(b->ch)));
        tb_TEKey(chr, b->edit_te);
        tb_SetPort(saved);
        return kDataBrowserPart;
    }
    layout(b);
    int cur = -1;
    for (int i = 0; i < b->nrows; i++) {
        db_item *it = find_item(b, b->rows[i]);
        if (it && (it->state & kDataBrowserItemIsSelected)) {
            cur = i;
            if (chr == 30)
                break;
        }
    }
    if (chr == 30 || chr == 31) { /* up, down */
        int next = cur < 0 ? (chr == 30 ? b->nrows - 1 : 0) : cur + (chr == 30 ? -1 : 1);
        if (next >= 0 && next < b->nrows) {
            uint32_t id = b->rows[next];
            select_items(b, &id, 1, (mods & 0x0200) && !(b->sel_flags & kDataBrowserSelectOnlyOne) ? kDataBrowserItemsAdd
                                                                                                   : kDataBrowserItemsAssign);
            reveal(b, id, false);
            redraw(b);
        }
    } else if ((chr == 28 || chr == 29) && cur >= 0 && is_container(b, b->rows[cur])) { /* left, right */
        db_item *it = find_item(b, b->rows[cur]);
        bool open = it && (it->state & kDataBrowserContainerIsOpen);
        if (open == (chr == 28))
            toggle_container(b, b->rows[cur]);
    }
    return kDataBrowserPart;
}

/* ---------------------------------------------------------------------- */
/* Containers */

static void remove_item(databrowser *b, uint32_t id, bool *sel_changed);

static void remove_children(databrowser *b, uint32_t container, bool *sel_changed)
{
    for (int i = 0; i < b->nitems;) {
        if (b->items[i].parent == container) {
            remove_item(b, b->items[i].id, sel_changed);
            i = 0; /* the array changed */
        } else {
            i++;
        }
    }
}

static void remove_item(databrowser *b, uint32_t id, bool *sel_changed)
{
    remove_children(b, id, sel_changed);
    db_item *it = find_item(b, id);
    if (!it)
        return;
    if (it->state & kDataBrowserItemIsSelected) {
        *sel_changed = true;
        notify(b, id, kDataBrowserItemDeselected);
    }
    if (b->edit_item == id)
        end_edit(b, false);
    *it = b->items[--b->nitems];
    notify(b, id, kDataBrowserItemRemoved);
    changed(b);
}

static int32_t open_container(databrowser *b, uint32_t id)
{
    db_item *it = find_item(b, id);
    if (!it)
        return errDataBrowserItemNotFound;
    if (it->state & kDataBrowserContainerIsOpen)
        return noErr;
    if (!item_bool(b, id, kDataBrowserContainerIsOpenableProperty, true))
        return noErr;
    it->state |= kDataBrowserContainerIsOpen;
    changed(b);
    notify(b, id, kDataBrowserContainerOpened);
    sort_container(b, id, b->sort_prop, false);
    redraw(b);
    return noErr;
}

static int32_t close_container(databrowser *b, uint32_t id)
{
    db_item *it = find_item(b, id);
    if (!it)
        return errDataBrowserItemNotFound;
    if (!(it->state & kDataBrowserContainerIsOpen))
        return noErr;
    if (!item_bool(b, id, kDataBrowserContainerIsClosableProperty, true))
        return noErr;
    notify(b, id, kDataBrowserContainerClosing);
    bool sel = false;
    remove_children(b, id, &sel);
    it = find_item(b, id);
    if (it)
        it->state &= ~(uint32_t)kDataBrowserContainerIsOpen;
    changed(b);
    notify(b, id, kDataBrowserContainerClosed);
    if (sel)
        notify(b, 0, kDataBrowserSelectionSetChanged);
    redraw(b);
    return noErr;
}

static void toggle_container(databrowser *b, uint32_t id)
{
    db_item *it = find_item(b, id);
    if (it && (it->state & kDataBrowserContainerIsOpen))
        close_container(b, id);
    else
        open_container(b, id);
}

/* ---------------------------------------------------------------------- */
/* The definition procedure */

static void dispose_browser(databrowser *b)
{
    if (b->edit_te)
        tb_TEDispose(b->edit_te);
    if (b->item_data)
        tb_DisposePtr(b->item_data);
    for (int i = 0; i < MAX_BROWSERS; i++)
        if (browsers[i] == b)
            browsers[i] = NULL;
    free(b->items);
    free(b->cols);
    free(b->pflags);
    free(b->rows);
    free(b->depth);
    free(b->path);
    free(b);
}

static int32_t data_access(databrowser *b, gaddr rec, bool set)
{
    uint32_t tag = rd32(rec);
    int32_t size = (int32_t)rd32(rec + 8);
    gaddr data = rd32(rec + 12);
    if (tag == TAG('b', 'r', 'd', 'r')) { /* kControlDataBrowserIncludesFrameAndFocusTag */
        if (set) {
            if (size >= 1 && data)
                b->frame = rd8(data) != 0;
        } else {
            if (data && size >= 1)
                wr8(data, b->frame);
            wr32(rec + 8, 1);
        }
        return noErr;
    }
    if (tag == TAG('f', 'l', 't', 'r') || tag == TAG('v', 'a', 'l', 'i')) {
        gaddr *slot = tag == TAG('f', 'l', 't', 'r') ? &b->key_filter : &b->validation;
        if (set) {
            if (size >= 4 && data)
                *slot = rd32(data);
        } else {
            if (data && size >= 4)
                wr32(data, *slot);
            wr32(rec + 8, 4);
        }
        return noErr;
    }
    return errDataNotSupported;
}

static void browser_cdef(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    int32_t param = (int32_t)cpu_pop32(c);
    int16_t msg = (int16_t)cpu_pop16(c);
    gaddr ch = cpu_pop32(c);
    cpu_pop16(c);
    databrowser *b = db_of(ch);
    int32_t result = 0;
    if (b) {
        switch (msg) {
        case drawCntl: draw(b); break;
        case testCntl:
            result = cvisible(ch) && cactive(ch) && in_rect(crect(ch), (uint32_t)param) ? kDataBrowserPart : 0;
            break;
        case calcCRgns: case 10: {
            qrect r = crect(ch);
            tb_SetRectRgn((gaddr)param & 0x7FFFFFFF, r.left, r.top, r.right, r.bottom);
            break;
        }
        case dispCntl: dispose_browser(b); break;
        case kControlMsgTestNewMsgSupport: result = NEW_MSG_OK; break;
        case kControlMsgGetFeatures:
            result = kControlSupportsFocus | kControlWantsIdle | kControlWantsActivate | kControlHandlesTracking |
                     kControlSupportsDataAccess | kControlGetsFocusOnClick | kControlSupportsContextualMenus |
                     kControlSupportsClickActivation;
            break;
        case kControlMsgHandleTracking: /* ControlTrackingRec: startPt, modifiers, action */
            result = track(b, rd32((gaddr)param), rd16((gaddr)param + 4));
            break;
        case kControlMsgKeyDown:
            if (b->key_filter) {
                uint32_t args[4] = { ch, (gaddr)param + 2, (gaddr)param + 4, (gaddr)param };
                if (!mm_call(b->key_filter, 0x00003FE0, args, 4))
                    break;
            }
            result = key(b, (gaddr)param);
            break;
        case kControlMsgFocus:
            if (param == 0) {
                if (b->edit_te)
                    end_edit(b, true);
                b->focused = false;
                if (cvisible(ch))
                    ap_focus_rect(inset(crect(ch), -2), false);
                result = 0;
            } else if (b->focused && param < 0) {
                result = 0;
            } else {
                b->focused = true;
                if (cvisible(ch) && b->frame)
                    ap_focus_rect(inset(crect(ch), -2), true);
                result = kDataBrowserPart;
            }
            break;
        case kControlMsgIdle:
            if (b->edit_te)
                tb_TEIdle(b->edit_te);
            break;
        case kControlMsgActivate:
            if (b->edit_te) {
                if (param)
                    tb_TEActivate(b->edit_te);
                else
                    tb_TEDeactivate(b->edit_te);
            }
            draw(b);
            break;
        case kControlMsgSetData: result = data_access(b, (gaddr)param, true); break;
        case kControlMsgGetData: result = data_access(b, (gaddr)param, false); break;
        case kControlMsgGetClickActivation: /* ControlClickActivationRec: point, modifiers, result */
            wr32((gaddr)param + 6, 3); /* kActivateAndHandleClick */
            break;
        case kControlMsgContextualMenuClick: { /* ControlContextualMenuClickRec: localPoint, menuDisplayed */
            wr8((gaddr)param + 4, 0);
            if (!b->cb[CB_GET_MENU])
                break;
            gaddr out = tb_NewPtr(20, false, true); /* menu, help type, help string, AEDesc (8) */
            mm_call(b->cb[CB_GET_MENU], piGetContextualMenu, (uint32_t[]){ ch, out, out + 4, out + 8, out + 12 }, 5);
            gaddr menu = rd32(out);
            if (menu) {
                gaddr g = tb_NewPtr(10, false, true);
                wr32(g, rd32((gaddr)param));
                tb_LocalToGlobal(g);
                int32_t r = tb_PopUpMenuSelect(menu, (int16_t)pt_v(rd32(g)), (int16_t)pt_h(rd32(g)), 0);
                tb_DisposePtr(g);
                wr8((gaddr)param + 4, 1);
                if (r && b->cb[CB_SELECT_MENU]) /* kCMMenuItemSelected 1 */
                    mm_call(b->cb[CB_SELECT_MENU], piSelectContextualMenu,
                            (uint32_t[]){ ch, menu, r ? 1u : 0u, (uint32_t)(uint16_t)(r >> 16), (uint32_t)(uint16_t)r }, 5);
            }
            tb_DisposePtr(out);
            break;
        }
        default: break;
        }
    }
    wr32(c->a[7], (uint32_t)result);
    cpu_push32(c, ret);
}

/* The definition handle: `NATIVE index; RTS`, as the system's CDEFs. */
static gaddr cdef_handle(void)
{
    if (!cdef_stub)
        cdef_stub = native_register(browser_cdef, "Data Browser", 0);
    int idx = native_index_for(cdef_stub);
    gaddr h = tb_NewHandle(8, true, false);
    if (!h)
        return 0;
    uint8_t s[8] = { 0x71, 0xFF, (uint8_t)(idx >> 8), (uint8_t)idx, 0x4E, 0x75, 0x4E, 0x71 };
    gput(rd32(h), s, 8);
    tb_HLock(h);
    return h;
}

/* ---------------------------------------------------------------------- */
/* The calls */

#define BROWSER(ch)                       \
    databrowser *b = db_of(ch);           \
    if (!b)                               \
        return errUnknownControl

int32_t tb_CreateDataBrowserControl(gaddr w, gaddr bounds, int32_t style_in, gaddr out)
{
    uint32_t style = (uint32_t)style_in;
    if (out)
        wr32(out, 0);
    if (!w || !bounds || !out)
        return paramErr;
    if (style != kDataBrowserListView && style != kDataBrowserColumnView)
        return paramErr;
    int slot = 0;
    while (slot < MAX_BROWSERS && browsers[slot])
        slot++;
    if (slot == MAX_BROWSERS)
        return memFullErr;
    databrowser *b = calloc(1, sizeof *b);
    b->style = style;
    b->row_height = 18;
    b->col_width = 150;
    b->header_height = 17;
    b->hilite = kDataBrowserTableViewFillHilite;
    b->sort_order = kDataBrowserOrderIncreasing;
    b->items_active = true;
    b->vscroll = true;
    b->hscroll = style == kDataBrowserColumnView;
    b->frame = true;
    b->display_type = kDataBrowserTextType;
    b->sel_flags = kDataBrowserCmdTogglesSelection;
    b->item_data = tb_NewPtr(ID_SIZE, true, true);
    browsers[slot] = b;
    gaddr defh = cdef_handle();
    /* The control made invisible, drawn once it is ours. */
    gaddr ch = ctl_new(w, bounds, 0, false, 0, 0, 0, 0, -1, defh, 0, 0);
    if (!ch) {
        browsers[slot] = NULL;
        tb_DisposePtr(b->item_data);
        free(b);
        return memFullErr;
    }
    b->ch = ch;
    tb_ShowControl(ch);
    wr32(out, ch);
    return noErr;
}

int32_t tb_GetDataBrowserViewStyle(gaddr ch, gaddr out)
{
    BROWSER(ch);
    if (out)
        wr32(out, b->style);
    return noErr;
}

int32_t tb_SetDataBrowserViewStyle(gaddr ch, int32_t style_in)
{
    uint32_t style = (uint32_t)style_in;
    BROWSER(ch);
    if (style != kDataBrowserListView && style != kDataBrowserColumnView)
        return paramErr;
    b->style = style;
    changed(b);
    redraw(b);
    return noErr;
}

int32_t tb_AddDataBrowserItems(gaddr ch, uint32_t container, uint32_t n, gaddr ids, uint32_t presort)
{
    BROWSER(ch);
    if (container && !find_item(b, container))
        return errDataBrowserItemNotFound;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t id = ids ? rd32(ids + 4 * i) : i + 1;
        if (!id || find_item(b, id))
            continue;
        if (b->nitems == b->cap_items) {
            b->cap_items = b->cap_items ? b->cap_items * 2 : 64;
            b->items = realloc(b->items, (size_t)b->cap_items * sizeof *b->items);
        }
        b->items[b->nitems++] = (db_item){ id, container, 0, 0 };
        notify(b, id, kDataBrowserItemAdded);
    }
    changed(b);
    /* in the sort order (presorted in it or not, the result is the same) */
    (void)presort;
    sort_container(b, container, b->sort_prop, false);
    redraw(b);
    return noErr;
}

int32_t tb_RemoveDataBrowserItems(gaddr ch, uint32_t container, uint32_t n, gaddr ids, uint32_t presort)
{
    BROWSER(ch);
    bool sel = false;
    if (!ids || !n) { /* all of the container's */
        remove_children(b, container, &sel);
    } else {
        for (uint32_t i = 0; i < n; i++)
            remove_item(b, rd32(ids + 4 * i), &sel);
    }
    if (sel)
        notify(b, 0, kDataBrowserSelectionSetChanged);
    (void)presort;
    clamp_scroll(b);
    redraw(b);
    return noErr;
}

int32_t tb_UpdateDataBrowserItems(gaddr ch, uint32_t container, uint32_t n, gaddr ids, uint32_t presort, uint32_t prop)
{
    BROWSER(ch);
    if (prop == b->sort_prop || presort != b->sort_prop)
        sort_container(b, container, b->sort_prop, false);
    (void)n;
    (void)ids;
    redraw(b);
    return noErr;
}

int8_t tb_EnableDataBrowserEditCommand(gaddr ch, uint32_t command)
{
    databrowser *b = db_of(ch);
    if (!b)
        return false;
    if (b->edit_te) {
        gaddr t = rd32(b->edit_te);
        bool range = TERec_selStart(t) != TERec_selEnd(t);
        return command == kDataBrowserEditMsgSelectAll || command == kDataBrowserEditMsgPaste ||
               ((command == kDataBrowserEditMsgCut || command == kDataBrowserEditMsgCopy || command == kDataBrowserEditMsgClear) && range);
    }
    return command == kDataBrowserEditMsgSelectAll && b->nitems > 0 && !(b->sel_flags & kDataBrowserSelectOnlyOne);
}

int32_t tb_ExecuteDataBrowserEditCommand(gaddr ch, uint32_t command)
{
    BROWSER(ch);
    if (!tb_EnableDataBrowserEditCommand(ch, command))
        return paramErr;
    if (b->edit_te) {
        gaddr saved = qd_port();
        tb_SetPort(ControlRecord_contrlOwner(cp(ch)));
        switch (command) {
        case kDataBrowserEditMsgCut: tb_TECut(b->edit_te); break;
        case kDataBrowserEditMsgCopy: tb_TECopy(b->edit_te); break;
        case kDataBrowserEditMsgPaste: tb_TEPaste(b->edit_te); break;
        case kDataBrowserEditMsgClear: tb_TEDelete(b->edit_te); break;
        default: tb_TESetSelect(0, 32767, b->edit_te); break;
        }
        tb_SetPort(saved);
        return noErr;
    }
    layout(b);
    select_items(b, b->rows, b->nrows, kDataBrowserItemsAdd);
    redraw(b);
    return noErr;
}

int32_t tb_GetDataBrowserSelectionAnchor(gaddr ch, gaddr first, gaddr last)
{
    BROWSER(ch);
    layout(b);
    uint32_t f = 0, l = 0;
    for (int i = 0; i < b->nrows; i++) {
        db_item *it = find_item(b, b->rows[i]);
        if (it && (it->state & kDataBrowserItemIsSelected)) {
            if (!f)
                f = it->id;
            l = it->id;
        }
    }
    if (first)
        wr32(first, f);
    if (last)
        wr32(last, l);
    return noErr;
}

/* kDataBrowserSelectionAnchorUp 0, Down 1, Left 2, Right 3 */
int32_t tb_MoveDataBrowserSelectionAnchor(gaddr ch, uint32_t direction, int8_t extend)
{
    BROWSER(ch);
    uint8_t rec[6] = { 0, (uint8_t)(extend ? 0x02 : 0), 0, 0, 0, (uint8_t)(direction == 0 ? 30 : direction == 1 ? 31 : direction == 2 ? 28 : 29) };
    gaddr r = tb_NewPtr(6, false, true);
    gput(r, rec, 6);
    key(b, r);
    tb_DisposePtr(r);
    return noErr;
}

int32_t tb_OpenDataBrowserContainer(gaddr ch, uint32_t container)
{
    BROWSER(ch);
    return open_container(b, container);
}

int32_t tb_CloseDataBrowserContainer(gaddr ch, uint32_t container)
{
    BROWSER(ch);
    return close_container(b, container);
}

int32_t tb_SortDataBrowserContainer(gaddr ch, uint32_t container, int8_t sort_children)
{
    BROWSER(ch);
    sort_container(b, container, b->sort_prop, sort_children != 0);
    redraw(b);
    return noErr;
}

static bool state_matches(uint32_t item_state, uint32_t want)
{
    if (want == kDataBrowserItemAnyState)
        return true;
    if (!want)
        return item_state == 0;
    return (item_state & want) == want;
}

/* The container's items in that state (and theirs, recursing). */
static int matching(databrowser *b, uint32_t container, bool recurse, uint32_t state, uint32_t *out, int max, int n)
{
    for (int i = 0; i < b->nitems; i++) {
        if (b->items[i].parent != container)
            continue;
        if (state_matches(b->items[i].state, state)) {
            if (out && n < max)
                out[n] = b->items[i].id;
            n++;
        }
        if (recurse)
            n = matching(b, b->items[i].id, true, state, out, max, n);
    }
    return n;
}

int32_t tb_GetDataBrowserItems(gaddr ch, uint32_t container, int8_t recurse, uint32_t state, gaddr h)
{
    BROWSER(ch);
    if (!h)
        return paramErr;
    int n = matching(b, container, recurse != 0, state, NULL, 0, 0);
    uint32_t *ids = malloc((size_t)(n ? n : 1) * sizeof *ids);
    matching(b, container, recurse != 0, state, ids, n, 0);
    tb_SetHandleSize(h, n * 4);
    if (rd16(LM_MemErr)) {
        free(ids);
        return memFullErr;
    }
    for (int i = 0; i < n; i++)
        wr32(rd32(h) + 4 * (uint32_t)i, ids[i]);
    free(ids);
    return noErr;
}

int32_t tb_GetDataBrowserItemCount(gaddr ch, uint32_t container, int8_t recurse, uint32_t state, gaddr out)
{
    BROWSER(ch);
    if (out)
        wr32(out, (uint32_t)matching(b, container, recurse != 0, state, NULL, 0, 0));
    return noErr;
}

int32_t tb_ForEachDataBrowserItem(gaddr ch, uint32_t container, int8_t recurse, uint32_t state, gaddr proc, gaddr client)
{
    BROWSER(ch);
    if (!proc)
        return paramErr;
    int n = matching(b, container, recurse != 0, state, NULL, 0, 0);
    uint32_t *ids = malloc((size_t)(n ? n : 1) * sizeof *ids);
    matching(b, container, recurse != 0, state, ids, n, 0);
    for (int i = 0; i < n; i++) {
        db_item *it = find_item(b, ids[i]);
        if (it)
            mm_call(proc, piItem, (uint32_t[]){ ids[i], it->state, client }, 3);
    }
    free(ids);
    return noErr;
}

int8_t tb_IsDataBrowserItemSelected(gaddr ch, uint32_t item)
{
    databrowser *b = db_of(ch);
    db_item *it = b ? find_item(b, item) : NULL;
    return it && (it->state & kDataBrowserItemIsSelected);
}

int32_t tb_GetDataBrowserItemState(gaddr ch, uint32_t item, gaddr out)
{
    BROWSER(ch);
    db_item *it = find_item(b, item);
    if (!it)
        return errDataBrowserItemNotFound;
    if (out)
        wr32(out, it->state);
    return noErr;
}

int32_t tb_RevealDataBrowserItem(gaddr ch, uint32_t item, uint32_t prop, uint8_t options)
{
    BROWSER(ch);
    if (!find_item(b, item))
        return errDataBrowserItemNotFound;
    (void)prop;
    reveal(b, item, (options & kDataBrowserRevealAndCenterInView) != 0);
    return noErr;
}

int32_t tb_SetDataBrowserSelectedItems(gaddr ch, uint32_t n, gaddr ids, uint32_t op)
{
    BROWSER(ch);
    uint32_t *list = malloc((size_t)(n ? n : 1) * sizeof *list);
    for (uint32_t i = 0; i < n; i++)
        list[i] = ids ? rd32(ids + 4 * i) : 0;
    select_items(b, list, (int)n, op);
    free(list);
    redraw(b);
    return noErr;
}

/* The user state: a CFData of the sort and the columns' widths. */
int32_t tb_SetDataBrowserUserState(gaddr ch, gaddr data)
{
    BROWSER(ch);
    if (!data)
        return paramErr;
    uint32_t len = carbon_call_export("CFDataGetLength", (uint32_t[]){ data }, 1);
    gaddr p = carbon_call_export("CFDataGetBytePtr", (uint32_t[]){ data }, 1);
    if (!p || len < 12 || rd32(p) != TAG('d', 'b', 'u', 's'))
        return paramErr;
    b->sort_prop = rd32(p + 4);
    b->sort_order = rd16(p + 8);
    int n = rd16(p + 10);
    for (int i = 0; i < n && 12 + 6 * (uint32_t)(i + 1) <= len; i++) {
        db_column *c = column(b, rd32(p + 12 + 6 * (uint32_t)i));
        if (c)
            c->width = rd16(p + 16 + 6 * (uint32_t)i);
    }
    sort_container(b, b->target, b->sort_prop, true);
    redraw(b);
    return noErr;
}

int32_t tb_GetDataBrowserUserState(gaddr ch, gaddr out)
{
    BROWSER(ch);
    if (!out)
        return paramErr;
    uint32_t len = 12 + 6 * (uint32_t)b->ncols;
    gaddr p = tb_NewPtr((int32_t)len, false, true);
    wr32(p, TAG('d', 'b', 'u', 's'));
    wr32(p + 4, b->sort_prop);
    wr16(p + 8, b->sort_order);
    wr16(p + 10, (uint16_t)b->ncols);
    for (int i = 0; i < b->ncols; i++) {
        wr32(p + 12 + 6 * (uint32_t)i, b->cols[i].prop);
        wr16(p + 16 + 6 * (uint32_t)i, b->cols[i].width);
    }
    gaddr d = carbon_call_export("CFDataCreate", (uint32_t[]){ 0, p, len }, 3);
    tb_DisposePtr(p);
    wr32(out, d);
    return d ? noErr : paramErr; /* no Core Foundation data: nothing to give */
}

int32_t tb_SetDataBrowserActiveItems(gaddr ch, int8_t active)
{
    BROWSER(ch);
    b->items_active = active != 0;
    redraw(b);
    return noErr;
}

int32_t tb_GetDataBrowserActiveItems(gaddr ch, gaddr out)
{
    BROWSER(ch);
    if (out)
        wr8(out, b->items_active);
    return noErr;
}

int32_t tb_SetDataBrowserScrollBarInset(gaddr ch, gaddr r)
{
    BROWSER(ch);
    if (r)
        b->bar_inset = qrect_get(r);
    redraw(b);
    return noErr;
}

int32_t tb_GetDataBrowserScrollBarInset(gaddr ch, gaddr r)
{
    BROWSER(ch);
    if (r)
        qrect_put(r, b->bar_inset);
    return noErr;
}

int32_t tb_SetDataBrowserTarget(gaddr ch, uint32_t target)
{
    BROWSER(ch);
    if (target && !find_item(b, target))
        return errDataBrowserItemNotFound;
    b->target = target;
    b->path_len = 0;
    b->top = b->left = 0;
    changed(b);
    notify(b, target, kDataBrowserTargetChanged);
    redraw(b);
    return noErr;
}

int32_t tb_GetDataBrowserTarget(gaddr ch, gaddr out)
{
    BROWSER(ch);
    if (out)
        wr32(out, b->style == kDataBrowserColumnView && b->path_len ? b->path[b->path_len - 1] : b->target);
    return noErr;
}

int32_t tb_SetDataBrowserSortOrder(gaddr ch, uint16_t order)
{
    BROWSER(ch);
    b->sort_order = order;
    sort_container(b, b->target, b->sort_prop, true);
    redraw(b);
    return noErr;
}

int32_t tb_GetDataBrowserSortOrder(gaddr ch, gaddr out)
{
    BROWSER(ch);
    if (out)
        wr16(out, b->sort_order);
    return noErr;
}

int32_t tb_SetDataBrowserScrollPosition(gaddr ch, uint32_t top, uint32_t left)
{
    BROWSER(ch);
    scroll_to(b, (int32_t)top, (int32_t)left);
    return noErr;
}

int32_t tb_GetDataBrowserScrollPosition(gaddr ch, gaddr top, gaddr left)
{
    BROWSER(ch);
    if (top)
        wr32(top, (uint32_t)b->top);
    if (left)
        wr32(left, (uint32_t)b->left);
    return noErr;
}

int32_t tb_SetDataBrowserHasScrollBars(gaddr ch, int8_t h, int8_t v)
{
    BROWSER(ch);
    b->hscroll = h != 0;
    b->vscroll = v != 0;
    redraw(b);
    return noErr;
}

int32_t tb_GetDataBrowserHasScrollBars(gaddr ch, gaddr h, gaddr v)
{
    BROWSER(ch);
    if (h)
        wr8(h, b->hscroll);
    if (v)
        wr8(v, b->vscroll);
    return noErr;
}

int32_t tb_SetDataBrowserSortProperty(gaddr ch, uint32_t prop)
{
    BROWSER(ch);
    b->sort_prop = prop;
    sort_container(b, b->target, prop, true);
    redraw(b);
    return noErr;
}

int32_t tb_GetDataBrowserSortProperty(gaddr ch, gaddr out)
{
    BROWSER(ch);
    if (out)
        wr32(out, b->sort_prop);
    return noErr;
}

int32_t tb_SetDataBrowserSelectionFlags(gaddr ch, uint32_t flags)
{
    BROWSER(ch);
    b->sel_flags = flags;
    return noErr;
}

int32_t tb_GetDataBrowserSelectionFlags(gaddr ch, gaddr out)
{
    BROWSER(ch);
    if (out)
        wr32(out, b->sel_flags);
    return noErr;
}

int32_t tb_SetDataBrowserPropertyFlags(gaddr ch, uint32_t prop, uint32_t flags)
{
    BROWSER(ch);
    db_column *c = column(b, prop);
    if (c) {
        c->flags = flags;
        return noErr;
    }
    for (int i = 0; i < b->npflags; i++)
        if (b->pflags[i].prop == prop) {
            b->pflags[i].flags = flags;
            return noErr;
        }
    b->pflags = realloc(b->pflags, (size_t)(b->npflags + 1) * sizeof *b->pflags);
    b->pflags[b->npflags++] = (db_flags){ prop, flags };
    return noErr;
}

int32_t tb_GetDataBrowserPropertyFlags(gaddr ch, uint32_t prop, gaddr out)
{
    BROWSER(ch);
    if (out)
        wr32(out, prop_flags(b, prop));
    return noErr;
}

int32_t tb_SetDataBrowserEditText(gaddr ch, gaddr text)
{
    BROWSER(ch);
    if (!b->edit_te)
        return errDataBrowserNotConfigured;
    uint8_t t[256];
    carbon_cfstring_to_pstr(text, t);
    gaddr p = tb_NewPtr(256, false, false);
    gput(p, t + 1, t[0]);
    gaddr saved = qd_port();
    tb_SetPort(ControlRecord_contrlOwner(cp(ch)));
    tb_TESetText(p, t[0], b->edit_te);
    tb_TESetSelect(0, 32767, b->edit_te);
    tb_SetPort(saved);
    tb_DisposePtr(p);
    redraw(b);
    return noErr;
}

/* GetDataBrowserEditText(browser, CFMutableStringRef): the text into the
 * program's mutable string. */
int32_t tb_GetDataBrowserEditText(gaddr ch, gaddr text)
{
    BROWSER(ch);
    if (!b->edit_te)
        return errDataBrowserNotConfigured;
    if (!text)
        return paramErr;
    gaddr t = rd32(b->edit_te);
    int32_t n = (int16_t)rd16(t + TERec_teLength_off);
    uint8_t buf[256];
    if (n > 255)
        n = 255;
    gget(buf, rd32(TERec_hText(t)), (uint32_t)n);
    gaddr cf = carbon_cfstring_from_text(buf, (size_t)n);
    if (!cfm_export_fn("CFStringReplaceAll")) {
        carbon_cfrelease(cf);
        return paramErr;
    }
    carbon_call_export("CFStringReplaceAll", (uint32_t[]){ text, cf }, 2);
    carbon_cfrelease(cf);
    return noErr;
}

int32_t tb_SetDataBrowserEditItem(gaddr ch, uint32_t item, uint32_t prop)
{
    BROWSER(ch);
    if (b->edit_te)
        end_edit(b, true);
    if (!item)
        return noErr;
    if (!find_item(b, item))
        return errDataBrowserItemNotFound;
    if (!column(b, prop) && b->style == kDataBrowserListView)
        return errDataBrowserPropertyNotFound;
    return start_edit(b, item, prop);
}

int32_t tb_GetDataBrowserEditItem(gaddr ch, gaddr item, gaddr prop)
{
    BROWSER(ch);
    if (item)
        wr32(item, b->edit_te ? b->edit_item : 0);
    if (prop)
        wr32(prop, b->edit_te ? b->edit_prop : 0);
    return noErr;
}

int32_t tb_GetDataBrowserItemPartBounds(gaddr ch, uint32_t item, uint32_t prop, int32_t part_in, gaddr out)
{
    uint32_t part = (uint32_t)part_in;
    BROWSER(ch);
    if (!find_item(b, item))
        return errDataBrowserItemNotFound;
    qrect cell;
    if (!cell_rect(b, item, prop, &cell))
        return errDataBrowserPropertyNotFound;
    int row = row_of(b, item);
    qrect r = cell;
    int indent = has_disclosure(b, prop) ? disclosure_indent(b, row) : 0;
    if (part == kDataBrowserPropertyDisclosurePart) {
        if (!has_disclosure(b, prop))
            return errDataBrowserInvalidPropertyPart;
        int top = cell.top + (cell.bottom - cell.top - 12) / 2;
        r = (qrect){ (int16_t)top, (int16_t)(cell.left + indent + 1), (int16_t)(top + 12), (int16_t)(cell.left + indent + 13) };
    } else if (part != kDataBrowserPropertyEnclosingPart) {
        r.left = (int16_t)(r.left + indent + (has_disclosure(b, prop) ? DISCLOSURE_W : 0));
        if (part == kDataBrowserCheckboxType) {
            int top = cell.top + (cell.bottom - cell.top - 12) / 2;
            r = (qrect){ (int16_t)top, (int16_t)(r.left + 3), (int16_t)(top + 12), (int16_t)(r.left + 15) };
        } else if (part == kDataBrowserIconType) {
            int sz = cell.bottom - cell.top >= 32 ? 32 : 16, top = cell.top + (cell.bottom - cell.top - sz) / 2;
            r = (qrect){ (int16_t)top, (int16_t)(r.left + 2), (int16_t)(top + sz), (int16_t)(r.left + 2 + sz) };
        } else if (part == kDataBrowserTextType) {
            db_column *c = column(b, prop);
            if (c && c->type == kDataBrowserIconAndTextType)
                r.left = (int16_t)(r.left + 2 + (cell.bottom - cell.top >= 32 ? 32 : 16) + 2);
            r.left = (int16_t)(r.left + 3);
        }
    }
    if (out)
        qrect_put(out, r);
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* The item data record */

static int32_t put32(gaddr d, uint32_t off, uint32_t set, uint32_t v)
{
    if (!d)
        return paramErr;
    wr32(d + off, v);
    wr32(d + ID_SET, rd32(d + ID_SET) | set);
    return noErr;
}

static int32_t get32(gaddr d, uint32_t off, gaddr out)
{
    if (!d || !out)
        return paramErr;
    wr32(out, rd32(d + off));
    return noErr;
}

int32_t tb_SetDataBrowserItemDataIcon(gaddr d, gaddr v) { return put32(d, ID_ICON, SET_ICON, v); }
int32_t tb_GetDataBrowserItemDataIcon(gaddr d, gaddr out) { return get32(d, ID_ICON, out); }
int32_t tb_SetDataBrowserItemDataText(gaddr d, gaddr v)
{
    /* kept for as long as the browser asks: retained, released at the next fetch's clear is not ours to do */
    return put32(d, ID_TEXT, SET_TEXT, v);
}
int32_t tb_GetDataBrowserItemDataText(gaddr d, gaddr out)
{
    if (!d || !out)
        return paramErr;
    gaddr cf = rd32(d + ID_TEXT);
    if (cf) /* the program owns what it gets (a copy, as CarbonLib gave) */
        cf = carbon_call_export("CFRetain", (uint32_t[]){ cf }, 1);
    wr32(out, cf);
    return noErr;
}
int32_t tb_SetDataBrowserItemDataValue(gaddr d, int32_t v) { return put32(d, ID_VALUE, SET_VALUE, (uint32_t)v); }
int32_t tb_GetDataBrowserItemDataValue(gaddr d, gaddr out) { return get32(d, ID_VALUE, out); }
int32_t tb_SetDataBrowserItemDataMinimum(gaddr d, int32_t v) { return put32(d, ID_MIN, SET_MIN, (uint32_t)v); }
int32_t tb_GetDataBrowserItemDataMinimum(gaddr d, gaddr out) { return get32(d, ID_MIN, out); }
int32_t tb_SetDataBrowserItemDataMaximum(gaddr d, int32_t v) { return put32(d, ID_MAX, SET_MAX, (uint32_t)v); }
int32_t tb_GetDataBrowserItemDataMaximum(gaddr d, gaddr out) { return get32(d, ID_MAX, out); }
int32_t tb_SetDataBrowserItemDataMenuRef(gaddr d, gaddr v) { return put32(d, ID_MENU, SET_MENU, v); }
int32_t tb_GetDataBrowserItemDataMenuRef(gaddr d, gaddr out) { return get32(d, ID_MENU, out); }
int32_t tb_SetDataBrowserItemDataDrawState(gaddr d, uint32_t v) { return put32(d, ID_DRAW_STATE, SET_DRAW_STATE, v); }
int32_t tb_GetDataBrowserItemDataDrawState(gaddr d, gaddr out) { return get32(d, ID_DRAW_STATE, out); }
int32_t tb_SetDataBrowserItemDataDateTime(gaddr d, int32_t v) { return put32(d, ID_DATE, SET_DATE, (uint32_t)v); }
int32_t tb_GetDataBrowserItemDataDateTime(gaddr d, gaddr out) { return get32(d, ID_DATE, out); }
int32_t tb_SetDataBrowserItemDataItemID(gaddr d, uint32_t v) { return put32(d, ID_ITEM_ID, SET_ITEM_ID, v); }
int32_t tb_GetDataBrowserItemDataItemID(gaddr d, gaddr out) { return get32(d, ID_ITEM_ID, out); }
int32_t tb_GetDataBrowserItemDataProperty(gaddr d, gaddr out) { return get32(d, ID_PROPERTY, out); }

int32_t tb_SetDataBrowserItemDataBooleanValue(gaddr d, int8_t v)
{
    if (!d)
        return paramErr;
    wr8(d + ID_BOOL, v != 0);
    wr16(d + ID_BUTTON, v != 0); /* a check box takes either */
    wr32(d + ID_SET, rd32(d + ID_SET) | SET_BOOL);
    return noErr;
}

int32_t tb_GetDataBrowserItemDataBooleanValue(gaddr d, gaddr out)
{
    if (!d || !out)
        return paramErr;
    wr8(out, rd8(d + ID_BOOL));
    return noErr;
}

int32_t tb_SetDataBrowserItemDataButtonValue(gaddr d, uint16_t v)
{
    if (!d)
        return paramErr;
    wr16(d + ID_BUTTON, v);
    wr8(d + ID_BOOL, v == 1);
    wr32(d + ID_SET, rd32(d + ID_SET) | SET_BUTTON);
    return noErr;
}

int32_t tb_GetDataBrowserItemDataButtonValue(gaddr d, gaddr out)
{
    if (!d || !out)
        return paramErr;
    wr16(out, rd16(d + ID_BUTTON));
    return noErr;
}

int32_t tb_SetDataBrowserItemDataIconTransform(gaddr d, int16_t v)
{
    if (!d)
        return paramErr;
    wr16(d + ID_TRANSFORM, (uint16_t)v);
    wr32(d + ID_SET, rd32(d + ID_SET) | SET_TRANSFORM);
    return noErr;
}

int32_t tb_GetDataBrowserItemDataIconTransform(gaddr d, gaddr out)
{
    if (!d || !out)
        return paramErr;
    wr16(out, rd16(d + ID_TRANSFORM));
    return noErr;
}

int32_t tb_SetDataBrowserItemDataRGBColor(gaddr d, gaddr rgb)
{
    if (!d || !rgb)
        return paramErr;
    gmemcpy(d + ID_RGB, rgb, 6);
    wr32(d + ID_SET, rd32(d + ID_SET) | SET_RGB);
    return noErr;
}

int32_t tb_GetDataBrowserItemDataRGBColor(gaddr d, gaddr out)
{
    if (!d || !out)
        return paramErr;
    gmemcpy(out, d + ID_RGB, 6);
    return noErr;
}

int32_t tb_SetDataBrowserItemDataLongDateTime(gaddr d, gaddr v)
{
    if (!d || !v)
        return paramErr;
    gmemcpy(d + ID_LONG_DATE, v, 8);
    wr32(d + ID_SET, rd32(d + ID_SET) | SET_LONG_DATE);
    return noErr;
}

int32_t tb_GetDataBrowserItemDataLongDateTime(gaddr d, gaddr out)
{
    if (!d || !out)
        return paramErr;
    gmemcpy(out, d + ID_LONG_DATE, 8);
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* Callbacks */

int32_t tb_InitDataBrowserCallbacks(gaddr cb)
{
    if (!cb || rd32(cb) != 0) /* kDataBrowserLatestCallbacks */
        return paramErr;
    gmemset(cb + 4, 0, 4 * CB_COUNT);
    return noErr;
}

int32_t tb_GetDataBrowserCallbacks(gaddr ch, gaddr cb)
{
    BROWSER(ch);
    if (!cb || rd32(cb) != 0)
        return paramErr;
    for (int i = 0; i < CB_COUNT; i++)
        wr32(cb + 4 + 4 * (uint32_t)i, b->cb[i]);
    return noErr;
}

int32_t tb_SetDataBrowserCallbacks(gaddr ch, gaddr cb)
{
    BROWSER(ch);
    if (!cb || rd32(cb) != 0)
        return paramErr;
    for (int i = 0; i < CB_COUNT; i++)
        b->cb[i] = rd32(cb + 4 + 4 * (uint32_t)i);
    changed(b);
    redraw(b);
    return noErr;
}

int32_t tb_InitDataBrowserCustomCallbacks(gaddr cb)
{
    if (!cb || rd32(cb) != 0) /* kDataBrowserLatestCustomCallbacks */
        return paramErr;
    gmemset(cb + 4, 0, 4 * CC_COUNT);
    return noErr;
}

int32_t tb_GetDataBrowserCustomCallbacks(gaddr ch, gaddr cb)
{
    BROWSER(ch);
    if (!cb || rd32(cb) != 0)
        return paramErr;
    for (int i = 0; i < CC_COUNT; i++)
        wr32(cb + 4 + 4 * (uint32_t)i, b->ccb[i]);
    return noErr;
}

int32_t tb_SetDataBrowserCustomCallbacks(gaddr ch, gaddr cb)
{
    BROWSER(ch);
    if (!cb || rd32(cb) != 0)
        return paramErr;
    for (int i = 0; i < CC_COUNT; i++)
        b->ccb[i] = rd32(cb + 4 + 4 * (uint32_t)i);
    redraw(b);
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* The table view (the list view's rows and columns) */

static int col_index(databrowser *b, uint32_t prop)
{
    for (int i = 0; i < b->ncols; i++)
        if (b->cols[i].prop == prop)
            return i;
    return -1;
}

int32_t tb_RemoveDataBrowserTableViewColumn(gaddr ch, uint32_t prop)
{
    BROWSER(ch);
    int i = col_index(b, prop);
    if (i < 0)
        return errDataBrowserPropertyNotFound;
    memmove(&b->cols[i], &b->cols[i + 1], (size_t)(b->ncols - i - 1) * sizeof *b->cols);
    b->ncols--;
    redraw(b);
    return noErr;
}

int32_t tb_GetDataBrowserTableViewColumnCount(gaddr ch, gaddr out)
{
    BROWSER(ch);
    if (out)
        wr32(out, (uint32_t)b->ncols);
    return noErr;
}

int32_t tb_SetDataBrowserTableViewHiliteStyle(gaddr ch, uint32_t style)
{
    BROWSER(ch);
    b->hilite = style;
    redraw(b);
    return noErr;
}

int32_t tb_GetDataBrowserTableViewHiliteStyle(gaddr ch, gaddr out)
{
    BROWSER(ch);
    if (out)
        wr32(out, b->hilite);
    return noErr;
}

int32_t tb_SetDataBrowserTableViewRowHeight(gaddr ch, uint16_t h)
{
    BROWSER(ch);
    b->row_height = h ? h : 1;
    redraw(b);
    return noErr;
}

int32_t tb_GetDataBrowserTableViewRowHeight(gaddr ch, gaddr out)
{
    BROWSER(ch);
    if (out)
        wr16(out, b->row_height);
    return noErr;
}

int32_t tb_SetDataBrowserTableViewColumnWidth(gaddr ch, uint16_t w)
{
    BROWSER(ch);
    b->col_width = w;
    for (int i = 0; i < b->ncols; i++) /* all columns that width */
        b->cols[i].width = w;
    redraw(b);
    return noErr;
}

int32_t tb_GetDataBrowserTableViewColumnWidth(gaddr ch, gaddr out)
{
    BROWSER(ch);
    if (out)
        wr16(out, b->col_width);
    return noErr;
}

int32_t tb_SetDataBrowserTableViewItemRowHeight(gaddr ch, uint32_t item, uint16_t h)
{
    BROWSER(ch);
    db_item *it = find_item(b, item);
    if (!it)
        return errDataBrowserItemNotFound;
    it->row_height = h;
    redraw(b);
    return noErr;
}

int32_t tb_GetDataBrowserTableViewItemRowHeight(gaddr ch, uint32_t item, gaddr out)
{
    BROWSER(ch);
    db_item *it = find_item(b, item);
    if (!it)
        return errDataBrowserItemNotFound;
    if (out)
        wr16(out, (uint16_t)row_h(b, item));
    return noErr;
}

int32_t tb_SetDataBrowserTableViewNamedColumnWidth(gaddr ch, uint32_t prop, uint16_t w)
{
    BROWSER(ch);
    db_column *c = column(b, prop);
    if (!c)
        return errDataBrowserPropertyNotFound;
    c->width = w;
    redraw(b);
    return noErr;
}

int32_t tb_GetDataBrowserTableViewNamedColumnWidth(gaddr ch, uint32_t prop, gaddr out)
{
    BROWSER(ch);
    db_column *c = column(b, prop);
    if (!c)
        return errDataBrowserPropertyNotFound;
    if (out)
        wr16(out, (uint16_t)col_w(b, c));
    return noErr;
}

int32_t tb_SetDataBrowserTableViewGeometry(gaddr ch, int8_t var_widths, int8_t var_heights)
{
    BROWSER(ch);
    b->var_widths = var_widths != 0;
    b->var_heights = var_heights != 0;
    redraw(b);
    return noErr;
}

int32_t tb_GetDataBrowserTableViewGeometry(gaddr ch, gaddr var_widths, gaddr var_heights)
{
    BROWSER(ch);
    if (var_widths)
        wr8(var_widths, b->var_widths);
    if (var_heights)
        wr8(var_heights, b->var_heights);
    return noErr;
}

int32_t tb_GetDataBrowserTableViewItemID(gaddr ch, uint32_t row, gaddr out)
{
    BROWSER(ch);
    layout(b);
    if (row >= (uint32_t)b->nrows)
        return errDataBrowserItemNotFound;
    if (out)
        wr32(out, b->rows[row]);
    return noErr;
}

/* Move an item to a row (among its container's items). */
int32_t tb_SetDataBrowserTableViewItemRow(gaddr ch, uint32_t item, uint32_t row)
{
    BROWSER(ch);
    db_item *it = find_item(b, item);
    if (!it)
        return errDataBrowserItemNotFound;
    layout(b);
    if (row >= (uint32_t)b->nrows)
        return paramErr;
    db_item moved = *it;
    int from = (int)(it - b->items);
    memmove(&b->items[from], &b->items[from + 1], (size_t)(b->nitems - from - 1) * sizeof *b->items);
    b->nitems--;
    /* before the item now at that row (or last) */
    uint32_t at_id = b->rows[row] == item ? (row + 1 < (uint32_t)b->nrows ? b->rows[row + 1] : 0) : b->rows[row];
    db_item *at = at_id ? find_item(b, at_id) : NULL;
    int to = at ? (int)(at - b->items) : b->nitems;
    memmove(&b->items[to + 1], &b->items[to], (size_t)(b->nitems - to) * sizeof *b->items);
    b->items[to] = moved;
    b->nitems++;
    changed(b);
    redraw(b);
    return noErr;
}

int32_t tb_GetDataBrowserTableViewItemRow(gaddr ch, uint32_t item, gaddr out)
{
    BROWSER(ch);
    int row = row_of(b, item);
    if (row < 0)
        return errDataBrowserItemNotFound;
    if (out)
        wr32(out, (uint32_t)row);
    return noErr;
}

int32_t tb_SetDataBrowserTableViewColumnPosition(gaddr ch, uint32_t prop, uint32_t position)
{
    BROWSER(ch);
    int i = col_index(b, prop);
    if (i < 0)
        return errDataBrowserPropertyNotFound;
    db_column c = b->cols[i];
    memmove(&b->cols[i], &b->cols[i + 1], (size_t)(b->ncols - i - 1) * sizeof c);
    int to = position >= (uint32_t)(b->ncols - 1) ? b->ncols - 1 : (int)position;
    memmove(&b->cols[to + 1], &b->cols[to], (size_t)(b->ncols - 1 - to) * sizeof c);
    b->cols[to] = c;
    redraw(b);
    return noErr;
}

int32_t tb_GetDataBrowserTableViewColumnPosition(gaddr ch, uint32_t prop, gaddr out)
{
    BROWSER(ch);
    int i = col_index(b, prop);
    if (i < 0)
        return errDataBrowserPropertyNotFound;
    if (out)
        wr32(out, (uint32_t)i);
    return noErr;
}

int32_t tb_GetDataBrowserTableViewColumnProperty(gaddr ch, uint32_t index, gaddr out)
{
    BROWSER(ch);
    if (index >= (uint32_t)b->ncols)
        return errDataBrowserPropertyNotFound;
    if (out)
        wr32(out, b->cols[index].prop);
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* The list view */

/* The columns sized to the browser: the room shared in proportion to
 * their widths, within their minimums and maximums. */
int32_t tb_AutoSizeDataBrowserListViewColumns(gaddr ch)
{
    BROWSER(ch);
    if (!b->ncols)
        return noErr;
    qrect rr = rows_rect(b);
    int room = rr.right - rr.left, have = total_width(b);
    for (int i = 0; i < b->ncols; i++) {
        db_column *c = &b->cols[i];
        int w = have ? (int)((long)col_w(b, c) * room / have) : room / b->ncols;
        if (c->min_width && w < c->min_width)
            w = c->min_width;
        if (c->max_width && w > c->max_width)
            w = c->max_width;
        c->width = (uint16_t)w;
    }
    redraw(b);
    return noErr;
}

/* DataBrowserListViewColumnDesc: the property (ID, type, flags), then the
 * header: version, minimum and maximum width, title offset, title
 * (CFString), initial order, font style, content. */
int32_t tb_AddDataBrowserListViewColumn(gaddr ch, gaddr desc, uint32_t position)
{
    BROWSER(ch);
    if (!desc)
        return paramErr;
    if (column(b, rd32(desc)))
        return paramErr;
    db_column c;
    memset(&c, 0, sizeof c);
    c.prop = rd32(desc);
    c.type = rd32(desc + 4);
    c.flags = rd32(desc + 8);
    c.min_width = rd16(desc + 16);
    c.max_width = rd16(desc + 18);
    c.title_offset = (int16_t)rd16(desc + 20);
    carbon_cfstring_to_pstr(rd32(desc + 22), c.title);
    c.initial_order = rd16(desc + 26);
    c.width = b->col_width;
    if (c.min_width && c.width < c.min_width)
        c.width = c.min_width;
    if (c.max_width && c.width > c.max_width)
        c.width = c.max_width;
    b->cols = realloc(b->cols, (size_t)(b->ncols + 1) * sizeof *b->cols);
    int at = position >= (uint32_t)b->ncols ? b->ncols : (int)position;
    memmove(&b->cols[at + 1], &b->cols[at], (size_t)(b->ncols - at) * sizeof c);
    b->cols[at] = c;
    b->ncols++;
    if (!b->sort_prop && (c.flags & kDataBrowserListViewSortableColumn)) {
        b->sort_prop = c.prop;
        if (c.initial_order)
            b->sort_order = c.initial_order;
    }
    redraw(b);
    return noErr;
}

int32_t tb_SetDataBrowserListViewHeaderBtnHeight(gaddr ch, uint16_t h)
{
    BROWSER(ch);
    b->header_height = h;
    redraw(b);
    return noErr;
}

int32_t tb_GetDataBrowserListViewHeaderBtnHeight(gaddr ch, gaddr out)
{
    BROWSER(ch);
    if (out)
        wr16(out, b->header_height);
    return noErr;
}

int32_t tb_SetDataBrowserListViewUsePlainBackground(gaddr ch, int8_t plain)
{
    BROWSER(ch);
    b->plain_bg = plain != 0;
    redraw(b);
    return noErr;
}

int32_t tb_GetDataBrowserListViewUsePlainBackground(gaddr ch, gaddr out)
{
    BROWSER(ch);
    if (out)
        wr8(out, b->plain_bg);
    return noErr;
}

int32_t tb_SetDataBrowserListViewDisclosureColumn(gaddr ch, uint32_t prop, int8_t expandable)
{
    BROWSER(ch);
    if (prop && !column(b, prop))
        return errDataBrowserPropertyNotFound;
    b->disclosure_col = prop;
    b->expandable = expandable != 0;
    redraw(b);
    return noErr;
}

int32_t tb_GetDataBrowserListViewDisclosureColumn(gaddr ch, gaddr prop, gaddr expandable)
{
    BROWSER(ch);
    if (prop)
        wr32(prop, b->disclosure_col);
    if (expandable)
        wr8(expandable, b->expandable);
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* The column view */

int32_t tb_GetDataBrowserColumnViewPath(gaddr ch, gaddr h)
{
    BROWSER(ch);
    if (!h)
        return paramErr;
    tb_SetHandleSize(h, b->path_len * 4);
    if (rd16(LM_MemErr))
        return memFullErr;
    for (int i = 0; i < b->path_len; i++)
        wr32(rd32(h) + 4 * (uint32_t)i, b->path[i]);
    return noErr;
}

int32_t tb_GetDataBrowserColumnViewPathLength(gaddr ch, gaddr out)
{
    BROWSER(ch);
    if (out)
        wr32(out, (uint32_t)b->path_len);
    return noErr;
}

int32_t tb_SetDataBrowserColumnViewPath(gaddr ch, uint32_t n, gaddr path)
{
    BROWSER(ch);
    if (n > 256 || (n && !path))
        return paramErr;
    b->path = realloc(b->path, (size_t)(n ? n : 1) * sizeof *b->path);
    for (uint32_t i = 0; i < n; i++)
        b->path[i] = rd32(path + 4 * i);
    b->path_len = (int)n;
    b->top = 0;
    changed(b);
    notify(b, n ? b->path[n - 1] : b->target, kDataBrowserTargetChanged);
    redraw(b);
    return noErr;
}

int32_t tb_SetDataBrowserColumnViewDisplayType(gaddr ch, int32_t type)
{
    BROWSER(ch);
    b->display_type = (uint32_t)type;
    redraw(b);
    return noErr;
}

int32_t tb_GetDataBrowserColumnViewDisplayType(gaddr ch, gaddr out)
{
    BROWSER(ch);
    if (out)
        wr32(out, b->display_type);
    return noErr;
}
