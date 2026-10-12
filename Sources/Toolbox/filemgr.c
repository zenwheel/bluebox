/*
 * File Manager over host folders.
 *
 * One volume (vRefNum -1, drive 1) maps to a host directory.  Directory
 * IDs are assigned on first use (the root is 2).  Working directories
 * give old-style vRefNums.  Files are AppleDouble: the data fork is the host
 * file, the resource fork and Finder info live in the "._" sidecar.  An open
 * fork is held in host memory and written back atomically on close/flush.
 *
 * Names are MacRoman inside the Mac, UTF-8 on the host; '/' in a Mac name is
 * ':' on the host (and ':' cannot occur in a Mac name).
 */
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "forks.h"
#include "tb_internal.h"

enum {
    dirFulErr = -33, dskFulErr = -34, nsvErr = -35, ioErrF = -36, bdNamErr = -37, fnOpnErr = -38, eofErr = -39,
    posErr = -40, tmfoErr = -42, fnfErrF = -43, wPrErr = -44, fLckdErr = -45, fBsyErr = -47, dupFNErr = -48,
    opWrErr = -49, rfNumErr = -51, permErr = -54, wrPermErr = -61, dirNFErr = -120, tmwdoErr = -121, badMovErr = -122,
    wrgVolTypErr = -123, controlErr = -17, statusErr = -18, notOpenErr = -28,
};
enum { fsAtMark = 0, fsFromStart, fsFromLEOF, fsFromMark };
enum { fsCurPerm = 0, fsRdPerm, fsWrPerm, fsRdWrPerm, fsRdWrShPerm };

#define VREF -1
#define ROOT_ID 2
#define MAX_DIRS 4096
#define MAX_WDS 64
#define FCB_FIRST 32 /* refnum slots below this belong to the Resource Manager */
#define MAX_FCBS 300

uint32_t mac_now(void);

static char root_path[PATH_MAX];
static char vol_name[32]; /* MacRoman */

/* Directory IDs: index i holds the host path of directory ID i (relative). */
static char *dirs[MAX_DIRS];
static int ndirs;

static struct {
    bool used;
    int32_t dirid;
} wds[MAX_WDS];

static int16_t default_vref = VREF;
static int32_t default_dir = ROOT_ID;

typedef struct {
    bool used;
    bool rsrc;
    bool write;
    bool dirty;
    bool host_text; /* a plain host text file: LF/UTF-8 on disk, CR/MacRoman inside */
    char path[PATH_MAX];
    uint8_t *buf;
    uint32_t len, cap, pos;
    int32_t parid;
    uint8_t name[32];
} fcb;

static fcb fcbs[MAX_FCBS];

static int16_t fcb_refnum(int i) { return (int16_t)(2 + (FCB_FIRST + i) * 94); }
static fcb *fcb_for(int16_t rn)
{
    if (rn < 2 || (rn - 2) % 94)
        return NULL;
    int i = (rn - 2) / 94 - FCB_FIRST;
    return i >= 0 && i < MAX_FCBS && fcbs[i].used ? &fcbs[i] : NULL;
}

/* ---------------------------------------------------------------------- */
/* Names and paths */

static void mac_to_host(const uint8_t *name, int n, char *out, size_t outsize)
{
    uint8_t tmp[64];
    if (n > 63)
        n = 63;
    for (int i = 0; i < n; i++)
        tmp[i] = name[i] == '/' ? ':' : name[i];
    macroman_to_utf8(tmp, (size_t)n, out, outsize);
}

static int host_to_mac(const char *name, uint8_t *out)
{
    char tmp[NAME_MAX + 1];
    snprintf(tmp, sizeof tmp, "%s", name);
    for (char *p = tmp; *p; p++)
        if (*p == ':')
            *p = '/';
    int n = (int)utf8_to_macroman(tmp, out, 31);
    return n;
}

static int32_t dir_id_for(const char *rel)
{
    for (int i = ROOT_ID; i < ndirs; i++)
        if (dirs[i] && !strcmp(dirs[i], rel))
            return i;
    if (ndirs >= MAX_DIRS)
        return 0;
    dirs[ndirs] = strdup(rel);
    return ndirs++;
}

static bool host_of_dir(int32_t id, char *out, size_t outsize)
{
    if (id < ROOT_ID || id >= ndirs || !dirs[id])
        return false;
    if (dirs[id][0])
        snprintf(out, outsize, "%s/%s", root_path, dirs[id]);
    else
        snprintf(out, outsize, "%s", root_path[0] ? root_path : "/");
    return true;
}

/* Relative path (from the volume root) of a host path. */
static const char *rel_of(const char *host)
{
    size_t n = strlen(root_path);
    if (!strncmp(host, root_path, n))
        return host[n] == '/' ? host + n + 1 : host + n;
    return host;
}

static void put_name(gaddr namep, const char *host);
static void put_name_fwd(gaddr namep, const char *host) { put_name(namep, host); }

static int32_t parent_id_of(const char *host)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", rel_of(host));
    char *slash = strrchr(tmp, '/');
    if (!slash)
        return ROOT_ID;
    *slash = 0;
    return dir_id_for(tmp);
}

/* Directory a (vRefNum, dirID) pair names, or 0. */
static int32_t base_dir(int16_t vref, int32_t dirid, bool hfs)
{
    if (hfs && dirid)
        return dirid == 1 ? ROOT_ID : dirid;
    if (vref == 0)
        return default_dir;
    if (vref <= -32000 && vref > -32000 - MAX_WDS) {
        int i = -32000 - vref;
        return wds[i].used ? wds[i].dirid : 0;
    }
    if (vref == VREF || vref == 1)
        return ROOT_ID;
    return 0;
}

/* Resolve a Mac name relative to a directory into a host path.
 * Returns noErr, with *is_dir set if it names a directory. */
static int16_t resolve(int16_t vref, int32_t dirid, gaddr namep, bool hfs, char *out, size_t outsize)
{
    int32_t dir = base_dir(vref, dirid, hfs);
    if (!dir)
        return nsvErr;
    char path[PATH_MAX];
    if (!host_of_dir(dir, path, sizeof path))
        return dirNFErr;
    if (!namep || rd8(namep) == 0) {
        snprintf(out, outsize, "%s", path);
        return noErr;
    }
    uint8_t name[256];
    int n = rd8(namep);
    gget(name, namep + 1, (uint32_t)n);
    int i = 0;
    /* A full path "Volume:..." starts at the root. */
    bool has_colon = memchr(name, ':', (size_t)n) != NULL;
    if (has_colon && name[0] != ':') {
        while (i < n && name[i] != ':')
            i++;
        snprintf(path, sizeof path, "%s", root_path[0] ? root_path : "");
        i++;
    } else if (name[0] == ':') {
        i = 1;
    }
    while (i < n) {
        int start = i;
        while (i < n && name[i] != ':')
            i++;
        int len = i - start;
        if (len == 0) { /* "::" goes up */
            char *slash = strrchr(path, '/');
            if (slash && slash != path)
                *slash = 0;
            else if (slash)
                slash[1] = 0;
        } else {
            char comp[256];
            mac_to_host(name + start, len, comp, sizeof comp);
            size_t pl = strlen(path);
            if (pl && path[pl - 1] == '/')
                pl--;
            snprintf(path + pl, sizeof path - pl, "/%s", comp);
        }
        if (i < n)
            i++;
    }
    snprintf(out, outsize, "%s", path);
    return noErr;
}

static bool is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

static bool hidden_name(const char *n)
{
    return n[0] == '.' || !strcmp(n, "Icon\r");
}

/* The n-th (1-based) visible entry of a directory; dirs and files mixed, sorted. */
static int cmp_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static bool nth_entry(const char *dir, int n, bool files_only, char *out, size_t outsize)
{
    DIR *d = opendir(dir);
    if (!d)
        return false;
    char **names = NULL;
    int count = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (hidden_name(e->d_name))
            continue;
        char full[PATH_MAX];
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        if (files_only && is_dir(full))
            continue;
        if (count == cap) {
            cap = cap ? cap * 2 : 32;
            names = realloc(names, (size_t)cap * sizeof *names);
        }
        names[count++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, (size_t)count, sizeof *names, cmp_names);
    bool ok = n >= 1 && n <= count;
    if (ok)
        snprintf(out, outsize, "%s/%s", dir, names[n - 1]);
    for (int i = 0; i < count; i++)
        free(names[i]);
    free(names);
    return ok;
}

static int count_entries(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)))
        if (!hidden_name(e->d_name))
            count++;
    closedir(d);
    return count;
}

static void put_name(gaddr namep, const char *host)
{
    if (!namep)
        return;
    const char *base = strrchr(host, '/');
    base = base ? base + 1 : host;
    uint8_t mac[32];
    int n = host_to_mac(base, mac);
    wr8(namep, (uint8_t)n);
    gput(namep + 1, mac, (uint32_t)n);
}

/* ---------------------------------------------------------------------- */
/* Setup */

void fm_init(const char *app_dir)
{
    /* The volume is the whole host disk; the app's folder is the default
     * directory, so documents anywhere can be opened. */
    snprintf(root_path, sizeof root_path, "%s", "");
    for (int i = 0; i < ndirs; i++) {
        free(dirs[i]);
        dirs[i] = NULL;
    }
    dirs[ROOT_ID] = strdup("");
    ndirs = ROOT_ID + 1;
    memset(wds, 0, sizeof wds);
    for (int i = 0; i < MAX_FCBS; i++)
        free(fcbs[i].buf);
    memset(fcbs, 0, sizeof fcbs);
    char app[PATH_MAX];
    if (!realpath(app_dir, app))
        snprintf(app, sizeof app, "%s", app_dir);
    default_vref = VREF;
    default_dir = dir_id_for(rel_of(app));
    static const char name[] = "Macintosh HD";
    vol_name[0] = (char)(sizeof name - 1);
    memcpy(vol_name + 1, name, sizeof name - 1);
    wr16(LM_DefVRefNum, (uint16_t)VREF);
    wr16(LM_SFSaveDisk, (uint16_t)(-VREF));
    wr32(LM_CurDirStore, (uint32_t)default_dir);
}

/* An FSSpec for a host path (the file need not exist). */
void fm_spec_for_path(const char *path, gaddr spec)
{
    char full[PATH_MAX];
    if (!realpath(path, full))
        snprintf(full, sizeof full, "%s", path);
    char parent[PATH_MAX];
    snprintf(parent, sizeof parent, "%s", full);
    char *slash = strrchr(parent, '/');
    if (slash)
        *slash = 0;
    wr16(spec + FSSpec_vRefNum_off, (uint16_t)VREF);
    wr32(spec + FSSpec_parID_off, (uint32_t)dir_id_for(rel_of(parent)));
    put_name_fwd(spec + FSSpec_name_off, full);
}

/* Host path for an (vRefNum, dirID, name) triple; used by the Resource Manager. */
bool fm_host_path(int16_t vref, int32_t dirid, gaddr name, bool hfs, char *out, size_t outsize)
{
    return resolve(vref, dirid, name, hfs, out, outsize) == noErr;
}

/* ---------------------------------------------------------------------- */
/* Plain host files */

/* Classic type and creator for a host file that has no sidecar, from its
 * extension.  Returns true for text, which is converted on the way in and out. */
static int implied_type(const char *path, uint8_t finfo[16])
{
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    if (!dot || (slash && dot < slash))
        return 0;
    static const struct {
        const char *ext;
        const char *type, *creator;
        bool text;
    } map[] = {
        { "txt", "TEXT", "ttxt", true }, { "text", "TEXT", "ttxt", true }, { "md", "TEXT", "ttxt", true },
        { "c", "TEXT", "ttxt", true },   { "h", "TEXT", "ttxt", true },    { "s", "TEXT", "ttxt", true },
        { "html", "TEXT", "ttxt", true }, { "csv", "TEXT", "ttxt", true }, { "log", "TEXT", "ttxt", true },
        { "pict", "PICT", "ttxt", false }, { "pct", "PICT", "ttxt", false },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        if (strcasecmp(dot + 1, map[i].ext))
            continue;
        memset(finfo, 0, 16);
        memcpy(finfo, map[i].type, 4);
        memcpy(finfo + 4, map[i].creator, 4);
        return map[i].text ? 2 : 1;
    }
    return 0;
}

/* A sidecar adds nothing to a host text file whose type its extension
 * already gives, with no resource fork and no Finder flags that matter. */
static bool sidecar_redundant(const char *path, const mac_file *mf)
{
    uint8_t implied[16];
    if (implied_type(path, implied) != 2 || mf->rsrc_len)
        return false;
    if (!mf->has_finder_info)
        return true;
    bool untyped = !memcmp(mf->finder_info, "\0\0\0\0", 4);
    uint16_t flags = (uint16_t)(mf->finder_info[8] << 8 | mf->finder_info[9]);
    return (untyped || !memcmp(mf->finder_info, implied, 4)) && !(flags & ~0x0100u);
}

/* Classic type and creator for a host file whose sidecar adds nothing, from
 * its extension.  Returns true for text, which is converted on the way in
 * and out. */
static bool host_file_type(const char *path, uint8_t finfo[16])
{
    char *side = appledouble_sidecar_path(path);
    bool has_side = side && exists(side);
    free(side);
    if (has_side) {
        mac_file mf;
        char err[256];
        if (!appledouble_read(path, &mf, err, sizeof err))
            return false;
        bool plain = sidecar_redundant(path, &mf);
        if (plain && mf.has_finder_info && memcmp(mf.finder_info, "\0\0\0\0", 4))
            memcpy(finfo, mf.finder_info, 16);
        else if (plain)
            implied_type(path, finfo);
        mac_file_free(&mf);
        return plain;
    }
    return implied_type(path, finfo) == 2;
}

/* Host text (UTF-8, LF or CRLF) to classic text (MacRoman, CR). */
static uint8_t *text_in(const uint8_t *b, size_t n, size_t *out_n)
{
    char *z = malloc(n + 1);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (b[i] == '\r' && i + 1 < n && b[i + 1] == '\n')
            continue;
        z[k++] = b[i] == '\n' ? '\r' : (char)b[i];
    }
    z[k] = 0;
    uint8_t *out = malloc(k + 1);
    *out_n = utf8_to_macroman(z, out, k);
    free(z);
    return out;
}

/* Classic text back to host text. */
static uint8_t *text_out(const uint8_t *b, size_t n, size_t *out_n)
{
    char *out = malloc(n * 3 + 1);
    size_t k = macroman_to_utf8(b, n, out, n * 3 + 1);
    for (size_t i = 0; i < k; i++)
        if (out[i] == '\r')
            out[i] = '\n';
    *out_n = k;
    return (uint8_t *)out;
}

/* ---------------------------------------------------------------------- */
/* Forks in memory */

static int16_t load_fork(fcb *f)
{
    mac_file mf;
    char err[256];
    if (!appledouble_read(f->path, &mf, err, sizeof err))
        return fnfErrF;
    uint8_t *src = f->rsrc ? mf.rsrc : mf.data;
    size_t n = f->rsrc ? mf.rsrc_len : mf.data_len;
    uint8_t finfo[16] = { 0 };
    uint8_t *conv = NULL;
    f->host_text = !f->rsrc && host_file_type(f->path, finfo);
    if (f->host_text) {
        conv = text_in(src, n, &n);
        src = conv;
    }
    f->cap = (uint32_t)n + 1024;
    f->buf = malloc(f->cap);
    memcpy(f->buf, src, n);
    f->len = (uint32_t)n;
    free(conv);
    mac_file_free(&mf);
    return noErr;
}

static int16_t save_fork(fcb *f)
{
    if (!f->dirty)
        return noErr;
    mac_file mf;
    char err[256];
    if (!appledouble_read(f->path, &mf, err, sizeof err))
        memset(&mf, 0, sizeof mf);
    if (f->rsrc) {
        free(mf.rsrc);
        mf.rsrc = malloc(f->len ? f->len : 1);
        memcpy(mf.rsrc, f->buf, f->len);
        mf.rsrc_len = f->len;
    } else if (f->host_text) {
        /* Plain host text stays plain: no sidecar, host line endings. */
        size_t n;
        uint8_t *out = text_out(f->buf, f->len, &n);
        mac_file_free(&mf);
        FILE *fp = fopen(f->path, "wb");
        bool ok = fp && fwrite(out, 1, n, fp) == n;
        if (fp)
            ok = (fclose(fp) == 0) && ok;
        free(out);
        f->dirty = false;
        return ok ? noErr : ioErrF;
    } else {
        free(mf.data);
        mf.data = malloc(f->len ? f->len : 1);
        memcpy(mf.data, f->buf, f->len);
        mf.data_len = f->len;
    }
    mf.modify_date = mac_now();
    bool ok = appledouble_write(f->path, &mf, err, sizeof err);
    mac_file_free(&mf);
    f->dirty = false;
    return ok ? noErr : ioErrF;
}


/* ---------------------------------------------------------------------- */
/* Opening and closing */

static bool hfs_call(void) { return (trap_current_word() & 0x0200) != 0; }

static int16_t open_fork(gaddr pb, bool rsrc, bool hfs)
{
    gaddr namep = rd32(pb + IOParam_ioNamePtr_off);
    /* Driver names start with '.': no drivers are installed. */
    if (!rsrc && namep && rd8(namep) && rd8(namep + 1) == '.')
        return fnfErrF;
    char path[PATH_MAX];
    int16_t err = resolve((int16_t)rd16(pb + IOParam_ioVRefNum_off), hfs ? (int32_t)rd32(pb + HFileParam_ioDirID_off) : 0,
                          namep, hfs, path, sizeof path);
    if (err)
        return err;
    if (!exists(path) || is_dir(path))
        return fnfErrF;
    uint8_t perm = rd8(pb + IOParam_ioPermssn_off);
    bool write = perm != fsRdPerm;
    /* Already open for writing?  Return that refnum with opWrErr. */
    for (int i = 0; i < MAX_FCBS; i++) {
        if (fcbs[i].used && fcbs[i].rsrc == rsrc && fcbs[i].write && write && !strcmp(fcbs[i].path, path)) {
            wr16(pb + IOParam_ioRefNum_off, (uint16_t)fcb_refnum(i));
            return opWrErr;
        }
    }
    for (int i = 0; i < MAX_FCBS; i++) {
        if (fcbs[i].used)
            continue;
        fcb *f = &fcbs[i];
        memset(f, 0, sizeof *f);
        f->used = true;
        f->rsrc = rsrc;
        f->write = write;
        snprintf(f->path, sizeof f->path, "%s", path);
        if ((err = load_fork(f))) {
            f->used = false;
            return err;
        }
        f->parid = parent_id_of(path);
        put_name(f->name, path);
        wr16(pb + IOParam_ioRefNum_off, (uint16_t)fcb_refnum(i));
        return noErr;
    }
    return tmfoErr;
}

int16_t tb_PBOpen(gaddr pb, bool async) { return open_fork(pb, false, hfs_call()); }
int16_t tb_PBHOpen(gaddr pb, bool async) { return open_fork(pb, false, true); }
int16_t tb_PBOpenRF(gaddr pb, bool async) { return open_fork(pb, true, hfs_call()); }
int16_t tb_PBHOpenRF(gaddr pb, bool async) { return open_fork(pb, true, true); }
int16_t tb_PBHOpenDF(gaddr pb, bool async) { return open_fork(pb, false, true); }

int16_t tb_PBClose(gaddr pb, bool async)
{
    fcb *f = fcb_for((int16_t)rd16(pb + IOParam_ioRefNum_off));
    if (!f)
        return rfNumErr;
    int16_t err = save_fork(f);
    free(f->buf);
    memset(f, 0, sizeof *f);
    return err;
}

/* ---------------------------------------------------------------------- */
/* Reading and writing */

static int16_t set_mark(fcb *f, int mode, int32_t off)
{
    int64_t pos;
    switch (mode & 3) {
    case fsFromStart: pos = off; break;
    case fsFromLEOF: pos = (int64_t)f->len + off; break;
    case fsFromMark: pos = (int64_t)f->pos + off; break;
    default: pos = f->pos; break;
    }
    if (pos < 0)
        return posErr;
    if (pos > f->len) {
        f->pos = f->len;
        return eofErr;
    }
    f->pos = (uint32_t)pos;
    return noErr;
}

int16_t tb_PBRead(gaddr pb, bool async)
{
    fcb *f = fcb_for((int16_t)rd16(pb + IOParam_ioRefNum_off));
    wr32(pb + IOParam_ioActCount_off, 0);
    if (!f)
        return rfNumErr;
    uint16_t mode = rd16(pb + IOParam_ioPosMode_off);
    int16_t err = set_mark(f, mode, (int32_t)rd32(pb + IOParam_ioPosOffset_off));
    if (err && err != eofErr)
        return err;
    uint32_t want = rd32(pb + IOParam_ioReqCount_off);
    uint32_t avail = f->len - f->pos;
    uint32_t n = want < avail ? want : avail;
    if (mode & 0x80) { /* newline mode: stop after the newline character */
        uint8_t nl = (uint8_t)(mode >> 8);
        for (uint32_t i = 0; i < n; i++)
            if (f->buf[f->pos + i] == nl) {
                n = i + 1;
                want = n;
                break;
            }
    }
    gput(rd32(pb + IOParam_ioBuffer_off), f->buf + f->pos, n);
    f->pos += n;
    wr32(pb + IOParam_ioActCount_off, n);
    wr32(pb + IOParam_ioPosOffset_off, f->pos);
    return n < want ? eofErr : noErr;
}

static bool reserve(fcb *f, uint32_t need)
{
    if (need <= f->cap)
        return true;
    uint32_t cap = f->cap ? f->cap : 1024;
    while (cap < need)
        cap *= 2;
    uint8_t *nb = realloc(f->buf, cap);
    if (!nb)
        return false;
    f->buf = nb;
    f->cap = cap;
    return true;
}

int16_t tb_PBWrite(gaddr pb, bool async)
{
    fcb *f = fcb_for((int16_t)rd16(pb + IOParam_ioRefNum_off));
    wr32(pb + IOParam_ioActCount_off, 0);
    if (!f)
        return rfNumErr;
    if (!f->write)
        return wrPermErr;
    uint16_t mode = rd16(pb + IOParam_ioPosMode_off);
    int16_t err = set_mark(f, mode, (int32_t)rd32(pb + IOParam_ioPosOffset_off));
    if (err && err != eofErr)
        return err;
    uint32_t n = rd32(pb + IOParam_ioReqCount_off);
    if (!reserve(f, f->pos + n))
        return dskFulErr;
    gget(f->buf + f->pos, rd32(pb + IOParam_ioBuffer_off), n);
    f->pos += n;
    if (f->pos > f->len)
        f->len = f->pos;
    f->dirty = true;
    wr32(pb + IOParam_ioActCount_off, n);
    wr32(pb + IOParam_ioPosOffset_off, f->pos);
    return noErr;
}

int16_t tb_PBGetEOF(gaddr pb, bool async)
{
    fcb *f = fcb_for((int16_t)rd16(pb + IOParam_ioRefNum_off));
    if (!f)
        return rfNumErr;
    wr32(pb + IOParam_ioMisc_off, f->len);
    return noErr;
}

int16_t tb_PBSetEOF(gaddr pb, bool async)
{
    fcb *f = fcb_for((int16_t)rd16(pb + IOParam_ioRefNum_off));
    if (!f)
        return rfNumErr;
    if (!f->write)
        return wrPermErr;
    uint32_t n = rd32(pb + IOParam_ioMisc_off);
    if (!reserve(f, n))
        return dskFulErr;
    if (n > f->len)
        memset(f->buf + f->len, 0, n - f->len);
    f->len = n;
    if (f->pos > n)
        f->pos = n;
    f->dirty = true;
    return noErr;
}

int16_t tb_PBGetFPos(gaddr pb, bool async)
{
    fcb *f = fcb_for((int16_t)rd16(pb + IOParam_ioRefNum_off));
    if (!f)
        return rfNumErr;
    wr32(pb + IOParam_ioReqCount_off, 0);
    wr32(pb + IOParam_ioActCount_off, 0);
    wr16(pb + IOParam_ioPosMode_off, 0);
    wr32(pb + IOParam_ioPosOffset_off, f->pos);
    return noErr;
}

int16_t tb_PBSetFPos(gaddr pb, bool async)
{
    fcb *f = fcb_for((int16_t)rd16(pb + IOParam_ioRefNum_off));
    if (!f)
        return rfNumErr;
    int16_t err = set_mark(f, rd16(pb + IOParam_ioPosMode_off), (int32_t)rd32(pb + IOParam_ioPosOffset_off));
    wr32(pb + IOParam_ioPosOffset_off, f->pos);
    return err;
}

int16_t tb_PBAllocate(gaddr pb, bool async)
{
    wr32(pb + IOParam_ioActCount_off, rd32(pb + IOParam_ioReqCount_off));
    return noErr;
}

int16_t tb_PBFlushFile(gaddr pb, bool async)
{
    fcb *f = fcb_for((int16_t)rd16(pb + IOParam_ioRefNum_off));
    return f ? save_fork(f) : rfNumErr;
}

int16_t tb_PBFlushVol(gaddr pb, bool async)
{
    for (int i = 0; i < MAX_FCBS; i++)
        if (fcbs[i].used)
            save_fork(&fcbs[i]);
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* Creating, deleting, renaming */

static int16_t create_file(const char *path)
{
    if (exists(path))
        return dupFNErr;
    mac_file mf;
    memset(&mf, 0, sizeof mf);
    uint8_t empty = 0;
    mf.data = &empty;
    mf.rsrc = &empty;
    mf.create_date = mf.modify_date = mac_now();
    if (sidecar_redundant(path, &mf)) {
        FILE *fp = fopen(path, "wb");
        return fp && fclose(fp) == 0 ? noErr : ioErrF;
    }
    char err[256];
    return appledouble_write(path, &mf, err, sizeof err) ? noErr : ioErrF;
}

static int16_t pb_path(gaddr pb, bool hfs, char *path, size_t n)
{
    return resolve((int16_t)rd16(pb + IOParam_ioVRefNum_off), hfs ? (int32_t)rd32(pb + HFileParam_ioDirID_off) : 0,
                   rd32(pb + IOParam_ioNamePtr_off), hfs, path, n);
}

int16_t tb_PBCreate(gaddr pb, bool async)
{
    char path[PATH_MAX];
    int16_t err = pb_path(pb, hfs_call(), path, sizeof path);
    return err ? err : create_file(path);
}

int16_t tb_PBHCreate(gaddr pb, bool async)
{
    char path[PATH_MAX];
    int16_t err = pb_path(pb, true, path, sizeof path);
    return err ? err : create_file(path);
}

static bool is_open(const char *path)
{
    for (int i = 0; i < MAX_FCBS; i++)
        if (fcbs[i].used && !strcmp(fcbs[i].path, path))
            return true;
    return false;
}

static int16_t delete_path(const char *path)
{
    if (!exists(path))
        return fnfErrF;
    if (is_open(path))
        return fBsyErr;
    if (is_dir(path))
        return rmdir(path) == 0 ? noErr : fBsyErr;
    char *side = appledouble_sidecar_path(path);
    unlink(side);
    free(side);
    return unlink(path) == 0 ? noErr : ioErrF;
}

int16_t tb_PBDelete(gaddr pb, bool async)
{
    char path[PATH_MAX];
    int16_t err = pb_path(pb, hfs_call(), path, sizeof path);
    return err ? err : delete_path(path);
}

int16_t tb_PBHDelete(gaddr pb, bool async)
{
    char path[PATH_MAX];
    int16_t err = pb_path(pb, true, path, sizeof path);
    return err ? err : delete_path(path);
}

static int16_t rename_path(const char *from, gaddr newname)
{
    if (!exists(from))
        return fnfErrF;
    char to[PATH_MAX], comp[256];
    uint8_t nm[256];
    int n = rd8(newname);
    gget(nm, newname + 1, (uint32_t)n);
    mac_to_host(nm, n, comp, sizeof comp);
    snprintf(to, sizeof to, "%s", from);
    char *slash = strrchr(to, '/');
    snprintf(slash + 1, sizeof to - (size_t)(slash + 1 - to), "%s", comp);
    if (exists(to))
        return dupFNErr;
    if (rename(from, to) != 0)
        return ioErrF;
    char *s1 = appledouble_sidecar_path(from), *s2 = appledouble_sidecar_path(to);
    rename(s1, s2);
    free(s1);
    free(s2);
    return noErr;
}

int16_t tb_PBRename(gaddr pb, bool async)
{
    char path[PATH_MAX];
    int16_t err = pb_path(pb, hfs_call(), path, sizeof path);
    return err ? err : rename_path(path, rd32(pb + IOParam_ioMisc_off));
}

int16_t tb_PBHRename(gaddr pb, bool async)
{
    char path[PATH_MAX];
    int16_t err = pb_path(pb, true, path, sizeof path);
    return err ? err : rename_path(path, rd32(pb + IOParam_ioMisc_off));
}

int16_t tb_PBSetFLock(gaddr pb, bool async) { return noErr; }
int16_t tb_PBRstFLock(gaddr pb, bool async) { return noErr; }

/* ---------------------------------------------------------------------- */
/* File information */

static void fill_file_info(gaddr pb, const char *path, bool cat)
{
    mac_file mf;
    char err[256];
    bool ok = appledouble_read(path, &mf, err, sizeof err);
    uint8_t attrib = is_open(path) ? 0x80 : 0;
    wr8(pb + FileParam_ioFlAttrib_off, attrib);
    gmemset(pb + FileParam_ioFlFndrInfo_off, 0, 16);
    uint32_t dl = ok ? (uint32_t)mf.data_len : 0, rl = ok ? (uint32_t)mf.rsrc_len : 0;
    if (ok) {
        uint8_t finfo[16] = { 0 };
        bool host_text = host_file_type(path, finfo);
        if (host_text || finfo[0]) {
            /* Plain host file: type from the extension; text length as seen inside. */
            gput(pb + FileParam_ioFlFndrInfo_off, finfo, 16);
            if (host_text) {
                size_t n;
                free(text_in(mf.data, mf.data_len, &n));
                dl = (uint32_t)n;
            }
        } else {
            gput(pb + FileParam_ioFlFndrInfo_off, mf.finder_info, 16);
        }
        if (cat)
            gput(pb + HFileInfo_ioFlXFndrInfo_off, mf.finder_info + 16, 16);
    }
    wr32(pb + FileParam_ioFlNum_off, (uint32_t)dir_id_for(rel_of(path)) + 0x10000); /* file IDs */
    wr16(pb + FileParam_ioFlStBlk_off, 0);
    wr32(pb + FileParam_ioFlLgLen_off, dl);
    wr32(pb + FileParam_ioFlPyLen_off, (dl + 511) & ~511u);
    wr16(pb + FileParam_ioFlRStBlk_off, 0);
    wr32(pb + FileParam_ioFlRLgLen_off, rl);
    wr32(pb + FileParam_ioFlRPyLen_off, (rl + 511) & ~511u);
    uint32_t c = ok && mf.create_date ? mf.create_date : mac_now(), m = ok && mf.modify_date ? mf.modify_date : c;
    wr32(pb + FileParam_ioFlCrDat_off, c);
    wr32(pb + FileParam_ioFlMdDat_off, m);
    if (cat) {
        wr32(pb + HFileInfo_ioFlBkDat_off, 0);
        wr32(pb + HFileInfo_ioFlParID_off, (uint32_t)parent_id_of(path));
        wr32(pb + HFileInfo_ioFlClpSiz_off, 0);
    }
    if (ok)
        mac_file_free(&mf);
}

static int16_t get_finfo(gaddr pb, bool hfs)
{
    char path[PATH_MAX];
    int16_t idx = (int16_t)rd16(pb + FileParam_ioFDirIndex_off);
    if (idx > 0) {
        char dir[PATH_MAX];
        int32_t d = base_dir((int16_t)rd16(pb + IOParam_ioVRefNum_off), hfs ? (int32_t)rd32(pb + HFileParam_ioDirID_off) : 0, hfs);
        if (!d || !host_of_dir(d, dir, sizeof dir))
            return nsvErr;
        if (!nth_entry(dir, idx, true, path, sizeof path))
            return fnfErrF;
        put_name(rd32(pb + IOParam_ioNamePtr_off), path);
    } else {
        int16_t err = pb_path(pb, hfs, path, sizeof path);
        if (err)
            return err;
        if (!exists(path) || is_dir(path))
            return fnfErrF;
    }
    fill_file_info(pb, path, false);
    if (hfs)
        wr32(pb + HFileParam_ioDirID_off, rd32(pb + FileParam_ioFlNum_off));
    return noErr;
}

int16_t tb_PBGetFInfo(gaddr pb, bool async) { return get_finfo(pb, hfs_call()); }
int16_t tb_PBHGetFInfo(gaddr pb, bool async) { return get_finfo(pb, true); }

static int16_t set_finfo(gaddr pb, bool hfs, bool cat)
{
    char path[PATH_MAX];
    int16_t err = pb_path(pb, hfs, path, sizeof path);
    if (err)
        return err;
    if (!exists(path))
        return fnfErrF;
    if (is_dir(path))
        return noErr;
    mac_file mf;
    char e[256];
    if (!appledouble_read(path, &mf, e, sizeof e))
        return ioErrF;
    gget(mf.finder_info, pb + FileParam_ioFlFndrInfo_off, 16);
    if (cat)
        gget(mf.finder_info + 16, pb + HFileInfo_ioFlXFndrInfo_off, 16);
    mf.has_finder_info = true;
    mf.create_date = rd32(pb + FileParam_ioFlCrDat_off);
    mf.modify_date = rd32(pb + FileParam_ioFlMdDat_off);
    if (sidecar_redundant(path, &mf)) {
        /* Plain host text: the extension says it all; drop any sidecar. */
        mac_file_free(&mf);
        char *side = appledouble_sidecar_path(path);
        if (side)
            unlink(side);
        free(side);
        return noErr;
    }
    bool ok = appledouble_write(path, &mf, e, sizeof e);
    mac_file_free(&mf);
    return ok ? noErr : ioErrF;
}

int16_t tb_PBSetFInfo(gaddr pb, bool async) { return set_finfo(pb, hfs_call(), false); }
int16_t tb_PBHSetFInfo(gaddr pb, bool async) { return set_finfo(pb, true, false); }

/* ---------------------------------------------------------------------- */
/* Volumes */

static int16_t get_vinfo(gaddr pb, bool hfs)
{
    int16_t idx = (int16_t)rd16(pb + VolumeParam_ioVolIndex_off);
    int16_t vref = (int16_t)rd16(pb + IOParam_ioVRefNum_off);
    if (idx > 1 || (idx <= 0 && vref != 0 && vref != VREF && vref != 1 && !(vref <= -32000 && vref > -32000 - MAX_WDS)))
        return nsvErr;
    gaddr namep = rd32(pb + IOParam_ioNamePtr_off);
    if (namep)
        gput(namep, vol_name, (uint32_t)vol_name[0] + 1);
    wr16(pb + IOParam_ioVRefNum_off, (uint16_t)VREF);
    wr32(pb + VolumeParam_ioVCrDate_off, mac_now());
    wr32(pb + HVolumeParam_ioVLsMod_off, mac_now());
    wr16(pb + VolumeParam_ioVAtrb_off, 0);
    wr16(pb + VolumeParam_ioVNmFls_off, (uint16_t)count_entries("/"));
    wr16(pb + HVolumeParam_ioVBitMap_off, 0);
    wr16(pb + HVolumeParam_ioVAllocPtr_off, 0);
    /* 65535 blocks of 32K: about 2 GB, the largest an HFS volume reports. */
    wr16(pb + VolumeParam_ioVNmAlBlks_off, 0xFFFF);
    wr32(pb + VolumeParam_ioVAlBlkSiz_off, 32768);
    wr32(pb + VolumeParam_ioVClpSiz_off, 32768);
    wr16(pb + VolumeParam_ioAlBlSt_off, 0);
    wr32(pb + HVolumeParam_ioVNxtCNID_off, (uint32_t)ndirs + 0x20000);
    wr16(pb + VolumeParam_ioVFrBlk_off, 0xC000);
    if (hfs) {
        wr16(pb + HVolumeParam_ioVSigWord_off, 0x4244);
        wr16(pb + HVolumeParam_ioVDrvInfo_off, 1);
        wr16(pb + HVolumeParam_ioVDRefNum_off, (uint16_t)-5);
        wr16(pb + HVolumeParam_ioVFSID_off, 0);
        wr32(pb + HVolumeParam_ioVBkUp_off, 0);
        wr16(pb + HVolumeParam_ioVSeqNum_off, 0);
        wr32(pb + HVolumeParam_ioVWrCnt_off, 0);
        wr32(pb + HVolumeParam_ioVFilCnt_off, 0);
        wr32(pb + HVolumeParam_ioVDirCnt_off, 0);
        gmemset(pb + HVolumeParam_ioVFndrInfo_off, 0, 32);
    }
    return noErr;
}

int16_t tb_PBGetVInfo(gaddr pb, bool async) { return get_vinfo(pb, false); }
int16_t tb_PBHGetVInfo(gaddr pb, bool async) { return get_vinfo(pb, true); }

int16_t tb_PBGetVol(gaddr pb, bool async)
{
    gaddr namep = rd32(pb + IOParam_ioNamePtr_off);
    if (namep)
        gput(namep, vol_name, (uint32_t)vol_name[0] + 1);
    wr16(pb + IOParam_ioVRefNum_off, (uint16_t)default_vref);
    return noErr;
}

int16_t tb_PBHGetVol(gaddr pb, bool async)
{
    tb_PBGetVol(pb, async);
    wr16(pb + WDPBRec_ioWDVRefNum_off, (uint16_t)VREF);
    wr32(pb + WDPBRec_ioWDDirID_off, (uint32_t)default_dir);
    wr32(pb + WDPBRec_ioWDProcID_off, 0);
    return noErr;
}

static int16_t set_vol(gaddr pb, bool hfs)
{
    int16_t vref = (int16_t)rd16(pb + IOParam_ioVRefNum_off);
    int32_t d = base_dir(vref, hfs ? (int32_t)rd32(pb + WDPBRec_ioWDDirID_off) : 0, hfs);
    if (!d)
        return nsvErr;
    default_vref = vref ? vref : default_vref;
    default_dir = d;
    wr32(LM_CurDirStore, (uint32_t)d);
    return noErr;
}

int16_t tb_PBSetVol(gaddr pb, bool async) { return set_vol(pb, false); }
int16_t tb_PBHSetVol(gaddr pb, bool async) { return set_vol(pb, true); }

/* ---------------------------------------------------------------------- */
/* Working directories and the catalog (FSDispatch) */

int16_t tb_PBOpenWD(gaddr pb, bool async)
{
    char path[PATH_MAX];
    int32_t dirid = (int32_t)rd32(pb + WDPBRec_ioWDDirID_off);
    int16_t err = resolve((int16_t)rd16(pb + IOParam_ioVRefNum_off), dirid, rd32(pb + IOParam_ioNamePtr_off), true, path,
                          sizeof path);
    if (err)
        return err;
    if (!is_dir(path))
        return dirNFErr;
    int32_t id = dir_id_for(rel_of(path));
    if (id == ROOT_ID) {
        wr16(pb + IOParam_ioVRefNum_off, (uint16_t)VREF);
        return noErr;
    }
    for (int i = 0; i < MAX_WDS; i++)
        if (wds[i].used && wds[i].dirid == id) {
            wr16(pb + IOParam_ioVRefNum_off, (uint16_t)(-32000 - i));
            return noErr;
        }
    for (int i = 0; i < MAX_WDS; i++)
        if (!wds[i].used) {
            wds[i].used = true;
            wds[i].dirid = id;
            wr16(pb + IOParam_ioVRefNum_off, (uint16_t)(-32000 - i));
            return noErr;
        }
    return tmwdoErr;
}

int16_t tb_PBCloseWD(gaddr pb, bool async)
{
    int16_t v = (int16_t)rd16(pb + IOParam_ioVRefNum_off);
    if (v <= -32000 && v > -32000 - MAX_WDS)
        wds[-32000 - v].used = false;
    return noErr;
}

int16_t tb_PBGetWDInfo(gaddr pb, bool async)
{
    int16_t v = (int16_t)rd16(pb + IOParam_ioVRefNum_off);
    int32_t d = base_dir(v, 0, false);
    if (!d)
        return nsvErr;
    wr16(pb + WDPBRec_ioWDVRefNum_off, (uint16_t)VREF);
    wr32(pb + WDPBRec_ioWDDirID_off, (uint32_t)d);
    wr32(pb + WDPBRec_ioWDProcID_off, 0);
    gaddr namep = rd32(pb + IOParam_ioNamePtr_off);
    if (namep)
        gput(namep, vol_name, (uint32_t)vol_name[0] + 1);
    return noErr;
}

int16_t tb_PBGetCatInfo(gaddr pb, bool async)
{
    char path[PATH_MAX];
    int16_t idx = (int16_t)rd16(pb + FileParam_ioFDirIndex_off);
    int32_t dirid = (int32_t)rd32(pb + HFileInfo_ioDirID_off);
    int16_t vref = (int16_t)rd16(pb + IOParam_ioVRefNum_off);
    gaddr namep = rd32(pb + IOParam_ioNamePtr_off);
    if (idx > 0) {
        char dir[PATH_MAX];
        int32_t d = base_dir(vref, dirid, true);
        if (!d || !host_of_dir(d, dir, sizeof dir))
            return dirNFErr;
        if (!nth_entry(dir, idx, false, path, sizeof path))
            return fnfErrF;
        put_name(namep, path);
    } else if (idx < 0) {
        int32_t d = base_dir(vref, dirid, true);
        if (!d || !host_of_dir(d, path, sizeof path))
            return dirNFErr;
        if (namep) {
            if (d == ROOT_ID)
                gput(namep, vol_name, (uint32_t)vol_name[0] + 1);
            else
                put_name(namep, path);
        }
    } else {
        int16_t err = resolve(vref, dirid, namep, true, path, sizeof path);
        if (err)
            return err;
        if (!exists(path))
            return fnfErrF;
    }
    if (is_dir(path)) {
        int32_t id = dir_id_for(rel_of(path));
        wr8(pb + DirInfo_ioFlAttrib_off, 0x10);
        gmemset(pb + DirInfo_ioDrUsrWds_off, 0, 16);
        wr32(pb + DirInfo_ioDrDirID_off, (uint32_t)id);
        wr16(pb + DirInfo_ioDrNmFls_off, (uint16_t)count_entries(path));
        wr32(pb + DirInfo_ioDrCrDat_off, mac_now());
        wr32(pb + DirInfo_ioDrMdDat_off, mac_now());
        wr32(pb + DirInfo_ioDrBkDat_off, 0);
        gmemset(pb + DirInfo_ioDrFndrInfo_off, 0, 16);
        wr32(pb + DirInfo_ioDrParID_off, id == ROOT_ID ? 1 : (uint32_t)parent_id_of(path));
    } else {
        fill_file_info(pb, path, true);
        wr32(pb + HFileInfo_ioDirID_off, rd32(pb + FileParam_ioFlNum_off));
    }
    return noErr;
}

int16_t tb_PBSetCatInfo(gaddr pb, bool async) { return set_finfo(pb, true, true); }

int16_t tb_PBDirCreate(gaddr pb, bool async)
{
    char path[PATH_MAX];
    int16_t err = resolve((int16_t)rd16(pb + IOParam_ioVRefNum_off), (int32_t)rd32(pb + HFileParam_ioDirID_off),
                          rd32(pb + IOParam_ioNamePtr_off), true, path, sizeof path);
    if (err)
        return err;
    if (exists(path))
        return dupFNErr;
    if (mkdir(path, 0755) != 0)
        return ioErrF;
    wr32(pb + HFileParam_ioDirID_off, (uint32_t)dir_id_for(rel_of(path)));
    return noErr;
}

int16_t tb_PBCatMove(gaddr pb, bool async)
{
    char from[PATH_MAX], to[PATH_MAX];
    int16_t err = resolve((int16_t)rd16(pb + IOParam_ioVRefNum_off), (int32_t)rd32(pb + HFileParam_ioDirID_off),
                          rd32(pb + IOParam_ioNamePtr_off), true, from, sizeof from);
    if (err)
        return err;
    err = resolve((int16_t)rd16(pb + IOParam_ioVRefNum_off), (int32_t)rd32(pb + 36) /* ioNewDirID */,
                  rd32(pb + 28) /* ioNewName */, true, to, sizeof to);
    if (err)
        return err;
    if (!is_dir(to))
        return badMovErr;
    const char *base = strrchr(from, '/');
    char dest[PATH_MAX];
    snprintf(dest, sizeof dest, "%s%s", to, base);
    if (rename(from, dest) != 0)
        return ioErrF;
    char *s1 = appledouble_sidecar_path(from), *s2 = appledouble_sidecar_path(dest);
    rename(s1, s2);
    free(s1);
    free(s2);
    return noErr;
}

int16_t tb_PBGetFCBInfo(gaddr pb, bool async)
{
    int16_t idx = (int16_t)rd16(pb + FCBPBRec_ioFCBIndx_off);
    fcb *f = NULL;
    int16_t rn = 0;
    if (idx > 0) {
        int n = 0;
        for (int i = 0; i < MAX_FCBS; i++)
            if (fcbs[i].used && ++n == idx) {
                f = &fcbs[i];
                rn = fcb_refnum(i);
                break;
            }
    } else {
        rn = (int16_t)rd16(pb + IOParam_ioRefNum_off);
        f = fcb_for(rn);
    }
    if (!f)
        return idx > 0 ? fnOpnErr : rfNumErr;
    wr16(pb + IOParam_ioRefNum_off, (uint16_t)rn);
    gaddr namep = rd32(pb + IOParam_ioNamePtr_off);
    if (namep)
        gput(namep, f->name, (uint32_t)f->name[0] + 1);
    wr32(pb + FCBPBRec_ioFCBFlNm_off, (uint32_t)dir_id_for(rel_of(f->path)) + 0x10000);
    wr16(pb + FCBPBRec_ioFCBFlags_off, (uint16_t)((f->write ? 0x0100 : 0) | (f->rsrc ? 0x0200 : 0)));
    wr16(pb + FCBPBRec_ioFCBStBlk_off, 0);
    wr32(pb + FCBPBRec_ioFCBEOF_off, f->len);
    wr32(pb + FCBPBRec_ioFCBPLen_off, (f->len + 511) & ~511u);
    wr32(pb + FCBPBRec_ioFCBCrPs_off, f->pos);
    wr16(pb + FCBPBRec_ioFCBVRefNum_off, (uint16_t)VREF);
    wr32(pb + FCBPBRec_ioFCBClpSiz_off, 0);
    wr32(pb + FCBPBRec_ioFCBParID_off, (uint32_t)f->parid);
    return noErr;
}

/* Device Manager calls on files or absent drivers. */
int16_t tb_PBControl(gaddr pbp, bool a) { return controlErr; }
int16_t tb_PBStatus(gaddr pbp, bool a) { return statusErr; }
int16_t tb_PBKillIO(gaddr pbp, bool a) { return noErr; }

/* ---------------------------------------------------------------------- */
/* FSSpec calls (HighLevelFSDispatch) */

int16_t tb_FSMakeFSSpec(int16_t vRefNum, int32_t dir_id, gaddr file_name, gaddr spec)
{
    char path[PATH_MAX];
    int16_t err = resolve(vRefNum, dir_id, file_name, true, path, sizeof path);
    if (err)
        return err;
    char parent[PATH_MAX];
    snprintf(parent, sizeof parent, "%s", path);
    char *slash = strrchr(parent, '/');
    bool at_root = !strcmp(path, "/");
    if (slash && !at_root)
        *slash = 0;
    wr16(spec + FSSpec_vRefNum_off, (uint16_t)VREF);
    wr32(spec + FSSpec_parID_off, at_root ? 1 : (uint32_t)dir_id_for(rel_of(parent)));
    if (at_root)
        gput(spec + FSSpec_name_off, vol_name, (uint32_t)vol_name[0] + 1);
    else
        put_name(spec + FSSpec_name_off, path);
    return exists(path) ? noErr : fnfErrF;
}

/* A parameter block for an FSSpec (in the system heap scratch area). */
static gaddr spec_pb(gaddr spec)
{
    static gaddr pb;
    if (!pb)
        pb = tb_NewPtr(HFileInfo_SIZEOF, true, true);
    gmemset(pb, 0, HFileInfo_SIZEOF);
    wr32(pb + IOParam_ioNamePtr_off, spec + FSSpec_name_off);
    wr16(pb + IOParam_ioVRefNum_off, rd16(spec + FSSpec_vRefNum_off));
    wr32(pb + HFileParam_ioDirID_off, rd32(spec + FSSpec_parID_off));
    return pb;
}

int16_t tb_FSpOpenDF(gaddr spec, int8_t perms, gaddr refNum_out)
{
    gaddr pb = spec_pb(spec);
    wr8(pb + IOParam_ioPermssn_off, (uint8_t)perms);
    int16_t err = open_fork(pb, false, true);
    wr16(refNum_out, rd16(pb + IOParam_ioRefNum_off));
    return err;
}

int16_t tb_FSpOpenRF(gaddr spec, int8_t perms, gaddr refNum_out)
{
    gaddr pb = spec_pb(spec);
    wr8(pb + IOParam_ioPermssn_off, (uint8_t)perms);
    int16_t err = open_fork(pb, true, true);
    wr16(refNum_out, rd16(pb + IOParam_ioRefNum_off));
    return err;
}

int16_t tb_FSpCreate(gaddr spec, int32_t creator, int32_t file_type, int16_t script)
{
    gaddr pb = spec_pb(spec);
    char path[PATH_MAX];
    int16_t err = pb_path(pb, true, path, sizeof path);
    if (err)
        return err;
    if ((err = create_file(path)))
        return err;
    get_finfo(pb, true);
    wr32(pb + FileParam_ioFlFndrInfo_off, (uint32_t)file_type);
    wr32(pb + FileParam_ioFlFndrInfo_off + 4, (uint32_t)creator);
    wr32(pb + HFileParam_ioDirID_off, rd32(spec + FSSpec_parID_off));
    return set_finfo(pb, true, false);
}

int16_t tb_FSpDelete(gaddr spec) { return tb_PBHDelete(spec_pb(spec), false); }

int16_t tb_FSpGetFInfo(gaddr spec, gaddr fndr_info)
{
    gaddr pb = spec_pb(spec);
    int16_t err = get_finfo(pb, true);
    if (!err)
        gmemcpy(fndr_info, pb + FileParam_ioFlFndrInfo_off, 16);
    return err;
}

int16_t tb_FSpSetFInfo(gaddr spec, gaddr fndr_info)
{
    gaddr pb = spec_pb(spec);
    int16_t err = get_finfo(pb, true);
    if (err)
        return err;
    gmemcpy(pb + FileParam_ioFlFndrInfo_off, fndr_info, 16);
    wr32(pb + HFileParam_ioDirID_off, rd32(spec + FSSpec_parID_off));
    return set_finfo(pb, true, false);
}

int16_t tb_FSpRename(gaddr spec, gaddr new_name)
{
    gaddr pb = spec_pb(spec);
    char path[PATH_MAX];
    int16_t err = pb_path(pb, true, path, sizeof path);
    return err ? err : rename_path(path, new_name);
}

int16_t tb_FSpDirCreate(gaddr spec, int16_t script, gaddr created_dir_id)
{
    gaddr pb = spec_pb(spec);
    int16_t err = tb_PBDirCreate(pb, false);
    if (!err && created_dir_id)
        wr32(created_dir_id, rd32(pb + HFileParam_ioDirID_off));
    return err;
}

/* Old-style (vRefNum, name) for a host file: a working directory for its folder. */
int16_t fm_wd_for_path(const char *path, gaddr name_out)
{
    char full[PATH_MAX], parent[PATH_MAX];
    if (!realpath(path, full))
        snprintf(full, sizeof full, "%s", path);
    snprintf(parent, sizeof parent, "%s", full);
    char *slash = strrchr(parent, '/');
    if (slash)
        *slash = 0;
    int32_t id = dir_id_for(rel_of(parent));
    put_name(name_out, full);
    for (int i = 0; i < MAX_WDS; i++)
        if (wds[i].used && wds[i].dirid == id)
            return (int16_t)(-32000 - i);
    for (int i = 0; i < MAX_WDS; i++)
        if (!wds[i].used) {
            wds[i].used = true;
            wds[i].dirid = id;
            return (int16_t)(-32000 - i);
        }
    return VREF;
}

/* Classic file type of a host file (from its sidecar or extension). */
uint32_t fm_type_of_path(const char *path)
{
    uint8_t finfo[16] = { 0 };
    if (host_file_type(path, finfo) || finfo[0])
        return ((uint32_t)finfo[0] << 24) | ((uint32_t)finfo[1] << 16) | ((uint32_t)finfo[2] << 8) | finfo[3];
    mac_file mf;
    char err[256];
    if (!appledouble_read(path, &mf, err, sizeof err))
        return 0;
    uint32_t t = mac_file_type(&mf);
    mac_file_free(&mf);
    return t;
}

/* ---------------------------------------------------------------------- */
/* Full pathnames, for the Alias Manager */

/* The Mac full path ("Macintosh HD:Users:me:file", MacRoman) of a host path.
 * Returns its length, or 0 if it does not fit. */
size_t fm_mac_path_of_host(const char *host, uint8_t *out, size_t outsize)
{
    char full[PATH_MAX];
    if (!realpath(host, full))
        snprintf(full, sizeof full, "%s", host);
    size_t n = strlen(vol_name);
    if (n >= outsize)
        return 0;
    memcpy(out, vol_name, n);
    const char *p = rel_of(full);
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        char comp[NAME_MAX + 1];
        snprintf(comp, sizeof comp, "%.*s", (int)len, p);
        uint8_t mac[32];
        int ml = host_to_mac(comp, mac);
        if (n + 1 + (size_t)ml > outsize)
            return 0;
        out[n++] = ':';
        memcpy(out + n, mac, (size_t)ml);
        n += (size_t)ml;
        p += len;
        if (*p == '/')
            p++;
    }
    if (n == strlen(vol_name)) { /* the volume itself */
        if (n + 1 > outsize)
            return 0;
        out[n++] = ':';
    }
    return n;
}

/* The host path of a Mac full path; the volume name is not checked. */
bool fm_host_of_mac_path(const uint8_t *mac, size_t n, char *out, size_t outsize)
{
    size_t i = 0;
    while (i < n && mac[i] != ':')
        i++;
    if (i == n)
        return false;
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s", root_path);
    for (i++; i < n;) {
        size_t start = i;
        while (i < n && mac[i] != ':')
            i++;
        if (i > start) {
            char comp[256];
            mac_to_host(mac + start, (int)(i - start), comp, sizeof comp);
            size_t pl = strlen(path);
            if (pl && path[pl - 1] == '/')
                pl--;
            snprintf(path + pl, sizeof path - pl, "/%s", comp);
        }
        i++;
    }
    snprintf(out, outsize, "%s", path[0] ? path : "/");
    return true;
}
