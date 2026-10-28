/* What the Movie Toolbox's files share (quicktime.c, qt_movies.c, qt_mfile.c, ...). */
#ifndef QT_MOVIE_INTERNAL_H
#define QT_MOVIE_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "mem.h"
#include "qt_model.h"

/* quicktime.c: the records behind a Movie, Track or Media the program
 * holds (NULL for one that isn't); the Movie for records. */
qtm_movie *qt_movie_model(gaddr theMovie);
qtm_track *qt_track_model(gaddr theTrack, qtm_movie **mv);
qtm_media *qt_media_model(gaddr theMedia, qtm_track **t, qtm_movie **mv);
gaddr qt_movie_handle(const qtm_movie *mv);
/* The file a movie's own references name, and a new one (CreateMovieFile, flattening). */
const char *qt_movie_path(const qtm_movie *mv);
void qt_movie_set_path(qtm_movie *mv, const char *path);
/* The records changed: durations again, and the movie played from them. */
void qt_movie_touch(qtm_movie *mv);
int16_t qt_set_error(int16_t err);
/* New movies: from a host file (as NewMovieFromFile), from a 'moov' atom
 * or its contents (self references naming path), or for records (taken). */
int16_t qt_open_movie_path(const char *path, int16_t *resid, uint64_t offset, bool from_data, gaddr moviep,
                           int16_t flags);
int16_t qt_movie_from_bytes(const uint8_t *bytes, uint32_t len, const char *path, gaddr moviep, int16_t flags);
gaddr qt_new_movie_for(qtm_movie *mv, const char *path);
qtm_movie *qt_copy_model(const qtm_movie *mv);
uint8_t *qt_moov_uncompress(uint8_t *moov, uint32_t *len);
int64_t qt_movie_now(const qtm_movie *mv);
bool qt_movie_frame_at(const qtm_movie *mv, int64_t time, uint8_t **rgba, int *w, int *h);
bool qt_scrap_has_movie(void);
bool qt_any_movie_playing(void);

/* qt_movies.c: a movie going away; one movie's segment into another's
 * tracks; whether a media type has a characteristic; the program's
 * drawing-complete routine. */
void qt_movie_forget(qtm_movie *mv);
void qt_movies_reset(void);
int16_t qt_insert_movie_segment(qtm_movie *src, qtm_movie *dst, int64_t src_in, int64_t len, int64_t dst_in);
bool qt_media_has_characteristic(uint32_t media_type, uint32_t characteristic);
void qt_movie_drawing_complete(qtm_movie *mv);

/* qt_callbacks.c: time base callbacks checked (MoviesTask; at interrupt
 * time, from the Time Manager's pump); a time base's time set, or gone. */
void qt_callbacks_poll(void);
void qt_callbacks_periodic(void);
void qt_callbacks_jumped(gaddr tb);
void qt_callbacks_timebase_gone(gaddr tb);
void qt_callbacks_reset(void);

/* qt_util.c: a copy of a PixMap (in a GWorld of ours) and its disposal. */
gaddr qt_copy_pixmap(gaddr pm);
void qt_dispose_pixmap(gaddr pm);
void qt_task_needed_sooner(void);
void qt_util_reset(void);

/* qt_mfile.c: a reference movie's target ('rmra'), and a picture of pixels. */
bool qt_reference_movie_target(const uint8_t *moov, uint32_t len, uint32_t *type, uint8_t **ref, uint32_t *rn);
gaddr qt_picture_of_rgba(const uint8_t *rgba, int w, int h);

/* qt_import.c: a file that isn't a movie, through a movie importer of ours. */
int16_t qt_import_file_as_movie(const char *path, gaddr moviep, int16_t flags);

#endif
