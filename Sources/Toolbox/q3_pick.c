/*
 * QuickDraw 3D's picks (QD3DPick.h): window-point, window-rectangle and
 * world-ray picks, and the hits a view's picking loop gives them
 * (q3_view.c tests what is submitted), sorted and limited as asked, with
 * their details: the pick ID, the path of group positions from the
 * submitted object, the object, its local-to-world matrix, the point,
 * distance and normal.  Shape parts (mesh faces, edges and vertices
 * picked) are objects of their own.
 */
#include <stdlib.h>

#include "q3_view.h"
#include "tb_internal.h"

#define PICK_POINT Q3T('p', 'k', 'w', 'p')
#define PICK_RECT Q3T('p', 'k', 'w', 'r')
#define PICK_RAY Q3T('p', 'k', 'r', 'y')

enum {
    kQ3PickDetailMaskPickID = 1, kQ3PickDetailMaskPath = 2, kQ3PickDetailMaskObject = 4,
    kQ3PickDetailMaskLocalToWorldMatrix = 8, kQ3PickDetailMaskXYZ = 16, kQ3PickDetailMaskDistance = 32,
    kQ3PickDetailMaskNormal = 64, kQ3PickDetailMaskShapePart = 128, kQ3PickDetailMaskPickPart = 256,
    kQ3PickDetailMaskUV = 512,
};
enum { kQ3PickSortNone, kQ3PickSortNearToFar, kQ3PickSortFarToNear };

typedef struct {
    uint32_t w[11];
    q3hit *hits;
    uint32_t nhits, cap;
} q3pick;

static q3pick *pick_of(gaddr g)
{
    q3obj *o = q3_get(g);
    return o && q3_type_is(o->type, Q3_PICK) ? o->data : NULL;
}

static void free_hits(q3pick *p)
{
    for (uint32_t i = 0; i < p->nhits; i++) {
        q3_release(p->hits[i].object);
        q3_release(p->hits[i].root);
        free(p->hits[i].positions);
    }
    p->nhits = 0;
}

static void pick_dispose(q3obj *o)
{
    q3pick *p = o->data;
    if (p) {
        free_hits(p);
        free(p->hits);
        free(p);
    }
}

static bool pick_duplicate(q3obj *to, const q3obj *from)
{
    q3pick *p = calloc(1, sizeof *p);
    if (!p)
        return false;
    memcpy(p->w, ((q3pick *)from->data)->w, sizeof p->w);
    to->data = p;
    return true;
}

void q3_pick_classes(void)
{
    q3_class_ops(PICK_POINT, pick_dispose, pick_duplicate, NULL, false);
    q3_class_ops(PICK_RECT, pick_dispose, pick_duplicate, NULL, false);
    q3_class_ops(PICK_RAY, pick_dispose, pick_duplicate, NULL, false);
}

bool q3_pick_query(gaddr g, q3pickq *q)
{
    q3obj *o = q3_get(g);
    q3pick *p = pick_of(g);
    if (!p) {
        q3_error(kQ3ErrorInvalidObject);
        return false;
    }
    memset(q, 0, sizeof *q);
    q->sort = p->w[0];
    q->mask = p->w[1];
    q->max = p->w[2];
    switch (o->type) {
    case PICK_POINT:
        q->kind = Q3PICK_POINT;
        q->pt[0] = q3d_f(p->w, 3);
        q->pt[1] = q3d_f(p->w, 4);
        q->vtol = q3d_f(p->w, 5);
        q->etol = q3d_f(p->w, 6);
        break;
    case PICK_RECT:
        q->kind = Q3PICK_RECT;
        for (int k = 0; k < 4; k++)
            q->rect[k] = q3d_f(p->w, 3 + k);
        break;
    default:
        q->kind = Q3PICK_RAY;
        for (int k = 0; k < 6; k++)
            q->ray[k] = q3d_f(p->w, 3 + k);
        q->vtol = q3d_f(p->w, 9);
        q->etol = q3d_f(p->w, 10);
        break;
    }
    return true;
}

void q3_pick_hit(gaddr g, const q3hit *h)
{
    q3pick *p = pick_of(g);
    if (!p)
        return;
    /* One hit an object (the nearest of its triangles). */
    for (uint32_t i = 0; i < p->nhits; i++)
        if (p->hits[i].object == h->object && p->hits[i].pickid == h->pickid) {
            if (h->distance < p->hits[i].distance) {
                memcpy(p->hits[i].xyz, h->xyz, sizeof h->xyz);
                memcpy(p->hits[i].normal, h->normal, sizeof h->normal);
                p->hits[i].distance = h->distance;
            }
            return;
        }
    if (p->nhits == p->cap) {
        uint32_t cap = p->cap ? p->cap * 2 : 16;
        q3hit *n = realloc(p->hits, cap * sizeof *n);
        if (!n)
            return;
        p->hits = n;
        p->cap = cap;
    }
    q3hit *x = &p->hits[p->nhits];
    *x = *h;
    x->object = h->object ? q3_ref(h->object) : 0;
    x->root = h->root ? q3_ref(h->root) : 0;
    x->positions = NULL;
    if (h->depth) {
        x->positions = malloc(h->depth * sizeof *x->positions);
        if (x->positions)
            memcpy(x->positions, h->positions, h->depth * sizeof *x->positions);
        else
            x->depth = 0;
    }
    p->nhits++;
    /* Sorted as asked; beyond the number to return, the last goes. */
    if (p->w[0] != kQ3PickSortNone)
        for (uint32_t i = p->nhits - 1; i > 0; i--) {
            bool swap = p->w[0] == kQ3PickSortNearToFar ? p->hits[i].distance < p->hits[i - 1].distance
                                                        : p->hits[i].distance > p->hits[i - 1].distance;
            if (!swap)
                break;
            q3hit t = p->hits[i];
            p->hits[i] = p->hits[i - 1];
            p->hits[i - 1] = t;
        }
    if (p->w[2] && p->nhits > p->w[2]) {
        q3hit *drop = &p->hits[--p->nhits];
        q3_release(drop->object);
        q3_release(drop->root);
        free(drop->positions);
    }
}

static gaddr new_pick(uint32_t type, gaddr data, uint32_t words)
{
    q3pick *p = calloc(1, sizeof *p);
    q3obj *o = p ? q3_new(type, p) : NULL;
    if (!o) {
        free(p);
        return 0;
    }
    for (uint32_t i = 0; data && i < words; i++)
        p->w[i] = rd32(data + 4 * i);
    return o->g;
}

static q3pick *pick_arg(ppc_cpu *c, uint32_t type)
{
    q3obj *o = q3_of(Q3R(0), type);
    return o ? o->data : NULL;
}

static void Q3Pick_GetType(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), Q3_PICK);
    Q3RETURN(o ? q3_type_below(o->type, Q3_PICK) : 0);
}

#define PICKW(fn, type, off, n, set)                                                                                 \
    static void fn(ppc_cpu *c)                                                                                       \
    {                                                                                                                \
        q3pick *p = pick_arg(c, type);                                                                               \
        if (p && Q3R(1))                                                                                             \
            for (int i = 0; i < (n); i++) {                                                                          \
                if (set)                                                                                             \
                    p->w[(off) + i] = rd32(Q3R(1) + 4 * (uint32_t)i);                                                \
                else                                                                                                 \
                    wr32(Q3R(1) + 4 * (uint32_t)i, p->w[(off) + i]);                                                 \
            }                                                                                                        \
        Q3RETURN(p && Q3R(1) ? kQ3Success : kQ3Failure);                                                             \
    }

PICKW(Q3Pick_GetData, Q3_PICK, 0, 3, false)
PICKW(Q3Pick_SetData, Q3_PICK, 0, 3, true)

/* The tolerances: in the point and ray picks only. */
static void tolerance(ppc_cpu *c, bool edge, bool set)
{
    q3obj *o = q3_of(Q3R(0), Q3_PICK);
    q3pick *p = o ? o->data : NULL;
    int at = !o ? -1 : o->type == PICK_POINT ? (edge ? 6 : 5) : o->type == PICK_RAY ? (edge ? 10 : 9) : -1;
    if (at < 0) {
        Q3RETURN(kQ3Failure);
        return;
    }
    if (set)
        q3d_setf(p->w, at, q3_farg(c, 0));
    else if (Q3R(1))
        wr32(Q3R(1), p->w[at]);
    Q3RETURN(kQ3Success);
}
static void Q3Pick_GetVertexTolerance(ppc_cpu *c) { tolerance(c, false, false); }
static void Q3Pick_GetEdgeTolerance(ppc_cpu *c) { tolerance(c, true, false); }
static void Q3Pick_SetVertexTolerance(ppc_cpu *c) { tolerance(c, false, true); }
static void Q3Pick_SetEdgeTolerance(ppc_cpu *c) { tolerance(c, true, true); }

static void Q3Pick_GetNumHits(ppc_cpu *c)
{
    q3pick *p = pick_arg(c, Q3_PICK);
    if (p && Q3R(1))
        wr32(Q3R(1), p->nhits);
    Q3RETURN(p && Q3R(1) ? kQ3Success : kQ3Failure);
}

static void Q3Pick_EmptyHitList(ppc_cpu *c)
{
    q3pick *p = pick_arg(c, Q3_PICK);
    if (p)
        free_hits(p);
    Q3RETURN(p ? kQ3Success : kQ3Failure);
}

static uint32_t details(const q3pick *p)
{
    uint32_t have = kQ3PickDetailMaskPickID | kQ3PickDetailMaskPath | kQ3PickDetailMaskObject |
                    kQ3PickDetailMaskLocalToWorldMatrix | kQ3PickDetailMaskXYZ | kQ3PickDetailMaskDistance |
                    kQ3PickDetailMaskNormal;
    return p->w[1] & have;
}

static void Q3Pick_GetPickDetailValidMask(ppc_cpu *c)
{
    q3pick *p = pick_arg(c, Q3_PICK);
    if (!p || Q3R(1) >= p->nhits || !Q3R(2)) {
        Q3RETURN(kQ3Failure);
        return;
    }
    wr32(Q3R(2), details(p));
    Q3RETURN(kQ3Success);
}

static void Q3Pick_GetPickDetailData(ppc_cpu *c)
{
    q3pick *p = pick_arg(c, Q3_PICK);
    uint32_t i = Q3R(1), which = Q3R(2);
    gaddr out = Q3R(3);
    if (!p || i >= p->nhits || !out || !(details(p) & which)) {
        Q3RETURN(kQ3Failure);
        return;
    }
    const q3hit *h = &p->hits[i];
    switch (which) {
    case kQ3PickDetailMaskPickID: wr32(out, h->pickid); break;
    case kQ3PickDetailMaskPath: {
        wr32(out, h->root ? q3_ref(h->root) : 0);
        wr32(out + 4, h->depth);
        gaddr arr = h->depth ? q3_galloc(h->depth * 4) : 0;
        for (uint32_t k = 0; arr && k < h->depth; k++)
            wr32(arr + k * 4, h->positions[k]);
        wr32(out + 8, arr);
        break;
    }
    case kQ3PickDetailMaskObject: wr32(out, h->object ? q3_ref(h->object) : 0); break;
    case kQ3PickDetailMaskLocalToWorldMatrix: q3m_put(out, h->m); break;
    case kQ3PickDetailMaskXYZ:
        for (int k = 0; k < 3; k++)
            wrf(out + 4 * (uint32_t)k, h->xyz[k]);
        break;
    case kQ3PickDetailMaskDistance: wrf(out, h->distance); break;
    case kQ3PickDetailMaskNormal:
        for (int k = 0; k < 3; k++)
            wrf(out + 4 * (uint32_t)k, h->normal[k]);
        break;
    default: Q3RETURN(kQ3Failure); return;
    }
    Q3RETURN(kQ3Success);
}

static void Q3HitPath_EmptyData(ppc_cpu *c)
{
    gaddr hp = Q3R(0);
    if (hp) {
        if (rd32(hp))
            q3_release(rd32(hp));
        q3_gfree(rd32(hp + 8));
        wr32(hp, 0);
        wr32(hp + 4, 0);
        wr32(hp + 8, 0);
    }
    Q3RETURN(hp ? kQ3Success : kQ3Failure);
}

static void Q3WindowPointPick_New(ppc_cpu *c) { Q3RETURN(new_pick(PICK_POINT, Q3R(0), 7)); }
PICKW(Q3WindowPointPick_GetPoint, PICK_POINT, 3, 2, false)
PICKW(Q3WindowPointPick_SetPoint, PICK_POINT, 3, 2, true)
PICKW(Q3WindowPointPick_GetData, PICK_POINT, 0, 7, false)
PICKW(Q3WindowPointPick_SetData, PICK_POINT, 0, 7, true)
static void Q3WindowRectPick_New(ppc_cpu *c) { Q3RETURN(new_pick(PICK_RECT, Q3R(0), 7)); }
PICKW(Q3WindowRectPick_GetRect, PICK_RECT, 3, 4, false)
PICKW(Q3WindowRectPick_SetRect, PICK_RECT, 3, 4, true)
PICKW(Q3WindowRectPick_GetData, PICK_RECT, 0, 7, false)
PICKW(Q3WindowRectPick_SetData, PICK_RECT, 0, 7, true)
static void Q3WorldRayPick_New(ppc_cpu *c) { Q3RETURN(new_pick(PICK_RAY, Q3R(0), 11)); }
PICKW(Q3WorldRayPick_GetRay, PICK_RAY, 3, 6, false)
PICKW(Q3WorldRayPick_SetRay, PICK_RAY, 3, 6, true)
PICKW(Q3WorldRayPick_GetData, PICK_RAY, 0, 11, false)
PICKW(Q3WorldRayPick_SetData, PICK_RAY, 0, 11, true)

/* Shape parts: a shape and a mesh part's component, face, edge or
 * vertex (words 0 and 1). */
static void Q3ShapePart_GetType(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), Q3_SHAPE_PART);
    Q3RETURN(o ? q3_type_below(o->type, Q3_SHAPE_PART) : 0);
}

static void Q3MeshPart_GetType(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), Q3T('s', 'p', 'm', 'h'));
    Q3RETURN(o ? q3_type_below(o->type, Q3T('s', 'p', 'm', 'h')) : 0);
}

static void part_word(ppc_cpu *c, uint32_t type, int at, bool ref)
{
    q3obj *o = q3_of(Q3R(0), type);
    const uint32_t *w = o ? o->data : NULL;
    if (w && Q3R(1))
        wr32(Q3R(1), ref && w[at] ? q3_ref(w[at]) : w[at]);
    Q3RETURN(w && Q3R(1) ? kQ3Success : kQ3Failure);
}
static void Q3ShapePart_GetShape(ppc_cpu *c) { part_word(c, Q3_SHAPE_PART, 0, true); }
static void Q3MeshPart_GetComponent(ppc_cpu *c) { part_word(c, Q3T('s', 'p', 'm', 'h'), 2, false); }
static void Q3MeshFacePart_GetFace(ppc_cpu *c) { part_word(c, Q3T('m', 'f', 'a', 'c'), 1, false); }
static void Q3MeshEdgePart_GetEdge(ppc_cpu *c) { part_word(c, Q3T('m', 'e', 'd', 'g'), 1, false); }
static void Q3MeshVertexPart_GetVertex(ppc_cpu *c) { part_word(c, Q3T('m', 'v', 't', 'x'), 1, false); }

#define E(n) { #n, n }
const ppc_export q3_pick_exports[] = {
    E(Q3Pick_GetType), E(Q3Pick_GetData), E(Q3Pick_SetData), E(Q3Pick_GetVertexTolerance),
    E(Q3Pick_GetEdgeTolerance), E(Q3Pick_SetVertexTolerance), E(Q3Pick_SetEdgeTolerance), E(Q3Pick_GetNumHits),
    E(Q3Pick_EmptyHitList), E(Q3Pick_GetPickDetailValidMask), E(Q3Pick_GetPickDetailData), E(Q3HitPath_EmptyData),
    E(Q3WindowPointPick_New), E(Q3WindowPointPick_GetPoint), E(Q3WindowPointPick_SetPoint),
    E(Q3WindowPointPick_GetData), E(Q3WindowPointPick_SetData), E(Q3WindowRectPick_New), E(Q3WindowRectPick_GetRect),
    E(Q3WindowRectPick_SetRect), E(Q3WindowRectPick_GetData), E(Q3WindowRectPick_SetData), E(Q3WorldRayPick_New),
    E(Q3WorldRayPick_GetRay), E(Q3WorldRayPick_SetRay), E(Q3WorldRayPick_GetData), E(Q3WorldRayPick_SetData),
    E(Q3ShapePart_GetType), E(Q3MeshPart_GetType), E(Q3ShapePart_GetShape), E(Q3MeshPart_GetComponent),
    E(Q3MeshFacePart_GetFace), E(Q3MeshEdgePart_GetEdge), E(Q3MeshVertexPart_GetVertex),
};
const unsigned q3_pick_export_count = sizeof q3_pick_exports / sizeof q3_pick_exports[0];
