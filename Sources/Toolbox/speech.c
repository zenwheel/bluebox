/*
 * Speech Manager (MacinTalk).
 *
 * Channels keep their voice, rate, pitch, volume, modes and callbacks here;
 * the host speaks (macOS: NSSpeechSynthesizer, which understands the
 * classic embedded commands and still has the classic voices) and reports
 * words and the end of each utterance.  Callbacks reach the program at the
 * Event Manager's safe points.  Without a host an utterance ends at the
 * next safe point, so headless runs see the same sequence of calls.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "forks.h"
#include "machine.h"
#include "tb_internal.h"
#include "trap.h"

enum { voiceNotFound = -244, siUnknownInfoType = -231 };

#define FIXED(x) ((int32_t)((x) * 65536))

/* The classic voices (MacinTalk 3 and Pro), in GetIndVoice order: creator
 * 'gala', IDs of our own, and the host voice that stands for each. */
static const struct {
    const char *name;
    int16_t gender; /* 0 neuter, 1 male, 2 female */
    int16_t age;
} voices[] = {
    { "Agnes", 2, 35 },    { "Albert", 1, 30 },     { "Bad News", 1, 30 }, { "Bahh", 1, 2 },
    { "Bells", 0, 0 },     { "Boing", 0, 0 },       { "Bruce", 1, 30 },    { "Bubbles", 0, 0 },
    { "Cellos", 0, 0 },    { "Deranged", 1, 30 },   { "Fred", 1, 30 },     { "Good News", 1, 30 },
    { "Hysterical", 1, 30 }, { "Junior", 1, 8 },    { "Kathy", 2, 30 },    { "Pipe Organ", 0, 0 },
    { "Princess", 2, 8 },  { "Ralph", 1, 50 },      { "Trinoids", 0, 0 },  { "Victoria", 2, 35 },
    { "Whisper", 0, 0 },   { "Zarvox", 0, 0 },
};
#define NVOICES ((int)(sizeof voices / sizeof voices[0]))
#define VOICE_CREATOR 0x67616C61u /* 'gala' */
#define DEFAULT_VOICE 10            /* Fred */

static int voice_index(gaddr spec)
{
    if (!spec)
        return DEFAULT_VOICE;
    uint32_t creator = rd32(spec);
    int32_t id = (int32_t)rd32(spec + 4);
    if (creator != VOICE_CREATOR || id < 1 || id > NVOICES)
        return -1;
    return id - 1;
}

/* ---------------------------------------------------------------------- */
/* Channels */

#define MAX_CHANNELS 16

typedef struct {
    gaddr chan;    /* the SpeechChannel the program holds (a 4-byte record) */
    int voice;
    int32_t rate, pitch, modulation, volume; /* Fixed */
    int32_t refcon;
    gaddr done_proc, text_done_proc, sync_proc, word_proc, error_proc, phoneme_proc;
    uint32_t input_mode, char_mode, number_mode;
    bool busy, paused;
    gaddr text;     /* the text being spoken (the program's buffer) */
    uint32_t text_len;
} channel;

static channel channels[MAX_CHANNELS];
static channel *default_channel; /* SpeakString's */

static machine_speech_fn speech_fn;
static void *speech_ctx;

void machine_set_speech_handler(machine_speech_fn fn, void *ctx)
{
    speech_fn = fn;
    speech_ctx = ctx;
}

static channel *chan_of(gaddr chan)
{
    for (int i = 0; i < MAX_CHANNELS; i++)
        if (channels[i].chan && channels[i].chan == chan)
            return &channels[i];
    return NULL;
}

static int chan_id(const channel *c) { return (int)(c - channels); }

static void defaults(channel *c, int voice)
{
    c->voice = voice;
    c->rate = FIXED(180);
    c->pitch = FIXED(46);
    c->modulation = FIXED(35);
    c->volume = FIXED(1);
    c->input_mode = 0x54455854u; /* 'TEXT' */
    c->char_mode = 0x4E4F524Du;  /* 'NORM' */
    c->number_mode = 0x4E4F524Du;
}

static channel *new_channel(int voice)
{
    for (int i = 0; i < MAX_CHANNELS; i++)
        if (!channels[i].chan) {
            channel *c = &channels[i];
            memset(c, 0, sizeof *c);
            c->chan = tb_NewPtr(4, true, true);
            if (!c->chan)
                return NULL;
            defaults(c, voice);
            return c;
        }
    return NULL;
}

static void host_request(channel *c, int op, const char *text)
{
    if (!speech_fn)
        return;
    machine_speech_request rq;
    memset(&rq, 0, sizeof rq);
    rq.channel = chan_id(c);
    rq.op = op;
    rq.voice = voices[c->voice].name;
    rq.rate = c->rate / 65536.0;
    rq.pitch = c->pitch / 65536.0;
    rq.modulation = c->modulation / 65536.0;
    rq.volume = c->volume / 65536.0;
    rq.text = text;
    rq.phonemes = c->input_mode == 0x5048304Eu; /* 'PHON' */
    speech_fn(&rq, speech_ctx);
}

/* ---------------------------------------------------------------------- */
/* Events from the host, delivered at safe points */

enum { EV_DONE, EV_WORD, EV_SYNC, EV_PHONEME };

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct {
    int channel, kind;
    int32_t a, b;
} speech_event;
static speech_event pending[256];
static int npending;

void machine_speech_event(int chan, int kind, int32_t a, int32_t b)
{
    pthread_mutex_lock(&lock);
    if (npending < 256)
        pending[npending++] = (speech_event){ chan, kind, a, b };
    pthread_mutex_unlock(&lock);
}

static void finish(channel *c)
{
    if (!c->busy)
        return;
    c->busy = false;
    c->paused = false;
    if (c->text_done_proc) { /* TextDone(chan, refCon, &nextBuf, &byteLen, &controlFlags) */
        gaddr out = tb_NewPtr(12, true, true);
        cpu_push32(g_cpu, c->chan);
        cpu_push32(g_cpu, (uint32_t)c->refcon);
        cpu_push32(g_cpu, out);
        cpu_push32(g_cpu, out + 4);
        cpu_push32(g_cpu, out + 8);
        guest_call(c->text_done_proc);
        tb_DisposePtr(out);
    }
    if (c->done_proc) { /* SpeechDone(chan, refCon) */
        cpu_push32(g_cpu, c->chan);
        cpu_push32(g_cpu, (uint32_t)c->refcon);
        guest_call(c->done_proc);
    }
}

/* Called by the Event Manager at safe points. */
void speech_pump(void)
{
    /* Without a host an utterance is over by the next safe point. */
    if (!speech_fn)
        for (int i = 0; i < MAX_CHANNELS; i++)
            if (channels[i].chan && channels[i].busy && !channels[i].paused)
                finish(&channels[i]);
    pthread_mutex_lock(&lock);
    int n = npending;
    speech_event evs[256];
    memcpy(evs, pending, sizeof evs[0] * (size_t)n);
    npending = 0;
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < n; i++) {
        if (evs[i].channel < 0 || evs[i].channel >= MAX_CHANNELS)
            continue;
        channel *c = &channels[evs[i].channel];
        if (!c->chan)
            continue;
        switch (evs[i].kind) {
        case EV_DONE:
            finish(c);
            break;
        case EV_WORD: /* WordCallBack(chan, refCon, wordPos, wordLen) */
            if (c->word_proc && c->busy) {
                cpu_push32(g_cpu, c->chan);
                cpu_push32(g_cpu, (uint32_t)c->refcon);
                cpu_push32(g_cpu, (uint32_t)evs[i].a);
                cpu_push16(g_cpu, (uint16_t)evs[i].b);
                guest_call(c->word_proc);
            }
            break;
        case EV_SYNC: /* SyncCallBack(chan, refCon, syncMessage) */
            if (c->sync_proc && c->busy) {
                cpu_push32(g_cpu, c->chan);
                cpu_push32(g_cpu, (uint32_t)c->refcon);
                cpu_push32(g_cpu, (uint32_t)evs[i].a);
                guest_call(c->sync_proc);
            }
            break;
        case EV_PHONEME: /* PhonemeCallBack(chan, refCon, phonemeOpcode) */
            if (c->phoneme_proc && c->busy) {
                cpu_push32(g_cpu, c->chan);
                cpu_push32(g_cpu, (uint32_t)c->refcon);
                cpu_push16(g_cpu, (uint16_t)evs[i].a);
                guest_call(c->phoneme_proc);
            }
            break;
        }
    }
}

void speech_reset(void)
{
    memset(channels, 0, sizeof channels);
    default_channel = NULL;
    pthread_mutex_lock(&lock);
    npending = 0;
    pthread_mutex_unlock(&lock);
}

/* ---------------------------------------------------------------------- */
/* Speaking */

static int16_t speak(channel *c, gaddr text, uint32_t len)
{
    if (c->busy) /* a new utterance replaces the old one */
        host_request(c, MACHINE_SPEECH_STOP, NULL);
    c->busy = len > 0;
    c->paused = false;
    c->text = text;
    c->text_len = len;
    if (!len)
        return noErr;
    char *utf8 = malloc((size_t)len * 3 + 1);
    uint8_t *mac = malloc(len);
    gget(mac, text, len);
    macroman_to_utf8(mac, len, utf8, (size_t)len * 3 + 1);
    free(mac);
    host_request(c, MACHINE_SPEECH_SPEAK, utf8);
    free(utf8);
    return noErr;
}

int32_t tb_SpeechManagerVersion(void) { return 0x01508000; } /* 1.5 */

int16_t tb_SpeechBusy(void)
{
    speech_pump(); /* programs wait for it in a loop of their own */
    int n = 0;
    for (int i = 0; i < MAX_CHANNELS; i++)
        if (channels[i].chan && channels[i].busy)
            n++;
    return (int16_t)n;
}

int16_t tb_SpeechBusySystemWide(void) { return tb_SpeechBusy(); }

int16_t tb_CountVoices(gaddr numVoices)
{
    if (numVoices)
        wr16(numVoices, NVOICES);
    return noErr;
}

int16_t tb_GetIndVoice(int16_t index, gaddr voice)
{
    if (index < 1 || index > NVOICES)
        return voiceNotFound;
    wr32(voice, VOICE_CREATOR);
    wr32(voice + 4, (uint32_t)index);
    return noErr;
}

int16_t tb_MakeVoiceSpec(int32_t creator, int32_t id, gaddr voice)
{
    if (!voice)
        return paramErr;
    wr32(voice, (uint32_t)creator);
    wr32(voice + 4, (uint32_t)id);
    return noErr;
}

/* VoiceDescription: length, voice, version, name (Str63), comment (Str255),
 * gender, age, script, language, region, reserved. */
static void describe(int v, gaddr info, int32_t len)
{
    uint8_t d[362];
    memset(d, 0, sizeof d);
    int32_t n = len < (int32_t)sizeof d ? len : (int32_t)sizeof d;
    d[0] = (uint8_t)(n >> 24), d[1] = (uint8_t)(n >> 16), d[2] = (uint8_t)(n >> 8), d[3] = (uint8_t)n;
    d[4] = 'g', d[5] = 'a', d[6] = 'l', d[7] = 'a';
    d[11] = (uint8_t)(v + 1);
    d[12] = 0, d[13] = 0, d[14] = 0x01, d[15] = 0x00; /* version 1 */
    size_t nl = strlen(voices[v].name);
    d[16] = (uint8_t)nl;
    memcpy(d + 17, voices[v].name, nl);
    const char *comment = "A MacinTalk voice.";
    d[80] = (uint8_t)strlen(comment);
    memcpy(d + 81, comment, strlen(comment));
    d[336] = 0, d[337] = (uint8_t)voices[v].gender;
    d[338] = 0, d[339] = (uint8_t)voices[v].age;
    /* script, language, region: Roman, English, US (0, 0, 0) */
    gput(info, d, (uint32_t)n);
}

int16_t tb_GetVoiceDescription(gaddr voice, gaddr info, int32_t infoLength)
{
    int v = voice_index(voice);
    if (v < 0)
        return voiceNotFound;
    if (info && infoLength > 0)
        describe(v, info, infoLength);
    return noErr;
}

int16_t tb_GetVoiceInfo(gaddr voice, int32_t selector, gaddr voiceInfo)
{
    int v = voice_index(voice);
    if (v < 0)
        return voiceNotFound;
    if (!voiceInfo)
        return paramErr;
    if ((uint32_t)selector == 0x696E666Fu) { /* 'info': VoiceDescription */
        describe(v, voiceInfo, (int32_t)rd32(voiceInfo) > 0 ? (int32_t)rd32(voiceInfo) : 362);
        return noErr;
    }
    if ((uint32_t)selector == 0x6C616E67u) { /* 'lang': VoiceLanguageInfo, one language: English */
        wr16(voiceInfo, 1);
        wr16(voiceInfo + 2, 0);
        return noErr;
    }
    return siUnknownInfoType;
}

int16_t tb_NewSpeechChannel(gaddr voice, gaddr chan)
{
    int v = voice_index(voice);
    if (v < 0)
        return voiceNotFound;
    channel *c = new_channel(v);
    if (!c)
        return memFullErr;
    wr32(chan, c->chan);
    return noErr;
}

int16_t tb_DisposeSpeechChannel(gaddr chan)
{
    channel *c = chan_of(chan);
    if (!c)
        return paramErr;
    if (c->busy)
        host_request(c, MACHINE_SPEECH_STOP, NULL);
    host_request(c, MACHINE_SPEECH_DISPOSE, NULL);
    tb_DisposePtr(c->chan);
    if (c == default_channel)
        default_channel = NULL;
    memset(c, 0, sizeof *c);
    return noErr;
}

int16_t tb_SpeakString(gaddr textToBeSpoken)
{
    if (!default_channel && !(default_channel = new_channel(DEFAULT_VOICE)))
        return memFullErr;
    if (!textToBeSpoken)
        return speak(default_channel, 0, 0);
    /* The string is copied: the program may reuse it at once. */
    uint32_t n = rd8(textToBeSpoken);
    static gaddr copy;
    if (copy)
        tb_DisposePtr(copy);
    copy = tb_NewPtr(256, true, true);
    gmemcpy(copy, textToBeSpoken + 1, n);
    return speak(default_channel, copy, n);
}

int16_t tb_SpeakText(gaddr chan, gaddr textBuf, uint32_t textBytes)
{
    channel *c = chan_of(chan);
    if (!c)
        return paramErr;
    return speak(c, textBuf, textBytes);
}

int16_t tb_SpeakBuffer(gaddr chan, gaddr textBuf, uint32_t textBytes, int32_t controlFlags)
{
    return tb_SpeakText(chan, textBuf, textBytes);
}

int16_t tb_StopSpeech(gaddr chan)
{
    channel *c = chan_of(chan);
    if (!c)
        return paramErr;
    if (c->busy) {
        host_request(c, MACHINE_SPEECH_STOP, NULL);
        c->busy = c->paused = false;
    }
    return noErr;
}

/* whereToStop: kImmediate 0, kEndOfWord 1, kEndOfSentence 2 (we stop at once). */
int16_t tb_StopSpeechAt(gaddr chan, int32_t whereToStop) { return tb_StopSpeech(chan); }

int16_t tb_PauseSpeechAt(gaddr chan, int32_t whereToPause)
{
    channel *c = chan_of(chan);
    if (!c)
        return paramErr;
    if (c->busy && !c->paused) {
        c->paused = true;
        host_request(c, MACHINE_SPEECH_PAUSE, NULL);
    }
    return noErr;
}

int16_t tb_ContinueSpeech(gaddr chan)
{
    channel *c = chan_of(chan);
    if (!c)
        return paramErr;
    if (c->paused) {
        c->paused = false;
        host_request(c, MACHINE_SPEECH_CONTINUE, NULL);
    }
    return noErr;
}

int16_t tb_SetSpeechRate(gaddr chan, int32_t rate)
{
    channel *c = chan_of(chan);
    if (!c)
        return paramErr;
    c->rate = rate;
    return noErr;
}

int16_t tb_GetSpeechRate(gaddr chan, gaddr rate)
{
    channel *c = chan_of(chan);
    if (!c)
        return paramErr;
    wr32(rate, (uint32_t)c->rate);
    return noErr;
}

int16_t tb_SetSpeechPitch(gaddr chan, int32_t pitch)
{
    channel *c = chan_of(chan);
    if (!c)
        return paramErr;
    c->pitch = pitch;
    return noErr;
}

int16_t tb_GetSpeechPitch(gaddr chan, gaddr pitch)
{
    channel *c = chan_of(chan);
    if (!c)
        return paramErr;
    wr32(pitch, (uint32_t)c->pitch);
    return noErr;
}

int16_t tb_UseDictionary(gaddr chan, gaddr dictionary) { return chan_of(chan) ? noErr : paramErr; }

/* The standard selectors (soStatus, soRate, soPitchBase ...). */
int16_t tb_GetSpeechInfo(gaddr chan, int32_t selector, gaddr info)
{
    channel *c = chan_of(chan);
    if (!c)
        return paramErr;
    switch ((uint32_t)selector) {
    case 0x73746174u: /* 'stat': SpeechStatusInfo */
        wr8(info, c->busy);
        wr8(info + 1, c->paused);
        wr32(info + 2, c->busy ? c->text_len : 0);
        wr16(info + 6, 0);
        return noErr;
    case 0x72617465u: wr32(info, (uint32_t)c->rate); return noErr;       /* 'rate' */
    case 0x70626173u: wr32(info, (uint32_t)c->pitch); return noErr;      /* 'pbas' */
    case 0x706D6F64u: wr32(info, (uint32_t)c->modulation); return noErr; /* 'pmod' */
    case 0x766F6C6Du: wr32(info, (uint32_t)c->volume); return noErr;     /* 'volm' */
    case 0x72656663u: wr32(info, (uint32_t)c->refcon); return noErr;     /* 'refc' */
    case 0x696E7074u: wr32(info, c->input_mode); return noErr;           /* 'inpt' */
    case 0x63686172u: wr32(info, c->char_mode); return noErr;            /* 'char' */
    case 0x6E6D6272u: wr32(info, c->number_mode); return noErr;          /* 'nmbr' */
    case 0x76657273u: wr32(info, 0x01508000); return noErr;              /* 'vers' */
    case 0x6572726Eu: /* 'erro': SpeechErrorInfo, no errors */
        gmemset(info, 0, 12);
        return noErr;
    case 0x63766F78u: /* 'cvox': the current voice */
        wr32(info, VOICE_CREATOR);
        wr32(info + 4, (uint32_t)(c->voice + 1));
        return noErr;
    default:
        return siUnknownInfoType;
    }
}

int16_t tb_SetSpeechInfo(gaddr chan, int32_t selector, gaddr info)
{
    channel *c = chan_of(chan);
    if (!c)
        return paramErr;
    switch ((uint32_t)selector) {
    case 0x72617465u: c->rate = (int32_t)rd32(info); return noErr;
    case 0x70626173u: c->pitch = (int32_t)rd32(info); return noErr;
    case 0x706D6F64u: c->modulation = (int32_t)rd32(info); return noErr;
    case 0x766F6C6Du: c->volume = (int32_t)rd32(info); return noErr;
    /* 'refc' and the callbacks take the value itself, not a pointer to it. */
    case 0x72656663u: c->refcon = (int32_t)info; return noErr;
    case 0x646E6362u: c->done_proc = info; return noErr;      /* 'dncb' */
    case 0x74646362u: c->text_done_proc = info; return noErr; /* 'tdcb' */
    case 0x73796362u: c->sync_proc = info; return noErr;      /* 'sycb' */
    case 0x77646362u: c->word_proc = info; return noErr;      /* 'wdcb' */
    case 0x65726362u: c->error_proc = info; return noErr;     /* 'ercb' */
    case 0x70686362u: c->phoneme_proc = info; return noErr;   /* 'phcb' */
    case 0x696E7074u: c->input_mode = rd32(info); return noErr;
    case 0x63686172u: c->char_mode = rd32(info); return noErr;
    case 0x6E6D6272u: c->number_mode = rd32(info); return noErr;
    case 0x72736574u: /* 'rset' */
        defaults(c, c->voice);
        return noErr;
    case 0x6376786Fu: { /* 'cvox' */
        int v = voice_index(info);
        if (v < 0)
            return voiceNotFound;
        c->voice = v;
        return noErr;
    }
    default:
        return siUnknownInfoType;
    }
}

/* Phonemes: the host's, when it can; otherwise the text itself. */
int16_t tb_TextToPhonemes(gaddr chan, gaddr textBuf, uint32_t textBytes, gaddr phonemeBuf, gaddr phonemeBytes)
{
    channel *c = chan_of(chan);
    if (!c || !phonemeBuf)
        return paramErr;
    char out[4096] = "";
    if (speech_fn && textBytes < 1024) {
        char *utf8 = malloc((size_t)textBytes * 3 + 1);
        uint8_t mac[1024];
        gget(mac, textBuf, textBytes);
        macroman_to_utf8(mac, textBytes, utf8, (size_t)textBytes * 3 + 1);
        machine_speech_request rq;
        memset(&rq, 0, sizeof rq);
        rq.channel = chan_id(c);
        rq.op = MACHINE_SPEECH_PHONEMES;
        rq.voice = voices[c->voice].name;
        rq.text = utf8;
        rq.result = out;
        rq.result_size = sizeof out;
        speech_fn(&rq, speech_ctx);
        free(utf8);
    }
    uint8_t mac[4096];
    size_t n = out[0] ? utf8_to_macroman(out, mac, sizeof mac) : 0;
    if (!n) {
        n = textBytes < sizeof mac ? textBytes : sizeof mac;
        gget(mac, textBuf, (uint32_t)n);
    }
    tb_SetHandleSize(phonemeBuf, (int32_t)n);
    if (rd32(phonemeBuf))
        gput(rd32(phonemeBuf), mac, (uint32_t)n);
    if (phonemeBytes)
        wr32(phonemeBytes, (uint32_t)n);
    return noErr;
}
