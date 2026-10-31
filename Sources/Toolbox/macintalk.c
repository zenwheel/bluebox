/*
 * MacinTalk 1: the old speech driver, before the Speech Manager.
 *
 * Programs of 1985-1993 speak with the MacinTalk glue linked into them
 * (SpeechOn, SpeechOff, SpeechRate, SpeechPitch, SpeechSex, Reader,
 * MacinTalk), which talks to a driver file, "MacinTalk", that the user
 * put in the System Folder.  SpeechOn makes a 100-byte SpeechRecord that
 * is also the driver's parameter block, gets DRVR ".SPEECH" from the
 * open resource files (programs open the file themselves, with
 * OpenResFile("MacinTalk"), which the poor man's search path finds in the
 * System Folder), gives the resource the ID of a free unit (19 to 31),
 * and _Opens it.  The driver's Open gets the name of an exceptions file
 * at $3C (none, or "noReader" for no Reader at all) and leaves the handle
 * of the Reader's code ('TALK' 1) at $34, which the glue keeps at $42 and
 * jumps into for Reader(theSpeech, englishInput, inputLength,
 * phoneticOutput), the speech handle taken off the stack.  MacinTalk()
 * is an immediate _Write of the phonemes handle (ioBuffer the handle,
 * ioReqCount its size); SpeechSex, SpeechRate, SpeechPitch and the pitch
 * mode are Controls 0 (0 male, 1 female), 2 (words a minute, up to 425),
 * 4 (Hz, 65 to 500) and 3 (natural or robotic); Close takes the driver's
 * entry out of the unit table, so the next SpeechOn finds the unit free.
 *
 * Ours is that file, written into the System Folder (rewritten when this
 * build's stubs differ; a file of someone else's is left alone), with a
 * driver and a Reader of our own that speak through the Speech Manager.
 * The Reader spells the English in MacinTalk's phonetic code (SAM's: IY
 * IH EH AE ... with stress digits) by rough letter-to-sound rules, and
 * remembers what it was given: phonemes that are still what it made are
 * spoken as the English, other phonemes (a program's own, written by
 * hand) in the host's phoneme mode.  MacinTalk was synchronous: the
 * _Write returns when the utterance is over.
 */
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "forks.h"
#include "tb_internal.h"
#include "trap.h"

enum { writErr = -20 };
enum { mt_rate = 150, mt_pitch = 110 }; /* MacinTalk's defaults: words a minute, Hz */

/* The speech of each open unit (one for each SpeechOn). */
#define MAX_SPEECH 16
typedef struct {
    int16_t refnum; /* 0: free */
    int16_t exceptions; /* the exceptions file the Open opened, or 0 */
    gaddr chan;     /* our Speech Manager channel, made at the first _Write */
    gaddr text;     /* what it is saying */
    int rate, pitch, sex, robotic;
} mt_speech;
static mt_speech speech[MAX_SPEECH];

static uint32_t open_stub, prime_stub, control_stub, status_stub, close_stub, reader_stub;

static mt_speech *speech_of(gaddr dce, bool make)
{
    int16_t rn = (int16_t)rd16(dce + DCtlEntry_dCtlRefNum_off);
    mt_speech *free_one = NULL;
    for (int i = 0; i < MAX_SPEECH; i++) {
        if (speech[i].refnum == rn)
            return &speech[i];
        if (!speech[i].refnum && !free_one)
            free_one = &speech[i];
    }
    if (!make || !free_one)
        return NULL;
    memset(free_one, 0, sizeof *free_one);
    free_one->refnum = rn;
    free_one->rate = mt_rate;
    free_one->pitch = mt_pitch;
    return free_one;
}

/* ---------------------------------------------------------------------- */
/* The phonetic code */

/* The Speech Manager's phoneme opcodes (speech.c's letter-to-sound rules
 * give these) in MacinTalk's code; a vowel is marked with a 'v'. */
static const char *const mt_of_opcode[42] = {
    "",    "",    "AEv", "EYv", "AOv", "AXv", "IYv", "EHv", "IHv", "AYv", "IXv", "AAv", "UWv", "UHv",
    "AHv", "OWv", "AWv", "OYv", "B",   "CH",  "D",   "DH",  "F",   "G",   "/H",  "J",   "K",   "L",
    "M",   "N",   "NX",  "P",   "R",   "S",   "SH",  "T",   "TH",  "V",   "W",   "Y",   "Z",   "ZH",
};

/* MacinTalk's phonemes in the host's (the Speech Manager's phoneme mode);
 * longest first, so that "SH" is not S and H.  Vowels take the stress
 * that follows them in MacinTalk and comes before them there. */
static const struct {
    const char *mt, *host;
    bool vowel;
} phoneme_map[] = {
    { "IY", "IY", 1 }, { "IH", "IH", 1 }, { "EH", "EH", 1 }, { "AE", "AE", 1 }, { "AA", "AA", 1 },
    { "AH", "UX", 1 }, { "AO", "AO", 1 }, { "UH", "UH", 1 }, { "AX", "AX", 1 }, { "IX", "IX", 1 },
    { "ER", "UXr", 1 }, { "UX", "UW", 1 }, { "OH", "OW", 1 }, { "EY", "EY", 1 }, { "AY", "AY", 1 },
    { "OY", "OY", 1 }, { "AW", "AW", 1 }, { "OW", "OW", 1 }, { "UW", "UW", 1 }, { "UL", "AXl", 1 },
    { "UM", "AXm", 1 }, { "UN", "AXn", 1 }, { "RX", "r", 0 }, { "LX", "l", 0 }, { "WX", "w", 0 },
    { "YX", "y", 0 }, { "WH", "w", 0 }, { "NX", "N", 0 }, { "DX", "d", 0 }, { "QX", "%", 0 },
    { "SH", "S", 0 }, { "TH", "T", 0 }, { "ZH", "Z", 0 }, { "DH", "D", 0 }, { "CH", "C", 0 },
    { "/H", "h", 0 }, { "/X", "h", 0 }, { "H", "h", 0 }, { "R", "r", 0 }, { "L", "l", 0 }, { "W", "w", 0 },
    { "Y", "y", 0 }, { "M", "m", 0 }, { "N", "n", 0 }, { "Q", "", 0 }, { "S", "s", 0 },
    { "F", "f", 0 }, { "Z", "z", 0 }, { "V", "v", 0 }, { "J", "J", 0 }, { "B", "b", 0 },
    { "D", "d", 0 }, { "G", "g", 0 }, { "P", "p", 0 }, { "T", "t", 0 }, { "K", "k", 0 },
};

/* English to MacinTalk phonemes, word by word, the first vowel of each
 * word stressed (4, MacinTalk's ordinary stress). */
static size_t english_to_mt(const uint8_t *in, size_t n, char *out, size_t outsize)
{
    size_t k = 0;
#define PUT(s) do { for (const char *p_ = (s); *p_ && k + 1 < outsize; p_++) out[k++] = *p_; } while (0)
    for (size_t i = 0; i < n;) {
        int ch = in[i];
        bool letter = ((ch | 0x20) >= 'a' && (ch | 0x20) <= 'z') || ch == '\'';
        if (!letter) {
            if (ch == '.' || ch == ',' || ch == '?' || ch == '!')
                PUT(((char[]){ (char)ch, 0 }));
            else if (k && out[k - 1] != ' ' && (ch == ' ' || ch == '\r' || ch == '\n' || ch == '\t' || ch == '-'))
                PUT(" ");
            i++;
            continue;
        }
        size_t start = i;
        while (i < n && ((((in[i] | 0x20) >= 'a' && (in[i] | 0x20) <= 'z')) || in[i] == '\''))
            i++;
        uint8_t ph[64];
        int np = speech_word_phonemes(in + start, (int)(i - start), ph, (int)sizeof ph);
        bool stressed = false;
        for (int j = 0; j < np; j++) {
            const char *m = ph[j] < 42 ? mt_of_opcode[ph[j]] : "";
            size_t ml = strlen(m);
            bool vowel = ml && m[ml - 1] == 'v';
            for (size_t c = 0; c < ml - (vowel ? 1 : 0) && k + 1 < outsize; c++)
                out[k++] = m[c];
            if (vowel && !stressed) {
                PUT("4");
                stressed = true;
            }
        }
    }
#undef PUT
    while (k && out[k - 1] == ' ')
        k--;
    out[k] = 0;
    return k;
}

/* MacinTalk phonemes in the host's phoneme code. */
static void mt_to_host(const uint8_t *in, size_t n, char *out, size_t outsize)
{
    size_t k = 0;
    size_t vowel_at = (size_t)-1; /* where the last vowel starts in out */
    for (size_t i = 0; i < n && k + 4 < outsize;) {
        int ch = in[i] >= 'a' && in[i] <= 'z' ? in[i] - 32 : in[i];
        if (ch >= '1' && ch <= '9') { /* stress, after its vowel: the host's goes before it */
            if (vowel_at != (size_t)-1) {
                memmove(out + vowel_at + 1, out + vowel_at, k - vowel_at);
                out[vowel_at] = ch <= '5' ? '1' : '2';
                k++;
                vowel_at = (size_t)-1;
            }
            i++;
            continue;
        }
        if (ch == '.' || ch == ',' || ch == '?' || ch == '!' || ch == ' ' || ch == '-') {
            out[k++] = ch == '-' ? ' ' : (char)ch;
            vowel_at = (size_t)-1;
            i++;
            continue;
        }
        size_t m = 0;
        for (; m < sizeof phoneme_map / sizeof phoneme_map[0]; m++) {
            size_t l = strlen(phoneme_map[m].mt);
            bool same = i + l <= n;
            for (size_t j = 0; j < l && same; j++) {
                int c = in[i + j] >= 'a' && in[i + j] <= 'z' ? in[i + j] - 32 : in[i + j];
                same = c == phoneme_map[m].mt[j];
            }
            if (same)
                break;
        }
        if (m == sizeof phoneme_map / sizeof phoneme_map[0]) { /* not a phoneme: left out */
            i++;
            continue;
        }
        if (phoneme_map[m].vowel)
            vowel_at = k;
        for (const char *p = phoneme_map[m].host; *p && k + 4 < outsize; p++)
            out[k++] = *p;
        i += strlen(phoneme_map[m].mt);
    }
    out[k] = 0;
}

/* What the Reader was given and what it made of it, the latest first. */
#define MAX_READ 8
static struct {
    uint8_t *english, *phonemes;
    size_t elen, plen;
} reads[MAX_READ];

static void remember(const uint8_t *english, size_t elen, const char *phonemes, size_t plen)
{
    free(reads[MAX_READ - 1].english);
    free(reads[MAX_READ - 1].phonemes);
    memmove(&reads[1], &reads[0], sizeof reads[0] * (MAX_READ - 1));
    reads[0].english = malloc(elen ? elen : 1);
    reads[0].phonemes = malloc(plen ? plen : 1);
    memcpy(reads[0].english, english, elen);
    memcpy(reads[0].phonemes, phonemes, plen);
    reads[0].elen = elen;
    reads[0].plen = plen;
}

/* ---------------------------------------------------------------------- */
/* The Reader ('TALK' 1): Reader(englishInput, inputLength, phoneticOutput)
 * once the glue has taken the speech handle off; Pascal, an OSErr. */

static void reader_native(cpu68k *c, uint16_t tw)
{
    (void)tw;
    uint32_t sp = c->a[7];
    gaddr ret = rd32(sp), out = rd32(sp + 4), text = rd32(sp + 12);
    uint32_t len = rd32(sp + 8);
    int16_t err = noErr;
    if (!out) {
        err = nilHandleErr;
    } else {
        if (len > 32767)
            len = 32767;
        uint8_t *english = malloc(len ? len : 1);
        gget(english, text, len);
        size_t room = (size_t)len * 6 + 16;
        char *ph = malloc(room);
        size_t n = english_to_mt(english, len, ph, room);
        tb_SetHandleSize(out, (int32_t)n);
        err = (int16_t)rd16(LM_MemErr);
        if (err == noErr && rd32(out)) {
            gput(rd32(out), ph, (uint32_t)n);
            remember(english, len, ph, n);
        }
        free(ph);
        free(english);
    }
    wr16(sp + 16, (uint16_t)err);
    c->a[7] = sp + 12;
    wr32(c->a[7], ret);
}

/* ---------------------------------------------------------------------- */
/* The driver: A0 the parameter block (the SpeechRecord), A1 the entry */

static void open_native(cpu68k *c, uint16_t tw)
{
    (void)tw;
    gaddr pb = c->a[0];
    mt_speech *s = speech_of(c->a[1], true);
    int16_t err = noErr;
    gaddr reader = 0;
    gaddr name = rd32(pb + 0x3C);
    uint8_t n = name ? rd8(name) : 0;
    char cname[256];
    for (int i = 0; i < n; i++)
        cname[i] = (char)rd8(name + 1 + (uint32_t)i);
    cname[n] = 0;
    if (!s) {
        err = -23; /* openErr: too many voices */
    } else if (!strcasecmp(cname, "noReader")) {
        reader = 0;
    } else {
        /* An exceptions file: opened as MacinTalk did (its rules are
         * not ours to use, but a missing one is an error). */
        if (n) {
            int16_t rn = tb_OpenResFile(name);
            err = (int16_t)rd16(LM_ResErr);
            if (rn != -1)
                s->exceptions = rn;
        }
        if (err == noErr) {
            reader = tb_GetResource((int32_t)fourcc("TALK"), 1);
            if (!reader)
                err = resNotFound;
        }
    }
    wr32(pb + 0x34, reader);
    if (err != noErr && s) {
        if (s->exceptions)
            tb_CloseResFile(s->exceptions);
        s->refnum = 0;
    }
    wr16(pb + IOParam_ioResult_off, (uint16_t)err);
    c->d[0] = (uint32_t)(int32_t)err;
}

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* The channel's voice and settings, from MacinTalk's. */
static void apply(mt_speech *s)
{
    gaddr spec = tb_NewPtr(8, true, true);
    tb_MakeVoiceSpec((int32_t)fourcc("gala"), s->sex ? 20 : 11, spec); /* Victoria or Fred */
    tb_SetSpeechInfo(s->chan, (int32_t)fourcc("cvox"), spec);
    tb_SetSpeechRate(s->chan, (int32_t)s->rate << 16);
    /* Hz to the Speech Manager's pitch, a MIDI note number. */
    double note = 69 + 12 * log2(s->pitch / 440.0);
    tb_SetSpeechPitch(s->chan, (int32_t)lround(note * 65536));
    wr32(spec, (uint32_t)(s->robotic ? 0 : 35 << 16));
    tb_SetSpeechInfo(s->chan, (int32_t)fourcc("pmod"), spec);
    tb_DisposePtr(spec);
}

/* Prime: MacinTalk(theSpeech, phonemes), a _Write. */
static void prime_native(cpu68k *c, uint16_t tw)
{
    (void)tw;
    gaddr pb = c->a[0];
    mt_speech *s = speech_of(c->a[1], false);
    int16_t err = noErr;
    if ((rd16(pb + IOParam_ioTrap_off) & 0xFF) != 3 || !s) {
        c->d[0] = (uint32_t)(int32_t)writErr;
        return;
    }
    gaddr h = rd32(pb + IOParam_ioBuffer_off);
    uint32_t count = rd32(pb + IOParam_ioReqCount_off);
    gaddr p = h ? rd32(h) : 0;
    int32_t size = h ? tb_GetHandleSize(h) : 0;
    if (size < 0)
        size = 0;
    if (count > (uint32_t)size)
        count = (uint32_t)size;
    if (!p)
        count = 0;
    if (!s->chan) {
        gaddr out = tb_NewPtr(4, true, true);
        if (tb_NewSpeechChannel(0, out) == noErr)
            s->chan = rd32(out);
        tb_DisposePtr(out);
    }
    if (s->chan && count) {
        uint8_t *ph = malloc(count);
        gget(ph, p, count);
        /* The Reader's own phonemes say its English; others are phonemes. */
        const uint8_t *say = NULL;
        size_t say_len = 0;
        for (int i = 0; i < MAX_READ && !say; i++)
            if (reads[i].phonemes && reads[i].plen == count && !memcmp(reads[i].phonemes, ph, count)) {
                say = reads[i].english;
                say_len = reads[i].elen;
            }
        char *host = NULL;
        if (!say) {
            size_t room = (size_t)count * 4 + 40;
            host = malloc(room);
            strcpy(host, "[[inpt PHON]]");
            mt_to_host(ph, count, host + strlen(host), room - strlen(host) - 16);
            strcat(host, "[[inpt TEXT]]");
            say = (const uint8_t *)host;
            say_len = strlen(host);
        }
        if (s->text)
            tb_DisposePtr(s->text);
        s->text = tb_NewPtr((int32_t)say_len, true, false);
        if (s->text) {
            gput(s->text, say, (uint32_t)say_len);
            apply(s);
            err = tb_SpeakText(s->chan, s->text, (uint32_t)say_len);
            /* Over before MacinTalk returns, as it was. */
            gaddr st = tb_NewPtr(8, true, true);
            double give_up = now_seconds() + 30 + say_len / 4.0;
            while (err == noErr && st) {
                tb_SpeechBusy(); /* delivers the host's news */
                if (tb_GetSpeechInfo(s->chan, (int32_t)fourcc("stat"), st) != noErr || !rd8(st))
                    break;
                if (now_seconds() > give_up) {
                    tb_StopSpeech(s->chan);
                    break;
                }
                struct timespec ts = { 0, 10000000 };
                nanosleep(&ts, NULL);
            }
            if (st)
                tb_DisposePtr(st);
        }
        free(host);
        free(ph);
    }
    wr32(pb + IOParam_ioActCount_off, count);
    c->d[0] = (uint32_t)(int32_t)err;
}

/* SpeechSex (0), SpeechRate (2), the pitch mode (3) and SpeechPitch (4);
 * values out of range are left as they were, as MacinTalk did. */
static void control_native(cpu68k *c, uint16_t tw)
{
    (void)tw;
    gaddr pb = c->a[0];
    mt_speech *s = speech_of(c->a[1], false);
    int16_t v = (int16_t)rd16(pb + CntrlParam_csParam_off);
    if (s) {
        switch ((int16_t)rd16(pb + CntrlParam_csCode_off)) {
        case 0: if (v == 0 || v == 1) s->sex = v; break;
        case 2: if (v >= 0 && v <= 425) s->rate = v; break;
        case 3: if (v == 0 || v == 1) s->robotic = v; break;
        case 4: if (v >= 65 && v <= 500) s->pitch = v; break;
        }
    }
    c->d[0] = noErr;
}

static void status_native(cpu68k *c, uint16_t tw)
{
    (void)tw;
    c->d[0] = noErr;
}

/* SpeechOff: the channel and the exceptions file closed, and the entry
 * out of the unit table, as MacinTalk's Close did. */
static void close_native(cpu68k *c, uint16_t tw)
{
    (void)tw;
    gaddr dce = c->a[1];
    mt_speech *s = speech_of(dce, false);
    if (s) {
        if (s->chan) {
            tb_StopSpeech(s->chan);
            tb_DisposeSpeechChannel(s->chan);
        }
        if (s->text)
            tb_DisposePtr(s->text);
        if (s->exceptions)
            tb_CloseResFile(s->exceptions);
        memset(s, 0, sizeof *s);
    }
    int unit = -1 - (int16_t)rd16(dce + DCtlEntry_dCtlRefNum_off);
    gaddr table = rd32(LM_UTableBase);
    if (table && unit >= 0 && unit < (int)rd16(LM_UnitNtryCnt)) {
        gaddr h = rd32(table + 4 * (uint32_t)unit);
        if (h && rd32(h) == dce) {
            wr32(table + 4 * (uint32_t)unit, 0);
            tb_DisposeHandle(h);
        }
    }
    c->d[0] = noErr;
}

/* ---------------------------------------------------------------------- */
/* The file */

void macintalk_reset(void)
{
    for (int i = 0; i < MAX_SPEECH; i++)
        speech[i].refnum = 0; /* the channels went with the last run's heap */
    for (int i = 0; i < MAX_READ; i++) {
        free(reads[i].english);
        free(reads[i].phonemes);
    }
    memset(reads, 0, sizeof reads);
    open_stub = native_register(open_native, "MacinTalk Open", 0);
    prime_stub = native_register(prime_native, "MacinTalk Prime", 0);
    control_stub = native_register(control_native, "MacinTalk Control", 0);
    status_stub = native_register(status_native, "MacinTalk Status", 0);
    close_stub = native_register(close_native, "MacinTalk Close", 0);
    reader_stub = native_register(reader_native, "MacinTalk Reader", 0);
}

static const char marker[] = "Blue Box MacinTalk: speaks with the Speech Manager";

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* The driver: header, then JSR to each native; Prime, Control and Status
 * end through jIODone, Open and Close with RTS. */
static size_t driver_code(uint8_t *d)
{
    static const uint8_t name[8] = { 7, '.', 'S', 'P', 'E', 'E', 'C', 'H' };
    memset(d, 0, 26);
    d[0] = 0x46; /* dNeedLock, dCtlEnable, dWritEnable: MacinTalk's flags */
    memcpy(d + 18, name, sizeof name);
    size_t k = 26;
    const uint32_t stubs[5] = { open_stub, prime_stub, control_stub, status_stub, close_stub };
    for (int r = 0; r < 5; r++) {
        d[8 + 2 * r] = (uint8_t)(k >> 8);
        d[9 + 2 * r] = (uint8_t)k;
        d[k] = 0x4E;
        d[k + 1] = 0xB9; /* JSR stub */
        put32(d + k + 2, stubs[r]);
        k += 6;
        if (r >= 1 && r <= 3) { /* MOVE.L jIODone,-(SP) */
            static const uint8_t iodone[4] = { 0x2F, 0x38, 0x08, 0xFC };
            memcpy(d + k, iodone, 4);
            k += 4;
        }
        d[k] = 0x4E;
        d[k + 1] = 0x75; /* RTS */
        k += 2;
    }
    return k;
}

/* "MacinTalk" in the System Folder, unless it is there already: ours as
 * this build makes it, or the real one, which is left for programs to
 * use. */
void macintalk_install_file(void)
{
    if (!open_stub)
        return;
    uint8_t drvr[128];
    size_t dlen = driver_code(drvr);
    uint8_t talk[6] = { 0x4E, 0xF9 }; /* JMP reader */
    put32(talk + 2, reader_stub);
    uint8_t str[sizeof marker];
    str[0] = (uint8_t)(sizeof marker - 1);
    memcpy(str + 1, marker, sizeof marker - 1);

    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/MacinTalk", fm_system_folder_host());
    mac_file old;
    char err[256];
    if (appledouble_read(path, &old, err, sizeof err)) {
        rsrc_index idx;
        bool ours = false, same_drvr = false, same_talk = false;
        if (old.rsrc_len && rsrc_parse(old.rsrc, old.rsrc_len, &idx, err, sizeof err)) {
            for (size_t i = 0; i < idx.count; i++) {
                const rsrc_entry *e = &idx.entries[i];
                const uint8_t *data = rsrc_data(old.rsrc, &idx, e);
                if (e->type == fourcc("STR ") && e->id == 14 && e->data_len == sizeof str &&
                    !memcmp(data, str, sizeof str))
                    ours = true;
                if (e->type == fourcc("DRVR") && e->data_len == dlen && !memcmp(data, drvr, dlen))
                    same_drvr = true;
                if (e->type == fourcc("TALK") && e->id == 1 && e->data_len == sizeof talk &&
                    !memcmp(data, talk, sizeof talk))
                    same_talk = true;
            }
            rsrc_index_free(&idx);
        }
        mac_file_free(&old);
        if (!ours || (same_drvr && same_talk))
            return;
    }
    rsrc_spec specs[3] = {
        { fourcc("DRVR"), 31, ".SPEECH", 0, drvr, (uint32_t)dlen },
        { fourcc("TALK"), 1, "", 0, talk, sizeof talk },
        { fourcc("STR "), 14, "", 0, str, sizeof str },
    };
    uint8_t *fork;
    size_t len;
    if (!rsrc_build(specs, 3, &fork, &len))
        return;
    mac_file mf;
    memset(&mf, 0, sizeof mf);
    mf.data = malloc(1);
    mf.rsrc = fork;
    mf.rsrc_len = len;
    memcpy(mf.finder_info, "ZSYSMACS", 8); /* as Apple's */
    mf.has_finder_info = true;
    strcpy(mf.name, "MacinTalk");
    appledouble_write(path, &mf, err, sizeof err);
    mac_file_free(&mf);
}
