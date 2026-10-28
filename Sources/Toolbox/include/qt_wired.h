/* Wired sprites: actions run on a sprite track's events (qt_wired.c). */
#ifndef QT_WIRED_H
#define QT_WIRED_H

#include <stdbool.h>
#include <stdint.h>

#include "mem.h"
#include "qd_internal.h"
#include "qt_model.h"

struct qt_native;
struct qt_sprite;

/* A click in the movie's box (its port's coordinates): the front-most
 * sprite with handlers there runs its mouse down and up actions; false if
 * there was none, so the controller does what it would. */
bool qt_wired_click(qtm_movie *mv, gaddr movie, struct qt_native *native, qrect box, int h, int v);
/* A movie's share of MoviesTask: frame-loaded handlers of samples shown,
 * the mouse entering, moving over and leaving sprites, idle events. */
void qt_wired_task(qtm_movie *mv, gaddr movie, struct qt_native *native, gaddr port, qrect box);
/* One sprite's handlers for an event (0: the track's own). */
bool qt_wired_sprite_event(qtm_movie *mv, gaddr movie, struct qt_native *native, int track, struct qt_sprite *s,
                           uint32_t sprite, uint32_t event);

#endif
