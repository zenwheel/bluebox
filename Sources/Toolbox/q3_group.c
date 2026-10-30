/*
 * QuickDraw 3D's groups (QD3DGroup.h): a list of objects (references
 * held) at positions the program walks.  Groups, display groups, ordered
 * display groups (kept in QuickDraw 3D's order: transforms, styles,
 * attribute sets, shaders, geometries, groups, then the rest), I/O proxy
 * display groups (the first object they can draw), light groups and
 * information groups.  Submitted, a display group draws its contents
 * inside a push and pop of the view's state unless it is inline.
 */
#include <stdlib.h>

#include "q3_view.h"
#include "tb_internal.h"

#define DISPLAY Q3T('d', 's', 'p', 'g')
#define ORDERED Q3T('o', 'r', 'd', 'g')
#define IOPROXY Q3T('i', 'o', 'p', 'x')
#define LIGHTS Q3T('l', 'g', 'h', 'g')
#define INFO Q3T('i', 'n', 'f', 'o')

enum {
    kQ3DisplayGroupStateMaskIsDrawn = 1, kQ3DisplayGroupStateMaskIsInline = 2,
    kQ3DisplayGroupStateMaskUseBoundingBox = 4, kQ3DisplayGroupStateMaskUseBoundingSphere = 8,
    kQ3DisplayGroupStateMaskIsPicked = 16, kQ3DisplayGroupStateMaskIsWritten = 32,
};

q3group *q3_group_of(gaddr g)
{
    q3obj *o = q3_get(g);
    return o && q3_type_is(o->type, Q3_GROUP) ? o->data : NULL;
}

static q3pos *pos_of(q3group *gr, gaddr p)
{
    q3pos *x = q3_handle_get(p, Q3H_POSITION);
    for (q3pos *i = gr ? gr->first : NULL; x && i; i = i->next)
        if (i == x)
            return x;
    return NULL;
}

static void unlink_pos(q3group *gr, q3pos *p)
{
    if (p->prev)
        p->prev->next = p->next;
    else
        gr->first = p->next;
    if (p->next)
        p->next->prev = p->prev;
    else
        gr->last = p->prev;
}

static void group_empty(q3group *gr, uint32_t type)
{
    for (q3pos *p = gr ? gr->first : NULL, *n; p; p = n) {
        n = p->next;
        q3obj *o = q3_get(p->obj);
        if (type && !(o && q3_type_is(o->type, type)))
            continue;
        unlink_pos(gr, p);
        q3_release(p->obj);
        q3_handle_free(p->g);
        free(p);
    }
}

static void group_dispose(q3obj *o)
{
    q3group *gr = o->data;
    if (gr) {
        group_empty(gr, 0);
        free(gr);
    }
}

static gaddr insert(q3group *gr, q3pos *before, q3pos *after, gaddr obj)
{
    q3pos *p = calloc(1, sizeof *p);
    if (!p)
        return 0;
    p->g = q3_handle_new(Q3H_POSITION, p);
    if (!p->g) {
        free(p);
        return 0;
    }
    p->obj = q3_ref(obj);
    if (before) {
        p->next = before;
        p->prev = before->prev;
        if (before->prev)
            before->prev->next = p;
        else
            gr->first = p;
        before->prev = p;
    } else if (after) {
        p->prev = after;
        p->next = after->next;
        if (after->next)
            after->next->prev = p;
        else
            gr->last = p;
        after->next = p;
    } else {
        p->prev = gr->last;
        if (gr->last)
            gr->last->next = p;
        else
            gr->first = p;
        gr->last = p;
    }
    return p->g;
}

static int order_rank(gaddr g)
{
    q3obj *o = q3_get(g);
    if (!o)
        return 7;
    static const uint32_t order[] = { Q3_TRANSFORM, Q3_STYLE, Q3_ATTRIBUTE_SET, Q3_SHADER, Q3_GEOMETRY, Q3_GROUP };
    for (int i = 0; i < 6; i++)
        if (q3_type_is(o->type, order[i]))
            return i;
    return 6;
}

/* What a group of type takes. */
static bool accepts(uint32_t type, gaddr obj)
{
    q3obj *o = q3_get(obj);
    if (!o)
        return false;
    if (type == LIGHTS)
        return q3_type_is(o->type, Q3_LIGHT);
    if (type == INFO)
        return q3_type_is(o->type, Q3_STRING) || q3_type_is(o->type, Q3_GROUP);
    if (q3_type_is(type, DISPLAY))
        return o->cls->drawable || q3_type_is(o->type, Q3_GROUP);
    return true;
}

gaddr q3_group_add(gaddr group, gaddr obj)
{
    q3obj *g = q3_get(group);
    q3group *gr = q3_group_of(group);
    if (!gr || !accepts(g->type, obj)) {
        q3_error(kQ3ErrorInvalidObjectForGroup);
        return 0;
    }
    q3pos *before = NULL;
    if (g->type == ORDERED) {
        int r = order_rank(obj);
        for (q3pos *p = gr->first; p; p = p->next)
            if (order_rank(p->obj) > r) {
                before = p;
                break;
            }
    }
    gaddr p = insert(gr, before, NULL, obj);
    q3_edited(g);
    return p;
}

static bool group_duplicate(q3obj *to, const q3obj *from)
{
    q3group *src = from->data, *gr = calloc(1, sizeof *gr);
    if (!gr)
        return false;
    if (!src) {
        to->data = gr;
        return true;
    }
    gr->state = src->state;
    gr->has_bounds = src->has_bounds;
    memcpy(gr->bounds, src->bounds, sizeof gr->bounds);
    to->data = gr;
    for (q3pos *p = src->first; p; p = p->next) {
        gaddr copy = q3_duplicate(p->obj);
        insert(gr, NULL, NULL, copy ? copy : p->obj);
        if (copy)
            q3_release(copy);
    }
    return true;
}

static void group_submit(q3obj *o, q3view *v)
{
    q3group *gr = o->data;
    if (!gr || !q3_type_is(o->type, DISPLAY))
        return; /* plain, light and information groups draw nothing */
    if (!(gr->state & kQ3DisplayGroupStateMaskIsDrawn))
        return;
    bool inl = gr->state & kQ3DisplayGroupStateMaskIsInline;
    if (!inl)
        q3v_push(v);
    for (q3pos *p = gr->first; p; p = p->next) {
        q3v_enter(v, p->g);
        q3v_submit(v, p->obj);
        q3v_leave(v);
        if (o->type == IOPROXY && q3_get(p->obj) && q3_get(p->obj)->cls->drawable)
            break; /* the first it can draw */
    }
    if (!inl)
        q3v_pop(v);
}

q3group *q3_group_data_new(void)
{
    q3group *gr = calloc(1, sizeof *gr);
    if (gr)
        gr->state = kQ3DisplayGroupStateMaskIsDrawn | kQ3DisplayGroupStateMaskIsPicked | kQ3DisplayGroupStateMaskIsWritten |
                    kQ3DisplayGroupStateMaskUseBoundingBox | kQ3DisplayGroupStateMaskUseBoundingSphere;
    return gr;
}

static gaddr new_group(uint32_t type)
{
    q3group *gr = q3_group_data_new();
    q3obj *o = gr ? q3_new(type, gr) : NULL;
    if (!o) {
        free(gr);
        return 0;
    }
    return o->g;
}

void q3_group_classes(void)
{
    const uint32_t types[] = { Q3_GROUP, DISPLAY, ORDERED, IOPROXY, LIGHTS, INFO };
    for (unsigned i = 0; i < sizeof types / sizeof types[0]; i++)
        q3_class_ops(types[i], group_dispose, group_duplicate, group_submit, q3_type_is(types[i], DISPLAY));
    q3_mesh_classes();
}

static void Q3Group_New(ppc_cpu *c) { Q3RETURN(new_group(Q3_GROUP)); }
static void Q3LightGroup_New(ppc_cpu *c) { Q3RETURN(new_group(LIGHTS)); }
static void Q3InfoGroup_New(ppc_cpu *c) { Q3RETURN(new_group(INFO)); }
static void Q3DisplayGroup_New(ppc_cpu *c) { Q3RETURN(new_group(DISPLAY)); }
static void Q3OrderedDisplayGroup_New(ppc_cpu *c) { Q3RETURN(new_group(ORDERED)); }
static void Q3IOProxyDisplayGroup_New(ppc_cpu *c) { Q3RETURN(new_group(IOPROXY)); }

static void Q3Group_GetType(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), Q3_GROUP);
    Q3RETURN(o ? q3_type_below(o->type, Q3_GROUP) : 0);
}

static void Q3DisplayGroup_GetType(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), DISPLAY);
    Q3RETURN(o ? q3_type_below(o->type, DISPLAY) : 0);
}

static void Q3Group_AddObject(ppc_cpu *c) { Q3RETURN(q3_of(Q3R(0), Q3_GROUP) ? q3_group_add(Q3R(0), Q3R(1)) : 0); }

static void add_beside(ppc_cpu *c, bool before)
{
    q3obj *g = q3_of(Q3R(0), Q3_GROUP);
    q3group *gr = g ? g->data : NULL;
    q3pos *p = pos_of(gr, Q3R(1));
    if (!p || !accepts(g->type, Q3R(2))) {
        q3_error(p ? kQ3ErrorInvalidObjectForGroup : kQ3ErrorInvalidPositionForGroup);
        Q3RETURN(0);
        return;
    }
    if (g->type == ORDERED) { /* the order decides */
        Q3RETURN(q3_group_add(g->g, Q3R(2)));
        return;
    }
    gaddr n = insert(gr, before ? p : NULL, before ? NULL : p, Q3R(2));
    q3_edited(g);
    Q3RETURN(n);
}
static void Q3Group_AddObjectBefore(ppc_cpu *c) { add_beside(c, true); }
static void Q3Group_AddObjectAfter(ppc_cpu *c) { add_beside(c, false); }

static void Q3Group_GetPositionObject(ppc_cpu *c)
{
    q3pos *p = pos_of(q3_group_of(Q3R(0)), Q3R(1));
    if (p && Q3R(2))
        wr32(Q3R(2), q3_ref(p->obj));
    if (!p)
        q3_error(kQ3ErrorInvalidPositionForGroup);
    Q3RETURN(p && Q3R(2) ? kQ3Success : kQ3Failure);
}

static void Q3Group_SetPositionObject(ppc_cpu *c)
{
    q3obj *g = q3_of(Q3R(0), Q3_GROUP);
    q3pos *p = pos_of(g ? g->data : NULL, Q3R(1));
    if (!p || !accepts(g->type, Q3R(2))) {
        Q3RETURN(kQ3Failure);
        return;
    }
    q3_assign(&p->obj, Q3R(2));
    q3_edited(g);
    Q3RETURN(kQ3Success);
}

/* The object goes back to the program with the group's reference. */
static void Q3Group_RemovePosition(ppc_cpu *c)
{
    q3obj *g = q3_of(Q3R(0), Q3_GROUP);
    q3group *gr = g ? g->data : NULL;
    q3pos *p = pos_of(gr, Q3R(1));
    if (!p) {
        Q3RETURN(0);
        return;
    }
    unlink_pos(gr, p);
    gaddr obj = p->obj;
    q3_handle_free(p->g);
    free(p);
    q3_edited(g);
    Q3RETURN(obj);
}

static bool type_ok(q3pos *p, uint32_t type)
{
    if (!type)
        return true;
    q3obj *o = q3_get(p->obj);
    return o && q3_type_is(o->type, type);
}

/* The first, last, next or previous position (of type, or holding obj). */
static void walk(ppc_cpu *c, int dir, bool from_first, uint32_t type, gaddr obj, gaddr out)
{
    q3group *gr = q3_group_of(Q3R(0));
    if (!gr || !out) {
        Q3RETURN(kQ3Failure);
        return;
    }
    q3pos *p;
    if (from_first) {
        p = dir > 0 ? gr->first : gr->last;
    } else {
        q3pos *cur = pos_of(gr, rd32(out));
        p = cur ? (dir > 0 ? cur->next : cur->prev) : NULL;
    }
    while (p && (!type_ok(p, type) || (obj && p->obj != obj)))
        p = dir > 0 ? p->next : p->prev;
    wr32(out, p ? p->g : 0);
    Q3RETURN(kQ3Success);
}
static void Q3Group_GetFirstPosition(ppc_cpu *c) { walk(c, 1, true, 0, 0, Q3R(1)); }
static void Q3Group_GetLastPosition(ppc_cpu *c) { walk(c, -1, true, 0, 0, Q3R(1)); }
static void Q3Group_GetNextPosition(ppc_cpu *c) { walk(c, 1, false, 0, 0, Q3R(1)); }
static void Q3Group_GetPreviousPosition(ppc_cpu *c) { walk(c, -1, false, 0, 0, Q3R(1)); }
static void Q3Group_GetFirstPositionOfType(ppc_cpu *c) { walk(c, 1, true, Q3R(1), 0, Q3R(2)); }
static void Q3Group_GetLastPositionOfType(ppc_cpu *c) { walk(c, -1, true, Q3R(1), 0, Q3R(2)); }
static void Q3Group_GetNextPositionOfType(ppc_cpu *c) { walk(c, 1, false, Q3R(1), 0, Q3R(2)); }
static void Q3Group_GetPreviousPositionOfType(ppc_cpu *c) { walk(c, -1, false, Q3R(1), 0, Q3R(2)); }
static void Q3Group_GetFirstObjectPosition(ppc_cpu *c) { walk(c, 1, true, 0, Q3R(1), Q3R(2)); }
static void Q3Group_GetLastObjectPosition(ppc_cpu *c) { walk(c, -1, true, 0, Q3R(1), Q3R(2)); }
static void Q3Group_GetNextObjectPosition(ppc_cpu *c) { walk(c, 1, false, 0, Q3R(1), Q3R(2)); }
static void Q3Group_GetPreviousObjectPosition(ppc_cpu *c) { walk(c, -1, false, 0, Q3R(1), Q3R(2)); }

static void count(ppc_cpu *c, uint32_t type, gaddr out)
{
    q3group *gr = q3_group_of(Q3R(0));
    uint32_t n = 0;
    for (q3pos *p = gr ? gr->first : NULL; p; p = p->next)
        n += type_ok(p, type);
    if (out)
        wr32(out, n);
    Q3RETURN(gr && out ? kQ3Success : kQ3Failure);
}
static void Q3Group_CountObjects(ppc_cpu *c) { count(c, 0, Q3R(1)); }
static void Q3Group_CountObjectsOfType(ppc_cpu *c) { count(c, Q3R(1), Q3R(2)); }

static void Q3Group_EmptyObjects(ppc_cpu *c)
{
    q3obj *g = q3_of(Q3R(0), Q3_GROUP);
    if (g) {
        group_empty(g->data, 0);
        q3_edited(g);
    }
    Q3RETURN(g ? kQ3Success : kQ3Failure);
}

static void Q3Group_EmptyObjectsOfType(ppc_cpu *c)
{
    q3obj *g = q3_of(Q3R(0), Q3_GROUP);
    if (g) {
        group_empty(g->data, Q3R(1) ? Q3R(1) : 0xFFFFFFFF);
        q3_edited(g);
    }
    Q3RETURN(g ? kQ3Success : kQ3Failure);
}

static void Q3DisplayGroup_GetState(ppc_cpu *c)
{
    q3obj *g = q3_of(Q3R(0), DISPLAY);
    if (g && Q3R(1))
        wr32(Q3R(1), ((q3group *)g->data)->state);
    Q3RETURN(g && Q3R(1) ? kQ3Success : kQ3Failure);
}

static void Q3DisplayGroup_SetState(ppc_cpu *c)
{
    q3obj *g = q3_of(Q3R(0), DISPLAY);
    if (g) {
        ((q3group *)g->data)->state = Q3R(1);
        q3_edited(g);
    }
    Q3RETURN(g ? kQ3Success : kQ3Failure);
}

static void Q3DisplayGroup_Submit(ppc_cpu *c)
{
    q3view *v = q3v_of(Q3R(1));
    if (v && q3_of(Q3R(0), DISPLAY))
        q3v_submit(v, Q3R(0));
    Q3RETURN(v ? kQ3Success : kQ3Failure);
}

static void Q3DisplayGroup_SetAndUseBoundingBox(ppc_cpu *c)
{
    q3obj *g = q3_of(Q3R(0), DISPLAY);
    if (g && Q3R(1)) {
        q3group *gr = g->data;
        for (int k = 0; k < 7; k++)
            gr->bounds[k] = rdf(Q3R(1) + 4 * (uint32_t)k);
        gr->has_bounds = true;
        gr->state |= kQ3DisplayGroupStateMaskUseBoundingBox;
    }
    Q3RETURN(g && Q3R(1) ? kQ3Success : kQ3Failure);
}

static void Q3DisplayGroup_GetBoundingBox(ppc_cpu *c)
{
    q3obj *g = q3_of(Q3R(0), DISPLAY);
    q3group *gr = g ? g->data : NULL;
    if (!gr || !gr->has_bounds || !Q3R(1)) {
        Q3RETURN(kQ3Failure);
        return;
    }
    for (int k = 0; k < 7; k++)
        wrf(Q3R(1) + 4 * (uint32_t)k, gr->bounds[k]);
    wr32(Q3R(1) + 24, gr->bounds[6] != 0);
    Q3RETURN(kQ3Success);
}

static void Q3DisplayGroup_RemoveBoundingBox(ppc_cpu *c)
{
    q3obj *g = q3_of(Q3R(0), DISPLAY);
    if (g)
        ((q3group *)g->data)->has_bounds = false;
    Q3RETURN(g ? kQ3Success : kQ3Failure);
}

/* Bound the group with the view's bounding loop, as the program would. */
static void Q3DisplayGroup_CalcAndUseBoundingBox(ppc_cpu *c)
{
    q3obj *g = q3_of(Q3R(0), DISPLAY);
    q3view *v = q3v_of(Q3R(2));
    if (!g || !v) {
        Q3RETURN(kQ3Failure);
        return;
    }
    q3group *gr = g->data;
    q3v_bound(v, g->g, Q3R(1) == 0, gr->bounds);
    gr->has_bounds = true;
    gr->state |= kQ3DisplayGroupStateMaskUseBoundingBox;
    Q3RETURN(kQ3Success);
}

static void Q3XGroup_GetPositionPrivate(ppc_cpu *c) { Q3RETURN(0); }

#define E(n) { #n, n }
const ppc_export q3_group_exports[] = {
    E(Q3Group_New), E(Q3Group_GetType), E(Q3Group_AddObject), E(Q3Group_AddObjectBefore), E(Q3Group_AddObjectAfter),
    E(Q3Group_GetPositionObject), E(Q3Group_SetPositionObject), E(Q3Group_RemovePosition),
    E(Q3Group_GetFirstPosition), E(Q3Group_GetLastPosition), E(Q3Group_GetNextPosition),
    E(Q3Group_GetPreviousPosition), E(Q3Group_CountObjects), E(Q3Group_EmptyObjects),
    E(Q3Group_GetFirstPositionOfType), E(Q3Group_GetLastPositionOfType), E(Q3Group_GetNextPositionOfType),
    E(Q3Group_GetPreviousPositionOfType), E(Q3Group_CountObjectsOfType), E(Q3Group_EmptyObjectsOfType),
    E(Q3Group_GetFirstObjectPosition), E(Q3Group_GetLastObjectPosition), E(Q3Group_GetNextObjectPosition),
    E(Q3Group_GetPreviousObjectPosition), E(Q3LightGroup_New), E(Q3InfoGroup_New), E(Q3DisplayGroup_New),
    E(Q3DisplayGroup_GetType), E(Q3DisplayGroup_GetState), E(Q3DisplayGroup_SetState), E(Q3DisplayGroup_Submit),
    E(Q3DisplayGroup_SetAndUseBoundingBox), E(Q3DisplayGroup_GetBoundingBox), E(Q3DisplayGroup_RemoveBoundingBox),
    E(Q3DisplayGroup_CalcAndUseBoundingBox), E(Q3OrderedDisplayGroup_New), E(Q3IOProxyDisplayGroup_New),
    E(Q3XGroup_GetPositionPrivate),
};
const unsigned q3_group_export_count = sizeof q3_group_exports / sizeof q3_group_exports[0];
