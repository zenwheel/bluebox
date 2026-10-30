/*
 * Picture Utilities (Pack15): what a picture or pixel map holds.
 *
 * GetPictInfo surveys a picture by drawing it, as Apple's package did,
 * into a port of its own that shows nothing; the playback (qd_picture.c)
 * tells the survey each opcode, image, colour, font and comment.  Out of
 * that come the counts of each kind of drawing, the deepest pixel map,
 * the resolution, the fonts and comments used, and the colours: how many
 * different ones, and the most representative few as a colour table or
 * palette (the popular method, or median cut).  NewPictInfo, RecordPictInfo,
 * RecordPixMapInfo and RetrievePictInfo survey several together.
 */
#include <stdlib.h>
#include <string.h>
#include "qd_internal.h"
#include "tb_internal.h"

enum {
    returnColorTable = 1, returnPalette = 2, recordComments = 4, recordFontInfo = 8, suppressBlackAndWhite = 16,
    systemMethod = 0, popularMethod = 1, medianMethod = 2,
    pictInfoVersionErr = -11000, pictInfoIDErr = -11001, pictInfoVerbErr = -11002,
    cantLoadPickMethodErr = -11003, colorsRequestedErr = -11004, pictureDataErr = -11005,
    HIST = 1 << 15, /* 5 bits of each component, as the package's histogram */
};

typedef struct {
    int16_t pict_id, sys_id;
    uint32_t sizes[4]; /* a bit for each size from 0 to 127 the text used */
    int16_t style;
    uint8_t name[256];
} font_rec;

struct pict_survey {
    int16_t verb, colors, method;
    int version, depth;
    uint32_t hres, vres;
    qrect src;
    bool any_src;
    uint32_t text, line, rect, rrect, oval, arc, poly, region, bitmap, pixmap, comment;
    uint8_t *seen;     /* one bit for each 24-bit colour met */
    uint32_t unique;
    uint32_t *count;   /* the histogram: pixels in each cell */
    uint64_t (*sum)[3]; /* and their components added up, for the cell's colour */
    struct { int16_t id, count; } *comments;
    int ncomments;
    font_rec *fonts;
    int nfonts;
    int16_t pict_font; /* the picture's txFont, as its own number */
};

pict_survey *qd_survey;

static pict_survey *survey_new(int16_t verb, int16_t colors, int16_t method)
{
    pict_survey *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->verb = verb;
    s->colors = colors;
    s->method = method;
    s->depth = 0;
    s->hres = s->vres = 72u << 16;
    if (verb & (returnColorTable | returnPalette)) {
        s->seen = calloc(1, 1u << 21);
        s->count = calloc(HIST, sizeof *s->count);
        s->sum = calloc(HIST, sizeof *s->sum);
    }
    return s;
}

static void survey_free(pict_survey *s)
{
    if (!s)
        return;
    free(s->seen);
    free(s->count);
    free(s->sum);
    free(s->comments);
    free(s->fonts);
    free(s);
}

/* ---------------------------------------------------------------------- */
/* What the playback reports */

static void add_color(pict_survey *s, uint8_t r, uint8_t g, uint8_t b, uint32_t n)
{
    uint32_t c = (uint32_t)r << 16 | (uint32_t)g << 8 | b;
    if (!(s->seen[c >> 3] & (1u << (c & 7)))) {
        s->seen[c >> 3] |= (uint8_t)(1u << (c & 7));
        s->unique++;
    }
    uint32_t cell = (uint32_t)(r >> 3) << 10 | (uint32_t)(g >> 3) << 5 | (uint32_t)(b >> 3);
    s->count[cell] += n;
    s->sum[cell][0] += (uint64_t)r * n;
    s->sum[cell][1] += (uint64_t)g * n;
    s->sum[cell][2] += (uint64_t)b * n;
}

void survey_rgb(pict_survey *s, const uint16_t rgb[3])
{
    if (s->seen)
        add_color(s, (uint8_t)(rgb[0] >> 8), (uint8_t)(rgb[1] >> 8), (uint8_t)(rgb[2] >> 8), 1);
}

/* An image drawn: a bit map (depth 1) or a pixel map, with its pixels
 * as RGBA when they were decoded. */
void survey_image(pict_survey *s, bool pixmap, int depth, const uint8_t *rgba, int w, int h)
{
    if (pixmap)
        s->pixmap++;
    else
        s->bitmap++;
    if (depth > s->depth)
        s->depth = depth;
    if (!s->seen || !rgba || w <= 0 || h <= 0)
        return;
    for (size_t i = 0, n = (size_t)w * (size_t)h; i < n; i++)
        add_color(s, rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2], 1);
}

/* Each opcode, for the counts of each kind of drawing (images and
 * comments are told apart where they are read). */
void survey_op(pict_survey *s, int op)
{
    if (op >= 0x20 && op <= 0x23)
        s->line++;
    else if (op >= 0x28 && op <= 0x2B)
        s->text++;
    else if (op >= 0x30 && op <= 0x8F && (op & 7) <= 4) {
        uint32_t *k[] = { &s->rect, &s->rrect, &s->oval, &s->arc, &s->poly, NULL, &s->region };
        uint32_t *c = k[(op - 0x30) >> 4];
        if (c)
            (*c)++;
    }
}

void survey_comment(pict_survey *s, int kind)
{
    s->comment++;
    if (!(s->verb & recordComments))
        return;
    for (int i = 0; i < s->ncomments; i++)
        if (s->comments[i].id == kind) {
            s->comments[i].count++;
            return;
        }
    void *n = realloc(s->comments, (size_t)(s->ncomments + 1) * sizeof *s->comments);
    if (!n)
        return;
    s->comments = n;
    s->comments[s->ncomments].id = (int16_t)kind;
    s->comments[s->ncomments++].count = 1;
}

static font_rec *font_for(pict_survey *s, int pict_id)
{
    for (int i = 0; i < s->nfonts; i++)
        if (s->fonts[i].pict_id == pict_id)
            return &s->fonts[i];
    void *n = realloc(s->fonts, (size_t)(s->nfonts + 1) * sizeof *s->fonts);
    if (!n)
        return NULL;
    s->fonts = n;
    font_rec *f = &s->fonts[s->nfonts++];
    memset(f, 0, sizeof *f);
    f->pict_id = f->sys_id = (int16_t)pict_id;
    return f;
}

/* txFont: the font the picture's text is in, by the picture's number. */
void survey_font(pict_survey *s, int pict_id) { s->pict_font = (int16_t)pict_id; }

/* fontName: the picture's number for a font, its name, and this system's
 * number for that name. */
void survey_font_name(pict_survey *s, int pict_id, int sys_id, const uint8_t *name)
{
    if (!(s->verb & recordFontInfo))
        return;
    font_rec *f = font_for(s, pict_id);
    if (!f)
        return;
    f->sys_id = (int16_t)sys_id;
    memcpy(f->name, name, (size_t)name[0] + 1);
}

/* Text drawn in the port's size and style. */
void survey_text(pict_survey *s, gaddr port)
{
    if (!(s->verb & recordFontInfo))
        return;
    font_rec *f = font_for(s, s->pict_font);
    if (!f)
        return;
    int size = GrafPort_txSize(port);
    if (size >= 0 && size < 128)
        f->sizes[size >> 5] |= 1u << (size & 31);
    f->style |= (uint8_t)GrafPort_txFace(port);
}

/* ---------------------------------------------------------------------- */
/* Choosing colours */

typedef struct { uint8_t lo[3], hi[3]; } box;

static uint64_t box_count(const pict_survey *s, const box *b, bool skip_bw)
{
    uint64_t n = 0;
    for (int r = b->lo[0]; r <= b->hi[0]; r++)
        for (int g = b->lo[1]; g <= b->hi[1]; g++)
            for (int bl = b->lo[2]; bl <= b->hi[2]; bl++) {
                int cell = r << 10 | g << 5 | bl;
                if (!(skip_bw && (cell == 0 || cell == HIST - 1)))
                    n += s->count[cell];
            }
    return n;
}

/* Shrink a box to the cells it has pixels in. */
static void box_fit(const pict_survey *s, box *b, bool skip_bw)
{
    uint8_t lo[3] = { 31, 31, 31 }, hi[3] = { 0, 0, 0 };
    for (int r = b->lo[0]; r <= b->hi[0]; r++)
        for (int g = b->lo[1]; g <= b->hi[1]; g++)
            for (int bl = b->lo[2]; bl <= b->hi[2]; bl++) {
                int cell = r << 10 | g << 5 | bl;
                if (!s->count[cell] || (skip_bw && (cell == 0 || cell == HIST - 1)))
                    continue;
                int c[3] = { r, g, bl };
                for (int k = 0; k < 3; k++) {
                    if (c[k] < lo[k])
                        lo[k] = (uint8_t)c[k];
                    if (c[k] > hi[k])
                        hi[k] = (uint8_t)c[k];
                }
            }
    memcpy(b->lo, lo, 3);
    memcpy(b->hi, hi, 3);
}

static void box_color(const pict_survey *s, const box *b, bool skip_bw, uint16_t out[3])
{
    uint64_t n = 0, t[3] = { 0, 0, 0 };
    for (int r = b->lo[0]; r <= b->hi[0]; r++)
        for (int g = b->lo[1]; g <= b->hi[1]; g++)
            for (int bl = b->lo[2]; bl <= b->hi[2]; bl++) {
                int cell = r << 10 | g << 5 | bl;
                if (skip_bw && (cell == 0 || cell == HIST - 1))
                    continue;
                n += s->count[cell];
                for (int k = 0; k < 3; k++)
                    t[k] += s->sum[cell][k];
            }
    for (int k = 0; k < 3; k++)
        out[k] = (uint16_t)(n ? t[k] / n * 0x101 : 0);
}

/* Median cut: the box with the most pixels split at its median along its
 * longest side, until there are as many boxes as colours wanted. */
static int pick_median(const pict_survey *s, int want, bool skip_bw, uint16_t (*out)[3])
{
    box *boxes = malloc((size_t)want * sizeof *boxes);
    if (!boxes)
        return 0;
    int n = 1;
    boxes[0] = (box){ { 0, 0, 0 }, { 31, 31, 31 } };
    box_fit(s, &boxes[0], skip_bw);
    if (boxes[0].lo[0] > boxes[0].hi[0])
        n = 0;
    while (n && n < want) {
        int best = -1;
        uint64_t most = 0;
        for (int i = 0; i < n; i++) {
            const box *b = &boxes[i];
            bool splittable = b->lo[0] < b->hi[0] || b->lo[1] < b->hi[1] || b->lo[2] < b->hi[2];
            uint64_t c = splittable ? box_count(s, b, skip_bw) : 0;
            if (c > most) {
                most = c;
                best = i;
            }
        }
        if (best < 0)
            break;
        box *b = &boxes[best];
        int axis = 0;
        for (int k = 1; k < 3; k++)
            if (b->hi[k] - b->lo[k] > b->hi[axis] - b->lo[axis])
                axis = k;
        /* The median along the axis. */
        uint64_t half = most / 2, run = 0;
        int cut = b->lo[axis];
        for (int v = b->lo[axis]; v < b->hi[axis]; v++) {
            box slab = *b;
            slab.lo[axis] = slab.hi[axis] = (uint8_t)v;
            run += box_count(s, &slab, skip_bw);
            cut = v;
            if (run >= half)
                break;
        }
        box hi = *b;
        b->hi[axis] = (uint8_t)cut;
        hi.lo[axis] = (uint8_t)(cut + 1);
        box_fit(s, b, skip_bw);
        box_fit(s, &hi, skip_bw);
        if (hi.lo[0] <= hi.hi[0])
            boxes[n++] = hi;
    }
    for (int i = 0; i < n; i++)
        box_color(s, &boxes[i], skip_bw, out[i]);
    free(boxes);
    return n;
}

/* The popular method: the cells with the most pixels. */
static int pick_popular(const pict_survey *s, int want, bool skip_bw, uint16_t (*out)[3])
{
    int n = 0;
    int *best = malloc((size_t)want * sizeof *best);
    if (!best)
        return 0;
    for (int cell = 0; cell < HIST; cell++) {
        if (!s->count[cell] || (skip_bw && (cell == 0 || cell == HIST - 1)))
            continue;
        int at = n < want ? n++ : want;
        while (at > 0 && s->count[best[at - 1]] < s->count[cell]) {
            if (at < want)
                best[at] = best[at - 1];
            at--;
        }
        if (at < want)
            best[at] = cell;
    }
    for (int i = 0; i < n; i++)
        for (int k = 0; k < 3; k++)
            out[i][k] = (uint16_t)(s->sum[best[i]][k] / s->count[best[i]] * 0x101);
    free(best);
    return n;
}

/* ---------------------------------------------------------------------- */
/* The answer */

static void put_info(const pict_survey *s, gaddr info, int16_t colors)
{
    gmemset(info, 0, PictInfo_SIZEOF);
    wr16(info + PictInfo_version_off, (uint16_t)s->version);
    wr32(info + PictInfo_hRes_off, s->hres);
    wr32(info + PictInfo_vRes_off, s->vres);
    wr16(info + PictInfo_depth_off, (uint16_t)(s->depth ? s->depth : 1));
    qrect_put(info + PictInfo_sourceRect_off, s->src);
    static const uint32_t offs[] = { PictInfo_textCount_off, PictInfo_lineCount_off, PictInfo_rectCount_off,
                                     PictInfo_rRectCount_off, PictInfo_ovalCount_off, PictInfo_arcCount_off,
                                     PictInfo_polyCount_off, PictInfo_regionCount_off, PictInfo_bitMapCount_off,
                                     PictInfo_pixMapCount_off, PictInfo_commentCount_off };
    const uint32_t counts[] = { s->text, s->line, s->rect, s->rrect, s->oval, s->arc,
                                s->poly, s->region, s->bitmap, s->pixmap, s->comment };
    for (size_t i = 0; i < sizeof offs / sizeof *offs; i++)
        wr32(info + offs[i], counts[i]);

    if (s->verb & recordComments) {
        wr32(info + PictInfo_uniqueComments_off, (uint32_t)s->ncomments);
        gaddr h = tb_NewHandle(s->ncomments * 4, false, false);
        for (int i = 0; h && rd32(h) && i < s->ncomments; i++) {
            wr16(rd32(h) + (uint32_t)i * 4, (uint16_t)s->comments[i].count);
            wr16(rd32(h) + (uint32_t)i * 4 + 2, (uint16_t)s->comments[i].id);
        }
        wr32(info + PictInfo_commentHandle_off, h);
    }
    if (s->verb & recordFontInfo) {
        /* The FontSpec records, then the names they point to. */
        uint32_t names = (uint32_t)s->nfonts * FontSpec_SIZEOF, len = names;
        for (int i = 0; i < s->nfonts; i++) {
            len += 256;
        }
        gaddr h = tb_NewHandle((int32_t)len, false, true);
        if (h && rd32(h)) {
            gaddr name = tb_NewPtr(256, false, true);
            uint32_t at = names;
            for (int i = 0; i < s->nfonts; i++) {
                const font_rec *f = &s->fonts[i];
                gaddr fs = rd32(h) + (uint32_t)i * FontSpec_SIZEOF;
                FontSpec_set_pictFontID(fs, f->pict_id);
                FontSpec_set_sysFontID(fs, f->sys_id);
                for (int k = 0; k < 4; k++)
                    wr32(fs + FontSpec_size_off + (uint32_t)k * 4, f->sizes[k]);
                FontSpec_set_style(fs, f->style);
                FontSpec_set_nameOffset(fs, (int32_t)at);
                if (f->name[0])
                    gput(name, f->name, (uint32_t)f->name[0] + 1);
                else
                    tb_GetFontName(f->sys_id, name);
                uint32_t nl = (uint32_t)rd8(name) + 1;
                gmemcpy(rd32(h) + at, name, nl);
                at += nl;
            }
            tb_DisposePtr(name);
            tb_SetHandleSize(h, (int32_t)at);
        }
        wr32(info + PictInfo_uniqueFonts_off, (uint32_t)s->nfonts);
        wr32(info + PictInfo_fontHandle_off, h);
    }
    if (!s->seen)
        return;
    wr32(info + PictInfo_uniqueColors_off, s->unique);
    bool skip_bw = s->verb & suppressBlackAndWhite;
    int want = colors > 0 ? colors : 0;
    uint16_t(*picked)[3] = calloc((size_t)(want ? want : 1), sizeof *picked);
    if (!picked)
        return;
    int n = want ? (s->method == medianMethod ? pick_median : pick_popular)(s, want, skip_bw, picked) : 0;
    gaddr ct = tb_NewHandle((int32_t)(ColorTable_ctTable_off + (uint32_t)(n ? n : 1) * ColorSpec_SIZEOF), false, true);
    if (ct && rd32(ct)) {
        gaddr t = rd32(ct);
        wr32(t, (uint32_t)tb_GetCTSeed());
        wr16(t + 4, 0);
        wr16(t + 6, (uint16_t)(n - 1));
        for (int i = 0; i < n; i++) {
            gaddr e = t + ColorTable_ctTable_off + (uint32_t)i * ColorSpec_SIZEOF;
            wr16(e, (uint16_t)i);
            for (int k = 0; k < 3; k++)
                wr16(e + 2 + (uint32_t)k * 2, picked[i][k]);
        }
    }
    free(picked);
    if (s->verb & returnPalette)
        wr32(info + PictInfo_thePalette_off, ct ? tb_NewPalette((int16_t)n, ct, 2 /* pmTolerant */, 0) : 0);
    if (s->verb & returnColorTable)
        wr32(info + PictInfo_theColorTable_off, ct);
    else if (ct)
        tb_DisposeHandle(ct);
}

static int16_t check_args(int16_t verb, int16_t colors, int16_t method, int16_t version)
{
    if (version != 0)
        return pictInfoVersionErr;
    if (verb & ~(returnColorTable | returnPalette | recordComments | recordFontInfo | suppressBlackAndWhite))
        return pictInfoVerbErr;
    if ((verb & (returnColorTable | returnPalette)) && (colors < 1 || colors > 256))
        return colorsRequestedErr;
    if (method < systemMethod || method > medianMethod)
        return cantLoadPickMethodErr; /* a 'cpkr' of the program's own */
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* Surveying */

static void unite(pict_survey *s, qrect r)
{
    if (!s->any_src) {
        s->src = r;
        s->any_src = true;
        return;
    }
    if (r.top < s->src.top) s->src.top = r.top;
    if (r.left < s->src.left) s->src.left = r.left;
    if (r.bottom > s->src.bottom) s->src.bottom = r.bottom;
    if (r.right > s->src.right) s->src.right = r.right;
}

static int16_t survey_picture(pict_survey *s, gaddr pic)
{
    if (!pic || !rd32(pic) || tb_GetHandleSize(pic) < 12)
        return pictureDataErr;
    gaddr p = rd32(pic);
    qrect frame = qrect_get(p + 2);
    bool v2 = rd16(p + 10) == 0x0011 && rd16(p + 12) == 0x02FF;
    if (s->version < (v2 ? 2 : 1))
        s->version = v2 ? 2 : 1;
    /* An extended version 2 header gives the resolution and the frame at it. */
    qrect src = frame;
    if (v2 && tb_GetHandleSize(pic) >= 40 && rd16(p + 14) == 0x0C00 && (int16_t)rd16(p + 16) == -2) {
        s->hres = rd32(p + 20);
        s->vres = rd32(p + 24);
        src = qrect_get(p + 28);
    }
    unite(s, src);

    /* Drawn into a port that shows nothing. */
    gaddr saved = qd_port();
    gaddr port = tb_NewPtr(GrafPort_SIZEOF, false, true), bm = tb_NewPtr(BitMap_SIZEOF, false, true);
    gaddr bits = tb_NewPtr(2, false, true), dst = tb_NewPtr(8, false, false);
    if (!port || !bm || !bits || !dst) {
        tb_DisposePtr(port);
        tb_DisposePtr(bm);
        tb_DisposePtr(bits);
        tb_DisposePtr(dst);
        return -108; /* memFullErr */
    }
    wr32(bm, bits);
    wr16(bm + 4, 2);
    qrect_put(bm + 6, (qrect){ 0, 0, 1, 1 });
    tb_OpenPort(port);
    tb_SetPortBits(bm);
    tb_SetEmptyRgn(GrafPort_visRgn(port));
    tb_SetEmptyRgn(GrafPort_clipRgn(port));
    qrect_put(dst, frame);
    pict_survey *outer = qd_survey;
    qd_survey = s;
    tb_DrawPicture(pic, dst);
    qd_survey = outer;
    tb_ClosePort(port);
    tb_SetPort(saved);
    tb_DisposePtr(dst);
    tb_DisposePtr(bits);
    tb_DisposePtr(bm);
    tb_DisposePtr(port);
    return noErr;
}

/* A pixel map's own pixels, through its colour table when it has one. */
static int16_t survey_pixmap(pict_survey *s, gaddr pmh)
{
    gaddr pm = pmh ? rd32(pmh) : 0;
    if (!pm)
        return pictureDataErr;
    int depth = PixMap_pixelSize(pm);
    if (!(rd16(pm + 4) & 0x8000))
        depth = 1; /* a BitMap */
    qrect b = qrect_get(pm + PixMap_bounds_off);
    unite(s, b);
    if (depth > 1 || (rd16(pm + 4) & 0x8000)) {
        s->hres = (uint32_t)PixMap_hRes(pm);
        s->vres = (uint32_t)PixMap_vRes(pm);
    }
    if (s->version < 2)
        s->version = 2;
    int w = b.right - b.left, h = b.bottom - b.top;
    uint8_t *rgba = NULL;
    if (s->seen && w > 0 && h > 0 && (int64_t)w * h <= 1 << 24) {
        gaddr base = qd_pixmap_base(pm, true);
        uint32_t rb = (uint16_t)rd16(pm + 4) & 0x3FFF;
        gaddr ct = depth <= 8 && (rd16(pm + 4) & 0x8000) ? PixMap_pmTable(pm) : 0;
        gaddr t = ct ? rd32(ct) : 0;
        int nct = t ? (int16_t)rd16(t + 6) + 1 : 0;
        rgba = base ? malloc((size_t)w * (size_t)h * 4) : NULL;
        for (int y = 0; rgba && y < h; y++)
            for (int x = 0; x < w; x++) {
                uint8_t *o = rgba + ((size_t)y * (size_t)w + (size_t)x) * 4;
                gaddr row = base + (uint32_t)y * rb;
                uint32_t v;
                if (depth == 32) {
                    v = rd32(row + (uint32_t)x * 4);
                    o[0] = (uint8_t)(v >> 16), o[1] = (uint8_t)(v >> 8), o[2] = (uint8_t)v;
                } else if (depth == 16) {
                    v = rd16(row + (uint32_t)x * 2);
                    o[0] = (uint8_t)((v >> 10 & 31) * 255 / 31);
                    o[1] = (uint8_t)((v >> 5 & 31) * 255 / 31);
                    o[2] = (uint8_t)((v & 31) * 255 / 31);
                } else {
                    uint32_t bit = (uint32_t)x * (uint32_t)depth;
                    v = (uint32_t)(rd8(row + bit / 8) >> (8 - depth - (int)(bit % 8))) & ((1u << depth) - 1);
                    /* By the table's own values (entry i is index i where the table says nothing else). */
                    int e = -1;
                    for (int i = 0; i < nct && e < 0; i++)
                        if ((rd16(t + 8 + (uint32_t)i * 8) & 0xFF) == v || (rd16(t + 4) & 0x8000 && (uint32_t)i == v))
                            e = i;
                    if (e >= 0) {
                        gaddr c = t + 8 + (uint32_t)e * 8 + 2;
                        o[0] = (uint8_t)(rd16(c) >> 8), o[1] = (uint8_t)(rd16(c + 2) >> 8), o[2] = (uint8_t)(rd16(c + 4) >> 8);
                    } else { /* no table: 0 white, the rest towards black */
                        uint8_t l = (uint8_t)(255 - v * 255 / ((1u << depth) - 1));
                        o[0] = o[1] = o[2] = l;
                    }
                }
                o[3] = 255;
            }
    }
    survey_image(s, depth > 1 || (rd16(pm + 4) & 0x8000), depth, rgba, w, h);
    free(rgba);
    return noErr;
}

int16_t tb_GetPictInfo(gaddr pic, gaddr info, int16_t verb, int16_t colors, int16_t method, int16_t version)
{
    int16_t err = check_args(verb, colors, method, version);
    if (err)
        return err;
    pict_survey *s = survey_new(verb, colors, method);
    if (!s)
        return -108;
    err = survey_picture(s, pic);
    if (!err && info)
        put_info(s, info, colors);
    survey_free(s);
    return err;
}

int16_t tb_GetPixMapInfo(gaddr pm, gaddr info, int16_t verb, int16_t colors, int16_t method, int16_t version)
{
    int16_t err = check_args(verb, colors, method, version);
    if (err)
        return err;
    pict_survey *s = survey_new(verb, colors, method);
    if (!s)
        return -108;
    err = survey_pixmap(s, pm);
    if (!err && info)
        put_info(s, info, colors);
    survey_free(s);
    return err;
}

/* Surveys kept between NewPictInfo and DisposePictInfo, by an ID that
 * says which. */
enum { MAX_INFOS = 64, ID_TAG = 0x50490000 };
static pict_survey *infos[MAX_INFOS];

static pict_survey *info_of(int32_t sid)
{
    uint32_t id = (uint32_t)sid, i = id - ID_TAG;
    return (id & 0xFFFF0000u) == ID_TAG && i < MAX_INFOS ? infos[i] : NULL;
}

int16_t tb_NewPictInfo(gaddr idp, int16_t verb, int16_t colors, int16_t method, int16_t version)
{
    int16_t err = check_args(verb, colors, method, version);
    if (err)
        return err;
    int i = 0;
    while (i < MAX_INFOS && infos[i])
        i++;
    if (i == MAX_INFOS || !(infos[i] = survey_new(verb, colors, method)))
        return -108;
    if (idp)
        wr32(idp, ID_TAG + (uint32_t)i);
    return noErr;
}

int16_t tb_RecordPictInfo(int32_t id, gaddr pic)
{
    pict_survey *s = info_of(id);
    return s ? survey_picture(s, pic) : pictInfoIDErr;
}

int16_t tb_RecordPixMapInfo(int32_t id, gaddr pm)
{
    pict_survey *s = info_of(id);
    return s ? survey_pixmap(s, pm) : pictInfoIDErr;
}

int16_t tb_RetrievePictInfo(int32_t id, gaddr info, int16_t colors)
{
    pict_survey *s = info_of(id);
    if (!s)
        return pictInfoIDErr;
    if ((s->verb & (returnColorTable | returnPalette)) && (colors < 1 || colors > 256))
        return colorsRequestedErr;
    if (info)
        put_info(s, info, colors);
    return noErr;
}

int16_t tb_DisposePictInfo(int32_t id)
{
    pict_survey *s = info_of(id);
    if (!s)
        return pictInfoIDErr;
    survey_free(s);
    infos[(uint32_t)id - ID_TAG] = NULL;
    return noErr;
}

/* A new launch starts with none. */
void pictutil_reset(void)
{
    for (int i = 0; i < MAX_INFOS; i++) {
        survey_free(infos[i]);
        infos[i] = NULL;
    }
}
