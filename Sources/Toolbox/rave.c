/*
 * RAVE, QuickDraw 3D's rasterizer interface (QuickDraw™ 3D Accelerator,
 * RAVE.h), on the host's renderer (r3d.h): one engine, draw contexts
 * whose method table is transition vectors to the calls here, textures
 * and bitmaps.
 *
 * A draw context renders into a host frame its rectangle's size; drawing
 * is batched there, and QARenderEnd (or QASwapBuffers, with
 * kQATag_DontSwap set) reads the frame back into the device: a memory
 * device's pixels at its pixel type, or a GDevice's pixel map through the
 * clip region the program gave.  Vertex coordinates are the draw
 * context's own (0, 0 at its rectangle's top left).
 *
 * The engine is Apple's (vendor 0) with an engine ID of its own, and says
 * the features it has are fast: games choose an engine on that.  Colour
 * table pixel types (CL4, CL8), the buffer access calls and multiple
 * textures are not supported yet.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "q3_internal.h"
#include "tb_internal.h"

enum {
    kQANoErr = 0, kQAError = 1, kQAOutOfMemory = 2, kQANotSupported = 3, kQAParamErr = 5, kQAGestaltUnknown = 6,
};
enum { kQADeviceMemory = 0, kQADeviceGDevice = 1 };
enum {
    kQAPixel_Alpha1 = 0, kQAPixel_RGB16 = 1, kQAPixel_ARGB16 = 2, kQAPixel_RGB32 = 3, kQAPixel_ARGB32 = 4,
    kQAPixel_RGB16_565 = 7, kQAPixel_RGB24 = 8, kQAPixel_ARGB16_4444 = 10,
};
enum {
    TAG_ZFunction = 0, TAG_Antialias = 8, TAG_Blend = 9, TAG_PerspectiveZ = 10, TAG_TextureFilter = 11,
    TAG_TextureOp = 12, TAG_Texture = 13, TAG_ZBufferMask = 28, TAG_DontSwap = 32, TAG_BitmapFilter = 54,
    TAG_GL_TextureWrapU = 101, TAG_GL_TextureWrapV = 102,
};
enum {
    TAG_ColorBG_a = 1, TAG_ColorBG_r, TAG_ColorBG_g, TAG_ColorBG_b, TAG_Width = 5, TAG_ZMinOffset = 6,
    TAG_ZMinScale = 7, TAG_BitmapScale_x = 52, TAG_BitmapScale_y = 53,
};
enum { kQAVertexMode_Point, kQAVertexMode_Line, kQAVertexMode_Polyline, kQAVertexMode_Tri, kQAVertexMode_Strip,
       kQAVertexMode_Fan };
enum {
    kQAGestalt_OptionalFeatures, kQAGestalt_FastFeatures, kQAGestalt_VendorID, kQAGestalt_EngineID,
    kQAGestalt_Revision, kQAGestalt_ASCIINameLength, kQAGestalt_ASCIIName, kQAGestalt_TextureMemory,
    kQAGestalt_FastTextureMemory, kQAGestalt_DrawContextPixelTypesAllowed,
    kQAGestalt_DrawContextPixelTypesPreferred, kQAGestalt_TexturePixelTypesAllowed,
    kQAGestalt_TexturePixelTypesPreferred, kQAGestalt_BitmapPixelTypesAllowed,
    kQAGestalt_BitmapPixelTypesPreferred, kQAGestalt_OptionalFeatures2, kQAGestalt_MultiTextureMax,
};
enum { kQAContext_NoZBuffer = 1, kQAContext_DoubleBuffer = 4 };
enum { kQATexture_Mipmap = 2, kQATexture_NoCopy = 32, kQATexture_FlipOrigin = 64 };
enum { kQAVersion_1_6 = 4 };

#define ENGINE_ID 0x426C7565 /* 'Blue' */
#define TAG_CTX 0x51416463u  /* 'QAdc' */
#define TAG_TEX 0x51417478u  /* 'QAtx' */
#define TAG_BMP 0x5141626Du  /* 'QAbm' */
static const char engine_name[] = "Blue Box Metal Engine";

typedef struct {
    gaddr guest;
    int texture;
    int width, height;
    bool bitmap, alpha1;
    /* NoCopy: the program's pixels, read again at QATextureDetach. */
    int format;
    uint32_t flags;
    gaddr image;
    uint32_t row_bytes;
} rtex;

typedef struct {
    gaddr guest;
    int devtype;
    gaddr gdevice;
    int32_t mem_row_bytes, mem_type, mem_width, mem_height;
    gaddr mem_base;
    int left, top, width, height;
    gaddr clip;
    uint32_t flags;
    uint32_t ints[160];
    float floats[160];
    gaddr texture, multitexture;
    r3d_frame *frame;
    uint32_t *pixels;
    bool rendering;
    gaddr notice[5], notice_ref[5];
    r3d_vertex *submitted;
    int nsubmitted;
} rctx;

enum { MAX_CTX = 64, MAX_TEX = 4096 };
static rctx *ctxs[MAX_CTX];
static rtex *texs[MAX_TEX];
static gaddr engine;

static gaddr the_engine(void)
{
    if (!engine && r3d_available()) {
        engine = tb_NewPtr(16, true, true);
        if (engine)
            wr32(engine, 0x51416567); /* 'QAeg' */
    }
    return engine;
}

static rctx *ctx_of(gaddr dc)
{
    if (!dc)
        return NULL;
    uint32_t priv = rd32(dc);
    if ((priv >> 16) != (TAG_CTX >> 16))
        return NULL;
    unsigned i = priv & 0xFFFF;
    return i < MAX_CTX && ctxs[i] && ctxs[i]->guest == dc ? ctxs[i] : NULL;
}

static rtex *tex_of(gaddr t)
{
    if (!t)
        return NULL;
    uint32_t tag = rd32(t), i = rd32(t + 4);
    if ((tag != TAG_TEX && tag != TAG_BMP) || i >= MAX_TEX || !texs[i] || texs[i]->guest != t)
        return NULL;
    return texs[i];
}

/* ---------------------------------------------------------------------- */
/* Vertices and state */

static float depth_of(const rctx *x, float z, float invw)
{
    if (x->ints[TAG_PerspectiveZ])
        return invw > 0 ? 1.0f / (1.0f + invw) : 1.0f;
    return z;
}

static void gouraud(const rctx *x, gaddr v, r3d_vertex *o)
{
    o->x = rdf(v);
    o->y = rdf(v + 4);
    o->z = depth_of(x, rdf(v + 8), rdf(v + 12));
    o->invw = 1;
    o->r = rdf(v + 16);
    o->g = rdf(v + 20);
    o->b = rdf(v + 24);
    o->a = rdf(v + 28);
    o->u = o->v = 0;
    o->kd_r = o->kd_g = o->kd_b = 1;
    o->ks_r = o->ks_g = o->ks_b = 0;
}

static void textured(const rctx *x, gaddr v, r3d_vertex *o)
{
    float invw = rdf(v + 12);
    o->x = rdf(v);
    o->y = rdf(v + 4);
    o->z = depth_of(x, rdf(v + 8), invw);
    o->invw = invw > 0 ? invw : 1;
    o->r = rdf(v + 16);
    o->g = rdf(v + 20);
    o->b = rdf(v + 24);
    o->a = rdf(v + 28);
    o->u = rdf(v + 32) / o->invw;
    o->v = rdf(v + 36) / o->invw;
    o->kd_r = rdf(v + 40);
    o->kd_g = rdf(v + 44);
    o->kd_b = rdf(v + 48);
    o->ks_r = rdf(v + 52);
    o->ks_g = rdf(v + 56);
    o->ks_b = rdf(v + 60);
}

static r3d_state state_of(const rctx *x, bool texture)
{
    r3d_state s;
    memset(&s, 0, sizeof s);
    s.zfunc = x->flags & kQAContext_NoZBuffer ? 0 : (int)x->ints[TAG_ZFunction];
    s.zwrite = x->ints[TAG_ZBufferMask] != 0;
    s.blend = x->ints[TAG_Blend] == 0 ? R3D_BLEND_PREMULTIPLY : R3D_BLEND_INTERPOLATE;
    rtex *t = texture ? tex_of(x->texture) : NULL;
    s.texture = t ? t->texture : 0;
    s.texop = (int)x->ints[TAG_TextureOp];
    s.linear = x->ints[TAG_TextureFilter] != 0;
    s.clamp_u = x->ints[TAG_GL_TextureWrapU] != 0;
    s.clamp_v = x->ints[TAG_GL_TextureWrapV] != 0;
    s.size = x->floats[TAG_Width] > 0 ? x->floats[TAG_Width] : 1;
    return s;
}

static void begin(rctx *x)
{
    if (x->rendering)
        return;
    uint32_t bg = (uint32_t)lrintf(fminf(fmaxf(x->floats[TAG_ColorBG_a], 0), 1) * 255) << 24 |
                  (uint32_t)lrintf(fminf(fmaxf(x->floats[TAG_ColorBG_r], 0), 1) * 255) << 16 |
                  (uint32_t)lrintf(fminf(fmaxf(x->floats[TAG_ColorBG_g], 0), 1) * 255) << 8 |
                  (uint32_t)lrintf(fminf(fmaxf(x->floats[TAG_ColorBG_b], 0), 1) * 255);
    r3d_begin(x->frame, NULL, bg, 1.0f);
    x->rendering = true;
}

static void draw(rctx *x, bool texture, int prim, const r3d_vertex *v, int n)
{
    begin(x);
    r3d_state s = state_of(x, texture);
    r3d_draw(x->frame, &s, prim, v, n);
}

/* A run of n vertices of vertex mode m as primitives. */
static void draw_mode(rctx *x, bool texture, int mode, const r3d_vertex *v, int n)
{
    switch (mode) {
    case kQAVertexMode_Point: draw(x, texture, R3D_POINTS, v, n); return;
    case kQAVertexMode_Line: draw(x, texture, R3D_LINES, v, n & ~1); return;
    case kQAVertexMode_Tri: draw(x, texture, R3D_TRIANGLES, v, n - n % 3); return;
    default: break;
    }
    if (n < 2)
        return;
    bool lines = mode == kQAVertexMode_Polyline;
    int count = lines ? 2 * (n - 1) : 3 * (n - 2);
    if (count <= 0)
        return;
    r3d_vertex *o = malloc((size_t)count * sizeof *o);
    if (!o)
        return;
    int k = 0;
    for (int i = 0; i + 1 < n && lines; i++) {
        o[k++] = v[i];
        o[k++] = v[i + 1];
    }
    for (int i = 0; i + 2 < n && !lines; i++) {
        o[k++] = mode == kQAVertexMode_Fan ? v[0] : v[i];
        o[k++] = v[i + 1];
        o[k++] = v[i + 2];
    }
    draw(x, texture, lines ? R3D_LINES : R3D_TRIANGLES, o, k);
    free(o);
}

/* ---------------------------------------------------------------------- */
/* Showing the frame */

static int device_format(int32_t rave_type)
{
    switch (rave_type) {
    case kQAPixel_RGB16: return Q3PX_RGB16;
    case kQAPixel_ARGB16: return Q3PX_ARGB16;
    case kQAPixel_RGB32: return Q3PX_RGB32;
    case kQAPixel_ARGB32: return Q3PX_ARGB32;
    case kQAPixel_RGB16_565: return Q3PX_RGB16_565;
    case kQAPixel_RGB24: return Q3PX_RGB24;
    default: return -1;
    }
}

static void present(rctx *x)
{
    if (x->rendering) {
        r3d_end(x->frame, x->pixels);
        x->rendering = false;
    }
    if (x->devtype == kQADeviceMemory) {
        int fmt = device_format(x->mem_type);
        if (fmt < 0 || !x->mem_base)
            return;
        for (int row = 0; row < x->height; row++) {
            int ty = x->top + row;
            if (ty < 0 || ty >= x->mem_height)
                continue;
            for (int col = 0; col < x->width; col++) {
                int tx = x->left + col;
                if (tx < 0 || tx >= x->mem_width)
                    continue;
                uint32_t v = x->pixels[(size_t)row * (size_t)x->width + (size_t)col];
                gaddr p = x->mem_base + (uint32_t)ty * (uint32_t)x->mem_row_bytes;
                switch (fmt) {
                case Q3PX_RGB32:
                case Q3PX_ARGB32: wr32(p + (uint32_t)tx * 4, fmt == Q3PX_RGB32 ? v & 0xFFFFFF : v); break;
                case Q3PX_RGB24:
                    wr8(p + (uint32_t)tx * 3, (uint8_t)(v >> 16));
                    wr8(p + (uint32_t)tx * 3 + 1, (uint8_t)(v >> 8));
                    wr8(p + (uint32_t)tx * 3 + 2, (uint8_t)v);
                    break;
                case Q3PX_RGB16_565:
                    wr16(p + (uint32_t)tx * 2, (uint16_t)((v >> 8 & 0xF800) | (v >> 5 & 0x07E0) | (v >> 3 & 0x1F)));
                    break;
                default:
                    wr16(p + (uint32_t)tx * 2, (uint16_t)((fmt == Q3PX_ARGB16 && v >> 31 ? 0x8000 : 0) |
                                                          (v >> 9 & 0x7C00) | (v >> 6 & 0x03E0) | (v >> 3 & 0x1F)));
                    break;
                }
            }
        }
        return;
    }
    if (!x->gdevice || !rd32(x->gdevice))
        return;
    gaddr pmh = rd32(rd32(x->gdevice) + GDevice_gdPMap_off);
    gaddr pm = pmh ? rd32(pmh) : 0;
    if (!pm)
        return;
    pixdst d;
    d.base = PixMap_baseAddr(pm);
    d.row_bytes = (uint16_t)PixMap_rowBytes(pm) & 0x3FFF;
    d.bounds = qrect_get(pm + PixMap_bounds_off);
    d.depth = PixMap_pixelSize(pm);
    d.ctab = PixMap_pmTable(pm);
    pixdst_clamp(&d);
    hrgn clip;
    hrgn_init(&clip);
    if (x->clip && rd32(x->clip))
        rgn_read(x->clip, &clip);
    else
        hrgn_rect(&clip, d.bounds);
    q3_put_pixels(x->pixels, x->width, x->height, &d, x->left, x->top, &clip);
    hrgn_free(&clip);
}

static void notify(rctx *x, int method)
{
    gaddr fn = x->notice[method];
    if (fn) {
        uint32_t args[4] = { x->guest, x->notice_ref[method] };
        ppc_call(fn, args, 2);
    }
}

/* ---------------------------------------------------------------------- */
/* The draw context's methods */

static void m_setFloat(ppc_cpu *c)
{
    rctx *x = ctx_of(Q3R(0));
    uint32_t tag = Q3R(1);
    if (x && tag < 160)
        x->floats[tag] = q3_farg(c, 0);
    Q3RETURN(0);
}

static void m_setInt(ppc_cpu *c)
{
    rctx *x = ctx_of(Q3R(0));
    uint32_t tag = Q3R(1);
    if (x && tag < 160)
        x->ints[tag] = Q3R(2);
    Q3RETURN(0);
}

static void m_setPtr(ppc_cpu *c)
{
    rctx *x = ctx_of(Q3R(0));
    if (x && Q3R(1) == TAG_Texture)
        x->texture = Q3R(2);
    else if (x && Q3R(1) == 26)
        x->multitexture = Q3R(2);
    Q3RETURN(0);
}

static void m_getFloat(ppc_cpu *c)
{
    rctx *x = ctx_of(Q3R(0));
    uint32_t tag = Q3R(1);
    q3_return_float(c, x && tag < 160 ? x->floats[tag] : 0);
}

static void m_getInt(ppc_cpu *c)
{
    rctx *x = ctx_of(Q3R(0));
    uint32_t tag = Q3R(1);
    Q3RETURN(x && tag < 160 ? x->ints[tag] : 0);
}

static void m_getPtr(ppc_cpu *c)
{
    rctx *x = ctx_of(Q3R(0));
    Q3RETURN(!x ? 0 : Q3R(1) == TAG_Texture ? x->texture : Q3R(1) == 26 ? x->multitexture : 0);
}

static void m_drawPoint(ppc_cpu *c)
{
    rctx *x = ctx_of(Q3R(0));
    if (x) {
        r3d_vertex v;
        gouraud(x, Q3R(1), &v);
        draw(x, false, R3D_POINTS, &v, 1);
    }
    Q3RETURN(0);
}

static void m_drawLine(ppc_cpu *c)
{
    rctx *x = ctx_of(Q3R(0));
    if (x) {
        r3d_vertex v[2];
        gouraud(x, Q3R(1), &v[0]);
        gouraud(x, Q3R(2), &v[1]);
        draw(x, false, R3D_LINES, v, 2);
    }
    Q3RETURN(0);
}

static void m_drawTriGouraud(ppc_cpu *c)
{
    rctx *x = ctx_of(Q3R(0));
    if (x) {
        r3d_vertex v[3];
        for (int i = 0; i < 3; i++)
            gouraud(x, Q3R(1 + i), &v[i]);
        draw(x, false, R3D_TRIANGLES, v, 3);
    }
    Q3RETURN(0);
}

static void m_drawTriTexture(ppc_cpu *c)
{
    rctx *x = ctx_of(Q3R(0));
    if (x) {
        r3d_vertex v[3];
        for (int i = 0; i < 3; i++)
            textured(x, Q3R(1 + i), &v[i]);
        draw(x, true, R3D_TRIANGLES, v, 3);
    }
    Q3RETURN(0);
}

static void draw_v(ppc_cpu *c, bool texture)
{
    rctx *x = ctx_of(Q3R(0));
    uint32_t n = Q3R(1), mode = Q3R(2);
    gaddr verts = Q3R(3);
    if (!x || !n || n > 1000000 || !verts) {
        Q3RETURN(0);
        return;
    }
    r3d_vertex *v = malloc(n * sizeof *v);
    if (v) {
        for (uint32_t i = 0; i < n; i++) {
            if (texture)
                textured(x, verts + i * 64, &v[i]);
            else
                gouraud(x, verts + i * 32, &v[i]);
        }
        draw_mode(x, texture, (int)mode, v, (int)n);
        free(v);
    }
    Q3RETURN(0);
}
static void m_drawVGouraud(ppc_cpu *c) { draw_v(c, false); }
static void m_drawVTexture(ppc_cpu *c) { draw_v(c, true); }

static void m_drawBitmap(ppc_cpu *c)
{
    rctx *x = ctx_of(Q3R(0));
    rtex *b = tex_of(Q3R(2));
    if (!x || !b) {
        Q3RETURN(0);
        return;
    }
    r3d_vertex at;
    gouraud(x, Q3R(1), &at);
    float sx = x->floats[TAG_BitmapScale_x] > 0 ? x->floats[TAG_BitmapScale_x] : 1;
    float sy = x->floats[TAG_BitmapScale_y] > 0 ? x->floats[TAG_BitmapScale_y] : 1;
    float x0 = at.x, y0 = at.y, x1 = x0 + b->width * sx, y1 = y0 + b->height * sy;
    r3d_vertex q[6];
    float xs[6] = { x0, x1, x0, x1, x1, x0 }, ys[6] = { y0, y0, y1, y0, y1, y1 };
    float us[6] = { 0, 1, 0, 1, 1, 0 }, vs[6] = { 0, 0, 1, 0, 1, 1 };
    for (int i = 0; i < 6; i++) {
        q[i] = at;
        q[i].x = xs[i];
        q[i].y = ys[i];
        q[i].u = us[i];
        q[i].v = vs[i];
        /* A 1-bit bitmap draws in the vertex's colour. */
        q[i].kd_r = b->alpha1 ? at.r : 1;
        q[i].kd_g = b->alpha1 ? at.g : 1;
        q[i].kd_b = b->alpha1 ? at.b : 1;
        q[i].a = b->alpha1 ? at.a : 1;
    }
    begin(x);
    r3d_state s = state_of(x, false);
    s.texture = b->texture;
    s.texop = R3D_TEXOP_MODULATE;
    s.blend = R3D_BLEND_INTERPOLATE;
    s.linear = x->ints[TAG_BitmapFilter] != 0;
    s.clamp_u = s.clamp_v = true;
    r3d_draw(x->frame, &s, R3D_TRIANGLES, q, 6);
    Q3RETURN(0);
}

static void m_renderStart(ppc_cpu *c)
{
    rctx *x = ctx_of(Q3R(0));
    if (x) {
        x->rendering = false;
        begin(x);
    }
    Q3RETURN(0);
}

static void m_renderEnd(ppc_cpu *c)
{
    rctx *x = ctx_of(Q3R(0));
    if (!x) {
        Q3RETURN(kQAParamErr);
        return;
    }
    if (!x->ints[TAG_DontSwap]) {
        present(x);
    } else if (x->rendering) {
        r3d_end(x->frame, x->pixels);
        x->rendering = false;
    }
    notify(x, 0); /* kQAMethod_RenderCompletion */
    Q3RETURN(kQANoErr);
}

static void m_renderAbort(ppc_cpu *c)
{
    rctx *x = ctx_of(Q3R(0));
    if (x && x->rendering) {
        r3d_end(x->frame, NULL);
        x->rendering = false;
    }
    Q3RETURN(kQANoErr);
}

static void m_noErr(ppc_cpu *c) { Q3RETURN(kQANoErr); }
static void m_notSupported(ppc_cpu *c) { Q3RETURN(kQANotSupported); }
static void m_busy(ppc_cpu *c) { Q3RETURN(0); }
static void m_nothing(ppc_cpu *c) { Q3RETURN(0); }

static void submit(ppc_cpu *c, bool texture)
{
    rctx *x = ctx_of(Q3R(0));
    uint32_t n = Q3R(1);
    gaddr verts = Q3R(2);
    if (!x || n > 1000000) {
        Q3RETURN(0);
        return;
    }
    r3d_vertex *v = realloc(x->submitted, (n ? n : 1) * sizeof *v);
    if (!v) {
        Q3RETURN(0);
        return;
    }
    x->submitted = v;
    x->nsubmitted = (int)n;
    for (uint32_t i = 0; i < n; i++) {
        if (texture)
            textured(x, verts + i * 64, &v[i]);
        else
            gouraud(x, verts + i * 32, &v[i]);
    }
    Q3RETURN(0);
}
static void m_submitVerticesGouraud(ppc_cpu *c) { submit(c, false); }
static void m_submitVerticesTexture(ppc_cpu *c) { submit(c, true); }

static void trimesh(ppc_cpu *c, bool texture)
{
    rctx *x = ctx_of(Q3R(0));
    uint32_t n = Q3R(1);
    gaddr tris = Q3R(2);
    if (!x || !n || n > 1000000 || !x->nsubmitted) {
        Q3RETURN(0);
        return;
    }
    r3d_vertex *v = malloc(n * 3 * sizeof *v);
    int k = 0;
    for (uint32_t i = 0; v && i < n; i++) {
        uint32_t a = rd32(tris + i * 16 + 4), b = rd32(tris + i * 16 + 8), d = rd32(tris + i * 16 + 12);
        if (a >= (uint32_t)x->nsubmitted || b >= (uint32_t)x->nsubmitted || d >= (uint32_t)x->nsubmitted)
            continue;
        v[k++] = x->submitted[a];
        v[k++] = x->submitted[b];
        v[k++] = x->submitted[d];
    }
    if (v)
        draw(x, texture, R3D_TRIANGLES, v, k);
    free(v);
    Q3RETURN(0);
}
static void m_drawTriMeshGouraud(ppc_cpu *c) { trimesh(c, false); }
static void m_drawTriMeshTexture(ppc_cpu *c) { trimesh(c, true); }

static void m_setNoticeMethod(ppc_cpu *c)
{
    rctx *x = ctx_of(Q3R(0));
    uint32_t m = Q3R(1);
    if (!x || m >= 5) {
        Q3RETURN(kQAParamErr);
        return;
    }
    x->notice[m] = Q3R(2);
    x->notice_ref[m] = Q3R(3);
    Q3RETURN(kQANoErr);
}

static void m_getNoticeMethod(ppc_cpu *c)
{
    rctx *x = ctx_of(Q3R(0));
    uint32_t m = Q3R(1);
    if (!x || m >= 5) {
        Q3RETURN(kQAParamErr);
        return;
    }
    if (Q3R(2))
        wr32(Q3R(2), x->notice[m]);
    if (Q3R(3))
        wr32(Q3R(3), x->notice_ref[m]);
    Q3RETURN(kQANoErr);
}

static void clear(ppc_cpu *c, bool color)
{
    rctx *x = ctx_of(Q3R(0));
    gaddr r = Q3R(1);
    if (!x) {
        Q3RETURN(kQAParamErr);
        return;
    }
    begin(x);
    int x0 = 0, y0 = 0, x1 = x->width, y1 = x->height;
    if (r) {
        x0 = (int32_t)rd32(r);
        x1 = (int32_t)rd32(r + 4);
        y0 = (int32_t)rd32(r + 8);
        y1 = (int32_t)rd32(r + 12);
    }
    uint32_t bg = (uint32_t)lrintf(fminf(fmaxf(x->floats[TAG_ColorBG_a], 0), 1) * 255) << 24 |
                  (uint32_t)lrintf(fminf(fmaxf(x->floats[TAG_ColorBG_r], 0), 1) * 255) << 16 |
                  (uint32_t)lrintf(fminf(fmaxf(x->floats[TAG_ColorBG_g], 0), 1) * 255) << 8 |
                  (uint32_t)lrintf(fminf(fmaxf(x->floats[TAG_ColorBG_b], 0), 1) * 255);
    r3d_clear(x->frame, x0, y0, x1, y1, color, bg, !color, 1.0f);
    Q3RETURN(kQANoErr);
}
static void m_clearDrawBuffer(ppc_cpu *c) { clear(c, true); }
static void m_clearZBuffer(ppc_cpu *c) { clear(c, false); }

static void m_swapBuffers(ppc_cpu *c)
{
    rctx *x = ctx_of(Q3R(0));
    if (x) {
        if (x->rendering)
            present(x);
        else if (x->ints[TAG_DontSwap]) {
            x->rendering = false;
            /* Already read back by QARenderEnd: show it. */
            int t = x->ints[TAG_DontSwap];
            x->ints[TAG_DontSwap] = 0;
            present(x);
            x->ints[TAG_DontSwap] = (uint32_t)t;
        }
    }
    Q3RETURN(kQANoErr);
}

static void (*const methods[35])(ppc_cpu *) = {
    m_setFloat,       m_setInt,         m_setPtr,
    m_getFloat,       m_getInt,         m_getPtr,
    m_drawPoint,      m_drawLine,       m_drawTriGouraud,
    m_drawTriTexture, m_drawVGouraud,   m_drawVTexture,
    m_drawBitmap,     m_renderStart,    m_renderEnd,
    m_renderAbort,    m_noErr,          m_noErr, /* flush, sync */
    m_submitVerticesGouraud, m_submitVerticesTexture, m_drawTriMeshGouraud,
    m_drawTriMeshTexture, m_setNoticeMethod, m_getNoticeMethod,
    m_nothing, /* submitMultiTextureParams */
    m_notSupported, m_notSupported, m_notSupported, m_notSupported, /* buffer access */
    m_clearDrawBuffer, m_clearZBuffer,
    m_notSupported, m_notSupported, /* texture and bitmap from the context */
    m_busy,           m_swapBuffers,
};
static const char *const method_names[35] = {
    "QA setFloat", "QA setInt", "QA setPtr", "QA getFloat", "QA getInt", "QA getPtr", "QA drawPoint",
    "QA drawLine", "QA drawTriGouraud", "QA drawTriTexture", "QA drawVGouraud", "QA drawVTexture",
    "QA drawBitmap", "QA renderStart", "QA renderEnd", "QA renderAbort", "QA flush", "QA sync",
    "QA submitVerticesGouraud", "QA submitVerticesTexture", "QA drawTriMeshGouraud", "QA drawTriMeshTexture",
    "QA setNoticeMethod", "QA getNoticeMethod", "QA submitMultiTextureParams", "QA accessDrawBuffer",
    "QA accessDrawBufferEnd", "QA accessZBuffer", "QA accessZBufferEnd", "QA clearDrawBuffer",
    "QA clearZBuffer", "QA textureFromContext", "QA bitmapFromContext", "QA busy", "QA swapBuffers",
};

/* ---------------------------------------------------------------------- */
/* The library */

static bool device_ok(gaddr dev)
{
    if (!dev)
        return false;
    uint32_t type = rd32(dev);
    if (type == kQADeviceMemory)
        return device_format((int32_t)rd32(dev + 8)) >= 0;
    return type == kQADeviceGDevice && rd32(dev + 4);
}

static void QADeviceGetFirstEngine(ppc_cpu *c) { Q3RETURN(device_ok(Q3R(0)) ? the_engine() : 0); }
static void QADeviceGetNextEngine(ppc_cpu *c) { Q3RETURN(0); }

static void QAEngineCheckDevice(ppc_cpu *c)
{
    Q3RETURN(Q3R(0) && Q3R(0) == engine && device_ok(Q3R(1)) ? kQANoErr : kQANotSupported);
}

static void QAEngineGestalt(ppc_cpu *c)
{
    gaddr resp = Q3R(2);
    if (!Q3R(0) || Q3R(0) != engine || !resp) {
        Q3RETURN(kQAParamErr);
        return;
    }
    uint32_t px16_32 = 1u << kQAPixel_RGB16 | 1u << kQAPixel_ARGB16 | 1u << kQAPixel_RGB32 | 1u << kQAPixel_ARGB32;
    uint32_t v;
    switch (Q3R(1)) {
    case kQAGestalt_OptionalFeatures:
        v = 1u << 0 | 1u << 1 | 1u << 2 | 1u << 3 | 1u << 4 | 1u << 5 | 1u << 8 | 1u << 22 | 1u << 28 | 1u << 29;
        break;
    case kQAGestalt_FastFeatures: v = 1u << 0 | 1u << 1 | 1u << 2 | 1u << 3 | 1u << 4; break;
    case kQAGestalt_VendorID: v = 0; break;
    case kQAGestalt_EngineID: v = ENGINE_ID; break;
    case kQAGestalt_Revision: v = 0x00010600; break;
    case kQAGestalt_ASCIINameLength: v = (uint32_t)strlen(engine_name); break;
    case kQAGestalt_ASCIIName:
        gput(resp, engine_name, sizeof engine_name);
        Q3RETURN(kQANoErr);
        return;
    case kQAGestalt_TextureMemory:
    case kQAGestalt_FastTextureMemory: v = 64u << 20; break;
    case kQAGestalt_DrawContextPixelTypesAllowed:
    case kQAGestalt_DrawContextPixelTypesPreferred:
    case kQAGestalt_TexturePixelTypesAllowed:
    case kQAGestalt_TexturePixelTypesPreferred: v = px16_32; break;
    case kQAGestalt_BitmapPixelTypesAllowed:
    case kQAGestalt_BitmapPixelTypesPreferred: v = px16_32 | 1u << kQAPixel_Alpha1; break;
    case kQAGestalt_OptionalFeatures2: v = 1u << 4; break; /* kQAOptional2_SwapBuffers */
    case kQAGestalt_MultiTextureMax: v = 0; break;
    default: Q3RETURN(kQAGestaltUnknown); return;
    }
    wr32(resp, v);
    Q3RETURN(kQANoErr);
}

static void QAEngineEnable(ppc_cpu *c) { Q3RETURN(kQANoErr); }
static void QAEngineDisable(ppc_cpu *c) { Q3RETURN(kQANoErr); }

static void QADrawContextNew(ppc_cpu *c)
{
    gaddr dev = Q3R(0), rect = Q3R(1), clip = Q3R(2), out = Q3R(5);
    uint32_t flags = Q3R(4);
    if (out)
        wr32(out, 0);
    if (!device_ok(dev) || !rect || !out || Q3R(3) != engine || !engine) {
        Q3RETURN(kQAParamErr);
        return;
    }
    int slot = -1;
    for (int i = 0; i < MAX_CTX; i++)
        if (!ctxs[i]) {
            slot = i;
            break;
        }
    int left = (int32_t)rd32(rect), right = (int32_t)rd32(rect + 4), top = (int32_t)rd32(rect + 8),
        bottom = (int32_t)rd32(rect + 12);
    if (slot < 0 || right <= left || bottom <= top) {
        Q3RETURN(slot < 0 ? kQAOutOfMemory : kQAParamErr);
        return;
    }
    rctx *x = calloc(1, sizeof *x);
    if (!x) {
        Q3RETURN(kQAOutOfMemory);
        return;
    }
    x->devtype = (int)rd32(dev);
    if (x->devtype == kQADeviceMemory) {
        x->mem_row_bytes = (int32_t)rd32(dev + 4);
        x->mem_type = (int32_t)rd32(dev + 8);
        x->mem_width = (int32_t)rd32(dev + 12);
        x->mem_height = (int32_t)rd32(dev + 16);
        x->mem_base = rd32(dev + 20);
    } else {
        x->gdevice = rd32(dev + 4);
    }
    x->left = left;
    x->top = top;
    x->width = right - left;
    x->height = bottom - top;
    x->clip = clip ? rd32(clip + 4) : 0;
    x->flags = flags;
    x->frame = r3d_frame_new(x->width, x->height);
    x->pixels = calloc((size_t)x->width * (size_t)x->height, 4);
    gaddr g = tb_NewPtr(8 + 35 * 4, false, true);
    if (!x->frame || !x->pixels || !g) {
        r3d_frame_free(x->frame);
        free(x->pixels);
        free(x);
        if (g)
            tb_DisposePtr(g);
        Q3RETURN(kQAOutOfMemory);
        return;
    }
    x->guest = g;
    wr32(g, (TAG_CTX & 0xFFFF0000u) | (uint32_t)slot);
    wr32(g + 4, kQAVersion_1_6);
    for (int i = 0; i < 35; i++)
        wr32(g + 8 + 4 * (uint32_t)i, cfm_native_tvector(methods[i], method_names[i]));
    x->ints[TAG_ZFunction] = flags & kQAContext_NoZBuffer ? 0 : 1; /* kQAZFunction_LT */
    x->ints[TAG_ZBufferMask] = 1;
    x->ints[27] = 0xF; /* the channel mask: all */
    x->floats[TAG_Width] = 1;
    x->floats[TAG_ZMinScale] = 1.0f / 65535;
    x->floats[TAG_BitmapScale_x] = x->floats[TAG_BitmapScale_y] = 1;
    ctxs[slot] = x;
    wr32(out, g);
    Q3RETURN(kQANoErr);
}

static void QADrawContextDelete(ppc_cpu *c)
{
    rctx *x = ctx_of(Q3R(0));
    if (x) {
        ctxs[rd32(x->guest) & 0xFFFF] = NULL;
        r3d_frame_free(x->frame);
        free(x->pixels);
        free(x->submitted);
        wr32(x->guest, 0);
        tb_DisposePtr(x->guest);
        free(x);
    }
    Q3RETURN(0);
}

static void QARegisterDrawMethod(ppc_cpu *c)
{
    gaddr dc = Q3R(0);
    uint32_t tag = Q3R(1);
    if (!dc || tag >= 35) {
        Q3RETURN(kQAParamErr);
        return;
    }
    wr32(dc + 8 + 4 * tag, Q3R(2));
    Q3RETURN(kQANoErr);
}

static int texture_format(uint32_t rave_type)
{
    switch (rave_type) {
    case kQAPixel_ARGB16_4444: return Q3PX_ARGB16_4444;
    default: return device_format((int32_t)rave_type);
    }
}

static int new_tex_slot(void)
{
    for (int i = 1; i < MAX_TEX; i++)
        if (!texs[i])
            return i;
    return -1;
}

static int upload(rtex *t)
{
    uint32_t *px;
    if (t->alpha1) {
        px = malloc((size_t)t->width * (size_t)t->height * 4);
        for (int y = 0; px && y < t->height; y++)
            for (int x = 0; x < t->width; x++) {
                bool on = rd8(t->image + (uint32_t)y * t->row_bytes + (uint32_t)(x >> 3)) & (0x80 >> (x & 7));
                px[(size_t)y * (size_t)t->width + (size_t)x] = on ? 0xFFFFFFFF : 0x00FFFFFF;
            }
    } else {
        px = q3_image_argb(t->image, t->width, t->height, t->row_bytes, t->format,
                           (t->flags & kQATexture_FlipOrigin) != 0);
    }
    if (!px)
        return 0;
    int id = r3d_texture_new(t->width, t->height, px, !t->bitmap && (t->flags & kQATexture_Mipmap));
    free(px);
    return id;
}

static void new_image(ppc_cpu *c, bool bitmap)
{
    uint32_t flags = Q3R(1), type = Q3R(2);
    gaddr images = Q3R(3), out = Q3R(4);
    if (out)
        wr32(out, 0);
    int fmt = type == kQAPixel_Alpha1 && bitmap ? 0 : texture_format(type);
    if (Q3R(0) != engine || !engine || !images || !out || fmt < 0) {
        Q3RETURN(fmt < 0 ? kQANotSupported : kQAParamErr);
        return;
    }
    int slot = new_tex_slot();
    rtex *t = slot > 0 ? calloc(1, sizeof *t) : NULL;
    gaddr g = t ? tb_NewPtr(8, false, true) : 0;
    if (!g) {
        free(t);
        Q3RETURN(kQAOutOfMemory);
        return;
    }
    t->guest = g;
    t->bitmap = bitmap;
    t->alpha1 = bitmap && type == kQAPixel_Alpha1;
    t->format = fmt;
    t->flags = flags;
    t->width = (int32_t)rd32(images);
    t->height = (int32_t)rd32(images + 4);
    t->row_bytes = rd32(images + 8);
    t->image = rd32(images + 12);
    t->texture = upload(t);
    if (!t->texture) {
        tb_DisposePtr(g);
        free(t);
        Q3RETURN(kQAOutOfMemory);
        return;
    }
    wr32(g, bitmap ? TAG_BMP : TAG_TEX);
    wr32(g + 4, (uint32_t)slot);
    texs[slot] = t;
    wr32(out, g);
    Q3RETURN(kQANoErr);
}
static void QATextureNew(ppc_cpu *c) { new_image(c, false); }
static void QABitmapNew(ppc_cpu *c) { new_image(c, true); }

static void QATextureDetach(ppc_cpu *c)
{
    rtex *t = tex_of(Q3R(1));
    if (t && (t->flags & kQATexture_NoCopy)) {
        int id = upload(t);
        if (id) {
            r3d_texture_free(t->texture);
            t->texture = id;
        }
    }
    Q3RETURN(t ? kQANoErr : kQAParamErr);
}

static void QATextureDelete(ppc_cpu *c)
{
    rtex *t = tex_of(Q3R(1));
    if (t) {
        texs[rd32(t->guest + 4)] = NULL;
        r3d_texture_free(t->texture);
        wr32(t->guest, 0);
        tb_DisposePtr(t->guest);
        free(t);
    }
    Q3RETURN(0);
}

static void QAColorTableNew(ppc_cpu *c)
{
    if (Q3R(4))
        wr32(Q3R(4), 0);
    Q3RETURN(kQANotSupported);
}
static void QARegisterDrawNotificationProc(ppc_cpu *c)
{
    static uint32_t next = 1;
    if (Q3R(3))
        wr32(Q3R(3), next++);
    Q3RETURN(kQANoErr);
}

const ppc_export rave_exports[] = {
    { "QADeviceGetFirstEngine", QADeviceGetFirstEngine },
    { "QADeviceGetNextEngine", QADeviceGetNextEngine },
    { "QAEngineCheckDevice", QAEngineCheckDevice },
    { "QAEngineGestalt", QAEngineGestalt },
    { "QAEngineEnable", QAEngineEnable },
    { "QAEngineDisable", QAEngineDisable },
    { "QADrawContextNew", QADrawContextNew },
    { "QADrawContextDelete", QADrawContextDelete },
    { "QARegisterDrawMethod", QARegisterDrawMethod },
    { "QATextureNew", QATextureNew },
    { "QATextureDetach", QATextureDetach },
    { "QATextureDelete", QATextureDelete },
    { "QATextureBindColorTable", m_notSupported },
    { "QABitmapNew", QABitmapNew },
    { "QABitmapDetach", QATextureDetach },
    { "QABitmapDelete", QATextureDelete },
    { "QABitmapBindColorTable", m_notSupported },
    { "QAColorTableNew", QAColorTableNew },
    { "QAColorTableDelete", m_nothing },
    { "QAAccessTexture", m_notSupported },
    { "QAAccessTextureEnd", m_notSupported },
    { "QAAccessBitmap", m_notSupported },
    { "QAAccessBitmapEnd", m_notSupported },
    { "QARegisterDrawNotificationProc", QARegisterDrawNotificationProc },
    { "QAUnregisterDrawNotificationProc", m_noErr },
    /* Engines a program or driver brings (RAVESystem.h) aren't taken. */
    { "QARegisterEngine", m_notSupported },
    { "QARegisterEngineWithRefCon", m_notSupported },
    { "QAGetCurrentEngineRefCon", m_nothing },
};
const unsigned rave_export_count = sizeof rave_exports / sizeof rave_exports[0];

/* Guest memory gone (a new machine): the contexts and textures with it. */
void rave_reset(void)
{
    for (int i = 0; i < MAX_CTX; i++)
        if (ctxs[i]) {
            r3d_frame_free(ctxs[i]->frame);
            free(ctxs[i]->pixels);
            free(ctxs[i]->submitted);
            free(ctxs[i]);
            ctxs[i] = NULL;
        }
    for (int i = 0; i < MAX_TEX; i++)
        if (texs[i]) {
            r3d_texture_free(texs[i]->texture);
            free(texs[i]);
            texs[i] = NULL;
        }
    engine = 0;
}
