/*
 * Sound Manager.
 *
 * Each channel keeps its command queue here and plays sampled sound and
 * notes through the host (machine_set_sound_handler).  The queue is paced
 * by wall-clock time: a command that plays sound keeps the channel busy
 * for the sound's length, and later commands (callbacks included) run at
 * the next safe point after that — the Event Manager calls, TickCount and
 * SystemTask — much as interrupt-time completion did.  Without a host the
 * timing is the same and nothing is heard, except that waiting for a sound
 * to finish (synchronous play, disposing of a channel) doesn't wait.
 *
 * Supported: 'snd ' formats 1 and 2; standard and extended sound headers
 * (8-bit unsigned and 16-bit signed, mono and stereo); compressed headers
 * in IMA 4:1 and the uncompressed 'twos'/'sowt'/'raw ' formats; square-wave
 * and wave-table notes; SysBeep.  MACE 3:1 and 6:1 decode through the
 * MACE target (FFmpeg's decoder, LGPL).  A program's modifier routines
 * (SndAddModifier) see each command before the synthesizer.  The output
 * device's settings (SndGetInfo, GetSoundOutputInfo) are kept here; its
 * volume and mute apply to all that plays.
 */
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "machine.h"
#include "mace_decoder.h"
#include "qt_tune.h"
#include "tb_internal.h"
#include "trap.h"

void machine_advance_ticks(void);

enum {
    nullCmd = 0, initCmd = 1, freeCmd = 2, quietCmd = 3, flushCmd = 4, reInitCmd = 5, waitCmd = 10, pauseCmd = 11, resumeCmd = 12,
    callBackCmd = 13, syncCmd = 14, requestNextCmd = 21, availableCmd = 24, versionCmd = 25, totalLoadCmd = 26, loadCmd = 27,
    freqDurationCmd = 40, restCmd = 41, freqCmd = 42, ampCmd = 43, timbreCmd = 44, getAmpCmd = 45, volumeCmd = 46,
    getVolumeCmd = 47, waveTableCmd = 60, soundCmd = 80, bufferCmd = 81, rateCmd = 82, continueCmd = 83,
    doubleBufferCmd = 84, getRateCmd = 85, rateMultiplierCmd = 86, getRateMultiplierCmd = 87, sizeCmd = 90,
    convertCmd = 91,
    filePlayDoneCmd = 0x7FF0, /* ours: SndStartFilePlay's sound has finished */
};
enum { dataPointerFlag = 0x8000 };
enum { squareWaveSynth = 1, waveTableSynth = 3, sampledSynth = 5 };
enum {
    badChannel = -205, queueFull = -203, resProblem = -204, badFormat = -206, notEnoughHardwareErr = -201,
    siInvalidSampleRate = -225, siInvalidSampleSize = -226, siUnknownInfoType = -231,
};

#define MAX_CHANNELS 32
#define QUEUE_LEN 128

typedef struct {
    uint16_t cmd;
    int16_t p1;
    int32_t p2;
} sndcmd;

typedef struct {
    gaddr chan;          /* guest SndChannel */
    int id;              /* for the host */
    bool owned;          /* allocated by us (nil passed to SndNewChannel / SndPlay) */
    sndcmd q[QUEUE_LEN];
    int head, tail;
    uint64_t busy_until; /* ns: the channel is playing until then */
    bool paused;
    int16_t amp;         /* 0..255 */
    uint32_t volume;     /* left in low word, right in high, 0x0100 = full */
    uint8_t wave[512];
    bool has_wave;
    gaddr sound_header;  /* the last sound, for freq/note commands on sampled channels */
    gaddr db;            /* SndPlayDoubleBuffer's header while it plays */
    int db_index;        /* the buffer to play next */
    gaddr file_buffer;   /* SndStartFilePlay: the sound read from a file */
    gaddr file_completion;
    bool file_playing;
    uint32_t rate_mult;  /* rateMultiplierCmd's UnsignedFixed, 0x00010000 = as recorded */
    bool sustained;      /* freqCmd's tone is sounding: it gives way to the next sound */
} channel;

static channel chans[MAX_CHANNELS];
static int next_id = 1;
static machine_sound_fn sound_fn;
static void *sound_ctx;
static uint16_t beep_state = 1;
static uint32_t beep_volume = 0x01000100;

/* The sound output device, as its component's GetInfo and SetInfo see it:
 * what a program reads back is what it set, and the volume and mute
 * apply to everything played, as the hardware's did.  The format is only
 * reported; the host mixes whatever is played to its own. */
static struct {
    uint32_t rate;     /* UnsignedFixed */
    int16_t size;      /* bits */
    int16_t channels;
    uint32_t quality;
    uint32_t volume;   /* left in low word, right in high, 0x0100 = full */
    bool hardware_mute;
    bool speaker_mute;
} out;

static void output_reset(void)
{
    out.rate = 0xAC440000u; /* 44100 Hz */
    out.size = 16;
    out.channels = 2;
    out.quality = 4; /* the best interpolation */
    out.volume = 0x01000100;
    out.hardware_mute = out.speaker_mute = false;
}

/* The device's share of every channel's loudness, 1.0 at full volume. */
static float output_gain(void)
{
    if (!out.rate)
        output_reset();
    if (out.hardware_mute || out.speaker_mute)
        return 0.0f;
    return (float)((out.volume & 0xFFFF) + (out.volume >> 16)) / 2.0f / 256.0f;
}

void machine_set_sound_handler(machine_sound_fn fn, void *ctx)
{
    sound_fn = fn;
    sound_ctx = ctx;
}

static uint64_t now_ns(void) { return machine_time_ns(); }

static channel *find(gaddr chan)
{
    for (int i = 0; i < MAX_CHANNELS; i++)
        if (chans[i].chan == chan && chan)
            return &chans[i];
    return NULL;
}

static channel *add(gaddr chan, bool owned)
{
    for (int i = 0; i < MAX_CHANNELS; i++)
        if (!chans[i].chan) {
            memset(&chans[i], 0, sizeof chans[i]);
            chans[i].chan = chan;
            chans[i].owned = owned;
            chans[i].id = next_id++;
            chans[i].amp = 255;
            chans[i].volume = 0x01000100;
            chans[i].rate_mult = 0x00010000;
            return &chans[i];
        }
    return NULL;
}

/* ---------------------------------------------------------------------- */
/* Output */

static void stop_host(channel *c);

static void emit(channel *c, const int16_t *pcm, int frames, int nch, double rate)
{
    if (frames <= 0 || rate <= 0)
        return;
    /* rateMultiplierCmd plays everything faster or slower, pitch and all. */
    if (c->rate_mult)
        rate *= (double)c->rate_mult / 65536.0;
    if (c->sustained) {
        c->sustained = false;
        stop_host(c);
    }
    uint64_t start = c->busy_until > now_ns() ? c->busy_until : now_ns();
    c->busy_until = start + (uint64_t)((double)frames / rate * 1e9);
    if (sound_fn) {
        float vol = (float)((c->volume & 0xFFFF) + (c->volume >> 16)) / 2.0f / 256.0f * (float)c->amp / 255.0f *
                    output_gain();
        sound_fn(c->id, pcm, frames, nch, rate, vol, sound_ctx);
    }
}

static machine_midi_fn midi_fn;
static void *midi_ctx;

void machine_set_midi_handler(machine_midi_fn fn, void *ctx)
{
    midi_fn = fn;
    midi_ctx = ctx;
}

void sound_host_midi(int id, const machine_midi_event *events, int n)
{
    if (midi_fn)
        midi_fn(id, events, n, midi_ctx);
}

/* Sound for the host from elsewhere (movies), on channels of their own. */
void sound_host_pcm(int id, const int16_t *pcm, int frames, int nch, double rate, float volume)
{
    if (sound_fn && frames > 0)
        sound_fn(id, pcm, frames, nch, rate, volume * output_gain(), sound_ctx);
}

void sound_host_stop(int id)
{
    if (sound_fn)
        sound_fn(id, NULL, 0, 0, 0, 0, sound_ctx);
}

static void stop_host(channel *c)
{
    c->busy_until = 0;
    c->sustained = false;
    if (sound_fn)
        sound_fn(c->id, NULL, 0, 0, 0, 0, sound_ctx); /* NULL: stop */
}

/* A note: square wave, or the channel's wave table. */
static void play_note(channel *c, int note, double seconds)
{
    if (seconds <= 0)
        return;
    double freq = 440.0 * pow(2.0, (note - 69) / 12.0);
    double rate = 22254.5454;
    int frames = (int)(seconds * rate);
    int16_t *pcm = malloc(sizeof *pcm * (size_t)(frames ? frames : 1));
    for (int i = 0; i < frames; i++) {
        double ph = fmod(i * freq / rate, 1.0);
        int v;
        if (c->has_wave)
            v = ((int)c->wave[(int)(ph * 512) & 511] - 128) * 192;
        else
            v = ph < 0.5 ? 6000 : -6000;
        /* Fade the edges to avoid clicks. */
        int edge = frames - i < 64 ? frames - i : i < 64 ? i : 64;
        pcm[i] = (int16_t)(v * edge / 64);
    }
    emit(c, pcm, frames, 1, rate);
    free(pcm);
}

static double fixed_rate(uint32_t r) { return (double)r / 65536.0; }

/* IMA 4:1 ('ima4'): packets of 34 bytes per channel, a 2-byte header (the
 * predictor's top 9 bits and the step index) then 64 four-bit samples, low
 * nibble first; stereo packets alternate.  Tables from the IMA standard. */
static const int16_t ima_steps[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};
static const int8_t ima_index[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

static int16_t *decode_ima4(gaddr data, uint32_t packets, int nch)
{
    uint32_t frames = packets * 64;
    int16_t *pcm = malloc(sizeof *pcm * (frames * (uint32_t)nch ? frames * (uint32_t)nch : 1));
    for (uint32_t p = 0; p < packets; p++)
        for (int ch = 0; ch < nch; ch++) {
            gaddr pk = data + (p * (uint32_t)nch + (uint32_t)ch) * 34;
            uint16_t hdr = rd16(pk);
            int pred = (int16_t)(hdr & 0xFF80), index = hdr & 0x7F;
            if (index > 88)
                index = 88;
            for (int k = 0; k < 64; k++) {
                int nib = (rd8(pk + 2 + (uint32_t)(k / 2)) >> ((k & 1) * 4)) & 15;
                int step = ima_steps[index], diff = step >> 3;
                if (nib & 1)
                    diff += step >> 2;
                if (nib & 2)
                    diff += step >> 1;
                if (nib & 4)
                    diff += step;
                pred += (nib & 8) ? -diff : diff;
                pred = pred < -32768 ? -32768 : pred > 32767 ? 32767 : pred;
                index += ima_index[nib];
                index = index < 0 ? 0 : index > 88 ? 88 : index;
                pcm[(p * 64 + (uint32_t)k) * (uint32_t)nch + (uint32_t)ch] = (int16_t)pred;
            }
        }
    return pcm;
}

/* Decode a sound header (standard, extended or compressed) to 16-bit PCM
 * (malloc'd).  NULL with *frames 0 when it makes no sense; NULL with the
 * frames, channels and rate set when its format is one we can't decode.
 * *bits is the sound's own sample size. */
static int16_t *decode_header(gaddr h, uint32_t *frames_out, int *nch_out, double *rate_out, int *bits_out)
{
    gaddr data = rd32(h);
    uint8_t encode = rd8(h + 20);
    double rate = fixed_rate(rd32(h + 8));
    if (rate < 1000 || rate > 100000)
        rate = 22254.5454;
    *rate_out = rate;
    *frames_out = 0;
    *nch_out = 1;
    *bits_out = 16;
    if (encode == 0x00) { /* standard: 8-bit unsigned mono, length in bytes */
        uint32_t n = rd32(h + 4);
        if (!data)
            data = h + 22;
        if (n > 0x4000000)
            return NULL;
        int16_t *pcm = malloc(sizeof *pcm * (n ? n : 1));
        for (uint32_t i = 0; i < n; i++)
            pcm[i] = (int16_t)(((int)rd8(data + i) - 128) << 8);
        *frames_out = n;
        *bits_out = 8;
        return pcm;
    }
    uint32_t nch = rd32(h + 4);
    uint32_t frames = rd32(h + 22);
    if (nch < 1 || nch > 2 || frames > 0x4000000)
        return NULL;
    *nch_out = (int)nch;
    if (encode == 0xFF) { /* extended */
        int bits = (int16_t)rd16(h + 48);
        if (!data)
            data = h + 64;
        uint32_t n = frames * nch;
        int16_t *pcm = malloc(sizeof *pcm * (n ? n : 1));
        for (uint32_t i = 0; i < n; i++)
            pcm[i] = bits == 16 ? (int16_t)rd16(data + i * 2) : (int16_t)(((int)rd8(data + i) - 128) << 8);
        *frames_out = frames;
        *bits_out = bits == 16 ? 16 : 8;
        return pcm;
    }
    /* compressed (0xFE) */
    uint32_t format = rd32(h + 40);
    if (!data)
        data = h + 64;
    if (format == fourcc("ima4")) {
        if (frames > 0x100000)
            return NULL;
        *frames_out = frames * 64; /* frames counts packets */
        return decode_ima4(data, frames, (int)nch);
    }
    if (format == fourcc("twos") || format == fourcc("sowt") || format == fourcc("raw ") || format == fourcc("NONE")) {
        int bits = (int16_t)rd16(h + 62);
        uint32_t n = frames * nch;
        int16_t *pcm = malloc(sizeof *pcm * (n ? n : 1));
        for (uint32_t i = 0; i < n; i++) {
            if (bits == 16)
                pcm[i] = format == fourcc("sowt") ? (int16_t)(rd8(data + i * 2) | rd8(data + i * 2 + 1) << 8)
                                                  : (int16_t)rd16(data + i * 2);
            else
                pcm[i] = format == fourcc("twos") ? (int16_t)((int8_t)rd8(data + i) << 8)
                                                  : (int16_t)(((int)rd8(data + i) - 128) << 8);
        }
        *frames_out = frames;
        *bits_out = bits == 16 ? 16 : 8;
        return pcm;
    }
    /* MACE 3:1 and 6:1 (compressionID 3 and 4, or the format): a frame is
     * a packet of 6 samples, 2 bytes (MACE 3) or 1 byte (MACE 6) per
     * channel. */
    int cid = (int16_t)rd16(h + 56);
    bool mace3 = cid == 3 || format == fourcc("MAC3"), mace6 = cid == 4 || format == fourcc("MAC6");
    if (mace3 || mace6) {
        uint32_t bytes = frames * (mace3 ? 2 : 1) * nch;
        uint8_t *in = malloc(bytes ? bytes : 1);
        gget(in, data, bytes);
        size_t cap = (size_t)bytes * (mace3 ? 3 : 6) / nch + 6;
        int16_t *pcm = calloc(cap * nch, sizeof *pcm);
        *frames_out = (uint32_t)mace_decode(in, bytes, (int)nch, mace3, pcm);
        free(in);
        return pcm;
    }
    *frames_out = frames;
    return NULL;
}

/* Play a sound header. */
static void play_header(channel *c, gaddr h)
{
    c->sound_header = h;
    uint32_t frames;
    int nch, bits;
    double rate;
    int16_t *pcm = decode_header(h, &frames, &nch, &rate, &bits);
    /* Formats we can't decode keep their timing as silence. */
    if (!pcm && frames)
        pcm = calloc((size_t)frames * (size_t)nch, sizeof *pcm);
    if (pcm)
        emit(c, pcm, (int)frames, nch, rate);
    free(pcm);
}

/* ---------------------------------------------------------------------- */
/* Commands */

static sndcmd read_cmd(gaddr p)
{
    return (sndcmd){ rd16(p), (int16_t)rd16(p + 2), (int32_t)rd32(p + 4) };
}

static void call_at_interrupt(gaddr fn);

static void call_back(channel *c, sndcmd cmd)
{
    gaddr proc = rd32(c->chan + SndChannel_callBack_off);
    if (!proc)
        return;
    gaddr cp = tb_NewPtr(SndCommand_SIZEOF, false, true);
    wr16(cp, cmd.cmd);
    wr16(cp + 2, (uint16_t)cmd.p1);
    wr32(cp + 4, (uint32_t)cmd.p2);
    cpu_push32(g_cpu, c->chan);
    cpu_push32(g_cpu, cp);
    call_at_interrupt(proc);
    tb_DisposePtr(cp);
}

/* Do one command now; returns false if it must wait for the channel. */
static void file_play_done(channel *c, bool call_completion);

static void exec(channel *c, sndcmd cmd, gaddr result_cmd)
{
    switch (cmd.cmd) {
    case quietCmd: stop_host(c); c->db = 0; break;
    case flushCmd: c->head = c->tail = 0; c->db = 0; break;
    case reInitCmd: break;
    case waitCmd: {
        uint64_t start = c->busy_until > now_ns() ? c->busy_until : now_ns();
        c->busy_until = start + (uint64_t)(uint16_t)cmd.p1 * 500000u; /* half-milliseconds */
        break;
    }
    case pauseCmd: c->paused = true; break;
    case resumeCmd: c->paused = false; break;
    case callBackCmd: call_back(c, cmd); break;
    case freqDurationCmd: {
        double secs = (uint16_t)cmd.p1 / 2000.0;
        int note = (int)(cmd.p2 & 0xFF);
        if (c->sound_header && !c->has_wave)
            play_header(c, c->sound_header);
        else
            play_note(c, note, secs);
        break;
    }
    /* A pitch with no duration: it sounds until the next sound or
     * quietCmd, and the commands after it run at once.  Taking a quarter
     * second of the channel each made MacMan, which sends one a frame for
     * its siren, fill the queue and then run at four frames a second,
     * waiting in SndDoCommand for room.  The host gets a few seconds of
     * the tone, cut off by whatever comes next. */
    case freqCmd: {
        uint64_t until = c->busy_until;
        play_note(c, (int)(cmd.p2 & 0xFF), 2.0);
        c->busy_until = until;
        c->sustained = true;
        break;
    }
    case restCmd: {
        uint64_t start = c->busy_until > now_ns() ? c->busy_until : now_ns();
        c->busy_until = start + (uint64_t)(uint16_t)cmd.p1 * 500000u;
        break;
    }
    case ampCmd: c->amp = cmd.p1; break;
    case getAmpCmd: if (cmd.p2) wr16((gaddr)cmd.p2, (uint16_t)c->amp); break;
    case volumeCmd: c->volume = (uint32_t)cmd.p2; break;
    case getVolumeCmd: if (cmd.p2) wr32((gaddr)cmd.p2, c->volume); break;
    case timbreCmd: break;
    case waveTableCmd:
        if (cmd.p2) {
            int n = cmd.p1 > 0 && cmd.p1 <= 512 ? cmd.p1 : 512;
            for (int i = 0; i < 512; i++)
                c->wave[i] = rd8((gaddr)cmd.p2 + (uint32_t)(i * n / 512));
            c->has_wave = true;
        }
        break;
    case soundCmd: c->sound_header = (gaddr)cmd.p2; break;
    case bufferCmd: if (cmd.p2) play_header(c, (gaddr)cmd.p2); break;
    case availableCmd: if (result_cmd) wr16(result_cmd + 2, 1); break;
    case versionCmd: if (result_cmd) wr32(result_cmd + 4, 0x00030000); break;
    case getRateCmd: if (cmd.p2) wr32((gaddr)cmd.p2, 0x00010000); break;
    case rateMultiplierCmd: /* 0 would never finish; taken as 1.0 */
        c->rate_mult = (uint32_t)cmd.p2 ? (uint32_t)cmd.p2 : 0x00010000;
        break;
    case getRateMultiplierCmd: if (cmd.p2) wr32((gaddr)cmd.p2, c->rate_mult); break;
    case filePlayDoneCmd: file_play_done(c, true); break;
    default: break;
    }
}

/* Modifiers (SndAddModifier): each sees a command before the ones after
 * it in the chain and the synthesizer, as FUNCTION Modifier(chan; VAR cmd;
 * mod: ModifierStubPtr): BOOLEAN.  It may change the command, or make it
 * nullCmd to drop it; TRUE asks to be called again, with requestNextCmd
 * (param1 counting the calls), to send another command of its own. */
enum { Stub_nextStub = 0, Stub_code = 4, Stub_userInfo = 8, Stub_count = 12, Stub_every = 16, Stub_flags = 20,
       Stub_hState = 21, Stub_SIZEOF = 22 };
#define MAX_REQUESTS 64 /* requestNextCmds in a row, for a modifier that never says it's done */

/* One call of a modifier; returns its BOOLEAN and leaves the command as it
 * left it in *cmd. */
static bool call_modifier(gaddr chan, gaddr stub, sndcmd *cmd)
{
    gaddr cp = tb_NewPtr(SndCommand_SIZEOF, false, true);
    if (!cp)
        return false;
    wr16(cp, cmd->cmd);
    wr16(cp + 2, (uint16_t)cmd->p1);
    wr32(cp + 4, (uint32_t)cmd->p2);
    cpu_push16(g_cpu, 0);
    cpu_push32(g_cpu, chan);
    cpu_push32(g_cpu, cp);
    cpu_push32(g_cpu, stub);
    guest_call(rd32(stub + Stub_code));
    bool more = (cpu_pop16(g_cpu) >> 8) != 0;
    *cmd = read_cmd(cp);
    tb_DisposePtr(cp);
    return more;
}

/* A command through the modifiers from stub on, then to the synthesizer. */
static void through(channel *c, sndcmd cmd, gaddr stub, gaddr result_cmd, int depth)
{
    if (!stub || depth > 16) {
        exec(c, cmd, result_cmd);
        return;
    }
    gaddr chan = c->chan, next = rd32(stub + Stub_nextStub);
    for (int n = 1; n <= MAX_REQUESTS; n++) {
        bool more = rd32(stub + Stub_code) && call_modifier(chan, stub, &cmd);
        /* The modifier may have disposed of the channel. */
        if (find(chan) != c)
            return;
        if (cmd.cmd != nullCmd)
            through(c, cmd, next, n == 1 ? result_cmd : 0, depth + 1);
        if (!more)
            return;
        cmd = (sndcmd){ requestNextCmd, (int16_t)n, 0 };
    }
}

/* A command for the channel: its own (filePlayDoneCmd) straight to us,
 * the program's through its modifiers. */
static void dispatch(channel *c, sndcmd cmd, gaddr result_cmd)
{
    /* SndPlay's and SysBeep's private channels have no record. */
    gaddr first = c->chan >= 0xFFFFFFFEu ? 0 : rd32(c->chan + SndChannel_firstMod_off);
    if (cmd.cmd == filePlayDoneCmd || !first)
        exec(c, cmd, result_cmd);
    else
        through(c, cmd, first, result_cmd, 0);
}

/* Call a program's sound routine (doubleBack, callBack, a completion) at
 * interrupt level, where a Mac ran it: interrupt-time work waits until it
 * returns.  Run at the program's level, the clock pump inside a long one
 * (PlayerPRO's doubleBack mixes its music) refilled the same channel again
 * from within, and the program's half-made buffer state crashed it. */
static void call_at_interrupt(gaddr fn)
{
    uint16_t sr = g_cpu->sr;
    if (!(sr & 0x0700))
        g_cpu->sr = (uint16_t)((sr & ~0x0700) | 0x0100);
    guest_call(fn);
    g_cpu->sr = (uint16_t)((g_cpu->sr & ~0x0700) | (sr & 0x0700));
}

/* Double buffering: each ready buffer goes to the host a little before the
 * one playing ends, then the application's doubleBack refills it. */
#define DB_LEAD_NS 100000000u

static void pump_double(channel *c)
{
    /* A few buffers a time: one with no frames takes no time, and the
     * program must get to run between doubleBack calls, as between
     * interrupts. */
    int rounds = 0;
    while (c->db && !c->paused && now_ns() + DB_LEAD_NS >= c->busy_until && rounds++ < 4) {
        gaddr h = c->db;
        gaddr buf = rd32(h + SndDoubleBufferHeader_dbhBufferPtr_off + (uint32_t)c->db_index * 4);
        uint32_t flags = rd32(buf + SndDoubleBuffer_dbFlags_off);
        if (!(flags & 1)) /* dbBufferReady: not yet; the app is late */
            return;
        int nch = (int16_t)rd16(h + SndDoubleBufferHeader_dbhNumChannels_off);
        int bits = (int16_t)rd16(h + SndDoubleBufferHeader_dbhSampleSize_off);
        double rate = fixed_rate(rd32(h + SndDoubleBufferHeader_dbhSampleRate_off));
        uint32_t frames = rd32(buf + SndDoubleBuffer_dbNumFrames_off);
        if (nch < 1 || nch > 2 || frames > 0x1000000)
            frames = 0;
        if (rate < 1000 || rate > 100000)
            rate = 22254.5454;
        uint32_t n = frames * (uint32_t)(nch > 0 ? nch : 1);
        int16_t *pcm = malloc(sizeof *pcm * (n ? n : 1));
        gaddr data = buf + SndDoubleBuffer_dbSoundData_off;
        for (uint32_t i = 0; i < n; i++)
            pcm[i] = bits == 16 ? (int16_t)rd16(data + i * 2) : (int16_t)(((int)rd8(data + i) - 128) << 8);
        emit(c, pcm, (int)frames, nch, rate);
        free(pcm);
        bool last = flags & 4; /* dbLastBuffer */
        wr32(buf + SndDoubleBuffer_dbFlags_off, flags & ~1u);
        c->db_index ^= 1;
        if (last) {
            c->db = 0;
            return;
        }
        gaddr back = rd32(h + SndDoubleBufferHeader_dbhDoubleBack_off);
        if (back) {
            cpu_push32(g_cpu, c->chan);
            cpu_push32(g_cpu, buf);
            call_at_interrupt(back);
        }
    }
}

int16_t tb_SndPlayDoubleBuffer(gaddr chan, gaddr params)
{
    channel *c = find(chan);
    if (!c)
        return badChannel;
    int bits = (int16_t)rd16(params + SndDoubleBufferHeader_dbhSampleSize_off);
    if (rd16(params + SndDoubleBufferHeader_dbhCompressionID_off) || (bits != 8 && bits != 16))
        return badFormat;
    c->db = params;
    c->db_index = 0;
    pump_double(c);
    return noErr;
}

/* Run queued commands whose time has come: those queued before this pump
 * (a callback that queues another callBackCmd gets it at the next). */
static void pump(channel *c)
{
    pump_double(c);
    int end = c->tail;
    while (!c->paused && c->head != end && c->head != c->tail && now_ns() >= c->busy_until) {
        sndcmd cmd = c->q[c->head];
        c->head = (c->head + 1) % QUEUE_LEN;
        dispatch(c, cmd, 0);
    }
}

static bool in_pump;
static void sd_pump(void);

/* Called at safe points (Event Manager, TickCount, SystemTask). */
/* Whether a channel is open: its completions and double-buffer refills
 * come at interrupt time, which must go on while the program waits. */
bool sound_busy(void)
{
    for (int i = 0; i < MAX_CHANNELS; i++)
        if (chans[i].chan)
            return true;
    return sound_input_busy();
}

void sound_pump(void)
{
    if (in_pump)
        return;
    in_pump = true;
    for (int i = 0; i < MAX_CHANNELS; i++)
        if (chans[i].chan)
            pump(&chans[i]);
    sd_pump();
    sound_input_pump();
    qt_tune_pump();
    in_pump = false;
}

static int16_t enqueue(channel *c, sndcmd cmd)
{
    int next = (c->tail + 1) % QUEUE_LEN;
    if (next == c->head)
        return queueFull;
    c->q[c->tail] = cmd;
    c->tail = next;
    pump(c);
    return noErr;
}

/* Wait (really) until the channel has played everything queued. */
static void drain(channel *c)
{
    for (;;) {
        if (!sound_fn)
            c->busy_until = 0; /* no host to hear it: no need to wait */
        pump(c);
        if (c->head == c->tail && now_ns() >= c->busy_until)
            return;
        if (!events_host_attached()) {
            /* Headless, the clock follows instructions: move it on to the end. */
            if (c->busy_until > now_ns())
                machine_skip_time(c->busy_until - now_ns());
        } else {
            struct timespec ts = { 0, 2000000 };
            nanosleep(&ts, NULL);
        }
        machine_advance_ticks();
    }
}

/* ---------------------------------------------------------------------- */
/* Traps */

int16_t tb_SndNewChannel(gaddr chanpp, int16_t synth, int32_t init, gaddr userroutinep)
{
    gaddr chan = rd32(chanpp);
    bool owned = !chan;
    if (owned) {
        chan = tb_NewPtr(SndChannel_SIZEOF, false, true);
        if (!chan)
            return memFullErr;
        wr32(chanpp, chan);
    }
    wr32(chan + SndChannel_callBack_off, userroutinep);
    wr32(chan + SndChannel_firstMod_off, 0);
    wr16(chan + SndChannel_qLength_off, 128);
    if (!add(chan, owned))
        return notEnoughHardwareErr;
    return noErr;
}

int16_t tb_SndDisposeChannel(gaddr chan, int8_t quitnow)
{
    channel *c = find(chan);
    if (!c)
        return badChannel;
    if (!quitnow)
        drain(c);
    if (quitnow || c->sustained) /* a freqCmd's tone ends with its channel */
        stop_host(c);
    /* Each modifier hears freeCmd, then its stub goes. */
    for (gaddr stub = rd32(chan + SndChannel_firstMod_off), next; stub; stub = next) {
        next = rd32(stub + Stub_nextStub);
        sndcmd cmd = { freeCmd, 0, 0 };
        if (rd32(stub + Stub_code))
            call_modifier(chan, stub, &cmd);
        tb_DisposePtr(stub);
    }
    wr32(chan + SndChannel_firstMod_off, 0);
    c = find(chan);
    if (!c)
        return noErr;
    bool owned = c->owned;
    c->chan = 0;
    if (owned)
        tb_DisposePtr(chan);
    return noErr;
}

/* Wait until the channel's queue has room for one command, as the Sound
 * Manager does for SndDoCommand without noWait: not for the whole queue to
 * play (a music driver keeps seconds of notes queued ahead; waiting those
 * out stalled the program, Prince of Persia every half minute). */
static void wait_for_room(channel *c)
{
    for (;;) {
        /* Interrupts go on while the program waits: a driver's VBL task may
         * be what resumes or feeds the channel. */
        time_pump();
        pump(c);
        if ((c->tail + 1) % QUEUE_LEN != c->head)
            return;
        if (!events_host_attached()) {
            /* Headless, the clock follows instructions: on to when the
             * command playing now ends. */
            uint64_t t = now_ns();
            machine_skip_time(c->busy_until > t ? c->busy_until - t : 1000000);
        } else {
            struct timespec ts = { 0, 1000000 };
            nanosleep(&ts, NULL);
        }
        machine_advance_ticks();
    }
}

int16_t tb_SndDoCommand(gaddr chan, gaddr cmdp, int8_t nowait)
{
    channel *c = find(chan);
    if (!c)
        return badChannel;
    int16_t err = enqueue(c, read_cmd(cmdp));
    if (err == queueFull && !nowait) {
        wait_for_room(c);
        err = enqueue(c, read_cmd(cmdp));
    }
    return err;
}

int16_t tb_SndDoImmediate(gaddr chan, gaddr cmdp)
{
    channel *c = find(chan);
    if (!c)
        return badChannel;
    dispatch(c, read_cmd(cmdp), cmdp);
    return noErr;
}

int16_t tb_SndControl(int16_t id, gaddr cmdp)
{
    sndcmd cmd = read_cmd(cmdp);
    if (cmd.cmd == availableCmd)
        wr16(cmdp + 2, 1);
    else if (cmd.cmd == versionCmd)
        wr32(cmdp + 4, 0x00030000);
    return noErr;
}

/* SndAddModifier(chan, modifier, id, init).  A modifier routine goes in
 * front of the channel's others and is sent initCmd with init.  Without
 * one, Sound Manager 3 takes only a synthesizer's id, which sets the kind
 * of data the channel plays: ours play every kind, so there is nothing to
 * change.  Any other id named a 'snth' resource, whose code was written for
 * Sound Manager 2's synthesizer interface and has no place to run here:
 * resProblem, as for a resource that won't load. */
int16_t tb_SndAddModifier(gaddr chan, gaddr mod, int16_t id, int32_t init)
{
    channel *c = find(chan);
    if (!c)
        return badChannel;
    if (!mod)
        return id == squareWaveSynth || id == waveTableSynth || id == sampledSynth ? noErr : resProblem;
    gaddr stub = tb_NewPtr(Stub_SIZEOF, false, true);
    if (!stub)
        return memFullErr;
    wr32(stub + Stub_nextStub, rd32(chan + SndChannel_firstMod_off));
    wr32(stub + Stub_code, mod);
    wr32(chan + SndChannel_firstMod_off, stub);
    sndcmd cmd = { initCmd, 0, init };
    call_modifier(chan, stub, &cmd);
    return noErr;
}

/* The commands of a 'snd ' resource (format 1 or 2), with buffer and sound
 * commands pointing into the resource. */
static int16_t play_resource(channel *c, gaddr sndh)
{
    gaddr p = rd32(sndh);
    int format = (int16_t)rd16(p);
    gaddr q;
    if (format == 1) {
        int nmods = (int16_t)rd16(p + 2);
        q = p + 4 + (uint32_t)nmods * 6;
    } else if (format == 2) {
        q = p + 4;
    } else {
        return badFormat;
    }
    int ncmds = (int16_t)rd16(q);
    q += 2;
    for (int i = 0; i < ncmds; i++, q += 8) {
        sndcmd cmd = read_cmd(q);
        if (cmd.cmd & dataPointerFlag) {
            cmd.cmd &= (uint16_t)~dataPointerFlag;
            cmd.p2 = (int32_t)(p + (uint32_t)cmd.p2);
        }
        int16_t err = enqueue(c, cmd);
        if (err)
            return err;
    }
    return noErr;
}

int16_t tb_SndPlay(gaddr chan, gaddr sndh, int8_t async)
{
    if (!sndh || !rd32(sndh))
        return badFormat;
    int8_t state = tb_HGetState(sndh);
    tb_HLock(sndh);
    channel *c = chan ? find(chan) : NULL;
    bool temp = !c;
    if (temp)
        c = add(0xFFFFFFFFu, true); /* a private channel */
    if (!c) {
        tb_HSetState(sndh, state);
        return chan ? badChannel : notEnoughHardwareErr;
    }
    int16_t err = play_resource(c, sndh);
    if (temp || !async)
        drain(c);
    if (temp)
        c->chan = 0;
    if (temp || !async)
        tb_HSetState(sndh, state); /* async: the app keeps it locked until done */
    return err;
}

int16_t tb_SndChannelStatus(gaddr chan, int16_t length, gaddr statusp)
{
    channel *c = find(chan);
    if (!c)
        return badChannel;
    /* At most an SCStatus (24 bytes), whatever length the caller claims. */
    if (length > 24)
        length = 24;
    if (length > 0)
        gmemset(statusp, 0, (uint32_t)length);
    pump(c);
    bool busy = c->head != c->tail || now_ns() < c->busy_until || c->db;
    if (length >= 16) {
        wr8(statusp + 8, busy ? 1 : 0);     /* scChannelBusy */
        wr8(statusp + 10, c->paused ? 1 : 0); /* scChannelPaused */
    }
    return noErr;
}

int16_t tb_SndManagerStatus(int16_t length, gaddr statusp)
{
    if (length > 6) /* an SMStatus */
        length = 6;
    if (length > 0)
        gmemset(statusp, 0, (uint32_t)length);
    if (length >= 2)
        wr16(statusp, 16); /* smMaxCPULoad: any value; we have plenty */
    return noErr;
}

/* 3.2.1 for every program, 68k too: it shipped for 68k Macs as well (an
 * extension for System 7.1 and 7.5, built into 7.6 and 8), programs only
 * ask for at least a version, and the one Sound Manager here does the
 * same for all (SoundJam MP's installer, a 68k program, wanted 3.2). */
int32_t tb_SndSoundManagerVersion(void) { return 0x03218000; }

/* A SoundInfoList: the count, then a handle to the n items of size bytes. */
static void put_list(gaddr info, const uint32_t *items, int n, int size)
{
    gaddr h = tb_NewHandle((int32_t)(n * size), false, true);
    wr16(info, (uint16_t)n);
    wr32(info + 2, h);
    for (int i = 0; h && i < n; i++) {
        if (size == 2)
            wr16(rd32(h) + (uint32_t)(i * 2), (uint16_t)items[i]);
        else
            wr32(rd32(h) + (uint32_t)(i * 4), items[i]);
    }
}

/* The output device's settings by selector (GetSoundOutputInfo, and
 * SndGetInfo for what a channel passes on to its device). */
int16_t sound_output_get_info(uint32_t sel, gaddr info)
{
    if (!out.rate)
        output_reset();
    if (!info)
        return paramErr;
    if (sel == fourcc("srat")) {
        wr32(info, out.rate);
    } else if (sel == fourcc("ssiz")) {
        wr16(info, (uint16_t)out.size);
    } else if (sel == fourcc("chan")) {
        wr16(info, (uint16_t)out.channels);
    } else if (sel == fourcc("comp")) {
        wr32(info, fourcc("NONE"));
    } else if (sel == fourcc("qual")) {
        wr32(info, out.quality);
    } else if (sel == fourcc("volu") || sel == fourcc("hvol")) {
        wr32(info, out.volume);
    } else if (sel == fourcc("hmut")) {
        wr16(info, out.hardware_mute);
    } else if (sel == fourcc("smut")) {
        wr16(info, out.speaker_mute);
    } else if (sel == fourcc("srav")) {
        /* No rates listed: the first two are the ends of a continuous range. */
        uint32_t range[2] = { 0x03E80000u, 0xBB800000u }; /* 1000 to 48000 Hz */
        put_list(info, range, 2, 4);
        wr16(info, 0);
    } else if (sel == fourcc("ssav")) {
        uint32_t sizes[2] = { 8, 16 };
        put_list(info, sizes, 2, 2);
    } else if (sel == fourcc("chav")) {
        uint32_t chans_[2] = { 1, 2 };
        put_list(info, chans_, 2, 2);
    } else if (sel == fourcc("cmav")) {
        uint32_t none = fourcc("NONE");
        put_list(info, &none, 1, 4);
    } else {
        return siUnknownInfoType;
    }
    return noErr;
}

/* GetSoundOutputInfo(outputDevice, selector, infoPtr) and Set: the output
 * device's settings, as the Sound Manager 3 output component reports them
 * (SndGetInfo's selectors, for the device rather than a channel); for 68k
 * programs through SoundDispatch, PowerPC ones through SoundLib. */
int16_t tb_GetSoundOutputInfo(gaddr outputDevice, int32_t selector, gaddr infoPtr)
{
    (void)outputDevice;
    return sound_output_get_info((uint32_t)selector, infoPtr);
}

int16_t tb_SetSoundOutputInfo(gaddr outputDevice, int32_t selector, gaddr infoPtr)
{
    (void)outputDevice;
    return sound_output_set_info((uint32_t)selector, infoPtr);
}

int16_t sound_output_set_info(uint32_t sel, gaddr info)
{
    if (!out.rate)
        output_reset();
    if (!info)
        return paramErr;
    if (sel == fourcc("srat")) {
        uint32_t r = rd32(info);
        if (r < 0x03E80000u || r > 0xBB800000u)
            return siInvalidSampleRate;
        out.rate = r;
    } else if (sel == fourcc("ssiz")) {
        int16_t n = (int16_t)rd16(info);
        if (n != 8 && n != 16)
            return siInvalidSampleSize;
        out.size = n;
    } else if (sel == fourcc("chan")) {
        int16_t n = (int16_t)rd16(info);
        if (n != 1 && n != 2)
            return paramErr;
        out.channels = n;
    } else if (sel == fourcc("qual")) {
        out.quality = rd32(info); /* the host interpolates as it will */
    } else if (sel == fourcc("volu") || sel == fourcc("hvol")) {
        out.volume = rd32(info);
    } else if (sel == fourcc("hmut")) {
        out.hardware_mute = rd16(info) != 0;
    } else if (sel == fourcc("smut")) {
        out.speaker_mute = rd16(info) != 0;
    } else {
        return siUnknownInfoType;
    }
    return noErr;
}

/* A channel's settings by selector (Sound Manager 3.1).  The channel's
 * own are its volume ('volu', what volumeCmd sets) and rate multiplier
 * ('rmul', rateMultiplierCmd's); the rest go on to the output device, as
 * the channel's component chain passed them on.  A pre-mixer component
 * ('prmx', for Speed Demon) is taken and has no effect: none of ours
 * sits in a chain to be replaced. */
int16_t tb_SndGetInfo(gaddr chan, int32_t selector, gaddr infoPtr)
{
    channel *c = find(chan);
    if (!c)
        return badChannel;
    uint32_t sel = (uint32_t)selector;
    if (!infoPtr)
        return paramErr;
    if (sel == fourcc("volu"))
        wr32(infoPtr, c->volume);
    else if (sel == fourcc("rmul"))
        wr32(infoPtr, c->rate_mult);
    else
        return sound_output_get_info(sel, infoPtr);
    return noErr;
}

int16_t tb_SndSetInfo(gaddr chan, int32_t selector, gaddr infoPtr)
{
    channel *c = find(chan);
    if (!c)
        return badChannel;
    uint32_t sel = (uint32_t)selector;
    if (sel == fourcc("prmx"))
        return noErr;
    if (!infoPtr)
        return paramErr;
    if (sel == fourcc("volu"))
        c->volume = rd32(infoPtr);
    else if (sel == fourcc("rmul"))
        c->rate_mult = rd32(infoPtr) ? rd32(infoPtr) : 0x00010000;
    else
        return sound_output_set_info(sel, infoPtr);
    return noErr;
}
int32_t tb_MACEVersion(void) { return 0x03218000; }
int32_t tb_SPBVersion(void) { return 0x03218000; }

/* SndDispVersion(dSelector): the version of the tool SoundDispatch reaches
 * by the selector's low word, 0 for one that isn't there. */
int32_t tb_FinaleUnknown1(int16_t dSelector)
{
    switch ((uint16_t)dSelector) {
    case 0x0004: return tb_DirectorUnknown3(); /* the MIDI Manager */
    case 0x0008: return tb_SndSoundManagerVersion();
    case 0x000C: return tb_SpeechManagerVersion();
    case 0x0010: return tb_MACEVersion();
    case 0x0014: return tb_SPBVersion();
    default: return 0;
    }
}

void tb_SndGetSysBeepState(gaddr statep) { wr16(statep, beep_state); }
int16_t tb_SndSetSysBeepState(int16_t state) { beep_state = (uint16_t)state; return noErr; }
int16_t tb_GetSysBeepVolume(gaddr levelp) { wr32(levelp, beep_volume); return noErr; }
int16_t tb_SetSysBeepVolume(int32_t level) { beep_volume = (uint32_t)level; return noErr; }
/* The default output device's volume: its hardware volume ('hvol'). */
int16_t tb_GetDefaultOutputVolume(gaddr levelp)
{
    if (!out.rate)
        output_reset();
    wr32(levelp, out.volume);
    return noErr;
}

int16_t tb_SetDefaultOutputVolume(int32_t level)
{
    if (!out.rate)
        output_reset();
    out.volume = (uint32_t)level;
    return noErr;
}
int16_t tb_GetSoundHeaderOffset(gaddr sndh, gaddr offset)
{
    gaddr p = rd32(sndh);
    int format = (int16_t)rd16(p);
    gaddr q = format == 1 ? p + 4 + (uint32_t)(int16_t)rd16(p + 2) * 6 : p + 4;
    int n = (int16_t)rd16(q);
    for (int i = 0; i < n; i++) {
        uint16_t cmd = rd16(q + 2 + (uint32_t)i * 8);
        if ((cmd & 0x7FFF) == bufferCmd || (cmd & 0x7FFF) == soundCmd) {
            wr32(offset, rd32(q + 2 + (uint32_t)i * 8 + 4));
            return noErr;
        }
    }
    wr32(offset, 0);
    return badFormat;
}

/* A Fixed sample rate as an 80-bit extended (AIFFSampleRate). */
static void rate_extended(gaddr x, uint32_t rate)
{
    double v = (double)rate / 65536.0;
    gmemset(x, 0, 10);
    if (v <= 0)
        return;
    int e;
    double m = frexp(v, &e); /* v = m * 2^e, m in [0.5, 1) */
    wr16(x, (uint16_t)(16383 + e - 1));
    uint64_t mant = (uint64_t)ldexp(m, 64);
    wr32(x + 2, (uint32_t)(mant >> 32));
    wr32(x + 6, (uint32_t)mant);
}

/* SetupSndHeader: a format 1 'snd ' (the sampled synthesizer and one
 * bufferCmd) with a standard, extended or compressed sound header for
 * numBytes of samples to follow it; headerLen is where they go.  A handle
 * too small for the header grows. */
int16_t tb_SetupSndHeader(gaddr sndh, int16_t nch, int32_t fixed_rate, int16_t size, int32_t compression,
                          int16_t base, int32_t nbytes, gaddr headerlen)
{
    uint32_t rate = (uint32_t)fixed_rate, comp = (uint32_t)compression;
    enum { initMono = 0x80, initStereo = 0xC0, initMACE3 = 0x300, initMACE6 = 0x400 };
    bool none = comp == fourcc("NONE") || comp == 0;
    int kind = !none ? 2 : (nch == 1 && size == 8) ? 0 : 1; /* standard, extended, compressed */
    uint32_t hlen = 20 + (kind == 0 ? 22 : 64);
    if (headerlen)
        wr16(headerlen, (uint16_t)hlen);
    if (!sndh)
        return noErr; /* just the length */
    if (tb_GetHandleSize(sndh) < (int32_t)hlen) {
        tb_SetHandleSize(sndh, (int32_t)hlen);
        if (!rd32(sndh) || tb_GetHandleSize(sndh) < (int32_t)hlen)
            return memFullErr;
    }
    gaddr p = rd32(sndh);
    gmemset(p, 0, hlen);
    uint32_t init = nch > 1 ? initStereo : initMono;
    if (comp == fourcc("MAC3"))
        init |= initMACE3;
    else if (comp == fourcc("MAC6"))
        init |= initMACE6;
    wr16(p, 1);                                  /* format 1 */
    wr16(p + 2, 1);                              /* one modifier: */
    wr16(p + 4, sampledSynth);
    wr32(p + 6, init);
    wr16(p + 10, 1);                             /* one command: */
    wr16(p + 12, dataPointerFlag | bufferCmd);   /* bufferCmd with the header's offset */
    wr32(p + 16, 20);
    gaddr h = p + 20;
    int channels = nch > 0 ? nch : 1;
    wr32(h + 8, rate);
    wr8(h + 21, (uint8_t)base);
    if (kind == 0) {
        wr32(h + 4, (uint32_t)nbytes);           /* length */
    } else {
        wr32(h + 4, (uint32_t)channels);
        wr8(h + 20, kind == 1 ? 0xFF : 0xFE);    /* encode: extSH or cmpSH */
        uint32_t frames;
        if (kind == 1) {
            int frame = channels * (size > 8 ? (size + 7) / 8 : 1);
            frames = (uint32_t)nbytes / (uint32_t)frame;
            wr16(h + 48, (uint16_t)size);        /* sampleSize */
        } else {
            /* Packets: MACE 3:1 two bytes, 6:1 one byte, IMA 4:1 34 bytes, per channel. */
            uint32_t packet = comp == fourcc("MAC3") ? 2 : comp == fourcc("MAC6") ? 1 :
                              comp == fourcc("ima4") ? 34 : 1;
            frames = (uint32_t)nbytes / (packet * (uint32_t)channels);
            wr32(h + 40, comp);                  /* format */
            int16_t id = comp == fourcc("MAC3") ? 3 : comp == fourcc("MAC6") ? 4 : -1;
            wr16(h + 56, (uint16_t)id);          /* compressionID */
            wr16(h + 62, (uint16_t)size);        /* sampleSize */
        }
        wr32(h + 22, frames);                    /* numFrames */
        rate_extended(h + 26, rate);             /* AIFFSampleRate */
    }
    return noErr;
}

/* SetupAIFFHeader: an AIFF file's header (AIFF-C when the sound is
 * compressed), written at the file's mark, for numBytes of samples to
 * follow it: FORM, the version (AIFF-C), COMM and the start of SSND. */
int16_t tb_SetupAIFFHeader(int16_t refnum, int16_t nch, int32_t fixed_rate, int16_t size, int32_t compression,
                           int32_t nbytes, int32_t nframes)
{
    uint32_t comp = (uint32_t)compression;
    bool aifc = comp && comp != fourcc("NONE");
    const char *name = comp == fourcc("MAC3") ? "MACE 3-to-1" : comp == fourcc("MAC6") ? "MACE 6-to-1"
                       : comp == fourcc("ima4") ? "IMA 4 to 1" : "not compressed";
    uint32_t namelen = aifc ? ((uint32_t)strlen(name) + 2) & ~1u : 0; /* a Pascal string, even */
    uint32_t comm = 18 + (aifc ? 4 + namelen : 0);
    uint32_t len = 12 + (aifc ? 12 : 0) + 8 + comm + 16;
    if (!nframes && nch > 0 && size > 0)
        nframes = nbytes / (nch * ((size + 7) / 8));
    gaddr h = tb_NewPtr((int32_t)len, false, true), pb = tb_NewPtr(80, false, true);
    if (!h || !pb) {
        if (h)
            tb_DisposePtr(h);
        return memFullErr;
    }
    gaddr p = h;
    wr32(p, fourcc("FORM"));
    wr32(p + 4, len - 8 + (uint32_t)nbytes);
    wr32(p + 8, fourcc(aifc ? "AIFC" : "AIFF"));
    p += 12;
    if (aifc) {
        wr32(p, fourcc("FVER"));
        wr32(p + 4, 4);
        wr32(p + 8, 0xA2805140); /* AIFF-C version 1 */
        p += 12;
    }
    wr32(p, fourcc("COMM"));
    wr32(p + 4, comm);
    wr16(p + 8, (uint16_t)nch);
    wr32(p + 10, (uint32_t)nframes);
    wr16(p + 14, (uint16_t)size);
    rate_extended(p + 16, (uint32_t)fixed_rate);
    if (aifc) {
        wr32(p + 26, comp);
        wr8(p + 30, (uint8_t)strlen(name));
        gput(p + 31, name, (uint32_t)strlen(name));
    }
    p += 8 + comm;
    wr32(p, fourcc("SSND"));
    wr32(p + 4, 8 + (uint32_t)nbytes);
    wr32(p + 8, 0);  /* offset */
    wr32(p + 12, 0); /* blockSize */
    wr16(pb + 24, (uint16_t)refnum); /* ioRefNum */
    wr32(pb + 32, h);                /* ioBuffer */
    wr32(pb + 36, len);              /* ioReqCount */
    wr16(pb + 44, 0);                /* ioPosMode: fsAtMark */
    int16_t err = tb_PBWrite(pb, false);
    tb_DisposePtr(pb);
    tb_DisposePtr(h);
    return err;
}

/* bluebox inspect: a resource's sampled sound, decoded as it would play.
 * The first sound or buffer command's header is the sound. */
bool machine_snd_pcm(const uint8_t *snd, size_t len, int16_t **pcm, uint32_t *frames, int *channels,
                     double *rate, int *bits)
{
    *pcm = NULL;
    if (len < 6 || len > 0x4000000)
        return false;
    gaddr p = tb_NewPtr((int32_t)len, false, false);
    if (!p)
        return false;
    gput(p, snd, (uint32_t)len);
    int format = (int16_t)rd16(p);
    gaddr q = format == 1 ? p + 4 + (uint32_t)(int16_t)rd16(p + 2) * 6 : format == 2 ? p + 4 : 0;
    int n = q && q + 2 <= p + len ? (int16_t)rd16(q) : 0;
    gaddr h = 0;
    for (int i = 0; i < n && !h && q + 10 + (uint32_t)i * 8 <= p + len; i++) {
        uint16_t cmd = rd16(q + 2 + (uint32_t)i * 8);
        uint32_t off = rd32(q + 2 + (uint32_t)i * 8 + 4);
        if (((cmd & 0x7FFF) == bufferCmd || (cmd & 0x7FFF) == soundCmd) && off + 22 <= len)
            h = p + off;
    }
    if (h)
        *pcm = decode_header(h, frames, channels, rate, bits);
    tb_DisposePtr(p);
    return *pcm != NULL;
}

/* SysBeep: a short, soft classic beep. */
void sound_beep(void)
{
    if (!(beep_state & 1))
        return;
    channel *c = add(0xFFFFFFFEu, true);
    if (!c)
        return;
    double rate = 22254.5454, secs = 0.12;
    int frames = (int)(secs * rate);
    int16_t *pcm = malloc(sizeof *pcm * (size_t)frames);
    for (int i = 0; i < frames; i++) {
        double t = i / rate;
        double env = exp(-t * 18.0);
        pcm[i] = (int16_t)(sin(2 * M_PI * 880.0 * t) * 9000 * env + (fmod(t * 440.0, 1.0) < 0.5 ? 1 : -1) * 2500 * env);
    }
    emit(c, pcm, frames, 1, rate);
    free(pcm);
    c->chan = 0; /* the host plays it out; we do not wait */
}

/* ---------------------------------------------------------------------- */
/* The Sound Driver (.Sound, refnum -4), from before the Sound Manager:
 * StartSound writes a synthesizer record to it, StopSound kills it.  The
 * record's first word is the mode: free-form (0, a rate and 8-bit
 * samples), four-tone (1, a pointer to an FTSoundRec the program may
 * change as it plays), or square wave (-1, count/amplitude/duration
 * triples, count 0 ending them). */

enum { ffMode = 0, ftMode = 1, swMode = -1, abortErr = -27 };
#define MAC_RATE 22254.5454
static channel sd_chan;
static gaddr sd_pb;            /* the write playing */
static gaddr sd_ft;            /* four-tone: its FTSoundRec */
static uint64_t sd_ft_next;    /* when the next tick of four-tone is due */

/* A tick of four-tone sound, ahead of time: the four voices' wave tables
 * stepped by their rates (fixed point, table positions a sample), the
 * phases and duration written back as the driver did. */
static void ft_render_tick(void)
{
    gaddr r = sd_ft;
    int frames = (int)(MAC_RATE / 60.0);
    int16_t pcm[400];
    uint32_t phase[4], rate[4];
    gaddr wave[4];
    for (int v = 0; v < 4; v++) {
        rate[v] = rd32(r + 2 + (uint32_t)v * 8);
        phase[v] = rd32(r + 6 + (uint32_t)v * 8);
        wave[v] = rd32(r + 34 + (uint32_t)v * 4);
    }
    for (int i = 0; i < frames; i++) {
        int sum = 0;
        for (int v = 0; v < 4; v++) {
            if (wave[v])
                sum += (int)rd8(wave[v] + ((phase[v] >> 16) & 255)) - 128;
            phase[v] += rate[v];
        }
        pcm[i] = (int16_t)(sum * 64);
    }
    for (int v = 0; v < 4; v++)
        wr32(r + 6 + (uint32_t)v * 8, phase[v]);
    emit(&sd_chan, pcm, frames, 1, MAC_RATE);
}

static void sd_done(int16_t result)
{
    gaddr pb = sd_pb;
    sd_pb = sd_ft = 0;
    if (!pb)
        return;
    wr16(pb + IOParam_ioResult_off, (uint16_t)result);
    io_complete_later(pb, result);
}

/* KillIO: the sound stops, and the write's completion runs now. */
static void sd_kill_now(void)
{
    gaddr pb = sd_pb;
    stop_host(&sd_chan);
    sd_pb = sd_ft = 0;
    if (!pb)
        return;
    wr16(pb + IOParam_ioResult_off, (uint16_t)abortErr);
    io_complete_now(pb, abortErr);
}

/* From the sound pump: four-tone goes on a tick at a time while its
 * duration lasts; a write finishes when its sound has played. */
static void sd_pump(void)
{
    if (!sd_pb)
        return;
    if (sd_ft) {
        while (sd_ft && now_ns() + DB_LEAD_NS / 2 >= sd_ft_next) {
            int16_t left = (int16_t)rd16(sd_ft);
            if (left <= 0) {
                sd_done(noErr);
                return;
            }
            wr16(sd_ft, (uint16_t)(left - 1));
            ft_render_tick();
            sd_ft_next = sd_chan.busy_until;
        }
        return;
    }
    if (now_ns() >= sd_chan.busy_until)
        sd_done(noErr);
}

static void sd_stop(void)
{
    stop_host(&sd_chan);
    if (sd_pb)
        sd_done(abortErr);
}

static int16_t sd_write(gaddr pb, bool async)
{
    gaddr rec = rd32(pb + IOParam_ioBuffer_off);
    uint32_t len = rd32(pb + IOParam_ioReqCount_off);
    int mode = (int16_t)rd16(rec);
    LOGF(LOG_TRAPS, "  .Sound mode %d, %u bytes%s\n", mode, len, async ? ", async" : "");
    if (sd_pb)
        sd_stop(); /* a new sound replaces the one playing */
    if (!sd_chan.id)
        sd_chan.id = next_id++;
    sd_chan.amp = 255;
    sd_chan.volume = 0x01000100;
    wr32(pb + IOParam_ioActCount_off, len);
    if (mode == ffMode && len > 6) {
        double rate = MAC_RATE * fixed_rate(rd32(rec + 2));
        uint32_t n = len - 6;
        int16_t *pcm = malloc(sizeof *pcm * n);
        for (uint32_t i = 0; i < n; i++)
            pcm[i] = (int16_t)(((int)rd8(rec + 6 + i) - 128) << 8);
        if (rate > 1000 && rate < 100000)
            emit(&sd_chan, pcm, (int)n, 1, rate);
        free(pcm);
    } else if (mode == swMode) {
        for (gaddr t = rec + 2; t + 6 <= rec + len; t += 6) {
            uint16_t count = rd16(t), amp = rd16(t + 2), ticks = rd16(t + 4);
            if (!count)
                break;
            double freq = 783360.0 / count, secs = ticks / 60.0;
            int frames = (int)(secs * MAC_RATE);
            int16_t *pcm = malloc(sizeof *pcm * (size_t)(frames ? frames : 1));
            for (int i = 0; i < frames; i++)
                pcm[i] = (int16_t)((fmod(i * freq / MAC_RATE, 1.0) < 0.5 ? 1 : -1) * (int)(amp & 255) * 24);
            emit(&sd_chan, pcm, frames, 1, MAC_RATE);
            free(pcm);
        }
    } else if (mode == ftMode) {
        sd_ft = rd32(rec + 2);
        sd_ft_next = now_ns();
    }
    sd_pb = pb;
    if (async) {
        wr16(pb + IOParam_ioResult_off, ioInProgress);
        sd_pump();
        return sd_pb == pb ? ioInProgress : (int16_t)rd16(pb + IOParam_ioResult_off);
    }
    /* Synchronous: the sound plays out first, with interrupt-time work
     * going on, as StartSound with no completion routine did. */
    while (sd_pb == pb && !g_cpu->halted) {
        if (events_host_attached()) {
            struct timespec ts = { 0, 5000000 };
            nanosleep(&ts, NULL);
        } else {
            uint64_t t = now_ns();
            uint64_t until = sd_ft ? sd_ft_next : sd_chan.busy_until;
            if (until > t)
                machine_skip_time(until - t);
        }
        machine_advance_ticks();
        sound_pump();
        time_pump();
    }
    sd_pb = 0;
    return noErr;
}

static int16_t sd_kill(gaddr pb)
{
    sd_kill_now();
    return noErr;
}

static int16_t sd_control(gaddr pb, bool async)
{
    if ((int16_t)rd16(pb + CntrlParam_csCode_off) == 1) /* killCode */
        sd_kill_now();
    return noErr;
}

const native_driver sound_driver = { ".Sound", 3, NULL, NULL, sd_control, NULL, NULL, sd_write, sd_kill };

void sound_reset(void)
{
    sd_pb = sd_ft = 0;
    memset(&sd_chan, 0, sizeof sd_chan);
    for (int i = 0; i < MAX_CHANNELS; i++)
        if (chans[i].chan)
            stop_host(&chans[i]);
    memset(chans, 0, sizeof chans);
    output_reset();
    sound_input_reset();
}


/* ---- Playing from files ---- */

/* The sound is finished (or stopped): free what we read, and tell the
 * application through its completion routine, PROCEDURE (chan). */
static void file_play_done(channel *c, bool call_completion)
{
    if (!c->file_playing)
        return;
    c->file_playing = false;
    if (c->file_buffer) {
        tb_DisposePtr(c->file_buffer);
        c->file_buffer = 0;
    }
    gaddr proc = c->file_completion;
    c->file_completion = 0;
    if (call_completion && proc && c->chan && c->chan != 0xFFFFFFFFu) {
        cpu_push32(g_cpu, c->chan);
        call_at_interrupt(proc);
    }
}

/* Read count bytes at offset of an open file into guest memory at dst. */
static int16_t file_read(int16_t refnum, uint32_t offset, gaddr dst, uint32_t count)
{
    gaddr pb = tb_NewPtr(80, false, true);
    if (!pb)
        return memFullErr;
    wr16(pb + 24, (uint16_t)refnum);       /* ioRefNum */
    wr32(pb + 32, dst);                     /* ioBuffer */
    wr32(pb + 36, count);                   /* ioReqCount */
    wr16(pb + 44, 1);                       /* ioPosMode: fsFromStart */
    wr32(pb + 46, offset);                  /* ioPosOffset */
    int16_t err = tb_PBRead(pb, false);
    if (err == -39 /* eofErr */ && rd32(pb + 40) == count)
        err = noErr;
    tb_DisposePtr(pb);
    return err;
}

static uint32_t file_eof(int16_t refnum)
{
    gaddr pb = tb_NewPtr(80, false, true);
    if (!pb)
        return 0;
    wr16(pb + 24, (uint16_t)refnum);
    uint32_t eof = tb_PBGetEOF(pb, false) ? 0 : rd32(pb + 28); /* ioMisc */
    tb_DisposePtr(pb);
    return eof;
}

/* An 80-bit extended sample rate to a Fixed. */
static uint32_t rate_fixed(const uint8_t *x)
{
    int e = ((x[0] & 0x7F) << 8 | x[1]) - 16383 - 63;
    uint64_t m = 0;
    for (int i = 0; i < 8; i++)
        m = m << 8 | x[2 + i];
    double r = (double)m;
    while (e > 0) { r *= 2; e--; }
    while (e < 0) { r /= 2; e++; }
    return r >= 65536.0 ? 0xFFFFFFFFu : (uint32_t)(r * 65536.0);
}

/* An AIFF or AIFF-C file as a sound header followed by its samples, in one
 * guest block. */
static gaddr read_aiff(int16_t refnum, int16_t *errp)
{
    uint8_t form[12];
    gaddr tmp = tb_NewPtr(64, false, true);
    *errp = badFormat;
    if (!tmp)
        return 0;
    uint32_t eof = file_eof(refnum);
    bool ok = eof >= 12 && !file_read(refnum, 0, tmp, 12);
    gget(form, tmp, 12);
    if (!ok || memcmp(form, "FORM", 4) || (memcmp(form + 8, "AIFF", 4) && memcmp(form + 8, "AIFC", 4))) {
        tb_DisposePtr(tmp);
        return 0;
    }
    bool aifc = !memcmp(form + 8, "AIFC", 4);
    uint8_t comm[26] = { 0 };
    bool have_comm = false;
    uint32_t ssnd_at = 0, ssnd_len = 0;
    for (uint32_t at = 12; at + 8 <= eof;) {
        uint8_t ck[8];
        if (file_read(refnum, at, tmp, 8))
            break;
        gget(ck, tmp, 8);
        uint32_t len = (uint32_t)ck[4] << 24 | (uint32_t)ck[5] << 16 | (uint32_t)ck[6] << 8 | ck[7];
        if (!memcmp(ck, "COMM", 4)) {
            uint32_t n = len < sizeof comm ? len : sizeof comm;
            if (file_read(refnum, at + 8, tmp, n))
                break;
            gget(comm, tmp, n);
            have_comm = true;
        } else if (!memcmp(ck, "SSND", 4) && len >= 8) {
            uint8_t off[4];
            if (file_read(refnum, at + 8, tmp, 4))
                break;
            gget(off, tmp, 4);
            uint32_t skip = (uint32_t)off[0] << 24 | (uint32_t)off[1] << 16 | (uint32_t)off[2] << 8 | off[3];
            ssnd_at = at + 16 + skip;
            ssnd_len = len - 8 - skip;
        }
        at += 8 + len + (len & 1);
    }
    tb_DisposePtr(tmp);
    if (!have_comm || !ssnd_at || ssnd_len > eof)
        return 0;
    uint16_t nch = (uint16_t)(comm[0] << 8 | comm[1]);
    uint32_t frames = (uint32_t)comm[2] << 24 | (uint32_t)comm[3] << 16 | (uint32_t)comm[4] << 8 | comm[5];
    uint16_t bits = (uint16_t)(comm[6] << 8 | comm[7]);
    uint32_t format = aifc ? (uint32_t)comm[18] << 24 | (uint32_t)comm[19] << 16 | (uint32_t)comm[20] << 8 | comm[21]
                           : fourcc("twos");
    gaddr h = tb_NewPtr((int32_t)(64 + ssnd_len), false, true);
    if (!h) {
        *errp = memFullErr;
        return 0;
    }
    if (file_read(refnum, ssnd_at, h + 64, ssnd_len)) {
        tb_DisposePtr(h);
        *errp = ioErr;
        return 0;
    }
    /* A compressed sound header (encode 0xFE) describes every format. */
    wr32(h + 0, h + 64);                  /* samplePtr */
    wr32(h + 4, nch);                     /* numChannels */
    wr32(h + 8, rate_fixed(comm + 8));    /* sampleRate */
    wr8(h + 20, 0xFE);                    /* encode: cmpSH */
    wr8(h + 21, 60);                      /* baseFrequency */
    wr32(h + 22, format == fourcc("ima4") ? ssnd_len / (34u * (nch ? nch : 1)) : frames);
    gput(h + 26, comm + 8, 10);           /* AIFFSampleRate */
    wr32(h + 40, format);                 /* format */
    wr16(h + 62, bits);                   /* sampleSize */
    *errp = noErr;
    return h;
}

/* SndStartFilePlay: a 'snd ' resource (resnum) or an AIFF/AIFF-C file
 * (refnum).  Asynchronous plays end with the completion routine. */
int16_t tb_SndStartFilePlay(gaddr chan, int16_t refnum, int16_t resnum, int32_t buffersize, gaddr bufferp,
                            gaddr selection, gaddr completion, int8_t async)
{
    channel *c = chan ? find(chan) : NULL;
    if (chan && !c)
        return badChannel;
    if (async && !c)
        return badChannel; /* asynchronous play needs the application's channel */
    if (resnum) {
        gaddr h = tb_GetResource((int32_t)fourcc("snd "), resnum);
        if (!h)
            return -192; /* resNotFound */
        int16_t err = tb_SndPlay(chan, h, async);
        if (!err && async) {
            c->file_playing = true;
            c->file_completion = completion;
            enqueue(c, (sndcmd){ filePlayDoneCmd, 0, 0 });
        }
        return err;
    }
    int16_t err;
    gaddr h = read_aiff(refnum, &err);
    if (!h)
        return err;
    bool temp = !c;
    if (temp)
        c = add(0xFFFFFFFFu, true);
    if (!c) {
        tb_DisposePtr(h);
        return notEnoughHardwareErr;
    }
    file_play_done(c, false); /* anything still playing from a file */
    c->file_playing = true;
    c->file_buffer = h;
    c->file_completion = async ? completion : 0;
    enqueue(c, (sndcmd){ bufferCmd, 0, (int32_t)h });
    enqueue(c, (sndcmd){ filePlayDoneCmd, 0, 0 });
    if (!async || temp)
        drain(c);
    if (temp)
        c->chan = 0;
    return noErr;
}

int16_t tb_SndPauseFilePlay(gaddr chan)
{
    channel *c = find(chan);
    if (!c || !c->file_playing)
        return badChannel;
    c->paused = !c->paused;
    if (c->paused)
        stop_host(c);
    return noErr;
}

int16_t tb_SndStopFilePlay(gaddr chan, int8_t quietNow)
{
    channel *c = find(chan);
    if (!c)
        return noErr;
    if (c->file_playing) {
        stop_host(c);
        c->head = c->tail = 0;
        c->paused = false;
        file_play_done(c, true);
    }
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* MACE expansion for programs that call it themselves (Diamonds) */

/* Exp1to3 and Exp1to6: cnt packets of MACE (2 bytes for 3:1, 1 for 6:1,
 * six samples each) for channel whichChannel of numChannels, whose packets
 * alternate, into 8-bit offset samples in the output, interleaved as the
 * channels are, so a program calls once a channel.  Each call decodes on
 * its own: the state buffers, which carried the predictor from one call
 * to the next, are left alone, and a long sound expanded a few packets at
 * a time may click where the calls meet. */
static void mace_expand(gaddr inp, gaddr outp, int32_t cnt, int32_t nch, int32_t which, bool mace3)
{
    if (cnt <= 0 || nch < 1 || which < 1 || which > nch)
        return;
    uint32_t per = mace3 ? 2 : 1;
    uint32_t bytes = (uint32_t)cnt * per;
    uint8_t *in = malloc(bytes);
    int16_t *pcm = malloc((size_t)cnt * 6 * sizeof *pcm);
    if (!in || !pcm) {
        free(in);
        free(pcm);
        return;
    }
    for (int32_t k = 0; k < cnt; k++) /* this channel's packets */
        gget(in + (size_t)k * per, inp + ((uint32_t)k * (uint32_t)nch + (uint32_t)(which - 1)) * per, per);
    size_t frames = mace_decode(in, bytes, 1, mace3, pcm);
    for (size_t i = 0; i < frames; i++)
        wr8(outp + (uint32_t)(i * (size_t)nch + (size_t)(which - 1)), (uint8_t)((pcm[i] >> 8) + 128));
    free(in);
    free(pcm);
}

void tb_Exp1to3(gaddr inp, gaddr outp, int32_t cnt, gaddr instatep, gaddr outstatep, int32_t numchannels,
                int32_t whichchannel)
{
    (void)instatep, (void)outstatep;
    mace_expand(inp, outp, cnt, numchannels, whichchannel, true);
}

void tb_Exp1to6(gaddr inp, gaddr outp, int32_t cnt, gaddr instatep, gaddr outstatep, int32_t numchannels,
                int32_t whichchannel)
{
    (void)instatep, (void)outstatep;
    mace_expand(inp, outp, cnt, numchannels, whichchannel, false);
}
