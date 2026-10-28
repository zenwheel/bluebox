/*
 * The Locale Manager (MacLocales.h): locales named by ISO strings
 * ("en_US"), tied to the Script Manager's language and region codes, with
 * names and the locales that collation and text breaking serve from the
 * host's own locale data.  Names come in English; asked for in another
 * display locale a call says kLocalesDefaultDisplayStatus and gives the
 * English.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "carbonrest.h"
#include "hostuc.h"
#include "carbonrest_loc.h"

enum {
    kLocalesBufferTooSmallErr = -30001,
    kLocalesTableFormatErr = -30002,
    kLocalesDefaultDisplayStatus = -30029,
};
enum { kLocaleLanguageMask = 1, kLocaleLanguageVariantMask = 2, kLocaleScriptMask = 4, kLocaleScriptVariantMask = 8,
       kLocaleRegionMask = 16, kLocaleRegionVariantMask = 32, kLocaleAllPartsMask = 0x3F };
enum { LOCALE_TAG = 0x4C430000, MAX_LOCALES = 128 };
#define T(s) fourcc(s)

/* Script Manager language codes and the ISO 639 languages they are. */
static const struct {
    int16_t code;
    const char *iso;
} languages[] = {
    { 0, "en" }, { 1, "fr" }, { 2, "de" }, { 3, "it" }, { 4, "nl" }, { 5, "sv" }, { 6, "es" }, { 7, "da" },
    { 8, "pt" }, { 9, "nb" }, { 10, "he" }, { 11, "ja" }, { 12, "ar" }, { 13, "fi" }, { 14, "el" }, { 15, "is" },
    { 16, "mt" }, { 17, "tr" }, { 18, "hr" }, { 19, "zh_TW" }, { 20, "ur" }, { 21, "hi" }, { 22, "th" }, { 23, "ko" },
    { 24, "lt" }, { 25, "pl" }, { 26, "hu" }, { 27, "et" }, { 28, "lv" }, { 29, "se" }, { 30, "fo" }, { 31, "fa" },
    { 32, "ru" }, { 33, "zh_CN" }, { 34, "nl_BE" }, { 35, "ga" }, { 36, "sq" }, { 37, "ro" }, { 38, "cs" },
    { 39, "sk" }, { 40, "sl" }, { 41, "yi" }, { 42, "sr" }, { 43, "mk" }, { 44, "bg" }, { 45, "uk" }, { 46, "be" },
    { 47, "uz" }, { 48, "kk" }, { 49, "az" }, { 50, "az" }, { 51, "hy" }, { 52, "ka" }, { 53, "ro" }, { 54, "ky" },
    { 55, "tg" }, { 56, "tk" }, { 57, "mn" }, { 58, "mn" }, { 59, "ps" }, { 60, "ku" }, { 61, "ks" }, { 62, "sd" },
    { 63, "bo" }, { 64, "ne" }, { 65, "sa" }, { 66, "mr" }, { 67, "bn" }, { 68, "as" }, { 69, "gu" }, { 70, "pa" },
    { 71, "or" }, { 72, "ml" }, { 73, "kn" }, { 74, "ta" }, { 75, "te" }, { 76, "si" }, { 77, "my" }, { 78, "km" },
    { 79, "lo" }, { 80, "vi" }, { 81, "id" }, { 82, "tl" }, { 83, "ms" }, { 84, "ms" }, { 85, "am" }, { 86, "ti" },
    { 87, "om" }, { 88, "so" }, { 89, "sw" }, { 90, "rw" }, { 91, "rn" }, { 92, "ny" }, { 93, "mg" }, { 94, "eo" },
    { 128, "cy" }, { 129, "eu" }, { 130, "ca" }, { 131, "la" }, { 132, "qu" }, { 133, "gn" }, { 134, "ay" },
    { 135, "tt" }, { 136, "ug" }, { 137, "dz" }, { 138, "jv" }, { 139, "su" }, { 140, "gl" }, { 141, "af" },
    { 142, "br" }, { 143, "iu" }, { 144, "gd" }, { 145, "gv" }, { 146, "ga" }, { 147, "to" }, { 148, "el" },
    { 149, "kl" }, { 150, "az" },
};

/* Region codes (verUS ...), their ISO 3166 region and the language they imply. */
static const struct {
    int16_t code;
    const char *region, *lang;
} regions[] = {
    { 0, "US", "en" }, { 1, "FR", "fr" }, { 2, "GB", "en" }, { 3, "DE", "de" }, { 4, "IT", "it" }, { 5, "NL", "nl" },
    { 6, "BE", "nl" }, { 7, "SE", "sv" }, { 8, "ES", "es" }, { 9, "DK", "da" }, { 10, "PT", "pt" },
    { 11, "CA", "fr" }, { 12, "NO", "nb" }, { 13, "IL", "he" }, { 14, "JP", "ja" }, { 15, "AU", "en" },
    { 16, "EG", "ar" }, { 17, "FI", "fi" }, { 18, "CH", "fr" }, { 19, "CH", "de" }, { 20, "GR", "el" },
    { 21, "IS", "is" }, { 22, "MT", "mt" }, { 23, "CY", "el" }, { 24, "TR", "tr" }, { 25, "HR", "hr" },
    { 26, "NL", "nl" }, { 27, "BE", "nl" }, { 28, "CA", "en" }, { 29, "CA", "en" }, { 30, "PT", "pt" },
    { 31, "NO", "nb" }, { 32, "DK", "da" }, { 33, "IN", "hi" }, { 34, "PK", "ur" }, { 35, "TR", "tr" },
    { 36, "CH", "it" }, { 39, "RO", "ro" }, { 40, "GR", "el" }, { 41, "LT", "lt" }, { 42, "PL", "pl" },
    { 43, "HU", "hu" }, { 44, "EE", "et" }, { 45, "LV", "lv" }, { 46, "NO", "se" }, { 47, "FO", "fo" },
    { 48, "IR", "fa" }, { 49, "RU", "ru" }, { 50, "IE", "en" }, { 51, "KR", "ko" }, { 52, "CN", "zh" },
    { 53, "TW", "zh" }, { 54, "TH", "th" }, { 56, "CZ", "cs" }, { 57, "SK", "sk" }, { 59, "HU", "hu" },
    { 60, "BD", "bn" }, { 61, "BY", "be" }, { 62, "UA", "uk" }, { 64, "GR", "el" }, { 65, "RS", "sr" },
    { 66, "SI", "sl" }, { 67, "MK", "mk" }, { 68, "HR", "hr" }, { 70, "DE", "de" }, { 71, "BR", "pt" },
    { 72, "BG", "bg" }, { 73, "ES", "ca" }, { 75, "GB", "gd" }, { 76, "IM", "gv" }, { 77, "FR", "br" },
    { 78, "CA", "iu" }, { 79, "GB", "cy" }, { 81, "IE", "ga" }, { 82, "CA", "en" }, { 83, "BT", "dz" },
    { 84, "AM", "hy" }, { 85, "GE", "ka" }, { 86, "MX", "es" }, { 88, "TO", "to" }, { 91, "FR", "fr" },
    { 92, "AT", "de" }, { 94, "IN", "gu" }, { 95, "IN", "pa" }, { 96, "IN", "ur" }, { 97, "VN", "vi" },
    { 98, "BE", "fr" }, { 99, "UZ", "uz" }, { 100, "SG", "en" }, { 101, "NO", "nn" }, { 102, "ZA", "af" },
    { 104, "IN", "mr" }, { 105, "CN", "bo" }, { 106, "NP", "ne" }, { 107, "GL", "kl" },
};

static const char *language_iso(int16_t code)
{
    for (unsigned i = 0; i < sizeof languages / sizeof *languages; i++)
        if (languages[i].code == code)
            return languages[i].iso;
    return NULL;
}

static int16_t language_code(const char *iso)
{
    for (unsigned i = 0; i < sizeof languages / sizeof *languages; i++)
        if (!strcmp(languages[i].iso, iso))
            return languages[i].code;
    return -1;
}

/* ---- Locale refs: a table of canonical identifiers ---- */

static char locales[MAX_LOCALES][32];
static int nlocales;

void rest_loc_reset(void) { nlocales = 0; }

static uint32_t locale_ref(const char *id)
{
    for (int i = 0; i < nlocales; i++)
        if (!strcmp(locales[i], id))
            return LOCALE_TAG + (uint32_t)i + 1;
    if (nlocales == MAX_LOCALES || strlen(id) >= 32)
        return 0;
    strcpy(locales[nlocales], id);
    return LOCALE_TAG + (uint32_t)++nlocales;
}

/* The identifier a ref names, or the user's own for none. */
const char *rest_locale_id(uint32_t ref, char *buf, size_t cap)
{
    uint32_t i = ref - LOCALE_TAG;
    if (i >= 1 && i <= (uint32_t)nlocales)
        return locales[i - 1];
    if (!ref && hu_current_locale(buf, cap))
        return buf;
    return NULL;
}

/* A locale string's parts: language, script (4 letters), region (2 letters or 3 digits). */
static void split_locale(const char *id, char lang[8], char script[8], char region[8])
{
    lang[0] = script[0] = region[0] = 0;
    char copy[64];
    snprintf(copy, sizeof copy, "%s", id);
    int part = 0;
    for (char *tok = strtok(copy, "_-"); tok; tok = strtok(NULL, "_-"), part++) {
        size_t n = strlen(tok);
        if (part == 0)
            snprintf(lang, 8, "%.7s", tok);
        else if (n == 4 && !script[0])
            snprintf(script, 8, "%.7s", tok);
        else if ((n == 2 || n == 3) && !region[0])
            snprintf(region, 8, "%.7s", tok);
    }
}

/* LocaleRefFromLangOrRegionCode(lang, region, &locale) */
static void loc_LocaleRefFromLangOrRegionCode(ppc_cpu *c)
{
    int16_t lang = S16(0), region = S16(1);
    if (!A(2))
        RET(rest_paramErr);
    wr32(A(2), 0);
    const char *li = lang == -1 ? NULL : language_iso(lang), *ri = NULL, *rl = NULL;
    if (lang != -1 && !li)
        RET(rest_paramErr);
    if (region != -1) {
        for (unsigned i = 0; i < sizeof regions / sizeof *regions; i++)
            if (regions[i].code == region) {
                ri = regions[i].region;
                rl = regions[i].lang;
            }
        if (!ri)
            RET(rest_paramErr);
    }
    char id[32];
    if (li && strchr(li, '_')) { /* Chinese: the script's form */
        snprintf(id, sizeof id, "%s", li);
    } else if (li || rl) {
        const char *l = li ? li : rl;
        if (ri)
            snprintf(id, sizeof id, "%s_%s", l, ri);
        else
            snprintf(id, sizeof id, "%s", l);
    } else {
        if (!hu_current_locale(id, sizeof id))
            RET(rest_paramErr);
    }
    char canon[32];
    if (hu_available() && hu_canonical_locale(id, canon, sizeof canon))
        snprintf(id, sizeof id, "%s", canon);
    uint32_t ref = locale_ref(id);
    if (!ref)
        RET(rest_memFullErr);
    wr32(A(2), ref);
    RET(noErr);
}

/* LocaleRefFromLocaleString(localeString, &locale) */
static void loc_LocaleRefFromLocaleString(ppc_cpu *c)
{
    char s[64], canon[32];
    if (!A(1))
        RET(rest_paramErr);
    wr32(A(1), 0);
    rest_cstr(A(0), s, sizeof s);
    if (!s[0])
        RET(rest_paramErr);
    if (!hu_available()) {
        snprintf(canon, sizeof canon, "%s", s);
    } else if (!hu_canonical_locale(s, canon, sizeof canon)) {
        RET(rest_paramErr);
    }
    uint32_t ref = locale_ref(canon);
    if (!ref)
        RET(rest_memFullErr);
    wr32(A(1), ref);
    RET(noErr);
}

/* LocaleRefGetPartString(locale, partMask, maxStringLen, partString) */
static void loc_LocaleRefGetPartString(ppc_cpu *c)
{
    char buf[32];
    const char *id = rest_locale_id(A(0), buf, sizeof buf);
    uint32_t mask = A(1), max = A(2);
    if (!id || !A(3) || (mask & ~(uint32_t)kLocaleAllPartsMask))
        RET(rest_paramErr);
    char lang[8], script[8], region[8], out[32] = "";
    split_locale(id, lang, script, region);
    const char *parts[6] = { lang, "", script, "", region, "" };
    for (int i = 0; i < 6; i++)
        if ((mask & (1u << i)) && parts[i][0]) {
            if (out[0])
                strcat(out, "_");
            strcat(out, parts[i]);
        }
    if (strlen(out) + 1 > max)
        RET(kLocalesBufferTooSmallErr);
    for (size_t i = 0; i <= strlen(out); i++)
        wr8(A(3) + (uint32_t)i, (uint8_t)out[i]);
    RET(noErr);
}

/* LocaleStringToLangAndRegionCodes(localeString, &lang, &region) */
static void loc_LocaleStringToLangAndRegionCodes(ppc_cpu *c)
{
    char s[64], lang[8], script[8], region[8];
    if (!A(0) || !A(1) || !A(2))
        RET(rest_paramErr);
    rest_cstr(A(0), s, sizeof s);
    if (!s[0])
        RET(rest_paramErr);
    split_locale(s, lang, script, region);
    int16_t l = -1, r = -1;
    char zh[16];
    if (!strcmp(lang, "zh")) { /* Chinese by script or region */
        bool trad = !strcmp(script, "Hant") || !strcmp(region, "TW") || !strcmp(region, "HK") || !strcmp(region, "MO");
        snprintf(zh, sizeof zh, "zh_%s", trad ? "TW" : "CN");
        l = language_code(zh);
    } else {
        l = language_code(lang);
    }
    if (region[0]) {
        for (unsigned i = 0; i < sizeof regions / sizeof *regions && r < 0; i++)
            if (!strcmp(regions[i].region, region) && !strcmp(regions[i].lang, lang))
                r = regions[i].code;
        for (unsigned i = 0; i < sizeof regions / sizeof *regions && r < 0; i++)
            if (!strcmp(regions[i].region, region))
                r = regions[i].code;
    }
    wr16(A(1), (uint16_t)l);
    wr16(A(2), (uint16_t)r);
    RET(noErr);
}

/* ---- Operations: collation and text breaks ---- */

static bool op_known(uint32_t op) { return op == T("ucol") || op == T("ubrk"); }

static const char *op_name(uint32_t op) { return op == T("ucol") ? "Collation" : op == T("ubrk") ? "Text Breaking" : NULL; }

static int16_t put_name(const uint16_t *u, size_t n, gaddr out, uint32_t max, gaddr actual)
{
    if (actual)
        wr32(actual, (uint32_t)n);
    if (n > max)
        return kLocalesBufferTooSmallErr;
    for (size_t i = 0; out && i < n; i++)
        wr16(out + 2 * (uint32_t)i, u[i]);
    return noErr;
}

static int16_t put_ascii_name(const char *s, gaddr out, uint32_t max, gaddr actual)
{
    uint16_t u[64];
    size_t n = strlen(s) < 64 ? strlen(s) : 63;
    for (size_t i = 0; i < n; i++)
        u[i] = (uint8_t)s[i];
    return put_name(u, n, out, max, actual);
}

/* Whether a display locale is English (or the default): the language we name in. */
static bool english(uint32_t ref)
{
    char buf[32], lang[8], script[8], region[8];
    const char *id = rest_locale_id(ref, buf, sizeof buf);
    if (!ref || !id)
        return true;
    split_locale(id, lang, script, region);
    return !strcmp(lang, "en");
}

/* LocaleOperationCountLocales(opClass, &count) */
static void loc_LocaleOperationCountLocales(ppc_cpu *c)
{
    if (!A(1))
        RET(rest_paramErr);
    char (*ids)[32] = malloc(512 * 32);
    size_t n = op_known(A(0)) ? hu_available_locales(ids, 512) : 0;
    free(ids);
    wr32(A(1), (uint32_t)n);
    RET(noErr);
}

/* LocaleOperationGetLocales(opClass, maxLocaleCount, &actualLocaleCount, localeVariantList) */
static void loc_LocaleOperationGetLocales(ppc_cpu *c)
{
    if (!A(2))
        RET(rest_paramErr);
    char (*ids)[32] = malloc(512 * 32);
    size_t n = op_known(A(0)) ? hu_available_locales(ids, 512) : 0;
    uint32_t k = 0;
    for (size_t i = 0; A(3) && i < n && k < A(1); i++) {
        uint32_t ref = locale_ref(ids[i]);
        if (!ref)
            continue;
        wr32(A(3) + 8 * k, ref);
        wr32(A(3) + 8 * k + 4, 0);
        k++;
    }
    free(ids);
    wr32(A(2), A(3) ? k : (uint32_t)n);
    RET(noErr);
}

/* LocaleGetName(locale, opVariant, nameMask, displayLocale, maxNameLen, &actual, displayName) */
static void loc_LocaleGetName(ppc_cpu *c)
{
    char buf[32], dbuf[32];
    const char *id = rest_locale_id(A(0), buf, sizeof buf);
    if (!id || (A(2) & ~3u))
        RET(rest_paramErr);
    const char *disp = A(3) ? rest_locale_id(A(3), dbuf, sizeof dbuf) : NULL;
    uint16_t u[128];
    size_t n = 0;
    bool defaulted = A(3) && !english(A(3));
    if (!hu_available())
        RET(rest_unimpErr);
    if (!(A(2) & 1)) { /* only the variant's name: none for variant 0 */
        n = 0;
    } else if (!hu_locale_display_name(id, defaulted ? "en" : disp, u, 128, &n)) {
        RET(rest_paramErr);
    }
    int16_t e = put_name(u, n, A(6), A(4), A(5));
    RET(e ? e : defaulted ? kLocalesDefaultDisplayStatus : noErr);
}

/* LocaleCountNames(locale, opVariant, nameMask, &count) */
static void loc_LocaleCountNames(ppc_cpu *c)
{
    char buf[32];
    if (!rest_locale_id(A(0), buf, sizeof buf) || !A(3) || (A(2) & ~3u))
        RET(rest_paramErr);
    wr32(A(3), 1);
    RET(noErr);
}

/* LocaleGetIndName(locale, opVariant, nameMask, nameIndex, maxNameLen, &actual, displayName, &displayLocale) */
static void loc_LocaleGetIndName(ppc_cpu *c)
{
    char buf[32];
    const char *id = rest_locale_id(A(0), buf, sizeof buf);
    if (!id || (A(2) & ~3u) || A(3) != 1)
        RET(rest_paramErr);
    uint16_t u[128];
    size_t n = 0;
    if (!hu_available())
        RET(rest_unimpErr);
    if ((A(2) & 1) && !hu_locale_display_name(id, "en", u, 128, &n))
        RET(rest_paramErr);
    if (A(7))
        wr32(A(7), locale_ref("en"));
    RET(put_name(u, n, A(6), A(4), A(5)));
}

/* LocaleGetRegionLanguageName(region, languageName): the Pascal name of the region's language */
static void loc_LocaleGetRegionLanguageName(ppc_cpu *c)
{
    int16_t region = S16(0);
    if (!A(1))
        RET(rest_paramErr);
    const char *lang = NULL;
    for (unsigned i = 0; i < sizeof regions / sizeof *regions; i++)
        if (regions[i].code == region)
            lang = regions[i].lang;
    if (!lang || !hu_available())
        RET(lang ? rest_unimpErr : rest_paramErr);
    uint16_t u[64];
    size_t n;
    if (!hu_language_name(lang, u, 64, &n))
        RET(rest_paramErr);
    uint8_t mac[256];
    n = svc_utf16_to_macroman(u, n, mac);
    wr8(A(1), (uint8_t)n);
    for (size_t i = 0; i < n; i++)
        wr8(A(1) + 1 + (uint32_t)i, mac[i]);
    RET(noErr);
}

/* LocaleOperationGetName(opClass, displayLocale, maxNameLen, &actual, displayName) */
static void loc_LocaleOperationGetName(ppc_cpu *c)
{
    const char *name = op_name(A(0));
    if (!name)
        RET(rest_paramErr);
    int16_t e = put_ascii_name(name, A(4), A(2), A(3));
    RET(e ? e : A(1) && !english(A(1)) ? kLocalesDefaultDisplayStatus : noErr);
}

static void loc_LocaleOperationCountNames(ppc_cpu *c)
{
    if (!op_name(A(0)) || !A(1))
        RET(rest_paramErr);
    wr32(A(1), 1);
    RET(noErr);
}

/* LocaleOperationGetIndName(opClass, nameIndex, maxNameLen, &actual, displayName, &displayLocale) */
static void loc_LocaleOperationGetIndName(ppc_cpu *c)
{
    const char *name = op_name(A(0));
    if (!name || A(1) != 1)
        RET(rest_paramErr);
    if (A(5))
        wr32(A(5), locale_ref("en"));
    RET(put_ascii_name(name, A(4), A(2), A(3)));
}

const ppc_export rest_loc_exports[] = {
    { "LocaleCountNames", loc_LocaleCountNames },
    { "LocaleGetIndName", loc_LocaleGetIndName },
    { "LocaleGetName", loc_LocaleGetName },
    { "LocaleGetRegionLanguageName", loc_LocaleGetRegionLanguageName },
    { "LocaleOperationCountLocales", loc_LocaleOperationCountLocales },
    { "LocaleOperationCountNames", loc_LocaleOperationCountNames },
    { "LocaleOperationGetIndName", loc_LocaleOperationGetIndName },
    { "LocaleOperationGetLocales", loc_LocaleOperationGetLocales },
    { "LocaleOperationGetName", loc_LocaleOperationGetName },
    { "LocaleRefFromLangOrRegionCode", loc_LocaleRefFromLangOrRegionCode },
    { "LocaleRefFromLocaleString", loc_LocaleRefFromLocaleString },
    { "LocaleRefGetPartString", loc_LocaleRefGetPartString },
    { "LocaleStringToLangAndRegionCodes", loc_LocaleStringToLangAndRegionCodes },
};
const unsigned rest_loc_export_count = sizeof rest_loc_exports / sizeof rest_loc_exports[0];
