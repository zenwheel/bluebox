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

#endif
