/*
 * The Speech Recognition Manager (SpeechRecognition.h, $AA56): the
 * recognition system, and the language objects (models, paths, phrases and
 * words) that tell a recognizer what to listen for.  The objects are all
 * here: built, changed, counted, given properties, saved to handles and
 * files and read back.  There is no recognizer: the host offers no speech
 * recognition a program could use for these command sets, so a recognizer
 * cannot be made (kSRNotAvailable) and what needs one answers as
 * PlainTalk's Manager did for an object that is not a recognizer.
 *
 * Each reference a program holds is its own; releasing one drops it, and
 * an object goes when no reference or container holds it.
 */
#include <stdlib.h>
#include <string.h>

#include "carbonrest.h"
#include "filemgr_internal.h"

enum {
    kSRNotAvailable = -5100,
    kSRComponentNotFound = -5102,
    kSROutOfMemory = -5103,
    kSRNotASpeechObject = -5104,
    kSRBadParameter = -5105,
    kSRParamOutOfRange = -5106,
    kSRBufferTooSmall = -5108,
    kSRNotARecSystem = -5109,
    kSRCantSetProperty = -5111,
    kSRCantGetProperty = -5112,
    kSRHasNoSubItems = -5122,
    kSRSubItemNotFound = -5123,
    kSRAlreadyReleased = -5125,
    kSRCantAdd = -5131,
    kSRCantReadLanguageObject = -5133,
};
#define T(s) fourcc(s)
enum { SR_SYSTEM, SR_MODEL, SR_PATH, SR_PHRASE, SR_WORD };
enum { MAX_REFS = 1024, REF_TAG = 0x53520000, MAX_CHILDREN = 4096, MAX_DEPTH = 16 };

typedef struct sr_obj sr_obj;
typedef struct {
    sr_obj *o;
    int32_t refcon;
} child;
struct sr_obj {
    int kind;
    uint8_t *name;
    uint32_t namelen;
    int32_t refcon;
    bool optional, enabled, repeatable, rejectable;
    uint16_t level;
    child *kids;
    int nkids;
    int holds; /* references and containers holding it */
};

static sr_obj *refs[MAX_REFS];
static bool released[MAX_REFS];
static uint32_t nrefs;

static void sr_free(sr_obj *o);

/* A data fork read or written at its mark, and what is left of it. */
static int16_t fm_fork_io_mark(int16_t ref, bool write, uint32_t count, gaddr buf, uint32_t *actual)
{
    return fm_fork_io(ref, write, 0 /* fsAtMark */, 0, count, buf, actual);
}

static int16_t fm_fork_remaining(int16_t ref, uint32_t *left)
{
    char path[1024];
    bool rsrc, write;
    uint32_t pos, len;
    if (!fm_fork_state(ref, path, sizeof path, &rsrc, &write, &pos, &len))
        return -51; /* rfNumErr */
    *left = len > pos ? len - pos : 0;
    return noErr;
}

static void drop(sr_obj *o)
{
    if (o && --o->holds <= 0)
        sr_free(o);
}

static void sr_free(sr_obj *o)
{
    for (int i = 0; i < o->nkids; i++)
        drop(o->kids[i].o);
    free(o->kids);
    free(o->name);
    free(o);
}

void rest_sr_reset(void)
{
    for (uint32_t i = 0; i < nrefs; i++)
        if (refs[i]) {
            drop(refs[i]);
            refs[i] = NULL;
        }
    nrefs = 0;
    memset(released, 0, sizeof released);
}

static sr_obj *new_obj(int kind, const uint8_t *name, uint32_t len)
{
    sr_obj *o = calloc(1, sizeof *o);
    if (!o)
        return NULL;
    o->kind = kind;
    o->enabled = true;
    o->level = 50; /* kSRDefaultRejectionLevel */
    o->name = malloc(len ? len : 1);
    memcpy(o->name, name, len);
    o->namelen = len;
    return o;
}

/* A new reference to an object (it takes a hold); the ref, or 0. */
static uint32_t make_ref(sr_obj *o)
{
    for (uint32_t i = 0; i < nrefs; i++)
        if (!refs[i] && !released[i]) {
            refs[i] = o;
            o->holds++;
            return REF_TAG + i + 1;
        }
    if (nrefs == MAX_REFS)
        return 0;
    refs[nrefs] = o;
    o->holds++;
    return REF_TAG + ++nrefs;
}

static sr_obj *obj_of(int32_t ref)
{
    uint32_t i = (uint32_t)ref - REF_TAG;
    return i >= 1 && i <= nrefs ? refs[i - 1] : NULL;
}

/* Why a ref is no object: released, or never one. */
static int16_t bad_ref(int32_t ref)
{
    uint32_t i = (uint32_t)ref - REF_TAG;
    return i >= 1 && i <= nrefs && released[i - 1] ? kSRAlreadyReleased : kSRNotASpeechObject;
}

static sr_obj *language_of(int32_t ref, int16_t *err)
{
    sr_obj *o = obj_of(ref);
    if (!o) {
        *err = bad_ref(ref);
        return NULL;
    }
    if (o->kind == SR_SYSTEM) {
        *err = kSRBadParameter;
        return NULL;
    }
    return o;
}

static sr_obj *system_of(int32_t ref, int16_t *err)
{
    sr_obj *o = obj_of(ref);
    if (!o) {
        *err = bad_ref(ref);
        return NULL;
    }
    if (o->kind != SR_SYSTEM) {
        *err = kSRNotARecSystem;
        return NULL;
    }
    return o;
}

static uint8_t *copy_guest(gaddr p, int32_t n)
{
    uint8_t *b = malloc(n > 0 ? (size_t)n : 1);
    if (n > 0)
        gget(b, p, (uint32_t)n);
    return b;
}

/* ---- The system ---- */

int16_t tb_SROpenRecognitionSystem(gaddr system, int32_t systemID)
{
    if (!system)
        return kSRBadParameter;
    wr32(system, 0);
    if (systemID != 0) /* kSRDefaultRecognitionSystemID */
        return kSRComponentNotFound;
    sr_obj *o = new_obj(SR_SYSTEM, NULL, 0);
    uint32_t r = o ? make_ref(o) : 0;
    if (!r) {
        if (o)
            sr_free(o);
        return kSROutOfMemory;
    }
    wr32(system, r);
    return noErr;
}

int16_t tb_SRCloseRecognitionSystem(int32_t system)
{
    int16_t e = noErr;
    if (!system_of(system, &e))
        return e;
    return tb_SRReleaseObject(system);
}

/* A recognizer cannot be made, so none is ever held. */
int16_t tb_SRNewRecognizer(int32_t system, gaddr recognizer, int32_t sourceID)
{
    int16_t e = noErr;
    if (!recognizer)
        return kSRBadParameter;
    wr32(recognizer, 0);
    if (!system_of(system, &e))
        return e;
    return kSRNotAvailable;
}

int16_t tb_SRReleaseObject(int32_t srObject)
{
    uint32_t i = (uint32_t)srObject - REF_TAG;
    if (i < 1 || i > nrefs)
        return kSRNotASpeechObject;
    if (!refs[i - 1])
        return released[i - 1] ? kSRAlreadyReleased : kSRNotASpeechObject;
    sr_obj *o = refs[i - 1];
    refs[i - 1] = NULL;
    released[i - 1] = true;
    drop(o);
    return noErr;
}

int16_t tb_SRGetReference(int32_t srObject, gaddr newObjectRef)
{
    sr_obj *o = obj_of(srObject);
    if (!newObjectRef)
        return kSRBadParameter;
    if (!o)
        return bad_ref(srObject);
    uint32_t r = make_ref(o);
    if (!r)
        return kSROutOfMemory;
    wr32(newObjectRef, r);
    return noErr;
}

/* ---- Properties ---- */

static uint32_t type_code(const sr_obj *o)
{
    return o->kind == SR_MODEL ? T("lmob") : o->kind == SR_PATH ? T("path") : o->kind == SR_PHRASE ? T("phra") : T("word");
}

int16_t tb_SRSetProperty(int32_t srObject, int32_t selector, gaddr property, int32_t propertyLen)
{
    sr_obj *o = obj_of(srObject);
    if (!o)
        return bad_ref(srObject);
    if (propertyLen < 0 || (!property && propertyLen))
        return kSRBadParameter;
    uint32_t sel = (uint32_t)selector;
    if (o->kind == SR_SYSTEM)
        return sel == T("clup") ? noErr : kSRCantSetProperty; /* cleanup on client exit: all go when we do */
    if (sel == T("spel")) {
        if (o->kind == SR_PATH)
            return kSRCantSetProperty;
        free(o->name);
        o->name = copy_guest(property, propertyLen);
        o->namelen = (uint32_t)propertyLen;
        return noErr;
    }
    if (sel == T("refc") && propertyLen == 4) {
        o->refcon = (int32_t)rd32(property);
        return noErr;
    }
    if ((sel == T("optl") || sel == T("enbl") || sel == T("rptb") || sel == T("rjbl")) && propertyLen == 1) {
        bool v = rd8(property) != 0;
        if (sel == T("optl"))
            o->optional = v;
        else if (sel == T("enbl"))
            o->enabled = v;
        else if (sel == T("rptb"))
            o->repeatable = v;
        else
            o->rejectable = v;
        return noErr;
    }
    if (sel == T("rjct") && propertyLen == 2) {
        o->level = rd16(property);
        return noErr;
    }
    return kSRCantSetProperty;
}

static int16_t give(gaddr buf, gaddr lenp, const void *data, uint32_t n)
{
    uint32_t room = lenp ? rd32(lenp) : 0;
    if (lenp)
        wr32(lenp, n);
    if (!buf)
        return noErr;
    if (n > room)
        return kSRBufferTooSmall;
    gput(buf, data, n);
    return noErr;
}

int16_t tb_SRGetProperty(int32_t srObject, int32_t selector, gaddr property, gaddr propertyLen)
{
    sr_obj *o = obj_of(srObject);
    if (!o)
        return bad_ref(srObject);
    if (!propertyLen)
        return kSRBadParameter;
    uint32_t sel = (uint32_t)selector;
    uint8_t b[4];
    if (o->kind == SR_SYSTEM) {
        if (sel == T("fbwn")) { /* feedback and listening modes: kSRNoFeedbackHasListenModes (no feedback window) */
            b[0] = b[1] = b[2] = 0, b[3] = 2;
            return give(property, propertyLen, b, 4);
        }
        return kSRCantGetProperty;
    }
    if (sel == T("spel") && o->kind != SR_PATH)
        return give(property, propertyLen, o->name, o->namelen);
    if (sel == T("lmtp")) {
        uint32_t t = type_code(o);
        b[0] = (uint8_t)(t >> 24), b[1] = (uint8_t)(t >> 16), b[2] = (uint8_t)(t >> 8), b[3] = (uint8_t)t;
        return give(property, propertyLen, b, 4);
    }
    if (sel == T("refc")) {
        uint32_t v = (uint32_t)o->refcon;
        b[0] = (uint8_t)(v >> 24), b[1] = (uint8_t)(v >> 16), b[2] = (uint8_t)(v >> 8), b[3] = (uint8_t)v;
        return give(property, propertyLen, b, 4);
    }
    if (sel == T("optl") || sel == T("enbl") || sel == T("rptb") || sel == T("rjbl")) {
        b[0] = sel == T("optl") ? o->optional : sel == T("enbl") ? o->enabled : sel == T("rptb") ? o->repeatable : o->rejectable;
        return give(property, propertyLen, b, 1);
    }
    if (sel == T("rjct")) {
        b[0] = (uint8_t)(o->level >> 8), b[1] = (uint8_t)o->level;
        return give(property, propertyLen, b, 2);
    }
    return kSRCantGetProperty;
}

/* ---- Language objects ---- */

static int16_t new_language(int32_t system, gaddr out, int kind, gaddr text, int32_t len)
{
    int16_t e = noErr;
    if (!out)
        return kSRBadParameter;
    wr32(out, 0);
    if (!system_of(system, &e))
        return e;
    if (len < 0 || (!text && len))
        return kSRBadParameter;
    uint8_t *name = copy_guest(text, len);
    sr_obj *o = new_obj(kind, name, (uint32_t)len);
    free(name);
    uint32_t r = o ? make_ref(o) : 0;
    if (!r) {
        if (o)
            sr_free(o);
        return kSROutOfMemory;
    }
    wr32(out, r);
    return noErr;
}

int16_t tb_SRNewLanguageModel(int32_t system, gaddr model, gaddr name, int32_t nameLength)
{
    return new_language(system, model, SR_MODEL, name, nameLength);
}
int16_t tb_SRNewPath(int32_t system, gaddr path) { return new_language(system, path, SR_PATH, 0, 0); }
int16_t tb_SRNewPhrase(int32_t system, gaddr phrase, gaddr text, int32_t textLength)
{
    return new_language(system, phrase, SR_PHRASE, text, textLength);
}
int16_t tb_SRNewWord(int32_t system, gaddr word, gaddr text, int32_t textLength)
{
    return new_language(system, word, SR_WORD, text, textLength);
}

int16_t tb_SRChangeLanguageObject(int32_t languageObject, gaddr text, int32_t textLength)
{
    int16_t e = noErr;
    sr_obj *o = language_of(languageObject, &e);
    if (!o)
        return e;
    if (o->kind == SR_PATH)
        return kSRCantSetProperty;
    if (textLength < 0 || (!text && textLength))
        return kSRBadParameter;
    free(o->name);
    o->name = copy_guest(text, textLength);
    o->namelen = (uint32_t)textLength;
    return noErr;
}

/* What a container may hold: models and paths hold anything language, a phrase words (and
 * models), a word nothing. */
static bool can_hold(const sr_obj *base, const sr_obj *add)
{
    if (base->kind == SR_WORD || base->kind == SR_SYSTEM || add->kind == SR_SYSTEM)
        return false;
    if (base->kind == SR_PHRASE)
        return add->kind == SR_WORD || add->kind == SR_MODEL;
    return true;
}

static bool contains(const sr_obj *base, const sr_obj *what, int depth)
{
    if (base == what)
        return true;
    for (int i = 0; depth < MAX_DEPTH && i < base->nkids; i++)
        if (contains(base->kids[i].o, what, depth + 1))
            return true;
    return false;
}

static int16_t add_child(sr_obj *base, sr_obj *add, int32_t refcon)
{
    if (!can_hold(base, add) || contains(add, base, 0) || base->nkids >= MAX_CHILDREN)
        return kSRCantAdd;
    base->kids = realloc(base->kids, (size_t)(base->nkids + 1) * sizeof *base->kids);
    base->kids[base->nkids++] = (child){ add, refcon };
    add->holds++;
    return noErr;
}

int16_t tb_SRAddLanguageObject(int32_t base, int32_t addon)
{
    int16_t e = noErr;
    sr_obj *b = language_of(base, &e);
    if (!b)
        return e;
    sr_obj *a = language_of(addon, &e);
    if (!a)
        return e;
    return add_child(b, a, a->refcon);
}

/* SRAddText(base, text, len, refCon): the text as a phrase (a word, if it is one) of the container */
int16_t tb_SRAddText(int32_t base, gaddr text, int32_t textLength, int32_t refCon)
{
    int16_t e = noErr;
    sr_obj *b = language_of(base, &e);
    if (!b)
        return e;
    if (textLength < 0 || (!text && textLength))
        return kSRBadParameter;
    uint8_t *name = copy_guest(text, textLength);
    bool space = false;
    for (int32_t i = 0; i < textLength; i++)
        space = space || name[i] == ' ';
    sr_obj *o = new_obj(space ? SR_PHRASE : SR_WORD, name, (uint32_t)textLength);
    free(name);
    if (!o)
        return kSROutOfMemory;
    o->refcon = refCon;
    e = add_child(b, o, refCon);
    if (e)
        sr_free(o);
    return e;
}

int16_t tb_SRRemoveLanguageObject(int32_t base, int32_t toRemove)
{
    int16_t e = noErr;
    sr_obj *b = language_of(base, &e);
    if (!b)
        return e;
    sr_obj *r = language_of(toRemove, &e);
    if (!r)
        return e;
    for (int i = 0; i < b->nkids; i++)
        if (b->kids[i].o == r) {
            drop(r);
            memmove(&b->kids[i], &b->kids[i + 1], (size_t)(b->nkids - i - 1) * sizeof *b->kids);
            b->nkids--;
            return noErr;
        }
    return kSRSubItemNotFound;
}

int16_t tb_SREmptyLanguageObject(int32_t languageObject)
{
    int16_t e = noErr;
    sr_obj *o = language_of(languageObject, &e);
    if (!o)
        return e;
    if (o->kind == SR_WORD)
        return kSRHasNoSubItems;
    for (int i = 0; i < o->nkids; i++)
        drop(o->kids[i].o);
    o->nkids = 0;
    return noErr;
}

int16_t tb_SRCountItems(int32_t container, gaddr count)
{
    int16_t e = noErr;
    sr_obj *o = language_of(container, &e);
    if (!o)
        return e;
    if (!count)
        return kSRBadParameter;
    if (o->kind == SR_WORD)
        return kSRHasNoSubItems;
    wr32(count, (uint32_t)o->nkids);
    return noErr;
}

int16_t tb_SRGetIndexedItem(int32_t container, gaddr item, int32_t index)
{
    int16_t e = noErr;
    sr_obj *o = language_of(container, &e);
    if (!o)
        return e;
    if (!item)
        return kSRBadParameter;
    wr32(item, 0);
    if (o->kind == SR_WORD)
        return kSRHasNoSubItems;
    if (index < 0 || index >= o->nkids)
        return kSRSubItemNotFound;
    uint32_t r = make_ref(o->kids[index].o);
    if (!r)
        return kSROutOfMemory;
    wr32(item, r);
    return noErr;
}

int16_t tb_SRSetIndexedItem(int32_t container, int32_t item, int32_t index)
{
    int16_t e = noErr;
    sr_obj *o = language_of(container, &e);
    if (!o)
        return e;
    sr_obj *it = language_of(item, &e);
    if (!it)
        return e;
    if (o->kind == SR_WORD)
        return kSRHasNoSubItems;
    if (index < 0 || index > o->nkids)
        return kSRSubItemNotFound;
    if (index == o->nkids)
        return add_child(o, it, it->refcon);
    if (!can_hold(o, it) || contains(it, o, 0))
        return kSRCantAdd;
    sr_obj *old = o->kids[index].o;
    o->kids[index] = (child){ it, it->refcon };
    it->holds++;
    drop(old);
    return noErr;
}

int16_t tb_SRRemoveIndexedItem(int32_t container, int32_t index)
{
    int16_t e = noErr;
    sr_obj *o = language_of(container, &e);
    if (!o)
        return e;
    if (o->kind == SR_WORD)
        return kSRHasNoSubItems;
    if (index < 0 || index >= o->nkids)
        return kSRSubItemNotFound;
    sr_obj *old = o->kids[index].o;
    memmove(&o->kids[index], &o->kids[index + 1], (size_t)(o->nkids - index - 1) * sizeof *o->kids);
    o->nkids--;
    drop(old);
    return noErr;
}

/* ---- Saving and reading ---- */

typedef struct {
    uint8_t *v;
    size_t n, cap;
} wbuf;
static void w_add(wbuf *b, const void *d, size_t n)
{
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 64;
        b->v = realloc(b->v, b->cap);
    }
    if (n)
        memcpy(b->v + b->n, d, n);
    b->n += n;
}
static void w32(wbuf *b, uint32_t v)
{
    uint8_t x[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    w_add(b, x, 4);
}

static void put_node(wbuf *b, const sr_obj *o, int32_t refcon)
{
    w32(b, type_code(o));
    w32(b, (uint32_t)refcon);
    uint8_t f = (uint8_t)(o->optional | o->enabled << 1 | o->repeatable << 2 | o->rejectable << 3);
    w_add(b, &f, 1);
    uint8_t lv[2] = { (uint8_t)(o->level >> 8), (uint8_t)o->level };
    w_add(b, lv, 2);
    w32(b, o->namelen);
    w_add(b, o->name, o->namelen);
    w32(b, (uint32_t)o->nkids);
    for (int i = 0; i < o->nkids; i++)
        put_node(b, o->kids[i].o, o->kids[i].refcon);
}

static wbuf serialize(const sr_obj *o)
{
    wbuf b = { 0 };
    w32(&b, T("lmfm")); /* kSRLanguageModelFormat */
    w32(&b, 1);
    put_node(&b, o, o->refcon);
    return b;
}

typedef struct {
    const uint8_t *p;
    size_t n, at;
    bool bad;
} rbuf;
static uint32_t r32(rbuf *r)
{
    if (r->at + 4 > r->n) {
        r->bad = true;
        return 0;
    }
    uint32_t v = (uint32_t)r->p[r->at] << 24 | (uint32_t)r->p[r->at + 1] << 16 | (uint32_t)r->p[r->at + 2] << 8 | r->p[r->at + 3];
    r->at += 4;
    return v;
}

static sr_obj *get_node(rbuf *r, int depth, int32_t *refcon)
{
    uint32_t type = r32(r);
    *refcon = (int32_t)r32(r);
    if (r->at + 3 > r->n || depth > MAX_DEPTH) {
        r->bad = true;
        return NULL;
    }
    uint8_t f = r->p[r->at++];
    uint16_t level = (uint16_t)(r->p[r->at] << 8 | r->p[r->at + 1]);
    r->at += 2;
    uint32_t nl = r32(r);
    if (r->bad || r->at + nl > r->n) {
        r->bad = true;
        return NULL;
    }
    int kind = type == T("lmob") ? SR_MODEL : type == T("path") ? SR_PATH : type == T("phra") ? SR_PHRASE : type == T("word") ? SR_WORD : -1;
    if (kind < 0) {
        r->bad = true;
        return NULL;
    }
    sr_obj *o = new_obj(kind, r->p + r->at, nl);
    r->at += nl;
    o->optional = f & 1, o->enabled = f >> 1 & 1, o->repeatable = f >> 2 & 1, o->rejectable = f >> 3 & 1;
    o->level = level;
    o->refcon = *refcon;
    uint32_t nk = r32(r);
    if (nk > MAX_CHILDREN)
        r->bad = true;
    for (uint32_t i = 0; i < nk && !r->bad; i++) {
        int32_t rc;
        sr_obj *k = get_node(r, depth + 1, &rc);
        if (!k)
            break;
        o->kids = realloc(o->kids, (size_t)(o->nkids + 1) * sizeof *o->kids);
        o->kids[o->nkids++] = (child){ k, rc };
        k->holds++;
    }
    if (r->bad) {
        sr_free(o);
        return NULL;
    }
    return o;
}

static int16_t from_bytes(int32_t system, gaddr out, const uint8_t *data, size_t n)
{
    int16_t e = noErr;
    if (!out)
        return kSRBadParameter;
    wr32(out, 0);
    if (!system_of(system, &e))
        return e;
    rbuf r = { data, n, 0, false };
    if (r32(&r) != T("lmfm") || r32(&r) != 1)
        return kSRCantReadLanguageObject;
    int32_t rc;
    sr_obj *o = get_node(&r, 0, &rc);
    if (!o)
        return kSRCantReadLanguageObject;
    uint32_t ref = make_ref(o);
    if (!ref) {
        sr_free(o);
        return kSROutOfMemory;
    }
    wr32(out, ref);
    return noErr;
}

int16_t tb_SRPutLanguageObjectIntoHandle(int32_t languageObject, gaddr lobjHandle)
{
    int16_t e = noErr;
    sr_obj *o = language_of(languageObject, &e);
    if (!o)
        return e;
    if (!lobjHandle || !rd32(lobjHandle))
        return kSRBadParameter;
    wbuf b = serialize(o);
    tb_SetHandleSize(lobjHandle, (int32_t)b.n);
    if (tb_GetHandleSize(lobjHandle) != (int32_t)b.n) {
        free(b.v);
        return kSROutOfMemory;
    }
    gput(rd32(lobjHandle), b.v, (uint32_t)b.n);
    free(b.v);
    return noErr;
}

int16_t tb_SRNewLanguageObjectFromHandle(int32_t system, gaddr languageObject, gaddr lObjHandle)
{
    if (!lObjHandle || !rd32(lObjHandle)) {
        if (languageObject)
            wr32(languageObject, 0);
        return kSRBadParameter;
    }
    int32_t n = tb_GetHandleSize(lObjHandle);
    uint8_t *d = copy_guest(rd32(lObjHandle), n);
    int16_t e = from_bytes(system, languageObject, d, (size_t)n);
    free(d);
    return e;
}

int16_t tb_SRPutLanguageObjectIntoDataFile(int32_t languageObject, int16_t fRefNum)
{
    int16_t e = noErr;
    sr_obj *o = language_of(languageObject, &e);
    if (!o)
        return e;
    wbuf b = serialize(o);
    gaddr buf = tb_NewPtr((int32_t)b.n, false, true);
    if (!buf) {
        free(b.v);
        return kSROutOfMemory;
    }
    gput(buf, b.v, (uint32_t)b.n);
    uint32_t actual = 0;
    e = fm_fork_io_mark(fRefNum, true, (uint32_t)b.n, buf, &actual);
    tb_DisposePtr(buf);
    free(b.v);
    return e;
}

int16_t tb_SRNewLanguageObjectFromDataFile(int32_t system, gaddr languageObject, int16_t fRefNum)
{
    uint32_t total = 0;
    gaddr buf = 0;
    uint8_t *d = NULL;
    int16_t e = fm_fork_remaining(fRefNum, &total);
    if (!e && total > 0 && total < (64u << 20)) {
        buf = tb_NewPtr((int32_t)total, false, true);
        uint32_t actual = 0;
        e = buf ? fm_fork_io_mark(fRefNum, false, total, buf, &actual) : kSROutOfMemory;
        if (!e) {
            d = copy_guest(buf, (int32_t)actual);
            e = from_bytes(system, languageObject, d, actual);
        }
    } else if (!e) {
        e = kSRCantReadLanguageObject;
        if (languageObject)
            wr32(languageObject, 0);
    }
    free(d);
    if (buf)
        tb_DisposePtr(buf);
    return e;
}

/* ---- Recognizers: none can be made, so every call that needs one finds none ---- */

static int16_t not_a_recognizer(int32_t ref)
{
    sr_obj *o = obj_of(ref);
    return o ? kSRBadParameter : bad_ref(ref);
}

int16_t tb_SRStartListening(int32_t recognizer) { return not_a_recognizer(recognizer); }
int16_t tb_SRStopListening(int32_t recognizer) { return not_a_recognizer(recognizer); }
int16_t tb_SRSetLanguageModel(int32_t recognizer, int32_t languageModel) { return not_a_recognizer(recognizer); }
int16_t tb_SRGetLanguageModel(int32_t recognizer, gaddr languageModel)
{
    if (languageModel)
        wr32(languageModel, 0);
    return not_a_recognizer(recognizer);
}
int16_t tb_SRContinueRecognition(int32_t recognizer) { return not_a_recognizer(recognizer); }
int16_t tb_SRCancelRecognition(int32_t recognizer) { return not_a_recognizer(recognizer); }
int16_t tb_SRIdle(void) { return noErr; }
int16_t tb_SRDrawText(int32_t recognizer, gaddr dispText, int32_t dispLength) { return not_a_recognizer(recognizer); }
int16_t tb_SRDrawRecognizedText(int32_t recognizer, gaddr dispText, int32_t dispLength) { return not_a_recognizer(recognizer); }
int16_t tb_SRSpeakText(int32_t recognizer, gaddr speakText, int32_t speakLength) { return not_a_recognizer(recognizer); }
int16_t tb_SRSpeakAndDrawText(int32_t recognizer, gaddr text, int32_t textLength) { return not_a_recognizer(recognizer); }
int16_t tb_SRStopSpeech(int32_t recognizer) { return not_a_recognizer(recognizer); }
int8_t tb_SRSpeechBusy(int32_t recognizer) { return 0; }
int16_t tb_SRProcessBegin(int32_t recognizer, int8_t failed) { return not_a_recognizer(recognizer); }
int16_t tb_SRProcessEnd(int32_t recognizer, int8_t failed) { return not_a_recognizer(recognizer); }
