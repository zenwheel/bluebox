/*
 * TextCommon, UnicodeConverter, TextEncodingConverter and ATSUnicodeLib's
 * remaining exports.
 *
 * The Unicode character properties (UCGetCharProperty: general category,
 * combining class and bidirectional category, as TextCommon.h numbers them)
 * come from the host's ICU (libicucore on macOS), so they follow its
 * Unicode version.  UCLoadCharPropsTablePriv builds the table the private
 * calls read: for the Basic Multilingual Plane, a two-stage table of ours,
 *   u32 'ucpt', u16 version (1), u16 pageCount, u16 index[256], then
 *   pageCount pages of 256 u32 entries (equal pages kept once), each
 *   entry the general category << 24 | combining class << 16 | bidi << 8.
 * Characters of the other planes are asked of ICU directly.
 */
#include <stdlib.h>
#include <string.h>

#include <unicode/uchar.h>

#include "ppc_bridge.h"
#include "tb_internal.h"
#include "os9rest.h"
#include "os9left.h"

enum {
    kTextUnsupportedEncodingErr = -8738,
    kTextMalformedInputErr = -8739,
    kTECNoConversionPathErr = -8749,
    kUCCharPropTypeGenlCategory = 1,
    kUCCharPropTypeCombiningClass = 2,
    kUCCharPropTypeBidiCategory = 3,
    TABLE_TAG = 0x75637074, /* 'ucpt' */
    TABLE_VERSION = 1,
    TAG_T2U = 0x54325563,
    TAG_U2T = 0x55325463,
};

/* ---- The properties ---- */

static uint32_t general_category(UChar32 u)
{
    static const uint8_t map[] = {
        [U_UNASSIGNED] = 0, [U_UPPERCASE_LETTER] = 14, [U_LOWERCASE_LETTER] = 15, [U_TITLECASE_LETTER] = 16,
        [U_MODIFIER_LETTER] = 17, [U_OTHER_LETTER] = 18, [U_NON_SPACING_MARK] = 5, [U_ENCLOSING_MARK] = 7,
        [U_COMBINING_SPACING_MARK] = 6, [U_DECIMAL_DIGIT_NUMBER] = 8, [U_LETTER_NUMBER] = 9,
        [U_OTHER_NUMBER] = 10, [U_SPACE_SEPARATOR] = 11, [U_LINE_SEPARATOR] = 12,
        [U_PARAGRAPH_SEPARATOR] = 13, [U_CONTROL_CHAR] = 1, [U_FORMAT_CHAR] = 2, [U_PRIVATE_USE_CHAR] = 4,
        [U_SURROGATE] = 3, [U_DASH_PUNCTUATION] = 21, [U_START_PUNCTUATION] = 22, [U_END_PUNCTUATION] = 23,
        [U_CONNECTOR_PUNCTUATION] = 20, [U_OTHER_PUNCTUATION] = 26, [U_MATH_SYMBOL] = 28,
        [U_CURRENCY_SYMBOL] = 29, [U_MODIFIER_SYMBOL] = 30, [U_OTHER_SYMBOL] = 31,
        [U_INITIAL_PUNCTUATION] = 24, [U_FINAL_PUNCTUATION] = 25,
    };
    int t = u_charType(u);
    return t >= 0 && t < (int)sizeof map ? map[t] : 0;
}

static uint32_t bidi_category(UChar32 u)
{
    static const uint8_t map[] = {
        [U_LEFT_TO_RIGHT] = 1, [U_RIGHT_TO_LEFT] = 2, [U_EUROPEAN_NUMBER] = 3,
        [U_EUROPEAN_NUMBER_SEPARATOR] = 4, [U_EUROPEAN_NUMBER_TERMINATOR] = 5, [U_ARABIC_NUMBER] = 6,
        [U_COMMON_NUMBER_SEPARATOR] = 7, [U_BLOCK_SEPARATOR] = 8, [U_SEGMENT_SEPARATOR] = 9,
        [U_WHITE_SPACE_NEUTRAL] = 10, [U_OTHER_NEUTRAL] = 11, [U_LEFT_TO_RIGHT_EMBEDDING] = 13,
        [U_LEFT_TO_RIGHT_OVERRIDE] = 15, [U_RIGHT_TO_LEFT_ARABIC] = 12, [U_RIGHT_TO_LEFT_EMBEDDING] = 14,
        [U_RIGHT_TO_LEFT_OVERRIDE] = 16, [U_POP_DIRECTIONAL_FORMAT] = 17, [U_DIR_NON_SPACING_MARK] = 18,
        [U_BOUNDARY_NEUTRAL] = 19,
    };
    if (u_charType(u) == U_UNASSIGNED)
        return 0;
    int d = u_charDirection(u);
    return d >= 0 && d < (int)sizeof map ? map[d] : 0;
}

static bool property_of(UChar32 u, int32_t type, uint32_t *value)
{
    switch (type) {
    case kUCCharPropTypeGenlCategory: *value = general_category(u); return true;
    case kUCCharPropTypeCombiningClass: *value = u_getCombiningClass(u); return true;
    case kUCCharPropTypeBidiCategory: *value = bidi_category(u); return true;
    }
    return false;
}

/* The first character of big-endian UTF-16 text: a pair of surrogates is
 * one; one alone is the surrogate. */
static UChar32 first_char(gaddr p, uint32_t n)
{
    uint32_t u = rd16(p);
    if ((u & 0xFC00) == 0xD800 && n >= 2 && (rd16(p + 2) & 0xFC00) == 0xDC00)
        return (UChar32)(0x10000 + ((u - 0xD800) << 10) + (rd16(p + 2) - 0xDC00));
    return (UChar32)u;
}

/* UCGetCharProperty(charPtr, textLength, propType, *propValue) */
static void tc_UCGetCharProperty(ppc_cpu *c)
{
    if (!R(0) || !R(1) || !R(3))
        RETURN(paramErr);
    /* A high surrogate needs its low half after it. */
    uint16_t u = rd16(R(0));
    if (u >= 0xD800 && u < 0xDC00) {
        uint16_t lo = R(1) >= 2 ? rd16(R(0) + 2) : 0;
        if (lo < 0xDC00 || lo >= 0xE000)
            RETURN(kTextMalformedInputErr);
    }
    uint32_t v;
    if (!property_of(first_char(R(0), R(1)), (int32_t)R(2), &v))
        RETURN(paramErr);
    wr32(R(3), v);
    RETURN(noErr);
}

/* GetCharacterProperties(charPtr, textLength, *properties): the three
 * properties together, as three longs (general category, combining class,
 * bidirectional category). */
static void tc_GetCharacterProperties(ppc_cpu *c)
{
    if (!R(0) || !R(1) || !R(2))
        RETURN(paramErr);
    UChar32 u = first_char(R(0), R(1));
    wr32(R(2), general_category(u));
    wr32(R(2) + 4, u_getCombiningClass(u));
    wr32(R(2) + 8, bidi_category(u));
    RETURN(noErr);
}

/* ---- The property table ---- */

enum { TBL_INDEX = 8, TBL_PAGES = TBL_INDEX + 512 };

static uint32_t entry_of(UChar32 u)
{
    return general_category(u) << 24 | (uint32_t)u_getCombiningClass(u) << 16 | bidi_category(u) << 8;
}

/* UCLoadCharPropsTablePriv(*table) */
static void tc_UCLoadCharPropsTablePriv(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    uint32_t (*pages)[256] = malloc(256 * sizeof *pages);
    if (!pages)
        RETURN(memFullErr);
    uint16_t index[256];
    unsigned n = 0;
    for (unsigned hi = 0; hi < 256; hi++) {
        uint32_t page[256];
        for (unsigned lo = 0; lo < 256; lo++)
            page[lo] = entry_of((UChar32)(hi << 8 | lo));
        unsigned k;
        for (k = 0; k < n; k++)
            if (!memcmp(pages[k], page, sizeof page))
                break;
        if (k == n)
            memcpy(pages[n++], page, sizeof page);
        index[hi] = (uint16_t)k;
    }
    gaddr t = tb_NewPtr((int32_t)(TBL_PAGES + n * 1024), false, true);
    if (!t) {
        free(pages);
        RETURN(memFullErr);
    }
    wr32(t, TABLE_TAG);
    wr16(t + 4, TABLE_VERSION);
    wr16(t + 6, (uint16_t)n);
    for (unsigned i = 0; i < 256; i++)
        wr16(t + TBL_INDEX + 2 * i, index[i]);
    for (unsigned k = 0; k < n; k++)
        for (unsigned lo = 0; lo < 256; lo++)
            wr32(t + TBL_PAGES + k * 1024 + lo * 4, pages[k][lo]);
    free(pages);
    wr32(R(0), t);
    RETURN(noErr);
}

static bool table_ok(gaddr t) { return t && rd32(t) == TABLE_TAG && rd16(t + 4) == TABLE_VERSION; }

/* UCUnloadCharPropsTablePriv(table) */
static void tc_UCUnloadCharPropsTablePriv(ppc_cpu *c)
{
    if (!table_ok(R(0)))
        RETURN(paramErr);
    wr32(R(0), 0);
    tb_DisposePtr(R(0));
    RETURN(noErr);
}

/* UCUseCharPropsTablePriv(table, char, propType): the value, from the
 * table for the BMP, from ICU beyond it; 0 for a type there isn't. */
static uint32_t table_use(gaddr t, UChar32 u, int32_t type)
{
    uint32_t v = 0;
    if (u > 0xFFFF || !table_ok(t))
        return property_of(u, type, &v) ? v : 0;
    uint32_t e = rd32(t + TBL_PAGES + (uint32_t)rd16(t + TBL_INDEX + 2 * (uint32_t)(u >> 8)) * 1024 + (u & 0xFF) * 4);
    switch (type) {
    case kUCCharPropTypeGenlCategory: return e >> 24;
    case kUCCharPropTypeCombiningClass: return e >> 16 & 0xFF;
    case kUCCharPropTypeBidiCategory: return e >> 8 & 0xFF;
    }
    return 0;
}

static void tc_UCUseCharPropsTablePriv(ppc_cpu *c) { RETURN(table_use(R(0), (UChar32)R(1), (int32_t)R(2))); }

/* UCGetCharPropsPriv(table, charPtr, textLength, propType, *propValue) */
static void tc_UCGetCharPropsPriv(ppc_cpu *c)
{
    if (!table_ok(R(0)) || !R(1) || !R(2) || !R(4) || (int32_t)R(3) < 1 || (int32_t)R(3) > 3)
        RETURN(paramErr);
    wr32(R(4), table_use(R(0), first_char(R(1), R(2)), (int32_t)R(3)));
    RETURN(noErr);
}

/* ---- Handles and tables of text resources ---- */

/* LockHandleCheck(h): locks a handle that isn't nil; the state it had. */
static void tc_LockHandleCheck(ppc_cpu *c)
{
    gaddr h = R(0);
    if (!h)
        RETURN(0);
    uint8_t state = (uint8_t)tb_HGetState(h);
    tb_HLock(h);
    RETURN(state);
}

/* UnlockHandleCheck(h, state): the state LockHandleCheck gave back. */
static void tc_UnlockHandleCheck(ppc_cpu *c)
{
    gaddr h = R(0);
    if (h)
        tb_HSetState(h, (int8_t)R(1));
    RETURN(h ? noErr : nilHandleErr);
}

/* LoadTextCommonTable(type, id, *table): a table is a text resource,
 * locked and kept until UnloadTextCommonTable. */
static void tc_LoadTextCommonTable(ppc_cpu *c)
{
    if (!R(2))
        RETURN(paramErr);
    wr32(R(2), 0);
    gaddr h = tb_GetResource((int32_t)R(0), (int16_t)R(1));
    if (!h)
        RETURN((int16_t)rd16(LM_ResErr) ? (int16_t)rd16(LM_ResErr) : resNotFound);
    tb_HLock(h);
    wr32(R(2), h);
    RETURN(noErr);
}

static void tc_UnloadTextCommonTable(ppc_cpu *c)
{
    if (!R(0))
        RETURN(nilHandleErr);
    tb_HUnlock(R(0));
    tb_ReleaseResource(R(0));
    RETURN(noErr);
}

/* TECLoadTableInternal(type, id, *table) and TECUnloadTableInternal(table)
 * are the same for the converters' mapping tables. */

/* ---- The installed scripts ---- */

static gaddr scripts_block;

/* GetInstalledScriptsPtr(): a long of the count, then a short for each
 * enabled script the Script Manager has, in script order. */
static void tc_GetInstalledScriptsPtr(ppc_cpu *c)
{
    if (!scripts_block)
        scripts_block = tb_NewPtr(4 + 2 * 32, true, true);
    if (!scripts_block)
        RETURN(0);
    uint32_t n = 0;
    for (int16_t s = 0; s < 32; s++)
        if (tb_GetScriptVariable(s, 4 /* smScriptEnabled */) != 0) {
            wr16(scripts_block + 4 + 2 * n, (uint16_t)s);
            n++;
        }
    wr32(scripts_block, n);
    RETURN(scripts_block);
}

/* ---- Globals by version (TEC's) ---- */

static gaddr tec_globals;
enum { TEC_VERSION = 0x0150 };

static void tc_TECGetGlobalsForVersion(ppc_cpu *c)
{
    if (R(0) > TEC_VERSION)
        RETURN(0);
    if (!tec_globals) {
        tec_globals = tb_NewPtr(16, true, true);
        if (tec_globals) {
            wr32(tec_globals, 0x54454347); /* 'TECG' */
            wr16(tec_globals + 4, TEC_VERSION);
        }
    }
    RETURN(tec_globals);
}

/* TECGetGlobalsEntryForVersion(version): the entry within the globals; the
 * block here has the one. */
static void tc_TECGetGlobalsEntryForVersion(ppc_cpu *c)
{
    if (R(0) > TEC_VERSION)
        RETURN(0);
    tc_TECGetGlobalsForVersion(c);
}

/* GetUnicodeTOCAbst(): the Unicode converter's table of contents, which
 * the globals' block stands for. */
static void tc_GetUnicodeTOCAbst(ppc_cpu *c)
{
    c->gpr[3] = TEC_VERSION;
    tc_TECGetGlobalsForVersion(c);
}

/* ---- UnicodeConverter ---- */

static bool o_is(gaddr o, uint32_t tag) { return o && o < 0xF0000000u && rd32(o) == tag; }

static bool info_is(gaddr o)
{
    return o && o < 0xF0000000u && (rd32(o) == TAG_T2U || rd32(o) == TAG_U2T);
}

/* LockTextToUnicodeInfo(info), LockUnicodeToTextInfo(info): the object is
 * a block that doesn't move, so locking it is the check that it is one. */
static void uc_LockTextToUnicodeInfo(ppc_cpu *c) { RETURN(o_is(R(0), TAG_T2U) ? R(0) : 0); }
static void uc_LockUnicodeToTextInfo(ppc_cpu *c) { RETURN(o_is(R(0), TAG_U2T) ? R(0) : 0); }

/* LockMappingTable(table): a table as LoadTextCommonTable gave it is a
 * locked handle. */
static void uc_LockMappingTable(ppc_cpu *c)
{
    if (!R(0))
        RETURN(0);
    tb_HLock(R(0));
    RETURN(R(0));
}

/* UnicodeValidConversionLength(info, text, length, *valid): how much of
 * the text, from the start, TruncateForTextToUnicode would convert whole. */
static void uc_UnicodeValidConversionLength(ppc_cpu *c)
{
    if (!R(3) || !info_is(R(0)))
        RETURN(paramErr);
    uint32_t args[5] = { R(0), R(2), R(1), R(2), R(3) };
    if (rd32(R(0)) == TAG_T2U)
        RETURN((int32_t)os9rest_call(c, "TruncateForTextToUnicode", args, 5));
    uint32_t a2[6] = { R(0), R(2), R(1), 0, R(2), R(3) };
    RETURN((int32_t)os9rest_call(c, "TruncateForUnicodeToText", a2, 6));
}

/* ---- TextEncodingConverter ---- */

/* TECConvertTextBuffer(converter, input, inputLength, output,
 * outputLength, *actualInputLength, *actualOutputLength): TECConvertText
 * with the output given before the lengths done. */
static void tec_TECConvertTextBuffer(ppc_cpu *c)
{
    uint32_t a[7] = { R(0), R(1), R(2), R(5), R(3), R(4), R(6) };
    RETURN(os9rest_call(c, "TECConvertText", a, 7));
}

/* TECCreateOneToManyConverterFromPath(*converter, path[], count): the
 * first encoding to each of the rest. */
static void tec_TECCreateOneToManyConverterFromPath(ppc_cpu *c)
{
    if (!R(1) || R(2) < 2)
        RETURN(paramErr);
    uint32_t a[4] = { R(0), rd32(R(1)), R(2) - 1, R(1) + 4 };
    RETURN(os9rest_call(c, "TECCreateOneToManyConverter", a, 4));
}

/* TECExamineTextEncoding(encoding, *info): noErr and the kinds of
 * conversion there is for it (to Unicode, from it), or the error for an
 * encoding there is none for. */
static void tec_TECExamineTextEncoding(ppc_cpu *c)
{
    uint32_t e = R(0);
    uint32_t unicode = 0x100;
    gaddr conv = tb_NewPtr(4, false, true);
    if (!conv)
        RETURN(memFullErr);
    uint32_t a[3] = { conv, e, unicode };
    int32_t err = (int32_t)os9rest_call(c, "TECCreateConverter", a, 3);
    if (!err) {
        uint32_t d = conv;
        os9rest_call(c, "TECDisposeConverter", &(uint32_t){ rd32(d) }, 1);
    }
    tb_DisposePtr(conv);
    if (err)
        RETURN(kTextUnsupportedEncodingErr);
    if (R(1))
        wr32(R(1), 3);
    RETURN(noErr);
}

/* ---- ATSUnicodeLib's memory settings ---- */

enum { MEM_SPEC = 0, MEM_BODY = 4, MEM_SIZE = 20, MEM_TAG = 0x41534D53 /* 'ASMS' */ };
enum { SPEC_CURRENT = 0, SPEC_APP = 1, SPEC_HEAP = 2, SPEC_CALLBACKS = 3 };
static gaddr current_setting;

/* ATSUCreateMemorySetting(callbacks, *setting): the callbacks (or the
 * heap) kept as given, in a setting that can be made the current one.  A
 * program's own heap or allocation routines are not what Blue Box's text
 * layout allocates with, so a setting is kept, not acted on. */
static void atsu_ATSUCreateMemorySetting(ppc_cpu *c)
{
    if (!R(1))
        RETURN(paramErr);
    gaddr s = tb_NewPtr(MEM_SIZE + 4, false, true);
    if (!s)
        RETURN(memFullErr);
    wr32(s, MEM_TAG);
    if (R(0))
        for (uint32_t i = 0; i < 16; i += 4)
            wr32(s + 4 + i, rd32(R(0) + i));
    wr32(R(1), s);
    RETURN(noErr);
}

static bool setting_ok(gaddr s) { return s && rd32(s) == MEM_TAG; }

static void atsu_ATSUDisposeMemorySetting(ppc_cpu *c)
{
    if (!setting_ok(R(0)))
        RETURN(paramErr);
    if (current_setting == R(0))
        RETURN(-50); /* in use: the current one can't be disposed of */
    wr32(R(0), 0);
    tb_DisposePtr(R(0));
    RETURN(noErr);
}

/* ATSUSetCurrentMemorySetting(setting) */
static void atsu_ATSUSetCurrentMemorySetting(ppc_cpu *c)
{
    if (!setting_ok(R(0)))
        RETURN(paramErr);
    current_setting = R(0);
    RETURN(noErr);
}

static void atsu_ATSUGetCurrentMemorySetting(ppc_cpu *c) { RETURN(current_setting); }

void os9left_text_reset(void)
{
    scripts_block = 0;
    tec_globals = 0;
    current_setting = 0;
}

const ppc_export os9left_text_exports[] = {
    { "ATSUCreateMemorySetting", atsu_ATSUCreateMemorySetting },
    { "ATSUDisposeMemorySetting", atsu_ATSUDisposeMemorySetting },
    { "ATSUGetCurrentMemorySetting", atsu_ATSUGetCurrentMemorySetting },
    { "ATSUSetCurrentMemorySetting", atsu_ATSUSetCurrentMemorySetting },
    { "GetCharacterProperties", tc_GetCharacterProperties },
    { "GetInstalledScriptsPtr", tc_GetInstalledScriptsPtr },
    { "GetUnicodeTOCAbst", tc_GetUnicodeTOCAbst },
    { "LoadTextCommonTable", tc_LoadTextCommonTable },
    { "LockHandleCheck", tc_LockHandleCheck },
    { "LockMappingTable", uc_LockMappingTable },
    { "LockTextToUnicodeInfo", uc_LockTextToUnicodeInfo },
    { "LockUnicodeToTextInfo", uc_LockUnicodeToTextInfo },
    { "TECConvertTextBuffer", tec_TECConvertTextBuffer },
    { "TECCreateOneToManyConverterFromPath", tec_TECCreateOneToManyConverterFromPath },
    { "TECExamineTextEncoding", tec_TECExamineTextEncoding },
    { "TECGetGlobalsEntryForVersion", tc_TECGetGlobalsEntryForVersion },
    { "TECGetGlobalsForVersion", tc_TECGetGlobalsForVersion },
    { "TECLoadTableInternal", tc_LoadTextCommonTable },
    { "TECUnloadTableInternal", tc_UnloadTextCommonTable },
    { "UCGetCharProperty", tc_UCGetCharProperty },
    { "UCGetCharPropsPriv", tc_UCGetCharPropsPriv },
    { "UCLoadCharPropsTablePriv", tc_UCLoadCharPropsTablePriv },
    { "UCUnloadCharPropsTablePriv", tc_UCUnloadCharPropsTablePriv },
    { "UCUseCharPropsTablePriv", tc_UCUseCharPropsTablePriv },
    { "UnicodeValidConversionLength", uc_UnicodeValidConversionLength },
    { "UnloadTextCommonTable", tc_UnloadTextCommonTable },
    { "UnlockHandleCheck", tc_UnlockHandleCheck },
};
const unsigned os9left_text_export_count = sizeof os9left_text_exports / sizeof os9left_text_exports[0];
