/*
 * QuickTime music tracks as MIDI: the QuickTime Music Architecture's
 * events, turned into note, controller and program events for the host's
 * General MIDI synthesizer.
 *
 * The 'musi' sample description holds the tune header: a general event per
 * part, each a NoteRequest naming the part's instrument (its General MIDI
 * number, or a drum kit).  Samples hold the tune's events, big-endian longs:
 * the top bits of a first long give the event's length (00/01 one long, 10
 * two, 11 a general event of the length in its low 16 bits) and its type:
 *   rest (000)      duration in the low 24 bits: the tune's time moves on
 *   note (001)      part 24-28, pitch 18-23 (+32), velocity 11-17, duration 0-10
 *   control (010)   part 24-28, controller 16-23, value 0-15 (8.8 fixed)
 *   marker (011)    subtype 16-23 and value: subtype 0 ends the tune
 *   extended note (1001) part 16-27 and pitch 0-15 (MIDI, or 8.8 above 127);
 *                   then velocity 22-28 and duration 0-21
 *   extended control (1010) part and value 0-15; then the controller 0-15
 *   knob (1011), general (1111)
 * Notes start where the tune's time is and don't move it; durations are in
 * the media's time units.  (Field positions from QuickTimeMusic.h.)
 */
#include <stdlib.h>
#include <string.h>

#include "qt_music.h"

enum { MAX_PARTS = 64 };

typedef struct {
    bool used, drums;
    int channel;
    int program; /* 0-127 */
} part;

typedef struct {
    part parts[MAX_PARTS];
    int next_channel;
    qt_midi_event *ev;
    size_t n, cap;
    bool ended;
} tune;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t ext(uint32_t v, int pos, int width) { return (v >> pos) & ((1u << width) - 1); }

static void emit(tune *t, int64_t at, uint8_t status, uint8_t d1, uint8_t d2, uint8_t order)
{
    if (t->n == t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 256;
        qt_midi_event *e = realloc(t->ev, cap * sizeof *e);
        if (!e)
            return;
        t->ev = e;
        t->cap = cap;
    }
    t->ev[t->n++] = (qt_midi_event){ at, status, d1, d2, order };
}

/* The part's channel: drums on MIDI channel 10, others in turn. */
static part *part_of(tune *t, uint32_t index)
{
    part *p = &t->parts[index % MAX_PARTS];
    if (!p->used) {
        p->used = true;
        p->channel = -1;
    }
    if (p->channel < 0) {
        if (p->drums) {
            p->channel = 9;
        } else {
            if (t->next_channel == 9)
                t->next_channel++;
            p->channel = t->next_channel < 16 ? t->next_channel++ : 15;
        }
    }
    return p;
}

/* A NoteRequest (info 8 bytes, then the ToneDescription: synthesizer type,
 * two Str31 names, instrument number, General MIDI number). */
static void note_request(tune *t, uint32_t index, const uint8_t *nr, size_t len, int64_t at, bool in_header)
{
    if (len < 84)
        return;
    int32_t inst = (int32_t)be32(nr + 76), gm = (int32_t)be32(nr + 80);
    part *p = &t->parts[index % MAX_PARTS];
    p->used = true;
    p->channel = -1;
    p->drums = (gm >= 0x4000 && gm <= 0x4080) || (inst >= 0x4000 && inst <= 0x4080);
    p->program = gm >= 1 && gm <= 128 ? gm - 1 : inst >= 1 && inst <= 128 ? inst - 1 : 0;
    part_of(t, index);
    if (!p->drums)
        emit(t, at, (uint8_t)(0xC0 | p->channel), (uint8_t)p->program, 0, 0);
    (void)in_header;
}

static uint8_t clamp7(int v) { return (uint8_t)(v < 0 ? 0 : v > 127 ? 127 : v); }

static void control(tune *t, uint32_t index, uint32_t controller, int16_t value, int64_t at)
{
    part *p = part_of(t, index);
    uint8_t st = (uint8_t)(0xB0 | p->channel);
    if (controller == 32) { /* pitch bend: semitones, 8.8; the synthesizer's range is two */
        int bend = 8192 + value * 8192 / (2 * 256);
        bend = bend < 0 ? 0 : bend > 16383 ? 16383 : bend;
        emit(t, at, (uint8_t)(0xE0 | p->channel), (uint8_t)(bend & 127), (uint8_t)(bend >> 7), 0);
    } else if (controller == 10) {
        /* pan: 0 the default, 1.0 left to 2.0 right; tunes converted from
         * MIDI files carry MIDI's 0-127 instead */
        int pan = value <= 0 ? 64 : value >= 256 && value <= 512 ? (value - 256) * 127 / 256 : value >> 8;
        emit(t, at, st, 10, clamp7(pan), 0);
    } else if (controller == 64) { /* sustain: on when positive */
        emit(t, at, st, 64, value > 0 ? 127 : 0, 0);
    } else if (controller < 128) {
        emit(t, at, st, (uint8_t)controller, clamp7(value >> 8), 0);
    }
}

static void note(tune *t, uint32_t index, int pitch, int velocity, int64_t at, int64_t duration)
{
    if (velocity <= 0 || pitch < 0 || pitch > 127)
        return;
    part *p = part_of(t, index);
    emit(t, at, (uint8_t)(0x90 | p->channel), (uint8_t)pitch, clamp7(velocity), 2);
    emit(t, at + (duration > 0 ? duration : 1), (uint8_t)(0x80 | p->channel), (uint8_t)pitch, 0, 1);
}

/* One run of events from media time at; returns the time at its end. */
static int64_t events(tune *t, const uint8_t *d, size_t len, int64_t at, bool header)
{
    size_t i = 0, n = len / 4;
    while (i < n && !t->ended) {
        uint32_t w = be32(d + i * 4);
        uint32_t lenbits = w >> 30;
        if (lenbits <= 1) {
            switch (ext(w, 29, 3)) {
            case 0: at += ext(w, 0, 24); break;
            case 1:
                note(t, ext(w, 24, 5), (int)ext(w, 18, 6) + 32, (int)ext(w, 11, 7), at, ext(w, 0, 11));
                break;
            case 2: control(t, ext(w, 24, 5), ext(w, 16, 8), (int16_t)ext(w, 0, 16), at); break;
            case 3:
                if (ext(w, 16, 8) == 0)
                    t->ended = true; /* the end marker */
                break;
            }
            i++;
        } else if (lenbits == 2) {
            if (i + 1 >= n)
                break;
            uint32_t w2 = be32(d + i * 4 + 4);
            switch (ext(w, 28, 4)) {
            case 9: {
                int pitch = (int)ext(w, 0, 16);
                if (pitch > 127)
                    pitch = (pitch + 128) >> 8; /* 8.8 fixed: the nearest note */
                note(t, ext(w, 16, 12), pitch, (int)ext(w2, 22, 7), at, ext(w2, 0, 22));
                break;
            }
            case 10: control(t, ext(w, 16, 12), ext(w2, 0, 16), (int16_t)ext(w, 0, 16), at); break;
            }
            i += 2;
        } else { /* general */
            uint32_t longs = ext(w, 0, 16);
            if (longs < 2 || i + longs > n)
                break;
            uint32_t tail = be32(d + (i + longs - 1) * 4);
            if (ext(tail, 16, 14) == 1) /* kGeneralEventNoteRequest */
                note_request(t, ext(w, 16, 12), d + (i + 1) * 4, (longs - 2) * 4, at, header);
            i += longs;
        }
    }
    return at;
}

qt_midi_event *qt_music_events(const uint8_t *header, size_t header_len, const qt_music_sample *samples,
                               size_t nsamples, size_t *count)
{
    tune t;
    memset(&t, 0, sizeof t);
    events(&t, header, header_len, 0, true);
    t.ended = false; /* the header ends with an end marker of its own */
    for (size_t s = 0; s < nsamples && !t.ended; s++)
        events(&t, samples[s].data, samples[s].len, samples[s].time, false);
    /* In time order; at one moment, programs and controllers, then notes
     * ending, then notes starting. */
    for (size_t i = 1; i < t.n; i++) {
        qt_midi_event e = t.ev[i];
        size_t j = i;
        while (j > 0 && (t.ev[j - 1].time > e.time || (t.ev[j - 1].time == e.time && t.ev[j - 1].order > e.order))) {
            t.ev[j] = t.ev[j - 1];
            j--;
        }
        t.ev[j] = e;
    }
    *count = t.n;
    return t.ev;
}
