/*
 * QuickDraw 3D's mesh (QD3DGeometry.h's Q3Mesh calls): a polyhedral
 * surface the program edits a vertex and a face at a time.  Vertices have
 * coordinates and attribute sets; faces have contours (the first the
 * outside, the rest holes) of vertices, and attribute sets; a corner (a
 * vertex in a face) may have its own set.  Edges are the faces' contour
 * sides, shared by the two faces either side; components are the
 * connected pieces.  Both are worked out again after the topology
 * changes, edges keeping their attribute sets by their vertices.
 *
 * Vertices, faces, edges, contours and components are handles
 * (q3_handle_new).  An iterator (TQ3MeshIterator) keeps what it walks
 * (var1), the next index (var2), what kind of walk (var3) and the mesh
 * (var4.field1); the list walked is made again at each step.
 */
#include <math.h>
#include <stdlib.h>

#include "q3_view.h"
#include "tb_internal.h"

#define MESH Q3T('m', 'e', 's', 'h')

typedef struct mesh mesh;
typedef struct mface mface;

typedef struct {
    gaddr h;
    mesh *m;
    float p[3];
    gaddr set;
    int comp;
} mvert;

typedef struct {
    gaddr h;
    mface *face;
    uint32_t n;
    mvert **v;
} mcont;

typedef struct {
    mvert *v;
    gaddr set;
} mcorner;

struct mface {
    gaddr h;
    mesh *m;
    uint32_t ncont;
    mcont **c;
    gaddr set;
    mcorner *corners;
    uint32_t ncorners;
};

typedef struct {
    gaddr h;
    mesh *m;
    mvert *a, *b;
    mface *f1, *f2;
    gaddr set;
    bool consistent; /* the two faces go along it opposite ways */
    bool many;       /* more than two faces */
} medge;

typedef struct {
    gaddr h;
    mesh *m;
    int index;
} mcomp;

struct mesh {
    gaddr g;
    gaddr set;
    mvert **v;
    uint32_t nv, capv;
    mface **f;
    uint32_t nf, capf;
    medge **e;
    uint32_t ne;
    mcomp **comps;
    uint32_t ncomps;
    bool dirty, delayed;
};

static bool grow(void **p, uint32_t *cap, uint32_t need, size_t size)
{
    if (need <= *cap)
        return true;
    uint32_t n = *cap ? *cap * 2 : 16;
    while (n < need)
        n *= 2;
    void *q = realloc(*p, n * size);
    if (!q)
        return false;
    *p = q;
    *cap = n;
    return true;
}

/* ---------------------------------------------------------------------- */
/* Edges and components */

static void free_edges(mesh *m, bool keep_sets)
{
    for (uint32_t i = 0; i < m->ne; i++) {
        q3_handle_free(m->e[i]->h);
        if (!keep_sets && m->e[i]->set)
            q3_release(m->e[i]->set);
        free(m->e[i]);
    }
    free(m->e);
    m->e = NULL;
    m->ne = 0;
}

static void free_comps(mesh *m)
{
    for (uint32_t i = 0; i < m->ncomps; i++) {
        q3_handle_free(m->comps[i]->h);
        free(m->comps[i]);
    }
    free(m->comps);
    m->comps = NULL;
    m->ncomps = 0;
}

static int find(int *parent, int i)
{
    while (parent[i] != i)
        i = parent[i] = parent[parent[i]];
    return i;
}

static int vindex(const mesh *m, const mvert *v)
{
    for (uint32_t i = 0; i < m->nv; i++)
        if (m->v[i] == v)
            return (int)i;
    return -1;
}

/* Edges from the contours (old ones kept, with their handles and sets,
 * where the same two vertices still have one), then the components. */
static void rebuild(mesh *m)
{
    if (!m->dirty)
        return;
    m->dirty = false;
    medge **old = m->e;
    uint32_t nold = m->ne;
    m->e = NULL;
    m->ne = 0;
    uint32_t cap = 0;
    for (uint32_t fi = 0; fi < m->nf; fi++) {
        mface *f = m->f[fi];
        for (uint32_t ci = 0; ci < f->ncont; ci++) {
            mcont *c = f->c[ci];
            for (uint32_t k = 0; k < c->n && c->n > 1; k++) {
                mvert *a = c->v[k], *b = c->v[(k + 1) % c->n];
                if (a == b)
                    continue;
                medge *e = NULL;
                for (uint32_t j = 0; j < m->ne; j++)
                    if ((m->e[j]->a == a && m->e[j]->b == b) || (m->e[j]->a == b && m->e[j]->b == a)) {
                        e = m->e[j];
                        break;
                    }
                if (e) {
                    if (!e->f2 && e->f1 != f) {
                        e->f2 = f;
                        e->consistent = e->a == b; /* the first face went a to b */
                    } else {
                        e->many = true;
                    }
                    continue;
                }
                if (!grow((void **)&m->e, &cap, m->ne + 1, sizeof *m->e))
                    continue;
                e = NULL;
                for (uint32_t j = 0; j < nold; j++)
                    if (old[j] && ((old[j]->a == a && old[j]->b == b) || (old[j]->a == b && old[j]->b == a))) {
                        e = old[j];
                        old[j] = NULL;
                        break;
                    }
                if (!e) {
                    e = calloc(1, sizeof *e);
                    if (!e)
                        continue;
                    e->h = q3_handle_new(Q3H_MESH_EDGE, e);
                    e->m = m;
                }
                e->a = a;
                e->b = b;
                e->f1 = f;
                e->f2 = NULL;
                e->consistent = true;
                e->many = false;
                m->e[m->ne++] = e;
            }
        }
    }
    for (uint32_t j = 0; j < nold; j++)
        if (old[j]) {
            q3_handle_free(old[j]->h);
            if (old[j]->set)
                q3_release(old[j]->set);
            free(old[j]);
        }
    free(old);
    /* Components: vertices joined by edges. */
    free_comps(m);
    int *parent = malloc((m->nv + 1) * sizeof *parent);
    if (!parent)
        return;
    for (uint32_t i = 0; i < m->nv; i++)
        parent[i] = (int)i;
    for (uint32_t fi = 0; fi < m->nf; fi++) {
        mface *f = m->f[fi];
        int first = -1;
        for (uint32_t ci = 0; ci < f->ncont; ci++)
            for (uint32_t k = 0; k < f->c[ci]->n; k++) {
                int vi = vindex(m, f->c[ci]->v[k]);
                if (vi < 0)
                    continue;
                if (first < 0)
                    first = vi;
                else
                    parent[find(parent, vi)] = find(parent, first);
            }
    }
    int *map = malloc((m->nv + 1) * sizeof *map);
    for (uint32_t i = 0; map && i < m->nv; i++)
        map[i] = -1;
    uint32_t ccap = 0;
    for (uint32_t i = 0; map && i < m->nv; i++) {
        int r = find(parent, (int)i);
        if (map[r] < 0 && grow((void **)&m->comps, &ccap, m->ncomps + 1, sizeof *m->comps)) {
            mcomp *k = calloc(1, sizeof *k);
            if (k) {
                k->m = m;
                k->index = (int)m->ncomps;
                k->h = q3_handle_new(Q3H_MESH_COMPONENT, k);
                map[r] = (int)m->ncomps;
                m->comps[m->ncomps++] = k;
            }
        }
        m->v[i]->comp = map[r];
    }
    free(map);
    free(parent);
}

/* ---------------------------------------------------------------------- */
/* The mesh object */

static mesh *mesh_of(gaddr g)
{
    q3obj *o = q3_of(g, MESH);
    return o ? o->data : NULL;
}

static void free_face(mface *f)
{
    for (uint32_t ci = 0; ci < f->ncont; ci++) {
        q3_handle_free(f->c[ci]->h);
        free(f->c[ci]->v);
        free(f->c[ci]);
    }
    free(f->c);
    for (uint32_t i = 0; i < f->ncorners; i++)
        if (f->corners[i].set)
            q3_release(f->corners[i].set);
    free(f->corners);
    if (f->set)
        q3_release(f->set);
    q3_handle_free(f->h);
    free(f);
}

static void mesh_free(mesh *m)
{
    for (uint32_t i = 0; i < m->nf; i++)
        free_face(m->f[i]);
    for (uint32_t i = 0; i < m->nv; i++) {
        if (m->v[i]->set)
            q3_release(m->v[i]->set);
        q3_handle_free(m->v[i]->h);
        free(m->v[i]);
    }
    free_edges(m, false);
    free_comps(m);
    if (m->set)
        q3_release(m->set);
    free(m->v);
    free(m->f);
    free(m);
}

static void mesh_dispose(q3obj *o)
{
    if (o->data)
        mesh_free(o->data);
    o->data = NULL;
}

static mvert *add_vertex(mesh *m, const float p[3], gaddr set)
{
    if (!grow((void **)&m->v, &m->capv, m->nv + 1, sizeof *m->v))
        return NULL;
    mvert *v = calloc(1, sizeof *v);
    if (!v)
        return NULL;
    v->h = q3_handle_new(Q3H_MESH_VERTEX, v);
    if (!v->h) {
        free(v);
        return NULL;
    }
    v->m = m;
    memcpy(v->p, p, sizeof v->p);
    v->set = set ? q3_ref(set) : 0;
    m->v[m->nv++] = v;
    m->dirty = true;
    return v;
}

static mcont *new_contour(mface *f, mvert *const *vs, uint32_t n)
{
    mcont *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->v = malloc((n ? n : 1) * sizeof *c->v);
    c->h = q3_handle_new(Q3H_MESH_CONTOUR, c);
    if (!c->v || !c->h) {
        free(c->v);
        q3_handle_free(c->h);
        free(c);
        return NULL;
    }
    memcpy(c->v, vs, n * sizeof *c->v);
    c->n = n;
    c->face = f;
    return c;
}

static bool face_add_contour(mface *f, mcont *c)
{
    mcont **n = realloc(f->c, (f->ncont + 1) * sizeof *n);
    if (!n)
        return false;
    f->c = n;
    f->c[f->ncont++] = c;
    c->face = f;
    return true;
}

static mface *add_face(mesh *m, mvert *const *vs, uint32_t n, gaddr set)
{
    if (!grow((void **)&m->f, &m->capf, m->nf + 1, sizeof *m->f))
        return NULL;
    mface *f = calloc(1, sizeof *f);
    if (!f)
        return NULL;
    f->m = m;
    f->h = q3_handle_new(Q3H_MESH_FACE, f);
    mcont *c = f->h ? new_contour(f, vs, n) : NULL;
    if (!c || !face_add_contour(f, c)) {
        q3_handle_free(f->h);
        free(f);
        return NULL;
    }
    f->set = set ? q3_ref(set) : 0;
    m->f[m->nf++] = f;
    m->dirty = true;
    return f;
}

static void remove_face(mesh *m, mface *f)
{
    for (uint32_t i = 0; i < m->nf; i++)
        if (m->f[i] == f) {
            memmove(&m->f[i], &m->f[i + 1], (m->nf - i - 1) * sizeof *m->f);
            m->nf--;
            break;
        }
    free_face(f);
    m->dirty = true;
}

static bool mesh_duplicate(q3obj *to, const q3obj *from)
{
    mesh *src = from->data, *m = calloc(1, sizeof *m);
    if (!m)
        return false;
    m->g = to->g;
    m->set = src->set ? q3_ref(src->set) : 0;
    for (uint32_t i = 0; i < src->nv; i++)
        add_vertex(m, src->v[i]->p, src->v[i]->set);
    for (uint32_t fi = 0; fi < src->nf; fi++) {
        mface *sf = src->f[fi];
        mface *f = NULL;
        for (uint32_t ci = 0; ci < sf->ncont; ci++) {
            mcont *sc = sf->c[ci];
            mvert **vs = malloc((sc->n ? sc->n : 1) * sizeof *vs);
            if (!vs)
                continue;
            for (uint32_t k = 0; k < sc->n; k++)
                vs[k] = m->v[vindex(src, sc->v[k])];
            if (!f)
                f = add_face(m, vs, sc->n, sf->set);
            else {
                mcont *c = new_contour(f, vs, sc->n);
                if (c && !face_add_contour(f, c))
                    free(c);
            }
            free(vs);
        }
        for (uint32_t k = 0; f && k < sf->ncorners; k++) {
            mcorner *n = realloc(f->corners, (f->ncorners + 1) * sizeof *n);
            if (!n)
                break;
            f->corners = n;
            f->corners[f->ncorners++] = (mcorner){ m->v[vindex(src, sf->corners[k].v)], q3_ref(sf->corners[k].set) };
        }
    }
    rebuild((mesh *)src);
    rebuild(m);
    /* Edge sets by their vertices' places. */
    for (uint32_t i = 0; i < src->ne; i++) {
        if (!src->e[i]->set)
            continue;
        mvert *a = m->v[vindex(src, src->e[i]->a)], *b = m->v[vindex(src, src->e[i]->b)];
        for (uint32_t j = 0; j < m->ne; j++)
            if ((m->e[j]->a == a && m->e[j]->b == b) || (m->e[j]->a == b && m->e[j]->b == a))
                m->e[j]->set = q3_ref(src->e[i]->set);
    }
    to->data = m;
    return true;
}

static gaddr corner_set(const mface *f, const mvert *v)
{
    for (uint32_t i = 0; i < f->ncorners; i++)
        if (f->corners[i].v == v)
            return f->corners[i].set;
    return 0;
}

/* Every face's contours triangulated, each face's vertices its own (a
 * corner's set over the vertex's), the face's set on its triangles. */
static void mesh_submit(q3obj *o, q3view *view)
{
    mesh *m = o->data;
    if (!m || !m->nf)
        return;
    uint32_t total = 0;
    for (uint32_t fi = 0; fi < m->nf; fi++)
        for (uint32_t ci = 0; ci < m->f[fi]->ncont; ci++)
            total += m->f[fi]->c[ci]->n;
    q3pv *pv = calloc(total + 1, sizeof *pv);
    float (*pts)[3] = calloc(total + 1, sizeof *pts);
    q3ptri *t = NULL;
    uint32_t nt = 0, cap = 0, at = 0;
    for (uint32_t fi = 0; pv && pts && fi < m->nf; fi++) {
        mface *f = m->f[fi];
        uint32_t **cs = calloc(f->ncont, sizeof *cs), *cn = calloc(f->ncont, sizeof *cn);
        bool ok = cs && cn;
        for (uint32_t ci = 0; ok && ci < f->ncont; ci++) {
            mcont *c = f->c[ci];
            cn[ci] = c->n;
            cs[ci] = calloc(c->n + 1, sizeof **cs);
            if (!cs[ci]) {
                ok = false;
                break;
            }
            for (uint32_t k = 0; k < c->n; k++) {
                q3pv *x = &pv[at];
                memcpy(x->p, c->v[k]->p, sizeof x->p);
                memcpy(pts[at], c->v[k]->p, sizeof pts[at]);
                q3attrs_fill(&x->a, corner_set(f, c->v[k]));
                q3attrs_fill(&x->a, c->v[k]->set);
                cs[ci][k] = at++;
            }
        }
        if (ok) {
            uint32_t first = nt;
            q3_triangulate((const float (*)[3])pts, (const uint32_t *const *)cs, cn, f->ncont, &t, &nt, &cap);
            for (uint32_t i = first; i < nt; i++)
                q3attrs_fill(&t[i].a, f->set);
        }
        for (uint32_t ci = 0; cs && ci < f->ncont; ci++)
            free(cs[ci]);
        free(cs);
        free(cn);
    }
    if (pv && pts && nt) {
        q3v_begin_object(view, o->g);
        q3v_triangles(view, pv, at, t, nt, m->set);
        q3v_end_object(view);
    }
    free(t);
    free(pts);
    free(pv);
}

void q3_mesh_classes(void) { q3_class_ops(MESH, mesh_dispose, mesh_duplicate, mesh_submit, true); }

gaddr q3_mesh_set(q3obj *o)
{
    mesh *m = o && o->type == MESH ? o->data : NULL;
    return m ? m->set : 0;
}

void q3_mesh_set_set(q3obj *o, gaddr set)
{
    mesh *m = o && o->type == MESH ? o->data : NULL;
    if (m)
        q3_assign(&m->set, set);
}

static gaddr new_mesh(void)
{
    mesh *m = calloc(1, sizeof *m);
    q3obj *o = m ? q3_new(MESH, m) : NULL;
    if (!o) {
        free(m);
        return 0;
    }
    m->g = o->g;
    return o->g;
}

gaddr q3_mesh_build(uint32_t nverts, const float (*pts)[3], const gaddr *vsets, uint32_t nfaces,
                    const uint32_t *face_nverts, const uint32_t *face_verts, const gaddr *fsets)
{
    gaddr g = new_mesh();
    mesh *m = g ? q3_get(g)->data : NULL;
    if (!m)
        return 0;
    for (uint32_t i = 0; i < nverts; i++)
        add_vertex(m, pts[i], vsets ? vsets[i] : 0);
    uint32_t at = 0;
    mface *last = NULL;
    for (uint32_t f = 0; f < nfaces; f++) {
        /* The top bit: a contour (a hole) of the face before. */
        bool hole = face_nverts[f] & 0x80000000u;
        uint32_t n = face_nverts[f] & 0x7FFFFFFFu;
        mvert **vs = malloc((n ? n : 1) * sizeof *vs);
        uint32_t k = 0;
        for (uint32_t i = 0; vs && i < n; i++)
            if (face_verts[at + i] < m->nv)
                vs[k++] = m->v[face_verts[at + i]];
        if (vs && k >= 3 && hole && last) {
            mcont *c = new_contour(last, vs, k);
            if (c && !face_add_contour(last, c))
                free(c);
            m->dirty = true;
        } else if (vs && k >= 3) {
            last = add_face(m, vs, k, fsets ? fsets[f] : 0);
        }
        free(vs);
        at += n;
    }
    return g;
}

static bool face_has(mface *f, mvert *v);

/* A corner's or an edge's set on a mesh made at once (the 3DMF reader),
 * by vertex and face index. */
void q3_mesh_set_corner(gaddr g, uint32_t vertex, uint32_t face, gaddr set)
{
    q3obj *o = q3_get(g);
    mesh *m = o && o->type == MESH ? o->data : NULL;
    if (!m || !set || vertex >= m->nv || face >= m->nf || !face_has(m->f[face], m->v[vertex]))
        return;
    mface *f = m->f[face];
    for (uint32_t i = 0; i < f->ncorners; i++)
        if (f->corners[i].v == m->v[vertex]) {
            q3_assign(&f->corners[i].set, set);
            return;
        }
    mcorner *n = realloc(f->corners, (f->ncorners + 1) * sizeof *n);
    if (!n)
        return;
    f->corners = n;
    f->corners[f->ncorners++] = (mcorner){ m->v[vertex], q3_ref(set) };
}

void q3_mesh_set_edge(gaddr g, uint32_t v1, uint32_t v2, gaddr set)
{
    q3obj *o = q3_get(g);
    mesh *m = o && o->type == MESH ? o->data : NULL;
    if (!m || v1 >= m->nv || v2 >= m->nv)
        return;
    rebuild(m);
    for (uint32_t i = 0; i < m->ne; i++) {
        medge *e = m->e[i];
        if ((e->a == m->v[v1] && e->b == m->v[v2]) || (e->a == m->v[v2] && e->b == m->v[v1])) {
            q3_assign(&e->set, set);
            return;
        }
    }
}

void q3_mesh_walk(q3obj *o, const q3mesh_walker *w, void *ctx)
{
    mesh *m = o && o->type == MESH ? o->data : NULL;
    if (!m)
        return;
    rebuild(m);
    for (uint32_t i = 0; w->vertex && i < m->nv; i++)
        w->vertex(ctx, i, m->v[i]->p, m->v[i]->set);
    for (uint32_t fi = 0; w->face && fi < m->nf; fi++) {
        mface *f = m->f[fi];
        uint32_t total = 0;
        for (uint32_t ci = 0; ci < f->ncont; ci++)
            total += f->c[ci]->n;
        uint32_t *nv = calloc(f->ncont, sizeof *nv), *vs = calloc(total + 1, sizeof *vs), at = 0;
        for (uint32_t ci = 0; nv && vs && ci < f->ncont; ci++) {
            nv[ci] = f->c[ci]->n;
            for (uint32_t k = 0; k < f->c[ci]->n; k++)
                vs[at++] = (uint32_t)vindex(m, f->c[ci]->v[k]);
        }
        if (nv && vs)
            w->face(ctx, fi, f->ncont, nv, vs, f->set);
        free(nv);
        free(vs);
    }
    for (uint32_t fi = 0; w->corner && fi < m->nf; fi++)
        for (uint32_t k = 0; k < m->f[fi]->ncorners; k++)
            w->corner(ctx, (uint32_t)vindex(m, m->f[fi]->corners[k].v), fi, m->f[fi]->corners[k].set);
    for (uint32_t i = 0; w->edge && i < m->ne; i++)
        if (m->e[i]->set)
            w->edge(ctx, (uint32_t)vindex(m, m->e[i]->a), (uint32_t)vindex(m, m->e[i]->b), m->e[i]->set);
}

/* ---------------------------------------------------------------------- */
/* Handles */

static mvert *vert_of(gaddr h)
{
    mvert *v = q3_handle_get(h, Q3H_MESH_VERTEX);
    if (!v)
        q3_error(h ? kQ3ErrorInvalidParameter : kQ3ErrorNULLParameter);
    return v;
}

static mface *face_of(gaddr h)
{
    mface *f = q3_handle_get(h, Q3H_MESH_FACE);
    if (!f)
        q3_error(h ? kQ3ErrorInvalidParameter : kQ3ErrorNULLParameter);
    return f;
}

static medge *edge_of(gaddr h)
{
    medge *e = q3_handle_get(h, Q3H_MESH_EDGE);
    if (!e)
        q3_error(h ? kQ3ErrorInvalidParameter : kQ3ErrorNULLParameter);
    return e;
}

static mcont *contour_of(gaddr h)
{
    mcont *c = q3_handle_get(h, Q3H_MESH_CONTOUR);
    if (!c)
        q3_error(h ? kQ3ErrorInvalidParameter : kQ3ErrorNULLParameter);
    return c;
}

static mcomp *comp_of(gaddr h)
{
    mcomp *k = q3_handle_get(h, Q3H_MESH_COMPONENT);
    if (!k)
        q3_error(h ? kQ3ErrorInvalidParameter : kQ3ErrorNULLParameter);
    return k;
}

/* A part of mesh g (checking it is that mesh's). */
#define PART(type, var, fn, h, m)                                                                                    \
    type *var = (m) ? fn(h) : NULL;                                                                                  \
    if (var && var->m != (m)) {                                                                                      \
        q3_error(kQ3ErrorInvalidParameter);                                                                          \
        var = NULL;                                                                                                  \
    }

static void Q3Mesh_New(ppc_cpu *c) { Q3RETURN(new_mesh()); }

static void Q3Mesh_VertexNew(ppc_cpu *c)
{
    mesh *m = mesh_of(Q3R(0));
    gaddr in = Q3R(1);
    if (!m || !in) {
        if (m)
            q3_error(kQ3ErrorNULLParameter);
        Q3RETURN(0);
        return;
    }
    float p[3] = { rdf(in), rdf(in + 4), rdf(in + 8) };
    gaddr set = rd32(in + 12);
    mvert *v = add_vertex(m, p, q3_get(set) ? set : 0);
    q3_edited(q3_get(Q3R(0)));
    Q3RETURN(v ? v->h : 0);
}

static void Q3Mesh_FaceNew(ppc_cpu *c)
{
    mesh *m = mesh_of(Q3R(0));
    uint32_t n = Q3R(1);
    gaddr vs = Q3R(2);
    if (!m || !vs || n < 1 || n > 1000000) {
        if (m)
            q3_error(vs ? kQ3ErrorInvalidParameter : kQ3ErrorNULLParameter);
        Q3RETURN(0);
        return;
    }
    mvert **v = malloc(n * sizeof *v);
    for (uint32_t i = 0; v && i < n; i++) {
        mvert *x = vert_of(rd32(vs + 4 * i));
        if (!x || x->m != m) {
            free(v);
            v = NULL;
            q3_error(kQ3ErrorInvalidParameter);
        } else {
            v[i] = x;
        }
    }
    mface *f = v ? add_face(m, v, n, q3_get(Q3R(3)) ? Q3R(3) : 0) : NULL;
    free(v);
    q3_edited(q3_get(Q3R(0)));
    Q3RETURN(f ? f->h : 0);
}

/* A vertex goes, and the faces with it in them. */
static void Q3Mesh_VertexDelete(ppc_cpu *c)
{
    mesh *m = mesh_of(Q3R(0));
    PART(mvert, v, vert_of, Q3R(1), m)
    if (!v) {
        Q3RETURN(kQ3Failure);
        return;
    }
    for (uint32_t fi = m->nf; fi-- > 0;) {
        mface *f = m->f[fi];
        bool uses = false;
        for (uint32_t ci = 0; ci < f->ncont && !uses; ci++)
            for (uint32_t k = 0; k < f->c[ci]->n; k++)
                uses |= f->c[ci]->v[k] == v;
        if (uses)
            remove_face(m, f);
    }
    int vi = vindex(m, v);
    memmove(&m->v[vi], &m->v[vi + 1], (m->nv - (uint32_t)vi - 1) * sizeof *m->v);
    m->nv--;
    if (v->set)
        q3_release(v->set);
    q3_handle_free(v->h);
    free(v);
    m->dirty = true;
    q3_edited(q3_get(Q3R(0)));
    Q3RETURN(kQ3Success);
}

static void Q3Mesh_FaceDelete(ppc_cpu *c)
{
    mesh *m = mesh_of(Q3R(0));
    PART(mface, f, face_of, Q3R(1), m)
    if (f)
        remove_face(m, f);
    q3_edited(q3_get(Q3R(0)));
    Q3RETURN(f ? kQ3Success : kQ3Failure);
}

static void Q3Mesh_DelayUpdates(ppc_cpu *c)
{
    mesh *m = mesh_of(Q3R(0));
    if (m)
        m->delayed = true;
    Q3RETURN(m ? kQ3Success : kQ3Failure);
}

static void Q3Mesh_ResumeUpdates(ppc_cpu *c)
{
    mesh *m = mesh_of(Q3R(0));
    if (m) {
        m->delayed = false;
        rebuild(m);
    }
    Q3RETURN(m ? kQ3Success : kQ3Failure);
}

/* face's contours become holes in container; face goes. */
static void Q3Mesh_FaceToContour(ppc_cpu *c)
{
    mesh *m = mesh_of(Q3R(0));
    PART(mface, into, face_of, Q3R(1), m)
    PART(mface, f, face_of, Q3R(2), m)
    if (!into || !f || into == f) {
        Q3RETURN(0);
        return;
    }
    gaddr first = 0;
    for (uint32_t ci = 0; ci < f->ncont; ci++) {
        mcont *k = f->c[ci];
        if (face_add_contour(into, k)) {
            if (!first)
                first = k->h;
            f->c[ci] = NULL;
        }
    }
    uint32_t keep = 0;
    for (uint32_t ci = 0; ci < f->ncont; ci++)
        if (f->c[ci])
            f->c[keep++] = f->c[ci];
    f->ncont = keep;
    remove_face(m, f);
    q3_edited(q3_get(Q3R(0)));
    Q3RETURN(first);
}

/* A contour becomes a face of its own (with its old face's set). */
static void Q3Mesh_ContourToFace(ppc_cpu *c)
{
    mesh *m = mesh_of(Q3R(0));
    mcont *k = m ? contour_of(Q3R(1)) : NULL;
    if (!k || k->face->m != m) {
        Q3RETURN(0);
        return;
    }
    mface *old = k->face;
    if (old->ncont == 1) { /* already one */
        Q3RETURN(old->h);
        return;
    }
    if (!grow((void **)&m->f, &m->capf, m->nf + 1, sizeof *m->f)) {
        Q3RETURN(0);
        return;
    }
    mface *f = calloc(1, sizeof *f);
    if (!f) {
        Q3RETURN(0);
        return;
    }
    f->m = m;
    f->h = q3_handle_new(Q3H_MESH_FACE, f);
    for (uint32_t ci = 0; ci < old->ncont; ci++)
        if (old->c[ci] == k) {
            memmove(&old->c[ci], &old->c[ci + 1], (old->ncont - ci - 1) * sizeof *old->c);
            old->ncont--;
            break;
        }
    face_add_contour(f, k);
    f->set = old->set ? q3_ref(old->set) : 0;
    m->f[m->nf++] = f;
    m->dirty = true;
    q3_edited(q3_get(Q3R(0)));
    Q3RETURN(f->h);
}

static void put(gaddr out, uint32_t v) { wr32(out, v); }

/* A count, the topology brought up to date first. */
static void count(ppc_cpu *c, uint32_t (*fn)(mesh *))
{
    mesh *m = mesh_of(Q3R(0));
    if (!m || !Q3R(1)) {
        if (m)
            q3_error(kQ3ErrorNULLParameter);
        Q3RETURN(kQ3Failure);
        return;
    }
    rebuild(m);
    put(Q3R(1), fn(m));
    Q3RETURN(kQ3Success);
}

static uint32_t n_comps(mesh *m) { return m->ncomps; }
static uint32_t n_edges(mesh *m) { return m->ne; }
static uint32_t n_verts(mesh *m) { return m->nv; }
static uint32_t n_faces(mesh *m) { return m->nf; }
static uint32_t n_corners(mesh *m)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < m->nf; i++)
        n += m->f[i]->ncorners;
    return n;
}
static uint32_t orientable(mesh *m)
{
    for (uint32_t i = 0; i < m->ne; i++)
        if (m->e[i]->many || (m->e[i]->f2 && !m->e[i]->consistent))
            return kQ3False;
    return kQ3True;
}
static void Q3Mesh_GetNumComponents(ppc_cpu *c) { count(c, n_comps); }
static void Q3Mesh_GetNumEdges(ppc_cpu *c) { count(c, n_edges); }
static void Q3Mesh_GetNumVertices(ppc_cpu *c) { count(c, n_verts); }
static void Q3Mesh_GetNumFaces(ppc_cpu *c) { count(c, n_faces); }
static void Q3Mesh_GetNumCorners(ppc_cpu *c) { count(c, n_corners); }
static void Q3Mesh_GetOrientable(ppc_cpu *c) { count(c, orientable); }

/* Components: whose vertices and edges. */
static void comp_info(ppc_cpu *c, int what)
{
    mesh *m = mesh_of(Q3R(0));
    if (m)
        rebuild(m);
    PART(mcomp, k, comp_of, Q3R(1), m)
    gaddr out = Q3R(2);
    if (!k || !out) {
        Q3RETURN(kQ3Failure);
        return;
    }
    uint32_t n = 0;
    bool orient = true, any = false;
    float mn[3] = { 0 }, mx[3] = { 0 };
    for (uint32_t i = 0; i < m->nv; i++) {
        if (m->v[i]->comp != k->index)
            continue;
        n += what == 0;
        for (int a = 0; a < 3; a++) {
            mn[a] = any ? fminf(mn[a], m->v[i]->p[a]) : m->v[i]->p[a];
            mx[a] = any ? fmaxf(mx[a], m->v[i]->p[a]) : m->v[i]->p[a];
        }
        any = true;
    }
    for (uint32_t i = 0; i < m->ne; i++) {
        if (m->e[i]->a->comp != k->index)
            continue;
        n += what == 1;
        if (m->e[i]->many || (m->e[i]->f2 && !m->e[i]->consistent))
            orient = false;
    }
    if (what == 2) {
        for (int a = 0; a < 3; a++) {
            wrf(out + 4 * (uint32_t)a, mn[a]);
            wrf(out + 12 + 4 * (uint32_t)a, mx[a]);
        }
        put(out + 24, !any);
    } else {
        put(out, what == 3 ? (orient ? kQ3True : kQ3False) : n);
    }
    Q3RETURN(kQ3Success);
}
static void Q3Mesh_GetComponentNumVertices(ppc_cpu *c) { comp_info(c, 0); }
static void Q3Mesh_GetComponentNumEdges(ppc_cpu *c) { comp_info(c, 1); }
static void Q3Mesh_GetComponentBoundingBox(ppc_cpu *c) { comp_info(c, 2); }
static void Q3Mesh_GetComponentOrientable(ppc_cpu *c) { comp_info(c, 3); }

/* Vertices. */
static bool on_boundary(mesh *m, mvert *v)
{
    for (uint32_t i = 0; i < m->ne; i++)
        if ((m->e[i]->a == v || m->e[i]->b == v) && !m->e[i]->f2)
            return true;
    return false;
}

static void vert_info(ppc_cpu *c, int what)
{
    mesh *m = mesh_of(Q3R(0));
    PART(mvert, v, vert_of, Q3R(1), m)
    gaddr out = Q3R(2);
    if (!v || !out) {
        if (v)
            q3_error(kQ3ErrorNULLParameter);
        Q3RETURN(kQ3Failure);
        return;
    }
    rebuild(m);
    switch (what) {
    case 0:
        for (int a = 0; a < 3; a++)
            wrf(out + 4 * (uint32_t)a, v->p[a]);
        break;
    case 1: put(out, (uint32_t)vindex(m, v)); break;
    case 2: put(out, on_boundary(m, v)); break;
    case 3: put(out, v->comp >= 0 && (uint32_t)v->comp < m->ncomps ? m->comps[v->comp]->h : 0); break;
    default: put(out, v->set ? q3_ref(v->set) : 0); break;
    }
    Q3RETURN(kQ3Success);
}
static void Q3Mesh_GetVertexCoordinates(ppc_cpu *c) { vert_info(c, 0); }
static void Q3Mesh_GetVertexIndex(ppc_cpu *c) { vert_info(c, 1); }
static void Q3Mesh_GetVertexOnBoundary(ppc_cpu *c) { vert_info(c, 2); }
static void Q3Mesh_GetVertexComponent(ppc_cpu *c) { vert_info(c, 3); }
static void Q3Mesh_GetVertexAttributeSet(ppc_cpu *c) { vert_info(c, 4); }

static void Q3Mesh_SetVertexCoordinates(ppc_cpu *c)
{
    mesh *m = mesh_of(Q3R(0));
    PART(mvert, v, vert_of, Q3R(1), m)
    if (!v || !Q3R(2)) {
        Q3RETURN(kQ3Failure);
        return;
    }
    for (int a = 0; a < 3; a++)
        v->p[a] = rdf(Q3R(2) + 4 * (uint32_t)a);
    q3_edited(q3_get(Q3R(0)));
    Q3RETURN(kQ3Success);
}

static void Q3Mesh_SetVertexAttributeSet(ppc_cpu *c)
{
    mesh *m = mesh_of(Q3R(0));
    PART(mvert, v, vert_of, Q3R(1), m)
    if (v) {
        q3_assign(&v->set, Q3R(2));
        q3_edited(q3_get(Q3R(0)));
    }
    Q3RETURN(v ? kQ3Success : kQ3Failure);
}

/* Faces. */
static uint32_t face_index(mesh *m, mface *f)
{
    for (uint32_t i = 0; i < m->nf; i++)
        if (m->f[i] == f)
            return i;
    return 0;
}

static void face_info(ppc_cpu *c, int what)
{
    mesh *m = mesh_of(Q3R(0));
    PART(mface, f, face_of, Q3R(1), m)
    gaddr out = Q3R(2);
    if (!f || !out) {
        if (f)
            q3_error(kQ3ErrorNULLParameter);
        Q3RETURN(kQ3Failure);
        return;
    }
    rebuild(m);
    switch (what) {
    case 0: {
        uint32_t n = 0;
        for (uint32_t ci = 0; ci < f->ncont; ci++)
            n += f->c[ci]->n;
        put(out, n);
        break;
    }
    case 1: { /* Newell's normal of the outside, and the plane through it */
        mcont *k = f->c[0];
        float n[3] = { 0, 0, 0 }, ctr[3] = { 0, 0, 0 };
        for (uint32_t i = 0; i < k->n; i++) {
            const float *a = k->v[i]->p, *b = k->v[(i + 1) % k->n]->p;
            n[0] += (a[1] - b[1]) * (a[2] + b[2]);
            n[1] += (a[2] - b[2]) * (a[0] + b[0]);
            n[2] += (a[0] - b[0]) * (a[1] + b[1]);
            for (int x = 0; x < 3; x++)
                ctr[x] += a[x] / (float)k->n;
        }
        float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (l > 0)
            for (int x = 0; x < 3; x++)
                n[x] /= l;
        for (int x = 0; x < 3; x++)
            wrf(out + 4 * (uint32_t)x, n[x]);
        wrf(out + 12, -(n[0] * ctr[0] + n[1] * ctr[1] + n[2] * ctr[2]));
        break;
    }
    case 2: put(out, f->ncont); break;
    case 3: put(out, face_index(m, f)); break;
    case 4: {
        mvert *v = f->c[0]->n ? f->c[0]->v[0] : NULL;
        put(out, v && v->comp >= 0 && (uint32_t)v->comp < m->ncomps ? m->comps[v->comp]->h : 0);
        break;
    }
    default: put(out, f->set ? q3_ref(f->set) : 0); break;
    }
    Q3RETURN(kQ3Success);
}
static void Q3Mesh_GetFaceNumVertices(ppc_cpu *c) { face_info(c, 0); }
static void Q3Mesh_GetFacePlaneEquation(ppc_cpu *c) { face_info(c, 1); }
static void Q3Mesh_GetFaceNumContours(ppc_cpu *c) { face_info(c, 2); }
static void Q3Mesh_GetFaceIndex(ppc_cpu *c) { face_info(c, 3); }
static void Q3Mesh_GetFaceComponent(ppc_cpu *c) { face_info(c, 4); }
static void Q3Mesh_GetFaceAttributeSet(ppc_cpu *c) { face_info(c, 5); }

static void Q3Mesh_SetFaceAttributeSet(ppc_cpu *c)
{
    mesh *m = mesh_of(Q3R(0));
    PART(mface, f, face_of, Q3R(1), m)
    if (f) {
        q3_assign(&f->set, Q3R(2));
        q3_edited(q3_get(Q3R(0)));
    }
    Q3RETURN(f ? kQ3Success : kQ3Failure);
}

/* Edges. */
static medge *mesh_edge(mesh *m, gaddr h)
{
    if (!m)
        return NULL;
    rebuild(m);
    PART(medge, e, edge_of, h, m)
    return e;
}

static void Q3Mesh_GetEdgeVertices(ppc_cpu *c)
{
    medge *e = mesh_edge(mesh_of(Q3R(0)), Q3R(1));
    if (e && Q3R(2))
        put(Q3R(2), e->a->h);
    if (e && Q3R(3))
        put(Q3R(3), e->b->h);
    Q3RETURN(e ? kQ3Success : kQ3Failure);
}

static void Q3Mesh_GetEdgeFaces(ppc_cpu *c)
{
    medge *e = mesh_edge(mesh_of(Q3R(0)), Q3R(1));
    if (e && Q3R(2))
        put(Q3R(2), e->f1 ? e->f1->h : 0);
    if (e && Q3R(3))
        put(Q3R(3), e->f2 ? e->f2->h : 0);
    Q3RETURN(e ? kQ3Success : kQ3Failure);
}

static void edge_info(ppc_cpu *c, int what)
{
    mesh *m = mesh_of(Q3R(0));
    medge *e = mesh_edge(m, Q3R(1));
    gaddr out = Q3R(2);
    if (!e || !out) {
        Q3RETURN(kQ3Failure);
        return;
    }
    if (what == 0)
        put(out, e->f2 == NULL);
    else if (what == 1)
        put(out, e->a->comp >= 0 && (uint32_t)e->a->comp < m->ncomps ? m->comps[e->a->comp]->h : 0);
    else
        put(out, e->set ? q3_ref(e->set) : 0);
    Q3RETURN(kQ3Success);
}
static void Q3Mesh_GetEdgeOnBoundary(ppc_cpu *c) { edge_info(c, 0); }
static void Q3Mesh_GetEdgeComponent(ppc_cpu *c) { edge_info(c, 1); }
static void Q3Mesh_GetEdgeAttributeSet(ppc_cpu *c) { edge_info(c, 2); }

static void Q3Mesh_SetEdgeAttributeSet(ppc_cpu *c)
{
    medge *e = mesh_edge(mesh_of(Q3R(0)), Q3R(1));
    if (e) {
        q3_assign(&e->set, Q3R(2));
        q3_edited(q3_get(Q3R(0)));
    }
    Q3RETURN(e ? kQ3Success : kQ3Failure);
}

/* Contours. */
static void Q3Mesh_GetContourFace(ppc_cpu *c)
{
    mesh *m = mesh_of(Q3R(0));
    mcont *k = m ? contour_of(Q3R(1)) : NULL;
    if (!k || k->face->m != m || !Q3R(2)) {
        Q3RETURN(kQ3Failure);
        return;
    }
    put(Q3R(2), k->face->h);
    Q3RETURN(kQ3Success);
}

static void Q3Mesh_GetContourNumVertices(ppc_cpu *c)
{
    mesh *m = mesh_of(Q3R(0));
    mcont *k = m ? contour_of(Q3R(1)) : NULL;
    if (!k || k->face->m != m || !Q3R(2)) {
        Q3RETURN(kQ3Failure);
        return;
    }
    put(Q3R(2), k->n);
    Q3RETURN(kQ3Success);
}

/* Corners: a vertex of a face. */
static bool face_has(mface *f, mvert *v)
{
    for (uint32_t ci = 0; ci < f->ncont; ci++)
        for (uint32_t k = 0; k < f->c[ci]->n; k++)
            if (f->c[ci]->v[k] == v)
                return true;
    return false;
}

static void Q3Mesh_GetCornerAttributeSet(ppc_cpu *c)
{
    mesh *m = mesh_of(Q3R(0));
    PART(mvert, v, vert_of, Q3R(1), m)
    PART(mface, f, face_of, Q3R(2), m)
    if (!v || !f || !Q3R(3) || !face_has(f, v)) {
        Q3RETURN(kQ3Failure);
        return;
    }
    gaddr s = corner_set(f, v);
    put(Q3R(3), s ? q3_ref(s) : 0);
    Q3RETURN(kQ3Success);
}

static void Q3Mesh_SetCornerAttributeSet(ppc_cpu *c)
{
    mesh *m = mesh_of(Q3R(0));
    PART(mvert, v, vert_of, Q3R(1), m)
    PART(mface, f, face_of, Q3R(2), m)
    if (!v || !f || !face_has(f, v)) {
        if (v && f)
            q3_error(kQ3ErrorInvalidParameter);
        Q3RETURN(kQ3Failure);
        return;
    }
    for (uint32_t i = 0; i < f->ncorners; i++)
        if (f->corners[i].v == v) {
            q3_assign(&f->corners[i].set, Q3R(3));
            if (!f->corners[i].set) {
                memmove(&f->corners[i], &f->corners[i + 1], (f->ncorners - i - 1) * sizeof *f->corners);
                f->ncorners--;
            }
            Q3RETURN(kQ3Success);
            return;
        }
    if (Q3R(3)) {
        mcorner *n = realloc(f->corners, (f->ncorners + 1) * sizeof *n);
        if (!n) {
            Q3RETURN(kQ3Failure);
            return;
        }
        f->corners = n;
        f->corners[f->ncorners++] = (mcorner){ v, q3_ref(Q3R(3)) };
    }
    q3_edited(q3_get(Q3R(0)));
    Q3RETURN(kQ3Success);
}

/* ---------------------------------------------------------------------- */
/* Iterators: the list a walk goes through, made from what it walks. */

enum {
    IT_MESH_COMP = 1, IT_COMP_VERT, IT_COMP_EDGE, IT_MESH_VERT, IT_MESH_FACE, IT_MESH_EDGE, IT_VERT_EDGE,
    IT_VERT_VERT, IT_VERT_FACE, IT_FACE_EDGE, IT_FACE_VERT, IT_FACE_FACE, IT_FACE_CONT, IT_CONT_EDGE,
    IT_CONT_VERT, IT_CONT_FACE,
};

typedef struct {
    gaddr *h;
    uint32_t n, cap;
} hlist;

static void hl_add(hlist *l, gaddr h)
{
    if (!h)
        return;
    for (uint32_t i = 0; i < l->n; i++)
        if (l->h[i] == h)
            return;
    if (grow((void **)&l->h, &l->cap, l->n + 1, sizeof *l->h))
        l->h[l->n++] = h;
}

static void edges_of_contour(mesh *m, mcont *k, hlist *l)
{
    for (uint32_t i = 0; i < k->n && k->n > 1; i++) {
        mvert *a = k->v[i], *b = k->v[(i + 1) % k->n];
        for (uint32_t j = 0; j < m->ne; j++)
            if ((m->e[j]->a == a && m->e[j]->b == b) || (m->e[j]->a == b && m->e[j]->b == a))
                hl_add(l, m->e[j]->h);
    }
}

static void faces_by_edges(mesh *m, const hlist *edges, mface *self, hlist *l)
{
    for (uint32_t i = 0; i < edges->n; i++) {
        medge *e = q3_handle_get(edges->h[i], Q3H_MESH_EDGE);
        if (!e)
            continue;
        if (e->f1 && e->f1 != self)
            hl_add(l, e->f1->h);
        if (e->f2 && e->f2 != self)
            hl_add(l, e->f2->h);
    }
}

/* The list for a walk of kind over what (a handle), in mesh m. */
static void walk_list(mesh *m, int kind, gaddr what, hlist *l)
{
    rebuild(m);
    switch (kind) {
    case IT_MESH_COMP:
        for (uint32_t i = 0; i < m->ncomps; i++)
            hl_add(l, m->comps[i]->h);
        break;
    case IT_MESH_VERT:
        for (uint32_t i = 0; i < m->nv; i++)
            hl_add(l, m->v[i]->h);
        break;
    case IT_MESH_FACE:
        for (uint32_t i = 0; i < m->nf; i++)
            hl_add(l, m->f[i]->h);
        break;
    case IT_MESH_EDGE:
        for (uint32_t i = 0; i < m->ne; i++)
            hl_add(l, m->e[i]->h);
        break;
    case IT_COMP_VERT:
    case IT_COMP_EDGE: {
        mcomp *k = q3_handle_get(what, Q3H_MESH_COMPONENT);
        for (uint32_t i = 0; k && kind == IT_COMP_VERT && i < m->nv; i++)
            if (m->v[i]->comp == k->index)
                hl_add(l, m->v[i]->h);
        for (uint32_t i = 0; k && kind == IT_COMP_EDGE && i < m->ne; i++)
            if (m->e[i]->a->comp == k->index)
                hl_add(l, m->e[i]->h);
        break;
    }
    case IT_VERT_EDGE:
    case IT_VERT_VERT: {
        mvert *v = q3_handle_get(what, Q3H_MESH_VERTEX);
        for (uint32_t i = 0; v && i < m->ne; i++)
            if (m->e[i]->a == v || m->e[i]->b == v)
                hl_add(l, kind == IT_VERT_EDGE ? m->e[i]->h : (m->e[i]->a == v ? m->e[i]->b : m->e[i]->a)->h);
        break;
    }
    case IT_VERT_FACE: {
        mvert *v = q3_handle_get(what, Q3H_MESH_VERTEX);
        for (uint32_t i = 0; v && i < m->nf; i++)
            if (face_has(m->f[i], v))
                hl_add(l, m->f[i]->h);
        break;
    }
    case IT_FACE_EDGE:
    case IT_FACE_VERT:
    case IT_FACE_FACE:
    case IT_FACE_CONT: {
        mface *f = q3_handle_get(what, Q3H_MESH_FACE);
        for (uint32_t ci = 0; f && ci < f->ncont; ci++) {
            mcont *k = f->c[ci];
            if (kind == IT_FACE_CONT)
                hl_add(l, k->h);
            else if (kind == IT_FACE_VERT)
                for (uint32_t i = 0; i < k->n; i++)
                    hl_add(l, k->v[i]->h);
            else
                edges_of_contour(m, k, l);
        }
        if (f && kind == IT_FACE_FACE) {
            hlist edges = *l;
            hlist faces = { NULL, 0, 0 };
            faces_by_edges(m, &edges, f, &faces);
            free(edges.h);
            *l = faces;
        }
        break;
    }
    case IT_CONT_EDGE:
    case IT_CONT_VERT:
    case IT_CONT_FACE: {
        mcont *k = q3_handle_get(what, Q3H_MESH_CONTOUR);
        if (!k)
            break;
        if (kind == IT_CONT_VERT) {
            for (uint32_t i = 0; i < k->n; i++)
                hl_add(l, k->v[i]->h);
            break;
        }
        edges_of_contour(m, k, l);
        if (kind == IT_CONT_FACE) {
            hlist edges = *l;
            hlist faces = { NULL, 0, 0 };
            faces_by_edges(m, &edges, k->face, &faces);
            free(edges.h);
            *l = faces;
        }
        break;
    }
    default: break;
    }
}

/* The mesh of what a walk starts from. */
static mesh *mesh_from(int kind, gaddr what)
{
    switch (kind) {
    case IT_MESH_COMP:
    case IT_MESH_VERT:
    case IT_MESH_FACE:
    case IT_MESH_EDGE: return mesh_of(what);
    case IT_COMP_VERT:
    case IT_COMP_EDGE: {
        mcomp *k = comp_of(what);
        return k ? k->m : NULL;
    }
    case IT_VERT_EDGE:
    case IT_VERT_VERT:
    case IT_VERT_FACE: {
        mvert *v = vert_of(what);
        return v ? v->m : NULL;
    }
    case IT_FACE_EDGE:
    case IT_FACE_VERT:
    case IT_FACE_FACE:
    case IT_FACE_CONT: {
        mface *f = face_of(what);
        return f ? f->m : NULL;
    }
    default: {
        mcont *k = contour_of(what);
        return k ? k->face->m : NULL;
    }
    }
}

static gaddr step(gaddr it)
{
    gaddr what = rd32(it), m_g = rd32(it + 12);
    uint32_t i = rd32(it + 4), kind = rd32(it + 8);
    q3obj *o = q3_get(m_g);
    mesh *m = o && o->type == MESH ? o->data : NULL;
    if (!m)
        return 0;
    hlist l = { NULL, 0, 0 };
    walk_list(m, (int)kind, what, &l);
    gaddr h = i < l.n ? l.h[i] : 0;
    free(l.h);
    wr32(it + 4, i + 1);
    return h;
}

static gaddr first(gaddr what, gaddr it, int kind)
{
    mesh *m = mesh_from(kind, what);
    if (!m || !it) {
        if (!it)
            q3_error(kQ3ErrorNULLParameter);
        return 0;
    }
    wr32(it, what);
    wr32(it + 4, 0);
    wr32(it + 8, (uint32_t)kind);
    wr32(it + 12, m->g);
    wr32(it + 16, 0);
    return step(it);
}

#define WALK(First, Next, KIND)                                                                                      \
    static void First(ppc_cpu *c) { Q3RETURN(first(Q3R(0), Q3R(1), KIND)); }                                         \
    static void Next(ppc_cpu *c) { Q3RETURN(Q3R(0) ? step(Q3R(0)) : 0); }

WALK(Q3Mesh_FirstMeshComponent, Q3Mesh_NextMeshComponent, IT_MESH_COMP)
WALK(Q3Mesh_FirstComponentVertex, Q3Mesh_NextComponentVertex, IT_COMP_VERT)
WALK(Q3Mesh_FirstComponentEdge, Q3Mesh_NextComponentEdge, IT_COMP_EDGE)
WALK(Q3Mesh_FirstMeshVertex, Q3Mesh_NextMeshVertex, IT_MESH_VERT)
WALK(Q3Mesh_FirstMeshFace, Q3Mesh_NextMeshFace, IT_MESH_FACE)
WALK(Q3Mesh_FirstMeshEdge, Q3Mesh_NextMeshEdge, IT_MESH_EDGE)
WALK(Q3Mesh_FirstVertexEdge, Q3Mesh_NextVertexEdge, IT_VERT_EDGE)
WALK(Q3Mesh_FirstVertexVertex, Q3Mesh_NextVertexVertex, IT_VERT_VERT)
WALK(Q3Mesh_FirstVertexFace, Q3Mesh_NextVertexFace, IT_VERT_FACE)
WALK(Q3Mesh_FirstFaceEdge, Q3Mesh_NextFaceEdge, IT_FACE_EDGE)
WALK(Q3Mesh_FirstFaceVertex, Q3Mesh_NextFaceVertex, IT_FACE_VERT)
WALK(Q3Mesh_FirstFaceFace, Q3Mesh_NextFaceFace, IT_FACE_FACE)
WALK(Q3Mesh_FirstFaceContour, Q3Mesh_NextFaceContour, IT_FACE_CONT)
WALK(Q3Mesh_FirstContourEdge, Q3Mesh_NextContourEdge, IT_CONT_EDGE)
WALK(Q3Mesh_FirstContourVertex, Q3Mesh_NextContourVertex, IT_CONT_VERT)
WALK(Q3Mesh_FirstContourFace, Q3Mesh_NextContourFace, IT_CONT_FACE)

#define E(n) { #n, n }
const ppc_export q3_mesh_exports[] = {
    E(Q3Mesh_New), E(Q3Mesh_VertexNew), E(Q3Mesh_FaceNew), E(Q3Mesh_VertexDelete), E(Q3Mesh_FaceDelete),
    E(Q3Mesh_DelayUpdates), E(Q3Mesh_ResumeUpdates), E(Q3Mesh_FaceToContour), E(Q3Mesh_ContourToFace),
    E(Q3Mesh_GetNumComponents), E(Q3Mesh_GetNumEdges), E(Q3Mesh_GetNumVertices), E(Q3Mesh_GetNumFaces),
    E(Q3Mesh_GetNumCorners), E(Q3Mesh_GetOrientable), E(Q3Mesh_GetComponentNumVertices),
    E(Q3Mesh_GetComponentNumEdges), E(Q3Mesh_GetComponentBoundingBox), E(Q3Mesh_GetComponentOrientable),
    E(Q3Mesh_GetVertexCoordinates), E(Q3Mesh_GetVertexIndex), E(Q3Mesh_GetVertexOnBoundary),
    E(Q3Mesh_GetVertexComponent), E(Q3Mesh_GetVertexAttributeSet), E(Q3Mesh_SetVertexCoordinates),
    E(Q3Mesh_SetVertexAttributeSet), E(Q3Mesh_GetFaceNumVertices), E(Q3Mesh_GetFacePlaneEquation),
    E(Q3Mesh_GetFaceNumContours), E(Q3Mesh_GetFaceIndex), E(Q3Mesh_GetFaceComponent), E(Q3Mesh_GetFaceAttributeSet),
    E(Q3Mesh_SetFaceAttributeSet), E(Q3Mesh_GetEdgeVertices), E(Q3Mesh_GetEdgeFaces), E(Q3Mesh_GetEdgeOnBoundary),
    E(Q3Mesh_GetEdgeComponent), E(Q3Mesh_GetEdgeAttributeSet), E(Q3Mesh_SetEdgeAttributeSet),
    E(Q3Mesh_GetContourFace), E(Q3Mesh_GetContourNumVertices), E(Q3Mesh_GetCornerAttributeSet),
    E(Q3Mesh_SetCornerAttributeSet),
    E(Q3Mesh_FirstMeshComponent), E(Q3Mesh_NextMeshComponent), E(Q3Mesh_FirstComponentVertex),
    E(Q3Mesh_NextComponentVertex), E(Q3Mesh_FirstComponentEdge), E(Q3Mesh_NextComponentEdge),
    E(Q3Mesh_FirstMeshVertex), E(Q3Mesh_NextMeshVertex), E(Q3Mesh_FirstMeshFace), E(Q3Mesh_NextMeshFace),
    E(Q3Mesh_FirstMeshEdge), E(Q3Mesh_NextMeshEdge), E(Q3Mesh_FirstVertexEdge), E(Q3Mesh_NextVertexEdge),
    E(Q3Mesh_FirstVertexVertex), E(Q3Mesh_NextVertexVertex), E(Q3Mesh_FirstVertexFace), E(Q3Mesh_NextVertexFace),
    E(Q3Mesh_FirstFaceEdge), E(Q3Mesh_NextFaceEdge), E(Q3Mesh_FirstFaceVertex), E(Q3Mesh_NextFaceVertex),
    E(Q3Mesh_FirstFaceFace), E(Q3Mesh_NextFaceFace), E(Q3Mesh_FirstFaceContour), E(Q3Mesh_NextFaceContour),
    E(Q3Mesh_FirstContourEdge), E(Q3Mesh_NextContourEdge), E(Q3Mesh_FirstContourVertex),
    E(Q3Mesh_NextContourVertex), E(Q3Mesh_FirstContourFace), E(Q3Mesh_NextContourFace),
};
const unsigned q3_mesh_export_count = sizeof q3_mesh_exports / sizeof q3_mesh_exports[0];
