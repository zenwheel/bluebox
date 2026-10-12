/*
 * Sound Manager.
 *
 * Each channel keeps its command queue here and plays sampled sound and
 * notes through the host (machine_set_sound_handler).  The queue is paced
 * by wall-clock time: a command that plays sound keeps the channel busy
 * for the sound's length, and later commands (callbacks included) run at
 * the next safe point after that — the Event Manager calls, TickCount and
 * SystemTask — much as interrupt-time completion did.  Without a host the
 * timing is the same and nothing is heard.
 *
 * Supported: 'snd ' formats 1 and 2; standard and extended sound headers
 * (8-bit unsigned and 16-bit signed, mono and stereo); square-wave and
 * wave-table notes; SysBeep.  Compressed (MACE, IMA) headers play silence
 * of the right length for now.
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

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

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
    /* compressed (0xFE): keep the timing, play silence */
    int16_t *pcm = calloc(frames * nch ? frames * nch : 1, sizeof *pcm);
    emit(c, pcm, (int)frames, (int)nch, rate);
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
        pump(c);
        if (c->head == c->tail && now_ns() >= c->busy_until)
            return;
        struct timespec ts = { 0, 2000000 };
        nanosleep(&ts, NULL);
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
