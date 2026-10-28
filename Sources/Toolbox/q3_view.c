/*
 * QuickDraw 3D's views (QD3DView.h): the submit loops (rendering,
 * bounding, picking and writing), the state they keep (the transform,
 * attributes, styles and shaders, pushed and popped), and the pipeline a
 * geometry's triangles, lines, points and images go down when rendering:
 * to world coordinates, lit per vertex there (ambient, directional, point
 * and spot lights; null, Lambert and Phong illumination), culled or
 * flipped as the backfacing style says, clipped to the frustum's near and
 * far planes, projected into the draw context's pane and drawn through
 * the host's renderer (r3d.h).  Transparent triangles wait for the end of
 * the frame and are drawn back to front without writing depth.
 *
 * The frame is read back at Q3View_EndRendering into the draw context: a
 * window or port (through its clip, as QuickDraw draws) or a pixmap.
 */
#include <math.h>
#include <stdlib.h>

#include "q3_viewpriv.h"
#include "tb_internal.h"

/* ---------------------------------------------------------------------- */
/* Matrices */

void q3m_identity(float m[16])
{
    memset(m, 0, 16 * sizeof *m);
    m[0] = m[5] = m[10] = m[15] = 1;
}

void q3m_mul(const float a[16], const float b[16], float out[16])
{
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(out, r, sizeof r);
}

bool q3m_invert(const float m[16], float out[16])
{
    double a[4][8];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++)
            a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++)
            if (fabs(a[r][c]) > fabs(a[p][c]))
                p = r;
        if (fabs(a[p][c]) < 1e-12)
            return false;
        for (int j = 0; j < 8; j++) {
            double t = a[c][j];
            a[c][j] = a[p][j];
            a[p][j] = t;
        }
        double d = a[c][c];
        for (int j = 0; j < 8; j++)
            a[c][j] /= d;
        for (int r = 0; r < 4; r++)
            if (r != c) {
                double f = a[r][c];
                for (int j = 0; j < 8; j++)
                    a[r][j] -= f * a[c][j];
            }
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            out[i * 4 + j] = (float)a[i][j + 4];
    return true;
}

void q3m_point(const float m[16], const float p[3], float out[3])
{
    float x = p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12];
    float y = p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13];
    float z = p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + m[14];
    float w = p[0] * m[3] + p[1] * m[7] + p[2] * m[11] + m[15];
    if (w != 0 && w != 1) {
        x /= w;
        y /= w;
        z /= w;
    }
    out[0] = x;
    out[1] = y;
    out[2] = z;
}

static void point4(const float m[16], const float p[3], float out[4])
{
    out[0] = p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12];
    out[1] = p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13];
    out[2] = p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + m[14];
    out[3] = p[0] * m[3] + p[1] * m[7] + p[2] * m[11] + m[15];
}

void q3m_vector(const float m[16], const float v[3], float out[3])
{
    float x = v[0] * m[0] + v[1] * m[4] + v[2] * m[8];
    float y = v[0] * m[1] + v[1] * m[5] + v[2] * m[9];
    float z = v[0] * m[2] + v[1] * m[6] + v[2] * m[10];
    out[0] = x;
    out[1] = y;
    out[2] = z;
}

void q3m_get(gaddr g, float m[16])
{
    for (int i = 0; i < 16; i++)
        m[i] = rdf(g + 4 * (uint32_t)i);
}

void q3m_put(gaddr g, const float m[16])
{
    for (int i = 0; i < 16; i++)
        wrf(g + 4 * (uint32_t)i, m[i]);
}

static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void sub3(const float *a, const float *b, float *o)
{
    o[0] = a[0] - b[0];
    o[1] = a[1] - b[1];
    o[2] = a[2] - b[2];
}
static void cross3(const float *a, const float *b, float *o)
{
    float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x;
    o[1] = y;
    o[2] = z;
}
static float norm3(float *a)
{
    float l = sqrtf(dot3(a, a));
    if (l > 1e-20f) {
        a[0] /= l;
        a[1] /= l;
        a[2] /= l;
    }
    return l;
}

/* ---------------------------------------------------------------------- */
/* Attributes */

void q3attrs_fill(q3attrs *a, gaddr set)
{
    q3set *s = q3_set_of(set);
    if (!s)
        return;
    for (int i = 0; i < s->n; i++) {
        const q3elem *e = &s->e[i];
        float f[6] = { 0 };
        for (uint32_t j = 0; j < e->size / 4 && j < 6; j++) {
            uint32_t u = (uint32_t)e->bytes[4 * j] << 24 | (uint32_t)e->bytes[4 * j + 1] << 16 |
                         (uint32_t)e->bytes[4 * j + 2] << 8 | e->bytes[4 * j + 3];
            memcpy(&f[j], &u, 4);
        }
        switch (e->type) {
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
        case kQ3AttributeTypeHighlightState: {
            uint32_t on = (uint32_t)e->bytes[0] << 24 | (uint32_t)e->bytes[1] << 16 | (uint32_t)e->bytes[2] << 8 | e->bytes[3];
            if (!(a->has & QA_HILITE)) { a->hilite = on != 0; a->has |= QA_HILITE; }
            break;
        }
        case kQ3AttributeTypeSurfaceShader:
            if (!(a->has & QA_SHADER)) { a->shader = q3_attr_shader(set); a->has |= QA_SHADER; }
            break;
        default: break;
        }
    }
}

void q3attrs_merge(q3attrs *a, const q3attrs *b)
{
    uint16_t m = (uint16_t)(b->has & ~a->has);
    if (m & QA_N) memcpy(a->n, b->n, 12);
    if (m & QA_UV) memcpy(a->uv, b->uv, 8);
    if (m & QA_KD) memcpy(a->kd, b->kd, 12);
    if (m & QA_KS) memcpy(a->ks, b->ks, 12);
    if (m & QA_KT) memcpy(a->kt, b->kt, 12);
    if (m & QA_SPC) a->spc = b->spc;
    if (m & QA_AMB) a->amb = b->amb;
    if (m & QA_SHADER) a->shader = b->shader;
    if (m & QA_HILITE) a->hilite = b->hilite;
    a->has |= m;
}

/* ---------------------------------------------------------------------- */
/* The view */

gaddr rave_frame_context(r3d_frame *f, int width, int height, gaddr *engine);
void rave_frame_context_free(gaddr dc);

enum { MAX_VIEWS = 64 };
static q3view *views[MAX_VIEWS];

static void drop_rave_context(q3view *v)
{
    if (!v->rave_dc)
        return;
    if (v->rave_destroy) {
        uint32_t arg = v->renderer;
        ppc_call(v->rave_destroy, &arg, 1);
    }
    rave_frame_context_free(v->rave_dc);
    v->rave_dc = v->rave_engine = 0;
}

/* The RAVE draw context of the view drawing with this renderer (made on
 * its frame now if need be); 0 if no view has drawn with it. */
gaddr q3v_rave_context(gaddr renderer, gaddr *engine, gaddr destroy);
gaddr q3v_rave_context(gaddr renderer, gaddr *engine, gaddr destroy)
{
    for (int i = 0; i < MAX_VIEWS; i++) {
        q3view *v = views[i];
        if (!v || v->renderer != renderer || !v->frame)
            continue;
        if (!v->rave_dc)
            v->rave_dc = rave_frame_context(v->frame, v->fw, v->fh, &v->rave_engine);
        if (destroy)
            v->rave_destroy = destroy;
        if (engine)
            *engine = v->rave_engine;
        return v->rave_dc;
    }
    return 0;
}

static void default_state(q3view *v)
{
    vstate *s = &v->st;
    memset(s, 0, sizeof *s);
    q3m_identity(s->m);
    s->a.has = QA_KD | QA_KS | QA_KT | QA_SPC | QA_AMB | QA_HILITE;
    s->a.kd[0] = s->a.kd[1] = s->a.kd[2] = 1;
    s->a.ks[0] = s->a.ks[1] = s->a.ks[2] = 0.5f;
    s->a.kt[0] = s->a.kt[1] = s->a.kt[2] = 1;
    s->a.spc = 4;
    s->a.amb = 1;
    s->backfacing = kQ3BackfacingStyleBoth;
    s->interpolation = kQ3InterpolationStyleVertex;
    s->fill = kQ3FillStyleFilled;
    s->subdiv_method = kQ3SubdivisionMethodScreenSpace;
    s->subdiv_c1 = s->subdiv_c2 = 20;
    s->pickid = 0;
    s->illumination = ILLUM_LAMBERT;
    /* The view's default attribute set over the defaults. */
    if (v->default_set) {
        q3attrs d;
        memset(&d, 0, sizeof d);
        q3attrs_fill(&d, v->default_set);
        q3attrs over = d;
        q3attrs_merge(&over, &s->a);
        s->a = over;
    }
    v->depth = 0;
}

void q3v_default_state(q3view *v) { default_state(v); }

q3view *q3v_drawing_into(gaddr dctx)
{
    for (int i = 0; i < MAX_VIEWS; i++)
        if (views[i] && views[i]->dctx == dctx && views[i]->loop == LOOP_RENDER)
            return views[i];
    for (int i = 0; i < MAX_VIEWS; i++)
        if (views[i] && views[i]->dctx == dctx)
            return views[i];
    return NULL;
}

q3view *q3v_of(gaddr g)
{
    q3obj *o = q3_of(g, Q3_VIEW);
    return o ? o->data : NULL;
}

void q3v_push(q3view *v)
{
    if (v->depth == v->cap) {
        int cap = v->cap ? v->cap * 2 : 16;
        vstate *n = realloc(v->stack, (size_t)cap * sizeof *n);
        if (!n)
            return;
        v->stack = n;
        v->cap = cap;
    }
    v->stack[v->depth++] = v->st;
    if (v->xr)
        q3xr_push(v);
}

void q3v_pop(q3view *v)
{
    if (!v->depth) {
        q3_error(kQ3ErrorNothingToPop);
        return;
    }
    v->st = v->stack[--v->depth];
    if (v->xr)
        q3xr_pop(v);
}

void q3v_transform(q3view *v, const float m[16]) { q3m_mul(m, v->st.m, v->st.m); }

void q3v_reset_transform(q3view *v);
void q3v_reset_transform(q3view *v) { q3m_identity(v->st.m); }

/* An attribute set submitted to the view: its attributes replace those
 * in force. */
void q3v_attribute_set(q3view *v, gaddr set)
{
    q3attrs a;
    memset(&a, 0, sizeof a);
    q3attrs_fill(&a, set);
    q3attrs_merge(&a, &v->st.a);
    v->st.a = a;
}

bool q3v_writing(q3view *v) { return v->loop == LOOP_WRITE; }

void q3v_enter(q3view *v, gaddr position)
{
    if (v->pathlen < 64)
        v->path[v->pathlen] = position;
    v->pathlen++;
}

void q3v_leave(q3view *v)
{
    if (v->pathlen > 0)
        v->pathlen--;
}

/* Submitting is by class: geometries draw, groups go through their
 * contents, the rest change the state (q3_shapes.c). */
void q3_file_write_object(gaddr file, gaddr object, q3view *v);

void q3v_submit(q3view *v, gaddr g)
{
    q3obj *o = q3_get(g);
    if (!o || !v || v->cancelled)
        return;
    if (v->loop == LOOP_WRITE && v->pathlen == 0) {
        q3_file_write_object(v->file, g, v);
        return;
    }
    bool top = v->pathlen == 0 && !v->object;
    if (top)
        v->object = g;
    if (o->cls->submit)
        o->cls->submit(o, v);
    if (top)
        v->object = 0;
}

void q3v_begin_object(q3view *v, gaddr object)
{
    (void)v;
    (void)object;
}
void q3v_end_object(q3view *v) { (void)v; }

void q3v_subdivision(q3view *v, int *method, float *c1, float *c2)
{
    *method = (int)v->st.subdiv_method;
    *c1 = v->st.subdiv_c1;
    *c2 = v->st.subdiv_c2;
}

/* Segments for a curve: constant c1, world-space c1 units a segment, or
 * screen-space c1 pixels a segment (with the curve's size on screen
 * guessed from the transform and the frame). */
int q3v_segments(q3view *v, float length, int fallback)
{
    float c1 = v->st.subdiv_c1;
    int n = fallback;
    switch (v->st.subdiv_method) {
    case kQ3SubdivisionMethodConstant: n = (int)c1; break;
    case kQ3SubdivisionMethodWorldSpace:
        if (length > 0 && c1 > 0) {
            float sc = sqrtf(fabsf(v->st.m[0] * v->st.m[5] - v->st.m[1] * v->st.m[4]));
            n = (int)ceilf(length * (sc > 0 ? sc : 1) / c1);
        }
        break;
    default:
        if (length > 0 && c1 > 0 && v->fh > 0) {
            float sc = sqrtf(fabsf(v->st.m[0] * v->st.m[5] - v->st.m[1] * v->st.m[4]));
            n = (int)ceilf(length * (sc > 0 ? sc : 1) * (float)v->fh / 4 / c1);
        }
        break;
    }
    if (n < 3)
        n = 3;
    if (n > 256)
        n = 256;
    return n;
}

/* ---------------------------------------------------------------------- */
/* Cameras (data in q3_shapes.c: placement, range, view port first) */

static void camera_world_to_view(const uint32_t *w, float m[16])
{
    float loc[3] = { q3d_f(w, 0), q3d_f(w, 1), q3d_f(w, 2) };
    float poi[3] = { q3d_f(w, 3), q3d_f(w, 4), q3d_f(w, 5) };
    float up[3] = { q3d_f(w, 6), q3d_f(w, 7), q3d_f(w, 8) };
    float z[3], x[3], y[3];
    sub3(loc, poi, z);
    if (norm3(z) < 1e-20f)
        z[2] = 1;
    cross3(up, z, x);
    if (norm3(x) < 1e-20f) {
        float alt[3] = { 0, 0, 1 };
        if (fabsf(z[2]) > 0.9f)
            alt[1] = 1, alt[2] = 0;
        cross3(alt, z, x);
        norm3(x);
    }
    cross3(z, x, y);
    q3m_identity(m);
    for (int i = 0; i < 3; i++) {
        m[i * 4 + 0] = x[i];
        m[i * 4 + 1] = y[i];
        m[i * 4 + 2] = z[i];
    }
    m[12] = -dot3(x, loc);
    m[13] = -dot3(y, loc);
    m[14] = -dot3(z, loc);
}

/* View to frustum: x and y to -1..1 across the camera's view port, z from
 * 0 at hither to -1 at yon. */
static void camera_view_to_frustum(uint32_t type, const uint32_t *w, float m[16])
{
    float n = q3d_f(w, 9), f = q3d_f(w, 10);
    if (n <= 0)
        n = 0.001f;
    if (f <= n)
        f = n + 1;
    memset(m, 0, 16 * sizeof *m);
    if (type == CAMERA_ORTH) {
        float l = q3d_f(w, 15), t = q3d_f(w, 16), r = q3d_f(w, 17), b = q3d_f(w, 18);
        if (r == l)
            r = l + 1;
        if (t == b)
            t = b + 1;
        m[0] = 2 / (r - l);
        m[12] = -(r + l) / (r - l);
        m[5] = 2 / (t - b);
        m[13] = -(t + b) / (t - b);
        m[10] = 1 / (f - n);
        m[14] = n / (f - n);
        m[15] = 1;
    } else {
        float sx, sy, cx = 0, cy = 0;
        if (type == CAMERA_VIEWPLANE) {
            float d = q3d_f(w, 15), hw = q3d_f(w, 16), hh = q3d_f(w, 17);
            cx = q3d_f(w, 18);
            cy = q3d_f(w, 19);
            sx = hw ? d / hw : 1;
            sy = hh ? d / hh : 1;
            cx = hw ? cx / hw : 0;
            cy = hh ? cy / hh : 0;
        } else {
            float fov = q3d_f(w, 15), aspect = q3d_f(w, 16);
            if (fov <= 0 || fov >= (float)M_PI)
                fov = 1;
            if (aspect <= 0)
                aspect = 1;
            float cot = 1 / tanf(fov / 2);
            /* The field of view is across the narrower dimension. */
            if (aspect >= 1) {
                sx = cot / aspect;
                sy = cot;
            } else {
                sx = cot;
                sy = cot * aspect;
            }
        }
        m[0] = sx;
        m[5] = sy;
        m[8] = cx; /* x_clip = x*sx + z*cx ... with w = -z: x/w = (x*sx - cx*w)/w */
        m[9] = cy;
        /* Scaled by 1/yon, as QuickDraw 3D's own matrix is (w is the
         * distance over yon): the same frustum, but programs that do
         * their own sums with it (Bugdom's culling multiplies by w)
         * need these numbers. */
        m[0] /= f;
        m[5] /= f;
        m[8] /= f;
        m[9] /= f;
        m[10] = 1 / (f - n);
        m[11] = -1 / f;
        m[14] = n / (f - n);
    }
    /* The camera's view port: its part of the view plane made -1..1. */
    float ox = q3d_f(w, 11), oy = q3d_f(w, 12), vw = q3d_f(w, 13), vh = q3d_f(w, 14);
    if (vw > 0 && vh > 0 && !(ox == -1 && oy == 1 && vw == 2 && vh == 2)) {
        float sx = 2 / vw, sy = 2 / vh, cx = ox + vw / 2, cy = oy - vh / 2;
        float vp[16] = { sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, 1, 0, -cx * sx, -cy * sy, 0, 1 };
        q3m_mul(m, vp, m);
    }
}

bool q3_camera_matrices(gaddr camera, float w2v[16], float v2f[16]);
bool q3_camera_matrices(gaddr camera, float w2v[16], float v2f[16])
{
    q3obj *o = q3_get(camera);
    if (!o || !q3_type_is(o->type, Q3_CAMERA) || !o->data)
        return false;
    const uint32_t *w = ((q3node *)o->data)->w;
    camera_world_to_view(w, w2v);
    camera_view_to_frustum(o->type, w, v2f);
    return true;
}

/* ---------------------------------------------------------------------- */
/* Draw contexts (data in q3_shapes.c) */

/* The draw context's destination and frame: the port (or pixmap) and the
 * pane's place and size in it. */
static bool dctx_area(gaddr dctx, gaddr *port, int *x, int *y, int *w, int *h, bool *pixmap)
{
    q3obj *o = q3_get(dctx);
    if (!o || !o->data)
        return false;
    const uint32_t *d = ((q3node *)o->data)->w;
    *pixmap = o->type == DC_PIXMAP;
    if (*pixmap) {
        *port = 0;
        *x = *y = 0;
        *w = (int)d[18];
        *h = (int)d[19];
    } else if (o->type == DC_MAC) {
        *port = d[20] ? d[20] : d[17];
        if (!*port)
            return false;
        qrect r = qrect_get(*port + GrafPort_portRect_off);
        *x = r.left;
        *y = r.top;
        *w = r.right - r.left;
        *h = r.bottom - r.top;
    } else {
        return false;
    }
    if (d[9]) { /* the pane */
        int x0 = (int)floorf(q3d_f(d, 5)), y0 = (int)floorf(q3d_f(d, 6)), x1 = (int)ceilf(q3d_f(d, 7)),
            y1 = (int)ceilf(q3d_f(d, 8));
        if (x1 > x0 && y1 > y0) {
            *x = *pixmap ? x0 : *x + x0;
            *y = *pixmap ? y0 : *y + y0;
            *w = x1 - x0;
            *h = y1 - y0;
        }
    }
    return *w > 0 && *h > 0 && *w <= 8192 && *h <= 8192;
}

bool q3v_dctx_area(gaddr dctx, gaddr *port, int *x, int *y, int *w, int *h, bool *pixmap)
{
    return dctx_area(dctx, port, x, y, w, h, pixmap);
}

static uint32_t argb_of(const uint32_t *d, int at)
{
    uint32_t out = 0;
    for (int k = 0; k < 4; k++) {
        float f = fminf(fmaxf(q3d_f(d, at + k), 0), 1);
        out = out << 8 | (uint32_t)lrintf(f * 255);
    }
    return out;
}

/* A pixmap draw context's pixels (TQ3Pixmap at word 17). */
static void pixmap_rw(const uint32_t *d, uint32_t *px, int x, int y, int w, int h, bool write)
{
    gaddr image = d[17];
    uint32_t pw = d[18], ph = d[19], rb = d[20], type = d[22], byte_order = d[24];
    int bpp = q3_pixel_bytes((int)type);
    if (!image || type > 5)
        return;
    for (int row = 0; row < h; row++) {
        int ty = y + row;
        if (ty < 0 || (uint32_t)ty >= ph)
            continue;
        for (int col = 0; col < w; col++) {
            int tx = x + col;
            if (tx < 0 || (uint32_t)tx >= pw)
                continue;
            gaddr p = image + (uint32_t)ty * rb + (uint32_t)tx * (uint32_t)bpp;
            uint32_t *v = &px[(size_t)row * (size_t)w + (size_t)col];
            if (!write) {
                *v = q3_pixel_argb((int)type, p);
                continue;
            }
            uint32_t c = *v, out;
            switch (type) {
            case 0:
            case 1: out = type == 0 ? c & 0xFFFFFF : c; break;
            case 5: out = c & 0xFFFFFF; break;
            case 4: out = (c >> 8 & 0xF800) | (c >> 5 & 0x07E0) | (c >> 3 & 0x1F); break;
            default: out = (type == 3 && c >> 31 ? 0x8000u : 0) | (c >> 9 & 0x7C00) | (c >> 6 & 0x03E0) | (c >> 3 & 0x1F); break;
            }
            for (int k = 0; k < bpp; k++) {
                int shift = 8 * (bpp - 1 - k);
                wr8(p + (uint32_t)(byte_order == 1 ? bpp - 1 - k : k), (uint8_t)(out >> shift));
            }
        }
    }
}

static void present(q3view *v)
{
    q3obj *o = q3_get(v->dctx);
    if (!o || !o->data || !v->pixels)
        return;
    const uint32_t *d = ((q3node *)o->data)->w;
    /* A mask: only where its bits are set. */
    if (d[15]) {
        q3node *mask = q3d_kid(o->data, q3_schema_of(o->type), 0, 0);
        uint32_t mw = d[11], mh = d[12], rb = d[13];
        if (mask && mask->bytes) {
            for (int y = 0; y < v->fh; y++)
                for (int x = 0; x < v->fw; x++) {
                    bool on = (uint32_t)x < mw && (uint32_t)y < mh && (uint32_t)y * rb + (uint32_t)x / 8 < mask->nbytes &&
                              (mask->bytes[(uint32_t)y * rb + (uint32_t)x / 8] & (0x80 >> (x & 7)));
                    if (!on)
                        v->pixels[y * v->fw + x] = 0; /* left as it was: below */
                }
        }
    }
    if (o->type == DC_PIXMAP) {
        pixmap_rw(d, v->pixels, v->px, v->py, v->fw, v->fh, true);
        return;
    }
    gaddr port = d[20] ? d[20] : d[17];
    q3_put_port(v->pixels, v->fw, v->fh, port, v->px, v->py);
}

void q3v_present(q3view *v) { present(v); }

void q3v_read_destination(q3view *v)
{
    q3obj *d = q3_get(v->dctx);
    if (!d || !d->data || !v->pixels)
        return;
    const uint32_t *dw = ((q3node *)d->data)->w;
    if (d->type == DC_PIXMAP) {
        pixmap_rw(dw, v->pixels, v->px, v->py, v->fw, v->fh, false);
    } else {
        gaddr port = dw[20] ? dw[20] : dw[17];
        pixdst pd = qd_port_pixels(port);
        q3_get_pixels(v->pixels, v->fw, v->fh, &pd, v->px, v->py);
    }
}

bool q3v_clear_color(gaddr dctx, uint32_t *argb)
{
    q3obj *d = q3_get(dctx);
    if (!d || !d->data)
        return false;
    const uint32_t *dw = ((q3node *)d->data)->w;
    *argb = argb_of(dw, 1);
    return dw[0] == 1; /* kQ3ClearMethodWithColor */
}

/* ---------------------------------------------------------------------- */
/* Textures (data in q3_shapes.c) to the renderer's, kept until edited */

typedef struct {
    gaddr texture;
    uint32_t edit;
    int id, w, h;
} texcache;

static texcache *texs;
static int ntexs, captexs;

uint32_t *q3_storage_pixmap_argb(const uint32_t *w, gaddr storage);
uint32_t *q3_texture_argb(q3obj *t, int *w, int *h);

static int texture_id(gaddr shader)
{
    q3obj *s = q3_get(shader);
    if (!s || s->type != TEXTURE_SHADER || !s->data)
        return 0;
    gaddr tex = ((q3node *)s->data)->w[11];
    q3obj *t = q3_get(tex);
    if (!t)
        return 0;
    for (int i = 0; i < ntexs; i++)
        if (texs[i].texture == tex) {
            if (texs[i].edit == t->edit)
                return texs[i].id;
            r3d_texture_free(texs[i].id);
            texs[i] = texs[--ntexs];
            break;
        }
    int w = 0, h = 0;
    uint32_t *px = q3_texture_argb(t, &w, &h);
    if (!px)
        return 0;
    /* QuickDraw 3D's v runs up the image; the renderer's down. */
    int id = r3d_texture_new(w, h, px, true);
    free(px);
    if (!id)
        return 0;
    if (ntexs == captexs) {
        int cap = captexs ? captexs * 2 : 32;
        texcache *n = realloc(texs, (size_t)cap * sizeof *n);
        if (!n) {
            r3d_texture_free(id);
            return 0;
        }
        texs = n;
        captexs = cap;
    }
    texs[ntexs++] = (texcache){ tex, t->edit, id, w, h };
    return id;
}

/* A texture object gone: its renderer texture with it. */
void q3_texture_forget(gaddr tex)
{
    for (int i = 0; i < ntexs; i++)
        if (texs[i].texture == tex) {
            r3d_texture_free(texs[i].id);
            texs[i] = texs[--ntexs];
            return;
        }
}

/* ---------------------------------------------------------------------- */
/* Lighting */

static void gather_lights(q3view *v)
{
    v->nL = 0;
    v->ambient[0] = v->ambient[1] = v->ambient[2] = 0;
    q3group *g = q3_group_of(v->lights);
    int n = 0;
    for (q3pos *p = g ? g->first : NULL; p; p = p->next)
        n++;
    free(v->L);
    v->L = n ? calloc((size_t)n, sizeof *v->L) : NULL;
    for (q3pos *p = g ? g->first : NULL; p && v->L; p = p->next) {
        q3obj *o = q3_get(p->obj);
        if (!o || !q3_type_is(o->type, Q3_LIGHT) || !o->data)
            continue;
        const uint32_t *w = ((q3node *)o->data)->w;
        if (!w[0]) /* off */
            continue;
        float b = q3d_f(w, 1), col[3] = { q3d_f(w, 2) * b, q3d_f(w, 3) * b, q3d_f(w, 4) * b };
        if (o->type == LIGHT_AMBIENT) {
            for (int k = 0; k < 3; k++)
                v->ambient[k] += col[k];
            continue;
        }
        vlight *l = &v->L[v->nL++];
        memcpy(l->color, col, sizeof col);
        if (o->type == LIGHT_DIRECTIONAL) {
            l->kind = 0;
            for (int k = 0; k < 3; k++)
                l->dir[k] = q3d_f(w, 6 + k);
            norm3(l->dir);
        } else if (o->type == LIGHT_POINT) {
            l->kind = 1;
            l->atten = (int)w[6];
            for (int k = 0; k < 3; k++)
                l->pos[k] = q3d_f(w, 7 + k);
        } else if (o->type == LIGHT_SPOT) {
            l->kind = 2;
            l->atten = (int)w[6];
            for (int k = 0; k < 3; k++) {
                l->pos[k] = q3d_f(w, 7 + k);
                l->dir[k] = q3d_f(w, 10 + k);
            }
            norm3(l->dir);
            l->hot = q3d_f(w, 13);
            l->outer = q3d_f(w, 14);
            l->falloff = (int)w[15];
        } else {
            v->nL--;
        }
    }
}

/* The diffuse and specular light reaching p (world) with normal n. */
static void light_at(const q3view *v, const float p[3], const float n[3], float spc, bool phong, float diff[3],
                     float spec[3])
{
    diff[0] = diff[1] = diff[2] = 0;
    spec[0] = spec[1] = spec[2] = 0;
    float to_eye[3];
    if (phong) {
        if (v->ortho)
            memcpy(to_eye, v->look, sizeof to_eye);
        else
            sub3(v->eye, p, to_eye);
        norm3(to_eye);
    }
    for (int i = 0; i < v->nL; i++) {
        const vlight *l = &v->L[i];
        float L[3], scale = 1;
        if (l->kind == 0) {
            L[0] = -l->dir[0], L[1] = -l->dir[1], L[2] = -l->dir[2];
        } else {
            sub3(l->pos, p, L);
            float d = norm3(L);
            if (l->atten == 1)
                scale = d > 1e-6f ? 1 / d : 1;
            else if (l->atten == 2)
                scale = d > 1e-6f ? 1 / (d * d) : 1;
            if (l->kind == 2) {
                float c = -dot3(L, l->dir), ang = acosf(fminf(fmaxf(c, -1), 1));
                if (ang > l->outer)
                    continue;
                if (ang > l->hot && l->outer > l->hot) {
                    float t = (ang - l->hot) / (l->outer - l->hot);
                    switch (l->falloff) {
                    case 0: break; /* none: a hard edge at the outer angle */
                    case 1: scale *= 1 - t; break;
                    case 2: scale *= expf(-4 * t); break;
                    default: scale *= cosf(t * (float)M_PI / 2); break;
                    }
                }
            }
        }
        float nl = dot3(n, L);
        if (nl <= 0)
            continue;
        for (int k = 0; k < 3; k++)
            diff[k] += l->color[k] * nl * scale;
        if (phong) {
            float h[3] = { L[0] + to_eye[0], L[1] + to_eye[1], L[2] + to_eye[2] };
            norm3(h);
            float nh = dot3(n, h);
            if (nh > 0) {
                float s = powf(nh, spc > 0 ? spc : 1) * scale;
                for (int k = 0; k < 3; k++)
                    spec[k] += l->color[k] * s;
            }
        }
    }
}

/* ---------------------------------------------------------------------- */
/* Projection and clipping */

typedef struct {
    float c[4];       /* clip (frustum) coordinates */
    float rgba[4], kd[3], ks[3], uv[2];
} cvert;

static cvert lerp_cv(const cvert *a, const cvert *b, float t)
{
    cvert o;
    for (int k = 0; k < 4; k++) {
        o.c[k] = a->c[k] + (b->c[k] - a->c[k]) * t;
        o.rgba[k] = a->rgba[k] + (b->rgba[k] - a->rgba[k]) * t;
    }
    for (int k = 0; k < 3; k++) {
        o.kd[k] = a->kd[k] + (b->kd[k] - a->kd[k]) * t;
        o.ks[k] = a->ks[k] + (b->ks[k] - a->ks[k]) * t;
    }
    o.uv[0] = a->uv[0] + (b->uv[0] - a->uv[0]) * t;
    o.uv[1] = a->uv[1] + (b->uv[1] - a->uv[1]) * t;
    return o;
}

/* Inside the near plane (z <= 0) and the far one (z >= -w). */
static float plane_d(const cvert *v, int plane) { return plane == 0 ? -v->c[2] : v->c[2] + v->c[3]; }

static int clip_poly(cvert *in, int n, cvert *out)
{
    cvert tmp[16];
    cvert *src = in, *dst = tmp;
    for (int plane = 0; plane < 2; plane++) {
        int m = 0;
        for (int i = 0; i < n && m < 14; i++) {
            const cvert *a = &src[i], *b = &src[(i + 1) % n];
            float da = plane_d(a, plane), db = plane_d(b, plane);
            if (da >= 0)
                dst[m++] = *a;
            if ((da >= 0) != (db >= 0))
                dst[m++] = lerp_cv(a, b, da / (da - db));
        }
        n = m;
        src = dst;
        dst = plane == 0 ? out : tmp;
    }
    if (src != out)
        memcpy(out, src, (size_t)n * sizeof *out);
    return n;
}

static void to_window(const q3view *v, const cvert *c, r3d_vertex *o)
{
    float w = c->c[3] > 1e-9f ? c->c[3] : 1e-9f;
    o->x = (c->c[0] / w + 1) * (float)v->fw / 2;
    o->y = (1 - c->c[1] / w) * (float)v->fh / 2;
    o->z = -c->c[2] / w;
    o->invw = v->ortho ? 1 : 1 / w;
    memcpy(&o->r, c->rgba, 16);
    o->u = c->uv[0];
    o->v = c->uv[1];
    o->kd_r = c->kd[0], o->kd_g = c->kd[1], o->kd_b = c->kd[2];
    o->ks_r = c->ks[0], o->ks_g = c->ks[1], o->ks_b = c->ks[2];
}

static bool linear_filter(const q3view *v)
{
    q3obj *r = q3_get(v->renderer);
    /* The renderer's RAVE texture filter: kQATextureFilter_Fast is nearest. */
    return !(r && r->data && ((q3node *)r->data)->w[5] == 0);
}

static void draw_tri(q3view *v, cvert *tri, int texture, bool clamp_u, bool clamp_v, bool transparent)
{
    cvert poly[16];
    int n = clip_poly(tri, 3, poly);
    if (n < 3)
        return;
    r3d_vertex rv[16];
    for (int i = 0; i < n; i++)
        to_window(v, &poly[i], &rv[i]);
    r3d_state s;
    memset(&s, 0, sizeof s);
    s.zfunc = R3D_Z_LE;
    s.zwrite = !transparent;
    s.blend = transparent ? R3D_BLEND_INTERPOLATE : R3D_BLEND_NONE;
    s.texture = texture;
    s.texop = R3D_TEXOP_MODULATE;
    s.linear = linear_filter(v);
    s.clamp_u = clamp_u;
    s.clamp_v = clamp_v;
    for (int i = 1; i + 1 < n; i++) {
        r3d_vertex t3[3] = { rv[0], rv[i], rv[i + 1] };
        if (transparent) {
            if (v->ndefer == v->capdefer) {
                int cap = v->capdefer ? v->capdefer * 2 : 64;
                deferred *d = realloc(v->defer, (size_t)cap * sizeof *d);
                if (!d)
                    return;
                v->defer = d;
                v->capdefer = cap;
            }
            deferred *d = &v->defer[v->ndefer++];
            memcpy(d->v, t3, sizeof t3);
            d->depth = t3[0].z + t3[1].z + t3[2].z;
            d->texture = texture;
            d->clamp_u = clamp_u;
            d->clamp_v = clamp_v;
        } else {
            r3d_draw(v->frame, &s, R3D_TRIANGLES, t3, 3);
        }
    }
}

static void draw_seg(q3view *v, cvert *a, cvert *b, float size, bool point)
{
    cvert in[2] = { *a, *b }, out[16];
    r3d_state s;
    memset(&s, 0, sizeof s);
    s.zfunc = R3D_Z_LE;
    s.zwrite = true;
    s.size = size;
    if (point) {
        if (plane_d(a, 0) < 0 || plane_d(a, 1) < 0)
            return;
        r3d_vertex rv;
        to_window(v, a, &rv);
        r3d_draw(v->frame, &s, R3D_POINTS, &rv, 1);
        return;
    }
    /* A segment against the two planes. */
    for (int plane = 0; plane < 2; plane++) {
        float da = plane_d(&in[0], plane), db = plane_d(&in[1], plane);
        if (da < 0 && db < 0)
            return;
        if (da < 0)
            in[0] = lerp_cv(&in[0], &in[1], da / (da - db));
        else if (db < 0)
            in[1] = lerp_cv(&in[0], &in[1], da / (da - db));
    }
    r3d_vertex rv[2];
    to_window(v, &in[0], &rv[0]);
    to_window(v, &in[1], &rv[1]);
    (void)out;
    r3d_draw(v->frame, &s, R3D_LINES, rv, 2);
}

/* ---------------------------------------------------------------------- */
/* What geometries hand the view */

static void bound_point(q3view *v, const float p[3])
{
    float w[3];
    q3m_point(v->st.m, p, w);
    if (v->bempty) {
        memcpy(v->bmin, w, sizeof w);
        memcpy(v->bmax, w, sizeof w);
        v->bempty = false;
    } else {
        for (int k = 0; k < 3; k++) {
            v->bmin[k] = fminf(v->bmin[k], w[k]);
            v->bmax[k] = fmaxf(v->bmax[k], w[k]);
        }
    }
    if (v->loop == LOOP_BOUND_SPHERE) {
        if (v->nbpts == v->capbpts) {
            uint32_t cap = v->capbpts ? v->capbpts * 2 : 256;
            float *n = realloc(v->bpts, (size_t)cap * 3 * sizeof *n);
            if (!n)
                return;
            v->bpts = n;
            v->capbpts = cap;
        }
        memcpy(&v->bpts[v->nbpts++ * 3], w, sizeof w);
    }
}

/* A point of the frame (window coordinates and depth 0..1). */
static bool window_of(const q3view *v, const float p[3], float out[3], float wc[4])
{
    float world[3];
    q3m_point(v->st.m, p, world);
    point4(v->w2f, world, wc);
    if (-wc[2] < 0 || wc[2] + wc[3] < 0 || wc[3] <= 1e-9f)
        return false;
    out[0] = (wc[0] / wc[3] + 1) * (float)v->fw / 2;
    out[1] = (1 - wc[1] / wc[3]) * (float)v->fh / 2;
    out[2] = -wc[2] / wc[3];
    return true;
}

static void hit(q3view *v, const float local[3], float depth)
{
    q3hit h;
    memset(&h, 0, sizeof h);
    h.object = v->object;
    h.root = v->pathlen ? v->object : 0;
    h.pickid = v->st.pickid;
    h.depth = (uint32_t)(v->pathlen < 64 ? v->pathlen : 64);
    h.positions = v->path;
    q3m_point(v->st.m, local, h.xyz);
    h.distance = v->ortho ? depth : sqrtf((h.xyz[0] - v->eye[0]) * (h.xyz[0] - v->eye[0]) +
                                          (h.xyz[1] - v->eye[1]) * (h.xyz[1] - v->eye[1]) +
                                          (h.xyz[2] - v->eye[2]) * (h.xyz[2] - v->eye[2]));
    memcpy(h.m, v->st.m, sizeof h.m);
    q3_pick_hit(v->pick, &h);
}

static float seg_dist(const float *p, const float *a, const float *b, float *t)
{
    float dx = b[0] - a[0], dy = b[1] - a[1], l2 = dx * dx + dy * dy;
    float u = l2 > 0 ? ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / l2 : 0;
    u = fminf(fmaxf(u, 0), 1);
    *t = u;
    float x = a[0] + u * dx - p[0], y = a[1] + u * dy - p[1];
    return sqrtf(x * x + y * y);
}

static bool pick_area(const q3view *v, const float w[3])
{
    return w[0] >= v->pq.rect[0] && w[0] <= v->pq.rect[2] && w[1] >= v->pq.rect[1] && w[1] <= v->pq.rect[3];
}

static void pick_tri(q3view *v, const q3pv *a, const q3pv *b, const q3pv *c)
{
    float wa[3], wb[3], wc[3], h[4];
    if (!window_of(v, a->p, wa, h) || !window_of(v, b->p, wb, h) || !window_of(v, c->p, wc, h))
        return;
    if (v->pq.kind == Q3PICK_RECT) {
        if (pick_area(v, wa) || pick_area(v, wb) || pick_area(v, wc))
            hit(v, a->p, wa[2]);
        return;
    }
    if (v->pq.kind != Q3PICK_POINT)
        return;
    const float *p = v->pq.pt;
    float d0 = (wb[0] - wa[0]) * (p[1] - wa[1]) - (wb[1] - wa[1]) * (p[0] - wa[0]);
    float d1 = (wc[0] - wb[0]) * (p[1] - wb[1]) - (wc[1] - wb[1]) * (p[0] - wb[0]);
    float d2 = (wa[0] - wc[0]) * (p[1] - wc[1]) - (wa[1] - wc[1]) * (p[0] - wc[0]);
    bool in = (d0 >= 0 && d1 >= 0 && d2 >= 0) || (d0 <= 0 && d1 <= 0 && d2 <= 0);
    float t;
    if (!in && v->pq.etol > 0)
        in = seg_dist(p, wa, wb, &t) <= v->pq.etol || seg_dist(p, wb, wc, &t) <= v->pq.etol ||
             seg_dist(p, wc, wa, &t) <= v->pq.etol;
    if (!in)
        return;
    /* Where: barycentric in the window. */
    float area = (wb[0] - wa[0]) * (wc[1] - wa[1]) - (wb[1] - wa[1]) * (wc[0] - wa[0]);
    float l1 = area ? ((p[0] - wa[0]) * (wc[1] - wa[1]) - (p[1] - wa[1]) * (wc[0] - wa[0])) / area : 0;
    float l2 = area ? ((wb[0] - wa[0]) * (p[1] - wa[1]) - (wb[1] - wa[1]) * (p[0] - wa[0])) / area : 0;
    float local[3];
    for (int k = 0; k < 3; k++)
        local[k] = a->p[k] + l1 * (b->p[k] - a->p[k]) + l2 * (c->p[k] - a->p[k]);
    hit(v, local, wa[2] + l1 * (wb[2] - wa[2]) + l2 * (wc[2] - wa[2]));
}

/* The world-ray pick against a triangle (Moller-Trumbore). */
static void pick_ray_tri(q3view *v, const q3pv *a, const q3pv *b, const q3pv *c)
{
    float A[3], B[3], C[3];
    q3m_point(v->st.m, a->p, A);
    q3m_point(v->st.m, b->p, B);
    q3m_point(v->st.m, c->p, C);
    const float *o = v->pq.ray, *d = v->pq.ray + 3;
    float e1[3], e2[3], pv[3], tv[3], qv[3];
    sub3(B, A, e1);
    sub3(C, A, e2);
    cross3(d, e2, pv);
    float det = dot3(e1, pv);
    if (fabsf(det) < 1e-12f)
        return;
    sub3(o, A, tv);
    float u = dot3(tv, pv) / det;
    if (u < 0 || u > 1)
        return;
    cross3(tv, e1, qv);
    float vv = dot3(d, qv) / det;
    if (vv < 0 || u + vv > 1)
        return;
    float t = dot3(e2, qv) / det;
    if (t < 0)
        return;
    q3hit h;
    memset(&h, 0, sizeof h);
    h.object = v->object;
    h.pickid = v->st.pickid;
    h.depth = (uint32_t)(v->pathlen < 64 ? v->pathlen : 64);
    h.positions = v->path;
    for (int k = 0; k < 3; k++)
        h.xyz[k] = o[k] + t * d[k];
    h.distance = t * sqrtf(dot3(d, d));
    cross3(e1, e2, h.normal);
    norm3(h.normal);
    memcpy(h.m, v->st.m, sizeof h.m);
    q3_pick_hit(v->pick, &h);
}

void q3v_triangles(q3view *v, const q3pv *pv, uint32_t npv, const q3ptri *t, uint32_t nt, gaddr geom_set)
{
    if (!v || !npv || !nt)
        return;
    if (v->xr && v->loop == LOOP_RENDER) {
        q3xr_triangles(v, pv, npv, t, nt, geom_set);
        return;
    }
    if (v->loop == LOOP_BOUND_BOX || v->loop == LOOP_BOUND_SPHERE) {
        for (uint32_t i = 0; i < npv; i++)
            bound_point(v, pv[i].p);
        return;
    }
    if (v->loop == LOOP_PICK) {
        for (uint32_t i = 0; i < nt; i++) {
            if (t[i].v[0] >= npv || t[i].v[1] >= npv || t[i].v[2] >= npv)
                continue;
            if (v->pq.kind == Q3PICK_RAY)
                pick_ray_tri(v, &pv[t[i].v[0]], &pv[t[i].v[1]], &pv[t[i].v[2]]);
            else
                pick_tri(v, &pv[t[i].v[0]], &pv[t[i].v[1]], &pv[t[i].v[2]]);
        }
        return;
    }
    if (v->loop != LOOP_RENDER || !v->frame)
        return;
    q3obj *rend = q3_get(v->renderer);
    if (rend && rend->type == RENDERER_GENERIC)
        return;
    bool wire = (rend && rend->type == RENDERER_WIREFRAME) || v->st.fill == kQ3FillStyleEdges;
    bool points = v->st.fill == kQ3FillStylePoints;
    q3attrs ga;
    memset(&ga, 0, sizeof ga);
    q3attrs_fill(&ga, geom_set);
    q3attrs_merge(&ga, &v->st.a);
    float nm[16], inv[16];
    if (q3m_invert(v->st.m, inv)) {
        /* Normals by the inverse transpose. */
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                nm[i * 4 + j] = inv[j * 4 + i];
    } else {
        memcpy(nm, v->st.m, sizeof nm);
    }
    float (*world)[3] = malloc(npv * sizeof *world);
    float (*clip)[4] = malloc(npv * sizeof *clip);
    if (!world || !clip) {
        free(world);
        free(clip);
        return;
    }
    for (uint32_t i = 0; i < npv; i++) {
        q3m_point(v->st.m, pv[i].p, world[i]);
        point4(v->w2f, world[i], clip[i]);
    }
    bool phong = v->st.illumination == ILLUM_PHONG, lit = v->st.illumination != ILLUM_NULL;
    bool flat = v->st.interpolation == kQ3InterpolationStyleNone;
    bool cw = v->st.orientation == 1;
    for (uint32_t i = 0; i < nt; i++) {
        const q3ptri *tr = &t[i];
        if (tr->v[0] >= npv || tr->v[1] >= npv || tr->v[2] >= npv)
            continue;
        const float *P[3] = { world[tr->v[0]], world[tr->v[1]], world[tr->v[2]] };
        float e1[3], e2[3], fn[3];
        sub3(P[1], P[0], e1);
        sub3(P[2], P[0], e2);
        cross3(e1, e2, fn);
        if (cw)
            fn[0] = -fn[0], fn[1] = -fn[1], fn[2] = -fn[2];
        if (norm3(fn) < 1e-20f)
            continue;
        float toeye[3];
        if (v->ortho)
            memcpy(toeye, v->look, sizeof toeye);
        else
            sub3(v->eye, P[0], toeye);
        bool back = dot3(fn, toeye) < 0;
        if (back && v->st.backfacing == kQ3BackfacingStyleRemove)
            continue;
        bool flip = back && v->st.backfacing == kQ3BackfacingStyleFlip;
        q3attrs fa = tr->a;
        q3attrs_merge(&fa, &ga);
        /* A face's own normal, if it has one, is the face's. */
        float face_n[3];
        if (tr->a.has & QA_N) {
            q3m_vector(nm, tr->a.n, face_n);
            norm3(face_n);
        } else {
            memcpy(face_n, fn, sizeof face_n);
        }
        cvert cv[3];
        bool transparent = false;
        int texture = 0;
        bool clamp_u = false, clamp_v = false;
        gaddr shader = (fa.has & QA_SHADER) ? fa.shader : v->st.texture;
        if (shader && !points) {
            texture = texture_id(shader);
            q3obj *sh = q3_get(shader);
            if (sh && sh->data) {
                clamp_u = ((q3node *)sh->data)->w[9] == 1;
                clamp_v = ((q3node *)sh->data)->w[10] == 1;
            }
        }
        for (int k = 0; k < 3; k++) {
            q3attrs a = pv[tr->v[k]].a;
            q3attrs_merge(&a, &fa);
            if (a.hilite && v->st.highlight) {
                q3attrs h;
                memset(&h, 0, sizeof h);
                q3attrs_fill(&h, v->st.highlight);
                q3attrs_merge(&h, &a);
                a = h;
            }
            float n[3];
            if (!flat && (pv[tr->v[k]].a.has & QA_N)) {
                q3m_vector(nm, pv[tr->v[k]].a.n, n);
                norm3(n);
            } else {
                memcpy(n, face_n, sizeof n);
            }
            if (flip)
                n[0] = -n[0], n[1] = -n[1], n[2] = -n[2];
            float diff[3] = { 1, 1, 1 }, spec[3] = { 0, 0, 0 };
            if (lit) {
                light_at(v, flat ? P[0] : P[k], n, a.spc, phong, diff, spec);
                for (int c = 0; c < 3; c++)
                    diff[c] += v->ambient[c] * a.amb;
            }
            cvert *o = &cv[k];
            memcpy(o->c, clip[tr->v[k]], sizeof o->c);
            float alpha = (a.kt[0] + a.kt[1] + a.kt[2]) / 3;
            if (alpha < 0.999f)
                transparent = true;
            for (int c = 0; c < 3; c++) {
                float ks = phong ? a.ks[c] * spec[c] : 0;
                if (texture) {
                    o->kd[c] = fminf(diff[c], 1);
                    o->ks[c] = ks;
                    o->rgba[c] = 1;
                } else {
                    o->rgba[c] = fminf(a.kd[c] * diff[c] + ks, 1);
                    o->kd[c] = 1;
                    o->ks[c] = 0;
                }
            }
            o->rgba[3] = alpha;
            /* u, v with v from the bottom; the renderer's from the top. */
            o->uv[0] = (a.has & QA_UV) ? a.uv[0] : 0;
            o->uv[1] = (a.has & QA_UV) ? 1 - a.uv[1] : 0;
        }
        if (texture) {
            q3obj *sh = q3_get(shader);
            if (sh && sh->data) { /* the shader's uv transform */
                const uint32_t *w = ((q3node *)sh->data)->w;
                for (int k = 0; k < 3; k++) {
                    float u = cv[k].uv[0], vv = 1 - cv[k].uv[1];
                    float nu = u * q3d_f(w, 0) + vv * q3d_f(w, 3) + q3d_f(w, 6);
                    float nv = u * q3d_f(w, 1) + vv * q3d_f(w, 4) + q3d_f(w, 7);
                    cv[k].uv[0] = nu;
                    cv[k].uv[1] = 1 - nv;
                }
            }
        }
        if (wire || points) {
            for (int k = 0; k < 3; k++) {
                if (points) {
                    draw_seg(v, &cv[k], &cv[k], 2, true);
                } else if (tr->edges & (1 << k)) {
                    draw_seg(v, &cv[k], &cv[(k + 1) % 3], 1, false);
                }
            }
            continue;
        }
        draw_tri(v, cv, texture, clamp_u, clamp_v, transparent);
    }
    free(world);
    free(clip);
}

static void unlit(q3view *v, const q3pv *p, gaddr geom_set, cvert *o)
{
    q3attrs a = p->a;
    q3attrs ga;
    memset(&ga, 0, sizeof ga);
    q3attrs_fill(&ga, geom_set);
    q3attrs_merge(&a, &ga);
    q3attrs_merge(&a, &v->st.a);
    float w[3];
    q3m_point(v->st.m, p->p, w);
    point4(v->w2f, w, o->c);
    memcpy(o->rgba, a.kd, 12);
    o->rgba[3] = 1;
    o->kd[0] = o->kd[1] = o->kd[2] = 1;
    o->ks[0] = o->ks[1] = o->ks[2] = 0;
    o->uv[0] = o->uv[1] = 0;
}

void q3v_lines(q3view *v, const q3pv *pv, uint32_t nseg, gaddr geom_set)
{
    if (!v)
        return;
    if (v->xr && v->loop == LOOP_RENDER) {
        q3xr_lines(v, pv, nseg, geom_set);
        return;
    }
    if (v->loop == LOOP_BOUND_BOX || v->loop == LOOP_BOUND_SPHERE) {
        for (uint32_t i = 0; i < 2 * nseg; i++)
            bound_point(v, pv[i].p);
        return;
    }
    if (v->loop == LOOP_PICK) {
        for (uint32_t i = 0; i < nseg; i++) {
            float a[3], b[3], h[4], t;
            if (!window_of(v, pv[2 * i].p, a, h) || !window_of(v, pv[2 * i + 1].p, b, h))
                continue;
            if (v->pq.kind == Q3PICK_RECT ? pick_area(v, a) || pick_area(v, b)
                                          : v->pq.kind == Q3PICK_POINT &&
                                                seg_dist(v->pq.pt, a, b, &t) <= fmaxf(v->pq.etol, 1)) {
                hit(v, pv[2 * i].p, a[2]);
            }
        }
        return;
    }
    if (v->loop != LOOP_RENDER || !v->frame)
        return;
    for (uint32_t i = 0; i < nseg; i++) {
        cvert a, b;
        unlit(v, &pv[2 * i], geom_set, &a);
        unlit(v, &pv[2 * i + 1], geom_set, &b);
        draw_seg(v, &a, &b, 1, false);
    }
}

void q3v_points(q3view *v, const q3pv *pv, uint32_t n, gaddr geom_set)
{
    if (!v)
        return;
    if (v->xr && v->loop == LOOP_RENDER) {
        q3xr_points(v, pv, n, geom_set);
        return;
    }
    if (v->loop == LOOP_BOUND_BOX || v->loop == LOOP_BOUND_SPHERE) {
        for (uint32_t i = 0; i < n; i++)
            bound_point(v, pv[i].p);
        return;
    }
    if (v->loop == LOOP_PICK) {
        for (uint32_t i = 0; i < n; i++) {
            float a[3], h[4];
            if (!window_of(v, pv[i].p, a, h))
                continue;
            float dx = a[0] - v->pq.pt[0], dy = a[1] - v->pq.pt[1];
            if (v->pq.kind == Q3PICK_RECT ? pick_area(v, a)
                                          : v->pq.kind == Q3PICK_POINT &&
                                                sqrtf(dx * dx + dy * dy) <= fmaxf(v->pq.vtol, 1))
                hit(v, pv[i].p, a[2]);
        }
        return;
    }
    if (v->loop != LOOP_RENDER || !v->frame)
        return;
    for (uint32_t i = 0; i < n; i++) {
        cvert a;
        unlit(v, &pv[i], geom_set, &a);
        draw_seg(v, &a, &a, 1, true);
    }
}

void q3v_image(q3view *v, const float p[3], int dx, int dy, int w, int h, const uint32_t *argb, gaddr set)
{
    if (!v)
        return;
    if (v->xr && v->loop == LOOP_RENDER) {
        q3xr_image(v, p, dx, dy, w, h, argb, set);
        return;
    }
    if (v->loop == LOOP_BOUND_BOX || v->loop == LOOP_BOUND_SPHERE) {
        bound_point(v, p);
        return;
    }
    float at[3], hc[4];
    if (!window_of(v, p, at, hc))
        return;
    float x0 = at[0] + (float)dx, y0 = at[1] + (float)dy, x1 = x0 + (float)w, y1 = y0 + (float)h;
    if (v->loop == LOOP_PICK) {
        if (v->pq.kind == Q3PICK_POINT && v->pq.pt[0] >= x0 && v->pq.pt[0] < x1 && v->pq.pt[1] >= y0 &&
            v->pq.pt[1] < y1)
            hit(v, p, at[2]);
        return;
    }
    if (v->loop != LOOP_RENDER || !v->frame)
        return;
    int id = r3d_texture_new(w, h, argb, false);
    if (!id)
        return;
    if (v->nframe_tex == v->capframe_tex) {
        int cap = v->capframe_tex ? v->capframe_tex * 2 : 8;
        int *n = realloc(v->frame_textures, (size_t)cap * sizeof *n);
        if (!n) {
            r3d_texture_free(id);
            return;
        }
        v->frame_textures = n;
        v->capframe_tex = cap;
    }
    v->frame_textures[v->nframe_tex++] = id;
    r3d_vertex q[6];
    float xs[6] = { x0, x1, x0, x1, x1, x0 }, ys[6] = { y0, y0, y1, y0, y1, y1 };
    float us[6] = { 0, 1, 0, 1, 1, 0 }, vs[6] = { 0, 0, 1, 0, 1, 1 };
    for (int i = 0; i < 6; i++)
        q[i] = (r3d_vertex){ xs[i], ys[i], at[2], 1, 1, 1, 1, 1, us[i], vs[i], 1, 1, 1, 0, 0, 0 };
    r3d_state s;
    memset(&s, 0, sizeof s);
    s.zfunc = R3D_Z_LE;
    s.zwrite = true;
    s.blend = R3D_BLEND_INTERPOLATE;
    s.texture = id;
    s.texop = R3D_TEXOP_MODULATE;
    s.clamp_u = s.clamp_v = true;
    r3d_draw(v->frame, &s, R3D_TRIANGLES, q, 6);
    (void)set;
}

/* ---------------------------------------------------------------------- */
/* The loops */

static bool frame_setup(q3view *v)
{
    gaddr port = 0;
    int x = 0, y = 0, w = 0, h = 0;
    bool pixmap = false;
    if (!dctx_area(v->dctx, &port, &x, &y, &w, &h, &pixmap))
        return false;
    if (!v->frame || r3d_frame_width(v->frame) != w || r3d_frame_height(v->frame) != h) {
        drop_rave_context(v);
        r3d_frame_free(v->frame);
        free(v->pixels);
        v->frame = r3d_frame_new(w, h);
        v->pixels = malloc((size_t)w * (size_t)h * 4);
        if (!v->frame || !v->pixels) {
            r3d_frame_free(v->frame);
            free(v->pixels);
            v->frame = NULL;
            v->pixels = NULL;
            return false;
        }
    }
    v->fw = w;
    v->fh = h;
    v->px = x;
    v->py = y;
    return true;
}

static bool plugin_frame_setup(q3view *v)
{
    gaddr port = 0;
    int x = 0, y = 0, w = 0, h = 0;
    bool pixmap = false;
    if (!dctx_area(v->dctx, &port, &x, &y, &w, &h, &pixmap))
        return false;
    if (!v->pixels || v->fw != w || v->fh != h) {
        drop_rave_context(v);
        r3d_frame_free(v->frame);
        v->frame = NULL;
        free(v->pixels);
        v->pixels = malloc((size_t)w * (size_t)h * 4);
        if (!v->pixels)
            return false;
    }
    v->fw = w;
    v->fh = h;
    v->px = x;
    v->py = y;
    return true;
}

/* The camera's matrices and the frustum to the frame. */
static bool camera_setup(q3view *v)
{
    if (!q3_camera_matrices(v->camera, v->w2v, v->v2f))
        return false;
    q3m_mul(v->w2v, v->v2f, v->w2f);
    q3obj *cam = q3_get(v->camera);
    const uint32_t *w = ((q3node *)cam->data)->w;
    v->ortho = cam->type == CAMERA_ORTH;
    for (int k = 0; k < 3; k++)
        v->eye[k] = q3d_f(w, k);
    float poi[3] = { q3d_f(w, 3), q3d_f(w, 4), q3d_f(w, 5) };
    sub3(v->eye, poi, v->look);
    norm3(v->look);
    q3m_identity(v->f2w);
    v->f2w[0] = (float)v->fw / 2;
    v->f2w[5] = -(float)v->fh / 2;
    v->f2w[12] = (float)v->fw / 2 + (float)v->px;
    v->f2w[13] = (float)v->fh / 2 + (float)v->py;
    return true;
}

static void start_loop(q3view *v, int loop)
{
    default_state(v);
    v->loop = loop;
    v->cancelled = false;
    v->retraverse = false;
    v->pathlen = 0;
    v->object = 0;
}

static void end_frame_textures(q3view *v)
{
    for (int i = 0; i < v->nframe_tex; i++)
        r3d_texture_free(v->frame_textures[i]);
    v->nframe_tex = 0;
}

static int cmp_defer(const void *a, const void *b)
{
    float da = ((const deferred *)a)->depth, db = ((const deferred *)b)->depth;
    return da < db ? 1 : da > db ? -1 : 0;
}

static void Q3View_New(ppc_cpu *c)
{
    q3view *v = calloc(1, sizeof *v);
    q3obj *o = v ? q3_new(Q3_VIEW, v) : NULL;
    if (!o) {
        free(v);
        Q3RETURN(0);
        return;
    }
    v->g = o->g;
    v->renderer = 0;
    default_state(v);
    for (int i = 0; i < MAX_VIEWS; i++)
        if (!views[i]) {
            views[i] = v;
            break;
        }
    Q3RETURN(o->g);
}

static void view_dispose(q3obj *o)
{
    q3view *v = o->data;
    if (!v)
        return;
    for (int i = 0; i < MAX_VIEWS; i++)
        if (views[i] == v)
            views[i] = NULL;
    drop_rave_context(v);
    q3xr_free(v);
    q3_assign(&v->renderer, 0);
    q3_assign(&v->camera, 0);
    q3_assign(&v->lights, 0);
    q3_assign(&v->dctx, 0);
    q3_assign(&v->default_set, 0);
    r3d_frame_free(v->frame);
    end_frame_textures(v);
    free(v->frame_textures);
    free(v->pixels);
    free(v->stack);
    free(v->L);
    free(v->defer);
    free(v->bpts);
    free(v);
}

static q3view *view_arg(ppc_cpu *c) { return q3v_of(Q3R(0)); }

static void Q3View_Cancel(ppc_cpu *c)
{
    q3view *v = view_arg(c);
    if (v) {
        v->cancelled = true;
        if (v->xr)
            q3xr_cancel(v);
    }
    Q3RETURN(v ? kQ3Success : kQ3Failure);
}

gaddr q3_renderer_new(uint32_t type);

static void Q3View_SetRendererByType(ppc_cpu *c)
{
    q3view *v = view_arg(c);
    gaddr r = v ? q3_renderer_new(Q3R(1)) : 0;
    if (r) {
        q3_assign(&v->renderer, r);
        q3_release(r);
    }
    Q3RETURN(r ? kQ3Success : kQ3Failure);
}

#define VIEW_SLOT(Set, Get, field, type)                                                                             \
    static void Set(ppc_cpu *c)                                                                                      \
    {                                                                                                                \
        q3view *v = view_arg(c);                                                                                     \
        if (v && Q3R(1) && !q3_of(Q3R(1), type)) {                                                                   \
            Q3RETURN(kQ3Failure);                                                                                    \
            return;                                                                                                  \
        }                                                                                                            \
        if (v)                                                                                                       \
            q3_assign(&v->field, Q3R(1));                                                                            \
        Q3RETURN(v ? kQ3Success : kQ3Failure);                                                                       \
    }                                                                                                                \
    static void Get(ppc_cpu *c)                                                                                      \
    {                                                                                                                \
        q3view *v = view_arg(c);                                                                                     \
        if (v && Q3R(1))                                                                                             \
            wr32(Q3R(1), v->field ? q3_ref(v->field) : 0);                                                           \
        Q3RETURN(v && Q3R(1) ? kQ3Success : kQ3Failure);                                                             \
    }

VIEW_SLOT(Q3View_SetRenderer, Q3View_GetRenderer, renderer, Q3_RENDERER)
VIEW_SLOT(Q3View_SetCamera, Q3View_GetCamera, camera, Q3_CAMERA)
VIEW_SLOT(Q3View_SetLightGroup, Q3View_GetLightGroup, lights, Q3_LIGHT_GROUP)
VIEW_SLOT(Q3View_SetDrawContext, Q3View_GetDrawContext, dctx, Q3_DRAW_CONTEXT)
VIEW_SLOT(Q3View_SetDefaultAttributeSet, Q3View_GetDefaultAttributeSet, default_set, Q3_ATTRIBUTE_SET)

static void Q3View_StartRendering(ppc_cpu *c)
{
    q3view *v = view_arg(c);
    if (!v) {
        Q3RETURN(kQ3Failure);
        return;
    }
    if (!v->dctx || !v->camera) {
        q3_error(!v->dctx ? kQ3ErrorDrawContextNotSet : kQ3ErrorCameraNotSet);
        Q3RETURN(kQ3Failure);
        return;
    }
    if (!v->renderer) /* no renderer set: the interactive one */
        v->renderer = q3_renderer_new(Q3T('c', 't', 'w', 'n'));
    if (q3xr_is_plugin(v->renderer)) {
        /* A plug-in draws for itself: the frame is only its pixels. */
        if (!plugin_frame_setup(v) || !camera_setup(v)) {
            q3_error(kQ3ErrorBadDrawContext);
            Q3RETURN(kQ3Failure);
            return;
        }
        start_loop(v, LOOP_RENDER);
        Q3RETURN(q3xr_start(v) ? kQ3Success : kQ3Failure);
        return;
    }
    if (!frame_setup(v) || !camera_setup(v)) {
        q3_error(kQ3ErrorBadDrawContext);
        Q3RETURN(kQ3Failure);
        return;
    }
    start_loop(v, LOOP_RENDER);
    gather_lights(v);
    v->ndefer = 0;
    q3obj *d = q3_get(v->dctx);
    const uint32_t *dw = ((q3node *)d->data)->w;
    if (dw[0] == 1) { /* kQ3ClearMethodWithColor */
        r3d_begin(v->frame, NULL, argb_of(dw, 1), 1.0f);
    } else { /* what's there now */
        if (d->type == DC_PIXMAP) {
            pixmap_rw(dw, v->pixels, v->px, v->py, v->fw, v->fh, false);
        } else {
            gaddr port = dw[20] ? dw[20] : dw[17];
            pixdst pd = qd_port_pixels(port);
            q3_get_pixels(v->pixels, v->fw, v->fh, &pd, v->px, v->py);
        }
        r3d_begin(v->frame, v->pixels, 0, 1.0f);
    }
    Q3RETURN(kQ3Success);
}

static void Q3View_EndRendering(ppc_cpu *c)
{
    q3view *v = view_arg(c);
    if (!v || v->loop != LOOP_RENDER) {
        if (v)
            q3_error(kQ3ErrorViewNotStarted);
        Q3RETURN(kQ3ViewStatusError);
        return;
    }
    if (v->xr && !v->cancelled) {
        int status = q3xr_end_pass(v);
        if (status == kQ3ViewStatusRetraverse) {
            Q3RETURN(status);
            return;
        }
        v->loop = LOOP_NONE;
        Q3RETURN(status);
        return;
    }
    if (v->cancelled) {
        if (v->xr) {
            v->loop = LOOP_NONE;
            Q3RETURN(kQ3ViewStatusCancelled);
            return;
        }
        r3d_end(v->frame, NULL);
        end_frame_textures(v);
        v->loop = LOOP_NONE;
        Q3RETURN(kQ3ViewStatusCancelled);
        return;
    }
    /* The transparent, back to front. */
    if (v->ndefer) {
        qsort(v->defer, (size_t)v->ndefer, sizeof *v->defer, cmp_defer);
        for (int i = 0; i < v->ndefer; i++) {
            r3d_state s;
            memset(&s, 0, sizeof s);
            s.zfunc = R3D_Z_LE;
            s.blend = R3D_BLEND_INTERPOLATE;
            s.texture = v->defer[i].texture;
            s.texop = R3D_TEXOP_MODULATE;
            s.linear = linear_filter(v);
            s.clamp_u = v->defer[i].clamp_u;
            s.clamp_v = v->defer[i].clamp_v;
            r3d_draw(v->frame, &s, R3D_TRIANGLES, v->defer[i].v, 3);
        }
        v->ndefer = 0;
    }
    r3d_end(v->frame, v->pixels);
    end_frame_textures(v);
    v->loop = LOOP_NONE;
    present(v);
    if (v->end_frame) {
        uint32_t args[2] = { v->g, v->end_frame_data };
        ppc_call(v->end_frame, args, 2);
    }
    Q3RETURN(kQ3ViewStatusDone);
}

static void Q3View_Flush(ppc_cpu *c)
{
    q3view *v = view_arg(c);
    if (v && v->xr)
        q3xr_flush(v);
    Q3RETURN(v ? kQ3Success : kQ3Failure);
}
static void Q3View_Sync(ppc_cpu *c)
{
    q3view *v = view_arg(c);
    if (v && v->xr)
        q3xr_sync(v);
    Q3RETURN(v ? kQ3Success : kQ3Failure);
}

static void start_bounds(ppc_cpu *c, int loop)
{
    q3view *v = view_arg(c);
    if (!v) {
        Q3RETURN(kQ3Failure);
        return;
    }
    start_loop(v, loop);
    v->exact = Q3R(1) == 0;
    v->bempty = true;
    v->nbpts = 0;
    /* Screen-space subdivision needs a frame; without a camera, none. */
    v->fh = 0;
    Q3RETURN(kQ3Success);
}
void q3v_bound(q3view *v, gaddr obj, bool exact, float box[7])
{
    int loop = v->loop;
    vstate st = v->st;
    int depth = v->depth;
    v->loop = LOOP_BOUND_BOX;
    v->exact = exact;
    v->bempty = true;
    q3m_identity(v->st.m);
    q3v_submit(v, obj);
    for (int k = 0; k < 3; k++) {
        box[k] = v->bempty ? 0 : v->bmin[k];
        box[3 + k] = v->bempty ? 0 : v->bmax[k];
    }
    box[6] = v->bempty;
    v->loop = loop;
    v->st = st;
    v->depth = depth;
}

static void Q3View_StartBoundingBox(ppc_cpu *c) { start_bounds(c, LOOP_BOUND_BOX); }
static void Q3View_StartBoundingSphere(ppc_cpu *c) { start_bounds(c, LOOP_BOUND_SPHERE); }

static void Q3View_EndBoundingBox(ppc_cpu *c)
{
    q3view *v = view_arg(c);
    gaddr r = Q3R(1);
    if (!v || v->loop != LOOP_BOUND_BOX) {
        Q3RETURN(kQ3ViewStatusError);
        return;
    }
    v->loop = LOOP_NONE;
    if (r) {
        for (int k = 0; k < 3; k++) {
            wrf(r + 4 * (uint32_t)k, v->bempty ? 0 : v->bmin[k]);
            wrf(r + 12 + 4 * (uint32_t)k, v->bempty ? 0 : v->bmax[k]);
        }
        wr32(r + 24, v->bempty);
    }
    Q3RETURN(kQ3ViewStatusDone);
}

static void Q3View_EndBoundingSphere(ppc_cpu *c)
{
    q3view *v = view_arg(c);
    gaddr r = Q3R(1);
    if (!v || v->loop != LOOP_BOUND_SPHERE) {
        Q3RETURN(kQ3ViewStatusError);
        return;
    }
    v->loop = LOOP_NONE;
    float ctr[3] = { 0, 0, 0 }, rad = 0;
    if (!v->bempty) {
        for (int k = 0; k < 3; k++)
            ctr[k] = (v->bmin[k] + v->bmax[k]) / 2;
        for (uint32_t i = 0; i < v->nbpts; i++) {
            float d[3];
            sub3(&v->bpts[i * 3], ctr, d);
            rad = fmaxf(rad, sqrtf(dot3(d, d)));
        }
    }
    if (r) {
        for (int k = 0; k < 3; k++)
            wrf(r + 4 * (uint32_t)k, ctr[k]);
        wrf(r + 12, rad);
        wr32(r + 16, v->bempty);
    }
    Q3RETURN(kQ3ViewStatusDone);
}

static void Q3View_StartPicking(ppc_cpu *c)
{
    q3view *v = view_arg(c);
    gaddr pick = Q3R(1);
    if (!v || !q3_pick_query(pick, &v->pq)) {
        Q3RETURN(kQ3Failure);
        return;
    }
    if (v->pq.kind != Q3PICK_RAY && (!v->dctx || !v->camera || !frame_setup(v) || !camera_setup(v))) {
        q3_error(!v->camera ? kQ3ErrorCameraNotSet : kQ3ErrorDrawContextNotSet);
        Q3RETURN(kQ3Failure);
        return;
    }
    if (v->pq.kind == Q3PICK_RAY && v->camera)
        camera_setup(v);
    start_loop(v, LOOP_PICK);
    v->pick = pick; /* a pick isn't shared: the program's own */
    Q3RETURN(kQ3Success);
}

static void Q3View_EndPicking(ppc_cpu *c)
{
    q3view *v = view_arg(c);
    if (!v || v->loop != LOOP_PICK) {
        Q3RETURN(kQ3ViewStatusError);
        return;
    }
    v->loop = LOOP_NONE;
    v->pick = 0;
    Q3RETURN(kQ3ViewStatusDone);
}

/* Writing: q3_file.c gets each object submitted at the top. */
bool q3_file_start_writing(gaddr file);
bool q3_file_end_writing(gaddr file);

static void Q3View_StartWriting(ppc_cpu *c)
{
    q3view *v = view_arg(c);
    if (!v || !q3_file_start_writing(Q3R(1))) {
        Q3RETURN(kQ3Failure);
        return;
    }
    start_loop(v, LOOP_WRITE);
    q3_assign(&v->file, Q3R(1));
    Q3RETURN(kQ3Success);
}

static void Q3View_EndWriting(ppc_cpu *c)
{
    q3view *v = view_arg(c);
    if (!v || v->loop != LOOP_WRITE) {
        Q3RETURN(kQ3ViewStatusError);
        return;
    }
    v->loop = LOOP_NONE;
    bool ok = q3_file_end_writing(v->file);
    q3_assign(&v->file, 0);
    Q3RETURN(ok ? kQ3ViewStatusDone : kQ3ViewStatusError);
}

void q3v_parts(q3view *v, gaddr parts[3]);
void q3v_parts(q3view *v, gaddr parts[3])
{
    parts[0] = v->renderer;
    parts[1] = v->camera;
    parts[2] = v->lights;
}

gaddr q3v_file(q3view *v);
gaddr q3v_file(q3view *v) { return v->file; }

static void Q3View_SetIdleMethod(ppc_cpu *c)
{
    q3view *v = view_arg(c);
    if (v) {
        v->idle = Q3R(1);
        v->idle_data = Q3R(2);
    }
    Q3RETURN(v ? kQ3Success : kQ3Failure);
}

static void Q3View_SetIdleProgressMethod(ppc_cpu *c)
{
    q3view *v = view_arg(c);
    if (v) {
        v->progress = Q3R(1);
        v->progress_data = Q3R(2);
    }
    Q3RETURN(v ? kQ3Success : kQ3Failure);
}

static void Q3View_SetEndFrameMethod(ppc_cpu *c)
{
    q3view *v = view_arg(c);
    if (v) {
        v->end_frame = Q3R(1);
        v->end_frame_data = Q3R(2);
    }
    Q3RETURN(v ? kQ3Success : kQ3Failure);
}

/* A push or pop in a writing loop is written as one. */
static bool write_operator(q3view *v, uint32_t type)
{
    if (v->loop != LOOP_WRITE)
        return false;
    gaddr g = q3_data_new(type, 0);
    if (g) {
        q3v_submit(v, g);
        q3_release(g);
    }
    return true;
}

static void Q3Push_Submit(ppc_cpu *c)
{
    q3view *v = view_arg(c);
    if (v && !write_operator(v, Q3T('p', 'u', 's', 'h')))
        q3v_push(v);
    Q3RETURN(v ? kQ3Success : kQ3Failure);
}

static void Q3Pop_Submit(ppc_cpu *c)
{
    q3view *v = view_arg(c);
    if (v && !write_operator(v, Q3T('p', 'o', 'p', ' ')))
        q3v_pop(v);
    Q3RETURN(v ? kQ3Success : kQ3Failure);
}

/* A box, in local coordinates, against the frustum. */
static void Q3View_IsBoundingBoxVisible(ppc_cpu *c)
{
    q3view *v = view_arg(c);
    gaddr b = Q3R(1);
    if (!v || !b || !v->camera) {
        Q3RETURN(kQ3True);
        return;
    }
    if (rd32(b + 24)) { /* empty */
        Q3RETURN(kQ3False);
        return;
    }
    int answer = v->xr ? q3xr_box_visible(v, b) : -1;
    if (answer >= 0) {
        Q3RETURN(answer);
        return;
    }
    float w2v[16], v2f[16], w2f[16], m[16];
    if (!q3_camera_matrices(v->camera, w2v, v2f)) {
        Q3RETURN(kQ3True);
        return;
    }
    q3m_mul(w2v, v2f, w2f);
    q3m_mul(v->st.m, w2f, m);
    int out[6] = { 0 };
    for (int i = 0; i < 8; i++) {
        float p[3] = { rdf(b + ((i & 1) ? 12u : 0u)), rdf(b + 4 + ((i & 2) ? 12u : 0u)), rdf(b + 8 + ((i & 4) ? 12u : 0u)) };
        float h[4];
        point4(m, p, h);
        out[0] += h[0] < -h[3];
        out[1] += h[0] > h[3];
        out[2] += h[1] < -h[3];
        out[3] += h[1] > h[3];
        out[4] += h[2] > 0;
        out[5] += h[2] < -h[3];
    }
    bool visible = true;
    for (int k = 0; k < 6; k++)
        if (out[k] == 8)
            visible = false;
    Q3RETURN(visible);
}

static void Q3View_AllowAllGroupCulling(ppc_cpu *c) { Q3RETURN(view_arg(c) ? kQ3Success : kQ3Failure); }

static void Q3View_GetLocalToWorldMatrixState(ppc_cpu *c)
{
    q3view *v = view_arg(c);
    if (v && Q3R(1))
        q3m_put(Q3R(1), v->st.m);
    Q3RETURN(v && Q3R(1) ? kQ3Success : kQ3Failure);
}

static void Q3View_GetWorldToFrustumMatrixState(ppc_cpu *c)
{
    q3view *v = view_arg(c);
    if (v && Q3R(1)) {
        float w2v[16], v2f[16], m[16];
        if (q3_camera_matrices(v->camera, w2v, v2f))
            q3m_mul(w2v, v2f, m);
        else
            q3m_identity(m);
        q3m_put(Q3R(1), m);
    }
    Q3RETURN(v && Q3R(1) ? kQ3Success : kQ3Failure);
}

static void Q3View_GetFrustumToWindowMatrixState(ppc_cpu *c)
{
    q3view *v = view_arg(c);
    if (v && Q3R(1)) {
        gaddr port;
        int x, y, w, h;
        bool pixmap;
        float m[16];
        q3m_identity(m);
        if (dctx_area(v->dctx, &port, &x, &y, &w, &h, &pixmap)) {
            m[0] = (float)w / 2;
            m[5] = -(float)h / 2;
            m[12] = (float)w / 2 + (float)x;
            m[13] = (float)h / 2 + (float)y;
        }
        q3m_put(Q3R(1), m);
    }
    Q3RETURN(v && Q3R(1) ? kQ3Success : kQ3Failure);
}

#define STYLE_STATE(fn, field)                                                                                       \
    static void fn(ppc_cpu *c)                                                                                       \
    {                                                                                                                \
        q3view *v = view_arg(c);                                                                                     \
        if (v && Q3R(1))                                                                                             \
            wr32(Q3R(1), v->st.field);                                                                               \
        Q3RETURN(v && Q3R(1) ? kQ3Success : kQ3Failure);                                                             \
    }
STYLE_STATE(Q3View_GetBackfacingStyleState, backfacing)
STYLE_STATE(Q3View_GetInterpolationStyleState, interpolation)
STYLE_STATE(Q3View_GetFillStyleState, fill)
STYLE_STATE(Q3View_GetOrientationStyleState, orientation)
STYLE_STATE(Q3View_GetReceiveShadowsStyleState, receive)
STYLE_STATE(Q3View_GetPickIDStyleState, pickid)
STYLE_STATE(Q3View_GetPickPartsStyleState, pickparts)

static void Q3View_GetHighlightStyleState(ppc_cpu *c)
{
    q3view *v = view_arg(c);
    if (v && Q3R(1))
        wr32(Q3R(1), v->st.highlight ? q3_ref(v->st.highlight) : 0);
    Q3RETURN(v && Q3R(1) ? kQ3Success : kQ3Failure);
}

static void Q3View_GetSubdivisionStyleState(ppc_cpu *c)
{
    q3view *v = view_arg(c);
    if (v && Q3R(1)) {
        wr32(Q3R(1), v->st.subdiv_method);
        wrf(Q3R(1) + 4, v->st.subdiv_c1);
        wrf(Q3R(1) + 8, v->st.subdiv_c2);
    }
    Q3RETURN(v && Q3R(1) ? kQ3Success : kQ3Failure);
}

static void Q3View_GetAntiAliasStyleState(ppc_cpu *c)
{
    q3view *v = view_arg(c);
    if (v && Q3R(1))
        for (int k = 0; k < 3; k++)
            wr32(Q3R(1) + 4 * (uint32_t)k, v->st.aa[k]);
    Q3RETURN(v && Q3R(1) ? kQ3Success : kQ3Failure);
}

/* The attributes in force as a new attribute set. */
static void put_attr(q3set *s, int32_t type, const float *f, int n)
{
    uint8_t b[24];
    for (int i = 0; i < n; i++) {
        uint32_t u;
        memcpy(&u, &f[i], 4);
        b[4 * i] = (uint8_t)(u >> 24), b[4 * i + 1] = (uint8_t)(u >> 16), b[4 * i + 2] = (uint8_t)(u >> 8),
        b[4 * i + 3] = (uint8_t)u;
    }
    q3set_add_bytes(s, type, b, (uint32_t)n * 4);
}

static gaddr state_set(const q3view *v)
{
    q3set *s = calloc(1, sizeof *s);
    q3obj *o = s ? q3_new(Q3_ATTRIBUTE_SET, s) : NULL;
    if (!o) {
        free(s);
        return 0;
    }
    const q3attrs *a = &v->st.a;
    put_attr(s, kQ3AttributeTypeAmbientCoefficient, &a->amb, 1);
    put_attr(s, kQ3AttributeTypeDiffuseColor, a->kd, 3);
    put_attr(s, kQ3AttributeTypeSpecularColor, a->ks, 3);
    put_attr(s, kQ3AttributeTypeSpecularControl, &a->spc, 1);
    put_attr(s, kQ3AttributeTypeTransparencyColor, a->kt, 3);
    uint8_t hl[4] = { 0, 0, 0, a->hilite };
    q3set_add_bytes(s, kQ3AttributeTypeHighlightState, hl, 4);
    if (a->has & QA_N)
        put_attr(s, kQ3AttributeTypeNormal, a->n, 3);
    if (a->has & QA_UV)
        put_attr(s, kQ3AttributeTypeSurfaceUV, a->uv, 2);
    if (a->shader) {
        uint8_t sh[4] = { (uint8_t)(a->shader >> 24), (uint8_t)(a->shader >> 16), (uint8_t)(a->shader >> 8),
                          (uint8_t)a->shader };
        q3set_add_bytes(s, kQ3AttributeTypeSurfaceShader, sh, 4);
    }
    return o->g;
}

static void Q3View_GetAttributeSetState(ppc_cpu *c)
{
    q3view *v = view_arg(c);
    if (v && Q3R(1))
        wr32(Q3R(1), state_set(v));
    Q3RETURN(v && Q3R(1) ? kQ3Success : kQ3Failure);
}

static void Q3View_GetAttributeState(ppc_cpu *c)
{
    q3view *v = view_arg(c);
    gaddr s = v ? state_set(v) : 0;
    const q3elem *e = q3set_find(q3_set_of(s), (int32_t)Q3R(1));
    if (e && Q3R(2))
        gput(Q3R(2), e->bytes, e->size);
    if (e && e->type == kQ3AttributeTypeSurfaceShader && e->size >= 4)
        q3_ref(rd32(Q3R(2)));
    q3_release(s);
    Q3RETURN(e ? kQ3Success : kQ3Failure);
}

/* The view's state, for the style, shader and transform objects
 * submitted (q3_shapes.c). */
void q3v_set_style(q3view *v, uint32_t type, const uint32_t *w);
void q3v_set_style(q3view *v, uint32_t type, const uint32_t *w)
{
    vstate *s = &v->st;
    switch (type) {
    case Q3T('b', 'c', 'k', 'f'): s->backfacing = w[0]; break;
    case Q3T('i', 'n', 't', 'p'): s->interpolation = w[0]; break;
    case Q3T('f', 'i', 's', 't'): s->fill = w[0]; break;
    case Q3T('p', 'k', 'i', 'd'): s->pickid = w[0]; break;
    case Q3T('r', 'c', 's', 'h'): s->receive = w[0]; break;
    case Q3T('h', 'i', 'g', 'h'): s->highlight = w[0]; break;
    case Q3T('o', 'f', 'd', 'r'): s->orientation = w[0]; break;
    case Q3T('p', 'k', 'p', 't'): s->pickparts = w[0]; break;
    case Q3T('s', 'b', 'd', 'v'):
        s->subdiv_method = w[0];
        s->subdiv_c1 = q3d_f(w, 1);
        s->subdiv_c2 = q3d_f(w, 2);
        break;
    case Q3T('a', 'n', 't', 'i'): memcpy(s->aa, w, sizeof s->aa); break;
    case Q3T('f', 'o', 'g', 'g'): memcpy(s->fog, w, sizeof s->fog); break;
    default: break;
    }
}

void q3v_set_shader(q3view *v, q3obj *shader);
void q3v_set_shader(q3view *v, q3obj *shader)
{
    if (q3_type_is(shader->type, Q3_ILLUMINATION)) {
        v->st.illumination = shader->type;
        v->st.illum = shader->g;
    }
    else if (q3_type_is(shader->type, Q3_SURFACE_SHADER))
        v->st.texture = shader->g;
}

static void view_classes(void) { q3_class_ops(Q3_VIEW, view_dispose, NULL, NULL, false); }

#define E(n) { #n, n }
const ppc_export q3_view_exports[] = {
    E(Q3View_New), E(Q3View_Cancel), E(Q3View_SetRendererByType), E(Q3View_SetRenderer), E(Q3View_GetRenderer),
    E(Q3View_StartRendering), E(Q3View_EndRendering), E(Q3View_Flush), E(Q3View_Sync),
    E(Q3View_StartBoundingBox), E(Q3View_EndBoundingBox), E(Q3View_StartBoundingSphere),
    E(Q3View_EndBoundingSphere), E(Q3View_StartPicking), E(Q3View_EndPicking), E(Q3View_GetCamera),
    E(Q3View_SetCamera), E(Q3View_SetLightGroup), E(Q3View_GetLightGroup), E(Q3View_SetIdleMethod),
    E(Q3View_SetIdleProgressMethod), E(Q3View_SetEndFrameMethod), E(Q3Push_Submit), E(Q3Pop_Submit),
    E(Q3View_IsBoundingBoxVisible), E(Q3View_AllowAllGroupCulling), E(Q3View_SetDrawContext),
    E(Q3View_GetDrawContext), E(Q3View_GetLocalToWorldMatrixState), E(Q3View_GetWorldToFrustumMatrixState),
    E(Q3View_GetFrustumToWindowMatrixState), E(Q3View_GetBackfacingStyleState),
    E(Q3View_GetInterpolationStyleState), E(Q3View_GetFillStyleState), E(Q3View_GetHighlightStyleState),
    E(Q3View_GetSubdivisionStyleState), E(Q3View_GetOrientationStyleState), E(Q3View_GetReceiveShadowsStyleState),
    E(Q3View_GetPickIDStyleState), E(Q3View_GetPickPartsStyleState), E(Q3View_GetAntiAliasStyleState),
    E(Q3View_GetDefaultAttributeSet), E(Q3View_SetDefaultAttributeSet), E(Q3View_GetAttributeSetState),
    E(Q3View_GetAttributeState), E(Q3View_StartWriting), E(Q3View_EndWriting),
};
const unsigned q3_view_export_count = sizeof q3_view_exports / sizeof q3_view_exports[0];

void q3_shapes_classes(void);

void q3_view_classes(void)
{
    view_classes();
    q3_shapes_classes();
    q3_pick_classes();
}

void q3_view_reset(void)
{
    memset(views, 0, sizeof views);
    for (int i = 0; i < ntexs; i++)
        r3d_texture_free(texs[i].id);
    ntexs = 0;
}
