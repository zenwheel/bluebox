/*
 * The Unicode Utilities (UnicodeUtilities.h) and TextCommon's character
 * properties: UCKeyTranslate over a 'uchr' keyboard layout (the layout's
 * tables, dead-key states and terminators read as its format sets out),
 * collation and text breaking from the host's text services, and the
 * general category, combining class and bidirectional category of any
 * code point (unicode_props_gen.c).
 *
 * A collation key is the text folded in levels (letters, then marks, then
 * case); comparing two keys gives the order the options ask for, and
 * comparing the texts themselves asks the host, with the locale.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "carbonrest.h"
#include "carbonrest_loc.h"
#include "hostuc.h"

enum { kUCOutputBufferTooSmall = -25340, kUCTextBreakLocatorMissingType = -25341, kTextMalformedInputErr = -8739 };
enum { kUCKeyActionDown = 0, kUCKeyActionUp = 1, kUCKeyActionAutoKey = 2, kUCKeyActionDisplay = 3 };
enum { kUCKeyTranslateNoDeadKeysMask = 1 };
enum { kUCTextBreakCharMask = 1, kUCTextBreakClusterMask = 4, kUCTextBreakWordMask = 16, kUCTextBreakLineMask = 64 };
enum { kUCTextBreakLeadingEdgeMask = 1, kUCTextBreakGoBackwardsMask = 2, kUCTextBreakIterateMask = 4 };
enum { COLLATOR_TAG = 0x55430000, BREAKER_TAG = 0x55420000, MAX_OBJECTS = 32 };

/* ---- Character properties ---- */

typedef struct {
    uint32_t start;
    uint8_t value;
} uc_run;
extern const uc_run uc_general_runs[], uc_combining_runs[], uc_bidi_runs[];
extern const size_t uc_general_count, uc_combining_count, uc_bidi_count;

static uint8_t lookup_run(const uc_run *runs, size_t n, uint32_t cp)
{
    size_t lo = 0, hi = n;
    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        if (runs[mid].start <= cp)
            lo = mid;
        else
            hi = mid;
    }
    return runs[lo].value;
}

/* UCGetCharProperty(charPtr, textLength, propType, &propValue): one character (a surrogate pair is one) */
static void uc_UCGetCharProperty(ppc_cpu *c)
{
    gaddr p = A(0);
    uint32_t n = A(1), type = A(2);
    if (!p || !A(3) || n < 1 || n > 2)
        RET(rest_paramErr);
    uint32_t cp = rd16(p);
    if (cp >= 0xD800 && cp < 0xDC00) {
        if (n != 2)
            RET(kTextMalformedInputErr);
        uint32_t lo = rd16(p + 2);
        if (lo < 0xDC00 || lo >= 0xE000)
            RET(kTextMalformedInputErr);
        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
    } else if (n != 1) {
        RET(rest_paramErr);
    }
    uint8_t v;
    if (type == 1) /* kUCCharPropTypeGenlCategory */
        v = lookup_run(uc_general_runs, uc_general_count, cp);
    else if (type == 2) /* kUCCharPropTypeCombiningClass */
        v = lookup_run(uc_combining_runs, uc_combining_count, cp);
    else if (type == 3) /* kUCCharPropTypeBidiCategory */
        v = lookup_run(uc_bidi_runs, uc_bidi_count, cp);
    else
        RET(rest_paramErr);
    wr32(A(3), v);
    RET(noErr);
}

/* ---- UCKeyTranslate ---- */

/* The characters of a UCKeyCharSeq (a character, a sequence index, or none) into out. */
typedef struct {
    uint16_t u[64];
    uint32_t n;
    bool overflow;
} outbuf;

static void emit(outbuf *o, uint16_t ch)
{
    if (o->n < 64)
        o->u[o->n++] = ch;
    else
        o->overflow = true;
}

static void emit_seq_data(outbuf *o, gaddr layout, gaddr seqindex, uint32_t i)
{
    if (!seqindex)
        return;
    uint32_t count = rd16(seqindex + 2);
    if (i >= count)
        return;
    uint32_t a = rd16(seqindex + 4 + 2 * i), b = rd16(seqindex + 4 + 2 * (i + 1));
    for (uint32_t k = a; k + 2 <= b; k += 2)
        emit(o, rd16(seqindex + k));
    (void)layout;
}

static void emit_charseq(outbuf *o, gaddr layout, gaddr seqindex, uint16_t v)
{
    if ((v & 0xC000) == 0x8000)
        emit_seq_data(o, layout, seqindex, v & 0x3FFF);
    else if (v < 0xFFFE)
        emit(o, v);
}

static void emit_terminator(outbuf *o, gaddr layout, gaddr seqindex, gaddr terms, uint32_t state)
{
    if (!terms || state == 0)
        return;
    uint32_t count = rd16(terms + 2);
    if (state - 1 < count)
        emit_charseq(o, layout, seqindex, rd16(terms + 4 + 2 * (state - 1)));
}

/* UCKeyTranslate(keyLayoutPtr, virtualKeyCode, keyAction, modifierKeyState, keyboardType,
 * keyTranslateOptions, &deadKeyState, maxStringLength, &actualStringLength, unicodeString) */
static void uc_UCKeyTranslate(ppc_cpu *c)
{
    gaddr layout = A(0);
    uint32_t key = A(1) & 0xFFFF, action = A(2) & 0xFFFF, mods = A(3), ktype = A(4), opts = A(5);
    gaddr deadp = A(6), out = A(9), actualp = A(8);
    uint32_t max = A(7);
    if (!layout || !actualp)
        RET(rest_paramErr);
    wr32(actualp, 0);
    if (rd16(layout) != 0x1002)
        RET(rest_paramErr);
    if (action > kUCKeyActionDisplay)
        RET(rest_paramErr);
    if (action == kUCKeyActionUp)
        RET(noErr);
    /* The keyboard type's tables */
    uint32_t ntypes = rd32(layout + 8), chosen = ntypes;
    for (uint32_t i = 0; i < ntypes; i++)
        if (rd32(layout + 12 + 28 * i) <= ktype && ktype <= rd32(layout + 12 + 28 * i + 4)) {
            chosen = i;
            break;
        }
    if (chosen == ntypes)
        chosen = ntypes ? ntypes - 1 : 0; /* the last is the default */
    if (!ntypes)
        RET(rest_paramErr);
    gaddr h = layout + 12 + 28 * chosen;
    gaddr modtbl = rd32(h + 8) ? layout + rd32(h + 8) : 0, keytbl = rd32(h + 12) ? layout + rd32(h + 12) : 0;
    gaddr statetbl = rd32(h + 16) ? layout + rd32(h + 16) : 0, terms = rd32(h + 20) ? layout + rd32(h + 20) : 0;
    gaddr seqindex = rd32(h + 24) ? layout + rd32(h + 24) : 0;
    if (!modtbl || !keytbl)
        RET(rest_paramErr);
    uint32_t mi = mods & 0xFF, mcount = rd32(modtbl + 4);
    uint32_t tablenum = mi < mcount ? rd8(modtbl + 8 + mi) : rd16(modtbl + 2);
    if (tablenum >= rd32(keytbl + 4) || key >= rd16(keytbl + 2))
        RET(rest_paramErr);
    gaddr table = layout + rd32(keytbl + 8 + 4 * tablenum);
    uint16_t o = rd16(table + 2 * key);
    uint32_t state = deadp && !(opts & kUCKeyTranslateNoDeadKeysMask) ? rd32(deadp) : 0;
    uint32_t newstate = state;
    outbuf ob = { .n = 0 };
    bool display = action == kUCKeyActionDisplay;
    if (display)
        state = 0;
    if (o >= 0xFFFE) { /* a key with no character: nothing, and a dead state waits */
    } else if ((o & 0xC000) == 0x4000 && statetbl) { /* a dead key's state record */
        uint32_t ri = o & 0x3FFF;
        if (ri >= rd16(statetbl + 2))
            RET(rest_paramErr);
        gaddr rec = layout + rd32(statetbl + 4 + 4 * ri);
        uint16_t zero_char = rd16(rec), zero_next = rd16(rec + 2), nent = rd16(rec + 4), fmt = rd16(rec + 6);
        /* A dead key (it names a next state) says nothing when pressed, but its own
         * character when dead keys are off or it is only being shown; any other
         * key with a record (one that combines with a dead key) gives its character. */
        bool dead = zero_next != 0;
        if (state == 0) {
            if (!dead || (opts & kUCKeyTranslateNoDeadKeysMask) || display)
                emit_charseq(&ob, layout, seqindex, zero_char);
            newstate = dead && !(opts & kUCKeyTranslateNoDeadKeysMask) ? zero_next : 0;
        } else {
            bool found = false;
            gaddr e = rec + 8;
            for (uint32_t i = 0; i < nent && !found; i++) {
                if (fmt == 1) { /* terminal: curState, charData */
                    if (rd16(e) == state) {
                        emit_charseq(&ob, layout, seqindex, rd16(e + 2));
                        newstate = 0;
                        found = true;
                    }
                    e += 4;
                } else { /* range: start, range, delta multiplier, charData, nextState */
                    uint32_t start = rd16(e), range = rd8(e + 2), mult = rd8(e + 3);
                    if (state >= start && state <= start + range) {
                        uint16_t ch = rd16(e + 4);
                        if ((ch & 0xC000) != 0x8000 && ch < 0xFFFE)
                            ch = (uint16_t)(ch + (state - start) * mult);
                        emit_charseq(&ob, layout, seqindex, ch);
                        newstate = rd16(e + 6);
                        found = true;
                    }
                    e += 8;
                }
            }
            if (!found) { /* it doesn't combine: the dead key's own character, then this key as if alone */
                emit_terminator(&ob, layout, seqindex, terms, state);
                if (!dead)
                    emit_charseq(&ob, layout, seqindex, zero_char);
                newstate = dead ? zero_next : 0;
            }
        }
    } else if ((o & 0xC000) == 0x8000) {
        emit_terminator(&ob, layout, seqindex, terms, state);
        emit_seq_data(&ob, layout, seqindex, o & 0x3FFF);
        newstate = 0;
    } else if ((o & 0xC000) == 0) {
        emit_terminator(&ob, layout, seqindex, terms, state);
        if (o < 0xFFFE)
            emit(&ob, o);
        newstate = 0;
    } else {
        RET(rest_paramErr);
    }
    if (ob.overflow || ob.n > max) {
        wr32(actualp, ob.n);
        RET(kUCOutputBufferTooSmall);
    }
    for (uint32_t i = 0; out && i < ob.n; i++)
        wr16(out + 2 * i, ob.u[i]);
    wr32(actualp, ob.n);
    if (deadp && !display && !(opts & kUCKeyTranslateNoDeadKeysMask))
        wr32(deadp, newstate);
    RET(noErr);
}

/* ---- Collation ---- */

typedef struct {
    bool used;
    uint32_t options;
    char locale[32];
    bool has_locale;
} collator;
static collator collators[MAX_OBJECTS];

typedef struct {
    bool used;
    uint32_t types;
    char locale[32];
    bool has_locale;
} breaker;
static breaker breakers[MAX_OBJECTS];

void rest_uc_reset(void)
{
    memset(collators, 0, sizeof collators);
    memset(breakers, 0, sizeof breakers);
}

static collator *collator_of(uint32_t ref)
{
    uint32_t i = ref - COLLATOR_TAG;
    return i >= 1 && i <= MAX_OBJECTS && collators[i - 1].used ? &collators[i - 1] : NULL;
}

static breaker *breaker_of(uint32_t ref)
{
    uint32_t i = ref - BREAKER_TAG;
    return i >= 1 && i <= MAX_OBJECTS && breakers[i - 1].used ? &breakers[i - 1] : NULL;
}

/* UTF-16 text from guest memory (malloc'd). */
static uint16_t *get_units(gaddr p, uint32_t n)
{
    uint16_t *u = malloc(((size_t)n + 1) * 2);
    for (uint32_t i = 0; i < n; i++)
        u[i] = rd16(p + 2 * i);
    return u;
}

/* UCCreateCollator(locale, opVariant, options, &collator) */
static void uc_UCCreateCollator(ppc_cpu *c)
{
    if (!A(3))
        RET(rest_paramErr);
    wr32(A(3), 0);
    char buf[32];
    const char *id = A(0) ? rest_locale_id(A(0), buf, sizeof buf) : NULL;
    if (A(0) && !id)
        RET(rest_paramErr);
    if (!hu_available())
        RET(rest_unimpErr);
    for (int i = 0; i < MAX_OBJECTS; i++)
        if (!collators[i].used) {
            collators[i] = (collator){ true, A(2), "", id != NULL };
            if (id)
                snprintf(collators[i].locale, sizeof collators[i].locale, "%s", id);
            wr32(A(3), COLLATOR_TAG + (uint32_t)i + 1);
            RET(noErr);
        }
    RET(rest_memFullErr);
}

static void uc_UCDisposeCollator(ppc_cpu *c)
{
    if (!A(0) || !collator_of(rd32(A(0))))
        RET(rest_paramErr);
    collators[rd32(A(0)) - COLLATOR_TAG - 1].used = false;
    wr32(A(0), 0);
    RET(noErr);
}

/* UCGetCollationKey(collator, text, length, maxKeySize, &actualKeySize, key) */
static void uc_UCGetCollationKey(ppc_cpu *c)
{
    collator *k = collator_of(A(0));
    if (!k || !A(4) || (!A(1) && A(2)))
        RET(rest_paramErr);
    uint16_t *u = get_units(A(1), A(2));
    uint32_t cap = A(3);
    uint32_t *key = malloc(((size_t)cap + 1) * 4);
    size_t n = hu_collation_key(u, A(2), k->options, key, cap);
    free(u);
    wr32(A(4), (uint32_t)n);
    if (n > cap || !A(5)) {
        free(key);
        RET(A(5) ? kUCOutputBufferTooSmall : noErr);
    }
    for (size_t i = 0; i < n; i++)
        wr32(A(5) + 4 * (uint32_t)i, key[i]);
    free(key);
    RET(noErr);
}

/* UCCompareCollationKeys(key1, len1, key2, len2, &equivalent, &order) */
static void uc_UCCompareCollationKeys(ppc_cpu *c)
{
    if ((!A(0) && A(1)) || (!A(2) && A(3)))
        RET(rest_paramErr);
    uint32_t n = A(1) < A(3) ? A(1) : A(3);
    int order = 0;
    for (uint32_t i = 0; i < n && !order; i++) {
        uint32_t a = rd32(A(0) + 4 * i), b = rd32(A(2) + 4 * i);
        order = a < b ? -1 : a > b;
    }
    if (!order && A(1) != A(3))
        order = A(1) < A(3) ? -1 : 1;
    if (A(4))
        wr8(A(4), order == 0);
    if (A(5))
        wr32(A(5), (uint32_t)(int32_t)order);
    RET(noErr);
}

static int16_t compare_texts(uint32_t options, const char *locale, bool use_locale, gaddr t1, uint32_t n1, gaddr t2,
                             uint32_t n2, gaddr equivalent, gaddr order)
{
    if ((!t1 && n1) || (!t2 && n2))
        return rest_paramErr;
    if (!hu_available())
        return rest_unimpErr;
    uint16_t *a = get_units(t1, n1), *b = get_units(t2, n2);
    int o = 0;
    bool ok = hu_compare_text(a, n1, b, n2, options, locale, use_locale, &o);
    free(a);
    free(b);
    if (!ok)
        return rest_paramErr;
    if (equivalent)
        wr8(equivalent, o == 0);
    if (order)
        wr32(order, (uint32_t)(int32_t)o);
    return noErr;
}

/* UCCompareText(collator, text1, len1, text2, len2, &equivalent, &order) */
static void uc_UCCompareText(ppc_cpu *c)
{
    collator *k = collator_of(A(0));
    if (!k)
        RET(rest_paramErr);
    RET((int32_t)compare_texts(k->options, k->has_locale ? k->locale : NULL, true, A(1), A(2), A(3), A(4), A(5), A(6)));
}

/* UCCompareTextDefault(options, text1, len1, text2, len2, &equivalent, &order): the user's locale */
static void uc_UCCompareTextDefault(ppc_cpu *c)
{
    RET((int32_t)compare_texts(A(0), NULL, true, A(1), A(2), A(3), A(4), A(5), A(6)));
}

/* UCCompareTextNoLocale(options, ...): no locale's rules */
static void uc_UCCompareTextNoLocale(ppc_cpu *c)
{
    RET((int32_t)compare_texts(A(0), NULL, false, A(1), A(2), A(3), A(4), A(5), A(6)));
}

/* ---- Text breaks ---- */

/* UCCreateTextBreakLocator(locale, opVariant, breakTypes, &breakRef) */
static void uc_UCCreateTextBreakLocator(ppc_cpu *c)
{
    if (!A(3))
        RET(rest_paramErr);
    wr32(A(3), 0);
    char buf[32];
    const char *id = A(0) ? rest_locale_id(A(0), buf, sizeof buf) : NULL;
    if (A(0) && !id)
        RET(rest_paramErr);
    if (!hu_available())
        RET(rest_unimpErr);
    for (int i = 0; i < MAX_OBJECTS; i++)
        if (!breakers[i].used) {
            breakers[i] = (breaker){ true, A(2), "", id != NULL };
            if (id)
                snprintf(breakers[i].locale, sizeof breakers[i].locale, "%s", id);
            wr32(A(3), BREAKER_TAG + (uint32_t)i + 1);
            RET(noErr);
        }
    RET(rest_memFullErr);
}

static void uc_UCDisposeTextBreakLocator(ppc_cpu *c)
{
    if (!A(0) || !breaker_of(rd32(A(0))))
        RET(rest_paramErr);
    breakers[rd32(A(0)) - BREAKER_TAG - 1].used = false;
    wr32(A(0), 0);
    RET(noErr);
}

/* UCFindTextBreak(breakRef, breakType, options, text, length, startOffset, &breakOffset) */
static void uc_UCFindTextBreak(ppc_cpu *c)
{
    breaker *b = breaker_of(A(0));
    uint32_t type = A(1), opts = A(2), n = A(4), start = A(5);
    if (!b || !A(6) || (!A(3) && n) || start > n)
        RET(rest_paramErr);
    if (!(b->types & type) || (type & (type - 1)))
        RET(kUCTextBreakLocatorMissingType);
    int unit = type == kUCTextBreakCharMask ? 0 : type == kUCTextBreakClusterMask ? 1 : type == kUCTextBreakWordMask ? 2 : 3;
    uint16_t *u = get_units(A(3), n);
    size_t cap = (size_t)n * 2 + 8;
    uint32_t *offs = malloc(cap * 4);
    size_t k = hu_text_breaks(u, n, unit, b->has_locale ? b->locale : NULL, offs, cap);
    free(u);
    if (k > cap)
        k = cap;
    bool back = opts & kUCTextBreakGoBackwardsMask, iterate = opts & kUCTextBreakIterateMask,
         leading = opts & kUCTextBreakLeadingEdgeMask;
    uint32_t result = back ? 0 : n;
    if (!back) {
        for (size_t i = 0; i < k; i++) {
            bool ok = (iterate || leading) ? offs[i] > start : offs[i] >= start;
            if (ok) {
                result = offs[i];
                break;
            }
        }
    } else {
        for (size_t i = k; i-- > 0;) {
            bool ok = (iterate || !leading) ? offs[i] < start : offs[i] <= start;
            if (ok) {
                result = offs[i];
                break;
            }
        }
    }
    free(offs);
    wr32(A(6), result);
    RET(noErr);
}

const ppc_export rest_uc_exports[] = {
    { "UCCompareCollationKeys", uc_UCCompareCollationKeys },
    { "UCCompareText", uc_UCCompareText },
    { "UCCompareTextDefault", uc_UCCompareTextDefault },
    { "UCCompareTextNoLocale", uc_UCCompareTextNoLocale },
    { "UCCreateCollator", uc_UCCreateCollator },
    { "UCCreateTextBreakLocator", uc_UCCreateTextBreakLocator },
    { "UCDisposeCollator", uc_UCDisposeCollator },
    { "UCDisposeTextBreakLocator", uc_UCDisposeTextBreakLocator },
    { "UCFindTextBreak", uc_UCFindTextBreak },
    { "UCGetCharProperty", uc_UCGetCharProperty },
    { "UCGetCollationKey", uc_UCGetCollationKey },
    { "UCKeyTranslate", uc_UCKeyTranslate },
};
const unsigned rest_uc_export_count = sizeof rest_uc_exports / sizeof rest_uc_exports[0];
