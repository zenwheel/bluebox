/*
 * The System 7 Dictionary Manager (_DictionaryDispatch, $AA53) and AVL
 * trees (_AVLTreeDispatch, $AA80).
 *
 * A dictionary is a file of keyed records: Pascal-string keys compared as
 * the dictionary's key attributes say (case sensitive or not, marks or
 * not), each with up to 1024 bytes of data.  Ours are kept whole in memory
 * while open and written back after each change, in a format of our own:
 *     'BBdc', version (word), script (word), maximum key length (word),
 *     key attributes (byte), a pad byte, the record count (long), then
 *     each record: the key (a Pascal string), its data's length (long)
 *     and the data, in key order.
 * Apple's own dictionary files (B-trees) read as notBTree.
 *
 * An AVL tree keeps copies of its items in the zone current at AVLInit,
 * ordered by the program's compare procedure; the balancing is ours.
 */
#include <stdlib.h>
#include <string.h>

#include "tb_internal.h"

enum {
    notBTree = -410,
    btDupRecErr = -414,
    btRecNotFnd = -415,
    btKeyLenErr = -416,
    btKeyAttrErr = -417,
    unknownInsertModeErr = -20000,
    recordDataTooBigErr = -20001,
    wrPermErr = -61,
    dupFNErr = -48,
};
enum { kInsert, kReplace, kInsertOrReplace };
enum { kIsCaseSensitive = 0x10, kIsNotDiacriticalSensitive = 0x20 };
enum { MAX_DATA = 1024, MAX_DICTS = 16, DICT_TAG = 0x44430000 };
#define DICT_MAGIC 0x42426463u /* 'BBdc' */

typedef struct {
    uint8_t key[256]; /* a Pascal string */
    uint8_t *data;
    uint32_t len;
} drecord;

typedef struct {
    bool used, writable;
    uint8_t spec[70];
    int16_t refnum;
    int16_t script, max_key;
    uint8_t attributes;
    drecord *recs;
    uint32_t count, cap;
} dictionary;

static dictionary dicts[MAX_DICTS];

static dictionary *dict_of(int32_t ref)
{
    if (((uint32_t)ref & 0xFFFF0000u) != DICT_TAG)
        return NULL;
    unsigned i = (uint32_t)ref & 0xFFFF;
    return i < MAX_DICTS && dicts[i].used ? &dicts[i] : NULL;
}

/* Keys compare by the dictionary's attributes. */
static int key_cmp(const dictionary *d, const uint8_t *a, const uint8_t *b)
{
    bool case_sens = d->attributes & kIsCaseSensitive, marks = !(d->attributes & kIsNotDiacriticalSensitive);
    int n = a[0] < b[0] ? a[0] : b[0];
    for (int i = 1; i <= n; i++) {
        int x = mac_char_key(a[i], case_sens, !marks), y = mac_char_key(b[i], case_sens, !marks);
        if (x != y)
            return x < y ? -1 : 1;
    }
    return a[0] - b[0];
}

/* The record with key, or where it would go (*found false). */
static uint32_t find(const dictionary *d, const uint8_t *key, bool *found)
{
    uint32_t lo = 0, hi = d->count;
    *found = false;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        int c = key_cmp(d, d->recs[mid].key, key);
        if (!c) {
            *found = true;
            return mid;
        }
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static void free_records(dictionary *d)
{
    for (uint32_t i = 0; i < d->count; i++)
        free(d->recs[i].data);
    free(d->recs);
    d->recs = NULL;
    d->count = d->cap = 0;
}

/* ---- The file ---- */

static gaddr new_pb(int16_t refnum)
{
    gaddr pb = tb_NewPtr(80, false, true);
    if (pb)
        wr16(pb + IOParam_ioRefNum_off, (uint16_t)refnum);
    return pb;
}

static int16_t file_read_all(int16_t refnum, uint8_t **out, uint32_t *len)
{
    gaddr pb = new_pb(refnum);
    if (!pb)
        return memFullErr;
    int16_t e = tb_PBGetEOF(pb, false);
    uint32_t n = e ? 0 : rd32(pb + IOParam_ioMisc_off);
    gaddr buf = n ? tb_NewPtr((int32_t)n, false, false) : 0;
    if (!e && n && !buf)
        e = memFullErr;
    if (!e && n) {
        wr16(pb + IOParam_ioPosMode_off, 1); /* fsFromStart */
        wr32(pb + IOParam_ioPosOffset_off, 0);
        wr32(pb + IOParam_ioBuffer_off, buf);
        wr32(pb + IOParam_ioReqCount_off, n);
        e = tb_PBRead(pb, false);
        if (e == -39) /* eofErr */
            e = noErr;
        n = rd32(pb + IOParam_ioActCount_off);
    }
    *len = 0;
    *out = NULL;
    if (!e) {
        *out = malloc(n ? n : 1);
        gget(*out, buf, n);
        *len = n;
    }
    if (buf)
        tb_DisposePtr(buf);
    tb_DisposePtr(pb);
    return e;
}

static int16_t file_write_all(int16_t refnum, const uint8_t *data, uint32_t n)
{
    gaddr pb = new_pb(refnum), buf = tb_NewPtr((int32_t)(n ? n : 1), false, false);
    int16_t e = pb && buf ? noErr : memFullErr;
    if (!e) {
        gput(buf, data, n);
        wr16(pb + IOParam_ioPosMode_off, 1);
        wr32(pb + IOParam_ioPosOffset_off, 0);
        wr32(pb + IOParam_ioBuffer_off, buf);
        wr32(pb + IOParam_ioReqCount_off, n);
        e = tb_PBWrite(pb, false);
    }
    if (!e) {
        wr32(pb + IOParam_ioMisc_off, n);
        e = tb_PBSetEOF(pb, false);
    }
    if (!e)
        e = tb_PBFlushFile(pb, false);
    if (buf)
        tb_DisposePtr(buf);
    if (pb)
        tb_DisposePtr(pb);
    return e;
}

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v >> 16); put16(p + 2, v); }
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t get32(const uint8_t *p) { return get16(p) << 16 | get16(p + 2); }

enum { HEADER = 16 };

static uint8_t *encode(const dictionary *d, uint32_t *len)
{
    uint32_t n = HEADER;
    for (uint32_t i = 0; i < d->count; i++)
        n += 1u + d->recs[i].key[0] + 4 + d->recs[i].len;
    uint8_t *b = malloc(n), *p = b + HEADER;
    put32(b, DICT_MAGIC);
    put16(b + 4, 1);
    put16(b + 6, (uint16_t)d->script);
    put16(b + 8, (uint16_t)d->max_key);
    b[10] = d->attributes;
    b[11] = 0;
    put32(b + 12, d->count);
    for (uint32_t i = 0; i < d->count; i++) {
        memcpy(p, d->recs[i].key, 1u + d->recs[i].key[0]);
        p += 1u + d->recs[i].key[0];
        put32(p, d->recs[i].len);
        memcpy(p + 4, d->recs[i].data, d->recs[i].len);
        p += 4 + d->recs[i].len;
    }
    *len = n;
    return b;
}

static int16_t decode(dictionary *d, const uint8_t *b, uint32_t n)
{
    if (n < HEADER || get32(b) != DICT_MAGIC)
        return notBTree;
    d->script = (int16_t)get16(b + 6);
    d->max_key = (int16_t)get16(b + 8);
    d->attributes = b[10];
    uint32_t count = get32(b + 12), at = HEADER;
    d->recs = calloc(count ? count : 1, sizeof *d->recs);
    d->cap = count ? count : 1;
    for (uint32_t i = 0; i < count; i++) {
        if (at >= n || at + 1u + b[at] + 4 > n)
            return notBTree;
        drecord *r = &d->recs[d->count];
        memcpy(r->key, b + at, 1u + b[at]);
        at += 1u + b[at];
        r->len = get32(b + at);
        at += 4;
        if (r->len > n - at)
            return notBTree;
        r->data = malloc(r->len ? r->len : 1);
        memcpy(r->data, b + at, r->len);
        at += r->len;
        d->count++;
    }
    return noErr;
}

static int16_t save(dictionary *d)
{
    uint32_t n;
    uint8_t *b = encode(d, &n);
    int16_t e = file_write_all(d->refnum, b, n);
    free(b);
    return e;
}

/* ---- The calls ---- */

int16_t tb_InitializeDictionary(gaddr spec, int16_t maximumKeyLength, int8_t keyAttributes, int16_t script)
{
    if (maximumKeyLength <= 0 || maximumKeyLength > 255)
        return btKeyLenErr;
    if ((uint8_t)keyAttributes & ~(kIsCaseSensitive | kIsNotDiacriticalSensitive))
        return btKeyAttrErr;
    int16_t e = tb_FSpCreate(spec, (int32_t)fourcc("dict"), (int32_t)fourcc("dict"), script);
    if (e && e != dupFNErr)
        return e;
    gaddr ref = tb_NewPtr(2, false, true);
    e = tb_FSpOpenDF(spec, 3, ref); /* fsRdWrPerm */
    if (!e) {
        dictionary d = { .script = script, .max_key = maximumKeyLength, .attributes = (uint8_t)keyAttributes,
                         .refnum = (int16_t)rd16(ref) };
        e = save(&d);
        gaddr pb = new_pb(d.refnum);
        tb_PBClose(pb, false);
        tb_DisposePtr(pb);
    }
    tb_DisposePtr(ref);
    return e;
}

int16_t tb_OpenDictionary(gaddr spec, int8_t accessPermission, gaddr dictionaryReference)
{
    unsigned i = 0;
    while (i < MAX_DICTS && dicts[i].used)
        i++;
    if (i == MAX_DICTS)
        return -42; /* tmfoErr */
    gaddr ref = tb_NewPtr(2, false, true);
    int16_t e = tb_FSpOpenDF(spec, accessPermission, ref);
    int16_t refnum = (int16_t)rd16(ref);
    tb_DisposePtr(ref);
    if (e)
        return e;
    dictionary *d = &dicts[i];
    *d = (dictionary){ .refnum = refnum, .writable = (accessPermission & 2) || !accessPermission };
    gget(d->spec, spec, sizeof d->spec);
    uint8_t *b;
    uint32_t n;
    e = file_read_all(refnum, &b, &n);
    if (!e) {
        e = decode(d, b, n);
        free(b);
    }
    if (e) {
        free_records(d);
        gaddr pb = new_pb(refnum);
        tb_PBClose(pb, false);
        tb_DisposePtr(pb);
        return e;
    }
    d->used = true;
    if (dictionaryReference)
        wr32(dictionaryReference, DICT_TAG | i);
    return noErr;
}

int16_t tb_CloseDictionary(int32_t dictionaryReference)
{
    dictionary *d = dict_of(dictionaryReference);
    if (!d)
        return paramErr;
    gaddr pb = new_pb(d->refnum);
    int16_t e = tb_PBClose(pb, false);
    tb_DisposePtr(pb);
    free_records(d);
    d->used = false;
    return e;
}

int16_t tb_InsertRecordToDictionary(int32_t dictionaryReference, gaddr key, gaddr recordDataHandle, int16_t whichMode)
{
    dictionary *d = dict_of(dictionaryReference);
    if (!d || !key)
        return paramErr;
    if (whichMode < kInsert || whichMode > kInsertOrReplace)
        return unknownInsertModeErr;
    if (!d->writable)
        return wrPermErr;
    uint8_t k[256];
    gget(k, key, 1u + rd8(key));
    if (!k[0] || k[0] > d->max_key)
        return btKeyLenErr;
    uint32_t len = recordDataHandle && rd32(recordDataHandle) ? (uint32_t)tb_GetHandleSize(recordDataHandle) : 0;
    if (len > MAX_DATA)
        return recordDataTooBigErr;
    bool found;
    uint32_t at = find(d, k, &found);
    if (found && whichMode == kInsert)
        return btDupRecErr;
    if (!found && whichMode == kReplace)
        return btRecNotFnd;
    uint8_t *data = malloc(len ? len : 1);
    if (len)
        gget(data, rd32(recordDataHandle), len);
    if (found) {
        free(d->recs[at].data);
    } else {
        if (d->count == d->cap) {
            d->cap = d->cap ? d->cap * 2 : 16;
            d->recs = realloc(d->recs, d->cap * sizeof *d->recs);
        }
        memmove(&d->recs[at + 1], &d->recs[at], (d->count - at) * sizeof *d->recs);
        d->count++;
        memcpy(d->recs[at].key, k, 1u + k[0]);
    }
    d->recs[at].data = data;
    d->recs[at].len = len;
    return save(d);
}

int16_t tb_DeleteRecordFromDictionary(int32_t dictionaryReference, gaddr key)
{
    dictionary *d = dict_of(dictionaryReference);
    if (!d || !key)
        return paramErr;
    if (!d->writable)
        return wrPermErr;
    uint8_t k[256];
    gget(k, key, 1u + rd8(key));
    bool found;
    uint32_t at = find(d, k, &found);
    if (!found)
        return btRecNotFnd;
    free(d->recs[at].data);
    memmove(&d->recs[at], &d->recs[at + 1], (d->count - at - 1) * sizeof *d->recs);
    d->count--;
    return save(d);
}

/* A record's data into the caller's handle.  (Every attribute's data: the
 * attribute table asks for no less.) */
static int16_t give_data(const drecord *r, gaddr h)
{
    if (!h)
        return noErr;
    tb_SetHandleSize(h, (int32_t)r->len);
    if (r->len && (int32_t)tb_GetHandleSize(h) != (int32_t)r->len)
        return memFullErr;
    if (r->len)
        gput(rd32(h), r->data, r->len);
    return noErr;
}

int16_t tb_FindRecordInDictionary(int32_t dictionaryReference, gaddr key, gaddr requestedAttributeTablePointer,
                                  gaddr recordDataHandle)
{
    dictionary *d = dict_of(dictionaryReference);
    if (!d || !key)
        return paramErr;
    uint8_t k[256];
    gget(k, key, 1u + rd8(key));
    bool found;
    uint32_t at = find(d, k, &found);
    return found ? give_data(&d->recs[at], recordDataHandle) : btRecNotFnd;
}

/* Records by index, from 0, in key order. */
int16_t tb_FindRecordByIndexInDictionary(int32_t dictionaryReference, int32_t recordIndex,
                                         gaddr requestedAttributeTablePointer, gaddr recordKey,
                                         gaddr recordDataHandle)
{
    dictionary *d = dict_of(dictionaryReference);
    if (!d)
        return paramErr;
    if (recordIndex < 0 || (uint32_t)recordIndex >= d->count)
        return btRecNotFnd;
    const drecord *r = &d->recs[recordIndex];
    if (recordKey)
        gput(recordKey, r->key, 1u + r->key[0]);
    return give_data(r, recordDataHandle);
}

/* DictionaryInformation: the FSSpec (70 bytes), numberOfRecords,
 * currentGarbageSize, script, maximumKeyLength, keyAttributes. */
int16_t tb_GetDictionaryInformation(int32_t dictionaryReference, gaddr info)
{
    dictionary *d = dict_of(dictionaryReference);
    if (!d || !info)
        return paramErr;
    gput(info, d->spec, sizeof d->spec);
    wr32(info + 70, d->count);
    wr32(info + 74, 0); /* written whole after each change: no garbage */
    wr16(info + 78, (uint16_t)d->script);
    wr16(info + 80, (uint16_t)d->max_key);
    wr8(info + 82, d->attributes);
    return noErr;
}

int16_t tb_CompactDictionary(int32_t dictionaryReference)
{
    dictionary *d = dict_of(dictionaryReference);
    if (!d)
        return paramErr;
    return d->writable ? save(d) : noErr;
}

/* ---------------------------------------------------------------------- */
/* AVL trees */

enum {
    errItemAlreadyInTree = -960,
    errNotValidTree = -961,
    errItemNotFoundInTree = -962,
    errCanNotInsertWhileWalkProcInProgress = -963,
    errTreeIsLocked = -964,
};
enum { kAVLPreOrder, kAVLInOrder, kAVLPostOrder };
enum { kLeftToRight, kRightToLeft };
enum { kAVLIsTree, kAVLIsLeftBranch, kAVLIsRightBranch, kAVLIsLeaf };
enum {
    uppAVLCompareItemsProcInfo = 0x00002FF0,
    uppAVLItemSizeProcInfo = 0x000003F0,
    uppAVLDisposeItemProcInfo = 0x000003C0,
    uppAVLWalkProcInfo = 0x000FEBE0,
};
#define AVL_SIGNATURE 0x41564C54u /* 'AVLT' */
enum { MAX_TREES = 64 };

typedef struct avl_node {
    struct avl_node *left, *right;
    int height;
    gaddr item;
    uint32_t size;
    bool dead; /* removed during a walk; unlinked after it */
} avl_node;

typedef struct {
    gaddr tree; /* the AVLTreeStruct in guest memory */
    gaddr compare, size_proc, dispose_proc, refcon;
    gaddr zone;
    avl_node *root;
    uint32_t count;
    int walking;
    bool has_dead;
} avl_tree;

static avl_tree trees[MAX_TREES];

static avl_tree *tree_of(gaddr t)
{
    if (!t)
        return NULL;
    for (unsigned i = 0; i < MAX_TREES; i++)
        if (trees[i].tree == t && rd32(t) == AVL_SIGNATURE)
            return &trees[i];
    return NULL;
}

static int height(const avl_node *n) { return n ? n->height : 0; }
static void fix_height(avl_node *n)
{
    int l = height(n->left), r = height(n->right);
    n->height = (l > r ? l : r) + 1;
}
static avl_node *rotate_right(avl_node *n)
{
    avl_node *l = n->left;
    n->left = l->right;
    l->right = n;
    fix_height(n);
    fix_height(l);
    return l;
}
static avl_node *rotate_left(avl_node *n)
{
    avl_node *r = n->right;
    n->right = r->left;
    r->left = n;
    fix_height(n);
    fix_height(r);
    return r;
}
static avl_node *rebalance(avl_node *n)
{
    fix_height(n);
    int b = height(n->right) - height(n->left);
    if (b > 1) {
        if (height(n->right->left) > height(n->right->right))
            n->right = rotate_right(n->right);
        return rotate_left(n);
    }
    if (b < -1) {
        if (height(n->left->right) > height(n->left->left))
            n->left = rotate_left(n->left);
        return rotate_right(n);
    }
    return n;
}

static int node_type(const avl_node *n)
{
    return n->left && n->right ? kAVLIsTree : n->left ? kAVLIsLeftBranch : n->right ? kAVLIsRightBranch : kAVLIsLeaf;
}

/* The program's comparison of an item (or key) with a node's. */
static int32_t compare(avl_tree *t, gaddr item, const avl_node *n)
{
    uint32_t args[4] = { t->tree, item, n->item, (uint32_t)node_type(n) };
    return (int32_t)mm_call(t->compare, uppAVLCompareItemsProcInfo, args, 4);
}

static avl_node *find_node(avl_tree *t, gaddr key)
{
    avl_node *n = t->root;
    while (n) {
        int32_t c = compare(t, key, n);
        if (!c)
            return n->dead ? NULL : n;
        n = c < 0 ? n->left : n->right;
    }
    return NULL;
}

static avl_node *insert_node(avl_tree *t, avl_node *n, avl_node *add, int16_t *err)
{
    if (!n)
        return add;
    int32_t c = compare(t, add->item, n);
    if (!c) {
        *err = errItemAlreadyInTree;
        return n;
    }
    if (c < 0)
        n->left = insert_node(t, n->left, add, err);
    else
        n->right = insert_node(t, n->right, add, err);
    return *err ? n : rebalance(n);
}

static avl_node *remove_min(avl_node *n, avl_node **min)
{
    if (!n->left) {
        *min = n;
        return n->right;
    }
    n->left = remove_min(n->left, min);
    return rebalance(n);
}

/* Unlink node `gone` (found by identity, the tree being ordered). */
static avl_node *unlink_node(avl_tree *t, avl_node *n, avl_node *gone)
{
    if (!n)
        return NULL;
    if (n == gone) {
        if (!n->left || !n->right)
            return n->left ? n->left : n->right;
        avl_node *min;
        avl_node *right = remove_min(n->right, &min);
        min->right = right;
        min->left = n->left;
        return rebalance(min);
    }
    int32_t c = compare(t, gone->item, n);
    if (c < 0)
        n->left = unlink_node(t, n->left, gone);
    else
        n->right = unlink_node(t, n->right, gone);
    return rebalance(n);
}

static void dispose_item(avl_tree *t, avl_node *n)
{
    if (t->dispose_proc) {
        uint32_t args[2] = { t->tree, n->item };
        mm_call(t->dispose_proc, uppAVLDisposeItemProcInfo, args, 2);
    }
}

static void give_item(const avl_node *n, gaddr dataPtr, gaddr itemSize)
{
    if (dataPtr)
        gmemcpy(dataPtr, n->item, n->size);
    if (itemSize)
        wr32(itemSize, n->size);
}

/* The dead (removed while a walk went on) gone for good. */
static void bury(avl_tree *t)
{
    while (t->has_dead) {
        t->has_dead = false;
        avl_node *stack[64], *dead = NULL;
        int sp = 0;
        if (t->root)
            stack[sp++] = t->root;
        while (sp && !dead) {
            avl_node *n = stack[--sp];
            if (n->dead)
                dead = n;
            if (n->left && sp < 64)
                stack[sp++] = n->left;
            if (n->right && sp < 64)
                stack[sp++] = n->right;
        }
        if (dead) {
            t->root = unlink_node(t, t->root, dead);
            tb_DisposePtr(dead->item);
            free(dead);
            t->has_dead = true;
        }
    }
}

int16_t tb_AVLInit(uint32_t flags, gaddr compareItemsProc, gaddr sizeItemProc, gaddr disposeItemProc, gaddr refCon,
                   gaddr tree)
{
    if (!tree || !compareItemsProc || !sizeItemProc)
        return paramErr;
    unsigned i = 0;
    while (i < MAX_TREES && trees[i].tree)
        i++;
    if (i == MAX_TREES)
        return memFullErr;
    gaddr t = tb_NewPtr(36, false, true);
    if (!t)
        return memFullErr;
    wr32(t, AVL_SIGNATURE);
    trees[i] = (avl_tree){ .tree = t, .compare = compareItemsProc, .size_proc = sizeItemProc,
                           .dispose_proc = disposeItemProc, .refcon = refCon, .zone = rd32(LM_TheZone) };
    wr32(tree, t);
    return noErr;
}

static void free_nodes(avl_tree *t, avl_node *n, bool rtl)
{
    if (!n)
        return;
    free_nodes(t, rtl ? n->right : n->left, rtl);
    free_nodes(t, rtl ? n->left : n->right, rtl);
    if (!n->dead)
        dispose_item(t, n);
    tb_DisposePtr(n->item);
    free(n);
}

int16_t tb_AVLDispose(gaddr tree, uint16_t order)
{
    avl_tree *t = tree ? tree_of(rd32(tree)) : NULL;
    if (!t)
        return errNotValidTree;
    if (t->walking)
        return errTreeIsLocked;
    free_nodes(t, t->root, order == kRightToLeft);
    wr32(t->tree, 0);
    tb_DisposePtr(t->tree);
    *t = (avl_tree){ 0 };
    wr32(tree, 0);
    return noErr;
}

/* The walk: each node at its three stages, the level from 1 at the root,
 * its balance (right height less left).  A nonzero result stops it. */
static int16_t walk(avl_tree *t, avl_node *n, gaddr proc, bool rtl, uint32_t level, gaddr refcon)
{
    if (!n)
        return noErr;
    uint32_t args[7] = { t->tree, n->item, kAVLPreOrder, (uint32_t)node_type(n), level,
                         (uint32_t)(height(n->right) - height(n->left)), refcon };
    int16_t e = 0;
    if (!n->dead)
        e = (int16_t)mm_call(proc, uppAVLWalkProcInfo, args, 7);
    if (!e)
        e = walk(t, rtl ? n->right : n->left, proc, rtl, level + 1, refcon);
    if (!e && !n->dead) {
        args[2] = kAVLInOrder;
        e = (int16_t)mm_call(proc, uppAVLWalkProcInfo, args, 7);
    }
    if (!e)
        e = walk(t, rtl ? n->left : n->right, proc, rtl, level + 1, refcon);
    if (!e && !n->dead) {
        args[2] = kAVLPostOrder;
        e = (int16_t)mm_call(proc, uppAVLWalkProcInfo, args, 7);
    }
    return e;
}

int16_t tb_AVLWalk(gaddr tree, gaddr walkProc, uint16_t order, gaddr walkRefCon)
{
    avl_tree *t = tree_of(tree);
    if (!t)
        return errNotValidTree;
    if (!walkProc)
        return paramErr;
    t->walking++;
    int16_t e = walk(t, t->root, walkProc, order == kRightToLeft, 1, walkRefCon);
    if (!--t->walking)
        bury(t);
    return e;
}

int16_t tb_AVLCount(gaddr tree, gaddr count)
{
    avl_tree *t = tree_of(tree);
    if (!t)
        return errNotValidTree;
    if (count)
        wr32(count, t->count);
    return noErr;
}

static avl_node *nth(avl_node *n, uint32_t *k)
{
    if (!n)
        return NULL;
    avl_node *r = nth(n->left, k);
    if (r)
        return r;
    if (!n->dead && !--*k)
        return n;
    return nth(n->right, k);
}

/* The index-th item, from 1, in order. */
int16_t tb_AVLGetIndItem(gaddr tree, uint32_t index, gaddr dataPtr, gaddr itemSize)
{
    avl_tree *t = tree_of(tree);
    if (!t)
        return errNotValidTree;
    uint32_t k = index;
    avl_node *n = index ? nth(t->root, &k) : NULL;
    if (!n)
        return errItemNotFoundInTree;
    give_item(n, dataPtr, itemSize);
    return noErr;
}

int16_t tb_AVLInsert(gaddr tree, gaddr data)
{
    avl_tree *t = tree_of(tree);
    if (!t)
        return errNotValidTree;
    if (t->walking)
        return errCanNotInsertWhileWalkProcInProgress;
    if (!data)
        return paramErr;
    uint32_t args[2] = { t->tree, data };
    uint32_t size = mm_call(t->size_proc, uppAVLItemSizeProcInfo, args, 2);
    if (find_node(t, data))
        return errItemAlreadyInTree;
    gaddr saved = rd32(LM_TheZone);
    wr32(LM_TheZone, t->zone);
    gaddr copy = tb_NewPtr((int32_t)(size ? size : 1), false, false);
    wr32(LM_TheZone, saved);
    if (!copy)
        return memFullErr;
    gmemcpy(copy, data, size);
    avl_node *n = calloc(1, sizeof *n);
    *n = (avl_node){ .height = 1, .item = copy, .size = size };
    int16_t e = noErr;
    t->root = insert_node(t, t->root, n, &e);
    if (e) {
        tb_DisposePtr(copy);
        free(n);
        return e;
    }
    t->count++;
    return noErr;
}

int16_t tb_AVLRemove(gaddr tree, gaddr key, gaddr dataPtr, gaddr itemSize)
{
    avl_tree *t = tree_of(tree);
    if (!t)
        return errNotValidTree;
    avl_node *n = key ? find_node(t, key) : NULL;
    if (!n)
        return errItemNotFoundInTree;
    give_item(n, dataPtr, itemSize);
    dispose_item(t, n);
    t->count--;
    if (t->walking) {
        n->dead = true;
        t->has_dead = true;
        return noErr;
    }
    t->root = unlink_node(t, t->root, n);
    tb_DisposePtr(n->item);
    free(n);
    return noErr;
}

int16_t tb_AVLFind(gaddr tree, gaddr key, gaddr dataPtr, gaddr itemSize)
{
    avl_tree *t = tree_of(tree);
    if (!t)
        return errNotValidTree;
    avl_node *n = key ? find_node(t, key) : NULL;
    if (!n)
        return errItemNotFoundInTree;
    give_item(n, dataPtr, itemSize);
    return noErr;
}

int16_t tb_AVLGetRefcon(gaddr tree, gaddr refCon)
{
    avl_tree *t = tree_of(tree);
    if (refCon)
        wr32(refCon, t ? t->refcon : 0);
    return t ? noErr : errNotValidTree;
}

/* A fresh machine: no dictionary open, no tree. */
void dictionary_reset(void)
{
    for (unsigned i = 0; i < MAX_DICTS; i++)
        if (dicts[i].used)
            free_records(&dicts[i]);
    memset(dicts, 0, sizeof dicts);
    for (unsigned i = 0; i < MAX_TREES; i++) {
        avl_node *stack[128];
        int sp = 0;
        if (trees[i].root)
            stack[sp++] = trees[i].root;
        while (sp) {
            avl_node *n = stack[--sp];
            if (n->left && sp < 128)
                stack[sp++] = n->left;
            if (n->right && sp < 128)
                stack[sp++] = n->right;
            free(n);
        }
    }
    memset(trees, 0, sizeof trees);
}
