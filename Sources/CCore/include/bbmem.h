/*
 * The tracked allocator.  Every block the C targets allocate for their own
 * state comes from here, carries the type of what it holds and is kept in
 * a list of live blocks, so a snapshot (hibernate.c) can save every block
 * and, in another process, make each again wherever it lands and rewrite
 * the pointers to it.
 *
 *   bb_malloc(n, BB_TAG(struct foo))    an array of struct foo (often one)
 *   bb_calloc(n, sizeof *p, BB_TAG(T))
 *   bb_realloc(p, n, BB_TAG(T))         the tag is for p == NULL
 *   bb_malloc(sizeof(T) + extra, BB_HEAD(T))   one T, then bytes
 *   bb_malloc(n, BB_BYTES)              bytes with no pointers in them
 *   bb_malloc(n, BB_UNTYPED)            not known yet (saved as bytes;
 *                                       the snapshot's scan looks at it)
 *   bb_strdup, bb_strndup               bytes
 *   bb_free(p)
 *
 * The pointer map (statemap_gen.c, made by scripts/state-map.py) gives the
 * layout of each tagged type.  bb_free and bb_realloc also take memory the
 * host allocated (a library's, or Swift's): such a block goes to free()
 * and realloc() as before.  Memory handed to a library that frees it
 * itself, and blocks holding only the host's state, come from
 * bb_host_malloc and friends, which are plain malloc and are never saved.
 */
#ifndef CCORE_BBMEM_H
#define CCORE_BBMEM_H

#include <stddef.h>
#include <stdint.h>

/* What a block holds.  The name is the type as written in the source and
 * the file the one it was written in (a type of the same name in two
 * files may be two types). */
typedef struct bb_type {
    const char *name;
    const char *file;
    uint32_t size; /* sizeof the type, to check the map against */
    uint32_t form; /* BB_FORM_* */
    uint32_t line; /* where it was written, for the snapshot's scan */
} bb_type;

enum {
    BB_FORM_ARRAY,   /* elements of the type, then any remainder as bytes */
    BB_FORM_HEAD,    /* one of the type, then bytes */
    BB_FORM_BYTES,   /* no pointers */
    BB_FORM_UNTYPED, /* not known */
};

extern const bb_type bb_type_bytes, bb_type_untyped;
/* A descriptor of static storage made where the tag is written (each
 * BB_BYTES and BB_UNTYPED names its file and line). */
#define BB_DESC_(name, size, form)                                                  \
    __extension__({                                                                 \
        static const bb_type bb_tag_ = { name, __FILE_NAME__, size, form, __LINE__ }; \
        &bb_tag_;                                                                   \
    })
#define BB_TAG(T) BB_DESC_(#T, sizeof(T), BB_FORM_ARRAY)
#define BB_HEAD(T) BB_DESC_(#T, sizeof(T), BB_FORM_HEAD)
#define BB_BYTES BB_DESC_("bytes", 1, BB_FORM_BYTES)
#define BB_UNTYPED BB_DESC_("untyped", 1, BB_FORM_UNTYPED)

void *bb_malloc(size_t n, const bb_type *t);
void *bb_calloc(size_t count, size_t size, const bb_type *t);
void *bb_realloc(void *p, size_t n, const bb_type *t);
void bb_free(void *p);
char *bb_strdup(const char *s);
char *bb_strndup(const char *s, size_t n);

/* The host's own memory: untracked and never saved. */
void *bb_host_malloc(size_t n);
void *bb_host_calloc(size_t count, size_t size);
void *bb_host_realloc(void *p, size_t n);
void bb_host_free(void *p);

/* ---- For the snapshot ---- */

/* A tracked block's header, just before the memory bb_malloc returned. */
typedef struct bb_block {
    struct bb_block *prev, *next;
    const bb_type *type;
    uint32_t size;
    uint32_t magic;
} bb_block;

#define BB_BLOCK_DATA(b) ((void *)((bb_block *)(b) + 1))

/* The live blocks, oldest first, walked under the lock. */
void bb_blocks_lock(void);
void bb_blocks_unlock(void);
bb_block *bb_blocks_first(void);
size_t bb_blocks_count(void);
/* The block p (as bb_malloc returned it) is, or NULL for another's memory. */
bb_block *bb_block_of(const void *p);
/* Takes every live block off the list (the caller frees them with
 * bb_blocks_release once nothing points to them) and leaves it empty. */
bb_block *bb_blocks_detach(void);
void bb_blocks_release(bb_block *list);

/* Memory a snapshot saves that is not a block: the guest's address space.
 * A fresh process registers the same names again; a restore copies each
 * region's bytes into the new one and moves the pointers into it. */
typedef struct bb_region {
    const char *name;
    uint8_t *base;
    size_t size;
    /* How much of it holds anything, from its start (NULL: all of it);
     * only the pages the host has given it are saved within that. */
    size_t (*extent)(void);
} bb_region;

void bb_region_add(const char *name, void *base, size_t size, size_t (*extent)(void));
int bb_region_count(void);
const bb_region *bb_region_at(int i);

#endif
