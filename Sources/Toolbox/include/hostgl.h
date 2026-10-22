/*
 * The host's OpenGL, as Apple's OpenGL for Mac OS 9 (OpenGLLibrary and
 * OpenGLUtility) uses it: opengl.c has the gl and glu calls and the host
 * contexts, agl.c the AGL calls that make them and show their frames.
 */
#ifndef HOSTGL_H
#define HOSTGL_H

#include <stdbool.h>
#include <stdint.h>

#include "mem.h"
#include "ppc_bridge.h"

/* A host context: its own framebuffer (a colour buffer and, when asked
 * for, a depth and stencil buffer) of the drawable's size. */
typedef struct hostgl_ctx hostgl_ctx;

/* Whether this host has an OpenGL to render with (macOS; not Linux yet). */
bool hostgl_available(void);
/* A context with depth_bits of depth and stencil_bits of stencil (0: none),
 * sharing textures and display lists with `share`; NULL when it can't be. */
hostgl_ctx *hostgl_create(hostgl_ctx *share, int depth_bits, int stencil_bits);
void hostgl_destroy(hostgl_ctx *h);
/* The context the program's gl calls go to (NULL: none, and they do nothing). */
void hostgl_set_current(hostgl_ctx *h);
/* The framebuffer's size; the first time, the viewport and scissor box
 * become the whole of it, as attaching a drawable does. */
void hostgl_resize(hostgl_ctx *h, int width, int height);
/* Copy part of the colour buffer (x, y from its bottom left) into guest
 * memory at dst, top row first, as 32-bit pixels (xRGB, big-endian). */
void hostgl_read_frame(hostgl_ctx *h, int x, int y, int width, int height, gaddr dst, uint32_t row_bytes);
/* aglCopyContext: the state in mask (glPushAttrib's bits) from src to dst. */
void hostgl_copy_state(hostgl_ctx *src, hostgl_ctx *dst, uint32_t mask);
/* aglUseFont: display list `list` draws one glyph, a 1-bit bitmap of
 * width by height (rows top first, row_bytes each) with its origin at
 * (xorig, yorig) from the bitmap's bottom left, advancing xmove. */
void hostgl_glyph_list(uint32_t list, int width, int height, int row_bytes, float xorig, float yorig, float xmove,
                       const uint8_t *bits);

/* The gl and glu exports (opengl.c), empty without a host OpenGL. */
extern const ppc_export gl_exports[];
extern const unsigned gl_export_count;
/* The AGL exports (agl.c). */
extern const ppc_export agl_exports[];
extern const unsigned agl_export_count;

/* Guest memory gone (machine reset): the strings and names made in it. */
void hostgl_reset(void);

/* glFlush or glFinish from the program: AGL shows the frame of a
 * single-buffered context, or one the program drew to the front of
 * (agl.c). */
void agl_flushed(bool front);
/* Contexts and pixel formats gone with the program (machine reset). */
void agl_reset(void);

#endif
