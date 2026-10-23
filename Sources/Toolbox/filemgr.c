/*
 * File Manager over host folders.
 *
 * One volume, "Macintosh HD" (vRefNum -1, drive 1), is a folder of its own
 * under the user's Library (Application Support/Blue Box/Macintosh HD, or
 * the XDG data directory elsewhere): installers and applications write
 * there freely, and the System Folder lives in it.  The host's files appear
 * in a folder at its root, "Host", but only where access was granted: the
 * application's own folder, the documents it was opened with, and the
 * files chosen in the Open and Save panels (with their folders, so saving
 * through a temporary file works).  Elsewhere the host is invisible and
 * read-only.  Directory IDs are assigned on first use (the root is 2).  Working directories
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
#include <time.h>
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
#define FCB_SLOTS (FCB_FIRST + MAX_FCBS)
#define FCB_LEN 94

uint32_t mac_now(void);

/* A host time as Mac time (local time, seconds since 1904). */
static uint32_t mac_time(time_t t)
{
    struct tm lt;
    localtime_r(&t, &lt);
    return (uint32_t)((int64_t)t + lt.tm_gmtoff + 2082844800LL);
}

/* A host path's modification (and status-change, for "created") times. */
static void host_times(const char *path, uint32_t *created, uint32_t *modified)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        *created = *modified = mac_now();
        return;
    }
#ifdef __APPLE__
    *created = mac_time(st.st_birthtimespec.tv_sec);
#else
    *created = mac_time(st.st_mtime);
#endif
    *modified = mac_time(st.st_mtime);
}

static char root_path[PATH_MAX]; /* the volume's folder */
static char vol_name[32]; /* MacRoman */

/* Further volumes: folders mounted as volumes of their own (a CD or disk
 * the application came on, whose programs look for their files at its
 * root).  Directory IDs stay global; a mounted root's ID is reported as 2
 * on its volume, and 2 (or no directory) on its refnum means that root. */
#define MAX_MOUNTS 8
typedef struct {
    int16_t vref;
    char name[32]; /* MacRoman, Pascal */
    char root[PATH_MAX];
    int32_t root_id;
} mount;
static mount mounts[MAX_MOUNTS];
static int nmounts;
static char pending_mount[MAX_MOUNTS][PATH_MAX], pending_name[MAX_MOUNTS][64];
static int npending;

void machine_mount_folder(const char *path, const char *name)
{
    if (npending >= MAX_MOUNTS)
        return;
    snprintf(pending_mount[npending], PATH_MAX, "%s", path);
    snprintf(pending_name[npending], 64, "%s", name ? name : "");
    npending++;
}

/* The volumes for the host's file panels: 0 the main one, then the mounts
 * (AppleShare volumes among them). */
int machine_volume_count(void) { return 1 + nmounts; }

bool machine_volume_info(int i, char *name, size_t namesize, char *root, size_t rootsize)
{
    const char *mac, *dir;
    if (i == 0) {
        mac = vol_name;
        dir = root_path;
    } else if (i >= 1 && i <= nmounts) {
        mac = mounts[i - 1].name;
        dir = mounts[i - 1].root;
    } else {
        return false;
    }
    macroman_to_utf8((const uint8_t *)mac + 1, (uint8_t)mac[0], name, namesize);
    snprintf(root, rootsize, "%s", dir);
    return true;
}

static mount *mount_of_vref(int16_t vref)
{
    for (int i = 0; i < nmounts; i++)
        if (mounts[i].vref == vref)
            return &mounts[i];
    return NULL;
}

static mount *mount_of_id(int32_t id)
{
    for (int i = 0; i < nmounts; i++)
        if (mounts[i].root_id == id)
            return &mounts[i];
    return NULL;
}

/* The mounted volume a host path is on, or NULL for the main volume. */
static mount *mount_of_path(const char *host)
{
    mount *best = NULL;
    size_t best_len = 0;
    for (int i = 0; i < nmounts; i++) {
        size_t n = strlen(mounts[i].root);
        if (!strncmp(host, mounts[i].root, n) && (host[n] == '/' || host[n] == 0) && n > best_len) {
            best = &mounts[i];
            best_len = n;
        }
    }
    return best;
}

/* A directory ID as its volume reports it: a mounted root is 2. */
static int32_t out_id(int32_t id) { return mount_of_id(id) ? ROOT_ID : id; }
static bool is_root_id(int32_t id) { return id == ROOT_ID || mount_of_id(id); }
static int16_t vref_of_path(const char *host)
{
    mount *m = mount_of_path(host);
    return m ? m->vref : VREF;
}

/* The folder at the volume's root standing for the host's "/". */
static const char host_folder[] = "Host";

/* Where host paths may be seen and changed. */
typedef enum { GRANT_TREE, GRANT_FILE } grant_kind;
#define MAX_GRANTS 128
static struct {
    char *path;
    grant_kind kind;
} grants[MAX_GRANTS];
static int ngrants;

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

/* A resource file the Resource Manager opened is an open resource fork
 * too: its reference number reads the fork through the File Manager, as on
 * a Mac (installers read their own compressed resources so, from the
 * offsets in the resource map).  Read-only views, each with its own mark. */
static struct {
    int16_t rn;
    const uint8_t *data;
    fcb f;
} rsrc_views[8];

static fcb *fcb_readable(int16_t rn)
{
    fcb *f = fcb_for(rn);
    const uint8_t *data;
    uint32_t len;
    if (f || !rm_fork_of(rn, &data, &len))
        return f;
    int slot = 0;
    for (int i = 0; i < 8; i++) {
        if (rsrc_views[i].rn == rn) {
            slot = i;
            break;
        }
        if (!rsrc_views[i].rn)
            slot = i;
    }
    if (rsrc_views[slot].rn != rn || rsrc_views[slot].data != data) {
        memset(&rsrc_views[slot], 0, sizeof rsrc_views[slot]);
        rsrc_views[slot].rn = rn;
        rsrc_views[slot].data = data;
    }
    f = &rsrc_views[slot].f;
    f->used = f->rsrc = true;
    f->buf = (uint8_t *)data;
    f->len = f->cap = len;
    if (f->pos > len)
        f->pos = len;
    return f;
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

/* A host name as a Mac name (at most 31 MacRoman bytes).  Longer names keep
 * their first 25 bytes and gain '#' and five hex digits of a hash of the
 * whole name, so they stay distinct and map back (see host_component). */
static int host_to_mac(const char *name, uint8_t *out)
{
    char tmp[NAME_MAX + 1];
    snprintf(tmp, sizeof tmp, "%s", name);
    for (char *p = tmp; *p; p++)
        if (*p == ':')
            *p = '/';
    uint8_t full[NAME_MAX * 2 + 1];
    int n = (int)utf8_to_macroman(tmp, full, sizeof full);
    if (n <= 31) {
        memcpy(out, full, (size_t)n);
        return n;
    }
    uint32_t h = 2166136261u; /* FNV-1a */
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        h = (h ^ *p) * 16777619u;
    memcpy(out, full, 25);
    char tail[8];
    snprintf(tail, sizeof tail, "#%05X", h & 0xFFFFF);
    memcpy(out + 25, tail, 6);
    return 31;
}

static const char *entry_name(const char *dir, const char *n);
static int mac_char_cmp(const uint8_t *a, const uint8_t *b, int n)
{
    for (int i = 0; i < n; i++)
        if (mac_char_key(a[i], false, false) != mac_char_key(b[i], false, false))
            return 1;
    return 0;
}

/* The host name in `dir` for a Mac name: the same name when it exists,
 * else an entry whose Mac name matches ignoring case (as HFS does) or that
 * was shortened to it; else the plain conversion (for new files). */
static void host_component(const char *dir, const uint8_t *mac, int n, char *out, size_t outsize)
{
    as_ensure_dir(dir[0] ? dir : "/"); /* an AppleShare folder looked into: from the server */
    mac_to_host(mac, n, out, outsize);
    char full[PATH_MAX];
    struct stat st;
    snprintf(full, sizeof full, "%s/%s", dir, out);
    if (stat(full, &st) == 0)
        return;
    char *side = appledouble_sidecar_path(full);
    bool have_side = side && stat(side, &st) == 0;
    free(side);
    if (have_side)
        return;
    DIR *d = opendir(dir[0] ? dir : "/");
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *name = entry_name(dir, e->d_name);
        if (!name)
            continue;
        uint8_t m[32];
        int ml = host_to_mac(name, m);
        if (ml == n && !mac_char_cmp(m, mac, n)) {
            snprintf(out, outsize, "%s", name);
            break;
        }
    }
    closedir(d);
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

/* Directories are recorded relative to the volume's folder ("" for its
 * root), or as \1 and the absolute path for the host's ("\1" alone for
 * "/", the Host folder). */
#define HOST_MARK '\1'

static bool host_of_dir(int32_t id, char *out, size_t outsize)
{
    if (id < ROOT_ID || id >= ndirs || !dirs[id])
        return false;
    if (dirs[id][0] == HOST_MARK)
        snprintf(out, outsize, "%s", dirs[id][1] ? dirs[id] + 1 : "/");
    else if (dirs[id][0])
        snprintf(out, outsize, "%s/%s", root_path, dirs[id]);
    else
        snprintf(out, outsize, "%s", root_path);
    return true;
}

/* Whether a host path is inside the volume's own folder. */
static bool on_volume(const char *host)
{
    size_t n = strlen(root_path);
    return !strncmp(host, root_path, n) && (host[n] == '/' || host[n] == 0);
}

/* The recorded form of a host path (see host_of_dir). */
static const char *rel_of(const char *host)
{
    static char buf[PATH_MAX + 2];
    if (on_volume(host)) {
        size_t n = strlen(root_path);
        return host[n] == '/' ? host + n + 1 : host + n;
    }
    snprintf(buf, sizeof buf, "%c%s", HOST_MARK, strcmp(host, "/") ? host : "");
    return buf;
}

/* ---- Access to the host's files ---- */

static bool under(const char *path, const char *dir)
{
    size_t n = strlen(dir);
    if (n == 1 && dir[0] == '/')
        return path[0] == '/';
    return !strncmp(path, dir, n) && (path[n] == '/' || path[n] == 0);
}

static void parent_of(const char *path, char *out, size_t outsize)
{
    snprintf(out, outsize, "%s", path);
    char *slash = strrchr(out, '/');
    if (slash && slash != out)
        *slash = 0;
    else if (slash)
        slash[1] = 0;
}

static void canonical_path(const char *path, char *full, size_t size);

/* Let the application see and change a host path: a folder and all inside
 * it, or a file and the other files in its folder. */
void fm_grant(const char *path, bool tree)
{
    char full[PATH_MAX];
    canonical_path(path, full, sizeof full); /* a file still to be saved need not exist */
    for (int i = 0; i < ngrants; i++)
        if (!strcmp(grants[i].path, full) && grants[i].kind == (tree ? GRANT_TREE : GRANT_FILE))
            return;
    if (ngrants < MAX_GRANTS) {
        grants[ngrants].path = strdup(full);
        grants[ngrants].kind = tree ? GRANT_TREE : GRANT_FILE;
        ngrants++;
    }
}

/* Changes are allowed on the volume, inside granted folders, and to
 * granted files and their neighbours. */
static bool writable(const char *path)
{
    if (on_volume(path))
        return true;
    char dir[PATH_MAX];
    parent_of(path, dir, sizeof dir);
    for (int i = 0; i < ngrants; i++) {
        if (grants[i].kind == GRANT_TREE && under(path, grants[i].path))
            return true;
        if (grants[i].kind == GRANT_FILE) {
            char gdir[PATH_MAX];
            parent_of(grants[i].path, gdir, sizeof gdir);
            if (!strcmp(path, grants[i].path) || !strcmp(dir, gdir))
                return true;
        }
    }
    return false;
}

/* Visible: what can be changed, and the folders leading to it. */
static bool visible(const char *path)
{
    if (writable(path))
        return true;
    for (int i = 0; i < ngrants; i++)
        if (under(grants[i].path, path))
            return true;
    return false;
}

static void put_name(gaddr namep, const char *host);
static void put_name_fwd(gaddr namep, const char *host) { put_name(namep, host); }

static int32_t parent_id_of(const char *host)
{
    char tmp[PATH_MAX + 2];
    snprintf(tmp, sizeof tmp, "%s", rel_of(host));
    if (tmp[0] == HOST_MARK && !tmp[1])
        return ROOT_ID; /* the Host folder sits at the root */
    char *slash = strrchr(tmp, '/');
    if (!slash)
        return ROOT_ID;
    *slash = 0;
    return dir_id_for(tmp);
}

/* Directory a (vRefNum, dirID) pair names, or 0. */
static int32_t base_dir(int16_t vref, int32_t dirid, bool hfs)
{
    mount *m = mount_of_vref(vref);
    if (!m && vref <= -32000 && vref > -32000 - MAX_WDS && hfs && (dirid == 1 || dirid == ROOT_ID)) {
        /* A working directory's root is its own volume's. */
        int i = -32000 - vref;
        char wpath[PATH_MAX];
        if (wds[i].used && host_of_dir(wds[i].dirid, wpath, sizeof wpath))
            m = mount_of_path(wpath);
    }
    if (m) {
        if (hfs && dirid && dirid != 1 && dirid != ROOT_ID)
            return dirid;
        if (!dirid && default_vref == vref)
            return default_dir;
        return m->root_id;
    }
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
    /* A full pathname ("Volume:...") names its own place: the refnum and
     * directory are ignored, whatever they hold. */
    bool full = false;
    if (namep && rd8(namep) && rd8(namep + 1) != ':') {
        uint8_t nm[256];
        int nn = rd8(namep);
        gget(nm, namep + 1, (uint32_t)nn);
        full = memchr(nm, ':', (size_t)nn) != NULL;
    }
    int32_t dir = full ? ROOT_ID : base_dir(vref, dirid, hfs);
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
    /* So does a volume's name in the root's parent (fsRtParID), as
     * FSMakeFSSpec gives a volume. */
    bool volume = false;
    if (hfs && dirid == 1 && !has_colon) {
        volume = vol_name[0] == n && !mac_char_cmp(name, (const uint8_t *)vol_name + 1, n);
        for (int k = 0; k < nmounts && !volume; k++)
            volume = mounts[k].name[0] == n && !mac_char_cmp(name, (const uint8_t *)mounts[k].name + 1, n);
    }
    if ((has_colon || volume) && name[0] != ':') {
        while (i < n && name[i] != ':')
            i++;
        snprintf(path, sizeof path, "%s", root_path);
        for (int k = 0; k < nmounts; k++) /* a mounted volume's name */
            if (mounts[k].name[0] == i && !mac_char_cmp(name, (const uint8_t *)mounts[k].name + 1, i))
                snprintf(path, sizeof path, "%s", mounts[k].root);
        i++;
    } else if (name[0] == ':') {
        i = 1;
    }
    while (i < n) {
        int start = i;
        while (i < n && name[i] != ':')
            i++;
        int len = i - start;
        if (len == 0) { /* "::" goes up, no further than the volume's root */
            if (!strcmp(path, "/")) {
                snprintf(path, sizeof path, "%s", root_path);
            } else if (strcmp(path, root_path) && !(mount_of_path(path) && !strcmp(mount_of_path(path)->root, path))) {
                char *slash = strrchr(path, '/');
                if (slash && slash != path)
                    *slash = 0;
                else if (slash)
                    slash[1] = 0;
            }
        } else if (!strcmp(path, root_path) && len == (int)strlen(host_folder) &&
                   !mac_char_cmp(name + start, (const uint8_t *)host_folder, len)) {
            snprintf(path, sizeof path, "/");
        } else {
            char comp[256];
            host_component(path, name + start, len, comp, sizeof comp);
            size_t pl = strlen(path);
            if (pl && path[pl - 1] == '/')
                pl--;
            snprintf(path + pl, sizeof path - pl, "/%s", comp);
        }
        if (i < n)
            i++;
    }
    snprintf(out, outsize, "%s", path);
    LOGF(LOG_TRAPS, "  name %.*s (vref %d, dir %d) -> %s\n", n, (const char *)name, vref, dirid, path);
    if (!visible(path)) {
        LOGF(LOG_TRAPS, "  (outside the volume and not granted)\n");
        return fnfErrF;
    }
    return noErr;
}

static bool is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* A file exists if its data fork does, or only its AppleDouble sidecar
 * (files with an empty data fork are often stored that way). */
bool fm_host_exists(const char *p)
{
    struct stat st;
    if (stat(p, &st) == 0)
        return true;
    char *side = appledouble_sidecar_path(p);
    bool ok = side && stat(side, &st) == 0;
    free(side);
    return ok;
}

static bool exists(const char *p) { return fm_host_exists(p); }

static bool hidden_name(const char *n)
{
    return n[0] == '.' || !strcmp(n, "Icon\r");
}

/* The Mac-visible name for a directory entry, or NULL: sidecars stand for
 * their file when the data fork itself is missing. */
static const char *entry_name(const char *dir, const char *n)
{
    if (n[0] == '.' && n[1] == '_' && n[2]) {
        char full[PATH_MAX];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", dir, n + 2);
        return stat(full, &st) == 0 ? NULL : n + 2;
    }
    return hidden_name(n) ? NULL : n;
}

/* The n-th (1-based) visible entry of a directory; dirs and files mixed, in
 * catalog order: HFS compares names without regard to case. */
static int cmp_names(const void *a, const void *b)
{
    const char *x = *(char *const *)a, *y = *(char *const *)b;
    int r = strcasecmp(x, y);
    return r ? r : strcmp(x, y);
}

/* The visible entries of a directory, sorted; the volume's root also lists
 * the Host folder (as "/"). */
static int list_entries(const char *dir, bool files_only, char ***out)
{
    char **names = NULL;
    int count = 0, cap = 0;
    bool root = !strcmp(dir, root_path), host_root = !strcmp(dir, "/");
    as_ensure_dir(dir);
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        const char *name = entry_name(dir, e->d_name);
        if (!name)
            continue;
        char full[PATH_MAX];
        snprintf(full, sizeof full, "%s/%s", host_root ? "" : dir, name);
        if (files_only && is_dir(full))
            continue;
        if (!on_volume(full) && !visible(full))
            continue;
        /* The volume's own folder is the volume, not a folder inside Host:
         * listed in its host folder too (when a granted folder beside it,
         * such as the library's, makes that one visible) it closed a loop,
         * Macintosh HD:Host:Users:...:Blue Box:Macintosh HD.  Greg's Browser
         * adds up a folder's size by walking it, and went round the loop
         * until its thread's stack ran down over its code. */
        if (!strcmp(full, root_path) && !on_volume(dir))
            continue;
        if (root && !strcmp(name, host_folder))
            continue; /* the Host folder's name is taken */
        if (count == cap) {
            cap = cap ? cap * 2 : 32;
            names = realloc(names, (size_t)cap * sizeof *names);
        }
        names[count++] = strdup(name);
    }
    if (d)
        closedir(d);
    if (root && !files_only && ngrants) {
        if (count == cap)
            names = realloc(names, (size_t)(cap = cap ? cap * 2 : 32) * sizeof *names);
        names[count++] = strdup(host_folder);
    }
    if (count)
        qsort(names, (size_t)count, sizeof *names, cmp_names);
    *out = names;
    return count;
}

static bool nth_entry(const char *dir, int n, bool files_only, char *out, size_t outsize)
{
    char **names;
    int count = list_entries(dir, files_only, &names);
    bool ok = n >= 1 && n <= count;
    if (ok) {
        if (!strcmp(dir, root_path) && !strcmp(names[n - 1], host_folder) && ngrants)
            snprintf(out, outsize, "/");
        else
            snprintf(out, outsize, "%s/%s", strcmp(dir, "/") ? dir : "", names[n - 1]);
    }
    for (int i = 0; i < count; i++)
        free(names[i]);
    free(names);
    return ok;
}

static int count_entries(const char *dir)
{
    char **names;
    int count = list_entries(dir, false, &names);
    for (int i = 0; i < count; i++)
        free(names[i]);
    free(names);
    return count;
}

/* A host path's Mac name as a Pascal string (host memory, Str31). */
static void host_pname(uint8_t out[32], const char *host)
{
    const char *base = strrchr(host, '/');
    base = base ? base + 1 : host;
    if (!strcmp(host, "/"))
        base = host_folder;
    out[0] = (uint8_t)host_to_mac(base, out + 1);
}

static void put_name(gaddr namep, const char *host)
{
    if (!namep)
        return;
    const char *base = strrchr(host, '/');
    base = base ? base + 1 : host;
    if (!strcmp(host, "/"))
        base = host_folder;
    uint8_t mac[32];
    int n = host_to_mac(base, mac);
    wr8(namep, (uint8_t)n);
    gput(namep + 1, mac, (uint32_t)n);
}

/* ---------------------------------------------------------------------- */
/* Setup */

/* ---------------------------------------------------------------------- */
/* The FCB table and the volume's control block, as applications read them
 * directly (FCBSPtr, FSFCBLen, VCBQHdr).  An access path's refnum is its
 * FCB's offset in the table; the Resource Manager's files are included. */

static gaddr fcb_table, the_vcb;

static void guest_tables_init(void)
{
    gaddr zone = rd32(LM_SysZone);
    uint32_t size = 2 + FCB_SLOTS * FCB_LEN;
    fcb_table = mm_new_ptr_in(zone, size, true);
    wr16(fcb_table, (uint16_t)size);
    wr32(LM_FCBSPtr, fcb_table);
    wr16(LM_FSFCBLen, FCB_LEN);
    gaddr v = the_vcb = mm_new_ptr_in(zone, VCB_SIZEOF, true);
    wr16(v + VCB_vcbSigWord_off, 0x4244);
    uint32_t vc, vm;
    host_times(root_path, &vc, &vm);
    wr32(v + VCB_vcbCrDate_off, vc);
    wr32(v + VCB_vcbLsMod_off, vm);
    wr16(v + VCB_vcbNmAlBlks_off, 0xFFFF);
    wr32(v + VCB_vcbAlBlkSiz_off, 32768);
    wr32(v + VCB_vcbClpSiz_off, 32768);
    wr16(v + VCB_vcbFreeBks_off, 0xC000);
    gput(v + VCB_vcbVN_off, vol_name, (uint32_t)vol_name[0] + 1);
    wr16(v + VCB_vcbDrvNum_off, 1);
    wr16(v + VCB_vcbDRefNum_off, (uint16_t)-5);
    wr16(v + VCB_vcbVRefNum_off, (uint16_t)VREF);
    wr16(LM_VCBQHdr, 0);
    wr32(LM_VCBQHdr + 2, v);
    /* The mounted volumes follow, each its own VCB on its own drive. */
    gaddr last = v;
    for (int i = 0; i < nmounts; i++) {
        gaddr mv = mm_new_ptr_in(zone, VCB_SIZEOF, true);
        if (!mv)
            break;
        gmemcpy(mv, v, VCB_SIZEOF);
        wr32(mv, 0);
        host_times(mounts[i].root, &vc, &vm);
        wr32(mv + VCB_vcbCrDate_off, vc);
        wr32(mv + VCB_vcbLsMod_off, vm);
        wr16(mv + VCB_vcbAtrb_off, 0x8080); /* locked */
        gput(mv + VCB_vcbVN_off, mounts[i].name, (uint32_t)mounts[i].name[0] + 1);
        wr16(mv + VCB_vcbDrvNum_off, (uint16_t)(2 + i));
        wr16(mv + VCB_vcbDRefNum_off, (uint16_t)(-1 - CD_UNIT));
        wr16(mv + VCB_vcbVRefNum_off, (uint16_t)mounts[i].vref);
        wr32(last, mv); /* qLink */
        last = mv;
    }
    wr32(LM_VCBQHdr + 6, last);
    wr32(LM_DefVCBPtr, v);
    /* The drive queue: drive 1 holds the volume, and each mounted volume
     * has its drive too, on the CD driver (programs look up the drive a
     * volume reports and its driver, as Prince of Persia 2's Mohawk engine
     * does for its CD).  Four flag bytes precede an element: write-protected,
     * a disk in place (fixed, or ejectable for a mounted one), installed,
     * one side. */
    wr16(LM_DrvQHdr, 0);
    wr32(LM_DrvQHdr + 2, 0);
    wr32(LM_DrvQHdr + 6, 0);
    for (int i = -1; i < nmounts; i++) {
        gaddr dq = mm_new_ptr_in(zone, 4 + DrvQEl_SIZEOF, true);
        if (!dq)
            break;
        wr8(dq, i < 0 ? 0 : 0x80);
        wr8(dq + 1, i < 0 ? 8 : 1);
        wr8(dq + 2, 1);
        gaddr e = dq + 4;
        wr16(e + DrvQEl_qType_off, 1); /* dQDrvSz2 is valid */
        wr16(e + DrvQEl_dQDrive_off, (uint16_t)(2 + i));
        wr16(e + DrvQEl_dQRefNum_off, (uint16_t)(i < 0 ? -5 : -1 - CD_UNIT));
        wr16(e + DrvQEl_dQDrvSz_off, 0); /* 2 GB in 512-byte blocks */
        wr16(e + DrvQEl_dQDrvSz2_off, 0x40);
        if (rd32(LM_DrvQHdr + 6))
            wr32(rd32(LM_DrvQHdr + 6), e); /* qLink */
        else
            wr32(LM_DrvQHdr + 2, e);
        wr32(LM_DrvQHdr + 6, e);
    }
}

void fm_fcb_record(int16_t rn, const char *path, bool rsrc, bool write, uint32_t eof, uint32_t pos)
{
    if (!fcb_table || rn < 2 || (rn - 2) % FCB_LEN || rn >= 2 + FCB_SLOTS * FCB_LEN)
        return;
    gaddr f = fcb_table + (uint32_t)rn;
    gmemset(f, 0, FCB_LEN);
    wr32(f, (uint32_t)dir_id_for(rel_of(path)) + 0x10000); /* fcbFlNum: never 0 while open */
    wr8(f + 4, (uint8_t)((write ? 0x01 : 0) | (rsrc ? 0x02 : 0)));
    wr32(f + 8, eof);
    wr32(f + 12, (eof + 511) & ~511u);
    wr32(f + 16, pos);
    wr32(f + 20, the_vcb);
    wr32(f + 58, (uint32_t)parent_id_of(path));
    uint8_t name[32];
    host_pname(name, path);
    gput(f + 62, name, (uint32_t)name[0] + 1);
}

void fm_fcb_forget(int16_t rn)
{
    if (fcb_table && rn >= 2 && (rn - 2) % FCB_LEN == 0 && rn < 2 + FCB_SLOTS * FCB_LEN)
        gmemset(fcb_table + (uint32_t)rn, 0, FCB_LEN);
}

void fm_fcb_eof(int16_t rn, uint32_t eof)
{
    if (fcb_table && rn >= 2 && (rn - 2) % FCB_LEN == 0 && rn < 2 + FCB_SLOTS * FCB_LEN) {
        wr32(fcb_table + (uint32_t)rn + 8, eof);
        wr32(fcb_table + (uint32_t)rn + 12, (eof + 511) & ~511u);
    }
}

void fm_reset(void) { fcb_table = the_vcb = 0; }

static void fcb_sync(int16_t rn)
{
    fcb *f = fcb_for(rn);
    if (f && fcb_table) {
        fm_fcb_eof(rn, f->len);
        wr32(fcb_table + (uint32_t)rn + 16, f->pos);
    }
}

static const char *disk_path(void);
static const char *system_folder_path(void);
static void dt_reset(void);

void fm_init(const char *app_dir)
{
    /* The volume is its own folder; the application's folder (on it or
     * granted on the host) is the default directory. */
    char disk[PATH_MAX];
    if (!realpath(disk_path(), disk))
        snprintf(disk, sizeof disk, "%s", disk_path());
    snprintf(root_path, sizeof root_path, "%s", disk);
    for (int i = 0; i < ngrants; i++)
        free(grants[i].path);
    ngrants = 0;
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
    dt_reset();
    char app[PATH_MAX];
    if (!realpath(app_dir, app))
        snprintf(app, sizeof app, "%s", app_dir);
    fm_grant(app, true);
    char sys[PATH_MAX];
    if (realpath(system_folder_path(), sys) && !on_volume(sys))
        fm_grant(sys, true); /* set elsewhere (machine_set_system_folder) */
    /* Folders mounted as volumes of their own (machine_mount_folder). */
    nmounts = 0;
    for (int i = 0; i < npending && nmounts < MAX_MOUNTS; i++) {
        mount *m = &mounts[nmounts];
        if (!realpath(pending_mount[i], m->root))
            continue;
        fm_grant(m->root, true);
        m->vref = (int16_t)(-2 - nmounts);
        m->root_id = dir_id_for(rel_of(m->root));
        uint8_t mac[64];
        size_t n = utf8_to_macroman(pending_name[i][0] ? pending_name[i] : strrchr(m->root, '/') + 1, mac, 27);
        m->name[0] = (char)n;
        memcpy(m->name + 1, mac, n);
        nmounts++;
    }
    npending = 0; /* for this launch */
    default_dir = dir_id_for(rel_of(app));
    mount *am = mount_of_path(app);
    default_vref = am ? am->vref : VREF;
    static const char name[] = "Macintosh HD";
    vol_name[0] = (char)(sizeof name - 1);
    memcpy(vol_name + 1, name, sizeof name - 1);
    wr16(LM_DefVRefNum, (uint16_t)VREF);
    wr16(LM_SFSaveDisk, (uint16_t)(-VREF));
    wr32(LM_CurDirStore, (uint32_t)default_dir);
    wr16(LM_BootDrive, (uint16_t)fm_system_vref());
    guest_tables_init();
}

/* The canonical form of a host path that need not exist yet: its folder
 * resolved (symbolic links and all), then its name. */
static void canonical_path(const char *path, char *full, size_t size)
{
    if (realpath(path, full))
        return;
    char dir[PATH_MAX], rdir[PATH_MAX];
    parent_of(path, dir, sizeof dir);
    const char *base = strrchr(path, '/');
    if (base && realpath(dir, rdir))
        snprintf(full, size, "%s%s", strcmp(rdir, "/") ? rdir : "", base);
    else
        snprintf(full, size, "%s", path);
}

/* An FSSpec for a host path (the file need not exist). */
void fm_spec_for_path(const char *path, gaddr spec)
{
    char full[PATH_MAX];
    canonical_path(path, full, sizeof full);
    char parent[PATH_MAX];
    snprintf(parent, sizeof parent, "%s", full);
    char *slash = strrchr(parent, '/');
    if (slash)
        *slash = 0;
    wr16(spec + FSSpec_vRefNum_off, (uint16_t)vref_of_path(full));
    wr32(spec + FSSpec_parID_off, (uint32_t)out_id(dir_id_for(rel_of(parent))));
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
        /* The rest as Internet Config's standard mappings give them. */
        { "mod", "STrk", "SNPL", false }, { "xm", "XM  ", "SNPL", false }, { "s3m", "S3M ", "SNPL", false },
        { "mid", "Midi", "TVOD", false }, { "midi", "Midi", "TVOD", false },
        { "aif", "AIFF", "TVOD", false }, { "aiff", "AIFF", "TVOD", false }, { "aifc", "AIFC", "TVOD", false },
        { "wav", "WAVE", "TVOD", false }, { "au", "ULAW", "TVOD", false }, { "snd", "ULAW", "TVOD", false },
        { "mp3", "MPG3", "TVOD", false }, { "mov", "MooV", "TVOD", false }, { "qt", "MooV", "TVOD", false },
        { "mpg", "MPEG", "TVOD", false }, { "mpeg", "MPEG", "TVOD", false },
        { "gif", "GIFf", "ogle", false }, { "jpg", "JPEG", "ogle", false }, { "jpeg", "JPEG", "ogle", false },
        { "png", "PNGf", "ogle", false }, { "tif", "TIFF", "ogle", false }, { "tiff", "TIFF", "ogle", false },
        { "bmp", "BMPp", "ogle", false },
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
    if (implied_type(path, implied) != 2 || mf->rsrc_len || mf->comment_len)
        return false;
    if (!mf->has_finder_info)
        return true;
    bool untyped = !memcmp(mf->finder_info, "\0\0\0\0", 4);
    uint16_t flags = (uint16_t)(mf->finder_info[8] << 8 | mf->finder_info[9]);
    return (untyped || !memcmp(mf->finder_info, implied, 4)) && !(flags & ~0x0100u);
}

/* Classic type and creator for a host file without a meaningful sidecar,
 * from its extension (or from its sidecar's Finder info).  Returns true for
 * text kept as host text (UTF-8, host line ends): a text extension, no
 * resource fork, and a text type (TEXT or ttro), with or without a sidecar
 * holding Finder info such as the stationery flag. */
static bool host_file_type(const char *path, uint8_t finfo[16])
{
    uint8_t implied[16];
    int kind = implied_type(path, implied);
    char *side = appledouble_sidecar_path(path);
    bool has_side = (side && exists(side)) || appledouble_has_native(path);
    free(side);
    if (!has_side) {
        if (kind)
            memcpy(finfo, implied, 16);
        return kind == 2;
    }
    mac_file mf;
    char err[256];
    if (!appledouble_read(path, &mf, err, sizeof err))
        return false;
    bool typed = mf.has_finder_info && memcmp(mf.finder_info, "\0\0\0\0", 4);
    bool text_type = !typed || !memcmp(mf.finder_info, "TEXT", 4) || !memcmp(mf.finder_info, "ttro", 4);
    bool text = kind == 2 && mf.rsrc_len == 0 && text_type;
    if (text) {
        if (typed)
            memcpy(finfo, mf.finder_info, 16);
        else
            memcpy(finfo, implied, 16);
    }
    mac_file_free(&mf);
    return text;
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
    as_ensure_file(f->path); /* an AppleShare placeholder: its forks from the server */
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
    if (ok)
        as_file_changed(f->path); /* to the server, on an AppleShare volume */
    return ok ? noErr : ioErrF;
}

/* Open forks written out: what a program wrote is on disk by the time it
 * waits for its next event, as the Mac's disk cache had it there soon
 * after (a document saved and kept open, as SimpleText keeps it, isn't
 * lost if the run ends without the program closing it). */
static bool any_dirty;

void fm_flush_dirty(void)
{
    if (!any_dirty)
        return;
    any_dirty = false;
    for (int i = 0; i < MAX_FCBS; i++)
        if (fcbs[i].used && fcbs[i].dirty)
            save_fork(&fcbs[i]);
}


/* ---------------------------------------------------------------------- */
/* Opening and closing */

static bool hfs_call(void) { return (trap_current_word() & 0x0200) != 0; }

/* The data fork opened last (QuickTime takes a movie with no resource
 * fork from it). */
static char last_data_open[PATH_MAX];

bool fm_last_data_fork(char *out, size_t outsize)
{
    if (!last_data_open[0] || strlen(last_data_open) >= outsize)
        return false;
    strcpy(out, last_data_open);
    return true;
}

static int16_t open_fork(gaddr pb, bool rsrc, bool hfs)
{
    gaddr namep = rd32(pb + IOParam_ioNamePtr_off);
    /* Driver names start with '.': our own drivers, or none. */
    if (!rsrc && namep && rd8(namep) && rd8(namep + 1) == '.') {
        int16_t derr;
        return driver_open(pb, &derr) ? derr : fnfErrF;
    }
    char path[PATH_MAX];
    int16_t err = resolve((int16_t)rd16(pb + IOParam_ioVRefNum_off), hfs ? (int32_t)rd32(pb + HFileParam_ioDirID_off) : 0,
                          namep, hfs, path, sizeof path);
    LOGF(LOG_TRAPS, "  open %s fork (trap %04X): vref %d dir %d -> %s (%d)\n", rsrc ? "resource" : "data", trap_current_word(),
         (int16_t)rd16(pb + IOParam_ioVRefNum_off), hfs ? (int32_t)rd32(pb + HFileParam_ioDirID_off) : 0,
         err ? "?" : path, err);
    if (err)
        return err;
    if (!exists(path) || is_dir(path))
        return fnfErrF;
    uint8_t perm = rd8(pb + IOParam_ioPermssn_off);
    bool write = perm != fsRdPerm;
    if (write && !writable(path)) {
        if (perm == fsWrPerm)
            return permErr;
        write = false; /* read-write (or current) permission: read-only here */
    }
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
        host_pname(f->name, path);
        wr16(pb + IOParam_ioRefNum_off, (uint16_t)fcb_refnum(i));
        fm_fcb_record(fcb_refnum(i), path, rsrc, write, f->len, 0);
        if (!rsrc)
            snprintf(last_data_open, sizeof last_data_open, "%s", path);
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
    if (driver_refnum((int16_t)rd16(pb + IOParam_ioRefNum_off)))
        return driver_call(pb, async, DRIVER_CLOSE);
    fcb *f = fcb_for((int16_t)rd16(pb + IOParam_ioRefNum_off));
    if (!f)
        return rfNumErr;
    int16_t err = save_fork(f);
    free(f->buf);
    memset(f, 0, sizeof *f);
    fm_fcb_forget((int16_t)rd16(pb + IOParam_ioRefNum_off));
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

static int16_t pb_read(gaddr pb, bool async)
{
    fcb *f = fcb_readable((int16_t)rd16(pb + IOParam_ioRefNum_off));
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

static int16_t pb_write(gaddr pb, bool async)
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
    f->dirty = any_dirty = true;
    wr32(pb + IOParam_ioActCount_off, n);
    wr32(pb + IOParam_ioPosOffset_off, f->pos);
    return noErr;
}

int16_t tb_PBGetEOF(gaddr pb, bool async)
{
    fcb *f = fcb_readable((int16_t)rd16(pb + IOParam_ioRefNum_off));
    if (!f)
        return rfNumErr;
    wr32(pb + IOParam_ioMisc_off, f->len);
    return noErr;
}

static int16_t pb_set_eof(gaddr pb, bool async)
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
    f->dirty = any_dirty = true;
    return noErr;
}

int16_t tb_PBGetFPos(gaddr pb, bool async)
{
    fcb *f = fcb_readable((int16_t)rd16(pb + IOParam_ioRefNum_off));
    if (!f)
        return rfNumErr;
    wr32(pb + IOParam_ioReqCount_off, 0);
    wr32(pb + IOParam_ioActCount_off, 0);
    wr16(pb + IOParam_ioPosMode_off, 0);
    wr32(pb + IOParam_ioPosOffset_off, f->pos);
    return noErr;
}

static int16_t pb_set_fpos(gaddr pb, bool async)
{
    fcb *f = fcb_readable((int16_t)rd16(pb + IOParam_ioRefNum_off));
    if (!f)
        return rfNumErr;
    int16_t err = set_mark(f, rd16(pb + IOParam_ioPosMode_off), (int32_t)rd32(pb + IOParam_ioPosOffset_off));
    wr32(pb + IOParam_ioPosOffset_off, f->pos);
    return err;
}

static int16_t pb_allocate(gaddr pb, bool async)
{
    wr32(pb + IOParam_ioActCount_off, rd32(pb + IOParam_ioReqCount_off));
    return noErr;
}

int16_t tb_PBFlushFile(gaddr pb, bool async)
{
    int16_t rn = (int16_t)rd16(pb + IOParam_ioRefNum_off);
    fcb *f = fcb_for(rn);
    if (f)
        return save_fork(f);
    /* A resource file: the Resource Manager writes it (UpdateResFile). */
    const uint8_t *data;
    uint32_t len;
    return rm_fork_of(rn, &data, &len) ? noErr : rfNumErr;
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
    if (!writable(path))
        return wPrErr;
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
    if (!appledouble_write(path, &mf, err, sizeof err))
        return ioErrF;
    as_file_changed(path);
    return noErr;
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
    if (!writable(path) || !strcmp(path, "/"))
        return wPrErr;
    if (is_open(path))
        return fBsyErr;
    if (is_dir(path)) {
        if (rmdir(path) != 0)
            return fBsyErr;
        as_deleted(path);
        return noErr;
    }
    char *side = appledouble_sidecar_path(path);
    unlink(side);
    free(side);
    if (unlink(path) != 0)
        return ioErrF;
    as_deleted(path);
    return noErr;
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
    if (!writable(from) || !writable(to) || !strcmp(from, "/"))
        return wPrErr;
    if (rename(from, to) != 0)
        return ioErrF;
    char *s1 = appledouble_sidecar_path(from), *s2 = appledouble_sidecar_path(to);
    rename(s1, s2);
    free(s1);
    free(s2);
    as_moved(from, to);
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

/* Locking: the file must be there; the lock itself isn't kept (the host
 * file's permissions stay the program's to change). */
static int16_t lock_file(gaddr pb, bool hfs)
{
    char path[PATH_MAX];
    int16_t err = pb_path(pb, hfs, path, sizeof path);
    struct stat st;
    return err ? err : stat(path, &st) != 0 ? fnfErrF : noErr;
}

int16_t tb_PBSetFLock(gaddr pb, bool async) { return lock_file(pb, hfs_call()); }
int16_t tb_PBRstFLock(gaddr pb, bool async) { return lock_file(pb, hfs_call()); }

/* SetFLock / RstFLock(fileName, vRefNum): the glue's parameter block. */
static int16_t lock_by_name(gaddr name, int16_t vrn)
{
    gaddr pb = tb_NewPtr(IOParam_SIZEOF, false, true);
    if (!pb)
        return memFullErr;
    wr32(pb + IOParam_ioNamePtr_off, name);
    wr16(pb + IOParam_ioVRefNum_off, (uint16_t)vrn);
    int16_t err = lock_file(pb, false);
    tb_DisposePtr(pb);
    return err;
}

int16_t tb_SetFLock(gaddr filen, int16_t vrn) { return lock_by_name(filen, vrn); }
int16_t tb_RstFLock(gaddr filen, int16_t vrn) { return lock_by_name(filen, vrn); }

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
    uint32_t hc, hm;
    host_times(path, &hc, &hm);
    uint32_t c = ok && mf.create_date ? mf.create_date : hc, m = ok && mf.modify_date ? mf.modify_date : hm;
    wr32(pb + FileParam_ioFlCrDat_off, c);
    wr32(pb + FileParam_ioFlMdDat_off, m);
    if (cat) {
        wr32(pb + HFileInfo_ioFlBkDat_off, 0);
        wr32(pb + HFileInfo_ioFlParID_off, (uint32_t)out_id(parent_id_of(path)));
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
    if (!writable(path))
        return wPrErr;
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
    if (ok)
        as_file_changed(path);
    return ok ? noErr : ioErrF;
}

int16_t tb_PBSetFInfo(gaddr pb, bool async) { return set_finfo(pb, hfs_call(), false); }
int16_t tb_PBHSetFInfo(gaddr pb, bool async) { return set_finfo(pb, true, false); }

/* ---------------------------------------------------------------------- */
/* Volumes */

static mount *default_mount(void);

/* Which volume a GetVInfo call names: by index (1 the main volume, then
 * the mounted ones), by name (index < 0, "Name:" or "Name"), or by refnum
 * (0 the default, a working directory its folder's).  *m is NULL for the
 * main volume; false if there is no such volume. */
static bool vinfo_volume(int16_t idx, int16_t vref, gaddr namep, mount **m)
{
    *m = NULL;
    if (idx > 0) {
        if (idx == 1)
            return true;
        if (idx - 2 < nmounts) {
            *m = &mounts[idx - 2];
            return true;
        }
        return false;
    }
    if (idx < 0 && namep && rd8(namep)) {
        uint8_t name[256];
        int n = rd8(namep);
        gget(name, namep + 1, (uint32_t)n);
        const uint8_t *colon = memchr(name, ':', (size_t)n);
        if (colon)
            n = (int)(colon - name);
        if (n) {
            if (vol_name[0] == n && !mac_char_cmp(name, (const uint8_t *)vol_name + 1, n))
                return true;
            for (int k = 0; k < nmounts; k++)
                if (mounts[k].name[0] == n && !mac_char_cmp(name, (const uint8_t *)mounts[k].name + 1, n)) {
                    *m = &mounts[k];
                    return true;
                }
            return false;
        }
    }
    if (vref == 0) {
        *m = default_mount();
        return true;
    }
    if (vref == VREF || vref == 1)
        return true;
    if ((*m = mount_of_vref(vref)))
        return true;
    if (vref <= -32000 && vref > -32000 - MAX_WDS) {
        int32_t d = base_dir(vref, 0, false);
        char dpath[PATH_MAX];
        if (!d || !host_of_dir(d, dpath, sizeof dpath))
            return false;
        *m = mount_of_path(dpath);
        return true;
    }
    return false;
}

static int16_t get_vinfo(gaddr pb, bool hfs)
{
    int16_t idx = (int16_t)rd16(pb + VolumeParam_ioVolIndex_off);
    int16_t vref = (int16_t)rd16(pb + IOParam_ioVRefNum_off);
    gaddr namep = rd32(pb + IOParam_ioNamePtr_off);
    mount *m;
    if (!vinfo_volume(idx, vref, namep, &m))
        return nsvErr;
    const char *root = m ? m->root : root_path;
    LOGF(LOG_TRAPS, "  GetVInfo index %d vref %d -> %s\n", idx, vref, m ? m->root : root_path);
    if (namep)
        gput(namep, m ? m->name : vol_name, (uint32_t)(m ? m->name[0] : vol_name[0]) + 1);
    wr16(pb + IOParam_ioVRefNum_off, (uint16_t)(m ? m->vref : VREF));
    uint32_t vc, vm;
    host_times(root, &vc, &vm);
    wr32(pb + VolumeParam_ioVCrDate_off, vc);
    wr32(pb + HVolumeParam_ioVLsMod_off, vm);
    wr16(pb + VolumeParam_ioVAtrb_off, m ? 0x8080 : 0); /* mounted: locked, as a CD is */
    wr16(pb + VolumeParam_ioVNmFls_off, (uint16_t)count_entries(root));
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
        wr16(pb + HVolumeParam_ioVDrvInfo_off, (uint16_t)(m ? 2 + (m - mounts) : 1));
        wr16(pb + HVolumeParam_ioVDRefNum_off, (uint16_t)(m ? -1 - CD_UNIT : -5));
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

static int16_t wd_for(int32_t id);

/* The default directory as a working directory, as the Finder left it at
 * launch: apps open files next to themselves with GetVol's refnum. */
static int16_t default_wd(void)
{
    int16_t wd = default_dir == ROOT_ID ? VREF : wd_for(default_dir);
    return wd ? wd : default_vref;
}

/* The volume the default directory is on (NULL: the main one). */
static mount *default_mount(void)
{
    char dpath[PATH_MAX];
    return host_of_dir(default_dir, dpath, sizeof dpath) ? mount_of_path(dpath) : NULL;
}

int16_t tb_PBGetVol(gaddr pb, bool async)
{
    gaddr namep = rd32(pb + IOParam_ioNamePtr_off);
    mount *dm = default_mount();
    if (namep)
        gput(namep, dm ? dm->name : vol_name, (uint32_t)(dm ? dm->name[0] : vol_name[0]) + 1);
    wr16(pb + IOParam_ioVRefNum_off, (uint16_t)default_wd());
    return noErr;
}

int16_t tb_PBHGetVol(gaddr pb, bool async)
{
    tb_PBGetVol(pb, async);
    mount *dm = default_mount();
    wr16(pb + WDPBRec_ioWDVRefNum_off, (uint16_t)(dm ? dm->vref : VREF));
    wr32(pb + WDPBRec_ioWDDirID_off, (uint32_t)out_id(default_dir));
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

/* The working directory refnum for a directory ID (the volume for the root);
 * 0 when every working directory is in use. */
static int16_t wd_for(int32_t id)
{
    if (id == ROOT_ID)
        return VREF;
    if (mount_of_id(id))
        return mount_of_id(id)->vref;
    for (int i = 0; i < MAX_WDS; i++)
        if (wds[i].used && wds[i].dirid == id)
            return (int16_t)(-32000 - i);
    for (int i = 0; i < MAX_WDS; i++)
        if (!wds[i].used) {
            wds[i].used = true;
            wds[i].dirid = id;
            return (int16_t)(-32000 - i);
        }
    return 0;
}

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
    int16_t wd = wd_for(dir_id_for(rel_of(path)));
    if (!wd)
        return tmwdoErr;
    wr16(pb + IOParam_ioVRefNum_off, (uint16_t)wd);
    return noErr;
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
    char dpath[PATH_MAX];
    mount *dm = host_of_dir(d, dpath, sizeof dpath) ? mount_of_path(dpath) : NULL;
    wr16(pb + WDPBRec_ioWDVRefNum_off, (uint16_t)(dm ? dm->vref : VREF));
    wr32(pb + WDPBRec_ioWDDirID_off, (uint32_t)out_id(d));
    wr32(pb + WDPBRec_ioWDProcID_off, 0);
    gaddr namep = rd32(pb + IOParam_ioNamePtr_off);
    if (namep)
        gput(namep, dm ? dm->name : vol_name, (uint32_t)(dm ? dm->name[0] : vol_name[0]) + 1);
    return noErr;
}

static void cat_info_of_path(gaddr pb, const char *path);

static int16_t get_cat_info(gaddr pb, char *path)
{
    int16_t idx = (int16_t)rd16(pb + FileParam_ioFDirIndex_off);
    int32_t dirid = (int32_t)rd32(pb + HFileInfo_ioDirID_off);
    int16_t vref = (int16_t)rd16(pb + IOParam_ioVRefNum_off);
    gaddr namep = rd32(pb + IOParam_ioNamePtr_off);
    if (idx > 0) {
        char dir[PATH_MAX];
        int32_t d = base_dir(vref, dirid, true);
        if (!d || !host_of_dir(d, dir, sizeof dir))
            return dirNFErr;
        if (!nth_entry(dir, idx, false, path, PATH_MAX))
            return fnfErrF;
        put_name(namep, path);
    } else if (idx < 0) {
        if (dirid == 1)
            return fnfErrF; /* the root's parent has no record of its own */
        int32_t d = base_dir(vref, dirid, true);
        if (!d || !host_of_dir(d, path, PATH_MAX))
            return dirNFErr;
        if (namep) {
            if (d == ROOT_ID)
                gput(namep, vol_name, (uint32_t)vol_name[0] + 1);
            else if (mount_of_id(d))
                gput(namep, mount_of_id(d)->name, (uint32_t)mount_of_id(d)->name[0] + 1);
            else
                put_name(namep, path);
        }
    } else {
        int16_t err = resolve(vref, dirid, namep, true, path, PATH_MAX);
        if (err)
            return err;
        if (!exists(path))
            return fnfErrF;
    }
    cat_info_of_path(pb, path);
    return noErr;
}

/* A catalog record (CInfoPBRec) for a host path. */
static void cat_info_of_path(gaddr pb, const char *path)
{
    if (is_dir(path)) {
        int32_t id = dir_id_for(rel_of(path));
        wr8(pb + DirInfo_ioFlAttrib_off, 0x10);
        gmemset(pb + DirInfo_ioDrUsrWds_off, 0, 16);
        wr32(pb + DirInfo_ioDrDirID_off, (uint32_t)out_id(id));
        wr16(pb + DirInfo_ioDrNmFls_off, (uint16_t)count_entries(path));
        uint32_t dc, dm;
        host_times(path, &dc, &dm);
        wr32(pb + DirInfo_ioDrCrDat_off, dc);
        wr32(pb + DirInfo_ioDrMdDat_off, dm);
        wr32(pb + DirInfo_ioDrBkDat_off, 0);
        gmemset(pb + DirInfo_ioDrFndrInfo_off, 0, 16);
        wr32(pb + DirInfo_ioDrParID_off, is_root_id(id) ? 1 : (uint32_t)out_id(parent_id_of(path)));
    } else {
        fill_file_info(pb, path, true);
        wr32(pb + HFileInfo_ioDirID_off, rd32(pb + FileParam_ioFlNum_off));
    }
}

int16_t tb_PBGetCatInfo(gaddr pb, bool async)
{
    char path[PATH_MAX] = "";
    int16_t idx = (int16_t)rd16(pb + FileParam_ioFDirIndex_off);
    int32_t dirid = (int32_t)rd32(pb + HFileInfo_ioDirID_off);
    int16_t vref = (int16_t)rd16(pb + IOParam_ioVRefNum_off);
    int16_t err = get_cat_info(pb, path);
    LOGF(LOG_TRAPS, "  PBGetCatInfo vref %d dir %d index %d -> %d %s\n", vref, dirid, idx, err, path);
    return err;
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
    if (!writable(path))
        return wPrErr;
    if (mkdir(path, 0755) != 0)
        return ioErrF;
    as_dir_created(path);
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
    snprintf(dest, sizeof dest, "%s%s", strcmp(to, "/") ? to : "", base);
    if (!writable(from) || !writable(dest) || !strcmp(from, "/"))
        return wPrErr;
    if (rename(from, dest) != 0)
        return ioErrF;
    char *s1 = appledouble_sidecar_path(from), *s2 = appledouble_sidecar_path(dest);
    rename(s1, s2);
    free(s1);
    free(s2);
    as_moved(from, dest);
    return noErr;
}

/* Move a file or folder into the folder the destination FSSpec names. */
int16_t tb_FSpCatMove(gaddr src, gaddr dst)
{
    if (rd16(src + FSSpec_vRefNum_off) != rd16(dst + FSSpec_vRefNum_off))
        return badMovErr; /* only within a volume, as PBCatMove */
    gaddr pb = tb_NewPtr(CInfoPBRec_SIZEOF, false, true);
    wr32(pb + IOParam_ioNamePtr_off, src + FSSpec_name_off);
    wr16(pb + IOParam_ioVRefNum_off, rd16(src + FSSpec_vRefNum_off));
    wr32(pb + HFileParam_ioDirID_off, rd32(src + FSSpec_parID_off));
    wr32(pb + 28, dst + FSSpec_name_off);         /* ioNewName */
    wr32(pb + 36, rd32(dst + FSSpec_parID_off)); /* ioNewDirID */
    int16_t err = tb_PBCatMove(pb, false);
    tb_DisposePtr(pb);
    return err;
}

int16_t tb_PBGetFCBInfo(gaddr pb, bool async)
{
    /* Every open access path, resource files included, from the FCB table. */
    int16_t idx = (int16_t)rd16(pb + FCBPBRec_ioFCBIndx_off);
    int16_t rn = 0;
    if (idx > 0) {
        int n = 0;
        for (int i = 0; i < FCB_SLOTS && !rn; i++)
            if (rd32(fcb_table + 2 + (uint32_t)i * FCB_LEN) && ++n == idx)
                rn = (int16_t)(2 + i * FCB_LEN);
        if (!rn)
            return fnOpnErr;
    } else {
        rn = (int16_t)rd16(pb + IOParam_ioRefNum_off);
        if (rn < 2 || (rn - 2) % FCB_LEN || rn >= 2 + FCB_SLOTS * FCB_LEN || !rd32(fcb_table + (uint32_t)rn))
            return rfNumErr;
    }
    gaddr f = fcb_table + (uint32_t)rn;
    wr16(pb + IOParam_ioRefNum_off, (uint16_t)rn);
    gaddr namep = rd32(pb + IOParam_ioNamePtr_off);
    if (namep)
        gmemcpy(namep, f + 62, (uint32_t)rd8(f + 62) + 1);
    wr32(pb + FCBPBRec_ioFCBFlNm_off, rd32(f));
    wr16(pb + FCBPBRec_ioFCBFlags_off, (uint16_t)(rd8(f + 4) << 8));
    wr16(pb + FCBPBRec_ioFCBStBlk_off, 0);
    wr32(pb + FCBPBRec_ioFCBEOF_off, rd32(f + 8));
    wr32(pb + FCBPBRec_ioFCBPLen_off, rd32(f + 12));
    wr32(pb + FCBPBRec_ioFCBCrPs_off, rd32(f + 16));
    int32_t par = (int32_t)rd32(f + 58);
    char ppath[PATH_MAX];
    mount *fm_ = host_of_dir(par, ppath, sizeof ppath) ? mount_of_path(ppath) : NULL;
    wr16(pb + FCBPBRec_ioFCBVRefNum_off, (uint16_t)(fm_ ? fm_->vref : VREF));
    wr32(pb + FCBPBRec_ioFCBClpSiz_off, 0);
    wr32(pb + FCBPBRec_ioFCBParID_off, (uint32_t)out_id(par));
    LOGF(LOG_TRAPS, "  PBGetFCBInfo refnum %d -> vref %d parID %d name %.*s\n", rn, (int16_t)rd16(pb + FCBPBRec_ioFCBVRefNum_off),
         out_id(par), rd8(f + 62), (const char *)gptr(f + 63));
    return noErr;
}

/* Device Manager calls on files or absent drivers. */
int16_t tb_PBControl(gaddr pbp, bool a)
{
    return driver_refnum((int16_t)rd16(pbp + IOParam_ioRefNum_off)) ? driver_call(pbp, a, DRIVER_CONTROL) : controlErr;
}
int16_t tb_PBStatus(gaddr pbp, bool a)
{
    return driver_refnum((int16_t)rd16(pbp + IOParam_ioRefNum_off)) ? driver_call(pbp, a, DRIVER_STATUS) : statusErr;
}
int16_t tb_PBKillIO(gaddr pbp, bool a)
{
    return driver_refnum((int16_t)rd16(pbp + IOParam_ioRefNum_off)) ? driver_call(pbp, a, DRIVER_KILLIO) : noErr;
}

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
    mount *pm = mount_of_path(path);
    if (pm && !strcmp(pm->root, path)) { /* a mounted volume's root */
        wr16(spec + FSSpec_vRefNum_off, (uint16_t)pm->vref);
        wr32(spec + FSSpec_parID_off, 1);
        gput(spec + FSSpec_name_off, pm->name, (uint32_t)pm->name[0] + 1);
        return noErr;
    }
    wr16(spec + FSSpec_vRefNum_off, (uint16_t)vref_of_path(path));
    wr32(spec + FSSpec_parID_off, at_root ? 1 : (uint32_t)out_id(dir_id_for(rel_of(parent))));
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

/* FSpExchangeFiles(src, dst): the two files trade contents, both forks and
 * the dates; each keeps its name and Finder info (programs save to a new
 * file and exchange it with the old: Internet Explorer). */
int16_t tb_FSpExchangeFiles(gaddr src, gaddr dst)
{
    char a[PATH_MAX], b[PATH_MAX];
    int16_t err = pb_path(spec_pb(src), true, a, sizeof a);
    if (!err)
        err = pb_path(spec_pb(dst), true, b, sizeof b);
    if (err)
        return err;
    mac_file fa, fb;
    char msg[256];
    if (!appledouble_read(a, &fa, msg, sizeof msg))
        return fnfErr;
    if (!appledouble_read(b, &fb, msg, sizeof msg)) {
        mac_file_free(&fa);
        return fnfErr;
    }
    mac_file na = fb, nb = fa; /* contents and dates traded */
    memcpy(na.finder_info, fa.finder_info, sizeof na.finder_info);
    na.has_finder_info = fa.has_finder_info;
    memcpy(nb.finder_info, fb.finder_info, sizeof nb.finder_info);
    nb.has_finder_info = fb.has_finder_info;
    /* Comments too: the desktop database has them by name. */
    memcpy(na.comment, fa.comment, sizeof na.comment);
    na.comment_len = fa.comment_len;
    memcpy(nb.comment, fb.comment, sizeof nb.comment);
    nb.comment_len = fb.comment_len;
    bool ok = appledouble_write(a, &na, msg, sizeof msg) && appledouble_write(b, &nb, msg, sizeof msg);
    mac_file_free(&fa);
    mac_file_free(&fb);
    if (ok) {
        as_file_changed(a);
        as_file_changed(b);
    }
    return ok ? noErr : ioErr;
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
    canonical_path(path, full, sizeof full);
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
    memcpy(out, vol_name + 1, n = (size_t)vol_name[0]);
    char rel[PATH_MAX + 16];
    snprintf(rel, sizeof rel, "%s", rel_of(full));
    if (rel[0] == HOST_MARK) /* Host:... */
        snprintf(rel, sizeof rel, "%s%s", host_folder, strcmp(full, "/") ? full : "");
    const char *p = rel;
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
    if (n == (size_t)vol_name[0]) { /* the volume itself */
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
        if (i > start && !strcmp(path, root_path) && i - start == strlen(host_folder) &&
            !mac_char_cmp(mac + start, (const uint8_t *)host_folder, (int)(i - start))) {
            snprintf(path, sizeof path, "/");
        } else if (i > start) {
            char comp[256];
            host_component(path, mac + start, (int)(i - start), comp, sizeof comp);
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

/* ---------------------------------------------------------------------- */
/* The System Folder and FindFolder
 *
 * A per-user host folder stands in for the System Folder, so preferences
 * and other files apps keep there persist between runs. */

static char sys_folder[PATH_MAX];

void machine_set_system_folder(const char *path)
{
    snprintf(sys_folder, sizeof sys_folder, "%s", path ? path : "");
}

static bool make_dirs(const char *path)
{
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            mkdir(p, 0755);
            *s = '/';
        }
    return mkdir(p, 0755) == 0 || is_dir(p);
}

/* Our folder in the user's data: Application Support/Blue Box on macOS. */
static void data_folder(char *out, size_t outsize)
{
    const char *home = getenv("HOME");
#ifdef __APPLE__
    snprintf(out, outsize, "%s/Library/Application Support/Blue Box", home ? home : "/tmp");
#else
    const char *data = getenv("XDG_DATA_HOME");
    if (data && *data)
        snprintf(out, outsize, "%s/bluebox", data);
    else
        snprintf(out, outsize, "%s/.local/share/bluebox", home ? home : "/tmp");
#endif
}

static char disk_folder[PATH_MAX];

void machine_set_disk(const char *path) { snprintf(disk_folder, sizeof disk_folder, "%s", path ? path : ""); }

/* The volume's folder ("Macintosh HD"), made on first use. */
static const char *disk_path(void)
{
    if (!disk_folder[0]) {
        char base[PATH_MAX];
        data_folder(base, sizeof base);
        snprintf(disk_folder, sizeof disk_folder, "%s/Macintosh HD", base);
    }
    make_dirs(disk_folder);
    return disk_folder;
}

/* The System Folder is on the volume.  One kept beside it by earlier
 * versions moves in. */
const char *fm_system_folder_host(void);

static const char *system_folder_path(void)
{
    if (!sys_folder[0]) {
        snprintf(sys_folder, sizeof sys_folder, "%s/System Folder", disk_path());
        char base[PATH_MAX], old[PATH_MAX];
        data_folder(base, sizeof base);
        snprintf(old, sizeof old, "%s/System Folder", base);
        if (!is_dir(sys_folder) && is_dir(old))
            rename(old, sys_folder);
    }
    make_dirs(sys_folder);
    return sys_folder;
}

/* The System Folder's working directory, as SysEnvirons and BootDrive give it. */
int16_t fm_system_vref(void)
{
    char full[PATH_MAX];
    if (!realpath(system_folder_path(), full))
        return VREF;
    int16_t wd = wd_for(dir_id_for(rel_of(full)));
    return wd ? wd : VREF;
}

static const struct {
    char type[5];
    const char *name; /* inside the System Folder; NULL for the System Folder itself */
} folders[] = {
    { "macs", NULL },
    { "pref", "Preferences" },
    { "extn", "Extensions" },
    { "ctrl", "Control Panels" },
    { "font", "Fonts" },
    { "strt", "Startup Items" },
    { "shdf", "Shutdown Items" },
    { "amnu", "Apple Menu Items" },
    { "prnt", "PrintMonitor Documents" },
    { "temp", "Temporary Items" },
    { "flnt", "Temporary Items" }, /* kChewableItemsFolderType */
    { "trsh", "Trash" },
    { "empt", "Trash" },
    { "asup", "Application Support" },
    { "appr", "Appearance" },
    { "cmnu", "Contextual Menu Items" },
    { "ctrb", "Control Strip Modules" },
    { "lang", "Language & Region Support" },
    { "info", "Help" },
    { "fasf", "Scripting Additions" },
    { "\xC4" "scr", "Scripts" }, /* 'ƒscr' */
};

int16_t tb_FindFolder(int16_t vRefNum, int32_t folderType, int8_t createFolder, gaddr foundVRefNum,
                      gaddr foundDirID)
{
    char path[PATH_MAX];
    bool found = false;
    if ((uint32_t)folderType == 0x6465736B /* 'desk' */) {
        snprintf(path, sizeof path, "%s/Desktop Folder", disk_path());
        found = true;
    }
    for (size_t i = 0; i < sizeof folders / sizeof folders[0] && !found; i++) {
        const uint8_t *t = (const uint8_t *)folders[i].type;
        uint32_t code = (uint32_t)t[0] << 24 | (uint32_t)t[1] << 16 | (uint32_t)t[2] << 8 | t[3];
        if (code != (uint32_t)folderType)
            continue;
        if (folders[i].name)
            snprintf(path, sizeof path, "%s/%s", system_folder_path(), folders[i].name);
        else
            snprintf(path, sizeof path, "%s", system_folder_path());
        found = true;
    }
    if (!found)
        return fnfErrF;
    /* The standard folders always exist, as on a working System. */
    if (!is_dir(path) && !make_dirs(path))
        return fnfErrF;
    char full[PATH_MAX];
    if (!realpath(path, full))
        return fnfErrF;
    if (foundVRefNum)
        wr16(foundVRefNum, (uint16_t)VREF);
    if (foundDirID)
        wr32(foundDirID, (uint32_t)dir_id_for(rel_of(full)));
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* Volume parameters: an HFS volume with no optional features. */

int16_t tb_PBHGetVolParms(gaddr pb, bool async)
{
    int16_t vref = (int16_t)rd16(pb + IOParam_ioVRefNum_off);
    if (vref != 0 && vref != VREF && vref != 1 && !(vref <= -32000 && vref > -32000 - MAX_WDS))
        return nsvErr;
    gaddr buf = rd32(pb + HIoParam_ioBuffer_off);
    int32_t want = (int32_t)rd32(pb + HIoParam_ioReqCount_off);
    uint8_t info[20] = { 0, 2 }; /* vMVersion 2, vMAttrib 0, no local handle or server */
    int32_t n = want < (int32_t)sizeof info ? want : (int32_t)sizeof info;
    if (n < 0)
        n = 0;
    if (buf)
        gput(buf, info, (uint32_t)n);
    wr32(pb + HIoParam_ioActCount_off, (uint32_t)n);
    return noErr;
}

int16_t tb_PBOpenDF(gaddr pb, bool async) { return open_fork(pb, false, hfs_call()); }

int16_t tb_PBRead(gaddr pb, bool async)
{
    if (driver_refnum((int16_t)rd16(pb + IOParam_ioRefNum_off)))
        return driver_call(pb, async, DRIVER_READ);
    int16_t err = pb_read(pb, async);
    fcb_sync((int16_t)rd16(pb + IOParam_ioRefNum_off));
    return err;
}

int16_t tb_PBWrite(gaddr pb, bool async)
{
    if (driver_refnum((int16_t)rd16(pb + IOParam_ioRefNum_off)))
        return driver_call(pb, async, DRIVER_WRITE);
    int16_t err = pb_write(pb, async);
    fcb_sync((int16_t)rd16(pb + IOParam_ioRefNum_off));
    return err;
}

int16_t tb_PBSetEOF(gaddr pb, bool async)
{
    int16_t err = pb_set_eof(pb, async);
    fcb_sync((int16_t)rd16(pb + IOParam_ioRefNum_off));
    return err;
}

int16_t tb_PBSetFPos(gaddr pb, bool async)
{
    int16_t err = pb_set_fpos(pb, async);
    fcb_sync((int16_t)rd16(pb + IOParam_ioRefNum_off));
    return err;
}

int16_t tb_PBAllocate(gaddr pb, bool async)
{
    int16_t err = pb_allocate(pb, async);
    fcb_sync((int16_t)rd16(pb + IOParam_ioRefNum_off));
    return err;
}

/* There is one volume, and it stays mounted. */
int16_t tb_PBMountVol(gaddr pb) { return -56; /* nsDrvErr: no such drive */ }
int16_t tb_PBUnmountVol(gaddr pb) { return fBsyErr; }
int16_t tb_PBEject(gaddr pb) { return -56; }
int16_t tb_PBOffLine(gaddr pb) { return -56; }

/* DiskEject(drive): the drives are fixed disks, which don't eject. */
int16_t tb_DiskEject(int16_t rn)
{
    for (gaddr e = rd32(LM_DrvQHdr + 2); e; e = rd32(e + DrvQEl_qLink_off))
        if ((int16_t)rd16(e + DrvQEl_dQDrive_off) == rn)
            return -17; /* controlErr */
    return -56;         /* nsDrvErr */
}

/* Opening with deny modes (AppleShare): the read and write access bits
 * become the permission; there is no one else to deny. */
static int16_t open_deny(gaddr pb, bool rsrc)
{
    uint16_t deny = rd16(pb + 26); /* ioDenyModes */
    uint8_t saved = rd8(pb + IOParam_ioPermssn_off);
    wr8(pb + IOParam_ioPermssn_off, (uint8_t)((deny & 3) ? (deny & 3) : fsRdPerm));
    int16_t err = open_fork(pb, rsrc, true);
    wr8(pb + IOParam_ioPermssn_off, saved);
    return err;
}

int16_t tb_PBHOpenDeny(gaddr pb, bool async) { return open_deny(pb, false); }
int16_t tb_PBHOpenRFDeny(gaddr pb, bool async) { return open_deny(pb, true); }

/* Access privileges are an AppleShare matter; the local volume has none. */
int16_t tb_PBHGetDirAccess(gaddr pb, bool async) { return paramErr; }

/* Volume dates, attributes and Finder information: the volume's are its
 * folder's, and nothing here is kept. */
int16_t tb_PBSetVInfo(gaddr pb, bool async) { return noErr; }

/* ---------------------------------------------------------------------- */
/* The Desktop Manager (PBDT calls).  Each volume has a desktop database,
 * its refnum the volume's negated: the applications on it by creator, for
 * opening documents; the icons of their bundles (BNDL, FREF and the icon
 * resources they name), read from an application when its creator is
 * first asked about, with the icons programs add kept beside them for the
 * run; and comments, a file's in its AppleDouble sidecar as the Finder's
 * comment entry, a folder's (which has no sidecar) for the run. */

enum { afpItemNotFound = -5018, afpIconTypeError = -5030 };

/* The Finder information in a file's AppleDouble sidecar, read without
 * loading the forks. */
static bool sidecar_finfo(const char *path, uint8_t out[16])
{
    char *side = appledouble_sidecar_path(path);
    FILE *fp = side ? fopen(side, "rb") : NULL;
    free(side);
    if (!fp) { /* none: the file's own (macOS) */
        uint8_t fi[32];
        if (!appledouble_native_finder_info(path, fi) || !memcmp(fi, "\0\0\0\0", 4))
            return false;
        memcpy(out, fi, 16);
        return true;
    }
    uint8_t h[26 + 12 * 16];
    size_t n = fread(h, 1, sizeof h, fp);
    bool ok = false;
    if (n >= 26) {
        int count = h[24] << 8 | h[25];
        for (int i = 0; i < count && 26 + 12 * (size_t)i + 12 <= n; i++) {
            const uint8_t *e = h + 26 + 12 * i;
            uint32_t id = (uint32_t)e[0] << 24 | (uint32_t)e[1] << 16 | (uint32_t)e[2] << 8 | e[3];
            uint32_t off = (uint32_t)e[4] << 24 | (uint32_t)e[5] << 16 | (uint32_t)e[6] << 8 | e[7];
            if (id == 9 && fseek(fp, (long)off, SEEK_SET) == 0 && fread(out, 1, 16, fp) == 16)
                ok = true;
        }
    }
    fclose(fp);
    return ok;
}

static struct {
    uint32_t creator;
    char *path;
} *appls;
static int nappls;
static bool appls_scanned;

static void scan_appls(const char *dir, int depth)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char full[PATH_MAX];
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        if (is_dir(full)) {
            if (depth < 5)
                scan_appls(full, depth + 1);
            continue;
        }
        uint8_t fi[16];
        if (!sidecar_finfo(full, fi) || memcmp(fi, "APPL", 4))
            continue;
        appls = realloc(appls, (size_t)(nappls + 1) * sizeof *appls);
        appls[nappls].creator = (uint32_t)fi[4] << 24 | (uint32_t)fi[5] << 16 | (uint32_t)fi[6] << 8 | fi[7];
        appls[nappls].path = strdup(full);
        nappls++;
    }
    closedir(d);
}

/* Mounted volumes' roots are granted trees, so their applications are
 * found with the host's. */
static void find_appls(void)
{
    if (appls_scanned)
        return;
    appls_scanned = true;
    scan_appls(root_path, 0);
    for (int i = 0; i < ngrants; i++)
        if (grants[i].kind == GRANT_TREE)
            scan_appls(grants[i].path, 0);
}

/* A desktop database's volume.  A refnum that names none is taken for the
 * main volume's, as every database was one before. */
static int16_t dt_vref(gaddr dtp)
{
    int16_t vref = (int16_t)-(int16_t)rd16(dtp + DTPBRec_ioDTRefNum_off);
    return mount_of_vref(vref) ? vref : VREF;
}

int16_t tb_PBDTGetPath(gaddr dtp)
{
    gaddr namep = rd32(dtp + DTPBRec_ioNamePtr_off);
    int16_t vref = (int16_t)rd16(dtp + DTPBRec_ioVRefNum_off);
    mount *m;
    if (!(namep && rd8(namep) && vinfo_volume(-1, vref, namep, &m)) && !vinfo_volume(0, vref, 0, &m))
        return nsvErr;
    wr16(dtp + DTPBRec_ioDTRefNum_off, (uint16_t)-(m ? m->vref : VREF));
    return noErr;
}

int16_t tb_PBDTOpenInform(gaddr dtp)
{
    int16_t err = tb_PBDTGetPath(dtp);
    if (!err)
        wr32(dtp + DTPBRec_ioTagInfo_off, 0); /* not newly created */
    return err;
}

int16_t tb_PBDTCloseDown(gaddr dtp) { return noErr; }

/* The index-th (1-based; 0 the first) application with a creator on the
 * database's volume. */
int16_t tb_PBDTGetAPPL(gaddr dtp, bool async)
{
    find_appls();
    uint32_t creator = rd32(dtp + DTPBRec_ioFileCreator_off);
    int16_t vref = dt_vref(dtp);
    int want = (int16_t)rd16(dtp + DTPBRec_ioIndex_off);
    if (want < 1)
        want = 1;
    for (int i = 0, k = 0; i < nappls; i++) {
        if (appls[i].creator != creator || vref_of_path(appls[i].path) != vref || ++k != want)
            continue;
        if (!exists(appls[i].path))
            break;
        put_name(rd32(dtp + DTPBRec_ioNamePtr_off), appls[i].path);
        wr32(dtp + DTPBRec_ioAPPLParID_off, (uint32_t)parent_id_of(appls[i].path));
        wr32(dtp + DTPBRec_ioTagInfo_off, 0);
        return noErr;
    }
    return afpItemNotFound;
}

static int ndt_read;

/* The applications looked for again next time, and their bundles read
 * again (adding only the icons not there). */
static void forget_appls(void)
{
    appls_scanned = false;
    for (int i = 0; i < nappls; i++)
        free(appls[i].path);
    nappls = 0;
    ndt_read = 0;
}

int16_t tb_PBDTAddAPPL(gaddr dtp, bool async)
{
    forget_appls();
    return noErr;
}

bool fm_app_for_document(const char *doc, char *app, size_t appsize)
{
    uint8_t fi[16] = { 0 };
    if (!sidecar_finfo(doc, fi) && !host_file_type(doc, fi))
        return false;
    uint32_t creator = (uint32_t)fi[4] << 24 | (uint32_t)fi[5] << 16 | (uint32_t)fi[6] << 8 | fi[7];
    return fm_app_for_creator(creator, app, appsize);
}

bool fm_app_for_creator(uint32_t creator, char *app, size_t appsize)
{
    if (!creator)
        return false;
    find_appls();
    for (int i = 0; i < nappls; i++)
        if (appls[i].creator == creator && exists(appls[i].path) && strlen(appls[i].path) < appsize) {
            strcpy(app, appls[i].path);
            return true;
        }
    return false;
}

/* ---------------------------------------------------------------------- */
/* PBCatSearch: the volume's files and folders matching ioSearchInfo1 (values)
 * and ioSearchInfo2 (masks and upper bounds) under ioSearchBits, up to
 * ioReqMatchCount at a time; ioCatPosition says where the next call goes on,
 * and eofErr ends the search.  The Host folder (the whole host disk) is
 * not searched. */

enum {
    fsSBPartialName = 1, fsSBFullName = 2, fsSBFlAttrib = 4, fsSBFlFndrInfo = 8, fsSBFlLgLen = 32,
    fsSBFlPyLen = 64, fsSBFlRLgLen = 128, fsSBFlRPyLen = 256, fsSBFlCrDat = 512, fsSBFlMdDat = 1024,
    fsSBFlBkDat = 2048, fsSBFlXFndrInfo = 4096, fsSBFlParID = 8192, fsSBNegate = 16384,
    fsSBDrNmFls = 16, fsSBDrUsrWds = 8,
};
enum { CS_MatchPtr = 24, CS_ReqCount = 28, CS_ActCount = 32, CS_Bits = 36, CS_Info1 = 40, CS_Info2 = 44,
       CS_Position = 52 };
enum { CI_Attrib = 30, CI_FndrInfo = 32, CI_LgLen = 54, CI_PyLen = 58, CI_RLgLen = 64, CI_RPyLen = 68,
       CI_CrDat = 72, CI_MdDat = 76, CI_BkDat = 80, CI_XFndrInfo = 84, CI_ParID = 100, CI_DrNmFls = 52 };

static bool in_range(gaddr rec, gaddr lo, gaddr hi, uint32_t off)
{
    uint32_t v = rd32(rec + off);
    return v >= rd32(lo + off) && v <= rd32(hi + off);
}

static bool masked_equal(gaddr rec, gaddr val, gaddr mask, uint32_t off, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if ((rd8(rec + off + i) ^ rd8(val + off + i)) & rd8(mask + off + i))
            return false;
    return true;
}

static bool cat_matches(gaddr rec, const uint8_t *name, int nlen, gaddr i1, gaddr i2, uint32_t bits)
{
    bool folder = rd8(rec + CI_Attrib) & 0x10;
    bool ok = true;
    if (bits & (fsSBPartialName | fsSBFullName)) {
        gaddr np = rd32(i1 + IOParam_ioNamePtr_off);
        int want = np ? rd8(np) : 0;
        uint8_t w[256];
        if (np)
            gget(w, np + 1, (uint32_t)want);
        if (bits & fsSBFullName) {
            ok = want == nlen && !mac_char_cmp(w, name, nlen);
        } else {
            ok = false;
            for (int k = 0; k + want <= nlen && !ok; k++)
                ok = !mac_char_cmp(w, name + k, want);
        }
    }
    if (ok && (bits & fsSBFlAttrib))
        ok = !((rd8(rec + CI_Attrib) ^ rd8(i1 + CI_Attrib)) & rd8(i2 + CI_Attrib));
    if (ok && (bits & fsSBFlFndrInfo))
        ok = masked_equal(rec, i1, i2, CI_FndrInfo, 16);
    if (ok && (bits & fsSBFlXFndrInfo))
        ok = masked_equal(rec, i1, i2, CI_XFndrInfo, 16);
    if (!folder) {
        if (ok && (bits & fsSBFlLgLen))
            ok = in_range(rec, i1, i2, CI_LgLen);
        if (ok && (bits & fsSBFlPyLen))
            ok = in_range(rec, i1, i2, CI_PyLen);
        if (ok && (bits & fsSBFlRLgLen))
            ok = in_range(rec, i1, i2, CI_RLgLen);
        if (ok && (bits & fsSBFlRPyLen))
            ok = in_range(rec, i1, i2, CI_RPyLen);
    } else if (ok && (bits & fsSBDrNmFls)) {
        uint16_t v = rd16(rec + CI_DrNmFls);
        ok = v >= rd16(i1 + CI_DrNmFls) && v <= rd16(i2 + CI_DrNmFls);
    }
    if (ok && (bits & fsSBFlCrDat))
        ok = in_range(rec, i1, i2, CI_CrDat);
    if (ok && (bits & fsSBFlMdDat))
        ok = in_range(rec, i1, i2, CI_MdDat);
    if (ok && (bits & fsSBFlBkDat))
        ok = in_range(rec, i1, i2, CI_BkDat);
    if (ok && (bits & fsSBFlParID))
        ok = in_range(rec, i1, i2, CI_ParID);
    return (bits & fsSBNegate) ? !ok : ok;
}

typedef struct {
    gaddr pb, rec, i1, i2, matches, name;
    uint32_t bits, want, found, skip, seen;
} cat_search;

/* Depth first over a folder: false once enough matches are found. */
static bool search_dir(cat_search *cs, const char *dir, int depth)
{
    as_ensure_dir(dir);
    DIR *d = opendir(dir);
    if (!d || depth > 64) {
        if (d)
            closedir(d);
        return true;
    }
    struct dirent *e;
    bool go_on = true;
    while (go_on && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue; /* dot files and AppleDouble sidecars */
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (cs->seen++ >= cs->skip) {
            gmemset(cs->rec, 0, 108);
            cat_info_of_path(cs->rec, path);
            put_name(cs->name, path);
            uint8_t name[256];
            int n = rd8(cs->name);
            gget(name, cs->name + 1, (uint32_t)n);
            if (cat_matches(cs->rec, name, n, cs->i1, cs->i2, cs->bits)) {
                fm_spec_for_path(path, cs->matches + cs->found * FSSpec_SIZEOF);
                cs->found++;
                if (cs->found >= cs->want)
                    go_on = false;
            }
        }
        if (go_on && is_dir(path))
            go_on = search_dir(cs, path, depth + 1);
    }
    closedir(d);
    return go_on;
}

int16_t tb_PBCatSearch(gaddr pb, bool async)
{
    gaddr matches = rd32(pb + CS_MatchPtr), i1 = rd32(pb + CS_Info1), i2 = rd32(pb + CS_Info2);
    uint32_t want = rd32(pb + CS_ReqCount);
    wr32(pb + CS_ActCount, 0);
    if (!matches || !i1 || !i2 || !want)
        return paramErr;
    cat_search cs = { .pb = pb, .rec = tb_NewPtr(108, false, true), .i1 = i1, .i2 = i2, .matches = matches,
                      .name = tb_NewPtr(256, false, true), .bits = rd32(pb + CS_Bits), .want = want,
                      .skip = rd32(pb + CS_Position) };
    bool done = search_dir(&cs, root_path, 0);
    tb_DisposePtr(cs.rec);
    tb_DisposePtr(cs.name);
    wr32(pb + CS_ActCount, cs.found);
    wr32(pb + CS_Position, cs.seen); /* entries looked at: where the next call starts */
    return done ? eofErr : noErr;
}

/* ---------------------------------------------------------------------- */
/* File IDs: a file's ID is its file number (ioFlNum), which lasts as long
 * as the file keeps its path. */

enum { fidNotFound = -1300, notAFileErr = -1302 };
enum { FIDParam_ioSrcDirID = 48, FIDParam_ioFileID = 54 };

int16_t tb_PBCreateFileIDRef(gaddr pb, bool async)
{
    char path[PATH_MAX];
    int16_t err = resolve((int16_t)rd16(pb + IOParam_ioVRefNum_off), (int32_t)rd32(pb + FIDParam_ioSrcDirID),
                          rd32(pb + IOParam_ioNamePtr_off), true, path, sizeof path);
    if (err)
        return err;
    struct stat st;
    if (stat(path, &st))
        return fnfErr;
    if (S_ISDIR(st.st_mode))
        return notAFileErr;
    wr32(pb + FIDParam_ioFileID, (uint32_t)dir_id_for(rel_of(path)) + 0x10000);
    return noErr;
}

/* The host path of a file ID, if the file is still there. */
static bool file_of_id(int32_t id, char *path, size_t size)
{
    struct stat st;
    return id >= 0x10000 && host_of_dir(id - 0x10000, path, size) && !stat(path, &st) && !S_ISDIR(st.st_mode) &&
           visible(path);
}

int16_t tb_PBResolveFileIDRef(gaddr pb, bool async)
{
    char path[PATH_MAX];
    if (!file_of_id((int32_t)rd32(pb + FIDParam_ioFileID), path, sizeof path))
        return fidNotFound;
    gaddr namep = rd32(pb + IOParam_ioNamePtr_off);
    if (namep)
        put_name(namep, path);
    wr32(pb + FIDParam_ioSrcDirID, (uint32_t)parent_id_of(path));
    return noErr;
}

int16_t tb_PBDeleteFileIDRef(gaddr pb, bool async)
{
    char path[PATH_MAX];
    return file_of_id((int32_t)rd32(pb + FIDParam_ioFileID), path, sizeof path) ? noErr : fidNotFound;
}

int16_t tb_PBDTRemoveAPPL(gaddr dtp, bool async) { return noErr; }

/* ---------------------------------------------------------------------- */
/* Desktop database icons: for each volume and creator, the icons of the
 * file types it owns, by kind (ioIconType). */

enum { kLargeIcon = 1, kLarge4BitIcon, kLarge8BitIcon, kSmallIcon, kSmall4BitIcon, kSmall8BitIcon };

static const struct {
    const char *type;
    uint32_t size; /* the icon with its mask, for ICN# and ics# */
} dt_kinds[] = {
    [kLargeIcon] = { "ICN#", 256 }, [kLarge4BitIcon] = { "icl4", 512 }, [kLarge8BitIcon] = { "icl8", 1024 },
    [kSmallIcon] = { "ics#", 64 },  [kSmall4BitIcon] = { "ics4", 128 }, [kSmall8BitIcon] = { "ics8", 256 },
};

typedef struct {
    int16_t vref;
    uint32_t creator, type, tag;
    uint8_t kind;
    uint8_t *data; /* dt_kinds[kind].size bytes */
} dt_icon;
static dt_icon *dt_icons;
static int ndt_icons;

/* The volumes and creators whose applications' bundles have been read. */
static struct {
    int16_t vref;
    uint32_t creator;
} *dt_read;

static dt_icon *dt_find_icon(int16_t vref, uint32_t creator, uint32_t type, int kind)
{
    for (int i = 0; i < ndt_icons; i++)
        if (dt_icons[i].vref == vref && dt_icons[i].creator == creator && dt_icons[i].type == type &&
            dt_icons[i].kind == kind)
            return &dt_icons[i];
    return NULL;
}

/* An icon kept, replacing one of its kind for the type when `replace` is
 * set (PBDTAddIcon) and otherwise leaving it (an earlier application's,
 * or one a program added). */
static void dt_put_icon(int16_t vref, uint32_t creator, uint32_t type, int kind, uint32_t tag, const uint8_t *d,
                        bool replace)
{
    dt_icon *e = dt_find_icon(vref, creator, type, kind);
    if (e && !replace)
        return;
    if (!e) {
        dt_icons = realloc(dt_icons, (size_t)(ndt_icons + 1) * sizeof *dt_icons);
        e = &dt_icons[ndt_icons++];
        *e = (dt_icon){ .vref = vref, .creator = creator, .type = type, .kind = (uint8_t)kind,
                        .data = malloc(dt_kinds[kind].size) };
    }
    e->tag = tag;
    memcpy(e->data, d, dt_kinds[kind].size);
}

static uint32_t be32_at(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static int16_t be16_at(const uint8_t *p) { return (int16_t)(p[0] << 8 | p[1]); }

/* A resource's data, decompressed if it is compressed; malloc'd. */
static uint8_t *fork_resource(const uint8_t *fork, const rsrc_index *idx, uint32_t type, int16_t id, uint32_t *len)
{
    for (size_t i = 0; i < idx->count; i++) {
        const rsrc_entry *e = &idx->entries[i];
        if (e->type != type || e->id != id)
            continue;
        const uint8_t *d = rsrc_data(fork, idx, e);
        if (!d)
            return NULL;
        uint8_t *out;
        size_t n;
        char err[64];
        if (rsrc_is_compressed(d, e->data_len)) {
            if (!rsrc_decompress(d, e->data_len, &out, &n, err, sizeof err))
                return NULL;
            *len = (uint32_t)n;
            return out;
        }
        out = malloc(e->data_len ? e->data_len : 1);
        memcpy(out, d, e->data_len);
        *len = e->data_len;
        return out;
    }
    return NULL;
}

/* An application's bundle, as the Finder copied it into the desktop
 * database: its BNDL (the one its creator owns, else the first) maps the
 * local IDs its FREFs give each file type to the ID of the type's icons. */
static void dt_read_bundle(const char *path, int16_t vref, uint32_t creator)
{
    mac_file mf;
    if (!appledouble_read_meta(path, &mf))
        return;
    rsrc_index idx;
    char err[64];
    if (!mf.rsrc_len || !rsrc_parse(mf.rsrc, mf.rsrc_len, &idx, err, sizeof err)) {
        mac_file_free(&mf);
        return;
    }
    uint8_t *bndl = NULL;
    uint32_t blen = 0;
    for (size_t i = 0; i < idx.count; i++) {
        if (idx.entries[i].type != fourcc("BNDL"))
            continue;
        uint32_t n;
        uint8_t *d = fork_resource(mf.rsrc, &idx, fourcc("BNDL"), idx.entries[i].id, &n);
        if (!d || n < 8 || (bndl && be32_at(bndl) == creator)) {
            free(d);
            continue;
        }
        free(bndl);
        bndl = d;
        blen = n;
    }
    if (bndl) {
        /* Owner, owner ID, type count - 1; then each type, its count - 1
         * and (local ID, resource ID) pairs. */
        int ntypes = be16_at(bndl + 6) + 1;
        uint32_t icn_off = 0, fref_off = 0;
        int icn_n = 0, fref_n = 0;
        uint32_t off = 8;
        for (int t = 0; t < ntypes && off + 6 <= blen; t++) {
            uint32_t type = be32_at(bndl + off);
            int count = be16_at(bndl + off + 4) + 1;
            off += 6;
            if (type == fourcc("ICN#")) {
                icn_off = off;
                icn_n = count;
            } else if (type == fourcc("FREF")) {
                fref_off = off;
                fref_n = count;
            }
            off += 4 * (uint32_t)count;
        }
        for (int f = 0; f < fref_n && fref_off + 4 * (uint32_t)f + 4 <= blen; f++) {
            uint32_t n;
            uint8_t *fref = fork_resource(mf.rsrc, &idx, fourcc("FREF"), be16_at(bndl + fref_off + 4 * f + 2), &n);
            if (!fref || n < 6) {
                free(fref);
                continue;
            }
            uint32_t ftype = be32_at(fref);
            int16_t local = be16_at(fref + 4);
            free(fref);
            for (int k = 0; k < icn_n && icn_off + 4 * (uint32_t)k + 4 <= blen; k++) {
                if (be16_at(bndl + icn_off + 4 * k) != local)
                    continue;
                int16_t id = be16_at(bndl + icn_off + 4 * k + 2);
                for (int kind = kLargeIcon; kind <= kSmall8BitIcon; kind++) {
                    uint8_t *d = fork_resource(mf.rsrc, &idx, fourcc(dt_kinds[kind].type), id, &n);
                    if (d && n >= dt_kinds[kind].size)
                        dt_put_icon(vref, creator, ftype, kind, 0, d, false);
                    free(d);
                }
                break;
            }
        }
        free(bndl);
    }
    rsrc_index_free(&idx);
    mac_file_free(&mf);
}

/* The bundles of a creator's applications on a volume, read once. */
static void dt_read_creator(int16_t vref, uint32_t creator)
{
    for (int i = 0; i < ndt_read; i++)
        if (dt_read[i].vref == vref && dt_read[i].creator == creator)
            return;
    dt_read = realloc(dt_read, (size_t)(ndt_read + 1) * sizeof *dt_read);
    dt_read[ndt_read].vref = vref;
    dt_read[ndt_read].creator = creator;
    ndt_read++;
    find_appls();
    for (int i = 0; i < nappls; i++)
        if (appls[i].creator == creator && vref_of_path(appls[i].path) == vref)
            dt_read_bundle(appls[i].path, vref, creator);
}

/* The icon of a kind for a creator and type: as much of it as the buffer
 * holds, and its tag. */
int16_t tb_PBDTGetIcon(gaddr dtp, bool async)
{
    int16_t vref = dt_vref(dtp);
    uint32_t creator = rd32(dtp + DTPBRec_ioFileCreator_off);
    int kind = rd8(dtp + DTPBRec_ioIconType_off);
    wr32(dtp + DTPBRec_ioDTActCount_off, 0);
    if (kind < kLargeIcon || kind > kSmall8BitIcon)
        return afpItemNotFound;
    dt_read_creator(vref, creator);
    dt_icon *e = dt_find_icon(vref, creator, rd32(dtp + DTPBRec_ioFileType_off), kind);
    if (!e)
        return afpItemNotFound;
    int32_t want = (int32_t)rd32(dtp + DTPBRec_ioDTReqCount_off);
    uint32_t n = want < 0 ? 0 : (uint32_t)want < dt_kinds[kind].size ? (uint32_t)want : dt_kinds[kind].size;
    gaddr buf = rd32(dtp + DTPBRec_ioDTBuffer_off);
    if (buf && n)
        gput(buf, e->data, n);
    wr32(dtp + DTPBRec_ioDTActCount_off, buf ? n : 0);
    wr32(dtp + DTPBRec_ioTagInfo_off, e->tag);
    return noErr;
}

/* The index-th (1-based) icon a creator has: its type, kind, size and tag. */
int16_t tb_PBDTGetIconInfo(gaddr dtp, bool async)
{
    int16_t vref = dt_vref(dtp);
    uint32_t creator = rd32(dtp + DTPBRec_ioFileCreator_off);
    int want = (int16_t)rd16(dtp + DTPBRec_ioIndex_off);
    dt_read_creator(vref, creator);
    for (int i = 0, k = 0; i < ndt_icons && want >= 1; i++) {
        dt_icon *e = &dt_icons[i];
        if (e->vref != vref || e->creator != creator || ++k != want)
            continue;
        wr32(dtp + DTPBRec_ioFileType_off, e->type);
        wr8(dtp + DTPBRec_ioIconType_off, e->kind);
        wr32(dtp + DTPBRec_ioDTActCount_off, dt_kinds[e->kind].size);
        wr32(dtp + DTPBRec_ioTagInfo_off, e->tag);
        return noErr;
    }
    return afpItemNotFound;
}

/* An icon for a creator and type, in place of the one of its kind there
 * was; its size must be its kind's. */
int16_t tb_PBDTAddIcon(gaddr dtp, bool async)
{
    int16_t vref = dt_vref(dtp);
    uint32_t creator = rd32(dtp + DTPBRec_ioFileCreator_off);
    int kind = rd8(dtp + DTPBRec_ioIconType_off);
    gaddr buf = rd32(dtp + DTPBRec_ioDTBuffer_off);
    if (kind < kLargeIcon || kind > kSmall8BitIcon || rd32(dtp + DTPBRec_ioDTReqCount_off) != dt_kinds[kind].size)
        return afpIconTypeError;
    if (!buf)
        return paramErr;
    dt_read_creator(vref, creator); /* so its bundle's icon doesn't come back over this one */
    uint8_t d[1024];
    gget(d, buf, dt_kinds[kind].size);
    dt_put_icon(vref, creator, rd32(dtp + DTPBRec_ioFileType_off), kind, rd32(dtp + DTPBRec_ioTagInfo_off), d, true);
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* Desktop database comments: at most 200 characters, as the Finder's. */

enum { kMaxCommentLength = 200 };

static struct {
    char *path;
    uint8_t len;
    uint8_t text[kMaxCommentLength];
} *folder_comments;
static int nfolder_comments;

static int folder_comment(const char *path)
{
    for (int i = 0; i < nfolder_comments; i++)
        if (!strcmp(folder_comments[i].path, path))
            return i;
    return -1;
}

/* The file or folder a comment call names: ioDirID and ioNamePtr on the
 * database's volume. */
static int16_t dt_item(gaddr dtp, char *path, size_t size)
{
    int16_t err = resolve(dt_vref(dtp), (int32_t)rd32(dtp + DTPBRec_ioDirID_off), rd32(dtp + DTPBRec_ioNamePtr_off),
                          true, path, size);
    if (!err && !exists(path))
        err = fnfErr;
    return err;
}

/* A file's comment into its sidecar (none to remove it); the sidecar goes
 * when nothing else needs it. */
static int16_t set_file_comment(const char *path, const uint8_t *text, uint8_t len)
{
    if (!writable(path))
        return wPrErr;
    as_ensure_file(path);
    mac_file mf;
    char e[256];
    if (!appledouble_read(path, &mf, e, sizeof e))
        return ioErrF;
    if (mf.comment_len == len && !memcmp(mf.comment, text, len)) {
        mac_file_free(&mf);
        return noErr;
    }
    memcpy(mf.comment, text, len);
    mf.comment_len = len;
    if (sidecar_redundant(path, &mf)) {
        mac_file_free(&mf);
        char *side = appledouble_sidecar_path(path);
        if (side)
            unlink(side);
        free(side);
        return noErr;
    }
    bool ok = appledouble_write(path, &mf, e, sizeof e);
    mac_file_free(&mf);
    if (ok)
        as_file_changed(path);
    return ok ? noErr : ioErrF;
}

static int16_t set_comment(gaddr dtp, const uint8_t *text, uint8_t len)
{
    char path[PATH_MAX];
    int16_t err = dt_item(dtp, path, sizeof path);
    if (err)
        return err;
    if (!is_dir(path))
        return set_file_comment(path, text, len);
    int i = folder_comment(path);
    if (i < 0) {
        if (!len)
            return noErr;
        folder_comments = realloc(folder_comments, (size_t)(nfolder_comments + 1) * sizeof *folder_comments);
        i = nfolder_comments++;
        folder_comments[i].path = strdup(path);
    }
    folder_comments[i].len = len;
    memcpy(folder_comments[i].text, text, len);
    return noErr;
}

int16_t tb_PBDTGetComment(gaddr dtp, bool async)
{
    wr32(dtp + DTPBRec_ioDTActCount_off, 0);
    char path[PATH_MAX];
    int16_t err = dt_item(dtp, path, sizeof path);
    if (err)
        return err;
    uint8_t text[kMaxCommentLength];
    uint8_t len = 0;
    if (is_dir(path)) {
        int i = folder_comment(path);
        if (i >= 0) {
            len = folder_comments[i].len;
            memcpy(text, folder_comments[i].text, len);
        }
    } else {
        mac_file mf;
        if (appledouble_read_meta(path, &mf)) {
            len = mf.comment_len;
            memcpy(text, mf.comment, len);
            mac_file_free(&mf);
        }
    }
    if (!len)
        return afpItemNotFound;
    int32_t want = (int32_t)rd32(dtp + DTPBRec_ioDTReqCount_off);
    uint32_t n = want < 0 ? 0 : (uint32_t)want < len ? (uint32_t)want : len;
    gaddr buf = rd32(dtp + DTPBRec_ioDTBuffer_off);
    if (!buf)
        n = 0;
    if (n)
        gput(buf, text, n);
    wr32(dtp + DTPBRec_ioDTActCount_off, n);
    return noErr;
}

/* Longer comments are cut to the Finder's 200 characters. */
int16_t tb_PBDTSetComment(gaddr dtp, bool async)
{
    int32_t want = (int32_t)rd32(dtp + DTPBRec_ioDTReqCount_off);
    gaddr buf = rd32(dtp + DTPBRec_ioDTBuffer_off);
    uint8_t len = want < 0 || !buf ? 0 : want > kMaxCommentLength ? kMaxCommentLength : (uint8_t)want;
    uint8_t text[kMaxCommentLength];
    if (len)
        gget(text, buf, len);
    return set_comment(dtp, text, len);
}

int16_t tb_PBDTRemoveComment(gaddr dtp, bool async)
{
    static const uint8_t none[1];
    return set_comment(dtp, none, 0);
}

/* What the desktop databases hold for the run, forgotten for the next. */
static void dt_reset(void)
{
    forget_appls();
    for (int i = 0; i < ndt_icons; i++)
        free(dt_icons[i].data);
    free(dt_icons);
    dt_icons = NULL;
    ndt_icons = 0;
    free(dt_read);
    dt_read = NULL;
    ndt_read = 0;
    for (int i = 0; i < nfolder_comments; i++)
        free(folder_comments[i].path);
    free(folder_comments);
    folder_comments = NULL;
    nfolder_comments = 0;
}

int16_t tb_PBDTFlush(gaddr dtp, bool async) { return noErr; }
int16_t tb_PBDTReset(gaddr dtp, bool async) { return noErr; }
int16_t tb_PBDTDelete(gaddr dtp, bool async) { return noErr; }

int16_t tb_PBDTGetInfo(gaddr dtp, bool async)
{
    wr32(dtp + DTPBRec_ioDTLgLen_off, 0);
    wr32(dtp + DTPBRec_ioDTPyLen_off, 0);
    return noErr;
}

/* The host file behind an open access path (QuickTime reads movie files
 * through their refnums). */
bool fm_refnum_host_path(int16_t refnum, char *out, size_t outsize)
{
    fcb *f = fcb_for(refnum);
    if (!f)
        return false;
    snprintf(out, outsize, "%s", f->path);
    return true;
}

/* ---------------------------------------------------------------------- */
/* The Apple Menu Items folder, for the Apple menu (AppendResMenu 'DRVR')
 * and OpenDeskAcc */

/* Its items' Mac names (MacRoman, C strings), sorted; returns the count. */
int fm_apple_menu_items(char (*names)[256], int max)
{
    char given[PATH_MAX], dir[PATH_MAX];
    snprintf(given, sizeof given, "%s/Apple Menu Items", system_folder_path());
    if (!realpath(given, dir)) /* grants are by real path */
        return 0;
    char **entries = NULL;
    int n = list_entries(dir, false, &entries), count = 0;
    for (int i = 0; i < n; i++) {
        if (count < max) {
            uint8_t mac[64];
            int len = host_to_mac(entries[i], mac);
            memcpy(names[count], mac, (size_t)len);
            names[count][len] = 0;
            count++;
        }
        free(entries[i]);
    }
    free(entries);
    return count;
}

uint32_t fm_type_of_path(const char *path);
bool process_launch(const char *app, const char *const *docs, int ndocs);

/* Open an Apple Menu Items entry by its Mac name: an application is
 * launched, in a run of its own.  False if it is not there or not an
 * application (documents, control panels and desk accessories wait for
 * the environment to run them). */
bool fm_open_apple_menu_item(const uint8_t *name, int n)
{
    char dir[PATH_MAX], path[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/Apple Menu Items", system_folder_path());
    host_component(dir, name, n, path, sizeof path);
    char full[PATH_MAX];
    snprintf(full, sizeof full, "%s/%s", dir, path);
    if (!exists(full))
        return false;
    if (fm_type_of_path(full) != 0x4150504Cu) /* APPL */
        return false;
    return process_launch(full, NULL, 0);
}

/* The host folder of the System Folder (for the Code Fragment Manager's
 * search of its Extensions). */
const char *fm_system_folder_host(void) { return system_folder_path(); }
