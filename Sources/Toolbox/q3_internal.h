/*
 * QuickDraw 3D and RAVE inside Blue Box: what their files share.  The
 * libraries (QuickDraw™ 3D, QD3DViewerLib, QuickDraw™ 3D Accelerator)
 * are PowerPC only; their calls come with the PowerPC calling convention
 * (integers and pointers in r3 on, floats in f1 on, each float also
 * taking a general register's slot).
 */
#ifndef Q3_INTERNAL_H
#define Q3_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "mem.h"
#include "ppc_bridge.h"
#include "qd_internal.h"
#include "r3d.h"

#define Q3R(n) (c->gpr[3 + (n)])
#define Q3RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; } while (0)

/* General-register slot s of the arguments (past r10 on the stack). */
static inline uint32_t q3_slot(ppc_cpu *c, int s) { return s < 8 ? c->gpr[3 + s] : rd32(c->gpr[1] + 24 + 4 * (uint32_t)s); }
/* Floating-point argument n (from 0). */
static inline float q3_farg(ppc_cpu *c, int n) { return n < 13 ? (float)ppc_fpr(c, 1 + n) : 0.0f; }
static inline void q3_return_float(ppc_cpu *c, float f)
{
    ppc_set_fpr(c, 1, (double)f);
    c->pc = c->lr;
}

static inline float rdf(gaddr a)
{
    uint32_t b = rd32(a);
    float f;
    memcpy(&f, &b, 4);
    return f;
}
static inline void wrf(gaddr a, float f)
{
    uint32_t b;
    memcpy(&b, &f, 4);
    wr32(a, b);
}

/* ---- Frames into guest pixels (q3_pixels.c) ---- */

/* 0xAARRGGBB pixels (width by height, top row first) into d at (x, y) of
 * its local coordinates, only where clip (local too; NULL: everywhere)
 * has them. */
void q3_put_pixels(const uint32_t *argb, int width, int height, const pixdst *d, int x, int y, const hrgn *clip);
/* The other way: what d has under the rectangle, as 0xFFRRGGBB pixels. */
void q3_get_pixels(uint32_t *argb, int width, int height, const pixdst *d, int x, int y);
/* Into a port at (x, y) of its local coordinates, through its clip and
 * visible region. */
void q3_put_port(const uint32_t *argb, int width, int height, gaddr port, int x, int y);
/* A pixel of another format as 0xAARRGGBB: QuickDraw 3D's and RAVE's
 * 16 and 32-bit pixel types, big-endian in guest memory. */
enum { Q3PX_RGB32, Q3PX_ARGB32, Q3PX_RGB16, Q3PX_ARGB16, Q3PX_RGB16_565, Q3PX_RGB24, Q3PX_ARGB16_4444 };
uint32_t q3_pixel_argb(int format, gaddr p);
int q3_pixel_bytes(int format);
/* Pixels from a guest image in one of those formats; malloc'd. */
uint32_t *q3_image_argb(gaddr image, int width, int height, uint32_t row_bytes, int format, bool flip);

/* ---- Exports ---- */
extern const ppc_export rave_exports[];
extern const unsigned rave_export_count;
extern const ppc_export q3_exports[];
extern const unsigned q3_export_count;
extern const ppc_export q3_math_exports[];
extern const unsigned q3_math_export_count;
extern const ppc_export q3_geom_exports[];
extern const unsigned q3_geom_export_count;
extern const ppc_export q3_view_exports[];
extern const unsigned q3_view_export_count;
extern const ppc_export q3_file_exports[];
extern const unsigned q3_file_export_count;
extern const ppc_export q3_viewer_exports[];
extern const unsigned q3_viewer_export_count;

#endif
