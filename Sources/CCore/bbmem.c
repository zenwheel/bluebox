#include "bbmem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <malloc/malloc.h>
#include <os/lock.h>
#else
#include <malloc.h>
#include <pthread.h>
#endif

/* The allocator is the host's: its list and lock are not saved (a restore
 * makes the list again from the blocks it saved). */

const bb_type bb_type_bytes = { "bytes", "bbmem.c", 1, BB_FORM_BYTES, 0 };
const bb_type bb_type_untyped = { "untyped", "bbmem.c", 1, BB_FORM_UNTYPED, 0 };

BB_HOST static bb_block *first, *last;
BB_HOST static size_t live;

#if defined(__APPLE__)
BB_HOST static os_unfair_lock lock = OS_UNFAIR_LOCK_INIT;
#define LOCK() os_unfair_lock_lock(&lock)
#define UNLOCK() os_unfair_lock_unlock(&lock)
#else
BB_HOST static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&lock)
#define UNLOCK() pthread_mutex_unlock(&lock)
#endif

/* Keyed by the header's address, so another allocator's memory never
 * looks like a header by chance; cleared when the block goes. */
static inline uint32_t magic_for(const bb_block *b)
{
    uintptr_t a = (uintptr_t)b;
    return (uint32_t)(a >> 4) ^ (uint32_t)(a >> 36) ^ 0xB10EB0C5u;
}

static void link_block(bb_block *b)
{
    LOCK();
    b->prev = last;
    b->next = NULL;
    if (last)
        last->next = b;
    else
        first = b;
    last = b;
    live++;
    UNLOCK();
}

static void unlink_block(bb_block *b)
{
    LOCK();
    if (b->prev)
        b->prev->next = b->next;
    else
        first = b->next;
    if (b->next)
        b->next->prev = b->prev;
    else
        last = b->prev;
    live--;
    UNLOCK();
}

bb_block *bb_block_of(const void *p)
{
    if (!p || ((uintptr_t)p & 15))
        return NULL;
#if defined(__APPLE__)
    /* The start of a block of the host's malloc is not one of ours (ours
     * start a header further on).  Asking costs a zone lookup, as much
     * again as the free itself (Shards frees a million region bands a
     * second), so it is asked only where reading the header could fault
     * (it would lie on an earlier page than p) or where AddressSanitizer
     * would call reading before another's block an error; anywhere else
     * the magic, keyed by the header's address, tells. */
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define BB_ASAN 1
#endif
#endif
#if defined(BB_ASAN) || defined(__SANITIZE_ADDRESS__)
    if (malloc_size(p))
        return NULL;
#else
    if (((uintptr_t)p & 4095) < sizeof(bb_block) && malloc_size(p))
        return NULL;
#endif
#endif
    bb_block *b = (bb_block *)p - 1;
    return b->magic == magic_for(b) ? b : NULL;
}

/* Always zeroed: what a block's owner has not written yet is the same in
 * every run (a snapshot then holds nothing of the memory's last user,
 * and its scan sees no stale pointers there). */
static void *fresh(size_t n, const bb_type *t)
{
    if (n > UINT32_MAX - sizeof(bb_block))
        return NULL;
    bb_block *b = calloc(1, sizeof *b + n);
    if (!b)
        return NULL;
    b->type = t ? t : &bb_type_untyped;
    b->size = (uint32_t)n;
    b->magic = magic_for(b);
    link_block(b);
    return b + 1;
}

void *bb_malloc(size_t n, const bb_type *t)
{
    return fresh(n, t);
}

void *bb_calloc(size_t count, size_t size, const bb_type *t)
{
    if (size && count > SIZE_MAX / size)
        return NULL;
    return fresh(count * size, t);
}

void bb_free(void *p)
{
    if (!p)
        return;
    bb_block *b = bb_block_of(p);
    if (!b) {
        free(p); /* the host's: a library's or Swift's */
        return;
    }
    unlink_block(b);
    b->magic = 0;
    free(b);
}

void *bb_realloc(void *p, size_t n, const bb_type *t)
{
    if (!p)
        return fresh(n, t);
    if (n > UINT32_MAX - sizeof(bb_block))
        return NULL;
    bb_block *b = bb_block_of(p);
    if (!b) {
        /* The host's block becomes one of ours. */
#if defined(__APPLE__)
        size_t old = malloc_size(p);
#else
        size_t old = malloc_usable_size(p);
#endif
        void *q = fresh(n, t);
        if (!q)
            return NULL;
        memcpy(q, p, old < n ? old : n);
        free(p);
        return q;
    }
    /* Off the list while it moves: the lock is not held across realloc. */
    uint32_t old = b->size;
    unlink_block(b);
    b->magic = 0;
    bb_block *nb = realloc(b, sizeof *b + n);
    if (!nb) {
        b->magic = magic_for(b);
        link_block(b);
        return NULL;
    }
    if (n > old)
        memset((uint8_t *)(nb + 1) + old, 0, n - old);
    nb->size = (uint32_t)n;
    if (t)
        nb->type = t;
    nb->magic = magic_for(nb);
    link_block(nb);
    return nb + 1;
}

char *bb_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = fresh(n, &bb_type_bytes);
    if (d)
        memcpy(d, s, n);
    return d;
}

char *bb_strndup(const char *s, size_t n)
{
    size_t len = strnlen(s, n);
    char *d = fresh(len + 1, &bb_type_bytes);
    if (d) {
        memcpy(d, s, len);
        d[len] = 0;
    }
    return d;
}

void *bb_host_malloc(size_t n)
{
    return malloc(n);
}

void *bb_host_calloc(size_t count, size_t size)
{
    return calloc(count, size);
}

void *bb_host_realloc(void *p, size_t n)
{
    return realloc(p, n);
}

void bb_host_free(void *p)
{
    free(p);
}

void bb_blocks_lock(void)
{
    LOCK();
}

void bb_blocks_unlock(void)
{
    UNLOCK();
}

bb_block *bb_blocks_first(void)
{
    return first;
}

size_t bb_blocks_count(void)
{
    return live;
}

bb_block *bb_blocks_detach(void)
{
    LOCK();
    bb_block *list = first;
    first = last = NULL;
    live = 0;
    UNLOCK();
    return list;
}

void bb_blocks_release(bb_block *list)
{
    while (list) {
        bb_block *next = list->next;
        list->magic = 0;
        free(list);
        list = next;
    }
}

/* ---- Regions ---- */

BB_HOST static bb_region regions[8];
BB_HOST static int nregions;

void bb_region_add(const char *name, void *base, size_t size, size_t (*extent)(void))
{
    for (int i = 0; i < nregions; i++) {
        if (strcmp(regions[i].name, name) == 0) {
            regions[i].base = base;
            regions[i].size = size;
            regions[i].extent = extent;
            return;
        }
    }
    if (nregions == (int)(sizeof regions / sizeof regions[0])) {
        fprintf(stderr, "bb_region_add: no room for %s\n", name);
        return;
    }
    regions[nregions++] = (bb_region){ name, base, size, extent };
}

int bb_region_count(void)
{
    return nregions;
}

const bb_region *bb_region_at(int i)
{
    return i >= 0 && i < nregions ? &regions[i] : NULL;
}
