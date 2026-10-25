/*
 * Core Foundation's URLs, URL access, bundles and plug-ins as CarbonLib
 * had them (CFURL.h, CFURLAccess.h, CFBundle.h, CFPlugIn.h).
 *
 * A CFURL is its string and its base, as RFC 1808 has them: the string
 * is kept as a CFString (CFURLGetString gives it) and taken apart when a
 * part is asked for.  A file: URL names a file by its volume's name and
 * the names on the way to it, "file://localhost/Macintosh%20HD/Folder/",
 * so it maps to the File Manager's view of the disk (and the host path
 * behind it); a file system path is POSIX, HFS ("Macintosh HD:Folder:")
 * or Windows style, and the "file system representation" is an HFS path
 * in the system's encoding, as on Mac OS 9.
 *
 * A Carbon program on Mac OS 9 was no bundle: its main bundle is the
 * folder it is in, its executable the application file, its info
 * dictionary the 'plst' 0 resource if it has one, and its resources its
 * own resource fork.  Folders laid out as bundles (Contents/Info.plist,
 * Contents/Resources, Contents/MacOSClassic) are bundles too; their code
 * is a CFM fragment, loaded with the Code Fragment Manager.  A plug-in is
 * a bundle with factories, as CFPlugIn.h describes.
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "carbon_cf.h"
#include "filemgr_internal.h"
#include "forks.h"

extern char g_app_path[];

enum { kCFURLUnknownError = -10, kCFURLUnknownSchemeError = -11, kCFURLResourceNotFoundError = -12,
       kCFURLResourceAccessViolationError = -13, kCFURLImproperArgumentsError = -15,
       kCFURLUnknownPropertyKeyError = -16 };

/* ---------------------------------------------------------------------- */
/* Escapes */

static bool unreserved(uint8_t ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || strchr("-_.!~*'()", ch);
}

/* What may stand in a URL unescaped (RFC 2396's reserved and unreserved, and '#'). */
static bool url_legal(uint16_t ch) { return ch < 128 && ch && (unreserved((uint8_t)ch) || strchr(";/?:@&=+$,#", ch)); }

static void add_escaped_byte(cf_text *t, uint8_t b)
{
    static const char hex[] = "0123456789ABCDEF";
    text_add_unit(t, '%');
    text_add_unit(t, (uint8_t)hex[b >> 4]);
    text_add_unit(t, (uint8_t)hex[b & 15]);
}

/* A path component (UTF-8) escaped for a URL's path: what is neither
 * unreserved nor ":@&=+$," becomes %XX. */
static void add_component(cf_text *t, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t b = (uint8_t)s[i];
        if (b < 128 && (unreserved(b) || strchr(":@&=+$,", b)))
            text_add_unit(t, b);
        else
            add_escaped_byte(t, b);
    }
}

static int hexval(uint16_t ch)
{
    return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
           : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
}

/* Percent escapes replaced (as UTF-8), but not those for characters in
 * keep; false for a bad escape or bytes that aren't UTF-8. */
static bool unescape(const uint16_t *u, uint32_t n, const uint16_t *keep, uint32_t nkeep, cf_text *out)
{
    for (uint32_t i = 0; i < n;) {
        if (u[i] != '%') {
            text_add_unit(out, u[i++]);
            continue;
        }
        /* A run of escapes: its bytes, as UTF-8. */
        uint8_t bytes[256];
        uint32_t nb = 0, at = i;
        while (i < n && u[i] == '%' && nb < sizeof bytes) {
            if (i + 2 >= n || hexval(u[i + 1]) < 0 || hexval(u[i + 2]) < 0)
                return false;
            bytes[nb++] = (uint8_t)(hexval(u[i + 1]) << 4 | hexval(u[i + 2]));
            i += 3;
        }
        uint16_t *chars;
        uint32_t nc;
        if (!cf_decode(kCFStringEncodingUTF8, bytes, nb, &chars, &nc))
            return false;
        /* A character to keep escaped stays as it was written. */
        for (uint32_t k = 0; k < nc; k++) {
            uint16_t ch = chars[k];
            uint32_t len = ch < 0x80 ? 1 : ch < 0x800 ? 2 : (ch & 0xFC00) == 0xD800 ? 4 : (ch & 0xFC00) == 0xDC00 ? 0 : 3;
            bool kept = false;
            for (uint32_t j = 0; j < nkeep; j++)
                kept |= keep[j] == ch;
            if (kept)
                text_add(out, u + at, len * 3);
            else
                text_add_unit(out, ch);
            at += len * 3;
        }
        free(chars);
    }
    return true;
}

/* ---------------------------------------------------------------------- */
/* URL objects */

typedef struct {
    cf_obj o;
    gaddr string, base;
    char *s; /* the string as UTF-8 */
} cf_url;

/* Where the parts of a URL string are (byte offsets; -1 none). */
typedef struct {
    int scheme;              /* the ':' after it */
    int net, net_end;        /* after "//" */
    int path, path_end;
    int param, param_end;    /* after ';' */
    int query, query_end;    /* after '?' */
    int frag, frag_end;      /* after '#' */
    bool decomposable;
} parts;

static void parse(const char *s, parts *p)
{
    int n = (int)strlen(s), i = 0;
    memset(p, -1, sizeof *p);
    p->decomposable = true;
    if (n && ((s[0] | 0x20) >= 'a' && (s[0] | 0x20) <= 'z')) {
        i = 1;
        while (i < n && (((s[i] | 0x20) >= 'a' && (s[i] | 0x20) <= 'z') || (s[i] >= '0' && s[i] <= '9') ||
                         s[i] == '+' || s[i] == '-' || s[i] == '.'))
            i++;
        if (i < n && s[i] == ':')
            p->scheme = i;
    }
    int rest = p->scheme >= 0 ? p->scheme + 1 : 0, end = n;
    const char *hash = strchr(s + rest, '#');
    if (hash) {
        p->frag = (int)(hash - s) + 1;
        p->frag_end = n;
        end = (int)(hash - s);
    }
    if (p->scheme >= 0 && (rest >= end || s[rest] != '/')) {
        p->decomposable = false;
        p->path = rest; /* the resource specifier, whole */
        p->path_end = end;
        return;
    }
    for (i = rest; i < end; i++)
        if (s[i] == '?') {
            p->query = i + 1;
            p->query_end = end;
            end = i;
            break;
        }
    i = rest;
    if (end - rest >= 2 && s[rest] == '/' && s[rest + 1] == '/') {
        p->net = rest + 2;
        i = p->net;
        while (i < end && s[i] != '/')
            i++;
        p->net_end = i;
    }
    p->path = i;
    p->path_end = end;
    for (int k = i; k < end; k++)
        if (s[k] == ';') {
            p->param = k + 1;
            p->param_end = end;
            p->path_end = k;
            break;
        }
}

static cf_url *url_of(gaddr ref) { return cf_get_type(ref, CFT_URL); }

static void url_finalize(cf_obj *o)
{
    cf_url *u = (cf_url *)o;
    cf_release(u->string);
    cf_release(u->base);
}

static void url_dispose(cf_obj *o) { free(((cf_url *)o)->s); }

static bool url_equal(cf_obj *a, cf_obj *b)
{
    cf_url *x = (cf_url *)a, *y = (cf_url *)b;
    return !strcmp(x->s, y->s) && (x->base == y->base || cf_equal(x->base, y->base));
}

static uint32_t url_hash(cf_obj *o) { return cf_hash(((cf_url *)o)->string); }

static void url_describe(cf_obj *o, cf_text *out)
{
    cf_url *u = (cf_url *)o;
    text_add_utf8(out, "string = ");
    text_add_utf8(out, u->s);
    text_add_utf8(out, ", encoding = 134217984, base = ");
    if (u->base)
        cf_describe(u->base, out);
    else
        text_add_utf8(out, "(null)");
}

static const cf_class url_class = { "CFURL", url_finalize, url_dispose, url_equal, url_hash, url_describe };

/* A URL of a string (retained) and base. */
gaddr cfurl_new_string(gaddr alloc, gaddr string, gaddr base)
{
    char *s = cfstr_utf8(string);
    if (!s)
        return 0;
    cf_url *u = cf_new(CFT_URL, sizeof *u, alloc, 0);
    if (!u) {
        free(s);
        return 0;
    }
    u->string = cf_retain(string);
    u->s = s;
    parts p;
    parse(s, &p);
    /* An absolute URL has no base. */
    u->base = p.scheme < 0 && url_of(base) ? cf_retain(base) : 0;
    return u->o.ref;
}

static gaddr url_from_text(gaddr alloc, const cf_text *t, gaddr base)
{
    gaddr s = cfstr_new(alloc, t->u, t->n, false);
    gaddr u = cfurl_new_string(alloc, s, base);
    cf_release(s);
    return u;
}

static gaddr url_from_utf8(gaddr alloc, const char *s, gaddr base)
{
    gaddr str = cfstr_from_utf8(s);
    gaddr u = cfurl_new_string(alloc, str, base);
    cf_release(str);
    return u;
}

/* A part of the string as a new string (escapes intact), or 0. */
static gaddr part_string(cf_url *u, int from, int to)
{
    if (from < 0 || to < from)
        return 0;
    char *t = malloc((size_t)(to - from) + 1);
    if (!t)
        return 0;
    memcpy(t, u->s + from, (size_t)(to - from));
    t[to - from] = 0;
    gaddr s = cfstr_from_utf8(t);
    free(t);
    return s;
}

/* A string with escapes replaced but for keep's characters (NULL: none). */
static gaddr unescaped(gaddr alloc, gaddr str, gaddr keep)
{
    uint32_t n, nk = 0;
    const uint16_t *u = cfstr_chars(str, &n), *k = keep ? cfstr_chars(keep, &nk) : NULL;
    if (!u)
        return 0;
    if (!keep)
        return cf_retain(str);
    cf_text t = { 0 };
    gaddr r = unescape(u, n, k, nk, &t) ? cfstr_new(alloc, t.u, t.n, false) : 0;
    text_free(&t);
    return r;
}

/* RFC 1808's resolution: a relative URL against its base, as a new string. */
static char *resolve(cf_url *u)
{
    cf_url *b = url_of(u->base);
    if (!b)
        return strdup(u->s);
    char *bs = b->base ? resolve(b) : strdup(b->s);
    if (!bs)
        return NULL;
    parts rp, bp;
    parse(u->s, &rp);
    parse(bs, &bp);
    size_t cap = strlen(u->s) + strlen(bs) + 8;
    char *out = calloc(1, cap);
    if (!out) {
        free(bs);
        return NULL;
    }
    if (!u->s[0]) {
        free(out);
        return bs;
    }
#define CAT(str, from, to) strncat(out, (str) + (from), (size_t)((to) - (from)))
    if (bp.scheme >= 0)
        CAT(bs, 0, bp.scheme + 1);
    if (rp.net >= 0) {
        strcat(out, "//");
        CAT(u->s, rp.net, (rp.frag >= 0 ? rp.frag - 1 : (int)strlen(u->s)));
        if (rp.frag >= 0)
            CAT(u->s, rp.frag - 1, rp.frag_end);
        free(bs);
        return out;
    }
    if (bp.net >= 0) {
        strcat(out, "//");
        CAT(bs, bp.net, bp.net_end);
    }
    char *path = NULL;
    bool rel_path = rp.path < rp.path_end;
    if (rel_path && u->s[rp.path] == '/') {
        path = malloc((size_t)(rp.path_end - rp.path) + 1);
        memcpy(path, u->s + rp.path, (size_t)(rp.path_end - rp.path));
        path[rp.path_end - rp.path] = 0;
    } else if (rel_path) {
        /* The base's directory, then the relative path; ./ and x/../ out. */
        int bl = bp.path_end - bp.path, slash = bl;
        while (slash > 0 && bs[bp.path + slash - 1] != '/')
            slash--;
        size_t rl = (size_t)(rp.path_end - rp.path);
        path = malloc((size_t)slash + rl + 2);
        memcpy(path, bs + bp.path, (size_t)slash);
        if (!slash && bp.net >= 0)
            path[slash++] = '/';
        memcpy(path + slash, u->s + rp.path, rl);
        path[slash + (int)rl] = 0;
        /* Segments */
        char *segs[512];
        int ns = 0;
        bool lead = path[0] == '/', trail = false;
        char *save = NULL;
        size_t plen = strlen(path);
        trail = plen && (path[plen - 1] == '/' || (plen >= 2 && !strcmp(path + plen - 2, "/.")) ||
                         (plen >= 3 && !strcmp(path + plen - 3, "/..")) || !strcmp(path, ".") || !strcmp(path, ".."));
        for (char *tok = strtok_r(path, "/", &save); tok && ns < 512; tok = strtok_r(NULL, "/", &save)) {
            if (!strcmp(tok, "."))
                continue;
            if (!strcmp(tok, "..") && ns && strcmp(segs[ns - 1], "..")) {
                ns--;
                continue;
            }
            segs[ns++] = tok;
        }
        char *np = calloc(1, plen + 4);
        if (lead)
            strcat(np, "/");
        for (int k = 0; k < ns; k++) {
            if (k)
                strcat(np, "/");
            strcat(np, segs[k]);
        }
        if (trail && ns)
            strcat(np, "/");
        free(path);
        path = np;
    }
    if (path) {
        strcat(out, path);
        free(path);
        if (rp.param >= 0) {
            strcat(out, ";");
            CAT(u->s, rp.param, rp.param_end);
        }
        if (rp.query >= 0) {
            strcat(out, "?");
            CAT(u->s, rp.query, rp.query_end);
        }
    } else {
        CAT(bs, bp.path, bp.path_end);
        if (rp.param >= 0) {
            strcat(out, ";");
            CAT(u->s, rp.param, rp.param_end);
        } else if (bp.param >= 0) {
            strcat(out, ";");
            CAT(bs, bp.param, bp.param_end);
        }
        if (rp.query >= 0) {
            strcat(out, "?");
            CAT(u->s, rp.query, rp.query_end);
        } else if (bp.query >= 0 && rp.param < 0) { /* new parameters drop the query (RFC 1808, 5a) */
            strcat(out, "?");
            CAT(bs, bp.query, bp.query_end);
        }
    }
    if (rp.frag >= 0) {
        strcat(out, "#");
        CAT(u->s, rp.frag, rp.frag_end);
    }
#undef CAT
    free(bs);
    return out;
}

/* The path's components (unescaped, UTF-8; malloc'd, freed by the caller),
 * and whether it ends with '/'. */
static int path_components(const char *s, const parts *p, char ***out, bool *dir, bool *absolute)
{
    int from = p->path, to = p->path_end, n = 0;
    *out = NULL;
    *dir = to > from && s[to - 1] == '/';
    *absolute = to > from && s[from] == '/';
    if (from < 0)
        return 0;
    char **v = calloc((size_t)(to - from) + 1, sizeof *v);
    int i = from;
    while (v && i < to) {
        while (i < to && s[i] == '/')
            i++;
        int j = i;
        while (j < to && s[j] != '/')
            j++;
        if (j > i) {
            /* Unescape the bytes. */
            char *c = malloc((size_t)(j - i) + 1);
            int k = 0;
            for (int x = i; x < j; x++) {
                if (s[x] == '%' && x + 2 < j && hexval((uint8_t)s[x + 1]) >= 0 && hexval((uint8_t)s[x + 2]) >= 0) {
                    c[k++] = (char)(hexval((uint8_t)s[x + 1]) << 4 | hexval((uint8_t)s[x + 2]));
                    x += 2;
                } else {
                    c[k++] = s[x];
                }
            }
            c[k] = 0;
            v[n++] = c;
        }
        i = j;
    }
    *out = v;
    return n;
}

static void free_components(char **v, int n)
{
    for (int i = 0; i < n; i++)
        free(v[i]);
    free(v);
}

static bool scheme_is(const char *s, const parts *p, const char *name)
{
    return p->scheme == (int)strlen(name) && !strncasecmp(s, name, strlen(name));
}

/* A file: URL (or a relative one) as a path of a style, unescaped. */
static gaddr file_system_path(cf_url *u, uint32_t style)
{
    parts p;
    parse(u->s, &p);
    if (!p.decomposable || (p.scheme >= 0 && !scheme_is(u->s, &p, "file")))
        return 0;
    char **c;
    bool dir, absolute;
    int n = path_components(u->s, &p, &c, &dir, &absolute);
    cf_text t = { 0 };
    if (style == kCFURLHFSPathStyle) {
        if (!absolute)
            text_add_unit(&t, ':');
        for (int i = 0; i < n; i++) {
            if (!strcmp(c[i], "..")) {
                text_add_unit(&t, ':');
                continue;
            }
            if (!strcmp(c[i], "."))
                continue;
            if (i && t.n && t.u[t.n - 1] != ':')
                text_add_unit(&t, ':');
            text_add_utf8(&t, c[i]);
        }
        /* A volume alone is "Name:". */
        if (absolute && n == 1)
            text_add_unit(&t, ':');
    } else if (style == kCFURLWindowsPathStyle) {
        for (int i = 0; i < n; i++) {
            if (i)
                text_add_unit(&t, '\\');
            text_add_utf8(&t, c[i]);
        }
    } else {
        if (absolute)
            text_add_unit(&t, '/');
        for (int i = 0; i < n; i++) {
            if (i)
                text_add_unit(&t, '/');
            text_add_utf8(&t, c[i]);
        }
    }
    free_components(c, n);
    gaddr s = cfstr_new(0, t.u, t.n, false);
    text_free(&t);
    return s;
}

/* The folder a relative file path is taken from: the application's. */
static gaddr working_directory(void)
{
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", g_app_path);
    char *slash = strrchr(dir, '/');
    if (!slash)
        return 0;
    *slash = 0;
    return cfurl_of_host_path(0, dir[0] ? dir : "/");
}

/* A path of a style as a URL. */
static gaddr url_of_path(gaddr alloc, gaddr path, uint32_t style, bool is_dir, gaddr base, bool base_given)
{
    char *s = cfstr_utf8(path);
    if (!s)
        return 0;
    cf_text t = { 0 };
    bool absolute = false;
    char sep = style == kCFURLHFSPathStyle ? ':' : style == kCFURLWindowsPathStyle ? '\\' : '/';
    const char *p = s;
    if (style == kCFURLHFSPathStyle) {
        absolute = s[0] && s[0] != ':' && strchr(s, ':');
        if (s[0] == ':')
            p++;
    } else if (style == kCFURLWindowsPathStyle) {
        absolute = s[0] && s[1] == ':';
    } else {
        absolute = s[0] == '/';
    }
    if (absolute)
        text_add_utf8(&t, "file://localhost/");
    bool last_sep = false;
    for (const char *q = p; *q;) {
        while (*q == sep && style != kCFURLHFSPathStyle)
            q++;
        const char *e = strchr(q, sep);
        size_t len = e ? (size_t)(e - q) : strlen(q);
        if (style == kCFURLHFSPathStyle && len == 0 && e) {
            text_add_utf8(&t, "../"); /* "::" is the parent */
            q = e + 1;
            last_sep = true;
            continue;
        }
        if (len) {
            add_component(&t, q, len);
            last_sep = false;
        }
        if (!e)
            break;
        text_add_unit(&t, '/');
        last_sep = true;
        q = e + 1;
    }
    if (is_dir && !last_sep && !(t.n && t.u[t.n - 1] == '/'))
        text_add_unit(&t, '/');
    if (!is_dir && t.n > 1 && t.u[t.n - 1] == '/' && !(absolute && t.n == 17))
        t.n--;
    free(s);
    gaddr b = absolute ? 0 : base_given ? base : working_directory();
    gaddr u = url_from_text(alloc, &t, b);
    if (!absolute && !base_given)
        cf_release(b);
    text_free(&t);
    return u;
}

/* ---------------------------------------------------------------------- */
/* URLs and the disk */

/* A guest Pascal string for a moment (MacRoman, from UTF-8). */
static gaddr scratch_pstr(const char *utf8)
{
    gaddr p = tb_NewPtr(256, false, true);
    if (!p)
        return 0;
    uint8_t buf[512];
    size_t n = utf8_to_macroman(utf8, buf, sizeof buf);
    if (n > 255)
        n = 255;
    wr8(p, (uint8_t)n);
    gput(p + 1, buf, (uint32_t)n);
    return p;
}

bool cfurl_host_path(gaddr ref, char *out, size_t outsize)
{
    cf_url *u = url_of(ref);
    if (!u)
        return false;
    char *s = resolve(u);
    if (!s)
        return false;
    parts p;
    parse(s, &p);
    if (p.scheme < 0 || !scheme_is(s, &p, "file") || !p.decomposable) {
        free(s);
        return false;
    }
    char **c;
    bool dir, absolute;
    int n = path_components(s, &p, &c, &dir, &absolute);
    free(s);
    bool ok = n > 0;
    char cur[PATH_MAX], next[PATH_MAX];
    if (ok) {
        /* The volume, by name. */
        char vol[300];
        snprintf(vol, sizeof vol, "%s:", c[0]);
        gaddr name = scratch_pstr(vol);
        ok = name && fm_resolve(0, 0, name, cur, sizeof cur) == noErr;
        if (name)
            tb_DisposePtr(name);
    }
    for (int i = 1; ok && i < n; i++) {
        if (!strcmp(c[i], ".") || !strcmp(c[i], ".."))
            continue; /* resolved already */
        gaddr name = scratch_pstr(c[i]);
        int16_t err = name ? fm_resolve(fm_vref_of_path(cur), (int32_t)fm_cat_id(cur), name, next, sizeof next) : -1;
        if (name)
            tb_DisposePtr(name);
        ok = err == noErr || (err == fnfErr && next[0] && i == n - 1);
        if (ok)
            snprintf(cur, sizeof cur, "%s", next);
    }
    free_components(c, n);
    if (ok)
        snprintf(out, outsize, "%s", cur);
    return ok;
}

/* A host path's parent as the File Manager sees it: the Host folder's
 * is the volume it is shown in. */
static bool host_parent(const char *host, char *out, size_t outsize)
{
    if (fm_is_volume_root(host))
        return false;
    if (!strcmp(host, "/")) {
        int16_t vref;
        uint8_t name[32];
        int16_t drive, driver;
        return fm_volume(0, fm_vref_of_path(host), &vref, out, outsize, name, &drive, &driver);
    }
    snprintf(out, outsize, "%s", host);
    char *slash = strrchr(out, '/');
    if (!slash)
        return false;
    if (slash == out)
        slash[1] = 0;
    else
        *slash = 0;
    return true;
}

gaddr cfurl_of_host_path(gaddr alloc, const char *host)
{
    gaddr spec = tb_NewPtr(80, false, true);
    if (!spec)
        return 0;
    char *names[256];
    int n = 0;
    char cur[PATH_MAX], up[PATH_MAX];
    /* The File Manager's spelling of the path (no symbolic links). */
    if (!realpath(host, cur))
        snprintf(cur, sizeof cur, "%s", host);
    bool dir = fm_is_dir(cur);
    for (;;) {
        fm_spec_of_path(cur, spec);
        uint8_t len = rd8(spec + 6);
        uint8_t mac[64];
        gget(mac, spec + 7, len);
        char utf8[256];
        macroman_to_utf8(mac, len, utf8, sizeof utf8);
        if (n < 256)
            names[n++] = strdup(utf8);
        if (!host_parent(cur, up, sizeof up))
            break;
        snprintf(cur, sizeof cur, "%s", up);
    }
    tb_DisposePtr(spec);
    cf_text t = { 0 };
    text_add_utf8(&t, "file://localhost/");
    for (int i = n - 1; i >= 0; i--) {
        add_component(&t, names[i], strlen(names[i]));
        if (i || dir)
            text_add_unit(&t, '/');
        free(names[i]);
    }
    gaddr u = url_from_text(alloc, &t, 0);
    text_free(&t);
    return u;
}

/* A file's data fork, whole, by host path. */
bool cf_read_host_file(const char *path, uint8_t **bytes, size_t *n)
{
    fm_info info;
    if (!fm_host_exists(path) || !fm_info_of(path, &info) || info.dir)
        return false;
    int16_t ref;
    if (fm_open_fork(path, false, 1, &ref) != noErr)
        return false;
    gaddr buf = tb_NewPtr((int32_t)(info.data_len ? info.data_len : 1), false, false);
    uint32_t got = 0;
    bool ok = buf && (!info.data_len || (fm_fork_io(ref, false, 1, 0, info.data_len, buf, &got) == noErr &&
                                         got == info.data_len));
    fm_close_fork(ref);
    if (ok) {
        *bytes = malloc(info.data_len ? info.data_len : 1);
        if (*bytes)
            gget(*bytes, buf, info.data_len);
        *n = info.data_len;
        ok = *bytes != NULL;
    }
    if (buf)
        tb_DisposePtr(buf);
    return ok;
}

/* A file's data fork made the bytes given (the file made if need be). */
bool cf_write_host_file(const char *path, const uint8_t *bytes, size_t n)
{
    int16_t ref;
    if (!fm_host_exists(path) && fm_create_file(path) != noErr)
        return false;
    if (fm_open_fork(path, false, 3, &ref) != noErr)
        return false;
    gaddr buf = tb_NewPtr((int32_t)(n ? n : 1), false, false);
    uint32_t wrote = 0;
    bool ok = buf && fm_fork_set_eof(ref, 1, 0) == noErr;
    if (ok && n) {
        gput(buf, bytes, (uint32_t)n);
        ok = fm_fork_io(ref, true, 1, 0, (uint32_t)n, buf, &wrote) == noErr && wrote == n;
    }
    fm_close_fork(ref);
    if (buf)
        tb_DisposePtr(buf);
    return ok;
}

bool cfurl_read_file(gaddr url, uint8_t **bytes, size_t *n)
{
    char path[PATH_MAX];
    return cfurl_host_path(url, path, sizeof path) && cf_read_host_file(path, bytes, n);
}

/* ---------------------------------------------------------------------- */
/* CFURL calls */

static void cf_CFURLGetTypeID(ppc_cpu *c) { RETURN(CFT_URL); }

/* Whether a string can be a URL's: no spaces or controls, no stray '%'. */
static bool valid_url_string(gaddr str)
{
    uint32_t n;
    const uint16_t *u = cfstr_chars(str, &n);
    if (!u)
        return false;
    for (uint32_t i = 0; i < n; i++) {
        if (u[i] <= 0x20 || u[i] >= 0x7F)
            return false;
        if (u[i] == '%' && (i + 2 >= n || hexval(u[i + 1]) < 0 || hexval(u[i + 2]) < 0))
            return false;
    }
    return true;
}

/* CFURLCreateWithString(allocator, URLString, baseURL) */
static void cf_CFURLCreateWithString(ppc_cpu *c)
{
    if (!valid_url_string(R(1)))
        RETURN(0);
    RETURN(cfurl_new_string(R(0), R(1), R(2)));
}

/* CFURLCreateWithBytes(allocator, URLBytes, length, encoding, baseURL):
 * what isn't 7-bit ASCII escaped in that encoding. */
static void cf_CFURLCreateWithBytes(ppc_cpu *c)
{
    uint32_t len = R(2), enc = R(3);
    if (len > 0x100000 || (!R(1) && len))
        RETURN(0);
    uint8_t *b = malloc(len + 1);
    if (!b)
        RETURN(0);
    gget(b, R(1), len);
    cf_text t = { 0 };
    for (uint32_t i = 0; i < len; i++) {
        if (b[i] >= 0x80 || b[i] <= 0x20)
            add_escaped_byte(&t, b[i]);
        else
            text_add_unit(&t, b[i]);
    }
    (void)enc;
    free(b);
    gaddr u = url_from_text(R(0), &t, R(4));
    text_free(&t);
    RETURN(u);
}

/* CFURLCreateData(allocator, url, encoding, escapeWhitespace) */
static void cf_CFURLCreateData(ppc_cpu *c)
{
    cf_url *u = url_of(R(1));
    if (!u)
        RETURN(0);
    bool esc = (R(3) & 0xFF) != 0;
    uint32_t n;
    const uint16_t *s = cfstr_chars(u->string, &n);
    cf_text t = { 0 };
    for (uint32_t i = 0; i < n; i++) {
        if (esc && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n'))
            add_escaped_byte(&t, (uint8_t)s[i]);
        else
            text_add_unit(&t, s[i]);
    }
    uint8_t *b;
    size_t nb;
    uint32_t used;
    gaddr d = 0;
    if (cf_encode(R(2), t.u, t.n, 0, &b, &nb, &used))
        d = cfdata_new(R(0), b, (uint32_t)nb, false);
    free(b);
    text_free(&t);
    RETURN(d);
}

/* CFURLCreateWithFileSystemPath(allocator, filePath, pathStyle, isDirectory) */
static void cf_CFURLCreateWithFileSystemPath(ppc_cpu *c)
{
    RETURN(url_of_path(R(0), R(1), R(2), (R(3) & 0xFF) != 0, 0, false));
}

/* ...RelativeToBase(allocator, filePath, pathStyle, isDirectory, baseURL) */
static void cf_CFURLCreateWithFileSystemPathRelativeToBase(ppc_cpu *c)
{
    RETURN(url_of_path(R(0), R(1), R(2), (R(3) & 0xFF) != 0, R(4), true));
}

/* The file system's representation: an HFS path in the system's encoding. */
static gaddr fsrep_path(gaddr buf, uint32_t len)
{
    uint8_t *b = malloc(len + 1);
    uint16_t *u;
    uint32_t n;
    gaddr s = 0;
    if (!b)
        return 0;
    gget(b, buf, len);
    uint32_t real = 0;
    while (real < len && b[real])
        real++;
    if (cf_decode(kCFStringEncodingMacRoman, b, real, &u, &n)) {
        s = cfstr_new(0, u, n, false);
        free(u);
    }
    free(b);
    return s;
}

/* CFURLCreateFromFileSystemRepresentation(allocator, buffer, bufLen, isDirectory) */
static void cf_CFURLCreateFromFileSystemRepresentation(ppc_cpu *c)
{
    gaddr path = fsrep_path(R(1), R(2));
    gaddr u = path ? url_of_path(R(0), path, kCFURLHFSPathStyle, (R(3) & 0xFF) != 0, 0, false) : 0;
    cf_release(path);
    RETURN(u);
}

/* ...RelativeToBase(allocator, buffer, bufLen, isDirectory, baseURL) */
static void cf_CFURLCreateFromFileSystemRepresentationRelativeToBase(ppc_cpu *c)
{
    gaddr path = fsrep_path(R(1), R(2));
    gaddr u = path ? url_of_path(R(0), path, kCFURLHFSPathStyle, (R(3) & 0xFF) != 0, R(4), true) : 0;
    cf_release(path);
    RETURN(u);
}

/* CFURLGetFileSystemRepresentation(url, resolveAgainstBase, buffer, maxBufLen) */
static void cf_CFURLGetFileSystemRepresentation(ppc_cpu *c)
{
    cf_url *u = url_of(R(0));
    if (!u || !R(2))
        RETURN(false);
    gaddr abs = 0;
    if ((R(1) & 0xFF) && u->base) {
        char *s = resolve(u);
        abs = s ? url_from_utf8(0, s, 0) : 0;
        free(s);
        u = url_of(abs);
    }
    gaddr path = u ? file_system_path(u, kCFURLHFSPathStyle) : 0;
    cf_release(abs);
    uint32_t n;
    const uint16_t *chars = cfstr_chars(path, &n);
    uint8_t *b = NULL;
    size_t nb = 0;
    uint32_t used;
    bool ok = chars && cf_encode(kCFStringEncodingMacRoman, chars, n, 0, &b, &nb, &used) && nb < R(3);
    if (ok) {
        gput(R(2), b, (uint32_t)nb);
        wr8(R(2) + (uint32_t)nb, 0);
    }
    free(b);
    cf_release(path);
    RETURN(ok);
}

static void cf_CFURLCopyAbsoluteURL(ppc_cpu *c)
{
    cf_url *u = url_of(R(0));
    if (!u)
        RETURN(0);
    if (!u->base)
        RETURN(cf_retain(R(0)));
    char *s = resolve(u);
    gaddr r = s ? url_from_utf8(u->o.alloc, s, 0) : 0;
    free(s);
    RETURN(r);
}

gaddr cfurl_string(gaddr url)
{
    cf_url *u = url_of(url);
    return u ? u->string : 0;
}

static void cf_CFURLGetString(ppc_cpu *c) { RETURN(cfurl_string(R(0))); }

static void cf_CFURLGetBaseURL(ppc_cpu *c)
{
    cf_url *u = url_of(R(0));
    RETURN(u ? u->base : 0);
}

static void cf_CFURLCanBeDecomposed(ppc_cpu *c)
{
    cf_url *u = url_of(R(0));
    parts p;
    if (!u)
        RETURN(false);
    parse(u->s, &p);
    if (p.scheme < 0 && u->base) {
        cf_url *b = url_of(u->base);
        parse(b->s, &p);
    }
    RETURN(p.decomposable);
}

static void cf_CFURLCopyScheme(ppc_cpu *c)
{
    cf_url *u = url_of(R(0));
    while (u) {
        parts p;
        parse(u->s, &p);
        if (p.scheme >= 0)
            RETURN(part_string(u, 0, p.scheme));
        u = url_of(u->base);
    }
    RETURN(0);
}

/* The URL whose net location a URL has: its own, or (when it has neither
 * scheme nor one) its base's. */
static cf_url *net_owner(cf_url *u, parts *p)
{
    while (u) {
        parse(u->s, p);
        if (p->net >= 0 || p->scheme >= 0)
            return p->net >= 0 ? u : NULL;
        u = url_of(u->base);
    }
    return NULL;
}

static void cf_CFURLCopyNetLocation(ppc_cpu *c)
{
    parts p;
    cf_url *u = net_owner(url_of(R(0)), &p);
    RETURN(u && p.decomposable ? part_string(u, p.net, p.net_end) : 0);
}

static void cf_CFURLCopyPath(ppc_cpu *c)
{
    cf_url *u = url_of(R(0));
    parts p;
    if (!u)
        RETURN(0);
    parse(u->s, &p);
    RETURN(p.decomposable ? part_string(u, p.path, p.path_end) : 0);
}

/* CFURLCopyStrictPath(anURL, *isAbsolute) */
static void cf_CFURLCopyStrictPath(ppc_cpu *c)
{
    cf_url *u = url_of(R(0));
    parts p;
    if (!u)
        RETURN(0);
    parse(u->s, &p);
    if (!p.decomposable)
        RETURN(0);
    bool abs = p.path < p.path_end && u->s[p.path] == '/';
    if (R(1))
        wr8(R(1), abs);
    RETURN(part_string(u, p.path + (abs ? 1 : 0), p.path_end));
}

/* CFURLCopyFileSystemPath(anURL, pathStyle) */
static void cf_CFURLCopyFileSystemPath(ppc_cpu *c)
{
    cf_url *u = url_of(R(0));
    RETURN(u ? file_system_path(u, R(1)) : 0);
}

static void cf_CFURLHasDirectoryPath(ppc_cpu *c)
{
    cf_url *u = url_of(R(0));
    parts p;
    if (!u)
        RETURN(false);
    parse(u->s, &p);
    RETURN(p.decomposable && p.path_end > p.path && u->s[p.path_end - 1] == '/');
}

static void cf_CFURLCopyResourceSpecifier(ppc_cpu *c)
{
    cf_url *u = url_of(R(0));
    parts p;
    if (!u)
        RETURN(0);
    parse(u->s, &p);
    if (!p.decomposable)
        RETURN(part_string(u, p.scheme + 1, (int)strlen(u->s)));
    int from = p.param >= 0 ? p.param - 1 : p.query >= 0 ? p.query - 1 : p.frag >= 0 ? p.frag - 1 : -1;
    RETURN(from >= 0 ? part_string(u, from, (int)strlen(u->s)) : 0);
}

/* The net location's pieces: user[:password]@host[:port]. */
static bool net_pieces(cf_url *u, const parts *p, int *user, int *user_end, int *pass, int *pass_end, int *host,
                       int *host_end, int *port)
{
    if (p->net < 0)
        return false;
    const char *s = u->s;
    int at = -1;
    for (int i = p->net; i < p->net_end; i++)
        if (s[i] == '@')
            at = i;
    *user = *pass = *port = -1;
    *host = p->net;
    if (at >= 0) {
        *user = p->net;
        *user_end = at;
        for (int i = p->net; i < at; i++)
            if (s[i] == ':') {
                *user_end = i;
                *pass = i + 1;
                *pass_end = at;
                break;
            }
        *host = at + 1;
    }
    *host_end = p->net_end;
    int bracket = *host < p->net_end && s[*host] == '[';
    for (int i = p->net_end - 1; i >= *host; i--) {
        if (s[i] == ']' && bracket)
            break;
        if (s[i] == ':') {
            *host_end = i;
            *port = i + 1;
            break;
        }
    }
    return true;
}

static void cf_CFURLCopyHostName(ppc_cpu *c)
{
    parts p;
    cf_url *u = net_owner(url_of(R(0)), &p);
    int user, ue, pass, pe, host, he, port;
    if (!u || !net_pieces(u, &p, &user, &ue, &pass, &pe, &host, &he, &port))
        RETURN(0);
    if (he > host && u->s[host] == '[' && u->s[he - 1] == ']')
        host++, he--;
    RETURN(part_string(u, host, he));
}

static void cf_CFURLGetPortNumber(ppc_cpu *c)
{
    parts p;
    cf_url *u = net_owner(url_of(R(0)), &p);
    int user, ue, pass, pe, host, he, port;
    if (!u || !net_pieces(u, &p, &user, &ue, &pass, &pe, &host, &he, &port) || port < 0 || port >= p.net_end)
        RETURN((uint32_t)-1);
    RETURN((uint32_t)atoi(u->s + port));
}

static void cf_CFURLCopyUserName(ppc_cpu *c)
{
    parts p;
    cf_url *u = net_owner(url_of(R(0)), &p);
    int user, ue, pass, pe, host, he, port;
    if (!u || !net_pieces(u, &p, &user, &ue, &pass, &pe, &host, &he, &port) || user < 0)
        RETURN(0);
    gaddr s = part_string(u, user, ue), e = cfstr_constant("");
    gaddr r = unescaped(0, s, e);
    cf_release(s);
    RETURN(r);
}

static void cf_CFURLCopyPassword(ppc_cpu *c)
{
    parts p;
    cf_url *u = net_owner(url_of(R(0)), &p);
    int user, ue, pass, pe, host, he, port;
    if (!u || !net_pieces(u, &p, &user, &ue, &pass, &pe, &host, &he, &port) || pass < 0)
        RETURN(0);
    gaddr s = part_string(u, pass, pe), e = cfstr_constant("");
    gaddr r = unescaped(0, s, e);
    cf_release(s);
    RETURN(r);
}

/* A part, with escapes replaced but for charactersToLeaveEscaped. */
static gaddr part_unescaped(gaddr url, int which, gaddr keep)
{
    cf_url *u = url_of(url);
    parts p;
    if (!u)
        return 0;
    parse(u->s, &p);
    int from = which == 0 ? p.param : which == 1 ? p.query : p.frag;
    int to = which == 0 ? p.param_end : which == 1 ? p.query_end : p.frag_end;
    if (from < 0)
        return 0;
    gaddr s = part_string(u, from, to);
    gaddr r = unescaped(0, s, keep);
    cf_release(s);
    return r;
}

static void cf_CFURLCopyParameterString(ppc_cpu *c) { RETURN(part_unescaped(R(0), 0, R(1))); }
static void cf_CFURLCopyQueryString(ppc_cpu *c) { RETURN(part_unescaped(R(0), 1, R(1))); }
static void cf_CFURLCopyFragment(ppc_cpu *c) { RETURN(part_unescaped(R(0), 2, R(1))); }

/* The last path component's span (escaped), trailing '/' aside. */
static bool last_component(cf_url *u, const parts *p, int *from, int *to)
{
    if (!p->decomposable || p->path < 0)
        return false;
    int e = p->path_end;
    if (e > p->path + 1 && u->s[e - 1] == '/')
        e--;
    int b = e;
    while (b > p->path && u->s[b - 1] != '/')
        b--;
    if (b == e && e > p->path && u->s[b] == '/')
        e = b + 1; /* the root itself */
    *from = b;
    *to = e;
    return true;
}

static void cf_CFURLCopyLastPathComponent(ppc_cpu *c)
{
    cf_url *u = url_of(R(0));
    parts p;
    int from, to;
    if (!u)
        RETURN(0);
    parse(u->s, &p);
    if (!last_component(u, &p, &from, &to))
        RETURN(0);
    gaddr s = part_string(u, from, to);
    gaddr r = unescaped(0, s, cfstr_constant(""));
    cf_release(s);
    RETURN(r);
}

static void cf_CFURLCopyPathExtension(ppc_cpu *c)
{
    cf_url *u = url_of(R(0));
    parts p;
    int from, to;
    if (!u)
        RETURN(0);
    parse(u->s, &p);
    if (!last_component(u, &p, &from, &to))
        RETURN(0);
    for (int i = to - 1; i > from; i--)
        if (u->s[i] == '.')
            RETURN(part_string(u, i + 1, to));
    RETURN(0);
}

/* A copy of a URL with its path's span [from, to) replaced by text. */
static gaddr replace_path(gaddr alloc, cf_url *u, int from, int to, const cf_text *with)
{
    cf_text t = { 0 };
    char *head = strndup(u->s, (size_t)from);
    text_add_utf8(&t, head);
    free(head);
    text_add(&t, with->u, with->n);
    text_add_utf8(&t, u->s + to);
    gaddr r = url_from_text(alloc, &t, u->base);
    text_free(&t);
    return r;
}

/* CFURLCreateCopyAppendingPathComponent(allocator, url, pathComponent, isDirectory) */
static void cf_CFURLCreateCopyAppendingPathComponent(ppc_cpu *c)
{
    cf_url *u = url_of(R(1));
    char *comp = cfstr_utf8(R(2));
    parts p;
    if (!u || !comp) {
        free(comp);
        RETURN(0);
    }
    parse(u->s, &p);
    if (!p.decomposable) {
        free(comp);
        RETURN(0);
    }
    cf_text t = { 0 };
    if (p.path_end > p.path && u->s[p.path_end - 1] != '/')
        text_add_unit(&t, '/');
    else if (p.path_end == p.path && p.net >= 0)
        text_add_unit(&t, '/');
    /* '/' may separate components given in one go. */
    for (char *q = comp; *q;) {
        char *e = strchr(q, '/');
        size_t len = e ? (size_t)(e - q) : strlen(q);
        add_component(&t, q, len);
        if (!e)
            break;
        text_add_unit(&t, '/');
        q = e + 1;
    }
    if ((R(3) & 0xFF) && !(t.n && t.u[t.n - 1] == '/'))
        text_add_unit(&t, '/');
    free(comp);
    gaddr r = replace_path(R(0), u, p.path_end, p.path_end, &t);
    text_free(&t);
    RETURN(r);
}

/* CFURLCreateCopyDeletingLastPathComponent(allocator, url) */
static void cf_CFURLCreateCopyDeletingLastPathComponent(ppc_cpu *c)
{
    cf_url *u = url_of(R(1));
    parts p;
    int from, to;
    if (!u)
        RETURN(0);
    parse(u->s, &p);
    if (!last_component(u, &p, &from, &to))
        RETURN(0);
    cf_text t = { 0 };
    int end = p.path_end;
    if (to - from == 1 && u->s[from] == '/') {
        text_add_utf8(&t, "/../"); /* past the root */
        from = p.path;
    } else if (from == p.path) {
        text_add_utf8(&t, "../");
        if (to - from == 2 && !strncmp(u->s + from, "..", 2))
            from = end; /* "../" becomes "../../" */
    }
    gaddr r = replace_path(R(0), u, from, end, &t);
    text_free(&t);
    RETURN(r);
}

/* CFURLCreateCopyAppendingPathExtension(allocator, url, extension) */
static void cf_CFURLCreateCopyAppendingPathExtension(ppc_cpu *c)
{
    cf_url *u = url_of(R(1));
    char *ext = cfstr_utf8(R(2));
    parts p;
    int from, to;
    if (!u || !ext) {
        free(ext);
        RETURN(0);
    }
    parse(u->s, &p);
    if (!last_component(u, &p, &from, &to)) {
        free(ext);
        RETURN(0);
    }
    cf_text t = { 0 };
    text_add_unit(&t, '.');
    add_component(&t, ext, strlen(ext));
    free(ext);
    gaddr r = replace_path(R(0), u, to, to, &t);
    text_free(&t);
    RETURN(r);
}

/* CFURLCreateCopyDeletingPathExtension(allocator, url) */
static void cf_CFURLCreateCopyDeletingPathExtension(ppc_cpu *c)
{
    cf_url *u = url_of(R(1));
    parts p;
    int from, to;
    if (!u)
        RETURN(0);
    parse(u->s, &p);
    if (!last_component(u, &p, &from, &to))
        RETURN(0);
    int dot = -1;
    for (int i = to - 1; i > from; i--)
        if (u->s[i] == '.') {
            dot = i;
            break;
        }
    if (dot < 0)
        RETURN(cf_retain(R(1)));
    cf_text t = { 0 };
    gaddr r = replace_path(R(0), u, dot, to, &t);
    RETURN(r);
}

/* CFURLCreateStringByReplacingPercentEscapes(allocator, originalString, charactersToLeaveEscaped) */
static void cf_CFURLCreateStringByReplacingPercentEscapes(ppc_cpu *c)
{
    uint32_t n;
    const uint16_t *u = cfstr_chars(R(1), &n);
    if (!u)
        RETURN(0);
    bool any = false;
    for (uint32_t i = 0; i < n; i++)
        any |= u[i] == '%';
    if (!R(2) || !any)
        RETURN(cf_retain(R(1)));
    RETURN(unescaped(R(0), R(1), R(2)));
}

/* CFURLCreateStringByAddingPercentEscapes(allocator, originalString,
 *     charactersToLeaveUnescaped, legalURLCharactersToBeEscaped, encoding) */
static void cf_CFURLCreateStringByAddingPercentEscapes(ppc_cpu *c)
{
    uint32_t n, nl = 0, ne = 0;
    const uint16_t *u = cfstr_chars(R(1), &n);
    const uint16_t *leave = R(2) ? cfstr_chars(R(2), &nl) : NULL, *esc = R(3) ? cfstr_chars(R(3), &ne) : NULL;
    if (!u)
        RETURN(0);
    cf_text t = { 0 };
    bool changed = false;
    for (uint32_t i = 0; i < n; i++) {
        uint16_t ch = u[i];
        bool escape = !url_legal(ch);
        for (uint32_t k = 0; esc && k < ne; k++)
            escape |= esc[k] == ch;
        for (uint32_t k = 0; leave && k < nl; k++)
            if (leave[k] == ch)
                escape = false;
        if (!escape) {
            text_add_unit(&t, ch);
            continue;
        }
        uint32_t units = i + 1 < n && (ch & 0xFC00) == 0xD800 ? 2 : 1;
        uint8_t *b;
        size_t nb;
        uint32_t used;
        if (!cf_encode(R(4), u + i, units, 0, &b, &nb, &used)) {
            free(b);
            text_free(&t);
            RETURN(0);
        }
        for (size_t k = 0; k < nb; k++)
            add_escaped_byte(&t, b[k]);
        free(b);
        i += units - 1;
        changed = true;
    }
    gaddr r = changed ? cfstr_new(R(0), t.u, t.n, false) : cf_retain(R(1));
    text_free(&t);
    RETURN(r);
}

/* CFURLCreateFromFSRef(allocator, fsRef) */
static void cf_CFURLCreateFromFSRef(ppc_cpu *c)
{
    char path[PATH_MAX];
    if (!R(1) || hfs_fsref_path(R(1), path, sizeof path) != noErr)
        RETURN(0);
    RETURN(cfurl_of_host_path(R(0), path));
}

/* CFURLGetFSRef(url, fsRef): for something that is there. */
static void cf_CFURLGetFSRef(ppc_cpu *c)
{
    char path[PATH_MAX];
    if (!R(1) || !cfurl_host_path(R(0), path, sizeof path) || !fm_host_exists(path))
        RETURN(false);
    hfs_make_fsref(R(1), path);
    RETURN(true);
}

/* ---------------------------------------------------------------------- */
/* CFURLAccess: file: URLs */

static bool want(gaddr desired, const char *key)
{
    if (!desired)
        return true;
    gaddr k = cfstr_constant(key);
    for (uint32_t i = 0; i < cfarray_count(desired); i++)
        if (cf_equal(cfarray_get(desired, i), k))
            return true;
    return false;
}

/* Mac local time (since 1904) as absolute time. */
static double absolute_of_mac(uint32_t secs)
{
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    return (double)secs - lt.tm_gmtoff - 3061152000.0;
}

static void put_property(gaddr dict, const char *key, gaddr value)
{
    if (value) {
        cfdict_set(dict, cfstr_constant(key), value);
        cf_release(value);
    }
}

static gaddr file_properties(gaddr alloc, const char *path, gaddr desired)
{
    gaddr d = cfdict_new(alloc, true);
    fm_info info;
    bool there = fm_host_exists(path) && fm_info_of(path, &info);
    if (want(desired, "kCFURLFileExists"))
        cfdict_set(d, cfstr_constant("kCFURLFileExists"), cfbool(there));
    if (!there)
        return d;
    if (info.dir && want(desired, "kCFURLFileDirectoryContents")) {
        char **paths;
        int n = fm_list_dir(path, &paths);
        gaddr a = cfarray_new(alloc, NULL, 0, true);
        for (int i = 0; i < n; i++) {
            gaddr u = cfurl_of_host_path(alloc, paths[i]);
            if (u) {
                cfarray_append(a, u);
                cf_release(u);
            }
            free(paths[i]);
        }
        free(paths);
        put_property(d, "kCFURLFileDirectoryContents", a);
    }
    if (want(desired, "kCFURLFileLength"))
        put_property(d, "kCFURLFileLength", cfnum_new_int(alloc, info.dir ? 0 : info.data_len));
    if (want(desired, "kCFURLFileLastModificationTime"))
        put_property(d, "kCFURLFileLastModificationTime", cfdate_new(alloc, absolute_of_mac(info.modified)));
    if (want(desired, "kCFURLFilePOSIXMode"))
        put_property(d, "kCFURLFilePOSIXMode",
                     cfnum_new_int(alloc, info.dir ? 040755 : fm_writable(path) ? 0100644 : 0100444));
    if (want(desired, "kCFURLFileOwnerID"))
        put_property(d, "kCFURLFileOwnerID", cfnum_new_int(alloc, 0));
    return d;
}

static bool is_file_url(gaddr url)
{
    cf_url *u = url_of(url);
    if (!u)
        return false;
    char *s = resolve(u);
    parts p;
    parse(s ? s : "", &p);
    bool f = s && scheme_is(s, &p, "file");
    free(s);
    return f;
}

/* CFURLCreateDataAndPropertiesFromResource(alloc, url, *resourceData, *properties,
 *                                          desiredProperties, *errorCode) */
static void cf_CFURLCreateDataAndPropertiesFromResource(ppc_cpu *c)
{
    gaddr data_p = R(2), props_p = R(3), err_p = R(5);
    int32_t err = 0;
    char path[PATH_MAX];
    if (data_p)
        wr32(data_p, 0);
    if (props_p)
        wr32(props_p, 0);
    if (!url_of(R(1))) {
        err = kCFURLImproperArgumentsError;
    } else if (!is_file_url(R(1))) {
        err = kCFURLUnknownSchemeError;
    } else if (!cfurl_host_path(R(1), path, sizeof path)) {
        err = kCFURLResourceNotFoundError;
    } else {
        if (props_p)
            wr32(props_p, file_properties(R(0), path, R(4)));
        if (data_p) {
            fm_info info;
            if (!fm_host_exists(path) || !fm_info_of(path, &info))
                err = kCFURLResourceNotFoundError;
            else if (info.dir)
                err = kCFURLImproperArgumentsError;
            else {
                uint8_t *b;
                size_t n;
                if (cfurl_read_file(R(1), &b, &n)) {
                    wr32(data_p, cfdata_new(R(0), b, (uint32_t)n, false));
                    free(b);
                } else {
                    err = kCFURLResourceAccessViolationError;
                }
            }
        } else if (props_p && !fm_host_exists(path)) {
            err = kCFURLResourceNotFoundError;
        }
    }
    if (err_p)
        wr32(err_p, (uint32_t)err);
    RETURN(err == 0);
}

/* CFURLWriteDataAndPropertiesToResource(url, dataToWrite, propertiesToWrite, *errorCode):
 * a URL with a directory path and no data makes the folder. */
static void cf_CFURLWriteDataAndPropertiesToResource(ppc_cpu *c)
{
    char path[PATH_MAX];
    int32_t err = 0;
    cf_url *u = url_of(R(0));
    if (!u) {
        err = kCFURLImproperArgumentsError;
    } else if (!is_file_url(R(0))) {
        err = kCFURLUnknownSchemeError;
    } else if (!cfurl_host_path(R(0), path, sizeof path)) {
        err = kCFURLResourceNotFoundError;
    } else {
        parts p;
        parse(u->s, &p);
        bool dir = p.path_end > p.path && u->s[p.path_end - 1] == '/';
        uint32_t n = 0;
        gaddr bytes = R(1) ? cfdata_bytes(R(1), &n) : 0;
        if (dir) {
            if (!fm_host_exists(path) && fm_create_dir(path) != noErr)
                err = kCFURLResourceAccessViolationError;
        } else if (R(1)) {
            int16_t ref;
            if (!fm_host_exists(path) && fm_create_file(path) != noErr)
                err = kCFURLResourceAccessViolationError;
            else if (fm_open_fork(path, false, 3, &ref) != noErr)
                err = kCFURLResourceAccessViolationError;
            else {
                uint32_t wrote = 0;
                if (fm_fork_set_eof(ref, 1, 0) != noErr ||
                    (n && fm_fork_io(ref, true, 1, 0, n, bytes, &wrote) != noErr) || wrote != n)
                    err = kCFURLResourceAccessViolationError;
                fm_close_fork(ref);
            }
        } else if (!fm_host_exists(path)) {
            err = kCFURLResourceNotFoundError;
        }
    }
    if (R(3))
        wr32(R(3), (uint32_t)err);
    RETURN(err == 0);
}

/* CFURLDestroyResource(url, *errorCode) */
static void cf_CFURLDestroyResource(ppc_cpu *c)
{
    char path[PATH_MAX];
    int32_t err = 0;
    if (!is_file_url(R(0)))
        err = url_of(R(0)) ? kCFURLUnknownSchemeError : kCFURLImproperArgumentsError;
    else if (!cfurl_host_path(R(0), path, sizeof path) || !fm_host_exists(path))
        err = kCFURLResourceNotFoundError;
    else if (fm_delete(path) != noErr)
        err = kCFURLResourceAccessViolationError;
    if (R(1))
        wr32(R(1), (uint32_t)err);
    RETURN(err == 0);
}

/* CFURLCreatePropertyFromResource(alloc, url, property, *errorCode) */
static void cf_CFURLCreatePropertyFromResource(ppc_cpu *c)
{
    char path[PATH_MAX];
    int32_t err = 0;
    gaddr value = 0;
    if (!is_file_url(R(1))) {
        err = url_of(R(1)) ? kCFURLUnknownSchemeError : kCFURLImproperArgumentsError;
    } else if (!cfurl_host_path(R(1), path, sizeof path)) {
        err = kCFURLResourceNotFoundError;
    } else {
        gaddr keys = cfarray_new(0, &R(2), 1, false);
        gaddr d = file_properties(R(0), path, keys);
        cf_release(keys);
        if (!cfdict_get(d, R(2), &value))
            err = fm_host_exists(path) ? kCFURLUnknownPropertyKeyError : kCFURLResourceNotFoundError;
        else
            cf_retain(value);
        cf_release(d);
    }
    if (R(3))
        wr32(R(3), (uint32_t)err);
    RETURN(value);
}

/* ---------------------------------------------------------------------- */
/* Bundles */

typedef struct {
    cf_obj o;
    char path[PATH_MAX]; /* the bundle's folder */
    bool main, plugin, load_on_demand;
    gaddr url, info, local_info;
    bool info_read, local_read;
    uint32_t conn; /* its CFM connection once loaded */
    int instances;
} cf_bundle;

static gaddr main_bundle;
static gaddr *bundles;
static unsigned nbundles, bundles_cap;
static gaddr all_bundles; /* CFBundleGetAllBundles' array */

static cf_bundle *bundle_of(gaddr ref) { return cf_get_type(ref, CFT_BUNDLE); }

static void bundle_finalize(cf_obj *o)
{
    cf_bundle *b = (cf_bundle *)o;
    for (unsigned i = 0; i < nbundles; i++)
        if (bundles[i] == o->ref) {
            bundles[i] = bundles[--nbundles];
            break;
        }
    cf_release(b->url);
    cf_release(b->info);
    cf_release(b->local_info);
}

static void bundle_describe(cf_obj *o, cf_text *out)
{
    cf_bundle *b = (cf_bundle *)o;
    text_add_utf8(out, "path = ");
    text_add_utf8(out, b->path);
    text_add_utf8(out, b->conn ? ", loaded" : ", not loaded");
}

static const cf_class bundle_class = { "CFBundle", bundle_finalize, NULL, NULL, NULL, bundle_describe };

static void join(char *out, size_t n, const char *dir, const char *name) { snprintf(out, n, "%s/%s", dir, name); }

/* A file in the bundle's folder, if it is there (case aside). */
static bool bundle_file(const char *dir, const char *rel, char *out, size_t n)
{
    char cur[PATH_MAX], next[PATH_MAX], part[PATH_MAX];
    snprintf(cur, sizeof cur, "%s", dir);
    snprintf(part, sizeof part, "%s", rel);
    char *save = NULL;
    for (char *tok = strtok_r(part, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        fm_child_path(cur, tok, next, sizeof next);
        if (!fm_host_exists(next))
            return false;
        snprintf(cur, sizeof cur, "%s", next);
    }
    snprintf(out, n, "%s", cur);
    return true;
}

static gaddr read_plist_file(const char *path)
{
    gaddr url = cfurl_of_host_path(0, path);
    uint8_t *b;
    size_t n;
    gaddr p = 0;
    if (url && cfurl_read_file(url, &b, &n)) {
        p = cfplist_from_data(0, b, n, 0, NULL);
        free(b);
    }
    cf_release(url);
    return p;
}

/* The application's own resource, read from its file. */
static gaddr app_resource(uint32_t type, int16_t id, uint8_t **bytes, size_t *n)
{
    int16_t cur = tb_CurResFile();
    tb_UseResFile((int16_t)rd16(LM_CurApRefNum));
    gaddr h = tb_Get1Resource((int32_t)type, id);
    tb_UseResFile(cur);
    if (!h || !rd32(h))
        return 0;
    *n = (size_t)tb_GetHandleSize(h);
    *bytes = malloc(*n ? *n : 1);
    if (*bytes)
        gget(*bytes, rd32(h), (uint32_t)*n);
    return h;
}

static gaddr bundle_info(cf_bundle *b)
{
    if (b->info_read)
        return b->info;
    b->info_read = true;
    if (b->main) {
        uint8_t *bytes;
        size_t n;
        if (app_resource(fourcc("plst"), 0, &bytes, &n)) {
            b->info = cfplist_from_data(0, bytes, n, 0, NULL);
            free(bytes);
        }
    } else {
        static const char *const names[] = { "Contents/Info-macos.plist", "Contents/Info.plist",
                                             "Resources/Info-macos.plist", "Resources/Info.plist", "Info.plist" };
        char path[PATH_MAX];
        for (unsigned i = 0; i < sizeof names / sizeof names[0] && !b->info; i++)
            if (bundle_file(b->path, names[i], path, sizeof path))
                b->info = read_plist_file(path);
    }
    if (cf_type_of(b->info) != CFT_DICTIONARY) {
        cf_release(b->info);
        b->info = cfdict_new(0, true);
    }
    return b->info;
}

static gaddr info_value(cf_bundle *b, const char *key)
{
    uint32_t v = 0;
    cfdict_get(bundle_info(b), cfstr_constant(key), &v);
    return v;
}

/* The resources folder: Contents/Resources, Resources, or the bundle's
 * own folder (the main bundle's). */
static void resources_dir(cf_bundle *b, char *out, size_t n)
{
    if (b->main || (!bundle_file(b->path, "Contents/Resources", out, n) && !bundle_file(b->path, "Resources", out, n)))
        snprintf(out, n, "%s", b->path);
}

/* The localizations to look in, best first. */
static const char *const languages[] = { "English", "en", "en_US", "Base" };

static cf_bundle *bundle_make(gaddr alloc, const char *given, bool main_)
{
    char path[PATH_MAX];
    if (!realpath(given, path))
        snprintf(path, sizeof path, "%s", given);
    for (unsigned i = 0; i < nbundles; i++) {
        cf_bundle *b = bundle_of(bundles[i]);
        if (b && !strcmp(b->path, path) && b->main == main_)
            return b;
    }
    cf_bundle *b = cf_new(CFT_BUNDLE, sizeof *b, alloc, 0);
    if (!b)
        return NULL;
    if (!realpath(path, b->path)) /* the File Manager's spelling */
        snprintf(b->path, sizeof b->path, "%s", path);
    b->main = main_;
    b->load_on_demand = true;
    b->url = cfurl_of_host_path(alloc, path);
    if (nbundles == bundles_cap) {
        unsigned cap = bundles_cap * 2 + 8;
        gaddr *p = realloc(bundles, sizeof *p * cap);
        if (p) {
            bundles = p;
            bundles_cap = cap;
        }
    }
    if (nbundles < bundles_cap)
        bundles[nbundles++] = b->o.ref;
    if (main_) {
        b->conn = 1; /* the application's fragment, connected first */
        cf_make_constant(&b->o);
    }
    return b;
}

static void cf_CFBundleGetTypeID(ppc_cpu *c) { RETURN(CFT_BUNDLE); }

static gaddr get_main_bundle(void)
{
    if (!main_bundle && g_app_path[0]) {
        char dir[PATH_MAX];
        snprintf(dir, sizeof dir, "%s", g_app_path);
        char *slash = strrchr(dir, '/');
        if (slash)
            *slash = 0;
        cf_bundle *b = bundle_make(cf_allocator_system(), dir[0] ? dir : "/", true);
        main_bundle = b ? b->o.ref : 0;
    }
    return main_bundle;
}

static void cf_CFBundleGetMainBundle(ppc_cpu *c) { RETURN(get_main_bundle()); }

gaddr cfbundle_main_identifier(void)
{
    cf_bundle *b = bundle_of(get_main_bundle());
    gaddr v = b ? info_value(b, "CFBundleIdentifier") : 0;
    return cf_type_of(v) == CFT_STRING ? v : 0;
}

/* CFBundleCreate(allocator, bundleURL): a folder; the same bundle again for
 * the same folder. */
static void cf_CFBundleCreate(ppc_cpu *c)
{
    char path[PATH_MAX];
    if (!cfurl_host_path(R(1), path, sizeof path) || !fm_is_dir(path))
        RETURN(0);
    for (unsigned i = 0; i < nbundles; i++) {
        cf_bundle *b = bundle_of(bundles[i]);
        if (b && !b->main && !strcmp(b->path, path))
            RETURN(cf_retain(b->o.ref));
    }
    cf_bundle *b = bundle_make(R(0), path, false);
    RETURN(b ? b->o.ref : 0);
}

static bool has_extension(const char *path, const char *ext)
{
    size_t n = strlen(path), e = strlen(ext);
    return n > e + 1 && path[n - e - 1] == '.' && !strcasecmp(path + n - e, ext);
}

/* CFBundleCreateBundlesFromDirectory(allocator, directoryURL, bundleType) */
static void cf_CFBundleCreateBundlesFromDirectory(ppc_cpu *c)
{
    char path[PATH_MAX];
    char *type = R(2) ? cfstr_utf8(R(2)) : NULL;
    gaddr a = cfarray_new(R(0), NULL, 0, true);
    if (cfurl_host_path(R(1), path, sizeof path) && fm_is_dir(path)) {
        char **paths;
        int n = fm_list_dir(path, &paths);
        for (int i = 0; i < n; i++) {
            if (fm_is_dir(paths[i]) && (!type || has_extension(paths[i], type))) {
                cf_bundle *b = bundle_make(R(0), paths[i], false);
                if (b) {
                    cfarray_append(a, b->o.ref);
                    cf_release(b->o.ref);
                }
            }
            free(paths[i]);
        }
        free(paths);
    }
    free(type);
    RETURN(a);
}

static void cf_CFBundleGetBundleWithIdentifier(ppc_cpu *c)
{
    get_main_bundle();
    for (unsigned i = 0; i < nbundles; i++) {
        cf_bundle *b = bundle_of(bundles[i]);
        if (b && cf_equal(info_value(b, "CFBundleIdentifier"), R(0)))
            RETURN(b->o.ref);
    }
    RETURN(0);
}

static void cf_CFBundleGetAllBundles(ppc_cpu *c)
{
    get_main_bundle();
    /* One array, as CF keeps, refilled. */
    if (!all_bundles)
        all_bundles = cfarray_new(0, NULL, 0, true);
    cfarray_remove_all(all_bundles);
    for (unsigned i = 0; i < nbundles; i++)
        cfarray_append(all_bundles, bundles[i]);
    RETURN(all_bundles);
}

static void cf_CFBundleCopyBundleURL(ppc_cpu *c)
{
    cf_bundle *b = bundle_of(R(0));
    RETURN(b ? cf_retain(b->url) : 0);
}

/* The localized info dictionary: InfoPlist.strings of the first
 * localization that has one. */
static gaddr local_info(cf_bundle *b)
{
    if (b->local_read)
        return b->local_info;
    b->local_read = true;
    char res[PATH_MAX], path[PATH_MAX], rel[64];
    resources_dir(b, res, sizeof res);
    for (unsigned i = 0; i < sizeof languages / sizeof languages[0] && !b->local_info; i++) {
        snprintf(rel, sizeof rel, "%s.lproj/InfoPlist.strings", languages[i]);
        if (bundle_file(res, rel, path, sizeof path))
            b->local_info = read_plist_file(path);
    }
    if (b->local_info && cf_type_of(b->local_info) != CFT_DICTIONARY) {
        cf_release(b->local_info);
        b->local_info = 0;
    }
    return b->local_info;
}

/* CFBundleGetValueForInfoDictionaryKey(bundle, key): localized first. */
static void cf_CFBundleGetValueForInfoDictionaryKey(ppc_cpu *c)
{
    cf_bundle *b = bundle_of(R(0));
    uint32_t v = 0;
    if (b && !cfdict_get(local_info(b), R(1), &v))
        cfdict_get(bundle_info(b), R(1), &v);
    RETURN(v);
}

static void cf_CFBundleGetInfoDictionary(ppc_cpu *c)
{
    cf_bundle *b = bundle_of(R(0));
    RETURN(b ? bundle_info(b) : 0);
}

static void cf_CFBundleGetLocalInfoDictionary(ppc_cpu *c)
{
    cf_bundle *b = bundle_of(R(0));
    RETURN(b ? local_info(b) : 0);
}

static uint32_t four_of(gaddr str)
{
    char *s = cfstr_utf8(str);
    uint32_t v = 0;
    if (s && strlen(s) == 4)
        v = fourcc(s);
    free(s);
    return v;
}

/* A folder's type and creator: Contents/PkgInfo, else its info. */
static bool package_info(const char *dir, gaddr info, uint32_t *type, uint32_t *creator)
{
    char path[PATH_MAX];
    *type = fourcc("BNDL");
    *creator = fourcc("????");
    if (bundle_file(dir, "Contents/PkgInfo", path, sizeof path)) {
        gaddr url = cfurl_of_host_path(0, path);
        uint8_t *b;
        size_t n;
        bool ok = cfurl_read_file(url, &b, &n);
        cf_release(url);
        if (ok && n >= 8) {
            *type = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
            *creator = (uint32_t)b[4] << 24 | (uint32_t)b[5] << 16 | (uint32_t)b[6] << 8 | b[7];
        }
        if (ok)
            free(b);
        if (ok && n >= 8)
            return true;
    }
    uint32_t t = 0, k = 0, v;
    if (cfdict_get(info, cfstr_constant("CFBundlePackageType"), &v))
        t = four_of(v);
    if (cfdict_get(info, cfstr_constant("CFBundleSignature"), &v))
        k = four_of(v);
    if (t)
        *type = t;
    if (k)
        *creator = k;
    return t || k;
}

/* CFBundleGetPackageInfo(bundle, *packageType, *packageCreator) */
static void cf_CFBundleGetPackageInfo(ppc_cpu *c)
{
    cf_bundle *b = bundle_of(R(0));
    uint32_t type = fourcc("BNDL"), creator = fourcc("????");
    fm_info info;
    if (b && b->main && fm_info_of(g_app_path, &info)) {
        type = (uint32_t)info.finder_info[0] << 24 | (uint32_t)info.finder_info[1] << 16 |
               (uint32_t)info.finder_info[2] << 8 | info.finder_info[3];
        creator = (uint32_t)info.finder_info[4] << 24 | (uint32_t)info.finder_info[5] << 16 |
                  (uint32_t)info.finder_info[6] << 8 | info.finder_info[7];
        if (!type) /* an application, whatever its file says */
            type = fourcc("APPL");
        if (!creator)
            creator = fourcc("????");
    } else if (b) {
        package_info(b->path, bundle_info(b), &type, &creator);
    }
    if (R(1))
        wr32(R(1), type);
    if (R(2))
        wr32(R(2), creator);
    RETURN_VOID();
}

static void cf_CFBundleGetIdentifier(ppc_cpu *c)
{
    cf_bundle *b = bundle_of(R(0));
    gaddr v = b ? info_value(b, "CFBundleIdentifier") : 0;
    RETURN(cf_type_of(v) == CFT_STRING ? v : 0);
}

/* "1.2.3d4" as a NumVersion: the major version in BCD, minor and bug fix
 * nibbles, the stage (development, alpha, beta, final) and its build. */
static uint32_t version_number(gaddr str)
{
    char *s = cfstr_utf8(str);
    if (!s)
        return 0;
    const char *p = s;
    uint32_t major = 0, minor = 0, fix = 0, stage = 0x80, build = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9' && digits < 2)
        major = major << 4 | (uint32_t)(*p++ - '0'), digits++;
    if (digits && *p == '.' && p[1] >= '0' && p[1] <= '9') {
        minor = (uint32_t)(p[1] - '0');
        p += 2;
        if (*p == '.' && p[1] >= '0' && p[1] <= '9') {
            fix = (uint32_t)(p[1] - '0');
            p += 2;
        }
    }
    if (digits && (*p == 'd' || *p == 'a' || *p == 'b' || *p == 'f')) {
        stage = *p == 'd' ? 0x20 : *p == 'a' ? 0x40 : *p == 'b' ? 0x60 : 0x80;
        p++;
        while (*p >= '0' && *p <= '9')
            build = build * 10 + (uint32_t)(*p++ - '0');
        if (build > 255)
            build = 255;
    }
    bool ok = digits && !*p;
    free(s);
    return ok ? major << 24 | minor << 20 | fix << 16 | stage << 8 | build : 0;
}

static void cf_CFBundleGetVersionNumber(ppc_cpu *c)
{
    cf_bundle *b = bundle_of(R(0));
    gaddr v = b ? info_value(b, "CFBundleVersion") : 0;
    if (cf_type_of(v) == CFT_NUMBER) {
        int64_t i = 0;
        cfnum_value(v, NULL, &i, NULL);
        RETURN((uint32_t)i);
    }
    RETURN(version_number(v));
}

static void cf_CFBundleGetDevelopmentRegion(ppc_cpu *c)
{
    cf_bundle *b = bundle_of(R(0));
    gaddr v = b ? info_value(b, "CFBundleDevelopmentRegion") : 0;
    RETURN(cf_type_of(v) == CFT_STRING ? v : 0);
}

/* The bundle's folders as URLs relative to its own, as CF gives them. */
static gaddr bundle_subdir(cf_bundle *b, const char *rel)
{
    if (!b)
        return 0;
    if (b->main)
        return cf_retain(b->url);
    char path[PATH_MAX];
    bool contents = bundle_file(b->path, "Contents", path, sizeof path);
    char full[128];
    snprintf(full, sizeof full, "%s%s", contents ? "Contents/" : "", rel);
    return url_from_utf8(0, full, b->url);
}

static void cf_CFBundleCopySupportFilesDirectoryURL(ppc_cpu *c) { RETURN(bundle_subdir(bundle_of(R(0)), "")); }
static void cf_CFBundleCopyResourcesDirectoryURL(ppc_cpu *c) { RETURN(bundle_subdir(bundle_of(R(0)), "Resources/")); }
static void cf_CFBundleCopyPrivateFrameworksURL(ppc_cpu *c) { RETURN(bundle_subdir(bundle_of(R(0)), "Frameworks/")); }
static void cf_CFBundleCopySharedFrameworksURL(ppc_cpu *c)
{
    RETURN(bundle_subdir(bundle_of(R(0)), "SharedFrameworks/"));
}
static void cf_CFBundleCopySharedSupportURL(ppc_cpu *c) { RETURN(bundle_subdir(bundle_of(R(0)), "SharedSupport/")); }
static void cf_CFBundleCopyBuiltInPlugInsURL(ppc_cpu *c) { RETURN(bundle_subdir(bundle_of(R(0)), "PlugIns/")); }

/* A bundle for a URL for a moment (the ...InDirectory calls). */
static cf_bundle *bundle_for_url(gaddr url, bool *made)
{
    char path[PATH_MAX];
    *made = false;
    if (!cfurl_host_path(url, path, sizeof path) || !fm_is_dir(path))
        return NULL;
    for (unsigned i = 0; i < nbundles; i++) {
        cf_bundle *b = bundle_of(bundles[i]);
        if (b && !b->main && !strcmp(b->path, path))
            return b;
    }
    *made = true;
    return bundle_make(0, path, false);
}

static void cf_CFBundleCopyInfoDictionaryInDirectory(ppc_cpu *c)
{
    bool made;
    cf_bundle *b = bundle_for_url(R(0), &made);
    gaddr info = b ? cf_retain(bundle_info(b)) : 0;
    if (made)
        cf_release(b->o.ref);
    RETURN(info);
}

/* CFBundleGetPackageInfoInDirectory(url, *packageType, *packageCreator) */
static void cf_CFBundleGetPackageInfoInDirectory(ppc_cpu *c)
{
    bool made;
    cf_bundle *b = bundle_for_url(R(0), &made);
    uint32_t type = fourcc("BNDL"), creator = fourcc("????");
    bool ok = b && package_info(b->path, bundle_info(b), &type, &creator);
    if (made)
        cf_release(b->o.ref);
    if (R(1))
        wr32(R(1), type);
    if (R(2))
        wr32(R(2), creator);
    RETURN(ok);
}

/* Look for name.type in the resources (subDir within them), not
 * localized first, then each localization (or only the one given).  With
 * no name, every file of the type; into the array if there is one. */
static gaddr find_resources(cf_bundle *b, gaddr name, gaddr type, gaddr subdir, gaddr loc, gaddr array)
{
    char res[PATH_MAX], dir[PATH_MAX], file[PATH_MAX], rel[PATH_MAX];
    char *n = name ? cfstr_utf8(name) : NULL, *t = type ? cfstr_utf8(type) : NULL;
    char *sd = subdir ? cfstr_utf8(subdir) : NULL, *l = loc ? cfstr_utf8(loc) : NULL;
    resources_dir(b, res, sizeof res);
    gaddr found = 0;
    unsigned nlang = sizeof languages / sizeof languages[0];
    for (int pass = l ? 1 : 0; pass <= (int)(l ? 1 : nlang) && !found; pass++) {
        const char *lang = l ? l : pass ? languages[pass - 1] : NULL;
        snprintf(rel, sizeof rel, "%s%s%s%s", lang ? lang : "", lang ? ".lproj" : "", lang && sd ? "/" : "",
                 sd ? sd : "");
        if (rel[0] ? !bundle_file(res, rel, dir, sizeof dir) : (snprintf(dir, sizeof dir, "%s", res), false))
            continue;
        if (n) {
            char want_name[PATH_MAX];
            snprintf(want_name, sizeof want_name, "%s%s%s", n, t && t[0] ? "." : "", t ? t : "");
            if (bundle_file(dir, want_name, file, sizeof file))
                found = cfurl_of_host_path(0, file);
        } else if (array) {
            char **paths;
            int k = fm_list_dir(dir, &paths);
            for (int i = 0; i < k; i++) {
                if (!fm_is_dir(paths[i]) && (!t || !t[0] || has_extension(paths[i], t))) {
                    gaddr u = cfurl_of_host_path(0, paths[i]);
                    if (u) {
                        cfarray_append(array, u);
                        cf_release(u);
                    }
                }
                free(paths[i]);
            }
            free(paths);
        }
        if (l)
            break;
    }
    free(n);
    free(t);
    free(sd);
    free(l);
    return found;
}

/* CFBundleCopyResourceURL(bundle, resourceName, resourceType, subDirName) */
static void cf_CFBundleCopyResourceURL(ppc_cpu *c)
{
    cf_bundle *b = bundle_of(R(0));
    RETURN(b && R(1) ? find_resources(b, R(1), R(2), R(3), 0, 0) : 0);
}

/* CFBundleCopyResourceURLsOfType(bundle, resourceType, subDirName) */
static void cf_CFBundleCopyResourceURLsOfType(ppc_cpu *c)
{
    cf_bundle *b = bundle_of(R(0));
    gaddr a = cfarray_new(0, NULL, 0, true);
    if (b)
        find_resources(b, 0, R(1), R(2), 0, a);
    RETURN(a);
}

/* ...ForLocalization(bundle, resourceName, resourceType, subDirName, localizationName) */
static void cf_CFBundleCopyResourceURLForLocalization(ppc_cpu *c)
{
    cf_bundle *b = bundle_of(R(0));
    RETURN(b && R(1) ? find_resources(b, R(1), R(2), R(3), R(4), 0) : 0);
}

static void cf_CFBundleCopyResourceURLsOfTypeForLocalization(ppc_cpu *c)
{
    cf_bundle *b = bundle_of(R(0));
    gaddr a = cfarray_new(0, NULL, 0, true);
    if (b)
        find_resources(b, 0, R(1), R(2), R(3), a);
    RETURN(a);
}

/* CFBundleCopyResourceURLInDirectory(bundleURL, resourceName, resourceType, subDirName) */
static void cf_CFBundleCopyResourceURLInDirectory(ppc_cpu *c)
{
    bool made;
    cf_bundle *b = bundle_for_url(R(0), &made);
    gaddr u = b && R(1) ? find_resources(b, R(1), R(2), R(3), 0, 0) : 0;
    if (made)
        cf_release(b->o.ref);
    RETURN(u);
}

static void cf_CFBundleCopyResourceURLsOfTypeInDirectory(ppc_cpu *c)
{
    bool made;
    cf_bundle *b = bundle_for_url(R(0), &made);
    gaddr a = cfarray_new(0, NULL, 0, true);
    if (b)
        find_resources(b, 0, R(1), R(2), 0, a);
    if (made)
        cf_release(b->o.ref);
    RETURN(a);
}

/* CFBundleCopyLocalizedString(bundle, key, value, tableName): from
 * tableName.strings (Localizable.strings), else the value, else the key. */
static void cf_CFBundleCopyLocalizedString(ppc_cpu *c)
{
    cf_bundle *b = bundle_of(R(0));
    gaddr key = R(1), value = R(2);
    uint32_t found = 0;
    if (b && key) {
        gaddr table = R(3) ? R(3) : cfstr_constant("Localizable");
        gaddr type = cfstr_constant("strings");
        gaddr url = find_resources(b, table, type, 0, 0, 0);
        uint8_t *bytes;
        size_t n;
        if (url && cfurl_read_file(url, &bytes, &n)) {
            gaddr d = cfplist_from_data(0, bytes, n, 0, NULL);
            free(bytes);
            if (cfdict_get(d, key, &found))
                cf_retain(found);
            cf_release(d);
        }
        cf_release(url);
    }
    if (found)
        RETURN(found);
    uint32_t n = 0;
    if (value && cfstr_chars(value, &n) && n)
        RETURN(cf_retain(value));
    RETURN(cf_retain(key));
}

/* CFBundleCopyBundleLocalizations(bundle): its .lproj folders. */
static void cf_CFBundleCopyBundleLocalizations(ppc_cpu *c)
{
    cf_bundle *b = bundle_of(R(0));
    gaddr a = cfarray_new(0, NULL, 0, true);
    if (!b)
        RETURN(a);
    char res[PATH_MAX];
    resources_dir(b, res, sizeof res);
    char **paths;
    int n = fm_list_dir(res, &paths);
    for (int i = 0; i < n; i++) {
        if (fm_is_dir(paths[i]) && has_extension(paths[i], "lproj")) {
            char *base = strrchr(paths[i], '/');
            base = base ? base + 1 : paths[i];
            base[strlen(base) - 6] = 0;
            gaddr s = cfstr_from_utf8(base);
            cfarray_append(a, s);
            cf_release(s);
        }
        free(paths[i]);
    }
    free(paths);
    gaddr dev = info_value(b, "CFBundleDevelopmentRegion");
    if (!cfarray_count(a) && cf_type_of(dev) == CFT_STRING)
        cfarray_append(a, dev);
    RETURN(a);
}

/* CFBundleCopyPreferredLocalizationsFromArray(locArray): the user's
 * language (English), else the first. */
static void cf_CFBundleCopyPreferredLocalizationsFromArray(ppc_cpu *c)
{
    gaddr a = cfarray_new(0, NULL, 0, true);
    uint32_t n = cfarray_count(R(0));
    for (unsigned l = 0; l < sizeof languages / sizeof languages[0] && !cfarray_count(a); l++)
        for (uint32_t i = 0; i < n; i++)
            if (cf_equal(cfarray_get(R(0), i), cfstr_constant(languages[l]))) {
                cfarray_append(a, cfarray_get(R(0), i));
                break;
            }
    if (!cfarray_count(a) && n)
        cfarray_append(a, cfarray_get(R(0), 0));
    RETURN(a);
}

/* The executable: Contents/MacOSClassic (CFM), Contents/MacOS, or for the
 * main bundle the application. */
static bool executable_path(cf_bundle *b, const char *name, char *out, size_t n)
{
    if (b->main && !name) {
        snprintf(out, n, "%s", g_app_path);
        return true;
    }
    char *exe = NULL;
    if (!name) {
        gaddr v = info_value(b, "CFBundleExecutable");
        exe = cfstr_utf8(v);
        name = exe;
    }
    bool ok = false;
    if (name) {
        static const char *const dirs[] = { "Contents/MacOSClassic/", "Contents/MacOS/", "MacOSClassic/", "" };
        char rel[PATH_MAX];
        for (unsigned i = 0; i < sizeof dirs / sizeof dirs[0] && !ok; i++) {
            snprintf(rel, sizeof rel, "%s%s", dirs[i], name);
            ok = bundle_file(b->path, rel, out, n) && !fm_is_dir(out);
        }
    }
    free(exe);
    return ok;
}

static void cf_CFBundleCopyExecutableURL(ppc_cpu *c)
{
    cf_bundle *b = bundle_of(R(0));
    char path[PATH_MAX];
    RETURN(b && executable_path(b, NULL, path, sizeof path) ? cfurl_of_host_path(0, path) : 0);
}

static void cf_CFBundleCopyAuxiliaryExecutableURL(ppc_cpu *c)
{
    cf_bundle *b = bundle_of(R(0));
    char path[PATH_MAX], *name = cfstr_utf8(R(1));
    gaddr u = b && name && executable_path(b, name, path, sizeof path) ? cfurl_of_host_path(0, path) : 0;
    free(name);
    RETURN(u);
}

static void cf_CFBundleIsExecutableLoaded(ppc_cpu *c)
{
    cf_bundle *b = bundle_of(R(0));
    RETURN(b && b->conn);
}

/* Load a bundle's code: its fragment, through the Code Fragment Manager. */
static bool bundle_load(cf_bundle *b)
{
    if (b->conn)
        return true;
    char path[PATH_MAX];
    if (!executable_path(b, NULL, path, sizeof path))
        return false;
    void (*get)(ppc_cpu *) = cfm_export_fn("GetDiskFragment");
    gaddr scratch = tb_NewPtr(80 + 16 + 256, false, true);
    if (!get || !scratch)
        return false;
    fm_spec_of_path(path, scratch);
    ppc_cpu sub = g_ppc;
    sub.gpr[3] = scratch;      /* fileSpec */
    sub.gpr[4] = 0;            /* offset */
    sub.gpr[5] = 0;            /* length: the whole data fork */
    sub.gpr[6] = 0;            /* fragName */
    sub.gpr[7] = 1;            /* kReferenceCFrag */
    sub.gpr[8] = scratch + 80; /* *connID */
    sub.gpr[9] = scratch + 84; /* *mainAddr */
    sub.gpr[10] = scratch + 96;
    get(&sub);
    if (sub.gpr[3] == 0)
        b->conn = rd32(scratch + 80);
    tb_DisposePtr(scratch);
    return b->conn != 0;
}

static void cf_CFBundleLoadExecutable(ppc_cpu *c)
{
    cf_bundle *b = bundle_of(R(0));
    RETURN(b && bundle_load(b));
}

/* CFBundleUnloadExecutable(bundle): the fragment stays (CFM keeps it), the
 * bundle forgets it. */
static void cf_CFBundleUnloadExecutable(ppc_cpu *c)
{
    cf_bundle *b = bundle_of(R(0));
    if (b && !b->main)
        b->conn = 0;
    RETURN_VOID();
}

/* A symbol of a bundle's fragment (loaded if need be); 0 if none. */
static gaddr bundle_symbol(cf_bundle *b, gaddr name)
{
    char *n = cfstr_utf8(name);
    if (!b || !n || !bundle_load(b)) {
        free(n);
        return 0;
    }
    void (*find)(ppc_cpu *) = cfm_export_fn("FindSymbol");
    gaddr scratch = tb_NewPtr(256 + 8, false, true);
    gaddr addr = 0;
    if (find && scratch) {
        size_t len = strlen(n) > 255 ? 255 : strlen(n);
        wr8(scratch, (uint8_t)len);
        gput(scratch + 1, n, (uint32_t)len);
        ppc_cpu sub = g_ppc;
        sub.gpr[3] = b->conn;
        sub.gpr[4] = scratch;
        sub.gpr[5] = scratch + 256;
        sub.gpr[6] = scratch + 260;
        find(&sub);
        if (sub.gpr[3] == 0)
            addr = rd32(scratch + 256);
    }
    if (scratch)
        tb_DisposePtr(scratch);
    free(n);
    return addr;
}

static void cf_CFBundleGetFunctionPointerForName(ppc_cpu *c) { RETURN(bundle_symbol(bundle_of(R(0)), R(1))); }
static void cf_CFBundleGetDataPointerForName(ppc_cpu *c) { RETURN(bundle_symbol(bundle_of(R(0)), R(1))); }

/* CFBundleGetFunctionPointersForNames(bundle, functionNames, ftbl[]) */
static void cf_CFBundleGetFunctionPointersForNames(ppc_cpu *c)
{
    cf_bundle *b = bundle_of(R(0));
    for (uint32_t i = 0; R(2) && i < cfarray_count(R(1)); i++)
        wr32(R(2) + 4 * i, bundle_symbol(b, cfarray_get(R(1), i)));
    RETURN_VOID();
}

/* CFBundleOpenBundleResourceMap(bundle): the main bundle's is the
 * application's own file, open already; a folder's its executable's
 * .rsrc file in Resources. */
static int16_t open_bundle_resources(cf_bundle *b, bool localized)
{
    if (b->main)
        return localized ? -1 : (int16_t)rd16(LM_CurApRefNum);
    char res[PATH_MAX], path[PATH_MAX], rel[PATH_MAX];
    resources_dir(b, res, sizeof res);
    char *exe = cfstr_utf8(info_value(b, "CFBundleExecutable"));
    bool found = false;
    if (localized) {
        for (unsigned i = 0; i < sizeof languages / sizeof languages[0] && !found; i++) {
            snprintf(rel, sizeof rel, "%s.lproj/Localized.rsrc", languages[i]);
            found = bundle_file(res, rel, path, sizeof path);
        }
    } else if (exe) {
        snprintf(rel, sizeof rel, "%s.rsrc", exe);
        found = bundle_file(res, rel, path, sizeof path);
    }
    free(exe);
    return found ? rm_open_path(path, 1 /* fsRdPerm */) : -1;
}

static void cf_CFBundleOpenBundleResourceMap(ppc_cpu *c)
{
    cf_bundle *b = bundle_of(R(0));
    int16_t r = b ? open_bundle_resources(b, false) : -1;
    if (b && r < 0)
        r = open_bundle_resources(b, true);
    RETURN((uint32_t)(int32_t)r);
}

/* CFBundleOpenBundleResourceFiles(bundle, *refNum, *localizedRefNum) */
static void cf_CFBundleOpenBundleResourceFiles(ppc_cpu *c)
{
    cf_bundle *b = bundle_of(R(0));
    int16_t r = b ? open_bundle_resources(b, false) : -1, l = b ? open_bundle_resources(b, true) : -1;
    if (R(1))
        wr16(R(1), (uint16_t)r);
    if (R(2))
        wr16(R(2), (uint16_t)l);
    RETURN((uint32_t)(r < 0 && l < 0 ? (int32_t)fnfErr : 0));
}

static void cf_CFBundleCloseBundleResourceMap(ppc_cpu *c)
{
    int16_t r = (int16_t)R(1);
    if (r > 0 && r != (int16_t)rd16(LM_CurApRefNum))
        tb_CloseResFile(r);
    RETURN_VOID();
}

/* ---------------------------------------------------------------------- */
/* Plug-ins: a bundle with factories, and the factories' registry */

typedef struct {
    uint8_t factory[16];
    gaddr fn;      /* the factory function, or */
    gaddr name;    /* its name in the plug-in's code */
    gaddr plugin;
    int instances;
} factory;

typedef struct {
    uint8_t type[16], factory[16];
} registration;

static factory *factories;
static unsigned nfactories, factories_cap;
static registration *registrations;
static unsigned nregs, regs_cap;

static factory *factory_of(const uint8_t id[16])
{
    for (unsigned i = 0; i < nfactories; i++)
        if (!memcmp(factories[i].factory, id, 16))
            return &factories[i];
    return NULL;
}

static bool add_factory(const uint8_t id[16], gaddr fn, gaddr name, gaddr plugin)
{
    if (factory_of(id))
        return false;
    if (nfactories == factories_cap) {
        unsigned cap = factories_cap * 2 + 8;
        factory *p = realloc(factories, sizeof *p * cap);
        if (!p)
            return false;
        factories = p;
        factories_cap = cap;
    }
    factory *f = &factories[nfactories++];
    memset(f, 0, sizeof *f);
    memcpy(f->factory, id, 16);
    f->fn = fn;
    f->name = name ? cf_retain(name) : 0;
    f->plugin = plugin;
    return true;
}

static bool add_registration(const uint8_t factory_id[16], const uint8_t type[16])
{
    for (unsigned i = 0; i < nregs; i++)
        if (!memcmp(registrations[i].factory, factory_id, 16) && !memcmp(registrations[i].type, type, 16))
            return true;
    if (nregs == regs_cap) {
        unsigned cap = regs_cap * 2 + 8;
        registration *p = realloc(registrations, sizeof *p * cap);
        if (!p)
            return false;
        registrations = p;
        regs_cap = cap;
    }
    memcpy(registrations[nregs].factory, factory_id, 16);
    memcpy(registrations[nregs++].type, type, 16);
    return true;
}

/* A bundle's plug-in information: its factories and the types they make,
 * and dynamic registration's function run. */
static void register_plugin(cf_bundle *b)
{
    gaddr info = bundle_info(b);
    uint32_t facs = 0, types = 0, dyn = 0;
    cfdict_get(info, cfstr_constant("CFPlugInFactories"), &facs);
    cfdict_get(info, cfstr_constant("CFPlugInTypes"), &types);
    cfdict_get(info, cfstr_constant("CFPlugInDynamicRegistration"), &dyn);
    if (!facs && !types && !dyn)
        return;
    b->plugin = true;
    uint32_t *keys, *vals, n = cfdict_entries(facs, &keys, &vals);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t id[16];
        if (cfuuid_parse(keys[i], id))
            add_factory(id, 0, vals[i], b->o.ref);
    }
    free(keys);
    free(vals);
    n = cfdict_entries(types, &keys, &vals);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t type[16], fid[16];
        if (!cfuuid_parse(keys[i], type))
            continue;
        for (uint32_t k = 0; k < cfarray_count(vals[i]); k++)
            if (cfuuid_parse(cfarray_get(vals[i], k), fid))
                add_registration(fid, type);
    }
    free(keys);
    free(vals);
    bool dynamic = dyn == cfbool(true);
    if (cf_type_of(dyn) == CFT_STRING) {
        char *s = cfstr_utf8(dyn);
        dynamic = s && (!strcasecmp(s, "YES") || !strcasecmp(s, "true"));
        free(s);
    }
    if (dynamic) {
        uint32_t name = 0;
        cfdict_get(info, cfstr_constant("CFPlugInDynamicRegisterFunction"), &name);
        gaddr fn = bundle_symbol(b, name ? name : cfstr_constant("CFPlugInDynamicRegister"));
        uint32_t arg = b->o.ref;
        if (fn)
            cf_call(fn, &arg, 1);
    }
}

static void cf_CFPlugInCreate(ppc_cpu *c)
{
    ppc_cpu sub = *c;
    cf_CFBundleCreate(&sub);
    cf_bundle *b = bundle_of(sub.gpr[3]);
    if (b && !b->plugin)
        register_plugin(b);
    RETURN(b ? b->o.ref : 0);
}

static void cf_CFBundleGetPlugIn(ppc_cpu *c)
{
    cf_bundle *b = bundle_of(R(0));
    if (b && !b->plugin)
        register_plugin(b);
    RETURN(b && b->plugin ? b->o.ref : 0);
}

static void cf_CFPlugInGetBundle(ppc_cpu *c) { RETURN(bundle_of(R(0)) ? R(0) : 0); }

static void cf_CFPlugInSetLoadOnDemand(ppc_cpu *c)
{
    cf_bundle *b = bundle_of(R(0));
    if (b)
        b->load_on_demand = (R(1) & 0xFF) != 0;
    RETURN_VOID();
}

static void cf_CFPlugInIsLoadOnDemand(ppc_cpu *c)
{
    cf_bundle *b = bundle_of(R(0));
    RETURN(b && b->load_on_demand);
}

static gaddr factories_for(gaddr type_ref, gaddr plugin)
{
    uint8_t type[16];
    if (!cfuuid_bytes(type_ref, type))
        return 0;
    gaddr a = cfarray_new(0, NULL, 0, true);
    for (unsigned i = 0; i < nregs; i++) {
        if (memcmp(registrations[i].type, type, 16))
            continue;
        factory *f = factory_of(registrations[i].factory);
        if (!f || (plugin && f->plugin != plugin))
            continue;
        gaddr u = cfuuid_new(0, f->factory, false);
        cfarray_append(a, u);
        cf_release(u);
    }
    return a;
}

static void cf_CFPlugInFindFactoriesForPlugInType(ppc_cpu *c) { RETURN(factories_for(R(0), 0)); }
static void cf_CFPlugInFindFactoriesForPlugInTypeInPlugIn(ppc_cpu *c) { RETURN(factories_for(R(0), R(1))); }

/* CFPlugInInstanceCreate(allocator, factoryUUID, typeUUID): the factory's
 * function, called as (allocator, typeUUID). */
static void cf_CFPlugInInstanceCreate(ppc_cpu *c)
{
    uint8_t fid[16], type[16];
    if (!cfuuid_bytes(R(1), fid) || !cfuuid_bytes(R(2), type))
        RETURN(0);
    factory *f = factory_of(fid);
    bool registered = false;
    for (unsigned i = 0; i < nregs; i++)
        registered |= !memcmp(registrations[i].factory, fid, 16) && !memcmp(registrations[i].type, type, 16);
    if (!f || !registered)
        RETURN(0);
    gaddr fn = f->fn ? f->fn : bundle_symbol(bundle_of(f->plugin), f->name);
    if (!fn)
        RETURN(0);
    uint32_t args[2] = { R(0), R(2) };
    RETURN(cf_call(fn, args, 2));
}

static void cf_CFPlugInRegisterFactoryFunction(ppc_cpu *c)
{
    uint8_t id[16];
    RETURN(cfuuid_bytes(R(0), id) && R(1) && add_factory(id, R(1), 0, 0));
}

/* CFPlugInRegisterFactoryFunctionByName(factoryUUID, plugIn, functionName) */
static void cf_CFPlugInRegisterFactoryFunctionByName(ppc_cpu *c)
{
    uint8_t id[16];
    RETURN(cfuuid_bytes(R(0), id) && bundle_of(R(1)) && R(2) && add_factory(id, 0, R(2), R(1)));
}

static void cf_CFPlugInUnregisterFactory(ppc_cpu *c)
{
    uint8_t id[16];
    factory *f = cfuuid_bytes(R(0), id) ? factory_of(id) : NULL;
    if (!f)
        RETURN(false);
    cf_release(f->name);
    *f = factories[--nfactories];
    RETURN(true);
}

static void cf_CFPlugInRegisterPlugInType(ppc_cpu *c)
{
    uint8_t fid[16], type[16];
    RETURN(cfuuid_bytes(R(0), fid) && cfuuid_bytes(R(1), type) && add_registration(fid, type));
}

static void cf_CFPlugInUnregisterPlugInType(ppc_cpu *c)
{
    uint8_t fid[16], type[16];
    if (!cfuuid_bytes(R(0), fid) || !cfuuid_bytes(R(1), type))
        RETURN(false);
    for (unsigned i = 0; i < nregs; i++)
        if (!memcmp(registrations[i].factory, fid, 16) && !memcmp(registrations[i].type, type, 16)) {
            registrations[i] = registrations[--nregs];
            RETURN(true);
        }
    RETURN(false);
}

static void count_instance(gaddr id_ref, int by)
{
    uint8_t id[16];
    factory *f = cfuuid_bytes(id_ref, id) ? factory_of(id) : NULL;
    if (f) {
        f->instances += by;
        cf_bundle *b = bundle_of(f->plugin);
        if (b)
            b->instances += by;
    }
}

static void cf_CFPlugInAddInstanceForFactory(ppc_cpu *c)
{
    count_instance(R(0), 1);
    RETURN_VOID();
}

static void cf_CFPlugInRemoveInstanceForFactory(ppc_cpu *c)
{
    count_instance(R(0), -1);
    RETURN_VOID();
}

/* Plug-in instances: CF's objects with the instance's data after them. */

typedef struct {
    cf_obj o;
    gaddr dealloc, name, get_interface;
} cf_instance;

static void instance_finalize(cf_obj *o)
{
    cf_instance *i = (cf_instance *)o;
    uint32_t data = o->ref + CF_HEADER;
    if (i->dealloc)
        cf_call(i->dealloc, &data, 1);
    cf_release(i->name);
}

static const cf_class instance_class = { "CFPlugInInstance", instance_finalize, NULL, NULL, NULL, NULL };

static void cf_CFPlugInInstanceGetTypeID(ppc_cpu *c) { RETURN(CFT_PLUGIN_INSTANCE); }

/* CFPlugInInstanceCreateWithInstanceDataSize(allocator, instanceDataSize,
 *     deallocateInstanceFunction, factoryName, getInterfaceFunction) */
static void cf_CFPlugInInstanceCreateWithInstanceDataSize(ppc_cpu *c)
{
    if ((int32_t)R(1) < 0)
        RETURN(0);
    cf_instance *i = cf_new(CFT_PLUGIN_INSTANCE, sizeof *i, R(0), R(1));
    if (!i)
        RETURN(0);
    i->dealloc = R(2);
    i->name = cf_retain(R(3));
    i->get_interface = R(4);
    RETURN(i->o.ref);
}

/* CFPlugInInstanceGetInterfaceFunctionTable(instance, interfaceName, ftbl) */
static void cf_CFPlugInInstanceGetInterfaceFunctionTable(ppc_cpu *c)
{
    cf_instance *i = cf_get_type(R(0), CFT_PLUGIN_INSTANCE);
    if (!i || !i->get_interface)
        RETURN(false);
    uint32_t args[3] = { R(0), R(1), R(2) };
    RETURN((cf_call(i->get_interface, args, 3) & 0xFF) != 0);
}

static void cf_CFPlugInInstanceGetFactoryName(ppc_cpu *c)
{
    cf_instance *i = cf_get_type(R(0), CFT_PLUGIN_INSTANCE);
    RETURN(i ? i->name : 0);
}

static void cf_CFPlugInInstanceGetInstanceData(ppc_cpu *c)
{
    RETURN(cf_get_type(R(0), CFT_PLUGIN_INSTANCE) ? R(0) + CF_HEADER : 0);
}

/* ---------------------------------------------------------------------- */

void cfurl_reset(void)
{
    main_bundle = all_bundles = 0;
    free(bundles);
    bundles = NULL;
    nbundles = bundles_cap = 0;
    free(factories);
    factories = NULL;
    nfactories = factories_cap = 0;
    free(registrations);
    registrations = NULL;
    nregs = regs_cap = 0;
    cf_register_class(CFT_URL, &url_class);
    cf_register_class(CFT_BUNDLE, &bundle_class);
    cf_register_class(CFT_PLUGIN_INSTANCE, &instance_class);
}

const ppc_export cfurl_exports[] = {
    { "CFURLGetTypeID", cf_CFURLGetTypeID },
    { "CFURLCreateWithBytes", cf_CFURLCreateWithBytes },
    { "CFURLCreateData", cf_CFURLCreateData },
    { "CFURLCreateWithString", cf_CFURLCreateWithString },
    { "CFURLCreateWithFileSystemPath", cf_CFURLCreateWithFileSystemPath },
    { "CFURLCreateFromFileSystemRepresentation", cf_CFURLCreateFromFileSystemRepresentation },
    { "CFURLCreateWithFileSystemPathRelativeToBase", cf_CFURLCreateWithFileSystemPathRelativeToBase },
    { "CFURLCreateFromFileSystemRepresentationRelativeToBase",
      cf_CFURLCreateFromFileSystemRepresentationRelativeToBase },
    { "CFURLGetFileSystemRepresentation", cf_CFURLGetFileSystemRepresentation },
    { "CFURLCopyAbsoluteURL", cf_CFURLCopyAbsoluteURL },
    { "CFURLGetString", cf_CFURLGetString },
    { "CFURLGetBaseURL", cf_CFURLGetBaseURL },
    { "CFURLCanBeDecomposed", cf_CFURLCanBeDecomposed },
    { "CFURLCopyScheme", cf_CFURLCopyScheme },
    { "CFURLCopyNetLocation", cf_CFURLCopyNetLocation },
    { "CFURLCopyPath", cf_CFURLCopyPath },
    { "CFURLCopyStrictPath", cf_CFURLCopyStrictPath },
    { "CFURLCopyFileSystemPath", cf_CFURLCopyFileSystemPath },
    { "CFURLHasDirectoryPath", cf_CFURLHasDirectoryPath },
    { "CFURLCopyResourceSpecifier", cf_CFURLCopyResourceSpecifier },
    { "CFURLCopyHostName", cf_CFURLCopyHostName },
    { "CFURLGetPortNumber", cf_CFURLGetPortNumber },
    { "CFURLCopyUserName", cf_CFURLCopyUserName },
    { "CFURLCopyPassword", cf_CFURLCopyPassword },
    { "CFURLCopyParameterString", cf_CFURLCopyParameterString },
    { "CFURLCopyQueryString", cf_CFURLCopyQueryString },
    { "CFURLCopyFragment", cf_CFURLCopyFragment },
    { "CFURLCopyLastPathComponent", cf_CFURLCopyLastPathComponent },
    { "CFURLCopyPathExtension", cf_CFURLCopyPathExtension },
    { "CFURLCreateCopyAppendingPathComponent", cf_CFURLCreateCopyAppendingPathComponent },
    { "CFURLCreateCopyDeletingLastPathComponent", cf_CFURLCreateCopyDeletingLastPathComponent },
    { "CFURLCreateCopyAppendingPathExtension", cf_CFURLCreateCopyAppendingPathExtension },
    { "CFURLCreateCopyDeletingPathExtension", cf_CFURLCreateCopyDeletingPathExtension },
    { "CFURLCreateStringByReplacingPercentEscapes", cf_CFURLCreateStringByReplacingPercentEscapes },
    { "CFURLCreateStringByAddingPercentEscapes", cf_CFURLCreateStringByAddingPercentEscapes },
    { "CFURLCreateFromFSRef", cf_CFURLCreateFromFSRef },
    { "CFURLGetFSRef", cf_CFURLGetFSRef },
    { "CFURLCreateDataAndPropertiesFromResource", cf_CFURLCreateDataAndPropertiesFromResource },
    { "CFURLWriteDataAndPropertiesToResource", cf_CFURLWriteDataAndPropertiesToResource },
    { "CFURLDestroyResource", cf_CFURLDestroyResource },
    { "CFURLCreatePropertyFromResource", cf_CFURLCreatePropertyFromResource },
    { "CFBundleGetMainBundle", cf_CFBundleGetMainBundle },
    { "CFBundleGetBundleWithIdentifier", cf_CFBundleGetBundleWithIdentifier },
    { "CFBundleGetAllBundles", cf_CFBundleGetAllBundles },
    { "CFBundleGetTypeID", cf_CFBundleGetTypeID },
    { "CFBundleCreate", cf_CFBundleCreate },
    { "CFBundleCreateBundlesFromDirectory", cf_CFBundleCreateBundlesFromDirectory },
    { "CFBundleCopyBundleURL", cf_CFBundleCopyBundleURL },
    { "CFBundleGetValueForInfoDictionaryKey", cf_CFBundleGetValueForInfoDictionaryKey },
    { "CFBundleGetInfoDictionary", cf_CFBundleGetInfoDictionary },
    { "CFBundleGetLocalInfoDictionary", cf_CFBundleGetLocalInfoDictionary },
    { "CFBundleGetPackageInfo", cf_CFBundleGetPackageInfo },
    { "CFBundleGetIdentifier", cf_CFBundleGetIdentifier },
    { "CFBundleGetVersionNumber", cf_CFBundleGetVersionNumber },
    { "CFBundleGetDevelopmentRegion", cf_CFBundleGetDevelopmentRegion },
    { "CFBundleCopySupportFilesDirectoryURL", cf_CFBundleCopySupportFilesDirectoryURL },
    { "CFBundleCopyResourcesDirectoryURL", cf_CFBundleCopyResourcesDirectoryURL },
    { "CFBundleCopyPrivateFrameworksURL", cf_CFBundleCopyPrivateFrameworksURL },
    { "CFBundleCopySharedFrameworksURL", cf_CFBundleCopySharedFrameworksURL },
    { "CFBundleCopySharedSupportURL", cf_CFBundleCopySharedSupportURL },
    { "CFBundleCopyBuiltInPlugInsURL", cf_CFBundleCopyBuiltInPlugInsURL },
    { "CFBundleCopyInfoDictionaryInDirectory", cf_CFBundleCopyInfoDictionaryInDirectory },
    { "CFBundleGetPackageInfoInDirectory", cf_CFBundleGetPackageInfoInDirectory },
    { "CFBundleCopyResourceURL", cf_CFBundleCopyResourceURL },
    { "CFBundleCopyResourceURLsOfType", cf_CFBundleCopyResourceURLsOfType },
    { "CFBundleCopyLocalizedString", cf_CFBundleCopyLocalizedString },
    { "CFBundleCopyResourceURLInDirectory", cf_CFBundleCopyResourceURLInDirectory },
    { "CFBundleCopyResourceURLsOfTypeInDirectory", cf_CFBundleCopyResourceURLsOfTypeInDirectory },
    { "CFBundleCopyBundleLocalizations", cf_CFBundleCopyBundleLocalizations },
    { "CFBundleCopyPreferredLocalizationsFromArray", cf_CFBundleCopyPreferredLocalizationsFromArray },
    { "CFBundleCopyResourceURLForLocalization", cf_CFBundleCopyResourceURLForLocalization },
    { "CFBundleCopyResourceURLsOfTypeForLocalization", cf_CFBundleCopyResourceURLsOfTypeForLocalization },
    { "CFBundleCopyExecutableURL", cf_CFBundleCopyExecutableURL },
    { "CFBundleIsExecutableLoaded", cf_CFBundleIsExecutableLoaded },
    { "CFBundleLoadExecutable", cf_CFBundleLoadExecutable },
    { "CFBundleUnloadExecutable", cf_CFBundleUnloadExecutable },
    { "CFBundleGetFunctionPointerForName", cf_CFBundleGetFunctionPointerForName },
    { "CFBundleGetFunctionPointersForNames", cf_CFBundleGetFunctionPointersForNames },
    { "CFBundleGetDataPointerForName", cf_CFBundleGetDataPointerForName },
    { "CFBundleGetDataPointersForNames", cf_CFBundleGetFunctionPointersForNames },
    { "CFBundleCopyAuxiliaryExecutableURL", cf_CFBundleCopyAuxiliaryExecutableURL },
    { "CFBundleGetPlugIn", cf_CFBundleGetPlugIn },
    { "CFBundleOpenBundleResourceMap", cf_CFBundleOpenBundleResourceMap },
    { "CFBundleOpenBundleResourceFiles", cf_CFBundleOpenBundleResourceFiles },
    { "CFBundleCloseBundleResourceMap", cf_CFBundleCloseBundleResourceMap },
    { "CFPlugInGetTypeID", cf_CFBundleGetTypeID },
    { "CFPlugInCreate", cf_CFPlugInCreate },
    { "CFPlugInGetBundle", cf_CFPlugInGetBundle },
    { "CFPlugInSetLoadOnDemand", cf_CFPlugInSetLoadOnDemand },
    { "CFPlugInIsLoadOnDemand", cf_CFPlugInIsLoadOnDemand },
    { "CFPlugInFindFactoriesForPlugInType", cf_CFPlugInFindFactoriesForPlugInType },
    { "CFPlugInFindFactoriesForPlugInTypeInPlugIn", cf_CFPlugInFindFactoriesForPlugInTypeInPlugIn },
    { "CFPlugInInstanceCreate", cf_CFPlugInInstanceCreate },
    { "CFPlugInRegisterFactoryFunction", cf_CFPlugInRegisterFactoryFunction },
    { "CFPlugInRegisterFactoryFunctionByName", cf_CFPlugInRegisterFactoryFunctionByName },
    { "CFPlugInUnregisterFactory", cf_CFPlugInUnregisterFactory },
    { "CFPlugInRegisterPlugInType", cf_CFPlugInRegisterPlugInType },
    { "CFPlugInUnregisterPlugInType", cf_CFPlugInUnregisterPlugInType },
    { "CFPlugInAddInstanceForFactory", cf_CFPlugInAddInstanceForFactory },
    { "CFPlugInRemoveInstanceForFactory", cf_CFPlugInRemoveInstanceForFactory },
    { "CFPlugInInstanceGetInterfaceFunctionTable", cf_CFPlugInInstanceGetInterfaceFunctionTable },
    { "CFPlugInInstanceGetFactoryName", cf_CFPlugInInstanceGetFactoryName },
    { "CFPlugInInstanceGetInstanceData", cf_CFPlugInInstanceGetInstanceData },
    { "CFPlugInInstanceGetTypeID", cf_CFPlugInInstanceGetTypeID },
    { "CFPlugInInstanceCreateWithInstanceDataSize", cf_CFPlugInInstanceCreateWithInstanceDataSize },
};
const unsigned cfurl_export_count = sizeof cfurl_exports / sizeof cfurl_exports[0];
