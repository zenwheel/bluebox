/*
 * The host's OpenGL contexts as opengl.c keeps them, and what hibernating
 * one needs (opengl_hib.c).  macOS only (a host OpenGL).
 */
#ifndef HOSTGL_INTERNAL_H
#define HOSTGL_INTERNAL_H

#ifdef __APPLE__
/* Deprecated since macOS 10.14, and still there. */
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <OpenGL/glu.h>

#include "hostgl.h"

/* A vertex array as the program described it. */
typedef struct {
    bool on;
    GLint size;
    GLenum type;
    GLsizei stride;
    gaddr ptr;
    void *host; /* converted elements */
    size_t host_cap;
} garray;

enum { MAX_UNITS = 8, CLIENT_STACK = 16 };

typedef struct {
    garray vertex, normal, color, index, edge, tex[MAX_UNITS];
} arrays;

/* A display list as the program made it: the calls compiled into it,
 * recorded (opengl.c) so that a fresh process can compile it again; none
 * for one glGenLists made and nothing was compiled into yet. */
typedef struct {
    uint32_t name;
    uint8_t *ops;
    uint32_t len, cap;
} gllist;

/* What contexts made sharing (aglCreateContext's share) have in common:
 * their texture objects' names and their display lists. */
typedef struct glgroup {
    int refs;
    uint32_t *tex; /* names made (glGenTextures) or bound, sorted */
    int ntex, captex;
    gllist *lists; /* sorted by name */
    int nlists, caplists;
    struct gltexture *saved; /* hibernated: the texture objects */
    int nsaved;
} glgroup;

struct hostgl_ctx {
    CGLContextObj cgl; /* NULL after resuming, until made again (opengl_hib.c) */
    GLuint fbo, color, depth;
    int depth_bits, stencil_bits;
    int width, height;
    bool sized;
    GLenum draw_buffer, read_buffer; /* as the program set them */
    arrays a;
    arrays saved[CLIENT_STACK]; /* glPushClientAttrib's vertex array state */
    int nsaved;
    GLenum client_unit; /* glClientActiveTexture, from 0 */
    /* glSelectBuffer and glFeedbackBuffer: the host's, and the program's */
    GLuint *select;
    GLfloat *feedback;
    gaddr select_to, feedback_to;
    GLsizei select_n, feedback_n;
    GLenum render_mode, feedback_type;
    glgroup *group;
    bool in_begin;         /* between glBegin and glEnd */
    bool recording;        /* between glNewList and glEndList */
    GLenum list_mode;      /* glNewList's */
    gllist compiling;      /* what it records */
    GLenum pending_error;  /* the host's, taken by hibernating: glGetError's next */
    struct glsaved *hib;   /* hibernated: the state and the framebuffer */
};

/* ---- opengl_hib.c ---- */

/* Why h cannot be saved as it is (it must be current), or NULL. */
const char *glh_unsettled(hostgl_ctx *h);
/* Hibernating: h's state, default textures and framebuffer read into
 * h->hib; a group's texture objects into g->saved.  The context must be
 * current; it is left as it was. */
void glh_capture(hostgl_ctx *h);
void glh_capture_textures(glgroup *g);
/* Resumed, in h's new context (current): the group's texture objects made
 * again, then (after its display lists) h's framebuffer and state. */
void glh_restore_textures(glgroup *g);
void glh_restore(hostgl_ctx *h);
void glh_free(hostgl_ctx *h);
void glh_free_textures(glgroup *g);
size_t glh_saved_bytes(const hostgl_ctx *h);
size_t glh_saved_texture_bytes(const glgroup *g);
/* The host's pixel store set to its defaults (no swapping: the host's
 * order), as the hibernating code reads and writes images. */
void glh_pixel_store_defaults(bool pack, bool unpack);

#endif
#endif
