/*
 * The QuickTime Music Architecture's Tune Player ('tune') and Note
 * Allocator ('nota') components, playing through the host's General MIDI
 * synthesizer.
 *
 * A tune player is given a tune header (TuneSetHeader: the parts'
 * NoteRequests, as in a music track's description) and then sequences
 * (TuneQueue: events through an end marker, in the tune's time scale, 600 a
 * second unless set), which play one after another, eight at most queued.
 * qt_music.c turns header and sequence into MIDI; the player sends it a
 * quarter of a second ahead of the clock from the machine's safe points
 * (qt_tune_pump, from sound_pump), on a synthesizer of its own, and calls a
 * sequence's TuneCallBack there when it has played.
 *
 * The note allocator hands out note channels, each a MIDI channel on one
 * synthesizer shared by all of them; NAPlayNote and the controller calls are
 * played at once.  There is no instrument picker: NAPickInstrument keeps the
 * instrument it is given.
 *
 * Both components are registered with the Component Manager at the start of
 * each run (components_reset), so OpenDefaultComponent and FindNextComponent
 * find them like any other.  Calls arrive as ComponentParameters: flags,
 * parameter size, selector, then the arguments (all longs here), the last
 * one first, then the instance.
 */
#include <stdlib.h>
#include <string.h>

#include "m68k.h"
#include "qt_music.h"
#include "qt_tune.h"
#include "tb_internal.h"
#include "trap.h"

#define MAX_PLAYERS 16
#define MAX_NOTE_CHANNELS 32
#define QUEUE_DEPTH 8                     /* kTuneQueueDepth */
#define AHEAD_NS 250000000ull             /* MIDI sent this far ahead */
#define MAX_EVENTS_BYTES (4u << 20)       /* a sequence or header longer than this is not one */
#define TUNE_SYNTH 0x54550000             /* host synthesizer ids: 'TU' + player */
#define NA_SYNTH 0x4E410000               /* 'NA', the note allocator's */

/* Music errors: ComponentResults with the high bit set. */
#define MUSIC_ERR(e) ((int32_t)(0x80000000u | (0xFFFFu & (uint32_t)(e))))
enum {
    noErr_ = 0,
    paramErr_ = -50,
    illegalInstrumentErr = MUSIC_ERR(-2079),
    illegalNoteChannelErr = MUSIC_ERR(-2084),
    tunePlayerFullErr = MUSIC_ERR(-2086),
    tuneParseErr = MUSIC_ERR(-2087),
    badComponentInstance_ = (int32_t)0x80008001,
    badComponentSelector_ = (int32_t)0x80008002,
};

enum { /* TuneQueue's flags */
    kTuneDontClipNotes = 2,
    kTuneExcludeEdgeNotes = 4,
    kTuneLoopUntil = 16,
};

/* ---- Common ---- */

/* Argument i (0 the first after the instance) of a call whose arguments are longs. */
static uint32_t arg(gaddr params, int i)
{
    uint32_t size = rd8(params + 1);
    if (size < 4u * (uint32_t)(i + 1))
        return 0;
    return rd32(params + 4 + size - 4 * (uint32_t)(i + 1));
}

static uint8_t clamp7(int64_t v) { return (uint8_t)(v < 0 ? 0 : v > 127 ? 127 : v); }

/* The length in bytes of a run of events in guest memory, its end marker
 * included, and the time its rests add up to. */
static size_t events_len(gaddr p, size_t limit, int64_t *rests)
{
    size_t i = 0, n = limit / 4;
    int64_t t = 0;
    while (p && i < n) {
        uint32_t w = rd32(p + (uint32_t)i * 4);
        uint32_t lenbits = w >> 30;
        if (lenbits <= 1) {
            uint32_t type = w >> 29;
            i++;
            if (type == 0)
                t += w & 0xFFFFFF;
            else if (type == 3 && ((w >> 16) & 0xFF) == 0)
                break; /* the end marker */
        } else if (lenbits == 2) {
            i += 2;
        } else {
            uint32_t longs = w & 0xFFFF;
            if (longs < 2)
                break;
            i += longs;
        }
    }
    if (rests)
        *rests = t;
    return (i < n ? i : n) * 4;
}

/* General MIDI's instrument names, for tone descriptions. */
static const char *const gm_names[128] = {
    "Acoustic Grand Piano", "Bright Acoustic Piano", "Electric Grand Piano", "Honky-tonk Piano",
    "Electric Piano 1", "Electric Piano 2", "Harpsichord", "Clavinet", "Celesta", "Glockenspiel", "Music Box",
    "Vibraphone", "Marimba", "Xylophone", "Tubular Bells", "Dulcimer", "Drawbar Organ", "Percussive Organ",
    "Rock Organ", "Church Organ", "Reed Organ", "Accordion", "Harmonica", "Tango Accordion",
    "Acoustic Guitar (nylon)", "Acoustic Guitar (steel)", "Electric Guitar (jazz)", "Electric Guitar (clean)",
    "Electric Guitar (muted)", "Overdriven Guitar", "Distortion Guitar", "Guitar Harmonics", "Acoustic Bass",
    "Electric Bass (finger)", "Electric Bass (pick)", "Fretless Bass", "Slap Bass 1", "Slap Bass 2",
    "Synth Bass 1", "Synth Bass 2", "Violin", "Viola", "Cello", "Contrabass", "Tremolo Strings",
    "Pizzicato Strings", "Orchestral Harp", "Timpani", "String Ensemble 1", "String Ensemble 2",
    "Synth Strings 1", "Synth Strings 2", "Choir Aahs", "Voice Oohs", "Synth Voice", "Orchestra Hit", "Trumpet",
    "Trombone", "Tuba", "Muted Trumpet", "French Horn", "Brass Section", "Synth Brass 1", "Synth Brass 2",
    "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax", "Oboe", "English Horn", "Bassoon", "Clarinet",
    "Piccolo", "Flute", "Recorder", "Pan Flute", "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
    "Lead 1 (square)", "Lead 2 (sawtooth)", "Lead 3 (calliope)", "Lead 4 (chiff)", "Lead 5 (charang)",
    "Lead 6 (voice)", "Lead 7 (fifths)", "Lead 8 (bass + lead)", "Pad 1 (new age)", "Pad 2 (warm)",
    "Pad 3 (polysynth)", "Pad 4 (choir)", "Pad 5 (bowed)", "Pad 6 (metallic)", "Pad 7 (halo)", "Pad 8 (sweep)",
    "FX 1 (rain)", "FX 2 (soundtrack)", "FX 3 (crystal)", "FX 4 (atmosphere)", "FX 5 (brightness)",
    "FX 6 (goblins)", "FX 7 (echoes)", "FX 8 (sci-fi)", "Sitar", "Banjo", "Shamisen", "Koto", "Kalimba",
    "Bagpipe", "Fiddle", "Shanai", "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock", "Taiko Drum",
    "Melodic Tom", "Synth Drum", "Reverse Cymbal", "Guitar Fret Noise", "Breath Noise", "Seashore", "Bird Tweet",
    "Telephone Ring", "Helicopter", "Applause", "Gunshot",
};

static bool is_drum_kit(int32_t n) { return n >= 0x4000 && n <= 0x4080; }

/* An instrument number's General MIDI program (GS ones by their program). */
static int program_of(int32_t inst) { return inst >= 1 ? (int)((inst - 1) & 127) : 0; }

/* Fill a ToneDescription (76 bytes) for an instrument number. */
static void stuff_tone(gaddr td, int32_t inst)
{
    if (!td)
        return;
    gmemset(td, 0, 76);
    const char *name = is_drum_kit(inst) ? "Standard Kit" : gm_names[program_of(inst)];
    size_t len = strlen(name);
    wr8(td + 36, (uint8_t)len);
    gput(td + 37, name, (uint32_t)len);
    wr32(td + 68, (uint32_t)inst);
    wr32(td + 72, (uint32_t)inst);
}

/* ---- Tune Player ---- */

typedef struct {
    gaddr tune;
    int32_t rate;                  /* Fixed */
    uint32_t flags;
    gaddr callback;
    int32_t refcon;
    uint32_t a5;
    qt_midi_event *ev;             /* in tune time, from first to end */
    size_t n, next;
    int64_t first, end;
} segment;

typedef struct {
    bool used;
    uint32_t ci;
    int id;                        /* the host synthesizer */
    uint8_t *header;
    size_t header_len;
    int32_t scale, volume;         /* a second; Fixed */
    int32_t softer;
    segment q[QUEUE_DEPTH];        /* q[0] playing */
    int nq;
    uint64_t start_ns;             /* when q[0]'s first moment plays */
    uint64_t last_end_ns;          /* when the last sequence ended */
    gaddr status;                  /* a TuneStatus for callbacks */
    uint32_t na;                   /* its note allocator, once asked for */
} player;

static player players[MAX_PLAYERS];
static uint32_t tune_component, na_component;
static bool pumping;

static player *player_of(uint32_t ci)
{
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (players[i].used && players[i].ci == ci)
            return &players[i];
    return NULL;
}

static uint64_t at_ns(const player *p, int64_t t)
{
    const segment *s = &p->q[0];
    double secs = (double)(t - s->first) * 65536.0 / ((double)p->scale * (double)s->rate);
    return p->start_ns + (uint64_t)(secs > 0 ? secs * 1e9 : 0);
}

/* The tune's time now, while q[0] plays. */
static int64_t tune_time(const player *p, uint64_t now)
{
    const segment *s = &p->q[0];
    if (now <= p->start_ns)
        return s->first;
    double t = (double)(now - p->start_ns) / 1e9 * (double)p->scale * (double)s->rate / 65536.0;
    int64_t v = s->first + (int64_t)t;
    return v < s->end ? v : s->end;
}

static void free_queue(player *p)
{
    for (int i = 0; i < p->nq; i++)
        free(p->q[i].ev);
    p->nq = 0;
}

/* A queued sequence's MIDI, between its start and stop positions. */
static int32_t prepare(player *p, segment *s, uint32_t start, uint32_t stop)
{
    int64_t rests;
    size_t len = events_len(s->tune, MAX_EVENTS_BYTES, &rests);
    if (!len)
        return tuneParseErr;
    uint8_t *buf = malloc(len);
    if (!buf)
        return memFullErr;
    gget(buf, s->tune, (uint32_t)len);
    qt_music_sample sample = { buf, len, 0 };
    size_t n = 0;
    qt_midi_event *ev = qt_music_events(p->header, p->header_len, &sample, 1, &n);
    free(buf);
    int64_t last = 0;
    for (size_t i = 0; i < n; i++)
        if (ev[i].time > last)
            last = ev[i].time;
    int64_t end = rests > 0 ? rests : last;
    if ((s->flags & kTuneDontClipNotes) && last > end)
        end = last;
    if (stop > start && (int64_t)stop < end)
        end = stop;
    int64_t first = (int64_t)start < end ? (int64_t)start : end;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        qt_midi_event e = ev[i];
        int kind = e.status & 0xF0;
        bool note = kind == 0x80 || kind == 0x90;
        if (e.time < first) { /* before the start: the parts' settings only */
            if (note)
                continue;
            e.time = first;
        } else if (e.time > end || (e.time == end && kind == 0x90 && (s->flags & kTuneExcludeEdgeNotes))) {
            if (kind != 0x80)
                continue;
            e.time = end; /* notes clipped at the end */
        }
        ev[k++] = e;
    }
    s->ev = ev;
    s->n = k;
    s->next = 0;
    s->first = first;
    s->end = end;
    return noErr_;
}

/* Send what plays before AHEAD_NS from now. */
static void feed(player *p, uint64_t now)
{
    segment *s = &p->q[0];
    machine_midi_event out[128];
    int k = 0;
    while (s->next < s->n) {
        const qt_midi_event *e = &s->ev[s->next];
        uint64_t when = at_ns(p, e->time);
        if (when > now + AHEAD_NS)
            break;
        uint8_t d2 = e->data2;
        if ((e->status & 0xF0) == 0x90) {
            int64_t v = (int64_t)d2 * p->volume / 0x10000;
            d2 = (uint8_t)(v < 1 ? 1 : v > 127 ? 127 : v);
        }
        out[k++] = (machine_midi_event){ when > now ? (int32_t)((when - now) / 1000000) : 0, e->status, e->data1, d2 };
        s->next++;
        if (k == 128) {
            sound_host_midi(p->id, out, k);
            k = 0;
        }
    }
    if (k)
        sound_host_midi(p->id, out, k);
}

/* TuneStatus: tune, tunePtr, time, queueCount, queueSpots, queueTime, reserved[3]. */
static void fill_status(const player *p, gaddr st, uint64_t now)
{
    if (!st)
        return;
    gmemset(st, 0, 28);
    wr16(st + 14, QUEUE_DEPTH - p->nq);
    if (!p->nq)
        return;
    const segment *s = &p->q[0];
    int64_t t = tune_time(p, now);
    int64_t queued = s->end - t;
    for (int i = 1; i < p->nq; i++)
        queued += p->q[i].end - p->q[i].first;
    wr32(st, s->tune);
    wr32(st + 4, s->tune);
    wr32(st + 8, (uint32_t)t);
    wr16(st + 12, (uint16_t)p->nq);
    wr32(st + 16, (uint32_t)queued);
}

/* TuneCallBack(const TuneStatus *status, long refCon), pascal. */
static void call_back(player *p, const segment *done, uint64_t now)
{
    fill_status(p, p->status, now);
    if (p->status) {
        wr32(p->status, done->tune);
        wr32(p->status + 4, done->tune);
        wr32(p->status + 8, (uint32_t)done->end);
    }
    cpu68k *c = g_cpu;
    uint32_t saved_a5 = c->a[5];
    if (done->a5)
        c->a[5] = done->a5;
    cpu_push32(c, p->status);
    cpu_push32(c, (uint32_t)done->refcon);
    guest_call(done->callback);
    c->a[5] = saved_a5;
}

/* A sequence after another starts where it ended, unless that was a while ago. */
static uint64_t chained_start(uint64_t end_ns, uint64_t now)
{
    return end_ns + 50000000ull >= now ? end_ns : now;
}

static void pump_player(player *p, uint64_t now)
{
    while (p->used && p->nq) {
        feed(p, now);
        segment *s = &p->q[0];
        uint64_t end_ns = at_ns(p, s->end);
        if (now < end_ns)
            return;
        if ((s->flags & kTuneLoopUntil) && p->nq == 1 && s->end > s->first) {
            s->next = 0;
            p->start_ns = chained_start(end_ns, now);
            continue;
        }
        segment done = *s;
        memmove(&p->q[0], &p->q[1], sizeof p->q[0] * (size_t)(p->nq - 1));
        p->nq--;
        free(done.ev);
        p->last_end_ns = end_ns;
        p->start_ns = chained_start(end_ns, now);
        if (done.callback)
            call_back(p, &done, now);
    }
}

void qt_tune_pump(void)
{
    if (pumping)
        return;
    pumping = true;
    uint64_t now = machine_time_ns();
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (players[i].used && players[i].nq)
            pump_player(&players[i], now);
    pumping = false;
}

static int32_t tune_queue(player *p, gaddr params)
{
    if (p->nq == QUEUE_DEPTH)
        return tunePlayerFullErr;
    segment s = { 0 };
    s.tune = arg(params, 0);
    s.rate = (int32_t)arg(params, 1);
    if (s.rate <= 0)
        s.rate = 0x10000;
    uint32_t start = arg(params, 2), stop = arg(params, 3);
    s.flags = arg(params, 4);
    s.callback = arg(params, 5);
    s.refcon = (int32_t)arg(params, 6);
    s.a5 = g_cpu->a[5];
    int32_t r = prepare(p, &s, start, stop);
    if (r)
        return r;
    uint64_t now = machine_time_ns();
    p->q[p->nq++] = s;
    if (p->nq == 1) {
        p->start_ns = p->last_end_ns ? chained_start(p->last_end_ns, now) : now;
        feed(p, now);
    }
    return noErr_;
}

static void tune_stop(player *p)
{
    if (p->nq)
        sound_host_midi(p->id, NULL, 0);
    free_queue(p);
    p->last_end_ns = 0;
}

/* TuneInstant: the tune as it sounds at one moment, held. */
static int32_t tune_instant(player *p, gaddr tune, uint32_t pos)
{
    segment s = { .tune = tune, .rate = 0x10000, .flags = kTuneDontClipNotes };
    int32_t r = prepare(p, &s, 0, 0);
    if (r)
        return r;
    sound_host_midi(p->id, NULL, 0);
    machine_midi_event out[128];
    int k = 0;
    for (size_t i = 0; i < s.n && k < 128; i++) {
        qt_midi_event e = s.ev[i];
        if (e.time > (int64_t)pos)
            break;
        int kind = e.status & 0xF0;
        if (kind == 0x90) { /* sounding: on now, and not off by pos */
            bool off = false;
            for (size_t j = i + 1; j < s.n && !off; j++)
                off = s.ev[j].status == (0x80 | (e.status & 15)) && s.ev[j].data1 == e.data1 &&
                      s.ev[j].time <= (int64_t)pos;
            if (off)
                continue;
        } else if (kind == 0x80) {
            continue;
        }
        out[k++] = (machine_midi_event){ 0, e.status, e.data1, e.data2 };
    }
    if (k)
        sound_host_midi(p->id, out, k);
    free(s.ev);
    return noErr_;
}

static int32_t tune_set_header(player *p, gaddr header, uint32_t size)
{
    size_t len = events_len(header, size ? size : MAX_EVENTS_BYTES, NULL);
    if (!len)
        return tuneParseErr;
    uint8_t *h = malloc(len);
    if (!h)
        return memFullErr;
    gget(h, header, (uint32_t)len);
    free(p->header);
    p->header = h;
    p->header_len = len;
    return noErr_;
}

static bool tune_can_do(int16_t sel)
{
    switch (sel) {
    case -1: case -2: case -3: case -4:
    case 4: case 5: case 6: case 7: case 8: case 10: case 11: case 12: case 13: case 16: case 17: case 18:
    case 19: case 20: case 21: case 23: case 24: case 25: case 26: case 27: case 28: case 29: case 30:
        return true;
    default:
        return false;
    }
}

static int32_t tune_open(uint32_t ci)
{
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!players[i].used) {
            player *p = &players[i];
            memset(p, 0, sizeof *p);
            p->used = true;
            p->ci = ci;
            p->id = TUNE_SYNTH + i;
            p->scale = 600;
            p->volume = 0x10000;
            p->status = tb_NewPtr(28, true, true);
            return noErr_;
        }
    }
    return memFullErr;
}

static void tune_close(player *p)
{
    tune_stop(p);
    free(p->header);
    if (p->status)
        tb_DisposePtr(p->status);
    if (p->na)
        components_close(p->na);
    memset(p, 0, sizeof *p);
}

/* pascal ComponentResult (ComponentParameters *params, Handle storage) */
static void tune_entry(cpu68k *c, uint16_t tw)
{
    (void)tw;
    uint32_t ret = cpu_pop32(c);
    cpu_pop32(c); /* storage */
    gaddr params = cpu_pop32(c);
    int16_t what = (int16_t)rd16(params + 2);
    uint32_t size = rd8(params + 1);
    int32_t r = noErr_;
    LOGF(LOG_TRAPS, "  TunePlayer selector %d\n", what);
    if (what == -1) { /* open */
        r = tune_open(rd32(params + 4));
    } else if (what == -2) { /* close */
        player *p = player_of(rd32(params + 4));
        if (p)
            tune_close(p);
    } else if (what == -3) { /* can do */
        r = tune_can_do((int16_t)rd16(params + 4));
    } else if (what == -4) { /* version */
        r = 0x00030000;
    } else if (what < 0) {
        r = badComponentSelector_;
    } else {
        player *p = player_of(rd32(params + 4 + size));
        if (!p) {
            r = badComponentInstance_;
        } else {
            switch (what) {
            case 4: r = tune_set_header(p, arg(params, 0), 0); break;          /* TuneSetHeader */
            case 28: r = tune_set_header(p, arg(params, 0), arg(params, 1)); break; /* TuneSetHeaderWithSize */
            case 5: if (arg(params, 0)) wr32(arg(params, 0), 0); break;       /* TuneGetTimeBase */
            case 6: p->scale = (int32_t)arg(params, 0) > 0 ? (int32_t)arg(params, 0) : 600; break;
            case 7: if (arg(params, 0)) wr32(arg(params, 0), (uint32_t)p->scale); break;
            case 8: if (arg(params, 1)) wr32(arg(params, 1), 0); break;       /* TuneGetIndexedNoteChannel */
            case 10: r = tune_queue(p, params); break;
            case 11: r = tune_instant(p, arg(params, 0), arg(params, 1)); break;
            case 12: /* TuneGetStatus */
                qt_tune_pump();
                fill_status(p, arg(params, 0), machine_time_ns());
                break;
            case 13: tune_stop(p); break;
            case 16: p->volume = (int32_t)arg(params, 0) < 0 ? 0 : (int32_t)arg(params, 0); break;
            case 17: r = p->volume; break;
            case 24: p->softer = (int32_t)arg(params, 0); break;
            case 25: qt_tune_pump(); break;                                   /* TuneTask */
            case 23: /* TuneGetNoteAllocator */
                if (!p->na && na_component)
                    p->na = components_open(na_component);
                r = (int32_t)p->na;
                break;
            case 30: /* TuneGetPartMix(part, &volume, &balance, &flags) */
                if (arg(params, 1)) wr32(arg(params, 1), 0x10000);
                if (arg(params, 2)) wr32(arg(params, 2), 0);
                if (arg(params, 3)) wr32(arg(params, 3), 0);
                break;
            case 18: case 19: case 20: case 21: case 26: case 27: case 29:
                break; /* preroll, unroll, note channels, transpose, balance, localization, part mix */
            default: r = badComponentSelector_; break;
            }
        }
    }
    wr32(c->a[7], (uint32_t)r);
    cpu_push32(c, ret);
}

/* ---- Note Allocator ---- */

typedef struct {
    bool used;
    gaddr nc;                      /* the NoteChannel handed out */
    uint32_t owner;                /* the allocator instance */
    int channel;                   /* MIDI channel */
    int32_t instrument;
    int32_t volume;                /* Fixed */
    int32_t controllers[128];
    uint8_t request[84];
} note_channel;

static note_channel channels[MAX_NOTE_CHANNELS];

static note_channel *channel_of(uint32_t nc)
{
    for (int i = 0; nc && i < MAX_NOTE_CHANNELS; i++)
        if (channels[i].used && channels[i].nc == nc)
            return &channels[i];
    return NULL;
}

static void na_send(uint8_t status, uint8_t d1, uint8_t d2)
{
    machine_midi_event e = { 0, status, d1, d2 };
    sound_host_midi(NA_SYNTH, &e, 1);
}

/* A MIDI channel for a new note channel: drums on 10, others the first free. */
static int free_midi_channel(bool drums)
{
    if (drums)
        return 9;
    for (int ch = 0; ch < 16; ch++) {
        bool taken = ch == 9;
        for (int i = 0; i < MAX_NOTE_CHANNELS && !taken; i++)
            taken = channels[i].used && channels[i].channel == ch;
        if (!taken)
            return ch;
    }
    return 15;
}

static void set_instrument(note_channel *n, int32_t inst)
{
    bool drums = is_drum_kit(inst);
    if (n->channel < 0 || drums != (n->channel == 9))
        n->channel = free_midi_channel(drums);
    n->instrument = inst;
    if (!drums)
        na_send((uint8_t)(0xC0 | n->channel), (uint8_t)program_of(inst), 0);
}

/* NANewNoteChannel(na, NoteRequest *, NoteChannel *): the NoteRequest's
 * tone, its General MIDI number first. */
static int32_t na_new_channel(uint32_t ci, gaddr request, gaddr out)
{
    int i = 0;
    while (i < MAX_NOTE_CHANNELS && channels[i].used)
        i++;
    if (i == MAX_NOTE_CHANNELS)
        return MUSIC_ERR(-2074); /* illegalVoiceAllocationErr */
    gaddr nc = tb_NewPtr(16, true, true);
    if (!nc)
        return memFullErr;
    note_channel *n = &channels[i];
    memset(n, 0, sizeof *n);
    n->used = true;
    n->nc = nc;
    n->owner = ci;
    n->volume = 0x10000;
    n->controllers[7] = 100 << 8;
    int32_t inst = 1;
    if (request) {
        gget(n->request, request, 84);
        int32_t ti = (int32_t)rd32(request + 76), gm = (int32_t)rd32(request + 80);
        inst = (gm >= 1 && gm <= 128) || is_drum_kit(gm) ? gm : ti >= 1 ? ti : 1;
    }
    n->channel = -1;
    set_instrument(n, inst);
    na_send((uint8_t)(0xB0 | n->channel), 121, 0); /* controllers reset */
    if (out)
        wr32(out, nc);
    return noErr_;
}

static void na_dispose(note_channel *n)
{
    na_send((uint8_t)(0xB0 | n->channel), 123, 0); /* all notes off */
    tb_DisposePtr(n->nc);
    n->used = false;
}

static void na_close_instance(uint32_t ci)
{
    for (int i = 0; i < MAX_NOTE_CHANNELS; i++)
        if (channels[i].used && channels[i].owner == ci)
            na_dispose(&channels[i]);
}

static int32_t na_play_note(note_channel *n, int32_t pitch, int32_t velocity)
{
    if (pitch > 127)
        pitch = (pitch + 128) >> 8; /* 8.8 fixed: the nearest note */
    if (pitch < 0 || pitch > 127)
        return noErr_;
    if (velocity <= 0) {
        na_send((uint8_t)(0x80 | n->channel), (uint8_t)pitch, 0);
    } else {
        int64_t v = (int64_t)velocity * n->volume / 0x10000;
        na_send((uint8_t)(0x90 | n->channel), (uint8_t)pitch, (uint8_t)(v < 1 ? 1 : v > 127 ? 127 : v));
    }
    return noErr_;
}

/* NASendMIDI: the packet's channel messages, on the note channel's channel. */
static void na_send_midi(note_channel *n, gaddr packet)
{
    if (!packet)
        return;
    uint32_t len = rd16(packet);
    if (len > 249)
        len = 249;
    gaddr d = packet + 6;
    for (uint32_t i = 0; i < len;) {
        uint8_t st = rd8(d + i);
        if (st < 0x80 || st >= 0xF0) {
            i++;
            continue;
        }
        uint32_t need = (st & 0xE0) == 0xC0 ? 1 : 2;
        if (i + need >= len)
            break;
        uint8_t d1 = rd8(d + i + 1), d2 = need == 2 ? rd8(d + i + 2) : 0;
        na_send((uint8_t)((st & 0xF0) | n->channel), d1, d2);
        i += 1 + need;
    }
}

static bool na_can_do(int16_t sel)
{
    switch (sel) {
    case -1: case -2: case -3: case -4:
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8: case 11: case 12: case 13: case 14:
    case 15: case 16: case 17: case 18: case 19: case 21: case 22: case 25: case 26: case 27: case 28: case 31:
    case 33: case 34: case 35: case 36: case 37: case 38: case 40: case 41: case 42: case 43: case 44: case 45:
        return true;
    default:
        return false;
    }
}

static void na_entry(cpu68k *c, uint16_t tw)
{
    (void)tw;
    uint32_t ret = cpu_pop32(c);
    cpu_pop32(c); /* storage */
    gaddr params = cpu_pop32(c);
    int16_t what = (int16_t)rd16(params + 2);
    uint32_t size = rd8(params + 1);
    int32_t r = noErr_;
    LOGF(LOG_TRAPS, "  NoteAllocator selector %d\n", what);
    if (what == -1) {
        r = noErr_;
    } else if (what == -2) {
        na_close_instance(rd32(params + 4));
    } else if (what == -3) {
        r = na_can_do((int16_t)rd16(params + 4));
    } else if (what == -4) {
        r = 0x00030000;
    } else if (what < 0) {
        r = badComponentSelector_;
    } else {
        uint32_t ci = rd32(params + 4 + size);
        /* the calls on a note channel name it first */
        note_channel *n = NULL;
        switch (what) {
        case 5: case 6: case 7: case 8: case 11: case 12: case 13: case 14: case 15: case 16: case 17: case 34:
        case 35: case 38: case 40: case 42: case 43: case 44: case 45:
            n = channel_of(arg(params, 0));
            if (!n) {
                r = illegalNoteChannelErr;
                goto done;
            }
            break;
        }
        switch (what) {
        case 4: r = na_new_channel(ci, arg(params, 0), arg(params, 1)); break;
        case 37: r = na_new_channel(ci, 0, arg(params, 2)); break; /* ...FromAtomicInstrument */
        case 5: na_dispose(n); break;
        case 6: /* NAGetNoteChannelInfo(nc, &index, &part) */
            if (arg(params, 1)) wr32(arg(params, 1), (uint32_t)(n - channels) + 1);
            if (arg(params, 2)) wr32(arg(params, 2), (uint32_t)n->channel + 1);
            break;
        case 11: n->volume = (int32_t)arg(params, 1) < 0 ? 0 : (int32_t)arg(params, 1); break;
        case 12: /* NAResetNoteChannel */
            na_send((uint8_t)(0xB0 | n->channel), 123, 0);
            na_send((uint8_t)(0xB0 | n->channel), 121, 0);
            break;
        case 13: r = na_play_note(n, (int32_t)arg(params, 1), (int32_t)arg(params, 2)); break;
        case 14: { /* NASetController(nc, controller, value) */
            uint32_t ctl = arg(params, 1);
            int16_t v = (int16_t)arg(params, 2);
            uint8_t m[3];
            if (ctl < 128)
                n->controllers[ctl] = (int32_t)arg(params, 2);
            if (qt_music_controller(n->channel, ctl, v, m))
                na_send(m[0], m[1], m[2]);
            break;
        }
        case 45: /* NAGetController(nc, controller, &value) */
            if (arg(params, 2))
                wr32(arg(params, 2), arg(params, 1) < 128 ? (uint32_t)n->controllers[arg(params, 1)] : 0);
            break;
        case 42: { /* NASetNoteChannelBalance(nc, balance): -128 left to 127 right */
            int32_t b = (int32_t)arg(params, 1);
            na_send((uint8_t)(0xB0 | n->channel), 10, clamp7(64 + b / 2));
            break;
        }
        case 17: case 43: /* NASetInstrumentNumber(nc, number) */
            if ((int32_t)arg(params, 1) < 1)
                r = illegalInstrumentErr;
            else
                set_instrument(n, (int32_t)arg(params, 1));
            break;
        case 16: /* NAFindNoteChannelTone(nc, td, &number) */
            stuff_tone(arg(params, 1), n->instrument);
            if (arg(params, 2)) wr32(arg(params, 2), (uint32_t)n->instrument);
            break;
        case 27: { /* NAStuffToneDescription(gmNumber, td) */
            int32_t gm = (int32_t)arg(params, 0);
            if (gm < 1)
                gm = 1;
            stuff_tone(arg(params, 1), gm);
            break;
        }
        case 34: /* NAGetNoteRequest(nc, nrOut) */
            if (arg(params, 1)) gput(arg(params, 1), n->request, 84);
            break;
        case 35: na_send_midi(n, arg(params, 1)); break;
        case 40: if (arg(params, 2)) wr32(arg(params, 2), 0); break; /* NAGetKnob */
        case 31: { /* NAGetIndNoteChannel(index, &nc, &seed) */
            int32_t k = (int32_t)arg(params, 0);
            r = illegalNoteChannelErr;
            for (int i = 0; i < MAX_NOTE_CHANNELS; i++) {
                if (channels[i].used && channels[i].owner == ci && --k == 0) {
                    if (arg(params, 1)) wr32(arg(params, 1), channels[i].nc);
                    if (arg(params, 2)) wr32(arg(params, 2), 1);
                    r = noErr_;
                    break;
                }
            }
            break;
        }
        case 2: { /* NAGetRegisteredMusicDevice(index, &type, name, &connections, &mc) */
            if (arg(params, 0) != 1) {
                r = paramErr_;
                break;
            }
            static const char name[] = "QuickTime Music Synthesizer";
            if (arg(params, 1)) wr32(arg(params, 1), fourcc("ss  "));
            if (arg(params, 2)) {
                wr8(arg(params, 2), sizeof name - 1);
                gput(arg(params, 2) + 1, name, sizeof name - 1);
            }
            if (arg(params, 3)) gmemset(arg(params, 3), 0, 32);
            if (arg(params, 4)) wr32(arg(params, 4), 0);
            break;
        }
        case 22: if (arg(params, 0)) gmemset(arg(params, 0), 0, 32); break; /* NAGetDefaultMIDIInput */
        case 33: { /* NAGetMIDIPorts(&in, &out): none */
            for (int k = 0; k < 2; k++) {
                if (arg(params, k)) {
                    gaddr h = tb_NewHandle(2, false, true);
                    wr32(arg(params, k), h);
                }
            }
            break;
        }
        case 0: case 1: case 3: case 7: case 8: case 15: case 18: case 19: case 21: case 25: case 26: case 28:
        case 36: case 38: case 41: case 44:
            /* registering devices, preroll, unroll, knobs, the pickers and dialogs (which keep what they
             * are given), MIDI input, the atomic instrument, the task, localization */
            break;
        default: r = badComponentSelector_; break;
        }
    }
done:
    wr32(c->a[7], (uint32_t)r);
    cpu_push32(c, ret);
}

/* ---- Registration ---- */

void qt_tune_register(void)
{
    memset(players, 0, sizeof players);
    memset(channels, 0, sizeof channels);
    pumping = false;
    tune_component = components_register_native(fourcc("tune"), 0, fourcc("appl"),
                                                native_register(tune_entry, "TunePlayer", 0));
    na_component = components_register_native(fourcc("nota"), 0, fourcc("appl"),
                                              native_register(na_entry, "NoteAllocator", 0));
}
