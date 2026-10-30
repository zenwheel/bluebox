/*
 * Disks on drives a program's driver serves: a disk image's driver (Disk
 * Copy's, a self-mounting image's) adds a drive to the drive queue and
 * asks the File Manager to mount the disk in it, or posts a disk-inserted
 * event for it.  As the real File Manager does, the volume's blocks are
 * read through the driver (Read calls to its reference number with the
 * drive's number); ForkStore's HFS reader takes the master directory block,
 * the extents and catalog B*-trees and the files from them, and the files
 * are copied out to a folder that is mounted as the volume (the File
 * Manager works on host folders).  The folder lives beside Macintosh HD,
 * under Mounted Disks, and goes when the disk is put away.
 *
 * A disk that can be written (HFS, not write-protected or locked) is
 * written back through the driver when it is flushed, put away or ejected,
 * and when the program ends: the folder is built into an HFS volume of the
 * same size and allocation blocks again (ForkStore's hfs_build), and the
 * blocks that changed are written.  HFS Plus and MFS disks mount locked.
 *
 * And the Disk Initialization Package, which answers for a disk that
 * couldn't be mounted, and formats and initializes disks through their
 * drivers.
 */
#include <dirent.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "diskimage.h"
#include "filemgr_internal.h"
#include "forks.h"
#include "tb_internal.h"

enum {
    controlErr = -17, wPrErr = -44, nsDrvErr = -56, noMacDskErr = -57, badMDBErr = -60,
    offLinErr = -65, noDriveErr = -64, volOnLinErr = -55, dskFulErr = -34,
};
enum { fsFromStart = 1 };
enum { verifyCode = 5, formatCode = 6, ejectCode = 7 };
enum { SECTOR = 512, CHUNK = 64 * 1024 };

uint32_t mac_now(void);

typedef struct {
    int16_t vref, drive, dref;
    uint8_t *image; /* the disk as last read or written */
    size_t len;
    hfs_volinfo info;
    bool locked;
    uint8_t name[28];
    uint64_t stamp; /* the folder's state when the image matched it */
    char root[PATH_MAX];
} disk;

#define MAX_DISKS 16
static disk disks[MAX_DISKS];
static int ndisks;

static disk *disk_of_vref(int16_t vref)
{
    for (int i = 0; i < ndisks; i++)
        if (disks[i].vref == vref)
            return &disks[i];
    return NULL;
}

static gaddr drive_element(int16_t drive)
{
    for (gaddr e = rd32(LM_DrvQHdr + 2); e; e = rd32(e))
        if ((int16_t)rd16(e + DrvQEl_dQDrive_off) == drive)
            return e;
    return 0;
}

/* ---- The drive's blocks, through its driver ---- */

static int16_t drive_io(int16_t drive, int16_t dref, bool write, uint32_t off, uint32_t len, uint8_t *host)
{
    uint32_t chunk = len < CHUNK ? len : CHUNK;
    gaddr buf = tb_NewPtr((int32_t)chunk, true, false);
    gaddr pb = tb_NewPtr(IOParam_SIZEOF, true, true);
    if (!buf || !pb) {
        if (buf)
            tb_DisposePtr(buf);
        if (pb)
            tb_DisposePtr(pb);
        return memFullErr;
    }
    int16_t err = noErr;
    for (uint32_t done = 0; done < len && !err;) {
        uint32_t n = len - done < chunk ? len - done : chunk;
        gmemset(pb, 0, IOParam_SIZEOF);
        wr16(pb + IOParam_ioRefNum_off, (uint16_t)dref);
        wr16(pb + IOParam_ioVRefNum_off, (uint16_t)drive);
        wr32(pb + IOParam_ioBuffer_off, buf);
        wr32(pb + IOParam_ioReqCount_off, n);
        wr16(pb + IOParam_ioPosMode_off, fsFromStart);
        wr32(pb + IOParam_ioPosOffset_off, off + done);
        if (write)
            gput(buf, host + done, n);
        err = driver_call(pb, false, write ? DRIVER_WRITE : DRIVER_READ);
        if (!err && rd32(pb + IOParam_ioActCount_off) < n)
            err = ioErr;
        if (!err && !write)
            gget(host + done, buf, n);
        done += n;
    }
    tb_DisposePtr(buf);
    tb_DisposePtr(pb);
    return err;
}

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* The volume's length, from its master directory block (or HFS Plus
 * volume header) in sector 2; 0 if it isn't a Macintosh volume. */
static uint64_t volume_length(const uint8_t *mdb)
{
    switch (be16(mdb)) {
    case 0x4244: /* HFS (or an HFS Plus volume's wrapper): to the alternate MDB and the sector after */
        return (uint64_t)be16(mdb + 28) * SECTOR + (uint64_t)be16(mdb + 18) * be32(mdb + 20) + 2 * SECTOR;
    case 0xD2D7: /* MFS */
        return (uint64_t)be16(mdb + 28) * SECTOR + (uint64_t)be16(mdb + 18) * be32(mdb + 20);
    case 0x482B:
    case 0x4858: /* HFS Plus, HFSX */
        return (uint64_t)be32(mdb + 44) * be32(mdb + 40);
    default:
        return 0;
    }
}

/* Read the disk: for HFS, only the allocation blocks its bitmap has in
 * use (the rest is never looked at), else all of it. */
static int16_t read_disk(int16_t drive, int16_t dref, uint8_t *img, size_t len)
{
    const uint8_t *mdb = img + 1024;
    if (be16(mdb) != 0x4244 || be16(mdb + 124) == 0x482B)
        return drive_io(drive, dref, false, 4 * SECTOR, (uint32_t)(len - 4 * SECTOR), img + 4 * SECTOR);
    uint32_t nb = be16(mdb + 18), bs = be32(mdb + 20), alst = be16(mdb + 28), vbm = be16(mdb + 14);
    if (!bs || bs % SECTOR || (uint64_t)alst * SECTOR > len)
        return badMDBErr;
    /* The sectors before the allocation blocks: the bitmap among them. */
    int16_t err = alst > 4 ? drive_io(drive, dref, false, 4 * SECTOR, (alst - 4) * SECTOR, img + 4 * SECTOR) : noErr;
    if (err)
        return err;
    size_t base = (size_t)alst * SECTOR;
    for (uint32_t b = 0; b < nb && !err;) {
        size_t bit = (size_t)vbm * SECTOR * 8 + b;
        if (!(img[bit / 8] & (0x80 >> (bit % 8)))) {
            b++;
            continue;
        }
        uint32_t run = 1;
        while (b + run < nb && run < 1024) {
            size_t k = (size_t)vbm * SECTOR * 8 + b + run;
            if (!(img[k / 8] & (0x80 >> (k % 8))))
                break;
            run++;
        }
        size_t off = base + (size_t)b * bs, n = (size_t)run * bs;
        if (off + n > len)
            n = off < len ? len - off : 0;
        if (n)
            err = drive_io(drive, dref, false, (uint32_t)off, (uint32_t)n, img + off);
        b += run;
    }
    /* The alternate master directory block. */
    if (!err && len >= 2 * SECTOR)
        drive_io(drive, dref, false, (uint32_t)(len - 2 * SECTOR), SECTOR, img + len - 2 * SECTOR);
    return err;
}

/* ---- The folders disks are copied out to ---- */

static void cache_folder(char *out, size_t outsize)
{
    char hd[PATH_MAX];
    snprintf(hd, sizeof hd, "%s", fm_volume_root());
    char *slash = strrchr(hd, '/');
    if (slash)
        *slash = 0;
    snprintf(out, outsize, "%s/Mounted Disks", hd);
}

static void remove_tree(const char *path)
{
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            char sub[PATH_MAX];
            snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
            struct stat st;
            if (lstat(sub, &st) == 0 && S_ISDIR(st.st_mode))
                remove_tree(sub);
            else
                unlink(sub);
        }
        closedir(d);
    }
    rmdir(path);
}

/* Folders left by runs that ended without putting their disks away. */
static void remove_stale(void)
{
    char dir[PATH_MAX];
    cache_folder(dir, sizeof dir);
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        long pid = strtol(e->d_name, NULL, 10);
        if (pid > 0 && pid != (long)getpid() && kill((pid_t)pid, 0) != 0) {
            char sub[PATH_MAX];
            snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
            remove_tree(sub);
        }
    }
    closedir(d);
}

static void remove_all(void)
{
    for (int i = 0; i < ndisks; i++)
        remove_tree(disks[i].root);
}

/* The folder's state: every entry's path, size and modification time. */
static void stamp_tree(const char *dir, uint64_t *h)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char sub[PATH_MAX];
        snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(sub, &st) != 0)
            continue;
        uint64_t v[3] = { (uint64_t)st.st_size, (uint64_t)st.st_mtime, (uint64_t)st.st_mode };
#ifdef __APPLE__
        v[1] = v[1] * 1000000000u + (uint64_t)st.st_mtimespec.tv_nsec;
#endif
        /* Order doesn't matter: each entry's hash is added. */
        uint64_t x = 14695981039346656037ull;
        for (const char *p = sub; *p; p++)
            x = (x ^ (uint8_t)*p) * 1099511628211ull;
        for (int i = 0; i < 3; i++)
            x = (x ^ v[i]) * 1099511628211ull;
        *h += x;
        if (S_ISDIR(st.st_mode))
            stamp_tree(sub, h);
    }
    closedir(d);
}

static uint64_t tree_stamp(const char *root)
{
    uint64_t h = 0;
    stamp_tree(root, &h);
    return h;
}

/* A new launch: the last one's disks are gone with its volumes. */
void drivevol_reset(void)
{
    for (int i = 0; i < ndisks; i++) {
        remove_tree(disks[i].root);
        free(disks[i].image);
    }
    ndisks = 0;
}

/* ---- Mounting ---- */

static void note_skipped(void *ctx, const char *message)
{
    LOGF(LOG_TRAPS, "  MountVol drive %d: %s\n", *(int16_t *)ctx, message);
}

int16_t drivevol_mount(int16_t drive, gaddr pb)
{
    gaddr e = drive_element(drive);
    if (!e)
        return nsDrvErr;
    int16_t dref = (int16_t)rd16(e + DrvQEl_dQRefNum_off);
    if (ndisks >= MAX_DISKS)
        return memFullErr;
    uint8_t head[4 * SECTOR];
    int16_t err = drive_io(drive, dref, false, 0, sizeof head, head);
    LOGF(LOG_TRAPS, "  MountVol drive %d (driver %d): master directory block %s\n", drive, dref,
         err ? "unreadable" : "read");
    if (err)
        return err == memFullErr ? err : err < 0 ? err : ioErr;
    uint64_t len = volume_length(head + 1024);
    if (!len)
        return noMacDskErr;
    /* No more than the drive holds, when it says. */
    uint32_t drive_blocks = rd16(e + DrvQEl_dQDrvSz_off);
    if (rd16(e + DrvQEl_qType_off) == 1)
        drive_blocks |= (uint32_t)rd16(e + DrvQEl_dQDrvSz2_off) << 16;
    if (drive_blocks >= 4 && (uint64_t)drive_blocks * SECTOR < len)
        len = (uint64_t)drive_blocks * SECTOR;
    if (len < sizeof head || len > 0x7FFFFE00u)
        return badMDBErr;
    uint8_t *img = calloc(1, (size_t)len);
    if (!img)
        return memFullErr;
    memcpy(img, head, sizeof head);
    err = read_disk(drive, dref, img, (size_t)len);
    if (err) {
        free(img);
        return err;
    }
    char why[256];
    hfs_volume *v = hfs_open(img, (size_t)len, why, sizeof why);
    if (!v) {
        LOGF(LOG_TRAPS, "  MountVol drive %d: %s\n", drive, why);
        free(img);
        return badMDBErr;
    }
    /* The folder its files go to. */
    static int serial;
    if (!serial)
        remove_stale();
    disk *d = &disks[ndisks];
    memset(d, 0, sizeof *d);
    char dir[PATH_MAX];
    cache_folder(dir, sizeof dir);
    mkdir(dir, 0755);
    snprintf(d->root, sizeof d->root, "%s/%ld-%d", dir, (long)getpid(), ++serial);
    remove_tree(d->root);
    mkdir(d->root, 0755);
    bool ok = hfs_extract(v, d->root, NULL, note_skipped, &drive, why, sizeof why);
    d->info = *hfs_volume_info(v);
    const uint8_t *vn = hfs_volume_name(v);
    memcpy(d->name, vn, (size_t)(vn[0] > 27 ? 27 : vn[0]) + 1);
    if (d->name[0] > 27)
        d->name[0] = 27;
    hfs_close(v);
    if (!ok) {
        LOGF(LOG_TRAPS, "  MountVol drive %d: %s\n", drive, why);
        remove_tree(d->root);
        free(img);
        return badMDBErr;
    }
    static bool cleanup;
    if (!cleanup) {
        atexit(remove_all);
        cleanup = true;
    }
    d->image = img;
    d->len = (size_t)len;
    d->drive = drive;
    d->dref = dref;
    /* Changes are kept only where they can be written back: an HFS disk,
     * neither write-protected (the drive's first flag byte) nor locked. */
    d->locked = (rd8(e - 4) & 0x80) || (d->info.attributes & 0x8080) || d->info.signature != 0x4244;
    d->stamp = tree_stamp(d->root);
    if (g_log_mask & LOG_TRAPS) {
        DIR *top = opendir(d->root);
        struct dirent *de;
        while (top && (de = readdir(top)))
            if (de->d_name[0] != '.')
                LOGF(LOG_TRAPS, "  MountVol drive %d: %s\n", drive, de->d_name);
        if (top)
            closedir(top);
    }
    fm_disk_info fi = {
        .signature = d->info.signature, .attributes = d->info.attributes, .create_date = d->info.create_date,
        .modify_date = d->info.modify_date, .backup_date = d->info.backup_date, .block_size = d->info.block_size,
        .total_blocks = d->info.total_blocks, .free_blocks = d->info.free_blocks, .first_block = d->info.first_block,
        .clump_size = d->info.clump_size, .write_count = d->info.write_count, .locked = d->locked,
    };
    memcpy(fi.finder_info, d->info.finder_info, 32);
    int16_t vref;
    err = fm_mount_disk(d->root, d->name, drive, dref, &fi, &vref);
    if (err) {
        remove_tree(d->root);
        free(img);
        return err;
    }
    d->vref = vref;
    ndisks++;
    wr8(e - 3, rd8(e - 3) ? rd8(e - 3) : 1); /* a disk in place */
    if (pb)
        wr16(pb + IOParam_ioVRefNum_off, (uint16_t)vref);
    return noErr;
}

void drivevol_forget(int16_t vref)
{
    disk *d = disk_of_vref(vref);
    if (!d)
        return;
    remove_tree(d->root);
    free(d->image);
    int i = (int)(d - disks);
    memmove(&disks[i], &disks[i + 1], (size_t)(ndisks - i - 1) * sizeof *disks);
    ndisks--;
}

int16_t drivevol_eject(int16_t drive)
{
    gaddr e = drive_element(drive);
    if (!e)
        return nsDrvErr;
    gaddr pb = tb_NewPtr(CntrlParam_SIZEOF, true, true);
    if (!pb)
        return memFullErr;
    wr16(pb + IOParam_ioRefNum_off, rd16(e + DrvQEl_dQRefNum_off));
    wr16(pb + IOParam_ioVRefNum_off, (uint16_t)drive);
    wr16(pb + CntrlParam_csCode_off, ejectCode);
    int16_t r = driver_call(pb, false, DRIVER_CONTROL);
    tb_DisposePtr(pb);
    LOGF(LOG_TRAPS, "  drive %d ejected: %d\n", drive, r);
    return r;
}

/* ---- Writing back ---- */

static int catalog_compare(const uint8_t *a, const uint8_t *b)
{
    int n = a[0] < b[0] ? a[0] : b[0];
    for (int i = 1; i <= n; i++) {
        int x = mac_char_key(a[i], false, false), y = mac_char_key(b[i], false, false);
        if (x != y)
            return x - y;
    }
    return a[0] - b[0];
}

typedef struct {
    hfs_build_entry *list;
    mac_file *files;
    size_t n, cap;
    uint32_t next_id;
    bool oom;
} walk_ctx;

static hfs_build_entry *add_entry(walk_ctx *w)
{
    if (w->n == w->cap) {
        size_t cap = w->cap ? w->cap * 2 : 64;
        hfs_build_entry *nl = realloc(w->list, cap * sizeof *nl);
        mac_file *nf = nl ? realloc(w->files, cap * sizeof *nf) : NULL;
        if (nl)
            w->list = nl;
        if (!nl || !nf) {
            w->oom = true;
            return NULL;
        }
        w->files = nf;
        w->cap = cap;
    }
    memset(&w->files[w->n], 0, sizeof w->files[w->n]);
    hfs_build_entry *e = &w->list[w->n++];
    memset(e, 0, sizeof *e);
    return e;
}

/* A folder's entries, as the File Manager lists them: a "._" sidecar is
 * its file (alone, a file with only a resource fork), other dot files
 * are the host's. */
static void walk(walk_ctx *w, const char *dir, uint32_t parent, int depth)
{
    DIR *d = opendir(dir);
    if (!d || depth > 100) {
        if (d)
            closedir(d);
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) && !w->oom) {
        const char *name = de->d_name;
        char path[PATH_MAX];
        struct stat st;
        if (name[0] == '.' && name[1] == '_' && name[2]) {
            snprintf(path, sizeof path, "%s/%s", dir, name + 2);
            if (lstat(path, &st) == 0)
                continue;
        } else if (name[0] == '.') {
            continue;
        } else {
            snprintf(path, sizeof path, "%s/%s", dir, name);
        }
        bool is_dir = stat(path, &st) == 0 && S_ISDIR(st.st_mode);
        size_t at = w->n;
        hfs_build_entry *e = add_entry(w);
        if (!e)
            break;
        e->cnid = w->next_id++;
        e->parent = parent;
        e->is_dir = is_dir;
        fm_mac_name(path, e->name);
        uint32_t when = mac_now();
        e->create_date = e->modify_date = when;
        if (is_dir) {
            walk(w, path, e->cnid, depth + 1);
            continue;
        }
        mac_file *f = &w->files[at];
        char ferr[128];
        if (!appledouble_read(path, f, ferr, sizeof ferr) && !appledouble_read_meta(path, f))
            continue;
        e = &w->list[at]; /* the list may have moved */
        memcpy(e->finder_info, f->finder_info, 32);
        if (f->create_date)
            e->create_date = f->create_date;
        if (f->modify_date)
            e->modify_date = f->modify_date;
        e->data = f->data;
        e->data_len = (uint32_t)f->data_len;
        e->rsrc = f->rsrc;
        e->rsrc_len = (uint32_t)f->rsrc_len;
    }
    closedir(d);
}

static void walk_free(walk_ctx *w)
{
    for (size_t i = 0; i < w->n; i++)
        mac_file_free(&w->files[i]);
    free(w->files);
    free(w->list);
}

int16_t drivevol_write_back(int16_t vref)
{
    disk *d = disk_of_vref(vref);
    if (!d || d->locked)
        return noErr;
    uint64_t stamp = tree_stamp(d->root);
    if (stamp == d->stamp)
        return noErr;
    walk_ctx w = { .next_id = 16 };
    walk(&w, d->root, 2, 0);
    if (w.oom) {
        walk_free(&w);
        return memFullErr;
    }
    hfs_build_params p = {
        .size = (uint32_t)d->len, .block_size = d->info.block_size, .create_date = d->info.create_date,
        .modify_date = mac_now(), .backup_date = d->info.backup_date, .write_count = d->info.write_count + 1,
        .attributes = d->info.attributes, .compare = catalog_compare,
    };
    memcpy(p.name, d->name, (size_t)d->name[0] + 1);
    memcpy(p.finder_info, d->info.finder_info, 32);
    uint8_t *img;
    char why[256];
    bool ok = hfs_build(&p, w.list, w.n, &img, why, sizeof why);
    walk_free(&w);
    if (!ok) {
        LOGF(LOG_TRAPS, "  volume %d not written back: %s\n", vref, why);
        return dskFulErr;
    }
    /* The sectors that changed, in runs. */
    int16_t err = noErr;
    size_t sectors = d->len / SECTOR, written = 0;
    for (size_t s = 0; s < sectors && !err;) {
        if (!memcmp(img + s * SECTOR, d->image + s * SECTOR, SECTOR)) {
            s++;
            continue;
        }
        size_t run = 1;
        while (s + run < sectors && run < CHUNK / SECTOR &&
               memcmp(img + (s + run) * SECTOR, d->image + (s + run) * SECTOR, SECTOR))
            run++;
        err = drive_io(d->drive, d->dref, true, (uint32_t)(s * SECTOR), (uint32_t)(run * SECTOR), img + s * SECTOR);
        written += run;
        s += run;
    }
    LOGF(LOG_TRAPS, "  volume %d written back: %zu sectors, %d\n", vref, written, err);
    if (err) {
        free(img);
        return err;
    }
    free(d->image);
    d->image = img;
    d->info.write_count++;
    d->stamp = stamp;
    return noErr;
}

/* The free blocks of a disk that can be written: its blocks less those its
 * files and the catalog's room take, as written back they would. */
uint32_t drivevol_free_blocks(int16_t vref)
{
    disk *d = disk_of_vref(vref);
    if (!d)
        return 0;
    if (d->locked)
        return d->info.free_blocks;
    walk_ctx w = { .next_id = 16 };
    walk(&w, d->root, 2, 0);
    uint64_t bs = d->info.block_size ? d->info.block_size : SECTOR, used = 0;
    for (size_t i = 0; i < w.n; i++)
        used += (w.list[i].data_len + bs - 1) / bs + (w.list[i].rsrc_len + bs - 1) / bs;
    /* The extents and catalog files: as hfs_build makes them, about a
     * 64th of the disk, more for many files. */
    uint64_t meta = (d->len / 64 + (uint64_t)w.n * 256 + bs - 1) / bs;
    walk_free(&w);
    used += meta;
    return used >= d->info.total_blocks ? 0 : (uint32_t)(d->info.total_blocks - used);
}

/* A disk-inserted event's drive mounted, as GetNextEvent does before a
 * program sees the event: the result for the event's high word. */
int16_t drivevol_disk_inserted(int16_t drive)
{
    gaddr pb = tb_NewPtr(IOParam_SIZEOF, true, true);
    if (!pb)
        return memFullErr;
    wr16(pb + IOParam_ioVRefNum_off, (uint16_t)drive);
    int16_t r = tb_PBMountVol(pb);
    tb_DisposePtr(pb);
    LOGF(LOG_TRAPS, "  disk inserted in drive %d: mounted %d\n", drive, r);
    return r;
}

/* ---- The Disk Initialization Package (Pack2) ---- */

void tb_DILoad(void) {}
void tb_DIUnload(void) {}

static int16_t drive_control(int16_t drive, int16_t code)
{
    gaddr e = drive_element(drive);
    if (!e)
        return noDriveErr;
    gaddr pb = tb_NewPtr(CntrlParam_SIZEOF, true, true);
    if (!pb)
        return memFullErr;
    wr16(pb + IOParam_ioRefNum_off, rd16(e + DrvQEl_dQRefNum_off));
    wr16(pb + IOParam_ioVRefNum_off, (uint16_t)drive);
    wr16(pb + CntrlParam_csCode_off, (uint16_t)code);
    int16_t r = driver_call(pb, false, DRIVER_CONTROL);
    tb_DisposePtr(pb);
    return r;
}

/* DIBadMount: a disk that couldn't be mounted (the event's high word the
 * error, its low word the drive).  The dialog's answer is always Eject: no
 * disk is initialized without a person asking, and there is no one to ask
 * here.  1 says it was ejected. */
int16_t tb_DIBadMount(uint32_t pt, int32_t evtmess)
{
    int16_t err = (int16_t)(evtmess >> 16), drive = (int16_t)(evtmess & 0xFFFF);
    if (err == noErr || err == volOnLinErr)
        return err;
    if (!drive_element(drive))
        return nsDrvErr;
    LOGF(LOG_TRAPS, "  DIBadMount drive %d (error %d): ejected\n", drive, err);
    drivevol_eject(drive);
    return 1;
}

int16_t tb_DIFormat(int16_t dn) { return drive_control(dn, formatCode); }
int16_t tb_DIVerify(int16_t dn) { return drive_control(dn, verifyCode); }

/* The drive's size in bytes, as its queue element says. */
static uint32_t drive_bytes(gaddr e)
{
    uint32_t blocks = rd16(e + DrvQEl_dQDrvSz_off);
    if (rd16(e + DrvQEl_qType_off) == 1)
        blocks |= (uint32_t)rd16(e + DrvQEl_dQDrvSz2_off) << 16;
    return blocks > 0x7FFFFE00u / SECTOR ? 0x7FFFFE00u : blocks * SECTOR;
}

/* DIZero: an empty HFS volume written on the disk in a drive, named
 * vname; it is left unmounted (MountVol mounts it).  The volume uses all
 * of the drive, or the size asked (DIXZero). */
static int16_t zero(int16_t drive, gaddr vname, uint32_t size)
{
    gaddr e = drive_element(drive);
    if (!e)
        return noDriveErr;
    for (int i = 0; i < ndisks; i++)
        if (disks[i].drive == drive)
            return volOnLinErr;
    if (!size)
        size = drive_bytes(e);
    if (size < 64 * SECTOR)
        return paramErr;
    if (rd8(e - 4) & 0x80)
        return wPrErr;
    hfs_build_params p = { .size = size, .create_date = mac_now(), .modify_date = mac_now() };
    uint8_t n = vname ? rd8(vname) : 0;
    if (n > 27)
        n = 27;
    if (n) {
        p.name[0] = n;
        gget(p.name + 1, vname + 1, n);
    } else {
        memcpy(p.name, "\x08Untitled", 9);
    }
    uint8_t *img;
    char why[128];
    if (!hfs_build(&p, NULL, 0, &img, why, sizeof why))
        return paramErr;
    /* What isn't zero: up to the end of the catalog, and the alternate
     * master directory block. */
    size_t head = (size_t)be16(img + 1024 + 28) * SECTOR +
                  ((size_t)be16(img + 1024 + 150) + be16(img + 1024 + 152)) * be32(img + 1024 + 20);
    int16_t err = drive_io(drive, (int16_t)rd16(e + DrvQEl_dQRefNum_off), true, 0, (uint32_t)head, img);
    if (!err)
        err = drive_io(drive, (int16_t)rd16(e + DrvQEl_dQRefNum_off), true, size - 2 * SECTOR, 2 * SECTOR,
                       img + size - 2 * SECTOR);
    free(img);
    LOGF(LOG_TRAPS, "  DIZero drive %d: %d\n", drive, err);
    return err;
}

int16_t tb_DIZero(int16_t dn, gaddr vname) { return zero(dn, vname, 0); }

int16_t tb_DIXFormat(int16_t drvNum, int8_t fmtFlag, uint32_t fmtArg, gaddr actSize)
{
    int16_t r = drive_control(drvNum, formatCode);
    gaddr e = drive_element(drvNum);
    if (actSize)
        wr32(actSize, r == noErr && e ? drive_bytes(e) / SECTOR : 0);
    return r;
}

/* DIXZero: volTypeSelector 0 (HFS) is the only kind made; a size in
 * 512-byte blocks, or 0 for the whole drive. */
int16_t tb_DIXZero(int16_t drvNum, gaddr volName, int16_t fsid, int16_t mediaStatus, int16_t volTypeSelector,
                   uint32_t volSize, gaddr extendedInfoPtr)
{
    if (fsid != 0 || volTypeSelector != 0)
        return paramErr; /* no other file system's initializer here */
    return zero(drvNum, volName, volSize > 0x7FFFFE00u / SECTOR ? 0 : volSize * SECTOR);
}

/* DIReformat: the disk formatted and initialized again, as the dialog's
 * Initialize would. */
int16_t tb_DIReformat(int16_t drvNum, int16_t fsid, gaddr volName, gaddr msgText)
{
    int16_t r = drive_control(drvNum, formatCode);
    if (r != noErr && r != controlErr)
        return r;
    return zero(drvNum, volName, 0);
}
