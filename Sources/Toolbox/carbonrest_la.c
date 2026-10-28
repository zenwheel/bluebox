/*
 * The Language Analysis Manager (LanguageAnalysis.h): splitting Japanese
 * text into morphemes.  Apple's engine used dictionaries we do not have;
 * the one environment here, "Japanese", segments with the host's own
 * dictionary-based word breaker (CFStringTokenizer) and takes words from
 * the dictionaries opened in it (Dictionary Manager files with yomi and
 * hyoki fields): such a word is one morpheme with its part of speech and a
 * homograph record naming the dictionary entry.  The parts of speech of
 * the rest are told from their characters alone: punctuation and symbols,
 * numbers, and nouns (kLASpeechMeishi) for words.
 *
 * Text is UTF-16.  Results are AERecords as LanguageAnalysis.h names them: a
 * bundle holds a list of paths under 'lmfp', a path a list of morphemes
 * under 'lmfn', a morpheme a list of homographs, each a record of the text
 * range ('lamt': offset and length in bytes), part of speech ('lamc') and,
 * for a dictionary word, the entry ('lahd') and its weight ('lahw').
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "carbonrest.h"
#include "hostuc.h"

int host_text_decode(uint32_t enc, const uint8_t *in, size_t n, size_t *used, uint16_t *out, size_t cap, size_t *produced);
int host_text_encode(uint32_t enc, const uint16_t *in, size_t n, size_t *used, uint8_t *out, size_t cap, size_t *produced);
uint32_t rest_dcm_id_for_file(gaddr spec, int16_t *err);
bool rest_dcm_has_word_fields(uint32_t id);
void rest_dcm_words(uint32_t id, void (*fn)(void *ctx, uint32_t uid, const uint8_t *hyoki, uint32_t hlen,
                                             uint32_t pos, uint16_t weight), void *ctx);

enum {
    laTooSmallBufferErr = -6984,
    laEnvironmentBusyErr = -6985,
    laEnvironmentNotFoundErr = -6986,
    laEnvironmentExistErr = -6987,
    laInvalidPathErr = -6988,
    laNoMoreMorphemeErr = -6989,
    laFailAnalysisErr = -6990,
    laDictionaryNotOpenedErr = -6992,
    laDictionaryUnknownErr = -6993,
    laDictionaryTooManyErr = -6994,
};
#define T(s) fourcc(s)
enum { ENV_TAG = 0x4C450000, CTX_TAG = 0x4C580000, MAX_ENVS = 16, MAX_CTXS = 16, MAX_DICTS = 16 };
enum {
    kLASpeechMeishi = 0, kLASpeechSuushi = 0xA200, kLASpeechKigou = 0xA100, kLASpeechKuten = 0xA110,
    kLASpeechTouten = 0xA120, kLASpeechDokuritsugo = 0xA300, kLASpeechJoshi = 0xC000,
};
enum { kLAEndOfSourceTextMask = 1 };

typedef struct {
    bool used, builtin;
    uint8_t name[64]; /* Pascal */
    uint32_t dicts[MAX_DICTS];
    uint8_t specs[MAX_DICTS][70];
    int ndicts;
    int contexts;
} environment;

typedef struct {
    uint32_t start, len; /* UTF-16 units */
    uint32_t pos;
    uint32_t dict, uid;
    uint16_t weight;
} morpheme;

typedef struct {
    bool used;
    int env;
    uint16_t *text;
    uint32_t n;
    morpheme *m;
    int nm;
} context;

static environment envs[MAX_ENVS];
static context ctxs[MAX_CTXS];

static void free_context(context *c)
{
    free(c->text);
    free(c->m);
    memset(c, 0, sizeof *c);
}

static void ensure_builtin(void)
{
    if (envs[0].used || !hu_available())
        return;
    envs[0].used = envs[0].builtin = true;
    const char *name = "Japanese";
    envs[0].name[0] = (uint8_t)strlen(name);
    memcpy(envs[0].name + 1, name, strlen(name));
}

void rest_la_reset(void)
{
    for (int i = 0; i < MAX_CTXS; i++)
        free_context(&ctxs[i]);
    memset(envs, 0, sizeof envs);
}

static environment *env_of(uint32_t ref)
{
    ensure_builtin();
    uint32_t i = ref - ENV_TAG;
    return i >= 1 && i <= MAX_ENVS && envs[i - 1].used ? &envs[i - 1] : NULL;
}

static context *ctx_of(uint32_t ref)
{
    uint32_t i = ref - CTX_TAG;
    return i >= 1 && i <= MAX_CTXS && ctxs[i - 1].used ? &ctxs[i - 1] : NULL;
}

static void la_LALibraryVersion(ppc_cpu *c) { RET(0x01000000); }

/* ---- Environments ---- */

static void la_LAGetEnvironmentList(ppc_cpu *c)
{
    ensure_builtin();
    uint32_t k = 0, total = 0;
    for (int i = 0; i < MAX_ENVS; i++)
        if (envs[i].used) {
            total++;
            if (A(2) && k < A(0))
                wr32(A(2) + 4 * k++, ENV_TAG + (uint32_t)i + 1);
        }
    if (A(1))
        wr32(A(1), A(2) ? k : total);
    RET(noErr);
}

static void la_LAGetEnvironmentName(ppc_cpu *c)
{
    environment *e = env_of(A(0));
    if (!e || !A(1))
        RET(e ? rest_paramErr : laEnvironmentNotFoundErr);
    gput(A(1), e->name, (uint32_t)e->name[0] + 1);
    RET(noErr);
}

static bool name_equal(const environment *e, gaddr name)
{
    if (rd8(name) != e->name[0])
        return false;
    for (uint32_t i = 1; i <= e->name[0]; i++)
        if (rd8(name + i) != e->name[i])
            return false;
    return true;
}

static void la_LAGetEnvironmentRef(ppc_cpu *c)
{
    ensure_builtin();
    if (!A(0) || !A(1))
        RET(rest_paramErr);
    for (int i = 0; i < MAX_ENVS; i++)
        if (envs[i].used && name_equal(&envs[i], A(0))) {
            wr32(A(1), ENV_TAG + (uint32_t)i + 1);
            RET(noErr);
        }
    wr32(A(1), 0);
    RET(laEnvironmentNotFoundErr);
}

/* LACreateCustomEnvironment(base, newName, persistent, &new) */
static void la_LACreateCustomEnvironment(ppc_cpu *c)
{
    environment *base = env_of(A(0));
    if (!base)
        RET(laEnvironmentNotFoundErr);
    if (!A(1) || !A(3) || !rd8(A(1)) || rd8(A(1)) > 31)
        RET(rest_paramErr);
    for (int i = 0; i < MAX_ENVS; i++)
        if (envs[i].used && name_equal(&envs[i], A(1)))
            RET(laEnvironmentExistErr);
    for (int i = 0; i < MAX_ENVS; i++)
        if (!envs[i].used) {
            envs[i] = *base;
            envs[i].builtin = false;
            envs[i].contexts = 0;
            gget(envs[i].name, A(1), (uint32_t)rd8(A(1)) + 1);
            wr32(A(3), ENV_TAG + (uint32_t)i + 1);
            RET(noErr);
        }
    RET(rest_memFullErr);
}

static void la_LADeleteCustomEnvironment(ppc_cpu *c)
{
    environment *e = env_of(A(0));
    if (!e)
        RET(laEnvironmentNotFoundErr);
    if (e->builtin || e->contexts)
        RET(laEnvironmentBusyErr);
    memset(e, 0, sizeof *e);
    RET(noErr);
}

/* ---- Dictionaries ---- */

static int dict_index(environment *e, gaddr spec)
{
    uint8_t s[70];
    gget(s, spec, 70);
    for (int i = 0; i < e->ndicts; i++)
        if (!memcmp(e->specs[i], s, 70))
            return i;
    return -1;
}

static void la_LAOpenDictionary(ppc_cpu *c)
{
    environment *e = env_of(A(0));
    if (!e)
        RET(laEnvironmentNotFoundErr);
    if (!A(1))
        RET(rest_paramErr);
    if (dict_index(e, A(1)) >= 0)
        RET(noErr);
    int16_t err;
    uint32_t id = rest_dcm_id_for_file(A(1), &err);
    if (!id || !rest_dcm_has_word_fields(id))
        RET(laDictionaryUnknownErr);
    if (e->ndicts == MAX_DICTS)
        RET(laDictionaryTooManyErr);
    e->dicts[e->ndicts] = id;
    gget(e->specs[e->ndicts], A(1), 70);
    e->ndicts++;
    RET(noErr);
}

static void la_LACloseDictionary(ppc_cpu *c)
{
    environment *e = env_of(A(0));
    if (!e)
        RET(laEnvironmentNotFoundErr);
    if (!A(1))
        RET(rest_paramErr);
    int i = dict_index(e, A(1));
    if (i < 0)
        RET(laDictionaryNotOpenedErr);
    memmove(&e->dicts[i], &e->dicts[i + 1], (size_t)(e->ndicts - i - 1) * sizeof e->dicts[0]);
    memmove(&e->specs[i], &e->specs[i + 1], (size_t)(e->ndicts - i - 1) * sizeof e->specs[0]);
    e->ndicts--;
    RET(noErr);
}

/* LAListAvailableDictionaries(env, maxCount, &actualCount, dictionaryList, opened): the
 * dictionaries the Dictionary Manager lists that have the fields, and whether each is open here. */
static void la_LAListAvailableDictionaries(ppc_cpu *c)
{
    environment *e = env_of(A(0));
    if (!e)
        RET(laEnvironmentNotFoundErr);
    gaddr scratch = tb_NewPtr(16 + 70, false, true);
    if (!scratch)
        RET(rest_memFullErr);
    uint32_t it[1] = { scratch };
    uint32_t k = 0, total = 0;
    if (svc_call_export("DCMCreateDictionaryIterator", it, 1) == 0) {
        uint32_t iter = rd32(scratch);
        for (;;) {
            uint32_t a[2] = { iter, scratch + 4 };
            if (svc_call_export("DCMIterateObject", a, 2) != 0)
                break;
            uint32_t id = rd32(scratch + 4);
            if (!rest_dcm_has_word_fields(id))
                continue;
            uint32_t g[2] = { id, scratch + 16 };
            if (svc_call_export("DCMGetFileFromDictionaryID", g, 2) != 0)
                continue;
            total++;
            if (A(3) && k < A(1)) {
                for (uint32_t b = 0; b < 70; b++)
                    wr8(A(3) + 70 * k + b, rd8(scratch + 16 + b));
                if (A(4))
                    wr8(A(4) + k, dict_index(e, scratch + 16) >= 0);
                k++;
            }
        }
        uint32_t d[1] = { iter };
        svc_call_export("DCMDisposeObjectIterator", d, 1);
    }
    tb_DisposePtr(scratch);
    if (A(2))
        wr32(A(2), A(3) ? k : total);
    RET(noErr);
}

/* LAAddNewWord(env, dictionary, dataList): a record in the dictionary under its yomi field */
static void la_LAAddNewWord(ppc_cpu *c)
{
    environment *e = env_of(A(0));
    if (!e)
        RET(laEnvironmentNotFoundErr);
    if (!A(1) || !A(2))
        RET(rest_paramErr);
    if (dict_index(e, A(1)) < 0)
        RET(laDictionaryNotOpenedErr);
    int16_t err;
    uint32_t id = rest_dcm_id_for_file(A(1), &err);
    if (!id)
        RET(laDictionaryUnknownErr);
    gaddr scratch = tb_NewPtr(32, false, true), yomi = tb_NewPtr(8, false, true);
    int32_t result = noErr;
    if (!scratch || !yomi || tb_AEGetParamDesc(A(2), (int32_t)T("yomi"), (int32_t)T("****"), yomi) != noErr) {
        result = rest_paramErr;
    } else {
        gaddr h = rd32(yomi + 4);
        uint32_t n = h ? (uint32_t)tb_GetHandleSize(h) : 0;
        uint32_t o[4] = { id, 0, 0, scratch };
        result = (int32_t)svc_call_export("DCMOpenDictionary", o, 4);
        if (!result) {
            uint32_t ref = rd32(scratch);
            uint32_t w[2] = { ref, 0 };
            result = (int32_t)svc_call_export("DCMGetDictionaryWriteAccess", w, 2);
            if (!result) {
                uint32_t add[8] = { ref, T("yomi"), n, h ? rd32(h) : 0, 0, A(2), scratch + 8 };
                result = (int32_t)svc_call_export("DCMAddRecord", add, 7);
                uint32_t rel[2] = { ref, result == 0 };
                svc_call_export("DCMReleaseDictionaryWriteAccess", rel, 2);
            }
            uint32_t cl[1] = { ref };
            svc_call_export("DCMCloseDictionary", cl, 1);
        }
    }
    if (yomi) {
        tb_AEDisposeDesc(yomi);
        tb_DisposePtr(yomi);
    }
    if (scratch)
        tb_DisposePtr(scratch);
    RET(result);
}

/* ---- Contexts ---- */

static void la_LAOpenAnalysisContext(ppc_cpu *c)
{
    environment *e = env_of(A(0));
    if (!e)
        RET(laEnvironmentNotFoundErr);
    if (!A(1))
        RET(rest_paramErr);
    for (int i = 0; i < MAX_CTXS; i++)
        if (!ctxs[i].used) {
            memset(&ctxs[i], 0, sizeof ctxs[i]);
            ctxs[i].used = true;
            ctxs[i].env = (int)(e - envs);
            e->contexts++;
            wr32(A(1), CTX_TAG + (uint32_t)i + 1);
            RET(noErr);
        }
    RET(rest_memFullErr);
}

static void la_LACloseAnalysisContext(ppc_cpu *c)
{
    context *x = ctx_of(A(0));
    if (!x)
        RET(rest_paramErr);
    if (envs[x->env].contexts)
        envs[x->env].contexts--;
    free_context(x);
    RET(noErr);
}

/* ---- Analysis ---- */

typedef struct {
    const uint16_t *text;
    uint32_t n;
    morpheme *m;
    int nm, cap;
    uint32_t dict;
} scan;

static void add_morpheme(scan *s, uint32_t start, uint32_t len, uint32_t pos, uint32_t dict, uint32_t uid, uint16_t w)
{
    if (s->nm == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 32;
        s->m = realloc(s->m, (size_t)s->cap * sizeof *s->m);
    }
    s->m[s->nm++] = (morpheme){ start, len, pos, dict, uid, w };
}

/* The part of speech a word's characters tell. */
static uint32_t classify(const uint16_t *t, uint32_t len)
{
    bool digits = true, punct = true, latin = true;
    for (uint32_t i = 0; i < len; i++) {
        uint16_t ch = t[i];
        bool d = (ch >= '0' && ch <= '9') || (ch >= 0xFF10 && ch <= 0xFF19) || (ch >= 0x3007 && ch <= 0x3007) ||
                 ch == 0x4E00 || ch == 0x4E8C || ch == 0x4E09;
        bool p = ch < 0x30 || (ch >= 0x3A && ch <= 0x40) || (ch >= 0x5B && ch <= 0x60) || (ch >= 0x7B && ch <= 0x7F) ||
                 (ch >= 0x3000 && ch <= 0x303F && ch != 0x3005 && ch != 0x3007) || (ch >= 0xFF01 && ch <= 0xFF0F) ||
                 (ch >= 0xFF1A && ch <= 0xFF20) || (ch >= 0x2010 && ch <= 0x2027);
        bool l = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= 0xFF21 && ch <= 0xFF5A);
        digits = digits && d;
        punct = punct && p;
        latin = latin && l;
    }
    if (punct) {
        if (len == 1 && t[0] == 0x3002)
            return kLASpeechKuten;
        if (len == 1 && (t[0] == 0x3001 || t[0] == ','))
            return kLASpeechTouten;
        return kLASpeechKigou;
    }
    if (digits)
        return kLASpeechSuushi;
    if (latin)
        return kLASpeechDokuritsugo;
    return kLASpeechMeishi;
}

typedef struct {
    scan *s;
    uint32_t dict;
    /* the words of the dictionaries, for matching */
    struct word {
        uint16_t *u;
        uint32_t n, pos, uid;
        uint16_t weight;
        uint32_t dict;
    } *w;
    int nw, cap;
} words;

static void collect(void *ctx, uint32_t uid, const uint8_t *h, uint32_t hl, uint32_t pos, uint16_t weight)
{
    words *ws = ctx;
    if (ws->nw == ws->cap) {
        ws->cap = ws->cap ? ws->cap * 2 : 64;
        ws->w = realloc(ws->w, (size_t)ws->cap * sizeof *ws->w);
    }
    uint32_t n = hl / 2;
    uint16_t *u = malloc((n ? n : 1) * 2);
    for (uint32_t i = 0; i < n; i++)
        u[i] = (uint16_t)(h[2 * i] << 8 | h[2 * i + 1]);
    ws->w[ws->nw++] = (struct word){ u, n, pos, uid, weight, ws->dict };
}

/* The morphemes of a text: dictionary words (the longest at each place) and, between them, the host's words. */
static int analyze(environment *e, const uint16_t *text, uint32_t n, morpheme **out)
{
    words ws = { 0 };
    scan s = { text, n, NULL, 0, 0, 0 };
    for (int d = 0; d < e->ndicts; d++) {
        ws.dict = e->dicts[d];
        rest_dcm_words(e->dicts[d], collect, &ws);
    }
    uint32_t at = 0, gap = 0;
    uint32_t *st = malloc(((size_t)n + 2) * 4), *ln = malloc(((size_t)n + 2) * 4);
    while (at < n) {
        int best = -1;
        for (int i = 0; i < ws.nw; i++)
            if (ws.w[i].n && at + ws.w[i].n <= n && !memcmp(text + at, ws.w[i].u, ws.w[i].n * 2) &&
                (best < 0 || ws.w[i].n > ws.w[best].n))
                best = i;
        if (best >= 0) {
            if (at > gap) { /* the host breaks what lies before */
                size_t k = hu_tokens(text + gap, at - gap, "ja", st, ln, n + 1);
                for (size_t j = 0; j < k; j++)
                    add_morpheme(&s, gap + st[j], ln[j], classify(text + gap + st[j], ln[j]), 0, 0, 0);
            }
            add_morpheme(&s, at, ws.w[best].n, ws.w[best].pos, ws.w[best].dict, ws.w[best].uid, ws.w[best].weight);
            at += ws.w[best].n;
            gap = at;
        } else {
            at++;
        }
    }
    if (n > gap) {
        size_t k = hu_tokens(text + gap, n - gap, "ja", st, ln, n + 1);
        for (size_t j = 0; j < k; j++)
            add_morpheme(&s, gap + st[j], ln[j], classify(text + gap + st[j], ln[j]), 0, 0, 0);
    }
    for (int i = 0; i < ws.nw; i++)
        free(ws.w[i].u);
    free(ws.w);
    free(st);
    free(ln);
    *out = s.m;
    return s.nm;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

static int16_t put_param(gaddr rec, uint32_t key, uint32_t type, const uint8_t *data, uint32_t len)
{
    gaddr p = tb_NewPtr((int32_t)len, false, true);
    if (!p)
        return memFullErr;
    gput(p, data, len);
    int16_t e = tb_AEPutParamPtr(rec, (int32_t)key, (int32_t)type, p, (int32_t)len);
    tb_DisposePtr(p);
    return e;
}

/* A morpheme as a list of one homograph record. */
static int16_t build_morpheme(const morpheme *m, gaddr out)
{
    int16_t e = tb_AECreateList(0, 0, 0, out);
    gaddr h = tb_NewPtr(8, false, true);
    if (!h)
        return memFullErr;
    if (!e)
        e = tb_AECreateList(0, 0, 1, h);
    uint8_t b[8];
    if (!e) {
        put32(b, m->start * 2);
        put32(b + 4, m->len * 2);
        e = put_param(h, T("lamt"), T("lamt"), b, 8);
    }
    if (!e) {
        put32(b, m->pos);
        e = put_param(h, T("lamc"), T("lamc"), b, 4);
    }
    if (!e && m->dict) {
        put32(b, m->dict);
        put32(b + 4, m->uid);
        e = put_param(h, T("lahd"), T("lahd"), b, 8);
        if (!e) {
            b[0] = (uint8_t)(m->weight >> 8);
            b[1] = (uint8_t)m->weight;
            e = put_param(h, T("lahw"), T("shor"), b, 2);
        }
    }
    if (!e)
        e = tb_AEPutDesc(out, 0, h);
    tb_AEDisposeDesc(h);
    tb_DisposePtr(h);
    return e;
}

/* A path of morphemes [from, from + count): an AERecord with the list under 'lmfn'. */
static int16_t build_path(const morpheme *m, int count, gaddr out)
{
    int16_t e = tb_AECreateList(0, 0, 1, out);
    gaddr list = tb_NewPtr(8, false, true), item = tb_NewPtr(8, false, true);
    if (!list || !item)
        e = memFullErr;
    if (!e)
        e = tb_AECreateList(0, 0, 0, list);
    for (int i = 0; !e && i < count; i++) {
        e = build_morpheme(&m[i], item);
        if (!e) {
            e = tb_AEPutDesc(list, 0, item);
            tb_AEDisposeDesc(item);
        }
    }
    if (!e)
        e = tb_AEPutParamDesc(out, (int32_t)T("lmfn"), list);
    if (list) {
        tb_AEDisposeDesc(list);
        tb_DisposePtr(list);
    }
    if (item)
        tb_DisposePtr(item);
    if (e)
        tb_AEDisposeDesc(out);
    return e;
}

/* LAMorphemeAnalysis(context, text, textLength, leadingPath, trailingPath, pathCount, &result): a bundle */
static void la_LAMorphemeAnalysis(ppc_cpu *c)
{
    context *x = ctx_of(A(0));
    if (!x || !A(6))
        RET(x ? rest_paramErr : laInvalidPathErr);
    if (!A(1) && A(2))
        RET(rest_paramErr);
    if (!hu_available())
        RET(laFailAnalysisErr);
    uint32_t n = A(2);
    uint16_t *t = malloc(((size_t)n + 1) * 2);
    for (uint32_t i = 0; i < n; i++)
        t[i] = rd16(A(1) + 2 * i);
    morpheme *m;
    int nm = analyze(&envs[x->env], t, n, &m);
    free(t);
    gaddr path = tb_NewPtr(8, false, true), paths = tb_NewPtr(8, false, true);
    int16_t e = path && paths ? tb_AECreateList(0, 0, 0, paths) : memFullErr;
    if (!e)
        e = build_path(m, nm, path);
    if (!e) {
        e = tb_AEPutDesc(paths, 0, path);
        tb_AEDisposeDesc(path);
    }
    if (!e)
        e = tb_AECreateList(0, 0, 1, A(6));
    if (!e)
        e = tb_AEPutParamDesc(A(6), (int32_t)T("lmfp"), paths);
    if (paths) {
        tb_AEDisposeDesc(paths);
        tb_DisposePtr(paths);
    }
    if (path)
        tb_DisposePtr(path);
    free(m);
    RET((int32_t)e);
}

/* LAContinuousMorphemeAnalysis(context, text, length, incrementalText, leadingPath, trailingPath, &modified) */
static void la_LAContinuousMorphemeAnalysis(ppc_cpu *c)
{
    context *x = ctx_of(A(0));
    if (!x)
        RET(laInvalidPathErr);
    if (!A(1) && A(2))
        RET(rest_paramErr);
    if (!hu_available())
        RET(laFailAnalysisErr);
    uint32_t add = A(2), keep = BOOL(3) ? x->n : 0;
    uint16_t *t = malloc(((size_t)keep + add + 1) * 2);
    memcpy(t, x->text, (size_t)keep * 2);
    for (uint32_t i = 0; i < add; i++)
        t[keep + i] = rd16(A(1) + 2 * i);
    free(x->text);
    x->text = t;
    x->n = keep + add;
    morpheme *old = x->m;
    int oldn = x->nm;
    x->nm = analyze(&envs[x->env], x->text, x->n, &x->m);
    bool changed = oldn != x->nm;
    for (int i = 0; !changed && i < oldn; i++)
        changed = old[i].start != x->m[i].start || old[i].len != x->m[i].len || old[i].pos != x->m[i].pos;
    free(old);
    if (A(6))
        wr8(A(6), changed);
    RET(noErr);
}

/* LAGetMorphemes(context, &result): the path of what is analysed so far */
static void la_LAGetMorphemes(ppc_cpu *c)
{
    context *x = ctx_of(A(0));
    if (!x || !A(1))
        RET(x ? rest_paramErr : laInvalidPathErr);
    if (!x->nm)
        RET(laNoMoreMorphemeErr);
    RET((int32_t)build_path(x->m, x->nm, A(1)));
}

/* LAShiftMorphemes(context, morphemeCount, &path, &shiftedLength): the first morphemes taken off */
static void la_LAShiftMorphemes(ppc_cpu *c)
{
    context *x = ctx_of(A(0));
    if (!x)
        RET(laInvalidPathErr);
    int k = A(1) == 0 || A(1) > (uint32_t)x->nm ? x->nm : (int)A(1);
    if (!k)
        RET(laNoMoreMorphemeErr);
    int16_t e = noErr;
    if (A(2))
        e = build_path(x->m, k, A(2));
    if (e)
        RET((int32_t)e);
    uint32_t cut = x->m[k - 1].start + x->m[k - 1].len;
    memmove(x->text, x->text + cut, (size_t)(x->n - cut) * 2);
    x->n -= cut;
    memmove(x->m, x->m + k, (size_t)(x->nm - k) * sizeof *x->m);
    x->nm -= k;
    for (int i = 0; i < x->nm; i++)
        x->m[i].start -= cut;
    if (A(3))
        wr32(A(3), cut);
    RET(noErr);
}

static void la_LAResetAnalysis(ppc_cpu *c)
{
    context *x = ctx_of(A(0));
    if (!x)
        RET(laInvalidPathErr);
    free(x->text);
    free(x->m);
    x->text = NULL;
    x->m = NULL;
    x->n = 0;
    x->nm = 0;
    RET(noErr);
}

/* LATextToMorphemes(context, preferedEncoding, textLength, sourceText, bufferSize,
 * convertFlags, structureVersion, &acceptedLength, resultBuffer) */
static void la_LATextToMorphemes(ppc_cpu *c)
{
    context *x = ctx_of(A(0));
    uint32_t enc = A(1), tl = A(2), bufsize = A(4), flags = A(5), version = A(6);
    gaddr src = A(3), accepted = A(7), result = A(8);
    if (!x || !result)
        RET(x ? rest_paramErr : laInvalidPathErr);
    if (version != 0 || (!src && tl))
        RET(rest_paramErr);
    if (!hu_available())
        RET(laFailAnalysisErr);
    uint8_t *bytes = malloc((size_t)tl + 1);
    gget(bytes, src, tl);
    uint16_t *u = malloc(((size_t)tl + 1) * 2);
    size_t used, produced;
    if (host_text_decode(enc, bytes, tl, &used, u, (size_t)tl + 1, &produced) > 1 && produced == 0 && tl) {
        free(bytes);
        free(u);
        RET(laFailAnalysisErr);
    }
    morpheme *m;
    int nm = analyze(&envs[x->env], u, (uint32_t)produced, &m);
    /* Without the end flag, the last word may continue: it waits for more text. */
    int take = (!(flags & kLAEndOfSourceTextMask) && nm > 0) ? nm - 1 : nm;
    uint32_t room = bufsize >= 12 ? (bufsize - 12) : 0, count = 0, used_text = 0, processed = 0;
    /* Records first, their texts after them: as many as fit. */
    uint8_t **enc_text = calloc((size_t)take + 1, sizeof *enc_text);
    uint32_t *enc_len = calloc((size_t)take + 1, 4);
    uint32_t cum_end = 0;
    for (int i = 0; i < take; i++) {
        uint8_t tmp[512];
        size_t u2, p2;
        host_text_encode(enc, u + m[i].start, m[i].len, &u2, tmp, sizeof tmp, &p2);
        uint32_t need = 20 + (uint32_t)p2;
        if (need > room)
            break;
        room -= need;
        enc_text[i] = malloc(p2 ? p2 : 1);
        memcpy(enc_text[i], tmp, p2);
        enc_len[i] = (uint32_t)p2;
        used_text += (uint32_t)p2;
        count++;
        cum_end += enc_len[i];
        processed = cum_end;
    }
    int32_t err = noErr;
    if (count == 0 && take > 0) {
        err = laTooSmallBufferErr;
    } else {
        wr32(result, count);
        wr32(result + 4, processed);
        wr32(result + 8, used_text);
        gaddr texts = result + 12 + 20 * count;
        gaddr at = texts;
        for (uint32_t i = 0; i < count; i++) {
            gaddr rec = result + 12 + 20 * i;
            for (uint32_t b = 0; b < enc_len[i]; b++)
                wr8(at + b, enc_text[i][b]);
            wr32(rec, enc_len[i]);
            wr32(rec + 4, at);
            wr32(rec + 8, enc_len[i]);
            wr32(rec + 12, at);
            wr32(rec + 16, m[i].pos);
            at += enc_len[i];
        }
        if (accepted)
            wr32(accepted, processed);
    }
    for (int i = 0; i < take; i++)
        free(enc_text[i]);
    free(enc_text);
    free(enc_len);
    free(m);
    free(bytes);
    free(u);
    RET(err);
}

const ppc_export rest_la_exports[] = {
    { "LAAddNewWord", la_LAAddNewWord },
    { "LACloseAnalysisContext", la_LACloseAnalysisContext },
    { "LACloseDictionary", la_LACloseDictionary },
    { "LAContinuousMorphemeAnalysis", la_LAContinuousMorphemeAnalysis },
    { "LACreateCustomEnvironment", la_LACreateCustomEnvironment },
    { "LADeleteCustomEnvironment", la_LADeleteCustomEnvironment },
    { "LAGetEnvironmentList", la_LAGetEnvironmentList },
    { "LAGetEnvironmentName", la_LAGetEnvironmentName },
    { "LAGetEnvironmentRef", la_LAGetEnvironmentRef },
    { "LAGetMorphemes", la_LAGetMorphemes },
    { "LALibraryVersion", la_LALibraryVersion },
    { "LAListAvailableDictionaries", la_LAListAvailableDictionaries },
    { "LAMorphemeAnalysis", la_LAMorphemeAnalysis },
    { "LAOpenAnalysisContext", la_LAOpenAnalysisContext },
    { "LAOpenDictionary", la_LAOpenDictionary },
    { "LAResetAnalysis", la_LAResetAnalysis },
    { "LAShiftMorphemes", la_LAShiftMorphemes },
    { "LATextToMorphemes", la_LATextToMorphemes },
};
const unsigned rest_la_export_count = sizeof rest_la_exports / sizeof rest_la_exports[0];
