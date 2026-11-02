#include "forks.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/xattr.h>
#endif

enum {
    AD_MAGIC = 0x00051607,
    AD_VERSION = 0x00020000,
    AD_DATA = 1,
    AD_RSRC = 2,
    AD_NAME = 3,
    AD_COMMENT = 4,
    AD_DATES = 8,
    AD_FINDER = 9,
};

/* AppleDouble dates count seconds from 2000-01-01 GMT; Mac dates from 1904. */
#define AD_EPOCH_DELTA 3029529600u

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

char *appledouble_sidecar_path(const char *path)
{
    const char *slash = strrchr(path, '/');
    size_t dirlen = slash ? (size_t)(slash - path + 1) : 0;
    size_t n = strlen(path) + 3;
    char *s = malloc(n);
    if (!s)
        return NULL;
    memcpy(s, path, dirlen);
    s[dirlen] = '.';
    s[dirlen + 1] = '_';
    strcpy(s + dirlen + 2, path + dirlen);
    return s;
}

bool appledouble_decode(const uint8_t *b, size_t len, mac_file *out, char *err, size_t errlen)
{
    if (len < 26 || be32(b) != AD_MAGIC || (be32(b + 4) != AD_VERSION && be32(b + 4) != 0x00010000)) {
        snprintf(err, errlen, "not an AppleDouble file");
        return false;
    }
    unsigned n = be16(b + 24);
    if (26 + (size_t)n * 12 > len) {
        snprintf(err, errlen, "AppleDouble entry table is truncated");
        return false;
    }
    for (unsigned i = 0; i < n; i++) {
        const uint8_t *e = b + 26 + i * 12;
        uint32_t id = be32(e), off = be32(e + 4), elen = be32(e + 8);
        if ((size_t)off + elen > len) {
            snprintf(err, errlen, "AppleDouble entry %u is out of range", id);
            return false;
        }
        switch (id) {
        case AD_RSRC:
            free(out->rsrc);
            out->rsrc = malloc(elen ? elen : 1);
            if (!out->rsrc) {
                snprintf(err, errlen, "out of memory");
                return false;
            }
            memcpy(out->rsrc, b + off, elen);
            out->rsrc_len = elen;
            break;
        case AD_FINDER:
            /* macOS appends extended attributes after the 32 bytes. */
            memcpy(out->finder_info, b + off, elen < 32 ? elen : 32);
            out->has_finder_info = true;
            break;
        case AD_DATES:
            if (elen >= 8) {
                out->create_date = be32(b + off) + AD_EPOCH_DELTA;
                out->modify_date = be32(b + off + 4) + AD_EPOCH_DELTA;
            }
            break;
        case AD_COMMENT:
            out->comment_len = (uint8_t)(elen < sizeof out->comment ? elen : sizeof out->comment);
            memcpy(out->comment, b + off, out->comment_len);
            break;
        case AD_NAME: {
            size_t m = elen < sizeof out->name - 1 ? elen : sizeof out->name - 1;
            memcpy(out->name, b + off, m);
            out->name[m] = 0;
            break;
        }
        default:
            break;
        }
    }
    return true;
}

uint8_t *appledouble_encode(const mac_file *f, size_t *len_out)
{
    unsigned n = 3 + (f->name[0] ? 1 : 0) + (f->comment_len ? 1 : 0);
    size_t namelen = strlen(f->name);
    size_t hdr = 26 + n * 12;
    size_t total = hdr + 32 + 16 + namelen + f->comment_len + f->rsrc_len;
    uint8_t *b = calloc(1, total);
    if (!b)
        return NULL;
    put32(b, AD_MAGIC);
    put32(b + 4, AD_VERSION);
    put16(b + 24, (uint16_t)n);
    size_t off = hdr;
    uint8_t *e = b + 26;
    /* Finder info */
    put32(e, AD_FINDER); put32(e + 4, (uint32_t)off); put32(e + 8, 32); e += 12;
    memcpy(b + off, f->finder_info, 32);
    off += 32;
    /* Dates: create, modify, backup, access */
    put32(e, AD_DATES); put32(e + 4, (uint32_t)off); put32(e + 8, 16); e += 12;
    put32(b + off, f->create_date - AD_EPOCH_DELTA);
    put32(b + off + 4, f->modify_date - AD_EPOCH_DELTA);
    put32(b + off + 8, 0x80000000u); /* never backed up */
    put32(b + off + 12, f->modify_date - AD_EPOCH_DELTA);
    off += 16;
    if (namelen) {
        put32(e, AD_NAME); put32(e + 4, (uint32_t)off); put32(e + 8, (uint32_t)namelen); e += 12;
        memcpy(b + off, f->name, namelen);
        off += namelen;
    }
    if (f->comment_len) {
        put32(e, AD_COMMENT); put32(e + 4, (uint32_t)off); put32(e + 8, f->comment_len); e += 12;
        memcpy(b + off, f->comment, f->comment_len);
        off += f->comment_len;
    }
    /* Resource fork last, so it can grow without moving anything else. */
    put32(e, AD_RSRC); put32(e + 4, (uint32_t)off); put32(e + 8, (uint32_t)f->rsrc_len);
    memcpy(b + off, f->rsrc, f->rsrc_len);
    *len_out = total;
    return b;
}

/* Without a sidecar: the Finder info and resource fork macOS keeps with the
 * file itself (extended attributes), as Finder copies, file sharing and
 * files from real Macs carry them.  Read only; writes go to a sidecar,
 * which is read first from then on. */
bool appledouble_native_finder_info(const char *path, uint8_t out[32])
{
#ifdef __APPLE__
    return getxattr(path, "com.apple.FinderInfo", out, 32, 0, 0) == 32;
#else
    (void)path;
    (void)out;
    return false;
#endif
}

static void native_forks(const char *path, mac_file *out)
{
    out->has_finder_info = appledouble_native_finder_info(path, out->finder_info);
#ifdef __APPLE__
    ssize_t n = getxattr(path, "com.apple.ResourceFork", NULL, 0, 0, 0);
    if (n > 0 && n < (64 << 20)) {
        uint8_t *r = malloc((size_t)n);
        if (r && getxattr(path, "com.apple.ResourceFork", r, (size_t)n, 0, 0) == n) {
            out->rsrc = r;
            out->rsrc_len = (size_t)n;
        } else {
            free(r);
        }
    }
#endif
}

bool appledouble_has_native(const char *path)
{
    uint8_t fi[32];
    if (appledouble_native_finder_info(path, fi) && memcmp(fi, "\0\0\0\0", 4))
        return true;
#ifdef __APPLE__
    return getxattr(path, "com.apple.ResourceFork", NULL, 0, 0, 0) > 0;
#else
    return false;
#endif
}

bool appledouble_read_meta(const char *path, mac_file *out)
{
    memset(out, 0, sizeof *out);
    char *side = appledouble_sidecar_path(path);
    uint8_t *sbuf;
    size_t slen;
    bool have_side = side && read_file(side, &sbuf, &slen);
    free(side);
    bool ok = true;
    if (have_side) {
        char err[64];
        ok = appledouble_decode(sbuf, slen, out, err, sizeof err);
        free(sbuf);
    } else {
        native_forks(path, out);
    }
    out->data = malloc(1);
    out->data_len = 0;
    if (!out->rsrc) {
        out->rsrc = malloc(1);
        out->rsrc_len = 0;
    }
    if (!ok)
        mac_file_free(out);
    return ok;
}

bool appledouble_read(const char *path, mac_file *out, char *err, size_t errlen)
{
    memset(out, 0, sizeof *out);
    char *side = appledouble_sidecar_path(path);
    if (!side) {
        snprintf(err, errlen, "out of memory");
        return false;
    }
    uint8_t *buf, *sbuf;
    size_t len, slen;
    bool have_side = read_file(side, &sbuf, &slen);
    free(side);
    if (!read_file(path, &buf, &len)) {
        /* Some tools write only the sidecar when the data fork is empty. */
        if (!have_side || errno != ENOENT) {
            snprintf(err, errlen, "%s: %s", path, strerror(errno));
            if (have_side)
                free(sbuf);
            return false;
        }
        buf = malloc(1);
        len = 0;
    }
    out->data = buf;
    out->data_len = len;
    bool ok = true;
    if (have_side) {
        ok = appledouble_decode(sbuf, slen, out, err, errlen);
        free(sbuf);
    } else {
        native_forks(path, out);
    }
    if (!out->rsrc) {
        out->rsrc = malloc(1);
        out->rsrc_len = 0;
    }
    if (!ok)
        mac_file_free(out);
    return ok;
}

static bool write_atomic(const char *path, const uint8_t *buf, size_t len, char *err, size_t errlen)
{
    size_t n = strlen(path) + 16;
    char *tmp = malloc(n);
    if (!tmp) {
        snprintf(err, errlen, "out of memory");
        return false;
    }
    snprintf(tmp, n, "%s.tmp%d", path, (int)getpid());
    FILE *fp = fopen(tmp, "wb");
    if (!fp) {
        snprintf(err, errlen, "%s: %s", tmp, strerror(errno));
        free(tmp);
        return false;
    }
    bool ok = fwrite(buf, 1, len, fp) == len;
    ok = (fclose(fp) == 0) && ok;
    if (ok && rename(tmp, path) != 0)
        ok = false;
    if (!ok) {
        snprintf(err, errlen, "%s: %s", path, strerror(errno));
        unlink(tmp);
    }
    free(tmp);
    return ok;
}

bool appledouble_write(const char *path, const mac_file *f, char *err, size_t errlen)
{
    if (!write_atomic(path, f->data ? f->data : (const uint8_t *)"", f->data_len, err, errlen))
        return false;
    size_t slen;
    uint8_t *side = appledouble_encode(f, &slen);
    char *spath = appledouble_sidecar_path(path);
    bool ok = side && spath && write_atomic(spath, side, slen, err, errlen);
    if ((!side || !spath) && err[0] == 0)
        snprintf(err, errlen, "out of memory");
    free(side);
    free(spath);
    return ok;
}

/* ---- A file's summary, without its forks ---- */

/* Where a resource fork's bytes come from: a span of the sidecar (whose
 * first bytes are read once: the entry table, the Finder info and often
 * the fork's header), or the file's own resource fork (an extended
 * attribute macOS reads at an offset). */
typedef struct {
    int fd;           /* the sidecar, or -1 for the file's own fork */
    const char *path; /* the file, for its own fork */
    uint64_t base;    /* where the fork starts in the sidecar */
    const uint8_t *head;
    size_t head_len;
} fork_reader;

static bool side_read(const fork_reader *r, uint64_t at, void *buf, size_t n)
{
    if (at + n <= r->head_len) {
        memcpy(buf, r->head + at, n);
        return true;
    }
    return pread(r->fd, buf, n, (off_t)at) == (ssize_t)n;
}

static bool fork_read(const fork_reader *r, uint64_t off, void *buf, size_t n)
{
    if (n == 0)
        return true;
    if (r->fd >= 0)
        return side_read(r, r->base + off, buf, n);
#ifdef __APPLE__
    if (off > UINT32_MAX)
        return false;
    return getxattr(r->path, "com.apple.ResourceFork", buf, n, (uint32_t)off, 0) == (ssize_t)n;
#else
    return false;
#endif
}

/* The types in a resource fork's map, checked as rsrc_parse checks it
 * (all but each resource's length, which is in the data). */
static bool fork_types(const fork_reader *r, uint64_t len, uint32_t **types, size_t *ntypes)
{
    *types = NULL;
    *ntypes = 0;
    if (len == 0)
        return true;
    uint8_t h[16];
    if (len < 16 || !fork_read(r, 0, h, 16))
        return false;
    uint32_t data_start = be32(h), map_start = be32(h + 4), data_len = be32(h + 8), map_len = be32(h + 12);
    if ((uint64_t)data_start + data_len > len || (uint64_t)map_start + map_len > len || map_len < 30)
        return false;
    uint8_t *m = malloc(map_len);
    if (!m || !fork_read(r, map_start, m, map_len)) {
        free(m);
        return false;
    }
    bool ok = false;
    uint16_t tlo = be16(m + 24);
    if ((size_t)tlo + 2 > map_len)
        goto out;
    unsigned n = be16(m + tlo) == 0xFFFF ? 0 : (unsigned)be16(m + tlo) + 1;
    uint32_t *t = malloc((n ? n : 1) * sizeof *t);
    if (!t)
        goto out;
    for (unsigned i = 0; i < n; i++) {
        size_t te = (size_t)tlo + 2 + i * 8;
        if (te + 8 > map_len) {
            free(t);
            goto out;
        }
        unsigned count = (unsigned)be16(m + te + 4) + 1;
        size_t rl = (size_t)tlo + be16(m + te + 6);
        for (unsigned j = 0; j < count; j++) {
            size_t re = rl + j * 12;
            if (re + 12 > map_len || (uint64_t)(be32(m + re + 4) & 0x00FFFFFF) + 4 > data_len) {
                free(t);
                goto out;
            }
        }
        t[i] = be32(m + te);
    }
    *types = t;
    *ntypes = n;
    ok = true;
out:
    free(m);
    return ok;
}

void mac_file_summary_free(mac_file_summary *s)
{
    free(s->types);
    s->types = NULL;
    s->ntypes = 0;
}

/* The file's own Finder info and resource fork. */
static void native_summary(const char *path, mac_file_summary *out)
{
    out->rsrc_ok = true;
#ifdef __APPLE__
    /* Most files have neither: one call to find that out. */
    char names[4096];
    ssize_t n = listxattr(path, names, sizeof names, 0);
    bool finder = n < 0, fork = n < 0; /* too many to list: ask for each */
    for (ssize_t i = 0; i < n; i += (ssize_t)strlen(names + i) + 1) {
        finder = finder || !strcmp(names + i, "com.apple.FinderInfo");
        fork = fork || !strcmp(names + i, "com.apple.ResourceFork");
    }
    if (finder)
        out->has_finder_info = appledouble_native_finder_info(path, out->finder_info);
    ssize_t len = fork ? getxattr(path, "com.apple.ResourceFork", NULL, 0, 0, 0) : 0;
    if (len > 0 && len < (64 << 20)) {
        fork_reader r = { .fd = -1, .path = path };
        out->rsrc_len = (size_t)len;
        out->rsrc_ok = fork_types(&r, (uint64_t)len, &out->types, &out->ntypes);
    }
#else
    (void)path;
#endif
}

bool appledouble_read_summary(const char *path, bool sidecar, mac_file_summary *out)
{
    memset(out, 0, sizeof *out);
    int fd = -1;
    if (sidecar) {
        char *side = appledouble_sidecar_path(path);
        if (!side)
            return false;
        fd = open(side, O_RDONLY);
        free(side);
    }
    struct stat st;
    if (fd >= 0 && (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))) {
        close(fd);
        fd = -1;
    }
    if (fd < 0) {
        native_summary(path, out);
        return true;
    }
    /* The sidecar's entry table, checked as appledouble_decode checks it. */
    uint64_t len = (uint64_t)st.st_size;
    uint8_t head[4096];
    ssize_t got = pread(fd, head, len < sizeof head ? (size_t)len : sizeof head, 0);
    fork_reader r = { .fd = fd, .head = head, .head_len = got > 0 ? (size_t)got : 0 };
    bool ok = r.head_len >= 26 && be32(head) == AD_MAGIC && (be32(head + 4) == AD_VERSION || be32(head + 4) == 0x00010000);
    unsigned n = ok ? be16(head + 24) : 0;
    ok = ok && 26 + (uint64_t)n * 12 <= len;
    uint64_t rsrc_off = 0, rsrc_len = 0;
    for (unsigned i = 0; ok && i < n; i++) {
        uint8_t e[12];
        if (!side_read(&r, 26 + (uint64_t)i * 12, e, 12)) {
            ok = false;
            break;
        }
        uint32_t id = be32(e), off = be32(e + 4), elen = be32(e + 8);
        if ((uint64_t)off + elen > len) {
            ok = false;
        } else if (id == AD_RSRC) {
            rsrc_off = off;
            rsrc_len = elen;
        } else if (id == AD_FINDER) {
            memset(out->finder_info, 0, 32);
            ok = side_read(&r, off, out->finder_info, elen < 32 ? elen : 32);
            out->has_finder_info = true;
        }
    }
    if (ok) {
        r.base = rsrc_off;
        out->rsrc_len = (size_t)rsrc_len;
        out->rsrc_ok = fork_types(&r, rsrc_len, &out->types, &out->ntypes);
    }
    close(fd);
    return ok;
}
