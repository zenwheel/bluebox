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
 * IH EH AE ... with stress digits), from the dictionary or else by rough
 * letter-to-sound rules, and remembers what it was given: phonemes that
 * are still what it made are spoken as the English.  Other phonemes (a
 * program's own, written by hand: all of the Talking Moose's phrases) go
 * to the Speech Manager as its phonemes, [[inpt PHON]], which turns them
 * back into words for the host's voice.  MacinTalk was synchronous: the
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
#include "hw68k.h"
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
    /* The _Write going on */
    bool speaking, stopped;
    uint32_t count;
    int16_t err;
    double give_up;
    gaddr stop_word; /* A6 while it waits: -1 there cuts the speech off */
} mt_speech;
static mt_speech speech[MAX_SPEECH];
static int given_rate = -1, given_pitch = -1, given_robotic = -1; /* macintalk_settings' */
static macintalk_swap_fn swap_fn; /* macintalk_host's */
static double pause_before, pause_after;

static uint32_t open_stub, prime_stub, wait_stub, finish_stub, control_stub, status_stub, close_stub, reader_stub;

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
    free_one->rate = given_rate >= 0 ? given_rate : mt_rate;
    free_one->pitch = given_pitch >= 0 ? given_pitch : mt_pitch;
    free_one->robotic = given_robotic > 0;
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
    bb_free(reads[MAX_READ - 1].english);
    bb_free(reads[MAX_READ - 1].phonemes);
    memmove(&reads[1], &reads[0], sizeof reads[0] * (MAX_READ - 1));
    reads[0].english = bb_malloc(elen ? elen : 1, BB_BYTES);
    reads[0].phonemes = bb_malloc(plen ? plen : 1, BB_BYTES);
    memcpy(reads[0].english, english, elen);
    memcpy(reads[0].phonemes, phonemes, plen);
    reads[0].elen = elen;
    reads[0].plen = plen;
}

/* The Reader for the host: English's phonemes in a new handle (0 if there
 * is no room), remembered, so that saying them says the English. */
gaddr macintalk_read(const uint8_t *english, size_t len)
{
    size_t room = len * 6 + 16;
    char *ph = bb_malloc(room, BB_BYTES);
    size_t n = english_to_mt(english, len, ph, room);
    gaddr h = tb_NewHandle((int32_t)n, true, false); /* the system heap: kept by code that lives there */
    if (h && rd32(h)) {
        gput(rd32(h), ph, (uint32_t)n);
        remember(english, len, ph, n);
    }
    bb_free(ph);
    return h;
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
        uint8_t *english = bb_malloc(len ? len : 1, BB_BYTES);
        gget(english, text, len);
        size_t room = (size_t)len * 6 + 16;
        char *ph = bb_malloc(room, BB_BYTES);
        size_t n = english_to_mt(english, len, ph, room);
        tb_SetHandleSize(out, (int32_t)n);
        err = (int16_t)rd16(LM_MemErr);
        if (err == noErr && rd32(out)) {
            gput(rd32(out), ph, (uint32_t)n);
            remember(english, len, ph, n);
        }
        bb_free(ph);
        bb_free(english);
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
    /* Robotic: the host's Fred (or Kathy), the voices of MacinTalk's time, in a monotone. */
    speech_channel_host_voice(s->chan, !s->robotic ? NULL : s->sex ? "Kathy" : "Fred");
    tb_DisposePtr(spec);
}

/* A quiet moment in MacinTalk: no interrupt tasks, which would take it
 * for speech (the Talking Moose moves its mouth from a VBL task whenever
 * MacinTalk is at work). */
static void wait_seconds(double secs)
{
    for (double end = now_seconds() + secs; now_seconds() < end;) {
        struct timespec ts = { 0, 10000000 };
        nanosleep(&ts, NULL);
    }
}

/* What the system code drew on the screen (the Moose itself, its mouth
 * shut) as the speech began, to put back for the moment after it. */
static uint8_t *drawn;
static int16_t drawn_rect[4];

static void keep_drawn(void)
{
    bb_free(drawn);
    drawn = NULL;
    bool on;
    machine_overlay(drawn_rect, &on);
    if (!on)
        return;
    gaddr base = rd32(LM_ScrnBase);
    uint32_t rb = qd_screen_row_bytes(), depth = (uint32_t)qd_screen_depth();
    uint32_t left = (uint32_t)drawn_rect[1] * depth / 8, right = ((uint32_t)drawn_rect[3] * depth + 7) / 8;
    uint32_t rows = (uint32_t)(drawn_rect[2] - drawn_rect[0]);
    if (right <= left || !rows || !(drawn = bb_malloc((size_t)(right - left) * rows, BB_BYTES)))
        return;
    for (uint32_t y = 0; y < rows; y++)
        gget(drawn + (size_t)y * (right - left), base + ((uint32_t)drawn_rect[0] + y) * rb + left, right - left);
}

static void put_back_drawn(void)
{
    if (!drawn)
        return;
    gaddr base = rd32(LM_ScrnBase);
    uint32_t rb = qd_screen_row_bytes(), depth = (uint32_t)qd_screen_depth();
    uint32_t left = (uint32_t)drawn_rect[1] * depth / 8, right = ((uint32_t)drawn_rect[3] * depth + 7) / 8;
    for (uint32_t y = 0; y < (uint32_t)(drawn_rect[2] - drawn_rect[0]); y++)
        gput(base + ((uint32_t)drawn_rect[0] + y) * rb + left, drawn + (size_t)y * (right - left), right - left);
    bb_free(drawn);
    drawn = NULL;
}

/* Prime: MacinTalk(theSpeech, phonemes), a _Write, in three parts: the
 * speech begun (prime_native), waited for (wait_native) and finished
 * (finish_native).  MacinTalk was synchronous: the _Write returns when
 * the utterance is over, or cut off. */
static void prime_native(cpu68k *c, uint16_t tw)
{
    (void)tw;
    gaddr pb = c->a[0];
    mt_speech *s = speech_of(c->a[1], false);
    if (!s)
        return;
    s->speaking = s->stopped = false;
    s->count = 0;
    s->err = noErr;
    if ((rd16(pb + IOParam_ioTrap_off) & 0xFF) != 3) {
        s->err = writErr;
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
    s->count = count;
    if (!s->chan) {
        gaddr out = tb_NewPtr(4, true, true);
        if (tb_NewSpeechChannel(0, out) == noErr)
            s->chan = rd32(out);
        tb_DisposePtr(out);
    }
    if (!s->chan || !count)
        return;
    uint8_t *ph = bb_malloc(count, BB_BYTES);
    size_t n = count;
    gget(ph, p, count);
    const uint8_t *say = NULL;
    size_t say_len = 0;
    /* The host may say something else (English, or other phonemes). */
    uint8_t *swapped = NULL;
    size_t swapped_len = 0;
    bool english = false;
    if (swap_fn && swap_fn(ph, n, &swapped, &swapped_len, &english)) {
        if (english) {
            say = swapped;
            say_len = swapped_len;
        } else {
            bb_free(ph);
            ph = swapped;
            n = swapped_len;
            swapped = NULL;
        }
    }
    /* The Reader's own phonemes say its English; others are phonemes,
     * which the Speech Manager makes words of. */
    for (int i = 0; i < MAX_READ && !say; i++)
        if (reads[i].phonemes && reads[i].plen == n && !memcmp(reads[i].phonemes, ph, n)) {
            say = reads[i].english;
            say_len = reads[i].elen;
        }
    char *host = NULL;
    if (!say) {
        size_t room = n * 4 + 40;
        host = bb_malloc(room, BB_BYTES);
        strcpy(host, "[[inpt PHON]]");
        mt_to_host(ph, n, host + strlen(host), room - strlen(host) - 16);
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
        if (pause_after > 0)
            keep_drawn();
        wait_seconds(pause_before);
        s->err = tb_SpeakText(s->chan, s->text, (uint32_t)say_len);
        s->speaking = s->err == noErr;
        s->give_up = now_seconds() + 30 + say_len / 4.0;
    }
    bb_free(host);
    bb_free(swapped);
    bb_free(ph);
}

/* Where MacinTalk 1.31 waited for the next tick between the frames it
 * made: interrupts during the speech are taken there.  The Talking
 * Moose's VBL task looks for the interrupted PC at $281C to $2858 of the
 * driver; for Quick Stop it then ends the speech by writing -1 at A6
 * (MacinTalk's next frame, here a word that says stop) and sends the
 * return address on the stack through its own code, which silences the
 * sound buffer. */
enum { mt_wait_pc = 0x281C, mt_wait_code = 0x2814 };

/* Until the speech is over or cut off. */
static void wait_native(cpu68k *c, uint16_t tw)
{
    (void)tw;
    mt_speech *s = speech_of(c->a[1], false);
    if (!s || !s->speaking)
        return;
    gaddr dce = c->a[1];
    gaddr drv = rd32(dce + DCtlEntry_dCtlDriver_off);
    if (rd16(dce + DCtlEntry_dCtlFlags_off) & 0x0040) /* dRAMBased: a handle */
        drv = rd32(drv);
    drv = mem_strip(drv); /* a master pointer's flags aren't part of it */
    if (!s->stop_word)
        s->stop_word = tb_NewPtr(2, true, true);
    gaddr st = tb_NewPtr(8, true, true);
    uint32_t sp = c->a[7], pc = c->pc, a6 = c->a[6];
    while (st) {
        tb_SpeechBusy(); /* delivers the host's news */
        if (tb_GetSpeechInfo(s->chan, (int32_t)fourcc("stat"), st) != noErr || !rd8(st))
            break;
        if (now_seconds() > s->give_up) {
            tb_StopSpeech(s->chan);
            break;
        }
        struct timespec ts = { 0, 10000000 };
        nanosleep(&ts, NULL);
        if (!machine_wall_clock()) /* headless, the clock follows instructions */
            machine_skip_time(10000000);
        /* Interrupts went on while MacinTalk spoke (the Talking Moose
         * moves its mouth from a VBL task), taken in its wait, the return
         * to the _Write's code on the stack. */
        if (s->stop_word && drv) {
            wr16(s->stop_word, 0);
            c->a[6] = s->stop_word;
            c->a[7] = sp + 4;
            c->pc = drv + mt_wait_pc;
        }
        uint32_t back = rd32(sp);
        machine_advance_ticks();
        events_sync_lowmem(); /* KeyTime and MBTicks: Quick Stop watches them */
        time_pump();
        wr32(sp, back); /* the interrupt's frame went over it */
        c->a[6] = a6;
        c->a[7] = sp;
        c->pc = pc;
        if (s->stop_word && (rd16(s->stop_word) & 0x8000)) { /* Quick Stop */
            tb_StopSpeech(s->chan);
            s->stopped = true;
            break;
        }
    }
    if (st)
        tb_DisposePtr(st);
}

static void finish_native(cpu68k *c, uint16_t tw)
{
    (void)tw;
    gaddr pb = c->a[0];
    mt_speech *s = speech_of(c->a[1], false);
    if (!s) {
        c->d[0] = (uint32_t)(int32_t)writErr;
        return;
    }
    if (s->speaking) {
        if (pause_after > 0)
            put_back_drawn();
        if (!s->stopped) /* cut off: gone at once */
            wait_seconds(pause_after);
        s->speaking = false;
    }
    if (s->err != writErr)
        wr32(pb + IOParam_ioActCount_off, s->count);
    c->d[0] = (uint32_t)(int32_t)s->err;
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
        if (s->stop_word)
            tb_DisposePtr(s->stop_word);
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

void macintalk_settings(int rate, int pitch, int robotic)
{
    if (rate >= 0 && rate <= 425)
        given_rate = rate;
    if (pitch >= 65 && pitch <= 500)
        given_pitch = pitch;
    if (robotic == 0 || robotic == 1)
        given_robotic = robotic;
    for (int i = 0; i < MAX_SPEECH; i++)
        if (speech[i].refnum) {
            if (given_rate >= 0) speech[i].rate = given_rate;
            if (given_pitch >= 0) speech[i].pitch = given_pitch;
            if (given_robotic >= 0) speech[i].robotic = given_robotic;
        }
}

void macintalk_host(macintalk_swap_fn swap, double before, double after)
{
    swap_fn = swap;
    pause_before = before;
    pause_after = after;
}

void macintalk_reset(void)
{
    macintalk_host(NULL, 0, 0);
    given_rate = given_pitch = given_robotic = -1;
    for (int i = 0; i < MAX_SPEECH; i++)
        speech[i].refnum = 0; /* the channels went with the last run's heap */
    for (int i = 0; i < MAX_READ; i++) {
        bb_free(reads[i].english);
        bb_free(reads[i].phonemes);
    }
    memset(reads, 0, sizeof reads);
    open_stub = native_register(open_native, "MacinTalk Open", 0);
    prime_stub = native_register(prime_native, "MacinTalk Prime", 0);
    wait_stub = native_register(wait_native, "MacinTalk Wait", 0);
    finish_stub = native_register(finish_native, "MacinTalk Finish", 0);
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

static size_t put_jsr(uint8_t *d, size_t k, uint32_t stub)
{
    d[k] = 0x4E;
    d[k + 1] = 0xB9; /* JSR stub */
    put32(d + k + 2, stub);
    return k + 6;
}

/* The driver: header, then JSR to each native; Prime, Control and Status
 * end through jIODone, Open and Close with RTS.  Prime's wait is a
 * routine of its own, below where MacinTalk 1.31's was (mt_wait_pc), the
 * driver as long as MacinTalk's code to there. */
enum { driver_size = mt_wait_pc + 0x40 };

static size_t driver_code(uint8_t *d)
{
    static const uint8_t name[8] = { 7, '.', 'S', 'P', 'E', 'E', 'C', 'H' };
    static const uint8_t iodone[6] = { 0x2F, 0x38, 0x08, 0xFC, 0x4E, 0x75 }; /* MOVE.L jIODone,-(SP); RTS */
    memset(d, 0, driver_size);
    d[0] = 0x46; /* dNeedLock, dCtlEnable, dWritEnable: MacinTalk's flags */
    memcpy(d + 18, name, sizeof name);
    size_t k = 26;
    const uint32_t stubs[5] = { open_stub, prime_stub, control_stub, status_stub, close_stub };
    for (int r = 0; r < 5; r++) {
        d[8 + 2 * r] = (uint8_t)(k >> 8);
        d[9 + 2 * r] = (uint8_t)k;
        k = put_jsr(d, k, stubs[r]);
        if (r == 1) {
            /* The wait, D3/A0/A1 kept: the Moose's Quick Stop returns
             * through code that uses them. */
            static const uint8_t save[4] = { 0x48, 0xE7, 0x10, 0xC0 }; /* MOVEM.L D3/A0-A1,-(SP) */
            static const uint8_t restore[4] = { 0x4C, 0xDF, 0x03, 0x08 }; /* MOVEM.L (SP)+,D3/A0-A1 */
            memcpy(d + k, save, 4);
            k += 4;
            d[k] = 0x61;
            d[k + 1] = 0x00; /* BSR.W wait */
            int32_t disp = (int32_t)mt_wait_code - (int32_t)(k + 2);
            d[k + 2] = (uint8_t)(disp >> 8);
            d[k + 3] = (uint8_t)disp;
            k += 4;
            memcpy(d + k, restore, 4);
            k += 4;
            k = put_jsr(d, k, finish_stub);
        }
        if (r >= 1 && r <= 3) {
            memcpy(d + k, iodone, sizeof iodone);
            k += sizeof iodone;
        } else {
            d[k] = 0x4E;
            d[k + 1] = 0x75; /* RTS */
            k += 2;
        }
    }
    k = put_jsr(d, mt_wait_code, wait_stub);
    d[k] = 0x4E;
    d[k + 1] = 0x75; /* RTS */
    return driver_size;
}

/* Apple's MacinTalk (1.31 and the others, a file a program keeps beside
 * it, which OpenResFile finds before the System Folder's) makes its sound
 * in a compact Mac's sound buffer, which the VIA's video circuitry plays,
 * and nowhere else: on a machine without that hardware it is silent.  A
 * program opening it there gets ours; the file is left as it is.  (On
 * --machine plus, se or classic it speaks, and is left to.) */
bool macintalk_substitute(const uint8_t *rsrc, size_t len, char *out, size_t outsize)
{
    const hw_profile *hw = hw_machine();
    if (!open_stub || !rsrc || !len || (hw && hw->via_sound))
        return false;
    rsrc_index idx;
    char err[128];
    if (!rsrc_parse(rsrc, len, &idx, err, sizeof err))
        return false;
    bool speech = false, ours = false;
    for (size_t i = 0; i < idx.count; i++) {
        const rsrc_entry *e = &idx.entries[i];
        if (e->type == fourcc("DRVR") && !strcasecmp(e->name, ".SPEECH"))
            speech = true;
        if (e->type == fourcc("STR ") && e->id == 14 && e->data_len == sizeof marker &&
            !memcmp(rsrc_data(rsrc, &idx, e) + 1, marker, sizeof marker - 1))
            ours = true;
    }
    rsrc_index_free(&idx);
    if (!speech || ours)
        return false;
    macintalk_install_file();
    snprintf(out, outsize, "%s/MacinTalk", fm_system_folder_host());
    return fm_host_exists(out);
}

/* "MacinTalk" in the System Folder, unless it is there already: ours as
 * this build makes it, or the real one, which is left for programs to
 * use. */
void macintalk_install_file(void)
{
    if (!open_stub)
        return;
    static uint8_t drvr[driver_size];
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
    mf.data = bb_malloc(1, BB_BYTES);
    mf.rsrc = fork;
    mf.rsrc_len = len;
    memcpy(mf.finder_info, "ZSYSMACS", 8); /* as Apple's */
    mf.has_finder_info = true;
    strcpy(mf.name, "MacinTalk");
    appledouble_write(path, &mf, err, sizeof err);
    mac_file_free(&mf);
}
