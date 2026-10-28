/*
 * HTMLRenderingLib (and CarbonLib's HTMLRendering): a renderer for HTML 3.2
 * pages that draws through QuickDraw into the program's port.
 *
 * Why not the host's WebKit: HRDraw has to paint synchronously into any
 * port the program gives (a window, a GWorld for printing), with the
 * program's fonts, in a QuickDraw clip; WebKit renders to a view of its own
 * and asynchronously, needs a host at all (the tests and headless runs
 * have none) and has no Linux counterpart in this code base.  This
 * renderer is plain C over the Toolbox's own text and drawing calls, so it
 * works wherever Blue Box does and measures text as the program's port
 * will draw it.
 *
 * The page is parsed into a tree (tags and entities as HTML 3.2 has them;
 * end tags implied where 3.2 let them be left out), laid out for the
 * rendering rectangle's width into a list of things to draw (text, rules,
 * bullets, frames, images), and drawn offset by the scroll position.
 * Supported: BODY colours, H1-H6, P, BR, HR, CENTER, DIV and ALIGN,
 * BLOCKQUOTE, ADDRESS, PRE and the other preformatted tags, UL, OL, DL, MENU
 * and DIR, the font styles (B, I, U, TT, STRIKE, BIG, SMALL, SUB, SUP and
 * the phrase tags), FONT and BASEFONT, A (links, named anchors), IMG (through
 * the graphics importers, else its ALT text in a frame), TABLE with
 * TR, TD, TH (COLSPAN, ROWSPAN, WIDTH, BGCOLOR, BORDER, CELLPADDING,
 * CELLSPACING), and framed stand-ins for form controls.  Links are
 * followed by clicking; a vertical scroll bar is drawn when asked for or
 * (automatic) needed.  Frames, applets and style sheets are not 3.2.
 *
 * Links and images name files: file: URLs and relative ones are looked up
 * on disk; any other the program's URL-to-FSSpec routine is asked for.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "carbon_events.h"
#include "carbon_svc.h"
#include "filemgr_internal.h"
#include "ppc_bridge.h"
#include "tb_internal.h"
#include "os9rest.h"
#include "os9left.h"

#undef A
#undef RET
#undef DONE

enum {
    kHRInvalidReferenceErr = -30,
    kHRNoImageErr = -43,
    HR_REF = 160,
};
enum { eHRScrollbarOn = 0, eHRScrollbarOff = 1, eHRScrollbarAuto = 2 };
enum { kHRLookingForHTMLSource = 1, kHRLookingForImage = 2 };

/* ---------------------------------------------------------------------- */
/* Strings and colours */

static char *dupn(const char *s, size_t n)
{
    char *p = malloc(n + 1);
    if (p) {
        memcpy(p, s, n);
        p[n] = 0;
    }
    return p;
}

static char *dups(const char *s) { return dupn(s, strlen(s)); }

typedef struct { uint8_t r, g, b; } rgb;

static const struct { const char *name; uint32_t v; } color_names[] = {
    { "black", 0x000000 }, { "silver", 0xC0C0C0 }, { "gray", 0x808080 }, { "white", 0xFFFFFF },
    { "maroon", 0x800000 }, { "red", 0xFF0000 }, { "purple", 0x800080 }, { "fuchsia", 0xFF00FF },
    { "green", 0x008000 }, { "lime", 0x00FF00 }, { "olive", 0x808000 }, { "yellow", 0xFFFF00 },
    { "navy", 0x000080 }, { "blue", 0x0000FF }, { "teal", 0x008080 }, { "aqua", 0x00FFFF },
    { "orange", 0xFFA500 }, { "brown", 0xA52A2A }, { "pink", 0xFFC0CB }, { "gold", 0xFFD700 },
    { "lightgray", 0xD3D3D3 }, { "darkgray", 0xA9A9A9 }, { "cyan", 0x00FFFF }, { "magenta", 0xFF00FF },
};

static bool parse_color(const char *s, rgb *out)
{
    if (!s)
        return false;
    while (*s == ' ')
        s++;
    uint32_t v = 0;
    if (*s == '#' || isxdigit((unsigned char)*s)) {
        const char *h = *s == '#' ? s + 1 : s;
        char *end;
        bool named = false;
        for (unsigned i = 0; i < sizeof color_names / sizeof color_names[0]; i++)
            if (!strcasecmp(h, color_names[i].name))
                named = true;
        if (!named || *s == '#') {
            v = (uint32_t)strtoul(h, &end, 16);
            out->r = (uint8_t)(v >> 16), out->g = (uint8_t)(v >> 8), out->b = (uint8_t)v;
            return true;
        }
    }
    for (unsigned i = 0; i < sizeof color_names / sizeof color_names[0]; i++)
        if (!strcasecmp(s, color_names[i].name)) {
            v = color_names[i].v;
            out->r = (uint8_t)(v >> 16), out->g = (uint8_t)(v >> 8), out->b = (uint8_t)v;
            return true;
        }
    return false;
}

/* ---------------------------------------------------------------------- */
/* The parse tree */

enum {
    T_TEXT = 0, T_OTHER, T_HTML, T_HEAD, T_BODY, T_TITLE, T_BASE, T_META, T_LINK, T_SCRIPT, T_STYLE,
    T_H1, T_H2, T_H3, T_H4, T_H5, T_H6, T_P, T_BR, T_HR, T_DIV, T_CENTER, T_BLOCKQUOTE, T_ADDRESS, T_PRE,
    T_UL, T_OL, T_DL, T_MENU, T_DIR, T_LI, T_DT, T_DD,
    T_B, T_I, T_U, T_TT, T_STRIKE, T_BIG, T_SMALL, T_SUB, T_SUP, T_EM, T_STRONG, T_CITE, T_CODE, T_SAMP,
    T_KBD, T_VAR, T_DFN, T_FONT, T_BASEFONT, T_A, T_IMG, T_NOBR,
    T_TABLE, T_CAPTION, T_TR, T_TD, T_TH, T_FORM, T_INPUT, T_SELECT, T_OPTION, T_TEXTAREA, T_MAP, T_AREA,
    T_APPLET, T_PARAM, T_ISINDEX, T_LISTING, T_XMP, T_PLAINTEXT,
};

static const struct { const char *name; int tag; bool is_void; } tag_names[] = {
    { "html", T_HTML, 0 }, { "head", T_HEAD, 0 }, { "body", T_BODY, 0 }, { "title", T_TITLE, 0 },
    { "base", T_BASE, 1 }, { "meta", T_META, 1 }, { "link", T_LINK, 1 }, { "script", T_SCRIPT, 0 },
    { "style", T_STYLE, 0 }, { "h1", T_H1, 0 }, { "h2", T_H2, 0 }, { "h3", T_H3, 0 }, { "h4", T_H4, 0 },
    { "h5", T_H5, 0 }, { "h6", T_H6, 0 }, { "p", T_P, 0 }, { "br", T_BR, 1 }, { "hr", T_HR, 1 },
    { "div", T_DIV, 0 }, { "center", T_CENTER, 0 }, { "blockquote", T_BLOCKQUOTE, 0 },
    { "address", T_ADDRESS, 0 }, { "pre", T_PRE, 0 }, { "ul", T_UL, 0 }, { "ol", T_OL, 0 }, { "dl", T_DL, 0 },
    { "menu", T_MENU, 0 }, { "dir", T_DIR, 0 }, { "li", T_LI, 0 }, { "dt", T_DT, 0 }, { "dd", T_DD, 0 },
    { "b", T_B, 0 }, { "i", T_I, 0 }, { "u", T_U, 0 }, { "tt", T_TT, 0 }, { "strike", T_STRIKE, 0 },
    { "s", T_STRIKE, 0 }, { "big", T_BIG, 0 }, { "small", T_SMALL, 0 }, { "sub", T_SUB, 0 },
    { "sup", T_SUP, 0 }, { "em", T_EM, 0 }, { "strong", T_STRONG, 0 }, { "cite", T_CITE, 0 },
    { "code", T_CODE, 0 }, { "samp", T_SAMP, 0 }, { "kbd", T_KBD, 0 }, { "var", T_VAR, 0 },
    { "dfn", T_DFN, 0 }, { "font", T_FONT, 0 }, { "basefont", T_BASEFONT, 1 }, { "a", T_A, 0 },
    { "img", T_IMG, 1 }, { "nobr", T_NOBR, 0 }, { "table", T_TABLE, 0 }, { "caption", T_CAPTION, 0 },
    { "tr", T_TR, 0 }, { "td", T_TD, 0 }, { "th", T_TH, 0 }, { "form", T_FORM, 0 }, { "input", T_INPUT, 1 },
    { "select", T_SELECT, 0 }, { "option", T_OPTION, 0 }, { "textarea", T_TEXTAREA, 0 }, { "map", T_MAP, 0 },
    { "area", T_AREA, 1 }, { "applet", T_APPLET, 0 }, { "param", T_PARAM, 1 }, { "isindex", T_ISINDEX, 1 },
    { "listing", T_LISTING, 0 }, { "xmp", T_XMP, 0 }, { "plaintext", T_PLAINTEXT, 0 },
};

typedef struct attr {
    char *name, *val;
    struct attr *next;
} attr;

typedef struct node {
    int tag;
    char *text; /* T_TEXT */
    attr *attrs;
    struct node *parent, *first, *last, *next;
} node;

static const char *attr_of(const node *n, const char *name)
{
    for (const attr *a = n->attrs; a; a = a->next)
        if (!strcasecmp(a->name, name))
            return a->val ? a->val : "";
    return NULL;
}

static int attr_int(const node *n, const char *name, int def)
{
    const char *v = attr_of(n, name);
    return v && *v ? atoi(v) : def;
}

static node *node_new(int tag, node *parent)
{
    node *n = calloc(1, sizeof *n);
    if (!n)
        return NULL;
    n->tag = tag;
    n->parent = parent;
    if (parent) {
        if (parent->last)
            parent->last->next = n;
        else
            parent->first = n;
        parent->last = n;
    }
    return n;
}

static void node_free(node *n)
{
    for (node *c = n->first, *next; c; c = next) {
        next = c->next;
        node_free(c);
    }
    for (attr *a = n->attrs, *next; a; a = next) {
        next = a->next;
        free(a->name);
        free(a->val);
        free(a);
    }
    free(n->text);
    free(n);
}

/* The entities of HTML 3.2 and ISO Latin-1, as MacRoman bytes for what
 * has one. */
static const struct { const char *name; uint8_t mac; } entities[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "nbsp", 0xCA }, { "copy", 0xA9 },
    { "reg", 0xA8 }, { "iexcl", 0xC1 }, { "cent", 0xA2 }, { "pound", 0xA3 }, { "yen", 0xB4 },
    { "sect", 0xA4 }, { "uml", 0xAC }, { "laquo", 0xC7 }, { "raquo", 0xC8 }, { "deg", 0xA1 },
    { "plusmn", 0xB1 }, { "micro", 0xB5 }, { "para", 0xA6 }, { "middot", 0xE1 }, { "iquest", 0xC0 },
    { "szlig", 0xA7 }, { "agrave", 0x88 }, { "aacute", 0x87 }, { "acirc", 0x89 }, { "atilde", 0x8B },
    { "auml", 0x8A }, { "aring", 0x8C }, { "aelig", 0xBE }, { "ccedil", 0x8D }, { "egrave", 0x8F },
    { "eacute", 0x8E }, { "ecirc", 0x90 }, { "euml", 0x91 }, { "igrave", 0x93 }, { "iacute", 0x92 },
    { "icirc", 0x94 }, { "iuml", 0x95 }, { "ntilde", 0x96 }, { "ograve", 0x98 }, { "oacute", 0x97 },
    { "ocirc", 0x99 }, { "otilde", 0x9B }, { "ouml", 0x9A }, { "oslash", 0xBF }, { "ugrave", 0x9D },
    { "uacute", 0x9C }, { "ucirc", 0x9E }, { "uuml", 0x9F }, { "yuml", 0xD8 }, { "Agrave", 0xCB },
    { "Aacute", 0xE7 }, { "Acirc", 0xE5 }, { "Atilde", 0xCC }, { "Auml", 0x80 }, { "Aring", 0x81 },
    { "AElig", 0xAE }, { "Ccedil", 0x82 }, { "Egrave", 0xE9 }, { "Eacute", 0x83 }, { "Ecirc", 0xE6 },
    { "Euml", 0xE8 }, { "Igrave", 0xED }, { "Iacute", 0xEA }, { "Icirc", 0xEB }, { "Iuml", 0xEC },
    { "Ntilde", 0x84 }, { "Ograve", 0xF1 }, { "Oacute", 0xEE }, { "Ocirc", 0xEF }, { "Otilde", 0xCD },
    { "Ouml", 0x85 }, { "Oslash", 0xAF }, { "Ugrave", 0xF4 }, { "Uacute", 0xF2 }, { "Ucirc", 0xF3 },
    { "Uuml", 0x86 },
};

/* MacRoman for a Unicode code point below 256 (ISO Latin-1 as HTML 3.2 numbered it). */
static uint8_t latin1_to_mac(unsigned c)
{
    static const uint8_t hi[96] = {
        0xCA, 0xC1, 0xA2, 0xA3, 0xDB, 0xB4, '|', 0xA4, 0xAC, 0xA9, 0xBB, 0xC7, 0xC2, '-', 0xA8, 0xF8,
        0xA1, 0xB1, '2', '3', 0xAB, 0xB5, 0xA6, 0xE1, 0xFC, '1', 0xBC, 0xC8, '?', '?', '?', 0xC0,
        0xCB, 0xE7, 0xE5, 0xCC, 0x80, 0x81, 0xAE, 0x82, 0xE9, 0x83, 0xE6, 0xE8, 0xED, 0xEA, 0xEB, 0xEC,
        '?', 0x84, 0xF1, 0xEE, 0xEF, 0xCD, 0x85, 'x', 0xAF, 0xF4, 0xF2, 0xF3, 0x86, '?', '?', 0xA7,
        0x88, 0x87, 0x89, 0x8B, 0x8A, 0x8C, 0xBE, 0x8D, 0x8F, 0x8E, 0x90, 0x91, 0x93, 0x92, 0x94, 0x95,
        '?', 0x96, 0x98, 0x97, 0x99, 0x9B, 0x9A, 0xD6, 0xBF, 0x9D, 0x9C, 0x9E, 0x9F, '?', '?', 0xD8,
    };
    if (c < 0xA0)
        return c < 128 ? (uint8_t)c : (uint8_t)'?';
    return c < 256 ? hi[c - 0xA0] : (uint8_t)'?';
}

/* Text with its entities replaced (and, unless keep, no change to blanks). */
static char *decode(const char *s, size_t n)
{
    char *out = malloc(n + 1);
    size_t o = 0;
    if (!out)
        return NULL;
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '&') {
            out[o++] = s[i];
            continue;
        }
        size_t j = i + 1;
        if (j < n && s[j] == '#') {
            unsigned v = 0;
            j++;
            if (j < n && (s[j] == 'x' || s[j] == 'X')) {
                j++;
                while (j < n && isxdigit((unsigned char)s[j]))
                    v = v * 16 + (unsigned)(isdigit((unsigned char)s[j]) ? s[j] - '0' : (tolower(s[j]) - 'a' + 10)), j++;
            } else {
                while (j < n && isdigit((unsigned char)s[j]))
                    v = v * 10 + (unsigned)(s[j] - '0'), j++;
            }
            if (j > i + 2) {
                out[o++] = (char)latin1_to_mac(v);
                i = j < n && s[j] == ';' ? j : j - 1;
                continue;
            }
        } else {
            size_t k = j;
            while (k < n && isalnum((unsigned char)s[k]))
                k++;
            for (unsigned e = 0; k > j && e < sizeof entities / sizeof entities[0]; e++)
                if (strlen(entities[e].name) == k - j && !strncmp(entities[e].name, s + j, k - j)) {
                    out[o++] = (char)entities[e].mac;
                    i = k < n && s[k] == ';' ? k : k - 1;
                    goto next;
                }
        }
        out[o++] = '&';
    next:;
    }
    out[o] = 0;
    return out;
}

static bool is_block_tag(int t)
{
    switch (t) {
    case T_H1: case T_H2: case T_H3: case T_H4: case T_H5: case T_H6: case T_P: case T_DIV: case T_CENTER:
    case T_BLOCKQUOTE: case T_ADDRESS: case T_PRE: case T_UL: case T_OL: case T_DL: case T_MENU: case T_DIR:
    case T_TABLE: case T_FORM: case T_HR: case T_LISTING: case T_XMP:
        return true;
    }
    return false;
}

/* Does an open element of tag `open` end when `tag` starts? */
static bool implied_end(int open, int tag)
{
    switch (open) {
    case T_P: return is_block_tag(tag) || tag == T_LI || tag == T_DT || tag == T_DD || tag == T_TR || tag == T_TD || tag == T_TH;
    case T_LI: return tag == T_LI;
    case T_DT: case T_DD: return tag == T_DT || tag == T_DD;
    case T_TR: return tag == T_TR;
    case T_TD: case T_TH: return tag == T_TD || tag == T_TH || tag == T_TR;
    case T_OPTION: return tag == T_OPTION;
    case T_HEAD: return tag == T_BODY;
    }
    return false;
}

static node *parse_html(const char *s, size_t n)
{
    node *root = node_new(T_OTHER, NULL), *cur = root;
    if (!root)
        return NULL;
    size_t i = 0;
    while (i < n) {
        if (s[i] != '<' || i + 1 >= n || !(isalpha((unsigned char)s[i + 1]) || s[i + 1] == '/' || s[i + 1] == '!')) {
            size_t j = i + 1;
            while (j < n && !(s[j] == '<' && j + 1 < n && (isalpha((unsigned char)s[j + 1]) || s[j + 1] == '/' || s[j + 1] == '!')))
                j++;
            char *t = decode(s + i, j - i);
            if (t && cur->tag != T_SCRIPT && cur->tag != T_STYLE) {
                node *tn = node_new(T_TEXT, cur);
                if (tn)
                    tn->text = t;
                else
                    free(t);
            } else {
                free(t);
            }
            i = j;
            continue;
        }
        if (!strncmp(s + i, "<!--", 4)) {
            const char *e = strstr(s + i + 4, "-->");
            i = e && (size_t)(e - s) < n ? (size_t)(e - s) + 3 : n;
            continue;
        }
        if (s[i + 1] == '!') {
            while (i < n && s[i] != '>')
                i++;
            i++;
            continue;
        }
        bool closing = s[i + 1] == '/';
        size_t j = i + 1 + closing;
        size_t ns = j;
        while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '-'))
            j++;
        char name[24] = "";
        snprintf(name, sizeof name, "%.*s", (int)(j - ns > 23 ? 23 : j - ns), s + ns);
        int tag = T_OTHER;
        bool is_void = false;
        for (unsigned k = 0; k < sizeof tag_names / sizeof tag_names[0]; k++)
            if (!strcasecmp(name, tag_names[k].name)) {
                tag = tag_names[k].tag;
                is_void = tag_names[k].is_void;
            }
        /* the attributes */
        attr *head = NULL, **tail = &head;
        while (j < n && s[j] != '>') {
            while (j < n && (isspace((unsigned char)s[j]) || s[j] == '/'))
                j++;
            if (j >= n || s[j] == '>')
                break;
            size_t as = j;
            while (j < n && !isspace((unsigned char)s[j]) && s[j] != '=' && s[j] != '>')
                j++;
            attr *a = calloc(1, sizeof *a);
            if (!a)
                break;
            a->name = dupn(s + as, j - as);
            while (j < n && isspace((unsigned char)s[j]))
                j++;
            if (j < n && s[j] == '=') {
                j++;
                while (j < n && isspace((unsigned char)s[j]))
                    j++;
                size_t vs, ve;
                if (j < n && (s[j] == '"' || s[j] == '\'')) {
                    char q = s[j++];
                    vs = j;
                    while (j < n && s[j] != q)
                        j++;
                    ve = j;
                    if (j < n)
                        j++;
                } else {
                    vs = j;
                    while (j < n && !isspace((unsigned char)s[j]) && s[j] != '>')
                        j++;
                    ve = j;
                }
                a->val = decode(s + vs, ve - vs);
            }
            *tail = a;
            tail = &a->next;
        }
        i = j < n ? j + 1 : n;
        if (closing) {
            for (node *o = cur; o && o != root; o = o->parent)
                if (o->tag == tag && tag != T_OTHER) {
                    cur = o->parent;
                    break;
                }
            for (attr *a = head, *nx; a; a = nx) {
                nx = a->next;
                free(a->name);
                free(a->val);
                free(a);
            }
            continue;
        }
        while (cur != root && implied_end(cur->tag, tag))
            cur = cur->parent;
        /* an item ends a sibling item of its own list only */
        node *e = node_new(tag, cur);
        if (!e)
            break;
        e->attrs = head;
        if (tag == T_SCRIPT || tag == T_STYLE) {
            const char *end = tag == T_SCRIPT ? "</script" : "</style";
            size_t el = strlen(end), k = i;
            while (k + el <= n && strncasecmp(s + k, end, el))
                k++;
            i = k;
            continue;
        }
        if (tag == T_PLAINTEXT) {
            node *tn = node_new(T_TEXT, e);
            if (tn)
                tn->text = dupn(s + i, n - i);
            i = n;
            continue;
        }
        if (!is_void)
            cur = e;
    }
    return root;
}

/* ---------------------------------------------------------------------- */
/* The renderer's state, and what it needs of the port */

enum { D_TEXT, D_FILL, D_FRAME, D_RULE, D_BULLET, D_IMAGE, D_OVAL, D_ROUND };

typedef struct {
    int kind, x, y, w, h; /* a text item's y is its baseline */
    char *text;
    int len, font, size, face;
    rgb color;
    bool under, strike;
    int link; /* index in the page's links, or -1 */
    int image; /* index in the page's images, or -1 */
    int shape; /* a bullet's: 0 disc, 1 circle, 2 square */
} ditem;

typedef struct {
    ditem *v;
    int n, cap;
} dlist;

typedef struct {
    char *src;
    int w, h; /* natural size, or 0 when there is no picture */
    gaddr importer;
    bool tried;
} image;

typedef struct hr {
    gaddr id, port;
    int16_t rect[4];
    bool active, border, cutout, force_qd;
    int vbar, hbar;
    gaddr window, embedding;
    /* the page */
    node *doc;
    char *html;
    char *url, *base, *title;
    uint8_t spec[70];
    bool have_spec;
    char **links;
    int nlinks;
    struct { char *name; int y; } *anchors;
    int nanchors;
    image *images;
    int nimages;
    rgb bg, fg, link, vlink;
    dlist items;
    int width_laid; /* the width the page was laid out for (0: not yet) */
    int doc_w, doc_h;
    int scroll_h, scroll_v;
    /* history */
    struct { char *url; int scroll; } hist[32];
    int nhist, curhist;
    /* callbacks */
    gaddr visited_upp, visited_ref, newurl_upp, newurl_ref, tospec_upp, tospec_ref;
} hr;

static ppc_cpu *hr_c; /* the call being served, for calls to other exports */
static gaddr scratch;  /* 128 bytes of guest memory: rects, colours, points */
static gaddr tbuf;     /* text for the port */
static gaddr cbuf[3];  /* strings given to the program's routines: kept until the next call */

static gaddr cstring_for_callback(int slot, const char *s)
{
    if (!cbuf[slot])
        cbuf[slot] = tb_NewPtr(1024, true, true);
    if (!cbuf[slot])
        return 0;
    size_t n = strlen(s) > 1023 ? 1023 : strlen(s);
    for (size_t i = 0; i < n; i++)
        wr8(cbuf[slot] + (uint32_t)i, (uint8_t)s[i]);
    wr8(cbuf[slot] + (uint32_t)n, 0);
    return cbuf[slot];
}

static hr *hr_of(gaddr ref) { return ref_get(ref, HR_REF); }

static bool scratch_ok(void)
{
    if (!scratch)
        scratch = tb_NewPtr(128, true, true);
    if (!tbuf)
        tbuf = tb_NewPtr(1024, true, true);
    return scratch && tbuf;
}

/* Fonts the port has (the Mac OS 9 system's). */
enum { F_GENEVA = 3, F_MONACO = 4, F_TIMES = 20, F_HELVETICA = 21, F_COURIER = 22 };
enum { FACE_BOLD = 1, FACE_ITALIC = 2, FACE_UNDERLINE = 4 };

typedef struct {
    int font, size, face;
    rgb color;
    bool under, strike, pre, nobr;
    int link, align; /* align: 0 left, 1 center, 2 right */
    int fsize;       /* the HTML size 1..7 */
} style;

static const int size_table[8] = { 12, 9, 10, 12, 14, 18, 24, 36 };

static void port_text(int font, int size, int face)
{
    tb_TextFont((int16_t)font);
    tb_TextSize((int16_t)size);
    tb_TextFace((int16_t)face);
}

static void set_rgb(rgb c, bool fore)
{
    wr16(scratch + 64, (uint16_t)(c.r * 257));
    wr16(scratch + 66, (uint16_t)(c.g * 257));
    wr16(scratch + 68, (uint16_t)(c.b * 257));
    if (fore)
        tb_RGBForeColor(scratch + 64);
    else
        tb_RGBBackColor(scratch + 64);
}

static void set_rect(int l, int t, int r, int b) { tb_SetRect(scratch, (int16_t)l, (int16_t)t, (int16_t)r, (int16_t)b); }

/* Text widths and metrics of a style, as the port will draw them. */
static int text_width(const char *s, int n, const style *st)
{
    int total = 0;
    port_text(st->font, st->size, st->face & ~FACE_UNDERLINE);
    while (n > 0) {
        int k = n > 1000 ? 1000 : n;
        for (int i = 0; i < k; i++)
            wr8(tbuf + (uint32_t)i, (uint8_t)s[i]);
        total += tb_TextWidth(tbuf, 0, (int16_t)k);
        s += k;
        n -= k;
    }
    return total;
}

static void font_metrics(const style *st, int *asc, int *desc)
{
    port_text(st->font, st->size, st->face & ~FACE_UNDERLINE);
    tb_GetFontInfo(scratch);
    *asc = (int16_t)rd16(scratch);
    *desc = (int16_t)rd16(scratch + 2) + (int16_t)rd16(scratch + 6);
}

/* ---- The list of things to draw ---- */

static ditem *dl_add(dlist *d, const ditem *it)
{
    if (d->n == d->cap) {
        int cap = d->cap ? d->cap * 2 : 64;
        ditem *v = realloc(d->v, (size_t)cap * sizeof *v);
        if (!v)
            return NULL;
        d->v = v;
        d->cap = cap;
    }
    d->v[d->n] = *it;
    return &d->v[d->n++];
}

static void dl_free(dlist *d)
{
    for (int i = 0; i < d->n; i++)
        free(d->v[i].text);
    free(d->v);
    d->v = NULL;
    d->n = d->cap = 0;
}

/* Move items from `from` to `to`, shifted; the text goes with them. */
static void dl_move(dlist *to, dlist *from, int dx, int dy)
{
    for (int i = 0; i < from->n; i++) {
        ditem it = from->v[i];
        it.x += dx;
        it.y += dy;
        dl_add(to, &it);
    }
    free(from->v);
    from->v = NULL;
    from->n = from->cap = 0;
}

/* ---- Pictures ---- */

static int image_index(hr *h, const char *src);

/* ---- Layout ---- */

typedef enum { A_WORD, A_SPACE, A_BREAK, A_BOX } akind;

typedef struct {
    akind kind;
    char *s;
    int len, w, asc, desc;
    style st;
    ditem box; /* A_BOX: relative to the box's top left */
    int valign; /* A_BOX: 0 bottom on the baseline, 1 middle, 2 top */
} atom;

typedef struct {
    hr *h;
    dlist out;
    int x0, x1, y;
    int gap;           /* the vertical gap waiting to be put before the next line */
    atom *a;
    int na, ca, lw;
    int maxx;          /* the widest line's right edge */
    style st;
    int lists[8];      /* list nesting: kind and counter */
    int nlist;
    int list_count[8];
    int list_kind[8];
    int indent_depth;
} lay;

static const int BIG = 100000;

static void lay_free(lay *l)
{
    for (int i = 0; i < l->na; i++)
        free(l->a[i].s);
    free(l->a);
    dl_free(&l->out);
}

static void atom_add(lay *l, atom a)
{
    if (l->na == l->ca) {
        int cap = l->ca ? l->ca * 2 : 32;
        atom *v = realloc(l->a, (size_t)cap * sizeof *v);
        if (!v)
            return;
        l->a = v;
        l->ca = cap;
    }
    l->a[l->na++] = a;
    l->lw += a.w;
}

/* The line is finished: its atoms become items. */
static void flush_line(lay *l, bool force)
{
    while (l->na && l->a[l->na - 1].kind == A_SPACE) { /* trailing blanks */
        l->lw -= l->a[l->na - 1].w;
        l->na--;
    }
    if (!l->na && !force)
        return;
    int asc = 0, desc = 0;
    for (int i = 0; i < l->na; i++) {
        atom *a = &l->a[i];
        int aa = a->asc, dd = a->desc;
        if (a->kind == A_BOX && a->valign == 1) {
            aa = a->asc / 2 + 4;
            dd = a->asc - aa;
        }
        if (aa > asc)
            asc = aa;
        if (dd > desc)
            desc = dd;
    }
    if (!l->na) {
        font_metrics(&l->st, &asc, &desc);
    }
    int top = l->y + l->gap;
    l->gap = 0;
    int avail = l->x1 - l->x0, x = l->x0;
    if (l->st.align == 1 && l->lw < avail)
        x += (avail - l->lw) / 2;
    else if (l->st.align == 2 && l->lw < avail)
        x += avail - l->lw;
    int base = top + asc, prev_x = x;
    for (int i = 0; i < l->na; i++) {
        atom *a = &l->a[i];
        if (a->kind == A_WORD && a->s) {
            ditem it = { .kind = D_TEXT, .x = x, .y = base, .w = a->w, .h = a->asc + a->desc, .text = a->s,
                         .len = a->len, .font = a->st.font, .size = a->st.size, .face = a->st.face,
                         .color = a->st.color, .under = a->st.under, .strike = a->st.strike, .link = a->st.link,
                         .image = -1 };
            if (dl_add(&l->out, &it))
                a->s = NULL;
        } else if (a->kind == A_SPACE && (a->st.under || a->st.strike) && a->st.link >= 0 && i > 0 && i + 1 < l->na) {
            ditem it = { .kind = D_FILL, .x = x, .y = base + 1, .w = a->w, .h = 1, .color = a->st.color,
                         .link = a->st.link, .image = -1 };
            dl_add(&l->out, &it);
        } else if (a->kind == A_BOX) {
            ditem it = a->box;
            if (it.kind == D_TEXT && a->w == 0) {
                /* text inside the box before it: its baseline above the box's bottom */
                it.x += prev_x;
                it.y = base - 3 - it.h;
                it.h = it.size;
            } else {
                int by = base - a->asc;
                if (a->valign == 1)
                    by = base - a->asc / 2 - 4;
                else if (a->valign == 2)
                    by = top;
                it.x += x;
                it.y += by;
            }
            dl_add(&l->out, &it);
        }
        prev_x = x;
        x += a->w;
    }
    if (x > l->maxx)
        l->maxx = x;
    for (int i = 0; i < l->na; i++)
        free(l->a[i].s);
    l->na = 0;
    l->lw = 0;
    l->y = top + asc + desc;
}

static void line_break(lay *l) { flush_line(l, true); }

/* Starting or ending a block: the line ends, and a gap of at least this
 * many pixels is between it and the next. */
static void block_break(lay *l, int gap)
{
    flush_line(l, false);
    if (gap > l->gap)
        l->gap = gap;
}

static void add_word(lay *l, const char *s, int n)
{
    int w = text_width(s, n, &l->st);
    if (!l->st.nobr && !l->st.pre && l->lw + w > l->x1 - l->x0 && l->na)
        flush_line(l, false);
    int asc, desc;
    font_metrics(&l->st, &asc, &desc);
    atom a = { .kind = A_WORD, .s = dupn(s, (size_t)n), .len = n, .w = w, .asc = asc, .desc = desc, .st = l->st };
    atom_add(l, a);
}

static void add_space(lay *l, int count)
{
    if (!l->na && !l->st.pre)
        return;
    if (l->na && l->a[l->na - 1].kind == A_SPACE && !l->st.pre)
        return;
    int w = text_width(" ", 1, &l->st) * count;
    int asc, desc;
    font_metrics(&l->st, &asc, &desc);
    atom a = { .kind = A_SPACE, .w = w, .asc = asc, .desc = desc, .st = l->st };
    if (!l->st.nobr && !l->st.pre && l->lw + w > l->x1 - l->x0 && l->na)
        flush_line(l, false);
    atom_add(l, a);
}

static void lay_text(lay *l, const char *t)
{
    size_t n = strlen(t);
    for (size_t i = 0; i < n;) {
        if (l->st.pre) {
            if (t[i] == '\n' || t[i] == '\r') {
                if (t[i] == '\r' && i + 1 < n && t[i + 1] == '\n')
                    i++;
                line_break(l);
                i++;
            } else if (t[i] == ' ' || t[i] == '\t') {
                size_t j = i;
                int cols = 0;
                while (j < n && (t[j] == ' ' || t[j] == '\t'))
                    cols += t[j++] == '\t' ? 8 : 1;
                add_space(l, cols);
                i = j;
            } else {
                size_t j = i;
                while (j < n && !strchr(" \t\r\n", t[j]))
                    j++;
                add_word(l, t + i, (int)(j - i));
                i = j;
            }
        } else if (isspace((unsigned char)t[i])) {
            while (i < n && isspace((unsigned char)t[i]))
                i++;
            add_space(l, 1);
        } else {
            size_t j = i;
            while (j < n && !isspace((unsigned char)t[j]))
                j++;
            add_word(l, t + i, (int)(j - i));
            i = j;
        }
    }
}

static int link_index(hr *h, const char *href)
{
    char **v = realloc(h->links, (size_t)(h->nlinks + 1) * sizeof *v);
    if (!v)
        return -1;
    h->links = v;
    h->links[h->nlinks] = dups(href);
    return h->nlinks++;
}

static void add_anchor(lay *l, const char *name)
{
    hr *h = l->h;
    void *v = realloc(h->anchors, (size_t)(h->nanchors + 1) * sizeof *h->anchors);
    if (!v)
        return;
    h->anchors = v;
    h->anchors[h->nanchors].name = dups(name);
    h->anchors[h->nanchors++].y = l->y + l->gap;
}

static void lay_children(lay *l, node *n);
static void lay_node(lay *l, node *n);
static void lay_table(lay *l, node *t);

static int font_for_face(const char *face, int def)
{
    if (!face)
        return def;
    char buf[64];
    snprintf(buf, sizeof buf, "%s", face);
    for (char *p = buf; *p; p++)
        *p = (char)tolower((unsigned char)*p);
    if (strstr(buf, "courier") || strstr(buf, "mono") || strstr(buf, "typewriter"))
        return F_COURIER;
    if (strstr(buf, "monaco"))
        return F_MONACO;
    if (strstr(buf, "geneva"))
        return F_GENEVA;
    if (strstr(buf, "helvetica") || strstr(buf, "arial") || strstr(buf, "sans"))
        return F_HELVETICA;
    if (strstr(buf, "times") || strstr(buf, "serif"))
        return F_TIMES;
    return def;
}

static void set_fsize(style *st, int fs)
{
    st->fsize = fs < 1 ? 1 : fs > 7 ? 7 : fs;
    st->size = size_table[st->fsize];
}

static int image_size(hr *h, int idx, int *w, int *hh);

/* A box standing for something drawn in a frame: image placeholder, a form
 * control. */
static void add_box(lay *l, int w, int hh, ditem proto, int valign)
{
    if (!l->st.nobr && l->lw + w > l->x1 - l->x0 && l->na)
        flush_line(l, false);
    proto.w = w;
    proto.h = hh;
    atom a = { .kind = A_BOX, .w = w, .asc = hh, .desc = 0, .st = l->st, .box = proto, .valign = valign };
    atom_add(l, a);
}

static void lay_image(lay *l, node *n)
{
    hr *h = l->h;
    const char *src = attr_of(n, "src"), *alt = attr_of(n, "alt");
    int idx = src ? image_index(h, src) : -1;
    int iw = 0, ih = 0;
    if (idx >= 0)
        image_size(h, idx, &iw, &ih);
    int w = attr_int(n, "width", iw), hh = attr_int(n, "height", ih);
    const char *wattr = attr_of(n, "width");
    if (wattr && strchr(wattr, '%'))
        w = (l->x1 - l->x0) * atoi(wattr) / 100;
    if (iw && ih && (attr_of(n, "width") != NULL) != (attr_of(n, "height") != NULL)) {
        if (attr_of(n, "width"))
            hh = ih * w / iw;
        else
            w = iw * hh / ih;
    }
    int bw = attr_int(n, "border", l->st.link >= 0 ? 2 : 0);
    int hs = attr_int(n, "hspace", 0), vs = attr_int(n, "vspace", 0);
    const char *al = attr_of(n, "align");
    int valign = al && !strcasecmp(al, "top") ? 2 : al && (!strcasecmp(al, "middle") || !strcasecmp(al, "absmiddle")) ? 1 : 0;
    if (idx >= 0 && iw) {
        ditem it = { .kind = D_IMAGE, .x = hs + bw, .y = vs + bw, .image = idx, .link = l->st.link, .color = l->st.color };
        int tw = w + 2 * (hs + bw), th = hh + 2 * (vs + bw);
        it.w = w;
        it.h = hh;
        add_box(l, tw, th, it, valign);
        return;
    }
    /* no picture: its ALT text in a frame, as a browser shows a missing image */
    int tw = w ? w : 0, th = hh ? hh : 0;
    char *label = dups(alt && *alt ? alt : "");
    int lw = text_width(label, (int)strlen(label), &l->st);
    if (!w)
        tw = lw + 8;
    if (!hh) {
        int asc, desc;
        font_metrics(&l->st, &asc, &desc);
        th = asc + desc + 6;
    }
    if (!tw)
        tw = 12;
    ditem it = { .kind = D_FRAME, .x = 0, .y = 0, .image = -1, .link = l->st.link, .color = l->st.color, .shape = 1 };
    add_box(l, tw, th, it, valign);
    if (*label) {
        int asc, desc;
        font_metrics(&l->st, &asc, &desc);
        ditem tx = { .kind = D_TEXT, .x = 4, .y = 0, .h = desc, .text = label, .len = (int)strlen(label),
                     .font = l->st.font, .size = l->st.size, .face = l->st.face, .color = l->st.color,
                     .link = l->st.link, .image = -1 };
        atom a = { .kind = A_BOX, .w = 0, .asc = 0, .desc = 0, .st = l->st, .box = tx, .valign = valign };
        atom_add(l, a);
    } else {
        free(label);
    }
}

static void lay_control(lay *l, node *n)
{
    const char *type = attr_of(n, "type");
    int asc, desc;
    font_metrics(&l->st, &asc, &desc);
    int lh = asc + desc;
    style st = l->st;
    if (n->tag == T_INPUT) {
        if (type && !strcasecmp(type, "hidden"))
            return;
        if (type && (!strcasecmp(type, "checkbox") || !strcasecmp(type, "radio"))) {
            ditem it = { .kind = !strcasecmp(type, "radio") ? D_OVAL : D_FRAME, .image = -1, .link = -1,
                         .color = st.color };
            add_box(l, 14, 12, it, 0);
            return;
        }
        if (type && (!strcasecmp(type, "submit") || !strcasecmp(type, "reset") || !strcasecmp(type, "button"))) {
            const char *v = attr_of(n, "value");
            if (!v || !*v)
                v = !strcasecmp(type, "reset") ? "Reset" : !strcasecmp(type, "submit") ? "Submit" : "Button";
            int tw = text_width(v, (int)strlen(v), &st) + 16;
            ditem it = { .kind = D_ROUND, .image = -1, .link = -1, .color = st.color };
            add_box(l, tw, lh + 6, it, 0);
            ditem tx = { .kind = D_TEXT, .x = 8, .y = 0, .h = desc, .text = dups(v), .len = (int)strlen(v),
                         .font = st.font, .size = st.size, .face = st.face, .color = st.color, .link = -1, .image = -1 };
            atom a = { .kind = A_BOX, .w = 0, .asc = 0, .desc = 0, .st = st, .box = tx };
            atom_add(l, a);
            return;
        }
        int size = attr_int(n, "size", 20);
        ditem it = { .kind = D_FRAME, .image = -1, .link = -1, .color = st.color, .shape = 1 };
        add_box(l, size * 6 + 6, lh + 6, it, 0);
        return;
    }
    if (n->tag == T_SELECT) {
        int widest = 40;
        for (node *o = n->first; o; o = o->next)
            if (o->tag == T_OPTION && o->first && o->first->text) {
                int w = text_width(o->first->text, (int)strlen(o->first->text), &st);
                if (w > widest)
                    widest = w;
            }
        ditem it = { .kind = D_FRAME, .image = -1, .link = -1, .color = st.color, .shape = 1 };
        add_box(l, widest + 28, lh + 6, it, 0);
        return;
    }
    if (n->tag == T_TEXTAREA) {
        int cols = attr_int(n, "cols", 20), rows = attr_int(n, "rows", 2);
        ditem it = { .kind = D_FRAME, .image = -1, .link = -1, .color = st.color, .shape = 1 };
        add_box(l, cols * 6 + 8, rows * lh + 6, it, 0);
    }
}

static void align_of(lay *l, node *n, int *saved)
{
    *saved = l->st.align;
    const char *a = attr_of(n, "align");
    if (a && !strcasecmp(a, "center"))
        l->st.align = 1;
    else if (a && !strcasecmp(a, "right"))
        l->st.align = 2;
    else if (a && !strcasecmp(a, "left"))
        l->st.align = 0;
}

static void lay_list(lay *l, node *n)
{
    int kind = n->tag == T_OL ? 1 : 0;
    if (l->nlist < 8) {
        l->list_kind[l->nlist] = kind;
        l->list_count[l->nlist] = attr_int(n, "start", 1);
        l->nlist++;
    }
    int saved0 = l->x0;
    block_break(l, l->nlist > 1 ? 0 : 8);
    l->x0 += 30;
    lay_children(l, n);
    block_break(l, l->nlist > 1 ? 0 : 8);
    l->x0 = saved0;
    if (l->nlist)
        l->nlist--;
}

static void lay_item(lay *l, node *n)
{
    block_break(l, 2);
    flush_line(l, false);
    int depth = l->nlist ? l->nlist - 1 : 0;
    int asc, desc;
    font_metrics(&l->st, &asc, &desc);
    int top = l->y + l->gap;
    if (l->nlist && l->list_kind[depth] == 1) {
        char num[16];
        snprintf(num, sizeof num, "%d.", l->list_count[depth]++);
        int w = text_width(num, (int)strlen(num), &l->st);
        ditem it = { .kind = D_TEXT, .x = l->x0 - 6 - w, .y = top + asc, .text = dups(num), .len = (int)strlen(num),
                     .font = l->st.font, .size = l->st.size, .face = l->st.face, .color = l->st.color, .link = -1,
                     .image = -1 };
        dl_add(&l->out, &it);
    } else {
        ditem it = { .kind = D_BULLET, .x = l->x0 - 15, .y = top + asc / 2 - 2, .w = 5, .h = 5, .color = l->st.color,
                     .link = -1, .image = -1, .shape = depth > 2 ? 2 : depth };
        dl_add(&l->out, &it);
    }
    (void)n;
}

static void lay_hr(lay *l, node *n)
{
    block_break(l, 6);
    flush_line(l, false);
    int avail = l->x1 - l->x0;
    const char *wa = attr_of(n, "width");
    int w = avail;
    if (wa) {
        w = atoi(wa);
        if (strchr(wa, '%'))
            w = avail * w / 100;
        if (w > avail)
            w = avail;
    }
    int size = attr_int(n, "size", 2);
    const char *al = attr_of(n, "align");
    int x = l->x0 + (al && !strcasecmp(al, "left") ? 0 : al && !strcasecmp(al, "right") ? avail - w : (avail - w) / 2);
    ditem it = { .kind = D_RULE, .x = x, .y = l->y + l->gap, .w = w, .h = size, .link = -1, .image = -1,
                 .shape = attr_of(n, "noshade") != NULL };
    dl_add(&l->out, &it);
    l->y += l->gap + size;
    l->gap = 6;
    if (x + w > l->maxx)
        l->maxx = x + w;
}

static void lay_node(lay *l, node *n)
{
    style saved = l->st;
    int saved_x0 = l->x0, saved_x1 = l->x1;
    int align_saved;
    switch (n->tag) {
    case T_TEXT:
        lay_text(l, n->text);
        return;
    case T_HEAD: case T_TITLE: case T_BASE: case T_META: case T_LINK: case T_SCRIPT: case T_STYLE: case T_PARAM:
    case T_OPTION: case T_MAP: case T_AREA: case T_ISINDEX:
        return;
    case T_BR: {
        line_break(l);
        return;
    }
    case T_HR:
        lay_hr(l, n);
        return;
    case T_H1: case T_H2: case T_H3: case T_H4: case T_H5: case T_H6: {
        align_of(l, n, &align_saved);
        block_break(l, 12);
        l->st.face |= FACE_BOLD;
        set_fsize(&l->st, n->tag == T_H1 ? 6 : n->tag == T_H2 ? 5 : n->tag == T_H3 ? 4 : n->tag == T_H4 ? 3
                                              : n->tag == T_H5 ? 2 : 1);
        lay_children(l, n);
        block_break(l, 8);
        break;
    }
    case T_P:
        align_of(l, n, &align_saved);
        block_break(l, 10);
        lay_children(l, n);
        block_break(l, 10);
        break;
    case T_DIV: case T_CENTER: case T_FORM:
        align_of(l, n, &align_saved);
        if (n->tag == T_CENTER)
            l->st.align = 1;
        if (n->tag != T_FORM || 1)
            block_break(l, 0);
        lay_children(l, n);
        block_break(l, 0);
        break;
    case T_BLOCKQUOTE:
        align_of(l, n, &align_saved);
        block_break(l, 10);
        l->x0 += 30;
        l->x1 -= 30;
        lay_children(l, n);
        block_break(l, 10);
        break;
    case T_ADDRESS:
        align_of(l, n, &align_saved);
        block_break(l, 8);
        l->st.face |= FACE_ITALIC;
        lay_children(l, n);
        block_break(l, 8);
        break;
    case T_PRE: case T_LISTING: case T_XMP:
        align_of(l, n, &align_saved);
        block_break(l, 10);
        l->st.pre = true;
        l->st.font = F_MONACO;
        l->st.size = 9;
        l->st.align = 0;
        /* a newline right after the tag does not count */
        if (n->first && n->first->tag == T_TEXT && n->first->text && (n->first->text[0] == '\n' || n->first->text[0] == '\r')) {
            char *t = n->first->text;
            memmove(t, t + (t[0] == '\r' && t[1] == '\n' ? 2 : 1), strlen(t));
        }
        lay_children(l, n);
        block_break(l, 10);
        break;
    case T_UL: case T_OL: case T_MENU: case T_DIR:
        lay_list(l, n);
        break;
    case T_LI:
        lay_item(l, n);
        lay_children(l, n);
        l->x0 = saved_x0;
        break;
    case T_DL:
        block_break(l, 8);
        lay_children(l, n);
        block_break(l, 8);
        break;
    case T_DT:
        block_break(l, 2);
        lay_children(l, n);
        break;
    case T_DD:
        block_break(l, 2);
        l->x0 += 36;
        lay_children(l, n);
        block_break(l, 2);
        break;
    case T_B: case T_STRONG:
        l->st.face |= FACE_BOLD;
        lay_children(l, n);
        break;
    case T_I: case T_EM: case T_CITE: case T_VAR: case T_DFN:
        l->st.face |= FACE_ITALIC;
        lay_children(l, n);
        break;
    case T_U:
        l->st.under = true;
        lay_children(l, n);
        break;
    case T_STRIKE:
        l->st.strike = true;
        lay_children(l, n);
        break;
    case T_TT: case T_CODE: case T_SAMP: case T_KBD:
        l->st.font = F_MONACO;
        l->st.size = l->st.size > 10 ? 9 : l->st.size;
        lay_children(l, n);
        break;
    case T_BIG:
        set_fsize(&l->st, l->st.fsize + 1);
        lay_children(l, n);
        break;
    case T_SMALL: case T_SUB: case T_SUP:
        set_fsize(&l->st, l->st.fsize - 1);
        lay_children(l, n);
        break;
    case T_FONT: {
        const char *sz = attr_of(n, "size"), *col = attr_of(n, "color"), *face = attr_of(n, "face");
        if (sz && *sz)
            set_fsize(&l->st, (*sz == '+' || *sz == '-') ? l->st.fsize + atoi(sz) : atoi(sz));
        rgb c;
        if (col && parse_color(col, &c))
            l->st.color = c;
        l->st.font = font_for_face(face, l->st.font);
        lay_children(l, n);
        break;
    }
    case T_BASEFONT:
        set_fsize(&saved, attr_int(n, "size", 3));
        l->st.fsize = saved.fsize;
        l->st.size = saved.size;
        return;
    case T_NOBR:
        l->st.nobr = true;
        lay_children(l, n);
        break;
    case T_A: {
        const char *href = attr_of(n, "href"), *name = attr_of(n, "name");
        if (name)
            add_anchor(l, name);
        if (href) {
            l->st.link = link_index(l->h, href);
            l->st.under = true;
            l->st.color = l->h->link;
        }
        lay_children(l, n);
        break;
    }
    case T_IMG:
        lay_image(l, n);
        return;
    case T_TABLE:
        lay_table(l, n);
        break;
    case T_INPUT: case T_SELECT: case T_TEXTAREA:
        lay_control(l, n);
        return;
    case T_CAPTION:
        return; /* laid out by its table */
    case T_TR: case T_TD: case T_TH: /* outside a table */
        block_break(l, 0);
        lay_children(l, n);
        block_break(l, 0);
        break;
    default:
        lay_children(l, n);
        break;
    }
    l->st = saved;
    l->x0 = saved_x0;
    l->x1 = saved_x1;
}

static void lay_children(lay *l, node *n)
{
    for (node *c = n->first; c; c = c->next)
        lay_node(l, c);
}

static void lay_init(lay *l, hr *h, int x0, int x1, const style *st)
{
    memset(l, 0, sizeof *l);
    l->h = h;
    l->x0 = x0;
    l->x1 = x1;
    l->st = *st;
}

/* ---- Tables ---- */

typedef struct {
    node *n;
    int row, col, cspan, rspan;
    int minw, maxw, w, h;
    dlist items;
} cell;

static void measure_cell(hr *h, node *n, const style *st, int *minw, int *maxw)
{
    lay a, b;
    lay_init(&a, h, 0, BIG, st);
    lay_children(&a, n);
    flush_line(&a, false);
    *maxw = a.maxx;
    lay_free(&a);
    lay_init(&b, h, 0, 1, st);
    b.st.nobr = false;
    lay_children(&b, n);
    flush_line(&b, false);
    *minw = b.maxx;
    lay_free(&b);
}

static void lay_table(lay *l, node *t)
{
    hr *h = l->h;
    int bw = 0;
    const char *ba = attr_of(t, "border");
    if (ba)
        bw = *ba ? atoi(ba) : 1;
    int cs = attr_int(t, "cellspacing", 2), cp = attr_int(t, "cellpadding", 1);
    cell *cells = NULL;
    int ncells = 0, nrows = 0, ncols = 0;
    node *caption = NULL;
    /* the cells, row by row; rows may sit in THEAD/TBODY, which are kept as OTHER */
    node *stack[4] = { t, NULL };
    (void)stack;
    for (node *r = t->first; r; r = r->next) {
        node *rows[2] = { r, NULL };
        if (r->tag == T_CAPTION)
            caption = r;
        if (r->tag == T_OTHER)
            rows[0] = r->first;
        for (node *row = (r->tag == T_OTHER ? r->first : r); row; row = r->tag == T_OTHER ? row->next : NULL) {
            if (row->tag != T_TR)
                continue;
            int col = 0;
            for (node *c = row->first; c; c = c->next) {
                if (c->tag != T_TD && c->tag != T_TH)
                    continue;
                cell *v = realloc(cells, (size_t)(ncells + 1) * sizeof *cells);
                if (!v)
                    break;
                cells = v;
                cell *ce = &cells[ncells++];
                memset(ce, 0, sizeof *ce);
                ce->n = c;
                ce->row = nrows;
                ce->col = col;
                ce->cspan = attr_int(c, "colspan", 1) < 1 ? 1 : attr_int(c, "colspan", 1);
                ce->rspan = attr_int(c, "rowspan", 1) < 1 ? 1 : attr_int(c, "rowspan", 1);
                col += ce->cspan;
            }
            if (col > ncols)
                ncols = col;
            nrows++;
        }
    }
    if (!ncells) {
        free(cells);
        return;
    }
    /* rowspans occupy the columns below them: shift cells right past them */
    for (int i = 0; i < ncells; i++) {
        for (;;) {
            bool clash = false;
            for (int j = 0; j < i && !clash; j++)
                if (cells[j].rspan > 1 && cells[j].row < cells[i].row && cells[i].row < cells[j].row + cells[j].rspan &&
                    cells[i].col < cells[j].col + cells[j].cspan && cells[j].col < cells[i].col + cells[i].cspan)
                    clash = true;
            if (!clash)
                break;
            cells[i].col++;
        }
        if (cells[i].col + cells[i].cspan > ncols)
            ncols = cells[i].col + cells[i].cspan;
    }
    int *colmin = calloc((size_t)ncols, sizeof(int)), *colmax = calloc((size_t)ncols, sizeof(int)),
        *colw = calloc((size_t)ncols, sizeof(int)), *rowh = calloc((size_t)nrows, sizeof(int)),
        *rowy = calloc((size_t)nrows + 1, sizeof(int));
    int pad = cp + (bw ? 1 : 0);
    for (int i = 0; i < ncells; i++) {
        cell *ce = &cells[i];
        style st = l->st;
        st.align = 0;
        if (ce->n->tag == T_TH)
            st.face |= FACE_BOLD;
        measure_cell(h, ce->n, &st, &ce->minw, &ce->maxw);
        ce->minw += 2 * pad;
        ce->maxw += 2 * pad;
        const char *wa = attr_of(ce->n, "width");
        if (wa && *wa && !strchr(wa, '%') && attr_of(ce->n, "nowrap") == NULL) {
            int w = atoi(wa) + 2 * pad;
            if (w > ce->minw)
                ce->minw = w;
            ce->maxw = w;
        }
        if (attr_of(ce->n, "nowrap"))
            ce->minw = ce->maxw;
    }
    for (int i = 0; i < ncells; i++)
        if (cells[i].cspan == 1) {
            int c = cells[i].col;
            if (cells[i].minw > colmin[c])
                colmin[c] = cells[i].minw;
            if (cells[i].maxw > colmax[c])
                colmax[c] = cells[i].maxw;
        }
    for (int i = 0; i < ncells; i++)
        if (cells[i].cspan > 1) {
            int sm = 0, sx = 0;
            for (int c = cells[i].col; c < cells[i].col + cells[i].cspan; c++)
                sm += colmin[c], sx += colmax[c];
            sm += (cells[i].cspan - 1) * (cs + (bw ? 0 : 0));
            sx += (cells[i].cspan - 1) * cs;
            int last = cells[i].col + cells[i].cspan - 1;
            if (cells[i].minw > sm)
                colmin[last] += cells[i].minw - sm;
            if (cells[i].maxw > sx)
                colmax[last] += cells[i].maxw - sx;
        }
    int avail = l->x1 - l->x0;
    const char *tw = attr_of(t, "width");
    int want = 0;
    if (tw && *tw)
        want = strchr(tw, '%') ? avail * atoi(tw) / 100 : atoi(tw);
    int spacing = cs * (ncols + 1) + 2 * bw;
    int summin = 0, summax = 0;
    for (int c = 0; c < ncols; c++)
        summin += colmin[c], summax += colmax[c];
    int target = want ? want : avail;
    if (target > avail)
        target = avail;
    int room = target - spacing;
    for (int c = 0; c < ncols; c++) {
        if (want && room > summax)
            colw[c] = summax ? colmax[c] + (room - summax) * colmax[c] / summax : room / ncols;
        else if (summax <= room)
            colw[c] = colmax[c];
        else if (summin >= room)
            colw[c] = colmin[c];
        else
            colw[c] = colmin[c] + (colmax[c] - colmin[c]) * (room - summin) / (summax - summin);
    }
    int tablew = spacing;
    for (int c = 0; c < ncols; c++)
        tablew += colw[c];
    int tx = l->x0;
    if (l->st.align == 1)
        tx += (avail - tablew) / 2;
    else if (l->st.align == 2)
        tx += avail - tablew;
    const char *tal = attr_of(t, "align");
    if (tal && !strcasecmp(tal, "center"))
        tx = l->x0 + (avail - tablew) / 2;
    else if (tal && !strcasecmp(tal, "right"))
        tx = l->x0 + avail - tablew;
    flush_line(l, false);
    block_break(l, 6);
    int top = l->y + l->gap;
    l->gap = 0;
    /* the caption above */
    if (caption) {
        lay cap;
        style st = l->st;
        st.align = 1;
        lay_init(&cap, h, tx, tx + tablew, &st);
        cap.y = top;
        lay_children(&cap, caption);
        flush_line(&cap, false);
        dl_move(&l->out, &cap.out, 0, 0);
        top = cap.y + 4;
        lay_free(&cap);
    }
    /* each cell laid out at its width, its height known */
    for (int i = 0; i < ncells; i++) {
        cell *ce = &cells[i];
        int w = (ce->cspan - 1) * cs;
        for (int c = ce->col; c < ce->col + ce->cspan && c < ncols; c++)
            w += colw[c];
        ce->w = w;
        lay sub;
        style st = l->st;
        st.align = 0;
        const char *ca = attr_of(ce->n, "align");
        if (ca && !strcasecmp(ca, "center"))
            st.align = 1;
        else if (ca && !strcasecmp(ca, "right"))
            st.align = 2;
        else if (ce->n->tag == T_TH)
            st.align = 1;
        if (ce->n->tag == T_TH)
            st.face |= FACE_BOLD;
        const char *fg = NULL;
        (void)fg;
        lay_init(&sub, h, pad, w - pad, &st);
        sub.y = pad;
        if (attr_of(ce->n, "nowrap"))
            sub.st.nobr = true;
        lay_children(&sub, ce->n);
        flush_line(&sub, false);
        ce->h = sub.y + pad;
        ce->items = sub.out;
        sub.out = (dlist){ 0 };
        lay_free(&sub);
    }
    for (int r = 0; r < nrows; r++)
        for (int i = 0; i < ncells; i++)
            if (cells[i].row == r && cells[i].rspan == 1 && cells[i].h > rowh[r])
                rowh[r] = cells[i].h;
    for (int i = 0; i < ncells; i++)
        if (cells[i].rspan > 1) {
            int sum = (cells[i].rspan - 1) * cs;
            int last = cells[i].row + cells[i].rspan - 1 < nrows ? cells[i].row + cells[i].rspan - 1 : nrows - 1;
            for (int r = cells[i].row; r <= last; r++)
                sum += rowh[r];
            if (cells[i].h > sum)
                rowh[last] += cells[i].h - sum;
        }
    int y = top + bw + cs;
    for (int r = 0; r < nrows; r++) {
        rowy[r] = y;
        y += rowh[r] + cs;
    }
    rowy[nrows] = y;
    int tableh = y - top + bw;
    /* the table's own background and border */
    rgb c;
    if (parse_color(attr_of(t, "bgcolor"), &c)) {
        ditem f = { .kind = D_FILL, .x = tx, .y = top, .w = tablew, .h = tableh, .color = c, .link = -1, .image = -1 };
        dl_add(&l->out, &f);
    }
    if (bw) {
        ditem f = { .kind = D_FRAME, .x = tx, .y = top, .w = tablew, .h = tableh, .color = { 0x60, 0x60, 0x60 },
                    .link = -1, .image = -1, .shape = bw };
        dl_add(&l->out, &f);
    }
    for (int i = 0; i < ncells; i++) {
        cell *ce = &cells[i];
        int x = tx + bw + cs;
        for (int k = 0; k < ce->col && k < ncols; k++)
            x += colw[k] + cs;
        int last = ce->row + ce->rspan - 1 < nrows ? ce->row + ce->rspan - 1 : nrows - 1;
        int ch = rowy[last] + rowh[last] - rowy[ce->row];
        if (parse_color(attr_of(ce->n, "bgcolor"), &c)) {
            ditem f = { .kind = D_FILL, .x = x, .y = rowy[ce->row], .w = ce->w, .h = ch, .color = c, .link = -1,
                        .image = -1 };
            dl_add(&l->out, &f);
        }
        if (bw) {
            ditem f = { .kind = D_FRAME, .x = x, .y = rowy[ce->row], .w = ce->w, .h = ch, .color = { 0x90, 0x90, 0x90 },
                        .link = -1, .image = -1, .shape = 1 };
            dl_add(&l->out, &f);
        }
        const char *va = attr_of(ce->n, "valign");
        int dy = 0;
        if (va && !strcasecmp(va, "middle"))
            dy = (ch - ce->h) / 2;
        else if (va && !strcasecmp(va, "bottom"))
            dy = ch - ce->h;
        else if (!va)
            dy = (ch - ce->h) / 2; /* browsers of the time centred cells vertically */
        dl_move(&l->out, &ce->items, x, rowy[ce->row] + dy);
    }
    l->y = top + tableh;
    l->gap = 6;
    if (tx + tablew > l->maxx)
        l->maxx = tx + tablew;
    free(cells);
    free(colmin);
    free(colmax);
    free(colw);
    free(rowh);
    free(rowy);
}

/* ---------------------------------------------------------------------- */
/* URLs and files */

static bool has_scheme(const char *u)
{
    if (!isalpha((unsigned char)*u))
        return false;
    const char *p = u;
    while (isalnum((unsigned char)*p) || *p == '+' || *p == '-' || *p == '.')
        p++;
    return *p == ':';
}

/* link resolved against root (RFC 1808): a new string. */
static char *full_url(const char *root, const char *link)
{
    if (!link)
        link = "";
    if (has_scheme(link))
        return dups(link);
    if (!root || !*root)
        return dups(link);
    size_t rl = strlen(root);
    size_t strip = strcspn(root, "#"); /* no fragment */
    if (*link == '#') {
        char *r = malloc(strip + strlen(link) + 1);
        memcpy(r, root, strip);
        strcpy(r + strip, link);
        return r;
    }
    (void)rl;
    size_t q = strcspn(root, "?#");
    const char *colon = strchr(root, ':');
    size_t auth_end = colon ? (size_t)(colon - root) + 1 : 0;
    if (root[auth_end] == '/' && root[auth_end + 1] == '/') {
        auth_end += 2;
        while (root[auth_end] && root[auth_end] != '/')
            auth_end++;
    }
    char *r = malloc(q + strlen(link) + 8);
    if (!r)
        return NULL;
    if (link[0] == '/' && link[1] == '/') {
        snprintf(r, q + strlen(link) + 8, "%.*s%s", colon ? (int)(colon - root) + 1 : 0, root, link);
        return r;
    }
    size_t dir = q;
    while (dir > auth_end && root[dir - 1] != '/')
        dir--;
    if (link[0] == '/')
        snprintf(r, q + strlen(link) + 8, "%.*s%s", (int)auth_end, root, link);
    else
        snprintf(r, q + strlen(link) + 8, "%.*s%s%s", (int)dir, root, dir > auth_end ? "" : "/", link);
    /* ./ and ../ */
    size_t start = auth_end;
    for (size_t i = start; r[i];) {
        if (!strncmp(r + i, "/./", 3)) {
            memmove(r + i, r + i + 2, strlen(r + i + 2) + 1);
        } else if (!strncmp(r + i, "/../", 4) && i > start) {
            size_t j = i;
            while (j > start && r[j - 1] != '/')
                j--;
            if (j > start) {
                memmove(r + j - 1, r + i + 3, strlen(r + i + 3) + 1);
                i = j > 1 ? j - 1 : start;
                continue;
            }
            i++;
        } else {
            i++;
        }
    }
    return r;
}

static void url_encode_into(char *out, size_t max, const char *s)
{
    size_t o = 0;
    for (; *s && o + 4 < max; s++) {
        unsigned char ch = (unsigned char)*s;
        if (isalnum(ch) || strchr("-_.~/", ch))
            out[o++] = (char)ch;
        else
            o += (size_t)snprintf(out + o, max - o, "%%%02X", ch);
    }
    out[o] = 0;
}

/* The file: URL of an FSSpec: the volume, the folders and the name. */
static bool url_of_spec(gaddr spec, char *out, size_t max)
{
    char parts[32][256];
    int n = 0;
    uint8_t len = rd8(spec + FSSpec_name_off);
    gget(parts[n], spec + FSSpec_name_off + 1, len);
    parts[n++][len] = 0;
    int16_t vref = (int16_t)rd16(spec + FSSpec_vRefNum_off);
    int32_t dir = (int32_t)rd32(spec + FSSpec_parID_off);
    gaddr pb = tb_NewPtr(CInfoPBRec_SIZEOF + 256, false, true);
    if (!pb)
        return false;
    gaddr nm = pb + CInfoPBRec_SIZEOF;
    while (dir != 1 && n < 32) {
        wr32(pb + HFileInfo_ioNamePtr_off, nm);
        wr16(pb + HFileInfo_ioVRefNum_off, (uint16_t)vref);
        wr32(pb + HFileInfo_ioDirID_off, (uint32_t)dir);
        wr16(pb + HFileInfo_ioFDirIndex_off, 0xFFFF);
        if (tb_PBGetCatInfo(pb, false)) {
            tb_DisposePtr(pb);
            return false;
        }
        len = rd8(nm);
        gget(parts[n], nm + 1, len);
        parts[n++][len] = 0;
        dir = (int32_t)rd32(pb + DirInfo_ioDrParID_off);
    }
    tb_DisposePtr(pb);
    size_t o = (size_t)snprintf(out, max, "file://");
    for (int i = n - 1; i >= 0 && o + 8 < max; i--) {
        out[o++] = '/';
        url_encode_into(out + o, max - o, parts[i]);
        o += strlen(out + o);
    }
    return true;
}

/* A file: URL as an FSSpec, if the file is there. */
static int16_t spec_of_file_url(const char *url, gaddr spec)
{
    if (strncasecmp(url, "file:", 5))
        return paramErr;
    const char *p = url + 5;
    if (p[0] == '/' && p[1] == '/') {
        p += 2;
        if (!strncasecmp(p, "localhost", 9))
            p += 9;
    }
    while (*p == '/')
        p++;
    char hfs[1024];
    size_t o = 0;
    for (; *p && *p != '#' && *p != '?' && o + 2 < sizeof hfs; p++) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hex[3] = { p[1], p[2], 0 };
            hfs[o++] = (char)strtol(hex, NULL, 16);
            p += 2;
        } else {
            hfs[o++] = *p == '/' ? ':' : *p;
        }
    }
    hfs[o] = 0;
    if (o == 0 || o > 255)
        return fnfErr;
    gaddr name = tb_NewPtr(256, false, true);
    if (!name)
        return memFullErr;
    wr8(name, (uint8_t)o);
    for (size_t i = 0; i < o; i++)
        wr8(name + 1 + (uint32_t)i, (uint8_t)hfs[i]);
    int16_t e = tb_FSMakeFSSpec(0, 0, name, spec);
    tb_DisposePtr(name);
    return e;
}

/* Find the file a link names: a file: URL (or one relative to a page that
 * is a file) by itself, any other by the program's routine. */
static int16_t find_file(hr *h, const char *link, int kind, gaddr spec)
{
    const char *root = h->base ? h->base : h->url;
    char *full = full_url(root, link);
    if (!full)
        return memFullErr;
    int16_t e = fnfErr;
    if (!strncasecmp(full, "file:", 5)) {
        e = spec_of_file_url(full, spec);
    } else if (h->tospec_upp) {
        gaddr r = cstring_for_callback(0, root ? root : ""), l = cstring_for_callback(1, link);
        if (r && l) {
            uint32_t args[5] = { r, l, spec, (uint32_t)kind, h->tospec_ref };
            e = (int16_t)svc_call_upp(h->tospec_upp, 0xEFF0, args, 5);
        }
    }
    free(full);
    return e;
}

static char *read_file(gaddr spec, size_t *size)
{
    char path[1024];
    gaddr name = spec + FSSpec_name_off;
    if (fm_resolve((int16_t)rd16(spec + FSSpec_vRefNum_off), (int32_t)rd32(spec + FSSpec_parID_off), name, path,
                   sizeof path))
        return NULL;
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (buf) {
        *size = fread(buf, 1, (size_t)n, f);
        buf[*size] = 0;
    }
    fclose(f);
    return buf;
}

/* ---------------------------------------------------------------------- */
/* Pictures */

static int image_index(hr *h, const char *src)
{
    for (int i = 0; i < h->nimages; i++)
        if (!strcmp(h->images[i].src, src))
            return i;
    image *v = realloc(h->images, (size_t)(h->nimages + 1) * sizeof *v);
    if (!v)
        return -1;
    h->images = v;
    h->images[h->nimages] = (image){ .src = dups(src) };
    return h->nimages++;
}

static void image_load(hr *h, int idx)
{
    image *im = &h->images[idx];
    if (im->tried)
        return;
    im->tried = true;
    gaddr spec = tb_NewPtr(70, false, true), gi = tb_NewPtr(4, false, true), rect = tb_NewPtr(8, false, true);
    if (spec && gi && rect && !find_file(h, im->src, kHRLookingForImage, spec) &&
        !tb_GetGraphicsImporterForFile(spec, gi) && rd32(gi)) {
        uint32_t a[2] = { rd32(gi), rect };
        if (!os9rest_call(hr_c, "GraphicsImportGetNaturalBounds", a, 2)) {
            im->w = (int16_t)rd16(rect + 6) - (int16_t)rd16(rect + 2);
            im->h = (int16_t)rd16(rect + 4) - (int16_t)rd16(rect);
            im->importer = rd32(gi);
        }
    }
    if (spec)
        tb_DisposePtr(spec);
    if (gi)
        tb_DisposePtr(gi);
    if (rect)
        tb_DisposePtr(rect);
}

static int image_size(hr *h, int idx, int *w, int *hh)
{
    image_load(h, idx);
    *w = h->images[idx].w;
    *hh = h->images[idx].h;
    return h->images[idx].importer != 0;
}

/* ---------------------------------------------------------------------- */
/* The page */

static void page_clear(hr *h)
{
    dl_free(&h->items);
    for (int i = 0; i < h->nlinks; i++)
        free(h->links[i]);
    free(h->links);
    h->links = NULL;
    h->nlinks = 0;
    for (int i = 0; i < h->nanchors; i++)
        free(h->anchors[i].name);
    free(h->anchors);
    h->anchors = NULL;
    h->nanchors = 0;
    h->width_laid = 0;
}

static void images_free(hr *h)
{
    for (int i = 0; i < h->nimages; i++) {
        if (h->images[i].importer)
            components_close(h->images[i].importer);
        free(h->images[i].src);
    }
    free(h->images);
    h->images = NULL;
    h->nimages = 0;
}

static void doc_free(hr *h)
{
    page_clear(h);
    if (h->doc)
        node_free(h->doc);
    h->doc = NULL;
    free(h->html);
    h->html = NULL;
    free(h->base);
    free(h->title);
    h->base = h->title = NULL;
    images_free(h);
}

static node *find_tag(node *n, int tag)
{
    if (n->tag == tag)
        return n;
    for (node *c = n->first; c; c = c->next) {
        node *f = find_tag(c, tag);
        if (f)
            return f;
    }
    return NULL;
}

static void text_of(node *n, char *out, size_t max)
{
    for (node *c = n->first; c; c = c->next)
        if (c->tag == T_TEXT && c->text && strlen(out) + strlen(c->text) + 1 < max)
            strcat(out, c->text);
}

/* The text is in; the title, the base and the colours are taken from it. */
static void doc_set(hr *h, char *html, size_t n)
{
    doc_free(h);
    h->html = html;
    h->doc = parse_html(html, n);
    h->bg = (rgb){ 255, 255, 255 };
    h->fg = (rgb){ 0, 0, 0 };
    h->link = (rgb){ 0, 0, 238 };
    h->vlink = (rgb){ 85, 26, 139 };
    if (!h->doc)
        return;
    node *t = find_tag(h->doc, T_TITLE);
    if (t) {
        char buf[256] = "";
        text_of(t, buf, sizeof buf);
        char *s = buf;
        while (isspace((unsigned char)*s))
            s++;
        size_t len = strlen(s);
        while (len && isspace((unsigned char)s[len - 1]))
            len--;
        h->title = dupn(s, len);
    }
    node *b = find_tag(h->doc, T_BASE);
    if (b && attr_of(b, "href"))
        h->base = dups(attr_of(b, "href"));
    node *body = find_tag(h->doc, T_BODY);
    if (body) {
        rgb c;
        if (parse_color(attr_of(body, "bgcolor"), &c))
            h->bg = c;
        if (parse_color(attr_of(body, "text"), &c))
            h->fg = c;
        if (parse_color(attr_of(body, "link"), &c))
            h->link = c;
        if (parse_color(attr_of(body, "vlink"), &c))
            h->vlink = c;
    }
}

static int rect_w(const hr *h) { return h->rect[3] - h->rect[1]; }
static int rect_h(const hr *h) { return h->rect[2] - h->rect[0]; }

static bool vbar_shown(const hr *h)
{
    return h->vbar == eHRScrollbarOn || (h->vbar == eHRScrollbarAuto && h->doc_h > rect_h(h));
}

static void with_port_begin(hr *h, gaddr *saved)
{
    tb_GetPort(scratch + 96);
    *saved = rd32(scratch + 96);
    if (h->port)
        tb_SetPort(h->port);
}

static void layout_pass(hr *h, int width)
{
    page_clear(h);
    node *body = h->doc ? find_tag(h->doc, T_BODY) : NULL;
    node *start = body ? body : h->doc;
    lay l;
    style st = { .font = F_TIMES, .size = 12, .face = 0, .color = h->fg, .link = -1, .fsize = 3 };
    lay_init(&l, h, 8, width - 8, &st);
    l.y = 8;
    if (start)
        lay_children(&l, start);
    flush_line(&l, false);
    h->doc_h = l.y + 8;
    h->doc_w = l.maxx + 8; /* the page's own width, which may be less than the rectangle's */
    dl_move(&h->items, &l.out, 0, 0);
    lay_free(&l);
    h->width_laid = width;
}

/* The page is laid out for the rectangle's width (less the scroll bar). */
static void ensure_layout(hr *h)
{
    if (!scratch_ok())
        return;
    int w = rect_w(h);
    if (h->width_laid && h->width_laid == w - (vbar_shown(h) ? 16 : 0))
        return;
    gaddr saved;
    with_port_begin(h, &saved);
    int ts = (int16_t)rd16(h->port ? h->port + GrafPort_txSize_off : 0);
    (void)ts;
    h->doc_h = 0;
    layout_pass(h, w > 40 ? w : 40);
    if (h->vbar == eHRScrollbarAuto && h->doc_h > rect_h(h))
        layout_pass(h, (w > 56 ? w : 56) - 16);
    else if (h->vbar == eHRScrollbarOn)
        layout_pass(h, (w > 56 ? w : 56) - 16);
    tb_SetPort(saved);
    int max = h->doc_h - rect_h(h);
    if (h->scroll_v > max)
        h->scroll_v = max > 0 ? max : 0;
}

/* ---------------------------------------------------------------------- */
/* Drawing */

static void draw_items(hr *h, int ox, int oy, const int16_t clip[4])
{
    for (int i = 0; i < h->items.n; i++) {
        ditem *it = &h->items.v[i];
        int x = ox + it->x, y = oy + it->y;
        int top = it->kind == D_TEXT ? y - it->h : y;
        int hh = it->kind == D_TEXT ? it->h + 4 : it->h;
        if (top > clip[2] || top + hh + 2 < clip[0])
            continue;
        switch (it->kind) {
        case D_TEXT: {
            port_text(it->font, it->size, it->face & ~FACE_UNDERLINE);
            set_rgb(it->link >= 0 ? it->color : it->color, true);
            for (int k = 0; k < it->len && k < 1000; k++)
                wr8(tbuf + (uint32_t)k, (uint8_t)it->text[k]);
            tb_MoveTo((int16_t)x, (int16_t)y);
            tb_DrawText(tbuf, 0, (int16_t)(it->len > 1000 ? 1000 : it->len));
            if (it->under) {
                tb_MoveTo((int16_t)x, (int16_t)(y + 1));
                tb_LineTo((int16_t)(x + it->w - 1), (int16_t)(y + 1));
            }
            if (it->strike) {
                tb_MoveTo((int16_t)x, (int16_t)(y - it->size / 3));
                tb_LineTo((int16_t)(x + it->w - 1), (int16_t)(y - it->size / 3));
            }
            break;
        }
        case D_FILL:
            set_rgb(it->color, true);
            set_rect(x, y, x + it->w, y + it->h);
            tb_PaintRect(scratch);
            break;
        case D_FRAME: {
            int t = it->shape > 0 ? it->shape : 1;
            if (it->shape == 1 && it->image < 0 && it->color.r == it->color.g) {
                /* an input field: white inside */
            }
            set_rgb(it->color, true);
            tb_PenSize((int16_t)t, (int16_t)t);
            set_rect(x, y, x + it->w, y + it->h);
            tb_FrameRect(scratch);
            tb_PenSize(1, 1);
            break;
        }
        case D_ROUND:
            set_rgb((rgb){ 0xDD, 0xDD, 0xDD }, true);
            set_rect(x, y, x + it->w, y + it->h);
            tb_PaintRect(scratch);
            set_rgb(it->color, true);
            tb_FrameRoundRect(scratch, 8, 8);
            break;
        case D_OVAL:
            set_rgb(it->color, true);
            set_rect(x, y, x + 12, y + 12);
            tb_FrameOval(scratch);
            break;
        case D_RULE:
            if (it->shape) {
                set_rgb((rgb){ 0x80, 0x80, 0x80 }, true);
                set_rect(x, y, x + it->w, y + it->h);
                tb_PaintRect(scratch);
            } else {
                set_rgb((rgb){ 0x80, 0x80, 0x80 }, true);
                set_rect(x, y, x + it->w, y + 1);
                tb_PaintRect(scratch);
                set_rect(x, y, x + 1, y + it->h);
                tb_PaintRect(scratch);
                set_rgb((rgb){ 0xE0, 0xE0, 0xE0 }, true);
                set_rect(x, y + it->h - 1, x + it->w, y + it->h);
                tb_PaintRect(scratch);
                set_rect(x + it->w - 1, y, x + it->w, y + it->h);
                tb_PaintRect(scratch);
            }
            break;
        case D_BULLET:
            set_rgb(it->color, true);
            set_rect(x, y, x + 5, y + 5);
            if (it->shape == 1)
                tb_FrameOval(scratch);
            else if (it->shape == 2)
                tb_PaintRect(scratch);
            else
                tb_PaintOval(scratch);
            break;
        case D_IMAGE:
            if (it->image >= 0 && h->images[it->image].importer) {
                gaddr gw;
                tb_GetPort(scratch + 100);
                gw = rd32(scratch + 100);
                set_rect(x, y, x + it->w, y + it->h);
                uint32_t a[2] = { h->images[it->image].importer, scratch };
                os9rest_call(hr_c, "GraphicsImportSetBoundsRect", a, 2);
                uint32_t b[3] = { h->images[it->image].importer, gw, 0 };
                os9rest_call(hr_c, "GraphicsImportSetGWorld", b, 3);
                uint32_t d[1] = { h->images[it->image].importer };
                os9rest_call(hr_c, "GraphicsImportDraw", d, 1);
            }
            break;
        }
    }
}

static void draw_scrollbar(hr *h, int left, int top, int right, int bottom)
{
    int x = right - 16;
    set_rgb((rgb){ 0xDD, 0xDD, 0xDD }, true);
    set_rect(x, top, right, bottom);
    tb_PaintRect(scratch);
    set_rgb((rgb){ 0, 0, 0 }, true);
    tb_FrameRect(scratch);
    set_rect(x, top, right, top + 16);
    tb_FrameRect(scratch);
    set_rect(x, bottom - 16, right, bottom);
    tb_FrameRect(scratch);
    (void)left;
    int track = bottom - top - 32, range = h->doc_h - rect_h(h);
    if (range > 0 && h->active) {
        int th = track * rect_h(h) / h->doc_h;
        if (th < 12)
            th = 12;
        int ty = top + 16 + (track - th) * h->scroll_v / range;
        set_rgb((rgb){ 0x99, 0x99, 0xCC }, true);
        set_rect(x + 1, ty, right - 1, ty + th);
        tb_PaintRect(scratch);
        set_rgb((rgb){ 0, 0, 0 }, true);
        tb_FrameRect(scratch);
    }
}

/* Draw the page into a port: in the rectangle, clipped to the region. */
static int32_t hr_draw(hr *h, gaddr update, gaddr port)
{
    if (!scratch_ok())
        return memFullErr;
    gaddr target = port ? port : h->port;
    if (!target)
        return paramErr;
    gaddr saved;
    tb_GetPort(scratch + 96);
    saved = rd32(scratch + 96);
    gaddr keep_port = h->port;
    h->port = target;
    ensure_layout(h);
    h->port = keep_port;
    tb_SetPort(target);
    /* the port's text, pen and clip kept */
    int16_t font = (int16_t)rd16(target + GrafPort_txFont_off), size = (int16_t)rd16(target + GrafPort_txSize_off),
            face = (int16_t)rd16(target + GrafPort_txFace_off);
    tb_GetForeColor(scratch + 104);
    uint16_t fg[3] = { rd16(scratch + 104), rd16(scratch + 106), rd16(scratch + 108) };
    gaddr oldclip = tb_NewRgn(), clip = tb_NewRgn(), box = tb_NewRgn();
    tb_GetClip(oldclip);
    set_rect(h->rect[1], h->rect[0], h->rect[3], h->rect[2]);
    tb_RectRgn(clip, scratch);
    tb_SectRgn(oldclip, clip, clip);
    if (update)
        tb_SectRgn(clip, update, clip);
    int left = h->rect[1], top = h->rect[0], right = h->rect[3], bottom = h->rect[2];
    int inset = h->border ? 1 : 0;
    bool bar = vbar_shown(h);
    set_rect(left + inset, top + inset, right - inset - (bar ? 16 : 0), bottom - inset);
    tb_RectRgn(box, scratch);
    tb_SectRgn(clip, box, box);
    tb_SetClip(box);
    set_rgb(h->bg, true);
    set_rect(left, top, right - (bar ? 16 : 0), bottom);
    tb_PaintRect(scratch);
    int16_t clip_box[4] = { (int16_t)(top - 1), (int16_t)left, (int16_t)(bottom + 1), (int16_t)right };
    draw_items(h, left - h->scroll_h, top - h->scroll_v, clip_box);
    tb_SetClip(clip);
    if (bar)
        draw_scrollbar(h, left, top, right, bottom);
    if (h->border) {
        set_rgb((rgb){ 0, 0, 0 }, true);
        set_rect(left, top, right, bottom);
        tb_FrameRect(scratch);
    }
    tb_SetClip(oldclip);
    tb_DisposeRgn(oldclip);
    tb_DisposeRgn(clip);
    tb_DisposeRgn(box);
    port_text(font, size, face);
    wr16(scratch + 64, fg[0]);
    wr16(scratch + 66, fg[1]);
    wr16(scratch + 68, fg[2]);
    tb_RGBForeColor(scratch + 64);
    tb_SetPort(saved);
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* Going to pages */

static int32_t hr_load_spec(hr *h, gaddr spec)
{
    size_t n = 0;
    char *html = read_file(spec, &n);
    if (!html)
        return fnfErr;
    char url[1100];
    char *u = NULL;
    if (url_of_spec(spec, url, sizeof url))
        u = dups(url);
    doc_set(h, html, n);
    free(h->url);
    h->url = u;
    gget(h->spec, spec, 70);
    h->have_spec = true;
    h->scroll_h = h->scroll_v = 0;
    return h->doc ? noErr : memFullErr;
}

static int32_t hr_refresh(hr *h, bool force)
{
    if (force && h->port)
        return hr_draw(h, 0, 0);
    return noErr;
}

static int32_t hr_go_file(hr *h, gaddr spec, bool force)
{
    int32_t e = hr_load_spec(h, spec);
    return e ? e : hr_refresh(h, force);
}

static int anchor_y(hr *h, const char *name)
{
    ensure_layout(h);
    for (int i = 0; i < h->nanchors; i++)
        if (!strcmp(h->anchors[i].name, name))
            return h->anchors[i].y;
    return -1;
}

static void scroll_to(hr *h, int hpos, int vpos)
{
    int maxv = h->doc_h - rect_h(h), maxh = h->doc_w - rect_w(h);
    h->scroll_v = vpos < 0 ? 0 : vpos > maxv ? (maxv > 0 ? maxv : 0) : vpos;
    h->scroll_h = hpos < 0 ? 0 : hpos > maxh ? (maxh > 0 ? maxh : 0) : hpos;
}

static int32_t hr_go_url(hr *h, const char *url, bool force)
{
    const char *root = h->base ? h->base : h->url;
    char *full = full_url(root, url);
    if (!full)
        return memFullErr;
    int32_t e = noErr;
    const char *hash = strchr(url, '#');
    if (*url == '#' && h->doc) {
        int y = anchor_y(h, url + 1);
        if (y >= 0)
            scroll_to(h, 0, y);
        else
            e = fnfErr;
        free(full);
        return e ? e : hr_refresh(h, force);
    }
    gaddr spec = tb_NewPtr(70, false, true);
    if (!spec) {
        free(full);
        return memFullErr;
    }
    e = find_file(h, url, kHRLookingForHTMLSource, spec);
    if (!e)
        e = hr_load_spec(h, spec);
    if (!e) {
        if (hash && full) {
            int y = anchor_y(h, hash + 1);
            if (y >= 0)
                scroll_to(h, 0, y);
        }
        e = hr_refresh(h, force);
    }
    tb_DisposePtr(spec);
    free(full);
    return e;
}

/* ---------------------------------------------------------------------- */
/* Events: a click on a link or a scroll bar */

static const char *link_at(hr *h, int x, int y)
{
    for (int i = h->items.n - 1; i >= 0; i--) {
        ditem *it = &h->items.v[i];
        if (it->link < 0 || it->link >= h->nlinks || (it->kind != D_TEXT && it->kind != D_IMAGE))
            continue;
        int top = it->kind == D_TEXT ? it->y - it->h * 3 / 4 : it->y, bot = it->kind == D_TEXT ? it->y + it->h / 4 : it->y + it->h;
        if (x >= it->x && x < it->x + (it->kind == D_TEXT ? it->w : it->w) && y >= top && y < bot)
            return h->links[it->link];
    }
    return NULL;
}

static bool click(hr *h, int px, int py)
{
    int left = h->rect[1], top = h->rect[0], right = h->rect[3], bottom = h->rect[2];
    if (px < left || px >= right || py < top || py >= bottom)
        return false;
    ensure_layout(h);
    if (vbar_shown(h) && px >= right - 16) {
        int range = h->doc_h - rect_h(h), line = 16, page = rect_h(h) - 16;
        if (py < top + 16)
            scroll_to(h, h->scroll_h, h->scroll_v - line);
        else if (py >= bottom - 16)
            scroll_to(h, h->scroll_h, h->scroll_v + line);
        else {
            int track = bottom - top - 32;
            int th = track * rect_h(h) / h->doc_h;
            if (th < 12)
                th = 12;
            int ty = top + 16 + (track - th) * (range > 0 ? h->scroll_v : 0) / (range > 0 ? range : 1);
            if (py < ty)
                scroll_to(h, h->scroll_h, h->scroll_v - page);
            else if (py >= ty + th)
                scroll_to(h, h->scroll_h, h->scroll_v + page);
        }
        hr_draw(h, 0, 0);
        return true;
    }
    const char *href = link_at(h, px - left + h->scroll_h, py - top + h->scroll_v);
    if (href) {
        char *url = dups(href);
        const char *root = h->base ? h->base : h->url;
        char *full = full_url(root, href);
        if (h->newurl_upp && full) {
            gaddr u = cstring_for_callback(2, full);
            if (u) {
                uint32_t args[4] = { u, 0, 1, h->newurl_ref };
                svc_call_upp(h->newurl_upp, 0x37F0, args, 4);
            }
        }
        hr_go_url(h, url, true);
        free(url);
        free(full);
    }
    return true;
}

/* ---------------------------------------------------------------------- */
/* The exports */

static void hr_NewReference(ppc_cpu *c)
{
    hr_c = c;
    if (!R(0) || R(1) != 0x68743332)
        RETURN(paramErr);
    wr32(R(0), 0);
    hr *h = calloc(1, sizeof *h);
    if (!h)
        RETURN(memFullErr);
    h->port = R(2);
    if (!h->port) {
        tb_GetPort(R(0));
        h->port = rd32(R(0));
    }
    h->vbar = eHRScrollbarAuto;
    h->active = true;
    h->bg = (rgb){ 255, 255, 255 };
    h->id = ref_new(HR_REF, h);
    wr32(R(0), h->id);
    RETURN(noErr);
}

static void hr_NewReferenceInWindow(ppc_cpu *c)
{
    hr_NewReference(c);
    hr *h = hr_of(rd32(R(0)));
    if (h) {
        h->window = R(2);
        h->port = R(2);
    }
}

#define WITH(h) \
    hr_c = c;   \
    hr *h = hr_of(R(0)); \
    if (!h)     \
        RETURN(paramErr)

static void hr_DisposeReference(ppc_cpu *c)
{
    WITH(h);
    doc_free(h);
    free(h->url);
    ref_free(h->id);
    free(h);
    RETURN(noErr);
}

static void hr_GetHTMLRenderingLibVersion(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    wr32(R(0), 0x01008000); /* 1.0.0 final */
    RETURN(noErr);
}

/* HRFreeMemory(bytesNeeded): the pictures kept open are given up. */
static void hr_FreeMemory(ppc_cpu *c)
{
    hr_c = c;
    int32_t freed = 0;
    for (gaddr id = ref_next(HR_REF, 0); id; id = ref_next(HR_REF, id)) {
        hr *h = hr_of(id);
        for (int i = 0; h && i < h->nimages; i++)
            if (h->images[i].importer) {
                freed += h->images[i].w * h->images[i].h * 4;
                components_close(h->images[i].importer);
                h->images[i].importer = 0;
                h->images[i].tried = false;
            }
    }
    RETURN(freed);
}

static void hr_ScreenConfigurationChanged(ppc_cpu *c)
{
    for (gaddr id = ref_next(HR_REF, 0); id; id = ref_next(HR_REF, id)) {
        hr *h = hr_of(id);
        if (h)
            h->width_laid = 0;
    }
    c->pc = c->lr;
}

/* HRIsHREvent(event): a mouse-down on one of the pages is its own. */
static void hr_IsHREvent(ppc_cpu *c)
{
    hr_c = c;
    if (!R(0) || !scratch_ok() || rd16(R(0)) != 1)
        RETURN(0);
    uint32_t where = rd32(R(0) + 10);
    for (gaddr id = ref_next(HR_REF, 0); id; id = ref_next(HR_REF, id)) {
        hr *h = hr_of(id);
        if (!h || !h->port)
            continue;
        gaddr saved;
        with_port_begin(h, &saved);
        wr32(scratch + 112, where);
        tb_GlobalToLocal(scratch + 112);
        int v = (int16_t)rd16(scratch + 112), x = (int16_t)rd16(scratch + 114);
        bool hit = click(h, x, v);
        tb_SetPort(saved);
        if (hit)
            RETURN(1);
    }
    RETURN(0);
}

static void hr_SetGrafPtr(ppc_cpu *c)
{
    WITH(h);
    h->port = R(1);
    h->width_laid = 0;
    RETURN(noErr);
}

static void hr_SetWindowRef(ppc_cpu *c)
{
    WITH(h);
    h->window = R(1);
    h->port = R(1);
    h->width_laid = 0;
    RETURN(noErr);
}

static void hr_SetEmbeddingControl(ppc_cpu *c)
{
    WITH(h);
    h->embedding = R(1);
    RETURN(noErr);
}

static void hr_Activate(ppc_cpu *c)
{
    WITH(h);
    h->active = true;
    RETURN(noErr);
}

static void hr_Deactivate(ppc_cpu *c)
{
    WITH(h);
    h->active = false;
    RETURN(noErr);
}

static void hr_Draw(ppc_cpu *c)
{
    WITH(h);
    RETURN(hr_draw(h, R(1), 0));
}

static void hr_DrawInPort(ppc_cpu *c)
{
    WITH(h);
    RETURN(hr_draw(h, R(1), R(2)));
}

static void hr_SetRenderingRect(ppc_cpu *c)
{
    WITH(h);
    if (!R(1))
        RETURN(paramErr);
    for (int i = 0; i < 4; i++)
        h->rect[i] = (int16_t)rd16(R(1) + 2u * (unsigned)i);
    h->width_laid = 0;
    RETURN(noErr);
}

static void image_size_out(hr *h, int *w, int *hh)
{
    ensure_layout(h);
    *w = h->doc_w;
    *hh = h->doc_h;
}

static void hr_GetRenderedImageSize(ppc_cpu *c)
{
    WITH(h);
    if (!R(1))
        RETURN(paramErr);
    int w, hh;
    image_size_out(h, &w, &hh);
    wr16(R(1), (uint16_t)hh); /* Point: v then h */
    wr16(R(1) + 2, (uint16_t)w);
    RETURN(noErr);
}

static void hr_GetRenderedImageSize32(ppc_cpu *c)
{
    WITH(h);
    if (!R(1) || !R(2))
        RETURN(paramErr);
    int w, hh;
    image_size_out(h, &w, &hh);
    wr32(R(1), (uint32_t)hh);
    wr32(R(2), (uint32_t)w);
    RETURN(noErr);
}

static void hr_ScrollToLocation(ppc_cpu *c)
{
    WITH(h);
    if (!R(1))
        RETURN(paramErr);
    ensure_layout(h);
    scroll_to(h, (int16_t)rd16(R(1) + 2), (int16_t)rd16(R(1)));
    RETURN(hr_refresh(h, true));
}

static void hr_ScrollToImageLocation32(ppc_cpu *c)
{
    WITH(h);
    ensure_layout(h);
    scroll_to(h, (int32_t)R(1), (int32_t)R(2));
    RETURN(hr_refresh(h, true));
}

static void hr_ForceQuickdraw(ppc_cpu *c)
{
    WITH(h);
    h->force_qd = (R(1) & 0xFF) != 0;
    RETURN(noErr);
}

static void hr_SetScrollbarState(ppc_cpu *c)
{
    WITH(h);
    if ((int16_t)R(1) < 0 || (int16_t)R(1) > 2 || (int16_t)R(2) < 0 || (int16_t)R(2) > 2)
        RETURN(paramErr);
    h->hbar = (int16_t)R(1);
    h->vbar = (int16_t)R(2);
    h->width_laid = 0;
    RETURN(noErr);
}

static void hr_SetDrawBorder(ppc_cpu *c)
{
    WITH(h);
    h->border = (R(1) & 0xFF) != 0;
    RETURN(noErr);
}

static void hr_SetGrowboxCutout(ppc_cpu *c)
{
    WITH(h);
    h->cutout = (R(1) & 0xFF) != 0;
    RETURN(noErr);
}

static void hr_GoToFile(ppc_cpu *c)
{
    WITH(h);
    if (!R(1))
        RETURN(paramErr);
    RETURN(hr_go_file(h, R(1), (R(3) & 0xFF) != 0));
}

static char *cstr_at(gaddr p)
{
    size_t n = 0;
    while (rd8(p + (uint32_t)n) && n < 4096)
        n++;
    char *s = malloc(n + 1);
    if (s) {
        for (size_t i = 0; i < n; i++)
            s[i] = (char)rd8(p + (uint32_t)i);
        s[n] = 0;
    }
    return s;
}

static void hr_GoToURL(ppc_cpu *c)
{
    WITH(h);
    if (!R(1))
        RETURN(paramErr);
    char *u = cstr_at(R(1));
    if (!u)
        RETURN(memFullErr);
    int32_t e = hr_go_url(h, u, (R(3) & 0xFF) != 0);
    free(u);
    RETURN(e);
}

static void hr_GoToAnchor(ppc_cpu *c)
{
    WITH(h);
    if (!R(1))
        RETURN(paramErr);
    char *name = cstr_at(R(1));
    int y = name ? anchor_y(h, name) : -1;
    free(name);
    if (y < 0)
        RETURN(fnfErr);
    scroll_to(h, 0, y);
    RETURN(hr_refresh(h, true));
}

static int32_t go_buffer(hr *h, const uint8_t *src, size_t n, bool force)
{
    char *html = malloc(n + 1);
    if (!html)
        return memFullErr;
    memcpy(html, src, n);
    html[n] = 0;
    doc_set(h, html, n);
    h->have_spec = false;
    h->scroll_h = h->scroll_v = 0;
    return hr_refresh(h, force);
}

static void hr_GoToPtr(ppc_cpu *c)
{
    WITH(h);
    if (!R(1))
        RETURN(paramErr);
    uint8_t *tmp = malloc(R(2) + 1);
    if (!tmp)
        RETURN(memFullErr);
    gget(tmp, R(1), R(2));
    int32_t e = go_buffer(h, tmp, R(2), (R(4) & 0xFF) != 0);
    free(tmp);
    RETURN(e);
}

/* Handles with a string in them: the text and its NUL. */
static int32_t put_handle(gaddr hnd, const char *s)
{
    if (!hnd)
        return paramErr;
    size_t n = s ? strlen(s) : 0;
    tb_SetHandleSize(hnd, (int32_t)n + 1);
    if (!rd32(hnd) || (int16_t)rd16(LM_MemErr))
        return memFullErr;
    for (size_t i = 0; i < n; i++)
        wr8(rd32(hnd) + (uint32_t)i, (uint8_t)s[i]);
    wr8(rd32(hnd) + (uint32_t)n, 0);
    return noErr;
}

static void hr_GetRootURL(ppc_cpu *c)
{
    WITH(h);
    RETURN(put_handle(R(1), h->base ? h->base : h->url));
}

static void hr_GetBaseURL(ppc_cpu *c)
{
    WITH(h);
    RETURN(put_handle(R(1), h->base));
}

static void hr_GetHTMLURL(ppc_cpu *c)
{
    WITH(h);
    RETURN(put_handle(R(1), h->url));
}

static void hr_GetTitle(ppc_cpu *c)
{
    WITH(h);
    if (!R(1))
        RETURN(paramErr);
    size_t n = h->title ? strlen(h->title) : 0;
    if (n > 255)
        n = 255;
    wr8(R(1), (uint8_t)n);
    for (size_t i = 0; i < n; i++)
        wr8(R(1) + 1 + (uint32_t)i, (uint8_t)h->title[i]);
    RETURN(noErr);
}

static void hr_GetHTMLFile(ppc_cpu *c)
{
    WITH(h);
    if (!R(1))
        RETURN(paramErr);
    if (!h->have_spec)
        RETURN(fnfErr);
    gput(R(1), h->spec, 70);
    RETURN(noErr);
}

static void hr_UtilCreateFullURL(ppc_cpu *c)
{
    if (!R(0) || !R(1) || !R(2))
        RETURN(paramErr);
    char *root = cstr_at(R(0)), *link = cstr_at(R(1));
    char *full = root && link ? full_url(root, link) : NULL;
    int32_t e = full ? put_handle(R(2), full) : memFullErr;
    free(root);
    free(link);
    free(full);
    RETURN(e);
}

static void hr_UtilGetFSSpecFromURL(ppc_cpu *c)
{
    if (!R(0) || !R(1) || !R(2))
        RETURN(paramErr);
    char *root = cstr_at(R(0)), *link = cstr_at(R(1));
    char *full = root && link ? full_url(root, link) : NULL;
    int32_t e = full ? spec_of_file_url(full, R(2)) : memFullErr;
    free(root);
    free(link);
    free(full);
    RETURN(e);
}

static void hr_UtilGetURLFromFSSpec(ppc_cpu *c)
{
    if (!R(0) || !R(1))
        RETURN(paramErr);
    char url[1100];
    if (!url_of_spec(R(0), url, sizeof url))
        RETURN(fnfErr);
    RETURN(put_handle(R(1), url));
}

/* Callbacks: kept with the reference they were registered for. */
static void hr_RegisterWasURLVisitedUPP(ppc_cpu *c)
{
    hr_c = c;
    hr *h = hr_of(R(1));
    if (h) {
        h->visited_upp = R(0);
        h->visited_ref = R(2);
    }
    c->pc = c->lr;
}

static void hr_UnregisterWasURLVisitedUPP(ppc_cpu *c)
{
    hr *h = hr_of(R(0));
    if (h)
        h->visited_upp = h->visited_ref = 0;
    c->pc = c->lr;
}

static void hr_RegisterNewURLUPP(ppc_cpu *c)
{
    hr *h = hr_of(R(1));
    if (h) {
        h->newurl_upp = R(0);
        h->newurl_ref = R(2);
    }
    c->pc = c->lr;
}

static void hr_UnregisterNewURLUPP(ppc_cpu *c)
{
    hr *h = hr_of(R(0));
    if (h)
        h->newurl_upp = h->newurl_ref = 0;
    c->pc = c->lr;
}

static void hr_RegisterURLToFSSpecUPP(ppc_cpu *c)
{
    hr *h = hr_of(R(1));
    if (h) {
        h->tospec_upp = R(0);
        h->tospec_ref = R(2);
    }
    c->pc = c->lr;
}

static void hr_UnregisterURLToFSSpecUPP(ppc_cpu *c)
{
    hr *h = hr_of(R(0));
    if (h)
        h->tospec_upp = h->tospec_ref = 0;
    c->pc = c->lr;
}

/* ---- CarbonLib's CFString, CFURL, CFData and FSRef forms ---- */

/* A CFString's text as a C string (MacRoman bytes), or NULL. */
static char *cf_to_cstr(gaddr cf)
{
    if (!cf)
        return NULL;
    uint16_t u[1024];
    uint32_t n = svc_cfstring_get(cf, u, 1023);
    char *s = malloc(n + 1);
    if (!s)
        return NULL;
    for (uint32_t i = 0; i < n; i++)
        s[i] = u[i] < 0x80 ? (char)u[i] : (char)latin1_to_mac(u[i]);
    s[n] = 0;
    return s;
}

static gaddr cf_from_cstr(const char *s)
{
    uint16_t u[1024];
    uint32_t n = 0;
    for (; s && s[n] && n < 1023; n++)
        u[n] = (uint8_t)s[n];
    return svc_cfstring(u, n);
}

static gaddr cfurl_from_cstr(ppc_cpu *c, const char *s)
{
    gaddr str = cf_from_cstr(s);
    if (!str)
        return 0;
    uint32_t a[3] = { 0, str, 0 };
    gaddr url = os9rest_call(c, "CFURLCreateWithString", a, 3);
    svc_cfrelease(str);
    return url;
}

static char *cfurl_to_cstr(ppc_cpu *c, gaddr url)
{
    uint32_t a[1] = { url };
    return cf_to_cstr(os9rest_call(c, "CFURLGetString", a, 1));
}

static void hr_GoToCFURL(ppc_cpu *c)
{
    WITH(h);
    char *u = cfurl_to_cstr(c, R(1));
    if (!u)
        RETURN(paramErr);
    int32_t e = hr_go_url(h, u, (R(3) & 0xFF) != 0);
    free(u);
    RETURN(e);
}

static void hr_GoToAnchorCFString(ppc_cpu *c)
{
    WITH(h);
    char *name = cf_to_cstr(R(1));
    int y = name ? anchor_y(h, name) : -1;
    free(name);
    if (y < 0)
        RETURN(fnfErr);
    scroll_to(h, 0, y);
    RETURN(hr_refresh(h, true));
}

static void hr_GoToFSRef(ppc_cpu *c)
{
    WITH(h);
    if (!R(1))
        RETURN(paramErr);
    gaddr spec = tb_NewPtr(70, false, true);
    if (!spec)
        RETURN(memFullErr);
    int16_t e = tb_FSGetCatalogInfo(R(1), 0, 0, 0, spec, 0);
    int32_t r = e ? e : hr_go_file(h, spec, (R(3) & 0xFF) != 0);
    tb_DisposePtr(spec);
    RETURN(r);
}

static void hr_GoToData(ppc_cpu *c)
{
    WITH(h);
    if (!R(1))
        RETURN(paramErr);
    uint32_t a[1] = { R(1) };
    gaddr bytes = os9rest_call(c, "CFDataGetBytePtr", a, 1);
    uint32_t n = os9rest_call(c, "CFDataGetLength", a, 1);
    uint8_t *tmp = malloc(n + 1);
    if (!tmp)
        RETURN(memFullErr);
    gget(tmp, bytes, n);
    int32_t e = go_buffer(h, tmp, n, (R(3) & 0xFF) != 0);
    free(tmp);
    RETURN(e);
}

static void out_cfstring(ppc_cpu *c, gaddr out, const char *s)
{
    if (!out)
        RETURN(paramErr);
    wr32(out, s ? cf_from_cstr(s) : 0);
    RETURN(s && !rd32(out) ? memFullErr : noErr);
}

static void hr_GetRootURLAsCFString(ppc_cpu *c)
{
    WITH(h);
    out_cfstring(c, R(1), h->base ? h->base : h->url);
}

static void hr_GetBaseURLAsCFString(ppc_cpu *c)
{
    WITH(h);
    out_cfstring(c, R(1), h->base);
}

static void hr_GetTitleAsCFString(ppc_cpu *c)
{
    WITH(h);
    out_cfstring(c, R(1), h->title);
}

static void hr_GetHTMLURLAsCFURL(ppc_cpu *c)
{
    WITH(h);
    if (!R(1))
        RETURN(paramErr);
    wr32(R(1), h->url ? cfurl_from_cstr(c, h->url) : 0);
    RETURN(h->url ? noErr : fnfErr);
}

static void hr_GetHTMLFileAsFSRef(ppc_cpu *c)
{
    WITH(h);
    if (!R(1))
        RETURN(paramErr);
    if (!h->have_spec)
        RETURN(fnfErr);
    gaddr spec = tb_NewPtr(70, false, true);
    if (!spec)
        RETURN(memFullErr);
    gput(spec, h->spec, 70);
    int16_t e = tb_FSpMakeFSRef(spec, R(1));
    tb_DisposePtr(spec);
    RETURN(e);
}

static void hr_UtilCreateFullCFURL(ppc_cpu *c)
{
    if (!R(0) || !R(1) || !R(2))
        RETURN(paramErr);
    char *root = cf_to_cstr(R(0)), *link = cf_to_cstr(R(1));
    char *full = root && link ? full_url(root, link) : NULL;
    if (full)
        wr32(R(2), cfurl_from_cstr(c, full));
    free(root);
    free(link);
    free(full);
    RETURN(full && rd32(R(2)) ? noErr : memFullErr);
}

static void hr_UtilGetFSRefFromURL(ppc_cpu *c)
{
    if (!R(0) || !R(1) || !R(2))
        RETURN(paramErr);
    char *root = cf_to_cstr(R(0)), *link = cf_to_cstr(R(1));
    char *full = root && link ? full_url(root, link) : NULL;
    gaddr spec = tb_NewPtr(70, false, true);
    int32_t e = full && spec ? spec_of_file_url(full, spec) : memFullErr;
    if (!e)
        e = tb_FSpMakeFSRef(spec, R(2));
    if (spec)
        tb_DisposePtr(spec);
    free(root);
    free(link);
    free(full);
    RETURN(e);
}

static void hr_UtilGetURLFromFSRef(ppc_cpu *c)
{
    if (!R(0) || !R(1))
        RETURN(paramErr);
    gaddr spec = tb_NewPtr(70, false, true);
    if (!spec)
        RETURN(memFullErr);
    char url[1100];
    int16_t e = tb_FSGetCatalogInfo(R(0), 0, 0, 0, spec, 0);
    if (!e && !url_of_spec(spec, url, sizeof url))
        e = fnfErr;
    if (!e)
        wr32(R(1), cfurl_from_cstr(c, url));
    tb_DisposePtr(spec);
    RETURN(e);
}

void os9left_html_reset(void)
{
    for (gaddr id = ref_next(HR_REF, 0); id; id = ref_next(HR_REF, id)) {
        hr *h = hr_of(id);
        if (h) {
            h->images = NULL; /* the importers went with the Component Manager */
            h->nimages = 0;
            doc_free(h);
            free(h->url);
            free(h);
        }
        ref_free(id);
    }
    scratch = tbuf = 0;
    cbuf[0] = cbuf[1] = cbuf[2] = 0;
}

const ppc_export os9left_html_exports[] = {
    { "HRActivate", hr_Activate },
    { "HRDeactivate", hr_Deactivate },
    { "HRDisposeReference", hr_DisposeReference },
    { "HRDraw", hr_Draw },
    { "HRDrawInPort", hr_DrawInPort },
    { "HRForceQuickdraw", hr_ForceQuickdraw },
    { "HRFreeMemory", hr_FreeMemory },
    { "HRGetBaseURL", hr_GetBaseURL },
    { "HRGetHTMLFile", hr_GetHTMLFile },
    { "HRGetHTMLRenderingLibVersion", hr_GetHTMLRenderingLibVersion },
    { "HRGetHTMLURL", hr_GetHTMLURL },
    { "HRGetRenderedImageSize", hr_GetRenderedImageSize },
    { "HRGetRenderedImageSize32", hr_GetRenderedImageSize32 },
    { "HRGetRootURL", hr_GetRootURL },
    { "HRGetTitle", hr_GetTitle },
    { "HRGetBaseURLAsCFString", hr_GetBaseURLAsCFString },
    { "HRGetHTMLFileAsFSRef", hr_GetHTMLFileAsFSRef },
    { "HRGetHTMLURLAsCFURL", hr_GetHTMLURLAsCFURL },
    { "HRGetRootURLAsCFString", hr_GetRootURLAsCFString },
    { "HRGetTitleAsCFString", hr_GetTitleAsCFString },
    { "HRGoToAnchor", hr_GoToAnchor },
    { "HRGoToAnchorCFString", hr_GoToAnchorCFString },
    { "HRGoToCFURL", hr_GoToCFURL },
    { "HRGoToData", hr_GoToData },
    { "HRGoToFSRef", hr_GoToFSRef },
    { "HRUtilCreateFullCFURL", hr_UtilCreateFullCFURL },
    { "HRUtilGetFSRefFromURL", hr_UtilGetFSRefFromURL },
    { "HRUtilGetURLFromFSRef", hr_UtilGetURLFromFSRef },
    { "HRGoToFile", hr_GoToFile },
    { "HRGoToPtr", hr_GoToPtr },
    { "HRGoToURL", hr_GoToURL },
    { "HRIsHREvent", hr_IsHREvent },
    { "HRNewReference", hr_NewReference },
    { "HRNewReferenceInWindow", hr_NewReferenceInWindow },
    { "HRRegisterNewURLUPP", hr_RegisterNewURLUPP },
    { "HRRegisterURLToFSSpecUPP", hr_RegisterURLToFSSpecUPP },
    { "HRRegisterWasURLVisitedUPP", hr_RegisterWasURLVisitedUPP },
    { "HRScreenConfigurationChanged", hr_ScreenConfigurationChanged },
    { "HRScrollToImageLocation32", hr_ScrollToImageLocation32 },
    { "HRScrollToLocation", hr_ScrollToLocation },
    { "HRSetDrawBorder", hr_SetDrawBorder },
    { "HRSetEmbeddingControl", hr_SetEmbeddingControl },
    { "HRSetGrafPtr", hr_SetGrafPtr },
    { "HRSetGrowboxCutout", hr_SetGrowboxCutout },
    { "HRSetRenderingRect", hr_SetRenderingRect },
    { "HRSetScrollbarState", hr_SetScrollbarState },
    { "HRSetWindowRef", hr_SetWindowRef },
    { "HRUnregisterNewURLUPP", hr_UnregisterNewURLUPP },
    { "HRUnregisterURLToFSSpecUPP", hr_UnregisterURLToFSSpecUPP },
    { "HRUnregisterWasURLVisitedUPP", hr_UnregisterWasURLVisitedUPP },
    { "HRUtilCreateFullURL", hr_UtilCreateFullURL },
    { "HRUtilGetFSSpecFromURL", hr_UtilGetFSSpecFromURL },
    { "HRUtilGetURLFromFSSpec", hr_UtilGetURLFromFSSpec },
};
const unsigned os9left_html_export_count = sizeof os9left_html_exports / sizeof os9left_html_exports[0];
