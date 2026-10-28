/*
 * The host's Unicode and locale services (hostuc.h) from CoreFoundation, on
 * its own because CoreFoundation's headers and the Toolbox's both define
 * noErr.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "hostuc.h"

#if defined(__APPLE__) && !defined(HOSTTEXT_ICONV)
#include <CoreFoundation/CoreFoundation.h>

bool hu_available(void) { return true; }

static CFLocaleRef make_locale(const char *id)
{
    if (!id)
        return CFLocaleCopyCurrent();
    CFStringRef s = CFStringCreateWithCString(NULL, id, kCFStringEncodingUTF8);
    if (!s)
        return NULL;
    CFLocaleRef l = CFLocaleCreate(NULL, s);
    CFRelease(s);
    return l;
}

bool hu_canonical_locale(const char *in, char *out, size_t cap)
{
    CFStringRef s = CFStringCreateWithCString(NULL, in, kCFStringEncodingUTF8);
    if (!s)
        return false;
    CFStringRef c = CFLocaleCreateCanonicalLocaleIdentifierFromString(NULL, s);
    CFRelease(s);
    if (!c)
        return false;
    bool ok = CFStringGetCString(c, out, (CFIndex)cap, kCFStringEncodingUTF8) && out[0];
    CFRelease(c);
    return ok;
}

bool hu_current_locale(char *out, size_t cap)
{
    CFLocaleRef l = CFLocaleCopyCurrent();
    CFStringRef id = CFLocaleGetIdentifier(l);
    bool ok = id && CFStringGetCString(id, out, (CFIndex)cap, kCFStringEncodingUTF8);
    CFRelease(l);
    return ok;
}

static bool copy_units(CFStringRef s, uint16_t *out, size_t cap, size_t *n)
{
    CFIndex len = CFStringGetLength(s);
    *n = (size_t)len;
    if ((size_t)len > cap)
        return false;
    CFStringGetCharacters(s, CFRangeMake(0, len), out);
    return true;
}

bool hu_locale_display_name(const char *locale, const char *display, uint16_t *out, size_t cap, size_t *n)
{
    CFLocaleRef d = make_locale(display);
    CFStringRef id = CFStringCreateWithCString(NULL, locale, kCFStringEncodingUTF8);
    if (!d || !id) {
        if (d)
            CFRelease(d);
        if (id)
            CFRelease(id);
        return false;
    }
    CFStringRef name = CFLocaleCopyDisplayNameForPropertyValue(d, kCFLocaleIdentifier, id);
    CFRelease(d);
    CFRelease(id);
    if (!name)
        return false;
    bool ok = copy_units(name, out, cap, n);
    CFRelease(name);
    return ok;
}

bool hu_language_name(const char *lang, uint16_t *out, size_t cap, size_t *n)
{
    CFLocaleRef d = CFLocaleCreate(NULL, CFSTR("en"));
    CFStringRef id = CFStringCreateWithCString(NULL, lang, kCFStringEncodingUTF8);
    if (!d || !id) {
        if (d)
            CFRelease(d);
        if (id)
            CFRelease(id);
        return false;
    }
    CFStringRef name = CFLocaleCopyDisplayNameForPropertyValue(d, kCFLocaleLanguageCode, id);
    CFRelease(d);
    CFRelease(id);
    if (!name)
        return false;
    bool ok = copy_units(name, out, cap, n);
    CFRelease(name);
    return ok;
}

size_t hu_available_locales(char (*out)[32], size_t max)
{
    CFArrayRef a = CFLocaleCopyAvailableLocaleIdentifiers();
    size_t n = 0;
    for (CFIndex i = 0; a && i < CFArrayGetCount(a) && n < max; i++) {
        char id[64];
        if (CFStringGetCString(CFArrayGetValueAtIndex(a, i), id, sizeof id, kCFStringEncodingUTF8) && strlen(id) < 32)
            strcpy(out[n++], id);
    }
    if (a)
        CFRelease(a);
    return n;
}

static CFStringRef make_string(const uint16_t *t, size_t n)
{
    return CFStringCreateWithCharacters(NULL, t, (CFIndex)n);
}

bool hu_compare_text(const uint16_t *a, size_t an, const uint16_t *b, size_t bn, uint32_t options,
                     const char *locale, bool use_locale, int *order)
{
    CFStringRef sa = make_string(a, an), sb = make_string(b, bn);
    CFLocaleRef loc = use_locale ? make_locale(locale) : NULL;
    if (!sa || !sb || (use_locale && !loc)) {
        if (sa)
            CFRelease(sa);
        if (sb)
            CFRelease(sb);
        if (loc)
            CFRelease(loc);
        return false;
    }
    CFOptionFlags f = 0;
    if (options & (1u << 3))
        f |= kCFCompareCaseInsensitive;
    if (options & (1u << 4))
        f |= kCFCompareDiacriticInsensitive;
    if (options & (1u << 2))
        f |= kCFCompareWidthInsensitive;
    if (options & (1u << 1))
        f |= kCFCompareNonliteral;
    if (options & (1u << 17))
        f |= kCFCompareNumerically;
    CFComparisonResult r = CFStringCompareWithOptionsAndLocale(sa, sb, CFRangeMake(0, (CFIndex)an), f, loc);
    CFRelease(sa);
    CFRelease(sb);
    if (loc)
        CFRelease(loc);
    *order = r < 0 ? -1 : r > 0 ? 1 : 0;
    return true;
}

/* The text as code points, decomposed (and folded as asked). */
static CFMutableStringRef folded(const uint16_t *t, size_t n, bool fold_case, bool strip_marks, bool fold_width)
{
    CFMutableStringRef s = CFStringCreateMutable(NULL, 0);
    CFStringAppendCharacters(s, t, (CFIndex)n);
    CFStringNormalize(s, kCFStringNormalizationFormD);
    if (fold_case || strip_marks || fold_width) {
        CFOptionFlags f = (fold_case ? kCFCompareCaseInsensitive : 0) | (strip_marks ? kCFCompareDiacriticInsensitive : 0) |
                          (fold_width ? kCFCompareWidthInsensitive : 0);
        CFStringFold(s, f, NULL);
        if (strip_marks) {
            CFStringNormalize(s, kCFStringNormalizationFormD);
            CFMutableStringRef out = CFStringCreateMutable(NULL, 0);
            CFCharacterSetRef marks = CFCharacterSetGetPredefined(kCFCharacterSetNonBase);
            for (CFIndex i = 0; i < CFStringGetLength(s); i++) {
                UniChar c = CFStringGetCharacterAtIndex(s, i);
                if (!CFCharacterSetIsCharacterMember(marks, c))
                    CFStringAppendCharacters(out, &c, 1);
            }
            CFRelease(s);
            s = out;
        }
    }
    return s;
}

static size_t put_cp(uint32_t *out, size_t max, size_t at, uint32_t v)
{
    if (at < max)
        out[at] = v;
    return at + 1;
}

size_t hu_collation_key(const uint16_t *t, size_t n, uint32_t options, uint32_t *out, size_t max)
{
    bool case_i = options & (1u << 3), dia_i = options & (1u << 4), width_i = options & (1u << 2);
    bool digits = options & (1u << 17);
    size_t at = 0;
    /* Primary: the letters alone, in one case, marks and width folded. */
    CFMutableStringRef p = folded(t, n, true, true, true);
    CFIndex len = CFStringGetLength(p);
    for (CFIndex i = 0; i < len; i++) {
        UniChar c = CFStringGetCharacterAtIndex(p, i);
        if (digits && c >= '0' && c <= '9') {
            CFIndex j = i;
            while (j < len && CFStringGetCharacterAtIndex(p, j) >= '0' && CFStringGetCharacterAtIndex(p, j) <= '9')
                j++;
            CFIndex k = i;
            while (k < j - 1 && CFStringGetCharacterAtIndex(p, k) == '0')
                k++;
            at = put_cp(out, max, at, 0x30000);
            at = put_cp(out, max, at, (uint32_t)(j - k));
            for (; k < j; k++)
                at = put_cp(out, max, at, (uint32_t)CFStringGetCharacterAtIndex(p, k) + 1);
            i = j - 1;
            continue;
        }
        at = put_cp(out, max, at, (uint32_t)c + 1);
    }
    CFRelease(p);
    at = put_cp(out, max, at, 0);
    /* Secondary: the marks, as the text decomposes (case folded). */
    if (!dia_i) {
        CFMutableStringRef s = folded(t, n, true, false, width_i);
        for (CFIndex i = 0; i < CFStringGetLength(s); i++)
            at = put_cp(out, max, at, (uint32_t)CFStringGetCharacterAtIndex(s, i) + 1);
        CFRelease(s);
        at = put_cp(out, max, at, 0);
    }
    /* Tertiary: case, lower before upper. */
    if (!case_i) {
        CFMutableStringRef s = folded(t, n, false, false, width_i);
        CFCharacterSetRef upper = CFCharacterSetGetPredefined(kCFCharacterSetUppercaseLetter);
        for (CFIndex i = 0; i < CFStringGetLength(s); i++)
            at = put_cp(out, max, at, CFCharacterSetIsCharacterMember(upper, CFStringGetCharacterAtIndex(s, i)) ? 2 : 1);
        CFRelease(s);
        at = put_cp(out, max, at, 0);
    }
    return at;
}

size_t hu_text_breaks(const uint16_t *t, size_t n, int unit, const char *locale, uint32_t *offs, size_t max)
{
    size_t k = 0;
    CFStringRef s = make_string(t, n);
    if (!s)
        return 0;
    if (unit <= 1) {
        for (CFIndex i = 0; i < (CFIndex)n;) {
            if (k < max)
                offs[k] = (uint32_t)i;
            k++;
            CFRange r = unit == 0 ? CFRangeMake(i, 1) : CFStringGetRangeOfComposedCharactersAtIndex(s, i);
            if (unit == 0 && CFStringGetCharacterAtIndex(s, i) >= 0xD800 && CFStringGetCharacterAtIndex(s, i) < 0xDC00 &&
                i + 1 < (CFIndex)n)
                r.length = 2;
            i += r.length > 0 ? r.length : 1;
        }
    } else {
        CFLocaleRef loc = locale ? make_locale(locale) : NULL;
        CFStringTokenizerRef tok = CFStringTokenizerCreate(
            NULL, s, CFRangeMake(0, (CFIndex)n), unit == 2 ? kCFStringTokenizerUnitWordBoundary : kCFStringTokenizerUnitLineBreak, loc);
        CFIndex end = 0;
        if (n && tok) {
            while (CFStringTokenizerAdvanceToNextToken(tok) != kCFStringTokenizerTokenNone) {
                CFRange r = CFStringTokenizerGetCurrentTokenRange(tok);
                if (r.location > end) { /* a gap the tokenizer skipped is a piece of its own */
                    if (k < max)
                        offs[k] = (uint32_t)end;
                    k++;
                }
                if (k < max)
                    offs[k] = (uint32_t)r.location;
                k++;
                end = r.location + r.length;
            }
        }
        if (tok)
            CFRelease(tok);
        if (loc)
            CFRelease(loc);
        if (n && end < (CFIndex)n) {
            if (k < max)
                offs[k] = (uint32_t)end;
            k++;
        }
        /* collapse repeats */
        size_t m = 0;
        for (size_t i = 0; i < k && i < max; i++)
            if (m == 0 || offs[m - 1] != offs[i])
                offs[m++] = offs[i];
        k = k < max ? m : k;
    }
    if (k < max)
        offs[k] = (uint32_t)n;
    k++;
    CFRelease(s);
    return k;
}

size_t hu_tokens(const uint16_t *t, size_t n, const char *locale, uint32_t *start, uint32_t *len, size_t max)
{
    CFStringRef s = make_string(t, n);
    if (!s)
        return 0;
    CFLocaleRef loc = locale ? make_locale(locale) : NULL;
    CFStringTokenizerRef tok =
        CFStringTokenizerCreate(NULL, s, CFRangeMake(0, (CFIndex)n), kCFStringTokenizerUnitWordBoundary, loc);
    size_t k = 0;
    CFIndex end = 0;
    while (n && tok && CFStringTokenizerAdvanceToNextToken(tok) != kCFStringTokenizerTokenNone) {
        CFRange r = CFStringTokenizerGetCurrentTokenRange(tok);
        if (r.location > end && k < max) { /* what lies between words: one piece */
            start[k] = (uint32_t)end;
            len[k] = (uint32_t)(r.location - end);
            k++;
        }
        if (k < max) {
            start[k] = (uint32_t)r.location;
            len[k] = (uint32_t)r.length;
            k++;
        }
        end = r.location + r.length;
    }
    if (n && end < (CFIndex)n && k < max) {
        start[k] = (uint32_t)end;
        len[k] = (uint32_t)((CFIndex)n - end);
        k++;
    }
    if (tok)
        CFRelease(tok);
    if (loc)
        CFRelease(loc);
    CFRelease(s);
    return k;
}

#else /* no host service */

bool hu_available(void) { return false; }
bool hu_canonical_locale(const char *in, char *out, size_t cap) { return false; }
bool hu_current_locale(char *out, size_t cap) { return false; }
bool hu_locale_display_name(const char *l, const char *d, uint16_t *o, size_t c, size_t *n) { return false; }
bool hu_language_name(const char *l, uint16_t *o, size_t c, size_t *n) { return false; }
size_t hu_available_locales(char (*out)[32], size_t max) { return 0; }
bool hu_compare_text(const uint16_t *a, size_t an, const uint16_t *b, size_t bn, uint32_t o, const char *l, bool u,
                     int *order)
{
    return false;
}
size_t hu_collation_key(const uint16_t *t, size_t n, uint32_t o, uint32_t *out, size_t max) { return 0; }
size_t hu_text_breaks(const uint16_t *t, size_t n, int unit, const char *l, uint32_t *offs, size_t max) { return 0; }
size_t hu_tokens(const uint16_t *t, size_t n, const char *l, uint32_t *s, uint32_t *len, size_t max) { return 0; }

#endif
