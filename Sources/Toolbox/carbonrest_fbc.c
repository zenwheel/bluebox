/*
 * Find By Content (FindByContent.h): searching what documents say rather
 * than what they are called.  Sherlock and Find File kept an index of each
 * volume's text; here a program (or FBCIndexItems) builds it: each file's
 * data fork, if it is text, is split into words (letters and digits, folded
 * to lower case and plain letters), and the index lives in memory for the
 * run.  A search ranks the documents holding the words of a query by how
 * often and how rarely they occur; an example search takes the topic words
 * of documents already found; hits can be asked for their score, the query
 * words they matched, and the words that characterise them.  Summarize
 * picks the sentences with the most frequent words.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "carbonrest.h"
#include "filemgr_internal.h"

enum {
    kFBCnoIndexesFound = -30501,
    kFBCbadParam = -30503,
    kFBCfileNotIndexed = -30504,
    kFBCindexNotFound = -30518,
    kFBCnoSearchSession = -30519,
    kFBCindexingCanceled = -30520,
    kFBCaccessCanceled = -30521,
    kFBCsearchFailed = -30524,
    kFBCillegalSessionChange = -30526,
    kFBCsummarizationCanceled = -30529,
};
enum { kFBCphIndexing = 0, kFBCphSearching = 6, kFBCphSummarizing = 9 };
enum { SESSION_TAG = 0x46420000, MAX_SESSIONS = 16, MAX_VOLUMES = 16 };
enum { uppFBCCallbackProcInfo = 0x00000F91 };

typedef struct {
    char *word;
    uint32_t count;
} term;
typedef struct {
    char path[1024];
    uint8_t spec[70];
    int16_t vref;
    term *terms; /* sorted by word */
    int nterms;
    uint32_t total;
} document;

typedef struct {
    int doc;
    float score;
    char **matched;
    int nmatched;
} hit;
typedef struct {
    bool used;
    int16_t vols[MAX_VOLUMES];
    int nvols;
    bool searched;
    hit *hits;
    int nhits;
    uint32_t maxWords;
} session;

static document *docs;
static int ndocs, capdocs;
static struct {
    int16_t vref;
    uint32_t stamp, size;
} indexed[MAX_VOLUMES];
static int nindexed;
static session sessions[MAX_SESSIONS];
static gaddr callback, callback_data;

static void free_hits(session *s)
{
    for (int i = 0; i < s->nhits; i++) {
        for (int k = 0; k < s->hits[i].nmatched; k++)
            free(s->hits[i].matched[k]);
        free(s->hits[i].matched);
    }
    free(s->hits);
    s->hits = NULL;
    s->nhits = 0;
}

void rest_fbc_reset(void)
{
    for (int i = 0; i < ndocs; i++) {
        for (int k = 0; k < docs[i].nterms; k++)
            free(docs[i].terms[k].word);
        free(docs[i].terms);
    }
    free(docs);
    docs = NULL;
    ndocs = capdocs = nindexed = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        free_hits(&sessions[i]);
        memset(&sessions[i], 0, sizeof sessions[i]);
    }
    callback = callback_data = 0;
}

static session *session_of(uint32_t ref)
{
    uint32_t i = ref - SESSION_TAG;
    return i >= 1 && i <= MAX_SESSIONS && sessions[i - 1].used ? &sessions[i - 1] : NULL;
}

/* The program's callback: true if it asks to stop. */
static bool progress(uint16_t phase, double percent)
{
    if (!callback)
        return false;
    float f = (float)percent;
    uint32_t bits;
    memcpy(&bits, &f, 4);
    uint32_t args[3] = { phase, bits, callback_data };
    uint64_t saved = g_ppc.fpr[1];
    double d = f;
    memcpy(&g_ppc.fpr[1], &d, 8);
    uint32_t r = aem_call_proc(callback, uppFBCCallbackProcInfo, args, 3);
    g_ppc.fpr[1] = saved;
    return (r & 0xFF) != 0;
}

/* ---- Words ---- */

/* A word's letters: MacRoman letters with marks to their plain forms. */
static int fold(uint8_t b)
{
    static const char *high = "AAACEN OUaaaaaaceeeeiiiinooooouuuu" /* 0x80-0x9F */
                              "                                " /* 0xA0-0xBF: symbols */
                              "                                " /* 0xC0-0xDF */
                              "                                ";
    if (b < 0x80)
        return (b >= 'A' && b <= 'Z') ? b + 32 : b;
    if (b >= 0x80 && b <= 0x9F && high[b - 0x80] != ' ')
        return (high[b - 0x80] >= 'A' && high[b - 0x80] <= 'Z') ? high[b - 0x80] + 32 : high[b - 0x80];
    return 0;
}

static bool alnum(int c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }

static const char *const stop[] = { "the", "and", "for", "are", "but", "not", "you", "all", "any", "can", "had",
                                    "her", "was", "one", "our", "out", "has", "have", "with", "this", "that",
                                    "from", "they", "will", "would", "there", "their", "what", "about", "which",
                                    "when", "were", "been", "its", "his", "she", "him", "who", "how", "than", "then" };

static bool is_stop(const char *w)
{
    for (size_t i = 0; i < sizeof stop / sizeof *stop; i++)
        if (!strcmp(w, stop[i]))
            return true;
    return false;
}

typedef struct {
    char **w;
    int n, cap;
} wordlist;

/* The words of some text: each as a malloc'd lower case string. */
static void words_of(const uint8_t *text, size_t n, wordlist *out)
{
    char cur[48];
    size_t k = 0;
    for (size_t i = 0; i <= n; i++) {
        int c = i < n ? fold(text[i]) : 0;
        if (c && alnum(c)) {
            if (k < sizeof cur - 1)
                cur[k++] = (char)c;
        } else if (k) {
            cur[k] = 0;
            if (k >= 2 && !is_stop(cur)) {
                if (out->n == out->cap) {
                    out->cap = out->cap ? out->cap * 2 : 64;
                    out->w = realloc(out->w, (size_t)out->cap * sizeof *out->w);
                }
                out->w[out->n++] = strdup(cur);
            }
            k = 0;
        }
    }
}

static int cmp_words(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void free_words(wordlist *l)
{
    for (int i = 0; i < l->n; i++)
        free(l->w[i]);
    free(l->w);
    memset(l, 0, sizeof *l);
}

/* Add a text to the index as a document. */
static void add_document(const char *path, int16_t vref, const uint8_t spec[70], const uint8_t *text, size_t n)
{
    wordlist wl = { 0 };
    words_of(text, n, &wl);
    if (!wl.n) {
        free_words(&wl);
        return;
    }
    qsort(wl.w, (size_t)wl.n, sizeof *wl.w, cmp_words);
    if (ndocs == capdocs) {
        capdocs = capdocs ? capdocs * 2 : 32;
        docs = realloc(docs, (size_t)capdocs * sizeof *docs);
    }
    int at = -1;
    for (int i = 0; i < ndocs; i++)
        if (!strcmp(docs[i].path, path))
            at = i;
    if (at < 0)
        at = ndocs++;
    else {
        for (int k = 0; k < docs[at].nterms; k++)
            free(docs[at].terms[k].word);
        free(docs[at].terms);
    }
    document *d = &docs[at];
    memset(d, 0, sizeof *d);
    snprintf(d->path, sizeof d->path, "%s", path);
    memcpy(d->spec, spec, 70);
    d->vref = vref;
    d->terms = calloc((size_t)wl.n, sizeof *d->terms);
    for (int i = 0; i < wl.n; i++) {
        if (d->nterms && !strcmp(d->terms[d->nterms - 1].word, wl.w[i])) {
            d->terms[d->nterms - 1].count++;
            free(wl.w[i]);
        } else {
            d->terms[d->nterms].word = wl.w[i];
            d->terms[d->nterms++].count = 1;
        }
        d->total++;
    }
    free(wl.w);
}

static term *find_term(const document *d, const char *w)
{
    int lo = 0, hi = d->nterms - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(d->terms[mid].word, w);
        if (c == 0)
            return &d->terms[mid];
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

/* ---- Indexing ---- */

static bool looks_like_text(const uint8_t *b, size_t n)
{
    size_t odd = 0, lim = n < 4096 ? n : 4096;
    for (size_t i = 0; i < lim; i++)
        if (b[i] < 9 || (b[i] > 13 && b[i] < 32 && b[i] != 27))
            odd++;
    return lim > 0 && odd * 20 < lim;
}

static void note_indexed(int16_t vref, uint32_t bytes)
{
    int i = 0;
    while (i < nindexed && indexed[i].vref != vref)
        i++;
    if (i == nindexed) {
        if (nindexed == MAX_VOLUMES)
            return;
        nindexed++;
        indexed[i].vref = vref;
        indexed[i].size = 0;
    }
    indexed[i].stamp = (uint32_t)time(NULL) + 2082844800u;
    indexed[i].size += bytes;
}

static int index_path(const char *path, int depth, bool *cancel, int total, int *done)
{
    if (*cancel)
        return 0;
    int n = 0;
    if (fm_is_dir(path)) {
        char **kids;
        int k = fm_list_dir(path, &kids);
        for (int i = 0; i < k; i++) {
            if (depth < 16)
                n += index_path(kids[i], depth + 1, cancel, total, done);
            free(kids[i]);
        }
        free(kids);
        return n;
    }
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size > 0 && size < (32L << 20)) {
            uint8_t *buf = malloc((size_t)size);
            size_t got = fread(buf, 1, (size_t)size, f);
            if (looks_like_text(buf, got)) {
                uint8_t spec[70] = { 0 };
                gaddr tmp = tb_NewPtr(70, false, true);
                if (tmp) {
                    fm_spec_of_path(path, tmp);
                    gget(spec, tmp, 70);
                    tb_DisposePtr(tmp);
                }
                int16_t vref = fm_vref_of_path(path);
                add_document(path, vref, spec, buf, got);
                note_indexed(vref, (uint32_t)(got / 2 + 64));
                n++;
            }
            free(buf);
        }
        fclose(f);
    }
    (*done)++;
    if (progress(kFBCphIndexing, total ? 100.0 * *done / total : 100.0))
        *cancel = true;
    return n;
}

/* FBCIndexItems(items, count): FSSpecs of files and folders */
static int16_t index_items(gaddr items, uint32_t count)
{
    if (!items && count)
        return kFBCbadParam;
    bool cancel = false;
    int done = 0;
    for (uint32_t i = 0; i < count && !cancel; i++) {
        char path[1024];
        gaddr spec = items + 70 * i;
        if (fm_resolve((int16_t)rd16(spec), (int32_t)rd32(spec + 2), spec + 6, path, sizeof path) != noErr)
            return kFBCfileNotIndexed;
        index_path(path, 0, &cancel, (int)count, &done);
    }
    progress(kFBCphIndexing, 100);
    return cancel ? kFBCindexingCanceled : noErr;
}

static void fbc_FBCIndexItems(ppc_cpu *c) { RET((int32_t)index_items(A(0), A(1))); }
static void fbc_FBCIndexItemsInLanguages(ppc_cpu *c) { RET((int32_t)index_items(A(0), A(1))); }

/* FBCFindIndexFileFolderForFolder(inFolder, outFolder): the index is kept in memory; the folder itself is its home */
static void fbc_FBCFindIndexFileFolderForFolder(ppc_cpu *c)
{
    if (!A(0) || !A(1))
        RET(kFBCbadParam);
    for (uint32_t i = 0; i < 80; i++)
        wr8(A(1) + i, rd8(A(0) + i));
    RET(noErr);
}

/* FBCDeleteIndexFileForFolder(folder): its documents leave the index */
static void fbc_FBCDeleteIndexFileForFolder(ppc_cpu *c)
{
    char path[1024];
    if (!A(0) || hfs_fsref_path(A(0), path, sizeof path))
        RET(kFBCbadParam);
    size_t pl = strlen(path);
    int removed = 0;
    for (int i = 0; i < ndocs;) {
        if (!strncmp(docs[i].path, path, pl) && (docs[i].path[pl] == '/' || !docs[i].path[pl])) {
            for (int k = 0; k < docs[i].nterms; k++)
                free(docs[i].terms[k].word);
            free(docs[i].terms);
            memmove(&docs[i], &docs[i + 1], (size_t)(ndocs - i - 1) * sizeof *docs);
            ndocs--;
            removed++;
        } else
            i++;
    }
    RET(removed ? noErr : kFBCindexNotFound);
}

/* ---- Volumes ---- */

static void fbc_FBCSetCallback(ppc_cpu *c)
{
    callback = A(0);
    callback_data = A(1);
    DONE();
}

static void fbc_FBCSetHeapReservation(ppc_cpu *c) { DONE(); }

static int indexed_slot(int16_t vref)
{
    for (int i = 0; i < nindexed; i++)
        if (indexed[i].vref == vref)
            return i;
    return -1;
}

static void fbc_FBCVolumeIsIndexed(ppc_cpu *c) { RET(indexed_slot(S16(0)) >= 0); }

/* A volume on a file server: none is mounted through a share here. */
static void fbc_FBCVolumeIsRemote(ppc_cpu *c) { RET(0); }

static void fbc_FBCVolumeIndexTimeStamp(ppc_cpu *c)
{
    int i = indexed_slot(S16(0));
    if (!A(1))
        RET(kFBCbadParam);
    if (i < 0)
        RET(kFBCindexNotFound);
    wr32(A(1), indexed[i].stamp);
    RET(noErr);
}

static void fbc_FBCVolumeIndexPhysicalSize(ppc_cpu *c)
{
    int i = indexed_slot(S16(0));
    if (!A(1))
        RET(kFBCbadParam);
    if (i < 0)
        RET(kFBCindexNotFound);
    wr32(A(1), indexed[i].size);
    RET(noErr);
}

/* ---- Sessions ---- */

static void fbc_FBCCreateSearchSession(ppc_cpu *c)
{
    if (!A(0))
        RET(kFBCbadParam);
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (!sessions[i].used) {
            memset(&sessions[i], 0, sizeof sessions[i]);
            sessions[i].used = true;
            wr32(A(0), SESSION_TAG + (uint32_t)i + 1);
            RET(noErr);
        }
    wr32(A(0), 0);
    RET(rest_memFullErr);
}

static void fbc_FBCDestroySearchSession(ppc_cpu *c)
{
    session *s = session_of(A(0));
    if (!s)
        RET(kFBCnoSearchSession);
    free_hits(s);
    memset(s, 0, sizeof *s);
    RET(noErr);
}

static int16_t add_volume(session *s, int16_t vref)
{
    if (s->searched)
        return kFBCillegalSessionChange;
    for (int i = 0; i < s->nvols; i++)
        if (s->vols[i] == vref)
            return noErr;
    if (s->nvols == MAX_VOLUMES)
        return rest_memFullErr;
    s->vols[s->nvols++] = vref;
    return noErr;
}

/* FBCAddAllVolumesToSession(session, includeRemote) */
static void fbc_FBCAddAllVolumesToSession(ppc_cpu *c)
{
    session *s = session_of(A(0));
    if (!s)
        RET(kFBCnoSearchSession);
    char root[1024];
    uint8_t name[32];
    int16_t vref, drive, driver;
    int16_t err = noErr;
    for (uint32_t i = 1; err == noErr && fm_volume(i, 0, &vref, root, sizeof root, name, &drive, &driver); i++)
        err = add_volume(s, vref);
    RET((int32_t)err);
}

/* FBCSetSessionVolumes(session, vRefNums[], numVolumes) */
static void fbc_FBCSetSessionVolumes(ppc_cpu *c)
{
    session *s = session_of(A(0));
    if (!s)
        RET(kFBCnoSearchSession);
    if (s->searched)
        RET(kFBCillegalSessionChange);
    if ((!A(1) && A(2)) || A(2) > MAX_VOLUMES)
        RET(kFBCbadParam);
    s->nvols = 0;
    for (uint32_t i = 0; i < A(2); i++)
        add_volume(s, (int16_t)rd16(A(1) + 2 * i));
    RET(noErr);
}

static void fbc_FBCAddVolumeToSession(ppc_cpu *c)
{
    session *s = session_of(A(0));
    RET(s ? (int32_t)add_volume(s, S16(1)) : kFBCnoSearchSession);
}

static void fbc_FBCRemoveVolumeFromSession(ppc_cpu *c)
{
    session *s = session_of(A(0));
    if (!s)
        RET(kFBCnoSearchSession);
    if (s->searched)
        RET(kFBCillegalSessionChange);
    for (int i = 0; i < s->nvols; i++)
        if (s->vols[i] == S16(1)) {
            memmove(&s->vols[i], &s->vols[i + 1], (size_t)(s->nvols - i - 1) * sizeof s->vols[0]);
            s->nvols--;
            RET(noErr);
        }
    RET(kFBCindexNotFound);
}

static void fbc_FBCGetSessionVolumeCount(ppc_cpu *c)
{
    session *s = session_of(A(0));
    if (!s || !A(1))
        RET(s ? kFBCbadParam : kFBCnoSearchSession);
    wr16(A(1), (uint16_t)s->nvols);
    RET(noErr);
}

/* FBCGetSessionVolumes(session, vRefNums[], &numVolumes): numVolumes in = room */
static void fbc_FBCGetSessionVolumes(ppc_cpu *c)
{
    session *s = session_of(A(0));
    if (!s || !A(1) || !A(2))
        RET(s ? kFBCbadParam : kFBCnoSearchSession);
    uint32_t room = rd16(A(2)), k = 0;
    for (; k < (uint32_t)s->nvols && k < room; k++)
        wr16(A(1) + 2 * k, (uint16_t)s->vols[k]);
    wr16(A(2), (uint16_t)k);
    RET(noErr);
}

static void fbc_FBCCloneSearchSession(ppc_cpu *c)
{
    session *s = session_of(A(0));
    if (!s || !A(1))
        RET(s ? kFBCbadParam : kFBCnoSearchSession);
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (!sessions[i].used) {
            memset(&sessions[i], 0, sizeof sessions[i]);
            sessions[i].used = true;
            memcpy(sessions[i].vols, s->vols, sizeof s->vols);
            sessions[i].nvols = s->nvols;
            wr32(A(1), SESSION_TAG + (uint32_t)i + 1);
            RET(noErr);
        }
    RET(rest_memFullErr);
}

/* ---- Searching ---- */

typedef struct {
    int doc;
    double score;
} scored;
static int cmp_scored(const void *a, const void *b)
{
    const scored *x = a, *y = b;
    if (x->score != y->score)
        return x->score < y->score ? 1 : -1;
    return x->doc - y->doc;
}

static bool in_session(const session *s, int16_t vref)
{
    for (int i = 0; i < s->nvols; i++)
        if (s->vols[i] == vref)
            return true;
    return false;
}

static bool under_targets(const document *d, gaddr targets, uint32_t n)
{
    if (!n)
        return true;
    for (uint32_t i = 0; i < n; i++) {
        char dir[1024];
        gaddr spec = targets + 70 * i;
        if (fm_resolve((int16_t)rd16(spec), (int32_t)rd32(spec + 2), spec + 6, dir, sizeof dir) != noErr)
            continue;
        size_t l = strlen(dir);
        if (!strncmp(d->path, dir, l) && (d->path[l] == '/' || !d->path[l]))
            return true;
    }
    return false;
}

/* The documents in scope holding any of the query words, ranked; nonmatching ones leave. */
static int16_t run_search(session *s, wordlist *q, gaddr targets, uint32_t ntargets, uint32_t maxHits, uint32_t maxWords,
                          const int *exclude, int nexclude)
{
    if (s->nvols == 0)
        return kFBCnoSearchSession;
    bool any = false;
    for (int i = 0; i < s->nvols; i++)
        any = any || indexed_slot(s->vols[i]) >= 0;
    if (!any)
        return kFBCnoIndexesFound;
    free_hits(s);
    s->searched = true;
    s->maxWords = maxWords;
    qsort(q->w, (size_t)q->n, sizeof *q->w, cmp_words);
    int uq = 0; /* distinct query words, in a list of their own */
    char **uw = malloc((size_t)(q->n ? q->n : 1) * sizeof *uw);
    for (int i = 0; i < q->n; i++)
        if (!uq || strcmp(uw[uq - 1], q->w[i]))
            uw[uq++] = q->w[i];
    scored *sc = calloc(ndocs ? (size_t)ndocs : 1, sizeof *sc);
    int ns = 0;
    double best = 0;
    for (int d = 0; d < ndocs; d++) {
        if (!in_session(s, docs[d].vref) || !under_targets(&docs[d], targets, ntargets))
            continue;
        bool ex = false;
        for (int e = 0; e < nexclude; e++)
            ex = ex || exclude[e] == d;
        if (ex)
            continue;
        double score = 0;
        for (int w = 0; w < uq; w++) {
            term *t = find_term(&docs[d], uw[w]);
            if (!t)
                continue;
            int df = 0;
            for (int o = 0; o < ndocs; o++)
                df += find_term(&docs[o], uw[w]) != NULL;
            double idf = log(1.0 + (double)ndocs / df);
            score += (1.0 + log((double)t->count)) * idf / sqrt((double)docs[d].total + 1.0);
        }
        if (score > 0) {
            sc[ns++] = (scored){ d, score };
            best = score > best ? score : best;
        }
        if (progress(kFBCphSearching, 100.0 * (d + 1) / ndocs)) {
            free(sc);
            free(uw);
            return kFBCaccessCanceled;
        }
    }
    qsort(sc, (size_t)ns, sizeof *sc, cmp_scored);
    if (maxHits && (uint32_t)ns > maxHits)
        ns = (int)maxHits;
    s->hits = calloc(ns ? (size_t)ns : 1, sizeof *s->hits);
    for (int i = 0; i < ns; i++) {
        hit *h = &s->hits[i];
        h->doc = sc[i].doc;
        h->score = (float)(sc[i].score / best);
        h->matched = calloc(uq ? (size_t)uq : 1, sizeof *h->matched);
        for (int w = 0; w < uq; w++)
            if (find_term(&docs[h->doc], uw[w]) && (!maxWords || (uint32_t)h->nmatched < maxWords))
                h->matched[h->nmatched++] = strdup(uw[w]);
    }
    s->nhits = ns;
    free(sc);
    free(uw);
    return noErr;
}

static void fbc_FBCDoQuerySearch(ppc_cpu *c)
{
    session *s = session_of(A(0));
    if (!s)
        RET(kFBCnoSearchSession);
    char query[1024];
    if (!A(1))
        RET(kFBCbadParam);
    size_t n = rest_cstr(A(1), query, sizeof query);
    wordlist q = { 0 };
    words_of((const uint8_t *)query, n, &q);
    int16_t e = q.n ? run_search(s, &q, A(2), A(3), A(4), A(5), NULL, 0) : kFBCbadParam;
    free_words(&q);
    RET((int32_t)e);
}

/* The topic words of a document: the most characteristic by frequency and rarity. */
static int topic_words(int doc, int max, char **out)
{
    document *d = &docs[doc];
    typedef struct {
        int t;
        double w;
    } tw;
    tw *all = calloc(d->nterms ? (size_t)d->nterms : 1, sizeof *all);
    for (int i = 0; i < d->nterms; i++) {
        int df = 0;
        for (int o = 0; o < ndocs; o++)
            df += find_term(&docs[o], d->terms[i].word) != NULL;
        all[i] = (tw){ i, (1.0 + log((double)d->terms[i].count)) * log(1.0 + (double)ndocs / df) };
    }
    int k = 0;
    for (int round = 0; round < max && round < d->nterms; round++) {
        int best = -1;
        for (int i = 0; i < d->nterms; i++)
            if (all[i].w >= 0 && (best < 0 || all[i].w > all[best].w))
                best = i;
        out[k++] = strdup(d->terms[all[best].t].word);
        all[best].w = -1;
    }
    free(all);
    return k;
}

/* FBCDoExampleSearch(session, exampleHitNums[], numExamples, targetDirs, numTargets, maxHits, maxHitWords) */
static void fbc_FBCDoExampleSearch(ppc_cpu *c)
{
    session *s = session_of(A(0));
    if (!s)
        RET(kFBCnoSearchSession);
    if ((!A(1) && A(2)) || !A(2))
        RET(kFBCbadParam);
    wordlist q = { 0 };
    int *ex = calloc(A(2), sizeof *ex);
    int nex = 0;
    for (uint32_t i = 0; i < A(2); i++) {
        uint32_t h = rd32(A(1) + 4 * i);
        if (h >= (uint32_t)s->nhits) {
            free(ex);
            RET(kFBCbadParam);
        }
        ex[nex++] = s->hits[h].doc;
        char *words[16];
        int k = topic_words(s->hits[h].doc, 16, words);
        for (int j = 0; j < k; j++) {
            if (q.n == q.cap) {
                q.cap = q.cap ? q.cap * 2 : 32;
                q.w = realloc(q.w, (size_t)q.cap * sizeof *q.w);
            }
            q.w[q.n++] = words[j];
        }
    }
    int16_t e = run_search(s, &q, A(3), A(4), A(5), A(6), ex, nex);
    free(ex);
    free_words(&q);
    RET((int32_t)e);
}

/* FBCBlindExampleSearch(examples[], numExamples, targetDirs, numTargets, maxHits, maxHitWords,
 * allIndexes, includeRemote, &session): documents given, not found before */
static void fbc_FBCBlindExampleSearch(ppc_cpu *c)
{
    if (!A(0) || !A(1) || !A(8))
        RET(kFBCbadParam);
    wr32(A(8), 0);
    wordlist q = { 0 };
    for (uint32_t i = 0; i < A(1); i++) {
        char path[1024];
        gaddr spec = A(0) + 70 * i;
        if (fm_resolve((int16_t)rd16(spec), (int32_t)rd32(spec + 2), spec + 6, path, sizeof path) != noErr)
            continue;
        FILE *f = fopen(path, "rb");
        if (!f)
            continue;
        uint8_t *buf = malloc(1 << 20);
        size_t got = fread(buf, 1, 1 << 20, f);
        fclose(f);
        wordlist w = { 0 };
        words_of(buf, got, &w);
        free(buf);
        /* the most frequent distinct words of the example */
        qsort(w.w, (size_t)w.n, sizeof *w.w, cmp_words);
        for (int k = 0; k < w.n && q.n < 64; k++) {
            if (k && !strcmp(w.w[k], w.w[k - 1]))
                continue;
            if (q.n == q.cap) {
                q.cap = q.cap ? q.cap * 2 : 32;
                q.w = realloc(q.w, (size_t)q.cap * sizeof *q.w);
            }
            q.w[q.n++] = strdup(w.w[k]);
        }
        free_words(&w);
    }
    uint32_t ref = 0;
    for (int i = 0; i < MAX_SESSIONS && !ref; i++)
        if (!sessions[i].used) {
            memset(&sessions[i], 0, sizeof sessions[i]);
            sessions[i].used = true;
            ref = SESSION_TAG + (uint32_t)i + 1;
        }
    if (!ref) {
        free_words(&q);
        RET(rest_memFullErr);
    }
    session *s = session_of(ref);
    char root[1024];
    uint8_t name[32];
    int16_t vref, drive, driver;
    for (uint32_t i = 1; fm_volume(i, 0, &vref, root, sizeof root, name, &drive, &driver); i++)
        if (A(6) || indexed_slot(vref) >= 0)
            add_volume(s, vref);
    int16_t e = q.n ? run_search(s, &q, A(2), A(3), A(4), A(5), NULL, 0) : kFBCbadParam;
    free_words(&q);
    if (e) {
        free_hits(s);
        memset(s, 0, sizeof *s);
        RET((int32_t)e);
    }
    wr32(A(8), ref);
    RET(noErr);
}

static void fbc_FBCGetHitCount(ppc_cpu *c)
{
    session *s = session_of(A(0));
    if (!s || !A(1))
        RET(s ? kFBCbadParam : kFBCnoSearchSession);
    wr32(A(1), (uint32_t)s->nhits);
    RET(noErr);
}

static hit *hit_of(uint32_t ref, uint32_t n, int16_t *err)
{
    session *s = session_of(ref);
    if (!s) {
        *err = kFBCnoSearchSession;
        return NULL;
    }
    if (n >= (uint32_t)s->nhits) {
        *err = kFBCbadParam;
        return NULL;
    }
    return &s->hits[n];
}

static void fbc_FBCGetHitDocument(ppc_cpu *c)
{
    int16_t e = noErr;
    hit *h = hit_of(A(0), A(1), &e);
    if (!h || !A(2))
        RET(h ? kFBCbadParam : e);
    for (uint32_t i = 0; i < 70; i++)
        wr8(A(2) + i, docs[h->doc].spec[i]);
    RET(noErr);
}

static void fbc_FBCGetHitDocumentRef(ppc_cpu *c)
{
    int16_t e = noErr;
    hit *h = hit_of(A(0), A(1), &e);
    if (!h || !A(2))
        RET(h ? kFBCbadParam : e);
    hfs_make_fsref(A(2), docs[h->doc].path);
    RET(noErr);
}

static void fbc_FBCGetHitScore(ppc_cpu *c)
{
    int16_t e = noErr;
    hit *h = hit_of(A(0), A(1), &e);
    if (!h || !A(2))
        RET(h ? kFBCbadParam : e);
    wr32(A(2), svc_f32_bits(h->score));
    RET(noErr);
}

/* A list of words in a block of char pointers then their text, all in one pointer. */
static int16_t give_words(char **w, int n, gaddr countp, gaddr listp)
{
    if (!countp || !listp)
        return kFBCbadParam;
    size_t chars = 0;
    for (int i = 0; i < n; i++)
        chars += strlen(w[i]) + 1;
    gaddr block = tb_NewPtr((int32_t)(4 * (size_t)n + chars + 4), false, true);
    if (!block)
        return rest_memFullErr;
    gaddr at = block + 4 * (uint32_t)n;
    for (int i = 0; i < n; i++) {
        wr32(block + 4 * (uint32_t)i, at);
        for (size_t k = 0; k <= strlen(w[i]); k++)
            wr8(at + (uint32_t)k, (uint8_t)w[i][k]);
        at += (uint32_t)strlen(w[i]) + 1;
    }
    wr32(countp, (uint32_t)n);
    wr32(listp, block);
    return noErr;
}

static void fbc_FBCGetMatchedWords(ppc_cpu *c)
{
    int16_t e = noErr;
    hit *h = hit_of(A(0), A(1), &e);
    if (!h)
        RET(e);
    RET((int32_t)give_words(h->matched, h->nmatched, A(2), A(3)));
}

static void fbc_FBCGetTopicWords(ppc_cpu *c)
{
    int16_t e = noErr;
    hit *h = hit_of(A(0), A(1), &e);
    if (!h)
        RET(e);
    session *s = session_of(A(0));
    char *words[32];
    int k = topic_words(h->doc, s->maxWords && s->maxWords < 32 ? (int)s->maxWords : 8, words);
    int16_t r = give_words(words, k, A(2), A(3));
    for (int i = 0; i < k; i++)
        free(words[i]);
    RET((int32_t)r);
}

static void fbc_FBCDestroyWordList(ppc_cpu *c)
{
    if (A(0))
        tb_DisposePtr(A(0));
    RET(noErr);
}

static void fbc_FBCReleaseSessionHits(ppc_cpu *c)
{
    session *s = session_of(A(0));
    if (!s)
        RET(kFBCnoSearchSession);
    free_hits(s);
    RET(noErr);
}

/* FBCSummarize(inBuf, inLength, outBuf, &outLength, &numSentences): the sentences with the commonest words,
 * in their order; outLength in is the room; numSentences in is how many are wanted. */
static void fbc_FBCSummarize(ppc_cpu *c)
{
    gaddr in = A(0), out = A(2), outlen = A(3), nsent = A(4);
    uint32_t len = A(1);
    if ((!in && len) || !out || !outlen || !nsent)
        RET(kFBCbadParam);
    uint8_t *text = malloc((size_t)len + 1);
    gget(text, in, len);
    /* Sentences end at . ! ? or a paragraph; words are counted over all of it */
    typedef struct {
        uint32_t start, end;
        double score;
    } sentence;
    sentence *ss = NULL;
    int n = 0, cap = 0;
    uint32_t st = 0;
    for (uint32_t i = 0; i <= len; i++) {
        bool endp = i == len || text[i] == '.' || text[i] == '!' || text[i] == '?' || text[i] == '\r' || text[i] == '\n';
        if (!endp)
            continue;
        uint32_t e = i < len ? i + 1 : i;
        bool real = false;
        for (uint32_t k = st; k < e; k++)
            real = real || fold(text[k]);
        if (real) {
            if (n == cap) {
                cap = cap ? cap * 2 : 16;
                ss = realloc(ss, (size_t)cap * sizeof *ss);
            }
            ss[n++] = (sentence){ st, e, 0 };
        }
        st = e;
    }
    wordlist all = { 0 };
    words_of(text, len, &all);
    qsort(all.w, (size_t)all.n, sizeof *all.w, cmp_words);
    for (int s = 0; s < n; s++) {
        wordlist w = { 0 };
        words_of(text + ss[s].start, ss[s].end - ss[s].start, &w);
        double sc = 0;
        for (int k = 0; k < w.n; k++) {
            int lo = 0, hi = all.n - 1, cnt = 0;
            char **p = bsearch(&w.w[k], all.w, (size_t)all.n, sizeof *all.w, cmp_words);
            if (p) {
                lo = (int)(p - all.w);
                hi = lo;
                while (lo > 0 && !strcmp(all.w[lo - 1], w.w[k]))
                    lo--;
                while (hi + 1 < all.n && !strcmp(all.w[hi + 1], w.w[k]))
                    hi++;
                cnt = hi - lo + 1;
            }
            sc += cnt;
        }
        ss[s].score = w.n ? sc / (w.n + 3.0) : 0;
        free_words(&w);
    }
    free_words(&all);
    uint32_t want = rd32(nsent);
    if (!want || want > (uint32_t)n)
        want = (uint32_t)n;
    /* the best by score, kept in text order */
    bool *take = calloc(n ? (size_t)n : 1, 1);
    for (uint32_t r = 0; r < want; r++) {
        int best = -1;
        for (int s = 0; s < n; s++)
            if (!take[s] && (best < 0 || ss[s].score > ss[best].score))
                best = s;
        if (best >= 0)
            take[best] = true;
    }
    uint32_t room = rd32(outlen), used = 0, got = 0;
    for (int s = 0; s < n; s++)
        if (take[s]) {
            uint32_t k = ss[s].end - ss[s].start;
            if (used + k > room)
                break;
            for (uint32_t b = 0; b < k; b++)
                wr8(out + used + b, text[ss[s].start + b]);
            used += k;
            got++;
        }
    wr32(outlen, used);
    wr32(nsent, got);
    free(take);
    free(ss);
    free(text);
    RET(progress(kFBCphSummarizing, 100) ? kFBCsummarizationCanceled : noErr);
}

const ppc_export rest_fbc_exports[] = {
    { "FBCAddAllVolumesToSession", fbc_FBCAddAllVolumesToSession },
    { "FBCAddVolumeToSession", fbc_FBCAddVolumeToSession },
    { "FBCBlindExampleSearch", fbc_FBCBlindExampleSearch },
    { "FBCCloneSearchSession", fbc_FBCCloneSearchSession },
    { "FBCCreateSearchSession", fbc_FBCCreateSearchSession },
    { "FBCDeleteIndexFileForFolder", fbc_FBCDeleteIndexFileForFolder },
    { "FBCDestroySearchSession", fbc_FBCDestroySearchSession },
    { "FBCDestroyWordList", fbc_FBCDestroyWordList },
    { "FBCDoExampleSearch", fbc_FBCDoExampleSearch },
    { "FBCDoQuerySearch", fbc_FBCDoQuerySearch },
    { "FBCFindIndexFileFolderForFolder", fbc_FBCFindIndexFileFolderForFolder },
    { "FBCGetHitCount", fbc_FBCGetHitCount },
    { "FBCGetHitDocument", fbc_FBCGetHitDocument },
    { "FBCGetHitDocumentRef", fbc_FBCGetHitDocumentRef },
    { "FBCGetHitScore", fbc_FBCGetHitScore },
    { "FBCGetMatchedWords", fbc_FBCGetMatchedWords },
    { "FBCGetSessionVolumeCount", fbc_FBCGetSessionVolumeCount },
    { "FBCGetSessionVolumes", fbc_FBCGetSessionVolumes },
    { "FBCGetTopicWords", fbc_FBCGetTopicWords },
    { "FBCIndexItems", fbc_FBCIndexItems },
    { "FBCIndexItemsInLanguages", fbc_FBCIndexItemsInLanguages },
    { "FBCReleaseSessionHits", fbc_FBCReleaseSessionHits },
    { "FBCRemoveVolumeFromSession", fbc_FBCRemoveVolumeFromSession },
    { "FBCSetCallback", fbc_FBCSetCallback },
    { "FBCSetHeapReservation", fbc_FBCSetHeapReservation },
    { "FBCSetSessionVolumes", fbc_FBCSetSessionVolumes },
    { "FBCSummarize", fbc_FBCSummarize },
    { "FBCVolumeIndexPhysicalSize", fbc_FBCVolumeIndexPhysicalSize },
    { "FBCVolumeIndexTimeStamp", fbc_FBCVolumeIndexTimeStamp },
    { "FBCVolumeIsIndexed", fbc_FBCVolumeIsIndexed },
    { "FBCVolumeIsRemote", fbc_FBCVolumeIsRemote },
};
const unsigned rest_fbc_export_count = sizeof rest_fbc_exports / sizeof rest_fbc_exports[0];
