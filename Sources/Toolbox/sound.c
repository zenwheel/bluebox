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
 * and wave-table notes; SysBeep.  MACE plays silence of the right length
 * for now.
 */
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "machine.h"
#include "tb_internal.h"
#include "trap.h"

void machine_advance_ticks(void);

enum {
    nullCmd = 0, quietCmd = 3, flushCmd = 4, reInitCmd = 5, waitCmd = 10, pauseCmd = 11, resumeCmd = 12,
    callBackCmd = 13, syncCmd = 14, availableCmd = 24, versionCmd = 25, totalLoadCmd = 26, loadCmd = 27,
    freqDurationCmd = 40, restCmd = 41, freqCmd = 42, ampCmd = 43, timbreCmd = 44, getAmpCmd = 45, volumeCmd = 46,
    getVolumeCmd = 47, waveTableCmd = 60, soundCmd = 80, bufferCmd = 81, rateCmd = 82, continueCmd = 83,
    doubleBufferCmd = 84, getRateCmd = 85, rateMultiplierCmd = 86, getRateMultiplierCmd = 87, sizeCmd = 90,
    convertCmd = 91,
    filePlayDoneCmd = 0x7FF0, /* ours: SndStartFilePlay's sound has finished */
};
enum { dataPointerFlag = 0x8000 };
enum { squareWaveSynth = 1, waveTableSynth = 3, sampledSynth = 5 };
enum { badChannel = -205, queueFull = -203, badFormat = -206, notEnoughHardwareErr = -201 };

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
} channel;

static channel chans[MAX_CHANNELS];
static int next_id = 1;
static machine_sound_fn sound_fn;
static void *sound_ctx;
static uint16_t beep_state = 1;
static uint32_t beep_volume = 0x01000100;

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
            return &chans[i];
        }
    return NULL;
}

/* ---------------------------------------------------------------------- */
/* Output */

static void emit(channel *c, const int16_t *pcm, int frames, int nch, double rate)
{
    if (frames <= 0 || rate <= 0)
        return;
    uint64_t start = c->busy_until > now_ns() ? c->busy_until : now_ns();
    c->busy_until = start + (uint64_t)((double)frames / rate * 1e9);
    if (sound_fn) {
        float vol = (float)((c->volume & 0xFFFF) + (c->volume >> 16)) / 2.0f / 256.0f * (float)c->amp / 255.0f;
        sound_fn(c->id, pcm, frames, nch, rate, vol, sound_ctx);
    }
}

static void stop_host(channel *c)
{
    c->busy_until = 0;
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

static void play_ima4(channel *c, gaddr data, uint32_t packets, int nch, double rate)
{
    if (packets > 0x100000)
        return;
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
    emit(c, pcm, (int)frames, nch, rate);
    free(pcm);
}

/* Play a sound header (standard, extended or compressed). */
static void play_header(channel *c, gaddr h)
{
    gaddr data = rd32(h);
    uint8_t encode = rd8(h + 20);
    double rate = fixed_rate(rd32(h + 8));
    if (rate < 1000 || rate > 100000)
        rate = 22254.5454;
    c->sound_header = h;
    if (encode == 0x00) { /* standard: 8-bit unsigned mono, length in bytes */
        uint32_t n = rd32(h + 4);
        if (!data)
            data = h + 22;
        if (n > 0x4000000)
            return;
        int16_t *pcm = malloc(sizeof *pcm * (n ? n : 1));
        for (uint32_t i = 0; i < n; i++)
            pcm[i] = (int16_t)(((int)rd8(data + i) - 128) << 8);
        emit(c, pcm, (int)n, 1, rate);
        free(pcm);
        return;
    }
    uint32_t nch = rd32(h + 4);
    uint32_t frames = rd32(h + 22);
    if (nch < 1 || nch > 2 || frames > 0x4000000)
        return;
    if (encode == 0xFF) { /* extended */
        int bits = (int16_t)rd16(h + 48);
        if (!data)
            data = h + 64;
        uint32_t n = frames * nch;
        int16_t *pcm = malloc(sizeof *pcm * (n ? n : 1));
        for (uint32_t i = 0; i < n; i++)
            pcm[i] = bits == 16 ? (int16_t)rd16(data + i * 2) : (int16_t)(((int)rd8(data + i) - 128) << 8);
        emit(c, pcm, (int)frames, (int)nch, rate);
        free(pcm);
        return;
    }
    /* compressed (0xFE) */
    uint32_t format = rd32(h + 40);
    if (!data)
        data = h + 64;
    if (format == fourcc("ima4")) {
        play_ima4(c, data, frames, (int)nch, rate);
        return;
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
        emit(c, pcm, (int)frames, (int)nch, rate);
        free(pcm);
        return;
    }
    /* MACE and others: keep the timing, play silence. */
    uint32_t samples = rd16(h + 56) == 3 ? frames * 6 : rd16(h + 56) == 4 ? frames * 12 : frames;
    int16_t *pcm = calloc(samples * nch ? samples * nch : 1, sizeof *pcm);
    emit(c, pcm, (int)samples, (int)nch, rate);
    free(pcm);
}

/* ---------------------------------------------------------------------- */
/* Commands */

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
    guest_call(proc);
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
    case freqCmd: play_note(c, (int)(cmd.p2 & 0xFF), 0.25); break;
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
    case getRateMultiplierCmd: if (cmd.p2) wr32((gaddr)cmd.p2, 0x00010000); break;
    case filePlayDoneCmd: file_play_done(c, true); break;
    default: break;
    }
}

/* Double buffering: each ready buffer goes to the host a little before the
 * one playing ends, then the application's doubleBack refills it. */
#define DB_LEAD_NS 100000000u

static void pump_double(channel *c)
{
    while (c->db && !c->paused && now_ns() + DB_LEAD_NS >= c->busy_until) {
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
            guest_call(back);
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

/* Run queued commands whose time has come. */
static void pump(channel *c)
{
    pump_double(c);
    while (!c->paused && c->head != c->tail && now_ns() >= c->busy_until) {
        sndcmd cmd = c->q[c->head];
        c->head = (c->head + 1) % QUEUE_LEN;
        exec(c, cmd, 0);
    }
}

static bool in_pump;

/* Called at safe points (Event Manager, TickCount, SystemTask). */
void sound_pump(void)
{
    if (in_pump)
        return;
    in_pump = true;
    for (int i = 0; i < MAX_CHANNELS; i++)
        if (chans[i].chan)
            pump(&chans[i]);
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

static sndcmd read_cmd(gaddr p)
{
    return (sndcmd){ rd16(p), (int16_t)rd16(p + 2), (int32_t)rd32(p + 4) };
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
    if (quitnow)
        stop_host(c);
    else
        drain(c);
    bool owned = c->owned;
    c->chan = 0;
    if (owned)
        tb_DisposePtr(chan);
    return noErr;
}

int16_t tb_SndDoCommand(gaddr chan, gaddr cmdp, int8_t nowait)
{
    channel *c = find(chan);
    if (!c)
        return badChannel;
    int16_t err = enqueue(c, read_cmd(cmdp));
    if (err == queueFull && !nowait) {
        drain(c);
        err = enqueue(c, read_cmd(cmdp));
    }
    return err;
}

int16_t tb_SndDoImmediate(gaddr chan, gaddr cmdp)
{
    channel *c = find(chan);
    if (!c)
        return badChannel;
    exec(c, read_cmd(cmdp), cmdp);
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

int16_t tb_SndAddModifier(gaddr chan, gaddr mod, int16_t id, int32_t init) { return noErr; }

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

int32_t tb_SndSoundManagerVersion(void) { return 0x03008000; }
int32_t tb_MACEVersion(void) { return 0x03008000; }
int32_t tb_SPBVersion(void) { return 0x03008000; }

void tb_SndGetSysBeepState(gaddr statep) { wr16(statep, beep_state); }
int16_t tb_SndSetSysBeepState(int16_t state) { beep_state = (uint16_t)state; return noErr; }
int16_t tb_GetSysBeepVolume(gaddr levelp) { wr32(levelp, beep_volume); return noErr; }
int16_t tb_SetSysBeepVolume(int32_t level) { beep_volume = (uint32_t)level; return noErr; }
int16_t tb_GetDefaultOutputVolume(gaddr levelp) { wr32(levelp, 0x01000100); return noErr; }
int16_t tb_SetDefaultOutputVolume(int32_t level) { return noErr; }
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

void sound_reset(void)
{
    for (int i = 0; i < MAX_CHANNELS; i++)
        if (chans[i].chan)
            stop_host(&chans[i]);
    memset(chans, 0, sizeof chans);
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
        guest_call(proc);
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

/* Sound input: there is no input device. */
int16_t tb_SPBOpenDevice(gaddr name, int16_t permission, gaddr refnum)
{
    if (refnum)
        wr32(refnum, 0);
    return -221; /* siNoSoundInHardware */
}
int16_t tb_SPBCloseDevice(int32_t refnum) { return noErr; }

