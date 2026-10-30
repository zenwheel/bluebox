/*
 * Read-only HFS (Mac OS Standard) and HFS+ (Mac OS Extended) volumes.
 *
 * HFS: the master directory block at 1024; the catalog and extents-overflow
 * files are B-trees of 512-byte nodes.  Only the leaf records are needed
 * here: we walk the leaf chain of the catalog, and search the extents tree
 * for files with more than three extents.
 *
 * HFS+ (and HFSX): the volume header at 1024, alone or inside an HFS
 * wrapper (whose master directory block names the embedded volume's
 * extent).  The same B-tree layout with bigger nodes; keys and records are
 * wider (32-bit IDs and block numbers, eight extents a fork, 64-bit sizes)
 * and names are UTF-16, decomposed, converted here to MacRoman.  Hard links
 * (a file of type 'hlnk', creator 'hfs+') are read through the file they
 * link to in the hidden "HFS+ Private Data" folder, which is left out.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "diskimage.h"
#include "forks.h"

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

struct hfs_volume {
    const uint8_t *d;
    size_t len;
    bool plus;             /* HFS+: d and len are the embedded volume */
    bool mfs;
    hfs_volinfo info;
    uint32_t block_size;   /* allocation block size */
    uint32_t first_block;  /* first allocation block, in 512-byte sectors */
    uint8_t name[256];
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

static hfs_volume *hfsplus_open(const uint8_t *d, size_t len, char *err, size_t errlen);
static hfs_volume *mfs_open(const uint8_t *d, size_t len, char *err, size_t errlen);

hfs_volume *hfs_open(const uint8_t *d, size_t len, char *err, size_t errlen)
{
    if (len < 1536) {
        snprintf(err, errlen, "volume too small");
        return NULL;
    }
    const uint8_t *mdb = d + 1024;
    uint16_t sig = be16(mdb);
    if (sig == 0xD2D7)
        return mfs_open(d, len, err, errlen);
    if (sig == 0x482B || sig == 0x4858)
        return hfsplus_open(d, len, err, errlen);
    if (sig != 0x4244) {
        snprintf(err, errlen, "not an HFS volume");
        return NULL;
    }
    if (be16(mdb + 124) == 0x482B) {
        /* An HFS wrapper: the HFS+ volume is its embedded extent. */
        size_t at = (size_t)be16(mdb + 28) * 512 + (size_t)be16(mdb + 126) * be32(mdb + 20);
        size_t n = (size_t)be16(mdb + 128) * be32(mdb + 20);
        if (at >= len) {
            snprintf(err, errlen, "embedded HFS+ volume beyond the image");
            return NULL;
        }
        return hfsplus_open(d + at, n && at + n <= len ? n : len - at, err, errlen);
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
    hfs_volinfo *in = &v->info;
    in->signature = 0x4244;
    in->create_date = be32(mdb + 2);
    in->modify_date = be32(mdb + 6);
    in->attributes = be16(mdb + 10);
    in->root_files = be16(mdb + 12);
    in->total_blocks = be16(mdb + 18);
    in->block_size = v->block_size;
    in->clump_size = be32(mdb + 24);
    in->first_block = v->first_block;
    in->next_cnid = be32(mdb + 30);
    in->free_blocks = be16(mdb + 34);
    in->backup_date = be32(mdb + 64);
    in->write_count = be32(mdb + 70);
    in->root_dirs = be16(mdb + 82);
    in->file_count = be32(mdb + 84);
    in->dir_count = be32(mdb + 88);
    memcpy(in->finder_info, mdb + 92, 32);
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
const hfs_volinfo *hfs_volume_info(const hfs_volume *v) { return &v->info; }

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

static bool hfsplus_list(hfs_volume *v, hfs_entry **entries, size_t *count, char *err, size_t errlen);
static bool mfs_list(hfs_volume *v, hfs_entry **entries, size_t *count, char *err, size_t errlen);

bool hfs_list(hfs_volume *v, hfs_entry **entries, size_t *count, char *err, size_t errlen)
{
    list_ctx ctx = { 0 };
    if (v->plus)
        return hfsplus_list(v, entries, count, err, errlen);
    if (v->mfs)
        return mfs_list(v, entries, count, err, errlen);
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

static bool hfsplus_read_fork(hfs_volume *v, const hfs_entry *e, bool rsrc, uint8_t **buf, size_t *len,
                              char *err, size_t errlen);
static bool mfs_read_fork(hfs_volume *v, const hfs_entry *e, bool rsrc, uint8_t **buf, size_t *len, char *err,
                          size_t errlen);

bool hfs_read_fork(hfs_volume *v, const hfs_entry *e, bool rsrc, uint8_t **buf, size_t *len, char *err,
                   size_t errlen)
{
    if (v->plus)
        return hfsplus_read_fork(v, e, rsrc, buf, len, err, errlen);
    if (v->mfs)
        return mfs_read_fork(v, e, rsrc, buf, len, err, errlen);
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

/* ---------------------------------------------------------------------- */
/* HFS+ */

static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

/* Copy a fork's blocks from up to eight extents (start, count: 32 bits
 * each); returns bytes copied. */
static size_t copy_plus_extents(hfs_volume *v, const uint8_t *ext, uint8_t *dst, size_t want, size_t done)
{
    for (int i = 0; i < 8 && done < want; i++) {
        uint32_t start = be32(ext + i * 8), count = be32(ext + i * 8 + 4);
        if (!count)
            break;
        size_t off = (size_t)start * v->block_size;
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

static uint32_t plus_extent_blocks(const uint8_t *ext)
{
    uint32_t sum = 0;
    for (int i = 0; i < 8; i++)
        sum += be32(ext + i * 8 + 4);
    return sum;
}

/* Extents overflow, HFS+: key keyLength(2)=10, forkType(1), pad(1),
 * fileID(4), startBlock(4); data: eight extents. */
typedef struct {
    uint8_t fork;
    uint32_t cnid, start;
    const uint8_t *found;
} plus_ext_ctx;

static bool find_plus_ext(void *vctx, const uint8_t *rec, size_t reclen)
{
    plus_ext_ctx *ctx = vctx;
    if (reclen < 12 + 64 || be16(rec) < 10)
        return true;
    if (rec[2] == ctx->fork && be32(rec + 4) == ctx->cnid && be32(rec + 8) == ctx->start) {
        ctx->found = rec + 12;
        return false;
    }
    return true;
}

/* Read a whole fork from its fork data record (logical size, ..., extents
 * at 16), going to the extents overflow file for more. */
static uint8_t *read_plus_fork(hfs_volume *v, const uint8_t *fork, uint8_t type, uint32_t cnid, size_t *outlen,
                               char *err, size_t errlen)
{
    uint64_t size64 = be64(fork);
    if (size64 > (uint64_t)v->len) {
        snprintf(err, errlen, "fork of file %u larger than the volume", cnid);
        return NULL;
    }
    size_t want = (size_t)size64;
    uint8_t *out = malloc(want ? want : 1);
    if (!out) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    size_t done = copy_plus_extents(v, fork + 16, out, want, 0);
    uint32_t blocks = plus_extent_blocks(fork + 16);
    while (done < want) {
        plus_ext_ctx ctx = { type, cnid, blocks, NULL };
        if (!v->extents || !walk_leaves(v->extents, v->extents_len, find_plus_ext, &ctx) || !ctx.found) {
            snprintf(err, errlen, "missing extents for file %u", cnid);
            free(out);
            return NULL;
        }
        size_t before = done;
        done = copy_plus_extents(v, ctx.found, out, want, done);
        blocks += plus_extent_blocks(ctx.found);
        if (done == before) {
            snprintf(err, errlen, "extents for file %u point outside the volume", cnid);
            free(out);
            return NULL;
        }
    }
    *outlen = want;
    return out;
}

static hfs_volume *hfsplus_open(const uint8_t *d, size_t len, char *err, size_t errlen)
{
    if (len < 1536) {
        snprintf(err, errlen, "HFS+ volume too small");
        return NULL;
    }
    const uint8_t *vh = d + 1024;
    hfs_volume *v = calloc(1, sizeof *v);
    if (!v) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    v->d = d;
    v->len = len;
    v->plus = true;
    v->block_size = be32(vh + 40);
    if (!v->block_size || v->block_size % 512) {
        snprintf(err, errlen, "bad allocation block size %u", v->block_size);
        free(v);
        return NULL;
    }
    hfs_volinfo *in = &v->info;
    in->signature = be16(vh);
    in->attributes = (uint16_t)be32(vh + 4);
    in->create_date = be32(vh + 16);
    in->modify_date = be32(vh + 20);
    in->backup_date = be32(vh + 24);
    in->file_count = be32(vh + 32);
    in->dir_count = be32(vh + 36);
    in->block_size = v->block_size;
    in->total_blocks = be32(vh + 44);
    in->free_blocks = be32(vh + 48);
    in->clump_size = be32(vh + 60);
    in->next_cnid = be32(vh + 64);
    in->write_count = be32(vh + 68);
    memcpy(in->finder_info, vh + 80, 32);
    char ferr[128];
    v->extents = read_plus_fork(v, vh + 192, 0, 3, &v->extents_len, ferr, sizeof ferr);
    v->catalog = read_plus_fork(v, vh + 272, 0, 4, &v->catalog_len, ferr, sizeof ferr);
    if (!v->catalog || v->catalog_len < 512) {
        snprintf(err, errlen, "cannot read the catalog (%s)", ferr);
        hfs_close(v);
        return NULL;
    }
    return v;
}

/* Precomposed MacRoman letters from a base letter and a combining mark
 * (HFS+ names are decomposed). */
static const uint16_t compose_table[][3] = {
    { 0x003D, 0x0338, 0x2260 }, { 0x0041, 0x0300, 0x00C0 }, { 0x0041, 0x0301, 0x00C1 }, { 0x0041, 0x0302, 0x00C2 },
    { 0x0041, 0x0303, 0x00C3 }, { 0x0041, 0x0308, 0x00C4 }, { 0x0041, 0x030A, 0x00C5 }, { 0x0043, 0x0327, 0x00C7 },
    { 0x0045, 0x0300, 0x00C8 }, { 0x0045, 0x0301, 0x00C9 }, { 0x0045, 0x0302, 0x00CA }, { 0x0045, 0x0308, 0x00CB },
    { 0x0049, 0x0300, 0x00CC }, { 0x0049, 0x0301, 0x00CD }, { 0x0049, 0x0302, 0x00CE }, { 0x0049, 0x0308, 0x00CF },
    { 0x004E, 0x0303, 0x00D1 }, { 0x004F, 0x0300, 0x00D2 }, { 0x004F, 0x0301, 0x00D3 }, { 0x004F, 0x0302, 0x00D4 },
    { 0x004F, 0x0303, 0x00D5 }, { 0x004F, 0x0308, 0x00D6 }, { 0x0055, 0x0300, 0x00D9 }, { 0x0055, 0x0301, 0x00DA },
    { 0x0055, 0x0302, 0x00DB }, { 0x0055, 0x0308, 0x00DC }, { 0x0059, 0x0308, 0x0178 }, { 0x0061, 0x0300, 0x00E0 },
    { 0x0061, 0x0301, 0x00E1 }, { 0x0061, 0x0302, 0x00E2 }, { 0x0061, 0x0303, 0x00E3 }, { 0x0061, 0x0308, 0x00E4 },
    { 0x0061, 0x030A, 0x00E5 }, { 0x0063, 0x0327, 0x00E7 }, { 0x0065, 0x0300, 0x00E8 }, { 0x0065, 0x0301, 0x00E9 },
    { 0x0065, 0x0302, 0x00EA }, { 0x0065, 0x0308, 0x00EB }, { 0x0069, 0x0300, 0x00EC }, { 0x0069, 0x0301, 0x00ED },
    { 0x0069, 0x0302, 0x00EE }, { 0x0069, 0x0308, 0x00EF }, { 0x006E, 0x0303, 0x00F1 }, { 0x006F, 0x0300, 0x00F2 },
    { 0x006F, 0x0301, 0x00F3 }, { 0x006F, 0x0302, 0x00F4 }, { 0x006F, 0x0303, 0x00F5 }, { 0x006F, 0x0308, 0x00F6 },
    { 0x0075, 0x0300, 0x00F9 }, { 0x0075, 0x0301, 0x00FA }, { 0x0075, 0x0302, 0x00FB }, { 0x0075, 0x0308, 0x00FC },
    { 0x0079, 0x0308, 0x00FF },
};

static uint32_t compose(uint32_t base, uint32_t mark)
{
    for (size_t i = 0; i < sizeof compose_table / sizeof compose_table[0]; i++)
        if (compose_table[i][0] == base && compose_table[i][1] == mark)
            return compose_table[i][2];
    return 0;
}

static size_t put_utf8(char *out, size_t at, size_t cap, uint32_t c)
{
    if (c < 0x80 && at + 1 < cap) {
        out[at++] = (char)c;
    } else if (c < 0x800 && at + 2 < cap) {
        out[at++] = (char)(0xC0 | c >> 6);
        out[at++] = (char)(0x80 | (c & 0x3F));
    } else if (c < 0x10000 && at + 3 < cap) {
        out[at++] = (char)(0xE0 | c >> 12);
        out[at++] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[at++] = (char)(0x80 | (c & 0x3F));
    } else if (at + 4 < cap) {
        out[at++] = (char)(0xF0 | c >> 18);
        out[at++] = (char)(0x80 | ((c >> 12) & 0x3F));
        out[at++] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[at++] = (char)(0x80 | (c & 0x3F));
    }
    return at;
}

/* An HFS+ name (big-endian UTF-16, decomposed) as a MacRoman Pascal string. */
static void plus_name(const uint8_t *u, unsigned n, uint8_t *pstr)
{
    uint32_t cps[255];
    unsigned k = 0;
    for (unsigned i = 0; i < n && k < 255; i++) {
        uint32_t c = be16(u + 2 * i);
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n) {
            uint32_t lo = be16(u + 2 * (i + 1));
            c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
            i++;
        }
        uint32_t composed = k ? compose(cps[k - 1], c) : 0;
        if (composed)
            cps[k - 1] = composed;
        else
            cps[k++] = c;
    }
    char utf8[1024];
    size_t at = 0;
    for (unsigned i = 0; i < k; i++)
        at = put_utf8(utf8, at, sizeof utf8, cps[i]);
    utf8[at] = 0;
    uint8_t mr[256];
    size_t m = utf8_to_macroman(utf8, mr, sizeof mr);
    if (m > 255)
        m = 255;
    pstr[0] = (uint8_t)m;
    memcpy(pstr + 1, mr, m);
}

typedef struct {
    hfs_entry *list;
    size_t n, cap;
    bool oom;
    uint32_t private_dir; /* the "HFS+ Private Data" folder's ID */
} plus_ctx;

static bool collect_plus(void *vctx, const uint8_t *rec, size_t reclen)
{
    plus_ctx *ctx = vctx;
    if (reclen < 8)
        return true;
    size_t keylen = be16(rec);
    size_t doff = 2 + keylen;
    if (doff + 2 > reclen || keylen < 6)
        return true;
    const uint8_t *data = rec + doff;
    uint16_t type = be16(data);
    if (type != 1 && type != 2)
        return true; /* threads */
    unsigned nlen = be16(rec + 6);
    if (8 + 2 * (size_t)nlen > 2 + keylen)
        return true;
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
    plus_name(rec + 8, nlen, e->name);
    if (type == 1) {
        if (doff + 88 > reclen)
            return true;
        e->is_dir = true;
        e->cnid = be32(data + 8);
        e->create_date = be32(data + 12);
        e->modify_date = be32(data + 16);
        memcpy(e->finder_info, data + 48, 32);
        /* The hidden folder of hard links' files: four NULs, then its name. */
        if (nlen >= 4 && !be16(rec + 8) && !be16(rec + 10) && !be16(rec + 12) && !be16(rec + 14))
            ctx->private_dir = e->cnid;
    } else {
        if (doff + 248 > reclen)
            return true;
        e->cnid = be32(data + 8);
        e->create_date = be32(data + 12);
        e->modify_date = be32(data + 16);
        memcpy(e->finder_info, data + 48, 32);
        memcpy(e->data_fork, data + 88, 80);
        memcpy(e->rsrc_fork, data + 168, 80);
        uint64_t dl = be64(data + 88), rl = be64(data + 168);
        e->data_len = dl > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)dl;
        e->rsrc_len = rl > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)rl;
        /* A hard link: its file's number, in the permissions' special word. */
        if (!memcmp(e->finder_info, "hlnkhfs+", 8))
            e->data_ext[0] = 1, memcpy(e->data_ext + 4, data + 44, 4);
    }
    ctx->n++;
    return true;
}

static bool hfsplus_list(hfs_volume *v, hfs_entry **entries, size_t *count, char *err, size_t errlen)
{
    plus_ctx ctx = { 0 };
    bool ok = walk_leaves(v->catalog, v->catalog_len, collect_plus, &ctx);
    if (!ok || ctx.oom) {
        snprintf(err, errlen, ctx.oom ? "out of memory" : "damaged catalog B-tree");
        free(ctx.list);
        return false;
    }
    /* Hard links take their file's forks; the private folder's own
     * entries are left out. */
    size_t out = 0;
    for (size_t i = 0; i < ctx.n; i++) {
        hfs_entry *e = &ctx.list[i];
        if (ctx.private_dir && (e->cnid == ctx.private_dir || e->parent == ctx.private_dir))
            continue;
        if (!e->is_dir && e->data_ext[0] == 1 && ctx.private_dir) {
            uint32_t inode = be32(e->data_ext + 4);
            char want[32];
            snprintf(want, sizeof want, "iNode%u", inode);
            for (size_t j = 0; j < ctx.n; j++) {
                hfs_entry *t = &ctx.list[j];
                if (t->parent == ctx.private_dir && t->name[0] == strlen(want) &&
                    !memcmp(t->name + 1, want, t->name[0])) {
                    memcpy(e->data_fork, t->data_fork, 80);
                    memcpy(e->rsrc_fork, t->rsrc_fork, 80);
                    e->data_len = t->data_len;
                    e->rsrc_len = t->rsrc_len;
                    memcpy(e->finder_info, t->finder_info, 32);
                    e->cnid = t->cnid;
                    break;
                }
            }
        }
        memset(e->data_ext, 0, sizeof e->data_ext);
        if (e->is_dir && e->cnid == 2) /* the root folder: the volume's name */
            memcpy(v->name, e->name, (size_t)e->name[0] + 1);
        ctx.list[out++] = *e;
    }
    *entries = ctx.list;
    *count = out;
    return true;
}

static bool hfsplus_read_fork(hfs_volume *v, const hfs_entry *e, bool rsrc, uint8_t **buf, size_t *len,
                              char *err, size_t errlen)
{
    uint8_t *out = read_plus_fork(v, rsrc ? e->rsrc_fork : e->data_fork, rsrc ? 0xFF : 0x00, e->cnid, len, err,
                                  errlen);
    if (!out)
        return false;
    *buf = out;
    return true;
}

/* ---------------------------------------------------------------------- */
/* MFS */

/* The master directory block's map: a 12-bit link for each allocation
 * block from 2 on (0 free, 1 a file's last block, else the next block). */
static uint16_t mfs_link(const hfs_volume *v, uint32_t block)
{
    size_t bit = (size_t)(block - 2) * 12;
    size_t at = 1024 + 64 + bit / 8;
    if (block < 2 || at + 1 >= v->len)
        return 1;
    uint16_t w = be16(v->d + at);
    return bit % 8 ? w & 0x0FFF : w >> 4;
}

static hfs_volume *mfs_open(const uint8_t *d, size_t len, char *err, size_t errlen)
{
    const uint8_t *mdb = d + 1024;
    hfs_volume *v = calloc(1, sizeof *v);
    if (!v) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    v->d = d;
    v->len = len;
    v->mfs = true;
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
    hfs_volinfo *in = &v->info;
    in->signature = 0xD2D7;
    in->create_date = in->modify_date = be32(mdb + 2);
    in->backup_date = be32(mdb + 6);
    in->attributes = be16(mdb + 10);
    in->root_files = in->file_count = be16(mdb + 12);
    in->total_blocks = be16(mdb + 18);
    in->block_size = v->block_size;
    in->clump_size = be32(mdb + 24);
    in->first_block = v->first_block;
    in->next_cnid = be32(mdb + 30);
    in->free_blocks = be16(mdb + 34);
    /* The directory: drBlLen blocks from drDirSt. */
    size_t dir = (size_t)be16(mdb + 14) * 512, dlen = (size_t)be16(mdb + 16) * 512;
    if (dir >= len || dir + dlen > len) {
        snprintf(err, errlen, "file directory beyond the volume");
        free(v);
        return NULL;
    }
    v->catalog = malloc(dlen ? dlen : 1);
    if (!v->catalog) {
        free(v);
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    memcpy(v->catalog, d + dir, dlen);
    v->catalog_len = dlen;
    return v;
}

/* Directory entries: flags (bit 7 in use), version, FInfo, file number,
 * the data fork's first block, logical and physical lengths, the resource
 * fork's, dates and name; an entry never crosses a 512-byte block, and the
 * rest of a block after the last is zero. */
static bool mfs_list(hfs_volume *v, hfs_entry **entries, size_t *count, char *err, size_t errlen)
{
    size_t n = 0, cap = 64;
    hfs_entry *list = calloc(cap, sizeof *list);
    if (!list) {
        snprintf(err, errlen, "out of memory");
        return false;
    }
    /* The root folder, the volume itself. */
    list[n].cnid = 2;
    list[n].parent = 1;
    list[n].is_dir = true;
    memcpy(list[n].name, v->name, (size_t)v->name[0] + 1);
    n++;
    for (size_t blk = 0; blk < v->catalog_len; blk += 512) {
        const uint8_t *b = v->catalog + blk;
        for (size_t at = 0; at + 51 <= 512;) {
            const uint8_t *e = b + at;
            if (!(e[0] & 0x80))
                break;
            size_t elen = 51 + (size_t)e[50];
            if (at + elen > 512)
                break;
            if (n == cap) {
                hfs_entry *nl = realloc(list, cap * 2 * sizeof *nl);
                if (!nl) {
                    free(list);
                    snprintf(err, errlen, "out of memory");
                    return false;
                }
                list = nl;
                cap *= 2;
            }
            hfs_entry *h = &list[n++];
            memset(h, 0, sizeof *h);
            h->parent = 2;
            h->cnid = be32(e + 18);
            memcpy(h->finder_info, e + 2, 16);
            memset(h->finder_info + 14, 0, 2);   /* fdFldr: MFS's folders are the Finder's own */
            memcpy(h->data_ext, e + 22, 2);
            h->data_len = be32(e + 24);
            memcpy(h->rsrc_ext, e + 32, 2);
            h->rsrc_len = be32(e + 34);
            h->create_date = be32(e + 42);
            h->modify_date = be32(e + 46);
            uint8_t nl = e[50] > 31 ? 31 : e[50];
            h->name[0] = nl;
            memcpy(h->name + 1, e + 51, nl);
            at += (elen + 1) & ~(size_t)1;
        }
    }
    *entries = list;
    *count = n;
    return true;
}

static bool mfs_read_fork(hfs_volume *v, const hfs_entry *e, bool rsrc, uint8_t **buf, size_t *len, char *err,
                          size_t errlen)
{
    size_t want = rsrc ? e->rsrc_len : e->data_len;
    uint8_t *out = malloc(want ? want : 1);
    if (!out) {
        snprintf(err, errlen, "out of memory");
        return false;
    }
    uint32_t block = be16(rsrc ? e->rsrc_ext : e->data_ext);
    size_t done = 0;
    unsigned guard = 0;
    while (done < want) {
        size_t off = (size_t)v->first_block * 512 + (size_t)(block - 2) * v->block_size;
        if (block < 2 || off >= v->len || ++guard > 65536) {
            snprintf(err, errlen, "file %u's blocks run outside the volume", e->cnid);
            free(out);
            return false;
        }
        size_t n = v->block_size;
        if (n > want - done)
            n = want - done;
        if (off + n > v->len)
            n = v->len - off;
        memcpy(out + done, v->d + off, n);
        done += n;
        block = mfs_link(v, block);
    }
    *buf = out;
    *len = want;
    return true;
}

/* ---------------------------------------------------------------------- */
/* Copying a volume's files out */

/* A folder and those leading to it. */
static void mkdir_p(const char *path)
{
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    mkdir(tmp, 0755);
}

/* A Mac name as a host one: '/' (a host path's separator) becomes ':'. */
static void host_name(const uint8_t *pname, char *out, size_t outsize)
{
    uint8_t tmp[32];
    int n = pname[0] > 31 ? 31 : pname[0];
    for (int i = 0; i < n; i++)
        tmp[i] = pname[1 + i] == '/' ? ':' : pname[1 + i];
    macroman_to_utf8(tmp, (size_t)n, out, outsize);
    if (!out[0] || !strcmp(out, ".") || !strcmp(out, ".."))
        snprintf(out, outsize, "Untitled");
}

/* A folder's host path by its ID (the root, 2, is root); depth-limited
 * against loops in a damaged catalog. */
static bool folder_path(const hfs_entry *list, size_t n, uint32_t id, const char *root, char *out, size_t outsize,
                        int depth)
{
    if (id == 2) {
        snprintf(out, outsize, "%s", root);
        return true;
    }
    if (depth > 100)
        return false;
    for (size_t i = 0; i < n; i++)
        if (list[i].is_dir && list[i].cnid == id) {
            char parent[4096], name[256];
            if (!folder_path(list, n, list[i].parent, root, parent, sizeof parent, depth + 1))
                return false;
            host_name(list[i].name, name, sizeof name);
            snprintf(out, outsize, "%s/%s", parent, name);
            return true;
        }
    return false;
}

bool hfs_extract(hfs_volume *v, const char *root, hfs_keep_fn keep, hfs_note_fn note, void *ctx, char *err,
                 size_t errlen)
{
    hfs_entry *list;
    size_t n;
    if (!hfs_list(v, &list, &n, err, errlen))
        return false;
    char path[4096], parent[4096], name[256];
    for (size_t i = 0; i < n; i++) {
        if (!list[i].is_dir || list[i].cnid == 2)
            continue;
        if (folder_path(list, n, list[i].cnid, root, path, sizeof path, 0))
            mkdir_p(path);
    }
    for (size_t i = 0; i < n; i++) {
        hfs_entry *e = &list[i];
        if (e->is_dir || !folder_path(list, n, e->parent, root, parent, sizeof parent, 0))
            continue;
        host_name(e->name, name, sizeof name);
        snprintf(path, sizeof path, "%s/%s", parent, name);
        if (keep && !keep(ctx, path + strlen(root) + 1))
            continue;
        mac_file f = { 0 };
        char ferr[256] = "";
        size_t dlen = 0, rlen = 0;
        if (!hfs_read_fork(v, e, false, &f.data, &dlen, ferr, sizeof ferr) ||
            !hfs_read_fork(v, e, true, &f.rsrc, &rlen, ferr, sizeof ferr)) {
            free(f.data);
            if (note) {
                char msg[512];
                snprintf(msg, sizeof msg, "%s: skipped (%s; image truncated or damaged?)", name, ferr);
                note(ctx, msg);
            }
            continue;
        }
        f.data_len = dlen;
        f.rsrc_len = rlen;
        memcpy(f.finder_info, e->finder_info, 32);
        f.has_finder_info = true;
        memcpy(f.name, e->name + 1, e->name[0] > 63 ? 63 : e->name[0]);
        f.create_date = e->create_date;
        f.modify_date = e->modify_date;
        bool ok = appledouble_write(path, &f, ferr, sizeof ferr);
        mac_file_free(&f);
        if (!ok) {
            snprintf(err, errlen, "%s", ferr);
            free(list);
            return false;
        }
    }
    free(list);
    return true;
}
