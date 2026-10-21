#include "jit.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#endif

/* ------------------------------------------------------------------ */
/* Executable memory */

bool jit_code_init(jit_code *j, size_t size)
{
    if (j->base)
        return true;
#if defined(__APPLE__)
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
#else
    void *p = mmap(NULL, size, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
#endif
    if (p == MAP_FAILED)
        return false;
    j->base = p;
    j->size = size;
    j->used = 0;
    return true;
}

void *jit_code_alloc(jit_code *j, size_t n)
{
    n = (n + 3) & ~(size_t)3;
    if (j->used + n > j->size)
        return NULL;
    void *p = j->base + j->used;
    j->used += n;
    return p;
}

void jit_code_write_begin(jit_code *j)
{
#if defined(__APPLE__)
    (void)j;
    pthread_jit_write_protect_np(0);
#else
    mprotect(j->base, j->size, PROT_READ | PROT_WRITE);
#endif
}

void jit_code_write_end(jit_code *j, void *p, size_t n)
{
#if defined(__APPLE__)
    (void)j;
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(p, n);
#else
    mprotect(j->base, j->size, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)p, (char *)p + n);
#endif
}

/* ------------------------------------------------------------------ */
/* The persistent translation cache
 *
 * File layout (host byte order; the file is only read by the build that
 * wrote it): a header of the magic "BBJT", a format number and the
 * translator's build identity, then records of
 *   magic "BLK1", pc, ninsn, guest_len, code_len, hash (8 bytes),
 *   a check of those fields and the code (8 bytes), code_len bytes of code.
 * A record cut short (a run that ended while writing) ends the file. */

enum { CACHE_FORMAT = 1, BUCKETS = 1 << 14 };

#define HDR_MAGIC 0x544A4242u /* "BBJT" */
#define REC_MAGIC 0x314B4C42u /* "BLK1" */

typedef struct rec_node {
    jit_cache_rec r;
    bool owned; /* code copied in this run (not part of the file as read) */
    struct rec_node *next;
} rec_node;

struct jit_cache {
    FILE *f;
    uint8_t *data; /* the file as read */
    rec_node *buckets[BUCKETS];
};

static unsigned bucket(uint32_t pc) { return (pc >> 2) * 2654435761u >> (32 - 14); }

static uint64_t rec_check(const jit_cache_rec *r)
{
    uint32_t f[4] = { r->pc, r->ninsn, r->guest_len, r->code_len };
    return jit_hash(r->code, r->code_len, jit_hash(f, sizeof f, r->hash));
}

static void insert(jit_cache *c, const jit_cache_rec *r, bool owned)
{
    unsigned b = bucket(r->pc);
    for (rec_node *n = c->buckets[b]; n; n = n->next)
        if (n->r.pc == r->pc) { /* a later translation replaces it */
            if (n->owned)
                free((void *)n->r.code);
            n->r = *r;
            n->owned = owned;
            return;
        }
    rec_node *n = malloc(sizeof *n);
    if (!n) {
        if (owned)
            free((void *)r->code);
        return;
    }
    n->r = *r;
    n->owned = owned;
    n->next = c->buckets[b];
    c->buckets[b] = n;
}

static void mkdirs(const char *dir)
{
    char path[1024];
    snprintf(path, sizeof path, "%s", dir);
    for (char *p = path + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(path, 0755);
            *p = '/';
        }
    mkdir(path, 0755);
}

jit_cache *jit_cache_open(const char *dir, const char *name, uint64_t build)
{
    jit_cache *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    mkdirs(dir);
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    bool fresh = true;
    FILE *in = fopen(path, "rb");
    if (in) {
        fseek(in, 0, SEEK_END);
        long size = ftell(in);
        fseek(in, 0, SEEK_SET);
        uint32_t hdr[4];
        if (size >= (long)sizeof hdr && fread(hdr, sizeof hdr, 1, in) == 1 && hdr[0] == HDR_MAGIC &&
            hdr[1] == CACHE_FORMAT && ((uint64_t)hdr[3] << 32 | hdr[2]) == build) {
            size_t n = (size_t)size - sizeof hdr;
            c->data = malloc(n ? n : 1);
            if (c->data && fread(c->data, 1, n, in) == n) {
                fresh = false;
                size_t at = 0;
                while (at + 36 <= n) {
                    uint32_t f[5];
                    uint64_t hash, check;
                    memcpy(f, c->data + at, sizeof f);
                    memcpy(&hash, c->data + at + 20, 8);
                    memcpy(&check, c->data + at + 28, 8);
                    if (f[0] != REC_MAGIC || at + 36 + f[4] > n)
                        break;
                    jit_cache_rec r = { f[1], f[2], f[3], f[4], hash, c->data + at + 36 };
                    if (rec_check(&r) != check)
                        break;
                    insert(c, &r, false);
                    at += 36 + f[4];
                }
                /* Whatever followed the last whole record is dropped. */
                if (at < n) {
                    fclose(in);
                    in = NULL;
                    if (truncate(path, (off_t)(sizeof hdr + at)) != 0)
                        fresh = true;
                }
            }
        }
        if (in)
            fclose(in);
    }
    c->f = fopen(path, fresh ? "wb" : "ab");
    if (c->f && fresh) {
        uint32_t hdr[4] = { HDR_MAGIC, CACHE_FORMAT, (uint32_t)build, (uint32_t)(build >> 32) };
        fwrite(hdr, sizeof hdr, 1, c->f);
    }
    return c;
}

const jit_cache_rec *jit_cache_find(jit_cache *c, uint32_t pc)
{
    for (rec_node *n = c->buckets[bucket(pc)]; n; n = n->next)
        if (n->r.pc == pc)
            return &n->r;
    return NULL;
}

void jit_cache_add(jit_cache *c, uint32_t pc, uint32_t ninsn, uint32_t guest_len, uint64_t hash, const void *code,
                   uint32_t code_len)
{
    if (!c->f)
        return;
    uint8_t *copy = malloc(code_len ? code_len : 1);
    if (!copy)
        return;
    memcpy(copy, code, code_len);
    jit_cache_rec r = { pc, ninsn, guest_len, code_len, hash, copy };
    uint32_t f[5] = { REC_MAGIC, pc, ninsn, guest_len, code_len };
    uint64_t check = rec_check(&r);
    fwrite(f, sizeof f, 1, c->f);
    fwrite(&hash, 8, 1, c->f);
    fwrite(&check, 8, 1, c->f);
    fwrite(code, 1, code_len, c->f);
    insert(c, &r, true);
}

void jit_cache_close(jit_cache *c)
{
    if (!c)
        return;
    if (c->f)
        fclose(c->f);
    for (int b = 0; b < BUCKETS; b++)
        for (rec_node *n = c->buckets[b], *next; n; n = next) {
            next = n->next;
            if (n->owned)
                free((void *)n->r.code);
            free(n);
        }
    free(c->data);
    free(c);
}
