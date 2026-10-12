/*
 * Read-only HFS (Mac OS Standard) volumes.
 *
 * Master directory block at 1024; the catalog and extents-overflow files are
 * B-trees of 512-byte nodes.  Only the leaf records are needed here: we walk
 * the leaf chain of the catalog, and search the extents tree for files with
 * more than three extents.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diskimage.h"

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

struct hfs_volume {
    const uint8_t *d;
    size_t len;
    uint32_t block_size;   /* allocation block size */
    uint32_t first_block;  /* first allocation block, in 512-byte sectors */
    uint8_t name[28];
    uint8_t *catalog;      /* catalog file contents */
    size_t catalog_len;
    uint8_t *extents;      /* extents overflow file contents */
    size_t extents_len;
};

typedef struct {
    uint16_t start, count;
} extent;

/* Copy the blocks described by up to three extents; returns bytes copied. */
static size_t copy_extents(hfs_volume *v, const uint8_t *rec, uint8_t *dst, size_t want, size_t done)
{
    for (int i = 0; i < 3 && done < want; i++) {
        uint16_t start = be16(rec + i * 4), count = be16(rec + i * 4 + 2);
        if (!count)
            break;
        size_t off = (size_t)v->first_block * 512 + (size_t)start * v->block_size;
        size_t n = (size_t)count * v->block_size;
        if (n > want - done)
            n = want - done;
        if (off >= v->len)
            return done;
        if (off + n > v->len)
            n = v->len - off;
        memcpy(dst + done, v->d + off, n);
        done += n;
    }
    return done;
}

/* Allocation blocks covered by an extent record. */
static uint32_t extent_blocks(const uint8_t *rec)
{
    uint32_t sum = 0;
    for (int i = 0; i < 3; i++)
        sum += be16(rec + i * 4 + 2);
    return sum;
}

/* B-tree helpers: walk the leaf chain, calling fn for each record. */
typedef bool (*leaf_fn)(void *ctx, const uint8_t *rec, size_t reclen);

static bool walk_leaves(const uint8_t *tree, size_t len, leaf_fn fn, void *ctx)
{
    if (len < 512)
        return false;
    const uint8_t *hdr = tree + 14;
    uint16_t node_size = be16(hdr + 18);
    if (node_size < 512 || node_size > 32768)
        node_size = 512;
    uint32_t node = be32(hdr + 10); /* first leaf */
    unsigned guard = 0;
    while (node) {
        if ((size_t)(node + 1) * node_size > len || ++guard > 1000000)
            return false;
        const uint8_t *n = tree + (size_t)node * node_size;
        uint16_t nrec = be16(n + 10);
        for (unsigned r = 0; r < nrec; r++) {
            uint16_t a = be16(n + node_size - 2 * (r + 1));
            uint16_t b = be16(n + node_size - 2 * (r + 2));
            if (a >= node_size || b > node_size || b < a)
                return false;
            if (!fn(ctx, n + a, (size_t)(b - a)))
                return true;
        }
        node = be32(n);
    }
    return true;
}

/* Read a special file (catalog/extents) from its MDB extent record. */
static uint8_t *read_special(hfs_volume *v, uint32_t size, const uint8_t *rec, size_t *outlen)
{
    uint8_t *buf = calloc(1, size ? size : 1);
    if (!buf)
        return NULL;
    *outlen = copy_extents(v, rec, buf, size, 0);
    return buf;
}

hfs_volume *hfs_open(const uint8_t *d, size_t len, char *err, size_t errlen)
{
    if (len < 1536) {
        snprintf(err, errlen, "volume too small");
        return NULL;
    }
    const uint8_t *mdb = d + 1024;
    uint16_t sig = be16(mdb);
    if (sig == 0xD2D7) {
        snprintf(err, errlen, "MFS volumes (400K disks) are not supported yet");
        return NULL;
    }
    if (sig != 0x4244) {
        snprintf(err, errlen, "not an HFS volume");
        return NULL;
    }
    if (be16(mdb + 124) == 0x482B) { /* embedded HFS+ */
        snprintf(err, errlen, "HFS+ volumes are not supported yet");
        return NULL;
    }
    hfs_volume *v = calloc(1, sizeof *v);
    if (!v) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    v->d = d;
    v->len = len;
    v->block_size = be32(mdb + 20);
    v->first_block = be16(mdb + 28);
    uint8_t nl = mdb[36] > 27 ? 27 : mdb[36];
    memcpy(v->name, mdb + 36, (size_t)nl + 1);
    v->name[0] = nl;
    if (!v->block_size || v->block_size % 512) {
        snprintf(err, errlen, "bad allocation block size %u", v->block_size);
        free(v);
        return NULL;
    }
    v->extents = read_special(v, be32(mdb + 130), mdb + 134, &v->extents_len);
    v->catalog = read_special(v, be32(mdb + 146), mdb + 150, &v->catalog_len);
    if (!v->catalog || v->catalog_len < 512) {
        snprintf(err, errlen, "cannot read the catalog");
        hfs_close(v);
        return NULL;
    }
    return v;
}

void hfs_close(hfs_volume *v)
{
    if (!v)
        return;
    free(v->catalog);
    free(v->extents);
    free(v);
}

const uint8_t *hfs_volume_name(const hfs_volume *v) { return v->name; }

/* ---- Catalog listing ---- */

typedef struct {
    hfs_entry *list;
    size_t n, cap;
    bool oom;
} list_ctx;

static bool collect(void *vctx, const uint8_t *rec, size_t reclen)
{
    list_ctx *ctx = vctx;
    if (reclen < 8)
        return true;
    uint8_t keylen = rec[0];
    size_t doff = (size_t)keylen + 1;
    if (doff & 1)
        doff++;
    if (doff + 2 > reclen)
        return true;
    const uint8_t *data = rec + doff;
    uint8_t type = data[0];
    if (type != 1 && type != 2)
        return true; /* threads */
    if (ctx->n == ctx->cap) {
        ctx->cap = ctx->cap ? ctx->cap * 2 : 256;
        hfs_entry *nl = realloc(ctx->list, ctx->cap * sizeof *nl);
        if (!nl) {
            ctx->oom = true;
            return false;
        }
        ctx->list = nl;
    }
    hfs_entry *e = &ctx->list[ctx->n];
    memset(e, 0, sizeof *e);
    e->parent = be32(rec + 2);
    uint8_t nlen = rec[6] > 31 ? 31 : rec[6];
    e->name[0] = nlen;
    memcpy(e->name + 1, rec + 7, nlen);
    if (type == 1) {
        if (doff + 70 > reclen)
            return true;
        e->is_dir = true;
        e->cnid = be32(data + 6);
        e->create_date = be32(data + 10);
        e->modify_date = be32(data + 14);
        memcpy(e->finder_info, data + 22, 32);
    } else {
        if (doff + 102 > reclen)
            return true;
        memcpy(e->finder_info, data + 4, 16);
        memcpy(e->finder_info + 16, data + 56, 16);
        e->cnid = be32(data + 20);
        e->data_len = be32(data + 26);
        e->rsrc_len = be32(data + 36);
        e->create_date = be32(data + 44);
        e->modify_date = be32(data + 48);
        memcpy(e->data_ext, data + 74, 12);
        memcpy(e->rsrc_ext, data + 86, 12);
    }
    ctx->n++;
    return true;
}

bool hfs_list(hfs_volume *v, hfs_entry **entries, size_t *count, char *err, size_t errlen)
{
    list_ctx ctx = { 0 };
    bool ok = walk_leaves(v->catalog, v->catalog_len, collect, &ctx);
    if (!ok || ctx.oom) {
        snprintf(err, errlen, ctx.oom ? "out of memory" : "damaged catalog B-tree");
        free(ctx.list);
        return false;
    }
    *entries = ctx.list;
    *count = ctx.n;
    return true;
}

/* ---- Fork reading ---- */

typedef struct {
    uint8_t fork; /* 0x00 data, 0xFF resource */
    uint32_t cnid;
    uint32_t start; /* first allocation block number of the wanted record */
    const uint8_t *found;
} ext_ctx;

static bool find_ext(void *vctx, const uint8_t *rec, size_t reclen)
{
    ext_ctx *ctx = vctx;
    /* key: keyLength(1)=7, forkType(1), fileID(4), startBlock(2); data: 3 extents */
    if (reclen < 8 + 12 || rec[0] < 7)
        return true;
    if (rec[1] == ctx->fork && be32(rec + 2) == ctx->cnid && be16(rec + 6) == ctx->start) {
        ctx->found = rec + 8;
        return false;
    }
    return true;
}

bool hfs_read_fork(hfs_volume *v, const hfs_entry *e, bool rsrc, uint8_t **buf, size_t *len, char *err,
                   size_t errlen)
{
    size_t want = rsrc ? e->rsrc_len : e->data_len;
    uint8_t *out = malloc(want ? want : 1);
    if (!out) {
        snprintf(err, errlen, "out of memory");
        return false;
    }
    const uint8_t *rec = rsrc ? e->rsrc_ext : e->data_ext;
    size_t done = copy_extents(v, rec, out, want, 0);
    uint32_t blocks = extent_blocks(rec);
    while (done < want) {
        /* More extents in the overflow file, keyed by the next block number. */
        ext_ctx ctx = { rsrc ? 0xFF : 0x00, e->cnid, blocks, NULL };
        if (!v->extents || !walk_leaves(v->extents, v->extents_len, find_ext, &ctx) || !ctx.found) {
            snprintf(err, errlen, "missing extents for file %u", e->cnid);
            free(out);
            return false;
        }
        size_t before = done;
        done = copy_extents(v, ctx.found, out, want, done);
        blocks += extent_blocks(ctx.found);
        if (done == before) {
            snprintf(err, errlen, "extents for file %u point outside the volume", e->cnid);
            free(out);
            return false;
        }
    }
    *buf = out;
    *len = want;
    return true;
}
