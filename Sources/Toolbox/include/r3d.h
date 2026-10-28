/*
 * The host's 3D renderer, as QuickDraw 3D and RAVE draw through it: a
 * frame (a colour and a depth buffer), state, batches of triangles, lines
 * and points already in window coordinates, textures, and the frame read
 * back into host memory.  Everything before (transforming, lighting,
 * clipping) is the caller's, platform-neutral C; a host implements only
 * this: Metal on macOS (r3d_metal.m), OpenGL on Linux later.
 */
#ifndef R3D_H
#define R3D_H

#include <stdbool.h>
#include <stdint.h>

/* A vertex as RAVE's TQAVTexture has it: x and y in pixels from the
 * frame's top left, z from 0 (near) to 1 (far), invw 1/w for perspective
 * (1 for none).  Untextured, r g b a is the colour; textured, the colour
 * is the texel times kd plus ks (as RAVE's modulate), its alpha the
 * texel's times a.  u and v are the texture coordinates themselves (not
 * divided by w), 0 to 1 across the texture, v from its top. */
typedef struct {
    float x, y, z, invw;
    float r, g, b, a;
    float u, v;
    float kd_r, kd_g, kd_b;
    float ks_r, ks_g, ks_b;
} r3d_vertex;

/* Depth tests: RAVE's kQAZFunction values. */
enum { R3D_Z_NONE = 0, R3D_Z_LT, R3D_Z_EQ, R3D_Z_LE, R3D_Z_GT, R3D_Z_NE, R3D_Z_GE, R3D_Z_TRUE, R3D_Z_FALSE };
/* Blending: none, or RAVE's two (premultiplied colours, or colours
 * interpolated by their alpha). */
enum { R3D_BLEND_NONE = 0, R3D_BLEND_PREMULTIPLY, R3D_BLEND_INTERPOLATE };
/* What a texel does (RAVE's kQATextureOp bits): modulate by kd (and add
 * ks), or decal (the texel over the colour by its alpha); 0 is the texel
 * alone. */
enum { R3D_TEXOP_MODULATE = 1, R3D_TEXOP_HIGHLIGHT = 2, R3D_TEXOP_DECAL = 4 };
enum { R3D_TRIANGLES, R3D_LINES, R3D_POINTS };

typedef struct {
    int zfunc;
    bool zwrite;
    int blend;
    int texture;      /* r3d_texture_new's, or 0 */
    int texop;
    bool linear;      /* bilinear filtering; else nearest */
    bool clamp_u, clamp_v;
    float size;       /* points' size in pixels */
} r3d_state;

typedef struct r3d_frame r3d_frame;

/* Whether the host can render (Metal on macOS; none on Linux yet). */
bool r3d_available(void);

r3d_frame *r3d_frame_new(int width, int height);
void r3d_frame_free(r3d_frame *f);
int r3d_frame_width(const r3d_frame *f);
int r3d_frame_height(const r3d_frame *f);
/* Start a frame: its colour buffer from pixels (width * height 32-bit
 * 0xAARRGGBB values, top row first) or, without them, cleared to
 * clear_argb; its depth buffer cleared to clear_z. */
void r3d_begin(r3d_frame *f, const uint32_t *pixels, uint32_t clear_argb, float clear_z);
/* Clear part of the frame mid-way (RAVE's clear calls): colour, depth or
 * both, x0 y0 x1 y1 in pixels. */
void r3d_clear(r3d_frame *f, int x0, int y0, int x1, int y1, bool color, uint32_t argb, bool depth, float z);
void r3d_draw(r3d_frame *f, const r3d_state *s, int primitive, const r3d_vertex *v, int count);
/* Finish the frame and read it into pixels (as r3d_begin's). */
void r3d_end(r3d_frame *f, uint32_t *pixels);

/* Mid-frame: what has been drawn so far, its colours into pixels and its
 * depths (0 to 1) into depth (either may be NULL), the frame going on. */
void r3d_read(r3d_frame *f, uint32_t *pixels, float *depth);
/* Mid-frame: the colours, or the depths, replaced (either may be NULL),
 * for what is drawn after. */
void r3d_write(r3d_frame *f, const uint32_t *pixels, const float *depth);

/* A texture from width * height 0xAARRGGBB pixels, top row first; 0 if
 * it can't be made. */
int r3d_texture_new(int width, int height, const uint32_t *pixels, bool mipmap);
void r3d_texture_free(int texture);

#endif
