/*
 * The host's text encodings, for the Text Encoding Converter (tec.c):
 * bytes in a TextEncoding to UTF-16 (host byte order) and back.  On macOS
 * CoreFoundation does it (a CFStringEncoding is the TextEncoding's base,
 * with UTF-7's format: the same numbers); elsewhere iconv, by name.  On
 * its own because CoreFoundation's headers and the Toolbox's both define
 * noErr.
 *
 * Both directions convert as much as they can and say why they stopped,
 * as iconv does: the end, the output full (a character that doesn't fit
 * isn't started), an invalid or unmappable character at *used, or an
 * incomplete one ending the input.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__) && !defined(HOSTTEXT_ICONV)
#include <CoreFoundation/CoreFoundation.h>
#else
#include <errno.h>
#include <iconv.h>
#endif

enum { HT_OK, HT_FULL, HT_INVALID, HT_INCOMPLETE, HT_UNSUPPORTED };

bool host_text_supported(uint32_t enc);
int host_text_decode(uint32_t enc, const uint8_t *in, size_t n, size_t *used, uint16_t *out, size_t cap,
                     size_t *produced);
int host_text_encode(uint32_t enc, const uint16_t *in, size_t n, size_t *used, uint8_t *out, size_t cap,
                     size_t *produced);

static bool high_surrogate(uint16_t u) { return (u & 0xFC00) == 0xD800; }

#if defined(__APPLE__) && !defined(HOSTTEXT_ICONV)

static CFStringEncoding cf_encoding(uint32_t enc)
{
    /* The base, and the format for UTF-7 (0x04000100); variants are
     * CoreFoundation's own (Mac OS Roman's euro sign). */
    return (enc & 0xFFFF) | (enc & 0xFC000000u);
}

bool host_text_supported(uint32_t enc) { return CFStringIsEncodingAvailable(cf_encoding(enc)); }

/* Single-byte encodings decode byte by byte from a table, made once. */
typedef struct {
    CFStringEncoding cfe;
    uint8_t len[256];      /* 0: no character */
    uint16_t units[256][4];
} byte_table;

static byte_table *tables[16];
static unsigned ntables;

static const byte_table *table_for(CFStringEncoding cfe)
{
    for (unsigned i = 0; i < ntables; i++)
        if (tables[i]->cfe == cfe)
            return tables[i];
    if (CFStringGetMaximumSizeForEncoding(1, cfe) != 1)
        return NULL;
    byte_table *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->cfe = cfe;
    for (int b = 0; b < 256; b++) {
        uint8_t byte = (uint8_t)b;
        CFStringRef s = CFStringCreateWithBytes(NULL, &byte, 1, cfe, false);
        if (!s)
            continue;
        CFIndex l = CFStringGetLength(s);
        if (l > 0 && l <= 4) {
            CFStringGetCharacters(s, CFRangeMake(0, l), t->units[b]);
            t->len[b] = (uint8_t)l;
        }
        CFRelease(s);
    }
    if (ntables == sizeof tables / sizeof tables[0]) {
        free(tables[0]);
        memmove(tables, tables + 1, sizeof tables - sizeof tables[0]);
        ntables--;
    }
    tables[ntables++] = t;
    return t;
}

/* The whole of in[0, n) in one string, if it decodes and fits. */
static bool decode_all(CFStringEncoding cfe, const uint8_t *in, size_t n, uint16_t *out, size_t cap, size_t *produced)
{
    CFStringRef s = CFStringCreateWithBytes(NULL, in, (CFIndex)n, cfe, false);
    if (!s)
        return false;
    CFIndex l = CFStringGetLength(s);
    bool ok = (size_t)l <= cap;
    if (ok) {
        CFStringGetCharacters(s, CFRangeMake(0, l), out);
        *produced = (size_t)l;
    }
    CFRelease(s);
    return ok;
}

/* Whether the bytes ending the input begin a character: one more byte
 * makes one (or three more, for GB 18030's four-byte ones). */
static bool could_continue(CFStringEncoding cfe, const uint8_t *in, size_t n)
{
    uint8_t b[8];
    memcpy(b, in, n);
    for (int t = 0x30; t <= 0xFE; t++) {
        for (int longer = 0; longer < 2; longer++) {
            size_t k = n;
            b[k++] = (uint8_t)t;
            if (longer)
                b[k++] = 0x81, b[k++] = 0x30;
            CFStringRef s = CFStringCreateWithBytes(NULL, b, (CFIndex)k, cfe, false);
            bool ok = s && CFStringGetLength(s) > 0;
            if (s)
                CFRelease(s);
            if (ok)
                return true;
        }
    }
    return false;
}

int host_text_decode(uint32_t enc, const uint8_t *in, size_t n, size_t *used, uint16_t *out, size_t cap,
                     size_t *produced)
{
    CFStringEncoding cfe = cf_encoding(enc);
    *used = *produced = 0;
    if (!CFStringIsEncodingAvailable(cfe))
        return HT_UNSUPPORTED;
    const byte_table *t = table_for(cfe);
    size_t pos = 0, opos = 0;
    if (!t && n) {
        /* All of it, else all but a character cut short at the end;
         * what's left goes a character at a time. */
        for (size_t cut = 0; cut < 4 && cut < n; cut++) {
            size_t k;
            if (decode_all(cfe, in, n - cut, out, cap, &k)) {
                pos = n - cut;
                opos = k;
                break;
            }
        }
    }
    while (pos < n) {
        if (t) {
            uint8_t l = t->len[in[pos]];
            if (!l) {
                *used = pos, *produced = opos;
                return HT_INVALID;
            }
            if (opos + l > cap) {
                *used = pos, *produced = opos;
                return HT_FULL;
            }
            memcpy(out + opos, t->units[in[pos]], l * sizeof(uint16_t));
            opos += l;
            pos++;
            continue;
        }
        /* The shortest run of bytes that makes a character. */
        size_t k, got = 0;
        CFStringRef s = NULL;
        for (k = 1; k <= 4 && pos + k <= n; k++) {
            s = CFStringCreateWithBytes(NULL, in + pos, (CFIndex)k, cfe, false);
            if (s && CFStringGetLength(s) > 0)
                break;
            if (s)
                CFRelease(s);
            s = NULL;
        }
        if (!s) {
            *used = pos, *produced = opos;
            return n - pos < 4 && could_continue(cfe, in + pos, n - pos) ? HT_INCOMPLETE : HT_INVALID;
        }
        got = (size_t)CFStringGetLength(s);
        if (opos + got > cap) {
            CFRelease(s);
            *used = pos, *produced = opos;
            return HT_FULL;
        }
        CFStringGetCharacters(s, CFRangeMake(0, (CFIndex)got), out + opos);
        CFRelease(s);
        opos += got;
        pos += k;
    }
    *used = pos, *produced = opos;
    return HT_OK;
}

int host_text_encode(uint32_t enc, const uint16_t *in, size_t n, size_t *used, uint8_t *out, size_t cap,
                     size_t *produced)
{
    CFStringEncoding cfe = cf_encoding(enc);
    *used = *produced = 0;
    if (!CFStringIsEncodingAvailable(cfe))
        return HT_UNSUPPORTED;
    if (!n)
        return HT_OK;
    CFStringRef s = CFStringCreateWithCharactersNoCopy(NULL, in, (CFIndex)n, kCFAllocatorNull);
    if (!s)
        return HT_INVALID;
    CFIndex bytes = 0;
    CFIndex done = CFStringGetBytes(s, CFRangeMake(0, (CFIndex)n), cfe, 0, false, out, (CFIndex)cap, &bytes);
    int st = HT_OK;
    if ((size_t)done < n) {
        /* Stopped short: for want of room, or at a character it can't
         * make (tried on its own). */
        CFIndex l = high_surrogate(in[done]) && (size_t)done + 1 < n ? 2 : 1, need = 0;
        if (high_surrogate(in[done]) && (size_t)done + 1 == n)
            st = HT_INCOMPLETE;
        else
            st = CFStringGetBytes(s, CFRangeMake(done, l), cfe, 0, false, NULL, 0, &need) == l ? HT_FULL : HT_INVALID;
    }
    CFRelease(s);
    *used = (size_t)done;
    *produced = (size_t)bytes;
    return st;
}

#else

/* iconv's names for the TextEncodings it may have (the base). */
static const struct {
    uint16_t base;
    const char *name;
} iconv_names[] = {
    { 0x0000, "MACINTOSH" }, { 0x0001, "SHIFT_JIS" }, { 0x0002, "BIG5" }, { 0x0003, "EUC-KR" },
    { 0x0004, "MACARABIC" }, { 0x0005, "MACHEBREW" }, { 0x0006, "MACGREEK" }, { 0x0007, "MACCYRILLIC" },
    { 0x0015, "MACTHAI" }, { 0x0019, "GB2312" }, { 0x001D, "MACCENTRALEUROPE" }, { 0x0023, "MACTURKISH" },
    { 0x0024, "MACCROATIAN" }, { 0x0025, "MACICELAND" }, { 0x0026, "MACROMANIA" }, { 0x0098, "MACUKRAINE" },
    { 0x0100, "UTF-7" },
    { 0x0201, "ISO-8859-1" }, { 0x0202, "ISO-8859-2" }, { 0x0203, "ISO-8859-3" }, { 0x0204, "ISO-8859-4" },
    { 0x0205, "ISO-8859-5" }, { 0x0206, "ISO-8859-6" }, { 0x0207, "ISO-8859-7" }, { 0x0208, "ISO-8859-8" },
    { 0x0209, "ISO-8859-9" }, { 0x020A, "ISO-8859-10" }, { 0x020D, "ISO-8859-13" }, { 0x020E, "ISO-8859-14" },
    { 0x020F, "ISO-8859-15" }, { 0x0210, "ISO-8859-16" },
    { 0x0400, "CP437" }, { 0x0405, "CP737" }, { 0x0406, "CP775" }, { 0x0410, "CP850" }, { 0x0412, "CP852" },
    { 0x0413, "CP855" }, { 0x0414, "CP857" }, { 0x0415, "CP860" }, { 0x0416, "CP861" }, { 0x0417, "CP862" },
    { 0x0418, "CP863" }, { 0x0419, "CP864" }, { 0x041A, "CP865" }, { 0x041B, "CP866" }, { 0x041C, "CP869" },
    { 0x041D, "CP874" }, { 0x0420, "CP932" }, { 0x0421, "CP936" }, { 0x0422, "CP949" }, { 0x0423, "CP950" },
    { 0x0500, "CP1252" }, { 0x0501, "CP1250" }, { 0x0502, "CP1251" }, { 0x0503, "CP1253" }, { 0x0504, "CP1254" },
    { 0x0505, "CP1255" }, { 0x0506, "CP1256" }, { 0x0507, "CP1257" }, { 0x0508, "CP1258" },
    { 0x0600, "US-ASCII" }, { 0x0631, "GBK" }, { 0x0632, "GB18030" },
    { 0x0820, "ISO-2022-JP" }, { 0x0821, "ISO-2022-JP-2" }, { 0x0830, "ISO-2022-CN" }, { 0x0840, "ISO-2022-KR" },
    { 0x0920, "EUC-JP" }, { 0x0930, "GB2312" }, { 0x0931, "EUC-TW" }, { 0x0940, "EUC-KR" },
    { 0x0A01, "SHIFT_JIS" }, { 0x0A02, "KOI8-R" }, { 0x0A03, "BIG5" }, { 0x0A05, "HZ" },
    { 0x0A06, "BIG5-HKSCS" }, { 0x0A07, "VISCII" }, { 0x0A08, "KOI8-U" },
};

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define UTF16_HOST "UTF-16BE"
#else
#define UTF16_HOST "UTF-16LE"
#endif

static const char *iconv_name(uint32_t enc)
{
    uint32_t base = enc & 0xFFFF;
    if (base >= 0x0100 && base <= 0x01FF && (enc >> 26 & 0x3F) != 1)
        return NULL; /* Unicode but UTF-7: tec.c's own */
    for (unsigned i = 0; i < sizeof iconv_names / sizeof iconv_names[0]; i++)
        if (iconv_names[i].base == (base >= 0x0100 && base <= 0x01FF ? 0x0100 : base))
            return iconv_names[i].name;
    return NULL;
}

/* Open converters, kept: a few each way. */
typedef struct {
    uint32_t enc;
    bool to_unicode;
    iconv_t cd;
} open_cd;

static open_cd cds[16];
static unsigned ncds;

static iconv_t cd_for(uint32_t enc, bool to_unicode)
{
    for (unsigned i = 0; i < ncds; i++)
        if (cds[i].enc == enc && cds[i].to_unicode == to_unicode) {
            iconv(cds[i].cd, NULL, NULL, NULL, NULL); /* the initial shift state */
            return cds[i].cd;
        }
    const char *name = iconv_name(enc);
    if (!name)
        return (iconv_t)-1;
    iconv_t cd = to_unicode ? iconv_open(UTF16_HOST, name) : iconv_open(name, UTF16_HOST);
    if (cd == (iconv_t)-1)
        return cd;
    if (ncds == sizeof cds / sizeof cds[0]) {
        iconv_close(cds[0].cd);
        memmove(cds, cds + 1, sizeof cds - sizeof cds[0]);
        ncds--;
    }
    cds[ncds++] = (open_cd){ enc, to_unicode, cd };
    return cd;
}

bool host_text_supported(uint32_t enc) { return cd_for(enc, true) != (iconv_t)-1; }

static int run_iconv(iconv_t cd, const char *in, size_t n, size_t *used, char *out, size_t cap, size_t *produced)
{
    char *ip = (char *)in, *op = out;
    size_t il = n, ol = cap;
    int st = HT_OK;
    if (iconv(cd, &ip, &il, &op, &ol) == (size_t)-1)
        st = errno == E2BIG ? HT_FULL : errno == EINVAL ? HT_INCOMPLETE : HT_INVALID;
    else if (iconv(cd, NULL, NULL, &op, &ol) == (size_t)-1) /* back to the initial shift state */
        st = HT_FULL;
    *used = n - il;
    *produced = cap - ol;
    return st;
}

int host_text_decode(uint32_t enc, const uint8_t *in, size_t n, size_t *used, uint16_t *out, size_t cap,
                     size_t *produced)
{
    *used = *produced = 0;
    iconv_t cd = cd_for(enc, true);
    if (cd == (iconv_t)-1)
        return HT_UNSUPPORTED;
    size_t bytes;
    int st = run_iconv(cd, (const char *)in, n, used, (char *)out, cap * 2, &bytes);
    *produced = bytes / 2;
    return st;
}

int host_text_encode(uint32_t enc, const uint16_t *in, size_t n, size_t *used, uint8_t *out, size_t cap,
                     size_t *produced)
{
    *used = *produced = 0;
    iconv_t cd = cd_for(enc, false);
    if (cd == (iconv_t)-1)
        return HT_UNSUPPORTED;
    size_t bytes;
    int st = run_iconv(cd, (const char *)in, n * 2, &bytes, (char *)out, cap, produced);
    *used = bytes / 2;
    if (st == HT_INCOMPLETE && !(*used < n && high_surrogate(in[*used])))
        st = HT_INVALID;
    return st;
}

#endif
