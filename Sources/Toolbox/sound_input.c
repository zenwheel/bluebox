/*
 * The Sound Input Manager, on the host's microphone.
 *
 * There is one device, "Built-in", which any number of programs may open.
 * The host captures while it is open (machine_set_sound_input_handler) and
 * hands its samples over as they come (machine_sound_input); they are
 * resampled to the device's rate and wait in a ring for a recording to take
 * them.  A recording is paced by the machine's clock: it takes what the host
 * has captured, and silence for whatever the host hasn't given it half a
 * second after it was due, so a run without a microphone (headless, or one
 * the user wouldn't allow) records silence for as long as it asked.
 * Asynchronous recordings fill their buffer (or file) at the sound pump's
 * safe points and then run their completion routine; synchronous ones wait,
 * as they did.  The data is uncompressed: 8-bit offset binary (two's
 * complement when asked) or 16-bit, mono or stereo, at any rate.
 *
 * SndRecord and SndRecordToFile show a small dialog of their own: Record,
 * Stop and Play, the time recorded, and Save or Cancel.
 *
 * SPBParamBlock (mac68k): inRefNum 0, count 4, milliseconds 8, bufferLength
 * 12, bufferPtr 16, completionRoutine 20, interruptRoutine 24, userLong 28,
 * error 32, unused1 34; 38 bytes.
 */
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "machine.h"
#include "qd_internal.h"
#include "tb_internal.h"
#include "trap.h"

enum {
    siBadSoundInDevice = -220, siNoSoundInHardware = -221, siInvalidCompression = -223, siInvalidSampleRate = -225,
    siInvalidSampleSize = -226, siDeviceBusyErr = -227, siBadDeviceName = -228, siBadRefNum = -229,
    siUnknownInfoType = -231, siUnknownQuality = -232, abortErr = -27, userCanceledErr = -128,
};
enum {
    SPB_inRefNum = 0, SPB_count = 4, SPB_milliseconds = 8, SPB_bufferLength = 12, SPB_bufferPtr = 16,
    SPB_completionRoutine = 20, SPB_interruptRoutine = 24, SPB_error = 32,
};
/* SICompletionProcPtr: pascal, the SPBPtr.  SIInterruptProcPtr: register
 * based, the SPBPtr in A0, the buffer in A1, the peak in D0.W and the
 * sample size in D1.L. */
enum { uppSICompletionProcInfo = 0xC0, uppModalFilterProcInfo = 0xFD0 };
#define uppSIInterruptProcInfo (2u | 19u << 11 | 23u << 16 | 2u << 21 | 7u << 26)

#define RATE_22K 0x56EE8BA3u /* 22254.54545 Hz */
#define MAX_RATE 96000.0
#define REFNUM_BASE 0x53490000 /* 'SI' and the open's number */

/* ---- The host ---- */

static machine_sound_input_fn host_fn;
static void *host_ctx;
static bool capturing; /* the host said it is capturing */

void machine_set_sound_input_handler(machine_sound_input_fn fn, void *ctx)
{
    host_fn = fn;
    host_ctx = ctx;
}

/* Captured samples at the device's rate, mono, waiting for a recording. */
#define RING (1 << 18)
static pthread_mutex_t ring_lock = PTHREAD_MUTEX_INITIALIZER;
static int16_t ring[RING];
static uint32_t ring_head, ring_count;
static double ring_rate = 22254.54545; /* the rate the ring holds */
static double phase;                    /* resampling: where the next output falls between inputs */
static float prev_in;

void machine_sound_input(const float *pcm, int frames, int channels, double rate)
{
    if (!pcm || frames <= 0 || channels <= 0 || rate <= 0)
        return;
    pthread_mutex_lock(&ring_lock);
    double step = rate / ring_rate; /* input frames per output frame */
    for (int i = 0; i < frames; i++) {
        float in = 0;
        for (int ch = 0; ch < channels; ch++)
            in += pcm[i * channels + ch];
        in /= (float)channels;
        /* Outputs between the previous input and this one. */
        while (phase < 1.0) {
            float v = prev_in + (in - prev_in) * (float)phase;
            int s = (int)lrintf(v * 32767.0f);
            s = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
            ring[(ring_head + ring_count) % RING] = (int16_t)s;
            if (ring_count < RING)
                ring_count++;
            else
                ring_head = (ring_head + 1) % RING;
            phase += step;
        }
        phase -= 1.0;
        prev_in = in;
    }
    pthread_mutex_unlock(&ring_lock);
}

static void ring_reset(double rate)
{
    pthread_mutex_lock(&ring_lock);
    ring_head = ring_count = 0;
    ring_rate = rate;
    phase = 0;
    prev_in = 0;
    pthread_mutex_unlock(&ring_lock);
}

static uint32_t ring_take(int16_t *out, uint32_t n)
{
    pthread_mutex_lock(&ring_lock);
    if (n > ring_count)
        n = ring_count;
    for (uint32_t i = 0; i < n; i++)
        out[i] = ring[(ring_head + i) % RING];
    ring_head = (ring_head + n) % RING;
    ring_count -= n;
    pthread_mutex_unlock(&ring_lock);
    return n;
}

static uint32_t ring_available(void)
{
    pthread_mutex_lock(&ring_lock);
    uint32_t n = ring_count;
    pthread_mutex_unlock(&ring_lock);
    return n;
}

/* ---- The device ---- */

static struct {
    int opens;
    bool writer;      /* opened with write permission (siWritePermission) */
    int next_open;
    uint32_t rate;    /* UnsignedFixed */
    int16_t size, channels;
    uint32_t compression, quality;
    int16_t agc, meter, playthru, twos, continuous, source;
    uint32_t gain;
    int level;        /* the meter: the last peak, 0-255 */
} dev;

static void device_defaults(void)
{
    dev.rate = RATE_22K;
    dev.size = 8;
    dev.channels = 1;
    dev.compression = fourcc("NONE");
    dev.quality = fourcc("best");
    dev.agc = 1;
    dev.meter = dev.playthru = dev.twos = dev.continuous = 0;
    dev.source = 1;
    dev.gain = 0x8000; /* 0.5 */
    dev.level = 0;
}

static bool refnum_ok(int32_t refnum)
{
    int n = refnum & 0xFFFF;
    return dev.opens > 0 && (refnum & 0xFFFF0000) == REFNUM_BASE && n && n <= dev.next_open;
}

static double rate_hz(uint32_t rate) { return (double)rate / 65536.0; }
static uint32_t frame_bytes(void) { return (uint32_t)(dev.channels * dev.size / 8); }

static uint32_t ms_to_bytes(uint32_t ms)
{
    return (uint32_t)((double)ms * rate_hz(dev.rate) / 1000.0) * frame_bytes();
}

static uint32_t bytes_to_ms(uint32_t bytes)
{
    return (uint32_t)((double)(bytes / frame_bytes()) * 1000.0 / rate_hz(dev.rate) + 0.5);
}

static void host_capture(bool on)
{
    if (on && !capturing && host_fn)
        capturing = host_fn(true, host_ctx);
    else if (!on && capturing) {
        host_fn(false, host_ctx);
        capturing = false;
    }
}

static const char device_name[] = "Built-in";

static void put_pstring(gaddr p, const char *s)
{
    size_t n = strlen(s);
    wr8(p, (uint8_t)n);
    gput(p + 1, s, (uint32_t)n);
}

/* A microphone, as an 'ICN#' (icon and mask). */
static gaddr device_icon(void)
{
    gaddr h = tb_NewHandle(256, false, true);
    if (!h)
        return 0;
    gaddr p = rd32(h);
    for (int y = 0; y < 32; y++) {
        uint32_t row = 0;
        if (y >= 3 && y <= 18)                /* the head, its sides */
            row = (y == 3 || y == 18) ? 0x007FFE00u >> 1 : 0x00C00300u | 0x00400200u;
        if (y >= 6 && y <= 15 && (y % 3) == 0) /* the grille */
            row |= 0x00155400u;
        if (y >= 19 && y <= 26)               /* the stand */
            row |= 0x00018000u;
        if (y == 27 || y == 28)               /* the base */
            row |= 0x001FF800u;
        wr32(p + (gaddr)y * 4, row);
        wr32(p + 128 + (gaddr)y * 4, row ? 0x001FFC00u | row : 0);
    }
    return h;
}

/* SPBGetIndexedDevice(count, deviceName, *deviceIconHandle): the one. */
int16_t tb_SPBGetIndexedDevice(int16_t count, gaddr name, gaddr iconp)
{
    if (count != 1) {
        if (iconp)
            wr32(iconp, 0);
        return siBadSoundInDevice;
    }
    if (name)
        put_pstring(name, device_name);
    if (iconp)
        wr32(iconp, device_icon());
    return noErr;
}

/* SPBOpenDevice(deviceName, permission, *inRefNum): nil or "Built-in". */
int16_t tb_SPBOpenDevice(gaddr name, int16_t permission, gaddr refnum)
{
    if (refnum)
        wr32(refnum, 0);
    if (name && rd8(name)) {
        char s[256];
        int n = rd8(name);
        for (int i = 0; i < n; i++)
            s[i] = (char)rd8(name + 1 + (gaddr)i);
        s[n] = 0;
        if (strcmp(s, device_name))
            return siBadDeviceName;
    }
    if (permission == 1 && dev.writer) /* siWritePermission: one at a time */
        return siDeviceBusyErr;
    if (!dev.opens) {
        device_defaults();
        dev.next_open = 0;
        ring_reset(rate_hz(dev.rate));
        host_capture(true);
    }
    dev.opens++;
    if (permission == 1)
        dev.writer = true;
    if (refnum)
        wr32(refnum, (uint32_t)(REFNUM_BASE + ++dev.next_open));
    return noErr;
}

/* ---- Recording ---- */

static struct {
    bool active, paused, async;
    gaddr pb;           /* the SPB, or 0 for SndRecord's own recording */
    int32_t refnum;
    int16_t file;       /* SPBRecordToFile's file, else 0 */
    gaddr buffer;       /* where the bytes go (0: nowhere, or a file) */
    uint8_t *host_buffer; /* SndRecord's: grows as it records */
    uint32_t host_size;
    uint32_t total;     /* bytes to record, 0 until stopped */
    uint32_t done;      /* bytes recorded */
    uint64_t start_ns, pause_ns, paused_ns;
    uint64_t frames;    /* frames taken */
} rec;

static void stop_recording(int16_t result);

/* Frames to bytes as the device keeps them, at `out`. */
static void store(const int16_t *mono, uint32_t frames, uint8_t *out)
{
    for (uint32_t i = 0; i < frames; i++)
        for (int ch = 0; ch < dev.channels; ch++) {
            int16_t s = mono[i];
            if (dev.size == 16) {
                *out++ = (uint8_t)((uint16_t)s >> 8);
                *out++ = (uint8_t)s;
            } else {
                *out++ = (uint8_t)((s >> 8) + (dev.twos ? 0 : 128));
            }
        }
}

static void write_file(int16_t refnum, const uint8_t *data, uint32_t n)
{
    gaddr buf = tb_NewPtr((int32_t)n, false, false), pb = tb_NewPtr(80, false, true);
    if (buf && pb) {
        gput(buf, data, n);
        wr16(pb + 24, (uint16_t)refnum); /* ioRefNum */
        wr32(pb + 32, buf);              /* ioBuffer */
        wr32(pb + 36, n);                /* ioReqCount */
        wr16(pb + 44, 0);                /* ioPosMode: fsAtMark */
        tb_PBWrite(pb, false);
    }
    if (buf)
        tb_DisposePtr(buf);
    if (pb)
        tb_DisposePtr(pb);
}

/* Take what is due: the host's samples, else (late) silence. */
static void record_pump(void)
{
    if (!rec.active || rec.paused)
        return;
    double hz = rate_hz(dev.rate);
    uint64_t elapsed = machine_time_ns() - rec.start_ns - rec.paused_ns;
    uint64_t due = (uint64_t)((double)elapsed * hz / 1e9);
    uint32_t fb = frame_bytes();
    uint64_t room = rec.total ? (rec.total - rec.done) / fb : UINT64_MAX;
    uint64_t want = due > rec.frames ? due - rec.frames : 0;
    uint32_t avail = capturing ? ring_available() : 0;
    /* The host's samples as they come (a little ahead of the clock is
     * fine); silence only for what it is half a second late with. */
    uint64_t take = avail;
    uint64_t late = (uint64_t)(hz / 2);
    if (!capturing)
        take = want;
    else if (want > avail + late)
        take = want - late;
    if (take > room)
        take = room;
    if (take > (uint64_t)hz * 4)
        take = (uint64_t)hz * 4;
    if (take) {
        int16_t *mono = calloc((size_t)take, sizeof *mono);
        uint8_t *bytes = malloc((size_t)take * fb);
        if (!mono || !bytes) {
            free(mono);
            free(bytes);
            return;
        }
        if (capturing)
            ring_take(mono, (uint32_t)take); /* any shortfall stays silent */
        int peak = 0;
        for (uint64_t i = 0; i < take; i++)
            peak = abs(mono[i]) > peak ? abs(mono[i]) : peak;
        dev.level = peak >> 7;
        store(mono, (uint32_t)take, bytes);
        uint32_t n = (uint32_t)take * fb;
        gaddr at = 0;
        if (rec.file) {
            write_file(rec.file, bytes, n);
        } else if (rec.buffer) {
            at = rec.buffer + rec.done;
            gput(at, bytes, n);
        } else if (!rec.pb) {
            if (rec.done + n > rec.host_size) {
                uint32_t size = (rec.done + n) * 2;
                uint8_t *grown = realloc(rec.host_buffer, size);
                if (grown)
                    rec.host_buffer = grown, rec.host_size = size;
            }
            if (rec.done + n <= rec.host_size)
                memcpy(rec.host_buffer + rec.done, bytes, n);
        }
        free(mono);
        free(bytes);
        rec.done += n;
        rec.frames += take;
        gaddr ir = rec.pb ? rd32(rec.pb + SPB_interruptRoutine) : 0;
        if (ir && at) {
            uint32_t args[4] = { rec.pb, at, (uint32_t)(dev.level << 7), (uint32_t)dev.size };
            mm_call(ir, uppSIInterruptProcInfo, args, 4);
        }
    }
    if (rec.total && rec.done + fb > rec.total)
        stop_recording(noErr);
}

/* The recording ends: its SPB says how much, and an asynchronous one's
 * completion routine runs. */
static void stop_recording(int16_t result)
{
    if (!rec.active)
        return;
    rec.active = false;
    gaddr pb = rec.pb;
    if (!pb)
        return;
    wr32(pb + SPB_count, rec.done);
    wr32(pb + SPB_milliseconds, bytes_to_ms(rec.done));
    wr16(pb + SPB_error, (uint16_t)result);
    gaddr done = rd32(pb + SPB_completionRoutine);
    if (rec.async && done) {
        uint32_t arg = pb;
        mm_call(done, uppSICompletionProcInfo, &arg, 1);
    }
}

bool sound_input_busy(void) { return rec.active && rec.async; }

void sound_input_pump(void) { record_pump(); }

static void start_recording(gaddr pb, int16_t file, bool async)
{
    uint32_t count = rd32(pb + SPB_count), ms = rd32(pb + SPB_milliseconds);
    gaddr buffer = file ? 0 : rd32(pb + SPB_bufferPtr);
    uint32_t length = rd32(pb + SPB_bufferLength);
    memset(&rec, 0, sizeof rec);
    rec.pb = pb;
    rec.refnum = (int32_t)rd32(pb + SPB_inRefNum);
    rec.file = file;
    rec.buffer = buffer;
    rec.async = async;
    rec.total = count ? count : ms ? ms_to_bytes(ms) : buffer ? length : 0;
    if (buffer && length && (!rec.total || rec.total > length))
        rec.total = length;
    rec.total -= rec.total % frame_bytes();
    rec.active = true;
    rec.start_ns = machine_time_ns();
    ring_reset(rate_hz(dev.rate));
    if (!capturing)
        host_capture(true);
    wr16(pb + SPB_error, async ? ioInProgress : 0);
}

/* Until the recording is done, with interrupt-time work going on. */
static void wait_recording(void)
{
    while (rec.active && !g_cpu->halted) {
        if (events_host_attached()) {
            struct timespec ts = { 0, 5000000 };
            nanosleep(&ts, NULL);
        } else if (rec.total) {
            uint64_t ns = (uint64_t)((double)((rec.total - rec.done) / frame_bytes()) * 1e9 / rate_hz(dev.rate));
            machine_skip_time(ns + 1000000);
        } else {
            break; /* recording until stopped: nothing will stop it */
        }
        machine_advance_ticks();
        record_pump();
        sound_pump();
        time_pump();
    }
}

static int16_t record(gaddr pb, int16_t file, bool async)
{
    if (!pb || !refnum_ok((int32_t)rd32(pb + SPB_inRefNum)))
        return siBadRefNum;
    if (rec.active)
        return siDeviceBusyErr;
    start_recording(pb, file, async);
    record_pump();
    if (async)
        return noErr;
    rec.async = false;
    if (!rec.total) {
        stop_recording(noErr);
        return noErr;
    }
    wait_recording();
    if (rec.active)
        stop_recording(abortErr);
    return (int16_t)rd16(pb + SPB_error);
}

int16_t tb_SPBRecord(gaddr pb, int8_t async) { return record(pb, 0, async != 0); }
int16_t tb_SPBRecordToFile(int16_t refnum, gaddr pb, int8_t async)
{
    return refnum ? record(pb, refnum, async != 0) : paramErr;
}

int16_t tb_SPBStopRecording(int32_t refnum)
{
    if (!refnum_ok(refnum))
        return siBadRefNum;
    if (rec.active && rec.refnum == refnum) {
        record_pump();
        stop_recording(abortErr);
    }
    return noErr;
}

int16_t tb_SPBPauseRecording(int32_t refnum)
{
    if (!refnum_ok(refnum))
        return siBadRefNum;
    if (rec.active && !rec.paused) {
        record_pump();
        rec.paused = true;
        rec.pause_ns = machine_time_ns();
    }
    return noErr;
}

int16_t tb_SPBResumeRecording(int32_t refnum)
{
    if (!refnum_ok(refnum))
        return siBadRefNum;
    if (rec.active && rec.paused) {
        rec.paused = false;
        rec.paused_ns += machine_time_ns() - rec.pause_ns;
        ring_reset(rate_hz(dev.rate));
    }
    return noErr;
}

int16_t tb_SPBCloseDevice(int32_t refnum)
{
    if (!refnum_ok(refnum))
        return siBadRefNum;
    if (rec.active && rec.refnum == refnum)
        stop_recording(abortErr);
    if (--dev.opens <= 0) {
        dev.opens = 0;
        dev.writer = false;
        host_capture(false);
    }
    return noErr;
}

/* SPBGetRecordingStatus(refnum, *status, *meterLevel, *totalSamplesToRecord,
 * *numberOfSamplesRecorded, *totalMsecsToRecord, *numberOfMsecsRecorded) */
int16_t tb_SPBGetRecordingStatus(int32_t refnum, gaddr status, gaddr meter, gaddr total_samps, gaddr samps,
                                 gaddr total_ms, gaddr ms)
{
    if (!refnum_ok(refnum))
        return siBadRefNum;
    record_pump();
    bool mine = rec.refnum == refnum;
    uint32_t sample_bytes = (uint32_t)(dev.size / 8);
    if (status)
        wr16(status, mine && rec.active ? 1 : 0);
    if (meter)
        wr16(meter, (uint16_t)dev.level);
    if (total_samps)
        wr32(total_samps, mine ? rec.total / sample_bytes : 0);
    if (samps)
        wr32(samps, mine ? rec.done / sample_bytes : 0);
    if (total_ms)
        wr32(total_ms, mine ? bytes_to_ms(rec.total) : 0);
    if (ms)
        wr32(ms, mine ? bytes_to_ms(rec.done) : 0);
    return noErr;
}

int16_t tb_SPBMillisecondsToBytes(int32_t refnum, gaddr millip)
{
    if (!refnum_ok(refnum))
        return siBadRefNum;
    wr32(millip, ms_to_bytes(rd32(millip)));
    return noErr;
}

int16_t tb_SPBBytesToMilliseconds(int32_t refnum, gaddr bytesp)
{
    if (!refnum_ok(refnum))
        return siBadRefNum;
    wr32(bytesp, bytes_to_ms(rd32(bytesp)));
    return noErr;
}

int16_t tb_SPBSignInDevice(int16_t refnum, gaddr name) { return noErr; }
int16_t tb_SPBSignOutDevice(int16_t refnum) { return noErr; }

/* ---- Device information ---- */

/* A count (short) and a handle of n values of size each. */
static void put_list(gaddr infop, const uint32_t *v, int n, int size)
{
    gaddr h = tb_NewHandle(n * size, false, true);
    for (int i = 0; h && i < n; i++) {
        if (size == 2)
            wr16(rd32(h) + (gaddr)(i * 2), (uint16_t)v[i]);
        else
            wr32(rd32(h) + (gaddr)(i * 4), v[i]);
    }
    wr16(infop, (uint16_t)n);
    wr32(infop + 2, h);
}

int16_t tb_SPBGetDeviceInfo(int32_t refnum, int32_t info, gaddr infop)
{
    if (!refnum_ok(refnum))
        return siBadRefNum;
    if (!infop)
        return paramErr;
    uint32_t sel = (uint32_t)info;
    if (sel == fourcc("name")) {
        put_pstring(infop, device_name);
    } else if (sel == fourcc("icon")) {
        wr32(infop, device_icon());
    } else if (sel == fourcc("srat")) {
        wr32(infop, dev.rate);
    } else if (sel == fourcc("srav")) {
        static const uint32_t rates[] = { 0x2B7745D1u /* 11127 */, 0x2B110000u /* 11025 */, RATE_22K,
                                          0x56220000u /* 22050 */, 0xAC440000u /* 44100 */, 0xBB800000u /* 48000 */ };
        put_list(infop, rates, 6, 4);
    } else if (sel == fourcc("ssiz")) {
        wr16(infop, (uint16_t)dev.size);
    } else if (sel == fourcc("ssav")) {
        static const uint32_t sizes[] = { 8, 16 };
        put_list(infop, sizes, 2, 2);
    } else if (sel == fourcc("chan")) {
        wr16(infop, (uint16_t)dev.channels);
    } else if (sel == fourcc("chav")) {
        wr16(infop, 2);
    } else if (sel == fourcc("comp")) {
        wr32(infop, dev.compression);
    } else if (sel == fourcc("cmav")) {
        uint32_t none = fourcc("NONE");
        put_list(infop, &none, 1, 4);
    } else if (sel == fourcc("qual")) {
        wr32(infop, dev.quality);
    } else if (sel == fourcc("agc ")) {
        wr16(infop, (uint16_t)dev.agc);
    } else if (sel == fourcc("lmet")) {
        record_pump();
        wr16(infop, (uint16_t)dev.meter);
        wr16(infop + 2, (uint16_t)dev.level);
    } else if (sel == fourcc("plth")) {
        wr16(infop, (uint16_t)dev.playthru);
    } else if (sel == fourcc("twos")) {
        wr16(infop, (uint16_t)dev.twos);
    } else if (sel == fourcc("cont")) {
        wr16(infop, (uint16_t)dev.continuous);
    } else if (sel == fourcc("asyn")) {
        wr16(infop, 1);
    } else if (sel == fourcc("dcon")) {
        wr16(infop, 1); /* siDeviceIsConnected */
    } else if (sel == fourcc("optd")) {
        wr16(infop, 0); /* no options dialog */
    } else if (sel == fourcc("sour")) {
        wr16(infop, (uint16_t)dev.source);
    } else if (sel == fourcc("snam")) {
        gaddr h = tb_NewHandle(2 + 11, false, true);
        if (h) {
            wr16(rd32(h), 1);
            put_pstring(rd32(h) + 2, "Microphone");
        }
        wr32(infop, h);
    } else if (sel == fourcc("gain")) {
        wr32(infop, dev.gain);
    } else if (sel == fourcc("sgai")) {
        wr32(infop, dev.gain);
        wr32(infop + 4, dev.gain);
    } else if (sel == fourcc("dbin")) {
        wr32(infop, 4096);
    } else {
        return siUnknownInfoType;
    }
    return noErr;
}

int16_t tb_SPBSetDeviceInfo(int32_t refnum, int32_t info, gaddr infop)
{
    if (!refnum_ok(refnum))
        return siBadRefNum;
    if (!infop)
        return paramErr;
    uint32_t sel = (uint32_t)info;
    if (rec.active && (sel == fourcc("srat") || sel == fourcc("ssiz") || sel == fourcc("chan")))
        return siDeviceBusyErr;
    if (sel == fourcc("srat")) {
        uint32_t r = rd32(infop);
        if (rate_hz(r) < 1000 || rate_hz(r) > MAX_RATE)
            return siInvalidSampleRate;
        dev.rate = r;
        ring_reset(rate_hz(r));
    } else if (sel == fourcc("ssiz")) {
        int16_t s = (int16_t)rd16(infop);
        if (s != 8 && s != 16)
            return siInvalidSampleSize;
        dev.size = s;
    } else if (sel == fourcc("chan")) {
        int16_t n = (int16_t)rd16(infop);
        if (n != 1 && n != 2)
            return paramErr;
        dev.channels = n;
    } else if (sel == fourcc("comp")) {
        uint32_t c = rd32(infop);
        if (c != fourcc("NONE") && c != 0)
            return siInvalidCompression;
        dev.compression = fourcc("NONE");
    } else if (sel == fourcc("qual")) {
        uint32_t q = rd32(infop);
        if (q != fourcc("best") && q != fourcc("betr") && q != fourcc("good") && q != fourcc("none"))
            return siUnknownQuality;
        dev.quality = q; /* recorded uncompressed whatever the quality */
    } else if (sel == fourcc("agc ")) {
        dev.agc = (int16_t)rd16(infop);
    } else if (sel == fourcc("lmet")) {
        dev.meter = (int16_t)rd16(infop);
    } else if (sel == fourcc("plth")) {
        dev.playthru = (int16_t)rd16(infop);
    } else if (sel == fourcc("twos")) {
        dev.twos = (int16_t)rd16(infop);
    } else if (sel == fourcc("cont")) {
        dev.continuous = (int16_t)rd16(infop);
    } else if (sel == fourcc("sour")) {
        dev.source = (int16_t)rd16(infop);
    } else if (sel == fourcc("gain")) {
        dev.gain = rd32(infop);
    } else if (sel == fourcc("optd")) {
        /* no options to show */
    } else {
        return siUnknownInfoType;
    }
    return noErr;
}

/* ---- SndRecord and SndRecordToFile ---- */

enum { itemSave = 1, itemCancel, itemRecord, itemStop, itemPlay, itemTime, nItems = itemTime };

static gaddr record_dialog, user_filter, filter_stub;
static uint32_t record_limit; /* bytes SndRecord may keep, 0 for no limit */
static uint32_t shown_ms = UINT32_MAX;

static int16_t ditl_item(gaddr p, qrect r, uint8_t type, const char *text)
{
    size_t n = strlen(text);
    wr32(p, 0);
    qrect_put(p + 4, r);
    wr8(p + 12, type);
    wr8(p + 13, (uint8_t)n);
    gput(p + 14, text, (uint32_t)n);
    return (int16_t)(14 + (((uint32_t)n + 1) & ~1u));
}

static void set_item_text(int item, const char *text)
{
    gaddr scratch = tb_NewPtr(256 + 16, false, true);
    gaddr type = scratch + 256, h = scratch + 258, r = scratch + 262;
    tb_GetDialogItem(record_dialog, (int16_t)item, type, h, r);
    put_pstring(scratch, text);
    if (rd32(h))
        tb_SetDialogItemText(rd32(h), scratch);
    tb_DisposePtr(scratch);
}

static void hilite_item(int item, bool enabled)
{
    gaddr scratch = tb_NewPtr(16, false, true);
    tb_GetDialogItem(record_dialog, (int16_t)item, scratch, scratch + 2, scratch + 6);
    if (rd32(scratch + 2))
        tb_HiliteControl(rd32(scratch + 2), enabled ? 0 : 255);
    tb_DisposePtr(scratch);
}

static void show_state(bool force)
{
    uint32_t ms = bytes_to_ms(rec.done);
    if (!force && ms / 1000 == shown_ms / 1000)
        return;
    shown_ms = ms;
    char text[64];
    if (record_limit)
        snprintf(text, sizeof text, "%u:%02u of %u:%02u", ms / 60000, ms / 1000 % 60, bytes_to_ms(record_limit) / 60000,
                 bytes_to_ms(record_limit) / 1000 % 60);
    else
        snprintf(text, sizeof text, "%u:%02u", ms / 60000, ms / 1000 % 60);
    set_item_text(itemTime, text);
    hilite_item(itemRecord, !rec.active);
    hilite_item(itemStop, rec.active);
    hilite_item(itemPlay, !rec.active && rec.done > 0);
    hilite_item(itemSave, !rec.active && rec.done > 0);
}

/* The dialog's filter: the program's own first, then ours (the recording
 * going on, Return and Escape). */
static void record_filter(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    gaddr itemp = cpu_pop32(c), evt = cpu_pop32(c), d = cpu_pop32(c);
    bool r = false;
    if (user_filter) {
        uint32_t args[3] = { d, evt, itemp };
        r = (mm_call(user_filter, uppModalFilterProcInfo, args, 3) & 0xFF) != 0;
    }
    if (!r) {
        record_pump();
        if (record_limit && rec.active && rec.done >= record_limit)
            stop_recording(noErr);
        show_state(false);
        if (rd16(evt) == 3 /* keyDown */) {
            uint8_t ch = (uint8_t)rd32(evt + 2);
            if (ch == 13 || ch == 3) {
                wr16(itemp, rec.done && !rec.active ? itemSave : itemRecord);
                r = true;
            } else if (ch == 27) {
                wr16(itemp, itemCancel);
                r = true;
            }
        }
    }
    wr16(c->a[7], r ? 0x0100 : 0);
    cpu_push32(c, ret);
}

/* The recording as a playable 'snd ' (format 1, a sound header). */
static gaddr recording_sound(void)
{
    gaddr h = tb_NewHandle(0, false, true), len = tb_NewPtr(2, false, true);
    if (!h || !len)
        return 0;
    tb_SetupSndHeader(h, dev.channels, (int32_t)dev.rate, dev.size, (int32_t)fourcc("NONE"), 60, (int32_t)rec.done,
                      len);
    uint32_t hlen = rd16(len);
    tb_DisposePtr(len);
    tb_SetHandleSize(h, (int32_t)(hlen + rec.done));
    if (tb_GetHandleSize(h) != (int32_t)(hlen + rec.done)) {
        tb_DisposeHandle(h);
        return 0;
    }
    if (rec.done)
        gput(rd32(h) + hlen, rec.host_buffer, rec.done);
    return h;
}

/* Run the dialog; true if the user saved a recording (in rec.host_buffer). */
static bool record_dialog_run(gaddr filter, uint32_t corner, uint32_t limit)
{
    int16_t top = (int16_t)(corner >> 16), left = (int16_t)corner;
    if (top <= 20 && left <= 0)
        top = 80, left = (int16_t)((g_screen_width - 300) / 2);
    gaddr ditl = tb_NewHandle(2 + nItems * (14 + 32), false, true);
    if (!ditl)
        return false;
    gaddr p = rd32(ditl) + 2;
    p += (gaddr)ditl_item(p, (qrect){ 90, 214, 110, 284 }, 4, "Save");
    p += (gaddr)ditl_item(p, (qrect){ 90, 130, 110, 200 }, 4, "Cancel");
    p += (gaddr)ditl_item(p, (qrect){ 50, 16, 70, 86 }, 4, "Record");
    p += (gaddr)ditl_item(p, (qrect){ 50, 114, 70, 184 }, 4, "Stop");
    p += (gaddr)ditl_item(p, (qrect){ 50, 212, 70, 282 }, 4, "Play");
    ditl_item(p, (qrect){ 14, 16, 32, 284 }, 8 | 0x80, "0:00");
    wr16(rd32(ditl), nItems - 1);
    gaddr scratch = tb_NewPtr(32, false, true);
    qrect_put(scratch, (qrect){ top, left, (int16_t)(top + 124), (int16_t)(left + 300) });
    record_dialog = tb_NewDialog(0, scratch, scratch + 8, 1, 1 /* dBoxProc */, 0xFFFFFFFF, 0, 0, ditl);
    if (!record_dialog) {
        tb_DisposePtr(scratch);
        return false;
    }
    tb_SetDialogDefaultItem(record_dialog, itemSave);
    tb_SetDialogCancelItem(record_dialog, itemCancel);
    if (!filter_stub)
        filter_stub = native_register(record_filter, "SndRecordFilter", 0);
    user_filter = filter;
    record_limit = limit;
    memset(&rec, 0, sizeof rec);
    shown_ms = UINT32_MAX;
    show_state(true);
    gaddr hit = scratch + 16;
    int16_t n;
    do {
        tb_ModalDialog(filter_stub, hit);
        n = (int16_t)rd16(hit);
        if (n == itemRecord && !rec.active) {
            free(rec.host_buffer);
            memset(&rec, 0, sizeof rec);
            rec.active = true;
            rec.total = limit;
            rec.start_ns = machine_time_ns();
            ring_reset(rate_hz(dev.rate));
        } else if (n == itemStop && rec.active) {
            record_pump();
            stop_recording(noErr);
        } else if (n == itemPlay && !rec.active && rec.done) {
            gaddr snd = recording_sound();
            if (snd) {
                tb_SndPlay(0, snd, false);
                tb_DisposeHandle(snd);
            }
        }
        show_state(true);
    } while (n != itemCancel && !(n == itemSave && !rec.active && rec.done) && !g_cpu->halted);
    if (rec.active) {
        record_pump();
        stop_recording(noErr);
    }
    tb_DisposeDialog(record_dialog);
    record_dialog = 0;
    user_filter = 0;
    tb_DisposePtr(scratch);
    return n == itemSave;
}

/* Open the device for the dialog (as SndRecord did), the quality's format. */
static int16_t open_for_dialog(int32_t quality, int32_t *refnum)
{
    gaddr r = tb_NewPtr(4, false, true);
    int16_t err = tb_SPBOpenDevice(0, 1, r);
    *refnum = (int32_t)rd32(r);
    tb_DisposePtr(r);
    if (err)
        return err;
    dev.rate = RATE_22K;
    dev.size = 8;
    dev.channels = 1;
    dev.twos = 0;
    dev.quality = (uint32_t)quality;
    return noErr;
}

/* SndRecord(filterProc, corner, quality, *sndHandle): into the handle (no
 * longer than it is) or a new one. */
int16_t tb_SndRecord(gaddr filterp, uint32_t corner, int32_t quality, gaddr sndhandlep)
{
    uint32_t q = (uint32_t)quality;
    if (q != fourcc("best") && q != fourcc("betr") && q != fourcc("good"))
        return siUnknownQuality;
    int32_t refnum;
    int16_t err = open_for_dialog(quality, &refnum);
    if (err)
        return err;
    /* Room for the samples after a standard sound header (42 bytes), or
     * ten minutes. */
    gaddr given = sndhandlep ? rd32(sndhandlep) : 0;
    uint32_t size = given ? (uint32_t)tb_GetHandleSize(given) : 0;
    uint32_t limit = !given ? (uint32_t)(rate_hz(dev.rate) * 600) : size > 42 ? size - 42 : 0;
    if (!limit) {
        tb_SPBCloseDevice(refnum);
        return memFullErr;
    }
    bool saved = record_dialog_run(filterp, corner, limit);
    if (saved) {
        gaddr snd = recording_sound();
        if (!snd) {
            err = memFullErr;
        } else if (given) {
            int32_t size = tb_GetHandleSize(snd);
            tb_SetHandleSize(given, size);
            gmemcpy(rd32(given), rd32(snd), (uint32_t)size);
            tb_DisposeHandle(snd);
        } else if (sndhandlep) {
            wr32(sndhandlep, snd);
        }
    } else {
        err = userCanceledErr;
    }
    free(rec.host_buffer);
    memset(&rec, 0, sizeof rec);
    tb_SPBCloseDevice(refnum);
    return err;
}

/* SndRecordToFile(filterProc, corner, quality, fRefNum): an AIFF file. */
int16_t tb_SndRecordToFile(gaddr filterp, uint32_t corner, int32_t quality, int16_t refnum)
{
    uint32_t q = (uint32_t)quality;
    if (q != fourcc("best") && q != fourcc("betr") && q != fourcc("good"))
        return siUnknownQuality;
    int32_t dref;
    int16_t err = open_for_dialog(quality, &dref);
    if (err)
        return err;
    bool saved = record_dialog_run(filterp, corner, (uint32_t)(rate_hz(dev.rate) * 600));
    if (saved) {
        /* AIFF samples are signed. */
        for (uint32_t i = 0; i < rec.done; i++)
            rec.host_buffer[i] ^= 0x80;
        gaddr pb = tb_NewPtr(80, false, true);
        wr16(pb + 24, (uint16_t)refnum);
        wr16(pb + 44, 1); /* fsFromStart */
        wr32(pb + 46, 0);
        tb_PBSetFPos(pb, false);
        tb_DisposePtr(pb);
        err = tb_SetupAIFFHeader(refnum, 1, (int32_t)dev.rate, 8, (int32_t)fourcc("NONE"), (int32_t)rec.done,
                                 (int32_t)rec.done);
        if (!err)
            write_file(refnum, rec.host_buffer, rec.done);
    } else {
        err = userCanceledErr;
    }
    free(rec.host_buffer);
    memset(&rec, 0, sizeof rec);
    tb_SPBCloseDevice(dref);
    return err;
}

void sound_input_reset(void)
{
    if (dev.opens)
        host_capture(false);
    free(rec.host_buffer);
    memset(&rec, 0, sizeof rec);
    memset(&dev, 0, sizeof dev);
    device_defaults();
    filter_stub = record_dialog = user_filter = 0;
    capturing = false;
}
