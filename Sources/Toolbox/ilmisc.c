/*
 * InterfaceLib's remaining utility calls with 68k traps: the Script
 * Manager's tokenizer and its other ScriptUtil calls, Pack6's script and
 * language orders, the Font Manager's FetchFontInfo and QDTextBounds, font
 * menus, GetPixRowBytes and custom transfer procedures, cursor components,
 * Start.h's parameter RAM, the Extension Manager after startup, and the
 * Printing Manager's driver loading.
 */
#include <string.h>

#include "qd_internal.h"
#include "tb_internal.h"

enum { unimpErr = -4, invalidComponentID = -3000 };

/* ---------------------------------------------------------------------- */
/* IntlTokenize: Roman text into tokens, as the Roman tokenizer did.
 * TokenBlock: source 0, sourceLength 4, tokenList 8, tokenLength 12,
 * tokenCount 16, stringList 20, stringLength 24, stringCount 28, doString
 * 32, doAppend 33, doAlphanumeric 34, doNest 35, leftDelims 36,
 * rightDelims 40, leftComment 44, rightComment 52, escapeCode 60,
 * decimalCode 62.  A TokenRec: theToken, position, length,
 * stringPosition (14 bytes). */
enum {
    tokenUnknown = 0, tokenWhite = 1, tokenLeftLit = 2, tokenRightLit = 3, tokenAlpha = 4, tokenNumeric = 5,
    tokenNewLine = 6, tokenLeftComment = 7, tokenRightComment = 8, tokenLiteral = 9, tokenEscape = 10,
    tokenRealNum = 12, tokenEmpty = -1,
};
enum { tokenOK = 0, tokenOverflow = 1, stringOverflow = 2, badDelim = 3, badEnding = 4 };

/* A character's own token (the symbols), or tokenUnknown. */
static int16_t symbol_token(uint8_t ch)
{
    static const struct { uint8_t ch; uint8_t token; } map[] = {
        { '(', 16 }, { ')', 17 }, { '[', 18 }, { ']', 19 }, { '{', 20 }, { '}', 21 }, { 0xC7, 22 }, { 0xC8, 23 },
        { '+', 24 }, { '-', 25 }, { '*', 26 }, { 0xD6, 27 }, { 0xB1, 28 }, { '/', 29 }, { '\\', 30 }, { '<', 31 },
        { '>', 32 }, { '=', 33 }, { 0xB2, 35 }, { 0xB3, 37 }, { 0xAD, 40 }, { '!', 43 }, { '~', 44 }, { ',', 45 },
        { '.', 46 }, { 0xD2, 47 }, { 0xD3, 48 }, { 0xD4, 49 }, { 0xD5, 50 }, { '"', 51 }, { '\'', 52 },
        { ';', 53 }, { '%', 54 }, { '^', 55 }, { '_', 56 }, { '&', 57 }, { '@', 58 }, { '|', 59 }, { '?', 60 },
        { 0xB9, 61 }, { 0xC3, 62 }, { 0xB7, 63 }, { 0xBA, 64 }, { 0xB5, 65 }, { 0xBD, 66 }, { 0xB0, 67 },
        { ':', 68 }, { '#', 69 }, { '$', 70 }, { 0xDB, 73 }, { 0xDC, 74 }, { 0xDD, 75 }, { 0xE4, 76 },
        { 0xC9, 77 }, { 0xE1, 78 },
    };
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (map[i].ch == ch)
            return map[i].token;
    return tokenUnknown;
}

/* Two characters that make one token: <= >= == := <> != */
static int16_t pair_token(uint8_t a, uint8_t b)
{
    if (b == '=')
        switch (a) {
        case '<': return 34;
        case '>': return 36;
        case '=': return 38;
        case ':': return 39;
        case '!': return 42;
        }
    return a == '<' && b == '>' ? 41 : tokenUnknown;
}

static bool is_alpha(uint8_t ch)
{
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'))
        return true;
    /* Mac Roman's letters */
    return (ch >= 0x80 && ch <= 0x9F) || ch == 0xA7 || ch == 0xAE || ch == 0xAF || ch == 0xBB || ch == 0xBC ||
           ch == 0xBE || ch == 0xBF || (ch >= 0xCB && ch <= 0xCF) || ch == 0xD8 || ch == 0xD9 || ch == 0xDE ||
           ch == 0xDF || (ch >= 0xE5 && ch <= 0xEF) || (ch >= 0xF1 && ch <= 0xF4) || ch == 0xF5;
}

static bool is_digit(uint8_t ch) { return ch >= '0' && ch <= '9'; }

typedef struct {
    gaddr tb, src;
    uint32_t len;
    int16_t result;
} tokenizer;

/* The token at i (a symbol or a pair of them), and its length. */
static int16_t token_at(const tokenizer *t, uint32_t i, uint32_t *n)
{
    uint8_t ch = rd8(t->src + i);
    if (i + 1 < t->len) {
        int16_t p = pair_token(ch, rd8(t->src + i + 1));
        if (p) {
            *n = 2;
            return p;
        }
    }
    *n = 1;
    return symbol_token(ch);
}

/* Does the token sequence (one or two tokens, the second tokenEmpty when
 * none) start at i?  Its length. */
static uint32_t match_delim(const tokenizer *t, uint32_t i, int16_t first, int16_t second)
{
    if (first == tokenEmpty || first == tokenUnknown || i >= t->len)
        return 0;
    uint32_t n1, n2;
    if (token_at(t, i, &n1) != first)
        return 0;
    if (second == tokenEmpty)
        return n1;
    if (i + n1 >= t->len || token_at(t, i + n1, &n2) != second)
        return 0;
    return n1 + n2;
}

static void emit(tokenizer *t, int16_t token, uint32_t at, uint32_t n)
{
    if (t->result)
        return;
    gaddr tb = t->tb;
    uint32_t count = rd32(tb + 16);
    if ((int32_t)count >= (int32_t)rd32(tb + 12)) {
        t->result = tokenOverflow;
        return;
    }
    gaddr rec = rd32(tb + 8) + count * 14;
    gaddr str = 0;
    if (rd8(tb + 32)) { /* doString: a Pascal copy in the string list */
        uint32_t used = rd32(tb + 28), m = n > 255 ? 255 : n;
        if (used + 1 + m > rd32(tb + 24)) {
            t->result = stringOverflow;
            return;
        }
        str = rd32(tb + 20) + used;
        wr8(str, (uint8_t)m);
        gmemcpy(str + 1, t->src + at, m);
        wr32(tb + 28, used + 1 + m + ((1 + m) & 1)); /* kept even */
    }
    wr16(rec, (uint16_t)token);
    wr32(rec + 2, t->src + at);
    wr32(rec + 6, n);
    wr32(rec + 10, str);
    wr32(tb + 16, count + 1);
}

int16_t tb_IntlTokenize(gaddr tb)
{
    if (!tb)
        return paramErr;
    tokenizer t = { tb, rd32(tb), rd32(tb + 4), tokenOK };
    if (!rd8(tb + 33)) { /* doAppend */
        wr32(tb + 16, 0);
        wr32(tb + 28, 0);
    }
    bool alnum = rd8(tb + 34), nest = rd8(tb + 35);
    int16_t escape = (int16_t)rd16(tb + 60), decimal = (int16_t)rd16(tb + 62);
    uint32_t i = 0;
    while (i < t.len && !t.result) {
        uint8_t ch = rd8(t.src + i);
        uint32_t start = i, n;
        /* Comments: up to two openers and closers of one or two tokens. */
        int which = -1;
        for (int k = 0; k < 2 && which < 0; k++)
            if ((n = match_delim(&t, i, (int16_t)rd16(tb + 44 + 4 * k), (int16_t)rd16(tb + 46 + 4 * k))))
                which = k;
        if (which >= 0) {
            emit(&t, tokenLeftComment, i, n);
            i += n;
            int depth = 1;
            int16_t c1 = (int16_t)rd16(tb + 52 + 4 * which), c2 = (int16_t)rd16(tb + 54 + 4 * which);
            int16_t o1 = (int16_t)rd16(tb + 44 + 4 * which), o2 = (int16_t)rd16(tb + 46 + 4 * which);
            while (i < t.len && depth) {
                uint32_t m;
                if ((m = match_delim(&t, i, c1, c2))) {
                    if (!--depth) {
                        emit(&t, tokenRightComment, i, m);
                        i += m;
                        break;
                    }
                    i += m;
                } else if (nest && (m = match_delim(&t, i, o1, o2))) {
                    depth++;
                    i += m;
                } else
                    i++;
            }
            if (depth && i >= t.len)
                t.result = badEnding;
            continue;
        }
        /* Literals: from a left delimiter to its right one; the escape
         * token keeps the next character in. */
        which = -1;
        for (int k = 0; k < 2 && which < 0; k++)
            if ((n = match_delim(&t, i, (int16_t)rd16(tb + 36 + 2 * k), tokenEmpty)))
                which = k;
        if (which >= 0) {
            emit(&t, tokenLeftLit, i, n);
            i += n;
            uint32_t body = i, m = 0;
            int16_t right = (int16_t)rd16(tb + 40 + 2 * which);
            while (i < t.len && !(m = match_delim(&t, i, right, tokenEmpty))) {
                uint32_t e = match_delim(&t, i, escape, tokenEmpty);
                i += e ? e + 1 : 1;
            }
            if (i > t.len)
                i = t.len;
            if (i > body)
                emit(&t, tokenLiteral, body, i - body);
            if (!m) {
                t.result = badEnding;
                break;
            }
            emit(&t, tokenRightLit, i, m);
            i += m;
            continue;
        }
        if (ch == ' ' || ch == '\t' || ch == 0xCA) {
            while (i < t.len && ((ch = rd8(t.src + i)) == ' ' || ch == '\t' || ch == 0xCA))
                i++;
            emit(&t, tokenWhite, start, i - start);
        } else if (ch == '\r' || ch == '\n') {
            i++;
            emit(&t, tokenNewLine, start, 1);
        } else if (is_alpha(ch)) {
            while (i < t.len && (is_alpha(ch = rd8(t.src + i)) || (alnum && is_digit(ch))))
                i++;
            emit(&t, tokenAlpha, start, i - start);
        } else if (is_digit(ch)) {
            int16_t token = tokenNumeric;
            while (i < t.len && is_digit(rd8(t.src + i)))
                i++;
            uint32_t d;
            if (i + 1 < t.len && decimal != tokenUnknown && decimal != tokenEmpty &&
                token_at(&t, i, &d) == decimal && is_digit(rd8(t.src + i + d))) {
                i += d;
                while (i < t.len && is_digit(rd8(t.src + i)))
                    i++;
                token = tokenRealNum;
            }
            emit(&t, token, start, i - start);
        } else {
            int16_t token = token_at(&t, i, &n);
            if (escape != tokenUnknown && token == escape && i + n < t.len) {
                emit(&t, tokenEscape, i, n + 1);
                i += n + 1;
                continue;
            }
            emit(&t, token, i, n);
            i += n;
        }
    }
    return t.result;
}

/* NFindWord: FindWord's, by the script's own break table (Roman's). */
void tb_NFindWord(gaddr textPtr, int16_t textLength, int16_t offset, int8_t leadingEdge, gaddr nbreaks, gaddr offsets)
{
    tb_FindWord(textPtr, textLength, offset, leadingEdge, 0, offsets);
}

/* PortionText: PortionLine's for a whole run at one to one. */
int32_t tb_PortionText(gaddr textPtr, int32_t textLength)
{
    return tb_PortionLine(textPtr, textLength, 0, 0x00010001, 0x00010001);
}

/* FormatRecToString: the canonical format back to its text (ours keeps
 * the text: fLength, fVersion, then the characters), and where each of
 * its three parts (positive; negative; zero) is in it, from 1. */
int16_t tb_FormatRecToString(gaddr myCanonical, gaddr partsTable, gaddr outString, gaddr positions)
{
    if (!myCanonical || !outString)
        return paramErr;
    uint8_t n = rd8(myCanonical);
    wr8(outString, n);
    gmemcpy(outString + 1, myCanonical + 2, n);
    if (positions) {
        int part = 0, start = 1;
        for (int i = 0; i < 3; i++) {
            wr16(positions + 4 * (gaddr)i, 0);
            wr16(positions + 4 * (gaddr)i + 2, 0);
        }
        for (int i = 1; i <= n + 1 && part < 3; i++)
            if (i == n + 1 || rd8(outString + i) == ';') {
                wr16(positions + 4 * (gaddr)part, (uint16_t)start);
                wr16(positions + 4 * (gaddr)part + 2, (uint16_t)(i - start));
                part++;
                start = i + 1;
            }
    }
    return 0; /* fFormatOK */
}

/* A script system's routines and QuickDraw patches: installed into a
 * table of ours and handed back; no other script system is installed to
 * call them. */
enum { MAX_ROUTINES = 32 };
static struct {
    bool qd, before, printing;
    int16_t selector, script;
    gaddr routine;
} routines[MAX_ROUTINES];

static gaddr find_routine(bool qd, int16_t selector, bool before, bool printing, int16_t script, int *slot)
{
    *slot = -1;
    for (int i = 0; i < MAX_ROUTINES; i++) {
        if (!routines[i].routine) {
            if (*slot < 0)
                *slot = i;
            continue;
        }
        if (routines[i].qd == qd && routines[i].selector == selector && routines[i].before == before &&
            routines[i].printing == printing && routines[i].script == script) {
            *slot = i;
            return routines[i].routine;
        }
    }
    return 0;
}

static void set_routine(bool qd, int16_t selector, bool before, bool printing, int16_t script, gaddr routine)
{
    int slot;
    find_routine(qd, selector, before, printing, script, &slot);
    if (slot < 0)
        return;
    routines[slot].qd = qd;
    routines[slot].selector = selector;
    routines[slot].before = before;
    routines[slot].printing = printing;
    routines[slot].script = script;
    routines[slot].routine = routine;
}

gaddr tb_GetScriptUtilityAddress(int16_t selector, int8_t before, int16_t script)
{
    int slot;
    return find_routine(false, selector, before, false, script, &slot);
}

void tb_SetScriptUtilityAddress(int16_t selector, int8_t before, gaddr routineAddr, int16_t script)
{
    set_routine(false, selector, before, false, script, routineAddr);
}

gaddr tb_GetScriptQDPatchAddress(int16_t trapNum, int8_t before, int8_t forPrinting, int16_t script)
{
    int slot;
    return find_routine(true, trapNum, before, forPrinting, script, &slot);
}

void tb_SetScriptQDPatchAddress(int16_t trapNum, int8_t before, int8_t forPrinting, gaddr routineAddr, int16_t script)
{
    set_routine(true, trapNum, before, forPrinting, script, routineAddr);
}

/* Scripts in order: Roman first, then by script code; languages by code. */
int16_t tb_IUScriptOrder(int16_t script1, int16_t script2)
{
    return script1 == script2 ? 0 : script1 < script2 ? -1 : 1;
}

int16_t tb_IULangOrder(int16_t l1, int16_t l2) { return l1 == l2 ? 0 : l1 < l2 ? -1 : 1; }

/* IUStringOrder: by script, then the text, then by language. */
int16_t il_string_order(gaddr a, gaddr b, int16_t aScript, int16_t bScript, int16_t aLang, int16_t bLang)
{
    int16_t r = tb_IUScriptOrder(aScript, bScript);
    if (!r && a && b)
        r = tb_IUMagString(a + 1, b + 1, rd8(a), rd8(b));
    return r ? r : tb_IULangOrder(aLang, bLang);
}

/* ---------------------------------------------------------------------- */
/* Fonts */

/* FetchFontInfo: GetFontInfo's for a font, size and style, the port's
 * own left as they were. */
int16_t tb_FetchFontInfo(int16_t fontID, int16_t fontSize, int16_t fontStyle, gaddr info)
{
    gaddr port = qd_port();
    if (!port || !info)
        return paramErr;
    uint16_t font = rd16(port + GrafPort_txFont_off), size = rd16(port + GrafPort_txSize_off);
    uint8_t face = rd8(port + GrafPort_txFace_off);
    tb_TextFont(fontID);
    tb_TextSize(fontSize);
    tb_TextFace((int16_t)(fontStyle & 0x7F));
    tb_GetFontInfo(info);
    wr16(port + GrafPort_txFont_off, font);
    wr16(port + GrafPort_txSize_off, size);
    wr8(port + GrafPort_txFace_off, face);
    return noErr;
}

/* QDTextBounds: the text's box drawn at (0, 0) in the port's font: its
 * width, from ascent above to descent below. */
void tb_QDTextBounds(int16_t byteCount, gaddr textAddr, gaddr bounds)
{
    if (!bounds)
        return;
    gaddr fi = tb_NewPtr(8, false, true);
    tb_GetFontInfo(fi);
    int16_t w = byteCount > 0 && textAddr ? tb_TextWidth(textAddr, 0, byteCount) : 0;
    wr16(bounds, (uint16_t)-(int16_t)rd16(fi));
    wr16(bounds + 2, 0);
    wr16(bounds + 4, rd16(fi + 2));
    wr16(bounds + 6, (uint16_t)w);
    tb_DisposePtr(fi);
}

/* Font menus: every script's fonts are Roman ones here, so the filter
 * keeps them all. */
void tb_InsertFontResMenu(gaddr theMenu, int16_t afterItem, int16_t scriptFilter)
{
    tb_InsertResMenu(theMenu, (int32_t)fourcc("FONT"), afterItem);
}

void tb_InsertIntlResMenu(gaddr theMenu, uint32_t theType, int16_t afterItem, int16_t scriptFilter)
{
    tb_InsertResMenu(theMenu, (int32_t)theType, afterItem);
}

/* ---------------------------------------------------------------------- */
/* QuickDraw */

int32_t tb_GetPixRowBytes(gaddr pm) { return pm && rd32(pm) ? rd16(rd32(pm) + PixMap_rowBytes_off) & 0x3FFF : 0; }

/* Custom transfer procedures: QuickDraw here draws its own modes only,
 * so none is installed; taking one away is no work. */
int16_t tb_GetPortCustomXFerProc(gaddr port, gaddr proc, gaddr flags, gaddr refCon)
{
    if (proc)
        wr32(proc, 0);
    if (flags)
        wr32(flags, 0);
    if (refCon)
        wr32(refCon, 0);
    return port ? noErr : paramErr;
}

int16_t tb_SetPortCustomXFerProc(gaddr port, gaddr proc, uint32_t flags, uint32_t refCon)
{
    if (!port)
        return paramErr;
    return proc ? unimpErr : noErr;
}

/* Cursor components: one can be opened and closed, but the cursor is the
 * host's, drawn from the program's Cursor records, and no component can
 * take it over. */
int16_t tb_OpenCursorComponent(uint32_t c, gaddr ci)
{
    uint32_t inst = c ? components_open(c) : 0;
    if (ci)
        wr32(ci, inst);
    return inst ? noErr : invalidComponentID;
}

int16_t tb_CloseCursorComponent(uint32_t ci)
{
    if (!ci)
        return paramErr;
    components_close(ci);
    return noErr;
}

int16_t tb_SetCursorComponent(uint32_t ci) { return ci ? unimpErr : noErr; }
int16_t tb_CursorComponentChanged(uint32_t ci) { return ci ? noErr : paramErr; }
int16_t tb_CursorComponentSetData(uint32_t ci, int32_t data) { return ci ? noErr : paramErr; }

/* Start.h's parameter RAM calls and the Extension Manager are in power.c
 * and devices.c. */

/* ---------------------------------------------------------------------- */
/* The Printing Manager's driver is ours, in memory and never purged. */
int16_t tb_PrLoadDriver(void) { return noErr; }
void tb_PrNoPurge(void) {}
void tb_PrPurge(void) {}

/* ---------------------------------------------------------------------- */
/* Small calls of other managers */

/* TESetWordBreak: the record's wordBreak field. */
void tb_TESetWordBreak(gaddr wb, gaddr teh)
{
    if (teh && rd32(teh))
        wr32(rd32(teh) + TERec_wordBreak_off, wb);
}

/* SetTagBuffer: no disk keeps tags. */
int16_t tb_SetTagBuffer(gaddr bp) { return noErr; }

/* A fresh machine. */
void ilmisc_reset(void)
{
    memset(routines, 0, sizeof routines);
}

/* LaunchDeskAccessory(spec, name): a desk accessory or Apple menu item,
 * by its name or its file's, opened as the Apple menu opens it. */
int16_t tb_LaunchDeskAccessory(gaddr pFileSpec, gaddr pDAName)
{
    gaddr name = pDAName && rd8(pDAName) ? pDAName : pFileSpec ? pFileSpec + 6 : 0;
    if (!name)
        return paramErr;
    uint8_t s[256];
    int n = rd8(name);
    gget(s, name + 1, (uint32_t)n);
    if (n && s[0] == 0) /* desk accessories' names start with a NUL */
        memmove(s, s + 1, (size_t)--n);
    return fm_open_apple_menu_item(s, n) ? noErr : -43; /* fnfErr */
}

/* LaunchControlPanel: control panels don't run here. */
int16_t tb_LaunchControlPanel(gaddr pFileSpec) { return pFileSpec ? unimpErr : paramErr; }
