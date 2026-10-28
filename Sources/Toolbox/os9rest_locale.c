/*
 * LocalesLib (MacLocales.h): locales as the Mac OS 9 Locales Manager has
 * them.  A locale string is the ISO 639 language code, then the script,
 * region (ISO 3166) and the variants of each, joined by '_': "en_US",
 * "fr_CH".  A LocaleRef is a reference to the locale a string names, the
 * same one for the same string.  The region and language codes of the
 * Script Manager convert to locales and back by the tables here; names are
 * English (a locale asked for in another display language is named in
 * English and says so, kLocalesDefaultDisplayStatus).  There are no
 * locale operation tables (collation, date formats) installed, so no
 * operation class has locales or names.
 */
#include <stdio.h>
#include <string.h>

#include "os9rest.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(int32_t)(v); c->pc = c->lr; return; } while (0)

enum { kLocalesBufferTooSmallErr = -30001, kLocalesDefaultDisplayStatus = -30029 };
enum { kDontCare = -128, smRegionCode = 40 };
enum {
    kLocaleLanguageMask = 1, kLocaleLanguageVariantMask = 2, kLocaleScriptMask = 4, kLocaleScriptVariantMask = 8,
    kLocaleRegionMask = 16, kLocaleRegionVariantMask = 32, kLocaleAllPartsMask = 63,
};
enum { kLocaleNameMask = 1, kLocaleOperationVariantNameMask = 2 };
enum { REF_TAG = 0x4C430000u, MAX_LOCALES = 256, MAX_PART = 16 };

typedef struct {
    int16_t lang;
    const char *iso, *name;
} lang_row;

static const lang_row languages[] = {
    { 0, "en", "English" }, { 1, "fr", "French" }, { 2, "de", "German" }, { 3, "it", "Italian" },
    { 4, "nl", "Dutch" }, { 5, "sv", "Swedish" }, { 6, "es", "Spanish" }, { 7, "da", "Danish" },
    { 8, "pt", "Portuguese" }, { 9, "no", "Norwegian" }, { 10, "iw", "Hebrew" }, { 11, "ja", "Japanese" },
    { 12, "ar", "Arabic" }, { 13, "fi", "Finnish" }, { 14, "el", "Greek" }, { 15, "is", "Icelandic" },
    { 16, "mt", "Maltese" }, { 17, "tr", "Turkish" }, { 18, "hr", "Croatian" }, { 19, "zh", "Chinese" },
    { 20, "ur", "Urdu" }, { 21, "hi", "Hindi" }, { 22, "th", "Thai" }, { 23, "ko", "Korean" },
    { 24, "lt", "Lithuanian" }, { 25, "pl", "Polish" }, { 26, "hu", "Hungarian" }, { 27, "et", "Estonian" },
    { 28, "lv", "Latvian" }, { 29, "se", "Sami" }, { 30, "fo", "Faroese" }, { 31, "fa", "Persian" },
    { 32, "ru", "Russian" }, { 34, "nl", "Flemish" }, { 35, "ga", "Irish Gaelic" }, { 36, "sq", "Albanian" },
    { 37, "ro", "Romanian" }, { 38, "cs", "Czech" }, { 39, "sk", "Slovak" }, { 40, "sl", "Slovenian" },
    { 41, "yi", "Yiddish" }, { 42, "sr", "Serbian" }, { 43, "mk", "Macedonian" }, { 44, "bg", "Bulgarian" },
    { 45, "uk", "Ukrainian" }, { 46, "be", "Byelorussian" }, { 47, "uz", "Uzbek" }, { 48, "kk", "Kazakh" },
    { 49, "az", "Azerbaijani" }, { 51, "hy", "Armenian" }, { 52, "ka", "Georgian" }, { 53, "mo", "Moldavian" },
    { 54, "ky", "Kirghiz" }, { 55, "tg", "Tajik" }, { 56, "tk", "Turkmen" }, { 57, "mn", "Mongolian" },
    { 59, "ps", "Pashto" }, { 60, "ku", "Kurdish" }, { 61, "ks", "Kashmiri" }, { 62, "sd", "Sindhi" },
    { 63, "bo", "Tibetan" }, { 64, "ne", "Nepali" }, { 65, "sa", "Sanskrit" }, { 66, "mr", "Marathi" },
    { 67, "bn", "Bengali" }, { 68, "as", "Assamese" }, { 69, "gu", "Gujarati" }, { 70, "pa", "Punjabi" },
    { 71, "or", "Oriya" }, { 72, "ml", "Malayalam" }, { 73, "kn", "Kannada" }, { 74, "ta", "Tamil" },
    { 75, "te", "Telugu" }, { 76, "si", "Sinhalese" }, { 77, "my", "Burmese" }, { 78, "km", "Khmer" },
    { 79, "lo", "Lao" }, { 80, "vi", "Vietnamese" }, { 81, "id", "Indonesian" }, { 82, "tl", "Tagalog" },
    { 83, "ms", "Malay" }, { 85, "am", "Amharic" }, { 86, "ti", "Tigrinya" }, { 87, "om", "Oromo" },
    { 88, "so", "Somali" }, { 89, "sw", "Swahili" }, { 90, "rw", "Kinyarwanda" }, { 91, "rn", "Rundi" },
    { 92, "ny", "Nyanja" }, { 93, "mg", "Malagasy" }, { 94, "eo", "Esperanto" }, { 128, "cy", "Welsh" },
    { 129, "eu", "Basque" }, { 130, "ca", "Catalan" }, { 131, "la", "Latin" }, { 132, "qu", "Quechua" },
    { 133, "gn", "Guarani" }, { 134, "ay", "Aymara" }, { 135, "tt", "Tatar" }, { 136, "ug", "Uighur" },
    { 137, "dz", "Dzongkha" }, { 138, "jv", "Javanese" }, { 139, "su", "Sundanese" }, { 140, "gl", "Galician" },
    { 141, "af", "Afrikaans" }, { 142, "br", "Breton" }, { 143, "iu", "Inuktitut" }, { 144, "gd", "Scottish Gaelic" },
    { 145, "gv", "Manx Gaelic" }, { 147, "to", "Tongan" }, { 149, "kl", "Greenlandic" },
};

typedef struct {
    int16_t region;
    const char *locale, *lang_name, *region_name; /* the region name NULL where the locale has no region */
} region_row;

static const region_row regions[] = {
    { 0, "en_US", "English", "United States" }, { 1, "fr_FR", "French", "France" },
    { 2, "en_GB", "English", "United Kingdom" }, { 3, "de_DE", "German", "Germany" },
    { 4, "it_IT", "Italian", "Italy" }, { 5, "nl_NL", "Dutch", "Netherlands" },
    { 6, "nl_BE", "Flemish", "Belgium" }, { 7, "sv_SE", "Swedish", "Sweden" },
    { 8, "es_ES", "Spanish", "Spain" }, { 9, "da_DK", "Danish", "Denmark" },
    { 10, "pt_PT", "Portuguese", "Portugal" }, { 11, "fr_CA", "French", "Canada" },
    { 12, "no_NO", "Norwegian", "Norway" }, { 13, "iw_IL", "Hebrew", "Israel" },
    { 14, "ja_JP", "Japanese", "Japan" }, { 15, "en_AU", "English", "Australia" },
    { 16, "ar", "Arabic", NULL }, { 17, "fi_FI", "Finnish", "Finland" },
    { 18, "fr_CH", "French", "Switzerland" }, { 19, "de_CH", "German", "Switzerland" },
    { 20, "el_GR", "Greek", "Greece" }, { 21, "is_IS", "Icelandic", "Iceland" },
    { 22, "mt_MT", "Maltese", "Malta" }, { 24, "tr_TR", "Turkish", "Turkey" },
    { 33, "hi_IN", "Hindi", "India" }, { 34, "ur_PK", "Urdu", "Pakistan" },
    { 36, "it_CH", "Italian", "Switzerland" }, { 37, "en", "English", NULL },
    { 39, "ro_RO", "Romanian", "Romania" }, { 41, "lt_LT", "Lithuanian", "Lithuania" },
    { 42, "pl_PL", "Polish", "Poland" }, { 43, "hu_HU", "Hungarian", "Hungary" },
    { 44, "et_EE", "Estonian", "Estonia" }, { 45, "lv_LV", "Latvian", "Latvia" },
    { 46, "se", "Sami", NULL }, { 47, "fo_FO", "Faroese", "Faroe Islands" },
    { 48, "fa_IR", "Persian", "Iran" }, { 49, "ru_RU", "Russian", "Russia" },
    { 50, "ga_IE", "Irish Gaelic", "Ireland" }, { 51, "ko_KR", "Korean", "South Korea" },
    { 52, "zh_CN", "Chinese", "China" }, { 53, "zh_TW", "Chinese", "Taiwan" },
    { 54, "th_TH", "Thai", "Thailand" }, { 56, "cs_CZ", "Czech", "Czech Republic" },
    { 57, "sk_SK", "Slovak", "Slovakia" }, { 60, "bn", "Bengali", NULL },
    { 61, "be_BY", "Byelorussian", "Belarus" }, { 62, "uk_UA", "Ukrainian", "Ukraine" },
    { 65, "sr_YU", "Serbian", "Yugoslavia" }, { 66, "sl_SI", "Slovenian", "Slovenia" },
    { 67, "mk_MK", "Macedonian", "Macedonia" }, { 68, "hr_HR", "Croatian", "Croatia" },
    { 71, "pt_BR", "Portuguese", "Brazil" }, { 72, "bg_BG", "Bulgarian", "Bulgaria" },
    { 73, "ca_ES", "Catalan", "Spain" }, { 75, "gd", "Scottish Gaelic", NULL },
    { 76, "gv", "Manx Gaelic", NULL }, { 77, "br", "Breton", NULL },
    { 78, "iu_CA", "Inuktitut", "Canada" }, { 79, "cy", "Welsh", NULL },
    { 82, "en_CA", "English", "Canada" }, { 83, "dz_BT", "Dzongkha", "Bhutan" },
    { 84, "hy_AM", "Armenian", "Armenia" }, { 85, "ka_GE", "Georgian", "Georgia" },
    { 86, "es", "Spanish", NULL }, { 88, "to_TO", "Tongan", "Tonga" },
    { 91, "fr", "French", NULL }, { 92, "de_AT", "German", "Austria" },
    { 94, "gu_IN", "Gujarati", "India" }, { 95, "pa", "Punjabi", NULL },
    { 96, "ur_IN", "Urdu", "India" }, { 97, "vi_VN", "Vietnamese", "Vietnam" },
    { 98, "fr_BE", "French", "Belgium" }, { 99, "uz_UZ", "Uzbek", "Uzbekistan" },
    { 102, "af_ZA", "Afrikaans", "South Africa" }, { 103, "eo", "Esperanto", NULL },
    { 104, "mr_IN", "Marathi", "India" }, { 105, "bo", "Tibetan", NULL },
    { 106, "ne_NP", "Nepali", "Nepal" }, { 107, "kl", "Greenlandic", NULL },
};

/* The region codes that name a region by another's locale: the reformed
 * German spelling, the dotted Irish script. */
static const struct { int16_t region, same_as; } region_aliases[] = { { 70, 3 }, { 81, 50 }, { 40, 20 } };

#define NLANG ((int)(sizeof languages / sizeof languages[0]))
#define NREGION ((int)(sizeof regions / sizeof regions[0]))

/* A locale: the six parts of its string. */
typedef struct {
    bool used;
    char part[6][MAX_PART + 1]; /* language, language variant, script, script variant, region, region variant */
} locale;

static locale locales[MAX_LOCALES];

void os9rest_locale_reset(void) { memset(locales, 0, sizeof locales); }

static bool is_alpha(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'); }
static bool is_upper(char ch) { return ch >= 'A' && ch <= 'Z'; }
static bool is_lower(char ch) { return ch >= 'a' && ch <= 'z'; }

/* Parse a locale string into its parts: the language first, then a token
 * of two capital letters (or three digits) is the region, one of two or
 * four letters led by a capital and followed by lower case the script, and
 * any other token a variant of the part before it. */
static bool parse(const char *s, locale *out)
{
    memset(out, 0, sizeof *out);
    size_t n = strcspn(s, "_");
    if (n < 2 || n > 3)
        return false;
    for (size_t i = 0; i < n; i++)
        if (!is_lower(s[i]))
            return false;
    memcpy(out->part[0], s, n);
    int last = 0;
    s += n;
    while (*s == '_') {
        s++;
        n = strcspn(s, "_");
        if (!n || n > MAX_PART)
            return false;
        char tok[MAX_PART + 1];
        memcpy(tok, s, n);
        tok[n] = 0;
        s += n;
        bool region = (n == 2 && is_upper(tok[0]) && is_upper(tok[1])) ||
                      (n == 3 && tok[0] >= '0' && tok[0] <= '9' && tok[1] >= '0' && tok[1] <= '9' &&
                       tok[2] >= '0' && tok[2] <= '9');
        bool script = !region && (n == 2 || n == 4) && is_upper(tok[0]) && is_lower(tok[1]) &&
                      (n == 2 || (is_lower(tok[2]) && is_lower(tok[3])));
        int slot;
        if (region && !out->part[4][0])
            slot = 4;
        else if (script && !out->part[2][0] && last < 4)
            slot = 2;
        else if (last == 0 || last == 1)
            slot = 1;
        else if (last == 2 || last == 3)
            slot = 3;
        else
            slot = 5;
        if (out->part[slot][0] && (slot & 1))
            return false; /* a second variant of the same part */
        if (slot != 1 && slot != 3 && slot != 5 && out->part[slot][0])
            return false;
        for (const char *p = tok; *p; p++)
            if (!is_alpha(*p) && !(*p >= '0' && *p <= '9') && *p != '-')
                return false;
        memcpy(out->part[slot], tok, n + 1);
        last = slot;
    }
    return true;
}

/* The string for the parts of a mask, joined by '_'. */
static size_t join(const locale *l, uint32_t mask, char *out, size_t max)
{
    size_t len = 0;
    if (out && max)
        out[0] = 0;
    for (int i = 0; i < 6; i++)
        if ((mask & (1u << i)) && l->part[i][0]) {
            int w = snprintf(out ? out + len : NULL, out ? max - len : 0, "%s%s", len ? "_" : "", l->part[i]);
            len += (size_t)w;
        }
    return len;
}

static uint32_t intern(const locale *l)
{
    char key[96], other[96];
    join(l, kLocaleAllPartsMask, key, sizeof key);
    for (int i = 0; i < MAX_LOCALES; i++)
        if (locales[i].used) {
            join(&locales[i], kLocaleAllPartsMask, other, sizeof other);
            if (!strcmp(key, other))
                return REF_TAG | (uint32_t)(i + 1);
        }
    for (int i = 0; i < MAX_LOCALES; i++)
        if (!locales[i].used) {
            locales[i] = *l;
            locales[i].used = true;
            return REF_TAG | (uint32_t)(i + 1);
        }
    return 0;
}

static const locale *ref_of(uint32_t ref)
{
    uint32_t i = (ref & 0xFFFF) - 1;
    return (ref & 0xFFFF0000u) == REF_TAG && i < MAX_LOCALES && locales[i].used ? &locales[i] : NULL;
}

static void get_cstr(gaddr p, char *out, size_t max)
{
    size_t n = 0;
    while (n + 1 < max && rd8(p + n)) {
        out[n] = (char)rd8(p + n);
        n++;
    }
    out[n] = 0;
}

static const region_row *region_row_of(int16_t region)
{
    for (unsigned i = 0; i < sizeof region_aliases / sizeof region_aliases[0]; i++)
        if (region_aliases[i].region == region)
            region = region_aliases[i].same_as;
    for (int i = 0; i < NREGION; i++)
        if (regions[i].region == region)
            return &regions[i];
    return NULL;
}

static const lang_row *lang_by_code(int16_t lang)
{
    for (int i = 0; i < NLANG; i++)
        if (languages[i].lang == lang)
            return &languages[i];
    /* Simplified Chinese, the Persian spelling and the scripts that share a language */
    static const struct { int16_t lang, as; } same[] = { { 33, 19 }, { 31, 31 }, { 50, 49 }, { 58, 57 }, { 84, 83 },
                                                          { 146, 35 }, { 148, 14 }, { 150, 49 } };
    for (unsigned i = 0; i < sizeof same / sizeof same[0]; i++)
        if (same[i].lang == lang && same[i].as != lang)
            return lang_by_code(same[i].as);
    return NULL;
}

static const lang_row *lang_by_iso(const char *iso)
{
    if (!strcmp(iso, "he"))
        iso = "iw";
    for (int i = 0; i < NLANG; i++)
        if (!strcmp(languages[i].iso, iso))
            return &languages[i];
    return NULL;
}

/* LocaleRefFromLangOrRegionCode(lang, region, *locale): the region's
 * locale if it has one, else the language's; both left unsaid is the
 * system's own region. */
static void loc_FromLangOrRegionCode(ppc_cpu *c)
{
    int16_t lang = (int16_t)R(0), region = (int16_t)R(1);
    if (!R(2))
        RETURN(paramErr);
    if (region == kDontCare && lang == kDontCare)
        region = (int16_t)tb_GetScriptManagerVariable(smRegionCode);
    const char *s = NULL;
    const region_row *rr = region != kDontCare ? region_row_of(region) : NULL;
    if (rr)
        s = rr->locale;
    else if (lang != kDontCare && lang_by_code(lang))
        s = lang_by_code(lang)->iso;
    locale l;
    if (!s || !parse(s, &l))
        RETURN(paramErr);
    uint32_t ref = intern(&l);
    if (!ref)
        RETURN(memFullErr);
    wr32(R(2), ref);
    RETURN(noErr);
}

/* LocaleRefFromLocaleString(string, *locale) */
static void loc_FromLocaleString(ppc_cpu *c)
{
    char s[96];
    locale l;
    if (!R(0) || !R(1))
        RETURN(paramErr);
    get_cstr(R(0), s, sizeof s);
    if (!parse(s, &l))
        RETURN(paramErr);
    uint32_t ref = intern(&l);
    if (!ref)
        RETURN(memFullErr);
    wr32(R(1), ref);
    RETURN(noErr);
}

/* LocaleRefGetPartString(locale, partMask, maxLen, buffer) */
static void loc_GetPartString(ppc_cpu *c)
{
    const locale *l = ref_of(R(0));
    char s[96];
    if (!l || !R(3))
        RETURN(paramErr);
    size_t len = join(l, R(1) & kLocaleAllPartsMask, s, sizeof s);
    if (len + 1 > R(2))
        RETURN(kLocalesBufferTooSmallErr);
    for (size_t i = 0; i <= len; i++)
        wr8(R(3) + i, (uint8_t)s[i]);
    RETURN(noErr);
}

/* LocaleStringToLangAndRegionCodes(string, *lang, *region): what the
 * Script Manager calls the locale's language and region; the codes a
 * locale has no equivalent of come back as "don't care". */
static void loc_StringToLangAndRegionCodes(ppc_cpu *c)
{
    char s[96];
    locale l;
    if (!R(0) || !R(1) || !R(2))
        RETURN(paramErr);
    get_cstr(R(0), s, sizeof s);
    if (!parse(s, &l))
        RETURN(paramErr);
    char key[32];
    join(&l, kLocaleLanguageMask | kLocaleRegionMask, key, sizeof key);
    int16_t region = kDontCare, lang = kDontCare;
    for (int i = 0; i < NREGION; i++)
        if (!strcmp(regions[i].locale, key) || (!strcmp(key, "he_IL") && !strcmp(regions[i].locale, "iw_IL"))) {
            region = regions[i].region;
            break;
        }
    const lang_row *lr = lang_by_iso(l.part[0]);
    if (lr)
        lang = lr->lang;
    if (!strcmp(key, "zh_CN"))
        lang = 33;
    wr16(R(1), (uint16_t)lang);
    wr16(R(2), (uint16_t)region);
    RETURN(noErr);
}

/* ---- Names ---- */

/* The English name of a locale: its language, and its region in
 * parentheses; a language this table doesn't know is its code. */
static size_t locale_name(const locale *l, char *out, size_t max)
{
    char key[32];
    join(l, kLocaleLanguageMask | kLocaleRegionMask, key, sizeof key);
    const char *lang = NULL, *region = NULL;
    for (int i = 0; i < NREGION; i++)
        if (!strcmp(regions[i].locale, key)) {
            lang = regions[i].lang_name;
            region = regions[i].region_name;
        }
    if (!lang) {
        const lang_row *lr = lang_by_iso(l->part[0]);
        lang = lr ? lr->name : l->part[0];
        region = l->part[4][0] ? l->part[4] : NULL;
    }
    int w = region ? snprintf(out, max, "%s (%s)", lang, region) : snprintf(out, max, "%s", lang);
    return (size_t)w;
}

static bool is_english(uint32_t display)
{
    const locale *d = ref_of(display);
    return !display || (d && !strcmp(d->part[0], "en"));
}

static void put_unichars(gaddr to, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        wr16(to + 2 * (uint32_t)i, (uint8_t)s[i]);
}

/* The name of a locale into the program's buffer; the status the name
 * came with. */
static int32_t give_name(const locale *l, uint32_t name_mask, uint32_t display, uint32_t max, gaddr actual,
                         gaddr buf)
{
    char name[128];
    size_t len = 0;
    if (name_mask & kLocaleNameMask)
        len = locale_name(l, name, sizeof name);
    else
        name[0] = 0; /* no operation has a variant of this locale */
    if (actual)
        wr32(actual, (uint32_t)len);
    if (len > max) {
        if (buf)
            put_unichars(buf, name, max);
        return kLocalesBufferTooSmallErr;
    }
    if (buf)
        put_unichars(buf, name, len);
    return is_english(display) ? noErr : kLocalesDefaultDisplayStatus;
}

/* LocaleGetName(locale, opVariant, nameMask, displayLocale, maxLen,
 * *actualLen, name) */
static void loc_GetName(ppc_cpu *c)
{
    const locale *l = ref_of(R(0));
    if (!l || !(R(2) & 3) || (R(3) && !ref_of(R(3))))
        RETURN(paramErr);
    RETURN(give_name(l, R(2), R(3), R(4), R(5), R(6)));
}

/* LocaleCountNames(locale, opVariant, nameMask, *count): the one name
 * there is, in English. */
static void loc_CountNames(ppc_cpu *c)
{
    if (!ref_of(R(0)) || !(R(2) & 3) || !R(3))
        RETURN(paramErr);
    wr32(R(3), 1);
    RETURN(noErr);
}

/* LocaleGetIndName(locale, opVariant, nameMask, index, maxLen, *actualLen,
 * name, *displayLocale): the names count from 1. */
static void loc_GetIndName(ppc_cpu *c)
{
    const locale *l = ref_of(R(0));
    if (!l || !(R(2) & 3) || R(3) != 1)
        RETURN(paramErr);
    int32_t err = give_name(l, R(2), 0, R(4), R(5), R(6));
    if (R(7)) {
        locale en;
        parse("en_US", &en);
        wr32(R(7), intern(&en));
    }
    RETURN(err);
}

/* LocaleGetRegionLanguageName(region, name): the Pascal name of the
 * language a region speaks. */
static void loc_GetRegionLanguageName(ppc_cpu *c)
{
    int16_t region = (int16_t)R(0);
    if (region < 0 || !R(1))
        RETURN(paramErr);
    const region_row *rr = region_row_of(region);
    if (!rr) {
        wr8(R(1), 0);
        RETURN(resNotFound);
    }
    size_t n = strlen(rr->lang_name);
    wr8(R(1), (uint8_t)n);
    for (size_t i = 0; i < n; i++)
        wr8(R(1) + 1 + (uint32_t)i, (uint8_t)rr->lang_name[i]);
    RETURN(noErr);
}

/* ---- Operations ---- */

/* No locale operation tables are installed: a class has no locales and no
 * names, and nothing can be loaded or unloaded. */
static void loc_OpCountLocales(ppc_cpu *c)
{
    if (!R(1))
        RETURN(paramErr);
    wr32(R(1), 0);
    RETURN(noErr);
}

static void loc_OpGetLocales(ppc_cpu *c)
{
    if (!R(2))
        RETURN(paramErr);
    wr32(R(2), 0);
    RETURN(noErr);
}

static void loc_OpCountNames(ppc_cpu *c)
{
    if (!R(1))
        RETURN(paramErr);
    wr32(R(1), 0);
    RETURN(noErr);
}

static void loc_OpNoName(ppc_cpu *c) { RETURN(paramErr); }

/* LocaleOperationLoadData(.., opClass, .., *index, *data): nothing to load. */
static void loc_OpLoadData(ppc_cpu *c)
{
    if (R(3))
        wr32(R(3), 0xFFFFFFFFu);
    if (R(4))
        wr32(R(4), 0);
    RETURN(paramErr);
}

static void loc_OpUnloadData(ppc_cpu *c) { RETURN(paramErr); }

const ppc_export os9rest_locale_exports[] = {
    { "LocaleRefFromLangOrRegionCode", loc_FromLangOrRegionCode },
    { "LocaleRefFromLocaleString", loc_FromLocaleString }, { "LocaleRefGetPartString", loc_GetPartString },
    { "LocaleStringToLangAndRegionCodes", loc_StringToLangAndRegionCodes },
    { "LocaleOperationCountLocales", loc_OpCountLocales }, { "LocaleOperationGetLocales", loc_OpGetLocales },
    { "LocaleGetName", loc_GetName }, { "LocaleCountNames", loc_CountNames }, { "LocaleGetIndName", loc_GetIndName },
    { "LocaleGetRegionLanguageName", loc_GetRegionLanguageName },
    { "LocaleOperationGetName", loc_OpNoName }, { "LocaleOperationCountNames", loc_OpCountNames },
    { "LocaleOperationGetIndName", loc_OpNoName },
    { "LocaleOperationLoadData", loc_OpLoadData }, { "LocaleOperationUnloadData", loc_OpUnloadData },
};
const unsigned os9rest_locale_export_count = sizeof os9rest_locale_exports / sizeof os9rest_locale_exports[0];
