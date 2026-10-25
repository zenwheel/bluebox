/*
 * The Multilingual Text Engine (MacTextEditor.h), on styled TextEdit.
 *
 * A TXNObject is a block (a tag and an index) standing for a record kept
 * here: a styled TextEdit record in a frame of its window, with the
 * frame's scroll bars, its controls (justification, wrapping, margins,
 * read-only and the rest), undo and redo, change counts and echo mode.
 * MLTE's offsets count UTF-16 units; the text is held in Mac OS Roman, a
 * byte for each, so offsets carry over (Unicode outside Mac OS Roman is
 * '?', a pair of surrogates one character).  TextEdit's limit of 32K
 * holds; embedded pictures, sounds and movies are not taken.
 */
#include <stdlib.h>
#include <string.h>

#include "carbon_svc.h"
#include "qd_internal.h"

enum {
    kTXNEndIterationErr = -22000,
    kTXNInvalidFrameIDErr = -22002,
    kTXNIllegalToCrossDataBoundariesErr = -22003,
    kTXNRunIndexOutofBoundsErr = -22007,
    kTXNNoMatchErr = -22008,
    kTXNSomeOrAllTagsInvalidForRunErr = -22010,
    kTXNAlreadyInitializedErr = -22012,
    kTXNCopyNotAllowedInEchoModeErr = -22014,
    kTXNDataTypeNotAllowedErr = -22015,
    kTXNOutsideOfLineErr = -22017,
    kTXNOutsideOfFrameErr = -22018,
    eofErr = -39,
};
/* Frame options */
enum {
    kTXNDrawGrowIconMask = 1 << 0, kTXNShowWindowMask = 1 << 1, kTXNWantHScrollBarMask = 1 << 2,
    kTXNWantVScrollBarMask = 1 << 3, kTXNReadOnlyMask = 1 << 5, kTXNNoSelectionMask = 1 << 7,
    kTXNSaveStylesAsSTYLResourceMask = 1 << 8, kOutputTextInUnicodeEncodingMask = 1 << 9,
    kTXNAlwaysWrapAtViewEdgeMask = 1 << 11, kTXNDontDrawCaretWhenInactiveMask = 1 << 12,
    kTXNDontDrawSelectionWhenInactiveMask = 1 << 13, kTXNSingleLineOnlyMask = 1 << 14,
    kTXNDisableDragAndDropMask = 1 << 15,
};
enum { kTXNSetFlushnessMask = 1, kTXNDontUpdateBoxRectMask = 1 << 5, kTXNDontDrawTextMask = 1 << 6 };
enum { kTXNFontContinuousMask = 1, kTXNSizeContinuousMask = 2, kTXNStyleContinuousMask = 4,
       kTXNColorContinuousMask = 8 };
enum { kTXNIgnoreCaseMask = 1, kTXNEntireWordMask = 2 };
enum { kTXNSystemDefaultEncoding = 0, kTXNMacOSEncoding = 1, kTXNUnicodeEncoding = 2 };
enum { kTXNTypingAction = 0, kTXNCutAction, kTXNPasteAction, kTXNClearAction, kTXNChangeFontAction,
       kTXNChangeFontColorAction, kTXNChangeFontSizeAction, kTXNChangeStyleAction, kTXNAlignLeftAction,
       kTXNAlignCenterAction, kTXNAlignRightAction, kTXNDropAction, kTXNMoveAction };
enum { kTXNTextInputCountMask = 1, kTXNRunCountMask = 2, kTXNAllCountMask = 3 };
enum { kTXNFlushDefault = 0, kTXNFlushLeft = 1, kTXNFlushRight = 2, kTXNCenter = 4, kTXNFullJust = 8,
       kTXNForceFullJust = 16 };
enum { teFlushDefault = 0, teCenter = 1, teFlushRight = -1, teFlushLeft = -2 };
enum { doFont = 1, doFace = 2, doSize = 4, doColor = 8, doAll = 15, addSize = 16 };
#define kTXNUseCurrentSelection 0xFFFFFFFFu
#define kTXNEndOffset 0x7FFFFFFFu
#define kTXNDontCareTypeSize 0xFFFFFFFFu
#define kTXNIncrementTypeSize 0x00000001u
#define kTXNDecrementTypeSize 0x80000000u
#define FOUR(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define TYPE_TEXT FOUR('T', 'E', 'X', 'T')
#define TYPE_UTXT FOUR('u', 't', 'x', 't')
#define TYPE_STYL FOUR('s', 't', 'y', 'l')
#define TYPE_MPSR FOUR('M', 'P', 'S', 'R')
#define TAG_FONTNAME FOUR('f', 'n', 't', 'n')
#define TAG_FONT FOUR('f', 'o', 'n', 't')
#define TAG_SIZE FOUR('s', 'i', 'z', 'e')
#define TAG_FACE FOUR('f', 'a', 'c', 'e')
#define TAG_COLOR FOUR('k', 'l', 'o', 'r')
#define TAG_ENCODING FOUR('e', 'n', 'c', 'd')
enum { uppTXNFindProcInfo = 0x0FFFFFF0 };
enum { scrollBarProc = 16, inUpButton = 20, inDownButton = 21, inPageUp = 22, inPageDown = 23, inThumb = 129 };
enum { shiftKey = 0x0200, cmdKey = 0x0100 };
enum { BAR = 16, MAX_TEXT = 32000 };

#define TXN_TAG 0x54584e6fu /* 'TXNo' */

typedef struct {
    bool valid;
    uint8_t *text;
    int len;
    gaddr styles; /* StScrpHandle */
    int a, b;
    uint32_t action;
} snapshot;

typedef struct {
    gaddr addr, window, te;
    bool is_window;
    qrect frame;
    uint32_t options, encoding, refcon;
    gaddr vbar, hbar;
    bool focused, bars_active, readonly, noselect, nowrap, autoindent, nouserio;
    bool caret_inactive, sel_inactive, nodrag;
    uint32_t just, line_dir, tabs, carbon_events;
    int16_t margins[4];
    uint32_t changes, typing_changes, run_changes;
    snapshot undo, redo;
    bool typing; /* the undo snapshot is an open run of typing */
    bool echo;
    uint16_t echo_char;
    uint8_t *shadow; /* echo mode: the real text */
    int shadow_len;
    bool has_bg;
    uint16_t bg[3];
    gaddr spec; /* the file, for TXNRevert */
} txn_t;

static txn_t **objects;
static int nobjects;
static bool initialized;
static uint8_t default_fonts[16][14]; /* TXNMacOSPreferredFontDescription */
static int ndefault_fonts;
static gaddr action_stub;
static txn_t *tracking; /* the object whose scroll bar is being tracked */
static gaddr page_format; /* TXNPageSetup's, for TXNPrint */

/* ---------------------------------------------------------------------- */
/* Objects */

static txn_t *object_of(gaddr o)
{
    if (!o || o >= 0xF0000000u || rd32(o) != TXN_TAG)
        return NULL;
    uint32_t i = rd32(o + 4);
    return i < (uint32_t)nobjects && objects[i] && objects[i]->addr == o ? objects[i] : NULL;
}

static void forget_snapshot(snapshot *s)
{
    free(s->text);
    if (s->styles)
        tb_DisposeHandle(s->styles);
    memset(s, 0, sizeof *s);
}

void carbon_mlte_reset(void)
{
    for (int i = 0; i < nobjects; i++)
        if (objects[i]) {
            free(objects[i]->undo.text);
            free(objects[i]->redo.text);
            free(objects[i]->shadow);
            free(objects[i]);
        }
    free(objects);
    objects = NULL;
    nobjects = 0;
    initialized = false;
    ndefault_fonts = 0;
    action_stub = 0;
    tracking = NULL;
    page_format = 0;
}

#define OBJ_ARG(t)                                                                                                     \
    txn_t *t = object_of(A(0));                                                                                        \
    if (!t)                                                                                                            \
        RET(paramErr);
#define OBJ_ARG_VOID(t)                                                                                                \
    txn_t *t = object_of(A(0));                                                                                        \
    if (!t)                                                                                                            \
        DONE();

/* TextEdit's calls run with the object's port current. */
typedef struct {
    gaddr saved;
} port_saver;
static port_saver enter(txn_t *t)
{
    port_saver s = { qd_port() };
    if (t->window)
        tb_SetPort(t->window);
    return s;
}
static void leave(port_saver s) { tb_SetPort(s.saved); }

static gaddr te_rec(txn_t *t) { return rd32(t->te); }
static int text_len(txn_t *t) { return (int16_t)rd16(te_rec(t) + TERec_teLength_off); }
static gaddr text_ptr(txn_t *t) { return rd32(rd32(te_rec(t) + TERec_hText_off)); }
static int sel_start(txn_t *t) { return (int16_t)rd16(te_rec(t) + TERec_selStart_off); }
static int sel_end(txn_t *t) { return (int16_t)rd16(te_rec(t) + TERec_selEnd_off); }

/* An MLTE offset as TextEdit's. */
static int offset(txn_t *t, uint32_t o, bool start)
{
    if (o == kTXNUseCurrentSelection)
        return start ? sel_start(t) : sel_end(t);
    return o > (uint32_t)text_len(t) ? text_len(t) : (int)o;
}

static gaddr rect_ptr(qrect r)
{
    gaddr p = tb_NewPtr(8, false, true);
    if (p)
        qrect_put(p, r);
    return p;
}

/* The text's view: the frame less the scroll bars and margins. */
static qrect view_rect(txn_t *t)
{
    qrect v = t->frame;
    if (t->vbar)
        v.right = (int16_t)(v.right - (BAR - 1));
    if (t->hbar || (t->options & kTXNDrawGrowIconMask))
        v.bottom = (int16_t)(v.bottom - (BAR - 1));
    v.top = (int16_t)(v.top + t->margins[0]);
    v.left = (int16_t)(v.left + t->margins[1]);
    return v;
}

static int text_height(txn_t *t)
{
    return tb_TEGetHeight((int16_t)rd16(te_rec(t) + TERec_nLines_off), 1, t->te);
}

/* The scroll bars follow the text's place in its view. */
static void sync_bars(txn_t *t)
{
    gaddr r = te_rec(t);
    qrect v = qrect_get(r + TERec_viewRect_off), d = qrect_get(r + TERec_destRect_off);
    if (t->vbar) {
        int max = text_height(t) - (v.bottom - v.top);
        tb_SetControlMaximum(t->vbar, (int16_t)(max > 0 ? max : 0));
        tb_SetControlValue(t->vbar, (int16_t)(v.top - d.top));
    }
    if (t->hbar) {
        int width = t->nowrap ? 2000 : d.right - d.left;
        int max = width - (v.right - v.left);
        tb_SetControlMaximum(t->hbar, (int16_t)(max > 0 ? max : 0));
        tb_SetControlValue(t->hbar, (int16_t)(v.left - d.left));
    }
}

/* Lay the record and the scroll bars out in the frame, the scroll kept. */
static void layout(txn_t *t)
{
    gaddr r = te_rec(t);
    qrect v = view_rect(t), old_v = qrect_get(r + TERec_viewRect_off), d = qrect_get(r + TERec_destRect_off);
    int dy = d.top - old_v.top, dx = d.left - old_v.left;
    qrect_put(r + TERec_viewRect_off, v);
    int right = t->nowrap ? v.left + 32000 / 2 : v.right - t->margins[3];
    qrect_put(r + TERec_destRect_off,
              (qrect){ (int16_t)(v.top + dy), (int16_t)(v.left + dx), (int16_t)(v.bottom + dy), (int16_t)(right + dx) });
    wr16(r + TERec_crOnly_off, (uint16_t)(t->nowrap ? -1 : 0));
    port_saver s = enter(t);
    tb_TECalText(t->te);
    if (t->vbar) {
        tb_MoveControl(t->vbar, (int16_t)(t->frame.right - BAR), (int16_t)(t->frame.top - 1));
        tb_SizeControl(t->vbar, BAR, (int16_t)(view_rect(t).bottom - t->frame.top + 2 - t->margins[0]));
    }
    if (t->hbar) {
        tb_MoveControl(t->hbar, (int16_t)(t->frame.left - 1), (int16_t)(t->frame.bottom - BAR));
        tb_SizeControl(t->hbar, (int16_t)(view_rect(t).right - t->frame.left + 2 - t->margins[1]), BAR);
    }
    sync_bars(t);
    leave(s);
}

/* A TextStyle (font, face, size, colour) for TESetStyle. */
static gaddr text_style(int16_t font, uint8_t face, int16_t size, const uint16_t *color)
{
    gaddr s = tb_NewPtr(TextStyle_SIZEOF, false, true);
    if (!s)
        return 0;
    wr16(s, (uint16_t)font);
    wr8(s + 2, face);
    wr16(s + 4, (uint16_t)size);
    for (int i = 0; color && i < 3; i++)
        wr16(s + 6 + 2 * (uint32_t)i, color[i]);
    return s;
}

/* The defaults' font for Roman, or the application font at 12 points. */
static void default_style(int16_t *font, int16_t *size, uint8_t *face)
{
    *font = (int16_t)rd16(LM_ApFontID);
    *size = 12;
    *face = 0;
    for (int i = 0; i < ndefault_fonts; i++) {
        const uint8_t *f = default_fonts[i];
        uint32_t enc = (uint32_t)f[8] << 24 | f[9] << 16 | f[10] << 8 | f[11];
        if ((enc & 0xFFFF) != 0) /* not Mac OS Roman */
            continue;
        int32_t id = (int32_t)((uint32_t)f[0] << 24 | f[1] << 16 | f[2] << 8 | f[3]);
        int32_t pt = (int32_t)((uint32_t)f[4] << 24 | f[5] << 16 | f[6] << 8 | f[7]);
        if (id != -1)
            *font = (int16_t)id;
        if (pt > 0)
            *size = (int16_t)(pt >> 16);
        if (f[12] != 0xFF)
            *face = f[12];
    }
}

/* ---------------------------------------------------------------------- */
/* Initialising */

/* TXNInitTextension(defaultFonts[], count, options) */
static void tx_TXNInitTextension(ppc_cpu *c)
{
    if (initialized)
        RET(kTXNAlreadyInitializedErr);
    uint32_t n = A(1);
    if (n && !A(0))
        RET(paramErr);
    ndefault_fonts = 0;
    for (uint32_t i = 0; i < n && i < 16; i++)
        gget(default_fonts[ndefault_fonts++], A(0) + 14 * i, 14);
    initialized = true;
    RET(noErr);
}

static void tx_TXNTerminateTextension(ppc_cpu *c)
{
    initialized = false;
    DONE();
}

/* TXNVersionInformation(&features): MLTE 1.1, not defaulting to ATSUI. */
static void tx_TXNVersionInformation(ppc_cpu *c)
{
    if (A(0))
        wr32(A(0), 0);
    RET(0x01100000);
}

/* ---------------------------------------------------------------------- */
/* Text in and out */

static void bump(txn_t *t, bool typing)
{
    t->changes++;
    if (typing)
        t->typing_changes++;
    else
        t->run_changes++;
}

/* The object's whole state, for undo. */
static void take_snapshot(txn_t *t, snapshot *s, uint32_t action)
{
    forget_snapshot(s);
    int n = text_len(t);
    s->text = malloc(n ? (size_t)n : 1);
    gget(s->text, text_ptr(t), (uint32_t)n);
    s->len = n;
    s->a = sel_start(t);
    s->b = sel_end(t);
    port_saver p = enter(t);
    tb_TESetSelect(0, n, t->te);
    s->styles = tb_TEGetStyleScrapHandle(t->te);
    tb_TESetSelect(s->a, s->b, t->te);
    leave(p);
    s->action = action;
    s->valid = true;
}

static void restore_snapshot(txn_t *t, const snapshot *s)
{
    port_saver p = enter(t);
    gaddr buf = tb_NewPtr(s->len ? s->len : 1, false, false);
    gput(buf, s->text, (uint32_t)s->len);
    tb_TESetText(buf, s->len, t->te);
    tb_DisposePtr(buf);
    if (s->styles)
        tb_TEUseStyleScrap(0, s->len, s->styles, 0, t->te);
    tb_TESetSelect(s->a, s->b, t->te);
    leave(p);
}

/* Before an edit: what undo goes back to (a run of typing is one). */
static void will_edit(txn_t *t, uint32_t action)
{
    if (action == kTXNTypingAction && t->typing && t->undo.valid)
        return;
    take_snapshot(t, &t->undo, action);
    forget_snapshot(&t->redo);
    t->typing = action == kTXNTypingAction;
}

/* Replace TextEdit's [a, b) with MacRoman bytes in the selection's style. */
static void replace(txn_t *t, int a, int b, const uint8_t *bytes, int n)
{
    if (text_len(t) - (b - a) + n > MAX_TEXT)
        n = MAX_TEXT - (text_len(t) - (b - a));
    if (n < 0)
        n = 0;
    if (t->echo) { /* the real text aside, echo characters shown */
        int len = t->shadow_len - (b - a) + n;
        uint8_t *s = malloc(len ? (size_t)len : 1);
        memcpy(s, t->shadow, (size_t)a);
        memcpy(s + a, bytes, (size_t)n);
        memcpy(s + a + n, t->shadow + b, (size_t)(t->shadow_len - b));
        free(t->shadow);
        t->shadow = s;
        t->shadow_len = len;
    }
    port_saver p = enter(t);
    tb_TESetSelect(a, b, t->te);
    tb_TEDelete(t->te);
    if (n) {
        gaddr buf = tb_NewPtr(n, false, false);
        if (t->echo) {
            uint16_t e = t->echo_char;
            uint8_t ch = '*';
            svc_utf16_to_macroman(&e, 1, &ch);
            gmemset(buf, ch, (uint32_t)n);
        } else {
            gput(buf, bytes, (uint32_t)n);
        }
        tb_TEInsert(buf, n, t->te);
        tb_DisposePtr(buf);
    }
    leave(p);
}

/* The text of [a, b): the real text in echo mode. */
static uint8_t *text_bytes(txn_t *t, int a, int b)
{
    uint8_t *out = malloc(b > a ? (size_t)(b - a) : 1);
    if (t->echo)
        memcpy(out, t->shadow + a, (size_t)(b - a));
    else
        gget(out, text_ptr(t) + (uint32_t)a, (uint32_t)(b - a));
    return out;
}

/* Bytes of a data type as MacRoman (a new buffer; *n its length). */
static uint8_t *as_macroman(uint32_t type, gaddr p, uint32_t size, int *n)
{
    if (type == TYPE_TEXT) {
        uint8_t *b = malloc(size ? size : 1);
        gget(b, p, size);
        *n = (int)size;
        return b;
    }
    size_t units = size / 2, start = 0;
    uint16_t *u = malloc(sizeof *u * (units + 1));
    for (size_t i = 0; i < units; i++)
        u[i] = rd16(p + 2 * (uint32_t)i);
    if (units && u[0] == 0xFEFF)
        start = 1; /* a byte order mark */
    uint8_t *b = malloc(units + 1);
    *n = (int)svc_utf16_to_macroman(u + start, units - start, b);
    free(u);
    return b;
}

static void after_edit(txn_t *t)
{
    port_saver p = enter(t);
    tb_TESelView(t->te);
    sync_bars(t);
    leave(p);
}

/* TXNSetData(object, type, data, size, start, end) */
static void tx_TXNSetData(ppc_cpu *c)
{
    OBJ_ARG(t);
    uint32_t type = A(1), size = A(3);
    gaddr data = A(2);
    if (type != TYPE_TEXT && type != TYPE_UTXT)
        RET(kTXNDataTypeNotAllowedErr);
    if (size && !data)
        RET(paramErr);
    int a = offset(t, A(4), true), b = offset(t, A(5), false);
    if (b < a)
        RET(paramErr);
    int n;
    uint8_t *bytes = as_macroman(type, data, size, &n);
    replace(t, a, b, bytes, n);
    free(bytes);
    t->typing = false;
    bump(t, false);
    after_edit(t);
    RET(noErr);
}

/* TXNGetData(object, start, end, &handle): Unicode for an object made
 * Unicode, else Mac OS Roman. */
static int32_t get_data(txn_t *t, uint32_t start, uint32_t end, gaddr out, uint32_t type)
{
    if (!out)
        return paramErr;
    wr32(out, 0);
    if (type != TYPE_TEXT && type != TYPE_UTXT)
        return kTXNDataTypeNotAllowedErr;
    int a = offset(t, start, true), b = offset(t, end, false);
    if (b < a)
        return paramErr;
    uint8_t *bytes = text_bytes(t, a, b);
    gaddr h;
    if (type == TYPE_TEXT) {
        h = tb_NewHandle(b - a, false, true);
        if (h)
            gput(rd32(h), bytes, (uint32_t)(b - a));
    } else {
        uint16_t *u = malloc(sizeof *u * (size_t)(b - a + 1));
        size_t k = svc_macroman_to_utf16(bytes, (size_t)(b - a), u);
        h = tb_NewHandle((int32_t)(2 * k), false, true);
        for (size_t i = 0; h && i < k; i++)
            wr16(rd32(h) + 2 * (uint32_t)i, u[i]);
        free(u);
    }
    free(bytes);
    if (!h)
        return memFullErr;
    wr32(out, h);
    return noErr;
}

static void tx_TXNGetData(ppc_cpu *c)
{
    OBJ_ARG(t);
    RET(get_data(t, A(1), A(2), A(3), t->encoding == kTXNUnicodeEncoding ? TYPE_UTXT : TYPE_TEXT));
}

static void tx_TXNGetDataEncoded(ppc_cpu *c)
{
    OBJ_ARG(t);
    RET(get_data(t, A(1), A(2), A(3), A(4)));
}

/* TXNDataSize(object): the characters' bytes, as UTF-16. */
static void tx_TXNDataSize(ppc_cpu *c)
{
    txn_t *t = object_of(A(0));
    RET(t ? 2 * (uint32_t)text_len(t) : 0);
}

/* Read n bytes (all to the end of file for kTXNEndOffset) from an open
 * file at its mark. */
static uint8_t *read_file(int16_t ref, uint32_t n, uint32_t *got, int16_t *err)
{
    gaddr pb = tb_NewPtr(ParamBlockRec_SIZEOF, false, true);
    *got = 0;
    wr16(pb + IOParam_ioRefNum_off, (uint16_t)ref);
    if (n == kTXNEndOffset) {
        *err = tb_PBGetEOF(pb, false);
        uint32_t eof = rd32(pb + IOParam_ioMisc_off);
        tb_PBGetFPos(pb, false);
        uint32_t mark = rd32(pb + IOParam_ioPosOffset_off);
        n = eof > mark ? eof - mark : 0;
    }
    if (n > 4 * MAX_TEXT)
        n = 4 * MAX_TEXT;
    gaddr buf = tb_NewPtr((int32_t)(n ? n : 1), false, false);
    wr32(pb + IOParam_ioBuffer_off, buf);
    wr32(pb + IOParam_ioReqCount_off, n);
    wr16(pb + IOParam_ioPosMode_off, 0); /* fsAtMark */
    *err = n ? tb_PBRead(pb, false) : noErr;
    if (*err == eofErr)
        *err = noErr;
    *got = rd32(pb + IOParam_ioActCount_off);
    uint8_t *out = malloc(*got ? *got : 1);
    gget(out, buf, *got);
    tb_DisposePtr(buf);
    tb_DisposePtr(pb);
    return out;
}

/* TXNSetDataFromFile(object, refNum, type, length, start, end) */
static void tx_TXNSetDataFromFile(ppc_cpu *c)
{
    OBJ_ARG(t);
    uint32_t type = A(2);
    if (type != TYPE_TEXT && type != TYPE_UTXT)
        RET(kTXNDataTypeNotAllowedErr);
    int16_t err;
    uint32_t got;
    uint8_t *raw = read_file((int16_t)A(1), A(3), &got, &err);
    if (err) {
        free(raw);
        RET(err);
    }
    gaddr g = tb_NewPtr((int32_t)(got ? got : 1), false, false);
    gput(g, raw, got);
    free(raw);
    int n;
    uint8_t *bytes = as_macroman(type, g, got, &n);
    tb_DisposePtr(g);
    int a = offset(t, A(4), true), b = offset(t, A(5), false);
    replace(t, a < b ? a : b, b, bytes, n);
    free(bytes);
    bump(t, false);
    after_edit(t);
    RET(noErr);
}

/* ---------------------------------------------------------------------- */
/* Making and disposing */

static gaddr new_bar(txn_t *t, bool vertical)
{
    qrect r = vertical ? (qrect){ t->frame.top, (int16_t)(t->frame.right - BAR), t->frame.bottom, t->frame.right }
                       : (qrect){ (int16_t)(t->frame.bottom - BAR), t->frame.left, t->frame.bottom, t->frame.right };
    gaddr rp = rect_ptr(r), title = tb_NewPtr(2, false, true);
    gaddr h = tb_NewControl(t->window, rp, title, 1, 0, 0, 0, scrollBarProc, (int32_t)t->addr);
    tb_DisposePtr(title);
    tb_DisposePtr(rp);
    return h;
}

static int32_t load_file(txn_t *t, gaddr spec);
static void delete_object(txn_t *t);

/* TXNNewObject(fileSpec, window, frame, frameOptions, frameType, fileType,
 * encoding, &object, &frameID, refCon) */
static void tx_TXNNewObject(ppc_cpu *c)
{
    gaddr spec = A(0), window = A(1), frame = A(2), out = A(7), frame_id = A(8);
    uint32_t options = A(3);
    if (!out)
        RET(paramErr);
    wr32(out, 0);
    int i = 0;
    while (i < nobjects && objects[i])
        i++;
    if (i == nobjects) {
        objects = realloc(objects, sizeof *objects * (size_t)(nobjects + 16));
        memset(objects + nobjects, 0, sizeof *objects * 16);
        nobjects += 16;
    }
    gaddr addr = tb_NewPtr(8, false, true);
    if (!addr)
        RET(memFullErr);
    wr32(addr, TXN_TAG);
    wr32(addr + 4, (uint32_t)i);
    txn_t *t = calloc(1, sizeof *t);
    t->addr = addr;
    t->window = window;
    t->is_window = true;
    t->options = options;
    t->encoding = A(6);
    t->refcon = A(9);
    t->readonly = (options & kTXNReadOnlyMask) != 0;
    t->noselect = (options & kTXNNoSelectionMask) != 0;
    t->nowrap = (options & kTXNSingleLineOnlyMask) != 0;
    t->caret_inactive = !(options & kTXNDontDrawCaretWhenInactiveMask);
    t->sel_inactive = !(options & kTXNDontDrawSelectionWhenInactiveMask);
    t->nodrag = (options & kTXNDisableDragAndDropMask) != 0;
    objects[i] = t;
    t->frame = frame ? qrect_get(frame) : window ? qrect_get(window + GrafPort_portRect_off) : (qrect){ 0, 0, 100, 100 };

    port_saver p = enter(t);
    qrect v = view_rect(t);
    gaddr vr = rect_ptr(v);
    t->te = tb_TEStyleNew(vr, vr);
    tb_DisposePtr(vr);
    int16_t font, size;
    uint8_t face;
    default_style(&font, &size, &face);
    gaddr st = text_style(font, face, size, NULL);
    tb_TESetStyle(doFont | doFace | doSize, st, 0, t->te);
    tb_DisposePtr(st);
    if (window && (options & kTXNWantVScrollBarMask))
        t->vbar = new_bar(t, true);
    if (window && (options & kTXNWantHScrollBarMask))
        t->hbar = new_bar(t, false);
    leave(p);
    layout(t);
    if (spec) {
        int32_t e = load_file(t, spec);
        if (e != noErr) { /* success must be complete */
            delete_object(t);
            RET(e);
        }
    }
    if (window && (options & kTXNShowWindowMask))
        tb_ShowWindow(window);
    wr32(out, addr);
    if (frame_id)
        wr32(frame_id, 0);
    RET(noErr);
}

static void delete_object(txn_t *t)
{
    port_saver p = enter(t);
    if (t->vbar)
        tb_DisposeControl(t->vbar);
    if (t->hbar)
        tb_DisposeControl(t->hbar);
    tb_TEDispose(t->te);
    leave(p);
    forget_snapshot(&t->undo);
    forget_snapshot(&t->redo);
    free(t->shadow);
    if (t->spec)
        tb_DisposePtr(t->spec);
    objects[rd32(t->addr + 4)] = NULL;
    wr32(t->addr, 0);
    tb_DisposePtr(t->addr);
    if (tracking == t)
        tracking = NULL;
    free(t);
}

/* TXNDeleteObject(object) */
static void tx_TXNDeleteObject(ppc_cpu *c)
{
    OBJ_ARG_VOID(t);
    delete_object(t);
    DONE();
}

/* TXNResizeFrame(object, width, height, frameID) */
static void tx_TXNResizeFrame(ppc_cpu *c)
{
    OBJ_ARG_VOID(t);
    t->frame.right = (int16_t)(t->frame.left + (int32_t)A(1));
    t->frame.bottom = (int16_t)(t->frame.top + (int32_t)A(2));
    layout(t);
    DONE();
}

/* TXNSetFrameBounds(object, top, left, bottom, right, frameID) */
static void tx_TXNSetFrameBounds(ppc_cpu *c)
{
    OBJ_ARG_VOID(t);
    t->frame = (qrect){ (int16_t)A(1), (int16_t)A(2), (int16_t)A(3), (int16_t)A(4) };
    layout(t);
    DONE();
}

static void tx_TXNGetViewRect(ppc_cpu *c)
{
    OBJ_ARG_VOID(t);
    if (A(1))
        qrect_put(A(1), qrect_get(te_rec(t) + TERec_viewRect_off));
    DONE();
}

/* TXNSetViewRect(object, rect): how much shows, not where lines wrap. */
static void tx_TXNSetViewRect(ppc_cpu *c)
{
    OBJ_ARG_VOID(t);
    if (A(1)) {
        qrect_put(te_rec(t) + TERec_viewRect_off, qrect_get(A(1)));
        sync_bars(t);
    }
    DONE();
}

/* TXNAttachObjectToWindow(object, port, isWindow) */
static void tx_TXNAttachObjectToWindow(ppc_cpu *c)
{
    OBJ_ARG(t);
    if (!A(1))
        RET(paramErr);
    if (t->window && t->window != A(1))
        RET(paramErr); /* one window per object */
    t->window = A(1);
    t->is_window = (A(2) & 0xFF) != 0;
    wr32(te_rec(t) + TERec_inPort_off, t->window);
    if (t->is_window && (t->options & kTXNWantVScrollBarMask) && !t->vbar)
        t->vbar = new_bar(t, true);
    if (t->is_window && (t->options & kTXNWantHScrollBarMask) && !t->hbar)
        t->hbar = new_bar(t, false);
    layout(t);
    RET(noErr);
}

static void tx_TXNIsObjectAttachedToWindow(ppc_cpu *c)
{
    txn_t *t = object_of(A(0));
    RET(t && t->window ? 1 : 0);
}

static void tx_TXNIsObjectAttachedToSpecificWindow(ppc_cpu *c)
{
    OBJ_ARG(t);
    if (!A(2))
        RET(paramErr);
    wr8(A(2), t->window && t->window == A(1));
    RET(noErr);
}

/* ---------------------------------------------------------------------- */
/* Drawing */

static void draw(txn_t *t, gaddr port)
{
    gaddr r = te_rec(t), saved_in = rd32(r + TERec_inPort_off);
    gaddr target = port ? port : t->window;
    if (!target)
        return;
    gaddr saved = qd_port();
    tb_SetPort(target);
    wr32(r + TERec_inPort_off, target);
    qrect v = qrect_get(r + TERec_viewRect_off);
    gaddr vr = rect_ptr(v);
    if (t->has_bg) {
        gaddr old = tb_NewPtr(6, false, true), bg = tb_NewPtr(6, false, true);
        tb_GetBackColor(old);
        for (int i = 0; i < 3; i++)
            wr16(bg + 2 * (uint32_t)i, t->bg[i]);
        tb_RGBBackColor(bg);
        tb_EraseRect(vr);
        tb_TEUpdate(vr, t->te);
        tb_RGBBackColor(old);
        tb_DisposePtr(bg);
        tb_DisposePtr(old);
    } else {
        tb_EraseRect(vr);
        tb_TEUpdate(vr, t->te);
    }
    tb_DisposePtr(vr);
    if (!port || port == t->window) {
        if (t->vbar)
            tb_Draw1Control(t->vbar);
        if (t->hbar)
            tb_Draw1Control(t->hbar);
        if ((t->options & kTXNDrawGrowIconMask) && t->is_window)
            tb_DrawGrowIcon(t->window);
    }
    wr32(r + TERec_inPort_off, saved_in);
    tb_SetPort(saved);
}

/* TXNDraw(object, port) */
static void tx_TXNDraw(ppc_cpu *c)
{
    OBJ_ARG_VOID(t);
    draw(t, A(1));
    DONE();
}

/* TXNUpdate(object): the window's update, all of it. */
static void tx_TXNUpdate(ppc_cpu *c)
{
    OBJ_ARG_VOID(t);
    if (!t->window || !t->is_window)
        DONE();
    gaddr saved = qd_port();
    tb_SetPort(t->window);
    tb_BeginUpdate(t->window);
    draw(t, 0);
    tb_EndUpdate(t->window);
    tb_SetPort(saved);
    DONE();
}

static void tx_TXNForceUpdate(ppc_cpu *c)
{
    OBJ_ARG_VOID(t);
    if (!t->window || !t->is_window)
        DONE();
    port_saver p = enter(t);
    gaddr fr = rect_ptr(t->frame);
    tb_InvalRect(fr);
    tb_DisposePtr(fr);
    leave(p);
    DONE();
}

/* TXNSetBackground(object, &background): an RGB colour. */
static void tx_TXNSetBackground(ppc_cpu *c)
{
    OBJ_ARG(t);
    gaddr b = A(1);
    if (!b || rd32(b) != 1) /* kTXNBackgroundTypeRGB */
        RET(paramErr);
    t->has_bg = true;
    for (int i = 0; i < 3; i++)
        t->bg[i] = rd16(b + 4 + 2 * (uint32_t)i);
    RET(noErr);
}

/* ---------------------------------------------------------------------- */
/* Events */

static void tx_TXNFocus(ppc_cpu *c)
{
    OBJ_ARG_VOID(t);
    bool on = (A(1) & 0xFF) != 0;
    t->focused = on;
    port_saver p = enter(t);
    if (on)
        tb_TEActivate(t->te);
    else
        tb_TEDeactivate(t->te);
    if (!t->bars_active) {
        if (t->vbar)
            tb_HiliteControl(t->vbar, on ? 0 : 255);
        if (t->hbar)
            tb_HiliteControl(t->hbar, on ? 0 : 255);
    }
    leave(p);
    DONE();
}

/* TXNActivate(object, frameID, scrollBarsAlwaysActive) */
static void tx_TXNActivate(ppc_cpu *c)
{
    OBJ_ARG(t);
    if (A(1) != 0)
        RET(kTXNInvalidFrameIDErr);
    t->bars_active = (A(2) & 0xFF) != 0;
    port_saver p = enter(t);
    int16_t h = t->bars_active || t->focused ? 0 : 255;
    if (t->vbar)
        tb_HiliteControl(t->vbar, h);
    if (t->hbar)
        tb_HiliteControl(t->hbar, h);
    leave(p);
    RET(noErr);
}

static bool editable(txn_t *t) { return !t->readonly && !t->nouserio; }

/* Forward delete: the character after an empty selection. */
static void forward_delete(txn_t *t)
{
    int a = sel_start(t), b = sel_end(t);
    if (a == b && b < text_len(t))
        b++;
    replace(t, a, b, NULL, 0);
}

/* A line's leading spaces and tabs, for auto-indent. */
static int indent_of(txn_t *t, int at, uint8_t *out)
{
    gaddr p = text_ptr(t);
    int s = at;
    while (s > 0 && rd8(p + (uint32_t)s - 1) != '\r')
        s--;
    int n = 0;
    while (s + n < at && n < 64 && (rd8(p + (uint32_t)(s + n)) == ' ' || rd8(p + (uint32_t)(s + n)) == '\t')) {
        out[n] = rd8(p + (uint32_t)(s + n));
        n++;
    }
    return n;
}

/* Scroll the text by (dh, dv) pixels, pinned. */
static void scroll_by(txn_t *t, int dh, int dv)
{
    port_saver p = enter(t);
    tb_TEPinScroll((int16_t)dh, (int16_t)dv, t->te);
    sync_bars(t);
    leave(p);
}

/* TXNKeyDown(object, event) */
static void tx_TXNKeyDown(ppc_cpu *c)
{
    OBJ_ARG_VOID(t);
    gaddr ev = A(1);
    if (!ev)
        DONE();
    uint32_t msg = rd32(ev + EventRecord_message_off);
    uint8_t ch = (uint8_t)msg;
    uint16_t mods = rd16(ev + EventRecord_modifiers_off);
    qrect v = qrect_get(te_rec(t) + TERec_viewRect_off);
    int page = v.bottom - v.top - 16;
    switch (ch) {
    case 0x01: /* home */
        scroll_by(t, 0, 32000);
        DONE();
    case 0x04: /* end */
        scroll_by(t, 0, -32000);
        DONE();
    case 0x0B: /* page up */
        scroll_by(t, 0, page);
        DONE();
    case 0x0C: /* page down */
        scroll_by(t, 0, -page);
        DONE();
    case 0x1C: case 0x1D: case 0x1E: case 0x1F: { /* arrows */
        if (t->noselect)
            DONE();
        port_saver p = enter(t);
        int a = sel_start(t), b = sel_end(t);
        tb_TEKey(ch, t->te);
        if (mods & shiftKey) { /* extending: from the end the arrow left */
            int na = sel_start(t);
            int lo = ch == 0x1C || ch == 0x1E ? (na < a ? na : a) : a;
            int hi = ch == 0x1C || ch == 0x1E ? b : (na > b ? na : b);
            tb_TESetSelect(lo, hi, t->te);
        }
        leave(p);
        t->typing = false;
        after_edit(t);
        DONE();
    }
    }
    if (!editable(t) || (mods & cmdKey))
        DONE();
    will_edit(t, kTXNTypingAction);
    if (ch == 0x7F) {
        forward_delete(t);
    } else if (ch == 0x08 || t->echo) {
        int a = sel_start(t), b = sel_end(t);
        if (ch == 0x08) {
            if (a == b && a > 0)
                a--;
            replace(t, a, b, NULL, 0);
        } else {
            replace(t, a, b, &ch, 1);
        }
    } else if (ch == '\r' && t->autoindent) {
        uint8_t ind[66];
        int a = sel_start(t), b = sel_end(t);
        ind[0] = '\r';
        int n = indent_of(t, a, ind + 1);
        replace(t, a, b, ind, n + 1);
    } else {
        port_saver p = enter(t);
        tb_TEKey(ch, t->te);
        leave(p);
    }
    bump(t, true);
    after_edit(t);
    DONE();
}

/* A scroll bar's action while tracked: pascal void (control, part). */
static void bar_action(cpu68k *cpu, uint16_t tw)
{
    uint32_t ret = cpu_pop32(cpu);
    int16_t part = (int16_t)cpu_pop16(cpu);
    gaddr ctl = cpu_pop32(cpu);
    txn_t *t = tracking;
    if (t && part) {
        bool vertical = ctl == t->vbar;
        qrect v = qrect_get(te_rec(t) + TERec_viewRect_off);
        int line = vertical ? (int16_t)rd16(te_rec(t) + TERec_lineHeight_off) : 16;
        if (line <= 0)
            line = 16;
        int page = (vertical ? v.bottom - v.top : v.right - v.left) - line;
        int d = part == inUpButton ? line : part == inDownButton ? -line : part == inPageUp ? page : part == inPageDown ? -page : 0;
        if (vertical)
            tb_TEPinScroll(0, (int16_t)d, t->te);
        else
            tb_TEPinScroll((int16_t)d, 0, t->te);
        sync_bars(t);
    }
    cpu_push32(cpu, ret);
}

/* TXNClick(object, event): in the text, TextEdit's selecting; in a scroll
 * bar, scrolling. */
static void tx_TXNClick(ppc_cpu *c)
{
    OBJ_ARG_VOID(t);
    gaddr ev = A(1);
    if (!ev || !t->window)
        DONE();
    port_saver p = enter(t);
    gaddr pt = tb_NewPtr(4, false, true);
    wr32(pt, rd32(ev + EventRecord_where_off));
    tb_GlobalToLocal(pt);
    uint32_t local = rd32(pt);
    tb_DisposePtr(pt);
    int16_t v = (int16_t)(local >> 16), h = (int16_t)local;
    gaddr bar = 0;
    for (int i = 0; i < 2; i++) {
        gaddr ctl = i ? t->hbar : t->vbar;
        if (ctl && rd32(ctl)) {
            qrect r = qrect_get(rd32(ctl) + 8); /* contrlRect */
            if (v >= r.top && v < r.bottom && h >= r.left && h < r.right)
                bar = ctl;
        }
    }
    if (bar) {
        gaddr cp = tb_NewPtr(4, false, true);
        int16_t part = tb_FindControl(local, t->window, cp);
        tb_DisposePtr(cp);
        if (!action_stub)
            action_stub = native_register(bar_action, "TXNScrollAction", 0);
        tracking = t;
        if (part == inThumb) {
            int16_t before = tb_GetControlValue(bar);
            tb_TrackControl(bar, local, 0);
            int d = before - tb_GetControlValue(bar);
            if (bar == t->vbar)
                tb_TEPinScroll(0, (int16_t)d, t->te);
            else
                tb_TEPinScroll((int16_t)d, 0, t->te);
            sync_bars(t);
        } else if (part) {
            tb_TrackControl(bar, local, action_stub);
        }
        tracking = NULL;
    } else if (!t->noselect) {
        qrect vr = qrect_get(te_rec(t) + TERec_viewRect_off);
        if (v >= vr.top && v < vr.bottom && h >= vr.left && h < vr.right) {
            bool ext = (rd16(ev + EventRecord_modifiers_off) & shiftKey) != 0;
            tb_TEClick(local, ext ? 1 : 0, t->te);
            t->typing = false;
            sync_bars(t);
        }
    }
    leave(p);
    DONE();
}

/* TXNAdjustCursor(object, cursorRgn): the I-beam over the text. */
static void tx_TXNAdjustCursor(ppc_cpu *c)
{
    OBJ_ARG_VOID(t);
    if (!t->window)
        DONE();
    port_saver p = enter(t);
    gaddr pt = tb_NewPtr(4, false, true);
    tb_GetMouse(pt);
    uint32_t local = rd32(pt);
    tb_DisposePtr(pt);
    int16_t v = (int16_t)(local >> 16), h = (int16_t)local;
    qrect vr = qrect_get(te_rec(t) + TERec_viewRect_off);
    bool in = v >= vr.top && v < vr.bottom && h >= vr.left && h < vr.right;
    if (in && !t->noselect) {
        gaddr ib = tb_GetCursor(1); /* iBeamCursor */
        if (ib && rd32(ib))
            tb_SetCursor(rd32(ib));
    } else {
        tb_InitCursor();
    }
    if (A(1) && in)
        tb_SetRectRgn(A(1), vr.left, vr.top, vr.right, vr.bottom);
    leave(p);
    DONE();
}

static void tx_TXNIdle(ppc_cpu *c)
{
    OBJ_ARG_VOID(t);
    if (t->focused) {
        port_saver p = enter(t);
        tb_TEIdle(t->te);
        leave(p);
    }
    DONE();
}

/* TXNGetSleepTicks(object): the caret's blink when one is shown. */
static void tx_TXNGetSleepTicks(ppc_cpu *c)
{
    txn_t *t = object_of(A(0));
    RET(t && t->focused && sel_start(t) == sel_end(t) ? tb_GetCaretTime() : 0xFFFFFFFFu);
}

/* TXNGrowWindow(object, event): the window resized, a whole-window frame
 * with it. */
static void tx_TXNGrowWindow(ppc_cpu *c)
{
    OBJ_ARG_VOID(t);
    gaddr ev = A(1);
    if (!ev || !t->window || !t->is_window)
        DONE();
    gaddr lim = rect_ptr((qrect){ 64, 64, 32000, 32000 });
    int32_t size = tb_GrowWindow(t->window, rd32(ev + EventRecord_where_off), lim);
    tb_DisposePtr(lim);
    if (size) {
        qrect pr = qrect_get(t->window + GrafPort_portRect_off);
        bool whole = t->frame.right == pr.right && t->frame.bottom == pr.bottom;
        tb_SizeWindow(t->window, (int16_t)(size & 0xFFFF), (int16_t)(size >> 16), 1);
        if (whole) {
            pr = qrect_get(t->window + GrafPort_portRect_off);
            t->frame.right = pr.right;
            t->frame.bottom = pr.bottom;
            layout(t);
        }
        port_saver p = enter(t);
        gaddr r = rect_ptr(qrect_get(t->window + GrafPort_portRect_off));
        tb_InvalRect(r);
        tb_DisposePtr(r);
        leave(p);
    }
    DONE();
}

/* TXNZoomWindow(object, part) */
static void tx_TXNZoomWindow(ppc_cpu *c)
{
    OBJ_ARG_VOID(t);
    if (!t->window || !t->is_window)
        DONE();
    qrect pr = qrect_get(t->window + GrafPort_portRect_off);
    bool whole = t->frame.right == pr.right && t->frame.bottom == pr.bottom;
    port_saver p = enter(t);
    gaddr r = rect_ptr(pr);
    tb_EraseRect(r);
    tb_DisposePtr(r);
    tb_ZoomWindow(t->window, (int16_t)A(1), 0);
    if (whole) {
        pr = qrect_get(t->window + GrafPort_portRect_off);
        t->frame.right = pr.right;
        t->frame.bottom = pr.bottom;
        layout(t);
    }
    r = rect_ptr(qrect_get(t->window + GrafPort_portRect_off));
    tb_InvalRect(r);
    tb_DisposePtr(r);
    leave(p);
    DONE();
}

/* ---------------------------------------------------------------------- */
/* Selection */

static void tx_TXNSelectAll(ppc_cpu *c)
{
    OBJ_ARG_VOID(t);
    port_saver p = enter(t);
    tb_TESetSelect(0, text_len(t), t->te);
    leave(p);
    t->typing = false;
    DONE();
}

static void tx_TXNGetSelection(ppc_cpu *c)
{
    OBJ_ARG_VOID(t);
    if (A(1))
        wr32(A(1), (uint32_t)sel_start(t));
    if (A(2))
        wr32(A(2), (uint32_t)sel_end(t));
    DONE();
}

static void tx_TXNSetSelection(ppc_cpu *c)
{
    OBJ_ARG(t);
    int a = offset(t, A(1), true), b = offset(t, A(2), false);
    port_saver p = enter(t);
    tb_TESetSelect(a < b ? a : b, a < b ? b : a, t->te);
    leave(p);
    t->typing = false;
    RET(noErr);
}

static void tx_TXNIsSelectionEmpty(ppc_cpu *c)
{
    txn_t *t = object_of(A(0));
    RET(!t || sel_start(t) == sel_end(t));
}

/* TXNShowSelection(object, showEnd) */
static void tx_TXNShowSelection(ppc_cpu *c)
{
    OBJ_ARG_VOID(t);
    port_saver p = enter(t);
    if (A(1) & 0xFF) {
        tb_TESelView(t->te);
    } else { /* the start in view */
        int a = sel_start(t), b = sel_end(t);
        tb_TESetSelect(a, a, t->te);
        tb_TESelView(t->te);
        tb_TESetSelect(a, b, t->te);
    }
    sync_bars(t);
    leave(p);
    DONE();
}

/* TXNOffsetToPoint(object, offset, &point), TXNPointToOffset(object, point, &offset) */
static void tx_TXNOffsetToPoint(ppc_cpu *c)
{
    OBJ_ARG(t);
    if (!A(2))
        RET(paramErr);
    if (A(1) > (uint32_t)text_len(t))
        RET(kTXNOutsideOfFrameErr);
    port_saver p = enter(t);
    int32_t pt = tb_TEGetPoint((int16_t)A(1), t->te);
    leave(p);
    wr32(A(2), (uint32_t)pt);
    RET(noErr);
}

static void tx_TXNPointToOffset(ppc_cpu *c)
{
    OBJ_ARG(t);
    if (!A(2))
        RET(paramErr);
    port_saver p = enter(t);
    int16_t o = tb_TEGetOffset(A(1), t->te);
    leave(p);
    wr32(A(2), (uint32_t)o);
    RET(noErr);
}

/* TXNGetLineCount(object, &lines), TXNGetLineMetrics(object, line, &width, &height) */
static void tx_TXNGetLineCount(ppc_cpu *c)
{
    OBJ_ARG(t);
    if (!A(1))
        RET(paramErr);
    int n = (int16_t)rd16(te_rec(t) + TERec_nLines_off);
    wr32(A(1), (uint32_t)(n > 0 ? n : 1));
    RET(noErr);
}

static void tx_TXNGetLineMetrics(ppc_cpu *c)
{
    OBJ_ARG(t);
    int n = (int16_t)rd16(te_rec(t) + TERec_nLines_off);
    uint32_t line = A(1);
    if (line >= (uint32_t)(n > 0 ? n : 1))
        RET(kTXNOutsideOfLineErr);
    gaddr r = te_rec(t);
    int s = (int16_t)rd16(r + TERec_lineStarts_off + 2 * line), e = (int16_t)rd16(r + TERec_lineStarts_off + 2 * (line + 1));
    port_saver p = enter(t);
    int32_t a = tb_TEGetPoint((int16_t)s, t->te), b = tb_TEGetPoint((int16_t)e, t->te);
    int h = tb_TEGetHeight((int16_t)(line + 1), (int16_t)(line + 1), t->te);
    leave(p);
    int w = (int16_t)b - (int16_t)a;
    if ((int16_t)(b >> 16) != (int16_t)(a >> 16)) { /* the end is on the next line: to the dest's edge */
        qrect d = qrect_get(r + TERec_destRect_off);
        w = d.right - (int16_t)a;
    }
    if (A(2))
        wr32(A(2), (uint32_t)((w > 0 ? w : 0) << 16));
    if (A(3))
        wr32(A(3), (uint32_t)(h << 16));
    RET(noErr);
}

/* ---------------------------------------------------------------------- */
/* Editing */

static void tx_TXNCanUndo(ppc_cpu *c)
{
    txn_t *t = object_of(A(0));
    if (!t || !t->undo.valid)
        RET(0);
    if (A(1))
        wr32(A(1), t->undo.action);
    RET(1);
}

static void tx_TXNCanRedo(ppc_cpu *c)
{
    txn_t *t = object_of(A(0));
    if (!t || !t->redo.valid)
        RET(0);
    if (A(1))
        wr32(A(1), t->redo.action);
    RET(1);
}

static void undo_redo(txn_t *t, snapshot *from, snapshot *to)
{
    if (!from->valid)
        return;
    snapshot keep = *from;
    memset(from, 0, sizeof *from);
    take_snapshot(t, to, keep.action);
    restore_snapshot(t, &keep);
    forget_snapshot(&keep);
    t->typing = false;
    bump(t, false);
    after_edit(t);
}

static void tx_TXNUndo(ppc_cpu *c)
{
    OBJ_ARG_VOID(t);
    undo_redo(t, &t->undo, &t->redo);
    draw(t, 0);
    DONE();
}

static void tx_TXNRedo(ppc_cpu *c)
{
    OBJ_ARG_VOID(t);
    undo_redo(t, &t->redo, &t->undo);
    draw(t, 0);
    DONE();
}

/* Copy: the selection to the desk scrap as text and its styles. */
static void tx_TXNCopy(ppc_cpu *c)
{
    OBJ_ARG(t);
    if (t->echo)
        RET(kTXNCopyNotAllowedInEchoModeErr);
    port_saver p = enter(t);
    if (sel_start(t) != sel_end(t))
        tb_TECopy(t->te);
    leave(p);
    RET(noErr);
}

static void tx_TXNCut(ppc_cpu *c)
{
    OBJ_ARG(t);
    if (t->echo)
        RET(kTXNCopyNotAllowedInEchoModeErr);
    if (!editable(t) || sel_start(t) == sel_end(t))
        RET(noErr);
    will_edit(t, kTXNCutAction);
    port_saver p = enter(t);
    tb_TECopy(t->te);
    leave(p);
    replace(t, sel_start(t), sel_end(t), NULL, 0);
    t->typing = false;
    bump(t, false);
    after_edit(t);
    RET(noErr);
}

static void tx_TXNClear(ppc_cpu *c)
{
    OBJ_ARG(t);
    if (!editable(t) || sel_start(t) == sel_end(t))
        RET(noErr);
    will_edit(t, kTXNClearAction);
    replace(t, sel_start(t), sel_end(t), NULL, 0);
    t->typing = false;
    bump(t, false);
    after_edit(t);
    RET(noErr);
}

/* Whether the desk scrap has text (or Unicode text). */
static int32_t scrap_text(uint32_t type)
{
    gaddr off = tb_NewPtr(4, false, true);
    int32_t n = tb_GetScrap(0, (int32_t)type, off);
    tb_DisposePtr(off);
    return n;
}

static void tx_TXNIsScrapPastable(ppc_cpu *c) { RET(scrap_text(TYPE_TEXT) >= 0 || scrap_text(TYPE_UTXT) >= 0); }

/* Paste: styled text from the desk scrap, or Unicode text made Roman. */
static void tx_TXNPaste(ppc_cpu *c)
{
    OBJ_ARG(t);
    if (!editable(t))
        RET(noErr);
    bool text = scrap_text(TYPE_TEXT) >= 0;
    if (!text && scrap_text(TYPE_UTXT) < 0)
        RET(-102); /* noTypeErr */
    will_edit(t, kTXNPasteAction);
    if (text && !t->echo) {
        port_saver p = enter(t);
        tb_TEStylePaste(t->te);
        leave(p);
    } else {
        gaddr h = tb_NewHandle(0, false, true), off = tb_NewPtr(4, false, true);
        int32_t n = tb_GetScrap(h, (int32_t)(text ? TYPE_TEXT : TYPE_UTXT), off);
        int k = 0;
        uint8_t *bytes = n > 0 ? as_macroman(text ? TYPE_TEXT : TYPE_UTXT, rd32(h), (uint32_t)n, &k) : NULL;
        replace(t, sel_start(t), sel_end(t), bytes, k);
        free(bytes);
        tb_DisposePtr(off);
        tb_DisposeHandle(h);
    }
    t->typing = false;
    bump(t, false);
    after_edit(t);
    RET(noErr);
}

/* The scrap is the desk scrap already. */
static void tx_noErr(ppc_cpu *c) { RET(noErr); }

/* ---------------------------------------------------------------------- */
/* Type attributes */

/* TXNSetTypeAttributes(object, count, attributes[], start, end):
 * TXNTypeAttributes is a tag, a size and the value (in place when it
 * fits in four bytes, else a pointer to it). */
static void tx_TXNSetTypeAttributes(ppc_cpu *c)
{
    OBJ_ARG(t);
    uint32_t n = A(1);
    gaddr attrs = A(2);
    if (n && !attrs)
        RET(paramErr);
    int16_t mode = 0, font = 0, size = 0;
    uint8_t face = 0;
    uint16_t color[3] = { 0, 0, 0 };
    uint32_t action = kTXNChangeFontAction;
    for (uint32_t i = 0; i < n; i++) {
        gaddr a = attrs + 12 * i;
        uint32_t tag = rd32(a), value = rd32(a + 8);
        switch (tag) {
        case TAG_FONTNAME: {
            gaddr num = tb_NewPtr(2, false, true);
            tb_GetFNum(value, num);
            font = (int16_t)rd16(num);
            tb_DisposePtr(num);
            mode |= doFont;
            break;
        }
        case TAG_FONT:
            font = (int16_t)value;
            mode |= doFont;
            break;
        case TAG_SIZE:
            if (value == kTXNIncrementTypeSize || value == kTXNDecrementTypeSize) {
                size = value == kTXNIncrementTypeSize ? 1 : -1;
                mode |= addSize;
            } else if (value != kTXNDontCareTypeSize) {
                size = (int16_t)((int32_t)value > 0x7FFF ? (int32_t)(value + 0x8000) >> 16 : (int32_t)value);
                mode |= doSize;
            }
            action = kTXNChangeFontSizeAction;
            break;
        case TAG_FACE:
            if ((value & 0xFF) != 0xFF) { /* kTXNDontCareTypeStyle */
                face = (uint8_t)(rd32(a + 4) == 1 && value > 0xFF ? value >> 24 : value);
                mode |= doFace;
            }
            action = kTXNChangeStyleAction;
            break;
        case TAG_COLOR:
            if (!value)
                RET(paramErr);
            for (int k = 0; k < 3; k++)
                color[k] = rd16(value + 2 * (uint32_t)k);
            mode |= doColor;
            action = kTXNChangeFontColorAction;
            break;
        case TAG_ENCODING: /* Mac OS Roman is what's kept */
            break;
        default:
            RET(kTXNSomeOrAllTagsInvalidForRunErr);
        }
    }
    if (!mode)
        RET(noErr);
    will_edit(t, action);
    int a = offset(t, A(3), true), b = offset(t, A(4), false);
    int sa = sel_start(t), sb = sel_end(t);
    port_saver p = enter(t);
    gaddr st = text_style(font, face, size, color);
    tb_TESetSelect(a, b, t->te);
    tb_TESetStyle(mode, st, 0, t->te);
    tb_TESetSelect(sa, sb, t->te);
    tb_DisposePtr(st);
    leave(p);
    t->typing = false;
    bump(t, false);
    layout(t);
    RET(noErr);
}

/* Fill requested attributes from a TextStyle. */
static void fill_attrs(gaddr attrs, uint32_t n, gaddr style)
{
    for (uint32_t i = 0; i < n; i++) {
        gaddr a = attrs + 12 * i;
        switch (rd32(a)) {
        case TAG_FONT:
            wr32(a + 8, rd16(style));
            break;
        case TAG_SIZE:
            wr32(a + 8, (uint32_t)rd16(style + 4) << 16);
            break;
        case TAG_FACE:
            wr32(a + 8, rd8(style + 2));
            break;
        case TAG_COLOR:
            if (rd32(a + 8))
                gmemcpy(rd32(a + 8), style + 6, 6);
            break;
        case TAG_FONTNAME:
            if (rd32(a + 8))
                tb_GetFontName((int16_t)rd16(style), rd32(a + 8));
            break;
        case TAG_ENCODING:
            wr32(a + 8, 0);
            break;
        }
    }
}

/* TXNGetContinuousTypeAttributes(object, &flags, count, attributes[]):
 * of the selection. */
static void tx_TXNGetContinuousTypeAttributes(ppc_cpu *c)
{
    OBJ_ARG(t);
    gaddr modep = tb_NewPtr(2, false, true), style = tb_NewPtr(TextStyle_SIZEOF, false, true);
    wr16(modep, doAll);
    port_saver p = enter(t);
    tb_TEContinuousStyle(modep, style, t->te);
    leave(p);
    uint16_t m = rd16(modep);
    if (A(1))
        wr32(A(1), (m & doFont ? kTXNFontContinuousMask : 0) | (m & doSize ? kTXNSizeContinuousMask : 0) |
                       (m & doFace ? kTXNStyleContinuousMask : 0) | (m & doColor ? kTXNColorContinuousMask : 0));
    if (A(3))
        fill_attrs(A(3), A(2), style);
    tb_DisposePtr(style);
    tb_DisposePtr(modep);
    RET(noErr);
}

/* The style runs: TextEdit's, as (start, style) pairs ending at the
 * text's length + 1. */
static int runs(txn_t *t, int **starts)
{
    gaddr sh = tb_TEGetStyleHandle(t->te);
    gaddr sr = sh ? rd32(sh) : 0;
    int n = sr ? (int16_t)rd16(sr) : 0;
    *starts = malloc(sizeof **starts * (size_t)(n + 2));
    if (!n) {
        (*starts)[0] = 0;
        (*starts)[1] = text_len(t);
        return 1;
    }
    for (int i = 0; i <= n; i++)
        (*starts)[i] = (int16_t)rd16(sr + 20 + 4 * (uint32_t)i);
    if ((*starts)[n] > text_len(t))
        (*starts)[n] = text_len(t);
    return n;
}

/* The runs meeting [a, b): their indices first..last. */
static int runs_in(txn_t *t, int a, int b, int *first, int **starts)
{
    int n = runs(t, starts), k = 0;
    *first = -1;
    for (int i = 0; i < n; i++) {
        int s = (*starts)[i], e = (*starts)[i + 1];
        if (e > a && s < (b > a ? b : a + 1)) {
            if (*first < 0)
                *first = i;
            k++;
        } else if (s == e && s == a && a == b && *first < 0) {
            *first = i;
            k++;
        }
    }
    return k;
}

static void tx_TXNCountRunsInRange(ppc_cpu *c)
{
    OBJ_ARG(t);
    if (!A(3))
        RET(paramErr);
    int a = offset(t, A(1), true), b = offset(t, A(2), false), first, *starts;
    int k = runs_in(t, a, b, &first, &starts);
    free(starts);
    wr32(A(3), (uint32_t)(k ? k : 1));
    RET(noErr);
}

/* TXNGetIndexedRunInfoFromRange(object, index, start, end, &runStart,
 * &runEnd, &type, count, attributes[]) */
static void tx_TXNGetIndexedRunInfoFromRange(ppc_cpu *c)
{
    OBJ_ARG(t);
    int a = offset(t, A(2), true), b = offset(t, A(3), false), first, *starts;
    int k = runs_in(t, a, b, &first, &starts);
    uint32_t index = A(1);
    if (index >= (uint32_t)(k ? k : 1)) {
        free(starts);
        RET(kTXNRunIndexOutofBoundsErr);
    }
    int run = (first < 0 ? 0 : first) + (int)index;
    int s = starts[run], e = starts[run + 1];
    free(starts);
    if (A(4))
        wr32(A(4), (uint32_t)(s > a ? s : a));
    if (A(5))
        wr32(A(5), (uint32_t)(e < b ? e : b));
    if (A(6))
        wr32(A(6), TYPE_TEXT);
    if (A(8) && A(7)) {
        gaddr style = tb_NewPtr(TextStyle_SIZEOF, false, true), lh = tb_NewPtr(2, false, true),
              asc = tb_NewPtr(2, false, true);
        port_saver p = enter(t);
        tb_TEGetStyle((int16_t)s, style, lh, asc, t->te);
        leave(p);
        fill_attrs(A(8), A(7), style);
        tb_DisposePtr(asc);
        tb_DisposePtr(lh);
        tb_DisposePtr(style);
    }
    RET(noErr);
}

/* ---------------------------------------------------------------------- */
/* Object controls */

#define TAG(a, b, c, d) FOUR(a, b, c, d)

static int16_t te_just(uint32_t j)
{
    return j == kTXNFlushRight ? teFlushRight : j == kTXNCenter ? teCenter : j == kTXNFlushLeft ? teFlushLeft
                                                                                                 : teFlushDefault;
}

static void set_defaults(txn_t *t)
{
    t->just = kTXNFlushDefault;
    t->line_dir = 0;
    t->readonly = (t->options & kTXNReadOnlyMask) != 0;
    t->noselect = (t->options & kTXNNoSelectionMask) != 0;
    t->nowrap = (t->options & kTXNSingleLineOnlyMask) != 0;
    t->autoindent = t->nouserio = false;
    t->tabs = 0;
    memset(t->margins, 0, sizeof t->margins);
}

/* TXNSetTXNObjectControls(object, clearAll, count, tags[], data[]) */
static void tx_TXNSetTXNObjectControls(ppc_cpu *c)
{
    OBJ_ARG(t);
    uint32_t n = A(2);
    gaddr tags = A(3), data = A(4);
    if (n && (!tags || !data))
        RET(paramErr);
    if (A(1) & 0xFF)
        set_defaults(t);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t tag = rd32(tags + 4 * i), v = rd32(data + 4 * i);
        switch (tag) {
        case TAG('l', 'n', 'd', 'r'):
            t->line_dir = v;
            break;
        case TAG('j', 'u', 's', 't'):
            t->just = v;
            break;
        case TAG('i', 'o', 'p', 'v'):
            t->readonly = v != 0;
            break;
        case TAG('s', 'l', 's', 't'):
            t->noselect = v == 0; /* kTXNSelectionOn is true */
            break;
        case TAG('w', 'w', 'r', 's'):
            t->nowrap = v != 0;
            break;
        case TAG('a', 'u', 'i', 'n'):
            t->autoindent = v != 0;
            break;
        case TAG('t', 'a', 'b', 's'):
            t->tabs = v;
            break;
        case TAG('r', 'f', 'c', 'n'):
            t->refcon = v;
            break;
        case TAG('m', 'a', 'r', 'g'):
            if (v)
                for (int k = 0; k < 4; k++)
                    t->margins[k] = (int16_t)rd16(v + 2 * (uint32_t)k);
            break;
        case TAG('n', 'u', 'i', 'o'):
            t->nouserio = v != 0;
            break;
        case TAG('c', 'b', 'c', 'b'):
            t->carbon_events = v;
            break;
        case TAG('d', 'c', 'r', 't'):
            t->caret_inactive = v != 0;
            break;
        case TAG('d', 's', 'l', 'n'):
            t->sel_inactive = v != 0;
            break;
        case TAG('d', 'r', 'a', 'g'):
            t->nodrag = v != 0;
            break;
        case TAG('i', 'n', 's', 't'): /* inline input: the host's */
        case TAG('k', 'b', 's', 'y'): /* keyboard sync */
        case TAG('f', 'l', 'a', 't'): /* movies */
        case TAG('f', 'S', 'u', 'b'): /* font substitution */
            break;
        default:
            RET(kTXNSomeOrAllTagsInvalidForRunErr);
        }
    }
    port_saver p = enter(t);
    tb_TESetAlignment(te_just(t->just), t->te);
    leave(p);
    layout(t);
    RET(noErr);
}

/* TXNGetTXNObjectControls(object, count, tags[], data[]) */
static void tx_TXNGetTXNObjectControls(ppc_cpu *c)
{
    OBJ_ARG(t);
    uint32_t n = A(1);
    gaddr tags = A(2), data = A(3);
    if (n && (!tags || !data))
        RET(paramErr);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t tag = rd32(tags + 4 * i), v;
        switch (tag) {
        case TAG('l', 'n', 'd', 'r'): v = t->line_dir; break;
        case TAG('j', 'u', 's', 't'): v = t->just; break;
        case TAG('i', 'o', 'p', 'v'): v = t->readonly; break;
        case TAG('s', 'l', 's', 't'): v = !t->noselect; break;
        case TAG('w', 'w', 'r', 's'): v = t->nowrap; break;
        case TAG('a', 'u', 'i', 'n'): v = t->autoindent; break;
        case TAG('t', 'a', 'b', 's'): v = t->tabs; break;
        case TAG('r', 'f', 'c', 'n'): v = t->refcon; break;
        case TAG('n', 'u', 'i', 'o'): v = t->nouserio; break;
        case TAG('c', 'b', 'c', 'b'): v = t->carbon_events; break;
        case TAG('d', 'c', 'r', 't'): v = t->caret_inactive; break;
        case TAG('d', 's', 'l', 'n'): v = t->sel_inactive; break;
        case TAG('d', 'r', 'a', 'g'): v = t->nodrag; break;
        case TAG('i', 'n', 's', 't'): case TAG('k', 'b', 's', 'y'): case TAG('f', 'l', 'a', 't'):
        case TAG('f', 'S', 'u', 'b'):
            v = 0;
            break;
        case TAG('m', 'a', 'r', 'g'): /* into the program's TXNMargins */
            v = rd32(data + 4 * i);
            if (v)
                for (int k = 0; k < 4; k++)
                    wr16(v + 2 * (uint32_t)k, (uint16_t)t->margins[k]);
            break;
        default:
            RET(kTXNSomeOrAllTagsInvalidForRunErr);
        }
        wr32(data + 4 * i, v);
    }
    RET(noErr);
}

/* TXNSetFontDefaults(object, count, defaults[]) and its getter: the
 * Roman entry is what new text gets. */
static void tx_TXNSetFontDefaults(ppc_cpu *c)
{
    OBJ_ARG(t);
    uint32_t n = A(1);
    if (n && !A(2))
        RET(paramErr);
    ndefault_fonts = 0;
    for (uint32_t i = 0; i < n && i < 16; i++)
        gget(default_fonts[ndefault_fonts++], A(2) + 14 * i, 14);
    if (!text_len(t)) {
        int16_t font, size;
        uint8_t face;
        default_style(&font, &size, &face);
        gaddr st = text_style(font, face, size, NULL);
        port_saver p = enter(t);
        tb_TESetStyle(doFont | doFace | doSize, st, 0, t->te);
        leave(p);
        tb_DisposePtr(st);
    }
    RET(noErr);
}

static void tx_TXNGetFontDefaults(ppc_cpu *c)
{
    OBJ_ARG(t);
    if (!A(1))
        RET(paramErr);
    if (A(2))
        for (int i = 0; i < ndefault_fonts && (uint32_t)i < rd32(A(1)); i++)
            gput(A(2) + 14 * (uint32_t)i, default_fonts[i], 14);
    wr32(A(1), (uint32_t)ndefault_fonts);
    RET(noErr);
}

/* TXNEchoMode(object, echoChar, encoding, on) */
static void tx_TXNEchoMode(ppc_cpu *c)
{
    OBJ_ARG(t);
    bool on = (A(3) & 0xFF) != 0;
    if (on == t->echo && (!on || t->echo_char == (uint16_t)A(1)))
        RET(noErr);
    int n = text_len(t);
    if (on) {
        if (!t->echo) { /* the text so far becomes the real text */
            free(t->shadow);
            t->shadow = malloc(n ? (size_t)n : 1);
            gget(t->shadow, text_ptr(t), (uint32_t)n);
            t->shadow_len = n;
        }
        t->echo = true;
        t->echo_char = (uint16_t)A(1);
        uint8_t *real = malloc(n ? (size_t)n : 1);
        memcpy(real, t->shadow, (size_t)n);
        int len = t->shadow_len;
        t->shadow_len = 0;
        free(t->shadow);
        t->shadow = NULL;
        port_saver p = enter(t);
        tb_TESetText(0, 0, t->te);
        leave(p);
        replace(t, 0, 0, real, len);
        free(real);
    } else {
        uint8_t *real = t->shadow;
        int len = t->shadow_len;
        t->shadow = NULL;
        t->echo = false;
        port_saver p = enter(t);
        tb_TESetText(0, 0, t->te);
        leave(p);
        replace(t, 0, 0, real, len);
        free(real);
    }
    after_edit(t);
    RET(noErr);
}

/* ---------------------------------------------------------------------- */
/* Counts */

static void tx_TXNGetChangeCount(ppc_cpu *c)
{
    txn_t *t = object_of(A(0));
    RET(t ? t->changes : 0);
}

static void tx_TXNGetActionChangeCount(ppc_cpu *c)
{
    OBJ_ARG(t);
    if (!A(2))
        RET(paramErr);
    uint32_t o = A(1), n = 0;
    if (o & kTXNTextInputCountMask)
        n += t->typing_changes;
    if (o & kTXNRunCountMask)
        n += t->run_changes;
    wr32(A(2), n);
    RET(noErr);
}

static void tx_TXNClearActionChangeCount(ppc_cpu *c)
{
    OBJ_ARG(t);
    if (A(1) & kTXNTextInputCountMask)
        t->typing_changes = 0;
    if (A(1) & kTXNRunCountMask)
        t->run_changes = 0;
    RET(noErr);
}

/* ---------------------------------------------------------------------- */
/* Files */

/* TXNSave(object, type, resType, encoding, spec, dataRef, resRef): the
 * text at the data fork's mark, and its styles as a 'styl' (or one style
 * as an 'MPSR') resource when there's a resource fork. */
static void tx_TXNSave(ppc_cpu *c)
{
    OBJ_ARG(t);
    uint32_t type = A(1), restype = A(2), enc = A(3);
    gaddr spec = A(4);
    int16_t dref = (int16_t)A(5), rref = (int16_t)A(6);
    if (type != TYPE_TEXT && type != TYPE_UTXT)
        RET(kTXNDataTypeNotAllowedErr);
    gaddr h = tb_NewHandle(0, false, true), out = tb_NewPtr(4, false, true);
    int32_t e = get_data(t, 0, kTXNEndOffset, out,
                         type == TYPE_UTXT || enc == kTXNUnicodeEncoding ? TYPE_UTXT : TYPE_TEXT);
    tb_DisposeHandle(h);
    h = rd32(out);
    tb_DisposePtr(out);
    if (e)
        RET(e);
    gaddr pb = tb_NewPtr(ParamBlockRec_SIZEOF, false, true);
    wr16(pb + IOParam_ioRefNum_off, (uint16_t)dref);
    tb_HLock(h);
    wr32(pb + IOParam_ioBuffer_off, rd32(h));
    wr32(pb + IOParam_ioReqCount_off, (uint32_t)tb_GetHandleSize(h));
    wr16(pb + IOParam_ioPosMode_off, 0);
    e = tb_PBWrite(pb, false);
    if (e == noErr) { /* the file ends where the text does */
        tb_PBGetFPos(pb, false);
        wr32(pb + IOParam_ioMisc_off, rd32(pb + IOParam_ioPosOffset_off));
        tb_PBSetEOF(pb, false);
    }
    tb_DisposePtr(pb);
    tb_DisposeHandle(h);
    if (e)
        RET(e);
    if (rref > 0 && type == TYPE_TEXT && !t->echo) {
        int16_t saved = tb_CurResFile();
        tb_UseResFile(rref);
        bool styl = restype == TYPE_STYL || (t->options & kTXNSaveStylesAsSTYLResourceMask);
        uint32_t rt = styl ? TYPE_STYL : TYPE_MPSR;
        int16_t rid = styl ? 128 : 1005;
        gaddr old = tb_Get1Resource((int32_t)rt, rid);
        if (old)
            tb_RemoveResource(old);
        gaddr res;
        if (styl) {
            port_saver p = enter(t);
            int a = sel_start(t), b = sel_end(t);
            tb_TESetSelect(0, text_len(t), t->te);
            res = tb_TEGetStyleScrapHandle(t->te);
            tb_TESetSelect(a, b, t->te);
            leave(p);
        } else { /* MPSR 1005: font name (32), size, then window state */
            gaddr style = tb_NewPtr(TextStyle_SIZEOF, false, true), lh = tb_NewPtr(2, false, true),
                  asc = tb_NewPtr(2, false, true);
            port_saver p = enter(t);
            tb_TEGetStyle(0, style, lh, asc, t->te);
            leave(p);
            res = tb_NewHandle(72, false, true);
            gaddr name = tb_NewPtr(256, false, true);
            tb_GetFontName((int16_t)rd16(style), name);
            uint32_t len = rd8(name) < 31 ? rd8(name) : 31;
            gmemcpy(rd32(res), name + 1, len);
            wr16(rd32(res) + 32, rd16(style + 4));
            tb_DisposePtr(name);
            tb_DisposePtr(asc);
            tb_DisposePtr(lh);
            tb_DisposePtr(style);
        }
        if (res) {
            gaddr empty = tb_NewPtr(2, false, true);
            tb_AddResource(res, (int32_t)rt, rid, empty);
            tb_DisposePtr(empty);
            tb_UpdateResFile(rref);
        }
        tb_UseResFile(saved);
    }
    if (spec) {
        if (!t->spec)
            t->spec = tb_NewPtr(FSSpec_SIZEOF, false, true);
        if (t->spec)
            gmemcpy(t->spec, spec, FSSpec_SIZEOF);
    }
    t->changes = 0;
    RET(noErr);
}

/* A file's text (and 'styl' styles) as the object's whole text. */
static int32_t load_file(txn_t *t, gaddr spec)
{
    gaddr ref = tb_NewPtr(2, false, true);
    int16_t e = tb_FSpOpenDF(spec, 1, ref);
    if (e) {
        tb_DisposePtr(ref);
        return e;
    }
    gaddr fi = tb_NewPtr(16, false, true);
    tb_FSpGetFInfo(spec, fi);
    uint32_t type = rd32(fi) == TYPE_UTXT ? TYPE_UTXT : TYPE_TEXT;
    tb_DisposePtr(fi);
    uint32_t got;
    int16_t err;
    uint8_t *raw = read_file((int16_t)rd16(ref), kTXNEndOffset, &got, &err);
    gaddr pb = tb_NewPtr(ParamBlockRec_SIZEOF, false, true);
    wr16(pb + IOParam_ioRefNum_off, rd16(ref));
    tb_PBClose(pb, false);
    tb_DisposePtr(pb);
    tb_DisposePtr(ref);
    if (err) {
        free(raw);
        return err;
    }
    gaddr g = tb_NewPtr((int32_t)(got ? got : 1), false, false);
    gput(g, raw, got);
    free(raw);
    int n;
    uint8_t *bytes = as_macroman(type, g, got, &n);
    tb_DisposePtr(g);
    replace(t, 0, text_len(t), bytes, n);
    free(bytes);
    /* SimpleText's styles, when the file has them. */
    int16_t rf = tb_FSpOpenResFile(spec, 1);
    if (rf > 0) {
        gaddr styl = tb_Get1Resource((int32_t)TYPE_STYL, 128);
        if (styl) {
            port_saver p = enter(t);
            tb_TEUseStyleScrap(0, text_len(t), styl, 0, t->te);
            leave(p);
        }
        tb_CloseResFile(rf);
    }
    port_saver p = enter(t);
    tb_TESetSelect(0, 0, t->te);
    leave(p);
    if (!t->spec)
        t->spec = tb_NewPtr(FSSpec_SIZEOF, false, true);
    if (t->spec)
        gmemcpy(t->spec, spec, FSSpec_SIZEOF);
    forget_snapshot(&t->undo);
    forget_snapshot(&t->redo);
    t->changes = 0;
    layout(t);
    return noErr;
}

/* TXNRevert(object): the file as last saved, or empty. */
static void tx_TXNRevert(ppc_cpu *c)
{
    OBJ_ARG(t);
    int32_t e = noErr;
    if (t->spec) {
        e = load_file(t, t->spec);
    } else {
        replace(t, 0, text_len(t), NULL, 0);
        forget_snapshot(&t->undo);
        forget_snapshot(&t->redo);
        t->changes = 0;
        layout(t);
    }
    draw(t, 0);
    RET(e);
}

/* ---------------------------------------------------------------------- */
/* Printing, through the Carbon Printing Manager */

static uint32_t call(const char *name, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    uint32_t args[4] = { a0, a1, a2, a3 };
    return svc_call_export(name, args, 4);
}

/* TXNPageSetup(object) */
static void tx_TXNPageSetup(ppc_cpu *c)
{
    OBJ_ARG(t);
    (void)t;
    gaddr v = tb_NewPtr(4, false, true);
    int32_t e = 0;
    if (!page_format) {
        e = (int32_t)call("PMCreatePageFormat", v, 0, 0, 0);
        page_format = rd32(v);
    }
    gaddr session = 0;
    if (!e)
        e = (int32_t)call("PMCreateSession", v, 0, 0, 0);
    session = e ? 0 : rd32(v);
    if (!e)
        e = (int32_t)call("PMSessionPageSetupDialog", session, page_format, v, 0);
    if (session)
        call("PMRelease", session, 0, 0, 0);
    tb_DisposePtr(v);
    RET(e);
}

/* TXNPrint(object): the text laid out again on the page, page by page. */
static void tx_TXNPrint(ppc_cpu *c)
{
    OBJ_ARG(t);
    gaddr v = tb_NewPtr(32, false, true);
    int32_t e = (int32_t)call("PMCreateSession", v, 0, 0, 0);
    gaddr session = e ? 0 : rd32(v), settings = 0;
    if (!e && !page_format) {
        e = (int32_t)call("PMCreatePageFormat", v, 0, 0, 0);
        page_format = rd32(v);
    }
    if (!e)
        e = (int32_t)call("PMCreatePrintSettings", v, 0, 0, 0);
    settings = e ? 0 : rd32(v);
    if (!e)
        e = (int32_t)call("PMSessionPrintDialog", session, settings, page_format, v);
    bool ok = !e && rd8(v);
    if (ok)
        e = (int32_t)call("PMSessionBeginDocument", session, settings, page_format, 0);
    if (ok && !e) {
        call("PMGetAdjustedPageRect", page_format, v, 0, 0);
        qrect page = { (int16_t)svc_rdd(v), (int16_t)svc_rdd(v + 8), (int16_t)svc_rdd(v + 16),
                       (int16_t)svc_rdd(v + 24) };
        /* A record of the page's width, the text and styles copied in. */
        e = (int32_t)call("PMSessionBeginPage", session, page_format, 0, 0);
        gaddr port = qd_port();
        gaddr pr = rect_ptr(page);
        gaddr te = tb_TEStyleNew(pr, pr);
        port_saver p = enter(t);
        int a = sel_start(t), b = sel_end(t);
        tb_TESetSelect(0, text_len(t), t->te);
        gaddr styles = tb_TEGetStyleScrapHandle(t->te);
        tb_TESetSelect(a, b, t->te);
        leave(p);
        tb_SetPort(port);
        uint8_t *bytes = text_bytes(t, 0, text_len(t));
        gaddr buf = tb_NewPtr(text_len(t) ? text_len(t) : 1, false, false);
        gput(buf, bytes, (uint32_t)text_len(t));
        free(bytes);
        tb_TEStyleInsert(buf, text_len(t), styles, te);
        tb_DisposePtr(buf);
        if (styles)
            tb_DisposeHandle(styles);
        int lines = (int16_t)rd16(rd32(te) + TERec_nLines_off), line = 0, height = page.bottom - page.top;
        while (!e && line < lines) {
            /* As many lines as fit on this page. */
            int first = line, used = 0;
            while (line < lines) {
                int h = tb_TEGetHeight((int16_t)(line + 1), (int16_t)(line + 1), te);
                if (used + h > height && line > first)
                    break;
                used += h;
                line++;
            }
            int top = tb_TEGetHeight((int16_t)first, 1, te) * (first > 0);
            qrect d = { (int16_t)(page.top - top), page.left, (int16_t)(page.bottom - top), page.right };
            qrect_put(rd32(te) + TERec_destRect_off, d);
            qrect_put(pr, (qrect){ page.top, page.left, (int16_t)(page.top + used), page.right });
            qrect_put(rd32(te) + TERec_viewRect_off, qrect_get(pr));
            gaddr clip = tb_NewRgn();
            tb_GetClip(clip);
            tb_ClipRect(pr);
            tb_TEUpdate(pr, te);
            tb_SetClip(clip);
            tb_DisposeRgn(clip);
            e = (int32_t)call("PMSessionEndPage", session, 0, 0, 0);
            if (!e && line < lines) {
                e = (int32_t)call("PMSessionBeginPage", session, page_format, 0, 0);
                tb_SetPort(port);
            }
        }
        if (!lines)
            call("PMSessionEndPage", session, 0, 0, 0);
        tb_TEDispose(te);
        tb_DisposePtr(pr);
        int32_t e2 = (int32_t)call("PMSessionEndDocument", session, 0, 0, 0);
        if (!e)
            e = e2;
    }
    if (settings)
        call("PMRelease", settings, 0, 0, 0);
    if (session)
        call("PMRelease", session, 0, 0, 0);
    tb_DisposePtr(v);
    RET(e);
}

/* ---------------------------------------------------------------------- */
/* Finding */

static uint8_t fold(uint8_t b) { return b >= 'A' && b <= 'Z' ? (uint8_t)(b + 32) : b; }
static bool white(uint8_t b) { return b <= ' '; }

/* TXNFind(object, matchText, type, options, start, end, findProc, refCon,
 * &matchStart, &matchEnd) */
static void tx_TXNFind(ppc_cpu *c)
{
    OBJ_ARG(t);
    gaddr match = A(1), proc = A(6), os = A(8), oe = A(9);
    uint32_t type = A(2), opts = A(3);
    if (os)
        wr32(os, 0xFFFFFFFFu);
    if (oe)
        wr32(oe, 0xFFFFFFFFu);
    if (type != TYPE_TEXT && type != TYPE_UTXT)
        RET(kTXNNoMatchErr); /* nothing but text is held */
    if (!match)
        RET(paramErr);
    int a = offset(t, A(4), true), b = offset(t, A(5), false);
    gaddr pat = rd32(match);
    int32_t plen = (int32_t)rd32(match + 4);
    if (plen < 0 || (plen && !pat))
        RET(paramErr);
    int n;
    uint8_t *needle = as_macroman(type, pat, (uint32_t)plen, &n);
    if (proc) { /* the program matches */
        gaddr gs = tb_NewPtr(12, false, true);
        uint8_t *hay = text_bytes(t, 0, text_len(t));
        gaddr text = tb_NewPtr(text_len(t) ? text_len(t) : 1, false, false);
        gput(text, hay, (uint32_t)text_len(t));
        free(hay);
        uint32_t args[11] = { match, TYPE_TEXT, opts, text + (uint32_t)a, 0, (uint32_t)a, (uint32_t)(b - a),
                              gs, gs + 4, gs + 8, A(7) };
        int32_t e = (int32_t)svc_call_upp(proc, uppTXNFindProcInfo, args, 11);
        bool found = rd8(gs + 8) != 0;
        if (os && found)
            wr32(os, rd32(gs));
        if (oe && found)
            wr32(oe, rd32(gs + 4));
        tb_DisposePtr(text);
        tb_DisposePtr(gs);
        free(needle);
        RET(e ? e : found ? noErr : kTXNNoMatchErr);
    }
    uint8_t *hay = text_bytes(t, 0, text_len(t));
    int len = text_len(t), found = -1;
    for (int i = a; n > 0 && i + n <= b && found < 0; i++) {
        int k = 0;
        while (k < n && ((opts & kTXNIgnoreCaseMask) ? fold(hay[i + k]) == fold(needle[k]) : hay[i + k] == needle[k]))
            k++;
        if (k < n)
            continue;
        if ((opts & kTXNEntireWordMask) && ((i > 0 && !white(hay[i - 1])) || (i + n < len && !white(hay[i + n]))))
            continue;
        found = i;
    }
    free(hay);
    free(needle);
    if (found < 0)
        RET(kTXNNoMatchErr);
    if (os)
        wr32(os, (uint32_t)found);
    if (oe)
        wr32(oe, (uint32_t)(found + n));
    RET(noErr);
}

/* ---------------------------------------------------------------------- */
/* Font menus: the Font Manager's families in a menu (and its
 * submenus none: each family is one item). */

#define FONT_MENU_TAG 0x54584e66u /* 'TXNf' */

/* TXNNewFontMenuObject(menu, menuID, startHierID, &object) */
static void tx_TXNNewFontMenuObject(ppc_cpu *c)
{
    gaddr menu = A(0), out = A(3);
    if (!menu || !out)
        RET(paramErr);
    tb_AppendResMenu(menu, (int32_t)FOUR('F', 'O', 'N', 'T'));
    gaddr o = tb_NewPtr(12, false, true);
    if (!o)
        RET(memFullErr);
    wr32(o, FONT_MENU_TAG);
    wr32(o + 4, menu);
    wr16(o + 8, (uint16_t)A(1));
    wr32(out, o);
    RET(noErr);
}

static bool font_menu(gaddr o) { return o && o < 0xF0000000u && rd32(o) == FONT_MENU_TAG; }

static void tx_TXNGetFontMenuHandle(ppc_cpu *c)
{
    if (!font_menu(A(0)) || !A(1))
        RET(paramErr);
    wr32(A(1), rd32(A(0) + 4));
    RET(noErr);
}

static void tx_TXNDisposeFontMenuObject(ppc_cpu *c)
{
    if (!font_menu(A(0)))
        RET(paramErr);
    wr32(A(0), 0);
    tb_DisposePtr(A(0));
    RET(noErr);
}

/* TXNDoFontMenuSelection(object, fontMenu, menuID, item): the item's
 * family on the selection. */
static void tx_TXNDoFontMenuSelection(ppc_cpu *c)
{
    OBJ_ARG(t);
    if (!font_menu(A(1)))
        RET(paramErr);
    gaddr name = tb_NewPtr(256, false, true), num = tb_NewPtr(2, false, true);
    tb_GetMenuItemText(rd32(A(1) + 4), (int16_t)A(3), name);
    tb_GetFNum(name, num);
    int16_t font = (int16_t)rd16(num);
    tb_DisposePtr(num);
    tb_DisposePtr(name);
    will_edit(t, kTXNChangeFontAction);
    gaddr st = text_style(font, 0, 0, NULL);
    port_saver p = enter(t);
    tb_TESetStyle(doFont, st, 1, t->te);
    leave(p);
    tb_DisposePtr(st);
    t->typing = false;
    bump(t, false);
    layout(t);
    RET(noErr);
}

/* TXNPrepareFontMenu(object, fontMenu): the selection's family checked. */
static void tx_TXNPrepareFontMenu(ppc_cpu *c)
{
    if (!font_menu(A(1)))
        RET(paramErr);
    txn_t *t = object_of(A(0));
    gaddr menu = rd32(A(1) + 4);
    int16_t n = tb_CountMItems(menu);
    int16_t font = -1;
    if (t) {
        gaddr modep = tb_NewPtr(2, false, true), style = tb_NewPtr(TextStyle_SIZEOF, false, true);
        wr16(modep, doFont);
        port_saver p = enter(t);
        tb_TEContinuousStyle(modep, style, t->te);
        leave(p);
        if (rd16(modep) & doFont)
            font = (int16_t)rd16(style);
        tb_DisposePtr(style);
        tb_DisposePtr(modep);
    }
    gaddr name = tb_NewPtr(256, false, true), num = tb_NewPtr(2, false, true);
    for (int16_t i = 1; i <= n; i++) {
        tb_GetMenuItemText(menu, i, name);
        tb_GetFNum(name, num);
        tb_CheckItem(menu, i, (int8_t)(font >= 0 && (int16_t)rd16(num) == font));
    }
    tb_DisposePtr(num);
    tb_DisposePtr(name);
    RET(noErr);
}

/* ---------------------------------------------------------------------- */
/* Text boxes, in an ATSUI style */

/* Lines of [0, n) that fit width (greedy, at spaces), into breaks. */
static int wrap(gaddr port, const uint8_t *b, int n, int width, int *breaks, int max)
{
    int k = 0, start = 0;
    while (start < n && k < max) {
        int i = start, last_space = -1, end = n;
        while (i < n) {
            if (b[i] == '\r' || b[i] == '\n') {
                end = i + 1;
                break;
            }
            if (b[i] == ' ')
                last_space = i + 1;
            if (width > 0 && qd_text_width(port, b + start, i + 1 - start) > width && i > start) {
                end = last_space > start ? last_space : i;
                break;
            }
            i++;
        }
        breaks[k++] = end;
        start = end;
    }
    return k;
}

/* TXNDrawUnicodeTextBox(text, length, &box, style, &options) */
static int32_t text_box(const uint16_t *u, uint32_t n, gaddr box, gaddr style, gaddr options)
{
    if (!box)
        return paramErr;
    uint32_t opts = options ? rd32(options) : 0;
    int32_t flush = options && (opts & kTXNSetFlushnessMask) ? (int32_t)rd32(options + 4) : 0;
    uint8_t *b = malloc(n + 1);
    int len = (int)svc_utf16_to_macroman(u, n, b);
    gaddr port = qd_port();
    int16_t sf = (int16_t)rd16(port + GrafPort_txFont_off), ss = (int16_t)rd16(port + GrafPort_txSize_off);
    uint8_t sface = rd8(port + GrafPort_txFace_off);
    gaddr fg = tb_NewPtr(6, false, true);
    tb_GetForeColor(fg);
    svc_qdstyle q;
    if (atsui_style_qd(style, &q)) {
        tb_TextFont(q.font);
        tb_TextSize(q.size);
        tb_TextFace(q.face);
        if (q.has_color) {
            gaddr rgb = tb_NewPtr(6, false, true);
            for (int i = 0; i < 3; i++)
                wr16(rgb + 2 * (uint32_t)i, q.color[i]);
            tb_RGBForeColor(rgb);
            tb_DisposePtr(rgb);
        }
    }
    qrect r = qrect_get(box);
    int breaks[512], nlines = wrap(port, b, len, r.right - r.left, breaks, 512);
    gaddr info = tb_NewPtr(8, false, true);
    tb_GetFontInfo(info);
    int asc = (int16_t)rd16(info), desc = (int16_t)rd16(info + 2), lead = (int16_t)rd16(info + 6);
    tb_DisposePtr(info);
    int y = r.top + asc, widest = 0, start = 0;
    gaddr buf = tb_NewPtr(len ? len : 1, false, false);
    gput(buf, b, (uint32_t)len);
    for (int l = 0; l < nlines; l++) {
        int end = breaks[l], shown = end;
        while (shown > start && (b[shown - 1] == '\r' || b[shown - 1] == '\n' || b[shown - 1] == ' '))
            shown--;
        int w = qd_text_width(port, b + start, shown - start);
        widest = w > widest ? w : widest;
        if (!(opts & kTXNDontDrawTextMask)) {
            int x = r.left + (int)((int64_t)(r.right - r.left - w) * flush >> 30);
            tb_MoveTo((int16_t)x, (int16_t)y);
            tb_DrawText(buf, (int16_t)start, (int16_t)(shown - start));
        }
        y += asc + desc + lead;
        start = end;
    }
    tb_DisposePtr(buf);
    if (!(opts & kTXNDontUpdateBoxRectMask)) {
        r.right = (int16_t)(r.left + widest);
        r.bottom = (int16_t)(r.top + nlines * (asc + desc + lead));
        qrect_put(box, r);
    }
    tb_TextFont(sf);
    tb_TextSize(ss);
    tb_TextFace(sface);
    tb_RGBForeColor(fg);
    tb_DisposePtr(fg);
    free(b);
    return noErr;
}

static void tx_TXNDrawUnicodeTextBox(ppc_cpu *c)
{
    uint32_t n = A(1);
    if (n && !A(0))
        RET(paramErr);
    uint16_t *u = malloc(sizeof *u * (n + 1));
    for (uint32_t i = 0; i < n; i++)
        u[i] = rd16(A(0) + 2 * i);
    int32_t e = text_box(u, n, A(2), A(3), A(4));
    free(u);
    RET(e);
}

static void tx_TXNDrawCFStringTextBox(ppc_cpu *c)
{
    uint32_t n = svc_cfstring_get(A(0), NULL, 0);
    uint16_t *u = malloc(sizeof *u * (n + 1));
    svc_cfstring_get(A(0), u, n);
    int32_t e = text_box(u, n, A(1), A(2), A(3));
    free(u);
    RET(e);
}

/* ---------------------------------------------------------------------- */
/* Dragging: the program's Drag Manager handlers ask; text dragged in is
 * inserted where it is dropped. */

static void tx_TXNDragTracker(ppc_cpu *c)
{
    OBJ_ARG(t);
    RET(t->nodrag ? -1850 /* dragNotAcceptedErr */ : noErr);
}

static void tx_TXNDragReceiver(ppc_cpu *c)
{
    OBJ_ARG(t);
    if (t->nodrag || !editable(t))
        RET(-1850);
    gaddr drag = A(3), v = tb_NewPtr(8, false, true);
    if (tb_CountDragItems(drag, v) || !rd16(v) || tb_GetDragItemReferenceNumber(drag, 1, v)) {
        tb_DisposePtr(v);
        RET(-1850);
    }
    uint32_t ref = rd32(v);
    if (tb_GetFlavorDataSize(drag, ref, (int32_t)TYPE_TEXT, v)) {
        tb_DisposePtr(v);
        RET(-1850);
    }
    int32_t size = (int32_t)rd32(v);
    gaddr data = tb_NewPtr(size ? size : 1, false, false);
    tb_GetFlavorData(drag, ref, (int32_t)TYPE_TEXT, data, v, 0);
    size = (int32_t)rd32(v);
    uint8_t *bytes = malloc(size ? (size_t)size : 1);
    gget(bytes, data, (uint32_t)size);
    will_edit(t, kTXNDropAction);
    replace(t, sel_start(t), sel_end(t), bytes, size);
    free(bytes);
    tb_DisposePtr(data);
    tb_DisposePtr(v);
    t->typing = false;
    bump(t, false);
    after_edit(t);
    RET(noErr);
}

const ppc_export carbon_mlte_exports[] = {
    { "TXNActivate", tx_TXNActivate },
    { "TXNAdjustCursor", tx_TXNAdjustCursor },
    { "TXNAttachObjectToWindow", tx_TXNAttachObjectToWindow },
    { "TXNCanRedo", tx_TXNCanRedo },
    { "TXNCanUndo", tx_TXNCanUndo },
    { "TXNClear", tx_TXNClear },
    { "TXNClearActionChangeCount", tx_TXNClearActionChangeCount },
    { "TXNClick", tx_TXNClick },
    { "TXNConvertFromPublicScrap", tx_noErr },
    { "TXNConvertToPublicScrap", tx_noErr },
    { "TXNCopy", tx_TXNCopy },
    { "TXNCountRunsInRange", tx_TXNCountRunsInRange },
    { "TXNCut", tx_TXNCut },
    { "TXNDataSize", tx_TXNDataSize },
    { "TXNDeleteObject", tx_TXNDeleteObject },
    { "TXNDisposeFontMenuObject", tx_TXNDisposeFontMenuObject },
    { "TXNDoFontMenuSelection", tx_TXNDoFontMenuSelection },
    { "TXNDragReceiver", tx_TXNDragReceiver },
    { "TXNDragTracker", tx_TXNDragTracker },
    { "TXNDraw", tx_TXNDraw },
    { "TXNDrawCFStringTextBox", tx_TXNDrawCFStringTextBox },
    { "TXNDrawUnicodeTextBox", tx_TXNDrawUnicodeTextBox },
    { "TXNEchoMode", tx_TXNEchoMode },
    { "TXNFind", tx_TXNFind },
    { "TXNFocus", tx_TXNFocus },
    { "TXNForceUpdate", tx_TXNForceUpdate },
    { "TXNGetActionChangeCount", tx_TXNGetActionChangeCount },
    { "TXNGetChangeCount", tx_TXNGetChangeCount },
    { "TXNGetContinuousTypeAttributes", tx_TXNGetContinuousTypeAttributes },
    { "TXNGetData", tx_TXNGetData },
    { "TXNGetDataEncoded", tx_TXNGetDataEncoded },
    { "TXNGetFontDefaults", tx_TXNGetFontDefaults },
    { "TXNGetFontMenuHandle", tx_TXNGetFontMenuHandle },
    { "TXNGetIndexedRunInfoFromRange", tx_TXNGetIndexedRunInfoFromRange },
    { "TXNGetLineCount", tx_TXNGetLineCount },
    { "TXNGetLineMetrics", tx_TXNGetLineMetrics },
    { "TXNGetSelection", tx_TXNGetSelection },
    { "TXNGetSleepTicks", tx_TXNGetSleepTicks },
    { "TXNGetTXNObjectControls", tx_TXNGetTXNObjectControls },
    { "TXNGetViewRect", tx_TXNGetViewRect },
    { "TXNGrowWindow", tx_TXNGrowWindow },
    { "TXNIdle", tx_TXNIdle },
    { "TXNInitTextension", tx_TXNInitTextension },
    { "TXNIsObjectAttachedToSpecificWindow", tx_TXNIsObjectAttachedToSpecificWindow },
    { "TXNIsObjectAttachedToWindow", tx_TXNIsObjectAttachedToWindow },
    { "TXNIsScrapPastable", tx_TXNIsScrapPastable },
    { "TXNIsSelectionEmpty", tx_TXNIsSelectionEmpty },
    { "TXNKeyDown", tx_TXNKeyDown },
    { "TXNNewFontMenuObject", tx_TXNNewFontMenuObject },
    { "TXNNewObject", tx_TXNNewObject },
    { "TXNOffsetToPoint", tx_TXNOffsetToPoint },
    { "TXNPageSetup", tx_TXNPageSetup },
    { "TXNPaste", tx_TXNPaste },
    { "TXNPointToOffset", tx_TXNPointToOffset },
    { "TXNPrepareFontMenu", tx_TXNPrepareFontMenu },
    { "TXNPrint", tx_TXNPrint },
    { "TXNRedo", tx_TXNRedo },
    { "TXNResizeFrame", tx_TXNResizeFrame },
    { "TXNRevert", tx_TXNRevert },
    { "TXNSave", tx_TXNSave },
    { "TXNSelectAll", tx_TXNSelectAll },
    { "TXNSetBackground", tx_TXNSetBackground },
    { "TXNSetData", tx_TXNSetData },
    { "TXNSetDataFromFile", tx_TXNSetDataFromFile },
    { "TXNSetFontDefaults", tx_TXNSetFontDefaults },
    { "TXNSetFrameBounds", tx_TXNSetFrameBounds },
    { "TXNSetSelection", tx_TXNSetSelection },
    { "TXNSetTXNObjectControls", tx_TXNSetTXNObjectControls },
    { "TXNSetTypeAttributes", tx_TXNSetTypeAttributes },
    { "TXNSetViewRect", tx_TXNSetViewRect },
    { "TXNShowSelection", tx_TXNShowSelection },
    { "TXNTerminateTextension", tx_TXNTerminateTextension },
    { "TXNUndo", tx_TXNUndo },
    { "TXNUpdate", tx_TXNUpdate },
    { "TXNVersionInformation", tx_TXNVersionInformation },
    { "TXNZoomWindow", tx_TXNZoomWindow },
};
const unsigned carbon_mlte_export_count = sizeof carbon_mlte_exports / sizeof carbon_mlte_exports[0];
