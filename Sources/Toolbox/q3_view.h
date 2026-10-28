/*
 * What QuickDraw 3D's geometries hand a view (q3_view.c): vertices with
 * their attributes, as triangles, lines, points or markers, in the
 * geometry's local coordinates.  The view transforms, lights, clips and
 * projects them and draws through r3d.h, or picks, bounds or writes them,
 * as its loop is doing.
 */
#ifndef Q3_VIEW_H
#define Q3_VIEW_H

#include "q3_data.h"

/* Attributes as a vertex, face or geometry has them. */
enum {
    QA_N = 1, QA_UV = 2, QA_KD = 4, QA_KS = 8, QA_KT = 16, QA_SPC = 32, QA_AMB = 64, QA_SHADER = 128,
    QA_HILITE = 256,
};
typedef struct {
    uint16_t has;
    bool hilite;
    float n[3], uv[2], kd[3], ks[3], kt[3], spc, amb;
    gaddr shader; /* a surface shader, borrowed */
} q3attrs;

/* What an attribute set has that a lacks. */
void q3attrs_fill(q3attrs *a, gaddr set);
/* What b has that a lacks. */
void q3attrs_merge(q3attrs *a, const q3attrs *b);

typedef struct {
    float p[3];
    q3attrs a;
} q3pv;

typedef struct {
    uint32_t v[3];
    uint8_t edges; /* which of v0-v1, v1-v2, v2-v0 are the shape's edges (bits 0-2) */
    q3attrs a;
} q3ptri;

/* Triangles for a polygon of contours (the first the outside, the rest
 * holes), each a list of indices into pts: appended to *t (grown).  The
 * triangles' edges are marked where they are the contours'. */
uint32_t q3_triangulate(const float (*pts)[3], const uint32_t *const *contours, const uint32_t *ncontour,
                        uint32_t ncontours, q3ptri **t, uint32_t *nt, uint32_t *cap);

void q3v_triangles(q3view *v, const q3pv *pv, uint32_t npv, const q3ptri *t, uint32_t nt, gaddr geom_set);
/* Segments: pv[2i] to pv[2i + 1]. */
void q3v_lines(q3view *v, const q3pv *pv, uint32_t nseg, gaddr geom_set);
void q3v_points(q3view *v, const q3pv *pv, uint32_t n, gaddr geom_set);
/* A 2D image (0xAARRGGBB, top row first) with its top left at the
 * projection of p, offset by (dx, dy) pixels. */
void q3v_image(q3view *v, const float p[3], int dx, int dy, int w, int h, const uint32_t *argb, gaddr set);

/* The subdivision style in force: method, c1, c2. */
void q3v_subdivision(q3view *v, int *method, float *c1, float *c2);
/* How many segments around a curve of this length (local units) gets. */
int q3v_segments(q3view *v, float length, int fallback);
/* Whether the loop is writing (q3_file.c then gets the object). */
bool q3v_writing(q3view *v);
/* The object submitted at the top of the traversal, for picks. */
void q3v_begin_object(q3view *v, gaddr object);
void q3v_end_object(q3view *v);

/* The view's state for submitting: attribute sets, transforms, styles,
 * shaders (q3_view.c), groups (q3_group.c). */
void q3v_submit(q3view *v, gaddr object);
void q3v_attribute_set(q3view *v, gaddr set);
void q3v_transform(q3view *v, const float m[16]);
void q3v_push(q3view *v);
void q3v_pop(q3view *v);
q3view *q3v_of(gaddr view);
/* The bounding box of obj submitted alone: min, max, isEmpty (1 or 0). */
void q3v_bound(q3view *v, gaddr obj, bool exact, float box[7]);

/* A geometry's submitted form: its class's submit method gets the
 * object; immediate mode makes a temporary data node. */
typedef void (*q3_tessellate)(q3view *v, const q3node *d, q3obj *o);

/* Group positions and contents (q3_group.c), for the view and the file. */
typedef struct q3pos q3pos;
struct q3pos {
    gaddr g, obj;
    q3pos *prev, *next;
};
typedef struct {
    q3pos *first, *last;
    uint32_t state;      /* a display group's */
    bool has_bounds;
    float bounds[7];     /* min, max, isEmpty */
} q3group;
q3group *q3_group_of(gaddr g);
void q3_group_classes(void);
gaddr q3_group_add(gaddr group, gaddr obj);

/* Picks (q3_pick.c): what a pick asks, and its hits. */
enum { Q3PICK_POINT = 1, Q3PICK_RECT, Q3PICK_RAY };
typedef struct {
    int kind;
    float pt[2], rect[4], ray[6];
    float vtol, etol;
    uint32_t sort, mask, max;
} q3pickq;
typedef struct {
    gaddr object, root;
    uint32_t pickid, depth, part;
    gaddr *positions;
    float xyz[3], distance, normal[3], uv[2], m[16];
} q3hit;
bool q3_pick_query(gaddr pick, q3pickq *q);
void q3_pick_hit(gaddr pick, const q3hit *h);
void q3_pick_classes(void);
/* The group positions from the traversal's root to what is drawn. */
void q3v_enter(q3view *v, gaddr position);
void q3v_leave(q3view *v);

/* The mesh (q3_mesh.c): its own attribute set. */
gaddr q3_mesh_set(q3obj *mesh);
void q3_mesh_set_set(q3obj *mesh, gaddr set);
void q3_mesh_classes(void);
/* A mesh made at once (the 3DMF reader): vertices with their sets, faces
 * of face_nverts[i] vertex indices each (one contour), with their sets;
 * a count with the top bit set is a contour (hole) of the face before. */
gaddr q3_mesh_build(uint32_t nverts, const float (*pts)[3], const gaddr *vsets, uint32_t nfaces,
                    const uint32_t *face_nverts, const uint32_t *face_verts, const gaddr *fsets);
/* A mesh gone through (the 3DMF writer): its vertices in order, its faces
 * (contours as vertex indices, the outside first), the corners and the
 * edges with attribute sets.  Callbacks may be NULL. */
typedef struct {
    void (*vertex)(void *ctx, uint32_t i, const float p[3], gaddr set);
    void (*face)(void *ctx, uint32_t i, uint32_t ncontours, const uint32_t *nverts, const uint32_t *verts, gaddr set);
    void (*corner)(void *ctx, uint32_t vertex, uint32_t face, gaddr set);
    void (*edge)(void *ctx, uint32_t v1, uint32_t v2, gaddr set);
} q3mesh_walker;
void q3_mesh_walk(q3obj *mesh, const q3mesh_walker *w, void *ctx);

/* 3DMF (q3_file.c): a metafile's objects, read from its bytes into a
 * new display group (0 if it isn't one); and objects written as one
 * (binary, malloc'd). */
gaddr q3_read_3dmf(const uint8_t *bytes, uint32_t size);
uint8_t *q3_write_3dmf(gaddr object, uint32_t *size);

/* 4x4 matrices, row vectors (q3_view.c). */
void q3m_identity(float m[16]);
void q3m_mul(const float a[16], const float b[16], float out[16]);
bool q3m_invert(const float m[16], float out[16]);
void q3m_point(const float m[16], const float p[3], float out[3]);
void q3m_vector(const float m[16], const float v[3], float out[3]);
void q3m_get(gaddr g, float m[16]);
void q3m_put(gaddr g, const float m[16]);

#endif
