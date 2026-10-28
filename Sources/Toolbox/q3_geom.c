/*
 * QuickDraw 3D's geometries (QD3DGeometry.h) but the mesh (q3_mesh.c):
 * their data as schemas (q3_data.h), the calls that make, get, set and
 * empty them and their fields, and each broken into what a view draws
 * (q3_view.h): triangles, lines, points and images.  Curved surfaces are
 * tessellated as the subdivision style says.
 */
#include <math.h>
#include <stdlib.h>

#include "q3_view.h"
#include "tb_internal.h"

#define BOX Q3T('b', 'o', 'x', ' ')
#define GPOLY Q3T('g', 'p', 'g', 'n')
#define LINE Q3T('l', 'i', 'n', 'e')
#define MARKER Q3T('m', 'r', 'k', 'r')
#define PMARKER Q3T('m', 'r', 'k', 'p')
#define NCURVE Q3T('n', 'r', 'b', 'c')
#define NPATCH Q3T('n', 'r', 'b', 'p')
#define POINT Q3T('p', 'n', 't', ' ')
#define POLYGON Q3T('p', 'l', 'y', 'g')
#define POLYLINE Q3T('p', 'l', 'y', 'l')
#define TRIANGLE Q3T('t', 'r', 'n', 'g')
#define TRIGRID Q3T('t', 'r', 'i', 'g')
#define CONE Q3T('c', 'o', 'n', 'e')
#define CYLINDER Q3T('c', 'y', 'l', 'n')
#define DISK Q3T('d', 'i', 's', 'k')
#define ELLIPSE Q3T('e', 'l', 'p', 's')
#define ELLIPSOID Q3T('e', 'l', 'p', 'd')
#define POLYHEDRON Q3T('p', 'l', 'h', 'd')
#define TORUS Q3T('t', 'o', 'r', 's')
#define TRIMESH Q3T('t', 'm', 's', 'h')

/* ---------------------------------------------------------------------- */
/* Schemas */

static uint32_t self0(const uint32_t *s, const uint32_t *p) { return s[0]; }
static uint32_t self1(const uint32_t *s, const uint32_t *p) { return s[1]; }
static uint32_t self2(const uint32_t *s, const uint32_t *p) { return s[2]; }
static uint32_t self4(const uint32_t *s, const uint32_t *p) { return s[4]; }
static uint32_t self5(const uint32_t *s, const uint32_t *p) { return s[5]; }
static uint32_t self7(const uint32_t *s, const uint32_t *p) { return s[7]; }
static uint32_t self9(const uint32_t *s, const uint32_t *p) { return s[9]; }
static uint32_t six(const uint32_t *s, const uint32_t *p) { return 6; }

static const q3schema word_s = { 1, 0, NULL };
static const q3field objword_f[] = { { 0, QF_OBJ, NULL, NULL } };
static const q3schema objword_s = { 1, 1, objword_f };
static const q3schema point3_s = { 3, 0, NULL };
static const q3schema point4_s = { 4, 0, NULL };
static const q3field vertex_f[] = { { 3, QF_OBJ, NULL, NULL } };
static const q3schema vertex_s = { 4, 1, vertex_f };

static const q3field box_f[] = { { 12, QF_PTR, six, &objword_s }, { 13, QF_OBJ, NULL, NULL } };
static const q3schema box_s = { 14, 2, box_f };
static const q3field cone_f[] = { { 17, QF_OBJ, NULL, NULL }, { 18, QF_OBJ, NULL, NULL }, { 19, QF_OBJ, NULL, NULL }, { 20, QF_OBJ, NULL, NULL } };
static const q3schema cone_s = { 21, 4, cone_f };
static const q3field cyl_f[] = { { 17, QF_OBJ, NULL, NULL }, { 18, QF_OBJ, NULL, NULL }, { 19, QF_OBJ, NULL, NULL }, { 20, QF_OBJ, NULL, NULL }, { 21, QF_OBJ, NULL, NULL } };
static const q3schema cyl_s = { 22, 5, cyl_f };
static const q3field disk_f[] = { { 13, QF_OBJ, NULL, NULL } };
static const q3schema disk_s = { 14, 1, disk_f };
static const q3field ellipse_f[] = { { 11, QF_OBJ, NULL, NULL } };
static const q3schema ellipse_s = { 12, 1, ellipse_f };
static const q3field ellipsoid_f[] = { { 17, QF_OBJ, NULL, NULL }, { 18, QF_OBJ, NULL, NULL } };
static const q3schema ellipsoid_s = { 19, 2, ellipsoid_f };
static const q3field contour_f[] = { { 1, QF_PTR, self0, &vertex_s } };
static const q3schema contour_s = { 2, 1, contour_f };
static const q3field gpoly_f[] = { { 1, QF_PTR, self0, &contour_s }, { 3, QF_OBJ, NULL, NULL } };
static const q3schema gpoly_s = { 4, 2, gpoly_f };
static const q3field line_f[] = { { 3, QF_OBJ, NULL, NULL }, { 7, QF_OBJ, NULL, NULL }, { 8, QF_OBJ, NULL, NULL } };
static const q3schema line_s = { 9, 3, line_f };
static uint32_t bitmap_bytes(const uint32_t *s, const uint32_t *p) { return s[7] * s[8]; }
static const q3field marker_f[] = { { 5, QF_BYTES, bitmap_bytes, NULL }, { 10, QF_OBJ, NULL, NULL } };
static const q3schema marker_s = { 11, 2, marker_f };
static uint32_t curve_knots(const uint32_t *s, const uint32_t *p) { return s[0] + s[1]; }
static const q3field ncurve_f[] = { { 2, QF_PTR, self1, &point4_s }, { 3, QF_PTR, curve_knots, &word_s },
                                    { 4, QF_OBJ, NULL, NULL } };
static const q3schema ncurve_s = { 5, 3, ncurve_f };
static const q3field trimcurve_f[] = { { 2, QF_PTR, self1, &point3_s }, { 3, QF_PTR, curve_knots, &word_s } };
static const q3schema trimcurve_s = { 4, 2, trimcurve_f };
static const q3field trimloop_f[] = { { 1, QF_PTR, self0, &trimcurve_s } };
static const q3schema trimloop_s = { 2, 1, trimloop_f };
static uint32_t patch_points(const uint32_t *s, const uint32_t *p) { return s[2] * s[3]; }
static uint32_t patch_uknots(const uint32_t *s, const uint32_t *p) { return s[0] + s[3]; }
static uint32_t patch_vknots(const uint32_t *s, const uint32_t *p) { return s[1] + s[2]; }
static const q3field npatch_f[] = { { 4, QF_PTR, patch_points, &point4_s }, { 5, QF_PTR, patch_uknots, &word_s },
                                    { 6, QF_PTR, patch_vknots, &word_s },   { 8, QF_PTR, self7, &trimloop_s },
                                    { 9, QF_OBJ, NULL, NULL } };
static const q3schema npatch_s = { 10, 5, npatch_f };
static const q3field pmarker_f[] = { { 5, QF_OBJ, NULL, NULL }, { 13, QF_OBJ, NULL, NULL } };
static const q3schema pmarker_s = { 14, 2, pmarker_f };
static const q3field point_f[] = { { 3, QF_OBJ, NULL, NULL } };
static const q3schema point_s = { 4, 1, point_f };
static const q3field polygon_f[] = { { 1, QF_PTR, self0, &vertex_s }, { 2, QF_OBJ, NULL, NULL } };
static const q3schema polygon_s = { 3, 2, polygon_f };
static const q3field phedge_f[] = { { 4, QF_OBJ, NULL, NULL } };
static const q3schema phedge_s = { 5, 1, phedge_f };
static const q3field phtri_f[] = { { 4, QF_OBJ, NULL, NULL } };
static const q3schema phtri_s = { 5, 1, phtri_f };
static const q3field polyhedron_f[] = { { 1, QF_PTR, self0, &vertex_s }, { 3, QF_PTR, self2, &phedge_s },
                                        { 5, QF_PTR, self4, &phtri_s },  { 6, QF_OBJ, NULL, NULL } };
static const q3schema polyhedron_s = { 7, 4, polyhedron_f };
static uint32_t segments(const uint32_t *s, const uint32_t *p) { return s[0] ? s[0] - 1 : 0; }
static const q3field polyline_f[] = { { 1, QF_PTR, self0, &vertex_s }, { 2, QF_PTR, segments, &objword_s },
                                      { 3, QF_OBJ, NULL, NULL } };
static const q3schema polyline_s = { 4, 3, polyline_f };
static const q3field torus_f[] = { { 18, QF_OBJ, NULL, NULL }, { 19, QF_OBJ, NULL, NULL } };
static const q3schema torus_s = { 20, 2, torus_f };
static const q3field triangle_f[] = { { 3, QF_OBJ, NULL, NULL }, { 7, QF_OBJ, NULL, NULL }, { 11, QF_OBJ, NULL, NULL }, { 12, QF_OBJ, NULL, NULL } };
static const q3schema triangle_s = { 13, 4, triangle_f };
static uint32_t grid_points(const uint32_t *s, const uint32_t *p) { return s[0] * s[1]; }
static uint32_t grid_facets(const uint32_t *s, const uint32_t *p)
{
    return s[0] > 1 && s[1] > 1 ? 2 * (s[0] - 1) * (s[1] - 1) : 0;
}
static const q3field trigrid_f[] = { { 2, QF_PTR, grid_points, &vertex_s }, { 3, QF_PTR, grid_facets, &objword_s },
                                     { 4, QF_OBJ, NULL, NULL } };
static const q3schema trigrid_s = { 5, 3, trigrid_f };
/* A trimesh's attribute arrays: the data's size is the attribute's times
 * the triangles', edges' or points' count, in the trimesh (parent). */
static uint32_t attr_size(const uint32_t *s)
{
    int32_t n = q3_element_size((int32_t)s[0]);
    return n > 0 ? (uint32_t)n : 0;
}
static uint32_t tri_data(const uint32_t *s, const uint32_t *p) { return p ? attr_size(s) * p[1] : 0; }
static uint32_t tri_use(const uint32_t *s, const uint32_t *p) { return p ? p[1] : 0; }
static uint32_t edge_data(const uint32_t *s, const uint32_t *p) { return p ? attr_size(s) * p[5] : 0; }
static uint32_t edge_use(const uint32_t *s, const uint32_t *p) { return p ? p[5] : 0; }
static uint32_t vtx_data(const uint32_t *s, const uint32_t *p) { return p ? attr_size(s) * p[9] : 0; }
static uint32_t vtx_use(const uint32_t *s, const uint32_t *p) { return p ? p[9] : 0; }
static const q3field tmtri_f[] = { { 1, QF_BYTES, tri_data, NULL }, { 2, QF_BYTES, tri_use, NULL } };
static const q3schema tmtri_s = { 3, 2, tmtri_f };
static const q3field tmedge_f[] = { { 1, QF_BYTES, edge_data, NULL }, { 2, QF_BYTES, edge_use, NULL } };
static const q3schema tmedge_s = { 3, 2, tmedge_f };
static const q3field tmvtx_f[] = { { 1, QF_BYTES, vtx_data, NULL }, { 2, QF_BYTES, vtx_use, NULL } };
static const q3schema tmvtx_s = { 3, 2, tmvtx_f };
static const q3schema tri3_s = { 3, 0, NULL };
static const q3schema edge4_s = { 4, 0, NULL };
static const q3field trimesh_f[] = {
    { 0, QF_OBJ, NULL, NULL },
    { 2, QF_PTR, self1, &tri3_s },
    { 4, QF_PTR, NULL, &tmtri_s },
    { 6, QF_PTR, self5, &edge4_s },
    { 8, QF_PTR, NULL, &tmedge_s },
    { 10, QF_PTR, self9, &point3_s },
    { 12, QF_PTR, NULL, &tmvtx_s },
};
static uint32_t self3(const uint32_t *s, const uint32_t *p) { return s[3]; }
static uint32_t self11(const uint32_t *s, const uint32_t *p) { return s[11]; }
static q3field trimesh_fields[7];
static const q3schema trimesh_s = { 20, 7, trimesh_fields };

/* Each geometry: its schema, where its own attribute set is, and how it
 * is drawn. */
typedef void (*tess_fn)(q3view *v, const q3node *d);
static void tess_box(q3view *, const q3node *), tess_cone(q3view *, const q3node *),
    tess_cylinder(q3view *, const q3node *), tess_disk(q3view *, const q3node *),
    tess_ellipse(q3view *, const q3node *), tess_ellipsoid(q3view *, const q3node *),
    tess_gpoly(q3view *, const q3node *), tess_line(q3view *, const q3node *),
    tess_marker(q3view *, const q3node *), tess_ncurve(q3view *, const q3node *),
    tess_npatch(q3view *, const q3node *), tess_pmarker(q3view *, const q3node *),
    tess_point(q3view *, const q3node *), tess_polygon(q3view *, const q3node *),
    tess_polyhedron(q3view *, const q3node *), tess_polyline(q3view *, const q3node *),
    tess_torus(q3view *, const q3node *), tess_triangle(q3view *, const q3node *),
    tess_trigrid(q3view *, const q3node *), tess_trimesh(q3view *, const q3node *);

static const struct geom {
    uint32_t type;
    const q3schema *s;
    int set_off;
    tess_fn tess;
} geoms[] = {
    { BOX, &box_s, 13, tess_box },
    { CONE, &cone_s, 20, tess_cone },
    { CYLINDER, &cyl_s, 21, tess_cylinder },
    { DISK, &disk_s, 13, tess_disk },
    { ELLIPSE, &ellipse_s, 11, tess_ellipse },
    { ELLIPSOID, &ellipsoid_s, 18, tess_ellipsoid },
    { GPOLY, &gpoly_s, 3, tess_gpoly },
    { LINE, &line_s, 8, tess_line },
    { MARKER, &marker_s, 10, tess_marker },
    { NCURVE, &ncurve_s, 4, tess_ncurve },
    { NPATCH, &npatch_s, 9, tess_npatch },
    { PMARKER, &pmarker_s, 13, tess_pmarker },
    { POINT, &point_s, 3, tess_point },
    { POLYGON, &polygon_s, 2, tess_polygon },
    { POLYHEDRON, &polyhedron_s, 6, tess_polyhedron },
    { POLYLINE, &polyline_s, 3, tess_polyline },
    { TORUS, &torus_s, 19, tess_torus },
    { TRIANGLE, &triangle_s, 12, tess_triangle },
    { TRIGRID, &trigrid_s, 4, tess_trigrid },
    { TRIMESH, &trimesh_s, 0, tess_trimesh },
};
enum { NGEOMS = sizeof geoms / sizeof geoms[0] };

static const struct geom *geom_of(uint32_t type)
{
    for (int depth = 0; type && depth < 32; depth++) {
        for (int i = 0; i < NGEOMS; i++)
            if (geoms[i].type == type)
                return &geoms[i];
        q3class *k = q3_class(type);
        type = k ? k->parent : 0;
    }
    return NULL;
}

/* A trimesh's bounding box, from its points, when it came empty. */
static void trimesh_bounds(q3node *d)
{
    if (!d->w[19])
        return;
    q3node *pts = q3d_kid(d, &trimesh_s, 0, 5);
    if (!pts || !pts->n)
        return;
    float mn[3], mx[3];
    for (int k = 0; k < 3; k++)
        mn[k] = mx[k] = q3d_f(pts->w, k);
    for (uint32_t i = 1; i < pts->n; i++)
        for (int k = 0; k < 3; k++) {
            float f = q3d_f(pts->w, (int)(i * 3) + k);
            mn[k] = fminf(mn[k], f);
            mx[k] = fmaxf(mx[k], f);
        }
    for (int k = 0; k < 3; k++) {
        q3d_setf(d->w, 13 + k, mn[k]);
        q3d_setf(d->w, 16 + k, mx[k]);
    }
    d->w[19] = 0;
}

static void geom_submit(q3obj *o, q3view *v)
{
    const struct geom *g = geom_of(o->type);
    if (g && o->data && q3xr_geometry(v, o->type, o->g, 0))
        return;
    if (g && o->data) {
        q3v_begin_object(v, o->g);
        g->tess(v, o->data);
        q3v_end_object(v);
    }
}

void q3_geom_classes(void)
{
    trimesh_fields[0] = trimesh_f[0];
    trimesh_fields[1] = trimesh_f[1];
    trimesh_fields[2] = (q3field){ 4, QF_PTR, self3, &tmtri_s };
    trimesh_fields[3] = trimesh_f[3];
    trimesh_fields[4] = (q3field){ 8, QF_PTR, self7, &tmedge_s };
    trimesh_fields[5] = trimesh_f[5];
    trimesh_fields[6] = (q3field){ 12, QF_PTR, self11, &tmvtx_s };
    for (int i = 0; i < NGEOMS; i++)
        q3_data_class(geoms[i].type, geoms[i].s, geom_submit, true);
    q3_group_classes();
}

static gaddr new_geom(uint32_t type, gaddr data);

/* Immediate mode: the program's structure drawn and forgotten (or, in a
 * writing loop, written). */
static void submit_now(ppc_cpu *c, uint32_t type)
{
    const struct geom *g = geom_of(type);
    q3view *v = q3v_of(Q3R(1));
    if (!v || !Q3R(0)) {
        Q3RETURN(kQ3Failure);
        return;
    }
    if (q3v_writing(v)) { /* written as the object it would make */
        gaddr obj = new_geom(type, Q3R(0));
        if (obj) {
            q3v_submit(v, obj);
            q3_release(obj);
        }
        Q3RETURN(obj ? kQ3Success : kQ3Failure);
        return;
    }
    if (q3xr_geometry(v, type, 0, Q3R(0))) {
        Q3RETURN(kQ3Success);
        return;
    }
    q3node *d = q3d_from_guest(Q3R(0), g->s, 1, NULL);
    if (d) {
        if (type == TRIMESH)
            trimesh_bounds(d);
        q3v_begin_object(v, 0);
        g->tess(v, d);
        q3v_end_object(v);
        q3d_free(d, g->s);
    }
    Q3RETURN(d ? kQ3Success : kQ3Failure);
}

static gaddr new_geom(uint32_t type, gaddr data)
{
    gaddr g = q3_data_new(type, data);
    if (g && type == TRIMESH)
        trimesh_bounds(q3_get(g)->data);
    return g;
}

/* ---------------------------------------------------------------------- */
/* Triangulation */

static void normal_of(const float (*pts)[3], const uint32_t *c, uint32_t n, float out[3])
{
    out[0] = out[1] = out[2] = 0;
    for (uint32_t i = 0; i < n; i++) {
        const float *a = pts[c[i]], *b = pts[c[(i + 1) % n]];
        out[0] += (a[1] - b[1]) * (a[2] + b[2]);
        out[1] += (a[2] - b[2]) * (a[0] + b[0]);
        out[2] += (a[0] - b[0]) * (a[1] + b[1]);
    }
}

static bool push_tri(q3ptri **t, uint32_t *nt, uint32_t *cap, uint32_t a, uint32_t b, uint32_t c, uint8_t edges)
{
    if (*nt == *cap) {
        uint32_t ncap = *cap ? *cap * 2 : 16;
        q3ptri *n = realloc(*t, ncap * sizeof *n);
        if (!n)
            return false;
        *t = n;
        *cap = ncap;
    }
    q3ptri *x = &(*t)[(*nt)++];
    memset(x, 0, sizeof *x);
    x->v[0] = a;
    x->v[1] = b;
    x->v[2] = c;
    x->edges = edges;
    return true;
}

typedef struct {
    float x, y;
    uint32_t idx;
    bool real_next; /* the edge to the next is a contour's */
} p2;

static float cross2(const p2 *a, const p2 *b, const p2 *c) { return (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x); }

static bool inside(const p2 *a, const p2 *b, const p2 *c, const p2 *p)
{
    return cross2(a, b, p) >= 0 && cross2(b, c, p) >= 0 && cross2(c, a, p) >= 0;
}

uint32_t q3_triangulate(const float (*pts)[3], const uint32_t *const *contours, const uint32_t *ncontour,
                        uint32_t ncontours, q3ptri **t, uint32_t *nt, uint32_t *cap)
{
    if (!ncontours || ncontour[0] < 3)
        return 0;
    /* The plane: the outside contour's normal; drop its largest axis. */
    float nrm[3];
    normal_of(pts, contours[0], ncontour[0], nrm);
    int drop = fabsf(nrm[0]) > fabsf(nrm[1]) ? (fabsf(nrm[0]) > fabsf(nrm[2]) ? 0 : 2) : (fabsf(nrm[1]) > fabsf(nrm[2]) ? 1 : 2);
    int ax = drop == 0 ? 1 : 0, ay = drop == 2 ? 1 : 2;
    bool flip = nrm[drop] < 0; /* keep the 2D outside counterclockwise */
    uint32_t total = 0;
    for (uint32_t k = 0; k < ncontours; k++)
        total += ncontour[k];
    p2 *poly = malloc((total + 2 * ncontours + 4) * sizeof *poly);
    if (!poly)
        return 0;
    uint32_t n = 0, first = *nt;
    for (uint32_t i = 0; i < ncontour[0]; i++) {
        const float *p = pts[contours[0][i]];
        poly[n++] = (p2){ p[ax], flip ? -p[ay] : p[ay], contours[0][i], true };
    }
    /* Simple convex case (or a triangle): a fan. */
    bool convex = ncontours == 1;
    for (uint32_t i = 0; convex && i < n; i++)
        if (cross2(&poly[i], &poly[(i + 1) % n], &poly[(i + 2) % n]) < 0)
            convex = false;
    if (convex) {
        for (uint32_t i = 1; i + 1 < n; i++)
            push_tri(t, nt, cap, poly[0].idx, poly[i].idx, poly[i + 1].idx,
                     (uint8_t)((i == 1 ? 1 : 0) | 2 | (i + 2 == n ? 4 : 0)));
        free(poly);
        return *nt - first;
    }
    /* Holes, rightmost first, bridged into the outside from their
     * rightmost vertex to the nearest outside vertex it sees along +x. */
    bool *done = calloc(ncontours, 1);
    for (uint32_t h = 1; done && h < ncontours; h++) {
        uint32_t best = 0, bi = 0;
        float bx = -INFINITY;
        for (uint32_t k = 1; k < ncontours; k++) {
            if (done[k] || ncontour[k] < 3)
                continue;
            for (uint32_t i = 0; i < ncontour[k]; i++) {
                float x = pts[contours[k][i]][ax];
                if (x > bx) {
                    bx = x;
                    best = k;
                    bi = i;
                }
            }
        }
        if (!best)
            break;
        done[best] = true;
        const float *hp = pts[contours[best][bi]];
        p2 hv = { hp[ax], flip ? -hp[ay] : hp[ay], 0, false };
        /* The outside vertex to bridge to: the closest to the right with
         * no edge between. */
        uint32_t to = 0;
        float bd = INFINITY;
        for (uint32_t i = 0; i < n; i++) {
            if (poly[i].x < hv.x)
                continue;
            float dx = poly[i].x - hv.x, dy = poly[i].y - hv.y, dd = dx * dx + dy * dy;
            if (dd < bd) {
                bd = dd;
                to = i;
            }
        }
        /* The hole, clockwise, from bi round to bi, then back to the
         * bridging vertex. */
        uint32_t hn = ncontour[best];
        p2 *hole = malloc((hn + 2) * sizeof *hole);
        if (!hole)
            break;
        float hnrm[3];
        normal_of(pts, contours[best], hn, hnrm);
        bool hrev = (hnrm[drop] < 0) == flip; /* holes go the other way round */
        for (uint32_t i = 0; i <= hn; i++) {
            uint32_t j = hrev ? (bi + hn - i % hn) % hn : (bi + i) % hn;
            const float *p = pts[contours[best][j]];
            hole[i] = (p2){ p[ax], flip ? -p[ay] : p[ay], contours[best][j], i < hn };
        }
        hole[hn].real_next = false;
        p2 back = poly[to];
        back.real_next = poly[to].real_next;
        memmove(&poly[to + 1 + hn + 2], &poly[to + 1], (n - to - 1) * sizeof *poly);
        poly[to].real_next = false;
        memcpy(&poly[to + 1], hole, (hn + 1) * sizeof *hole);
        poly[to + 1 + hn + 1] = back;
        n += hn + 2;
        free(hole);
    }
    free(done);
    /* Ear clipping. */
    uint32_t guard = 0;
    while (n > 3 && guard++ < total * total + 16) {
        bool cut = false;
        for (uint32_t i = 0; i < n && !cut; i++) {
            const p2 *a = &poly[(i + n - 1) % n], *b = &poly[i], *c = &poly[(i + 1) % n];
            if (cross2(a, b, c) <= 0)
                continue;
            bool ear = true;
            for (uint32_t j = 0; j < n && ear; j++) {
                const p2 *p = &poly[j];
                if (p == a || p == b || p == c || p->idx == a->idx || p->idx == b->idx || p->idx == c->idx)
                    continue;
                if (inside(a, b, c, p))
                    ear = false;
            }
            if (!ear)
                continue;
            push_tri(t, nt, cap, a->idx, b->idx, c->idx, (uint8_t)((a->real_next ? 1 : 0) | (b->real_next ? 2 : 0)));
            poly[(i + n - 1) % n].real_next = false;
            memmove(&poly[i], &poly[i + 1], (n - i - 1) * sizeof *poly);
            n--;
            cut = true;
        }
        if (!cut) { /* degenerate: fan what is left */
            for (uint32_t i = 1; i + 1 < n; i++)
                push_tri(t, nt, cap, poly[0].idx, poly[i].idx, poly[i + 1].idx, 0);
            n = 0;
        }
    }
    if (n == 3)
        push_tri(t, nt, cap, poly[0].idx, poly[1].idx, poly[2].idx,
                 (uint8_t)((poly[0].real_next ? 1 : 0) | (poly[1].real_next ? 2 : 0) | (poly[2].real_next ? 4 : 0)));
    free(poly);
    return *nt - first;
}

/* ---------------------------------------------------------------------- */
/* Drawing them */

static void vtx_from(q3pv *pv, const uint32_t *w)
{
    memset(pv, 0, sizeof *pv);
    for (int k = 0; k < 3; k++)
        pv->p[k] = q3d_f(w, k);
    q3attrs_fill(&pv->a, w[3]);
}

static q3pv *vertices_of(const q3node *vs)
{
    if (!vs || !vs->n)
        return NULL;
    q3pv *pv = calloc(vs->n, sizeof *pv);
    for (uint32_t i = 0; pv && i < vs->n; i++)
        vtx_from(&pv[i], vs->w + i * 4);
    return pv;
}

static void tess_triangle(q3view *v, const q3node *d)
{
    q3pv pv[3];
    for (int i = 0; i < 3; i++)
        vtx_from(&pv[i], d->w + i * 4);
    q3ptri t;
    memset(&t, 0, sizeof t);
    t.v[1] = 1;
    t.v[2] = 2;
    t.edges = 7;
    q3v_triangles(v, pv, 3, &t, 1, d->w[12]);
}

static void polygon_of(q3view *v, const q3pv *pv, uint32_t n, gaddr set)
{
    if (n < 3)
        return;
    float (*pts)[3] = malloc(n * sizeof *pts);
    uint32_t *idx = malloc(n * sizeof *idx);
    q3ptri *t = NULL;
    uint32_t nt = 0, cap = 0;
    if (pts && idx) {
        for (uint32_t i = 0; i < n; i++) {
            memcpy(pts[i], pv[i].p, sizeof pts[i]);
            idx[i] = i;
        }
        const uint32_t *cs[1] = { idx };
        q3_triangulate((const float (*)[3])pts, cs, &n, 1, &t, &nt, &cap);
        q3v_triangles(v, pv, n, t, nt, set);
    }
    free(t);
    free(pts);
    free(idx);
}

static void tess_polygon(q3view *v, const q3node *d)
{
    q3node *vs = q3d_kid(d, &polygon_s, 0, 0);
    q3pv *pv = vertices_of(vs);
    if (pv)
        polygon_of(v, pv, vs->n, d->w[2]);
    free(pv);
}

static void tess_gpoly(q3view *v, const q3node *d)
{
    q3node *cs = q3d_kid(d, &gpoly_s, 0, 0);
    if (!cs || !cs->n)
        return;
    uint32_t total = 0;
    for (uint32_t k = 0; k < cs->n; k++) {
        q3node *vs = q3d_kid(cs, &contour_s, k, 0);
        total += vs ? vs->n : 0;
    }
    q3pv *pv = calloc(total + 1, sizeof *pv);
    float (*pts)[3] = calloc(total + 1, sizeof *pts);
    uint32_t **cidx = calloc(cs->n, sizeof *cidx), *cn = calloc(cs->n, sizeof *cn);
    uint32_t at = 0;
    for (uint32_t k = 0; pv && pts && cidx && cn && k < cs->n; k++) {
        q3node *vs = q3d_kid(cs, &contour_s, k, 0);
        cn[k] = vs ? vs->n : 0;
        cidx[k] = calloc(cn[k] + 1, sizeof **cidx);
        for (uint32_t i = 0; cidx[k] && i < cn[k]; i++) {
            vtx_from(&pv[at], vs->w + i * 4);
            memcpy(pts[at], pv[at].p, sizeof pts[at]);
            cidx[k][i] = at++;
        }
    }
    q3ptri *t = NULL;
    uint32_t nt = 0, cap = 0;
    if (pv && pts && cidx && cn) {
        q3_triangulate((const float (*)[3])pts, (const uint32_t *const *)cidx, cn, cs->n, &t, &nt, &cap);
        q3v_triangles(v, pv, at, t, nt, d->w[3]);
    }
    for (uint32_t k = 0; cidx && k < cs->n; k++)
        free(cidx[k]);
    free(cidx);
    free(cn);
    free(t);
    free(pts);
    free(pv);
}

static void tess_point(q3view *v, const q3node *d)
{
    q3pv pv;
    vtx_from(&pv, d->w);
    q3v_points(v, &pv, 1, 0);
}

static void tess_line(q3view *v, const q3node *d)
{
    q3pv pv[2];
    vtx_from(&pv[0], d->w);
    vtx_from(&pv[1], d->w + 4);
    q3v_lines(v, pv, 1, d->w[8]);
}

static void tess_polyline(q3view *v, const q3node *d)
{
    q3node *vs = q3d_kid(d, &polyline_s, 0, 0), *segs = q3d_kid(d, &polyline_s, 0, 1);
    if (!vs || vs->n < 2)
        return;
    q3pv *pv = calloc(2 * (vs->n - 1), sizeof *pv);
    for (uint32_t i = 0; pv && i + 1 < vs->n; i++) {
        vtx_from(&pv[2 * i], vs->w + i * 4);
        vtx_from(&pv[2 * i + 1], vs->w + (i + 1) * 4);
        if (segs && i < segs->n) {
            q3attrs_fill(&pv[2 * i].a, segs->w[i]);
            q3attrs_fill(&pv[2 * i + 1].a, segs->w[i]);
        }
    }
    if (pv)
        q3v_lines(v, pv, vs->n - 1, d->w[3]);
    free(pv);
}

static void tess_trigrid(q3view *v, const q3node *d)
{
    uint32_t rows = d->w[0], cols = d->w[1];
    q3node *vs = q3d_kid(d, &trigrid_s, 0, 0), *facets = q3d_kid(d, &trigrid_s, 0, 1);
    if (!vs || rows < 2 || cols < 2 || vs->n < rows * cols)
        return;
    q3pv *pv = vertices_of(vs);
    q3ptri *t = calloc(2 * (rows - 1) * (cols - 1), sizeof *t);
    uint32_t nt = 0;
    for (uint32_t r = 0; pv && t && r + 1 < rows; r++)
        for (uint32_t c = 0; c + 1 < cols; c++) {
            uint32_t a = r * cols + c, b = a + 1, cc = a + cols, dd = cc + 1;
            /* The diagonal alternates, as a checkerboard. */
            bool alt = (r + c) & 1;
            uint32_t tri[2][3] = { { a, cc, alt ? dd : b }, { alt ? a : b, alt ? dd : cc, alt ? b : dd } };
            if (alt) {
                tri[0][0] = a, tri[0][1] = cc, tri[0][2] = dd;
                tri[1][0] = a, tri[1][1] = dd, tri[1][2] = b;
            } else {
                tri[0][0] = a, tri[0][1] = cc, tri[0][2] = b;
                tri[1][0] = b, tri[1][1] = cc, tri[1][2] = dd;
            }
            for (int k = 0; k < 2; k++) {
                q3ptri *x = &t[nt];
                memcpy(x->v, tri[k], sizeof x->v);
                x->edges = alt ? (uint8_t)(k ? 6 : 3) : (uint8_t)(k ? 6 : 5);
                if (facets && nt < facets->n)
                    q3attrs_fill(&x->a, facets->w[nt]);
                nt++;
            }
        }
    if (pv && t)
        q3v_triangles(v, pv, vs->n, t, nt, d->w[4]);
    free(pv);
    free(t);
}

static void tess_polyhedron(q3view *v, const q3node *d)
{
    q3node *vs = q3d_kid(d, &polyhedron_s, 0, 0), *ts = q3d_kid(d, &polyhedron_s, 0, 2);
    if (!vs || !ts || !ts->n)
        return;
    q3pv *pv = vertices_of(vs);
    q3ptri *t = calloc(ts->n, sizeof *t);
    uint32_t nt = 0;
    for (uint32_t i = 0; pv && t && i < ts->n; i++) {
        const uint32_t *w = ts->w + i * 5;
        if (w[0] >= vs->n || w[1] >= vs->n || w[2] >= vs->n)
            continue;
        q3ptri *x = &t[nt++];
        memcpy(x->v, w, 12);
        x->edges = (uint8_t)(w[3] & 7);
        q3attrs_fill(&x->a, w[4]);
    }
    if (pv && t)
        q3v_triangles(v, pv, vs->n, t, nt, d->w[6]);
    free(pv);
    free(t);
}

/* A trimesh attribute array's element i into a. */
static void trimesh_attr(q3attrs *a, const q3node *arr, const q3schema *s, uint32_t i)
{
    for (uint32_t k = 0; arr && k < arr->n; k++) {
        int32_t type = (int32_t)arr->w[k * 3];
        q3node *data = q3d_kid(arr, s, k, 0), *use = q3d_kid(arr, s, k, 1);
        int32_t size = q3_element_size(type);
        if (!data || size <= 0 || (uint64_t)(i + 1) * (uint32_t)size > data->nbytes)
            continue;
        if (use && i < use->nbytes && !use->bytes[i])
            continue;
        const uint8_t *b = data->bytes + (size_t)i * (size_t)size;
        float f[6];
        for (int j = 0; j < size / 4 && j < 6; j++) {
            uint32_t u = (uint32_t)b[4 * j] << 24 | (uint32_t)b[4 * j + 1] << 16 | (uint32_t)b[4 * j + 2] << 8 | b[4 * j + 3];
            memcpy(&f[j], &u, 4);
        }
        switch (type) {
        case kQ3AttributeTypeNormal:
            if (!(a->has & QA_N)) { memcpy(a->n, f, 12); a->has |= QA_N; }
            break;
        case kQ3AttributeTypeSurfaceUV:
        case kQ3AttributeTypeShadingUV:
            if (!(a->has & QA_UV)) { memcpy(a->uv, f, 8); a->has |= QA_UV; }
            break;
        case kQ3AttributeTypeDiffuseColor:
            if (!(a->has & QA_KD)) { memcpy(a->kd, f, 12); a->has |= QA_KD; }
            break;
        case kQ3AttributeTypeSpecularColor:
            if (!(a->has & QA_KS)) { memcpy(a->ks, f, 12); a->has |= QA_KS; }
            break;
        case kQ3AttributeTypeTransparencyColor:
            if (!(a->has & QA_KT)) { memcpy(a->kt, f, 12); a->has |= QA_KT; }
            break;
        case kQ3AttributeTypeSpecularControl:
            if (!(a->has & QA_SPC)) { a->spc = f[0]; a->has |= QA_SPC; }
            break;
        case kQ3AttributeTypeAmbientCoefficient:
            if (!(a->has & QA_AMB)) { a->amb = f[0]; a->has |= QA_AMB; }
            break;
        case kQ3AttributeTypeHighlightState:
            if (!(a->has & QA_HILITE)) { a->hilite = f[0] != 0; a->has |= QA_HILITE; }
            break;
        default: break;
        }
    }
}

static void tess_trimesh(q3view *v, const q3node *d)
{
    q3node *tris = q3d_kid(d, &trimesh_s, 0, 1), *tattr = q3d_kid(d, &trimesh_s, 0, 2),
           *pts = q3d_kid(d, &trimesh_s, 0, 5), *vattr = q3d_kid(d, &trimesh_s, 0, 6);
    if (!tris || !pts || !tris->n || !pts->n)
        return;
    q3pv *pv = calloc(pts->n, sizeof *pv);
    q3ptri *t = calloc(tris->n, sizeof *t);
    uint32_t nt = 0;
    for (uint32_t i = 0; pv && i < pts->n; i++) {
        for (int k = 0; k < 3; k++)
            pv[i].p[k] = q3d_f(pts->w, (int)(i * 3) + k);
        trimesh_attr(&pv[i].a, vattr, &tmvtx_s, i);
    }
    for (uint32_t i = 0; pv && t && i < tris->n; i++) {
        const uint32_t *w = tris->w + i * 3;
        if (w[0] >= pts->n || w[1] >= pts->n || w[2] >= pts->n)
            continue;
        q3ptri *x = &t[nt++];
        memcpy(x->v, w, 12);
        x->edges = 7;
        trimesh_attr(&x->a, tattr, &tmtri_s, i);
    }
    if (pv && t)
        q3v_triangles(v, pv, pts->n, t, nt, d->w[0]);
    free(pv);
    free(t);
}

static void add3(float *o, const float *a, float s, const float *b)
{
    for (int k = 0; k < 3; k++)
        o[k] = a[k] + s * b[k];
}

static void vec(const q3node *d, int off, float out[3])
{
    for (int k = 0; k < 3; k++)
        out[k] = q3d_f(d->w, off + k);
}

static void tess_box(q3view *v, const q3node *d)
{
    float o[3], up[3], maj[3], min[3];
    vec(d, 0, o);
    vec(d, 3, up);
    vec(d, 6, maj);
    vec(d, 9, min);
    /* Corner i: origin + bit 0 minor + bit 1 orientation + bit 2 major. */
    q3pv pv[24];
    memset(pv, 0, sizeof pv);
    /* Faces outward and counterclockwise, the order QuickDraw 3D's face
     * attribute sets are in: -minor, +minor... as left, right, front,
     * back, top, bottom. */
    static const int faces[6][4] = {
        { 0, 4, 6, 2 }, /* minor = 0 */
        { 1, 3, 7, 5 }, /* minor = 1 */
        { 4, 5, 7, 6 }, /* major = 1 */
        { 0, 2, 3, 1 }, /* major = 0 */
        { 2, 6, 7, 3 }, /* orientation = 1 */
        { 0, 1, 5, 4 }, /* orientation = 0 */
    };
    q3node *fsets = q3d_kid(d, &box_s, 0, 0);
    q3ptri t[12];
    memset(t, 0, sizeof t);
    for (int f = 0; f < 6; f++) {
        for (int k = 0; k < 4; k++) {
            int cidx = faces[f][k];
            float *p = pv[f * 4 + k].p;
            memcpy(p, o, sizeof o);
            if (cidx & 1)
                add3(p, p, 1, min);
            if (cidx & 2)
                add3(p, p, 1, up);
            if (cidx & 4)
                add3(p, p, 1, maj);
            static const float uvs[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
            memcpy(pv[f * 4 + k].a.uv, uvs[k], 8);
            pv[f * 4 + k].a.has = QA_UV;
        }
        uint32_t b = (uint32_t)f * 4;
        t[f * 2] = (q3ptri){ { b, b + 1, b + 2 }, 3, { 0 } };
        t[f * 2 + 1] = (q3ptri){ { b, b + 2, b + 3 }, 6, { 0 } };
        if (fsets && (uint32_t)f < fsets->n) {
            q3attrs_fill(&t[f * 2].a, fsets->w[f]);
            q3attrs_fill(&t[f * 2 + 1].a, fsets->w[f]);
        }
    }
    q3v_triangles(v, pv, 24, t, 12, d->w[13]);
}

/* A parametric surface: p(u, v) and its normal on a grid of nu by nv
 * quads, drawn as triangles with face attributes fa. */
typedef void (*surf_fn)(const q3node *d, float u, float v, float p[3], float n[3]);

/* Which triangles of a surface stay, by their middle's (u, v): a NURB
 * patch's trim loops (NULL: all). */
static bool (*surface_keep)(float u, float v);

static void surface(q3view *v, const q3node *d, surf_fn fn, int nu, int nv, float u0, float u1, float v0, float v1,
                    gaddr face_set, gaddr geom_set)
{
    if (nu < 1 || nv < 1)
        return;
    uint32_t np = (uint32_t)((nu + 1) * (nv + 1));
    q3pv *pv = calloc(np, sizeof *pv);
    q3ptri *t = calloc((size_t)nu * (size_t)nv * 2, sizeof *t);
    if (!pv || !t) {
        free(pv);
        free(t);
        return;
    }
    for (int j = 0; j <= nv; j++)
        for (int i = 0; i <= nu; i++) {
            q3pv *x = &pv[j * (nu + 1) + i];
            float uu = u0 + (u1 - u0) * (float)i / (float)nu, vv = v0 + (v1 - v0) * (float)j / (float)nv;
            fn(d, uu, vv, x->p, x->a.n);
            float len = sqrtf(x->a.n[0] * x->a.n[0] + x->a.n[1] * x->a.n[1] + x->a.n[2] * x->a.n[2]);
            x->a.has = QA_UV;
            if (len > 1e-12f) {
                for (int k = 0; k < 3; k++)
                    x->a.n[k] /= len;
                x->a.has |= QA_N;
            }
            x->a.uv[0] = uu;
            x->a.uv[1] = vv;
        }
    uint32_t nt = 0;
    q3attrs fa;
    memset(&fa, 0, sizeof fa);
    q3attrs_fill(&fa, face_set);
    for (int j = 0; j < nv; j++)
        for (int i = 0; i < nu; i++) {
            uint32_t a = (uint32_t)(j * (nu + 1) + i), b = a + 1, c = a + (uint32_t)nu + 1, e = c + 1;
            if (surface_keep) {
                float du = (u1 - u0) / (float)nu, dv = (v1 - v0) / (float)nv;
                float uu = u0 + du * (float)i, vv = v0 + dv * (float)j;
                if (surface_keep(uu + du * 2 / 3, vv + dv / 3))
                    t[nt++] = (q3ptri){ { a, b, e }, 0, fa };
                if (surface_keep(uu + du / 3, vv + dv * 2 / 3))
                    t[nt++] = (q3ptri){ { a, e, c }, 0, fa };
                continue;
            }
            t[nt] = (q3ptri){ { a, b, e }, 3, fa };
            t[nt + 1] = (q3ptri){ { a, e, c }, 6, fa };
            nt += 2;
        }
    q3v_triangles(v, pv, np, t, nt, geom_set);
    free(pv);
    free(t);
}

static float len3(const float *a) { return sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }

static void cross3(const float *a, const float *b, float *o)
{
    float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x;
    o[1] = y;
    o[2] = z;
}

/* Tessellation of a curve of size `len`: the subdivision style's. */
static int steps(q3view *v, float len, int fallback) { return q3v_segments(v, len, fallback); }

static void ellipsoid_at(const q3node *d, float u, float vv, float p[3], float n[3])
{
    float o[3], up[3], maj[3], min[3];
    vec(d, 0, o);
    vec(d, 3, up);
    vec(d, 6, maj);
    vec(d, 9, min);
    float th = u * 2 * (float)M_PI, ph = vv * (float)M_PI;
    float ring[3], dth[3], dph[3];
    for (int k = 0; k < 3; k++) {
        ring[k] = cosf(th) * maj[k] + sinf(th) * min[k];
        p[k] = o[k] - cosf(ph) * up[k] + sinf(ph) * ring[k];
        dth[k] = sinf(ph) * (-sinf(th) * maj[k] + cosf(th) * min[k]);
        dph[k] = sinf(ph) * up[k] + cosf(ph) * ring[k];
    }
    cross3(dth, dph, n);
    if (len3(n) < 1e-9f) /* the poles */
        for (int k = 0; k < 3; k++)
            n[k] = (vv < 0.5f ? -1.0f : 1.0f) * up[k];
}

static void tess_ellipsoid(q3view *v, const q3node *d)
{
    float maj[3];
    vec(d, 6, maj);
    int n = steps(v, 2 * (float)M_PI * len3(maj), 24);
    surface(v, d, ellipsoid_at, n, n / 2 > 2 ? n / 2 : 3, q3d_f(d->w, 12), q3d_f(d->w, 13), q3d_f(d->w, 14),
            q3d_f(d->w, 15), 0, d->w[18]);
}

static void torus_at(const q3node *d, float u, float vv, float p[3], float n[3])
{
    float o[3], up[3], maj[3], min[3];
    vec(d, 0, o);
    vec(d, 3, up);
    vec(d, 6, maj);
    vec(d, 9, min);
    float ratio = q3d_f(d->w, 12), th = u * 2 * (float)M_PI, ph = vv * 2 * (float)M_PI;
    float ring[3], r = 0;
    for (int k = 0; k < 3; k++)
        ring[k] = cosf(th) * maj[k] + sinf(th) * min[k];
    r = len3(ring);
    float dir[3] = { ring[0] / (r ? r : 1), ring[1] / (r ? r : 1), ring[2] / (r ? r : 1) };
    float tube = ratio * len3(up);
    for (int k = 0; k < 3; k++) {
        p[k] = o[k] + ring[k] + cosf(ph) * tube * dir[k] + sinf(ph) * up[k];
        n[k] = cosf(ph) * dir[k] * len3(up) + sinf(ph) * up[k] / (len3(up) ? len3(up) : 1) * tube;
    }
}

static void tess_torus(q3view *v, const q3node *d)
{
    float maj[3];
    vec(d, 6, maj);
    int n = steps(v, 2 * (float)M_PI * len3(maj), 24);
    surface(v, d, torus_at, n, n / 2 > 3 ? n / 2 : 4, q3d_f(d->w, 13), q3d_f(d->w, 14), q3d_f(d->w, 15),
            q3d_f(d->w, 16), 0, d->w[19]);
}

/* A cone or cylinder's side: at u around, v from the bottom (0) to the
 * top; the cone's top a point. */
static bool side_cone;
static void side_at(const q3node *d, float u, float vv, float p[3], float n[3])
{
    float o[3], up[3], maj[3], min[3];
    vec(d, 0, o);
    vec(d, 3, up);
    vec(d, 6, maj);
    vec(d, 9, min);
    float th = u * 2 * (float)M_PI, s = side_cone ? 1 - vv : 1;
    float ring[3], dth[3], dv[3];
    for (int k = 0; k < 3; k++) {
        ring[k] = cosf(th) * maj[k] + sinf(th) * min[k];
        p[k] = o[k] + vv * up[k] + s * ring[k];
        dth[k] = -sinf(th) * maj[k] + cosf(th) * min[k];
        dv[k] = up[k] - (side_cone ? ring[k] : 0);
    }
    cross3(dth, dv, n);
}

/* A cap (a disk in the plane of major and minor at `at`), facing along
 * `facing` (+1 or -1 times orientation). */
static void cap(q3view *v, const float *center, const float *maj, const float *min, const float *axis, int n,
                float u0, float u1, gaddr face_set, gaddr geom_set)
{
    q3pv *pv = calloc((size_t)n + 2, sizeof *pv);
    q3ptri *t = calloc((size_t)n, sizeof *t);
    if (!pv || !t) {
        free(pv);
        free(t);
        return;
    }
    float nrm[3];
    cross3(maj, min, nrm);
    if (nrm[0] * axis[0] + nrm[1] * axis[1] + nrm[2] * axis[2] < 0)
        for (int k = 0; k < 3; k++)
            nrm[k] = -nrm[k];
    memcpy(pv[0].p, center, 12);
    for (int i = 0; i <= n; i++) {
        float th = (u0 + (u1 - u0) * (float)i / (float)n) * 2 * (float)M_PI;
        for (int k = 0; k < 3; k++)
            pv[i + 1].p[k] = center[k] + cosf(th) * maj[k] + sinf(th) * min[k];
        pv[i + 1].a.uv[0] = 0.5f + 0.5f * cosf(th);
        pv[i + 1].a.uv[1] = 0.5f + 0.5f * sinf(th);
    }
    pv[0].a.uv[0] = pv[0].a.uv[1] = 0.5f;
    for (int i = 0; i < n + 2; i++) {
        memcpy(pv[i].a.n, nrm, 12);
        pv[i].a.has = QA_N | QA_UV;
    }
    q3attrs fa;
    memset(&fa, 0, sizeof fa);
    q3attrs_fill(&fa, face_set);
    bool flipped = false;
    float c2[3];
    cross3(maj, min, c2);
    flipped = c2[0] * axis[0] + c2[1] * axis[1] + c2[2] * axis[2] < 0;
    for (int i = 0; i < n; i++)
        t[i] = flipped ? (q3ptri){ { 0, (uint32_t)i + 2, (uint32_t)i + 1 }, 2, fa }
                       : (q3ptri){ { 0, (uint32_t)i + 1, (uint32_t)i + 2 }, 2, fa };
    q3v_triangles(v, pv, (uint32_t)n + 2, t, (uint32_t)n, geom_set);
    free(pv);
    free(t);
}

enum { kQ3EndCapMaskTop = 1, kQ3EndCapMaskBottom = 2 };

static void tess_cone(q3view *v, const q3node *d)
{
    float o[3], up[3], maj[3], min[3], down[3];
    vec(d, 0, o);
    vec(d, 3, up);
    vec(d, 6, maj);
    vec(d, 9, min);
    int n = steps(v, 2 * (float)M_PI * len3(maj), 24);
    side_cone = true;
    surface(v, d, side_at, n, 4, q3d_f(d->w, 12), q3d_f(d->w, 13), q3d_f(d->w, 14), q3d_f(d->w, 15), d->w[18],
            d->w[20]);
    for (int k = 0; k < 3; k++)
        down[k] = -up[k];
    if (d->w[16] & kQ3EndCapMaskBottom)
        cap(v, o, maj, min, down, n, q3d_f(d->w, 12), q3d_f(d->w, 13), d->w[19], d->w[20]);
}

static void tess_cylinder(q3view *v, const q3node *d)
{
    float o[3], up[3], maj[3], min[3], top[3], down[3];
    vec(d, 0, o);
    vec(d, 3, up);
    vec(d, 6, maj);
    vec(d, 9, min);
    int n = steps(v, 2 * (float)M_PI * len3(maj), 24);
    side_cone = false;
    surface(v, d, side_at, n, 1, q3d_f(d->w, 12), q3d_f(d->w, 13), q3d_f(d->w, 14), q3d_f(d->w, 15), d->w[19],
            d->w[21]);
    for (int k = 0; k < 3; k++) {
        top[k] = o[k] + up[k];
        down[k] = -up[k];
    }
    if (d->w[16] & kQ3EndCapMaskBottom)
        cap(v, o, maj, min, down, n, q3d_f(d->w, 12), q3d_f(d->w, 13), d->w[20], d->w[21]);
    if (d->w[16] & kQ3EndCapMaskTop)
        cap(v, top, maj, min, up, n, q3d_f(d->w, 12), q3d_f(d->w, 13), d->w[18], d->w[21]);
}

static void tess_disk(q3view *v, const q3node *d)
{
    float o[3], maj[3], min[3], axis[3];
    vec(d, 0, o);
    vec(d, 3, maj);
    vec(d, 6, min);
    cross3(maj, min, axis);
    int n = steps(v, 2 * (float)M_PI * len3(maj), 24);
    cap(v, o, maj, min, axis, n, q3d_f(d->w, 9), q3d_f(d->w, 10), 0, d->w[13]);
}

static void tess_ellipse(q3view *v, const q3node *d)
{
    float o[3], maj[3], min[3];
    vec(d, 0, o);
    vec(d, 3, maj);
    vec(d, 6, min);
    int n = steps(v, 2 * (float)M_PI * len3(maj), 32);
    float u0 = q3d_f(d->w, 9), u1 = q3d_f(d->w, 10);
    q3pv *pv = calloc((size_t)n * 2, sizeof *pv);
    for (int i = 0; pv && i < n; i++)
        for (int e = 0; e < 2; e++) {
            float th = (u0 + (u1 - u0) * (float)(i + e) / (float)n) * 2 * (float)M_PI;
            for (int k = 0; k < 3; k++)
                pv[2 * i + e].p[k] = o[k] + cosf(th) * maj[k] + sinf(th) * min[k];
        }
    if (pv)
        q3v_lines(v, pv, (uint32_t)n, d->w[11]);
    free(pv);
}

/* NURBS: the basis functions by Cox-de Boor. */
static float basis(const float *knots, int i, int k, float t)
{
    if (k == 1) {
        if (knots[i] <= t && t < knots[i + 1])
            return 1;
        return 0;
    }
    float a = 0, b = 0, d1 = knots[i + k - 1] - knots[i], d2 = knots[i + k] - knots[i + 1];
    if (d1 > 0)
        a = (t - knots[i]) / d1 * basis(knots, i, k - 1, t);
    if (d2 > 0)
        b = (knots[i + k] - t) / d2 * basis(knots, i + 1, k - 1, t);
    return a + b;
}

/* Evaluate at t within the knots' range (the end clamped in). */
static float clamp_t(const float *knots, int order, int n, float t)
{
    float lo = knots[order - 1], hi = knots[n];
    if (t >= hi)
        t = hi - (hi - lo) * 1e-5f;
    if (t < lo)
        t = lo;
    return t;
}

static void tess_ncurve(q3view *v, const q3node *d)
{
    int order = (int)d->w[0], n = (int)d->w[1];
    q3node *cp = q3d_kid(d, &ncurve_s, 0, 0), *kn = q3d_kid(d, &ncurve_s, 0, 1);
    if (order < 1 || n < order || !cp || !kn || cp->n < (uint32_t)n || kn->n < (uint32_t)(order + n))
        return;
    float *knots = malloc((size_t)(order + n) * sizeof *knots);
    if (!knots)
        return;
    for (int i = 0; i < order + n; i++)
        knots[i] = q3d_f(kn->w, i);
    int seg = q3v_segments(v, 0, 8 * n);
    q3pv *pv = calloc((size_t)seg * 2, sizeof *pv);
    float prev[3] = { 0 };
    for (int s = 0; pv && s <= seg; s++) {
        float t = clamp_t(knots, order, n, knots[order - 1] + (knots[n] - knots[order - 1]) * (float)s / (float)seg);
        float p[4] = { 0 };
        for (int i = 0; i < n; i++) {
            float b = basis(knots, i, order, t);
            for (int k = 0; k < 4; k++)
                p[k] += b * q3d_f(cp->w, i * 4 + k);
        }
        float q[3] = { p[0] / (p[3] ? p[3] : 1), p[1] / (p[3] ? p[3] : 1), p[2] / (p[3] ? p[3] : 1) };
        if (s) {
            memcpy(pv[2 * (s - 1)].p, prev, 12);
            memcpy(pv[2 * (s - 1) + 1].p, q, 12);
        }
        memcpy(prev, q, 12);
    }
    if (pv)
        q3v_lines(v, pv, (uint32_t)seg, d->w[4]);
    free(pv);
    free(knots);
}

static const q3node *patch_d;
static float *patch_uk, *patch_vk;

static void patch_point(const q3node *d, float u, float vv, float out[3])
{
    int uo = (int)d->w[0], vo = (int)d->w[1], rows = (int)d->w[2], cols = (int)d->w[3];
    q3node *cp = q3d_kid(d, &npatch_s, 0, 0);
    u = clamp_t(patch_uk, uo, cols, patch_uk[uo - 1] + (patch_uk[cols] - patch_uk[uo - 1]) * u);
    vv = clamp_t(patch_vk, vo, rows, patch_vk[vo - 1] + (patch_vk[rows] - patch_vk[vo - 1]) * vv);
    float p[4] = { 0 };
    for (int r = 0; r < rows; r++) {
        float bv = basis(patch_vk, r, vo, vv);
        if (bv == 0)
            continue;
        for (int c = 0; c < cols; c++) {
            float b = bv * basis(patch_uk, c, uo, u);
            for (int k = 0; k < 4; k++)
                p[k] += b * q3d_f(cp->w, (r * cols + c) * 4 + k);
        }
    }
    for (int k = 0; k < 3; k++)
        out[k] = p[k] / (p[3] ? p[3] : 1);
}

static void patch_at(const q3node *d, float u, float vv, float p[3], float n[3])
{
    const float e = 1e-3f;
    float pu[3], pv2[3];
    patch_point(d, u, vv, p);
    patch_point(d, u + e <= 1 ? u + e : u - e, vv, pu);
    patch_point(d, u, vv + e <= 1 ? vv + e : vv - e, pv2);
    float du[3], dv[3];
    for (int k = 0; k < 3; k++) {
        du[k] = (pu[k] - p[k]) * (u + e <= 1 ? 1 : -1);
        dv[k] = (pv2[k] - p[k]) * (vv + e <= 1 ? 1 : -1);
    }
    cross3(du, dv, n);
}

/* Trim loops as polygons in the patch's (u, v) from 0 to 1, each with
 * its winding: counterclockwise loops bound what stays, clockwise ones
 * cut holes; with only holes, the rest of the patch stays. */
typedef struct {
    float (*p)[2];
    int n;
} trimpoly;
static trimpoly *trims;
static int ntrims;
static bool trims_bounding;

static float poly_area(const trimpoly *t)
{
    float a = 0;
    for (int i = 0; i < t->n; i++) {
        const float *p = t->p[i], *q = t->p[(i + 1) % t->n];
        a += p[0] * q[1] - q[0] * p[1];
    }
    return a / 2;
}

static int winding(const trimpoly *t, float u, float v)
{
    int w = 0;
    for (int i = 0; i < t->n; i++) {
        const float *a = t->p[i], *b = t->p[(i + 1) % t->n];
        if (a[1] <= v) {
            if (b[1] > v && (b[0] - a[0]) * (v - a[1]) - (u - a[0]) * (b[1] - a[1]) > 0)
                w++;
        } else if (b[1] <= v && (b[0] - a[0]) * (v - a[1]) - (u - a[0]) * (b[1] - a[1]) < 0) {
            w--;
        }
    }
    return w;
}

static bool trim_keep(float u, float v)
{
    int w = trims_bounding ? 0 : 1;
    for (int i = 0; i < ntrims; i++)
        w += winding(&trims[i], u, v);
    return w > 0;
}

/* The patch's trim loops (word 7 their count, 8 the array: each a count
 * of curves and the curves, 2D rational NURBS of order, count, points
 * (x, y, w) and knots) sampled into polygons. */
static void trim_loops(const q3node *d)
{
    q3node *loops = q3d_kid(d, &npatch_s, 0, 3);
    float ulo = patch_uk[(int)d->w[0] - 1], uhi = patch_uk[d->w[3]], vlo = patch_vk[(int)d->w[1] - 1], vhi = patch_vk[d->w[2]];
    if (!loops || uhi <= ulo || vhi <= vlo)
        return;
    trims = calloc(loops->n, sizeof *trims);
    for (uint32_t l = 0; trims && l < loops->n; l++) {
        q3node *curves = q3d_kid(loops, &trimloop_s, l, 0);
        trimpoly *t = &trims[ntrims];
        int cap = 0;
        for (uint32_t c = 0; curves && c < curves->n; c++) {
            const uint32_t *cw = curves->w + c * curves->ew;
            int order = (int)cw[0], n = (int)cw[1];
            q3node *cp = q3d_kid(curves, &trimcurve_s, c, 0), *kn = q3d_kid(curves, &trimcurve_s, c, 1);
            if (order < 1 || n < order || !cp || !kn || cp->n < (uint32_t)n || kn->n < (uint32_t)(order + n))
                continue;
            float *knots = malloc((size_t)(order + n) * sizeof *knots);
            if (!knots)
                continue;
            for (int i = 0; i < order + n; i++)
                knots[i] = q3d_f(kn->w, i);
            int seg = 8 * n;
            for (int k = 0; k <= seg; k++) {
                float tt = clamp_t(knots, order, n, knots[order - 1] + (knots[n] - knots[order - 1]) * (float)k / (float)seg);
                float p[3] = { 0 };
                for (int i = 0; i < n; i++) {
                    float b = basis(knots, i, order, tt);
                    for (int j = 0; j < 3; j++)
                        p[j] += b * q3d_f(cp->w, i * 3 + j);
                }
                if (t->n == cap) {
                    cap = cap ? cap * 2 : 64;
                    float(*np)[2] = realloc(t->p, (size_t)cap * sizeof *np);
                    if (!np)
                        break;
                    t->p = np;
                }
                float w = p[2] ? p[2] : 1;
                t->p[t->n][0] = (p[0] / w - ulo) / (uhi - ulo);
                t->p[t->n][1] = (p[1] / w - vlo) / (vhi - vlo);
                t->n++;
            }
            free(knots);
        }
        if (t->n >= 3) {
            if (poly_area(t) > 0)
                trims_bounding = true;
            ntrims++;
        } else {
            free(t->p);
            t->p = NULL;
            t->n = 0;
        }
    }
}

static void tess_npatch(q3view *v, const q3node *d)
{
    int uo = (int)d->w[0], vo = (int)d->w[1], rows = (int)d->w[2], cols = (int)d->w[3];
    q3node *cp = q3d_kid(d, &npatch_s, 0, 0), *uk = q3d_kid(d, &npatch_s, 0, 1), *vk = q3d_kid(d, &npatch_s, 0, 2);
    if (uo < 1 || vo < 1 || cols < uo || rows < vo || !cp || !uk || !vk || cp->n < (uint32_t)(rows * cols) ||
        uk->n < (uint32_t)(uo + cols) || vk->n < (uint32_t)(vo + rows))
        return;
    patch_uk = malloc((size_t)(uo + cols) * sizeof(float));
    patch_vk = malloc((size_t)(vo + rows) * sizeof(float));
    if (patch_uk && patch_vk) {
        for (int i = 0; i < uo + cols; i++)
            patch_uk[i] = q3d_f(uk->w, i);
        for (int i = 0; i < vo + rows; i++)
            patch_vk[i] = q3d_f(vk->w, i);
        patch_d = d;
        int nu = q3v_segments(v, 0, 4 * cols), nv = q3v_segments(v, 0, 4 * rows);
        trims_bounding = false;
        if (d->w[7])
            trim_loops(d);
        if (ntrims) { /* finer, so the trimmed edges follow the loops */
            nu *= 4;
            nv *= 4;
            surface_keep = trim_keep;
        }
        surface(v, d, patch_at, nu, nv, 0, 1, 0, 1, 0, d->w[9]);
        surface_keep = NULL;
        for (int i = 0; i < ntrims; i++)
            free(trims[i].p);
        free(trims);
        trims = NULL;
        ntrims = 0;
    }
    free(patch_uk);
    free(patch_vk);
    patch_uk = patch_vk = NULL;
}

static void tess_marker(q3view *v, const q3node *d)
{
    uint32_t w = d->w[6], h = d->w[7], rb = d->w[8];
    q3node *img = q3d_kid(d, &marker_s, 0, 0);
    if (!img || !w || !h || w > 4096 || h > 4096 || img->nbytes < rb * h)
        return;
    uint32_t *px = malloc((size_t)w * h * 4);
    if (!px)
        return;
    /* The bitmap's set bits in the marker's diffuse colour (white without). */
    q3attrs a;
    memset(&a, 0, sizeof a);
    q3attrs_fill(&a, d->w[10]);
    uint32_t color = 0xFFFFFFFF;
    if (a.has & QA_KD)
        color = 0xFF000000 | (uint32_t)(fminf(fmaxf(a.kd[0], 0), 1) * 255) << 16 |
                (uint32_t)(fminf(fmaxf(a.kd[1], 0), 1) * 255) << 8 | (uint32_t)(fminf(fmaxf(a.kd[2], 0), 1) * 255);
    bool little = d->w[9] == 1; /* kQ3EndianLittle: bit 0 is the leftmost */
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint8_t byte = img->bytes[y * rb + x / 8];
            bool on = little ? byte & (1 << (x & 7)) : byte & (0x80 >> (x & 7));
            px[y * w + x] = on ? color : 0;
        }
    float p[3] = { q3d_f(d->w, 0), q3d_f(d->w, 1), q3d_f(d->w, 2) };
    q3v_image(v, p, (int32_t)d->w[3], (int32_t)d->w[4], (int)w, (int)h, px, d->w[10]);
    free(px);
}

/* A storage object's bytes (q3_file.c). */
uint8_t *q3_storage_bytes(gaddr storage, uint32_t *size);

uint32_t *q3_storage_pixmap_argb(const uint32_t *w, gaddr storage);

/* A TQ3StoragePixmap's words (image storage, width, height, rowBytes,
 * pixelSize, pixelType, bitOrder, byteOrder) as 0xAARRGGBB pixels. */
uint32_t *q3_storage_pixmap_argb(const uint32_t *w, gaddr storage)
{
    uint32_t width = w[1], height = w[2], rb = w[3], type = w[5], byte_order = w[7];
    uint32_t size = 0;
    uint8_t *bytes = q3_storage_bytes(storage, &size);
    if (!bytes || !width || !height || width > 8192 || height > 8192 || type > 5) {
        free(bytes);
        return NULL;
    }
    int bpp = q3_pixel_bytes((int)type);
    if ((uint64_t)rb * (height - 1) + (uint64_t)width * (uint32_t)bpp > size) {
        free(bytes);
        return NULL;
    }
    uint32_t *px = malloc((size_t)width * height * 4);
    for (uint32_t y = 0; px && y < height; y++)
        for (uint32_t x = 0; x < width; x++) {
            const uint8_t *s = bytes + y * rb + x * (uint32_t)bpp;
            uint8_t b[4];
            for (int k = 0; k < bpp; k++)
                b[k] = byte_order == 1 ? s[bpp - 1 - k] : s[k];
            uint32_t v;
            switch (type) {
            case 0: v = 0xFF000000 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; break;
            case 1: v = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; break;
            case 5: v = 0xFF000000 | (uint32_t)b[0] << 16 | (uint32_t)b[1] << 8 | b[2]; break;
            default: {
                uint32_t h16 = (uint32_t)b[0] << 8 | b[1];
                if (type == 4) {
                    uint32_t r = h16 >> 11 & 31, g = h16 >> 5 & 63, bl = h16 & 31;
                    v = 0xFF000000 | (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (bl << 3 | bl >> 2);
                } else {
                    uint32_t r = h16 >> 10 & 31, g = h16 >> 5 & 31, bl = h16 & 31;
                    v = (type == 3 ? (h16 & 0x8000 ? 0xFF000000u : 0) : 0xFF000000u) | (r << 3 | r >> 2) << 16 |
                        (g << 3 | g >> 2) << 8 | (bl << 3 | bl >> 2);
                }
                break;
            }
            }
            px[y * width + x] = v;
        }
    free(bytes);
    return px;
}

static void tess_pmarker(q3view *v, const q3node *d)
{
    uint32_t *px = q3_storage_pixmap_argb(d->w + 5, d->w[5]);
    if (!px)
        return;
    float p[3] = { q3d_f(d->w, 0), q3d_f(d->w, 1), q3d_f(d->w, 2) };
    q3v_image(v, p, (int32_t)d->w[3], (int32_t)d->w[4], (int)d->w[6], (int)d->w[7], px, d->w[13]);
    free(px);
}

/* ---------------------------------------------------------------------- */
/* The calls */

static void Q3Geometry_GetType(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), Q3_GEOMETRY);
    Q3RETURN(o ? q3_type_below(o->type, Q3_GEOMETRY) : 0);
}

static void Q3Geometry_GetAttributeSet(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), Q3_GEOMETRY);
    const struct geom *g = o ? geom_of(o->type) : NULL;
    if (o && Q3R(1)) {
        gaddr s = g && o->data ? ((q3node *)o->data)->w[g->set_off] : 0;
        if (!g) /* the mesh keeps its own */
            s = q3_mesh_set(o);
        wr32(Q3R(1), s ? q3_ref(s) : 0);
    }
    Q3RETURN(o && Q3R(1) ? kQ3Success : kQ3Failure);
}

static void Q3Geometry_SetAttributeSet(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), Q3_GEOMETRY);
    const struct geom *g = o ? geom_of(o->type) : NULL;
    if (o && g && o->data)
        q3_assign(&((q3node *)o->data)->w[g->set_off], Q3R(1));
    else if (o)
        q3_mesh_set_set(o, Q3R(1));
    q3_edited(o);
    Q3RETURN(o ? kQ3Success : kQ3Failure);
}

static void Q3Geometry_Submit(ppc_cpu *c)
{
    q3view *v = q3v_of(Q3R(1));
    if (v && q3_of(Q3R(0), Q3_GEOMETRY))
        q3v_submit(v, Q3R(0));
    Q3RETURN(v ? kQ3Success : kQ3Failure);
}

/* An element of a vertex array: its position or attribute set. */
static q3node *array_of(gaddr g, uint32_t type, const q3schema *s, int field)
{
    q3node *d = q3_data_of(g, type);
    return d ? q3d_kid(d, s, 0, field) : NULL;
}

static void vertex_pos(ppc_cpu *c, q3node *arr, uint32_t i, gaddr out, bool set, uint32_t ew)
{
    if (!arr || i >= arr->n || !out) {
        q3_error(kQ3ErrorParameterOutOfRange);
        Q3RETURN(kQ3Failure);
        return;
    }
    for (int k = 0; k < 3; k++) {
        if (set)
            arr->w[i * ew + (uint32_t)k] = rd32(out + 4 * (uint32_t)k);
        else
            wr32(out + 4 * (uint32_t)k, arr->w[i * ew + (uint32_t)k]);
    }
    if (set)
        q3_edited(q3_get(Q3R(0)));
    Q3RETURN(kQ3Success);
}

static void vertex_set(ppc_cpu *c, q3node *arr, uint32_t i, gaddr val, bool set, uint32_t ew, uint32_t off)
{
    if (!arr || i >= arr->n || (!set && !val)) {
        q3_error(kQ3ErrorParameterOutOfRange);
        Q3RETURN(kQ3Failure);
        return;
    }
    gaddr *slot = &arr->w[i * ew + off];
    if (set) {
        q3_assign(slot, val);
        q3_edited(q3_get(Q3R(0)));
    } else {
        wr32(val, *slot ? q3_ref(*slot) : 0);
    }
    Q3RETURN(kQ3Success);
}

/* The vertices of a fixed-size vertex array in the top structure. */
static void fixed_vertex(ppc_cpu *c, uint32_t type, uint32_t nverts, bool attr, bool set)
{
    q3node *d = q3_data_of(Q3R(0), type);
    uint32_t i = Q3R(1);
    if (!d || i >= nverts) {
        q3_error(kQ3ErrorParameterOutOfRange);
        Q3RETURN(kQ3Failure);
        return;
    }
    q3node one = { nverts, 4, d->w, NULL, 0, NULL };
    if (attr)
        vertex_set(c, &one, i, Q3R(2), set, 4, 3);
    else
        vertex_pos(c, &one, i, Q3R(2), set, 4);
}

#define GEOM_CALLS(Name, TYPE)                                                                                       \
    static void Q3##Name##_New(ppc_cpu *c) { Q3RETURN(new_geom(TYPE, Q3R(0))); }                                     \
    static void Q3##Name##_Submit(ppc_cpu *c) { submit_now(c, TYPE); }                                               \
    Q3_SETDATA(Q3##Name##_SetData, TYPE)                                                                             \
    Q3_GETDATA(Q3##Name##_GetData, TYPE)                                                                             \
    Q3_EMPTYDATA(Q3##Name##_EmptyData, TYPE)

GEOM_CALLS(Box, BOX)
GEOM_CALLS(Cone, CONE)
GEOM_CALLS(Cylinder, CYLINDER)
GEOM_CALLS(Disk, DISK)
GEOM_CALLS(Ellipse, ELLIPSE)
GEOM_CALLS(Ellipsoid, ELLIPSOID)
GEOM_CALLS(GeneralPolygon, GPOLY)
GEOM_CALLS(Line, LINE)
GEOM_CALLS(Marker, MARKER)
GEOM_CALLS(NURBCurve, NCURVE)
GEOM_CALLS(NURBPatch, NPATCH)
GEOM_CALLS(PixmapMarker, PMARKER)
GEOM_CALLS(Point, POINT)
GEOM_CALLS(Polygon, POLYGON)
GEOM_CALLS(Polyhedron, POLYHEDRON)
GEOM_CALLS(PolyLine, POLYLINE)
GEOM_CALLS(Torus, TORUS)
GEOM_CALLS(Triangle, TRIANGLE)
GEOM_CALLS(TriGrid, TRIGRID)
GEOM_CALLS(TriMesh, TRIMESH)

/* The quadrics' common fields. */
#define AXES(Name, TYPE, ORIENT, MAJ, MIN)                                                                           \
    Q3_SETW(Q3##Name##_SetOrigin, TYPE, 0, 3)                                                                        \
    Q3_GETW(Q3##Name##_GetOrigin, TYPE, 0, 3)                                                                        \
    Q3_SETW(Q3##Name##_SetMajorRadius, TYPE, MAJ, 3)                                                                 \
    Q3_GETW(Q3##Name##_GetMajorRadius, TYPE, MAJ, 3)                                                                 \
    Q3_SETW(Q3##Name##_SetMinorRadius, TYPE, MIN, 3)                                                                 \
    Q3_GETW(Q3##Name##_GetMinorRadius, TYPE, MIN, 3)

AXES(Cone, CONE, 3, 6, 9)
Q3_SETW(Q3Cone_SetOrientation, CONE, 3, 3)
Q3_GETW(Q3Cone_GetOrientation, CONE, 3, 3)
Q3_SETV(Q3Cone_SetCaps, CONE, 16)
Q3_GETW(Q3Cone_GetCaps, CONE, 16, 1)
Q3_SETO(Q3Cone_SetBottomAttributeSet, CONE, 19)
Q3_GETO(Q3Cone_GetBottomAttributeSet, CONE, 19)
Q3_SETO(Q3Cone_SetFaceAttributeSet, CONE, 18)
Q3_GETO(Q3Cone_GetFaceAttributeSet, CONE, 18)

AXES(Cylinder, CYLINDER, 3, 6, 9)
Q3_SETW(Q3Cylinder_SetOrientation, CYLINDER, 3, 3)
Q3_GETW(Q3Cylinder_GetOrientation, CYLINDER, 3, 3)
Q3_SETV(Q3Cylinder_SetCaps, CYLINDER, 16)
Q3_GETW(Q3Cylinder_GetCaps, CYLINDER, 16, 1)
Q3_SETO(Q3Cylinder_SetTopAttributeSet, CYLINDER, 18)
Q3_GETO(Q3Cylinder_GetTopAttributeSet, CYLINDER, 18)
Q3_SETO(Q3Cylinder_SetFaceAttributeSet, CYLINDER, 19)
Q3_GETO(Q3Cylinder_GetFaceAttributeSet, CYLINDER, 19)
Q3_SETO(Q3Cylinder_SetBottomAttributeSet, CYLINDER, 20)
Q3_GETO(Q3Cylinder_GetBottomAttributeSet, CYLINDER, 20)

Q3_SETW(Q3Disk_SetOrigin, DISK, 0, 3)
Q3_GETW(Q3Disk_GetOrigin, DISK, 0, 3)
Q3_SETW(Q3Disk_SetMajorRadius, DISK, 3, 3)
Q3_GETW(Q3Disk_GetMajorRadius, DISK, 3, 3)
Q3_SETW(Q3Disk_SetMinorRadius, DISK, 6, 3)
Q3_GETW(Q3Disk_GetMinorRadius, DISK, 6, 3)

Q3_SETW(Q3Ellipse_SetOrigin, ELLIPSE, 0, 3)
Q3_GETW(Q3Ellipse_GetOrigin, ELLIPSE, 0, 3)
Q3_SETW(Q3Ellipse_SetMajorRadius, ELLIPSE, 3, 3)
Q3_GETW(Q3Ellipse_GetMajorRadius, ELLIPSE, 3, 3)
Q3_SETW(Q3Ellipse_SetMinorRadius, ELLIPSE, 6, 3)
Q3_GETW(Q3Ellipse_GetMinorRadius, ELLIPSE, 6, 3)

AXES(Ellipsoid, ELLIPSOID, 3, 6, 9)
Q3_SETW(Q3Ellipsoid_SetOrientation, ELLIPSOID, 3, 3)
Q3_GETW(Q3Ellipsoid_GetOrientation, ELLIPSOID, 3, 3)

AXES(Torus, TORUS, 3, 6, 9)
Q3_SETW(Q3Torus_SetOrientation, TORUS, 3, 3)
Q3_GETW(Q3Torus_GetOrientation, TORUS, 3, 3)
Q3_SETF(Q3Torus_SetRatio, TORUS, 12)
Q3_GETW(Q3Torus_GetRatio, TORUS, 12, 1)

Q3_SETW(Q3Box_SetOrigin, BOX, 0, 3)
Q3_GETW(Q3Box_GetOrigin, BOX, 0, 3)
Q3_SETW(Q3Box_SetOrientation, BOX, 3, 3)
Q3_GETW(Q3Box_GetOrientation, BOX, 3, 3)
Q3_SETW(Q3Box_SetMajorAxis, BOX, 6, 3)
Q3_GETW(Q3Box_GetMajorAxis, BOX, 6, 3)
Q3_SETW(Q3Box_SetMinorAxis, BOX, 9, 3)
Q3_GETW(Q3Box_GetMinorAxis, BOX, 9, 3)

/* A box's face sets: an array of six, made when first set. */
static void box_face(ppc_cpu *c, bool set)
{
    q3node *d = q3_data_of(Q3R(0), BOX);
    uint32_t i = Q3R(1);
    if (!d || i >= 6) {
        q3_error(kQ3ErrorParameterOutOfRange);
        Q3RETURN(kQ3Failure);
        return;
    }
    q3node *f = q3d_kid(d, &box_s, 0, 0);
    if (!f && set) {
        f = q3d_new(&objword_s, 6);
        q3d_set_kid(d, &box_s, 0, 0, f);
    }
    if (set) {
        if (f)
            q3_assign(&f->w[i], Q3R(2));
        q3_edited(q3_get(Q3R(0)));
    } else if (Q3R(2)) {
        wr32(Q3R(2), f && f->w[i] ? q3_ref(f->w[i]) : 0);
    }
    Q3RETURN(kQ3Success);
}
static void Q3Box_GetFaceAttributeSet(ppc_cpu *c) { box_face(c, false); }
static void Q3Box_SetFaceAttributeSet(ppc_cpu *c) { box_face(c, true); }

static void gpoly_vertex(ppc_cpu *c, bool attr, bool set)
{
    q3node *cs = array_of(Q3R(0), GPOLY, &gpoly_s, 0);
    uint32_t ci = Q3R(1), pi = Q3R(2);
    q3node *vs = cs && ci < cs->n ? q3d_kid(cs, &contour_s, ci, 0) : NULL;
    if (attr)
        vertex_set(c, vs, pi, Q3R(3), set, 4, 3);
    else
        vertex_pos(c, vs, pi, Q3R(3), set, 4);
}
static void Q3GeneralPolygon_GetVertexPosition(ppc_cpu *c) { gpoly_vertex(c, false, false); }
static void Q3GeneralPolygon_SetVertexPosition(ppc_cpu *c) { gpoly_vertex(c, false, true); }
static void Q3GeneralPolygon_GetVertexAttributeSet(ppc_cpu *c) { gpoly_vertex(c, true, false); }
static void Q3GeneralPolygon_SetVertexAttributeSet(ppc_cpu *c) { gpoly_vertex(c, true, true); }
Q3_SETV(Q3GeneralPolygon_SetShapeHint, GPOLY, 2)
Q3_GETW(Q3GeneralPolygon_GetShapeHint, GPOLY, 2, 1)

static void Q3Line_GetVertexPosition(ppc_cpu *c) { fixed_vertex(c, LINE, 2, false, false); }
static void Q3Line_SetVertexPosition(ppc_cpu *c) { fixed_vertex(c, LINE, 2, false, true); }
static void Q3Line_GetVertexAttributeSet(ppc_cpu *c) { fixed_vertex(c, LINE, 2, true, false); }
static void Q3Line_SetVertexAttributeSet(ppc_cpu *c) { fixed_vertex(c, LINE, 2, true, true); }
static void Q3Triangle_GetVertexPosition(ppc_cpu *c) { fixed_vertex(c, TRIANGLE, 3, false, false); }
static void Q3Triangle_SetVertexPosition(ppc_cpu *c) { fixed_vertex(c, TRIANGLE, 3, false, true); }
static void Q3Triangle_GetVertexAttributeSet(ppc_cpu *c) { fixed_vertex(c, TRIANGLE, 3, true, false); }
static void Q3Triangle_SetVertexAttributeSet(ppc_cpu *c) { fixed_vertex(c, TRIANGLE, 3, true, true); }

Q3_GETW(Q3Marker_GetPosition, MARKER, 0, 3)
Q3_SETW(Q3Marker_SetPosition, MARKER, 0, 3)
Q3_GETW(Q3Marker_GetXOffset, MARKER, 3, 1)
Q3_SETV(Q3Marker_SetXOffset, MARKER, 3)
Q3_GETW(Q3Marker_GetYOffset, MARKER, 4, 1)
Q3_SETV(Q3Marker_SetYOffset, MARKER, 4)


/* The marker's bitmap: a copy made for the program, or one taken. */
static void Q3Marker_GetBitmap(ppc_cpu *c)
{
    q3node *d = q3_data_of(Q3R(0), MARKER);
    gaddr out = Q3R(1);
    if (!d || !out) {
        Q3RETURN(kQ3Failure);
        return;
    }
    q3node *img = q3d_kid(d, &marker_s, 0, 0);
    gaddr b = img ? q3_galloc(img->nbytes) : 0;
    if (b && img->nbytes)
        gput(b, img->bytes, img->nbytes);
    wr32(out, b);
    for (int k = 1; k < 5; k++)
        wr32(out + 4 * (uint32_t)k, d->w[5 + k]);
    Q3RETURN(kQ3Success);
}

static void Q3Marker_SetBitmap(ppc_cpu *c)
{
    q3node *d = q3_data_of(Q3R(0), MARKER);
    gaddr in = Q3R(1);
    if (!d || !in) {
        Q3RETURN(kQ3Failure);
        return;
    }
    for (int k = 1; k < 5; k++)
        d->w[5 + k] = rd32(in + 4 * (uint32_t)k);
    uint32_t n = d->w[7] * d->w[8];
    q3node *img = calloc(1, sizeof *img);
    if (img) {
        img->bytes = malloc(n ? n : 1);
        img->nbytes = n;
        if (img->bytes && n && rd32(in))
            gget(img->bytes, rd32(in), n);
    }
    q3d_set_kid(d, &marker_s, 0, 0, img);
    q3_edited(q3_get(Q3R(0)));
    Q3RETURN(kQ3Success);
}

static void nurb_point(ppc_cpu *c, uint32_t type, const q3schema *s, int field, uint32_t i, gaddr p, bool set)
{
    q3node *cp = array_of(Q3R(0), type, s, field);
    if (!cp || i >= cp->n || !p) {
        q3_error(kQ3ErrorParameterOutOfRange);
        Q3RETURN(kQ3Failure);
        return;
    }
    for (uint32_t k = 0; k < cp->ew; k++) {
        if (set)
            cp->w[i * cp->ew + k] = rd32(p + 4 * k);
        else
            wr32(p + 4 * k, cp->w[i * cp->ew + k]);
    }
    if (set)
        q3_edited(q3_get(Q3R(0)));
    Q3RETURN(kQ3Success);
}

static void Q3NURBCurve_SetControlPoint(ppc_cpu *c) { nurb_point(c, NCURVE, &ncurve_s, 0, Q3R(1), Q3R(2), true); }
static void Q3NURBCurve_GetControlPoint(ppc_cpu *c) { nurb_point(c, NCURVE, &ncurve_s, 0, Q3R(1), Q3R(2), false); }

/* A knot by value (set: f1) or into a float. */
static void knot(ppc_cpu *c, uint32_t type, const q3schema *s, int field, bool set)
{
    q3node *k = array_of(Q3R(0), type, s, field);
    uint32_t i = Q3R(1);
    if (!k || i >= k->n || (!set && !Q3R(2))) {
        q3_error(kQ3ErrorParameterOutOfRange);
        Q3RETURN(kQ3Failure);
        return;
    }
    if (set) {
        q3d_setf(k->w, (int)i, q3_farg(c, 0));
        q3_edited(q3_get(Q3R(0)));
    } else {
        wr32(Q3R(2), k->w[i]);
    }
    Q3RETURN(kQ3Success);
}
static void Q3NURBCurve_SetKnot(ppc_cpu *c) { knot(c, NCURVE, &ncurve_s, 1, true); }
static void Q3NURBCurve_GetKnot(ppc_cpu *c) { knot(c, NCURVE, &ncurve_s, 1, false); }
static void Q3NURBPatch_SetUKnot(ppc_cpu *c) { knot(c, NPATCH, &npatch_s, 1, true); }
static void Q3NURBPatch_GetUKnot(ppc_cpu *c) { knot(c, NPATCH, &npatch_s, 1, false); }
static void Q3NURBPatch_SetVKnot(ppc_cpu *c) { knot(c, NPATCH, &npatch_s, 2, true); }
static void Q3NURBPatch_GetVKnot(ppc_cpu *c) { knot(c, NPATCH, &npatch_s, 2, false); }

static void patch_cp(ppc_cpu *c, bool set)
{
    q3node *d = q3_data_of(Q3R(0), NPATCH);
    uint32_t r = Q3R(1), col = Q3R(2);
    if (!d || r >= d->w[2] || col >= d->w[3]) {
        q3_error(kQ3ErrorParameterOutOfRange);
        Q3RETURN(kQ3Failure);
        return;
    }
    nurb_point(c, NPATCH, &npatch_s, 0, r * d->w[3] + col, Q3R(3), set);
}
static void Q3NURBPatch_SetControlPoint(ppc_cpu *c) { patch_cp(c, true); }
static void Q3NURBPatch_GetControlPoint(ppc_cpu *c) { patch_cp(c, false); }

Q3_GETW(Q3PixmapMarker_GetPosition, PMARKER, 0, 3)
Q3_SETW(Q3PixmapMarker_SetPosition, PMARKER, 0, 3)
Q3_GETW(Q3PixmapMarker_GetXOffset, PMARKER, 3, 1)
Q3_SETV(Q3PixmapMarker_SetXOffset, PMARKER, 3)
Q3_GETW(Q3PixmapMarker_GetYOffset, PMARKER, 4, 1)
Q3_SETV(Q3PixmapMarker_SetYOffset, PMARKER, 4)

static void Q3PixmapMarker_GetPixmap(ppc_cpu *c)
{
    q3node *d = q3_data_of(Q3R(0), PMARKER);
    if (d && Q3R(1)) {
        for (int k = 0; k < 8; k++)
            wr32(Q3R(1) + 4 * (uint32_t)k, d->w[5 + k]);
        if (d->w[5])
            q3_ref(d->w[5]);
    }
    Q3RETURN(d && Q3R(1) ? kQ3Success : kQ3Failure);
}

static void Q3PixmapMarker_SetPixmap(ppc_cpu *c)
{
    q3node *d = q3_data_of(Q3R(0), PMARKER);
    if (d && Q3R(1)) {
        q3_assign(&d->w[5], rd32(Q3R(1)));
        for (int k = 1; k < 8; k++)
            d->w[5 + k] = rd32(Q3R(1) + 4 * (uint32_t)k);
        q3_edited(q3_get(Q3R(0)));
    }
    Q3RETURN(d && Q3R(1) ? kQ3Success : kQ3Failure);
}

Q3_SETW(Q3Point_SetPosition, POINT, 0, 3)
Q3_GETW(Q3Point_GetPosition, POINT, 0, 3)

#define ARRAY_VERTEX(Name, TYPE, SCHEMA, FIELD)                                                                      \
    static void Q3##Name##_GetVertexPosition(ppc_cpu *c)                                                             \
    {                                                                                                                \
        vertex_pos(c, array_of(Q3R(0), TYPE, SCHEMA, FIELD), Q3R(1), Q3R(2), false, 4);                              \
    }                                                                                                                \
    static void Q3##Name##_SetVertexPosition(ppc_cpu *c)                                                             \
    {                                                                                                                \
        vertex_pos(c, array_of(Q3R(0), TYPE, SCHEMA, FIELD), Q3R(1), Q3R(2), true, 4);                               \
    }                                                                                                                \
    static void Q3##Name##_GetVertexAttributeSet(ppc_cpu *c)                                                         \
    {                                                                                                                \
        vertex_set(c, array_of(Q3R(0), TYPE, SCHEMA, FIELD), Q3R(1), Q3R(2), false, 4, 3);                           \
    }                                                                                                                \
    static void Q3##Name##_SetVertexAttributeSet(ppc_cpu *c)                                                         \
    {                                                                                                                \
        vertex_set(c, array_of(Q3R(0), TYPE, SCHEMA, FIELD), Q3R(1), Q3R(2), true, 4, 3);                            \
    }

ARRAY_VERTEX(Polygon, POLYGON, &polygon_s, 0)
ARRAY_VERTEX(Polyhedron, POLYHEDRON, &polyhedron_s, 0)
ARRAY_VERTEX(PolyLine, POLYLINE, &polyline_s, 0)

static void polyline_segment(ppc_cpu *c, bool set)
{
    q3node *d = q3_data_of(Q3R(0), POLYLINE);
    uint32_t i = Q3R(1);
    if (!d || !d->w[0] || i >= d->w[0] - 1) {
        q3_error(kQ3ErrorParameterOutOfRange);
        Q3RETURN(kQ3Failure);
        return;
    }
    q3node *s = q3d_kid(d, &polyline_s, 0, 1);
    if (!s && set) {
        s = q3d_new(&objword_s, d->w[0] - 1);
        q3d_set_kid(d, &polyline_s, 0, 1, s);
    }
    if (set) {
        if (s)
            q3_assign(&s->w[i], Q3R(2));
        q3_edited(q3_get(Q3R(0)));
    } else if (Q3R(2)) {
        wr32(Q3R(2), s && s->w[i] ? q3_ref(s->w[i]) : 0);
    }
    Q3RETURN(kQ3Success);
}
static void Q3PolyLine_GetSegmentAttributeSet(ppc_cpu *c) { polyline_segment(c, false); }
static void Q3PolyLine_SetSegmentAttributeSet(ppc_cpu *c) { polyline_segment(c, true); }

/* A polyhedron's triangle or edge, the structure in and out. */
static void ph_element(ppc_cpu *c, int field, const q3schema *es, bool set)
{
    q3node *arr = array_of(Q3R(0), POLYHEDRON, &polyhedron_s, field);
    uint32_t i = Q3R(1);
    gaddr p = Q3R(2);
    if (!arr || i >= arr->n || !p) {
        q3_error(kQ3ErrorParameterOutOfRange);
        Q3RETURN(kQ3Failure);
        return;
    }
    uint32_t *w = arr->w + i * es->words;
    if (set) {
        gaddr old = w[4];
        for (uint32_t k = 0; k < 4; k++)
            w[k] = rd32(p + 4 * k);
        w[4] = rd32(p + 16) ? q3_ref(rd32(p + 16)) : 0;
        if (old)
            q3_release(old);
        q3_edited(q3_get(Q3R(0)));
    } else {
        for (uint32_t k = 0; k < 4; k++)
            wr32(p + 4 * k, w[k]);
        wr32(p + 16, w[4] ? q3_ref(w[4]) : 0);
    }
    Q3RETURN(kQ3Success);
}
static void Q3Polyhedron_GetTriangleData(ppc_cpu *c) { ph_element(c, 2, &phtri_s, false); }
static void Q3Polyhedron_SetTriangleData(ppc_cpu *c) { ph_element(c, 2, &phtri_s, true); }
static void Q3Polyhedron_GetEdgeData(ppc_cpu *c) { ph_element(c, 1, &phedge_s, false); }
static void Q3Polyhedron_SetEdgeData(ppc_cpu *c) { ph_element(c, 1, &phedge_s, true); }

static void trigrid_vertex(ppc_cpu *c, bool attr, bool set)
{
    q3node *d = q3_data_of(Q3R(0), TRIGRID);
    uint32_t r = Q3R(1), col = Q3R(2);
    if (!d || r >= d->w[0] || col >= d->w[1]) {
        q3_error(kQ3ErrorParameterOutOfRange);
        Q3RETURN(kQ3Failure);
        return;
    }
    q3node *vs = q3d_kid(d, &trigrid_s, 0, 0);
    if (attr)
        vertex_set(c, vs, r * d->w[1] + col, Q3R(3), set, 4, 3);
    else
        vertex_pos(c, vs, r * d->w[1] + col, Q3R(3), set, 4);
}
static void Q3TriGrid_GetVertexPosition(ppc_cpu *c) { trigrid_vertex(c, false, false); }
static void Q3TriGrid_SetVertexPosition(ppc_cpu *c) { trigrid_vertex(c, false, true); }
static void Q3TriGrid_GetVertexAttributeSet(ppc_cpu *c) { trigrid_vertex(c, true, false); }
static void Q3TriGrid_SetVertexAttributeSet(ppc_cpu *c) { trigrid_vertex(c, true, true); }

static void trigrid_facet(ppc_cpu *c, bool set)
{
    q3node *d = q3_data_of(Q3R(0), TRIGRID);
    uint32_t i = Q3R(1), n = d ? grid_facets(d->w, NULL) : 0;
    if (!d || i >= n) {
        q3_error(kQ3ErrorParameterOutOfRange);
        Q3RETURN(kQ3Failure);
        return;
    }
    q3node *f = q3d_kid(d, &trigrid_s, 0, 1);
    if (!f && set) {
        f = q3d_new(&objword_s, n);
        q3d_set_kid(d, &trigrid_s, 0, 1, f);
    }
    if (set) {
        if (f)
            q3_assign(&f->w[i], Q3R(2));
        q3_edited(q3_get(Q3R(0)));
    } else if (Q3R(2)) {
        wr32(Q3R(2), f && f->w[i] ? q3_ref(f->w[i]) : 0);
    }
    Q3RETURN(kQ3Success);
}
static void Q3TriGrid_GetFacetAttributeSet(ppc_cpu *c) { trigrid_facet(c, false); }
static void Q3TriGrid_SetFacetAttributeSet(ppc_cpu *c) { trigrid_facet(c, true); }

#define E(n) { #n, n }
#define GEOM_EXPORTS(Name)                                                                                           \
    E(Q3##Name##_New), E(Q3##Name##_Submit), E(Q3##Name##_SetData), E(Q3##Name##_GetData), E(Q3##Name##_EmptyData)

const ppc_export q3_geom_exports[] = {
    E(Q3Geometry_GetType), E(Q3Geometry_GetAttributeSet), E(Q3Geometry_SetAttributeSet), E(Q3Geometry_Submit),
    GEOM_EXPORTS(Box), GEOM_EXPORTS(Cone), GEOM_EXPORTS(Cylinder), GEOM_EXPORTS(Disk), GEOM_EXPORTS(Ellipse),
    GEOM_EXPORTS(Ellipsoid), GEOM_EXPORTS(GeneralPolygon), GEOM_EXPORTS(Line), GEOM_EXPORTS(Marker),
    GEOM_EXPORTS(NURBCurve), GEOM_EXPORTS(NURBPatch), GEOM_EXPORTS(PixmapMarker), GEOM_EXPORTS(Point),
    GEOM_EXPORTS(Polygon), GEOM_EXPORTS(Polyhedron), GEOM_EXPORTS(PolyLine), GEOM_EXPORTS(Torus),
    GEOM_EXPORTS(Triangle), GEOM_EXPORTS(TriGrid), GEOM_EXPORTS(TriMesh),
    E(Q3Box_SetOrigin), E(Q3Box_GetOrigin), E(Q3Box_SetOrientation), E(Q3Box_GetOrientation),
    E(Q3Box_SetMajorAxis), E(Q3Box_GetMajorAxis), E(Q3Box_SetMinorAxis), E(Q3Box_GetMinorAxis),
    E(Q3Box_GetFaceAttributeSet), E(Q3Box_SetFaceAttributeSet),
    E(Q3Cone_SetOrigin), E(Q3Cone_GetOrigin), E(Q3Cone_SetOrientation), E(Q3Cone_GetOrientation),
    E(Q3Cone_SetMajorRadius), E(Q3Cone_GetMajorRadius), E(Q3Cone_SetMinorRadius), E(Q3Cone_GetMinorRadius),
    E(Q3Cone_SetCaps), E(Q3Cone_GetCaps), E(Q3Cone_SetBottomAttributeSet), E(Q3Cone_GetBottomAttributeSet),
    E(Q3Cone_SetFaceAttributeSet), E(Q3Cone_GetFaceAttributeSet),
    E(Q3Cylinder_SetOrigin), E(Q3Cylinder_GetOrigin), E(Q3Cylinder_SetOrientation), E(Q3Cylinder_GetOrientation),
    E(Q3Cylinder_SetMajorRadius), E(Q3Cylinder_GetMajorRadius), E(Q3Cylinder_SetMinorRadius),
    E(Q3Cylinder_GetMinorRadius), E(Q3Cylinder_SetCaps), E(Q3Cylinder_GetCaps), E(Q3Cylinder_SetTopAttributeSet),
    E(Q3Cylinder_GetTopAttributeSet), E(Q3Cylinder_SetBottomAttributeSet), E(Q3Cylinder_GetBottomAttributeSet),
    E(Q3Cylinder_SetFaceAttributeSet), E(Q3Cylinder_GetFaceAttributeSet),
    E(Q3Disk_SetOrigin), E(Q3Disk_GetOrigin), E(Q3Disk_SetMajorRadius), E(Q3Disk_GetMajorRadius),
    E(Q3Disk_SetMinorRadius), E(Q3Disk_GetMinorRadius),
    E(Q3Ellipse_SetOrigin), E(Q3Ellipse_GetOrigin), E(Q3Ellipse_SetMajorRadius), E(Q3Ellipse_GetMajorRadius),
    E(Q3Ellipse_SetMinorRadius), E(Q3Ellipse_GetMinorRadius),
    E(Q3Ellipsoid_SetOrigin), E(Q3Ellipsoid_GetOrigin), E(Q3Ellipsoid_SetOrientation),
    E(Q3Ellipsoid_GetOrientation), E(Q3Ellipsoid_SetMajorRadius), E(Q3Ellipsoid_GetMajorRadius),
    E(Q3Ellipsoid_SetMinorRadius), E(Q3Ellipsoid_GetMinorRadius),
    E(Q3GeneralPolygon_GetVertexPosition), E(Q3GeneralPolygon_SetVertexPosition),
    E(Q3GeneralPolygon_GetVertexAttributeSet), E(Q3GeneralPolygon_SetVertexAttributeSet),
    E(Q3GeneralPolygon_SetShapeHint), E(Q3GeneralPolygon_GetShapeHint),
    E(Q3Line_GetVertexPosition), E(Q3Line_SetVertexPosition), E(Q3Line_GetVertexAttributeSet),
    E(Q3Line_SetVertexAttributeSet),
    E(Q3Marker_GetPosition), E(Q3Marker_SetPosition), E(Q3Marker_GetXOffset), E(Q3Marker_SetXOffset),
    E(Q3Marker_GetYOffset), E(Q3Marker_SetYOffset), E(Q3Marker_GetBitmap), E(Q3Marker_SetBitmap),
    E(Q3NURBCurve_SetControlPoint), E(Q3NURBCurve_GetControlPoint), E(Q3NURBCurve_SetKnot), E(Q3NURBCurve_GetKnot),
    E(Q3NURBPatch_SetControlPoint), E(Q3NURBPatch_GetControlPoint), E(Q3NURBPatch_SetUKnot),
    E(Q3NURBPatch_SetVKnot), E(Q3NURBPatch_GetUKnot), E(Q3NURBPatch_GetVKnot),
    E(Q3PixmapMarker_GetPosition), E(Q3PixmapMarker_SetPosition), E(Q3PixmapMarker_GetXOffset),
    E(Q3PixmapMarker_SetXOffset), E(Q3PixmapMarker_GetYOffset), E(Q3PixmapMarker_SetYOffset),
    E(Q3PixmapMarker_GetPixmap), E(Q3PixmapMarker_SetPixmap),
    E(Q3Point_SetPosition), E(Q3Point_GetPosition),
    E(Q3Polygon_GetVertexPosition), E(Q3Polygon_SetVertexPosition), E(Q3Polygon_GetVertexAttributeSet),
    E(Q3Polygon_SetVertexAttributeSet),
    E(Q3Polyhedron_GetVertexPosition), E(Q3Polyhedron_SetVertexPosition), E(Q3Polyhedron_GetVertexAttributeSet),
    E(Q3Polyhedron_SetVertexAttributeSet), E(Q3Polyhedron_GetTriangleData), E(Q3Polyhedron_SetTriangleData),
    E(Q3Polyhedron_GetEdgeData), E(Q3Polyhedron_SetEdgeData),
    E(Q3PolyLine_GetVertexPosition), E(Q3PolyLine_SetVertexPosition), E(Q3PolyLine_GetVertexAttributeSet),
    E(Q3PolyLine_SetVertexAttributeSet), E(Q3PolyLine_GetSegmentAttributeSet), E(Q3PolyLine_SetSegmentAttributeSet),
    E(Q3Torus_SetOrigin), E(Q3Torus_GetOrigin), E(Q3Torus_SetOrientation), E(Q3Torus_GetOrientation),
    E(Q3Torus_SetMajorRadius), E(Q3Torus_GetMajorRadius), E(Q3Torus_SetMinorRadius), E(Q3Torus_GetMinorRadius),
    E(Q3Torus_SetRatio), E(Q3Torus_GetRatio),
    E(Q3Triangle_GetVertexPosition), E(Q3Triangle_SetVertexPosition), E(Q3Triangle_GetVertexAttributeSet),
    E(Q3Triangle_SetVertexAttributeSet),
    E(Q3TriGrid_GetVertexPosition), E(Q3TriGrid_SetVertexPosition), E(Q3TriGrid_GetVertexAttributeSet),
    E(Q3TriGrid_SetVertexAttributeSet), E(Q3TriGrid_GetFacetAttributeSet), E(Q3TriGrid_SetFacetAttributeSet),
};
const unsigned q3_geom_export_count = sizeof q3_geom_exports / sizeof q3_geom_exports[0];

/* For the 3DMF reader and writer: a geometry's schema. */
const q3schema *q3_geom_schema(uint32_t type)
{
    const struct geom *g = geom_of(type);
    return g ? g->s : NULL;
}
