/*
 * The Text Encoding Converter (Mac OS 8's TextCommon, UnicodeConverter and
 * TextEncodingConverter libraries, as TEC 1.3 had them): TextEncodings,
 * conversions between Unicode and other encodings, converters between any
 * two, Internet charset names and an encoding sniffer.  Apple's own
 * libraries find their globals through a table their boot-time code set
 * up, which nothing here runs, so these are ours.
 *
 * A TextEncoding is its base (bits 0-15), variant (16-25) and format
 * (26-31).  Unicode (bases 0x100-0x1FF) in UTF-16, UTF-8 and UTF-32 is
 * converted here; every other encoding by the host (hosttext.c:
 * CoreFoundation on macOS, iconv elsewhere), to and from UTF-16.  UTF-16
 * in guest memory is big-endian, as a PowerPC Mac's UniChars are.
 *
 * Conversions go through UTF-16: the text decoded, then encoded.  A whole
 * buffer goes at once when it can; where it can't (the output full, a
 * byte sequence that isn't a character, a character the other encoding
 * lacks) the trouble is found a character at a time.  Fallbacks are '?'
 * (U+FFFD into Unicode), with kTECUsedFallbacksStatus, or without them an
 * error.  A TEC converter keeps a character cut short at the end of one
 * buffer until the next (TECFlushText gives it up as a fallback).
 *
 * The objects (TECObjectRef, TextToUnicodeInfo, UnicodeToTextInfo,
 * UnicodeToTextRunInfo, TECSnifferObjectRef) are blocks of ours in the
 * application heap: a tag, the encodings, options, the pending bytes.
 *
 * The sniffer is a heuristic: each candidate decoded, scored by the byte
 * sequences that aren't characters in it (errors) and the multibyte
 * characters and shifts it found (features).  Stateful encodings
 * (ISO-2022) are converted a buffer at a time only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "machine.h"
#include "ppc_bridge.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
/* The result, and back to the caller. */
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; return; } while (0)

/* Argument word n of a call (GPR slots, then the parameter area). */
static uint32_t arg(const ppc_cpu *c, unsigned n) { return n < 8 ? c->gpr[3 + n] : rd32(c->gpr[1] + 24 + 4 * n); }

/* hosttext.c */
enum { HT_OK, HT_FULL, HT_INVALID, HT_INCOMPLETE, HT_UNSUPPORTED };
bool host_text_supported(uint32_t enc);
int host_text_decode(uint32_t enc, const uint8_t *in, size_t n, size_t *used, uint16_t *out, size_t cap,
                     size_t *produced);
int host_text_encode(uint32_t enc, const uint16_t *in, size_t n, size_t *used, uint8_t *out, size_t cap,
                     size_t *produced);

enum {
    kTextUnsupportedEncodingErr = -8738,
    kTextMalformedInputErr = -8739,
    kTextUndefinedElementErr = -8740,
    kTECCorruptConverterErr = -8748,
    kTECNoConversionPathErr = -8749,
    kTECArrayFullErr = -8751,
    kTECPartialCharErr = -8753,
    kTECUnmappableElementErr = -8754,
    kTECItemUnavailableErr = -8771,
    kTECUsedFallbacksStatus = -8783,
    kTECOutputBufferFullStatus = -8785,
};

enum {
    kTextEncodingMacRoman = 0,
    kTextEncodingMacSymbol = 33,
    kTextEncodingMacDingbats = 34,
    kTextEncodingUnicodeDefault = 0x100,
    kTextEncodingUnicodeV2_0 = 0x103,
    kTextEncodingUS_ASCII = 0x600,
    kTextScriptDontCare = -128,
    kTextLanguageDontCare = -128,
    kTextRegionDontCare = -128,
};
enum { kUnicodeUTF16Format = 0, kUnicodeUTF7Format = 1, kUnicodeUTF8Format = 2, kUnicodeUTF32Format = 3,
       kUnicodeUTF16BEFormat = 4, kUnicodeUTF16LEFormat = 5, kUnicodeUTF32BEFormat = 6, kUnicodeUTF32LEFormat = 7 };
/* Control flags */
enum { kUnicodeUseFallbacksMask = 1 << 0, kUnicodeKeepSameEncodingMask = 1 << 8,
       kTECDisableFallbacksMask = 1 << 16 };

static uint32_t te_base(uint32_t e) { return e & 0xFFFF; }
static uint32_t te_format(uint32_t e) { return e >> 26 & 0x3F; }
static uint32_t te_make(uint32_t base, uint32_t variant, uint32_t format)
{
    return (base & 0xFFFF) | (variant & 0x3FF) << 16 | (format & 0x3F) << 26;
}
static bool is_unicode(uint32_t e) { return te_base(e) >= 0x100 && te_base(e) <= 0x1FF; }
/* Unicode in a form converted here (UTF-7 is the host's). */
static bool own_unicode(uint32_t e) { return is_unicode(e) && te_format(e) != kUnicodeUTF7Format && te_format(e) <= 7; }
static bool supported(uint32_t e) { return own_unicode(e) || host_text_supported(e); }
static uint32_t utf16(void) { return kTextEncodingUnicodeDefault; }

/* ---------------------------------------------------------------------- */
/* Unicode forms */

static bool is_high(uint32_t u) { return (u & 0xFC00) == 0xD800; }
static bool is_low(uint32_t u) { return (u & 0xFC00) == 0xDC00; }

/* One UTF-8 sequence at in: its scalar value and length; 0 if it isn't
 * one, -1 if it is cut short by the end. */
static int utf8_char(const uint8_t *in, size_t n, uint32_t *cp)
{
    uint8_t b = in[0];
    int len = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 0;
    if (!len)
        return 0;
    uint32_t v = len == 1 ? b : b & (0x7F >> len);
    for (int i = 1; i < len; i++) {
        if ((size_t)i >= n)
            return -1;
        if ((in[i] & 0xC0) != 0x80)
            return 0;
        v = v << 6 | (in[i] & 0x3F);
    }
    static const uint32_t least[] = { 0, 0, 0x80, 0x800, 0x10000 };
    if (v < least[len] || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF))
        return 0;
    *cp = v;
    return len;
}

static uint32_t get_unit(const uint8_t *p, bool le, int size)
{
    if (size == 2)
        return le ? (uint32_t)p[1] << 8 | p[0] : (uint32_t)p[0] << 8 | p[1];
    return le ? (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]
              : (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void put_unit(uint8_t *p, uint32_t v, bool le, int size)
{
    for (int i = 0; i < size; i++)
        p[le ? i : size - 1 - i] = (uint8_t)(v >> 8 * i);
}

/* Bytes of Unicode to UTF-16, with the host's conventions (HT_...). */
static int unicode_decode(uint32_t e, const uint8_t *in, size_t n, size_t *used, uint16_t *out, size_t cap,
                          size_t *produced)
{
    uint32_t f = te_format(e);
    size_t pos = 0, opos = 0;
    int st = HT_OK;
    while (pos < n) {
        uint32_t cp;
        size_t len;
        if (f == kUnicodeUTF8Format) {
            int l = utf8_char(in + pos, n - pos, &cp);
            if (l <= 0) {
                st = l < 0 ? HT_INCOMPLETE : HT_INVALID;
                break;
            }
            len = (size_t)l;
        } else if (f == kUnicodeUTF32Format || f == kUnicodeUTF32BEFormat || f == kUnicodeUTF32LEFormat) {
            if (n - pos < 4) {
                st = HT_INCOMPLETE;
                break;
            }
            cp = get_unit(in + pos, f == kUnicodeUTF32LEFormat, 4);
            if (cp > 0x10FFFF) {
                st = HT_INVALID;
                break;
            }
            len = 4;
        } else {
            bool le = f == kUnicodeUTF16LEFormat;
            if (n - pos < 2) {
                st = HT_INCOMPLETE;
                break;
            }
            cp = get_unit(in + pos, le, 2);
            len = 2;
            if (is_high(cp)) {
                /* A pair kept together; a lone surrogate passes as it is. */
                if (n - pos < 4) {
                    st = HT_INCOMPLETE;
                    break;
                }
                uint32_t lo = get_unit(in + pos + 2, le, 2);
                if (is_low(lo)) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    len = 4;
                }
            }
        }
        size_t units = cp >= 0x10000 ? 2 : 1;
        if (opos + units > cap) {
            st = HT_FULL;
            break;
        }
        if (units == 2) {
            out[opos++] = (uint16_t)(0xD800 + ((cp - 0x10000) >> 10));
            out[opos++] = (uint16_t)(0xDC00 + ((cp - 0x10000) & 0x3FF));
        } else {
            out[opos++] = (uint16_t)cp;
        }
        pos += len;
    }
    *used = pos;
    *produced = opos;
    return st;
}

static int unicode_encode(uint32_t e, const uint16_t *in, size_t n, size_t *used, uint8_t *out, size_t cap,
                          size_t *produced)
{
    uint32_t f = te_format(e);
    size_t pos = 0, opos = 0;
    int st = HT_OK;
    while (pos < n) {
        uint32_t cp = in[pos];
        size_t units = 1;
        if (is_high(cp)) {
            if (pos + 1 == n) {
                st = HT_INCOMPLETE;
                break;
            }
            if (is_low(in[pos + 1])) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (in[pos + 1] - 0xDC00u);
                units = 2;
            }
        }
        uint8_t b[4];
        size_t len;
        if (f == kUnicodeUTF8Format) {
            if (cp >= 0xD800 && cp <= 0xDFFF) {
                st = HT_INVALID; /* a lone surrogate has no UTF-8 */
                break;
            }
            if (cp < 0x80) {
                b[0] = (uint8_t)cp, len = 1;
            } else if (cp < 0x800) {
                b[0] = (uint8_t)(0xC0 | cp >> 6), b[1] = (uint8_t)(0x80 | (cp & 0x3F)), len = 2;
            } else if (cp < 0x10000) {
                b[0] = (uint8_t)(0xE0 | cp >> 12), b[1] = (uint8_t)(0x80 | (cp >> 6 & 0x3F));
                b[2] = (uint8_t)(0x80 | (cp & 0x3F)), len = 3;
            } else {
                b[0] = (uint8_t)(0xF0 | cp >> 18), b[1] = (uint8_t)(0x80 | (cp >> 12 & 0x3F));
                b[2] = (uint8_t)(0x80 | (cp >> 6 & 0x3F)), b[3] = (uint8_t)(0x80 | (cp & 0x3F)), len = 4;
            }
        } else if (f == kUnicodeUTF32Format || f == kUnicodeUTF32BEFormat || f == kUnicodeUTF32LEFormat) {
            put_unit(b, cp, f == kUnicodeUTF32LEFormat, 4);
            len = 4;
        } else {
            bool le = f == kUnicodeUTF16LEFormat;
            put_unit(b, in[pos], le, 2);
            if (units == 2)
                put_unit(b + 2, in[pos + 1], le, 2);
            len = units * 2;
        }
        if (opos + len > cap) {
            st = HT_FULL;
            break;
        }
        memcpy(out + opos, b, len);
        opos += len;
        pos += units;
    }
    *used = pos;
    *produced = opos;
    return st;
}

static int decode(uint32_t e, const uint8_t *in, size_t n, size_t *used, uint16_t *out, size_t cap, size_t *produced)
{
    if (own_unicode(e))
        return unicode_decode(e, in, n, used, out, cap, produced);
    return host_text_decode(e, in, n, used, out, cap, produced);
}

static int encode(uint32_t e, const uint16_t *in, size_t n, size_t *used, uint8_t *out, size_t cap, size_t *produced)
{
    if (own_unicode(e))
        return unicode_encode(e, in, n, used, out, cap, produced);
    return host_text_encode(e, in, n, used, out, cap, produced);
}

/* One character from in: its UTF-16 (up to 8 units) and its bytes.
 * HT_OK, or why there isn't one. */
static int decode_one(uint32_t e, const uint8_t *in, size_t n, uint16_t *units, size_t *nunits, size_t *bytes)
{
    /* No character is longer than this (with a shift before it). */
    enum { LONGEST = 16 };
    size_t look = n < LONGEST ? n : LONGEST;
    for (size_t cap = 1; cap <= 8; cap *= 2) {
        int st = decode(e, in, look, bytes, units, cap, nunits);
        if (*nunits)
            return HT_OK;
        if (st == HT_INCOMPLETE && look < n)
            return HT_INVALID;
        if (st != HT_FULL)
            return st == HT_OK ? HT_INCOMPLETE : st;
    }
    return HT_INVALID;
}

/* ---------------------------------------------------------------------- */
/* Conversion */

enum { CV_OK, CV_FULL, CV_PARTIAL, CV_MALFORMED, CV_UNMAPPABLE, CV_UNSUPPORTED };

typedef struct {
    uint32_t src, dst;
    bool fallbacks;
    bool final;        /* the end of the text: a character cut short is a bad one */
    /* Offsets in the input to find in the output (ascending), or none. */
    const uint32_t *in_offsets;
    size_t n_in_offsets;
    uint32_t *out_offsets;
    size_t n_out_offsets;
    /* Results */
    size_t used, produced;
    bool fell_back;
} conversion;

/* The fallback for a character dst can't have, or a bad one in the input. */
static int write_fallback(conversion *j, uint8_t *out, size_t cap, size_t *len)
{
    uint16_t rep = is_unicode(j->dst) ? 0xFFFD : '?';
    size_t u;
    int st = encode(j->dst, &rep, 1, &u, out, cap, len);
    if (st == HT_INVALID && rep != '?') {
        rep = '?';
        st = encode(j->dst, &rep, 1, &u, out, cap, len);
    }
    j->fell_back = true;
    return st;
}

static void note_offsets(conversion *j, size_t pos, size_t opos)
{
    while (j->n_out_offsets < j->n_in_offsets && j->in_offsets[j->n_out_offsets] <= pos)
        j->out_offsets[j->n_out_offsets++] = (uint32_t)opos;
}

/* One character at in[pos]: CV_OK to go on, else why it stopped. */
static int convert_one(conversion *j, const uint8_t *in, size_t n, uint8_t *out, size_t cap, size_t *pos,
                       size_t *opos, size_t *units_done)
{
    uint16_t units[8];
    size_t nu = 0, bytes = 0, eu, ep;
    int st = decode_one(j->src, in + *pos, n - *pos, units, &nu, &bytes);
    if (st == HT_UNSUPPORTED)
        return CV_UNSUPPORTED;
    if (st == HT_INCOMPLETE && !j->final)
        return CV_PARTIAL;
    if (st != HT_OK) {
        if (!j->fallbacks)
            return CV_MALFORMED;
        if (write_fallback(j, out + *opos, cap - *opos, &ep) != HT_OK)
            return CV_FULL;
        *opos += ep;
        *pos += 1;
        *units_done += 1;
        return CV_OK;
    }
    st = encode(j->dst, units, nu, &eu, out + *opos, cap - *opos, &ep);
    if (st == HT_UNSUPPORTED)
        return CV_UNSUPPORTED;
    if (st == HT_FULL)
        return CV_FULL;
    if (st != HT_OK) {
        if (!j->fallbacks)
            return CV_UNMAPPABLE;
        if (write_fallback(j, out + *opos, cap - *opos, &ep) != HT_OK)
            return CV_FULL;
    }
    *opos += ep;
    *pos += bytes;
    *units_done += nu;
    return CV_OK;
}

/* Convert in[0, n) into out[0, cap): j->used and j->produced say how far
 * it got; the result says why it stopped. */
static int convert(conversion *j, const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    size_t pos = 0, opos = 0;
    int result = CV_OK;
    bool slow = j->n_in_offsets > 0; /* offsets are found a character at a time */
    uint16_t *ubuf = NULL;
    while (pos < n) {
        size_t trouble = 0; /* UTF-16 units to go a character at a time */
        if (!slow) {
            size_t ucap = (n - pos) * 4 + 8, u, k, eu, ep;
            uint16_t *nb = realloc(ubuf, ucap * sizeof *ubuf);
            if (!nb) {
                result = CV_FULL;
                break;
            }
            ubuf = nb;
            int st1 = decode(j->src, in + pos, n - pos, &u, ubuf, ucap, &k);
            if (st1 == HT_UNSUPPORTED) {
                result = CV_UNSUPPORTED;
                break;
            }
            int st2 = encode(j->dst, ubuf, k, &eu, out + opos, cap - opos, &ep);
            if (st2 == HT_UNSUPPORTED) {
                result = CV_UNSUPPORTED;
                break;
            }
            if (st2 == HT_OK && eu == k) {
                pos += u;
                opos += ep;
                if (st1 == HT_OK || st1 == HT_FULL)
                    continue;
                trouble = 1; /* the bad or cut-short character at pos */
            } else {
                trouble = eu + 1; /* through the one that stopped it */
            }
        }
        size_t done = 0;
        do {
            note_offsets(j, pos, opos);
            result = convert_one(j, in, n, out, cap, &pos, &opos, &done);
        } while (result == CV_OK && pos < n && (slow || done < trouble));
        if (result != CV_OK)
            break;
    }
    free(ubuf);
    if (result == CV_OK)
        note_offsets(j, pos, opos);
    j->used = pos;
    j->produced = opos;
    return result;
}

/* Guest buffers: copied in, converted, the output copied back. */
static int convert_guest(conversion *j, gaddr in, uint32_t n, gaddr out, uint32_t cap, const uint8_t *prefix,
                         size_t nprefix)
{
    uint8_t *src = malloc(nprefix + n + 1), *dst = malloc((size_t)cap + 1);
    if (!src || !dst) {
        free(src);
        free(dst);
        j->used = j->produced = 0;
        return CV_FULL;
    }
    memcpy(src, prefix, nprefix);
    if (n)
        gget(src + nprefix, in, n);
    int r = convert(j, src, nprefix + n, dst, cap);
    if (j->produced)
        gput(out, dst, (uint32_t)j->produced);
    free(src);
    free(dst);
    return r;
}

/* ---------------------------------------------------------------------- */
/* Objects */

enum {
    TAG_TEC = 0x54454363,  /* 'TECc': a TECObjectRef */
    TAG_T2U = 0x54325563,  /* 'T2Uc': TextToUnicodeInfo */
    TAG_U2T = 0x55325463,  /* 'U2Tc': UnicodeToTextInfo */
    TAG_RUN = 0x55325272,  /* 'U2Rr': UnicodeToTextRunInfo */
    TAG_SNIFF = 0x54454373 /* 'TECs': TECSnifferObjectRef */
};
#define OBJ_TAG 0
#define OBJ_SRC 4          /* the source encoding */
#define OBJ_DST 8          /* the destination (the first of several) */
#define OBJ_OPTIONS 12
#define OBJ_NPEND 16       /* a character cut short: its bytes so far */
#define OBJ_PEND 17
#define OBJ_NENC 32        /* encodings for runs, one-to-many and sniffing */
#define OBJ_ENC 36
enum { MAX_PEND = 15, MAX_ENC = 64 };

static gaddr obj_new(uint32_t tag, uint32_t src, uint32_t dst, uint32_t nenc)
{
    if (nenc > MAX_ENC)
        nenc = MAX_ENC;
    gaddr o = tb_NewPtr(OBJ_ENC + 4 * (int32_t)nenc, false, true);
    if (!o)
        return 0;
    wr32(o + OBJ_TAG, tag);
    wr32(o + OBJ_SRC, src);
    wr32(o + OBJ_DST, dst);
    wr32(o + OBJ_NENC, nenc);
    return o;
}

static bool obj_is(gaddr o, uint32_t tag) { return o && o < 0xF0000000u && rd32(o + OBJ_TAG) == tag; }

static void obj_dispose(gaddr o)
{
    wr32(o + OBJ_TAG, 0);
    tb_DisposePtr(o);
}

static uint32_t obj_nenc(gaddr o)
{
    uint32_t n = rd32(o + OBJ_NENC);
    return n > MAX_ENC ? MAX_ENC : n;
}

static uint32_t mac_encoding_for_script(int16_t script) { return (uint32_t)(uint16_t)script; }

/* ---------------------------------------------------------------------- */
/* TextCommon */

/* CreateTextEncoding(base, variant, format) */
static void tc_CreateTextEncoding(ppc_cpu *c) { RETURN(te_make(R(0), R(1), R(2))); }
static void tc_GetTextEncodingBase(ppc_cpu *c) { RETURN(te_base(R(0))); }
static void tc_GetTextEncodingVariant(ppc_cpu *c) { RETURN(R(0) >> 16 & 0x3FF); }
static void tc_GetTextEncodingFormat(ppc_cpu *c) { RETURN(te_format(R(0))); }

/* ResolveDefaultTextEncoding(encoding): Unicode's default is 2.0. */
static void tc_ResolveDefaultTextEncoding(ppc_cpu *c)
{
    uint32_t e = R(0);
    if (te_base(e) == kTextEncodingUnicodeDefault)
        e = (e & ~0xFFFFu) | kTextEncodingUnicodeV2_0;
    RETURN(e);
}

static bool pstr_is(gaddr p, const char *s)
{
    size_t n = strlen(s);
    if (!p || rd8(p) != n)
        return false;
    for (size_t i = 0; i < n; i++) {
        uint8_t a = rd8(p + 1 + (gaddr)i), b = (uint8_t)s[i];
        if ((a | 0x20) != (b | 0x20))
            return false;
    }
    return true;
}

static void pstr_put(gaddr p, const char *s)
{
    size_t n = strlen(s) > 255 ? 255 : strlen(s);
    wr8(p, (uint8_t)n);
    gput(p + 1, s, (uint32_t)n);
}

/* Languages and regions that have encodings (or scripts) of their own. */
static const struct {
    int16_t lang;
    uint16_t encoding;
} lang_encodings[] = {
    { 17, 35 }, { 15, 37 }, { 18, 36 }, { 37, 38 }, { 31, 0x8C }, { 45, 0x98 },  /* Turkish, Icelandic, Croatian, Romanian, Farsi, Ukrainian */
    { 11, 1 }, { 19, 2 }, { 23, 3 }, { 12, 4 }, { 10, 5 }, { 14, 6 }, { 32, 7 }, { 22, 21 }, { 33, 25 },
    { 25, 29 }, { 26, 29 }, { 27, 29 }, { 28, 29 }, { 24, 29 }, { 38, 29 }, { 39, 29 }, /* Central European */
    { 21, 9 }, { 75, 9 },  /* Hindi, Marathi: Devanagari */
    { 30, 30 },            /* Vietnamese */
};
static const struct {
    int16_t region;
    int16_t lang;
} region_langs[] = {
    { 24, 17 }, { 21, 15 }, { 68, 18 }, { 39, 37 }, { 48, 31 }, { 62, 45 }, { 14, 11 }, { 53, 19 }, { 51, 23 },
    { 16, 12 }, { 13, 10 }, { 20, 14 }, { 49, 32 }, { 54, 22 }, { 52, 33 },
};

static int32_t encoding_for_lang(int16_t lang)
{
    for (unsigned i = 0; i < sizeof lang_encodings / sizeof lang_encodings[0]; i++)
        if (lang_encodings[i].lang == lang)
            return lang_encodings[i].encoding;
    return -1;
}

/* UpgradeScriptInfoToTextEncoding(script, language, region, fontName, *encoding) */
static void tc_UpgradeScriptInfoToTextEncoding(ppc_cpu *c)
{
    int16_t script = (int16_t)R(0), lang = (int16_t)R(1), region = (int16_t)R(2);
    gaddr font = R(3), out = R(4);
    if (!out)
        RETURN(paramErr);
    int32_t e = -1;
    if (pstr_is(font, "Symbol"))
        e = kTextEncodingMacSymbol;
    else if (pstr_is(font, "Zapf Dingbats"))
        e = kTextEncodingMacDingbats;
    if (e < 0 && lang == kTextLanguageDontCare && region != kTextRegionDontCare)
        for (unsigned i = 0; i < sizeof region_langs / sizeof region_langs[0]; i++)
            if (region_langs[i].region == region)
                lang = region_langs[i].lang;
    if (e < 0 && script == kTextScriptDontCare) {
        e = lang == kTextLanguageDontCare ? -1 : encoding_for_lang(lang);
        if (e < 0)
            e = kTextEncodingMacRoman; /* the system script's */
    }
    if (e < 0) {
        if (script < 0 || script > 32)
            RETURN(paramErr);
        e = (int32_t)mac_encoding_for_script(script);
        /* The languages with encodings of their own in a script. */
        int32_t le = lang == kTextLanguageDontCare ? -1 : encoding_for_lang(lang);
        if ((script == 0 && (le == 35 || le == 36 || le == 37 || le == 38)) || (script == 4 && le == 0x8C) ||
            (script == 7 && le == 0x98))
            e = le;
    }
    wr32(out, (uint32_t)e);
    RETURN(noErr);
}

/* RevertTextEncodingToScriptInfo(encoding, *script, *language, fontName) */
static void tc_RevertTextEncodingToScriptInfo(ppc_cpu *c)
{
    uint32_t base = te_base(R(0));
    gaddr script = R(1), lang = R(2), font = R(3);
    int16_t s, l = kTextLanguageDontCare;
    const char *fname = "";
    if (base <= 32) {
        s = (int16_t)base;
    } else if (base >= 33 && base <= 41) {
        static const int16_t langs[] = { -128, -128, 17, 18, 15, 37, -128, -128, -128 };
        s = 0;
        l = langs[base - 33];
        fname = base == 33 ? "Symbol" : base == 34 ? "Zapf Dingbats" : "";
    } else if (base == 0x8C) {
        s = 4, l = 31;
    } else if (base == 0x98) {
        s = 7, l = 45;
    } else {
        RETURN(kTextUnsupportedEncodingErr);
    }
    if (script)
        wr16(script, (uint16_t)s);
    if (lang)
        wr16(lang, (uint16_t)l);
    if (font)
        pstr_put(font, fname);
    RETURN(noErr);
}

/* GetTextEncodingFromScriptInfo(script, language, region, *encoding) */
static void tc_GetTextEncodingFromScriptInfo(ppc_cpu *c)
{
    R(4) = R(3);
    R(3) = 0;
    tc_UpgradeScriptInfoToTextEncoding(c);
}

/* GetScriptInfoFromTextEncoding(encoding, *script, *language) */
static void tc_GetScriptInfoFromTextEncoding(ppc_cpu *c)
{
    R(3) = 0;
    tc_RevertTextEncodingToScriptInfo(c);
}

/* The Mac OS encoding nearest each other one, by base. */
static const struct {
    uint16_t lo, hi, mac;
} nearest[] = {
    { 0x0100, 0x01FF, 0 }, { 0x0201, 0x0201, 0 }, { 0x0202, 0x0202, 29 }, { 0x0203, 0x0203, 0 },
    { 0x0204, 0x0204, 29 }, { 0x0205, 0x0205, 7 }, { 0x0206, 0x0206, 4 }, { 0x0207, 0x0207, 6 },
    { 0x0208, 0x0208, 5 }, { 0x0209, 0x0209, 35 }, { 0x020A, 0x0210, 0 },
    { 0x0400, 0x0404, 0 }, { 0x0405, 0x0405, 6 }, { 0x0406, 0x0406, 29 }, { 0x0410, 0x0410, 0 },
    { 0x0411, 0x0411, 6 }, { 0x0412, 0x0412, 29 }, { 0x0413, 0x0413, 7 }, { 0x0414, 0x0414, 35 },
    { 0x0415, 0x0416, 0 }, { 0x0417, 0x0417, 5 }, { 0x0418, 0x0418, 0 }, { 0x0419, 0x0419, 4 },
    { 0x041A, 0x041A, 0 }, { 0x041B, 0x041B, 7 }, { 0x041C, 0x041C, 6 }, { 0x041D, 0x041D, 21 },
    { 0x0420, 0x0420, 1 }, { 0x0421, 0x0421, 25 }, { 0x0422, 0x0422, 3 }, { 0x0423, 0x0423, 2 },
    { 0x0500, 0x0500, 0 }, { 0x0501, 0x0501, 29 }, { 0x0502, 0x0502, 7 }, { 0x0503, 0x0503, 6 },
    { 0x0504, 0x0504, 35 }, { 0x0505, 0x0505, 5 }, { 0x0506, 0x0506, 4 }, { 0x0507, 0x0507, 29 },
    { 0x0508, 0x0508, 30 }, { 0x0510, 0x0510, 3 },
    { 0x0600, 0x0601, 0 }, { 0x0620, 0x062F, 1 }, { 0x0630, 0x063F, 25 }, { 0x0640, 0x064F, 3 },
    { 0x0650, 0x065F, 2 }, { 0x0820, 0x082F, 1 }, { 0x0830, 0x083F, 25 }, { 0x0840, 0x084F, 3 },
    { 0x0920, 0x0920, 1 }, { 0x0930, 0x0930, 25 }, { 0x0931, 0x0931, 2 }, { 0x0940, 0x0940, 3 },
    { 0x0A01, 0x0A01, 1 }, { 0x0A02, 0x0A02, 7 }, { 0x0A03, 0x0A03, 2 }, { 0x0A04, 0x0A04, 0 },
    { 0x0A05, 0x0A05, 25 }, { 0x0A06, 0x0A06, 2 }, { 0x0A07, 0x0A07, 30 }, { 0x0A08, 0x0A08, 7 },
    { 0x0A09, 0x0A09, 2 }, { 0x0B01, 0x0B01, 0 }, { 0x0B02, 0x0B02, 1 }, { 0x0C01, 0x0C02, 0 },
};

/* NearestMacTextEncodings(generalEncoding, *bestMacEncoding, *alternateMacEncoding) */
static void tc_NearestMacTextEncodings(ppc_cpu *c)
{
    uint32_t base = te_base(R(0));
    int32_t mac = base <= 0xFF ? (int32_t)base : -1;
    for (unsigned i = 0; mac < 0 && i < sizeof nearest / sizeof nearest[0]; i++)
        if (base >= nearest[i].lo && base <= nearest[i].hi)
            mac = nearest[i].mac;
    if (mac < 0)
        RETURN(kTextUnsupportedEncodingErr);
    if (R(1))
        wr32(R(1), base <= 0xFF ? R(0) : (uint32_t)mac);
    if (R(2))
        wr32(R(2), kTextEncodingMacRoman);
    RETURN(noErr);
}

/* Names, for GetTextEncodingName and the Internet's charsets. */
static const struct {
    uint16_t base;
    const char *name;
} encoding_names[] = {
    { 0, "Mac OS Roman" }, { 1, "Mac OS Japanese" }, { 2, "Mac OS Traditional Chinese" }, { 3, "Mac OS Korean" },
    { 4, "Mac OS Arabic" }, { 5, "Mac OS Hebrew" }, { 6, "Mac OS Greek" }, { 7, "Mac OS Cyrillic" },
    { 9, "Mac OS Devanagari" }, { 10, "Mac OS Gurmukhi" }, { 11, "Mac OS Gujarati" }, { 21, "Mac OS Thai" },
    { 25, "Mac OS Simplified Chinese" }, { 29, "Mac OS Central European" }, { 30, "Mac OS Vietnamese" },
    { 33, "Mac OS Symbol" }, { 34, "Mac OS Dingbats" }, { 35, "Mac OS Turkish" }, { 36, "Mac OS Croatian" },
    { 37, "Mac OS Icelandic" }, { 38, "Mac OS Romanian" }, { 0x8C, "Mac OS Farsi" }, { 0x98, "Mac OS Ukrainian" },
    { 0x100, "Unicode" },
    { 0x201, "ISO Latin 1" }, { 0x202, "ISO Latin 2" }, { 0x203, "ISO Latin 3" }, { 0x204, "ISO Latin 4" },
    { 0x205, "ISO Cyrillic" }, { 0x206, "ISO Arabic" }, { 0x207, "ISO Greek" }, { 0x208, "ISO Hebrew" },
    { 0x209, "ISO Latin 5" }, { 0x20A, "ISO Latin 6" }, { 0x20D, "ISO Latin 7" }, { 0x20E, "ISO Latin 8" },
    { 0x20F, "ISO Latin 9" }, { 0x400, "DOS Latin US" }, { 0x410, "DOS Latin 1" }, { 0x41B, "DOS Russian" },
    { 0x420, "DOS Japanese" }, { 0x421, "DOS Simplified Chinese" }, { 0x422, "DOS Korean" },
    { 0x423, "DOS Traditional Chinese" },
    { 0x500, "Windows Latin 1" }, { 0x501, "Windows Latin 2" }, { 0x502, "Windows Cyrillic" },
    { 0x503, "Windows Greek" }, { 0x504, "Windows Latin 5" }, { 0x505, "Windows Hebrew" },
    { 0x506, "Windows Arabic" }, { 0x507, "Windows Baltic" }, { 0x508, "Windows Vietnamese" },
    { 0x600, "ASCII" }, { 0x820, "ISO-2022-JP" }, { 0x821, "ISO-2022-JP-2" }, { 0x830, "ISO-2022-CN" },
    { 0x840, "ISO-2022-KR" }, { 0x920, "EUC-JP" }, { 0x930, "EUC-CN" }, { 0x931, "EUC-TW" }, { 0x940, "EUC-KR" },
    { 0xA01, "Shift-JIS" }, { 0xA02, "KOI8-R" }, { 0xA03, "Big-5" }, { 0xA05, "HZ" }, { 0xA08, "KOI8-U" },
};

/* GetTextEncodingName(encoding, selector, preferredRegion, preferredEncoding, bufLen, *nameLength,
 *                     *actualRegion, *actualEncoding, name): in Mac OS Roman, U.S. English. */
static void tc_GetTextEncodingName(ppc_cpu *c)
{
    uint32_t e = R(0), sel = R(1), buflen = R(4);
    gaddr namelen = R(5), region = R(6), actual = R(7), out = arg(c, 8);
    const char *name = NULL;
    static const char *const formats[] = { "UTF-16", "UTF-7", "UTF-8", "UTF-32", "UTF-16BE", "UTF-16LE",
                                           "UTF-32BE", "UTF-32LE" };
    if (sel == 3) { /* kTextEncodingFormatName */
        if (is_unicode(e) && te_format(e) < 8)
            name = formats[te_format(e)];
    } else if (sel == 2) { /* kTextEncodingVariantName */
        name = "";
    } else {
        uint32_t base = is_unicode(e) ? 0x100 : te_base(e);
        for (unsigned i = 0; i < sizeof encoding_names / sizeof encoding_names[0]; i++)
            if (encoding_names[i].base == base)
                name = encoding_names[i].name;
    }
    if (!name)
        RETURN(kTECItemUnavailableErr);
    char full[64];
    if (sel == 0 && is_unicode(e) && te_format(e) < 8) /* kTextEncodingFullName */
        snprintf(full, sizeof full, "Unicode (%s)", formats[te_format(e)]);
    else
        snprintf(full, sizeof full, "%s", name);
    size_t n = strlen(full);
    if (out)
        gput(out, full, (uint32_t)(n < buflen ? n : buflen));
    if (namelen)
        wr32(namelen, (uint32_t)n);
    if (region)
        wr16(region, 0); /* verUS */
    if (actual)
        wr32(actual, kTextEncodingMacRoman);
    RETURN(n > buflen ? kTECArrayFullErr : noErr);
}

/* TECGetInfo(*tecInfo): a new handle, TEC 1.3.1's (the caller disposes of it). */
static void tc_TECGetInfo(ppc_cpu *c)
{
    gaddr h = tb_NewHandle(84, false, true);
    if (!h)
        RETURN(memFullErr);
    gaddr p = rd32(h);
    wr16(p + 0, 2);       /* kTECInfoCurrentFormat */
    wr16(p + 2, 0x0131);  /* tecVersion */
    wr32(p + 4, 0);
    wr32(p + 8, 0x0F);    /* the Unicode Converter's fixes through 1.3 */
    wr32(p + 12, 0);
    pstr_put(p + 16, "Text Encodings");
    pstr_put(p + 48, "Text Encoding Converter");
    wr16(p + 80, 0x0131);
    wr16(p + 82, 0x0131);
    if (R(0))
        wr32(R(0), h);
    RETURN(noErr);
}

/* ---------------------------------------------------------------------- */
/* UnicodeConverter */

/* A UnicodeMapping: unicodeEncoding, otherEncoding, mappingVersion. */
static void create_info(ppc_cpu *c, uint32_t tag, uint32_t unicode, uint32_t other, gaddr out)
{
    if (!out)
        RETURN(paramErr);
    wr32(out, 0);
    if (!is_unicode(unicode) || !supported(unicode) || !supported(other))
        RETURN(kTextUnsupportedEncodingErr);
    gaddr o = tag == TAG_T2U ? obj_new(tag, other, unicode, 0) : obj_new(tag, unicode, other, 0);
    if (!o)
        RETURN(memFullErr);
    wr32(out, o);
    RETURN(noErr);
}

/* CreateTextToUnicodeInfo(mapping, *info) */
static void uc_CreateTextToUnicodeInfo(ppc_cpu *c)
{
    gaddr m = R(0);
    if (!m)
        RETURN(paramErr);
    create_info(c, TAG_T2U, rd32(m), rd32(m + 4), R(1));
}

static void uc_CreateTextToUnicodeInfoByEncoding(ppc_cpu *c) { create_info(c, TAG_T2U, utf16(), R(0), R(1)); }

static void uc_CreateUnicodeToTextInfo(ppc_cpu *c)
{
    gaddr m = R(0);
    if (!m)
        RETURN(paramErr);
    create_info(c, TAG_U2T, rd32(m), rd32(m + 4), R(1));
}

static void uc_CreateUnicodeToTextInfoByEncoding(ppc_cpu *c) { create_info(c, TAG_U2T, utf16(), R(0), R(1)); }

/* Run infos: the encodings, from mappings (12 bytes each, the other
 * encoding at 4), encodings or script codes; none means the system's. */
static void create_run_info(ppc_cpu *c, uint32_t n, gaddr list, int kind, gaddr out)
{
    if (!out)
        RETURN(paramErr);
    wr32(out, 0);
    if (n > MAX_ENC)
        n = MAX_ENC;
    gaddr o = obj_new(TAG_RUN, utf16(), kTextEncodingMacRoman, n ? n : 1);
    if (!o)
        RETURN(memFullErr);
    for (uint32_t i = 0; i < n && list; i++) {
        uint32_t e = kind == 0 ? rd32(list + 12 * i + 4)
                   : kind == 1 ? rd32(list + 4 * i)
                               : mac_encoding_for_script((int16_t)rd16(list + 2 * i));
        if (kind == 0 && i == 0)
            wr32(o + OBJ_SRC, rd32(list));
        if (!supported(e)) {
            obj_dispose(o);
            RETURN(kTextUnsupportedEncodingErr);
        }
        wr32(o + OBJ_ENC + 4 * i, e);
    }
    if (!n || !list) {
        wr32(o + OBJ_NENC, 1);
        wr32(o + OBJ_ENC, kTextEncodingMacRoman);
    }
    wr32(out, o);
    RETURN(noErr);
}

static void uc_CreateUnicodeToTextRunInfo(ppc_cpu *c) { create_run_info(c, R(0), R(1), 0, R(2)); }
static void uc_CreateUnicodeToTextRunInfoByEncoding(ppc_cpu *c) { create_run_info(c, R(0), R(1), 1, R(2)); }
static void uc_CreateUnicodeToTextRunInfoByScriptCode(ppc_cpu *c) { create_run_info(c, R(0), R(1), 2, R(2)); }

/* Dispose...Info(*info): the info, and the reference cleared. */
static void dispose_info(ppc_cpu *c, uint32_t tag)
{
    gaddr p = R(0);
    if (!p || !obj_is(rd32(p), tag))
        RETURN(paramErr);
    obj_dispose(rd32(p));
    wr32(p, 0);
    RETURN(noErr);
}

static void uc_DisposeTextToUnicodeInfo(ppc_cpu *c) { dispose_info(c, TAG_T2U); }
static void uc_DisposeUnicodeToTextInfo(ppc_cpu *c) { dispose_info(c, TAG_U2T); }
static void uc_DisposeUnicodeToTextRunInfo(ppc_cpu *c) { dispose_info(c, TAG_RUN); }

/* Change...Info(info, mapping) */
static void change_info(ppc_cpu *c, uint32_t tag)
{
    gaddr o = R(0), m = R(1);
    if (!obj_is(o, tag) || !m)
        RETURN(paramErr);
    uint32_t unicode = rd32(m), other = rd32(m + 4);
    if (!is_unicode(unicode) || !supported(unicode) || !supported(other))
        RETURN(kTextUnsupportedEncodingErr);
    wr32(o + OBJ_SRC, tag == TAG_T2U ? other : unicode);
    wr32(o + OBJ_DST, tag == TAG_T2U ? unicode : other);
    wr8(o + OBJ_NPEND, 0);
    RETURN(noErr);
}

static void uc_ChangeTextToUnicodeInfo(ppc_cpu *c) { change_info(c, TAG_T2U); }
static void uc_ChangeUnicodeToTextInfo(ppc_cpu *c) { change_info(c, TAG_U2T); }

static void uc_ResetInfo(ppc_cpu *c)
{
    gaddr o = R(0);
    if (!obj_is(o, TAG_T2U) && !obj_is(o, TAG_U2T) && !obj_is(o, TAG_RUN))
        RETURN(paramErr);
    wr8(o + OBJ_NPEND, 0);
    RETURN(noErr);
}

/* Offsets wanted in the output: the guest's array read in. */
static uint32_t *read_offsets(gaddr a, uint32_t n)
{
    if (!a || !n)
        return NULL;
    uint32_t *v = malloc(n * sizeof *v);
    for (uint32_t i = 0; v && i < n; i++)
        v[i] = rd32(a + 4 * i);
    return v;
}

/* The status a Unicode Converter call returns for a conversion. */
static int32_t uc_status(int r, const conversion *j)
{
    switch (r) {
    case CV_FULL: return kTECOutputBufferFullStatus;
    case CV_PARTIAL: return kTECPartialCharErr;
    case CV_MALFORMED: return kTextMalformedInputErr;
    case CV_UNMAPPABLE: return kTECUnmappableElementErr;
    case CV_UNSUPPORTED: return kTextUnsupportedEncodingErr;
    default: return j->fell_back ? kTECUsedFallbacksStatus : noErr;
    }
}

/* The calls' common part: info, length, source, flags, offsetCount, offsets, *oOffsetCount,
 * oOffsets, outLen, *read, *outLength, output. */
static void uc_convert(ppc_cpu *c, uint32_t tag)
{
    gaddr o = arg(c, 0), src = arg(c, 2), in_off = arg(c, 5), n_out_off = arg(c, 6), out_off = arg(c, 7);
    uint32_t len = arg(c, 1), flags = arg(c, 3), n_in_off = arg(c, 4), cap = arg(c, 8);
    gaddr read = arg(c, 9), outlen = arg(c, 10), out = arg(c, 11);
    if (!obj_is(o, tag))
        RETURN(paramErr);
    conversion j = { .src = rd32(o + OBJ_SRC), .dst = rd32(o + OBJ_DST),
                     .fallbacks = (flags & kUnicodeUseFallbacksMask) != 0, .final = false };
    uint32_t *want = out_off && n_out_off ? read_offsets(in_off, n_in_off) : NULL;
    uint32_t *found = want ? calloc(n_in_off, sizeof *found) : NULL;
    if (want && found) {
        j.in_offsets = want;
        j.n_in_offsets = n_in_off;
        j.out_offsets = found;
    }
    int r = convert_guest(&j, src, len, out, cap, NULL, 0);
    if (read)
        wr32(read, (uint32_t)j.used);
    if (outlen)
        wr32(outlen, (uint32_t)j.produced);
    if (n_out_off)
        wr32(n_out_off, (uint32_t)j.n_out_offsets);
    for (size_t i = 0; found && i < j.n_out_offsets; i++)
        wr32(out_off + 4 * (gaddr)i, found[i]);
    free(want);
    free(found);
    RETURN(uc_status(r, &j));
}

static void uc_ConvertFromTextToUnicode(ppc_cpu *c) { uc_convert(c, TAG_T2U); }
static void uc_ConvertFromUnicodeToText(ppc_cpu *c) { uc_convert(c, TAG_U2T); }

/* Whether a character can be made in e: its bytes, or 0. */
static size_t encode_char(uint32_t e, const uint16_t *units, size_t nu, uint8_t *out, size_t cap, int *st)
{
    size_t u, p;
    *st = encode(e, units, nu, &u, out, cap, &p);
    return *st == HT_OK ? p : 0;
}

/* Text in several encodings: each character in the first of encs that has
 * it (the current one first, keeping the same encoding), runs recorded
 * as TextEncodingRuns (offset, encoding) in runs. */
typedef struct {
    conversion j;      /* src, fallbacks; used and produced */
    const uint32_t *encs;
    uint32_t nencs;
    bool keep_same;
    uint32_t current;  /* the run's encoding, or ~0 */
    gaddr runs;
    uint32_t max_runs, nruns;
    bool scripts;      /* ScriptCodeRuns (offset, a 2-byte script) */
} run_conversion;

static int convert_runs(run_conversion *rc, const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    size_t pos = 0, opos = 0;
    int result = CV_OK;
    while (pos < n) {
        uint16_t units[8];
        size_t nu, bytes, len = 0;
        int st = decode_one(rc->j.src, in + pos, n - pos, units, &nu, &bytes);
        if (st == HT_UNSUPPORTED) {
            result = CV_UNSUPPORTED;
            break;
        }
        if (st == HT_INCOMPLETE && !rc->j.final) {
            result = CV_PARTIAL;
            break;
        }
        uint8_t tmp[32];
        uint32_t enc = rc->current;
        bool found = false;
        if (st == HT_OK) {
            int est;
            if (rc->keep_same && enc != ~0u && (len = encode_char(enc, units, nu, tmp, sizeof tmp, &est)) > 0)
                found = true;
            for (uint32_t i = 0; !found && i < rc->nencs; i++)
                if ((len = encode_char(rc->encs[i], units, nu, tmp, sizeof tmp, &est)) > 0) {
                    enc = rc->encs[i];
                    found = true;
                }
        } else {
            bytes = 1;
        }
        if (!found) {
            if (!rc->j.fallbacks) {
                result = st == HT_OK ? CV_UNMAPPABLE : CV_MALFORMED;
                break;
            }
            if (enc == ~0u)
                enc = rc->encs[0];
            uint16_t q = '?';
            int est;
            len = encode_char(enc, &q, 1, tmp, sizeof tmp, &est);
            rc->j.fell_back = true;
        }
        if (opos + len > cap) {
            result = CV_FULL;
            break;
        }
        if (enc != rc->current) {
            if (rc->nruns >= rc->max_runs) {
                result = -1; /* the runs are full */
                break;
            }
            if (rc->runs) {
                gaddr r = rc->runs + (rc->scripts ? 6 : 8) * rc->nruns;
                wr32(r, (uint32_t)opos);
                if (rc->scripts) {
                    uint32_t b = te_base(enc);
                    wr16(r + 4, (uint16_t)(b <= 32 ? b : b == 0x8C ? 4 : b == 0x98 ? 7 : 0));
                } else {
                    wr32(r + 4, enc);
                }
            }
            rc->nruns++;
            rc->current = enc;
        }
        memcpy(out + opos, tmp, len);
        opos += len;
        pos += bytes;
    }
    rc->j.used = pos;
    rc->j.produced = opos;
    return result;
}

/* ConvertFromUnicodeToTextRun(info, len, source, flags, offsetCount, offsets, *oOffsetCount,
 *     oOffsets, outLen, *read, *outLength, output, runBufLen, *runCount, runs);
 * ConvertFromUnicodeToScriptCodeRun: the same, with ScriptCodeRuns. */
static void uc_convert_runs(ppc_cpu *c, bool scripts)
{
    gaddr o = arg(c, 0), src = arg(c, 2), n_out_off = arg(c, 6);
    uint32_t len = arg(c, 1), flags = arg(c, 3), cap = arg(c, 8);
    gaddr read = arg(c, 9), outlen = arg(c, 10), out = arg(c, 11), nruns = arg(c, 13), runs = arg(c, 14);
    if (!obj_is(o, TAG_RUN))
        RETURN(paramErr);
    uint32_t encs[MAX_ENC], n = obj_nenc(o);
    for (uint32_t i = 0; i < n; i++)
        encs[i] = rd32(o + OBJ_ENC + 4 * i);
    run_conversion rc = { .j = { .src = rd32(o + OBJ_SRC), .fallbacks = (flags & kUnicodeUseFallbacksMask) != 0 },
                          .encs = encs, .nencs = n, .keep_same = (flags & kUnicodeKeepSameEncodingMask) != 0,
                          .current = ~0u, .runs = runs, .max_runs = arg(c, 12), .scripts = scripts };
    uint8_t *s = malloc((size_t)len + 1), *d = malloc((size_t)cap + 1);
    int r = CV_FULL;
    if (s && d) {
        if (len)
            gget(s, src, len);
        r = convert_runs(&rc, s, len, d, cap);
        if (rc.j.produced)
            gput(out, d, (uint32_t)rc.j.produced);
    }
    free(s);
    free(d);
    if (read)
        wr32(read, (uint32_t)rc.j.used);
    if (outlen)
        wr32(outlen, (uint32_t)rc.j.produced);
    if (n_out_off)
        wr32(n_out_off, 0);
    if (nruns)
        wr32(nruns, rc.nruns);
    RETURN(r < 0 ? kTECArrayFullErr : uc_status(r, &rc.j));
}

static void uc_ConvertFromUnicodeToTextRun(ppc_cpu *c) { uc_convert_runs(c, false); }
static void uc_ConvertFromUnicodeToScriptCodeRun(ppc_cpu *c) { uc_convert_runs(c, true); }

/* TruncateForUnicodeToText(info, len, source, flags, maxLen, *truncatedLen):
 * not between a surrogate pair. */
static void uc_TruncateForUnicodeToText(ppc_cpu *c)
{
    uint32_t len = R(1), max = R(4) & ~1u;
    gaddr src = R(2);
    if (!R(5))
        RETURN(paramErr);
    uint32_t n = len < max ? len & ~1u : max;
    if (n < len && n >= 2 && is_high(rd16(src + n - 2)))
        n -= 2;
    wr32(R(5), n);
    RETURN(noErr);
}

/* TruncateForTextToUnicode(info, len, source, maxLen, *truncatedLen): at
 * the last whole character. */
static void uc_TruncateForTextToUnicode(ppc_cpu *c)
{
    gaddr o = R(0), src = R(2);
    uint32_t len = R(1), max = R(3);
    if (!obj_is(o, TAG_T2U) || !R(4))
        RETURN(paramErr);
    uint32_t n = len < max ? len : max;
    uint8_t *b = malloc((size_t)n + 1);
    size_t pos = 0;
    if (b) {
        gget(b, src, n);
        uint16_t u[8];
        size_t nu, bytes;
        while (pos < n && decode_one(rd32(o + OBJ_SRC), b + pos, n - pos, u, &nu, &bytes) == HT_OK)
            pos += bytes;
        if (pos < n && n == len)
            pos = n; /* the end of the text itself */
        free(b);
    }
    wr32(R(4), (uint32_t)pos);
    RETURN(noErr);
}

/* ConvertFromPStringToUnicode(info, pString, outLen, *unicodeLen, unicodeStr) */
static void uc_ConvertFromPStringToUnicode(ppc_cpu *c)
{
    gaddr o = R(0), p = R(1);
    if (!obj_is(o, TAG_T2U) || !p)
        RETURN(paramErr);
    conversion j = { .src = rd32(o + OBJ_SRC), .dst = rd32(o + OBJ_DST), .final = true };
    int r = convert_guest(&j, p + 1, rd8(p), R(4), R(2), NULL, 0);
    if (R(3))
        wr32(R(3), (uint32_t)j.produced);
    RETURN(uc_status(r, &j));
}

/* ConvertFromUnicodeToPString(info, unicodeLen, unicodeStr, pString) */
static void uc_ConvertFromUnicodeToPString(ppc_cpu *c)
{
    gaddr o = R(0), p = R(3);
    if (!obj_is(o, TAG_U2T) || !p)
        RETURN(paramErr);
    conversion j = { .src = rd32(o + OBJ_SRC), .dst = rd32(o + OBJ_DST), .final = true };
    int r = convert_guest(&j, R(2), R(1), p + 1, 255, NULL, 0);
    wr8(p, (uint8_t)j.produced);
    RETURN(uc_status(r, &j));
}

/* SetFallbackUnicodeToText(info, fallback, flags, infoPtr): ours are the
 * defaults, a program's own handler isn't called. */
static void uc_SetFallback(ppc_cpu *c) { RETURN(obj_is(R(0), TAG_U2T) || obj_is(R(0), TAG_RUN) ? noErr : paramErr); }

/* ---------------------------------------------------------------------- */
/* TextEncodingConverter */

/* The Internet's names: the preferred one first for each encoding. */
static const struct {
    uint32_t encoding;
    const char *name;
} internet_names[] = {
    { 0x0600, "US-ASCII" }, { 0x0600, "ascii" }, { 0x0600, "us" }, { 0x0600, "ANSI_X3.4-1968" },
    { 0x0600, "ISO646-US" },
    { 0x0201, "ISO-8859-1" }, { 0x0201, "ISO_8859-1" }, { 0x0201, "latin1" }, { 0x0201, "l1" },
    { 0x0201, "ISO8859-1" }, { 0x0202, "ISO-8859-2" }, { 0x0202, "latin2" }, { 0x0203, "ISO-8859-3" },
    { 0x0204, "ISO-8859-4" }, { 0x0205, "ISO-8859-5" }, { 0x0206, "ISO-8859-6" }, { 0x0207, "ISO-8859-7" },
    { 0x0208, "ISO-8859-8" }, { 0x0209, "ISO-8859-9" }, { 0x0209, "latin5" }, { 0x020A, "ISO-8859-10" },
    { 0x020D, "ISO-8859-13" }, { 0x020E, "ISO-8859-14" }, { 0x020F, "ISO-8859-15" }, { 0x0210, "ISO-8859-16" },
    { 0x0500, "windows-1252" }, { 0x0500, "x-cp1252" }, { 0x0500, "cp1252" }, { 0x0501, "windows-1250" },
    { 0x0501, "x-cp1250" }, { 0x0502, "windows-1251" }, { 0x0502, "x-cp1251" }, { 0x0503, "windows-1253" },
    { 0x0504, "windows-1254" }, { 0x0505, "windows-1255" }, { 0x0506, "windows-1256" },
    { 0x0507, "windows-1257" }, { 0x0508, "windows-1258" },
    { 0x0000, "macintosh" }, { 0x0000, "x-mac-roman" }, { 0x0000, "mac" }, { 0x0000, "csMacintosh" },
    { 0x0001, "x-mac-japanese" }, { 0x0002, "x-mac-trad-chinese" }, { 0x0002, "x-mac-chinesetrad" },
    { 0x0003, "x-mac-korean" }, { 0x0004, "x-mac-arabic" }, { 0x0005, "x-mac-hebrew" },
    { 0x0006, "x-mac-greek" }, { 0x0007, "x-mac-cyrillic" }, { 0x0015, "x-mac-thai" },
    { 0x0019, "x-mac-simp-chinese" }, { 0x0019, "x-mac-chinesesimp" }, { 0x001D, "x-mac-centraleurroman" },
    { 0x001D, "x-mac-ce" }, { 0x0023, "x-mac-turkish" }, { 0x0024, "x-mac-croatian" },
    { 0x0025, "x-mac-icelandic" }, { 0x0026, "x-mac-romanian" }, { 0x0098, "x-mac-ukrainian" },
    { 0x0A01, "Shift_JIS" }, { 0x0A01, "x-sjis" }, { 0x0A01, "sjis" }, { 0x0A01, "shift-jis" },
    { 0x0A01, "MS_Kanji" }, { 0x0A01, "csShiftJIS" }, { 0x0420, "Windows-31J" }, { 0x0420, "cp932" },
    { 0x0920, "EUC-JP" }, { 0x0920, "x-euc-jp" }, { 0x0920, "euc_jp" }, { 0x0920, "x-euc" },
    { 0x0820, "ISO-2022-JP" }, { 0x0820, "csISO2022JP" }, { 0x0820, "jis" }, { 0x0821, "ISO-2022-JP-2" },
    { 0x0840, "ISO-2022-KR" }, { 0x0830, "ISO-2022-CN" },
    { 0x0A03, "Big5" }, { 0x0A03, "x-x-big5" }, { 0x0A03, "cn-big5" }, { 0x0A03, "csBig5" },
    { 0x0A06, "Big5-HKSCS" }, { 0x0423, "cp950" },
    { 0x0930, "GB2312" }, { 0x0930, "EUC-CN" }, { 0x0930, "x-euc-cn" }, { 0x0930, "csGB2312" },
    { 0x0930, "GB_2312-80" }, { 0x0930, "chinese" }, { 0x0421, "GBK" }, { 0x0421, "cp936" },
    { 0x0632, "GB18030" }, { 0x0A05, "HZ-GB-2312" }, { 0x0931, "EUC-TW" },
    { 0x0940, "EUC-KR" }, { 0x0940, "csEUCKR" }, { 0x0422, "ks_c_5601-1987" }, { 0x0422, "cp949" },
    { 0x0940, "korean" },
    { 0x0A02, "KOI8-R" }, { 0x0A02, "csKOI8R" }, { 0x0A08, "KOI8-U" }, { 0x0A07, "VISCII" },
    { 0x0400, "IBM437" }, { 0x0400, "cp437" }, { 0x0410, "IBM850" }, { 0x0410, "cp850" }, { 0x0412, "IBM852" },
    { 0x041B, "IBM866" }, { 0x041B, "cp866" },
    { 0x08000100, "UTF-8" }, { 0x08000100, "utf8" }, { 0x08000100, "x-unicode-2-0-utf-8" },
    { 0x00000100, "UTF-16" }, { 0x00000100, "ISO-10646-UCS-2" }, { 0x00000100, "csUnicode" },
    { 0x10000100, "UTF-16BE" }, { 0x14000100, "UTF-16LE" }, { 0x04000100, "UTF-7" },
    { 0x0C000100, "UTF-32" }, { 0x18000100, "UTF-32BE" }, { 0x1C000100, "UTF-32LE" },
};

/* TECGetTextEncodingInternetName(encoding, name) */
static void tec_TECGetTextEncodingInternetName(ppc_cpu *c)
{
    uint32_t e = R(0);
    uint32_t key = is_unicode(e) ? (e & 0xFC000000u) | 0x100 : te_base(e);
    for (unsigned i = 0; i < sizeof internet_names / sizeof internet_names[0]; i++)
        if (internet_names[i].encoding == key) {
            if (R(1))
                pstr_put(R(1), internet_names[i].name);
            RETURN(noErr);
        }
    RETURN(kTextUnsupportedEncodingErr);
}

/* TECGetTextEncodingFromInternetName(*encoding, name) */
static void tec_TECGetTextEncodingFromInternetName(ppc_cpu *c)
{
    gaddr name = R(1);
    if (!R(0) || !name)
        RETURN(paramErr);
    for (unsigned i = 0; i < sizeof internet_names / sizeof internet_names[0]; i++)
        if (pstr_is(name, internet_names[i].name)) {
            wr32(R(0), internet_names[i].encoding);
            RETURN(noErr);
        }
    RETURN(kTextUnsupportedEncodingErr);
}

/* What there is: these, where the host has them. */
static const uint32_t candidates[] = {
    0x0000, 0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007, 0x0015, 0x0019, 0x001D, 0x0021, 0x0022,
    0x0023, 0x0024, 0x0025, 0x0026, 0x008C, 0x0098,
    0x0100, 0x08000100, 0x04000100,
    0x0201, 0x0202, 0x0203, 0x0204, 0x0205, 0x0206, 0x0207, 0x0208, 0x0209, 0x020A, 0x020D, 0x020E, 0x020F,
    0x0400, 0x0405, 0x0406, 0x0410, 0x0412, 0x0413, 0x0414, 0x0417, 0x0419, 0x041B, 0x041D, 0x0420, 0x0421,
    0x0422, 0x0423, 0x0500, 0x0501, 0x0502, 0x0503, 0x0504, 0x0505, 0x0506, 0x0507, 0x0508,
    0x0600, 0x0631, 0x0632, 0x0820, 0x0821, 0x0830, 0x0840, 0x0920, 0x0930, 0x0931, 0x0940,
    0x0A01, 0x0A02, 0x0A03, 0x0A05, 0x0A06, 0x0A08,
};
static const uint32_t web_encodings[] = {
    0x0201, 0x0000, 0x08000100, 0x0500, 0x0A01, 0x0920, 0x0820, 0x0A03, 0x0930, 0x0A05, 0x0940, 0x0840,
    0x0A02, 0x0202, 0x0501, 0x0205, 0x0502, 0x0207, 0x0503, 0x0209, 0x0208, 0x0505, 0x0206, 0x0506,
};
static const uint32_t mail_encodings[] = {
    0x0600, 0x0201, 0x08000100, 0x0820, 0x0A03, 0x0930, 0x0A05, 0x0940, 0x0A02, 0x0202, 0x0205, 0x0207, 0x0209,
    0x0208,
};

/* Count and list (*count, or encodings[max] and *actual) the supported ones of a set. */
static void list_supported(ppc_cpu *c, const uint32_t *set, unsigned n, gaddr list, uint32_t max, gaddr count)
{
    uint32_t k = 0, total = 0;
    for (unsigned i = 0; i < n; i++) {
        if (!supported(set[i]))
            continue;
        if (list && k < max)
            wr32(list + 4 * k++, set[i]);
        total++;
    }
    if (count)
        wr32(count, list ? k : total);
    RETURN(list && total > max ? kTECArrayFullErr : noErr);
}

#define NELEM(a) (unsigned)(sizeof a / sizeof a[0])
static void tec_TECCountAvailableTextEncodings(ppc_cpu *c) { list_supported(c, candidates, NELEM(candidates), 0, 0, R(0)); }
static void tec_TECGetAvailableTextEncodings(ppc_cpu *c) { list_supported(c, candidates, NELEM(candidates), R(0), R(1), R(2)); }
static void tec_TECCountWebTextEncodings(ppc_cpu *c) { list_supported(c, web_encodings, NELEM(web_encodings), 0, 0, R(1)); }
static void tec_TECGetWebTextEncodings(ppc_cpu *c) { list_supported(c, web_encodings, NELEM(web_encodings), R(1), R(2), R(3)); }
static void tec_TECCountMailTextEncodings(ppc_cpu *c) { list_supported(c, mail_encodings, NELEM(mail_encodings), 0, 0, R(1)); }
static void tec_TECGetMailTextEncodings(ppc_cpu *c) { list_supported(c, mail_encodings, NELEM(mail_encodings), R(1), R(2), R(3)); }
static void tec_TECCountAvailableSniffers(ppc_cpu *c) { list_supported(c, candidates, NELEM(candidates), 0, 0, R(0)); }
static void tec_TECGetAvailableSniffers(ppc_cpu *c) { list_supported(c, candidates, NELEM(candidates), R(0), R(1), R(2)); }

/* Direct conversions: each encoding to and from Unicode (TECConversionInfo:
 * source, destination, two reserved words). */
static void direct_conversions(ppc_cpu *c, gaddr list, uint32_t max, gaddr count)
{
    uint32_t k = 0, total = 0;
    for (unsigned i = 0; i < NELEM(candidates); i++) {
        if (is_unicode(candidates[i]) || !supported(candidates[i]))
            continue;
        for (int dir = 0; dir < 2; dir++) {
            if (list && k < max) {
                gaddr r = list + 12 * k++;
                wr32(r, dir ? utf16() : candidates[i]);
                wr32(r + 4, dir ? candidates[i] : utf16());
                wr32(r + 8, 0);
            }
            total++;
        }
    }
    if (count)
        wr32(count, list ? k : total);
    RETURN(list && total > max ? kTECArrayFullErr : noErr);
}

static void tec_TECCountDirectTextEncodingConversions(ppc_cpu *c) { direct_conversions(c, 0, 0, R(0)); }
static void tec_TECGetDirectTextEncodingConversions(ppc_cpu *c) { direct_conversions(c, R(0), R(1), R(2)); }

/* Destinations from an encoding: Unicode from any, any from Unicode. */
static void destinations(ppc_cpu *c, uint32_t from, gaddr list, uint32_t max, gaddr count)
{
    if (!supported(from))
        RETURN(kTextUnsupportedEncodingErr);
    if (!is_unicode(from)) {
        if (list && max)
            wr32(list, utf16());
        if (count)
            wr32(count, 1);
        RETURN(list && !max ? kTECArrayFullErr : noErr);
    }
    uint32_t k = 0, total = 0;
    for (unsigned i = 0; i < NELEM(candidates); i++) {
        if (is_unicode(candidates[i]) || !supported(candidates[i]))
            continue;
        if (list && k < max)
            wr32(list + 4 * k++, candidates[i]);
        total++;
    }
    if (count)
        wr32(count, list ? k : total);
    RETURN(list && total > max ? kTECArrayFullErr : noErr);
}

static void tec_TECCountDestinationTextEncodings(ppc_cpu *c) { destinations(c, R(0), 0, 0, R(1)); }
static void tec_TECGetDestinationTextEncodings(ppc_cpu *c) { destinations(c, R(0), R(1), R(2), R(3)); }

/* Sub-encodings (ISO-2022's parts): none listed. */
static void tec_TECCountSubTextEncodings(ppc_cpu *c)
{
    if (R(1))
        wr32(R(1), 0);
    RETURN(noErr);
}

static void tec_TECGetSubTextEncodings(ppc_cpu *c)
{
    if (R(3))
        wr32(R(3), 0);
    RETURN(noErr);
}

static void new_converter(ppc_cpu *c, gaddr out, uint32_t from, const uint32_t *to, uint32_t nto)
{
    if (!out || !nto)
        RETURN(paramErr);
    wr32(out, 0);
    if (!supported(from))
        RETURN(kTECNoConversionPathErr);
    for (uint32_t i = 0; i < nto; i++)
        if (!supported(to[i]))
            RETURN(kTECNoConversionPathErr);
    gaddr o = obj_new(TAG_TEC, from, to[0], nto);
    if (!o)
        RETURN(memFullErr);
    for (uint32_t i = 0; i < nto && i < MAX_ENC; i++)
        wr32(o + OBJ_ENC + 4 * i, to[i]);
    wr32(out, o);
    RETURN(noErr);
}

/* TECCreateConverter(*converter, inputEncoding, outputEncoding) */
static void tec_TECCreateConverter(ppc_cpu *c)
{
    uint32_t to = R(2);
    new_converter(c, R(0), R(1), &to, 1);
}

/* TECCreateConverterFromPath(*converter, path[], count): its ends. */
static void tec_TECCreateConverterFromPath(ppc_cpu *c)
{
    gaddr path = R(1);
    uint32_t n = R(2);
    if (!path || n < 2)
        RETURN(paramErr);
    uint32_t to = rd32(path + 4 * (n - 1));
    new_converter(c, R(0), rd32(path), &to, 1);
}

/* TECCreateOneToManyConverter(*converter, inputEncoding, count, outputEncodings[]) */
static void tec_TECCreateOneToManyConverter(ppc_cpu *c)
{
    uint32_t n = R(2) > MAX_ENC ? MAX_ENC : R(2), to[MAX_ENC];
    if (!R(3))
        RETURN(paramErr);
    for (uint32_t i = 0; i < n; i++)
        to[i] = rd32(R(3) + 4 * i);
    new_converter(c, R(0), R(1), to, n);
}

static void tec_TECDisposeConverter(ppc_cpu *c)
{
    if (!obj_is(R(0), TAG_TEC))
        RETURN(kTECCorruptConverterErr);
    obj_dispose(R(0));
    RETURN(noErr);
}

static void tec_TECClearConverterContextInfo(ppc_cpu *c)
{
    if (!obj_is(R(0), TAG_TEC))
        RETURN(kTECCorruptConverterErr);
    wr8(R(0) + OBJ_NPEND, 0);
    RETURN(noErr);
}

/* TECSetBasicOptions(converter, controlFlags) */
static void tec_TECSetBasicOptions(ppc_cpu *c)
{
    if (!obj_is(R(0), TAG_TEC))
        RETURN(kTECCorruptConverterErr);
    wr32(R(0) + OBJ_OPTIONS, R(1));
    RETURN(noErr);
}

/* TECGetEncodingList(converter, *count, *handle): the path, a new handle. */
static void tec_TECGetEncodingList(ppc_cpu *c)
{
    gaddr o = R(0);
    if (!obj_is(o, TAG_TEC))
        RETURN(kTECCorruptConverterErr);
    gaddr h = tb_NewHandle(8, false, true);
    if (!h)
        RETURN(memFullErr);
    wr32(rd32(h), rd32(o + OBJ_SRC));
    wr32(rd32(h) + 4, rd32(o + OBJ_DST));
    if (R(1))
        wr32(R(1), 2);
    if (R(2))
        wr32(R(2), h);
    RETURN(noErr);
}

static int32_t tec_status(int r, const conversion *j)
{
    switch (r) {
    case CV_FULL: return kTECOutputBufferFullStatus;
    case CV_MALFORMED: return kTextMalformedInputErr;
    case CV_UNMAPPABLE: return kTECUnmappableElementErr;
    case CV_UNSUPPORTED: return kTECNoConversionPathErr;
    default: return j->fell_back ? kTECUsedFallbacksStatus : noErr; /* a partial character kept */
    }
}

/* After a converter's call: what of the input it took (the pending bytes it
 * started with aren't the caller's), and the cut-short character kept. */
static uint32_t settle_pending(gaddr o, const uint8_t *pend, size_t npend, gaddr in, uint32_t len, size_t used,
                               int r)
{
    size_t total = npend + len, taken = used;
    if (r == CV_PARTIAL && total - used <= MAX_PEND) {
        uint8_t keep[MAX_PEND];
        for (size_t i = used; i < total; i++)
            keep[i - used] = i < npend ? pend[i] : rd8(in + (gaddr)(i - npend));
        gput(o + OBJ_PEND, keep, (uint32_t)(total - used));
        wr8(o + OBJ_NPEND, (uint8_t)(total - used));
        taken = total;
    } else if (used < npend) {
        /* Stopped inside what was kept: keep the rest of it. */
        gput(o + OBJ_PEND, pend + used, (uint32_t)(npend - used));
        wr8(o + OBJ_NPEND, (uint8_t)(npend - used));
        return 0;
    } else {
        wr8(o + OBJ_NPEND, 0);
    }
    return (uint32_t)(taken - npend);
}

static conversion converter_job(gaddr o, bool final)
{
    return (conversion){ .src = rd32(o + OBJ_SRC), .dst = rd32(o + OBJ_DST),
                         .fallbacks = !(rd32(o + OBJ_OPTIONS) & kTECDisableFallbacksMask), .final = final };
}

/* TECConvertText(converter, input, inputLength, *actualInputLength, output, outputLength,
 *                *actualOutputLength) */
static void tec_TECConvertText(ppc_cpu *c)
{
    gaddr o = R(0), in = R(1), actual_in = R(3), out = R(4), actual_out = R(6);
    uint32_t len = R(2), cap = R(5);
    if (!obj_is(o, TAG_TEC))
        RETURN(kTECCorruptConverterErr);
    uint8_t pend[MAX_PEND];
    size_t npend = rd8(o + OBJ_NPEND);
    if (npend > MAX_PEND)
        npend = 0;
    gget(pend, o + OBJ_PEND, (uint32_t)npend);
    conversion j = converter_job(o, false);
    int r = convert_guest(&j, in, len, out, cap, pend, npend);
    uint32_t took = settle_pending(o, pend, npend, in, len, j.used, r);
    if (actual_in)
        wr32(actual_in, took);
    if (actual_out)
        wr32(actual_out, (uint32_t)j.produced);
    RETURN(tec_status(r, &j));
}

/* TECFlushText(converter, output, outputLength, *actualOutputLength): the
 * kept bytes, as fallbacks. */
static void tec_TECFlushText(ppc_cpu *c)
{
    gaddr o = R(0), out = R(1), actual_out = R(3);
    if (!obj_is(o, TAG_TEC))
        RETURN(kTECCorruptConverterErr);
    uint8_t pend[MAX_PEND];
    size_t npend = rd8(o + OBJ_NPEND);
    if (npend > MAX_PEND)
        npend = 0;
    gget(pend, o + OBJ_PEND, (uint32_t)npend);
    conversion j = converter_job(o, true);
    int r = convert_guest(&j, 0, 0, out, R(2), pend, npend);
    settle_pending(o, pend, npend, 0, 0, j.used, r);
    if (actual_out)
        wr32(actual_out, (uint32_t)j.produced);
    RETURN(tec_status(r, &j));
}

/* TECConvertTextToMultipleEncodings(converter, input, inputLength, *actualInputLength,
 *     output, outputLength, *actualOutputLength, runs[], maxRuns, *actualRuns) */
static void multiple(ppc_cpu *c, bool flush)
{
    gaddr o = arg(c, 0);
    gaddr in = flush ? 0 : arg(c, 1), actual_in = flush ? 0 : arg(c, 3);
    uint32_t len = flush ? 0 : arg(c, 2);
    unsigned a = flush ? 1 : 4; /* where output starts */
    gaddr out = arg(c, a), actual_out = arg(c, a + 2), runs = arg(c, a + 3), actual_runs = arg(c, a + 5);
    uint32_t cap = arg(c, a + 1), max_runs = arg(c, a + 4);
    if (!obj_is(o, TAG_TEC))
        RETURN(kTECCorruptConverterErr);
    uint32_t encs[MAX_ENC], n = obj_nenc(o);
    for (uint32_t i = 0; i < n; i++)
        encs[i] = rd32(o + OBJ_ENC + 4 * i);
    uint8_t pend[MAX_PEND];
    size_t npend = rd8(o + OBJ_NPEND);
    if (npend > MAX_PEND)
        npend = 0;
    gget(pend, o + OBJ_PEND, (uint32_t)npend);
    run_conversion rc = { .j = converter_job(o, flush), .encs = encs, .nencs = n, .keep_same = true,
                          .current = ~0u, .runs = runs, .max_runs = max_runs };
    uint8_t *s = malloc(npend + len + 1), *d = malloc((size_t)cap + 1);
    int r = CV_FULL;
    if (s && d) {
        memcpy(s, pend, npend);
        if (len)
            gget(s + npend, in, len);
        r = convert_runs(&rc, s, npend + len, d, cap);
        if (rc.j.produced)
            gput(out, d, (uint32_t)rc.j.produced);
    }
    free(s);
    free(d);
    uint32_t took = settle_pending(o, pend, npend, in, len, rc.j.used, r);
    if (actual_in)
        wr32(actual_in, took);
    if (actual_out)
        wr32(actual_out, (uint32_t)rc.j.produced);
    if (actual_runs)
        wr32(actual_runs, rc.nruns);
    RETURN(r < 0 ? kTECArrayFullErr : tec_status(r, &rc.j));
}

static void tec_TECConvertTextToMultipleEncodings(ppc_cpu *c) { multiple(c, false); }

/* TECFlushMultipleEncodings(converter, output, outputLength, *actualOutputLength,
 *                           runs[], maxRuns, *actualRuns) */
static void tec_TECFlushMultipleEncodings(ppc_cpu *c) { multiple(c, true); }

/* ---- The sniffer ---- */

/* TECCreateSniffer(*sniffer, encodings[], count) */
static void tec_TECCreateSniffer(ppc_cpu *c)
{
    gaddr out = R(0), list = R(1);
    uint32_t n = R(2) > MAX_ENC ? MAX_ENC : R(2);
    if (!out)
        RETURN(paramErr);
    wr32(out, 0);
    gaddr o = obj_new(TAG_SNIFF, 0, 0, n);
    if (!o)
        RETURN(memFullErr);
    for (uint32_t i = 0; i < n && list; i++) {
        uint32_t e = rd32(list + 4 * i);
        if (!supported(e)) {
            obj_dispose(o);
            RETURN(kTextUnsupportedEncodingErr);
        }
        wr32(o + OBJ_ENC + 4 * i, e);
    }
    wr32(out, o);
    RETURN(noErr);
}

static void tec_TECDisposeSniffer(ppc_cpu *c)
{
    if (!obj_is(R(0), TAG_SNIFF))
        RETURN(paramErr);
    obj_dispose(R(0));
    RETURN(noErr);
}

static void tec_TECClearSnifferContextInfo(ppc_cpu *c) { RETURN(obj_is(R(0), TAG_SNIFF) ? noErr : paramErr); }

/* How well text reads as an encoding: the bytes that aren't characters in
 * it, and its multibyte characters and shifts (bytes beyond the UTF-16
 * units they made), each counted up to a limit. */
static void sniff(uint32_t e, const uint8_t *b, size_t n, uint32_t max_errs, uint32_t max_features, uint32_t *errs,
                  uint32_t *features)
{
    size_t cap = n + 8, pos = 0;
    uint16_t *u = malloc(cap * sizeof *u);
    *errs = *features = 0;
    while (u && pos < n && *errs < max_errs) {
        size_t used, k;
        int st = decode(e, b + pos, n - pos, &used, u, cap, &k);
        if (used > k)
            *features += (uint32_t)(used - k);
        pos += used;
        if (st == HT_INVALID) {
            (*errs)++;
            pos++;
        } else if (st != HT_FULL) {
            if (st == HT_UNSUPPORTED)
                *errs = max_errs;
            break;
        }
    }
    free(u);
    if (*features > max_features)
        *features = max_features;
}

/* TECSniffTextEncoding(sniffer, input, length, encodings[], count, numErrs[], maxErrs,
 *                      numFeatures[], maxFeatures): the encodings put in order, the likeliest
 * (fewest errors, then most features) first. */
static void tec_TECSniffTextEncoding(ppc_cpu *c)
{
    gaddr o = arg(c, 0), in = arg(c, 1), encs = arg(c, 3), errs = arg(c, 5), feats = arg(c, 7);
    uint32_t len = arg(c, 2), n = arg(c, 4), max_errs = arg(c, 6), max_feats = arg(c, 8);
    if (!obj_is(o, TAG_SNIFF) || !encs)
        RETURN(paramErr);
    if (n > MAX_ENC)
        n = MAX_ENC;
    uint8_t *b = malloc((size_t)len + 1);
    if (!b)
        RETURN(memFullErr);
    if (len)
        gget(b, in, len);
    uint32_t e[MAX_ENC], ne[MAX_ENC], nf[MAX_ENC];
    for (uint32_t i = 0; i < n; i++) {
        e[i] = rd32(encs + 4 * i);
        sniff(e[i], b, len, max_errs ? max_errs : 1, max_feats, &ne[i], &nf[i]);
    }
    free(b);
    /* A stable insertion sort. */
    for (uint32_t i = 1; i < n; i++)
        for (uint32_t k = i; k > 0 && (ne[k] < ne[k - 1] || (ne[k] == ne[k - 1] && nf[k] > nf[k - 1])); k--) {
            uint32_t t;
            t = e[k], e[k] = e[k - 1], e[k - 1] = t;
            t = ne[k], ne[k] = ne[k - 1], ne[k - 1] = t;
            t = nf[k], nf[k] = nf[k - 1], nf[k - 1] = t;
        }
    for (uint32_t i = 0; i < n; i++) {
        wr32(encs + 4 * i, e[i]);
        if (errs)
            wr32(errs + 4 * i, ne[i]);
        if (feats)
            wr32(feats + 4 * i, nf[i]);
    }
    RETURN(noErr);
}

/* ---------------------------------------------------------------------- */

/* ---------------------------------------------------------------------- */
/* For Core Foundation's strings (carbon_cfstring.c) */

bool tec_encoding_available(uint32_t e) { return supported(e); }

int tec_decode(uint32_t e, const uint8_t *in, size_t n, size_t *used, uint16_t *out, size_t cap, size_t *produced)
{
    return decode(e, in, n, used, out, cap, produced);
}

int tec_encode(uint32_t e, const uint16_t *in, size_t n, size_t *used, uint8_t *out, size_t cap, size_t *produced)
{
    return encode(e, in, n, used, out, cap, produced);
}

/* An encoding's name as GetTextEncodingName gives it, or NULL. */
const char *tec_encoding_name(uint32_t e)
{
    uint32_t base = is_unicode(e) ? 0x100 : te_base(e);
    for (unsigned i = 0; i < NELEM(encoding_names); i++)
        if (encoding_names[i].base == base)
            return encoding_names[i].name;
    return NULL;
}

/* The Internet's preferred name for an encoding, or NULL. */
const char *tec_internet_name(uint32_t e)
{
    uint32_t key = is_unicode(e) ? (e & 0xFC000000u) | 0x100 : te_base(e);
    for (unsigned i = 0; i < NELEM(internet_names); i++)
        if (internet_names[i].encoding == key)
            return internet_names[i].name;
    return NULL;
}

/* An encoding from any of its Internet names, ignoring case. */
bool tec_encoding_of_internet_name(const char *name, uint32_t *e)
{
    for (unsigned i = 0; i < NELEM(internet_names); i++) {
        const char *a = internet_names[i].name, *b = name;
        while (*a && *b && (*a | 0x20) == (*b | 0x20))
            a++, b++;
        if (!*a && !*b) {
            *e = internet_names[i].encoding;
            return true;
        }
    }
    return false;
}

/* The encodings there are (up to max of them); how many. */
unsigned tec_available_encodings(uint32_t *out, unsigned max)
{
    unsigned k = 0;
    for (unsigned i = 0; i < NELEM(candidates); i++)
        if (supported(candidates[i]) && k < max)
            out[k++] = candidates[i];
    return k;
}

const ppc_export tec_exports[] = {
    /* TextCommon */
    { "CreateTextEncoding", tc_CreateTextEncoding },
    { "GetScriptInfoFromTextEncoding", tc_GetScriptInfoFromTextEncoding },
    { "GetTextEncodingBase", tc_GetTextEncodingBase },
    { "GetTextEncodingFormat", tc_GetTextEncodingFormat },
    { "GetTextEncodingFromScriptInfo", tc_GetTextEncodingFromScriptInfo },
    { "GetTextEncodingName", tc_GetTextEncodingName },
    { "GetTextEncodingVariant", tc_GetTextEncodingVariant },
    { "NearestMacTextEncodings", tc_NearestMacTextEncodings },
    { "ResolveDefaultTextEncoding", tc_ResolveDefaultTextEncoding },
    { "RevertTextEncodingToScriptInfo", tc_RevertTextEncodingToScriptInfo },
    { "TECGetInfo", tc_TECGetInfo },
    { "UpgradeScriptInfoToTextEncoding", tc_UpgradeScriptInfoToTextEncoding },
    /* UnicodeConverter */
    { "ChangeTextToUnicodeInfo", uc_ChangeTextToUnicodeInfo },
    { "ChangeUnicodeToTextInfo", uc_ChangeUnicodeToTextInfo },
    { "ConvertFromPStringToUnicode", uc_ConvertFromPStringToUnicode },
    { "ConvertFromTextToUnicode", uc_ConvertFromTextToUnicode },
    { "ConvertFromUnicodeToPString", uc_ConvertFromUnicodeToPString },
    { "ConvertFromUnicodeToScriptCodeRun", uc_ConvertFromUnicodeToScriptCodeRun },
    { "ConvertFromUnicodeToText", uc_ConvertFromUnicodeToText },
    { "ConvertFromUnicodeToTextRun", uc_ConvertFromUnicodeToTextRun },
    { "CreateTextToUnicodeInfo", uc_CreateTextToUnicodeInfo },
    { "CreateTextToUnicodeInfoByEncoding", uc_CreateTextToUnicodeInfoByEncoding },
    { "CreateUnicodeToTextInfo", uc_CreateUnicodeToTextInfo },
    { "CreateUnicodeToTextInfoByEncoding", uc_CreateUnicodeToTextInfoByEncoding },
    { "CreateUnicodeToTextRunInfo", uc_CreateUnicodeToTextRunInfo },
    { "CreateUnicodeToTextRunInfoByEncoding", uc_CreateUnicodeToTextRunInfoByEncoding },
    { "CreateUnicodeToTextRunInfoByScriptCode", uc_CreateUnicodeToTextRunInfoByScriptCode },
    { "DisposeTextToUnicodeInfo", uc_DisposeTextToUnicodeInfo },
    { "DisposeUnicodeToTextInfo", uc_DisposeUnicodeToTextInfo },
    { "DisposeUnicodeToTextRunInfo", uc_DisposeUnicodeToTextRunInfo },
    { "ResetTextToUnicodeInfo", uc_ResetInfo },
    { "ResetUnicodeToTextInfo", uc_ResetInfo },
    { "ResetUnicodeToTextRunInfo", uc_ResetInfo },
    { "SetFallbackUnicodeToText", uc_SetFallback },
    { "SetFallbackUnicodeToTextRun", uc_SetFallback },
    { "TruncateForTextToUnicode", uc_TruncateForTextToUnicode },
    { "TruncateForUnicodeToText", uc_TruncateForUnicodeToText },
    /* TextEncodingConverter */
    { "TECClearConverterContextInfo", tec_TECClearConverterContextInfo },
    { "TECClearSnifferContextInfo", tec_TECClearSnifferContextInfo },
    { "TECConvertText", tec_TECConvertText },
    { "TECConvertTextToMultipleEncodings", tec_TECConvertTextToMultipleEncodings },
    { "TECCountAvailableSniffers", tec_TECCountAvailableSniffers },
    { "TECCountAvailableTextEncodings", tec_TECCountAvailableTextEncodings },
    { "TECCountDestinationTextEncodings", tec_TECCountDestinationTextEncodings },
    { "TECCountDirectTextEncodingConversions", tec_TECCountDirectTextEncodingConversions },
    { "TECCountMailTextEncodings", tec_TECCountMailTextEncodings },
    { "TECCountSubTextEncodings", tec_TECCountSubTextEncodings },
    { "TECCountWebTextEncodings", tec_TECCountWebTextEncodings },
    { "TECCreateConverter", tec_TECCreateConverter },
    { "TECCreateConverterFromPath", tec_TECCreateConverterFromPath },
    { "TECCreateOneToManyConverter", tec_TECCreateOneToManyConverter },
    { "TECCreateSniffer", tec_TECCreateSniffer },
    { "TECDisposeConverter", tec_TECDisposeConverter },
    { "TECDisposeSniffer", tec_TECDisposeSniffer },
    { "TECFlushMultipleEncodings", tec_TECFlushMultipleEncodings },
    { "TECFlushText", tec_TECFlushText },
    { "TECGetAvailableSniffers", tec_TECGetAvailableSniffers },
    { "TECGetAvailableTextEncodings", tec_TECGetAvailableTextEncodings },
    { "TECGetDestinationTextEncodings", tec_TECGetDestinationTextEncodings },
    { "TECGetDirectTextEncodingConversions", tec_TECGetDirectTextEncodingConversions },
    { "TECGetEncodingList", tec_TECGetEncodingList },
    { "TECGetMailTextEncodings", tec_TECGetMailTextEncodings },
    { "TECGetSubTextEncodings", tec_TECGetSubTextEncodings },
    { "TECGetTextEncodingFromInternetName", tec_TECGetTextEncodingFromInternetName },
    { "TECGetTextEncodingInternetName", tec_TECGetTextEncodingInternetName },
    { "TECGetWebTextEncodings", tec_TECGetWebTextEncodings },
    { "TECSetBasicOptions", tec_TECSetBasicOptions },
    { "TECSniffTextEncoding", tec_TECSniffTextEncoding },
};
const unsigned tec_export_count = sizeof tec_exports / sizeof tec_exports[0];
