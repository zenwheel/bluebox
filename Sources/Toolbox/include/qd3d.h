/*
 * QuickDraw 3D, its Viewer and RAVE (q3_*.c, rave.c), drawn by the
 * host's renderer (r3d.h).
 */
#ifndef QD3D_H
#define QD3D_H

#include <stdbool.h>

#include "ppc_bridge.h"

/* A QuickDraw 3D, Viewer, Custom Elements or RAVE export, or NULL. */
void (*qd3d_export_fn(const char *name))(ppc_cpu *);
/* Whether name is one of their libraries (there only where the host can
 * render: r3d_available). */
bool qd3d_library(const char *name);
/* Guest memory gone (a new machine): every object with it. */
void qd3d_reset(void);

#endif
