/*
 * The view's insides (q3_view.c), shared with the code that drives a
 * renderer plug-in a program or an extension registered (q3_xrender.c).
 */
#ifndef Q3_VIEWPRIV_H
#define Q3_VIEWPRIV_H

#include "q3_view.h"

#define CAMERA_ORTH Q3T('o', 'r', 't', 'h')
#define CAMERA_VIEWPLANE Q3T('v', 'w', 'p', 'l')
#define CAMERA_VANA Q3T('v', 'a', 'n', 'a')
#define LIGHT_AMBIENT Q3T('a', 'm', 'b', 'n')
#define LIGHT_DIRECTIONAL Q3T('d', 'r', 'c', 't')
#define LIGHT_POINT Q3T('p', 'n', 't', 'l')
#define LIGHT_SPOT Q3T('s', 'p', 'o', 't')
#define DC_PIXMAP Q3T('d', 'p', 'x', 'p')
#define DC_MAC Q3T('d', 'm', 'a', 'c')
#define RENDERER_WIREFRAME Q3T('w', 'r', 'f', 'r')
#define RENDERER_GENERIC Q3T('g', 'n', 'r', 'r')
#define ILLUM_PHONG Q3T('p', 'h', 'i', 'l')
#define ILLUM_LAMBERT Q3T('l', 'm', 'i', 'l')
#define ILLUM_NULL Q3T('n', 'u', 'i', 'l')
#define TEXTURE_SHADER Q3T('t', 'x', 's', 'u')

enum { kQ3ViewStatusDone, kQ3ViewStatusRetraverse, kQ3ViewStatusError, kQ3ViewStatusCancelled };
enum { LOOP_NONE, LOOP_RENDER, LOOP_BOUND_BOX, LOOP_BOUND_SPHERE, LOOP_PICK, LOOP_WRITE };
enum { kQ3BackfacingStyleBoth, kQ3BackfacingStyleRemove, kQ3BackfacingStyleFlip };
enum { kQ3InterpolationStyleNone, kQ3InterpolationStyleVertex, kQ3InterpolationStylePixel };
enum { kQ3FillStyleFilled, kQ3FillStyleEdges, kQ3FillStylePoints };
enum { kQ3SubdivisionMethodConstant, kQ3SubdivisionMethodWorldSpace, kQ3SubdivisionMethodScreenSpace };

typedef struct {
    float m[16];
    q3attrs a;
    uint32_t backfacing, interpolation, fill, orientation, receive, pickid, pickparts;
    gaddr highlight;
    uint32_t subdiv_method;
    float subdiv_c1, subdiv_c2;
    uint32_t aa[3], fog[9];
    uint32_t illumination;
    gaddr illum;        /* the illumination shader object in force, borrowed */
    gaddr texture;      /* the texture shader in force */
} vstate;

typedef struct {
    int kind;
    float color[3], dir[3], pos[3];
    int atten, falloff;
    float hot, outer;
} vlight;

typedef struct {
    r3d_vertex v[3];
    float depth;
    int texture;
    bool clamp_u, clamp_v;
} deferred;

struct q3view {
    gaddr g;
    gaddr renderer, camera, lights, dctx, default_set;
    int loop;
    bool exact, cancelled, retraverse;
    vstate st, *stack;
    int depth, cap;
    /* The frame. */
    r3d_frame *frame;
    uint32_t *pixels;
    int fw, fh, px, py; /* its size; the pane's top left in the port */
    float w2v[16], v2f[16], w2f[16], f2w[16];
    float eye[3], look[3];
    bool ortho;
    vlight *L;
    int nL;
    float ambient[3];
    deferred *defer;
    int ndefer, capdefer;
    int *frame_textures; /* made for this frame (markers), freed at its end */
    int nframe_tex, capframe_tex;
    /* Bounds. */
    float bmin[3], bmax[3];
    bool bempty;
    float *bpts;
    uint32_t nbpts, capbpts;
    /* Picking. */
    gaddr pick, object;
    q3pickq pq;
    gaddr path[64];
    int pathlen;
    /* Writing. */
    gaddr file;
    gaddr idle, idle_data, progress, progress_data, end_frame, end_frame_data;
    /* The interactive renderer's RAVE draw context on the frame, for the
     * program (Q3InteractiveRenderer_GetRAVEDrawContexts), and what to
     * call when it goes. */
    gaddr rave_dc, rave_engine, rave_destroy;
    /* A renderer plug-in drawing this frame (q3_xrender.c), or NULL. */
    struct q3xr *xr;
};

/* ---- For q3_xrender.c (q3_view.c) ---- */
/* The draw context's destination: its port (0 for a pixmap) and the
 * pane's place and size there. */
bool q3v_dctx_area(gaddr dctx, gaddr *port, int *x, int *y, int *w, int *h, bool *pixmap);
/* The frame's pixels (v->pixels) into the draw context. */
void q3v_present(q3view *v);
/* What the destination has under the pane, into v->pixels. */
void q3v_read_destination(q3view *v);
/* The draw context's clear colour as 0xAARRGGBB, and whether it clears. */
bool q3v_clear_color(gaddr dctx, uint32_t *argb);
/* The view drawing into this draw context now, if any. */
q3view *q3v_drawing_into(gaddr dctx);
/* The state in force back to the defaults (a new pass). */
void q3v_default_state(q3view *v);

/* ---- Renderer plug-ins (q3_xrender.c) ---- */
/* Whether this renderer object is of a class a program registered. */
bool q3xr_is_plugin(gaddr renderer);
/* The frame's start: StartFrame, the default state, StartPass.  False if
 * the renderer failed it. */
bool q3xr_start(q3view *v);
/* The pass's end: EndPass's status (a retraverse starts the next pass). */
int q3xr_end_pass(q3view *v);
void q3xr_cancel(q3view *v);
/* Q3View_Sync: an asynchronous renderer's EndFrame. */
void q3xr_sync(q3view *v);
void q3xr_flush(q3view *v);
void q3xr_free(q3view *v);
void q3xr_push(q3view *v);
void q3xr_pop(q3view *v);
/* What everything else is broken into. */
void q3xr_triangles(q3view *v, const q3pv *pv, uint32_t npv, const q3ptri *t, uint32_t nt, gaddr geom_set);
void q3xr_lines(q3view *v, const q3pv *pv, uint32_t nseg, gaddr geom_set);
void q3xr_points(q3view *v, const q3pv *pv, uint32_t n, gaddr geom_set);
void q3xr_image(q3view *v, const float p[3], int dx, int dy, int w, int h, const uint32_t *argb, gaddr set);
/* Q3View_IsBoundingBoxVisible's answer from the renderer: -1 if it has
 * no method. */
int q3xr_box_visible(q3view *v, gaddr box);

#endif
