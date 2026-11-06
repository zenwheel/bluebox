/*
 * r3d.h's host side where the host has no renderer yet (Linux, until its
 * OpenGL one): nothing renders, and QuickDraw 3D's and RAVE's libraries
 * aren't there (cfm.c asks r3d_available).
 */
#ifndef __APPLE__
#include <stddef.h>

#include "r3d.h"

bool r3dh_available(void) { return false; }
bool r3dh_frame_make(int slot, int width, int height) { return false; }
void r3dh_frame_drop(int slot) {}
bool r3dh_frame_live(int slot) { return false; }
void r3dh_begin(int slot, const uint32_t *pixels, uint32_t clear_argb, float clear_z) {}
void r3dh_clear(int slot, int x0, int y0, int x1, int y1, bool color, uint32_t argb, bool depth, float z) {}
void r3dh_draw(int slot, const r3d_state *s, int primitive, const r3d_vertex *v, int count) {}
void r3dh_end(int slot, uint32_t *pixels) {}
void r3dh_read(int slot, uint32_t *pixels, float *depth) {}
void r3dh_write(int slot, const uint32_t *pixels, const float *depth) {}
bool r3dh_texture_make(int slot, int width, int height, const uint32_t *pixels, bool mipmap) { return false; }
void r3dh_texture_drop(int slot) {}
bool r3dh_texture_live(int slot) { return false; }
void r3dh_texture_read(int slot, uint32_t *pixels) {}
void r3dh_drop_all(void) {}
#endif
