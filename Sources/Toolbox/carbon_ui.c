/*
 * CarbonLib's List Manager and TextEdit accessors, the Font Manager's
 * family and font calls (Mac OS 9's FontManager library, over the FOND
 * and 'sfnt' resources of the open resource files), Appearance's text and
 * theme calls, help tags (kept for each window, control and menu; Blue
 * Box shows no tags, as it shows no balloons), and what the CarbonLib
 * files of this area share: CFStrings by way of Core Foundation's
 * exports, calls to other libraries' exports, and the one lookup cfm.c
 * makes for all of them.
 */
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define CARBON_HI_EXPORTS
#include "carbon_hi.h"
#include "forks.h"
#include "os9_toolbox.h"
#include "qd_internal.h"
#include "tb_internal.h"


/* ---- Shared ---- */

uint32_t hi_call(const char *name, const uint32_t *args, int nargs, bool *found)
{
    void (*fn)(ppc_cpu *) = cfm_export_fn(name);
    if (found)
        *found = fn != NULL;
    if (!fn)
        return (uint32_t)paramErr;
    ppc_cpu saved = g_ppc;
    for (int i = 0; i < nargs && i < 8; i++)
        g_ppc.gpr[3 + i] = args[i];
    uint32_t sp = g_ppc.gpr[1];
    if (nargs > 8) {
        /* the rest in a parameter area below the stack */
        sp = (sp - 128) & ~15u;
        for (int i = 8; i < nargs; i++)
            wr32(sp + 24 + 4 * (uint32_t)i, args[i]);
        g_ppc.gpr[1] = sp;
    }
    g_ppc.lr = g_ppc.pc;
    fn(&g_ppc);
    uint32_t r = g_ppc.gpr[3];
    bool halted = g_ppc.halted;
    g_ppc = saved;
    g_ppc.halted = halted;
    return r;
}

gaddr hi_cf_from_pstr(gaddr pstr)
{
    if (!pstr)
        return 0;
    uint32_t n = rd8(pstr);
    gaddr c = tb_NewPtr((int32_t)n + 1, true, true);
    if (!c)
        return 0;
    gmemcpy(c, pstr + 1, n);
    uint32_t args[3] = { 0, c, 0 /* kCFStringEncodingMacRoman */ };
    gaddr s = hi_call("CFStringCreateWithCString", args, 3, NULL);
    tb_DisposePtr(c);
    return (int32_t)s == paramErr ? 0 : s;
}

/* The string's UTF-16 to UTF-8 to Mac Roman. */
void hi_cf_to_pstr(gaddr cf, gaddr out)
{
    wr8(out, 0);
    if (!cf)
        return;
    uint32_t args[4] = { cf };
    bool found = false;
    uint32_t len = hi_call("CFStringGetLength", args, 1, &found);
    if (!found || len > 0x10000)
        return;
    gaddr units = tb_NewPtr((int32_t)(len * 2 + 2), true, true);
    if (!units)
        return;
    args[1] = 0;
    args[2] = len;
    args[3] = units;
    hi_call("CFStringGetCharacters", args, 4, NULL);
    char *utf8 = malloc(len * 3 + 1);
    size_t k = 0;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t u = rd16(units + 2 * i);
        if (u >= 0xD800 && u < 0xDC00 && i + 1 < len) {
            u = 0x10000 + ((u - 0xD800) << 10) + (rd16(units + 2 * (i + 1)) - 0xDC00);
            i++;
        }
        if (u < 0x80) {
            utf8[k++] = (char)u;
        } else if (u < 0x800) {
            utf8[k++] = (char)(0xC0 | u >> 6);
            utf8[k++] = (char)(0x80 | (u & 0x3F));
        } else if (u < 0x10000) {
            utf8[k++] = (char)(0xE0 | u >> 12);
            utf8[k++] = (char)(0x80 | ((u >> 6) & 0x3F));
            utf8[k++] = (char)(0x80 | (u & 0x3F));
        } else {
            utf8[k++] = '?';
        }
        if (k > len * 3 - 4 && i + 1 < len)
            break;
    }
    utf8[k] = 0;
    uint8_t roman[256];
    size_t n = utf8_to_macroman(utf8, roman, sizeof roman);
    if (n > 255)
        n = 255;
    wr8(out, (uint8_t)n);
    gput(out + 1, roman, (uint32_t)n);
    free(utf8);
    tb_DisposePtr(units);
}

void hi_cf_retain(gaddr cf)
{
    uint32_t args[1] = { cf };
    hi_call("CFRetain", args, 1, NULL);
}

void hi_cf_release(gaddr cf)
{
    uint32_t args[1] = { cf };
    hi_call("CFRelease", args, 1, NULL);
}

/* ---- The List Manager ---- */

static gaddr lrec(gaddr list) { return list && rd32(list) ? rd32(list) : 0; }

#define LIST_GET(name, expr)                                                                                           \
    static void ls_##name(ppc_cpu *c)                                                                                  \
    {                                                                                                                  \
        gaddr l = lrec(R(0));                                                                                          \
        RETURN(l ? (expr) : 0);                                                                                        \
    }

/* the ones that copy a field to the caller's and return its address */
#define LIST_COPY(name, off, size)                                                                                     \
    static void ls_##name(ppc_cpu *c)                                                                                  \
    {                                                                                                                  \
        gaddr l = lrec(R(0));                                                                                          \
        if (l && R(1))                                                                                                 \
            gmemcpy(R(1), l + (off), size);                                                                            \
        RETURN(R(1));                                                                                                  \
    }

LIST_GET(GetListActive, rd8(l + ListRec_lActive_off) != 0)
LIST_GET(GetListClickTime, rd32(l + ListRec_clikTime_off))
LIST_GET(GetListDataHandle, rd32(l + ListRec_cells_off))
LIST_GET(GetListDefinition, rd32(l + ListRec_listDefProc_off))
LIST_GET(GetListFlags, rd8(l + ListRec_listFlags_off))
LIST_GET(GetListHorizontalScrollBar, rd32(l + ListRec_hScroll_off))
LIST_GET(GetListVerticalScrollBar, rd32(l + ListRec_vScroll_off))
LIST_GET(GetListPort, rd32(l + ListRec_port_off))
LIST_GET(GetListRefCon, rd32(l + ListRec_refCon_off))
LIST_GET(GetListSelectionFlags, rd8(l + ListRec_selFlags_off))
LIST_GET(GetListUserHandle, rd32(l + ListRec_userHandle_off))
LIST_COPY(GetListCellIndent, ListRec_indent_off, 4)
LIST_COPY(GetListCellSize, ListRec_cellSize_off, 4)
LIST_COPY(GetListClickLocation, ListRec_clikLoc_off, 4)
LIST_COPY(GetListDataBounds, ListRec_dataBounds_off, 8)
LIST_COPY(GetListMouseLocation, ListRec_mouseLoc_off, 4)
LIST_COPY(GetListViewBounds, ListRec_rView_off, 8)
LIST_COPY(GetListVisibleCells, ListRec_visible_off, 8)

/* GetListClickLoop(list): the click-loop routine (not a CarbonLib 1.0
 * export of UI 3.4's, but its Lists.h declares it). */
LIST_GET(GetListClickLoop, rd32(l + ListRec_lClikLoop_off))

#define LIST_SET(name, stmt)                                                                                           \
    static void ls_##name(ppc_cpu *c)                                                                                  \
    {                                                                                                                  \
        gaddr l = lrec(R(0));                                                                                          \
        if (l)                                                                                                         \
            stmt;                                                                                                      \
        DONE();                                                                                                        \
    }

LIST_SET(SetListClickTime, wr32(l + ListRec_clikTime_off, R(1)))
LIST_SET(SetListFlags, wr8(l + ListRec_listFlags_off, (uint8_t)R(1)))
LIST_SET(SetListPort, wr32(l + ListRec_port_off, R(1)))
LIST_SET(SetListRefCon, wr32(l + ListRec_refCon_off, R(1)))
LIST_SET(SetListSelectionFlags, wr8(l + ListRec_selFlags_off, (uint8_t)R(1)))
LIST_SET(SetListUserHandle, wr32(l + ListRec_userHandle_off, R(1)))
LIST_SET(SetListClickLoop, wr32(l + ListRec_lClikLoop_off, R(1)))
LIST_SET(SetListCellIndent, if (R(1)) gmemcpy(l + ListRec_indent_off, R(1), 4))
LIST_SET(SetListLastClick, if (R(1)) gmemcpy(l + ListRec_lastClick_off, R(1), 4))
LIST_SET(SetListViewBounds, if (R(1)) gmemcpy(l + ListRec_rView_off, R(1), 8))

/* Definitions: CreateCustomList's procedure, for the LNew it makes; and
 * RegisterListDefinition's (CarbonLib 1.3), by LDEF ID. */
static gaddr pending_ldef;
static struct {
    int16_t id;
    gaddr def;
} registered_ldefs[16];

gaddr carbonui_list_def(int16_t id)
{
    if (pending_ldef) {
        gaddr d = pending_ldef;
        pending_ldef = 0;
        return d;
    }
    for (int i = 0; i < 16; i++)
        if (registered_ldefs[i].def && registered_ldefs[i].id == id)
            return registered_ldefs[i].def;
    return 0;
}

static gaddr def_handle(gaddr upp)
{
    if (!upp || !mm_is_descriptor(upp))
        return 0;
    uint32_t n = 12 + 20 * (rd16(upp + 10) + 1u);
    gaddr h = tb_NewHandle((int32_t)n, true, true);
    if (h)
        gmemcpy(rd32(h), upp, n);
    return h;
}

enum { kListDefProcPtr = 0, kListDefStandardTextType = 1, kListDefStandardIconType = 2 };

/* CreateCustomList(rView, dataBounds, cellSize, theSpec, theWindow,
 * drawIt, hasGrow, scrollHoriz, scrollVert, outList) */
static void ls_CreateCustomList(ppc_cpu *c)
{
    gaddr spec = R(3), out = R(9);
    if (!R(0) || !R(1) || !spec || !out) {
        RETURN((uint32_t)paramErr);
        return;
    }
    uint32_t kind = rd32(spec);
    if (kind == kListDefProcPtr) {
        pending_ldef = def_handle(rd32(spec + 4));
        if (!pending_ldef) {
            RETURN((uint32_t)paramErr);
            return;
        }
    } else if (kind != kListDefStandardTextType) {
        /* (the system's icon list definition isn't here) */
        RETURN((uint32_t)paramErr);
        return;
    }
    gaddr list = tb_LNew(R(0), R(1), R(2), 0, R(4), (int8_t)(R(5) & 0xFF), (int8_t)(R(6) & 0xFF),
                         (int8_t)(R(7) & 0xFF), (int8_t)(R(8) & 0xFF));
    if (pending_ldef) {
        tb_DisposeHandle(pending_ldef);
        pending_ldef = 0;
    }
    wr32(out, list);
    RETURN(list ? noErr : (uint32_t)memFullErr);
}

/* RegisterListDefinition(resID, defSpec) */
static void ls_RegisterListDefinition(ppc_cpu *c)
{
    int16_t id = (int16_t)R(0);
    gaddr spec = R(1);
    int slot = -1;
    for (int i = 0; i < 16; i++)
        if (registered_ldefs[i].def && registered_ldefs[i].id == id)
            slot = i;
    gaddr def = spec && rd32(spec) == kListDefProcPtr ? def_handle(rd32(spec + 4)) : 0;
    if (!def) {
        RETURN((uint32_t)paramErr);
        return;
    }
    for (int i = 0; i < 16 && slot < 0; i++)
        if (!registered_ldefs[i].def)
            slot = i;
    if (slot < 0) {
        tb_DisposeHandle(def);
        RETURN((uint32_t)memFullErr);
        return;
    }
    if (registered_ldefs[slot].def)
        tb_DisposeHandle(registered_ldefs[slot].def);
    registered_ldefs[slot].id = id;
    registered_ldefs[slot].def = def;
    RETURN(noErr);
}

/* ---- TextEdit ---- */

static void te_TEGetScrapHandle(ppc_cpu *c) { RETURN(tb_TEScrapHandle()); }

static void te_TESetScrapHandle(ppc_cpu *c)
{
    wr32(LM_TEScrpHandle, R(0));
    DONE();
}

/* The DoText, Recalc and FindWord hooks: TextEdit's own work is native
 * here and calls none of them, as the ROM's called them; they're kept for
 * the programs that read them back (DoText and Recalc in low memory). */
static gaddr find_word_hook;

static void te_TEGetDoTextHook(ppc_cpu *c) { RETURN(rd32(LM_TEDoText)); }
static void te_TEGetRecalcHook(ppc_cpu *c) { RETURN(rd32(LM_TERecal)); }
static void te_TEGetFindWordHook(ppc_cpu *c) { RETURN(find_word_hook); }

static void te_TESetDoTextHook(ppc_cpu *c)
{
    wr32(LM_TEDoText, R(0));
    DONE();
}

static void te_TESetRecalcHook(ppc_cpu *c)
{
    wr32(LM_TERecal, R(0));
    DONE();
}

static void te_TESetFindWordHook(ppc_cpu *c)
{
    find_word_hook = R(0);
    DONE();
}

/* ---- The Font Manager ---- */

enum { kFMIterationCompleted = -980, kFMInvalidFontFamilyErr = -981, kFMInvalidFontErr = -982,
       kFMFontTableAccessErr = -984, kFMFontContainerAccessErr = -985 };
enum { kFMFontTechnologyFilterSelector = 1, kFMFontContainerFilterSelector, kFMGenerationFilterSelector,
       kFMFontFamilyCallbackFilterSelector, kFMFontCallbackFilterSelector };
enum { kFMTrueTypeFontTechnology = 0x74727565 /* 'true' */, FORMAT_BITMAP = 0x4E464E54 /* 'NFNT' */ };
enum { T_FOND = 0x464F4E44, T_SFNT = 0x73666E74, T_FONT = 0x464F4E54, T_NFNT = 0x4E464E54 };
enum { uppFMFilterProcInfo = 0x000003F1 };

static uint32_t generation = 1;

/* The families: every FOND of the open resource files, by ID. */
static int families(int16_t *ids, int max)
{
    int n = 0;
    uint8_t saved = rd8(LM_ResLoad);
    tb_SetResLoad(0);
    gaddr id = tb_NewPtr(2, true, true), type = tb_NewPtr(4, true, true);
    int count = tb_CountResources((int32_t)T_FOND);
    for (int i = 1; i <= count && n < max; i++) {
        gaddr h = tb_GetIndResource((int32_t)T_FOND, (int16_t)i);
        if (!h)
            continue;
        tb_GetResInfo(h, id, type, 0);
        int16_t v = (int16_t)rd16(id);
        bool dup = false;
        for (int k = 0; k < n && !dup; k++)
            dup = ids[k] == v;
        if (!dup)
            ids[n++] = v;
    }
    wr8(LM_ResLoad, saved);
    tb_DisposePtr(id);
    tb_DisposePtr(type);
    /* in ID order */
    for (int i = 1; i < n; i++)
        for (int k = i; k > 0 && ids[k - 1] > ids[k]; k--) {
            int16_t t = ids[k];
            ids[k] = ids[k - 1];
            ids[k - 1] = t;
        }
    return n;
}

static gaddr fond_of(int16_t family)
{
    gaddr h = tb_GetResource((int32_t)T_FOND, family);
    return h && rd32(h) && tb_GetHandleSize(h) >= (int32_t)FamRec_SIZEOF + 2 ? h : 0;
}

/* A family's fonts: the entries of its FOND's font association table.
 * An FMFont is the family in the high word and the entry (from 1) in the
 * low. */
static int assoc_count(gaddr fond) { return (int16_t)rd16(rd32(fond) + FamRec_SIZEOF) + 1; }

static bool assoc_entry(gaddr fond, int i, int *size, int *style, int *id)
{
    if (i < 0 || i >= assoc_count(fond) || (uint32_t)tb_GetHandleSize(fond) < FamRec_SIZEOF + 2 + (uint32_t)(i + 1) * 6)
        return false;
    gaddr e = rd32(fond) + FamRec_SIZEOF + 2 + (uint32_t)i * 6;
    *size = (int16_t)rd16(e);
    *style = (int16_t)rd16(e + 2);
    *id = (int16_t)rd16(e + 4);
    return true;
}

static bool font_parts(uint32_t font, gaddr *fond, int *size, int *style, int *id)
{
    int16_t family = (int16_t)(font >> 16);
    int index = (int)(font & 0xFFFF) - 1;
    *fond = font ? fond_of(family) : 0;
    return *fond && assoc_entry(*fond, index, size, style, id);
}

static uint32_t make_font(int16_t family, int index) { return (uint32_t)(uint16_t)family << 16 | (uint32_t)(index + 1); }

/* Whether a filter lets a family (or a font of it) through. */
static bool passes(gaddr filter, int16_t family, uint32_t font, gaddr refcon)
{
    if (!filter)
        return true;
    uint32_t sel = rd32(filter + 4);
    gaddr fond = fond_of(family);
    int size = 0, style = 0, id = 0;
    switch (sel) {
    case kFMFontTechnologyFilterSelector: {
        /* families with TrueType fonts (the only technology here) */
        if (rd32(filter + 8) != kFMTrueTypeFontTechnology)
            return false;
        if (font)
            return font_parts(font, &fond, &size, &style, &id) && size == 0;
        for (int i = 0; fond && i < assoc_count(fond); i++)
            if (assoc_entry(fond, i, &size, &style, &id) && size == 0)
                return true;
        return false;
    }
    case kFMGenerationFilterSelector: return rd32(filter + 8) <= generation;
    case kFMFontFamilyCallbackFilterSelector:
        if (font)
            return true;
        {
            uint32_t args[2] = { (uint32_t)(int32_t)family, refcon };
            return mm_call(rd32(filter + 8), uppFMFilterProcInfo, args, 2) == noErr;
        }
    case kFMFontCallbackFilterSelector:
        if (!font)
            return true;
        {
            uint32_t args[2] = { font, refcon };
            return mm_call(rd32(filter + 8), uppFMFilterProcInfo, args, 2) == noErr;
        }
    default: return true;
    }
}

/* The iterators (16 words of the caller's): the kind, the position, the
 * filter (copied: format, selector and the first word of its union), its
 * refCon, and for an instance iterator the family. */
enum { IT_KIND = 0, IT_POS = 4, IT_FILTER = 8, IT_REFCON = 24, IT_FAMILY = 28 };
enum { KIND_FAMILIES = 0x666D6C79, KIND_FONTS = 0x666F6E74, KIND_INSTANCES = 0x696E7374 };

static void iterator_init(gaddr it, uint32_t kind, gaddr filter, gaddr refcon)
{
    for (uint32_t i = 0; i < 64; i += 4)
        wr32(it + i, 0);
    wr32(it + IT_KIND, kind);
    if (filter) {
        wr32(it + IT_FILTER, 1);
        gmemcpy(it + IT_FILTER + 4, filter, 12);
    }
    wr32(it + IT_REFCON, refcon);
}

static gaddr iterator_filter(gaddr it) { return rd32(it + IT_FILTER) ? it + IT_FILTER + 4 : 0; }

static void fm_CreateFontFamilyIterator(ppc_cpu *c)
{
    if (!R(3)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    iterator_init(R(3), KIND_FAMILIES, R(0), R(1));
    RETURN(noErr);
}

static void fm_CreateFontIterator(ppc_cpu *c)
{
    if (!R(3)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    iterator_init(R(3), KIND_FONTS, R(0), R(1));
    RETURN(noErr);
}

static void fm_CreateFontFamilyInstanceIterator(ppc_cpu *c)
{
    if (!R(1)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    iterator_init(R(1), KIND_INSTANCES, 0, 0);
    wr32(R(1) + IT_FAMILY, (uint32_t)(int32_t)(int16_t)R(0));
    RETURN(fond_of((int16_t)R(0)) ? noErr : (uint32_t)kFMInvalidFontFamilyErr);
}

static void fm_DisposeIterator(ppc_cpu *c)
{
    if (R(0))
        wr32(R(0) + IT_KIND, 0);
    RETURN(R(0) ? noErr : (uint32_t)paramErr);
}

/* FMGetNextFontFamily(iterator, outFamily) */
static void fm_GetNextFontFamily(ppc_cpu *c)
{
    gaddr it = R(0);
    if (!it || rd32(it + IT_KIND) != KIND_FAMILIES) {
        RETURN((uint32_t)paramErr);
        return;
    }
    int16_t ids[512];
    int n = families(ids, 512);
    for (uint32_t pos = rd32(it + IT_POS); pos < (uint32_t)n; pos++) {
        if (!passes(iterator_filter(it), ids[pos], 0, rd32(it + IT_REFCON)))
            continue;
        wr32(it + IT_POS, pos + 1);
        if (R(1))
            wr16(R(1), (uint16_t)ids[pos]);
        RETURN(noErr);
        return;
    }
    wr32(it + IT_POS, (uint32_t)n);
    RETURN((uint32_t)kFMIterationCompleted);
}

/* FMGetNextFont(iterator, outFont): every family's fonts in turn (the
 * position: the family's index in the high word, the entry in the low). */
static void fm_GetNextFont(ppc_cpu *c)
{
    gaddr it = R(0);
    if (!it || rd32(it + IT_KIND) != KIND_FONTS) {
        RETURN((uint32_t)paramErr);
        return;
    }
    int16_t ids[512];
    int n = families(ids, 512);
    uint32_t pos = rd32(it + IT_POS);
    for (int f = (int)(pos >> 16); f < n; f++, pos = (uint32_t)f << 16) {
        gaddr fond = fond_of(ids[f]);
        for (int e = (int)(pos & 0xFFFF); fond && e < assoc_count(fond); e++) {
            uint32_t font = make_font(ids[f], e);
            if (!passes(iterator_filter(it), ids[f], font, rd32(it + IT_REFCON)))
                continue;
            wr32(it + IT_POS, (uint32_t)f << 16 | (uint32_t)(e + 1));
            if (R(1))
                wr32(R(1), font);
            RETURN(noErr);
            return;
        }
    }
    wr32(it + IT_POS, (uint32_t)n << 16);
    RETURN((uint32_t)kFMIterationCompleted);
}

/* FMGetNextFontFamilyInstance(iterator, outFont, outStyle, outSize) */
static void fm_GetNextFontFamilyInstance(ppc_cpu *c)
{
    gaddr it = R(0);
    if (!it || rd32(it + IT_KIND) != KIND_INSTANCES) {
        RETURN((uint32_t)paramErr);
        return;
    }
    int16_t family = (int16_t)rd32(it + IT_FAMILY);
    gaddr fond = fond_of(family);
    uint32_t pos = rd32(it + IT_POS);
    int size, style, id;
    if (!fond || !assoc_entry(fond, (int)pos, &size, &style, &id)) {
        RETURN((uint32_t)kFMIterationCompleted);
        return;
    }
    wr32(it + IT_POS, pos + 1);
    if (R(1))
        wr32(R(1), make_font(family, (int)pos));
    if (R(2))
        wr16(R(2), (uint16_t)style);
    if (R(3))
        wr16(R(3), (uint16_t)size);
    RETURN(noErr);
}

static void fm_ResetFontFamilyIterator(ppc_cpu *c)
{
    if (!R(3)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    iterator_init(R(3), KIND_FAMILIES, R(0), R(1));
    RETURN(noErr);
}

static void fm_ResetFontIterator(ppc_cpu *c)
{
    if (!R(3)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    iterator_init(R(3), KIND_FONTS, R(0), R(1));
    RETURN(noErr);
}

static void fm_ResetFontFamilyInstanceIterator(ppc_cpu *c) { fm_CreateFontFamilyInstanceIterator(c); }

/* FMGetFontFamilyFromName(name): the family GetFNum finds, when it's the
 * one of that name (kInvalidFontFamily when there's none). */
static void fm_GetFontFamilyFromName(ppc_cpu *c)
{
    gaddr name = R(0);
    if (!name || !rd8(name)) {
        RETURN((uint32_t)(int32_t)-1);
        return;
    }
    gaddr num = tb_NewPtr(2, true, true), found = tb_NewPtr(256, true, true);
    tb_GetFNum(name, num);
    int16_t family = (int16_t)rd16(num);
    tb_GetFontName(family, found);
    char a[256], b[256];
    pstr_to_c(name, a, sizeof a);
    pstr_to_c(found, b, sizeof b);
    tb_DisposePtr(num);
    tb_DisposePtr(found);
    RETURN((uint32_t)(int32_t)(b[0] && !strcasecmp(a, b) ? family : -1));
}

static void fm_GetFontFamilyName(ppc_cpu *c)
{
    if (!R(1)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    wr8(R(1), 0);
    if (!fond_of((int16_t)R(0))) {
        RETURN((uint32_t)kFMInvalidFontFamilyErr);
        return;
    }
    tb_GetFontName((int16_t)R(0), R(1));
    RETURN(rd8(R(1)) ? noErr : (uint32_t)kFMInvalidFontFamilyErr);
}

/* FMGetFontFamilyTextEncoding(family, outEncoding): the family's script
 * (the Mac OS encodings' numbers are the scripts'). */
static void fm_GetFontFamilyTextEncoding(ppc_cpu *c)
{
    if (!fond_of((int16_t)R(0))) {
        RETURN((uint32_t)kFMInvalidFontFamilyErr);
        return;
    }
    if (R(1))
        wr32(R(1), (uint32_t)(uint16_t)tb_FontToScript((int16_t)R(0)));
    RETURN(noErr);
}

static void fm_GetFontFamilyGeneration(ppc_cpu *c)
{
    if (!fond_of((int16_t)R(0))) {
        RETURN((uint32_t)kFMInvalidFontFamilyErr);
        return;
    }
    if (R(1))
        wr32(R(1), generation);
    RETURN(noErr);
}

static void fm_GetFontGeneration(ppc_cpu *c)
{
    gaddr fond;
    int size, style, id;
    if (!font_parts(R(0), &fond, &size, &style, &id)) {
        RETURN((uint32_t)kFMInvalidFontErr);
        return;
    }
    if (R(1))
        wr32(R(1), generation);
    RETURN(noErr);
}

static void fm_GetGeneration(ppc_cpu *c) { RETURN(generation); }

/* FMGetFontFamilyInstanceFromFont(font, outFamily, outStyle) */
static void fm_GetFontFamilyInstanceFromFont(ppc_cpu *c)
{
    gaddr fond;
    int size, style, id;
    if (!font_parts(R(0), &fond, &size, &style, &id)) {
        RETURN((uint32_t)kFMInvalidFontErr);
        return;
    }
    if (R(1))
        wr16(R(1), (uint16_t)(R(0) >> 16));
    if (R(2))
        wr16(R(2), (uint16_t)style);
    RETURN(noErr);
}

/* FMGetFontFromFontFamilyInstance(family, style, outFont,
 * outIntrinsicStyle): the outline font of that style, else a bitmap one,
 * else the plain one (whose style QuickDraw then makes). */
static void fm_GetFontFromFontFamilyInstance(ppc_cpu *c)
{
    int16_t family = (int16_t)R(0);
    int want = (int16_t)R(1);
    gaddr fond = fond_of(family);
    if (!fond) {
        RETURN((uint32_t)kFMInvalidFontFamilyErr);
        return;
    }
    int best = -1, best_rank = 0;
    for (int i = 0; i < assoc_count(fond); i++) {
        int size, style, id;
        if (!assoc_entry(fond, i, &size, &style, &id))
            continue;
        int rank = style == want ? (size == 0 ? 4 : 3) : style == 0 ? (size == 0 ? 2 : 1) : 0;
        if (rank > best_rank) {
            best_rank = rank;
            best = i;
        }
    }
    if (best < 0) {
        RETURN((uint32_t)kFMInvalidFontErr);
        return;
    }
    int size, style, id;
    assoc_entry(fond, best, &size, &style, &id);
    if (R(2))
        wr32(R(2), make_font(family, best));
    if (R(3))
        wr16(R(3), (uint16_t)style);
    RETURN(noErr);
}

/* FMGetFontFormat(font, outFormat): TrueType for an outline font, a
 * bitmap strike's resource type for the others. */
static void fm_GetFontFormat(ppc_cpu *c)
{
    gaddr fond;
    int size, style, id;
    if (!font_parts(R(0), &fond, &size, &style, &id)) {
        RETURN((uint32_t)kFMInvalidFontErr);
        return;
    }
    if (R(1))
        wr32(R(1), size == 0 ? kFMTrueTypeFontTechnology : FORMAT_BITMAP);
    RETURN(noErr);
}

/* An outline font's 'sfnt' resource. */
static gaddr sfnt_of(uint32_t font, int16_t *err)
{
    gaddr fond;
    int size, style, id;
    if (!font_parts(font, &fond, &size, &style, &id)) {
        *err = kFMInvalidFontErr;
        return 0;
    }
    gaddr h = size == 0 ? tb_GetResource((int32_t)T_SFNT, (int16_t)id) : 0;
    *err = h && rd32(h) && tb_GetHandleSize(h) >= 12 ? noErr : kFMFontTableAccessErr;
    return *err ? 0 : h;
}

/* FMGetFontTableDirectory(font, length, buffer, actualLength) */
static void fm_GetFontTableDirectory(ppc_cpu *c)
{
    int16_t err;
    gaddr h = sfnt_of(R(0), &err);
    if (!h) {
        RETURN((uint32_t)(int32_t)err);
        return;
    }
    uint32_t n = 12 + 16u * rd16(rd32(h) + 4);
    if (n > (uint32_t)tb_GetHandleSize(h))
        n = (uint32_t)tb_GetHandleSize(h);
    if (R(2))
        gmemcpy(R(2), rd32(h), R(1) < n ? R(1) : n);
    if (R(3))
        wr32(R(3), n);
    RETURN(noErr);
}

/* FMGetFontTable(font, tag, offset, length, buffer, actualLength) */
static void fm_GetFontTable(ppc_cpu *c)
{
    int16_t err;
    gaddr h = sfnt_of(R(0), &err);
    if (!h) {
        RETURN((uint32_t)(int32_t)err);
        return;
    }
    gaddr p = rd32(h);
    uint32_t size = (uint32_t)tb_GetHandleSize(h), tables = rd16(p + 4);
    for (uint32_t i = 0; i < tables && 12 + 16 * (i + 1) <= size; i++) {
        gaddr e = p + 12 + 16 * i;
        if (rd32(e) != R(1))
            continue;
        uint32_t off = rd32(e + 8), len = rd32(e + 12);
        if (off > size || len > size - off) {
            RETURN((uint32_t)kFMFontTableAccessErr);
            return;
        }
        uint32_t from = R(2) < len ? R(2) : len, n = len - from;
        if (R(4) && n > R(3))
            n = R(3);
        if (R(4))
            gmemcpy(R(4), p + off + from, n);
        if (R(5))
            wr32(R(5), R(4) ? n : len);
        RETURN(noErr);
        return;
    }
    RETURN((uint32_t)kFMFontTableAccessErr);
}

/* FMGetFontContainer(font, outContainer): the file the font's family
 * came from. */
static void fm_GetFontContainer(ppc_cpu *c)
{
    gaddr fond;
    int size, style, id;
    if (!font_parts(R(0), &fond, &size, &style, &id)) {
        RETURN((uint32_t)kFMInvalidFontErr);
        return;
    }
    int16_t ref = tb_HomeResFile(fond);
    gaddr pb = tb_NewPtr(FCBPBRec_SIZEOF, true, true), name = tb_NewPtr(64, true, true);
    wr32(pb + FCBPBRec_ioNamePtr_off, name);
    wr16(pb + FCBPBRec_ioRefNum_off, (uint16_t)ref);
    int16_t err = ref > 0 ? tb_PBGetFCBInfo(pb, false) : kFMFontContainerAccessErr;
    if (!err && R(1)) {
        wr16(R(1), rd16(pb + FCBPBRec_ioFCBVRefNum_off));
        wr32(R(1) + 2, rd32(pb + FCBPBRec_ioFCBParID_off));
        gmemcpy(R(1) + 6, name, rd8(name) + 1u);
    }
    tb_DisposePtr(pb);
    tb_DisposePtr(name);
    RETURN((uint32_t)(int32_t)(err ? kFMFontContainerAccessErr : noErr));
}

/* FMActivateFonts(container, filter, refCon, options): a font file's
 * fonts made available by keeping its resource fork open (as the Font
 * Manager finds fonts in every open resource file); FMDeactivateFonts
 * closes it again. */
static struct {
    uint8_t spec[70];
    int16_t ref;
} active_files[16];

static void fm_ActivateFonts(ppc_cpu *c)
{
    gaddr spec = R(0);
    if (!spec) {
        RETURN((uint32_t)paramErr);
        return;
    }
    int slot = -1;
    for (int i = 0; i < 16 && slot < 0; i++)
        if (!active_files[i].ref)
            slot = i;
    if (slot < 0) {
        RETURN((uint32_t)memFullErr);
        return;
    }
    int16_t cur = tb_CurResFile();
    int16_t ref = tb_FSpOpenResFile(spec, 1 /* fsRdPerm */);
    int16_t err = (int16_t)rd16(LM_ResErr);
    tb_UseResFile(cur);
    if (ref <= 0) {
        RETURN((uint32_t)(int32_t)(err ? err : kFMFontContainerAccessErr));
        return;
    }
    active_files[slot].ref = ref;
    gget(active_files[slot].spec, spec, 70);
    generation++;
    RETURN(noErr);
}

static void fm_DeactivateFonts(ppc_cpu *c)
{
    gaddr spec = R(0);
    if (!spec) {
        RETURN((uint32_t)paramErr);
        return;
    }
    uint8_t want[70];
    gget(want, spec, 70);
    for (int i = 0; i < 16; i++)
        if (active_files[i].ref && !memcmp(active_files[i].spec, want, 6) &&
            !memcmp(active_files[i].spec + 6, want + 6, want[6] + 1u)) {
            tb_CloseResFile(active_files[i].ref);
            active_files[i].ref = 0;
            generation++;
            RETURN(noErr);
            return;
        }
    RETURN((uint32_t)kFMFontContainerAccessErr);
}

/* ---- Appearance ---- */

enum { smSystemScript = -1, teFlushDefault = 0 };

/* The text of a CFString in a pointer of its own, in the theme font. */
static gaddr theme_text(gaddr cf, uint16_t font, int32_t *len)
{
    gaddr p = tb_NewPtr(256, true, true);
    if (!p)
        return 0;
    hi_cf_to_pstr(cf, p);
    *len = rd8(p);
    tb_UseThemeFont(font, smSystemScript);
    return p;
}

typedef struct {
    int16_t font, face, size;
} text_state;

static text_state save_text(void)
{
    gaddr p = qd_port();
    return (text_state){ GrafPort_txFont(p), rd8(p + GrafPort_txFace_off), GrafPort_txSize(p) };
}

static void restore_text(text_state s)
{
    tb_TextFont(s.font);
    tb_TextFace((int16_t)s.face);
    tb_TextSize(s.size);
}

/* DrawThemeTextBox(string, fontID, state, wrapToWidth, box, just,
 * context): TextBox's drawing in the theme font (wrapped, or on one line
 * cut off at the box's edge). */
static void ap_DrawThemeTextBox(ppc_cpu *c)
{
    if (!R(4) || !qd_port()) {
        RETURN((uint32_t)paramErr);
        return;
    }
    text_state saved = save_text();
    int32_t len = 0;
    gaddr p = theme_text(R(0), (uint16_t)R(1), &len);
    if (!p) {
        RETURN((uint32_t)memFullErr);
        return;
    }
    if (R(3) & 0xFF) {
        tb_TETextBox(p + 1, len, R(4), (int16_t)R(5));
    } else {
        qrect box = qrect_get(R(4));
        gaddr fi = tb_NewPtr(8, true, true);
        tb_GetFontInfo(fi);
        int ascent = (int16_t)rd16(fi);
        tb_DisposePtr(fi);
        tb_TruncString((int16_t)(box.right - box.left), p, 0 /* truncEnd */);
        int w = tb_StringWidth(p);
        int16_t just = (int16_t)R(5);
        int x = just == 1 ? box.left + (box.right - box.left - w) / 2 : just == -1 ? box.right - w : box.left;
        tb_MoveTo((int16_t)x, (int16_t)(box.top + ascent));
        tb_DrawString(p);
    }
    tb_DisposePtr(p);
    restore_text(saved);
    RETURN(noErr);
}

/* GetThemeTextDimensions(string, fontID, state, wrapToWidth, ioBounds,
 * outBaseline): the text's height and width (wrapped to ioBounds' width
 * when asked), and its baseline, negative, from the bottom of a line. */
static void ap_GetThemeTextDimensions(ppc_cpu *c)
{
    if (!R(4)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    gaddr saved_port = qd_port();
    if (!saved_port) {
        RETURN((uint32_t)paramErr);
        return;
    }
    text_state saved = save_text();
    int32_t len = 0;
    gaddr p = theme_text(R(0), (uint16_t)R(1), &len);
    gaddr fi = tb_NewPtr(8, true, true);
    tb_GetFontInfo(fi);
    int ascent = (int16_t)rd16(fi), descent = (int16_t)rd16(fi + 2), leading = (int16_t)rd16(fi + 6);
    tb_DisposePtr(fi);
    int line = ascent + descent + leading, width = 0, lines = 1;
    int limit = (R(3) & 0xFF) ? pt_h(rd32(R(4))) : 0;
    if (limit > 0) {
        /* word by word, a new line when the next word would go past */
        int x = 0, start = 0;
        for (int i = 0; i <= len; i++) {
            bool end = i == len, space = !end && rd8(p + 1 + (uint32_t)i) == ' ';
            bool cr = !end && rd8(p + 1 + (uint32_t)i) == '\r';
            if (!end && !space && !cr)
                continue;
            int w = tb_TextWidth(p + 1, (int16_t)start, (int16_t)(i - start + (space ? 1 : 0)));
            if (x + w > limit && x > 0) {
                lines++;
                x = 0;
            }
            x += w;
            if (x > width)
                width = x;
            if (cr) {
                lines++;
                x = 0;
            }
            start = i + 1;
        }
        if (width > limit)
            width = limit;
    } else {
        width = tb_TextWidth(p + 1, 0, (int16_t)len);
    }
    wr32(R(4), make_pt((int16_t)(lines * line), (int16_t)width));
    if (R(5))
        wr16(R(5), (uint16_t)(int16_t)-(descent + leading));
    tb_DisposePtr(p);
    restore_text(saved);
    RETURN(noErr);
}

/* GetThemeTextShadowOutset: no theme here draws text with a shadow. */
static void ap_GetThemeTextShadowOutset(ppc_cpu *c)
{
    if (!R(2)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    qrect_put(R(2), (qrect){ 0, 0, 0, 0 });
    RETURN(noErr);
}

/* TruncateThemeText(string, fontID, state, width, truncWhere,
 * outTruncated): TruncString's work, the mutable string given the
 * result (CFStringReplaceAll). */
static void ap_TruncateThemeText(ppc_cpu *c)
{
    if (!R(0) || !qd_port()) {
        RETURN((uint32_t)paramErr);
        return;
    }
    text_state saved = save_text();
    int32_t len = 0;
    gaddr p = theme_text(R(0), (uint16_t)R(1), &len);
    int16_t r = tb_TruncString((int16_t)R(3), p, (int16_t)R(4));
    restore_text(saved);
    if (R(5))
        wr8(R(5), r > 0);
    int16_t err = noErr;
    if (r > 0) {
        gaddr s = hi_cf_from_pstr(p);
        uint32_t args[2] = { R(0), s };
        bool found = false;
        hi_call("CFStringReplaceAll", args, 2, &found);
        if (s)
            hi_cf_release(s);
        if (!found)
            err = paramErr;
    }
    tb_DisposePtr(p);
    RETURN((uint32_t)(int32_t)err);
}

/* GetTextAndEncodingFromCFString(string, outText, maxLength, outLength,
 * outEncoding): the text in Mac Roman. */
static void ap_GetTextAndEncodingFromCFString(ppc_cpu *c)
{
    if (!R(0) || !R(1)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    gaddr p = tb_NewPtr(256, true, true);
    hi_cf_to_pstr(R(0), p);
    uint32_t n = rd8(p) < R(2) ? rd8(p) : R(2);
    gmemcpy(R(1), p + 1, n);
    if (R(3))
        wr32(R(3), n);
    if (R(4))
        wr32(R(4), 0); /* kTextEncodingMacRoman */
    tb_DisposePtr(p);
    RETURN(noErr);
}

/* GetTheme(collection) / SetTheme(collection): the theme's settings by
 * tag: its name, highlight colour, scroll bar styles and fonts. */
enum { kThemeNameTag = 0x6E616D65, kThemeVariantNameTag = 0x7661726E, kThemeHighlightColorTag = 0x68636F6C,
       kThemeScrollBarArrowStyleTag = 0x73626172, kThemeScrollBarThumbStyleTag = 0x73627468,
       kThemeSoundsEnabledTag = 0x736E6473, kThemeDblClickCollapseTag = 0x636F6C6C,
       kThemeSystemFontTag = 0x6C677366, kThemeSmallSystemFontTag = 0x736D7366, kThemeViewsFontTag = 0x76666E74,
       kThemeViewsFontSizeTag = 0x7666737A };

static void ap_GetTheme(ppc_cpu *c)
{
    gaddr coll = R(0);
    if (!coll) {
        RETURN((uint32_t)paramErr);
        return;
    }
    gaddr buf = tb_NewPtr(256, true, true), size = tb_NewPtr(2, true, true), style = tb_NewPtr(2, true, true);
    pstr_from_c(buf, "Mac OS Default");
    tb_AddCollectionItem(coll, (int32_t)kThemeNameTag, 0, rd8(buf) + 1, buf);
    pstr_from_c(buf, "Platinum");
    tb_AddCollectionItem(coll, (int32_t)kThemeVariantNameTag, 0, rd8(buf) + 1, buf);
    gmemcpy(buf, LM_HiliteRGB, 6);
    tb_AddCollectionItem(coll, (int32_t)kThemeHighlightColorTag, 0, 6, buf);
    const appearance_settings *set = ap_settings();
    wr16(buf, set->arrow_style);
    tb_AddCollectionItem(coll, (int32_t)kThemeScrollBarArrowStyleTag, 0, 2, buf);
    wr16(buf, set->thumb_style);
    tb_AddCollectionItem(coll, (int32_t)kThemeScrollBarThumbStyleTag, 0, 2, buf);
    wr8(buf, set->sounds);
    tb_AddCollectionItem(coll, (int32_t)kThemeSoundsEnabledTag, 0, 1, buf);
    wr8(buf, set->dbl_click == 1);
    tb_AddCollectionItem(coll, (int32_t)kThemeDblClickCollapseTag, 0, 1, buf);
    static const struct {
        uint32_t tag;
        uint16_t font;
    } fonts[] = { { kThemeSystemFontTag, 0 }, { kThemeSmallSystemFontTag, 1 }, { kThemeViewsFontTag, 3 } };
    for (unsigned i = 0; i < 3; i++) {
        wr8(buf, 0);
        tb_GetThemeFont(fonts[i].font, smSystemScript, buf, size, style);
        tb_AddCollectionItem(coll, (int32_t)fonts[i].tag, 0, rd8(buf) + 1, buf);
        if (fonts[i].font == 3)
            tb_AddCollectionItem(coll, (int32_t)kThemeViewsFontSizeTag, 0, 2, size);
    }
    tb_DisposePtr(buf);
    tb_DisposePtr(size);
    tb_DisposePtr(style);
    RETURN(noErr);
}

/* SetTheme: the highlight colour taken, and the control panel's settings
 * (fonts, scroll bars, sounds); the appearance is the one there is. */
static void ap_SetTheme(ppc_cpu *c)
{
    gaddr coll = R(0);
    if (!coll) {
        RETURN((uint32_t)paramErr);
        return;
    }
    gaddr size = tb_NewPtr(4, true, true), rgb = tb_NewPtr(8, true, true);
    wr32(size, 6);
    if (tb_GetCollectionItem(coll, (int32_t)kThemeHighlightColorTag, 0, size, rgb) == noErr && rd32(size) == 6) {
        gmemcpy(LM_HiliteRGB, rgb, 6);
        appearance_hilite_changed(rd16(rgb), rd16(rgb + 2), rd16(rgb + 4));
    }
    tb_DisposePtr(size);
    tb_DisposePtr(rgb);
    RETURN(ap_apply_theme(coll));
}

/* IterateThemes(proc, userData): the theme files of the Appearance
 * folder; there are none here, the one theme being built in. */
static void ap_IterateThemes(ppc_cpu *c) { RETURN(noErr); }

/* ---- Help tags ---- */

enum { HM_CONTENT_SIZE = 534 };
enum { kMacHelpVersion = 3 };

typedef struct {
    gaddr object; /* control, window or menu */
    int item;     /* the menu item (0: the title, -1: not a menu) */
    uint8_t content[HM_CONTENT_SIZE];
    bool has_content;
    gaddr callback;
} help_tag;

static help_tag *tags;
static int ntags, maxtags;
static bool tags_displayed = true;
static uint32_t tag_delay = 1000; /* milliseconds */

static help_tag *tag_for(gaddr object, int item, bool create)
{
    for (int i = 0; i < ntags; i++)
        if (tags[i].object == object && tags[i].item == item)
            return &tags[i];
    if (!create)
        return NULL;
    if (ntags == maxtags) {
        int n = maxtags ? maxtags * 2 : 16;
        help_tag *more = realloc(tags, sizeof *tags * (size_t)n);
        if (!more)
            return NULL;
        tags = more;
        maxtags = n;
    }
    help_tag *t = &tags[ntags++];
    memset(t, 0, sizeof *t);
    t->object = object;
    t->item = item;
    return t;
}

static int16_t set_content(gaddr object, int item, gaddr content)
{
    if (!object)
        return paramErr;
    help_tag *t = tag_for(object, item, true);
    if (!t)
        return memFullErr;
    if (content) {
        gget(t->content, content, HM_CONTENT_SIZE);
        t->has_content = true;
    } else {
        t->has_content = false;
    }
    return noErr;
}

static int16_t get_content(gaddr object, int item, gaddr out)
{
    if (!object || !out)
        return paramErr;
    help_tag *t = tag_for(object, item, false);
    if (t && t->has_content) {
        gput(out, t->content, HM_CONTENT_SIZE);
    } else {
        /* kHMNoContent in both */
        for (uint32_t i = 0; i < HM_CONTENT_SIZE; i += 2)
            wr16(out + i, 0);
        wr32(out, kMacHelpVersion);
    }
    return noErr;
}

static void hm_SetControlHelpContent(ppc_cpu *c) { RETURN((uint32_t)(int32_t)set_content(R(0), -1, R(1))); }
static void hm_GetControlHelpContent(ppc_cpu *c) { RETURN((uint32_t)(int32_t)get_content(R(0), -1, R(1))); }
static void hm_SetWindowHelpContent(ppc_cpu *c) { RETURN((uint32_t)(int32_t)set_content(R(0), -2, R(1))); }
static void hm_GetWindowHelpContent(ppc_cpu *c) { RETURN((uint32_t)(int32_t)get_content(R(0), -2, R(1))); }

static void hm_SetMenuItemHelpContent(ppc_cpu *c)
{
    RETURN((uint32_t)(int32_t)set_content(R(0), (int)(uint16_t)R(1), R(2)));
}

static void hm_GetMenuItemHelpContent(ppc_cpu *c)
{
    RETURN((uint32_t)(int32_t)get_content(R(0), (int)(uint16_t)R(1), R(2)));
}

/* The content callbacks, by object. */
static int16_t set_callback(gaddr object, int item, gaddr upp)
{
    help_tag *t = object ? tag_for(object, item, true) : NULL;
    if (!t)
        return object ? memFullErr : paramErr;
    t->callback = upp;
    return noErr;
}

static int16_t get_callback(gaddr object, int item, gaddr out)
{
    if (!object || !out)
        return paramErr;
    help_tag *t = tag_for(object, item, false);
    wr32(out, t ? t->callback : 0);
    return noErr;
}

static void hm_InstallControlContentCallback(ppc_cpu *c) { RETURN((uint32_t)(int32_t)set_callback(R(0), -1, R(1))); }
static void hm_InstallWindowContentCallback(ppc_cpu *c) { RETURN((uint32_t)(int32_t)set_callback(R(0), -2, R(1))); }
static void hm_InstallMenuTitleContentCallback(ppc_cpu *c) { RETURN((uint32_t)(int32_t)set_callback(R(0), -3, R(1))); }
static void hm_InstallMenuItemContentCallback(ppc_cpu *c) { RETURN((uint32_t)(int32_t)set_callback(R(0), -4, R(1))); }
static void hm_GetControlContentCallback(ppc_cpu *c) { RETURN((uint32_t)(int32_t)get_callback(R(0), -1, R(1))); }
static void hm_GetWindowContentCallback(ppc_cpu *c) { RETURN((uint32_t)(int32_t)get_callback(R(0), -2, R(1))); }
static void hm_GetMenuTitleContentCallback(ppc_cpu *c) { RETURN((uint32_t)(int32_t)get_callback(R(0), -3, R(1))); }
static void hm_GetMenuItemContentCallback(ppc_cpu *c) { RETURN((uint32_t)(int32_t)get_callback(R(0), -4, R(1))); }

static void hm_HMAreHelpTagsDisplayed(ppc_cpu *c) { RETURN(tags_displayed); }

static void hm_HMSetHelpTagsDisplayed(ppc_cpu *c)
{
    tags_displayed = (R(0) & 0xFF) != 0;
    RETURN(noErr);
}

static void hm_HMGetTagDelay(ppc_cpu *c)
{
    if (!R(0)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    wr32(R(0), tag_delay);
    RETURN(noErr);
}

static void hm_HMSetTagDelay(ppc_cpu *c)
{
    tag_delay = R(0);
    RETURN(noErr);
}

/* HMGetHelpMenu(outHelpMenu, outFirstCustomItemIndex): the Help menu, and
 * where the program's own items go. */
static void hm_HMGetHelpMenu(ppc_cpu *c)
{
    gaddr mh = tb_NewPtr(4, true, true);
    int16_t err = tb_HMGetHelpMenuHandle(mh);
    gaddr m = rd32(mh);
    tb_DisposePtr(mh);
    if (err || !m) {
        RETURN((uint32_t)(int32_t)(err ? err : resNotFound));
        return;
    }
    if (R(0))
        wr32(R(0), m);
    if (R(1))
        wr16(R(1), (uint16_t)(tb_CountMItems(m) + 1));
    RETURN(noErr);
}

/* HMSetMenuHelpFromBalloonRsrc(menu, hmnuID) and
 * HMSetDialogHelpFromBalloonRsrc(dialog, hdlgID, itemStart): CarbonLib
 * makes tags of the balloons' messages; the resources must be there. */
static void hm_HMSetMenuHelpFromBalloonRsrc(ppc_cpu *c)
{
    if (!R(0)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    RETURN(tb_GetResource((int32_t)0x686D6E75 /* 'hmnu' */, (int16_t)R(1)) ? noErr : (uint32_t)resNotFound);
}

static void hm_HMSetDialogHelpFromBalloonRsrc(ppc_cpu *c)
{
    if (!R(0)) {
        RETURN((uint32_t)paramErr);
        return;
    }
    RETURN(tb_GetResource((int32_t)0x68646C67 /* 'hdlg' */, (int16_t)R(1)) ? noErr : (uint32_t)resNotFound);
}

/* ---- Together ---- */

const ppc_export carbonui_exports[] = {
    { "CreateCustomList", ls_CreateCustomList },
    { "GetListActive", ls_GetListActive },
    { "GetListCellIndent", ls_GetListCellIndent },
    { "GetListCellSize", ls_GetListCellSize },
    { "GetListClickLocation", ls_GetListClickLocation },
    { "GetListClickLoop", ls_GetListClickLoop },
    { "GetListClickTime", ls_GetListClickTime },
    { "GetListDataBounds", ls_GetListDataBounds },
    { "GetListDataHandle", ls_GetListDataHandle },
    { "GetListDefinition", ls_GetListDefinition },
    { "GetListFlags", ls_GetListFlags },
    { "GetListHorizontalScrollBar", ls_GetListHorizontalScrollBar },
    { "GetListMouseLocation", ls_GetListMouseLocation },
    { "GetListPort", ls_GetListPort },
    { "GetListRefCon", ls_GetListRefCon },
    { "GetListSelectionFlags", ls_GetListSelectionFlags },
    { "GetListUserHandle", ls_GetListUserHandle },
    { "GetListVerticalScrollBar", ls_GetListVerticalScrollBar },
    { "GetListViewBounds", ls_GetListViewBounds },
    { "GetListVisibleCells", ls_GetListVisibleCells },
    { "RegisterListDefinition", ls_RegisterListDefinition },
    { "SetListCellIndent", ls_SetListCellIndent },
    { "SetListClickLoop", ls_SetListClickLoop },
    { "SetListClickTime", ls_SetListClickTime },
    { "SetListFlags", ls_SetListFlags },
    { "SetListLastClick", ls_SetListLastClick },
    { "SetListPort", ls_SetListPort },
    { "SetListRefCon", ls_SetListRefCon },
    { "SetListSelectionFlags", ls_SetListSelectionFlags },
    { "SetListUserHandle", ls_SetListUserHandle },
    { "SetListViewBounds", ls_SetListViewBounds },
    { "TEGetDoTextHook", te_TEGetDoTextHook },
    { "TEGetFindWordHook", te_TEGetFindWordHook },
    { "TEGetRecalcHook", te_TEGetRecalcHook },
    { "TEGetScrapHandle", te_TEGetScrapHandle },
    { "TESetDoTextHook", te_TESetDoTextHook },
    { "TESetFindWordHook", te_TESetFindWordHook },
    { "TESetRecalcHook", te_TESetRecalcHook },
    { "TESetScrapHandle", te_TESetScrapHandle },
    { "FMActivateFonts", fm_ActivateFonts },
    { "FMCreateFontFamilyInstanceIterator", fm_CreateFontFamilyInstanceIterator },
    { "FMCreateFontFamilyIterator", fm_CreateFontFamilyIterator },
    { "FMCreateFontIterator", fm_CreateFontIterator },
    { "FMDeactivateFonts", fm_DeactivateFonts },
    { "FMDisposeFontFamilyInstanceIterator", fm_DisposeIterator },
    { "FMDisposeFontFamilyIterator", fm_DisposeIterator },
    { "FMDisposeFontIterator", fm_DisposeIterator },
    { "FMGetFontContainer", fm_GetFontContainer },
    { "FMGetFontFamilyFromName", fm_GetFontFamilyFromName },
    { "FMGetFontFamilyGeneration", fm_GetFontFamilyGeneration },
    { "FMGetFontFamilyInstanceFromFont", fm_GetFontFamilyInstanceFromFont },
    { "FMGetFontFamilyName", fm_GetFontFamilyName },
    { "FMGetFontFamilyTextEncoding", fm_GetFontFamilyTextEncoding },
    { "FMGetFontFormat", fm_GetFontFormat },
    { "FMGetFontFromFontFamilyInstance", fm_GetFontFromFontFamilyInstance },
    { "FMGetFontGeneration", fm_GetFontGeneration },
    { "FMGetFontTable", fm_GetFontTable },
    { "FMGetFontTableDirectory", fm_GetFontTableDirectory },
    { "FMGetGeneration", fm_GetGeneration },
    { "FMGetNextFont", fm_GetNextFont },
    { "FMGetNextFontFamily", fm_GetNextFontFamily },
    { "FMGetNextFontFamilyInstance", fm_GetNextFontFamilyInstance },
    { "FMResetFontFamilyInstanceIterator", fm_ResetFontFamilyInstanceIterator },
    { "FMResetFontFamilyIterator", fm_ResetFontFamilyIterator },
    { "FMResetFontIterator", fm_ResetFontIterator },
    { "DrawThemeTextBox", ap_DrawThemeTextBox },
    { "GetTextAndEncodingFromCFString", ap_GetTextAndEncodingFromCFString },
    { "GetTheme", ap_GetTheme },
    { "GetThemeTextDimensions", ap_GetThemeTextDimensions },
    { "GetThemeTextShadowOutset", ap_GetThemeTextShadowOutset },
    { "IterateThemes", ap_IterateThemes },
    { "SetTheme", ap_SetTheme },
    { "TruncateThemeText", ap_TruncateThemeText },
    { "HMAreHelpTagsDisplayed", hm_HMAreHelpTagsDisplayed },
    { "HMGetControlContentCallback", hm_GetControlContentCallback },
    { "HMGetControlHelpContent", hm_GetControlHelpContent },
    { "HMGetHelpMenu", hm_HMGetHelpMenu },
    { "HMGetMenuItemContentCallback", hm_GetMenuItemContentCallback },
    { "HMGetMenuItemHelpContent", hm_GetMenuItemHelpContent },
    { "HMGetMenuTitleContentCallback", hm_GetMenuTitleContentCallback },
    { "HMGetTagDelay", hm_HMGetTagDelay },
    { "HMGetWindowContentCallback", hm_GetWindowContentCallback },
    { "HMGetWindowHelpContent", hm_GetWindowHelpContent },
    { "HMInstallControlContentCallback", hm_InstallControlContentCallback },
    { "HMInstallMenuItemContentCallback", hm_InstallMenuItemContentCallback },
    { "HMInstallMenuTitleContentCallback", hm_InstallMenuTitleContentCallback },
    { "HMInstallWindowContentCallback", hm_InstallWindowContentCallback },
    { "HMSetControlHelpContent", hm_SetControlHelpContent },
    { "HMSetDialogHelpFromBalloonRsrc", hm_HMSetDialogHelpFromBalloonRsrc },
    { "HMSetHelpTagsDisplayed", hm_HMSetHelpTagsDisplayed },
    { "HMSetMenuHelpFromBalloonRsrc", hm_HMSetMenuHelpFromBalloonRsrc },
    { "HMSetMenuItemHelpContent", hm_SetMenuItemHelpContent },
    { "HMSetTagDelay", hm_HMSetTagDelay },
    { "HMSetWindowHelpContent", hm_SetWindowHelpContent },
};
const unsigned carbonui_export_count = sizeof carbonui_exports / sizeof carbonui_exports[0];

void carbonui_reset(void)
{
    pending_ldef = 0;
    memset(registered_ldefs, 0, sizeof registered_ldefs);
    find_word_hook = 0;
    generation = 1;
    memset(active_files, 0, sizeof active_files);
    free(tags);
    tags = NULL;
    ntags = maxtags = 0;
    tags_displayed = true;
    tag_delay = 1000;
}

void carbonhi_reset(void)
{
    carbonqd_reset();
    carbonwm_reset();
    carbonmenu_reset();
    carbonui_reset();
}

/* Every table of this area, for cfm.c's lookup. */
void (*carbonhi_export_fn(const char *name))(ppc_cpu *)
{
    static const struct {
        const ppc_export *table;
        const unsigned *count;
    } tables[] = {
        { carbonqd_exports, &carbonqd_export_count },     { carbonwm_exports, &carbonwm_export_count },
        { carbonmenu_exports, &carbonmenu_export_count }, { carbonui_exports, &carbonui_export_count },
        { carbonupp_exports, &carbonupp_export_count },
    };
    for (unsigned t = 0; t < sizeof tables / sizeof tables[0]; t++)
        for (unsigned i = 0; i < *tables[t].count; i++)
            if (!strcmp(tables[t].table[i].name, name))
                return tables[t].table[i].fn;
    return NULL;
}
