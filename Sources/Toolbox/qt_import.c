/*
 * Movie importers of our own: files that aren't movies (pictures, sounds,
 * MIDI, text) opened as movies, as QuickTime 3 did through its 'eat '
 * components.
 */
#include "qt_errors.h"
#include "qt_movie_internal.h"
#include "tb_internal.h"

int16_t qt_import_file_as_movie(const char *path, gaddr moviep, int16_t flags)
{
    (void)path, (void)flags;
    wr32(moviep, 0);
    return noMovieFound;
}
