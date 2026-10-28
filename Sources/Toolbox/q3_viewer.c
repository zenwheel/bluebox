/*
 * The QuickDraw 3D Viewer (QD3DViewerLib, QD3DViewer.h): a model shown in
 * a rectangle of a port, with a controller strip below it whose buttons
 * choose how a drag moves the camera (truck, orbit, zoom, dolly), pick a
 * camera view and reset it.
 *
 * A viewer is QuickDraw 3D's own objects driven through its calls: a
 * view, a Macintosh draw context on the viewer's port whose pane is the
 * content rectangle, a view angle aspect camera fitted to the model's
 * bounding box, an ambient and a directional light following the camera,
 * and the model, a group.  Its handle is a word of guest memory naming
 * the host record (q3_handle_new).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "filemgr_internal.h"
#include "q3_view.h"
#include "tb_internal.h"

enum {
    kQ3ViewerShowBadge = 1 << 0, kQ3ViewerActive = 1 << 1, kQ3ViewerControllerVisible = 1 << 2,
    kQ3ViewerDrawFrame = 1 << 3, kQ3ViewerDraggingOff = 1 << 4, kQ3ViewerButtonCamera = 1 << 5,
    kQ3ViewerButtonTruck = 1 << 6, kQ3ViewerButtonOrbit = 1 << 7, kQ3ViewerButtonZoom = 1 << 8,
    kQ3ViewerButtonDolly = 1 << 9, kQ3ViewerButtonReset = 1 << 10, kQ3ViewerButtonOptions = 1 << 17,
    kQ3ViewerDefault = (int)(1u << 31),
};
enum { kQ3ViewerEmpty = 0, kQ3ViewerHasModel = 1, kQ3ViewerHasUndo = 2 };
enum {
    kQ3ViewerCameraRestore, kQ3ViewerCameraFit, kQ3ViewerCameraFront, kQ3ViewerCameraBack, kQ3ViewerCameraLeft,
    kQ3ViewerCameraRight, kQ3ViewerCameraTop, kQ3ViewerCameraBottom,
};
enum { kEofErr = -39 };
enum { mouseDown = 1, mouseUp = 2, keyDown = 3, autoKey = 5, updateEvt = 6, activateEvt = 8 };

#define VIEWER_KIND 0x5156 /* handles of our own kind */
#define STRIP 20           /* the controller strip's height */
#define BUTTON 24          /* a button's width */
#define DEFAULT_FLAGS                                                                                                \
    (kQ3ViewerActive | kQ3ViewerControllerVisible | kQ3ViewerDrawFrame | kQ3ViewerButtonCamera |                     \
     kQ3ViewerButtonTruck | kQ3ViewerButtonOrbit | kQ3ViewerButtonDolly | kQ3ViewerButtonReset)

typedef struct {
    float loc[3], poi[3], up[3], fov, hither, yon;
} camstate;

typedef struct {
    gaddr g;
    gaddr port;
    qrect bounds;
    uint32_t flags, button;
    gaddr view, dctx, camera, lights, ambient, directional, illum, group;
    gaddr scratch; /* guest structures handed to QuickDraw 3D's calls */
    float bg[4];   /* a, r, g, b */
    camstate cam, home, undo;
    bool has_undo;
    float center[3], radius;
    float brightness;
    bool remove_backfaces, phong;
    uint32_t renderer;
    /* Tracking. */
    bool tracking;
    int lastx, lasty;
    int camera_view;
    gaddr draw_cb, draw_data, resize_cb, resize_data, pane_cb, pane_data;
} viewer;

/* ---------------------------------------------------------------------- */
/* Calling QuickDraw 3D's own exports */

static uint32_t q3call(const char *name, int n, const uint32_t *args, int nf, const float *f)
{
    void (*fn)(ppc_cpu *) = cfm_export_fn(name);
    if (!fn)
        return 0;
    ppc_cpu cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.gpr[1] = g_ppc.gpr[1];
    for (int i = 0; i < n && i < 8; i++)
        cpu.gpr[3 + i] = args[i];
    for (int i = 0; i < nf; i++)
        ppc_set_fpr(&cpu, 1 + i, f[i]);
    fn(&cpu);
    return cpu.gpr[3];
}
#define CALL(name, ...) q3call(name, (int)(sizeof((uint32_t[]){ __VA_ARGS__ }) / 4), (uint32_t[]){ __VA_ARGS__ }, 0, NULL)
#define CALL0(name) q3call(name, 0, NULL, 0, NULL)

static void put_floats(gaddr p, const float *f, int n)
{
    for (int i = 0; i < n; i++)
        wrf(p + 4 * (uint32_t)i, f[i]);
}

static viewer *viewer_of(gaddr g) { return q3_handle_get(g, VIEWER_KIND); }

/* ---------------------------------------------------------------------- */
/* Geometry of the viewer */

static qrect content_rect(const viewer *v)
{
    qrect r = v->bounds;
    if (v->flags & kQ3ViewerControllerVisible)
        r.bottom = (int16_t)(r.bottom - STRIP);
    if (v->flags & kQ3ViewerDrawFrame) {
        r.left++, r.top++, r.right--;
        if (!(v->flags & kQ3ViewerControllerVisible))
            r.bottom--;
    }
    if (r.bottom < r.top)
        r.bottom = r.top;
    return r;
}

static qrect strip_rect(const viewer *v)
{
    qrect r = v->bounds;
    r.top = (int16_t)(r.bottom - STRIP);
    return r;
}

static const uint32_t button_order[] = { kQ3ViewerButtonCamera, kQ3ViewerButtonTruck, kQ3ViewerButtonOrbit,
                                         kQ3ViewerButtonZoom,   kQ3ViewerButtonDolly, kQ3ViewerButtonReset };

static bool button_rect(const viewer *v, uint32_t button, qrect *out)
{
    if (!(v->flags & kQ3ViewerControllerVisible) || !(v->flags & button))
        return false;
    qrect s = strip_rect(v);
    int n = 0, at = -1;
    for (int i = 0; i < 6; i++)
        if (v->flags & button_order[i]) {
            if (button_order[i] == button)
                at = n;
            n++;
        }
    if (at < 0)
        return false;
    int left = s.left + ((s.right - s.left) - n * BUTTON) / 2 + at * BUTTON;
    *out = (qrect){ (int16_t)(s.top + 2), (int16_t)left, (int16_t)(s.bottom - 2), (int16_t)(left + BUTTON - 2) };
    return true;
}

/* ---------------------------------------------------------------------- */
/* The camera */

static void norm(float *a)
{
    float l = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    if (l > 1e-20f)
        a[0] /= l, a[1] /= l, a[2] /= l;
}
static void cross(const float *a, const float *b, float *o)
{
    float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x, o[1] = y, o[2] = z;
}
static float dist(const float *a, const float *b)
{
    float d[3] = { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
    return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

static float aspect_of(const viewer *v)
{
    qrect c = content_rect(v);
    int w = c.right - c.left, h = c.bottom - c.top;
    return w > 0 && h > 0 ? (float)w / (float)h : 1;
}

/* Hither and yon around the model from where the camera is. */
static void fit_range(viewer *v)
{
    float d = dist(v->cam.loc, v->center), r = v->radius > 0 ? v->radius : 1;
    v->cam.hither = fmaxf(d - 1.5f * r, d * 0.01f);
    if (v->cam.hither <= 0)
        v->cam.hither = 0.01f;
    v->cam.yon = d + 2 * r;
}

/* Look at the model's centre from direction dir (towards the camera),
 * far enough that it fills the view. */
static void fit_camera(viewer *v, const float dir[3], const float up[3])
{
    float r = v->radius > 0 ? v->radius : 1;
    float fov = 0.8f, half = fov / 2;
    if (aspect_of(v) < 1) /* the field of view is across the narrower side */
        half = fov / 2;
    float d = r / sinf(half) * 1.05f;
    float n[3] = { dir[0], dir[1], dir[2] };
    norm(n);
    for (int k = 0; k < 3; k++) {
        v->cam.loc[k] = v->center[k] + n[k] * d;
        v->cam.poi[k] = v->center[k];
        v->cam.up[k] = up[k];
    }
    v->cam.fov = fov;
    fit_range(v);
}

/* The model's bounding box, through the view's bounding loop. */
static void measure(viewer *v)
{
    v->center[0] = v->center[1] = v->center[2] = 0;
    v->radius = 1;
    if (!v->group)
        return;
    gaddr bb = v->scratch;
    if (CALL("Q3View_StartBoundingBox", v->view, 1) != kQ3Success)
        return;
    CALL("Q3Object_Submit", v->group, v->view);
    CALL("Q3View_EndBoundingBox", v->view, bb);
    if (rd32(bb + 24))
        return;
    float mn[3], mx[3];
    for (int k = 0; k < 3; k++) {
        mn[k] = rdf(bb + 4 * (uint32_t)k);
        mx[k] = rdf(bb + 12 + 4 * (uint32_t)k);
        v->center[k] = (mn[k] + mx[k]) / 2;
    }
    v->radius = dist(mn, mx) / 2;
    if (v->radius <= 0)
        v->radius = 1;
}

static void set_view_direction(viewer *v, int which)
{
    static const float dirs[8][6] = {
        { 0, 0, 1, 0, 1, 0 },  { 0, 0, 1, 0, 1, 0 },  { 0, 0, 1, 0, 1, 0 }, { 0, 0, -1, 0, 1, 0 },
        { -1, 0, 0, 0, 1, 0 }, { 1, 0, 0, 0, 1, 0 },  { 0, 1, 0, 0, 0, -1 }, { 0, -1, 0, 0, 0, 1 },
    };
    if (which == kQ3ViewerCameraRestore) {
        v->cam = v->home;
        return;
    }
    if (which == kQ3ViewerCameraFit) { /* as it looks now, refitted */
        float dir[3] = { v->cam.loc[0] - v->cam.poi[0], v->cam.loc[1] - v->cam.poi[1], v->cam.loc[2] - v->cam.poi[2] };
        float up[3] = { v->cam.up[0], v->cam.up[1], v->cam.up[2] };
        fit_camera(v, dir, up);
        return;
    }
    if (which < 0 || which > 7)
        return;
    fit_camera(v, dirs[which], dirs[which] + 3);
}

static void save_undo(viewer *v)
{
    v->undo = v->cam;
    v->has_undo = true;
}

/* The camera, lights and draw context brought up to date. */
static void sync(viewer *v)
{
    gaddr s = v->scratch;
    float cam[17] = { v->cam.loc[0], v->cam.loc[1], v->cam.loc[2], v->cam.poi[0], v->cam.poi[1], v->cam.poi[2],
                      v->cam.up[0],  v->cam.up[1],  v->cam.up[2],  v->cam.hither, v->cam.yon, -1, 1, 2, 2,
                      v->cam.fov,    aspect_of(v) };
    put_floats(s, cam, 17);
    CALL("Q3ViewAngleAspectCamera_SetData", v->camera, s);
    /* The light from over the viewer's left shoulder. */
    float fwd[3] = { v->cam.poi[0] - v->cam.loc[0], v->cam.poi[1] - v->cam.loc[1], v->cam.poi[2] - v->cam.loc[2] };
    float up[3] = { v->cam.up[0], v->cam.up[1], v->cam.up[2] }, right[3];
    norm(fwd);
    cross(fwd, up, right);
    norm(right);
    cross(right, fwd, up);
    float dir[3];
    for (int k = 0; k < 3; k++)
        dir[k] = fwd[k] + 0.5f * right[k] - 0.5f * up[k];
    put_floats(s, dir, 3);
    CALL("Q3DirectionalLight_SetDirection", v->directional, s);
    float b = v->brightness;
    q3call("Q3Light_SetBrightness", 1, &v->directional, 1, &b);
    float a = 0.4f * v->brightness;
    q3call("Q3Light_SetBrightness", 1, &v->ambient, 1, &a);
    /* The pane: the content rectangle, in the port's local coordinates
     * from its top left. */
    qrect c = content_rect(v), pr = v->port ? qrect_get(v->port + GrafPort_portRect_off) : (qrect){ 0, 0, 0, 0 };
    float pane[4] = { (float)(c.left - pr.left), (float)(c.top - pr.top), (float)(c.right - pr.left),
                      (float)(c.bottom - pr.top) };
    put_floats(s, pane, 4);
    CALL("Q3DrawContext_SetPane", v->dctx, s);
    CALL("Q3DrawContext_SetPaneState", v->dctx, 1);
    put_floats(s, v->bg, 4);
    CALL("Q3DrawContext_SetClearImageColor", v->dctx, s);
    CALL("Q3MacDrawContext_SetWindow", v->dctx, v->port);
    CALL("Q3MacDrawContext_SetGrafPort", v->dctx, v->port);
}

/* ---------------------------------------------------------------------- */
/* Drawing */

static void rect_put(gaddr p, qrect r) { qrect_put(p, r); }

static void fore(gaddr scratch, uint16_t r, uint16_t g, uint16_t b)
{
    wr16(scratch, r);
    wr16(scratch + 2, g);
    wr16(scratch + 4, b);
    tb_RGBForeColor(scratch);
}

static void draw_content(viewer *v)
{
    qrect c = content_rect(v);
    if (!v->port || c.right <= c.left || c.bottom <= c.top)
        return;
    sync(v);
    for (int pass = 0; pass < 4; pass++) {
        if (CALL("Q3View_StartRendering", v->view) != kQ3Success)
            return;
        CALL("Q3Object_Submit", v->illum, v->view);
        if (v->remove_backfaces)
            CALL("Q3BackfacingStyle_Submit", 1, v->view);
        if (v->group)
            CALL("Q3Object_Submit", v->group, v->view);
        if (CALL("Q3View_EndRendering", v->view) != 1) /* kQ3ViewStatusRetraverse */
            break;
    }
}

/* A button's picture: a few lines suggesting what it does. */
static void draw_glyph(uint32_t button, qrect r)
{
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    switch (button) {
    case kQ3ViewerButtonCamera: /* a camera body and lens */
        tb_MoveTo((int16_t)(cx - 6), (int16_t)(cy - 3));
        tb_LineTo((int16_t)(cx + 4), (int16_t)(cy - 3));
        tb_LineTo((int16_t)(cx + 4), (int16_t)(cy + 3));
        tb_LineTo((int16_t)(cx - 6), (int16_t)(cy + 3));
        tb_LineTo((int16_t)(cx - 6), (int16_t)(cy - 3));
        tb_MoveTo((int16_t)(cx + 4), (int16_t)cy);
        tb_LineTo((int16_t)(cx + 7), (int16_t)(cy - 3));
        tb_LineTo((int16_t)(cx + 7), (int16_t)(cy + 3));
        tb_LineTo((int16_t)(cx + 4), (int16_t)cy);
        break;
    case kQ3ViewerButtonTruck: /* a cross of arrows */
        tb_MoveTo((int16_t)(cx - 6), (int16_t)cy);
        tb_LineTo((int16_t)(cx + 6), (int16_t)cy);
        tb_MoveTo((int16_t)cx, (int16_t)(cy - 6));
        tb_LineTo((int16_t)cx, (int16_t)(cy + 6));
        break;
    case kQ3ViewerButtonOrbit: /* a circle round a point */
        for (int i = 0; i <= 16; i++) {
            float a = (float)i * (float)M_PI / 8;
            int16_t x = (int16_t)(cx + lrintf(6 * cosf(a))), y = (int16_t)(cy + lrintf(4 * sinf(a)));
            if (i)
                tb_LineTo(x, y);
            else
                tb_MoveTo(x, y);
        }
        tb_MoveTo((int16_t)cx, (int16_t)cy);
        tb_LineTo((int16_t)cx, (int16_t)cy);
        break;
    case kQ3ViewerButtonZoom: /* a magnifier */
        for (int i = 0; i <= 12; i++) {
            float a = (float)i * (float)M_PI / 6;
            int16_t x = (int16_t)(cx - 1 + lrintf(4 * cosf(a))), y = (int16_t)(cy - 1 + lrintf(4 * sinf(a)));
            if (i)
                tb_LineTo(x, y);
            else
                tb_MoveTo(x, y);
        }
        tb_MoveTo((int16_t)(cx + 2), (int16_t)(cy + 2));
        tb_LineTo((int16_t)(cx + 6), (int16_t)(cy + 6));
        break;
    case kQ3ViewerButtonDolly: /* in and out */
        tb_MoveTo((int16_t)cx, (int16_t)(cy - 6));
        tb_LineTo((int16_t)cx, (int16_t)(cy + 6));
        tb_MoveTo((int16_t)(cx - 3), (int16_t)(cy - 3));
        tb_LineTo((int16_t)cx, (int16_t)(cy - 6));
        tb_LineTo((int16_t)(cx + 3), (int16_t)(cy - 3));
        tb_MoveTo((int16_t)(cx - 3), (int16_t)(cy + 3));
        tb_LineTo((int16_t)cx, (int16_t)(cy + 6));
        tb_LineTo((int16_t)(cx + 3), (int16_t)(cy + 3));
        break;
    default: /* reset: a curved arrow */
        tb_MoveTo((int16_t)(cx - 5), (int16_t)(cy + 3));
        tb_LineTo((int16_t)(cx - 5), (int16_t)(cy - 3));
        tb_LineTo((int16_t)(cx + 5), (int16_t)(cy - 3));
        tb_LineTo((int16_t)(cx + 5), (int16_t)(cy + 3));
        tb_MoveTo((int16_t)(cx + 2), (int16_t)cy);
        tb_LineTo((int16_t)(cx + 5), (int16_t)(cy + 3));
        tb_LineTo((int16_t)(cx + 8), (int16_t)cy);
        break;
    }
}

static void draw_strip(viewer *v)
{
    if (!(v->flags & kQ3ViewerControllerVisible) || !v->port)
        return;
    gaddr s = v->scratch + 64, r = v->scratch + 80;
    qrect sr = strip_rect(v);
    bool active = v->flags & kQ3ViewerActive;
    fore(s, 0xDDDD, 0xDDDD, 0xDDDD);
    rect_put(r, sr);
    tb_PaintRect(r);
    fore(s, 0x8888, 0x8888, 0x8888);
    tb_MoveTo(sr.left, sr.top);
    tb_LineTo((int16_t)(sr.right - 1), sr.top);
    for (int i = 0; i < 6; i++) {
        qrect b;
        if (!button_rect(v, button_order[i], &b))
            continue;
        bool on = v->button == button_order[i];
        if (on)
            fore(s, 0x9999, 0x9999, 0xBBBB);
        else
            fore(s, 0xEEEE, 0xEEEE, 0xEEEE);
        rect_put(r, b);
        tb_PaintRect(r);
        fore(s, 0x5555, 0x5555, 0x5555);
        tb_FrameRect(r);
        if (active)
            fore(s, 0, 0, 0);
        else
            fore(s, 0x9999, 0x9999, 0x9999);
        draw_glyph(button_order[i], b);
    }
    fore(s, 0, 0, 0);
}

static void draw_frame_and_badge(viewer *v)
{
    gaddr s = v->scratch + 64, r = v->scratch + 80;
    if (v->flags & kQ3ViewerDrawFrame) {
        fore(s, 0, 0, 0);
        rect_put(r, v->bounds);
        tb_FrameRect(r);
    }
    if ((v->flags & kQ3ViewerShowBadge) && !(v->flags & kQ3ViewerActive)) {
        qrect c = content_rect(v);
        qrect b = { (int16_t)(c.top + 2), (int16_t)(c.left + 2), (int16_t)(c.top + 14), (int16_t)(c.left + 20) };
        fore(s, 0xFFFF, 0xFFFF, 0xFFFF);
        rect_put(r, b);
        tb_PaintRect(r);
        fore(s, 0, 0, 0);
        tb_FrameRect(r);
        tb_MoveTo((int16_t)(b.left + 4), (int16_t)(b.bottom - 3));
        tb_LineTo((int16_t)(b.left + 8), (int16_t)(b.bottom - 3));
        tb_LineTo((int16_t)(b.left + 8), (int16_t)(b.top + 3));
        tb_LineTo((int16_t)(b.left + 4), (int16_t)(b.top + 3));
        tb_MoveTo((int16_t)(b.left + 5), (int16_t)((b.top + b.bottom) / 2));
        tb_LineTo((int16_t)(b.left + 8), (int16_t)((b.top + b.bottom) / 2));
        tb_MoveTo((int16_t)(b.left + 11), (int16_t)(b.top + 3));
        tb_LineTo((int16_t)(b.left + 11), (int16_t)(b.bottom - 3));
        tb_LineTo((int16_t)(b.left + 14), (int16_t)(b.bottom - 4));
        tb_LineTo((int16_t)(b.left + 14), (int16_t)(b.top + 4));
        tb_LineTo((int16_t)(b.left + 11), (int16_t)(b.top + 3));
    }
}

/* Around a drawing: the viewer's port current, the program's colours
 * and port back afterwards. */
static gaddr begin_port(viewer *v)
{
    gaddr saved = v->scratch + 96;
    tb_GetPort(saved);
    tb_GetForeColor(saved + 4);
    tb_GetBackColor(saved + 10);
    if (v->port)
        tb_SetPort(v->port);
    return saved;
}

static void end_port(gaddr saved)
{
    tb_RGBForeColor(saved + 4);
    tb_RGBBackColor(saved + 10);
    tb_SetPort(rd32(saved));
}

static void callback(gaddr fn, gaddr g, gaddr data)
{
    if (fn) {
        uint32_t args[2] = { g, data };
        ppc_call(fn, args, 2);
    }
}

static void draw_all(viewer *v, bool content, bool strip)
{
    gaddr saved = begin_port(v);
    if (content)
        draw_content(v);
    if (strip)
        draw_strip(v);
    draw_frame_and_badge(v);
    end_port(saved);
    if (content)
        callback(v->draw_cb, v->g, v->draw_data);
}

/* ---------------------------------------------------------------------- */
/* Making and disposing */

static void release(gaddr *g)
{
    if (*g)
        CALL("Q3Object_Dispose", *g);
    *g = 0;
}

static void use_group(viewer *v, gaddr group)
{
    release(&v->group);
    v->group = group;
    measure(v);
    set_view_direction(v, kQ3ViewerCameraFront);
    v->home = v->cam;
    v->has_undo = false;
}

static void Q3ViewerNew(ppc_cpu *c)
{
    gaddr port = Q3R(0), rect = Q3R(1);
    uint32_t flags = Q3R(2);
    viewer *v = calloc(1, sizeof *v);
    if (!v || !rect) {
        free(v);
        Q3RETURN(0);
        return;
    }
    if (!q3_initialized())
        CALL0("Q3Initialize");
    v->scratch = tb_NewPtr(256, true, true);
    v->g = q3_handle_new(VIEWER_KIND, v);
    if (!v->scratch || !v->g) {
        if (v->scratch)
            tb_DisposePtr(v->scratch);
        free(v);
        Q3RETURN(0);
        return;
    }
    v->port = port;
    v->bounds = qrect_get(rect);
    v->flags = (flags & (uint32_t)kQ3ViewerDefault) ? DEFAULT_FLAGS : flags;
    v->button = kQ3ViewerButtonOrbit;
    v->bg[0] = v->bg[1] = v->bg[2] = v->bg[3] = 1; /* white */
    v->brightness = 1;
    v->phong = true;
    v->renderer = Q3T('c', 't', 'w', 'n');
    gaddr s = v->scratch;
    v->view = CALL0("Q3View_New");
    /* A Macintosh draw context: cleared to the background, double-buffered. */
    for (int i = 0; i < 21; i++)
        wr32(s + 4 * (uint32_t)i, 0);
    wr32(s, 1);
    put_floats(s + 4, v->bg, 4);
    wr32(s + 16 * 4, 1);
    wr32(s + 17 * 4, port);
    wr32(s + 18 * 4, 1); /* kQ3Mac2DLibraryQuickDraw */
    wr32(s + 20 * 4, port);
    v->dctx = CALL("Q3MacDrawContext_New", s);
    float cam[17] = { 0, 0, 5, 0, 0, 0, 0, 1, 0, 1, 10, -1, 1, 2, 2, 0.8f, 1 };
    put_floats(s, cam, 17);
    v->camera = CALL("Q3ViewAngleAspectCamera_New", s);
    v->lights = CALL0("Q3LightGroup_New");
    float amb[5] = { 0, 0.4f, 1, 1, 1 };
    put_floats(s, amb, 5);
    wr32(s, 1);
    v->ambient = CALL("Q3AmbientLight_New", s);
    float dir[9] = { 0, 1, 1, 1, 1, 0, 0.5f, -0.5f, -1 };
    put_floats(s, dir, 9);
    wr32(s, 1);
    wr32(s + 20, 0);
    v->directional = CALL("Q3DirectionalLight_New", s);
    CALL("Q3Group_AddObject", v->lights, v->ambient);
    CALL("Q3Group_AddObject", v->lights, v->directional);
    v->illum = CALL0("Q3PhongIllumination_New");
    CALL("Q3View_SetDrawContext", v->view, v->dctx);
    CALL("Q3View_SetCamera", v->view, v->camera);
    CALL("Q3View_SetLightGroup", v->view, v->lights);
    CALL("Q3View_SetRendererByType", v->view, v->renderer);
    use_group(v, 0);
    Q3RETURN(v->g);
}

static void Q3ViewerDispose(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    if (!v) {
        Q3RETURN(paramErr);
        return;
    }
    release(&v->group);
    release(&v->view);
    release(&v->dctx);
    release(&v->camera);
    release(&v->ambient);
    release(&v->directional);
    release(&v->lights);
    release(&v->illum);
    tb_DisposePtr(v->scratch);
    q3_handle_free(v->g);
    free(v);
    Q3RETURN(noErr);
}

/* ---------------------------------------------------------------------- */
/* Models */

static int use_bytes(viewer *v, const uint8_t *bytes, uint32_t size)
{
    gaddr g = q3_read_3dmf(bytes, size);
    if (!g)
        return paramErr;
    use_group(v, g);
    return noErr;
}

static void Q3ViewerUseFile(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    int16_t ref = (int16_t)Q3R(1);
    char path[16];
    bool rsrc, write;
    uint32_t pos, len;
    if (!v || !fm_fork_state(ref, path, sizeof path, &rsrc, &write, &pos, &len)) {
        Q3RETURN(paramErr);
        return;
    }
    gaddr buf = tb_NewPtr((int32_t)(len ? len : 1), false, false);
    if (!buf)
        buf = tb_NewPtr((int32_t)(len ? len : 1), true, false);
    if (!buf) {
        Q3RETURN(memFullErr);
        return;
    }
    uint32_t got = 0;
    int16_t err = fm_fork_io(ref, false, 1 /* fsFromStart */, 0, len, buf, &got);
    int r = err && err != kEofErr ? err : use_bytes(v, gptr(buf), got);
    tb_DisposePtr(buf);
    Q3RETURN(r);
}

static void Q3ViewerUseData(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    gaddr data = Q3R(1);
    int32_t size = (int32_t)Q3R(2);
    Q3RETURN(!v || !data || size <= 0 ? paramErr : use_bytes(v, gptr(data), (uint32_t)size));
}

static void Q3ViewerUseGroup(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    if (!v || !Q3R(1)) {
        Q3RETURN(paramErr);
        return;
    }
    use_group(v, q3_ref(Q3R(1)));
    Q3RETURN(noErr);
}

static void Q3ViewerGetGroup(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    Q3RETURN(v && v->group ? q3_ref(v->group) : 0);
}

static void Q3ViewerWriteFile(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    if (!v || !v->group) {
        Q3RETURN(paramErr);
        return;
    }
    uint32_t size = 0;
    uint8_t *bytes = q3_write_3dmf(v->group, &size);
    gaddr buf = bytes ? tb_NewPtr((int32_t)(size ? size : 1), false, false) : 0;
    if (!buf) {
        free(bytes);
        Q3RETURN(bytes ? memFullErr : paramErr);
        return;
    }
    gput(buf, bytes, size);
    uint32_t done = 0;
    int16_t err = fm_fork_io((int16_t)Q3R(1), true, 1, 0, size, buf, &done);
    if (!err)
        err = fm_fork_set_eof((int16_t)Q3R(1), 1, size);
    tb_DisposePtr(buf);
    free(bytes);
    Q3RETURN(err);
}

static void Q3ViewerWriteData(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    gaddr h = Q3R(1);
    if (!v || !v->group || !h) {
        Q3RETURN(0);
        return;
    }
    uint32_t size = 0;
    uint8_t *bytes = q3_write_3dmf(v->group, &size);
    if (!bytes) {
        Q3RETURN(0);
        return;
    }
    tb_SetHandleSize(h, (int32_t)size);
    if ((uint32_t)tb_GetHandleSize(h) < size) {
        free(bytes);
        Q3RETURN(0);
        return;
    }
    gput(rd32(h), bytes, size);
    free(bytes);
    Q3RETURN(size);
}

/* ---------------------------------------------------------------------- */
/* Drawing calls */

static void Q3ViewerDraw(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    if (v)
        draw_all(v, true, true);
    Q3RETURN(v ? noErr : paramErr);
}

static void Q3ViewerDrawContent(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    if (v)
        draw_all(v, true, false);
    Q3RETURN(v ? noErr : paramErr);
}

static void Q3ViewerDrawControlStrip(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    if (v)
        draw_all(v, false, true);
    Q3RETURN(v ? noErr : paramErr);
}

/* A picture of the content: rendered into an offscreen world and
 * recorded with CopyBits. */
static void Q3ViewerGetPict(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    if (!v) {
        Q3RETURN(0);
        return;
    }
    qrect cr = content_rect(v);
    int w = cr.right - cr.left, h = cr.bottom - cr.top;
    gaddr s = v->scratch + 128;
    if (w <= 0 || h <= 0) {
        Q3RETURN(0);
        return;
    }
    qrect local = { 0, 0, (int16_t)h, (int16_t)w };
    rect_put(s + 4, local);
    if (tb_NewGWorld(s, 32, s + 4, 0, 0, 0) || !rd32(s)) {
        Q3RETURN(0);
        return;
    }
    gaddr gw = rd32(s);
    tb_LockPixels(tb_GetGWorldPixMap(gw));
    /* Render there: the viewer on the world for a moment. */
    gaddr port = v->port;
    qrect bounds = v->bounds;
    uint32_t flags = v->flags;
    v->port = gw;
    v->bounds = local;
    v->flags &= ~(uint32_t)(kQ3ViewerControllerVisible | kQ3ViewerDrawFrame);
    gaddr saved = begin_port(v);
    draw_content(v);
    v->port = port;
    v->bounds = bounds;
    v->flags = flags;
    sync(v);
    tb_SetPort(gw);
    gaddr pic = tb_OpenPicture(s + 4);
    if (pic) {
        gaddr pm = gw + CGrafPort_portPixMap_off;
        tb_CopyBits(pm, pm, s + 4, s + 4, 0, 0);
        tb_ClosePicture();
    }
    end_port(saved);
    tb_DisposeGWorld(gw);
    Q3RETURN(pic);
}

/* ---------------------------------------------------------------------- */
/* Events and tracking */

static void apply_drag(viewer *v, int dx, int dy)
{
    float fwd[3] = { v->cam.poi[0] - v->cam.loc[0], v->cam.poi[1] - v->cam.loc[1], v->cam.poi[2] - v->cam.loc[2] };
    float d = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
    float up[3] = { v->cam.up[0], v->cam.up[1], v->cam.up[2] }, right[3];
    norm(fwd);
    cross(fwd, up, right);
    norm(right);
    cross(right, fwd, up);
    qrect cr = content_rect(v);
    float h = (float)(cr.bottom - cr.top > 0 ? cr.bottom - cr.top : 1);
    switch (v->button) {
    case kQ3ViewerButtonTruck: {
        float scale = 2 * d * tanf(v->cam.fov / 2) / h;
        for (int k = 0; k < 3; k++) {
            float m = -(float)dx * scale * right[k] + (float)dy * scale * up[k];
            v->cam.loc[k] += m;
            v->cam.poi[k] += m;
            v->center[k] += m;
        }
        break;
    }
    case kQ3ViewerButtonZoom:
        v->cam.fov *= expf((float)dy * 0.01f);
        v->cam.fov = fminf(fmaxf(v->cam.fov, 0.02f), 2.8f);
        break;
    case kQ3ViewerButtonDolly: {
        float nd = fmaxf(d * expf((float)dy * 0.01f), v->radius * 0.05f + 1e-4f);
        for (int k = 0; k < 3; k++)
            v->cam.loc[k] = v->cam.poi[k] - fwd[k] * nd;
        fit_range(v);
        break;
    }
    default: { /* orbit about the model's centre */
        float ax = (float)dx * 0.01f, ay = (float)dy * 0.01f;
        float back[3] = { -fwd[0] * d, -fwd[1] * d, -fwd[2] * d };
        /* Round up by ax, then round right by ay. */
        float b1[3], u1[3];
        for (int k = 0; k < 3; k++) {
            b1[k] = back[k] * cosf(ax) - right[k] * d * sinf(ax);
            u1[k] = up[k];
        }
        float r1[3];
        float f1[3] = { -b1[0], -b1[1], -b1[2] };
        norm(f1);
        cross(f1, u1, r1);
        norm(r1);
        float b2[3], u2[3];
        for (int k = 0; k < 3; k++) {
            b2[k] = b1[k] * cosf(ay) + u1[k] * d * sinf(ay);
            u2[k] = u1[k] * cosf(ay) - b1[k] / (d ? d : 1) * sinf(ay);
        }
        for (int k = 0; k < 3; k++) {
            v->cam.loc[k] = v->cam.poi[k] + b2[k];
            v->cam.up[k] = u2[k];
        }
        norm(v->cam.up);
        fit_range(v);
        break;
    }
    }
}

static bool in_rect(int x, int y, qrect r) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

/* A click in the strip: a mode chosen, the camera turned to the next
 * view, or the camera reset. */
static bool click_strip(viewer *v, int x, int y)
{
    for (int i = 0; i < 6; i++) {
        qrect b;
        if (!button_rect(v, button_order[i], &b) || !in_rect(x, y, b))
            continue;
        uint32_t button = button_order[i];
        if (button == kQ3ViewerButtonCamera) {
            save_undo(v);
            v->camera_view = v->camera_view >= kQ3ViewerCameraBottom ? kQ3ViewerCameraFront : v->camera_view + 1;
            if (v->camera_view < kQ3ViewerCameraFront)
                v->camera_view = kQ3ViewerCameraFront;
            set_view_direction(v, v->camera_view);
            draw_all(v, true, true);
        } else if (button == kQ3ViewerButtonReset) {
            save_undo(v);
            set_view_direction(v, kQ3ViewerCameraRestore);
            draw_all(v, true, true);
        } else {
            v->button = button;
            draw_all(v, false, true);
        }
        return true;
    }
    return in_rect(x, y, strip_rect(v));
}

static bool mouse_down(viewer *v, int x, int y)
{
    if (!in_rect(x, y, v->bounds))
        return false;
    if ((v->flags & kQ3ViewerControllerVisible) && in_rect(x, y, strip_rect(v)))
        return click_strip(v, x, y);
    save_undo(v);
    v->tracking = true;
    v->lastx = x;
    v->lasty = y;
    return true;
}

static bool continue_tracking(viewer *v, int x, int y)
{
    if (!v->tracking)
        return false;
    if (x != v->lastx || y != v->lasty) {
        apply_drag(v, x - v->lastx, y - v->lasty);
        v->lastx = x;
        v->lasty = y;
        draw_all(v, true, false);
    }
    return true;
}

static bool mouse_up(viewer *v, int x, int y)
{
    if (!v->tracking)
        return false;
    continue_tracking(v, x, y);
    v->tracking = false;
    return true;
}

static void Q3ViewerMouseDown(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    Q3RETURN(v && mouse_down(v, (int32_t)Q3R(1), (int32_t)Q3R(2)));
}

static void Q3ViewerContinueTracking(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    Q3RETURN(v && continue_tracking(v, (int32_t)Q3R(1), (int32_t)Q3R(2)));
}

static void Q3ViewerMouseUp(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    Q3RETURN(v && mouse_up(v, (int32_t)Q3R(1), (int32_t)Q3R(2)));
}

/* Arrow keys orbit; others aren't the viewer's. */
static bool key(viewer *v, gaddr evt)
{
    uint8_t ch = (uint8_t)rd32(evt + 2);
    int dx = 0, dy = 0;
    switch (ch) {
    case 0x1C: dx = -10; break;
    case 0x1D: dx = 10; break;
    case 0x1E: dy = -10; break;
    case 0x1F: dy = 10; break;
    default: return false;
    }
    if (!(v->flags & kQ3ViewerActive))
        return false;
    save_undo(v);
    uint32_t b = v->button;
    v->button = kQ3ViewerButtonOrbit;
    apply_drag(v, dx, dy);
    v->button = b;
    draw_all(v, true, false);
    return true;
}

static void Q3ViewerHandleKeyEvent(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    Q3RETURN(v && Q3R(1) && key(v, Q3R(1)));
}

static void local_point(viewer *v, gaddr evt, int *x, int *y)
{
    gaddr pt = v->scratch + 120;
    wr32(pt, rd32(evt + 10));
    gaddr saved = begin_port(v);
    tb_GlobalToLocal(pt);
    end_port(saved);
    *y = (int16_t)rd16(pt);
    *x = (int16_t)rd16(pt + 2);
}

static void Q3ViewerEvent(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    gaddr evt = Q3R(1);
    if (!v || !evt) {
        Q3RETURN(0);
        return;
    }
    uint16_t what = rd16(evt);
    uint32_t msg = rd32(evt + 2);
    bool handled = false;
    switch (what) {
    case mouseDown: {
        int x, y;
        local_point(v, evt, &x, &y);
        handled = mouse_down(v, x, y);
        if (handled && v->tracking) {
            /* Track until the button comes up. */
            gaddr pt = v->scratch + 124;
            for (int n = 0; n < 100000 && tb_StillDown(); n++) {
                gaddr saved = begin_port(v);
                tb_GetMouse(pt);
                end_port(saved);
                continue_tracking(v, (int16_t)rd16(pt + 2), (int16_t)rd16(pt));
            }
            mouse_up(v, v->lastx, v->lasty);
        }
        break;
    }
    case keyDown:
    case autoKey: handled = key(v, evt); break;
    case updateEvt:
        if (msg == v->port) {
            tb_BeginUpdate(msg);
            draw_all(v, true, true);
            tb_EndUpdate(msg);
            handled = true;
        }
        break;
    case activateEvt:
        if (msg == v->port) {
            if (rd16(evt + 14) & 1)
                v->flags |= kQ3ViewerActive;
            else
                v->flags &= ~(uint32_t)kQ3ViewerActive;
            draw_all(v, false, true);
            handled = true;
        }
        break;
    default: break;
    }
    Q3RETURN(handled);
}

static void Q3ViewerAdjustCursor(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    gaddr pt = Q3R(1);
    if (!v || !pt) {
        Q3RETURN(0);
        return;
    }
    int y = (int16_t)rd16(pt), x = (int16_t)rd16(pt + 2);
    Q3RETURN(in_rect(x, y, content_rect(v)));
}

static void Q3ViewerCursorChanged(ppc_cpu *c) { Q3RETURN(viewer_of(Q3R(0)) ? noErr : paramErr); }

/* ---------------------------------------------------------------------- */
/* Buttons, flags, bounds and the rest */

static void Q3ViewerGetButtonRect(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    qrect b;
    if (!v || !Q3R(2) || !button_rect(v, Q3R(1), &b)) {
        Q3RETURN(paramErr);
        return;
    }
    rect_put(Q3R(2), b);
    Q3RETURN(noErr);
}

static void Q3ViewerGetCurrentButton(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    Q3RETURN(v ? v->button : 0);
}

static void Q3ViewerSetCurrentButton(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    uint32_t b = Q3R(1);
    if (!v || (b != kQ3ViewerButtonTruck && b != kQ3ViewerButtonOrbit && b != kQ3ViewerButtonZoom &&
               b != kQ3ViewerButtonDolly)) {
        Q3RETURN(paramErr);
        return;
    }
    v->button = b;
    Q3RETURN(noErr);
}

static void Q3ViewerSetBackgroundColor(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    if (!v || !Q3R(1)) {
        Q3RETURN(paramErr);
        return;
    }
    for (int k = 0; k < 4; k++)
        v->bg[k] = rdf(Q3R(1) + 4 * (uint32_t)k);
    Q3RETURN(noErr);
}

static void Q3ViewerGetBackgroundColor(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    if (!v || !Q3R(1)) {
        Q3RETURN(paramErr);
        return;
    }
    put_floats(Q3R(1), v->bg, 4);
    Q3RETURN(noErr);
}

static void Q3ViewerGetView(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    if (v)
        sync(v);
    Q3RETURN(v ? v->view : 0);
}

static void Q3ViewerRestoreView(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    if (v) {
        save_undo(v);
        set_view_direction(v, kQ3ViewerCameraRestore);
    }
    Q3RETURN(v ? noErr : paramErr);
}

static void Q3ViewerSetFlags(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    if (v)
        v->flags = (Q3R(1) & (uint32_t)kQ3ViewerDefault) ? DEFAULT_FLAGS : Q3R(1);
    Q3RETURN(v ? noErr : paramErr);
}

static void Q3ViewerGetFlags(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    Q3RETURN(v ? v->flags : 0);
}

static void set_bounds(viewer *v, qrect r)
{
    v->bounds = r;
    callback(v->pane_cb, v->g, v->pane_data);
}

static void Q3ViewerSetBounds(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    if (!v || !Q3R(1)) {
        Q3RETURN(paramErr);
        return;
    }
    set_bounds(v, qrect_get(Q3R(1)));
    Q3RETURN(noErr);
}

static void Q3ViewerGetBounds(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    if (!v || !Q3R(1)) {
        Q3RETURN(paramErr);
        return;
    }
    rect_put(Q3R(1), v->bounds);
    Q3RETURN(noErr);
}

static void Q3ViewerSetDimension(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    if (!v) {
        Q3RETURN(paramErr);
        return;
    }
    qrect r = v->bounds;
    r.right = (int16_t)(r.left + (int32_t)Q3R(1));
    r.bottom = (int16_t)(r.top + (int32_t)Q3R(2));
    set_bounds(v, r);
    Q3RETURN(noErr);
}

static void Q3ViewerGetDimension(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    if (!v) {
        Q3RETURN(paramErr);
        return;
    }
    if (Q3R(1))
        wr32(Q3R(1), (uint32_t)(v->bounds.right - v->bounds.left));
    if (Q3R(2))
        wr32(Q3R(2), (uint32_t)(v->bounds.bottom - v->bounds.top));
    Q3RETURN(noErr);
}

/* Room for the buttons shown and a little content. */
static void Q3ViewerGetMinimumDimension(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    if (!v) {
        Q3RETURN(paramErr);
        return;
    }
    int n = 0;
    for (int i = 0; i < 6; i++)
        n += (v->flags & button_order[i]) != 0;
    uint32_t w = (uint32_t)(n * BUTTON + 16), h = (v->flags & kQ3ViewerControllerVisible) ? STRIP + 16 : 16;
    if (w < 32)
        w = 32;
    if (Q3R(1))
        wr32(Q3R(1), w);
    if (Q3R(2))
        wr32(Q3R(2), h);
    Q3RETURN(noErr);
}

static void Q3ViewerSetPort(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    if (v)
        v->port = Q3R(1);
    Q3RETURN(v ? noErr : paramErr);
}

static void Q3ViewerGetPort(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    Q3RETURN(v ? v->port : 0);
}

static void Q3ViewerGetState(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    Q3RETURN(!v ? 0 : (v->group ? kQ3ViewerHasModel : 0) | (v->has_undo ? kQ3ViewerHasUndo : 0));
}

static void clear_model(viewer *v)
{
    use_group(v, 0);
    draw_all(v, true, false);
}

static void Q3ViewerClear(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    if (v)
        clear_model(v);
    Q3RETURN(v ? noErr : paramErr);
}

/* The model on the scrap as 3DMF, and its picture. */
static int copy_model(viewer *v)
{
    if (!v->group)
        return paramErr;
    uint32_t size = 0;
    uint8_t *bytes = q3_write_3dmf(v->group, &size);
    tb_ZeroScrap();
    if (bytes) {
        gaddr buf = tb_NewPtr((int32_t)(size ? size : 1), false, false);
        if (buf) {
            gput(buf, bytes, size);
            tb_PutScrap((int32_t)size, (int32_t)Q3T('3', 'D', 'M', 'F'), buf);
            tb_DisposePtr(buf);
        }
        free(bytes);
    }
    ppc_cpu cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.gpr[3] = v->g;
    Q3ViewerGetPict(&cpu);
    gaddr pic = cpu.gpr[3];
    if (pic) {
        tb_HLock(pic);
        tb_PutScrap(tb_GetHandleSize(pic), (int32_t)Q3T('P', 'I', 'C', 'T'), rd32(pic));
        tb_KillPicture(pic);
    }
    return noErr;
}

static void Q3ViewerCopy(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    Q3RETURN(v ? copy_model(v) : paramErr);
}

static void Q3ViewerCut(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    int err = v ? copy_model(v) : paramErr;
    if (!err)
        clear_model(v);
    Q3RETURN(err);
}

static void Q3ViewerPaste(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    if (!v) {
        Q3RETURN(paramErr);
        return;
    }
    gaddr h = tb_NewHandle(0, false, false);
    gaddr off = v->scratch + 124;
    int32_t n = h ? tb_GetScrap(h, (int32_t)Q3T('3', 'D', 'M', 'F'), off) : -1;
    int err = n > 0 ? use_bytes(v, gptr(rd32(h)), (uint32_t)n) : paramErr;
    if (h)
        tb_DisposeHandle(h);
    if (!err)
        draw_all(v, true, false);
    Q3RETURN(err);
}

static void Q3ViewerUndo(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    if (!v || !v->has_undo) {
        Q3RETURN(paramErr);
        return;
    }
    camstate now = v->cam;
    v->cam = v->undo;
    v->undo = now;
    draw_all(v, true, false);
    Q3RETURN(noErr);
}

static void Q3ViewerGetUndoString(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    gaddr str = Q3R(1), cnt = Q3R(2);
    if (!v || !v->has_undo || !cnt) {
        Q3RETURN(0);
        return;
    }
    static const char text[] = "Undo Camera Change";
    uint32_t room = rd32(cnt), n = (uint32_t)strlen(text);
    if (str && room)
        gput(str, text, n < room ? n : room);
    wr32(cnt, n);
    Q3RETURN(1);
}

/* The model's own cameras: those at its top level. */
static int model_cameras(viewer *v, int want, gaddr *found)
{
    q3group *gr = q3_group_of(v->group);
    int n = 0;
    for (q3pos *p = gr ? gr->first : NULL; p; p = p->next) {
        q3obj *o = q3_get(p->obj);
        if (o && q3_type_is(o->type, Q3_CAMERA)) {
            if (++n == want && found)
                *found = p->obj;
        }
    }
    return n;
}

static void Q3ViewerGetCameraCount(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    if (!v || !Q3R(1)) {
        Q3RETURN(paramErr);
        return;
    }
    wr32(Q3R(1), (uint32_t)model_cameras(v, 0, NULL));
    Q3RETURN(noErr);
}

static void Q3ViewerSetCameraByNumber(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    gaddr cam = 0;
    if (!v || !model_cameras(v, (int)Q3R(1), &cam) || !cam) {
        Q3RETURN(paramErr);
        return;
    }
    save_undo(v);
    gaddr s = v->scratch;
    CALL("Q3Camera_GetData", cam, s);
    for (int k = 0; k < 3; k++) {
        v->cam.loc[k] = rdf(s + 4 * (uint32_t)k);
        v->cam.poi[k] = rdf(s + 12 + 4 * (uint32_t)k);
        v->cam.up[k] = rdf(s + 24 + 4 * (uint32_t)k);
    }
    v->cam.hither = rdf(s + 36);
    v->cam.yon = rdf(s + 40);
    if (q3_type_is(q3_get(cam)->type, Q3T('v', 'a', 'n', 'a'))) {
        CALL("Q3ViewAngleAspectCamera_GetFOV", cam, s);
        v->cam.fov = rdf(s);
    }
    Q3RETURN(noErr);
}

static void Q3ViewerSetCameraByView(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    if (!v || Q3R(1) > kQ3ViewerCameraBottom) {
        Q3RETURN(paramErr);
        return;
    }
    save_undo(v);
    set_view_direction(v, (int)Q3R(1));
    Q3RETURN(noErr);
}

static void Q3ViewerSetRendererType(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    if (!v || CALL("Q3View_SetRendererByType", v->view, Q3R(1)) != kQ3Success) {
        Q3RETURN(paramErr);
        return;
    }
    v->renderer = Q3R(1);
    Q3RETURN(noErr);
}

static void Q3ViewerGetRendererType(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    if (!v || !Q3R(1)) {
        Q3RETURN(paramErr);
        return;
    }
    wr32(Q3R(1), v->renderer);
    Q3RETURN(noErr);
}

static void Q3ViewerChangeBrightness(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    if (v)
        v->brightness = fminf(fmaxf(q3_farg(c, 0), 0), 1);
    Q3RETURN(v ? noErr : paramErr);
}

static void Q3ViewerSetRemoveBackfaces(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    if (v)
        v->remove_backfaces = Q3R(1) != 0;
    Q3RETURN(v ? noErr : paramErr);
}

static void Q3ViewerGetRemoveBackfaces(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    if (v && Q3R(1))
        wr32(Q3R(1), v->remove_backfaces);
    Q3RETURN(v && Q3R(1) ? noErr : paramErr);
}

static void Q3ViewerSetPhongShading(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    if (v && v->phong != (Q3R(1) != 0)) {
        v->phong = Q3R(1) != 0;
        release(&v->illum);
        v->illum = v->phong ? CALL0("Q3PhongIllumination_New") : CALL0("Q3LambertIllumination_New");
    }
    Q3RETURN(v ? noErr : paramErr);
}

static void Q3ViewerGetPhongShading(ppc_cpu *c)
{
    viewer *v = viewer_of(Q3R(0));
    if (v && Q3R(1))
        wr32(Q3R(1), v->phong);
    Q3RETURN(v && Q3R(1) ? noErr : paramErr);
}

#define SET_CALLBACK(fn, cb, data)                                                                                   \
    static void fn(ppc_cpu *c)                                                                                       \
    {                                                                                                                \
        viewer *v = viewer_of(Q3R(0));                                                                               \
        if (v) {                                                                                                     \
            v->cb = Q3R(1);                                                                                          \
            v->data = Q3R(2);                                                                                        \
        }                                                                                                            \
        Q3RETURN(v ? noErr : paramErr);                                                                              \
    }
SET_CALLBACK(Q3ViewerSetDrawingCallbackMethod, draw_cb, draw_data)
SET_CALLBACK(Q3ViewerSetWindowResizeCallback, resize_cb, resize_data)
SET_CALLBACK(Q3ViewerSetPaneResizeNotifyCallback, pane_cb, pane_data)

static void Q3ViewerGetVersion(ppc_cpu *c)
{
    if (Q3R(0))
        wr32(Q3R(0), 1);
    if (Q3R(1))
        wr32(Q3R(1), 6);
    Q3RETURN(noErr);
}

static void Q3ViewerGetReleaseVersion(ppc_cpu *c)
{
    if (Q3R(0))
        wr32(Q3R(0), 0x01608000); /* 1.6, final */
    Q3RETURN(noErr);
}

#define E(n) { #n, n }
const ppc_export q3_viewer_exports[] = {
    E(Q3ViewerGetVersion), E(Q3ViewerGetReleaseVersion), E(Q3ViewerNew), E(Q3ViewerDispose), E(Q3ViewerUseFile),
    E(Q3ViewerUseData), E(Q3ViewerWriteFile), E(Q3ViewerWriteData), E(Q3ViewerDraw), E(Q3ViewerDrawContent),
    E(Q3ViewerDrawControlStrip), E(Q3ViewerEvent), E(Q3ViewerGetPict), E(Q3ViewerGetButtonRect),
    E(Q3ViewerGetCurrentButton), E(Q3ViewerSetCurrentButton), E(Q3ViewerUseGroup), E(Q3ViewerGetGroup),
    E(Q3ViewerSetBackgroundColor), E(Q3ViewerGetBackgroundColor), E(Q3ViewerGetView), E(Q3ViewerRestoreView),
    E(Q3ViewerSetFlags), E(Q3ViewerGetFlags), E(Q3ViewerSetBounds), E(Q3ViewerGetBounds), E(Q3ViewerSetDimension),
    E(Q3ViewerGetDimension), E(Q3ViewerGetMinimumDimension), E(Q3ViewerSetPort), E(Q3ViewerGetPort),
    E(Q3ViewerAdjustCursor), E(Q3ViewerCursorChanged), E(Q3ViewerGetState), E(Q3ViewerClear), E(Q3ViewerCut),
    E(Q3ViewerCopy), E(Q3ViewerPaste), E(Q3ViewerMouseDown), E(Q3ViewerContinueTracking), E(Q3ViewerMouseUp),
    E(Q3ViewerHandleKeyEvent), E(Q3ViewerSetDrawingCallbackMethod), E(Q3ViewerSetWindowResizeCallback),
    E(Q3ViewerSetPaneResizeNotifyCallback), E(Q3ViewerUndo), E(Q3ViewerGetUndoString), E(Q3ViewerGetCameraCount),
    E(Q3ViewerSetCameraByNumber), E(Q3ViewerSetCameraByView), E(Q3ViewerSetRendererType),
    E(Q3ViewerGetRendererType), E(Q3ViewerChangeBrightness), E(Q3ViewerSetRemoveBackfaces),
    E(Q3ViewerGetRemoveBackfaces), E(Q3ViewerSetPhongShading), E(Q3ViewerGetPhongShading),
};
const unsigned q3_viewer_export_count = sizeof q3_viewer_exports / sizeof q3_viewer_exports[0];
