/* Movies played by our own decoders (qt_native.c). */
#ifndef QT_NATIVE_H
#define QT_NATIVE_H

#include <stdbool.h>
#include <stdint.h>

#include "qt_music.h"

typedef struct qt_native qt_native;

/* A player for a movie from its whole 'moov' atom, its samples read from
 * data_path (chunk offsets are into that file), or NULL when a track needs
 * something we don't decode.  Palettized video without colours of its own
 * takes screen_palette (0x00RRGGBB) when screen_depth matches. */
qt_native *qt_native_open(const uint8_t *moov, uint32_t len, const char *data_path, int32_t movie_scale,
                          const uint32_t *screen_palette, int screen_depth);
/* The same with a reader for the samples: offset is the chunk offset as
 * the description has it (one with virtual offsets: data reference index
 * << 40 | offset). */
typedef bool (*qt_native_reader)(void *ctx, uint64_t offset, void *buf, uint32_t n);
qt_native *qt_native_open_reader(const uint8_t *moov, uint32_t len, qt_native_reader reader, void *ctx,
                                 int32_t movie_scale, const uint32_t *screen_palette, int screen_depth);
void qt_native_close(qt_native *p);
bool qt_native_has_video(const qt_native *p);
bool qt_native_has_sound(const qt_native *p);
bool qt_native_has_music(const qt_native *p);

/* The picture at movie time ms, if it differs from the one *serial names:
 * copied to *rgba (realloc'd, *w * *h * 4 bytes) and *serial updated. */
bool qt_native_frame(qt_native *p, int64_t ms, uint32_t *serial, uint8_t **rgba, int *w, int *h);
/* The sound of movie time [from_ms, to_ms) as 16-bit PCM (*pcm malloc'd,
 * interleaved); returns the frames.  Consecutive stretches join exactly. */
uint32_t qt_native_sound(qt_native *p, int64_t from_ms, int64_t to_ms, int16_t **pcm, int *channels, double *rate);

/* A music track's MIDI events in movie time [from_ms, to_ms) (their time
 * fields in ms): *first, and the count. */
size_t qt_native_music(qt_native *p, int64_t from_ms, int64_t to_ms, const qt_midi_event **first);

/* The media handlers' hold on what plays: a sprite track's sprites (its
 * place among the movie's tracks, 0-based), a track's picture made again
 * after they changed, and the sample showing (-1 none). */
struct qt_sprite;
struct qt_sprite *qt_native_sprite(qt_native *p, int track);
struct qt_text;
struct qt_text *qt_native_text(qt_native *p, int track);
void qt_native_redraw(qt_native *p, int track);
int64_t qt_native_showing(qt_native *p, int track);
void qt_native_text_override(qt_native *p, int track, int64_t sample, const uint8_t *bytes, uint32_t len, bool hidden);

#endif
