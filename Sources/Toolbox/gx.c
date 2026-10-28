/*
 * QuickDraw GX, the part programs of the time used for printing: a
 * graphics client, shapes (empty, full, rectangles, and pictures, either
 * of parts or converted from a QuickDraw picture), their mappings and view
 * ports, the printing architecture's jobs, formats and dialogs, and the
 * Collection Manager that jobs keep their settings in.
 *
 * Shapes are drawn with QuickDraw: rectangles framed or filled, converted
 * pictures with DrawPicture, through the scale and translation of the
 * shape's mapping (rotation and skew are not drawn).  So a GX page prints
 * through the Printing Manager's port, recorded and rendered to PDF as any
 * other printing is, and a window view port shows the shape in its window.
 *
 * Objects are numbers of ours, not guest memory: programs only hold and
 * pass them.  Graphics calls are C calls (the trap glue reads them in
 * place); printing and collection calls are Pascal.
 *
 * gxRectangle: left, top, right, bottom (Fixed).  gxMapping: 3x3, rows of
 * Fixed, Fixed, fract; x' = x*m[0][0] + y*m[1][0] + m[2][0], y' likewise.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "qd_internal.h"
#include "tb_internal.h"

enum { gxEmptyType = 1, gxPointType, gxLineType, gxCurveType, gxRectangleType, gxPolygonType, gxPathType,
       gxBitmapType, gxTextType, gxGlyphType, gxLayoutType, gxFullType, gxPictureType };
enum { gxNoFill = 0, gxOpenFrameFill = 1, gxFrameFill = 1, gxClosedFrameFill = 2, gxHollowFill = 2,
       gxEvenOddFill = 3, gxSolidFill = 3, gxWindingFill = 4, gxInverseEvenOddFill = 5, gxInverseWindingFill = 6 };
enum { gxCancelSelected = 0, gxOKSelected = 1 };

#define MAX_OBJECTS 512
#define OBJ_TAG 0x47580000u /* 'GX' and the slot */

enum kind { K_FREE, K_SHAPE, K_VIEWPORT, K_JOB, K_FORMAT, K_COLLECTION };

typedef struct {
    int32_t m[9];
} mapping;

typedef struct {
    enum kind kind;
    int owners;             /* shapes: references (GXDisposeShape drops one) */
    /* shapes */
    int32_t type, fill;
    int32_t rect[4];        /* rectangles: left, top, right, bottom */
    mapping map;
    uint8_t *pict;          /* converted pictures: the PICT's bytes */
    uint32_t pict_len;
    qrect pict_dst;         /* where the picture was converted to */
    uint32_t *parts;        /* pictures of parts */
    int nparts;
    uint32_t viewports[4];
    int nviewports;
    /* view ports */
    gaddr window;
    /* jobs */
    gaddr print;            /* the job's THPrint */
    uint32_t format, collection;
    gaddr port;             /* printing: the Printing Manager's port */
    gaddr refcon;
    /* collections: items */
    struct item { int32_t tag, id, attrs; uint32_t len; uint8_t *data; } *items;
    int nitems;
} object;

static object objects[MAX_OBJECTS];

static uint32_t new_object(enum kind k)
{
    for (int i = 1; i < MAX_OBJECTS; i++)
        if (objects[i].kind == K_FREE) {
            memset(&objects[i], 0, sizeof objects[i]);
            objects[i].kind = k;
            return OBJ_TAG | (uint32_t)i;
        }
    return 0;
}

static object *obj(uint32_t ref, enum kind k)
{
    uint32_t i = ref & 0xFFFF;
    if ((ref & 0xFFFF0000u) != OBJ_TAG || i == 0 || i >= MAX_OBJECTS || objects[i].kind != k)
        return NULL;
    return &objects[i];
}

static void free_object(uint32_t ref)
{
    uint32_t i = ref & 0xFFFF;
    if ((ref & 0xFFFF0000u) != OBJ_TAG || i == 0 || i >= MAX_OBJECTS)
        return;
    object *o = &objects[i];
    free(o->pict);
    free(o->parts);
    for (int k = 0; k < o->nitems; k++)
        free(o->items[k].data);
    free(o->items);
    memset(o, 0, sizeof *o);
}

static void translators_reset(void);

void gx_reset(void)
{
    translators_reset();
    for (int i = 1; i < MAX_OBJECTS; i++)
        if (objects[i].kind != K_FREE)
            free_object(OBJ_TAG | (uint32_t)i);
}

static void identity(mapping *m)
{
    memset(m, 0, sizeof *m);
    m->m[0] = m->m[4] = 0x10000;
    m->m[8] = 0x40000000;
}

/* ---------------------------------------------------------------------- */
/* The graphics client */

int32_t tb_GXNewGraphicsClient(gaddr memoryStart, int32_t memoryLength, int32_t attributes) { return 0x47584331; }
void tb_GXDisposeGraphicsClient(int32_t client) {}
int32_t tb_GXGetGraphicsClient(void) { return 0x47584331; }
void tb_GXSetGraphicsClient(int32_t client) {}
void tb_GXEnterGraphics(void) {}
void tb_GXExitGraphics(void) {}

int32_t tb_GXGetGraphicsError(gaddr stickyError)
{
    if (stickyError)
        wr32(stickyError, 0);
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Shapes */

static uint32_t new_shape(int32_t type)
{
    uint32_t ref = new_object(K_SHAPE);
    object *s = obj(ref, K_SHAPE);
    if (!s)
        return 0;
    s->type = type;
    s->owners = 1;
    s->fill = type == gxRectangleType ? gxClosedFrameFill : gxSolidFill;
    identity(&s->map);
    return ref;
}

int32_t tb_GXNewShape(int32_t aType) { return (int32_t)new_shape(aType >= gxEmptyType && aType <= gxPictureType ? aType : gxEmptyType); }

int32_t tb_GXNewRectangle(gaddr data)
{
    uint32_t ref = new_shape(gxRectangleType);
    object *s = obj(ref, K_SHAPE);
    if (s && data)
        for (int i = 0; i < 4; i++)
            s->rect[i] = (int32_t)rd32(data + 4 * (uint32_t)i);
    return (int32_t)ref;
}

int32_t tb_GXNewPicture(int32_t count, gaddr shapes, gaddr styles, gaddr inks, gaddr transforms)
{
    uint32_t ref = new_shape(gxPictureType);
    tb_GXSetPictureParts((int32_t)ref, 1, 0, count, shapes, styles, inks, transforms);
    return (int32_t)ref;
}

static void release(uint32_t ref, int depth)
{
    object *s = obj(ref, K_SHAPE);
    if (!s || --s->owners > 0 || depth > 16)
        return;
    for (int i = 0; i < s->nparts; i++)
        release(s->parts[i], depth + 1);
    free_object(ref);
}

void tb_GXDisposeShape(int32_t target) { release((uint32_t)target, 0); }

int32_t tb_GXGetShapeType(int32_t source)
{
    object *s = obj((uint32_t)source, K_SHAPE);
    return s ? s->type : 0;
}

int32_t tb_GXGetShapeFill(int32_t source)
{
    object *s = obj((uint32_t)source, K_SHAPE);
    return s ? s->fill : 0;
}

void tb_GXSetShapeFill(int32_t target, int32_t newFill)
{
    object *s = obj((uint32_t)target, K_SHAPE);
    if (s)
        s->fill = newFill;
}

void tb_GXSetShapePen(int32_t target, int32_t pen) {}
void tb_GXSetShapeInk(int32_t target, int32_t newInk) {}
void tb_GXSetShapeColor(int32_t target, gaddr data) {}

gaddr tb_GXGetShapeMapping(int32_t source, gaddr map)
{
    object *s = obj((uint32_t)source, K_SHAPE);
    mapping m;
    identity(&m);
    if (s)
        m = s->map;
    if (map)
        for (int i = 0; i < 9; i++)
            wr32(map + 4 * (uint32_t)i, (uint32_t)m.m[i]);
    return map;
}

void tb_GXSetShapeMapping(int32_t target, gaddr map)
{
    object *s = obj((uint32_t)target, K_SHAPE);
    if (s && map)
        for (int i = 0; i < 9; i++)
            s->map.m[i] = (int32_t)rd32(map + 4 * (uint32_t)i);
}

static int32_t fixmul(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 16); }

/* The shape's mapping followed by another (the 2x2 part and translation). */
static void concat(mapping *m, const mapping *by)
{
    mapping r = *m;
    for (int row = 0; row < 3; row++) {
        int32_t x = m->m[row * 3], y = m->m[row * 3 + 1];
        r.m[row * 3] = fixmul(x, by->m[0]) + fixmul(y, by->m[3]) + (row == 2 ? by->m[6] : 0);
        r.m[row * 3 + 1] = fixmul(x, by->m[1]) + fixmul(y, by->m[4]) + (row == 2 ? by->m[7] : 0);
    }
    *m = r;
}

void tb_GXMapShape(int32_t target, gaddr map)
{
    object *s = obj((uint32_t)target, K_SHAPE);
    if (!s || !map)
        return;
    mapping by;
    for (int i = 0; i < 9; i++)
        by.m[i] = (int32_t)rd32(map + 4 * (uint32_t)i);
    concat(&s->map, &by);
}

void tb_GXMoveShape(int32_t target, int32_t deltaX, int32_t deltaY)
{
    object *s = obj((uint32_t)target, K_SHAPE);
    if (s) {
        s->map.m[6] += deltaX;
        s->map.m[7] += deltaY;
    }
}

void tb_GXRotateShape(int32_t target, int32_t degrees, int32_t xOffset, int32_t yOffset)
{
    /* Kept in the mapping for GXGetShapeMapping; drawn without it. */
    object *s = obj((uint32_t)target, K_SHAPE);
    if (!s || !degrees)
        return;
    double a = degrees / 65536.0 * 3.14159265358979 / 180.0;
    mapping r;
    identity(&r);
    r.m[0] = r.m[4] = (int32_t)(65536 * cos(a));
    r.m[1] = (int32_t)(65536 * sin(a));
    r.m[3] = -r.m[1];
    tb_GXMoveShape(target, -xOffset, -yOffset);
    concat(&s->map, &r);
    tb_GXMoveShape(target, xOffset, yOffset);
    LOGF(LOG_UNIMPL, "QuickDraw GX: shapes are drawn without their rotation\n");
}

void tb_GXSetShapeBounds(int32_t target, gaddr newBounds)
{
    object *s = obj((uint32_t)target, K_SHAPE);
    if (s && newBounds && s->type == gxRectangleType)
        for (int i = 0; i < 4; i++)
            s->rect[i] = (int32_t)rd32(newBounds + 4 * (uint32_t)i);
}

/* A shape's bounds, mapped (pictures: the area their parts cover). */
static void bounds_of(const object *s, int32_t b[4])
{
    b[0] = b[1] = b[2] = b[3] = 0;
    if (s->type == gxRectangleType) {
        memcpy(b, s->rect, sizeof s->rect);
    } else if (s->type == gxPictureType && s->pict) {
        b[0] = s->pict_dst.left << 16;
        b[1] = s->pict_dst.top << 16;
        b[2] = s->pict_dst.right << 16;
        b[3] = s->pict_dst.bottom << 16;
    }
    for (int i = 0; s->type == gxPictureType && i < s->nparts; i++) {
        object *p = obj(s->parts[i], K_SHAPE);
        int32_t pb[4];
        if (!p)
            continue;
        bounds_of(p, pb);
        if (b[2] <= b[0] || b[3] <= b[1]) {
            memcpy(b, pb, sizeof pb);
        } else {
            b[0] = pb[0] < b[0] ? pb[0] : b[0];
            b[1] = pb[1] < b[1] ? pb[1] : b[1];
            b[2] = pb[2] > b[2] ? pb[2] : b[2];
            b[3] = pb[3] > b[3] ? pb[3] : b[3];
        }
    }
    int32_t x0 = fixmul(b[0], s->map.m[0]) + s->map.m[6], y0 = fixmul(b[1], s->map.m[4]) + s->map.m[7];
    int32_t x1 = fixmul(b[2], s->map.m[0]) + s->map.m[6], y1 = fixmul(b[3], s->map.m[4]) + s->map.m[7];
    b[0] = x0 < x1 ? x0 : x1;
    b[2] = x0 < x1 ? x1 : x0;
    b[1] = y0 < y1 ? y0 : y1;
    b[3] = y0 < y1 ? y1 : y0;
}

gaddr tb_GXGetShapeBounds(int32_t source, int32_t index, gaddr bounds)
{
    object *s = obj((uint32_t)source, K_SHAPE);
    int32_t b[4] = { 0 };
    if (s)
        bounds_of(s, b);
    if (bounds)
        for (int i = 0; i < 4; i++)
            wr32(bounds + 4 * (uint32_t)i, (uint32_t)b[i]);
    return bounds;
}

void tb_GXSetPictureParts(int32_t target, int32_t index, int32_t oldCount, int32_t newCount, gaddr shapes,
                          gaddr styles, gaddr inks, gaddr transforms)
{
    object *s = obj((uint32_t)target, K_SHAPE);
    if (s && index == 0) /* index 0: after the last part */
        index = s->nparts + 1;
    if (!s || index < 1 || oldCount < 0 || newCount < 0 || index - 1 + oldCount > s->nparts || newCount > 4096)
        return;
    int keep_after = s->nparts - (index - 1 + oldCount);
    uint32_t *p = malloc(sizeof *p * (size_t)(index - 1 + newCount + keep_after + 1));
    if (!p)
        return;
    memcpy(p, s->parts, sizeof *p * (size_t)(index - 1));
    for (int i = 0; i < newCount; i++) { /* the picture holds a reference to each part */
        p[index - 1 + i] = shapes ? rd32(shapes + 4 * (uint32_t)i) : 0;
        object *part = obj(p[index - 1 + i], K_SHAPE);
        if (part)
            part->owners++;
    }
    for (int i = 0; i < oldCount; i++)
        release(s->parts[index - 1 + i], 0);
    memcpy(p + index - 1 + newCount, s->parts + index - 1 + oldCount, sizeof *p * (size_t)keep_after);
    free(s->parts);
    s->parts = p;
    s->nparts = index - 1 + newCount + keep_after;
}

/* A QuickDraw picture as a picture shape, at dstRect (or its own frame). */
int32_t tb_GXConvertPICTToShape(gaddr pict, int32_t options, gaddr srcRect, gaddr dstRect, uint32_t styleStretch,
                                int32_t destination, gaddr stats)
{
    if (stats)
        wr32(stats, 0);
    if (!pict || !rd32(pict))
        return 0;
    uint32_t ref = obj((uint32_t)destination, K_SHAPE) ? (uint32_t)destination : new_shape(gxPictureType);
    object *s = obj(ref, K_SHAPE);
    if (!s)
        return 0;
    free(s->pict);
    s->type = gxPictureType;
    s->pict_len = (uint32_t)tb_GetHandleSize(pict);
    s->pict = malloc(s->pict_len ? s->pict_len : 1);
    gget(s->pict, rd32(pict), s->pict_len);
    s->pict_dst = dstRect ? qrect_get(dstRect) : qrect_get(rd32(pict) + 2);
    return (int32_t)ref;
}

/* ---------------------------------------------------------------------- */
/* View ports */

int32_t tb_GXNewViewGroup(void) { return 0x47585647; }
int32_t tb_GXNewViewPort(int32_t group) { return (int32_t)new_object(K_VIEWPORT); }

int32_t tb_GXNewWindowViewPort(gaddr qdWindow)
{
    uint32_t ref = new_object(K_VIEWPORT);
    object *v = obj(ref, K_VIEWPORT);
    if (v)
        v->window = qdWindow;
    return (int32_t)ref;
}

void tb_GXDisposeViewPort(int32_t target)
{
    if (obj((uint32_t)target, K_VIEWPORT))
        free_object((uint32_t)target);
}

void tb_GXSetViewPortClip(int32_t target, int32_t clip) {}
void tb_GXSetViewPortParent(int32_t target, int32_t parent) {}
int32_t tb_GXGetViewPortAttributes(int32_t source) { return 0; }

void tb_GXSetShapeViewPorts(int32_t target, int32_t count, gaddr list)
{
    object *s = obj((uint32_t)target, K_SHAPE);
    if (!s)
        return;
    s->nviewports = 0;
    for (int32_t i = 0; i < count && i < 4 && list; i++)
        s->viewports[s->nviewports++] = rd32(list + 4 * (uint32_t)i);
}

int32_t tb_GXGetShapeViewPorts(int32_t source, gaddr list)
{
    object *s = obj((uint32_t)source, K_SHAPE);
    if (!s)
        return 0;
    for (int i = 0; i < s->nviewports && list; i++)
        wr32(list + 4 * (uint32_t)i, s->viewports[i]);
    return s->nviewports;
}

/* ---------------------------------------------------------------------- */
/* Drawing, with QuickDraw in the current port */

static int16_t to_px(int32_t f) { return (int16_t)((f + 0x8000) >> 16); }

/* Draw a shape under an outer mapping, offset by (dx, dy) pixels. */
static void draw_shape(const object *s, const mapping *outer, int dx, int dy, int depth)
{
    if (depth > 16)
        return;
    mapping m = s->map;
    concat(&m, outer);
    gaddr r = tb_NewPtr(8, false, true);
    if (s->type == gxRectangleType || s->type == gxFullType) {
        int32_t x0 = fixmul(s->rect[0], m.m[0]) + m.m[6], y0 = fixmul(s->rect[1], m.m[4]) + m.m[7];
        int32_t x1 = fixmul(s->rect[2], m.m[0]) + m.m[6], y1 = fixmul(s->rect[3], m.m[4]) + m.m[7];
        qrect q = { (int16_t)(to_px(y0) + dy), (int16_t)(to_px(x0) + dx), (int16_t)(to_px(y1) + dy),
                    (int16_t)(to_px(x1) + dx) };
        qrect_put(r, q);
        if (s->type == gxFullType || s->fill == gxSolidFill || s->fill == gxWindingFill)
            tb_PaintRect(r);
        else if (s->fill != gxNoFill)
            tb_FrameRect(r);
    } else if (s->type == gxPictureType) {
        if (s->pict) {
            gaddr h = tb_NewHandle((int32_t)s->pict_len, false, true);
            if (h) {
                gput(rd32(h), s->pict, s->pict_len);
                qrect d = s->pict_dst;
                double sx = m.m[0] / 65536.0, sy = m.m[4] / 65536.0, tx = m.m[6] / 65536.0, ty = m.m[7] / 65536.0;
                qrect q = { (int16_t)(d.top * sy + ty + dy), (int16_t)(d.left * sx + tx + dx),
                            (int16_t)(d.bottom * sy + ty + dy), (int16_t)(d.right * sx + tx + dx) };
                qrect_put(r, q);
                tb_DrawPicture(h, r);
                tb_DisposeHandle(h);
            }
        }
        for (int i = 0; i < s->nparts; i++) {
            const object *p = obj(s->parts[i], K_SHAPE);
            if (p)
                draw_shape(p, &m, dx, dy, depth + 1);
        }
    }
    tb_DisposePtr(r);
}

void tb_GXDrawShape(int32_t source)
{
    object *s = obj((uint32_t)source, K_SHAPE);
    if (!s)
        return;
    mapping id;
    identity(&id);
    gaddr saved = qd_port();
    for (int i = 0; i < s->nviewports; i++) {
        object *v = obj(s->viewports[i], K_VIEWPORT);
        if (!v || !v->window)
            continue;
        tb_SetPort(v->window);
        draw_shape(s, &id, 0, 0, 0);
    }
    tb_SetPort(saved);
}

/* ---------------------------------------------------------------------- */
/* Collections */

/* A collection is born with one retain; RetainCollection adds one, and
 * ReleaseCollection or DisposeCollection drops one, freeing at none. */
gaddr tb_NewCollection(void)
{
    uint32_t c = new_object(K_COLLECTION);
    object *o = obj(c, K_COLLECTION);
    if (o)
        o->owners = 1;
    return c;
}

void tb_DisposeCollection(gaddr c)
{
    object *o = obj(c, K_COLLECTION);
    if (o && --o->owners <= 0)
        free_object(c);
}

int32_t tb_GetCollectionRetainCount(gaddr c)
{
    object *o = obj(c, K_COLLECTION);
    return o ? o->owners : 0;
}

gaddr tb_RetainCollection(gaddr c)
{
    object *o = obj(c, K_COLLECTION);
    if (o)
        o->owners++;
    return c;
}

int32_t tb_ReleaseCollection(gaddr c)
{
    if (!obj(c, K_COLLECTION))
        return paramErr;
    tb_DisposeCollection(c);
    return noErr;
}

int32_t tb_CountCollectionItems(gaddr c)
{
    object *o = obj(c, K_COLLECTION);
    return o ? o->nitems : 0;
}

enum { collectionItemNotFoundErr = -5752, collectionIndexRangeErr = -5753 };

static struct item *find_item(object *o, int32_t tag, int32_t id, int *index)
{
    for (int i = 0; i < o->nitems; i++)
        if (o->items[i].tag == tag && o->items[i].id == id) {
            if (index)
                *index = i;
            return &o->items[i];
        }
    return NULL;
}

static int16_t set_item(struct item *it, int32_t size, gaddr data)
{
    if (size < 0 || size > (16 << 20))
        return paramErr;
    uint8_t *d = malloc(size ? (size_t)size : 1);
    if (!d)
        return memFullErr;
    if (data && size)
        gget(d, data, (uint32_t)size);
    free(it->data);
    it->data = d;
    it->len = (uint32_t)size;
    return noErr;
}

int16_t tb_AddCollectionItem(gaddr c, int32_t tag, int32_t id, int32_t itemSize, gaddr itemData)
{
    object *o = obj(c, K_COLLECTION);
    if (!o)
        return paramErr;
    struct item *it = find_item(o, tag, id, NULL);
    if (!it) {
        struct item *grown = realloc(o->items, sizeof *grown * (size_t)(o->nitems + 1));
        if (!grown)
            return memFullErr;
        o->items = grown;
        it = &o->items[o->nitems++];
        memset(it, 0, sizeof *it);
        it->tag = tag;
        it->id = id;
    }
    return set_item(it, itemSize, itemData);
}

static void get_data(struct item *it, gaddr itemSize, gaddr itemData)
{
    if (itemData && itemSize) {
        uint32_t n = rd32(itemSize) < it->len ? rd32(itemSize) : it->len;
        gput(itemData, it->data, n);
    }
    if (itemSize)
        wr32(itemSize, it->len);
}

int16_t tb_GetCollectionItem(gaddr c, int32_t tag, int32_t id, gaddr itemSize, gaddr itemData)
{
    object *o = obj(c, K_COLLECTION);
    struct item *it = o ? find_item(o, tag, id, NULL) : NULL;
    if (!it)
        return collectionItemNotFoundErr;
    get_data(it, itemSize, itemData);
    return noErr;
}

int16_t tb_RemoveCollectionItem(gaddr c, int32_t tag, int32_t id)
{
    object *o = obj(c, K_COLLECTION);
    int i;
    struct item *it = o ? find_item(o, tag, id, &i) : NULL;
    if (!it)
        return collectionItemNotFoundErr;
    free(it->data);
    memmove(&o->items[i], &o->items[i + 1], sizeof *it * (size_t)(o->nitems - i - 1));
    o->nitems--;
    return noErr;
}

int16_t tb_GetCollectionItemInfo(gaddr c, int32_t tag, int32_t id, gaddr index, gaddr itemSize, gaddr attributes)
{
    object *o = obj(c, K_COLLECTION);
    int i;
    struct item *it = o ? find_item(o, tag, id, &i) : NULL;
    if (!it)
        return collectionItemNotFoundErr;
    if (index)
        wr32(index, (uint32_t)(i + 1));
    if (itemSize)
        wr32(itemSize, it->len);
    if (attributes)
        wr32(attributes, (uint32_t)it->attrs);
    return noErr;
}

int16_t tb_ReplaceIndexedCollectionItem(gaddr c, int32_t index, int32_t itemSize, gaddr itemData)
{
    object *o = obj(c, K_COLLECTION);
    if (!o || index < 1 || index > o->nitems)
        return collectionIndexRangeErr;
    return set_item(&o->items[index - 1], itemSize, itemData);
}

int16_t tb_GetIndexedCollectionItem(gaddr c, int32_t index, gaddr itemSize, gaddr itemData)
{
    object *o = obj(c, K_COLLECTION);
    if (!o || index < 1 || index > o->nitems)
        return collectionIndexRangeErr;
    get_data(&o->items[index - 1], itemSize, itemData);
    return noErr;
}

int16_t tb_GetIndexedCollectionItemInfo(gaddr c, int32_t index, gaddr tag, gaddr id, gaddr itemSize, gaddr attributes)
{
    object *o = obj(c, K_COLLECTION);
    if (!o || index < 1 || index > o->nitems)
        return collectionIndexRangeErr;
    struct item *it = &o->items[index - 1];
    if (tag)
        wr32(tag, (uint32_t)it->tag);
    if (id)
        wr32(id, (uint32_t)it->id);
    if (itemSize)
        wr32(itemSize, it->len);
    if (attributes)
        wr32(attributes, (uint32_t)it->attrs);
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* Printing: jobs on the Printing Manager */

int16_t tb_GXInitPrinting(void) { return noErr; }
int16_t tb_GXExitPrinting(void) { return noErr; }

int16_t tb_GXNewJob(gaddr aJob)
{
    uint32_t ref = new_object(K_JOB);
    object *j = obj(ref, K_JOB);
    if (aJob)
        wr32(aJob, ref);
    if (!j)
        return memFullErr;
    j->print = tb_NewHandle(120, false, true);
    tb_PrintDefault(j->print);
    j->format = new_object(K_FORMAT);
    j->collection = tb_NewCollection();
    object *f = obj(j->format, K_FORMAT);
    if (f)
        f->print = j->print; /* the format's dimensions are the job's page setup */
    return noErr;
}

int16_t tb_GXDisposeJob(int32_t aJob)
{
    object *j = obj((uint32_t)aJob, K_JOB);
    if (!j)
        return paramErr;
    tb_DisposeHandle(j->print);
    free_object(j->format);
    free_object(j->collection);
    free_object((uint32_t)aJob);
    return noErr;
}

int16_t tb_GXGetJobError(int32_t aJob) { return noErr; }
gaddr tb_GXGetJobCollection(int32_t aJob)
{
    object *j = obj((uint32_t)aJob, K_JOB);
    return j ? j->collection : 0;
}

int32_t tb_GXGetJobFormat(int32_t aJob, int32_t whichFormat)
{
    object *j = obj((uint32_t)aJob, K_JOB);
    return j ? (int32_t)j->format : 0;
}

/* The page (the printable area) and the paper, in points from the paper's corner. */
void tb_GXGetFormatDimensions(int32_t aFormat, gaddr pageSize, gaddr paperSize)
{
    object *f = obj((uint32_t)aFormat, K_FORMAT);
    if (!f || !f->print || !rd32(f->print))
        return;
    gaddr p = rd32(f->print);
    qrect page = qrect_get(p + 8), paper = qrect_get(p + 16);
    int vres = (int16_t)rd16(p + 4), hres = (int16_t)rd16(p + 6);
    double sv = 72.0 / (vres > 0 ? vres : 72), sh = 72.0 / (hres > 0 ? hres : 72);
    int32_t pr[4] = { (int32_t)(-paper.left * sh * 65536), (int32_t)(-paper.top * sv * 65536),
                      (int32_t)((page.right - paper.left) * sh * 65536), (int32_t)((page.bottom - paper.top) * sv * 65536) };
    int32_t pa[4] = { 0, 0, (int32_t)((paper.right - paper.left) * sh * 65536),
                      (int32_t)((paper.bottom - paper.top) * sv * 65536) };
    for (int i = 0; i < 4; i++) {
        if (pageSize)
            wr32(pageSize + 4 * (uint32_t)i, (uint32_t)pr[i]);
        if (paperSize)
            wr32(paperSize + 4 * (uint32_t)i, (uint32_t)pa[i]);
    }
}

/* Page Setup: the host's page layout panel, through the job's print record. */
int32_t tb_GXJobDefaultFormatDialog(int32_t aJob, gaddr anEditMenuRec)
{
    object *j = obj((uint32_t)aJob, K_JOB);
    return j && tb_PrStlDialog(j->print) ? gxOKSelected : gxCancelSelected;
}

/* The print dialog says yes: the host's print panel comes when the job ends. */
int32_t tb_GXJobPrintDialog(int32_t aJob, gaddr anEditMenuRec)
{
    object *j = obj((uint32_t)aJob, K_JOB);
    return j && tb_PrJobDialog(j->print) ? gxOKSelected : gxCancelSelected;
}

void tb_GXGetJobPageRange(int32_t theJob, gaddr firstPage, gaddr lastPage)
{
    if (firstPage)
        wr32(firstPage, 1);
    if (lastPage)
        wr32(lastPage, 0x7FFFFFFF);
}

void tb_GXSelectJobOutputPrinter(int32_t aJob, gaddr printerName) {}

gaddr tb_GXGetJobRefCon(int32_t aJob)
{
    object *j = obj((uint32_t)aJob, K_JOB);
    return j ? j->refcon : 0;
}

void tb_GXSetJobRefCon(int32_t aJob, gaddr refCon)
{
    object *j = obj((uint32_t)aJob, K_JOB);
    if (j)
        j->refcon = refCon;
}

void tb_GXIdleJob(int32_t aJob) {}

/* The output printer: the host's, through its print panel, a PostScript
 * printer as far as the program can tell. */
int32_t tb_GXGetJobPrinter(int32_t aJob) { return obj((uint32_t)aJob, K_JOB) ? 0x47585052 : 0; }

void tb_GXGetPrinterName(int32_t aPrinter, gaddr printerName)
{
    static const char name[] = "Mac Printer";
    if (printerName) {
        wr8(printerName, sizeof name - 1);
        gput(printerName + 1, name, sizeof name - 1);
    }
}

int32_t tb_GXGetPrinterDriverType(int32_t aPrinter) { return 0x706F7374; /* 'post' */ }
void tb_GXInstallApplicationOverride(int32_t aJob, int16_t messageID, gaddr override) {}
int8_t tb_GXUpdateJob(int32_t aJob) { return 0; }

/* A job flattened: 'GXjb' and its print record (the page setup). */
gaddr tb_GXFlattenJobToHdl(int32_t aJob, gaddr aHdl)
{
    object *j = obj((uint32_t)aJob, K_JOB);
    if (!j)
        return aHdl;
    if (!aHdl)
        aHdl = tb_NewHandle(124, false, true);
    else
        tb_SetHandleSize(aHdl, 124);
    if (!aHdl || tb_GetHandleSize(aHdl) < 124)
        return aHdl;
    wr32(rd32(aHdl), 0x47586A62);
    gmemcpy(rd32(aHdl) + 4, rd32(j->print), 120);
    return aHdl;
}

int32_t tb_GXUnflattenJobFromHdl(int32_t aJob, gaddr aHdl)
{
    object *j = obj((uint32_t)aJob, K_JOB);
    if (j && aHdl && rd32(aHdl) && tb_GetHandleSize(aHdl) >= 124 && rd32(rd32(aHdl)) == 0x47586A62) {
        gmemcpy(rd32(j->print), rd32(aHdl) + 4, 120);
        tb_PrValidate(j->print);
    }
    return aJob;
}

void tb_GXStartJob(int32_t theJob, gaddr docName, int32_t pageCount)
{
    object *j = obj((uint32_t)theJob, K_JOB);
    if (!j)
        return;
    tb_PrOpen();
    j->port = tb_PrOpenDoc(j->print, 0, 0);
}

/* A page: its shape drawn in the printing port, from the paper's corner. */
void tb_GXPrintPage(int32_t theJob, int32_t pageNumber, int32_t theFormat, int32_t thePage)
{
    object *j = obj((uint32_t)theJob, K_JOB), *s = obj((uint32_t)thePage, K_SHAPE);
    if (!j || !j->port)
        return;
    gaddr saved = qd_port();
    tb_SetPort(j->port);
    tb_PrOpenPage(j->port, 0);
    if (s) {
        gaddr p = rd32(j->print);
        qrect paper = qrect_get(p + 16);
        mapping id;
        identity(&id);
        draw_shape(s, &id, paper.left, paper.top, 0); /* paper.left/top: the paper's corner, negative */
    }
    tb_PrClosePage(j->port);
    tb_SetPort(saved);
}

void tb_GXFinishJob(int32_t theJob)
{
    object *j = obj((uint32_t)theJob, K_JOB);
    if (!j || !j->port)
        return;
    tb_PrCloseDoc(j->port);
    tb_PrClose();
    j->port = 0;
}

/* ---------------------------------------------------------------------- */
/* QuickDraw translators: QuickDraw drawing in a port, between installing
 * and removing the translator, becomes GX shapes for the spool procedure
 * (programs print this way, collecting the shapes into the page's
 * picture).  The drawing is recorded as a QuickDraw picture (which draws
 * nothing meanwhile, as the translator's port did) and spooled as one
 * picture shape when the translator is removed. */

#define MAX_TRANSLATORS 4

static struct {
    gaddr port, pict, spool, reference;
    qrect dst;
    bool has_dst;
} translators[MAX_TRANSLATORS];

void tb_GXInstallQDTranslator(gaddr port, int32_t options, gaddr srcRect, gaddr dstRect, uint32_t styleStrech,
                              gaddr userFunction, gaddr reference)
{
    if (!port)
        return;
    int i = 0;
    while (i < MAX_TRANSLATORS && translators[i].port && translators[i].port != port)
        i++;
    if (i == MAX_TRANSLATORS || translators[i].port)
        return;
    gaddr saved = qd_port();
    tb_SetPort(port);
    gaddr r = tb_NewPtr(8, false, true);
    if (srcRect)
        gmemcpy(r, srcRect, 8);
    else
        gmemcpy(r, port + GrafPort_portRect_off, 8);
    translators[i].port = port;
    translators[i].pict = tb_OpenPicture(r);
    translators[i].spool = userFunction;
    translators[i].reference = reference;
    translators[i].has_dst = dstRect != 0;
    if (dstRect)
        translators[i].dst = qrect_get(dstRect);
    tb_DisposePtr(r);
    tb_SetPort(saved);
}

int32_t tb_GXRemoveQDTranslator(gaddr port, gaddr statistic)
{
    if (statistic)
        wr32(statistic, 0);
    int i = 0;
    while (i < MAX_TRANSLATORS && translators[i].port != port)
        i++;
    if (i == MAX_TRANSLATORS || !port)
        return 0;
    gaddr saved = qd_port();
    tb_SetPort(port);
    tb_ClosePicture();
    tb_SetPort(saved);
    gaddr pict = translators[i].pict, spool = translators[i].spool, reference = translators[i].reference;
    gaddr dst = 0;
    if (translators[i].has_dst) {
        dst = tb_NewPtr(8, false, true);
        qrect_put(dst, translators[i].dst);
    }
    memset(&translators[i], 0, sizeof translators[i]);
    if (!pict)
        return 0;
    uint32_t shape = (uint32_t)tb_GXConvertPICTToShape(pict, 0, 0, dst, 0, 0, 0);
    if (dst)
        tb_DisposePtr(dst);
    tb_KillPicture(pict);
    if (shape && spool) { /* C: OSErr spool(gxShape toSpool, long refCon) */
        cpu_push32(g_cpu, reference);
        cpu_push32(g_cpu, shape);
        guest_call(spool);
        g_cpu->a[7] += 8;
    }
    release(shape, 0); /* the spool procedure took its own reference, if it kept the shape */
    return 0;
}

static void translators_reset(void) { memset(translators, 0, sizeof translators); }
