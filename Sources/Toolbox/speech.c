/*
 * Speech Manager (MacinTalk).
 *
 * Channels keep their voice, rate, pitch, volume, modes and callbacks here;
 * the host speaks and reports words and the end of each utterance.  Every
 * classic voice speaks with the host's system voice: programs look voices
 * up by name or VoiceSpec and give up if theirs is missing (Mouth wants
 * "Victoria, high quality"), so all of them are here and any VoiceSpec at
 * all opens a channel.  macOS no longer reports phonemes, so a channel's
 * phoneme callbacks are made up from each word as it is spoken, timed by
 * their kinds and by how fast the voice has been going.  Callbacks reach
 * the program at interrupt time (the Time Manager's pump) and at the Event
 * Manager's safe points.  Without a host an utterance ends at the next
 * safe point, so headless runs see the same sequence of calls.
 *
 * Phonemes given as input ([[inpt PHON]], the 'PHON' and 'TUNE' input
 * modes) never reach the host, whose voices read them out as letters:
 * they are turned back into words (phonwords.c) and the host says those.
 * The words it reports are placed back on the phonemes they came from,
 * so word callbacks give the program's own offsets, and the mouth moves
 * to the phonemes as written.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "forks.h"
#include "machine.h"
#include "phonwords.h"
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

/* soPhonemeSymbols: MacinTalk's American English phonemes, each with an
 * example word, its sound bracketed. */
static const struct {
    const char *symbol, *example;
} phoneme_symbols[] = {
    { "%", "silence" }, { "@", "breath intake" }, { "AE", "b[a]t" }, { "EY", "b[ai]t" }, { "AO", "c[au]ght" },
    { "AX", "[a]bout" }, { "IY", "b[ee]t" }, { "EH", "b[e]t" }, { "IH", "b[i]t" }, { "AY", "b[i]te" },
    { "IX", "ros[e]s" }, { "AA", "c[o]t" }, { "UW", "b[oo]t" }, { "UH", "b[oo]k" }, { "UX", "b[u]d" },
    { "OW", "b[oa]t" }, { "AW", "b[ou]t" }, { "OY", "b[oy]" }, { "b", "[b]in" }, { "C", "[ch]in" },
    { "d", "[d]in" }, { "D", "[th]em" }, { "f", "[f]in" }, { "g", "[g]ain" }, { "h", "[h]at" },
    { "J", "[j]ump" }, { "k", "[k]in" }, { "l", "[l]imb" }, { "m", "[m]at" }, { "n", "[n]ap" },
    { "N", "ta[ng]" }, { "p", "[p]in" }, { "r", "[r]an" }, { "s", "[s]in" }, { "S", "[sh]in" },
    { "t", "[t]in" }, { "T", "[th]in" }, { "v", "[v]an" }, { "w", "[w]et" }, { "y", "[y]et" },
    { "z", "[z]oo" }, { "Z", "mea[s]ure" },
};
#define NPHONEMES ((int)(sizeof phoneme_symbols / sizeof phoneme_symbols[0]))

/* Phonemes written as their symbols ([[inpt PHON]] and TUNE input):
 * stress, pitch and length marks and TUNE's {...} are skipped. */
static int symbol_phonemes(const uint8_t *text, int len, uint8_t *out, int max)
{
    int k = 0;
    for (int i = 0; i < len && k < max;) {
        if (text[i] == '{') {
            while (i < len && text[i] != '}')
                i++;
            i++;
            continue;
        }
        int found = -1;
        for (int p = 0; p < NPHONEMES && found < 0; p++) /* the two-letter ones first */
            if (phoneme_symbols[p].symbol[1] && i + 1 < len && text[i] == (uint8_t)phoneme_symbols[p].symbol[0] &&
                text[i + 1] == (uint8_t)phoneme_symbols[p].symbol[1])
                found = p;
        if (found >= 0) {
            out[k++] = (uint8_t)found;
            i += 2;
            continue;
        }
        for (int p = 0; p < NPHONEMES && found < 0; p++)
            if (!phoneme_symbols[p].symbol[1] && text[i] == (uint8_t)phoneme_symbols[p].symbol[0])
                found = p;
        if (found > PH_BREATH) /* silence and breaths between words aren't the word's */
            out[k++] = (uint8_t)found;
        i++;
    }
    return k;
}

static bool vowel(int ch) { return ch == 'a' || ch == 'e' || ch == 'i' || ch == 'o' || ch == 'u'; }

/* A word's phonemes: the dictionary's (phonwords.c), else by rough
 * letter-to-sound rules, enough to move a cartoon mouth (closed for m, b
 * and p, round for o and w ...), not to speak with. */
static int word_phonemes(const uint8_t *text, int len, uint8_t *out, int max)
{
    int known = phonwords_word_phonemes(text, len, out, max);
    if (known)
        return known;
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

int speech_symbol_phonemes(const uint8_t *text, int len, uint8_t *out, int max)
{
    return symbol_phonemes(text, len, out, max);
}

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* The clock phonemes are timed by (a test's own, or the host's). */
static double (*clock_fn)(void) = now_seconds;
void machine_set_speech_clock(double (*fn)(void)) { clock_fn = fn ? fn : now_seconds; }

/* How long a phoneme lasts, relative to the others: vowels longer than
 * consonants, diphthongs longest, stops, flaps and glides short (roughly
 * the averages of American English speech). */
static double phoneme_weight(int op)
{
    switch (op) {
    case PH_EY: case PH_AY: case PH_OW: case PH_AW: case PH_OY:
        return 1.5;
    case PH_b: case PH_d: case PH_g: case PH_k: case PH_p: case PH_t:
        return 0.8;
    case PH_l: case PH_r: case PH_w: case PH_y: case PH_h: case PH_BREATH:
        return 0.75;
    case PH_m: case PH_n: case PH_N:
        return 0.9;
    case PH_f: case PH_v: case PH_s: case PH_z: case PH_S: case PH_Z: case PH_T: case PH_D: case PH_C: case PH_J:
        return 1.05;
    case PH_SILENCE: /* a pause inside a word (an address's at each "dot") */
        return 6;
    default:
        return op >= PH_AE && op <= PH_OY ? 1.2 : 1.0;
    }
}

/* Seconds a unit of phoneme weight takes at a rate (words per minute):
 * the host's voices say about 14 phonemes a second at 175. */
static double unit_seconds(double wpm) { return 12.6 / (wpm > 20 ? wpm : 180); }

/* The host reports an utterance's first word about this late (its audio
 * has begun; later words come as they are heard, and the end with the
 * last sound). */
#define FIRST_WORD_LATE 0.07
/* A word before a pause is drawn out. */
#define FINAL_LENGTHENING 1.35
/* Longer than the voice pauses at punctuation (0.4 s at a comma). */
#define PAUSE_LONGEST 1.0
/* How long the mouth holds when a word outlasts its phonemes by a little. */
#define FILL_GRACE 0.1

/* ---------------------------------------------------------------------- */
/* Channels */

#define MAX_CHANNELS 16
#define MAX_WORD_PHONEMES 1024 /* a URL spelled out has hundreds */

typedef struct {
    gaddr chan;    /* the SpeechChannel the program holds (a 4-byte record) */
    int voice;
    int32_t rate, pitch, modulation, volume; /* Fixed */
    int32_t refcon;
    uint32_t a5;   /* soCurrentA5: A5 for the callbacks, 0 to leave it */
    gaddr done_proc, text_done_proc, sync_proc, word_proc, error_proc, phoneme_proc;
    uint32_t input_mode, char_mode, number_mode;
    bool busy, paused;
    gaddr text;     /* the text being spoken (the program's buffer) */
    uint32_t text_len;
    /* The current word's made-up phonemes, each said when its time comes
     * (none once the host has reported phonemes of its own). */
    uint8_t ph[MAX_WORD_PHONEMES];
    double dur[MAX_WORD_PHONEMES]; /* seconds */
    int nph, iph;
    double ph_next;   /* when the next is due */
    double word_at;   /* when the current word began (0: none) */
    double word_units; /* its length in units of phoneme weight */
    bool word_pause;  /* a pause follows it (punctuation, the end) */
    double close_at;  /* when the mouth closes if no word has come (0: not) */
    double fill_at;   /* when the phonemes go round again if no word has come (0: not) */
    bool filling;     /* they have: the word outlasted them */
    uint32_t word_from, word_end; /* the text the current word covers */
    uint32_t prev_from, prev_end; /* and the word before it (a year's "in") */
    double unit;      /* seconds a unit takes, learnt from the words so far */
    int32_t unit_rate; /* the rate it was learnt at */
    bool first_word;
    bool host_phonemes, mouth_open;
    int16_t phoneme;  /* the last said, for soStatus */
    uint32_t recent_sync; /* soRecentSync */
    uint32_t delimiters;  /* soCommandDelimiter (we speak with the host's [[ ]]) */
    /* How far the text has been read for the words' phonemes: inside an
     * embedded command or not, the command so far, phonemes as input. */
    uint32_t scan_pos;
    bool scan_cmd, scan_skip, scan_phon;
    char cmd[32];
    int cmdlen;
    double spoke_at;  /* when the text was given (for the speech log) */
    /* Phonemes turned into words: for each byte the host was given, the
     * part of the program's text it came from (NULL: the text as it was). */
    uint32_t *said_from, *said_to;
    uint32_t said_len;
} channel;

/* What TextToPhonemes made of each text, the latest first: phonemes that
 * are still what it made are said as that text. */
#define MAX_MADE 8
static struct {
    uint8_t *text, *phonemes;
    uint32_t tlen, plen;
} made[MAX_MADE];

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
    speech_fn(&rq, speech_ctx);
}

/* ---------------------------------------------------------------------- */
/* Events from the host, delivered at interrupt time and at safe points */

enum { EV_DONE, EV_WORD, EV_SYNC, EV_PHONEME };

/* A callback, its arguments pushed: with the channel's A5 if it has one. */
static void call_back(channel *c, gaddr proc)
{
    cpu68k *cpu = g_cpu;
    uint32_t a5 = cpu->a[5];
    if (c->a5)
        cpu->a[5] = c->a5;
    guest_call(proc);
    cpu->a[5] = a5;
}

/* PhonemeCallBack(chan, refCon, phonemeOpcode) */
static void say_phoneme(channel *c, int opcode)
{
    c->phoneme = (int16_t)opcode;
    c->mouth_open = opcode != PH_SILENCE;
    if (!c->phoneme_proc)
        return;
    LOGF(LOG_SPEECH, "speech: %.3f channel %d phoneme %d\n", clock_fn() - c->spoke_at, chan_id(c), opcode);
    cpu_push32(g_cpu, c->chan);
    cpu_push32(g_cpu, (uint32_t)c->refcon);
    cpu_push16(g_cpu, (uint16_t)opcode);
    call_back(c, c->phoneme_proc);
}

/* The made-up phonemes that are due (all of them: the word is over), and
 * the silence that closes the mouth at a pause.  A word whose sound
 * outlasts its phonemes (the voice spells a URL out for 10 s, and the
 * guess can be short) has them go round again at the same pace until the
 * next word or the end, so the mouth never stops while the voice goes on;
 * then what is left when the word ends isn't said in a burst. */
static void phonemes_due(channel *c, double now, bool all)
{
    if (all && (c->filling || c->nph - c->iph > 4))
        c->iph = c->nph;
    for (;;) {
        while (c->iph < c->nph && (all || now >= c->ph_next)) {
            c->ph_next += c->dur[c->iph];
            say_phoneme(c, c->ph[c->iph++]);
        }
        if (all || !c->nph || c->iph < c->nph || !c->fill_at || now < c->fill_at)
            break;
        c->iph = 0;
        c->filling = true;
        if (c->ph_next < now - 0.05) /* kept waiting: from now, not in a burst */
            c->ph_next = now;
    }
    if (c->iph >= c->nph && (all || !c->fill_at))
        c->nph = c->iph = 0;
    if (!all && c->iph >= c->nph && c->close_at && now >= c->close_at) {
        c->close_at = 0;
        if (c->mouth_open)
            say_phoneme(c, PH_SILENCE);
    }
}

static bool phoneme_input(uint32_t mode) { return mode == 0x50484F4Eu || mode == 0x54554E45u; } /* 'PHON', 'TUNE' */

/* An embedded command read: [[inpt PHON]], TUNE and TEXT change the input. */
static void command_done(channel *c)
{
    c->cmd[c->cmdlen] = 0;
    const char *s = c->cmd;
    while (*s == ' ')
        s++;
    if (!strncasecmp(s, "inpt", 4)) {
        s += 4;
        while (*s == ' ')
            s++;
        if (!strncasecmp(s, "PHON", 4) || !strncasecmp(s, "TUNE", 4))
            c->scan_phon = true;
        else if (!strncasecmp(s, "TEXT", 4))
            c->scan_phon = false;
    }
    c->cmdlen = 0;
}

/* Read the text up to pos (words come in order). */
static void scan_to(channel *c, uint32_t pos)
{
    if (pos < c->scan_pos) {
        c->scan_pos = 0;
        c->scan_cmd = c->scan_skip = false;
        c->scan_phon = phoneme_input(c->input_mode);
        c->cmdlen = 0;
    }
    for (; c->scan_pos < pos && c->scan_pos < c->text_len; c->scan_pos++) {
        uint8_t ch = rd8(c->text + c->scan_pos);
        uint8_t next = c->scan_pos + 1 < c->text_len ? rd8(c->text + c->scan_pos + 1) : 0;
        if (c->scan_skip) /* the second bracket */
            c->scan_skip = false;
        else if (!c->scan_cmd) {
            if (ch == '[' && next == '[')
                c->scan_cmd = c->scan_skip = true;
        } else if (ch == ']' && next == ']') {
            command_done(c);
            c->scan_cmd = false;
            c->scan_skip = true;
        } else if (ch == ';') {
            command_done(c);
        } else if (c->cmdlen < (int)sizeof c->cmd - 1) {
            c->cmd[c->cmdlen++] = (char)ch;
        }
    }
}

/* The word's text without embedded commands, and whether it is phonemes. */
static uint32_t word_text(channel *c, uint32_t pos, uint32_t len, uint8_t *w, uint32_t max, bool *phon)
{
    uint32_t n = 0;
    *phon = false;
    for (uint32_t i = pos; i < pos + len && i < c->text_len && n < max; i++) {
        scan_to(c, i);
        uint8_t ch = rd8(c->text + i);
        uint8_t next = i + 1 < c->text_len ? rd8(c->text + i + 1) : 0;
        if (c->scan_cmd || c->scan_skip || (ch == '[' && next == '['))
            continue;
        if (!n)
            *phon = c->scan_phon;
        w[n++] = ch;
    }
    return n;
}

/* Whether a pause follows the word at pos: punctuation, or the end. */
static bool pause_after(channel *c, uint32_t pos, uint32_t len)
{
    for (uint32_t i = pos + len > 0 ? pos + len - 1 : 0; i < c->text_len && i < pos + len + 8; i++) {
        uint8_t ch = rd8(c->text + i);
        uint8_t next = i + 1 < c->text_len ? rd8(c->text + i + 1) : ' ';
        if ((ch == ',' || ch == '.' || ch == ';' || ch == ':' || ch == '!' || ch == '?') &&
            !((next >= '0' && next <= '9') || ((next | 0x20) >= 'a' && (next | 0x20) <= 'z'))) /* not "example.org" */
            return true;
        if (i >= pos + len && (ch == '\r' || ch == '\n' || (ch == '[' && i + 1 < c->text_len && rd8(c->text + i + 1) == '[')))
            return true; /* a line's end, an embedded command (a silence, perhaps) */
        if (i >= pos + len && ch != ' ' && ch != '"' && ch != '\'' && ch != ')' && ch != '\t')
            return false;
    }
    for (uint32_t i = pos + len; i < c->text_len; i++) { /* the end, but for spaces */
        uint8_t ch = rd8(c->text + i);
        if (ch != ' ' && ch != '\r' && ch != '\n' && ch != '\t')
            return false;
    }
    return true;
}

static bool word_char(uint8_t ch) { return ch > ' ' && ch != '[' && ch != ']'; }
static bool alnum_char(uint8_t ch) { return (ch >= '0' && ch <= '9') || ((ch | 0x20) >= 'a' && (ch | 0x20) <= 'z') || ch >= 0x80; }

/* The text a word callback's range stands for.  The host's ranges can be
 * odd: cut short ("tod" for "today." after an address), overlapping the
 * last ("https://github.com", then "github.com/zenwheel/..."), the last
 * again ("42 kB", then "42 kB,"), or leaving out what it said between two
 * ("bob@example", "org": the "dot").  Returns false for the last again. */
static bool word_range(channel *c, uint32_t *from, uint32_t *end)
{
    uint32_t f = *from, e = *end;
    while (e < c->text_len && e > f && alnum_char(rd8(c->text + e - 1)) && alnum_char(rd8(c->text + e)))
        e++;
    if (c->word_end && f < c->word_end && c->word_end < e)
        f = c->word_end;
    else if (c->word_end && f > c->word_end && f - c->word_end < 8) {
        uint32_t g = c->word_end;
        while (g < f && word_char(rd8(c->text + g)))
            g++;
        if (g == f && word_char(rd8(c->text + c->word_end - 1)))
            f = c->word_end;
    }
    bool said = false;
    for (uint32_t i = f; i < e && !said; i++)
        said = alnum_char(rd8(c->text + i));
    if (!said && *from < c->word_end)
        return false;
    *from = f;
    *end = e;
    return true;
}

/* A word is starting.  The last one is over: what is left of its phonemes
 * is said, and the time it took tells how fast the voice speaks, unless a
 * pause followed it or it was far off its phonemes' length (the guess at
 * how it is said was wrong).  This one's phonemes are made from it as the
 * voice says it and timed by that pace, each by its kind; a pause after it
 * closes the mouth when they are done, and so does a long wait for the
 * next word after phonemes given as input. */
static void word_started(channel *c, double now, int32_t pos, int32_t len)
{
    uint32_t from = pos < 0 ? 0 : (uint32_t)pos, end = from + (len > 0 ? (uint32_t)len : 0);
    if (end > c->text_len)
        end = c->text_len;
    bool exact = false;
    if (c->said_from && pos >= 0 && from < c->text_len) {
        /* Phonemes said as words: the range is ours, placed on the phonemes
         * the word came from, and run on into the next word's ("mEYkmIY"). */
        scan_to(c, from);
        exact = c->scan_phon && !c->scan_cmd;
    }
    if (exact && c->word_end && end <= c->word_end)
        return; /* the same word again */
    if (!exact && pos >= 0 && len > 0 && from < c->text_len && !word_range(c, &from, &end))
        return; /* the same word again: it goes on */
    phonemes_due(c, now, true);
    if (c->word_at && !c->word_pause && c->word_units > 0) {
        double seen = (now - c->word_at) / c->word_units;
        if (seen >= c->unit / 2 && seen <= c->unit * 2)
            c->unit += (seen - c->unit) / 2;
    }
    c->word_at = c->fill_at = 0;
    c->filling = false;
    if (!c->phoneme_proc || c->host_phonemes || pos < 0 || len <= 0 || (uint32_t)pos >= c->text_len)
        return;
    c->prev_from = c->word_from;
    c->prev_end = c->word_end;
    c->word_from = from;
    if (end > c->word_end)
        c->word_end = end;
    uint8_t *w = malloc(end - from);
    bool phon;
    uint32_t n = word_text(c, from, end - from, w, end - from, &phon);
    if (phon) {
        c->nph = symbol_phonemes(w, (int)n, c->ph, MAX_WORD_PHONEMES);
    } else {
        uint8_t prev[32];
        uint32_t np = 0;
        for (uint32_t i = c->prev_from; i < c->prev_end && i < from && np < sizeof prev; i++)
            prev[np++] = rd8(c->text + i);
        c->nph = speech_spoken_phonemes(w, (int)n, prev, (int)np, c->ph, MAX_WORD_PHONEMES);
    }
    free(w);
    c->iph = 0;
    if (!c->nph) { /* nothing said ("--", "..."): a pause */
        c->close_at = now;
        return;
    }
    double start = now;
    if (c->first_word) { /* its sound began a little before */
        c->first_word = false;
        start -= FIRST_WORD_LATE;
        if (start < c->spoke_at)
            start = c->spoke_at;
    }
    c->word_pause = pause_after(c, from, end - from);
    c->word_units = 0;
    for (int i = 0; i < c->nph; i++)
        c->word_units += phoneme_weight(c->ph[i]);
    double unit = c->unit * (c->word_pause ? FINAL_LENGTHENING : 1);
    double total = 0;
    for (int i = 0; i < c->nph; i++)
        total += c->dur[i] = phoneme_weight(c->ph[i]) * unit;
    c->word_at = start;
    c->ph_next = start;
    /* Made-up phonemes go round again when the word outlasts them: soon if
     * no pause follows, else after a longer silence than the voice's
     * pauses.  Phonemes given as input are the word. */
    if (phon)
        c->close_at = start + total + (c->word_pause ? 0 : (total / 2 > 0.15 ? total / 2 : 0.15));
    else
        c->close_at = c->word_pause ? start + total : 0;
    c->fill_at = phon ? 0 : start + total + (c->word_pause ? PAUSE_LONGEST : FILL_GRACE);
    phonemes_due(c, now, false);
}

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct {
    int channel, kind;
    int32_t a, b;
    double at; /* when the host reported it */
} speech_event;
static speech_event pending[256];
static int npending;

void machine_speech_event(int chan, int kind, int32_t a, int32_t b)
{
    pthread_mutex_lock(&lock);
    if (npending < 256)
        pending[npending++] = (speech_event){ chan, kind, a, b, clock_fn() };
    pthread_mutex_unlock(&lock);
}

static void finish(channel *c)
{
    if (!c->busy)
        return;
    c->busy = false;
    c->paused = false;
    /* The rest of the made-up phonemes, then the silence that closes the mouth. */
    phonemes_due(c, 0, true);
    if (c->mouth_open && c->phoneme_proc && !c->host_phonemes)
        say_phoneme(c, PH_SILENCE);
    c->mouth_open = false;
    c->word_at = c->close_at = c->fill_at = 0;
    c->nph = c->iph = 0;
    if (c->text_done_proc) { /* TextDone(chan, refCon, &nextBuf, &byteLen, &controlFlags) */
        gaddr out = tb_NewPtr(12, true, true);
        cpu_push32(g_cpu, c->chan);
        cpu_push32(g_cpu, (uint32_t)c->refcon);
        cpu_push32(g_cpu, out);
        cpu_push32(g_cpu, out + 4);
        cpu_push32(g_cpu, out + 8);
        call_back(c, c->text_done_proc);
        tb_DisposePtr(out);
    }
    if (c->done_proc) { /* SpeechDone(chan, refCon) */
        cpu_push32(g_cpu, c->chan);
        cpu_push32(g_cpu, (uint32_t)c->refcon);
        call_back(c, c->done_proc);
    }
}

/* What the host reported, and the phonemes that are due.  On a Mac the
 * callbacks came at interrupt time, as the sound was heard; here they come
 * from the Time Manager's pump (between slices of emulation, and while
 * WaitNextEvent sleeps) as well as at the Event Manager's safe points, so
 * a program drawing a mouth from them keeps up with the voice whatever it
 * is doing.  A callback that calls SpeechBusy doesn't go round again. */
static void deliver(void)
{
    static bool delivering;
    if (delivering)
        return;
    delivering = true;
    pthread_mutex_lock(&lock);
    int n = npending;
    speech_event evs[256];
    memcpy(evs, pending, sizeof evs[0] * (size_t)n);
    npending = 0;
    pthread_mutex_unlock(&lock);
    double now = clock_fn();
    for (int i = 0; i < n; i++) {
        if (evs[i].channel < 0 || evs[i].channel >= MAX_CHANNELS)
            continue;
        channel *c = &channels[evs[i].channel];
        if (!c->chan || evs[i].at < c->spoke_at) /* the utterance a new one replaced */
            continue;
        switch (evs[i].kind) {
        case EV_DONE:
            LOGF(LOG_SPEECH, "speech: %.3f channel %d done (from the host at %.3f)\n", now - c->spoke_at,
                 (int)evs[i].channel, evs[i].at - c->spoke_at);
            finish(c);
            break;
        case EV_WORD: /* WordCallBack(chan, refCon, wordPos, wordLen) */
            if (!c->busy)
                break;
            if (c->said_from && evs[i].b > 0 && evs[i].a >= 0 && (uint32_t)(evs[i].a + evs[i].b) <= c->said_len) {
                /* A word of ours: the phonemes it came from */
                uint32_t from = c->said_from[evs[i].a], to = c->said_to[evs[i].a + evs[i].b - 1];
                evs[i].a = (int32_t)from;
                evs[i].b = (int32_t)(to > from ? to - from : 1);
            }
            LOGF(LOG_SPEECH, "speech: %.3f channel %d word %d+%d (from the host at %.3f)\n", now - c->spoke_at,
                 (int)evs[i].channel, (int)evs[i].a, (int)evs[i].b, evs[i].at - c->spoke_at);
            if (c->word_proc) {
                cpu_push32(g_cpu, c->chan);
                cpu_push32(g_cpu, (uint32_t)c->refcon);
                cpu_push32(g_cpu, (uint32_t)evs[i].a);
                cpu_push16(g_cpu, (uint16_t)evs[i].b);
                call_back(c, c->word_proc);
            }
            /* Timed from when the host said so: the program may have kept
             * us waiting (its interrupts masked). */
            word_started(c, evs[i].at < now ? evs[i].at : now, evs[i].a, evs[i].b);
            break;
        case EV_SYNC: /* SyncCallBack(chan, refCon, syncMessage) */
            c->recent_sync = (uint32_t)evs[i].a;
            if (c->sync_proc && c->busy) {
                cpu_push32(g_cpu, c->chan);
                cpu_push32(g_cpu, (uint32_t)c->refcon);
                cpu_push32(g_cpu, (uint32_t)evs[i].a);
                call_back(c, c->sync_proc);
            }
            break;
        case EV_PHONEME: /* the host's own: no more made-up ones */
            c->host_phonemes = true;
            c->nph = c->iph = 0;
            c->close_at = c->fill_at = 0;
            if (c->busy)
                say_phoneme(c, evs[i].a);
            break;
        }
    }
    now = clock_fn();
    for (int i = 0; i < MAX_CHANNELS; i++)
        if (channels[i].chan && channels[i].busy && !channels[i].paused && (channels[i].nph || channels[i].close_at))
            phonemes_due(&channels[i], now, false);
    delivering = false;
}

/* Whether a channel is speaking to the host: WaitNextEvent's sleep is cut
 * into slices for its callbacks (and a headless run's wait takes real time). */
bool speech_waiting(void)
{
    if (!speech_fn || speech_recording(speech_fn)) /* the record backend has said everything already */
        return false;
    for (int i = 0; i < MAX_CHANNELS; i++)
        if (channels[i].chan && channels[i].busy && !channels[i].paused)
            return true;
    return false;
}

/* The host's synthesizers report on the main thread, which runs the
 * machine in a headless run: it is served while a channel speaks. */
static void serve_host(void)
{
    static double last;
    double t = now_seconds();
    if (t - last >= 0.002 && speech_waiting()) {
        last = t;
        machine_serve_main_thread();
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
    serve_host();
    deliver();
}

/* Called by the Time Manager's pump, at interrupt time. */
void speech_interrupt(void)
{
    if (!speech_fn)
        return;
    serve_host();
    deliver();
}

static void forget_said(channel *c)
{
    free(c->said_from);
    free(c->said_to);
    c->said_from = c->said_to = NULL;
    c->said_len = 0;
}

void speech_reset(void)
{
    nextra = 0; /* voices registered and defaults set belong to a run */
    speech_defaults.set = false;
    for (int i = 0; i < MAX_CHANNELS; i++)
        forget_said(&channels[i]);
    memset(channels, 0, sizeof channels);
    for (int i = 0; i < MAX_MADE; i++) {
        free(made[i].text);
        free(made[i].phonemes);
    }
    memset(made, 0, sizeof made);
    default_channel = NULL;
    pthread_mutex_lock(&lock);
    npending = 0;
    pthread_mutex_unlock(&lock);
}

/* ---------------------------------------------------------------------- */
/* Speaking */

/* ---------------------------------------------------------------------- */
/* Phonemes as input, said as words */

static void remember_made(const uint8_t *text, uint32_t tlen, const uint8_t *ph, uint32_t plen)
{
    free(made[MAX_MADE - 1].text);
    free(made[MAX_MADE - 1].phonemes);
    memmove(&made[1], &made[0], sizeof made[0] * (MAX_MADE - 1));
    made[0].text = malloc(tlen ? tlen : 1);
    made[0].phonemes = malloc(plen ? plen : 1);
    if (made[0].text && made[0].phonemes) {
        memcpy(made[0].text, text, tlen);
        memcpy(made[0].phonemes, ph, plen);
    }
    made[0].tlen = made[0].text && made[0].phonemes ? tlen : 0;
    made[0].plen = made[0].text && made[0].phonemes ? plen : 0;
}

/* What the host is given, and where each byte of it came from. */
typedef struct {
    uint8_t *s;
    uint32_t *from, *to;
    uint32_t k, size;
} said;

static bool said_room(said *o, uint32_t more)
{
    if (o->k + more <= o->size)
        return true;
    uint32_t size = (o->k + more) * 2 + 64;
    uint8_t *s = realloc(o->s, size);
    uint32_t *from = realloc(o->from, sizeof *from * size);
    uint32_t *to = realloc(o->to, sizeof *to * size);
    if (s)
        o->s = s;
    if (from)
        o->from = from;
    if (to)
        o->to = to;
    if (!s || !from || !to)
        return false;
    o->size = size;
    return true;
}

static void said_put(said *o, const void *bytes, uint32_t n, uint32_t from, uint32_t to)
{
    if (!said_room(o, n))
        return;
    for (uint32_t i = 0; i < n; i++) {
        o->s[o->k] = ((const uint8_t *)bytes)[i];
        o->from[o->k] = from;
        o->to[o->k++] = to;
    }
}

static bool letter_or_digit(uint8_t ch) { return ((ch | 0x20) >= 'a' && (ch | 0x20) <= 'z') || (ch >= '0' && ch <= '9'); }

/* A run of phonemes (text[at..at+n]) as words. */
static void say_phonemes(said *o, const uint8_t *text, uint32_t at, uint32_t n)
{
    const uint8_t *ph = text + at;
    while (n && (ph[0] == ' ' || ph[0] == '\r')) { /* not part of what TextToPhonemes made */
        ph++, at++, n--;
    }
    while (n && (ph[n - 1] == ' ' || ph[n - 1] == '\r'))
        n--;
    if (!n)
        return;
    if (o->k && letter_or_digit(o->s[o->k - 1]))
        said_put(o, " ", 1, at, at + 1);
    for (int m = 0; m < MAX_MADE; m++) {
        if (!made[m].plen || made[m].plen != n || memcmp(made[m].phonemes, ph, n))
            continue;
        /* The text it came from, its words on the phonemes made for them:
         * TextToPhonemes keeps what is between the words. */
        const uint8_t *t = made[m].text;
        uint32_t tl = made[m].tlen, ti = 0, pi = 0;
        while (ti < tl) {
            uint32_t ts = ti, ps = pi;
            while (ti < tl && t[ti] != ' ')
                ti++;
            while (pi < n && ph[pi] != ' ')
                pi++;
            said_put(o, t + ts, ti - ts, at + ps, at + (pi > ps ? pi : n));
            for (; ti < tl && t[ti] == ' '; ti++)
                said_put(o, " ", 1, at + pi, at + pi + 1);
            while (pi < n && ph[pi] == ' ')
                pi++;
        }
        return;
    }
    size_t room = (size_t)n * 6 + 64;
    char *out = malloc(room);
    uint32_t *from = malloc(sizeof *from * room), *to = malloc(sizeof *to * room);
    if (out && from && to) {
        size_t k = phonemes_to_text(ph, n, out, room, from, to);
        for (size_t i = 0; i < k; i++)
            said_put(o, out + i, 1, at + from[i], at + to[i]);
    }
    free(out);
    free(from);
    free(to);
}

/* The text with its phonemes turned into words and the [[inpt]] commands
 * that switched to them left out, if it has any; false if it hasn't. */
static bool phonemes_said(channel *c, const uint8_t *text, uint32_t len, said *o)
{
    bool phon = phoneme_input(c->input_mode), any = phon;
    for (uint32_t i = 0; i < len;) {
        if (i + 1 < len && text[i] == '[' && text[i + 1] == '[') {
            uint32_t end = i + 2;
            while (end < len && !(text[end] == ']' && end + 1 < len && text[end + 1] == ']'))
                end++;
            uint32_t close = end + 2 <= len ? end + 2 : len;
            /* The commands, without inpt: [[inpt PHON; rate 200]] */
            char kept[256];
            size_t nk = 0;
            for (uint32_t p = i + 2; p < end;) {
                uint32_t q = p;
                while (q < end && text[q] != ';')
                    q++;
                uint32_t a = p;
                while (a < q && text[a] == ' ')
                    a++;
                if (q - a >= 4 && !strncasecmp((const char *)text + a, "inpt", 4)) {
                    uint32_t v = a + 4;
                    while (v < q && text[v] == ' ')
                        v++;
                    if (q - v >= 4 && (!strncasecmp((const char *)text + v, "PHON", 4) ||
                                       !strncasecmp((const char *)text + v, "TUNE", 4)))
                        phon = any = true;
                    else if (q - v >= 4 && !strncasecmp((const char *)text + v, "TEXT", 4))
                        phon = false;
                } else if (q > a && nk + (q - a) + 2 < sizeof kept) {
                    if (nk)
                        kept[nk++] = ';';
                    memcpy(kept + nk, text + a, q - a);
                    nk += q - a;
                }
                p = q + 1;
            }
            if (nk) {
                said_put(o, "[[", 2, i, close);
                said_put(o, kept, (uint32_t)nk, i, close);
                said_put(o, "]]", 2, i, close);
            }
            i = close;
            continue;
        }
        uint32_t end = i;
        while (end < len && !(end + 1 < len && text[end] == '[' && text[end + 1] == '['))
            end++;
        if (phon) {
            say_phonemes(o, text, i, end - i);
        } else {
            if (o->k && letter_or_digit(o->s[o->k - 1]) && letter_or_digit(text[i]))
                said_put(o, " ", 1, i, i + 1);
            for (uint32_t j = i; j < end; j++)
                said_put(o, text + j, 1, j, j + 1);
        }
        i = end;
    }
    return any;
}

static int16_t speak(channel *c, gaddr text, uint32_t len)
{
    if (c->busy) /* a new utterance replaces the old one */
        host_request(c, MACHINE_SPEECH_STOP, NULL);
    c->busy = len > 0;
    c->paused = false;
    c->nph = c->iph = 0;
    c->word_at = c->close_at = c->fill_at = 0;
    c->word_from = c->word_end = c->prev_from = c->prev_end = 0;
    c->first_word = true;
    c->scan_pos = 1; /* read again from the start */
    scan_to(c, 0);
    if (!c->unit || c->unit_rate != c->rate) { /* what was learnt holds while the rate does */
        c->unit = unit_seconds(c->rate / 65536.0);
        c->unit_rate = c->rate;
    }
    c->text = text;
    c->text_len = len;
    c->spoke_at = clock_fn();
    forget_said(c);
    if (!len)
        return noErr;
    uint8_t *mac = malloc(len);
    gget(mac, text, len);
    said o = { 0 };
    bool words = phonemes_said(c, mac, len, &o);
    if (words) {
        c->said_from = o.from;
        c->said_to = o.to;
        c->said_len = o.k;
        free(mac);
        mac = o.s;
        len = o.k;
    } else {
        free(o.s);
        free(o.from);
        free(o.to);
    }
    char *utf8 = malloc((size_t)len * 3 + 1);
    macroman_to_utf8(mac, len, utf8, (size_t)len * 3 + 1);
    free(mac);
    if (words)
        LOGF(LOG_SPEECH, "speech: channel %d says the phonemes as \"%s\"\n", chan_id(c), utf8);
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
    forget_said(c);
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
        c->word_at = c->close_at = c->fill_at = 0;
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

/* PhonemeDescriptor: a count, then PhonemeInfo each: opcode, phStr
 * (Str15), exampleStr (Str31), hiliteStart and hiliteEnd (the sound's
 * characters in the example). */
static gaddr phoneme_descriptor(void)
{
    enum { INFO = 2 + 16 + 32 + 2 + 2 };
    gaddr h = tb_NewHandle(2 + NPHONEMES * INFO, false, true);
    if (!h)
        return 0;
    gaddr p = rd32(h);
    wr16(p, NPHONEMES);
    for (int i = 0; i < NPHONEMES; i++) {
        gaddr e = p + 2 + (uint32_t)i * INFO;
        wr16(e, (uint16_t)i);
        size_t n = strlen(phoneme_symbols[i].symbol);
        wr8(e + 2, (uint8_t)n);
        gput(e + 3, phoneme_symbols[i].symbol, (uint32_t)n);
        int k = 0, start = 0, end = 0;
        for (const char *x = phoneme_symbols[i].example; *x; x++)
            if (*x == '[')
                start = k;
            else if (*x == ']')
                end = k;
            else
                wr8(e + 18 + 1 + (uint32_t)k++, (uint8_t)*x);
        wr8(e + 18, (uint8_t)k);
        wr16(e + 50, (uint16_t)start);
        wr16(e + 52, (uint16_t)end);
    }
    return h;
}

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
        wr16(info + 6, c->busy ? (uint16_t)c->phoneme : 0); /* the phoneme being said */
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
    case 0x6D794135u: wr32(info, c->a5); return noErr;                   /* 'myA5' */
    case 0x73796E63u: wr32(info, c->recent_sync); return noErr;          /* 'sync': the last sync message */
    case 0x646C696Du: wr32(info, c->delimiters); return noErr;           /* 'dlim' */
    case 0x70687379u: { /* 'phsy': a new handle to the PhonemeDescriptor, the program's */
        gaddr h = phoneme_descriptor();
        if (!h)
            return memFullErr;
        wr32(info, h);
        return noErr;
    }
    case 0x63766F78u: /* 'cvox': the current voice */
        wr32(info, VOICE_CREATOR);
        wr32(info + 4, (uint32_t)(c->voice + 1));
        return noErr;
    default:
        LOGF(LOG_UNIMPL, "GetSpeechInfo: selector %08X unknown\n", (uint32_t)selector);
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
    case 0x6D794135u: c->a5 = info; return noErr;             /* 'myA5': A5 for the callbacks */
    case 0x646C696Du: c->delimiters = rd32(info); return noErr; /* 'dlim': DelimiterInfo */
    case 0x736E646Fu: return noErr;                           /* 'sndo': the sound output (the host's) */
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
        LOGF(LOG_UNIMPL, "SetSpeechInfo: selector %08X unknown\n", (uint32_t)selector);
        return siUnknownInfoType;
    }
}

/* The text's words as phoneme symbols by the letter-to-sound rules, the
 * spaces and punctuation between them kept. */
static size_t text_symbols(gaddr text, uint32_t len, uint8_t *out, size_t max)
{
    size_t n = 0;
    for (uint32_t i = 0; i < len && n < max;) {
        uint8_t ch = rd8(text + i);
        if (((ch | 0x20) < 'a' || (ch | 0x20) > 'z')) {
            out[n++] = ch;
            i++;
            continue;
        }
        uint8_t w[64], ph[64];
        int k = 0;
        for (; i < len && k < (int)sizeof w; i++) {
            uint8_t x = rd8(text + i);
            if (((x | 0x20) < 'a' || (x | 0x20) > 'z') && x != '\'')
                break;
            w[k++] = x;
        }
        int np = word_phonemes(w, k, ph, (int)sizeof ph);
        for (int j = 0; j < np; j++)
            for (const char *sym = phoneme_symbols[ph[j]].symbol; *sym && n < max; sym++)
                out[n++] = (uint8_t)*sym;
    }
    return n;
}

/* Phonemes: the host's, when it can; otherwise our own. */
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
    if (!n) /* the host has none (macOS's say -50 now): ours, from the dictionary and the letters */
        n = text_symbols(textBuf, textBytes, mac, sizeof mac);
    if (textBytes <= 4096) { /* said as this text if they come back as input */
        uint8_t *t = malloc(textBytes ? textBytes : 1);
        gget(t, textBuf, textBytes);
        remember_made(t, textBytes, mac, (uint32_t)n);
        free(t);
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
