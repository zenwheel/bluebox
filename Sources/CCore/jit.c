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

/* ------------------------------------------------------------------ */
/* The block engine */

#if defined(__aarch64__) || defined(__arm64__)

#include "a64.h"
#include "mem.h"

static jit_engine *engines[2]; /* by code map bit */

static unsigned pc_bucket(const jit_engine *e, uint32_t pc)
{
    return (pc >> e->align_shift) * 2654435761u >> 16;
}
static unsigned rg_bucket(uint32_t r) { return r * 2654435761u >> 16; }
static jit_fast *fast_slot(jit_engine *e, uint32_t pc)
{
    return &e->fast[(pc >> e->align_shift) & ((1u << JIT_FAST_BITS) - 1)];
}

jit_block *jit_lookup(jit_engine *e, uint32_t pc)
{
    for (jit_block *b = e->pcs[pc_bucket(e, pc)]; b; b = b->next)
        if (b->pc == pc) {
            if (b->ninsn) {
                jit_fast *f = fast_slot(e, pc);
                f->pc = pc;
                f->entry = b->entry;
            }
            return b;
        }
    return NULL;
}

static void write_word(jit_engine *e, uint32_t *at, uint32_t w)
{
    jit_code_write_begin(&e->code);
    *at = w;
    jit_code_write_end(&e->code, at, 4);
}

/* Take a block out of use: no more lookups find it, and code that still
 * branches to it (chained exits) is sent back to the dispatcher. */
static void kill(jit_engine *e, jit_block *b)
{
    if (!b->live)
        return;
    b->live = false;
    for (jit_block **p = &e->pcs[pc_bucket(e, b->pc)]; *p; p = &(*p)->next)
        if (*p == b) {
            *p = b->next;
            break;
        }
    jit_fast *f = fast_slot(e, b->pc);
    if (f->pc == b->pc) {
        f->pc = 1; /* never a guest pc: those are aligned */
        f->entry = NULL;
    }
    if (b->ninsn)
        write_word(e, b->entry, a64_b_word((int32_t)(b->dead - b->entry)));
    e->invalidated++;
}

/* mem.h: a store landed in a region holding translated code. */
static void region_written(jit_engine *e, gaddr lo)
{
    uint32_t r = lo >> CODE_MAP_SHIFT;
    for (jit_region_link **p = &e->regions[rg_bucket(r)]; *p;) {
        jit_region_link *l = *p;
        if (l->region == r) {
            kill(e, l->b);
            *p = l->next;
        } else {
            p = &l->next;
        }
    }
    mem_code_unmark(lo, lo + 1, e->map_bit);
}
static void written_ppc(gaddr lo, gaddr hi)
{
    (void)hi;
    region_written(engines[0], lo);
}
static void written_68k(gaddr lo, gaddr hi)
{
    (void)hi;
    region_written(engines[1], lo);
}

void jit_flush(jit_engine *e)
{
    for (size_t i = 0; i < e->npool; i++) {
        jit_block *b = e->pool[i];
        if (b->live)
            mem_code_unmark(b->pc, b->pc + b->len, e->map_bit);
        free(b);
    }
    e->npool = 0;
    memset(e->pcs, 0, sizeof e->pcs);
    memset(e->regions, 0, sizeof e->regions);
    for (size_t i = 0; i < (1u << JIT_FAST_BITS); i++)
        e->fast[i] = (jit_fast){ 1, 0, NULL };
    e->code.used = e->code_start;
    e->generation++;
    e->flushes++;
}

static jit_block *new_block(jit_engine *e, uint32_t pc, uint32_t ninsn, uint32_t len)
{
    if (e->npool == e->cappool) {
        size_t cap = e->cappool ? 2 * e->cappool : 4096;
        jit_block **p = realloc(e->pool, cap * sizeof *p);
        if (!p)
            return NULL;
        e->pool = p;
        e->cappool = cap;
    }
    jit_block *b = calloc(1, sizeof *b);
    if (!b)
        return NULL;
    b->pc = pc;
    b->ninsn = ninsn;
    b->len = len;
    b->live = true;
    e->pool[e->npool++] = b;
    b->next = e->pcs[pc_bucket(e, pc)];
    e->pcs[pc_bucket(e, pc)] = b;
    /* The regions it was translated from (and the one just before). */
    uint32_t end = pc + len;
    uint32_t r0 = (pc >= 8 ? pc - 8 : 0) >> CODE_MAP_SHIFT, r1 = (end - 1) >> CODE_MAP_SHIFT;
    for (uint32_t r = r0; r <= r1 && b->nlinks < 3; r++) {
        jit_region_link *l = &b->links[b->nlinks++];
        l->region = r;
        l->b = b;
        l->next = e->regions[rg_bucket(r)];
        e->regions[rg_bucket(r)] = l;
    }
    mem_code_mark(pc, end, e->map_bit);
    return b;
}

jit_block *jit_marker(jit_engine *e, uint32_t pc, uint32_t len) { return new_block(e, pc, 0, len); }

jit_block *jit_install(jit_engine *e, uint32_t pc, uint32_t ninsn, uint32_t len, const uint32_t *code,
                       uint32_t nwords, uint32_t dead_at)
{
    uint32_t *at = jit_code_alloc(&e->code, nwords * 4);
    if (!at) {
        jit_flush(e);
        at = jit_code_alloc(&e->code, nwords * 4);
        if (!at)
            return NULL;
    }
    jit_code_write_begin(&e->code);
    memcpy(at, code, nwords * 4);
    jit_code_write_end(&e->code, at, nwords * 4);
    jit_block *b = new_block(e, pc, ninsn, len);
    if (!b)
        return NULL;
    b->entry = at;
    b->dead = at + dead_at;
    jit_lookup(e, pc); /* into the fast table */
    return b;
}

void jit_chain(jit_engine *e, jit_block *n, uint64_t generation)
{
    uint32_t *stub = e->ctx.last_stub;
    if (!stub || !n || !n->ninsn || !n->live || generation != e->generation)
        return;
    write_word(e, stub, a64_b_word((int32_t)(n->entry - stub)));
    e->chained++;
}

void jit_engine_cache(jit_engine *e, const char *dir, const char *name)
{
    if (e->cache) {
        jit_cache_close(e->cache);
        e->cache = NULL;
    }
    e->cache_tried = false;
    snprintf(e->cache_dir, sizeof e->cache_dir, "%s", dir ? dir : "");
    snprintf(e->cache_name, sizeof e->cache_name, "%s", name ? name : "");
}

static jit_cache *cache_of(jit_engine *e)
{
    if (!e->cache_tried) {
        e->cache_tried = true;
        if (e->cache_dir[0] && e->cache_name[0])
            e->cache = jit_cache_open(e->cache_dir, e->cache_name, e->build);
    }
    return e->cache;
}

const jit_cache_rec *jit_cached(jit_engine *e, uint32_t pc, uint64_t seed)
{
    jit_cache *c = cache_of(e);
    if (!c)
        return NULL;
    const jit_cache_rec *r = jit_cache_find(c, pc);
    if (!r || !r->ninsn || r->code_len < 8 || !r->guest_len)
        return NULL;
    if (jit_hash(g_mem + pc, r->guest_len, seed ^ pc) != r->hash)
        return NULL;
    return r;
}

void jit_remember(jit_engine *e, uint32_t pc, uint32_t ninsn, uint32_t len, uint64_t seed, const uint32_t *code,
                  uint32_t nwords, uint32_t dead_at)
{
    jit_cache *c = cache_of(e);
    if (!c)
        return;
    uint32_t *copy = malloc(4 * (nwords + 1));
    if (!copy)
        return;
    memcpy(copy, code, 4 * nwords);
    copy[nwords] = dead_at;
    jit_cache_add(c, pc, ninsn, len, jit_hash(g_mem + pc, len, seed ^ pc), copy, 4 * (nwords + 1));
    free(copy);
}

#define CTX(f) ((uint32_t)offsetof(jit_ctx, f))

bool jit_engine_init(jit_engine *e, int map_bit, int align_shift, size_t code_size)
{
    if (e->ready)
        return true;
    if (!jit_code_init(&e->code, code_size))
        return false;
    e->map_bit = map_bit;
    e->align_shift = align_shift;
    uint32_t words[64];
    a64_buf b = { words, 0, 64, false };
    /* enter(cpu x0, ctx x1, budget x2, entry x3) -> budget left */
    a64_stp_pre(&b, 29, 30, A64_SP, -96);
    a64_stp(&b, 19, 20, A64_SP, 16);
    a64_stp(&b, 21, 22, A64_SP, 32);
    a64_stp(&b, 23, 24, A64_SP, 48);
    a64_stp(&b, 25, 26, A64_SP, 64);
    a64_stp(&b, 27, 28, A64_SP, 80);
    a64_addsub_imm(&b, A64_ADDI, true, 29, A64_SP, 0);
    a64_mov(&b, true, 19, 0);
    a64_mov(&b, true, 21, 1);
    a64_mov(&b, true, 22, 2);
    a64_ldst(&b, A64_LDRX, 20, 21, CTX(mem));
    a64_ldst(&b, A64_LDRX, 23, 21, CTX(map));
    a64_br(&b, 3);
    uint32_t exit_at = b.n;
    a64_ldst(&b, A64_STRX, 1, 21, CTX(last_stub));
    a64_mov(&b, true, 0, 22);
    a64_ldp(&b, 27, 28, A64_SP, 80);
    a64_ldp(&b, 25, 26, A64_SP, 64);
    a64_ldp(&b, 23, 24, A64_SP, 48);
    a64_ldp(&b, 21, 22, A64_SP, 32);
    a64_ldp(&b, 19, 20, A64_SP, 16);
    a64_ldp_post(&b, 29, 30, A64_SP, 96);
    a64_ret(&b);
    uint32_t *at = jit_code_alloc(&e->code, b.n * 4);
    jit_code_write_begin(&e->code);
    memcpy(at, words, b.n * 4);
    jit_code_write_end(&e->code, at, b.n * 4);
    e->code_start = e->code.used;
    e->enter = (uint64_t (*)(void *, jit_ctx *, int64_t, uint32_t *))(void *)at;
    e->ctx.exit = at + exit_at;
    e->ctx.mem = g_mem;
    e->ctx.fast = e->fast;
    for (size_t i = 0; i < (1u << JIT_FAST_BITS); i++)
        e->fast[i] = (jit_fast){ 1, 0, NULL };
    /* The code map, made now so translated code can rely on it. */
    if (!mem_code_mark(0, 4, 0))
        return false;
    e->ctx.map = g_code_map;
    int t = map_bit == CODE_PPC ? 0 : 1;
    engines[t] = e;
    g_code_written[t] = t == 0 ? written_ppc : written_68k;
    e->ready = true;
    return true;
}

#endif
