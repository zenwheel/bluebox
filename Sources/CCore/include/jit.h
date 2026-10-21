/*
 * What the JITs share: host memory for translated code (writable or
 * executable, never both at once), and the persistent translation cache.
 *
 * Executable memory: on macOS one MAP_JIT region, switched between writing
 * and running per thread (pthread_jit_write_protect_np); elsewhere an
 * ordinary mapping switched with mprotect.  Writes are bracketed by
 * jit_code_write_begin and jit_code_write_end, which also makes the
 * instruction cache see them.
 *
 * The translation cache keeps blocks on disk between runs: a record per
 * block holds its guest address, its length in guest instructions, a hash
 * of its guest code and the host code translated from it.  The host code
 * must not depend on where it is put (no absolute addresses; calls and
 * exits go through a context the code is handed).  A record is only used
 * when the guest code at that address hashes the same, and a file is only
 * read by the translator build that wrote it.
 */
#ifndef CCORE_JIT_H
#define CCORE_JIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Executable memory */

typedef struct jit_code {
    uint8_t *base;
    size_t size;
    size_t used;
} jit_code;

bool jit_code_init(jit_code *j, size_t size);
/* Room for n bytes (aligned to 4), or NULL when full. */
void *jit_code_alloc(jit_code *j, size_t n);
void jit_code_write_begin(jit_code *j);
/* Done writing [p, p+n): back to executable, instruction cache flushed. */
void jit_code_write_end(jit_code *j, void *p, size_t n);

/* ------------------------------------------------------------------ */
/* Hashing */

static inline uint64_t jit_hash(const void *data, size_t n, uint64_t seed)
{
    const uint8_t *p = data;
    uint64_t h = 0xcbf29ce484222325ull ^ seed;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x100000001b3ull;
    return h ^ (h >> 29);
}

/* ------------------------------------------------------------------ */
/* The persistent translation cache */

typedef struct jit_cache_rec {
    uint32_t pc;        /* guest address */
    uint32_t ninsn;     /* guest instructions */
    uint32_t guest_len; /* bytes of guest code hashed */
    uint32_t code_len;  /* bytes of host code */
    uint64_t hash;      /* of the guest code */
    const uint8_t *code;
} jit_cache_rec;

typedef struct jit_cache jit_cache;

/* Open (and read) dir/name, creating dir as needed.  `build` identifies the
 * translator: records written by another build are ignored.  NULL when the
 * cache can't be used (the JIT then translates everything itself). */
jit_cache *jit_cache_open(const char *dir, const char *name, uint64_t build);
/* The record for guest address pc, or NULL. */
const jit_cache_rec *jit_cache_find(jit_cache *c, uint32_t pc);
/* Add a translation (appended to the file at once). */
void jit_cache_add(jit_cache *c, uint32_t pc, uint32_t ninsn, uint32_t guest_len, uint64_t hash, const void *code,
                   uint32_t code_len);
void jit_cache_close(jit_cache *c);

/* ------------------------------------------------------------------ */
/* The block engine both translators use (arm64 hosts)
 *
 * An engine holds one translator's blocks: their host code in a buffer of
 * its own, a table from guest address to block, the code map regions each
 * was translated from (so stores there drop them), the table translated
 * code looks indirect branches up in, and the persistent cache.
 *
 * Translated code runs with:
 *   x19  the guest CPU structure      x20  the guest memory arena
 *   x21  the engine's jit_ctx         x22  the instruction budget left
 *   x23  the code map                 x24-x28  free for the translator
 * entered through jit_enter, and leaves by branching to ctx.exit with x1
 * the exit stub to chain (or 0).  A block starts with the budget check
 * (subs x22, x22, #n; b.lt over) and has a dead path (pc stored, exit)
 * that kill() points its entry at. */

enum { JIT_FAST_BITS = 14, JIT_MAX_HELPERS = 16 };

typedef struct jit_fast {
    uint32_t pc;
    uint32_t pad;
    uint32_t *entry;
} jit_fast;

typedef struct jit_ctx {
    void *exit;              /* the exit routine */
    uint8_t *mem;            /* g_mem */
    uint8_t *map;            /* g_code_map */
    uint32_t watch_lo, watch_len;
    uint32_t *last_stub;     /* the exit stub that left last, or NULL */
    jit_fast *fast;          /* indirect-branch table */
    uint32_t keep[4];        /* values kept across a helper call */
    uint32_t killed;         /* a block was dropped since entry (a store
                                into code: the running block may be stale) */
    void *helper[JIT_MAX_HELPERS]; /* the translator's */
} jit_ctx;

typedef struct jit_block jit_block;
typedef struct jit_region_link {
    uint32_t region;
    jit_block *b;
    struct jit_region_link *next;
} jit_region_link;

struct jit_block {
    uint32_t pc;
    uint32_t ninsn;          /* 0: a marker, the interpreter runs what is here */
    uint32_t len;            /* bytes of guest code */
    uint32_t *entry;
    uint32_t *dead;
    bool live;
    jit_block *next;
    jit_region_link links[3];
    int nlinks;
};

typedef struct jit_engine {
    int map_bit;             /* CODE_PPC or CODE_68K */
    int align_shift;         /* guest instructions are 1 << this aligned */
    bool ready;
    jit_code code;
    size_t code_start;
    uint64_t (*enter)(void *cpu, jit_ctx *x, int64_t budget, uint32_t *entry);
    jit_ctx ctx;
    jit_fast fast[1 << JIT_FAST_BITS];
    jit_block *pcs[1 << 16];
    jit_region_link *regions[1 << 16];
    jit_block **pool;
    size_t npool, cappool;
    uint64_t generation;     /* flushes so far */
    jit_cache *cache;
    char cache_dir[1024], cache_name[256];
    bool cache_tried;
    uint64_t build;          /* the translator's identity, for the cache */
    uint64_t invalidated, flushes, chained;
} jit_engine;

/* Set up an engine (once; false if there is no executable memory). */
bool jit_engine_init(jit_engine *e, int map_bit, int align_shift, size_t code_size);
jit_block *jit_lookup(jit_engine *e, uint32_t pc);
/* A block from host code (copied into the engine's buffer, flushing it
 * when full): ninsn guest instructions in len bytes at pc; dead_at the
 * word offset of its dead path. */
jit_block *jit_install(jit_engine *e, uint32_t pc, uint32_t ninsn, uint32_t len, const uint32_t *code,
                       uint32_t nwords, uint32_t dead_at);
/* A marker: nothing translated at pc (len bytes looked at). */
jit_block *jit_marker(jit_engine *e, uint32_t pc, uint32_t len);
void jit_flush(jit_engine *e);
/* Point the exit stub that just left (if any) at n's entry. */
void jit_chain(jit_engine *e, jit_block *n, uint64_t generation);
/* Run translated code from entry; returns the budget left. */
static inline uint64_t jit_enter(jit_engine *e, void *cpu, int64_t budget, uint32_t *entry)
{
    return e->enter(cpu, &e->ctx, budget, entry);
}
/* Persistent cache: where (dir NULL: none), and the record for pc whose
 * guest code still hashes the same (seed mixed into the hash). */
void jit_engine_cache(jit_engine *e, const char *dir, const char *name);
const jit_cache_rec *jit_cached(jit_engine *e, uint32_t pc, uint64_t seed);
/* Record a translation in the cache (the code with its dead-path word). */
void jit_remember(jit_engine *e, uint32_t pc, uint32_t ninsn, uint32_t len, uint64_t seed, const uint32_t *code,
                  uint32_t nwords, uint32_t dead_at);

#endif
