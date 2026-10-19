/* Movie editing on a movie's description (qt_edit.c). */
#ifndef QT_EDIT_H
#define QT_EDIT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int64_t len;           /* in the movie's time units */
    int64_t media_time;    /* in the media's; -1: an empty stretch */
    int32_t rate;          /* Fixed */
} qt_edit;

typedef struct {
    int n;
    qt_edit *e;
    int32_t media_scale, movie_scale;
} qt_edit_track;

typedef struct {
    int32_t scale;         /* the movie's time scale */
    int ntracks;
    qt_edit_track *tracks;
} qt_edits;

/* Each track's edits from a whole 'moov' atom (version 0 headers). */
bool qt_edits_read(const uint8_t *moov, uint32_t len, qt_edits *out);
void qt_edits_free(qt_edits *e);
int64_t qt_edits_duration(const qt_edits *e);
int64_t qt_edits_track_duration(const qt_edit_track *t);

/* Times in movie units: from/at, len. */
bool qt_edits_delete(qt_edits *m, int64_t from, int64_t len);
bool qt_edits_insert_empty(qt_edits *m, int64_t at, int64_t len);
/* src's [from, from + len) (src's units) into dst at dst_at, track by
 * track; the movies show the same media (one movie's file). */
bool qt_edits_insert(qt_edits *dst, const qt_edits *src, int64_t src_from, int64_t src_len, int64_t dst_at);
bool qt_edits_copy(const qt_edits *src, int64_t from, int64_t len, qt_edits *out);

/* The 'moov' with each track's edit list and the durations replaced (malloc'd). */
uint8_t *qt_edits_write(const uint8_t *moov, uint32_t len, const qt_edits *e, uint32_t *out_len);

#endif
