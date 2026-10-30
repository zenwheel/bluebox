/*
 * Writing an HFS volume: a disk's worth of bytes holding a set of files and
 * folders, as the File Manager would have left them (for a disk a program
 * serves that is written back, and for initializing one).
 *
 * The layout: boot blocks, the master directory block in sector 2, the
 * volume bitmap from sector 3, then the allocation blocks: the extents
 * overflow file (empty: every fork here is one extent), the catalog, and
 * each fork in turn; the alternate master directory block in the
 * next-to-last sector.  The catalog is a B*-tree of 512-byte nodes: the
 * header node, the leaves (folder threads and folder and file records,
 * in key order), index nodes over them, and map nodes when the header's
 * map can't cover the file.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diskimage.h"

enum { SECTOR = 512, NODE = 512, CAT_KEYLEN = 37, XT_KEYLEN = 7 };

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static int ascii_compare(const uint8_t *a, const uint8_t *b)
{
    int n = a[0] < b[0] ? a[0] : b[0];
    for (int i = 1; i <= n; i++) {
        int x = a[i] >= 'a' && a[i] <= 'z' ? a[i] - 32 : a[i];
        int y = b[i] >= 'a' && b[i] <= 'z' ? b[i] - 32 : b[i];
        if (x != y)
            return x - y;
    }
    return a[0] - b[0];
}

/* A catalog record: its key (parent ID, name) and its data. */
typedef struct {
    uint32_t parent;
    const uint8_t *name; /* Pascal */
    uint8_t data[102];
    uint16_t len;
} record;

static int (*sort_compare)(const uint8_t *, const uint8_t *);

static int record_order(const void *pa, const void *pb)
{
    const record *a = pa, *b = pb;
    if (a->parent != b->parent)
        return a->parent < b->parent ? -1 : 1;
    return sort_compare(a->name, b->name);
}

/* A leaf record's bytes: the key (length, reserved, parent, name), padded
 * to an even length, then the data. */
static uint16_t leaf_size(const record *r) { return (uint16_t)(((8 + r->name[0]) & ~1) + r->len); }

static void put_leaf(uint8_t *p, const record *r)
{
    memset(p, 0, leaf_size(r));
    p[0] = (uint8_t)(6 + r->name[0]);
    put32(p + 2, r->parent);
    memcpy(p + 6, r->name, (size_t)r->name[0] + 1);
    memcpy(p + ((8 + r->name[0]) & ~1), r->data, r->len);
}

/* An index record: the key at its full length (HFS's index keys always
 * are), and the child's node number. */
static void put_index(uint8_t *p, const record *r, uint32_t child)
{
    memset(p, 0, 42);
    p[0] = CAT_KEYLEN;
    put32(p + 2, r->parent);
    memcpy(p + 6, r->name, (size_t)r->name[0] + 1);
    put32(p + 38, child);
}

static void put_descriptor(uint8_t *n, uint32_t flink, uint32_t blink, int8_t kind, uint8_t height, uint16_t nrec)
{
    put32(n, flink);
    put32(n + 4, blink);
    n[8] = (uint8_t)kind;
    n[9] = height;
    put16(n + 10, nrec);
}

/* A record's offset (and, after the last, the free space's) at the end of
 * the node, counted from the end. */
static void put_offset(uint8_t *n, int i, uint16_t off) { put16(n + NODE - 2 * (i + 1), off); }

/* The header node: the tree's description, 128 bytes of the user's, and
 * the map of the first 2048 nodes; further map nodes follow it. */
static void put_header(uint8_t *tree, uint32_t total, uint32_t used, uint16_t depth, uint32_t root, uint32_t leaf_recs,
                       uint32_t first_leaf, uint32_t last_leaf, uint16_t keylen, uint32_t first_map)
{
    uint8_t *n = tree;
    put_descriptor(n, first_map, 0, 1, 0, 3);
    uint8_t *h = n + 14;
    put16(h, depth);
    put32(h + 2, root);
    put32(h + 6, leaf_recs);
    put32(h + 10, first_leaf);
    put32(h + 14, last_leaf);
    put16(h + 18, NODE);
    put16(h + 20, keylen);
    put32(h + 22, total);
    put32(h + 26, total - used);
    put_offset(n, 0, 14);
    put_offset(n, 1, 120);
    put_offset(n, 2, 248);
    put_offset(n, 3, 504);
    /* The map: a bit for each node in use, the header's 256 bytes first,
     * then each map node's 494. */
    for (uint32_t i = 0; i < used; i++) {
        uint8_t *m;
        uint32_t bit = i;
        if (bit < 2048) {
            m = n + 248;
        } else {
            uint32_t k = (bit - 2048) / 3952;
            m = tree + (size_t)(first_map + k) * NODE + 14;
            bit = (bit - 2048) % 3952;
        }
        m[bit / 8] |= (uint8_t)(0x80 >> (bit % 8));
    }
}

/* Map nodes after the header's map: how many a tree of total nodes needs. */
static uint32_t map_nodes(uint32_t total) { return total <= 2048 ? 0 : (total - 2048 + 3951) / 3952; }

static uint32_t round_up(uint32_t n, uint32_t m) { return (n + m - 1) / m * m; }

bool hfs_build(const hfs_build_params *p, const hfs_build_entry *entries, size_t count, uint8_t **out, char *err,
               size_t errlen)
{
    uint32_t sectors = p->size / SECTOR;
    if (sectors < 64) {
        snprintf(err, errlen, "disk too small for HFS");
        return false;
    }
    /* Allocation blocks: at most 65535, of the size asked or the smallest
     * multiple of 512 bytes that covers the disk; the bitmap before them. */
    uint32_t bs = p->block_size, nb = 0, alst = 0;
    for (uint32_t try = bs ? bs : SECTOR;; try += SECTOR) {
        uint32_t est = (uint32_t)((uint64_t)(sectors - 5) * SECTOR / try);
        if (est > 65535 && !bs)
            continue;
        if (est > 65535)
            est = 65535;
        alst = 3 + (est + 4095) / 4096;
        nb = (uint32_t)((uint64_t)(sectors - alst - 2) * SECTOR / try);
        if (nb > 65535)
            nb = 65535;
        bs = try;
        break;
    }
    if (bs % SECTOR || !nb) {
        snprintf(err, errlen, "bad allocation block size %u", bs);
        return false;
    }
    uint64_t vol_bytes = (uint64_t)nb * bs;

    /* The catalog's records. */
    size_t nrec = 2;
    for (size_t i = 0; i < count; i++)
        nrec += entries[i].is_dir ? 2 : 1;
    record *recs = calloc(nrec, sizeof *recs);
    uint32_t *valence = NULL;
    uint32_t max_id = 15;
    for (size_t i = 0; i < count; i++)
        if (entries[i].cnid > max_id)
            max_id = entries[i].cnid;
    valence = calloc((size_t)max_id + 1, sizeof *valence);
    if (!recs || !valence) {
        free(recs);
        free(valence);
        snprintf(err, errlen, "out of memory");
        return false;
    }
    uint32_t files = 0, dirs = 0, root_files = 0, root_dirs = 0;
    for (size_t i = 0; i < count; i++) {
        const hfs_build_entry *e = &entries[i];
        if (e->parent <= max_id)
            valence[e->parent]++;
        if (e->is_dir) {
            dirs++;
            root_dirs += e->parent == 2;
        } else {
            files++;
            root_files += e->parent == 2;
        }
    }
    uint8_t vname[32] = { 0 };
    memcpy(vname, p->name, (size_t)(p->name[0] > 27 ? 27 : p->name[0]) + 1);
    if (vname[0] > 27)
        vname[0] = 27;

    /* Space: the extents file, the catalog (room for its records and some
     * more), then the forks. */
    uint32_t leaf_bytes = 0;
    size_t r = 0;
    record *root = &recs[r++], *root_thread = &recs[r++];
    root->parent = 1;
    root->name = vname;
    root_thread->parent = 2;
    static const uint8_t empty[1] = { 0 };
    root_thread->name = empty;
    for (size_t i = 0; i < count; i++) {
        const hfs_build_entry *e = &entries[i];
        recs[r].parent = e->parent;
        recs[r].name = e->name;
        recs[r].len = e->is_dir ? 70 : 102;
        r++;
        if (e->is_dir) {
            recs[r].parent = e->cnid;
            recs[r].name = empty;
            recs[r].len = 46;
            r++;
        }
    }
    root->len = 70;
    root_thread->len = 46;
    for (size_t i = 0; i < nrec; i++)
        leaf_bytes += leaf_size(&recs[i]) + 2;
    uint32_t leaves_est = leaf_bytes / (NODE - 16) + 1;
    uint32_t cat_nodes_est = 1 + leaves_est + leaves_est / 10 + 3;
    uint32_t xt_bytes = round_up((uint32_t)(vol_bytes / 128 < 4 * NODE ? 4 * NODE : vol_bytes / 128 > (1u << 20) ? (1u << 20) : vol_bytes / 128), bs);
    uint32_t cat_bytes = cat_nodes_est * NODE + cat_nodes_est * NODE / 4 + 8 * NODE;
    if (cat_bytes < xt_bytes)
        cat_bytes = xt_bytes;
    cat_bytes = round_up(cat_bytes, bs);
    uint32_t next = 0;
    uint32_t xt_start = next;
    next += xt_bytes / bs;
    uint32_t cat_start = next;
    next += cat_bytes / bs;
    /* Each fork, one extent. */
    uint32_t *dstart = calloc(count ? count : 1, sizeof *dstart), *rstart = calloc(count ? count : 1, sizeof *rstart);
    if (!dstart || !rstart) {
        free(recs);
        free(valence);
        free(dstart);
        free(rstart);
        snprintf(err, errlen, "out of memory");
        return false;
    }
    for (size_t i = 0; i < count && next <= nb; i++) {
        if (entries[i].is_dir)
            continue;
        dstart[i] = next;
        next += (entries[i].data_len + bs - 1) / bs;
        rstart[i] = next;
        next += (entries[i].rsrc_len + bs - 1) / bs;
    }
    if (next > nb) {
        snprintf(err, errlen, "the files need %u allocation blocks of %u bytes; the disk has %u", next, bs, nb);
        free(recs);
        free(valence);
        free(dstart);
        free(rstart);
        return false;
    }

    /* The records' data. */
    r = 0;
    {
        uint8_t *d = root->data;
        d[0] = 1;
        put16(d + 4, (uint16_t)(valence[2] > 0xFFFF ? 0xFFFF : valence[2]));
        put32(d + 6, 2);
        put32(d + 10, p->create_date);
        put32(d + 14, p->modify_date);
        put32(d + 18, p->backup_date);
        d = root_thread->data;
        d[0] = 3;
        put32(d + 10, 1);
        memcpy(d + 14, vname, (size_t)vname[0] + 1);
    }
    r = 2;
    for (size_t i = 0; i < count; i++) {
        const hfs_build_entry *e = &entries[i];
        uint8_t *d = recs[r++].data;
        if (e->is_dir) {
            d[0] = 1;
            uint32_t val = e->cnid <= max_id ? valence[e->cnid] : 0;
            put16(d + 4, (uint16_t)(val > 0xFFFF ? 0xFFFF : val));
            put32(d + 6, e->cnid);
            put32(d + 10, e->create_date);
            put32(d + 14, e->modify_date);
            memcpy(d + 22, e->finder_info, 32);
            d = recs[r++].data;
            d[0] = 3;
            put32(d + 10, e->parent);
            memcpy(d + 14, e->name, (size_t)(e->name[0] > 31 ? 31 : e->name[0]) + 1);
        } else {
            d[0] = 2;
            memcpy(d + 4, e->finder_info, 16);
            put32(d + 20, e->cnid);
            uint32_t db = (e->data_len + bs - 1) / bs, rb = (e->rsrc_len + bs - 1) / bs;
            put16(d + 24, (uint16_t)(db ? dstart[i] : 0));
            put32(d + 26, e->data_len);
            put32(d + 30, db * bs);
            put16(d + 34, (uint16_t)(rb ? rstart[i] : 0));
            put32(d + 36, e->rsrc_len);
            put32(d + 40, rb * bs);
            put32(d + 44, e->create_date);
            put32(d + 48, e->modify_date);
            memcpy(d + 56, e->finder_info + 16, 16);
            if (db) {
                put16(d + 74, (uint16_t)dstart[i]);
                put16(d + 76, (uint16_t)db);
            }
            if (rb) {
                put16(d + 86, (uint16_t)rstart[i]);
                put16(d + 88, (uint16_t)rb);
            }
        }
    }
    sort_compare = p->compare ? p->compare : ascii_compare;
    qsort(recs, nrec, sizeof *recs, record_order);

    /* The catalog file. */
    uint32_t total = cat_bytes / NODE;
    uint8_t *cat = calloc(1, cat_bytes);
    /* Leaf and index nodes, level by level: each level's first keys. */
    uint32_t *first = malloc((nrec + 1) * sizeof *first); /* record index starting each node */
    uint32_t *nodes = malloc((nrec + 1) * sizeof *nodes);
    if (!cat || !first || !nodes) {
        free(cat);
        free(first);
        free(nodes);
        free(recs);
        free(valence);
        free(dstart);
        free(rstart);
        snprintf(err, errlen, "out of memory");
        return false;
    }
    uint32_t node = 1, nleaves = 0;
    bool full = false;
    for (size_t i = 0; i < nrec;) {
        if (node >= total) {
            full = true;
            break;
        }
        uint8_t *n = cat + (size_t)node * NODE;
        uint16_t at = 14, k = 0;
        first[nleaves] = (uint32_t)i;
        nodes[nleaves] = node;
        while (i < nrec && at + leaf_size(&recs[i]) + 2 * (k + 2) <= NODE) {
            put_leaf(n + at, &recs[i]);
            put_offset(n, k, at);
            at = (uint16_t)(at + leaf_size(&recs[i]));
            k++;
            i++;
        }
        put_offset(n, k, at);
        put_descriptor(n, 0, nleaves ? nodes[nleaves - 1] : 0, -1, 1, k);
        if (nleaves)
            put32(cat + (size_t)nodes[nleaves - 1] * NODE, node);
        nleaves++;
        node++;
    }
    /* Index levels until one node holds the level. */
    uint32_t level_n = nleaves, height = 1, rootnode = nodes[0];
    uint32_t *lfirst = first, *lnodes = nodes;
    while (!full && level_n > 1) {
        height++;
        uint32_t up_n = 0;
        uint32_t *ufirst = malloc((level_n + 1) * sizeof *ufirst), *unodes = malloc((level_n + 1) * sizeof *unodes);
        for (uint32_t i = 0; i < level_n;) {
            if (node >= total) {
                full = true;
                break;
            }
            uint8_t *n = cat + (size_t)node * NODE;
            uint16_t k = 0;
            ufirst[up_n] = lfirst[i];
            unodes[up_n] = node;
            while (i < level_n && 14 + 42 * (k + 1) + 2 * (k + 2) <= NODE) {
                put_index(n + 14 + 42 * k, &recs[lfirst[i]], lnodes[i]);
                put_offset(n, k, (uint16_t)(14 + 42 * k));
                k++;
                i++;
            }
            put_offset(n, k, (uint16_t)(14 + 42 * k));
            put_descriptor(n, 0, up_n ? unodes[up_n - 1] : 0, 0, (uint8_t)height, k);
            if (up_n)
                put32(cat + (size_t)unodes[up_n - 1] * NODE, node);
            up_n++;
            node++;
        }
        if (lfirst != first) {
            free(lfirst);
            free(lnodes);
        }
        lfirst = ufirst;
        lnodes = unodes;
        level_n = up_n;
        rootnode = unodes[0];
    }
    if (lfirst != first) {
        free(lfirst);
        free(lnodes);
    }
    uint32_t maps = map_nodes(total);
    if (node + maps > total)
        full = true;
    if (full) {
        snprintf(err, errlen, "the catalog doesn't fit");
        free(cat);
        free(first);
        free(nodes);
        free(recs);
        free(valence);
        free(dstart);
        free(rstart);
        return false;
    }
    /* Map nodes at the end of the file, chained from the header. */
    for (uint32_t m = 0; m < maps; m++) {
        uint32_t at = total - maps + m;
        uint8_t *n = cat + (size_t)at * NODE;
        put_descriptor(n, m + 1 < maps ? at + 1 : 0, 0, 2, 0, 1);
        put_offset(n, 0, 14);
        put_offset(n, 1, NODE - 4);
    }
    /* Nodes in use: the header, the leaves and index nodes, the map nodes
     * (marked by number: they sit at the end). */
    put_header(cat, total, node, (uint16_t)height, rootnode, (uint32_t)nrec, nodes[0], nodes[nleaves - 1], CAT_KEYLEN,
               maps ? total - maps : 0);
    for (uint32_t m = 0; m < maps; m++) {
        uint32_t bit = total - maps + m;
        uint8_t *mp = bit < 2048 ? cat + 248 : cat + (size_t)(total - maps + (bit - 2048) / 3952) * NODE + 14;
        uint32_t b = bit < 2048 ? bit : (bit - 2048) % 3952;
        mp[b / 8] |= (uint8_t)(0x80 >> (b % 8));
    }
    uint32_t used_nodes = node + maps;
    put32(cat + 14 + 26, total - used_nodes);

    /* The disk. */
    uint8_t *disk = calloc(1, p->size);
    if (!disk) {
        free(cat);
        free(first);
        free(nodes);
        free(recs);
        free(valence);
        free(dstart);
        free(rstart);
        snprintf(err, errlen, "out of memory");
        return false;
    }
    size_t base = (size_t)alst * SECTOR;
    memcpy(disk + base + (size_t)cat_start * bs, cat, cat_bytes);
    /* The extents tree: its header alone. */
    uint8_t *xt = disk + base + (size_t)xt_start * bs;
    uint32_t xt_total = xt_bytes / NODE;
    put_header(xt, xt_total, 1, 0, 0, 0, 0, 0, XT_KEYLEN, 0);
    for (size_t i = 0; i < count; i++) {
        const hfs_build_entry *e = &entries[i];
        if (e->is_dir)
            continue;
        if (e->data_len)
            memcpy(disk + base + (size_t)dstart[i] * bs, e->data, e->data_len);
        if (e->rsrc_len)
            memcpy(disk + base + (size_t)rstart[i] * bs, e->rsrc, e->rsrc_len);
    }
    /* The volume bitmap. */
    for (uint32_t b = 0; b < next; b++)
        disk[3 * SECTOR + b / 8] |= (uint8_t)(0x80 >> (b % 8));
    /* The master directory block, and its copy. */
    uint8_t *m = disk + 2 * SECTOR;
    put16(m, 0x4244);
    put32(m + 2, p->create_date);
    put32(m + 6, p->modify_date);
    put16(m + 10, (uint16_t)((p->attributes & 0x8080) | 0x0100));
    put16(m + 12, (uint16_t)root_files);
    put16(m + 14, 3);
    put16(m + 16, (uint16_t)next);
    put16(m + 18, (uint16_t)nb);
    put32(m + 20, bs);
    put32(m + 24, 4 * bs);
    put16(m + 28, (uint16_t)alst);
    put32(m + 30, max_id + 1);
    put16(m + 34, (uint16_t)(nb - next));
    memcpy(m + 36, vname, (size_t)vname[0] + 1);
    put32(m + 64, p->backup_date);
    put32(m + 70, p->write_count);
    put32(m + 74, xt_bytes);
    put32(m + 78, cat_bytes);
    put16(m + 82, (uint16_t)root_dirs);
    put32(m + 84, files);
    put32(m + 88, dirs);
    memcpy(m + 92, p->finder_info, 32);
    put32(m + 130, xt_bytes);
    put16(m + 134, (uint16_t)xt_start);
    put16(m + 136, (uint16_t)(xt_bytes / bs));
    put32(m + 146, cat_bytes);
    put16(m + 150, (uint16_t)cat_start);
    put16(m + 152, (uint16_t)(cat_bytes / bs));
    memcpy(disk + (size_t)(sectors - 2) * SECTOR, m, SECTOR);
    free(cat);
    free(first);
    free(nodes);
    free(recs);
    free(valence);
    free(dstart);
    free(rstart);
    *out = disk;
    return true;
}
