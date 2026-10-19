/* QuickTime music tracks as MIDI events (qt_music.c). */
#ifndef QT_MUSIC_H
#define QT_MUSIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int64_t time;          /* in the media's time units */
    uint8_t status, data1, data2;
    uint8_t order;         /* within one moment: 0 programs and controllers, 1 notes off, 2 notes on */
} qt_midi_event;

typedef struct {
    const uint8_t *data;
    size_t len;
    int64_t time;          /* the sample's start, in media time */
} qt_music_sample;

/* The tune's events in time order, from its header (the events after the
 * 'musi' description's flags) and its samples; malloc'd, *count of them. */
qt_midi_event *qt_music_events(const uint8_t *header, size_t header_len, const qt_music_sample *samples,
                               size_t nsamples, size_t *count);

/* The MIDI message for a QuickTime controller (its value 8.8 fixed) on a
 * channel: 3, or 0 when there is none. */
int qt_music_controller(int channel, uint32_t controller, int16_t value, uint8_t msg[3]);

#endif
