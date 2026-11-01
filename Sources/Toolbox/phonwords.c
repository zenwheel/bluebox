/*
 * Phonemes back into words.
 *
 * Programs gave MacinTalk phonemes of their own (the Talking Moose's
 * phrases are all phonemes, written by hand) and the Speech Manager takes
 * them too, as [[inpt PHON]] or the 'PHON' input mode.  Today's voices
 * read phonemes out as letters, so they are turned back into the words
 * they were meant to be, and the host says those.
 *
 * The words come from a table of the CMU Pronouncing Dictionary's
 * pronunciations for SCOWL's commoner words (phonwords_gen.c), kept as a
 * trie of sounds.  Each run of phonemes between spaces, pauses and
 * punctuation (people wrote several words without a space) is cut into
 * words by dynamic programming: each word costs a little, a rare word
 * more, and a word may differ from the phonemes written by a weighted
 * edit distance (stress and vowel reduction differ, a hand-written
 * phoneme string drops or doubles sounds), searched along the trie.
 * What no word matches is respelled by its sounds in English letters,
 * which the voice says roughly right.  Two words that sound the same are
 * the same here: which is written doesn't matter to the voice.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "phonwords.h"

static bool is_vowel(int op) { return op >= PH_AE && op <= PH_OY; }

/* ---------------------------------------------------------------------- */
/* The table */

typedef struct {
    int32_t child, sibling; /* trie: first child, next sibling (-1: none) */
    int32_t entry;          /* the commonest word that sounds like this, or -1 */
    uint8_t ph;
} node;

static node *nodes;
static int nnodes, nalloc;
static char *pool;              /* the words, each NUL-ended */
static uint32_t *word_of;       /* entry: its word in pool */
static uint8_t *level_of;       /* entry: its level */
static uint32_t *sound_of;      /* entry: its phonemes in sounds */
static uint8_t *sound_len;
static uint8_t *sounds;
static int32_t *by_word;        /* hash: word -> its first entry */
static uint32_t hash_size;
static bool loaded, failed;

static uint32_t hash_word(const char *w, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ (uint8_t)w[i]) * 16777619u;
    return h;
}

static int32_t new_node(int ph)
{
    if (nnodes == nalloc) {
        nalloc = nalloc ? nalloc * 2 : 1 << 17;
        node *n = realloc(nodes, sizeof *nodes * (size_t)nalloc);
        if (!n)
            return -1;
        nodes = n;
    }
    nodes[nnodes] = (node){ -1, -1, -1, (uint8_t)ph };
    return nnodes++;
}

static bool load(void)
{
    if (loaded || failed)
        return loaded;
    failed = true;
    int8_t code_of[128];
    memset(code_of, -1, sizeof code_of);
    for (int i = 0; phw_codes[i]; i++)
        code_of[(uint8_t)phw_codes[i]] = (int8_t)(PH_AE + i);
    size_t nwords = strlen(phw_words), nsounds = strlen(phw_pron);
    int n = phw_count;
    /* Front coding only shortens: the words and sounds fit in this. */
    pool = malloc(nwords * 8 + 64);
    sounds = malloc(nsounds * 8 + 64);
    word_of = malloc(sizeof *word_of * (size_t)n);
    level_of = malloc((size_t)n);
    sound_of = malloc(sizeof *sound_of * (size_t)n);
    sound_len = malloc((size_t)n);
    hash_size = 1;
    while (hash_size < (uint32_t)n * 2)
        hash_size <<= 1;
    by_word = malloc(sizeof *by_word * hash_size);
    if (!pool || !sounds || !word_of || !level_of || !sound_of || !sound_len || !by_word || new_node(0) < 0)
        return false;
    for (uint32_t i = 0; i < hash_size; i++)
        by_word[i] = -1;
    const char *p = phw_pron, *w = phw_words;
    uint8_t cur[64];
    char word[64];
    int curlen = 0, wordlen = 0;
    int32_t path[65] = { 0 };
    size_t pool_used = 0, sounds_used = 0;
    for (int e = 0; e < n; e++) {
        int k = *p++ - 'a';
        curlen = k;
        while (*p && !(*p >= 'a' && *p <= 'z') && curlen < 64)
            cur[curlen++] = (uint8_t)code_of[(uint8_t)*p++ & 127];
        wordlen = *w++ - 'A';
        while (*w && (*w < '0' || *w > '9') && wordlen < 63)
            word[wordlen++] = *w++;
        int level = *w ? *w++ - '0' : 4;
        word[wordlen] = 0;
        word_of[e] = (uint32_t)pool_used;
        memcpy(pool + pool_used, word, (size_t)wordlen + 1);
        pool_used += (size_t)wordlen + 1;
        level_of[e] = (uint8_t)level;
        sound_of[e] = (uint32_t)sounds_used;
        sound_len[e] = (uint8_t)curlen;
        memcpy(sounds + sounds_used, cur, (size_t)curlen);
        sounds_used += (size_t)curlen;
        /* Into the trie: the first k nodes are the last entry's. */
        for (int d = k + 1; d <= curlen; d++) {
            int32_t c = new_node(cur[d - 1]);
            if (c < 0)
                return false;
            nodes[c].sibling = nodes[path[d - 1]].child;
            nodes[path[d - 1]].child = c;
            path[d] = c;
        }
        node *at = &nodes[path[curlen]];
        if (at->entry < 0 || level_of[at->entry] % 5 > level % 5)
            at->entry = e;
        /* By the word: its first sound. */
        uint32_t h = hash_word(word, (size_t)wordlen) & (hash_size - 1);
        while (by_word[h] >= 0 && strcmp(pool + word_of[by_word[h]], word))
            h = (h + 1) & (hash_size - 1);
        if (by_word[h] < 0 || (level_of[by_word[h]] >= 5 && level < 5))
            by_word[h] = e;
    }
    loaded = true;
    failed = false;
    return true;
}

int phonwords_word_phonemes(const uint8_t *text, int len, uint8_t *out, int max)
{
    char w[64];
    int n = 0;
    for (int i = 0; i < len && n < (int)sizeof w - 1; i++) {
        int ch = text[i];
        if (ch >= 'A' && ch <= 'Z')
            ch += 32;
        if ((ch >= 'a' && ch <= 'z') || ch == '\'')
            w[n++] = (char)ch;
        else if (ch == 0xD5) /* MacRoman's right single quote */
            w[n++] = '\'';
    }
    w[n] = 0;
    if (!n || !load())
        return 0;
    uint32_t h = hash_word(w, (size_t)n) & (hash_size - 1);
    while (by_word[h] >= 0 && strcmp(pool + word_of[by_word[h]], w))
        h = (h + 1) & (hash_size - 1);
    if (by_word[h] < 0)
        return 0;
    int e = by_word[h], k = 0;
    for (int i = 0; i < sound_len[e] && k < max; i++)
        out[k++] = sounds[sound_of[e] + (uint32_t)i];
    return k;
}

/* ---------------------------------------------------------------------- */
/* How far apart two sounds are */

/* Vowels near each other: reduced to a schwa, or neighbours in the mouth
 * (and, before r, nearly all alike: the hand-written "FOHR" is "for"). */
static float vowel_sub(int a, int b)
{
    static const struct {
        uint8_t a, b;
        float cost;
    } near[] = {
        { PH_IH, PH_IX, 0.15f }, { PH_AX, PH_UX, 0.2f },  { PH_AX, PH_IX, 0.2f },  { PH_IY, PH_IH, 0.3f },
        { PH_IY, PH_IX, 0.3f },  { PH_IH, PH_EH, 0.3f },  { PH_EH, PH_AE, 0.3f },  { PH_EH, PH_EY, 0.3f },
        { PH_AA, PH_AO, 0.25f }, { PH_AA, PH_UX, 0.3f },  { PH_AO, PH_OW, 0.3f },  { PH_OW, PH_UW, 0.45f },
        { PH_UW, PH_UH, 0.3f },  { PH_UH, PH_UX, 0.3f },  { PH_AE, PH_AA, 0.4f },  { PH_AY, PH_AA, 0.5f },
        { PH_AW, PH_AA, 0.5f },  { PH_OY, PH_AO, 0.5f },  { PH_AO, PH_UX, 0.45f }, { PH_EY, PH_IY, 0.45f },
        { PH_UH, PH_AX, 0.3f },  { PH_IH, PH_AX, 0.3f },  { PH_EH, PH_AX, 0.35f }, { PH_AA, PH_AX, 0.35f },
        { PH_AE, PH_AX, 0.4f },  { PH_UW, PH_AX, 0.4f },  { PH_OW, PH_AX, 0.4f },  { PH_AO, PH_AX, 0.4f },
        { PH_UX, PH_EH, 0.45f }, { PH_UW, PH_IX, 0.35f },
    };
    for (size_t i = 0; i < sizeof near / sizeof near[0]; i++)
        if ((near[i].a == a && near[i].b == b) || (near[i].a == b && near[i].b == a))
            return near[i].cost;
    return 0.8f;
}

static float consonant_sub(int a, int b)
{
    static const struct {
        uint8_t a, b;
        float cost;
    } near[] = {
        { PH_t, PH_d, 0.3f }, { PH_p, PH_b, 0.4f }, { PH_k, PH_g, 0.4f }, { PH_f, PH_v, 0.4f },
        { PH_s, PH_z, 0.3f }, { PH_S, PH_Z, 0.4f }, { PH_T, PH_D, 0.3f }, { PH_C, PH_J, 0.4f },
        { PH_n, PH_N, 0.4f }, { PH_m, PH_n, 0.5f }, { PH_C, PH_S, 0.45f }, { PH_J, PH_Z, 0.45f },
        { PH_s, PH_S, 0.5f }, { PH_T, PH_t, 0.5f }, { PH_D, PH_d, 0.5f }, { PH_T, PH_f, 0.5f },
        { PH_w, PH_h, 0.6f }, { PH_y, PH_IY, 0.5f }, { PH_w, PH_UW, 0.5f }, { PH_z, PH_Z, 0.5f },
    };
    for (size_t i = 0; i < sizeof near / sizeof near[0]; i++)
        if ((near[i].a == a && near[i].b == b) || (near[i].a == b && near[i].b == a))
            return near[i].cost;
    return 1.0f;
}

static float sub_cost(int a, int b)
{
    if (a == b)
        return 0;
    if (is_vowel(a) && is_vowel(b))
        return vowel_sub(a, b);
    return consonant_sub(a, b);
}

/* A sound of the word's that isn't written: a schwa, h or a glide is
 * easily missed, a full vowel (a syllable) isn't. */
static float missing_cost(int op)
{
    switch (op) {
    case PH_AX: case PH_IX: return 0.3f;
    case PH_h: return 0.45f;
    case PH_r: case PH_l: case PH_w: case PH_y: return 0.6f;
    case PH_t: case PH_d: return 0.55f;
    default: return is_vowel(op) ? 0.9f : 0.8f;
    }
}

/* A sound written that the word hasn't: the same, except that a vowel
 * next to another is how people spelt a long one ("EHIY" any, "WAAAHN"). */
static float extra_cost(const uint8_t *ph, int n, int i)
{
    int op = ph[i];
    if (is_vowel(op) && ((i > 0 && is_vowel(ph[i - 1])) || (i + 1 < n && is_vowel(ph[i + 1]))))
        return 0.35f;
    if (op == PH_AX || op == PH_IX)
        return 0.5f;
    return missing_cost(op);
}

/* ---------------------------------------------------------------------- */
/* Cutting a run of phonemes into words */

#define MAX_RUN 64
#define WORD_COST 1.0f        /* each word: fewer, longer words are likelier */
#define ALT_COST 0.15f        /* a word's other pronunciation */
#define EDIT_WEIGHT 2.0f
#define MAX_EDIT 1.6f
#define EDIT_PER_SOUND 0.25f   /* a word may differ by this much a sound */
#define SPELL_COST 1.5f       /* each sound no word matched */
static const float level_cost[5] = { 0, 0.15f, 0.35f, 0.6f, 0.9f };

static const uint8_t *run;
static int run_len, run_start;
static float arc_cost[MAX_RUN + 1];
static int32_t arc_entry[MAX_RUN + 1];
static float arc_edit[MAX_RUN + 1];
static float rows[MAX_RUN + 2][MAX_RUN + 1];
static float extra[MAX_RUN]; /* each sound's extra_cost */

/* Along the trie from node at depth d (rows[d] its edit distances to the
 * run from run_start), the best word for each end. */
static void walk(int32_t at, int d)
{
    int L = run_len - run_start;
    const float *row = rows[d];
    for (int32_t c = nodes[at].child; c >= 0; c = nodes[c].sibling) {
        if (d + 1 > MAX_RUN)
            break;
        int q = nodes[c].ph;
        float *nr = rows[d + 1];
        float gq = missing_cost(q);
        nr[0] = row[0] + gq;
        float least = nr[0];
        for (int k = 1; k <= L; k++) {
            int pk = run[run_start + k - 1];
            float v = row[k - 1] + sub_cost(pk, q);
            float del = row[k] + gq;
            float ins = nr[k - 1] + extra[run_start + k - 1];
            if (del < v)
                v = del;
            if (ins < v)
                v = ins;
            nr[k] = v;
            if (v < least)
                least = v;
        }
        if (least > MAX_EDIT)
            continue;
        int32_t e = nodes[c].entry;
        if (e >= 0) {
            int lv = level_of[e];
            float base = WORD_COST + level_cost[lv % 5] + (lv >= 5 ? ALT_COST : 0);
            for (int k = 1; k <= L; k++) {
                float edit = nr[k];
                int longer = k > d + 1 ? k : d + 1;
                if (edit > EDIT_PER_SOUND * (float)longer + 0.001f || edit > MAX_EDIT)
                    continue;
                float cost = base + EDIT_WEIGHT * edit;
                if (cost < arc_cost[k]) {
                    arc_cost[k] = cost;
                    arc_entry[k] = e;
                    arc_edit[k] = edit;
                }
            }
        }
        walk(c, d + 1);
    }
}

/* Each sound in English letters, the vowels by whether a consonant
 * closes them (cat, but, hot; kah, buh, hah). */
static const char *spelling(int op, bool closed, bool first)
{
    switch (op) {
    case PH_AE: return closed ? "a" : "aa";
    case PH_EY: return "ay";
    case PH_AO: return "aw";
    case PH_AX: return closed ? "u" : "uh";
    case PH_IY: return "ee";
    case PH_EH: return closed ? "e" : "eh";
    case PH_IH: return closed ? "i" : "ih";
    case PH_AY: return first ? "eye" : "igh";
    case PH_IX: return closed ? "i" : "ih";
    case PH_AA: return closed ? "o" : "ah";
    case PH_UW: return "oo";
    case PH_UH: return closed ? "oo" : "ooh";
    case PH_UX: return closed ? "u" : "uh";
    case PH_OW: return "oh";
    case PH_AW: return "ow";
    case PH_OY: return "oy";
    case PH_b: return "b";
    case PH_C: return "ch";
    case PH_d: return "d";
    case PH_D: return "th";
    case PH_f: return "f";
    case PH_g: return "g";
    case PH_h: return "h";
    case PH_J: return "j";
    case PH_k: return "k";
    case PH_l: return "l";
    case PH_m: return "m";
    case PH_n: return "n";
    case PH_N: return "ng";
    case PH_p: return "p";
    case PH_r: return "r";
    case PH_s: return "s";
    case PH_S: return "sh";
    case PH_t: return "t";
    case PH_T: return "th";
    case PH_v: return "v";
    case PH_w: return "w";
    case PH_y: return "y";
    case PH_z: return "z";
    case PH_Z: return "zh";
    default: return "";
    }
}

/* ---------------------------------------------------------------------- */
/* The text */

typedef struct {
    char *out;
    size_t k, size;
    uint32_t *from, *to;
} text_out;

static void put(text_out *t, const char *s, size_t n, uint32_t from, uint32_t to)
{
    for (size_t i = 0; i < n && t->k + 1 < t->size; i++) {
        if (t->from)
            t->from[t->k] = from;
        if (t->to)
            t->to[t->k] = to;
        t->out[t->k++] = s[i];
    }
}

static void put_space(text_out *t, uint32_t at)
{
    if (t->k && t->out[t->k - 1] != ' ')
        put(t, " ", 1, at, at + 1);
}

static int stat_exact, stat_near, stat_spelled;

void phonemes_to_text_stats(int *exact, int *near, int *spelled)
{
    *exact = stat_exact;
    *near = stat_near;
    *spelled = stat_spelled;
}

/* A run of phonemes (where[i]..where[i+1] in the input for each) as words. */
static void say_run(text_out *t, const uint8_t *ph, const uint32_t *where, int n, bool emphatic)
{
    if (!n)
        return;
    float best[MAX_RUN + 1];
    int back[MAX_RUN + 1];
    int32_t back_entry[MAX_RUN + 1];
    float back_edit[MAX_RUN + 1];
    for (int j = 0; j <= n; j++)
        best[j] = 1e30f;
    best[0] = 0;
    bool have = load();
    run = ph;
    run_len = n;
    for (int i = 0; i < n; i++)
        extra[i] = extra_cost(ph, n, i);
    for (int i = 0; i < n; i++) {
        if (best[i] >= 1e30f)
            continue;
        for (int k = 0; k <= n - i; k++) {
            arc_cost[k] = 1e30f;
            arc_entry[k] = -1;
        }
        if (have) {
            run_start = i;
            for (int k = 0; k <= n - i; k++)
                rows[0][k] = k ? rows[0][k - 1] + extra[i + k - 1] : 0;
            walk(0, 0);
        }
        if (SPELL_COST < arc_cost[1]) { /* the sound spelt */
            arc_cost[1] = SPELL_COST;
            arc_entry[1] = -1;
        }
        for (int k = 1; k <= n - i; k++) {
            float c = best[i] + arc_cost[k];
            if (c < best[i + k]) {
                best[i + k] = c;
                back[i + k] = i;
                back_entry[i + k] = arc_entry[k];
                back_edit[i + k] = arc_edit[k];
            }
        }
    }
    /* The words, first to last. */
    int ends[MAX_RUN + 1], nw = 0;
    for (int j = n; j > 0; j = back[j])
        ends[nw++] = j;
    /* Sounds spelt with no vowel would be read as letters ("z", "k"): they
     * are spelt with the word beside them. */
    for (int w = nw - 1; w >= 0; w--) {
        int j = ends[w], i = back[j];
        if (back_entry[j] >= 0)
            continue;
        bool vowel = false;
        for (int x = i; x < j; x++)
            vowel |= is_vowel(ph[x]);
        if (vowel)
            continue;
        if (w + 1 < nw)
            back_entry[ends[w + 1]] = -1;
        else if (w > 0)
            back_entry[ends[w - 1]] = -1;
    }
    if (emphatic)
        put(t, "[[emph +]]", 10, where[0], where[0]);
    for (int w = nw - 1; w >= 0; w--) {
        int j = ends[w], i = back[j];
        if (back_entry[j] >= 0) {
            const char *s = pool + word_of[back_entry[j]];
            put_space(t, where[i]);
            if (s[0] == 'i' && (!s[1] || s[1] == '\'')) { /* I, I'm: a capital, or it is the letter */
                put(t, "I", 1, where[i], where[j]);
                s++;
            }
            put(t, s, strlen(s), where[i], where[j]);
            if (back_edit[j] > 0)
                stat_near++;
            else
                stat_exact++;
            continue;
        }
        /* Sounds no word has, together, spelt. */
        int start = i;
        while (w > 0 && back_entry[ends[w - 1]] < 0)
            j = ends[--w];
        put_space(t, where[start]);
        for (int x = start; x < j; x++) {
            bool closed = x + 1 < j && !is_vowel(ph[x + 1]);
            const char *s = spelling(ph[x], closed, x == start);
            put(t, s, strlen(s), where[start], where[j]);
        }
        stat_spelled++;
    }
}

/* The Speech Manager's phoneme symbols, two letters first. */
static int symbol_at(const uint8_t *in, size_t n, size_t i, int *len)
{
    static const char *const two[] = { "AE", "EY", "AO", "AX", "IY", "EH", "IH", "AY",
                                       "IX", "AA", "UW", "UH", "UX", "OW", "AW", "OY" };
    static const char one[] = "bCdDfghJklmnNprsStTvwyzZ";
    static const uint8_t one_op[] = { PH_b, PH_C, PH_d, PH_D, PH_f, PH_g, PH_h, PH_J, PH_k, PH_l, PH_m, PH_n,
                                      PH_N, PH_p, PH_r, PH_s, PH_S, PH_t, PH_T, PH_v, PH_w, PH_y, PH_z, PH_Z };
    if (i + 1 < n)
        for (int k = 0; k < 16; k++)
            if (in[i] == (uint8_t)two[k][0] && in[i + 1] == (uint8_t)two[k][1]) {
                *len = 2;
                return PH_AE + k;
            }
    const char *p = memchr(one, in[i], sizeof one - 1);
    if (p && in[i]) {
        *len = 1;
        return one_op[p - one];
    }
    *len = 1;
    if (in[i] == '%')
        return PH_SILENCE;
    if (in[i] == '@')
        return PH_BREATH;
    return -1;
}

size_t phonemes_to_text(const uint8_t *in, size_t n, char *out, size_t outsize, uint32_t *from, uint32_t *to)
{
    text_out t = { out, 0, outsize, from, to };
    stat_exact = stat_near = stat_spelled = 0;
    if (!outsize)
        return 0;
    uint8_t ph[MAX_RUN];
    uint32_t where[MAX_RUN + 1];
    int np = 0;
    bool emphatic = false;
    int silences = 0;
    bool tune = memchr(in, '{', n) != NULL;
    size_t i = 0;
    while (i <= n) {
        if (i < n && in[i] == '{') { /* TUNE's durations and pitches */
            while (i < n && in[i] != '}')
                i++;
            i += i < n;
            continue;
        }
        int len = 1;
        int op = i < n ? symbol_at(in, n, i, &len) : -1;
        if (op > PH_BREATH && np < MAX_RUN) {
            /* The same sound twice is one ("LLUH9K", "SLIY6IY5IY4IY3IY1PIY"). */
            if (!np || ph[np - 1] != op) {
                where[np] = (uint32_t)i;
                ph[np++] = (uint8_t)op;
            }
            where[np] = (uint32_t)(i + (size_t)len);
            i += (size_t)len;
            continue;
        }
        /* Stress, pitch and length marks are within words; so are TUNE's
         * line ends (it gives one phoneme a line, words marked by _). */
        if (op < 0 && i < n && in[i] && (strchr("12=/\\<>#", in[i]) || (tune && strchr(" \t\r\n", in[i])))) {
            i++;
            continue;
        }
        if (op > PH_BREATH) { /* a run too long for one go */
            say_run(&t, ph, where, np, emphatic);
            np = 0;
            emphatic = false;
            continue;
        }
        /* The end of a run: a space, a pause, punctuation, a mark. */
        say_run(&t, ph, where, np, emphatic);
        if (np)
            emphatic = false;
        np = 0;
        if (i == n)
            break;
        uint8_t ch = in[i];
        if (op == PH_SILENCE || op == PH_BREATH) {
            while (t.k && t.out[t.k - 1] == ' ')
                t.k--;
            if (++silences == 2 && t.k && !strchr(".,?!;:", t.out[t.k - 1])) /* a pause */
                put(&t, ",", 1, (uint32_t)i, (uint32_t)i + 1);
        } else {
            silences = 0;
        }
        if (ch == '.' || ch == ',' || ch == '?' || ch == '!' || ch == ';' || ch == ':') {
            if (t.k && t.out[t.k - 1] == ' ')
                t.k--;
            put(&t, (const char *)&ch, 1, (uint32_t)i, (uint32_t)i + 1);
        } else if (ch == ' ' || ch == '-' || ch == '\r' || ch == '\n' || ch == '\t' || op >= 0) {
            put_space(&t, (uint32_t)i);
        }
        if (ch == '+') /* emphatic: the next word */
            emphatic = true;
        else if (ch == '_' || ch == '~')
            emphatic = false;
        /* _ ~ + (prominence) start a word. */
        if (ch == '_' || ch == '~' || ch == '+')
            put_space(&t, (uint32_t)i);
        i++;
    }
    while (t.k && t.out[t.k - 1] == ' ')
        t.k--;
    t.out[t.k] = 0;
    return t.k;
}
