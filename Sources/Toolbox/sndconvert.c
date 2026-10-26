/*
 * The Sound Manager 3.2 Sound Converter (SoundLib): sound from one format
 * to another, buffer by buffer.  The input is decoded by QuickTime's sound
 * decoders (uncompressed 8- and 16-bit, IMA 4:1, MACE 3:1 and 6:1), mixed
 * to the output's channels, taken to its rate by linear interpolation (the
 * last input frame kept for the next buffer), and written uncompressed.
 * ParseSndHeader describes a 'snd ' resource's sound the same way.
 *
 * SoundComponentData: flags 0, format 4, numChannels 8, sampleSize 10,
 * sampleRate 12 (UnsignedFixed), sampleCount 16, buffer 20; 28 bytes.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "os9svc.h"
#include "ppc_bridge.h"
#include "qt_codec.h"
#include "tb_internal.h"

enum { badFormat = -206, notEnoughBufferSpace = -207, kNoSampleRateConversion = 1 << 1 };

typedef struct {
    bool used;
    qt_sound_format in;
    uint32_t out_format;
    int out_channels, out_bits;
    double out_rate;
    bool resample;
    double pos;       /* the next output frame's place, in input frames after `last` */
    int16_t last[2];  /* the last input frame converted, at the output's channels */
    bool have_last;
    int32_t roll_off;  /* the rate converter's filter slope, dB (siRateConverterRollOffSlope) */
} converter;

enum { MAX_CONVERTERS = 16, CONVERTER_TAG = 0x53430000 };
static converter converters[MAX_CONVERTERS];

void sound_converters_reset(void)
{
    memset(converters, 0, sizeof converters);
    os9svc_sound_reset();
}

static bool is(uint32_t f, const char *s) { return f == fourcc(s); }

static bool uncompressed(uint32_t f) { return is(f, "raw ") || is(f, "twos") || is(f, "sowt") || is(f, "NONE"); }

/* A SoundComponentData's format: 'NONE' is offset binary at 8 bits and
 * big-endian twos-complement at 16. */
static uint32_t real_format(uint32_t f, int bits)
{
    return is(f, "NONE") ? fourcc(bits == 8 ? "raw " : "twos") : f;
}

static converter *conv_of(uint32_t sc)
{
    uint32_t i = sc - CONVERTER_TAG;
    return (sc & 0xFFFF0000u) == CONVERTER_TAG && i < MAX_CONVERTERS && converters[i].used ? &converters[i] : NULL;
}

/* Bytes in one frame of the input (a packet, for the compressed formats)
 * and the samples it holds. */
static uint32_t frame_bytes(const qt_sound_format *f)
{
    if (is(f->format, "ima4"))
        return 34 * (uint32_t)f->channels;
    if (is(f->format, "MAC3"))
        return 2 * (uint32_t)f->channels;
    if (is(f->format, "MAC6"))
        return (uint32_t)f->channels;
    return (uint32_t)(f->channels * f->bits / 8);
}

static uint32_t frame_samples(const qt_sound_format *f)
{
    return is(f->format, "ima4") ? 64 : is(f->format, "MAC3") || is(f->format, "MAC6") ? 6 : 1;
}

static double fixed_rate(uint32_t r) { return r / 65536.0; }

/* SoundConverterOpen(inputFormat, outputFormat, *sc) */
static int16_t sc_open(gaddr in, gaddr out, gaddr scp)
{
    if (scp)
        wr32(scp, 0);
    if (!in || !out || !scp)
        return paramErr;
    int in_bits = (int16_t)rd16(in + 10), out_bits = (int16_t)rd16(out + 10);
    qt_sound_format f = { real_format(rd32(in + 4), in_bits), (int16_t)rd16(in + 8), in_bits, fixed_rate(rd32(in + 12)) };
    if (!uncompressed(f.format))
        f.bits = 16;
    uint32_t of = real_format(rd32(out + 4), out_bits);
    int och = (int16_t)rd16(out + 8);
    double orate = fixed_rate(rd32(out + 12));
    if (!qt_adec_supported(&f) || !uncompressed(of) || (out_bits != 8 && out_bits != 16) || och < 1 || och > 2)
        return badFormat;
    for (int i = 0; i < MAX_CONVERTERS; i++)
        if (!converters[i].used) {
            converter *c = &converters[i];
            memset(c, 0, sizeof *c);
            c->used = true;
            c->in = f;
            c->out_format = of;
            c->out_channels = och;
            c->out_bits = out_bits;
            c->out_rate = orate >= 1000 ? orate : f.rate;
            c->roll_off = 60;
            c->resample = !(rd32(out) & kNoSampleRateConversion) && fabs(c->out_rate - f.rate) > 0.5;
            wr32(scp, CONVERTER_TAG + (uint32_t)i);
            return noErr;
        }
    return memFullErr;
}

/* Output frames for this many input frames, at most. */
static uint32_t out_frames_for(const converter *c, uint32_t in_frames)
{
    double samples = (double)in_frames * frame_samples(&c->in);
    return c->resample ? (uint32_t)ceil(samples * c->out_rate / c->in.rate) + 1 : (uint32_t)samples;
}

static uint32_t out_frame_bytes(const converter *c) { return (uint32_t)(c->out_channels * c->out_bits / 8); }

static void put_sample(const converter *c, uint8_t *p, int16_t v)
{
    if (c->out_bits == 8)
        *p = is(c->out_format, "raw ") ? (uint8_t)((v >> 8) + 128) : (uint8_t)(v >> 8);
    else if (is(c->out_format, "sowt"))
        p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8);
    else
        p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v;
}

/* Convert input bytes; the output frames written to out (host memory, of
 * room for out_frames_for frames). */
static uint32_t convert(converter *c, const uint8_t *data, size_t len, uint8_t *out)
{
    int16_t *pcm = NULL;
    uint32_t frames = len ? qt_adec_decode(&c->in, data, len, &pcm) : 0;
    if (!frames) {
        free(pcm);
        return 0;
    }
    /* The input at the output's channels, after the frame kept from before. */
    int nch = c->out_channels, ich = c->in.channels;
    uint32_t n = frames + (c->have_last ? 1 : 0);
    int16_t *src = malloc(sizeof *src * n * (size_t)nch);
    if (c->have_last)
        for (int k = 0; k < nch; k++)
            src[k] = c->last[k];
    uint32_t at = c->have_last ? 1 : 0;
    for (uint32_t i = 0; i < frames; i++, at++)
        for (int k = 0; k < nch; k++) {
            int v = ich == nch ? pcm[i * (uint32_t)ich + (uint32_t)k]
                    : ich == 1 ? pcm[i]
                               : (pcm[i * 2] + pcm[i * 2 + 1]) / 2;
            src[at * (uint32_t)nch + (uint32_t)k] = (int16_t)v;
        }
    free(pcm);
    uint32_t done = 0, fb = out_frame_bytes(c), ss = (uint32_t)c->out_bits / 8;
    if (!c->resample) {
        for (uint32_t i = c->have_last ? 1 : 0; i < n; i++, done++)
            for (int k = 0; k < nch; k++)
                put_sample(c, out + done * fb + (uint32_t)k * ss, src[i * (uint32_t)nch + (uint32_t)k]);
    } else {
        double step = c->in.rate / c->out_rate, t = c->pos;
        for (; t <= (double)(n - 1); t += step, done++) {
            uint32_t i = (uint32_t)t;
            double frac = t - i;
            for (int k = 0; k < nch; k++) {
                int a = src[i * (uint32_t)nch + (uint32_t)k];
                int b = i + 1 < n ? src[(i + 1) * (uint32_t)nch + (uint32_t)k] : a;
                put_sample(c, out + done * fb + (uint32_t)k * ss, (int16_t)lrint(a + (b - a) * frac));
            }
        }
        c->pos = t - (double)(n - 1);
    }
    for (int k = 0; k < nch; k++)
        c->last[k] = src[(n - 1) * (uint32_t)nch + (uint32_t)k];
    c->have_last = true;
    free(src);
    return done;
}

/* ---- Converter settings ---- */

enum { siUnknownInfoType = -231 };

/* SoundConverterGetInfo(sc, selector, infoPtr): the converter's output
 * format (rate as an UnsignedFixed, size and channels as shorts, the format
 * as an OSType) and its rate converter's roll-off. */
int16_t tb_SoundConverterGetInfo(int32_t sc, int32_t selector, gaddr infoPtr)
{
    converter *k = conv_of((uint32_t)sc);
    if (!k || !infoPtr)
        return paramErr;
    uint32_t sel = (uint32_t)selector;
    if (sel == fourcc("srat"))
        wr32(infoPtr, (uint32_t)(k->out_rate * 65536.0));
    else if (sel == fourcc("ssiz"))
        wr16(infoPtr, (uint16_t)k->out_bits);
    else if (sel == fourcc("chan"))
        wr16(infoPtr, (uint16_t)k->out_channels);
    else if (sel == fourcc("comp"))
        wr32(infoPtr, k->out_format);
    else if (sel == fourcc("rcdb"))
        wr32(infoPtr, (uint32_t)k->roll_off);
    else
        return siUnknownInfoType;
    return noErr;
}

/* SoundConverterSetInfo(sc, selector, infoPtr): the rate converter's
 * roll-off, a long passed as the pointer, 20 (fast) to 90 (best); the
 * output format is fixed when the converter opens. */
int16_t tb_SoundConverterSetInfo(int32_t sc, int32_t selector, gaddr infoPtr)
{
    converter *k = conv_of((uint32_t)sc);
    if (!k)
        return paramErr;
    if ((uint32_t)selector != fourcc("rcdb"))
        return siUnknownInfoType;
    if ((int32_t)infoPtr < 20 || (int32_t)infoPtr > 90)
        return paramErr;
    k->roll_off = (int32_t)infoPtr;
    return noErr;
}

/* ---- PowerPC exports ---- */

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(int32_t)(v); c->pc = c->lr; } while (0)

static void out32(gaddr p, uint32_t v)
{
    if (p)
        wr32(p, v);
}

static void s_SoundConverterOpen(ppc_cpu *c) { RETURN(sc_open(R(0), R(1), R(2))); }

static void s_SoundConverterClose(ppc_cpu *c)
{
    converter *k = conv_of(R(0));
    if (k)
        k->used = false;
    RETURN(k ? noErr : paramErr);
}

/* SoundConverterGetBufferSizes(sc, inputBytesTarget, *inputFrames,
 * *inputBytes, *outputBytes): whole input frames within the target. */
static void s_SoundConverterGetBufferSizes(ppc_cpu *c)
{
    converter *k = conv_of(R(0));
    if (!k) {
        RETURN(paramErr);
        return;
    }
    uint32_t fb = frame_bytes(&k->in), frames = R(1) / fb;
    out32(R(2), frames);
    out32(R(3), frames * fb);
    out32(R(4), out_frames_for(k, frames) * out_frame_bytes(k));
    RETURN(noErr);
}

static void s_SoundConverterBeginConversion(ppc_cpu *c)
{
    converter *k = conv_of(R(0));
    if (k)
        k->pos = 0, k->have_last = false;
    RETURN(k ? noErr : paramErr);
}

/* SoundConverterConvertBuffer(sc, input, inputFrames, output,
 * *outputFrames, *outputBytes) */
static void s_SoundConverterConvertBuffer(ppc_cpu *c)
{
    converter *k = conv_of(R(0));
    if (!k || !R(1) || !R(3)) {
        RETURN(paramErr);
        return;
    }
    uint32_t len = R(2) * frame_bytes(&k->in);
    uint8_t *in = malloc(len ? len : 1);
    uint8_t *out = malloc((size_t)out_frames_for(k, R(2)) * out_frame_bytes(k) + 4);
    gget(in, R(1), len);
    uint32_t n = convert(k, in, len, out);
    gput(R(3), out, n * out_frame_bytes(k));
    free(in);
    free(out);
    out32(R(4), n);
    out32(R(5), n * out_frame_bytes(k));
    RETURN(noErr);
}

/* SoundConverterEndConversion(sc, output, *outputFrames, *outputBytes):
 * nothing is held back. */
static void s_SoundConverterEndConversion(ppc_cpu *c)
{
    converter *k = conv_of(R(0));
    out32(R(2), 0);
    out32(R(3), 0);
    if (k)
        k->have_last = false;
    RETURN(k ? noErr : paramErr);
}

/* ParseSndHeader(sndHandle, *sndInfo, *numFrames, *dataOffset): the first
 * sound's format, its frames (packets, when compressed), and where its
 * samples start in the handle. */
static void s_ParseSndHeader(ppc_cpu *c)
{
    gaddr h = R(0), info = R(1), offp = tb_NewPtr(4, false, true);
    int16_t err = h && rd32(h) && offp ? tb_GetSoundHeaderOffset(h, offp) : paramErr;
    uint32_t off = offp ? rd32(offp) : 0;
    if (offp)
        tb_DisposePtr(offp);
    if (err) {
        RETURN(err);
        return;
    }
    gaddr p = rd32(h) + off, samples = rd32(p);
    uint8_t encode = rd8(p + 20);
    uint32_t format, frames, data;
    int channels = 1, bits = 8;
    if (encode == 0x00) {
        format = fourcc("raw ");
        frames = rd32(p + 4);
        data = off + 22;
    } else if (encode == 0xFF || encode == 0xFE) {
        channels = (int)rd32(p + 4);
        frames = rd32(p + 22);
        data = off + 64;
        if (encode == 0xFF) {
            bits = (int16_t)rd16(p + 48);
            format = fourcc(bits == 16 ? "twos" : "raw ");
        } else {
            int16_t cid = (int16_t)rd16(p + 56);
            format = rd32(p + 40);
            bits = (int16_t)rd16(p + 62);
            if (cid == 3 || cid == 4)
                format = fourcc(cid == 3 ? "MAC3" : "MAC6");
            else if (!format || cid == 0)
                format = fourcc(bits == 16 ? "twos" : "raw ");
        }
    } else {
        RETURN(badFormat);
        return;
    }
    if (info) {
        wr32(info, 0);
        wr32(info + 4, format);
        wr16(info + 8, (uint16_t)channels);
        wr16(info + 10, (uint16_t)bits);
        wr32(info + 12, rd32(p + 8));
        wr32(info + 16, frames);
        wr32(info + 20, samples ? samples : rd32(h) + data);
        wr32(info + 24, 0);
    }
    out32(R(2), frames);
    out32(R(3), data);
    RETURN(noErr);
}

const ppc_export snd_exports[] = {
    { "ParseSndHeader", s_ParseSndHeader },
    { "SoundConverterBeginConversion", s_SoundConverterBeginConversion },
    { "SoundConverterClose", s_SoundConverterClose },
    { "SoundConverterConvertBuffer", s_SoundConverterConvertBuffer },
    { "SoundConverterEndConversion", s_SoundConverterEndConversion },
    { "SoundConverterGetBufferSizes", s_SoundConverterGetBufferSizes },
    { "SoundConverterOpen", s_SoundConverterOpen },
};
const unsigned snd_export_count = sizeof snd_exports / sizeof snd_exports[0];
