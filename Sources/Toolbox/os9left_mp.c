/*
 * MPLibrary's calls beyond Multiprocessing.h (the ones Mac OS 9's kernel
 * interface exported), on a Mac with one processor, one process and one
 * address space.  None has a header; the argument lists below are Blue
 * Box's own, taken from what each name does:
 *
 *   Areas are real allocations: MPCreateArea(space, *base, size, attrs,
 *   access, alignmentLog2, *area) makes size bytes (to whole 4 KB pages,
 *   aligned as asked), cleared if attrs has bit 0; *base is where it was
 *   made (a base asked for in it can't be had: the memory is the Memory
 *   Manager's).  MPCreateAliasArea(space, area, offset, size, access,
 *   *alias) is a second window on the same bytes.  MPGetAreaInfo(area,
 *   *info) gives 6 longs: version 1, base, size, attrs, access, space.  A
 *   backing provider (MPSetAreaBackingProvider(area, proc, refCon), with
 *   proc(refCon, offset, page, 4096) returning an OSStatus) fills each
 *   page when set, as a first touch of each would, for all of them are
 *   resident.
 *
 *   Pages: MPHoldPages/MPUnholdPages/MPLockPages/MPUnlockPages(address,
 *   count) count holds and locks per page; MPGetPageAttributes(address,
 *   *attrs) gives bit 0 held, bit 1 locked, bit 2 mapped and the program's
 *   own bits in the high half, which MPSetPageAttributes(address, count,
 *   attrs) sets.  Pages age (MPSetPageAge/MPGetPageAge); a registered ager
 *   proc(refCon, page, *age) is asked of a page before MPFindVictimPage
 *   picks the oldest that is neither held nor locked.  MPGetFreePage hands
 *   out a 4 KB page and MPPutFreePage takes it back.  Addresses are their
 *   own physical addresses.
 *
 *   A pool (MPCreatePool(options, size, *pool)) is a set of blocks
 *   MPAllocateFromPool(pool, size, options) made and MPFree'd, kept to
 *   count and to give back when the pool is deleted; its statistics are 4
 *   longs: version 1, blocks, bytes, the most bytes there have been.
 *
 *   An OID table (OIDCreateTable(class, capacity, *table)) hands out IDs
 *   for values: class (4 bits) << 28 | generation (12 bits) << 16 |
 *   index + 1 (16 bits), so an ID released and reused is not the same ID.
 *
 *   The queries (MPQueryQueue, MPQuerySemaphore, MPQueryCriticalRegion,
 *   MPQueryEvent: (id, *info) with 5 longs: version 1, then what
 *   multiprocessing.c's mp_query gives; MPQueryBlocking(task, *info): 3
 *   longs: version 1, the kind of the object it waits on, its ID).
 *
 *   There is one process and one address space: a second can't be made
 *   (kMPInsufficientResourcesErr), the one there is can't be deleted.
 */
#include <stdlib.h>
#include <string.h>

#include "carbon_events.h"
#include "ppc_bridge.h"
#include "tb_internal.h"
#include "os9rest.h"
#include "os9left_mp.h"
#include "os9left.h"

enum {
    kMPDeletedErr = -29295,
    kMPInsufficientResourcesErr = -29298,
    kMPInvalidIDErr = -29299,
    kMPIterationEndErr = -29275,
};
enum { PAGE = 4096 };

static gaddr one_ref(int kind, void *slot) /* the one process / address space */
{
    static struct { int kind; gaddr id; } refs[3];
    for (int i = 0; i < 3; i++) {
        if (!refs[i].kind)
            refs[i].kind = kind;
        if (refs[i].kind == kind) {
            if (!refs[i].id || ref_get(refs[i].id, kind) != slot)
                refs[i].id = ref_new(kind, slot);
            return refs[i].id;
        }
    }
    return 0;
}

/* ---- Areas ---- */

typedef struct area {
    gaddr id;
    gaddr raw, base;
    uint32_t size, attrs, access;
    struct area *source; /* an alias of it */
    uint32_t offset;
    int aliases;
    gaddr provider, refcon;
    struct area *next;
} area;
static area *areas;

static area *area_of(gaddr id) { return ref_get(id, REF_AREA); }

static void free_area(area *a)
{
    for (area **p = &areas; *p; p = &(*p)->next)
        if (*p == a) {
            *p = a->next;
            break;
        }
    if (a->source)
        a->source->aliases--;
    else if (a->raw)
        tb_DisposePtr(a->raw);
    ref_free(a->id);
    free(a);
}

static uint32_t round_page(uint32_t n) { return (n + PAGE - 1) & ~(uint32_t)(PAGE - 1); }

static bool space_ok(gaddr s) { return !s || ref_get(s, REF_SPACE) != NULL; }

static void add_area(area *a)
{
    a->id = ref_new(REF_AREA, a);
    a->next = areas;
    areas = a;
}

static void mp_MPCreateArea(ppc_cpu *c)
{
    gaddr space = R(0), basep = R(1), out = R(6);
    uint32_t size = R(2), attrs = R(3), access = R(4), align = R(5) & 0xFF;
    if (!out || !basep || !size || !space_ok(space) || align > 24)
        RETURN(paramErr);
    if (rd32(basep))
        RETURN(kMPInsufficientResourcesErr);
    uint32_t pad = (align > 12 ? 1u << align : PAGE);
    area *a = calloc(1, sizeof *a);
    if (!a)
        RETURN(memFullErr);
    a->size = round_page(size);
    a->raw = tb_NewPtr((int32_t)(a->size + pad), false, (attrs & 1) != 0);
    if (!a->raw) {
        free(a);
        RETURN(memFullErr);
    }
    a->base = (a->raw + pad - 1) & ~(pad - 1);
    a->attrs = attrs;
    a->access = access;
    add_area(a);
    wr32(basep, a->base);
    wr32(out, a->id);
    RETURN(noErr);
}

static void mp_MPCreateAliasArea(ppc_cpu *c)
{
    area *src = area_of(R(1));
    gaddr out = R(5);
    uint32_t off = R(2), size = R(3);
    if (!src || !out)
        RETURN(!out ? paramErr : kMPInvalidIDErr);
    if (!space_ok(R(0)) || !size || off >= src->size || size > src->size - off)
        RETURN(paramErr);
    area *a = calloc(1, sizeof *a);
    if (!a)
        RETURN(memFullErr);
    while (src->source) { /* an alias of an alias is one of the original */
        off += src->offset;
        src = src->source;
    }
    a->source = src;
    a->offset = off;
    a->base = src->base + off;
    a->size = size;
    a->access = R(4);
    src->aliases++;
    add_area(a);
    wr32(out, a->id);
    RETURN(noErr);
}

static void mp_MPDeleteArea(ppc_cpu *c)
{
    area *a = area_of(R(0));
    if (!a)
        RETURN(kMPInvalidIDErr);
    if (a->aliases)
        RETURN(kMPInsufficientResourcesErr);
    free_area(a);
    RETURN(noErr);
}

static void mp_MPGetAreaInfo(ppc_cpu *c)
{
    area *a = area_of(R(0));
    if (!a)
        RETURN(kMPInvalidIDErr);
    if (!R(1))
        RETURN(paramErr);
    uint32_t v[6] = { 1, a->base, a->size, a->attrs, a->access, one_ref(REF_SPACE, (void *)&areas) };
    for (int i = 0; i < 6; i++)
        wr32(R(1) + 4u * (unsigned)i, v[i]);
    RETURN(noErr);
}

static void mp_MPGetAreaFromAddress(ppc_cpu *c)
{
    if (!R(1))
        RETURN(paramErr);
    for (area *a = areas; a; a = a->next)
        if (R(0) >= a->base && R(0) - a->base < a->size) {
            wr32(R(1), a->id);
            RETURN(noErr);
        }
    RETURN(kMPInvalidIDErr);
}

/* MPSetAreaSize(area, size): a bigger or smaller block, the bytes kept. */
static void mp_MPSetAreaSize(ppc_cpu *c)
{
    area *a = area_of(R(0));
    if (!a)
        RETURN(kMPInvalidIDErr);
    if (!R(1))
        RETURN(paramErr);
    if (a->source || a->aliases)
        RETURN(kMPInsufficientResourcesErr);
    uint32_t size = round_page(R(1));
    if (size == a->size)
        RETURN(noErr);
    gaddr raw = tb_NewPtr((int32_t)(size + PAGE), false, true);
    if (!raw)
        RETURN(memFullErr);
    gaddr base = (raw + PAGE - 1) & ~(uint32_t)(PAGE - 1);
    uint32_t keep = size < a->size ? size : a->size;
    uint8_t *tmp = malloc(keep);
    if (tmp) {
        gget(tmp, a->base, keep);
        gput(base, tmp, keep);
        free(tmp);
    }
    tb_DisposePtr(a->raw);
    a->raw = raw;
    a->base = base;
    a->size = size;
    RETURN(noErr);
}

static void mp_MPSetAreaAccess(ppc_cpu *c)
{
    area *a = area_of(R(0));
    if (!a)
        RETURN(kMPInvalidIDErr);
    a->access = R(1);
    RETURN(noErr);
}

static void mp_MPSetAreaBackingProvider(ppc_cpu *c)
{
    area *a = area_of(R(0));
    if (!a)
        RETURN(kMPInvalidIDErr);
    a->provider = R(1);
    a->refcon = R(2);
    if (a->provider)
        for (uint32_t off = 0; off < a->size; off += PAGE) {
            uint32_t args[4] = { a->refcon, off, a->base + off, PAGE };
            int32_t e = (int32_t)ppc_call(a->provider, args, 4);
            if (e)
                RETURN(e);
        }
    RETURN(noErr);
}

int32_t os9left_mp_next_area(gaddr after, gaddr idp)
{
    area *list[256];
    int n = 0, at = -1;
    for (area *a = areas; a && n < 256; a = a->next)
        list[n++] = a;
    if (after) {
        at = -2;
        for (int i = 0; i < n; i++) /* oldest first, as the tasks are */
            if (list[n - 1 - i]->id == after)
                at = i;
        if (at == -2)
            return kMPInvalidIDErr;
    }
    if (at + 1 >= n)
        return kMPIterationEndErr;
    wr32(idp, list[n - 1 - (at + 1)]->id);
    return noErr;
}

/* ---- Address spaces, the process and the processor ---- */

static void mp_MPCreateAddressSpace(ppc_cpu *c) { RETURN(R(0) ? kMPInsufficientResourcesErr : paramErr); }
static void mp_MPCreateProcess(ppc_cpu *c) { RETURN(R(0) ? kMPInsufficientResourcesErr : paramErr); }
static void mp_MPCreateProcessor(ppc_cpu *c) { RETURN(R(0) ? kMPInsufficientResourcesErr : paramErr); }

static void mp_MPDeleteAddressSpace(ppc_cpu *c)
{
    RETURN(ref_get(R(0), REF_SPACE) ? kMPInsufficientResourcesErr : kMPInvalidIDErr);
}

static void mp_MPDeleteProcess(ppc_cpu *c)
{
    RETURN(ref_get(R(0), REF_PROCESS) ? kMPInsufficientResourcesErr : kMPInvalidIDErr);
}

static void mp_MPDeleteProcessor(ppc_cpu *c)
{
    RETURN(ref_get(R(0), REF_CPU) ? kMPInsufficientResourcesErr : kMPInvalidIDErr);
}

/* MPTerminateProcess(process, status): every task but the caller's ends
 * with the status. */
static void mp_MPTerminateProcess(ppc_cpu *c)
{
    if (!ref_get(R(0), REF_PROCESS))
        RETURN(kMPInvalidIDErr);
    mp_terminate_all((int32_t)R(1));
    RETURN(noErr);
}

/* MPStartScheduling()/MPStopScheduling(): whether the tasks get turns. */
static void mp_MPStartScheduling(ppc_cpu *c) { mp_scheduling = true; RETURN(noErr); }
static void mp_MPStopScheduling(ppc_cpu *c) { mp_scheduling = false; RETURN(noErr); }

/* MPGetKernelStateSize(*size), MPGetKernelState(buffer, size): the number
 * of tasks, areas, pages held and whether it is scheduling, as longs. */
static uint32_t held_pages(void);
static void kernel_state(uint32_t v[5])
{
    gaddr ids[64];
    uint32_t na = 0;
    for (area *a = areas; a; a = a->next)
        na++;
    v[0] = 1;
    v[1] = mp_task_ids(ids, 64);
    v[2] = na;
    v[3] = held_pages();
    v[4] = mp_scheduling;
}
static void mp_MPGetKernelStateSize(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    wr32(R(0), 20);
    RETURN(noErr);
}
static void mp_MPGetKernelState(ppc_cpu *c)
{
    uint32_t v[5];
    if (!R(0) || R(1) < 20)
        RETURN(paramErr);
    kernel_state(v);
    for (int i = 0; i < 5; i++)
        wr32(R(0) + 4u * (unsigned)i, v[i]);
    RETURN(noErr);
}

/* ---- Pages ---- */

typedef struct {
    uint32_t page;
    uint16_t holds, locks;
    uint32_t age, user;
    bool mapped;
} pageinfo;
static pageinfo *pages;
static unsigned npages, cap_pages;
static gaddr *free_pages;
static unsigned nfree, cap_free;
static gaddr ager, ager_refcon;

static pageinfo *page_find(uint32_t page, bool make)
{
    for (unsigned i = 0; i < npages; i++)
        if (pages[i].page == page)
            return &pages[i];
    if (!make)
        return NULL;
    if (npages == cap_pages) {
        cap_pages = cap_pages ? cap_pages * 2 : 64;
        pages = realloc(pages, cap_pages * sizeof *pages);
        if (!pages) {
            npages = cap_pages = 0;
            return NULL;
        }
    }
    pages[npages] = (pageinfo){ .page = page };
    return &pages[npages++];
}

static uint32_t held_pages(void)
{
    uint32_t n = 0;
    for (unsigned i = 0; i < npages; i++)
        n += pages[i].holds || pages[i].locks;
    return n;
}

/* A page range's pages: each held, locked or unheld in turn. */
static int32_t each_page(gaddr addr, uint32_t count, int what)
{
    if (!count || (addr & (PAGE - 1)))
        return paramErr;
    for (uint32_t i = 0; i < count; i++) {
        pageinfo *p = page_find(addr / PAGE + i, what > 0);
        if (what > 0 && !p)
            return memFullErr;
        if (what < 0 && (!p || !(what == -1 ? p->holds : p->locks)))
            return paramErr;
    }
    for (uint32_t i = 0; i < count; i++) {
        pageinfo *p = page_find(addr / PAGE + i, true);
        switch (what) {
        case 1: p->holds++; break;
        case -1: p->holds--; break;
        case 2: p->locks++; break;
        case -2: p->locks--; break;
        }
    }
    return noErr;
}

static void mp_MPHoldPages(ppc_cpu *c) { RETURN(each_page(R(0), R(1), 1)); }
static void mp_MPUnholdPages(ppc_cpu *c) { RETURN(each_page(R(0), R(1), -1)); }
static void mp_MPLockPages(ppc_cpu *c) { RETURN(each_page(R(0), R(1), 2)); }
static void mp_MPUnlockPages(ppc_cpu *c) { RETURN(each_page(R(0), R(1), -2)); }

static void mp_MPGetPageAttributes(ppc_cpu *c)
{
    if (!R(1))
        RETURN(paramErr);
    pageinfo *p = page_find(R(0) / PAGE, false);
    wr32(R(1), p ? (p->holds || p->locks) | (p->locks ? 2u : 0) | (p->mapped ? 4u : 0) | p->user << 16 : 0);
    RETURN(noErr);
}

static void mp_MPSetPageAttributes(ppc_cpu *c)
{
    if (!R(1) || (R(0) & (PAGE - 1)))
        RETURN(paramErr);
    for (uint32_t i = 0; i < R(1); i++) {
        pageinfo *p = page_find(R(0) / PAGE + i, true);
        if (!p)
            RETURN(memFullErr);
        p->user = R(2) >> 16;
    }
    RETURN(noErr);
}

static void mp_MPGetPageAge(ppc_cpu *c)
{
    if (!R(1))
        RETURN(paramErr);
    pageinfo *p = page_find(R(0) / PAGE, false);
    wr32(R(1), p ? p->age : 0);
    RETURN(noErr);
}

static void mp_MPSetPageAge(ppc_cpu *c)
{
    pageinfo *p = page_find(R(0) / PAGE, true);
    if (!p)
        RETURN(memFullErr);
    p->age = R(1);
    RETURN(noErr);
}

static void mp_MPRegisterAger(ppc_cpu *c)
{
    ager = R(0);
    ager_refcon = R(1);
    RETURN(noErr);
}

/* MPFindVictimPage(*address): the oldest page that is neither held nor
 * locked, after the ager has been asked of each. */
static void mp_MPFindVictimPage(ppc_cpu *c)
{
    pageinfo *best = NULL;
    if (!R(0))
        RETURN(paramErr);
    for (unsigned i = 0; i < npages; i++) {
        pageinfo *p = &pages[i];
        if (p->holds || p->locks)
            continue;
        if (ager) {
            gaddr age = tb_NewPtr(4, false, true);
            if (age) {
                wr32(age, p->age);
                uint32_t args[3] = { ager_refcon, p->page * PAGE, age };
                if (!(int32_t)ppc_call(ager, args, 3))
                    p->age = rd32(age);
                tb_DisposePtr(age);
            }
        }
        if (!best || p->age > best->age)
            best = p;
    }
    if (!best)
        RETURN(kMPInsufficientResourcesErr);
    wr32(R(0), best->page * PAGE);
    RETURN(noErr);
}

static void mp_MPGetFreePage(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    gaddr p;
    if (nfree) {
        p = free_pages[--nfree];
    } else {
        gaddr raw = tb_NewPtr(2 * PAGE, false, true);
        if (!raw)
            RETURN(memFullErr);
        p = (raw + PAGE - 1) & ~(uint32_t)(PAGE - 1);
    }
    gmemset(p, 0, PAGE);
    wr32(R(0), p);
    RETURN(noErr);
}

static void mp_MPPutFreePage(ppc_cpu *c)
{
    if (!R(0) || (R(0) & (PAGE - 1)))
        RETURN(paramErr);
    if (nfree == cap_free) {
        cap_free = cap_free ? cap_free * 2 : 16;
        free_pages = realloc(free_pages, cap_free * sizeof *free_pages);
        if (!free_pages) {
            nfree = cap_free = 0;
            RETURN(memFullErr);
        }
    }
    free_pages[nfree++] = R(0);
    RETURN(noErr);
}

static void mp_MPGetFreePageCount(ppc_cpu *c) { RETURN(nfree); }

/* The pages of the areas that nothing holds. */
static void mp_MPGetUnheldPageCount(ppc_cpu *c)
{
    uint32_t n = 0;
    for (area *a = areas; a; a = a->next)
        if (!a->source)
            for (uint32_t off = 0; off < a->size; off += PAGE) {
                pageinfo *p = page_find((a->base + off) / PAGE, false);
                n += !(p && (p->holds || p->locks));
            }
    RETURN(n);
}

/* MPMapPage(space, address, physical) and MPUnmapPages(address, count):
 * an address is its own physical address, so only that is mapped. */
static void mp_MPMapPage(ppc_cpu *c)
{
    if (!space_ok(R(0)) || (R(1) & (PAGE - 1)))
        RETURN(paramErr);
    if (R(2) != R(1))
        RETURN(kMPInsufficientResourcesErr);
    pageinfo *p = page_find(R(1) / PAGE, true);
    if (!p)
        RETURN(memFullErr);
    p->mapped = true;
    RETURN(noErr);
}

static void mp_MPUnmapPages(ppc_cpu *c)
{
    if (!R(1) || (R(0) & (PAGE - 1)))
        RETURN(paramErr);
    for (uint32_t i = 0; i < R(1); i++) {
        pageinfo *p = page_find(R(0) / PAGE + i, false);
        if (p)
            p->mapped = false;
    }
    RETURN(noErr);
}

static void mp_MPGetPhysicalAddress(ppc_cpu *c)
{
    if (!R(1))
        RETURN(paramErr);
    wr32(R(1), R(0));
    RETURN(noErr);
}

/* MPGetPhysicalExtent(address, size, *physical, *extent): all of it, in one. */
static void mp_MPGetPhysicalExtent(ppc_cpu *c)
{
    if (!R(2) || !R(3))
        RETURN(paramErr);
    wr32(R(2), R(0));
    wr32(R(3), R(1));
    RETURN(noErr);
}

/* ---- Pools ---- */

typedef struct block {
    gaddr p;
    uint32_t size;
    struct block *next;
} block;

typedef struct pool {
    gaddr id;
    block *blocks;
    uint32_t count, bytes, peak;
} pool;
static pool *standard_pool;

static pool *pool_of(gaddr id) { return ref_get(id, REF_POOL); }

static void mp_MPCreatePool(ppc_cpu *c)
{
    if (!R(2))
        RETURN(paramErr);
    pool *p = calloc(1, sizeof *p);
    if (!p)
        RETURN(memFullErr);
    p->id = ref_new(REF_POOL, p);
    wr32(R(2), p->id);
    RETURN(noErr);
}

/* A pool block's memory is an MPAllocate'd block (multiprocessing.c's:
 * its raw start is the long 8 before it). */
static void block_dispose(gaddr p) { tb_DisposePtr(rd32(p - 8)); }

static void pool_empty(pool *p)
{
    while (p->blocks) {
        block *b = p->blocks;
        p->blocks = b->next;
        block_dispose(b->p);
        free(b);
    }
    p->count = p->bytes = 0;
}

/* Multiprocessing.c's MPFree has freed a block: a pool forgets it. */
void os9left_mp_freed(gaddr p)
{
    for (gaddr id = ref_next(REF_POOL, 0); id; id = ref_next(REF_POOL, id)) {
        pool *pl = pool_of(id);
        for (block **b = &pl->blocks; pl && *b; b = &(*b)->next)
            if ((*b)->p == p) {
                block *dead = *b;
                *b = dead->next;
                pl->count--;
                pl->bytes -= dead->size;
                free(dead);
                return;
            }
    }
}

static void mp_MPDeletePool(ppc_cpu *c)
{
    pool *p = pool_of(R(0));
    if (!p)
        RETURN(kMPInvalidIDErr);
    if (p == standard_pool)
        RETURN(kMPInsufficientResourcesErr);
    pool_empty(p);
    ref_free(p->id);
    free(p);
    RETURN(noErr);
}

/* MPAllocateFromPool(pool, size, options): a block, or 0 (options as
 * MPAllocateAligned's, bit 0 clearing it). */
static void mp_MPAllocateFromPool(ppc_cpu *c)
{
    pool *p = pool_of(R(0));
    if (!p || !R(1))
        RETURN(0);
    uint32_t args[3] = { R(1), 5, R(2) };
    gaddr m = os9rest_call(c, "MPAllocateAligned", args, 3);
    if (!m)
        RETURN(0);
    block *b = calloc(1, sizeof *b);
    if (!b) {
        block_dispose(m);
        RETURN(0);
    }
    b->p = m;
    b->size = R(1);
    b->next = p->blocks;
    p->blocks = b;
    p->count++;
    p->bytes += b->size;
    if (p->bytes > p->peak)
        p->peak = p->bytes;
    RETURN(m);
}

/* MPCoalescePool(pool): the blocks are the Memory Manager's, so none is
 * left in pieces to join. */
static void mp_MPCoalescePool(ppc_cpu *c) { RETURN(pool_of(R(0)) ? noErr : kMPInvalidIDErr); }

/* MPValidatePool(pool): each block's header still says what it was. */
static void mp_MPValidatePool(ppc_cpu *c)
{
    pool *p = pool_of(R(0));
    if (!p)
        RETURN(kMPInvalidIDErr);
    for (block *b = p->blocks; b; b = b->next)
        if (!rd32(b->p - 8) || rd32(b->p - 4) != b->size)
            RETURN(paramErr);
    RETURN(noErr);
}

static void mp_MPGetPoolStatistics(ppc_cpu *c)
{
    pool *p = pool_of(R(0));
    if (!p)
        RETURN(kMPInvalidIDErr);
    if (!R(1))
        RETURN(paramErr);
    uint32_t v[4] = { 1, p->count, p->bytes, p->peak };
    for (int i = 0; i < 4; i++)
        wr32(R(1) + 4u * (unsigned)i, v[i]);
    RETURN(noErr);
}

static void mp_MPGetStandardPoolRef(ppc_cpu *c)
{
    if (!standard_pool) {
        standard_pool = calloc(1, sizeof *standard_pool);
        if (!standard_pool)
            RETURN(0);
        standard_pool->id = ref_new(REF_POOL, standard_pool);
    }
    RETURN(standard_pool->id);
}

/* ---- OID tables ---- */

typedef struct {
    uint32_t value;
    uint16_t generation;
    bool used;
} oid_slot;

typedef struct {
    gaddr id;
    uint32_t cls, capacity, count;
    oid_slot *slot;
    uint32_t nslots;
    int locks;
} oid_table;

static oid_table *table_of(gaddr id) { return ref_get(id, REF_OID); }

static void mp_OIDCreateTable(ppc_cpu *c)
{
    if (!R(2) || R(0) > 15 || R(1) > 0xFFFF)
        RETURN(paramErr);
    oid_table *t = calloc(1, sizeof *t);
    if (!t)
        RETURN(memFullErr);
    t->cls = R(0);
    t->capacity = R(1) ? R(1) : 0xFFFF;
    t->id = ref_new(REF_OID, t);
    wr32(R(2), t->id);
    RETURN(noErr);
}

static void mp_OIDDeleteTable(ppc_cpu *c)
{
    oid_table *t = table_of(R(0));
    if (!t)
        RETURN(kMPInvalidIDErr);
    if (t->locks)
        RETURN(kMPInsufficientResourcesErr);
    ref_free(t->id);
    free(t->slot);
    free(t);
    RETURN(noErr);
}

static uint32_t oid_make(const oid_table *t, uint32_t index)
{
    return t->cls << 28 | (uint32_t)(t->slot[index].generation & 0xFFF) << 16 | (index + 1);
}

/* The slot an ID names in the table, if it still does. */
static oid_slot *oid_find(oid_table *t, uint32_t id)
{
    uint32_t index = (id & 0xFFFF) - 1;
    if (!t || index >= t->nslots || id >> 28 != t->cls || !t->slot[index].used)
        return NULL;
    return (t->slot[index].generation & 0xFFF) == (id >> 16 & 0xFFF) ? &t->slot[index] : NULL;
}

static void mp_OIDAssignID(ppc_cpu *c)
{
    oid_table *t = table_of(R(0));
    if (!t)
        RETURN(kMPInvalidIDErr);
    if (!R(2))
        RETURN(paramErr);
    uint32_t i;
    for (i = 0; i < t->nslots && t->slot[i].used; i++)
        ;
    if (i == t->nslots) {
        if (t->nslots >= t->capacity)
            RETURN(kMPInsufficientResourcesErr);
        uint32_t n = t->nslots ? t->nslots * 2 : 16;
        if (n > t->capacity)
            n = t->capacity;
        oid_slot *grown = realloc(t->slot, n * sizeof *t->slot);
        if (!grown)
            RETURN(memFullErr);
        t->slot = grown;
        memset(t->slot + t->nslots, 0, (n - t->nslots) * sizeof *t->slot);
        t->nslots = n;
    }
    t->slot[i].used = true;
    t->slot[i].value = R(1);
    t->count++;
    wr32(R(2), oid_make(t, i));
    RETURN(noErr);
}

static void mp_OIDTranslateID(ppc_cpu *c)
{
    oid_table *t = table_of(R(0));
    if (!t)
        RETURN(kMPInvalidIDErr);
    oid_slot *s = oid_find(t, R(1));
    if (!s)
        RETURN(kMPInvalidIDErr);
    if (R(2))
        wr32(R(2), s->value);
    RETURN(noErr);
}

static void mp_OIDSetIDValues(ppc_cpu *c)
{
    oid_slot *s = oid_find(table_of(R(0)), R(1));
    if (!s)
        RETURN(kMPInvalidIDErr);
    s->value = R(2);
    RETURN(noErr);
}

static void mp_OIDReleaseID(ppc_cpu *c)
{
    oid_table *t = table_of(R(0));
    oid_slot *s = oid_find(t, R(1));
    if (!s)
        RETURN(kMPInvalidIDErr);
    s->used = false;
    s->generation = (s->generation + 1) & 0xFFF;
    t->count--;
    RETURN(noErr);
}

/* OIDExtractID(table, id, *index): the slot an ID is. */
static void mp_OIDExtractID(ppc_cpu *c)
{
    oid_slot *s = oid_find(table_of(R(0)), R(1));
    if (!s)
        RETURN(kMPInvalidIDErr);
    if (R(2))
        wr32(R(2), (R(1) & 0xFFFF) - 1);
    RETURN(noErr);
}

/* OIDDecomposeID(id, *class, *generation, *index): no table needed. */
static void mp_OIDDecomposeID(ppc_cpu *c)
{
    if (!(R(0) & 0xFFFF))
        RETURN(paramErr);
    if (R(1))
        wr32(R(1), R(0) >> 28);
    if (R(2))
        wr32(R(2), R(0) >> 16 & 0xFFF);
    if (R(3))
        wr32(R(3), (R(0) & 0xFFFF) - 1);
    RETURN(noErr);
}

/* OIDGetArrayInfo(table, *slots, *used): the table's size now. */
static void mp_OIDGetArrayInfo(ppc_cpu *c)
{
    oid_table *t = table_of(R(0));
    if (!t)
        RETURN(kMPInvalidIDErr);
    if (R(1))
        wr32(R(1), t->nslots);
    if (R(2))
        wr32(R(2), t->count);
    RETURN(noErr);
}

static void mp_OIDLockTable(ppc_cpu *c)
{
    oid_table *t = table_of(R(0));
    if (!t)
        RETURN(kMPInvalidIDErr);
    t->locks++;
    RETURN(noErr);
}

static void mp_OIDUnlockTable(ppc_cpu *c)
{
    oid_table *t = table_of(R(0));
    if (!t)
        RETURN(kMPInvalidIDErr);
    if (!t->locks)
        RETURN(paramErr);
    t->locks--;
    RETURN(noErr);
}

/* ---- Queries ---- */

static void query(ppc_cpu *c, int kind)
{
    uint32_t q[4];
    if (!R(1))
        RETURN(paramErr);
    if (!mp_query(R(0), q) || (int)q[0] != kind)
        RETURN(kMPInvalidIDErr);
    wr32(R(1), 1);
    for (int i = 1; i < 4; i++)
        wr32(R(1) + 4u * (unsigned)i, q[i]);
    wr32(R(1) + 16, 0);
    RETURN(noErr);
}

static void mp_MPQueryQueue(ppc_cpu *c) { query(c, REF_QUEUE); }
static void mp_MPQuerySemaphore(ppc_cpu *c) { query(c, REF_SEMAPHORE); }
static void mp_MPQueryCriticalRegion(ppc_cpu *c) { query(c, REF_REGION); }
static void mp_MPQueryEvent(ppc_cpu *c) { query(c, REF_EVENT); }

static void mp_MPQueryBlocking(ppc_cpu *c)
{
    gaddr on;
    if (!R(1))
        RETURN(paramErr);
    if (!mp_task_blocking(R(0), &on))
        RETURN(kMPInvalidIDErr);
    wr32(R(1), 1);
    wr32(R(1) + 4, on ? (uint32_t)ref_kind(on) : 0);
    wr32(R(1) + 8, on);
    RETURN(noErr);
}

/* ---- Handlers and queues the kernel keeps ---- */

/* MPRegisterPMFHandler(proc, refCon) and MPRegisterThermalHandler(proc,
 * refCon): proc(refCon, event, value) is called when the machine's power
 * management or thermal state changes.  Blue Box's never does by itself;
 * MPSetSWIEvent(kind, value) is the software interrupt that raises one
 * (kind 1: power management, 2: thermal), calling the handler registered
 * for it.  MPMarkPMFTask(task, mark) tells power management the task is
 * one to wake first (kept for MPQuery's sake: an ID is checked). */
static struct { gaddr proc, refcon; } handlers[3];
static gaddr marked_task;

static void register_handler(ppc_cpu *c, int kind)
{
    handlers[kind].proc = R(0);
    handlers[kind].refcon = R(1);
    RETURN(noErr);
}

static void mp_MPRegisterPMFHandler(ppc_cpu *c) { register_handler(c, 1); }
static void mp_MPRegisterThermalHandler(ppc_cpu *c) { register_handler(c, 2); }

static void mp_MPSetSWIEvent(ppc_cpu *c)
{
    if (R(0) != 1 && R(0) != 2)
        RETURN(paramErr);
    if (!handlers[R(0)].proc)
        RETURN(noErr);
    uint32_t args[3] = { handlers[R(0)].refcon, R(0), R(1) };
    RETURN((int32_t)ppc_call(handlers[R(0)].proc, args, 3));
}

static void mp_MPMarkPMFTask(ppc_cpu *c)
{
    uint32_t q[4];
    (void)q;
    gaddr on;
    if (!mp_task_blocking(R(0), &on))
        RETURN(kMPInvalidIDErr);
    marked_task = R(1) ? R(0) : 0;
    RETURN(noErr);
}

/* MPCpuPlugin(selector, a, b) calls the plugin MPRegisterCpuPlugin(proc,
 * refCon) kept: proc(refCon, selector, a, b); noErr when there is none. */
static gaddr plugin, plugin_refcon;
static void mp_MPRegisterCpuPlugin(ppc_cpu *c)
{
    plugin = R(0);
    plugin_refcon = R(1);
    RETURN(noErr);
}

static void mp_MPCpuPlugin(ppc_cpu *c)
{
    if (!plugin)
        RETURN(noErr);
    uint32_t args[4] = { plugin_refcon, R(0), R(1), R(2) };
    RETURN((int32_t)ppc_call(plugin, args, 4));
}

/* The queues for remote calls: MPGetSystemRPCQueueID is the one calls to
 * the system wait on, MPGetLocalReplyQueue the one answers to this task
 * come back on; both are made when first asked for.  MPDoPendingRemoteCalls
 * answers what is waiting on the system's: a message is (proc, parameter,
 * reply queue), and proc(parameter)'s result is notified to the reply
 * queue. */
static gaddr rpc_queue, reply_queue;

static gaddr make_queue(ppc_cpu *c)
{
    gaddr cell = tb_NewPtr(4, false, true);
    if (!cell)
        return 0;
    uint32_t args[1] = { cell };
    gaddr q = os9rest_call(c, "MPCreateQueue", args, 1) == 0 ? rd32(cell) : 0;
    tb_DisposePtr(cell);
    return q;
}

static void mp_MPGetSystemRPCQueueID(ppc_cpu *c)
{
    if (!rpc_queue || ref_kind(rpc_queue) != REF_QUEUE)
        rpc_queue = make_queue(c);
    RETURN(rpc_queue);
}

static void mp_MPGetLocalReplyQueue(ppc_cpu *c)
{
    if (!reply_queue || ref_kind(reply_queue) != REF_QUEUE)
        reply_queue = make_queue(c);
    RETURN(reply_queue);
}

static void mp_MPDoPendingRemoteCalls(ppc_cpu *c)
{
    if (!rpc_queue || ref_kind(rpc_queue) != REF_QUEUE)
        RETURN(noErr);
    gaddr m = tb_NewPtr(12, false, true);
    if (!m)
        RETURN(memFullErr);
    for (int n = 0; n < 64; n++) {
        uint32_t args[5] = { rpc_queue, m, m + 4, m + 8, 0 };
        if (os9rest_call(c, "MPWaitOnQueue", args, 5) != 0)
            break;
        uint32_t result = rd32(m) ? ppc_call(rd32(m), &(uint32_t){ rd32(m + 4) }, 1) : 0;
        if (rd32(m + 8)) {
            uint32_t notify[4] = { rd32(m + 8), result, 0, 0 };
            os9rest_call(c, "MPNotifyQueue", notify, 4);
        }
    }
    tb_DisposePtr(m);
    RETURN(noErr);
}

void os9left_mp_reset(void)
{
    areas = NULL; /* their memory went with the heap */
    free(pages);
    pages = NULL;
    npages = cap_pages = 0;
    free(free_pages);
    free_pages = NULL;
    nfree = cap_free = 0;
    ager = ager_refcon = 0;
    standard_pool = NULL;
    memset(handlers, 0, sizeof handlers);
    marked_task = 0;
    plugin = plugin_refcon = 0;
    rpc_queue = reply_queue = 0;
    mp_scheduling = true;
}

const ppc_export os9left_mp_exports[] = {
    { "MPAllocateFromPool", mp_MPAllocateFromPool },
    { "MPCoalescePool", mp_MPCoalescePool },
    { "MPCpuPlugin", mp_MPCpuPlugin },
    { "MPCreateAddressSpace", mp_MPCreateAddressSpace },
    { "MPCreateAliasArea", mp_MPCreateAliasArea },
    { "MPCreateArea", mp_MPCreateArea },
    { "MPCreatePool", mp_MPCreatePool },
    { "MPCreateProcess", mp_MPCreateProcess },
    { "MPCreateProcessor", mp_MPCreateProcessor },
    { "MPDeleteAddressSpace", mp_MPDeleteAddressSpace },
    { "MPDeleteArea", mp_MPDeleteArea },
    { "MPDeletePool", mp_MPDeletePool },
    { "MPDeleteProcess", mp_MPDeleteProcess },
    { "MPDeleteProcessor", mp_MPDeleteProcessor },
    { "MPDoPendingRemoteCalls", mp_MPDoPendingRemoteCalls },
    { "MPFindVictimPage", mp_MPFindVictimPage },
    { "MPGetAreaFromAddress", mp_MPGetAreaFromAddress },
    { "MPGetAreaInfo", mp_MPGetAreaInfo },
    { "MPGetFreePage", mp_MPGetFreePage },
    { "MPGetFreePageCount", mp_MPGetFreePageCount },
    { "MPGetKernelState", mp_MPGetKernelState },
    { "MPGetKernelStateSize", mp_MPGetKernelStateSize },
    { "MPGetLocalReplyQueue", mp_MPGetLocalReplyQueue },
    { "MPGetPageAge", mp_MPGetPageAge },
    { "MPGetPageAttributes", mp_MPGetPageAttributes },
    { "MPGetPhysicalAddress", mp_MPGetPhysicalAddress },
    { "MPGetPhysicalExtent", mp_MPGetPhysicalExtent },
    { "MPGetPoolStatistics", mp_MPGetPoolStatistics },
    { "MPGetStandardPoolRef", mp_MPGetStandardPoolRef },
    { "MPGetSystemRPCQueueID", mp_MPGetSystemRPCQueueID },
    { "MPGetUnheldPageCount", mp_MPGetUnheldPageCount },
    { "MPHoldPages", mp_MPHoldPages },
    { "MPLockPages", mp_MPLockPages },
    { "MPMapPage", mp_MPMapPage },
    { "MPMarkPMFTask", mp_MPMarkPMFTask },
    { "MPPutFreePage", mp_MPPutFreePage },
    { "MPQueryBlocking", mp_MPQueryBlocking },
    { "MPQueryCriticalRegion", mp_MPQueryCriticalRegion },
    { "MPQueryEvent", mp_MPQueryEvent },
    { "MPQueryQueue", mp_MPQueryQueue },
    { "MPQuerySemaphore", mp_MPQuerySemaphore },
    { "MPRegisterAger", mp_MPRegisterAger },
    { "MPRegisterCpuPlugin", mp_MPRegisterCpuPlugin },
    { "MPRegisterPMFHandler", mp_MPRegisterPMFHandler },
    { "MPRegisterThermalHandler", mp_MPRegisterThermalHandler },
    { "MPSetAreaAccess", mp_MPSetAreaAccess },
    { "MPSetAreaBackingProvider", mp_MPSetAreaBackingProvider },
    { "MPSetAreaSize", mp_MPSetAreaSize },
    { "MPSetPageAge", mp_MPSetPageAge },
    { "MPSetPageAttributes", mp_MPSetPageAttributes },
    { "MPSetSWIEvent", mp_MPSetSWIEvent },
    { "MPStartScheduling", mp_MPStartScheduling },
    { "MPStopScheduling", mp_MPStopScheduling },
    { "MPTerminateProcess", mp_MPTerminateProcess },
    { "MPUnholdPages", mp_MPUnholdPages },
    { "MPUnlockPages", mp_MPUnlockPages },
    { "MPUnmapPages", mp_MPUnmapPages },
    { "MPValidatePool", mp_MPValidatePool },
    { "OIDAssignID", mp_OIDAssignID },
    { "OIDCreateTable", mp_OIDCreateTable },
    { "OIDDecomposeID", mp_OIDDecomposeID },
    { "OIDDeleteTable", mp_OIDDeleteTable },
    { "OIDExtractID", mp_OIDExtractID },
    { "OIDGetArrayInfo", mp_OIDGetArrayInfo },
    { "OIDLockTable", mp_OIDLockTable },
    { "OIDReleaseID", mp_OIDReleaseID },
    { "OIDSetIDValues", mp_OIDSetIDValues },
    { "OIDTranslateID", mp_OIDTranslateID },
    { "OIDUnlockTable", mp_OIDUnlockTable },
};
const unsigned os9left_mp_export_count = sizeof os9left_mp_exports / sizeof os9left_mp_exports[0];
