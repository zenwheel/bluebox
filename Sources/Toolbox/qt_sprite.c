/*
 * Sprite tracks (QuickTime 2.1 and later): pictures moved about over a
 * background colour, drawn natively for qt_native.c.
 *
 * Every sample is a QT atom container: a 12-byte header (10 reserved bytes
 * and a lock count), then the root atom 'sean'.  A QT atom's header is its
 * size (header included), type, ID, 2 reserved bytes, the count of its
 * children and 4 reserved bytes; a leaf (no children) holds data instead.
 *
 * A key frame holds the shared data, 'dflt' 1 > 'imct' 1 > 'imag' 1..n,
 * each image's 'imda' leaf an ImageDescription followed by the compressed
 * image (decoded with our video codecs) and optionally 'imrg' (its
 * registration point, a FixedPoint).  Beside it, one 'sprt' atom per sprite
 * (IDs from 1) whose children are the properties, each of atom ID 1:
 *     1    matrix (MatrixRecord: a b u / c d v / tx ty w, Fixed and Fract)
 *     4    visible (short)
 *     5    layer (short; lower layers are in front, 32767 the background)
 *     6    graphics mode (long mode, then the RGBColor opColor)
 *     100  image index (short: the 'imag' atom's ID)
 * Override samples (frame differences) hold 'sprt' atoms with only the
 * properties that change.  By default (kKeyFrameAndSingleOverride) each
 * override applies to the key frame alone; with kKeyFrameAndAllOverrides
 * they add up.
 *
 * Track properties come from the media's property atom container, which we
 * look for after the sample description's header: 101 background colour
 * (RGBColor, black by default), 103 sample format (long), 106 visible.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "qt_codec.h"
#include "qt_sprite.h"

enum {
    P_MATRIX = 1, P_VISIBLE = 4, P_LAYER = 5, P_MODE = 6, P_IMAGE = 100,
    T_BACKGROUND = 101, T_FORMAT = 103, T_VISIBLE = 106,
    ALL_OVERRIDES = 1 << 2,
};
/* graphics modes */
enum { srcCopy = 0, blend = 32, addPin = 33, addOver = 34, subPin = 35, transparent = 36, addMax = 37, subOver = 38, adMin = 39 };

typedef struct {
    uint32_t id;
    double a, b, c, d, tx, ty;
    bool visible;
    int16_t layer;
    uint32_t image;
    int32_t mode;
    uint16_t op[3];
} sprite;

typedef struct {
    uint32_t id;
    int w, h;
    uint8_t *rgba;
    double rx, ry;  /* registration point */
} image;

struct qt_sprite {
    int w, h;
    uint8_t bg[3];
    bool visible, all_overrides;
    image *img;
    int nimg;
    sprite *key, *cur;
    int nspr;
};

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static double fixed(const uint8_t *p) { return (int32_t)be32(p) / 65536.0; }
static uint32_t four(const char *s) { return (uint32_t)s[0] << 24 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 8 | (uint8_t)s[3]; }

/* ---- QT atoms ---- */

typedef struct {
    uint32_t type, id;
    int children;
    const uint8_t *body, *end; /* children or data */
} qtatom;

static bool atom_at(const uint8_t *p, const uint8_t *end, qtatom *a)
{
    if (end - p < 20)
        return false;
    uint32_t size = be32(p);
    if (size < 20 || size > (uint64_t)(end - p))
        return false;
    a->type = be32(p + 4);
    a->id = be32(p + 8);
    a->children = (int)be16(p + 14);
    a->body = p + 20;
    a->end = p + size;
    return true;
}

/* The container's root: after the 12-byte header, or at the start; failing
 * both, the bytes after the header taken as the root's children. */
static bool root_of(const uint8_t *data, uint32_t len, qtatom *r)
{
    const uint8_t *end = data + len;
    if (len >= 32 && be32(data + 16) == four("sean") && atom_at(data + 12, end, r))
        return true;
    if (len >= 20 && be32(data + 4) == four("sean") && atom_at(data, end, r))
        return true;
    if (len < 12)
        return false;
    *r = (qtatom){ four("sean"), 1, 1, data + 12, end };
    return true;
}

/* The next child of parent after *it (it->body NULL: the first). */
static bool next_child(const qtatom *parent, qtatom *it)
{
    if (!parent->children)
        return false;
    const uint8_t *p = it->body ? it->end : parent->body;
    return atom_at(p, parent->end, it);
}

static bool find_child(const qtatom *parent, uint32_t type, uint32_t id, qtatom *out)
{
    qtatom it = { 0 };
    while (next_child(parent, &it))
        if (it.type == type && (!id || it.id == id)) {
            *out = it;
            return true;
        }
    return false;
}

static uint32_t leaf_len(const qtatom *a) { return a->children ? 0 : (uint32_t)(a->end - a->body); }

/* ---- Track ---- */

static void track_properties(qt_sprite *s, const qtatom *root)
{
    qtatom a;
    if (find_child(root, T_BACKGROUND, 0, &a) && leaf_len(&a) >= 6)
        for (int k = 0; k < 3; k++)
            s->bg[k] = a.body[k * 2];
    if (find_child(root, T_FORMAT, 0, &a) && leaf_len(&a) >= 2)
        s->all_overrides = (leaf_len(&a) >= 4 ? be32(a.body) : be16(a.body)) & ALL_OVERRIDES;
    if (find_child(root, T_VISIBLE, 0, &a) && leaf_len(&a) >= 1)
        s->visible = leaf_len(&a) >= 2 ? be16(a.body) != 0 : a.body[0] != 0;
}

qt_sprite *qt_sprite_new(const uint8_t *desc, uint32_t len, int w, int h)
{
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
        return NULL;
    qt_sprite *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->w = w;
    s->h = h;
    s->visible = true;
    /* SpriteDescription: size, format, reserved, data reference, then
     * version 16, decompressor 20 and the sample flags 24, how overrides
     * apply (a track property may say so too). */
    if (len >= 28)
        s->all_overrides = (be32(desc + 24) & ALL_OVERRIDES) != 0;
    /* The property container after the header (16 bytes, or 28 with the
     * later SpriteDescription's version, decompressor and flags). */
    for (uint32_t at = 16; at + 12 + 20 <= len; at += 4) {
        if (be32(desc + at + 16) == four("sean") || be32(desc + at + 4) == four("sean")) {
            qtatom r;
            if (root_of(desc + at, len - at, &r))
                track_properties(s, &r);
            break;
        }
    }
    return s;
}

static void free_scene(qt_sprite *s)
{
    for (int i = 0; i < s->nimg; i++)
        free(s->img[i].rgba);
    free(s->img);
    free(s->key);
    free(s->cur);
    s->img = NULL;
    s->key = s->cur = NULL;
    s->nimg = s->nspr = 0;
}

void qt_sprite_free(qt_sprite *s)
{
    if (!s)
        return;
    free_scene(s);
    free(s);
}

bool qt_sprite_all_overrides(const qt_sprite *s) { return s->all_overrides; }

/* An 'imda' leaf: ImageDescription (size, codec, ..., width 32, height 34,
 * depth 82, clutID 84, a colour table after it when clutID is 0), then the
 * compressed picture. */
static bool decode_image(image *im, const uint8_t *d, uint32_t len)
{
    if (len < 86)
        return false;
    uint32_t idsize = be32(d);
    if (idsize < 86 || idsize > len)
        return false;
    qt_video_format f = { be32(d + 4), (int)be16(d + 32), (int)be16(d + 34), (int16_t)be16(d + 82), NULL };
    uint32_t palette[256];
    int bits = f.depth > 32 ? f.depth - 32 : f.depth;
    if ((int16_t)be16(d + 84) == 0 && bits >= 1 && bits <= 8 && f.depth <= 8 && idsize >= 94) {
        const uint8_t *c = d + 86;
        memcpy(palette, qt_standard_palette(f.depth), sizeof(uint32_t) << f.depth);
        int count = (int)be16(c + 6) + 1;
        for (int k = 0; k < count && k < 256 && 94 + (uint32_t)k * 8 + 8 <= idsize; k++) {
            const uint8_t *e = c + 8 + k * 8;
            int v = (int)(be16(c + 4) & 0x8000 ? (uint32_t)k : be16(e)) & 255;
            palette[v] = (uint32_t)e[2] << 16 | (uint32_t)e[4] << 8 | e[6];
        }
        f.palette = palette;
    }
    if (f.width <= 0 || f.height <= 0 || f.width > 4096 || f.height > 4096 || !qt_vdec_supported(f.format, f.depth))
        return false;
    qt_vdec *dec = qt_vdec_new(&f);
    if (!dec)
        return false;
    im->w = f.width;
    im->h = f.height;
    im->rgba = calloc((size_t)f.width * (size_t)f.height, 4);
    bool ok = im->rgba && qt_vdec_decode(dec, d + idsize, len - idsize, im->rgba);
    qt_vdec_free(dec);
    return ok;
}

static void set_properties(sprite *sp, const qtatom *a)
{
    qtatom p = { 0 };
    while (next_child(a, &p)) {
        uint32_t n = leaf_len(&p);
        const uint8_t *d = p.body;
        switch (p.type) {
        case P_MATRIX:
            if (n >= 36) {
                sp->a = fixed(d);
                sp->b = fixed(d + 4);
                sp->c = fixed(d + 12);
                sp->d = fixed(d + 16);
                sp->tx = fixed(d + 24);
                sp->ty = fixed(d + 28);
            }
            break;
        case P_VISIBLE:
            if (n >= 1)
                sp->visible = n >= 2 ? be16(d) != 0 : d[0] != 0;
            break;
        case P_LAYER:
            if (n >= 2)
                sp->layer = (int16_t)be16(d);
            break;
        case P_MODE:
            if (n >= 4)
                sp->mode = (int32_t)be32(d);
            if (n >= 10)
                for (int k = 0; k < 3; k++)
                    sp->op[k] = (uint16_t)be16(d + 4 + k * 2);
            break;
        case P_IMAGE:
            if (n >= 2)
                sp->image = n >= 4 && n != 2 ? be32(d) : be16(d);
            break;
        }
    }
}

bool qt_sprite_key(qt_sprite *s, const uint8_t *data, uint32_t len)
{
    qtatom root, dflt, imct;
    if (!root_of(data, len, &root) || !find_child(&root, four("dflt"), 0, &dflt) ||
        !find_child(&dflt, four("imct"), 0, &imct))
        return false;
    free_scene(s);
    track_properties(s, &root);
    qtatom it = { 0 };
    while (next_child(&imct, &it)) {
        qtatom imda, reg;
        if (it.type != four("imag") || !find_child(&it, four("imda"), 0, &imda))
            continue;
        image *grown = realloc(s->img, (size_t)(s->nimg + 1) * sizeof *grown);
        if (!grown)
            break;
        s->img = grown;
        image *im = &s->img[s->nimg];
        memset(im, 0, sizeof *im);
        im->id = it.id;
        if (find_child(&it, four("imrg"), 0, &reg) && leaf_len(&reg) >= 8) {
            im->rx = fixed(reg.body);
            im->ry = fixed(reg.body + 4);
        }
        if (decode_image(im, imda.body, leaf_len(&imda)))
            s->nimg++;
        else
            free(im->rgba);
    }
    it = (qtatom){ 0 };
    while (next_child(&root, &it)) {
        if (it.type != four("sprt"))
            continue;
        sprite *grown = realloc(s->key, (size_t)(s->nspr + 1) * sizeof *grown);
        if (!grown)
            break;
        s->key = grown;
        sprite *sp = &s->key[s->nspr++];
        *sp = (sprite){ it.id, 1, 0, 0, 1, 0, 0, false, 0, 1, 64, { 0, 0, 0 } };
        set_properties(sp, &it);
    }
    s->cur = malloc((size_t)(s->nspr ? s->nspr : 1) * sizeof *s->cur);
    qt_sprite_reset(s);
    return true;
}

void qt_sprite_reset(qt_sprite *s)
{
    if (s->cur && s->nspr)
        memcpy(s->cur, s->key, (size_t)s->nspr * sizeof *s->cur);
}

void qt_sprite_override(qt_sprite *s, const uint8_t *data, uint32_t len)
{
    qtatom root;
    if (!s->cur || !root_of(data, len, &root))
        return;
    qtatom it = { 0 };
    while (next_child(&root, &it)) {
        if (it.type != four("sprt"))
            continue;
        for (int i = 0; i < s->nspr; i++)
            if (s->cur[i].id == it.id)
                set_properties(&s->cur[i], &it);
    }
}

/* ---- Drawing ---- */

static const image *image_of(const qt_sprite *s, uint32_t id)
{
    for (int i = 0; i < s->nimg; i++)
        if (s->img[i].id == id)
            return &s->img[i];
    return id >= 1 && id <= (uint32_t)s->nimg ? &s->img[id - 1] : NULL;
}

static uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

static void draw_sprite(const qt_sprite *s, const sprite *sp, uint8_t *out)
{
    const image *im = image_of(s, sp->image);
    double det = sp->a * sp->d - sp->b * sp->c;
    if (!im || fabs(det) < 1e-9)
        return;
    /* The image's corners, registration point at the origin, through the
     * matrix: x' = a x + c y + tx, y' = b x + d y + ty. */
    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (int k = 0; k < 4; k++) {
        double x = (k & 1 ? im->w : 0) - im->rx, y = (k & 2 ? im->h : 0) - im->ry;
        double X = sp->a * x + sp->c * y + sp->tx, Y = sp->b * x + sp->d * y + sp->ty;
        x0 = fmin(x0, X);
        x1 = fmax(x1, X);
        y0 = fmin(y0, Y);
        y1 = fmax(y1, Y);
    }
    int c0 = (int)floor(fmax(x0, 0)), c1 = (int)ceil(fmin(x1, s->w));
    int r0 = (int)floor(fmax(y0, 0)), r1 = (int)ceil(fmin(y1, s->h));
    uint8_t key[3] = { (uint8_t)(sp->op[0] >> 8), (uint8_t)(sp->op[1] >> 8), (uint8_t)(sp->op[2] >> 8) };
    for (int row = r0; row < r1; row++)
        for (int col = c0; col < c1; col++) {
            double X = col + 0.5 - sp->tx, Y = row + 0.5 - sp->ty;
            double x = (sp->d * X - sp->c * Y) / det + im->rx, y = (-sp->b * X + sp->a * Y) / det + im->ry;
            if (x < 0 || y < 0 || x >= im->w || y >= im->h)
                continue;
            const uint8_t *src = im->rgba + ((size_t)(int)y * (size_t)im->w + (size_t)(int)x) * 4;
            uint8_t *dst = out + ((size_t)row * (size_t)s->w + (size_t)col) * 4;
            switch (sp->mode) {
            case transparent:
                if (!memcmp(src, key, 3))
                    break;
                memcpy(dst, src, 3);
                break;
            case blend:
                for (int k = 0; k < 3; k++)
                    dst[k] = (uint8_t)((src[k] * (int)key[k] + dst[k] * (255 - (int)key[k])) / 255);
                break;
            case addPin:
            case addOver:
                for (int k = 0; k < 3; k++)
                    dst[k] = sp->mode == addOver ? (uint8_t)(src[k] + dst[k]) : clamp8(src[k] + dst[k]);
                break;
            case subPin:
            case subOver:
                for (int k = 0; k < 3; k++)
                    dst[k] = sp->mode == subOver ? (uint8_t)(dst[k] - src[k]) : clamp8(dst[k] - src[k]);
                break;
            case addMax:
            case adMin:
                for (int k = 0; k < 3; k++)
                    dst[k] = sp->mode == addMax ? (src[k] > dst[k] ? src[k] : dst[k]) : (src[k] < dst[k] ? src[k] : dst[k]);
                break;
            default: /* srcCopy, ditherCopy, the alpha modes (our pictures are opaque) */
                memcpy(dst, src, 3);
                break;
            }
            dst[3] = 255;
        }
}

bool qt_sprite_draw(const qt_sprite *s, uint8_t *rgba)
{
    if (!s->visible)
        return false;
    size_t n = (size_t)s->w * (size_t)s->h;
    for (size_t i = 0; i < n; i++) {
        memcpy(rgba + i * 4, s->bg, 3);
        rgba[i * 4 + 3] = 255;
    }
    if (!s->cur)
        return true;
    /* Back to front: the highest layer first, and within a layer the
     * sprites in order of their IDs. */
    int *order = malloc((size_t)(s->nspr ? s->nspr : 1) * sizeof *order);
    if (!order)
        return true;
    for (int i = 0; i < s->nspr; i++)
        order[i] = i;
    for (int i = 1; i < s->nspr; i++)
        for (int j = i; j > 0; j--) {
            const sprite *a = &s->cur[order[j - 1]], *b = &s->cur[order[j]];
            if (a->layer > b->layer || (a->layer == b->layer && a->id <= b->id))
                break;
            int t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    for (int i = 0; i < s->nspr; i++)
        if (s->cur[order[i]].visible)
            draw_sprite(s, &s->cur[order[i]], rgba);
    free(order);
    return true;
}

/* ---- At run time: the sprite media handler's calls ---- */

int qt_sprite_count(const qt_sprite *s) { return s->cur ? s->nspr : 0; }

uint32_t qt_sprite_id(const qt_sprite *s, int index)
{
    return s->cur && index >= 1 && index <= s->nspr ? s->cur[index - 1].id : 0;
}

int qt_sprite_index(const qt_sprite *s, uint32_t id)
{
    for (int i = 0; s->cur && i < s->nspr; i++)
        if (s->cur[i].id == id)
            return i + 1;
    return 0;
}

static sprite *find_sprite(const qt_sprite *s, uint32_t id)
{
    int i = qt_sprite_index(s, id);
    return i ? &s->cur[i - 1] : NULL;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t to_fixed(double v) { return (uint32_t)(int32_t)lround(v * 65536.0); }

/* A property's value as the atoms hold it (big-endian): the matrix 36
 * bytes, visible and layer and image index 2, the graphics mode 10. */
bool qt_sprite_get(const qt_sprite *s, uint32_t id, uint32_t prop, uint8_t *out, uint32_t *len)
{
    const sprite *sp = find_sprite(s, id);
    if (!sp)
        return false;
    switch (prop) {
    case P_MATRIX:
        memset(out, 0, 36);
        put32(out, to_fixed(sp->a));
        put32(out + 4, to_fixed(sp->b));
        put32(out + 12, to_fixed(sp->c));
        put32(out + 16, to_fixed(sp->d));
        put32(out + 24, to_fixed(sp->tx));
        put32(out + 28, to_fixed(sp->ty));
        put32(out + 32, 0x40000000);
        *len = 36;
        return true;
    case P_VISIBLE:
        out[0] = 0;
        out[1] = sp->visible;
        *len = 2;
        return true;
    case P_LAYER:
        out[0] = (uint8_t)((uint16_t)sp->layer >> 8);
        out[1] = (uint8_t)sp->layer;
        *len = 2;
        return true;
    case P_MODE:
        put32(out, (uint32_t)sp->mode);
        for (int k = 0; k < 3; k++) {
            out[4 + k * 2] = (uint8_t)(sp->op[k] >> 8);
            out[5 + k * 2] = (uint8_t)sp->op[k];
        }
        *len = 10;
        return true;
    case P_IMAGE:
        out[0] = (uint8_t)(sp->image >> 8);
        out[1] = (uint8_t)sp->image;
        *len = 2;
        return true;
    }
    return false;
}

bool qt_sprite_set(qt_sprite *s, uint32_t id, uint32_t prop, const uint8_t *val, uint32_t len)
{
    sprite *sp = find_sprite(s, id);
    if (!sp)
        return false;
    /* as an override sample would: one property atom */
    uint8_t atom[20 + 40];
    if (len > 40)
        return false;
    memset(atom, 0, 20);
    put32(atom, 20 + len);
    put32(atom + 4, prop);
    put32(atom + 8, 1);
    memcpy(atom + 20, val, len);
    qtatom parent = { four("sprt"), id, 1, atom, atom + 20 + len };
    if (prop != P_MATRIX && prop != P_VISIBLE && prop != P_LAYER && prop != P_MODE && prop != P_IMAGE)
        return false;
    set_properties(sp, &parent);
    return true;
}

/* A sprite made at run time (SpriteMediaNewSprite): it lasts as the key
 * frame's do. */
bool qt_sprite_add(qt_sprite *s, uint32_t id, uint32_t image, const double m[6], bool visible, int16_t layer,
                   int32_t mode, const uint16_t op[3])
{
    if (!s->cur || find_sprite(s, id))
        return false;
    sprite sp = { id, m[0], m[1], m[2], m[3], m[4], m[5], visible, layer, image, mode, { op[0], op[1], op[2] } };
    sprite *k = realloc(s->key, (size_t)(s->nspr + 1) * sizeof *k), *c;
    if (!k)
        return false;
    s->key = k;
    c = realloc(s->cur, (size_t)(s->nspr + 1) * sizeof *c);
    if (!c)
        return false;
    s->cur = c;
    s->key[s->nspr] = s->cur[s->nspr] = sp;
    s->nspr++;
    return true;
}

bool qt_sprite_remove(qt_sprite *s, uint32_t id)
{
    int i = qt_sprite_index(s, id);
    if (!i)
        return false;
    memmove(&s->cur[i - 1], &s->cur[i], (size_t)(s->nspr - i) * sizeof *s->cur);
    int k = -1;
    for (int j = 0; j < s->nspr; j++)
        if (s->key[j].id == id)
            k = j;
    if (k >= 0)
        memmove(&s->key[k], &s->key[k + 1], (size_t)(s->nspr - k - 1) * sizeof *s->key);
    s->nspr--;
    return true;
}

int qt_sprite_image_count(const qt_sprite *s) { return s->nimg; }

bool qt_sprite_image_info(const qt_sprite *s, int index, int *w, int *h, double *rx, double *ry)
{
    if (index < 1 || index > s->nimg)
        return false;
    const image *im = &s->img[index - 1];
    *w = im->w;
    *h = im->h;
    *rx = im->rx;
    *ry = im->ry;
    return true;
}

/* Whether a sprite covers a point of the track: its image's bounds there,
 * or (with image set) an opaque pixel of it. */
bool qt_sprite_hits(const qt_sprite *s, uint32_t id, double x, double y, bool image_test, bool invisible_too)
{
    const sprite *sp = find_sprite(s, id);
    if (!sp || (!sp->visible && !invisible_too))
        return false;
    const image *im = image_of(s, sp->image);
    double det = sp->a * sp->d - sp->b * sp->c;
    if (!im || fabs(det) < 1e-9)
        return false;
    double X = x - sp->tx, Y = y - sp->ty;
    double ix = (sp->d * X - sp->c * Y) / det + im->rx, iy = (-sp->b * X + sp->a * Y) / det + im->ry;
    if (ix < 0 || iy < 0 || ix >= im->w || iy >= im->h)
        return false;
    if (!image_test)
        return true;
    const uint8_t *px = im->rgba + ((size_t)(int)iy * (size_t)im->w + (size_t)(int)ix) * 4;
    if (sp->mode == transparent) {
        uint8_t key[3] = { (uint8_t)(sp->op[0] >> 8), (uint8_t)(sp->op[1] >> 8), (uint8_t)(sp->op[2] >> 8) };
        return memcmp(px, key, 3) != 0;
    }
    return px[3] != 0;
}

/* The front-most sprite at a point (the lowest layer, then the highest ID). */
uint32_t qt_sprite_hit(const qt_sprite *s, double x, double y, bool image_test, bool invisible_too)
{
    uint32_t best = 0;
    int16_t best_layer = 0;
    for (int i = 0; s->cur && i < s->nspr; i++) {
        const sprite *sp = &s->cur[i];
        if (!qt_sprite_hits(s, sp->id, x, y, image_test, invisible_too))
            continue;
        if (!best || sp->layer < best_layer || (sp->layer == best_layer && sp->id > best)) {
            best = sp->id;
            best_layer = sp->layer;
        }
    }
    return best;
}

bool qt_sprite_track_visible(const qt_sprite *s) { return s->visible; }
void qt_sprite_set_track_visible(qt_sprite *s, bool visible) { s->visible = visible; }

void qt_sprite_background(const qt_sprite *s, uint8_t rgb[3]) { memcpy(rgb, s->bg, 3); }
void qt_sprite_set_background(qt_sprite *s, const uint8_t rgb[3]) { memcpy(s->bg, rgb, 3); }
