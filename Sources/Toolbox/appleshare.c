/*
 * AppleShare volumes, mirrored: a server's volume (over TCP or AppleTalk,
 * afp.c) appears as a volume of its own, kept in a cache folder that the
 * File Manager serves like any mounted folder.
 *
 * When the session opens, the volume's folders are made in the cache and
 * its files as placeholders of the right sizes, types and dates (so the
 * host's Open panel, which lists the cache itself, shows them all); a
 * file's forks come from the server the first time it is opened.  What the
 * program changes goes back to the server as it happens: a file's forks
 * and Finder information when a fork is written out or the information
 * set, new files and folders, deletions, renames and moves.  It is not a
 * live view: changes others make on the server after the session opened
 * don't show.
 *
 * Mount specifications (machine_mount_appleshare):
 *     tcp:[user[:password]@]host[:port]/Volume
 *     atalk:[user[:password]@]Server:AFPServer[@zone]/Volume
 */
#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "afp.h"
#include "forks.h"
#include "tb_internal.h"

#define MAX_SHARES 4
#define MAX_DIRS_PER_SHARE 4096

typedef struct {
    char path[1024]; /* host path of a mirrored folder */
    uint32_t id;     /* its AFP directory ID */
    bool filled;
} share_dir;

#define MAX_PENDING 8192
#define MAX_TREE_DIRS 2000 /* folders made at connection; deeper ones when looked into */

typedef struct {
    bool used, atalk, failed;
    char server[256], user[64], password[64];
    char volume[32];        /* MacRoman */
    char root[1024];        /* the cache folder */
    afp_session *s;
    uint16_t vol;
    share_dir *dirs;
    int ndirs;
    char **pending;         /* placeholder files whose forks are still on the server */
    int npending;
    bool busy;              /* filling: our own writes aren't changes */
} share;

static share shares[MAX_SHARES];

/* ---- names ---- */

/* A MacRoman name as a host file name ('/' becomes ':'). */
static void host_name(const char *mac, char *out, size_t n)
{
    macroman_to_utf8((const uint8_t *)mac, strlen(mac), out, n);
    for (char *p = out; *p; p++)
        if (*p == '/')
            *p = ':';
}

/* A host file name as a MacRoman name. */
static void mac_name(const char *host, char *out, size_t n)
{
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s", host);
    for (char *p = tmp; *p; p++)
        if (*p == ':')
            *p = '/';
    size_t k = utf8_to_macroman(tmp, (uint8_t *)out, n - 1);
    out[k] = 0;
}

/* ---- mounting ---- */

bool machine_mount_appleshare(const char *spec, char *err, size_t errlen)
{
    share *sh = NULL;
    for (int i = 0; i < MAX_SHARES && !sh; i++)
        if (!shares[i].used)
            sh = &shares[i];
    if (!sh) {
        snprintf(err, errlen, "too many AppleShare volumes");
        return false;
    }
    memset(sh, 0, sizeof *sh);
    const char *p;
    if (!strncmp(spec, "tcp:", 4)) {
        p = spec + 4;
    } else if (!strncmp(spec, "atalk:", 6)) {
        p = spec + 6;
        sh->atalk = true;
    } else {
        snprintf(err, errlen, "an AppleShare volume is tcp:host/Volume or atalk:Server:AFPServer@*/Volume");
        return false;
    }
    const char *slash = strrchr(p, '/'), *at = strchr(p, '@');
    /* user[:password]@ first; an NBP name has an '@zone' of its own, after
     * its type, so over AppleTalk an '@' is the credentials' only when the
     * name (with its ':type') still follows it. */
    bool creds = at && slash && at < slash;
    if (creds && sh->atalk) {
        const char *colon = strchr(at + 1, ':');
        creds = colon && colon < slash;
    }
    if (creds) {
        char cred[160];
        snprintf(cred, sizeof cred, "%.*s", (int)(at - p), p);
        char *colon = strchr(cred, ':');
        if (colon) {
            *colon = 0;
            snprintf(sh->password, sizeof sh->password, "%s", colon + 1);
        }
        snprintf(sh->user, sizeof sh->user, "%s", cred);
        p = at + 1;
    }
    if (!slash || slash < p || !slash[1]) {
        snprintf(err, errlen, "no volume name in %s", spec);
        return false;
    }
    snprintf(sh->server, sizeof sh->server, "%.*s", (int)(slash - p), p);
    if (sh->atalk && !strchr(sh->server, '@'))
        strncat(sh->server, "@*", sizeof sh->server - strlen(sh->server) - 1);
    mac_name(slash + 1, sh->volume, sizeof sh->volume);
    /* the cache folder: TMPDIR/bluebox-appleshare-PID-N/Volume */
    const char *tmp = getenv("TMPDIR");
    char base[900];
    snprintf(base, sizeof base, "%s/bluebox-appleshare-%d-%d", tmp && *tmp ? tmp : "/tmp", (int)getpid(),
             (int)(sh - shares));
    mkdir(base, 0700);
    char vh[256];
    host_name(sh->volume, vh, sizeof vh);
    snprintf(sh->root, sizeof sh->root, "%s/%s", base, vh);
    if (mkdir(sh->root, 0700) != 0 && errno != EEXIST) {
        snprintf(err, errlen, "%s: %s", sh->root, strerror(errno));
        return false;
    }
    sh->used = true;
    char real[1024];
    if (realpath(sh->root, real))
        snprintf(sh->root, sizeof sh->root, "%s", real);
    machine_mount_folder(sh->root, vh);
    return true;
}

static share *share_of(const char *path)
{
    for (int i = 0; i < MAX_SHARES; i++) {
        size_t n = strlen(shares[i].root);
        if (shares[i].used && !strncmp(path, shares[i].root, n) && (path[n] == '/' || path[n] == 0))
            return &shares[i];
    }
    return NULL;
}

static share_dir *dir_of(share *sh, const char *path)
{
    for (int i = 0; i < sh->ndirs; i++)
        if (!strcmp(sh->dirs[i].path, path))
            return &sh->dirs[i];
    return NULL;
}

static share_dir *add_dir(share *sh, const char *path, uint32_t id)
{
    share_dir *d = dir_of(sh, path);
    if (d) {
        d->id = id;
        return d;
    }
    if (sh->ndirs == MAX_DIRS_PER_SHARE)
        return NULL;
    share_dir *grown = realloc(sh->dirs, sizeof *grown * (size_t)(sh->ndirs + 1));
    if (!grown)
        return NULL;
    sh->dirs = grown;
    d = &sh->dirs[sh->ndirs++];
    memset(d, 0, sizeof *d);
    snprintf(d->path, sizeof d->path, "%s", path);
    d->id = id;
    return d;
}

static void build_tree(share *sh);
static bool locate(share *sh, const char *path, uint32_t *dir, char *name, size_t n);

/* The session, connected (and the volume opened) on first use. */
static bool connected(share *sh)
{
    if (sh->s)
        return true;
    if (sh->failed)
        return false;
    char err[256] = "";
    sh->s = sh->atalk ? afp_connect_atalk(sh->server, sh->user, sh->password, err, sizeof err)
                      : afp_connect_tcp(sh->server, sh->user, sh->password, err, sizeof err);
    if (sh->s && afp_open_volume(sh->s, sh->volume, &sh->vol) != 0) {
        snprintf(err, sizeof err, "no volume \"%s\" on %s", sh->volume, sh->server);
        afp_close(sh->s);
        sh->s = NULL;
    }
    if (!sh->s) {
        LOGF(LOG_UNIMPL, "AppleShare: %s\n", err);
        sh->failed = true; /* once per run: the volume stays empty */
        return false;
    }
    add_dir(sh, sh->root, 2);
    build_tree(sh);
    return true;
}

static void fill(share *sh, share_dir *d);

/* The whole tree at connection, breadth first (folders, and placeholders
 * for files), up to MAX_TREE_DIRS folders. */
static void build_tree(share *sh)
{
    for (int i = 0; i < sh->ndirs && i < MAX_TREE_DIRS; i++)
        if (!sh->dirs[i].filled)
            fill(sh, &sh->dirs[i]);
}

/* ---- filling ---- */

static void add_pending(share *sh, const char *path)
{
    if (sh->npending == MAX_PENDING)
        return;
    char **grown = realloc(sh->pending, sizeof *grown * (size_t)(sh->npending + 1));
    if (!grown)
        return;
    sh->pending = grown;
    sh->pending[sh->npending++] = strdup(path);
}

static int pending_index(share *sh, const char *path)
{
    for (int i = 0; i < sh->npending; i++)
        if (sh->pending[i] && !strcmp(sh->pending[i], path))
            return i;
    return -1;
}

static void drop_pending(share *sh, int i)
{
    free(sh->pending[i]);
    sh->pending[i] = NULL;
}

/* A folder's entries: subfolders made, files as placeholders (the data fork
 * its length of zeros, sparse; the resource fork its length; the Finder
 * information and dates as on the server). */
static void fill(share *sh, share_dir *d)
{
    afp_entry *e = NULL;
    int n = 0;
    d->filled = true;
    char dirpath[1024];
    snprintf(dirpath, sizeof dirpath, "%s", d->path);
    uint32_t id = d->id;
    if (afp_list(sh->s, sh->vol, id, &e, &n) != 0)
        return;
    sh->busy = true;
    for (int i = 0; i < n; i++) {
        char hn[256], path[1100];
        host_name(e[i].name, hn, sizeof hn);
        snprintf(path, sizeof path, "%s/%s", dirpath, hn);
        if (e[i].dir) {
            mkdir(path, 0700);
            add_dir(sh, path, e[i].id);
            continue;
        }
        mac_file mf;
        memset(&mf, 0, sizeof mf);
        uint8_t *rsrc = calloc(e[i].rsrc_len ? e[i].rsrc_len : 1, 1), empty = 0;
        mf.data = &empty;
        mf.rsrc = rsrc;
        mf.rsrc_len = e[i].rsrc_len;
        memcpy(mf.finder_info, e[i].finder_info, 32);
        mf.has_finder_info = true;
        mf.create_date = e[i].create_date;
        mf.modify_date = e[i].modify_date;
        char err[128];
        if (appledouble_write(path, &mf, err, sizeof err)) {
            if (e[i].data_len)
                truncate(path, (off_t)e[i].data_len);
            add_pending(sh, path);
        }
        free(rsrc);
    }
    sh->busy = false;
    free(e);
}

/* Before a file's forks are read: from the server, if they are still there. */
void as_ensure_file(const char *path)
{
    share *sh = share_of(path);
    if (!sh || sh->busy || !sh->s)
        return;
    int i = pending_index(sh, path);
    uint32_t dir;
    char name[32];
    if (i < 0 || !locate(sh, path, &dir, name, sizeof name))
        return;
    drop_pending(sh, i);
    mac_file mf;
    char err[128];
    if (!appledouble_read(path, &mf, err, sizeof err))
        return;
    uint8_t *data = NULL, *rsrc = NULL;
    size_t dl = 0, rl = 0;
    bool ok = afp_read_fork(sh->s, sh->vol, dir, name, false, &data, &dl) == 0 &&
              afp_read_fork(sh->s, sh->vol, dir, name, true, &rsrc, &rl) == 0;
    if (ok) {
        uint8_t empty = 0;
        free(mf.data);
        free(mf.rsrc);
        mf.data = data ? data : &empty;
        mf.data_len = dl;
        mf.rsrc = rsrc ? rsrc : &empty;
        mf.rsrc_len = rl;
        sh->busy = true;
        appledouble_write(path, &mf, err, sizeof err);
        sh->busy = false;
        if (data)
            free(data);
        if (rsrc)
            free(rsrc);
        mf.data = mf.rsrc = NULL;
    } else {
        free(data);
        free(rsrc);
        LOGF(LOG_UNIMPL, "AppleShare: %s: not read from the server\n", path);
    }
    mac_file_free(&mf);
}

void as_ensure_dir(const char *host_dir)
{
    share *sh = share_of(host_dir);
    if (!sh || sh->busy || !connected(sh))
        return;
    share_dir *d = dir_of(sh, host_dir);
    if (d && !d->filled)
        fill(sh, d);
}

/* ---- changes ---- */

/* The AFP directory and MacRoman name of a host path in a share. */
static bool locate(share *sh, const char *path, uint32_t *dir, char *name, size_t n)
{
    char parent[1024];
    snprintf(parent, sizeof parent, "%s", path);
    char *slash = strrchr(parent, '/');
    if (!slash)
        return false;
    *slash = 0;
    share_dir *d = dir_of(sh, parent);
    if (!d)
        return false;
    *dir = d->id;
    mac_name(slash + 1, name, n);
    return true;
}

static share *changed_share(const char *path)
{
    share *sh = share_of(path);
    return sh && !sh->busy && connected(sh) ? sh : NULL;
}

void as_file_changed(const char *path)
{
    share *sh = changed_share(path);
    uint32_t dir;
    char name[32];
    if (!sh || !locate(sh, path, &dir, name, sizeof name))
        return;
    mac_file mf;
    char err[128];
    if (!appledouble_read(path, &mf, err, sizeof err))
        return;
    if (pending_index(sh, path) >= 0) { /* forks still on the server: only its information changed */
        if (mf.has_finder_info)
            afp_set_finder_info(sh->s, sh->vol, dir, name, mf.finder_info);
        mac_file_free(&mf);
        return;
    }
    afp_entry e;
    if (afp_stat(sh->s, sh->vol, dir, name, &e) != 0)
        afp_create_file(sh->s, sh->vol, dir, name);
    afp_write_fork(sh->s, sh->vol, dir, name, false, mf.data, mf.data_len);
    afp_write_fork(sh->s, sh->vol, dir, name, true, mf.rsrc, mf.rsrc_len);
    if (mf.has_finder_info)
        afp_set_finder_info(sh->s, sh->vol, dir, name, mf.finder_info);
    mac_file_free(&mf);
}

void as_dir_created(const char *path)
{
    share *sh = changed_share(path);
    uint32_t dir, id = 0;
    char name[32];
    if (!sh || !locate(sh, path, &dir, name, sizeof name))
        return;
    if (afp_create_dir(sh->s, sh->vol, dir, name, &id) == 0) {
        share_dir *d = add_dir(sh, path, id);
        if (d)
            d->filled = true; /* new and empty */
    }
}

void as_deleted(const char *path)
{
    share *sh = changed_share(path);
    uint32_t dir;
    char name[32];
    if (sh && locate(sh, path, &dir, name, sizeof name)) {
        afp_delete(sh->s, sh->vol, dir, name);
        int i = pending_index(sh, path);
        if (i >= 0)
            drop_pending(sh, i);
    }
}

void as_moved(const char *from, const char *to)
{
    share *sh = changed_share(from);
    uint32_t fdir, tdir;
    char fname[32], tname[32];
    if (!sh || share_of(to) != sh || !locate(sh, from, &fdir, fname, sizeof fname) ||
        !locate(sh, to, &tdir, tname, sizeof tname))
        return;
    if (afp_move(sh->s, sh->vol, fdir, fname, tdir, tname) != 0)
        return;
    /* folders and placeholders under the old path are under the new one now */
    size_t n = strlen(from);
    for (int i = 0; i < sh->npending; i++)
        if (sh->pending[i] && !strncmp(sh->pending[i], from, n) && (sh->pending[i][n] == '/' || sh->pending[i][n] == 0)) {
            char moved[1100];
            snprintf(moved, sizeof moved, "%s%s", to, sh->pending[i] + n);
            free(sh->pending[i]);
            sh->pending[i] = strdup(moved);
        }
    for (int i = 0; i < sh->ndirs; i++)
        if (!strncmp(sh->dirs[i].path, from, n) && (sh->dirs[i].path[n] == '/' || sh->dirs[i].path[n] == 0)) {
            char moved[1024];
            snprintf(moved, sizeof moved, "%s%s", to, sh->dirs[i].path + n);
            snprintf(sh->dirs[i].path, sizeof sh->dirs[i].path, "%s", moved);
        }
}

static int remove_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st;
    (void)flag;
    (void)ftw;
    return remove(path);
}

/* The sessions closed, the cache folders removed and the volumes forgotten
 * (at exit, or between tests). */
void machine_close_appleshares(void)
{
    for (int i = 0; i < MAX_SHARES; i++) {
        if (shares[i].s)
            afp_close(shares[i].s);
        if (shares[i].used && shares[i].root[0]) {
            char base[1024];
            snprintf(base, sizeof base, "%s", shares[i].root);
            char *slash = strrchr(base, '/'); /* the bluebox-appleshare-PID-N folder above the volume */
            if (slash)
                *slash = 0;
            nftw(base, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
        }
        free(shares[i].dirs);
        for (int k = 0; k < shares[i].npending; k++)
            free(shares[i].pending[k]);
        free(shares[i].pending);
        memset(&shares[i], 0, sizeof shares[i]);
    }
}

/* At each event call: tickles keep idle sessions open. */
void as_idle(void)
{
    for (int i = 0; i < MAX_SHARES; i++) {
        if (shares[i].used && !shares[i].s && !shares[i].failed)
            connected(&shares[i]); /* at the first event call: the volume there before the Open panel lists it */
        if (shares[i].used && shares[i].s)
            afp_idle(shares[i].s);
    }
}
