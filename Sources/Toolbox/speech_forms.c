/*
 * Spoken forms: a word of text as the host's voice says it, as phonemes.
 *
 * A channel's phoneme callbacks are made up from the words the host
 * reports (speech.c), and many words aren't said as their letters: a URL
 * is read out part by part ("h t t p s colon forward slash forward slash
 * w w w dot ..."), an address has its "at" and "dot"s, numbers are said as
 * numbers (years as years, decimals digit by digit), most acronyms letter
 * by letter, and symbols by their names.  Each form here was checked
 * against macOS's voice, by the length of "Say <word> now." against the
 * same with the word written out, so that a word's phonemes last about as
 * long as its sound does.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "machine.h"
#include "tb_internal.h"

typedef struct {
    uint8_t *out;
    int n, max;
} sink;

/* Phonemes written as MacinTalk's symbols. */
static void symbols(sink *s, const char *sym)
{
    if (s->n < s->max)
        s->n += speech_symbol_phonemes((const uint8_t *)sym, (int)strlen(sym), s->out + s->n, s->max - s->n);
}

/* The words the forms below say, as the voice says them (the letter rules
 * get "one", "eight" and "colon" wrong, and their lengths with them). */
static const struct {
    const char *word, *ph;
} lexicon[] = {
    { "zero", "zIYrOW" }, { "oh", "OW" }, { "one", "wUXn" }, { "two", "tUW" }, { "three", "TrIY" },
    { "four", "fAOr" }, { "five", "fAYv" }, { "six", "sIHks" }, { "seven", "sEHvAXn" }, { "eight", "EYt" },
    { "nine", "nAYn" }, { "ten", "tEHn" }, { "eleven", "IXlEHvAXn" }, { "twelve", "twEHlv" },
    { "thirteen", "TUXrtIYn" }, { "fourteen", "fAOrtIYn" }, { "fifteen", "fIHftIYn" }, { "sixteen", "sIHkstIYn" },
    { "seventeen", "sEHvAXntIYn" }, { "eighteen", "EYtIYn" }, { "nineteen", "nAYntIYn" }, { "twenty", "twEHntIY" },
    { "thirty", "TUXrtIY" }, { "forty", "fAOrtIY" }, { "fifty", "fIHftIY" }, { "sixty", "sIHkstIY" },
    { "seventy", "sEHvAXntIY" }, { "eighty", "EYtIY" }, { "ninety", "nAYntIY" }, { "hundred", "hUXndrAXd" },
    { "thousand", "TAWzAXnd" }, { "million", "mIHlyAXn" }, { "billion", "bIHlyAXn" }, { "trillion", "trIHlyAXn" },
    { "first", "fUXrst" }, { "second", "sEHkAXnd" }, { "third", "TUXrd" }, { "fifth", "fIHfT" },
    { "eighth", "EYtT" }, { "ninth", "nAYnT" }, { "twelfth", "twEHlfT" },
    { "and", "AEnd" }, { "at", "AEt" }, { "dot", "dAAt" }, { "colon", "kOWlAXn" }, { "forward", "fAOrwUXrd" },
    { "slash", "slAES" }, { "point", "pOYnt" }, { "minus", "mAYnAXs" }, { "plus", "plUXs" },
    { "equals", "IYkwAXlz" }, { "percent", "pUXrsEHnt" }, { "dollar", "dAAlUXr" }, { "dollars", "dAAlUXrz" },
    { "cent", "sEHnt" }, { "cents", "sEHnts" }, { "number", "nUXmbUXr" }, { "degrees", "dIXgrIYz" },
    { "less", "lEHs" }, { "than", "DAEn" }, { "greater", "grEYtUXr" }, { "question", "kwEHsCAXn" },
    { "mark", "mAArk" }, { "tilde", "tIHldAX" }, { "o'clock", "AXklAAk" }, { "pounds", "pAWndz" },
    { "euros", "yUXrOWz" }, { "yen", "yEHn" }, { "copyright", "kAApIYrAYt" }, { "registered", "rEHJIHstUXrd" },
    { "trademark", "trEYdmAArk" }, { "mister", "mIHstUXr" }, { "missus", "mIHsIXz" }, { "doctor", "dAAktUXr" },
    { "street", "strIYt" }, { "versus", "vUXrsAXs" }, { "et", "EHt" }, { "cetera", "sEHtUXrAX" },
    { "junior", "JUWnyUXr" }, { "senior", "sIYnyUXr" }, { "avenue", "AEvAXnUW" },
};

/* The letters' names, for spelling. */
static const char *const letter_names[26] = {
    "EY", "bIY", "sIY", "dIY", "IY", "EHf", "JIY", "EYC", "AY", "JEY", "kEY", "EHl", "EHm",
    "EHn", "OW", "pIY", "kyUW", "AAr", "EHs", "tIY", "yUW", "vIY", "dUXbAXlyUW", "EHks", "wAY", "zIY",
};

static bool lookup(sink *s, const char *w, int n)
{
    for (size_t i = 0; i < sizeof lexicon / sizeof lexicon[0]; i++)
        if ((int)strlen(lexicon[i].word) == n && !memcmp(lexicon[i].word, w, (size_t)n)) {
            symbols(s, lexicon[i].ph);
            return true;
        }
    return false;
}

static void spell(sink *s, int ch)
{
    ch |= 0x20;
    if (ch >= 'a' && ch <= 'z')
        symbols(s, letter_names[ch - 'a']);
}

static bool is_vowel(const char *w, int i)
{
    int ch = w[i];
    return ch == 'a' || ch == 'e' || ch == 'i' || ch == 'o' || ch == 'u' || (ch == 'y' && i > 0);
}

/* Whether letters (lower case) can be said as a word: a vowel, and an
 * English onset before it.  "www", "txt" and "http"
 * can't, and are spelled. */
static bool pronounceable(const char *w, int n)
{
    static const char *const onsets[] = {
        "bl", "br", "ch", "cl", "cr", "dr", "dw", "fl", "fr", "gl", "gr", "gn", "kl", "kn", "kr", "ph", "pl",
        "pr", "ps", "qu", "rh", "sc", "sh", "sk", "sl", "sm", "sn", "sp", "st", "sw", "th", "tr", "tw", "wh",
        "wr", "sch", "scr", "shr", "spl", "spr", "squ", "str", "thr", "chr", "phr", "sph",
    };
    int first = -1;
    for (int i = 0; i < n && first < 0; i++)
        if (is_vowel(w, i))
            first = i;
    if (first < 0)
        return false;
    if (first <= 1)
        return true;
    for (size_t k = 0; k < sizeof onsets / sizeof onsets[0]; k++)
        if ((int)strlen(onsets[k]) == first && !memcmp(onsets[k], w, (size_t)first))
            return true;
    return false;
}

/* Letters said as a word if they can be, else spelled: the letters before
 * a sayable rest one by one ("jpeg": j, "peg"), or all of them. */
static void letters(sink *s, const char *w, int n)
{
    while (n > 0 && !pronounceable(w, n)) {
        bool any = false;
        for (int i = 0; i < n; i++)
            any |= is_vowel(w, i);
        if (!any) {
            for (int i = 0; i < n; i++)
                spell(s, w[i]);
            return;
        }
        spell(s, *w++);
        n--;
    }
    for (int i = 0; i < n && s->n < s->max; i += 32) /* the letter rules take a word at most 64 long */
        s->n += speech_word_phonemes((const uint8_t *)w + i, n - i < 32 ? n - i : 32, s->out + s->n, s->max - s->n);
}

/* A word (lower case): from the lexicon, an ordinal of one there
 * ("fourth", "twentieth"), or by its letters. */
static void say_word(sink *s, const char *w, int n)
{
    if (lookup(s, w, n))
        return;
    char base[24];
    if (n > 4 && n < (int)sizeof base && !memcmp(w + n - 4, "ieth", 4)) {
        memcpy(base, w, (size_t)n - 4);
        base[n - 4] = 'y';
        if (lookup(s, base, n - 3)) {
            symbols(s, "IXT");
            return;
        }
    }
    if (n > 2 && n < (int)sizeof base && !memcmp(w + n - 2, "th", 2) && lookup(s, w, n - 2)) {
        symbols(s, "T");
        return;
    }
    letters(s, w, n);
}

/* Words separated by spaces. */
static void say_words(sink *s, const char *text)
{
    while (*text) {
        while (*text == ' ')
            text++;
        int n = 0;
        while (text[n] && text[n] != ' ')
            n++;
        if (n)
            say_word(s, text, n);
        text += n;
    }
}

/* ---------------------------------------------------------------------- */
/* Numbers, written out as words */

typedef struct {
    char b[512];
    size_t n;
} words;

static void add(words *w, const char *word)
{
    int k = snprintf(w->b + w->n, sizeof w->b - w->n, "%s%s", w->n ? " " : "", word);
    if (k > 0 && w->n + (size_t)k < sizeof w->b)
        w->n += (size_t)k;
}

static const char *const ones[20] = {
    "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
    "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen",
};
static const char *const tens[10] = { "", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety" };

static void below_1000(words *w, unsigned v)
{
    if (v >= 100) {
        add(w, ones[v / 100]);
        add(w, "hundred");
        v %= 100;
        if (!v)
            return;
    }
    if (v >= 20) {
        add(w, tens[v / 10]);
        if (v % 10)
            add(w, ones[v % 10]);
    } else {
        add(w, ones[v]);
    }
}

/* As macOS says them: "one thousand two hundred thirty four", no "and". */
static void cardinal(words *w, uint64_t v)
{
    static const char *const scale[] = { "", "thousand", "million", "billion", "trillion" };
    if (!v) {
        add(w, "zero");
        return;
    }
    unsigned groups[5] = { 0 };
    int ng = 0;
    for (; v && ng < 5; ng++, v /= 1000)
        groups[ng] = (unsigned)(v % 1000);
    for (int g = ng - 1; g >= 0; g--)
        if (groups[g]) {
            below_1000(w, groups[g]);
            if (g)
                add(w, scale[g]);
        }
}

/* Years: "nineteen eighty four", "nineteen hundred", "twenty ten". */
static void year(words *w, unsigned v)
{
    below_1000(w, v / 100);
    if (v % 100 == 0) {
        add(w, "hundred");
    } else if (v % 100 < 10) {
        add(w, "oh");
        add(w, ones[v % 10]);
    } else {
        below_1000(w, v % 100);
    }
}

/* The last word made an ordinal: "one" "first", "twenty" "twentieth". */
static void ordinal(words *w)
{
    static const struct { const char *card, *ord; } irregular[] = {
        { "one", "first" }, { "two", "second" }, { "three", "third" }, { "five", "fifth" },
        { "eight", "eighth" }, { "nine", "ninth" }, { "twelve", "twelfth" },
    };
    size_t start = w->n;
    while (start && w->b[start - 1] != ' ')
        start--;
    char last[24];
    snprintf(last, sizeof last, "%s", w->b + start);
    w->n = start ? start - 1 : 0;
    w->b[w->n] = 0;
    for (size_t i = 0; i < sizeof irregular / sizeof irregular[0]; i++)
        if (!strcmp(last, irregular[i].card)) {
            add(w, irregular[i].ord);
            return;
        }
    size_t l = strlen(last);
    char ord[32];
    if (l && last[l - 1] == 'y')
        snprintf(ord, sizeof ord, "%.*sieth", (int)l - 1, last);
    else
        snprintf(ord, sizeof ord, "%sth", last);
    add(w, ord);
}

/* Digits one by one ("oh oh seven", "point one four one"). */
static void digits(words *w, const char *d, int n, const char *zero)
{
    for (int i = 0; i < n; i++)
        add(w, d[i] == '0' ? zero : ones[d[i] - '0']);
}

/* A whole number's digits: leading zeros and very long ones digit by
 * digit, years as years when they are years. */
static void whole(words *w, const char *d, int n, bool year_context)
{
    if (n > 1 && d[0] == '0') {
        digits(w, d, n, n == 2 ? "zero" : "oh"); /* "zero one", "oh oh seven" */
        return;
    }
    if (n > 15) {
        digits(w, d, n, "zero");
        return;
    }
    uint64_t v = 0;
    for (int i = 0; i < n; i++)
        v = v * 10 + (uint64_t)(d[i] - '0');
    if (n == 4 && ((v >= 1900 && v <= 1999) || (year_context && ((v >= 1100 && v <= 1999) || (v >= 2010 && v <= 2099)))))
        year(w, (unsigned)v);
    else
        cardinal(w, v);
}

static bool digit(int ch) { return ch >= '0' && ch <= '9'; }
static bool letter(int ch) { return (ch | 0x20) >= 'a' && (ch | 0x20) <= 'z'; }
static bool alnum(int ch) { return digit(ch) || letter(ch); }
static int lower(int ch) { return letter(ch) ? ch | 0x20 : ch; }

static bool starts(const char *t, int n, const char *p)
{
    int l = (int)strlen(p);
    if (l > n)
        return false;
    for (int i = 0; i < l; i++)
        if (lower(t[i]) != p[i])
            return false;
    return true;
}

/* Units after a number ("5kg", "10am"). */
static const struct { const char *unit, *said; } units[] = {
    { "kg", "kilograms" }, { "km", "kilometers" }, { "cm", "centimeters" }, { "mm", "millimeters" },
    { "lb", "pounds" }, { "lbs", "pounds" }, { "oz", "ounces" }, { "ft", "feet" }, { "mph", "miles per hour" },
    { "kph", "kilometers per hour" }, { "kb", "kilobytes" }, { "mb", "megabytes" }, { "gb", "gigabytes" },
    { "tb", "terabytes" }, { "hz", "hertz" }, { "khz", "kilohertz" }, { "mhz", "megahertz" },
    { "ghz", "gigahertz" }, { "ms", "milliseconds" }, { "am", "a m" }, { "pm", "p m" },
};

static const char *const months[12] = {
    "january", "february", "march", "april", "may", "june", "july", "august", "september", "october", "november", "december",
};

/* A number at t (digits, or a sign, currency or '.' before them) said into
 * w: with its thousands' commas, decimals, a time ("3:45"), a date
 * ("12/25/2024"), a phone number's dashes, and a percent sign, ordinal
 * ending, degree sign or unit after it.  Returns the characters used. */
static int number(words *w, const char *t, int n, bool url, bool year_context)
{
    int i = 0;
    const char *currency = NULL, *currency1 = NULL, *fraction = NULL, *fraction1 = NULL;
    if (t[i] == '$') {
        currency = "dollars", currency1 = "dollar", fraction = "cents", fraction1 = "cent";
        i++;
    } else if ((uint8_t)t[i] == 0xA3) { /* £ */
        currency = currency1 = "pounds";
        i++;
    } else if ((uint8_t)t[i] == 0xDB) { /* € */
        currency = currency1 = "euros";
        i++;
    } else if ((uint8_t)t[i] == 0xB4) { /* ¥ */
        currency = currency1 = "yen";
        i++;
    }
    if (i < n && (t[i] == '-' || (uint8_t)t[i] == 0xD0) && i + 1 < n && (digit(t[i + 1]) || t[i + 1] == '.')) {
        add(w, "minus");
        i++;
    } else if (i < n && t[i] == '+' && i + 1 < n && digit(t[i + 1])) {
        add(w, "plus");
        i++;
    }
    char d[64];
    int nd = 0;
    while (i < n && (digit(t[i]) || (!url && t[i] == ',' && i + 3 < n && nd && digit(t[i + 1]) && digit(t[i + 2]) &&
                                          digit(t[i + 3]) && (i + 4 >= n || !digit(t[i + 4]))))) {
        if (digit(t[i]) && nd < (int)sizeof d)
            d[nd++] = t[i];
        i++;
    }
    /* A time, a date, a phone number. */
    if (nd && nd <= 2 && i + 2 < n && t[i] == ':' && digit(t[i + 1]) && digit(t[i + 2]) && (i + 3 >= n || !digit(t[i + 3]))) {
        whole(w, d, nd, false);
        int m = (t[i + 1] - '0') * 10 + t[i + 2] - '0';
        if (!m)
            add(w, "o'clock");
        else if (m < 10)
            add(w, "oh"), add(w, ones[m]);
        else
            below_1000(w, (unsigned)m);
        return i + 3;
    }
    if (nd && nd <= 2 && !url && i + 1 < n && t[i] == '/' && digit(t[i + 1])) {
        int j = i + 1, m2 = 0, n2 = 0;
        while (j < n && digit(t[j]))
            m2 = m2 * 10 + t[j++] - '0', n2++;
        int month = nd == 1 ? d[0] - '0' : (d[0] - '0') * 10 + d[1] - '0';
        if (n2 <= 2 && j + 1 < n && t[j] == '/' && digit(t[j + 1]) && month >= 1 && month <= 12 && m2 >= 1 && m2 <= 31) {
            int k = j + 1, ny = 0;
            char y[8];
            while (k < n && digit(t[k]))
                if (ny < (int)sizeof y)
                    y[ny++] = t[k++];
                else
                    k++;
            add(w, months[month - 1]);
            below_1000(w, (unsigned)m2);
            ordinal(w);
            whole(w, y, ny, true);
            return k;
        }
    }
    if (nd == 3 && !url && i + 3 < n && t[i] == '-' && digit(t[i + 1]) && digit(t[i + 2]) && digit(t[i + 3])) { /* 555-1234 */
        digits(w, d, nd, "zero");
        while (i + 1 < n && t[i] == '-' && digit(t[i + 1]))
            for (i++; i < n && digit(t[i]); i++)
                digits(w, t + i, 1, "zero");
        return i;
    }
    bool none = nd && currency && i + 1 < n && t[i] == '.' && digit(t[i + 1]); /* "$0.50": fifty cents */
    for (int k = 0; none && k < nd; k++)
        none = d[k] == '0';
    if (none)
        nd = 0;
    if (nd)
        whole(w, d, nd, year_context && !currency);
    /* Decimals: "point one four", or dollars and cents. */
    int nf = 0;
    const char *f = NULL;
    if (!url && i + 1 < n && t[i] == '.' && digit(t[i + 1])) {
        f = t + i + 1;
        for (i++; i < n && digit(t[i]); i++)
            nf++;
    }
    if (currency) {
        bool one = nd == 1 && d[0] == '1';
        if (nd)
            add(w, one ? currency1 : currency);
        if (f && fraction) {
            unsigned c = (unsigned)(f[0] - '0') * 10 + (nf > 1 ? (unsigned)(f[1] - '0') : 0);
            if (c) {
                if (nd)
                    add(w, "and");
                below_1000(w, c);
                add(w, c == 1 ? fraction1 : fraction);
            }
        } else if (f) {
            add(w, "point");
            digits(w, f, nf, "zero");
        }
        return i;
    }
    if (f) {
        add(w, "point");
        digits(w, f, nf, "zero");
    }
    if (!nd && !f)
        return 0;
    if (nd && !f && i + 1 < n && t[i] == '/' && digit(t[i + 1])) { /* "1/2": one slash two */
        add(w, "slash");
        int k = i + 1;
        while (k < n && digit(t[k]))
            k++;
        whole(w, t + i + 1, k - i - 1, false);
        return k;
    }
    /* What follows the number. */
    if (i < n && t[i] == '%') {
        add(w, "percent");
        return i + 1;
    }
    if (i < n && (uint8_t)t[i] == 0xA1) { /* ° */
        add(w, "degrees");
        return i + 1;
    }
    if (nd && !f && i + 1 < n && (i + 2 >= n || !letter(t[i + 2]))) {
        char a = (char)lower(t[i]), b = (char)lower(t[i + 1]);
        int last = d[nd - 1] - '0', tens_digit = nd > 1 ? d[nd - 2] - '0' : 0;
        const char *want = tens_digit == 1 ? "th" : last == 1 ? "st" : last == 2 ? "nd" : last == 3 ? "rd" : "th";
        if ((a == want[0] && b == want[1]) || (a == 't' && b == 'h')) {
            ordinal(w);
            return i + 2;
        }
    }
    int l = 0;
    while (i + l < n && letter(t[i + l]))
        l++;
    for (size_t u = 0; l && u < sizeof units / sizeof units[0]; u++)
        if ((int)strlen(units[u].unit) == l && starts(t + i, l, units[u].unit)) {
            add(w, units[u].said);
            return i + l;
        }
    return i;
}

/* ---------------------------------------------------------------------- */
/* Words */

/* Abbreviations said in full when written with their period. */
static const struct { const char *abbr, *said; } abbreviations[] = {
    { "mr", "mister" }, { "mrs", "missus" }, { "dr", "doctor" }, { "st", "street" }, { "vs", "versus" },
    { "etc", "et cetera" }, { "jr", "junior" }, { "sr", "senior" }, { "ave", "avenue" },
};

/* A run of letters, as written (its case tells an acronym). */
static void piece(sink *s, const char *t, int n, bool period)
{
    char w[64];
    int caps = 0, vowels = 0;
    for (int i = 0; i < n; i++) {
        caps += t[i] >= 'A' && t[i] <= 'Z';
        if (i < (int)sizeof w)
            w[i] = (char)lower(t[i]);
    }
    if (n > (int)sizeof w) { /* longer than any word: the letter rules, in pieces */
        char *big = bb_malloc((size_t)n, BB_BYTES);
        for (int i = 0; i < n; i++)
            big[i] = (char)lower(t[i]);
        letters(s, big, n);
        bb_free(big);
        return;
    }
    if (period)
        for (size_t a = 0; a < sizeof abbreviations / sizeof abbreviations[0]; a++)
            if ((int)strlen(abbreviations[a].abbr) == n && !memcmp(abbreviations[a].abbr, w, (size_t)n)) {
                say_words(s, abbreviations[a].said);
                return;
            }
    if (n >= 2 && caps == n) { /* an acronym: FBI, IBM and USA spelled, NASA and GIF said */
        for (int i = 0; i < n; i++)
            vowels += w[i] == 'a' || w[i] == 'e' || w[i] == 'i' || w[i] == 'o' || w[i] == 'u';
        bool cvc = n == 3 && !is_vowel(w, 0) && vowels == 1 && is_vowel(w, 1) && !is_vowel(w, 2);
        if (n <= 3 ? !cvc : !vowels) {
            for (int i = 0; i < n; i++)
                spell(s, w[i]);
            return;
        }
    }
    if (n == 1) { /* a letter on its own is its name, but for "a" and "I" */
        if (w[0] == 'a' && t[0] == 'a')
            symbols(s, "AX");
        else
            spell(s, w[0]);
        return;
    }
    say_word(s, w, n);
}

/* Letters in camel case are words and acronyms run together ("MacOS",
 * "iPhone", "HTMLParser"). */
static void run(sink *s, const char *t, int n, bool period)
{
    int start = 0;
    for (int i = 1; i < n; i++) {
        bool up = t[i] >= 'A' && t[i] <= 'Z', was_up = t[i - 1] >= 'A' && t[i - 1] <= 'Z';
        bool next_low = i + 1 < n && t[i + 1] >= 'a' && t[i + 1] <= 'z';
        if (up && (!was_up || next_low) && t[i - 1] != '\'' && !(i == 2 && t[0] == 'M' && t[1] == 'c')) {
            piece(s, t + start, i - start, false);
            start = i;
        }
    }
    piece(s, t + start, n - start, period);
}

/* What a mark between words is called, where it is said: in a URL or
 * an address, or between letters ("C++", "a+b=c", "AT&T"). */
static const char *mark_name(int ch, bool url, bool between)
{
    switch (ch) {
    case '@': return "at";
    case '&': return "and";
    case '+': return "plus";
    case '=': return "equals";
    case '%': return "percent";
    case '.': return url && between ? "dot" : NULL;
    case '/': return url ? "forward slash" : NULL;
    case ':': return url ? "colon" : NULL;
    case '?': return url && between ? "question mark" : NULL;
    case '~': return url ? "tilde" : NULL;
    case '<': return between ? "less than" : NULL;
    case '>': return between ? "greater than" : NULL;
    case 0xA1: return "degrees";
    case 0xA9: return "copyright";
    case 0xA8: return "registered";
    case 0xAA: return "trademark";
    default: return NULL;
    }
}

/* Mac Roman's accented letters as their plain ones. */
static int plain(uint8_t ch)
{
    static const char accented[] = "AACENOUaaaaaaceeeeiiiinooooouuuu"; /* 0x80-0x9F */
    if (ch >= 0x80 && ch <= 0x9F)
        return accented[ch - 0x80];
    switch (ch) {
    case 0xAF: case 0xCD: case 0xEE: case 0xEF: case 0xF1: return 'O';
    case 0xBF: return 'o';
    case 0xCB: case 0xCC: case 0xE5: case 0xE7: return 'A';
    case 0xE6: case 0xE8: case 0xE9: return 'E';
    case 0xEA: case 0xEB: case 0xEC: case 0xED: return 'I';
    case 0xF2: case 0xF3: case 0xF4: return 'U';
    case 0xD8: return 'y';
    case 0xD9: return 'Y';
    case 0xA7: return 's'; /* ß */
    default: return ch;
    }
}

static bool silent_edge(uint8_t ch)
{
    return ch == '"' || ch == '\'' || ch == '(' || ch == ')' || ch == '[' || ch == ']' || ch == '{' || ch == '}' ||
           ch == ',' || ch == ';' || ch == '!' || ch == '?' || ch == '.' || ch == ':' || ch == '*' || ch == '<' ||
           ch == '>' || ch == '`' || ch == '-' || ch == '_' || ch == '~' || ch == '|' || ch == '^' ||
           (ch >= 0xD0 && ch <= 0xD5) || ch == 0xC7 || ch == 0xC8 || ch == 0xC9 || ch == 0xA5 || ch == 0xE1 ||
           ch == 0xE3 || ch == 0xE2 || ch == 0xDC || ch == 0xDD;
}

static const char *const year_words[] = {
    "in", "since", "from", "until", "till", "by", "of", "year", "circa", "before", "after", "during", "around",
    "summer", "winter", "spring", "fall", "autumn", "early", "late", "mid", "between", "to", "and",
};

/* A word of text (Mac Roman, any length) as the host's voice says it;
 * the word before it (or NULL) tells a year.  Returns the phonemes made,
 * at most max. */
int speech_spoken_phonemes(const uint8_t *text, int len, const uint8_t *prev, int prevlen, uint8_t *out, int max)
{
    sink s = { out, 0, max };
    if (len <= 0 || max <= 0)
        return 0;
    char *t = bb_malloc((size_t)len + 1, BB_BYTES);
    int n = 0;
    for (int i = 0; i < len; i++)
        t[n++] = (char)plain(text[i]);
    t[n] = 0;
    /* Quotes, brackets and punctuation at the edges aren't said; a sign or
     * currency before a number, a percent sign after one, are. */
    int a = 0;
    while (a < n && silent_edge((uint8_t)t[a]) &&
           !((t[a] == '-' || t[a] == '.') && a + 1 < n && digit(t[a + 1])))
        a++;
    bool period = false;
    while (n > a && silent_edge((uint8_t)t[n - 1])) {
        period |= t[n - 1] == '.';
        n--;
    }
    bool year_context = false;
    if (prev && prevlen > 0) {
        int pa = 0, pn = prevlen;
        while (pa < pn && !letter(prev[pa]))
            pa++;
        while (pn > pa && !letter(prev[pn - 1]))
            pn--;
        for (size_t k = 0; k < sizeof year_words / sizeof year_words[0]; k++)
            if ((int)strlen(year_words[k]) == pn - pa && starts((const char *)prev + pa, pn - pa, year_words[k]))
                year_context = true;
    }
    /* A part the host gave on its own: ".org" after "bob@example". */
    if (a > 0 && (t[a - 1] == '.' || t[a - 1] == '@') && a < n && letter(t[a]) && (a == 1 || !silent_edge((uint8_t)t[a - 2]))) {
        say_words(&s, t[a - 1] == '.' ? "dot" : "at");
    }
    const char *w = t + a;
    n -= a;
    /* A URL or an address: its dots, slashes and colons are said. */
    bool url = false, email = false, dotted_letters = n >= 3;
    for (int i = 0; i < n; i++) {
        if (i + 2 < n && w[i] == ':' && w[i + 1] == '/' && w[i + 2] == '/')
            url = true;
        if (w[i] == '@' && i > 0 && i + 1 < n) {
            url = true;
            email = memchr(w + i, '.', (size_t)(n - i)) != NULL;
        }
        if (w[i] == '.' && i > 0 && alnum(w[i - 1]) && i + 1 < n && letter(w[i + 1]))
            url = true;
        if (dotted_letters && (i % 2 ? w[i] != '.' : !letter(w[i])))
            dotted_letters = false;
    }
    if (starts(w, n, "www."))
        url = true;
    if (dotted_letters) /* "e.g.", "U.S.A.": letters */
        url = false;
    for (int i = 0; i < n && s.n < max;) {
        uint8_t ch = (uint8_t)w[i];
        bool start = !i || !alnum(w[i - 1]);
        if (digit(ch) || (start && i + 1 < n && digit(w[i + 1]) &&
                          (ch == '$' || ch == '-' || ch == '+' || ch == '.' || ch == 0xA3 || ch == 0xDB || ch == 0xB4 ||
                           ch == 0xD0))) {
            words nw = { "", 0 };
            int used = number(&nw, w + i, n - i, url, year_context);
            if (used > 0) {
                say_words(&s, nw.b);
                i += used;
                continue;
            }
        }
        if (ch == '#' && i + 1 < n && digit(w[i + 1])) { /* "#1": number one */
            say_words(&s, "number");
            i++;
            continue;
        }
        if (letter(ch)) {
            int j = i;
            while (j < n && (letter(w[j]) || (w[j] == '\'' && j + 1 < n && letter(w[j + 1]))))
                j++;
            run(&s, w + i, j - i, period && j == n);
            /* A drive letter's colon ("C:\Windows"). */
            if (j - i == 1 && j < n && w[j] == ':' && !url) {
                say_words(&s, "colon");
                j++;
            }
            i = j;
            continue;
        }
        bool between = i > 0 && i + 1 < n && alnum(w[i - 1]) && (alnum(w[i + 1]) || w[i + 1] == '/');
        const char *name = mark_name(ch, url, between);
        if (name && email && (ch == '@' || ch == '.') && s.n < max)
            s.out[s.n++] = 0; /* a whole address is read with a pause at each */
        if (name)
            say_words(&s, name);
        i++;
    }
    bb_free(t);
    return s.n;
}
