/*
 * Core Foundation's strings (CFString.h) and character sets
 * (CFCharacterSet.h), as CarbonLib had them.
 *
 * A string's characters are UTF-16 on the host; what a program may read
 * in place (CFStringGetCStringPtr, ...GetPascalStringPtr,
 * ...GetCharactersPtr) is the bytes it gave a NoCopy call, or a copy made
 * in guest memory the first time it asks, for strings that can't change.
 * A mutable string on a program's own buffer
 * (CFStringCreateMutableWithExternalCharactersNoCopy) writes its changes
 * back there while they fit.
 *
 * Encodings are the Text Encoding Converter's (tec.c), whose numbers
 * CFStringEncoding shares: Mac OS Roman and the rest of the Mac scripts,
 * ISO, Windows and DOS code pages, the East Asian encodings and Unicode's
 * forms.  Unicode (0x0100) here is UTF-16 in the PowerPC's byte order.
 *
 * Case mapping and the predefined character sets come from tables of
 * the scripts a Mac OS 9 system shipped with (Latin, Greek, Cyrillic,
 * Armenian, Hebrew, Arabic, Indic, Thai, the CJK blocks), the same on
 * every host.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "carbon_cf.h"

/* tec.c */
enum { HT_OK, HT_FULL, HT_INVALID, HT_INCOMPLETE, HT_UNSUPPORTED };
bool tec_encoding_available(uint32_t e);
int tec_decode(uint32_t e, const uint8_t *in, size_t n, size_t *used, uint16_t *out, size_t cap, size_t *produced);
int tec_encode(uint32_t e, const uint16_t *in, size_t n, size_t *used, uint8_t *out, size_t cap, size_t *produced);
const char *tec_encoding_name(uint32_t e);
const char *tec_internet_name(uint32_t e);
bool tec_encoding_of_internet_name(const char *name, uint32_t *e);
unsigned tec_available_encodings(uint32_t *out, unsigned max);

enum { kCFStringEncodingNextStepLatin = 0x0B01 };
enum { kCFCompareCaseInsensitive = 1, kCFCompareBackwards = 4, kCFCompareAnchored = 8, kCFCompareNonliteral = 16,
       kCFCompareLocalized = 32, kCFCompareNumerically = 64 };

typedef struct {
    cf_obj o;
    uint16_t *u;
    uint32_t n, cap;
    bool mut, wide;
    uint32_t enc8;             /* the 8-bit encoding it was made from */
    gaddr src8, src8_free;     /* a NoCopy C or Pascal string, and who frees it */
    bool src8_pascal, src8_owned;
    gaddr src16, src16_free;   /* NoCopy characters */
    bool src16_owned;
    gaddr ext, ext_alloc;      /* a mutable string's external buffer */
    uint32_t ext_cap;
    gaddr cptr, pptr, uptr;    /* our copies for the Ptr calls */
    uint32_t cptr_enc, pptr_enc;
} cf_string;

static void cfcharset_reset(void);

/* ---------------------------------------------------------------------- */
/* Encodings */

static bool is_unicode16(uint32_t enc) { return enc == kCFStringEncodingUnicode; }

/* Mac OS Roman's upper half, made once from the converter. */
static uint16_t roman_hi[128];
static bool roman_ready;

static void roman_table(void)
{
    if (roman_ready)
        return;
    for (int i = 0; i < 128; i++) {
        uint8_t b = (uint8_t)(128 + i);
        size_t used = 0, made = 0;
        uint16_t u[4] = { 0 };
        tec_decode(kCFStringEncodingMacRoman, &b, 1, &used, u, 4, &made);
        roman_hi[i] = made ? u[0] : 0xFFFD;
    }
    roman_ready = true;
}

static int roman_byte(uint16_t u)
{
    if (u < 128)
        return u;
    roman_table();
    for (int i = 0; i < 128; i++)
        if (roman_hi[i] == u)
            return 128 + i;
    return -1;
}

bool cf_decode(uint32_t enc, const uint8_t *b, size_t n, uint16_t **out, uint32_t *nout)
{
    uint16_t *u = malloc(sizeof *u * (n * 2 + 1));
    if (!u)
        return false;
    size_t k = 0;
    if (enc == kCFStringEncodingMacRoman) {
        roman_table();
        for (size_t i = 0; i < n; i++)
            u[k++] = b[i] < 128 ? b[i] : roman_hi[b[i] - 128];
    } else if (enc == kCFStringEncodingASCII || enc == kCFStringEncodingISOLatin1) {
        for (size_t i = 0; i < n; i++) {
            if (enc == kCFStringEncodingASCII && b[i] > 127)
                goto fail;
            u[k++] = b[i];
        }
    } else if (is_unicode16(enc)) {
        for (size_t i = 0; i + 1 < n; i += 2)
            u[k++] = (uint16_t)(b[i] << 8 | b[i + 1]);
    } else if (enc == kCFStringEncodingNonLossyASCII) {
        /* ASCII, with \uXXXX (and octal \ooo) for the rest. */
        for (size_t i = 0; i < n; i++) {
            if (b[i] > 127)
                goto fail;
            if (b[i] == '\\' && i + 5 < n && b[i + 1] == 'u') {
                char hex[5] = { (char)b[i + 2], (char)b[i + 3], (char)b[i + 4], (char)b[i + 5], 0 };
                u[k++] = (uint16_t)strtoul(hex, NULL, 16);
                i += 5;
            } else if (b[i] == '\\' && i + 3 < n && b[i + 1] >= '0' && b[i + 1] <= '3') {
                u[k++] = (uint16_t)((b[i + 1] - '0') << 6 | (b[i + 2] - '0') << 3 | (b[i + 3] - '0'));
                i += 3;
            } else {
                u[k++] = b[i];
            }
        }
    } else {
        size_t used = 0, made = 0;
        if (!tec_encoding_available(enc) || tec_decode(enc, b, n, &used, u, n * 2 + 1, &made) != HT_OK ||
            used != n)
            goto fail;
        k = made;
    }
    *out = u;
    *nout = (uint32_t)k;
    return true;
fail:
    free(u);
    return false;
}

/* One character (a unit or a surrogate pair) in an encoding: its bytes, or
 * -1 if the encoding lacks it. */
static int encode_char(uint32_t enc, const uint16_t *u, uint32_t units, uint8_t out[16])
{
    uint16_t c = u[0];
    switch (enc) {
    case kCFStringEncodingMacRoman: {
        int b = roman_byte(c);
        if (b < 0)
            return -1;
        out[0] = (uint8_t)b;
        return 1;
    }
    case kCFStringEncodingASCII:
        if (c > 127)
            return -1;
        out[0] = (uint8_t)c;
        return 1;
    case kCFStringEncodingISOLatin1:
        if (c > 255)
            return -1;
        out[0] = (uint8_t)c;
        return 1;
    case kCFStringEncodingUnicode:
        out[0] = (uint8_t)(c >> 8), out[1] = (uint8_t)c;
        if (units == 2)
            out[2] = (uint8_t)(u[1] >> 8), out[3] = (uint8_t)u[1];
        return (int)units * 2;
    case kCFStringEncodingUTF8: {
        uint32_t cp = c;
        if (units == 2)
            cp = 0x10000 + ((uint32_t)(c - 0xD800) << 10) + (u[1] - 0xDC00);
        if (cp < 0x80) {
            out[0] = (uint8_t)cp;
            return 1;
        }
        if (cp < 0x800) {
            out[0] = (uint8_t)(0xC0 | cp >> 6), out[1] = (uint8_t)(0x80 | (cp & 0x3F));
            return 2;
        }
        if (cp < 0x10000) {
            out[0] = (uint8_t)(0xE0 | cp >> 12), out[1] = (uint8_t)(0x80 | (cp >> 6 & 0x3F));
            out[2] = (uint8_t)(0x80 | (cp & 0x3F));
            return 3;
        }
        out[0] = (uint8_t)(0xF0 | cp >> 18), out[1] = (uint8_t)(0x80 | (cp >> 12 & 0x3F));
        out[2] = (uint8_t)(0x80 | (cp >> 6 & 0x3F)), out[3] = (uint8_t)(0x80 | (cp & 0x3F));
        return 4;
    }
    case kCFStringEncodingNonLossyASCII:
        if (c < 128 && c != '\\') {
            out[0] = (uint8_t)c;
            return 1;
        }
        return snprintf((char *)out, 16, "\\u%04x", c);
    default: {
        size_t used = 0, made = 0;
        if (tec_encode(enc, u, units, &used, out, 16, &made) != HT_OK || used != units || !made)
            return -1;
        return (int)made;
    }
    }
}

static uint32_t char_units(const uint16_t *u, uint32_t left)
{
    return left >= 2 && (u[0] & 0xFC00) == 0xD800 && (u[1] & 0xFC00) == 0xDC00 ? 2 : 1;
}

/* Stateful encodings (ISO-2022's escapes) go a string at a time. */
static bool stateful(uint32_t enc) { return (enc & 0xFFFF) >= 0x0820 && (enc & 0xFFFF) <= 0x0840 && enc != kCFStringEncodingUTF8; }

/* Encode characters into out (NULL: count only) up to max bytes: the
 * bytes made; *used the units taken.  Stops at a character the encoding
 * lacks unless there is a loss byte. */
static size_t encode_chars(uint32_t enc, const uint16_t *u, uint32_t n, uint8_t loss, uint8_t *out, size_t max,
                           uint32_t *used)
{
    size_t k = 0;
    uint32_t i = 0;
    if (stateful(enc)) {
        size_t cap = (size_t)n * 8 + 16, made = 0, took = 0;
        uint8_t *tmp = malloc(cap);
        if (tmp && tec_encode(enc, u, n, &took, tmp, cap, &made) == HT_OK && took == n && made <= max) {
            if (out)
                memcpy(out, tmp, made);
            i = n;
            k = made;
        }
        free(tmp);
        *used = i;
        return k;
    }
    while (i < n) {
        uint8_t b[16];
        uint32_t units = char_units(u + i, n - i);
        int m = encode_char(enc, u + i, units, b);
        if (m < 0) {
            if (!loss)
                break;
            b[0] = loss;
            m = 1;
        }
        if (k + (size_t)m > max)
            break;
        if (out)
            memcpy(out + k, b, (size_t)m);
        k += (size_t)m;
        i += units;
    }
    *used = i;
    return k;
}

bool cf_encode(uint32_t enc, const uint16_t *u, uint32_t n, uint8_t loss, uint8_t **out, size_t *nout, uint32_t *used)
{
    size_t cap = (size_t)n * 8 + 16;
    uint8_t *b = malloc(cap);
    uint32_t took = 0;
    if (!b)
        return false;
    size_t k = encode_chars(enc, u, n, loss, b, cap, &took);
    *out = b;
    *nout = k;
    if (used)
        *used = took;
    return took == n;
}

static bool encoding_available(uint32_t enc)
{
    return enc == kCFStringEncodingMacRoman || enc == kCFStringEncodingASCII || enc == kCFStringEncodingISOLatin1 ||
           enc == kCFStringEncodingUnicode || enc == kCFStringEncodingUTF8 || enc == kCFStringEncodingNonLossyASCII ||
           tec_encoding_available(enc);
}

/* The system's encoding: its script's Mac encoding. */
static uint32_t system_encoding(void)
{
    uint32_t script = (uint32_t)tb_GetScriptManagerVariable(18); /* smSysScript */
    return script <= 32 ? script : kCFStringEncodingMacRoman;
}

/* ---------------------------------------------------------------------- */
/* Case and character classes */

static uint16_t to_lower(uint16_t c)
{
    if (c < 0x80)
        return c >= 'A' && c <= 'Z' ? (uint16_t)(c + 32) : c;
    if ((c >= 0xC0 && c <= 0xDE && c != 0xD7))
        return (uint16_t)(c + 32);
    if (c == 0x178)
        return 0xFF;
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177))
        return c & 1 ? c : (uint16_t)(c + 1);
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E))
        return c & 1 ? (uint16_t)(c + 1) : c;
    if (c >= 0x391 && c <= 0x3AB && c != 0x3A2)
        return (uint16_t)(c + 32);
    if (c == 0x386)
        return 0x3AC;
    if (c >= 0x388 && c <= 0x38A)
        return (uint16_t)(c + 37);
    if (c == 0x38C)
        return 0x3CC;
    if (c == 0x38E || c == 0x38F)
        return (uint16_t)(c + 63);
    if (c >= 0x410 && c <= 0x42F)
        return (uint16_t)(c + 32);
    if (c >= 0x400 && c <= 0x40F)
        return (uint16_t)(c + 80);
    if ((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x52F))
        return c & 1 ? c : (uint16_t)(c + 1);
    if (c >= 0x4C1 && c <= 0x4CE)
        return c & 1 ? (uint16_t)(c + 1) : c;
    if (c >= 0x531 && c <= 0x556)
        return (uint16_t)(c + 48);
    if ((c >= 0x1E00 && c <= 0x1E95) || (c >= 0x1EA0 && c <= 0x1EFF))
        return c & 1 ? c : (uint16_t)(c + 1);
    if (c >= 0xFF21 && c <= 0xFF3A)
        return (uint16_t)(c + 32);
    return c;
}

static uint16_t to_upper(uint16_t c)
{
    if (c < 0x80)
        return c >= 'a' && c <= 'z' ? (uint16_t)(c - 32) : c;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7)
        return (uint16_t)(c - 32);
    if (c == 0xFF)
        return 0x178;
    if (c == 0xB5)
        return 0x39C;
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177))
        return c & 1 ? (uint16_t)(c - 1) : c;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E))
        return c & 1 ? c : (uint16_t)(c - 1);
    if (c == 0x3C2)
        return 0x3A3;
    if (c >= 0x3B1 && c <= 0x3CB)
        return (uint16_t)(c - 32);
    if (c == 0x3AC)
        return 0x386;
    if (c >= 0x3AD && c <= 0x3AF)
        return (uint16_t)(c - 37);
    if (c == 0x3CC)
        return 0x38C;
    if (c == 0x3CD || c == 0x3CE)
        return (uint16_t)(c - 63);
    if (c >= 0x430 && c <= 0x44F)
        return (uint16_t)(c - 32);
    if (c >= 0x450 && c <= 0x45F)
        return (uint16_t)(c - 80);
    if ((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x52F))
        return c & 1 ? (uint16_t)(c - 1) : c;
    if (c >= 0x4C1 && c <= 0x4CE)
        return c & 1 ? c : (uint16_t)(c - 1);
    if (c >= 0x561 && c <= 0x586)
        return (uint16_t)(c - 48);
    if ((c >= 0x1E00 && c <= 0x1E95) || (c >= 0x1EA0 && c <= 0x1EFF))
        return c & 1 ? (uint16_t)(c - 1) : c;
    if (c >= 0xFF41 && c <= 0xFF5A)
        return (uint16_t)(c - 32);
    return c;
}

/* Inclusive ranges of the BMP, sorted. */
typedef struct {
    uint16_t lo, hi;
} urange;

static const urange letters[] = {
    { 0x41, 0x5A }, { 0x61, 0x7A }, { 0xAA, 0xAA }, { 0xB5, 0xB5 }, { 0xBA, 0xBA }, { 0xC0, 0xD6 },
    { 0xD8, 0xF6 }, { 0xF8, 0x2C1 }, { 0x2C6, 0x2D1 }, { 0x2E0, 0x2E4 }, { 0x2EC, 0x2EC }, { 0x2EE, 0x2EE },
    { 0x370, 0x374 }, { 0x376, 0x377 }, { 0x37A, 0x37D }, { 0x386, 0x386 }, { 0x388, 0x38A }, { 0x38C, 0x38C },
    { 0x38E, 0x3A1 }, { 0x3A3, 0x3F5 }, { 0x3F7, 0x481 }, { 0x48A, 0x52F }, { 0x531, 0x556 }, { 0x559, 0x559 },
    { 0x561, 0x587 }, { 0x5D0, 0x5EA }, { 0x5F0, 0x5F2 }, { 0x620, 0x64A }, { 0x66E, 0x66F }, { 0x671, 0x6D3 },
    { 0x6D5, 0x6D5 }, { 0x6E5, 0x6E6 }, { 0x6EE, 0x6EF }, { 0x6FA, 0x6FC }, { 0x6FF, 0x6FF }, { 0x904, 0x939 },
    { 0x93D, 0x93D }, { 0x950, 0x950 }, { 0x958, 0x961 }, { 0x971, 0x97F }, { 0x985, 0x9B9 }, { 0xA05, 0xA39 },
    { 0xA85, 0xAB9 }, { 0xB05, 0xB39 }, { 0xB85, 0xBB9 }, { 0xC05, 0xC39 }, { 0xC85, 0xCB9 }, { 0xD05, 0xD3A },
    { 0xE01, 0xE30 }, { 0xE32, 0xE33 }, { 0xE40, 0xE46 }, { 0xE81, 0xEB0 }, { 0xF40, 0xF6C }, { 0x10A0, 0x10FF },
    { 0x1100, 0x11FF }, { 0x1E00, 0x1F15 }, { 0x1F18, 0x1F1D }, { 0x1F20, 0x1F45 }, { 0x1F48, 0x1F4D },
    { 0x1F50, 0x1F7D }, { 0x1F80, 0x1FB4 }, { 0x1FB6, 0x1FBC }, { 0x1FC2, 0x1FCC }, { 0x1FD0, 0x1FDB },
    { 0x1FE0, 0x1FEC }, { 0x1FF2, 0x1FFC }, { 0x2071, 0x2071 }, { 0x207F, 0x207F }, { 0x2102, 0x2102 },
    { 0x2107, 0x2107 }, { 0x210A, 0x2113 }, { 0x2115, 0x2115 }, { 0x2119, 0x211D }, { 0x2124, 0x2124 },
    { 0x2126, 0x2126 }, { 0x2128, 0x2128 }, { 0x212A, 0x212D }, { 0x212F, 0x2139 }, { 0x3005, 0x3006 },
    { 0x3031, 0x3035 }, { 0x3041, 0x3096 }, { 0x309D, 0x309F }, { 0x30A1, 0x30FA }, { 0x30FC, 0x30FF },
    { 0x3105, 0x312F }, { 0x3131, 0x318E }, { 0x31A0, 0x31BA }, { 0x31F0, 0x31FF }, { 0x3400, 0x4DB5 },
    { 0x4E00, 0x9FFF }, { 0xA000, 0xA48C }, { 0xAC00, 0xD7A3 }, { 0xF900, 0xFAFF }, { 0xFB00, 0xFB06 },
    { 0xFB13, 0xFB17 }, { 0xFB1D, 0xFB1D }, { 0xFB1F, 0xFB28 }, { 0xFB2A, 0xFB4F }, { 0xFB50, 0xFDFB },
    { 0xFE70, 0xFEFC }, { 0xFF21, 0xFF3A }, { 0xFF41, 0xFF5A }, { 0xFF66, 0xFFDC },
};

static const urange marks[] = {
    { 0x300, 0x36F }, { 0x483, 0x489 }, { 0x591, 0x5BD }, { 0x5BF, 0x5BF }, { 0x5C1, 0x5C2 }, { 0x5C4, 0x5C5 },
    { 0x5C7, 0x5C7 }, { 0x610, 0x61A }, { 0x64B, 0x65F }, { 0x670, 0x670 }, { 0x6D6, 0x6DC }, { 0x6DF, 0x6E4 },
    { 0x6E7, 0x6E8 }, { 0x6EA, 0x6ED }, { 0x900, 0x903 }, { 0x93A, 0x93C }, { 0x93E, 0x94F }, { 0x951, 0x957 },
    { 0x962, 0x963 }, { 0x981, 0x983 }, { 0x9BC, 0x9D7 }, { 0xA01, 0xA03 }, { 0xA3C, 0xA51 }, { 0xA81, 0xA83 },
    { 0xABC, 0xACD }, { 0xB01, 0xB03 }, { 0xB3C, 0xB57 }, { 0xBBE, 0xBD7 }, { 0xC01, 0xC03 }, { 0xC3E, 0xC56 },
    { 0xCBC, 0xCD6 }, { 0xD02, 0xD03 }, { 0xD3E, 0xD57 }, { 0xE31, 0xE31 }, { 0xE34, 0xE3A }, { 0xE47, 0xE4E },
    { 0xEB1, 0xEB1 }, { 0xEB4, 0xEBC }, { 0xEC8, 0xECD }, { 0x20D0, 0x20FF }, { 0x302A, 0x302F },
    { 0x3099, 0x309A }, { 0xFB1E, 0xFB1E }, { 0xFE00, 0xFE0F }, { 0xFE20, 0xFE2F },
};

static const urange digits[] = {
    { 0x30, 0x39 }, { 0x660, 0x669 }, { 0x6F0, 0x6F9 }, { 0x966, 0x96F }, { 0x9E6, 0x9EF }, { 0xA66, 0xA6F },
    { 0xAE6, 0xAEF }, { 0xB66, 0xB6F }, { 0xBE6, 0xBEF }, { 0xC66, 0xC6F }, { 0xCE6, 0xCEF }, { 0xD66, 0xD6F },
    { 0xE50, 0xE59 }, { 0xED0, 0xED9 }, { 0xF20, 0xF29 }, { 0xFF10, 0xFF19 },
};

static const urange other_numbers[] = {
    { 0xB2, 0xB3 }, { 0xB9, 0xB9 }, { 0xBC, 0xBE }, { 0x2070, 0x2070 }, { 0x2074, 0x2079 }, { 0x2080, 0x2089 },
    { 0x2150, 0x2189 }, { 0x2460, 0x249B }, { 0x24EA, 0x24FF }, { 0x2776, 0x2793 }, { 0x3007, 0x3007 },
    { 0x3021, 0x3029 }, { 0x3038, 0x303A }, { 0x3192, 0x3195 }, { 0x3220, 0x3229 }, { 0x3280, 0x3289 },
};

static const urange punctuation[] = {
    { 0x21, 0x23 }, { 0x25, 0x2A }, { 0x2C, 0x2F }, { 0x3A, 0x3B }, { 0x3F, 0x40 }, { 0x5B, 0x5D }, { 0x5F, 0x5F },
    { 0x7B, 0x7B }, { 0x7D, 0x7D }, { 0xA1, 0xA1 }, { 0xA7, 0xA7 }, { 0xAB, 0xAB }, { 0xB6, 0xB7 }, { 0xBB, 0xBB },
    { 0xBF, 0xBF }, { 0x37E, 0x37E }, { 0x387, 0x387 }, { 0x55A, 0x55F }, { 0x589, 0x58A }, { 0x5BE, 0x5BE },
    { 0x5C0, 0x5C0 }, { 0x5C3, 0x5C3 }, { 0x5C6, 0x5C6 }, { 0x5F3, 0x5F4 }, { 0x609, 0x60A }, { 0x60C, 0x60D },
    { 0x61B, 0x61B }, { 0x61E, 0x61F }, { 0x66A, 0x66D }, { 0x6D4, 0x6D4 }, { 0x964, 0x965 }, { 0x970, 0x970 },
    { 0xE4F, 0xE4F }, { 0xE5A, 0xE5B }, { 0x2010, 0x2027 }, { 0x2030, 0x2043 }, { 0x2045, 0x2051 },
    { 0x2053, 0x205E }, { 0x207D, 0x207E }, { 0x208D, 0x208E }, { 0x2329, 0x232A }, { 0x3001, 0x3003 },
    { 0x3008, 0x3011 }, { 0x3014, 0x301F }, { 0x3030, 0x3030 }, { 0x303D, 0x303D }, { 0x30A0, 0x30A0 },
    { 0x30FB, 0x30FB }, { 0xFD3E, 0xFD3F }, { 0xFE10, 0xFE19 }, { 0xFE30, 0xFE52 }, { 0xFE54, 0xFE61 },
    { 0xFE63, 0xFE63 }, { 0xFE68, 0xFE68 }, { 0xFE6A, 0xFE6B }, { 0xFF01, 0xFF03 }, { 0xFF05, 0xFF0A },
    { 0xFF0C, 0xFF0F }, { 0xFF1A, 0xFF1B }, { 0xFF1F, 0xFF20 }, { 0xFF3B, 0xFF3D }, { 0xFF3F, 0xFF3F },
    { 0xFF5B, 0xFF5B }, { 0xFF5D, 0xFF5D }, { 0xFF5F, 0xFF65 },
};

static const urange controls[] = {
    { 0x00, 0x1F }, { 0x7F, 0x9F }, { 0xAD, 0xAD }, { 0x600, 0x605 }, { 0x6DD, 0x6DD }, { 0x200B, 0x200F },
    { 0x202A, 0x202E }, { 0x2060, 0x2064 }, { 0x206A, 0x206F }, { 0xFEFF, 0xFEFF }, { 0xFFF9, 0xFFFB },
};

static const urange whitespace[] = {
    { 0x09, 0x09 }, { 0x20, 0x20 }, { 0xA0, 0xA0 }, { 0x1680, 0x1680 }, { 0x2000, 0x200B }, { 0x202F, 0x202F },
    { 0x205F, 0x205F }, { 0x3000, 0x3000 },
};

static const urange newlines[] = { { 0x0A, 0x0D }, { 0x85, 0x85 }, { 0x2028, 0x2029 } };

static const urange decomposables[] = {
    { 0xC0, 0xC5 }, { 0xC7, 0xCF }, { 0xD1, 0xD6 }, { 0xD9, 0xDD }, { 0xE0, 0xE5 }, { 0xE7, 0xEF }, { 0xF1, 0xF6 },
    { 0xF9, 0xFD }, { 0xFF, 0x10F }, { 0x112, 0x125 }, { 0x128, 0x130 }, { 0x134, 0x137 }, { 0x139, 0x13E },
    { 0x143, 0x148 }, { 0x14C, 0x151 }, { 0x154, 0x165 }, { 0x168, 0x17E }, { 0x1A0, 0x1A1 }, { 0x1AF, 0x1B0 },
    { 0x1CD, 0x1DC }, { 0x1DE, 0x1E3 }, { 0x1E6, 0x1F0 }, { 0x1F4, 0x1F5 }, { 0x1F8, 0x21B }, { 0x21E, 0x21F },
    { 0x226, 0x233 }, { 0x340, 0x341 }, { 0x343, 0x344 }, { 0x374, 0x374 }, { 0x37E, 0x37E }, { 0x385, 0x38A },
    { 0x38C, 0x38C }, { 0x38E, 0x390 }, { 0x3AA, 0x3B0 }, { 0x3CA, 0x3CE }, { 0x3D3, 0x3D4 }, { 0x400, 0x401 },
    { 0x403, 0x403 }, { 0x407, 0x407 }, { 0x40C, 0x40E }, { 0x419, 0x419 }, { 0x439, 0x439 }, { 0x450, 0x451 },
    { 0x453, 0x453 }, { 0x457, 0x457 }, { 0x45C, 0x45E }, { 0x476, 0x477 }, { 0x4C1, 0x4C2 }, { 0x4D0, 0x4D3 },
    { 0x4D6, 0x4D7 }, { 0x4DA, 0x4DF }, { 0x4E2, 0x4E7 }, { 0x4EA, 0x4F5 }, { 0x4F8, 0x4F9 }, { 0x622, 0x626 },
    { 0x6C0, 0x6C0 }, { 0x6C2, 0x6C2 }, { 0x6D3, 0x6D3 }, { 0x929, 0x929 }, { 0x931, 0x931 }, { 0x934, 0x934 },
    { 0x958, 0x95F }, { 0x1E00, 0x1E99 }, { 0x1E9B, 0x1E9B }, { 0x1EA0, 0x1EF9 }, { 0x1F00, 0x1F15 },
    { 0x1F18, 0x1F1D }, { 0x1F20, 0x1F45 }, { 0x1F48, 0x1F4D }, { 0x1F50, 0x1F57 }, { 0x1F59, 0x1F7D },
    { 0x1F80, 0x1FB4 }, { 0x1FB6, 0x1FC4 }, { 0x1FC6, 0x1FD3 }, { 0x1FD6, 0x1FDB }, { 0x1FDD, 0x1FEF },
    { 0x1FF2, 0x1FF4 }, { 0x1FF6, 0x1FFD }, { 0x2000, 0x2001 }, { 0x2126, 0x2126 }, { 0x212A, 0x212B },
    { 0x219A, 0x219B }, { 0x21AE, 0x21AE }, { 0x21CD, 0x21CF }, { 0x2204, 0x2204 }, { 0x2209, 0x2209 },
    { 0x220C, 0x220C }, { 0x2224, 0x2224 }, { 0x2226, 0x2226 }, { 0x2241, 0x2241 }, { 0x2244, 0x2244 },
    { 0x2247, 0x2247 }, { 0x2249, 0x2249 }, { 0x2260, 0x2260 }, { 0x2262, 0x2262 }, { 0x226D, 0x2271 },
    { 0x2274, 0x2275 }, { 0x2278, 0x2279 }, { 0x2280, 0x2281 }, { 0x2284, 0x2285 }, { 0x2288, 0x2289 },
    { 0x22AC, 0x22AF }, { 0x22E0, 0x22E3 }, { 0x22EA, 0x22ED }, { 0x2329, 0x232A }, { 0x304C, 0x304C },
    { 0x304E, 0x304E }, { 0x3050, 0x3050 }, { 0x3052, 0x3052 }, { 0x3054, 0x3054 }, { 0x3056, 0x3056 },
    { 0x3058, 0x3058 }, { 0x305A, 0x305A }, { 0x305C, 0x305C }, { 0x305E, 0x305E }, { 0x3060, 0x3060 },
    { 0x3062, 0x3062 }, { 0x3065, 0x3065 }, { 0x3067, 0x3067 }, { 0x3069, 0x3069 }, { 0x3070, 0x3071 },
    { 0x3073, 0x3074 }, { 0x3076, 0x3077 }, { 0x3079, 0x307A }, { 0x307C, 0x307D }, { 0x3094, 0x3094 },
    { 0x309E, 0x309E }, { 0x30AC, 0x30AC }, { 0x30AE, 0x30AE }, { 0x30B0, 0x30B0 }, { 0x30B2, 0x30B2 },
    { 0x30B4, 0x30B4 }, { 0x30B6, 0x30B6 }, { 0x30B8, 0x30B8 }, { 0x30BA, 0x30BA }, { 0x30BC, 0x30BC },
    { 0x30BE, 0x30BE }, { 0x30C0, 0x30C0 }, { 0x30C2, 0x30C2 }, { 0x30C5, 0x30C5 }, { 0x30C7, 0x30C7 },
    { 0x30C9, 0x30C9 }, { 0x30D0, 0x30D1 }, { 0x30D3, 0x30D4 }, { 0x30D6, 0x30D7 }, { 0x30D9, 0x30DA },
    { 0x30DC, 0x30DD }, { 0x30F4, 0x30F4 }, { 0x30F7, 0x30FA }, { 0x30FE, 0x30FE }, { 0xAC00, 0xD7A3 },
    { 0xF900, 0xFA0D }, { 0xFA10, 0xFA10 }, { 0xFA12, 0xFA12 }, { 0xFA15, 0xFA1E }, { 0xFA20, 0xFA20 },
    { 0xFA22, 0xFA22 }, { 0xFA25, 0xFA26 }, { 0xFA2A, 0xFA2D }, { 0xFB1D, 0xFB1D }, { 0xFB1F, 0xFB1F },
    { 0xFB2A, 0xFB36 }, { 0xFB38, 0xFB3C }, { 0xFB3E, 0xFB3E }, { 0xFB40, 0xFB41 }, { 0xFB43, 0xFB44 },
    { 0xFB46, 0xFB4E },
};

static const urange illegal[] = { { 0xFDD0, 0xFDEF }, { 0xFFFE, 0xFFFF } };

#define NELEM(a) (unsigned)(sizeof a / sizeof a[0])

static bool in_ranges(const urange *r, unsigned n, uint16_t c)
{
    unsigned lo = 0, hi = n;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (c < r[mid].lo)
            hi = mid;
        else if (c > r[mid].hi)
            lo = mid + 1;
        else
            return true;
    }
    return false;
}

static bool is_whitespace(uint16_t c) { return in_ranges(whitespace, NELEM(whitespace), c); }
static bool is_newline(uint16_t c) { return in_ranges(newlines, NELEM(newlines), c); }
static bool is_letter(uint16_t c) { return in_ranges(letters, NELEM(letters), c) || in_ranges(marks, NELEM(marks), c); }
static bool is_digit(uint16_t c) { return in_ranges(digits, NELEM(digits), c); }
static int digit_value(uint16_t c)
{
    for (unsigned i = 0; i < NELEM(digits); i++)
        if (c >= digits[i].lo && c <= digits[i].hi)
            return c - digits[i].lo;
    return -1;
}

/* ---------------------------------------------------------------------- */
/* String objects */

static cf_string *str_of(gaddr ref) { return cf_get_type(ref, CFT_STRING); }

static void str_set(cf_string *s, const uint16_t *u, uint32_t n)
{
    if (n + 1 > s->cap) {
        uint32_t cap = n + 16;
        uint16_t *p = realloc(s->u, sizeof *p * cap);
        if (!p)
            return;
        s->u = p;
        s->cap = cap;
    }
    if (n && u != s->u)
        memmove(s->u, u, sizeof *u * n);
    s->n = n;
}

/* The external buffer gets what changed while it holds it; past its
 * capacity the string takes a bigger one from its allocator, or keeps its
 * characters itself where that is the null allocator. */
static void str_write_back(cf_string *s)
{
    if (!s->ext)
        return;
    if (s->n > s->ext_cap) {
        uint32_t cap = s->n * 2;
        gaddr p = s->ext_alloc != cf_allocator_null() ? cf_reallocate(s->ext_alloc, s->ext, cap * 2, s->ext_cap * 2) : 0;
        if (!p) {
            s->ext = 0;
            return;
        }
        s->ext = p;
        s->ext_cap = cap;
    }
    for (uint32_t i = 0; i < s->n; i++)
        wr16(s->ext + 2 * i, s->u[i]);
}

/* Replace the range [loc, loc+len) with n units. */
static void str_replace(cf_string *s, uint32_t loc, uint32_t len, const uint16_t *u, uint32_t n)
{
    if (loc > s->n)
        loc = s->n;
    if (len > s->n - loc)
        len = s->n - loc;
    uint32_t total = s->n - len + n;
    uint16_t *copy = NULL;
    if (n && u >= s->u && u < s->u + s->cap) { /* from itself */
        copy = malloc(sizeof *copy * n);
        if (!copy)
            return;
        memcpy(copy, u, sizeof *copy * n);
        u = copy;
    }
    if (total + 1 > s->cap) {
        uint32_t cap = total * 2 + 16;
        uint16_t *p = realloc(s->u, sizeof *p * cap);
        if (!p) {
            free(copy);
            return;
        }
        s->u = p;
        s->cap = cap;
    }
    memmove(s->u + loc + n, s->u + loc + len, sizeof *s->u * (s->n - loc - len));
    if (n)
        memcpy(s->u + loc, u, sizeof *u * n);
    s->n = total;
    free(copy);
    str_write_back(s);
}

static void str_finalize(cf_obj *o)
{
    cf_string *s = (cf_string *)o;
    if (s->src8 && s->src8_owned)
        cf_deallocate(s->src8_free, s->src8);
    if (s->src16 && s->src16_owned)
        cf_deallocate(s->src16_free, s->src16);
    if (s->cptr)
        tb_DisposePtr(s->cptr);
    if (s->pptr)
        tb_DisposePtr(s->pptr);
    if (s->uptr)
        tb_DisposePtr(s->uptr);
}

static void str_dispose(cf_obj *o) { free(((cf_string *)o)->u); }

static bool str_equal(cf_obj *a, cf_obj *b)
{
    cf_string *x = (cf_string *)a, *y = (cf_string *)b;
    return x->n == y->n && (!x->n || !memcmp(x->u, y->u, sizeof *x->u * x->n));
}

static uint32_t str_hash(cf_obj *o)
{
    cf_string *s = (cf_string *)o;
    uint32_t h = s->n;
    for (uint32_t i = 0; i < s->n; i++)
        h = h * 257 + s->u[i];
    return h + (h << (s->n & 31));
}

static void str_describe(cf_obj *o, cf_text *out)
{
    cf_string *s = (cf_string *)o;
    text_add_utf8(out, "contents = \"");
    text_add(out, s->u, s->n);
    text_add_unit(out, '"');
}

static const cf_class string_class = { "CFString", str_finalize, str_dispose, str_equal, str_hash, str_describe };

static cf_string *str_make(gaddr alloc, const uint16_t *u, uint32_t n, bool mutable_)
{
    cf_string *s = cf_new(CFT_STRING, sizeof *s, alloc, 0);
    if (!s)
        return NULL;
    s->mut = mutable_;
    s->enc8 = kCFStringEncodingInvalidId;
    str_set(s, u, n);
    if (!s->u)
        str_set(s, (const uint16_t *)"", 0);
    return s;
}

gaddr cfstr_new(gaddr alloc, const uint16_t *u, uint32_t n, bool mutable_)
{
    cf_string *s = str_make(alloc, u, n, mutable_);
    return s ? s->o.ref : 0;
}

gaddr cfstr_from_text(const cf_text *t) { return cfstr_new(0, t->u, t->n, false); }

gaddr cfstr_from_utf8(const char *s)
{
    uint16_t *u;
    uint32_t n;
    if (!cf_decode(kCFStringEncodingUTF8, (const uint8_t *)s, strlen(s), &u, &n))
        return 0;
    gaddr r = cfstr_new(0, u, n, false);
    free(u);
    return r;
}

const uint16_t *cfstr_chars(gaddr ref, uint32_t *n)
{
    cf_string *s = str_of(ref);
    if (!s)
        return NULL;
    *n = s->n;
    return s->u;
}

char *cfstr_utf8(gaddr ref)
{
    cf_string *s = str_of(ref);
    if (!s)
        return NULL;
    uint8_t *b;
    size_t n;
    uint32_t used;
    cf_encode(kCFStringEncodingUTF8, s->u, s->n, '?', &b, &n, &used);
    char *r = malloc(n + 1);
    if (r) {
        memcpy(r, b, n);
        r[n] = 0;
    }
    free(b);
    return r;
}

/* Bytes from guest memory in an encoding: a new string, or 0. */
static cf_string *str_from_bytes(gaddr alloc, gaddr bytes, uint32_t n, uint32_t enc, bool mutable_)
{
    if (n > 0x1000000 || (n && !bytes))
        return NULL;
    uint8_t *b = malloc(n ? n : 1);
    if (!b)
        return NULL;
    gget(b, bytes, n);
    uint16_t *u;
    uint32_t k;
    cf_string *s = NULL;
    if (cf_decode(enc, b, n, &u, &k)) {
        s = str_make(alloc, u, k, mutable_);
        if (s && !is_unicode16(enc))
            s->enc8 = enc;
        if (s && is_unicode16(enc))
            s->wide = true;
        free(u);
    }
    free(b);
    return s;
}

static uint32_t cstr_len(gaddr p)
{
    uint32_t n = 0;
    while (p && rd8(p + n))
        n++;
    return n;
}

/* The constant strings: CFSTR's, by text, and ours. */
typedef struct {
    char *text;
    gaddr ref;
} constant;
static constant *constants;
static unsigned nconstants, constants_cap;

static gaddr constant_of(const char *text, size_t len, uint32_t enc, gaddr literal)
{
    for (unsigned i = 0; i < nconstants; i++)
        if (strlen(constants[i].text) == len && !memcmp(constants[i].text, text, len))
            return constants[i].ref;
    uint16_t *u;
    uint32_t n;
    if (!cf_decode(enc, (const uint8_t *)text, len, &u, &n))
        return 0;
    cf_string *s = str_make(cf_allocator_system(), u, n, false);
    free(u);
    if (!s)
        return 0;
    cf_make_constant(&s->o);
    s->enc8 = enc;
    s->src8 = literal; /* the literal is its C string */
    if (nconstants == constants_cap) {
        unsigned cap = constants_cap ? constants_cap * 2 : 64;
        constant *p = realloc(constants, sizeof *p * cap);
        if (!p)
            return s->o.ref;
        constants = p;
        constants_cap = cap;
    }
    constants[nconstants].text = malloc(len + 1);
    if (constants[nconstants].text) {
        memcpy(constants[nconstants].text, text, len);
        constants[nconstants].text[len] = 0;
        constants[nconstants++].ref = s->o.ref;
    }
    return s->o.ref;
}

gaddr cfstr_constant(const char *ascii) { return constant_of(ascii, strlen(ascii), kCFStringEncodingMacRoman, 0); }

void cfstr_reset(void)
{
    for (unsigned i = 0; i < nconstants; i++)
        free(constants[i].text);
    free(constants);
    constants = NULL;
    nconstants = constants_cap = 0;
    cf_register_class(CFT_STRING, &string_class);
    cfcharset_reset();
}

/* ---------------------------------------------------------------------- */
/* Creating */

static void cf_CFStringGetTypeID(ppc_cpu *c) { RETURN(CFT_STRING); }

/* CFStringCreateWithPascalString(alloc, pStr, encoding) */
static void cf_CFStringCreateWithPascalString(ppc_cpu *c)
{
    cf_string *s = R(1) ? str_from_bytes(R(0), R(1) + 1, rd8(R(1)), R(2), false) : NULL;
    RETURN(s ? s->o.ref : 0);
}

/* CFStringCreateWithCString(alloc, cStr, encoding) */
static void cf_CFStringCreateWithCString(ppc_cpu *c)
{
    cf_string *s = R(1) ? str_from_bytes(R(0), R(1), cstr_len(R(1)), R(2), false) : NULL;
    RETURN(s ? s->o.ref : 0);
}

/* CFStringCreateWithCharacters(alloc, chars, numChars) */
static void cf_CFStringCreateWithCharacters(ppc_cpu *c)
{
    cf_string *s = str_from_bytes(R(0), R(1), R(2) * 2, kCFStringEncodingUnicode, false);
    RETURN(s ? s->o.ref : 0);
}

/* The NoCopy forms: the bytes stay the program's, freed with the string by
 * contentsDeallocator (NULL the default allocator; kCFAllocatorNull none). */
static void keep8(cf_string *s, gaddr src, gaddr dealloc, bool pascal)
{
    s->src8 = src;
    s->src8_pascal = pascal;
    s->src8_owned = dealloc != cf_allocator_null();
    s->src8_free = dealloc;
}

/* CFStringCreateWithPascalStringNoCopy(alloc, pStr, encoding, contentsDeallocator) */
static void cf_CFStringCreateWithPascalStringNoCopy(ppc_cpu *c)
{
    cf_string *s = R(1) ? str_from_bytes(R(0), R(1) + 1, rd8(R(1)), R(2), false) : NULL;
    if (s)
        keep8(s, R(1), R(3), true);
    RETURN(s ? s->o.ref : 0);
}

/* CFStringCreateWithCStringNoCopy(alloc, cStr, encoding, contentsDeallocator) */
static void cf_CFStringCreateWithCStringNoCopy(ppc_cpu *c)
{
    cf_string *s = R(1) ? str_from_bytes(R(0), R(1), cstr_len(R(1)), R(2), false) : NULL;
    if (s)
        keep8(s, R(1), R(3), false);
    RETURN(s ? s->o.ref : 0);
}

/* CFStringCreateWithCharactersNoCopy(alloc, chars, numChars, contentsDeallocator) */
static void cf_CFStringCreateWithCharactersNoCopy(ppc_cpu *c)
{
    cf_string *s = str_from_bytes(R(0), R(1), R(2) * 2, kCFStringEncodingUnicode, false);
    if (s) {
        s->src16 = R(1);
        s->src16_owned = R(3) != cf_allocator_null();
        s->src16_free = R(3);
    }
    RETURN(s ? s->o.ref : 0);
}

/* A range of a string, if it is within it. */
static bool range_ok(cf_string *s, uint32_t loc, uint32_t len) { return s && loc <= s->n && len <= s->n - loc; }

/* CFStringCreateWithSubstring(alloc, str, range) */
static void cf_CFStringCreateWithSubstring(ppc_cpu *c)
{
    cf_string *s = str_of(R(1));
    if (!range_ok(s, R(2), R(3)))
        RETURN(0);
    RETURN(cfstr_new(R(0), s->u + R(2), R(3), false));
}

/* CFStringCreateCopy(alloc, theString): an immutable one is itself. */
static void cf_CFStringCreateCopy(ppc_cpu *c)
{
    cf_string *s = str_of(R(1));
    if (!s)
        RETURN(0);
    if (!s->mut && !s->ext && cf_allocator_resolve(R(0)) == cf_allocator_resolve(s->o.alloc))
        RETURN(cf_retain(R(1)));
    RETURN(cfstr_new(R(0), s->u, s->n, false));
}

/* CFStringCreateMutable(alloc, maxLength) */
static void cf_CFStringCreateMutable(ppc_cpu *c) { RETURN(cfstr_new(R(0), NULL, 0, true)); }

/* CFStringCreateMutableCopy(alloc, maxLength, theString) */
static void cf_CFStringCreateMutableCopy(ppc_cpu *c)
{
    cf_string *s = str_of(R(2));
    RETURN(s ? cfstr_new(R(0), s->u, s->n, true) : 0);
}

/* CFStringCreateMutableWithExternalCharactersNoCopy(alloc, chars, numChars, capacity,
 *                                                    externalCharactersAllocator) */
static void cf_CFStringCreateMutableWithExternalCharactersNoCopy(ppc_cpu *c)
{
    cf_string *s = str_from_bytes(R(0), R(1), R(1) ? R(2) * 2 : 0, kCFStringEncodingUnicode, true);
    if (!s)
        RETURN(0);
    s->ext = R(1);
    s->ext_cap = R(3) > R(2) ? R(3) : R(2);
    s->ext_alloc = R(4);
    RETURN(s->o.ref);
}

/* CFStringSetExternalCharactersNoCopy(theString, chars, length, capacity):
 * the buffer (perhaps changed by the program) is the string again. */
static void cf_CFStringSetExternalCharactersNoCopy(ppc_cpu *c)
{
    cf_string *s = str_of(R(0));
    if (!s || R(2) > 0x800000)
        RETURN_VOID();
    uint16_t *u = malloc(sizeof *u * (R(2) + 1));
    if (!u)
        RETURN_VOID();
    for (uint32_t i = 0; i < R(2); i++)
        u[i] = rd16(R(1) + 2 * i);
    s->ext = 0;
    str_set(s, u, R(2));
    free(u);
    s->ext = R(1);
    s->ext_cap = R(3) > R(2) ? R(3) : R(2);
    RETURN_VOID();
}

/* ---------------------------------------------------------------------- */
/* Formats */

typedef struct {
    const ppc_cpu *c;
    gaddr va;       /* a va_list: the words in memory */
    unsigned first; /* else the call's argument words from this one */
} fmt_args;

static uint32_t fmt_word(const fmt_args *a, unsigned i)
{
    return a->va ? rd32(a->va + 4 * i) : cf_arg(a->c, a->first + i);
}

enum { MAX_FMT_ARGS = 64 };

/* One conversion as parsed: where it is and what it takes. */
typedef struct {
    int pos;                  /* argument number (0-based), -1 none */
    int width, prec;          /* -1 none; -2 from an argument */
    int width_arg, prec_arg;  /* their argument numbers */
    char flags[8];
    int size;                 /* -2 hh, -1 h, 0, 1 l, 2 ll/q, 3 L */
    uint16_t conv;
    uint32_t end;             /* index after the conversion */
} fmt_spec;

static bool parse_spec(const uint16_t *f, uint32_t n, uint32_t i, int *next, fmt_spec *sp)
{
    memset(sp, 0, sizeof *sp);
    sp->width = sp->prec = -1;
    i++; /* '%' */
    /* n$ */
    uint32_t j = i;
    int num = 0;
    while (j < n && f[j] >= '0' && f[j] <= '9')
        num = num * 10 + (f[j++] - '0');
    int explicit_pos = -1;
    if (j > i && j < n && f[j] == '$') {
        explicit_pos = num - 1;
        i = j + 1;
    }
    size_t nf = 0;
    while (i < n && (f[i] == '-' || f[i] == '+' || f[i] == ' ' || f[i] == '#' || f[i] == '0' || f[i] == '\'')) {
        if (f[i] != '\'' && nf < sizeof sp->flags - 1)
            sp->flags[nf++] = (char)f[i];
        i++;
    }
    if (i < n && f[i] == '*') {
        sp->width = -2;
        sp->width_arg = (*next)++;
        i++;
    } else {
        while (i < n && f[i] >= '0' && f[i] <= '9')
            sp->width = (sp->width < 0 ? 0 : sp->width * 10) + (f[i++] - '0');
    }
    if (i < n && f[i] == '.') {
        i++;
        sp->prec = 0;
        if (i < n && f[i] == '*') {
            sp->prec = -2;
            sp->prec_arg = (*next)++;
            i++;
        } else {
            while (i < n && f[i] >= '0' && f[i] <= '9')
                sp->prec = sp->prec * 10 + (f[i++] - '0');
        }
    }
    for (; i < n; i++) {
        uint16_t m = f[i];
        if (m == 'h')
            sp->size = sp->size == -1 ? -2 : -1;
        else if (m == 'l')
            sp->size = sp->size == 1 ? 2 : 1;
        else if (m == 'q')
            sp->size = 2;
        else if (m == 'L')
            sp->size = 3;
        else if (m != 'j' && m != 'z' && m != 't')
            break;
    }
    if (i >= n)
        return false;
    sp->conv = f[i];
    sp->end = i + 1;
    sp->pos = sp->conv == '%' ? -1 : explicit_pos >= 0 ? explicit_pos : (*next)++;
    return true;
}

static int spec_words(const fmt_spec *sp)
{
    switch (sp->conv) {
    case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A':
        return 2;
    case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'D': case 'U': case 'O':
        return sp->size == 2 ? 2 : 1;
    default:
        return 1;
    }
}

static void add_ascii(cf_text *t, const char *s)
{
    while (*s)
        text_add_unit(t, (uint8_t)*s++);
}

/* Units padded to a width (left-justified with '-'). */
static void add_padded(cf_text *t, const uint16_t *u, uint32_t n, const fmt_spec *sp, int width)
{
    bool left = strchr(sp->flags, '-') != NULL;
    uint32_t pad = width > 0 && (uint32_t)width > n ? (uint32_t)width - n : 0;
    if (!left)
        for (uint32_t i = 0; i < pad; i++)
            text_add_unit(t, ' ');
    text_add(t, u, n);
    if (left)
        for (uint32_t i = 0; i < pad; i++)
            text_add_unit(t, ' ');
}

/* A format and its arguments, appended to out. */
static void format_into(cf_text *out, const uint16_t *f, uint32_t n, const fmt_args *a)
{
    /* First the arguments' sizes, so a numbered one can be found. */
    int sizes[MAX_FMT_ARGS] = { 0 }, next = 0, maxarg = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (f[i] != '%')
            continue;
        fmt_spec sp;
        if (!parse_spec(f, n, i, &next, &sp))
            break;
        if (sp.width == -2 && sp.width_arg < MAX_FMT_ARGS)
            sizes[sp.width_arg] = 1;
        if (sp.prec == -2 && sp.prec_arg < MAX_FMT_ARGS)
            sizes[sp.prec_arg] = 1;
        if (sp.pos >= 0 && sp.pos < MAX_FMT_ARGS)
            sizes[sp.pos] = spec_words(&sp);
        i = sp.end - 1;
    }
    unsigned offset[MAX_FMT_ARGS + 1];
    offset[0] = 0;
    for (int k = 0; k < MAX_FMT_ARGS; k++) {
        if (sizes[k])
            maxarg = k + 1;
        offset[k + 1] = offset[k] + (unsigned)(sizes[k] ? sizes[k] : 1);
    }
    (void)maxarg;
    next = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (f[i] != '%') {
            text_add_unit(out, f[i]);
            continue;
        }
        fmt_spec sp;
        if (!parse_spec(f, n, i, &next, &sp)) {
            text_add(out, f + i, n - i);
            break;
        }
        i = sp.end - 1;
        if (sp.conv == '%') {
            text_add_unit(out, '%');
            continue;
        }
        if (sp.pos < 0 || sp.pos >= MAX_FMT_ARGS)
            continue;
        unsigned w = offset[sp.pos];
        int width = sp.width, prec = sp.prec;
        if (width == -2) {
            width = (int32_t)fmt_word(a, offset[sp.width_arg]);
            if (width < 0) {
                strcat(sp.flags, "-");
                width = -width;
            }
        }
        if (prec == -2)
            prec = (int32_t)fmt_word(a, offset[sp.prec_arg]);
        char spec[48], piece[512];
        int sl = snprintf(spec, sizeof spec, "%%%s", sp.flags);
        if (width >= 0)
            sl += snprintf(spec + sl, sizeof spec - (size_t)sl, "%d", width);
        if (prec >= 0)
            sl += snprintf(spec + sl, sizeof spec - (size_t)sl, ".%d", prec);
        piece[0] = 0;
        switch (sp.conv) {
        case 'd': case 'i': case 'D': {
            long long v = sp.size == 2 ? (long long)((uint64_t)fmt_word(a, w) << 32 | fmt_word(a, w + 1))
                                       : (int32_t)fmt_word(a, w);
            if (sp.size == -1)
                v = (int16_t)v;
            else if (sp.size == -2)
                v = (int8_t)v;
            snprintf(spec + sl, sizeof spec - (size_t)sl, "lld");
            snprintf(piece, sizeof piece, spec, v);
            add_ascii(out, piece);
            break;
        }
        case 'u': case 'o': case 'x': case 'X': case 'U': case 'O': {
            unsigned long long v = sp.size == 2 ? (uint64_t)fmt_word(a, w) << 32 | fmt_word(a, w + 1) : fmt_word(a, w);
            if (sp.size == -1)
                v &= 0xFFFF;
            else if (sp.size == -2)
                v &= 0xFF;
            char conv = sp.conv == 'U' ? 'u' : sp.conv == 'O' ? 'o' : (char)sp.conv;
            snprintf(spec + sl, sizeof spec - (size_t)sl, "ll%c", conv);
            snprintf(piece, sizeof piece, spec, v);
            add_ascii(out, piece);
            break;
        }
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
            uint64_t bits = (uint64_t)fmt_word(a, w) << 32 | fmt_word(a, w + 1);
            double d;
            memcpy(&d, &bits, 8);
            snprintf(spec + sl, sizeof spec - (size_t)sl, "%c", (char)sp.conv);
            snprintf(piece, sizeof piece, spec, d);
            add_ascii(out, piece);
            break;
        }
        case 'p':
            snprintf(piece, sizeof piece, "0x%x", fmt_word(a, w));
            add_ascii(out, piece);
            break;
        case 'c': {
            uint8_t b = (uint8_t)fmt_word(a, w);
            uint16_t u = b < 128 ? b : (roman_table(), roman_hi[b - 128]);
            add_padded(out, &u, 1, &sp, width);
            break;
        }
        case 'C': {
            uint16_t u = (uint16_t)fmt_word(a, w);
            add_padded(out, &u, 1, &sp, width);
            break;
        }
        case 's': {
            gaddr p = fmt_word(a, w);
            if (!p) {
                const uint16_t null_s[] = { '(', 'n', 'u', 'l', 'l', ')' };
                add_padded(out, null_s, 6, &sp, width);
                break;
            }
            uint32_t len = 0;
            while (rd8(p + len) && (prec < 0 || len < (uint32_t)prec))
                len++;
            uint8_t *b = malloc(len + 1);
            uint16_t *u;
            uint32_t k;
            gget(b, p, len);
            if (cf_decode(system_encoding(), b, len, &u, &k) || cf_decode(kCFStringEncodingMacRoman, b, len, &u, &k)) {
                add_padded(out, u, k, &sp, width);
                free(u);
            }
            free(b);
            break;
        }
        case 'S': {
            gaddr p = fmt_word(a, w);
            uint32_t len = 0;
            while (p && rd16(p + 2 * len) && (prec < 0 || len < (uint32_t)prec))
                len++;
            uint16_t *u = malloc(sizeof *u * (len + 1));
            for (uint32_t k = 0; k < len; k++)
                u[k] = rd16(p + 2 * k);
            add_padded(out, u, len, &sp, width);
            free(u);
            break;
        }
        case '@': {
            gaddr ref = fmt_word(a, w);
            cf_text t = { 0 };
            if (ref)
                cf_format_describe(ref, &t);
            else
                add_ascii(&t, "(null)");
            uint32_t len = prec >= 0 && (uint32_t)prec < t.n ? (uint32_t)prec : t.n;
            add_padded(out, t.u, len, &sp, width);
            text_free(&t);
            break;
        }
        default:
            text_add_unit(out, '%');
            text_add_unit(out, sp.conv);
            break;
        }
    }
}

static gaddr create_formatted(gaddr alloc, gaddr format, const fmt_args *a)
{
    cf_string *f = str_of(format);
    if (!f)
        return 0;
    cf_text t = { 0 };
    format_into(&t, f->u, f->n, a);
    gaddr s = cfstr_new(alloc, t.u, t.n, false);
    text_free(&t);
    return s;
}

/* CFStringCreateWithFormat(alloc, formatOptions, format, ...) */
static void cf_CFStringCreateWithFormat(ppc_cpu *c)
{
    fmt_args a = { c, 0, 3 };
    RETURN(create_formatted(R(0), R(2), &a));
}

/* CFStringCreateWithFormatAndArguments(alloc, formatOptions, format, arguments) */
static void cf_CFStringCreateWithFormatAndArguments(ppc_cpu *c)
{
    fmt_args a = { c, R(3), 0 };
    RETURN(create_formatted(R(0), R(2), &a));
}

static void append_formatted(gaddr str, gaddr format, const fmt_args *a)
{
    cf_string *s = str_of(str), *f = str_of(format);
    if (!s || !f)
        return;
    cf_text t = { 0 };
    format_into(&t, f->u, f->n, a);
    str_replace(s, s->n, 0, t.u, t.n);
    text_free(&t);
}

/* CFStringAppendFormat(theString, formatOptions, format, ...) */
static void cf_CFStringAppendFormat(ppc_cpu *c)
{
    fmt_args a = { c, 0, 3 };
    append_formatted(R(0), R(2), &a);
    RETURN_VOID();
}

/* CFStringAppendFormatAndArguments(theString, formatOptions, format, arguments) */
static void cf_CFStringAppendFormatAndArguments(ppc_cpu *c)
{
    fmt_args a = { c, R(3), 0 };
    append_formatted(R(0), R(2), &a);
    RETURN_VOID();
}

/* ---------------------------------------------------------------------- */
/* Contents */

static void cf_CFStringGetLength(ppc_cpu *c)
{
    cf_string *s = str_of(R(0));
    RETURN(s ? s->n : 0);
}

static void cf_CFStringGetCharacterAtIndex(ppc_cpu *c)
{
    cf_string *s = str_of(R(0));
    RETURN(s && R(1) < s->n ? s->u[R(1)] : 0);
}

/* CFStringGetCharacters(theString, range, buffer) */
static void cf_CFStringGetCharacters(ppc_cpu *c)
{
    cf_string *s = str_of(R(0));
    gaddr buf = R(3);
    if (range_ok(s, R(1), R(2)) && buf)
        for (uint32_t i = 0; i < R(2); i++)
            wr16(buf + 2 * i, s->u[R(1) + i]);
    RETURN_VOID();
}

/* The whole string in an encoding, if it all converts: malloc'd. */
static uint8_t *whole_bytes(cf_string *s, uint32_t enc, size_t *n)
{
    uint8_t *b;
    uint32_t used;
    if (!cf_encode(enc, s->u, s->n, 0, &b, n, &used)) {
        free(b);
        return NULL;
    }
    return b;
}

/* CFStringGetPascalString(theString, buffer, bufferSize, encoding) */
static void cf_CFStringGetPascalString(ppc_cpu *c)
{
    cf_string *s = str_of(R(0));
    gaddr buf = R(1);
    int32_t size = (int32_t)R(2);
    size_t n;
    uint8_t *b = s && buf && size > 0 ? whole_bytes(s, R(3), &n) : NULL;
    bool ok = b && n <= 255 && (int32_t)n < size;
    if (ok) {
        wr8(buf, (uint8_t)n);
        gput(buf + 1, b, (uint32_t)n);
    }
    free(b);
    RETURN(ok);
}

/* CFStringGetCString(theString, buffer, bufferSize, encoding) */
static void cf_CFStringGetCString(ppc_cpu *c)
{
    cf_string *s = str_of(R(0));
    gaddr buf = R(1);
    int32_t size = (int32_t)R(2);
    size_t n;
    uint8_t *b = s && buf && size > 0 ? whole_bytes(s, R(3), &n) : NULL;
    bool ok = b && (int32_t)n < size;
    if (ok) {
        gput(buf, b, (uint32_t)n);
        wr8(buf + (uint32_t)n, 0);
    }
    free(b);
    RETURN(ok);
}

/* Whether the bytes a NoCopy call gave are the string in this encoding. */
static bool src8_matches(cf_string *s, uint32_t enc)
{
    if (!s->src8 || s->mut)
        return false;
    if (enc == s->enc8)
        return true;
    if (enc != kCFStringEncodingASCII)
        return false;
    for (uint32_t i = 0; i < s->n; i++)
        if (s->u[i] >= 128)
            return false;
    return true;
}

/* CFStringGetPascalStringPtr(theString, encoding): only an immutable string
 * made from 8-bit text, in that encoding. */
static void cf_CFStringGetPascalStringPtr(ppc_cpu *c)
{
    cf_string *s = str_of(R(0));
    if (!s || s->mut || s->wide || s->n > 255)
        RETURN(0);
    if (src8_matches(s, R(1)) && s->src8_pascal)
        RETURN(s->src8);
    if (s->pptr && s->pptr_enc == R(1))
        RETURN(s->pptr);
    if (R(1) != s->enc8)
        RETURN(0);
    size_t n;
    uint8_t *b = whole_bytes(s, R(1), &n);
    if (!b || n > 255) {
        free(b);
        RETURN(0);
    }
    if (s->pptr)
        tb_DisposePtr(s->pptr);
    s->pptr = tb_NewPtr((int32_t)n + 1, false, false);
    if (s->pptr) {
        wr8(s->pptr, (uint8_t)n);
        gput(s->pptr + 1, b, (uint32_t)n);
        s->pptr_enc = R(1);
    }
    free(b);
    RETURN(s->pptr);
}

/* CFStringGetCStringPtr(theString, encoding) */
static void cf_CFStringGetCStringPtr(ppc_cpu *c)
{
    cf_string *s = str_of(R(0));
    if (!s || s->mut || s->wide)
        RETURN(0);
    if (src8_matches(s, R(1)) && !s->src8_pascal)
        RETURN(s->src8);
    if (s->cptr && s->cptr_enc == R(1))
        RETURN(s->cptr);
    if (R(1) != s->enc8)
        RETURN(0);
    size_t n;
    uint8_t *b = whole_bytes(s, R(1), &n);
    if (!b || memchr(b, 0, n)) {
        free(b);
        RETURN(0);
    }
    if (s->cptr)
        tb_DisposePtr(s->cptr);
    s->cptr = tb_NewPtr((int32_t)n + 1, false, false);
    if (s->cptr) {
        gput(s->cptr, b, (uint32_t)n);
        wr8(s->cptr + (uint32_t)n, 0);
        s->cptr_enc = R(1);
    }
    free(b);
    RETURN(s->cptr);
}

/* CFStringGetCharactersPtr(theString): a string made from UTF-16. */
static void cf_CFStringGetCharactersPtr(ppc_cpu *c)
{
    cf_string *s = str_of(R(0));
    if (!s)
        RETURN(0);
    if (s->ext)
        RETURN(s->ext);
    if (s->mut || !s->wide)
        RETURN(0);
    if (s->src16)
        RETURN(s->src16);
    if (!s->uptr && s->n) {
        s->uptr = tb_NewPtr((int32_t)(s->n * 2), false, false);
        for (uint32_t i = 0; s->uptr && i < s->n; i++)
            wr16(s->uptr + 2 * i, s->u[i]);
    }
    RETURN(s->uptr);
}

/* CFStringGetBytes(theString, range, encoding, lossByte, isExternalRepresentation,
 *                  buffer, maxBufLen, *usedBufLen): the characters converted. */
static void cf_CFStringGetBytes(ppc_cpu *c)
{
    cf_string *s = str_of(R(0));
    uint32_t loc = R(1), len = R(2), enc = R(3);
    uint8_t loss = (uint8_t)R(4);
    bool ext = (R(5) & 0xFF) != 0;
    gaddr buf = R(6);
    uint32_t max = R(7);
    gaddr used_p = cf_arg(c, 8);
    if (!range_ok(s, loc, len) || !encoding_available(enc)) {
        if (used_p)
            wr32(used_p, 0);
        RETURN(0);
    }
    size_t room = buf ? max : (size_t)-1, k = 0;
    uint8_t *out = malloc((size_t)len * 8 + 4);
    if (!out)
        RETURN(0);
    if (ext && is_unicode16(enc) && room >= 2) {
        out[0] = 0xFE, out[1] = 0xFF;
        k = 2;
    }
    uint32_t took = 0;
    k += encode_chars(enc, s->u + loc, len, loss, out + k, room - k, &took);
    if (buf)
        gput(buf, out, (uint32_t)k);
    if (used_p)
        wr32(used_p, (uint32_t)k);
    free(out);
    RETURN(took);
}

/* CFStringCreateWithBytes(alloc, bytes, numBytes, encoding, isExternalRepresentation):
 * an external UTF-16 representation starts with its byte order mark. */
static void cf_CFStringCreateWithBytes(ppc_cpu *c)
{
    gaddr b = R(1);
    uint32_t n = R(2), enc = R(3);
    if (is_unicode16(enc) && n >= 2 && rd16(b) == 0xFEFF) {
        b += 2, n -= 2;
    } else if (is_unicode16(enc) && n >= 2 && rd16(b) == 0xFFFE) { /* little-endian */
        uint16_t *u = malloc(sizeof *u * (n / 2 + 1));
        for (uint32_t i = 0; u && i < n / 2 - 1; i++)
            u[i] = (uint16_t)(rd8(b + 2 + 2 * i) | rd8(b + 3 + 2 * i) << 8);
        gaddr r = u ? cfstr_new(R(0), u, n / 2 - 1, false) : 0;
        free(u);
        RETURN(r);
    }
    cf_string *s = b || !n ? str_from_bytes(R(0), b, n, enc, false) : NULL;
    RETURN(s ? s->o.ref : 0);
}

/* CFStringCreateFromExternalRepresentation(alloc, data, encoding) */
static void cf_CFStringCreateFromExternalRepresentation(ppc_cpu *c)
{
    uint32_t n;
    gaddr b = cfdata_bytes(R(1), &n);
    if (!b && n)
        RETURN(0);
    ppc_cpu sub = *c;
    sub.gpr[3] = R(0), sub.gpr[4] = b, sub.gpr[5] = n, sub.gpr[6] = R(2), sub.gpr[7] = 1;
    cf_CFStringCreateWithBytes(&sub);
    RETURN(sub.gpr[3]);
}

/* CFStringCreateExternalRepresentation(alloc, theString, encoding, lossByte):
 * UTF-16 with its byte order mark. */
static void cf_CFStringCreateExternalRepresentation(ppc_cpu *c)
{
    cf_string *s = str_of(R(1));
    if (!s || !encoding_available(R(2)))
        RETURN(0);
    uint8_t *b = malloc((size_t)s->n * 8 + 4);
    size_t k = 0;
    if (!b)
        RETURN(0);
    if (is_unicode16(R(2)))
        b[0] = 0xFE, b[1] = 0xFF, k = 2;
    uint32_t took = 0;
    k += encode_chars(R(2), s->u, s->n, (uint8_t)R(3), b + k, (size_t)s->n * 8, &took);
    gaddr d = took == s->n ? cfdata_new(R(0), b, (uint32_t)k, false) : 0;
    free(b);
    RETURN(d);
}

/* Whether the system's 8-bit encoding holds the whole string. */
static bool fits_system(cf_string *s)
{
    uint32_t enc = system_encoding();
    for (uint32_t i = 0; i < s->n; i++) {
        uint8_t b[16];
        if (encode_char(enc, s->u + i, 1, b) != 1)
            return false;
    }
    return true;
}

static void cf_CFStringGetSmallestEncoding(ppc_cpu *c)
{
    cf_string *s = str_of(R(0));
    RETURN(s && fits_system(s) ? system_encoding() : kCFStringEncodingUnicode);
}

static void cf_CFStringGetFastestEncoding(ppc_cpu *c)
{
    cf_string *s = str_of(R(0));
    RETURN(s && !s->wide && fits_system(s) ? system_encoding() : kCFStringEncodingUnicode);
}

static void cf_CFStringGetSystemEncoding(ppc_cpu *c) { RETURN(system_encoding()); }

/* CFStringGetMaximumSizeForEncoding(length, encoding) */
static void cf_CFStringGetMaximumSizeForEncoding(ppc_cpu *c)
{
    uint32_t n = R(0), enc = R(1), base = enc & 0xFFFF;
    if (enc == kCFStringEncodingUnicode)
        RETURN(n * 2);
    if (enc == kCFStringEncodingUTF8)
        RETURN(n * 3);
    if (enc == kCFStringEncodingNonLossyASCII)
        RETURN(n * 6);
    if ((base >= 1 && base <= 3) || base == 25 || (base >= 0x420 && base <= 0x423) || (base >= 0x620 && base <= 0x6FF) ||
        (base >= 0x920 && base <= 0x940) || (base >= 0xA01 && base <= 0xA06) || (enc & 0xFFFF) == 0x0510)
        RETURN(n * 2);
    if (base >= 0x800 && base <= 0x8FF)
        RETURN(n * 8);
    RETURN(n);
}

/* ---------------------------------------------------------------------- */
/* Comparing and finding */

static uint16_t fold(uint16_t c, uint32_t opts) { return opts & kCFCompareCaseInsensitive ? to_lower(c) : c; }

static int compare_units(const uint16_t *a, uint32_t na, const uint16_t *b, uint32_t nb, uint32_t opts)
{
    uint32_t i = 0, j = 0;
    while (i < na && j < nb) {
        if (opts & kCFCompareNumerically && is_digit(a[i]) && is_digit(b[j])) {
            /* Runs of digits by their values. */
            uint32_t ei = i, ej = j;
            while (ei < na && is_digit(a[ei]))
                ei++;
            while (ej < nb && is_digit(b[ej]))
                ej++;
            uint32_t si = i, sj = j;
            while (si + 1 < ei && digit_value(a[si]) == 0)
                si++;
            while (sj + 1 < ej && digit_value(b[sj]) == 0)
                sj++;
            if (ei - si != ej - sj)
                return ei - si < ej - sj ? kCFCompareLessThan : kCFCompareGreaterThan;
            for (; si < ei; si++, sj++)
                if (digit_value(a[si]) != digit_value(b[sj]))
                    return digit_value(a[si]) < digit_value(b[sj]) ? kCFCompareLessThan : kCFCompareGreaterThan;
            i = ei, j = ej;
            continue;
        }
        uint16_t x = fold(a[i], opts), y = fold(b[j], opts);
        if (x != y)
            return x < y ? kCFCompareLessThan : kCFCompareGreaterThan;
        i++, j++;
    }
    if (i < na)
        return kCFCompareGreaterThan;
    if (j < nb)
        return kCFCompareLessThan;
    return kCFCompareEqualTo;
}

/* CFStringCompareWithOptions(theString1, theString2, rangeToCompare, compareOptions) */
static void cf_CFStringCompareWithOptions(ppc_cpu *c)
{
    cf_string *a = str_of(R(0)), *b = str_of(R(1));
    if (!range_ok(a, R(2), R(3)) || !b)
        RETURN(kCFCompareEqualTo);
    RETURN((uint32_t)compare_units(a->u + R(2), R(3), b->u, b->n, R(4)));
}

/* CFStringCompare(theString1, theString2, compareOptions) */
static void cf_CFStringCompare(ppc_cpu *c)
{
    cf_string *a = str_of(R(0)), *b = str_of(R(1));
    if (!a || !b)
        RETURN(kCFCompareEqualTo);
    RETURN((uint32_t)compare_units(a->u, a->n, b->u, b->n, R(2)));
}

static bool match_at(const uint16_t *s, const uint16_t *f, uint32_t n, uint32_t opts)
{
    for (uint32_t i = 0; i < n; i++)
        if (fold(s[i], opts) != fold(f[i], opts))
            return false;
    return true;
}

/* Find f in s[loc, loc+len): its index, or -1. */
static int64_t find_units(const uint16_t *s, uint32_t loc, uint32_t len, const uint16_t *f, uint32_t fn, uint32_t opts)
{
    if (!fn || fn > len)
        return -1;
    uint32_t last = loc + len - fn;
    if (opts & kCFCompareBackwards) {
        if (opts & kCFCompareAnchored)
            return match_at(s + last, f, fn, opts) ? (int64_t)last : -1;
        for (uint32_t i = last + 1; i-- > loc;)
            if (match_at(s + i, f, fn, opts))
                return i;
        return -1;
    }
    if (opts & kCFCompareAnchored)
        return match_at(s + loc, f, fn, opts) ? (int64_t)loc : -1;
    for (uint32_t i = loc; i <= last; i++)
        if (match_at(s + i, f, fn, opts))
            return i;
    return -1;
}

/* CFStringFindWithOptions(theString, stringToFind, rangeToSearch, searchOptions, *result) */
static void cf_CFStringFindWithOptions(ppc_cpu *c)
{
    cf_string *s = str_of(R(0)), *f = str_of(R(1));
    if (!range_ok(s, R(2), R(3)) || !f)
        RETURN(false);
    int64_t at = find_units(s->u, R(2), R(3), f->u, f->n, R(4));
    if (at >= 0 && R(5)) {
        wr32(R(5), (uint32_t)at);
        wr32(R(5) + 4, f->n);
    }
    RETURN(at >= 0);
}

/* CFStringCreateArrayWithFindResults(alloc, theString, stringToFind, rangeToSearch,
 *                                    compareOptions): an array of pointers to CFRanges
 * (in one block the array keeps), or NULL if there are none. */
void cfarray_own_block(gaddr array, gaddr block);
static void cf_CFStringCreateArrayWithFindResults(ppc_cpu *c)
{
    cf_string *s = str_of(R(1)), *f = str_of(R(2));
    uint32_t loc = R(3), len = R(4), opts = R(5);
    if (!range_ok(s, loc, len) || !f || !f->n)
        RETURN(0);
    uint32_t found[1024], n = 0, end = loc + len;
    bool back = opts & kCFCompareBackwards;
    while (n < 1024) {
        int64_t at = find_units(s->u, loc, end - loc, f->u, f->n, opts);
        if (at < 0)
            break;
        found[n++] = (uint32_t)at;
        if (back)
            end = (uint32_t)at;
        else
            loc = (uint32_t)at + f->n;
        if (opts & kCFCompareAnchored)
            break;
    }
    if (!n)
        RETURN(0);
    gaddr block = cf_allocate(R(0), 8 * n);
    if (!block)
        RETURN(0);
    uint32_t *vals = malloc(sizeof *vals * n);
    for (uint32_t i = 0; i < n; i++) {
        wr32(block + 8 * i, found[i]);
        wr32(block + 8 * i + 4, f->n);
        vals[i] = block + 8 * i;
    }
    gaddr a = cfarray_new(R(0), NULL, 0, true);
    cfarray_own_block(a, block);
    for (uint32_t i = 0; i < n; i++)
        cfarray_append(a, vals[i]);
    free(vals);
    RETURN(a);
}

/* CFStringFind(theString, stringToFind, compareOptions): a CFRange through r3. */
static void cf_CFStringFind(ppc_cpu *c)
{
    gaddr res = R(0);
    cf_string *s = str_of(R(1)), *f = str_of(R(2));
    int64_t at = s && f ? find_units(s->u, 0, s->n, f->u, f->n, R(3)) : -1;
    if (res) {
        wr32(res, at >= 0 ? (uint32_t)at : (uint32_t)kCFNotFound);
        wr32(res + 4, at >= 0 ? f->n : 0);
    }
    RETURN(res);
}

static void cf_CFStringHasPrefix(ppc_cpu *c)
{
    cf_string *s = str_of(R(0)), *p = str_of(R(1));
    RETURN(s && p && p->n && p->n <= s->n && !memcmp(s->u, p->u, sizeof *p->u * p->n));
}

static void cf_CFStringHasSuffix(ppc_cpu *c)
{
    cf_string *s = str_of(R(0)), *p = str_of(R(1));
    RETURN(s && p && p->n && p->n <= s->n && !memcmp(s->u + s->n - p->n, p->u, sizeof *p->u * p->n));
}

/* The end of a line separator at i (0 if none there). */
static uint32_t separator_len(const uint16_t *u, uint32_t n, uint32_t i)
{
    if (u[i] == '\r')
        return i + 1 < n && u[i + 1] == '\n' ? 2 : 1;
    return u[i] == '\n' || u[i] == 0x85 || u[i] == 0x2028 || u[i] == 0x2029 ? 1 : 0;
}

/* CFStringGetLineBounds(theString, range, *lineBeginIndex, *lineEndIndex, *contentsEndIndex) */
static void cf_CFStringGetLineBounds(ppc_cpu *c)
{
    cf_string *s = str_of(R(0));
    uint32_t loc = R(1), len = R(2);
    if (!range_ok(s, loc, len))
        RETURN_VOID();
    if (R(3)) {
        uint32_t b = loc;
        while (b > 0 && !separator_len(s->u, s->n, b - 1))
            b--;
        wr32(R(3), b);
    }
    uint32_t e = len ? loc + len - 1 : loc;
    if (e > 0 && e < s->n && s->u[e] == '\n' && s->u[e - 1] == '\r')
        e--;
    while (e < s->n && !separator_len(s->u, s->n, e))
        e++;
    uint32_t contents = e, after = e < s->n ? e + separator_len(s->u, s->n, e) : e;
    if (R(4))
        wr32(R(4), after);
    if (R(5))
        wr32(R(5), contents);
    RETURN_VOID();
}

/* CFStringCreateByCombiningStrings(alloc, theArray, separatorString) */
static void cf_CFStringCreateByCombiningStrings(ppc_cpu *c)
{
    cf_string *sep = str_of(R(2));
    uint32_t n = cfarray_count(R(1));
    cf_text t = { 0 };
    for (uint32_t i = 0; i < n; i++) {
        if (i && sep)
            text_add(&t, sep->u, sep->n);
        cf_string *s = str_of(cfarray_get(R(1), i));
        if (s)
            text_add(&t, s->u, s->n);
    }
    gaddr r = cfstr_new(R(0), t.u, t.n, false);
    text_free(&t);
    RETURN(r);
}

/* CFStringCreateArrayBySeparatingStrings(alloc, theString, separatorString) */
static void cf_CFStringCreateArrayBySeparatingStrings(ppc_cpu *c)
{
    cf_string *s = str_of(R(1)), *sep = str_of(R(2));
    if (!s || !sep)
        RETURN(0);
    gaddr a = cfarray_new(R(0), NULL, 0, false);
    uint32_t start = 0;
    for (;;) {
        int64_t at = start <= s->n ? find_units(s->u, start, s->n - start, sep->u, sep->n, 0) : -1;
        uint32_t end = at >= 0 ? (uint32_t)at : s->n;
        gaddr piece = cfstr_new(R(0), s->u + start, end - start, false);
        cfarray_append(a, piece);
        cf_release(piece);
        if (at < 0)
            break;
        start = end + sep->n;
    }
    RETURN(a);
}

/* An ASCII copy of a string, for numbers (malloc'd). */
static char *ascii_copy(cf_string *s)
{
    char *b = malloc(s->n + 1);
    if (!b)
        return NULL;
    for (uint32_t i = 0; i < s->n; i++) {
        int d = digit_value(s->u[i]);
        b[i] = s->u[i] < 128 ? (char)s->u[i] : d >= 0 ? (char)('0' + d) : '?';
    }
    b[s->n] = 0;
    return b;
}

/* CFStringGetIntValue(str): 0 if it isn't a number; the extremes past them. */
static void cf_CFStringGetIntValue(ppc_cpu *c)
{
    cf_string *s = str_of(R(0));
    char *b = s ? ascii_copy(s) : NULL;
    if (!b)
        RETURN(0);
    char *p = b;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    long long v = strtoll(p, NULL, 10);
    free(b);
    RETURN((uint32_t)(int32_t)(v > 2147483647LL ? 2147483647LL : v < -2147483648LL ? -2147483648LL : v));
}

/* CFStringGetDoubleValue(str) */
static void cf_CFStringGetDoubleValue(ppc_cpu *c)
{
    cf_string *s = str_of(R(0));
    char *b = s ? ascii_copy(s) : NULL;
    double d = b ? strtod(b, NULL) : 0.0;
    free(b);
    cf_return_double(c, d);
}

/* ---------------------------------------------------------------------- */
/* Changing */

static void cf_CFStringAppend(ppc_cpu *c)
{
    cf_string *s = str_of(R(0)), *a = str_of(R(1));
    if (s && a)
        str_replace(s, s->n, 0, a->u, a->n);
    RETURN_VOID();
}

/* CFStringAppendCharacters(theString, chars, numChars) */
static void cf_CFStringAppendCharacters(ppc_cpu *c)
{
    cf_string *s = str_of(R(0));
    if (!s || !R(1) || R(2) > 0x800000)
        RETURN_VOID();
    uint16_t *u = malloc(sizeof *u * (R(2) + 1));
    for (uint32_t i = 0; u && i < R(2); i++)
        u[i] = rd16(R(1) + 2 * i);
    if (u)
        str_replace(s, s->n, 0, u, R(2));
    free(u);
    RETURN_VOID();
}

static void append_bytes(cf_string *s, gaddr b, uint32_t n, uint32_t enc)
{
    uint8_t *bytes = malloc(n + 1);
    uint16_t *u;
    uint32_t k;
    if (!bytes)
        return;
    gget(bytes, b, n);
    if (cf_decode(enc, bytes, n, &u, &k)) {
        str_replace(s, s->n, 0, u, k);
        free(u);
    }
    free(bytes);
}

/* CFStringAppendPascalString(theString, pStr, encoding) */
static void cf_CFStringAppendPascalString(ppc_cpu *c)
{
    cf_string *s = str_of(R(0));
    if (s && R(1))
        append_bytes(s, R(1) + 1, rd8(R(1)), R(2));
    RETURN_VOID();
}

/* CFStringAppendCString(theString, cStr, encoding) */
static void cf_CFStringAppendCString(ppc_cpu *c)
{
    cf_string *s = str_of(R(0));
    if (s && R(1))
        append_bytes(s, R(1), cstr_len(R(1)), R(2));
    RETURN_VOID();
}

/* CFStringInsert(str, idx, insertedStr) */
static void cf_CFStringInsert(ppc_cpu *c)
{
    cf_string *s = str_of(R(0)), *a = str_of(R(2));
    if (s && a && R(1) <= s->n)
        str_replace(s, R(1), 0, a->u, a->n);
    RETURN_VOID();
}

/* CFStringDelete(theString, range) */
static void cf_CFStringDelete(ppc_cpu *c)
{
    cf_string *s = str_of(R(0));
    if (range_ok(s, R(1), R(2)))
        str_replace(s, R(1), R(2), NULL, 0);
    RETURN_VOID();
}

/* CFStringReplace(theString, range, replacement) */
static void cf_CFStringReplace(ppc_cpu *c)
{
    cf_string *s = str_of(R(0)), *a = str_of(R(3));
    if (range_ok(s, R(1), R(2)) && a)
        str_replace(s, R(1), R(2), a->u, a->n);
    RETURN_VOID();
}

/* CFStringReplaceAll(theString, replacement) */
static void cf_CFStringReplaceAll(ppc_cpu *c)
{
    cf_string *s = str_of(R(0)), *a = str_of(R(1));
    if (s && a)
        str_replace(s, 0, s->n, a->u, a->n);
    RETURN_VOID();
}

/* CFStringPad(theString, padString, length, indexIntoPad): cut to length,
 * or filled to it from the pad string, starting at indexIntoPad. */
static void cf_CFStringPad(ppc_cpu *c)
{
    cf_string *s = str_of(R(0)), *pad = str_of(R(1));
    uint32_t len = R(2), at = R(3);
    if (!s || len > 0x800000)
        RETURN_VOID();
    if (len <= s->n) {
        str_replace(s, len, s->n - len, NULL, 0);
        RETURN_VOID();
    }
    if (!pad || !pad->n)
        RETURN_VOID();
    uint32_t add = len - s->n;
    uint16_t *u = malloc(sizeof *u * add);
    for (uint32_t i = 0; u && i < add; i++)
        u[i] = pad->u[(at + i) % pad->n];
    if (u)
        str_replace(s, s->n, 0, u, add);
    free(u);
    RETURN_VOID();
}

/* CFStringTrim(theString, trimString): the trim string taken off both ends,
 * as often as it is there. */
static void cf_CFStringTrim(ppc_cpu *c)
{
    cf_string *s = str_of(R(0)), *t = str_of(R(1));
    if (!s || !t || !t->n)
        RETURN_VOID();
    uint32_t b = 0, e = s->n;
    while (e - b >= t->n && match_at(s->u + b, t->u, t->n, 0))
        b += t->n;
    while (e - b >= t->n && match_at(s->u + e - t->n, t->u, t->n, 0))
        e -= t->n;
    str_replace(s, e, s->n - e, NULL, 0);
    str_replace(s, 0, b, NULL, 0);
    RETURN_VOID();
}

static void cf_CFStringTrimWhitespace(ppc_cpu *c)
{
    cf_string *s = str_of(R(0));
    if (!s)
        RETURN_VOID();
    uint32_t b = 0, e = s->n;
    while (b < e && (is_whitespace(s->u[b]) || is_newline(s->u[b])))
        b++;
    while (e > b && (is_whitespace(s->u[e - 1]) || is_newline(s->u[e - 1])))
        e--;
    str_replace(s, e, s->n - e, NULL, 0);
    str_replace(s, 0, b, NULL, 0);
    RETURN_VOID();
}

static void change_case(cf_string *s, int how)
{
    if (!s)
        return;
    bool word_start = true;
    for (uint32_t i = 0; i < s->n; i++) {
        uint16_t ch = s->u[i];
        if (how == 0)
            s->u[i] = to_lower(ch);
        else if (how == 1)
            s->u[i] = to_upper(ch);
        else
            s->u[i] = word_start ? to_upper(ch) : to_lower(ch);
        word_start = !(is_letter(ch) || is_digit(ch) || ch == '\'');
    }
    str_write_back(s);
}

static void cf_CFStringLowercase(ppc_cpu *c)
{
    change_case(str_of(R(0)), 0);
    RETURN_VOID();
}

static void cf_CFStringUppercase(ppc_cpu *c)
{
    change_case(str_of(R(0)), 1);
    RETURN_VOID();
}

static void cf_CFStringCapitalize(ppc_cpu *c)
{
    change_case(str_of(R(0)), 2);
    RETURN_VOID();
}

/* ---------------------------------------------------------------------- */
/* Encodings' names and numbers */

static void cf_CFStringIsEncodingAvailable(ppc_cpu *c) { RETURN(encoding_available(R(0))); }

/* CFStringGetListOfAvailableEncodings(): ended by kCFStringEncodingInvalidId. */
static void cf_CFStringGetListOfAvailableEncodings(ppc_cpu *c)
{
    static const uint32_t own[] = { kCFStringEncodingMacRoman, kCFStringEncodingUnicode, kCFStringEncodingUTF8,
                                    kCFStringEncodingASCII, kCFStringEncodingISOLatin1,
                                    kCFStringEncodingNonLossyASCII };
    uint32_t list[160], more[150];
    unsigned n = 0, m = tec_available_encodings(more, 150);
    for (unsigned i = 0; i < NELEM(own); i++)
        list[n++] = own[i];
    for (unsigned i = 0; i < m && n < 159; i++) {
        bool dup = false;
        for (unsigned j = 0; j < n; j++)
            dup |= list[j] == more[i];
        if (!dup)
            list[n++] = more[i];
    }
    list[n++] = kCFStringEncodingInvalidId;
    gaddr p = cf_data_cell("(available encodings)", 4 * n);
    for (unsigned i = 0; p && i < n; i++)
        wr32(p + 4 * i, list[i]);
    RETURN(p);
}

/* CFStringGetNameOfEncoding(encoding) */
static void cf_CFStringGetNameOfEncoding(ppc_cpu *c)
{
    uint32_t e = R(0);
    static const char *const formats[] = { "UTF-16", "UTF-7", "UTF-8", "UTF-32" };
    char buf[64];
    if (!encoding_available(e))
        RETURN(0);
    if (e == kCFStringEncodingNonLossyASCII)
        RETURN(cfstr_from_utf8("Non-lossy ASCII"));
    if ((e & 0xFF00) == 0x0100 && (e >> 26) < 4) {
        snprintf(buf, sizeof buf, "Unicode (%s)", formats[e >> 26]);
        RETURN(cfstr_from_utf8(buf));
    }
    const char *name = tec_encoding_name(e);
    RETURN(name ? cfstr_from_utf8(name) : 0);
}

/* NSStringEncoding: the few OpenStep had, then the rest with the high bit. */
static const uint32_t ns_encodings[][2] = {
    { 1, kCFStringEncodingASCII }, { 2, kCFStringEncodingNextStepLatin }, { 3, 0x0920 },
    { 4, kCFStringEncodingUTF8 }, { 5, kCFStringEncodingISOLatin1 }, { 6, 33 },
    { 7, kCFStringEncodingNonLossyASCII }, { 8, 0x0A01 }, { 9, 0x0202 }, { 10, kCFStringEncodingUnicode },
    { 11, 0x0502 }, { 12, 0x0500 }, { 13, 0x0503 }, { 14, 0x0504 }, { 15, 0x0501 }, { 21, 0x0820 },
    { 30, kCFStringEncodingMacRoman },
};

static void cf_CFStringConvertEncodingToNSStringEncoding(ppc_cpu *c)
{
    for (unsigned i = 0; i < NELEM(ns_encodings); i++)
        if (ns_encodings[i][1] == R(0))
            RETURN(ns_encodings[i][0]);
    RETURN(0x80000000u | R(0));
}

static void cf_CFStringConvertNSStringEncodingToEncoding(ppc_cpu *c)
{
    for (unsigned i = 0; i < NELEM(ns_encodings); i++)
        if (ns_encodings[i][0] == R(0))
            RETURN(ns_encodings[i][1]);
    RETURN(R(0) & 0x80000000u ? R(0) & 0x7FFFFFFFu : (uint32_t)kCFStringEncodingInvalidId);
}

static const uint32_t codepages[][2] = {
    { 0, 10000 }, { 1, 10001 }, { 2, 10002 }, { 3, 10003 }, { 4, 10004 }, { 5, 10005 }, { 6, 10006 },
    { 7, 10007 }, { 21, 10021 }, { 25, 10008 }, { 29, 10029 }, { 35, 10081 }, { 36, 10082 }, { 37, 10079 },
    { 38, 10010 }, { 0x98, 10017 },
    { 0x0201, 28591 }, { 0x0202, 28592 }, { 0x0203, 28593 }, { 0x0204, 28594 }, { 0x0205, 28595 },
    { 0x0206, 28596 }, { 0x0207, 28597 }, { 0x0208, 28598 }, { 0x0209, 28599 }, { 0x020D, 28603 },
    { 0x020F, 28605 },
    { 0x0400, 437 }, { 0x0405, 737 }, { 0x0406, 775 }, { 0x0410, 850 }, { 0x0411, 851 }, { 0x0412, 852 },
    { 0x0413, 855 }, { 0x0414, 857 }, { 0x0415, 860 }, { 0x0416, 861 }, { 0x0417, 862 }, { 0x0418, 863 },
    { 0x0419, 864 }, { 0x041A, 865 }, { 0x041B, 866 }, { 0x041C, 869 }, { 0x041D, 874 }, { 0x0420, 932 },
    { 0x0421, 936 }, { 0x0422, 949 }, { 0x0423, 950 },
    { 0x0500, 1252 }, { 0x0501, 1250 }, { 0x0502, 1251 }, { 0x0503, 1253 }, { 0x0504, 1254 }, { 0x0505, 1255 },
    { 0x0506, 1256 }, { 0x0507, 1257 }, { 0x0508, 1258 }, { 0x0510, 1361 },
    { 0x0600, 20127 }, { 0x0630, 20936 }, { 0x0820, 50220 }, { 0x0840, 50225 }, { 0x0830, 50227 },
    { 0x0920, 51932 }, { 0x0930, 51936 }, { 0x0940, 51949 }, { 0x0A02, 20866 }, { 0x0A05, 52936 },
    { kCFStringEncodingUTF8, 65001 }, { kCFStringEncodingUnicode, 1200 },
};

static void cf_CFStringConvertEncodingToWindowsCodepage(ppc_cpu *c)
{
    for (unsigned i = 0; i < NELEM(codepages); i++)
        if (codepages[i][0] == R(0))
            RETURN(codepages[i][1]);
    RETURN(kCFStringEncodingInvalidId);
}

static void cf_CFStringConvertWindowsCodepageToEncoding(ppc_cpu *c)
{
    for (unsigned i = 0; i < NELEM(codepages); i++)
        if (codepages[i][1] == R(0))
            RETURN(codepages[i][0]);
    RETURN(kCFStringEncodingInvalidId);
}

uint32_t cf_encoding_of_iana(const char *name)
{
    uint32_t e;
    return name && tec_encoding_of_internet_name(name, &e) ? e : (uint32_t)kCFStringEncodingInvalidId;
}

/* CFStringConvertIANACharSetNameToEncoding(theString) */
static void cf_CFStringConvertIANACharSetNameToEncoding(ppc_cpu *c)
{
    char *name = cfstr_utf8(R(0));
    uint32_t e = kCFStringEncodingInvalidId;
    if (name && !tec_encoding_of_internet_name(name, &e))
        e = kCFStringEncodingInvalidId;
    free(name);
    RETURN(e);
}

/* CFStringConvertEncodingToIANACharSetName(encoding) */
static void cf_CFStringConvertEncodingToIANACharSetName(ppc_cpu *c)
{
    const char *name = tec_internet_name(R(0));
    RETURN(name ? cfstr_constant(name) : 0);
}

/* CFStringGetMostCompatibleMacStringEncoding(encoding) */
static void cf_CFStringGetMostCompatibleMacStringEncoding(ppc_cpu *c)
{
    uint32_t e = R(0), b = e & 0xFFFF;
    if (e <= 0xFF)
        RETURN(e);
    switch (b) {
    case 0x0100: case 0x0201: case 0x0500: case 0x0600: case 0x0B01: case 0x0BFF: case 0x0410: case 0x0400:
    case 0x020F: case 0x0A04:
        RETURN(0);
    case 0x0202: case 0x0501: case 0x0412: case 0x0204: case 0x020D:
        RETURN(29);
    case 0x0205: case 0x0502: case 0x0413: case 0x041B: case 0x0A02:
        RETURN(7);
    case 0x0A08:
        RETURN(0x98);
    case 0x0207: case 0x0503: case 0x0405: case 0x0411: case 0x041C:
        RETURN(6);
    case 0x0208: case 0x0505: case 0x0417:
        RETURN(5);
    case 0x0206: case 0x0506: case 0x0419:
        RETURN(4);
    case 0x0209: case 0x0504: case 0x0414:
        RETURN(35);
    case 0x020B: case 0x041D:
        RETURN(21);
    case 0x0416:
        RETURN(37);
    case 0x0A01: case 0x0920: case 0x0820: case 0x0821: case 0x0420: case 0x0620: case 0x0621: case 0x0622:
        RETURN(1);
    case 0x0930: case 0x0A05: case 0x0421: case 0x0630: case 0x0631: case 0x0632: case 0x0830:
        RETURN(25);
    case 0x0A03: case 0x0931: case 0x0423: case 0x0A06:
        RETURN(2);
    case 0x0940: case 0x0840: case 0x0422: case 0x0640:
        RETURN(3);
    default:
        RETURN(kCFStringEncodingInvalidId);
    }
}

/* ---------------------------------------------------------------------- */
/* Debugging */

/* CFShow(obj): to the log (on a Mac, to a debugger's console if any). */
static void cf_CFShow(ppc_cpu *c)
{
    cf_text t = { 0 };
    if (R(0))
        cf_format_describe(R(0), &t);
    else
        add_ascii(&t, "(null)");
    gaddr s = cfstr_from_text(&t);
    char *u = cfstr_utf8(s);
    LOGF(LOG_TRAPS, "CFShow: %s\n", u ? u : "");
    free(u);
    cf_release(s);
    text_free(&t);
    RETURN_VOID();
}

static void cf_CFShowStr(ppc_cpu *c)
{
    cf_string *s = str_of(R(0));
    char *u = s ? cfstr_utf8(R(0)) : NULL;
    if (s)
        LOGF(LOG_TRAPS, "CFShowStr: Length %u\nIsEightBit %d\nHasLengthByte 0\nHasNullByte 1\nInlineContents 0\n"
                        "Allocator %08X\nMutable %d\nContents %s\n",
             s->n, !s->wide, s->o.alloc, s->mut, u ? u : "");
    else
        LOGF(LOG_TRAPS, "CFShowStr: This is a NULL string\n");
    free(u);
    RETURN_VOID();
}

/* __CFStringMakeConstantString(cStr): CFSTR("..."), one string a text, in
 * the system's encoding, never freed. */
static void cf_MakeConstantString(ppc_cpu *c)
{
    gaddr lit = R(0);
    if (!lit)
        RETURN(0);
    uint32_t n = cstr_len(lit);
    char *b = malloc(n + 1);
    if (!b)
        RETURN(0);
    gget(b, lit, n);
    b[n] = 0;
    gaddr s = constant_of(b, n, system_encoding(), lit);
    free(b);
    RETURN(s);
}

/* ---------------------------------------------------------------------- */
/* Character sets: a bitmap of the BMP, as CFCharacterSetCreateBitmapRepresentation
 * gives it (bit c&7 of byte c>>3). */

enum { BITMAP = 8192 };

typedef struct {
    cf_obj o;
    uint8_t *bits;
    bool mut;
} cf_charset;

static bool cs_has(const cf_charset *s, uint16_t ch) { return s->bits[ch >> 3] & (1 << (ch & 7)); }
static void cs_set(cf_charset *s, uint16_t ch, bool on)
{
    if (on)
        s->bits[ch >> 3] |= (uint8_t)(1 << (ch & 7));
    else
        s->bits[ch >> 3] &= (uint8_t)~(1 << (ch & 7));
}

static void cs_dispose(cf_obj *o) { free(((cf_charset *)o)->bits); }
static bool cs_equal(cf_obj *a, cf_obj *b) { return !memcmp(((cf_charset *)a)->bits, ((cf_charset *)b)->bits, BITMAP); }
static uint32_t cs_hash(cf_obj *o)
{
    uint32_t h = 0;
    for (int i = 0; i < BITMAP; i++)
        h = h * 31 + ((cf_charset *)o)->bits[i];
    return h;
}

static const cf_class charset_class = { "CFCharacterSet", NULL, cs_dispose, cs_equal, cs_hash, NULL };

static cf_charset *cs_new(gaddr alloc, bool mutable_)
{
    cf_charset *s = cf_new(CFT_CHARSET, sizeof *s, alloc, 0);
    if (!s)
        return NULL;
    s->bits = calloc(1, BITMAP);
    s->mut = mutable_;
    if (!s->bits) {
        cf_release(s->o.ref);
        return NULL;
    }
    return s;
}

static cf_charset *cs_of(gaddr ref) { return cf_get_type(ref, CFT_CHARSET); }

static void cs_add_ranges(cf_charset *s, const urange *r, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        for (uint32_t ch = r[i].lo; ch <= r[i].hi; ch++)
            cs_set(s, (uint16_t)ch, true);
}

static gaddr predefined[13];

static void cf_CFCharacterSetGetTypeID(ppc_cpu *c) { RETURN(CFT_CHARSET); }

/* CFCharacterSetGetPredefined(theSetIdentifier) */
static void cf_CFCharacterSetGetPredefined(ppc_cpu *c)
{
    uint32_t id = R(0);
    if (id < 1 || id > 12)
        RETURN(0);
    if (predefined[id])
        RETURN(predefined[id]);
    cf_charset *s = cs_new(cf_allocator_system(), false);
    if (!s)
        RETURN(0);
    switch (id) {
    case 1: /* kCFCharacterSetControl */
        cs_add_ranges(s, controls, NELEM(controls));
        break;
    case 2: /* kCFCharacterSetWhitespace */
        cs_add_ranges(s, whitespace, NELEM(whitespace));
        break;
    case 3: /* kCFCharacterSetWhitespaceAndNewline */
        cs_add_ranges(s, whitespace, NELEM(whitespace));
        cs_add_ranges(s, newlines, NELEM(newlines));
        break;
    case 4: /* kCFCharacterSetDecimalDigit */
        cs_add_ranges(s, digits, NELEM(digits));
        break;
    case 5: /* kCFCharacterSetLetter */
        cs_add_ranges(s, letters, NELEM(letters));
        cs_add_ranges(s, marks, NELEM(marks));
        break;
    case 6: /* kCFCharacterSetLowercaseLetter */
    case 7: /* kCFCharacterSetUppercaseLetter */
        for (uint32_t ch = 0; ch < 0x10000; ch++) {
            uint16_t u = (uint16_t)ch;
            bool lower = to_upper(u) != u || u == 0xDF || u == 0x138 || u == 0x149;
            bool upper = to_lower(u) != u;
            if (id == 6 ? lower : upper)
                cs_set(s, u, true);
        }
        break;
    case 8: /* kCFCharacterSetNonBase */
        cs_add_ranges(s, marks, NELEM(marks));
        break;
    case 9: /* kCFCharacterSetDecomposable */
        cs_add_ranges(s, decomposables, NELEM(decomposables));
        break;
    case 10: /* kCFCharacterSetAlphaNumeric */
        cs_add_ranges(s, letters, NELEM(letters));
        cs_add_ranges(s, marks, NELEM(marks));
        cs_add_ranges(s, digits, NELEM(digits));
        cs_add_ranges(s, other_numbers, NELEM(other_numbers));
        break;
    case 11: /* kCFCharacterSetPunctuation */
        cs_add_ranges(s, punctuation, NELEM(punctuation));
        break;
    case 12: /* kCFCharacterSetIllegal */
        cs_add_ranges(s, illegal, NELEM(illegal));
        break;
    }
    cf_make_constant(&s->o);
    predefined[id] = s->o.ref;
    RETURN(s->o.ref);
}

/* CFCharacterSetCreateWithCharactersInRange(alloc, theRange) */
static void cf_CFCharacterSetCreateWithCharactersInRange(ppc_cpu *c)
{
    cf_charset *s = cs_new(R(0), false);
    if (s)
        for (uint32_t ch = R(1); ch < R(1) + R(2) && ch < 0x10000; ch++)
            cs_set(s, (uint16_t)ch, true);
    RETURN(s ? s->o.ref : 0);
}

static void cs_string(cf_charset *s, gaddr str, bool on)
{
    uint32_t n;
    const uint16_t *u = cfstr_chars(str, &n);
    for (uint32_t i = 0; s && u && i < n; i++)
        cs_set(s, u[i], on);
}

/* CFCharacterSetCreateWithCharactersInString(alloc, theString) */
static void cf_CFCharacterSetCreateWithCharactersInString(ppc_cpu *c)
{
    cf_charset *s = cs_new(R(0), false);
    cs_string(s, R(1), true);
    RETURN(s ? s->o.ref : 0);
}

/* CFCharacterSetCreateWithBitmapRepresentation(alloc, theData) */
static void cf_CFCharacterSetCreateWithBitmapRepresentation(ppc_cpu *c)
{
    cf_charset *s = cs_new(R(0), false);
    uint32_t n = 0;
    gaddr b = cfdata_bytes(R(1), &n);
    if (s && b)
        gget(s->bits, b, n < BITMAP ? n : BITMAP);
    RETURN(s ? s->o.ref : 0);
}

static void cf_CFCharacterSetCreateMutable(ppc_cpu *c)
{
    cf_charset *s = cs_new(R(0), true);
    RETURN(s ? s->o.ref : 0);
}

static void cf_CFCharacterSetCreateMutableCopy(ppc_cpu *c)
{
    cf_charset *from = cs_of(R(1)), *s = from ? cs_new(R(0), true) : NULL;
    if (s)
        memcpy(s->bits, from->bits, BITMAP);
    RETURN(s ? s->o.ref : 0);
}

/* CFCharacterSetIsCharacterMember(theSet, theChar) */
static void cf_CFCharacterSetIsCharacterMember(ppc_cpu *c)
{
    cf_charset *s = cs_of(R(0));
    RETURN(s && cs_has(s, (uint16_t)R(1)));
}

static void cf_CFCharacterSetCreateBitmapRepresentation(ppc_cpu *c)
{
    cf_charset *s = cs_of(R(1));
    RETURN(s ? cfdata_new(R(0), s->bits, BITMAP, false) : 0);
}

/* CFCharacterSetAddCharactersInRange(theSet, theRange) and its opposite */
static void cf_CFCharacterSetAddCharactersInRange(ppc_cpu *c)
{
    cf_charset *s = cs_of(R(0));
    for (uint32_t ch = R(1); s && ch < R(1) + R(2) && ch < 0x10000; ch++)
        cs_set(s, (uint16_t)ch, true);
    RETURN_VOID();
}

static void cf_CFCharacterSetRemoveCharactersInRange(ppc_cpu *c)
{
    cf_charset *s = cs_of(R(0));
    for (uint32_t ch = R(1); s && ch < R(1) + R(2) && ch < 0x10000; ch++)
        cs_set(s, (uint16_t)ch, false);
    RETURN_VOID();
}

static void cf_CFCharacterSetAddCharactersInString(ppc_cpu *c)
{
    cs_string(cs_of(R(0)), R(1), true);
    RETURN_VOID();
}

static void cf_CFCharacterSetRemoveCharactersInString(ppc_cpu *c)
{
    cs_string(cs_of(R(0)), R(1), false);
    RETURN_VOID();
}

static void cf_CFCharacterSetUnion(ppc_cpu *c)
{
    cf_charset *s = cs_of(R(0)), *o = cs_of(R(1));
    for (int i = 0; s && o && i < BITMAP; i++)
        s->bits[i] |= o->bits[i];
    RETURN_VOID();
}

static void cf_CFCharacterSetIntersect(ppc_cpu *c)
{
    cf_charset *s = cs_of(R(0)), *o = cs_of(R(1));
    for (int i = 0; s && o && i < BITMAP; i++)
        s->bits[i] &= o->bits[i];
    RETURN_VOID();
}

static void cf_CFCharacterSetInvert(ppc_cpu *c)
{
    cf_charset *s = cs_of(R(0));
    for (int i = 0; s && i < BITMAP; i++)
        s->bits[i] = (uint8_t)~s->bits[i];
    RETURN_VOID();
}

/* Registered with the strings' (cfstr_reset runs at each launch). */
static void cfcharset_reset(void)
{
    memset(predefined, 0, sizeof predefined);
    cf_register_class(CFT_CHARSET, &charset_class);
}

const ppc_export cfstr_exports[] = {
    { "CFStringGetTypeID", cf_CFStringGetTypeID },
    { "CFStringCreateWithPascalString", cf_CFStringCreateWithPascalString },
    { "CFStringCreateWithCString", cf_CFStringCreateWithCString },
    { "CFStringCreateWithCharacters", cf_CFStringCreateWithCharacters },
    { "CFStringCreateWithPascalStringNoCopy", cf_CFStringCreateWithPascalStringNoCopy },
    { "CFStringCreateWithCStringNoCopy", cf_CFStringCreateWithCStringNoCopy },
    { "CFStringCreateWithCharactersNoCopy", cf_CFStringCreateWithCharactersNoCopy },
    { "CFStringCreateWithSubstring", cf_CFStringCreateWithSubstring },
    { "CFStringCreateCopy", cf_CFStringCreateCopy },
    { "CFStringCreateWithFormat", cf_CFStringCreateWithFormat },
    { "CFStringCreateWithFormatAndArguments", cf_CFStringCreateWithFormatAndArguments },
    { "CFStringCreateMutable", cf_CFStringCreateMutable },
    { "CFStringCreateMutableCopy", cf_CFStringCreateMutableCopy },
    { "CFStringCreateMutableWithExternalCharactersNoCopy", cf_CFStringCreateMutableWithExternalCharactersNoCopy },
    { "CFStringGetLength", cf_CFStringGetLength },
    { "CFStringGetCharacterAtIndex", cf_CFStringGetCharacterAtIndex },
    { "CFStringGetCharacters", cf_CFStringGetCharacters },
    { "CFStringGetPascalString", cf_CFStringGetPascalString },
    { "CFStringGetCString", cf_CFStringGetCString },
    { "CFStringGetPascalStringPtr", cf_CFStringGetPascalStringPtr },
    { "CFStringGetCStringPtr", cf_CFStringGetCStringPtr },
    { "CFStringGetCharactersPtr", cf_CFStringGetCharactersPtr },
    { "CFStringGetBytes", cf_CFStringGetBytes },
    { "CFStringCreateWithBytes", cf_CFStringCreateWithBytes },
    { "CFStringCreateFromExternalRepresentation", cf_CFStringCreateFromExternalRepresentation },
    { "CFStringCreateExternalRepresentation", cf_CFStringCreateExternalRepresentation },
    { "CFStringGetSmallestEncoding", cf_CFStringGetSmallestEncoding },
    { "CFStringGetFastestEncoding", cf_CFStringGetFastestEncoding },
    { "CFStringGetSystemEncoding", cf_CFStringGetSystemEncoding },
    { "CFStringGetMaximumSizeForEncoding", cf_CFStringGetMaximumSizeForEncoding },
    { "CFStringCompareWithOptions", cf_CFStringCompareWithOptions },
    { "CFStringCompare", cf_CFStringCompare },
    { "CFStringFindWithOptions", cf_CFStringFindWithOptions },
    { "CFStringCreateArrayWithFindResults", cf_CFStringCreateArrayWithFindResults },
    { "CFStringFind", cf_CFStringFind },
    { "CFStringHasPrefix", cf_CFStringHasPrefix },
    { "CFStringHasSuffix", cf_CFStringHasSuffix },
    { "CFStringGetLineBounds", cf_CFStringGetLineBounds },
    { "CFStringCreateByCombiningStrings", cf_CFStringCreateByCombiningStrings },
    { "CFStringCreateArrayBySeparatingStrings", cf_CFStringCreateArrayBySeparatingStrings },
    { "CFStringGetIntValue", cf_CFStringGetIntValue },
    { "CFStringGetDoubleValue", cf_CFStringGetDoubleValue },
    { "CFStringAppend", cf_CFStringAppend },
    { "CFStringAppendCharacters", cf_CFStringAppendCharacters },
    { "CFStringAppendPascalString", cf_CFStringAppendPascalString },
    { "CFStringAppendCString", cf_CFStringAppendCString },
    { "CFStringAppendFormat", cf_CFStringAppendFormat },
    { "CFStringAppendFormatAndArguments", cf_CFStringAppendFormatAndArguments },
    { "CFStringInsert", cf_CFStringInsert },
    { "CFStringDelete", cf_CFStringDelete },
    { "CFStringReplace", cf_CFStringReplace },
    { "CFStringReplaceAll", cf_CFStringReplaceAll },
    { "CFStringSetExternalCharactersNoCopy", cf_CFStringSetExternalCharactersNoCopy },
    { "CFStringPad", cf_CFStringPad },
    { "CFStringTrim", cf_CFStringTrim },
    { "CFStringTrimWhitespace", cf_CFStringTrimWhitespace },
    { "CFStringLowercase", cf_CFStringLowercase },
    { "CFStringUppercase", cf_CFStringUppercase },
    { "CFStringCapitalize", cf_CFStringCapitalize },
    { "CFStringIsEncodingAvailable", cf_CFStringIsEncodingAvailable },
    { "CFStringGetListOfAvailableEncodings", cf_CFStringGetListOfAvailableEncodings },
    { "CFStringGetNameOfEncoding", cf_CFStringGetNameOfEncoding },
    { "CFStringConvertEncodingToNSStringEncoding", cf_CFStringConvertEncodingToNSStringEncoding },
    { "CFStringConvertNSStringEncodingToEncoding", cf_CFStringConvertNSStringEncodingToEncoding },
    { "CFStringConvertEncodingToWindowsCodepage", cf_CFStringConvertEncodingToWindowsCodepage },
    { "CFStringConvertWindowsCodepageToEncoding", cf_CFStringConvertWindowsCodepageToEncoding },
    { "CFStringConvertIANACharSetNameToEncoding", cf_CFStringConvertIANACharSetNameToEncoding },
    { "CFStringConvertEncodingToIANACharSetName", cf_CFStringConvertEncodingToIANACharSetName },
    { "CFStringGetMostCompatibleMacStringEncoding", cf_CFStringGetMostCompatibleMacStringEncoding },
    { "CFShow", cf_CFShow },
    { "CFShowStr", cf_CFShowStr },
    { "__CFStringMakeConstantString", cf_MakeConstantString },
    { "CFCharacterSetGetTypeID", cf_CFCharacterSetGetTypeID },
    { "CFCharacterSetGetPredefined", cf_CFCharacterSetGetPredefined },
    { "CFCharacterSetCreateWithCharactersInRange", cf_CFCharacterSetCreateWithCharactersInRange },
    { "CFCharacterSetCreateWithCharactersInString", cf_CFCharacterSetCreateWithCharactersInString },
    { "CFCharacterSetCreateWithBitmapRepresentation", cf_CFCharacterSetCreateWithBitmapRepresentation },
    { "CFCharacterSetCreateMutable", cf_CFCharacterSetCreateMutable },
    { "CFCharacterSetCreateMutableCopy", cf_CFCharacterSetCreateMutableCopy },
    { "CFCharacterSetIsCharacterMember", cf_CFCharacterSetIsCharacterMember },
    { "CFCharacterSetCreateBitmapRepresentation", cf_CFCharacterSetCreateBitmapRepresentation },
    { "CFCharacterSetAddCharactersInRange", cf_CFCharacterSetAddCharactersInRange },
    { "CFCharacterSetRemoveCharactersInRange", cf_CFCharacterSetRemoveCharactersInRange },
    { "CFCharacterSetAddCharactersInString", cf_CFCharacterSetAddCharactersInString },
    { "CFCharacterSetRemoveCharactersInString", cf_CFCharacterSetRemoveCharactersInString },
    { "CFCharacterSetUnion", cf_CFCharacterSetUnion },
    { "CFCharacterSetIntersect", cf_CFCharacterSetIntersect },
    { "CFCharacterSetInvert", cf_CFCharacterSetInvert },
};
const unsigned cfstr_export_count = sizeof cfstr_exports / sizeof cfstr_exports[0];
