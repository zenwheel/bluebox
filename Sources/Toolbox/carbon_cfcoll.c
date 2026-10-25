/*
 * Core Foundation's collections as CarbonLib had them: CFArray,
 * CFDictionary, CFSet, CFBag, CFTree and CFData.
 *
 * A collection's values are guest words, kept on the host in the order
 * they went in.  Its callbacks are the program's: copied from the
 * structure it passed, and called as guest code when they are its own,
 * run here when they are CF's (kCFTypeArrayCallBacks' CFRetain ...).  A
 * dictionary, set or bag keeps each key's hash, so its equal callback
 * runs only on keys that hash alike.
 *
 * CFData's bytes are in guest memory, from the data's allocator (or the
 * program's own, with ...NoCopy), so CFDataGetBytePtr can give them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "carbon_cf.h"

/* The callbacks: version, retain, release, copyDescription, equal, hash. */
typedef struct {
    gaddr retain, release, describe, equal, hash;
} callbacks;

static void read_callbacks(gaddr p, bool hash, callbacks *cb)
{
    memset(cb, 0, sizeof *cb);
    if (!p)
        return;
    cb->retain = rd32(p + 4);
    cb->release = rd32(p + 8);
    cb->describe = rd32(p + 12);
    cb->equal = rd32(p + 16);
    if (hash)
        cb->hash = rd32(p + 20);
}

/* CF's own: CFRetain, CFRelease, CFCopyDescription, CFEqual, CFHash. */
static callbacks type_callbacks(void)
{
    gaddr p = carbon_cf_data_export("kCFTypeDictionaryKeyCallBacks");
    callbacks cb;
    read_callbacks(p, true, &cb);
    return cb;
}

static uint32_t cb_retain(const callbacks *cb, gaddr alloc, uint32_t v)
{
    if (!cb->retain)
        return v;
    if (cf_is_type_retain(cb->retain))
        return cf_retain(v);
    uint32_t args[2] = { cf_allocator_resolve(alloc), v };
    return cf_call(cb->retain, args, 2);
}

static void cb_release(const callbacks *cb, gaddr alloc, uint32_t v)
{
    if (!cb->release)
        return;
    if (cf_is_type_release(cb->release)) {
        cf_release(v);
        return;
    }
    uint32_t args[2] = { cf_allocator_resolve(alloc), v };
    cf_call(cb->release, args, 2);
}

static bool cb_equal(const callbacks *cb, uint32_t a, uint32_t b)
{
    if (!cb->equal)
        return a == b;
    if (cf_is_type_equal(cb->equal))
        return cf_equal(a, b);
    uint32_t args[2] = { a, b };
    return (cf_call(cb->equal, args, 2) & 0xFF) != 0;
}

static uint32_t cb_hash(const callbacks *cb, uint32_t v)
{
    if (!cb->hash)
        return v;
    if (cf_is_type_hash(cb->hash))
        return cf_hash(v);
    return cf_call(cb->hash, &v, 1);
}

static void cb_describe(const callbacks *cb, uint32_t v, cf_text *out)
{
    if (!cb->describe) {
        text_printf(out, "<0x%x>", v);
        return;
    }
    if (cf_is_type_describe(cb->describe)) {
        cf_describe(v, out);
        return;
    }
    gaddr s = cf_call(cb->describe, &v, 1);
    uint32_t n;
    const uint16_t *u = cfstr_chars(s, &n);
    if (u)
        text_add(out, u, n);
    else
        text_printf(out, "<0x%x>", v);
    cf_release(s);
}

static void write_callbacks(gaddr p, const callbacks *cb, bool hash)
{
    if (!p)
        return;
    wr32(p, 0);
    wr32(p + 4, cb->retain);
    wr32(p + 8, cb->release);
    wr32(p + 12, cb->describe);
    wr32(p + 16, cb->equal);
    if (hash)
        wr32(p + 20, cb->hash);
}

/* Guest words from memory (malloc'd). */
static uint32_t *read_words(gaddr p, uint32_t n)
{
    uint32_t *v = malloc(sizeof *v * (n ? n : 1));
    for (uint32_t i = 0; v && i < n; i++)
        v[i] = p ? rd32(p + 4 * i) : 0;
    return v;
}

/* ---------------------------------------------------------------------- */
/* CFArray */

typedef struct {
    cf_obj o;
    uint32_t *v, n, cap;
    bool mut;
    callbacks cb;
    gaddr block; /* CFStringCreateArrayWithFindResults' ranges */
} cf_array;

static cf_array *array_of(gaddr ref) { return cf_get_type(ref, CFT_ARRAY); }

static bool array_room(cf_array *a, uint32_t more)
{
    if (a->n + more <= a->cap)
        return true;
    uint32_t cap = (a->n + more) * 2 + 4;
    uint32_t *p = realloc(a->v, sizeof *p * cap);
    if (!p)
        return false;
    a->v = p;
    a->cap = cap;
    return true;
}

/* Insert values at idx (retained). */
static void array_insert(cf_array *a, uint32_t idx, const uint32_t *v, uint32_t n)
{
    if (idx > a->n || !array_room(a, n))
        return;
    memmove(a->v + idx + n, a->v + idx, sizeof *a->v * (a->n - idx));
    for (uint32_t i = 0; i < n; i++)
        a->v[idx + i] = cb_retain(&a->cb, a->o.alloc, v[i]);
    a->n += n;
}

/* Take out [idx, idx+n) (released). */
static void array_remove(cf_array *a, uint32_t idx, uint32_t n)
{
    if (idx > a->n || n > a->n - idx)
        return;
    uint32_t *gone = malloc(sizeof *gone * (n ? n : 1));
    if (!gone)
        return;
    memcpy(gone, a->v + idx, sizeof *gone * n);
    memmove(a->v + idx, a->v + idx + n, sizeof *a->v * (a->n - idx - n));
    a->n -= n;
    for (uint32_t i = 0; i < n; i++)
        cb_release(&a->cb, a->o.alloc, gone[i]);
    free(gone);
}

static void array_finalize(cf_obj *o)
{
    cf_array *a = (cf_array *)o;
    array_remove(a, 0, a->n);
    if (a->block)
        cf_deallocate(a->o.alloc, a->block);
}

static void array_dispose(cf_obj *o) { free(((cf_array *)o)->v); }

static bool array_equal(cf_obj *x, cf_obj *y)
{
    cf_array *a = (cf_array *)x, *b = (cf_array *)y;
    if (a->n != b->n)
        return false;
    for (uint32_t i = 0; i < a->n; i++)
        if (!cb_equal(&a->cb, a->v[i], b->v[i]))
            return false;
    return true;
}

static uint32_t array_hash(cf_obj *o) { return ((cf_array *)o)->n; }

static void array_describe(cf_obj *o, cf_text *out)
{
    cf_array *a = (cf_array *)o;
    text_printf(out, "type = %s, count = %u, values = (\n", a->mut ? "mutable-small" : "immutable", a->n);
    for (uint32_t i = 0; i < a->n; i++) {
        text_printf(out, "\t%u : ", i);
        cb_describe(&a->cb, a->v[i], out);
        text_add_unit(out, '\n');
    }
    text_add_unit(out, ')');
}

static const cf_class array_class = { "CFArray", array_finalize, array_dispose, array_equal, array_hash,
                                      array_describe };

static cf_array *array_make(gaddr alloc, const callbacks *cb, bool mutable_)
{
    cf_array *a = cf_new(CFT_ARRAY, sizeof *a, alloc, 0);
    if (!a)
        return NULL;
    a->mut = mutable_;
    if (cb)
        a->cb = *cb;
    return a;
}

gaddr cfarray_new(gaddr alloc, const uint32_t *v, uint32_t n, bool mutable_)
{
    callbacks cb = type_callbacks();
    cf_array *a = array_make(alloc, &cb, mutable_);
    if (!a)
        return 0;
    array_insert(a, 0, v, n);
    return a->o.ref;
}

uint32_t cfarray_count(gaddr ref)
{
    cf_array *a = array_of(ref);
    return a ? a->n : 0;
}

uint32_t cfarray_get(gaddr ref, uint32_t i)
{
    cf_array *a = array_of(ref);
    return a && i < a->n ? a->v[i] : 0;
}

void cfarray_append(gaddr ref, uint32_t v)
{
    cf_array *a = array_of(ref);
    if (a)
        array_insert(a, a->n, &v, 1);
}

void cfarray_remove_all(gaddr ref)
{
    cf_array *a = array_of(ref);
    if (a)
        array_remove(a, 0, a->n);
}

void cfarray_own_block(gaddr ref, gaddr block)
{
    cf_array *a = array_of(ref);
    if (a)
        a->block = block;
}

static bool arange_ok(cf_array *a, uint32_t loc, uint32_t len) { return a && loc <= a->n && len <= a->n - loc; }

static void cf_CFArrayGetTypeID(ppc_cpu *c) { RETURN(CFT_ARRAY); }

/* CFArrayCreate(allocator, values, numValues, callBacks) */
static void cf_CFArrayCreate(ppc_cpu *c)
{
    callbacks cb;
    read_callbacks(R(3), false, &cb);
    cf_array *a = array_make(R(0), &cb, false);
    if (!a)
        RETURN(0);
    uint32_t *v = read_words(R(1), R(2));
    if (v)
        array_insert(a, 0, v, R(2));
    free(v);
    RETURN(a->o.ref);
}

static gaddr array_copy(gaddr alloc, gaddr from, bool mutable_)
{
    cf_array *src = array_of(from);
    if (!src)
        return 0;
    cf_array *a = array_make(alloc, &src->cb, mutable_);
    if (a)
        array_insert(a, 0, src->v, src->n);
    return a ? a->o.ref : 0;
}

static void cf_CFArrayCreateCopy(ppc_cpu *c) { RETURN(array_copy(R(0), R(1), false)); }

/* CFArrayCreateMutable(allocator, capacity, callBacks) */
static void cf_CFArrayCreateMutable(ppc_cpu *c)
{
    callbacks cb;
    read_callbacks(R(2), false, &cb);
    cf_array *a = array_make(R(0), &cb, true);
    RETURN(a ? a->o.ref : 0);
}

static void cf_CFArrayCreateMutableCopy(ppc_cpu *c) { RETURN(array_copy(R(0), R(2), true)); }

static void cf_CFArrayGetCount(ppc_cpu *c) { RETURN(cfarray_count(R(0))); }

/* CFArrayGetCountOfValue(theArray, range, value) */
static void cf_CFArrayGetCountOfValue(ppc_cpu *c)
{
    cf_array *a = array_of(R(0));
    uint32_t k = 0;
    if (arange_ok(a, R(1), R(2)))
        for (uint32_t i = R(1); i < R(1) + R(2); i++)
            k += cb_equal(&a->cb, a->v[i], R(3));
    RETURN(k);
}

static int64_t array_index(cf_array *a, uint32_t loc, uint32_t len, uint32_t v, bool last)
{
    if (!arange_ok(a, loc, len))
        return -1;
    if (last) {
        for (uint32_t i = loc + len; i-- > loc;)
            if (cb_equal(&a->cb, a->v[i], v))
                return i;
    } else {
        for (uint32_t i = loc; i < loc + len; i++)
            if (cb_equal(&a->cb, a->v[i], v))
                return i;
    }
    return -1;
}

static void cf_CFArrayContainsValue(ppc_cpu *c) { RETURN(array_index(array_of(R(0)), R(1), R(2), R(3), false) >= 0); }
static void cf_CFArrayGetFirstIndexOfValue(ppc_cpu *c)
{
    RETURN((uint32_t)(int32_t)array_index(array_of(R(0)), R(1), R(2), R(3), false));
}
static void cf_CFArrayGetLastIndexOfValue(ppc_cpu *c)
{
    RETURN((uint32_t)(int32_t)array_index(array_of(R(0)), R(1), R(2), R(3), true));
}

static void cf_CFArrayGetValueAtIndex(ppc_cpu *c) { RETURN(cfarray_get(R(0), R(1))); }

/* CFArrayGetValues(theArray, range, values) */
static void cf_CFArrayGetValues(ppc_cpu *c)
{
    cf_array *a = array_of(R(0));
    if (arange_ok(a, R(1), R(2)) && R(3))
        for (uint32_t i = 0; i < R(2); i++)
            wr32(R(3) + 4 * i, a->v[R(1) + i]);
    RETURN_VOID();
}

/* CFArrayApplyFunction(theArray, range, applier, context): over a copy, as
 * the applier may change the array. */
static void cf_CFArrayApplyFunction(ppc_cpu *c)
{
    cf_array *a = array_of(R(0));
    gaddr fn = R(3), ctx = R(4);
    if (!arange_ok(a, R(1), R(2)) || !fn)
        RETURN_VOID();
    uint32_t n = R(2), *v = malloc(sizeof *v * (n ? n : 1));
    if (!v)
        RETURN_VOID();
    memcpy(v, a->v + R(1), sizeof *v * n);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t args[2] = { v[i], ctx };
        cf_call(fn, args, 2);
    }
    free(v);
    RETURN_VOID();
}

static int compare_values(gaddr fn, uint32_t a, uint32_t b, gaddr ctx)
{
    uint32_t args[3] = { a, b, ctx };
    return (int32_t)cf_call(fn, args, 3);
}

/* CFArrayBSearchValues(theArray, range, value, comparator, context): where
 * the value is, or would go (the first greater one, or the range's end). */
static void cf_CFArrayBSearchValues(ppc_cpu *c)
{
    cf_array *a = array_of(R(0));
    uint32_t loc = R(1), len = R(2);
    if (!arange_ok(a, loc, len) || !R(4))
        RETURN(loc);
    uint32_t lo = loc, hi = loc + len;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        int r = compare_values(R(4), a->v[mid], R(3), R(5));
        if (r < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    RETURN(lo);
}

static void cf_CFArrayAppendValue(ppc_cpu *c)
{
    cfarray_append(R(0), R(1));
    RETURN_VOID();
}

/* CFArrayInsertValueAtIndex(theArray, idx, value) */
static void cf_CFArrayInsertValueAtIndex(ppc_cpu *c)
{
    cf_array *a = array_of(R(0));
    uint32_t v = R(2);
    if (a)
        array_insert(a, R(1), &v, 1);
    RETURN_VOID();
}

/* CFArraySetValueAtIndex(theArray, idx, value): at the end, an append. */
static void cf_CFArraySetValueAtIndex(ppc_cpu *c)
{
    cf_array *a = array_of(R(0));
    uint32_t idx = R(1), v = R(2);
    if (!a || idx > a->n)
        RETURN_VOID();
    if (idx == a->n) {
        array_insert(a, idx, &v, 1);
        RETURN_VOID();
    }
    uint32_t old = a->v[idx];
    a->v[idx] = cb_retain(&a->cb, a->o.alloc, v);
    cb_release(&a->cb, a->o.alloc, old);
    RETURN_VOID();
}

static void cf_CFArrayRemoveValueAtIndex(ppc_cpu *c)
{
    cf_array *a = array_of(R(0));
    if (a)
        array_remove(a, R(1), 1);
    RETURN_VOID();
}

static void cf_CFArrayRemoveAllValues(ppc_cpu *c)
{
    cf_array *a = array_of(R(0));
    if (a)
        array_remove(a, 0, a->n);
    RETURN_VOID();
}

/* CFArrayReplaceValues(theArray, range, newValues, newCount) */
static void cf_CFArrayReplaceValues(ppc_cpu *c)
{
    cf_array *a = array_of(R(0));
    if (!arange_ok(a, R(1), R(2)))
        RETURN_VOID();
    uint32_t *v = read_words(R(3), R(4));
    if (!v)
        RETURN_VOID();
    /* The new ones retained before the old ones go, in case they are the same. */
    array_insert(a, R(1) + R(2), v, R(4));
    array_remove(a, R(1), R(2));
    free(v);
    RETURN_VOID();
}

static void cf_CFArrayExchangeValuesAtIndices(ppc_cpu *c)
{
    cf_array *a = array_of(R(0));
    if (a && R(1) < a->n && R(2) < a->n) {
        uint32_t t = a->v[R(1)];
        a->v[R(1)] = a->v[R(2)];
        a->v[R(2)] = t;
    }
    RETURN_VOID();
}

/* A stable merge sort with the program's comparator. */
static void merge_sort(uint32_t *v, uint32_t *tmp, uint32_t n, gaddr fn, gaddr ctx)
{
    if (n < 2)
        return;
    uint32_t h = n / 2;
    merge_sort(v, tmp, h, fn, ctx);
    merge_sort(v + h, tmp, n - h, fn, ctx);
    uint32_t i = 0, j = h, k = 0;
    while (i < h && j < n)
        tmp[k++] = compare_values(fn, v[j], v[i], ctx) < 0 ? v[j++] : v[i++];
    while (i < h)
        tmp[k++] = v[i++];
    while (j < n)
        tmp[k++] = v[j++];
    memcpy(v, tmp, sizeof *v * n);
}

/* CFArraySortValues(theArray, range, comparator, context) */
static void cf_CFArraySortValues(ppc_cpu *c)
{
    cf_array *a = array_of(R(0));
    if (!arange_ok(a, R(1), R(2)) || !R(3))
        RETURN_VOID();
    uint32_t n = R(2), *v = malloc(sizeof *v * (n ? n : 1)), *tmp = malloc(sizeof *tmp * (n ? n : 1));
    if (v && tmp) {
        memcpy(v, a->v + R(1), sizeof *v * n);
        merge_sort(v, tmp, n, R(3), R(4));
        a = array_of(R(0)); /* the comparator may have changed it */
        if (arange_ok(a, R(1), n))
            memcpy(a->v + R(1), v, sizeof *v * n);
    }
    free(v);
    free(tmp);
    RETURN_VOID();
}

/* CFArrayAppendArray(theArray, otherArray, otherRange) */
static void cf_CFArrayAppendArray(ppc_cpu *c)
{
    cf_array *a = array_of(R(0)), *o = array_of(R(1));
    if (a && arange_ok(o, R(2), R(3))) {
        uint32_t *v = malloc(sizeof *v * (R(3) ? R(3) : 1));
        if (v) {
            memcpy(v, o->v + R(2), sizeof *v * R(3));
            array_insert(a, a->n, v, R(3));
        }
        free(v);
    }
    RETURN_VOID();
}

/* ---------------------------------------------------------------------- */
/* CFDictionary, CFSet and CFBag: entries with their keys' hashes */

typedef struct {
    uint32_t key, value, hash, count;
} entry;

typedef struct {
    cf_obj o;
    entry *e;
    uint32_t n, cap;
    bool mut;
    callbacks kcb, vcb; /* a set's or bag's values are its keys */
} cf_hashed;

static cf_hashed *hashed_of(gaddr ref, uint32_t type) { return cf_get_type(ref, type); }

static int64_t hashed_find(cf_hashed *h, uint32_t key)
{
    uint32_t hv = cb_hash(&h->kcb, key);
    for (uint32_t i = 0; i < h->n; i++)
        if (h->e[i].hash == hv && cb_equal(&h->kcb, h->e[i].key, key))
            return i;
    return -1;
}

static bool is_dict(cf_hashed *h) { return h->o.type == CFT_DICTIONARY; }

static void hashed_add(cf_hashed *h, uint32_t key, uint32_t value)
{
    if (h->n == h->cap) {
        uint32_t cap = h->cap * 2 + 8;
        entry *p = realloc(h->e, sizeof *p * cap);
        if (!p)
            return;
        h->e = p;
        h->cap = cap;
    }
    entry *e = &h->e[h->n];
    e->hash = cb_hash(&h->kcb, key);
    e->key = cb_retain(&h->kcb, h->o.alloc, key);
    e->value = is_dict(h) ? cb_retain(&h->vcb, h->o.alloc, value) : e->key;
    e->count = 1;
    h->n++;
}

static void hashed_drop(cf_hashed *h, uint32_t i)
{
    entry e = h->e[i];
    memmove(h->e + i, h->e + i + 1, sizeof *h->e * (h->n - i - 1));
    h->n--;
    for (uint32_t k = 0; k < e.count; k++)
        cb_release(&h->kcb, h->o.alloc, e.key);
    if (is_dict(h))
        cb_release(&h->vcb, h->o.alloc, e.value);
}

/* A dictionary's value, or a set's or bag's member, replaced. */
static void hashed_replace(cf_hashed *h, uint32_t i, uint32_t key, uint32_t value)
{
    entry *e = &h->e[i];
    if (is_dict(h)) {
        uint32_t old = e->value;
        e->value = cb_retain(&h->vcb, h->o.alloc, value);
        cb_release(&h->vcb, h->o.alloc, old);
        return;
    }
    uint32_t old = e->key, n = e->count;
    for (uint32_t k = 0; k < n; k++)
        e->key = cb_retain(&h->kcb, h->o.alloc, key);
    e = &h->e[i];
    e->value = e->key;
    e->hash = cb_hash(&h->kcb, key);
    for (uint32_t k = 0; k < n; k++)
        cb_release(&h->kcb, h->o.alloc, old);
}

static void hashed_finalize(cf_obj *o)
{
    cf_hashed *h = (cf_hashed *)o;
    while (h->n)
        hashed_drop(h, h->n - 1);
}

static void hashed_dispose(cf_obj *o) { free(((cf_hashed *)o)->e); }

static bool hashed_equal(cf_obj *x, cf_obj *y)
{
    cf_hashed *a = (cf_hashed *)x, *b = (cf_hashed *)y;
    if (a->n != b->n)
        return false;
    for (uint32_t i = 0; i < a->n; i++) {
        int64_t j = hashed_find(b, a->e[i].key);
        if (j < 0 || b->e[j].count != a->e[i].count)
            return false;
        if (is_dict(a) && !cb_equal(&a->vcb, a->e[i].value, b->e[j].value))
            return false;
    }
    return true;
}

static uint32_t hashed_hash(cf_obj *o) { return ((cf_hashed *)o)->n; }

static uint32_t total(cf_hashed *h)
{
    uint32_t k = 0;
    for (uint32_t i = 0; i < h->n; i++)
        k += h->e[i].count;
    return k;
}

static void hashed_describe(cf_obj *o, cf_text *out)
{
    cf_hashed *h = (cf_hashed *)o;
    text_printf(out, "type = %s, count = %u, capacity = %u, %s = (\n", h->mut ? "mutable" : "immutable", total(h),
                h->cap, is_dict(h) ? "pairs" : "values");
    for (uint32_t i = 0, k = 0; i < h->n; i++)
        for (uint32_t r = 0; r < h->e[i].count; r++, k++) {
            text_printf(out, "\t%u : ", k);
            cb_describe(&h->kcb, h->e[i].key, out);
            if (is_dict(h)) {
                text_add_utf8(out, " = ");
                cb_describe(&h->vcb, h->e[i].value, out);
            }
            text_add_unit(out, '\n');
        }
    text_add_unit(out, ')');
}

static const cf_class dict_class = { "CFDictionary", hashed_finalize, hashed_dispose, hashed_equal, hashed_hash,
                                     hashed_describe };
static const cf_class set_class = { "CFSet", hashed_finalize, hashed_dispose, hashed_equal, hashed_hash,
                                    hashed_describe };
static const cf_class bag_class = { "CFBag", hashed_finalize, hashed_dispose, hashed_equal, hashed_hash,
                                    hashed_describe };

static cf_hashed *hashed_make(uint32_t type, gaddr alloc, const callbacks *kcb, const callbacks *vcb, bool mutable_)
{
    cf_hashed *h = cf_new(type, sizeof *h, alloc, 0);
    if (!h)
        return NULL;
    h->mut = mutable_;
    if (kcb)
        h->kcb = *kcb;
    if (vcb)
        h->vcb = *vcb;
    return h;
}

static gaddr hashed_copy(uint32_t type, gaddr alloc, gaddr from, bool mutable_)
{
    cf_hashed *src = hashed_of(from, type);
    if (!src)
        return 0;
    cf_hashed *h = hashed_make(type, alloc, &src->kcb, &src->vcb, mutable_);
    for (uint32_t i = 0; h && i < src->n; i++)
        for (uint32_t r = 0; r < src->e[i].count; r++) {
            int64_t j = type == CFT_BAG ? hashed_find(h, src->e[i].key) : -1;
            if (j >= 0) {
                h->e[j].count++;
                cb_retain(&h->kcb, h->o.alloc, src->e[i].key);
            } else {
                hashed_add(h, src->e[i].key, src->e[i].value);
            }
        }
    return h ? h->o.ref : 0;
}

/* Into a dictionary: add only (AddValue), replace only (ReplaceValue), or either (SetValue). */
enum { ADD = 1, REPLACE = 2 };
static void hashed_put(cf_hashed *h, uint32_t key, uint32_t value, int how)
{
    if (!h)
        return;
    int64_t i = hashed_find(h, key);
    if (i >= 0) {
        if (h->o.type == CFT_BAG && how == ADD) {
            h->e[i].count++;
            cb_retain(&h->kcb, h->o.alloc, key);
        } else if (how & REPLACE) {
            hashed_replace(h, (uint32_t)i, key, value);
        }
    } else if (how & ADD) {
        hashed_add(h, key, value);
    }
}

static void hashed_remove(cf_hashed *h, uint32_t key)
{
    int64_t i = h ? hashed_find(h, key) : -1;
    if (i < 0)
        return;
    if (h->e[i].count > 1) {
        h->e[i].count--;
        cb_release(&h->kcb, h->o.alloc, h->e[i].key);
    } else {
        hashed_drop(h, (uint32_t)i);
    }
}

static void hashed_remove_all(cf_hashed *h)
{
    while (h && h->n)
        hashed_drop(h, h->n - 1);
}

/* Each value (with its key), for an applier: copied first. */
static void hashed_apply(cf_hashed *h, gaddr fn, gaddr ctx)
{
    if (!h || !fn)
        return;
    uint32_t n = h->n;
    entry *e = malloc(sizeof *e * (n ? n : 1));
    if (!e)
        return;
    memcpy(e, h->e, sizeof *e * n);
    bool dict = is_dict(h);
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t r = 0; r < e[i].count; r++) {
            uint32_t args[3] = { e[i].key, dict ? e[i].value : ctx, ctx };
            cf_call(fn, args, dict ? 3 : 2);
        }
    free(e);
}

/* Dictionaries */

gaddr cfdict_new(gaddr alloc, bool mutable_)
{
    callbacks cb = type_callbacks(), vcb = cb;
    vcb.hash = 0;
    cf_hashed *h = hashed_make(CFT_DICTIONARY, alloc, &cb, &vcb, mutable_);
    return h ? h->o.ref : 0;
}

void cfdict_set(gaddr d, uint32_t key, uint32_t value) { hashed_put(hashed_of(d, CFT_DICTIONARY), key, value, ADD | REPLACE); }

bool cfdict_get(gaddr d, uint32_t key, uint32_t *value)
{
    cf_hashed *h = hashed_of(d, CFT_DICTIONARY);
    int64_t i = h ? hashed_find(h, key) : -1;
    if (i >= 0 && value)
        *value = h->e[i].value;
    return i >= 0;
}

void cfdict_remove(gaddr d, uint32_t key) { hashed_remove(hashed_of(d, CFT_DICTIONARY), key); }

uint32_t cfdict_count(gaddr d)
{
    cf_hashed *h = hashed_of(d, CFT_DICTIONARY);
    return h ? h->n : 0;
}

uint32_t cfdict_entries(gaddr d, uint32_t **keys, uint32_t **values)
{
    cf_hashed *h = hashed_of(d, CFT_DICTIONARY);
    uint32_t n = h ? h->n : 0;
    *keys = malloc(sizeof **keys * (n ? n : 1));
    *values = malloc(sizeof **values * (n ? n : 1));
    for (uint32_t i = 0; i < n && *keys && *values; i++) {
        (*keys)[i] = h->e[i].key;
        (*values)[i] = h->e[i].value;
    }
    return n;
}

static void cf_CFDictionaryGetTypeID(ppc_cpu *c) { RETURN(CFT_DICTIONARY); }

/* CFDictionaryCreate(allocator, keys, values, numValues, keyCallBacks, valueCallBacks) */
static void cf_CFDictionaryCreate(ppc_cpu *c)
{
    callbacks kcb, vcb;
    read_callbacks(R(4), true, &kcb);
    read_callbacks(R(5), false, &vcb);
    cf_hashed *h = hashed_make(CFT_DICTIONARY, R(0), &kcb, &vcb, false);
    if (!h)
        RETURN(0);
    for (uint32_t i = 0; i < R(3); i++)
        hashed_put(h, rd32(R(1) + 4 * i), R(2) ? rd32(R(2) + 4 * i) : 0, ADD | REPLACE);
    RETURN(h->o.ref);
}

static void cf_CFDictionaryCreateCopy(ppc_cpu *c) { RETURN(hashed_copy(CFT_DICTIONARY, R(0), R(1), false)); }

/* CFDictionaryCreateMutable(allocator, capacity, keyCallBacks, valueCallBacks) */
static void cf_CFDictionaryCreateMutable(ppc_cpu *c)
{
    callbacks kcb, vcb;
    read_callbacks(R(2), true, &kcb);
    read_callbacks(R(3), false, &vcb);
    cf_hashed *h = hashed_make(CFT_DICTIONARY, R(0), &kcb, &vcb, true);
    RETURN(h ? h->o.ref : 0);
}

static void cf_CFDictionaryCreateMutableCopy(ppc_cpu *c) { RETURN(hashed_copy(CFT_DICTIONARY, R(0), R(2), true)); }

static void cf_CFDictionaryGetCount(ppc_cpu *c) { RETURN(cfdict_count(R(0))); }
static void cf_CFDictionaryGetCountOfKey(ppc_cpu *c) { RETURN(cfdict_get(R(0), R(1), NULL)); }
static void cf_CFDictionaryContainsKey(ppc_cpu *c) { RETURN(cfdict_get(R(0), R(1), NULL)); }

static uint32_t count_values(cf_hashed *h, uint32_t v)
{
    uint32_t k = 0;
    for (uint32_t i = 0; h && i < h->n; i++)
        k += cb_equal(&h->vcb, h->e[i].value, v);
    return k;
}

static void cf_CFDictionaryGetCountOfValue(ppc_cpu *c) { RETURN(count_values(hashed_of(R(0), CFT_DICTIONARY), R(1))); }
static void cf_CFDictionaryContainsValue(ppc_cpu *c) { RETURN(count_values(hashed_of(R(0), CFT_DICTIONARY), R(1)) > 0); }

static void cf_CFDictionaryGetValue(ppc_cpu *c)
{
    uint32_t v = 0;
    cfdict_get(R(0), R(1), &v);
    RETURN(v);
}

/* CFDictionaryGetValueIfPresent(theDict, key, *value) */
static void cf_CFDictionaryGetValueIfPresent(ppc_cpu *c)
{
    uint32_t v = 0;
    bool found = cfdict_get(R(0), R(1), &v);
    if (found && R(2))
        wr32(R(2), v);
    RETURN(found);
}

/* CFDictionaryGetKeysAndValues(theDict, keys, values) */
static void cf_CFDictionaryGetKeysAndValues(ppc_cpu *c)
{
    cf_hashed *h = hashed_of(R(0), CFT_DICTIONARY);
    for (uint32_t i = 0; h && i < h->n; i++) {
        if (R(1))
            wr32(R(1) + 4 * i, h->e[i].key);
        if (R(2))
            wr32(R(2) + 4 * i, h->e[i].value);
    }
    RETURN_VOID();
}

/* CFDictionaryApplyFunction(theDict, applier, context): applier(key, value, context) */
static void cf_CFDictionaryApplyFunction(ppc_cpu *c)
{
    hashed_apply(hashed_of(R(0), CFT_DICTIONARY), R(1), R(2));
    RETURN_VOID();
}

static void cf_CFDictionaryAddValue(ppc_cpu *c)
{
    hashed_put(hashed_of(R(0), CFT_DICTIONARY), R(1), R(2), ADD);
    RETURN_VOID();
}

static void cf_CFDictionarySetValue(ppc_cpu *c)
{
    hashed_put(hashed_of(R(0), CFT_DICTIONARY), R(1), R(2), ADD | REPLACE);
    RETURN_VOID();
}

static void cf_CFDictionaryReplaceValue(ppc_cpu *c)
{
    hashed_put(hashed_of(R(0), CFT_DICTIONARY), R(1), R(2), REPLACE);
    RETURN_VOID();
}

static void cf_CFDictionaryRemoveValue(ppc_cpu *c)
{
    hashed_remove(hashed_of(R(0), CFT_DICTIONARY), R(1));
    RETURN_VOID();
}

static void cf_CFDictionaryRemoveAllValues(ppc_cpu *c)
{
    hashed_remove_all(hashed_of(R(0), CFT_DICTIONARY));
    RETURN_VOID();
}

/* Sets and bags: the same calls on their own types. */

static gaddr hashed_values_new(uint32_t type, gaddr alloc, gaddr values, uint32_t n, gaddr cbs, bool mutable_)
{
    callbacks cb;
    read_callbacks(cbs, true, &cb);
    cf_hashed *h = hashed_make(type, alloc, &cb, NULL, mutable_);
    for (uint32_t i = 0; h && i < n; i++)
        hashed_put(h, rd32(values + 4 * i), 0, type == CFT_BAG ? ADD : ADD | REPLACE);
    return h ? h->o.ref : 0;
}

static uint32_t hashed_values(gaddr ref, uint32_t type, uint32_t **values)
{
    cf_hashed *h = hashed_of(ref, type);
    uint32_t n = h ? total(h) : 0, k = 0;
    *values = malloc(sizeof **values * (n ? n : 1));
    for (uint32_t i = 0; h && *values && i < h->n; i++)
        for (uint32_t r = 0; r < h->e[i].count; r++)
            (*values)[k++] = h->e[i].key;
    return n;
}

gaddr cfset_new(gaddr alloc, const uint32_t *v, uint32_t n, bool mutable_)
{
    callbacks cb = type_callbacks();
    cf_hashed *h = hashed_make(CFT_SET, alloc, &cb, NULL, mutable_);
    for (uint32_t i = 0; h && i < n; i++)
        hashed_put(h, v[i], 0, ADD);
    return h ? h->o.ref : 0;
}

gaddr cfbag_new(gaddr alloc, const uint32_t *v, uint32_t n, bool mutable_)
{
    callbacks cb = type_callbacks();
    cf_hashed *h = hashed_make(CFT_BAG, alloc, &cb, NULL, mutable_);
    for (uint32_t i = 0; h && i < n; i++)
        hashed_put(h, v[i], 0, ADD);
    return h ? h->o.ref : 0;
}

uint32_t cfset_entries(gaddr s, uint32_t **values) { return hashed_values(s, CFT_SET, values); }
uint32_t cfbag_entries(gaddr s, uint32_t **values) { return hashed_values(s, CFT_BAG, values); }

#define HASHED_CALLS(Name, TYPE)                                                                            \
    static void cf_##Name##GetTypeID(ppc_cpu *c) { RETURN(TYPE); }                                          \
    static void cf_##Name##Create(ppc_cpu *c) { RETURN(hashed_values_new(TYPE, R(0), R(1), R(2), R(3), false)); } \
    static void cf_##Name##CreateCopy(ppc_cpu *c) { RETURN(hashed_copy(TYPE, R(0), R(1), false)); }         \
    static void cf_##Name##CreateMutable(ppc_cpu *c) { RETURN(hashed_values_new(TYPE, R(0), 0, 0, R(2), true)); } \
    static void cf_##Name##CreateMutableCopy(ppc_cpu *c) { RETURN(hashed_copy(TYPE, R(0), R(2), true)); }   \
    static void cf_##Name##GetCount(ppc_cpu *c)                                                             \
    {                                                                                                       \
        cf_hashed *h = hashed_of(R(0), TYPE);                                                               \
        RETURN(h ? total(h) : 0);                                                                           \
    }                                                                                                       \
    static void cf_##Name##GetCountOfValue(ppc_cpu *c)                                                      \
    {                                                                                                       \
        cf_hashed *h = hashed_of(R(0), TYPE);                                                               \
        int64_t i = h ? hashed_find(h, R(1)) : -1;                                                          \
        RETURN(i >= 0 ? h->e[i].count : 0);                                                                 \
    }                                                                                                       \
    static void cf_##Name##ContainsValue(ppc_cpu *c)                                                        \
    {                                                                                                       \
        cf_hashed *h = hashed_of(R(0), TYPE);                                                               \
        RETURN(h && hashed_find(h, R(1)) >= 0);                                                             \
    }                                                                                                       \
    static void cf_##Name##GetValue(ppc_cpu *c)                                                             \
    {                                                                                                       \
        cf_hashed *h = hashed_of(R(0), TYPE);                                                               \
        int64_t i = h ? hashed_find(h, R(1)) : -1;                                                          \
        RETURN(i >= 0 ? h->e[i].key : 0);                                                                   \
    }                                                                                                       \
    static void cf_##Name##GetValueIfPresent(ppc_cpu *c)                                                    \
    {                                                                                                       \
        cf_hashed *h = hashed_of(R(0), TYPE);                                                               \
        int64_t i = h ? hashed_find(h, R(1)) : -1;                                                          \
        if (i >= 0 && R(2))                                                                                 \
            wr32(R(2), h->e[i].key);                                                                        \
        RETURN(i >= 0);                                                                                     \
    }                                                                                                       \
    static void cf_##Name##GetValues(ppc_cpu *c)                                                            \
    {                                                                                                       \
        uint32_t *v, n = hashed_values(R(0), TYPE, &v);                                                     \
        for (uint32_t i = 0; v && R(1) && i < n; i++)                                                       \
            wr32(R(1) + 4 * i, v[i]);                                                                       \
        free(v);                                                                                            \
        RETURN_VOID();                                                                                      \
    }                                                                                                       \
    static void cf_##Name##ApplyFunction(ppc_cpu *c)                                                        \
    {                                                                                                       \
        hashed_apply(hashed_of(R(0), TYPE), R(1), R(2));                                                    \
        RETURN_VOID();                                                                                      \
    }                                                                                                       \
    static void cf_##Name##AddValue(ppc_cpu *c)                                                             \
    {                                                                                                       \
        hashed_put(hashed_of(R(0), TYPE), R(1), 0, ADD);                                                    \
        RETURN_VOID();                                                                                      \
    }                                                                                                       \
    static void cf_##Name##ReplaceValue(ppc_cpu *c)                                                         \
    {                                                                                                       \
        hashed_put(hashed_of(R(0), TYPE), R(1), 0, REPLACE);                                                \
        RETURN_VOID();                                                                                      \
    }                                                                                                       \
    static void cf_##Name##SetValue(ppc_cpu *c)                                                             \
    {                                                                                                       \
        hashed_put(hashed_of(R(0), TYPE), R(1), 0, ADD | REPLACE);                                          \
        RETURN_VOID();                                                                                      \
    }                                                                                                       \
    static void cf_##Name##RemoveValue(ppc_cpu *c)                                                          \
    {                                                                                                       \
        hashed_remove(hashed_of(R(0), TYPE), R(1));                                                         \
        RETURN_VOID();                                                                                      \
    }                                                                                                       \
    static void cf_##Name##RemoveAllValues(ppc_cpu *c)                                                      \
    {                                                                                                       \
        hashed_remove_all(hashed_of(R(0), TYPE));                                                           \
        RETURN_VOID();                                                                                      \
    }

HASHED_CALLS(CFSet, CFT_SET)
HASHED_CALLS(CFBag, CFT_BAG)

/* ---------------------------------------------------------------------- */
/* CFTree */

/* CFTreeContext: version, info, retain, release, copyDescription. */
typedef struct {
    cf_obj o;
    gaddr parent;
    gaddr *kids;
    uint32_t n, cap;
    uint32_t info, retain, release, describe;
} cf_tree;

static cf_tree *tree_of(gaddr ref) { return cf_get_type(ref, CFT_TREE); }

static void tree_set_context(cf_tree *t, gaddr ctx)
{
    uint32_t info = ctx ? rd32(ctx + 4) : 0, retain = ctx ? rd32(ctx + 8) : 0;
    if (retain)
        info = cf_call_info(retain, info);
    uint32_t old = t->info, old_release = t->release;
    t->info = info;
    t->retain = retain;
    t->release = ctx ? rd32(ctx + 12) : 0;
    t->describe = ctx ? rd32(ctx + 16) : 0;
    if (old_release)
        cf_call_info(old_release, old);
}

static void tree_detach(cf_tree *t, uint32_t i)
{
    memmove(t->kids + i, t->kids + i + 1, sizeof *t->kids * (t->n - i - 1));
    t->n--;
}

static void tree_remove_all(cf_tree *t)
{
    while (t->n) {
        gaddr k = t->kids[t->n - 1];
        tree_detach(t, t->n - 1);
        cf_tree *kt = tree_of(k);
        if (kt)
            kt->parent = 0;
        cf_release(k);
    }
}

static void tree_finalize(cf_obj *o)
{
    cf_tree *t = (cf_tree *)o;
    tree_remove_all(t);
    if (t->release)
        cf_call_info(t->release, t->info);
}

static void tree_dispose(cf_obj *o) { free(((cf_tree *)o)->kids); }

static void tree_describe(cf_obj *o, cf_text *out)
{
    cf_tree *t = (cf_tree *)o;
    text_printf(out, "children = %u, context = ", t->n);
    if (t->describe) {
        gaddr s = cf_call_info(t->describe, t->info);
        uint32_t n;
        const uint16_t *u = cfstr_chars(s, &n);
        if (u)
            text_add(out, u, n);
        cf_release(s);
    } else {
        text_printf(out, "<0x%x>", t->info);
    }
}

static const cf_class tree_class = { "CFTree", tree_finalize, tree_dispose, NULL, NULL, tree_describe };

static bool tree_insert(cf_tree *t, uint32_t at, gaddr child)
{
    cf_tree *k = tree_of(child);
    if (!k || k->parent || child == t->o.ref)
        return false;
    if (t->n == t->cap) {
        uint32_t cap = t->cap * 2 + 4;
        gaddr *p = realloc(t->kids, sizeof *p * cap);
        if (!p)
            return false;
        t->kids = p;
        t->cap = cap;
    }
    memmove(t->kids + at + 1, t->kids + at, sizeof *t->kids * (t->n - at));
    t->kids[at] = cf_retain(child);
    t->n++;
    k->parent = t->o.ref;
    return true;
}

static void cf_CFTreeGetTypeID(ppc_cpu *c) { RETURN(CFT_TREE); }

/* CFTreeCreate(allocator, context) */
static void cf_CFTreeCreate(ppc_cpu *c)
{
    cf_tree *t = cf_new(CFT_TREE, sizeof *t, R(0), 0);
    if (t)
        tree_set_context(t, R(1));
    RETURN(t ? t->o.ref : 0);
}

gaddr cftree_new_with_info(gaddr alloc, gaddr info)
{
    cf_tree *t = cf_new(CFT_TREE, sizeof *t, alloc, 0);
    if (!t)
        return 0;
    t->info = cf_retain(info);
    t->retain = cf_tv_retain();
    t->release = cf_tv_release();
    t->describe = cf_tv_describe();
    return t->o.ref;
}

void cftree_append(gaddr tree, gaddr child)
{
    cf_tree *t = tree_of(tree);
    if (t)
        tree_insert(t, t->n, child);
}

uint32_t cftree_children(gaddr tree, gaddr **children)
{
    cf_tree *t = tree_of(tree);
    uint32_t n = t ? t->n : 0;
    *children = malloc(sizeof **children * (n ? n : 1));
    if (*children && n)
        memcpy(*children, t->kids, sizeof **children * n);
    return n;
}

gaddr cftree_info(gaddr tree)
{
    cf_tree *t = tree_of(tree);
    return t ? t->info : 0;
}

gaddr cftree_parent(gaddr tree)
{
    cf_tree *t = tree_of(tree);
    return t ? t->parent : 0;
}

static void cf_CFTreeGetParent(ppc_cpu *c) { RETURN(cftree_parent(R(0))); }

static int64_t index_in_parent(cf_tree *t)
{
    cf_tree *p = t ? tree_of(t->parent) : NULL;
    for (uint32_t i = 0; p && i < p->n; i++)
        if (p->kids[i] == t->o.ref)
            return i;
    return -1;
}

static void cf_CFTreeGetNextSibling(ppc_cpu *c)
{
    cf_tree *t = tree_of(R(0));
    int64_t i = index_in_parent(t);
    cf_tree *p = i >= 0 ? tree_of(t->parent) : NULL;
    RETURN(p && i + 1 < p->n ? p->kids[i + 1] : 0);
}

static void cf_CFTreeGetFirstChild(ppc_cpu *c)
{
    cf_tree *t = tree_of(R(0));
    RETURN(t && t->n ? t->kids[0] : 0);
}

/* CFTreeGetContext(tree, context) */
static void cf_CFTreeGetContext(ppc_cpu *c)
{
    cf_tree *t = tree_of(R(0));
    gaddr ctx = R(1);
    if (t && ctx) {
        wr32(ctx, 0);
        wr32(ctx + 4, t->info);
        wr32(ctx + 8, t->retain);
        wr32(ctx + 12, t->release);
        wr32(ctx + 16, t->describe);
    }
    RETURN_VOID();
}

static void cf_CFTreeGetChildCount(ppc_cpu *c)
{
    cf_tree *t = tree_of(R(0));
    RETURN(t ? t->n : 0);
}

static void cf_CFTreeGetChildAtIndex(ppc_cpu *c)
{
    cf_tree *t = tree_of(R(0));
    RETURN(t && R(1) < t->n ? t->kids[R(1)] : 0);
}

static void cf_CFTreeGetChildren(ppc_cpu *c)
{
    cf_tree *t = tree_of(R(0));
    for (uint32_t i = 0; t && R(1) && i < t->n; i++)
        wr32(R(1) + 4 * i, t->kids[i]);
    RETURN_VOID();
}

/* CFTreeApplyFunctionToChildren(tree, applier, context) */
static void cf_CFTreeApplyFunctionToChildren(ppc_cpu *c)
{
    gaddr *k;
    uint32_t n = cftree_children(R(0), &k);
    for (uint32_t i = 0; k && R(1) && i < n; i++) {
        uint32_t args[2] = { k[i], R(2) };
        cf_call(R(1), args, 2);
    }
    free(k);
    RETURN_VOID();
}

static void cf_CFTreeFindRoot(ppc_cpu *c)
{
    gaddr t = R(0);
    while (cftree_parent(t))
        t = cftree_parent(t);
    RETURN(tree_of(t) ? t : 0);
}

static void cf_CFTreeSetContext(ppc_cpu *c)
{
    cf_tree *t = tree_of(R(0));
    if (t)
        tree_set_context(t, R(1));
    RETURN_VOID();
}

static void cf_CFTreePrependChild(ppc_cpu *c)
{
    cf_tree *t = tree_of(R(0));
    if (t)
        tree_insert(t, 0, R(1));
    RETURN_VOID();
}

static void cf_CFTreeAppendChild(ppc_cpu *c)
{
    cftree_append(R(0), R(1));
    RETURN_VOID();
}

/* CFTreeInsertSibling(tree, newSibling): after tree, in its parent. */
static void cf_CFTreeInsertSibling(ppc_cpu *c)
{
    cf_tree *t = tree_of(R(0));
    int64_t i = index_in_parent(t);
    if (i >= 0)
        tree_insert(tree_of(t->parent), (uint32_t)i + 1, R(1));
    RETURN_VOID();
}

/* CFTreeRemove(tree): out of its parent, which lets it go. */
static void cf_CFTreeRemove(ppc_cpu *c)
{
    cf_tree *t = tree_of(R(0));
    int64_t i = index_in_parent(t);
    if (i >= 0) {
        tree_detach(tree_of(t->parent), (uint32_t)i);
        t->parent = 0;
        cf_release(R(0));
    }
    RETURN_VOID();
}

static void cf_CFTreeRemoveAllChildren(ppc_cpu *c)
{
    cf_tree *t = tree_of(R(0));
    if (t)
        tree_remove_all(t);
    RETURN_VOID();
}

/* CFTreeSortChildren(tree, comparator, context): the comparator gets the
 * children's infos. */
static void cf_CFTreeSortChildren(ppc_cpu *c)
{
    cf_tree *t = tree_of(R(0));
    if (!t || !R(1) || t->n < 2)
        RETURN_VOID();
    uint32_t n = t->n, *info = malloc(sizeof *info * n), *tmp = malloc(sizeof *tmp * n);
    gaddr *kids = malloc(sizeof *kids * n);
    if (info && tmp && kids) {
        memcpy(kids, t->kids, sizeof *kids * n);
        for (uint32_t i = 0; i < n; i++)
            info[i] = i;
        /* Sort the indices, comparing the children's infos. */
        for (uint32_t w = 1; w < n; w *= 2)
            for (uint32_t lo = 0; lo < n; lo += 2 * w) {
                uint32_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
                uint32_t a = lo, b = mid, k = lo;
                while (a < mid && b < hi)
                    tmp[k++] = compare_values(R(1), cftree_info(kids[info[b]]), cftree_info(kids[info[a]]), R(2)) < 0
                                   ? info[b++]
                                   : info[a++];
                while (a < mid)
                    tmp[k++] = info[a++];
                while (b < hi)
                    tmp[k++] = info[b++];
                memcpy(info + lo, tmp + lo, sizeof *info * (hi - lo));
            }
        t = tree_of(R(0));
        if (t && t->n == n)
            for (uint32_t i = 0; i < n; i++)
                t->kids[i] = kids[info[i]];
    }
    free(info);
    free(tmp);
    free(kids);
    RETURN_VOID();
}

/* ---------------------------------------------------------------------- */
/* CFData */

typedef struct {
    cf_obj o;
    gaddr bytes;
    uint32_t len, cap;
    bool mut;
    gaddr bytes_alloc; /* who frees the bytes */
    bool owned;
} cf_data;

static cf_data *data_of(gaddr ref) { return cf_get_type(ref, CFT_DATA); }

static void data_finalize(cf_obj *o)
{
    cf_data *d = (cf_data *)o;
    if (d->bytes && d->owned)
        cf_deallocate(d->bytes_alloc, d->bytes);
}

static bool data_equal(cf_obj *x, cf_obj *y)
{
    cf_data *a = (cf_data *)x, *b = (cf_data *)y;
    return a->len == b->len && (!a->len || !memcmp(gptr(a->bytes), gptr(b->bytes), a->len));
}

static uint32_t data_hash(cf_obj *o)
{
    cf_data *d = (cf_data *)o;
    uint32_t h = d->len, n = d->len < 80 ? d->len : 80;
    for (uint32_t i = 0; i < n; i++)
        h = h * 31 + rd8(d->bytes + i);
    return h;
}

static void data_describe(cf_obj *o, cf_text *out)
{
    cf_data *d = (cf_data *)o;
    text_printf(out, "length = %u, capacity = %u, bytes = 0x", d->len, d->mut ? d->cap : d->len);
    for (uint32_t i = 0; i < d->len; i++) {
        if (d->len > 32 && i == 24) {
            text_add_utf8(out, " ... ");
            i = d->len - 8;
        }
        text_printf(out, "%02x", rd8(d->bytes + i));
    }
}

static const cf_class data_class = { "CFData", data_finalize, NULL, data_equal, data_hash, data_describe };

/* Room for len bytes. */
static bool data_room(cf_data *d, uint32_t len)
{
    if (len <= d->cap && d->bytes)
        return true;
    if (!d->owned) { /* the program's bytes: ours from now on */
        gaddr p = cf_allocate(d->o.alloc, len + 16);
        if (!p)
            return false;
        if (d->len)
            gmemcpy(p, d->bytes, d->len);
        if (d->bytes && d->bytes_alloc)
            cf_deallocate(d->bytes_alloc, d->bytes);
        d->bytes = p;
        d->cap = len + 16;
        d->owned = true;
        d->bytes_alloc = d->o.alloc;
        return true;
    }
    uint32_t cap = len * 2 + 16;
    gaddr p = d->bytes ? cf_reallocate(d->bytes_alloc, d->bytes, cap, d->cap) : cf_allocate(d->bytes_alloc, cap);
    if (!p)
        return false;
    d->bytes = p;
    d->cap = cap;
    return true;
}

static cf_data *data_make(gaddr alloc, gaddr from, const uint8_t *host, uint32_t len, bool mutable_)
{
    cf_data *d = cf_new(CFT_DATA, sizeof *d, alloc, 0);
    if (!d)
        return NULL;
    d->mut = mutable_;
    d->owned = true;
    d->bytes_alloc = d->o.alloc;
    if (!data_room(d, len)) {
        cf_release(d->o.ref);
        return NULL;
    }
    if (from && len)
        gmemcpy(d->bytes, from, len);
    else if (host && len)
        gput(d->bytes, host, len);
    d->len = len;
    return d;
}

gaddr cfdata_new(gaddr alloc, const uint8_t *b, uint32_t n, bool mutable_)
{
    cf_data *d = data_make(alloc, 0, b, n, mutable_);
    return d ? d->o.ref : 0;
}

gaddr cfdata_bytes(gaddr ref, uint32_t *n)
{
    cf_data *d = data_of(ref);
    *n = d ? d->len : 0;
    return d ? d->bytes : 0;
}

static void cf_CFDataGetTypeID(ppc_cpu *c) { RETURN(CFT_DATA); }

/* CFDataCreate(allocator, bytes, length) */
static void cf_CFDataCreate(ppc_cpu *c)
{
    cf_data *d = (int32_t)R(2) >= 0 ? data_make(R(0), R(1), NULL, R(2), false) : NULL;
    RETURN(d ? d->o.ref : 0);
}

/* CFDataCreateWithBytesNoCopy(allocator, bytes, length, bytesDeallocator) */
static void cf_CFDataCreateWithBytesNoCopy(ppc_cpu *c)
{
    cf_data *d = cf_new(CFT_DATA, sizeof *d, R(0), 0);
    if (!d)
        RETURN(0);
    d->bytes = R(1);
    d->len = d->cap = R(2);
    d->owned = R(3) != cf_allocator_null();
    d->bytes_alloc = R(3);
    RETURN(d->o.ref);
}

static void cf_CFDataCreateCopy(ppc_cpu *c)
{
    cf_data *s = data_of(R(1)), *d = s ? data_make(R(0), s->bytes, NULL, s->len, false) : NULL;
    RETURN(d ? d->o.ref : 0);
}

/* CFDataCreateMutable(allocator, capacity) */
static void cf_CFDataCreateMutable(ppc_cpu *c)
{
    cf_data *d = data_make(R(0), 0, NULL, 0, true);
    RETURN(d ? d->o.ref : 0);
}

/* CFDataCreateMutableCopy(allocator, capacity, theData) */
static void cf_CFDataCreateMutableCopy(ppc_cpu *c)
{
    cf_data *s = data_of(R(2)), *d = s ? data_make(R(0), s->bytes, NULL, s->len, true) : NULL;
    RETURN(d ? d->o.ref : 0);
}

static void cf_CFDataGetLength(ppc_cpu *c)
{
    cf_data *d = data_of(R(0));
    RETURN(d ? d->len : 0);
}

static void cf_CFDataGetBytePtr(ppc_cpu *c)
{
    cf_data *d = data_of(R(0));
    RETURN(d ? d->bytes : 0);
}

/* CFDataGetBytes(theData, range, buffer) */
static void cf_CFDataGetBytes(ppc_cpu *c)
{
    cf_data *d = data_of(R(0));
    if (d && R(3) && R(1) <= d->len && R(2) <= d->len - R(1) && R(2))
        gmemcpy(R(3), d->bytes + R(1), R(2));
    RETURN_VOID();
}

/* Replace [loc, loc+len) with n bytes (from guest memory, or zeros). */
static void data_replace(cf_data *d, uint32_t loc, uint32_t len, gaddr from, uint32_t n)
{
    if (!d || loc > d->len || len > d->len - loc)
        return;
    uint32_t total = d->len - len + n;
    uint8_t *copy = NULL;
    if (from && n) { /* the bytes may be the data's own */
        copy = malloc(n);
        if (!copy)
            return;
        gget(copy, from, n);
    }
    if (!data_room(d, total)) {
        free(copy);
        return;
    }
    gmemcpy(d->bytes + loc + n, d->bytes + loc + len, d->len - loc - len);
    if (copy)
        gput(d->bytes + loc, copy, n);
    else if (n)
        gmemset(d->bytes + loc, 0, n);
    d->len = total;
    free(copy);
}

/* CFDataSetLength(theData, length): longer is zeros. */
static void cf_CFDataSetLength(ppc_cpu *c)
{
    cf_data *d = data_of(R(0));
    if (d && (int32_t)R(1) >= 0) {
        if (R(1) < d->len)
            data_replace(d, R(1), d->len - R(1), 0, 0);
        else
            data_replace(d, d->len, 0, 0, R(1) - d->len);
    }
    RETURN_VOID();
}

static void cf_CFDataIncreaseLength(ppc_cpu *c)
{
    cf_data *d = data_of(R(0));
    if (d && (int32_t)R(1) > 0)
        data_replace(d, d->len, 0, 0, R(1));
    RETURN_VOID();
}

/* CFDataAppendBytes(theData, bytes, length) */
static void cf_CFDataAppendBytes(ppc_cpu *c)
{
    cf_data *d = data_of(R(0));
    if (d && R(1) && (int32_t)R(2) > 0)
        data_replace(d, d->len, 0, R(1), R(2));
    RETURN_VOID();
}

/* CFDataReplaceBytes(theData, range, newBytes, newLength) */
static void cf_CFDataReplaceBytes(ppc_cpu *c)
{
    if ((int32_t)R(4) >= 0)
        data_replace(data_of(R(0)), R(1), R(2), R(3), R(4));
    RETURN_VOID();
}

/* CFDataDeleteBytes(theData, range) */
static void cf_CFDataDeleteBytes(ppc_cpu *c)
{
    data_replace(data_of(R(0)), R(1), R(2), 0, 0);
    RETURN_VOID();
}

/* ---------------------------------------------------------------------- */

void cfcoll_reset(void)
{
    cf_register_class(CFT_ARRAY, &array_class);
    cf_register_class(CFT_DICTIONARY, &dict_class);
    cf_register_class(CFT_SET, &set_class);
    cf_register_class(CFT_BAG, &bag_class);
    cf_register_class(CFT_TREE, &tree_class);
    cf_register_class(CFT_DATA, &data_class);
}

const ppc_export cfcoll_exports[] = {
    { "CFArrayGetTypeID", cf_CFArrayGetTypeID },
    { "CFArrayCreate", cf_CFArrayCreate },
    { "CFArrayCreateCopy", cf_CFArrayCreateCopy },
    { "CFArrayCreateMutable", cf_CFArrayCreateMutable },
    { "CFArrayCreateMutableCopy", cf_CFArrayCreateMutableCopy },
    { "CFArrayGetCount", cf_CFArrayGetCount },
    { "CFArrayGetCountOfValue", cf_CFArrayGetCountOfValue },
    { "CFArrayContainsValue", cf_CFArrayContainsValue },
    { "CFArrayGetValueAtIndex", cf_CFArrayGetValueAtIndex },
    { "CFArrayGetValues", cf_CFArrayGetValues },
    { "CFArrayApplyFunction", cf_CFArrayApplyFunction },
    { "CFArrayGetFirstIndexOfValue", cf_CFArrayGetFirstIndexOfValue },
    { "CFArrayGetLastIndexOfValue", cf_CFArrayGetLastIndexOfValue },
    { "CFArrayBSearchValues", cf_CFArrayBSearchValues },
    { "CFArrayAppendValue", cf_CFArrayAppendValue },
    { "CFArrayInsertValueAtIndex", cf_CFArrayInsertValueAtIndex },
    { "CFArraySetValueAtIndex", cf_CFArraySetValueAtIndex },
    { "CFArrayRemoveValueAtIndex", cf_CFArrayRemoveValueAtIndex },
    { "CFArrayRemoveAllValues", cf_CFArrayRemoveAllValues },
    { "CFArrayReplaceValues", cf_CFArrayReplaceValues },
    { "CFArrayExchangeValuesAtIndices", cf_CFArrayExchangeValuesAtIndices },
    { "CFArraySortValues", cf_CFArraySortValues },
    { "CFArrayAppendArray", cf_CFArrayAppendArray },
    { "CFDictionaryGetTypeID", cf_CFDictionaryGetTypeID },
    { "CFDictionaryCreate", cf_CFDictionaryCreate },
    { "CFDictionaryCreateCopy", cf_CFDictionaryCreateCopy },
    { "CFDictionaryCreateMutable", cf_CFDictionaryCreateMutable },
    { "CFDictionaryCreateMutableCopy", cf_CFDictionaryCreateMutableCopy },
    { "CFDictionaryGetCount", cf_CFDictionaryGetCount },
    { "CFDictionaryGetCountOfKey", cf_CFDictionaryGetCountOfKey },
    { "CFDictionaryGetCountOfValue", cf_CFDictionaryGetCountOfValue },
    { "CFDictionaryContainsKey", cf_CFDictionaryContainsKey },
    { "CFDictionaryContainsValue", cf_CFDictionaryContainsValue },
    { "CFDictionaryGetValue", cf_CFDictionaryGetValue },
    { "CFDictionaryGetValueIfPresent", cf_CFDictionaryGetValueIfPresent },
    { "CFDictionaryGetKeysAndValues", cf_CFDictionaryGetKeysAndValues },
    { "CFDictionaryApplyFunction", cf_CFDictionaryApplyFunction },
    { "CFDictionaryAddValue", cf_CFDictionaryAddValue },
    { "CFDictionarySetValue", cf_CFDictionarySetValue },
    { "CFDictionaryReplaceValue", cf_CFDictionaryReplaceValue },
    { "CFDictionaryRemoveValue", cf_CFDictionaryRemoveValue },
    { "CFDictionaryRemoveAllValues", cf_CFDictionaryRemoveAllValues },
    { "CFSetGetTypeID", cf_CFSetGetTypeID },
    { "CFSetCreate", cf_CFSetCreate },
    { "CFSetCreateCopy", cf_CFSetCreateCopy },
    { "CFSetCreateMutable", cf_CFSetCreateMutable },
    { "CFSetCreateMutableCopy", cf_CFSetCreateMutableCopy },
    { "CFSetGetCount", cf_CFSetGetCount },
    { "CFSetGetCountOfValue", cf_CFSetGetCountOfValue },
    { "CFSetContainsValue", cf_CFSetContainsValue },
    { "CFSetGetValue", cf_CFSetGetValue },
    { "CFSetGetValueIfPresent", cf_CFSetGetValueIfPresent },
    { "CFSetGetValues", cf_CFSetGetValues },
    { "CFSetApplyFunction", cf_CFSetApplyFunction },
    { "CFSetAddValue", cf_CFSetAddValue },
    { "CFSetReplaceValue", cf_CFSetReplaceValue },
    { "CFSetSetValue", cf_CFSetSetValue },
    { "CFSetRemoveValue", cf_CFSetRemoveValue },
    { "CFSetRemoveAllValues", cf_CFSetRemoveAllValues },
    { "CFBagGetTypeID", cf_CFBagGetTypeID },
    { "CFBagCreate", cf_CFBagCreate },
    { "CFBagCreateCopy", cf_CFBagCreateCopy },
    { "CFBagCreateMutable", cf_CFBagCreateMutable },
    { "CFBagCreateMutableCopy", cf_CFBagCreateMutableCopy },
    { "CFBagGetCount", cf_CFBagGetCount },
    { "CFBagGetCountOfValue", cf_CFBagGetCountOfValue },
    { "CFBagContainsValue", cf_CFBagContainsValue },
    { "CFBagGetValue", cf_CFBagGetValue },
    { "CFBagGetValueIfPresent", cf_CFBagGetValueIfPresent },
    { "CFBagGetValues", cf_CFBagGetValues },
    { "CFBagApplyFunction", cf_CFBagApplyFunction },
    { "CFBagAddValue", cf_CFBagAddValue },
    { "CFBagReplaceValue", cf_CFBagReplaceValue },
    { "CFBagSetValue", cf_CFBagSetValue },
    { "CFBagRemoveValue", cf_CFBagRemoveValue },
    { "CFBagRemoveAllValues", cf_CFBagRemoveAllValues },
    { "CFTreeGetTypeID", cf_CFTreeGetTypeID },
    { "CFTreeCreate", cf_CFTreeCreate },
    { "CFTreeGetParent", cf_CFTreeGetParent },
    { "CFTreeGetNextSibling", cf_CFTreeGetNextSibling },
    { "CFTreeGetFirstChild", cf_CFTreeGetFirstChild },
    { "CFTreeGetContext", cf_CFTreeGetContext },
    { "CFTreeGetChildCount", cf_CFTreeGetChildCount },
    { "CFTreeGetChildAtIndex", cf_CFTreeGetChildAtIndex },
    { "CFTreeGetChildren", cf_CFTreeGetChildren },
    { "CFTreeApplyFunctionToChildren", cf_CFTreeApplyFunctionToChildren },
    { "CFTreeFindRoot", cf_CFTreeFindRoot },
    { "CFTreeSetContext", cf_CFTreeSetContext },
    { "CFTreePrependChild", cf_CFTreePrependChild },
    { "CFTreeAppendChild", cf_CFTreeAppendChild },
    { "CFTreeInsertSibling", cf_CFTreeInsertSibling },
    { "CFTreeRemove", cf_CFTreeRemove },
    { "CFTreeRemoveAllChildren", cf_CFTreeRemoveAllChildren },
    { "CFTreeSortChildren", cf_CFTreeSortChildren },
    { "CFDataGetTypeID", cf_CFDataGetTypeID },
    { "CFDataCreate", cf_CFDataCreate },
    { "CFDataCreateWithBytesNoCopy", cf_CFDataCreateWithBytesNoCopy },
    { "CFDataCreateCopy", cf_CFDataCreateCopy },
    { "CFDataCreateMutable", cf_CFDataCreateMutable },
    { "CFDataCreateMutableCopy", cf_CFDataCreateMutableCopy },
    { "CFDataGetLength", cf_CFDataGetLength },
    { "CFDataGetBytePtr", cf_CFDataGetBytePtr },
    { "CFDataGetMutableBytePtr", cf_CFDataGetBytePtr },
    { "CFDataGetBytes", cf_CFDataGetBytes },
    { "CFDataSetLength", cf_CFDataSetLength },
    { "CFDataIncreaseLength", cf_CFDataIncreaseLength },
    { "CFDataAppendBytes", cf_CFDataAppendBytes },
    { "CFDataReplaceBytes", cf_CFDataReplaceBytes },
    { "CFDataDeleteBytes", cf_CFDataDeleteBytes },
};
const unsigned cfcoll_export_count = sizeof cfcoll_exports / sizeof cfcoll_exports[0];
