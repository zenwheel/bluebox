/*
 * Speech Manager (MacinTalk).
 *
 * Channels keep their voice, rate, pitch, volume, modes and callbacks here;
 * the host speaks and reports words and the end of each utterance.  Every
 * classic voice speaks with the host's system voice: programs look voices
 * up by name or VoiceSpec and give up if theirs is missing (Mouth wants
 * "Victoria, high quality"), so all of them are here and any VoiceSpec at
 * all opens a channel.  macOS no longer reports phonemes, so a channel's
 * phoneme callbacks are made up from each word as it is spoken.  Callbacks
 * reach the program at the Event Manager's safe points.  Without a host an
 * utterance ends at the next safe point, so headless runs see the same
 * sequence of calls.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "forks.h"
#include "machine.h"
#include "tb_internal.h"
#include "trap.h"

enum { voiceNotFound = -244, siUnknownInfoType = -231 };

#define FIXED(x) ((int32_t)((x) * 65536))

/* The classic voices (MacinTalk 3 and Pro), in GetIndVoice order: creator
 * 'gala' and IDs of our own.  All of them speak with the host's voice; the
 * name only tells the program what it found. */
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
    /* MacinTalk Pro's, after the others so that their IDs stay put. */
    { "Agnes, high quality", 2, 35 }, { "Bruce, high quality", 1, 30 }, { "Victoria, high quality", 2, 35 },
};
#define NVOICES ((int)(sizeof voices / sizeof voices[0]))
#define VOICE_CREATOR 0x67616C61u /* 'gala' */
#define DEFAULT_VOICE 10            /* Fred */

/* Any VoiceSpec is a voice: one of ours, or else the default, since they
 * all sound the same and a program that names a voice we don't list (a
 * third-party synthesizer's, or one it remembers from another Mac) would
 * otherwise not speak at all. */
/* The voices a program registered (RegisterVoices), after ours. */
enum { MAX_EXTRA = 16 };
static struct { uint32_t creator; int32_t id; } extra[MAX_EXTRA];
static int nextra;
/* SetSpeechDefaults': the voice and settings a new channel starts with. */
static struct { bool set; uint32_t creator; int32_t id; int32_t rate, pitch, volume; } speech_defaults;

static int voice_index(gaddr spec)
{
    if (!spec)
        return DEFAULT_VOICE;
    uint32_t creator = rd32(spec);
    int32_t id = (int32_t)rd32(spec + 4);
    for (int k = 0; k < nextra; k++)
        if (extra[k].creator == creator && extra[k].id == id)
            return NVOICES + k;
    if (creator != VOICE_CREATOR || id < 1 || id > NVOICES)
        return DEFAULT_VOICE;
    return id - 1;
}

/* ---------------------------------------------------------------------- */
/* Phonemes made up from the spoken words */

/* The MacinTalk phoneme opcodes (American English). */
enum {
    PH_SILENCE, PH_BREATH, PH_AE, PH_EY, PH_AO, PH_AX, PH_IY, PH_EH, PH_IH, PH_AY, PH_IX, PH_AA, PH_UW, PH_UH,
    PH_UX, PH_OW, PH_AW, PH_OY, PH_b, PH_C, PH_d, PH_D, PH_f, PH_g, PH_h, PH_J, PH_k, PH_l, PH_m, PH_n, PH_N,
    PH_p, PH_r, PH_s, PH_S, PH_t, PH_T, PH_v, PH_w, PH_y, PH_z, PH_Z,
};

static bool vowel(int ch) { return ch == 'a' || ch == 'e' || ch == 'i' || ch == 'o' || ch == 'u'; }

/* A word's phonemes by rough letter-to-sound rules: enough to move a
 * cartoon mouth (closed for m, b and p, round for o and w ...), not to
 * speak with. */
static int word_phonemes(const uint8_t *text, int len, uint8_t *out, int max)
{
    static const struct {
        const char *letters;
        uint8_t ph[2];
    } groups[] = {
        { "tch", { PH_C } }, { "igh", { PH_AY } }, { "sh", { PH_S } },  { "ch", { PH_C } },  { "th", { PH_T } },
        { "ph", { PH_f } },  { "ng", { PH_N } },   { "ck", { PH_k } },  { "wh", { PH_w } },  { "qu", { PH_k, PH_w } },
        { "ee", { PH_IY } }, { "ea", { PH_IY } },  { "ie", { PH_IY } }, { "oo", { PH_UW } }, { "ou", { PH_AW } },
        { "ow", { PH_AW } }, { "oi", { PH_OY } },  { "oy", { PH_OY } }, { "ai", { PH_EY } }, { "ay", { PH_EY } },
        { "ei", { PH_EY } }, { "ey", { PH_EY } },  { "au", { PH_AO } }, { "aw", { PH_AO } }, { "oa", { PH_OW } },
    };
    static const uint8_t single[26] = {
        PH_AE, PH_b, PH_k, PH_d, PH_EH, PH_f, PH_g, PH_h, PH_IH, PH_J, PH_k, PH_l, PH_m,
        PH_n, PH_AA, PH_p, PH_k, PH_r, PH_s, PH_t, PH_AX, PH_v, PH_w, PH_k, PH_IY, PH_z,
    };
    char w[64];
    int n = 0;
    for (int i = 0; i < len && n < (int)sizeof w; i++)
        if ((text[i] | 0x20) >= 'a' && (text[i] | 0x20) <= 'z')
            w[n++] = (char)(text[i] | 0x20);
    int vowels = 0;
    for (int i = 0; i < n; i++)
        vowels += vowel(w[i]);
    int k = 0;
    for (int i = 0; i < n && k < max;) {
        size_t g = 0;
        for (; g < sizeof groups / sizeof groups[0]; g++) {
            size_t gl = strlen(groups[g].letters);
            if (i + (int)gl <= n && !memcmp(w + i, groups[g].letters, gl))
                break;
        }
        if (g < sizeof groups / sizeof groups[0]) {
            for (int j = 0; j < 2 && groups[g].ph[j] && k < max; j++)
                out[k++] = groups[g].ph[j];
            i += (int)strlen(groups[g].letters);
            continue;
        }
        int ch = w[i];
        if (i > 0 && ch == w[i - 1] && !vowel(ch)) { /* a doubled consonant is one sound */
            i++;
            continue;
        }
        if (ch == 'e' && i == n - 1 && i > 0) { /* final e: silent, unless it is the only vowel (he, the) */
            if (vowels == 1)
                out[k++] = PH_IY;
            i++;
            continue;
        }
        if (ch == 'c' && i + 1 < n && (w[i + 1] == 'e' || w[i + 1] == 'i' || w[i + 1] == 'y'))
            out[k++] = PH_s;
        else if (ch == 'y' && i == 0)
            out[k++] = PH_y;
        else if (ch == 'x') {
            out[k++] = PH_k;
            if (k < max)
                out[k++] = PH_s;
        } else
            out[k++] = single[ch - 'a'];
        i++;
    }
    return k;
}

int speech_word_phonemes(const uint8_t *text, int len, uint8_t *out, int max)
{
    return word_phonemes(text, len, out, max);
}

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
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
    /* The current word's made-up phonemes, said one by one as time passes;
     * none once the host has reported phonemes of its own. */
    uint8_t ph[64];
    int nph, iph;
    double ph_next, ph_step;
    bool host_phonemes, said_phonemes;
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
    c->rate = speech_defaults.set ? speech_defaults.rate : FIXED(180);
    c->pitch = speech_defaults.set ? speech_defaults.pitch : FIXED(46);
    c->modulation = FIXED(35);
    c->volume = speech_defaults.set ? speech_defaults.volume : FIXED(1);
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
    rq.phonemes = c->input_mode == 0x50484F4Eu; /* 'PHON' */
    speech_fn(&rq, speech_ctx);
}

/* ---------------------------------------------------------------------- */
/* Events from the host, delivered at safe points */

enum { EV_DONE, EV_WORD, EV_SYNC, EV_PHONEME };

/* PhonemeCallBack(chan, refCon, phonemeOpcode) */
static void say_phoneme(channel *c, int opcode)
{
    cpu_push32(g_cpu, c->chan);
    cpu_push32(g_cpu, (uint32_t)c->refcon);
    cpu_push16(g_cpu, (uint16_t)opcode);
    guest_call(c->phoneme_proc);
}

/* The made-up phonemes that are due, or all of them. */
static void phonemes_due(channel *c, bool all)
{
    double now = all ? 0 : now_seconds();
    while (c->iph < c->nph && c->phoneme_proc && (all || now >= c->ph_next)) {
        say_phoneme(c, c->ph[c->iph++]);
        c->ph_next += c->ph_step;
        c->said_phonemes = true;
    }
    if (c->iph >= c->nph || !c->phoneme_proc)
        c->nph = c->iph = 0;
}

/* A word is starting: the last one's phonemes are over, and this one's
 * are spread over most of the time a word takes at the channel's rate. */
static void word_started(channel *c, int32_t pos, int32_t len)
{
    phonemes_due(c, true);
    if (!c->phoneme_proc || c->host_phonemes || pos < 0 || len <= 0 || (uint32_t)pos >= c->text_len)
        return;
    uint8_t w[64];
    uint32_t n = (uint32_t)len < sizeof w ? (uint32_t)len : sizeof w;
    if (n > c->text_len - (uint32_t)pos)
        n = c->text_len - (uint32_t)pos;
    gget(w, c->text + (uint32_t)pos, n);
    c->nph = word_phonemes(w, (int)n, c->ph, (int)sizeof c->ph);
    c->iph = 0;
    double wpm = c->rate > 0 ? c->rate / 65536.0 : 180;
    c->ph_step = c->nph ? 60.0 / wpm * 0.8 / c->nph : 0;
    c->ph_next = now_seconds();
}

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
    /* The rest of the made-up phonemes, then the silence that closes the mouth. */
    phonemes_due(c, true);
    if (c->said_phonemes && c->phoneme_proc && !c->host_phonemes)
        say_phoneme(c, PH_SILENCE);
    c->said_phonemes = false;
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
            if (!c->busy)
                break;
            if (c->word_proc) {
                cpu_push32(g_cpu, c->chan);
                cpu_push32(g_cpu, (uint32_t)c->refcon);
                cpu_push32(g_cpu, (uint32_t)evs[i].a);
                cpu_push16(g_cpu, (uint16_t)evs[i].b);
                guest_call(c->word_proc);
            }
            word_started(c, evs[i].a, evs[i].b);
            break;
        case EV_SYNC: /* SyncCallBack(chan, refCon, syncMessage) */
            if (c->sync_proc && c->busy) {
                cpu_push32(g_cpu, c->chan);
                cpu_push32(g_cpu, (uint32_t)c->refcon);
                cpu_push32(g_cpu, (uint32_t)evs[i].a);
                guest_call(c->sync_proc);
            }
            break;
        case EV_PHONEME: /* the host's own: no more made-up ones */
            c->host_phonemes = true;
            c->nph = c->iph = 0;
            if (c->phoneme_proc && c->busy)
                say_phoneme(c, evs[i].a);
            break;
        }
    }
    for (int i = 0; i < MAX_CHANNELS; i++)
        if (channels[i].chan && channels[i].busy && !channels[i].paused && channels[i].nph)
            phonemes_due(&channels[i], false);
}

void speech_reset(void)
{
    nextra = 0; /* voices registered and defaults set belong to a run */
    speech_defaults.set = false;
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
    c->nph = c->iph = 0;
    c->said_phonemes = false;
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
        wr16(numVoices, (uint16_t)(NVOICES + nextra));
    return noErr;
}

int16_t tb_GetIndVoice(int16_t index, gaddr voice)
{
    if (index > NVOICES && index <= NVOICES + nextra) {
        wr32(voice, extra[index - NVOICES - 1].creator);
        wr32(voice + 4, (uint32_t)extra[index - NVOICES - 1].id);
        return noErr;
    }
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
    char registered[40];
    const char *name;
    int gender = 0, age = 0;
    if (v >= NVOICES) {
        memcpy(d + 4, &(uint32_t[]){ __builtin_bswap32(extra[v - NVOICES].creator) }, 4);
        uint32_t id = (uint32_t)extra[v - NVOICES].id;
        d[8] = (uint8_t)(id >> 24), d[9] = (uint8_t)(id >> 16), d[10] = (uint8_t)(id >> 8), d[11] = (uint8_t)id;
        snprintf(registered, sizeof registered, "Voice %d", (int)id);
        name = registered;
    } else {
        name = voices[v].name;
        gender = voices[v].gender;
        age = voices[v].age;
    }
    size_t nl = strlen(name);
    d[16] = (uint8_t)nl;
    memcpy(d + 17, name, nl);
    const char *comment = "A MacinTalk voice.";
    d[80] = (uint8_t)strlen(comment);
    memcpy(d + 81, comment, strlen(comment));
    d[336] = 0, d[337] = (uint8_t)gender;
    d[338] = 0, d[339] = (uint8_t)age;
    /* script, language, region: Roman, English, US (0, 0, 0) */
    gput(info, d, (uint32_t)n);
}

int16_t tb_GetVoiceDescription(gaddr voice, gaddr info, int32_t infoLength)
{
    int v = voice_index(voice);
    if (info && infoLength > 0)
        describe(v, info, infoLength);
    return noErr;
}

int16_t tb_GetVoiceInfo(gaddr voice, int32_t selector, gaddr voiceInfo)
{
    int v = voice_index(voice);
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

/* KillProcessChannels and ShutDownSpeechManager: every channel the program
 * opened (and SpeakString's) is stopped and disposed of. */
void speech_kill_channels(void)
{
    for (int i = 0; i < MAX_CHANNELS; i++)
        if (channels[i].chan)
            tb_DisposeSpeechChannel(channels[i].chan);
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
        c->nph = c->iph = 0;
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
    case 0x63766F78u: /* 'cvox' */
        c->voice = voice_index(info);
        return noErr;
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

/* ---- For SpeechLib's own calls (os9left_misc.c) ---- */

/* RegisterVoices / UnRegisterVoices: a voice by its creator and ID. */
int16_t speech_register_voice(uint32_t creator, int32_t id)
{
    for (int k = 0; k < nextra; k++)
        if (extra[k].creator == creator && extra[k].id == id)
            return noErr;
    if (creator == VOICE_CREATOR && id >= 1 && id <= NVOICES)
        return noErr;
    if (nextra == MAX_EXTRA)
        return memFullErr;
    extra[nextra].creator = creator;
    extra[nextra++].id = id;
    return noErr;
}

int16_t speech_unregister_voice(uint32_t creator, int32_t id)
{
    for (int k = 0; k < nextra; k++)
        if (extra[k].creator == creator && extra[k].id == id) {
            for (int j = k; j + 1 < nextra; j++)
                extra[j] = extra[j + 1];
            nextra--;
            return noErr;
        }
    return voiceNotFound;
}

/* The defaults: voice (creator, ID), rate, pitch, volume. */
void speech_get_defaults(uint32_t *creator, int32_t *id, int32_t *rate, int32_t *pitch, int32_t *volume)
{
    *creator = speech_defaults.set ? speech_defaults.creator : VOICE_CREATOR;
    *id = speech_defaults.set ? speech_defaults.id : DEFAULT_VOICE + 1;
    *rate = speech_defaults.set ? speech_defaults.rate : FIXED(180);
    *pitch = speech_defaults.set ? speech_defaults.pitch : FIXED(46);
    *volume = speech_defaults.set ? speech_defaults.volume : FIXED(1);
}

void speech_set_defaults(uint32_t creator, int32_t id, int32_t rate, int32_t pitch, int32_t volume)
{
    speech_defaults.set = true;
    speech_defaults.creator = creator;
    speech_defaults.id = id;
    speech_defaults.rate = rate;
    speech_defaults.pitch = pitch;
    speech_defaults.volume = volume;
}

int speech_channel_voice(gaddr chan)
{
    channel *c = chan_of(chan);
    return c ? c->voice : -1;
}
