/*
 * The Apple event helpers (AEHelpers.h): AEBuildDesc, AEBuildParameters and
 * AEBuildAppleEvent build descriptors from a format string, AEPrintDescToHandle
 * writes one back as text, and AEStream builds one by writing to a stream.
 *
 * The format language (the header leaves it "to be described"):
 *     desc     = number | string | 'type' | type(data) | type{...} | [...] | {...}
 *              | true | false | @@
 *     number   = 42, -7 (a 'long'), 1.5 or 2e3 (a 'doub')
 *     string   = "text" (TEXT)
 *     'type'   = four characters in quotes (a descriptor of type 'type')
 *     type(..) = data coerced to the type: a number, a string, a 'type',
 *                «hex», nothing, or a substitution
 *     list     = [desc, desc ...]
 *     record   = {key: desc, key: desc ...}; with a type before it, a record of that type
 * A type or keyword is four characters, quoted, or bare when made of
 * letters and digits (a bare three-character one is padded with a space,
 * so obj{...} is 'obj '{...}).  A substitution takes the next argument:
 * type(@) a long, shor, bool, type/enum/keyw/sign (an OSType), doub or
 * sing (a double), TEXT (a C string) or, for any other type, a pointer and
 * a length; @@ an AEDesc pointer, copied (type(@@) coerced).  The
 * parameters of an event are key: desc pairs, with &key: for an attribute.
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "carbonrest.h"

int aem_desc_shape(gaddr desc);
uint32_t aem_entry_count(gaddr desc, bool attr);
int16_t aem_entry_desc(gaddr desc, bool attr, uint32_t i, uint32_t *key, gaddr out);

#define T(s) fourcc(s)
enum {
    errAEStreamBadNesting = -1737,
    errAEBuildSyntaxError = -1740,
    errAENotAEDesc = -1704,
    errAEWrongDataType = -1703,
    errAEDescIsNull = -1739,
};
enum {
    sx_ok, sx_bad_token, sx_bad_eof, sx_no_eof, sx_bad_negative, sx_missing_quote, sx_bad_hex, sx_odd_hex,
    sx_no_close_hex, sx_uncoerced_hex, sx_no_close_string, sx_bad_desc, sx_bad_data, sx_no_close_paren,
    sx_no_close_bracket, sx_no_close_brace, sx_no_key, sx_no_colon, sx_coerced_list, sx_uncoerced_double_at
};

static bool desc_type_is(gaddr d, uint32_t type) { return rd32(d) == type; }

static void null_desc(gaddr d)
{
    wr32(d, T("null"));
    wr32(d + 4, 0);
}

/* A descriptor from host bytes. */
static int16_t make_desc(uint32_t type, const void *data, uint32_t n, gaddr out)
{
    gaddr tmp = n ? tb_NewPtr((int32_t)n, false, true) : 0;
    if (n && !tmp)
        return memFullErr;
    if (n)
        gput(tmp, data, n);
    int16_t err = tb_AECreateDesc((int32_t)type, tmp, (int32_t)n, out);
    if (tmp)
        tb_DisposePtr(tmp);
    return err;
}

/* ---------------------------------------------------------------------- */
/* The builder */

typedef struct {
    const uint8_t *s;
    size_t n, p;
    int err;       /* a syntax error code, or 0 */
    size_t errpos;
    int16_t oserr; /* a failure that is not syntax */
    ppc_cpu *c;
    unsigned arg;  /* the next argument word (register convention) */
    gaddr va;      /* or the va_list: the next argument word's address */
} bld;

static uint32_t next_word(bld *b)
{
    if (b->va) {
        uint32_t w = rd32(b->va);
        b->va += 4;
        return w;
    }
    return svc_arg(b->c, b->arg++);
}

static double next_double(bld *b)
{
    uint64_t hi = next_word(b), lo = next_word(b), v = hi << 32 | lo;
    double d;
    memcpy(&d, &v, 8);
    return d;
}

static int fail(bld *b, int code)
{
    if (!b->err) {
        b->err = code;
        b->errpos = b->p;
    }
    return b->err;
}

static void ws(bld *b)
{
    while (b->p < b->n && (b->s[b->p] == ' ' || b->s[b->p] == '\t' || b->s[b->p] == '\r' || b->s[b->p] == '\n'))
        b->p++;
}

static int peek(bld *b) { return b->p < b->n ? b->s[b->p] : -1; }

static bool ident_char(int c) { return c >= 0 && (c == '_' || (c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'z')); }

enum { MAC_LDQUO = 0xD2, MAC_RDQUO = 0xD3, MAC_LGUIL = 0xC7, MAC_RGUIL = 0xC8 };

/* A type or keyword: quoted, or bare letters and digits (three padded). */
static bool read_code(bld *b, uint32_t *out)
{
    ws(b);
    int c = peek(b);
    uint8_t code[4] = { ' ', ' ', ' ', ' ' };
    if (c == '\'') {
        size_t start = b->p + 1, e = start;
        while (e < b->n && b->s[e] != '\'')
            e++;
        if (e >= b->n) {
            b->p = e;
            fail(b, sx_missing_quote);
            return false;
        }
        if (e - start != 4) {
            fail(b, sx_bad_desc);
            return false;
        }
        memcpy(code, b->s + start, 4);
        b->p = e + 1;
    } else if (ident_char(c)) {
        size_t e = b->p;
        while (e < b->n && ident_char(b->s[e]))
            e++;
        size_t len = e - b->p;
        if (len != 4 && len != 3) {
            fail(b, sx_bad_desc);
            return false;
        }
        memcpy(code, b->s + b->p, len);
        b->p = e;
    } else {
        fail(b, sx_bad_token);
        return false;
    }
    *out = (uint32_t)code[0] << 24 | (uint32_t)code[1] << 16 | (uint32_t)code[2] << 8 | code[3];
    return true;
}

static void put_be(uint8_t *p, uint64_t v, int n)
{
    for (int i = 0; i < n; i++)
        p[i] = (uint8_t)(v >> (8 * (n - 1 - i)));
}

typedef struct {
    uint8_t *v;
    uint32_t n, cap;
} bytes;

static void bytes_add(bytes *b, const void *d, uint32_t n)
{
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 16;
        b->v = realloc(b->v, b->cap);
    }
    if (n)
        memcpy(b->v + b->n, d, n);
    b->n += n;
}

static int parse_desc(bld *b, gaddr out);

/* A string literal: its bytes. */
static bool read_string(bld *b, bytes *out)
{
    int open = peek(b);
    int close = open == '"' ? '"' : MAC_RDQUO;
    b->p++;
    while (b->p < b->n && b->s[b->p] != close) {
        if (b->s[b->p] == '\\' && b->p + 1 < b->n && (b->s[b->p + 1] == close || b->s[b->p + 1] == '\\'))
            b->p++;
        uint8_t ch = b->s[b->p++];
        bytes_add(out, &ch, 1);
    }
    if (b->p >= b->n) {
        fail(b, sx_no_close_string);
        return false;
    }
    b->p++;
    return true;
}

/* «hex digits» */
static bool read_hex(bld *b, bytes *out)
{
    b->p++;
    int hi = -1;
    while (b->p < b->n && b->s[b->p] != MAC_RGUIL) {
        int ch = b->s[b->p], v;
        if (ch == ' ') {
            b->p++;
            continue;
        }
        if (ch >= '0' && ch <= '9')
            v = ch - '0';
        else if ((ch | 32) >= 'a' && (ch | 32) <= 'f')
            v = (ch | 32) - 'a' + 10;
        else
            return fail(b, sx_bad_hex), false;
        if (hi < 0)
            hi = v;
        else {
            uint8_t byte = (uint8_t)(hi << 4 | v);
            bytes_add(out, &byte, 1);
            hi = -1;
        }
        b->p++;
    }
    if (b->p >= b->n)
        return fail(b, sx_no_close_hex), false;
    if (hi >= 0)
        return fail(b, sx_odd_hex), false;
    b->p++;
    return true;
}

/* A number literal: an integer or a real. */
static bool read_number(bld *b, bool *is_real, double *real, int64_t *integer)
{
    size_t start = b->p;
    bool neg = false;
    if (peek(b) == '-') {
        neg = true;
        b->p++;
        if (peek(b) < '0' || peek(b) > '9')
            return fail(b, sx_bad_negative), false;
    }
    size_t digits = b->p;
    while (peek(b) >= '0' && peek(b) <= '9')
        b->p++;
    *is_real = false;
    if (peek(b) == '.') {
        *is_real = true;
        b->p++;
        while (peek(b) >= '0' && peek(b) <= '9')
            b->p++;
    }
    if ((peek(b) | 32) == 'e' && b->p > digits) {
        size_t save = b->p;
        b->p++;
        if (peek(b) == '+' || peek(b) == '-')
            b->p++;
        if (peek(b) >= '0' && peek(b) <= '9') {
            *is_real = true;
            while (peek(b) >= '0' && peek(b) <= '9')
                b->p++;
        } else
            b->p = save;
    }
    char text[64];
    size_t len = b->p - start;
    if (len >= sizeof text)
        len = sizeof text - 1;
    memcpy(text, b->s + start, len);
    text[len] = 0;
    *real = strtod(text, NULL);
    *integer = (int64_t)strtoll(text, NULL, 10);
    if (!*is_real && (*real > 2147483647.0 || *real < -2147483648.0))
        *is_real = true;
    (void)neg;
    return true;
}

/* A substitution's value: how many bytes of a type's data it is. */
static void subst_data(bld *b, uint32_t type, bytes *out)
{
    uint8_t buf[8];
    if (type == T("long") || type == T("type") || type == T("enum") || type == T("keyw") || type == T("sign") ||
        type == T("magn") || type == T("ucom") || type == T("fixd") || type == T("fract")) {
        put_be(buf, next_word(b), 4);
        bytes_add(out, buf, 4);
    } else if (type == T("shor")) {
        put_be(buf, next_word(b) & 0xFFFF, 2);
        bytes_add(out, buf, 2);
    } else if (type == T("bool")) {
        buf[0] = (next_word(b) & 0xFF) != 0;
        bytes_add(out, buf, 1);
    } else if (type == T("doub")) {
        double d = next_double(b);
        uint64_t v;
        memcpy(&v, &d, 8);
        put_be(buf, v, 8);
        bytes_add(out, buf, 8);
    } else if (type == T("sing")) {
        float f = (float)next_double(b);
        uint32_t v;
        memcpy(&v, &f, 4);
        put_be(buf, v, 4);
        bytes_add(out, buf, 4);
    } else if (type == T("TEXT")) {
        gaddr s = next_word(b);
        for (uint32_t i = 0; s && rd8(s + i); i++) {
            uint8_t ch = rd8(s + i);
            bytes_add(out, &ch, 1);
        }
    } else {
        gaddr p = next_word(b);
        uint32_t n = next_word(b);
        if (p && n && n < (16u << 20)) {
            uint8_t *tmp = malloc(n);
            gget(tmp, p, n);
            bytes_add(out, tmp, n);
            free(tmp);
        }
    }
}

/* A number as data of a type. */
static void number_data(uint32_t type, bool is_real, double real, int64_t integer, bytes *out)
{
    uint8_t buf[8];
    if (type == T("doub") || (is_real && type != T("sing"))) {
        uint64_t v;
        memcpy(&v, &real, 8);
        put_be(buf, v, 8);
        bytes_add(out, buf, 8);
    } else if (type == T("sing")) {
        float f = (float)real;
        uint32_t v;
        memcpy(&v, &f, 4);
        put_be(buf, v, 4);
        bytes_add(out, buf, 4);
    } else if (type == T("shor")) {
        put_be(buf, (uint64_t)integer, 2);
        bytes_add(out, buf, 2);
    } else if (type == T("bool")) {
        buf[0] = integer != 0;
        bytes_add(out, buf, 1);
    } else {
        put_be(buf, (uint64_t)integer, 4);
        bytes_add(out, buf, 4);
    }
}

/* type(data): the data between the parentheses, as a descriptor. */
static int parse_coerced(bld *b, uint32_t type, gaddr out)
{
    b->p++; /* ( */
    ws(b);
    bytes data = { 0 };
    int c = peek(b);
    int rc = 0;
    gaddr whole = 0;
    if (c == ')') {
        /* nothing */
    } else if (c == '@' && b->p + 1 < b->n && b->s[b->p + 1] == '@') {
        b->p += 2;
        gaddr src = next_word(b);
        whole = tb_NewPtr(8, false, true);
        if (!whole || !src || tb_AEDuplicateDesc(src, whole)) {
            b->oserr = whole ? errAENotAEDesc : memFullErr;
            rc = b->oserr;
        }
    } else if (c == '@') {
        b->p++;
        subst_data(b, type, &data);
    } else if (c == MAC_LGUIL) {
        if (!read_hex(b, &data))
            rc = b->err;
    } else if (c == '"' || c == MAC_LDQUO) {
        if (!read_string(b, &data))
            rc = b->err;
    } else if (c == '\'') {
        uint32_t code;
        if (!read_code(b, &code))
            rc = b->err;
        else {
            uint8_t buf[4];
            put_be(buf, code, 4);
            bytes_add(&data, buf, 4);
        }
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        bool is_real;
        double real;
        int64_t integer;
        if (!read_number(b, &is_real, &real, &integer))
            rc = b->err;
        else
            number_data(type, is_real, real, integer, &data);
    } else {
        rc = fail(b, sx_bad_data);
    }
    if (!rc) {
        ws(b);
        if (peek(b) != ')')
            rc = fail(b, sx_no_close_paren);
        else
            b->p++;
    }
    if (!rc) {
        if (whole) { /* coerced from the AEDesc given */
            if (desc_type_is(whole, type) || type == T("****")) {
                wr32(out, rd32(whole));
                wr32(out + 4, rd32(whole + 4));
                tb_DisposePtr(whole);
                whole = 0;
            } else {
                int16_t e = tb_AECoerceDesc(whole, (int32_t)type, out);
                tb_AEDisposeDesc(whole);
                tb_DisposePtr(whole);
                whole = 0;
                if (e) {
                    b->oserr = e;
                    rc = e;
                }
            }
        } else {
            int16_t e = make_desc(type, data.v, data.n, out);
            if (e) {
                b->oserr = e;
                rc = e;
            }
        }
    }
    if (whole) {
        tb_AEDisposeDesc(whole);
        tb_DisposePtr(whole);
    }
    free(data.v);
    return rc;
}

static int parse_list(bld *b, gaddr out)
{
    b->p++; /* [ */
    int16_t e = tb_AECreateList(0, 0, 0, out);
    if (e)
        return b->oserr = e;
    gaddr item = tb_NewPtr(8, false, true);
    if (!item) {
        tb_AEDisposeDesc(out);
        return b->oserr = memFullErr;
    }
    int rc = 0;
    ws(b);
    if (peek(b) == ']') {
        b->p++;
    } else {
        for (;;) {
            null_desc(item);
            if ((rc = parse_desc(b, item)))
                break;
            e = tb_AEPutDesc(out, 0, item);
            tb_AEDisposeDesc(item);
            if (e) {
                rc = b->oserr = e;
                break;
            }
            ws(b);
            if (peek(b) == ',') {
                b->p++;
                continue;
            }
            if (peek(b) == ']') {
                b->p++;
                break;
            }
            rc = fail(b, b->p >= b->n ? sx_no_close_bracket : sx_no_close_bracket);
            break;
        }
    }
    tb_DisposePtr(item);
    if (rc)
        tb_AEDisposeDesc(out);
    return rc;
}

/* The items of a record up to its closing brace; the open brace read. */
static int parse_record(bld *b, uint32_t type, gaddr out)
{
    b->p++; /* { */
    int16_t e = tb_AECreateList(0, 0, 1, out);
    if (e)
        return b->oserr = e;
    gaddr item = tb_NewPtr(8, false, true);
    if (!item) {
        tb_AEDisposeDesc(out);
        return b->oserr = memFullErr;
    }
    int rc = 0;
    ws(b);
    if (peek(b) == '}') {
        b->p++;
    } else {
        for (;;) {
            uint32_t key;
            ws(b);
            if (peek(b) == -1 || peek(b) == '}') {
                rc = fail(b, sx_no_key);
                break;
            }
            if (!read_code(b, &key)) {
                rc = b->err == sx_bad_token || b->err == sx_bad_desc ? (b->err = sx_no_key) : b->err;
                break;
            }
            ws(b);
            if (peek(b) != ':') {
                rc = fail(b, sx_no_colon);
                break;
            }
            b->p++;
            null_desc(item);
            if ((rc = parse_desc(b, item)))
                break;
            e = tb_AEPutParamDesc(out, (int32_t)key, item);
            tb_AEDisposeDesc(item);
            if (e) {
                rc = b->oserr = e;
                break;
            }
            ws(b);
            if (peek(b) == ',') {
                b->p++;
                continue;
            }
            if (peek(b) == '}') {
                b->p++;
                break;
            }
            rc = fail(b, sx_no_close_brace);
            break;
        }
    }
    tb_DisposePtr(item);
    if (rc)
        tb_AEDisposeDesc(out);
    else if (type != T("reco"))
        wr32(out, type);
    return rc;
}

static int parse_desc(bld *b, gaddr out)
{
    ws(b);
    int c = peek(b);
    if (c < 0)
        return fail(b, sx_bad_eof);
    if (c == '[')
        return parse_list(b, out);
    if (c == '{')
        return parse_record(b, T("reco"), out);
    if (c == '"' || c == MAC_LDQUO) {
        bytes s = { 0 };
        int rc = 0;
        if (!read_string(b, &s))
            rc = b->err;
        else if ((b->oserr = make_desc(T("TEXT"), s.v, s.n, out)))
            rc = b->oserr;
        free(s.v);
        return rc;
    }
    if (c == MAC_LGUIL)
        return fail(b, sx_uncoerced_hex);
    if (c == '-' || (c >= '0' && c <= '9')) {
        bool is_real;
        double real;
        int64_t integer;
        if (!read_number(b, &is_real, &real, &integer))
            return b->err;
        bytes d = { 0 };
        number_data(is_real ? T("doub") : T("long"), is_real, real, integer, &d);
        int rc = (b->oserr = make_desc(is_real ? T("doub") : T("long"), d.v, d.n, out));
        free(d.v);
        return rc;
    }
    if (c == '@') {
        if (b->p + 1 < b->n && b->s[b->p + 1] == '@') {
            b->p += 2;
            gaddr src = next_word(b);
            if (!src)
                return b->oserr = errAENotAEDesc;
            return b->oserr = tb_AEDuplicateDesc(src, out);
        }
        return fail(b, sx_bad_desc);
    }
    if (c == '\'' || ident_char(c)) {
        bool quoted = c == '\'';
        size_t at = b->p;
        uint32_t code;
        if (!read_code(b, &code))
            return b->err;
        size_t after = b->p;
        ws(b);
        int next = peek(b);
        if (next != '(' && next != '{' && next != '[')
            b->p = after;
        if (next == '(')
            return parse_coerced(b, code, out);
        if (next == '{')
            return parse_record(b, code, out);
        if (next == '[')
            return fail(b, sx_coerced_list);
        if (quoted) { /* a type value */
            uint8_t buf[4];
            put_be(buf, code, 4);
            return b->oserr = make_desc(T("type"), buf, 4, out);
        }
        /* true and false */
        if (code == T("true"))
            return b->oserr = make_desc(T("true"), NULL, 0, out);
        if (code == T("fals") || (b->p - at == 5 && !memcmp(b->s + at, "false", 5)))
            return b->oserr = make_desc(T("fals"), NULL, 0, out);
        b->p = at;
        return fail(b, sx_bad_desc);
    }
    return fail(b, sx_bad_token);
}

static void set_error(gaddr error, int code, size_t pos)
{
    if (error) {
        wr32(error, (uint32_t)code);
        wr32(error + 4, (uint32_t)pos);
    }
}

static int16_t finish(bld *b, int rc, gaddr error)
{
    if (!rc) {
        set_error(error, 0, 0);
        return noErr;
    }
    if (b->err) {
        set_error(error, b->err, b->errpos);
        return errAEBuildSyntaxError;
    }
    set_error(error, 0, 0);
    return b->oserr ? b->oserr : errAEBuildSyntaxError;
}

static bool load_format(bld *b, gaddr fmt, uint8_t **buf)
{
    size_t n = 0;
    while (fmt && rd8(fmt + (uint32_t)n) && n < (1u << 20))
        n++;
    *buf = malloc(n + 1);
    if (!*buf)
        return false;
    gget(*buf, fmt, (uint32_t)n);
    (*buf)[n] = 0;
    b->s = *buf;
    b->n = n;
    b->p = 0;
    return true;
}

/* The whole format as one descriptor. */
static int16_t build_desc(ppc_cpu *c, gaddr dst, gaddr error, gaddr fmt, unsigned firstarg, gaddr va)
{
    bld b = { .c = c, .arg = firstarg, .va = va };
    uint8_t *buf;
    if (!dst || !fmt)
        return paramErr;
    null_desc(dst);
    if (!load_format(&b, fmt, &buf))
        return memFullErr;
    gaddr tmp = tb_NewPtr(8, false, true);
    int rc = 0;
    if (!tmp) {
        free(buf);
        return memFullErr;
    }
    null_desc(tmp);
    rc = parse_desc(&b, tmp);
    if (!rc) {
        ws(&b);
        if (b.p < b.n)
            rc = fail(&b, sx_no_eof);
    }
    if (rc)
        tb_AEDisposeDesc(tmp);
    else {
        wr32(dst, rd32(tmp));
        wr32(dst + 4, rd32(tmp + 4));
    }
    tb_DisposePtr(tmp);
    free(buf);
    return finish(&b, rc, error);
}

/* key: desc pairs put in an event or record (&key: attributes of an event). */
static int params_into(bld *b, gaddr event)
{
    gaddr item = tb_NewPtr(8, false, true);
    if (!item)
        return b->oserr = memFullErr;
    int rc = 0;
    for (;;) {
        ws(b);
        if (peek(b) < 0)
            break;
        bool attr = false;
        if (peek(b) == '&') {
            attr = true;
            b->p++;
        }
        uint32_t key;
        if (!read_code(b, &key)) {
            rc = b->err == sx_bad_token ? (b->err = sx_no_key) : b->err;
            break;
        }
        ws(b);
        if (peek(b) != ':') {
            rc = fail(b, sx_no_colon);
            break;
        }
        b->p++;
        null_desc(item);
        if ((rc = parse_desc(b, item)))
            break;
        int16_t e = attr ? tb_AEPutAttributeDesc(event, (int32_t)key, item) : tb_AEPutParamDesc(event, (int32_t)key, item);
        tb_AEDisposeDesc(item);
        if (e) {
            rc = b->oserr = e;
            break;
        }
        ws(b);
        if (peek(b) == ',') {
            b->p++;
            continue;
        }
        if (peek(b) >= 0) {
            rc = fail(b, sx_bad_token);
            break;
        }
    }
    tb_DisposePtr(item);
    return rc;
}

static int16_t build_parameters(ppc_cpu *c, gaddr event, gaddr error, gaddr fmt, unsigned firstarg, gaddr va)
{
    bld b = { .c = c, .arg = firstarg, .va = va };
    uint8_t *buf;
    if (!event || !fmt)
        return paramErr;
    if (!load_format(&b, fmt, &buf))
        return memFullErr;
    int rc = params_into(&b, event);
    free(buf);
    return finish(&b, rc, error);
}

static int16_t build_event(ppc_cpu *c, unsigned firstarg, gaddr va)
{
    /* class, id, addressType, addressData, addressLength, returnID, transactionID, result, error, format */
    uint32_t cls = A(0), id = A(1), atype = A(2), adata = A(3), alen = A(4);
    int16_t rid = S16(5);
    uint32_t tid = A(6);
    gaddr result = A(7), error = A(8), fmt = A(9);
    if (!result || !fmt)
        return paramErr;
    gaddr addr = tb_NewPtr(8, false, true);
    if (!addr)
        return memFullErr;
    int16_t e = tb_AECreateDesc((int32_t)atype, adata, (int32_t)alen, addr);
    if (!e)
        e = tb_AECreateAppleEvent((int32_t)cls, (int32_t)id, addr, rid, (int32_t)tid, result);
    tb_AEDisposeDesc(addr);
    tb_DisposePtr(addr);
    if (e) {
        null_desc(result);
        return e;
    }
    int16_t r = build_parameters(c, result, error, fmt, firstarg, va);
    if (r)
        tb_AEDisposeDesc(result);
    return r;
}

/* AEBuildDesc(dst, error, src, ...) */
static void ae_AEBuildDesc(ppc_cpu *c) { RET((int32_t)build_desc(c, A(0), A(1), A(2), 3, 0)); }
static void ae_vAEBuildDesc(ppc_cpu *c) { RET((int32_t)build_desc(c, A(0), A(1), A(2), 0, A(3))); }
static void ae_AEBuildParameters(ppc_cpu *c) { RET((int32_t)build_parameters(c, A(0), A(1), A(2), 3, 0)); }
static void ae_vAEBuildParameters(ppc_cpu *c) { RET((int32_t)build_parameters(c, A(0), A(1), A(2), 0, A(3))); }
static void ae_AEBuildAppleEvent(ppc_cpu *c) { RET((int32_t)build_event(c, 10, 0)); }
static void ae_vAEBuildAppleEvent(ppc_cpu *c) { RET((int32_t)build_event(c, 0, A(10))); }

/* ---------------------------------------------------------------------- */
/* AEPrintDescToHandle */

typedef struct {
    char *v;
    size_t n, cap;
} text;

static void tprintf(text *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void tprintf(text *t, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof buf)
        n = sizeof buf - 1;
    if (t->n + (size_t)n + 1 > t->cap) {
        t->cap = (t->n + (size_t)n) * 2 + 64;
        t->v = realloc(t->v, t->cap);
    }
    memcpy(t->v + t->n, buf, (size_t)n);
    t->n += (size_t)n;
}

static void print_code(text *t, uint32_t code)
{
    char c[5] = { (char)(code >> 24), (char)(code >> 16), (char)(code >> 8), (char)code, 0 };
    bool bare = true;
    for (int i = 0; i < 4; i++)
        if (!ident_char((uint8_t)c[i]))
            bare = false;
    if (bare)
        tprintf(t, "%s", c);
    else {
        bool printable = true;
        for (int i = 0; i < 4; i++)
            if ((uint8_t)c[i] < 32 || (uint8_t)c[i] == 0x7F || c[i] == '\'')
                printable = false;
        if (printable)
            tprintf(t, "'%s'", c);
        else
            tprintf(t, "'\\x%08X'", code);
    }
}

static void print_desc(text *t, gaddr desc, int depth);

static void print_entries(text *t, gaddr desc, int depth)
{
    gaddr item = tb_NewPtr(8, false, true);
    if (!item)
        return;
    uint32_t na = aem_entry_count(desc, true), n = aem_entry_count(desc, false);
    bool first = true;
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; i < (pass ? n : na); i++) {
            uint32_t key = 0;
            if (aem_entry_desc(desc, pass == 0, i, &key, item))
                continue;
            tprintf(t, "%s", first ? "" : ", ");
            first = false;
            if (pass == 0)
                tprintf(t, "&");
            print_code(t, key);
            tprintf(t, ":");
            print_desc(t, item, depth + 1);
            tb_AEDisposeDesc(item);
        }
    tb_DisposePtr(item);
}

static void print_desc(text *t, gaddr desc, int depth)
{
    uint32_t type = rd32(desc);
    int shape = depth < 32 ? aem_desc_shape(desc) : 0;
    if (shape == 1) {
        gaddr item = tb_NewPtr(8, false, true);
        tprintf(t, "[");
        uint32_t n = aem_entry_count(desc, false);
        for (uint32_t i = 0; item && i < n; i++) {
            if (aem_entry_desc(desc, false, i, NULL, item))
                continue;
            tprintf(t, "%s", i ? ", " : "");
            print_desc(t, item, depth + 1);
            tb_AEDisposeDesc(item);
        }
        tprintf(t, "]");
        if (item)
            tb_DisposePtr(item);
        return;
    }
    if (shape == 2) {
        if (type != T("reco"))
            print_code(t, type);
        tprintf(t, "{");
        print_entries(t, desc, depth);
        tprintf(t, "}");
        return;
    }
    gaddr h = rd32(desc + 4);
    uint32_t n = h ? (uint32_t)tb_GetHandleSize(h) : 0;
    uint8_t *d = malloc(n + 1);
    if (n)
        gget(d, rd32(h), n);
    print_code(t, type);
    tprintf(t, "(");
    if (type == T("long") && n == 4)
        tprintf(t, "%d", (int32_t)(d[0] << 24 | d[1] << 16 | d[2] << 8 | d[3]));
    else if (type == T("shor") && n == 2)
        tprintf(t, "%d", (int16_t)(d[0] << 8 | d[1]));
    else if (type == T("bool") && n == 1)
        tprintf(t, "%s", d[0] ? "true" : "false");
    else if (type == T("doub") && n == 8) {
        uint64_t v = 0;
        for (int i = 0; i < 8; i++)
            v = v << 8 | d[i];
        double x;
        memcpy(&x, &v, 8);
        tprintf(t, "%.17g", x);
    } else if (type == T("sing") && n == 4) {
        uint32_t v = (uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3];
        float x;
        memcpy(&x, &v, 4);
        tprintf(t, "%.9g", (double)x);
    } else if ((type == T("type") || type == T("enum") || type == T("keyw") || type == T("sign")) && n == 4) {
        tprintf(t, "'%c%c%c%c'", d[0], d[1], d[2], d[3]);
    } else if (type == T("TEXT") || type == T("ustr")) {
        bool plain = true;
        for (uint32_t i = 0; i < n; i++)
            if (d[i] < 32 || d[i] == 0x7F || d[i] == '"' || d[i] == '\\')
                plain = false;
        if (plain && type == T("TEXT")) {
            tprintf(t, "\"%.*s\"", (int)n, (const char *)d);
        } else {
            tprintf(t, "%c", MAC_LGUIL);
            for (uint32_t i = 0; i < n; i++)
                tprintf(t, "%02X", d[i]);
            tprintf(t, "%c", MAC_RGUIL);
        }
    } else if (n) {
        tprintf(t, "%c", MAC_LGUIL);
        for (uint32_t i = 0; i < n; i++)
            tprintf(t, "%02X", d[i]);
        tprintf(t, "%c", MAC_RGUIL);
    }
    tprintf(t, ")");
    free(d);
}

static void ae_AEPrintDescToHandle(ppc_cpu *c)
{
    gaddr desc = A(0), result = A(1);
    if (!desc || !result)
        RET(paramErr);
    if (rd32(desc) == T("null") && !rd32(desc + 4))
        ; /* a null descriptor prints as such */
    text t = { 0 };
    print_desc(&t, desc, 0);
    gaddr h = tb_NewHandle((int32_t)t.n, false, true);
    if (h && t.n)
        gput(rd32(h), t.v, (uint32_t)t.n);
    free(t.v);
    wr32(result, h);
    RET(h ? noErr : memFullErr);
}

/* ---------------------------------------------------------------------- */
/* AEStream: a stack of what is open */

enum { F_TOP, F_LIST, F_RECORD, F_DESC, F_EVENT };
enum { MAX_FRAMES = 32, STREAM_TAG = 0x41455300 };
typedef struct {
    int kind;
    gaddr desc;     /* the container's AEDesc (8 bytes) */
    uint32_t type;  /* an open desc's type, or a record's */
    bytes data;     /* an open desc's data so far */
    bool have_key;  /* a key is waiting for its value */
    uint32_t key;
} frame;
typedef struct {
    bool used;
    frame f[MAX_FRAMES];
    int depth;      /* frames in use; f[0] is the top */
    bool have_result;
    gaddr result;   /* the single value written at the top, or the event */
} stream;
enum { MAX_STREAMS = 16 };
static stream streams[MAX_STREAMS];

void rest_ae_reset(void)
{
    for (int i = 0; i < MAX_STREAMS; i++) {
        for (int k = 0; k < streams[i].depth; k++)
            free(streams[i].f[k].data.v);
        memset(&streams[i], 0, sizeof streams[i]);
    }
}

static stream *stream_of(gaddr ref)
{
    uint32_t i = ref - STREAM_TAG;
    return (ref & 0xFFFFFF00u) == STREAM_TAG && i < MAX_STREAMS && streams[i].used ? &streams[i] : NULL;
}

static stream *stream_new(gaddr *ref)
{
    for (int i = 0; i < MAX_STREAMS; i++)
        if (!streams[i].used) {
            memset(&streams[i], 0, sizeof streams[i]);
            streams[i].used = true;
            streams[i].result = tb_NewPtr(8, false, true);
            if (!streams[i].result) {
                streams[i].used = false;
                return NULL;
            }
            null_desc(streams[i].result);
            streams[i].f[0].kind = F_TOP;
            streams[i].depth = 1;
            *ref = STREAM_TAG + (uint32_t)i;
            return &streams[i];
        }
    return NULL;
}

static void stream_free(stream *s)
{
    for (int k = 0; k < s->depth; k++) {
        free(s->f[k].data.v);
        if (k && s->f[k].desc) {
            tb_AEDisposeDesc(s->f[k].desc);
            tb_DisposePtr(s->f[k].desc);
        }
    }
    if (s->result) {
        tb_AEDisposeDesc(s->result);
        tb_DisposePtr(s->result);
    }
    memset(s, 0, sizeof *s);
}

static frame *top(stream *s) { return &s->f[s->depth - 1]; }

/* A finished value (a descriptor, consumed) into the open container. */
static int16_t add_value(stream *s, gaddr value)
{
    frame *f = top(s);
    int16_t e;
    switch (f->kind) {
    case F_LIST:
        e = tb_AEPutDesc(f->desc, 0, value);
        break;
    case F_RECORD:
        if (!f->have_key)
            return errAEStreamBadNesting;
        e = tb_AEPutParamDesc(f->desc, (int32_t)f->key, value);
        f->have_key = false;
        break;
    case F_EVENT:
        if (!f->have_key)
            return errAEStreamBadNesting;
        e = tb_AEPutParamDesc(f->desc, (int32_t)f->key, value);
        f->have_key = false;
        break;
    case F_TOP:
        if (s->have_result)
            return errAEStreamBadNesting;
        tb_AEDisposeDesc(s->result);
        wr32(s->result, rd32(value));
        wr32(s->result + 4, rd32(value + 4));
        wr32(value + 4, 0);
        s->have_result = true;
        return noErr;
    default:
        return errAEStreamBadNesting;
    }
    tb_AEDisposeDesc(value);
    return e;
}

static int16_t push_container(stream *s, int kind, uint32_t type)
{
    frame *cur = top(s);
    if (cur->kind == F_DESC || (cur->kind == F_TOP && s->have_result) || s->depth >= MAX_FRAMES)
        return errAEStreamBadNesting;
    if ((cur->kind == F_RECORD || cur->kind == F_EVENT) && !cur->have_key)
        return errAEStreamBadNesting;
    gaddr d = tb_NewPtr(8, false, true);
    if (!d)
        return memFullErr;
    int16_t e = tb_AECreateList(0, 0, kind == F_RECORD, d);
    if (e) {
        tb_DisposePtr(d);
        return e;
    }
    frame *f = &s->f[s->depth++];
    memset(f, 0, sizeof *f);
    f->kind = kind;
    f->desc = d;
    f->type = type;
    if (kind == F_RECORD && type != T("reco"))
        wr32(d, type);
    return noErr;
}

static int16_t pop_container(stream *s, int kind)
{
    frame *f = top(s);
    if (f->kind != kind || s->depth < 2)
        return errAEStreamBadNesting;
    gaddr d = f->desc;
    s->depth--;
    int16_t e = add_value(s, d);
    if (rd32(d + 4) == 0 && e == noErr && top(s)->kind != F_TOP) {
        /* consumed */
    }
    tb_DisposePtr(d);
    return e;
}

static int16_t write_desc(stream *s, uint32_t type, gaddr data, uint32_t len)
{
    if (top(s)->kind == F_DESC)
        return errAEStreamBadNesting;
    frame *cur = top(s);
    if ((cur->kind == F_RECORD || cur->kind == F_EVENT) && !cur->have_key)
        return errAEStreamBadNesting;
    gaddr d = tb_NewPtr(8, false, true);
    if (!d)
        return memFullErr;
    int16_t e = tb_AECreateDesc((int32_t)type, data, (int32_t)len, d);
    if (!e)
        e = add_value(s, d);
    else
        tb_AEDisposeDesc(d);
    tb_DisposePtr(d);
    return e;
}

static void st_AEStreamOpen(ppc_cpu *c)
{
    gaddr ref = 0;
    RET(stream_new(&ref) ? ref : 0);
}

static void st_AEStreamClose(ppc_cpu *c)
{
    stream *s = stream_of(A(0));
    if (!s)
        RET(paramErr);
    int16_t e = noErr;
    gaddr out = A(1);
    if (s->depth == 2 && s->f[1].kind == F_EVENT) {
        /* The event, as it stands. */
        if (out) {
            wr32(out, rd32(s->f[1].desc));
            wr32(out + 4, rd32(s->f[1].desc + 4));
            wr32(s->f[1].desc + 4, 0);
        }
    } else if (s->depth != 1) {
        e = errAEStreamBadNesting;
    } else if (out) {
        if (s->have_result) {
            wr32(out, rd32(s->result));
            wr32(out + 4, rd32(s->result + 4));
            wr32(s->result + 4, 0);
            wr32(s->result, T("null"));
        } else {
            null_desc(out);
        }
    }
    stream_free(s);
    RET((int32_t)e);
}

static void st_AEStreamOpenDesc(ppc_cpu *c)
{
    stream *s = stream_of(A(0));
    if (!s)
        RET(paramErr);
    frame *cur = top(s);
    if (cur->kind == F_DESC || (cur->kind == F_TOP && s->have_result) ||
        ((cur->kind == F_RECORD || cur->kind == F_EVENT) && !cur->have_key) || s->depth >= MAX_FRAMES)
        RET(errAEStreamBadNesting);
    frame *f = &s->f[s->depth++];
    memset(f, 0, sizeof *f);
    f->kind = F_DESC;
    f->type = A(1);
    RET(noErr);
}

static void st_AEStreamWriteData(ppc_cpu *c)
{
    stream *s = stream_of(A(0));
    if (!s)
        RET(paramErr);
    frame *f = top(s);
    if (f->kind != F_DESC)
        RET(errAEStreamBadNesting);
    int32_t n = (int32_t)A(2);
    if (n < 0 || (n && !A(1)))
        RET(paramErr);
    uint8_t *tmp = malloc((size_t)n + 1);
    gget(tmp, A(1), (uint32_t)n);
    bytes_add(&f->data, tmp, (uint32_t)n);
    free(tmp);
    RET(noErr);
}

static void st_AEStreamCloseDesc(ppc_cpu *c)
{
    stream *s = stream_of(A(0));
    if (!s)
        RET(paramErr);
    frame *f = top(s);
    if (f->kind != F_DESC)
        RET((int32_t)errAEStreamBadNesting);
    uint32_t type = f->type;
    gaddr data = 0;
    uint32_t n = f->data.n;
    if (n) {
        data = tb_NewPtr((int32_t)n, false, true);
        if (!data)
            RET(memFullErr);
        gput(data, f->data.v, n);
    }
    free(f->data.v);
    s->depth--;
    int16_t e = write_desc(s, type, data, n);
    if (data)
        tb_DisposePtr(data);
    RET((int32_t)e);
}

static void st_AEStreamWriteDesc(ppc_cpu *c)
{
    stream *s = stream_of(A(0));
    if (!s)
        RET(paramErr);
    RET((int32_t)write_desc(s, A(1), A(2), A(3)));
}

static void st_AEStreamWriteAEDesc(ppc_cpu *c)
{
    stream *s = stream_of(A(0));
    if (!s || !A(1))
        RET(paramErr);
    gaddr d = tb_NewPtr(8, false, true);
    if (!d)
        RET(memFullErr);
    int16_t e = tb_AEDuplicateDesc(A(1), d);
    if (!e)
        e = top(s)->kind == F_DESC ? errAEStreamBadNesting : add_value(s, d);
    if (e)
        tb_AEDisposeDesc(d);
    tb_DisposePtr(d);
    RET((int32_t)e);
}

static void st_AEStreamOpenList(ppc_cpu *c)
{
    stream *s = stream_of(A(0));
    RET(s ? (int32_t)push_container(s, F_LIST, T("list")) : paramErr);
}
static void st_AEStreamCloseList(ppc_cpu *c)
{
    stream *s = stream_of(A(0));
    RET(s ? (int32_t)pop_container(s, F_LIST) : paramErr);
}
static void st_AEStreamOpenRecord(ppc_cpu *c)
{
    stream *s = stream_of(A(0));
    RET(s ? (int32_t)push_container(s, F_RECORD, A(1)) : paramErr);
}
static void st_AEStreamSetRecordType(ppc_cpu *c)
{
    stream *s = stream_of(A(0));
    if (!s)
        RET(paramErr);
    frame *f = top(s);
    if (f->kind != F_RECORD)
        RET((int32_t)errAEStreamBadNesting);
    f->type = A(1);
    wr32(f->desc, A(1));
    RET(noErr);
}
static void st_AEStreamCloseRecord(ppc_cpu *c)
{
    stream *s = stream_of(A(0));
    RET(s ? (int32_t)pop_container(s, F_RECORD) : paramErr);
}

/* The key that the next value goes under (records and events). */
static int16_t set_key(stream *s, uint32_t key)
{
    frame *f = top(s);
    if ((f->kind != F_RECORD && f->kind != F_EVENT) || f->have_key)
        return errAEStreamBadNesting;
    f->have_key = true;
    f->key = key;
    return noErr;
}

static void st_AEStreamWriteKey(ppc_cpu *c)
{
    stream *s = stream_of(A(0));
    RET(s ? (int32_t)set_key(s, A(1)) : paramErr);
}
static void st_AEStreamWriteKeyDesc(ppc_cpu *c)
{
    stream *s = stream_of(A(0));
    if (!s)
        RET(paramErr);
    int16_t e = set_key(s, A(1));
    if (!e)
        e = write_desc(s, A(2), A(3), A(4));
    RET((int32_t)e);
}
static void st_AEStreamOpenKeyDesc(ppc_cpu *c)
{
    stream *s = stream_of(A(0));
    if (!s)
        RET(paramErr);
    int16_t e = set_key(s, A(1));
    if (!e) {
        frame *f = &s->f[s->depth++];
        memset(f, 0, sizeof *f);
        f->kind = F_DESC;
        f->type = A(2);
    }
    RET((int32_t)e);
}

/* AEStreamCreateEvent(class, id, targetType, targetData, targetLength,
 * returnID, transactionID): a stream with the event begun; the parameters
 * written to it are the event's, and closing it gives the event. */
static void st_AEStreamCreateEvent(ppc_cpu *c)
{
    gaddr ref = 0;
    stream *s = stream_new(&ref);
    if (!s)
        RET(0);
    gaddr addr = tb_NewPtr(8, false, true), ev = tb_NewPtr(8, false, true);
    int16_t e = addr && ev ? tb_AECreateDesc((int32_t)A(2), A(3), (int32_t)A(4), addr) : memFullErr;
    if (!e)
        e = tb_AECreateAppleEvent((int32_t)A(0), (int32_t)A(1), addr, S16(5), (int32_t)A(6), ev);
    if (addr) {
        tb_AEDisposeDesc(addr);
        tb_DisposePtr(addr);
    }
    if (e) {
        if (ev)
            tb_DisposePtr(ev);
        stream_free(s);
        RET(0);
    }
    frame *f = &s->f[s->depth++];
    memset(f, 0, sizeof *f);
    f->kind = F_EVENT;
    f->desc = ev;
    RET(ref);
}

/* AEStreamOpenEvent(event): the event given, to add to; closing the stream
 * gives it back (the stream owns it meanwhile). */
static void st_AEStreamOpenEvent(ppc_cpu *c)
{
    gaddr evt = A(0);
    if (!evt || rd32(evt) != T("aevt"))
        RET(0);
    gaddr ref = 0;
    stream *s = stream_new(&ref);
    if (!s)
        RET(0);
    gaddr ev = tb_NewPtr(8, false, true);
    if (!ev) {
        stream_free(s);
        RET(0);
    }
    wr32(ev, rd32(evt));
    wr32(ev + 4, rd32(evt + 4));
    frame *f = &s->f[s->depth++];
    memset(f, 0, sizeof *f);
    f->kind = F_EVENT;
    f->desc = ev;
    RET(ref);
}

/* AEStreamOptionalParam(ref, key): the keyword named in the event's list of optional parameters. */
static void st_AEStreamOptionalParam(ppc_cpu *c)
{
    stream *s = stream_of(A(0));
    if (!s)
        RET(paramErr);
    frame *f = top(s);
    if (f->kind != F_EVENT)
        RET((int32_t)errAEStreamBadNesting);
    uint8_t key[4];
    put_be(key, A(1), 4);
    gaddr tmp = tb_NewPtr(4, false, true);
    if (!tmp)
        RET(memFullErr);
    gput(tmp, key, 4);
    int16_t e = tb_AEPutAttributePtr(f->desc, (int32_t)T("optk"), (int32_t)T("keyw"), tmp, 4);
    tb_DisposePtr(tmp);
    RET((int32_t)e);
}

const ppc_export rest_ae_exports[] = {
    { "AEBuildAppleEvent", ae_AEBuildAppleEvent },
    { "AEBuildDesc", ae_AEBuildDesc },
    { "AEBuildParameters", ae_AEBuildParameters },
    { "AEPrintDescToHandle", ae_AEPrintDescToHandle },
    { "AEStreamClose", st_AEStreamClose },
    { "AEStreamCloseDesc", st_AEStreamCloseDesc },
    { "AEStreamCloseList", st_AEStreamCloseList },
    { "AEStreamCloseRecord", st_AEStreamCloseRecord },
    { "AEStreamCreateEvent", st_AEStreamCreateEvent },
    { "AEStreamOpen", st_AEStreamOpen },
    { "AEStreamOpenDesc", st_AEStreamOpenDesc },
    { "AEStreamOpenEvent", st_AEStreamOpenEvent },
    { "AEStreamOpenKeyDesc", st_AEStreamOpenKeyDesc },
    { "AEStreamOpenList", st_AEStreamOpenList },
    { "AEStreamOpenRecord", st_AEStreamOpenRecord },
    { "AEStreamOptionalParam", st_AEStreamOptionalParam },
    { "AEStreamSetRecordType", st_AEStreamSetRecordType },
    { "AEStreamWriteAEDesc", st_AEStreamWriteAEDesc },
    { "AEStreamWriteData", st_AEStreamWriteData },
    { "AEStreamWriteDesc", st_AEStreamWriteDesc },
    { "AEStreamWriteKey", st_AEStreamWriteKey },
    { "AEStreamWriteKeyDesc", st_AEStreamWriteKeyDesc },
    { "vAEBuildAppleEvent", ae_vAEBuildAppleEvent },
    { "vAEBuildDesc", ae_vAEBuildDesc },
    { "vAEBuildParameters", ae_vAEBuildParameters },
};
const unsigned rest_ae_export_count = sizeof rest_ae_exports / sizeof rest_ae_exports[0];
