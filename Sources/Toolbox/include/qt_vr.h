/* QuickTime VR: panoramas and object movies shown and navigated (qt_vr.c, qt_vr_lib.c). */
#ifndef QT_VR_H
#define QT_VR_H

#include <stdbool.h>
#include <stdint.h>

#include "mem.h"
#include "qd_internal.h"
#include "ppc_bridge.h"

/* Whether QuickTime VR's calls are there (for Gestalt 'qtvr'). */
bool qt_vr_available(void);

/* The Movie Toolbox's hooks.  A VR movie (one with a QTVR track, or a
 * QTVR 1.0 panorama) is drawn as its view, not its tracks' frames: true
 * when the movie is one (drawn if forced or changed). */
bool qt_vr_movie(gaddr movie);
bool qt_vr_movie_draw(gaddr movie, gaddr port, qrect box, bool force);
/* Time passes for it (MoviesTask): animation, and a changed view drawn. */
void qt_vr_movie_idle(gaddr movie);
/* The movie controller's click in the movie (a local point), keys and the
 * modifiers held: true if the movie is a VR movie and took it. */
bool qt_vr_movie_click(gaddr movie, gaddr mc, uint32_t pt, uint16_t modifiers);
bool qt_vr_movie_key(gaddr movie, uint8_t ch, uint16_t modifiers);
void qt_vr_movie_modifiers(gaddr movie, uint16_t modifiers);
/* The movie went away. */
void qt_vr_movie_gone(gaddr movie);
void qt_vr_reset(void);

/* QuickTimeVRLib's exports (qt_vr_lib.c). */
void (*qt_vr_export_fn(const char *name))(ppc_cpu *);

#endif
