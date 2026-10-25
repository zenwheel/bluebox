/*
 * Core Foundation as CarbonLib had it: the objects every type shares
 * (CFBase.h: retain counts, type IDs, equality, hashes, descriptions and
 * allocators), the kCF data exports, and the table of all of CF's
 * exports (carbon_cf*.c) that cfm.c looks names up in.
 *
 * An object is a block of guest memory from its allocator: 'CFob', the
 * type ID, the retain count (CF_CONSTANT for those never freed) and the
 * slot of its state on the host.  A reference that isn't such a block is
 * no object: calls given one do nothing, as far as they can.
 *
 * Allocators are objects too.  kCFAllocatorDefault is NULL and
 * kCFAllocatorUseContext 1, as in CFBase.h; the system's (and malloc's)
 * allocator takes blocks from the current heap with NewPtr, the null
 * allocator none, and a program's own (CFAllocatorCreate) are its
 * callbacks, called as guest code.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "carbon_cf.h"

#define CF_MAGIC 0x43466F62u /* 'CFob' */

void cf_return_double(ppc_cpu *c, double d)
{
    ppc_set_fpr(c, 1, d);
    c->pc = c->lr;
}

void cf_return64(ppc_cpu *c, uint64_t v)
{
    c->gpr[3] = (uint32_t)(v >> 32);
    c->gpr[4] = (uint32_t)v;
    c->pc = c->lr;
}

/* ---------------------------------------------------------------------- */
/* Text */

void text_add(cf_text *t, const uint16_t *u, uint32_t n)
{
    if (t->n + n + 1 > t->cap) {
        uint32_t cap = (t->n + n + 1) * 2;
        uint16_t *p = realloc(t->u, sizeof *p * cap);
        if (!p)
            return;
        t->u = p;
        t->cap = cap;
    }
    if (n)
        memcpy(t->u + t->n, u, sizeof *u * n);
    t->n += n;
}

void text_add_unit(cf_text *t, uint16_t u) { text_add(t, &u, 1); }

void text_add_utf8(cf_text *t, const char *s)
{
    uint16_t *u;
    uint32_t n;
    if (s && cf_decode(kCFStringEncodingUTF8, (const uint8_t *)s, strlen(s), &u, &n)) {
        text_add(t, u, n);
        free(u);
    }
}

void text_printf(cf_text *t, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    text_add_utf8(t, buf);
}

void text_free(cf_text *t)
{
    free(t->u);
    t->u = NULL;
    t->n = t->cap = 0;
}

/* ---------------------------------------------------------------------- */
/* Objects */

static const cf_class *classes[CFT_COUNT];
static cf_obj **slots;
static uint32_t nslots, slot_cap;
static uint32_t free_slot; /* a chain through the empty slots, by index */

void cf_register_class(uint32_t type, const cf_class *cls)
{
    if (type < CFT_COUNT)
        classes[type] = cls;
}

static uint32_t take_slot(cf_obj *o)
{
    if (free_slot) {
        uint32_t s = free_slot;
        free_slot = (uint32_t)(uintptr_t)slots[s] >> 1;
        slots[s] = o;
        return s;
    }
    if (nslots + 1 >= slot_cap) {
        uint32_t cap = slot_cap ? slot_cap * 2 : 256;
        cf_obj **p = realloc(slots, sizeof *p * cap);
        if (!p)
            return 0;
        slots = p;
        slot_cap = cap;
        if (!nslots)
            nslots = 1; /* slot 0 is none */
    }
    slots[nslots] = o;
    return nslots++;
}

/* An empty slot holds the next empty one's index, shifted and tagged so it
 * is never a pointer. */
static void give_slot(uint32_t s)
{
    slots[s] = (cf_obj *)(uintptr_t)(free_slot << 1 | 1);
    free_slot = s;
}

void *cf_new(uint32_t type, size_t size, gaddr alloc, uint32_t extra)
{
    gaddr a = cf_allocator_resolve(alloc);
    gaddr ref = cf_allocate(a, CF_HEADER + extra);
    if (!ref)
        return NULL;
    cf_obj *o = calloc(1, size);
    uint32_t slot = o ? take_slot(o) : 0;
    if (!slot) {
        free(o);
        cf_deallocate(a, ref);
        return NULL;
    }
    gmemset(ref, 0, CF_HEADER + extra);
    wr32(ref, CF_MAGIC);
    wr32(ref + 4, type);
    wr32(ref + 8, 1);
    wr32(ref + 12, slot);
    o->ref = ref;
    o->type = type;
    o->slot = slot;
    /* A program's own allocator is kept while its objects are. */
    if (a != cf_allocator_system() && a != cf_allocator_null() && cf_type_of(a) == CFT_ALLOCATOR)
        o->alloc = cf_retain(a);
    return o;
}

void cf_make_constant(cf_obj *o)
{
    if (o)
        wr32(o->ref + 8, CF_CONSTANT);
}

cf_obj *cf_get(gaddr ref)
{
    if (!ref || ref > 0xFFFFFFF0u || (ref & 1) || rd32(ref) != CF_MAGIC)
        return NULL;
    uint32_t s = rd32(ref + 12);
    if (!s || s >= nslots || ((uintptr_t)slots[s] & 1) || slots[s]->ref != ref)
        return NULL;
    return slots[s];
}

void *cf_get_type(gaddr ref, uint32_t type)
{
    cf_obj *o = cf_get(ref);
    return o && o->type == type ? o : NULL;
}

uint32_t cf_type_of(gaddr ref)
{
    cf_obj *o = cf_get(ref);
    return o ? o->type : 0;
}

gaddr cf_retain(gaddr ref)
{
    if (cf_get(ref)) {
        uint32_t n = rd32(ref + 8);
        if (n != CF_CONSTANT)
            wr32(ref + 8, n + 1);
    }
    return ref;
}

void cf_release(gaddr ref)
{
    cf_obj *o = cf_get(ref);
    if (!o)
        return;
    uint32_t n = rd32(ref + 8);
    if (n == CF_CONSTANT)
        return;
    if (n > 1) {
        wr32(ref + 8, n - 1);
        return;
    }
    wr32(ref + 8, 0);
    const cf_class *cls = classes[o->type];
    if (cls && cls->finalize)
        cls->finalize(o);
    if (cls && cls->dispose)
        cls->dispose(o);
    gaddr alloc = o->alloc;
    give_slot(o->slot);
    wr32(ref, 0);
    free(o);
    cf_deallocate(alloc ? alloc : cf_allocator_system(), ref);
    if (alloc)
        cf_release(alloc);
}

bool cf_equal(gaddr a, gaddr b)
{
    if (a == b)
        return true;
    cf_obj *x = cf_get(a), *y = cf_get(b);
    if (!x || !y || x->type != y->type)
        return false;
    const cf_class *cls = classes[x->type];
    return cls && cls->equal && cls->equal(x, y);
}

uint32_t cf_hash(gaddr ref)
{
    cf_obj *o = cf_get(ref);
    if (!o)
        return ref;
    const cf_class *cls = classes[o->type];
    return cls && cls->hash ? cls->hash(o) : ref;
}

static const char *type_name(uint32_t type)
{
    const cf_class *cls = type < CFT_COUNT ? classes[type] : NULL;
    return cls ? cls->name : type == CFT_TYPE ? "CFType" : NULL;
}

void cf_describe(gaddr ref, cf_text *out)
{
    cf_obj *o = cf_get(ref);
    if (!o) {
        text_printf(out, ref ? "<0x%x>" : "(null)", ref);
        return;
    }
    const cf_class *cls = classes[o->type];
    text_printf(out, "<%s 0x%x [0x%x]>", cls ? cls->name : "CFType", ref,
                o->alloc ? o->alloc : cf_allocator_system());
    if (cls && cls->describe) {
        text_add_unit(out, '{');
        cls->describe(o, out);
        text_add_unit(out, '}');
    }
}

void cf_format_describe(gaddr ref, cf_text *out)
{
    uint32_t n;
    const uint16_t *u = cfstr_chars(ref, &n);
    if (u)
        text_add(out, u, n);
    else
        cf_describe(ref, out);
}

/* ---------------------------------------------------------------------- */
/* Guest code */

uint32_t cf_call(gaddr fn, const uint32_t *args, int n)
{
    if (!fn)
        return 0;
    if (mm_is_descriptor(fn))
        return mm_call(fn, rd32(fn + 12), args, n); /* the first routine record's procInfo */
    return ppc_call(fn, args, n);
}

gaddr cf_native(void (*fn)(ppc_cpu *), const char *name) { return cfm_native_tvector(fn, name); }

/* ---------------------------------------------------------------------- */
/* Allocators */

enum { A_SYSTEM, A_MALLOC, A_NULL, A_CUSTOM };
/* CFAllocatorContext: version, info, retain, release, copyDescription,
 * allocate, reallocate, deallocate, preferredSize. */
enum { CTX_INFO = 1, CTX_RETAIN, CTX_RELEASE, CTX_DESC, CTX_ALLOCATE, CTX_REALLOCATE, CTX_DEALLOCATE,
       CTX_PREFERRED, CTX_WORDS };

typedef struct {
    cf_obj o;
    int kind;
    uint32_t ctx[CTX_WORDS];
} cf_allocator;

static gaddr system_allocator, malloc_allocator, null_allocator, default_allocator;

static void allocator_finalize(cf_obj *o)
{
    cf_allocator *a = (cf_allocator *)o;
    if (a->ctx[CTX_RELEASE])
        cf_call(a->ctx[CTX_RELEASE], &a->ctx[CTX_INFO], 1);
}

static void allocator_describe(cf_obj *o, cf_text *out)
{
    static const char *const kinds[] = { "kCFAllocatorSystemDefault", "kCFAllocatorMalloc", "kCFAllocatorNull" };
    cf_allocator *a = (cf_allocator *)o;
    if (a->kind != A_CUSTOM)
        text_printf(out, "%s", kinds[a->kind]);
    else
        text_printf(out, "info = 0x%x", a->ctx[CTX_INFO]);
}

static const cf_class allocator_class = { "CFAllocator", allocator_finalize, NULL, NULL, NULL, allocator_describe };

/* One of ours: made as the system's objects are, from NewPtr. */
static gaddr builtin_allocator(int kind)
{
    gaddr ref = tb_NewPtr(CF_HEADER, false, true);
    cf_allocator *a = calloc(1, sizeof *a);
    uint32_t slot = ref && a ? take_slot(&a->o) : 0;
    if (!slot) {
        free(a);
        return 0;
    }
    wr32(ref, CF_MAGIC);
    wr32(ref + 4, CFT_ALLOCATOR);
    wr32(ref + 8, CF_CONSTANT);
    wr32(ref + 12, slot);
    a->o.ref = ref;
    a->o.type = CFT_ALLOCATOR;
    a->o.slot = slot;
    a->kind = kind;
    return ref;
}

gaddr cf_allocator_system(void)
{
    if (!system_allocator) {
        cf_register_class(CFT_ALLOCATOR, &allocator_class);
        system_allocator = builtin_allocator(A_SYSTEM);
    }
    return system_allocator;
}

gaddr cf_allocator_null(void)
{
    if (!null_allocator)
        null_allocator = builtin_allocator(A_NULL);
    return null_allocator;
}

static gaddr allocator_malloc(void)
{
    if (!malloc_allocator)
        malloc_allocator = builtin_allocator(A_MALLOC);
    return malloc_allocator;
}

gaddr cf_allocator_resolve(gaddr alloc)
{
    if (alloc == 0 || alloc == 1)
        return default_allocator ? default_allocator : cf_allocator_system();
    return cf_type_of(alloc) == CFT_ALLOCATOR ? alloc : cf_allocator_system();
}

gaddr cf_allocate(gaddr alloc, uint32_t size)
{
    cf_allocator *a = cf_get_type(cf_allocator_resolve(alloc), CFT_ALLOCATOR);
    if (!a || a->kind == A_NULL || (int32_t)size < 0)
        return 0;
    if (a->kind != A_CUSTOM)
        return tb_NewPtr((int32_t)(size ? size : 1), false, false);
    uint32_t args[3] = { size, 0, a->ctx[CTX_INFO] };
    return cf_call(a->ctx[CTX_ALLOCATE], args, 3);
}

void cf_deallocate(gaddr alloc, gaddr p)
{
    cf_allocator *a = cf_get_type(cf_allocator_resolve(alloc), CFT_ALLOCATOR);
    if (!a || !p || a->kind == A_NULL)
        return;
    if (a->kind != A_CUSTOM) {
        tb_DisposePtr(p);
        return;
    }
    uint32_t args[2] = { p, a->ctx[CTX_INFO] };
    cf_call(a->ctx[CTX_DEALLOCATE], args, 2);
}

gaddr cf_reallocate(gaddr alloc, gaddr p, uint32_t size, uint32_t old_size)
{
    cf_allocator *a = cf_get_type(cf_allocator_resolve(alloc), CFT_ALLOCATOR);
    if (!a || a->kind == A_NULL)
        return 0;
    if (!p)
        return size ? cf_allocate(alloc, size) : 0;
    if (!size) {
        cf_deallocate(alloc, p);
        return 0;
    }
    if (a->kind == A_CUSTOM) {
        if (!a->ctx[CTX_REALLOCATE])
            return 0;
        uint32_t args[4] = { p, size, 0, a->ctx[CTX_INFO] };
        return cf_call(a->ctx[CTX_REALLOCATE], args, 4);
    }
    tb_SetPtrSize(p, (int32_t)size);
    if ((uint32_t)tb_GetPtrSize(p) == size)
        return p;
    gaddr q = tb_NewPtr((int32_t)size, false, false);
    if (!q)
        return 0;
    gmemcpy(q, p, old_size < size ? old_size : size);
    tb_DisposePtr(p);
    return q;
}

static void cf_CFAllocatorGetTypeID(ppc_cpu *c) { RETURN(CFT_ALLOCATOR); }

/* CFAllocatorSetDefault(allocator): the new one kept; the old one stays
 * (objects may still use it). */
static void cf_CFAllocatorSetDefault(ppc_cpu *c)
{
    gaddr a = R(0);
    if (cf_type_of(a) == CFT_ALLOCATOR && a != default_allocator) {
        cf_retain(a);
        default_allocator = a;
    }
    RETURN_VOID();
}

static void cf_CFAllocatorGetDefault(ppc_cpu *c) { RETURN(cf_allocator_resolve(0)); }

/* CFAllocatorCreate(allocator, context): kCFAllocatorUseContext makes the
 * allocator with its own allocate callback. */
static void cf_CFAllocatorCreate(ppc_cpu *c)
{
    gaddr ctx = R(1);
    if (!ctx)
        RETURN(0);
    uint32_t words[CTX_WORDS];
    for (int i = 0; i < CTX_WORDS; i++)
        words[i] = rd32(ctx + 4 * (uint32_t)i);
    cf_allocator *a;
    if (R(0) == 1) {
        uint32_t args[3] = { CF_HEADER, 0, words[CTX_INFO] };
        gaddr ref = cf_call(words[CTX_ALLOCATE], args, 3);
        a = ref ? calloc(1, sizeof *a) : NULL;
        uint32_t slot = a ? take_slot(&a->o) : 0;
        if (!slot) {
            free(a);
            RETURN(0);
        }
        gmemset(ref, 0, CF_HEADER);
        wr32(ref, CF_MAGIC);
        wr32(ref + 4, CFT_ALLOCATOR);
        wr32(ref + 8, 1);
        wr32(ref + 12, slot);
        a->o.ref = ref;
        a->o.type = CFT_ALLOCATOR;
        a->o.slot = slot;
    } else {
        a = cf_new(CFT_ALLOCATOR, sizeof *a, R(0), 0);
        if (!a)
            RETURN(0);
    }
    a->kind = A_CUSTOM;
    memcpy(a->ctx, words, sizeof words);
    if (words[CTX_RETAIN])
        a->ctx[CTX_INFO] = cf_call(words[CTX_RETAIN], &words[CTX_INFO], 1);
    /* Made by itself, it is freed by itself too. */
    if (R(0) == 1)
        a->o.alloc = 0;
    RETURN(a->o.ref);
}

static void cf_CFAllocatorAllocate(ppc_cpu *c) { RETURN(cf_allocate(R(0), R(1))); }

static void cf_CFAllocatorReallocate(ppc_cpu *c)
{
    gaddr p = R(1);
    uint32_t old = 0;
    cf_allocator *a = cf_get_type(cf_allocator_resolve(R(0)), CFT_ALLOCATOR);
    if (p && a && a->kind != A_CUSTOM)
        old = (uint32_t)tb_GetPtrSize(p);
    RETURN(cf_reallocate(R(0), p, R(2), old));
}

static void cf_CFAllocatorDeallocate(ppc_cpu *c)
{
    cf_deallocate(R(0), R(1));
    RETURN_VOID();
}

static void cf_CFAllocatorGetPreferredSizeForSize(ppc_cpu *c)
{
    cf_allocator *a = cf_get_type(cf_allocator_resolve(R(0)), CFT_ALLOCATOR);
    if (a && a->kind == A_CUSTOM && a->ctx[CTX_PREFERRED]) {
        uint32_t args[3] = { R(1), R(2), a->ctx[CTX_INFO] };
        RETURN(cf_call(a->ctx[CTX_PREFERRED], args, 3));
    }
    RETURN(R(1));
}

/* Our allocators' callbacks, for their contexts. */
static void sys_allocate(ppc_cpu *c) { RETURN(cf_allocate(cf_allocator_system(), R(0))); }
static void sys_reallocate(ppc_cpu *c)
{
    RETURN(cf_reallocate(cf_allocator_system(), R(0), R(1), R(0) ? (uint32_t)tb_GetPtrSize(R(0)) : 0));
}
static void sys_deallocate(ppc_cpu *c)
{
    cf_deallocate(cf_allocator_system(), R(0));
    RETURN_VOID();
}
static void sys_preferred(ppc_cpu *c) { RETURN(R(0)); }
static void null_allocate(ppc_cpu *c) { RETURN(0); }
static void null_deallocate(ppc_cpu *c) { RETURN_VOID(); }

/* CFAllocatorGetContext(allocator, context) */
static void cf_CFAllocatorGetContext(ppc_cpu *c)
{
    cf_allocator *a = cf_get_type(cf_allocator_resolve(R(0)), CFT_ALLOCATOR);
    gaddr ctx = R(1);
    if (!a || !ctx)
        RETURN_VOID();
    uint32_t w[CTX_WORDS] = { 0 };
    if (a->kind == A_CUSTOM) {
        memcpy(w, a->ctx, sizeof w);
    } else if (a->kind == A_NULL) {
        w[CTX_ALLOCATE] = cf_native(null_allocate, "__CFAllocatorNullAllocate");
        w[CTX_REALLOCATE] = w[CTX_ALLOCATE];
        w[CTX_DEALLOCATE] = cf_native(null_deallocate, "__CFAllocatorNullDeallocate");
    } else {
        w[CTX_ALLOCATE] = cf_native(sys_allocate, "__CFAllocatorSystemAllocate");
        w[CTX_REALLOCATE] = cf_native(sys_reallocate, "__CFAllocatorSystemReallocate");
        w[CTX_DEALLOCATE] = cf_native(sys_deallocate, "__CFAllocatorSystemDeallocate");
        w[CTX_PREFERRED] = cf_native(sys_preferred, "__CFAllocatorSystemPreferredSize");
    }
    w[0] = 0;
    for (int i = 0; i < CTX_WORDS; i++)
        wr32(ctx + 4 * (uint32_t)i, w[i]);
    RETURN_VOID();
}

/* ---------------------------------------------------------------------- */
/* CFType */

/* __CFRangeMake(loc, len): a CFRange, through the pointer in r3. */
static void cf_RangeMake(ppc_cpu *c)
{
    gaddr res = R(0);
    if (res) {
        wr32(res, R(1));
        wr32(res + 4, R(2));
    }
    RETURN(res);
}

static void cf_CFGetTypeID(ppc_cpu *c) { RETURN(cf_type_of(R(0))); }

static void cf_CFCopyTypeIDDescription(ppc_cpu *c)
{
    const char *name = type_name(R(0));
    RETURN(name ? cfstr_from_utf8(name) : 0);
}

static void cf_CFRetain(ppc_cpu *c) { RETURN(cf_retain(R(0))); }

static void cf_CFRelease(ppc_cpu *c)
{
    cf_release(R(0));
    RETURN_VOID();
}

static void cf_CFGetRetainCount(ppc_cpu *c) { RETURN(cf_get(R(0)) ? rd32(R(0) + 8) : 0); }
static void cf_CFEqual(ppc_cpu *c) { RETURN(cf_equal(R(0), R(1))); }
static void cf_CFHash(ppc_cpu *c) { RETURN(cf_hash(R(0))); }

static void cf_CFCopyDescription(ppc_cpu *c)
{
    if (!cf_get(R(0)))
        RETURN(0);
    cf_text t = { 0 };
    cf_describe(R(0), &t);
    gaddr s = cfstr_from_text(&t);
    text_free(&t);
    RETURN(s);
}

static void cf_CFGetAllocator(ppc_cpu *c)
{
    cf_obj *o = cf_get(R(0));
    if (!o)
        RETURN(0);
    RETURN(o->type == CFT_ALLOCATOR ? R(0) : o->alloc ? o->alloc : cf_allocator_system());
}

gaddr cf_tv_retain(void) { return cf_native(cf_CFRetain, "CFRetain"); }
gaddr cf_tv_release(void) { return cf_native(cf_CFRelease, "CFRelease"); }
gaddr cf_tv_describe(void) { return cf_native(cf_CFCopyDescription, "CFCopyDescription"); }

uint32_t cf_call_info(gaddr fn, uint32_t info)
{
    if (!fn)
        return 0;
    if (fn == cf_tv_retain())
        return cf_retain(info);
    if (fn == cf_tv_release()) {
        cf_release(info);
        return 0;
    }
    if (fn == cf_tv_describe()) {
        cf_text t = { 0 };
        cf_describe(info, &t);
        gaddr s = cfstr_from_text(&t);
        text_free(&t);
        return s;
    }
    return cf_call(fn, &info, 1);
}

/* ---------------------------------------------------------------------- */
/* The callback structures' routines (kCFTypeArrayCallBacks ...) */

static void cb_retain(ppc_cpu *c) { RETURN(cf_retain(R(1))); }
static void cb_release(ppc_cpu *c)
{
    cf_release(R(1));
    RETURN_VOID();
}
static void cb_copy_description(ppc_cpu *c)
{
    if (!cf_get(R(0)))
        RETURN(0);
    cf_text t = { 0 };
    cf_describe(R(0), &t);
    gaddr s = cfstr_from_text(&t);
    text_free(&t);
    RETURN(s);
}
static void cb_equal(ppc_cpu *c) { RETURN(cf_equal(R(0), R(1))); }
static void cb_hash(ppc_cpu *c) { RETURN(cf_hash(R(0))); }
/* kCFCopyString...: a copy of the string kept rather than the string. */
static void cb_copy_string(ppc_cpu *c)
{
    uint32_t n;
    const uint16_t *u = cfstr_chars(R(1), &n);
    RETURN(u ? cfstr_new(R(0), u, n, false) : cf_retain(R(1)));
}

static gaddr callbacks(bool copy_string, bool hash, bool equal_desc)
{
    gaddr p = cf_data_cell(NULL, hash ? 24 : 20);
    if (!p)
        return 0;
    wr32(p + 4, copy_string ? cf_native(cb_copy_string, "__CFStringCollectionCopy")
                            : cf_native(cb_retain, "__CFTypeCollectionRetain"));
    wr32(p + 8, cf_native(cb_release, "__CFTypeCollectionRelease"));
    if (equal_desc) {
        wr32(p + 12, cf_native(cb_copy_description, "CFCopyDescription"));
        wr32(p + 16, cf_native(cb_equal, "CFEqual"));
    }
    if (hash)
        wr32(p + 20, cf_native(cb_hash, "CFHash"));
    return p;
}

bool cf_is_type_retain(gaddr fn) { return fn && fn == cf_native(cb_retain, "__CFTypeCollectionRetain"); }
bool cf_is_type_release(gaddr fn) { return fn && fn == cf_native(cb_release, "__CFTypeCollectionRelease"); }
bool cf_is_type_equal(gaddr fn) { return fn && fn == cf_native(cb_equal, "CFEqual"); }
bool cf_is_type_hash(gaddr fn) { return fn && fn == cf_native(cb_hash, "CFHash"); }
bool cf_is_type_describe(gaddr fn) { return fn && fn == cf_native(cb_copy_description, "CFCopyDescription"); }
bool cf_is_copy_string(gaddr fn) { return fn && fn == cf_native(cb_copy_string, "__CFStringCollectionCopy"); }

/* ---------------------------------------------------------------------- */
/* Data exports */

/* What each one is, made on first use: a value in a cell. */
static gaddr make_data(const char *name);

static struct {
    char name[48];
    gaddr addr;
} cells[96];
static int ncells;

gaddr cf_data_cell(const char *name, uint32_t size)
{
    if (name)
        for (int i = 0; i < ncells; i++)
            if (!strcmp(cells[i].name, name))
                return cells[i].addr;
    gaddr p = tb_NewPtr((int32_t)size, false, true);
    if (p && name && ncells < (int)(sizeof cells / sizeof cells[0])) {
        snprintf(cells[ncells].name, sizeof cells[0].name, "%s", name);
        cells[ncells++].addr = p;
    }
    return p;
}

static const char *const string_keys[][2] = {
    { "kCFBundleInfoDictionaryVersionKey", "CFBundleInfoDictionaryVersion" },
    { "kCFBundleExecutableKey", "CFBundleExecutable" },
    { "kCFBundleIdentifierKey", "CFBundleIdentifier" },
    { "kCFBundleVersionKey", "CFBundleVersion" },
    { "kCFBundleDevelopmentRegionKey", "CFBundleDevelopmentRegion" },
    { "kCFBundleNameKey", "CFBundleName" },
    { "kCFPlugInDynamicRegistrationKey", "CFPlugInDynamicRegistration" },
    { "kCFPlugInDynamicRegisterFunctionKey", "CFPlugInDynamicRegisterFunction" },
    { "kCFPlugInUnloadFunctionKey", "CFPlugInUnloadFunction" },
    { "kCFPlugInFactoriesKey", "CFPlugInFactories" },
    { "kCFPlugInTypesKey", "CFPlugInTypes" },
    { "kCFPreferencesAnyApplication", "kCFPreferencesAnyApplication" },
    { "kCFPreferencesCurrentApplication", "kCFPreferencesCurrentApplication" },
    { "kCFPreferencesAnyHost", "kCFPreferencesAnyHost" },
    { "kCFPreferencesCurrentHost", "kCFPreferencesCurrentHost" },
    { "kCFPreferencesAnyUser", "kCFPreferencesAnyUser" },
    { "kCFPreferencesCurrentUser", "kCFPreferencesCurrentUser" },
    /* The URL properties, under both names CarbonLib exported. */
    { "kCFURLFileExists", "kCFURLFileExists" },
    { "kCFFileURLExists", "kCFURLFileExists" },
    { "kCFURLFileDirectoryContents", "kCFURLFileDirectoryContents" },
    { "kCFFileURLDirectoryContents", "kCFURLFileDirectoryContents" },
    { "kCFURLFileLength", "kCFURLFileLength" },
    { "kCFFileURLSize", "kCFURLFileLength" },
    { "kCFURLFileLastModificationTime", "kCFURLFileLastModificationTime" },
    { "kCFFileURLLastModificationTime", "kCFURLFileLastModificationTime" },
    { "kCFURLFilePOSIXMode", "kCFURLFilePOSIXMode" },
    { "kCFFileURLPOSIXMode", "kCFURLFilePOSIXMode" },
    { "kCFURLFileOwnerID", "kCFURLFileOwnerID" },
    { "kCFURLHTTPStatusCode", "kCFURLHTTPStatusCode" },
    { "kCFHTTPURLStatusCode", "kCFURLHTTPStatusCode" },
    { "kCFURLHTTPStatusLine", "kCFURLHTTPStatusLine" },
    { "kCFHTTPURLStatusLine", "kCFURLHTTPStatusLine" },
};

static const char *const other_data[] = {
    "kCFAllocatorDefault", "kCFAllocatorSystemDefault", "kCFAllocatorMalloc", "kCFAllocatorNull",
    "kCFAllocatorUseContext", "kCFBooleanTrue", "kCFBooleanFalse", "kCFNumberPositiveInfinity",
    "kCFNumberNegativeInfinity", "kCFNumberNaN", "kCFAbsoluteTimeIntervalSince1970",
    "kCFAbsoluteTimeIntervalSince1904", "kCFTypeArrayCallBacks", "kCFTypeBagCallBacks", "kCFCopyStringBagCallBacks",
    "kCFTypeSetCallBacks", "kCFCopyStringSetCallBacks", "kCFTypeDictionaryKeyCallBacks",
    "kCFCopyStringDictionaryKeyCallBacks", "kCFTypeDictionaryValueCallBacks",
};

bool carbon_cf_data_known(const char *name)
{
    if (strncmp(name, "kCF", 3))
        return false;
    for (unsigned i = 0; i < sizeof string_keys / sizeof string_keys[0]; i++)
        if (!strcmp(name, string_keys[i][0]))
            return true;
    for (unsigned i = 0; i < sizeof other_data / sizeof other_data[0]; i++)
        if (!strcmp(name, other_data[i]))
            return true;
    return false;
}

static gaddr word_cell(const char *name, uint32_t v)
{
    gaddr p = cf_data_cell(name, 4);
    if (p)
        wr32(p, v);
    return p;
}

static gaddr double_cell(const char *name, double d)
{
    gaddr p = cf_data_cell(name, 8);
    uint64_t bits;
    memcpy(&bits, &d, 8);
    if (p) {
        wr32(p, (uint32_t)(bits >> 32));
        wr32(p + 4, (uint32_t)bits);
    }
    return p;
}

static gaddr make_data(const char *name)
{
    for (unsigned i = 0; i < sizeof string_keys / sizeof string_keys[0]; i++)
        if (!strcmp(name, string_keys[i][0]))
            return word_cell(name, cfstr_constant(string_keys[i][1]));
    if (!strcmp(name, "kCFAllocatorDefault"))
        return word_cell(name, 0);
    if (!strcmp(name, "kCFAllocatorUseContext"))
        return word_cell(name, 1);
    if (!strcmp(name, "kCFAllocatorSystemDefault"))
        return word_cell(name, cf_allocator_system());
    if (!strcmp(name, "kCFAllocatorMalloc"))
        return word_cell(name, allocator_malloc());
    if (!strcmp(name, "kCFAllocatorNull"))
        return word_cell(name, cf_allocator_null());
    if (!strcmp(name, "kCFBooleanTrue"))
        return word_cell(name, cfbool(true));
    if (!strcmp(name, "kCFBooleanFalse"))
        return word_cell(name, cfbool(false));
    if (!strcmp(name, "kCFNumberPositiveInfinity") || !strcmp(name, "kCFNumberNegativeInfinity") ||
        !strcmp(name, "kCFNumberNaN")) {
        double d = name[9] == 'P' ? 1.0 / 0.0 : name[9] == 'N' && name[10] == 'e' ? -1.0 / 0.0 : 0.0 / 0.0;
        gaddr n = cfnum_new_double(cf_allocator_system(), d);
        cf_make_constant(cf_get(n));
        return word_cell(name, n);
    }
    if (!strcmp(name, "kCFAbsoluteTimeIntervalSince1970"))
        return double_cell(name, 978307200.0);
    if (!strcmp(name, "kCFAbsoluteTimeIntervalSince1904"))
        return double_cell(name, 3061152000.0);
    gaddr p = 0;
    if (!strcmp(name, "kCFTypeArrayCallBacks") || !strcmp(name, "kCFTypeDictionaryValueCallBacks"))
        p = callbacks(false, false, true);
    else if (!strcmp(name, "kCFTypeBagCallBacks") || !strcmp(name, "kCFTypeSetCallBacks") ||
             !strcmp(name, "kCFTypeDictionaryKeyCallBacks"))
        p = callbacks(false, true, true);
    else if (!strcmp(name, "kCFCopyStringBagCallBacks") || !strcmp(name, "kCFCopyStringSetCallBacks") ||
             !strcmp(name, "kCFCopyStringDictionaryKeyCallBacks"))
        p = callbacks(true, true, true);
    if (p && ncells < (int)(sizeof cells / sizeof cells[0])) {
        snprintf(cells[ncells].name, sizeof cells[0].name, "%s", name);
        cells[ncells++].addr = p;
    }
    return p;
}

gaddr carbon_cf_data_export(const char *name)
{
    for (int i = 0; i < ncells; i++)
        if (!strcmp(cells[i].name, name))
            return cells[i].addr;
    return carbon_cf_data_known(name) ? make_data(name) : 0;
}

/* ---------------------------------------------------------------------- */

void carbon_cf_reset(void)
{
    for (uint32_t i = 1; i < nslots; i++)
        if (!((uintptr_t)slots[i] & 1)) {
            /* The guest memory is gone; the host state goes with it. */
            const cf_class *cls = classes[slots[i]->type];
            if (cls && cls->dispose)
                cls->dispose(slots[i]);
            free(slots[i]);
        }
    free(slots);
    slots = NULL;
    nslots = slot_cap = free_slot = 0;
    system_allocator = malloc_allocator = null_allocator = default_allocator = 0;
    ncells = 0;
    cfstr_reset();
    cfcoll_reset();
    cfnum_reset();
    cfurl_reset();
    cfplist_reset();
}

const ppc_export cfbase_exports[] = {
    { "__CFRangeMake", cf_RangeMake },
    { "CFAllocatorGetTypeID", cf_CFAllocatorGetTypeID },
    { "CFAllocatorSetDefault", cf_CFAllocatorSetDefault },
    { "CFAllocatorGetDefault", cf_CFAllocatorGetDefault },
    { "CFAllocatorCreate", cf_CFAllocatorCreate },
    { "CFAllocatorAllocate", cf_CFAllocatorAllocate },
    { "CFAllocatorReallocate", cf_CFAllocatorReallocate },
    { "CFAllocatorDeallocate", cf_CFAllocatorDeallocate },
    { "CFAllocatorGetPreferredSizeForSize", cf_CFAllocatorGetPreferredSizeForSize },
    { "CFAllocatorGetContext", cf_CFAllocatorGetContext },
    { "CFGetTypeID", cf_CFGetTypeID },
    { "CFCopyTypeIDDescription", cf_CFCopyTypeIDDescription },
    { "CFRetain", cf_CFRetain },
    { "CFRelease", cf_CFRelease },
    { "CFGetRetainCount", cf_CFGetRetainCount },
    { "CFEqual", cf_CFEqual },
    { "CFHash", cf_CFHash },
    { "CFCopyDescription", cf_CFCopyDescription },
    { "CFGetAllocator", cf_CFGetAllocator },
};
const unsigned cfbase_export_count = sizeof cfbase_exports / sizeof cfbase_exports[0];

void (*carbon_cf_export_fn(const char *name))(ppc_cpu *)
{
    static const struct {
        const ppc_export *table;
        const unsigned *count;
    } tables[] = {
        { cfbase_exports, &cfbase_export_count }, { cfstr_exports, &cfstr_export_count },
        { cfcoll_exports, &cfcoll_export_count }, { cfnum_exports, &cfnum_export_count },
        { cfurl_exports, &cfurl_export_count },   { cfplist_exports, &cfplist_export_count },
    };
    /* CF's names, and Math64's (S64..., U64..., ...). */
    if (strncmp(name, "CF", 2) && strncmp(name, "__CF", 4) && !strchr("SUL", name[0]))
        return NULL;
    for (unsigned t = 0; t < sizeof tables / sizeof tables[0]; t++)
        for (unsigned i = 0; i < *tables[t].count; i++)
            if (!strcmp(tables[t].table[i].name, name))
                return tables[t].table[i].fn;
    return NULL;
}
