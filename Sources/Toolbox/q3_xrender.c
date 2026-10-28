/*
 * QuickDraw 3D's renderer plug-ins (QD3DRenderer.h's extension part): a
 * renderer class a program or an extension registered under
 * kQ3SharedTypeRenderer draws a view's frames itself.  The view calls its
 * methods as QuickDraw 3D did: StartFrame, updates for the default state,
 * StartPass, then for each geometry the state that changed (matrices,
 * attributes, styles, shaders) and the geometry, whole if the renderer
 * takes its type and otherwise broken into the triangles, lines, points
 * and markers every renderer must take; EndPass at Q3View_EndRendering
 * (again from StartPass if it asks for another pass), EndFrame at
 * Q3View_Sync for an asynchronous one, Push and Pop if it keeps its own
 * state stack.
 *
 * A renderer finds where to draw through its draw context's draw region:
 * either the destination as it is (Q3XDrawRegion_Start, with its device,
 * port and clip) or an image buffer of 32-bit pixels the size of the pane
 * (Q3XDrawRegion_StartAccessToImageBuffer), which goes into the draw
 * context, through its clip, when the pass ends.
 */
#include <math.h>
#include <stdlib.h>

#include "q3_viewpriv.h"
#include "tb_internal.h"

#define T4(s) Q3T((s)[0], (s)[1], (s)[2], (s)[3])

/* Renderer method types (QD3DRenderer.h). */
#define M_START_FRAME T4("rdcl")
#define M_START_PASS T4("rdst")
#define M_FLUSH_FRAME T4("rdfl")
#define M_END_PASS T4("rded")
#define M_END_FRAME T4("rdsy")
#define M_CANCEL T4("rdab")
#define M_PUSH T4("rdps")
#define M_POP T4("rdpo")
#define M_BOX_VISIBLE T4("rdbx")
#define M_GEOMETRY T4("rdgm")
#define M_CAMERA T4("rdcm")
#define M_LIGHT T4("rdlg")
#define M_STYLE T4("rdyu")
#define M_ATTRIBUTE T4("rdau")
#define M_SHADER T4("rdsu")
#define M_MATRIX T4("rdxu")
#define M_INTERACTIVE T4("isin")
#define M_CONFIGURE T4("rdmc")
#define M_NICKNAME T4("rdns")
#define M_GET_CONFIG T4("rdgp")
#define M_SET_CONFIG T4("rdsp")

enum { MX_L2W, MX_L2W_INV, MX_L2W_INVT, MX_L2C, MX_L2F, MX_W2F, NMATRIX };
static const uint32_t matrix_types[NMATRIX] = { T4("ulwx"), T4("ulwi"), T4("ulwt"), T4("ulcx"), T4("ulfx"), T4("uwfx") };

/* The styles a renderer is told about, in the order their words are
 * compared. */
enum { ST_BACKFACING, ST_INTERP, ST_FILL, ST_PICKID, ST_RECEIVE, ST_HIGHLIGHT, ST_ORIENT, ST_PICKPARTS, ST_SUBDIV,
       ST_AA, ST_FOG, NSTYLE };
static const uint32_t style_types[NSTYLE] = { T4("bckf"), T4("intp"), T4("fist"), T4("pkid"), T4("rcsh"), T4("high"),
                                              T4("ofdr"), T4("pkpt"), T4("sbdv"), T4("anti"), T4("fogg") };

#define GEOM_TRIANGLE T4("trng")
#define GEOM_LINE T4("line")
#define GEOM_POINT T4("pnt ")
#define GEOM_PMARKER T4("mrkp")
#define SHADER_ILLUMINATION T4("ilsh")
#define SHADER_SURFACE T4("sush")

enum { NATTR = 12 }; /* attribute types 1 to 11 */

/* What the renderer has been told: the attributes as their bytes, the
 * styles as their words, the shaders and the local-to-world matrix. */
typedef struct {
    bool valid;
    float m[16];
    uint8_t attr[NATTR][24];
    uint8_t attr_size[NATTR]; /* 0: not in force */
    uint32_t style[NSTYLE][9];
    gaddr illum, surface;
} told;

typedef struct {
    uint32_t type;
    gaddr fn;
} cached;

struct q3xr {
    gaddr renderer, priv;
    gaddr start_frame, start_pass, flush_frame, end_pass, end_frame, cancel, push, pop, box_visible;
    gaddr geom_meta, camera_meta, light_meta;
    gaddr matrix[NMATRIX], attr[NATTR], style[NSTYLE], illum, surface;
    cached geoms[48];
    int ngeoms;
    told now, *stack;
    int depth, cap;
    gaddr scratch; /* guest room for what the updates point at */
};

static gaddr meta_call(gaddr meta, uint32_t type)
{
    if (!meta)
        return 0;
    uint32_t arg = type;
    return ppc_call(meta, &arg, 1);
}

static q3class *plugin_class(gaddr renderer)
{
    q3obj *o = q3_get(renderer);
    return o && o->cls && o->cls->registered && q3_type_is(o->type, Q3_RENDERER) ? o->cls : NULL;
}

bool q3xr_is_plugin(gaddr renderer) { return plugin_class(renderer) != NULL; }

/* The renderer's method for a geometry type, kept once asked. */
static gaddr geom_method(struct q3xr *x, uint32_t type)
{
    for (int i = 0; i < x->ngeoms; i++)
        if (x->geoms[i].type == type)
            return x->geoms[i].fn;
    gaddr fn = meta_call(x->geom_meta, type);
    if (x->ngeoms < (int)(sizeof x->geoms / sizeof x->geoms[0]))
        x->geoms[x->ngeoms++] = (cached){ type, fn };
    return fn;
}

static struct q3xr *xr_new(q3view *v)
{
    q3class *k = plugin_class(v->renderer);
    struct q3xr *x = calloc(1, sizeof *x);
    if (!k || !x) {
        free(x);
        return NULL;
    }
    x->renderer = v->renderer;
    x->priv = q3_get(v->renderer)->instance;
    x->start_frame = q3_class_method(k, M_START_FRAME);
    x->start_pass = q3_class_method(k, M_START_PASS);
    x->flush_frame = q3_class_method(k, M_FLUSH_FRAME);
    x->end_pass = q3_class_method(k, M_END_PASS);
    x->end_frame = q3_class_method(k, M_END_FRAME);
    x->cancel = q3_class_method(k, M_CANCEL);
    x->push = q3_class_method(k, M_PUSH);
    x->pop = q3_class_method(k, M_POP);
    x->box_visible = q3_class_method(k, M_BOX_VISIBLE);
    x->geom_meta = q3_class_method(k, M_GEOMETRY);
    x->camera_meta = q3_class_method(k, M_CAMERA);
    x->light_meta = q3_class_method(k, M_LIGHT);
    gaddr matrix_meta = q3_class_method(k, M_MATRIX);
    for (int i = 0; i < NMATRIX; i++)
        x->matrix[i] = meta_call(matrix_meta, matrix_types[i]);
    gaddr attr_meta = q3_class_method(k, M_ATTRIBUTE);
    for (int i = 1; i < NATTR; i++)
        x->attr[i] = meta_call(attr_meta, (uint32_t)i);
    gaddr style_meta = q3_class_method(k, M_STYLE);
    for (int i = 0; i < NSTYLE; i++)
        x->style[i] = meta_call(style_meta, style_types[i]);
    gaddr shader_meta = q3_class_method(k, M_SHADER);
    x->illum = meta_call(shader_meta, SHADER_ILLUMINATION);
    x->surface = meta_call(shader_meta, SHADER_SURFACE);
    x->scratch = tb_NewPtr(256, true, true);
    return x;
}

void q3xr_free(q3view *v)
{
    struct q3xr *x = v->xr;
    if (!x)
        return;
    if (x->scratch)
        tb_DisposePtr(x->scratch);
    free(x->stack);
    free(x);
    v->xr = NULL;
}

/* ---------------------------------------------------------------------- */
/* Attributes as QuickDraw 3D keeps them: big-endian bytes */

static void put_be(uint8_t *b, uint32_t u)
{
    b[0] = (uint8_t)(u >> 24);
    b[1] = (uint8_t)(u >> 16);
    b[2] = (uint8_t)(u >> 8);
    b[3] = (uint8_t)u;
}

static void put_floats(uint8_t *b, const float *f, int n)
{
    for (int i = 0; i < n; i++) {
        uint32_t u;
        memcpy(&u, &f[i], 4);
        put_be(b + 4 * i, u);
    }
}

/* Attribute `type` of a as its bytes; its size, 0 if a hasn't it. */
static int attr_bytes(const q3attrs *a, int type, uint8_t out[24])
{
    switch (type) {
    case kQ3AttributeTypeSurfaceUV:
        if (!(a->has & QA_UV))
            return 0;
        put_floats(out, a->uv, 2);
        return 8;
    case kQ3AttributeTypeNormal:
        if (!(a->has & QA_N))
            return 0;
        put_floats(out, a->n, 3);
        return 12;
    case kQ3AttributeTypeAmbientCoefficient:
        if (!(a->has & QA_AMB))
            return 0;
        put_floats(out, &a->amb, 1);
        return 4;
    case kQ3AttributeTypeDiffuseColor:
        if (!(a->has & QA_KD))
            return 0;
        put_floats(out, a->kd, 3);
        return 12;
    case kQ3AttributeTypeSpecularColor:
        if (!(a->has & QA_KS))
            return 0;
        put_floats(out, a->ks, 3);
        return 12;
    case kQ3AttributeTypeSpecularControl:
        if (!(a->has & QA_SPC))
            return 0;
        put_floats(out, &a->spc, 1);
        return 4;
    case kQ3AttributeTypeTransparencyColor:
        if (!(a->has & QA_KT))
            return 0;
        put_floats(out, a->kt, 3);
        return 12;
    case kQ3AttributeTypeHighlightState:
        if (!(a->has & QA_HILITE))
            return 0;
        put_be(out, a->hilite ? 1 : 0);
        return 4;
    case kQ3AttributeTypeSurfaceShader:
        if (!(a->has & QA_SHADER) || !a->shader)
            return 0;
        put_be(out, a->shader);
        return 4;
    default: return 0;
    }
}

/* An attribute set holding what a has (0 if nothing); the caller
 * releases it. */
static gaddr attrs_set(const q3attrs *a)
{
    if (!a->has)
        return 0;
    q3set *s = calloc(1, sizeof *s);
    q3obj *o = s ? q3_new(Q3_ATTRIBUTE_SET, s) : NULL;
    if (!o) {
        free(s);
        return 0;
    }
    uint8_t b[24];
    for (int t = 1; t < NATTR; t++) {
        int n = attr_bytes(a, t, b);
        if (n)
            q3set_add_bytes(s, t, b, (uint32_t)n);
    }
    if (!s->n) {
        q3_release(o->g);
        return 0;
    }
    return o->g;
}

/* ---------------------------------------------------------------------- */
/* Telling the renderer what changed */

static void style_words(const vstate *s, int i, uint32_t w[9])
{
    memset(w, 0, 9 * sizeof *w);
    switch (i) {
    case ST_BACKFACING: w[0] = s->backfacing; break;
    case ST_INTERP: w[0] = s->interpolation; break;
    case ST_FILL: w[0] = s->fill; break;
    case ST_PICKID: w[0] = s->pickid; break;
    case ST_RECEIVE: w[0] = s->receive; break;
    case ST_HIGHLIGHT: w[0] = s->highlight; break;
    case ST_ORIENT: w[0] = s->orientation; break;
    case ST_PICKPARTS: w[0] = s->pickparts; break;
    case ST_SUBDIV:
        w[0] = s->subdiv_method;
        memcpy(&w[1], &s->subdiv_c1, 4);
        memcpy(&w[2], &s->subdiv_c2, 4);
        break;
    case ST_AA: memcpy(w, s->aa, sizeof s->aa); break;
    case ST_FOG: memcpy(w, s->fog, sizeof s->fog); break;
    default: break;
    }
}

static int style_nwords(int i) { return i == ST_SUBDIV || i == ST_AA ? 3 : i == ST_FOG ? 9 : 1; }

static void call_update(q3view *v, gaddr fn, gaddr data)
{
    uint32_t args[3] = { v->g, v->xr->priv, data };
    ppc_call(fn, args, 3);
}

static void send_matrices(q3view *v)
{
    struct q3xr *x = v->xr;
    float m[16], out[16];
    memcpy(m, v->st.m, sizeof m);
    for (int i = 0; i < NMATRIX; i++) {
        if (!x->matrix[i] || i == MX_W2F)
            continue;
        switch (i) {
        case MX_L2W: memcpy(out, m, sizeof out); break;
        case MX_L2W_INV:
            if (!q3m_invert(m, out))
                q3m_identity(out);
            break;
        case MX_L2W_INVT: {
            float inv[16];
            if (!q3m_invert(m, inv))
                q3m_identity(inv);
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++)
                    out[r * 4 + c] = inv[c * 4 + r];
            break;
        }
        case MX_L2C: q3m_mul(m, v->w2v, out); break;
        case MX_L2F: q3m_mul(m, v->w2f, out); break;
        default: break;
        }
        q3m_put(x->scratch, out);
        call_update(v, x->matrix[i], x->scratch);
    }
}

/* Everything in the view's state the renderer hasn't been told, told. */
static void update(q3view *v)
{
    struct q3xr *x = v->xr;
    told *t = &x->now;
    const vstate *s = &v->st;
    if (!t->valid || memcmp(t->m, s->m, sizeof t->m)) {
        send_matrices(v);
        memcpy(t->m, s->m, sizeof t->m);
    }
    uint8_t b[24];
    for (int a = 1; a < NATTR; a++) {
        int n = attr_bytes(&s->a, a, b);
        if (t->valid && n == t->attr_size[a] && !memcmp(b, t->attr[a], (size_t)n))
            continue;
        if (x->attr[a] && (n || t->attr_size[a] || !t->valid)) {
            if (n)
                gput(x->scratch, b, (uint32_t)n);
            call_update(v, x->attr[a], n ? x->scratch : 0);
        }
        memcpy(t->attr[a], b, (size_t)n);
        t->attr_size[a] = (uint8_t)n;
    }
    for (int i = 0; i < NSTYLE; i++) {
        uint32_t w[9];
        style_words(s, i, w);
        if (t->valid && !memcmp(w, t->style[i], sizeof w))
            continue;
        if (x->style[i]) {
            for (int k = 0; k < style_nwords(i); k++)
                wr32(x->scratch + 4 * (uint32_t)k, w[k]);
            call_update(v, x->style[i], x->scratch);
        }
        memcpy(t->style[i], w, sizeof w);
    }
    if (!t->valid || t->illum != s->illum) {
        if (x->illum) {
            wr32(x->scratch, s->illum);
            call_update(v, x->illum, x->scratch);
        }
        t->illum = s->illum;
    }
    if (!t->valid || t->surface != s->texture) {
        if (x->surface) {
            wr32(x->scratch, s->texture);
            call_update(v, x->surface, x->scratch);
        }
        t->surface = s->texture;
    }
    t->valid = true;
}

void q3xr_push(q3view *v)
{
    struct q3xr *x = v->xr;
    if (!x || !x->push || v->loop != LOOP_RENDER)
        return;
    update(v);
    if (x->depth == x->cap) {
        int cap = x->cap ? x->cap * 2 : 16;
        told *n = realloc(x->stack, (size_t)cap * sizeof *n);
        if (!n)
            return;
        x->stack = n;
        x->cap = cap;
    }
    x->stack[x->depth++] = x->now;
    uint32_t args[2] = { v->g, x->priv };
    ppc_call(x->push, args, 2);
}

/* A renderer that keeps a stack pops its own state; one that doesn't is
 * told what changed back before it draws again. */
void q3xr_pop(q3view *v)
{
    struct q3xr *x = v->xr;
    if (!x || !x->pop || v->loop != LOOP_RENDER || !x->depth)
        return;
    x->now = x->stack[--x->depth];
    uint32_t args[2] = { v->g, x->priv };
    ppc_call(x->pop, args, 2);
}

/* ---------------------------------------------------------------------- */
/* The frame */

typedef struct region region;
static region *region_of(gaddr dctx, bool make);
static void region_frame_start(region *r);
static void region_present(region *r, q3view *v);

/* Lights and the camera to a renderer that transforms them itself. */
static void submit_with_data(q3view *v, gaddr fn, gaddr obj)
{
    q3obj *o = q3_get(obj);
    const q3schema *s = o ? q3_schema_of(o->type) : NULL;
    if (!fn || !o || !s || !o->data)
        return;
    gaddr data = q3_galloc(s->words * 4);
    if (!data)
        return;
    q3d_to_guest(data, o->data, s, 0);
    uint32_t args[4] = { v->g, v->xr->priv, obj, data };
    ppc_call(fn, args, 4);
    q3d_empty_guest(data, s, 1, NULL);
    q3_gfree(data);
}

static bool start_pass(q3view *v)
{
    struct q3xr *x = v->xr;
    x->now.valid = false;
    x->depth = 0;
    update(v);
    if (x->start_pass) {
        uint32_t args[4] = { v->g, x->priv, v->camera, x->light_meta ? 0 : v->lights };
        if (ppc_call(x->start_pass, args, 4) != kQ3Success)
            return false;
    }
    q3obj *cam = q3_get(v->camera);
    if (x->camera_meta && cam)
        submit_with_data(v, meta_call(x->camera_meta, cam->type), v->camera);
    if (x->light_meta && v->lights) {
        q3group *g = q3_group_of(v->lights);
        for (q3pos *p = g ? g->first : NULL; p; p = p->next) {
            q3obj *l = q3_get(p->obj);
            if (l)
                submit_with_data(v, meta_call(x->light_meta, l->type), p->obj);
        }
    }
    return true;
}

bool q3xr_start(q3view *v)
{
    q3xr_free(v);
    v->xr = xr_new(v);
    struct q3xr *x = v->xr;
    if (!x)
        return false;
    region *r = region_of(v->dctx, true);
    if (r)
        region_frame_start(r);
    if (x->start_frame) {
        uint32_t args[3] = { v->g, x->priv, v->dctx };
        if (ppc_call(x->start_frame, args, 3) != kQ3Success) {
            q3xr_free(v);
            v->loop = LOOP_NONE;
            return false;
        }
    }
    if (!start_pass(v)) {
        q3xr_free(v);
        v->loop = LOOP_NONE;
        return false;
    }
    return true;
}

int q3xr_end_pass(q3view *v)
{
    struct q3xr *x = v->xr;
    int status = kQ3ViewStatusDone;
    if (x->end_pass) {
        uint32_t args[2] = { v->g, x->priv };
        status = (int)ppc_call(x->end_pass, args, 2);
    }
    if (status == kQ3ViewStatusRetraverse) {
        q3v_default_state(v);
        v->pathlen = 0;
        v->object = 0;
        if (start_pass(v))
            return status;
        status = kQ3ViewStatusError;
    }
    region *r = region_of(v->dctx, false);
    if (r)
        region_present(r, v);
    /* An asynchronous renderer says when its frame is done
     * (Q3XView_EndFrame). */
    if (status == kQ3ViewStatusDone && !x->end_frame && v->end_frame) {
        uint32_t args[2] = { v->g, v->end_frame_data };
        ppc_call(v->end_frame, args, 2);
    }
    return status;
}

void q3xr_cancel(q3view *v)
{
    struct q3xr *x = v->xr;
    if (x && x->cancel) {
        uint32_t args[2] = { v->g, x->priv };
        ppc_call(x->cancel, args, 2);
    }
}

void q3xr_sync(q3view *v)
{
    struct q3xr *x = v->xr;
    if (x && x->end_frame) {
        uint32_t args[3] = { v->g, x->priv, v->dctx };
        ppc_call(x->end_frame, args, 3);
        region *r = region_of(v->dctx, false);
        if (r)
            region_present(r, v);
    }
}

void q3xr_flush(q3view *v)
{
    struct q3xr *x = v->xr;
    if (x && x->flush_frame && v->loop == LOOP_RENDER) {
        uint32_t args[3] = { v->g, x->priv, v->dctx };
        ppc_call(x->flush_frame, args, 3);
    }
}

int q3xr_box_visible(q3view *v, gaddr box)
{
    struct q3xr *x = v->xr;
    if (!x || !x->box_visible || v->loop != LOOP_RENDER)
        return -1;
    update(v);
    uint32_t args[3] = { v->g, x->priv, box };
    return ppc_call(x->box_visible, args, 3) ? 1 : 0;
}

/* ---------------------------------------------------------------------- */
/* Geometry */

static void call_geometry(q3view *v, gaddr fn, gaddr obj, gaddr data)
{
    uint32_t args[4] = { v->g, v->xr->priv, obj, data };
    ppc_call(fn, args, 4);
}

bool q3xr_geometry(q3view *v, uint32_t type, gaddr obj, gaddr data)
{
    if (!v || !v->xr || v->loop != LOOP_RENDER || v->cancelled)
        return false;
    gaddr fn = geom_method(v->xr, type);
    if (!fn)
        return false;
    update(v);
    if (data) {
        call_geometry(v, fn, obj, data);
        return true;
    }
    q3obj *o = q3_get(obj);
    const q3schema *s = o ? q3_schema_of(o->type) : NULL;
    if (!s || !o->data)
        return false;
    gaddr g = q3_galloc(s->words * 4);
    if (!g)
        return true;
    q3d_to_guest(g, o->data, s, 0);
    call_geometry(v, fn, obj, g);
    q3d_empty_guest(g, s, 1, NULL);
    q3_gfree(g);
    return true;
}

/* A vertex (TQ3Vertex3D) at p: its point and attribute set. */
gaddr q3_memory_storage_new(const uint8_t *b, uint32_t n);

static void put_vertex(gaddr p, const q3pv *pv, gaddr set)
{
    for (int k = 0; k < 3; k++)
        wrf(p + 4 * (uint32_t)k, pv->p[k]);
    wr32(p + 12, set);
}

/* The geometry's own attributes under a part's. */
static gaddr part_set(const q3attrs *part, const q3attrs *geom)
{
    q3attrs a = *part;
    q3attrs_merge(&a, geom);
    return attrs_set(&a);
}

void q3xr_triangles(q3view *v, const q3pv *pv, uint32_t npv, const q3ptri *t, uint32_t nt, gaddr geom_set)
{
    gaddr fn = geom_method(v->xr, GEOM_TRIANGLE);
    if (!fn || v->cancelled)
        return;
    update(v);
    q3attrs ga;
    memset(&ga, 0, sizeof ga);
    q3attrs_fill(&ga, geom_set);
    gaddr *sets = calloc(npv, sizeof *sets);
    gaddr data = q3_galloc(52); /* TQ3TriangleData */
    if (!sets || !data) {
        free(sets);
        q3_gfree(data);
        return;
    }
    for (uint32_t i = 0; i < npv; i++)
        sets[i] = attrs_set(&pv[i].a);
    gaddr whole = attrs_set(&ga);
    for (uint32_t i = 0; i < nt && !v->cancelled; i++) {
        if (t[i].v[0] >= npv || t[i].v[1] >= npv || t[i].v[2] >= npv)
            continue;
        for (int k = 0; k < 3; k++)
            put_vertex(data + 16 * (uint32_t)k, &pv[t[i].v[k]], sets[t[i].v[k]]);
        gaddr tset = t[i].a.has ? part_set(&t[i].a, &ga) : whole;
        wr32(data + 48, tset);
        call_geometry(v, fn, 0, data);
        if (tset != whole)
            q3_release(tset);
    }
    q3_release(whole);
    for (uint32_t i = 0; i < npv; i++)
        q3_release(sets[i]);
    free(sets);
    q3_gfree(data);
}

void q3xr_lines(q3view *v, const q3pv *pv, uint32_t nseg, gaddr geom_set)
{
    gaddr fn = geom_method(v->xr, GEOM_LINE);
    if (!fn || v->cancelled)
        return;
    update(v);
    q3attrs ga;
    memset(&ga, 0, sizeof ga);
    q3attrs_fill(&ga, geom_set);
    gaddr whole = attrs_set(&ga), data = q3_galloc(36); /* TQ3LineData */
    for (uint32_t i = 0; data && i < nseg && !v->cancelled; i++) {
        gaddr s0 = attrs_set(&pv[2 * i].a), s1 = attrs_set(&pv[2 * i + 1].a);
        put_vertex(data, &pv[2 * i], s0);
        put_vertex(data + 16, &pv[2 * i + 1], s1);
        wr32(data + 32, whole);
        call_geometry(v, fn, 0, data);
        q3_release(s0);
        q3_release(s1);
    }
    q3_release(whole);
    q3_gfree(data);
}

void q3xr_points(q3view *v, const q3pv *pv, uint32_t n, gaddr geom_set)
{
    gaddr fn = geom_method(v->xr, GEOM_POINT);
    if (!fn || v->cancelled)
        return;
    update(v);
    q3attrs ga;
    memset(&ga, 0, sizeof ga);
    q3attrs_fill(&ga, geom_set);
    gaddr data = q3_galloc(16); /* TQ3PointData */
    for (uint32_t i = 0; data && i < n && !v->cancelled; i++) {
        gaddr s = part_set(&pv[i].a, &ga);
        for (int k = 0; k < 3; k++)
            wrf(data + 4 * (uint32_t)k, pv[i].p[k]);
        wr32(data + 12, s);
        call_geometry(v, fn, 0, data);
        q3_release(s);
    }
    q3_gfree(data);
}

/* A marker or text, already an image: a pixmap marker of 32-bit pixels. */
void q3xr_image(q3view *v, const float p[3], int dx, int dy, int w, int h, const uint32_t *argb, gaddr set)
{
    gaddr fn = geom_method(v->xr, GEOM_PMARKER);
    if (!fn || v->cancelled || w <= 0 || h <= 0)
        return;
    update(v);
    uint8_t *px = malloc((size_t)w * (size_t)h * 4);
    if (!px)
        return;
    for (int i = 0; i < w * h; i++)
        put_be(px + 4 * i, argb[i]);
    gaddr storage = q3_memory_storage_new(px, (uint32_t)(w * h * 4));
    free(px);
    gaddr data = q3_galloc(56); /* TQ3PixmapMarkerData */
    if (storage && data) {
        for (int k = 0; k < 3; k++)
            wrf(data + 4 * (uint32_t)k, p[k]);
        wr32(data + 12, (uint32_t)dx);
        wr32(data + 16, (uint32_t)dy);
        wr32(data + 20, storage);
        wr32(data + 24, (uint32_t)w);
        wr32(data + 28, (uint32_t)h);
        wr32(data + 32, (uint32_t)w * 4);
        wr32(data + 36, 32);
        wr32(data + 40, 2); /* kQ3PixelTypeARGB32 */
        wr32(data + 44, 0); /* big-endian bits and bytes */
        wr32(data + 48, 0);
        wr32(data + 52, set);
        call_geometry(v, fn, 0, data);
    }
    q3_gfree(data);
    q3_release(storage);
}

/* ---------------------------------------------------------------------- */
/* Draw regions: one per draw context, the pane on its destination */

enum { kQ3XDevicePixelTypeRGB32 = 1, kQ3XDevicePixelTypeRGB16 = 4, kQ3XDevicePixelTypeIndexed8 = 7,
       kQ3XDevicePixelTypeIndexed4, kQ3XDevicePixelTypeIndexed2, kQ3XDevicePixelTypeIndexed1 };
enum { kQ3XDrawRegionServicesClearFlag = 1 };
enum { kQ3XDrawContextValidationAll = 0xFFFFFFFFu, kQ3XDrawContextValidationWindowSize = 1 << 9,
       kQ3XDrawContextValidationWindowClip = 1 << 10, kQ3XDrawContextValidationWindowPosition = 1 << 11,
       kQ3XDrawContextValidationPane = 1 << 5 };

struct region {
    gaddr g, dctx;
    gaddr descriptor, transform, clip, image;
    uint32_t image_size;
    gaddr priv, priv_delete;
    bool use_default, filled, drawn;
    /* What the context was when its validation flags were cleared. */
    bool cleared;
    uint32_t edit;
    int x, y, w, h, gx, gy;
};

enum { Q3H_DRAW_REGION = 20 };
enum { MAX_REGIONS = 32 };
static region *regions[MAX_REGIONS];

static void region_free(region *r)
{
    if (r->priv_delete) {
        uint32_t arg = r->priv;
        ppc_call(r->priv_delete, &arg, 1);
    }
    q3_handle_free(r->g);
    if (r->descriptor)
        tb_DisposePtr(r->descriptor);
    if (r->transform)
        tb_DisposePtr(r->transform);
    if (r->image)
        tb_DisposePtr(r->image);
    if (r->clip)
        tb_DisposeRgn(r->clip);
    free(r);
}

static bool is_dctx(gaddr g)
{
    q3obj *o = q3_get(g);
    return o && q3_type_is(o->type, Q3_DRAW_CONTEXT);
}

static region *region_of(gaddr dctx, bool make)
{
    for (int i = 0; i < MAX_REGIONS; i++) {
        region *r = regions[i];
        if (r && !is_dctx(r->dctx)) { /* its draw context went */
            region_free(r);
            regions[i] = NULL;
        } else if (r && r->dctx == dctx) {
            return r;
        }
    }
    if (!make || !is_dctx(dctx))
        return NULL;
    for (int i = 0; i < MAX_REGIONS; i++)
        if (!regions[i]) {
            region *r = calloc(1, sizeof *r);
            if (!r)
                return NULL;
            r->dctx = dctx;
            r->g = q3_handle_new(Q3H_DRAW_REGION, r);
            r->descriptor = tb_NewPtr(64, true, true);
            r->transform = tb_NewPtr(64, true, true);
            regions[i] = r;
            return r;
        }
    return NULL;
}

static region *region_arg(gaddr g) { return q3_handle_get(g, Q3H_DRAW_REGION); }

/* The pane: in the port (local coordinates) and on the screen. */
static bool region_area(region *r)
{
    gaddr port = 0;
    bool pixmap = false;
    if (!q3v_dctx_area(r->dctx, &port, &r->x, &r->y, &r->w, &r->h, &pixmap))
        return false;
    r->gx = r->x;
    r->gy = r->y;
    if (port) { /* the port's bounds place its local coordinates on the screen */
        gaddr bits = qd_port_is_color(port) ? rd32(rd32(port + CGrafPort_portPixMap_off)) : port + GrafPort_portBits_off;
        r->gx -= (int16_t)rd16(bits + 8);
        r->gy -= (int16_t)rd16(bits + 6);
    }
    return true;
}

static void region_frame_start(region *r)
{
    r->filled = false;
    r->drawn = false;
}

/* The image buffer to the draw context, once drawn into. */
static void region_present(region *r, q3view *v)
{
    if (!r->drawn || !r->image || !v->pixels || r->w != v->fw || r->h != v->fh)
        return;
    for (int i = 0; i < r->w * r->h; i++)
        v->pixels[i] = rd32(r->image + 4 * (uint32_t)i) | 0xFF000000u;
    q3v_present(v);
    r->drawn = false;
}

static void put_transform(region *r, bool on_screen)
{
    float m[16];
    q3m_identity(m);
    m[0] = (float)r->w / 2;
    m[5] = -(float)r->h / 2;
    m[10] = -1; /* frustum z, 0 at hither to -1 at yon, as 0 to 1 */
    m[12] = (float)r->w / 2 + (on_screen ? (float)r->gx : 0);
    m[13] = (float)r->h / 2 + (on_screen ? (float)r->gy : 0);
    q3m_put(r->transform, m);
}

/* TQ3XDrawRegionDescriptor: the image buffer's (32-bit, the pane's size)
 * or the screen's. */
static void put_descriptor(region *r, bool buffer)
{
    gaddr d = r->descriptor;
    uint32_t depth = 32, row_bytes = (uint32_t)r->w * 4, type = kQ3XDevicePixelTypeRGB32;
    if (!buffer) {
        gaddr gd = rd32(LM_MainDevice);
        gaddr pm = gd && rd32(gd) ? rd32(rd32(gd) + GDevice_gdPMap_off) : 0;
        if (pm && rd32(pm)) {
            depth = (uint16_t)rd16(rd32(pm) + PixMap_pixelSize_off);
            row_bytes = rd16(rd32(pm) + 4) & 0x3FFF;
        }
        type = depth == 32 ? kQ3XDevicePixelTypeRGB32 : depth == 16 ? kQ3XDevicePixelTypeRGB16
               : depth == 8 ? kQ3XDevicePixelTypeIndexed8 : depth == 4 ? kQ3XDevicePixelTypeIndexed4
               : depth == 2 ? kQ3XDevicePixelTypeIndexed2 : kQ3XDevicePixelTypeIndexed1;
    }
    wr32(d, (uint32_t)r->w);
    wr32(d + 4, (uint32_t)r->h);
    wr32(d + 8, row_bytes);
    wr32(d + 12, depth);
    wr32(d + 16, type);
    /* The colour descriptor: shifts and masks of red, green, blue, alpha. */
    uint32_t cd[8] = { 16, 0xFF, 8, 0xFF, 0, 0xFF, 24, 0 };
    if (depth == 16) {
        uint32_t c16[8] = { 10, 0x1F, 5, 0x1F, 0, 0x1F, 15, 0 };
        memcpy(cd, c16, sizeof cd);
    } else if (depth < 16) {
        memset(cd, 0, sizeof cd);
    }
    for (int i = 0; i < 8; i++)
        wr32(d + 20 + 4 * (uint32_t)i, cd[i]);
    wr32(d + 52, 0); /* kQ3EndianBig, bits and bytes */
    wr32(d + 56, 0);
    wr32(d + 60, 0); /* no clip mask */
}

static void fill_image(region *r, bool clear)
{
    q3view *v = q3v_drawing_into(r->dctx);
    uint32_t color = 0xFF000000u;
    bool with_color = q3v_clear_color(r->dctx, &color);
    if (clear || with_color || !v || !v->pixels || v->fw != r->w || v->fh != r->h) {
        for (uint32_t i = 0; i < (uint32_t)(r->w * r->h); i++)
            wr32(r->image + 4 * i, color & 0xFFFFFF);
        return;
    }
    q3v_read_destination(v);
    for (int i = 0; i < r->w * r->h; i++)
        wr32(r->image + 4 * (uint32_t)i, v->pixels[i] & 0xFFFFFF);
}

static void Q3XDrawContext_GetDrawRegion(ppc_cpu *c)
{
    region *r = region_of(Q3R(0), true);
    if (r && Q3R(1))
        wr32(Q3R(1), r->g);
    Q3RETURN(r && Q3R(1) ? kQ3Success : kQ3Failure);
}

/* What changed in the draw context since the flags were last cleared. */
static uint32_t validation(region *r)
{
    q3obj *o = q3_get(r->dctx);
    int x = r->x, y = r->y, w = r->w, h = r->h, gx = r->gx, gy = r->gy;
    if (!o || !r->cleared || o->edit != r->edit || !region_area(r))
        return kQ3XDrawContextValidationAll;
    uint32_t f = 0;
    if (w != r->w || h != r->h)
        f |= kQ3XDrawContextValidationWindowSize | kQ3XDrawContextValidationWindowClip | kQ3XDrawContextValidationPane;
    if (x != r->x || y != r->y || gx != r->gx || gy != r->gy)
        f |= kQ3XDrawContextValidationWindowPosition | kQ3XDrawContextValidationWindowClip;
    return f;
}

static void Q3XDrawContext_GetValidationFlags(ppc_cpu *c)
{
    region *r = region_of(Q3R(0), true);
    if (r && Q3R(1))
        wr32(Q3R(1), validation(r));
    Q3RETURN(r && Q3R(1) ? kQ3Success : kQ3Failure);
}

static void Q3XDrawContext_ClearValidationFlags(ppc_cpu *c)
{
    region *r = region_of(Q3R(0), true);
    q3obj *o = q3_get(Q3R(0));
    if (r && o && region_area(r)) {
        r->cleared = true;
        r->edit = o->edit;
    }
    Q3RETURN(r ? kQ3Success : kQ3Failure);
}

#define REGION_FLOAT(fn, expr)                                                                                       \
    static void fn(ppc_cpu *c)                                                                                       \
    {                                                                                                                \
        region *r = region_arg(Q3R(0));                                                                              \
        if (r && Q3R(1)) {                                                                                           \
            region_area(r);                                                                                          \
            wrf(Q3R(1), (float)(expr));                                                                              \
        }                                                                                                            \
        Q3RETURN(r && Q3R(1) ? kQ3Success : kQ3Failure);                                                             \
    }
/* The frustum to the device (the screen) and to the window: x * scale
 * + offset, y likewise (its scale negative, y growing down). */
REGION_FLOAT(Q3XDrawRegion_GetDeviceScaleX, r->w / 2.0)
REGION_FLOAT(Q3XDrawRegion_GetDeviceScaleY, -r->h / 2.0)
REGION_FLOAT(Q3XDrawRegion_GetDeviceOffsetX, r->gx + r->w / 2.0)
REGION_FLOAT(Q3XDrawRegion_GetDeviceOffsetY, r->gy + r->h / 2.0)
REGION_FLOAT(Q3XDrawRegion_GetWindowScaleX, r->w / 2.0)
REGION_FLOAT(Q3XDrawRegion_GetWindowScaleY, -r->h / 2.0)
REGION_FLOAT(Q3XDrawRegion_GetWindowOffsetX, r->x + r->w / 2.0)
REGION_FLOAT(Q3XDrawRegion_GetWindowOffsetY, r->y + r->h / 2.0)

static void Q3XDrawRegion_IsActive(ppc_cpu *c)
{
    region *r = region_arg(Q3R(0));
    if (r && Q3R(1))
        wr32(Q3R(1), kQ3True);
    Q3RETURN(r && Q3R(1) ? kQ3Success : kQ3Failure);
}

/* One device, one region. */
static void Q3XDrawRegion_GetNextRegion(ppc_cpu *c)
{
    region *r = region_arg(Q3R(0));
    if (Q3R(1))
        wr32(Q3R(1), 0);
    Q3RETURN(r && Q3R(1) ? kQ3Success : kQ3Failure);
}

static void Q3XDrawRegion_Start(ppc_cpu *c)
{
    region *r = region_arg(Q3R(0));
    if (!r || !region_area(r)) {
        Q3RETURN(kQ3Failure);
        return;
    }
    put_descriptor(r, false);
    put_transform(r, true);
    if (Q3R(1) & kQ3XDrawRegionServicesClearFlag) { /* the pane cleared on the destination */
        q3view *v = q3v_drawing_into(r->dctx);
        uint32_t color = 0xFF000000u;
        q3v_clear_color(r->dctx, &color);
        if (v && v->pixels && v->fw == r->w && v->fh == r->h) {
            for (int i = 0; i < r->w * r->h; i++)
                v->pixels[i] = color | 0xFF000000u;
            q3v_present(v);
        }
    }
    if (Q3R(2))
        wr32(Q3R(2), r->descriptor);
    Q3RETURN(kQ3Success);
}

static void Q3XDrawRegion_StartAccessToImageBuffer(ppc_cpu *c)
{
    region *r = region_arg(Q3R(0));
    if (!r || !region_area(r)) {
        Q3RETURN(kQ3Failure);
        return;
    }
    uint32_t size = (uint32_t)r->w * (uint32_t)r->h * 4;
    if (!r->image || r->image_size != size) {
        if (r->image)
            tb_DisposePtr(r->image);
        r->image = tb_NewPtr((int32_t)size, false, false);
        r->image_size = r->image ? size : 0;
        r->filled = false;
    }
    if (!r->image) {
        q3_error(kQ3ErrorOutOfMemory);
        Q3RETURN(kQ3Failure);
        return;
    }
    bool clear = Q3R(1) & kQ3XDrawRegionServicesClearFlag;
    if (!r->filled || clear) {
        fill_image(r, clear);
        r->filled = true;
    }
    r->drawn = true;
    put_descriptor(r, true);
    put_transform(r, false);
    if (Q3R(2))
        wr32(Q3R(2), r->descriptor);
    if (Q3R(3))
        wr32(Q3R(3), r->image);
    Q3RETURN(kQ3Success);
}

/* The end of an access: drawn after the pass, or now if no pass is on. */
static void Q3XDrawRegion_End(ppc_cpu *c)
{
    region *r = region_arg(Q3R(0));
    q3view *v = r ? q3v_drawing_into(r->dctx) : NULL;
    if (r && v && v->loop != LOOP_RENDER)
        region_present(r, v);
    Q3RETURN(r ? kQ3Success : kQ3Failure);
}

static void Q3XDrawRegion_GetDeviceTransform(ppc_cpu *c)
{
    region *r = region_arg(Q3R(0));
    if (r && Q3R(1)) {
        if (!rd32(r->transform)) { /* not started yet: the buffer's */
            region_area(r);
            put_transform(r, false);
        }
        wr32(Q3R(1), r->transform);
    }
    Q3RETURN(r && Q3R(1) ? kQ3Success : kQ3Failure);
}

/* A draw context's mask makes the pane partly exposed. */
static void Q3XDrawRegion_GetClipFlags(ppc_cpu *c)
{
    region *r = region_arg(Q3R(0));
    q3obj *o = r ? q3_get(r->dctx) : NULL;
    bool masked = o && o->data && ((q3node *)o->data)->w[15];
    if (r && Q3R(1))
        wr32(Q3R(1), masked ? 1 : 0); /* kQ3XClipMaskPartiallyExposed, FullyExposed */
    Q3RETURN(r && Q3R(1) ? kQ3Success : kQ3Failure);
}

static void Q3XDrawRegion_GetClipMask(ppc_cpu *c)
{
    region *r = region_arg(Q3R(0));
    q3obj *o = r ? q3_get(r->dctx) : NULL;
    gaddr mask = 0;
    if (o && o->data && ((q3node *)o->data)->w[15]) { /* TQ3Bitmap at word 10 of the context's data */
        q3node *bits = q3d_kid(o->data, q3_schema_of(o->type), 0, 0);
        const uint32_t *w = ((q3node *)o->data)->w;
        if (bits && bits->bytes) {
            mask = tb_NewPtr((int32_t)(20 + bits->nbytes), true, true);
            if (mask) {
                gput(mask + 20, bits->bytes, bits->nbytes);
                wr32(mask, mask + 20);
                for (int k = 1; k < 5; k++)
                    wr32(mask + 4 * (uint32_t)k, w[10 + k]);
            }
        }
    }
    if (r && Q3R(1))
        wr32(Q3R(1), mask);
    Q3RETURN(r && Q3R(1) ? kQ3Success : kQ3Failure);
}

/* The port's visible region and clip, as QuickDraw clips drawing there. */
static void Q3XDrawRegion_GetClipRegion(ppc_cpu *c)
{
    region *r = region_arg(Q3R(0));
    q3obj *o = r ? q3_get(r->dctx) : NULL;
    gaddr port = 0;
    if (o && o->type == DC_MAC && o->data) {
        const uint32_t *w = ((q3node *)o->data)->w;
        port = w[20] ? w[20] : w[17];
    }
    if (r && port) {
        if (!r->clip)
            r->clip = tb_NewRgn();
        tb_SectRgn(GrafPort_visRgn(port), GrafPort_clipRgn(port), r->clip);
    }
    if (r && Q3R(1))
        wr32(Q3R(1), port ? r->clip : 0);
    Q3RETURN(r && port && Q3R(1) ? kQ3Success : kQ3Failure);
}

static void Q3XDrawRegion_GetGDHandle(ppc_cpu *c)
{
    region *r = region_arg(Q3R(0));
    if (r && Q3R(1))
        wr32(Q3R(1), rd32(LM_MainDevice));
    Q3RETURN(r && Q3R(1) ? kQ3Success : kQ3Failure);
}

static void Q3XDrawRegion_GetRendererPrivate(ppc_cpu *c)
{
    region *r = region_arg(Q3R(0));
    if (r && Q3R(1))
        wr32(Q3R(1), r->priv);
    Q3RETURN(r && Q3R(1) ? kQ3Success : kQ3Failure);
}

static void Q3XDrawRegion_SetRendererPrivate(ppc_cpu *c)
{
    region *r = region_arg(Q3R(0));
    if (r) {
        if (r->priv_delete && r->priv != Q3R(1)) {
            uint32_t arg = r->priv;
            ppc_call(r->priv_delete, &arg, 1);
        }
        r->priv = Q3R(1);
        r->priv_delete = Q3R(2);
    }
    Q3RETURN(r ? kQ3Success : kQ3Failure);
}

static void Q3XDrawRegion_SetUseDefaultRendererFlag(ppc_cpu *c)
{
    region *r = region_arg(Q3R(0));
    if (r)
        r->use_default = Q3R(1) != 0;
    Q3RETURN(r ? kQ3Success : kQ3Failure);
}

static void Q3XDrawRegion_GetUseDefaultRendererFlag(ppc_cpu *c)
{
    region *r = region_arg(Q3R(0));
    if (r && Q3R(1))
        wr32(Q3R(1), r->use_default);
    Q3RETURN(r && Q3R(1) ? kQ3Success : kQ3Failure);
}

/* ---------------------------------------------------------------------- */
/* The rest of the calls a renderer makes */

/* An attribute's data where the set keeps it: a guest copy made once,
 * good while the attribute stays. */
static void Q3XAttributeSet_GetPointer(ppc_cpu *c)
{
    q3set *s = q3_set_of(Q3R(0));
    q3elem *e = s ? (q3elem *)q3set_find(s, (int32_t)Q3R(1)) : NULL;
    if (e && !e->g) {
        e->g = tb_NewPtr((int32_t)(e->size ? e->size : 4), false, true);
        if (e->g)
            gput(e->g, e->bytes, e->size);
    }
    Q3RETURN(e ? e->g : 0);
}

/* An asynchronous renderer's frame is done: the view's end-frame method. */
static void Q3XView_EndFrame(ppc_cpu *c)
{
    q3view *v = q3v_of(Q3R(0));
    if (v) {
        region *r = region_of(v->dctx, false);
        if (r)
            region_present(r, v);
        if (v->end_frame) {
            uint32_t args[2] = { v->g, v->end_frame_data };
            ppc_call(v->end_frame, args, 2);
        }
    }
    Q3RETURN(v ? kQ3Success : kQ3Failure);
}

/* How far a long frame has got: the view's idle progress method (or its
 * idle method); a failure cancels the frame. */
static void Q3XView_IdleProgress(ppc_cpu *c)
{
    q3view *v = q3v_of(Q3R(0));
    uint32_t status = kQ3Success;
    if (v && v->progress) {
        uint32_t args[4] = { v->g, v->progress_data, Q3R(1), Q3R(2) };
        status = ppc_call(v->progress, args, 4);
    } else if (v && v->idle) {
        uint32_t args[2] = { v->g, v->idle_data };
        status = ppc_call(v->idle, args, 2);
    }
    if (v && status != kQ3Success) {
        v->cancelled = true;
        q3xr_cancel(v);
    }
    Q3RETURN(v ? status : kQ3Failure);
}

/* ---------------------------------------------------------------------- */
/* A registered renderer's class methods (Q3Renderer_ and
 * Q3RendererClass_ calls, q3_shapes.c) */

gaddr q3xr_class_method(uint32_t renderer_type, uint32_t method);
gaddr q3xr_class_method(uint32_t renderer_type, uint32_t method)
{
    q3class *k = q3_class(renderer_type);
    return k && k->registered && q3_type_is(k->type, Q3_RENDERER) ? q3_class_method(k, method) : 0;
}

bool q3xr_interactive(gaddr renderer);
bool q3xr_interactive(gaddr renderer)
{
    q3class *k = plugin_class(renderer);
    /* The "method" is the answer itself. */
    return k && q3_class_method(k, M_INTERACTIVE) != 0;
}

#define E(n) { #n, n }
const ppc_export q3_xrender_exports[] = {
    E(Q3XDrawContext_GetDrawRegion), E(Q3XDrawContext_GetValidationFlags), E(Q3XDrawContext_ClearValidationFlags),
    E(Q3XDrawRegion_GetDeviceScaleX), E(Q3XDrawRegion_GetDeviceScaleY), E(Q3XDrawRegion_GetDeviceOffsetX),
    E(Q3XDrawRegion_GetDeviceOffsetY), E(Q3XDrawRegion_GetWindowScaleX), E(Q3XDrawRegion_GetWindowScaleY),
    E(Q3XDrawRegion_GetWindowOffsetX), E(Q3XDrawRegion_GetWindowOffsetY), E(Q3XDrawRegion_IsActive),
    E(Q3XDrawRegion_GetNextRegion), E(Q3XDrawRegion_Start), E(Q3XDrawRegion_StartAccessToImageBuffer),
    E(Q3XDrawRegion_End), E(Q3XDrawRegion_GetDeviceTransform), E(Q3XDrawRegion_GetClipFlags),
    E(Q3XDrawRegion_GetClipMask), E(Q3XDrawRegion_GetClipRegion), E(Q3XDrawRegion_GetGDHandle),
    E(Q3XDrawRegion_GetRendererPrivate), E(Q3XDrawRegion_SetRendererPrivate),
    E(Q3XDrawRegion_SetUseDefaultRendererFlag), E(Q3XDrawRegion_GetUseDefaultRendererFlag),
    E(Q3XAttributeSet_GetPointer), E(Q3XView_EndFrame), E(Q3XView_IdleProgress),
};
const unsigned q3_xrender_export_count = sizeof q3_xrender_exports / sizeof q3_xrender_exports[0];
