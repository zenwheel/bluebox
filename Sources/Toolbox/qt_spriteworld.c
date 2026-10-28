/*
 * The Sprite Toolbox (QuickTime 2.5): a program's own sprites composited
 * over a background colour or picture in a sprite layer GWorld, and copied
 * to a destination port when the world is idled.
 *
 * Each sprite has an image (an ImageDescription and the program's data,
 * decoded with our codecs), a matrix, a layer (lower in front), visibility
 * and a graphics mode; the world keeps the scene as qt_sprite.c draws it,
 * and what changed is drawn again at the next SpriteWorldIdle unless the
 * program said it would invalidate for itself.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "qd_internal.h"
#include "qt_errors.h"
#include "qt_movie_internal.h"
#include "qt_sprite.h"
#include "tb_internal.h"

enum {
    kSpritePropertyMatrix = 1, kSpritePropertyImageDescription = 2, kSpritePropertyImageDataPtr = 3,
    kSpritePropertyVisible = 4, kSpritePropertyLayer = 5, kSpritePropertyGraphicsMode = 6,
    kSpritePropertyImageDataSize = 7, kSpritePropertyActionHandlingSpriteID = 8,
    kSpritePropertyCanBeHitTested = 9, kSpritePropertyImageIndex = 100,
    kOnlyDrawToSpriteWorld = 1, kSpriteWorldPreflight = 2,
    kSpriteWorldDidDraw = 1, kSpriteWorldNeedsToDraw = 2,
    kScaleSpritesToScaleWorld = 2, kSpriteWorldHighQuality = 4, kSpriteWorldDontAutoInvalidate = 8,
    kSpriteWorldInvisible = 16, kSpriteWorldDirtyInsteadOfFlush = 32,
    kSpriteHitTestBounds = 1, kSpriteHitTestImage = 2, kSpriteHitTestInvisibleSprites = 4,
    kSpriteHitTestLocInDisplayCoordinates = 16,
};

#define MAX_WORLDS 32
#define MAX_SPRITES 1024

typedef struct world world;

typedef struct {
    gaddr token;          /* the Sprite the program holds; 0 free */
    world *w;
    uint32_t id;
    gaddr idh;            /* its ImageDescriptionHandle and data, the program's */
    gaddr data;
    int32_t data_size;    /* 0: the description's */
    bool visible;
    int16_t layer;
    int32_t action_sprite;
    bool hit_testable;
} sprite_rec;

struct world {
    gaddr token;          /* the SpriteWorld; 0 free */
    gaddr dest, layer, background;
    uint16_t bg[3];
    qt_sprite *scene;
    int width, height;    /* the sprite layer's */
    qrect bounds;         /* the sprite layer's port rect */
    int32_t matrix[9];
    gaddr clip;           /* a copy, or 0 */
    int32_t mode;
    uint16_t op[3];
    int32_t flags;
    bool invalid;
    uint32_t next_id;
};

static world worlds[MAX_WORLDS];
static sprite_rec sprites[MAX_SPRITES];

static world *world_of(gaddr token)
{
    for (int i = 0; token && i < MAX_WORLDS; i++)
        if (worlds[i].token == token)
            return &worlds[i];
    return NULL;
}

static sprite_rec *sprite_of(gaddr token)
{
    for (int i = 0; token && i < MAX_SPRITES; i++)
        if (sprites[i].token == token)
            return &sprites[i];
    return NULL;
}

static double fixed(int32_t v) { return v / 65536.0; }

static void read_matrix(gaddr m, int32_t out[9])
{
    if (!m) {
        memset(out, 0, 9 * sizeof *out);
        out[0] = out[4] = 0x10000;
        out[8] = 0x40000000;
        return;
    }
    for (int i = 0; i < 9; i++)
        out[i] = (int32_t)rd32(m + 4 * (uint32_t)i);
}

/* A sprite's image decoded again from its description and data. */
static bool load_image(sprite_rec *sp)
{
    if (!sp->idh || !rd32(sp->idh) || !sp->data)
        return false;
    gaddr d = rd32(sp->idh);
    uint32_t dlen = rd32(d);
    if (dlen < 86 || dlen > 0x100000)
        return false;
    uint32_t size = sp->data_size > 0 ? (uint32_t)sp->data_size : rd32(d + 44);
    if (!size) { /* uncompressed images may leave it out */
        uint32_t w = rd16(d + 32), h = rd16(d + 34), depth = rd16(d + 82);
        size = (w * (depth > 32 ? depth - 32 : depth) + 7) / 8 * h;
    }
    if (size > 0x4000000)
        return false;
    uint8_t *b = malloc(dlen + size);
    if (!b)
        return false;
    gget(b, d, dlen);
    gget(b + dlen, sp->data, size);
    bool ok = qt_sprite_set_image(sp->w->scene, sp->id, b, dlen + size);
    free(b);
    return ok;
}

static void invalidate(world *w)
{
    if (!(w->flags & kSpriteWorldDontAutoInvalidate))
        w->invalid = true;
}

/* The pixels of a port's rect as RGBA (the background GWorld). */
static void port_pixels(gaddr port, qrect r, uint8_t *rgba, int w, int h)
{
    gaddr saved = tb_NewPtr(8, false, true), c = tb_NewPtr(6, false, true);
    tb_GetGWorld(saved, saved + 4);
    tb_SetGWorld(port, 0);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            tb_GetCPixel((int16_t)(r.left + x), (int16_t)(r.top + y), c);
            uint8_t *o = rgba + ((size_t)y * (size_t)w + (size_t)x) * 4;
            o[0] = (uint8_t)(rd16(c) >> 8), o[1] = (uint8_t)(rd16(c + 2) >> 8), o[2] = (uint8_t)(rd16(c + 4) >> 8);
            o[3] = 255;
        }
    tb_SetGWorld(rd32(saved), rd32(saved + 4));
    tb_DisposePtr(saved);
    tb_DisposePtr(c);
}

/* The scene composited into the sprite layer, then copied to the
 * destination through the world's matrix and clip. */
static void draw_world(world *w, bool to_dest)
{
    size_t n = (size_t)w->width * (size_t)w->height;
    uint8_t *rgba = malloc(n * 4);
    if (!rgba)
        return;
    if (w->background) {
        port_pixels(w->background, w->bounds, rgba, w->width, w->height);
    } else {
        for (size_t i = 0; i < n; i++) {
            rgba[i * 4] = (uint8_t)(w->bg[0] >> 8), rgba[i * 4 + 1] = (uint8_t)(w->bg[1] >> 8);
            rgba[i * 4 + 2] = (uint8_t)(w->bg[2] >> 8), rgba[i * 4 + 3] = 255;
        }
    }
    qt_sprite_draw_over(w->scene, rgba);
    gaddr saved = tb_NewPtr(8, false, true);
    tb_GetGWorld(saved, saved + 4);
    tb_SetGWorld(w->layer, 0);
    qd_draw_rgba(rgba, w->width, w->height, w->bounds);
    free(rgba);
    if (to_dest && !(w->flags & kSpriteWorldInvisible) && w->dest) {
        tb_SetGWorld(w->dest, 0);
        gaddr src = tb_NewPtr(8, false, true), dst = tb_NewPtr(8, false, true);
        qrect_put(src, w->bounds);
        double sx = fixed(w->matrix[0]), sy = fixed(w->matrix[4]), tx = fixed(w->matrix[6]), ty = fixed(w->matrix[7]);
        qrect d = { (int16_t)lround(w->bounds.top * sy + ty), (int16_t)lround(w->bounds.left * sx + tx),
                    (int16_t)lround(w->bounds.bottom * sy + ty), (int16_t)lround(w->bounds.right * sx + tx) };
        qrect_put(dst, d);
        gaddr pm = rd32(tb_GetGWorldPixMap(w->layer));
        tb_ForeColor(33);  /* black and white, so CopyBits doesn't colourize */
        tb_BackColor(30);
        tb_CopyBits(pm, w->dest + GrafPort_portBits_off, src, dst, (int16_t)(w->mode & 0xFFFF), w->clip);
        tb_DisposePtr(src);
        tb_DisposePtr(dst);
    }
    tb_SetGWorld(rd32(saved), rd32(saved + 4));
    tb_DisposePtr(saved);
}

/* ---- Worlds ---- */

int16_t tb_NewSpriteWorld(gaddr newSpriteWorld, int32_t destination, int32_t spriteLayer, gaddr backgroundColor,
                          int32_t background)
{
    if (newSpriteWorld)
        wr32(newSpriteWorld, 0);
    if (!newSpriteWorld || !destination || !spriteLayer)
        return qt_set_error(missingRequiredParameterErr);
    world *w = NULL;
    for (int i = 0; i < MAX_WORLDS && !w; i++)
        if (!worlds[i].token)
            w = &worlds[i];
    if (!w)
        return qt_set_error(memFullErr);
    gaddr lpm = rd32(tb_GetGWorldPixMap((gaddr)spriteLayer));
    qrect lb = qrect_get(lpm + 6);
    if (background) {
        gaddr bpm = rd32(tb_GetGWorldPixMap((gaddr)background));
        qrect bb = qrect_get(bpm + 6);
        if (bb.bottom - bb.top != lb.bottom - lb.top || bb.right - bb.left != lb.right - lb.left ||
            rd16(bpm + 32) != rd16(lpm + 32))
            return qt_set_error(gWorldsNotSameDepthAndSizeErr);
    }
    memset(w, 0, sizeof *w);
    w->token = tb_NewPtr(4, false, true);
    w->dest = (gaddr)destination;
    w->layer = (gaddr)spriteLayer;
    w->background = (gaddr)background;
    w->bounds = lb;
    w->width = lb.right - lb.left;
    w->height = lb.bottom - lb.top;
    if (backgroundColor) {
        w->bg[0] = rd16(backgroundColor), w->bg[1] = rd16(backgroundColor + 2), w->bg[2] = rd16(backgroundColor + 4);
    } else {
        w->bg[0] = w->bg[1] = w->bg[2] = 0xFFFF;
    }
    w->scene = qt_sprite_world(w->width, w->height);
    if (!w->scene) {
        tb_DisposePtr(w->token);
        memset(w, 0, sizeof *w);
        return qt_set_error(memFullErr);
    }
    read_matrix(0, w->matrix);
    w->invalid = true;
    w->next_id = 1;
    wr32(newSpriteWorld, w->token);
    return qt_set_error(noErr);
}

void tb_DisposeAllSprites(int32_t theSpriteWorld)
{
    world *w = world_of((gaddr)theSpriteWorld);
    if (!w)
        return;
    for (int i = 0; i < MAX_SPRITES; i++)
        if (sprites[i].token && sprites[i].w == w) {
            qt_sprite_remove(w->scene, sprites[i].id);
            qt_sprite_forget_image(w->scene, sprites[i].id);
            tb_DisposePtr(sprites[i].token);
            memset(&sprites[i], 0, sizeof sprites[i]);
        }
    invalidate(w);
}

void tb_DisposeSpriteWorld(int32_t theSpriteWorld)
{
    world *w = world_of((gaddr)theSpriteWorld);
    if (!w)
        return;
    tb_DisposeAllSprites(theSpriteWorld);
    qt_sprite_free(w->scene);
    if (w->clip)
        tb_DisposeRgn(w->clip);
    tb_DisposePtr(w->token);
    memset(w, 0, sizeof *w);
}

int16_t tb_SetSpriteWorldClip(int32_t theSpriteWorld, int32_t clipRgn)
{
    world *w = world_of((gaddr)theSpriteWorld);
    if (!w)
        return qt_set_error(paramErr);
    if (w->clip)
        tb_DisposeRgn(w->clip);
    w->clip = 0;
    if (clipRgn) {
        w->clip = tb_NewRgn();
        tb_CopyRgn((gaddr)clipRgn, w->clip);
    }
    invalidate(w);
    return qt_set_error(noErr);
}

int16_t tb_SetSpriteWorldMatrix(int32_t theSpriteWorld, gaddr matrix)
{
    world *w = world_of((gaddr)theSpriteWorld);
    if (!w)
        return qt_set_error(paramErr);
    read_matrix(matrix, w->matrix);
    invalidate(w);
    return qt_set_error(noErr);
}

int16_t tb_SetSpriteWorldGraphicsMode(int32_t theSpriteWorld, int32_t mode, gaddr opColor)
{
    world *w = world_of((gaddr)theSpriteWorld);
    if (!w)
        return qt_set_error(paramErr);
    w->mode = mode;
    if (opColor)
        w->op[0] = rd16(opColor), w->op[1] = rd16(opColor + 2), w->op[2] = rd16(opColor + 4);
    invalidate(w);
    return qt_set_error(noErr);
}

int16_t tb_SetSpriteWorldFlags(int32_t spriteWorld, int32_t flags, int32_t flagsMask)
{
    world *w = world_of((gaddr)spriteWorld);
    if (!w)
        return qt_set_error(paramErr);
    w->flags = (w->flags & ~flagsMask) | (flags & flagsMask);
    return qt_set_error(noErr);
}

int16_t tb_InvalidateSpriteWorld(int32_t theSpriteWorld, gaddr invalidArea)
{
    world *w = world_of((gaddr)theSpriteWorld);
    if (!w)
        return qt_set_error(paramErr);
    (void)invalidArea; /* the whole layer is composited again */
    w->invalid = true;
    return qt_set_error(noErr);
}

int16_t tb_SpriteWorldIdle(int32_t theSpriteWorld, int32_t flagsIn, gaddr flagsOut)
{
    world *w = world_of((gaddr)theSpriteWorld);
    if (flagsOut)
        wr32(flagsOut, 0);
    if (!w)
        return qt_set_error(paramErr);
    if (!w->invalid)
        return qt_set_error(noErr);
    if (flagsIn & kSpriteWorldPreflight) {
        if (flagsOut)
            wr32(flagsOut, kSpriteWorldNeedsToDraw);
        return qt_set_error(noErr);
    }
    bool to_dest = !(flagsIn & kOnlyDrawToSpriteWorld);
    draw_world(w, to_dest);
    w->invalid = false;
    if (flagsOut)
        wr32(flagsOut, kSpriteWorldDidDraw | (to_dest ? 0 : kSpriteWorldNeedsToDraw));
    return qt_set_error(noErr);
}

/* A point in the destination (display coordinates) into the sprite layer's. */
static void to_layer(const world *w, int32_t loc, int32_t flags, double *x, double *y)
{
    double h = (int16_t)(loc & 0xFFFF), v = (int16_t)((uint32_t)loc >> 16);
    if (flags & kSpriteHitTestLocInDisplayCoordinates) {
        double sx = fixed(w->matrix[0]), sy = fixed(w->matrix[4]);
        h = (h - fixed(w->matrix[6])) / (sx ? sx : 1);
        v = (v - fixed(w->matrix[7])) / (sy ? sy : 1);
    }
    *x = h - w->bounds.left + 0.5;
    *y = v - w->bounds.top + 0.5;
}

int16_t tb_SpriteWorldHitTest(int32_t theSpriteWorld, int32_t flags, int32_t loc, gaddr spriteHit)
{
    world *w = world_of((gaddr)theSpriteWorld);
    if (spriteHit)
        wr32(spriteHit, 0);
    if (!w || !spriteHit)
        return qt_set_error(paramErr);
    double x, y;
    to_layer(w, loc, flags, &x, &y);
    uint32_t id = qt_sprite_hit(w->scene, x, y, (flags & kSpriteHitTestImage) != 0,
                                (flags & kSpriteHitTestInvisibleSprites) != 0);
    for (int i = 0; id && i < MAX_SPRITES; i++)
        if (sprites[i].token && sprites[i].w == w && sprites[i].id == id && sprites[i].hit_testable)
            wr32(spriteHit, sprites[i].token);
    return qt_set_error(noErr);
}

/* ---- Sprites ---- */

int16_t tb_NewSprite(gaddr newSprite, int32_t itsSpriteWorld, int32_t idh, int32_t imageDataPtr, gaddr matrix,
                     int8_t visible, int16_t layer)
{
    if (newSprite)
        wr32(newSprite, 0);
    world *w = world_of((gaddr)itsSpriteWorld);
    if (!w || !newSprite)
        return qt_set_error(paramErr);
    sprite_rec *sp = NULL;
    for (int i = 0; i < MAX_SPRITES && !sp; i++)
        if (!sprites[i].token)
            sp = &sprites[i];
    if (!sp)
        return qt_set_error(memFullErr);
    *sp = (sprite_rec){ tb_NewPtr(4, false, true), w, w->next_id++, (gaddr)idh, (gaddr)imageDataPtr, 0,
                        visible != 0, layer, 0, true };
    int32_t m[9];
    read_matrix(matrix, m);
    double mm[6] = { fixed(m[0]), fixed(m[1]), fixed(m[3]), fixed(m[4]), fixed(m[6]), fixed(m[7]) };
    uint16_t op[3] = { 0, 0, 0 };
    load_image(sp);
    if (!qt_sprite_add(w->scene, sp->id, sp->id, mm, visible != 0, layer, 0, op)) {
        tb_DisposePtr(sp->token);
        memset(sp, 0, sizeof *sp);
        return qt_set_error(memFullErr);
    }
    invalidate(w);
    wr32(newSprite, sp->token);
    return qt_set_error(noErr);
}

void tb_DisposeSprite(int32_t theSprite)
{
    sprite_rec *sp = sprite_of((gaddr)theSprite);
    if (!sp)
        return;
    qt_sprite_remove(sp->w->scene, sp->id);
    qt_sprite_forget_image(sp->w->scene, sp->id);
    invalidate(sp->w);
    tb_DisposePtr(sp->token);
    memset(sp, 0, sizeof *sp);
}

void tb_InvalidateSprite(int32_t theSprite)
{
    sprite_rec *sp = sprite_of((gaddr)theSprite);
    if (sp)
        sp->w->invalid = true;
}

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v; }

int16_t tb_SetSpriteProperty(int32_t theSprite, int32_t propertyType, gaddr propertyValue)
{
    sprite_rec *sp = sprite_of((gaddr)theSprite);
    if (!sp)
        return qt_set_error(paramErr);
    qt_sprite *s = sp->w->scene;
    uint8_t v[36];
    switch (propertyType) {
    case kSpritePropertyMatrix:
        if (!propertyValue)
            return qt_set_error(paramErr);
        gget(v, propertyValue, 36);
        qt_sprite_set(s, sp->id, 1, v, 36);
        break;
    case kSpritePropertyVisible: /* the values themselves, not pointers to them */
        put16(v, propertyValue ? 1 : 0);
        sp->visible = propertyValue != 0;
        qt_sprite_set(s, sp->id, 4, v, 2);
        break;
    case kSpritePropertyLayer:
        sp->layer = (int16_t)propertyValue;
        put16(v, (uint16_t)sp->layer);
        qt_sprite_set(s, sp->id, 5, v, 2);
        break;
    case kSpritePropertyGraphicsMode: /* a ModifierTrackGraphicsModeRecord: mode, then the RGBColor */
        if (!propertyValue)
            return qt_set_error(paramErr);
        gget(v, propertyValue, 10);
        qt_sprite_set(s, sp->id, 6, v, 10);
        break;
    case kSpritePropertyImageDescription:
        sp->idh = propertyValue;
        load_image(sp);
        break;
    case kSpritePropertyImageDataPtr:
        sp->data = propertyValue;
        load_image(sp);
        break;
    case kSpritePropertyImageDataSize:
        sp->data_size = (int32_t)propertyValue;
        load_image(sp);
        break;
    case kSpritePropertyActionHandlingSpriteID:
        sp->action_sprite = (int32_t)propertyValue;
        return qt_set_error(noErr);
    case kSpritePropertyCanBeHitTested:
        sp->hit_testable = propertyValue != 0;
        return qt_set_error(noErr);
    default:
        return qt_set_error(invalidSpritePropertyErr);
    }
    invalidate(sp->w);
    return qt_set_error(noErr);
}

int16_t tb_GetSpriteProperty(int32_t theSprite, int32_t propertyType, gaddr propertyValue)
{
    sprite_rec *sp = sprite_of((gaddr)theSprite);
    if (!sp || !propertyValue)
        return qt_set_error(paramErr);
    uint8_t v[36];
    uint32_t n;
    switch (propertyType) {
    case kSpritePropertyMatrix:
        if (qt_sprite_get(sp->w->scene, sp->id, 1, v, &n))
            gput(propertyValue, v, 36);
        break;
    case kSpritePropertyVisible: wr8(propertyValue, sp->visible); break;
    case kSpritePropertyLayer: wr16(propertyValue, (uint16_t)sp->layer); break;
    case kSpritePropertyGraphicsMode:
        if (qt_sprite_get(sp->w->scene, sp->id, 6, v, &n))
            gput(propertyValue, v, 10);
        break;
    case kSpritePropertyImageDescription: wr32(propertyValue, sp->idh); break;
    case kSpritePropertyImageDataPtr: wr32(propertyValue, sp->data); break;
    case kSpritePropertyImageDataSize:
        wr32(propertyValue, sp->data_size ? (uint32_t)sp->data_size : sp->idh && rd32(sp->idh) ? rd32(rd32(sp->idh) + 44) : 0);
        break;
    case kSpritePropertyActionHandlingSpriteID: wr32(propertyValue, (uint32_t)sp->action_sprite); break;
    case kSpritePropertyCanBeHitTested: wr8(propertyValue, sp->hit_testable); break;
    default: return qt_set_error(invalidSpritePropertyErr);
    }
    return qt_set_error(noErr);
}

int16_t tb_SpriteHitTest(int32_t theSprite, int32_t flags, int32_t loc, gaddr wasHit)
{
    sprite_rec *sp = sprite_of((gaddr)theSprite);
    if (wasHit)
        wr8(wasHit, 0);
    if (!sp || !wasHit)
        return qt_set_error(paramErr);
    double x, y;
    to_layer(sp->w, loc, flags, &x, &y);
    wr8(wasHit, qt_sprite_hits(sp->w->scene, sp->id, x, y, (flags & kSpriteHitTestImage) != 0,
                               (flags & kSpriteHitTestInvisibleSprites) != 0));
    return qt_set_error(noErr);
}

/* A new run: the old one's worlds go with it. */
void qt_spriteworld_reset(void)
{
    for (int i = 0; i < MAX_WORLDS; i++)
        qt_sprite_free(worlds[i].scene);
    memset(worlds, 0, sizeof worlds);
    memset(sprites, 0, sizeof sprites);
}
