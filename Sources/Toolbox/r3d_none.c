/*
 * r3d.h where the host has no renderer yet (Linux, until its OpenGL
 * one): nothing renders, and QuickDraw 3D's and RAVE's libraries aren't
 * there (cfm.c asks r3d_available).
 */
#ifndef __APPLE__
#include <stddef.h>

#include "r3d.h"

bool r3d_available(void) { return false; }
r3d_frame *r3d_frame_new(int width, int height) { return NULL; }
void r3d_frame_free(r3d_frame *f) {}
int r3d_frame_width(const r3d_frame *f) { return 0; }
int r3d_frame_height(const r3d_frame *f) { return 0; }
void r3d_begin(r3d_frame *f, const uint32_t *pixels, uint32_t clear_argb, float clear_z) {}
void r3d_clear(r3d_frame *f, int x0, int y0, int x1, int y1, bool color, uint32_t argb, bool depth, float z) {}
void r3d_draw(r3d_frame *f, const r3d_state *s, int primitive, const r3d_vertex *v, int count) {}
void r3d_end(r3d_frame *f, uint32_t *pixels) {}
int r3d_texture_new(int width, int height, const uint32_t *pixels, bool mipmap) { return 0; }
void r3d_texture_free(int texture) {}
#endif
