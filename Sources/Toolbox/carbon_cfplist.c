/*
 * Core Foundation's property lists, preferences and XML parser as
 * CarbonLib had them (CFPropertyList.h, CFPreferences.h, CFXMLNode.h,
 * CFXMLParser.h).
 *
 * XML is read into a tree on the host first (xml_parse); a property list
 * is made from that tree, and CFXMLParser walks it calling the program's
 * callbacks in document order, as CF's parser calls them while it reads.
 * Property lists are written as XML the way Mac OS X 10.1's CF wrote
 * them, keys sorted; the older ASCII form ("key" = "value"; and .strings
 * files) is read too.
 *
 * Preferences live in the System Folder's Preferences folder, a domain a
 * file: <application ID>.plist (".GlobalPreferences" for any
 * application, those for the current host in a ByHost folder).  Mac OS 9
 * has one user, so kCFPreferencesAnyUser's domains are the current
 * user's.  A value set is written at once, so a program that never
 * synchronizes keeps its settings.
 */
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "carbon_cf.h"
#include "filemgr_internal.h"
#include "forks.h"

enum { kCFPropertyListImmutable = 0, kCFPropertyListMutableContainers = 1, kCFPropertyListMutableContainersAndLeaves = 2 };

/* CFXMLNodeTypeCode */
enum { XN_DOCUMENT = 1, XN_ELEMENT, XN_ATTRIBUTE, XN_PI, XN_COMMENT, XN_TEXT, XN_CDATA, XN_FRAGMENT, XN_ENTITY,
       XN_ENTITY_REF, XN_DOCTYPE, XN_WHITESPACE, XN_NOTATION, XN_ELEMENT_DECL, XN_ATTLIST_DECL };
/* CFXMLParserOptions */
enum { kCFXMLParserValidateDocument = 1, kCFXMLParserSkipMetaData = 2, kCFXMLParserReplacePhysicalEntities = 4,
       kCFXMLParserSkipWhitespace = 8 };
/* CFXMLParserStatusCode */
enum { kCFXMLStatusParseNotBegun = -2, kCFXMLStatusParseInProgress = -1, kCFXMLStatusParseSuccessful = 0,
       kCFXMLErrorUnexpectedEOF = 1, kCFXMLErrorUnknownEncoding, kCFXMLErrorEncodingConversionFailure,
       kCFXMLErrorMalformedProcessingInstruction, kCFXMLErrorMalformedDTD, kCFXMLErrorMalformedName,
       kCFXMLErrorMalformedCDSect, kCFXMLErrorMalformedCloseTag, kCFXMLErrorMalformedStartTag,
       kCFXMLErrorMalformedDocument, kCFXMLErrorElementlessDocument, kCFXMLErrorMalformedComment,
       kCFXMLErrorMalformedCharacterReference, kCFXMLErrorMalformedParsedCharacterData, kCFXMLErrorNoData };

/* ---------------------------------------------------------------------- */
/* Text from bytes: a byte order mark, an XML declaration's encoding, else
 * UTF-8, else Mac OS Roman. */

static bool text_of_bytes(const uint8_t *b, size_t n, uint16_t **u, uint32_t *nu)
{
    if (n >= 2 && ((b[0] == 0xFE && b[1] == 0xFF) || (b[0] == 0xFF && b[1] == 0xFE))) {
        bool le = b[0] == 0xFF;
        *nu = (uint32_t)(n / 2 - 1);
        *u = malloc(sizeof **u * (*nu + 1));
        if (!*u)
            return false;
        for (uint32_t i = 0; i < *nu; i++)
            (*u)[i] = le ? (uint16_t)(b[2 + 2 * i] | b[3 + 2 * i] << 8) : (uint16_t)(b[2 + 2 * i] << 8 | b[3 + 2 * i]);
        return true;
    }
    if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF)
        b += 3, n -= 3;
    /* <?xml ... encoding="name"?> */
    if (n > 5 && !memcmp(b, "<?xml", 5)) {
        const uint8_t *end = memchr(b, '>', n);
        size_t len = end ? (size_t)(end - b) : 0;
        char decl[256];
        if (len && len < sizeof decl) {
            memcpy(decl, b, len);
            decl[len] = 0;
            char *e = strstr(decl, "encoding");
            if (e && (e = strpbrk(e, "\"'"))) {
                char q = *e++, *close = strchr(e, q);
                if (close) {
                    *close = 0;
                    uint32_t enc = cf_encoding_of_iana(e);
                    if (enc != (uint32_t)kCFStringEncodingInvalidId && cf_decode(enc, b, n, u, nu))
                        return true;
                }
            }
        }
    }
    return cf_decode(kCFStringEncodingUTF8, b, n, u, nu) || cf_decode(kCFStringEncodingMacRoman, b, n, u, nu);
}

/* ---------------------------------------------------------------------- */
/* XML into a tree on the host */

typedef struct xnode xnode;
struct xnode {
    int type;
    cf_text s;           /* name, target, text */
    cf_text data;        /* a PI's data; a DOCTYPE's system ID */
    cf_text pub;         /* a DOCTYPE's public ID */
    cf_text *an, *av;    /* attributes */
    uint32_t nattr;
    bool empty, closed;
    uint32_t pos;
    xnode **kids;
    uint32_t nkids, cap;
};

static void xnode_free(xnode *x)
{
    if (!x)
        return;
    text_free(&x->s);
    text_free(&x->data);
    text_free(&x->pub);
    for (uint32_t i = 0; i < x->nattr; i++) {
        text_free(&x->an[i]);
        text_free(&x->av[i]);
    }
    free(x->an);
    free(x->av);
    for (uint32_t i = 0; i < x->nkids; i++)
        xnode_free(x->kids[i]);
    free(x->kids);
    free(x);
}

static xnode *xnode_add(xnode *parent, int type, uint32_t pos)
{
    xnode *x = calloc(1, sizeof *x);
    if (!x)
        return NULL;
    x->type = type;
    x->pos = pos;
    x->closed = true;
    if (parent) {
        if (parent->nkids == parent->cap) {
            uint32_t cap = parent->cap * 2 + 4;
            xnode **p = realloc(parent->kids, sizeof *p * cap);
            if (!p) {
                free(x);
                return NULL;
            }
            parent->kids = p;
            parent->cap = cap;
        }
        parent->kids[parent->nkids++] = x;
    }
    return x;
}

typedef struct {
    const uint16_t *u;
    uint32_t n, i;
    uint32_t options;
    int status;
} xreader;

static bool at(xreader *r, const char *s)
{
    size_t len = strlen(s);
    if (r->i + len > r->n)
        return false;
    for (size_t k = 0; k < len; k++)
        if (r->u[r->i + k] != (uint8_t)s[k])
            return false;
    return true;
}

static bool xml_space(uint16_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static void skip_space(xreader *r)
{
    while (r->i < r->n && xml_space(r->u[r->i]))
        r->i++;
}
static bool name_char(uint16_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == ':' ||
           c == '-' || c == '.' || c >= 0x80;
}

static bool read_name(xreader *r, cf_text *t)
{
    uint32_t s = r->i;
    while (r->i < r->n && name_char(r->u[r->i]))
        r->i++;
    if (r->i == s || (r->u[s] >= '0' && r->u[s] <= '9') || r->u[s] == '-' || r->u[s] == '.')
        return false;
    text_add(t, r->u + s, r->i - s);
    return true;
}

/* An entity or character reference at r->i ('&'): its text appended; the
 * name of one that isn't predefined in *name. */
static bool read_reference(xreader *r, cf_text *t, bool *replaced)
{
    uint32_t s = ++r->i;
    while (r->i < r->n && r->u[r->i] != ';' && r->i - s < 32)
        r->i++;
    if (r->i >= r->n || r->u[r->i] != ';') {
        r->status = kCFXMLErrorMalformedCharacterReference;
        return false;
    }
    uint32_t len = r->i - s;
    const uint16_t *e = r->u + s;
    r->i++;
    *replaced = true;
    if (len >= 2 && e[0] == '#') {
        uint32_t v = 0;
        bool hex = e[1] == 'x' || e[1] == 'X';
        for (uint32_t k = hex ? 2 : 1; k < len; k++) {
            int d = e[k] >= '0' && e[k] <= '9' ? e[k] - '0' : hex && (e[k] | 0x20) >= 'a' && (e[k] | 0x20) <= 'f' ? (e[k] | 0x20) - 'a' + 10 : -1;
            if (d < 0) {
                r->status = kCFXMLErrorMalformedCharacterReference;
                return false;
            }
            v = v * (hex ? 16 : 10) + (uint32_t)d;
        }
        if (v >= 0x10000) {
            v -= 0x10000;
            text_add_unit(t, (uint16_t)(0xD800 | v >> 10));
            text_add_unit(t, (uint16_t)(0xDC00 | (v & 0x3FF)));
        } else {
            text_add_unit(t, (uint16_t)v);
        }
        return true;
    }
    static const struct {
        const char *name;
        uint16_t ch;
    } predefined[] = { { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' } };
    for (unsigned k = 0; k < sizeof predefined / sizeof predefined[0]; k++) {
        size_t pl = strlen(predefined[k].name);
        bool same = pl == len;
        for (size_t j = 0; same && j < pl; j++)
            same = e[j] == (uint8_t)predefined[k].name[j];
        if (same) {
            text_add_unit(t, predefined[k].ch);
            return true;
        }
    }
    /* Some other entity: kept as written. */
    *replaced = false;
    text_add_unit(t, '&');
    text_add(t, e, len);
    text_add_unit(t, ';');
    return true;
}

static bool read_quoted(xreader *r, cf_text *t, bool references)
{
    if (r->i >= r->n || (r->u[r->i] != '"' && r->u[r->i] != '\''))
        return false;
    uint16_t q = r->u[r->i++];
    while (r->i < r->n && r->u[r->i] != q) {
        if (references && r->u[r->i] == '&') {
            bool rep;
            if (!read_reference(r, t, &rep))
                return false;
        } else {
            text_add_unit(t, r->u[r->i++]);
        }
    }
    if (r->i >= r->n)
        return false;
    r->i++;
    return true;
}

/* Up to the end marker; the text between. */
static bool read_until(xreader *r, const char *end, cf_text *t)
{
    while (r->i < r->n && !at(r, end))
        text_add_unit(t, r->u[r->i++]);
    if (r->i >= r->n)
        return false;
    r->i += (uint32_t)strlen(end);
    return true;
}

static bool only_space(const cf_text *t)
{
    for (uint32_t i = 0; i < t->n; i++)
        if (!xml_space(t->u[i]))
            return false;
    return true;
}

static void read_content(xreader *r, xnode *parent, int depth);

static bool read_element(xreader *r, xnode *parent, int depth)
{
    uint32_t pos = r->i++;
    xnode *e = xnode_add(parent, XN_ELEMENT, pos);
    if (!e || !read_name(r, &e->s)) {
        r->status = kCFXMLErrorMalformedName;
        return false;
    }
    for (;;) {
        skip_space(r);
        if (r->i >= r->n) {
            r->status = kCFXMLErrorUnexpectedEOF;
            return false;
        }
        if (at(r, "/>")) {
            r->i += 2;
            e->empty = true;
            return true;
        }
        if (r->u[r->i] == '>') {
            r->i++;
            break;
        }
        cf_text an = { 0 }, av = { 0 };
        if (!read_name(r, &an)) {
            r->status = kCFXMLErrorMalformedStartTag;
            text_free(&an);
            return false;
        }
        skip_space(r);
        if (r->i >= r->n || r->u[r->i] != '=') {
            r->status = kCFXMLErrorMalformedStartTag;
            text_free(&an);
            return false;
        }
        r->i++;
        skip_space(r);
        if (!read_quoted(r, &av, true)) {
            if (!r->status)
                r->status = kCFXMLErrorMalformedStartTag;
            text_free(&an);
            text_free(&av);
            return false;
        }
        cf_text *pa = realloc(e->an, sizeof *pa * (e->nattr + 1)), *pv = pa ? realloc(e->av, sizeof *pv * (e->nattr + 1)) : NULL;
        if (pa)
            e->an = pa;
        if (!pa || !pv) {
            text_free(&an);
            text_free(&av);
            return false;
        }
        e->av = pv;
        e->an[e->nattr] = an;
        e->av[e->nattr++] = av;
    }
    e->closed = false;
    read_content(r, e, depth + 1);
    if (r->status)
        return false;
    /* </name> */
    if (!at(r, "</")) {
        r->status = r->i >= r->n ? kCFXMLErrorUnexpectedEOF : kCFXMLErrorMalformedCloseTag;
        return false;
    }
    r->i += 2;
    cf_text close = { 0 };
    bool ok = read_name(r, &close) && close.n == e->s.n && !memcmp(close.u, e->s.u, sizeof *close.u * close.n);
    text_free(&close);
    skip_space(r);
    if (!ok || r->i >= r->n || r->u[r->i] != '>') {
        r->status = kCFXMLErrorMalformedCloseTag;
        return false;
    }
    r->i++;
    e->closed = true;
    return true;
}

static bool read_markup(xreader *r, xnode *parent, int depth)
{
    uint32_t pos = r->i;
    if (at(r, "<!--")) {
        r->i += 4;
        xnode *x = xnode_add(parent, XN_COMMENT, pos);
        if (!x || !read_until(r, "-->", &x->s)) {
            r->status = kCFXMLErrorMalformedComment;
            return false;
        }
        return true;
    }
    if (at(r, "<![CDATA[")) {
        r->i += 9;
        xnode *x = xnode_add(parent, XN_CDATA, pos);
        if (!x || !read_until(r, "]]>", &x->s)) {
            r->status = kCFXMLErrorMalformedCDSect;
            return false;
        }
        return true;
    }
    if (at(r, "<?")) {
        r->i += 2;
        xnode *x = xnode_add(parent, XN_PI, pos);
        if (!x || !read_name(r, &x->s)) {
            r->status = kCFXMLErrorMalformedProcessingInstruction;
            return false;
        }
        skip_space(r);
        if (!read_until(r, "?>", &x->data)) {
            r->status = kCFXMLErrorMalformedProcessingInstruction;
            return false;
        }
        return true;
    }
    if (at(r, "<!DOCTYPE")) {
        r->i += 9;
        skip_space(r);
        xnode *x = xnode_add(parent, XN_DOCTYPE, pos);
        if (!x || !read_name(r, &x->s)) {
            r->status = kCFXMLErrorMalformedDTD;
            return false;
        }
        skip_space(r);
        if (at(r, "PUBLIC")) {
            r->i += 6;
            skip_space(r);
            if (!read_quoted(r, &x->pub, false)) {
                r->status = kCFXMLErrorMalformedDTD;
                return false;
            }
            skip_space(r);
            read_quoted(r, &x->data, false);
        } else if (at(r, "SYSTEM")) {
            r->i += 6;
            skip_space(r);
            if (!read_quoted(r, &x->data, false)) {
                r->status = kCFXMLErrorMalformedDTD;
                return false;
            }
        }
        skip_space(r);
        if (r->i < r->n && r->u[r->i] == '[') { /* the internal subset, passed over */
            int level = 0;
            for (; r->i < r->n; r->i++) {
                if (r->u[r->i] == '[')
                    level++;
                else if (r->u[r->i] == ']' && --level == 0)
                    break;
            }
            r->i++;
            skip_space(r);
        }
        if (r->i >= r->n || r->u[r->i] != '>') {
            r->status = kCFXMLErrorMalformedDTD;
            return false;
        }
        r->i++;
        return true;
    }
    if (at(r, "<!")) {
        r->status = kCFXMLErrorMalformedDTD;
        return false;
    }
    return read_element(r, parent, depth);
}

/* Text and markup until the parent's end tag (or the end). */
static void read_content(xreader *r, xnode *parent, int depth)
{
    if (depth > 512) {
        r->status = kCFXMLErrorMalformedDocument;
        return;
    }
    while (r->i < r->n && !r->status) {
        if (at(r, "</"))
            return;
        if (r->u[r->i] == '<') {
            read_markup(r, parent, depth);
            continue;
        }
        /* Character data, with references. */
        uint32_t pos = r->i;
        cf_text t = { 0 };
        while (r->i < r->n && r->u[r->i] != '<') {
            if (r->u[r->i] != '&') {
                text_add_unit(&t, r->u[r->i++]);
                continue;
            }
            uint32_t ref_at = r->i;
            cf_text ref = { 0 };
            bool replaced;
            if (!read_reference(r, &ref, &replaced)) {
                text_free(&ref);
                text_free(&t);
                return;
            }
            bool keep_ref = !(r->options & kCFXMLParserReplacePhysicalEntities) &&
                            !(r->u[ref_at + 1] == '#') && parent && parent->type != XN_DOCUMENT;
            if (keep_ref || !replaced) {
                /* An entity reference node between text nodes. */
                if (t.n) {
                    xnode *x = xnode_add(parent, only_space(&t) ? XN_WHITESPACE : XN_TEXT, pos);
                    if (x)
                        x->s = t;
                    else
                        text_free(&t);
                    memset(&t, 0, sizeof t);
                }
                xnode *x = xnode_add(parent, XN_ENTITY_REF, ref_at);
                if (x) {
                    uint32_t end = r->i - 1;
                    text_add(&x->s, r->u + ref_at + 1, end - ref_at - 1);
                    x->data = ref; /* what it stands for */
                } else {
                    text_free(&ref);
                }
                pos = r->i;
                continue;
            }
            text_add(&t, ref.u, ref.n);
            text_free(&ref);
        }
        if (!t.n)
            continue;
        bool ws = only_space(&t);
        if (ws && (r->options & kCFXMLParserSkipWhitespace || parent->type == XN_DOCUMENT)) {
            text_free(&t);
            continue;
        }
        if (parent->type == XN_DOCUMENT) {
            text_free(&t);
            r->status = kCFXMLErrorMalformedDocument;
            return;
        }
        xnode *x = xnode_add(parent, ws ? XN_WHITESPACE : XN_TEXT, pos);
        if (x)
            x->s = t;
        else
            text_free(&t);
    }
}

/* The whole document: a tree (complete, or as far as it went) and the status. */
static xnode *xml_parse(const uint16_t *u, uint32_t n, uint32_t options, int *status, uint32_t *where)
{
    xreader r = { u, n, 0, options, 0 };
    xnode *doc = xnode_add(NULL, XN_DOCUMENT, 0);
    if (!doc) {
        *status = kCFXMLErrorNoData;
        return NULL;
    }
    if (!n) {
        *status = kCFXMLErrorNoData;
        return doc;
    }
    read_content(&r, doc, 0);
    if (!r.status && r.i < r.n)
        r.status = kCFXMLErrorMalformedDocument;
    if (!r.status) {
        bool element = false;
        for (uint32_t i = 0; i < doc->nkids; i++)
            element |= doc->kids[i]->type == XN_ELEMENT;
        if (!element)
            r.status = kCFXMLErrorElementlessDocument;
    }
    *status = r.status;
    if (where)
        *where = r.i;
    return doc;
}

static xnode *first_element(xnode *x)
{
    for (uint32_t i = 0; x && i < x->nkids; i++)
        if (x->kids[i]->type == XN_ELEMENT)
            return x->kids[i];
    return NULL;
}

static bool name_is(const xnode *x, const char *s)
{
    size_t n = strlen(s);
    if (x->s.n != n)
        return false;
    for (size_t i = 0; i < n; i++)
        if (x->s.u[i] != (uint8_t)s[i])
            return false;
    return true;
}

/* An element's text content (its text, CDATA and references). */
static void element_text(const xnode *x, cf_text *t)
{
    for (uint32_t i = 0; i < x->nkids; i++) {
        const xnode *k = x->kids[i];
        if (k->type == XN_TEXT || k->type == XN_CDATA || k->type == XN_WHITESPACE)
            text_add(t, k->s.u, k->s.n);
        else if (k->type == XN_ENTITY_REF)
            text_add(t, k->data.u, k->data.n);
    }
}

static char *text_ascii(const cf_text *t)
{
    char *s = malloc(t->n + 1);
    if (!s)
        return NULL;
    for (uint32_t i = 0; i < t->n; i++)
        s[i] = t->u[i] < 128 ? (char)t->u[i] : '?';
    s[t->n] = 0;
    return s;
}

/* ---------------------------------------------------------------------- */
/* Base64 and dates */

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static uint8_t *base64_decode(const cf_text *t, size_t *n)
{
    uint8_t *out = malloc(t->n * 3 / 4 + 4);
    uint32_t acc = 0;
    int bits = 0;
    size_t k = 0;
    for (uint32_t i = 0; out && i < t->n; i++) {
        const char *p = t->u[i] < 128 && t->u[i] ? strchr(b64, (char)t->u[i]) : NULL;
        if (!p)
            continue;
        acc = acc << 6 | (uint32_t)(p - b64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[k++] = (uint8_t)(acc >> bits);
        }
    }
    *n = k;
    return out;
}

static void base64_encode(const uint8_t *b, size_t n, cf_text *t, const char *indent)
{
    char line[80];
    size_t ll = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)b[i] << 16 | (i + 1 < n ? (uint32_t)b[i + 1] << 8 : 0) | (i + 2 < n ? b[i + 2] : 0);
        line[ll++] = b64[v >> 18 & 63];
        line[ll++] = b64[v >> 12 & 63];
        line[ll++] = i + 1 < n ? b64[v >> 6 & 63] : '=';
        line[ll++] = i + 2 < n ? b64[v & 63] : '=';
        if (ll >= 76 || i + 3 >= n) {
            line[ll] = 0;
            text_add_utf8(t, indent);
            text_add_utf8(t, line);
            text_add_unit(t, '\n');
            ll = 0;
        }
    }
}

/* "2001-01-01T00:00:00Z" */
static bool parse_date(const char *s, double *at)
{
    int y, mo, d, h = 0, mi = 0, sec = 0;
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) < 3)
        return false;
    /* Days from 2001 by the civil calendar. */
    int64_t yy = y - (mo <= 2), era = (yy >= 0 ? yy : yy - 399) / 400, yoe = yy - era * 400;
    int64_t doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + doe - 719468 - 11323;
    *at = (double)days * 86400.0 + h * 3600.0 + mi * 60.0 + sec;
    return true;
}

static void format_date(double at, char out[32])
{
    int64_t z = (int64_t)floor(at / 86400.0);
    double rem = at - (double)z * 86400.0;
    z += 719468 + 11323;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097, doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    int64_t y = yoe + era * 400 + (m <= 2);
    int secs = (int)rem;
    snprintf(out, 32, "%04lld-%02lld-%02lldT%02d:%02d:%02dZ", (long long)y, (long long)m, (long long)d, secs / 3600,
             secs / 60 % 60, secs % 60);
}

/* ---------------------------------------------------------------------- */
/* Property lists from XML */

static gaddr plist_of_element(gaddr alloc, xnode *x, uint32_t mut, gaddr *error)
{
    cf_text t = { 0 };
    gaddr r = 0;
    bool leaves = mut == kCFPropertyListMutableContainersAndLeaves, containers = mut != kCFPropertyListImmutable;
    if (name_is(x, "dict")) {
        r = cfdict_new(alloc, containers);
        gaddr key = 0;
        for (uint32_t i = 0; i < x->nkids; i++) {
            xnode *k = x->kids[i];
            if (k->type != XN_ELEMENT)
                continue;
            if (name_is(k, "key")) {
                cf_release(key);
                cf_text kt = { 0 };
                element_text(k, &kt);
                key = cfstr_new(alloc, kt.u, kt.n, false);
                text_free(&kt);
                continue;
            }
            gaddr v = plist_of_element(alloc, k, mut, error);
            if (!v || !key) {
                cf_release(v);
                cf_release(key);
                cf_release(r);
                if (error && !*error)
                    *error = cfstr_from_utf8("Encountered a value without a key in a dictionary");
                return 0;
            }
            cfdict_set(r, key, v);
            cf_release(v);
            cf_release(key);
            key = 0;
        }
        cf_release(key);
        return r;
    }
    if (name_is(x, "array")) {
        r = cfarray_new(alloc, NULL, 0, true);
        for (uint32_t i = 0; i < x->nkids; i++) {
            if (x->kids[i]->type != XN_ELEMENT)
                continue;
            gaddr v = plist_of_element(alloc, x->kids[i], mut, error);
            if (!v) {
                cf_release(r);
                return 0;
            }
            cfarray_append(r, v);
            cf_release(v);
        }
        if (!containers) { /* an immutable copy */
            uint32_t n = cfarray_count(r), *v = malloc(sizeof *v * (n ? n : 1));
            for (uint32_t i = 0; v && i < n; i++)
                v[i] = cfarray_get(r, i);
            gaddr im = cfarray_new(alloc, v, n, false);
            free(v);
            cf_release(r);
            r = im;
        }
        return r;
    }
    if (name_is(x, "true") || name_is(x, "false"))
        return cfbool(name_is(x, "true"));
    element_text(x, &t);
    if (name_is(x, "string")) {
        r = cfstr_new(alloc, t.u, t.n, leaves);
    } else if (name_is(x, "integer")) {
        char *s = text_ascii(&t);
        r = s ? cfnum_new_int(alloc, strtoll(s, NULL, 0)) : 0;
        free(s);
    } else if (name_is(x, "real")) {
        char *s = text_ascii(&t);
        r = s ? cfnum_new_double(alloc, strtod(s, NULL)) : 0;
        free(s);
    } else if (name_is(x, "date")) {
        char *s = text_ascii(&t);
        double at;
        r = s && parse_date(s, &at) ? cfdate_new(alloc, at) : 0;
        free(s);
    } else if (name_is(x, "data")) {
        size_t n;
        uint8_t *b = base64_decode(&t, &n);
        r = b ? cfdata_new(alloc, b, (uint32_t)n, leaves) : 0;
        free(b);
    }
    text_free(&t);
    if (!r && error && !*error)
        *error = cfstr_from_utf8("Encountered unknown tag or bad value in the property list");
    return r;
}

/* ---------------------------------------------------------------------- */
/* Property lists in the old ASCII form (and .strings files) */

typedef struct {
    const uint16_t *u;
    uint32_t n, i;
} areader;

static void a_skip(areader *r)
{
    for (;;) {
        while (r->i < r->n && (xml_space(r->u[r->i]) || r->u[r->i] == 0xFEFF))
            r->i++;
        if (r->i + 1 < r->n && r->u[r->i] == '/' && r->u[r->i + 1] == '/') {
            while (r->i < r->n && r->u[r->i] != '\n')
                r->i++;
        } else if (r->i + 1 < r->n && r->u[r->i] == '/' && r->u[r->i + 1] == '*') {
            r->i += 2;
            while (r->i + 1 < r->n && !(r->u[r->i] == '*' && r->u[r->i + 1] == '/'))
                r->i++;
            r->i += 2;
        } else {
            return;
        }
    }
}

static bool a_unquoted(uint16_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$' ||
           c == '+' || c == '/' || c == ':' || c == '.' || c == '-';
}

static gaddr a_value(gaddr alloc, areader *r, uint32_t mut, int depth);

static gaddr a_string(gaddr alloc, areader *r, uint32_t mut)
{
    cf_text t = { 0 };
    if (r->i < r->n && r->u[r->i] == '"') {
        r->i++;
        while (r->i < r->n && r->u[r->i] != '"') {
            uint16_t ch = r->u[r->i++];
            if (ch == '\\' && r->i < r->n) {
                ch = r->u[r->i++];
                switch (ch) {
                case 'n': ch = '\n'; break;
                case 't': ch = '\t'; break;
                case 'r': ch = '\r'; break;
                case 'a': ch = 7; break;
                case 'b': ch = 8; break;
                case 'f': ch = 12; break;
                case 'v': ch = 11; break;
                case 'U': case 'u': {
                    uint32_t v = 0;
                    for (int k = 0; k < 4 && r->i < r->n; k++) {
                        uint16_t h = r->u[r->i];
                        int d = h >= '0' && h <= '9' ? h - '0' : (h | 0x20) >= 'a' && (h | 0x20) <= 'f' ? (h | 0x20) - 'a' + 10 : -1;
                        if (d < 0)
                            break;
                        v = v * 16 + (uint32_t)d;
                        r->i++;
                    }
                    ch = (uint16_t)v;
                    break;
                }
                default:
                    if (ch >= '0' && ch <= '7') {
                        uint32_t v = ch - '0';
                        for (int k = 0; k < 2 && r->i < r->n && r->u[r->i] >= '0' && r->u[r->i] <= '7'; k++)
                            v = v * 8 + (uint32_t)(r->u[r->i++] - '0');
                        ch = (uint16_t)v;
                    }
                }
            }
            text_add_unit(&t, ch);
        }
        if (r->i >= r->n) {
            text_free(&t);
            return 0;
        }
        r->i++;
    } else {
        while (r->i < r->n && a_unquoted(r->u[r->i]))
            text_add_unit(&t, r->u[r->i++]);
        if (!t.n)
            return 0;
    }
    gaddr s = cfstr_new(alloc, t.u, t.n, mut == kCFPropertyListMutableContainersAndLeaves);
    text_free(&t);
    return s;
}

/* key = value; ... up to the closing brace (or the end, for .strings). */
static gaddr a_dict(gaddr alloc, areader *r, uint32_t mut, int depth, bool top)
{
    gaddr d = cfdict_new(alloc, mut != kCFPropertyListImmutable);
    for (;;) {
        a_skip(r);
        if (r->i >= r->n) {
            if (top)
                return d;
            break;
        }
        if (!top && r->u[r->i] == '}') {
            r->i++;
            return d;
        }
        gaddr key = a_string(alloc, r, kCFPropertyListImmutable);
        if (!key)
            break;
        a_skip(r);
        gaddr v;
        if (r->i < r->n && r->u[r->i] == '=') {
            r->i++;
            a_skip(r);
            v = a_value(alloc, r, mut, depth + 1);
        } else {
            v = cf_retain(key); /* "key"; alone is itself */
        }
        a_skip(r);
        if (!v || r->i >= r->n || r->u[r->i] != ';') {
            cf_release(key);
            cf_release(v);
            break;
        }
        r->i++;
        cfdict_set(d, key, v);
        cf_release(key);
        cf_release(v);
    }
    cf_release(d);
    return 0;
}

static gaddr a_value(gaddr alloc, areader *r, uint32_t mut, int depth)
{
    a_skip(r);
    if (r->i >= r->n || depth > 256)
        return 0;
    uint16_t ch = r->u[r->i];
    if (ch == '{') {
        r->i++;
        return a_dict(alloc, r, mut, depth, false);
    }
    if (ch == '(') {
        r->i++;
        gaddr a = cfarray_new(alloc, NULL, 0, true);
        a_skip(r);
        if (r->i < r->n && r->u[r->i] == ')') {
            r->i++;
            return a;
        }
        for (;;) {
            gaddr v = a_value(alloc, r, mut, depth + 1);
            if (!v)
                break;
            cfarray_append(a, v);
            cf_release(v);
            a_skip(r);
            if (r->i < r->n && r->u[r->i] == ',') {
                r->i++;
                a_skip(r);
                if (r->i < r->n && r->u[r->i] == ')') {
                    r->i++;
                    return a;
                }
                continue;
            }
            if (r->i < r->n && r->u[r->i] == ')') {
                r->i++;
                return a;
            }
            break;
        }
        cf_release(a);
        return 0;
    }
    if (ch == '<') {
        r->i++;
        uint8_t *b = malloc(r->n);
        size_t k = 0;
        int half = -1;
        while (b && r->i < r->n && r->u[r->i] != '>') {
            uint16_t h = r->u[r->i++];
            int d = h >= '0' && h <= '9' ? h - '0' : (h | 0x20) >= 'a' && (h | 0x20) <= 'f' ? (h | 0x20) - 'a' + 10 : -1;
            if (d < 0)
                continue;
            if (half < 0)
                half = d;
            else {
                b[k++] = (uint8_t)(half << 4 | d);
                half = -1;
            }
        }
        r->i++;
        gaddr data = b ? cfdata_new(alloc, b, (uint32_t)k, mut == kCFPropertyListMutableContainersAndLeaves) : 0;
        free(b);
        return data;
    }
    return a_string(alloc, r, mut);
}

static gaddr plist_ascii(gaddr alloc, const uint16_t *u, uint32_t n, uint32_t mut)
{
    areader r = { u, n, 0 };
    gaddr v = a_value(alloc, &r, mut, 0);
    a_skip(&r);
    if (v && r.i >= r.n)
        return v;
    cf_release(v);
    /* A .strings file: a dictionary without its braces. */
    r.i = 0;
    return a_dict(alloc, &r, mut, 0, true);
}

gaddr cfplist_from_data(gaddr alloc, const uint8_t *b, size_t n, uint32_t mut, gaddr *error)
{
    uint16_t *u;
    uint32_t nu;
    if (error)
        *error = 0;
    if (!text_of_bytes(b, n, &u, &nu)) {
        if (error)
            *error = cfstr_from_utf8("Could not determine the encoding of the property list");
        return 0;
    }
    uint32_t i = 0;
    while (i < nu && (xml_space(u[i]) || u[i] == 0xFEFF))
        i++;
    gaddr r = 0;
    if (i < nu && u[i] == '<' && i + 1 < nu && (u[i + 1] == '?' || u[i + 1] == '!' || (u[i + 1] | 0x20) >= 'a')) {
        int status;
        xnode *doc = xml_parse(u, nu, kCFXMLParserReplacePhysicalEntities | kCFXMLParserSkipWhitespace, &status, NULL);
        xnode *top = status ? NULL : first_element(doc);
        if (top && name_is(top, "plist"))
            top = first_element(top);
        if (top)
            r = plist_of_element(alloc, top, mut, error);
        else if (error && !*error)
            *error = cfstr_from_utf8(status ? "XML parser error" : "Empty property list");
        xnode_free(doc);
    } else {
        r = plist_ascii(alloc, u + i, nu - i, mut);
        if (!r && error)
            *error = cfstr_from_utf8("Unexpected character in the property list");
    }
    free(u);
    return r;
}

/* ---------------------------------------------------------------------- */
/* Property lists to XML */

static bool is_plist(gaddr v, int depth)
{
    switch (cf_type_of(v)) {
    case CFT_STRING: case CFT_DATA: case CFT_NUMBER: case CFT_BOOLEAN: case CFT_DATE:
        return true;
    case CFT_ARRAY:
        for (uint32_t i = 0; i < cfarray_count(v); i++)
            if (depth > 256 || !is_plist(cfarray_get(v, i), depth + 1))
                return false;
        return true;
    case CFT_DICTIONARY: {
        uint32_t *k, *vals, n = cfdict_entries(v, &k, &vals);
        bool ok = depth <= 256;
        for (uint32_t i = 0; ok && i < n; i++)
            ok = cf_type_of(k[i]) == CFT_STRING && is_plist(vals[i], depth + 1);
        free(k);
        free(vals);
        return ok;
    }
    default:
        return false;
    }
}

static void add_escaped_text(cf_text *t, gaddr s)
{
    uint32_t n;
    const uint16_t *u = cfstr_chars(s, &n);
    for (uint32_t i = 0; u && i < n; i++) {
        if (u[i] == '&')
            text_add_utf8(t, "&amp;");
        else if (u[i] == '<')
            text_add_utf8(t, "&lt;");
        else if (u[i] == '>')
            text_add_utf8(t, "&gt;");
        else
            text_add_unit(t, u[i]);
    }
}

static int compare_keys(const void *a, const void *b)
{
    uint32_t na, nb;
    const uint16_t *x = cfstr_chars(*(const uint32_t *)a, &na), *y = cfstr_chars(*(const uint32_t *)b, &nb);
    for (uint32_t i = 0; i < na && i < nb; i++)
        if (x[i] != y[i])
            return x[i] < y[i] ? -1 : 1;
    return na < nb ? -1 : na > nb;
}

static void xml_value(cf_text *t, gaddr v, int depth)
{
    char indent[64];
    int d = depth < 60 ? depth : 60;
    memset(indent, '\t', (size_t)d);
    indent[d] = 0;
    text_add_utf8(t, indent);
    switch (cf_type_of(v)) {
    case CFT_STRING:
        text_add_utf8(t, "<string>");
        add_escaped_text(t, v);
        text_add_utf8(t, "</string>\n");
        break;
    case CFT_BOOLEAN:
        text_add_utf8(t, v == cfbool(true) ? "<true/>\n" : "<false/>\n");
        break;
    case CFT_NUMBER: {
        bool f;
        int64_t i;
        double dv;
        cfnum_value(v, &f, &i, &dv);
        if (f)
            text_printf(t, "<real>%.17g</real>\n", dv);
        else
            text_printf(t, "<integer>%lld</integer>\n", (long long)i);
        break;
    }
    case CFT_DATE: {
        double at;
        char s[32];
        cfdate_value(v, &at);
        format_date(at, s);
        text_printf(t, "<date>%s</date>\n", s);
        break;
    }
    case CFT_DATA: {
        uint32_t n;
        gaddr p = cfdata_bytes(v, &n);
        uint8_t *b = malloc(n ? n : 1);
        if (b && n)
            gget(b, p, n);
        text_add_utf8(t, "<data>\n");
        if (b)
            base64_encode(b, n, t, indent);
        free(b);
        text_add_utf8(t, indent);
        text_add_utf8(t, "</data>\n");
        break;
    }
    case CFT_ARRAY: {
        uint32_t n = cfarray_count(v);
        if (!n) {
            text_add_utf8(t, "<array/>\n");
            break;
        }
        text_add_utf8(t, "<array>\n");
        for (uint32_t i = 0; i < n; i++)
            xml_value(t, cfarray_get(v, i), depth + 1);
        text_add_utf8(t, indent);
        text_add_utf8(t, "</array>\n");
        break;
    }
    case CFT_DICTIONARY: {
        uint32_t *k, *vals, n = cfdict_entries(v, &k, &vals);
        if (!n) {
            text_add_utf8(t, "<dict/>\n");
        } else {
            qsort(k, n, sizeof *k, compare_keys);
            text_add_utf8(t, "<dict>\n");
            for (uint32_t i = 0; i < n; i++) {
                uint32_t value = 0;
                cfdict_get(v, k[i], &value);
                text_add_utf8(t, indent);
                text_add_utf8(t, "\t<key>");
                add_escaped_text(t, k[i]);
                text_add_utf8(t, "</key>\n");
                xml_value(t, value, depth + 1);
            }
            text_add_utf8(t, indent);
            text_add_utf8(t, "</dict>\n");
        }
        free(k);
        free(vals);
        break;
    }
    }
}

char *cfplist_to_xml(gaddr plist, size_t *n)
{
    if (!is_plist(plist, 0))
        return NULL;
    cf_text t = { 0 };
    text_add_utf8(&t, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                      "<!DOCTYPE plist PUBLIC \"-//Apple Computer//DTD PLIST 1.0//EN\" "
                      "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                      "<plist version=\"1.0\">\n");
    xml_value(&t, plist, 0);
    text_add_utf8(&t, "</plist>\n");
    uint8_t *b;
    size_t nb;
    uint32_t used;
    cf_encode(kCFStringEncodingUTF8, t.u, t.n, '?', &b, &nb, &used);
    text_free(&t);
    *n = nb;
    return (char *)b;
}

/* CFPropertyListCreateFromXMLData(allocator, xmlData, mutabilityOption, *errorString) */
static void cf_CFPropertyListCreateFromXMLData(ppc_cpu *c)
{
    uint32_t n;
    gaddr p = cfdata_bytes(R(1), &n);
    gaddr err = 0;
    gaddr r = 0;
    if (cf_type_of(R(1)) == CFT_DATA) {
        uint8_t *b = malloc(n ? n : 1);
        if (b) {
            gget(b, p, n);
            r = cfplist_from_data(R(0), b, n, R(2), &err);
            free(b);
        }
    } else {
        err = cfstr_from_utf8("No property list data");
    }
    if (R(3))
        wr32(R(3), r ? 0 : err);
    if (r || !R(3))
        cf_release(err);
    RETURN(r);
}

static void cf_CFPropertyListCreateXMLData(ppc_cpu *c)
{
    size_t n;
    char *x = cfplist_to_xml(R(1), &n);
    gaddr d = x ? cfdata_new(R(0), (const uint8_t *)x, (uint32_t)n, false) : 0;
    free(x);
    RETURN(d);
}

static gaddr deep_copy(gaddr alloc, gaddr v, uint32_t mut, int depth)
{
    bool containers = mut != kCFPropertyListImmutable, leaves = mut == kCFPropertyListMutableContainersAndLeaves;
    if (depth > 256)
        return 0;
    switch (cf_type_of(v)) {
    case CFT_STRING: {
        uint32_t n;
        const uint16_t *u = cfstr_chars(v, &n);
        return cfstr_new(alloc, u, n, leaves);
    }
    case CFT_DATA: {
        uint32_t n;
        gaddr p = cfdata_bytes(v, &n);
        uint8_t *b = malloc(n ? n : 1);
        if (!b)
            return 0;
        gget(b, p, n);
        gaddr r = cfdata_new(alloc, b, n, leaves);
        free(b);
        return r;
    }
    case CFT_NUMBER: case CFT_BOOLEAN: case CFT_DATE:
        return cf_retain(v);
    case CFT_ARRAY: {
        uint32_t n = cfarray_count(v), *vals = malloc(sizeof *vals * (n ? n : 1));
        for (uint32_t i = 0; vals && i < n; i++)
            vals[i] = deep_copy(alloc, cfarray_get(v, i), mut, depth + 1);
        gaddr r = vals ? cfarray_new(alloc, vals, n, containers) : 0;
        for (uint32_t i = 0; vals && i < n; i++)
            cf_release(vals[i]);
        free(vals);
        return r;
    }
    case CFT_DICTIONARY: {
        uint32_t *k, *vals, n = cfdict_entries(v, &k, &vals);
        gaddr r = cfdict_new(alloc, containers);
        for (uint32_t i = 0; i < n; i++) {
            gaddr cv = deep_copy(alloc, vals[i], mut, depth + 1);
            cfdict_set(r, k[i], cv);
            cf_release(cv);
        }
        free(k);
        free(vals);
        return r;
    }
    default:
        return 0;
    }
}

/* CFPropertyListCreateDeepCopy(allocator, propertyList, mutabilityOption) */
static void cf_CFPropertyListCreateDeepCopy(ppc_cpu *c) { RETURN(deep_copy(R(0), R(1), R(2), 0)); }

/* ---------------------------------------------------------------------- */
/* Preferences */

typedef struct {
    char *app;    /* the file's name, less ".plist" */
    bool by_host;
    gaddr dict;   /* its keys and values */
    bool loaded;
} domain;

static domain *domains;
static unsigned ndomains, domains_cap;

typedef struct {
    char *app, *suite;
} suite_link;
static suite_link *suites;
static unsigned nsuites;

/* An application ID's name for its file: the current application is its
 * bundle's identifier, or the application's name. */
static char *app_name(gaddr id)
{
    char *s = cfstr_utf8(id);
    if (!s)
        return NULL;
    if (!strcmp(s, "kCFPreferencesAnyApplication")) {
        free(s);
        return strdup(".GlobalPreferences");
    }
    if (strcmp(s, "kCFPreferencesCurrentApplication"))
        return s;
    free(s);
    char *name = cfstr_utf8(cfbundle_main_identifier());
    if (name && name[0])
        return name;
    free(name);
    uint8_t len = rd8(LM_CurApName);
    char mac[64], utf8[256];
    if (len > 31)
        len = 31;
    gget(mac, LM_CurApName + 1, len);
    macroman_to_utf8((const uint8_t *)mac, len, utf8, sizeof utf8);
    return strdup(utf8[0] ? utf8 : "Application");
}

static bool is_current_host(gaddr host)
{
    char *s = cfstr_utf8(host);
    bool r = s && !strcmp(s, "kCFPreferencesCurrentHost");
    free(s);
    return r;
}

/* The domain's file: Preferences[:ByHost]:<name>.plist, within the 31
 * characters of an HFS name. */
static bool domain_path(const domain *d, char *out, size_t n)
{
    char dir[PATH_MAX];
    if (fm_find_folder(fourcc("pref"), true, dir, sizeof dir) != noErr)
        return false;
    if (d->by_host) {
        char sub[PATH_MAX];
        fm_child_path(dir, "ByHost", sub, sizeof sub);
        if (!fm_host_exists(sub) && fm_create_dir(sub) != noErr)
            return false;
        snprintf(dir, sizeof dir, "%s", sub);
    }
    char name[64];
    uint8_t mac[512];
    size_t len = utf8_to_macroman(d->app, mac, sizeof mac);
    if (len + 6 <= 31) {
        snprintf(name, sizeof name, "%s.plist", d->app);
    } else {
        uint32_t h = 0;
        for (const char *p = d->app; *p; p++)
            h = h * 31 + (uint8_t)*p;
        char head[64];
        snprintf(head, sizeof head, "%s", d->app);
        /* 20 characters, a mark and four hex digits. */
        size_t keep = 0, chars = 0;
        while (head[keep] && chars < 20) {
            keep++;
            while ((head[keep] & 0xC0) == 0x80)
                keep++;
            chars++;
        }
        head[keep] = 0;
        snprintf(name, sizeof name, "%s~%04X.plist", head, h & 0xFFFF);
    }
    for (char *p = name; *p; p++)
        if (*p == '/')
            *p = ':'; /* as the host keeps a Mac name's slash */
    fm_child_path(dir, name, out, n);
    return true;
}

static void domain_load(domain *d)
{
    char path[PATH_MAX];
    uint8_t *b;
    size_t n;
    gaddr dict = 0;
    if (domain_path(d, path, sizeof path) && cf_read_host_file(path, &b, &n)) {
        dict = cfplist_from_data(cf_allocator_system(), b, n, kCFPropertyListMutableContainers, NULL);
        free(b);
    }
    if (cf_type_of(dict) != CFT_DICTIONARY) {
        cf_release(dict);
        dict = cfdict_new(cf_allocator_system(), true);
    }
    cf_release(d->dict);
    d->dict = dict;
    d->loaded = true;
}

static bool domain_save(domain *d)
{
    char path[PATH_MAX];
    size_t n;
    char *x = cfplist_to_xml(d->dict, &n);
    bool ok = x && domain_path(d, path, sizeof path) && cf_write_host_file(path, (const uint8_t *)x, n);
    free(x);
    return ok;
}

static domain *domain_named(const char *app, bool by_host)
{
    for (unsigned i = 0; i < ndomains; i++)
        if (domains[i].by_host == by_host && !strcmp(domains[i].app, app)) {
            if (!domains[i].loaded)
                domain_load(&domains[i]);
            return &domains[i];
        }
    if (ndomains == domains_cap) {
        unsigned cap = domains_cap * 2 + 8;
        domain *p = realloc(domains, sizeof *p * cap);
        if (!p)
            return NULL;
        domains = p;
        domains_cap = cap;
    }
    domain *d = &domains[ndomains++];
    memset(d, 0, sizeof *d);
    d->app = strdup(app);
    d->by_host = by_host;
    domain_load(d);
    return d;
}

static domain *domain_of(gaddr app, gaddr host)
{
    char *name = app_name(app);
    domain *d = name ? domain_named(name, is_current_host(host)) : NULL;
    free(name);
    return d;
}

/* The search list of the App calls: the application's domains, its
 * suites', then any application's. */
static unsigned search_list(gaddr app, domain **out, unsigned max)
{
    unsigned n = 0;
    char *name = app_name(app);
    if (!name)
        return 0;
    const char *names[2 + 16];
    unsigned nn = 0;
    names[nn++] = name;
    for (unsigned i = 0; i < nsuites && nn < 17; i++)
        if (!strcmp(suites[i].app, name))
            names[nn++] = suites[i].suite;
    names[nn++] = ".GlobalPreferences";
    for (unsigned i = 0; i < nn && n + 2 <= max; i++) {
        domain *d = domain_named(names[i], true);
        if (d)
            out[n++] = d;
        d = domain_named(names[i], false);
        if (d)
            out[n++] = d;
    }
    free(name);
    return n;
}

static gaddr app_value(gaddr key, gaddr app)
{
    domain *list[40];
    unsigned n = search_list(app, list, 40);
    for (unsigned i = 0; i < n; i++) {
        uint32_t v;
        if (cfdict_get(list[i]->dict, key, &v))
            return v;
    }
    return 0;
}

/* CFPreferencesCopyAppValue(key, applicationID) */
static void cf_CFPreferencesCopyAppValue(ppc_cpu *c)
{
    gaddr v = R(0) ? app_value(R(0), R(1)) : 0;
    RETURN(cf_retain(v));
}

/* CFPreferencesGetAppBooleanValue(key, applicationID, *keyExistsAndHasValidFormat) */
static void cf_CFPreferencesGetAppBooleanValue(ppc_cpu *c)
{
    gaddr v = R(0) ? app_value(R(0), R(1)) : 0;
    bool valid = false, value = false;
    if (cf_type_of(v) == CFT_BOOLEAN) {
        valid = true;
        value = v == cfbool(true);
    } else if (cf_type_of(v) == CFT_NUMBER) {
        double d;
        cfnum_value(v, NULL, NULL, &d);
        valid = true;
        value = d != 0;
    } else if (cf_type_of(v) == CFT_STRING) {
        char *s = cfstr_utf8(v);
        if (s && (!strcasecmp(s, "YES") || !strcasecmp(s, "true") || !strcmp(s, "1")))
            valid = value = true;
        else if (s && (!strcasecmp(s, "NO") || !strcasecmp(s, "false") || !strcmp(s, "0")))
            valid = true;
        free(s);
    }
    if (R(2))
        wr8(R(2), valid);
    RETURN(value);
}

/* CFPreferencesGetAppIntegerValue(key, applicationID, *keyExistsAndHasValidFormat) */
static void cf_CFPreferencesGetAppIntegerValue(ppc_cpu *c)
{
    gaddr v = R(0) ? app_value(R(0), R(1)) : 0;
    bool valid = false;
    int64_t value = 0;
    if (cf_type_of(v) == CFT_NUMBER) {
        valid = true;
        cfnum_value(v, NULL, &value, NULL);
    } else if (cf_type_of(v) == CFT_STRING) {
        char *s = cfstr_utf8(v), *end = NULL;
        if (s) {
            value = strtoll(s, &end, 10);
            valid = end != s && !*end;
            if (!valid)
                value = 0;
        }
        free(s);
    }
    if (R(2))
        wr8(R(2), valid);
    RETURN((uint32_t)(int32_t)value);
}

static void domain_set(domain *d, gaddr key, gaddr value)
{
    if (!d || !key)
        return;
    if (value)
        cfdict_set(d->dict, key, value);
    else
        cfdict_remove(d->dict, key);
    domain_save(d);
}

/* CFPreferencesSetAppValue(key, value, applicationID) */
static void cf_CFPreferencesSetAppValue(ppc_cpu *c)
{
    domain_set(domain_of(R(2), cfstr_constant("kCFPreferencesAnyHost")), R(0), R(1));
    RETURN_VOID();
}

/* CFPreferencesAddSuitePreferencesToApp(applicationID, suiteID) */
static void cf_CFPreferencesAddSuitePreferencesToApp(ppc_cpu *c)
{
    char *app = app_name(R(0)), *suite = app_name(R(1));
    bool have = false;
    for (unsigned i = 0; app && suite && i < nsuites; i++)
        have |= !strcmp(suites[i].app, app) && !strcmp(suites[i].suite, suite);
    suite_link *p = app && suite && !have ? realloc(suites, sizeof *p * (nsuites + 1)) : NULL;
    if (p) {
        suites = p;
        suites[nsuites].app = app;
        suites[nsuites++].suite = suite;
    } else {
        free(app);
        free(suite);
    }
    RETURN_VOID();
}

static void cf_CFPreferencesRemoveSuitePreferencesFromApp(ppc_cpu *c)
{
    char *app = app_name(R(0)), *suite = app_name(R(1));
    for (unsigned i = 0; app && suite && i < nsuites; i++)
        if (!strcmp(suites[i].app, app) && !strcmp(suites[i].suite, suite)) {
            free(suites[i].app);
            free(suites[i].suite);
            suites[i] = suites[--nsuites];
            break;
        }
    free(app);
    free(suite);
    RETURN_VOID();
}

/* Synchronizing writes what is set (already written) and reads the file
 * again, for changes others made. */
static bool synchronize(domain *d)
{
    if (!d)
        return false;
    bool ok = domain_save(d);
    domain_load(d);
    return ok;
}

static void cf_CFPreferencesAppSynchronize(ppc_cpu *c)
{
    domain *list[40];
    unsigned n = search_list(R(0), list, 40);
    bool ok = n > 0;
    for (unsigned i = 0; i < n; i++)
        if (cfdict_count(list[i]->dict))
            ok &= synchronize(list[i]);
    RETURN(ok);
}

/* CFPreferencesCopyValue(key, applicationID, userName, hostName) */
static void cf_CFPreferencesCopyValue(ppc_cpu *c)
{
    domain *d = domain_of(R(1), R(3));
    uint32_t v = 0;
    if (d && R(0))
        cfdict_get(d->dict, R(0), &v);
    RETURN(cf_retain(v));
}

/* CFPreferencesCopyMultiple(keysToFetch, applicationID, userName, hostName) */
static void cf_CFPreferencesCopyMultiple(ppc_cpu *c)
{
    domain *d = domain_of(R(1), R(3));
    gaddr r = cfdict_new(0, false);
    if (!d)
        RETURN(r);
    if (!R(0)) {
        uint32_t *k, *v, n = cfdict_entries(d->dict, &k, &v);
        for (uint32_t i = 0; i < n; i++)
            cfdict_set(r, k[i], v[i]);
        free(k);
        free(v);
    } else {
        for (uint32_t i = 0; i < cfarray_count(R(0)); i++) {
            uint32_t v;
            if (cfdict_get(d->dict, cfarray_get(R(0), i), &v))
                cfdict_set(r, cfarray_get(R(0), i), v);
        }
    }
    RETURN(r);
}

/* CFPreferencesSetValue(key, value, applicationID, userName, hostName) */
static void cf_CFPreferencesSetValue(ppc_cpu *c)
{
    domain_set(domain_of(R(2), R(4)), R(0), R(1));
    RETURN_VOID();
}

/* CFPreferencesSetMultiple(keysToSet, keysToRemove, applicationID, userName, hostName) */
static void cf_CFPreferencesSetMultiple(ppc_cpu *c)
{
    domain *d = domain_of(R(2), R(4));
    if (!d)
        RETURN_VOID();
    uint32_t *k, *v, n = cfdict_entries(R(0), &k, &v);
    for (uint32_t i = 0; i < n; i++)
        cfdict_set(d->dict, k[i], v[i]);
    free(k);
    free(v);
    for (uint32_t i = 0; i < cfarray_count(R(1)); i++)
        cfdict_remove(d->dict, cfarray_get(R(1), i));
    domain_save(d);
    RETURN_VOID();
}

/* CFPreferencesSynchronize(applicationID, userName, hostName) */
static void cf_CFPreferencesSynchronize(ppc_cpu *c) { RETURN(synchronize(domain_of(R(0), R(2)))); }

/* CFPreferencesCopyApplicationList(userName, hostName): the domains' files. */
static void cf_CFPreferencesCopyApplicationList(ppc_cpu *c)
{
    gaddr a = cfarray_new(0, NULL, 0, true);
    char dir[PATH_MAX];
    if (fm_find_folder(fourcc("pref"), true, dir, sizeof dir) != noErr)
        RETURN(a);
    if (is_current_host(R(1))) {
        char sub[PATH_MAX];
        fm_child_path(dir, "ByHost", sub, sizeof sub);
        snprintf(dir, sizeof dir, "%s", sub);
    }
    char **paths;
    int n = fm_is_dir(dir) ? fm_list_dir(dir, &paths) : 0;
    for (int i = 0; i < n; i++) {
        char *base = strrchr(paths[i], '/');
        base = base ? base + 1 : paths[i];
        size_t len = strlen(base);
        if (len > 6 && !strcmp(base + len - 6, ".plist") && strcmp(base, ".GlobalPreferences.plist") &&
            !fm_is_dir(paths[i])) {
            base[len - 6] = 0;
            for (char *p = base; *p; p++)
                if (*p == ':')
                    *p = '/';
            gaddr s = cfstr_from_utf8(base);
            cfarray_append(a, s);
            cf_release(s);
        }
        free(paths[i]);
    }
    if (n)
        free(paths);
    RETURN(a);
}

/* CFPreferencesCopyKeyList(applicationID, userName, hostName): NULL when
 * there are none. */
static void cf_CFPreferencesCopyKeyList(ppc_cpu *c)
{
    domain *d = domain_of(R(0), R(2));
    uint32_t *k, *v, n = d ? cfdict_entries(d->dict, &k, &v) : 0;
    gaddr a = 0;
    if (d) {
        a = n ? cfarray_new(0, k, n, false) : 0;
        free(k);
        free(v);
    }
    RETURN(a);
}

/* ---------------------------------------------------------------------- */
/* CFXMLNode: its information structure in guest memory after the header */

typedef struct {
    cf_obj o;
    uint32_t code;
    gaddr string;
    int32_t version;
    uint32_t info_size;
} cf_xmlnode;

static cf_xmlnode *node_of(gaddr ref) { return cf_get_type(ref, CFT_XML_NODE); }

/* The words of an information structure that are CF objects, by type. */
static void info_objects(uint32_t code, gaddr info, gaddr *objs, int *n)
{
    *n = 0;
    switch (code) {
    case XN_ELEMENT: /* attributes, attributeOrder, isEmpty */
        objs[(*n)++] = rd32(info);
        objs[(*n)++] = rd32(info + 4);
        break;
    case XN_PI: case XN_ELEMENT_DECL: /* dataString; contentDescription */
        objs[(*n)++] = rd32(info);
        break;
    case XN_DOCUMENT: /* sourceURL, encoding */
        objs[(*n)++] = rd32(info);
        break;
    case XN_DOCTYPE: case XN_NOTATION: /* externalID: systemID, publicID */
        objs[(*n)++] = rd32(info);
        objs[(*n)++] = rd32(info + 4);
        break;
    case XN_ENTITY: /* entityType, replacementText, entityID {systemID, publicID}, notationName */
        objs[(*n)++] = rd32(info + 4);
        objs[(*n)++] = rd32(info + 8);
        objs[(*n)++] = rd32(info + 12);
        objs[(*n)++] = rd32(info + 16);
        break;
    }
}

static uint32_t info_size(uint32_t code)
{
    switch (code) {
    case XN_ELEMENT: return 12;
    case XN_PI: case XN_ELEMENT_DECL: case XN_ENTITY_REF: return 4;
    case XN_DOCUMENT: case XN_DOCTYPE: case XN_NOTATION: case XN_ATTLIST_DECL: return 8;
    case XN_ENTITY: return 20;
    default: return 0;
    }
}

static void node_finalize(cf_obj *o)
{
    cf_xmlnode *x = (cf_xmlnode *)o;
    cf_release(x->string);
    gaddr info = o->ref + CF_HEADER, objs[4];
    int n;
    info_objects(x->code, info, objs, &n);
    for (int i = 0; i < n; i++)
        cf_release(objs[i]);
    if (x->code == XN_ATTLIST_DECL) {
        uint32_t count = rd32(info);
        gaddr list = rd32(info + 4);
        for (uint32_t i = 0; list && i < count; i++)
            for (int k = 0; k < 3; k++)
                cf_release(rd32(list + 12 * i + 4 * (uint32_t)k));
    }
}

static bool node_equal(cf_obj *a, cf_obj *b)
{
    cf_xmlnode *x = (cf_xmlnode *)a, *y = (cf_xmlnode *)b;
    return x->code == y->code && cf_equal(x->string, y->string) &&
           !memcmp(gptr(a->ref + CF_HEADER), gptr(b->ref + CF_HEADER), x->info_size);
}

static void node_describe(cf_obj *o, cf_text *out)
{
    cf_xmlnode *x = (cf_xmlnode *)o;
    text_printf(out, "type = %u, string = ", x->code);
    uint32_t n;
    const uint16_t *u = cfstr_chars(x->string, &n);
    if (u)
        text_add(out, u, n);
    else
        text_add_utf8(out, "(null)");
}

static const cf_class node_class = { "CFXMLNode", node_finalize, NULL, node_equal, NULL, node_describe };

/* A node: its string retained, its information copied (objects retained). */
static gaddr node_new(gaddr alloc, uint32_t code, gaddr string, gaddr info, int32_t version)
{
    if (code < XN_DOCUMENT || code > XN_ATTLIST_DECL)
        return 0;
    uint32_t size = info_size(code), list_count = 0;
    if (code == XN_ATTLIST_DECL && info)
        list_count = rd32(info);
    cf_xmlnode *x = cf_new(CFT_XML_NODE, sizeof *x, alloc, size + 12 * list_count);
    if (!x)
        return 0;
    x->code = code;
    x->string = cf_retain(string);
    x->version = version;
    x->info_size = size;
    gaddr mine = x->o.ref + CF_HEADER;
    if (info && size)
        gmemcpy(mine, info, size);
    gaddr objs[4];
    int n;
    info_objects(code, mine, objs, &n);
    for (int i = 0; i < n; i++)
        cf_retain(objs[i]);
    if (code == XN_ATTLIST_DECL && info) {
        gaddr list = mine + size, theirs = rd32(info + 4);
        wr32(mine + 4, list_count ? list : 0);
        for (uint32_t i = 0; theirs && i < list_count; i++)
            for (int k = 0; k < 3; k++)
                wr32(list + 12 * i + 4 * (uint32_t)k, cf_retain(rd32(theirs + 12 * i + 4 * (uint32_t)k)));
    }
    return x->o.ref;
}

static void cf_CFXMLNodeGetTypeID(ppc_cpu *c) { RETURN(CFT_XML_NODE); }

/* CFXMLNodeCreate(alloc, xmlType, dataString, additionalInfoPtr, version) */
static void cf_CFXMLNodeCreate(ppc_cpu *c) { RETURN(node_new(R(0), R(1), R(2), R(3), (int32_t)R(4))); }

static void cf_CFXMLNodeCreateCopy(ppc_cpu *c)
{
    cf_xmlnode *x = node_of(R(1));
    RETURN(x ? node_new(R(0), x->code, x->string, x->info_size ? R(1) + CF_HEADER : 0, x->version) : 0);
}

static void cf_CFXMLNodeGetTypeCode(ppc_cpu *c)
{
    cf_xmlnode *x = node_of(R(0));
    RETURN(x ? x->code : 0);
}

static void cf_CFXMLNodeGetString(ppc_cpu *c)
{
    cf_xmlnode *x = node_of(R(0));
    RETURN(x ? x->string : 0);
}

static void cf_CFXMLNodeGetInfoPtr(ppc_cpu *c)
{
    cf_xmlnode *x = node_of(R(0));
    RETURN(x && x->info_size ? R(0) + CF_HEADER : 0);
}

static void cf_CFXMLNodeGetVersion(ppc_cpu *c)
{
    cf_xmlnode *x = node_of(R(0));
    RETURN(x ? (uint32_t)x->version : 0);
}

static void cf_CFXMLTreeCreateWithNode(ppc_cpu *c) { RETURN(node_of(R(1)) ? cftree_new_with_info(R(0), R(1)) : 0); }
static void cf_CFXMLTreeGetNode(ppc_cpu *c) { RETURN(cftree_info(R(0))); }

/* A node for a piece of the host tree (its information in a scratch
 * block, copied by node_new). */
static gaddr node_of_x(gaddr alloc, const xnode *x, gaddr source_url, int32_t version)
{
    gaddr info = tb_NewPtr(24, false, true), string = 0, objs[4] = { 0 };
    int nobjs = 0;
    if (!info)
        return 0;
    switch (x->type) {
    case XN_DOCUMENT:
        wr32(info, source_url);
        wr32(info + 4, kCFStringEncodingUTF8);
        break;
    case XN_ELEMENT: {
        string = cfstr_new(alloc, x->s.u, x->s.n, false);
        gaddr attrs = cfdict_new(alloc, false), order = cfarray_new(alloc, NULL, 0, false);
        for (uint32_t i = 0; i < x->nattr; i++) {
            gaddr k = cfstr_new(alloc, x->an[i].u, x->an[i].n, false), v = cfstr_new(alloc, x->av[i].u, x->av[i].n, false);
            cfdict_set(attrs, k, v);
            cfarray_append(order, k);
            cf_release(k);
            cf_release(v);
        }
        wr32(info, attrs);
        wr32(info + 4, order);
        wr8(info + 8, x->empty);
        objs[nobjs++] = attrs;
        objs[nobjs++] = order;
        break;
    }
    case XN_PI: {
        string = cfstr_new(alloc, x->s.u, x->s.n, false);
        /* The data without the space before "?>". */
        uint32_t n = x->data.n;
        while (n && xml_space(x->data.u[n - 1]))
            n--;
        gaddr d = cfstr_new(alloc, x->data.u, n, false);
        wr32(info, d);
        objs[nobjs++] = d;
        break;
    }
    case XN_DOCTYPE: {
        string = cfstr_new(alloc, x->s.u, x->s.n, false);
        gaddr sys = 0, pub = 0;
        if (x->data.n) {
            gaddr s = cfstr_new(alloc, x->data.u, x->data.n, false);
            sys = cfurl_new_string(alloc, s, 0);
            cf_release(s);
        }
        if (x->pub.n)
            pub = cfstr_new(alloc, x->pub.u, x->pub.n, false);
        wr32(info, sys);
        wr32(info + 4, pub);
        objs[nobjs++] = sys;
        objs[nobjs++] = pub;
        break;
    }
    case XN_ENTITY_REF:
        string = cfstr_new(alloc, x->s.u, x->s.n, false);
        wr32(info, 1); /* kCFXMLEntityTypeParsedInternal */
        break;
    default:
        string = cfstr_new(alloc, x->s.u, x->s.n, false);
        break;
    }
    gaddr node = node_new(alloc, (uint32_t)x->type, string, info, version);
    cf_release(string);
    for (int i = 0; i < nobjs; i++)
        cf_release(objs[i]);
    tb_DisposePtr(info);
    return node;
}

/* ---------------------------------------------------------------------- */
/* CFXMLParser */

typedef struct {
    cf_obj o;
    gaddr data, url;
    uint32_t options;
    int32_t version;
    uint32_t cb[6];  /* version, createXMLStructure, addChild, endXMLStructure, resolveExternalEntity, handleError */
    uint32_t ctx[5]; /* version, info, retain, release, copyDescription */
    gaddr document;
    int32_t status;
    gaddr error;
    uint32_t location, line;
    const uint16_t *text; /* while parsing */
    bool aborted;
} cf_parser;

static cf_parser *parser_of(gaddr ref) { return cf_get_type(ref, CFT_XML_PARSER); }

static void parser_finalize(cf_obj *o)
{
    cf_parser *p = (cf_parser *)o;
    cf_release(p->data);
    cf_release(p->url);
    cf_release(p->error);
    if (p->ctx[3])
        cf_call_info(p->ctx[3], p->ctx[1]);
}

static const cf_class parser_class = { "CFXMLParser", parser_finalize, NULL, NULL, NULL, NULL };

static gaddr parser_new(gaddr alloc, gaddr data, gaddr url, uint32_t options, int32_t version, gaddr cbs, gaddr ctx)
{
    cf_parser *p = cf_new(CFT_XML_PARSER, sizeof *p, alloc, 0);
    if (!p)
        return 0;
    p->data = cf_retain(data);
    p->url = cf_retain(url);
    p->options = options;
    p->version = version;
    p->status = kCFXMLStatusParseNotBegun;
    for (int i = 0; cbs && i < 6; i++)
        p->cb[i] = rd32(cbs + 4 * (uint32_t)i);
    for (int i = 0; ctx && i < 5; i++)
        p->ctx[i] = rd32(ctx + 4 * (uint32_t)i);
    if (p->ctx[2])
        p->ctx[1] = cf_call_info(p->ctx[2], p->ctx[1]);
    return p->o.ref;
}

static void cf_CFXMLParserGetTypeID(ppc_cpu *c) { RETURN(CFT_XML_PARSER); }

/* CFXMLParserCreate(allocator, xmlData, dataSource, parseOptions, versionOfNodes,
 *                   callBacks, context) */
static void cf_CFXMLParserCreate(ppc_cpu *c)
{
    if (cf_type_of(R(1)) != CFT_DATA || !R(5))
        RETURN(0);
    RETURN(parser_new(R(0), R(1), R(2), R(3), (int32_t)R(4), R(5), R(6)));
}

/* CFXMLParserCreateWithDataFromURL(allocator, dataSource, parseOptions, versionOfNodes,
 *                                  callBacks, context) */
static void cf_CFXMLParserCreateWithDataFromURL(ppc_cpu *c)
{
    uint8_t *b;
    size_t n;
    if (!R(4) || !cfurl_read_file(R(1), &b, &n))
        RETURN(0);
    gaddr data = cfdata_new(R(0), b, (uint32_t)n, false);
    free(b);
    gaddr p = parser_new(R(0), data, R(1), R(2), (int32_t)R(3), R(4), R(5));
    cf_release(data);
    RETURN(p);
}

static void cf_CFXMLParserGetContext(ppc_cpu *c)
{
    cf_parser *p = parser_of(R(0));
    for (int i = 0; p && R(1) && i < 5; i++)
        wr32(R(1) + 4 * (uint32_t)i, p->ctx[i]);
    RETURN_VOID();
}

static void cf_CFXMLParserGetCallBacks(ppc_cpu *c)
{
    cf_parser *p = parser_of(R(0));
    for (int i = 0; p && R(1) && i < 6; i++)
        wr32(R(1) + 4 * (uint32_t)i, p->cb[i]);
    RETURN_VOID();
}

static void cf_CFXMLParserGetSourceURL(ppc_cpu *c)
{
    cf_parser *p = parser_of(R(0));
    RETURN(p ? p->url : 0);
}

static void cf_CFXMLParserGetLocation(ppc_cpu *c)
{
    cf_parser *p = parser_of(R(0));
    RETURN(p ? p->location : 0);
}

static void cf_CFXMLParserGetLineNumber(ppc_cpu *c)
{
    cf_parser *p = parser_of(R(0));
    RETURN(p ? p->line : 0);
}

static void cf_CFXMLParserGetDocument(ppc_cpu *c)
{
    cf_parser *p = parser_of(R(0));
    RETURN(p ? p->document : 0);
}

static void cf_CFXMLParserGetStatusCode(ppc_cpu *c)
{
    cf_parser *p = parser_of(R(0));
    RETURN(p ? (uint32_t)p->status : 0);
}

static void cf_CFXMLParserCopyErrorDescription(ppc_cpu *c)
{
    cf_parser *p = parser_of(R(0));
    RETURN(p ? cf_retain(p->error) : 0);
}

/* CFXMLParserAbort(parser, errorCode, errorDescription) */
static void cf_CFXMLParserAbort(ppc_cpu *c)
{
    cf_parser *p = parser_of(R(0));
    if (p && (int32_t)R(1) > 0) {
        p->aborted = true;
        p->status = (int32_t)R(1);
        cf_release(p->error);
        p->error = cf_retain(R(2));
    }
    RETURN_VOID();
}

static void set_position(cf_parser *p, uint32_t pos)
{
    p->location = pos;
    p->line = 1;
    for (uint32_t i = 0; p->text && i < pos; i++)
        p->line += p->text[i] == '\n';
}

/* What a walk calls for each node: create, addChild and end. */
typedef struct walker walker;
struct walker {
    uint32_t (*create)(walker *w, gaddr node);
    void (*add_child)(walker *w, uint32_t parent, uint32_t child);
    void (*end)(walker *w, uint32_t structure);
    bool (*stop)(walker *w);
    cf_parser *parser;
    gaddr alloc, url;
    int32_t version;
    uint32_t options;
};

static bool skipped(const walker *w, const xnode *x)
{
    if (w->options & kCFXMLParserSkipMetaData)
        return x->type == XN_COMMENT || x->type == XN_PI || x->type == XN_DOCTYPE;
    return false;
}

static void walk(walker *w, const xnode *x, uint32_t parent)
{
    for (uint32_t i = 0; i < x->nkids && !w->stop(w); i++) {
        const xnode *k = x->kids[i];
        if (skipped(w, k))
            continue;
        if (w->parser)
            set_position(w->parser, k->pos);
        gaddr node = node_of_x(w->alloc, k, w->url, w->version);
        uint32_t made = w->create(w, node);
        cf_release(node);
        if (!made || w->stop(w))
            continue;
        if (parent)
            w->add_child(w, parent, made);
        if (k->type == XN_ELEMENT && k->nkids)
            walk(w, k, made);
        if (!w->stop(w) && (k->type != XN_ELEMENT || k->closed))
            w->end(w, made);
    }
}

/* The program's callbacks. */
static uint32_t guest_create(walker *w, gaddr node)
{
    cf_parser *p = w->parser;
    uint32_t args[3] = { p->o.ref, node, p->ctx[1] };
    return p->cb[1] ? cf_call(p->cb[1], args, 3) : 0;
}

static void guest_add_child(walker *w, uint32_t parent, uint32_t child)
{
    cf_parser *p = w->parser;
    uint32_t args[4] = { p->o.ref, parent, child, p->ctx[1] };
    if (p->cb[2])
        cf_call(p->cb[2], args, 4);
}

static void guest_end(walker *w, uint32_t structure)
{
    cf_parser *p = w->parser;
    uint32_t args[3] = { p->o.ref, structure, p->ctx[1] };
    if (p->cb[3])
        cf_call(p->cb[3], args, 3);
}

static bool guest_stop(walker *w) { return w->parser->aborted; }

static const char *const status_text[] = {
    "", "Unexpected end of file", "Unknown encoding", "Encoding conversion failure",
    "Malformed processing instruction", "Malformed DTD", "Malformed name", "Malformed CDATA section",
    "Malformed close tag", "Malformed start tag", "Malformed document", "Document has no element",
    "Malformed comment", "Malformed character reference", "Malformed parsed character data", "No data",
};

/* CFXMLParserParse(parser) */
static void cf_CFXMLParserParse(ppc_cpu *c)
{
    cf_parser *p = parser_of(R(0));
    if (!p || p->status != kCFXMLStatusParseNotBegun)
        RETURN(false);
    uint32_t n;
    gaddr bytes = cfdata_bytes(p->data, &n);
    uint8_t *b = malloc(n ? n : 1);
    uint16_t *u = NULL;
    uint32_t nu = 0;
    if (b)
        gget(b, bytes, n);
    p->status = kCFXMLStatusParseInProgress;
    bool decoded = b && text_of_bytes(b, n, &u, &nu);
    free(b);
    int status = decoded ? 0 : kCFXMLErrorEncodingConversionFailure;
    uint32_t where = 0;
    xnode *doc = decoded ? xml_parse(u, nu, p->options, &status, &where) : NULL;
    p->text = u;
    walker w = { guest_create, guest_add_child, guest_end, guest_stop, p, p->o.alloc, p->url, p->version,
                 p->options };
    if (doc) {
        gaddr node = node_of_x(p->o.alloc, doc, p->url, p->version);
        p->document = w.create(&w, node);
        cf_release(node);
        if (!p->aborted)
            walk(&w, doc, p->document);
        if (!p->aborted && !status && p->document)
            w.end(&w, p->document);
    }
    if (!p->aborted && status) {
        set_position(p, where);
        p->status = status;
        p->error = cfstr_from_utf8(status < (int)(sizeof status_text / sizeof status_text[0]) ? status_text[status] : "XML error");
        if (p->cb[5]) {
            uint32_t args[3] = { p->o.ref, (uint32_t)status, p->ctx[1] };
            cf_call(p->cb[5], args, 3);
        }
    } else if (!p->aborted) {
        p->status = kCFXMLStatusParseSuccessful;
    }
    p->text = NULL;
    xnode_free(doc);
    free(u);
    RETURN(p->status == kCFXMLStatusParseSuccessful);
}

/* Trees: a CFXMLTree for each structure. */
static uint32_t tree_create(walker *w, gaddr node) { return cftree_new_with_info(w->alloc, node); }
static void tree_add_child(walker *w, uint32_t parent, uint32_t child)
{
    cftree_append(parent, child);
    cf_release(child);
}
static void tree_end(walker *w, uint32_t structure) {}
static bool tree_stop(walker *w) { return false; }

static gaddr tree_from_bytes(gaddr alloc, const uint8_t *b, size_t n, gaddr url, uint32_t options, int32_t version)
{
    uint16_t *u;
    uint32_t nu;
    if (!text_of_bytes(b, n, &u, &nu))
        return 0;
    int status;
    xnode *doc = xml_parse(u, nu, options, &status, NULL);
    free(u);
    gaddr tree = 0;
    if (doc && !status) {
        walker w = { tree_create, tree_add_child, tree_end, tree_stop, NULL, alloc, url, version, options };
        gaddr node = node_of_x(alloc, doc, url, version);
        tree = cftree_new_with_info(alloc, node);
        cf_release(node);
        walk(&w, doc, tree);
    }
    xnode_free(doc);
    return tree;
}

/* CFXMLTreeCreateFromData(allocator, xmlData, dataSource, parseOptions, versionOfNodes) */
static void cf_CFXMLTreeCreateFromData(ppc_cpu *c)
{
    uint32_t n;
    gaddr bytes = cfdata_bytes(R(1), &n);
    if (cf_type_of(R(1)) != CFT_DATA)
        RETURN(0);
    uint8_t *b = malloc(n ? n : 1);
    if (!b)
        RETURN(0);
    gget(b, bytes, n);
    gaddr t = tree_from_bytes(R(0), b, n, R(2), R(3), (int32_t)R(4));
    free(b);
    RETURN(t);
}

/* CFXMLTreeCreateWithDataFromURL(allocator, dataSource, parseOptions, versionOfNodes) */
static void cf_CFXMLTreeCreateWithDataFromURL(ppc_cpu *c)
{
    uint8_t *b;
    size_t n;
    if (!cfurl_read_file(R(1), &b, &n))
        RETURN(0);
    gaddr t = tree_from_bytes(R(0), b, n, R(1), R(2), (int32_t)R(3));
    free(b);
    RETURN(t);
}

static void add_string(cf_text *t, gaddr s, bool escape)
{
    if (escape) {
        add_escaped_text(t, s);
        return;
    }
    uint32_t n;
    const uint16_t *u = cfstr_chars(s, &n);
    if (u)
        text_add(t, u, n);
}

static void xml_of_tree(cf_text *t, gaddr tree, int depth)
{
    cf_xmlnode *x = node_of(cftree_info(tree));
    gaddr *kids;
    uint32_t nk = cftree_children(tree, &kids);
    if (!x || depth > 512) {
        free(kids);
        return;
    }
    gaddr info = x->o.ref + CF_HEADER;
    switch (x->code) {
    case XN_DOCUMENT:
        for (uint32_t i = 0; i < nk; i++)
            xml_of_tree(t, kids[i], depth + 1);
        break;
    case XN_ELEMENT: {
        text_add_unit(t, '<');
        add_string(t, x->string, false);
        gaddr attrs = rd32(info), order = rd32(info + 4);
        for (uint32_t i = 0; i < cfarray_count(order); i++) {
            uint32_t v = 0;
            gaddr k = cfarray_get(order, i);
            cfdict_get(attrs, k, &v);
            text_add_unit(t, ' ');
            add_string(t, k, false);
            text_add_utf8(t, "=\"");
            uint32_t n;
            const uint16_t *u = cfstr_chars(v, &n);
            for (uint32_t j = 0; u && j < n; j++) {
                if (u[j] == '"')
                    text_add_utf8(t, "&quot;");
                else if (u[j] == '&')
                    text_add_utf8(t, "&amp;");
                else if (u[j] == '<')
                    text_add_utf8(t, "&lt;");
                else
                    text_add_unit(t, u[j]);
            }
            text_add_unit(t, '"');
        }
        if (!nk && rd8(info + 8)) {
            text_add_utf8(t, "/>");
            break;
        }
        text_add_unit(t, '>');
        for (uint32_t i = 0; i < nk; i++)
            xml_of_tree(t, kids[i], depth + 1);
        text_add_utf8(t, "</");
        add_string(t, x->string, false);
        text_add_unit(t, '>');
        break;
    }
    case XN_PI:
        text_add_utf8(t, "<?");
        add_string(t, x->string, false);
        if (rd32(info)) {
            text_add_unit(t, ' ');
            add_string(t, rd32(info), false);
        }
        text_add_utf8(t, "?>");
        break;
    case XN_COMMENT:
        text_add_utf8(t, "<!--");
        add_string(t, x->string, false);
        text_add_utf8(t, "-->");
        break;
    case XN_TEXT:
        add_string(t, x->string, true);
        break;
    case XN_WHITESPACE:
        add_string(t, x->string, false);
        break;
    case XN_CDATA:
        text_add_utf8(t, "<![CDATA[");
        add_string(t, x->string, false);
        text_add_utf8(t, "]]>");
        break;
    case XN_ENTITY_REF:
        text_add_unit(t, '&');
        add_string(t, x->string, false);
        text_add_unit(t, ';');
        break;
    case XN_DOCTYPE: {
        text_add_utf8(t, "<!DOCTYPE ");
        add_string(t, x->string, false);
        gaddr sys = rd32(info), pub = rd32(info + 4);
        if (pub) {
            text_add_utf8(t, " PUBLIC \"");
            add_string(t, pub, false);
            text_add_unit(t, '"');
        } else if (sys) {
            text_add_utf8(t, " SYSTEM");
        }
        if (sys) {
            text_add_utf8(t, " \"");
            add_string(t, cfurl_string(sys), false);
            text_add_unit(t, '"');
        }
        text_add_unit(t, '>');
        break;
    }
    default:
        break;
    }
    free(kids);
}

/* CFXMLTreeCreateXMLData(allocator, xmlTree) */
static void cf_CFXMLTreeCreateXMLData(ppc_cpu *c)
{
    if (!node_of(cftree_info(R(1))))
        RETURN(0);
    cf_text t = { 0 };
    xml_of_tree(&t, R(1), 0);
    uint8_t *b;
    size_t n;
    uint32_t used;
    cf_encode(kCFStringEncodingUTF8, t.u, t.n, '?', &b, &n, &used);
    text_free(&t);
    gaddr d = cfdata_new(R(0), b, (uint32_t)n, false);
    free(b);
    RETURN(d);
}

/* ---------------------------------------------------------------------- */

void cfplist_reset(void)
{
    for (unsigned i = 0; i < ndomains; i++)
        free(domains[i].app);
    free(domains);
    domains = NULL;
    ndomains = domains_cap = 0;
    for (unsigned i = 0; i < nsuites; i++) {
        free(suites[i].app);
        free(suites[i].suite);
    }
    free(suites);
    suites = NULL;
    nsuites = 0;
    cf_register_class(CFT_XML_NODE, &node_class);
    cf_register_class(CFT_XML_PARSER, &parser_class);
}

const ppc_export cfplist_exports[] = {
    { "CFPropertyListCreateFromXMLData", cf_CFPropertyListCreateFromXMLData },
    { "CFPropertyListCreateXMLData", cf_CFPropertyListCreateXMLData },
    { "CFPropertyListCreateDeepCopy", cf_CFPropertyListCreateDeepCopy },
    { "CFPreferencesCopyAppValue", cf_CFPreferencesCopyAppValue },
    { "CFPreferencesGetAppBooleanValue", cf_CFPreferencesGetAppBooleanValue },
    { "CFPreferencesGetAppIntegerValue", cf_CFPreferencesGetAppIntegerValue },
    { "CFPreferencesSetAppValue", cf_CFPreferencesSetAppValue },
    { "CFPreferencesAddSuitePreferencesToApp", cf_CFPreferencesAddSuitePreferencesToApp },
    { "CFPreferencesRemoveSuitePreferencesFromApp", cf_CFPreferencesRemoveSuitePreferencesFromApp },
    { "CFPreferencesAppSynchronize", cf_CFPreferencesAppSynchronize },
    { "CFPreferencesCopyValue", cf_CFPreferencesCopyValue },
    { "CFPreferencesCopyMultiple", cf_CFPreferencesCopyMultiple },
    { "CFPreferencesSetValue", cf_CFPreferencesSetValue },
    { "CFPreferencesSetMultiple", cf_CFPreferencesSetMultiple },
    { "CFPreferencesSynchronize", cf_CFPreferencesSynchronize },
    { "CFPreferencesCopyApplicationList", cf_CFPreferencesCopyApplicationList },
    { "CFPreferencesCopyKeyList", cf_CFPreferencesCopyKeyList },
    { "CFXMLNodeGetTypeID", cf_CFXMLNodeGetTypeID },
    { "CFXMLNodeCreate", cf_CFXMLNodeCreate },
    { "CFXMLNodeCreateCopy", cf_CFXMLNodeCreateCopy },
    { "CFXMLNodeGetTypeCode", cf_CFXMLNodeGetTypeCode },
    { "CFXMLNodeGetString", cf_CFXMLNodeGetString },
    { "CFXMLNodeGetInfoPtr", cf_CFXMLNodeGetInfoPtr },
    { "CFXMLNodeGetVersion", cf_CFXMLNodeGetVersion },
    { "CFXMLTreeCreateWithNode", cf_CFXMLTreeCreateWithNode },
    { "CFXMLTreeGetNode", cf_CFXMLTreeGetNode },
    { "CFXMLParserGetTypeID", cf_CFXMLParserGetTypeID },
    { "CFXMLParserCreate", cf_CFXMLParserCreate },
    { "CFXMLParserCreateWithDataFromURL", cf_CFXMLParserCreateWithDataFromURL },
    { "CFXMLParserGetContext", cf_CFXMLParserGetContext },
    { "CFXMLParserGetCallBacks", cf_CFXMLParserGetCallBacks },
    { "CFXMLParserGetSourceURL", cf_CFXMLParserGetSourceURL },
    { "CFXMLParserGetLocation", cf_CFXMLParserGetLocation },
    { "CFXMLParserGetLineNumber", cf_CFXMLParserGetLineNumber },
    { "CFXMLParserGetDocument", cf_CFXMLParserGetDocument },
    { "CFXMLParserGetStatusCode", cf_CFXMLParserGetStatusCode },
    { "CFXMLParserCopyErrorDescription", cf_CFXMLParserCopyErrorDescription },
    { "CFXMLParserAbort", cf_CFXMLParserAbort },
    { "CFXMLParserParse", cf_CFXMLParserParse },
    { "CFXMLTreeCreateFromData", cf_CFXMLTreeCreateFromData },
    { "CFXMLTreeCreateWithDataFromURL", cf_CFXMLTreeCreateWithDataFromURL },
    { "CFXMLTreeCreateXMLData", cf_CFXMLTreeCreateXMLData },
};
const unsigned cfplist_export_count = sizeof cfplist_exports / sizeof cfplist_exports[0];
