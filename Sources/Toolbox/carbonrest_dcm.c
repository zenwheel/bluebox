/*
 * The Dictionary Manager (Dictionary.h): the dictionaries Japanese input
 * methods and Language Analysis used, files of records with typed fields.
 * (The System 7 Dictionary Manager, $AA53, is dictionary.c.)
 *
 * A dictionary is a file of type 'dict' with the header of the interface
 * (signature, version, size, access method) and our own body: its fields
 * (tag, type, largest size, attributes, default, find methods), properties
 * and records (a unique ID and the data of each field).  There is one access
 * method, Blue Box's.  The dictionary is kept whole in memory while
 * registered and written back at each commit.
 *
 * Records are found and ordered by one field, the key, as the calls name it:
 * by its bytes.  Field data goes in and out as an AERecord keyed by field
 * tag.  A change needs the dictionary's write access, got from its ref and
 * given up committing or not (not: the changes since are undone).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "carbonrest.h"
#include "filemgr_internal.h"

enum {
    dcmParamErr = -7100,
    dcmNotDictionaryErr = -7101,
    dcmBadDictionaryErr = -7102,
    dcmPermissionErr = -7103,
    dcmDictionaryNotOpenErr = -7104,
    dcmDictionaryBusyErr = -7105,
    dcmNoRecordErr = -7108,
    dcmDupRecordErr = -7109,
    dcmNecessaryFieldErr = -7110,
    dcmBadFieldInfoErr = -7111,
    dcmBadFieldTypeErr = -7112,
    dcmNoFieldErr = -7113,
    dcmBadKeyErr = -7115,
    dcmBadDataSizeErr = -7117,
    dcmBadFindMethodErr = -7118,
    dcmBadPropertyErr = -7119,
    dcmNoAccessMethodErr = -7122,
    dcmIterationCompleteErr = -7126,
    dcmBufferOverflowErr = -7127,
};
#define T(s) fourcc(s)
enum {
    kDCMIndexedFieldMask = 1, kDCMRequiredFieldMask = 2, kDCMIdentifyFieldMask = 4, kDCMFixedSizeFieldMask = 8,
};
enum { kDCMReadOnlyDictionary = 0, kDCMReadWriteDictionary = 1 };
enum { kAEEquals = 0x3D202020, kAEBeginsWith = 0x62677774, kAEContains = 0x636F6E74, kAEEndsWith = 0x656E6473 };
enum { DICT_TAG = 0x44430000, REF_TAG = 0x44520000, METHOD_ID = 0x44414D01, ITER_TAG = 0x44490000 };
enum { MAX_DICTS = 32, MAX_REFS = 64, MAX_ITERS = 64, MAX_FIND = 8 };
#define HEADER_MAGIC 0x64696374u /* 'dict' */

typedef struct {
    uint32_t tag, type, maxsize, attrs;
    uint32_t deftype;
    uint8_t *def;
    uint32_t deflen;
    bool has_default;
    uint32_t nfind, find[MAX_FIND];
} dfield;
typedef struct {
    uint32_t tag, type, len;
    uint8_t *data;
} dvalue;
typedef struct {
    uint32_t uid;
    dvalue *v;
    int nv;
} drecord;
typedef struct {
    uint32_t tag;
    uint8_t *data;
    uint32_t len;
} dprop;

typedef struct {
    bool used;
    uint8_t spec[70];
    char path[1024];
    int16_t script;
    bool invisible;
    dfield *f;
    int nf;
    drecord *r;
    int nr;
    dprop *p;
    int np;
    uint32_t next_uid;
    int write_ref; /* index of the ref holding write access, or -1 */
    drecord *snap; /* the records when write access was taken */
    int nsnap;
    uint32_t snap_uid;
} dict;
typedef struct {
    bool used;
    int dict;
} dref;
typedef struct {
    bool used;
    int kind; /* 0 dictionaries, 1 access methods, 2 found records */
    uint32_t *ids;
    int n, at;
    /* found records */
    drecord *found; /* copies: uid and the prefetched values */
    uint32_t *keylen;
    uint8_t **keys;
} diter;

static dict dicts[MAX_DICTS];
static dref refs[MAX_REFS];
static diter iters[MAX_ITERS];

/* ---- Memory ---- */

static void free_value(dvalue *v) { free(v->data); }
static void free_record(drecord *r)
{
    for (int i = 0; i < r->nv; i++)
        free_value(&r->v[i]);
    free(r->v);
}
static drecord copy_record(const drecord *s)
{
    drecord c = { s->uid, calloc(s->nv ? (size_t)s->nv : 1, sizeof(dvalue)), s->nv };
    for (int i = 0; i < s->nv; i++) {
        c.v[i] = s->v[i];
        c.v[i].data = malloc(s->v[i].len ? s->v[i].len : 1);
        memcpy(c.v[i].data, s->v[i].data, s->v[i].len);
    }
    return c;
}
static void free_records(drecord *r, int n)
{
    for (int i = 0; i < n; i++)
        free_record(&r[i]);
    free(r);
}

static void free_dict(dict *d)
{
    for (int i = 0; i < d->nf; i++)
        free(d->f[i].def);
    free(d->f);
    free_records(d->r, d->nr);
    for (int i = 0; i < d->np; i++)
        free(d->p[i].data);
    free(d->p);
    free_records(d->snap, d->nsnap);
    memset(d, 0, sizeof *d);
}

static void iter_free(diter *it)
{
    free(it->ids);
    if (it->found)
        free_records(it->found, it->n);
    for (int i = 0; i < it->n && it->keys; i++)
        free(it->keys[i]);
    free(it->keys);
    free(it->keylen);
    memset(it, 0, sizeof *it);
}

void rest_dcm_reset(void)
{
    for (int i = 0; i < MAX_DICTS; i++)
        free_dict(&dicts[i]);
    for (int i = 0; i < MAX_ITERS; i++)
        iter_free(&iters[i]);
    memset(refs, 0, sizeof refs);
}

static dict *dict_of(uint32_t id)
{
    uint32_t i = id - DICT_TAG;
    return i >= 1 && i <= MAX_DICTS && dicts[i - 1].used ? &dicts[i - 1] : NULL;
}

static dict *dict_of_ref(uint32_t ref, int *ri)
{
    uint32_t i = ref - REF_TAG;
    if (i < 1 || i > MAX_REFS || !refs[i - 1].used)
        return NULL;
    if (ri)
        *ri = (int)i - 1;
    return &dicts[refs[i - 1].dict];
}

static dfield *field_of(dict *d, uint32_t tag)
{
    for (int i = 0; i < d->nf; i++)
        if (d->f[i].tag == tag)
            return &d->f[i];
    return NULL;
}

static dvalue *value_of(const drecord *r, uint32_t tag)
{
    for (int i = 0; i < r->nv; i++)
        if (r->v[i].tag == tag)
            return &r->v[i];
    return NULL;
}

static dprop *prop_of(dict *d, uint32_t tag)
{
    for (int i = 0; i < d->np; i++)
        if (d->p[i].tag == tag)
            return &d->p[i];
    return NULL;
}

static void set_prop(dict *d, uint32_t tag, const void *data, uint32_t len)
{
    dprop *p = prop_of(d, tag);
    if (!p) {
        d->p = realloc(d->p, (size_t)(d->np + 1) * sizeof *d->p);
        p = &d->p[d->np++];
        p->tag = tag;
        p->data = NULL;
    }
    free(p->data);
    p->data = malloc(len ? len : 1);
    memcpy(p->data, data, len);
    p->len = len;
}

static void set_prop32(dict *d, uint32_t tag, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    set_prop(d, tag, b, 4);
}

/* ---- Persistence ---- */

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

static bool save_dict(dict *d)
{
    wbuf b = { 0 };
    w32(&b, HEADER_MAGIC);
    w32(&b, 2);
    w32(&b, 4 + 4 + 4 + 64); /* headerSize: through the Str63 */
    uint8_t am[64] = { 0 };
    const char *name = "Blue Box Dictionary";
    am[0] = (uint8_t)strlen(name);
    memcpy(am + 1, name, am[0]);
    w_add(&b, am, 64);
    w32(&b, (uint32_t)d->script & 0xFFFF);
    w32(&b, d->invisible);
    w32(&b, d->next_uid);
    w32(&b, (uint32_t)d->nf);
    for (int i = 0; i < d->nf; i++) {
        dfield *f = &d->f[i];
        w32(&b, f->tag), w32(&b, f->type), w32(&b, f->maxsize), w32(&b, f->attrs);
        w32(&b, f->has_default), w32(&b, f->deftype), w32(&b, f->deflen);
        w_add(&b, f->def, f->deflen);
        w32(&b, f->nfind);
        for (uint32_t k = 0; k < f->nfind; k++)
            w32(&b, f->find[k]);
    }
    w32(&b, (uint32_t)d->np);
    for (int i = 0; i < d->np; i++) {
        w32(&b, d->p[i].tag), w32(&b, d->p[i].len);
        w_add(&b, d->p[i].data, d->p[i].len);
    }
    w32(&b, (uint32_t)d->nr);
    for (int i = 0; i < d->nr; i++) {
        drecord *r = &d->r[i];
        w32(&b, r->uid), w32(&b, (uint32_t)r->nv);
        for (int k = 0; k < r->nv; k++) {
            w32(&b, r->v[k].tag), w32(&b, r->v[k].type), w32(&b, r->v[k].len);
            w_add(&b, r->v[k].data, r->v[k].len);
        }
    }
    FILE *f = fopen(d->path, "wb");
    bool ok = f && fwrite(b.v, 1, b.n, f) == b.n;
    if (f)
        ok = fclose(f) == 0 && ok;
    free(b.v);
    return ok;
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
static uint8_t *rbytes(rbuf *r, uint32_t n)
{
    if (r->at + n > r->n || n > (64u << 20)) {
        r->bad = true;
        return calloc(1, 1);
    }
    uint8_t *c = malloc(n ? n : 1);
    memcpy(c, r->p + r->at, n);
    r->at += n;
    return c;
}

/* Read the file at d->path; dcmNotDictionaryErr / dcmBadDictionaryErr. */
static int16_t load_dict(dict *d)
{
    FILE *f = fopen(d->path, "rb");
    if (!f)
        return fnfErr;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(size > 0 ? (size_t)size : 1);
    size_t got = size > 0 ? fread(buf, 1, (size_t)size, f) : 0;
    fclose(f);
    rbuf r = { buf, got, 0, false };
    if (got < 12 || r32(&r) != HEADER_MAGIC) {
        free(buf);
        return dcmNotDictionaryErr;
    }
    uint32_t version = r32(&r);
    uint32_t hsize = r32(&r);
    if (version != 2 || hsize < 12 || hsize > got) {
        free(buf);
        return dcmBadDictionaryErr;
    }
    r.at = hsize;
    d->script = (int16_t)r32(&r);
    d->invisible = r32(&r) != 0;
    d->next_uid = r32(&r);
    uint32_t nf = r32(&r);
    if (nf > 1024) {
        free(buf);
        return dcmBadDictionaryErr;
    }
    d->f = calloc(nf ? nf : 1, sizeof *d->f);
    for (uint32_t i = 0; i < nf && !r.bad; i++) {
        dfield *fl = &d->f[d->nf++];
        fl->tag = r32(&r), fl->type = r32(&r), fl->maxsize = r32(&r), fl->attrs = r32(&r);
        fl->has_default = r32(&r) != 0;
        fl->deftype = r32(&r);
        fl->deflen = r32(&r);
        fl->def = rbytes(&r, fl->deflen);
        fl->nfind = r32(&r);
        if (fl->nfind > MAX_FIND)
            r.bad = true;
        for (uint32_t k = 0; k < fl->nfind && !r.bad; k++)
            fl->find[k] = r32(&r);
    }
    uint32_t np = r32(&r);
    for (uint32_t i = 0; i < np && !r.bad; i++) {
        uint32_t tag = r32(&r), len = r32(&r);
        uint8_t *data = rbytes(&r, len);
        if (!r.bad)
            set_prop(d, tag, data, len);
        free(data);
    }
    uint32_t nr = r32(&r);
    if (nr > (1u << 22))
        r.bad = true;
    d->r = calloc(nr && !r.bad ? nr : 1, sizeof *d->r);
    for (uint32_t i = 0; i < nr && !r.bad; i++) {
        drecord *rec = &d->r[d->nr++];
        rec->uid = r32(&r);
        uint32_t nv = r32(&r);
        if (nv > 1024) {
            r.bad = true;
            break;
        }
        rec->v = calloc(nv ? nv : 1, sizeof *rec->v);
        for (uint32_t k = 0; k < nv && !r.bad; k++) {
            dvalue *v = &rec->v[rec->nv++];
            v->tag = r32(&r), v->type = r32(&r), v->len = r32(&r);
            v->data = rbytes(&r, v->len);
        }
    }
    free(buf);
    return r.bad ? dcmBadDictionaryErr : noErr;
}

/* ---- Registry ---- */

static bool spec_path(gaddr spec, char *out, size_t size, bool *exists)
{
    int16_t e = fm_resolve((int16_t)rd16(spec), (int32_t)rd32(spec + 2), spec + 6, out, size);
    if (e != noErr && !(e == fnfErr && out[0]))
        return false;
    *exists = e == noErr && fm_host_exists(out);
    return true;
}

static dict *new_dict(uint32_t *id)
{
    for (int i = 0; i < MAX_DICTS; i++)
        if (!dicts[i].used) {
            memset(&dicts[i], 0, sizeof dicts[i]);
            dicts[i].used = true;
            dicts[i].write_ref = -1;
            dicts[i].next_uid = 1;
            *id = DICT_TAG + (uint32_t)i + 1;
            return &dicts[i];
        }
    return NULL;
}

static void init_properties(dict *d)
{
    if (!prop_of(d, T("amtd")))
        set_prop32(d, T("amtd"), METHOD_ID);
    if (!prop_of(d, T("perm")))
        set_prop32(d, T("perm"), kDCMReadWriteDictionary);
    if (!prop_of(d, T("list")))
        set_prop32(d, T("list"), 0);
    if (!prop_of(d, T("pcls")))
        set_prop32(d, T("pcls"), 0);
}

/* ---- Field info records (AERecords) ---- */

static int16_t put_param(gaddr rec, uint32_t key, uint32_t type, const void *data, uint32_t len)
{
    gaddr p = tb_NewPtr(len ? (int32_t)len : 1, false, true);
    if (!p)
        return memFullErr;
    gput(p, data, len);
    int16_t e = tb_AEPutParamPtr(rec, (int32_t)key, (int32_t)type, p, (int32_t)len);
    tb_DisposePtr(p);
    return e;
}

static void be32put(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

/* A parameter's data of a record, malloc'd (NULL if absent). */
static uint8_t *get_param(gaddr rec, uint32_t key, uint32_t *type, uint32_t *len)
{
    gaddr d = tb_NewPtr(8, false, true);
    if (!d)
        return NULL;
    uint8_t *out = NULL;
    if (tb_AEGetParamDesc(rec, (int32_t)key, (int32_t)T("****"), d) == noErr) {
        gaddr h = rd32(d + 4);
        uint32_t n = h ? (uint32_t)tb_GetHandleSize(h) : 0;
        out = malloc(n ? n : 1);
        if (n)
            gget(out, rd32(h), n);
        *len = n;
        *type = rd32(d);
        tb_AEDisposeDesc(d);
    }
    tb_DisposePtr(d);
    return out;
}

static uint32_t be32get(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* A field from a field info record. */
static int16_t field_from_record(gaddr rec, dfield *f)
{
    memset(f, 0, sizeof *f);
    uint32_t type, len, tl = 0, ftl = 0;
    uint8_t *tg = get_param(rec, T("ftag"), &type, &tl);
    uint8_t *ty = get_param(rec, T("ftyp"), &type, &ftl);
    if (!tg || tl != 4 || !ty || ftl != 4) {
        free(tg);
        free(ty);
        return dcmBadFieldInfoErr;
    }
    f->tag = be32get(tg);
    f->type = be32get(ty);
    free(tg);
    free(ty);
    uint8_t *mx = get_param(rec, T("mrsz"), &type, &len);
    if (mx && len == 4)
        f->maxsize = be32get(mx);
    free(mx);
    uint8_t *at = get_param(rec, T("fatr"), &type, &len);
    if (at && len == 4)
        f->attrs = be32get(at);
    free(at);
    f->def = get_param(rec, T("fdef"), &f->deftype, &f->deflen);
    f->has_default = f->def != NULL;
    if (!f->def)
        f->def = calloc(1, 1);
    gaddr list = tb_NewPtr(8, false, true), item = tb_NewPtr(8, false, true);
    if (list && item && tb_AEGetParamDesc(rec, (int32_t)T("ffnd"), (int32_t)T("list"), list) == noErr) {
        gaddr cnt = tb_NewPtr(4, false, true);
        if (cnt && tb_AECountItems(list, cnt) == noErr)
            for (uint32_t i = 1; i <= rd32(cnt) && f->nfind < MAX_FIND; i++) {
                if (tb_AEGetNthDesc(list, (int32_t)i, (int32_t)T("****"), 0, item) == noErr) {
                    gaddr h = rd32(item + 4);
                    if (h && tb_GetHandleSize(h) == 4)
                        f->find[f->nfind++] = rd32(rd32(h));
                    tb_AEDisposeDesc(item);
                }
            }
        if (cnt)
            tb_DisposePtr(cnt);
        tb_AEDisposeDesc(list);
    }
    if (list)
        tb_DisposePtr(list);
    if (item)
        tb_DisposePtr(item);
    return noErr;
}

static int16_t record_from_field(const dfield *f, gaddr out)
{
    int16_t e = tb_AECreateList(0, 0, 1, out);
    uint8_t b[4];
    if (!e) {
        be32put(b, f->tag);
        e = put_param(out, T("ftag"), T("type"), b, 4);
    }
    if (!e) {
        be32put(b, f->type);
        e = put_param(out, T("ftyp"), T("type"), b, 4);
    }
    if (!e) {
        be32put(b, f->maxsize);
        e = put_param(out, T("mrsz"), T("long"), b, 4);
    }
    if (!e) {
        be32put(b, f->attrs);
        e = put_param(out, T("fatr"), T("fatr"), b, 4);
    }
    if (!e && f->has_default)
        e = put_param(out, T("fdef"), f->deftype, f->def, f->deflen);
    if (!e && f->nfind) {
        gaddr list = tb_NewPtr(8, false, true);
        e = list ? tb_AECreateList(0, 0, 0, list) : memFullErr;
        for (uint32_t i = 0; !e && i < f->nfind; i++) {
            be32put(b, f->find[i]);
            gaddr p = tb_NewPtr(4, false, true);
            gput(p, b, 4);
            e = tb_AEPutPtr(list, 0, (int32_t)T("fmth"), p, 4);
            tb_DisposePtr(p);
        }
        if (!e)
            e = tb_AEPutParamDesc(out, (int32_t)T("ffnd"), list);
        if (list) {
            tb_AEDisposeDesc(list);
            tb_DisposePtr(list);
        }
    }
    if (e)
        tb_AEDisposeDesc(out);
    return e;
}

/* ---- Records ---- */

static int sorted_cmp_tag;
static int cmp_records(const void *a, const void *b)
{
    const drecord *x = a, *y = b;
    dvalue *vx = value_of(x, (uint32_t)sorted_cmp_tag), *vy = value_of(y, (uint32_t)sorted_cmp_tag);
    uint32_t lx = vx ? vx->len : 0, ly = vy ? vy->len : 0;
    int c = memcmp(vx ? vx->data : (uint8_t *)"", vy ? vy->data : (uint8_t *)"", lx < ly ? lx : ly);
    if (c)
        return c;
    if (lx != ly)
        return lx < ly ? -1 : 1;
    return x->uid < y->uid ? -1 : x->uid > y->uid;
}

/* The records' indices in the order of a key field. */
static int *sorted_order(dict *d, uint32_t tag)
{
    drecord *tmp = malloc((size_t)(d->nr ? d->nr : 1) * sizeof *tmp);
    int *order = malloc((size_t)(d->nr ? d->nr : 1) * sizeof *order);
    /* sort copies of the headers with their index in uid-less form: use a parallel array */
    for (int i = 0; i < d->nr; i++)
        tmp[i] = d->r[i];
    sorted_cmp_tag = (int)tag;
    qsort(tmp, (size_t)d->nr, sizeof *tmp, cmp_records);
    for (int i = 0; i < d->nr; i++)
        for (int k = 0; k < d->nr; k++)
            if (d->r[k].uid == tmp[i].uid) {
                order[i] = k;
                break;
            }
    free(tmp);
    return order;
}

static bool key_ok(dict *d, uint32_t tag, int16_t *err)
{
    if (!field_of(d, tag)) {
        *err = dcmNoFieldErr;
        return false;
    }
    return true;
}

/* The record with a key (and unique ID, when given); index or -1. */
static int find_record(dict *d, uint32_t tag, uint32_t keylen, const uint8_t *key, uint32_t uid)
{
    for (int i = 0; i < d->nr; i++) {
        dvalue *v = value_of(&d->r[i], tag);
        if (v && v->len == keylen && !memcmp(v->data, key, keylen) && (!uid || d->r[i].uid == uid))
            return i;
    }
    return -1;
}

static uint8_t *host_key(gaddr key, uint32_t size)
{
    uint8_t *k = malloc(size ? size : 1);
    if (size)
        gget(k, key, size);
    return k;
}

/* The values of a dataList (an AERecord) into a record. */
static int16_t values_from_list(dict *d, gaddr list, drecord *rec)
{
    gaddr cnt = tb_NewPtr(4, false, true), item = tb_NewPtr(8, false, true), key = tb_NewPtr(4, false, true);
    int16_t e = noErr;
    if (!cnt || !item || !key)
        e = memFullErr;
    if (!e && list && rd32(list) != T("null") && tb_AECountItems(list, cnt) == noErr)
        for (uint32_t i = 1; !e && i <= rd32(cnt); i++) {
            if ((e = tb_AEGetNthDesc(list, (int32_t)i, (int32_t)T("****"), key, item)))
                break;
            uint32_t tag = rd32(key);
            dfield *f = field_of(d, tag);
            gaddr h = rd32(item + 4);
            uint32_t n = h ? (uint32_t)tb_GetHandleSize(h) : 0, type = rd32(item);
            if (!f) {
                e = dcmNoFieldErr;
            } else if (f->maxsize && n > f->maxsize) {
                e = dcmBadDataSizeErr;
            } else if ((f->attrs & kDCMFixedSizeFieldMask) && f->maxsize && n != f->maxsize) {
                e = dcmBadDataSizeErr;
            } else {
                dvalue v = { tag, type, n, malloc(n ? n : 1) };
                if (n)
                    gget(v.data, rd32(h), n);
                rec->v = realloc(rec->v, (size_t)(rec->nv + 1) * sizeof *rec->v);
                rec->v[rec->nv++] = v;
            }
            tb_AEDisposeDesc(item);
        }
    if (cnt)
        tb_DisposePtr(cnt);
    if (item)
        tb_DisposePtr(item);
    if (key)
        tb_DisposePtr(key);
    return e;
}

/* The tags given as a record's values (data AERecord) out of a stored record. */
static int16_t list_from_values(const drecord *rec, int ntags, const uint32_t *tags, gaddr out)
{
    int16_t e = tb_AECreateList(0, 0, 1, out);
    for (int i = 0; !e && i < rec->nv; i++) {
        bool want = ntags == 0;
        for (int k = 0; k < ntags; k++)
            want = want || tags[k] == rec->v[i].tag;
        if (want)
            e = put_param(out, rec->v[i].tag, rec->v[i].type, rec->v[i].data, rec->v[i].len);
    }
    return e;
}

static bool matches(uint32_t method, const uint8_t *val, uint32_t vl, const uint8_t *key, uint32_t kl)
{
    switch (method) {
    case kAEEquals:
        return vl == kl && !memcmp(val, key, kl);
    case kAEBeginsWith:
        return vl >= kl && !memcmp(val, key, kl);
    case kAEEndsWith:
        return vl >= kl && !memcmp(val + vl - kl, key, kl);
    case kAEContains:
        for (uint32_t i = 0; i + kl <= vl; i++)
            if (!memcmp(val + i, key, kl))
                return true;
        return kl == 0;
    case 0x66747269: /* 'ftri': the key begins with the value */
        return vl <= kl && !memcmp(key, val, vl);
    case 0x62747269: /* 'btri': the key ends with the value */
        return vl <= kl && !memcmp(key + kl - vl, val, vl);
    }
    return false;
}

/* ---- The calls ---- */

static void dcm_DCMLibraryVersion(ppc_cpu *c) { RET(0x00010000); }

static bool access_method_ok(uint32_t id) { return id == METHOD_ID || id == 0; }

/* DCMNewDictionary(accessMethodID, file, scriptTag, listOfFieldInfoRecords, invisible, capacity, &id) */
static void dcm_DCMNewDictionary(ppc_cpu *c)
{
    gaddr file = A(1), list = A(3), out = A(6);
    if (!file || !out)
        RET(dcmParamErr);
    wr32(out, 0);
    if (!access_method_ok(A(0)))
        RET(dcmNoAccessMethodErr);
    char path[1024];
    bool exists;
    if (!spec_path(file, path, sizeof path, &exists))
        RET(fnfErr);
    if (exists)
        RET(rest_dupFNErr);
    uint32_t id;
    dict *d = new_dict(&id);
    if (!d)
        RET(memFullErr);
    snprintf(d->path, sizeof d->path, "%s", path);
    gget(d->spec, file, 70);
    d->script = S16(2);
    d->invisible = BOOL(4);
    int16_t e = noErr;
    gaddr cnt = tb_NewPtr(4, false, true), item = tb_NewPtr(8, false, true);
    if (!list || !cnt || !item || tb_AECountItems(list, cnt) != noErr || rd32(cnt) == 0)
        e = dcmBadFieldInfoErr;
    for (uint32_t i = 1; !e && i <= rd32(cnt); i++) {
        if ((e = tb_AEGetNthDesc(list, (int32_t)i, (int32_t)T("****"), 0, item)))
            break;
        dfield f;
        e = field_from_record(item, &f);
        if (!e && (f.type == T("****") || !f.tag || f.tag == T("****")))
            e = dcmBadFieldTypeErr;
        if (!e && field_of(d, f.tag))
            e = dcmBadFieldInfoErr;
        if (!e) {
            d->f = realloc(d->f, (size_t)(d->nf + 1) * sizeof *d->f);
            d->f[d->nf++] = f;
        } else
            free(f.def);
        tb_AEDisposeDesc(item);
    }
    if (cnt)
        tb_DisposePtr(cnt);
    if (item)
        tb_DisposePtr(item);
    if (!e) {
        init_properties(d);
        uint8_t finfo[16] = { 'd', 'i', 'c', 't', 'd', 'c', 'm', 'g', 0 };
        e = fm_create_file(path);
        if (!e)
            fm_set_info(path, finfo, NULL, 0, 0);
        if (!e && !save_dict(d))
            e = ioErr;
    }
    if (e) {
        free_dict(d);
        RET((int32_t)e);
    }
    wr32(out, id);
    RET(noErr);
}

/* DCMDeriveNewDictionary(src, file, scriptTag, invisible, capacity, &id): the fields, no records */
static void dcm_DCMDeriveNewDictionary(ppc_cpu *c)
{
    dict *s = dict_of(A(0));
    gaddr file = A(1), out = A(5);
    if (!s || !file || !out)
        RET(dcmParamErr);
    wr32(out, 0);
    char path[1024];
    bool exists;
    if (!spec_path(file, path, sizeof path, &exists))
        RET(fnfErr);
    if (exists)
        RET(rest_dupFNErr);
    uint32_t id;
    dict *d = new_dict(&id);
    if (!d)
        RET(memFullErr);
    snprintf(d->path, sizeof d->path, "%s", path);
    gget(d->spec, file, 70);
    d->script = S16(2);
    d->invisible = BOOL(3);
    d->f = calloc((size_t)(s->nf ? s->nf : 1), sizeof *d->f);
    for (int i = 0; i < s->nf; i++) {
        d->f[i] = s->f[i];
        d->f[i].def = malloc(s->f[i].deflen ? s->f[i].deflen : 1);
        memcpy(d->f[i].def, s->f[i].def, s->f[i].deflen);
    }
    d->nf = s->nf;
    init_properties(d);
    uint8_t finfo[16] = { 'd', 'i', 'c', 't', 'd', 'c', 'm', 'g', 0 };
    int16_t e = fm_create_file(path);
    if (!e)
        fm_set_info(path, finfo, NULL, 0, 0);
    if (!e && !save_dict(d))
        e = ioErr;
    if (e) {
        free_dict(d);
        RET((int32_t)e);
    }
    wr32(out, id);
    RET(noErr);
}

static void drop_refs_of(int di)
{
    for (int i = 0; i < MAX_REFS; i++)
        if (refs[i].used && refs[i].dict == di)
            refs[i].used = false;
}

/* DCMDeleteDictionary(id): its file too */
static void dcm_DCMDeleteDictionary(ppc_cpu *c)
{
    dict *d = dict_of(A(0));
    if (!d)
        RET(dcmParamErr);
    drop_refs_of((int)(d - dicts));
    fm_delete(d->path);
    free_dict(d);
    RET(noErr);
}

/* DCMRegisterDictionaryFile(file, &id) */
static void dcm_DCMRegisterDictionaryFile(ppc_cpu *c)
{
    gaddr file = A(0), out = A(1);
    if (!file || !out)
        RET(dcmParamErr);
    wr32(out, 0);
    char path[1024];
    bool exists;
    if (!spec_path(file, path, sizeof path, &exists) || !exists)
        RET(fnfErr);
    for (int i = 0; i < MAX_DICTS; i++)
        if (dicts[i].used && !strcmp(dicts[i].path, path)) {
            wr32(out, DICT_TAG + (uint32_t)i + 1);
            RET(noErr);
        }
    uint32_t id;
    dict *d = new_dict(&id);
    if (!d)
        RET(memFullErr);
    snprintf(d->path, sizeof d->path, "%s", path);
    gget(d->spec, file, 70);
    int16_t e = load_dict(d);
    if (e) {
        free_dict(d);
        RET((int32_t)e);
    }
    init_properties(d);
    wr32(out, id);
    RET(noErr);
}

static void dcm_DCMUnregisterDictionary(ppc_cpu *c)
{
    dict *d = dict_of(A(0));
    if (!d)
        RET(dcmParamErr);
    for (int i = 0; i < MAX_REFS; i++)
        if (refs[i].used && refs[i].dict == (int)(d - dicts))
            RET(dcmDictionaryBusyErr);
    free_dict(d);
    RET(noErr);
}

/* DCMOpenDictionary(id, protectKeySize, protectKey, &ref) */
static void dcm_DCMOpenDictionary(ppc_cpu *c)
{
    dict *d = dict_of(A(0));
    if (!d || !A(3))
        RET(dcmParamErr);
    wr32(A(3), 0);
    for (int i = 0; i < MAX_REFS; i++)
        if (!refs[i].used) {
            refs[i] = (dref){ true, (int)(d - dicts) };
            wr32(A(3), REF_TAG + (uint32_t)i + 1);
            RET(noErr);
        }
    RET(memFullErr);
}

static void dcm_DCMCloseDictionary(ppc_cpu *c)
{
    int ri;
    dict *d = dict_of_ref(A(0), &ri);
    if (!d)
        RET(dcmDictionaryNotOpenErr);
    if (d->write_ref == ri) { /* closing with write access commits */
        free_records(d->snap, d->nsnap);
        d->snap = NULL, d->nsnap = 0;
        d->write_ref = -1;
        save_dict(d);
    }
    refs[ri].used = false;
    RET(noErr);
}

/* DCMGetDictionaryWriteAccess(ref, timeOut): taken if free (we cannot wait) */
static void dcm_DCMGetDictionaryWriteAccess(ppc_cpu *c)
{
    int ri;
    dict *d = dict_of_ref(A(0), &ri);
    if (!d)
        RET(dcmDictionaryNotOpenErr);
    dprop *perm = prop_of(d, T("perm"));
    if (perm && perm->len == 4 && be32get(perm->data) == kDCMReadOnlyDictionary)
        RET(dcmPermissionErr);
    if (d->write_ref == ri)
        RET(noErr);
    if (d->write_ref >= 0)
        RET(dcmDictionaryBusyErr);
    d->write_ref = ri;
    d->snap = calloc(d->nr ? (size_t)d->nr : 1, sizeof *d->snap);
    d->nsnap = d->nr;
    for (int i = 0; i < d->nr; i++)
        d->snap[i] = copy_record(&d->r[i]);
    d->snap_uid = d->next_uid;
    RET(noErr);
}

/* DCMReleaseDictionaryWriteAccess(ref, commitTransaction) */
static void dcm_DCMReleaseDictionaryWriteAccess(ppc_cpu *c)
{
    int ri;
    dict *d = dict_of_ref(A(0), &ri);
    if (!d)
        RET(dcmDictionaryNotOpenErr);
    if (d->write_ref != ri)
        RET(dcmPermissionErr);
    if (BOOL(1)) {
        free_records(d->snap, d->nsnap);
        save_dict(d);
    } else { /* undo */
        free_records(d->r, d->nr);
        d->r = d->snap;
        d->nr = d->nsnap;
        d->next_uid = d->snap_uid;
    }
    d->snap = NULL, d->nsnap = 0;
    d->write_ref = -1;
    RET(noErr);
}

/* A new iterator of a kind. */
static diter *new_iter(uint32_t *id)
{
    for (int i = 0; i < MAX_ITERS; i++)
        if (!iters[i].used) {
            memset(&iters[i], 0, sizeof iters[i]);
            iters[i].used = true;
            *id = ITER_TAG + (uint32_t)i + 1;
            return &iters[i];
        }
    return NULL;
}

static diter *iter_of(uint32_t id)
{
    uint32_t i = id - ITER_TAG;
    return i >= 1 && i <= MAX_ITERS && iters[i - 1].used ? &iters[i - 1] : NULL;
}

/* DCMFindRecords(ref, keyFieldTag, keySize, keyData, findMethod, preFetchedDataNum,
 * preFetchedData[], skipCount, maxRecordCount, &iterator) */
static void dcm_DCMFindRecords(ppc_cpu *c)
{
    int ri;
    dict *d = dict_of_ref(A(0), &ri);
    if (!d || !A(9))
        RET(!d ? dcmDictionaryNotOpenErr : dcmParamErr);
    wr32(A(9), 0);
    uint32_t tag = A(1), ksize = A(2), method = A(4), nfetch = A(5), skip = A(7), maxn = A(8);
    int16_t e = noErr;
    if (!key_ok(d, tag, &e))
        RET(e);
    dfield *f = field_of(d, tag);
    bool allowed = f->nfind == 0 ? method == kAEEquals : false;
    for (uint32_t i = 0; i < f->nfind; i++)
        allowed = allowed || f->find[i] == method;
    if (!allowed)
        RET(dcmBadFindMethodErr);
    uint8_t *key = host_key(A(3), ksize);
    uint32_t fetch[64];
    for (uint32_t i = 0; i < nfetch && i < 64; i++)
        fetch[i] = rd32(A(6) + 4 * i);
    if (nfetch > 64)
        nfetch = 64;
    int *order = sorted_order(d, tag);
    uint32_t id;
    diter *it = new_iter(&id);
    if (!it) {
        free(key);
        free(order);
        RET(memFullErr);
    }
    it->kind = 2;
    it->found = calloc(d->nr ? (size_t)d->nr : 1, sizeof *it->found);
    it->keys = calloc(d->nr ? (size_t)d->nr : 1, sizeof *it->keys);
    it->keylen = calloc(d->nr ? (size_t)d->nr : 1, sizeof *it->keylen);
    uint32_t skipped = 0;
    for (int i = 0; i < d->nr; i++) {
        drecord *r = &d->r[order[i]];
        dvalue *v = value_of(r, tag);
        if (!v || !matches(method, v->data, v->len, key, ksize))
            continue;
        if (skipped < skip) {
            skipped++;
            continue;
        }
        if (maxn && (uint32_t)it->n >= maxn)
            break;
        drecord keep = { r->uid, NULL, 0 };
        for (int k = 0; k < r->nv; k++) {
            bool want = false;
            for (uint32_t m = 0; m < nfetch; m++)
                want = want || fetch[m] == r->v[k].tag;
            if (!want)
                continue;
            keep.v = realloc(keep.v, (size_t)(keep.nv + 1) * sizeof *keep.v);
            keep.v[keep.nv] = r->v[k];
            keep.v[keep.nv].data = malloc(r->v[k].len ? r->v[k].len : 1);
            memcpy(keep.v[keep.nv].data, r->v[k].data, r->v[k].len);
            keep.nv++;
        }
        it->found[it->n] = keep;
        it->keys[it->n] = malloc(v->len ? v->len : 1);
        memcpy(it->keys[it->n], v->data, v->len);
        it->keylen[it->n] = v->len;
        it->n++;
    }
    free(key);
    free(order);
    wr32(A(9), id);
    RET(noErr);
}

static void dcm_DCMCountRecordIterator(ppc_cpu *c)
{
    diter *it = iter_of(A(0));
    RET(it && it->kind == 2 ? (uint32_t)(it->n - it->at) : 0);
}

/* DCMIterateFoundRecord(iterator, maxKeySize, &actualKeySize, keyData, &uniqueID, &dataList) */
static void dcm_DCMIterateFoundRecord(ppc_cpu *c)
{
    diter *it = iter_of(A(0));
    if (!it || it->kind != 2)
        RET(dcmParamErr);
    if (it->at >= it->n)
        RET(dcmIterationCompleteErr);
    int i = it->at;
    uint32_t kl = it->keylen[i];
    if (A(2))
        wr32(A(2), kl);
    if (A(3) && kl > A(1))
        RET(dcmBufferOverflowErr);
    if (A(3) && kl)
        gput(A(3), it->keys[i], kl);
    if (A(4))
        wr32(A(4), it->found[i].uid);
    if (A(5)) {
        int16_t e = list_from_values(&it->found[i], 0, NULL, A(5));
        if (e)
            RET((int32_t)e);
    }
    it->at++;
    RET(noErr);
}

static void dcm_DCMDisposeRecordIterator(ppc_cpu *c)
{
    diter *it = iter_of(A(0));
    if (!it)
        RET(dcmParamErr);
    iter_free(it);
    RET(noErr);
}

static void dcm_DCMCountRecord(ppc_cpu *c)
{
    dict *d = dict_of(A(0));
    if (!d || !A(1))
        RET(dcmParamErr);
    wr32(A(1), (uint32_t)d->nr);
    RET(noErr);
}

/* The record from (key, uid) of a ref's dictionary, or its error. */
static int locate(dict *d, uint32_t tag, gaddr key, uint32_t ksize, uint32_t uid, int16_t *err)
{
    if (!key_ok(d, tag, err))
        return -1;
    uint8_t *k = host_key(key, ksize);
    int i = find_record(d, tag, ksize, k, uid);
    free(k);
    if (i < 0)
        *err = dcmNoRecordErr;
    return i;
}

/* DCMGetRecordSequenceNumber(ref, tag, keySize, keyData, uniqueID, &seq): 1-based, in key order */
static void dcm_DCMGetRecordSequenceNumber(ppc_cpu *c)
{
    dict *d = dict_of_ref(A(0), NULL);
    if (!d || !A(5))
        RET(dcmDictionaryNotOpenErr);
    int16_t e = noErr;
    int at = locate(d, A(1), A(3), A(2), A(4), &e);
    if (at < 0)
        RET(e);
    int *order = sorted_order(d, A(1));
    for (int i = 0; i < d->nr; i++)
        if (order[i] == at)
            wr32(A(5), (uint32_t)i + 1);
    free(order);
    RET(noErr);
}

static int16_t give_key(dict *d, uint32_t tag, int idx, uint32_t maxKey, gaddr sizep, gaddr keyData, gaddr uid)
{
    dvalue *v = value_of(&d->r[idx], tag);
    uint32_t kl = v ? v->len : 0;
    if (sizep)
        wr32(sizep, kl);
    if (keyData && kl > maxKey)
        return dcmBufferOverflowErr;
    if (keyData && kl)
        gput(keyData, v->data, kl);
    if (uid)
        wr32(uid, d->r[idx].uid);
    return noErr;
}

/* DCMGetNthRecord(ref, tag, serialNum, maxKeySize, &keySize, keyData, &uniqueID) */
static void dcm_DCMGetNthRecord(ppc_cpu *c)
{
    dict *d = dict_of_ref(A(0), NULL);
    if (!d)
        RET(dcmDictionaryNotOpenErr);
    int16_t e = noErr;
    if (!key_ok(d, A(1), &e))
        RET(e);
    if (A(2) < 1 || A(2) > (uint32_t)d->nr)
        RET(dcmNoRecordErr);
    int *order = sorted_order(d, A(1));
    e = give_key(d, A(1), order[A(2) - 1], A(3), A(4), A(5), A(6));
    free(order);
    RET((int32_t)e);
}

/* The record after or before one: DCMGetNextRecord / DCMGetPrevRecord(ref, tag, keySize,
 * keyData, uniqueID, maxKeySize, &keySize, keyData, &uniqueID) */
static void neighbor(ppc_cpu *c, int dir)
{
    dict *d = dict_of_ref(A(0), NULL);
    if (!d)
        RET(dcmDictionaryNotOpenErr);
    int16_t e = noErr;
    int at = locate(d, A(1), A(3), A(2), A(4), &e);
    if (at < 0)
        RET(e);
    int *order = sorted_order(d, A(1));
    int pos = 0;
    for (int i = 0; i < d->nr; i++)
        if (order[i] == at)
            pos = i;
    pos += dir;
    if (pos < 0 || pos >= d->nr) {
        free(order);
        RET(dcmIterationCompleteErr);
    }
    e = give_key(d, A(1), order[pos], A(5), A(6), A(7), A(8));
    free(order);
    RET((int32_t)e);
}
static void dcm_DCMGetNextRecord(ppc_cpu *c) { neighbor(c, 1); }
static void dcm_DCMGetPrevRecord(ppc_cpu *c) { neighbor(c, -1); }

/* DCMGetFieldData(ref, tag, keySize, keyData, uniqueID, numOfData, dataTag[], &dataList) */
static void dcm_DCMGetFieldData(ppc_cpu *c)
{
    dict *d = dict_of_ref(A(0), NULL);
    if (!d || !A(7))
        RET(!d ? dcmDictionaryNotOpenErr : dcmParamErr);
    int16_t e = noErr;
    int at = locate(d, A(1), A(3), A(2), A(4), &e);
    if (at < 0)
        RET(e);
    uint32_t n = A(5), tags[64];
    if (n > 64)
        RET(dcmParamErr);
    int32_t any = 0;
    for (uint32_t i = 0; i < n; i++) {
        tags[i] = rd32(A(6) + 4 * i);
        if (tags[i] == T("****"))
            any = 1;
        else if (!field_of(d, tags[i]))
            RET(dcmNoFieldErr);
    }
    RET((int32_t)list_from_values(&d->r[at], any ? 0 : (int)n, tags, A(7)));
}

static bool has_write(dict *d, uint32_t refv)
{
    int ri;
    dict *x = dict_of_ref(refv, &ri);
    return x == d && d->write_ref == ri;
}

/* DCMSetFieldData(ref, tag, keySize, keyData, uniqueID, dataList) */
static void dcm_DCMSetFieldData(ppc_cpu *c)
{
    dict *d = dict_of_ref(A(0), NULL);
    if (!d)
        RET(dcmDictionaryNotOpenErr);
    if (!has_write(d, A(0)))
        RET(dcmPermissionErr);
    int16_t e = noErr;
    int at = locate(d, A(1), A(3), A(2), A(4), &e);
    if (at < 0)
        RET(e);
    drecord upd = { 0, NULL, 0 };
    e = values_from_list(d, A(5), &upd);
    if (e) {
        free_record(&upd);
        RET((int32_t)e);
    }
    drecord *r = &d->r[at];
    for (int i = 0; i < upd.nv; i++) {
        dvalue *old = value_of(r, upd.v[i].tag);
        if (old) {
            free(old->data);
            *old = upd.v[i];
        } else {
            r->v = realloc(r->v, (size_t)(r->nv + 1) * sizeof *r->v);
            r->v[r->nv++] = upd.v[i];
        }
    }
    free(upd.v);
    RET(noErr);
}

/* DCMAddRecord(ref, tag, keySize, keyData, checkOnly, dataList, &newUniqueID) */
static void dcm_DCMAddRecord(ppc_cpu *c)
{
    dict *d = dict_of_ref(A(0), NULL);
    if (!d)
        RET(dcmDictionaryNotOpenErr);
    if (!has_write(d, A(0)))
        RET(dcmPermissionErr);
    uint32_t tag = A(1);
    int16_t e = noErr;
    if (!key_ok(d, tag, &e))
        RET(e);
    drecord rec = { 0, NULL, 0 };
    e = values_from_list(d, A(5), &rec);
    if (!e && !value_of(&rec, tag)) { /* the key given is the key field's value */
        uint32_t ks = A(2);
        dfield *kf = field_of(d, tag);
        if ((kf->maxsize && ks > kf->maxsize))
            e = dcmBadDataSizeErr;
        else {
            dvalue v = { tag, kf->type, ks, host_key(A(3), ks) };
            rec.v = realloc(rec.v, (size_t)(rec.nv + 1) * sizeof *rec.v);
            rec.v[rec.nv++] = v;
        }
    }
    /* Defaults for what is missing; required fields without one are an error. */
    for (int i = 0; !e && i < d->nf; i++) {
        dfield *f = &d->f[i];
        if (value_of(&rec, f->tag))
            continue;
        if (f->has_default) {
            dvalue v = { f->tag, f->deftype, f->deflen, malloc(f->deflen ? f->deflen : 1) };
            memcpy(v.data, f->def, f->deflen);
            rec.v = realloc(rec.v, (size_t)(rec.nv + 1) * sizeof *rec.v);
            rec.v[rec.nv++] = v;
        } else if (f->attrs & (kDCMRequiredFieldMask | kDCMIdentifyFieldMask)) {
            e = dcmNecessaryFieldErr;
        }
    }
    /* A duplicate: the same identify fields (the key alone when none is marked). */
    if (!e) {
        bool anyid = false;
        for (int i = 0; i < d->nf; i++)
            anyid = anyid || (d->f[i].attrs & kDCMIdentifyFieldMask);
        for (int k = 0; !e && k < d->nr; k++) {
            bool same = true;
            for (int i = 0; i < d->nf && same; i++) {
                dfield *f = &d->f[i];
                if (anyid ? !(f->attrs & kDCMIdentifyFieldMask) : f->tag != tag)
                    continue;
                dvalue *a = value_of(&rec, f->tag), *b = value_of(&d->r[k], f->tag);
                same = a && b && a->len == b->len && !memcmp(a->data, b->data, a->len);
            }
            if (same)
                e = dcmDupRecordErr;
        }
    }
    if (!e && !A(4)) {
        rec.uid = d->next_uid++;
        d->r = realloc(d->r, (size_t)(d->nr + 1) * sizeof *d->r);
        d->r[d->nr++] = rec;
        if (A(6))
            wr32(A(6), rec.uid);
        RET(noErr);
    }
    free_record(&rec);
    RET((int32_t)e);
}

/* DCMDeleteRecord(ref, tag, keySize, keyData, uniqueID) */
static void dcm_DCMDeleteRecord(ppc_cpu *c)
{
    dict *d = dict_of_ref(A(0), NULL);
    if (!d)
        RET(dcmDictionaryNotOpenErr);
    if (!has_write(d, A(0)))
        RET(dcmPermissionErr);
    int16_t e = noErr;
    int at = locate(d, A(1), A(3), A(2), A(4), &e);
    if (at < 0)
        RET(e);
    free_record(&d->r[at]);
    memmove(&d->r[at], &d->r[at + 1], (size_t)(d->nr - at - 1) * sizeof *d->r);
    d->nr--;
    RET(noErr);
}

/* DCMReorganizeDictionary(id, extraCapacity, progressProc, userData), DCMCompactDictionary(id, proc, ud):
 * the file rewritten whole; the progress routine told it is done. */
static int16_t rewrite(dict *d, gaddr proc, uint32_t userData)
{
    if (proc) {
        uint32_t args[3] = { 1, 100, userData };
        aem_call_proc(proc, 0x00000E50, args, 3);
    }
    return save_dict(d) ? noErr : ioErr;
}
static void dcm_DCMReorganizeDictionary(ppc_cpu *c)
{
    dict *d = dict_of(A(0));
    RET(d ? (int32_t)rewrite(d, A(2), A(3)) : dcmParamErr);
}
static void dcm_DCMCompactDictionary(ppc_cpu *c)
{
    dict *d = dict_of(A(0));
    RET(d ? (int32_t)rewrite(d, A(1), A(2)) : dcmParamErr);
}

static void dcm_DCMGetFileFromDictionaryID(ppc_cpu *c)
{
    dict *d = dict_of(A(0));
    if (!d || !A(1))
        RET(dcmParamErr);
    gput(A(1), d->spec, 70);
    RET(noErr);
}

/* DCMGetDictionaryIDFromFile(file, &id): a registered one, else the file is registered */
static void dcm_DCMGetDictionaryIDFromFile(ppc_cpu *c)
{
    gaddr file = A(0), out = A(1);
    if (!file || !out)
        RET(dcmParamErr);
    char path[1024];
    bool exists;
    if (!spec_path(file, path, sizeof path, &exists) || !exists) {
        wr32(out, 0);
        RET(fnfErr);
    }
    for (int i = 0; i < MAX_DICTS; i++)
        if (dicts[i].used && !strcmp(dicts[i].path, path)) {
            wr32(out, DICT_TAG + (uint32_t)i + 1);
            RET(noErr);
        }
    dcm_DCMRegisterDictionaryFile(c);
}

static void dcm_DCMGetDictionaryIDFromRef(ppc_cpu *c)
{
    int ri;
    dict *d = dict_of_ref(A(0), &ri);
    RET(d ? DICT_TAG + (uint32_t)(d - dicts) + 1 : 0);
}

/* DCMGetDictionaryFieldInfo(id, fieldTag, &fieldInfoRecord) */
static void dcm_DCMGetDictionaryFieldInfo(ppc_cpu *c)
{
    dict *d = dict_of(A(0));
    if (!d || !A(2))
        RET(dcmParamErr);
    dfield *f = field_of(d, A(1));
    if (!f)
        RET(dcmNoFieldErr);
    RET((int32_t)record_from_field(f, A(2)));
}

/* DCMGetDictionaryProperty(id, tag, maxSize, &actualSize, value) */
static void dcm_DCMGetDictionaryProperty(ppc_cpu *c)
{
    dict *d = dict_of(A(0));
    if (!d)
        RET(dcmParamErr);
    dprop *p = prop_of(d, A(1));
    if (!p)
        RET(dcmBadPropertyErr);
    if (A(3))
        wr32(A(3), p->len);
    if (A(4) && p->len > A(2))
        RET(dcmBufferOverflowErr);
    if (A(4) && p->len)
        gput(A(4), p->data, p->len);
    RET(noErr);
}

/* DCMSetDictionaryProperty(id, tag, size, value) */
static void dcm_DCMSetDictionaryProperty(ppc_cpu *c)
{
    dict *d = dict_of(A(0));
    if (!d || (!A(3) && A(2)))
        RET(dcmParamErr);
    uint8_t *v = host_key(A(3), A(2));
    set_prop(d, A(1), v, A(2));
    free(v);
    save_dict(d);
    RET(noErr);
}

/* DCMGetDictionaryPropertyList(id, maxPropertyNum, &numProperties, tags[]) */
static void dcm_DCMGetDictionaryPropertyList(ppc_cpu *c)
{
    dict *d = dict_of(A(0));
    if (!d)
        RET(dcmParamErr);
    if (A(2))
        wr32(A(2), (uint32_t)d->np);
    for (int i = 0; A(3) && i < d->np && (uint32_t)i < A(1); i++)
        wr32(A(3) + 4u * (uint32_t)i, d->p[i].tag);
    RET(A(3) && (uint32_t)d->np > A(1) ? dcmBufferOverflowErr : noErr);
}

/* DCMCreateDictionaryIterator(&it), DCMCreateAccessMethodIterator(&it) */
static void create_object_iterator(ppc_cpu *c, int kind)
{
    if (!A(0))
        RET(dcmParamErr);
    uint32_t id;
    diter *it = new_iter(&id);
    if (!it)
        RET(memFullErr);
    it->kind = kind;
    it->ids = calloc(MAX_DICTS + 1, sizeof *it->ids);
    if (kind == 1) {
        it->ids[it->n++] = METHOD_ID;
    } else {
        for (int i = 0; i < MAX_DICTS; i++) {
            if (!dicts[i].used || dicts[i].invisible)
                continue;
            dprop *l = prop_of(&dicts[i], T("list"));
            if (l && l->len == 4 && be32get(l->data) == 1) /* kDCMProhibitListing */
                continue;
            it->ids[it->n++] = DICT_TAG + (uint32_t)i + 1;
        }
    }
    wr32(A(0), id);
    RET(noErr);
}
static void dcm_DCMCreateDictionaryIterator(ppc_cpu *c) { create_object_iterator(c, 0); }
static void dcm_DCMCreateAccessMethodIterator(ppc_cpu *c) { create_object_iterator(c, 1); }

static void dcm_DCMCountObjectIterator(ppc_cpu *c)
{
    diter *it = iter_of(A(0));
    RET(it && it->kind != 2 ? (uint32_t)it->n : 0);
}

static void dcm_DCMIterateObject(ppc_cpu *c)
{
    diter *it = iter_of(A(0));
    if (!it || it->kind == 2 || !A(1))
        RET(dcmParamErr);
    if (it->at >= it->n)
        RET(dcmIterationCompleteErr);
    wr32(A(1), it->ids[it->at++]);
    RET(noErr);
}

static void dcm_DCMResetObjectIterator(ppc_cpu *c)
{
    diter *it = iter_of(A(0));
    if (!it)
        RET(dcmParamErr);
    it->at = 0;
    RET(noErr);
}

static void dcm_DCMDisposeObjectIterator(ppc_cpu *c)
{
    diter *it = iter_of(A(0));
    if (!it)
        RET(dcmParamErr);
    iter_free(it);
    RET(noErr);
}

/* DCMGetAccessMethodIDFromName(name, &id) */
static void dcm_DCMGetAccessMethodIDFromName(ppc_cpu *c)
{
    if (!A(0) || !A(1))
        RET(dcmParamErr);
    char name[64];
    rest_pstr_get(A(0), name, sizeof name);
    if (!strcmp(name, "Blue Box Dictionary")) {
        wr32(A(1), METHOD_ID);
        RET(noErr);
    }
    wr32(A(1), 0);
    RET(dcmNoAccessMethodErr);
}

/* DCMCreateFieldInfoRecord(tag, type, maxSize, attrs, defaultData, nFind, findMethods[], &record) */
static void dcm_DCMCreateFieldInfoRecord(ppc_cpu *c)
{
    if (!A(7) || (A(5) && !A(6)))
        RET(dcmParamErr);
    dfield f = { A(0), A(1), A(2), A(3), 0, NULL, 0, false, A(5) > MAX_FIND ? MAX_FIND : A(5), { 0 } };
    for (uint32_t i = 0; i < f.nfind; i++)
        f.find[i] = rd32(A(6) + 4 * i);
    uint8_t *def = NULL;
    if (A(4) && rd32(A(4)) != T("null")) {
        gaddr h = rd32(A(4) + 4);
        uint32_t n = h ? (uint32_t)tb_GetHandleSize(h) : 0;
        def = malloc(n ? n : 1);
        if (n)
            gget(def, rd32(h), n);
        f.def = def;
        f.deflen = n;
        f.deftype = rd32(A(4));
        f.has_default = true;
    }
    int16_t e = record_from_field(&f, A(7));
    free(def);
    RET((int32_t)e);
}

static void dcm_DCMGetFieldTagAndType(ppc_cpu *c)
{
    dfield f;
    if (!A(0) || !A(1) || !A(2))
        RET(dcmParamErr);
    int16_t e = field_from_record(A(0), &f);
    if (!e) {
        wr32(A(1), f.tag);
        wr32(A(2), f.type);
        free(f.def);
    }
    RET((int32_t)e);
}

static void dcm_DCMGetFieldMaxRecordSize(ppc_cpu *c)
{
    dfield f;
    if (!A(0) || !A(1))
        RET(dcmParamErr);
    int16_t e = field_from_record(A(0), &f);
    if (!e) {
        wr32(A(1), f.maxsize);
        free(f.def);
    }
    RET((int32_t)e);
}

static void dcm_DCMGetFieldAttributes(ppc_cpu *c)
{
    dfield f;
    if (!A(0) || !A(1))
        RET(dcmParamErr);
    int16_t e = field_from_record(A(0), &f);
    if (!e) {
        wr32(A(1), f.attrs);
        free(f.def);
    }
    RET((int32_t)e);
}

/* DCMGetFieldDefaultData(record, desiredType, &data) */
static void dcm_DCMGetFieldDefaultData(ppc_cpu *c)
{
    dfield f;
    if (!A(0) || !A(2))
        RET(dcmParamErr);
    int16_t e = field_from_record(A(0), &f);
    if (e)
        RET((int32_t)e);
    if (!f.has_default) {
        free(f.def);
        RET(dcmBadFieldInfoErr);
    }
    gaddr tmp = tb_NewPtr(f.deflen ? (int32_t)f.deflen : 1, false, true);
    gput(tmp, f.def, f.deflen);
    e = tb_AECreateDesc((int32_t)f.deftype, tmp, (int32_t)f.deflen, A(2));
    tb_DisposePtr(tmp);
    if (!e && A(1) != T("****") && A(1) != f.deftype) {
        gaddr coerced = tb_NewPtr(8, false, true);
        e = tb_AECoerceDesc(A(2), (int32_t)A(1), coerced);
        if (!e) {
            tb_AEDisposeDesc(A(2));
            wr32(A(2), rd32(coerced));
            wr32(A(2) + 4, rd32(coerced + 4));
        } else
            tb_AEDisposeDesc(A(2));
        tb_DisposePtr(coerced);
    }
    free(f.def);
    RET((int32_t)e);
}

/* DCMGetFieldFindMethods(record, max, methods[], &actual) */
static void dcm_DCMGetFieldFindMethods(ppc_cpu *c)
{
    dfield f;
    if (!A(0))
        RET(dcmParamErr);
    int16_t e = field_from_record(A(0), &f);
    if (e)
        RET((int32_t)e);
    if (A(3))
        wr32(A(3), f.nfind);
    for (uint32_t i = 0; A(2) && i < f.nfind && i < A(1); i++)
        wr32(A(2) + 4 * i, f.find[i]);
    free(f.def);
    RET(A(2) && f.nfind > A(1) ? dcmBufferOverflowErr : noErr);
}

/* ---- For Language Analysis (carbonrest_la.c): a dictionary by its file, and its words ---- */

/* The dictionary of a file (registered if need be): its ID, or 0 and the error. */
uint32_t rest_dcm_id_for_file(gaddr spec, int16_t *err)
{
    char path[1024];
    bool exists;
    *err = noErr;
    if (!spec_path(spec, path, sizeof path, &exists) || !exists) {
        *err = fnfErr;
        return 0;
    }
    for (int i = 0; i < MAX_DICTS; i++)
        if (dicts[i].used && !strcmp(dicts[i].path, path))
            return DICT_TAG + (uint32_t)i + 1;
    uint32_t id;
    dict *d = new_dict(&id);
    if (!d) {
        *err = memFullErr;
        return 0;
    }
    snprintf(d->path, sizeof d->path, "%s", path);
    gget(d->spec, spec, 70);
    *err = load_dict(d);
    if (*err) {
        free_dict(d);
        return 0;
    }
    init_properties(d);
    return id;
}

/* Whether a dictionary has the Japanese fields analysis needs (yomi and hyoki). */
bool rest_dcm_has_word_fields(uint32_t id)
{
    dict *d = dict_of(id);
    return d && field_of(d, T("yomi")) && field_of(d, T("hyok"));
}

/* Each record's hyoki (UTF-16 bytes), yomi, part of speech and weight, for Language Analysis. */
void rest_dcm_words(uint32_t id, void (*fn)(void *ctx, uint32_t uid, const uint8_t *hyoki, uint32_t hlen,
                                             uint32_t pos, uint16_t weight), void *ctx)
{
    dict *d = dict_of(id);
    for (int i = 0; d && i < d->nr; i++) {
        dvalue *h = value_of(&d->r[i], T("hyok")), *p = value_of(&d->r[i], T("hins")), *w = value_of(&d->r[i], T("hind"));
        if (!h)
            continue;
        fn(ctx, d->r[i].uid, h->data, h->len, p && p->len == 4 ? be32get(p->data) : 0,
           w && w->len == 2 ? (uint16_t)(w->data[0] << 8 | w->data[1]) : 0);
    }
}

const ppc_export rest_dcm_exports[] = {
    { "DCMAddRecord", dcm_DCMAddRecord },
    { "DCMCloseDictionary", dcm_DCMCloseDictionary },
    { "DCMCompactDictionary", dcm_DCMCompactDictionary },
    { "DCMCountObjectIterator", dcm_DCMCountObjectIterator },
    { "DCMCountRecord", dcm_DCMCountRecord },
    { "DCMCountRecordIterator", dcm_DCMCountRecordIterator },
    { "DCMCreateAccessMethodIterator", dcm_DCMCreateAccessMethodIterator },
    { "DCMCreateDictionaryIterator", dcm_DCMCreateDictionaryIterator },
    { "DCMCreateFieldInfoRecord", dcm_DCMCreateFieldInfoRecord },
    { "DCMDeleteDictionary", dcm_DCMDeleteDictionary },
    { "DCMDeleteRecord", dcm_DCMDeleteRecord },
    { "DCMDeriveNewDictionary", dcm_DCMDeriveNewDictionary },
    { "DCMDisposeObjectIterator", dcm_DCMDisposeObjectIterator },
    { "DCMDisposeRecordIterator", dcm_DCMDisposeRecordIterator },
    { "DCMFindRecords", dcm_DCMFindRecords },
    { "DCMGetAccessMethodIDFromName", dcm_DCMGetAccessMethodIDFromName },
    { "DCMGetDictionaryFieldInfo", dcm_DCMGetDictionaryFieldInfo },
    { "DCMGetDictionaryIDFromFile", dcm_DCMGetDictionaryIDFromFile },
    { "DCMGetDictionaryIDFromRef", dcm_DCMGetDictionaryIDFromRef },
    { "DCMGetDictionaryProperty", dcm_DCMGetDictionaryProperty },
    { "DCMGetDictionaryPropertyList", dcm_DCMGetDictionaryPropertyList },
    { "DCMGetDictionaryWriteAccess", dcm_DCMGetDictionaryWriteAccess },
    { "DCMGetFieldAttributes", dcm_DCMGetFieldAttributes },
    { "DCMGetFieldData", dcm_DCMGetFieldData },
    { "DCMGetFieldDefaultData", dcm_DCMGetFieldDefaultData },
    { "DCMGetFieldFindMethods", dcm_DCMGetFieldFindMethods },
    { "DCMGetFieldMaxRecordSize", dcm_DCMGetFieldMaxRecordSize },
    { "DCMGetFieldTagAndType", dcm_DCMGetFieldTagAndType },
    { "DCMGetFileFromDictionaryID", dcm_DCMGetFileFromDictionaryID },
    { "DCMGetNextRecord", dcm_DCMGetNextRecord },
    { "DCMGetNthRecord", dcm_DCMGetNthRecord },
    { "DCMGetPrevRecord", dcm_DCMGetPrevRecord },
    { "DCMGetRecordSequenceNumber", dcm_DCMGetRecordSequenceNumber },
    { "DCMIterateFoundRecord", dcm_DCMIterateFoundRecord },
    { "DCMIterateObject", dcm_DCMIterateObject },
    { "DCMLibraryVersion", dcm_DCMLibraryVersion },
    { "DCMNewDictionary", dcm_DCMNewDictionary },
    { "DCMOpenDictionary", dcm_DCMOpenDictionary },
    { "DCMRegisterDictionaryFile", dcm_DCMRegisterDictionaryFile },
    { "DCMReleaseDictionaryWriteAccess", dcm_DCMReleaseDictionaryWriteAccess },
    { "DCMReorganizeDictionary", dcm_DCMReorganizeDictionary },
    { "DCMResetObjectIterator", dcm_DCMResetObjectIterator },
    { "DCMSetDictionaryProperty", dcm_DCMSetDictionaryProperty },
    { "DCMSetFieldData", dcm_DCMSetFieldData },
    { "DCMUnregisterDictionary", dcm_DCMUnregisterDictionary },
};
const unsigned rest_dcm_export_count = sizeof rest_dcm_exports / sizeof rest_dcm_exports[0];
