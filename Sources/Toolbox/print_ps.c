/*
 * Recorded QuickDraw pages as PostScript, for a LaserWriter (pap.c).
 *
 * The same pages the host turns into PDF (PrintRenderer.swift), written
 * as a PostScript Level 1 program as the LaserWriter driver did: shapes
 * are paths painted in the grey their pattern makes, text is set in the
 * printer's own fonts (the LaserWriter's substitutions for the classic
 * bitmap families) fitted to the width it had on screen, and pictures'
 * bitmaps are grey images.  Text is re-encoded in the Macintosh character
 * set, as LaserPrep did.
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chooser.h"

typedef struct {
    char *b;
    size_t len, cap;
} buf;

static void put(buf *o, const char *fmt, ...)
{
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int n = vsnprintf(o->b + o->len, o->cap - o->len, fmt, ap);
        va_end(ap);
        if (n >= 0 && o->len + (size_t)n < o->cap) {
            o->len += (size_t)n;
            return;
        }
        size_t want = o->cap * 2 + (size_t)(n > 0 ? n : 0) + 4096;
        char *g = realloc(o->b, want);
        if (!g)
            return;
        o->b = g;
        o->cap = want;
    }
}

/* The Macintosh character set's glyphs from 128 up (Adobe's names). */
static const char *const mac_upper[128] = {
    "Adieresis", "Aring", "Ccedilla", "Eacute", "Ntilde", "Odieresis", "Udieresis", "aacute", "agrave", "acircumflex",
    "adieresis", "atilde", "aring", "ccedilla", "eacute", "egrave", "ecircumflex", "edieresis", "iacute", "igrave",
    "icircumflex", "idieresis", "ntilde", "oacute", "ograve", "ocircumflex", "odieresis", "otilde", "uacute", "ugrave",
    "ucircumflex", "udieresis", "dagger", "degree", "cent", "sterling", "section", "bullet", "paragraph",
    "germandbls", "registered", "copyright", "trademark", "acute", "dieresis", "notequal", "AE", "Oslash", "infinity",
    "plusminus", "lessequal", "greaterequal", "yen", "mu", "partialdiff", "summation", "product", "pi", "integral",
    "ordfeminine", "ordmasculine", "Omega", "ae", "oslash", "questiondown", "exclamdown", "logicalnot", "radical",
    "florin", "approxequal", "Delta", "guillemotleft", "guillemotright", "ellipsis", "space", "Agrave", "Atilde",
    "Otilde", "OE", "oe", "endash", "emdash", "quotedblleft", "quotedblright", "quoteleft", "quoteright", "divide",
    "lozenge", "ydieresis", "Ydieresis", "fraction", "currency", "guilsinglleft", "guilsinglright", "fi", "fl",
    "daggerdbl", "periodcentered", "quotesinglbase", "quotedblbase", "perthousand", "Acircumflex", "Ecircumflex",
    "Aacute", "Edieresis", "Egrave", "Iacute", "Icircumflex", "Idieresis", "Igrave", "Oacute", "Ocircumflex", "apple",
    "Ograve", "Uacute", "Ucircumflex", "Ugrave", "dotlessi", "circumflex", "tilde", "macron", "breve", "dotaccent",
    "ring", "cedilla", "hungarumlaut", "ogonek", "caron",
};

/* The printer's font for a classic family and style, as the LaserWriter
 * substituted Times for New York, Helvetica for Geneva and Courier for
 * Monaco. */
static const char *ps_font(const machine_print_op *op, bool *mac_encoded)
{
    static char name[80];
    const char *fam = op->font_name;
    bool bold = op->face & 1, italic = op->face & 2;
    int id = op->font_id;
    const char *base = "Helvetica";
    if (id == 2 || id == 20 || strstr(fam, "Times") || strstr(fam, "New York"))
        base = "Times";
    else if (id == 4 || id == 22 || strstr(fam, "Courier") || strstr(fam, "Monaco"))
        base = "Courier";
    else if (id == 23 || !strcmp(fam, "Symbol"))
        base = "Symbol";
    else if (strstr(fam, "Palatino"))
        base = "Palatino";
    else if (strstr(fam, "Bookman"))
        base = "Bookman";
    else if (strstr(fam, "Avant Garde") || strstr(fam, "AvantGarde"))
        base = "AvantGarde";
    else if (strstr(fam, "Century"))
        base = "NewCenturySchlbk";
    else if (strstr(fam, "Zapf Chancery"))
        base = "ZapfChancery";
    else if (strstr(fam, "Zapf Dingbats") || strstr(fam, "Dingbats"))
        base = "ZapfDingbats";
    else if (strstr(fam, "Narrow"))
        base = "Helvetica-Narrow";
    else if (id == 0 || strstr(fam, "Chicago"))
        bold = true; /* Chicago: Helvetica Bold */
    *mac_encoded = strcmp(base, "Symbol") && strcmp(base, "ZapfDingbats");
    static const char *const roman[4] = { "-Roman", "-Bold", "-Italic", "-BoldItalic" };
    static const char *const sans[4] = { "", "-Bold", "-Oblique", "-BoldOblique" };
    static const char *const bookman[4] = { "-Light", "-Demi", "-LightItalic", "-DemiItalic" };
    static const char *const avant[4] = { "-Book", "-Demi", "-BookOblique", "-DemiOblique" };
    int s = (bold ? 1 : 0) | (italic ? 2 : 0);
    if (!strcmp(base, "Times") || !strcmp(base, "Palatino") || !strcmp(base, "NewCenturySchlbk"))
        snprintf(name, sizeof name, "%s%s", base, roman[s]);
    else if (!strcmp(base, "Bookman"))
        snprintf(name, sizeof name, "%s%s", base, bookman[s]);
    else if (!strcmp(base, "AvantGarde"))
        snprintf(name, sizeof name, "%s%s", base, avant[s]);
    else if (!strcmp(base, "ZapfChancery"))
        snprintf(name, sizeof name, "ZapfChancery-MediumItalic");
    else if (!strcmp(base, "Symbol") || !strcmp(base, "ZapfDingbats"))
        snprintf(name, sizeof name, "%s", base);
    else
        snprintf(name, sizeof name, "%s%s", base, sans[s]);
    return name;
}

static double coverage(const uint8_t pat[8])
{
    int n = 0;
    for (int i = 0; i < 8; i++)
        for (int b = 0; b < 8; b++)
            n += (pat[i] >> b) & 1;
    return n / 64.0;
}

/* The grey an operation paints, or -1 for nothing (PostScript paints
 * opaquely: "or" and "bic" with a pattern lay its grey down). */
static double grey_of(const machine_print_op *op)
{
    double cov = coverage(op->pattern);
    if (op->mode & 4)
        cov = 1 - cov;
    double ink = op->fg_black ? 0 : 1;
    switch (op->mode & 3) {
    case 0: return 1 - cov * (1 - ink);
    case 1: return cov > 0 ? 1 - cov * (1 - ink) : -1;
    case 2: return -1; /* inverting: paper can't */
    default: return cov > 0 ? 1 : -1;
    }
}

static void rect_path(buf *o, mprect r)
{
    put(o, "%d %d %d %d R ", r.left, r.top, r.right - r.left, r.bottom - r.top);
}

static void oval_path(buf *o, double l, double t, double r, double b)
{
    if (r <= l || b <= t)
        return;
    put(o, "%.2f %.2f %.2f %.2f O ", (l + r) / 2, (t + b) / 2, (r - l) / 2, (b - t) / 2);
}

static void rrect_path(buf *o, double l, double t, double r, double b, double ow, double oh)
{
    if (r <= l || b <= t)
        return;
    double rx = fmin(fmax(ow / 2, 0), (r - l) / 2), ry = fmin(fmax(oh / 2, 0), (b - t) / 2);
    if (rx <= 0 || ry <= 0) {
        put(o, "%.2f %.2f %.2f %.2f R ", l, t, r - l, b - t);
        return;
    }
    put(o, "%.2f %.2f %.2f %.2f %.2f %.2f RR ", l, t, r, b, rx, ry);
}

/* An arc's points: QuickDraw's angles start at 12 o'clock, clockwise. */
static void arc_points(buf *o, double l, double t, double r, double b, double start, double angle, bool first)
{
    int steps = (int)fmax(2, fabs(angle) / 2);
    for (int i = 0; i <= steps; i++) {
        double a = (start + angle * i / steps) * M_PI / 180;
        double x = (l + r) / 2 + (r - l) / 2 * sin(a), y = (t + b) / 2 - (b - t) / 2 * cos(a);
        put(o, "%.2f %.2f %s ", x, y, first && i == 0 ? "moveto" : "lineto");
    }
}

/* A line's pen sweep: the hull of the pen at its two ends. */
static void line_path(buf *o, const machine_print_op *op)
{
    double w = op->pen_w, h = op->pen_h;
    double p[8][2] = { { op->h0, op->v0 }, { op->h0 + w, op->v0 }, { op->h0, op->v0 + h }, { op->h0 + w, op->v0 + h },
                       { op->h1, op->v1 }, { op->h1 + w, op->v1 }, { op->h1, op->v1 + h }, { op->h1 + w, op->v1 + h } };
    /* monotone chain */
    for (int i = 1; i < 8; i++)
        for (int k = i; k > 0 && (p[k][0] < p[k - 1][0] || (p[k][0] == p[k - 1][0] && p[k][1] < p[k - 1][1])); k--) {
            double t0 = p[k][0], t1 = p[k][1];
            p[k][0] = p[k - 1][0], p[k][1] = p[k - 1][1];
            p[k - 1][0] = t0, p[k - 1][1] = t1;
        }
    double hull[16][2];
    int n = 0;
#define CROSS(a, b, c) (((b)[0] - (a)[0]) * ((c)[1] - (a)[1]) - ((b)[1] - (a)[1]) * ((c)[0] - (a)[0]))
    for (int i = 0; i < 8; i++) {
        while (n >= 2 && CROSS(hull[n - 2], hull[n - 1], p[i]) <= 0)
            n--;
        hull[n][0] = p[i][0], hull[n][1] = p[i][1], n++;
    }
    for (int i = 6, lo = n + 1; i >= 0; i--) {
        while (n >= lo && CROSS(hull[n - 2], hull[n - 1], p[i]) <= 0)
            n--;
        hull[n][0] = p[i][0], hull[n][1] = p[i][1], n++;
    }
#undef CROSS
    n--;
    for (int i = 0; i < n; i++)
        put(o, "%.2f %.2f %s ", hull[i][0], hull[i][1], i ? "lineto" : "moveto");
    put(o, "closepath ");
}

static void ps_string(buf *o, const uint8_t *s, int n)
{
    put(o, "(");
    for (int i = 0; i < n; i++) {
        uint8_t c = s[i];
        if (c == '(' || c == ')' || c == '\\')
            put(o, "\\%c", c);
        else if (c < 32 || c >= 127)
            put(o, "\\%03o", c == 13 ? 32 : c);
        else
            put(o, "%c", c);
    }
    put(o, ")");
}

static void text(buf *o, const machine_print_op *op)
{
    if (op->len <= 0 || !op->text)
        return;
    bool white = !op->fg_black || (op->mode & 3) == 3;
    bool mac;
    const char *font = ps_font(op, &mac);
    put(o, "gsave %d setgray ", white ? 1 : 0);
    if (op->angle != 0 || op->flip_h || op->flip_v)
        put(o, "%.2f %.2f translate %.3f rotate %d %d scale %.2f %.2f translate ", op->rot_h, op->rot_v, op->angle,
            op->flip_h ? -1 : 1, op->flip_v ? -1 : 1, -op->rot_h, -op->rot_v);
    put(o, "/%s%s %d F %d %d translate ", font, mac ? "-Mac" : "", op->size > 0 ? op->size : 12, op->h0, op->v0);
    ps_string(o, op->text, op->len);
    put(o, " %d T", op->width);
    if (op->face & 4) /* underline, as QuickDraw draws it */
        put(o, " grestore gsave %d setgray newpath %d %d %d 1 R fill", white ? 1 : 0, op->h0, op->v0 + 1, op->width);
    put(o, " grestore\n");
}

static void image(buf *o, const machine_print_op *op)
{
    int w = op->image_w, h = op->image_h;
    if (w <= 0 || h <= 0 || !op->image)
        return;
    put(o, "gsave %d %d translate %d %d scale /pix %d string def %d %d 8 [%d 0 0 %d 0 0] "
           "{currentfile pix readhexstring pop} image\n",
        op->r.left, op->r.top, op->r.right - op->r.left, op->r.bottom - op->r.top, w, w, h, w, h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const uint8_t *p = op->image + ((size_t)y * (size_t)w + (size_t)x) * 4;
            /* premultiplied, over white paper */
            int g = (p[0] * 77 + p[1] * 151 + p[2] * 28) >> 8;
            g += 255 - p[3];
            if (g > 255)
                g = 255;
            put(o, "%02x", g);
            if ((x & 31) == 31)
                put(o, "\n");
        }
        put(o, "\n");
    }
    put(o, "grestore\n");
}

static void shape(buf *o, const machine_print_op *op)
{
    double grey = grey_of(op);
    if (grey < 0)
        return;
    bool frame = op->verb == MACHINE_VERB_FRAME;
    double pw = op->pen_w, ph = op->pen_h;
    mprect r = op->r;
    put(o, "newpath ");
    switch (op->kind) {
    case MACHINE_PRINT_RECT:
        rect_path(o, r);
        if (frame)
            put(o, "%.2f %.2f %.2f %.2f R ", r.left + pw, r.top + ph, r.right - r.left - 2 * pw, r.bottom - r.top - 2 * ph);
        break;
    case MACHINE_PRINT_OVAL:
        oval_path(o, r.left, r.top, r.right, r.bottom);
        if (frame)
            oval_path(o, r.left + pw, r.top + ph, r.right - pw, r.bottom - ph);
        break;
    case MACHINE_PRINT_RRECT:
        rrect_path(o, r.left, r.top, r.right, r.bottom, op->a, op->b);
        if (frame)
            rrect_path(o, r.left + pw, r.top + ph, r.right - pw, r.bottom - ph, op->a - 2 * pw, op->b - 2 * ph);
        break;
    case MACHINE_PRINT_ARC:
        if (frame) {
            arc_points(o, r.left, r.top, r.right, r.bottom, op->a, op->b, true);
            arc_points(o, r.left + pw, r.top + ph, r.right - pw, r.bottom - ph, op->a + op->b, -op->b, false);
        } else {
            put(o, "%.2f %.2f moveto ", (r.left + r.right) / 2.0, (r.top + r.bottom) / 2.0);
            arc_points(o, r.left, r.top, r.right, r.bottom, op->a, op->b, false);
        }
        put(o, "closepath ");
        frame = false;
        break;
    case MACHINE_PRINT_REGION:
        for (int i = 0; i < op->nrects; i++)
            rect_path(o, op->rects[i]);
        frame = false;
        break;
    case MACHINE_PRINT_LINE:
        if (op->pen_w <= 0 || op->pen_h <= 0)
            return;
        line_path(o, op);
        frame = false;
        break;
    default: return;
    }
    put(o, "%.3f setgray %s\n", grey, frame ? "eofill" : "fill");
}

static void collect_fonts(const machine_print_job *job, char (*fonts)[80], int *n, int max)
{
    for (int p = 0; p < job->npages; p++)
        for (int i = 0; i < job->pages[p].nops; i++) {
            const machine_print_op *op = &job->pages[p].ops[i];
            if (op->kind != MACHINE_PRINT_TEXT)
                continue;
            bool mac;
            const char *f = ps_font(op, &mac);
            if (!mac)
                continue;
            bool seen = false;
            for (int k = 0; k < *n && !seen; k++)
                seen = !strcmp(fonts[k], f);
            if (!seen && *n < max)
                snprintf(fonts[(*n)++], 80, "%s", f);
        }
}

size_t print_postscript(const machine_print_job *job, char **out)
{
    buf o = { malloc(65536), 0, 65536 };
    if (!o.b)
        return 0;
    double sx = job->hres > 72 ? 72.0 / job->hres : 1, sy = job->vres > 72 ? 72.0 / job->vres : 1;
    double pw = (job->paper.right - job->paper.left) * sx, ph = (job->paper.bottom - job->paper.top) * sy;
    put(&o, "%%!PS-Adobe-3.0\n%%%%Creator: Blue Box\n%%%%Title: ");
    for (const char *t = job->title; *t; t++)
        put(&o, "%c", *t >= 32 && *t < 127 ? *t : '?');
    put(&o, "\n%%%%Pages: %d\n%%%%BoundingBox: 0 0 %d %d\n%%%%LanguageLevel: 1\n%%%%EndComments\n", job->npages,
        (int)ceil(pw), (int)ceil(ph));
    put(&o, "%%%%BeginProlog\n/bd {bind def} bind def\n"
            "/R {4 2 roll moveto 1 index 0 rlineto 0 exch rlineto neg 0 rlineto closepath} bd\n"
            "/O {matrix currentmatrix 5 1 roll 4 2 roll translate scale 0 0 1 0 360 arc closepath setmatrix} bd\n"
            "/RR {/ry exch def /rx exch def /b exch def /r exch def /t exch def /l exch def\n"
            " l rx add t moveto r t r b rx ry add 2 div arcto 4 {pop} repeat\n"
            " r b l b rx ry add 2 div arcto 4 {pop} repeat l b l t rx ry add 2 div arcto 4 {pop} repeat\n"
            " l t r t rx ry add 2 div arcto 4 {pop} repeat closepath} bd\n"
            "/F {exch findfont exch scalefont setfont} bd\n"
            "/T {1 index stringwidth pop dup 0 gt {div} {pop pop 1} ifelse -1 scale 0 0 moveto show} bd\n"
            "/MacEncoding StandardEncoding 256 array copy def\n"
            "MacEncoding 39 /quotesingle put MacEncoding 96 /grave put\nMacEncoding 128 [");
    for (int i = 0; i < 128; i++)
        put(&o, "%s/%s", i % 8 ? " " : "\n", mac_upper[i]);
    put(&o, "] putinterval\n"
            "/RF {findfont dup length dict begin {1 index /FID ne {def} {pop pop} ifelse} forall\n"
            " /Encoding MacEncoding def currentdict end definefont pop} bd\n%%%%EndProlog\n%%%%BeginSetup\n");
    put(&o, "/setpagedevice where {pop << /PageSize [%.2f %.2f] >> setpagedevice} if\n", pw, ph);
    char fonts[32][80];
    int nf = 0;
    collect_fonts(job, fonts, &nf, 32);
    for (int i = 0; i < nf; i++)
        put(&o, "/%s-Mac /%s RF\n", fonts[i], fonts[i]);
    put(&o, "%%%%EndSetup\n");
    for (int p = 0; p < job->npages; p++) {
        put(&o, "%%%%Page: %d %d\nsave\n0 %.2f translate %.5f %.5f scale %d %d translate\n", p + 1, p + 1, ph, sx, -sy,
            -job->paper.left, -job->paper.top);
        const machine_print_page *page = &job->pages[p];
        for (int i = 0; i < page->nops; i++) {
            const machine_print_op *op = &page->ops[i];
            if (op->nclip <= 0)
                continue; /* clipped away */
            put(&o, "gsave newpath ");
            for (int k = 0; k < op->nclip; k++)
                rect_path(&o, op->clip[k]);
            put(&o, "clip newpath\n");
            if (op->kind == MACHINE_PRINT_TEXT)
                text(&o, op);
            else if (op->kind == MACHINE_PRINT_IMAGE)
                image(&o, op);
            else
                shape(&o, op);
            put(&o, "grestore\n");
        }
        put(&o, "restore\nshowpage\n");
    }
    put(&o, "%%%%Trailer\n%%%%EOF\n");
    *out = o.b;
    return o.len;
}

char *machine_postscript(const machine_print_job *job, size_t *len)
{
    char *ps = NULL;
    *len = print_postscript(job, &ps);
    return ps;
}
