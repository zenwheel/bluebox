/*
 * Alias Manager.
 *
 * Records use the System 7 layout (version 2): a fixed part naming the
 * volume, parent directory and target, then tagged variable data ending with
 * tag -1.  We resolve by the absolute path (tag 2), since directory IDs are
 * assigned per session.
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "tb_internal.h"

enum { fnfErrA = -43, paramErrA = -50, nsvErrA = -35 };
/* tagHostPath is ours: the UTF-8 host path, exact even where Mac names
 * had to be shortened. */
enum { tagParentName = 0, tagAbsolutePath = 2, tagHostPath = 0x434C, tagEnd = -1 };
enum { asiVolumeName = -1, asiAliasName = 0, asiParentName = 1 };

#define FIXED_SIZE 150

static void put_pstr(uint8_t *dst, size_t max, const uint8_t *s, size_t n)
{
    if (n > max)
        n = max;
    dst[0] = (uint8_t)n;
    memcpy(dst + 1, s, n);
}

static void be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void be32(uint8_t *p, uint32_t v) { be16(p, (uint16_t)(v >> 16)); be16(p + 2, (uint16_t)v); }

static size_t add_tag(uint8_t *rec, size_t n, int16_t tag, const uint8_t *data, size_t len)
{
    be16(rec + n, (uint16_t)tag);
    be16(rec + n + 2, (uint16_t)len);
    memcpy(rec + n + 4, data, len);
    n += 4 + len;
    if (len & 1)
        rec[n++] = 0;
    return n;
}

/* Build an alias record for a Mac full path (and host path, if known) into
 * rec; returns its size. */
static size_t build_record(const uint8_t *path, size_t plen, const char *host_path, bool folder, uint8_t *rec,
                           uint32_t user_type)
{
    memset(rec, 0, FIXED_SIZE);
    /* Split "Volume:dir:...:parent:name". */
    size_t vend = 0;
    while (vend < plen && path[vend] != ':')
        vend++;
    size_t end = plen;
    if (end > vend + 1 && path[end - 1] == ':')
        end--;
    size_t nstart = end;
    while (nstart > vend + 1 && path[nstart - 1] != ':')
        nstart--;
    size_t pstart = nstart > vend + 1 ? nstart - 1 : vend;
    while (pstart > vend + 1 && path[pstart - 1] != ':')
        pstart--;

    be32(rec, user_type);
    be16(rec + 6, 2);
    be16(rec + 8, folder ? 1 : 0);
    put_pstr(rec + 10, 27, path, vend);
    be16(rec + 42, 0x4244); /* 'BD': HFS */
    char host[PATH_MAX];
    if (fm_host_of_mac_path(path, plen, host, sizeof host)) {
        gaddr spec = tb_NewPtr(FSSpec_SIZEOF, false, true);
        fm_spec_for_path(host, spec);
        be32(rec + 46, rd32(spec + FSSpec_parID_off));
        tb_DisposePtr(spec);
    }
    put_pstr(rec + 50, 63, path + nstart, end - nstart);
    be16(rec + 130, 0xFFFF);
    be16(rec + 132, 0xFFFF);

    size_t n = FIXED_SIZE;
    if (nstart > vend + 1)
        n = add_tag(rec, n, tagParentName, path + pstart, nstart - 1 - pstart);
    n = add_tag(rec, n, tagAbsolutePath, path, plen);
    if (host_path)
        n = add_tag(rec, n, tagHostPath, (const uint8_t *)host_path, strlen(host_path));
    be16(rec + n, (uint16_t)tagEnd);
    be16(rec + n + 2, 0);
    n += 4;
    be16(rec + 4, (uint16_t)n);
    return n;
}

/* The bytes and size an alias record needs for this path, at most. */
#define MAX_RECORD (FIXED_SIZE + 4 + 64 + 4 + 1024 + 4 + PATH_MAX + 4)

static int16_t store(gaddr alias_out_or_handle, bool existing, const uint8_t *rec, size_t n)
{
    gaddr h = existing ? alias_out_or_handle : tb_NewHandle((int32_t)n, false, false);
    if (!h)
        return (int16_t)rd16(LM_MemErr);
    if (existing) {
        tb_SetHandleSize(h, (int32_t)n);
        if (rd16(LM_MemErr))
            return (int16_t)rd16(LM_MemErr);
    } else {
        wr32(alias_out_or_handle, h);
    }
    gput(rd32(h), rec, (uint32_t)n);
    return noErr;
}

static int16_t alias_for_host(const char *given, gaddr alias, bool existing)
{
    char host[PATH_MAX];
    if (!realpath(given, host))
        snprintf(host, sizeof host, "%s", given);
    uint8_t path[1024], rec[MAX_RECORD];
    size_t plen = fm_mac_path_of_host(host, path, sizeof path);
    if (!plen)
        return paramErrA;
    struct stat st;
    bool folder = stat(host, &st) == 0 && S_ISDIR(st.st_mode);
    uint32_t user_type = existing && rd32(alias) ? rd32(rd32(alias)) : 0;
    return store(alias, existing, rec, build_record(path, plen, host, folder, rec, user_type));
}

int16_t alias_new_for_host(const char *host, gaddr alias_out)
{
    wr32(alias_out, 0);
    return alias_for_host(host, alias_out, false);
}

static int16_t make_alias(gaddr target, gaddr alias, bool existing)
{
    if (!target || !alias)
        return paramErrA;
    char host[PATH_MAX];
    if (!fm_host_path((int16_t)rd16(target + FSSpec_vRefNum_off), (int32_t)rd32(target + FSSpec_parID_off),
                      target + FSSpec_name_off, true, host, sizeof host))
        return nsvErrA;
    return alias_for_host(host, alias, existing);
}

int16_t tb_NewAlias(gaddr fromFile, gaddr target, gaddr alias)
{
    if (alias)
        wr32(alias, 0);
    return make_alias(target, alias, false);
}

int16_t tb_NewAliasMinimal(gaddr target, gaddr alias) { return tb_NewAlias(0, target, alias); }

int16_t tb_NewAliasMinimalFromFullPath(int16_t fullpathLength, gaddr fullpath, gaddr zoneName, gaddr serverName,
                                       gaddr alias)
{
    if (!alias || fullpathLength <= 0 || fullpathLength > 1023)
        return paramErrA;
    wr32(alias, 0);
    uint8_t path[1024], rec[MAX_RECORD];
    gget(path, fullpath, (uint32_t)fullpathLength);
    bool folder = path[fullpathLength - 1] == ':';
    return store(alias, false, rec, build_record(path, (size_t)fullpathLength, NULL, folder, rec, 0));
}

int16_t tb_UpdateAlias(gaddr fromFile, gaddr target, gaddr alias, gaddr wasChanged)
{
    if (wasChanged)
        wr8(wasChanged, 1);
    return make_alias(target, alias, true);
}

/* Find a tag in the record; returns its data address (0 if absent). */
static gaddr find_tag(gaddr rec, int16_t tag, uint16_t *len)
{
    uint32_t size = rd16(rec + 4);
    uint32_t off = FIXED_SIZE;
    while (off + 4 <= size) {
        int16_t t = (int16_t)rd16(rec + off);
        uint16_t l = rd16(rec + off + 2);
        if (t == tagEnd)
            break;
        if (t == tag) {
            *len = l;
            return rec + off + 4;
        }
        off += 4 + ((l + 1u) & ~1u);
    }
    return 0;
}

/* Blue Box was called Classic, and its data folder moved with the name: a
 * path into the old folder is tried again in the new one. */
static bool renamed_path(const char *in, char *out, size_t outsize)
{
    const char *home = getenv("HOME");
    if (!home)
        return false;
    char old_dir[PATH_MAX], new_dir[PATH_MAX];
#ifdef __APPLE__
    snprintf(old_dir, sizeof old_dir, "%s/Library/Application Support/Classic/", home);
    snprintf(new_dir, sizeof new_dir, "%s/Library/Application Support/Blue Box/", home);
#else
    const char *data = getenv("XDG_DATA_HOME");
    char base[PATH_MAX];
    snprintf(base, sizeof base, "%s", data && *data ? data : "");
    if (!*base)
        snprintf(base, sizeof base, "%s/.local/share", home);
    snprintf(old_dir, sizeof old_dir, "%s/classic/", base);
    snprintf(new_dir, sizeof new_dir, "%s/bluebox/", base);
#endif
    size_t n = strlen(old_dir);
    if (strncmp(in, old_dir, n))
        return false;
    snprintf(out, outsize, "%s%s", new_dir, in + n);
    struct stat st;
    return stat(out, &st) == 0;
}

/* The host path an alias names: its host path if that is still there
 * (exact where Mac names are ambiguous), else where its Mac path now leads
 * (the disk moved: the Classic folder became Blue Box's), *moved set. */
static bool target_host_moved(gaddr alias, char *host, size_t hostsize, bool *moved)
{
    if (moved)
        *moved = false;
    if (!alias || !rd32(alias))
        return false;
    gaddr rec = rd32(alias);
    uint16_t len;
    gaddr p = find_tag(rec, tagHostPath, &len);
    bool have_host = false;
    if (p && len < hostsize) {
        gget(host, p, len);
        host[len] = 0;
        struct stat st;
        if (stat(host, &st) == 0)
            return true;
        char renamed[PATH_MAX];
        if (renamed_path(host, renamed, sizeof renamed)) {
            snprintf(host, hostsize, "%s", renamed);
            if (moved)
                *moved = true;
            return true;
        }
        have_host = true;
    }
    p = find_tag(rec, tagAbsolutePath, &len);
    if (!p)
        return have_host; /* gone, but that is where it was */
    uint8_t path[1024];
    if (len > sizeof path)
        return have_host;
    gget(path, p, len);
    char now[PATH_MAX];
    if (!fm_host_of_mac_path(path, len, now, sizeof now))
        return have_host;
    struct stat st;
    if (have_host && stat(now, &st) != 0)
        return true; /* neither is there: keep the exact one */
    snprintf(host, hostsize, "%s", now);
    if (moved)
        *moved = have_host;
    return true;
}

static bool target_host(gaddr alias, char *host, size_t hostsize)
{
    return target_host_moved(alias, host, hostsize, NULL);
}

/* The host path of an alias record held in guest memory (not a handle). */
bool alias_host_of_record(gaddr rec, uint32_t len, char *host, size_t hostsize)
{
    gaddr h = tb_NewHandle((int32_t)len, false, false);
    if (!h)
        return false;
    gmemcpy(rd32(h), rec, len);
    bool ok = target_host(h, host, hostsize);
    tb_DisposeHandle(h);
    return ok;
}

int16_t tb_ResolveAlias(gaddr fromFile, gaddr alias, gaddr target, gaddr wasChanged)
{
    if (wasChanged)
        wr8(wasChanged, 0);
    char host[PATH_MAX];
    bool moved;
    if (!target_host_moved(alias, host, sizeof host, &moved))
        return paramErrA;
    fm_spec_for_path(host, target);
    if (moved && wasChanged)
        wr8(wasChanged, 1); /* the program may update the record (UpdateAlias) */
    struct stat st;
    return stat(host, &st) == 0 ? noErr : fnfErrA;
}

int16_t tb_MatchAlias(gaddr fromFile, int32_t rulesMask, gaddr alias, gaddr aliasCount, gaddr aliasList,
                      gaddr needsUpdate, gaddr aliasFilter, gaddr yourDataPtr)
{
    if (needsUpdate)
        wr8(needsUpdate, 0);
    if (!aliasCount || (int16_t)rd16(aliasCount) < 1) {
        if (aliasCount)
            wr16(aliasCount, 0);
        return paramErrA;
    }
    int16_t err = tb_ResolveAlias(fromFile, alias, aliasList, 0);
    wr16(aliasCount, err == noErr ? 1 : 0);
    return err;
}

/* There are no alias files on the host: every file resolves to itself. */
int16_t tb_ResolveAliasFile(gaddr theSpec, int8_t resolveAliasChains, gaddr targetIsFolder, gaddr wasAliased)
{
    if (wasAliased)
        wr8(wasAliased, 0);
    char host[PATH_MAX];
    if (!fm_host_path((int16_t)rd16(theSpec + FSSpec_vRefNum_off), (int32_t)rd32(theSpec + FSSpec_parID_off),
                      theSpec + FSSpec_name_off, true, host, sizeof host))
        return nsvErrA;
    if (!fm_host_exists(host))
        return fnfErrA;
    struct stat st;
    if (targetIsFolder)
        wr8(targetIsFolder, stat(host, &st) == 0 && S_ISDIR(st.st_mode) ? 1 : 0);
    return noErr;
}

int16_t tb_GetAliasInfo(gaddr alias, int16_t index, gaddr theString)
{
    if (!alias || !rd32(alias) || !theString)
        return paramErrA;
    gaddr rec = rd32(alias);
    wr8(theString, 0);
    switch (index) {
    case asiVolumeName:
        gmemcpy(theString, rec + 10, rd8(rec + 10) + 1u);
        return noErr;
    case asiAliasName:
        gmemcpy(theString, rec + 50, rd8(rec + 50) + 1u);
        return noErr;
    case asiParentName: {
        uint16_t len;
        gaddr p = find_tag(rec, tagParentName, &len);
        if (p) {
            wr8(theString, (uint8_t)(len > 255 ? 255 : len));
            gmemcpy(theString + 1, p, len > 255 ? 255 : len);
        }
        return noErr;
    }
    default:
        return noErr; /* zone and server names, deeper parents: empty */
    }
}
