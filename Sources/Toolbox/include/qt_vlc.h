/* Movie decoding through libvlc, loaded at run time (qt_vlc.c). */
#ifndef QT_VLC_H
#define QT_VLC_H

#include <stdbool.h>
#include <stdint.h>

typedef struct qt_vlc_player qt_vlc_player;

/* Whether libvlc could be loaded (tried once). */
bool qt_vlc_available(void);

/* A player for a movie file VLC can read; NULL without libvlc. */
qt_vlc_player *qt_vlc_open(const char *path);
void qt_vlc_close(qt_vlc_player *p);

void qt_vlc_play(qt_vlc_player *p);
void qt_vlc_pause(qt_vlc_player *p);
void qt_vlc_rewind(qt_vlc_player *p);
void qt_vlc_seek(qt_vlc_player *p, int64_t ms);
int64_t qt_vlc_time(qt_vlc_player *p);
bool qt_vlc_ended(qt_vlc_player *p);
void qt_vlc_set_rate(qt_vlc_player *p, float rate);
void qt_vlc_set_volume(qt_vlc_player *p, int percent);

/* The latest frame, if it is newer than *serial: copied to *rgba
 * (realloc'd, w*h*4 bytes), and *serial updated. */
bool qt_vlc_frame(qt_vlc_player *p, uint32_t *serial, uint8_t **rgba, int *w, int *h);

#endif
