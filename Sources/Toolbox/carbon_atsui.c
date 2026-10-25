/*
 * ATSUI, Apple Type Services for Unicode Imaging (ATSUnicode.h), on
 * QuickDraw text: a layout's Unicode is drawn and measured run by run in
 * the current port, each run's style giving the port its font, size,
 * face and colour, the text in Mac OS Roman (what the families here
 * hold; other characters are '?').
 *
 * Styles and layouts are small blocks (a tag and an index) standing for
 * records kept here: a style's attributes, font features and variations;
 * a layout's text (a pointer or a handle, an offset and a length into
 * it), its style runs, layout and line controls and soft line breaks.
 * Lines are drawn left to right, unrotated and unjustified; a line width
 * with a flush factor places the text in it.  Font IDs are the families'
 * numbers with their QuickDraw style.  The glyphs are bitmaps, so there
 * are no outlines to give, nor features, variations or tracking tables.
 */
#include <stdlib.h>
#include <string.h>

#include "carbon_svc.h"
#include "qd_internal.h"

enum {
    kATSUInvalidTextLayoutErr = -8790,
    kATSUInvalidStyleErr = -8791,
    kATSUInvalidTextRangeErr = -8792,
    kATSUFontsMatched = -8793,
    kATSUFontsNotMatched = -8794,
    kATSUNoCorrespondingFontErr = -8795,
    kATSUInvalidFontErr = -8796,
    kATSUInvalidAttributeValueErr = -8797,
    kATSUInvalidAttributeSizeErr = -8798,
    kATSUInvalidAttributeTagErr = -8799,
    kATSUInvalidCacheErr = -8800,
    kATSUNotSetErr = -8801,
    kATSUNoStyleRunsAssignedErr = -8802,
    kATSUNoFontScalerAvailableErr = -8806,
    kATSULineBreakInWord = -8808,
};
/* Tags */
enum {
    kATSULineWidthTag = 1, kATSULineRotationTag, kATSULineDirectionTag, kATSULineJustificationFactorTag,
    kATSULineFlushFactorTag, kATSULineBaselineValuesTag, kATSULineLayoutOptionsTag, kATSULineAscentTag,
    kATSULineDescentTag, kATSULineLangRegionTag, kATSULineTextLocatorTag, kATSULineTruncationTag,
    kATSULineFontFallbacksTag,
    kATSUCGContextTag = 32767,
    kATSUQDBoldfaceTag = 256, kATSUQDItalicTag, kATSUQDUnderlineTag, kATSUQDCondensedTag, kATSUQDExtendedTag,
    kATSUFontTag, kATSUSizeTag, kATSUColorTag, kATSULangRegionTag, kATSUVerticalCharacterTag, kATSUImposeWidthTag,
    kATSUBeforeWithStreamShiftTag, kATSUAfterWithStreamShiftTag, kATSUCrossStreamShiftTag, kATSUTrackingTag,
    kATSUHangingInhibitFactorTag, kATSUKerningInhibitFactorTag, kATSUDecompositionFactorTag,
    kATSUBaselineClassTag, kATSUPriorityJustOverrideTag, kATSUNoLigatureSplitTag, kATSUNoCaretAngleTag,
    kATSUSuppressCrossKerningTag, kATSUNoOpticalAlignmentTag, kATSUForceHangingTag,
    kATSUNoSpecialJustificationTag, kATSUStyleTextLocatorTag, kATSUStyleRenderingOptionsTag,
    kATSUMaxATSUITagValue = 65535,
};
enum { kATSUStyleUnequal = 0, kATSUStyleContains, kATSUStyleEquals, kATSUStyleContainedBy };
enum { kATSUByCharacter = 0, kATSUByTypographicCluster = 1, kATSUByWord = 2, kATSUByCharacterCluster = 3 };
#define kATSUUseGrafPortPenLoc 0xFFFFFFFFu
#define kATSUClearAll 0xFFFFFFFFu
#define kATSUFromTextBeginning 0xFFFFFFFFu
#define kATSUToTextEnd 0xFFFFFFFFu
#define kATSUUseLineControlWidth 0x7FFFFFFFu
enum { kFontNoName = -1, kFontNoPlatform = -1, kFontNoScript = -1, kFontNoLanguage = -1 };
enum { kFontFamilyName = 1, kFontStyleName = 2, kFontUniqueName = 3, kFontFullName = 4 };
enum { kFontMacintoshPlatform = 1, kFontRomanScript = 0, kFontEnglishLanguage = 0 };
enum { bold = 1, italic = 2, underline = 4, condense = 32, extend = 64 };

#define STYLE_TAG 0x41545373u  /* 'ATSs' */
#define LAYOUT_TAG 0x4154536cu /* 'ATSl' */

typedef struct {
    uint32_t tag, size;
    uint8_t *data;
} attr;
typedef struct {
    int n;
    attr *a;
} attrset;

enum { MAX_FEATURES = 64, MAX_VARIATIONS = 32 };
typedef struct {
    gaddr addr;
    attrset attrs;
    int nfeat, nvar;
    uint16_t feat_type[MAX_FEATURES], feat_sel[MAX_FEATURES];
    uint32_t var_axis[MAX_VARIATIONS], var_val[MAX_VARIATIONS];
    uint32_t refcon;
} style_t;

typedef struct {
    uint32_t start, len;
    gaddr style;
} run_t;
typedef struct {
    uint32_t start;
    attrset a;
} linectl;
typedef struct {
    gaddr addr;
    gaddr text;
    bool handle;
    uint32_t offset, length, total;
    run_t *runs;
    int nruns;
    attrset controls;
    linectl *lines;
    int nlines;
    uint32_t *breaks;
    int nbreaks;
    uint32_t refcon;
    bool transient;
} layout_t;

static style_t **styles;
static int nstyles;
static layout_t **layouts;
static int nlayouts;
static uint32_t fallbacks[64];
static int nfallbacks;
static uint16_t fallback_method;
static gaddr scratch; /* FontInfo */

/* ---------------------------------------------------------------------- */
/* Attribute sets */

static void attrs_clear(attrset *s)
{
    for (int i = 0; i < s->n; i++)
        free(s->a[i].data);
    free(s->a);
    s->a = NULL;
    s->n = 0;
}

static attr *attrs_find(const attrset *s, uint32_t tag)
{
    for (int i = 0; i < s->n; i++)
        if (s->a[i].tag == tag)
            return &s->a[i];
    return NULL;
}

static void attrs_set(attrset *s, uint32_t tag, const uint8_t *data, uint32_t size)
{
    attr *a = attrs_find(s, tag);
    if (!a) {
        s->a = realloc(s->a, sizeof *s->a * (size_t)(s->n + 1));
        a = &s->a[s->n++];
        a->tag = tag;
        a->data = NULL;
    }
    free(a->data);
    a->data = malloc(size ? size : 1);
    memcpy(a->data, data, size);
    a->size = size;
}

static void attrs_remove(attrset *s, uint32_t tag)
{
    for (int i = 0; i < s->n; i++)
        if (s->a[i].tag == tag) {
            free(s->a[i].data);
            s->a[i] = s->a[--s->n];
            return;
        }
}

static void attrs_copy(attrset *dst, const attrset *src)
{
    attrs_clear(dst);
    for (int i = 0; i < src->n; i++)
        attrs_set(dst, src->a[i].tag, src->a[i].data, src->a[i].size);
}

static bool attr_equal(const attr *a, const attr *b)
{
    return a->size == b->size && !memcmp(a->data, b->data, a->size);
}

/* The size a tag's value has (0: any), or -1 for a tag of Apple's range
 * that isn't one. */
static int tag_size(uint32_t tag, bool layout)
{
    if (tag > kATSUMaxATSUITagValue)
        return 0; /* the program's own */
    if (layout) {
        switch (tag) {
        case kATSULineDirectionTag:
            return 1;
        case kATSULineLangRegionTag:
            return 2;
        case kATSULineBaselineValuesTag:
            return 128;
        case kATSULineWidthTag: case kATSULineRotationTag: case kATSULineJustificationFactorTag:
        case kATSULineFlushFactorTag: case kATSULineLayoutOptionsTag: case kATSULineAscentTag:
        case kATSULineDescentTag: case kATSULineTextLocatorTag: case kATSULineTruncationTag:
        case kATSULineFontFallbacksTag: case kATSUCGContextTag:
            return 4;
        }
        return -1;
    }
    switch (tag) {
    case kATSUQDBoldfaceTag: case kATSUQDItalicTag: case kATSUQDUnderlineTag: case kATSUQDCondensedTag:
    case kATSUQDExtendedTag: case kATSUNoLigatureSplitTag: case kATSUNoCaretAngleTag:
    case kATSUSuppressCrossKerningTag: case kATSUNoOpticalAlignmentTag: case kATSUForceHangingTag:
    case kATSUNoSpecialJustificationTag:
        return 1;
    case kATSULangRegionTag: case kATSUVerticalCharacterTag:
        return 2;
    case kATSUColorTag:
        return 6;
    case kATSUPriorityJustOverrideTag:
        return 0;
    case kATSUFontTag: case kATSUSizeTag: case kATSUImposeWidthTag: case kATSUBeforeWithStreamShiftTag:
    case kATSUAfterWithStreamShiftTag: case kATSUCrossStreamShiftTag: case kATSUTrackingTag:
    case kATSUHangingInhibitFactorTag: case kATSUKerningInhibitFactorTag: case kATSUDecompositionFactorTag:
    case kATSUBaselineClassTag: case kATSUStyleTextLocatorTag: case kATSUStyleRenderingOptionsTag:
        return 4;
    }
    return -1;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* Set attributes from the program's arrays of tags, sizes and value
 * pointers, checking each first. */
static int32_t set_attrs(attrset *s, uint32_t count, gaddr tags, gaddr sizes, gaddr values, bool layout)
{
    if (count && (!tags || !sizes || !values))
        return paramErr;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t tag = rd32(tags + 4 * i), size = rd32(sizes + 4 * i);
        int want = tag_size(tag, layout);
        if (want < 0)
            return kATSUInvalidAttributeTagErr;
        if ((want && size != (uint32_t)want) || size > 4096)
            return kATSUInvalidAttributeSizeErr;
        if (!rd32(values + 4 * i))
            return paramErr;
        if (tag == kATSUSizeTag && (int32_t)rd32(rd32(values + 4 * i)) <= 0)
            return kATSUInvalidAttributeValueErr;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t size = rd32(sizes + 4 * i);
        uint8_t *buf = malloc(size ? size : 1);
        gget(buf, rd32(values + 4 * i), size);
        attrs_set(s, rd32(tags + 4 * i), buf, size);
        free(buf);
    }
    return noErr;
}

/* The value an unset attribute has. */
static uint32_t default_font(void);
static void default_value(uint32_t tag, uint8_t *out, uint32_t size)
{
    memset(out, 0, size);
    uint32_t v = 0;
    if (tag == kATSUFontTag)
        v = default_font();
    else if (tag == kATSUSizeTag)
        v = 12u << 16;
    else
        return;
    for (uint32_t i = 0; i < 4 && i < size; i++)
        out[i] = (uint8_t)(v >> (24 - 8 * i));
}

/* ATSUGetAttribute and the layout and line controls: the value, or the
 * default with kATSUNotSetErr. */
static int32_t get_attr(const attrset *s, uint32_t tag, uint32_t expected, gaddr value, gaddr actual)
{
    attr *a = attrs_find(s, tag);
    uint8_t def[128];
    const uint8_t *data = def;
    uint32_t size;
    if (a) {
        data = a->data;
        size = a->size;
    } else {
        int t = tag_size(tag, tag < 256 || tag == kATSUCGContextTag);
        size = t > 0 ? (uint32_t)t : 0;
        default_value(tag, def, size);
    }
    if (actual)
        wr32(actual, size);
    if (value)
        gput(value, data, expected < size ? expected : size);
    return a ? noErr : kATSUNotSetErr;
}

static int32_t get_all_attrs(const attrset *s, gaddr info, uint32_t max, gaddr count)
{
    uint32_t k = 0;
    for (int i = 0; i < s->n; i++) {
        if (info && k < max) {
            wr32(info + 8 * k, s->a[i].tag);
            wr32(info + 8 * k + 4, s->a[i].size);
        }
        k++;
    }
    if (count)
        wr32(count, k);
    return noErr;
}

static int32_t clear_attrs(attrset *s, uint32_t count, gaddr tags)
{
    if (count == kATSUClearAll) {
        attrs_clear(s);
        return noErr;
    }
    if (count && !tags)
        return paramErr;
    for (uint32_t i = 0; i < count; i++)
        attrs_remove(s, rd32(tags + 4 * i));
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* Objects */

static gaddr new_handle_block(uint32_t tag, int index)
{
    gaddr p = tb_NewPtr(8, false, true);
    if (p) {
        wr32(p, tag);
        wr32(p + 4, (uint32_t)index);
    }
    return p;
}

static style_t *style_of(gaddr s)
{
    if (!s || s >= 0xF0000000u || rd32(s) != STYLE_TAG)
        return NULL;
    uint32_t i = rd32(s + 4);
    return i < (uint32_t)nstyles && styles[i] && styles[i]->addr == s ? styles[i] : NULL;
}

static layout_t *layout_of(gaddr l)
{
    if (!l || l >= 0xF0000000u || rd32(l) != LAYOUT_TAG)
        return NULL;
    uint32_t i = rd32(l + 4);
    return i < (uint32_t)nlayouts && layouts[i] && layouts[i]->addr == l ? layouts[i] : NULL;
}

static style_t *new_style(void)
{
    int i = 0;
    while (i < nstyles && styles[i])
        i++;
    if (i == nstyles) {
        styles = realloc(styles, sizeof *styles * (size_t)(nstyles + 16));
        memset(styles + nstyles, 0, sizeof *styles * 16);
        nstyles += 16;
    }
    style_t *st = calloc(1, sizeof *st);
    st->addr = new_handle_block(STYLE_TAG, i);
    if (!st->addr) {
        free(st);
        return NULL;
    }
    styles[i] = st;
    return st;
}

static void free_style(style_t *st)
{
    styles[rd32(st->addr + 4)] = NULL;
    wr32(st->addr, 0);
    tb_DisposePtr(st->addr);
    attrs_clear(&st->attrs);
    free(st);
}

static layout_t *new_layout(void)
{
    int i = 0;
    while (i < nlayouts && layouts[i])
        i++;
    if (i == nlayouts) {
        layouts = realloc(layouts, sizeof *layouts * (size_t)(nlayouts + 16));
        memset(layouts + nlayouts, 0, sizeof *layouts * 16);
        nlayouts += 16;
    }
    layout_t *l = calloc(1, sizeof *l);
    l->addr = new_handle_block(LAYOUT_TAG, i);
    if (!l->addr) {
        free(l);
        return NULL;
    }
    layouts[i] = l;
    return l;
}

static void clear_lines(layout_t *l)
{
    for (int i = 0; i < l->nlines; i++)
        attrs_clear(&l->lines[i].a);
    free(l->lines);
    l->lines = NULL;
    l->nlines = 0;
}

static void free_layout(layout_t *l)
{
    layouts[rd32(l->addr + 4)] = NULL;
    wr32(l->addr, 0);
    tb_DisposePtr(l->addr);
    attrs_clear(&l->controls);
    clear_lines(l);
    free(l->runs);
    free(l->breaks);
    free(l);
}

void carbon_atsui_reset(void)
{
    /* The blocks went with the heap; the records go too. */
    for (int i = 0; i < nstyles; i++)
        if (styles[i]) {
            attrs_clear(&styles[i]->attrs);
            free(styles[i]);
        }
    for (int i = 0; i < nlayouts; i++)
        if (layouts[i]) {
            attrs_clear(&layouts[i]->controls);
            for (int k = 0; k < layouts[i]->nlines; k++)
                attrs_clear(&layouts[i]->lines[k].a);
            free(layouts[i]->lines);
            free(layouts[i]->runs);
            free(layouts[i]->breaks);
            free(layouts[i]);
        }
    free(styles);
    free(layouts);
    styles = NULL;
    layouts = NULL;
    nstyles = nlayouts = 0;
    nfallbacks = 0;
    fallback_method = 0;
    scratch = 0;
}

/* ---------------------------------------------------------------------- */
/* Fonts: an ID is 0x10000 | family, with the QuickDraw style above. */

static uint32_t font_id(int16_t family, uint8_t face) { return (uint32_t)(face & 0x7F) << 24 | 0x10000u | (uint16_t)family; }
static bool font_parts(uint32_t id, int16_t *family, uint8_t *face)
{
    if (!(id & 0x10000u) || (id & 0x80FE0000u))
        return false;
    *family = (int16_t)(id & 0xFFFF);
    *face = (uint8_t)(id >> 24 & 0x7F);
    return true;
}

/* The families: the FOND resources there are, by number. */
enum { MAX_FAMILIES = 256 };
static int families(int16_t *ids, uint8_t names[][256], int max)
{
    gaddr id = tb_NewPtr(2, false, true), type = tb_NewPtr(4, false, true), name = tb_NewPtr(256, false, true);
    int n = 0;
    uint8_t load = rd8(LM_ResLoad);
    wr8(LM_ResLoad, 0);
    int16_t count = tb_CountResources((int32_t)fourcc("FOND"));
    for (int16_t i = 1; i <= count && n < max; i++) {
        gaddr h = tb_GetIndResource((int32_t)fourcc("FOND"), i);
        if (!h)
            continue;
        wr8(name, 0);
        tb_GetResInfo(h, id, type, name);
        int16_t fid = (int16_t)rd16(id);
        bool dup = false;
        for (int k = 0; k < n; k++)
            dup |= ids[k] == fid;
        if (dup || !rd8(name))
            continue;
        ids[n] = fid;
        if (names)
            gget(names[n], name, 256);
        n++;
    }
    wr8(LM_ResLoad, load);
    tb_DisposePtr(name);
    tb_DisposePtr(type);
    tb_DisposePtr(id);
    return n;
}

static bool family_name(int16_t family, uint8_t *out)
{
    gaddr name = tb_NewPtr(256, false, true);
    tb_GetFontName(family, name);
    gget(out, name, 256);
    tb_DisposePtr(name);
    return out[0] != 0;
}

static uint32_t default_font(void) { return font_id((int16_t)rd16(LM_ApFontID), 0); }

/* A style's QuickDraw font, size, face and colour. */
bool atsui_style_qd(gaddr s, svc_qdstyle *q)
{
    style_t *st = style_of(s);
    memset(q, 0, sizeof *q);
    int16_t family;
    uint8_t face;
    font_parts(default_font(), &family, &face);
    q->font = family;
    q->size = 12;
    if (!st)
        return false;
    attr *a;
    if ((a = attrs_find(&st->attrs, kATSUFontTag)) && font_parts(be32(a->data), &family, &face)) {
        q->font = family;
        q->face = face;
    }
    if ((a = attrs_find(&st->attrs, kATSUSizeTag))) {
        int32_t sz = (int32_t)be32(a->data);
        q->size = (int16_t)((sz + 0x8000) >> 16);
        if (q->size < 1)
            q->size = 1;
    }
    static const struct {
        uint32_t tag;
        uint8_t bit;
    } faces[] = { { kATSUQDBoldfaceTag, bold }, { kATSUQDItalicTag, italic }, { kATSUQDUnderlineTag, underline },
                  { kATSUQDCondensedTag, condense }, { kATSUQDExtendedTag, extend } };
    for (size_t i = 0; i < sizeof faces / sizeof *faces; i++)
        if ((a = attrs_find(&st->attrs, faces[i].tag)))
            q->face = (uint8_t)(a->data[0] ? q->face | faces[i].bit : q->face & ~faces[i].bit);
    if ((a = attrs_find(&st->attrs, kATSUColorTag))) {
        q->has_color = true;
        for (int i = 0; i < 3; i++)
            q->color[i] = (uint16_t)(a->data[2 * i] << 8 | a->data[2 * i + 1]);
    }
    return true;
}

/* ---------------------------------------------------------------------- */
/* Text in the port */

typedef struct {
    gaddr port;
    int16_t font, size;
    uint8_t face;
    uint16_t fg[3];
    gaddr fgp;
    uint32_t pen;
} port_state;

static void save_port(port_state *s)
{
    s->port = qd_port();
    s->font = (int16_t)rd16(s->port + GrafPort_txFont_off);
    s->face = rd8(s->port + GrafPort_txFace_off);
    s->size = (int16_t)rd16(s->port + GrafPort_txSize_off);
    s->pen = rd32(s->port + GrafPort_pnLoc_off);
    s->fgp = tb_NewPtr(6, false, true);
    tb_GetForeColor(s->fgp);
}

static void restore_port(port_state *s)
{
    tb_TextFont(s->font);
    tb_TextFace(s->face);
    tb_TextSize(s->size);
    wr32(s->port + GrafPort_pnLoc_off, s->pen);
    tb_RGBForeColor(s->fgp);
    tb_DisposePtr(s->fgp);
}

static void use_style(gaddr style, bool color)
{
    svc_qdstyle q;
    atsui_style_qd(style, &q);
    tb_TextFont(q.font);
    tb_TextFace(q.face);
    tb_TextSize(q.size);
    if (color) {
        gaddr rgb = tb_NewPtr(6, false, true);
        for (int i = 0; i < 3; i++)
            wr16(rgb + 2 * (uint32_t)i, q.has_color ? q.color[i] : 0);
        tb_RGBForeColor(rgb);
        tb_DisposePtr(rgb);
    }
}

static void font_metrics(int *ascent, int *descent, int *leading)
{
    if (!scratch)
        scratch = tb_NewPtr(8, false, true);
    tb_GetFontInfo(scratch);
    *ascent = (int16_t)rd16(scratch);
    *descent = (int16_t)rd16(scratch + 2);
    *leading = (int16_t)rd16(scratch + 6);
}

static gaddr text_base(const layout_t *l)
{
    return l->handle ? (l->text && rd32(l->text) ? rd32(l->text) : 0) : l->text;
}
static uint32_t text_end(const layout_t *l) { return l->offset + l->length; }

/* The style for an offset: its run's, or (outside any) the nearest. */
static gaddr style_at(const layout_t *l, uint32_t off, uint32_t *run_end)
{
    gaddr s = l->nruns ? l->runs[0].style : 0;
    uint32_t end = text_end(l);
    for (int i = 0; i < l->nruns; i++) {
        if (l->runs[i].start > off) {
            end = l->runs[i].start;
            break;
        }
        s = l->runs[i].style;
        if (off < l->runs[i].start + l->runs[i].len) {
            end = l->runs[i].start + l->runs[i].len;
            break;
        }
    }
    if (run_end)
        *run_end = end;
    return s;
}

/* A segment's characters as MacRoman, line ends left out. */
static size_t segment_bytes(const layout_t *l, uint32_t a, uint32_t b, uint8_t *out)
{
    gaddr base = text_base(l);
    size_t n = 0;
    uint16_t u[2];
    for (uint32_t i = a; i < b && base; i++) {
        u[0] = rd16(base + 2 * i);
        if (u[0] == 0x0D || u[0] == 0x0A || u[0] == 0x2028 || u[0] == 0x2029)
            continue;
        if (u[0] >= 0xD800 && u[0] < 0xDC00 && i + 1 < b) {
            u[1] = rd16(base + 2 * (i + 1));
            n += svc_utf16_to_macroman(u, 2, out + n);
            i++;
            continue;
        }
        n += svc_utf16_to_macroman(u, 1, out + n);
    }
    return n;
}

typedef struct {
    int32_t width; /* Fixed */
    int ascent, descent, leading;
} measure_t;

/* Measure [a, b), or draw it from x (pixels) at baseline y when draw. */
static void walk(const layout_t *l, uint32_t a, uint32_t b, bool draw, int x, int y, measure_t *m)
{
    memset(m, 0, sizeof *m);
    port_state ps;
    save_port(&ps);
    int px = x;
    for (uint32_t pos = a; pos < b;) {
        uint32_t end;
        gaddr st = style_at(l, pos, &end);
        if (end > b || end <= pos)
            end = b;
        use_style(st, draw);
        int asc, desc, lead;
        font_metrics(&asc, &desc, &lead);
        m->ascent = asc > m->ascent ? asc : m->ascent;
        m->descent = desc > m->descent ? desc : m->descent;
        m->leading = lead > m->leading ? lead : m->leading;
        uint8_t *bytes = malloc(end - pos + 1);
        size_t n = segment_bytes(l, pos, end, bytes);
        int w = n ? qd_text_width(ps.port, bytes, (int)n) : 0;
        style_t *sty = style_of(st);
        attr *imp = sty ? attrs_find(&sty->attrs, kATSUImposeWidthTag) : NULL;
        if (imp && be32(imp->data))
            w = (int)n * (int)(((int32_t)be32(imp->data) + 0x8000) >> 16);
        if (draw && n) {
            gaddr buf = tb_NewPtr((int32_t)n, false, false);
            if (buf) {
                gput(buf, bytes, (uint32_t)n);
                tb_MoveTo((int16_t)px, (int16_t)y);
                tb_DrawText(buf, 0, (int16_t)n);
                tb_DisposePtr(buf);
            }
        }
        free(bytes);
        px += w;
        m->width += w << 16;
        pos = end;
    }
    if (a >= b) { /* an empty line still has its font's height */
        use_style(style_at(l, a, NULL), false);
        font_metrics(&m->ascent, &m->descent, &m->leading);
    }
    restore_port(&ps);
}

/* A line's start: the soft break (or the text's start) at or before off. */
static uint32_t line_start(const layout_t *l, uint32_t off)
{
    uint32_t s = l->offset;
    for (int i = 0; i < l->nbreaks; i++)
        if (l->breaks[i] <= off && l->breaks[i] > s)
            s = l->breaks[i];
    return s;
}

/* A line control's value, else the layout's. */
static attr *control(const layout_t *l, uint32_t start, uint32_t tag)
{
    for (int i = 0; i < l->nlines; i++)
        if (l->lines[i].start == start) {
            attr *a = attrs_find(&l->lines[i].a, tag);
            if (a)
                return a;
        }
    return attrs_find(&l->controls, tag);
}

/* Where a line's text starts across its width: the flush factor (a Fract)
 * of the room left. */
static int flush_offset(const layout_t *l, uint32_t start, int32_t width)
{
    attr *w = control(l, start, kATSULineWidthTag), *f = control(l, start, kATSULineFlushFactorTag);
    if (!w || !f)
        return 0;
    int64_t room = (int64_t)(int32_t)be32(w->data) - width;
    if (room <= 0)
        return 0;
    return (int)((room * (int32_t)be32(f->data) >> 30) >> 16);
}

/* A range's line-relative bounds. */
static int32_t range_check(const layout_t *l, uint32_t *start, uint32_t *len)
{
    if (*start == kATSUFromTextBeginning)
        *start = l->offset;
    if (*len == kATSUToTextEnd)
        *len = *start <= text_end(l) ? text_end(l) - *start : 0;
    if (*start < l->offset || *start > text_end(l) || *len > text_end(l) - *start)
        return kATSUInvalidTextRangeErr;
    return noErr;
}

static int fixed_to_int(uint32_t f) { return (int)(((int32_t)f + 0x8000) >> 16); }

static void pen_or(uint32_t fx, uint32_t fy, int *x, int *y)
{
    uint32_t pen = rd32(qd_port() + GrafPort_pnLoc_off);
    *x = fx == kATSUUseGrafPortPenLoc ? (int16_t)(pen & 0xFFFF) : fixed_to_int(fx);
    *y = fy == kATSUUseGrafPortPenLoc ? (int16_t)(pen >> 16) : fixed_to_int(fy);
}

/* ---------------------------------------------------------------------- */
/* Style calls */

#define STYLE_ARG(st, n)                                                                                               \
    style_t *st = style_of(A(n));                                                                                      \
    if (!st)                                                                                                           \
        RET(kATSUInvalidStyleErr);
#define LAYOUT_ARG(l, n)                                                                                               \
    layout_t *l = layout_of(A(n));                                                                                     \
    if (!l)                                                                                                            \
        RET(kATSUInvalidTextLayoutErr);

static void at_ATSUCreateStyle(ppc_cpu *c)
{
    if (!A(0))
        RET(paramErr);
    style_t *st = new_style();
    wr32(A(0), st ? st->addr : 0);
    RET(st ? noErr : memFullErr);
}

static void copy_style(style_t *dst, const style_t *src)
{
    attrs_copy(&dst->attrs, &src->attrs);
    dst->nfeat = src->nfeat;
    memcpy(dst->feat_type, src->feat_type, sizeof dst->feat_type);
    memcpy(dst->feat_sel, src->feat_sel, sizeof dst->feat_sel);
    dst->nvar = src->nvar;
    memcpy(dst->var_axis, src->var_axis, sizeof dst->var_axis);
    memcpy(dst->var_val, src->var_val, sizeof dst->var_val);
}

static void at_ATSUCreateAndCopyStyle(ppc_cpu *c)
{
    STYLE_ARG(src, 0);
    if (!A(1))
        RET(paramErr);
    style_t *st = new_style();
    if (!st)
        RET(memFullErr);
    copy_style(st, src);
    st->refcon = src->refcon;
    wr32(A(1), st->addr);
    RET(noErr);
}

static void at_ATSUDisposeStyle(ppc_cpu *c)
{
    STYLE_ARG(st, 0);
    free_style(st);
    RET(noErr);
}

static void at_ATSUSetStyleRefCon(ppc_cpu *c)
{
    STYLE_ARG(st, 0);
    st->refcon = A(1);
    RET(noErr);
}

static void at_ATSUGetStyleRefCon(ppc_cpu *c)
{
    STYLE_ARG(st, 0);
    if (!A(1))
        RET(paramErr);
    wr32(A(1), st->refcon);
    RET(noErr);
}

/* Whether every attribute, feature and variation of a is in b. */
static bool style_subset(const style_t *a, const style_t *b)
{
    for (int i = 0; i < a->attrs.n; i++) {
        attr *o = attrs_find(&b->attrs, a->attrs.a[i].tag);
        if (!o || !attr_equal(o, &a->attrs.a[i]))
            return false;
    }
    for (int i = 0; i < a->nfeat; i++) {
        bool found = false;
        for (int k = 0; k < b->nfeat && !found; k++)
            found = b->feat_type[k] == a->feat_type[i] && b->feat_sel[k] == a->feat_sel[i];
        if (!found)
            return false;
    }
    for (int i = 0; i < a->nvar; i++) {
        bool found = false;
        for (int k = 0; k < b->nvar && !found; k++)
            found = b->var_axis[k] == a->var_axis[i] && b->var_val[k] == a->var_val[i];
        if (!found)
            return false;
    }
    return true;
}

static void at_ATSUCompareStyles(ppc_cpu *c)
{
    STYLE_ARG(a, 0);
    STYLE_ARG(b, 1);
    if (!A(2))
        RET(paramErr);
    bool ab = style_subset(a, b), ba = style_subset(b, a);
    wr16(A(2), ab && ba ? kATSUStyleEquals : ba ? kATSUStyleContains : ab ? kATSUStyleContainedBy : kATSUStyleUnequal);
    RET(noErr);
}

/* ATSUCopyAttributes(src, dst): dst's replaced by src's.  Overwrite: src's
 * set over dst's.  Underwrite: src's only where dst has none. */
static void at_ATSUCopyAttributes(ppc_cpu *c)
{
    STYLE_ARG(src, 0);
    STYLE_ARG(dst, 1);
    copy_style(dst, src);
    RET(noErr);
}

static void merge(style_t *dst, const style_t *src, bool over)
{
    for (int i = 0; i < src->attrs.n; i++)
        if (over || !attrs_find(&dst->attrs, src->attrs.a[i].tag))
            attrs_set(&dst->attrs, src->attrs.a[i].tag, src->attrs.a[i].data, src->attrs.a[i].size);
    for (int i = 0; i < src->nfeat; i++) {
        int k = 0;
        while (k < dst->nfeat && dst->feat_type[k] != src->feat_type[i])
            k++;
        if (k == dst->nfeat && k < MAX_FEATURES) {
            dst->feat_type[k] = src->feat_type[i];
            dst->feat_sel[k] = src->feat_sel[i];
            dst->nfeat++;
        } else if (k < dst->nfeat && over) {
            dst->feat_sel[k] = src->feat_sel[i];
        }
    }
    for (int i = 0; i < src->nvar; i++) {
        int k = 0;
        while (k < dst->nvar && dst->var_axis[k] != src->var_axis[i])
            k++;
        if (k == dst->nvar && k < MAX_VARIATIONS) {
            dst->var_axis[k] = src->var_axis[i];
            dst->var_val[k] = src->var_val[i];
            dst->nvar++;
        } else if (k < dst->nvar && over) {
            dst->var_val[k] = src->var_val[i];
        }
    }
}

static void at_ATSUOverwriteAttributes(ppc_cpu *c)
{
    STYLE_ARG(src, 0);
    STYLE_ARG(dst, 1);
    merge(dst, src, true);
    RET(noErr);
}

static void at_ATSUUnderwriteAttributes(ppc_cpu *c)
{
    STYLE_ARG(src, 0);
    STYLE_ARG(dst, 1);
    merge(dst, src, false);
    RET(noErr);
}

static void at_ATSUClearStyle(ppc_cpu *c)
{
    STYLE_ARG(st, 0);
    attrs_clear(&st->attrs);
    st->nfeat = st->nvar = 0;
    RET(noErr);
}

static void at_ATSUStyleIsEmpty(ppc_cpu *c)
{
    STYLE_ARG(st, 0);
    if (!A(1))
        RET(paramErr);
    wr8(A(1), !st->attrs.n && !st->nfeat && !st->nvar);
    RET(noErr);
}

/* ATSUCalculateBaselineDeltas(style, baselineClass, deltas[32]): the
 * baselines from the font's ascent and descent, relative to the class's. */
static void at_ATSUCalculateBaselineDeltas(ppc_cpu *c)
{
    STYLE_ARG(st, 0);
    gaddr out = A(2);
    if (!out)
        RET(paramErr);
    port_state ps;
    save_port(&ps);
    use_style(st->addr, false);
    int asc, desc, lead;
    font_metrics(&asc, &desc, &lead);
    restore_port(&ps);
    /* Roman 0, ideographic centre, ideographic low, hanging, math. */
    int32_t base[5] = { 0, (asc - desc) << 15, -(desc << 16), (asc * 4 / 5) << 16, asc << 15 };
    uint32_t cls = A(1);
    int32_t ref = cls < 5 ? base[cls] : 0;
    gmemset(out, 0, 128);
    for (int i = 0; i < 5; i++)
        wr32(out + 4 * (uint32_t)i, (uint32_t)(base[i] - ref));
    RET(noErr);
}

/* ATSUSetAttributes(style, count, tags[], sizes[], values[]) */
static void at_ATSUSetAttributes(ppc_cpu *c)
{
    STYLE_ARG(st, 0);
    RET(set_attrs(&st->attrs, A(1), A(2), A(3), A(4), false));
}

/* ATSUGetAttribute(style, tag, expectedSize, value, &actualSize) */
static void at_ATSUGetAttribute(ppc_cpu *c)
{
    STYLE_ARG(st, 0);
    RET(get_attr(&st->attrs, A(1), A(2), A(3), A(4)));
}

static void at_ATSUGetAllAttributes(ppc_cpu *c)
{
    STYLE_ARG(st, 0);
    RET(get_all_attrs(&st->attrs, A(1), A(2), A(3)));
}

static void at_ATSUClearAttributes(ppc_cpu *c)
{
    STYLE_ARG(st, 0);
    RET(clear_attrs(&st->attrs, A(1), A(2)));
}

/* Font features: (type, selector) pairs, a type once. */
static void at_ATSUSetFontFeatures(ppc_cpu *c)
{
    STYLE_ARG(st, 0);
    uint32_t n = A(1);
    gaddr types = A(2), sels = A(3);
    if (n && (!types || !sels))
        RET(paramErr);
    for (uint32_t i = 0; i < n; i++) {
        uint16_t t = rd16(types + 2 * i), s = rd16(sels + 2 * i);
        int k = 0;
        while (k < st->nfeat && st->feat_type[k] != t)
            k++;
        if (k == st->nfeat) {
            if (k >= MAX_FEATURES)
                RET(memFullErr);
            st->nfeat++;
        }
        st->feat_type[k] = t;
        st->feat_sel[k] = s;
    }
    RET(noErr);
}

static void at_ATSUGetFontFeature(ppc_cpu *c)
{
    STYLE_ARG(st, 0);
    uint32_t i = A(1);
    if (i >= (uint32_t)st->nfeat)
        RET(kATSUNotSetErr);
    if (A(2))
        wr16(A(2), st->feat_type[i]);
    if (A(3))
        wr16(A(3), st->feat_sel[i]);
    RET(noErr);
}

static void at_ATSUGetAllFontFeatures(ppc_cpu *c)
{
    STYLE_ARG(st, 0);
    uint32_t max = A(1);
    for (uint32_t i = 0; i < (uint32_t)st->nfeat && i < max; i++) {
        if (A(2))
            wr16(A(2) + 2 * i, st->feat_type[i]);
        if (A(3))
            wr16(A(3) + 2 * i, st->feat_sel[i]);
    }
    if (A(4))
        wr32(A(4), (uint32_t)st->nfeat);
    RET(noErr);
}

static void at_ATSUClearFontFeatures(ppc_cpu *c)
{
    STYLE_ARG(st, 0);
    uint32_t n = A(1);
    if (n == kATSUClearAll) {
        st->nfeat = 0;
        RET(noErr);
    }
    if (n && (!A(2) || !A(3)))
        RET(paramErr);
    for (uint32_t i = 0; i < n; i++)
        for (int k = 0; k < st->nfeat; k++)
            if (st->feat_type[k] == rd16(A(2) + 2 * i) && st->feat_sel[k] == rd16(A(3) + 2 * i)) {
                st->feat_type[k] = st->feat_type[st->nfeat - 1];
                st->feat_sel[k] = st->feat_sel[--st->nfeat];
                break;
            }
    RET(noErr);
}

/* Variations: (axis, value) pairs. */
static void at_ATSUSetVariations(ppc_cpu *c)
{
    STYLE_ARG(st, 0);
    uint32_t n = A(1);
    if (n && (!A(2) || !A(3)))
        RET(paramErr);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t axis = rd32(A(2) + 4 * i);
        int k = 0;
        while (k < st->nvar && st->var_axis[k] != axis)
            k++;
        if (k == st->nvar) {
            if (k >= MAX_VARIATIONS)
                RET(memFullErr);
            st->nvar++;
        }
        st->var_axis[k] = axis;
        st->var_val[k] = rd32(A(3) + 4 * i);
    }
    RET(noErr);
}

static void at_ATSUGetFontVariationValue(ppc_cpu *c)
{
    STYLE_ARG(st, 0);
    for (int k = 0; k < st->nvar; k++)
        if (st->var_axis[k] == A(1)) {
            if (A(2))
                wr32(A(2), st->var_val[k]);
            RET(noErr);
        }
    if (A(2))
        wr32(A(2), 0);
    RET(kATSUNotSetErr);
}

static void at_ATSUGetAllFontVariations(ppc_cpu *c)
{
    STYLE_ARG(st, 0);
    uint32_t max = A(1);
    for (uint32_t i = 0; i < (uint32_t)st->nvar && i < max; i++) {
        if (A(2))
            wr32(A(2) + 4 * i, st->var_axis[i]);
        if (A(3))
            wr32(A(3) + 4 * i, st->var_val[i]);
    }
    if (A(4))
        wr32(A(4), (uint32_t)st->nvar);
    RET(noErr);
}

static void at_ATSUClearFontVariations(ppc_cpu *c)
{
    STYLE_ARG(st, 0);
    uint32_t n = A(1);
    if (n == kATSUClearAll) {
        st->nvar = 0;
        RET(noErr);
    }
    if (n && !A(2))
        RET(paramErr);
    for (uint32_t i = 0; i < n; i++)
        for (int k = 0; k < st->nvar; k++)
            if (st->var_axis[k] == rd32(A(2) + 4 * i)) {
                st->var_axis[k] = st->var_axis[st->nvar - 1];
                st->var_val[k] = st->var_val[--st->nvar];
                break;
            }
    RET(noErr);
}

/* ATSUCopyToHandle(style, handle), ATSUPasteFromHandle(style, handle): the
 * attributes flattened as 'ATSs', a count, then (tag, size, data padded
 * to 4) for each. */
static void at_ATSUCopyToHandle(ppc_cpu *c)
{
    STYLE_ARG(st, 0);
    gaddr h = A(1);
    if (!h)
        RET(paramErr);
    uint32_t size = 8;
    for (int i = 0; i < st->attrs.n; i++)
        size += 8 + ((st->attrs.a[i].size + 3) & ~3u);
    tb_SetHandleSize(h, (int32_t)size);
    if ((uint32_t)tb_GetHandleSize(h) != size)
        RET(memFullErr);
    gaddr p = rd32(h);
    wr32(p, STYLE_TAG);
    wr32(p + 4, (uint32_t)st->attrs.n);
    p += 8;
    for (int i = 0; i < st->attrs.n; i++) {
        wr32(p, st->attrs.a[i].tag);
        wr32(p + 4, st->attrs.a[i].size);
        gput(p + 8, st->attrs.a[i].data, st->attrs.a[i].size);
        p += 8 + ((st->attrs.a[i].size + 3) & ~3u);
    }
    RET(noErr);
}

static void at_ATSUPasteFromHandle(ppc_cpu *c)
{
    STYLE_ARG(st, 0);
    gaddr h = A(1);
    if (!h || !rd32(h))
        RET(paramErr);
    uint32_t size = (uint32_t)tb_GetHandleSize(h);
    gaddr p = rd32(h);
    if (size < 8 || rd32(p) != STYLE_TAG)
        RET(kATSUInvalidCacheErr);
    uint32_t n = rd32(p + 4), pos = 8;
    for (uint32_t i = 0; i < n && pos + 8 <= size; i++) {
        uint32_t tag = rd32(p + pos), len = rd32(p + pos + 4);
        if (len > size - pos - 8)
            RET(kATSUInvalidCacheErr);
        uint8_t *buf = malloc(len ? len : 1);
        gget(buf, p + pos + 8, len);
        attrs_set(&st->attrs, tag, buf, len);
        free(buf);
        pos += 8 + ((len + 3) & ~3u);
    }
    RET(noErr);
}

/* ---------------------------------------------------------------------- */
/* Layouts */

static void add_run(layout_t *l, uint32_t start, uint32_t len, gaddr style)
{
    l->runs = realloc(l->runs, sizeof *l->runs * (size_t)(l->nruns + 1));
    l->runs[l->nruns++] = (run_t){ start, len, style };
}

/* Runs in order, neighbours with one style joined, empty ones gone. */
static int run_cmp(const void *a, const void *b)
{
    const run_t *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start;
}

static void tidy_runs(layout_t *l)
{
    qsort(l->runs, (size_t)l->nruns, sizeof *l->runs, run_cmp);
    int k = 0;
    for (int i = 0; i < l->nruns; i++) {
        if (!l->runs[i].len)
            continue;
        if (k && l->runs[k - 1].style == l->runs[i].style &&
            l->runs[k - 1].start + l->runs[k - 1].len == l->runs[i].start)
            l->runs[k - 1].len += l->runs[i].len;
        else
            l->runs[k++] = l->runs[i];
    }
    l->nruns = k;
}

/* A style over [start, start+len), cutting what was there. */
static void set_run(layout_t *l, uint32_t start, uint32_t len, gaddr style)
{
    uint32_t end = start + len;
    int n = l->nruns;
    for (int i = 0; i < n; i++) {
        run_t *r = &l->runs[i];
        uint32_t rs = r->start, re = r->start + r->len;
        if (re <= start || rs >= end)
            continue;
        if (rs < start && re > end) { /* split around */
            r->len = start - rs;
            add_run(l, end, re - end, l->runs[i].style);
            continue;
        }
        if (rs < start)
            r->len = start - rs;
        else if (re > end) {
            r->start = end;
            r->len = re - end;
        } else
            r->len = 0;
    }
    add_run(l, start, len, style);
    tidy_runs(l);
}

/* The text changed in length: runs end at the end. */
static void clip_runs(layout_t *l)
{
    uint32_t end = text_end(l);
    for (int i = 0; i < l->nruns; i++) {
        run_t *r = &l->runs[i];
        if (r->start < l->offset) {
            uint32_t cut = l->offset - r->start;
            r->len = r->len > cut ? r->len - cut : 0;
            r->start = l->offset;
        }
        if (r->start >= end)
            r->len = 0;
        else if (r->start + r->len > end)
            r->len = end - r->start;
    }
    tidy_runs(l);
    if (l->nruns && l->runs[l->nruns - 1].start + l->runs[l->nruns - 1].len < end)
        l->runs[l->nruns - 1].len = end - l->runs[l->nruns - 1].start;
}

static void at_ATSUCreateTextLayout(ppc_cpu *c)
{
    if (!A(0))
        RET(paramErr);
    layout_t *l = new_layout();
    wr32(A(0), l ? l->addr : 0);
    RET(l ? noErr : memFullErr);
}

/* ATSUCreateTextLayoutWithText{Ptr,Handle}(text, offset, length, total,
 * nRuns, runLengths[], styles[], &layout) */
static void create_with_text(ppc_cpu *c, bool handle)
{
    gaddr text = A(0), lens = A(5), stys = A(6), out = A(7);
    uint32_t off = A(1), len = A(2), total = A(3), nruns = A(4);
    if (!out || !text || (nruns && (!lens || !stys)))
        RET(paramErr);
    if (off > total || len > total - off)
        RET(kATSUInvalidTextRangeErr);
    for (uint32_t i = 0; i < nruns; i++)
        if (!style_of(rd32(stys + 4 * i)))
            RET(kATSUInvalidStyleErr);
    layout_t *l = new_layout();
    if (!l)
        RET(memFullErr);
    l->text = text;
    l->handle = handle;
    l->offset = off;
    l->length = len;
    l->total = total;
    uint32_t pos = off;
    for (uint32_t i = 0; i < nruns && pos < off + len; i++) {
        uint32_t rl = rd32(lens + 4 * i);
        if (rl == kATSUToTextEnd || rl > off + len - pos)
            rl = off + len - pos;
        add_run(l, pos, rl, rd32(stys + 4 * i));
        pos += rl;
    }
    tidy_runs(l);
    wr32(out, l->addr);
    RET(noErr);
}

static void at_ATSUCreateTextLayoutWithTextPtr(ppc_cpu *c) { create_with_text(c, false); }
static void at_ATSUCreateTextLayoutWithTextHandle(ppc_cpu *c) { create_with_text(c, true); }

static void at_ATSUCreateAndCopyTextLayout(ppc_cpu *c)
{
    LAYOUT_ARG(src, 0);
    if (!A(1))
        RET(paramErr);
    layout_t *l = new_layout();
    if (!l)
        RET(memFullErr);
    gaddr addr = l->addr;
    *l = *src;
    l->addr = addr;
    l->runs = NULL;
    l->nruns = 0;
    for (int i = 0; i < src->nruns; i++)
        add_run(l, src->runs[i].start, src->runs[i].len, src->runs[i].style);
    l->controls = (attrset){ 0, NULL };
    attrs_copy(&l->controls, &src->controls);
    l->lines = NULL;
    l->nlines = 0;
    for (int i = 0; i < src->nlines; i++) {
        l->lines = realloc(l->lines, sizeof *l->lines * (size_t)(l->nlines + 1));
        l->lines[l->nlines] = (linectl){ src->lines[i].start, { 0, NULL } };
        attrs_copy(&l->lines[l->nlines++].a, &src->lines[i].a);
    }
    l->breaks = src->nbreaks ? malloc(sizeof *l->breaks * (size_t)src->nbreaks) : NULL;
    if (src->nbreaks)
        memcpy(l->breaks, src->breaks, sizeof *l->breaks * (size_t)src->nbreaks);
    wr32(A(1), l->addr);
    RET(noErr);
}

static void at_ATSUDisposeTextLayout(ppc_cpu *c)
{
    LAYOUT_ARG(l, 0);
    free_layout(l);
    RET(noErr);
}

static void at_ATSUSetTextLayoutRefCon(ppc_cpu *c)
{
    LAYOUT_ARG(l, 0);
    l->refcon = A(1);
    RET(noErr);
}

static void at_ATSUGetTextLayoutRefCon(ppc_cpu *c)
{
    LAYOUT_ARG(l, 0);
    if (!A(1))
        RET(paramErr);
    wr32(A(1), l->refcon);
    RET(noErr);
}

/* Nothing is cached; layouts are measured as they are drawn. */
static void at_layout_noop(ppc_cpu *c)
{
    LAYOUT_ARG(l, 0);
    (void)l;
    RET(noErr);
}

/* ATSUSetText{Pointer,Handle}Location(layout, text, offset, length, total) */
static void set_text(ppc_cpu *c, bool handle)
{
    LAYOUT_ARG(l, 0);
    uint32_t off = A(2), len = A(3), total = A(4);
    if (!A(1))
        RET(paramErr);
    if (off > total || len > total - off)
        RET(kATSUInvalidTextRangeErr);
    l->text = A(1);
    l->handle = handle;
    l->offset = off;
    l->length = len;
    l->total = total;
    l->nbreaks = 0;
    clear_lines(l);
    clip_runs(l);
    RET(noErr);
}

static void at_ATSUSetTextPointerLocation(ppc_cpu *c) { set_text(c, false); }
static void at_ATSUSetTextHandleLocation(ppc_cpu *c) { set_text(c, true); }

/* ATSUGetTextLocation(layout, &text, &isHandle, &offset, &length, &total) */
static void at_ATSUGetTextLocation(ppc_cpu *c)
{
    LAYOUT_ARG(l, 0);
    if (A(1))
        wr32(A(1), l->text);
    if (A(2))
        wr8(A(2), l->handle);
    if (A(3))
        wr32(A(3), l->offset);
    if (A(4))
        wr32(A(4), l->length);
    if (A(5))
        wr32(A(5), l->total);
    RET(noErr);
}

/* ATSUTextDeleted(layout, start, length): the runs and breaks after
 * close up. */
static void at_ATSUTextDeleted(ppc_cpu *c)
{
    LAYOUT_ARG(l, 0);
    uint32_t start = A(1), len = A(2);
    if (start == kATSUFromTextBeginning)
        start = l->offset;
    if (len == kATSUToTextEnd)
        len = text_end(l) > start ? text_end(l) - start : 0;
    if (start < l->offset || start > text_end(l) || len > text_end(l) - start)
        RET(kATSUInvalidTextRangeErr);
    uint32_t end = start + len;
    for (int i = 0; i < l->nruns; i++) {
        run_t *r = &l->runs[i];
        uint32_t rs = r->start, re = rs + r->len;
        uint32_t cut_s = rs > start ? rs : start, cut_e = re < end ? re : end;
        if (cut_e > cut_s)
            r->len -= cut_e - cut_s;
        if (rs >= end)
            r->start -= len;
        else if (rs > start)
            r->start = start;
    }
    int k = 0;
    for (int i = 0; i < l->nbreaks; i++) {
        uint32_t b = l->breaks[i];
        if (b > start && b <= end)
            continue;
        l->breaks[k++] = b > end ? b - len : b;
    }
    l->nbreaks = k;
    l->length -= len;
    l->total -= len;
    tidy_runs(l);
    RET(noErr);
}

/* ATSUTextInserted(layout, location, length): the run there grows. */
static void at_ATSUTextInserted(ppc_cpu *c)
{
    LAYOUT_ARG(l, 0);
    uint32_t at = A(1), len = A(2);
    if (at < l->offset || at > text_end(l))
        RET(kATSUInvalidTextRangeErr);
    bool grown = false;
    for (int i = 0; i < l->nruns; i++) {
        run_t *r = &l->runs[i];
        if (!grown && at >= r->start && at <= r->start + r->len && (at < r->start + r->len || i == l->nruns - 1 ||
                                                                     l->runs[i + 1].start != at)) {
            r->len += len;
            grown = true;
        } else if (r->start >= at) {
            r->start += len;
        }
    }
    for (int i = 0; i < l->nbreaks; i++)
        if (l->breaks[i] > at)
            l->breaks[i] += len;
    l->length += len;
    l->total += len;
    RET(noErr);
}

static void at_ATSUTextMoved(ppc_cpu *c)
{
    LAYOUT_ARG(l, 0);
    if (!A(1))
        RET(paramErr);
    l->text = A(1);
    l->handle = false;
    RET(noErr);
}

/* Layout controls */
static void at_ATSUCopyLayoutControls(ppc_cpu *c)
{
    LAYOUT_ARG(src, 0);
    LAYOUT_ARG(dst, 1);
    attrs_copy(&dst->controls, &src->controls);
    RET(noErr);
}

static void at_ATSUSetLayoutControls(ppc_cpu *c)
{
    LAYOUT_ARG(l, 0);
    RET(set_attrs(&l->controls, A(1), A(2), A(3), A(4), true));
}

static void at_ATSUGetLayoutControl(ppc_cpu *c)
{
    LAYOUT_ARG(l, 0);
    RET(get_attr(&l->controls, A(1), A(2), A(3), A(4)));
}

static void at_ATSUGetAllLayoutControls(ppc_cpu *c)
{
    LAYOUT_ARG(l, 0);
    RET(get_all_attrs(&l->controls, A(1), A(2), A(3)));
}

static void at_ATSUClearLayoutControls(ppc_cpu *c)
{
    LAYOUT_ARG(l, 0);
    RET(clear_attrs(&l->controls, A(1), A(2)));
}

/* Line controls, by the line's start. */
static attrset *line_controls(layout_t *l, uint32_t start, bool make)
{
    for (int i = 0; i < l->nlines; i++)
        if (l->lines[i].start == start)
            return &l->lines[i].a;
    if (!make)
        return NULL;
    l->lines = realloc(l->lines, sizeof *l->lines * (size_t)(l->nlines + 1));
    l->lines[l->nlines] = (linectl){ start, { 0, NULL } };
    return &l->lines[l->nlines++].a;
}

static void at_ATSUCopyLineControls(ppc_cpu *c)
{
    LAYOUT_ARG(src, 0);
    LAYOUT_ARG(dst, 2);
    attrset *s = line_controls(src, A(1), false);
    attrset *d = line_controls(dst, A(3), true);
    if (s)
        attrs_copy(d, s);
    else
        attrs_clear(d);
    RET(noErr);
}

static void at_ATSUSetLineControls(ppc_cpu *c)
{
    LAYOUT_ARG(l, 0);
    if (A(1) < l->offset || A(1) > text_end(l))
        RET(kATSUInvalidTextRangeErr);
    RET(set_attrs(line_controls(l, A(1), true), A(2), A(3), A(4), A(5), true));
}

static void at_ATSUGetLineControl(ppc_cpu *c)
{
    LAYOUT_ARG(l, 0);
    attrset empty = { 0, NULL }, *s = line_controls(l, A(1), false);
    RET(get_attr(s ? s : &empty, A(2), A(3), A(4), A(5)));
}

static void at_ATSUGetAllLineControls(ppc_cpu *c)
{
    LAYOUT_ARG(l, 0);
    attrset empty = { 0, NULL }, *s = line_controls(l, A(1), false);
    RET(get_all_attrs(s ? s : &empty, A(2), A(3), A(4)));
}

static void at_ATSUClearLineControls(ppc_cpu *c)
{
    LAYOUT_ARG(l, 0);
    attrset *s = line_controls(l, A(1), false);
    RET(s ? clear_attrs(s, A(2), A(3)) : noErr);
}

/* ATSUSetRunStyle(layout, style, start, length) */
static void at_ATSUSetRunStyle(ppc_cpu *c)
{
    LAYOUT_ARG(l, 0);
    if (!style_of(A(1)))
        RET(kATSUInvalidStyleErr);
    uint32_t start = A(2), len = A(3);
    int32_t e = range_check(l, &start, &len);
    if (e)
        RET(e);
    if (len)
        set_run(l, start, len, A(1));
    RET(noErr);
}

/* ATSUGetRunStyle(layout, offset, &style, &runStart, &runLength) */
static void at_ATSUGetRunStyle(ppc_cpu *c)
{
    LAYOUT_ARG(l, 0);
    uint32_t off = A(1);
    if (off == kATSUFromTextBeginning)
        off = l->offset;
    if (off < l->offset || off > text_end(l))
        RET(kATSUInvalidTextRangeErr);
    for (int i = 0; i < l->nruns; i++)
        if (off >= l->runs[i].start && off < l->runs[i].start + l->runs[i].len) {
            if (A(2))
                wr32(A(2), l->runs[i].style);
            if (A(3))
                wr32(A(3), l->runs[i].start);
            if (A(4))
                wr32(A(4), l->runs[i].len);
            RET(noErr);
        }
    RET(l->nruns ? kATSUInvalidTextRangeErr : kATSUNoStyleRunsAssignedErr);
}

/* ATSUGetContinuousAttributes(layout, offset, length, style): into the
 * style, the attributes every run over the range shares. */
static void at_ATSUGetContinuousAttributes(ppc_cpu *c)
{
    LAYOUT_ARG(l, 0);
    style_t *out = style_of(A(3));
    if (!out)
        RET(kATSUInvalidStyleErr);
    uint32_t start = A(1), len = A(2);
    int32_t e = range_check(l, &start, &len);
    if (e)
        RET(e);
    attrs_clear(&out->attrs);
    out->nfeat = out->nvar = 0;
    bool first = true;
    uint32_t end = start + (len ? len : 1);
    for (int i = 0; i < l->nruns; i++) {
        run_t *r = &l->runs[i];
        if (r->start + r->len <= start || r->start >= end)
            continue;
        style_t *st = style_of(r->style);
        if (!st)
            continue;
        if (first) {
            copy_style(out, st);
            first = false;
            continue;
        }
        for (int k = 0; k < out->attrs.n;) {
            attr *o = attrs_find(&st->attrs, out->attrs.a[k].tag);
            if (!o || !attr_equal(o, &out->attrs.a[k]))
                attrs_remove(&out->attrs, out->attrs.a[k].tag);
            else
                k++;
        }
    }
    RET(noErr);
}

/* ---------------------------------------------------------------------- */
/* Drawing and measuring */

static int32_t ready(const layout_t *l, uint32_t *start, uint32_t *len)
{
    int32_t e = range_check(l, start, len);
    if (e)
        return e;
    if (!l->nruns && l->length)
        return kATSUNoStyleRunsAssignedErr;
    if (!text_base(l) && l->length)
        return kATSUInvalidTextLayoutErr;
    return noErr;
}

/* ATSUDrawText(layout, lineOffset, lineLength, x, y) */
static void at_ATSUDrawText(ppc_cpu *c)
{
    LAYOUT_ARG(l, 0);
    uint32_t start = A(1), len = A(2);
    int32_t e = ready(l, &start, &len);
    if (e)
        RET(e);
    int x, y;
    pen_or(A(3), A(4), &x, &y);
    measure_t m;
    walk(l, start, start + len, false, 0, 0, &m);
    walk(l, start, start + len, true, x + flush_offset(l, start, m.width), y, &m);
    RET(noErr);
}

/* ATSUMeasureText(layout, lineStart, lineLength, &before, &after, &ascent, &descent) */
static void at_ATSUMeasureText(ppc_cpu *c)
{
    LAYOUT_ARG(l, 0);
    uint32_t start = A(1), len = A(2);
    int32_t e = ready(l, &start, &len);
    if (e)
        RET(e);
    measure_t m;
    walk(l, start, start + len, false, 0, 0, &m);
    attr *asc = control(l, start, kATSULineAscentTag), *desc = control(l, start, kATSULineDescentTag);
    if (A(3))
        wr32(A(3), 0);
    if (A(4))
        wr32(A(4), (uint32_t)m.width);
    if (A(5))
        wr32(A(5), asc ? be32(asc->data) : (uint32_t)m.ascent << 16);
    if (A(6))
        wr32(A(6), desc ? be32(desc->data) : (uint32_t)m.descent << 16);
    RET(noErr);
}

/* The rectangle of a range drawn on a line at (x, y). */
static void range_rect(const layout_t *l, uint32_t start, uint32_t len, int x, int y, qrect *r, int *asc,
                       int *desc)
{
    uint32_t ls = line_start(l, start);
    measure_t before, range, line;
    walk(l, ls, start, false, 0, 0, &before);
    walk(l, start, start + len, false, 0, 0, &range);
    uint32_t le = text_end(l);
    for (int i = 0; i < l->nbreaks; i++)
        if (l->breaks[i] > ls && l->breaks[i] < le)
            le = l->breaks[i];
    walk(l, ls, le, false, 0, 0, &line);
    x += flush_offset(l, ls, line.width);
    int left = x + (before.width >> 16);
    *asc = line.ascent;
    *desc = line.descent;
    *r = (qrect){ (int16_t)(y - line.ascent), (int16_t)left, (int16_t)(y + line.descent),
                  (int16_t)(left + (range.width >> 16)) };
}

/* ATSUMeasureTextImage(layout, offset, length, x, y, &rect) */
static void at_ATSUMeasureTextImage(ppc_cpu *c)
{
    LAYOUT_ARG(l, 0);
    uint32_t start = A(1), len = A(2);
    int32_t e = ready(l, &start, &len);
    if (e)
        RET(e);
    if (!A(5))
        RET(paramErr);
    measure_t m;
    walk(l, start, start + len, false, 0, 0, &m);
    int x = fixed_to_int(A(3)), y = fixed_to_int(A(4));
    qrect_put(A(5), (qrect){ (int16_t)(y - m.ascent), (int16_t)x, (int16_t)(y + m.descent),
                             (int16_t)(x + (m.width >> 16)) });
    RET(noErr);
}

/* ATSUGetGlyphBounds(layout, x, y, start, length, type, max, bounds[], &count):
 * one trapezoid, left to right. */
static void at_ATSUGetGlyphBounds(ppc_cpu *c)
{
    LAYOUT_ARG(l, 0);
    uint32_t start = A(3), len = A(4), max = A(6);
    gaddr out = A(7), count = A(8);
    int32_t e = ready(l, &start, &len);
    if (e)
        RET(e);
    int x = A(1) == kATSUUseGrafPortPenLoc ? 0 : fixed_to_int(A(1)), y = A(2) == kATSUUseGrafPortPenLoc ? 0 : fixed_to_int(A(2));
    qrect r;
    int asc, desc;
    range_rect(l, start, len, x, y, &r, &asc, &desc);
    uint32_t n = 0;
    if (out && max) {
        int32_t pts[8] = { r.left, r.top, r.right, r.top, r.right, r.bottom, r.left, r.bottom };
        for (int i = 0; i < 8; i++)
            wr32(out + 4 * (uint32_t)i, (uint32_t)(pts[i] << 16));
        n = 1;
    }
    if (count)
        wr32(count, n);
    RET(noErr);
}

/* Highlighting: the range's rectangle inverted in the highlight colour;
 * taken off, the text drawn over its erased background. */
static void hilite(ppc_cpu *c, bool on, gaddr rgn)
{
    LAYOUT_ARG(l, 0);
    uint32_t start = A(3), len = A(4);
    int32_t e = ready(l, &start, &len);
    if (e)
        RET(e);
    int x, y;
    pen_or(A(1), A(2), &x, &y);
    qrect r;
    int asc, desc;
    range_rect(l, start, len, x, y, &r, &asc, &desc);
    if (rgn) {
        tb_SetRectRgn(rgn, r.left, r.top, r.right, r.bottom);
        RET(noErr);
    }
    gaddr rp = tb_NewPtr(8, false, true);
    qrect_put(rp, r);
    if (on) {
        wr8(LM_HiliteMode, rd8(LM_HiliteMode) & 0x7F);
        tb_InvertRect(rp);
    } else {
        tb_EraseRect(rp);
        measure_t m;
        walk(l, start, start + len, true, r.left, y, &m);
    }
    tb_DisposePtr(rp);
    RET(noErr);
}

static void at_ATSUHighlightText(ppc_cpu *c) { hilite(c, true, 0); }
static void at_ATSUUnhighlightText(ppc_cpu *c) { hilite(c, false, 0); }
static void at_ATSUGetTextHighlight(ppc_cpu *c)
{
    if (!A(5))
        RET(paramErr);
    hilite(c, true, A(5));
}

/* ATSUOffsetToPosition(layout, offset, isLeading, &main, &second, &split):
 * a caret from the line's ascent to its descent. */
static void at_ATSUOffsetToPosition(ppc_cpu *c)
{
    LAYOUT_ARG(l, 0);
    uint32_t off = A(1), zero = 0;
    int32_t e = ready(l, &off, &zero);
    if (e)
        RET(e);
    qrect r;
    int asc, desc;
    range_rect(l, off, 0, 0, 0, &r, &asc, &desc);
    for (int k = 3; k <= 4; k++)
        if (A(k)) {
            wr32(A(k), (uint32_t)(r.left << 16));
            wr32(A(k) + 4, (uint32_t)(-asc << 16));
            wr32(A(k) + 8, (uint32_t)(r.left << 16));
            wr32(A(k) + 12, (uint32_t)(desc << 16));
        }
    if (A(5))
        wr8(A(5), 0);
    RET(noErr);
}

/* ATSUPositionToOffset(layout, x, y, &ioOffset, &isLeading, &secondary):
 * *ioOffset comes in as the line's start; the nearest boundary goes out. */
static void at_ATSUPositionToOffset(ppc_cpu *c)
{
    LAYOUT_ARG(l, 0);
    gaddr io = A(3);
    if (!io)
        RET(paramErr);
    uint32_t ls = rd32(io), zero = 0;
    int32_t e = ready(l, &ls, &zero);
    if (e)
        RET(e);
    uint32_t le = text_end(l);
    for (int i = 0; i < l->nbreaks; i++)
        if (l->breaks[i] > ls && l->breaks[i] < le)
            le = l->breaks[i];
    measure_t line;
    walk(l, ls, le, false, 0, 0, &line);
    int32_t x = (int32_t)A(1) - (flush_offset(l, ls, line.width) << 16);
    uint32_t best = ls;
    int32_t prev = 0;
    for (uint32_t i = ls + 1; i <= le; i++) {
        measure_t m;
        walk(l, ls, i, false, 0, 0, &m);
        if (m.width >= x) {
            best = x - prev < m.width - x ? i - 1 : i;
            break;
        }
        prev = m.width;
        best = i;
    }
    gaddr base = text_base(l);
    if (best > ls && best < le && base && (rd16(base + 2 * best) & 0xFC00) == 0xDC00)
        best--; /* not inside a surrogate pair */
    if (best == le && le > ls && base) { /* not past the line's end */
        uint16_t u = rd16(base + 2 * (le - 1));
        if (u == 0x0D || u == 0x0A)
            best = le - 1;
    }
    wr32(io, best);
    if (A(4))
        wr8(A(4), 1);
    if (A(5))
        wr32(A(5), best);
    RET(noErr);
}

/* Cursor movement: by character (a surrogate pair is one) or by word. */
static bool word_char(uint16_t u) { return u >= 0x80 || (u >= '0' && u <= '9') || ((u | 0x20) >= 'a' && (u | 0x20) <= 'z') || u == '_'; }

static uint32_t step(const layout_t *l, uint32_t off, int dir, uint16_t how)
{
    gaddr base = text_base(l);
    uint32_t lo = l->offset, hi = text_end(l);
    if (!base)
        return off;
    if (how == kATSUByWord) {
        if (dir > 0) {
            while (off < hi && !word_char(rd16(base + 2 * off)))
                off++;
            while (off < hi && word_char(rd16(base + 2 * off)))
                off++;
        } else {
            while (off > lo && !word_char(rd16(base + 2 * (off - 1))))
                off--;
            while (off > lo && word_char(rd16(base + 2 * (off - 1))))
                off--;
        }
        return off;
    }
    if (dir > 0 && off < hi) {
        off++;
        if (off < hi && (rd16(base + 2 * off) & 0xFC00) == 0xDC00)
            off++;
    } else if (dir < 0 && off > lo) {
        off--;
        if (off > lo && (rd16(base + 2 * off) & 0xFC00) == 0xDC00)
            off--;
    }
    return off;
}

static void move_cursor(ppc_cpu *c, int dir)
{
    LAYOUT_ARG(l, 0);
    uint32_t off = A(1);
    if (off < l->offset || off > text_end(l))
        RET(kATSUInvalidTextRangeErr);
    if (!A(3))
        RET(paramErr);
    wr32(A(3), step(l, off, dir, (uint16_t)A(2)));
    RET(noErr);
}

static void at_ATSUNextCursorPosition(ppc_cpu *c) { move_cursor(c, 1); }
static void at_ATSUPreviousCursorPosition(ppc_cpu *c) { move_cursor(c, -1); }

/* ATSUBreakLine(layout, lineStart, lineWidth, soft, &break): the last
 * space that fits, a hard line end, or mid-word when no space does. */
static void add_break(layout_t *l, uint32_t b)
{
    for (int i = 0; i < l->nbreaks; i++)
        if (l->breaks[i] == b)
            return;
    l->breaks = realloc(l->breaks, sizeof *l->breaks * (size_t)(l->nbreaks + 1));
    int i = l->nbreaks++;
    while (i > 0 && l->breaks[i - 1] > b) {
        l->breaks[i] = l->breaks[i - 1];
        i--;
    }
    l->breaks[i] = b;
}

static void at_ATSUBreakLine(ppc_cpu *c)
{
    LAYOUT_ARG(l, 0);
    uint32_t start = A(1), width = A(2), zero = 0;
    bool soft = (A(3) & 0xFF) != 0;
    int32_t e = ready(l, &start, &zero);
    if (e)
        RET(e);
    if (width == kATSUUseLineControlWidth) {
        attr *w = control(l, start, kATSULineWidthTag);
        width = w ? be32(w->data) : 0;
    }
    gaddr base = text_base(l);
    uint32_t end = text_end(l), brk = end, space = 0;
    bool in_word = false;
    for (uint32_t i = start; i < end; i++) {
        uint16_t u = base ? rd16(base + 2 * i) : 0;
        if (u == 0x0D || u == 0x0A || u == 0x2028 || u == 0x2029) {
            brk = i + 1;
            break;
        }
        measure_t m;
        walk(l, start, i + 1, false, 0, 0, &m);
        if (width && (uint32_t)m.width > width && u != ' ') {
            if (space > start)
                brk = space;
            else {
                brk = i > start ? i : i + 1;
                in_word = true;
            }
            break;
        }
        if (u == ' ' || u == '\t')
            space = i + 1;
    }
    if (soft && brk < end)
        add_break(l, brk);
    if (A(4))
        wr32(A(4), brk);
    RET(in_word ? kATSULineBreakInWord : noErr);
}

static void at_ATSUSetSoftLineBreak(ppc_cpu *c)
{
    LAYOUT_ARG(l, 0);
    if (A(1) <= l->offset || A(1) > text_end(l))
        RET(kATSUInvalidTextRangeErr);
    add_break(l, A(1));
    RET(noErr);
}

/* ATSUGetSoftLineBreaks(layout, start, length, max, breaks[], &count) */
static void at_ATSUGetSoftLineBreaks(ppc_cpu *c)
{
    LAYOUT_ARG(l, 0);
    uint32_t start = A(1), len = A(2), max = A(3);
    int32_t e = range_check(l, &start, &len);
    if (e)
        RET(e);
    uint32_t n = 0;
    for (int i = 0; i < l->nbreaks; i++)
        if (l->breaks[i] >= start && l->breaks[i] <= start + len) {
            if (A(4) && n < max)
                wr32(A(4) + 4 * n, l->breaks[i]);
            n++;
        }
    if (A(5))
        wr32(A(5), n < max || !A(4) ? n : max);
    RET(noErr);
}

static void at_ATSUClearSoftLineBreaks(ppc_cpu *c)
{
    LAYOUT_ARG(l, 0);
    uint32_t start = A(1), len = A(2);
    int32_t e = range_check(l, &start, &len);
    if (e)
        RET(e);
    int k = 0;
    for (int i = 0; i < l->nbreaks; i++)
        if (l->breaks[i] < start || l->breaks[i] > start + len)
            l->breaks[k++] = l->breaks[i];
    l->nbreaks = k;
    RET(noErr);
}

/* ---------------------------------------------------------------------- */
/* Fonts */

static void at_ATSUSetFontFallbacks(ppc_cpu *c)
{
    uint32_t n = A(0);
    if (n > 64 || (n && !A(1)))
        RET(paramErr);
    for (uint32_t i = 0; i < n; i++) {
        int16_t f;
        uint8_t face;
        if (!font_parts(rd32(A(1) + 4 * i), &f, &face))
            RET(kATSUInvalidFontErr);
        fallbacks[i] = rd32(A(1) + 4 * i);
    }
    nfallbacks = (int)n;
    fallback_method = (uint16_t)A(2);
    RET(noErr);
}

static void at_ATSUGetFontFallbacks(ppc_cpu *c)
{
    uint32_t max = A(0);
    for (uint32_t i = 0; i < (uint32_t)nfallbacks && i < max && A(1); i++)
        wr32(A(1) + 4 * i, fallbacks[i]);
    if (A(2))
        wr16(A(2), fallback_method);
    if (A(3))
        wr32(A(3), (uint32_t)nfallbacks);
    RET(noErr);
}

/* ATSUMatchFontsToText(layout, start, length, &font, &changedOffset,
 * &changedLength): the families here hold Mac OS Roman; text outside it
 * has no font. */
static void at_ATSUMatchFontsToText(ppc_cpu *c)
{
    LAYOUT_ARG(l, 0);
    uint32_t start = A(1), len = A(2);
    int32_t e = ready(l, &start, &len);
    if (e)
        RET(e);
    gaddr base = text_base(l);
    uint32_t bad = 0, nbad = 0;
    for (uint32_t i = start; i < start + len && base; i++) {
        uint16_t u = rd16(base + 2 * i);
        uint8_t b;
        bool ok = u < 0x80 || u == '?' || (svc_utf16_to_macroman(&u, 1, &b) == 1 && b != '?');
        if (!ok) {
            if (!nbad)
                bad = i;
            nbad++;
        } else if (nbad) {
            break;
        }
    }
    if (A(3))
        wr32(A(3), 0);
    if (A(4))
        wr32(A(4), nbad ? bad : start + len);
    if (A(5))
        wr32(A(5), nbad);
    RET(nbad ? kATSUFontsNotMatched : noErr);
}

static void at_ATSUSetTransientFontMatching(ppc_cpu *c)
{
    LAYOUT_ARG(l, 0);
    l->transient = (A(1) & 0xFF) != 0;
    RET(noErr);
}

static void at_ATSUGetTransientFontMatching(ppc_cpu *c)
{
    LAYOUT_ARG(l, 0);
    if (!A(1))
        RET(paramErr);
    wr8(A(1), l->transient);
    RET(noErr);
}

static void at_ATSUFontCount(ppc_cpu *c)
{
    if (!A(0))
        RET(paramErr);
    int16_t ids[MAX_FAMILIES];
    wr32(A(0), (uint32_t)families(ids, NULL, MAX_FAMILIES));
    RET(noErr);
}

/* ATSUGetFontIDs(ids[], arraySize, &count) */
static void at_ATSUGetFontIDs(ppc_cpu *c)
{
    int16_t ids[MAX_FAMILIES];
    int n = families(ids, NULL, MAX_FAMILIES);
    for (uint32_t i = 0; i < (uint32_t)n && i < A(1) && A(0); i++)
        wr32(A(0) + 4 * i, font_id(ids[i], 0));
    if (A(2))
        wr32(A(2), (uint32_t)n);
    RET(noErr);
}

static bool family_exists(int16_t family)
{
    uint8_t name[256];
    return family_name(family, name);
}

static void at_ATSUFONDtoFontID(ppc_cpu *c)
{
    int16_t family = (int16_t)A(0);
    if (!A(2))
        RET(paramErr);
    if (!family_exists(family))
        RET(kATSUNoCorrespondingFontErr);
    wr32(A(2), font_id(family, (uint8_t)A(1)));
    RET(noErr);
}

static void at_ATSUFontIDtoFOND(ppc_cpu *c)
{
    int16_t family;
    uint8_t face;
    if (!font_parts(A(0), &family, &face) || !family_exists(family))
        RET(kATSUInvalidFontErr);
    if (A(1))
        wr16(A(1), (uint16_t)family);
    if (A(2))
        wr8(A(2), face);
    RET(noErr);
}

/* A font's names: family, style, unique and full. */
static const uint16_t name_codes[] = { kFontFamilyName, kFontStyleName, kFontUniqueName, kFontFullName };
enum { NNAMES = 4 };

static int font_name(uint32_t id, int index, uint8_t *out)
{
    int16_t family;
    uint8_t face, fam[256];
    if (!font_parts(id, &family, &face) || !family_name(family, fam))
        return -1;
    char style[64] = "";
    static const char *const words[] = { "Bold", "Italic", "Underline", "Outline", "Shadow", "Condensed",
                                         "Extended" };
    for (int b = 0; b < 7; b++)
        if (face & (1 << b)) {
            if (style[0])
                strcat(style, " ");
            strcat(style, words[b]);
        }
    const char *sname = style[0] ? style : "Regular";
    int n = 0;
    switch (name_codes[index]) {
    case kFontFamilyName:
        memcpy(out, fam + 1, fam[0]);
        return fam[0];
    case kFontStyleName:
        n = (int)strlen(sname);
        memcpy(out, sname, (size_t)n);
        return n;
    case kFontUniqueName:
    case kFontFullName:
        memcpy(out, fam + 1, fam[0]);
        n = fam[0];
        if (style[0]) {
            out[n++] = ' ';
            memcpy(out + n, style, strlen(style));
            n += (int)strlen(style);
        }
        return n;
    }
    return -1;
}

static void at_ATSUCountFontNames(ppc_cpu *c)
{
    uint8_t buf[300];
    if (font_name(A(0), 0, buf) < 0)
        RET(kATSUInvalidFontErr);
    if (A(1))
        wr32(A(1), NNAMES);
    RET(noErr);
}

static void put_name(const uint8_t *name, int n, uint32_t max, gaddr out, gaddr actual)
{
    if (out)
        gput(out, name, (uint32_t)n < max ? (uint32_t)n : max);
    if (actual)
        wr32(actual, (uint32_t)n);
}

/* ATSUGetIndFontName(id, index, max, name, &length, &code, &platform, &script, &language) */
static void at_ATSUGetIndFontName(ppc_cpu *c)
{
    uint8_t buf[300];
    if (font_name(A(0), 0, buf) < 0)
        RET(kATSUInvalidFontErr);
    uint32_t index = A(1);
    if (index >= NNAMES)
        RET(kATSUNotSetErr);
    int n = font_name(A(0), (int)index, buf);
    put_name(buf, n, A(2), A(3), A(4));
    if (A(5))
        wr32(A(5), name_codes[index]);
    if (A(6))
        wr32(A(6), kFontMacintoshPlatform);
    if (A(7))
        wr32(A(7), kFontRomanScript);
    if (A(8))
        wr32(A(8), kFontEnglishLanguage);
    RET(noErr);
}

static bool name_matches(uint32_t code, uint32_t platform, uint32_t script, uint32_t lang, int index)
{
    return (code == (uint32_t)kFontNoName || code == name_codes[index]) &&
           (platform == (uint32_t)kFontNoPlatform || platform == kFontMacintoshPlatform || platform == 0) &&
           (script == (uint32_t)kFontNoScript || script == kFontRomanScript) &&
           (lang == (uint32_t)kFontNoLanguage || lang == kFontEnglishLanguage);
}

/* ATSUFindFontName(id, code, platform, script, language, max, name, &length, &index) */
static void at_ATSUFindFontName(ppc_cpu *c)
{
    uint8_t buf[300];
    if (font_name(A(0), 0, buf) < 0)
        RET(kATSUInvalidFontErr);
    for (int i = 0; i < NNAMES; i++)
        if (name_matches(A(1), A(2), A(3), A(4), i)) {
            int n = font_name(A(0), i, buf);
            put_name(buf, n, A(5), A(6), A(7));
            if (A(8))
                wr32(A(8), (uint32_t)i);
            RET(noErr);
        }
    RET(kATSUNotSetErr);
}

/* ATSUFindFontFromName(name, length, code, platform, script, language, &id):
 * a family's name, or a full name with its style. */
static void at_ATSUFindFontFromName(ppc_cpu *c)
{
    gaddr name = A(0), out = A(6);
    uint32_t len = A(1);
    if (!name || !out || len > 255)
        RET(paramErr);
    uint8_t want[256];
    gget(want, name, len);
    int16_t ids[MAX_FAMILIES];
    int n = families(ids, NULL, MAX_FAMILIES);
    for (int f = 0; f < n; f++)
        for (uint8_t face = 0; face < 4; face++) /* plain, bold, italic, bold italic */
            for (int i = 0; i < NNAMES; i++) {
                if (!name_matches(A(2), A(3), A(4), A(5), i))
                    continue;
                if (face && name_codes[i] == kFontFamilyName)
                    continue;
                uint8_t buf[300];
                int k = font_name(font_id(ids[f], face), i, buf);
                if (k == (int)len && !memcmp(buf, want, len) && name_codes[i] != kFontStyleName) {
                    wr32(out, font_id(ids[f], face));
                    RET(noErr);
                }
            }
    wr32(out, 0);
    RET(kATSUInvalidFontErr);
}

/* Bitmap families: no feature, tracking or variation tables. */
static bool valid_font(uint32_t id)
{
    int16_t family;
    uint8_t face;
    return font_parts(id, &family, &face) && family_exists(family);
}

static void count_none(ppc_cpu *c, int out)
{
    if (!valid_font(A(0)))
        RET(kATSUInvalidFontErr);
    if (A(out))
        wr32(A(out), 0);
    RET(noErr);
}

static void at_ATSUCountFontFeatureTypes(ppc_cpu *c) { count_none(c, 1); }
static void at_ATSUCountFontFeatureSelectors(ppc_cpu *c) { count_none(c, 2); }
static void at_ATSUGetFontFeatureTypes(ppc_cpu *c) { count_none(c, 3); }
static void at_ATSUCountFontTracking(ppc_cpu *c) { count_none(c, 2); }
static void at_ATSUCountFontVariations(ppc_cpu *c) { count_none(c, 1); }
static void at_ATSUCountFontInstances(ppc_cpu *c) { count_none(c, 1); }
static void at_ATSUGetFontInstance(ppc_cpu *c)
{
    if (!valid_font(A(0)))
        RET(kATSUInvalidFontErr);
    RET(paramErr); /* no instance at that index */
}

/* ATSUGetFontFeatureSelectors(id, type, max, selectors[], onByDefault[], &count, &exclusive) */
static void at_ATSUGetFontFeatureSelectors(ppc_cpu *c)
{
    if (!valid_font(A(0)))
        RET(kATSUInvalidFontErr);
    if (A(5))
        wr32(A(5), 0);
    if (A(6))
        wr8(A(6), 0);
    RET(noErr);
}

static void at_no_entry(ppc_cpu *c)
{
    if (!valid_font(A(0)))
        RET(kATSUInvalidFontErr);
    RET(kATSUNotSetErr);
}

/* ---------------------------------------------------------------------- */
/* Glyphs: here a glyph is a Mac OS Roman character code. */

enum { GI_SIZE = 28, GIA_HEADER = 8 };

/* ATSUGetGlyphInfo(layout, start, length, &bufferSize, array) */
static void at_ATSUGetGlyphInfo(ppc_cpu *c)
{
    LAYOUT_ARG(l, 0);
    uint32_t start = A(1), len = A(2);
    gaddr size = A(3), out = A(4);
    int32_t e = ready(l, &start, &len);
    if (e)
        RET(e);
    if (!size)
        RET(paramErr);
    gaddr base = text_base(l);
    uint32_t n = 0;
    for (uint32_t i = start; i < start + len; i++)
        if (!(i > start && (rd16(base + 2 * i) & 0xFC00) == 0xDC00))
            n++;
    uint32_t need = GIA_HEADER + GI_SIZE * n, room = rd32(size);
    if (out && room >= GIA_HEADER) {
        uint32_t fit = (room - GIA_HEADER) / GI_SIZE, k = 0;
        wr32(out, l->addr);
        port_state ps;
        save_port(&ps);
        int32_t x = 0;
        for (uint32_t i = start; i < start + len && k < fit; i++) {
            uint16_t u[2] = { rd16(base + 2 * i), 0 };
            int units = 1;
            if ((u[0] & 0xFC00) == 0xD800 && i + 1 < start + len) {
                u[1] = rd16(base + 2 * (i + 1));
                units = 2;
            }
            uint8_t b = '?';
            svc_utf16_to_macroman(u, (size_t)units, &b);
            gaddr st = style_at(l, i, NULL);
            use_style(st, false);
            gaddr g = out + GIA_HEADER + GI_SIZE * k++;
            wr16(g, b);
            wr16(g + 2, 0);
            wr32(g + 4, 0);
            wr32(g + 8, i);
            wr32(g + 12, st);
            wr32(g + 16, 0);
            wr32(g + 20, svc_f32_bits((float)x));
            wr16(g + 24, (uint16_t)x);
            wr16(g + 26, (uint16_t)x);
            bool line_end = u[0] == 0x0D || u[0] == 0x0A;
            x += line_end ? 0 : qd_text_width(ps.port, &b, 1);
            i += (uint32_t)units - 1;
        }
        restore_port(&ps);
        wr32(out + 4, k);
    }
    wr32(size, need);
    RET(noErr);
}

/* ATSUDrawGlyphInfo(array, location): the location, a Float32Point, comes
 * in r4 and r5. */
static void at_ATSUDrawGlyphInfo(ppc_cpu *c)
{
    gaddr arr = A(0);
    if (!arr)
        RET(paramErr);
    float lx = svc_f32(A(1)), ly = svc_f32(A(2));
    uint32_t n = rd32(arr + 4);
    port_state ps;
    save_port(&ps);
    gaddr buf = tb_NewPtr(2, false, true);
    for (uint32_t k = 0; k < n; k++) {
        gaddr g = arr + GIA_HEADER + GI_SIZE * k;
        use_style(rd32(g + 12), true);
        wr8(buf, (uint8_t)rd16(g));
        tb_MoveTo((int16_t)(lx + svc_f32(rd32(g + 20)) + 0.5f), (int16_t)(ly + svc_f32(rd32(g + 16)) + 0.5f));
        tb_DrawText(buf, 0, 1);
    }
    tb_DisposePtr(buf);
    restore_port(&ps);
    RET(noErr);
}

/* Each glyph's advance from its character's width. */
static void glyph_metrics(ppc_cpu *c, bool screen)
{
    STYLE_ARG(st, 0);
    uint32_t n = A(1), stride = A(3) ? A(3) : 2;
    gaddr ids = A(2), out = screen ? A(6) : A(4);
    if (n && (!ids || !out))
        RET(paramErr);
    port_state ps;
    save_port(&ps);
    use_style(st->addr, false);
    int asc, desc, lead;
    font_metrics(&asc, &desc, &lead);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t b = (uint8_t)rd16(ids + stride * i);
        float w = (float)qd_text_width(ps.port, &b, 1);
        if (screen) { /* deviceAdvance, topLeft, height, width, side bearings */
            gaddr m = out + 40 * i;
            gmemset(m, 0, 40);
            wr32(m, svc_f32_bits(w));
            wr32(m + 12, svc_f32_bits((float)-asc));
            wr32(m + 16, (uint32_t)(asc + desc));
            wr32(m + 20, (uint32_t)w);
        } else { /* advance, side bearings */
            gaddr m = out + 24 * i;
            gmemset(m, 0, 24);
            wr32(m, svc_f32_bits(w));
        }
    }
    restore_port(&ps);
    RET(noErr);
}

static void at_ATSUGlyphGetIdealMetrics(ppc_cpu *c) { glyph_metrics(c, false); }
static void at_ATSUGlyphGetScreenMetrics(ppc_cpu *c) { glyph_metrics(c, true); }

/* Outlines: the glyphs are bitmaps. */
static void at_no_outlines(ppc_cpu *c)
{
    STYLE_ARG(st, 0);
    (void)st;
    RET(kATSUNoFontScalerAvailableErr);
}

const ppc_export carbon_atsui_exports[] = {
    { "ATSUBreakLine", at_ATSUBreakLine },
    { "ATSUCalculateBaselineDeltas", at_ATSUCalculateBaselineDeltas },
    { "ATSUClearAttributes", at_ATSUClearAttributes },
    { "ATSUClearFontFeatures", at_ATSUClearFontFeatures },
    { "ATSUClearFontVariations", at_ATSUClearFontVariations },
    { "ATSUClearLayoutCache", at_layout_noop },
    { "ATSUClearLayoutControls", at_ATSUClearLayoutControls },
    { "ATSUClearLineControls", at_ATSUClearLineControls },
    { "ATSUClearSoftLineBreaks", at_ATSUClearSoftLineBreaks },
    { "ATSUClearStyle", at_ATSUClearStyle },
    { "ATSUCompareStyles", at_ATSUCompareStyles },
    { "ATSUCopyAttributes", at_ATSUCopyAttributes },
    { "ATSUCopyLayoutControls", at_ATSUCopyLayoutControls },
    { "ATSUCopyLineControls", at_ATSUCopyLineControls },
    { "ATSUCopyToHandle", at_ATSUCopyToHandle },
    { "ATSUCountFontFeatureSelectors", at_ATSUCountFontFeatureSelectors },
    { "ATSUCountFontFeatureTypes", at_ATSUCountFontFeatureTypes },
    { "ATSUCountFontInstances", at_ATSUCountFontInstances },
    { "ATSUCountFontNames", at_ATSUCountFontNames },
    { "ATSUCountFontTracking", at_ATSUCountFontTracking },
    { "ATSUCountFontVariations", at_ATSUCountFontVariations },
    { "ATSUCreateAndCopyStyle", at_ATSUCreateAndCopyStyle },
    { "ATSUCreateAndCopyTextLayout", at_ATSUCreateAndCopyTextLayout },
    { "ATSUCreateStyle", at_ATSUCreateStyle },
    { "ATSUCreateTextLayout", at_ATSUCreateTextLayout },
    { "ATSUCreateTextLayoutWithTextHandle", at_ATSUCreateTextLayoutWithTextHandle },
    { "ATSUCreateTextLayoutWithTextPtr", at_ATSUCreateTextLayoutWithTextPtr },
    { "ATSUDisposeStyle", at_ATSUDisposeStyle },
    { "ATSUDisposeTextLayout", at_ATSUDisposeTextLayout },
    { "ATSUDrawGlyphInfo", at_ATSUDrawGlyphInfo },
    { "ATSUDrawText", at_ATSUDrawText },
    { "ATSUFONDtoFontID", at_ATSUFONDtoFontID },
    { "ATSUFindFontFromName", at_ATSUFindFontFromName },
    { "ATSUFindFontName", at_ATSUFindFontName },
    { "ATSUFontCount", at_ATSUFontCount },
    { "ATSUFontIDtoFOND", at_ATSUFontIDtoFOND },
    { "ATSUGetAllAttributes", at_ATSUGetAllAttributes },
    { "ATSUGetAllFontFeatures", at_ATSUGetAllFontFeatures },
    { "ATSUGetAllFontVariations", at_ATSUGetAllFontVariations },
    { "ATSUGetAllLayoutControls", at_ATSUGetAllLayoutControls },
    { "ATSUGetAllLineControls", at_ATSUGetAllLineControls },
    { "ATSUGetAttribute", at_ATSUGetAttribute },
    { "ATSUGetContinuousAttributes", at_ATSUGetContinuousAttributes },
    { "ATSUGetFontFallbacks", at_ATSUGetFontFallbacks },
    { "ATSUGetFontFeature", at_ATSUGetFontFeature },
    { "ATSUGetFontFeatureNameCode", at_no_entry },
    { "ATSUGetFontFeatureSelectors", at_ATSUGetFontFeatureSelectors },
    { "ATSUGetFontFeatureTypes", at_ATSUGetFontFeatureTypes },
    { "ATSUGetFontIDs", at_ATSUGetFontIDs },
    { "ATSUGetFontInstance", at_ATSUGetFontInstance },
    { "ATSUGetFontInstanceNameCode", at_no_entry },
    { "ATSUGetFontVariationNameCode", at_no_entry },
    { "ATSUGetFontVariationValue", at_ATSUGetFontVariationValue },
    { "ATSUGetGlyphBounds", at_ATSUGetGlyphBounds },
    { "ATSUGetGlyphInfo", at_ATSUGetGlyphInfo },
    { "ATSUGetIndFontName", at_ATSUGetIndFontName },
    { "ATSUGetIndFontTracking", at_no_entry },
    { "ATSUGetIndFontVariation", at_no_entry },
    { "ATSUGetLayoutControl", at_ATSUGetLayoutControl },
    { "ATSUGetLineControl", at_ATSUGetLineControl },
    { "ATSUGetNativeCurveType", at_no_outlines },
    { "ATSUGetRunStyle", at_ATSUGetRunStyle },
    { "ATSUGetSoftLineBreaks", at_ATSUGetSoftLineBreaks },
    { "ATSUGetStyleRefCon", at_ATSUGetStyleRefCon },
    { "ATSUGetTextHighlight", at_ATSUGetTextHighlight },
    { "ATSUGetTextLayoutRefCon", at_ATSUGetTextLayoutRefCon },
    { "ATSUGetTextLocation", at_ATSUGetTextLocation },
    { "ATSUGetTransientFontMatching", at_ATSUGetTransientFontMatching },
    { "ATSUGlyphGetCubicPaths", at_no_outlines },
    { "ATSUGlyphGetCurvePaths", at_no_outlines },
    { "ATSUGlyphGetIdealMetrics", at_ATSUGlyphGetIdealMetrics },
    { "ATSUGlyphGetQuadraticPaths", at_no_outlines },
    { "ATSUGlyphGetScreenMetrics", at_ATSUGlyphGetScreenMetrics },
    { "ATSUHighlightText", at_ATSUHighlightText },
    { "ATSUIdle", at_layout_noop },
    { "ATSULeftwardCursorPosition", at_ATSUPreviousCursorPosition },
    { "ATSUMatchFontsToText", at_ATSUMatchFontsToText },
    { "ATSUMeasureText", at_ATSUMeasureText },
    { "ATSUMeasureTextImage", at_ATSUMeasureTextImage },
    { "ATSUNextCursorPosition", at_ATSUNextCursorPosition },
    { "ATSUOffsetToPosition", at_ATSUOffsetToPosition },
    { "ATSUOverwriteAttributes", at_ATSUOverwriteAttributes },
    { "ATSUPasteFromHandle", at_ATSUPasteFromHandle },
    { "ATSUPositionToOffset", at_ATSUPositionToOffset },
    { "ATSUPreviousCursorPosition", at_ATSUPreviousCursorPosition },
    { "ATSURightwardCursorPosition", at_ATSUNextCursorPosition },
    { "ATSUSetAttributes", at_ATSUSetAttributes },
    { "ATSUSetFontFallbacks", at_ATSUSetFontFallbacks },
    { "ATSUSetFontFeatures", at_ATSUSetFontFeatures },
    { "ATSUSetLayoutControls", at_ATSUSetLayoutControls },
    { "ATSUSetLineControls", at_ATSUSetLineControls },
    { "ATSUSetRunStyle", at_ATSUSetRunStyle },
    { "ATSUSetSoftLineBreak", at_ATSUSetSoftLineBreak },
    { "ATSUSetStyleRefCon", at_ATSUSetStyleRefCon },
    { "ATSUSetTextHandleLocation", at_ATSUSetTextHandleLocation },
    { "ATSUSetTextLayoutRefCon", at_ATSUSetTextLayoutRefCon },
    { "ATSUSetTextPointerLocation", at_ATSUSetTextPointerLocation },
    { "ATSUSetTransientFontMatching", at_ATSUSetTransientFontMatching },
    { "ATSUSetVariations", at_ATSUSetVariations },
    { "ATSUStyleIsEmpty", at_ATSUStyleIsEmpty },
    { "ATSUTextDeleted", at_ATSUTextDeleted },
    { "ATSUTextInserted", at_ATSUTextInserted },
    { "ATSUTextMoved", at_ATSUTextMoved },
    { "ATSUUnderwriteAttributes", at_ATSUUnderwriteAttributes },
    { "ATSUUnhighlightText", at_ATSUUnhighlightText },
};
const unsigned carbon_atsui_export_count = sizeof carbon_atsui_exports / sizeof carbon_atsui_exports[0];
