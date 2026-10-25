/*
 * AGL, Apple's OpenGL for Mac OS 9 (OpenGLLibrary's agl calls): pixel
 * formats, renderer information, contexts and their drawables.  A context
 * is a host one (opengl.c) rendering into a framebuffer the drawable's
 * size; aglSwapBuffers (and glFlush, single-buffered) reads the frame back
 * into a 32-bit pixel map in the system heap and copies it to the drawable
 * with CopyBits, so a window shows it as it shows anything drawn into it,
 * at whatever depth the screen has.
 *
 * Drawables: a window or other colour port (aglSetDrawable), the part of
 * it AGL_BUFFER_RECT gives (x, y from its bottom left), memory
 * (aglSetOffScreen: 32-bit pixels at the program's row bytes), or the
 * whole screen (aglSetFullScreen: a black window covering it, the frame
 * scaled to fill it as the display would have changed to its size).
 *
 * The program's objects are system-heap blocks: a tag and an index here.
 * Errors are kept for aglGetError, which clears them.  Without a host
 * OpenGL there is no OpenGLLibrary (cfm.c), and these say AGL_BAD_ALLOC.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hostgl.h"
#include "qd_internal.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; } while (0)

enum {
    AGL_NONE = 0, AGL_ALL_RENDERERS = 1, AGL_BUFFER_SIZE = 2, AGL_LEVEL = 3, AGL_RGBA = 4, AGL_DOUBLEBUFFER = 5,
    AGL_STEREO = 6, AGL_AUX_BUFFERS = 7, AGL_RED_SIZE = 8, AGL_GREEN_SIZE = 9, AGL_BLUE_SIZE = 10,
    AGL_ALPHA_SIZE = 11, AGL_DEPTH_SIZE = 12, AGL_STENCIL_SIZE = 13, AGL_ACCUM_RED_SIZE = 14,
    AGL_ACCUM_GREEN_SIZE = 15, AGL_ACCUM_BLUE_SIZE = 16, AGL_ACCUM_ALPHA_SIZE = 17,
    AGL_PIXEL_SIZE = 50, AGL_MINIMUM_POLICY = 51, AGL_MAXIMUM_POLICY = 52, AGL_OFFSCREEN = 53,
    AGL_FULLSCREEN = 54, AGL_SAMPLE_BUFFERS_ARB = 55, AGL_SAMPLES_ARB = 56, AGL_AUX_DEPTH_STENCIL = 57,
    AGL_COLOR_FLOAT = 58, AGL_MULTISAMPLE = 59, AGL_SUPERSAMPLE = 60, AGL_SAMPLE_ALPHA = 61,
    AGL_RENDERER_ID = 70, AGL_SINGLE_RENDERER = 71, AGL_NO_RECOVERY = 72, AGL_ACCELERATED = 73,
    AGL_CLOSEST_POLICY = 74, AGL_ROBUST = 75, AGL_BACKING_STORE = 76, AGL_MP_SAFE = 78, AGL_WINDOW = 80,
    AGL_MULTISCREEN = 81, AGL_VIRTUAL_SCREEN = 82, AGL_COMPLIANT = 83, AGL_PBUFFER = 90,
    /* renderer properties */
    AGL_BUFFER_MODES = 100, AGL_MIN_LEVEL = 101, AGL_MAX_LEVEL = 102, AGL_COLOR_MODES = 103,
    AGL_ACCUM_MODES = 104, AGL_DEPTH_MODES = 105, AGL_STENCIL_MODES = 106, AGL_MAX_AUX_BUFFERS = 107,
    AGL_VIDEO_MEMORY = 120, AGL_TEXTURE_MEMORY = 121, AGL_RENDERER_COUNT = 128,
    /* context options */
    AGL_SWAP_RECT = 200, AGL_BUFFER_RECT = 202, AGL_SWAP_LIMIT = 203, AGL_COLORMAP_TRACKING = 210,
    AGL_COLORMAP_ENTRY = 212, AGL_RASTERIZATION = 220, AGL_SWAP_INTERVAL = 222, AGL_STATE_VALIDATION = 230,
};
enum {
    AGL_NO_ERROR = 0, AGL_BAD_ATTRIBUTE = 10000, AGL_BAD_PROPERTY = 10001, AGL_BAD_PIXELFMT = 10002,
    AGL_BAD_RENDINFO = 10003, AGL_BAD_CONTEXT = 10004, AGL_BAD_DRAWABLE = 10005, AGL_BAD_GDEV = 10006,
    AGL_BAD_STATE = 10007, AGL_BAD_VALUE = 10008, AGL_BAD_MATCH = 10009, AGL_BAD_ENUM = 10010,
    AGL_BAD_OFFSCREEN = 10011, AGL_BAD_FULLSCREEN = 10012, AGL_BAD_WINDOW = 10013, AGL_BAD_POINTER = 10014,
    AGL_BAD_MODULE = 10015, AGL_BAD_ALLOC = 10016, AGL_BAD_CONNECTION = 10017,
};
/* Buffer, colour and depth mode bits. */
enum { AGL_SINGLEBUFFER_BIT = 4, AGL_DOUBLEBUFFER_BIT = 8, AGL_MONOSCOPIC_BIT = 1 };
enum { AGL_RGB888_BIT = 0x4000, AGL_ARGB8888_BIT = 0x8000, AGL_RGB565_BIT = 0x1000, AGL_ARGB1555_BIT = 0x400 };
enum { AGL_0_BIT = 1, AGL_8_BIT = 0x80, AGL_16_BIT = 0x400, AGL_24_BIT = 0x800, AGL_32_BIT = 0x1000 };
/* Our one renderer: accelerated, its ID among the ATI ones Mac OS 9's
 * accelerated renderers had. */
#define RENDERER_ID 0x00021000u
#define TAG_PIX 0x61676C70u  /* 'aglp' */
#define TAG_CTX 0x61676C63u  /* 'aglc' */
#define TAG_REND 0x61676C72u /* 'aglr' */

typedef struct {
    gaddr guest;
    bool dbl, fullscreen, offscreen;
    int depth, stencil, alpha;
} apix;

enum { DRAW_NONE, DRAW_PORT, DRAW_OFFSCREEN, DRAW_FULLSCREEN };

typedef struct {
    gaddr guest;
    hostgl_ctx *h;
    apix fmt;
    int kind;
    gaddr port;                     /* a window or GWorld; the cover window, full screen */
    gaddr off_base;                 /* aglSetOffScreen's memory */
    uint32_t off_row_bytes;
    int width, height;              /* the framebuffer's */
    int32_t buffer_rect[4], swap_rect[4];
    bool buffer_rect_on, swap_rect_on, rasterization;
    int32_t swap_interval;
    gaddr frame;                    /* the frame as a PixMap, its pixels after it */
    int frame_w, frame_h;
    gaddr scratch;                  /* rectangles, colours, the saved port and clip */
} actx;

enum { MAX_OBJECTS = 64 };
static apix *pixs[MAX_OBJECTS];
static actx *ctxs[MAX_OBJECTS];
static gaddr renderers[MAX_OBJECTS];
static actx *current;
static uint32_t last_error;
static gaddr error_strings[20], devices;

static void set_error(uint32_t e)
{
    if (!last_error)
        last_error = e;
}

/* A system-heap block standing for object i of a kind. */
static gaddr new_object(uint32_t tag, int i)
{
    gaddr g = tb_NewPtr(8, true, true);
    if (g) {
        wr32(g, tag);
        wr32(g + 4, (uint32_t)i);
    }
    return g;
}
static int object_index(gaddr g, uint32_t tag)
{
    if (!g || rd32(g) != tag || rd32(g + 4) >= MAX_OBJECTS)
        return -1;
    return (int)rd32(g + 4);
}
static apix *pix_of(gaddr g)
{
    int i = object_index(g, TAG_PIX);
    return i >= 0 && pixs[i] && pixs[i]->guest == g ? pixs[i] : NULL;
}
static actx *ctx_of(gaddr g)
{
    int i = object_index(g, TAG_CTX);
    return i >= 0 && ctxs[i] && ctxs[i]->guest == g ? ctxs[i] : NULL;
}

/* ---------------------------------------------------------------------- */
/* Pixel formats and renderers */

static bool attr_takes_value(uint32_t a)
{
    switch (a) {
    case AGL_BUFFER_SIZE: case AGL_LEVEL: case AGL_AUX_BUFFERS: case AGL_RED_SIZE: case AGL_GREEN_SIZE:
    case AGL_BLUE_SIZE: case AGL_ALPHA_SIZE: case AGL_DEPTH_SIZE: case AGL_STENCIL_SIZE: case AGL_ACCUM_RED_SIZE:
    case AGL_ACCUM_GREEN_SIZE: case AGL_ACCUM_BLUE_SIZE: case AGL_ACCUM_ALPHA_SIZE: case AGL_PIXEL_SIZE:
    case AGL_SAMPLE_BUFFERS_ARB: case AGL_SAMPLES_ARB: case AGL_RENDERER_ID: case AGL_VIRTUAL_SCREEN:
        return true;
    default:
        return false;
    }
}

/* aglChoosePixelFormat(gdevs, ndev, attribs) */
static void agl_ChoosePixelFormat(ppc_cpu *c)
{
    gaddr attribs = R(2);
    apix f = { .depth = 0 };
    bool ok = true;
    for (int n = 0; attribs && n < 256; n++) {
        uint32_t a = rd32(attribs + 4 * (uint32_t)n);
        if (a == AGL_NONE)
            break;
        uint32_t v = attr_takes_value(a) ? rd32(attribs + 4 * (uint32_t)++n) : 1;
        switch (a) {
        case AGL_DOUBLEBUFFER: f.dbl = true; break;
        case AGL_DEPTH_SIZE: f.depth = v ? (v > 24 ? 24 : (int)v) : 0; break;
        case AGL_STENCIL_SIZE: f.stencil = v ? 8 : 0; break;
        case AGL_ALPHA_SIZE: f.alpha = v ? 8 : 0; break;
        case AGL_FULLSCREEN: f.fullscreen = true; break;
        case AGL_OFFSCREEN: f.offscreen = true; break;
        case AGL_RENDERER_ID: ok = v == RENDERER_ID || v == 0x00020200u /* generic */; break;
        case AGL_STEREO: ok = false; break;
        }
    }
    int slot = -1;
    for (int i = 0; i < MAX_OBJECTS && slot < 0; i++)
        if (!pixs[i])
            slot = i;
    if (!ok || !hostgl_available() || slot < 0) {
        set_error(hostgl_available() ? AGL_BAD_PIXELFMT : AGL_BAD_ALLOC);
        RETURN(0);
        return;
    }
    apix *p = malloc(sizeof *p);
    *p = f;
    p->guest = new_object(TAG_PIX, slot);
    if (!p->guest) {
        free(p);
        set_error(AGL_BAD_ALLOC);
        RETURN(0);
        return;
    }
    pixs[slot] = p;
    RETURN(p->guest);
}

static void agl_DestroyPixelFormat(ppc_cpu *c)
{
    apix *p = pix_of(R(0));
    if (p) {
        pixs[object_index(R(0), TAG_PIX)] = NULL;
        tb_DisposePtr(p->guest);
        free(p);
    } else if (R(0)) {
        set_error(AGL_BAD_PIXELFMT);
    }
    RETURN(0);
}

static void agl_NextPixelFormat(ppc_cpu *c)
{
    if (!pix_of(R(0)))
        set_error(AGL_BAD_PIXELFMT);
    RETURN(0);
}

/* A pixel format's or the renderer's answer for attribute a. */
static bool describe(const apix *f, uint32_t a, int32_t *v)
{
    switch (a) {
    case AGL_RGBA: case AGL_ACCELERATED: case AGL_WINDOW: case AGL_FULLSCREEN: case AGL_OFFSCREEN:
    case AGL_COMPLIANT: case AGL_SINGLE_RENDERER: case AGL_NO_RECOVERY: case AGL_ROBUST: case AGL_MP_SAFE:
    case AGL_MULTISCREEN:
        *v = 1;
        return true;
    case AGL_DOUBLEBUFFER: *v = f && f->dbl; return true;
    case AGL_STEREO: case AGL_LEVEL: case AGL_AUX_BUFFERS: case AGL_VIRTUAL_SCREEN: case AGL_BACKING_STORE:
    case AGL_SAMPLE_BUFFERS_ARB: case AGL_SAMPLES_ARB: case AGL_ACCUM_RED_SIZE: case AGL_ACCUM_GREEN_SIZE:
    case AGL_ACCUM_BLUE_SIZE: case AGL_ACCUM_ALPHA_SIZE: case AGL_MIN_LEVEL: case AGL_MAX_LEVEL:
    case AGL_MAX_AUX_BUFFERS:
        *v = 0;
        return true;
    case AGL_RED_SIZE: case AGL_GREEN_SIZE: case AGL_BLUE_SIZE: *v = 8; return true;
    case AGL_ALPHA_SIZE: *v = f ? f->alpha : 8; return true;
    case AGL_BUFFER_SIZE: case AGL_PIXEL_SIZE: *v = 32; return true;
    case AGL_DEPTH_SIZE: *v = f ? f->depth : 24; return true;
    case AGL_STENCIL_SIZE: *v = f ? f->stencil : 8; return true;
    case AGL_RENDERER_ID: *v = (int32_t)RENDERER_ID; return true;
    case AGL_BUFFER_MODES: *v = AGL_MONOSCOPIC_BIT | AGL_SINGLEBUFFER_BIT | AGL_DOUBLEBUFFER_BIT; return true;
    case AGL_COLOR_MODES: *v = AGL_RGB888_BIT | AGL_ARGB8888_BIT | AGL_RGB565_BIT | AGL_ARGB1555_BIT; return true;
    case AGL_ACCUM_MODES: *v = AGL_0_BIT; return true;
    case AGL_DEPTH_MODES: *v = AGL_0_BIT | AGL_16_BIT | AGL_24_BIT; return true;
    case AGL_STENCIL_MODES: *v = AGL_0_BIT | AGL_8_BIT; return true;
    case AGL_VIDEO_MEMORY: *v = 32 << 20; return true;
    case AGL_TEXTURE_MEMORY: *v = 24 << 20; return true;
    case AGL_RENDERER_COUNT: *v = 1; return true;
    default: return false;
    }
}

/* aglDescribePixelFormat(pix, attrib, &value) */
static void agl_DescribePixelFormat(ppc_cpu *c)
{
    apix *f = pix_of(R(0));
    int32_t v = 0;
    if (!f) {
        set_error(AGL_BAD_PIXELFMT);
        RETURN(0);
    } else if (!describe(f, R(1), &v)) {
        set_error(AGL_BAD_ATTRIBUTE);
        RETURN(0);
    } else {
        if (R(2))
            wr32(R(2), (uint32_t)v);
        RETURN(1);
    }
}

/* aglDevicesOfPixelFormat(pix, &ndevs): the main screen's device. */
static void agl_DevicesOfPixelFormat(ppc_cpu *c)
{
    if (!pix_of(R(0))) {
        set_error(AGL_BAD_PIXELFMT);
        RETURN(0);
        return;
    }
    if (!devices)
        devices = tb_NewPtr(4, true, true);
    if (devices)
        wr32(devices, rd32(LM_MainDevice));
    if (R(1))
        wr32(R(1), 1);
    RETURN(devices);
}

/* aglQueryRendererInfo(gdevs, ndev): one renderer. */
static void agl_QueryRendererInfo(ppc_cpu *c)
{
    for (int i = 0; i < MAX_OBJECTS; i++)
        if (!renderers[i]) {
            renderers[i] = hostgl_available() ? new_object(TAG_REND, i) : 0;
            if (!renderers[i])
                set_error(AGL_BAD_ALLOC);
            RETURN(renderers[i]);
            return;
        }
    set_error(AGL_BAD_ALLOC);
    RETURN(0);
}

static bool is_renderer(gaddr g)
{
    int i = object_index(g, TAG_REND);
    return i >= 0 && renderers[i] == g;
}

static void agl_NextRendererInfo(ppc_cpu *c)
{
    if (!is_renderer(R(0)))
        set_error(AGL_BAD_RENDINFO);
    RETURN(0);
}

static void agl_DestroyRendererInfo(ppc_cpu *c)
{
    if (is_renderer(R(0))) {
        renderers[object_index(R(0), TAG_REND)] = 0;
        tb_DisposePtr(R(0));
    } else if (R(0)) {
        set_error(AGL_BAD_RENDINFO);
    }
    RETURN(0);
}

/* aglDescribeRenderer(rend, prop, &value) */
static void agl_DescribeRenderer(ppc_cpu *c)
{
    int32_t v = 0;
    if (!is_renderer(R(0))) {
        set_error(AGL_BAD_RENDINFO);
        RETURN(0);
    } else if (!describe(NULL, R(1), &v)) {
        set_error(AGL_BAD_PROPERTY);
        RETURN(0);
    } else {
        if (R(2))
            wr32(R(2), (uint32_t)v);
        RETURN(1);
    }
}

/* ---------------------------------------------------------------------- */
/* Contexts */

static void free_frame(actx *a)
{
    if (a->frame) {
        if (PixMap_pmTable(a->frame))
            tb_DisposeHandle(PixMap_pmTable(a->frame));
        tb_DisposePtr(a->frame);
        a->frame = 0;
    }
}

static void close_fullscreen(actx *a)
{
    if (a->kind == DRAW_FULLSCREEN && a->port)
        tb_DisposeWindow(a->port);
    a->kind = DRAW_NONE;
    a->port = 0;
}

/* aglCreateContext(pix, share) */
static void agl_CreateContext(ppc_cpu *c)
{
    apix *f = pix_of(R(0));
    actx *share = R(1) ? ctx_of(R(1)) : NULL;
    if (!f || (R(1) && !share)) {
        set_error(f ? AGL_BAD_CONTEXT : AGL_BAD_PIXELFMT);
        RETURN(0);
        return;
    }
    int slot = -1;
    for (int i = 0; i < MAX_OBJECTS && slot < 0; i++)
        if (!ctxs[i])
            slot = i;
    hostgl_ctx *h = slot >= 0 ? hostgl_create(share ? share->h : NULL, f->depth, f->stencil) : NULL;
    gaddr g = h ? new_object(TAG_CTX, slot) : 0;
    gaddr s = g ? tb_NewPtr(64, true, true) : 0;
    if (!s) {
        if (g)
            tb_DisposePtr(g);
        hostgl_destroy(h);
        set_error(AGL_BAD_ALLOC);
        RETURN(0);
        return;
    }
    actx *a = calloc(1, sizeof *a);
    a->guest = g;
    a->h = h;
    a->fmt = *f;
    a->scratch = s;
    a->rasterization = true;
    a->swap_interval = 0;
    ctxs[slot] = a;
    RETURN(g);
}

static void destroy(actx *a)
{
    if (current == a) {
        current = NULL;
        hostgl_set_current(NULL);
    }
    ctxs[object_index(a->guest, TAG_CTX)] = NULL;
    close_fullscreen(a);
    hostgl_destroy(a->h);
    free_frame(a);
    tb_DisposePtr(a->scratch);
    tb_DisposePtr(a->guest);
    free(a);
}

static void agl_DestroyContext(ppc_cpu *c)
{
    actx *a = ctx_of(R(0));
    if (!a) {
        set_error(AGL_BAD_CONTEXT);
        RETURN(0);
        return;
    }
    destroy(a);
    RETURN(1);
}

/* aglCopyContext(src, dst, mask) */
static void agl_CopyContext(ppc_cpu *c)
{
    actx *s = ctx_of(R(0)), *d = ctx_of(R(1));
    if (!s || !d) {
        set_error(AGL_BAD_CONTEXT);
        RETURN(0);
        return;
    }
    hostgl_copy_state(s->h, d->h, R(2));
    if (current)
        hostgl_set_current(current->h);
    RETURN(1);
}

static void agl_SetCurrentContext(ppc_cpu *c)
{
    actx *a = R(0) ? ctx_of(R(0)) : NULL;
    if (R(0) && !a) {
        set_error(AGL_BAD_CONTEXT);
        RETURN(0);
        return;
    }
    current = a;
    hostgl_set_current(a ? a->h : NULL);
    RETURN(1);
}

static void agl_GetCurrentContext(ppc_cpu *c) { RETURN(current ? current->guest : 0); }

/* The framebuffer's size: the drawable's, or the part of it the buffer
 * rectangle gives. */
static void size_to_drawable(actx *a)
{
    int w = a->width, h = a->height;
    if (a->kind == DRAW_PORT) {
        qrect pr = qrect_get(a->port + GrafPort_portRect_off);
        w = pr.right - pr.left;
        h = pr.bottom - pr.top;
    }
    if (a->kind == DRAW_PORT && a->buffer_rect_on) {
        w = a->buffer_rect[2];
        h = a->buffer_rect[3];
    }
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    if (w > 4000)
        w = 4000;
    if (h > 4000)
        h = 4000;
    a->width = w;
    a->height = h;
    hostgl_resize(a->h, w, h);
}

/* aglSetDrawable(ctx, port) */
static void agl_SetDrawable(ppc_cpu *c)
{
    actx *a = ctx_of(R(0));
    if (!a) {
        set_error(AGL_BAD_CONTEXT);
        RETURN(0);
        return;
    }
    close_fullscreen(a);
    if (R(1)) {
        a->kind = DRAW_PORT;
        a->port = R(1);
        size_to_drawable(a);
    }
    RETURN(1);
}

static void agl_GetDrawable(ppc_cpu *c)
{
    actx *a = ctx_of(R(0));
    if (!a)
        set_error(AGL_BAD_CONTEXT);
    RETURN(a && a->kind == DRAW_PORT ? a->port : 0);
}

/* aglSetOffScreen(ctx, width, height, rowbytes, baseaddr) */
static void agl_SetOffScreen(ppc_cpu *c)
{
    actx *a = ctx_of(R(0));
    if (!a || !R(4) || (int32_t)R(1) <= 0 || (int32_t)R(2) <= 0 || R(3) < R(1) * 4) {
        set_error(a ? AGL_BAD_OFFSCREEN : AGL_BAD_CONTEXT);
        RETURN(0);
        return;
    }
    close_fullscreen(a);
    a->kind = DRAW_OFFSCREEN;
    a->off_base = R(4);
    a->off_row_bytes = R(3);
    a->width = (int)R(1);
    a->height = (int)R(2);
    size_to_drawable(a);
    RETURN(1);
}

/* aglSetFullScreen(ctx, width, height, freq, device): a black window over
 * the whole screen. */
static void agl_SetFullScreen(ppc_cpu *c)
{
    actx *a = ctx_of(R(0));
    if (!a) {
        set_error(AGL_BAD_CONTEXT);
        RETURN(0);
        return;
    }
    close_fullscreen(a);
    gaddr r = a->scratch;
    qrect_put(r, (qrect){ 0, 0, (int16_t)g_screen_height, (int16_t)g_screen_width });
    wr16(r + 8, 0); /* an empty title */
    gaddr w = tb_NewCWindow(0, r, r + 8, true, 2 /* plainDBox */, 0xFFFFFFFFu, false, 0);
    if (!w) {
        set_error(AGL_BAD_FULLSCREEN);
        RETURN(0);
        return;
    }
    gaddr saved = r + 16, black = r + 24;
    tb_GetPort(saved);
    tb_SetPort(w);
    memset(gptr(black), 0, 6);
    tb_RGBForeColor(black);
    tb_PaintRect(r);
    tb_SetPort(rd32(saved));
    a->kind = DRAW_FULLSCREEN;
    a->port = w;
    a->width = (int32_t)R(1) > 0 ? (int)R(1) : g_screen_width;
    a->height = (int32_t)R(2) > 0 ? (int)R(2) : g_screen_height;
    size_to_drawable(a);
    RETURN(1);
}

static void agl_UpdateContext(ppc_cpu *c)
{
    actx *a = ctx_of(R(0));
    if (!a) {
        set_error(AGL_BAD_CONTEXT);
        RETURN(0);
        return;
    }
    size_to_drawable(a);
    RETURN(1);
}

static void agl_SetVirtualScreen(ppc_cpu *c) { RETURN(ctx_of(R(0)) && R(1) == 0); }
static void agl_GetVirtualScreen(ppc_cpu *c) { RETURN(0); }

/* ---------------------------------------------------------------------- */
/* Showing frames */

/* The frame pixel map: 32-bit, the framebuffer's size, its pixels after
 * the record. */
static bool frame_for(actx *a)
{
    if (a->frame && a->frame_w == a->width && a->frame_h == a->height)
        return true;
    free_frame(a);
    uint32_t rb = (uint32_t)a->width * 4;
    gaddr f = tb_NewPtr((int32_t)(64 + rb * (uint32_t)a->height), true, false);
    if (!f)
        f = tb_NewPtr((int32_t)(64 + rb * (uint32_t)a->height), false, false);
    if (!f)
        return false;
    gmemset(f, 0, 64);
    PixMap_set_baseAddr(f, f + 64);
    PixMap_set_rowBytes(f, (int16_t)(0x8000 | rb));
    qrect_put(f + PixMap_bounds_off, (qrect){ 0, 0, (int16_t)a->height, (int16_t)a->width });
    PixMap_set_hRes(f, 72 << 16);
    PixMap_set_vRes(f, 72 << 16);
    PixMap_set_pixelType(f, 16); /* RGBDirect */
    PixMap_set_pixelSize(f, 32);
    PixMap_set_cmpCount(f, 3);
    PixMap_set_cmpSize(f, 8);
    PixMap_set_pmTable(f, tb_GetCTable(8));
    a->frame = f;
    a->frame_w = a->width;
    a->frame_h = a->height;
    return true;
}

/* The frame (or the swap rectangle of it) to the drawable. */
static void present(actx *a)
{
    if (!a || a->kind == DRAW_NONE)
        return;
    int x = 0, y = 0, w = a->width, h = a->height;
    if (a->swap_rect_on) {
        int x1 = a->swap_rect[0] + a->swap_rect[2], y1 = a->swap_rect[1] + a->swap_rect[3];
        x = a->swap_rect[0] > 0 ? a->swap_rect[0] : 0;
        y = a->swap_rect[1] > 0 ? a->swap_rect[1] : 0;
        w = (x1 < a->width ? x1 : a->width) - x;
        h = (y1 < a->height ? y1 : a->height) - y;
        if (w <= 0 || h <= 0)
            return;
    }
    /* Rows from the top: the rectangle's top is height - (y + h). */
    int top = a->height - y - h;
    if (a->kind == DRAW_OFFSCREEN) {
        hostgl_read_frame(a->h, x, y, w, h, a->off_base + (uint32_t)top * a->off_row_bytes + (uint32_t)x * 4,
                          a->off_row_bytes);
        return;
    }
    if (!a->port || !frame_for(a))
        return;
    uint32_t rb = (uint32_t)a->width * 4;
    hostgl_read_frame(a->h, x, y, w, h, PixMap_baseAddr(a->frame) + (uint32_t)top * rb + (uint32_t)x * 4, rb);

    gaddr src = a->scratch, dst = a->scratch + 8, saved = a->scratch + 16, fg = a->scratch + 24,
          bg = a->scratch + 30, black = a->scratch + 36, white = a->scratch + 42;
    qrect pr = qrect_get(a->port + GrafPort_portRect_off), d;
    qrect s = { (int16_t)top, (int16_t)x, (int16_t)(top + h), (int16_t)(x + w) };
    if (a->kind == DRAW_FULLSCREEN) {
        /* The whole frame, scaled to fill the screen as the display would
         * have taken its size; the swap rectangle is moot. */
        int sw = pr.right - pr.left, sh = pr.bottom - pr.top;
        int dw = sw, dh = (int)((long)sw * a->height / a->width);
        if (dh > sh) {
            dh = sh;
            dw = (int)((long)sh * a->width / a->height);
        }
        s = (qrect){ 0, 0, (int16_t)a->height, (int16_t)a->width };
        d = (qrect){ (int16_t)(pr.top + (sh - dh) / 2), (int16_t)(pr.left + (sw - dw) / 2), 0, 0 };
        d.bottom = (int16_t)(d.top + dh);
        d.right = (int16_t)(d.left + dw);
    } else {
        int bx = a->buffer_rect_on ? a->buffer_rect[0] : 0, by = a->buffer_rect_on ? a->buffer_rect[1] : 0;
        int16_t ftop = (int16_t)(a->buffer_rect_on ? pr.bottom - by - a->height : pr.top);
        d = (qrect){ (int16_t)(ftop + s.top), (int16_t)(pr.left + bx + s.left), (int16_t)(ftop + s.bottom),
                     (int16_t)(pr.left + bx + s.right) };
    }
    qrect_put(src, s);
    qrect_put(dst, d);

    /* Into the port as it is but for its colours and clipping, which
     * aren't the frame's business. */
    tb_GetGWorld(saved, saved + 4);
    tb_SetGWorld(a->port, 0);
    tb_GetForeColor(fg);
    tb_GetBackColor(bg);
    memset(gptr(black), 0, 6);
    memset(gptr(white), 0xFF, 6);
    tb_RGBForeColor(black);
    tb_RGBBackColor(white);
    gaddr clip = tb_NewRgn();
    if (clip) {
        tb_GetClip(clip);
        tb_ClipRect(a->port + GrafPort_portRect_off);
    }
    tb_CopyBits(a->frame, a->port + CGrafPort_portPixMap_off, src, dst, 0 /* srcCopy */, 0);
    if (clip) {
        tb_SetClip(clip);
        tb_DisposeRgn(clip);
    }
    tb_RGBForeColor(fg);
    tb_RGBBackColor(bg);
    tb_SetGWorld(rd32(saved), rd32(saved + 4));
}

static void agl_SwapBuffers(ppc_cpu *c)
{
    actx *a = ctx_of(R(0));
    if (!a) {
        set_error(AGL_BAD_CONTEXT);
        RETURN(0);
        return;
    }
    present(a);
    RETURN(0);
}

void agl_flushed(bool front)
{
    if (current && (!current->fmt.dbl || front))
        present(current);
}

/* ---------------------------------------------------------------------- */
/* Options */

static bool *option(actx *a, uint32_t pname)
{
    switch (pname) {
    case AGL_SWAP_RECT: return &a->swap_rect_on;
    case AGL_BUFFER_RECT: return &a->buffer_rect_on;
    case AGL_RASTERIZATION: return &a->rasterization;
    default: return NULL;
    }
}

static void enable(ppc_cpu *c, bool on)
{
    actx *a = ctx_of(R(0));
    if (!a) {
        set_error(AGL_BAD_CONTEXT);
        RETURN(0);
        return;
    }
    bool *o = option(a, R(1));
    if (o) {
        *o = on;
        if (R(1) == AGL_BUFFER_RECT)
            size_to_drawable(a);
    } else if (R(1) != AGL_COLORMAP_TRACKING && R(1) != AGL_STATE_VALIDATION) {
        set_error(AGL_BAD_ENUM);
        RETURN(0);
        return;
    }
    RETURN(1);
}
static void agl_Enable(ppc_cpu *c) { enable(c, true); }
static void agl_Disable(ppc_cpu *c) { enable(c, false); }

static void agl_IsEnabled(ppc_cpu *c)
{
    actx *a = ctx_of(R(0));
    bool *o = a ? option(a, R(1)) : NULL;
    if (!a)
        set_error(AGL_BAD_CONTEXT);
    RETURN(o && *o);
}

/* aglSetInteger(ctx, pname, params) */
static void agl_SetInteger(ppc_cpu *c)
{
    actx *a = ctx_of(R(0));
    gaddr p = R(2);
    if (!a) {
        set_error(AGL_BAD_CONTEXT);
        RETURN(0);
        return;
    }
    switch (R(1)) {
    case AGL_SWAP_RECT:
    case AGL_BUFFER_RECT: {
        int32_t *r = R(1) == AGL_SWAP_RECT ? a->swap_rect : a->buffer_rect;
        for (int k = 0; p && k < 4; k++)
            r[k] = (int32_t)rd32(p + 4 * (uint32_t)k);
        if (R(1) == AGL_BUFFER_RECT)
            size_to_drawable(a);
        break;
    }
    case AGL_SWAP_INTERVAL:
        if (p)
            a->swap_interval = (int32_t)rd32(p);
        break;
    case AGL_SWAP_LIMIT: case AGL_COLORMAP_ENTRY:
        break;
    default:
        set_error(AGL_BAD_ENUM);
        RETURN(0);
        return;
    }
    RETURN(1);
}

static void agl_GetInteger(ppc_cpu *c)
{
    actx *a = ctx_of(R(0));
    gaddr p = R(2);
    if (!a || !p) {
        set_error(a ? AGL_BAD_POINTER : AGL_BAD_CONTEXT);
        RETURN(0);
        return;
    }
    switch (R(1)) {
    case AGL_SWAP_RECT:
    case AGL_BUFFER_RECT: {
        int32_t *r = R(1) == AGL_SWAP_RECT ? a->swap_rect : a->buffer_rect;
        for (int k = 0; k < 4; k++)
            wr32(p + 4 * (uint32_t)k, (uint32_t)r[k]);
        break;
    }
    case AGL_SWAP_INTERVAL: wr32(p, (uint32_t)a->swap_interval); break;
    case AGL_SWAP_LIMIT: wr32(p, 0); break;
    default:
        set_error(AGL_BAD_ENUM);
        RETURN(0);
        return;
    }
    RETURN(1);
}

/* aglUseFont(ctx, fontID, face, size, first, count, base): each character
 * drawn by QuickDraw into a 1-bit world and made a bitmap display list. */
static void agl_UseFont(ppc_cpu *c)
{
    actx *a = ctx_of(R(0));
    int32_t first = (int32_t)R(4), count = (int32_t)R(5);
    uint32_t base = R(6);
    if (!a || a != current || count < 0 || first < 0 || first + count > 256) {
        set_error(a ? AGL_BAD_VALUE : AGL_BAD_CONTEXT);
        RETURN(0);
        return;
    }
    gaddr t = a->scratch, saved = t + 16, info = t + 24, gwp = t + 32;
    tb_GetGWorld(saved, saved + 4);
    int size = (int16_t)R(3) > 0 ? (int16_t)R(3) : 12;
    int w = size * 3, h = size * 3;
    qrect_put(t, (qrect){ 0, 0, (int16_t)h, (int16_t)w });
    if (tb_NewGWorld(gwp, 1, t, 0, 0, 0) != 0) {
        set_error(AGL_BAD_ALLOC);
        RETURN(0);
        return;
    }
    gaddr gw = rd32(gwp);
    tb_SetGWorld(gw, 0);
    tb_TextFont((int16_t)R(1));
    tb_TextFace((int16_t)R(2));
    tb_TextSize((int16_t)size);
    tb_GetFontInfo(info);
    int ascent = (int16_t)rd16(info), descent = (int16_t)rd16(info + 2);
    int left = size / 2; /* room for kerned and italic glyphs */
    gaddr pm = rd32(rd32(gw + CGrafPort_portPixMap_off));
    gaddr bits = qd_pixmap_base(pm, true);
    uint32_t rb = (uint32_t)(PixMap_rowBytes(pm) & 0x3FFF);
    int gh = ascent + descent;
    if (gh > h)
        gh = h;
    for (int32_t ch = first; ch < first + count; ch++) {
        tb_EraseRect(t);
        tb_MoveTo((int16_t)left, (int16_t)ascent);
        tb_DrawChar((int16_t)ch);
        int adv = tb_CharWidth((int16_t)ch);
        hostgl_glyph_list(base + (uint32_t)(ch - first), w, gh, (int)rb, (float)left, (float)descent, (float)adv,
                          gptr(bits));
    }
    tb_SetGWorld(rd32(saved), rd32(saved + 4));
    tb_DisposeGWorld(gw);
    RETURN(1);
}

/* ---------------------------------------------------------------------- */
/* The library */

static void agl_GetVersion(ppc_cpu *c)
{
    if (R(0))
        wr32(R(0), 2);
    if (R(1))
        wr32(R(1), 0);
    RETURN(0);
}

static void agl_Configure(ppc_cpu *c) { RETURN(1); }

static void agl_GetError(ppc_cpu *c)
{
    uint32_t e = last_error;
    last_error = 0;
    RETURN(e);
}

static void agl_ErrorString(ppc_cpu *c)
{
    static const char *const names[] = {
        "invalid pixel format attribute", "invalid renderer property", "invalid pixel format",
        "invalid renderer info", "invalid context", "invalid drawable", "invalid graphics device",
        "invalid context state", "invalid numerical value", "invalid share context", "invalid enumerant",
        "invalid offscreen drawable", "invalid offscreen drawable", "invalid window", "invalid pointer",
        "invalid code module", "memory allocation failure", "invalid CoreGraphics connection",
    };
    uint32_t e = R(0);
    unsigned k = e == AGL_NO_ERROR ? 18 : e >= AGL_BAD_ATTRIBUTE && e <= AGL_BAD_CONNECTION ? e - AGL_BAD_ATTRIBUTE : 19;
    const char *s = k == 18 ? "no error" : k == 19 ? "unknown error" : names[k];
    if (!error_strings[k]) {
        size_t n = strlen(s) + 1;
        if ((error_strings[k] = tb_NewPtr((int32_t)n, true, false)) != 0)
            gput(error_strings[k], s, (uint32_t)n);
    }
    RETURN(error_strings[k]);
}

static void destroy_all(void)
{
    for (int i = 0; i < MAX_OBJECTS; i++) {
        if (ctxs[i])
            destroy(ctxs[i]);
        if (pixs[i]) {
            tb_DisposePtr(pixs[i]->guest);
            free(pixs[i]);
            pixs[i] = NULL;
        }
        if (renderers[i]) {
            tb_DisposePtr(renderers[i]);
            renderers[i] = 0;
        }
    }
}

static void agl_ResetLibrary(ppc_cpu *c)
{
    destroy_all();
    last_error = 0;
    RETURN(0);
}

/* Guest memory is already gone: only the host's side to let go of. */
void agl_reset(void)
{
    for (int i = 0; i < MAX_OBJECTS; i++) {
        if (ctxs[i]) {
            hostgl_destroy(ctxs[i]->h);
            free(ctxs[i]);
            ctxs[i] = NULL;
        }
        free(pixs[i]);
        pixs[i] = NULL;
        renderers[i] = 0;
    }
    current = NULL;
    last_error = 0;
    devices = 0;
    memset(error_strings, 0, sizeof error_strings);
    hostgl_reset();
}

const ppc_export agl_exports[] = {
    { "aglChoosePixelFormat", agl_ChoosePixelFormat },
    { "aglDestroyPixelFormat", agl_DestroyPixelFormat },
    { "aglNextPixelFormat", agl_NextPixelFormat },
    { "aglDescribePixelFormat", agl_DescribePixelFormat },
    { "aglDevicesOfPixelFormat", agl_DevicesOfPixelFormat },
    { "aglQueryRendererInfo", agl_QueryRendererInfo },
    { "aglDestroyRendererInfo", agl_DestroyRendererInfo },
    { "aglNextRendererInfo", agl_NextRendererInfo },
    { "aglDescribeRenderer", agl_DescribeRenderer },
    { "aglCreateContext", agl_CreateContext },
    { "aglDestroyContext", agl_DestroyContext },
    { "aglCopyContext", agl_CopyContext },
    { "aglUpdateContext", agl_UpdateContext },
    { "aglSetCurrentContext", agl_SetCurrentContext },
    { "aglGetCurrentContext", agl_GetCurrentContext },
    { "aglSetDrawable", agl_SetDrawable },
    { "aglSetOffScreen", agl_SetOffScreen },
    { "aglSetFullScreen", agl_SetFullScreen },
    { "aglGetDrawable", agl_GetDrawable },
    { "aglSetVirtualScreen", agl_SetVirtualScreen },
    { "aglGetVirtualScreen", agl_GetVirtualScreen },
    { "aglGetVersion", agl_GetVersion },
    { "aglConfigure", agl_Configure },
    { "aglSwapBuffers", agl_SwapBuffers },
    { "aglEnable", agl_Enable },
    { "aglDisable", agl_Disable },
    { "aglIsEnabled", agl_IsEnabled },
    { "aglSetInteger", agl_SetInteger },
    { "aglGetInteger", agl_GetInteger },
    { "aglUseFont", agl_UseFont },
    { "aglGetError", agl_GetError },
    { "aglErrorString", agl_ErrorString },
    { "aglResetLibrary", agl_ResetLibrary },
};
const unsigned agl_export_count = sizeof agl_exports / sizeof agl_exports[0];
